/**
 * signaled_peer.c - WebRTC DataChannel peer using room-based WebSocket signaling
 *
 * Requires a signaling server that speaks the repo's simple JSON protocol:
 *   join / joined / peers / peer-joined / offer / answer / candidate
 *
 * Usage:
 *   rtc_signaled_peer --offer --room demo
 *   rtc_signaled_peer --answer --room demo
 */

#include <platform.h>

#include "ice_integration.h"
#include "tlog.h"
#include "turbo_datachannel.h"
#include <http_client/http.h>
#include <cnet/websocket.h>
#include <json_parser.h>
#include "turbo_sdp.h"
#ifdef TURBO_MEDIA_TEST_PUBLIC_C11_SCTP
#include "turbo_datachannel_internal.h"
#include "sctp_assoc_internal.h"
#include "sctp_dcep_internal.h"
#include "sctp_webrtc_internal.h"
#include "sctp_gmssl_mac_internal.h"
#include <gmssl/rand.h>
#endif
#include <cmeta_error.h>
#include <cmeta_thread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROOM_LEN 64
#define MAX_PEER_ID_LEN 64
#define MAX_SIGNAL_HOST_LEN 256
#define MAX_SIGNAL_PATH_LEN 128
#define MAX_CACHED_CANDIDATES 32
#define SIGNAL_MAX_MESSAGE_BYTES (16u * 1024u)
#define SIGNAL_MAX_HANDSHAKE_HEADER_BYTES (8u * 1024u)
#define SIGNAL_OPERATION_TIMEOUT_MS 5000u
#define SIGNAL_RECEIVE_TIMEOUT_MS 50u
#define SIGNAL_SHUTDOWN_TIMEOUT_MS 1000u

typedef struct signal_message_s signal_message_t;

struct signal_message_s {
  char *json;
  signal_message_t *next;
};

typedef struct {
  chttp_websocket_client signal_client;
  int signal_client_initialized;

  ice_integration_ctx_t *ice;
  turbo_dc_context_t *dc_ctx;
  turbo_dc_peer_t *dc_peer;
  _Atomic(turbo_dc_channel_t *) channel;

  int is_offerer;
  int force_stun;
  int janus_echo_mode;
  atomic_int running;
  int ws_connected;
  int local_gathering_complete;
  int remote_credentials_set;
  int remote_candidate_count;
  int remote_end_of_candidates;
  int media_ready;
  int gather_task_started;
  int checks_task_started;
  int local_end_of_candidates_pending;
  int local_end_of_candidates_sent;
  int local_description_sent;
  int answer_pending;
  atomic_int message_sent;
  atomic_int completed;
  atomic_int callback_failed;
  uint64_t last_peer_query_ms;

  char room[MAX_ROOM_LEN];
  char signal_host[MAX_SIGNAL_HOST_LEN];
  char signal_path[MAX_SIGNAL_PATH_LEN];
  uint16_t signal_port;
  int use_tls;

  char peer_id[MAX_PEER_ID_LEN];
  char requested_peer_id[MAX_PEER_ID_LEN];
  char remote_peer_id[MAX_PEER_ID_LEN];
  const char *join_token;

  char cached_candidates[MAX_CACHED_CANDIDATES][512];
  int cached_candidate_count;
  char emitted_candidates[MAX_CACHED_CANDIDATES][512];
  int emitted_candidate_count;

  char pending_candidates[MAX_CACHED_CANDIDATES][512];
  atomic_uint pending_candidate_write;
  atomic_uint pending_candidate_read;

  signal_message_t *outbox_head;
  signal_message_t *outbox_tail;

  sdp_session_t remote_sdp;
  int have_remote_sdp;
#ifdef TURBO_MEDIA_TEST_PUBLIC_C11_SCTP
  cnet_sctp_assoc c11_assoc;
  cnet_sctp_dcep c11_dcep;
  cnet_sctp_gmssl_mac c11_cookie_mac;
  uint64_t c11_now_ms;
  int c11_assoc_initialized;
  int c11_dcep_initialized;
  int c11_dcep_open_sent;
  int c11_local_is_dtls_server;
  int c11_binary_echo_pending;
  int c11_large_echo_pending;
  int c11_secondary_open_sent;
  int c11_unordered_sent;
  int c11_reset_requested;
  int c11_reset_sent;
  int c11_reset_incoming_done;
  int c11_reset_outgoing_done;
  int c11_reuse_open_sent;
  int c11_reuse_echo_sent;
  int c11_pr_rtx_open_sent;
  int c11_pr_rtx_payload_sent;
  /* Test-only deterministic loss on the negotiated SID5 RTX1 channel.
   * No production SCTP packet, timer, handler or socket is faulted. */
  int c11_pr_loss_active;
  int c11_pr_loss_drops;
  int c11_pr_forward_sent;
  int c11_pr_recovery_sent;
  uint32_t c11_pr_loss_tsn;
  uint16_t c11_pr_fault_sid;
  unsigned int c11_pr_loss_budget;
  int c11_pr_zero_open_sent;
  int c11_pr_zero_payload_sent;
  int c11_pr_zero_loss_active;
  int c11_pr_zero_recovery_sent;
  int c11_pr_timed_open_sent;
  int c11_pr_timed_payload_sent;
  int c11_pr_timed_loss_active;
  int c11_pr_timed_recovery_sent;
  uint64_t c11_pr_timed_drop_ms;
#endif
} app_state_t;

static void tracef_app(const app_state_t *app, const char *fmt, ...) {
  const char *path = getenv("TURBO_SIGNAL_TRACE");
  FILE *fp;
  va_list args;

  if (!path || path[0] == '\0') {
    return;
  }

  fp = fopen(path, "a");
  if (!fp) {
    return;
  }

  fprintf(fp, "[role=%s room=%s] ", app && app->is_offerer ? "offerer" : "answerer",
          app ? app->room : "");
  va_start(args, fmt);
  vfprintf(fp, fmt, args);
  va_end(args);
  fputc('\n', fp);
  fclose(fp);
}

static int run_signaling(app_state_t *app);
static int ensure_media_runtime(app_state_t *app);
static int enqueue_signalf(app_state_t *app, const char *fmt, ...);

static void request_peer_list(app_state_t *app) {
  if (!app || !app->ws_connected) {
    return;
  }
  if (enqueue_signalf(app, "{\"type\":\"list-peers\"}") == 0) {
    app->last_peer_query_ms = cmeta_monotonic_ms();
  }
}


static int signal_host_is_loopback(const char *host) {
  if (!host) {
    return 0;
  }
  return strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0 ||
         strcmp(host, "::1") == 0;
}

static void copy_json_string(json_value_t *value, char *buffer, size_t buffer_size) {
  const char *str = json_string(value);
  size_t len = json_string_len(value);
  if (buffer_size == 0) {
    return;
  }
  if (len >= buffer_size) {
    len = buffer_size - 1;
  }
  memcpy(buffer, str, len);
  buffer[len] = '\0';
}

static char *escape_json_string(const char *str) {
  size_t len = strlen(str);
  char *escaped = (char *)malloc(len * 2 + 1);
  char *dst;
  const char *src;

  if (!escaped) {
    return NULL;
  }

  dst = escaped;
  for (src = str; *src; ++src) {
    switch (*src) {
      case '\n':
        *dst++ = '\\';
        *dst++ = 'n';
        break;
      case '\r':
        *dst++ = '\\';
        *dst++ = 'r';
        break;
      case '\t':
        *dst++ = '\\';
        *dst++ = 't';
        break;
      case '"':
        *dst++ = '\\';
        *dst++ = '"';
        break;
      case '\\':
        *dst++ = '\\';
        *dst++ = '\\';
        break;
      default:
        *dst++ = *src;
        break;
    }
  }
  *dst = '\0';
  return escaped;
}

static void app_request_stop(app_state_t *app) {
  if (!app) {
    return;
  }
  atomic_store(&app->running, 0);
}

static int enqueue_signal_text(app_state_t *app, char *json_owned) {
  signal_message_t *msg;

  if (!app || !json_owned) {
    free(json_owned);
    return -1;
  }

  msg = (signal_message_t *)calloc(1, sizeof(*msg));
  if (!msg) {
    free(json_owned);
    return -1;
  }

  msg->json = json_owned;
  if (app->outbox_tail) {
    app->outbox_tail->next = msg;
  } else {
    app->outbox_head = msg;
  }
  app->outbox_tail = msg;
  return 0;
}

static int enqueue_signalf(app_state_t *app, const char *fmt, ...) {
  char *json;
  int rc;
  va_list ap;
  int needed;

  if (!app || !fmt) {
    return -1;
  }

  va_start(ap, fmt);
#ifdef _WIN32
  {
    va_list ap_copy;
    va_copy(ap_copy, ap);
    needed = _vscprintf(fmt, ap_copy);
    va_end(ap_copy);
  }
#else
  {
    va_list ap_copy;
    va_copy(ap_copy, ap);
    needed = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
  }
#endif
  if (needed < 0) {
    va_end(ap);
    return -1;
  }

  json = (char *)malloc((size_t)needed + 1);
  if (!json) {
    va_end(ap);
    return -1;
  }

  vsnprintf(json, (size_t)needed + 1, fmt, ap);
  va_end(ap);

  rc = enqueue_signal_text(app, json);
  if (rc != 0) {
    return -1;
  }
  return 0;
}

static int flush_signal_outbox(app_state_t *app) {
  while (app->outbox_head) {
    signal_message_t *msg = app->outbox_head;
    app->outbox_head = msg->next;
    if (!app->outbox_head) {
      app->outbox_tail = NULL;
    }

    TLOG_INFOF("Sending signaling payload ({} bytes)", strlen(msg->json));
    if (!app->signal_client_initialized ||
        chttp_websocket_client_send_text(&app->signal_client, msg->json, strlen(msg->json),
                                         SIGNAL_OPERATION_TIMEOUT_MS) != SALTS_OK) {
      free(msg->json);
      free(msg);
      return -1;
    }

    free(msg->json);
    free(msg);
  }

  return 0;
}

static void free_signal_outbox(app_state_t *app) {
  signal_message_t *msg = app->outbox_head;
  while (msg) {
    signal_message_t *next = msg->next;
    free(msg->json);
    free(msg);
    msg = next;
  }
  app->outbox_head = NULL;
  app->outbox_tail = NULL;
}

static void flush_cached_candidates(app_state_t *app) {
  int i;
  char *escaped = NULL;

  if (!app->ws_connected || app->remote_peer_id[0] == '\0' || !app->local_description_sent) {
    return;
  }

  for (i = 0; i < app->cached_candidate_count; ++i) {
    escaped = escape_json_string(app->cached_candidates[i]);
    if (!escaped) {
      continue;
    }
    enqueue_signalf(app, "{\"type\":\"candidate\",\"to\":\"%s\",\"candidate\":\"%s\"}",
                    app->remote_peer_id, escaped);
    free(escaped);
  }

  app->cached_candidate_count = 0;
}

static int has_emitted_candidate(const app_state_t *app, const char *candidate_sdp) {
  int i;

  if (!app || !candidate_sdp) {
    return 0;
  }

  for (i = 0; i < app->emitted_candidate_count; ++i) {
    if (strcmp(app->emitted_candidates[i], candidate_sdp) == 0) {
      return 1;
    }
  }

  return 0;
}

static void copy_candidate_text(char *destination, size_t capacity,
                                const char *source) {
  size_t length = 0u;
  if (!destination || capacity == 0u) return;
  if (!source) {
    destination[0] = '\0';
    return;
  }
  while (length + 1u < capacity && source[length] != '\0') ++length;
  memmove(destination, source, length);
  destination[length] = '\0';
}

static void emit_local_candidate_sdp(app_state_t *app, const char *candidate_sdp) {
  char *escaped = NULL;
  ice_candidate_t parsed;

  if (!app || !candidate_sdp || candidate_sdp[0] == '\0' ||
      has_emitted_candidate(app, candidate_sdp)) {
    return;
  }
  if (ice_candidate_parse(candidate_sdp, &parsed) == 0 && parsed.port == 0) {
    return;
  }

  if (app->emitted_candidate_count < MAX_CACHED_CANDIDATES) {
    copy_candidate_text(app->emitted_candidates[app->emitted_candidate_count],
                        sizeof(app->emitted_candidates[app->emitted_candidate_count]),
                        candidate_sdp);
    app->emitted_candidate_count++;
  }

  if (app->ws_connected && app->remote_peer_id[0] != '\0' && app->local_description_sent) {
    escaped = escape_json_string(candidate_sdp);
    if (!escaped) {
      return;
    }
    enqueue_signalf(app, "{\"type\":\"candidate\",\"to\":\"%s\",\"candidate\":\"%s\"}",
                    app->remote_peer_id, escaped);
    free(escaped);
    return;
  }

  if (app->cached_candidate_count < MAX_CACHED_CANDIDATES) {
    copy_candidate_text(app->cached_candidates[app->cached_candidate_count],
                        sizeof(app->cached_candidates[app->cached_candidate_count]),
                        candidate_sdp);
    app->cached_candidate_count++;
  }
}

static void drain_pending_candidates(app_state_t *app) {
  unsigned int read_index = atomic_load_explicit(&app->pending_candidate_read,
                                                  memory_order_relaxed);
  unsigned int write_index = atomic_load_explicit(&app->pending_candidate_write,
                                                   memory_order_acquire);

  while (read_index != write_index) {
    emit_local_candidate_sdp(
        app, app->pending_candidates[read_index % MAX_CACHED_CANDIDATES]);
    read_index++;
  }
  atomic_store_explicit(&app->pending_candidate_read, read_index,
                        memory_order_release);
}

