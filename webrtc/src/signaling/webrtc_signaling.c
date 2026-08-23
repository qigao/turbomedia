/**
 * @file webrtc_signaling.c
 * @brief WebRTC signaling server implemented on CoroNet WebSockets.
 */

#include "webrtc_signaling.h"

#include "platform.h"
#include "tlog.h"
#include "turbo_media_auth.h"
#include <turbostl/hash_map.h>
#include "turbo_parser.h"
#include "turbo_str.h"

#include <CoroNet/turbo_coro_context.h>
#include <CoroNet/turbo_coro_socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct peer_message_s peer_message_t;
typedef struct webrtc_room_s webrtc_room_t;
typedef struct signaling_source_state_s signaling_source_state_t;

enum {
  SIGNALING_WS_HANDSHAKE_TIMEOUT_MS = 5000,
  SIGNALING_PEER_IO_TIMEOUT_MS = 1000,
  SIGNALING_CLEANUP_INTERVAL_MS = 1000,
  SIGNALING_RATE_TOKEN_UNITS = 1000,
  SIGNALING_SOURCE_FAMILY_IPV4 = 4,
  SIGNALING_SOURCE_FAMILY_IPV6 = 6
};

typedef enum {
  SIGNALING_SOURCE_ADMITTED = 0,
  SIGNALING_SOURCE_REJECT_ADDRESS = -1,
  SIGNALING_SOURCE_REJECT_CAPACITY = -2,
  SIGNALING_SOURCE_REJECT_RATE = -3,
  SIGNALING_SOURCE_REJECT_CONCURRENCY = -4,
  SIGNALING_SOURCE_REJECT_MEMORY = -5
} signaling_source_admission_result_t;

#define SIGNALING_PEER_AUTH_AUDIENCE "turbomedia-signaling-peer"
#define SIGNALING_PEER_JOIN_SCOPE "signaling.peer.join"

typedef struct {
  uint8_t family;
  uint8_t address[16];
  uint32_t scope_id;
} signaling_source_key_t;

struct signaling_source_state_s {
  signaling_source_key_t key;
  uint64_t rate_last_refill_ms;
  uint64_t rate_tokens;
  uint64_t last_seen_ms;
  size_t active_connections;
  signaling_source_state_t *inactive_next;
  signaling_source_state_t *inactive_prev;
  int inactive_linked;
};

struct peer_message_s {
  char *json;
  size_t bytes;
  peer_message_t *next;
};

struct webrtc_peer_s {
  webrtc_signaling_server_t *server;
  tstr id;
  tstr room;
  webrtc_room_t *room_ptr;
  coro_socket_t *socket;
  uint64_t connected_at;
  uint64_t last_activity;
  uint64_t rate_last_refill_ms;
  uint64_t rate_tokens;
  webrtc_peer_t *next;
  webrtc_peer_t *prev;
  webrtc_peer_t *next_in_room;
  webrtc_peer_t *prev_in_room;
  peer_message_t *outbox_head;
  peer_message_t *outbox_tail;
  size_t outbox_message_count;
  size_t outbox_bytes;
  signaling_source_key_t source_key;
  int closing;
  int identity_bound;
  int source_admitted;
};

struct webrtc_room_s {
  tstr id;
  webrtc_peer_t *peers_head;
  webrtc_peer_t *peers_tail;
  int peer_count;
  webrtc_room_t *next;
  webrtc_room_t *prev;
};

static size_t webrtc_str_hash(const void *key, size_t key_size, void *ctx) {
  const char *str = *(const char *const *)key;
  (void)key_size;
  return hash_bytes(str, strlen(str), ctx);
}

static bool webrtc_str_equal(const void *left, const void *right, size_t key_size, void *ctx) {
  const char *left_str = *(const char *const *)left;
  const char *right_str = *(const char *const *)right;
  (void)key_size;
  (void)ctx;
  return strcmp(left_str, right_str) == 0;
}

static int source_key_from_sockaddr(const struct sockaddr_storage *address,
                                    signaling_source_key_t *key) {
  const uint8_t *ipv6_bytes;
  size_t index;
  int mapped_ipv4 = 1;

  if (!address || !key) {
    return -1;
  }
  memset(key, 0, sizeof(*key));
  if (address->ss_family == AF_INET) {
    const struct sockaddr_in *ipv4 =
        (const struct sockaddr_in *)address;
    key->family = SIGNALING_SOURCE_FAMILY_IPV4;
    memcpy(key->address, &ipv4->sin_addr, 4U);
    return 0;
  }
  if (address->ss_family != AF_INET6) {
    return -1;
  }

  ipv6_bytes = (const uint8_t *)&((const struct sockaddr_in6 *)address)->sin6_addr;
  for (index = 0U; index < 10U; ++index) {
    if (ipv6_bytes[index] != 0U) {
      mapped_ipv4 = 0;
      break;
    }
  }
  if (mapped_ipv4 && ipv6_bytes[10] == 0xffU && ipv6_bytes[11] == 0xffU) {
    key->family = SIGNALING_SOURCE_FAMILY_IPV4;
    memcpy(key->address, ipv6_bytes + 12U, 4U);
    return 0;
  }

  key->family = SIGNALING_SOURCE_FAMILY_IPV6;
  memcpy(key->address, ipv6_bytes, sizeof(key->address));
  key->scope_id = ((const struct sockaddr_in6 *)address)->sin6_scope_id;
  return 0;
}

static int source_key_from_socket(coro_socket_t *socket,
                                  signaling_source_key_t *key) {
  struct sockaddr_storage address;

  if (!socket || !key || coro_socket_get_peer_address(socket, &address) != 0) {
    return -1;
  }
  return source_key_from_sockaddr(&address, key);
}

typedef struct {
  webrtc_signaling_server_t *server;
  tstr room;
  tstr from;
  tstr message;
} signal_broadcast_op_t;

typedef struct {
  webrtc_signaling_server_t *server;
  tstr room_id;
  tstr peer_id;
  tstr reason;
} signal_kick_op_t;

static void free_broadcast_op(signal_broadcast_op_t *op) {
  if (!op) {
    return;
  }
  tstr_free(op->room);
  tstr_free(op->from);
  tstr_free(op->message);
  free(op);
}

static void free_kick_op(signal_kick_op_t *op) {
  if (!op) {
    return;
  }
  tstr_free(op->room_id);
  tstr_free(op->peer_id);
  tstr_free(op->reason);
  free(op);
}

struct webrtc_signaling_server_s {
  turbo_loop_t *loop;
  int owns_loop;
  coro_context_t *ctx;
  coro_socket_t *listener;
  webrtc_signaling_config_t config;

  hash_map_t local_peers;
  hash_map_t local_rooms;
  hash_map_t source_states;

  signaling_source_state_t *inactive_sources_head;
  signaling_source_state_t *inactive_sources_tail;

  webrtc_peer_t *peers_head;
  webrtc_peer_t *peers_tail;
  int peer_count;

  webrtc_room_t *rooms_head;
  webrtc_room_t *rooms_tail;
  int room_count;
  uint64_t next_peer_sequence;
  uint64_t authentication_rejections;
  uint64_t join_timeout_rejections;
  uint64_t message_rate_rejections;
  uint64_t outbox_overflow_rejections;
  uint64_t source_address_rejections;
  uint64_t source_capacity_rejections;
  uint64_t source_rate_rejections;
  uint64_t source_concurrency_rejections;

  int running;
  int stop_posted;
  turbo_timer_t *cleanup_timer;
  turbo_mutex_t mutex;
};

static int source_policy_enabled(const webrtc_signaling_config_t *config) {
  return config &&
         (config->max_connections_per_source > 0 ||
          config->source_admissions_per_second > 0);
}

static int consume_token_bucket(int refill_rate, int burst,
                                uint64_t *last_refill_ms, uint64_t *tokens,
                                uint64_t now_ms) {
  uint64_t capacity;
  uint64_t elapsed;
  uint64_t missing;
  uint64_t refill_to_capacity_ms;

  if (refill_rate <= 0) {
    return 1;
  }
  if (burst <= 0 || !last_refill_ms || !tokens) {
    return 0;
  }
  capacity = (uint64_t)burst * SIGNALING_RATE_TOKEN_UNITS;
  if (*last_refill_ms == 0U) {
    *last_refill_ms = now_ms == 0U ? 1U : now_ms;
    *tokens = capacity;
  } else if (now_ms > *last_refill_ms) {
    elapsed = now_ms - *last_refill_ms;
    if (*tokens >= capacity) {
      *tokens = capacity;
    }
    missing = capacity - *tokens;
    refill_to_capacity_ms =
        missing == 0U ? 0U : (missing + (uint64_t)refill_rate - 1U) /
                                    (uint64_t)refill_rate;
    if (elapsed >= refill_to_capacity_ms) {
      *tokens = capacity;
    } else {
      *tokens += elapsed * (uint64_t)refill_rate;
    }
    *last_refill_ms = now_ms;
  }
  if (*tokens < SIGNALING_RATE_TOKEN_UNITS) {
    return 0;
  }
  *tokens -= SIGNALING_RATE_TOKEN_UNITS;
  return 1;
}

