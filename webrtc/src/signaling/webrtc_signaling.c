/**
 * @file webrtc_signaling.c
 * @brief WebRTC signaling server implemented on CoroNet WebSockets.
 */

#include "webrtc_signaling.h"

#include "platform.h"
#include "tlog.h"
#include "turbo_hash.h"
#include "turbo_parser.h"
#include "turbo_str.h"

#include <CoroNet/turbo_coro_context.h>
#include <CoroNet/turbo_coro_socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct peer_message_s peer_message_t;
typedef struct webrtc_room_s webrtc_room_t;

enum {
  SIGNALING_WS_HANDSHAKE_TIMEOUT_MS = 5000,
  SIGNALING_PEER_IO_TIMEOUT_MS = 1000,
  SIGNALING_CLEANUP_INTERVAL_MS = 1000
};

struct peer_message_s {
  char *json;
  peer_message_t *next;
};

struct webrtc_peer_s {
  tstr_t id;
  tstr_t room;
  webrtc_room_t *room_ptr;
  coro_socket_t *socket;
  uint64_t last_activity;
  webrtc_peer_t *next;
  webrtc_peer_t *prev;
  webrtc_peer_t *next_in_room;
  webrtc_peer_t *prev_in_room;
  peer_message_t *outbox_head;
  peer_message_t *outbox_tail;
  int closing;
};

struct webrtc_room_s {
  tstr_t id;
  webrtc_peer_t *peers_head;
  webrtc_peer_t *peers_tail;
  int peer_count;
  webrtc_room_t *next;
  webrtc_room_t *prev;
};

static size_t webrtc_str_hash(const void *key, size_t key_size, void *ctx) {
  const char *str = *(const char *const *)key;
  (void)key_size;
  return turbo_hash_bytes(str, strlen(str), ctx);
}

static bool webrtc_str_equal(const void *left, const void *right, size_t key_size, void *ctx) {
  const char *left_str = *(const char *const *)left;
  const char *right_str = *(const char *const *)right;
  (void)key_size;
  (void)ctx;
  return strcmp(left_str, right_str) == 0;
}

typedef struct {
  webrtc_signaling_server_t *server;
  tstr_t room;
  tstr_t from;
  tstr_t message;
} signal_broadcast_op_t;

typedef struct {
  webrtc_signaling_server_t *server;
  tstr_t room_id;
  tstr_t peer_id;
  tstr_t reason;
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

  turbo_hash_map_t local_peers;
  turbo_hash_map_t local_rooms;

  webrtc_peer_t *peers_head;
  webrtc_peer_t *peers_tail;
  int peer_count;

  webrtc_room_t *rooms_head;
  webrtc_room_t *rooms_tail;
  int room_count;
  uint64_t next_peer_sequence;

  int running;
  int stop_posted;
  turbo_timer_t *cleanup_timer;
  turbo_mutex_t mutex;
};

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

static tstr_t escape_json_string(const char *str) {
  tstr_t s = tstr_new();
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
      case '"':
        s = tstr_cat(s, "\\\"");
        break;
      case '\\':
        s = tstr_cat(s, "\\\\");
        break;
      default:
        s = tstr_cat_len(s, src, 1);
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
      case '"':
      case '\\':
        return 1;
      default:
        break;
    }
    src++;
  }

  return 0;
}