static void flush_end_of_candidates(app_state_t *app) {
  if (!app || !app->local_end_of_candidates_pending || app->local_end_of_candidates_sent) {
    return;
  }
  if (!app->ws_connected || app->remote_peer_id[0] == '\0' || !app->local_description_sent) {
    return;
  }

  if (enqueue_signalf(app, "{\"type\":\"end-of-candidates\",\"to\":\"%s\"}",
                      app->remote_peer_id) == 0) {
    app->local_end_of_candidates_pending = 0;
    app->local_end_of_candidates_sent = 1;
  }
}

static int sdp_candidate_to_string(const sdp_candidate_t *candidate, char *buffer, size_t buffer_size) {
  int len = snprintf(buffer, buffer_size, "candidate:%s %d %s %u %s %u typ %s",
                     candidate->foundation, candidate->component, candidate->transport,
                     candidate->priority, candidate->address, candidate->port, candidate->type);
  if (len < 0 || (size_t)len >= buffer_size) {
    return -1;
  }

  if (candidate->rel_addr[0] != '\0' && candidate->rel_port != 0) {
    int extra = snprintf(buffer + len, buffer_size - (size_t)len, " raddr %s rport %u",
                         candidate->rel_addr, candidate->rel_port);
    if (extra < 0 || (size_t)extra >= buffer_size - (size_t)len) {
      return -1;
    }
  }

  return 0;
}

static void maybe_start_checks(app_state_t *app) {
  if (app->checks_task_started) {
    tracef_app(app, "maybe_start_checks skip: already started");
    return;
  }
  if (!app->ice || !app->remote_credentials_set) {
    tracef_app(app, "maybe_start_checks wait: ice=%p remote_credentials_set=%d",
               (void *)app->ice, app->remote_credentials_set);
    return;
  }
  if (!app->local_gathering_complete) {
    tracef_app(app, "maybe_start_checks wait: local_gathering_complete=%d",
               app->local_gathering_complete);
    return;
  }
  if (app->remote_candidate_count <= 0) {
    tracef_app(app, "maybe_start_checks wait: remote_candidate_count=%d",
               app->remote_candidate_count);
    return;
  }
  if (!app->remote_end_of_candidates) {
    tracef_app(app, "maybe_start_checks wait: remote_end_of_candidates=%d",
               app->remote_end_of_candidates);
    return;
  }
  if (app->emitted_candidate_count <= 0) {
    tracef_app(app, "maybe_start_checks wait: local_candidate_count=%d",
               app->emitted_candidate_count);
    return;
  }

  tracef_app(app, "maybe_start_checks start: local_candidate_count=%d remote_candidate_count=%d",
             app->emitted_candidate_count, app->remote_candidate_count);
  ice_integration_end_of_candidates(app->ice);
  app->checks_task_started = 1;
  tracef_app(app, "maybe_start_checks started");
}

static int build_local_sdp(app_state_t *app, int answering, char *buffer, size_t buffer_size) {
  char ufrag[32];
  char pwd[64];
  char fp_hash[16];
  char fingerprint[128];
  sdp_session_t local_sdp;
  sdp_media_t *media;

  sdp_session_init(&local_sdp);
  /* Candidate messages are sent after the description, so advertise ICE
   * trickle explicitly rather than making the remote endpoint assume a
   * complete candidate list is embedded in this SDP. */
  local_sdp.ice_trickle = 1;
  TLOG_INFO("build_local_sdp: add datachannel m-line");
  media = sdp_add_datachannel(&local_sdp, "0", 5000);
  if (!media) {
    return -1;
  }

  TLOG_INFO("build_local_sdp: fetch ICE credentials");
  if (ice_integration_get_local_credentials(app->ice, ufrag, sizeof(ufrag),
                                            pwd, sizeof(pwd)) != 0) {
    return -1;
  }
  TLOG_INFO("build_local_sdp: attach ICE credentials");
  sdp_media_set_ice(media, ufrag, pwd);
  media->setup = answering ? SDP_ROLE_PASSIVE : SDP_ROLE_ACTPASS;
  media->max_message_size = 262144;

  TLOG_INFO("build_local_sdp: fetch DTLS fingerprint");
  if (turbo_dc_context_get_local_fingerprint(app->dc_ctx, fp_hash, sizeof(fp_hash),
                                             fingerprint, sizeof(fingerprint)) != 0) {
    return -1;
  }
  TLOG_INFO("build_local_sdp: attach DTLS fingerprint");
  sdp_media_set_fingerprint(media, fp_hash, fingerprint);

  if (answering) {
    if (!app->have_remote_sdp) {
      return -1;
    }
    TLOG_INFO("build_local_sdp: generate SDP answer");
    return sdp_generate_answer(&local_sdp, &app->remote_sdp, buffer, buffer_size);
  }

  TLOG_INFO("build_local_sdp: generate SDP offer");
  return sdp_generate_offer(&local_sdp, buffer, buffer_size);
}

static int sdp_has_end_of_candidates(const char *sdp) {
  const char *line;
  if (!sdp) return 0;
  line = sdp;
  while (*line) {
    const char *end = strchr(line, '\n');
    size_t len = end ? (size_t)(end - line) : strlen(line);
    if (len > 0 && line[len - 1] == '\r') len--;
    if (len == sizeof("a=end-of-candidates") - 1 &&
        memcmp(line, "a=end-of-candidates", len) == 0) {
      return 1;
    }
    if (!end) break;
    line = end + 1;
  }
  return 0;
}

static int apply_remote_sdp(app_state_t *app, const char *type, const char *sdp_str) {
  int i;
  char candidate_line[512];
  sdp_session_t remote_sdp;
  sdp_media_t *media;

  if (sdp_parse(sdp_str, strlen(sdp_str), &remote_sdp) != 0) {
    TLOG_ERROR("Failed to parse remote SDP");
    return -1;
  }

  app->remote_sdp = remote_sdp;
  app->have_remote_sdp = 1;

  media = sdp_find_media_by_type(&remote_sdp, SDP_MEDIA_APPLICATION);
  if (!media) {
    TLOG_ERROR("Remote SDP does not contain an application media section");
    return -1;
  }

  /* The answer's setup attribute determines which endpoint initiates DTLS.
   * If Janus answers with setup:active, Janus sends ClientHello and this
   * offerer must use DTLS server/accept state. setup:passive requires the
   * inverse. Do this while the peer is still NEW, before ICE can connect and
   * start the handshake. */
  if (strcmp(type, "answer") == 0) {
    int local_is_dtls_server;
    const char *remote_setup;
    if (media->setup == SDP_ROLE_ACTIVE) {
      local_is_dtls_server = 1;
      remote_setup = "active";
    } else if (media->setup == SDP_ROLE_PASSIVE) {
      local_is_dtls_server = 0;
      remote_setup = "passive";
    } else {
      TLOG_ERRORF("Invalid DTLS setup role in SDP answer: {}",
                  media->setup == SDP_ROLE_ACTPASS ? "actpass" : "unknown");
      return -1;
    }
    if (turbo_dc_peer_set_dtls_role(app->dc_peer, local_is_dtls_server) != 0) {
      TLOG_ERRORF("Failed to apply DTLS role from remote answer setup:{}",
                  remote_setup);
      return -1;
    }
#ifdef TURBO_MEDIA_TEST_PUBLIC_C11_SCTP
    app->c11_local_is_dtls_server = local_is_dtls_server;
#endif
    TLOG_INFOF("Remote answer setup:{}; local DTLS role:{}",
               remote_setup, local_is_dtls_server ? "server/passive" : "client/active");
  }

  if (ice_integration_set_remote_credentials(app->ice, media->ice_ufrag,
                                             media->ice_pwd) != 0) {
    TLOG_ERROR("Failed to apply remote ICE credentials");
    return -1;
  }
  app->remote_credentials_set = 1;

  if (media->fingerprint_hash[0] == '\0' || media->fingerprint[0] == '\0') {
    TLOG_ERROR("Remote SDP is missing a DTLS fingerprint");
    return -1;
  }
  {
    int fingerprint_status = turbo_dc_peer_set_remote_fingerprint(
        app->dc_peer, media->fingerprint_hash, media->fingerprint);
    if (fingerprint_status != 0) {
      TLOG_ERRORF("Invalid remote DTLS fingerprint in SDP (hash={}, length={}, status={})",
                  media->fingerprint_hash, strlen(media->fingerprint),
                  fingerprint_status);
      return -1;
    }
  }
  TLOG_INFOF("Applied remote DTLS fingerprint: hash={} length={}",
             media->fingerprint_hash, strlen(media->fingerprint));

  for (i = 0; i < media->candidate_count; ++i) {
    if (sdp_candidate_to_string(&media->candidates[i], candidate_line, sizeof(candidate_line)) == 0) {
      if (ice_integration_add_remote_candidate(app->ice, candidate_line) == 0) {
        app->remote_candidate_count++;
      }
    }
  }

  /* Some public peers (including Janus) include the complete ICE candidate
   * set and a=end-of-candidates in the SDP answer instead of sending a
   * separate trickle-complete event. Preserve that end marker so connectivity
   * checks do not wait forever for a signaling event that will never arrive. */
  if (sdp_has_end_of_candidates(sdp_str)) {
    app->remote_end_of_candidates = 1;
    ice_integration_end_of_candidates(app->ice);
  }
  TLOG_INFOF("Remote {} SDP: candidates={} end_of_candidates={}",
             type, media->candidate_count, app->remote_end_of_candidates);
  TLOG_INFOF("Applied remote {} SDP", type);
  return 0;
}

static void send_offer(app_state_t *app) {
  char sdp[4096];
  char *escaped = NULL;

  if (app->remote_peer_id[0] == '\0') {
    return;
  }
  TLOG_INFO("send_offer: preparing media runtime");
  if (ensure_media_runtime(app) != 0) {
    TLOG_ERROR("send_offer: ensure_media_runtime failed");
    return;
  }
  TLOG_INFO("send_offer: building local SDP");
  if (build_local_sdp(app, 0, sdp, sizeof(sdp)) <= 0) {
    TLOG_ERROR("Failed to generate SDP offer");
    return;
  }

  escaped = escape_json_string(sdp);
  if (!escaped) {
    return;
  }

  if (enqueue_signalf(app, "{\"type\":\"offer\",\"to\":\"%s\",\"sdp\":\"%s\"}",
                      app->remote_peer_id, escaped) == 0) {
    TLOG_INFOF("Sent offer to {}", app->remote_peer_id);
    if (flush_signal_outbox(app) != 0) {
      TLOG_ERROR("Failed to flush SDP offer");
    }
    app->local_description_sent = 1;
  }
  free(escaped);
  flush_cached_candidates(app);
  flush_end_of_candidates(app);
  if (flush_signal_outbox(app) != 0) {
    TLOG_ERROR("Failed to flush local ICE candidates");
  }
}

static void send_answer(app_state_t *app) {
  char sdp[4096];
  char *escaped = NULL;

  if (app->remote_peer_id[0] == '\0') {
    return;
  }
  if (build_local_sdp(app, 1, sdp, sizeof(sdp)) <= 0) {
    TLOG_ERROR("Failed to generate SDP answer");
    return;
  }

  escaped = escape_json_string(sdp);
  if (!escaped) {
    return;
  }

  if (enqueue_signalf(app, "{\"type\":\"answer\",\"to\":\"%s\",\"sdp\":\"%s\"}",
                      app->remote_peer_id, escaped) == 0) {
    TLOG_INFOF("Sent answer to {}", app->remote_peer_id);
    if (flush_signal_outbox(app) != 0) {
      TLOG_ERROR("Failed to flush SDP answer");
    }
    app->local_description_sent = 1;
  }
  free(escaped);
  flush_cached_candidates(app);
  flush_end_of_candidates(app);
  if (flush_signal_outbox(app) != 0) {
    TLOG_ERROR("Failed to flush local ICE candidates");
  }
}

static void handle_joined(app_state_t *app, json_value_t *root) {
  json_value_t *peer_id = json_object_get(root, "peerId");
  if (!peer_id || json_type(peer_id) != JSON_STRING) {
    return;
  }
  copy_json_string(peer_id, app->peer_id, sizeof(app->peer_id));
  TLOG_INFOF("Joined room '{}' as {}", app->room, app->peer_id);
  if (app->is_offerer && app->remote_peer_id[0] == '\0') {
    request_peer_list(app);
  }
}

static void handle_peers(app_state_t *app, json_value_t *root) {
  size_t i;
  json_value_t *peers = json_object_get(root, "peers");
  if (!app->is_offerer || app->remote_peer_id[0] != '\0') {
    return;
  }
  if (!peers || json_type(peers) != JSON_ARRAY) {
    return;
  }

  for (i = 0; i < json_array_size(peers); ++i) {
    json_value_t *value = json_array_get(peers, i);
    if (!value || json_type(value) != JSON_STRING) {
      continue;
    }
    copy_json_string(value, app->remote_peer_id, sizeof(app->remote_peer_id));
    if (strcmp(app->remote_peer_id, app->peer_id) != 0) {
      TLOG_INFOF("Selected remote peer {}", app->remote_peer_id);
      send_offer(app);
      return;
    }
  }
  app->remote_peer_id[0] = '\0';
}

