#include "ivr_control_ws.h"

#include "ivr_thread.h"

#include <platform.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    IVR_CONTROL_WS_DEFAULT_QUEUE_MESSAGES = 64,
    IVR_CONTROL_WS_DEFAULT_QUEUE_BYTES = 256 * 1024,
    IVR_CONTROL_WS_DEFAULT_MESSAGE_BYTES = 64 * 1024,
    IVR_CONTROL_WS_DEFAULT_TIMEOUT_MS = 5000,
    IVR_CONTROL_WS_DEFAULT_IO_SLICE_MS = 20,
    IVR_CONTROL_WS_DEFAULT_RECONNECT_INITIAL_MS = 1000,
    IVR_CONTROL_WS_DEFAULT_RECONNECT_MAX_MS = 30000,
    IVR_CONTROL_WS_SERVER_RESTART_INITIAL_MS = 1000,
    IVR_CONTROL_WS_SERVER_RESTART_MAX_MS = 30000,
    IVR_CONTROL_WS_IDENTITY_CAPACITY = 128,
    IVR_CONTROL_WS_HANDSHAKE_HEADER_BYTES = 16 * 1024
};

typedef struct ivr_control_ws_message_s {
    uint8_t *data;
    size_t size;
} ivr_control_ws_message_t;

typedef struct ivr_control_ws_peer_entry_s {
    int active;
    char identity[IVR_CONTROL_WS_IDENTITY_CAPACITY];
    ivr_control_ws_route_t route;
} ivr_control_ws_peer_entry_t;

struct ivr_control_ws_client_s {
    char *uri;
    char identity[IVR_CONTROL_WS_IDENTITY_CAPACITY];
    chttp_tls_profile tls_profile;
    int has_tls_profile;
    chttp_websocket_client websocket;
    chttp_websocket_client_config websocket_config;
    uint32_t start_timeout_ms;
    uint32_t io_timeout_ms;
    uint32_t reconnect_initial_ms;
    uint32_t reconnect_max_ms;
    size_t maximum_message_bytes;
    ivr_control_ws_client_message_fn on_message;
    ivr_control_ws_client_connection_fn on_connection;
    void *callback_context;
    ivr_thread_t thread;
    ivr_mutex_t lock;
    ivr_cond_t changed;
    ivr_control_ws_message_t *queue;
    size_t queue_capacity;
    size_t queue_bytes_capacity;
    size_t queue_bytes;
    size_t queue_head;
    size_t queue_count;
    int start_done;
    ivr_status_t start_status;
    atomic_int started;
    atomic_int accepting;
    atomic_int stop_requested;
    atomic_int connected;
};

struct ivr_control_ws_server_s {
    chttp_server server;
    char *host;
    char *path;
    uint16_t port;
    cnet_tls_server_config tls;
    int has_tls;
    char *tls_cert_file;
    char *tls_key_file;
    char *tls_key_password;
    char *tls_ca_file;
    char *tls_ca_path;
    size_t maximum_message_bytes;
    uint32_t shutdown_timeout_ms;
    ivr_control_ws_verify_identity_fn verify_identity;
    void *verify_identity_context;
    ivr_control_ws_peer_fn on_peer;
    ivr_control_ws_server_message_fn on_message;
    void *callback_context;
    ivr_mutex_t peers_lock;
    ivr_control_ws_peer_entry_t *peers;
    size_t peer_capacity;
    uint64_t next_restart_ms;
    uint32_t restart_delay_ms;
    atomic_int started;
};

static int ivr_control_ws_server_open(
    void *context, chttp_websocket *websocket,
    const chttp_server_request_view *request,
    chttp_server_response *response);
static void ivr_control_ws_server_event(
    void *context, chttp_websocket *websocket,
    const chttp_websocket_event *event);

static char *ivr_control_ws_copy_string(const char *value) {
    size_t size;
    char *copy;
    if (!value) {
        return NULL;
    }
    size = strlen(value);
    if (size == SIZE_MAX) {
        return NULL;
    }
    copy = (char *)malloc(size + 1u);
    if (copy) {
        memcpy(copy, value, size + 1u);
    }
    return copy;
}

static native_io_backend_kind ivr_control_ws_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static size_t ivr_control_ws_wire_capacity(size_t message_bytes) {
    const size_t overhead = 64u;
    return message_bytes <= SIZE_MAX - overhead ? message_bytes + overhead : 0u;
}

static size_t ivr_control_ws_power_of_two_at_least(size_t value) {
    size_t capacity = 1u;
    while (capacity < value) {
        if (capacity > SIZE_MAX / 2u) {
            return 0u;
        }
        capacity *= 2u;
    }
    return capacity;
}

static ivr_status_t ivr_control_ws_status(int status) {
    if (status == SALTS_OK) {
        return IVR_OK;
    }
    if (status == SALTS_ENOBUFS || status == SALTS_ENOMEM ||
        status == SALTS_EMSGSIZE) {
        return IVR_ENOSPC;
    }
    if (status == SALTS_EINVAL || status == SALTS_ERANGE) {
        return IVR_EINVAL;
    }
    return IVR_ESTATE;
}

