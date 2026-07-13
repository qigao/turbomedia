/**
 * simple_echo.c - Simple P2P echo example using WebRTC DataChannel
 *
 * This example demonstrates:
 * - Creating a peer-to-peer connection (no signaling server)
 * - Opening a data channel
 * - Sending and receiving messages
 *
 * Usage:
 *   Terminal 1: ./simple_echo server 127.0.0.1 5000
 *   Terminal 2: ./simple_echo client 127.0.0.1 5000
 */

#include "turbo_datachannel.h"
#include "turbo_datachannel_errors.h"
#include "tlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#ifdef _WIN32
  #include <windows.h>
  #define SLEEP_MS(ms) Sleep(ms)
#else
  #include <unistd.h>
  #define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

typedef struct {
  turbo_dc_context_t *ctx;
  turbo_dc_peer_t *peer;
  turbo_dc_channel_t *channel;
  int is_server;
  int messages_received;
  int running;
} app_state_t;

/* ============================================================================
 * Callbacks
 * ============================================================================ */

static void on_channel_message(turbo_dc_channel_t *channel, const void *data, size_t len,
                               int is_binary, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)channel;
  (void)is_binary;

    char msg_buf[512];
    int pr_len = (int)len > 511 ? 511 : (int)len;
    snprintf(msg_buf, sizeof(msg_buf), "%.*s", pr_len, (const char *)data);
    TLOG_INFO("Received: {}", msg_buf);

  app->messages_received++;

  /* Echo back if server */
  if (app->is_server) {
    TLOG_INFO("Echoing back...");
    turbo_dc_channel_send(channel, data, len, 0);
  }
}

static void on_channel_open(turbo_dc_channel_t *channel, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;

  TLOG_INFO("Channel '{}' opened!", turbo_dc_channel_get_label(channel));

  /* Client sends first message */
  if (!app->is_server) {
    const char *msg = "Hello from client!";
    TLOG_INFO("Sending: {}", msg);
    turbo_dc_channel_send(channel, msg, strlen(msg), 0);
  }
}

static void on_channel_close(turbo_dc_channel_t *channel, void *user_data) {
  (void)channel;
  (void)user_data;
  TLOG_INFO("Channel closed");
}

static void on_peer_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)peer;

  TLOG_INFO("Incoming channel: {}", turbo_dc_channel_get_label(channel));

  app->channel = channel;
  turbo_dc_channel_set_user_data(channel, app);
  turbo_dc_channel_on_message(channel, on_channel_message);
  turbo_dc_channel_on_close(channel, on_channel_close);
}

static void on_peer_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                          turbo_dc_state_t new_state, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)peer;
  (void)old_state;

  TLOG_INFO("State: {} -> {}", ENUM_NAME(old_state), ENUM_NAME(new_state));

  if (new_state == TURBO_DC_STATE_CLOSED || new_state == TURBO_DC_STATE_FAILED) {
    app->running = 0;
  }

  if (new_state == TURBO_DC_STATE_CONNECTED) {
    TLOG_INFO("Peer connected!");

    /* Client creates and opens channel */
    if (!app->is_server) {
      app->channel = turbo_dc_channel_create(app->peer, "echo", NULL);
      if (!app->channel) {
        TLOG_ERROR("Failed to create channel");
        return;
      }

      turbo_dc_channel_on_open(app->channel, on_channel_open);
      turbo_dc_channel_on_message(app->channel, on_channel_message);
      turbo_dc_channel_on_close(app->channel, on_channel_close);
      turbo_dc_channel_set_user_data(app->channel, app);

      if (turbo_dc_channel_open(app->channel) != 0) {
        turbo_dc_error_t err = turbo_dc_peer_get_error(app->peer);
        TLOG_ERROR("Failed to open channel: {}", turbo_dc_error_string(err.code));
      }
    }
  }
}

static void on_peer_error(turbo_dc_peer_t *peer, int error_code, const char *error_msg,
                          void *user_data) {
  (void)peer;
  (void)user_data;
  TLOG_ERROR("Peer error {}: {}", error_code, error_msg);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
  if (argc != 4) {
    TLOG_ERROR("Usage: {} <server|client> <host> <port>", argv[0]);
    return 1;
  }

  const char *mode = argv[1];
  const char *host = argv[2];
  int port = atoi(argv[3]);

  int is_server = (strcmp(mode, "server") == 0);

  TLOG_INFO("=== WebRTC DataChannel Simple Echo Example ===");
  TLOG_INFO("Mode: {}", is_server ? "SERVER" : "CLIENT");
  TLOG_INFO("Address: {}:{}", host, port);

  /* Initialize app state */
  app_state_t app = {0};
  app.is_server = is_server;
  app.running = 1;

  /* Create context */
  turbo_dc_config_t config = {.is_server = is_server, .cert_pem = NULL, .key_pem = NULL};

  app.ctx = turbo_dc_context_create(&config);
  if (!app.ctx) {
    TLOG_ERROR("Failed to create context");
    return 1;
  }

  /* Create peer */
  app.peer = turbo_dc_peer_create(app.ctx, host, (uint16_t)port, &app);
  if (!app.peer) {
    turbo_dc_error_t err = turbo_dc_context_get_error(app.ctx);
    TLOG_ERROR("Failed to create peer: {}", turbo_dc_error_string(err.code));
    turbo_dc_context_destroy(app.ctx);
    return 1;
  }

  /* Set callbacks */
  turbo_dc_peer_on_state(app.peer, on_peer_state);
  turbo_dc_peer_on_channel(app.peer, on_peer_channel);
  turbo_dc_peer_on_error(app.peer, on_peer_error);

  /* Connect */
  if (turbo_dc_peer_connect(app.peer) != 0) {
    turbo_dc_error_t err = turbo_dc_peer_get_error(app.peer);
    TLOG_ERROR("Failed to connect: {}", turbo_dc_error_string(err.code));
    turbo_dc_peer_destroy(app.peer);
    turbo_dc_context_destroy(app.ctx);
    return 1;
  }

  TLOG_INFO("Waiting for connection... (Press Ctrl+C to exit)");

  /* Wait for connection/disconnection */
  while (app.running) {
    SLEEP_MS(100);
  }

  /* Cleanup */
  turbo_dc_peer_destroy(app.peer);
  turbo_dc_context_destroy(app.ctx);

  TLOG_INFO("Received {} messages", app.messages_received);
  TLOG_INFO("Done!");

  return 0;
}