static void handle_peer_joined(app_state_t *app, json_value_t *root) {
  json_value_t *peer_id = json_object_get(root, "peerId");
  if (!app->is_offerer || app->remote_peer_id[0] != '\0') {
    return;
  }
  if (!peer_id || json_type(peer_id) != JSON_STRING) {
    return;
  }

  copy_json_string(peer_id, app->remote_peer_id, sizeof(app->remote_peer_id));
  if (strcmp(app->remote_peer_id, app->peer_id) == 0) {
    app->remote_peer_id[0] = '\0';
    return;
  }

  TLOG_INFOF("Peer joined: {}", app->remote_peer_id);
  send_offer(app);
}

static void handle_offer(app_state_t *app, json_value_t *root) {
  char from[MAX_PEER_ID_LEN];
  char sdp[4096];
  json_value_t *from_value = json_object_get(root, "from");
  json_value_t *sdp_value = json_object_get(root, "sdp");

  if (!from_value || !sdp_value) {
    return;
  }
  if (json_type(from_value) != JSON_STRING || json_type(sdp_value) != JSON_STRING) {
    return;
  }

  copy_json_string(from_value, from, sizeof(from));
  copy_json_string(sdp_value, sdp, sizeof(sdp));
  snprintf(app->remote_peer_id, sizeof(app->remote_peer_id), "%s", from);

  if (ensure_media_runtime(app) != 0) {
    return;
  }
  if (apply_remote_sdp(app, "offer", sdp) == 0) {
    if (app->local_gathering_complete) {
      send_answer(app);
      maybe_start_checks(app);
    } else {
      app->answer_pending = 1;
    }
  }
}

static void handle_answer(app_state_t *app, json_value_t *root) {
  char sdp[4096];
  json_value_t *sdp_value = json_object_get(root, "sdp");

  if (!sdp_value || json_type(sdp_value) != JSON_STRING) {
    return;
  }

  copy_json_string(sdp_value, sdp, sizeof(sdp));
  tracef_app(app, "handle_answer sdp_len=%zu", strlen(sdp));
  if (apply_remote_sdp(app, "answer", sdp) == 0) {
    maybe_start_checks(app);
  }
}

static void handle_candidate(app_state_t *app, json_value_t *root) {
  char candidate[512];
  json_value_t *candidate_value = json_object_get(root, "candidate");

  if (!candidate_value || json_type(candidate_value) != JSON_STRING) {
    return;
  }
  if (ensure_media_runtime(app) != 0) {
    return;
  }

  copy_json_string(candidate_value, candidate, sizeof(candidate));
  if (ice_integration_add_remote_candidate(app->ice, candidate) == 0) {
    app->remote_candidate_count++;
    tracef_app(app, "handle_candidate added count=%d candidate='%s'", app->remote_candidate_count,
               candidate);
    maybe_start_checks(app);
  } else {
    tracef_app(app, "handle_candidate failed candidate='%s'", candidate);
    TLOG_WARN("Failed to add remote ICE candidate");
  }
}

static void handle_end_of_candidates(app_state_t *app) {
  if (ensure_media_runtime(app) != 0) {
    return;
  }

  app->remote_end_of_candidates = 1;
  tracef_app(app, "handle_end_of_candidates remote_candidate_count=%d",
             app->remote_candidate_count);
  maybe_start_checks(app);
}

static void process_signaling_message(app_state_t *app, const char *message, size_t len) {
  char *json_text = NULL;
  json_value_t *root = NULL;
  json_value_t *type = NULL;
  char type_name[32];

  json_text = (char *)malloc(len + 1);
  if (!json_text) {
    return;
  }
  memcpy(json_text, message, len);
  json_text[len] = '\0';

  if (((root = json_parse((const char *)((const uint8_t *)json_text), len)) ? 0 : -1) != 0 || !root ||
      json_type(root) != JSON_OBJECT) {
    TLOG_WARN("Ignoring invalid signaling message");
    if (root) {
      json_free(root);
      root = NULL;
    }
    free(json_text);
    return;
  }

  type = json_object_get(root, "type");
  if (!type || json_type(type) != JSON_STRING) {
    json_free(root);
    root = NULL;
    free(json_text);
    return;
  }

  copy_json_string(type, type_name, sizeof(type_name));
  tracef_app(app, "process_signaling_message type=%s len=%zu", type_name, len);
  if (strcmp(type_name, "joined") == 0) {
    handle_joined(app, root);
  } else if (strcmp(type_name, "peers") == 0) {
    handle_peers(app, root);
  } else if (strcmp(type_name, "peer-joined") == 0) {
    handle_peer_joined(app, root);
  } else if (strcmp(type_name, "offer") == 0) {
    handle_offer(app, root);
  } else if (strcmp(type_name, "answer") == 0) {
    handle_answer(app, root);
  } else if (strcmp(type_name, "candidate") == 0) {
    handle_candidate(app, root);
  } else if (strcmp(type_name, "end-of-candidates") == 0) {
    handle_end_of_candidates(app);
  } else if (strcmp(type_name, "peer-left") == 0) {
    TLOG_INFO("Remote peer left room");
    app->remote_peer_id[0] = '\0';
  } else if (strcmp(type_name, "error") == 0) {
    TLOG_ERRORF("Signaling server returned an error (payload_bytes={})", len);
  }

  json_free(root);

  root = NULL;
  free(json_text);
}

static void on_ice_state_change(ice_state_t new_state, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  TLOG_INFOF("ICE state: {}", ice_state_name(new_state));

  if (new_state == ICE_STATE_FAILED) {
    app_request_stop(app);
  }
}

static void on_ice_candidate(const char *candidate_sdp, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  unsigned int write_index = atomic_load_explicit(&app->pending_candidate_write,
                                                   memory_order_relaxed);
  unsigned int read_index = atomic_load_explicit(&app->pending_candidate_read,
                                                  memory_order_acquire);

  if (write_index - read_index >= MAX_CACHED_CANDIDATES) {
    atomic_store(&app->callback_failed, 1);
    app_request_stop(app);
    return;
  }
  snprintf(app->pending_candidates[write_index % MAX_CACHED_CANDIDATES],
           sizeof(app->pending_candidates[0]), "%s", candidate_sdp);
  atomic_store_explicit(&app->pending_candidate_write, write_index + 1,
                        memory_order_release);
}

static void on_dc_message(turbo_dc_channel_t *channel, const void *data, size_t len, int is_binary,
                          void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  char message[512];
  size_t copy_len = len < sizeof(message) - 1 ? len : sizeof(message) - 1;
  (void)channel;
  (void)is_binary;

  memcpy(message, data, copy_len);
  message[copy_len] = '\0';
  TLOG_INFOF("Received: {}", message);

  if (!app->is_offerer && strncmp(message, "ECHO:", 5) != 0) {
    char echo[512];
    int echo_len = snprintf(echo, sizeof(echo), "ECHO: %s", message);
    if (echo_len > 0 && (size_t)echo_len < sizeof(echo)) {
      turbo_dc_channel_send(channel, echo, (size_t)echo_len, 0);
      atomic_store(&app->completed, 1);
    }
  } else if (app->is_offerer) {
    const char *janus_prefix = "Janus EchoTest here! You wrote: ";
    if (app->janus_echo_mode) {
      const size_t prefix_len = strlen(janus_prefix);
      if (strncmp(message, janus_prefix, prefix_len) != 0) {
        return;
      }
      if (strcmp(message + prefix_len, "hello over signaled ICE") != 0) {
        TLOG_ERROR("Janus EchoTest returned a different payload");
        app_request_stop(app);
        return;
      }
      puts("PUBLIC_JANUS_ECHO_PASS: exact DataChannel payload returned");
      TLOG_INFO("Public Janus EchoTest returned the exact DataChannel payload");
    } else if (strncmp(message, "ECHO:", 5) != 0) {
      return;
    }
    atomic_store(&app->completed, 1);
    app_request_stop(app);
  }
}

static void on_dc_open(turbo_dc_channel_t *channel, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  const char *hello = "hello over signaled ICE";
  TLOG_INFOF("Channel '{}' opened", turbo_dc_channel_get_label(channel));

  if (app->is_offerer && !atomic_exchange(&app->message_sent, 1)) {
    turbo_dc_channel_send(channel, hello, strlen(hello), 0);
  }
}

static void on_dc_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)peer;
  atomic_store(&app->channel, channel);
  turbo_dc_channel_set_user_data(channel, app);
  turbo_dc_channel_on_open(channel, on_dc_open);
  turbo_dc_channel_on_message(channel, on_dc_message);
}

#ifdef TURBO_MEDIA_TEST_PUBLIC_C11_SCTP
/* Private qualification executable only; production DataChannel keeps usrsctp. */
static int c11_is_owner(void *context) {
  app_state_t *app = (app_state_t *)context;
  return app && app->dc_peer && dc_peer_is_on_transport_owner(app->dc_peer);
}
static int c11_entropy(void *context, uint8_t *out, size_t size) {
  (void)context;
  return out && rand_bytes(out, size) == 1 ? 0 : -1;
}
static int c11_mac_sign(void *context, const uint8_t *bytes, size_t size,
                        uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  app_state_t *app = (app_state_t *)context;
  return app ? cnet_sctp_gmssl_mac_sign(&app->c11_cookie_mac, bytes, size, out) : -1;
}
static int c11_mac_verify(void *context, const uint8_t *bytes, size_t size,
                          const uint8_t mac[CNET_SCTP_ASSOC_MAC_BYTES]) {
  app_state_t *app = (app_state_t *)context;
  return app ? cnet_sctp_gmssl_mac_verify(&app->c11_cookie_mac, bytes, size, mac) : -1;
}
static void c11_wipe(void *memory, size_t size) {
  volatile uint8_t *p = (volatile uint8_t *)memory;
  while (size-- != 0u) *p++ = 0u;
}
static uint64_t c11_clock(app_state_t *app) {
  uint64_t now = cmeta_monotonic_ms();
  if (now < app->c11_now_ms) return app->c11_now_ms;
  app->c11_now_ms = now;
  return now;
}
static void c11_fail(app_state_t *app, const char *reason) {
  TLOG_ERRORF("Private C11 SCTP public interop failed: {}", reason);
  atomic_store(&app->callback_failed, 1);
  app_request_stop(app);
}
/* Private fault boundary: drop only one selected PPID53 DATA/I-DATA TSN
 * according to the RTX1/RTX0/TIMED test budget. Never fabricate ACKs,
 * touch production packets or modify the protocol clock/timer state. */
#define C11_PR_TIMED_LIFETIME_MS 500u
static uint32_t c11_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24u) | ((uint32_t)p[1] << 16u) |
         ((uint32_t)p[2] << 8u) | (uint32_t)p[3];
}
static int c11_send_packet(app_state_t *app, const uint8_t *packet, size_t size) {
  int drop = 0, forward = 0;
  if (size == 0u) return 0;
  if (!packet || size > CNET_SCTP_ASSOC_MAX_PACKET) {
    c11_fail(app, "C11 SCTP packet exceeded local egress limit");
    return -1;
  }
  if (app->c11_pr_loss_active) {
    cnet_sctp_wire_header header = {0};
    cnet_sctp_wire_decoder decoder = {0};
    cnet_sctp_wire_chunk chunk = {0};
    size_t i;
    if (cnet_sctp_wire_decode(packet, size, &header, &decoder) !=
        CNET_SCTP_WIRE_OK) {
      c11_fail(app, "C11 test-loss boundary rejected malformed SCTP packet");
      return -1;
    }
    for (i = 0u; i < header.chunk_count; ++i) {
      uint16_t sid;
      uint32_t tsn;
      if (cnet_sctp_wire_next_chunk(&decoder, &chunk) != CNET_SCTP_WIRE_OK) {
        c11_fail(app, "C11 test-loss boundary truncated SCTP chunk");
        return -1;
      }
      if (chunk.type == 192u || chunk.type == 194u) {
        const uint32_t advanced = chunk.value_size >= 4u ?
            c11_be32(chunk.value) : 0u;
        if (chunk.value_size < 4u ||
            (uint32_t)(advanced - app->c11_pr_loss_tsn) >=
                UINT32_C(0x80000000) ||
            (unsigned int)app->c11_pr_loss_drops !=
                app->c11_pr_loss_budget) {
          c11_fail(app, "C11 PR-SCTP FORWARD-TSN before expected loss budget");
          return -1;
        }
        if (app->c11_pr_timed_loss_active) {
          const uint64_t now = c11_clock(app);
          if (!app->c11_pr_timed_drop_ms ||
              now < app->c11_pr_timed_drop_ms ||
              now - app->c11_pr_timed_drop_ms < C11_PR_TIMED_LIFETIME_MS) {
            c11_fail(app, "C11 PR-SCTP timed FORWARD-TSN preceded expiry");
            return -1;
          }
        }
        forward = 1;
      }
      if (chunk.type != 0u && chunk.type != 64u) continue;
      if (chunk.value_size < (chunk.type == 64u ? 16u : 12u)) {
        c11_fail(app, "C11 test-loss DATA/I-DATA chunk too short");
        return -1;
      }
      sid = (uint16_t)(((uint16_t)chunk.value[4u] << 8u) |
                       chunk.value[5u]);
      tsn = c11_be32(chunk.value);
      if (sid != app->c11_pr_fault_sid) continue;
      /* DCEP PPID 50 is never faulted. I-DATA has PPID only in a
       * beginning (B) fragment; this test admits one-fragment PR DATA. */
      if (chunk.type == 64u && (chunk.flags & 0x02u) == 0u) continue;
      if (c11_be32(chunk.value + (chunk.type == 64u ? 12u : 8u)) !=
          CNET_SCTP_WEBRTC_BINARY_PPID) continue;
      if (app->c11_pr_loss_drops == 0) app->c11_pr_loss_tsn = tsn;
      if (tsn != app->c11_pr_loss_tsn) continue;
      if ((unsigned int)app->c11_pr_loss_drops >=
          app->c11_pr_loss_budget) {
        c11_fail(app, "C11 PR-SCTP exceeded its negotiated retransmit budget");
        return -1;
      }
      ++app->c11_pr_loss_drops;
      if (app->c11_pr_timed_loss_active &&
          app->c11_pr_loss_drops == 1 &&
          !app->c11_pr_timed_drop_ms) {
        c11_fail(app, "C11 timed DATA admission timestamp not captured");
        return -1;
      }
      drop = 1;
      TLOG_INFOF("Private C11 fault injected drop #{} on PR-SCTP TSN {}",
                 app->c11_pr_loss_drops, tsn);
    }
    if (drop && header.chunk_count != 1u) {
      c11_fail(app, "C11 PR-SCTP loss fixture refuses bundled control/data packet");
      return -1;
    }
  }
  if (drop) return 0; /* Only in the isolated rtc_signaled_peer_c11 executable. */
  if (dtls_test_write_application_data(app->dc_peer, packet, size) != 0) {
    c11_fail(app, "failed to write SCTP packet as authenticated DTLS application data");
    return -1;
  }
  if (forward) {
    app->c11_pr_forward_sent = 1;
    TLOG_INFOF("Private C11 RFC3758 FORWARD-TSN sent for abandoned TSN {}",
               app->c11_pr_loss_tsn);
  }
  return 0;
}
static int c11_send_message_with_order(app_state_t *app, uint16_t sid,
                                       uint32_t ppid, int unordered,
                                       const void *payload, size_t payload_size) {
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t packet_size = 0u;
  cnet_sctp_assoc_status status = cnet_sctp_assoc_send_message(
      &app->c11_assoc, c11_clock(app), sid, ppid, unordered,
      payload, payload_size, packet, sizeof(packet), &packet_size);
  if (status != CNET_SCTP_ASSOC_OK || packet_size == 0u) {
    TLOG_ERRORF("C11 SCTP send refused sid={} ppid={} unordered={} status={}",
                sid, ppid, unordered, (int)status);
    c11_fail(app, "C11 SCTP DATA send was rejected");
    return -1;
  }
  return c11_send_packet(app, packet, packet_size);
}
static int c11_send_message(app_state_t *app, uint16_t sid, uint32_t ppid,
                            const void *payload, size_t payload_size) {
  return c11_send_message_with_order(app, sid, ppid, 0, payload, payload_size);
}