static int ivr_control_ws_route_equal(const ivr_control_ws_route_t *left,
                                      const ivr_control_ws_route_t *right) {
    return left && right && left->session.impl == right->session.impl &&
           left->session.connection_slot == right->session.connection_slot &&
           left->session.connection_generation ==
               right->session.connection_generation &&
           left->session.stream_id == right->session.stream_id;
}

static int ivr_control_ws_server_init_runtime(
    ivr_control_ws_server_t *server) {
    chttp_server_config config;
    chttp_server_websocket_options route;
    size_t wire_capacity =
        ivr_control_ws_wire_capacity(server->maximum_message_bytes);
    int status;
    memset(&config, 0, sizeof(config));
    config.host = server->host;
    config.port = server->port;
    config.backlog = server->peer_capacity;
    config.network.backend = ivr_control_ws_backend();
    config.network.connection_capacity = server->peer_capacity;
    config.network.command_capacity = ivr_control_ws_power_of_two_at_least(
        server->peer_capacity * 4u + 16u);
    config.network.request_capacity = server->peer_capacity * 2u + 8u;
    config.network.completion_batch_capacity = server->peer_capacity + 8u;
    config.network.event_capacity = ivr_control_ws_power_of_two_at_least(
        server->peer_capacity * 4u + 16u);
    config.network.max_send_bytes = wire_capacity;
    config.network.receive_buffer_bytes = wire_capacity;
    config.network.connect_timeout_ms = server->shutdown_timeout_ms;
    config.network.read_timeout_ms = server->shutdown_timeout_ms;
    config.network.write_timeout_ms = server->shutdown_timeout_ms;
    if (server->has_tls) {
        config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
        config.network.tls_handshake_timeout_ms = server->shutdown_timeout_ms;
    }
    config.route_capacity = 1u;
    config.middleware_capacity = 1u;
    config.max_route_middleware_count = 1u;
    config.max_route_param_count = 1u;
    config.max_route_param_bytes = 128u;
    config.max_target_bytes = 512u;
    config.max_header_count = 32u;
    config.max_header_bytes = IVR_CONTROL_WS_HANDSHAKE_HEADER_BYTES;
    config.max_request_body_bytes = 1u;
    config.max_response_header_count = 16u;
    config.max_response_header_bytes = 1024u;
    config.max_response_body_bytes = 1u;
    config.poll_slice_ms = IVR_CONTROL_WS_DEFAULT_IO_SLICE_MS;
    config.tls = server->has_tls ? &server->tls : NULL;
    config.enable_http2 = 0;
    status = chttp_server_init(&server->server, &config);
    if (status != SALTS_OK) {
        fprintf(stderr,
                "IVR control WebSocket server init failed: stage=chttp_init "
                "status=%d tls=%d\n",
                status, server->has_tls);
        return status;
    }
    memset(&route, 0, sizeof(route));
    route.size = sizeof(route);
    route.path = server->path;
    route.max_frame_bytes = server->maximum_message_bytes;
    route.max_message_bytes = server->maximum_message_bytes;
    route.max_buffered_input_bytes = wire_capacity;
    route.on_open = ivr_control_ws_server_open;
    route.on_event = ivr_control_ws_server_event;
    route.user = server;
    status = chttp_server_websocket_with(&server->server, &route);
    if (status != SALTS_OK) {
        fprintf(stderr,
                "IVR control WebSocket server init failed: "
                "stage=websocket_route status=%d\n",
                status);
        (void)chttp_server_destroy(&server->server);
        memset(&server->server, 0, sizeof(server->server));
    }
    return status;
}

static void ivr_control_ws_server_clear_peers(
    ivr_control_ws_server_t *server) {
    ivr_mutex_lock(&server->peers_lock);
    for (size_t i = 0; i < server->peer_capacity; ++i) {
        server->peers[i].active = 0;
        server->peers[i].identity[0] = '\0';
        memset(&server->peers[i].route, 0,
               sizeof(server->peers[i].route));
    }
    ivr_mutex_unlock(&server->peers_lock);
}

void ivr_control_ws_client_config_init(
    ivr_control_ws_client_config_t *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->maximum_queue_messages =
        IVR_CONTROL_WS_DEFAULT_QUEUE_MESSAGES;
    config->maximum_queue_bytes = IVR_CONTROL_WS_DEFAULT_QUEUE_BYTES;
    config->maximum_message_bytes = IVR_CONTROL_WS_DEFAULT_MESSAGE_BYTES;
    config->start_timeout_ms = IVR_CONTROL_WS_DEFAULT_TIMEOUT_MS;
    config->io_timeout_ms = IVR_CONTROL_WS_DEFAULT_IO_SLICE_MS;
    config->reconnect_initial_ms =
        IVR_CONTROL_WS_DEFAULT_RECONNECT_INITIAL_MS;
    config->reconnect_max_ms = IVR_CONTROL_WS_DEFAULT_RECONNECT_MAX_MS;
}

static void ivr_control_ws_queue_clear(ivr_control_ws_client_t *client) {
    if (!client || !client->queue) {
        return;
    }
    for (size_t i = 0; i < client->queue_capacity; ++i) {
        free(client->queue[i].data);
        client->queue[i].data = NULL;
        client->queue[i].size = 0u;
    }
    client->queue_head = 0u;
    client->queue_count = 0u;
    client->queue_bytes = 0u;
}

