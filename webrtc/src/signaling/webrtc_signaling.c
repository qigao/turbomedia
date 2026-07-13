/**
 * @file webrtc_signaling.c
 * @brief WebRTC signaling server implemented on CoroNet WebSockets.
 */

#include "webrtc_signaling.h"

#include "platform.h"
#include "tlog.h"
#include "turbo_parser.h"
#include "turbo_str.h"

#include <CoroNet/turbo_coro_context.h>
#include <CoroNet/turbo_coro_socket.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct peer_message_s peer_message_t;
typedef struct webrtc_room_s webrtc_room_t;

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

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
  unsigned char *recv_buffer;
  size_t recv_buffer_len;
  size_t recv_buffer_cap;
  int waiting_for_recv;
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

static int webrtc_str_eq(const tstr_t *a, const tstr_t *b) {
  return strcmp(*a, *b) == 0;
}

static size_t webrtc_str_hash(const tstr_t *k) {
  const char *str = *k;
  size_t hash = 14695981039346656037ULL;
  while (*str) {
    hash ^= (unsigned char)*str++;
    hash *= 1099511628211ULL;
  }
  return hash;
}

#define i_type PeerMap
#define i_key tstr_t
#define i_no_clone
#define i_eq webrtc_str_eq
#define i_hash webrtc_str_hash
#define i_val webrtc_peer_t *
#include <stc/hmap.h>

#define i_type RoomMap
#define i_key tstr_t
#define i_no_clone
#define i_eq webrtc_str_eq
#define i_hash webrtc_str_hash
#define i_val webrtc_room_t *
#include <stc/hmap.h>

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

  PeerMap local_peers;
  RoomMap local_rooms;

  webrtc_peer_t *peers_head;
  webrtc_peer_t *peers_tail;
  int peer_count;

  webrtc_room_t *rooms_head;
  webrtc_room_t *rooms_tail;
  int room_count;
  uint64_t next_peer_sequence;

  int running;
  turbo_timer_t *cleanup_timer;
  turbo_mutex_t mutex;
};

static int ws_send_text(coro_socket_t *socket, const char *json);

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
  const PeerMap_value *res = PeerMap_get(&server->local_peers, (tstr_t)id);
  return res ? res->second : NULL;
}

static webrtc_room_t *find_room_locked(webrtc_signaling_server_t *server, const char *id) {
  const RoomMap_value *res = RoomMap_get(&server->local_rooms, (tstr_t)id);
  return res ? res->second : NULL;
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
  if (server->rooms_tail) {
    server->rooms_tail->next = room;
    room->prev = server->rooms_tail;
  }
  server->rooms_tail = room;
  if (!server->rooms_head) {
    server->rooms_head = room;
  }

  RoomMap_insert(&server->local_rooms, room->id, room);
  server->room_count++;
  return room;
}

