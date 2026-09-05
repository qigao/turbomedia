#include "ivr_worker_http.h"

#include "CoroNet.h"
#include "iris/iris_app.h"
#include "iris/router.h"
#include "iris/server.h"
#include "salts_thread.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IVR_WORKER_HTTP_CONTEXT_PATH "/internal/ivr-worker-http"
#define IVR_WORKER_HTTP_HOST_CAPACITY 46u

enum {
    IVR_WORKER_HTTP_STOPPED = 0,
    IVR_WORKER_HTTP_STARTING,
    IVR_WORKER_HTTP_RUNNING,
    IVR_WORKER_HTTP_STOPPING,
    IVR_WORKER_HTTP_FAILED
};

struct ivr_worker_http_s {
    iris_app_t *app;
    ivr_worker_health_t *health;
    ivr_worker_metrics_t *metrics;
    ivr_worker_http_drain_fn drain_callback;
    void *drain_context;
    salts_thread_t thread;
    int thread_started;
    salts_mutex_t lock;
    salts_cond_t cond;
    coro_context_t *context;
    coro_socket_t *listener;
    int state;
    char host[IVR_WORKER_HTTP_HOST_CAPACITY];
    int port;
};

static ivr_worker_http_t *http_from_request(Req *request) {
    return request && request->app
               ? (ivr_worker_http_t *)iris_app_lookup_rpc_context(
                     request->app, IVR_WORKER_HTTP_CONTEXT_PATH)
               : NULL;
}

static void handle_live(Req *request, Res *response) {
    (void)request;
    send_json(response, 200, "{\"live\":true}");
}

static void handle_ready(Req *request, Res *response) {
    ivr_worker_http_t *server = http_from_request(request);
    ivr_worker_health_snapshot_t snapshot;
    char json[1024];
    if (!server ||
        ivr_worker_health_snapshot(server->health, &snapshot) != 0 ||
        ivr_worker_health_json(server->health, json, sizeof(json)) < 0) {
        send_json(response, 500, "{\"ready\":false}");
        return;
    }
    send_json(response, snapshot.ready && !snapshot.draining ? 200 : 503,
              json);
}

static void handle_health(Req *request, Res *response) {
    ivr_worker_http_t *server = http_from_request(request);
    char json[1024];
    if (!server ||
        ivr_worker_health_json(server->health, json, sizeof(json)) < 0) {
        send_json(response, 500, "{\"ready\":false}");
        return;
    }
    send_json(response, 200, json);
}

static void handle_metrics(Req *request, Res *response) {
    ivr_worker_http_t *server = http_from_request(request);
    ivr_worker_health_snapshot_t health;
    char text[16384];
    int length;
    if (!server || !server->metrics ||
        ivr_worker_health_snapshot(server->health, &health) != 0) {
        send_text(response, 500, "ivr worker metrics unavailable\n");
        return;
    }
    length = ivr_worker_metrics_render(server->metrics, &health, text,
                                       sizeof(text));
    if (length < 0) {
        send_text(response, 500, "ivr worker metrics too large\n");
        return;
    }
    send_text(response, 200, text);
}

static void handle_drain(Req *request, Res *response) {
    ivr_worker_http_t *server = http_from_request(request);
    ivr_worker_http_drain_fn callback = NULL;
    void *context = NULL;
    if (server) {
        salts_mutex_lock(&server->lock);
        callback = server->drain_callback;
        context = server->drain_context;
        salts_mutex_unlock(&server->lock);
    }
    if (!callback) {
        send_json(response, 503, "{\"accepted\":false}");
        return;
    }
    if (callback(context) != 0) {
        send_json(response, 503, "{\"accepted\":false}");
        return;
    }
    send_json(response, 202, "{\"accepted\":true}");
}

static void mark_running(void *arg1, void *arg2) {
    ivr_worker_http_t *server = (ivr_worker_http_t *)arg1;
    (void)arg2;
    salts_mutex_lock(&server->lock);
    if (server->state == IVR_WORKER_HTTP_STARTING) {
        server->state = IVR_WORKER_HTTP_RUNNING;
        salts_cond_broadcast(&server->cond);
    }
    salts_mutex_unlock(&server->lock);
}