static void unlink_inactive_source_locked(
    webrtc_signaling_server_t *server, signaling_source_state_t *source) {
  if (!server || !source || !source->inactive_linked) {
    return;
  }
  if (source->inactive_prev) {
    source->inactive_prev->inactive_next = source->inactive_next;
  } else {
    server->inactive_sources_head = source->inactive_next;
  }
  if (source->inactive_next) {
    source->inactive_next->inactive_prev = source->inactive_prev;
  } else {
    server->inactive_sources_tail = source->inactive_prev;
  }
  source->inactive_prev = NULL;
  source->inactive_next = NULL;
  source->inactive_linked = 0;
}

static void link_inactive_source_locked(webrtc_signaling_server_t *server,
                                        signaling_source_state_t *source,
                                        uint64_t now_ms) {
  if (!server || !source || source->active_connections != 0U) {
    return;
  }
  unlink_inactive_source_locked(server, source);
  source->last_seen_ms = now_ms;
  source->inactive_prev = server->inactive_sources_tail;
  if (server->inactive_sources_tail) {
    server->inactive_sources_tail->inactive_next = source;
  } else {
    server->inactive_sources_head = source;
  }
  server->inactive_sources_tail = source;
  source->inactive_linked = 1;
}

static void expire_source_states_locked(webrtc_signaling_server_t *server,
                                        uint64_t now_ms) {
  signaling_source_state_t *source;

  if (!server || server->config.source_state_ttl_ms <= 0) {
    return;
  }
  while ((source = server->inactive_sources_head) != NULL) {
    if (now_ms < source->last_seen_ms ||
        now_ms - source->last_seen_ms <
            (uint64_t)server->config.source_state_ttl_ms) {
      break;
    }
    unlink_inactive_source_locked(server, source);
    hash_map_remove(&server->source_states, &source->key, NULL);
    free(source);
  }
}

static signaling_source_admission_result_t admit_source_locked(
    webrtc_signaling_server_t *server, const signaling_source_key_t *key,
    uint64_t now_ms) {
  signaling_source_state_t **entry;
  signaling_source_state_t *source;

  if (!server || !key) {
    return SIGNALING_SOURCE_REJECT_ADDRESS;
  }
  if (!source_policy_enabled(&server->config)) {
    return SIGNALING_SOURCE_ADMITTED;
  }

  entry = (signaling_source_state_t **)hash_map_get(
      &server->source_states, key);
  source = entry ? *entry : NULL;
  if (!source) {
    expire_source_states_locked(server, now_ms);
    if (hash_map_size(&server->source_states) >=
        server->config.max_source_states) {
      return SIGNALING_SOURCE_REJECT_CAPACITY;
    }
    source = (signaling_source_state_t *)calloc(1, sizeof(*source));
    if (!source) {
      return SIGNALING_SOURCE_REJECT_MEMORY;
    }
    source->key = *key;
    if (hash_map_put(&server->source_states, &source->key, &source) !=
        STL_OK) {
      free(source);
      return SIGNALING_SOURCE_REJECT_MEMORY;
    }
  } else {
    unlink_inactive_source_locked(server, source);
  }

  source->last_seen_ms = now_ms;
  if (!consume_token_bucket(server->config.source_admissions_per_second,
                            server->config.source_admission_burst,
                            &source->rate_last_refill_ms,
                            &source->rate_tokens, now_ms)) {
    if (source->active_connections == 0U) {
      link_inactive_source_locked(server, source, now_ms);
    }
    return SIGNALING_SOURCE_REJECT_RATE;
  }
  if (server->config.max_connections_per_source > 0 &&
      source->active_connections >=
          (size_t)server->config.max_connections_per_source) {
    return SIGNALING_SOURCE_REJECT_CONCURRENCY;
  }

  source->active_connections++;
  return SIGNALING_SOURCE_ADMITTED;
}

static void release_source_key_locked(webrtc_signaling_server_t *server,
                                      const signaling_source_key_t *key,
                                      uint64_t now_ms) {
  signaling_source_state_t **entry;
  signaling_source_state_t *source;

  if (!server || !key) {
    return;
  }
  entry = (signaling_source_state_t **)hash_map_get(
      &server->source_states, key);
  source = entry ? *entry : NULL;
  if (source && source->active_connections > 0U) {
    source->active_connections--;
    source->last_seen_ms = now_ms;
    if (source->active_connections == 0U) {
      link_inactive_source_locked(server, source, now_ms);
    }
  }
}

static void release_source_locked(webrtc_signaling_server_t *server,
                                  webrtc_peer_t *peer, uint64_t now_ms) {
  if (!server || !peer || !peer->source_admitted) {
    return;
  }
  release_source_key_locked(server, &peer->source_key, now_ms);
  peer->source_admitted = 0;
}

static void destroy_source_states(webrtc_signaling_server_t *server) {
  size_t slot;

  if (!server) {
    return;
  }
  for (slot = 0U; slot < hash_map_capacity(&server->source_states);
       ++slot) {
    signaling_source_state_t *const *source =
        (signaling_source_state_t *const *)hash_map_value_at_const(
            &server->source_states, slot);
    if (source) {
      free(*source);
    }
  }
  hash_map_destroy(&server->source_states);
  server->inactive_sources_head = NULL;
  server->inactive_sources_tail = NULL;
}

static void record_source_rejection_locked(
    webrtc_signaling_server_t *server,
    signaling_source_admission_result_t result) {
  if (!server) {
    return;
  }
  switch (result) {
    case SIGNALING_SOURCE_REJECT_ADDRESS:
      server->source_address_rejections++;
      break;
    case SIGNALING_SOURCE_REJECT_RATE:
      server->source_rate_rejections++;
      break;
    case SIGNALING_SOURCE_REJECT_CONCURRENCY:
      server->source_concurrency_rejections++;
      break;
    case SIGNALING_SOURCE_REJECT_CAPACITY:
    case SIGNALING_SOURCE_REJECT_MEMORY:
      server->source_capacity_rejections++;
      break;
    default:
      break;
  }
}

static void signaling_stop_listener_post_cb(void *arg1, void *arg2) {
  webrtc_signaling_server_t *server = (webrtc_signaling_server_t *)arg1;
  coro_socket_t *listener = (coro_socket_t *)arg2;

  if (listener) {
    (void)coro_socket_server_stop(listener);
  }

  turbo_mutex_lock(&server->mutex);
  server->stop_posted = 0;
  turbo_mutex_unlock(&server->mutex);
}

static void signaling_drain_listener(webrtc_signaling_server_t *server) {
  coro_socket_t *listener;
  int stop_posted;

  if (!server || !server->listener) {
    return;
  }

  listener = server->listener;
  turbo_mutex_lock(&server->mutex);
  stop_posted = server->stop_posted;
  turbo_mutex_unlock(&server->mutex);

  if (!stop_posted) {
    (void)coro_socket_server_stop(listener);
  }

  for (;;) {
    turbo_mutex_lock(&server->mutex);
    stop_posted = server->stop_posted;
    turbo_mutex_unlock(&server->mutex);
    if (!stop_posted && coro_socket_server_is_stopped(listener)) {
      break;
    }
    coro_context_run(server->ctx, TURBO_RUN_ONCE);
  }

  coro_socket_destroy(listener);
  server->listener = NULL;
}

static int signaling_send_text(coro_socket_t *socket, const char *json) {
  if (!socket || !json || json[0] == '\0') {
    return TURBO_EINVAL;
  }
  return coro_socket_send_ws_text(socket, json, strlen(json));
}

static tstr escape_json_string(const char *str) {
  tstr s = tstr_new();
  const char *src = str;
  while (*src) {
    switch (*src) {
      case '\n':
        s = tstr_cat(s, "\\n");
        break;
      case '\r':
        s = tstr_cat(s, "\\r");
        break;
      case '\t':
        s = tstr_cat(s, "\\t");
        break;
      case '\b':
        s = tstr_cat(s, "\\b");
        break;
      case '\f':
        s = tstr_cat(s, "\\f");
        break;
      case '"':
        s = tstr_cat(s, "\\\"");
        break;
      case '\\':
        s = tstr_cat(s, "\\\\");
        break;
      default:
        if ((unsigned char)*src < 0x20u) {
          s = tstr_cat_fmt(s, "\\u%04x", (unsigned int)(unsigned char)*src);
        } else {
          s = tstr_cat_len(s, src, 1);
        }
        break;
    }
    src++;
  }
  return s;
}

static int json_string_needs_escape(const char *str) {
  const char *src = str;

  if (!src) {
    return 0;
  }

  while (*src) {
    switch (*src) {
      case '\n':
      case '\r':
      case '\t':
      case '\b':
      case '\f':
      case '"':
      case '\\':
        return 1;
      default:
        if ((unsigned char)*src < 0x20u) {
          return 1;
        }
        break;
    }
    src++;
  }

  return 0;
}

