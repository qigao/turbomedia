#include "sfu_node/config.h"
#include <stdio.h>
#include <stdlib.h>

static const char *sfu_node_env_value(const char *name) {
    const char *value = getenv(name);
    return (value && value[0] != '\0') ? value : NULL;
}

void sfu_node_app_config_init(sfu_node_app_config_t *config) {
    if (!config) {
        return;
    }

    config->config_file = NULL;
    config->bind_host = "0.0.0.0";
    config->bind_port = 9190;
    config->node_id = "sfu-node-local";
    config->max_rooms = 128;
    config->default_room_capacity = 50;
    config->dry_run = 0;
    config->log_level = "info";
    config->control_token = sfu_node_env_value("TURBO_SFU_NODE_CONTROL_TOKEN");
}

int sfu_node_app_config_load(sfu_node_app_config_t *config, const char *filename) {
    (void)config;
    (void)filename;
    return 0;
}

void sfu_node_app_config_cleanup(sfu_node_app_config_t *config) {
    (void)config;
}

int sfu_node_app_config_validate(const sfu_node_app_config_t *config) {
    if (!config || !config->bind_host || !config->node_id || !config->log_level) {
        return -1;
    }
    if (config->bind_port <= 0 || config->max_rooms <= 0 ||
        config->default_room_capacity <= 0) {
        return -1;
    }
    if (config->control_token && config->control_token[0] == '\0') {
        return -1;
    }
    return 0;
}

void sfu_node_app_config_print(const sfu_node_app_config_t *config) {
    if (!config) {
        return;
    }

    printf("SFU Node Configuration\n");
    printf("  bind: %s:%d\n", config->bind_host, config->bind_port);
    printf("  node_id: %s\n", config->node_id);
    printf("  max_rooms: %d\n", config->max_rooms);
    printf("  default_room_capacity: %d\n", config->default_room_capacity);
    printf("  dry_run: %s\n", config->dry_run ? "true" : "false");
    printf("  log_level: %s\n", config->log_level);
    printf("  control_auth: %s\n",
           (config->control_token && config->control_token[0]) ? "enabled" : "disabled");
}
