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
#include "turbo_cnet_send_internal.h"
#include <cmeta_error.h>
#include <stdlib.h>
#include <string.h>
#include <tstr.h>
#include <cmeta_crypto.h>
#include "tlog.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif
#include "ice/salts_ice.h"
#include "turbo_srtp_defs.h"

#include "dc_identity.h"

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

/* ============================================================================
 * Transport Helpers
 * ============================================================================ */

enum {
    DC_CNET_CONNECTION_CAPACITY = 1,
    DC_CNET_QUEUE_CAPACITY = 64,
    DC_CNET_MAX_SEND_BYTES = 64 * 1024,
    DC_CNET_RECEIVE_BYTES = 64 * 1024,
    DC_CNET_POLL_INTERVAL_MS = 1,
    DC_DTLS_POLL_INTERVAL_MS = 10,
    DC_CNET_STOP_TIMEOUT_MS = 5000,
    DC_CNET_LISTENER_BACKLOG = 128,
    DC_CNET_DATAGRAM_SEND_CAPACITY = 64,
    DC_CNET_DATAGRAM_REQUEST_CAPACITY = 128,
    DC_CNET_DATAGRAM_COMPLETION_CAPACITY = 64
};

#define DC_CNET_POLL_INTERVAL_NS \
    ((uint64_t)DC_CNET_POLL_INTERVAL_MS * UINT64_C(1000000))

static SALTS_THREAD_LOCAL turbo_dc_context_t *g_dc_transport_owner = NULL;

static int dc_is_ipv6_host(const char *host) {
    return host && strchr(host, ':') != NULL;
}

static int dc_is_on_transport_thread(const turbo_dc_context_t *ctx) {
    return ctx && g_dc_transport_owner == ctx;
}

int dc_peer_acquire(turbo_dc_peer_t *peer) {
    if (!peer || !peer->operation_sync_initialized) {
        return -1;
    }

    cmeta_mutex_lock(&peer->operation_mutex);
    if (peer->destroying) {
        cmeta_mutex_unlock(&peer->operation_mutex);
        return -1;
    }
    peer->active_operations++;
    cmeta_mutex_unlock(&peer->operation_mutex);
    return 0;
}

void dc_peer_release(turbo_dc_peer_t *peer) {
    if (!peer || !peer->operation_sync_initialized) {
        return;
    }

    cmeta_mutex_lock(&peer->operation_mutex);
    if (peer->active_operations > 0) {
        peer->active_operations--;
    }
    if (peer->destroying && peer->active_operations == 0 &&
        peer->transport_data_callbacks == 0) {
        cmeta_cond_broadcast(&peer->operation_cond);
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
}

static int dc_peer_begin_destroy(turbo_dc_peer_t *peer) {
    if (!peer || !peer->operation_sync_initialized) {
        return -1;
    }

    cmeta_mutex_lock(&peer->operation_mutex);
    if (peer->destroying) {
        cmeta_mutex_unlock(&peer->operation_mutex);
        return -1;
    }
    peer->destroying = 1;
    while (peer->active_operations != 0 ||
           peer->transport_data_callbacks != 0) {
        cmeta_cond_wait(&peer->operation_cond, &peer->operation_mutex);
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
    return 0;
}

static void dc_peer_unlink_from_context(turbo_dc_peer_t *peer) {
    turbo_dc_peer_t **cursor;
    turbo_dc_context_t *ctx;

    if (!peer || !peer->ctx || !peer->ctx->peer_mutex_initialized) {
        return;
    }

    ctx = peer->ctx;
    cmeta_mutex_lock(&ctx->peer_mutex);
    cursor = &ctx->peers_head;
    while (*cursor) {
        if (*cursor == peer) {
            *cursor = peer->next_in_context;
            peer->next_in_context = NULL;
            break;
        }
        cursor = &(*cursor)->next_in_context;
    }
    cmeta_mutex_unlock(&ctx->peer_mutex);
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

    dtls_session_shutdown(peer);
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
    dtls_session_shutdown(peer);
    dc_notify_state(peer, TURBO_DC_STATE_CLOSED);
}

static int dc_post_sync(turbo_dc_context_t *ctx, dc_transport_task_fn fn,
                        void *arg1, void *arg2) {
    if (!ctx || !fn) {
        return SALTS_EINVAL;
    }
    if (dc_is_on_transport_thread(ctx)) {
        fn(arg1, arg2);
        return SALTS_OK;
    }

    cmeta_mutex_lock(&ctx->transport_mutex);
    while (ctx->transport_command_pending && !ctx->transport_stop_requested) {
        cmeta_cond_wait(&ctx->transport_cond, &ctx->transport_mutex);
    }
    if (ctx->transport_stop_requested) {
        cmeta_mutex_unlock(&ctx->transport_mutex);
        return SALTS_ESHUTDOWN;
    }
    ctx->transport_command = fn;
    ctx->transport_command_arg1 = arg1;
    ctx->transport_command_arg2 = arg2;
    ctx->transport_command_done = 0;
    ctx->transport_command_pending = 1;
    cmeta_cond_broadcast(&ctx->transport_cond);
    while (!ctx->transport_command_done && !ctx->transport_stop_requested) {
        cmeta_cond_wait(&ctx->transport_cond, &ctx->transport_mutex);
    }
    ctx->transport_command_pending = 0;
    cmeta_cond_broadcast(&ctx->transport_cond);
    cmeta_mutex_unlock(&ctx->transport_mutex);
    return ctx->transport_stop_requested ? SALTS_ESHUTDOWN : SALTS_OK;
}

static int dc_resolve_datagram_peer(const char *host, uint16_t port,
                                    cnet_datagram_peer *out_peer) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    char port_buf[16];
    int rc;

    if (!host || !host[0] || port == 0 || !out_peer) return SALTS_EINVAL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)port);
    rc = getaddrinfo(host, port_buf, &hints, &result);
    if (rc != 0 || !result) {
        if (result) freeaddrinfo(result);
        return SALTS_ENOENT;
    }

    memset(out_peer, 0, sizeof(*out_peer));
    if (result->ai_family == AF_INET) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)result->ai_addr;
        out_peer->family = CNET_DATAGRAM_ADDRESS_IPV4;
        out_peer->port = ntohs(address->sin_port);
        memcpy(out_peer->address, &address->sin_addr, sizeof(address->sin_addr));
    } else if (result->ai_family == AF_INET6) {
        const struct sockaddr_in6 *address = (const struct sockaddr_in6 *)result->ai_addr;
        out_peer->family = CNET_DATAGRAM_ADDRESS_IPV6;
        out_peer->port = ntohs(address->sin6_port);
        out_peer->scope_id = address->sin6_scope_id;
        memcpy(out_peer->address, &address->sin6_addr, sizeof(address->sin6_addr));
    } else {
        freeaddrinfo(result);
        return SALTS_ENOTSUP;
    }
    freeaddrinfo(result);
    return SALTS_OK;
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
        turbo_dc_transport_data_cb callback = NULL;
        void *user_data = NULL;

        cmeta_mutex_lock(&peer->operation_mutex);
        if (!peer->destroying && peer->on_transport_data) {
            callback = peer->on_transport_data;
            user_data = peer->transport_data_user_data;
            peer->transport_data_callbacks++;
        }
        cmeta_mutex_unlock(&peer->operation_mutex);

        if (callback) {
            callback(user_data, bytes, len);
            cmeta_mutex_lock(&peer->operation_mutex);
            if (peer->transport_data_callbacks > 0) {
                peer->transport_data_callbacks--;
            }
            if (peer->transport_data_callbacks == 0) {
                cmeta_cond_broadcast(&peer->operation_cond);
            }
            cmeta_mutex_unlock(&peer->operation_mutex);
        }
    }
}

