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

typedef struct sfu_node_app_config_s {
    const char *config_file;
    const char *bind_host;
    int bind_port;
    const char *node_id;
    int max_rooms;
    int default_room_capacity;
    int dry_run;
    const char *log_level;
    const char *control_token;
} sfu_node_app_config_t;

void sfu_node_app_config_init(sfu_node_app_config_t *config);
int sfu_node_app_config_load(sfu_node_app_config_t *config, const char *filename);
void sfu_node_app_config_cleanup(sfu_node_app_config_t *config);
int sfu_node_app_config_validate(const sfu_node_app_config_t *config);
void sfu_node_app_config_print(const sfu_node_app_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SFU_NODE_APP_CONFIG_H */
