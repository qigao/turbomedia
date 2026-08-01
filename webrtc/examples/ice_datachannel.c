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

#include "ice/turbo_ice.h"
#include "tlog.h"
#include "turbo_datachannel.h"
#include <turbo_coro_context.h>
#include <stb_sprintf.h>
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
  turbo_loop_t *loop;
  coro_context_t *ice_ctx;

  /* ICE */
  turbo_ice_agent_t *ice_agent;
  int ice_connected;

  /* DataChannel */
  turbo_dc_context_t *dc_ctx;
  turbo_dc_peer_t *dc_peer;
  turbo_dc_channel_t *channel;

  int is_offerer;
  int running;
  int messages_received;
} app_state_t;

/* Global for ICE callback access */
static app_state_t *g_app = NULL;

/* ============================================================================
 * ICE Callbacks
 * ============================================================================ */

static void on_ice_state(turbo_ice_agent_t *agent, ice_state_t old_state, ice_state_t new_state,
                         void *user_data) {
  (void)agent;
  (void)old_state;
  app_state_t *app = (app_state_t *)user_data;

  TLOG_INFO("ICE State: {} -> {}", ENUM_NAME(old_state), ENUM_NAME(new_state));

  if (new_state == ICE_STATE_CONNECTED) {
    TLOG_INFO("ICE connected! Starting DTLS...");
    app->ice_connected = 1;

    /* Now start DataChannel connection */
    turbo_dc_peer_connect(app->dc_peer);
  } else if (new_state == ICE_STATE_FAILED) {
    TLOG_ERROR("ICE failed!");
    app->running = 0;
  }
}

static void on_ice_gathering(turbo_ice_agent_t *agent, ice_gathering_state_t state,
                             void *user_data) {
  (void)agent;
  (void)user_data;

  if (state == ICE_GATHERING_COMPLETE) {
    TLOG_INFO("ICE gathering complete!");
  }
}

static void on_ice_candidate(turbo_ice_agent_t *agent, const ice_candidate_t *candidate,
                             void *user_data) {
  (void)agent;
  (void)user_data;

  char sdp[256];
  ice_candidate_to_sdp(candidate, sdp, sizeof(sdp));
  TLOG_INFO("Local candidate: {}", sdp);
}

static void on_ice_data(turbo_ice_agent_t *agent, const void *data, size_t len, void *user_data) {
  (void)agent;
  app_state_t *app = (app_state_t *)user_data;

  /* Feed ICE data to DataChannel */
  turbo_dc_peer_feed_ice_data(app->dc_peer, data, len);
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
  TLOG_INFO("Received: {}", msg_buf);

  /* Don't echo messages that are already echoes (prevent infinite loop) */
  if (len >= 5 && strncmp((const char *)data, "ECHO:", 5) == 0) {
    return;
  }

  /* Echo back with prefix */
  char echo[512];
  int echo_len = stbsp_snprintf(echo, sizeof(echo), "ECHO: %.*s", (int)len, (const char *)data);
  if (echo_len > 0 && echo_len < (int)sizeof(echo)) {
    turbo_dc_channel_send(channel, echo, (size_t)echo_len, 0);
    TLOG_INFO("Sent echo: {}", echo);
  }
}

static void on_dc_open(turbo_dc_channel_t *channel, void *user_data) {
  (void)user_data;
  TLOG_INFO("Channel '{}' opened!", turbo_dc_channel_get_label(channel));
}

static void on_dc_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                        turbo_dc_state_t new_state, void *user_data) {
  (void)peer;
  (void)old_state;
  app_state_t *app = (app_state_t *)user_data;

  TLOG_INFO("DC state: {} -> {}", ENUM_NAME(old_state), ENUM_NAME(new_state));

  if (new_state == TURBO_DC_STATE_CONNECTED) {
    TLOG_INFO("DataChannel connected!");

    if (app->is_offerer) {
      /* Create and open channel */
      app->channel = turbo_dc_channel_create(app->dc_peer, "test", NULL);
      if (app->channel) {
        turbo_dc_channel_on_open(app->channel, on_dc_open);
        turbo_dc_channel_on_message(app->channel, on_dc_message);
        turbo_dc_channel_open(app->channel);
      }
    }
  } else if (new_state == TURBO_DC_STATE_CLOSED || new_state == TURBO_DC_STATE_FAILED) {
    app->running = 0;
  }
}

static void on_dc_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel, void *user_data) {
  (void)peer;
  app_state_t *app = (app_state_t *)user_data;

  TLOG_INFO("Incoming channel: {}", turbo_dc_channel_get_label(channel));
  app->channel = channel;

  /* Set callbacks for incoming channel */
  turbo_dc_channel_on_open(channel, on_dc_open);
  turbo_dc_channel_on_message(channel, on_dc_message);
}

