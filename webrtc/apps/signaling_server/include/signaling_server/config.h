/**
 * @file config.h
 * @brief Signaling server configuration
 */

#ifndef SIGNALING_SERVER_CONFIG_H
#define SIGNALING_SERVER_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define SIGNALING_SERVER_VERSION "1.0.0"

/**
 * Server configuration structure
 */
typedef struct signaling_server_config_s {
    /* Configuration file */
    const char *config_file;
    
    /* Node identity */
    const char *node_id;
    
    /* WebSocket server */
    const char *ws_host;
    int ws_port;
    int ws_use_tls;
    const char *ws_cert_file;
    const char *ws_key_file;
    
    /* HTTP API server */
    int http_enabled;
    const char *http_host;
    int http_port;
    
    /* Limits */
    int max_peers;
    int max_rooms;
    int peer_timeout_ms;
    
    /* JWT authentication */
    int jwt_enabled;
    const char *jwt_secret;
    int jwt_ttl_seconds;
    const char *jwt_algorithm;
    
    /* Redis */
    int redis_enabled;
    const char *redis_host;
    int redis_port;
    const char *redis_password;
    int redis_db;
    int redis_use_streams;
    int redis_stream_read_interval_ms;
    int redis_stream_max_len;
    
    /* Logging */
    const char *log_level;
    const char *log_format;
    const char *log_output;
    
} signaling_server_config_t;

/**
 * Initialize configuration with defaults
 */
void signaling_server_config_init(signaling_server_config_t *config);

/**
 * Load configuration from file (TOML format)
 */
int signaling_server_config_load(signaling_server_config_t *config, const char *filename);

/**
 * Cleanup configuration resources
 */
void signaling_server_config_cleanup(signaling_server_config_t *config);

/**
 * Validate configuration
 */
int signaling_server_config_validate(const signaling_server_config_t *config);

/**
 * Print configuration (for debugging)
 */
void signaling_server_config_print(const signaling_server_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* SIGNALING_SERVER_CONFIG_H */
