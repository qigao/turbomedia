#include "room_service/config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *room_service_env_value(const char *name) {
    const char *value = getenv(name);
    return (value && value[0] != '\0') ? value : NULL;
}

static int room_service_has_nonspace(const char *start, const char *end) {
    while (start < end) {
        if (!isspace((unsigned char)*start)) {
            return 1;
        }
        start++;
    }
    return 0;
}

static int room_service_sfu_nodes_syntax_valid(const char *nodes) {
    const char *entry;

    if (!nodes || nodes[0] == '\0') {
        return 1;
    }

    entry = nodes;
    while (*entry) {
        const char *end = strchr(entry, ',');
        const char *equals;

        if (!end) {
            end = entry + strlen(entry);
        }
        equals = memchr(entry, '=', (size_t)(end - entry));
        if (!equals ||
            !room_service_has_nonspace(entry, equals) ||
            !room_service_has_nonspace(equals + 1, end)) {
            return 0;
        }
        entry = (*end == ',') ? end + 1 : end;
    }

    return 1;
}

void room_service_app_config_init(room_service_app_config_t *config) {
    if (!config) {
        return;
    }

    config->config_file = NULL;
    config->bind_host = "0.0.0.0";
    config->bind_port = 9090;
    config->node_id = "room-service-local";
    config->control_token = room_service_env_value("TURBO_ROOM_SERVICE_CONTROL_TOKEN");
    config->sfu_control_url = NULL;
    config->sfu_nodes = room_service_env_value("TURBO_ROOM_SERVICE_SFU_NODES");
    config->sfu_control_token = room_service_env_value("TURBO_ROOM_SERVICE_SFU_CONTROL_TOKEN");
    config->max_rooms = 1024;
    config->auto_create_rooms = 1;
    config->dry_run = 0;
    config->log_level = "info";
}

int room_service_app_config_load(room_service_app_config_t *config, const char *filename) {
    (void)config;
    (void)filename;
    return 0;
}

void room_service_app_config_cleanup(room_service_app_config_t *config) {
    (void)config;
}

int room_service_app_config_validate(const room_service_app_config_t *config) {
    if (!config || !config->bind_host || !config->node_id || !config->log_level) {
        return -1;
    }
    if (config->bind_port <= 0 || config->max_rooms <= 0) {
        return -1;
    }
    if ((config->control_token && config->control_token[0] == '\0') ||
        (config->sfu_nodes && config->sfu_nodes[0] == '\0') ||
        (config->sfu_control_token && config->sfu_control_token[0] == '\0')) {
        return -1;
    }
    if (!room_service_sfu_nodes_syntax_valid(config->sfu_nodes)) {
        return -1;
    }
    return 0;
}

void room_service_app_config_print(const room_service_app_config_t *config) {
    if (!config) {
        return;
    }

    printf("Room Service Configuration\n");
    printf("  bind: %s:%d\n", config->bind_host, config->bind_port);
    printf("  node_id: %s\n", config->node_id);
    printf("  control_auth: %s\n",
           (config->control_token && config->control_token[0])
               ? "enabled"
               : "disabled");
    printf("  sfu_control_url: %s\n",
           config->sfu_control_url ? config->sfu_control_url : "(disabled)");
    printf("  sfu_nodes: %s\n", config->sfu_nodes ? config->sfu_nodes : "(none)");
    printf("  sfu_control_auth: %s\n",
           (config->sfu_control_token && config->sfu_control_token[0])
               ? "enabled"
               : "disabled");
    printf("  max_rooms: %d\n", config->max_rooms);
    printf("  auto_create_rooms: %s\n", config->auto_create_rooms ? "true" : "false");
    printf("  dry_run: %s\n", config->dry_run ? "true" : "false");
    printf("  log_level: %s\n", config->log_level);
}
