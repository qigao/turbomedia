/** TurboMedia stream/datagram transport backed by Salts CNet/CHTTP. */
#include "turbo_transport.h"
#include "transport_internal.h"

#include <salts/clock.h>
#include <salts/error_codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

enum {
    TRANSPORT_QUEUE_CAPACITY = 16,
    TRANSPORT_MAX_BYTES = 1024 * 1024,
    TRANSPORT_WS_FRAME_BYTES = 64 * 1024,
    TRANSPORT_WS_FRAME_HEADER_BYTES = 14,
    TRANSPORT_CONNECT_TIMEOUT_MS = 30000,
    TRANSPORT_STOP_TIMEOUT_MS = 5000
};

turbo_transport_t *turbo_transport_create_http(const turbo_transport_config_t *config);
int turbo_transport_destroy_http(turbo_transport_t *transport);
int turbo_transport_connect_http(turbo_transport_t *transport);
int turbo_transport_disconnect_http(turbo_transport_t *transport);
int turbo_transport_is_connected_http(turbo_transport_t *transport);
void turbo_transport_set_event_callback_http(
    turbo_transport_t *transport, turbo_transport_event_cb callback,
    void *user_data);
const char *turbo_transport_get_error_http(turbo_transport_t *transport);

typedef struct turbo_transport_s {
    turbo_transport_base_t base;
    cnet_client owned_client;
    cnet_client *client;
    cnet_connection connection;
    cnet_datagram datagram;
    cnet_datagram_peer datagram_peer;
    chttp_websocket_client websocket;
    chttp_tls_profile websocket_tls;
    int owns_client;
    int datagram_initialized;
    int websocket_initialized;
    int websocket_tls_initialized;
    int connected;
    int connection_active;
    int terminal;
    int receive_ready;
    int receive_is_text;
    int send_pending;
    int send_completed;
    uint8_t *receive_data;
    size_t receive_size;
    char error_msg[256];
    turbo_transport_event_cb event_callback;
    void *event_user_data;
} turbo_transport_impl_t;

static char *transport_dup(const char *value) {
    size_t size;
    char *copy;
    if (!value) return NULL;
    size = strlen(value) + 1;
    copy = (char *)malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}

static native_io_backend_kind transport_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static uint32_t transport_timeout(const turbo_transport_impl_t *transport) {
    return transport->base.config.read_timeout_ms > 0
        ? (uint32_t)transport->base.config.read_timeout_ms : TRANSPORT_CONNECT_TIMEOUT_MS;
}

static void transport_set_error(turbo_transport_impl_t *transport, const char *error) {
    snprintf(transport->error_msg, sizeof(transport->error_msg), "%s",
             error ? error : "CNet transport failed");
}

static void transport_fire_event(turbo_transport_impl_t *transport,
                                 turbo_transport_event_t event, void *data) {
    if (transport->event_callback)
        transport->event_callback((turbo_transport_t *)transport, event, data,
                                  transport->event_user_data);
}

static cnet_client_config transport_client_config(
    const turbo_transport_impl_t *transport) {
    uint32_t connect_timeout = transport->base.config.connect_timeout_ms > 0
        ? (uint32_t)transport->base.config.connect_timeout_ms : TRANSPORT_CONNECT_TIMEOUT_MS;
    return (cnet_client_config){
        .backend = transport_backend(),
        .connection_capacity = 1,
        .command_capacity = TRANSPORT_QUEUE_CAPACITY,
        .request_capacity = TRANSPORT_QUEUE_CAPACITY,
        .completion_batch_capacity = TRANSPORT_QUEUE_CAPACITY,
        .event_capacity = TRANSPORT_QUEUE_CAPACITY,
        .max_send_bytes = TRANSPORT_MAX_BYTES,
        .receive_buffer_bytes = TRANSPORT_MAX_BYTES,
        .connect_timeout_ms = connect_timeout,
        .read_timeout_ms = transport_timeout(transport),
        .write_timeout_ms = transport->base.config.write_timeout_ms > 0
            ? (uint32_t)transport->base.config.write_timeout_ms : TRANSPORT_CONNECT_TIMEOUT_MS,
        .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
        .tls_handshake_timeout_ms = connect_timeout,
        .command_buffer_bytes = TRANSPORT_MAX_BYTES,
        .event_buffer_bytes = TRANSPORT_MAX_BYTES
    };
}