static void destroy_room_locked(webrtc_signaling_server_t *server, webrtc_room_t *room) {
  RoomMap_erase(&server->local_rooms, room->id);
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
  if (peer->socket && peer->waiting_for_recv) {
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
    PeerMap_erase(&server->local_peers, peer->id);
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
  free(peer->recv_buffer);
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

    if (ws_send_text(peer->socket, msg->json) != 0) {
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

static int ws_buffer_reserve(unsigned char **buffer, size_t *capacity, size_t needed) {
  unsigned char *new_buffer = NULL;
  size_t new_capacity = 0;

  if (needed <= *capacity) {
    return 0;
  }

  new_capacity = *capacity ? *capacity : 1024;
  while (new_capacity < needed) {
    new_capacity *= 2;
  }

  new_buffer = (unsigned char *)realloc(*buffer, new_capacity);
  if (!new_buffer) {
    return TURBO_ENOMEM;
  }

  *buffer = new_buffer;
  *capacity = new_capacity;
  return 0;
}

static int ws_buffer_append(unsigned char **buffer, size_t *length, size_t *capacity,
                            const void *data, size_t data_len) {
  int rc = 0;

  if (data_len == 0) {
    return 0;
  }

  rc = ws_buffer_reserve(buffer, capacity, *length + data_len);
  if (rc != 0) {
    return rc;
  }

  memcpy(*buffer + *length, data, data_len);
  *length += data_len;
  return 0;
}

static void ws_buffer_consume(unsigned char *buffer, size_t *length, size_t consumed) {
  if (consumed >= *length) {
    *length = 0;
    return;
  }

  memmove(buffer, buffer + consumed, *length - consumed);
  *length -= consumed;
}

static int ws_ascii_eq_ci(const char *a, const char *b) {
  unsigned char ca = 0;
  unsigned char cb = 0;

  while (*a && *b) {
    ca = (unsigned char)*a++;
    cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') {
      ca = (unsigned char)(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (unsigned char)(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return 0;
    }
  }

  return *a == '\0' && *b == '\0';
}

static int ws_ascii_eq_n_ci(const char *a, const char *b, size_t n) {
  size_t i = 0;

  for (i = 0; i < n; ++i) {
    unsigned char ca = (unsigned char)a[i];
    unsigned char cb = (unsigned char)b[i];
    if (ca >= 'A' && ca <= 'Z') {
      ca = (unsigned char)(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (unsigned char)(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return 0;
    }
  }

  return 1;
}

static size_t ws_find_double_crlf(const unsigned char *buffer, size_t length) {
  size_t i = 0;

  if (length < 4) {
    return SIZE_MAX;
  }

  for (i = 0; i + 3 < length; ++i) {
    if (buffer[i] == '\r' && buffer[i + 1] == '\n' && buffer[i + 2] == '\r' &&
        buffer[i + 3] == '\n') {
      return i;
    }
  }

  return SIZE_MAX;
}

static const char *ws_trim_left(const char *value) {
  while (*value == ' ' || *value == '\t') {
    value++;
  }
  return value;
}

static int ws_find_header_value(const char *request, const char *name, char *out, size_t out_size) {
  const char *line = request;
  size_t name_len = strlen(name);

  while (line && *line) {
    const char *line_end = strstr(line, "\r\n");
    const char *colon = strchr(line, ':');
    size_t value_len = 0;

    if (!line_end) {
      line_end = line + strlen(line);
    }
    if (line_end == line) {
      break;
    }

    if (colon && colon < line_end) {
      size_t header_len = (size_t)(colon - line);
      if (header_len == name_len && ws_ascii_eq_n_ci(line, name, name_len)) {
        const char *value = ws_trim_left(colon + 1);
        while (line_end > value && (line_end[-1] == ' ' || line_end[-1] == '\t')) {
          line_end--;
        }
        value_len = (size_t)(line_end - value);
        if (value_len + 1 > out_size) {
          return TURBO_EMSGSIZE;
        }
        memcpy(out, value, value_len);
        out[value_len] = '\0';
        return 0;
      }
    }

    line = strstr(line, "\r\n");
    if (!line) {
      break;
    }
    line += 2;
  }

  return TURBO_ENOENT;
}

static int ws_send_frame(coro_socket_t *socket, unsigned char opcode, const void *payload,
                         size_t payload_len) {
  unsigned char header[10];
  unsigned char small_frame[2048];
  size_t header_len = 0;
  size_t total_len = 0;

  header[header_len++] = (unsigned char)(0x80u | (opcode & 0x0Fu));
  if (payload_len < 126) {
    header[header_len++] = (unsigned char)payload_len;
  } else if (payload_len <= 0xFFFFu) {
    header[header_len++] = 126;
    header[header_len++] = (unsigned char)((payload_len >> 8) & 0xFFu);
    header[header_len++] = (unsigned char)(payload_len & 0xFFu);
  } else {
    header[header_len++] = 127;
    header[header_len++] = 0;
    header[header_len++] = 0;
    header[header_len++] = 0;
    header[header_len++] = 0;
    header[header_len++] = (unsigned char)((payload_len >> 24) & 0xFFu);
    header[header_len++] = (unsigned char)((payload_len >> 16) & 0xFFu);
    header[header_len++] = (unsigned char)((payload_len >> 8) & 0xFFu);
    header[header_len++] = (unsigned char)(payload_len & 0xFFu);
  }

  total_len = header_len + payload_len;
  if (payload_len > 0 && total_len <= sizeof(small_frame)) {
    memcpy(small_frame, header, header_len);
    memcpy(small_frame + header_len, payload, payload_len);
    return coro_socket_send(socket, (const char *)small_frame, total_len);
  }

  if (coro_socket_send(socket, (const char *)header, header_len) != 0) {
    return -1;
  }
  if (payload_len > 0 && coro_socket_send(socket, (const char *)payload, payload_len) != 0) {
    return -1;
  }

  return 0;
}

static int ws_send_text(coro_socket_t *socket, const char *json) {
  return ws_send_frame(socket, 0x1u, json, strlen(json));
}

static int ws_build_accept_key(const char *client_key, unsigned char *accept_b64,
                               size_t accept_b64_size) {
  char accept_source[128];
  int len = 0;
  unsigned char accept_sha1[SHA_DIGEST_LENGTH];

  if (!client_key || !accept_b64 || accept_b64_size < 29) {
    return TURBO_EINVAL;
  }

  len = snprintf(accept_source, sizeof(accept_source), "%s%s", client_key, WS_GUID);
  if (len < 0 || (size_t)len >= sizeof(accept_source)) {
    return TURBO_EPROTO;
  }

  SHA1((const unsigned char *)accept_source, (size_t)len, accept_sha1);
  if (EVP_EncodeBlock(accept_b64, accept_sha1, SHA_DIGEST_LENGTH) <= 0) {
    return TURBO_EPROTO;
  }

  return 0;
}

static int ws_server_handshake(coro_socket_t *socket, unsigned char **buffer, size_t *buffer_len,
                               size_t *buffer_cap) {
  char request_key[96];
  char request_protocol[128];
  size_t header_end = SIZE_MAX;
  char *request = NULL;
  char *recv_data = NULL;
  size_t recv_len = 0;
  int rc = 0;

  for (;;) {
    size_t request_len = 0;
    unsigned char accept_b64[64];
    tstr_t response = NULL;

    header_end = ws_find_double_crlf(*buffer, *buffer_len);
    if (header_end == SIZE_MAX) {
      recv_data = NULL;
      recv_len = 0;
      rc = coro_socket_recv(socket, &recv_data, &recv_len);
      if (rc != 0) {
        if (recv_data) {
          coro_socket_free_recv(recv_data);
        }
        return rc;
      }
      if (!recv_data || recv_len == 0) {
        continue;
      }
      rc = ws_buffer_append(buffer, buffer_len, buffer_cap, recv_data, recv_len);
      coro_socket_free_recv(recv_data);
      if (rc != 0) {
        return rc;
      }
      if (*buffer_len > 65536) {
        return TURBO_EMSGSIZE;
      }
      continue;
    }

    request_len = header_end + 4;
    request = (char *)malloc(request_len + 1);
    if (!request) {
      return TURBO_ENOMEM;
    }
    memcpy(request, *buffer, request_len);
    request[request_len] = '\0';

    if (strncmp(request, "GET ", 4) != 0) {
      free(request);
      return TURBO_EPROTO;
    }
    if (ws_find_header_value(request, "Sec-WebSocket-Key", request_key,
                             sizeof(request_key)) != 0) {
      free(request);
      return TURBO_EPROTO;
    }

    request_protocol[0] = '\0';
    ws_find_header_value(request, "Sec-WebSocket-Protocol", request_protocol,
                         sizeof(request_protocol));
    if (request_protocol[0] != '\0' && !ws_ascii_eq_ci(request_protocol, "webrtc-signaling")) {
      free(request);
      return TURBO_EPROTO;
    }

    rc = ws_build_accept_key(request_key, accept_b64, sizeof(accept_b64));
    if (rc != 0) {
      free(request);
      return rc;
    }

    response = tstr_new();
    response = tstr_cat(response,
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: ");
    response = tstr_cat(response, (const char *)accept_b64);
    response = tstr_cat(response, "\r\n");
    if (request_protocol[0] != '\0') {
      response = tstr_cat(response, "Sec-WebSocket-Protocol: ");
      response = tstr_cat(response, request_protocol);
      response = tstr_cat(response, "\r\n");
    }
    response = tstr_cat(response, "\r\n");

    if (coro_socket_send(socket, response, strlen(response)) != 0) {
      tstr_free(response);
      free(request);
      return -1;
    }

    tstr_free(response);
    free(request);
    ws_buffer_consume(*buffer, buffer_len, request_len);
    return 0;
  }
}

static int ws_recv_text_message(coro_socket_t *socket, unsigned char **buffer, size_t *buffer_len,
                                size_t *buffer_cap, char **message, size_t *message_len) {
  char *recv_data = NULL;
  size_t recv_len = 0;
  int rc = 0;

  *message = NULL;
  *message_len = 0;

  for (;;) {
    if (*buffer_len >= 2) {
      const unsigned char *frame = *buffer;
      unsigned char opcode = (unsigned char)(frame[0] & 0x0Fu);
      int fin = (frame[0] & 0x80u) != 0;
      int masked = (frame[1] & 0x80u) != 0;
      uint64_t payload_len = (uint64_t)(frame[1] & 0x7Fu);
      size_t header_len = 2;
      size_t i = 0;
      unsigned char mask[4];

      if (!fin) {
        return TURBO_EPROTO;
      }
      if (!masked) {
        return TURBO_EPROTO;
      }

      if (payload_len == 126) {
        if (*buffer_len < 4) {
          payload_len = UINT64_MAX;
        } else {
          payload_len = ((uint64_t)frame[2] << 8) | (uint64_t)frame[3];
          header_len = 4;
        }
      } else if (payload_len == 127) {
        if (*buffer_len < 10) {
          payload_len = UINT64_MAX;
        } else {
          payload_len = ((uint64_t)frame[2] << 56) | ((uint64_t)frame[3] << 48) |
                        ((uint64_t)frame[4] << 40) | ((uint64_t)frame[5] << 32) |
                        ((uint64_t)frame[6] << 24) | ((uint64_t)frame[7] << 16) |
                        ((uint64_t)frame[8] << 8) | (uint64_t)frame[9];
          header_len = 10;
        }
      }

      if (payload_len == UINT64_MAX) {
        goto need_more_data;
      }
      if (payload_len > SIZE_MAX - (header_len + 4)) {
        return TURBO_EMSGSIZE;
      }
      if (*buffer_len < header_len + 4 + (size_t)payload_len) {
        goto need_more_data;
      }

      memcpy(mask, frame + header_len, 4);
      if (opcode == 0x8u) {
        ws_buffer_consume(*buffer, buffer_len, header_len + 4 + (size_t)payload_len);
        return TURBO_EOF;
      }
      if (opcode == 0x9u) {
        for (i = 0; i < (size_t)payload_len; ++i) {
          (*buffer)[header_len + 4 + i] ^= mask[i % 4];
        }
        ws_send_frame(socket, 0xAu, *buffer + header_len + 4, (size_t)payload_len);
        ws_buffer_consume(*buffer, buffer_len, header_len + 4 + (size_t)payload_len);
        continue;
      }
      if (opcode == 0xAu) {
        ws_buffer_consume(*buffer, buffer_len, header_len + 4 + (size_t)payload_len);
        continue;
      }
      if (opcode != 0x1u && opcode != 0x2u) {
        return TURBO_EPROTO;
      }

      *message = (char *)malloc((size_t)payload_len + 1);
      if (!*message) {
        return TURBO_ENOMEM;
      }
      for (i = 0; i < (size_t)payload_len; ++i) {
        (*message)[i] =
            (char)(frame[header_len + 4 + i] ^ mask[i % 4]);
      }
      (*message)[payload_len] = '\0';
      *message_len = (size_t)payload_len;
      ws_buffer_consume(*buffer, buffer_len, header_len + 4 + (size_t)payload_len);
      return 0;
    }

need_more_data:
    recv_data = NULL;
    recv_len = 0;
    rc = coro_socket_recv(socket, &recv_data, &recv_len);
    if (rc == TURBO_ETIMEDOUT && *buffer_len == 0) {
      return rc;
    }
    if (rc != 0) {
      if (recv_data) {
        coro_socket_free_recv(recv_data);
      }
      return rc;
    }
    if (!recv_data || recv_len == 0) {
      if (*buffer_len == 0) {
        return 0;
      }
      continue;
    }
    rc = ws_buffer_append(buffer, buffer_len, buffer_cap, recv_data, recv_len);
    coro_socket_free_recv(recv_data);
    if (rc != 0) {
      return rc;
    }
    if (*buffer_len > 1024 * 1024) {
      return TURBO_EMSGSIZE;
    }
  }
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
  unsigned char *recv_buffer = NULL;
  size_t recv_buffer_len = 0;
  size_t recv_buffer_cap = 0;
  tstr_t leave_msg = NULL;

  if (!server || !client) {
    return;
  }

  if (server->config.jwt_enabled) {
    TLOG_ERROR("JWT authentication is not supported on the CoroNet signaling server yet");
    return;
  }

  coro_socket_set_timeout(client, 5000);
  recv_status = ws_server_handshake(client, &recv_buffer, &recv_buffer_len, &recv_buffer_cap);
  if (recv_status != 0) {
    free(recv_buffer);
    if (recv_status != TURBO_EOF && recv_status != TURBO_ETIMEDOUT) {
      TLOG_ERROR("Failed to complete accepted WebSocket handshake: {} ({})", recv_status,
                 turbo_strerror(recv_status));
    }
    return;
  }

  turbo_mutex_lock(&server->mutex);
  if (server->config.max_peers > 0 && server->peer_count >= server->config.max_peers) {
    turbo_mutex_unlock(&server->mutex);
    free(recv_buffer);
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
    free(recv_buffer);
    free(peer);
    return;
  }
  peer->socket = client;
  peer->recv_buffer = recv_buffer;
  peer->recv_buffer_len = recv_buffer_len;
  peer->recv_buffer_cap = recv_buffer_cap;
  peer->last_activity = turbo_monotonic_ms();
  peer->prev = server->peers_tail;
  if (server->peers_tail) {
    server->peers_tail->next = peer;
  }
  server->peers_tail = peer;
  if (!server->peers_head) {
    server->peers_head = peer;
  }
  server->peer_count++;
  PeerMap_insert(&server->local_peers, peer->id, peer);
  coro_socket_set_user_data(client, server);
  coro_socket_set_timeout(client, 1000);
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
    peer->waiting_for_recv = 1;
    recv_status = ws_recv_text_message(client, &peer->recv_buffer, &peer->recv_buffer_len,
                                       &peer->recv_buffer_cap, &data, &len);
    peer->waiting_for_recv = 0;
    if (recv_status == TURBO_ETIMEDOUT) {
      continue;
    }
    if (recv_status < 0) {
      break;
    }
    if (!data || len == 0) {
      continue;
    }

    turbo_mutex_lock(&server->mutex);
    if (!peer->closing) {
      handle_message(server, peer, data, len);
    }
    turbo_mutex_unlock(&server->mutex);

    free(data);
    data = NULL;
  }

  if (data) {
    free(data);
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
  server->local_peers = PeerMap_init();
  server->local_rooms = RoomMap_init();

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

  if (server->config.use_tls) {
    socket_type = CORO_SOCKET_TLS;
  } else if (server->config.host && strchr(server->config.host, ':')) {
    socket_type = CORO_SOCKET_TCP_V6;
  }

  server->listener = coro_socket_create(server->ctx, socket_type);
  if (!server->listener) {
    return -1;
  }

  if (coro_socket_listen_on(server->listener, server->config.host ? server->config.host : "0.0.0.0",
                            server->config.port, signaling_client_handler, server) != 0) {
    coro_socket_destroy(server->listener);
    server->listener = NULL;
    return -1;
  }

  server->running = 1;
  if (server->config.peer_timeout_ms > 0 && server->cleanup_timer) {
    turbo_timer_start(server->cleanup_timer, on_cleanup_timer, 1000, 1000);
  }

  TLOG_INFO("Signaling server listening on {}://{}:{}", server->config.use_tls ? "wss" : "ws",
            server->config.host ? server->config.host : "0.0.0.0", server->config.port);
  return 0;
}

void webrtc_signaling_stop(webrtc_signaling_server_t *server) {
  webrtc_peer_t *peer = NULL;

  if (!server || !server->running) {
    return;
  }

  if (server->cleanup_timer) {
    turbo_timer_stop(server->cleanup_timer);
  }

  turbo_mutex_lock(&server->mutex);
  server->running = 0;
  for (peer = server->peers_head; peer; peer = peer->next) {
    close_peer_locked(peer);
  }
  turbo_mutex_unlock(&server->mutex);

  if (server->listener) {
    coro_socket_destroy(server->listener);
    server->listener = NULL;
  }
}

int webrtc_signaling_run(webrtc_signaling_server_t *server, turbo_run_mode_t mode) {
  if (!server || !server->ctx) {
    return 0;
  }

  return coro_context_run(server->ctx, mode);
}

void webrtc_signaling_destroy(webrtc_signaling_server_t *server) {
  webrtc_peer_t *peer = NULL;
  webrtc_peer_t *next = NULL;

  if (!server) {
    return;
  }

  webrtc_signaling_stop(server);

  turbo_mutex_lock(&server->mutex);
  peer = server->peers_head;
  while (peer) {
    next = peer->next;
    if (peer->socket) {
      coro_socket_destroy(peer->socket);
      peer->socket = NULL;
    }
    remove_peer_locked(server, peer);
    peer = next;
  }
  turbo_mutex_unlock(&server->mutex);

  if (server->cleanup_timer) {
    turbo_timer_destroy(server->cleanup_timer);
  }
  if (server->ctx) {
    coro_context_destroy(server->ctx);
  }
  PeerMap_drop(&server->local_peers);
  RoomMap_drop(&server->local_rooms);
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
