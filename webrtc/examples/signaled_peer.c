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

#include "ice/turbo_ice.h"
#include "tlog.h"
#include "turbo_datachannel.h"
#include "turbo_parser.h"
#include "turbo_sdp.h"
#include <CoroNet/turbo_coro_context.h>
#include <CoroNet/turbo_coro_socket.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROOM_LEN 64
#define MAX_PEER_ID_LEN 64
#define MAX_SIGNAL_HOST_LEN 256
#define MAX_SIGNAL_PATH_LEN 128
#define MAX_CACHED_CANDIDATES 32

typedef struct signal_message_s signal_message_t;

typedef struct {
  ice_config_t config;
  coro_context_t *ctx;
  ice_state_t state;
  ice_gathering_state_t gathering_state;
  ice_role_t role;
  uint64_t tie_breaker;
  char local_ufrag[32];
  char local_pwd[64];
  char remote_ufrag[32];
  char remote_pwd[64];
  ice_candidate_t local_candidates[ICE_MAX_CANDIDATES];
  int local_candidate_count;
} ice_agent_layout_t;

struct signal_message_s {
  char *json;
  signal_message_t *next;
};

typedef struct {
  coro_context_t *ctx;
  coro_socket_t *signal_socket;

  turbo_ice_agent_t *ice_agent;
  turbo_dc_context_t *dc_ctx;
  turbo_dc_peer_t *dc_peer;
  turbo_dc_channel_t *channel;

  int is_offerer;
  int running;
  int ws_connected;
  int local_gathering_complete;
  int remote_credentials_set;
  int remote_candidate_count;
  int remote_end_of_candidates;
  int media_ready;
  int signal_task_started;
  int signal_task_done;
  int gather_task_started;
  int gather_task_done;
  int checks_task_started;
  int checks_task_done;
  int dc_connect_started;
  int timer_task_started;
  int timer_task_done;
  int context_stop_requested;
  int local_end_of_candidates_pending;
  int local_end_of_candidates_sent;
  int local_description_sent;
  int answer_pending;
  int message_sent;
  int completed;
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

static void signal_task(coro_t *co, void *arg);
static void ice_gather_task(coro_t *co, void *arg);
static void ice_checks_task(coro_t *co, void *arg);
static void dc_timer_task(coro_t *co, void *arg);
static char *dup_printf(const char *fmt, ...);
static int ensure_media_runtime(app_state_t *app);
static int enqueue_signalf(app_state_t *app, const char *fmt, ...);

static void wake_context_post(void *arg1, void *arg2) {
  (void)arg1;
  (void)arg2;
}

static void request_peer_list(app_state_t *app) {
  if (!app || !app->ws_connected) {
    return;
  }
  if (enqueue_signalf(app, "{\"type\":\"list-peers\"}") == 0) {
    app->last_peer_query_ms = turbo_monotonic_ms();
  }
}


static int signal_host_is_loopback(const char *host) {
  if (!host) {
    return 0;
  }
  return strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0 ||
         strcmp(host, "::1") == 0;
}

static void maybe_stop_context(app_state_t *app) {
  if (!app || !app->ctx || app->running || app->context_stop_requested) {
    return;
  }
  if (app->signal_task_started && !app->signal_task_done) {
    return;
  }
  if (app->gather_task_started && !app->gather_task_done) {
    return;
  }
  if (app->checks_task_started && !app->checks_task_done) {
    return;
  }
  if (app->timer_task_started && !app->timer_task_done) {
    return;
  }

  app->context_stop_requested = 1;
  coro_context_stop(app->ctx);
}

static void copy_json_string(json_value_t *value, char *buffer, size_t buffer_size) {
  const char *str = turbo_json_string(value);
  size_t len = turbo_json_string_len(value);
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

static void wake_signal_task(app_state_t *app) {
  if (app->signal_socket) {
    coro_socket_interrupt_wait(app->signal_socket, 0);
  }
}

static void app_request_stop(app_state_t *app) {
  if (!app || !app->running) {
    return;
  }

  app->running = 0;
  if (app->ice_agent) {
    ice_agent_close(app->ice_agent);
  }
  wake_signal_task(app);
  if (app->ctx) {
    coro_post(app->ctx, wake_context_post, NULL, NULL);
  }
  maybe_stop_context(app);
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
  wake_signal_task(app);
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

    TLOG_INFO("Sending signaling message: {}", msg->json);
    if (app->signal_socket &&
        coro_socket_send_ws_text(app->signal_socket, msg->json, strlen(msg->json)) != 0) {
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

static void emit_all_local_candidates(app_state_t *app) {
  int i;
  int count;

  if (!app || !app->ice_agent) {
    return;
  }

  count = ice_agent_get_local_candidate_count(app->ice_agent);
  for (i = 0; i < count; ++i) {
    ice_candidate_t candidate;
    char candidate_sdp[512];

    if (ice_agent_get_local_candidate(app->ice_agent, i, &candidate) != 0) {
      continue;
    }
    if (ice_candidate_to_sdp(&candidate, candidate_sdp, sizeof(candidate_sdp)) <= 0) {
      continue;
    }

    emit_local_candidate_sdp(app, candidate_sdp);
  }
}

static void repair_local_candidate_ports(app_state_t *app) {
  ice_agent_layout_t *layout;
  int i;

  if (!app || !app->ice_agent) {
    return;
  }

  layout = (ice_agent_layout_t *)app->ice_agent;
  for (i = 0; i < layout->local_candidate_count; ++i) {
    ice_candidate_t *candidate = &layout->local_candidates[i];
    struct sockaddr_storage local_addr;

    if (candidate->port != 0 || !candidate->socket || candidate->family != AF_INET) {
      continue;
    }

    memset(&local_addr, 0, sizeof(local_addr));
    if (coro_socket_get_local_address((coro_socket_t *)candidate->socket, &local_addr) == 0 &&
        local_addr.ss_family == AF_INET) {
      struct sockaddr_in *addr4 = (struct sockaddr_in *)&local_addr;
      candidate->port = ntohs(addr4->sin_port);
      TLOG_INFO("Repaired local candidate {}:{} type={}", candidate->ip, candidate->port,
                ice_candidate_type_name(candidate->type));
    }
  }
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
  int local_candidate_count;

  if (app->checks_task_started) {
    tracef_app(app, "maybe_start_checks skip: already started");
    return;
  }
  if (!app->ice_agent || !app->remote_credentials_set) {
    tracef_app(app, "maybe_start_checks wait: ice_agent=%p remote_credentials_set=%d",
               (void *)app->ice_agent, app->remote_credentials_set);
    return;
  }
  if (!app->local_gathering_complete) {
    tracef_app(app, "maybe_start_checks wait: local_gathering_complete=%d",
               app->local_gathering_complete);
    return;
  }
  if (ice_agent_get_gathering_state(app->ice_agent) != ICE_GATHERING_COMPLETE) {
    tracef_app(app, "maybe_start_checks wait: gathering_state=%d",
               (int)ice_agent_get_gathering_state(app->ice_agent));
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
  local_candidate_count = ice_agent_get_local_candidate_count(app->ice_agent);
  if (local_candidate_count <= 0) {
    tracef_app(app, "maybe_start_checks wait: local_candidate_count=%d", local_candidate_count);
    return;
  }

  tracef_app(app, "maybe_start_checks start: local_candidate_count=%d remote_candidate_count=%d",
             local_candidate_count, app->remote_candidate_count);
  if (!app->ctx) {
    tracef_app(app, "maybe_start_checks failed: missing context");
    app_request_stop(app);
    return;
  }
  if (coro_context_spawn(app->ctx, ice_checks_task, app) != 0) {
    tracef_app(app, "maybe_start_checks failed: spawn");
    TLOG_ERROR("Failed to spawn ICE checks task");
    app_request_stop(app);
    return;
  }
  if (app->signal_socket) {
    coro_socket_set_timeout(app->signal_socket, 50);
    wake_signal_task(app);
  }
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
  ice_agent_get_local_credentials(app->ice_agent, ufrag, sizeof(ufrag), pwd, sizeof(pwd));
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

  if (ice_agent_set_remote_credentials(app->ice_agent, media->ice_ufrag, media->ice_pwd) != 0) {
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
      if (ice_agent_add_remote_candidate(app->ice_agent, candidate_line) == 0) {
        app->remote_candidate_count++;
      }
    }
  }

  TLOG_INFO("Applied remote %s SDP", type);
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
    TLOG_INFO("Sent offer to {}", app->remote_peer_id);
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
    TLOG_INFO("Sent answer to {}", app->remote_peer_id);
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
  json_value_t *peer_id = turbo_json_object_get(root, "peerId");
  if (!peer_id || turbo_json_type(peer_id) != TURBO_JSON_STRING) {
    return;
  }
  copy_json_string(peer_id, app->peer_id, sizeof(app->peer_id));
  TLOG_INFO("Joined room '{}' as {}", app->room, app->peer_id);
  if (app->is_offerer && app->remote_peer_id[0] == '\0') {
    request_peer_list(app);
  }
}

static void handle_peers(app_state_t *app, json_value_t *root) {
  size_t i;
  json_value_t *peers = turbo_json_object_get(root, "peers");
  if (!app->is_offerer || app->remote_peer_id[0] != '\0') {
    return;
  }
  if (!peers || turbo_json_type(peers) != TURBO_JSON_ARRAY) {
    return;
  }

  for (i = 0; i < turbo_json_array_size(peers); ++i) {
    json_value_t *value = turbo_json_array_get(peers, i);
    if (!value || turbo_json_type(value) != TURBO_JSON_STRING) {
      continue;
    }
    copy_json_string(value, app->remote_peer_id, sizeof(app->remote_peer_id));
    if (strcmp(app->remote_peer_id, app->peer_id) != 0) {
      TLOG_INFO("Selected remote peer {}", app->remote_peer_id);
      send_offer(app);
      return;
    }
  }
  app->remote_peer_id[0] = '\0';
}

static void handle_peer_joined(app_state_t *app, json_value_t *root) {
  json_value_t *peer_id = turbo_json_object_get(root, "peerId");
  if (!app->is_offerer || app->remote_peer_id[0] != '\0') {
    return;
  }
  if (!peer_id || turbo_json_type(peer_id) != TURBO_JSON_STRING) {
    return;
  }

  copy_json_string(peer_id, app->remote_peer_id, sizeof(app->remote_peer_id));
  if (strcmp(app->remote_peer_id, app->peer_id) == 0) {
    app->remote_peer_id[0] = '\0';
    return;
  }

  TLOG_INFO("Peer joined: {}", app->remote_peer_id);
  send_offer(app);
}

static void handle_offer(app_state_t *app, json_value_t *root) {
  char from[MAX_PEER_ID_LEN];
  char sdp[4096];
  json_value_t *from_value = turbo_json_object_get(root, "from");
  json_value_t *sdp_value = turbo_json_object_get(root, "sdp");

  if (!from_value || !sdp_value) {
    return;
  }
  if (turbo_json_type(from_value) != TURBO_JSON_STRING || turbo_json_type(sdp_value) != TURBO_JSON_STRING) {
    return;
  }

  copy_json_string(from_value, from, sizeof(from));
  copy_json_string(sdp_value, sdp, sizeof(sdp));
  snprintf(app->remote_peer_id, sizeof(app->remote_peer_id), "%s", from);

  if (ensure_media_runtime(app) != 0) {
    return;
  }
  if (apply_remote_sdp(app, "offer", sdp) == 0) {
    if (app->local_gathering_complete &&
        ice_agent_get_gathering_state(app->ice_agent) == ICE_GATHERING_COMPLETE) {
      send_answer(app);
      maybe_start_checks(app);
    } else {
      app->answer_pending = 1;
    }
  }
}

static void handle_answer(app_state_t *app, json_value_t *root) {
  char sdp[4096];
  json_value_t *sdp_value = turbo_json_object_get(root, "sdp");

  if (!sdp_value || turbo_json_type(sdp_value) != TURBO_JSON_STRING) {
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
  json_value_t *candidate_value = turbo_json_object_get(root, "candidate");

  if (!candidate_value || turbo_json_type(candidate_value) != TURBO_JSON_STRING) {
    return;
  }
  if (ensure_media_runtime(app) != 0) {
    return;
  }

  copy_json_string(candidate_value, candidate, sizeof(candidate));
  if (ice_agent_add_remote_candidate(app->ice_agent, candidate) == 0) {
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
  ice_agent_end_of_candidates(app->ice_agent);
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

  if (turbo_parse_json((const uint8_t *)json_text, len, &root) != 0 || !root ||
      turbo_json_type(root) != TURBO_JSON_OBJECT) {
    TLOG_WARN("Ignoring invalid signaling message");
    if (root) {
      turbo_free_json(&root);
    }
    free(json_text);
    return;
  }

  type = turbo_json_object_get(root, "type");
  if (!type || turbo_json_type(type) != TURBO_JSON_STRING) {
    turbo_free_json(&root);
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
    TLOG_ERROR("Signaling server returned an error: {}", json_text);
  }

  turbo_free_json(&root);
  free(json_text);
}

static void on_ice_state_change(turbo_ice_agent_t *agent, ice_state_t old_state,
                                ice_state_t new_state, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)agent;
  TLOG_INFO("ICE state: {} -> {}", ice_state_name(old_state), ice_state_name(new_state));

  if ((new_state == ICE_STATE_CONNECTED || new_state == ICE_STATE_COMPLETED) &&
      !app->dc_connect_started) {
    app->dc_connect_started = 1;
    if (app->signal_socket) {
      coro_socket_set_timeout(app->signal_socket, 50);
      wake_signal_task(app);
    }
    if (!app->timer_task_started && app->ctx &&
        coro_context_spawn(app->ctx, dc_timer_task, app) == 0) {
      app->timer_task_started = 1;
    }
    if (turbo_dc_peer_connect(app->dc_peer) != 0) {
      TLOG_ERROR("Failed to start DataChannel over ICE");
      app_request_stop(app);
    }
  } else if (new_state == ICE_STATE_FAILED) {
    app_request_stop(app);
  }
}

static void on_ice_gathering_change(turbo_ice_agent_t *agent, ice_gathering_state_t state,
                                    void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)agent;
  switch (state) {
  case ICE_GATHERING_NEW:
    TLOG_INFO("ICE gathering: NEW");
    break;
  case ICE_GATHERING_GATHERING:
    TLOG_INFO("ICE gathering: GATHERING");
    break;
  case ICE_GATHERING_COMPLETE:
    TLOG_INFO("ICE gathering: COMPLETE");
    break;
  default:
    TLOG_INFO("ICE gathering: UNKNOWN");
    break;
  }
  if (state == ICE_GATHERING_COMPLETE) {
    app->local_gathering_complete = 1;
    app->local_end_of_candidates_pending = 1;
    repair_local_candidate_ports(app);
    emit_all_local_candidates(app);
    flush_cached_candidates(app);
    flush_end_of_candidates(app);
    if (app->answer_pending) {
      app->answer_pending = 0;
      send_answer(app);
    }
    maybe_start_checks(app);
  }
}

static void on_ice_candidate(turbo_ice_agent_t *agent, const ice_candidate_t *candidate,
                             void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  char candidate_sdp[512];
  (void)agent;

  if (ice_candidate_to_sdp(candidate, candidate_sdp, sizeof(candidate_sdp)) <= 0) {
    return;
  }

  emit_local_candidate_sdp(app, candidate_sdp);
}

static void on_ice_data(turbo_ice_agent_t *agent, const void *data, size_t len, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)agent;
  turbo_dc_peer_feed_ice_data(app->dc_peer, data, len);
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
  TLOG_INFO("Received: {}", message);

  if (!app->is_offerer && strncmp(message, "ECHO:", 5) != 0) {
    char echo[512];
    int echo_len = snprintf(echo, sizeof(echo), "ECHO: %s", message);
    if (echo_len > 0 && (size_t)echo_len < sizeof(echo)) {
      turbo_dc_channel_send(channel, echo, (size_t)echo_len, 0);
      app->completed = 1;
    }
  } else if (app->is_offerer && strncmp(message, "ECHO:", 5) == 0) {
    app->completed = 1;
    app_request_stop(app);
  }
}

static void on_dc_open(turbo_dc_channel_t *channel, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  const char *hello = "hello over signaled ICE";
  TLOG_INFO("Channel '{}' opened", turbo_dc_channel_get_label(channel));

  if (app->is_offerer && !app->message_sent) {
    turbo_dc_channel_send(channel, hello, strlen(hello), 0);
    app->message_sent = 1;
  }
}

static void on_dc_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)peer;
  app->channel = channel;
  turbo_dc_channel_set_user_data(channel, app);
  turbo_dc_channel_on_open(channel, on_dc_open);
  turbo_dc_channel_on_message(channel, on_dc_message);
}

static void on_dc_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                        turbo_dc_state_t new_state, void *user_data) {
  app_state_t *app = (app_state_t *)user_data;
  (void)peer;
  TLOG_INFO("DC state: {} -> {}", ENUM_NAME(old_state), ENUM_NAME(new_state));

  if (new_state == TURBO_DC_STATE_CONNECTED && app->is_offerer && !app->channel) {
    app->channel = turbo_dc_channel_create(app->dc_peer, "chat", NULL);
    if (app->channel) {
      turbo_dc_channel_set_user_data(app->channel, app);
      turbo_dc_channel_on_open(app->channel, on_dc_open);
      turbo_dc_channel_on_message(app->channel, on_dc_message);
      turbo_dc_channel_open(app->channel);
    }
  } else if (new_state == TURBO_DC_STATE_CONNECTED && !app->is_offerer && !app->channel) {
    app->channel = turbo_dc_channel_create(app->dc_peer, "chat", NULL);
    if (app->channel) {
      TLOG_INFO("Answerer creating fallback chat channel");
      turbo_dc_channel_set_user_data(app->channel, app);
      turbo_dc_channel_on_open(app->channel, on_dc_open);
      turbo_dc_channel_on_message(app->channel, on_dc_message);
      turbo_dc_channel_open(app->channel);
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
  TLOG_ERROR("DataChannel error {}: {}", error_code, error_msg);
  app_request_stop(app);
}

static int ensure_media_stack(app_state_t *app) {
  ice_config_t ice_cfg;
  ice_callbacks_t callbacks;
  turbo_dc_config_t dc_cfg;

  if (app->media_ready) {
    return 0;
  }

  TLOG_INFO("ensure_media_stack: init ICE config");
  ice_cfg = ice_default_config();
  ice_cfg.is_controlling = app->is_offerer ? 1 : 0;
  ice_cfg.aggressive_nomination = 1;
  ice_cfg.use_mdns_candidates = 0;
  if (signal_host_is_loopback(app->signal_host)) {
    ice_cfg.allow_loopback = 1;
    ice_cfg.stun_server_count = 0;
    ice_cfg.turn_server_count = 0;
  } else {
    /*
     * Use a literal OpenRelay address for this smoke example. The current
     * Windows ASan build reports in CoroNet's async DNS path during TURN
     * hostname resolution, which masks the RTC path we want to test here.
     */
    snprintf(ice_cfg.turn_servers[0].url, sizeof(ice_cfg.turn_servers[0].url),
             "turn:161.97.65.129:3478");
    snprintf(ice_cfg.turn_servers[0].username, sizeof(ice_cfg.turn_servers[0].username),
             "turbonet");
    snprintf(ice_cfg.turn_servers[0].credential, sizeof(ice_cfg.turn_servers[0].credential),
             "turbonet-secret");
    snprintf(ice_cfg.stun_servers[0].url, sizeof(ice_cfg.stun_servers[0].url),
             "stun:162.159.207.0:3478");
    ice_cfg.stun_server_count = 1;
    ice_cfg.turn_server_count = 1;
  }

  TLOG_INFO("ensure_media_stack: creating ICE agent");
  app->ice_agent = ice_agent_create(app->ctx, &ice_cfg);
  if (!app->ice_agent) {
    TLOG_ERROR("Failed to create ICE agent");
    return -1;
  }
  if (signal_host_is_loopback(app->signal_host)) {
    ice_agent_set_allow_loopback(app->ice_agent, 1);
  }

  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.on_state_change = on_ice_state_change;
  callbacks.on_gathering_change = on_ice_gathering_change;
  callbacks.on_data = on_ice_data;
  callbacks.user_data = app;
  ice_agent_set_callbacks(app->ice_agent, &callbacks);

  TLOG_INFO("ensure_media_stack: creating DataChannel context");
  memset(&dc_cfg, 0, sizeof(dc_cfg));
  dc_cfg.is_server = app->is_offerer ? 0 : 1;
  dc_cfg.transport = TURBO_DC_TRANSPORT_ICE;
  dc_cfg.dtls_mtu = 1200;
  dc_cfg.sctp_mtu = 1000;

  app->dc_ctx = turbo_dc_context_create(&dc_cfg);
  if (!app->dc_ctx) {
    TLOG_ERROR("Failed to create DataChannel context");
    ice_agent_destroy(app->ice_agent);
    app->ice_agent = NULL;
    return -1;
  }

  TLOG_INFO("ensure_media_stack: creating DataChannel peer");
  app->dc_peer = turbo_dc_peer_create(app->dc_ctx, NULL, 0, app);
  if (!app->dc_peer) {
    TLOG_ERROR("Failed to create DataChannel peer");
    turbo_dc_context_destroy(app->dc_ctx);
    app->dc_ctx = NULL;
    ice_agent_destroy(app->ice_agent);
    app->ice_agent = NULL;
    return -1;
  }

  turbo_dc_peer_on_state(app->dc_peer, on_dc_state);
  turbo_dc_peer_on_channel(app->dc_peer, on_dc_channel);
  turbo_dc_peer_on_error(app->dc_peer, on_dc_error);
  if (turbo_dc_peer_set_ice_agent(app->dc_peer, app->ice_agent) != 0) {
    TLOG_ERROR("Failed to attach TurboNet ICE transport to DataChannel peer");
    turbo_dc_peer_destroy(app->dc_peer);
    app->dc_peer = NULL;
    turbo_dc_context_destroy(app->dc_ctx);
    app->dc_ctx = NULL;
    ice_agent_destroy(app->ice_agent);
    app->ice_agent = NULL;
    return -1;
  }
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
    TLOG_INFO("ensure_media_runtime: gather candidates inline");
    rc = ice_agent_gather_candidates(app->ice_agent);
    if (rc != 0) {
      TLOG_ERROR("Failed to gather ICE candidates: {}", rc);
      app_request_stop(app);
      return -1;
    }
    app->gather_task_started = 1;
    app->gather_task_done = 1;
  }

  TLOG_INFO("ensure_media_runtime: ready");
  return 0;
}

static coro_socket_type_t select_signal_socket_type(const app_state_t *app) {
  if (app->use_tls) {
    return CORO_SOCKET_TLS;
  }
  if (app->signal_host[0] != '\0' && strchr(app->signal_host, ':')) {
    return CORO_SOCKET_TCP_V6;
  }
  return CORO_SOCKET_TCP_V4;
}

static void signal_task(coro_t *co, void *arg) {
  app_state_t *app = (app_state_t *)arg;
  char *data = NULL;
  size_t len = 0;
  int is_text = 0;
  int rc;
  (void)co;

  app->signal_socket = coro_socket_create(app->ctx, select_signal_socket_type(app));
  if (!app->signal_socket) {
    TLOG_ERROR("Failed to create signaling socket");
    app_request_stop(app);
    goto done;
  }

  coro_socket_set_timeout(app->signal_socket, 5000);
  rc = coro_socket_connect_ws_ex(app->signal_socket, app->signal_host, app->signal_port,
                                 app->signal_path, app->use_tls, "webrtc-signaling");
  if (rc != 0) {
    TLOG_ERROR("Failed to connect to signaling WebSocket: {} ({})", rc,
               turbo_strerror(rc));
    coro_socket_destroy(app->signal_socket);
    app->signal_socket = NULL;
    app_request_stop(app);
    goto done;
  }

  app->ws_connected = 1;
  coro_socket_set_timeout(app->signal_socket, 500);
  TLOG_INFO("Connected to signaling server at {}:{}{}", app->signal_host, app->signal_port,
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
    if (app->signal_socket) {
      coro_socket_destroy(app->signal_socket);
      app->signal_socket = NULL;
    }
    app_request_stop(app);
    goto done;
  }

  while (app->running) {
    if (app->is_offerer && app->remote_peer_id[0] == '\0' && app->ws_connected) {
      uint64_t now_ms = turbo_monotonic_ms();
      if (app->last_peer_query_ms == 0 || now_ms - app->last_peer_query_ms >= 1000) {
        request_peer_list(app);
      }
    }

    if (flush_signal_outbox(app) != 0) {
      TLOG_ERROR("Failed to send signaling message");
      break;
    }

    data = NULL;
    len = 0;
    is_text = 0;
    rc = coro_socket_recv_ws(app->signal_socket, &data, &len, &is_text);
    if (!app->running) {
      if (data) {
        coro_socket_free_recv(data);
      }
      break;
    }

    if (rc == TURBO_ETIMEDOUT) {
      if (data) {
        coro_socket_free_recv(data);
      }
      continue;
    }
    if (rc != 0) {
      TLOG_ERROR("Signaling socket receive failed: {} ({})", rc, turbo_strerror(rc));
      if (data) {
        coro_socket_free_recv(data);
      }
      break;
    }

    if (!data || len == 0) {
      if (data) {
        coro_socket_free_recv(data);
      }
      continue;
    }

    if (!is_text) {
      TLOG_ERROR("Signaling server sent a non-text WebSocket message");
      coro_socket_free_recv(data);
      break;
    }

    process_signaling_message(app, data, len);
    coro_socket_free_recv(data);
  }

  app->ws_connected = 0;
  if (app->signal_socket) {
    coro_socket_destroy(app->signal_socket);
    app->signal_socket = NULL;
  }

  app_request_stop(app);

done:
  app->signal_task_done = 1;
  maybe_stop_context(app);
}

static void ice_gather_task(coro_t *co, void *arg) {
  app_state_t *app = (app_state_t *)arg;
  int rc;
  (void)co;

  rc = ice_agent_gather_candidates(app->ice_agent);
  if (rc != 0) {
    TLOG_ERROR("Failed to gather ICE candidates: {}", rc);
    app_request_stop(app);
  }
  app->gather_task_done = 1;
  maybe_stop_context(app);
}

static void ice_checks_task(coro_t *co, void *arg) {
  app_state_t *app = (app_state_t *)arg;
  int rc;
  (void)co;

  tracef_app(app, "ice_checks_task enter");
  while (app->running) {
    if (ice_agent_get_gathering_state(app->ice_agent) != ICE_GATHERING_COMPLETE) {
      coro_sleep(app->ctx, 10);
      continue;
    }

    rc = ice_agent_start_checks(app->ice_agent);
    tracef_app(app, "ice_checks_task ice_agent_start_checks rc=%d state=%d", rc,
               (int)ice_agent_get_state(app->ice_agent));
    if (rc == -2) {
      coro_sleep(app->ctx, 10);
      continue;
    }
    if (rc != 0 && app->running) {
      TLOG_ERROR("Failed to start ICE checks: {}", rc);
      app_request_stop(app);
    }
    break;
  }
  app->checks_task_done = 1;
  maybe_stop_context(app);
}

static void dc_timer_task(coro_t *co, void *arg) {
  app_state_t *app = (app_state_t *)arg;
  (void)co;

  while (app->running) {
    turbo_dc_handle_timers();
    turbo_dc_peer_poll(app->dc_peer);
    coro_sleep(app->ctx, 10);
  }
  app->timer_task_done = 1;
  maybe_stop_context(app);
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
  int role_specified = 0;
  app_state_t app;

  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  memset(&app, 0, sizeof(app));
  app.running = 1;
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
  TLOG_INFO("Mode: {}", app.is_offerer ? "offerer" : "answerer");
  TLOG_INFO("Room: {}", app.room);

  app.ctx = coro_context_create(NULL);
  if (!app.ctx) {
    TLOG_ERROR("Failed to create coroutine context");
    return 1;
  }
  coro_context_set_persistent(app.ctx, 1);

  if (coro_context_spawn(app.ctx, signal_task, &app) != 0) {
    TLOG_ERROR("Failed to start example tasks");
    app_request_stop(&app);
    maybe_stop_context(&app);
  } else {
    app.signal_task_started = 1;
  }

  coro_context_run(app.ctx, TURBO_RUN_DEFAULT);

  if (app.signal_socket) {
    coro_socket_destroy(app.signal_socket);
    app.signal_socket = NULL;
  }
  free_signal_outbox(&app);
  if (app.dc_peer) {
    turbo_dc_peer_destroy(app.dc_peer);
  }
  if (app.dc_ctx) {
    turbo_dc_context_destroy(app.dc_ctx);
  }
  if (app.ice_agent) {
    ice_agent_destroy(app.ice_agent);
  }
  if (app.ctx) {
    coro_context_destroy(app.ctx);
  }

  return app.completed ? 0 : 1;
}