static ivr_control_ws_message_t *
ivr_control_ws_queue_front(ivr_control_ws_client_t *client) {
    return client->queue_count ? &client->queue[client->queue_head] : NULL;
}

static void ivr_control_ws_queue_pop(ivr_control_ws_client_t *client) {
    ivr_control_ws_message_t *item =
        ivr_control_ws_queue_front(client);
    if (!item) {
        return;
    }
    client->queue_bytes -= item->size;
    free(item->data);
    item->data = NULL;
    item->size = 0u;
    client->queue_head = (client->queue_head + 1u) % client->queue_capacity;
    --client->queue_count;
}

static void ivr_control_ws_client_signal_start(
    ivr_control_ws_client_t *client, ivr_status_t status) {
    ivr_mutex_lock(&client->lock);
    if (!client->start_done) {
        client->start_status = status;
        client->start_done = 1;
        ivr_cond_broadcast(&client->changed);
    }
    ivr_mutex_unlock(&client->lock);
}

static int ivr_control_ws_client_connect(ivr_control_ws_client_t *client) {
    chttp_websocket_connect_options options;
    chttp_header identity_header;
    unsigned int http_status = 0u;
    int status;

    memset(&options, 0, sizeof(options));
    memset(&identity_header, 0, sizeof(identity_header));
    identity_header.name = IVR_CONTROL_WS_IDENTITY_HEADER;
    identity_header.value = client->identity;
    options.size = sizeof(options);
    options.uri = client->uri;
    options.headers = &identity_header;
    options.header_count = 1u;
    options.tls = client->has_tls_profile ? &client->tls_profile : NULL;
    options.timeout_ms = client->start_timeout_ms;
    options.protocol = CHTTP_HTTP_1_1;
    options.subprotocol = IVR_CONTROL_WS_SUBPROTOCOL;

    memset(&client->websocket, 0, sizeof(client->websocket));
    status = chttp_websocket_client_init(&client->websocket,
                                         &client->websocket_config);
    if (status != SALTS_OK) {
        fprintf(stderr,
                "IVR control WebSocket client failed: stage=chttp_init "
                "status=%d\n",
                status);
        return status;
    }
    status = chttp_websocket_client_connect(&client->websocket, &options,
                                            &http_status);
    if (status != SALTS_OK || http_status != 101u) {
        fprintf(stderr,
                "IVR control WebSocket client failed: stage=connect "
                "status=%d http_status=%u\n",
                status, http_status);
        (void)chttp_websocket_client_destroy(&client->websocket,
                                             client->start_timeout_ms);
        memset(&client->websocket, 0, sizeof(client->websocket));
        return status != SALTS_OK ? status : SALTS_EPROTO;
    }
    return SALTS_OK;
}

static void ivr_control_ws_client_disconnect(
    ivr_control_ws_client_t *client) {
    if (atomic_exchange_explicit(&client->connected, 0,
                                 memory_order_acq_rel)) {
        if (client->on_connection) {
            client->on_connection(client->callback_context, 0);
        }
    }
    if (client->websocket.impl) {
        (void)chttp_websocket_client_close(
            &client->websocket, 1000u, NULL, 0u, client->io_timeout_ms);
        (void)chttp_websocket_client_destroy(&client->websocket,
                                             client->start_timeout_ms);
        memset(&client->websocket, 0, sizeof(client->websocket));
    }
}

static int ivr_control_ws_client_send_front(
    ivr_control_ws_client_t *client) {
    ivr_control_ws_message_t item;
    int status;
    memset(&item, 0, sizeof(item));
    ivr_mutex_lock(&client->lock);
    if (client->queue_count) {
        item = *ivr_control_ws_queue_front(client);
    }
    ivr_mutex_unlock(&client->lock);
    if (!item.data) {
        return SALTS_OK;
    }
    status = chttp_websocket_client_send_binary(
        &client->websocket, item.data, item.size, client->start_timeout_ms);
    if (status == SALTS_OK) {
        ivr_mutex_lock(&client->lock);
        if (client->queue_count &&
            ivr_control_ws_queue_front(client)->data == item.data) {
            ivr_control_ws_queue_pop(client);
        }
        ivr_mutex_unlock(&client->lock);
    }
    return status;
}

