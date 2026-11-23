#include "ivr_worker_http.h"

#include "threads/threads.h"

#include <chttp_app/application.h>
#include <core/error_codes.h>

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
    sync_mutex_t lock;
    sync_cond_t state_changed;
    chttp_application http;
    salts_configurator_provider_binding http_provider;
    salts_configurator_deployment http_deployment;
    chttp_controller http_controller;
    chttp_action http_actions[5];
    int state;
};

cmeta_reflect_data(ivr_worker_http_t, "turbomedia.ivr.ManagementHost",
                   cmeta_field(int, state));
cmeta_component_empty(TurboIvrManagement);

static cmeta_status ivr_worker_http_provider_create(
    void *user, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *dependencies, size_t count,
    cmeta_object_ref *out) {
    (void)data;
    (void)config;
    (void)dependencies;
    if (count != 0u) {
        return CMETA_INVALID_ARGUMENT;
    }
    return cmeta_object_borrow(out, user, cmeta_reflected_data(ivr_worker_http_t), NULL);
}

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

static int ivr_worker_http_reply(const chttp_invocation *invocation,
                                 unsigned int status,
                                 const char *content_type,
                                 const char *body) {
    return chttp_invocation_respond(invocation, status, content_type, body,
                              body ? strlen(body) : 0u);
}

CHTTP_ACTION(ivr_worker_http_live);
int ivr_worker_http_live(void *user, const chttp_invocation *invocation) {
    (void)user;
    return ivr_worker_http_reply(invocation, 200u, "application/json",
                                 "{\"live\":true}");
}

CHTTP_ACTION(ivr_worker_http_ready);
int ivr_worker_http_ready(void *user, const chttp_invocation *invocation) {
    ivr_worker_http_t *server = (ivr_worker_http_t *)user;
    ivr_worker_health_snapshot_t snapshot;
    char json[1024];
    if (!server ||
        ivr_worker_health_snapshot(server->health, &snapshot) != 0 ||
        ivr_worker_health_json(server->health, json, sizeof(json)) < 0) {
        return ivr_worker_http_reply(invocation, 500u, "application/json",
                                     "{\"ready\":false}");
    }
    return ivr_worker_http_reply(
        invocation, snapshot.ready && !snapshot.draining ? 200u : 503u,
        "application/json", json);
}

CHTTP_ACTION(ivr_worker_http_health);
int ivr_worker_http_health(void *user, const chttp_invocation *invocation) {
    ivr_worker_http_t *server = (ivr_worker_http_t *)user;
    char json[1024];
    if (!server ||
        ivr_worker_health_json(server->health, json, sizeof(json)) < 0) {
        return ivr_worker_http_reply(invocation, 500u, "application/json",
                                     "{\"ready\":false}");
    }
    return ivr_worker_http_reply(invocation, 200u, "application/json", json);
}

CHTTP_ACTION(ivr_worker_http_metrics);
int ivr_worker_http_metrics(void *user, const chttp_invocation *invocation) {
    ivr_worker_http_t *server = (ivr_worker_http_t *)user;
    ivr_worker_health_snapshot_t health;
    char text[16384];
    int length;
    if (!server || !server->metrics ||
        ivr_worker_health_snapshot(server->health, &health) != 0) {
        return ivr_worker_http_reply(invocation, 500u, "text/plain",
                                     "ivr worker metrics unavailable\n");
    }
    length = ivr_worker_metrics_render(server->metrics, &health, text,
                                       sizeof(text));
    if (length < 0) {
        return ivr_worker_http_reply(invocation, 500u, "text/plain",
                                     "ivr worker metrics too large\n");
    }
    return chttp_invocation_respond(invocation, 200u, "text/plain", text,
                              (size_t)length);
}

CHTTP_ACTION(ivr_worker_http_drain);
int ivr_worker_http_drain(void *user, const chttp_invocation *invocation) {
    ivr_worker_http_t *server = (ivr_worker_http_t *)user;
    ivr_worker_http_drain_fn callback = NULL;
    void *context = NULL;
    if (server) {
        sync_mutex_lock(&server->lock);
        callback = server->drain_callback;
        context = server->drain_context;
        sync_mutex_unlock(&server->lock);
    }
    if (!callback || callback(context) != 0) {
        return ivr_worker_http_reply(invocation, 503u, "application/json",
                                     "{\"accepted\":false}");
    }
    return ivr_worker_http_reply(invocation, 202u, "application/json",
                                 "{\"accepted\":true}");
}

