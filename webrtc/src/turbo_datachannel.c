/**
 * turbo_datachannel.c - WebRTC Data Channel API implementation
 *
 * This file contains the public API and coordination logic.
 * Protocol implementations are split into separate files:
 * - dcep_protocol.c - DCEP message handling
 * - dtls_session.c - DTLS wrapper
 * - sctp_session.c - SCTP wrapper
 */

#include "turbo_datachannel_internal.h"
#include <stdlib.h>
#include <string.h>
#include <turbo_str.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/srtp.h>
#include "tlog.h"
#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#endif
#include "ice/turbo_ice.h"
#include "turbo_srtp_defs.h"

/* TurboNet X.509 certificate generation */
#include <asn1/x509_generate.h>

/* SRTP profile key length helper */
size_t srtp_profile_key_len(uint16_t profile) {
    switch (profile) {
        case SRTP_PROFILE_AES128_CM_SHA1_80:
        case SRTP_PROFILE_AES128_CM_SHA1_32:
        case SRTP_PROFILE_AEAD_AES_128_GCM:
            return 16;
        case SRTP_PROFILE_AEAD_AES_256_GCM:
            return 32;
        default:
            return 16;
    }
}

/* SRTP profile salt length helper */
size_t srtp_profile_salt_len(uint16_t profile) {
    switch (profile) {
        case SRTP_PROFILE_AES128_CM_SHA1_80:
        case SRTP_PROFILE_AES128_CM_SHA1_32:
            return 14;
        case SRTP_PROFILE_AEAD_AES_128_GCM:
        case SRTP_PROFILE_AEAD_AES_256_GCM:
            return 12;
        default:
            return 14;
    }
}

/* Derive SRTP keys from DTLS handshake */
int srtp_derive_keys_from_dtls(void *ssl_ptr,
                               srtp_keying_material_t *keys,
                               uint16_t profile) {
    if (!ssl_ptr || !keys) return -1;

    SSL *ssl = (SSL *)ssl_ptr;

    size_t key_len = srtp_profile_key_len(profile);
    size_t salt_len = srtp_profile_salt_len(profile);
    size_t total_len = 2 * (key_len + salt_len);

    /* Buffer for keying material:
     * client_key || server_key || client_salt || server_salt
     */
    uint8_t keying_material[128];
    if (total_len > sizeof(keying_material)) return -1;

    /* Export keying material using RFC 5705 */
    int result = SSL_export_keying_material(
        ssl,
        keying_material, total_len,
        DTLS_SRTP_PROFILE_LABEL, strlen(DTLS_SRTP_PROFILE_LABEL),
        NULL, 0,
        0  /* No context */
    );

    if (result != 1) return -1;

    /* Parse keying material */
    uint8_t *p = keying_material;
    memcpy(keys->client_key, p, key_len);
    p += key_len;
    memcpy(keys->server_key, p, key_len);
    p += key_len;
    memcpy(keys->client_salt, p, salt_len);
    p += salt_len;
    memcpy(keys->server_salt, p, salt_len);

    keys->key_len = key_len;
    keys->salt_len = salt_len;

    /* Clear sensitive data from stack */
    memset(keying_material, 0, sizeof(keying_material));

    return 0;
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */
 
 static int calculate_fingerprint(const uint8_t *cert_der, size_t cert_len, char *fingerprint, size_t fp_size) {
     if (!cert_der || cert_len == 0 || !fingerprint || fp_size < 96) {
         return -1;
     }

     /* Use TurboNet SHA-256 fingerprint */
     return x509_cert_fingerprint_string(cert_der, cert_len, fingerprint);
 }

 /* ============================================================================
  * Self-signed Certificate Generation
  * ============================================================================ */
 
 static int generate_self_signed_cert(turbo_dc_context_t *ctx) {
     EVP_PKEY *pkey = NULL;
     X509 *x509 = NULL;
     uint8_t *cert_der = NULL;
     char *cert_pem = NULL;
     size_t cert_pem_len = 0;
     char *key_pem = NULL;
     size_t key_pem_len = 0;
     int ret = -1;

     /* Generate ECDSA P-256 certificate using TurboNet X.509 generator */
     if (x509_generate_tls_cert_pem_ecdsa("TurboNet DataChannel", 365, &cert_pem, &cert_pem_len, &key_pem, &key_pem_len) != 0) {
         goto cleanup;
     }

     /* Load Certificate from PEM */
     BIO *bio = BIO_new_mem_buf(cert_pem, (int)cert_pem_len);
     if (!bio) goto cleanup;
     x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL);
     BIO_free(bio);
     if (!x509) goto cleanup;

     /* Load Private Key from PEM */
     bio = BIO_new_mem_buf(key_pem, (int)key_pem_len);
     if (!bio) goto cleanup;
     pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
     BIO_free(bio);
     if (!pkey) goto cleanup;

     /* Get DER representation for fingerprint calculation */
     int der_len = i2d_X509(x509, NULL);
     if (der_len <= 0) goto cleanup;
     
     cert_der = (uint8_t *)malloc(der_len);
     if (!cert_der) goto cleanup;
     
     uint8_t *p = cert_der;
     if (i2d_X509(x509, &p) <= 0) goto cleanup;

     /* Calculate and store fingerprint using TurboNet SHA-256 */
     ctx->local_fingerprint_hash = tstr_dup("sha-256");
     
     char fp_buf[128];
     if (calculate_fingerprint(cert_der, der_len, fp_buf, sizeof(fp_buf)) != 0) {
         goto cleanup;
     }
     ctx->local_fingerprint = tstr_dup(fp_buf);

     /* Apply to SSL context */
     if (SSL_CTX_use_certificate(ctx->ssl_ctx, x509) != 1) goto cleanup;
     if (SSL_CTX_use_PrivateKey(ctx->ssl_ctx, pkey) != 1) goto cleanup;

     ret = 0;

 cleanup:
     if (x509) X509_free(x509);
     if (pkey) EVP_PKEY_free(pkey);
     if (cert_der) free(cert_der);
     if (cert_pem) free(cert_pem);
     if (key_pem) free(key_pem);

     return ret;
 }

/* ============================================================================
 * Transport Helpers
 * ============================================================================ */

