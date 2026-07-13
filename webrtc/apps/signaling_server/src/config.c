/**
 * @file config.c
 * @brief Signaling server configuration implementation
 */

#include "signaling_server/config.h"
#include <tlog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Initialize configuration with defaults
 */
void signaling_server_config_init(signaling_server_config_t *config) {
    memset(config, 0, sizeof(*config));
    
    /* Defaults */
    config->config_file = NULL;
    config->node_id = NULL; /* Auto-generated */
    
    /* WebSocket */
    config->ws_host = "0.0.0.0";
    config->ws_port = 8080;
    config->ws_use_tls = 0;
    config->ws_cert_file = NULL;
    config->ws_key_file = NULL;
    
    /* HTTP API */
    config->http_enabled = 1;
    config->http_host = "0.0.0.0";
    config->http_port = 8081;
    
    /* Limits */
    config->max_peers = 1000;
    config->max_rooms = 100;
    config->peer_timeout_ms = 60000; /* 60 seconds */
    
    /* JWT */
    config->jwt_enabled = 0; /* Disabled by default for testing */
    config->jwt_secret = NULL;
    config->jwt_ttl_seconds = 3600; /* 1 hour */
    config->jwt_algorithm = "HS256";
    
    /* Redis */
    config->redis_enabled = 0; /* Disabled by default */
    config->redis_host = "localhost";
    config->redis_port = 6379;
    config->redis_password = NULL;
    config->redis_db = 0;
    config->redis_use_streams = 1;
    config->redis_stream_read_interval_ms = 100;
    config->redis_stream_max_len = 1000;
    
    /* Logging */
    config->log_level = "info";
    config->log_format = "json";
    config->log_output = "stdout";
}

/**
 * Load configuration from TOML file
 * 
 * TODO: Implement TOML parsing
 * For now, this is a placeholder that returns success
 */
int signaling_server_config_load(signaling_server_config_t *config, const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        TLOG_ERROR("Failed to open configuration file: {}", filename);
        return -1;
    }
    
    /* TODO: Parse TOML file and update config */
    /* For now, just close the file and use defaults */
    fclose(fp);
    
    TLOG_WARN("TOML parsing not yet implemented, using defaults");
    
    return 0;
}

/**
 * Cleanup configuration resources
 */
void signaling_server_config_cleanup(signaling_server_config_t *config) {
    /* Nothing to cleanup for now (all strings are const char*) */
    /* In the future, if we allocate memory for config values, free them here */
    (void)config;
}

/**
 * Validate configuration
 */
int signaling_server_config_validate(const signaling_server_config_t *config) {
    /* Validate ports */
    if (config->ws_port < 1 || config->ws_port > 65535) {
        TLOG_ERROR("Invalid WebSocket port: {}", config->ws_port);
        return -1;
    }
    
    if (config->http_enabled && (config->http_port < 1 || config->http_port > 65535)) {
        TLOG_ERROR("Invalid HTTP port: {}", config->http_port);
        return -1;
    }
    
    /* Validate limits */
    if (config->max_peers < 1) {
        TLOG_ERROR("Invalid max_peers: {}", config->max_peers);
        return -1;
    }
    
    if (config->max_rooms < 1) {
        TLOG_ERROR("Invalid max_rooms: {}", config->max_rooms);
        return -1;
    }
    
    /* Validate JWT */
    if (config->jwt_enabled && !config->jwt_secret) {
        TLOG_ERROR("JWT enabled but no secret provided");
        return -1;
    }
    
    /* Validate TLS */
    if (config->ws_use_tls) {
        if (!config->ws_cert_file || !config->ws_key_file) {
            TLOG_ERROR("TLS enabled but cert/key files not provided");
            return -1;
        }
    }
    
    return 0;
}

/**
 * Print configuration (for debugging)
 */
void signaling_server_config_print(const signaling_server_config_t *config) {
    TLOG_DEBUG("Configuration:");
    TLOG_DEBUG("  Node ID: {}", config->node_id ? config->node_id : "auto");
    TLOG_DEBUG("  WebSocket: {}:{}", config->ws_host, config->ws_port);
    TLOG_DEBUG("  WebSocket TLS: {}", config->ws_use_tls ? "enabled" : "disabled");
    TLOG_DEBUG("  HTTP API: {}:{}", config->http_host, config->http_port);
    TLOG_DEBUG("  HTTP API: {}", config->http_enabled ? "enabled" : "disabled");
    TLOG_DEBUG("  Max Peers: {}", config->max_peers);
    TLOG_DEBUG("  Max Rooms: {}", config->max_rooms);
    TLOG_DEBUG("  Peer Timeout: {}ms", config->peer_timeout_ms);
    TLOG_DEBUG("  JWT: {}", config->jwt_enabled ? "enabled" : "disabled");
    TLOG_DEBUG("  Redis: {}", config->redis_enabled ? "enabled" : "disabled");
    if (config->redis_enabled) {
        TLOG_DEBUG("  Redis Host: {}:{}", config->redis_host, config->redis_port);
        TLOG_DEBUG("  Redis Streams: {}", config->redis_use_streams ? "enabled" : "disabled");
    }
    TLOG_DEBUG("  Log Level: {}", config->log_level);
}