static void dc_stream_state_cb(void *user, cnet_connection connection,
                               cnet_connection_state state,
                               const cnet_error *error) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)user;
    (void)connection;
    if (!peer) return;
    if (state == CNET_CONNECTION_CONNECTED) {
        if (cnet_receive(&peer->stream_client, peer->stream_connection, 1u) != SALTS_OK) {
            dc_fail_peer(peer, TURBO_DC_ERROR_CREATE_TRANSPORT,
                         "failed to initialize CNet stream receive");
            return;
        }
        dc_notify_state(peer, TURBO_DC_STATE_CONNECTING);
        if (!peer->ctx->is_server) dtls_process_handshake(peer);
    } else if (state == CNET_CONNECTION_FAILED) {
        dc_fail_peer(peer, TURBO_DC_ERROR_CONNECT_FAILED,
                     error && error->stage ? error->stage : "CNet stream failed");
    } else if (state == CNET_CONNECTION_CLOSED) {
        dc_notify_closed(peer);
    }
}

static void dc_stream_receive_cb(void *user, cnet_connection connection,
                                 const cnet_receive_view *view) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)user;
    if (!peer || !view) return;
    dc_handle_incoming_packet(peer, view->data, view->size);
    (void)cnet_receive(&peer->stream_client, connection, 1u);
}

static void dc_datagram_receive_cb(void *user, cnet_datagram *datagram,
                                   const cnet_datagram_peer *remote,
                                   const cnet_receive_view *view) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)user;
    if (!peer || !remote || !view) return;
    if (peer->ctx->is_server) {
        peer->remote_datagram_peer = *remote;
        peer->has_remote_datagram_peer = 1;
    }
    dc_handle_incoming_packet(peer, view->data, view->size);
    (void)cnet_datagram_receive(datagram, 1u);
}

typedef struct {
    turbo_dc_peer_t *peer;
    const void *data;
    size_t len;
    int status;
} dc_send_command_t;

static void dc_send_task(void *arg1, void *arg2) {
    dc_send_command_t *command = (dc_send_command_t *)arg1;
    turbo_dc_peer_t *peer;
    (void)arg2;
    if (!command || !command->peer) return;
    peer = command->peer;
    command->status = SALTS_ENOTCONN;
    if (peer->ctx->transport == TURBO_DC_TRANSPORT_TCP &&
        peer->stream_client_initialized && peer->stream_connection.generation != 0u) {
        if (command->len > DC_CNET_MAX_SEND_BYTES) {
            command->status = SALTS_EMSGSIZE;
            return;
        }
        command->status = turbo_media_cnet_send_copy(
            &peer->stream_client, peer->stream_connection,
            command->data, command->len, 0);
    } else if (peer->ctx->transport == TURBO_DC_TRANSPORT_UDP &&
               peer->datagram_initialized && peer->has_remote_datagram_peer) {
        command->status = cnet_datagram_send(&peer->datagram,
                                             &peer->remote_datagram_peer,
                                             command->data, command->len, 0u);
    }
}

