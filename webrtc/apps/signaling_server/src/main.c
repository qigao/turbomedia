/**
 * @file main.c
 * @brief Production WebRTC Signaling Server
 * @version 1.0.0
 * 
 * Main entry point for the WebRTC signaling server.
 */

#include "signaling_server/config.h"
#include "signaling_server/server.h"
#include "webrtc_signaling.h"
#include <platform.h>
#include <tlog.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PLATFORM_NAME
#ifdef _WIN32
#define PLATFORM_NAME "Windows"
#else
#define PLATFORM_NAME "Unix"
#endif
#endif

// Forward declare tlog helper if not in header
// void tlog_set_level_str(const char *level);

/* Global server instance for signal handling */
static signaling_server_t *g_server = NULL;

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int signum) {
    const char *signame = "UNKNOWN";
    
    switch (signum) {
        case SIGINT:  signame = "SIGINT";  break;
        case SIGTERM: signame = "SIGTERM"; break;
        default: break;
    }
    
    TLOG_INFOF("Received signal {} ({}), initiating graceful shutdown...",
              signame, signum);
    
    if (g_server) {
        signaling_server_stop(g_server);
    }
}

/**
 * Setup signal handlers
 */
static void setup_signal_handlers(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
#ifndef _WIN32
    /* Ignore SIGPIPE on Unix systems */
    signal(SIGPIPE, SIG_IGN);
#endif
}

/**
 * Print usage information
 */
static void print_usage(const char *program_name) {
    printf("TurboNet WebRTC Signaling Server v%s\n\n", SIGNALING_SERVER_VERSION);
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Options:\n");
    printf("  -c, --config FILE      Configuration file path (default: none)\n");
    printf("  -h, --host HOST        WebSocket host (default: 0.0.0.0)\n");
    printf("  -p, --port PORT        WebSocket port (default: 8080)\n");
    printf("  --tls-cert FILE        Enable WSS with this certificate chain\n");
    printf("  --tls-key FILE         Enable WSS with this private key\n");
    printf("  --http-port PORT       HTTP API port (default: 8081)\n");
    printf("  --http-tls-cert FILE   Enable management HTTPS with this certificate\n");
    printf("  --http-tls-key FILE    Enable management HTTPS with this private key\n");
    printf("  --http-auth-key-id ID  Active scoped management token key id\n");
    printf("  --http-auth-secret SECRET  Active HS256 management token secret\n");
    printf("  --http-auth-previous-key-id ID  Previous key id during rotation\n");
    printf("  --http-auth-previous-secret SECRET  Previous rotation secret\n");
    printf("  --no-http              Disable HTTP management API\n");
    printf("  --node-id ID           Node identifier (default: auto-generated)\n");
    printf("  --redis-host HOST      Redis host (default: localhost)\n");
    printf("  --redis-port PORT      Redis port (default: 6379)\n");
    printf("  --max-peers N          Maximum peers (default: 1000)\n");
    printf("  --max-rooms N          Maximum rooms (default: 100)\n");
    printf("  --log-level LEVEL      Log level: trace|debug|info|warn|error (default: info)\n");
    printf("  --help                 Show this help message\n");
    printf("  --version              Show version information\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --config /etc/turbonet/signaling.toml\n", program_name);
    printf("  %s --port 8080 --http-port 8081 --max-peers 5000\n", program_name);
    printf("  %s --redis-host redis.example.com --node-id node-1\n", program_name);
    printf("\n");
}

/**
 * Print version information
 */
static void print_version(void) {
    printf("TurboNet WebRTC Signaling Server\n");
    printf("Version: %s\n", SIGNALING_SERVER_VERSION);
    printf("Build Date: %s %s\n", __DATE__, __TIME__);
    printf("Platform: %s\n", PLATFORM_NAME);
    printf("\n");
    printf("Features:\n");
    printf("  - WebSocket signaling (CoroNet)\n");
    printf("  - HTTP management API (Iris)\n");
    printf("\n");
}

/**
 * Parse command line arguments
 */
