/**
 * @file server.c
 * @brief Signaling server wrapper implementation
 */

#include "signaling_server/server.h"
#include "signaling_server/config.h"
#include "http_api.h"
#include <tlog.h>
#include <platform.h>
#include <turbo_coro_context.h>
#include <stdlib.h>
#include <string.h>

/**
 * Signaling server instance
 */
struct signaling_server_s {
    signaling_server_config_t config;
    webrtc_signaling_server_t *ws_server;
    http_api_server_t *http_server;
    turbo_loop_t *loop;
    int running;
};

/**
 * Generate node ID if not provided
 */
static char *generate_node_id(void) {
    static char node_id[64];
    uint64_t timestamp = turbo_monotonic_ms();
    int pid = turbo_getpid();
    
    snprintf(node_id, sizeof(node_id), "node-%d-%llu", pid, 
             (unsigned long long)timestamp);
    
    return node_id;
}

/**
 * Create signaling server instance
 */
signaling_server_t *signaling_server_create(const signaling_server_config_t *config) {
    signaling_server_t *server = NULL;
    
    /* Validate configuration */
    if (signaling_server_config_validate(config) != 0) {
        TLOG_ERROR("Invalid configuration");
        return NULL;
    }
    
    /* Allocate server */
    server = (signaling_server_t *)calloc(1, sizeof(signaling_server_t));
    if (!server) {
        TLOG_ERROR("Failed to allocate server");
        return NULL;
    }
    
    /* Copy configuration */
    memcpy(&server->config, config, sizeof(signaling_server_config_t));
    
    /* Generate node ID if not provided */
    if (!server->config.node_id) {
        server->config.node_id = generate_node_id();
        TLOG_INFOF("Generated node ID: {}", server->config.node_id);
    }
    
    /* Create event loop */
    server->loop = turbo_loop_create();
    if (!server->loop) {
        TLOG_ERROR("Failed to create event loop");
        free(server);
        return NULL;
    }
    
    /* Create WebRTC signaling server */
    webrtc_signaling_config_t ws_config = {
        .host = server->config.ws_host,
        .port = (uint16_t)server->config.ws_port,
        .use_tls = server->config.ws_use_tls,
        .cert_file = server->config.ws_cert_file,
        .key_file = server->config.ws_key_file,
        .max_peers = server->config.max_peers,
        .max_rooms = server->config.max_rooms,
        .peer_timeout_ms = server->config.peer_timeout_ms,
        .join_timeout_ms = server->config.join_timeout_ms,
        .max_message_size = (size_t)server->config.max_message_size,
        .messages_per_second = server->config.messages_per_second,
        .message_burst = server->config.message_burst,
        .max_outbox_messages =
            (size_t)server->config.max_outbox_messages,
        .max_outbox_bytes = (size_t)server->config.max_outbox_bytes,
        .max_connections_per_source =
            server->config.max_connections_per_source,
        .source_admissions_per_second =
            server->config.source_admissions_per_second,
        .source_admission_burst = server->config.source_admission_burst,
        .max_source_states = (size_t)server->config.max_source_states,
        .source_state_ttl_ms = server->config.source_state_ttl_ms,
        .jwt_enabled = server->config.jwt_enabled,
        .jwt_issuer = server->config.jwt_issuer,
        .jwt_active_key_id = server->config.jwt_active_key_id,
        .jwt_secret = server->config.jwt_secret,
        .jwt_previous_key_id = server->config.jwt_previous_key_id,
        .jwt_previous_secret = server->config.jwt_previous_secret,
        .jwt_revoked_token_sha256 =
            server->config.jwt_revoked_token_sha256,
        .jwt_clock_skew_seconds = server->config.jwt_clock_skew_seconds,
        .jwt_max_ttl_seconds = server->config.jwt_ttl_seconds,
        .jwt_algo = server->config.jwt_algorithm
    };
    
    server->ws_server = webrtc_signaling_create(server->loop, &ws_config);
    if (!server->ws_server) {
        TLOG_ERROR("Failed to create WebRTC signaling server");
        free(server);
        return NULL;
    }
    
    server->running = 0;
    server->http_server = NULL;
    
    TLOG_INFO("Signaling server created successfully");
    
    return server;
}

