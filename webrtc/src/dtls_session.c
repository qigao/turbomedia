/**
 * dtls_session.c - DTLS session management wrapper
 *
 * Wraps BoringSSL DTLS for WebRTC DataChannel usage.
 * Handles DTLS handshake and encryption/decryption.
 */

#include "turbo_datachannel_internal.h"
#include <openssl/err.h>
#include <platform.h>
#include "tlog.h"
#include <stb_sprintf.h>
#include <limits.h>
#include <cstl/deque.h>

/* BoringSSL's DTLS BIO contract is packet-oriented. A stream BIO loses both
 * MTU and record boundaries. All access here is under the peer's SSL admission;
 * raw deque entries transfer owned tstr pointers explicitly. */
typedef struct {
    deque_t packets;
    size_t bytes;
} dtls_bio_t;

static tstr dtls_bio_take(BIO *bio) {
    dtls_bio_t *queue = BIO_get_data(bio);
    tstr packet = NULL;
    if (deque_pop_front(&queue->packets, &packet) == STL_OK)
        queue->bytes -= tstr_len(packet);
    return packet;
}

static int dtls_bio_create(BIO *bio) {
    dtls_bio_t *queue = calloc(1, sizeof(*queue));
    if (!queue) return 0;
    if (deque_init_bytes(&queue->packets, sizeof(tstr), _Alignof(tstr),
                         DTLS_BIO_MAX_PACKETS) != STL_OK) {
        free(queue);
        return 0;
    }
    BIO_set_data(bio, queue);
    BIO_set_init(bio, 1);
    return 1;
}

static void dtls_bio_clear(BIO *bio) {
    tstr packet;
    while ((packet = dtls_bio_take(bio)) != NULL) tstr_free(packet);
}

static int dtls_bio_destroy(BIO *bio) {
    dtls_bio_t *queue = BIO_get_data(bio);
    if (queue) {
        dtls_bio_clear(bio);
        deque_destroy(&queue->packets);
        free(queue);
    }
    return 1;
}

static int dtls_bio_write(BIO *bio, const char *data, int len) {
    dtls_bio_t *queue = BIO_get_data(bio);
    BIO_clear_retry_flags(bio);
    if (len <= 0) return 0;
    if ((size_t)len > DTLS_BIO_MAX_PACKET_BYTES ||
        (size_t)len > DTLS_BIO_MAX_BYTES - queue->bytes ||
        deque_size(&queue->packets) == DTLS_BIO_MAX_PACKETS) {
        OPENSSL_PUT_ERROR(BIO, ERR_R_OVERFLOW);
        return -1;
    }
    tstr packet = tstr_new_len(data, (size_t)len);
    if (!packet || deque_push_back(&queue->packets, &packet) != STL_OK) {
        tstr_free(packet);
        OPENSSL_PUT_ERROR(BIO, ERR_R_MALLOC_FAILURE);
        return -1;
    }
    queue->bytes += (size_t)len;
    return len;
}

static int dtls_bio_read(BIO *bio, char *data, int len) {
    BIO_clear_retry_flags(bio);
    if (len <= 0) return 0;
    tstr packet = dtls_bio_take(bio);
    if (!packet) {
        BIO_set_retry_read(bio);
        return -1;
    }
    size_t copied = tstr_len(packet);
    if (copied > (size_t)len) copied = (size_t)len;
    memcpy(data, packet, copied);
    /* Like a datagram socket, a short read consumes the entire packet. */
    tstr_free(packet);
    return (int)copied;
}

static long dtls_bio_ctrl(BIO *bio, int command, long arg, void *ptr) {
    dtls_bio_t *queue = BIO_get_data(bio);
    (void)arg; (void)ptr;
    switch (command) {
        case BIO_CTRL_FLUSH: return 1;
        case BIO_CTRL_EOF: return deque_empty(&queue->packets);
        case BIO_CTRL_PENDING: {
            tstr *packet = deque_front(&queue->packets);
            return packet ? (long)tstr_len(*packet) : 0;
        }
        case BIO_CTRL_RESET:
            dtls_bio_clear(bio);
            BIO_clear_retry_flags(bio);
            return 1;
        default: return 0;
    }
}