static void *ivr_control_ws_client_thread(void *opaque) {
    ivr_control_ws_client_t *client =
        (ivr_control_ws_client_t *)opaque;
    uint32_t reconnect_delay = client->reconnect_initial_ms;
    int first_attempt = 1;

    while (!atomic_load_explicit(&client->stop_requested,
                                 memory_order_acquire)) {
        int status = ivr_control_ws_client_connect(client);
        if (status != SALTS_OK) {
            if (first_attempt) {
                ivr_control_ws_client_signal_start(
                    client, ivr_control_ws_status(status));
                break;
            }
            ivr_thread_sleep_ms(reconnect_delay);
            if (reconnect_delay < client->reconnect_max_ms) {
                uint64_t next = (uint64_t)reconnect_delay * 2u;
                reconnect_delay =
                    next > client->reconnect_max_ms
                        ? client->reconnect_max_ms
                        : (uint32_t)next;
            }
            continue;
        }
        first_attempt = 0;
        reconnect_delay = client->reconnect_initial_ms;
        atomic_store_explicit(&client->connected, 1,
                              memory_order_release);
        if (client->on_connection) {
            client->on_connection(client->callback_context, 1);
        }
        ivr_control_ws_client_signal_start(client, IVR_OK);

        while (!atomic_load_explicit(&client->stop_requested,
                                     memory_order_acquire)) {
            chttp_websocket_event event;
            status = ivr_control_ws_client_send_front(client);
            if (status != SALTS_OK) {
                break;
            }
            memset(&event, 0, sizeof(event));
            status = chttp_websocket_client_receive(
                &client->websocket, client->io_timeout_ms, &event);
            if (status == SALTS_ETIMEDOUT) {
                continue;
            }
            if (status != SALTS_OK ||
                event.kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
                if (!atomic_load_explicit(&client->stop_requested,
                                          memory_order_acquire)) {
                    fprintf(stderr,
                            "IVR control WebSocket client disconnected: "
                            "identity=%s status=%d event=%d close_code=%u\n",
                            client->identity, status, (int)event.kind,
                            (unsigned int)event.close_code);
                }
                break;
            }
            if (event.kind == CHTTP_WEBSOCKET_EVENT_MESSAGE) {
                if (event.message_type != CHTTP_WEBSOCKET_MESSAGE_BINARY) {
                    status = SALTS_EPROTO;
                    break;
                }
                if (client->on_message) {
                    client->on_message(client->callback_context, event.data,
                                       event.size);
                }
            }
        }
        ivr_control_ws_client_disconnect(client);
        if (!atomic_load_explicit(&client->stop_requested,
                                  memory_order_acquire)) {
            ivr_thread_sleep_ms(reconnect_delay);
        }
    }
    ivr_control_ws_client_disconnect(client);
    ivr_control_ws_client_signal_start(client, IVR_ESTATE);
    return NULL;
}

ivr_status_t ivr_control_ws_client_create(
    const ivr_control_ws_client_config_t *config,
    ivr_control_ws_client_t **out_client) {
    ivr_control_ws_client_t *client;
    size_t wire_capacity;
    size_t identity_size;
    size_t queue_capacity;
    size_t queue_bytes_capacity;
    size_t message_capacity;

    if (out_client) {
        *out_client = NULL;
    }
    if (!config || !out_client || !config->uri || !config->identity) {
        return IVR_EINVAL;
    }
    identity_size = strlen(config->identity);
    queue_capacity = config->maximum_queue_messages;
    queue_bytes_capacity = config->maximum_queue_bytes;
    message_capacity = config->maximum_message_bytes;
    wire_capacity = ivr_control_ws_wire_capacity(message_capacity);
    if (!identity_size || identity_size >= IVR_CONTROL_WS_IDENTITY_CAPACITY ||
        !queue_capacity || !queue_bytes_capacity || !message_capacity ||
        !wire_capacity || queue_capacity > SIZE_MAX - 8u ||
        message_capacity > queue_bytes_capacity) {
        return IVR_EINVAL;
    }
    client = (ivr_control_ws_client_t *)calloc(1, sizeof(*client));
    if (!client) {
        return IVR_ENOSPC;
    }
    client->uri = ivr_control_ws_copy_string(config->uri);
    client->queue = (ivr_control_ws_message_t *)calloc(
        queue_capacity, sizeof(*client->queue));
    if (!client->uri || !client->queue || ivr_mutex_init(&client->lock) != 0 ||
        ivr_cond_init(&client->changed) != 0) {
        ivr_control_ws_client_destroy(client);
        return IVR_ENOSPC;
    }
    memcpy(client->identity, config->identity, identity_size + 1u);
    client->queue_capacity = queue_capacity;
    client->queue_bytes_capacity = queue_bytes_capacity;
    client->maximum_message_bytes = message_capacity;
    client->start_timeout_ms = config->start_timeout_ms
                                   ? config->start_timeout_ms
                                   : IVR_CONTROL_WS_DEFAULT_TIMEOUT_MS;
    client->io_timeout_ms = config->io_timeout_ms
                                ? config->io_timeout_ms
                                : IVR_CONTROL_WS_DEFAULT_IO_SLICE_MS;
    client->reconnect_initial_ms =
        config->reconnect_initial_ms
            ? config->reconnect_initial_ms
            : IVR_CONTROL_WS_DEFAULT_RECONNECT_INITIAL_MS;
    client->reconnect_max_ms = config->reconnect_max_ms
                                   ? config->reconnect_max_ms
                                   : IVR_CONTROL_WS_DEFAULT_RECONNECT_MAX_MS;
    if (client->reconnect_initial_ms > client->reconnect_max_ms) {
        ivr_control_ws_client_destroy(client);
        return IVR_EINVAL;
    }
    client->on_message = config->on_message;
    client->on_connection = config->on_connection;
    client->callback_context = config->callback_context;
    memset(&client->websocket_config, 0,
           sizeof(client->websocket_config));
    client->websocket_config.size = sizeof(client->websocket_config);
    client->websocket_config.network.backend = ivr_control_ws_backend();
    client->websocket_config.network.connection_capacity = 1u;
    client->websocket_config.network.command_capacity =
        ivr_control_ws_power_of_two_at_least(queue_capacity + 8u);
    client->websocket_config.network.request_capacity = 8u;
    client->websocket_config.network.completion_batch_capacity = 8u;
    client->websocket_config.network.event_capacity =
        ivr_control_ws_power_of_two_at_least(queue_capacity + 8u);
    client->websocket_config.network.max_send_bytes = wire_capacity;
    client->websocket_config.network.receive_buffer_bytes = wire_capacity;
    client->websocket_config.network.connect_timeout_ms =
        client->start_timeout_ms;
    client->websocket_config.network.read_timeout_ms =
        client->start_timeout_ms;
    client->websocket_config.network.write_timeout_ms =
        client->start_timeout_ms;
    if (config->tls) {
        client->websocket_config.network.tls_io_buffer_bytes =
            CNET_TLS_MIN_IO_BUFFER_BYTES;
        client->websocket_config.network.tls_handshake_timeout_ms =
            client->start_timeout_ms;
    }
    client->websocket_config.max_frame_bytes = message_capacity;
    client->websocket_config.max_message_bytes = message_capacity;
    client->websocket_config.max_buffered_input_bytes = wire_capacity;
    client->websocket_config.max_handshake_header_bytes =
        IVR_CONTROL_WS_HANDSHAKE_HEADER_BYTES;
    client->websocket_config.event_capacity =
        ivr_control_ws_power_of_two_at_least(queue_capacity + 8u);
    if (config->tls) {
        int status = chttp_tls_profile_init(&client->tls_profile,
                                            config->tls);
        if (status != SALTS_OK) {
            ivr_control_ws_client_destroy(client);
            return ivr_control_ws_status(status);
        }
        client->has_tls_profile = 1;
    }
    atomic_init(&client->started, 0);
    atomic_init(&client->accepting, 0);
    atomic_init(&client->stop_requested, 0);
    atomic_init(&client->connected, 0);
    *out_client = client;
    return IVR_OK;
}

