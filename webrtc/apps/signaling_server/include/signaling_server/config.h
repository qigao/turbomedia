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

    /* Optional trusted reverse-proxy source identity */
    const char *trusted_proxy_map;
    const char *trusted_proxy_ca_file;
    
    /* HTTP API server */
    int http_enabled;
    const char *http_host;
    int http_port;
    int http_use_tls;
    const char *http_cert_file;
    const char *http_key_file;
    int http_auth_enabled;
    const char *http_admin_token;
    const char *http_auth_issuer;
    const char *http_auth_active_key_id;
    const char *http_auth_active_secret;
    const char *http_auth_previous_key_id;
    const char *http_auth_previous_secret;
    const char *http_auth_revoked_token_sha256;
    int http_auth_clock_skew_seconds;
    int http_auth_max_ttl_seconds;
    
    /* Limits */
    int connection_capacity;
    int max_peers;
    int max_rooms;
    int peer_timeout_ms;
    int join_timeout_ms;
    int max_message_size;
    int messages_per_second;
    int message_burst;
    int max_outbox_messages;
    int max_outbox_bytes;
    int max_connections_per_source;
    int source_admissions_per_second;
    int source_admission_burst;
    int max_source_states;
    int source_state_ttl_ms;
    
    /* JWT authentication */
    int jwt_enabled;
    const char *jwt_issuer;
    const char *jwt_active_key_id;
    const char *jwt_secret;
    const char *jwt_previous_key_id;
    const char *jwt_previous_secret;
    const char *jwt_revoked_token_sha256;
    int jwt_dynamic_revocation_capacity;
    int jwt_clock_skew_seconds;
    int jwt_ttl_seconds;
    const char *jwt_algorithm;

    /* Per-node tenant quota lease projection. Zero disables. */
    int tenant_quota_capacity;
    
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

    /*
     * Owned TOML values. Internal to the signaling application; callers must
     * use signaling_server_config_cleanup() instead of accessing this field.
     */
    void *private_data;
} signaling_server_config_t;

/**
 * Initialize configuration with defaults
 */
void signaling_server_config_init(signaling_server_config_t *config);

/**
 * Load and validate a TOML configuration file.
 *
 * The update is transactional: on failure, config remains unchanged.
 *
 * @param config Initialized configuration to update.
 * @param filename TOML file path.
 * @return 0 on success, -1 on read, parse, schema, allocation, or validation
 *         failure.
 */
int signaling_server_config_load(signaling_server_config_t *config, const char *filename);
void signaling_server_config_apply_environment(signaling_server_config_t *config);

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