static int ivr_worker_http_init(ivr_worker_http_t *server, const char *host, uint16_t port) {
    chttp_application_config application = chttp_application_config_default();
    server->http_provider = (salts_configurator_provider_binding){
        sizeof(server->http_provider), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
        cmeta_component_meta(TurboIvrManagement), server, NULL,
        ivr_worker_http_provider_create, NULL, NULL};
    server->http_deployment = (salts_configurator_deployment){&server->http_provider, NULL, NULL};
    server->http_actions[0] = (chttp_action)CHTTP_GET("/live", ivr_worker_http_live);
    server->http_actions[1] = (chttp_action)CHTTP_GET("/ready", ivr_worker_http_ready);
    server->http_actions[2] = (chttp_action)CHTTP_GET("/health", ivr_worker_http_health);
    server->http_actions[3] = (chttp_action)CHTTP_GET("/metrics", ivr_worker_http_metrics);
    server->http_actions[4] = (chttp_action)CHTTP_POST("/drain", ivr_worker_http_drain);
    server->http_controller = (chttp_controller){
        .component_id = "TurboIvrManagement",
        .receiver = cmeta_reflected_data(ivr_worker_http_t),
        .actions = server->http_actions,
        .action_count = sizeof(server->http_actions) / sizeof(server->http_actions[0])};
    application.http = ivr_worker_http_config(host, port);
    application.workers = (chttp_worker_config){
        .workers = 1u,
        .capacity = IVR_WORKER_HTTP_CONNECTION_CAPACITY,
        .retained_bytes = IVR_WORKER_HTTP_BUFFER_CAPACITY_BYTES,
        .reply_bytes = IVR_WORKER_HTTP_MAX_RESPONSE_BODY_BYTES,
        .header_capacity = IVR_WORKER_HTTP_MAX_HEADER_COUNT,
        .header_bytes = IVR_WORKER_HTTP_MAX_HEADER_BYTES};
    application.components = &server->http_deployment;
    application.component_count = 1u;
    application.dependency_capacity = 1u;
    application.controllers = &server->http_controller;
    application.controller_count = 1u;
    return chttp_application_init(&server->http, &application);
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
    sync_mutex_init(&server->lock);
    sync_cond_init(&server->state_changed);
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
    sync_mutex_lock(&server->lock);
    if (server->state != IVR_WORKER_HTTP_STOPPED) {
        sync_mutex_unlock(&server->lock);
        return -1;
    }
    server->metrics = metrics;
    sync_mutex_unlock(&server->lock);
    return 0;
}

int ivr_worker_http_set_drain_handler(ivr_worker_http_t *server,
                                      ivr_worker_http_drain_fn callback,
                                      void *context) {
    if (!server || !callback) {
        return -1;
    }
    sync_mutex_lock(&server->lock);
    if (server->state != IVR_WORKER_HTTP_STOPPED) {
        sync_mutex_unlock(&server->lock);
        return -1;
    }
    server->drain_callback = callback;
    server->drain_context = context;
    sync_mutex_unlock(&server->lock);
    return 0;
}

int ivr_worker_http_start(ivr_worker_http_t *server, const char *host,
                          int port) {
    int status;
    if (!server || !host ||
        (strcmp(host, "127.0.0.1") != 0 && strcmp(host, "::1") != 0) ||
        port <= 0 || port > UINT16_MAX) {
        return -1;
    }
    sync_mutex_lock(&server->lock);
    if (server->state != IVR_WORKER_HTTP_STOPPED) {
        sync_mutex_unlock(&server->lock);
        return -1;
    }
    server->state = IVR_WORKER_HTTP_STARTING;
    sync_mutex_unlock(&server->lock);

    status = ivr_worker_http_init(server, host, (uint16_t)port);
    if (status == SALTS_OK) {
        status = chttp_application_start(&server->http);
    }
    if (status != SALTS_OK && server->http.impl) {
        (void)chttp_application_close(&server->http, 0u);
    }

    sync_mutex_lock(&server->lock);
    if (status == SALTS_OK) {
        server->state = IVR_WORKER_HTTP_RUNNING;
    } else {
        server->state = server->http.impl ? IVR_WORKER_HTTP_FAILED : IVR_WORKER_HTTP_STOPPED;
    }
    sync_cond_broadcast(&server->state_changed);
    sync_mutex_unlock(&server->lock);
    return status == SALTS_OK ? 0 : -1;
}

void ivr_worker_http_stop(ivr_worker_http_t *server) {
    if (!server) {
        return;
    }
    sync_mutex_lock(&server->lock);
    while (server->state == IVR_WORKER_HTTP_STARTING ||
           server->state == IVR_WORKER_HTTP_STOPPING) {
        sync_cond_wait(&server->state_changed, &server->lock);
    }
    if (server->state == IVR_WORKER_HTTP_STOPPED) {
        sync_mutex_unlock(&server->lock);
        return;
    }
    server->state = IVR_WORKER_HTTP_STOPPING;
    sync_mutex_unlock(&server->lock);

    (void)chttp_application_close(&server->http, 0u);

    sync_mutex_lock(&server->lock);
    server->state = server->http.impl ? IVR_WORKER_HTTP_FAILED : IVR_WORKER_HTTP_STOPPED;
    sync_cond_broadcast(&server->state_changed);
    sync_mutex_unlock(&server->lock);
}

void ivr_worker_http_destroy(ivr_worker_http_t *server) {
    if (!server) {
        return;
    }
    ivr_worker_http_stop(server);
    if (server->http.impl) {
        return;
    }
    sync_cond_destroy(&server->state_changed);
    sync_mutex_destroy(&server->lock);
    free(server);
}