static const char *json_string_maybe_escape(const char *str, tstr *owned) {
  if (owned) {
    *owned = NULL;
  }

  if (!str) {
    return NULL;
  }

  if (!json_string_needs_escape(str)) {
    return str;
  }

  if (!owned) {
    return NULL;
  }

  *owned = escape_json_string(str);
  return *owned;
}

static tstr generate_peer_id_locked(webrtc_signaling_server_t *server) {
  uint64_t timestamp = turbo_monotonic_ms();
  tstr id = tstr_new();
  uint64_t sequence = 0;

  if (!server || !id) {
    return id;
  }

  sequence = ++server->next_peer_sequence;
  id = tstr_cat_fmt(id, "peer_%llu_%llu", (unsigned long long)timestamp,
                    (unsigned long long)sequence);
  return id;
}

static webrtc_peer_t *find_peer_by_id_locked(webrtc_signaling_server_t *server, const char *id) {
  webrtc_peer_t *const *peer =
      (webrtc_peer_t *const *)hash_map_get_const(&server->local_peers, &id);
  return peer ? *peer : NULL;
}

static webrtc_peer_t *find_routable_peer_locked(
    webrtc_signaling_server_t *server, const webrtc_peer_t *from_peer,
    const char *to_peer_id) {
  webrtc_peer_t *to_peer = NULL;

  if (!server || !from_peer || !from_peer->room_ptr || !to_peer_id) {
    return NULL;
  }

  to_peer = find_peer_by_id_locked(server, to_peer_id);
  if (!to_peer || !to_peer->room_ptr ||
      to_peer->room_ptr != from_peer->room_ptr) {
    return NULL;
  }
  return to_peer;
}

static webrtc_room_t *find_room_locked(webrtc_signaling_server_t *server, const char *id) {
  webrtc_room_t *const *room =
      (webrtc_room_t *const *)hash_map_get_const(&server->local_rooms, &id);
  return room ? *room : NULL;
}

static webrtc_room_t *create_room_locked(webrtc_signaling_server_t *server, const char *id) {
  webrtc_room_t *room;

  if (server->config.max_rooms > 0 && server->room_count >= server->config.max_rooms) {
    return NULL;
  }

  room = (webrtc_room_t *)calloc(1, sizeof(*room));
  if (!room) {
    return NULL;
  }

  room->id = tstr_dup(id);
  if (!room->id) {
    free(room);
    return NULL;
  }
  if (hash_map_put(&server->local_rooms, &room->id, &room) != STL_OK) {
    tstr_free(room->id);
    free(room);
    return NULL;
  }
  if (server->rooms_tail) {
    server->rooms_tail->next = room;
    room->prev = server->rooms_tail;
  }
  server->rooms_tail = room;
  if (!server->rooms_head) {
    server->rooms_head = room;
  }

  server->room_count++;
  return room;
}

static void destroy_room_locked(webrtc_signaling_server_t *server, webrtc_room_t *room) {
  hash_map_remove(&server->local_rooms, &room->id, NULL);
  if (room->prev) {
    room->prev->next = room->next;
  }
  if (room->next) {
    room->next->prev = room->prev;
  }
  if (server->rooms_head == room) {
    server->rooms_head = room->next;
  }
  if (server->rooms_tail == room) {
    server->rooms_tail = room->prev;
  }
  server->room_count--;
  tstr_free(room->id);
  free(room);
}

static void free_outbox_locked(webrtc_peer_t *peer) {
  peer_message_t *msg = peer->outbox_head;
  while (msg) {
    peer_message_t *next = msg->next;
    free(msg->json);
    free(msg);
    msg = next;
  }
  peer->outbox_head = NULL;
  peer->outbox_tail = NULL;
  peer->outbox_message_count = 0U;
  peer->outbox_bytes = 0U;
}

static int peer_outbox_limit_exceeded(const webrtc_peer_t *peer,
                                      size_t message_bytes) {
  const webrtc_signaling_config_t *config = NULL;

  if (!peer || !peer->server) {
    return 0;
  }
  config = &peer->server->config;
  if (config->max_outbox_messages > 0U &&
      peer->outbox_message_count >= config->max_outbox_messages) {
    return 1;
  }
  return config->max_outbox_bytes > 0U &&
         (message_bytes > config->max_outbox_bytes ||
          peer->outbox_bytes > config->max_outbox_bytes - message_bytes);
}

static void wake_peer_locked(webrtc_peer_t *peer) {
  if (peer->socket) {
    coro_socket_interrupt_wait(peer->socket, 0);
  }
}

static int enqueue_message_locked(webrtc_peer_t *peer, const char *json) {
  peer_message_t *msg;
  size_t len;

  if (!peer || !json || peer->closing) {
    return -1;
  }

  len = strlen(json);
  if (peer_outbox_limit_exceeded(peer, len)) {
    peer->server->outbox_overflow_rejections++;
    peer->closing = 1;
    wake_peer_locked(peer);
    return -1;
  }
  msg = (peer_message_t *)calloc(1, sizeof(*msg));
  if (!msg) {
    return -1;
  }

  msg->json = (char *)malloc(len + 1);
  if (!msg->json) {
    free(msg);
    return -1;
  }

  memcpy(msg->json, json, len + 1);
  msg->bytes = len;

  if (peer->outbox_tail) {
    peer->outbox_tail->next = msg;
  } else {
    peer->outbox_head = msg;
  }
  peer->outbox_tail = msg;
  peer->outbox_message_count++;
  peer->outbox_bytes += len;
  wake_peer_locked(peer);
  return 0;
}

static int send_json_message_locked(webrtc_peer_t *peer, const char *json) {
  return enqueue_message_locked(peer, json);
}

static tstr create_error_message(const char *error) {
  tstr msg = tstr_new();
  msg = tstr_cat_fmt(msg, "{\"type\":\"error\",\"error\":\"%s\"}", error);
  return msg;
}

static tstr create_peer_list_message_locked(webrtc_room_t *room) {
  tstr msg = tstr_new();
  int added = 0;
  webrtc_peer_t *peer = NULL;

  msg = tstr_cat(msg, "{\"type\":\"peers\",\"peers\":[");
  for (peer = room->peers_head; peer; peer = peer->next_in_room) {
    tstr escaped_id = NULL;
    const char *id_json = json_string_maybe_escape(peer->id, &escaped_id);
    if (!id_json) {
      tstr_free(msg);
      return NULL;
    }
    if (added > 0) {
      msg = tstr_cat(msg, ",");
    }
    msg = tstr_cat(msg, "\"");
    msg = tstr_cat(msg, id_json);
    msg = tstr_cat(msg, "\"");
    tstr_free(escaped_id);
    added++;
  }
  msg = tstr_cat(msg, "]}");
  return msg;
}

static void send_peer_list_locked(webrtc_peer_t *peer) {
  tstr peer_list = NULL;

  if (!peer || !peer->room_ptr) {
    return;
  }

  peer_list = create_peer_list_message_locked(peer->room_ptr);
  if (!peer_list) {
    return;
  }

  send_json_message_locked(peer, peer_list);
  tstr_free(peer_list);
}

static int broadcast_locked(webrtc_signaling_server_t *server, const char *room, const char *from,
                            const char *message) {
  int sent = 0;
  webrtc_room_t *room_ptr = find_room_locked(server, room);
  webrtc_peer_t *peer = NULL;

  if (!room_ptr) {
    return 0;
  }

  for (peer = room_ptr->peers_head; peer; peer = peer->next_in_room) {
    if (from && strcmp(peer->id, from) == 0) {
      continue;
    }
    if (enqueue_message_locked(peer, message) == 0) {
      sent++;
    }
  }
  return sent;
}

static void remove_peer_from_room_locked(webrtc_signaling_server_t *server, webrtc_peer_t *peer) {
  webrtc_room_t *room = NULL;

  if (!peer->room_ptr) {
    return;
  }

  room = peer->room_ptr;
  if (peer->prev_in_room) {
    peer->prev_in_room->next_in_room = peer->next_in_room;
  }
  if (peer->next_in_room) {
    peer->next_in_room->prev_in_room = peer->prev_in_room;
  }
  if (room->peers_head == peer) {
    room->peers_head = peer->next_in_room;
  }
  if (room->peers_tail == peer) {
    room->peers_tail = peer->prev_in_room;
  }
  room->peer_count--;
  peer->room_ptr = NULL;
  peer->prev_in_room = NULL;
  peer->next_in_room = NULL;
  if (peer->room) {
    tstr_free(peer->room);
    peer->room = NULL;
  }

  if (room->peer_count == 0) {
    destroy_room_locked(server, room);
  }
}