ivr_status_t ivr_control_ws_client_start(ivr_control_ws_client_t *client) {
    ivr_status_t status;
    if (!client || atomic_exchange_explicit(&client->started, 1,
                                             memory_order_acq_rel)) {
        return IVR_EINVAL;
    }
    client->start_done = 0;
    client->start_status = IVR_ESTATE;
    atomic_store_explicit(&client->stop_requested, 0,
                          memory_order_release);
    atomic_store_explicit(&client->accepting, 1, memory_order_release);
    if (ivr_thread_create(&client->thread, ivr_control_ws_client_thread,
                          client) != 0) {
        atomic_store_explicit(&client->accepting, 0,
                              memory_order_release);
        atomic_store_explicit(&client->started, 0, memory_order_release);
        return IVR_ENOSPC;
    }
    ivr_mutex_lock(&client->lock);
    while (!client->start_done) {
        ivr_cond_wait(&client->changed, &client->lock);
    }
    status = client->start_status;
    ivr_mutex_unlock(&client->lock);
    if (status != IVR_OK) {
        ivr_control_ws_client_stop(client);
    }
    return status;
}

ivr_status_t ivr_control_ws_client_send_copy(ivr_control_ws_client_t *client,
                                              const uint8_t *data,
                                              size_t size) {
    ivr_control_ws_message_t item;
    size_t tail;
    if (!client || !data || !size || size > client->maximum_message_bytes ||
        !atomic_load_explicit(&client->accepting, memory_order_acquire)) {
        return IVR_EINVAL;
    }
    item.data = (uint8_t *)malloc(size);
    item.size = size;
    if (!item.data) {
        return IVR_ENOSPC;
    }
    memcpy(item.data, data, size);
    ivr_mutex_lock(&client->lock);
    if (!atomic_load_explicit(&client->accepting, memory_order_acquire) ||
        client->queue_count == client->queue_capacity ||
        size > client->queue_bytes_capacity - client->queue_bytes) {
        ivr_mutex_unlock(&client->lock);
        free(item.data);
        return IVR_ENOSPC;
    }
    tail = (client->queue_head + client->queue_count) %
           client->queue_capacity;
    client->queue[tail] = item;
    ++client->queue_count;
    client->queue_bytes += size;
    ivr_cond_signal(&client->changed);
    ivr_mutex_unlock(&client->lock);
    return IVR_OK;
}

void ivr_control_ws_client_stop(ivr_control_ws_client_t *client) {
    if (!client || !atomic_exchange_explicit(&client->started, 0,
                                              memory_order_acq_rel)) {
        return;
    }
    atomic_store_explicit(&client->accepting, 0, memory_order_release);
    atomic_store_explicit(&client->stop_requested, 1,
                          memory_order_release);
    ivr_mutex_lock(&client->lock);
    ivr_cond_broadcast(&client->changed);
    ivr_mutex_unlock(&client->lock);
    if (client->thread.handle) {
        (void)ivr_thread_join(&client->thread);
    }
}