typedef struct {
    turbo_mutex_t mutex;
    turbo_cond_t cond;
    int done;
    coro_post_fn fn;
    void *arg1;
    void *arg2;
} dc_sync_post_t;

typedef struct {
    turbo_dc_peer_t *peer;
    size_t len;
#if defined(_MSC_VER)
    char data[1];
#else
    char data[];
#endif
} dc_send_task_t;

static int dc_is_ipv6_host(const char *host) {
    return host && strchr(host, ':') != NULL;
}

static int dc_is_on_transport_thread(const turbo_dc_context_t *ctx) {
    return ctx && ctx->transport_ctx && coro_context_current() == ctx->transport_ctx;
}

void dc_notify_state(turbo_dc_peer_t *peer, turbo_dc_state_t new_state) {
    turbo_dc_state_t old_state;

    if (!peer) return;
    old_state = peer->state;
    if (old_state == new_state) return;

    peer->state = new_state;
    if (peer->on_state) {
        peer->on_state(peer, old_state, new_state, peer->user_data);
    }
}

void dc_fail_peer(turbo_dc_peer_t *peer, turbo_dc_error_code_t code, const char *detail) {
    const char *message;
    turbo_dc_state_t old_state;

    if (!peer) return;

    dc_set_peer_error(peer, code, detail);
    message = detail ? detail : turbo_dc_error_string(code);

    if (peer->state != TURBO_DC_STATE_FAILED) {
        old_state = peer->state;
        peer->state = TURBO_DC_STATE_FAILED;
        if (peer->on_error) {
            peer->on_error(peer, code, message, peer->user_data);
        }
        if (peer->on_state) {
            peer->on_state(peer, old_state, TURBO_DC_STATE_FAILED, peer->user_data);
        }
    }
}

static void dc_notify_closed(turbo_dc_peer_t *peer) {
    if (!peer || peer->state == TURBO_DC_STATE_CLOSED) return;
    dc_notify_state(peer, TURBO_DC_STATE_CLOSED);
}

static void dc_transport_thread_main(void *arg) {
    turbo_dc_context_t *ctx = (turbo_dc_context_t *)arg;
    if (!ctx || !ctx->transport_ctx) return;
    coro_context_run(ctx->transport_ctx, TURBO_RUN_DEFAULT);
}

static void dc_transport_stop_post(void *arg1, void *arg2) {
    coro_context_t *transport_ctx = (coro_context_t *)arg1;
    (void)arg2;

    coro_context_set_persistent(transport_ctx, 0);
    coro_context_stop(transport_ctx);
}

static void dc_sync_post_runner(void *arg1, void *arg2) {
    dc_sync_post_t *sync = (dc_sync_post_t *)arg1;
    (void)arg2;

    sync->fn(sync->arg1, sync->arg2);

    turbo_mutex_lock(&sync->mutex);
    sync->done = 1;
    turbo_cond_signal(&sync->cond);
    turbo_mutex_unlock(&sync->mutex);
}

static int dc_post_sync(turbo_dc_context_t *ctx, coro_post_fn fn, void *arg1, void *arg2) {
    dc_sync_post_t sync;
    int post_rc;

    if (!ctx || !ctx->transport_ctx || dc_is_on_transport_thread(ctx)) {
        fn(arg1, arg2);
        return 0;
    }

    memset(&sync, 0, sizeof(sync));
    turbo_mutex_init(&sync.mutex);
    turbo_cond_init(&sync.cond);
    sync.fn = fn;
    sync.arg1 = arg1;
    sync.arg2 = arg2;

    turbo_mutex_lock(&sync.mutex);
    post_rc = coro_post(ctx->transport_ctx, dc_sync_post_runner, &sync, NULL);
    if (post_rc == 0) {
        while (!sync.done) {
            turbo_cond_wait(&sync.cond, &sync.mutex);
        }
    }
    turbo_mutex_unlock(&sync.mutex);

    turbo_cond_destroy(&sync.cond);
    turbo_mutex_destroy(&sync.mutex);
    return post_rc;
}

static int dc_post_async(turbo_dc_context_t *ctx, coro_post_fn fn, void *arg1, void *arg2) {
    if (!ctx || !ctx->transport_ctx || dc_is_on_transport_thread(ctx)) {
        fn(arg1, arg2);
        return 0;
    }
    return coro_post(ctx->transport_ctx, fn, arg1, arg2);
}

static void dc_copy_sockaddr(struct sockaddr_storage *dst, const struct sockaddr *src) {
    size_t len = sizeof(*dst);

    if (!dst || !src) return;

    switch (src->sa_family) {
        case AF_INET:
            len = sizeof(struct sockaddr_in);
            break;
        case AF_INET6:
            len = sizeof(struct sockaddr_in6);
            break;
        default:
            break;
    }

    memset(dst, 0, sizeof(*dst));
    memcpy(dst, src, len);
}

static int dc_resolve_bind_addr(const char *host,
                                uint16_t port,
                                int socktype,
                                struct sockaddr_storage *out_addr,
                                socklen_t *out_len) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char port_buf[16];
    const char *bind_host = (host && host[0]) ? host : "0.0.0.0";
    int rc;

    if (!out_addr || !out_len) return -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_flags = AI_PASSIVE;

    snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)port);
    rc = getaddrinfo(bind_host, port_buf, &hints, &result);
    if (rc != 0 || !result) {
        if (result) freeaddrinfo(result);
        return -1;
    }

    memset(out_addr, 0, sizeof(*out_addr));
    memcpy(out_addr, result->ai_addr, result->ai_addrlen);
    *out_len = (socklen_t)result->ai_addrlen;
    freeaddrinfo(result);
    return 0;
}

static turbo_stream_kind_t dc_stream_kind_for_host(const char *host) {
    return dc_is_ipv6_host(host) ? TURBO_STREAM_TCP6 : TURBO_STREAM_TCP4;
}

static turbo_datagram_kind_t dc_datagram_kind_for_host(const char *host) {
    return dc_is_ipv6_host(host) ? TURBO_DATAGRAM_UDP6 : TURBO_DATAGRAM_UDP4;
}

static int dc_is_dtls_record(const uint8_t *data, size_t len) {
    uint8_t first_byte;

    if (!data || len == 0) {
        return 0;
    }

    first_byte = data[0];
    return first_byte >= 20 && first_byte <= 63;
}