static void remove_peer_locked(webrtc_signaling_server_t *server, webrtc_peer_t *peer) {
  if (peer->id) {
    hash_map_remove(&server->local_peers, &peer->id, NULL);
  }

  remove_peer_from_room_locked(server, peer);

  if (peer->prev) {
    peer->prev->next = peer->next;
  }
  if (peer->next) {
    peer->next->prev = peer->prev;
  }
  if (server->peers_head == peer) {
    server->peers_head = peer->next;
  }
  if (server->peers_tail == peer) {
    server->peers_tail = peer->prev;
  }
  server->peer_count--;
  release_source_locked(server, peer, turbo_monotonic_ms());

  free_outbox_locked(peer);
  if (peer->id) {
    tstr_free(peer->id);
  }
  free(peer);
}

static int flush_peer_outbox(webrtc_peer_t *peer) {
  peer_message_t *msg = NULL;

  for (;;) {
    turbo_mutex_lock(&peer->server->mutex);
    msg = peer->outbox_head;
    if (msg) {
      peer->outbox_head = msg->next;
      if (!peer->outbox_head) {
        peer->outbox_tail = NULL;
      }
      peer->outbox_message_count--;
      peer->outbox_bytes -= msg->bytes;
    }
    turbo_mutex_unlock(&peer->server->mutex);

    if (!msg) {
      return 0;
    }

    if (signaling_send_text(peer->socket, msg->json) != 0) {
      free(msg->json);
      free(msg);
      return -1;
    }

    free(msg->json);
    free(msg);
  }
}

static void close_peer_locked(webrtc_peer_t *peer) {
  peer->closing = 1;
  wake_peer_locked(peer);
}

static int consume_peer_message_budget_locked(
    const webrtc_signaling_server_t *server, webrtc_peer_t *peer,
    uint64_t now_ms) {
  if (!server || !peer) {
    return 1;
  }
  return consume_token_bucket(
      server->config.messages_per_second, server->config.message_burst,
      &peer->rate_last_refill_ms, &peer->rate_tokens, now_ms);
}

static turbo_media_auth_config_t signaling_peer_auth_config(
    const webrtc_signaling_config_t *config) {
  turbo_media_auth_config_t auth_config = {
      .issuer = config ? config->jwt_issuer : NULL,
      .active_key_id = config ? config->jwt_active_key_id : NULL,
      .active_secret = config ? config->jwt_secret : NULL,
      .previous_key_id = config ? config->jwt_previous_key_id : NULL,
      .previous_secret = config ? config->jwt_previous_secret : NULL,
      .revoked_token_sha256 =
          config ? config->jwt_revoked_token_sha256 : NULL,
      .clock_skew_seconds = config ? config->jwt_clock_skew_seconds : 0,
      .max_ttl_seconds =
          config && config->jwt_max_ttl_seconds > 0
              ? config->jwt_max_ttl_seconds
              : TURBO_MEDIA_AUTH_DEFAULT_MAX_TTL_SECONDS};
  return auth_config;
}

static int authorize_join_message(const webrtc_signaling_server_t *server,
                                  json_value_t *data,
                                  tstr *authorized_peer_id) {
  json_value_t *room_value = NULL;
  json_value_t *peer_value = NULL;
  json_value_t *token_value = NULL;
  const char *room_id = NULL;
  const char *peer_id = NULL;
  const char *token = NULL;
  turbo_media_auth_config_t auth_config;
  turbo_media_auth_policy_t policy;

  if (!server || !data || !authorized_peer_id) {
    return -1;
  }
  *authorized_peer_id = NULL;
  if (!server->config.jwt_enabled) {
    return 0;
  }

  room_value = turbo_json_object_get(data, "room");
  peer_value = turbo_json_object_get(data, "peer_id");
  token_value = turbo_json_object_get(data, "token");
  if (!room_value || turbo_json_type(room_value) != TURBO_JSON_STRING ||
      !peer_value || turbo_json_type(peer_value) != TURBO_JSON_STRING ||
      !token_value || turbo_json_type(token_value) != TURBO_JSON_STRING) {
    return -1;
  }
  room_id = turbo_json_string(room_value);
  peer_id = turbo_json_string(peer_value);
  token = turbo_json_string(token_value);
  if (!room_id || !peer_id || !token) {
    return -1;
  }

  auth_config = signaling_peer_auth_config(&server->config);
  memset(&policy, 0, sizeof(policy));
  policy.audience = SIGNALING_PEER_AUTH_AUDIENCE;
  policy.required_scope = SIGNALING_PEER_JOIN_SCOPE;
  policy.room_id = room_id;
  policy.participant_id = peer_id;
  if (turbo_media_auth_authorize_token(token, &auth_config, &policy) !=
      TURBO_MEDIA_AUTH_SIGNED_TOKEN) {
    return -1;
  }

  *authorized_peer_id = tstr_dup(peer_id);
  return *authorized_peer_id ? 0 : -1;
}

static int bind_peer_identity_locked(webrtc_signaling_server_t *server,
                                     webrtc_peer_t *peer,
                                     tstr *authorized_peer_id) {
  webrtc_peer_t *existing = NULL;
  tstr old_id = NULL;

  if (!server->config.jwt_enabled) {
    return 0;
  }
  if (!authorized_peer_id || !*authorized_peer_id) {
    return -1;
  }
  if (peer->identity_bound) {
    return strcmp(peer->id, *authorized_peer_id) == 0 ? 0 : -1;
  }

  existing = find_peer_by_id_locked(server, *authorized_peer_id);
  if (existing && existing != peer) {
    return -2;
  }
  if (strcmp(peer->id, *authorized_peer_id) == 0) {
    peer->identity_bound = 1;
    return 0;
  }

  if (hash_map_put(&server->local_peers, authorized_peer_id, &peer) !=
      STL_OK) {
    return -3;
  }
  old_id = peer->id;
  hash_map_remove(&server->local_peers, &old_id, NULL);
  peer->id = *authorized_peer_id;
  *authorized_peer_id = NULL;
  peer->identity_bound = 1;
  tstr_free(old_id);
  return 0;
}

static void handle_join_message(webrtc_signaling_server_t *server, webrtc_peer_t *peer,
                                json_value_t *data,
                                tstr *authorized_peer_id) {
  json_value_t *room_value = turbo_json_object_get(data, "room");
  const char *room_id_str = NULL;
  webrtc_room_t *room_ptr = NULL;
  int bind_result = 0;
  int created_room = 0;
  int already_in_room = 0;
  const char *room_json = NULL;
  const char *peer_id_json = NULL;
  tstr escaped_room = NULL;
  tstr escaped_peer_id = NULL;
  tstr join_resp = NULL;
  tstr peer_list = NULL;
  tstr notify_msg = NULL;
  tstr err = NULL;

  if (!room_value || turbo_json_type(room_value) != TURBO_JSON_STRING) {
    err = create_error_message("Missing room");
    send_json_message_locked(peer, err);
    tstr_free(err);
    return;
  }

  bind_result =
      bind_peer_identity_locked(server, peer, authorized_peer_id);
  if (bind_result != 0) {
    err = create_error_message(bind_result == -2 ? "Peer ID already connected"
                                                 : "Peer identity rejected");
    send_json_message_locked(peer, err);
    tstr_free(err);
    close_peer_locked(peer);
    return;
  }

  room_id_str = turbo_json_string(room_value);
  if (!room_id_str) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(peer, err);
    tstr_free(err);
    return;
  }
  TLOG_INFOF("signal: peer {} joining room {}", peer->id, room_id_str);
  room_ptr = find_room_locked(server, room_id_str);
  already_in_room = room_ptr && peer->room_ptr == room_ptr;
  if (room_ptr && !already_in_room && server->config.max_peers > 0 &&
      room_ptr->peer_count >= server->config.max_peers) {
    err = create_error_message("Room peer limit reached");
    send_json_message_locked(peer, err);
    tstr_free(err);
    return;
  }

  if (peer->room_ptr && !already_in_room) {
    remove_peer_from_room_locked(server, peer);
  }

  if (!room_ptr) {
    room_ptr = create_room_locked(server, room_id_str);
    if (!room_ptr) {
      err = create_error_message("Room limit reached");
      send_json_message_locked(peer, err);
      tstr_free(err);
      return;
    }
    created_room = 1;
  }

  if (!already_in_room) {
    peer->room = tstr_dup(room_id_str);
    if (!peer->room) {
      err = create_error_message("Allocation failure");
      send_json_message_locked(peer, err);
      tstr_free(err);
      if (created_room) {
        destroy_room_locked(server, room_ptr);
      }
      return;
    }
    peer->room_ptr = room_ptr;
    peer->prev_in_room = NULL;

    if (room_ptr->peers_tail) {
      room_ptr->peers_tail->next_in_room = peer;
      peer->prev_in_room = room_ptr->peers_tail;
    }
    room_ptr->peers_tail = peer;
    if (!room_ptr->peers_head) {
      room_ptr->peers_head = peer;
    }
    room_ptr->peer_count++;
    peer->next_in_room = NULL;
  }

  room_json = json_string_maybe_escape(peer->room, &escaped_room);
  if (!room_json) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(peer, err);
    tstr_free(err);
    remove_peer_from_room_locked(server, peer);
    return;
  }
  peer_id_json = json_string_maybe_escape(peer->id, &escaped_peer_id);
  if (!peer_id_json) {
    tstr_free(escaped_room);
    err = create_error_message("Allocation failure");
    send_json_message_locked(peer, err);
    tstr_free(err);
    remove_peer_from_room_locked(server, peer);
    return;
  }

  join_resp = tstr_new();
  join_resp = tstr_cat_fmt(join_resp, "{\"type\":\"joined\",\"peerId\":\"%s\",\"room\":\"%s\"}",
                           peer_id_json, room_json);
  send_json_message_locked(peer, join_resp);
  tstr_free(join_resp);
  tstr_free(escaped_room);

  peer_list = create_peer_list_message_locked(room_ptr);
  send_json_message_locked(peer, peer_list);
  tstr_free(peer_list);

  notify_msg = tstr_new();
  notify_msg = tstr_cat_fmt(notify_msg, "{\"type\":\"peer-joined\",\"peerId\":\"%s\"}",
                            peer_id_json);
  broadcast_locked(server, peer->room, peer->id, notify_msg);
  tstr_free(notify_msg);
  tstr_free(escaped_peer_id);
}