void ivr_control_ws_client_destroy(ivr_control_ws_client_t *client) {
    if (!client) {
        return;
    }
    ivr_control_ws_client_stop(client);
    ivr_control_ws_queue_clear(client);
    free(client->queue);
    client->queue = NULL;
    if (client->has_tls_profile) {
        (void)chttp_tls_profile_destroy(&client->tls_profile);
        client->has_tls_profile = 0;
    }
    ivr_cond_destroy(&client->changed);
    ivr_mutex_destroy(&client->lock);
    free(client->uri);
    client->uri = NULL;
    free(client);
}

int ivr_control_ws_client_running(const ivr_control_ws_client_t *client) {
    return client && atomic_load_explicit(&client->connected,
                                           memory_order_acquire);
}

void ivr_control_ws_server_config_init(
    ivr_control_ws_server_config_t *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->host = "127.0.0.1";
    config->path = "/internal/ivr/control";
    config->maximum_connections = 64u;
    config->maximum_message_bytes = IVR_CONTROL_WS_DEFAULT_MESSAGE_BYTES;
    config->shutdown_timeout_ms = IVR_CONTROL_WS_DEFAULT_TIMEOUT_MS;
}

static ivr_control_ws_peer_entry_t *ivr_control_ws_server_find_route_locked(
    ivr_control_ws_server_t *server, const ivr_control_ws_route_t *route) {
    for (size_t i = 0; i < server->peer_capacity; ++i) {
        if (server->peers[i].active &&
            ivr_control_ws_route_equal(&server->peers[i].route, route)) {
            return &server->peers[i];
        }
    }
    return NULL;
}

static int ivr_control_ws_server_open(void *context,
                                      chttp_websocket *websocket,
                                      const chttp_server_request_view *request,
                                      chttp_server_response *response) {
    ivr_control_ws_server_t *server =
        (ivr_control_ws_server_t *)context;
    const char *identity;
    size_t identity_size;
    ivr_control_ws_route_t route;
    ivr_control_ws_peer_entry_t *slot = NULL;
    int status;

    if (!server || !websocket || !request || !response ||
        request->http_major != 1u || request->method != CHTTP_METHOD_GET) {
        return SALTS_EPROTO;
    }
    identity = chttp_server_request_header(
        request, IVR_CONTROL_WS_IDENTITY_HEADER);
    identity_size = identity ? strlen(identity) : 0u;
    if (!identity_size || identity_size >= IVR_CONTROL_WS_IDENTITY_CAPACITY) {
        return SALTS_EPERM;
    }
    if (server->verify_identity &&
        !server->verify_identity(server->verify_identity_context,
                                 request->peer_certificate_sha256,
                                 identity)) {
        return SALTS_EPERM;
    }
    status = chttp_server_response_select_websocket_subprotocol(
        response, request, IVR_CONTROL_WS_SUBPROTOCOL);
    if (status != SALTS_OK) {
        return status;
    }
    memset(&route, 0, sizeof(route));
    status = chttp_server_websocket_session_capture(websocket,
                                                    &route.session);
    if (status != SALTS_OK) {
        return status;
    }
    ivr_mutex_lock(&server->peers_lock);
    for (size_t i = 0; i < server->peer_capacity; ++i) {
        if (server->peers[i].active &&
            strcmp(server->peers[i].identity, identity) == 0) {
            slot = &server->peers[i];
            break;
        }
        if (!slot && !server->peers[i].active) {
            slot = &server->peers[i];
        }
    }
    if (!slot) {
        ivr_mutex_unlock(&server->peers_lock);
        return SALTS_ENOBUFS;
    }
    slot->active = 1;
    memcpy(slot->identity, identity, identity_size + 1u);
    slot->route = route;
    ivr_mutex_unlock(&server->peers_lock);
    if (server->on_peer) {
        server->on_peer(server->callback_context, &route, identity, 1);
    }
    return SALTS_OK;
}

static void ivr_control_ws_server_event(void *context,
                                        chttp_websocket *websocket,
                                        const chttp_websocket_event *event) {
    ivr_control_ws_server_t *server =
        (ivr_control_ws_server_t *)context;
    ivr_control_ws_route_t route;
    ivr_control_ws_peer_entry_t *peer;
    char identity[IVR_CONTROL_WS_IDENTITY_CAPACITY];
    int remove = 0;
    int found = 0;
    if (!server || !websocket || !event ||
        chttp_server_websocket_session_capture(websocket,
                                               &route.session) != SALTS_OK) {
        return;
    }
    identity[0] = '\0';
    ivr_mutex_lock(&server->peers_lock);
    peer = ivr_control_ws_server_find_route_locked(server, &route);
    if (peer) {
        memcpy(identity, peer->identity, sizeof(identity));
        found = 1;
        remove = event->kind == CHTTP_WEBSOCKET_EVENT_CLOSE;
        if (remove) {
            peer->active = 0;
            peer->identity[0] = '\0';
            memset(&peer->route, 0, sizeof(peer->route));
        }
    }
    ivr_mutex_unlock(&server->peers_lock);
    if (!found) {
        return;
    }
    if (event->kind == CHTTP_WEBSOCKET_EVENT_MESSAGE) {
        if (event->message_type != CHTTP_WEBSOCKET_MESSAGE_BINARY) {
            (void)chttp_websocket_close(websocket, 1003u, NULL, 0u);
            return;
        }
        if (server->on_message &&
            server->on_message(server->callback_context, &route, identity,
                               event->data, event->size) != IVR_OK) {
            (void)chttp_websocket_close(websocket, 1008u, NULL, 0u);
        }
    } else if (remove && server->on_peer) {
        server->on_peer(server->callback_context, &route, identity, 0);
    }
}

