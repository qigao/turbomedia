/**
 * dtls_session.c - DTLS session management wrapper
 *
 * Wraps OpenSSL DTLS for WebRTC DataChannel usage.
 * Handles DTLS handshake and encryption/decryption.
 */

#include "turbo_datachannel_internal.h"
#include <openssl/err.h>
#include <platform.h>
#include "tlog.h"
#include <stb_sprintf.h>

/* ============================================================================
 * DTLS Retransmission Timer
 * ============================================================================ */

static void dtls_handle_error(turbo_dc_peer_t *peer, int ret);

static void on_dtls_retransmit_timer(turbo_timer_t *timer) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)turbo_timer_get_data(timer);
    if (!peer || peer->dtls.handshake_done) {
        turbo_timer_stop(timer);
        return;
    }

    /* Handle DTLS timeout - triggers retransmission */
    DTLSv1_handle_timeout(peer->dtls.ssl);
    dtls_send_output(peer);

    /* Schedule next timeout if handshake still in progress */
    if (!peer->dtls.handshake_done) {
        struct timeval tv;
        if (DTLSv1_get_timeout(peer->dtls.ssl, &tv)) {
            uint64_t timeout_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
            if (timeout_ms == 0) timeout_ms = 1;  /* Minimum 1ms */
            turbo_timer_start(timer, on_dtls_retransmit_timer, timeout_ms, 0);
        }
    }
}

static void dtls_schedule_retransmit(turbo_dc_peer_t *peer) {
    if (!peer->dtls.retransmit_timer || peer->dtls.handshake_done) {
        return;
    }

    struct timeval tv;
    if (DTLSv1_get_timeout(peer->dtls.ssl, &tv)) {
        uint64_t timeout_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        if (timeout_ms == 0) timeout_ms = 1;
        turbo_timer_start(peer->dtls.retransmit_timer, on_dtls_retransmit_timer, timeout_ms, 0);
    }
}

static void dtls_stop_retransmit_timer(turbo_dc_peer_t *peer) {
    if (peer->dtls.retransmit_timer) {
        turbo_timer_stop(peer->dtls.retransmit_timer);
    }
}

