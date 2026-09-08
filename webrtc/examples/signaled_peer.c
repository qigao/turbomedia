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
#include <chttp/chttp.h>
#include <json_parser.h>
#include "turbo_sdp.h"
#include <salts_error.h>
#include <salts_thread.h>
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
#define SIGNAL_MAX_MESSAGE_BYTES (64u * 1024u)
#define SIGNAL_MAX_HANDSHAKE_HEADER_BYTES (32u * 1024u)
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
static char *dup_printf(const char *fmt, ...);
static int ensure_media_runtime(app_state_t *app);
static int enqueue_signalf(app_state_t *app, const char *fmt, ...);

static void request_peer_list(app_state_t *app) {
  if (!app || !app->ws_connected) {
    return;
  }
  if (enqueue_signalf(app, "{\"type\":\"list-peers\"}") == 0) {
    app->last_peer_query_ms = salts_monotonic_ms();
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

static char *dup_printf(const char *fmt, ...) {
  char *buffer;
  int needed;
  va_list ap;

  va_start(ap, fmt);
#ifdef _WIN32
  needed = _vscprintf(fmt, ap);
#else
  {
    va_list ap_copy;
    va_copy(ap_copy, ap);
    needed = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
  }
#endif
  va_end(ap);

  if (needed < 0) {
    return NULL;
  }

  buffer = (char *)malloc((size_t)needed + 1);
  if (!buffer) {
    return NULL;
  }

  va_start(ap, fmt);
  vsnprintf(buffer, (size_t)needed + 1, fmt, ap);
  va_end(ap);
  return buffer;
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

    TLOG_INFOF("Sending signaling message: {}", msg->json);
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
    snprintf(app->emitted_candidates[app->emitted_candidate_count],
             sizeof(app->emitted_candidates[app->emitted_candidate_count]), "%s", candidate_sdp);
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
    snprintf(app->cached_candidates[app->cached_candidate_count],
             sizeof(app->cached_candidates[app->cached_candidate_count]), "%s", candidate_sdp);
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

  if (ice_integration_set_remote_credentials(app->ice, media->ice_ufrag,
                                             media->ice_pwd) != 0) {
    TLOG_ERROR("Failed to apply remote ICE credentials");
    return -1;
  }
  app->remote_credentials_set = 1;

  if (media->fingerprint_hash[0] != '\0' && media->fingerprint[0] != '\0') {
    if (turbo_dc_peer_set_remote_fingerprint(app->dc_peer, media->fingerprint_hash,
                                             media->fingerprint) != 0) {
      TLOG_WARN("Failed to set remote fingerprint from SDP");
    }
  }

  for (i = 0; i < media->candidate_count; ++i) {
    if (sdp_candidate_to_string(&media->candidates[i], candidate_line, sizeof(candidate_line)) == 0) {
      if (ice_integration_add_remote_candidate(app->ice, candidate_line) == 0) {
        app->remote_candidate_count++;
      }
    }
  }

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
    TLOG_ERRORF("Signaling server returned an error: {}", json_text);
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
  } else if (app->is_offerer && strncmp(message, "ECHO:", 5) == 0) {
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

static void on_dc_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                        turbo_dc_state_t new_state, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)peer;
  TLOG_INFOF("DC state: {} -> {}", ENUM_NAME(old_state), ENUM_NAME(new_state));

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

  loopback = signal_host_is_loopback(app->signal_host);

  TLOG_INFO("ensure_media_stack: creating DataChannel context");
  memset(&dc_cfg, 0, sizeof(dc_cfg));
  dc_cfg.is_server = app->is_offerer ? 0 : 1;
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
  config.network.read_timeout_ms = SIGNAL_RECEIVE_TIMEOUT_MS;
  config.network.write_timeout_ms = SIGNAL_OPERATION_TIMEOUT_MS;
  config.network.tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  config.network.tls_handshake_timeout_ms = SIGNAL_OPERATION_TIMEOUT_MS;
  config.network.command_buffer_bytes = SIGNAL_MAX_MESSAGE_BYTES;
  config.network.event_buffer_bytes = SIGNAL_MAX_MESSAGE_BYTES;
  config.max_frame_bytes = SIGNAL_MAX_MESSAGE_BYTES;
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
                salts_strerror(rc));
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
                rc, salts_strerror(rc), http_status);
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
    }

    if (app->is_offerer && app->remote_peer_id[0] == '\0' && app->ws_connected) {
      uint64_t now_ms = salts_monotonic_ms();
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
      TLOG_ERRORF("Signaling socket receive failed: {} ({})", rc, salts_strerror(rc));
      break;
    }
    if (event.kind == CHTTP_WEBSOCKET_EVENT_CLOSE) {
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
          "          [--path /ws] [--secure] [--peer-id id]\n"
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
