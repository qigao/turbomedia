/**
 * @file config.h
 * @brief Room service configuration
 */
#ifndef TURBO_ROOM_SERVICE_APP_CONFIG_H
#define TURBO_ROOM_SERVICE_APP_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_ROOM_SERVICE_APP_VERSION "1.0.0"
#define ROOM_SERVICE_FMQ_MAX_WORKER_IDENTITIES 32

typedef struct room_service_fmq_worker_identity_s {
    const char *worker_id;
    const char *active_certificate_sha256;
    const char *previous_certificate_sha256;
    uint64_t previous_expires_at_ms;
    uint64_t generation;
    /* P0-04.4 authorization scope. NULL = not restricted for that dimension.
       tenant_id uses the "<tenant>/" room_id prefix convention; room_scope,
       call_scope and content_capabilities are "*" or comma-separated ids.
       pub_topics is the comma-separated PUB/SUB topic allowlist (defaults to
       [fmq].pub_topic when absent). */
    const char *tenant_id;
    const char *room_scope;
    const char *call_scope;
    const char *content_capabilities;
    const char *pub_topics;
} room_service_fmq_worker_identity_t;

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

    /* IVR FlowMQ bridge (ROUTER command endpoint + PUB domain events).
       Both ports are required when IVR is enabled; zero means that the IVR
       feature, not an individual participant transport, is disabled. */
    const char *fmq_bind_host;
    int fmq_bind_port;
    int fmq_pub_port;
    const char *fmq_pub_topic;
    int fmq_worker_heartbeat_ms;
    int fmq_worker_lease_ms;
    int fmq_dispatch_deadline_ms;
    int fmq_use_tls;
    int fmq_allow_insecure_loopback;
    const char *fmq_ca_file;
    const char *fmq_cert_file;
    const char *fmq_key_file;
    const char *fmq_key_password;
    const char *fmq_shared_secret;
    int fmq_tls_rotation_generation;
    room_service_fmq_worker_identity_t
        fmq_worker_identities[ROOM_SERVICE_FMQ_MAX_WORKER_IDENTITIES];
    int fmq_worker_identity_count;

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