/* Private negotiated PR-SCTP data path. DCEP itself continues to travel
 * as ordered reliable PPID 50, irrespective of channel reliability. */
static int c11_send_pr_rtx1(app_state_t *app, uint16_t sid,
                           const void *payload, size_t payload_size) {
  const cnet_sctp_data_reliability policy = {
      CNET_SCTP_DATA_RELIABILITY_RETRANSMITS, 1u
  };
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t packet_size = 0u;
  cnet_sctp_assoc_status status = cnet_sctp_assoc_send_message_pr(
      &app->c11_assoc, c11_clock(app), sid, CNET_SCTP_WEBRTC_BINARY_PPID,
      0, &policy, payload, payload_size, packet, sizeof(packet),
      &packet_size);
  if (status != CNET_SCTP_ASSOC_OK || packet_size == 0u) {
    TLOG_ERRORF("C11 SCTP negotiated PR-SCTP RTX1 send refused status={}",
                (int)status);
    c11_fail(app, "C11 SCTP PR-SCTP RTX1 data admission rejected");
    return -1;
  }
  return c11_send_packet(app, packet, packet_size);
}
static int c11_send_pr_rtx0(app_state_t *app, uint16_t sid,
                           const void *payload, size_t payload_size) {
  const cnet_sctp_data_reliability policy = {
      CNET_SCTP_DATA_RELIABILITY_RETRANSMITS, 0u
  };
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t packet_size = 0u;
  cnet_sctp_assoc_status status = cnet_sctp_assoc_send_message_pr(
      &app->c11_assoc, c11_clock(app), sid, CNET_SCTP_WEBRTC_BINARY_PPID,
      0, &policy, payload, payload_size, packet, sizeof(packet),
      &packet_size);
  if (status != CNET_SCTP_ASSOC_OK || packet_size == 0u) {
    TLOG_ERRORF("C11 PR-SCTP RTX0 send refused status={}", (int)status);
    c11_fail(app, "C11 SCTP PR-SCTP RTX0 data admission rejected");
    return -1;
  }
  return c11_send_packet(app, packet, packet_size);
}
/* A real C11 partial-reliability send; only its *new successor* retries
 * admission under bounded CWND/RWND pressure, never the lost DATA itself. */
static int c11_send_pr_timed(app_state_t *app, uint16_t sid,
                            const void *payload, size_t payload_size,
                            int allow_backpressure) {
  const cnet_sctp_data_reliability policy = {
      CNET_SCTP_DATA_RELIABILITY_TIMED, C11_PR_TIMED_LIFETIME_MS
  };
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t packet_size = 0u;
  const uint64_t now = c11_clock(app);
  cnet_sctp_assoc_status status;
  /* Capture the EXACT engine-admission time used for expiry, before wire
   * serialization. Sampling again at the drop hook can be 1ms later and
   * falsely reject a correctly timed FORWARD-TSN on a tick boundary. */
  if (app->c11_pr_timed_loss_active && app->c11_pr_loss_drops == 0)
    app->c11_pr_timed_drop_ms = now;
  status = cnet_sctp_assoc_send_message_pr(
      &app->c11_assoc, now, sid, CNET_SCTP_WEBRTC_BINARY_PPID,
      0, &policy, payload, payload_size,
      packet, sizeof(packet), &packet_size);
  if (allow_backpressure &&
      (status == CNET_SCTP_ASSOC_CAPACITY ||
       status == CNET_SCTP_ASSOC_RETRANSMIT_PENDING))
    return 1;
  if (status != CNET_SCTP_ASSOC_OK || !packet_size) {
    TLOG_ERRORF("C11 PR-SCTP TIMED send refused status={}", (int)status);
    c11_fail(app, "C11 SCTP PR-SCTP TIMED send rejected");
    return -1;
  }
  return c11_send_packet(app, packet, packet_size);
}
static uint16_t c11_local_sid(const app_state_t *app) {
  return app->c11_local_is_dtls_server ? 1u : 0u;
}
static uint16_t c11_unordered_sid(const app_state_t *app) {
  return (uint16_t)(c11_local_sid(app) + 2u);
}
static uint16_t c11_pr_rtx_sid(const app_state_t *app) {
  return (uint16_t)(c11_local_sid(app) + 4u);
}
static uint16_t c11_pr_zero_sid(const app_state_t *app) {
  return (uint16_t)(c11_local_sid(app) + 6u);
}
static uint16_t c11_pr_timed_sid(const app_state_t *app) {
  return (uint16_t)(c11_local_sid(app) + 8u);
}
static const uint8_t c11_pr_timed_echo_payload[] = {
    0x54u, 0x49u, 0x4du, 0x45u, 0x44u, 0x00u, 0x93u, 0x16u
};
static const uint8_t c11_pr_timed_lost_payload[] = {
    0x45u, 0x58u, 0x50u, 0x49u, 0x52u, 0x45u, 0xaau, 0x01u
};
static const uint8_t c11_pr_timed_recovery_payload[] = {
    0x54u, 0x49u, 0x4du, 0x45u, 0x52u, 0x00u, 0xf2u, 0x03u
};
static const uint8_t c11_pr_zero_echo_payload[] = {
    0x50u, 0x52u, 0x00u, 0xf0u, 0x30u, 0x00u, 0x5au, 0x19u
};
static const uint8_t c11_pr_zero_lost_payload[] = {
    0xd0u, 0x00u, 0x00u, 0xf0u, 0xabu, 0xceu, 0x22u, 0x11u
};
static const uint8_t c11_pr_zero_recovery_payload[] = {
    0x4eu, 0x4fu, 0x52u, 0x54u, 0x58u, 0x00u, 0xf1u, 0x03u
};
static const uint8_t c11_pr_rtx_echo_payload[] = {
    0x50u, 0x52u, 0x01u, 0xf0u, 0x00u, 0x7fu, 0xabu, 0x12u
};
static const uint8_t c11_pr_lost_payload[] = {
    0xdeu, 0xadu, 0xb0u, 0x00u, 0x00u, 0xedu, 0xf0u, 0x01u
};
static const uint8_t c11_pr_recovery_payload[] = {
    0x52u, 0x45u, 0x43u, 0x4fu, 0x56u, 0x00u, 0xfcu, 0x02u
};

static const uint8_t c11_unordered_echo_payload[] = {
    0x00u, 0xffu, 0x81u, 0x7fu, 0xa5u, 0x00u, 0x3cu, 0x5au
};
/* Distinct payload after a real bidirectional reset and new OPEN/ACK. */
static const uint8_t c11_reused_echo_payload[] = {
    0x53u, 0x43u, 0x54u, 0x50u, 0x00u, 0xfeu, 0x22u, 0x07u
};

static const uint8_t c11_binary_echo_payload[] = {
    0x00u, 0x01u, 0x02u, 0x7fu, 0x80u, 0xffu, 0x00u
};

/* 20 KiB exceeds one SCTP DATA record and 16 outstanding TX slots.
 * A deterministic binary pattern spans NUL/high-byte values and proves the
 * whole message was reassembled by an independent public Janus peer. */
#define C11_PUBLIC_FRAGMENT_BYTES (20u * 1024u)
static uint8_t c11_fragment_byte(size_t index) {
  return (uint8_t)((index * 131u + (index >> 3u) +
                    (index >> 9u) * 7u) & 0xffu);
}

static int c11_queue_large_echo(app_state_t *app, uint16_t sid) {
  uint8_t message[C11_PUBLIC_FRAGMENT_BYTES];
  cnet_sctp_assoc_status status;
  size_t i;
  for (i = 0u; i < sizeof(message); ++i)
    message[i] = c11_fragment_byte(i);
  status = cnet_sctp_assoc_queue_message(&app->c11_assoc, c11_clock(app),
              sid, CNET_SCTP_WEBRTC_BINARY_PPID, 0,
              message, sizeof(message));
  if (status != CNET_SCTP_ASSOC_OK) {
    c11_fail(app, "C11 SCTP 20 KiB binary message queue rejected");
    return -1;
  }
  app->c11_large_echo_pending = 1;
  TLOG_INFO("Private C11 SCTP queued 20480-byte fragmented binary echo");
  return 0;
}

static int c11_echo_channel_ready(const app_state_t *app, uint16_t sid) {
  return app->c11_dcep_open_sent && atomic_load(&app->message_sent) &&
         sid == c11_local_sid(app) &&
         cnet_sctp_dcep_ready(&app->c11_dcep, sid);
}

static int c11_open_datachannel(app_state_t *app) {
  uint8_t open[CNET_SCTP_DCEP_OPEN_MAX];
  size_t open_size = 0u;
  uint16_t streams = app->c11_assoc.config.outbound_streams;
  uint16_t sid = c11_local_sid(app);
  cnet_sctp_dcep_status status;
  if (app->c11_assoc.peer_inbound_streams < streams)
    streams = app->c11_assoc.peer_inbound_streams;
  if (streams > CNET_SCTP_DCEP_STREAMS) streams = CNET_SCTP_DCEP_STREAMS;
  if (streams <= sid) {
    c11_fail(app, "negotiated SCTP stream count cannot carry local DCEP SID");
    return -1;
  }
  status = cnet_sctp_dcep_init(&app->c11_dcep, streams,
                               app->c11_local_is_dtls_server ? 0 : 1,
                               c11_is_owner, app);
  if (status != CNET_SCTP_DCEP_OK) {
    c11_fail(app, "C11 DCEP initialization failed");
    return -1;
  }
  app->c11_dcep_initialized = 1;
  status = cnet_sctp_dcep_open(&app->c11_dcep, sid, 0u, 0u, 0u,
                               "chat", 4u, NULL, 0u,
                               open, sizeof(open), &open_size);
  if (status != CNET_SCTP_DCEP_OK || open_size == 0u) {
    c11_fail(app, "C11 DCEP OPEN creation failed");
    return -1;
  }
  if (c11_send_message(app, sid, CNET_SCTP_DCEP_PPID, open, open_size) != 0)
    return -1;
  app->c11_dcep_open_sent = 1;
  TLOG_INFOF("Private C11 SCTP association established; DCEP OPEN sent on SID {}",
             sid);
  return 0;
}
/* Follow the proven ordered SID 1 exchange with an independent, reliable
 * unordered channel. DCEP OPEN itself remains reliably ordered (PPID 50).
 * The new SID uses local DTLS-role parity, never an arbitrary remote SID. */