static const BIO_METHOD dtls_bio_method = {
    .type = BIO_TYPE_SOURCE_SINK,
    .name = "TurboMedia DTLS datagrams",
    .bwrite = dtls_bio_write,
    .bread = dtls_bio_read,
    .ctrl = dtls_bio_ctrl,
    .create = dtls_bio_create,
    .destroy = dtls_bio_destroy
};

/* ============================================================================
 * DTLS Retransmission Timer
 * ============================================================================ */

static void dtls_handle_error(turbo_dc_peer_t *peer, int ssl_error,
                              unsigned long error);
/* Called under DTLS admission. The context worker owns deadline observation;
 * ordinary callers only publish whether this peer still needs it. */
static void dtls_set_timer_active(turbo_dc_peer_t *peer, int active) {
    if (peer->dtls.timer_active == active) return;
    cmeta_mutex_lock(&peer->ctx->transport_mutex);
    peer->dtls.timer_active = active;
    if (active) ++peer->ctx->active_dtls_timers;
    else --peer->ctx->active_dtls_timers;
    cmeta_cond_broadcast(&peer->ctx->transport_cond);
    cmeta_mutex_unlock(&peer->ctx->transport_mutex);
}

void dtls_session_lock(turbo_dc_peer_t *peer) {
    cmeta_mutex_lock(&peer->operation_mutex);
    while (peer->dtls.busy) {
        cmeta_cond_wait(&peer->operation_cond, &peer->operation_mutex);
    }
    peer->dtls.busy = 1;
    cmeta_mutex_unlock(&peer->operation_mutex);
}

void dtls_session_unlock(turbo_dc_peer_t *peer) {
    cmeta_mutex_lock(&peer->operation_mutex);
    peer->dtls.busy = 0;
    cmeta_cond_broadcast(&peer->operation_cond);
    cmeta_mutex_unlock(&peer->operation_mutex);
}

void dtls_session_poll_timeout(turbo_dc_peer_t *peer) {
    int result = 0;
    dtls_session_lock(peer);
    if (peer->dtls.timer_active && !peer->dtls.stopped && !peer->dtls.handshake_done) {
        result = DTLSv1_handle_timeout(peer->dtls.ssl);
        if (result < 0) {
            peer->dtls.stopped = 1;
            dtls_set_timer_active(peer, 0);
        }
    }
    dtls_session_unlock(peer);
    if (result > 0) dtls_send_output(peer);
    else if (result < 0) {
        dc_fail_peer(peer, TURBO_DC_ERROR_DTLS_HANDSHAKE,
                     "DTLS retransmission failed");
    }
}

static void dtls_drain_application_data(turbo_dc_peer_t *peer) {
    /* Once DTLS finishes, the same datagram can already contain SCTP payload.
     * Drain any pending application records immediately instead of waiting for
     * a later network event that may never arrive. */
    size_t buf_size;
    char *decrypted;
    int decrypted_len;

    buf_size = peer->ctx->dtls_mtu > DTLS_MTU_DEFAULT
             ? peer->ctx->dtls_mtu
             : DTLS_MTU_DEFAULT;
    decrypted = (char *)alloca(buf_size);

    for (;;) {
        int ssl_error = SSL_ERROR_NONE;
        unsigned long error = 0;
        dtls_session_lock(peer);
        if (!peer->dtls.application_ready || peer->dtls.stopped) {
            dtls_session_unlock(peer);
            return;
        }
        ERR_clear_error();
        decrypted_len = SSL_read(peer->dtls.ssl, decrypted, (int)buf_size);
        if (decrypted_len < 0) {
            ssl_error = SSL_get_error(peer->dtls.ssl, decrypted_len);
            error = ERR_peek_error();
        }
        dtls_session_unlock(peer);
        if (decrypted_len < 0) dtls_handle_error(peer, ssl_error, error);
        /* A repeated Finished can regenerate our last handshake flight even
         * when SSL_read returns WANT_READ and there is no application data.
         * Send outside SSL admission; terminal errors suppress output. */
        dtls_send_output(peer);
        if (decrypted_len <= 0) return;
        if (!peer->ctx->disable_sctp) {
            usrsctp_conninput(peer, decrypted, decrypted_len, 0);
            sctp_poll_status(peer, "post-conninput");
        }
    }
}

