/**
 * file_transfer.c - P2P file transfer using WebRTC DataChannel
 *
 * This example demonstrates:
 * - Chunked file transfer over DataChannel
 * - Progress tracking
 * - Binary data handling
 *
 * Usage:
 *   Terminal 1 (receiver): ./file_transfer server 127.0.0.1 5000 output.bin
 *   Terminal 2 (sender):   ./file_transfer client 127.0.0.1 5000 input.bin
 */

#include "tlog.h"
#include "turbo_datachannel.h"
#include "turbo_datachannel_errors.h"
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

#define CHUNK_SIZE 16384 /* 16KB chunks */

typedef struct {
  turbo_dc_context_t *ctx;
  turbo_dc_peer_t *peer;
  turbo_dc_channel_t *channel;
  int is_server;
  int running;

  /* File info */
  FILE *file;
  const char *filename;
  size_t file_size;
  size_t bytes_transferred;

  /* Transfer state */
  int transfer_active;
  int transfer_complete;
} app_state_t;

/* ============================================================================
 * File Transfer Logic
 * ============================================================================ */

static void send_next_chunk(app_state_t *app) {
  if (!app->channel || !turbo_dc_channel_is_open(app->channel)) {
    return;
  }

  uint8_t buffer[CHUNK_SIZE];
  size_t read = fread(buffer, 1, CHUNK_SIZE, app->file);

  if (read > 0) {
    if (turbo_dc_channel_send(app->channel, buffer, read, 1) != 0) {
      turbo_dc_error_t err = turbo_dc_peer_get_error(app->peer);
      TLOG_ERRORF("Failed to send chunk: {}", turbo_dc_error_string(err.code));
      app->transfer_active = 0;
      return;
    }

    app->bytes_transferred += read;

    float progress = (float)app->bytes_transferred / app->file_size * 100.0f;
    /* Use printf for progress, but minimal updates or just log periodically?
     * Since this is a CLI, printf/fflush is acceptable for a progress bar, but log_info is better
     * for structured logs. Let's use printf for the progress bar itself since it uses \r, which
     * log_info doesn't support well. */
    printf("\r[SENDER] Progress: %.1f%% (%zu/%zu bytes)", progress, app->bytes_transferred,
           app->file_size);
    fflush(stdout);
  }

  if (read < CHUNK_SIZE || feof(app->file)) {
    TLOG_INFO("Transfer complete!");
    app->transfer_active = 0;
    app->transfer_complete = 1;
    fclose(app->file);
    app->file = NULL;
  }
}

static void start_file_transfer(app_state_t *app) {
  app->file = fopen(app->filename, "rb");
  if (!app->file) {
    TLOG_ERRORF("Failed to open file: {}", app->filename);
    return;
  }

  /* Get file size */
  fseek(app->file, 0, SEEK_END);
  app->file_size = ftell(app->file);
  fseek(app->file, 0, SEEK_SET);

  TLOG_INFOF("Transferring {} ({} bytes)", app->filename, app->file_size);

  app->bytes_transferred = 0;
  app->transfer_active = 1;
}

/* ============================================================================
 * Callbacks
 * ============================================================================ */

static void on_channel_message(turbo_dc_channel_t *channel, const void *data, size_t len,
                               int is_binary, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)channel;

  if (!is_binary) {
    TLOG_WARN("Unexpected text message");
    return;
  }

  /* Write received data to file */
  if (app->file) {
    size_t written = fwrite(data, 1, len, app->file);
    if (written != len) {
      TLOG_ERROR("Write error");
      return;
    }

    app->bytes_transferred += written;

    /* Show progress */
    /* Show progress with printf because of \r */
    printf("\r[RECEIVER] Received: %zu bytes", app->bytes_transferred);
    fflush(stdout);
  }
}

static void on_channel_open(turbo_dc_channel_t *channel, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;

  TLOG_INFOF("Channel '{}' opened! ({})", turbo_dc_channel_get_label(channel),
            app->is_server ? "RECEIVER" : "SENDER");

  /* Sender starts transfer */
  if (!app->is_server) {
    start_file_transfer(app);
  } else {
    /* Receiver prepares to receive */
    app->file = fopen(app->filename, "wb");
    if (!app->file) {
      TLOG_ERRORF("Failed to create output file: {}", app->filename);
      return;
    }
    TLOG_INFO("Ready to receive...");
  }
}

static void on_channel_close(turbo_dc_channel_t *channel, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)channel;

  TLOG_INFOF("Channel closed ({})", app->is_server ? "RECEIVER" : "SENDER");

  if (app->file) {
    fclose(app->file);
    app->file = NULL;
  }
  app->running = 0;
}