static int c11_open_unordered_channel(app_state_t *app) {
  uint8_t open[CNET_SCTP_DCEP_OPEN_MAX];
  size_t open_size = 0u;
  uint16_t sid = c11_unordered_sid(app);
  cnet_sctp_dcep_status status;
  if (!app->c11_dcep_initialized || app->c11_secondary_open_sent ||
      sid >= app->c11_dcep.streams) {
    c11_fail(app, "C11 second DataChannel SID not available");
    return -1;
  }
  const char *label = app->c11_reuse_open_sent ? "reused" : "unordered";
  size_t label_size = strlen(label);
  status = cnet_sctp_dcep_open(&app->c11_dcep, sid, 0x80u, 0u, 0u,
                              label, label_size,
                              NULL, 0u, open, sizeof(open), &open_size);
  if (status != CNET_SCTP_DCEP_OK || open_size == 0u) {
    c11_fail(app, "C11 reliable-unordered DCEP OPEN rejected");
    return -1;
  }
  if (c11_send_message(app, sid, CNET_SCTP_DCEP_PPID,
                       open, open_size) != 0) return -1;
  app->c11_secondary_open_sent = 1;
  TLOG_INFOF("Private C11 DCEP reliable-unordered OPEN sent on SID {} reuse={}",
             sid, app->c11_reuse_open_sent);
  return 0;
}

/* The SCTP engine reports RFC 6525 completion in both directions. Only when
 * BOTH are performed may the owning ULP retire this SID's old DCEP record.
 * This is a private adapter proof, NOT a production DCEP retirement API. */
static int c11_open_pr_rtx_channel(app_state_t *app) {
  uint8_t open[CNET_SCTP_DCEP_OPEN_MAX];
  size_t open_size = 0u;
  const uint16_t sid = c11_pr_rtx_sid(app);
  cnet_sctp_dcep_status status;
  if (!app->c11_reuse_echo_sent || app->c11_pr_rtx_open_sent ||
      !app->c11_assoc.peer_forward_tsn_supported ||
      !app->c11_assoc.data.config.forward_tsn_supported ||
      sid >= app->c11_dcep.streams) {
    c11_fail(app, "peer PR-SCTP capability missing or third SID unavailable");
    return -1;
  }
  /* RFC 8832 channel type 0x01 = ordered maxRetransmits, limit 1.
   * A successful no-loss echo proves interoperability of the DCEP+PR send
   * path, NOT expiry/abandonment or FORWARD-TSN under deliberate loss. */
  status = cnet_sctp_dcep_open(&app->c11_dcep, sid, 0x01u, 0u, 1u,
                              "pr-rtx1", sizeof("pr-rtx1") - 1u,
                              NULL, 0u, open, sizeof(open), &open_size);
  if (status != CNET_SCTP_DCEP_OK || open_size == 0u) {
    c11_fail(app, "C11 PR-SCTP maxRetransmits=1 DCEP OPEN rejected");
    return -1;
  }
  if (c11_send_message(app, sid, CNET_SCTP_DCEP_PPID,
                       open, open_size) != 0) return -1;
  app->c11_pr_rtx_open_sent = 1;
  TLOG_INFOF("Private C11 PR-SCTP DCEP RTX1 OPEN sent on SID {}", sid);
  return 0;
}

static int c11_open_pr_zero_channel(app_state_t *app) {
  uint8_t open[CNET_SCTP_DCEP_OPEN_MAX];
  size_t open_size = 0u;
  const uint16_t sid = c11_pr_zero_sid(app);
  cnet_sctp_dcep_status status;
  if (!app->c11_pr_recovery_sent || !app->c11_pr_forward_sent ||
      app->c11_pr_loss_drops != 2 || app->c11_pr_zero_open_sent ||
      !app->c11_assoc.peer_forward_tsn_supported ||
      !app->c11_assoc.data.config.forward_tsn_supported ||
      sid >= app->c11_dcep.streams) {
    c11_fail(app, "C11 RTX0 DCEP OPEN before RTX1 proof or missing capability");
    return -1;
  }
  /* RFC 8832 0x01 + 0: ordered channel with zero retransmission allowance.
   * A separate SID prevents mixing the previously negotiated RTX1 policy. */
  status = cnet_sctp_dcep_open(&app->c11_dcep, sid, 0x01u, 0u, 0u,
                              "pr-rtx0", sizeof("pr-rtx0") - 1u,
                              NULL, 0u, open, sizeof(open), &open_size);
  if (status != CNET_SCTP_DCEP_OK || open_size == 0u) {
    c11_fail(app, "C11 PR-SCTP RTX0 DCEP OPEN rejected");
    return -1;
  }
  /* Disable the previous SID's test-only fault filter before DCEP OPEN. */
  app->c11_pr_loss_active = 0;
  if (c11_send_message(app, sid, CNET_SCTP_DCEP_PPID,
                       open, open_size) != 0) return -1;
  app->c11_pr_zero_open_sent = 1;
  TLOG_INFOF("Private C11 PR-SCTP RTX0 DCEP OPEN sent on SID {}", sid);
  return 0;
}

static int c11_open_pr_timed_channel(app_state_t *app) {
  uint8_t open[CNET_SCTP_DCEP_OPEN_MAX];
  size_t open_size = 0u;
  const uint16_t sid = c11_pr_timed_sid(app);
  cnet_sctp_dcep_status status;
  if (!app->c11_pr_zero_recovery_sent || !app->c11_pr_forward_sent ||
      app->c11_pr_loss_drops != 1 || app->c11_pr_timed_open_sent ||
      !app->c11_assoc.peer_forward_tsn_supported ||
      !app->c11_assoc.data.config.forward_tsn_supported ||
      sid >= app->c11_dcep.streams) {
    c11_fail(app, "C11 timed channel premature or unavailable");
    return -1;
  }
  status = cnet_sctp_dcep_open(&app->c11_dcep, sid, 0x02u, 0u,
                              C11_PR_TIMED_LIFETIME_MS,
                              "pr-timed", sizeof("pr-timed") - 1u,
                              NULL, 0u, open, sizeof(open), &open_size);
  if (status != CNET_SCTP_DCEP_OK || !open_size) {
    c11_fail(app, "C11 DCEP TIMED 500ms OPEN rejected");
    return -1;
  }
  app->c11_pr_loss_active = 0;
  if (c11_send_message(app, sid, CNET_SCTP_DCEP_PPID,
                       open, open_size) != 0) return -1;
  app->c11_pr_timed_open_sent = 1;
  TLOG_INFOF("Private C11 DCEP TIMED 500ms OPEN sent on SID {}", sid);
  return 0;
}

static int c11_drain_reset_events(app_state_t *app) {
  unsigned int i;
  for (i = 0u; i < 2u * CNET_SCTP_DCEP_STREAMS; ++i) {
    cnet_sctp_assoc_stream_reset_event event = {0};
    cnet_sctp_assoc_status status =
        cnet_sctp_assoc_take_stream_reset(&app->c11_assoc, &event);
    if (status == CNET_SCTP_ASSOC_EMPTY) break;
    if (status != CNET_SCTP_ASSOC_OK || !app->c11_reset_sent ||
        event.stream_id != c11_unordered_sid(app) ||
        event.result != CNET_SCTP_STREAM_RESET_PERFORMED) {
      c11_fail(app, "C11 SCTP reset event failed or unexpected SID/result");
      return -1;
    }
    if (event.incoming) {
      if (app->c11_reset_incoming_done) {
        c11_fail(app, "duplicate C11 incoming stream-reset completion");
        return -1;
      }
      app->c11_reset_incoming_done = 1;
    } else {
      if (app->c11_reset_outgoing_done) {
        c11_fail(app, "duplicate C11 outgoing stream-reset completion");
        return -1;
      }
      app->c11_reset_outgoing_done = 1;
    }
    TLOG_INFOF("Private C11 RFC6525 SID {} reset completed direction={}",
               event.stream_id, event.incoming ? "incoming" : "outgoing");
  }
  if (i == 2u * CNET_SCTP_DCEP_STREAMS) {
    c11_fail(app, "C11 SCTP reset event drain exceeded bound");
    return -1;
  }
  if (app->c11_reset_incoming_done && app->c11_reset_outgoing_done &&
      !app->c11_reuse_open_sent) {
    const uint16_t sid = c11_unordered_sid(app);
    cnet_sctp_dcep_channel *channel = &app->c11_dcep.channels[sid];
    if (!c11_is_owner(app) || !app->c11_reset_sent ||
        !app->c11_secondary_open_sent || !channel->locally_initiated ||
        channel->state != 2u) {
      c11_fail(app, "C11 DCEP retirement denied before bidirectional reset");
      return -1;
    }
    /* Private ULP projection: the upstream DCEP prototype currently has no
     * per-SID retire API. Reclaim ONLY this channel's owned record after
     * the real remote reciprocal reset; never reset the whole association. */
    memset(channel, 0, sizeof(*channel));
    app->c11_secondary_open_sent = 0;
    app->c11_reuse_open_sent = 1;
    if (c11_open_unordered_channel(app) != 0) return -1;
    TLOG_INFOF("Private C11 RFC6525 bilateral reset admitted DCEP reuse SID {}",
               sid);
  }
  return 0;
}
static int c11_start_association(app_state_t *app) {
  cnet_sctp_assoc_config config = {0};
  uint8_t key[CNET_SCTP_GMSSL_KEY_BYTES] = {0};
  uint64_t binding = 0u;
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t packet_size = 0u;
  cnet_sctp_assoc_status status;
  if (!c11_is_owner(app)) {
    c11_fail(app, "association start was not called by the DataChannel Owner");
    return -1;
  }
  if (app->c11_assoc_initialized) return 0;
  if (rand_bytes(key, sizeof(key)) != 1 ||
      rand_bytes((uint8_t *)&binding, sizeof(binding)) != 1 || binding == 0u) {
    c11_wipe(key, sizeof(key));
    c11_fail(app, "cryptographic key or DTLS generation binding unavailable");
    return -1;
  }
  if (cnet_sctp_gmssl_mac_init(&app->c11_cookie_mac, key, sizeof(key)) != 0) {
    c11_wipe(key, sizeof(key));
    c11_fail(app, "GmSSL Cookie MAC initialization failed");
    return -1;
  }
  c11_wipe(key, sizeof(key));
  config.local_port = 5000u;
  config.remote_port = 5000u;
  config.receive_window = CNET_SCTP_DATA_RECEIVE_CEILING;
  config.outbound_streams = CNET_SCTP_DATA_STREAMS;
  config.inbound_streams = CNET_SCTP_DATA_STREAMS;
  config.t1_ms = 1000u;
  config.t1_max_ms = 8000u;
  config.cookie_lifetime_ms = 30000u;
  config.peer_binding = binding;
  config.max_retransmits = 5u;
  config.is_owner = c11_is_owner;
  config.entropy = c11_entropy;
  config.mac = c11_mac_sign;
  config.verify_mac = c11_mac_verify;
  config.provider_context = app;
  status = cnet_sctp_assoc_init(&app->c11_assoc, &config, c11_clock(app));
  if (status != CNET_SCTP_ASSOC_OK) {
    cnet_sctp_gmssl_mac_retire(&app->c11_cookie_mac);
    c11_fail(app, "C11 SCTP association initialization failed");
    return -1;
  }
  app->c11_assoc_initialized = 1;
  status = cnet_sctp_assoc_start(&app->c11_assoc, c11_clock(app),
                                packet, sizeof(packet), &packet_size);
  if (status != CNET_SCTP_ASSOC_OK || packet_size == 0u) {
    c11_fail(app, "C11 SCTP INIT creation failed");
    return -1;
  }
  TLOG_INFO("Starting experimental C11 SCTP association over authenticated DTLS");
  return c11_send_packet(app, packet, packet_size);
}

