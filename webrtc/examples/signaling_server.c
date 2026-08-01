/**
 * @file signaling_server_example.c
 * @brief WebRTC Signaling Server Example
 *
 * Simple WebSocket-based signaling server for WebRTC connections.
 *
 * Usage: signaling_server [port]
 * Default port: 8080
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NORPC
#define NOSERVICE
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <signal.h> 

#include "webrtc_signaling.h"
#include "tlog.h"
#include <turbo_coro_context.h>

static volatile int g_running = 1;
static webrtc_signaling_server_t *g_server = NULL;
enum { SIGNALING_SERVER_ACTIVE_BURST = 32 };

static void signaling_server_idle_yield(void) {
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
#endif
}

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

    TLOG_INFO("=== TurboNet WebRTC Signaling Server ===");

    /* Create event loop */
    turbo_loop_t *loop = turbo_loop_create();
    if (!loop) {
        TLOG_ERROR("Failed to create event loop");
        return 1;
    }

    /* Configure signaling server */
    webrtc_signaling_config_t config = {
        .host = "0.0.0.0",
        .port = port,
        .use_tls = 0,
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
    g_server = webrtc_signaling_create(loop, &config);
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
    TLOG_INFO("WebSocket URL: ws://0.0.0.0:{}", port);
    TLOG_INFO("Protocol: webrtc-signaling");
    TLOG_INFO("Press Ctrl+C to stop...");

    /* Run event loop */
    while (g_running) {
        int active_handles = 0;
        int burst = 0;
        do {
            active_handles = webrtc_signaling_run(g_server, TURBO_RUN_ONCE);
            burst++;
        } while (g_running && active_handles != 0 && burst < SIGNALING_SERVER_ACTIVE_BURST);
        if (active_handles == 0) {
            signaling_server_idle_yield();
        }
        /* Check peer count periodically */
        static uint64_t last_check = 0;
        uint64_t now = turbo_loop_now(loop);
        if (now - last_check > 5000) {
            int peer_count = webrtc_signaling_get_peer_count(g_server);
            TLOG_INFO("Active peers: {}", peer_count);
            last_check = now;
        }
    }

    /* Cleanup */
    TLOG_INFO("Shutting down gracefully...");
    webrtc_signaling_stop(g_server);
    webrtc_signaling_destroy(g_server);

    /* Close loop */
    turbo_loop_destroy(loop);

    TLOG_INFO("Server stopped.");
    return 0;
}