ivr_status_t ivr_control_ws_server_create(
    const ivr_control_ws_server_config_t *config,
    ivr_control_ws_server_t **out_server) {
    ivr_control_ws_server_t *server;
    size_t wire_capacity;
    size_t connections;
    size_t message_capacity;
    int status;
    if (out_server) {
        *out_server = NULL;
    }
    if (!config || !out_server || !config->host || !config->path ||
        config->path[0] != '/') {
        return IVR_EINVAL;
    }
    connections = config->maximum_connections;
    message_capacity = config->maximum_message_bytes;
    wire_capacity = ivr_control_ws_wire_capacity(message_capacity);
    if (!connections || !message_capacity || !wire_capacity ||
        connections > (SIZE_MAX - 16u) / 4u) {
        return IVR_EINVAL;
    }
    server = (ivr_control_ws_server_t *)calloc(1, sizeof(*server));
    if (!server) {
        return IVR_ENOSPC;
    }
    server->host = ivr_control_ws_copy_string(config->host);
    server->path = ivr_control_ws_copy_string(config->path);
    server->port = config->port;
    server->peers = (ivr_control_ws_peer_entry_t *)calloc(
        connections, sizeof(*server->peers));
    if (!server->host || !server->path || !server->peers ||
        ivr_mutex_init(&server->peers_lock) != 0) {
        ivr_control_ws_server_destroy(server);
        return IVR_ENOSPC;
    }
    server->peer_capacity = connections;
    server->maximum_message_bytes = message_capacity;
    server->shutdown_timeout_ms = config->shutdown_timeout_ms
                                      ? config->shutdown_timeout_ms
                                      : IVR_CONTROL_WS_DEFAULT_TIMEOUT_MS;
    server->verify_identity = config->verify_identity;
    server->verify_identity_context = config->verify_identity_context;
    server->on_peer = config->on_peer;
    server->on_message = config->on_message;
    server->callback_context = config->callback_context;
    if (config->tls) {
        server->tls_cert_file =
            ivr_control_ws_copy_string(config->tls->cert_file);
        server->tls_key_file =
            ivr_control_ws_copy_string(config->tls->key_file);
        server->tls_key_password =
            ivr_control_ws_copy_string(config->tls->key_password);
        server->tls_ca_file =
            ivr_control_ws_copy_string(config->tls->ca_file);
        server->tls_ca_path =
            ivr_control_ws_copy_string(config->tls->ca_path);
        if ((config->tls->cert_file && !server->tls_cert_file) ||
            (config->tls->key_file && !server->tls_key_file) ||
            (config->tls->key_password && !server->tls_key_password) ||
            (config->tls->ca_file && !server->tls_ca_file) ||
            (config->tls->ca_path && !server->tls_ca_path)) {
            ivr_control_ws_server_destroy(server);
            return IVR_ENOSPC;
        }
        memset(&server->tls, 0, sizeof(server->tls));
        server->tls.size = sizeof(server->tls);
        server->tls.cert_file = server->tls_cert_file;
        server->tls.key_file = server->tls_key_file;
        server->tls.key_password = server->tls_key_password;
        server->tls.ca_file = server->tls_ca_file;
        server->tls.ca_path = server->tls_ca_path;
        server->tls.client_auth = config->tls->client_auth;
        server->has_tls = 1;
    }
    status = ivr_control_ws_server_init_runtime(server);
    if (status != SALTS_OK) {
        ivr_control_ws_server_destroy(server);
        return ivr_control_ws_status(status);
    }
    atomic_init(&server->started, 0);
    *out_server = server;
    return IVR_OK;
}

ivr_status_t ivr_control_ws_server_start(ivr_control_ws_server_t *server) {
    int status;
    if (!server || atomic_exchange_explicit(&server->started, 1,
                                             memory_order_acq_rel)) {
        return IVR_EINVAL;
    }
    if (!server->server.impl) {
        status = ivr_control_ws_server_init_runtime(server);
        if (status != SALTS_OK) {
            atomic_store_explicit(&server->started, 0,
                                  memory_order_release);
            return ivr_control_ws_status(status);
        }
    }
    status = chttp_server_start(&server->server);
    if (status != SALTS_OK) {
        atomic_store_explicit(&server->started, 0, memory_order_release);
    } else {
        uint16_t bound_port = 0u;
        if (chttp_server_port(&server->server, &bound_port) == SALTS_OK) {
            server->port = bound_port;
        }
        server->next_restart_ms = 0u;
        server->restart_delay_ms = IVR_CONTROL_WS_SERVER_RESTART_INITIAL_MS;
    }
    return ivr_control_ws_status(status);
}