static void on_dc_error(turbo_dc_peer_t *peer, int code, const char *msg, void *user_data) {
  (void)peer;
  (void)user_data;
  TLOG_ERROR("DC error {}: {}", code, msg);
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
  TLOG_INFO("Mode: {}", is_offerer ? "OFFERER" : "ANSWERER");

  app_state_t app = {0};
  app.is_offerer = is_offerer;
  app.running = 1;
  g_app = &app;

  app.loop = turbo_loop_create();
  if (!app.loop) {
    TLOG_ERROR("Failed to create loop");
    return 1;
  }

  app.ice_ctx = coro_context_create(app.loop);
  if (!app.ice_ctx) {
    TLOG_ERROR("Failed to create ICE context");
    turbo_loop_destroy(app.loop);
    return 1;
  }

  coro_context_set_persistent(app.ice_ctx, 1);

  /* ========== Create ICE Agent ========== */
  ice_config_t ice_config = ice_default_config();
  ice_config.is_controlling = is_offerer;


  /* Add public STUN server */
  strncpy(ice_config.stun_servers[0].url, "stun:stun.l.google.com:19302",
          sizeof(ice_config.stun_servers[0].url) - 1);
  ice_config.stun_server_count = 1;

  app.ice_agent = ice_agent_create(app.ice_ctx, &ice_config);
  if (!app.ice_agent) {
    TLOG_ERROR("Failed to create ICE agent");
    coro_context_destroy(app.ice_ctx);
    turbo_loop_destroy(app.loop);
    return 1;
  }

  ice_callbacks_t ice_cbs = {.on_state_change = on_ice_state,
                             .on_gathering_change = on_ice_gathering,
                             .on_candidate = on_ice_candidate,
                             .on_data = on_ice_data,
                             .user_data = &app};
  ice_agent_set_callbacks(app.ice_agent, &ice_cbs);

  /* ========== Create DataChannel Context ========== */
  turbo_dc_config_t dc_config = {.is_server = !is_offerer, /* Answerer is DTLS server */
                                 .transport = TURBO_DC_TRANSPORT_ICE};

  app.dc_ctx = turbo_dc_context_create(&dc_config);
  if (!app.dc_ctx) {
    TLOG_ERROR("Failed to create DC context");
    ice_agent_destroy(app.ice_agent);
    return 1;
  }

  app.dc_peer = turbo_dc_peer_create(app.dc_ctx, NULL, 0, &app);
  if (!app.dc_peer) {
    TLOG_ERROR("Failed to create DC peer");
    turbo_dc_context_destroy(app.dc_ctx);
    ice_agent_destroy(app.ice_agent);
    return 1;
  }

  turbo_dc_peer_on_state(app.dc_peer, on_dc_state);
  turbo_dc_peer_on_channel(app.dc_peer, on_dc_channel);
  turbo_dc_peer_on_error(app.dc_peer, on_dc_error);

  /* Set ICE agent as transport */
  if (turbo_dc_peer_set_ice_agent(app.dc_peer, app.ice_agent) != 0) {
    TLOG_ERROR("Failed to attach TurboNet ICE transport to DataChannel peer");
    turbo_dc_peer_destroy(app.dc_peer);
    turbo_dc_context_destroy(app.dc_ctx);
    ice_agent_destroy(app.ice_agent);
    coro_context_destroy(app.ice_ctx);
    turbo_loop_destroy(app.loop);
    return 1;
  }

  /* ========== Start ICE Gathering ========== */
  TLOG_INFO("Starting ICE gathering...");
  ice_agent_gather_candidates(app.ice_agent);

  /* Get local credentials */
  char local_ufrag[32], local_pwd[64];
  ice_agent_get_local_credentials(app.ice_agent, local_ufrag, sizeof(local_ufrag), local_pwd,
                                  sizeof(local_pwd));
  TLOG_INFO("=== Local ICE Credentials ===");
  TLOG_INFO("ufrag: {}", local_ufrag);
  TLOG_INFO("pwd: {}", local_pwd);

  /* Wait for gathering */
  TLOG_INFO("Waiting for candidate gathering...");
  while (ice_agent_get_gathering_state(app.ice_agent) != ICE_GATHERING_COMPLETE) {
    turbo_loop_poll(app.loop, 10, 0);
    SLEEP_MS(100);
  }



  /* ========== Exchange Credentials (Manual) ========== */
  printf("\n=== Enter Remote ICE Credentials ===\n");
  char remote_ufrag[32], remote_pwd[64];
  printf("Remote ufrag: ");
  if (scanf("%31s", remote_ufrag) != 1)
    return 1;
  printf("Remote pwd: ");
  if (scanf("%63s", remote_pwd) != 1)
    return 1;

  /* Consume leftover newline from scanf */
  int ch;
  while ((ch = getchar()) != '\n' && ch != EOF)
    ;

  ice_agent_set_remote_credentials(app.ice_agent, remote_ufrag, remote_pwd);

  /* Add remote candidates */
  printf("\nEnter remote candidates (one per line, empty line to finish):\n");
  char line[256];
  while (fgets(line, sizeof(line), stdin)) {
    if (line[0] == '\n' || line[0] == '\0')
      break;
    line[strcspn(line, "\n")] = 0; /* Remove newline */
    if (strlen(line) > 0) {
      ice_agent_add_remote_candidate(app.ice_agent, line);
      printf("Added: %s\n", line);
    }
  }
  ice_agent_end_of_candidates(app.ice_agent);

  /* Start connectivity checks */
  TLOG_INFO("Starting ICE connectivity checks...");
  ice_agent_start_checks(app.ice_agent);

  /* ========== Main Loop ========== */
  TLOG_INFO("Running... (Press Ctrl+C to exit)");
  while (app.running) {
    turbo_loop_poll(app.loop, 10, 0);

    /* Process SCTP timers - required for message delivery */
    turbo_dc_handle_timers();

    SLEEP_MS(10);

    /* Send test message if channel is open */
    if (app.channel && turbo_dc_channel_is_open(app.channel)) {
      static int sent = 0;
      if (!sent) {
        const char *msg = "Hello via ICE!";
        TLOG_INFO("Sending: {}", msg);
        turbo_dc_channel_send(app.channel, msg, strlen(msg), 0);
        sent = 1;
      }
    }
  }

  /* Cleanup */
  turbo_dc_peer_destroy(app.dc_peer);
  turbo_dc_context_destroy(app.dc_ctx);
  ice_agent_destroy(app.ice_agent);
  coro_context_destroy(app.ice_ctx);
  turbo_loop_destroy(app.loop);

  TLOG_INFO("Done!");
  return 0;
}
