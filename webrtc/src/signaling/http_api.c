/**
 * @file http_api.c
 * @brief HTTP Management API Implementation
 */

#include "http_api.h"
#include "turbo_media_auth.h"
#include <iris/iris_app.h>
#include <iris/server.h>
#include <iris/router.h>
#include <platform.h>
#include <tlog.h>
#include <turbo_coro_context.h>
#include <turbo_coro_socket.h>
#include <stb_sprintf.h>
#include <stdlib.h>
#include <string.h>
#include "turbo_str.h"

static const char *HTTP_API_SIGNALING_CONTEXT_PATH = "/_turbomedia/webrtc-signaling";
static const char *HTTP_API_SERVER_CONTEXT_PATH = "/_turbomedia/http-api";
#define SIGNALING_MANAGEMENT_AUDIENCE "turbomedia-signaling-management"
#define SIGNALING_MANAGEMENT_SCOPE_READ "signaling.management.read"
#define SIGNALING_MANAGEMENT_SCOPE_WRITE "signaling.management.write"
#define SIGNALING_MANAGEMENT_SCOPE_DANGEROUS "signaling.management.dangerous"

struct http_api_server_s {
    iris_app_t *app;
    http_api_config_t config;
    webrtc_signaling_server_t *signaling;
    turbo_thread_t thread;
    int thread_started;
    turbo_mutex_t lifecycle_mutex;
    turbo_cond_t lifecycle_cond;
    coro_context_t *ctx;
    coro_socket_t *listener;
    int state;
};

typedef enum {
    HTTP_API_STOPPED = 0,
    HTTP_API_STARTING,
    HTTP_API_RUNNING,
    HTTP_API_STOPPING,
    HTTP_API_FAILED
} http_api_state_t;

static webrtc_signaling_server_t *signaling_from_request(const Req *req) {
    if (!req || !req->app) {
        return NULL;
    }
    return (webrtc_signaling_server_t *)iris_app_lookup_rpc_context(
        req->app, HTTP_API_SIGNALING_CONTEXT_PATH);
}

static http_api_server_t *http_api_from_request(const Req *req) {
    if (!req || !req->app) {
        return NULL;
    }
    return (http_api_server_t *)iris_app_lookup_rpc_context(
        req->app, HTTP_API_SERVER_CONTEXT_PATH);
}

static turbo_media_auth_config_t signed_auth_config(
    const http_api_config_t *config) {
    turbo_media_auth_config_t auth = {0};

    if (!config) {
        return auth;
    }
    auth.issuer = config->auth_issuer;
    auth.active_key_id = config->auth_active_key_id;
    auth.active_secret = config->auth_active_secret;
    auth.previous_key_id = config->auth_previous_key_id;
    auth.previous_secret = config->auth_previous_secret;
    auth.revoked_token_sha256 = config->auth_revoked_token_sha256;
    auth.clock_skew_seconds = config->auth_clock_skew_seconds;
    auth.max_ttl_seconds = config->auth_max_ttl_seconds;
    return auth;
}

static int authorize_management_request(
    Req *req, Res *res, const char *required_scope,
    const char *room_id, const char *participant_id) {
    http_api_server_t *server = http_api_from_request(req);
    const char *authorization;
    turbo_media_auth_config_t auth;
    turbo_media_auth_policy_t policy;

    if (!server) {
        send_text(res, 500, "Server not initialized");
        return 0;
    }
    if (!server->config.auth_enabled) {
        return 1;
    }

    authorization = get_headers(req, "Authorization");
    if (!authorization) {
        authorization = get_headers(req, "authorization");
    }
    auth = signed_auth_config(&server->config);
    memset(&policy, 0, sizeof(policy));
    policy.audience = SIGNALING_MANAGEMENT_AUDIENCE;
    policy.required_scope = required_scope;
    policy.room_id = room_id;
    policy.participant_id = participant_id;
    if (turbo_media_auth_authorize(
            authorization, server->config.admin_token, &auth, &policy) ==
        TURBO_MEDIA_AUTH_DENIED) {
        set_header(res, "WWW-Authenticate", "Bearer");
        send_text(res, 401, "Unauthorized");
        return 0;
    }
    return 1;
}