static int copy_receive(turbo_transport_impl_t *transport,
                        const void *data, size_t size, int is_text) {
    uint8_t *copy = (uint8_t *)malloc(size ? size : 1);
    if (!copy) return SALTS_ENOMEM;
    if (size) memcpy(copy, data, size);
    free(transport->receive_data);
    transport->receive_data = copy;
    transport->receive_size = size;
    transport->receive_is_text = is_text;
    transport->receive_ready = 1;
    return SALTS_OK;
}

static void cnet_state(void *user, cnet_connection connection,
                       cnet_connection_state state, const cnet_error *error) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)user;
    (void)connection;
    if (state == CNET_CONNECTION_CONNECTED) {
        transport->connected = 1;
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_CONNECTED, NULL);
    } else if (state == CNET_CONNECTION_FAILED) {
        transport->connected = 0;
        transport->connection_active = 0;
        transport->terminal = 1;
        transport_set_error(transport, error && error->stage ? error->stage : "CNet connect failed");
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_ERROR, transport->error_msg);
    } else if (state == CNET_CONNECTION_CLOSED) {
        transport->connected = 0;
        transport->connection_active = 0;
        transport->terminal = 1;
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_DISCONNECTED, NULL);
    }
}

static void cnet_receive_cb(void *user, cnet_connection connection,
                            const cnet_receive_view *view) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)user;
    (void)connection;
    if (view && copy_receive(transport, view->data, view->size, 0) == SALTS_OK)
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_DATA_RECEIVED,
                             transport->receive_data);
}

static void cnet_send_cb(void *user, cnet_connection connection, size_t size) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)user;
    (void)connection;
    (void)size;
    transport->send_pending = 0;
    transport->send_completed = 1;
}

static void datagram_receive_cb(void *user, cnet_datagram *datagram,
                                const cnet_datagram_peer *peer,
                                const cnet_receive_view *view) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)user;
    (void)datagram; (void)peer;
    if (view && copy_receive(transport, view->data, view->size, 0) == SALTS_OK)
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_DATA_RECEIVED,
                             transport->receive_data);
}

static int resolve_datagram_peer(const char *host, uint16_t port,
                                 cnet_datagram_peer *out_peer) {
    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;
    char service[16];
    int status = SALTS_ENOTSUP;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (!host || getaddrinfo(host, service, &hints, &result) != 0 || !result)
        return SALTS_ENOENT;
    memset(out_peer, 0, sizeof(*out_peer));
    if (result->ai_family == AF_INET) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)result->ai_addr;
        out_peer->family = CNET_DATAGRAM_ADDRESS_IPV4;
        out_peer->port = ntohs(address->sin_port);
        memcpy(out_peer->address, &address->sin_addr, sizeof(address->sin_addr));
        status = SALTS_OK;
    } else if (result->ai_family == AF_INET6) {
        const struct sockaddr_in6 *address = (const struct sockaddr_in6 *)result->ai_addr;
        out_peer->family = CNET_DATAGRAM_ADDRESS_IPV6;
        out_peer->port = ntohs(address->sin6_port);
        out_peer->scope_id = address->sin6_scope_id;
        memcpy(out_peer->address, &address->sin6_addr, sizeof(address->sin6_addr));
        status = SALTS_OK;
    }
    freeaddrinfo(result);
    return status;
}

