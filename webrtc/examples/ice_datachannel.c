/**
 * ice_datachannel.c - WebRTC DataChannel with ICE NAT Traversal
 *
 * This example demonstrates:
 * - ICE candidate gathering (host + server-reflexive)
 * - Candidate exchange via console (simulating signaling)
 * - DataChannel over ICE transport
 *
 * Usage:
 *   Terminal 1: ./ice_datachannel offer
 *   Terminal 2: ./ice_datachannel answer
 *
 * Then exchange ICE credentials and candidates via copy/paste.
 */

#include "ice_integration.h"
#include "tlog.h"
#include "turbo_datachannel.h"
#include <salts_thread.h>
#include <stb_sprintf.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  /* ICE */
  ice_integration_ctx_t *ice;
  atomic_int ice_connected;

  /* DataChannel */
  turbo_dc_context_t *dc_ctx;
  turbo_dc_peer_t *dc_peer;
  _Atomic(turbo_dc_channel_t *) channel;

  int is_offerer;
  atomic_int running;
} app_state_t;

/* ============================================================================
 * ICE Callbacks
 * ============================================================================ */

static void on_ice_state(ice_state_t state, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;

  TLOG_INFOF("ICE state: {}", ENUM_NAME(state));

  if (state == ICE_STATE_CONNECTED || state == ICE_STATE_COMPLETED) {
    atomic_store(&app->ice_connected, 1);
  } else if (state == ICE_STATE_FAILED) {
    TLOG_ERROR("ICE failed!");
    atomic_store(&app->running, 0);
  }
}

static void on_ice_candidate(const char *candidate_sdp, void *user_data) {
  (void)user_data;
  TLOG_INFOF("Local candidate: {}", candidate_sdp);
}

/* ============================================================================
 * DataChannel Callbacks
 * ============================================================================ */

static void on_dc_message(turbo_dc_channel_t *channel, const void *data, size_t len, int is_binary,
                          void *user_data) {
  (void)user_data;
  (void)is_binary;

  char msg_buf[512];
  int pr_len = (int)len > 511 ? 511 : (int)len;
  stbsp_snprintf(msg_buf, sizeof(msg_buf), "%.*s", pr_len, (const char *)data);
  TLOG_INFOF("Received: {}", msg_buf);

  /* Don't echo messages that are already echoes (prevent infinite loop) */
  if (len >= 5 && strncmp((const char *)data, "ECHO:", 5) == 0) {
    return;
  }

  /* Echo back with prefix */
  char echo[512];
  int echo_len = stbsp_snprintf(echo, sizeof(echo), "ECHO: %.*s", (int)len, (const char *)data);
  if (echo_len > 0 && echo_len < (int)sizeof(echo)) {
    turbo_dc_channel_send(channel, echo, (size_t)echo_len, 0);
    TLOG_INFOF("Sent echo: {}", echo);
  }
}

static void on_dc_open(turbo_dc_channel_t *channel, void *user_data) {
  (void)user_data;
  TLOG_INFOF("Channel '{}' opened!", turbo_dc_channel_get_label(channel));
}

static void on_dc_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                        turbo_dc_state_t new_state, void *user_data) {
  (void)peer;
  (void)old_state;
  app_state_t *app = (app_state_t *)user_data;

  TLOG_INFOF("DC state: {} -> {}", ENUM_NAME(old_state), ENUM_NAME(new_state));

  if (new_state == TURBO_DC_STATE_CONNECTED) {
    TLOG_INFO("DataChannel connected!");

    if (app->is_offerer) {
      /* Create and open channel */
      turbo_dc_channel_t *channel = turbo_dc_channel_create(app->dc_peer, "test", NULL);
      if (channel) {
        turbo_dc_channel_on_open(channel, on_dc_open);
        turbo_dc_channel_on_message(channel, on_dc_message);
        atomic_store(&app->channel, channel);
        turbo_dc_channel_open(channel);
      }
    }
  } else if (new_state == TURBO_DC_STATE_CLOSED || new_state == TURBO_DC_STATE_FAILED) {
    atomic_store(&app->running, 0);
  }
}

static void on_dc_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel, void *user_data) {
  (void)peer;
  app_state_t *app = (app_state_t *)user_data;

  TLOG_INFOF("Incoming channel: {}", turbo_dc_channel_get_label(channel));
  atomic_store(&app->channel, channel);

  /* Set callbacks for incoming channel */
  turbo_dc_channel_on_open(channel, on_dc_open);
  turbo_dc_channel_on_message(channel, on_dc_message);
}

