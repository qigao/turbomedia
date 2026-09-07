#include "ivr_worker_http.h"

#include "salts_thread.h"

#include <chttp/chttp.h>
#include <salts/error_codes.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    IVR_WORKER_HTTP_STOPPED = 0,
    IVR_WORKER_HTTP_STARTING,
    IVR_WORKER_HTTP_RUNNING,
    IVR_WORKER_HTTP_STOPPING,
    IVR_WORKER_HTTP_FAILED
};

enum {
    IVR_WORKER_HTTP_CONNECTION_CAPACITY = 16,
    IVR_WORKER_HTTP_COMMAND_CAPACITY = 32,
    IVR_WORKER_HTTP_REQUEST_CAPACITY = 32,
    IVR_WORKER_HTTP_COMPLETION_CAPACITY = 16,
    IVR_WORKER_HTTP_EVENT_CAPACITY = 32,
    IVR_WORKER_HTTP_BACKLOG = 16,
    IVR_WORKER_HTTP_ROUTE_CAPACITY = 8,
    IVR_WORKER_HTTP_MAX_TARGET_BYTES = 4096,
    IVR_WORKER_HTTP_MAX_HEADER_COUNT = 32,
    IVR_WORKER_HTTP_MAX_HEADER_BYTES = 16 * 1024,
    IVR_WORKER_HTTP_MAX_REQUEST_BODY_BYTES = 4096,
    IVR_WORKER_HTTP_MAX_RESPONSE_BODY_BYTES = 32 * 1024,
    IVR_WORKER_HTTP_MAX_SEND_BYTES = 48 * 1024,
    IVR_WORKER_HTTP_RECEIVE_BUFFER_BYTES = 8192,
    IVR_WORKER_HTTP_BUFFER_CAPACITY_BYTES = 1024 * 1024,
    IVR_WORKER_HTTP_TIMEOUT_MS = 5000,
    IVR_WORKER_HTTP_POLL_SLICE_MS = 10
};

struct ivr_worker_http_s {
    ivr_worker_health_t *health;
    ivr_worker_metrics_t *metrics;
    ivr_worker_http_drain_fn drain_callback;
    void *drain_context;
    salts_mutex_t lock;
    chttp_server http;
    int http_initialized;
    int state;
};

static native_io_backend_kind ivr_worker_http_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config ivr_worker_http_network_config(void) {
    const cnet_client_config config = {
        .backend = ivr_worker_http_backend(),
        .connection_capacity = IVR_WORKER_HTTP_CONNECTION_CAPACITY,
        .command_capacity = IVR_WORKER_HTTP_COMMAND_CAPACITY,
        .request_capacity = IVR_WORKER_HTTP_REQUEST_CAPACITY,
        .completion_batch_capacity = IVR_WORKER_HTTP_COMPLETION_CAPACITY,
        .event_capacity = IVR_WORKER_HTTP_EVENT_CAPACITY,
        .max_send_bytes = IVR_WORKER_HTTP_MAX_SEND_BYTES,
        .receive_buffer_bytes = IVR_WORKER_HTTP_RECEIVE_BUFFER_BYTES,
        .connect_timeout_ms = IVR_WORKER_HTTP_TIMEOUT_MS,
        .read_timeout_ms = IVR_WORKER_HTTP_TIMEOUT_MS,
        .write_timeout_ms = IVR_WORKER_HTTP_TIMEOUT_MS};
    return config;
}

static chttp_server_config ivr_worker_http_config(const char *host,
                                                   uint16_t port) {
    const chttp_server_config config = {
        .host = host,
        .port = port,
        .backlog = IVR_WORKER_HTTP_BACKLOG,
        .network = ivr_worker_http_network_config(),
        .route_capacity = IVR_WORKER_HTTP_ROUTE_CAPACITY,
        .max_target_bytes = IVR_WORKER_HTTP_MAX_TARGET_BYTES,
        .max_header_count = IVR_WORKER_HTTP_MAX_HEADER_COUNT,
        .max_header_bytes = IVR_WORKER_HTTP_MAX_HEADER_BYTES,
        .max_request_body_bytes = IVR_WORKER_HTTP_MAX_REQUEST_BODY_BYTES,
        .max_response_header_count = IVR_WORKER_HTTP_MAX_HEADER_COUNT,
        .max_response_header_bytes = IVR_WORKER_HTTP_MAX_HEADER_BYTES,
        .max_response_body_bytes = IVR_WORKER_HTTP_MAX_RESPONSE_BODY_BYTES,
        .poll_slice_ms = IVR_WORKER_HTTP_POLL_SLICE_MS,
        .buffer_capacity_bytes = IVR_WORKER_HTTP_BUFFER_CAPACITY_BYTES};
    return config;
}

