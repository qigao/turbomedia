/**
 * @file config.h
 * @brief SFU node configuration
 */
#ifndef TURBO_SFU_NODE_APP_CONFIG_H
#define TURBO_SFU_NODE_APP_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_SFU_NODE_APP_VERSION "1.0.0"
#define TURBO_SFU_NODE_MAX_STUN_SERVERS 4
#define TURBO_SFU_NODE_MAX_TURN_SERVERS 4

typedef struct sfu_node_app_config_s {
    const char *config_file;
    const char *bind_host;
    int bind_port;
    int use_tls;
    const char *tls_cert_file;
    const char *tls_key_file;
    const char *node_id;
    int max_rooms;
    int default_room_capacity;
    int dry_run;
    const char *log_level;
    const char *control_token;
    const char *media_access_token;
    const char *auth_issuer;
    const char *auth_active_key_id;
    const char *auth_active_secret;
    const char *auth_previous_key_id;
    const char *auth_previous_secret;
    const char *auth_revoked_token_sha256;
    int auth_dynamic_revocation_capacity;
    int auth_clock_skew_seconds;
    int auth_max_ttl_seconds;
    const char *stun_servers[TURBO_SFU_NODE_MAX_STUN_SERVERS];
    int stun_server_count;
    const char *turn_servers[TURBO_SFU_NODE_MAX_TURN_SERVERS];
    int turn_server_count;
    int ice_allow_loopback;

    /*
     * Owned TOML values. Internal to the SFU node application; callers must
     * release them through sfu_node_app_config_cleanup().
     */
    void *private_data;
} sfu_node_app_config_t;

void sfu_node_app_config_init(sfu_node_app_config_t *config);

/**
 * Replace an initialized destination with an independently owned copy.
 *
 * On failure, destination remains unchanged.
 */
int sfu_node_app_config_copy(sfu_node_app_config_t *destination,
                             const sfu_node_app_config_t *source);

/**
 * Load and validate a TOML configuration file transactionally.
 *
 * On failure, config remains unchanged.
 */
int sfu_node_app_config_load(sfu_node_app_config_t *config, const char *filename);
void sfu_node_app_config_apply_environment(sfu_node_app_config_t *config);
void sfu_node_app_config_cleanup(sfu_node_app_config_t *config);
int sfu_node_app_config_validate(const sfu_node_app_config_t *config);
void sfu_node_app_config_print(const sfu_node_app_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SFU_NODE_APP_CONFIG_H */