static const char *json_string_maybe_escape(const char *str, tstr_t *owned) {
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

static tstr_t generate_peer_id_locked(webrtc_signaling_server_t *server) {
  uint64_t timestamp = turbo_monotonic_ms();
  tstr_t id = tstr_new();
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
      (webrtc_peer_t *const *)turbo_hash_map_get_const(&server->local_peers, &id);
  return peer ? *peer : NULL;
}

static webrtc_room_t *find_room_locked(webrtc_signaling_server_t *server, const char *id) {
  webrtc_room_t *const *room =
      (webrtc_room_t *const *)turbo_hash_map_get_const(&server->local_rooms, &id);
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
  if (turbo_hash_map_put(&server->local_rooms, &room->id, &room) != TURBO_OK) {
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
  turbo_hash_map_remove(&server->local_rooms, &room->id, NULL);
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
}

static void wake_peer_locked(webrtc_peer_t *peer) {
  if (peer->socket) {
    coro_socket_interrupt_wait(peer->socket, 0);
  }
}

static int enqueue_message_locked(webrtc_peer_t *peer, const char *json) {
  peer_message_t *msg;
  size_t len;

  if (!peer || peer->closing) {
    return -1;
  }

  msg = (peer_message_t *)calloc(1, sizeof(*msg));
  if (!msg) {
    return -1;
  }

  len = strlen(json);
  msg->json = (char *)malloc(len + 1);
  if (!msg->json) {
    free(msg);
    return -1;
  }

  memcpy(msg->json, json, len + 1);

  if (peer->outbox_tail) {
    peer->outbox_tail->next = msg;
  } else {
    peer->outbox_head = msg;
  }
  peer->outbox_tail = msg;
  wake_peer_locked(peer);
  return 0;
}

static int send_json_message_locked(webrtc_peer_t *peer, const char *json) {
  return enqueue_message_locked(peer, json);
}

static tstr_t create_error_message(const char *error) {
  tstr_t msg = tstr_new();
  msg = tstr_cat_fmt(msg, "{\"type\":\"error\",\"error\":\"%s\"}", error);
  return msg;
}

static tstr_t create_peer_list_message_locked(webrtc_room_t *room) {
  tstr_t msg = tstr_new();
  int added = 0;
  webrtc_peer_t *peer = NULL;

  msg = tstr_cat(msg, "{\"type\":\"peers\",\"peers\":[");
  for (peer = room->peers_head; peer; peer = peer->next_in_room) {
    if (added > 0) {
      msg = tstr_cat(msg, ",");
    }
    msg = tstr_cat(msg, "\"");
    msg = tstr_cat(msg, peer->id);
    msg = tstr_cat(msg, "\"");
    added++;
  }
  msg = tstr_cat(msg, "]}");
  return msg;
}

static void send_peer_list_locked(webrtc_peer_t *peer) {
  tstr_t peer_list = NULL;

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
    turbo_hash_map_remove(&server->local_peers, &peer->id, NULL);
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

  free_outbox_locked(peer);
  if (peer->id) {
    tstr_free(peer->id);
  }
  free(peer);
}

static int flush_peer_outbox(webrtc_peer_t *peer) {
  peer_message_t *msg = NULL;

  for (;;) {
    turbo_mutex_lock(&((webrtc_signaling_server_t *)coro_socket_get_user_data(peer->socket))->mutex);
    msg = peer->outbox_head;
    if (msg) {
      peer->outbox_head = msg->next;
      if (!peer->outbox_head) {
        peer->outbox_tail = NULL;
      }
    }
    turbo_mutex_unlock(&((webrtc_signaling_server_t *)coro_socket_get_user_data(peer->socket))->mutex);

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

static void handle_join_message(webrtc_signaling_server_t *server, webrtc_peer_t *peer,
                                json_value_t *data) {
  json_value_t *room_value = turbo_json_object_get(data, "room");
  const char *room_id_str = NULL;
  webrtc_room_t *room_ptr = NULL;
  int created_room = 0;
  const char *room_json = NULL;
  tstr_t escaped_room = NULL;
  tstr_t join_resp = NULL;
  tstr_t peer_list = NULL;
  tstr_t notify_msg = NULL;
  tstr_t err = NULL;

  if (!room_value || turbo_json_type(room_value) != TURBO_JSON_STRING) {
    err = create_error_message("Missing room");
    send_json_message_locked(peer, err);
    tstr_free(err);
    return;
  }

  if (peer->room_ptr) {
    remove_peer_from_room_locked(server, peer);
  }

  room_id_str = turbo_json_string(room_value);
  if (!room_id_str) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(peer, err);
    tstr_free(err);
    return;
  }
  TLOG_INFO("signal: peer {} joining room {}", peer->id, room_id_str);
  room_ptr = find_room_locked(server, room_id_str);
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

  room_json = json_string_maybe_escape(peer->room, &escaped_room);
  if (!room_json) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(peer, err);
    tstr_free(err);
    remove_peer_from_room_locked(server, peer);
    return;
  }

  join_resp = tstr_new();
  join_resp = tstr_cat_fmt(join_resp, "{\"type\":\"joined\",\"peerId\":\"%s\",\"room\":\"%s\"}",
                           peer->id, room_json);
  send_json_message_locked(peer, join_resp);
  tstr_free(join_resp);
  tstr_free(escaped_room);

  peer_list = create_peer_list_message_locked(room_ptr);
  send_json_message_locked(peer, peer_list);
  tstr_free(peer_list);

  notify_msg = tstr_new();
  notify_msg = tstr_cat_fmt(notify_msg, "{\"type\":\"peer-joined\",\"peerId\":\"%s\"}", peer->id);
  broadcast_locked(server, peer->room, peer->id, notify_msg);
  tstr_free(notify_msg);
}

static void handle_list_peers_message(webrtc_signaling_server_t *server, webrtc_peer_t *peer) {
  (void)server;
  TLOG_INFO("signal: peer {} requested peer list", peer ? peer->id : "(null)");
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
  tstr_t escaped_sdp = NULL;
  tstr_t msg = NULL;
  tstr_t err = NULL;

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

  to_peer = find_peer_by_id_locked(server, to_peer_id);
  if (!to_peer) {
    err = create_error_message("Peer not found");
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
  msg = tstr_new();
  msg = tstr_cat_fmt(msg, "{\"type\":\"offer\",\"from\":\"%s\",\"sdp\":\"%s\"}", from_peer->id,
                     sdp_json);
  send_json_message_locked(to_peer, msg);
  tstr_free(msg);
  tstr_free(escaped_sdp);
}

static void handle_answer_message(webrtc_signaling_server_t *server, webrtc_peer_t *from_peer,
                                  json_value_t *data) {
  json_value_t *to_value = turbo_json_object_get(data, "to");
  json_value_t *sdp_value = turbo_json_object_get(data, "sdp");
  const char *to_peer_id = NULL;
  const char *sdp = NULL;
  webrtc_peer_t *to_peer = NULL;
  const char *sdp_json = NULL;
  tstr_t escaped_sdp = NULL;
  tstr_t msg = NULL;
  tstr_t err = NULL;

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

  to_peer = find_peer_by_id_locked(server, to_peer_id);
  if (!to_peer) {
    err = create_error_message("Peer not found");
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
  msg = tstr_new();
  msg = tstr_cat_fmt(msg, "{\"type\":\"answer\",\"from\":\"%s\",\"sdp\":\"%s\"}", from_peer->id,
                     sdp_json);
  send_json_message_locked(to_peer, msg);
  tstr_free(msg);
  tstr_free(escaped_sdp);
}

static void handle_candidate_message(webrtc_signaling_server_t *server, webrtc_peer_t *from_peer,
                                     json_value_t *data) {
  json_value_t *to_value = turbo_json_object_get(data, "to");
  json_value_t *cand_value = turbo_json_object_get(data, "candidate");
  const char *to_peer_id = NULL;
  const char *candidate = NULL;
  webrtc_peer_t *to_peer = NULL;
  const char *candidate_json = NULL;
  tstr_t escaped_candidate = NULL;
  tstr_t msg = NULL;
  tstr_t err = NULL;

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

  to_peer = find_peer_by_id_locked(server, to_peer_id);
  if (!to_peer) {
    return;
  }

  candidate_json = json_string_maybe_escape(candidate, &escaped_candidate);
  if (!candidate_json) {
    err = create_error_message("Allocation failure");
    send_json_message_locked(from_peer, err);
    tstr_free(err);
    return;
  }
  msg = tstr_new();
  msg = tstr_cat_fmt(msg, "{\"type\":\"candidate\",\"from\":\"%s\",\"candidate\":\"%s\"}",
                     from_peer->id, candidate_json);
  send_json_message_locked(to_peer, msg);
  tstr_free(msg);
  tstr_free(escaped_candidate);
}

static void handle_end_of_candidates_message(webrtc_signaling_server_t *server,
                                             webrtc_peer_t *from_peer, json_value_t *data) {
  json_value_t *to_value = turbo_json_object_get(data, "to");
  const char *to_peer_id = NULL;
  webrtc_peer_t *to_peer = NULL;
  tstr_t msg = NULL;
  tstr_t err = NULL;

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

  to_peer = find_peer_by_id_locked(server, to_peer_id);
  if (!to_peer) {
    return;
  }

  msg = tstr_new();
  msg = tstr_cat_fmt(msg, "{\"type\":\"end-of-candidates\",\"from\":\"%s\"}", from_peer->id);
  send_json_message_locked(to_peer, msg);
  tstr_free(msg);
}

static void handle_message(webrtc_signaling_server_t *server, webrtc_peer_t *peer,
                           const char *message, size_t len) {
  char *null_terminated = NULL;
  json_value_t *root = NULL;
  json_value_t *type_value = NULL;
  const char *type = NULL;
  size_t type_len = 0;
  tstr_t err = NULL;

  peer->last_activity = turbo_monotonic_ms();

  null_terminated = (char *)malloc(len + 1);
  if (!null_terminated) {
    return;
  }
  memcpy(null_terminated, message, len);
  null_terminated[len] = '\0';

  if (turbo_parse_json((const uint8_t *)null_terminated, len, &root) != 0 || !root ||
      turbo_json_type(root) != TURBO_JSON_OBJECT) {
    err = create_error_message("Invalid JSON");
    send_json_message_locked(peer, err);
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
    send_json_message_locked(peer, err);
    tstr_free(err);
    turbo_free_json(&root);
    free(null_terminated);
    return;
  }

  type = turbo_json_string(type_value);
  type_len = turbo_json_string_len(type_value);
  if ((type_len == 9 && strncmp(type, "candidate", 9) == 0) ||
      (type_len == 17 && strncmp(type, "end-of-candidates", 17) == 0)) {
    TLOG_DEBUG("signal: received message type {} from {}", type,
               peer && peer->id ? peer->id : "(unknown)");
  } else {
    TLOG_INFO("signal: received message type {} from {}", type,
              peer && peer->id ? peer->id : "(unknown)");
  }

  if (type_len == 4 && strncmp(type, "join", 4) == 0) {
    handle_join_message(server, peer, root);
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

  turbo_free_json(&root);
  free(null_terminated);
}

static void on_cleanup_timer(turbo_timer_t *handle) {
  webrtc_signaling_server_t *server =
      (webrtc_signaling_server_t *)turbo_timer_get_data(handle);
  uint64_t now = turbo_monotonic_ms();
  webrtc_peer_t *peer = NULL;

  if (!server || server->config.peer_timeout_ms <= 0) {
    return;
  }

  turbo_mutex_lock(&server->mutex);
  for (peer = server->peers_head; peer; peer = peer->next) {
    if (!peer->closing &&
        now - peer->last_activity > (uint64_t)server->config.peer_timeout_ms) {
      close_peer_locked(peer);
    }
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
  tstr_t msg = NULL;
  (void)arg2;

  if (!op) {
    return;
  }

  turbo_mutex_lock(&op->server->mutex);
  peer = find_peer_by_id_locked(op->server, op->peer_id);
  if (peer && peer->room && strcmp(peer->room, op->room_id) == 0) {
    msg = tstr_new();
    msg = tstr_cat_fmt(msg, "{\"type\":\"kicked\",\"reason\":\"%s\"}",
                       op->reason ? (const char *)op->reason : "Kicked by admin");
    send_json_message_locked(peer, msg);
    close_peer_locked(peer);
    tstr_free(msg);
  }
  turbo_mutex_unlock(&op->server->mutex);

  free_kick_op(op);
}

static void signaling_client_handler(coro_socket_t *client, void *arg) {
  webrtc_signaling_server_t *server = (webrtc_signaling_server_t *)arg;
  webrtc_peer_t *peer = NULL;
  char *data = NULL;
  size_t len = 0;
  int recv_status = 0;
  int is_text = 0;
  tstr_t leave_msg = NULL;

  if (!server || !client) {
    return;
  }

  if (server->config.jwt_enabled) {
    TLOG_ERROR("JWT authentication is not supported on the CoroNet signaling server yet");
    return;
  }

  turbo_mutex_lock(&server->mutex);
  if (server->config.max_peers > 0 && server->peer_count >= server->config.max_peers) {
    turbo_mutex_unlock(&server->mutex);
    return;
  }

  peer = (webrtc_peer_t *)calloc(1, sizeof(*peer));
  if (!peer) {
    turbo_mutex_unlock(&server->mutex);
    return;
  }

  peer->id = generate_peer_id_locked(server);
  if (!peer->id) {
    turbo_mutex_unlock(&server->mutex);
    free(peer);
    return;
  }
  peer->socket = client;
  peer->last_activity = turbo_monotonic_ms();
  if (turbo_hash_map_put(&server->local_peers, &peer->id, &peer) != TURBO_OK) {
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

    turbo_mutex_lock(&server->mutex);
    if (!peer->closing) {
      handle_message(server, peer, data, len);
    }
    turbo_mutex_unlock(&server->mutex);

    coro_socket_free_recv(data);
    data = NULL;
  }

  if (data) {
    coro_socket_free_recv(data);
  }

  turbo_mutex_lock(&server->mutex);
  if (peer->room) {
    leave_msg = tstr_new();
    leave_msg = tstr_cat_fmt(leave_msg, "{\"type\":\"peer-left\",\"peerId\":\"%s\"}", peer->id);
    broadcast_locked(server, peer->room, peer->id, leave_msg);
    tstr_free(leave_msg);
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
  if (turbo_hash_map_init(&server->local_peers, sizeof(tstr_t), sizeof(webrtc_peer_t *),
                          webrtc_str_hash, webrtc_str_equal, NULL) != TURBO_OK ||
      turbo_hash_map_init(&server->local_rooms, sizeof(tstr_t), sizeof(webrtc_room_t *),
                          webrtc_str_hash, webrtc_str_equal, NULL) != TURBO_OK) {
    turbo_hash_map_destroy(&server->local_peers);
    turbo_hash_map_destroy(&server->local_rooms);
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

  if (config->use_tls && (config->cert_file || config->key_file)) {
    TLOG_WARN("Custom TLS certificate configuration is not wired into CoroNet signaling yet");
  }

  return server;
}

int webrtc_signaling_start(webrtc_signaling_server_t *server) {
  coro_socket_type_t socket_type = CORO_SOCKET_TCP_V4;

  if (!server || server->running) {
    return server ? 0 : -1;
  }

  if (server->config.jwt_enabled) {
    TLOG_ERROR("JWT authentication is not supported on the CoroNet signaling server yet");
    return -1;
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

  coro_socket_set_timeout(server->listener, SIGNALING_WS_HANDSHAKE_TIMEOUT_MS);
  if (coro_socket_listen_ws(server->listener,
                            server->config.host ? server->config.host : "0.0.0.0",
                            server->config.port, server->config.use_tls,
                            signaling_client_handler, server) != 0) {
    signaling_drain_listener(server);
    return -1;
  }

  server->running = 1;
  if (server->config.peer_timeout_ms > 0 && server->cleanup_timer) {
    turbo_timer_start(server->cleanup_timer, on_cleanup_timer,
                      SIGNALING_CLEANUP_INTERVAL_MS,
                      SIGNALING_CLEANUP_INTERVAL_MS);
  }

  TLOG_INFO("Signaling server listening on {}://{}:{}", server->config.use_tls ? "wss" : "ws",
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
      TLOG_ERROR("Failed to post signaling listener stop: {}", rc);
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
  turbo_hash_map_destroy(&server->local_peers);
  turbo_hash_map_destroy(&server->local_rooms);
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
  coro_post(server->ctx, broadcast_post_cb, op, NULL);
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
  tstr_t json = tstr_new();
  int added = 0;
  webrtc_room_t *room = NULL;
  char *result = NULL;

  if (!server) {
    return NULL;
  }

  turbo_mutex_lock(&server->mutex);
  json = tstr_cat(json, "{\"rooms\":[");
  for (room = server->rooms_head; room; room = room->next) {
    if (added > 0) {
      json = tstr_cat(json, ",");
    }
    json = tstr_cat_fmt(json, "{\"id\":\"%s\",\"peer_count\":%d}", room->id, room->peer_count);
    added++;
  }
  json = tstr_cat_fmt(json, "],\"total\":%d}", server->room_count);
  turbo_mutex_unlock(&server->mutex);

  result = tstr_to_cstr(json);
  tstr_free(json);
  return result;
}

char *webrtc_signaling_get_room_peers_json(webrtc_signaling_server_t *server, const char *room_id) {
  tstr_t list = NULL;
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
  coro_post(server->ctx, kick_post_cb, op, NULL);
  return 0;
}

char *webrtc_signaling_get_status_json(webrtc_signaling_server_t *server) {
  tstr_t json = tstr_new();
  char *result = NULL;

  if (!server) {
    return NULL;
  }

  turbo_mutex_lock(&server->mutex);
  json = tstr_cat_fmt(json,
                      "{\"peer_count\":%d,\"room_count\":%d,\"timestamp\":%llu,"
                      "\"version\":\"1.0.0\",\"running\":%d}",
                      server->peer_count, server->room_count,
                      (unsigned long long)turbo_monotonic_ms(), server->running);
  turbo_mutex_unlock(&server->mutex);

  result = tstr_to_cstr(json);
  tstr_free(json);
  return result;
}