static void c11_handle_dcep(app_state_t *app,
                            const cnet_sctp_data_message *message,
                            const uint8_t *payload, size_t payload_size) {
  uint8_t reply[CNET_SCTP_DCEP_OPEN_MAX];
  size_t reply_size = 0u;
  cnet_sctp_dcep_status status = cnet_sctp_dcep_input(
      &app->c11_dcep, message->stream_id, payload, payload_size,
      reply, sizeof(reply), &reply_size);
  if (status != CNET_SCTP_DCEP_OK) {
    c11_fail(app, "C11 DCEP rejected peer OPEN/ACK");
    return;
  }
  if (reply_size != 0u) {
    if (c11_send_message(app, message->stream_id, CNET_SCTP_DCEP_PPID,
                         reply, reply_size) != 0) return;
    status = cnet_sctp_dcep_ack_admitted(&app->c11_dcep, message->stream_id);
    if (status != CNET_SCTP_DCEP_OK) {
      c11_fail(app, "C11 DCEP ACK admission failed");
      return;
    }
  }
  /* Only ACK of our own OPEN is evidence that our local DataChannel exists.
   * A remote-initiated OPEN also becomes ready after we send its ACK, but
   * must never unlock the public test payload on a different SID. */
  if (app->is_offerer && app->janus_echo_mode && app->c11_dcep_open_sent &&
      reply_size == 0u && message->stream_id == c11_local_sid(app) &&
      cnet_sctp_dcep_ready(&app->c11_dcep, message->stream_id) &&
      !atomic_exchange(&app->message_sent, 1)) {
    static const char hello[] = "hello over signaled ICE";
    TLOG_INFOF("Private C11 DCEP ACK admitted on local SID {}",
               message->stream_id);
    if (c11_send_message(app, message->stream_id,
                         CNET_SCTP_WEBRTC_TEXT_PPID, hello, sizeof(hello) - 1u) != 0)
      return;
    TLOG_INFO("Private C11 SCTP sent exact Janus EchoTest payload");
  }
  if (app->is_offerer && app->janus_echo_mode &&
      app->c11_secondary_open_sent && reply_size == 0u &&
      message->stream_id == c11_unordered_sid(app) &&
      cnet_sctp_dcep_ready(&app->c11_dcep, message->stream_id)) {
    if (app->c11_reuse_open_sent) {
      if (app->c11_reuse_echo_sent ||
          !app->c11_reset_incoming_done || !app->c11_reset_outgoing_done) {
        c11_fail(app, "C11 reused DCEP ACK without bilateral reset");
        return;
      }
      if (c11_send_message_with_order(
              app, message->stream_id, CNET_SCTP_WEBRTC_BINARY_PPID, 1,
              c11_reused_echo_payload,
              sizeof(c11_reused_echo_payload)) != 0) return;
      app->c11_reuse_echo_sent = 1;
      TLOG_INFOF("Private C11 reused DCEP SID {} ACK; sent U=1 PPID 53",
                 message->stream_id);
    } else if (!app->c11_unordered_sent) {
      if (c11_send_message_with_order(
              app, message->stream_id, CNET_SCTP_WEBRTC_BINARY_PPID, 1,
              c11_unordered_echo_payload,
              sizeof(c11_unordered_echo_payload)) != 0) return;
      app->c11_unordered_sent = 1;
      TLOG_INFOF("Private C11 DCEP ACK on unordered SID {}; sent U=1 PPID 53",
                 message->stream_id);
    }
  }
  if (app->is_offerer && app->janus_echo_mode &&
      app->c11_pr_rtx_open_sent && !app->c11_pr_rtx_payload_sent &&
      reply_size == 0u && message->stream_id == c11_pr_rtx_sid(app) &&
      cnet_sctp_dcep_ready(&app->c11_dcep, message->stream_id)) {
    if (c11_send_pr_rtx1(app, message->stream_id, c11_pr_rtx_echo_payload,
                         sizeof(c11_pr_rtx_echo_payload)) != 0) return;
    app->c11_pr_rtx_payload_sent = 1;
    TLOG_INFOF("Private C11 PR-SCTP RTX1 ACK on SID {}; sent ordered PPID 53",
               message->stream_id);
  }
  if (app->is_offerer && app->janus_echo_mode &&
      app->c11_pr_zero_open_sent && !app->c11_pr_zero_payload_sent &&
      reply_size == 0u && message->stream_id == c11_pr_zero_sid(app) &&
      cnet_sctp_dcep_ready(&app->c11_dcep, message->stream_id)) {
    if (c11_send_pr_rtx0(app, message->stream_id,
                         c11_pr_zero_echo_payload,
                         sizeof(c11_pr_zero_echo_payload)) != 0) return;
    app->c11_pr_zero_payload_sent = 1;
    TLOG_INFOF("Private C11 PR-SCTP RTX0 ACK on SID {}; sent ordered PPID 53",
               message->stream_id);
  }
  if (app->is_offerer && app->janus_echo_mode &&
      app->c11_pr_timed_open_sent && !app->c11_pr_timed_payload_sent &&
      reply_size == 0u && message->stream_id == c11_pr_timed_sid(app) &&
      cnet_sctp_dcep_ready(&app->c11_dcep, message->stream_id)) {
    if (c11_send_pr_timed(app, message->stream_id,
                          c11_pr_timed_echo_payload,
                          sizeof(c11_pr_timed_echo_payload), 0) != 0)
      return;
    app->c11_pr_timed_payload_sent = 1;
    TLOG_INFOF("Private C11 DCEP TIMED SID {} ACK; nominal PPID53 sent",
               message->stream_id);
  }
}