static void direct_send(turbo_dc_peer_t *peer, const void *data, size_t len) {
    dc_send_command_t command;
    if (!peer || !data || len == 0) return;
    command = (dc_send_command_t){peer, data, len, SALTS_EINVAL};
    if (dc_post_sync(peer->ctx, dc_send_task, &command, NULL) != SALTS_OK ||
        command.status != SALTS_OK) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT,
                          cmeta_strerror(command.status));
    }
}

static void dc_transport_close_task(void *arg1, void *arg2) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)arg1;
    int *status = (int *)arg2;

    if (status) *status = 0;
    if (!peer) return;

    switch (peer->ctx->transport) {
        case TURBO_DC_TRANSPORT_TCP:
            if (peer->stream_client_initialized &&
                peer->stream_connection.generation != 0u)
                (void)cnet_close(&peer->stream_client, peer->stream_connection);
            if (peer->listener_initialized) (void)cnet_listener_close(&peer->listener);
            break;

        case TURBO_DC_TRANSPORT_UDP:
        default:
            if (peer->datagram_initialized)
                (void)cnet_datagram_stop(&peer->datagram, DC_CNET_STOP_TIMEOUT_MS);
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
            if (peer->stream_client_initialized) {
                (void)cnet_client_stop(&peer->stream_client, DC_CNET_STOP_TIMEOUT_MS);
                (void)cnet_client_destroy(&peer->stream_client);
                peer->stream_client_initialized = 0;
            }
            if (peer->listener_initialized) {
                (void)cnet_listener_close(&peer->listener);
                (void)cnet_listener_destroy(&peer->listener);
                peer->listener_initialized = 0;
            }
            break;

        case TURBO_DC_TRANSPORT_UDP:
        default:
            if (peer->datagram_initialized) {
                (void)cnet_datagram_stop(&peer->datagram, DC_CNET_STOP_TIMEOUT_MS);
                (void)cnet_datagram_destroy(&peer->datagram);
                peer->datagram_initialized = 0;
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
    if (!peer || !peer->ctx) return;

    switch (peer->ctx->transport) {
        case TURBO_DC_TRANSPORT_TCP:
        {
            cnet_client_config client_config = {
                .backend = dc_is_ipv6_host(peer->remote_host)
#if defined(_WIN32)
                               ? NATIVE_IO_BACKEND_IOCP : NATIVE_IO_BACKEND_IOCP,
#else
                               ? NATIVE_IO_BACKEND_EPOLL : NATIVE_IO_BACKEND_EPOLL,
#endif
                .connection_capacity = DC_CNET_CONNECTION_CAPACITY,
                .command_capacity = DC_CNET_QUEUE_CAPACITY,
                .request_capacity = DC_CNET_QUEUE_CAPACITY,
                .completion_batch_capacity = DC_CNET_QUEUE_CAPACITY,
                .event_capacity = DC_CNET_QUEUE_CAPACITY,
                .max_send_bytes = DC_CNET_MAX_SEND_BYTES,
                .receive_buffer_bytes = DC_CNET_RECEIVE_BYTES,
                .connect_timeout_ms = DC_CNET_STOP_TIMEOUT_MS,
                .read_timeout_ms = 0,
                .write_timeout_ms = DC_CNET_STOP_TIMEOUT_MS,
                .tls_io_buffer_bytes = 0,
                .tls_handshake_timeout_ms = 0,
                .command_buffer_bytes = DC_CNET_MAX_SEND_BYTES,
                .event_buffer_bytes = DC_CNET_RECEIVE_BYTES
            };
            cnet_observer observer = {
                .on_state = dc_stream_state_cb,
                .on_receive = dc_stream_receive_cb,
                .user = peer,
                .on_send = NULL
            };
            int rc = cnet_client_init(&peer->stream_client, &client_config);
            if (rc != SALTS_OK) {
                dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, cmeta_strerror(rc));
                return;
            }
            peer->stream_client_initialized = 1;
            if (peer->ctx->is_server) {
                cnet_listener_config listener_config = {
                    .backend = client_config.backend,
                    .host = peer->remote_host && peer->remote_host[0]
                                ? peer->remote_host : "0.0.0.0",
                    .port = peer->remote_port,
                    .backlog = DC_CNET_LISTENER_BACKLOG
                };
                rc = cnet_listener_init(&peer->listener, &listener_config);
                if (rc != SALTS_OK) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_LISTEN_FAILED, cmeta_strerror(rc));
                    return;
                }
                peer->listener_initialized = 1;
                peer->transport_ops = &g_direct_ops;
                dc_notify_state(peer, TURBO_DC_STATE_CONNECTING);
                if (status) *status = 0;
            } else {
                char uri[512];
                int written = snprintf(uri, sizeof(uri),
                    dc_is_ipv6_host(peer->remote_host) ? "tcp://[%s]:%u" : "tcp://%s:%u",
                    peer->remote_host, (unsigned)peer->remote_port);
                cnet_connect_options options = {.uri = uri, .observer = observer};
                if (written < 0 || (size_t)written >= sizeof(uri)) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CONNECT_FAILED, "CNet URI too long");
                    return;
                }
                rc = cnet_connect(&peer->stream_client, &options,
                                  &peer->stream_connection);
                if (rc != SALTS_OK) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CONNECT_FAILED, cmeta_strerror(rc));
                    return;
                }
                peer->transport_ops = &g_direct_ops;
                if (status) *status = 0;
            }
            break;
        }

        case TURBO_DC_TRANSPORT_KCP:
            dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT,
                              "KCP transport requires a configured CNet packet session");
            return;

        case TURBO_DC_TRANSPORT_UDP:
        default: {
            cnet_datagram_config config = CNET_DATAGRAM_CONFIG_INIT;
            const char *bind_host = dc_is_ipv6_host(peer->remote_host) ? "::" : "0.0.0.0";
            int rc;
            config.backend =
#if defined(_WIN32)
                NATIVE_IO_BACKEND_IOCP;
#else
                NATIVE_IO_BACKEND_EPOLL;
#endif
            config.host = peer->ctx->is_server && peer->remote_host && peer->remote_host[0]
                            ? peer->remote_host : bind_host;
            config.port = peer->ctx->is_server ? peer->remote_port : 0;
            config.send_capacity = DC_CNET_DATAGRAM_SEND_CAPACITY;
            config.request_capacity = DC_CNET_DATAGRAM_REQUEST_CAPACITY;
            config.completion_batch_capacity = DC_CNET_DATAGRAM_COMPLETION_CAPACITY;
            config.max_datagram_bytes = CNET_DATAGRAM_MAX_PAYLOAD_BYTES;
            config.receive_buffer_bytes = CNET_DATAGRAM_MAX_PAYLOAD_BYTES;
            config.observer.on_receive = dc_datagram_receive_cb;
            config.observer.user = peer;
            rc = cnet_datagram_init(&peer->datagram, &config);
            if (rc != SALTS_OK) {
                dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, cmeta_strerror(rc));
                return;
            }
            peer->datagram_initialized = 1;
            if (peer->ctx->is_server) {
                if (cnet_datagram_receive(&peer->datagram, 1u) != SALTS_OK) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to start datagram receive");
                    return;
                }
                peer->transport_ops = &g_direct_ops;
                dc_notify_state(peer, TURBO_DC_STATE_CONNECTING);
                if (status) *status = 0;
            } else {
                rc = dc_resolve_datagram_peer(peer->remote_host, peer->remote_port,
                                              &peer->remote_datagram_peer);
                if (rc != SALTS_OK) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CONNECT_FAILED, cmeta_strerror(rc));
                    return;
                }
                peer->has_remote_datagram_peer = 1;
                if (cnet_datagram_receive(&peer->datagram, 1u) != SALTS_OK) {
                    dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to start datagram receive");
                    return;
                }
                peer->transport_ops = &g_direct_ops;
                dc_notify_state(peer, TURBO_DC_STATE_CONNECTING);
                dtls_process_handshake(peer);
                if (status) *status = 0;
            }
            break;
        }
    }
}