/* ============================================================================
 * DTLS Session API
 * ============================================================================ */

static int dtls_verify_callback(int preverify_ok, X509_STORE_CTX *ctx) {
    /* Always return 1 to allow the handshake to proceed with self-signed certs.
     * We verify the fingerprint manually in verify_remote_fingerprint().
     * (preverify_ok is usually 0 for self-signed certs because CA is not known)
     */
    (void)preverify_ok;
    (void)ctx;
    return 1;
}

int dtls_session_init(turbo_dc_peer_t *peer) {
    peer->dtls.ssl = SSL_new(peer->ctx->ssl_ctx);
    if (!peer->dtls.ssl) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_SSL_CREATE, NULL);
        return -1;
    }

    peer->dtls.read_bio = BIO_new(&dtls_bio_method);
    peer->dtls.write_bio = BIO_new(&dtls_bio_method);
    if (!peer->dtls.read_bio || !peer->dtls.write_bio) {
        if (peer->dtls.read_bio) BIO_free(peer->dtls.read_bio);
        if (peer->dtls.write_bio) BIO_free(peer->dtls.write_bio);
        SSL_free(peer->dtls.ssl);
        peer->dtls.ssl = NULL;
        dc_set_peer_error(peer, TURBO_DC_ERROR_SSL_CREATE, "BIO allocation failed");
        return -1;
    }
    SSL_set_bio(peer->dtls.ssl, peer->dtls.read_bio, peer->dtls.write_bio);

    if (peer->is_dtls_server) {
        SSL_set_accept_state(peer->dtls.ssl);
    } else {
        SSL_set_connect_state(peer->dtls.ssl);
    }
 
    /* Request remote certificate for fingerprint verification */
    /* Verify callback must return 1 to allow self-signed certs */
    SSL_set_verify(peer->dtls.ssl, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, dtls_verify_callback);

    /* Enforce MTU to prevent BoringSSL from exceeding the transport buffer. */
    long mtu = peer->ctx->dtls_mtu ? peer->ctx->dtls_mtu : DTLS_MTU_DEFAULT;
    SSL_set_mtu(peer->dtls.ssl, mtu);

    return 0;
}

void dtls_session_shutdown(turbo_dc_peer_t *peer) {
    dtls_session_lock(peer);
    if (!peer->dtls.stopped && peer->dtls.ssl) SSL_shutdown(peer->dtls.ssl);
    peer->dtls.stopped = 1;
    dtls_set_timer_active(peer, 0);
    dtls_session_unlock(peer);
}

int dtls_session_is_ready(turbo_dc_peer_t *peer) {
    int ready;
    dtls_session_lock(peer);
    ready = peer->dtls.handshake_done && !peer->dtls.stopped;
    dtls_session_unlock(peer);
    return ready;
}

int dtls_write_application_data(turbo_dc_peer_t *peer, const void *data, size_t len) {
    int written = -1;
    if (len > INT_MAX) return -1;
    dtls_session_lock(peer);
    if (peer->dtls.handshake_done && !peer->dtls.stopped) {
        ERR_clear_error();
        written = SSL_write(peer->dtls.ssl, data, (int)len);
    }
    dtls_session_unlock(peer);
    if (written > 0) dtls_send_output(peer);
    return written;
}

/* ============================================================================
 * DTLS Helpers
 * ============================================================================ */