static int dc_is_rtp_or_rtcp_packet(const uint8_t *data, size_t len) {
    uint8_t first_byte;

    if (!data || len < 2) {
        return 0;
    }

    first_byte = data[0];
    return first_byte >= 128 && first_byte <= 191;
}

static void dc_handle_incoming_packet(turbo_dc_peer_t *peer, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;

    if (!peer || !bytes || len == 0) {
        return;
    }

    if (dc_is_dtls_record(bytes, len)) {
        dtls_handle_incoming(peer, bytes, len);
        return;
    }

    if (dc_is_rtp_or_rtcp_packet(bytes, len)) {
        if (peer->on_transport_data) {
            peer->on_transport_data(peer->transport_data_user_data, bytes, len);
        }
    }
}

static int dc_stream_recv_cb(void *handle, const mem_slice_t *data, void *peer_ctx) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)turbo_stream_get_user_data((turbo_stream_t *)handle);
    (void)peer_ctx;
    if (!peer || !data || !data->data || data->length == 0) return 0;
    dc_handle_incoming_packet(peer, data->data, data->length);
    return 0;
}

static void dc_stream_close_cb(void *handle) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)turbo_stream_get_user_data((turbo_stream_t *)handle);
    if (!peer) return;

    if (peer->server_stream == (turbo_stream_t *)handle) {
        peer->server_stream = NULL;
    }
    dc_notify_closed(peer);
}

static void dc_stream_connect_cb(void *handle, int status, void *peer_ctx) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)turbo_stream_get_user_data((turbo_stream_t *)handle);
    (void)peer_ctx;

    if (!peer) return;
    if (status != 0) {
        dc_fail_peer(peer, TURBO_DC_ERROR_CONNECT_FAILED, turbo_strerror(status));
        return;
    }

    if (dtls_session_init_timer(peer) != 0) {
        dc_fail_peer(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to create DTLS timer");
        return;
    }
    if (turbo_stream_recv_start((turbo_stream_t *)handle, dc_stream_recv_cb) != 0) {
        dc_fail_peer(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to start stream receive");
        return;
    }

    dc_notify_state(peer, TURBO_DC_STATE_CONNECTING);
    dtls_process_handshake(peer);
}

static void dc_stream_accept_cb(void *server_handle, void *client_handle, void *peer_ctx) {
    turbo_dc_peer_t *peer = NULL;
    turbo_stream_t *client = (turbo_stream_t *)client_handle;
    (void)peer_ctx;

    if (server_handle) {
        peer = (turbo_dc_peer_t *)turbo_stream_listener_get_user_data((turbo_stream_listener_t *)server_handle);
    }
    if (!peer || !client) return;

    if (peer->server_stream) {
        turbo_stream_close(client);
        turbo_stream_destroy(client);
        return;
    }

    peer->server_stream = client;
    turbo_stream_set_user_data(client, peer);

    if (dtls_session_init_timer(peer) != 0) {
        dc_fail_peer(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to create DTLS timer");
        return;
    }
    if (turbo_stream_recv_start(client, dc_stream_recv_cb) != 0) {
        dc_fail_peer(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to start stream receive");
        return;
    }

    dc_notify_state(peer, TURBO_DC_STATE_CONNECTING);
}

static int dc_datagram_recv_cb(void *handle, const mem_slice_t *data, void *peer_ctx) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)turbo_datagram_get_user_data((turbo_datagram_t *)handle);
    const struct sockaddr *src = (const struct sockaddr *)peer_ctx;

    if (!peer || !data || !data->data || data->length == 0) return 0;

    if (src) {
        dc_copy_sockaddr(&peer->remote_addr, src);
        peer->has_remote_addr = 1;
    }

    dc_handle_incoming_packet(peer, data->data, data->length);
    return 0;
}

static void dc_async_send_task(void *arg1, void *arg2) {
    dc_send_task_t *task = (dc_send_task_t *)arg1;
    turbo_dc_peer_t *peer;
    (void)arg2;

    if (!task || !task->peer) {
        free(task);
        return;
    }

    peer = task->peer;

    switch (peer->ctx->transport) {
        case TURBO_DC_TRANSPORT_TCP:
            if (peer->ctx->is_server) {
                if (peer->server_stream) {
                    turbo_stream_send(peer->server_stream, task->data, task->len);
                    turbo_stream_flush(peer->server_stream);
                }
            } else if (peer->transport) {
                turbo_stream_send((turbo_stream_t *)peer->transport, task->data, task->len);
                turbo_stream_flush((turbo_stream_t *)peer->transport);
            }
            break;

        case TURBO_DC_TRANSPORT_UDP:
        default:
            if (!peer->transport) break;
            if (peer->ctx->is_server) {
                if (peer->has_remote_addr) {
                    turbo_datagram_sendto((turbo_datagram_t *)peer->transport,
                                          (const struct sockaddr *)&peer->remote_addr,
                                          task->data, task->len);
                }
            } else {
                turbo_datagram_send((turbo_datagram_t *)peer->transport, task->data, task->len);
            }
            break;
    }

    free(task);
}

static void direct_send(turbo_dc_peer_t *peer, const void *data, size_t len) {
    dc_send_task_t *task;

    if (!peer || !data || len == 0) return;

    task = (dc_send_task_t *)malloc(sizeof(*task) + len);
    if (!task) return;

    task->peer = peer;
    task->len = len;
    memcpy(task->data, data, len);

    if (dc_post_async(peer->ctx, dc_async_send_task, task, NULL) != 0) {
        free(task);
    }
}

static void dc_transport_close_task(void *arg1, void *arg2) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)arg1;
    int *status = (int *)arg2;

    if (status) *status = 0;
    if (!peer) return;

    switch (peer->ctx->transport) {
        case TURBO_DC_TRANSPORT_TCP:
            if (peer->server_stream) {
                turbo_stream_close(peer->server_stream);
            }
            if (!peer->ctx->is_server && peer->transport) {
                turbo_stream_close((turbo_stream_t *)peer->transport);
            }
            if (peer->listener) {
                turbo_stream_listener_close(peer->listener);
            }
            break;

        case TURBO_DC_TRANSPORT_UDP:
        default:
            if (peer->transport) {
                turbo_datagram_close((turbo_datagram_t *)peer->transport);
            }
            break;
    }
}