static void dc_poll_direct_peer(turbo_dc_peer_t *peer) {
    size_t events = 0;
    if (!peer) return;
    if (peer->listener_initialized && peer->stream_connection.generation == 0u) {
        int ready = 0;
        if (cnet_listener_wait(&peer->listener, 0u, &ready) == SALTS_OK && ready) {
            cnet_observer observer = {
                .on_state = dc_stream_state_cb,
                .on_receive = dc_stream_receive_cb,
                .user = peer,
                .on_send = NULL
            };
            (void)cnet_listener_accept(&peer->listener, &peer->stream_client,
                                       &observer, &peer->stream_connection);
        }
    }
    if (peer->stream_client_initialized)
        (void)cnet_client_poll(&peer->stream_client, 0u, &events);
    if (peer->datagram_initialized)
        (void)cnet_datagram_poll(&peer->datagram, 0u, &events);
}

static void dc_transport_thread_main(void *arg) {
    turbo_dc_context_t *ctx = (turbo_dc_context_t *)arg;
    if (!ctx) return;
    g_dc_transport_owner = ctx;
    for (;;) {
        dc_transport_task_fn command = NULL;
        void *arg1 = NULL;
        void *arg2 = NULL;
        turbo_dc_peer_t *peer;

        cmeta_mutex_lock(&ctx->transport_mutex);
        if (!ctx->transport_stop_requested && !ctx->transport_command_pending) {
            if (ctx->transport == TURBO_DC_TRANSPORT_ICE && ctx->active_dtls_timers == 0) {
                cmeta_cond_wait(&ctx->transport_cond, &ctx->transport_mutex);
            } else {
                uint64_t interval = ctx->transport == TURBO_DC_TRANSPORT_ICE
                    ? (uint64_t)DC_DTLS_POLL_INTERVAL_MS * UINT64_C(1000000)
                    : DC_CNET_POLL_INTERVAL_NS;
                (void)cmeta_cond_timedwait(&ctx->transport_cond, &ctx->transport_mutex,
                                           interval);
            }
        }
        if (ctx->transport_stop_requested) {
            cmeta_mutex_unlock(&ctx->transport_mutex);
            break;
        }
        if (ctx->transport_command_pending && !ctx->transport_command_done) {
            command = ctx->transport_command;
            arg1 = ctx->transport_command_arg1;
            arg2 = ctx->transport_command_arg2;
        }
        cmeta_mutex_unlock(&ctx->transport_mutex);

        if (command) {
            command(arg1, arg2);
            cmeta_mutex_lock(&ctx->transport_mutex);
            ctx->transport_command_done = 1;
            cmeta_cond_broadcast(&ctx->transport_cond);
            cmeta_mutex_unlock(&ctx->transport_mutex);
            continue;
        }

        cmeta_mutex_lock(&ctx->peer_mutex);
        peer = ctx->peers_head;
        while (peer) {
            turbo_dc_peer_t *next;
            if (dc_peer_acquire(peer) == 0) {
                /* Keep this node linked, but never hold the list mutex across
                 * transport/SCTP/application callbacks. Destruction drains the
                 * lease before unlinking, so next is read from a live node. */
                cmeta_mutex_unlock(&ctx->peer_mutex);
                dc_poll_direct_peer(peer);
                dtls_session_poll_timeout(peer);
                cmeta_mutex_lock(&ctx->peer_mutex);
                next = peer->next_in_context;
                dc_peer_release(peer);
            } else {
                next = peer->next_in_context;
            }
            peer = next;
        }
        cmeta_mutex_unlock(&ctx->peer_mutex);
    }
    g_dc_transport_owner = NULL;
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
    (void)ice_agent_send((salts_ice_agent_t *)transport, data, len);
}