/**
 * Start signaling server
 */
int signaling_server_start(signaling_server_t *server) {
    if (!server) {
        return -1;
    }
    
    /* Start WebSocket server */
    if (webrtc_signaling_start(server->ws_server) != 0) {
        TLOG_ERROR("Failed to start WebSocket server");
        return -1;
    }
    
    server->running = 1;
    
    TLOG_INFOF("WebSocket server listening on {}:{}",
              server->config.ws_host, 
              server->config.ws_port);
    
    /* Start HTTP API server */
    if (server->config.http_enabled) {
        http_api_config_t http_conf = {
            .host = server->config.http_host,
            .port = server->config.http_port,
            .auth_enabled = server->config.http_auth_enabled,
            .admin_token = server->config.http_admin_token,
            .auth_issuer = server->config.http_auth_issuer,
            .auth_active_key_id =
                server->config.http_auth_active_key_id,
            .auth_active_secret =
                server->config.http_auth_active_secret,
            .auth_previous_key_id =
                server->config.http_auth_previous_key_id,
            .auth_previous_secret =
                server->config.http_auth_previous_secret,
            .auth_revoked_token_sha256 =
                server->config.http_auth_revoked_token_sha256,
            .auth_clock_skew_seconds =
                server->config.http_auth_clock_skew_seconds,
            .auth_max_ttl_seconds =
                server->config.http_auth_max_ttl_seconds,
            .use_tls = server->config.http_use_tls,
            .cert_file = server->config.http_cert_file,
            .key_file = server->config.http_key_file
        };
        server->http_server = http_api_create(server->loop, &http_conf, server->ws_server);
        if (!server->http_server || http_api_start(server->http_server) != 0) {
            TLOG_ERROR("Failed to start HTTP API server");
            http_api_destroy(server->http_server);
            server->http_server = NULL;
            webrtc_signaling_stop(server->ws_server);
            server->running = 0;
            return -1;
        }
        TLOG_INFOF("{} API server started on {}:{}",
                  server->config.http_use_tls ? "HTTPS" : "HTTP",
                  server->config.http_host, server->config.http_port);
    }

    /* TODO: Connect to Redis (Phase 4) */
    if (server->config.redis_enabled) {
        TLOG_WARN("Redis integration not yet implemented (Phase 4)");
    }
    
    return 0;
}

/**
 * Run signaling server (blocks until stopped)
 */
int signaling_server_run(signaling_server_t *server) {
    if (!server || !server->running) {
        return -1;
    }
    
    TLOG_INFO("Entering event loop...");

    webrtc_signaling_run(server->ws_server, TURBO_RUN_DEFAULT);

    TLOG_INFO("Event loop exited");
    return 0;
}

/**
 * Stop signaling server
 */
void signaling_server_stop(signaling_server_t *server) {
    if (!server) {
        return;
    }
    
    TLOG_INFO("Stopping signaling server...");
    
    server->running = 0;
    
    /* Stop WebSocket server */
    if (server->ws_server) {
        webrtc_signaling_stop(server->ws_server);
    }
    
    /* Stop HTTP API server */
    if (server->http_server) {
        http_api_stop(server->http_server);
    }
    
    /* Stop event loop */
    if (server->loop) {
        turbo_loop_stop(server->loop);
    }
    
    TLOG_INFO("Signaling server stopped");
}

/**
 * Destroy signaling server instance
 */
void signaling_server_destroy(signaling_server_t *server) {
    if (!server) {
        return;
    }
    
    TLOG_INFO("Destroying signaling server...");
    
    /* Ensure server is stopped */
    if (server->running) {
        signaling_server_stop(server);
    }
    
    /* Destroy WebSocket server */
    if (server->ws_server) {
        webrtc_signaling_destroy(server->ws_server);
        server->ws_server = NULL;
    }
    
    /* Destroy HTTP API server */
    if (server->http_server) {
        http_api_destroy(server->http_server);
        server->http_server = NULL;
    }
    
    /* Close event loop */
    if (server->loop) {
        turbo_loop_destroy(server->loop);
        server->loop = NULL;
    }
    
    /* Free server */
    free(server);
    
    TLOG_INFO("Signaling server destroyed");
}