static void dc_transport_destroy_task(void *arg1, void *arg2) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)arg1;
    int *status = (int *)arg2;

    if (status) *status = 0;
    if (!peer) return;

    switch (peer->ctx->transport) {
        case TURBO_DC_TRANSPORT_TCP:
            if (peer->server_stream) {
                turbo_stream_destroy(peer->server_stream);
                peer->server_stream = NULL;
            }
            if (!peer->ctx->is_server && peer->transport) {
                turbo_stream_destroy((turbo_stream_t *)peer->transport);
            }
            peer->transport = NULL;
            if (peer->listener) {
                turbo_stream_listener_close(peer->listener);
                peer->listener = NULL;
            }
            break;

        case TURBO_DC_TRANSPORT_UDP:
        default:
            if (peer->transport) {
                turbo_datagram_destroy((turbo_datagram_t *)peer->transport);
                peer->transport = NULL;
            }
            break;
    }
}

static void direct_close(turbo_dc_peer_t *peer) {
    int status = 0;
    if (!peer) return;
    dc_post_sync(peer->ctx, dc_transport_close_task, peer, &status);
}

static void direct_destroy(turbo_dc_peer_t *peer) {
    int status = 0;
    if (!peer) return;
    dc_post_sync(peer->ctx, dc_transport_destroy_task, peer, &status);
}

static const dc_transport_ops_t g_direct_ops = {
    .send = direct_send,
    .close = direct_close,
    .destroy = direct_destroy
};

static void dc_start_transport_task(void *arg1, void *arg2) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)arg1;
    int *status = (int *)arg2;

    if (status) *status = -1;
    if (!peer || !peer->ctx || !peer->ctx->transport_ctx) return;

    switch (peer->ctx->transport) {
        case TURBO_DC_TRANSPORT_TCP:
            if (peer->ctx->is_server) {
                struct sockaddr_storage bind_addr;
                socklen_t bind_len = 0;
                turbo_stream_kind_t kind;

                if (dc_resolve_bind_addr(peer->remote_host, peer->remote_port, SOCK_STREAM,
                                         &bind_addr, &bind_len) != 0) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_LISTEN_FAILED, "failed to resolve bind address");
                    return;
                }

                kind = (bind_addr.ss_family == AF_INET6) ? TURBO_STREAM_TCP6 : TURBO_STREAM_TCP4;
                peer->listener = turbo_stream_listen(peer->ctx->transport_ctx, kind,
                                                     (const struct sockaddr *)&bind_addr, 128,
                                                     dc_stream_accept_cb);
                if (!peer->listener) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_LISTEN_FAILED, turbo_strerror(coro_context_get_last_error(peer->ctx->transport_ctx)));
                    return;
                }

                turbo_stream_listener_set_user_data(peer->listener, peer);
                peer->transport_ops = &g_direct_ops;
                dc_notify_state(peer, TURBO_DC_STATE_CONNECTING);
                if (status) *status = 0;
            } else {
                turbo_stream_t *stream;

                stream = turbo_stream_create(peer->ctx->transport_ctx, dc_stream_kind_for_host(peer->remote_host));
                if (!stream) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, turbo_strerror(coro_context_get_last_error(peer->ctx->transport_ctx)));
                    return;
                }

                turbo_stream_set_user_data(stream, peer);
                if (turbo_stream_connect(stream, peer->remote_host, peer->remote_port,
                                         dc_stream_connect_cb, dc_stream_close_cb) != 0) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CONNECT_FAILED, turbo_strerror(coro_context_get_last_error(peer->ctx->transport_ctx)));
                    turbo_stream_destroy(stream);
                    return;
                }

                peer->transport = stream;
                peer->transport_ops = &g_direct_ops;
                if (status) *status = 0;
            }
            break;

        case TURBO_DC_TRANSPORT_KCP:
            dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT,
                              "KCP transport is not available in the installed TurboNet::CoroNet package");
            return;

        case TURBO_DC_TRANSPORT_UDP:
        default: {
            turbo_datagram_t *dg = turbo_datagram_create(peer->ctx->transport_ctx,
                                                         dc_datagram_kind_for_host(peer->remote_host));
            if (!dg) {
                dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, turbo_strerror(coro_context_get_last_error(peer->ctx->transport_ctx)));
                return;
            }

            turbo_datagram_set_user_data(dg, peer);
            if (peer->ctx->is_server) {
                if (turbo_datagram_bind(dg,
                                        (peer->remote_host && peer->remote_host[0]) ? peer->remote_host : "0.0.0.0",
                                        peer->remote_port) != 0) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_LISTEN_FAILED, turbo_strerror(coro_context_get_last_error(peer->ctx->transport_ctx)));
                    turbo_datagram_destroy(dg);
                    return;
                }

                if (turbo_datagram_recv_start(dg, dc_datagram_recv_cb) != 0) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to start datagram receive");
                    turbo_datagram_destroy(dg);
                    return;
                }

                peer->transport = dg;
                peer->transport_ops = &g_direct_ops;
                if (dtls_session_init_timer(peer) != 0) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to create DTLS timer");
                    return;
                }
                dc_notify_state(peer, TURBO_DC_STATE_CONNECTING);
                if (status) *status = 0;
            } else {
                if (turbo_datagram_connect(dg, peer->remote_host, peer->remote_port) != 0) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CONNECT_FAILED, turbo_strerror(coro_context_get_last_error(peer->ctx->transport_ctx)));
                    turbo_datagram_destroy(dg);
                    return;
                }

                if (turbo_datagram_recv_start(dg, dc_datagram_recv_cb) != 0) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to start datagram receive");
                    turbo_datagram_destroy(dg);
                    return;
                }

                peer->transport = dg;
                peer->transport_ops = &g_direct_ops;
                if (dtls_session_init_timer(peer) != 0) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to create DTLS timer");
                    return;
                }
                dc_notify_state(peer, TURBO_DC_STATE_CONNECTING);
                dtls_process_handshake(peer);
                if (status) *status = 0;
            }
            break;
        }
    }
}

/* ============================================================================
 * Transport Operations - externally owned datagram transport
 * ============================================================================ */

static void external_transport_send(turbo_dc_peer_t *peer,
                                    const void *data,
                                    size_t len) {
    if (peer->external_transport && peer->external_transport_send) {
        peer->external_transport_send(peer->external_transport, data, len);
    }
}