static void c11_on_dtls_application_data(turbo_dc_peer_t *peer,
                                         const uint8_t *data, size_t size,
                                         void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  uint8_t output[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t output_size = 0u;
  cnet_sctp_assoc_status status;
  unsigned int attempts;
  if (!app || peer != app->dc_peer || !c11_is_owner(app) ||
      !app->c11_assoc_initialized || !data || size == 0u) {
    if (app) c11_fail(app, "invalid or non-owner DTLS application-data callback");
    return;
  }
  status = cnet_sctp_assoc_input(&app->c11_assoc, c11_clock(app), data, size,
                                 output, sizeof(output), &output_size);
  if (status != CNET_SCTP_ASSOC_OK) {
    TLOG_ERRORF("C11 SCTP input rejected status={} packet_bytes={}",
                (int)status, size);
    c11_fail(app, "C11 SCTP rejected authenticated DTLS application data");
    return;
  }
  if (c11_send_packet(app, output, output_size) != 0) return;
  if (c11_drain_reset_events(app) != 0) return;

  if (app->c11_assoc.state == CNET_SCTP_ASSOC_ESTABLISHED &&
      !app->c11_dcep_initialized) {
    if (c11_open_datachannel(app) != 0) return;
  }
  if (app->c11_assoc.state != CNET_SCTP_ASSOC_ESTABLISHED ||
      !app->c11_dcep_initialized) return;

  for (attempts = 0u; attempts < CNET_SCTP_DATA_RX_SLOTS; ++attempts) {
    cnet_sctp_data_message message = {0};
    uint8_t payload[CNET_SCTP_DATA_MESSAGE_MAX];
    size_t payload_size = 0u;
    status = cnet_sctp_assoc_take_message(&app->c11_assoc, &message,
                                          payload, sizeof(payload), &payload_size);
    if (status == CNET_SCTP_ASSOC_EMPTY) break;
    if (status != CNET_SCTP_ASSOC_OK) {
      c11_fail(app, "C11 SCTP message dequeue failed");
      return;
    }
    if (message.ppid == CNET_SCTP_DCEP_PPID) {
      c11_handle_dcep(app, &message, payload, payload_size);
      if (!atomic_load(&app->running)) return;
    } else if (message.ppid == CNET_SCTP_WEBRTC_TEXT_PPID &&
               app->is_offerer && app->janus_echo_mode) {
      static const char prefix[] = "Janus EchoTest here! You wrote: ";
      static const char expected[] = "hello over signaled ICE";
      /* Require an ACKed local DCEP channel and the exact text payload.
       * A second text reply before binary must fail, not complete the test. */
      if (!c11_echo_channel_ready(app, message.stream_id) ||
          app->c11_binary_echo_pending) {
        c11_fail(app, "Janus C11 text echo arrived on wrong SID or duplicate phase");
        return;
      }
      if (payload_size != sizeof(prefix) - 1u + sizeof(expected) - 1u ||
          memcmp(payload, prefix, sizeof(prefix) - 1u) != 0 ||
          memcmp(payload + sizeof(prefix) - 1u, expected, sizeof(expected) - 1u) != 0) {
        c11_fail(app, "Janus C11 SCTP text echo payload did not match exactly");
        return;
      }
      /* Janus EchoTest replies to binary messages byte-for-byte, unlike the
       * prefixed text reply. Verify a binary payload with NUL/high bytes on
       * the same negotiated SID before emitting any C11 success marker. */
      app->c11_binary_echo_pending = 1;
      if (c11_send_message(app, message.stream_id,
                           CNET_SCTP_WEBRTC_BINARY_PPID,
                           c11_binary_echo_payload,
                           sizeof(c11_binary_echo_payload)) != 0)
        return;
      TLOG_INFO("Private C11 SCTP exact text echo received; binary PPID 53 sent");
    } else if (message.ppid == CNET_SCTP_WEBRTC_BINARY_PPID &&
               app->is_offerer && app->janus_echo_mode) {
      if (message.stream_id == c11_pr_timed_sid(app)) {
        if (!app->c11_pr_timed_open_sent ||
            !app->c11_pr_timed_payload_sent ||
            !cnet_sctp_dcep_ready(&app->c11_dcep, message.stream_id) ||
            message.unordered) {
          c11_fail(app, "Janus C11 timed SID/ordered/DCEP mismatch");
          return;
        }
        if (!app->c11_pr_timed_loss_active) {
          if (payload_size != sizeof(c11_pr_timed_echo_payload) ||
              memcmp(payload, c11_pr_timed_echo_payload,
                     sizeof(c11_pr_timed_echo_payload)) != 0) {
            c11_fail(app, "Janus C11 timed nominal echo mismatch");
            return;
          }
          app->c11_pr_timed_loss_active = 1;
          app->c11_pr_loss_drops = 0;
          app->c11_pr_loss_budget = 1u;
          app->c11_pr_loss_tsn = 0u;
          app->c11_pr_fault_sid = message.stream_id;
          app->c11_pr_forward_sent = 0;
          app->c11_pr_timed_drop_ms = 0u;
          app->c11_pr_loss_active = 1;
          if (c11_send_pr_timed(app, message.stream_id,
                                c11_pr_timed_lost_payload,
                                sizeof(c11_pr_timed_lost_payload), 0) != 0 ||
              app->c11_pr_loss_drops != 1)
            return;
          TLOG_INFOF("Private C11 TIMED 500ms loss on SID {}; awaiting expiry",
                     message.stream_id);
          return;
        }
        if (!app->c11_pr_forward_sent ||
            !app->c11_pr_timed_recovery_sent ||
            app->c11_pr_loss_drops != 1 ||
            payload_size != sizeof(c11_pr_timed_recovery_payload) ||
            memcmp(payload, c11_pr_timed_recovery_payload,
                   sizeof(c11_pr_timed_recovery_payload)) != 0) {
          c11_fail(app, "Janus C11 TIMED expiry or ordered recovery mismatch");
          return;
        }
        puts("PUBLIC_JANUS_C11_SCTP_ECHO_PASS: text, binary, 20480-byte fragments, unordered, RFC6525 reset/reuse, RTX1+RTX0 loss and TIMED 500ms expiry FORWARD-TSN ordered recovery returned");
        atomic_store(&app->completed, 1);
        app_request_stop(app);
        return;
      }
      if (message.stream_id == c11_pr_zero_sid(app)) {
        if (!app->c11_pr_zero_open_sent || !app->c11_pr_zero_payload_sent ||
            !cnet_sctp_dcep_ready(&app->c11_dcep, message.stream_id) ||
            message.unordered) {
          c11_fail(app, "Janus C11 RTX0 SID/ordered/DCEP mismatch");
          return;
        }
        if (!app->c11_pr_zero_loss_active) {
          if (payload_size != sizeof(c11_pr_zero_echo_payload) ||
              memcmp(payload, c11_pr_zero_echo_payload,
                     sizeof(c11_pr_zero_echo_payload)) != 0) {
            c11_fail(app, "Janus C11 PR-SCTP RTX0 nominal echo mismatch");
            return;
          }
          app->c11_pr_zero_loss_active = 1;
          app->c11_pr_loss_drops = 0;
          app->c11_pr_loss_budget = 1u;
          app->c11_pr_loss_tsn = 0u;
          app->c11_pr_fault_sid = message.stream_id;
          app->c11_pr_forward_sent = 0;
          app->c11_pr_loss_active = 1;
          if (c11_send_pr_rtx0(app, message.stream_id,
                               c11_pr_zero_lost_payload,
                               sizeof(c11_pr_zero_lost_payload)) != 0)
            return;
          if (app->c11_pr_loss_drops != 1 ||
              c11_send_pr_rtx0(app, message.stream_id,
                               c11_pr_zero_recovery_payload,
                               sizeof(c11_pr_zero_recovery_payload)) != 0)
            return;
          app->c11_pr_zero_recovery_sent = 1;
          TLOG_INFOF("Private C11 RTX0 loss fixture armed on SID {}; ordered successor sent",
                     message.stream_id);
          return;
        }
        if (!app->c11_pr_zero_recovery_sent ||
            !app->c11_pr_forward_sent || app->c11_pr_loss_drops != 1 ||
            payload_size != sizeof(c11_pr_zero_recovery_payload) ||
            memcmp(payload, c11_pr_zero_recovery_payload,
                   sizeof(c11_pr_zero_recovery_payload)) != 0) {
          c11_fail(app, "Janus C11 RTX0 abandoned DATA leaked or recovery mismatch");
          return;
        }
        /* RTX1 and RTX0 loss/forward proof precedes a separately
         * negotiated TIMED DCEP channel on another local SID. */
        if (c11_open_pr_timed_channel(app) != 0) return;
        return;
      }
      if (message.stream_id == c11_pr_rtx_sid(app)) {
        if (!app->c11_pr_rtx_open_sent || !app->c11_pr_rtx_payload_sent ||
            !cnet_sctp_dcep_ready(&app->c11_dcep, message.stream_id) ||
            message.unordered) {
          c11_fail(app, "Janus C11 PR-SCTP RTX1 SID/ordered/DCEP mismatch");
          return;
        }
        if (!app->c11_pr_loss_active) {
          if (payload_size != sizeof(c11_pr_rtx_echo_payload) ||
              memcmp(payload, c11_pr_rtx_echo_payload,
                     sizeof(c11_pr_rtx_echo_payload)) != 0) {
            c11_fail(app, "Janus C11 initial PR-SCTP RTX1 echo mismatch");
            return;
          }
          /* Reuse this negotiated maxRetransmits=1 channel for one controlled
           * two-packet loss. The following distinct RTX1 DATA is NOT dropped,
           * and cannot be delivered in order until Janus processes FWD-TSN. */
          app->c11_pr_fault_sid = message.stream_id;
          app->c11_pr_loss_budget = 2u;
          app->c11_pr_loss_drops = 0;
          app->c11_pr_forward_sent = 0;
          app->c11_pr_loss_active = 1;
          if (c11_send_pr_rtx1(app, message.stream_id,
                               c11_pr_lost_payload,
                               sizeof(c11_pr_lost_payload)) != 0)
            return;
          if (app->c11_pr_loss_drops != 1 ||
              c11_send_pr_rtx1(app, message.stream_id,
                               c11_pr_recovery_payload,
                               sizeof(c11_pr_recovery_payload)) != 0)
            return;
          app->c11_pr_recovery_sent = 1;
          TLOG_INFOF("Private C11 RTX1 loss fixture armed on SID {}; recovery DATA sent",
                     message.stream_id);
          return;
        }
        if (!app->c11_pr_recovery_sent || !app->c11_pr_forward_sent ||
            app->c11_pr_loss_drops != 2 ||
            payload_size != sizeof(c11_pr_recovery_payload) ||
            memcmp(payload, c11_pr_recovery_payload,
                   sizeof(c11_pr_recovery_payload)) != 0) {
          c11_fail(app, "Janus C11 PR-SCTP abandoned DATA leaked or ordered recovery failed");
          return;
        }
        /* First prove the RTX1 loss/recovery independently, then transition
         * to a NEW DCEP RTX0 SID without weakening the prior assertions. */
        if (c11_open_pr_zero_channel(app) != 0) return;
        return;
      }
      if (message.stream_id == c11_unordered_sid(app)) {
        if (!app->c11_secondary_open_sent ||
            !cnet_sctp_dcep_ready(&app->c11_dcep, message.stream_id) ||
            !message.unordered) {
          c11_fail(app, "Janus C11 unordered echo SID, U flag or DCEP readiness mismatch");
          return;
        }
        if (app->c11_reuse_open_sent) {
          if (!app->c11_reset_incoming_done || !app->c11_reset_outgoing_done ||
              !app->c11_reuse_echo_sent ||
              payload_size != sizeof(c11_reused_echo_payload) ||
              memcmp(payload, c11_reused_echo_payload,
                     sizeof(c11_reused_echo_payload)) != 0) {
            c11_fail(app, "Janus C11 reused-SID echo bytes or reset state mismatch");
            return;
          }
          /* A reset/reuse PASS alone is no longer sufficient: negotiate an
           * independent maxRetransmits=1 channel and exercise the real
           * PR-SCTP send API through public Janus before declaring success. */
          if (c11_open_pr_rtx_channel(app) != 0) return;
          return;
        }
        if (!app->c11_unordered_sent ||
            payload_size != sizeof(c11_unordered_echo_payload) ||
            memcmp(payload, c11_unordered_echo_payload,
                   sizeof(c11_unordered_echo_payload)) != 0) {
          c11_fail(app, "Janus C11 reliable-unordered first echo bytes mismatch");
          return;
        }
        app->c11_reset_requested = 1;
        TLOG_INFOF("Private C11 independent unordered SID {} echo passed; RFC6525 reset requested",
                   message.stream_id);
        return;
      }
      if (!c11_echo_channel_ready(app, message.stream_id) ||
          !app->c11_binary_echo_pending) {
        c11_fail(app, "Janus C11 SCTP binary echo on an unacknowledged SID");
        return;
      }
      if (!app->c11_large_echo_pending) {
        if (payload_size != sizeof(c11_binary_echo_payload) ||
            memcmp(payload, c11_binary_echo_payload,
                   sizeof(c11_binary_echo_payload)) != 0) {
          c11_fail(app, "Janus C11 SCTP initial binary echo bytes did not match");
          return;
        }
        if (c11_queue_large_echo(app, message.stream_id) != 0) return;
      } else {
        size_t i;
        if (payload_size != C11_PUBLIC_FRAGMENT_BYTES) {
          c11_fail(app, "Janus C11 SCTP fragmented echo length did not match");
          return;
        }
        for (i = 0u; i < payload_size; ++i) {
          if (payload[i] != c11_fragment_byte(i)) {
            c11_fail(app, "Janus C11 SCTP fragmented echo bytes did not match");
            return;
          }
        }
        /* Do not report PASS until a second DCEP OPEN/ACK on a different,
         * reliable-unordered SID and its exact binary echo also succeed. */
        if (c11_open_unordered_channel(app) != 0) return;
        return;
      }
    } else {
      c11_fail(app, "unexpected PPID or DataChannel message for public EchoTest");
      return;
    }
  }
}

static void c11_cleanup_owner_task(void *arg1, void *arg2) {
  app_state_t *app = (app_state_t *)arg1;
  (void)arg2;
  if (!app || !c11_is_owner(app)) return;
  if (app->c11_dcep_initialized) {
    (void)cnet_sctp_dcep_stop(&app->c11_dcep);
    app->c11_dcep_initialized = 0;
  }
  if (app->c11_assoc_initialized) {
    (void)cnet_sctp_assoc_stop(&app->c11_assoc);
    if (cnet_sctp_assoc_destroy(&app->c11_assoc) != CNET_SCTP_ASSOC_OK) {
      TLOG_ERROR("Private C11 SCTP Association destroy failed during cleanup");
      atomic_store(&app->callback_failed, 1);
    }
    app->c11_assoc_initialized = 0;
  }
  cnet_sctp_gmssl_mac_retire(&app->c11_cookie_mac);
}

static void c11_poll_owner_task(void *arg1, void *arg2) {
  app_state_t *app = (app_state_t *)arg1;
  uint8_t output[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t output_size = 0u;
  uint64_t deadline = 0u;
  int has_deadline = 0;
  cnet_sctp_assoc_status status;
  unsigned int fragments;
  (void)arg2;
  if (!app || !app->c11_assoc_initialized || !c11_is_owner(app)) return;
  status = cnet_sctp_assoc_next_deadline(&app->c11_assoc, &deadline, &has_deadline);
  if (status != CNET_SCTP_ASSOC_OK) {
    c11_fail(app, "C11 SCTP deadline query failed");
    return;
  }
  /* A due T1/T3 or fast retransmission always precedes new fragments.
   * Every packet stays on the existing Peer Owner and encrypted DTLS path. */
  if (has_deadline && c11_clock(app) >= deadline) {
    status = cnet_sctp_assoc_advance(&app->c11_assoc, c11_clock(app),
                                    output, sizeof(output), &output_size);
    if (status != CNET_SCTP_ASSOC_OK) {
      c11_fail(app, "C11 SCTP retransmission deadline failed");
      return;
    }
    if (c11_send_packet(app, output, output_size) != 0) return;
    if (c11_drain_reset_events(app) != 0) return;
  }
  if (app->c11_assoc.state == CNET_SCTP_ASSOC_FAILED) {
    c11_fail(app, "C11 SCTP association exhausted its retry budget");
    return;
  }
  /* A fresh TIMED successor starts only after the real peer ACKs FWD-TSN,
   * avoiding expiration behind an outstanding abandoned-record barrier. */
  if (app->c11_pr_timed_loss_active && app->c11_pr_forward_sent &&
      !app->c11_pr_timed_recovery_sent &&
      !app->c11_assoc.data.forward_active) {
    const int sent = c11_send_pr_timed(app, c11_pr_timed_sid(app),
                          c11_pr_timed_recovery_payload,
                          sizeof(c11_pr_timed_recovery_payload), 1);
    if (sent < 0) return;
    if (sent == 0) {
      app->c11_pr_timed_recovery_sent = 1;
      TLOG_INFOF("Private C11 TIMED FORWARD-TSN ACKed; ordered successor sent on SID {}",
                 c11_pr_timed_sid(app));
    }
  }
  if (app->c11_reset_requested && !app->c11_reset_sent) {
    output_size = 0u;
    status = cnet_sctp_assoc_reset_stream(&app->c11_assoc, c11_clock(app),
               c11_unordered_sid(app), output, sizeof(output), &output_size);
    if (status == CNET_SCTP_ASSOC_BUSY ||
        status == CNET_SCTP_ASSOC_RETRANSMIT_PENDING) {
      /* Wait for outstanding DATA to retire; never reset an inflight SID. */
      return;
    }
    if (status != CNET_SCTP_ASSOC_OK || output_size == 0u) {
      TLOG_ERRORF("C11 RFC6525 outgoing reset rejected status={}", (int)status);
      c11_fail(app, "C11 SCTP outgoing stream reset denied");
      return;
    }
    app->c11_reset_sent = 1;
    if (c11_send_packet(app, output, output_size) != 0) return;
    TLOG_INFOF("Private C11 RFC6525 outgoing reset sent for SID {}",
               c11_unordered_sid(app));
  }
  if (!app->c11_large_echo_pending ||
      app->c11_assoc.state != CNET_SCTP_ASSOC_ESTABLISHED) return;

  /* Bounded per-tick pump; DATA cwnd/rwnd/ticket pressure is normal and
   * retried after a peer SACK or the next Owner deadline. No extra worker,
   * socket, busy-wait, unbounded queue or implicit application fallback. */
  for (fragments = 0u; fragments < 4u; ++fragments) {
    output_size = 0u;
    status = cnet_sctp_assoc_pump_message(&app->c11_assoc, c11_clock(app),
                                          output, sizeof(output), &output_size);
    if (status == CNET_SCTP_ASSOC_EMPTY ||
        status == CNET_SCTP_ASSOC_CAPACITY ||
        status == CNET_SCTP_ASSOC_RETRANSMIT_PENDING) break;
    if (status != CNET_SCTP_ASSOC_OK || output_size == 0u) {
      c11_fail(app, "C11 SCTP fragmented DATA pump failed");
      return;
    }
    if (c11_send_packet(app, output, output_size) != 0) return;
  }
}

#endif /* TURBO_MEDIA_TEST_PUBLIC_C11_SCTP helpers */

static const char *dc_state_name(turbo_dc_state_t state) {
  switch (state) {
    case TURBO_DC_STATE_NEW: return "NEW";
    case TURBO_DC_STATE_CONNECTING: return "CONNECTING";
    case TURBO_DC_STATE_CONNECTED: return "CONNECTED";
    case TURBO_DC_STATE_DISCONNECTING: return "DISCONNECTING";
    case TURBO_DC_STATE_CLOSED: return "CLOSED";
    case TURBO_DC_STATE_FAILED: return "FAILED";
    default: return "UNKNOWN";
  }
}

static void on_dc_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                        turbo_dc_state_t new_state, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)peer;
  TLOG_INFOF("DC state: {} -> {}", dc_state_name(old_state), dc_state_name(new_state));

#ifdef TURBO_MEDIA_TEST_PUBLIC_C11_SCTP
  if (new_state == TURBO_DC_STATE_CONNECTED && !app->c11_assoc_initialized) {
    if (c11_start_association(app) != 0) app_request_stop(app);
  }
#else
  if (new_state == TURBO_DC_STATE_CONNECTED && !atomic_load(&app->channel)) {
    turbo_dc_channel_t *channel = turbo_dc_channel_create(app->dc_peer, "chat", NULL);
    if (channel) {
      if (!app->is_offerer) {
        TLOG_INFO("Answerer creating fallback chat channel");
      }
      turbo_dc_channel_set_user_data(channel, app);
      turbo_dc_channel_on_open(channel, on_dc_open);
      turbo_dc_channel_on_message(channel, on_dc_message);
      atomic_store(&app->channel, channel);
      turbo_dc_channel_open(channel);
    }
  }
#endif

  if (new_state == TURBO_DC_STATE_FAILED || new_state == TURBO_DC_STATE_CLOSED) {
    app_request_stop(app);
  }
}

static void on_dc_error(turbo_dc_peer_t *peer, int error_code, const char *error_msg,
                        void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)peer;
  TLOG_ERRORF("DataChannel error {}: {}", error_code, error_msg);
  app_request_stop(app);
}

static int ensure_media_stack(app_state_t *app) {
  turbo_dc_config_t dc_cfg;
  const char *stun_servers[] = {"stun:stun.l.google.com:19302"};
  int loopback;

  if (app->media_ready) {
    return 0;
  }

  loopback = signal_host_is_loopback(app->signal_host) && !app->force_stun;

  TLOG_INFO("ensure_media_stack: creating DataChannel context");
  memset(&dc_cfg, 0, sizeof(dc_cfg));
  dc_cfg.is_server = app->is_offerer ? 0 : 1;
#ifdef TURBO_MEDIA_TEST_PUBLIC_C11_SCTP
  /* Keep production usrsctp dormant in this private C11 interoperability target. */
  dc_cfg.disable_sctp = 1;
#endif
  dc_cfg.transport = TURBO_DC_TRANSPORT_ICE;
  dc_cfg.dtls_mtu = 1200;
  dc_cfg.sctp_mtu = 1000;

  app->dc_ctx = turbo_dc_context_create(&dc_cfg);
  if (!app->dc_ctx) {
    TLOG_ERROR("Failed to create DataChannel context");
    return -1;
  }

  TLOG_INFO("ensure_media_stack: creating DataChannel peer");
  app->dc_peer = turbo_dc_peer_create(app->dc_ctx, NULL, 0, app);
  if (!app->dc_peer) {
    TLOG_ERROR("Failed to create DataChannel peer");
    turbo_dc_context_destroy(app->dc_ctx);
    app->dc_ctx = NULL;
    return -1;
  }

  turbo_dc_peer_on_state(app->dc_peer, on_dc_state);
  turbo_dc_peer_on_channel(app->dc_peer, on_dc_channel);
  turbo_dc_peer_on_error(app->dc_peer, on_dc_error);
#ifdef TURBO_MEDIA_TEST_PUBLIC_C11_SCTP
  if (dc_peer_set_test_dtls_application_handler(
          app->dc_peer, c11_on_dtls_application_data, app) != 0) {
    TLOG_ERROR("Failed to install private C11 SCTP DTLS application handler");
    turbo_dc_peer_destroy(app->dc_peer);
    app->dc_peer = NULL;
    turbo_dc_context_destroy(app->dc_ctx);
    app->dc_ctx = NULL;
    return -1;
  }
#endif

  app->ice = ice_integration_create(app->dc_peer, NULL,
                                    loopback ? NULL : stun_servers,
                                    loopback ? 0 : 1,
                                    NULL, NULL, NULL, 0);
  if (!app->ice) {
    TLOG_ERROR("Failed to attach SaltsNet ICE transport to DataChannel peer");
    turbo_dc_peer_destroy(app->dc_peer);
    app->dc_peer = NULL;
    turbo_dc_context_destroy(app->dc_ctx);
    app->dc_ctx = NULL;
    return -1;
  }
  ice_integration_set_allow_loopback(app->ice, loopback);
  ice_integration_on_state_change(app->ice, on_ice_state_change, app);
  ice_integration_on_candidate(app->ice, on_ice_candidate, app);
  app->media_ready = 1;
  return 0;
}