static void handle_list_peers_message(webrtc_signaling_server_t *server, webrtc_peer_t *peer) {
  (void)server;
  TLOG_INFOF("signal: peer {} requested peer list", peer ? peer->id : "(null)");
  send_peer_list_locked(peer);
}

static void handle_offer_message(webrtc_signaling_server_t *server, webrtc_peer_t *from_peer,
                                 json_value_t *data) {
  json_value_t *to_value = turbo_json_object_get(data, "to");
  json_value_t *sdp_value = turbo_json_object_get(data, "sdp");
  const char *to_peer_id = NULL;
  const char *sdp = NULL;
  webrtc_peer_t *to_peer = NULL;
  const char *sdp_json = NULL;
  const char *from_json = NULL;
  tstr escaped_sdp = NULL;
  tstr escaped_from = NULL;
  tstr msg = NULL;
  tstr err = NULL;

  if (!to_value || !sdp_value) {
    err = create_error_message("Missing to/sdp");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  to_peer_id = turbo_json_string(to_value);
  sdp = turbo_json_string(sdp_value);
  if (!to_peer_id || !sdp) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  to_peer = find_routable_peer_locked(server, from_peer, to_peer_id);
  if (!to_peer) {
    err = create_error_message("Peer not available in room");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  sdp_json = json_string_maybe_escape(sdp, &escaped_sdp);
  if (!sdp_json) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }
  from_json = json_string_maybe_escape(from_peer->id, &escaped_from);
  if (!from_json) {
    tstr_free(escaped_sdp);
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }
  msg = tstr_new();
  msg = tstr_cat_fmt(msg, "{\"type\":\"offer\",\"from\":\"%s\",\"sdp\":\"%s\"}",
                     from_json, sdp_json);
  send_json_message_locked(to_peer, msg);
  tstr_free(msg);
  tstr_free(escaped_sdp);
  tstr_free(escaped_from);
}

static void handle_answer_message(webrtc_signaling_server_t *server, webrtc_peer_t *from_peer,
                                  json_value_t *data) {
  json_value_t *to_value = turbo_json_object_get(data, "to");
  json_value_t *sdp_value = turbo_json_object_get(data, "sdp");
  const char *to_peer_id = NULL;
  const char *sdp = NULL;
  webrtc_peer_t *to_peer = NULL;
  const char *sdp_json = NULL;
  const char *from_json = NULL;
  tstr escaped_sdp = NULL;
  tstr escaped_from = NULL;
  tstr msg = NULL;
  tstr err = NULL;

  if (!to_value || !sdp_value) {
    err = create_error_message("Missing to/sdp");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  to_peer_id = turbo_json_string(to_value);
  sdp = turbo_json_string(sdp_value);
  if (!to_peer_id || !sdp) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  to_peer = find_routable_peer_locked(server, from_peer, to_peer_id);
  if (!to_peer) {
    err = create_error_message("Peer not available in room");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  sdp_json = json_string_maybe_escape(sdp, &escaped_sdp);
  if (!sdp_json) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }
  from_json = json_string_maybe_escape(from_peer->id, &escaped_from);
  if (!from_json) {
    tstr_free(escaped_sdp);
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }
  msg = tstr_new();
  msg = tstr_cat_fmt(msg, "{\"type\":\"answer\",\"from\":\"%s\",\"sdp\":\"%s\"}", from_json,
                     sdp_json);
  send_json_message_locked(to_peer, msg);
  tstr_free(msg);
  tstr_free(escaped_sdp);
  tstr_free(escaped_from);
}

static void handle_candidate_message(webrtc_signaling_server_t *server, webrtc_peer_t *from_peer,
                                     json_value_t *data) {
  json_value_t *to_value = turbo_json_object_get(data, "to");
  json_value_t *cand_value = turbo_json_object_get(data, "candidate");
  const char *to_peer_id = NULL;
  const char *candidate = NULL;
  webrtc_peer_t *to_peer = NULL;
  const char *candidate_json = NULL;
  const char *from_json = NULL;
  tstr escaped_candidate = NULL;
  tstr escaped_from = NULL;
  tstr msg = NULL;
  tstr err = NULL;

  if (!to_value || !cand_value) {
    err = create_error_message("Missing to/candidate");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  to_peer_id = turbo_json_string(to_value);
  candidate = turbo_json_string(cand_value);
  if (!to_peer_id || !candidate) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  to_peer = find_routable_peer_locked(server, from_peer, to_peer_id);
  if (!to_peer) {
    err = create_error_message("Peer not available in room");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  candidate_json = json_string_maybe_escape(candidate, &escaped_candidate);
  if (!candidate_json) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }
  from_json = json_string_maybe_escape(from_peer->id, &escaped_from);
  if (!from_json) {
    tstr_free(escaped_candidate);
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }
  msg = tstr_new();
  msg = tstr_cat_fmt(msg, "{\"type\":\"candidate\",\"from\":\"%s\",\"candidate\":\"%s\"}",
                     from_json, candidate_json);
  send_json_message_locked(to_peer, msg);
  tstr_free(msg);
  tstr_free(escaped_candidate);
  tstr_free(escaped_from);
}

static void handle_end_of_candidates_message(webrtc_signaling_server_t *server,
                                             webrtc_peer_t *from_peer, json_value_t *data) {
  json_value_t *to_value = turbo_json_object_get(data, "to");
  const char *to_peer_id = NULL;
  webrtc_peer_t *to_peer = NULL;
  const char *from_json = NULL;
  tstr escaped_from = NULL;
  tstr msg = NULL;
  tstr err = NULL;

  if (!to_value) {
    err = create_error_message("Missing to");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  to_peer_id = turbo_json_string(to_value);
  if (!to_peer_id) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  to_peer = find_routable_peer_locked(server, from_peer, to_peer_id);
  if (!to_peer) {
    err = create_error_message("Peer not available in room");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  from_json = json_string_maybe_escape(from_peer->id, &escaped_from);
  if (!from_json) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }

  msg = tstr_new();
  msg = tstr_cat_fmt(msg, "{\"type\":\"end-of-candidates\",\"from\":\"%s\"}", from_json);
  send_json_message_locked(to_peer, msg);
  tstr_free(msg);
  tstr_free(escaped_from);
}

static void handle_message(webrtc_signaling_server_t *server, webrtc_peer_t *peer,
                           const char *message, size_t len) {
  char *null_terminated = NULL;
  json_value_t *root = NULL;
  json_value_t *type_value = NULL;
  const char *type = NULL;
  size_t type_len = 0;
  tstr err = NULL;
  tstr authorized_peer_id = NULL;
  int is_join = 0;
  uint64_t now_ms = turbo_monotonic_ms();

  turbo_mutex_lock(&server->mutex);
  if (!peer->closing &&
      !consume_peer_message_budget_locked(server, peer, now_ms)) {
    err = create_error_message("Message rate limit exceeded");
    server->message_rate_rejections++;
    send_json_message_locked(peer, err);
    close_peer_locked(peer);
  }
  if (peer->closing) {
    turbo_mutex_unlock(&server->mutex);
    tstr_free(err);
    return;
  }
  turbo_mutex_unlock(&server->mutex);

  null_terminated = (char *)malloc(len + 1);
  if (!null_terminated) {
    return;
  }
  memcpy(null_terminated, message, len);
  null_terminated[len] = '\0';

  if (turbo_parse_json((const uint8_t *)null_terminated, len, &root) != 0 || !root ||
      turbo_json_type(root) != TURBO_JSON_OBJECT) {
    err = create_error_message("Invalid JSON");
    turbo_mutex_lock(&server->mutex);
    if (!peer->closing) {
      peer->last_activity = now_ms;
      send_json_message_locked(peer, err);
    }
    turbo_mutex_unlock(&server->mutex);
    tstr_free(err);
    free(null_terminated);
    if (root) {
      turbo_free_json(&root);
    }
    return;
  }

  type_value = turbo_json_object_get(root, "type");
  if (!type_value || turbo_json_type(type_value) != TURBO_JSON_STRING) {
    err = create_error_message("Missing type");
    turbo_mutex_lock(&server->mutex);
    if (!peer->closing) {
      peer->last_activity = now_ms;
      send_json_message_locked(peer, err);
    }
    turbo_mutex_unlock(&server->mutex);
    tstr_free(err);
    turbo_free_json(&root);
    free(null_terminated);
    return;
  }

  type = turbo_json_string(type_value);
  type_len = turbo_json_string_len(type_value);
  is_join = type_len == 4 && strncmp(type, "join", 4) == 0;
  if ((type_len == 9 && strncmp(type, "candidate", 9) == 0) ||
      (type_len == 17 && strncmp(type, "end-of-candidates", 17) == 0)) {
    TLOG_DEBUGF("signal: received message type {} from {}", type,
               peer && peer->id ? peer->id : "(unknown)");
  } else {
    TLOG_INFOF("signal: received message type {} from {}", type,
              peer && peer->id ? peer->id : "(unknown)");
  }

  if (is_join &&
      authorize_join_message(server, root, &authorized_peer_id) != 0) {
    err = create_error_message("Unauthorized join");
    turbo_mutex_lock(&server->mutex);
    if (!peer->closing) {
      peer->last_activity = now_ms;
      server->authentication_rejections++;
      send_json_message_locked(peer, err);
      close_peer_locked(peer);
    }
    turbo_mutex_unlock(&server->mutex);
    tstr_free(err);
    turbo_free_json(&root);
    free(null_terminated);
    return;
  }

  turbo_mutex_lock(&server->mutex);
  if (peer->closing) {
    turbo_mutex_unlock(&server->mutex);
    tstr_free(authorized_peer_id);
    turbo_free_json(&root);
    free(null_terminated);
    return;
  }
  peer->last_activity = now_ms;
  if (server->config.jwt_enabled && !is_join && !peer->identity_bound) {
    err = create_error_message("Authenticated join required");
    send_json_message_locked(peer, err);
    close_peer_locked(peer);
    tstr_free(err);
  } else if (is_join) {
    handle_join_message(server, peer, root, &authorized_peer_id);
  } else if (type_len == 5 && strncmp(type, "offer", 5) == 0) {
    handle_offer_message(server, peer, root);
  } else if (type_len == 6 && strncmp(type, "answer", 6) == 0) {
    handle_answer_message(server, peer, root);
  } else if (type_len == 9 && strncmp(type, "candidate", 9) == 0) {
    handle_candidate_message(server, peer, root);
  } else if (type_len == 10 && strncmp(type, "list-peers", 10) == 0) {
    handle_list_peers_message(server, peer);
  } else if (type_len == 17 && strncmp(type, "end-of-candidates", 17) == 0) {
    handle_end_of_candidates_message(server, peer, root);
  } else {
    err = create_error_message("Unknown message type");
    send_json_message_locked(peer, err);
    tstr_free(err);
  }
  turbo_mutex_unlock(&server->mutex);

  tstr_free(authorized_peer_id);
  turbo_free_json(&root);
  free(null_terminated);
}

static void expire_peers_locked(webrtc_signaling_server_t *server,
                                uint64_t now) {
  webrtc_peer_t *peer = NULL;

  if (!server) {
    return;
  }

  for (peer = server->peers_head; peer; peer = peer->next) {
    if (!peer->closing && !peer->room_ptr &&
        server->config.join_timeout_ms > 0 &&
        now >= peer->connected_at &&
        now - peer->connected_at >=
            (uint64_t)server->config.join_timeout_ms) {
      server->join_timeout_rejections++;
      close_peer_locked(peer);
    } else if (!peer->closing && server->config.peer_timeout_ms > 0 &&
               now >= peer->last_activity &&
               now - peer->last_activity >
                   (uint64_t)server->config.peer_timeout_ms) {
      close_peer_locked(peer);
    }
  }
}

static void on_cleanup_timer(turbo_timer_t *handle) {
  webrtc_signaling_server_t *server =
      (webrtc_signaling_server_t *)turbo_timer_get_data(handle);

  if (!server ||
      (server->config.join_timeout_ms <= 0 &&
       server->config.peer_timeout_ms <= 0 &&
       !source_policy_enabled(&server->config))) {
    return;
  }

  turbo_mutex_lock(&server->mutex);
  {
    uint64_t now_ms = turbo_monotonic_ms();
    expire_peers_locked(server, now_ms);
    expire_source_states_locked(server, now_ms);
  }
  turbo_mutex_unlock(&server->mutex);
}

static void broadcast_post_cb(void *arg1, void *arg2) {
  signal_broadcast_op_t *op = (signal_broadcast_op_t *)arg1;
  (void)arg2;

  if (!op) {
    return;
  }

  turbo_mutex_lock(&op->server->mutex);
  broadcast_locked(op->server, op->room, op->from, op->message);
  turbo_mutex_unlock(&op->server->mutex);

  free_broadcast_op(op);
}

static void kick_post_cb(void *arg1, void *arg2) {
  signal_kick_op_t *op = (signal_kick_op_t *)arg1;
  webrtc_peer_t *peer = NULL;
  tstr msg = NULL;
  tstr escaped_reason = NULL;
  const char *reason_json = NULL;
  (void)arg2;

  if (!op) {
    return;
  }

  turbo_mutex_lock(&op->server->mutex);
  peer = find_peer_by_id_locked(op->server, op->peer_id);
  if (peer && peer->room && strcmp(peer->room, op->room_id) == 0) {
    reason_json = json_string_maybe_escape(
        op->reason ? (const char *)op->reason : "Kicked by admin", &escaped_reason);
    msg = tstr_new();
    if (reason_json) {
      msg = tstr_cat_fmt(msg, "{\"type\":\"kicked\",\"reason\":\"%s\"}", reason_json);
      send_json_message_locked(peer, msg);
    }
    close_peer_locked(peer);
    tstr_free(msg);
    tstr_free(escaped_reason);
  }
  turbo_mutex_unlock(&op->server->mutex);

  free_kick_op(op);
}

static void signaling_client_handler(coro_socket_t *client, void *arg) {
  webrtc_signaling_server_t *server = (webrtc_signaling_server_t *)arg;
  webrtc_peer_t *peer = NULL;
  signaling_source_key_t source_key;
  signaling_source_admission_result_t source_result =
      SIGNALING_SOURCE_ADMITTED;
  char *data = NULL;
  size_t len = 0;
  int recv_status = 0;
  int is_text = 0;
  tstr leave_msg = NULL;

  if (!server || !client) {
    return;
  }

  memset(&source_key, 0, sizeof(source_key));
  if (source_policy_enabled(&server->config) &&
      source_key_from_socket(client, &source_key) != 0) {
    turbo_mutex_lock(&server->mutex);
    record_source_rejection_locked(server, SIGNALING_SOURCE_REJECT_ADDRESS);
    turbo_mutex_unlock(&server->mutex);
    return;
  }

  turbo_mutex_lock(&server->mutex);
  if (source_policy_enabled(&server->config)) {
    source_result =
        admit_source_locked(server, &source_key, turbo_monotonic_ms());
    if (source_result != SIGNALING_SOURCE_ADMITTED) {
      record_source_rejection_locked(server, source_result);
      turbo_mutex_unlock(&server->mutex);
      return;
    }
  }

  peer = (webrtc_peer_t *)calloc(1, sizeof(*peer));
  if (!peer) {
    if (source_policy_enabled(&server->config)) {
      release_source_key_locked(server, &source_key, turbo_monotonic_ms());
    }
    turbo_mutex_unlock(&server->mutex);
    return;
  }

  peer->server = server;
  peer->source_key = source_key;
  peer->source_admitted = source_policy_enabled(&server->config);
  peer->id = generate_peer_id_locked(server);
  if (!peer->id) {
    release_source_locked(server, peer, turbo_monotonic_ms());
    turbo_mutex_unlock(&server->mutex);
    free(peer);
    return;
  }
  peer->socket = client;
  peer->connected_at = turbo_monotonic_ms();
  peer->last_activity = peer->connected_at;
  peer->rate_last_refill_ms = peer->connected_at;
  peer->rate_tokens =
      (uint64_t)server->config.message_burst * SIGNALING_RATE_TOKEN_UNITS;
  if (hash_map_put(&server->local_peers, &peer->id, &peer) != STL_OK) {
    release_source_locked(server, peer, turbo_monotonic_ms());
    turbo_mutex_unlock(&server->mutex);
    tstr_free(peer->id);
    free(peer);
    return;
  }
  peer->prev = server->peers_tail;
  if (server->peers_tail) {
    server->peers_tail->next = peer;
  }
  server->peers_tail = peer;
  if (!server->peers_head) {
    server->peers_head = peer;
  }
  server->peer_count++;
  coro_socket_set_user_data(client, server);
  coro_socket_set_timeout(client, SIGNALING_PEER_IO_TIMEOUT_MS);
  turbo_mutex_unlock(&server->mutex);
  for (;;) {
    if (flush_peer_outbox(peer) != 0) {
      break;
    }

    turbo_mutex_lock(&server->mutex);
    if (!server->running || peer->closing) {
      turbo_mutex_unlock(&server->mutex);
      break;
    }
    turbo_mutex_unlock(&server->mutex);

    data = NULL;
    len = 0;
    is_text = 0;
    recv_status = coro_socket_recv_ws(client, &data, &len, &is_text);
    if (recv_status == TURBO_ETIMEDOUT) {
      continue;
    }
    if (recv_status < 0) {
      break;
    }
    if (!data || len == 0) {
      continue;
    }
    if (!is_text) {
      coro_socket_free_recv(data);
      data = NULL;
      break;
    }

    handle_message(server, peer, data, len);

    coro_socket_free_recv(data);
    data = NULL;
  }

  if (data) {
    coro_socket_free_recv(data);
  }

  turbo_mutex_lock(&server->mutex);
  if (peer->room) {
    tstr escaped_peer_id = NULL;
    const char *peer_id_json = json_string_maybe_escape(peer->id, &escaped_peer_id);
    leave_msg = tstr_new();
    if (peer_id_json) {
      leave_msg = tstr_cat_fmt(leave_msg, "{\"type\":\"peer-left\",\"peerId\":\"%s\"}",
                               peer_id_json);
      broadcast_locked(server, peer->room, peer->id, leave_msg);
    }
    tstr_free(leave_msg);
    tstr_free(escaped_peer_id);
  }
  remove_peer_locked(server, peer);
  turbo_mutex_unlock(&server->mutex);
}

webrtc_signaling_server_t *webrtc_signaling_create(void *loop,
                                                   const webrtc_signaling_config_t *config) {
  webrtc_signaling_server_t *server = NULL;

  if (!config) {
    return NULL;
  }

  server = (webrtc_signaling_server_t *)calloc(1, sizeof(*server));
  if (!server) {
    return NULL;
  }

  server->loop = (turbo_loop_t *)loop;
  server->owns_loop = 0;
  if (!server->loop) {
    server->loop = turbo_loop_create();
    if (!server->loop) {
      free(server);
      return NULL;
    }
    server->owns_loop = 1;
  }

  server->config = *config;
  if (server->config.jwt_max_ttl_seconds == 0) {
    server->config.jwt_max_ttl_seconds =
        TURBO_MEDIA_AUTH_DEFAULT_MAX_TTL_SECONDS;
  }
  if (server->config.jwt_enabled) {
    turbo_media_auth_config_t auth_config =
        signaling_peer_auth_config(&server->config);
    if ((server->config.jwt_algo &&
         strcmp(server->config.jwt_algo, "HS256") != 0) ||
        turbo_media_auth_config_validate(&auth_config) != 0) {
      if (server->owns_loop) {
        turbo_loop_destroy(server->loop);
      }
      free(server);
      return NULL;
    }
  }
  if (server->config.join_timeout_ms < 0 ||
      server->config.messages_per_second < 0 ||
      server->config.message_burst < 0 ||
      server->config.max_connections_per_source < 0 ||
      server->config.source_admissions_per_second < 0 ||
      server->config.source_admission_burst < 0 ||
      server->config.source_state_ttl_ms < 0 ||
      ((server->config.messages_per_second == 0) !=
       (server->config.message_burst == 0)) ||
      ((server->config.source_admissions_per_second == 0) !=
       (server->config.source_admission_burst == 0)) ||
      (source_policy_enabled(&server->config) &&
       (server->config.max_source_states == 0U ||
        server->config.source_state_ttl_ms == 0))) {
    if (server->owns_loop) {
      turbo_loop_destroy(server->loop);
    }
    free(server);
    return NULL;
  }
  server->ctx = coro_context_create(server->loop);
  if (!server->ctx) {
    if (server->owns_loop) {
      turbo_loop_destroy(server->loop);
    }
    free(server);
    return NULL;
  }

  server->cleanup_timer = turbo_timer_create(server->loop);
  if (server->cleanup_timer) {
    turbo_timer_set_data(server->cleanup_timer, server);
  }

  turbo_mutex_init(&server->mutex);
  if (hash_map_init_bytes(
          &server->local_peers, sizeof(tstr), CMETA_ALIGNOF(tstr),
          sizeof(webrtc_peer_t *), CMETA_ALIGNOF(webrtc_peer_t *),
          SIZE_MAX / (sizeof(tstr) + sizeof(webrtc_peer_t *)),
          webrtc_str_hash, webrtc_str_equal, NULL) != STL_OK ||
      hash_map_init_bytes(
          &server->local_rooms, sizeof(tstr), CMETA_ALIGNOF(tstr),
          sizeof(webrtc_room_t *), CMETA_ALIGNOF(webrtc_room_t *),
          SIZE_MAX / (sizeof(tstr) + sizeof(webrtc_room_t *)),
          webrtc_str_hash, webrtc_str_equal, NULL) != STL_OK ||
      hash_map_init_bytes(
          &server->source_states, sizeof(signaling_source_key_t),
          CMETA_ALIGNOF(signaling_source_key_t),
          sizeof(signaling_source_state_t *),
          CMETA_ALIGNOF(signaling_source_state_t *),
          source_policy_enabled(&server->config)
              ? server->config.max_source_states
              : SIZE_MAX / (sizeof(signaling_source_key_t) +
                            sizeof(signaling_source_state_t *)),
          hash_bytes, hash_key_equal, NULL) != STL_OK ||
      (source_policy_enabled(&server->config) &&
       hash_map_reserve(&server->source_states,
                        server->config.max_source_states) != STL_OK)) {
    hash_map_destroy(&server->local_peers);
    hash_map_destroy(&server->local_rooms);
    destroy_source_states(server);
    turbo_mutex_destroy(&server->mutex);
    if (server->cleanup_timer) {
      turbo_timer_destroy(server->cleanup_timer);
    }
    coro_context_destroy(server->ctx);
    if (server->owns_loop) {
      turbo_loop_destroy(server->loop);
    }
    free(server);
    return NULL;
  }

  if (config->use_tls &&
      (!config->cert_file || config->cert_file[0] == '\0' ||
       !config->key_file || config->key_file[0] == '\0')) {
    webrtc_signaling_destroy(server);
    return NULL;
  }
  return server;
}

int webrtc_signaling_start(webrtc_signaling_server_t *server) {
  coro_socket_type_t socket_type = CORO_SOCKET_TCP_V4;
  turbo_tls_server_config_t tls_config;
  coro_ws_server_config_t ws_config = CORO_WS_SERVER_CONFIG_DEFAULT;

  if (!server || server->running) {
    return server ? 0 : -1;
  }

  if (server->listener) {
    signaling_drain_listener(server);
  }

  if (server->config.use_tls) {
    socket_type = CORO_SOCKET_TLS;
  } else if (server->config.host && strchr(server->config.host, ':')) {
    socket_type = CORO_SOCKET_TCP_V6;
  }

  server->listener = coro_socket_create(server->ctx, socket_type);
  if (!server->listener) {
    return -1;
  }

  if (server->config.use_tls) {
    memset(&tls_config, 0, sizeof(tls_config));
    tls_config.size = sizeof(tls_config);
    tls_config.cert_file = server->config.cert_file;
    tls_config.key_file = server->config.key_file;
    tls_config.client_auth = TURBO_TLS_CLIENT_AUTH_NONE;
    if (coro_socket_set_tls_server_config(server->listener, &tls_config) != 0) {
      signaling_drain_listener(server);
      return -1;
    }
  }

  ws_config.max_message_size = server->config.max_message_size;
  if (coro_socket_set_ws_server_config(server->listener, &ws_config) != 0) {
    signaling_drain_listener(server);
    return -1;
  }

  coro_socket_set_timeout(server->listener, SIGNALING_WS_HANDSHAKE_TIMEOUT_MS);
  if (coro_socket_listen_ws(server->listener,
                            server->config.host ? server->config.host : "0.0.0.0",
                            server->config.port, server->config.use_tls,
                            signaling_client_handler, server) != 0) {
    signaling_drain_listener(server);
    return -1;
  }

  server->running = 1;
  if ((server->config.join_timeout_ms > 0 ||
       server->config.peer_timeout_ms > 0 ||
       source_policy_enabled(&server->config)) &&
      server->cleanup_timer) {
    turbo_timer_start(server->cleanup_timer, on_cleanup_timer,
                      SIGNALING_CLEANUP_INTERVAL_MS,
                      SIGNALING_CLEANUP_INTERVAL_MS);
  }

  TLOG_INFOF("Signaling server listening on {}://{}:{}", server->config.use_tls ? "wss" : "ws",
            server->config.host ? server->config.host : "0.0.0.0", server->config.port);
  return 0;
}

void webrtc_signaling_stop(webrtc_signaling_server_t *server) {
  webrtc_peer_t *peer = NULL;
  coro_socket_t *listener = NULL;
  int post_stop = 0;
  int rc;

  if (!server) {
    return;
  }

  if (server->cleanup_timer) {
    turbo_timer_stop(server->cleanup_timer);
  }

  turbo_mutex_lock(&server->mutex);
  if (!server->running) {
    turbo_mutex_unlock(&server->mutex);
    return;
  }
  server->running = 0;
  for (peer = server->peers_head; peer; peer = peer->next) {
    close_peer_locked(peer);
  }
  if (server->listener && !server->stop_posted) {
    listener = server->listener;
    server->stop_posted = 1;
    post_stop = 1;
  }
  turbo_mutex_unlock(&server->mutex);

  if (post_stop) {
    rc = coro_post(server->ctx, signaling_stop_listener_post_cb, server, listener);
    if (rc != TURBO_OK) {
      turbo_mutex_lock(&server->mutex);
      server->stop_posted = 0;
      turbo_mutex_unlock(&server->mutex);
      TLOG_ERRORF("Failed to post signaling listener stop: {}", rc);
    }
  }
}

int webrtc_signaling_run(webrtc_signaling_server_t *server, turbo_run_mode_t mode) {
  int running;
  int stop_posted;

  if (!server || !server->ctx) {
    return 0;
  }

  turbo_mutex_lock(&server->mutex);
  running = server->running;
  stop_posted = server->stop_posted;
  turbo_mutex_unlock(&server->mutex);
  if (!running && !stop_posted && server->listener &&
      !coro_socket_server_is_stopped(server->listener)) {
    (void)coro_socket_server_stop(server->listener);
  }

  return coro_context_run(server->ctx, mode);
}

void webrtc_signaling_destroy(webrtc_signaling_server_t *server) {
  if (!server) {
    return;
  }

  webrtc_signaling_stop(server);

  if (server->listener) {
    signaling_drain_listener(server);
  }

  if (server->cleanup_timer) {
    turbo_timer_destroy(server->cleanup_timer);
  }
  if (server->ctx) {
    coro_context_destroy(server->ctx);
  }
  hash_map_destroy(&server->local_peers);
  hash_map_destroy(&server->local_rooms);
  destroy_source_states(server);
  turbo_mutex_destroy(&server->mutex);
  if (server->owns_loop && server->loop) {
    turbo_loop_destroy(server->loop);
  }
  free(server);
}

int webrtc_signaling_get_peer_count(webrtc_signaling_server_t *server) {
  int count = 0;
  if (!server) {
    return 0;
  }
  turbo_mutex_lock(&server->mutex);
  count = server->peer_count;
  turbo_mutex_unlock(&server->mutex);
  return count;
}

int webrtc_signaling_broadcast(webrtc_signaling_server_t *server, const char *room,
                               const char *from, const char *message) {
  int sent = 0;
  signal_broadcast_op_t *op = NULL;
  webrtc_room_t *room_ptr = NULL;
  webrtc_peer_t *peer = NULL;

  if (!server || !room || !message) {
    return -1;
  }

  turbo_mutex_lock(&server->mutex);
  room_ptr = find_room_locked(server, room);
  if (room_ptr) {
    for (peer = room_ptr->peers_head; peer; peer = peer->next_in_room) {
      if (!from || strcmp(peer->id, from) != 0) {
        sent++;
      }
    }
  }
  turbo_mutex_unlock(&server->mutex);

  op = (signal_broadcast_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    return -1;
  }
  op->server = server;
  op->room = tstr_dup(room);
  op->from = tstr_dup(from ? from : "");
  op->message = tstr_dup(message);
  if (!op->room || !op->from || !op->message) {
    free_broadcast_op(op);
    return -1;
  }
  if (coro_post(server->ctx, broadcast_post_cb, op, NULL) != TURBO_OK) {
    free_broadcast_op(op);
    return -1;
  }
  return sent;
}

int webrtc_signaling_get_room_count(webrtc_signaling_server_t *server) {
  int count = 0;
  if (!server) {
    return 0;
  }
  turbo_mutex_lock(&server->mutex);
  count = server->room_count;
  turbo_mutex_unlock(&server->mutex);
  return count;
}

char *webrtc_signaling_get_rooms_json(webrtc_signaling_server_t *server) {
  tstr json = tstr_new();
  int added = 0;
  webrtc_room_t *room = NULL;
  char *result = NULL;

  if (!server) {
    return NULL;
  }

  turbo_mutex_lock(&server->mutex);
  json = tstr_cat(json, "{\"rooms\":[");
  for (room = server->rooms_head; room; room = room->next) {
    tstr escaped_room_id = NULL;
    const char *room_id_json = json_string_maybe_escape(room->id, &escaped_room_id);
    if (!room_id_json) {
      turbo_mutex_unlock(&server->mutex);
      tstr_free(json);
      return NULL;
    }
    if (added > 0) {
      json = tstr_cat(json, ",");
    }
    json = tstr_cat_fmt(json, "{\"id\":\"%s\",\"peer_count\":%d}",
                        room_id_json, room->peer_count);
    tstr_free(escaped_room_id);
    added++;
  }
  json = tstr_cat_fmt(json, "],\"total\":%d}", server->room_count);
  turbo_mutex_unlock(&server->mutex);

  result = tstr_to_cstr(json);
  tstr_free(json);
  return result;
}

char *webrtc_signaling_get_room_peers_json(webrtc_signaling_server_t *server, const char *room_id) {
  tstr list = NULL;
  webrtc_room_t *room = NULL;
  char *result = NULL;

  if (!server || !room_id) {
    return NULL;
  }

  turbo_mutex_lock(&server->mutex);
  room = find_room_locked(server, room_id);
  if (room) {
    list = create_peer_list_message_locked(room);
  }
  turbo_mutex_unlock(&server->mutex);

  if (!list) {
    return NULL;
  }

  result = tstr_to_cstr(list);
  tstr_free(list);
  return result;
}

int webrtc_signaling_kick_peer(webrtc_signaling_server_t *server, const char *room_id,
                               const char *peer_id, const char *reason) {
  signal_kick_op_t *op = NULL;
  int ret = -1;
  webrtc_peer_t *peer = NULL;

  if (!server || !room_id || !peer_id) {
    return -1;
  }

  turbo_mutex_lock(&server->mutex);
  peer = find_peer_by_id_locked(server, peer_id);
  if (peer && peer->room && strcmp(peer->room, room_id) == 0) {
    ret = 0;
  }
  turbo_mutex_unlock(&server->mutex);
  if (ret != 0) {
    return -1;
  }

  op = (signal_kick_op_t *)calloc(1, sizeof(*op));
  if (!op) {
    return -1;
  }
  op->server = server;
  op->room_id = tstr_dup(room_id);
  op->peer_id = tstr_dup(peer_id);
  op->reason = tstr_dup(reason ? reason : "Kicked by admin");
  if (!op->room_id || !op->peer_id || !op->reason) {
    free_kick_op(op);
    return -1;
  }
  if (coro_post(server->ctx, kick_post_cb, op, NULL) != TURBO_OK) {
    free_kick_op(op);
    return -1;
  }
  return 0;
}

char *webrtc_signaling_get_status_json(webrtc_signaling_server_t *server) {
  tstr json = tstr_new();
  char *result = NULL;

  if (!server) {
    return NULL;
  }

  turbo_mutex_lock(&server->mutex);
  json = tstr_cat_fmt(json,
                      "{\"peer_count\":%d,\"room_count\":%d,"
                      "\"source_state_count\":%zu,\"timestamp\":%llu,"
                      "\"version\":\"1.0.0\",\"running\":%d,"
                      "\"rejections\":{\"authentication\":%llu,"
                      "\"join_timeout\":%llu,\"message_rate\":%llu,"
                      "\"outbox_overflow\":%llu,\"source_address\":%llu,"
                      "\"source_capacity\":%llu,\"source_rate\":%llu,"
                      "\"source_concurrency\":%llu}}",
                      server->peer_count, server->room_count,
                      hash_map_size(&server->source_states),
                      (unsigned long long)turbo_monotonic_ms(), server->running,
                      (unsigned long long)server->authentication_rejections,
                      (unsigned long long)server->join_timeout_rejections,
                      (unsigned long long)server->message_rate_rejections,
                      (unsigned long long)server->outbox_overflow_rejections,
                      (unsigned long long)server->source_address_rejections,
                      (unsigned long long)server->source_capacity_rejections,
                      (unsigned long long)server->source_rate_rejections,
                      (unsigned long long)server->source_concurrency_rejections);
  turbo_mutex_unlock(&server->mutex);

  result = tstr_to_cstr(json);
  tstr_free(json);
  return result;
}