static void external_transport_close(turbo_dc_peer_t *peer) {
    (void)peer;
}

static void external_transport_destroy(turbo_dc_peer_t *peer) {
    peer->external_transport = NULL;
    peer->external_transport_send = NULL;
}

static const dc_transport_ops_t g_external_transport_ops = {
    .send = external_transport_send,
    .close = external_transport_close,
    .destroy = external_transport_destroy
};

static void ice_agent_transport_send(void *transport,
                                     const void *data,
                                     size_t len) {
    (void)ice_agent_send((turbo_ice_agent_t *)transport, data, len);
}

/* ============================================================================
 * Transport Helper
 * ============================================================================ */

CXX_C_API void turbo_dc_peer_send_transport_data(turbo_dc_peer_t *peer, const void *data, size_t len) {
    if (peer->transport_ops && peer->transport_ops->send) {
        peer->transport_ops->send(peer, data, len);
    }
}

/* Internal alias for backward compatibility */
void dc_send_transport_data(turbo_dc_peer_t *peer, const void *data, size_t len) {
    turbo_dc_peer_send_transport_data(peer, data, len);
}

CXX_C_API void turbo_dc_peer_set_transport_data_handler(
    turbo_dc_peer_t *peer,
    turbo_dc_transport_data_cb cb,
    void *user_data) {
    if (!peer) {
        return;
    }

    peer->on_transport_data = cb;
    peer->transport_data_user_data = user_data;
}

/* ============================================================================
 * Context API
 * ============================================================================ */

