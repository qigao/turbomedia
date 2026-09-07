/**
 * @file signaling_server_example.c
 * @brief WebRTC Signaling Server Example
 *
 * Simple WebSocket-based signaling server for WebRTC connections.
 *
 * Usage: signaling_server [port]
 * Default port: 8080
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h> 

#include "webrtc_signaling.h"
#include "tlog.h"
#include <salts/thread.h>
#include <platform.h>

static volatile int g_running = 1;
static webrtc_signaling_server_t *g_server = NULL;
enum { SIGNALING_SERVER_STATUS_INTERVAL_MS = 5000 };

static void signal_handler(int sig) {
    (void)sig;
    TLOG_INFO("Shutting down...");
    g_running = 0;
}

int main(int argc, char *argv[]) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    TLOG_INFO("=== TurboMedia WebRTC Signaling Server ===");

    /* Configure signaling server */
    webrtc_signaling_config_t config = {
        .host = "0.0.0.0",
        .port = port,
        .use_tls = 0,
        .connection_capacity = 128,
        .max_peers = 100,
        .peer_timeout_ms = 60000,
        .join_timeout_ms = 10000,
        .max_message_size = 65536,
        .messages_per_second = 100,
        .message_burst = 200,
        .max_outbox_messages = 256,
        .max_outbox_bytes = 1048576,
        .max_connections_per_source = 100,
        .source_admissions_per_second = 20,
        .source_admission_burst = 50,
        .max_source_states = 4096,
        .source_state_ttl_ms = 300000
    };

    /* Create signaling server */
    g_server = webrtc_signaling_create(NULL, &config);
    if (!g_server) {
        TLOG_ERROR("Failed to create signaling server");
        return 1;
    }

    /* Start server */
    if (webrtc_signaling_start(g_server) < 0) {
        TLOG_ERROR("Failed to start signaling server");
        webrtc_signaling_destroy(g_server);
        return 1;
    }

    TLOG_INFO("Signaling server started successfully!");
    TLOG_INFOF("WebSocket URL: ws://0.0.0.0:{}", port);
    TLOG_INFO("Protocol: webrtc-signaling");
    TLOG_INFO("Press Ctrl+C to stop...");

    /* CHTTP owns its network thread; this loop only reports status. */
    uint64_t last_check = salts_monotonic_ms();
    while (g_running) {
        uint64_t now;
        salts_sleep_ms(100);
        now = salts_monotonic_ms();
        if (now - last_check >= SIGNALING_SERVER_STATUS_INTERVAL_MS) {
            int peer_count = webrtc_signaling_get_peer_count(g_server);
            TLOG_INFOF("Active peers: {}", peer_count);
            last_check = now;
        }
    }

    /* Cleanup */
    TLOG_INFO("Shutting down gracefully...");
    webrtc_signaling_stop(g_server);
    webrtc_signaling_destroy(g_server);

    TLOG_INFO("Server stopped.");
    return 0;
}