static void dtls_drain_application_data(turbo_dc_peer_t *peer) {
    /* Once DTLS finishes, the same datagram can already contain SCTP payload.
     * Drain any pending application records immediately instead of waiting for
     * a later network event that may never arrive. */
    size_t buf_size;
    char *decrypted;
    int decrypted_len;

    if (!peer || !peer->dtls.handshake_done) {
        return;
    }

    buf_size = peer->ctx->dtls_mtu > DTLS_MTU_DEFAULT
             ? peer->ctx->dtls_mtu
             : DTLS_MTU_DEFAULT;
    decrypted = (char *)alloca(buf_size);

    while ((decrypted_len = SSL_read(peer->dtls.ssl, decrypted, (int)buf_size)) > 0) {
        usrsctp_conninput(peer, decrypted, decrypted_len, 0);
        sctp_poll_status(peer, "post-conninput");
    }

    if (decrypted_len < 0) {
        int ssl_error = SSL_get_error(peer->dtls.ssl, decrypted_len);
        if (ssl_error != SSL_ERROR_WANT_READ && ssl_error != SSL_ERROR_WANT_WRITE) {
            dtls_handle_error(peer, decrypted_len);
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

    peer->dtls.read_bio = BIO_new(BIO_s_mem());
    peer->dtls.write_bio = BIO_new(BIO_s_mem());
    if (!peer->dtls.read_bio || !peer->dtls.write_bio) {
        if (peer->dtls.read_bio) BIO_free(peer->dtls.read_bio);
        if (peer->dtls.write_bio) BIO_free(peer->dtls.write_bio);
        SSL_free(peer->dtls.ssl);
        peer->dtls.ssl = NULL;
        dc_set_peer_error(peer, TURBO_DC_ERROR_SSL_CREATE, "BIO allocation failed");
        return -1;
    }
    BIO_set_mem_eof_return(peer->dtls.read_bio, -1);
    BIO_set_mem_eof_return(peer->dtls.write_bio, -1);

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

    /* Timer is created later for ICE mode via dtls_session_init_timer() */
    peer->dtls.retransmit_timer = NULL;

    return 0;
}

int dtls_session_init_timer(turbo_dc_peer_t *peer) {
    if (!peer) return -1;

    if (peer->dtls.retransmit_timer) return 0;  /* Already initialized */

    /* Use the backend-native timer abstraction instead of reaching for libuv. */
    peer->dtls.retransmit_timer = turbo_timer_create(NULL);

    if (peer->dtls.retransmit_timer) {
        turbo_timer_set_data(peer->dtls.retransmit_timer, peer);
        return 0;
    }
    return -1;
}

void dtls_session_cleanup(turbo_dc_peer_t *peer) {
    /* Stop and close retransmit timer */
    if (peer->dtls.retransmit_timer) {
        turbo_timer_destroy(peer->dtls.retransmit_timer);
        peer->dtls.retransmit_timer = NULL;
    }
}

/* ============================================================================
 * DTLS Helpers
 * ============================================================================ */

void dtls_send_output(turbo_dc_peer_t *peer) {
    /* Buffer must be large enough to hold the largest possible DTLS record.
     * We use 2KB to be safe even if MTU is slightly larger.
     */
    char encrypted[2048];
    int encrypted_len;

    /* Read ALL available data from the BIO.
     * Note: BIO_s_mem might coalesce multiple records.
     * OpenSSL's DTLS stack handles receiving multiple records in one packet.
     */
    while ((encrypted_len = BIO_read(peer->dtls.write_bio, encrypted, sizeof(encrypted))) > 0) {
        TLOG_INFO("DTLS outbound flight bytes={}", encrypted_len);
        dc_send_transport_data(peer, encrypted, encrypted_len);
    }
}

static void dtls_handle_error(turbo_dc_peer_t *peer, int ret) {
    int ssl_error = SSL_get_error(peer->dtls.ssl, ret);

    switch (ssl_error) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            /* Non-fatal, need more data - normal for async DTLS */
            break;

        case SSL_ERROR_SYSCALL: {
            /* For UDP DTLS, SYSCALL with ret=-1 during handshake often just means
             * we need to wait for response. Only treat as error if there's an
             * actual error in the queue. */
            unsigned long err = ERR_peek_error();
            if (err == 0) {
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

                unsigned long err = ERR_get_error();
                char err_buf[256];
                if (err) {
                    ERR_error_string_n(err, err_buf, sizeof(err_buf));
                    TLOG_DEBUG("DTLS handshake error: {}", err_buf);
                } else {
                    stbsp_snprintf(err_buf, sizeof(err_buf), "SSL error %d", ssl_error);
                    TLOG_DEBUG("DTLS handshake error: {}", err_buf);
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
         /* No fingerprint provided to verify against - optional or not yet set */
         return 0;
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
        TLOG_ERROR("DTLS fingerprint mismatch: expected {}, got {}",
                   peer->remote_fingerprint, actual_fp);
        dc_set_peer_error(peer, TURBO_DC_ERROR_DTLS_FINGERPRINT, "fingerprint mismatch");
        return -1;
    }
 
     return 0;
 }
 
 void dtls_process_handshake(turbo_dc_peer_t *peer) {
    int ret = SSL_do_handshake(peer->dtls.ssl);
    TLOG_INFO("DTLS handshake step ret={} state={}", ret, peer ? (int)peer->state : -1);

    if (ret == 1) {
        TLOG_INFO("%s", "DTLS handshake completed");
        /* Verify fingerprint before proceeding */
        if (verify_remote_fingerprint(peer) != 0) {
            peer->state = TURBO_DC_STATE_FAILED;
            if (peer->on_state) {
                peer->on_state(peer, TURBO_DC_STATE_CONNECTING, TURBO_DC_STATE_FAILED, peer->user_data);
            }
            return;
        }
 
        peer->dtls.handshake_done = 1;
        dtls_stop_retransmit_timer(peer);

        if (peer->ctx->disable_sctp) {
            dc_notify_state(peer, TURBO_DC_STATE_CONNECTED);
            dtls_send_output(peer);
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
        }
    } else {
        dtls_handle_error(peer, ret);
        /* Schedule retransmit timer for handshake packets */
        dtls_schedule_retransmit(peer);
    }

    dtls_send_output(peer);
}

void dtls_handle_incoming(turbo_dc_peer_t *peer, const void *data, size_t len) {
    TLOG_INFO("DTLS incoming bytes={} handshake_done={}", len, peer ? peer->dtls.handshake_done : 0);
    BIO_write(peer->dtls.read_bio, data, (int)len);

    if (!peer->dtls.handshake_done) {
        dtls_process_handshake(peer);
    }

    dtls_drain_application_data(peer);
}
