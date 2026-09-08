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
#define ROOM_SERVICE_CONTROL_MAX_WORKER_IDENTITIES 32

typedef struct room_service_control_worker_identity_s {
    const char *worker_id;
    const char *active_certificate_sha256;
    const char *previous_certificate_sha256;
    uint64_t previous_expires_at_ms;
    uint64_t generation;
    /* P0-04.4 authorization scope. NULL = not restricted for that dimension.
       tenant_id uses the "<tenant>/" room_id prefix convention; room_scope,
       call_scope and content_capabilities are "*" or comma-separated ids. */
    const char *tenant_id;
    const char *room_scope;
    const char *call_scope;
    const char *content_capabilities;
} room_service_control_worker_identity_t;

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

    /* Iris owns workflow state; RoomService only returns provider facts. */
    const char *iris_control_host;
    int iris_control_port;
    const char *iris_control_path;
    const char *iris_provider_instance_id;
    const char *iris_identity;
    const char *iris_control_ca_file;
    const char *iris_control_cert_file;
    const char *iris_control_key_file;
    const char *iris_control_key_password;
    const char *iris_control_server_name;
    int iris_control_use_tls;
    int iris_control_allow_insecure_loopback;
    int iris_ack_timeout_ms;

    const char *iris_event_store_config;
    const char *iris_event_store_channel;
    const char *iris_command_ledger_channel;
    int iris_correlation_capacity;
    int iris_completion_queue_capacity;
    int iris_reconcile_inventory_queue_capacity;
    int iris_outbox_request_queue_capacity;
    int iris_command_ledger_queue_capacity;
    int iris_command_terminal_retention_seconds;
    int iris_command_retention_batch_size;
    int iris_dead_retention_seconds;
    int iris_archive_retention_seconds;
    int iris_retention_sweep_interval_ms;
    int iris_retention_sweep_batch_size;
    int iris_retry_max_attempts;
    int iris_retry_backoff_ms;
    int iris_drain_timeout_ms;

    /* IVR CHTTP HTTP/1.1 WebSocket endpoint. Zero disables IVR transport. */
    const char *control_ws_bind_host;
    int control_ws_bind_port;
    const char *control_ws_path;
    int control_ws_worker_heartbeat_ms;
    int control_ws_worker_lease_ms;
    int control_ws_dispatch_deadline_ms;
    int control_ws_dialog_capacity;
    int control_ws_use_tls;
    int control_ws_allow_insecure_loopback;
    const char *control_ws_ca_file;
    const char *control_ws_cert_file;
    const char *control_ws_key_file;
    const char *control_ws_key_password;
    room_service_control_worker_identity_t
        control_ws_worker_identities[ROOM_SERVICE_CONTROL_MAX_WORKER_IDENTITIES];
    int control_ws_worker_identity_count;

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