/* ============================================================================
 * Transport Helper
 * ============================================================================ */

TURBO_MEDIA_C_API void turbo_dc_peer_send_transport_data(turbo_dc_peer_t *peer, const void *data, size_t len) {
    if (!peer || dc_peer_acquire(peer) != 0) {
        return;
    }
    if (peer->transport_ops && peer->transport_ops->send) {
        peer->transport_ops->send(peer, data, len);
    }
    dc_peer_release(peer);
}

/* Internal alias for backward compatibility */
void dc_send_transport_data(turbo_dc_peer_t *peer, const void *data, size_t len) {
    turbo_dc_peer_send_transport_data(peer, data, len);
}

TURBO_MEDIA_C_API void turbo_dc_peer_set_transport_data_handler(
    turbo_dc_peer_t *peer,
    turbo_dc_transport_data_cb cb,
    void *user_data) {
    if (!peer || dc_peer_acquire(peer) != 0) {
        return;
    }

    cmeta_mutex_lock(&peer->operation_mutex);
    peer->on_transport_data = cb;
    peer->transport_data_user_data = user_data;
    if (!cb) {
        while (peer->transport_data_callbacks != 0) {
            cmeta_cond_wait(&peer->operation_cond, &peer->operation_mutex);
        }
    }
    cmeta_mutex_unlock(&peer->operation_mutex);
    dc_peer_release(peer);
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

    if (ctx->dtls_mtu < 256 ||
        turbo_dc_identity_create(config->cert_pem, config->key_pem,
                                  &ctx->dtls_context, &ctx->local_fingerprint) != 0) {
        free(ctx);
        return NULL;
    }
    ctx->local_fingerprint_hash = tstr_dup("sha-256");
    if (!ctx->local_fingerprint_hash) {
        turbo_gdtls_context_destroy(ctx->dtls_context);
        tstr_free(ctx->local_fingerprint);
        free(ctx);
        return NULL;
    }

    if (sctp_global_init() != 0) {
        dc_set_context_error(ctx, TURBO_DC_ERROR_SCTP_INIT, NULL);
        tstr_free(ctx->local_fingerprint);
        tstr_free(ctx->local_fingerprint_hash);
        turbo_gdtls_context_destroy(ctx->dtls_context);
        free(ctx);
        return NULL;
    }

    cmeta_mutex_init(&ctx->peer_mutex);
    ctx->peer_mutex_initialized = 1;

    {
        cmeta_mutex_init(&ctx->transport_mutex);
        cmeta_cond_init(&ctx->transport_cond);
        ctx->transport_sync_initialized = 1;
        if (cmeta_thread_create(&ctx->transport_thread, dc_transport_thread_main, ctx) != 0) {
            dc_set_context_error(ctx, TURBO_DC_ERROR_CREATE_TRANSPORT,
                                 "failed to start transport/DTLS owner");
            cmeta_cond_destroy(&ctx->transport_cond);
            cmeta_mutex_destroy(&ctx->transport_mutex);
            ctx->transport_sync_initialized = 0;
            cmeta_mutex_destroy(&ctx->peer_mutex);
            ctx->peer_mutex_initialized = 0;
            sctp_global_cleanup();
            turbo_gdtls_context_destroy(ctx->dtls_context);
            tstr_free(ctx->local_fingerprint);
            tstr_free(ctx->local_fingerprint_hash);
            free(ctx);
            return NULL;
        }
        ctx->transport_thread_started = 1;
    }

    ctx->initialized = 1;
    return ctx;
}