static int parse_args(int argc, char **argv, signaling_server_config_t *config) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 1;
        }
        else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
            print_version();
            return 1;
        }
        else if (strcmp(arg, "--config") == 0 || strcmp(arg, "-c") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --config requires an argument\n");
                return -1;
            }
            config->config_file = argv[i];
        }
        else if (strcmp(arg, "--host") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --host requires an argument\n");
                return -1;
            }
            config->ws_host = argv[i];
        }
        else if (strcmp(arg, "--port") == 0 || strcmp(arg, "-p") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --port requires an argument\n");
                return -1;
            }
            config->ws_port = atoi(argv[i]);
        }
        else if (strcmp(arg, "--http-port") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --http-port requires an argument\n");
                return -1;
            }
            config->http_port = atoi(argv[i]);
            config->http_enabled = 1;
        }
        else if (strcmp(arg, "--tls-cert") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --tls-cert requires an argument\n");
                return -1;
            }
            config->ws_use_tls = 1;
            config->ws_cert_file = argv[i];
        }
        else if (strcmp(arg, "--tls-key") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --tls-key requires an argument\n");
                return -1;
            }
            config->ws_use_tls = 1;
            config->ws_key_file = argv[i];
        }
        else if (strcmp(arg, "--http-tls-cert") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --http-tls-cert requires an argument\n");
                return -1;
            }
            config->http_enabled = 1;
            config->http_use_tls = 1;
            config->http_cert_file = argv[i];
        }
        else if (strcmp(arg, "--http-tls-key") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --http-tls-key requires an argument\n");
                return -1;
            }
            config->http_enabled = 1;
            config->http_use_tls = 1;
            config->http_key_file = argv[i];
        }
        else if (strcmp(arg, "--http-auth-key-id") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --http-auth-key-id requires an argument\n");
                return -1;
            }
            config->http_auth_active_key_id = argv[i];
        }
        else if (strcmp(arg, "--http-auth-secret") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --http-auth-secret requires an argument\n");
                return -1;
            }
            config->http_auth_active_secret = argv[i];
        }
        else if (strcmp(arg, "--http-auth-previous-key-id") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --http-auth-previous-key-id requires an argument\n");
                return -1;
            }
            config->http_auth_previous_key_id = argv[i];
        }
        else if (strcmp(arg, "--http-auth-previous-secret") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --http-auth-previous-secret requires an argument\n");
                return -1;
            }
            config->http_auth_previous_secret = argv[i];
        }
        else if (strcmp(arg, "--no-http") == 0) {
            config->http_enabled = 0;
        }
        else if (strcmp(arg, "--node-id") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --node-id requires an argument\n");
                return -1;
            }
            config->node_id = argv[i];
        }
        else if (strcmp(arg, "--redis-host") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --redis-host requires an argument\n");
                return -1;
            }
            config->redis_host = argv[i];
        }
        else if (strcmp(arg, "--redis-port") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --redis-port requires an argument\n");
                return -1;
            }
            config->redis_port = atoi(argv[i]);
        }
        else if (strcmp(arg, "--max-peers") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --max-peers requires an argument\n");
                return -1;
            }
            config->max_peers = atoi(argv[i]);
        }
        else if (strcmp(arg, "--max-rooms") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --max-rooms requires an argument\n");
                return -1;
            }
            config->max_rooms = atoi(argv[i]);
        }
        else if (strcmp(arg, "--log-level") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --log-level requires an argument\n");
                return -1;
            }
            config->log_level = argv[i];
        }
        else {
            fprintf(stderr, "Error: Unknown option '%s'\n", arg);
            fprintf(stderr, "Use --help for usage information\n");
            return -1;
        }
    }
    
    return 0;
}

/**
 * Main entry point
 */
int main(int argc, char **argv) {
    int ret = 0;
    int parse_result;
    signaling_server_config_t config;
    signaling_server_config_t cli_probe;
    
    /* Initialize default configuration */
    signaling_server_config_init(&config);
    signaling_server_config_init(&cli_probe);
    
    /*
     * Probe the complete command line first so help/errors have no file I/O.
     * Applying it again after TOML gives CLI values the documented priority.
     */
    parse_result = parse_args(argc, argv, &cli_probe);
    if (parse_result != 0) {
        return parse_result > 0 ? 0 : 1;
    }
    
    /* Load configuration file if specified */
    if (cli_probe.config_file) {
        TLOG_INFOF("Loading configuration from: {}", cli_probe.config_file);
        if (signaling_server_config_load(&config, cli_probe.config_file) != 0) {
            TLOG_ERRORF("Failed to load configuration file: {}", cli_probe.config_file);
            ret = 1;
            goto cleanup;
        }
    }

    signaling_server_config_apply_environment(&config);
    parse_result = parse_args(argc, argv, &config);
    if (parse_result != 0 || signaling_server_config_validate(&config) != 0) {
        ret = 1;
        goto cleanup;
    }
    
    /* Initialize logging */
    tlog_set_level(tlog_get_default(), turbo_log_level_from_name(config.log_level));
    
    /* Print startup banner */
    TLOG_INFO("=================================================");
    TLOG_INFOF("TurboNet WebRTC Signaling Server v{}", SIGNALING_SERVER_VERSION);
    TLOG_INFO("=================================================");
    TLOG_INFOF("Node ID: {}", config.node_id ? config.node_id : "auto");
    TLOG_INFOF("WebSocket: {}:{}", config.ws_host, config.ws_port);
    if (config.http_enabled) {
        TLOG_INFOF("HTTP API: {}:{}", config.http_host, config.http_port);
        TLOG_INFO("HTTP management auth: enabled");
    } else {
        TLOG_INFO("HTTP API: disabled");
    }
    TLOG_INFOF("Max Peers: {}", config.max_peers);
    TLOG_INFOF("Max Rooms: {}", config.max_rooms);
    TLOG_INFOF("Redis: {}:{} ({})",
              config.redis_host, 
              config.redis_port,
              config.redis_enabled ? "enabled" : "disabled");
    TLOG_INFOF("Peer admission auth: {}",
              config.jwt_enabled ? "enabled" : "disabled");
    TLOG_INFOF("Log Level: {}", config.log_level);
    TLOG_INFO("=================================================");
    
    /* Setup signal handlers */
    setup_signal_handlers();
    
    /* Create server instance */
    g_server = signaling_server_create(&config);
    if (!g_server) {
        TLOG_ERROR("Failed to create signaling server");
        ret = 1;
        goto cleanup;
    }
    
    /* Start server */
    TLOG_INFO("Starting signaling server...");
    if (signaling_server_start(g_server) != 0) {
        TLOG_ERROR("Failed to start signaling server");
        ret = 1;
        goto cleanup;
    }
    
    TLOG_INFO("Signaling server started successfully");
    TLOG_INFO("Press Ctrl+C to stop");
    
    /* Run server (blocks until stopped) */
    ret = signaling_server_run(g_server);
    
    TLOG_INFO("Server stopped");
    
cleanup:
    /* Cleanup */
    if (g_server) {
        signaling_server_destroy(g_server);
        g_server = NULL;
    }
    
    signaling_server_config_cleanup(&config);
    
    TLOG_INFO("Shutdown complete");
    
    return ret;
}