static int ensure_media_runtime(app_state_t *app) {
  TLOG_INFO("ensure_media_runtime: ensure_media_stack");
  if (ensure_media_stack(app) != 0) {
    return -1;
  }

  if (!app->gather_task_started) {
    int rc;
    TLOG_INFO("ensure_media_runtime: start candidate gathering");
    rc = ice_integration_start_gathering(app->ice);
    if (rc != 0) {
      TLOG_ERRORF("Failed to gather ICE candidates: {}", rc);
      app_request_stop(app);
      return -1;
    }
    app->gather_task_started = 1;
  }

  TLOG_INFO("ensure_media_runtime: ready");
  return 0;
}

static native_io_backend_kind signal_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_websocket_client_config signal_client_config(void) {
  chttp_websocket_client_config config = {0};

  config.size = sizeof(config);
  config.network.backend = signal_backend();
  config.network.connection_capacity = 1;
  config.network.command_capacity = 16;
  config.network.request_capacity = 16;
  config.network.completion_batch_capacity = 16;
  config.network.event_capacity = 16;
  config.network.max_send_bytes = SIGNAL_MAX_MESSAGE_BYTES;
  config.network.receive_buffer_bytes = SIGNAL_MAX_MESSAGE_BYTES;
  config.network.connect_timeout_ms = SIGNAL_OPERATION_TIMEOUT_MS;
  /* The per-call receive timeout drives polling; the transport-level idle
   * timeout must stay disabled or it closes signaling while Janus processes
   * an asynchronous SDP answer. */
  config.network.read_timeout_ms = 0u;
  config.network.write_timeout_ms = SIGNAL_OPERATION_TIMEOUT_MS;
  config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  config.network.tls_handshake_timeout_ms = SIGNAL_OPERATION_TIMEOUT_MS;
  config.network.command_buffer_bytes = SIGNAL_MAX_MESSAGE_BYTES;
  config.network.event_buffer_bytes = SIGNAL_MAX_MESSAGE_BYTES;
  /* CHTTP reserves WebSocket framing overhead inside max_send_bytes. */
  config.max_frame_bytes = SIGNAL_MAX_MESSAGE_BYTES - CNET_WEBSOCKET_MAX_HEADER_BYTES;
  config.max_message_bytes = SIGNAL_MAX_MESSAGE_BYTES;
  config.max_buffered_input_bytes = SIGNAL_MAX_MESSAGE_BYTES;
  config.max_handshake_header_bytes = SIGNAL_MAX_HANDSHAKE_HEADER_BYTES;
  config.event_capacity = 16;
  return config;
}

static void update_ice_progress(app_state_t *app) {
  if (!app->ice) {
    return;
  }

  ice_integration_poll(app->ice);
  drain_pending_candidates(app);
  if (!app->local_gathering_complete &&
      ice_integration_is_gathering_complete(app->ice)) {
    app->local_gathering_complete = 1;
    app->local_end_of_candidates_pending = 1;
    TLOG_INFO("ICE gathering: COMPLETE");
    flush_cached_candidates(app);
    flush_end_of_candidates(app);
    if (app->answer_pending) {
      app->answer_pending = 0;
      send_answer(app);
    }
    maybe_start_checks(app);
  }
}

static int run_signaling(app_state_t *app) {
  chttp_websocket_client_config config = signal_client_config();
  chttp_websocket_connect_options options = {0};
  chttp_websocket_event event = {0};
  char uri[512];
  const char *uri_format = strchr(app->signal_host, ':')
                               ? "%s://[%s]:%u%s"
                               : "%s://%s:%u%s";
  unsigned int http_status = 0;
  int rc;
  int uri_len;

  uri_len = snprintf(uri, sizeof(uri), uri_format,
                     app->use_tls ? "wss" : "ws", app->signal_host,
                     (unsigned int)app->signal_port, app->signal_path);
  if (uri_len < 0 || (size_t)uri_len >= sizeof(uri)) {
    return -1;
  }
  rc = chttp_websocket_client_init(&app->signal_client, &config);
  if (rc != SALTS_OK) {
    TLOG_ERRORF("Failed to initialize signaling WebSocket: {} ({})", rc,
                cmeta_strerror(rc));
    return -1;
  }
  app->signal_client_initialized = 1;

  options.size = sizeof(options);
  options.uri = uri;
  options.timeout_ms = SIGNAL_OPERATION_TIMEOUT_MS;
  options.protocol = CHTTP_HTTP_1_1;
  options.subprotocol = "webrtc-signaling";
  rc = chttp_websocket_client_connect(&app->signal_client, &options,
                                      &http_status);
  if (rc != SALTS_OK) {
    TLOG_ERRORF("Failed to connect to signaling WebSocket: {} ({}) HTTP {}",
                rc, cmeta_strerror(rc), http_status);
    return -1;
  }

  app->ws_connected = 1;
  TLOG_INFOF("Connected to signaling server at {}:{}{}", app->signal_host, app->signal_port,
            app->signal_path);

  if (app->join_token) {
    enqueue_signalf(app,
                    "{\"type\":\"join\",\"room\":\"%s\","
                    "\"peer_id\":\"%s\",\"token\":\"%s\"}",
                    app->room, app->requested_peer_id, app->join_token);
  } else {
    enqueue_signalf(app, "{\"type\":\"join\",\"room\":\"%s\"}", app->room);
  }
  if (flush_signal_outbox(app) != 0) {
    TLOG_ERROR("Failed to send initial join message");
    return -1;
  }

  while (atomic_load(&app->running)) {
    update_ice_progress(app);
    if (atomic_load(&app->callback_failed)) {
      TLOG_ERROR("ICE callback queue capacity exceeded");
      break;
    }

    if (app->dc_peer) {
      turbo_dc_handle_timers();
      turbo_dc_peer_poll(app->dc_peer);
#ifdef TURBO_MEDIA_TEST_PUBLIC_C11_SCTP
      if (app->c11_assoc_initialized) {
        (void)dc_peer_post_owner_sync(app->dc_peer, c11_poll_owner_task, app, NULL);
      }
#endif
    }

    if (app->is_offerer && app->remote_peer_id[0] == '\0' && app->ws_connected) {
      uint64_t now_ms = cmeta_monotonic_ms();
      if (app->last_peer_query_ms == 0 || now_ms - app->last_peer_query_ms >= 1000) {
        request_peer_list(app);
      }
    }

    if (flush_signal_outbox(app) != 0) {
      TLOG_ERROR("Failed to send signaling message");
      break;
    }

    memset(&event, 0, sizeof(event));
    rc = chttp_websocket_client_receive(&app->signal_client,
                                        SIGNAL_RECEIVE_TIMEOUT_MS, &event);
    if (!atomic_load(&app->running)) {
      break;
    }

    if (rc == SALTS_ETIMEDOUT) {
      continue;
    }
    if (rc != SALTS_OK) {
      TLOG_ERRORF("Signaling socket receive failed: {} ({})", rc, cmeta_strerror(rc));
      break;
    }
    if (event.kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
      TLOG_ERROR("Signaling WebSocket received CLOSE event from server");
      break;
    }
    if (event.kind == CHTTP_WEBSOCKET_EVENT_PING) {
      if (chttp_websocket_client_send_pong(&app->signal_client, event.data,
                                           event.size,
                                           SIGNAL_OPERATION_TIMEOUT_MS) != SALTS_OK) {
        break;
      }
      continue;
    }
    if (event.kind != CHTTP_WEBSOCKET_EVENT_MESSAGE ||
        event.message_type != CHTTP_WEBSOCKET_MESSAGE_TEXT) {
      TLOG_ERROR("Signaling server sent a non-text WebSocket message");
      break;
    }

    process_signaling_message(app, (const char *)event.data, event.size);
  }

  app->ws_connected = 0;
  app_request_stop(app);
  return atomic_load(&app->completed) ? 0 : -1;
}

static void print_usage(const char *argv0) {
  fprintf(stderr,
          "Usage: %s (--offer|--answer) [--room name] [--server host] [--port port]\n"
          "          [--path /ws] [--secure] [--peer-id id] [--force-stun] [--janus-echo]\n"
          "  --force-stun   Use public STUN even when signaling is loopback.\n"
          "  --janus-echo   Require the public Janus EchoTest response payload.\n"
          "Set TURBO_SIGNALING_PEER_TOKEN for authenticated admission.\n",
          argv0);
}

int main(int argc, char **argv) {
  int i;
  int run_result;
  int role_specified = 0;
  app_state_t app;

  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  memset(&app, 0, sizeof(app));
  atomic_init(&app.running, 1);
  atomic_init(&app.channel, NULL);
  atomic_init(&app.message_sent, 0);
  atomic_init(&app.completed, 0);
  atomic_init(&app.callback_failed, 0);
  atomic_init(&app.pending_candidate_write, 0);
  atomic_init(&app.pending_candidate_read, 0);
  app.signal_port = 8080;
  snprintf(app.room, sizeof(app.room), "default");
  snprintf(app.signal_host, sizeof(app.signal_host), "127.0.0.1");
  snprintf(app.signal_path, sizeof(app.signal_path), "/");
  app.join_token = getenv("TURBO_SIGNALING_PEER_TOKEN");
  {
    const char *peer_id = getenv("TURBO_SIGNALING_PEER_ID");
    if (peer_id && peer_id[0] != '\0') {
      snprintf(app.requested_peer_id, sizeof(app.requested_peer_id), "%s",
               peer_id);
    }
  }

  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--offer") == 0) {
      app.is_offerer = 1;
      role_specified = 1;
    } else if (strcmp(argv[i], "--answer") == 0) {
      app.is_offerer = 0;
      role_specified = 1;
    } else if (strcmp(argv[i], "--room") == 0 && i + 1 < argc) {
      snprintf(app.room, sizeof(app.room), "%s", argv[++i]);
    } else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
      snprintf(app.signal_host, sizeof(app.signal_host), "%s", argv[++i]);
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      app.signal_port = (uint16_t)atoi(argv[++i]);
    } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
      snprintf(app.signal_path, sizeof(app.signal_path), "%s", argv[++i]);
    } else if (strcmp(argv[i], "--secure") == 0) {
      app.use_tls = 1;
    } else if (strcmp(argv[i], "--force-stun") == 0) {
      app.force_stun = 1;
    } else if (strcmp(argv[i], "--janus-echo") == 0) {
      app.janus_echo_mode = 1;
    } else if (strcmp(argv[i], "--peer-id") == 0 && i + 1 < argc) {
      snprintf(app.requested_peer_id, sizeof(app.requested_peer_id), "%s",
               argv[++i]);
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  if (!role_specified) {
    print_usage(argv[0]);
    return 1;
  }
  if (app.join_token && app.join_token[0] == '\0') {
    app.join_token = NULL;
  }
  if ((app.join_token && app.requested_peer_id[0] == '\0') ||
      (!app.join_token && app.requested_peer_id[0] != '\0')) {
    fprintf(stderr,
            "Authenticated signaling requires both a peer ID and "
            "TURBO_SIGNALING_PEER_TOKEN.\n");
    return 1;
  }

  TLOG_INFO("=== WebRTC Signaled Peer Example ===");
  TLOG_INFOF("Mode: {}", app.is_offerer ? "offerer" : "answerer");
  TLOG_INFOF("Room: {}", app.room);

  run_result = run_signaling(&app);

  if (app.signal_client_initialized) {
    (void)chttp_websocket_client_close(&app.signal_client, 1000, NULL, 0,
                                       SIGNAL_SHUTDOWN_TIMEOUT_MS);
    (void)chttp_websocket_client_destroy(&app.signal_client,
                                         SIGNAL_SHUTDOWN_TIMEOUT_MS);
    app.signal_client_initialized = 0;
  }
  free_signal_outbox(&app);
#ifdef TURBO_MEDIA_TEST_PUBLIC_C11_SCTP
  if (app.dc_peer && (app.c11_assoc_initialized || app.c11_dcep_initialized)) {
    (void)dc_peer_post_owner_sync(app.dc_peer, c11_cleanup_owner_task, &app, NULL);
  }
#endif
  if (app.ice) {
    ice_integration_destroy(app.ice);
  }
  if (app.dc_peer) {
    turbo_dc_peer_destroy(app.dc_peer);
  }
  if (app.dc_ctx) {
    turbo_dc_context_destroy(app.dc_ctx);
  }

  return run_result == 0 && atomic_load(&app.completed) &&
                 !atomic_load(&app.callback_failed)
             ? 0
             : 1;
}