static void ivr_control_ws_server_schedule_restart(
    ivr_control_ws_server_t *server, uint64_t now_ms) {
    uint64_t next_delay;
    server->next_restart_ms = now_ms + server->restart_delay_ms;
    next_delay = (uint64_t)server->restart_delay_ms * 2u;
    server->restart_delay_ms =
        next_delay > IVR_CONTROL_WS_SERVER_RESTART_MAX_MS
            ? IVR_CONTROL_WS_SERVER_RESTART_MAX_MS
            : (uint32_t)next_delay;
}

ivr_status_t ivr_control_ws_server_maintain(
    ivr_control_ws_server_t *server, int *out_restarted) {
    chttp_server_stats stats;
    uint64_t now_ms;
    int status;
    int terminal_status = SALTS_ESHUTDOWN;
    if (out_restarted) {
        *out_restarted = 0;
    }
    if (!server || !out_restarted ||
        !atomic_load_explicit(&server->started, memory_order_acquire)) {
        return IVR_EINVAL;
    }
    memset(&stats, 0, sizeof(stats));
    status = chttp_server_get_stats(&server->server, &stats);
    if (status == SALTS_OK && stats.running) {
        return IVR_OK;
    }
    if (status == SALTS_OK) {
        terminal_status = stats.terminal_status;
    }
    now_ms = salts_monotonic_ms();
    if (server->next_restart_ms && now_ms < server->next_restart_ms) {
        return IVR_ESTATE;
    }

    fprintf(stderr,
            "IVR control WebSocket server terminal: status=%d; "
            "restarting listener\n",
            terminal_status);
    if (server->server.impl) {
        status = chttp_server_stop(&server->server,
                                   server->shutdown_timeout_ms);
        if (status == SALTS_ETIMEDOUT) {
            ivr_control_ws_server_schedule_restart(server, now_ms);
            return IVR_ESTATE;
        }
        status = chttp_server_destroy(&server->server);
        if (status != SALTS_OK) {
            ivr_control_ws_server_schedule_restart(server, now_ms);
            return IVR_ESTATE;
        }
        memset(&server->server, 0, sizeof(server->server));
    }
    ivr_control_ws_server_clear_peers(server);
    status = ivr_control_ws_server_init_runtime(server);
    if (status == SALTS_OK) {
        status = chttp_server_start(&server->server);
    }
    if (status != SALTS_OK) {
        if (server->server.impl) {
            int destroy_status =
                chttp_server_destroy(&server->server);
            if (destroy_status == SALTS_OK) {
                memset(&server->server, 0, sizeof(server->server));
            }
        }
        ivr_control_ws_server_schedule_restart(server, now_ms);
        return IVR_ESTATE;
    }
    server->next_restart_ms = 0u;
    server->restart_delay_ms = IVR_CONTROL_WS_SERVER_RESTART_INITIAL_MS;
    *out_restarted = 1;
    fprintf(stderr, "IVR control WebSocket server listener restarted\n");
    return IVR_OK;
}

void ivr_control_ws_server_stop(ivr_control_ws_server_t *server) {
    if (!server || !atomic_exchange_explicit(&server->started, 0,
                                              memory_order_acq_rel)) {
        return;
    }
    (void)chttp_server_stop(&server->server, server->shutdown_timeout_ms);
    (void)chttp_server_destroy(&server->server);
    memset(&server->server, 0, sizeof(server->server));
    ivr_control_ws_server_clear_peers(server);
}

void ivr_control_ws_server_destroy(ivr_control_ws_server_t *server) {
    if (!server) {
        return;
    }
    ivr_control_ws_server_stop(server);
    if (server->server.impl) {
        (void)chttp_server_destroy(&server->server);
        memset(&server->server, 0, sizeof(server->server));
    }
    ivr_mutex_destroy(&server->peers_lock);
    free(server->peers);
    free(server->tls_ca_path);
    free(server->tls_ca_file);
    free(server->tls_key_password);
    free(server->tls_key_file);
    free(server->tls_cert_file);
    free(server->path);
    free(server->host);
    free(server);
}

ivr_status_t ivr_control_ws_server_port(const ivr_control_ws_server_t *server,
                                         uint16_t *out_port) {
    if (!server || !out_port) {
        return IVR_EINVAL;
    }
    return ivr_control_ws_status(
        chttp_server_port((chttp_server *)&server->server, out_port));
}

ivr_status_t ivr_control_ws_server_send_copy(
    ivr_control_ws_server_t *server, const ivr_control_ws_route_t *route,
    const uint8_t *data, size_t size) {
    int active;
    int status;
    if (!server || !route || !data || !size ||
        size > server->maximum_message_bytes ||
        !atomic_load_explicit(&server->started, memory_order_acquire)) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&server->peers_lock);
    active = ivr_control_ws_server_find_route_locked(server, route) != NULL;
    ivr_mutex_unlock(&server->peers_lock);
    if (!active) {
        return IVR_ESTATE;
    }
    status = chttp_server_websocket_send_binary(&route->session, data, size);
    if (status != SALTS_OK) {
        chttp_server_stats stats;
        memset(&stats, 0, sizeof(stats));
        if (chttp_server_get_stats(&server->server, &stats) == SALTS_OK) {
            fprintf(stderr,
                    "IVR control WebSocket server send failed: status=%d "
                    "running=%d terminal_status=%d\n",
                    status, stats.running, stats.terminal_status);
        }
    }
    return ivr_control_ws_status(status);
}
