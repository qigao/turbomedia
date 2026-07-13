#include "room_service/config.h"
#include "room_service/server.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static room_service_app_server_t *g_server = NULL;

static void room_service_signal_handler(int signum) {
    (void)signum;
    if (g_server) {
        room_service_app_server_stop(g_server);
    }
}

static void room_service_setup_signal_handlers(void) {
    signal(SIGINT, room_service_signal_handler);
    signal(SIGTERM, room_service_signal_handler);
}

static void room_service_print_usage(const char *program_name) {
    printf("TurboNet Room Service v%s\n\n", TURBO_ROOM_SERVICE_APP_VERSION);
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Options:\n");
    printf("  -c, --config FILE      Configuration file path\n");
    printf("  --host HOST            Bind host (default: 0.0.0.0)\n");
    printf("  --port PORT            Bind port (default: 9090)\n");
    printf("  --node-id ID           Node identifier\n");
    printf("  --control-token TOKEN  Require bearer token for mutating control API commands\n");
    printf("                         Env: TURBO_ROOM_SERVICE_CONTROL_TOKEN\n");
    printf("  --sfu-control-url URL  Optional SFU control API base URL\n");
    printf("  --sfu-nodes LIST       Comma-separated SFU registry: node_id=url,node2=url2\n");
    printf("                         Env: TURBO_ROOM_SERVICE_SFU_NODES\n");
    printf("  --sfu-control-token TOKEN  Bearer token for SFU control API commands\n");
    printf("                         Env: TURBO_ROOM_SERVICE_SFU_CONTROL_TOKEN\n");
    printf("  --max-rooms N          Maximum rooms tracked\n");
    printf("  --dry-run              Validate config and exit\n");
    printf("  --help                 Show this help message\n");
    printf("  --version              Show version information\n");
}

static void room_service_print_version(void) {
    printf("TurboNet Room Service\n");
    printf("Version: %s\n", TURBO_ROOM_SERVICE_APP_VERSION);
    printf("Build Date: %s %s\n", __DATE__, __TIME__);
}

static int room_service_parse_args(int argc, char **argv,
                                   room_service_app_config_t *config) {
    int i;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            room_service_print_usage(argv[0]);
            return -1;
        } else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
            room_service_print_version();
            return -1;
        } else if (strcmp(arg, "--config") == 0 || strcmp(arg, "-c") == 0) {
            if (++i >= argc) return -1;
            config->config_file = argv[i];
        } else if (strcmp(arg, "--host") == 0) {
            if (++i >= argc) return -1;
            config->bind_host = argv[i];
        } else if (strcmp(arg, "--port") == 0) {
            if (++i >= argc) return -1;
            config->bind_port = atoi(argv[i]);
        } else if (strcmp(arg, "--node-id") == 0) {
            if (++i >= argc) return -1;
            config->node_id = argv[i];
        } else if (strcmp(arg, "--control-token") == 0) {
            if (++i >= argc) return -1;
            config->control_token = argv[i];
        } else if (strcmp(arg, "--sfu-control-url") == 0) {
            if (++i >= argc) return -1;
            config->sfu_control_url = argv[i];
        } else if (strcmp(arg, "--sfu-nodes") == 0) {
            if (++i >= argc) return -1;
            config->sfu_nodes = argv[i];
        } else if (strcmp(arg, "--sfu-control-token") == 0) {
            if (++i >= argc) return -1;
            config->sfu_control_token = argv[i];
        } else if (strcmp(arg, "--max-rooms") == 0) {
            if (++i >= argc) return -1;
            config->max_rooms = atoi(argv[i]);
        } else if (strcmp(arg, "--dry-run") == 0) {
            config->dry_run = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    int ret = 0;
    room_service_app_config_t config;

    room_service_app_config_init(&config);

    if (room_service_parse_args(argc, argv, &config) != 0) {
        return (argc > 1 &&
                (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "--version") == 0))
                   ? 0
                   : 1;
    }

    if (config.config_file &&
        room_service_app_config_load(&config, config.config_file) != 0) {
        fprintf(stderr, "Failed to load config: %s\n", config.config_file);
        ret = 1;
        goto cleanup;
    }

    if (room_service_app_config_validate(&config) != 0) {
        fprintf(stderr, "Invalid room service configuration\n");
        ret = 1;
        goto cleanup;
    }

    room_service_app_config_print(&config);
    room_service_setup_signal_handlers();

    g_server = room_service_app_server_create(&config);
    if (!g_server) {
        fprintf(stderr, "Failed to create room service server\n");
        ret = 1;
        goto cleanup;
    }

    if (room_service_app_server_start(g_server) != 0) {
        fprintf(stderr, "Failed to start room service server\n");
        ret = 1;
        goto cleanup;
    }

    ret = room_service_app_server_run(g_server);

cleanup:
    if (g_server) {
        room_service_app_server_destroy(g_server);
        g_server = NULL;
    }
    room_service_app_config_cleanup(&config);
    return ret;
}