turbo_transport_t *turbo_transport_create(const turbo_transport_config_t *config) {
    turbo_transport_impl_t *transport;
    cnet_client_config client_config;
    cnet_tls_client_config tls_config = {0};
    if (!config || !config->host || !config->host[0]) return NULL;
    if (config->type == TURBO_TRANSPORT_HTTP)
        return turbo_transport_create_http(config);
    transport = (turbo_transport_impl_t *)calloc(1, sizeof(*transport));
    if (!transport) return NULL;
    transport->base.config = *config;
    transport->base.config.host = transport_dup(config->host);
    transport->base.config.path = transport_dup(config->path);
    if (!transport->base.config.host ||
        (config->path && !transport->base.config.path)) goto fail;
    if (config->type == TURBO_TRANSPORT_WEBSOCKET) {
        if (config->use_tls) {
            if (config->tls) {
                tls_config = *config->tls;
            } else {
                tls_config.size = sizeof(tls_config);
                tls_config.ca_file = config->ca_cert_path;
            }
            if (!tls_config.server_name || !tls_config.server_name[0])
                tls_config.server_name = transport->base.config.host;
            if (tls_config.size != sizeof(tls_config) ||
                chttp_tls_profile_init(&transport->websocket_tls,
                                       &tls_config) != SALTS_OK) goto fail;
            transport->websocket_tls_initialized = 1;
        } else if (config->tls ||
                   (config->ca_cert_path && config->ca_cert_path[0])) {
            goto fail;
        }
    }
    if (config->type == TURBO_TRANSPORT_UDP) {
        cnet_datagram_config datagram_config = CNET_DATAGRAM_CONFIG_INIT;
        datagram_config.backend = transport_backend();
        datagram_config.host = strchr(config->host, ':') ? "::" : "0.0.0.0";
        datagram_config.send_capacity = TRANSPORT_QUEUE_CAPACITY;
        datagram_config.request_capacity = TRANSPORT_QUEUE_CAPACITY + 1;
        datagram_config.completion_batch_capacity = TRANSPORT_QUEUE_CAPACITY;
        datagram_config.max_datagram_bytes = CNET_DATAGRAM_MAX_PAYLOAD_BYTES;
        datagram_config.receive_buffer_bytes = CNET_DATAGRAM_MAX_PAYLOAD_BYTES;
        datagram_config.observer.on_receive = datagram_receive_cb;
        datagram_config.observer.user = transport;
        if (resolve_datagram_peer(config->host, (uint16_t)config->port,
                                  &transport->datagram_peer) != SALTS_OK ||
            cnet_datagram_init(&transport->datagram, &datagram_config) != SALTS_OK) goto fail;
        transport->datagram_initialized = 1;
    } else if (config->type != TURBO_TRANSPORT_WEBSOCKET) {
        if (config->cnet_client) {
            transport->client = config->cnet_client;
        } else {
            client_config = transport_client_config(transport);
            if (cnet_client_init(&transport->owned_client, &client_config) != SALTS_OK) goto fail;
            transport->client = &transport->owned_client;
            transport->owns_client = 1;
        }
    }
    return (turbo_transport_t *)transport;
fail:
    if (transport->websocket_tls_initialized)
        (void)chttp_tls_profile_destroy(&transport->websocket_tls);
    free((void *)transport->base.config.host);
    free((void *)transport->base.config.path);
    free(transport);
    return NULL;
}

static int connect_websocket(turbo_transport_impl_t *transport) {
    chttp_websocket_client_config config = {0};
    chttp_websocket_connect_options options = {0};
    char uri[1024];
    unsigned int status = 0;
    int init_status;
    int uri_size;
    if (transport->websocket_initialized) {
        int destroy_status = chttp_websocket_client_destroy(
            &transport->websocket, TRANSPORT_STOP_TIMEOUT_MS);
        if (destroy_status != SALTS_OK) {
            snprintf(transport->error_msg, sizeof(transport->error_msg),
                     "CHTTP WebSocket cleanup failed: status=%d",
                     destroy_status);
            return -1;
        }
        memset(&transport->websocket, 0, sizeof(transport->websocket));
        transport->websocket_initialized = 0;
    }
    config.size = sizeof(config);
    config.network = transport_client_config(transport);
    config.max_frame_bytes = TRANSPORT_WS_FRAME_BYTES;
    config.max_message_bytes = TRANSPORT_MAX_BYTES;
    config.max_buffered_input_bytes =
        TRANSPORT_WS_FRAME_BYTES + TRANSPORT_WS_FRAME_HEADER_BYTES;
    config.max_handshake_header_bytes = 32 * 1024;
    init_status = chttp_websocket_client_init(&transport->websocket, &config);
    if (init_status != SALTS_OK) {
        snprintf(transport->error_msg, sizeof(transport->error_msg),
                 "CHTTP WebSocket init failed: status=%d", init_status);
        return -1;
    }
    transport->websocket_initialized = 1;
    uri_size = snprintf(uri, sizeof(uri), strchr(transport->base.config.host, ':')
                            ? "%s://[%s]:%d%s" : "%s://%s:%d%s",
                        transport->base.config.use_tls ? "wss" : "ws",
                        transport->base.config.host, transport->base.config.port,
                        transport->base.config.path ? transport->base.config.path : "/");
    if (uri_size < 0 || (size_t)uri_size >= sizeof(uri)) {
        transport_set_error(transport, "WebSocket URI exceeds transport limit");
        return -1;
    }
    options.size = sizeof(options);
    options.uri = uri;
    options.tls = transport->websocket_tls_initialized
                      ? &transport->websocket_tls : NULL;
    options.timeout_ms = transport_timeout(transport);
    options.protocol = CHTTP_HTTP_1_1;
    {
        int connect_status = chttp_websocket_client_connect(
            &transport->websocket, &options, &status);
        if (connect_status != SALTS_OK) {
            snprintf(transport->error_msg, sizeof(transport->error_msg),
                     "CHTTP WebSocket connect failed: status=%d http=%u",
                     connect_status, status);
            return -1;
        }
    }
    transport->connected = 1;
    transport_fire_event(transport, TURBO_TRANSPORT_EVENT_CONNECTED, NULL);
    return 0;
}