static void on_dc_error(turbo_dc_peer_t *peer, int code, const char *msg, void *user_data) {
  (void)peer;
  (void)user_data;
  TLOG_ERRORF("DC error {}: {}", code, msg);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char **argv) {
  if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    printf("Usage: %s <offer|answer>\n", argv[0]);
    printf("Runs the console ICE/DataChannel interoperability example.\n");
    return 0;
  }

  if (argc != 2 || (strcmp(argv[1], "offer") != 0 && strcmp(argv[1], "answer") != 0)) {
    fprintf(stderr, "Usage: %s <offer|answer>\n", argv[0]);
    return 1;
  }

  int is_offerer = (strcmp(argv[1], "offer") == 0);

  TLOG_INFO("=== WebRTC DataChannel + ICE Example ===");
  TLOG_INFOF("Mode: {}", is_offerer ? "OFFERER" : "ANSWERER");

  app_state_t app = {0};
  app.is_offerer = is_offerer;
  atomic_init(&app.running, 1);
  atomic_init(&app.ice_connected, 0);
  atomic_init(&app.channel, NULL);

  /* ========== Create DataChannel Context ========== */
  turbo_dc_config_t dc_config = {.is_server = !is_offerer, /* Answerer is DTLS server */
                                 .transport = TURBO_DC_TRANSPORT_ICE};

  app.dc_ctx = turbo_dc_context_create(&dc_config);
  if (!app.dc_ctx) {
    TLOG_ERROR("Failed to create DC context");
    return 1;
  }

  app.dc_peer = turbo_dc_peer_create(app.dc_ctx, NULL, 0, &app);
  if (!app.dc_peer) {
    TLOG_ERROR("Failed to create DC peer");
    turbo_dc_context_destroy(app.dc_ctx);
    return 1;
  }

  turbo_dc_peer_on_state(app.dc_peer, on_dc_state);
  turbo_dc_peer_on_channel(app.dc_peer, on_dc_channel);
  turbo_dc_peer_on_error(app.dc_peer, on_dc_error);

  const char *stun_servers[] = {"stun:stun.l.google.com:19302"};
  app.ice = ice_integration_create(app.dc_peer, NULL, stun_servers, 1,
                                   NULL, NULL, NULL, 0);
  if (!app.ice) {
    TLOG_ERROR("Failed to attach SaltsNet ICE transport to DataChannel peer");
    turbo_dc_peer_destroy(app.dc_peer);
    turbo_dc_context_destroy(app.dc_ctx);
    return 1;
  }
  ice_integration_on_state_change(app.ice, on_ice_state, &app);
  ice_integration_on_candidate(app.ice, on_ice_candidate, &app);

  /* ========== Start ICE Gathering ========== */
  TLOG_INFO("Starting ICE gathering...");
  if (ice_integration_start_gathering(app.ice) != 0) {
    TLOG_ERROR("Failed to start ICE gathering");
    ice_integration_destroy(app.ice);
    turbo_dc_peer_destroy(app.dc_peer);
    turbo_dc_context_destroy(app.dc_ctx);
    return 1;
  }

  /* Get local credentials */
  char local_ufrag[32], local_pwd[64];
  if (ice_integration_get_local_credentials(app.ice, local_ufrag, sizeof(local_ufrag),
                                            local_pwd, sizeof(local_pwd)) != 0) {
    TLOG_ERROR("Failed to get local ICE credentials");
    ice_integration_destroy(app.ice);
    turbo_dc_peer_destroy(app.dc_peer);
    turbo_dc_context_destroy(app.dc_ctx);
    return 1;
  }
  TLOG_INFO("=== Local ICE Credentials ===");
  TLOG_INFOF("ufrag: {}", local_ufrag);
  TLOG_INFOF("pwd: {}", local_pwd);

  /* Wait for gathering */
  TLOG_INFO("Waiting for candidate gathering...");
  while (!ice_integration_is_gathering_complete(app.ice)) {
    ice_integration_poll(app.ice);
    salts_sleep_ms(100u);
  }



  /* ========== Exchange Credentials (Manual) ========== */
  printf("\n=== Enter Remote ICE Credentials ===\n");
  char remote_ufrag[32], remote_pwd[64];
  printf("Remote ufrag: ");
  if (scanf("%31s", remote_ufrag) != 1) {
    atomic_store(&app.running, 0);
  }
  printf("Remote pwd: ");
  if (atomic_load(&app.running) && scanf("%63s", remote_pwd) != 1) {
    atomic_store(&app.running, 0);
  }

  /* Consume leftover newline from scanf */
  int ch;
  while ((ch = getchar()) != '\n' && ch != EOF)
    ;

  if (atomic_load(&app.running) &&
      ice_integration_set_remote_credentials(app.ice, remote_ufrag, remote_pwd) != 0) {
    TLOG_ERROR("Failed to set remote ICE credentials");
    atomic_store(&app.running, 0);
  }

  /* Add remote candidates */
  printf("\nEnter remote candidates (one per line, empty line to finish):\n");
  char line[256];
  while (atomic_load(&app.running) && fgets(line, sizeof(line), stdin)) {
    if (line[0] == '\n' || line[0] == '\0')
      break;
    line[strcspn(line, "\n")] = 0; /* Remove newline */
    if (strlen(line) > 0) {
      if (ice_integration_add_remote_candidate(app.ice, line) != 0) {
        TLOG_ERRORF("Failed to add remote ICE candidate: {}", line);
        atomic_store(&app.running, 0);
        break;
      }
      printf("Added: %s\n", line);
    }
  }
  if (atomic_load(&app.running)) {
    TLOG_INFO("Starting ICE connectivity checks...");
    ice_integration_end_of_candidates(app.ice);
  }

  /* ========== Main Loop ========== */
  TLOG_INFO("Running... (Press Ctrl+C to exit)");
  while (atomic_load(&app.running)) {
    ice_integration_poll(app.ice);

    /* Process SCTP timers - required for message delivery */
    turbo_dc_handle_timers();

    salts_sleep_ms(10u);

    /* Send test message if channel is open */
    turbo_dc_channel_t *channel = atomic_load(&app.channel);
    if (channel && turbo_dc_channel_is_open(channel)) {
      static int sent = 0;
      if (!sent) {
        const char *msg = "Hello via ICE!";
        TLOG_INFOF("Sending: {}", msg);
        turbo_dc_channel_send(channel, msg, strlen(msg), 0);
        sent = 1;
      }
    }
  }

  /* Cleanup */
  ice_integration_destroy(app.ice);
  turbo_dc_peer_destroy(app.dc_peer);
  turbo_dc_context_destroy(app.dc_ctx);

  TLOG_INFO("Done!");
  return atomic_load(&app.ice_connected) ? 0 : 1;
}