void turbo_dc_context_destroy(turbo_dc_context_t *ctx) {
    turbo_dc_peer_t *peer;

    if (!ctx) return;

    /* Context destruction is the owner-level quiesce point.  Peers must be
     * gone before the CNet owner, SCTP resources, or context storage is
     * released. */
    if (ctx->peer_mutex_initialized) {
        cmeta_mutex_lock(&ctx->peer_mutex);
        ctx->destroying = 1;
        cmeta_mutex_unlock(&ctx->peer_mutex);

        for (;;) {
            cmeta_mutex_lock(&ctx->peer_mutex);
            peer = ctx->peers_head;
            cmeta_mutex_unlock(&ctx->peer_mutex);
            if (!peer) {
                break;
            }
            turbo_dc_peer_destroy(peer);
        }
    }

    if (ctx->transport_sync_initialized && ctx->transport_thread_started) {
        cmeta_mutex_lock(&ctx->transport_mutex);
        ctx->transport_stop_requested = 1;
        cmeta_cond_broadcast(&ctx->transport_cond);
        cmeta_mutex_unlock(&ctx->transport_mutex);
    }
    if (ctx->transport_thread_started) {
        cmeta_thread_join(&ctx->transport_thread);
        cmeta_thread_destroy(&ctx->transport_thread);
        ctx->transport_thread_started = 0;
    }
    if (ctx->transport_sync_initialized) {
        cmeta_cond_destroy(&ctx->transport_cond);
        cmeta_mutex_destroy(&ctx->transport_mutex);
        ctx->transport_sync_initialized = 0;
    }

    if (ctx->dtls_context) {
        turbo_gdtls_context_destroy(ctx->dtls_context);
    }

    sctp_global_cleanup();

    tstr_free(ctx->local_fingerprint);
    tstr_free(ctx->local_fingerprint_hash);
    if (ctx->peer_mutex_initialized) {
        cmeta_mutex_destroy(&ctx->peer_mutex);
        ctx->peer_mutex_initialized = 0;
    }
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
    int operation_sync_initialized = 0;

    if (!ctx) {
        return NULL;
    }

    if (!ctx->peer_mutex_initialized) {
        return NULL;
    }

    cmeta_mutex_lock(&ctx->peer_mutex);
    if (ctx->destroying) {
        cmeta_mutex_unlock(&ctx->peer_mutex);
        return NULL;
    }

    turbo_dc_peer_t *peer = calloc(1, sizeof(*peer));
    if (!peer) {
        dc_set_context_error(ctx, TURBO_DC_ERROR_ALLOC_PEER, NULL);
        cmeta_mutex_unlock(&ctx->peer_mutex);
        return NULL;
    }

    peer->ctx = ctx;
    peer->user_data = user_data;
    peer->state = TURBO_DC_STATE_NEW;
    peer->is_dtls_server = ctx->is_server;

    cmeta_mutex_init(&peer->operation_mutex);
    cmeta_cond_init(&peer->operation_cond);
    peer->operation_sync_initialized = 1;
    operation_sync_initialized = 1;

    /* Initialize channel ID bitmap */
    peer->channel_ids = roaring_bitmap_create();
    if (!peer->channel_ids) {
        dc_set_context_error(ctx, TURBO_DC_ERROR_ALLOC_PEER, "failed to create channel bitmap");
        goto peer_create_fail;
    }

    if (hash_map_init_bytes(&peer->channels, sizeof(uint16_t), CMETA_ALIGNOF(uint16_t),
                            sizeof(turbo_dc_channel_t *),
                            CMETA_ALIGNOF(turbo_dc_channel_t *), UINT16_MAX,
                            hash_bytes, hash_key_equal, NULL) != STL_OK) {
        dc_set_context_error(ctx, TURBO_DC_ERROR_ALLOC_PEER, "failed to create channel map");
        goto peer_create_fail;
    }
    peer->channels_initialized = 1;

    if (remote_host) {
        peer->remote_host = tstr_dup(remote_host);
    }
    peer->remote_port = remote_port;

    if (dtls_session_init(peer) != 0) {
        goto peer_create_fail;
    }

    if (!ctx->disable_sctp && sctp_session_init(peer) != 0) {
        goto peer_create_fail;
    }

    peer->next_in_context = ctx->peers_head;
    ctx->peers_head = peer;
    cmeta_mutex_unlock(&ctx->peer_mutex);
    return peer;

peer_create_fail:
    if (peer->sctp.socket) {
        usrsctp_close(peer->sctp.socket);
        peer->sctp.socket = NULL;
    }
    if (peer->sctp_address_registered) {
        usrsctp_deregister_address(peer);
        peer->sctp_address_registered = 0;
    }
    if (peer->dtls.engine) {
        turbo_gdtls_destroy(peer->dtls.engine);
        peer->dtls.engine = NULL;
    }
    if (peer->channels_initialized) {
        hash_map_destroy(&peer->channels);
        peer->channels_initialized = 0;
    }
    if (peer->channel_ids) {
        roaring_bitmap_free(peer->channel_ids);
    }
    tstr_free(peer->remote_host);
    if (operation_sync_initialized) {
        cmeta_cond_destroy(&peer->operation_cond);
        cmeta_mutex_destroy(&peer->operation_mutex);
    }
    free(peer);
    cmeta_mutex_unlock(&ctx->peer_mutex);
    return NULL;
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
    tstr new_hash;
    tstr new_fingerprint;
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

    if (dc_peer_acquire(peer) != 0) {
        tstr_free(new_hash);
        tstr_free(new_fingerprint);
        return -1;
    }
    dtls_session_lock(peer);
    if (peer->dtls.handshake_started || peer->dtls.stopped) {
        /* Reapplying the same SDP pin is harmless; changing an admitted
         * identity requires a new peer association. */
        int same = peer->remote_fingerprint &&
                   strcmp(peer->remote_fingerprint, new_fingerprint) == 0;
        dtls_session_unlock(peer);
        dc_peer_release(peer);
        tstr_free(new_hash);
        tstr_free(new_fingerprint);
        return same ? 0 : -5;
    }
    tstr_free(peer->remote_fingerprint_hash);
    tstr_free(peer->remote_fingerprint);
    peer->remote_fingerprint_hash = new_hash;
    peer->remote_fingerprint = new_fingerprint;
    dtls_session_unlock(peer);
    dc_peer_release(peer);
    return 0;
}