int turbo_transport_connect(turbo_transport_t *transport_ptr) {
    turbo_transport_impl_t *transport;
    size_t events = 0;
    char uri[1024];
    cnet_connect_options options = {0};
    int uri_size;
    if (!transport_ptr) return -1;
    if (turbo_transport_base(transport_ptr)->config.type == TURBO_TRANSPORT_HTTP)
        return turbo_transport_connect_http(transport_ptr);
    transport = (turbo_transport_impl_t *)transport_ptr;
    if (transport->connected) return 0;
    if (transport->connection_active) return -1;
    transport->terminal = 0;
    transport->receive_ready = 0;
    transport->send_pending = 0;
    transport->send_completed = 0;
    if (transport->base.config.type == TURBO_TRANSPORT_WEBSOCKET) return connect_websocket(transport);
    if (transport->base.config.type == TURBO_TRANSPORT_UDP) {
        transport->connected = 1;
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_CONNECTED, NULL);
        return 0;
    }
    uri_size = snprintf(
        uri, sizeof(uri), strchr(transport->base.config.host, ':')
                               ? "%s://[%s]:%d" : "%s://%s:%d",
        transport->base.config.type == TURBO_TRANSPORT_TLS ? "tls" : "tcp",
        transport->base.config.host, transport->base.config.port);
    if (uri_size < 0 || (size_t)uri_size >= sizeof(uri)) {
        transport_set_error(transport, "CNet URI exceeds transport limit");
        return -1;
    }
    options.uri = uri;
    options.observer = (cnet_observer){cnet_state, cnet_receive_cb, transport, cnet_send_cb};
    options.tls = transport->base.config.type == TURBO_TRANSPORT_TLS
                      ? transport->base.config.tls
                      : NULL;
    if (cnet_connect(transport->client, &options, &transport->connection) != SALTS_OK)
        return -1;
    transport->connection_active = 1;
    while (!transport->connected && !transport->terminal) {
        if (cnet_client_poll(transport->client, transport_timeout(transport), &events) != SALTS_OK)
            break;
    }
    return transport->connected ? 0 : -1;
}

int turbo_transport_disconnect(turbo_transport_t *transport_ptr) {
    turbo_transport_impl_t *transport;
    int status;
    if (!transport_ptr) return -1;
    if (turbo_transport_base(transport_ptr)->config.type == TURBO_TRANSPORT_HTTP)
        return turbo_transport_disconnect_http(transport_ptr);
    transport = (turbo_transport_impl_t *)transport_ptr;
    if (!transport->connected && !transport->connection_active) return 0;
    if (transport->websocket_initialized) {
        (void)chttp_websocket_client_close(
            &transport->websocket, 1000, NULL, 0, TRANSPORT_STOP_TIMEOUT_MS);
        status = chttp_websocket_client_destroy(
            &transport->websocket, TRANSPORT_STOP_TIMEOUT_MS);
        if (status != SALTS_OK) return -1;
        memset(&transport->websocket, 0, sizeof(transport->websocket));
        transport->websocket_initialized = 0;
        transport->connected = 0;
        transport->terminal = 1;
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_DISCONNECTED,
                             NULL);
    } else if (transport->client) {
        uint64_t start_ms = salts_monotonic_ms();
        uint64_t deadline_ms = start_ms + TRANSPORT_STOP_TIMEOUT_MS;
        status = cnet_close(transport->client, transport->connection);
        if (status != SALTS_OK && status != SALTS_EALREADY) return -1;
        while (!transport->terminal) {
            uint64_t now_ms = salts_monotonic_ms();
            uint32_t remaining_ms;
            size_t events = 0u;
            if (now_ms >= deadline_ms) return -1;
            remaining_ms = (uint32_t)(deadline_ms - now_ms);
            status = cnet_client_poll(transport->client, remaining_ms, &events);
            if (status != SALTS_OK) return -1;
        }
    } else {
        transport->connected = 0;
        transport->terminal = 1;
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_DISCONNECTED,
                             NULL);
    }
    return 0;
}