turbo_dc_context_t *turbo_dc_context_create(const turbo_dc_config_t *config) {
    if (!config) {
        return NULL;
    }

    turbo_dc_context_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->is_server = config->is_server;
    ctx->transport = config->transport;
    ctx->sctp_mtu = config->sctp_mtu ? config->sctp_mtu : SCTP_MTU_DEFAULT;
    ctx->dtls_mtu = config->dtls_mtu ? config->dtls_mtu : DTLS_MTU_DEFAULT;
    ctx->disable_sctp = config->disable_sctp ? 1 : 0;

    SSL_library_init();
    SSL_load_error_strings();

    /* WebRTC DTLS role is negotiated independently from SDP offer/answer.
     * Use the generic DTLS method here so each peer can switch between
     * connect/accept state later via SSL_set_connect_state()/accept_state(). */
    const SSL_METHOD *method = DTLS_method();
    ctx->ssl_ctx = SSL_CTX_new(method);
    if (!ctx->ssl_ctx) {
        dc_set_context_error(ctx, TURBO_DC_ERROR_SSL_CONTEXT, NULL);
        free(ctx);
        return NULL;
    }

    if (SSL_CTX_set_tlsext_use_srtp(
            ctx->ssl_ctx,
            "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32:"
            "SRTP_AEAD_AES_128_GCM:SRTP_AEAD_AES_256_GCM") != 0) {
        dc_set_context_error(ctx, TURBO_DC_ERROR_SSL_CONTEXT, "failed to enable DTLS-SRTP");
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    /* Modern cipher list for DTLS 1.2:
     * - Prefer ECDHE for forward secrecy
     * - AES-GCM for authenticated encryption
     * - Exclude weak ciphers (NULL, EXPORT, DES, RC4, MD5) */
    SSL_CTX_set_cipher_list(ctx->ssl_ctx,
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-CHACHA20-POLY1305:"
        "DHE-RSA-AES128-GCM-SHA256:"
        "DHE-RSA-AES256-GCM-SHA384");

    /* Ensure ECDH works */
    SSL_CTX_set_ecdh_auto(ctx->ssl_ctx, 1);

    /* Disable certificate verification for self-signed certs */
    SSL_CTX_set_verify(ctx->ssl_ctx, SSL_VERIFY_NONE, NULL);

    if (config->cert_pem && config->key_pem) {
        if (SSL_CTX_use_certificate_file(ctx->ssl_ctx, config->cert_pem, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, config->key_pem, SSL_FILETYPE_PEM) != 1) {
            dc_set_context_error(ctx, TURBO_DC_ERROR_CERT_LOAD, NULL);
            SSL_CTX_free(ctx->ssl_ctx);
            free(ctx);
            return NULL;
        }
    } else {
        /* Generate self-signed certificate for DTLS */
        if (generate_self_signed_cert(ctx) != 0) {
            dc_set_context_error(ctx, TURBO_DC_ERROR_CERT_LOAD, "failed to generate self-signed cert");
            SSL_CTX_free(ctx->ssl_ctx);
            free(ctx);
            return NULL;
        }
    }

    if (sctp_global_init() != 0) {
        dc_set_context_error(ctx, TURBO_DC_ERROR_SCTP_INIT, NULL);
        SSL_CTX_free(ctx->ssl_ctx);
        free(ctx);
        return NULL;
    }

    if (ctx->transport != TURBO_DC_TRANSPORT_ICE) {
        ctx->transport_ctx = coro_context_create(NULL);
        if (!ctx->transport_ctx) {
            dc_set_context_error(ctx, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to create CoroNet context");
            sctp_global_cleanup();
            SSL_CTX_free(ctx->ssl_ctx);
            free(ctx);
            return NULL;
        }

        coro_context_set_persistent(ctx->transport_ctx, 1);
        if (turbo_thread_create(&ctx->transport_thread, dc_transport_thread_main, ctx) != 0) {
            dc_set_context_error(ctx, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to start CoroNet transport thread");
            coro_context_set_persistent(ctx->transport_ctx, 0);
            coro_context_destroy(ctx->transport_ctx);
            ctx->transport_ctx = NULL;
            sctp_global_cleanup();
            SSL_CTX_free(ctx->ssl_ctx);
            free(ctx);
            return NULL;
        }
        ctx->transport_thread_started = 1;
    }

    ctx->initialized = 1;
    return ctx;
}

void turbo_dc_context_destroy(turbo_dc_context_t *ctx) {
    int stop_rc;

    if (!ctx) return;

    if (ctx->transport_ctx && ctx->transport_thread_started) {
        stop_rc = coro_post(ctx->transport_ctx,
                            dc_transport_stop_post,
                            ctx->transport_ctx,
                            NULL);
        if (stop_rc != TURBO_OK) {
            TLOG_ERROR("Failed to post DataChannel transport stop: {}", stop_rc);
            coro_context_stop(ctx->transport_ctx);
        }
    }
    if (ctx->transport_thread_started) {
        turbo_thread_join(&ctx->transport_thread);
        turbo_thread_destroy(&ctx->transport_thread);
        ctx->transport_thread_started = 0;
    }
    if (ctx->transport_ctx) {
        coro_context_set_persistent(ctx->transport_ctx, 0);
        coro_context_destroy(ctx->transport_ctx);
        ctx->transport_ctx = NULL;
    }

    if (ctx->ssl_ctx) {
        SSL_CTX_free(ctx->ssl_ctx);
    }

    sctp_global_cleanup();

    tstr_free(ctx->local_fingerprint);
    tstr_free(ctx->local_fingerprint_hash);
    free(ctx);
}

void turbo_dc_global_cleanup(void) {
    sctp_global_cleanup();
}
 
 int turbo_dc_context_get_local_fingerprint(
     turbo_dc_context_t *ctx,
     char *hash,
     size_t hash_len,
     char *fingerprint,
     size_t fp_len
 ) {
     if (!ctx || !hash || !fingerprint) return -1;
     if (!ctx->local_fingerprint || !ctx->local_fingerprint[0]) return -2;
 
     strncpy(hash, ctx->local_fingerprint_hash, hash_len);
     strncpy(fingerprint, ctx->local_fingerprint, fp_len);
     return 0;
 }

/* ============================================================================
 * Peer API
 * ============================================================================ */

turbo_dc_peer_t *turbo_dc_peer_create(
    turbo_dc_context_t *ctx,
    const char *remote_host,
    uint16_t remote_port,
    void *user_data
) {
    if (!ctx) {
        return NULL;
    }

    turbo_dc_peer_t *peer = calloc(1, sizeof(*peer));
    if (!peer) {
        dc_set_context_error(ctx, TURBO_DC_ERROR_ALLOC_PEER, NULL);
        return NULL;
    }

    peer->ctx = ctx;
    peer->user_data = user_data;
    peer->state = TURBO_DC_STATE_NEW;
    peer->is_dtls_server = ctx->is_server;

    /* Initialize channel ID bitmap */
    peer->channel_ids = roaring_bitmap_create();
    if (!peer->channel_ids) {
        dc_set_context_error(ctx, TURBO_DC_ERROR_ALLOC_PEER, "failed to create channel bitmap");
        free(peer);
        return NULL;
    }

    if (remote_host) {
        peer->remote_host = tstr_dup(remote_host);
    }
    peer->remote_port = remote_port;

    if (dtls_session_init(peer) != 0) {
        roaring_bitmap_free(peer->channel_ids);
        free(peer);
        return NULL;
    }

    if (!ctx->disable_sctp && sctp_session_init(peer) != 0) {
        dtls_session_cleanup(peer);  /* Clean up DTLS timer */
        SSL_free(peer->dtls.ssl);
        roaring_bitmap_free(peer->channel_ids);
        free(peer);
        return NULL;
    }

    return peer;
}

void turbo_dc_peer_on_state(turbo_dc_peer_t *peer, turbo_dc_state_cb cb) {
    if (peer) peer->on_state = cb;
}

void turbo_dc_peer_on_channel(turbo_dc_peer_t *peer, turbo_dc_channel_cb cb) {
    if (peer) peer->on_channel = cb;
}

void turbo_dc_peer_on_error(turbo_dc_peer_t *peer, turbo_dc_error_cb cb) {
     if (peer) peer->on_error = cb;
 }
 
static int dc_ascii_equal_ignore_case(const char *left, const char *right) {
    unsigned char a;
    unsigned char b;

    if (!left || !right) return 0;
    while (*left && *right) {
        a = (unsigned char)*left++;
        b = (unsigned char)*right++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return *left == '\0' && *right == '\0';
}

static int dc_is_sha256_fingerprint(const char *fingerprint) {
    enum {
        SHA256_FINGERPRINT_BYTES = 32,
        SHA256_FINGERPRINT_LENGTH = SHA256_FINGERPRINT_BYTES * 3 - 1
    };
    size_t i;

    if (!fingerprint || strlen(fingerprint) != SHA256_FINGERPRINT_LENGTH) {
        return 0;
    }
    for (i = 0; i < SHA256_FINGERPRINT_BYTES; ++i) {
        size_t offset = i * 3;
        unsigned char high = (unsigned char)fingerprint[offset];
        unsigned char low = (unsigned char)fingerprint[offset + 1];
        int high_is_hex = (high >= '0' && high <= '9') ||
                          (high >= 'a' && high <= 'f') ||
                          (high >= 'A' && high <= 'F');
        int low_is_hex = (low >= '0' && low <= '9') ||
                         (low >= 'a' && low <= 'f') ||
                         (low >= 'A' && low <= 'F');

        if (!high_is_hex || !low_is_hex ||
            (i + 1 < SHA256_FINGERPRINT_BYTES &&
             fingerprint[offset + 2] != ':')) {
            return 0;
        }
    }
    return 1;
}

int turbo_dc_peer_set_remote_fingerprint(
    turbo_dc_peer_t *peer,
    const char *hash,
    const char *fingerprint
) {
    tstr_t new_hash;
    tstr_t new_fingerprint;
    char *p;

    if (!peer || !hash || !fingerprint) return -1;
    if (!dc_ascii_equal_ignore_case(hash, "sha-256")) return -2;
    if (!dc_is_sha256_fingerprint(fingerprint)) return -3;

    new_hash = tstr_dup("sha-256");
    new_fingerprint = tstr_dup(fingerprint);
    if (!new_hash || !new_fingerprint) {
        tstr_free(new_hash);
        tstr_free(new_fingerprint);
        return -4;
    }

    for (p = new_fingerprint; *p; ++p) {
        if (*p >= 'a' && *p <= 'f') *p = (char)(*p - ('a' - 'A'));
    }

    tstr_free(peer->remote_fingerprint_hash);
    tstr_free(peer->remote_fingerprint);
    peer->remote_fingerprint_hash = new_hash;
    peer->remote_fingerprint = new_fingerprint;
    return 0;
}

int turbo_dc_peer_set_dtls_role(turbo_dc_peer_t *peer, int is_server) {
    if (!peer || !peer->dtls.ssl) return -1;
    if (peer->state != TURBO_DC_STATE_NEW) return -2;

    peer->is_dtls_server = is_server ? 1 : 0;
    if (peer->is_dtls_server) {
        SSL_set_accept_state(peer->dtls.ssl);
    } else {
        SSL_set_connect_state(peer->dtls.ssl);
    }

    return 0;
}

int turbo_dc_peer_is_dtls_server(const turbo_dc_peer_t *peer) {
    return peer ? peer->is_dtls_server : 0;
}

int turbo_dc_peer_set_external_transport(turbo_dc_peer_t *peer,
                                         void *transport,
                                         turbo_dc_transport_send_cb send_cb) {
    if (!peer) {
        return -1;
    }

    if (!transport) {
        peer->external_transport = NULL;
        peer->external_transport_send = NULL;
        if (peer->transport_ops == &g_external_transport_ops) {
            peer->transport_ops = NULL;
        }
        return 0;
    }

    if (!send_cb) {
        return -1;
    }
    if (peer->transport_ops && peer->transport_ops != &g_external_transport_ops) {
        return -2;
    }

    peer->external_transport = transport;
    peer->external_transport_send = send_cb;
    peer->transport_ops = &g_external_transport_ops;

    return dtls_session_init_timer(peer);
}

void turbo_dc_peer_feed_transport_data(turbo_dc_peer_t *peer,
                                       const void *data,
                                       size_t len) {
    if (!peer || !data || len == 0) return;

    dc_handle_incoming_packet(peer, data, len);
}

int turbo_dc_peer_set_ice_agent(turbo_dc_peer_t *peer, struct turbo_ice_agent_s *ice_agent) {
    if (!peer || !ice_agent || ice_agent_get_state(ice_agent) == ICE_STATE_CLOSED) {
        return -1;
    }

    return turbo_dc_peer_set_external_transport(
        peer, ice_agent, ice_agent_transport_send);
}

void turbo_dc_peer_feed_ice_data(turbo_dc_peer_t *peer, const void *data, size_t len) {
    turbo_dc_peer_feed_transport_data(peer, data, len);
}

int turbo_dc_peer_connect(turbo_dc_peer_t *peer) {
    int status = -1;

    if (!peer) {
        return -1;
    }

    if (peer->external_transport) {
        TLOG_INFO("DC peer connect via ICE");
        peer->state = TURBO_DC_STATE_CONNECTING;

        /* Start DTLS handshake - the ICE agent should already be connected */
        /* Use dtls_process_handshake to properly schedule retransmit timer */
        dtls_process_handshake(peer);

        return 0;
    }

    /* ICE transport requires ICE agent to be set first */
    if (peer->ctx->transport == TURBO_DC_TRANSPORT_ICE) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "ICE transport requires ice_agent");
        return -1;
    }

    if (!peer->ctx->transport_ctx) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "transport context is not initialized");
        return -1;
    }

    if (dc_post_sync(peer->ctx, dc_start_transport_task, peer, &status) != 0) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to schedule transport setup");
        return -1;
    }

    return status;
}