static int ivr_worker_http_reply(chttp_server_response *response,
                                 unsigned int status,
                                 const char *content_type,
                                 const char *body) {
    return chttp_server_reply(response, status, content_type, body,
                              body ? strlen(body) : 0u);
}

static int handle_live(void *user,
                       const chttp_server_request_view *request,
                       chttp_server_response *response) {
    (void)user;
    (void)request;
    return ivr_worker_http_reply(response, 200u, "application/json",
                                 "{\"live\":true}");
}

static int handle_ready(void *user,
                        const chttp_server_request_view *request,
                        chttp_server_response *response) {
    ivr_worker_http_t *server = (ivr_worker_http_t *)user;
    ivr_worker_health_snapshot_t snapshot;
    char json[1024];
    (void)request;
    if (!server ||
        ivr_worker_health_snapshot(server->health, &snapshot) != 0 ||
        ivr_worker_health_json(server->health, json, sizeof(json)) < 0) {
        return ivr_worker_http_reply(response, 500u, "application/json",
                                     "{\"ready\":false}");
    }
    return ivr_worker_http_reply(
        response, snapshot.ready && !snapshot.draining ? 200u : 503u,
        "application/json", json);
}

static int handle_health(void *user,
                         const chttp_server_request_view *request,
                         chttp_server_response *response) {
    ivr_worker_http_t *server = (ivr_worker_http_t *)user;
    char json[1024];
    (void)request;
    if (!server ||
        ivr_worker_health_json(server->health, json, sizeof(json)) < 0) {
        return ivr_worker_http_reply(response, 500u, "application/json",
                                     "{\"ready\":false}");
    }
    return ivr_worker_http_reply(response, 200u, "application/json", json);
}

static int handle_metrics(void *user,
                          const chttp_server_request_view *request,
                          chttp_server_response *response) {
    ivr_worker_http_t *server = (ivr_worker_http_t *)user;
    ivr_worker_health_snapshot_t health;
    char text[16384];
    int length;
    (void)request;
    if (!server || !server->metrics ||
        ivr_worker_health_snapshot(server->health, &health) != 0) {
        return ivr_worker_http_reply(response, 500u, "text/plain",
                                     "ivr worker metrics unavailable\n");
    }
    length = ivr_worker_metrics_render(server->metrics, &health, text,
                                       sizeof(text));
    if (length < 0) {
        return ivr_worker_http_reply(response, 500u, "text/plain",
                                     "ivr worker metrics too large\n");
    }
    return chttp_server_reply(response, 200u, "text/plain", text,
                              (size_t)length);
}

static int handle_drain(void *user,
                        const chttp_server_request_view *request,
                        chttp_server_response *response) {
    ivr_worker_http_t *server = (ivr_worker_http_t *)user;
    ivr_worker_http_drain_fn callback = NULL;
    void *context = NULL;
    (void)request;
    if (server) {
        salts_mutex_lock(&server->lock);
        callback = server->drain_callback;
        context = server->drain_context;
        salts_mutex_unlock(&server->lock);
    }
    if (!callback || callback(context) != 0) {
        return ivr_worker_http_reply(response, 503u, "application/json",
                                     "{\"accepted\":false}");
    }
    return ivr_worker_http_reply(response, 202u, "application/json",
                                 "{\"accepted\":true}");
}