int turbo_transport_send(turbo_transport_t *transport_ptr, const uint8_t *data, size_t size) {
    turbo_transport_impl_t *transport;
    size_t events = 0;
    int status;
    if (!transport_ptr ||
        turbo_transport_base(transport_ptr)->config.type == TURBO_TRANSPORT_HTTP)
        return -1;
    transport = (turbo_transport_impl_t *)transport_ptr;
    if (!transport->connected || !data || size == 0) return -1;
    if (transport->websocket_initialized)
        status = chttp_websocket_client_send_binary(&transport->websocket, data, size,
                                                    transport_timeout(transport));
    else if (transport->datagram_initialized)
        status = cnet_datagram_send(&transport->datagram, &transport->datagram_peer,
                                    data, size, 0);
    else {
        transport->send_pending = 1;
        transport->send_completed = 0;
        status = cnet_send(transport->client, transport->connection, data, size);
        if (status != SALTS_OK) transport->send_pending = 0;
        while (status == SALTS_OK && transport->send_pending && !transport->terminal)
            status = cnet_client_poll(
                transport->client, transport_timeout(transport), &events);
        if (!transport->send_completed) status = SALTS_EIO;
    }
    return status == SALTS_OK ? (int)size : -1;
}

int turbo_transport_recv(turbo_transport_t *transport_ptr, uint8_t **data, size_t *size) {
    turbo_transport_impl_t *transport;
    size_t events = 0;
    int status;
    if (!transport_ptr ||
        turbo_transport_base(transport_ptr)->config.type == TURBO_TRANSPORT_HTTP)
        return -1;
    transport = (turbo_transport_impl_t *)transport_ptr;
    if (!transport->connected || !data || !size) return -1;
    transport->receive_ready = 0;
    if (transport->datagram_initialized) status = cnet_datagram_receive(&transport->datagram, 1);
    else status = cnet_receive(transport->client, transport->connection, 1);
    if (status != SALTS_OK) return -1;
    while (!transport->receive_ready && transport->connected) {
        status = transport->datagram_initialized
            ? cnet_datagram_poll(&transport->datagram, transport_timeout(transport), &events)
            : cnet_client_poll(transport->client, transport_timeout(transport), &events);
        if (status != SALTS_OK) return -1;
    }
    if (!transport->receive_ready) return 0;
    *data = transport->receive_data;
    *size = transport->receive_size;
    transport->receive_data = NULL;
    transport->receive_size = 0;
    return (int)*size;
}

void turbo_transport_free_recv(turbo_transport_t *transport, uint8_t *data) {
    (void)transport;
    free(data);
}

int turbo_transport_is_connected(turbo_transport_t *transport) {
    turbo_transport_impl_t *impl;
    if (!transport) return 0;
    if (turbo_transport_base(transport)->config.type == TURBO_TRANSPORT_HTTP)
        return turbo_transport_is_connected_http(transport);
    impl = (turbo_transport_impl_t *)transport;
    return impl->connected;
}

void turbo_transport_set_event_callback(turbo_transport_t *transport,
                                        turbo_transport_event_cb callback, void *user_data) {
    turbo_transport_impl_t *impl;
    if (!transport) return;
    if (turbo_transport_base(transport)->config.type == TURBO_TRANSPORT_HTTP) {
        turbo_transport_set_event_callback_http(transport, callback, user_data);
        return;
    }
    impl = (turbo_transport_impl_t *)transport;
    impl->event_callback = callback;
    impl->event_user_data = user_data;
}

int turbo_transport_get_connection(turbo_transport_t *transport,
                                   cnet_connection *connection) {
    turbo_transport_impl_t *impl;
    if (!transport || !connection ||
        turbo_transport_base(transport)->config.type == TURBO_TRANSPORT_HTTP)
        return -1;
    impl = (turbo_transport_impl_t *)transport;
    if (!impl->client) return -1;
    *connection = impl->connection;
    return 0;
}

const char *turbo_transport_get_error(turbo_transport_t *transport) {
    turbo_transport_impl_t *impl;
    if (!transport) return NULL;
    if (turbo_transport_base(transport)->config.type == TURBO_TRANSPORT_HTTP)
        return turbo_transport_get_error_http(transport);
    impl = (turbo_transport_impl_t *)transport;
    return impl->error_msg[0] ? impl->error_msg : NULL;
}