void dtls_send_output(turbo_dc_peer_t *peer) {
    for (;;) {
        dtls_session_lock(peer);
        tstr packet = peer->dtls.stopped ? NULL : dtls_bio_take(peer->dtls.write_bio);
        dtls_session_unlock(peer);
        if (!packet) break;
        dc_send_transport_data(peer, packet, tstr_len(packet));
        tstr_free(packet);
    }
}

static void dtls_handle_error(turbo_dc_peer_t *peer, int ssl_error,
                              unsigned long error) {
    if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE &&
        !(ssl_error == SSL_ERROR_SYSCALL && error == 0)) {
        dtls_session_lock(peer);
        peer->dtls.stopped = 1;
        dtls_set_timer_active(peer, 0);
        dtls_session_unlock(peer);
    }
    switch (ssl_error) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            /* Non-fatal, need more data - normal for async DTLS */
            break;

        case SSL_ERROR_SYSCALL: {
            /* For UDP DTLS, SYSCALL with ret=-1 during handshake often just means
             * we need to wait for response. Only treat as error if there's an
             * actual error in the queue. */
            if (error == 0) {
                /* No error - just need to wait */
                break;
            }
            /* Fall through to error handling */
        }
        /* fallthrough */

        case SSL_ERROR_ZERO_RETURN: {
            turbo_dc_state_t old_state = peer->state;
            if (old_state != TURBO_DC_STATE_CLOSED) {
                peer->state = TURBO_DC_STATE_CLOSED;
                if (peer->on_state) {
                    peer->on_state(peer, old_state, TURBO_DC_STATE_CLOSED, peer->user_data);
                }
            }
            break;
        }

        default: {
            turbo_dc_state_t old_state = peer->state;
            if (old_state != TURBO_DC_STATE_FAILED) {
                peer->state = TURBO_DC_STATE_FAILED;

                char err_buf[256];
                if (error) {
                    ERR_error_string_n(error, err_buf, sizeof(err_buf));
                    TLOG_DEBUGF("DTLS handshake error: {}", err_buf);
                } else {
                    stbsp_snprintf(err_buf, sizeof(err_buf), "SSL error %d", ssl_error);
                    TLOG_DEBUGF("DTLS handshake error: {}", err_buf);
                }

                if (peer->on_error) {
                    peer->on_error(peer, TURBO_DC_ERROR_DTLS_HANDSHAKE, err_buf, peer->user_data);
                }

                if (peer->on_state) {
                    peer->on_state(peer, old_state, TURBO_DC_STATE_FAILED, peer->user_data);
                }
            }
            break;
        }
    }
}
 
 static int verify_remote_fingerprint(turbo_dc_peer_t *peer) {
     if (!peer->remote_fingerprint || !peer->remote_fingerprint[0]) {
         dc_set_peer_error(peer, TURBO_DC_ERROR_DTLS_FINGERPRINT,
                           "remote fingerprint is required");
         return -1;
     }
 
     X509 *cert = SSL_get_peer_certificate(peer->dtls.ssl);
     if (!cert) {
         dc_set_peer_error(peer, TURBO_DC_ERROR_DTLS_FINGERPRINT, "no peer certificate provided");
         return -1;
     }
 
     unsigned char md[EVP_MAX_MD_SIZE];
     unsigned int md_len;
     if (X509_digest(cert, EVP_sha256(), md, &md_len) != 1) {
         X509_free(cert);
         dc_set_peer_error(peer, TURBO_DC_ERROR_DTLS_FINGERPRINT, "failed to calculate fingerprint");
         return -1;
     }
     X509_free(cert);
 
    char actual_fp[128];
    static const char hex[] = "0123456789ABCDEF";
    size_t fp_pos = 0;

    if (md_len == 0 || ((size_t)md_len * 3) > sizeof(actual_fp)) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_DTLS_FINGERPRINT, "fingerprint buffer too small");
        return -1;
    }

    for (unsigned int i = 0; i < md_len; ++i) {
        actual_fp[fp_pos++] = hex[(md[i] >> 4) & 0x0F];
        actual_fp[fp_pos++] = hex[md[i] & 0x0F];
        if (i + 1 != md_len) {
            actual_fp[fp_pos++] = ':';
        }
    }
    actual_fp[fp_pos] = '\0';
 
    if (strcmp(actual_fp, peer->remote_fingerprint) != 0) {
        TLOG_ERRORF("DTLS fingerprint mismatch: expected {}, got {}",
                   peer->remote_fingerprint, actual_fp);
        dc_set_peer_error(peer, TURBO_DC_ERROR_DTLS_FINGERPRINT, "fingerprint mismatch");
        return -1;
    }
 
     return 0;
 }
 
 void dtls_process_handshake(turbo_dc_peer_t *peer) {
    int ret;
    int ssl_error = SSL_ERROR_NONE;
    unsigned long error = 0;
    int fingerprint_failed = 0;
    turbo_dc_error_t fingerprint_error = {0};
    dtls_session_lock(peer);
    if (peer->dtls.handshake_done || peer->dtls.stopped) {
        dtls_session_unlock(peer);
        return;
    }
    ERR_clear_error();
    peer->dtls.handshake_started = 1;
    ret = SSL_do_handshake(peer->dtls.ssl);
    if (ret == 1) {
        dtls_set_timer_active(peer, 0);
        if (verify_remote_fingerprint(peer) != 0) {
            fingerprint_failed = 1;
            fingerprint_error = turbo_dc_peer_get_error(peer);
            peer->dtls.stopped = 1;
        } else {
            /* Elect exactly one caller to start SCTP/notify completion. */
            peer->dtls.handshake_done = 1;
        }
    } else {
        ssl_error = SSL_get_error(peer->dtls.ssl, ret);
        error = ERR_peek_error();
        dtls_set_timer_active(peer, 1);
    }
    dtls_session_unlock(peer);
    TLOG_DEBUGF("DTLS handshake step ret={} state={}", ret, peer ? (int)peer->state : -1);

    if (ret == 1) {
        /* Verify fingerprint before proceeding */
        if (fingerprint_failed) {
            dc_fail_peer(peer, TURBO_DC_ERROR_DTLS_FINGERPRINT, fingerprint_error.detail);
            return;
        }
        TLOG_INFO("DTLS handshake completed");
 
        if (peer->ctx->disable_sctp) {
            dtls_session_lock(peer);
            peer->dtls.application_ready = 1;
            dtls_session_unlock(peer);
            dc_notify_state(peer, TURBO_DC_STATE_CONNECTED);
            dtls_send_output(peer);
            dtls_drain_application_data(peer);
            return;
        }

        /* Start SCTP association after DTLS is ready.
         * The peer is only CONNECTED after SCTP reports COMM_UP. */
        if (sctp_start_association(peer) != 0) {
            turbo_dc_state_t old_state = peer->state;
            peer->state = TURBO_DC_STATE_FAILED;
            if (peer->on_state) {
                peer->on_state(peer, old_state, TURBO_DC_STATE_FAILED, peer->user_data);
            }
        } else {
            dtls_session_lock(peer);
            peer->dtls.application_ready = 1;
            dtls_session_unlock(peer);
            dtls_drain_application_data(peer);
        }
    } else {
        dtls_handle_error(peer, ssl_error, error);
    }

    dtls_send_output(peer);
}

void dtls_handle_incoming(turbo_dc_peer_t *peer, const void *data, size_t len) {
    if (len > INT_MAX) return;
    dtls_session_lock(peer);
    if (peer->dtls.stopped) {
        dtls_session_unlock(peer);
        return;
    }
    int accepted = BIO_write(peer->dtls.read_bio, data, (int)len);
    if (accepted != (int)len) {
        peer->dtls.stopped = 1;
        dtls_set_timer_active(peer, 0);
    }
    dtls_session_unlock(peer);
    if (accepted != (int)len) {
        dc_fail_peer(peer, TURBO_DC_ERROR_DTLS_HANDSHAKE,
                     "DTLS input datagram admission failed");
        return;
    }
    dtls_process_handshake(peer);
    dtls_drain_application_data(peer);
}
