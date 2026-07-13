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
    const char *node_id;
    const char *control_token;
    const char *sfu_control_url;
    const char *sfu_nodes;
    const char *sfu_control_token;
    int max_rooms;
    int auto_create_rooms;
    int dry_run;
    const char *log_level;
} room_service_app_config_t;

void room_service_app_config_init(room_service_app_config_t *config);
int room_service_app_config_load(room_service_app_config_t *config, const char *filename);
void room_service_app_config_cleanup(room_service_app_config_t *config);
int room_service_app_config_validate(const room_service_app_config_t *config);
void room_service_app_config_print(const room_service_app_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_ROOM_SERVICE_APP_CONFIG_H */