int turbo_dc_peer_set_dtls_role(turbo_dc_peer_t *peer, int is_server) {
    if (!peer || dc_peer_acquire(peer) != 0) return -1;
    dtls_session_lock(peer);
    if (peer->state != TURBO_DC_STATE_NEW || peer->dtls.handshake_started ||
        peer->dtls.handshake_done || peer->dtls.stopped) {
        dtls_session_unlock(peer);
        dc_peer_release(peer);
        return -2;
    }

    peer->is_dtls_server = is_server ? 1 : 0;

    dtls_session_unlock(peer);
    dc_peer_release(peer);
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

    if (dc_peer_acquire(peer) != 0) {
        return -1;
    }

    if (!transport) {
        peer->external_transport = NULL;
        peer->external_transport_send = NULL;
        if (peer->transport_ops == &g_external_transport_ops) {
            peer->transport_ops = NULL;
        }
        dc_peer_release(peer);
        return 0;
    }

    if (!send_cb) {
        dc_peer_release(peer);
        return -1;
    }
    if (peer->transport_ops && peer->transport_ops != &g_external_transport_ops) {
        dc_peer_release(peer);
        return -2;
    }

    peer->external_transport = transport;
    peer->external_transport_send = send_cb;
    peer->transport_ops = &g_external_transport_ops;

    dc_peer_release(peer);
    return 0;
}

void turbo_dc_peer_feed_transport_data(turbo_dc_peer_t *peer,
                                       const void *data,
                                       size_t len) {
    if (!peer || !data || len == 0 || dc_peer_acquire(peer) != 0) return;

    dc_handle_incoming_packet(peer, data, len);
    dc_peer_release(peer);
}

int turbo_dc_peer_set_ice_agent(turbo_dc_peer_t *peer, struct salts_ice_agent_s *ice_agent) {
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

    if (!peer || dc_peer_acquire(peer) != 0) {
        return -1;
    }

    if (peer->external_transport) {
        TLOG_INFO("DC peer connect via ICE");
        dtls_session_lock(peer);
        if (!peer->dtls.handshake_done && !peer->dtls.stopped)
            peer->state = TURBO_DC_STATE_CONNECTING;
        dtls_session_unlock(peer);

        /* Start DTLS handshake - the ICE agent should already be connected */
        /* Use dtls_process_handshake to properly schedule retransmit timer */
        dtls_process_handshake(peer);

        dc_peer_release(peer);
        return 0;
    }

    /* ICE transport requires ICE agent to be set first */
    if (peer->ctx->transport == TURBO_DC_TRANSPORT_ICE) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "ICE transport requires ice_agent");
        dc_peer_release(peer);
        return -1;
    }

    if (!peer->ctx->transport_sync_initialized ||
        !peer->ctx->transport_thread_started) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "transport context is not initialized");
        dc_peer_release(peer);
        return -1;
    }

    if (dc_post_sync(peer->ctx, dc_start_transport_task, peer, &status) != 0) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_CREATE_TRANSPORT, "failed to schedule transport setup");
        dc_peer_release(peer);
        return -1;
    }

    dc_peer_release(peer);
    return status;
}

void turbo_dc_peer_poll(turbo_dc_peer_t *peer) {
    if (!peer || dc_peer_acquire(peer) != 0) {
        return;
    }

    if (peer->sctp.socket) {
        sctp_poll_status(peer, "peer-poll");
    }
    dc_peer_release(peer);
}

turbo_dc_state_t turbo_dc_peer_get_state(turbo_dc_peer_t *peer) {
    return peer ? peer->state : TURBO_DC_STATE_CLOSED;
}

static void dc_peer_close_impl(turbo_dc_peer_t *peer) {
    if (!peer || peer->state == TURBO_DC_STATE_CLOSED) {
        return;
    }

    dc_notify_state(peer, TURBO_DC_STATE_DISCONNECTING);

    if (peer->sctp.socket) {
        usrsctp_shutdown(peer->sctp.socket, SHUT_RDWR);
        usrsctp_close(peer->sctp.socket);
        peer->sctp.socket = NULL;
    }

    dtls_session_shutdown(peer);

    if (peer->transport_ops && peer->transport_ops->close) {
        peer->transport_ops->close(peer);
    }

    dc_notify_closed(peer);
}

void turbo_dc_peer_close(turbo_dc_peer_t *peer) {
    if (!peer || dc_peer_acquire(peer) != 0) return;
    dc_peer_close_impl(peer);
    dc_peer_release(peer);
}