static void http_thread(void *opaque) {
    ivr_worker_http_t *server = (ivr_worker_http_t *)opaque;
    coro_context_t *context = coro_context_create(NULL);
    coro_socket_t *listener = NULL;
    if (context) {
        listener = iris_server_start_on(server->app, context, server->host,
                                        (unsigned short)server->port);
    }
    if (!context || !listener) {
        if (context) {
            coro_context_destroy(context);
        }
        salts_mutex_lock(&server->lock);
        server->state = IVR_WORKER_HTTP_FAILED;
        salts_cond_broadcast(&server->cond);
        salts_mutex_unlock(&server->lock);
        return;
    }
    coro_context_set_persistent(context, 1);
    salts_mutex_lock(&server->lock);
    server->context = context;
    server->listener = listener;
    salts_mutex_unlock(&server->lock);
    if (coro_post(context, mark_running, server, NULL) != 0) {
        salts_mutex_lock(&server->lock);
        server->context = NULL;
        server->listener = NULL;
        server->state = IVR_WORKER_HTTP_FAILED;
        salts_cond_broadcast(&server->cond);
        salts_mutex_unlock(&server->lock);
        coro_context_set_persistent(context, 0);
        coro_socket_destroy(listener);
        coro_context_destroy(context);
        return;
    }
    (void)coro_context_run(context, TURBO_RUN_DEFAULT);
    salts_mutex_lock(&server->lock);
    server->context = NULL;
    server->listener = NULL;
    server->state = IVR_WORKER_HTTP_STOPPING;
    salts_mutex_unlock(&server->lock);
    coro_context_set_persistent(context, 0);
    coro_socket_destroy(listener);
    coro_context_destroy(context);
    salts_mutex_lock(&server->lock);
    server->state = IVR_WORKER_HTTP_STOPPED;
    salts_cond_broadcast(&server->cond);
    salts_mutex_unlock(&server->lock);
}

int ivr_worker_http_create(ivr_worker_health_t *health,
                           ivr_worker_http_t **out_server) {
    ivr_worker_http_t *server;
    if (!health || !out_server) {
        return -1;
    }
    server = (ivr_worker_http_t *)calloc(1, sizeof(*server));
    if (!server) {
        return -1;
    }
    salts_mutex_init(&server->lock);
    salts_cond_init(&server->cond);
    server->app = iris_app_create();
    if (!server->app) {
        salts_cond_destroy(&server->cond);
        salts_mutex_destroy(&server->lock);
        free(server);
        return -1;
    }
    server->health = health;
    server->state = IVR_WORKER_HTTP_STOPPED;
    if (iris_app_bind_rpc_context(server->app, IVR_WORKER_HTTP_CONTEXT_PATH,
                                  server) != 0) {
        iris_app_destroy(server->app);
        salts_cond_destroy(&server->cond);
        salts_mutex_destroy(&server->lock);
        free(server);
        return -1;
    }
    iris_app_get(server->app, "/live", handle_live);
    iris_app_get(server->app, "/ready", handle_ready);
    iris_app_get(server->app, "/health", handle_health);
    iris_app_get(server->app, "/metrics", handle_metrics);
    iris_app_post(server->app, "/drain", handle_drain);
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
    if (!server || !callback) return -1;
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
    if (!server || !host ||
        (strcmp(host, "127.0.0.1") != 0 && strcmp(host, "::1") != 0) ||
        port <= 0 || port > UINT16_MAX) {
        return -1;
    }
    salts_mutex_lock(&server->lock);
    if (server->state != IVR_WORKER_HTTP_STOPPED || server->thread_started) {
        salts_mutex_unlock(&server->lock);
        return -1;
    }
    if (snprintf(server->host, sizeof(server->host), "%s", host) < 0 ||
        strlen(server->host) != strlen(host)) {
        salts_mutex_unlock(&server->lock);
        return -1;
    }
    server->port = port;
    server->state = IVR_WORKER_HTTP_STARTING;
    if (salts_thread_create(&server->thread, http_thread, server) != 0) {
        server->state = IVR_WORKER_HTTP_STOPPED;
        salts_mutex_unlock(&server->lock);
        return -1;
    }
    server->thread_started = 1;
    while (server->state == IVR_WORKER_HTTP_STARTING) {
        salts_cond_wait(&server->cond, &server->lock);
    }
    if (server->state == IVR_WORKER_HTTP_RUNNING) {
        salts_mutex_unlock(&server->lock);
        return 0;
    }
    salts_mutex_unlock(&server->lock);
    salts_thread_join(&server->thread);
    salts_mutex_lock(&server->lock);
    server->thread_started = 0;
    server->state = IVR_WORKER_HTTP_STOPPED;
    salts_mutex_unlock(&server->lock);
    return -1;
}

void ivr_worker_http_stop(ivr_worker_http_t *server) {
    int should_join;
    if (!server) {
        return;
    }
    salts_mutex_lock(&server->lock);
    if (server->state == IVR_WORKER_HTTP_RUNNING && server->context) {
        server->state = IVR_WORKER_HTTP_STOPPING;
        coro_context_stop(server->context);
    }
    should_join = server->thread_started;
    salts_mutex_unlock(&server->lock);
    if (should_join) {
        salts_thread_join(&server->thread);
        salts_mutex_lock(&server->lock);
        server->thread_started = 0;
        server->state = IVR_WORKER_HTTP_STOPPED;
        salts_mutex_unlock(&server->lock);
    }
}

void ivr_worker_http_destroy(ivr_worker_http_t *server) {
    if (!server) {
        return;
    }
    ivr_worker_http_stop(server);
    (void)iris_app_unbind_rpc_context(server->app,
                                      IVR_WORKER_HTTP_CONTEXT_PATH, server);
    iris_app_destroy(server->app);
    salts_cond_destroy(&server->cond);
    salts_mutex_destroy(&server->lock);
    free(server);
}
