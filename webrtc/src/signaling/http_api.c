/**
 * @file http_api.c
 * @brief HTTP Management API Implementation
 */

#include "http_api.h"
#include <async.h>
#include <iris_app.h>
#include <server.h>
#include <router.h>
#include <platform.h>
#include <tlog.h>
#include <turbo_coro_context.h>
#include <turbo_coro_socket.h>
#include <stb_sprintf.h>
#include <stdlib.h>
#include <string.h>
#include "turbo_str.h"

/* Global reference for handlers */
static webrtc_signaling_server_t *g_signaling_server = NULL;

struct http_api_server_s {
    iris_app_t *app;
    http_api_config_t config;
    webrtc_signaling_server_t *signaling;
    turbo_thread_t thread;
    int thread_started;
    coro_context_t *ctx;
    coro_socket_t *listener;
    int running;
};

/* --- Handlers --- */

static void handle_health(Req *req, Res *res) {
    (void)req;
    send_text(res, 200, "OK");
}

static void handle_status(Req *req, Res *res) {
    (void)req;
    if (!g_signaling_server) {
        send_text(res, 500, "Server not initialized");
        return;
    }
    char *json = webrtc_signaling_get_status_json(g_signaling_server);
    if (json) {
        send_json(res, 200, json);
        free(json);
    } else {
        send_text(res, 500, "Internal Error");
    }
}

static void handle_get_rooms(Req *req, Res *res) {
    (void)req;
    if (!g_signaling_server) {
        send_text(res, 500, "Server not initialized");
        return;
    }
    char *json = webrtc_signaling_get_rooms_json(g_signaling_server);
    if (json) {
        send_json(res, 200, json);
        free(json);
    } else {
        send_text(res, 500, "Internal Error");
    }
}

static void handle_get_room_peers(Req *req, Res *res) {
    const char *room_id = get_params(req, "id");
    if (!room_id) {
        send_text(res, 400, "Missing room id");
        return;
    }
    char *json = webrtc_signaling_get_room_peers_json(g_signaling_server, room_id);
    if (json) {
        send_json(res, 200, json);
        free(json);
    } else {
        send_text(res, 404, "Room not found");
    }
}

static void handle_kick_peer(Req *req, Res *res) {
    const char *room_id = get_params(req, "id");
    const char *peer_id = get_params(req, "peer_id");
    
    if (!room_id || !peer_id) {
        send_text(res, 400, "Missing params");
        return;
    }
    
    int ret = webrtc_signaling_kick_peer(g_signaling_server, room_id, peer_id, "Kicked by admin");
    if (ret == 0) {
        send_json(res, 200, "{\"success\":true}");
    } else {
        send_text(res, 404, "Peer mismatch or not found");
    }
}

static void handle_broadcast(Req *req, Res *res) {
    const char *room_id = get_params(req, "id");
    if (!req->body || req->body_len == 0) {
        send_text(res, 400, "Missing param body");
        return;
    }
    
    /* We expect body to be the message JSON */
    tstr_t msg = tstr_dup_len(req->body, req->body_len);
    if (msg) {
        int sent = webrtc_signaling_broadcast(g_signaling_server, room_id, "admin", msg);
        tstr_free(msg);
        
        tstr_t resp = tstr_new();
        resp = tstr_cat_fmt(resp, "{\"sent\":%d}", sent);
        send_json(res, 200, resp);
        tstr_free(resp);
    } else {
        send_text(res, 500, "Allocation failed");
    }
}

/* --- Lifecycle --- */

http_api_server_t *http_api_create(void *loop, const http_api_config_t *config, webrtc_signaling_server_t *signaling) {
    http_api_server_t *server = malloc(sizeof(http_api_server_t));
    if (!server) return NULL;
    memset(server, 0, sizeof(*server));
    (void)loop;
    
    server->config = *config;
    server->signaling = signaling;
    server->app = iris_app_create();
    
    if (!server->app) {
        free(server);
        return NULL;
    }
    
    g_signaling_server = signaling;
    
    /* Enable CORS */
    cors_t cors_opts = {0};
    cors_opts.origin = "*";
    cors_opts.methods = "GET, POST, DELETE, OPTIONS";
    cors_opts.headers = "Content-Type, Authorization";
    cors_opts.enabled = 1;
    iris_app_cors(server->app, &cors_opts);
    
    /* Register Routes */
    iris_app_get(server->app, "/health", handle_health);
    iris_app_get(server->app, "/api/v1/status", handle_status);
    iris_app_get(server->app, "/api/v1/rooms", handle_get_rooms);
    iris_app_get(server->app, "/api/v1/rooms/:id/peers", handle_get_room_peers);
    iris_app_delete(server->app, "/api/v1/rooms/:id/peers/:peer_id", handle_kick_peer);
    iris_app_post(server->app, "/api/v1/rooms/:id/broadcast", handle_broadcast);
    
    return server;
}

static void http_api_thread_func(void *arg) {
    http_api_server_t *server = (http_api_server_t *)arg;
    int run_result = 0;
    int async_initialized = 0;

    TLOG_INFO("HTTP API starting on port {}", server->config.port);
    server->ctx = coro_context_create(NULL);
    if (!server->ctx) {
        TLOG_ERROR("Failed to create HTTP API coroutine context");
        server->running = 0;
        return;
    }

    if (iris_async_init(1) == 0) {
        async_initialized = 1;
    } else {
        TLOG_WARN("Failed to initialize Iris async thread pool with a single worker");
    }

    server->listener = iris_server_start(server->app, server->ctx, (unsigned short)server->config.port);
    if (!server->listener) {
        TLOG_ERROR("Failed to start HTTP API listener on port {}", server->config.port);
        coro_context_destroy(server->ctx);
        server->ctx = NULL;
        server->running = 0;
        return;
    }

    coro_context_set_persistent(server->ctx, 1);
    run_result = coro_context_run(server->ctx, TURBO_RUN_DEFAULT);
    (void)run_result;

    if (server->listener) {
        coro_socket_destroy(server->listener);
        server->listener = NULL;
    }
    if (server->ctx) {
        coro_context_destroy(server->ctx);
        server->ctx = NULL;
    }
    if (async_initialized) {
        iris_async_shutdown();
    }

    TLOG_INFO("HTTP API thread exiting");
}

int http_api_start(http_api_server_t *server) {
    if (server->running) return 0;
    
    if (turbo_thread_create(&server->thread, http_api_thread_func, server) != 0) {
        TLOG_ERROR("Failed to create HTTP API thread");
        return -1;
    }
    server->thread_started = 1;
    server->running = 1;
    return 0;
}

void http_api_stop(http_api_server_t *server) {
    if (!server->running) return;
    /* Clean stop not fully implemented due to Iris threading model */
    server->running = 0;
    if (server->ctx) {
        coro_context_set_persistent(server->ctx, 0);
    }
    if (server->listener) {
        coro_socket_destroy(server->listener);
        server->listener = NULL;
    }
    if (server->ctx) {
        coro_context_stop(server->ctx);
    }
    if (server->thread_started) {
        turbo_thread_join(&server->thread);
        server->thread_started = 0;
    }
}

void http_api_destroy(http_api_server_t *server) {
    if (server) {
        if (server->running) {
            http_api_stop(server);
        } else if (server->thread_started) {
            turbo_thread_join(&server->thread);
            server->thread_started = 0;
        }
        iris_app_destroy(server->app);
        free(server);
    }
}
