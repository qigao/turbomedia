/**
 * @file config.h
 * @brief Room service configuration
 */
#ifndef TURBO_ROOM_SERVICE_APP_CONFIG_H
#define TURBO_ROOM_SERVICE_APP_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_ROOM_SERVICE_APP_VERSION "1.0.0"

typedef struct room_service_app_config_s {
    const char *config_file;
    const char *bind_host;
    int bind_port;
    int use_tls;
    const char *tls_cert_file;
    const char *tls_key_file;
    const char *node_id;
    const char *control_token;
    const char *auth_issuer;
    const char *auth_active_key_id;
    const char *auth_active_secret;
    const char *auth_previous_key_id;
    const char *auth_previous_secret;
    const char *auth_revoked_token_sha256;
    int auth_clock_skew_seconds;
    int auth_max_ttl_seconds;
    const char *sfu_control_url;
    const char *sfu_nodes;
    const char *sfu_control_token;
    const char *sfu_ca_file;
    const char *sfu_auth_issuer;
    const char *sfu_auth_key_id;
    const char *sfu_auth_secret;
    int sfu_auth_ttl_seconds;
    int max_rooms;
    int auto_create_rooms;
    int dry_run;
    const char *log_level;

    /*
     * Owned TOML values. Internal to the room service application; callers
     * must release them through room_service_app_config_cleanup().
     */
    void *private_data;
} room_service_app_config_t;

void room_service_app_config_init(room_service_app_config_t *config);

/**
 * Load and validate a TOML configuration file transactionally.
 *
 * On failure, config remains unchanged.
 */
int room_service_app_config_load(room_service_app_config_t *config, const char *filename);
void room_service_app_config_apply_environment(room_service_app_config_t *config);
void room_service_app_config_cleanup(room_service_app_config_t *config);
int room_service_app_config_validate(const room_service_app_config_t *config);
void room_service_app_config_print(const room_service_app_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_ROOM_SERVICE_APP_CONFIG_H */