int turbo_transport_ws_send_text(turbo_transport_t *transport_ptr, const char *text) {
    turbo_transport_impl_t *transport;
    if (!transport_ptr ||
        turbo_transport_base(transport_ptr)->config.type == TURBO_TRANSPORT_HTTP)
        return -1;
    transport = (turbo_transport_impl_t *)transport_ptr;
    if (!transport->connected || !text || !transport->websocket_initialized)
        return -1;
    return chttp_websocket_client_send_text(&transport->websocket, text, strlen(text),
                                            transport_timeout(transport)) == SALTS_OK
        ? (int)strlen(text) : -1;
}

int turbo_transport_ws_send_binary(turbo_transport_t *transport,
                                   const uint8_t *data, size_t size) {
    return turbo_transport_send(transport, data, size);
}

int turbo_transport_ws_recv(turbo_transport_t *transport_ptr, uint8_t **data,
                            size_t *size, int *is_text) {
    turbo_transport_impl_t *transport;
    chttp_websocket_event event = {0};
    if (!transport_ptr ||
        turbo_transport_base(transport_ptr)->config.type == TURBO_TRANSPORT_HTTP)
        return -1;
    transport = (turbo_transport_impl_t *)transport_ptr;
    if (!data || !size || !is_text || !transport->websocket_initialized) return -1;
    if (chttp_websocket_client_receive(&transport->websocket,
                                       transport_timeout(transport), &event) != SALTS_OK) return -1;
    if (event.kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
        transport->connected = 0;
        return 0;
    }
    if (event.kind != CHTTP_WEBSOCKET_EVENT_MESSAGE ||
        copy_receive(transport, event.data, event.size,
                     event.message_type == CHTTP_WEBSOCKET_MESSAGE_TEXT) != SALTS_OK) return -1;
    *data = transport->receive_data;
    *size = transport->receive_size;
    *is_text = transport->receive_is_text;
    transport->receive_data = NULL;
    transport->receive_size = 0;
    return (int)*size;
}

int turbo_transport_destroy(turbo_transport_t *transport_ptr) {
    turbo_transport_impl_t *transport;
    int stop_status;
    if (!transport_ptr) return 0;
    if (turbo_transport_base(transport_ptr)->config.type == TURBO_TRANSPORT_HTTP) {
        return turbo_transport_destroy_http(transport_ptr);
    }
    transport = (turbo_transport_impl_t *)transport_ptr;
    if ((transport->connected || transport->connection_active) &&
        turbo_transport_disconnect(transport_ptr) != 0 && transport->client &&
        !transport->owns_client) {
        /* The external CNet owner still retains this observer. Suppress every
           user callback before retaining the wrapper for owner-side retry. */
        transport->event_callback = NULL;
        transport->event_user_data = NULL;
        return -1;
    }
    if (transport->websocket_initialized) {
        if (chttp_websocket_client_destroy(&transport->websocket,
                                           TRANSPORT_STOP_TIMEOUT_MS) != SALTS_OK)
            return -1;
        memset(&transport->websocket, 0, sizeof(transport->websocket));
        transport->websocket_initialized = 0;
    }
    if (transport->websocket_tls_initialized) {
        if (chttp_tls_profile_destroy(&transport->websocket_tls) != SALTS_OK)
            return -1;
        memset(&transport->websocket_tls, 0, sizeof(transport->websocket_tls));
        transport->websocket_tls_initialized = 0;
    }
    if (transport->datagram_initialized) {
        if (cnet_datagram_stop(&transport->datagram,
                               TRANSPORT_STOP_TIMEOUT_MS) != SALTS_OK ||
            cnet_datagram_destroy(&transport->datagram) != SALTS_OK)
            return -1;
        memset(&transport->datagram, 0, sizeof(transport->datagram));
        transport->datagram_initialized = 0;
    }
    if (transport->owns_client) {
        stop_status = cnet_client_stop(&transport->owned_client,
                                       TRANSPORT_STOP_TIMEOUT_MS);
        if (stop_status == SALTS_ETIMEDOUT ||
            cnet_client_destroy(&transport->owned_client) != SALTS_OK)
            return -1;
        memset(&transport->owned_client, 0, sizeof(transport->owned_client));
        transport->client = NULL;
        transport->owns_client = 0;
    }
    free(transport->receive_data);
    free((void *)transport->base.config.host);
    free((void *)transport->base.config.path);
    free(transport);
    return 0;
}