/* --- Handlers --- */

static void handle_health(Req *req, Res *res) {
    (void)req;
    send_text(res, 200, "OK");
}

static void handle_status(Req *req, Res *res) {
    if (!authorize_management_request(
            req, res, SIGNALING_MANAGEMENT_SCOPE_READ, NULL, NULL)) {
        return;
    }
    webrtc_signaling_server_t *signaling = signaling_from_request(req);
    if (!signaling) {
        send_text(res, 500, "Server not initialized");
        return;
    }
    char *json = webrtc_signaling_get_status_json(signaling);
    if (json) {
        send_json(res, 200, json);
        free(json);
    } else {
        send_text(res, 500, "Internal Error");
    }
}

static void handle_get_rooms(Req *req, Res *res) {
    if (!authorize_management_request(
            req, res, SIGNALING_MANAGEMENT_SCOPE_READ, NULL, NULL)) {
        return;
    }
    webrtc_signaling_server_t *signaling = signaling_from_request(req);
    if (!signaling) {
        send_text(res, 500, "Server not initialized");
        return;
    }
    char *json = webrtc_signaling_get_rooms_json(signaling);
    if (json) {
        send_json(res, 200, json);
        free(json);
    } else {
        send_text(res, 500, "Internal Error");
    }
}

static void handle_get_room_peers(Req *req, Res *res) {
    webrtc_signaling_server_t *signaling = signaling_from_request(req);
    const char *room_id = get_params(req, "id");
    if (!authorize_management_request(
            req, res, SIGNALING_MANAGEMENT_SCOPE_READ, room_id, NULL)) {
        return;
    }
    if (!signaling) {
        send_text(res, 500, "Server not initialized");
        return;
    }
    if (!room_id) {
        send_text(res, 400, "Missing room id");
        return;
    }
    char *json = webrtc_signaling_get_room_peers_json(signaling, room_id);
    if (json) {
        send_json(res, 200, json);
        free(json);
    } else {
        send_text(res, 404, "Room not found");
    }
}

static void handle_kick_peer(Req *req, Res *res) {
    webrtc_signaling_server_t *signaling = signaling_from_request(req);
    const char *room_id = get_params(req, "id");
    const char *peer_id = get_params(req, "peer_id");
    if (!authorize_management_request(
            req, res, SIGNALING_MANAGEMENT_SCOPE_DANGEROUS,
            room_id, peer_id)) {
        return;
    }
    
    if (!signaling) {
        send_text(res, 500, "Server not initialized");
        return;
    }
    if (!room_id || !peer_id) {
        send_text(res, 400, "Missing params");
        return;
    }
    
    int ret = webrtc_signaling_kick_peer(signaling, room_id, peer_id, "Kicked by admin");
    if (ret == 0) {
        send_json(res, 200, "{\"success\":true}");
    } else {
        send_text(res, 404, "Peer mismatch or not found");
    }
}