static void on_peer_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)peer;

  TLOG_INFOF("Incoming channel: {}", turbo_dc_channel_get_label(channel));

  app->channel = channel;
  turbo_dc_channel_set_user_data(channel, app);
  turbo_dc_channel_on_message(channel, on_channel_message);
  turbo_dc_channel_on_close(channel, on_channel_close);

  /* Channel is already open when we receive it - prepare to receive file */
  app->file = fopen(app->filename, "wb");
  if (!app->file) {
    TLOG_ERRORF("Failed to create output file: {}", app->filename);
    return;
  }
  TLOG_INFO("Ready to receive...");
}

static void on_peer_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                          turbo_dc_state_t new_state, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)peer;
  (void)old_state;

  TLOG_INFOF("State: {} -> {} ({})", ENUM_NAME(old_state), ENUM_NAME(new_state),
            app->is_server ? "RECEIVER" : "SENDER");

  if (new_state == TURBO_DC_STATE_CLOSED || new_state == TURBO_DC_STATE_FAILED) {
    app->running = 0;
  }

  if (new_state == TURBO_DC_STATE_CONNECTED) {
    TLOG_INFOF("Peer connected! ({})", app->is_server ? "RECEIVER" : "SENDER");

    /* Sender creates channel */
    if (!app->is_server) {
      turbo_dc_channel_config_t config = turbo_dc_default_channel_config();
      config.ordered = 1; /* Ordered delivery for file transfer */

      app->channel = turbo_dc_channel_create(app->peer, "file-transfer", &config);
      if (!app->channel) {
        TLOG_ERROR("Failed to create channel");
        return;
      }

      turbo_dc_channel_set_user_data(app->channel, app);
      turbo_dc_channel_on_open(app->channel, on_channel_open);
      turbo_dc_channel_on_message(app->channel, on_channel_message);
      turbo_dc_channel_on_close(app->channel, on_channel_close);

      if (turbo_dc_channel_open(app->channel) != 0) {
        turbo_dc_error_t err = turbo_dc_peer_get_error(app->peer);
        TLOG_ERRORF("Failed to open channel: {}", turbo_dc_error_string(err.code));
      }
    }
  }
}

static void on_peer_error(turbo_dc_peer_t *peer, int error_code, const char *error_msg,
                          void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)peer;
  app->running = 0;
  TLOG_ERRORF("Peer error {}: {}", error_code, error_msg);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
  if (argc != 5) {
    TLOG_ERRORF("Usage (sender):   {} client <host> <port> <input_file>", argv[0]);
    TLOG_ERRORF("Usage (receiver): {} server <host> <port> <output_file>", argv[0]);
    return 1;
  }

  const char *mode = argv[1];
  const char *host = argv[2];
  int port = atoi(argv[3]);
  const char *filename = argv[4];

  int is_server = (strcmp(mode, "server") == 0);

  TLOG_INFO("=== WebRTC DataChannel File Transfer ===");
  TLOG_INFOF("Mode: {}", is_server ? "RECEIVER" : "SENDER");
  TLOG_INFOF("Address: {}:{}", host, port);
  TLOG_INFOF("File: {}", filename);

  /* Initialize app state */
  app_state_t app = {0};
  app.is_server = is_server;
  app.filename = filename;
  app.running = 1;

  /* For demo: assume file size is known (in real app, send metadata first) */
  if (!is_server) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
      TLOG_ERRORF("Cannot open input file: {}", filename);
      return 1;
    }
    fseek(f, 0, SEEK_END);
    app.file_size = ftell(f);
    fclose(f);
  } else {
    /* In real app, receive file size in metadata message */
    app.file_size = 1024 * 1024; /* Dummy value for demo */
  }

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
    TLOG_ERRORF("Failed to create peer: {}", turbo_dc_error_string(err.code));
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
    TLOG_ERRORF("Failed to connect: {}", turbo_dc_error_string(err.code));
    turbo_dc_peer_destroy(app.peer);
    turbo_dc_context_destroy(app.ctx);
    return 1;
  }

  TLOG_INFO("Waiting for connection... (Press Ctrl+C to exit)");

  /* Main loop */
  while (app.running) {
    /* If transfer is active, send chunks */
    if (app.transfer_active && app.file) {
      send_next_chunk(&app);
    }
    SLEEP_MS(10);
  }

  /* Cleanup */
  if (app.file) {
    fclose(app.file);
  }
  turbo_dc_peer_destroy(app.peer);
  turbo_dc_context_destroy(app.ctx);

  TLOG_INFO("Done!");

  return 0;
}