void turbo_dc_peer_destroy(turbo_dc_peer_t *peer) {
    size_t slot;

    if (!peer || dc_peer_begin_destroy(peer) != 0) return;

    dc_peer_close_impl(peer);

    /* A transport can have reported CLOSED before explicit close. Always
     * remove its deadline registration, including that already-closed path. */
    dtls_session_shutdown(peer);

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

    /* Clean up channels.  The map is bounded by the uint16_t stream-id
     * space, without imposing a 512 KiB pointer array on every peer. */
    while (peer->channels_initialized &&
           !hash_map_empty(&peer->channels)) {
        int removed = 0;
        for (slot = 0; slot < hash_map_capacity(&peer->channels); slot++) {
            const uint16_t *id = (const uint16_t *)hash_map_key_at(
                &peer->channels, slot);
            turbo_dc_channel_t **channel = (turbo_dc_channel_t **)
                hash_map_value_at(&peer->channels, slot);
            if (id && channel && *channel) {
                turbo_dc_channel_t *owned = *channel;
                hash_map_remove(&peer->channels, id, NULL);
                tstr_free(owned->label);
                tstr_free(owned->protocol);
                free(owned);
                removed = 1;
                break;
            }
        }
        if (!removed) {
            break;
        }
    }

    if (peer->channels_initialized) {
        hash_map_destroy(&peer->channels);
        peer->channels_initialized = 0;
    }

    /* Free channel ID bitmap */
    if (peer->channel_ids) {
        roaring_bitmap_free(peer->channel_ids);
    }
 
    tstr_free(peer->remote_host);
    tstr_free(peer->remote_fingerprint);
    tstr_free(peer->remote_fingerprint_hash);

    /* Keep the peer linked until every cleanup path that can touch ctx has
     * completed.  A concurrent context destroy therefore cannot mistake an
     * in-flight peer destruction for an empty peer list. */
    dc_peer_unlink_from_context(peer);

    cmeta_cond_destroy(&peer->operation_cond);
    cmeta_mutex_destroy(&peer->operation_mutex);

    free(peer);
}
 
uint16_t turbo_dc_peer_get_srtp_keys(turbo_dc_peer_t *peer, void *material) {
    uint16_t result = 0;
    if (!peer || !material || dc_peer_acquire(peer) != 0) return 0;
    dtls_session_lock(peer);
    if (peer->dtls.handshake_done && !peer->dtls.stopped) {
        uint16_t profile = turbo_gdtls_srtp_profile(peer->dtls.engine);
        uint8_t exported[88];
        size_t size = 0;
        size_t key_len = srtp_profile_key_len(profile);
        size_t salt_len = srtp_profile_salt_len(profile);
        if (profile && turbo_gdtls_export_srtp(peer->dtls.engine, exported,
                sizeof(exported), &size) == 0 && size == 2 * (key_len + salt_len)) {
            srtp_keying_material_t *keys = material;
            memcpy(keys->client_key, exported, key_len);
            memcpy(keys->server_key, exported + key_len, key_len);
            memcpy(keys->client_salt, exported + 2 * key_len, salt_len);
            memcpy(keys->server_salt, exported + 2 * key_len + salt_len, salt_len);
            keys->key_len = key_len;
            keys->salt_len = salt_len;
            result = profile;
        }
        cmeta_crypto_clear(exported, sizeof(exported));
    }
    dtls_session_unlock(peer);
    dc_peer_release(peer);
    return result;
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
        if (config->max_retransmits < 0 || config->max_lifetime_ms < 0 ||
            (config->max_retransmits > 0 && config->max_lifetime_ms > 0)) {
            peer_free_channel_id(peer, channel_id);
            free(channel);
            dc_set_peer_error(peer, TURBO_DC_ERROR_INVALID_PEER_STATE,
                              "invalid partial reliability configuration");
            return NULL;
        }
        channel->config = *config;
        if (config->protocol) {
            channel->protocol = tstr_dup(config->protocol);
        }
    } else {
        channel->config = turbo_dc_default_channel_config();
    }

    if (peer_set_channel(peer, channel->id, channel) != 0) {
        peer_free_channel_id(peer, channel_id);
        tstr_free(channel->label);
        tstr_free(channel->protocol);
        free(channel);
        dc_set_peer_error(peer, TURBO_DC_ERROR_ALLOC_CHANNEL, "channel map is full");
        return NULL;
    }
    return channel;
}

int turbo_dc_channel_open(turbo_dc_channel_t *channel) {
    if (!channel || !channel->peer) {
        return -1;
    }

    if (channel->peer->state != TURBO_DC_STATE_CONNECTED) {
        TLOG_INFOF("DCEP OPEN deferred sid={} peer_state={}", channel->id, (int)channel->peer->state);
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
    TLOG_INFOF("DCEP OPEN queued sid={} label='{}'", channel->id, channel->label ? channel->label : "");
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

    if (channel->config.max_retransmits < 0 || channel->config.max_lifetime_ms < 0 ||
        (channel->config.max_retransmits > 0 && channel->config.max_lifetime_ms > 0)) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_SEND,
                          "retransmit and lifetime limits are mutually exclusive");
        return -1;
    }
    if (channel->config.max_retransmits > UINT32_MAX ||
        channel->config.max_lifetime_ms > UINT32_MAX) {
        dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_SEND, "partial reliability limit is too large");
        return -1;
    }
    if (channel->config.max_retransmits > 0) {
        spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
        spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_RTX;
        spa.sendv_prinfo.pr_value = (uint32_t)channel->config.max_retransmits;
    } else if (channel->config.max_lifetime_ms > 0) {
        spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
        spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_TTL;
        spa.sendv_prinfo.pr_value = (uint32_t)channel->config.max_lifetime_ms;
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

    /* Reset the SCTP stream before detaching the local channel. */
    if (peer && peer->sctp.socket) {
        if (sctp_reset_channel_stream(peer, id) != 0) {
            dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_SEND, "stream reset failed");
        }
    }

    /* Remove from peer before callback (prevents use-after-free in callback) */
    if (peer) {
        peer_remove_channel(peer, id);
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