static void handle_broadcast(Req *req, Res *res) {
    webrtc_signaling_server_t *signaling = signaling_from_request(req);
    const char *room_id = get_params(req, "id");
    if (!authorize_management_request(
            req, res, SIGNALING_MANAGEMENT_SCOPE_WRITE, room_id, NULL)) {
        return;
    }
    if (!signaling) {
        send_text(res, 500, "Server not initialized");
        return;
    }
    if (!room_id || !req->body || req->body_len == 0) {
        send_text(res, 400, "Missing param body");
        return;
    }
    
    /* We expect body to be the message JSON */
    tstr_t msg = tstr_dup_len(req->body, req->body_len);
    if (msg) {
        int sent = webrtc_signaling_broadcast(signaling, room_id, "admin", msg);
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

static int http_api_auth_config_valid(const http_api_config_t *config) {
    turbo_media_auth_config_t auth;
    int has_static;
    int has_signed;

    if (!config || !config->auth_enabled) {
        return config != NULL;
    }
    has_static = config->admin_token && config->admin_token[0] != '\0';
    has_signed = config->auth_active_secret &&
                 config->auth_active_secret[0] != '\0';
    if (!has_static && !has_signed) {
        return 0;
    }
    if (!has_signed) {
        return 1;
    }
    auth = signed_auth_config(config);
    return turbo_media_auth_config_validate(&auth) == 0;
}

static void http_api_free_config_strings(http_api_config_t *config) {
    if (!config) {
        return;
    }
    tstr_free((tstr_t)config->auth_revoked_token_sha256);
    tstr_free((tstr_t)config->auth_previous_secret);
    tstr_free((tstr_t)config->auth_previous_key_id);
    tstr_free((tstr_t)config->auth_active_secret);
    tstr_free((tstr_t)config->auth_active_key_id);
    tstr_free((tstr_t)config->auth_issuer);
    tstr_free((tstr_t)config->admin_token);
    tstr_free((tstr_t)config->cert_file);
    tstr_free((tstr_t)config->key_file);
    tstr_free((tstr_t)config->host);
    memset(config, 0, sizeof(*config));
}

static int http_api_copy_optional_string(const char *source,
                                         const char **destination) {
    if (!destination) {
        return -1;
    }
    *destination = source ? tstr_dup(source) : NULL;
    return source && !*destination ? -1 : 0;
}

http_api_server_t *http_api_create(void *loop, const http_api_config_t *config, webrtc_signaling_server_t *signaling) {
    if (!config || !signaling || config->port <= 0 || config->port > UINT16_MAX ||
        !config->host || config->host[0] == '\0' ||
        !http_api_auth_config_valid(config) ||
        (config->use_tls &&
         (!config->cert_file || config->cert_file[0] == '\0' ||
          !config->key_file || config->key_file[0] == '\0'))) {
        return NULL;
    }
    http_api_server_t *server = malloc(sizeof(http_api_server_t));
    if (!server) return NULL;
    memset(server, 0, sizeof(*server));
    (void)loop;
    
    server->config = *config;
    server->config.host = tstr_dup(config->host);
    server->config.admin_token = NULL;
    server->config.auth_issuer = NULL;
    server->config.auth_active_key_id = NULL;
    server->config.auth_active_secret = NULL;
    server->config.auth_previous_key_id = NULL;
    server->config.auth_previous_secret = NULL;
    server->config.auth_revoked_token_sha256 = NULL;
    server->config.cert_file = NULL;
    server->config.key_file = NULL;
    if (!server->config.host ||
        http_api_copy_optional_string(config->admin_token,
                                      &server->config.admin_token) != 0 ||
        http_api_copy_optional_string(config->auth_issuer,
                                      &server->config.auth_issuer) != 0 ||
        http_api_copy_optional_string(config->auth_active_key_id,
                                      &server->config.auth_active_key_id) != 0 ||
        http_api_copy_optional_string(config->auth_active_secret,
                                      &server->config.auth_active_secret) != 0 ||
        http_api_copy_optional_string(config->auth_previous_key_id,
                                      &server->config.auth_previous_key_id) != 0 ||
        http_api_copy_optional_string(config->auth_previous_secret,
                                      &server->config.auth_previous_secret) != 0 ||
        http_api_copy_optional_string(
            config->auth_revoked_token_sha256,
            &server->config.auth_revoked_token_sha256) != 0) {
        http_api_free_config_strings(&server->config);
        free(server);
        return NULL;
    }
    if (config->use_tls) {
        server->config.cert_file = tstr_dup(config->cert_file);
        server->config.key_file = tstr_dup(config->key_file);
        if (!server->config.cert_file || !server->config.key_file) {
            http_api_free_config_strings(&server->config);
            free(server);
            return NULL;
        }
    }
    server->signaling = signaling;
    turbo_mutex_init(&server->lifecycle_mutex);
    turbo_cond_init(&server->lifecycle_cond);
    server->app = iris_app_create();
    
    if (!server->app) {
        http_api_free_config_strings(&server->config);
        turbo_cond_destroy(&server->lifecycle_cond);
        turbo_mutex_destroy(&server->lifecycle_mutex);
        free(server);
        return NULL;
    }
    
    if (iris_app_bind_rpc_context(server->app, HTTP_API_SIGNALING_CONTEXT_PATH,
                                  signaling) != 0 ||
        iris_app_bind_rpc_context(server->app, HTTP_API_SERVER_CONTEXT_PATH,
                                  server) != 0) {
        iris_app_unbind_rpc_context(server->app, HTTP_API_SIGNALING_CONTEXT_PATH,
                                    signaling);
        iris_app_destroy(server->app);
        http_api_free_config_strings(&server->config);
        turbo_cond_destroy(&server->lifecycle_cond);
        turbo_mutex_destroy(&server->lifecycle_mutex);
        free(server);
        return NULL;
    }
    
    if (!config->auth_enabled) {
        cors_t cors_opts = {0};
        cors_opts.origin = "*";
        cors_opts.methods = "GET, POST, DELETE, OPTIONS";
        cors_opts.headers = "Content-Type, Authorization";
        cors_opts.enabled = 1;
        iris_app_cors(server->app, &cors_opts);
    }
    
    /* Register Routes */
    iris_app_get(server->app, "/health", handle_health);
    iris_app_get(server->app, "/api/v1/status", handle_status);
    iris_app_get(server->app, "/api/v1/rooms", handle_get_rooms);
    iris_app_get(server->app, "/api/v1/rooms/:id/peers", handle_get_room_peers);
    iris_app_delete(server->app, "/api/v1/rooms/:id/peers/:peer_id", handle_kick_peer);
    iris_app_post(server->app, "/api/v1/rooms/:id/broadcast", handle_broadcast);
    
    return server;
}

static void http_api_mark_running(void *arg1, void *arg2) {
    http_api_server_t *server = (http_api_server_t *)arg1;
    (void)arg2;

    turbo_mutex_lock(&server->lifecycle_mutex);
    if (server->state == HTTP_API_STARTING) {
        server->state = HTTP_API_RUNNING;
        turbo_cond_broadcast(&server->lifecycle_cond);
    }
    turbo_mutex_unlock(&server->lifecycle_mutex);
}

static void http_api_thread_func(void *arg) {
    http_api_server_t *server = (http_api_server_t *)arg;
    int run_result = 0;
    coro_context_t *ctx = NULL;
    coro_socket_t *listener = NULL;

    TLOG_INFO("{} API starting on {}:{}", server->config.use_tls ? "HTTPS" : "HTTP",
              server->config.host, server->config.port);
    ctx = coro_context_create(NULL);
    if (!ctx) {
        TLOG_ERROR("Failed to create HTTP API coroutine context");
        turbo_mutex_lock(&server->lifecycle_mutex);
        server->state = HTTP_API_FAILED;
        turbo_cond_broadcast(&server->lifecycle_cond);
        turbo_mutex_unlock(&server->lifecycle_mutex);
        return;
    }

    if (server->config.use_tls) {
        turbo_tls_server_config_t tls_config;

        memset(&tls_config, 0, sizeof(tls_config));
        tls_config.size = sizeof(tls_config);
        tls_config.cert_file = server->config.cert_file;
        tls_config.key_file = server->config.key_file;
        tls_config.client_auth = TURBO_TLS_CLIENT_AUTH_NONE;
        listener = iris_server_start_tls_on(
            server->app, ctx, server->config.host,
            (unsigned short)server->config.port, &tls_config);
    } else {
        listener = iris_server_start_on(
            server->app, ctx, server->config.host,
            (unsigned short)server->config.port);
    }
    if (!listener) {
        TLOG_ERROR("Failed to start HTTP API listener on port {}", server->config.port);
        coro_context_destroy(ctx);
        turbo_mutex_lock(&server->lifecycle_mutex);
        server->state = HTTP_API_FAILED;
        turbo_cond_broadcast(&server->lifecycle_cond);
        turbo_mutex_unlock(&server->lifecycle_mutex);
        return;
    }

    coro_context_set_persistent(ctx, 1);
    turbo_mutex_lock(&server->lifecycle_mutex);
    server->ctx = ctx;
    server->listener = listener;
    turbo_mutex_unlock(&server->lifecycle_mutex);

    if (coro_post(ctx, http_api_mark_running, server, NULL) != 0) {
        turbo_mutex_lock(&server->lifecycle_mutex);
        server->listener = NULL;
        server->ctx = NULL;
        server->state = HTTP_API_FAILED;
        turbo_cond_broadcast(&server->lifecycle_cond);
        turbo_mutex_unlock(&server->lifecycle_mutex);
        coro_context_set_persistent(ctx, 0);
        coro_socket_destroy(listener);
        coro_context_destroy(ctx);
        return;
    }

    run_result = coro_context_run(ctx, TURBO_RUN_DEFAULT);
    (void)run_result;

    turbo_mutex_lock(&server->lifecycle_mutex);
    server->listener = NULL;
    server->ctx = NULL;
    server->state = HTTP_API_STOPPING;
    turbo_mutex_unlock(&server->lifecycle_mutex);

    coro_context_set_persistent(ctx, 0);
    coro_socket_destroy(listener);
    coro_context_destroy(ctx);

    turbo_mutex_lock(&server->lifecycle_mutex);
    server->state = HTTP_API_STOPPED;
    turbo_cond_broadcast(&server->lifecycle_cond);
    turbo_mutex_unlock(&server->lifecycle_mutex);

    TLOG_INFO("HTTP API thread exiting");
}

int http_api_start(http_api_server_t *server) {
    if (!server) return -1;
    turbo_mutex_lock(&server->lifecycle_mutex);
    if (server->state == HTTP_API_RUNNING) {
        turbo_mutex_unlock(&server->lifecycle_mutex);
        return 0;
    }
    if (server->state != HTTP_API_STOPPED || server->thread_started) {
        turbo_mutex_unlock(&server->lifecycle_mutex);
        return -1;
    }
    server->state = HTTP_API_STARTING;
    if (turbo_thread_create(&server->thread, http_api_thread_func, server) != 0) {
        server->state = HTTP_API_STOPPED;
        turbo_mutex_unlock(&server->lifecycle_mutex);
        TLOG_ERROR("Failed to create HTTP API thread");
        return -1;
    }
    server->thread_started = 1;
    while (server->state == HTTP_API_STARTING) {
        turbo_cond_wait(&server->lifecycle_cond, &server->lifecycle_mutex);
    }
    if (server->state == HTTP_API_RUNNING) {
        turbo_mutex_unlock(&server->lifecycle_mutex);
        return 0;
    }
    turbo_mutex_unlock(&server->lifecycle_mutex);
    turbo_thread_join(&server->thread);
    turbo_mutex_lock(&server->lifecycle_mutex);
    server->thread_started = 0;
    server->state = HTTP_API_STOPPED;
    turbo_mutex_unlock(&server->lifecycle_mutex);
    return -1;
}

void http_api_stop(http_api_server_t *server) {
    int should_join = 0;
    if (!server) return;
    turbo_mutex_lock(&server->lifecycle_mutex);
    if (server->state == HTTP_API_RUNNING && server->ctx) {
        server->state = HTTP_API_STOPPING;
        coro_context_stop(server->ctx);
    }
    should_join = server->thread_started;
    turbo_mutex_unlock(&server->lifecycle_mutex);

    if (should_join) {
        turbo_thread_join(&server->thread);
        turbo_mutex_lock(&server->lifecycle_mutex);
        server->thread_started = 0;
        server->state = HTTP_API_STOPPED;
        turbo_mutex_unlock(&server->lifecycle_mutex);
    }
}

void http_api_destroy(http_api_server_t *server) {
    if (server) {
        http_api_stop(server);
        iris_app_unbind_rpc_context(server->app, HTTP_API_SIGNALING_CONTEXT_PATH,
                                    server->signaling);
        iris_app_unbind_rpc_context(server->app, HTTP_API_SERVER_CONTEXT_PATH,
                                    server);
        iris_app_destroy(server->app);
        http_api_free_config_strings(&server->config);
        turbo_cond_destroy(&server->lifecycle_cond);
        turbo_mutex_destroy(&server->lifecycle_mutex);
        free(server);
    }
}