void turbo_dc_peer_poll(turbo_dc_peer_t *peer) {
    if (!peer) {
        return;
    }

    if (peer->sctp.socket) {
        sctp_poll_status(peer, "peer-poll");
    }
}

turbo_dc_state_t turbo_dc_peer_get_state(turbo_dc_peer_t *peer) {
    return peer ? peer->state : TURBO_DC_STATE_CLOSED;
}

void turbo_dc_peer_close(turbo_dc_peer_t *peer) {
    if (!peer) return;

    if (peer->sctp.socket) {
        usrsctp_shutdown(peer->sctp.socket, SHUT_RDWR);
        usrsctp_close(peer->sctp.socket);
        peer->sctp.socket = NULL;
    }

    if (peer->dtls.ssl) {
        SSL_shutdown(peer->dtls.ssl);
    }

    if (peer->transport_ops && peer->transport_ops->close) {
        peer->transport_ops->close(peer);
    }

    dc_notify_closed(peer);
}

void turbo_dc_peer_destroy(turbo_dc_peer_t *peer) {
    if (!peer) return;

    turbo_dc_peer_close(peer);

    /* Clean up DTLS timer before freeing SSL */
    dtls_session_cleanup(peer);

    if (peer->dtls.ssl) {
        SSL_free(peer->dtls.ssl);
    }

    /* Close SCTP socket before deregistering address */
    if (peer->sctp.socket) {
        usrsctp_close(peer->sctp.socket);
        peer->sctp.socket = NULL;
    }

    /* AF_CONN addresses remain process-global until explicitly deregistered. */
    if (peer->sctp_address_registered) {
        usrsctp_deregister_address(peer);
        peer->sctp_address_registered = 0;
    }

    if (peer->transport_ops && peer->transport_ops->destroy) {
        peer->transport_ops->destroy(peer);
    }

    /* Clean up channels */
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (peer->channels[i]) {
            tstr_free(peer->channels[i]->label);
            tstr_free(peer->channels[i]->protocol);
            free(peer->channels[i]);
            peer->channels[i] = NULL;
        }
    }

    /* Free channel ID bitmap */
    if (peer->channel_ids) {
        roaring_bitmap_free(peer->channel_ids);
    }
 
    tstr_free(peer->remote_host);
    tstr_free(peer->remote_fingerprint);
    tstr_free(peer->remote_fingerprint_hash);
 
    free(peer);
}
 
uint16_t turbo_dc_peer_get_srtp_keys(turbo_dc_peer_t *peer, void *material) {
    if (!peer || !peer->dtls.ssl || !material) return 0;
    if (!SSL_is_init_finished(peer->dtls.ssl)) return 0;
 
    /* Get negotiated SRTP profile */
    const SRTP_PROTECTION_PROFILE *profile = SSL_get_selected_srtp_profile(peer->dtls.ssl);
    if (!profile) return 0;
 
    /* Derive keys */
    if (srtp_derive_keys_from_dtls(peer->dtls.ssl, (srtp_keying_material_t *)material, (uint16_t)profile->id) != 0) {
        return 0;
    }
 
    return (uint16_t)profile->id;
}