static int ivr_worker_http_register_routes(ivr_worker_http_t *server) {
    int status = chttp_server_get(&server->http, "/live", handle_live,
                                  server);
    if (status == SALTS_OK) {
        status = chttp_server_get(&server->http, "/ready", handle_ready,
                                  server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_get(&server->http, "/health", handle_health,
                                  server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_get(&server->http, "/metrics", handle_metrics,
                                  server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_post(&server->http, "/drain", handle_drain,
                                   server);
    }
    return status;
}

int ivr_worker_http_create(ivr_worker_health_t *health,
                           ivr_worker_http_t **out_server) {
    ivr_worker_http_t *server;
    if (!health || !out_server) {
        return -1;
    }
    *out_server = NULL;
    server = (ivr_worker_http_t *)calloc(1, sizeof(*server));
    if (!server) {
        return -1;
    }
    salts_mutex_init(&server->lock);
    server->health = health;
    server->state = IVR_WORKER_HTTP_STOPPED;
    *out_server = server;
    return 0;
}

int ivr_worker_http_set_metrics(ivr_worker_http_t *server,
                                ivr_worker_metrics_t *metrics) {
    if (!server || !metrics) {
        return -1;
    }
    salts_mutex_lock(&server->lock);
    if (server->state != IVR_WORKER_HTTP_STOPPED) {
        salts_mutex_unlock(&server->lock);
        return -1;
    }
    server->metrics = metrics;
    salts_mutex_unlock(&server->lock);
    return 0;
}

int ivr_worker_http_set_drain_handler(ivr_worker_http_t *server,
                                      ivr_worker_http_drain_fn callback,
                                      void *context) {
    if (!server || !callback) {
        return -1;
    }
    salts_mutex_lock(&server->lock);
    if (server->state != IVR_WORKER_HTTP_STOPPED) {
        salts_mutex_unlock(&server->lock);
        return -1;
    }
    server->drain_callback = callback;
    server->drain_context = context;
    salts_mutex_unlock(&server->lock);
    return 0;
}

int ivr_worker_http_start(ivr_worker_http_t *server, const char *host,
                          int port) {
    chttp_server_config config;
    int status;
    if (!server || !host ||
        (strcmp(host, "127.0.0.1") != 0 && strcmp(host, "::1") != 0) ||
        port <= 0 || port > UINT16_MAX) {
        return -1;
    }
    salts_mutex_lock(&server->lock);
    if (server->state != IVR_WORKER_HTTP_STOPPED ||
        server->http_initialized) {
        salts_mutex_unlock(&server->lock);
        return -1;
    }
    server->state = IVR_WORKER_HTTP_STARTING;
    salts_mutex_unlock(&server->lock);

    config = ivr_worker_http_config(host, (uint16_t)port);
    status = chttp_server_init(&server->http, &config);
    if (status == SALTS_OK) {
        salts_mutex_lock(&server->lock);
        server->http_initialized = 1;
        salts_mutex_unlock(&server->lock);
        status = ivr_worker_http_register_routes(server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_start(&server->http);
    }
    if (status != SALTS_OK) {
        (void)chttp_server_destroy(&server->http);
        salts_mutex_lock(&server->lock);
        server->http_initialized = 0;
        server->state = IVR_WORKER_HTTP_STOPPED;
        salts_mutex_unlock(&server->lock);
        return -1;
    }

    salts_mutex_lock(&server->lock);
    server->state = IVR_WORKER_HTTP_RUNNING;
    salts_mutex_unlock(&server->lock);
    return 0;
}

void ivr_worker_http_stop(ivr_worker_http_t *server) {
    int initialized;
    if (!server) {
        return;
    }
    salts_mutex_lock(&server->lock);
    if (server->state == IVR_WORKER_HTTP_STOPPED) {
        salts_mutex_unlock(&server->lock);
        return;
    }
    server->state = IVR_WORKER_HTTP_STOPPING;
    initialized = server->http_initialized;
    salts_mutex_unlock(&server->lock);

    if (initialized) {
        (void)chttp_server_stop(&server->http, 0u);
        (void)chttp_server_destroy(&server->http);
    }

    salts_mutex_lock(&server->lock);
    server->http_initialized = 0;
    server->state = IVR_WORKER_HTTP_STOPPED;
    salts_mutex_unlock(&server->lock);
}

void ivr_worker_http_destroy(ivr_worker_http_t *server) {
    if (!server) {
        return;
    }
    ivr_worker_http_stop(server);
    salts_mutex_destroy(&server->lock);
    free(server);
}