/* ============================================================================
 * Channel API
 * ============================================================================ */

turbo_dc_channel_t *turbo_dc_channel_create(
    turbo_dc_peer_t *peer,
    const char *label,
    const turbo_dc_channel_config_t *config
) {
    if (!peer || !label) {
        return NULL;
    }
    if (peer->ctx && peer->ctx->disable_sctp) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_INVALID_PEER_STATE,
                          "data channels are disabled for this DTLS transport");
        return NULL;
    }

    uint16_t channel_id;
    if (peer_alloc_channel_id(peer, &channel_id) != 0) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_ALLOC_CHANNEL, "no free channel IDs");
        return NULL;
    }

    turbo_dc_channel_t *channel = calloc(1, sizeof(*channel));
    if (!channel) {
        peer_free_channel_id(peer, channel_id);
        dc_set_peer_error(peer, TURBO_DC_ERROR_ALLOC_CHANNEL, NULL);
        return NULL;
    }

    channel->peer = peer;
    channel->id = channel_id;
    channel->label = tstr_dup(label);

    if (config) {
        channel->config = *config;
        if (config->protocol) {
            channel->protocol = tstr_dup(config->protocol);
        }
    } else {
        channel->config = turbo_dc_default_channel_config();
    }

    peer->channels[channel->id] = channel;
    return channel;
}

int turbo_dc_channel_open(turbo_dc_channel_t *channel) {
    if (!channel || !channel->peer) {
        return -1;
    }

    if (channel->peer->state != TURBO_DC_STATE_CONNECTED) {
        TLOG_INFO("DCEP OPEN deferred sid={} peer_state={}", channel->id, (int)channel->peer->state);
        dc_set_peer_error(channel->peer, TURBO_DC_ERROR_PEER_NOT_CONNECTED, NULL);
        return -1;
    }

    if (channel->dcep_sent) {
        return 0;
    }

    if (dcep_send_open(channel) != 0) {
        dc_set_peer_error(channel->peer, TURBO_DC_ERROR_DCEP_SEND_OPEN, NULL);
        return -1;
    }

    channel->dcep_sent = 1;
    TLOG_INFO("DCEP OPEN queued sid={} label='{}'", channel->id, channel->label ? channel->label : "");
    return 0;
}

void turbo_dc_channel_on_open(turbo_dc_channel_t *channel, turbo_dc_open_cb cb) {
    if (channel) channel->on_open = cb;
}

void turbo_dc_channel_on_message(turbo_dc_channel_t *channel, turbo_dc_message_cb cb) {
    if (channel) channel->on_message = cb;
}

void turbo_dc_channel_on_close(turbo_dc_channel_t *channel, turbo_dc_close_cb cb) {
    if (channel) channel->on_close = cb;
}

void turbo_dc_channel_set_user_data(turbo_dc_channel_t *channel, void *user_data) {
    if (channel) channel->user_data = user_data;
}

void *turbo_dc_channel_get_user_data(turbo_dc_channel_t *channel) {
    return channel ? channel->user_data : NULL;
}

int turbo_dc_channel_send(
    turbo_dc_channel_t *channel,
    const void *data,
    size_t len,
    int is_binary
) {
    if (!channel || !channel->peer) {
        return -1;
    }
    if (!data) {
        dc_set_peer_error(channel->peer, TURBO_DC_ERROR_NULL_DATA, NULL);
        return -1;
    }

    if (!channel->is_open) {
        dc_set_peer_error(channel->peer, TURBO_DC_ERROR_CHANNEL_NOT_OPEN, NULL);
        return -1;
    }

    turbo_dc_peer_t *peer = channel->peer;
    if (!peer->sctp.socket) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_INVALID_PEER_STATE, NULL);
        return -1;
    }

    uint32_t ppid;
    if (len == 0) {
        ppid = is_binary ? SCTP_PPID_BINARY_EMPTY : SCTP_PPID_STRING_EMPTY;
    } else {
        ppid = is_binary ? SCTP_PPID_BINARY : SCTP_PPID_STRING;
    }

    struct sctp_sendv_spa spa;
    memset(&spa, 0, sizeof(spa));
    spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
    spa.sendv_sndinfo.snd_sid = channel->id;
    spa.sendv_sndinfo.snd_ppid = htonl(ppid);

    if (!channel->config.ordered) {
        spa.sendv_sndinfo.snd_flags |= SCTP_UNORDERED;
    }

    ssize_t sent = usrsctp_sendv(peer->sctp.socket, data, len,
                                  NULL, 0, &spa, sizeof(spa),
                                  SCTP_SENDV_SPA, 0);

    if (sent < 0) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_SEND, NULL);
        return -1;
    }

    return 0;
}

const char *turbo_dc_channel_get_label(turbo_dc_channel_t *channel) {
    return channel ? channel->label : NULL;
}

uint16_t turbo_dc_channel_get_id(turbo_dc_channel_t *channel) {
    return channel ? channel->id : 0;
}

int turbo_dc_channel_is_open(turbo_dc_channel_t *channel) {
    return channel ? channel->is_open : 0;
}

size_t turbo_dc_channel_buffered_amount(turbo_dc_channel_t *channel) {
    return channel ? channel->buffered_amount : 0;
}

void turbo_dc_channel_close(turbo_dc_channel_t *channel) {
    if (!channel) return;

    turbo_dc_peer_t *peer = channel->peer;
    uint16_t id = channel->id;

    /* Mark as closed first */
    channel->is_open = 0;

    /* Remove from peer before callback (prevents use-after-free in callback) */
    if (peer && id < MAX_CHANNELS) {
        peer->channels[id] = NULL;
        peer_free_channel_id(peer, id);
    }

    /* Callback after removal - channel pointer still valid but detached */
    if (channel->on_close) {
        channel->on_close(channel, channel->user_data);
    }

    tstr_free(channel->label);
    tstr_free(channel->protocol);
    free(channel);
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

turbo_dc_channel_config_t turbo_dc_default_channel_config(void) {
    turbo_dc_channel_config_t config = {
        .ordered = 1,
        .max_retransmits = 0,
        .max_lifetime_ms = 0,
        .protocol = NULL
    };
    return config;
}

void turbo_dc_handle_timers(void) {
    usrsctp_handle_timers(0);
}
