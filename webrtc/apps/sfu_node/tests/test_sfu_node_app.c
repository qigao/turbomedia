#include "tinytest_compat.h"
#include "sfu_node/config.h"
#include "sfu_node/http_api.h"
#include "sfu_node/server.h"
#include "http_client.h"
#include "turbo_parser.h"
#include "turbo_peer_connection.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
static void app_test_sleep_ms(unsigned int ms) { Sleep(ms); }
static uint64_t app_test_now_ms(void) { return GetTickCount64(); }
#define app_strdup _strdup
#else
#include <unistd.h>
#include <time.h>
static void app_test_sleep_ms(unsigned int ms) { usleep(ms * 1000); }
static uint64_t app_test_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}
#define app_strdup strdup
#endif

#define MAX_TEST_CANDIDATES 32

static void app_test_set_env(const char *name, const char *value) {
#ifdef _WIN32
  _putenv_s(name, value ? value : "");
#else
  if (value) {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
#endif
}

static char *app_test_save_env(const char *name) {
  const char *value = getenv(name);
  return value ? app_strdup(value) : NULL;
}

static void app_test_restore_env(const char *name, char *saved_value) {
  app_test_set_env(name, saved_value);
  free(saved_value);
}

typedef struct {
  int connected;
  char *candidates[MAX_TEST_CANDIDATES];
  int candidate_count;
} offerer_callback_state_t;

typedef struct {
  int frame_count;
  size_t last_len;
  uint64_t last_timestamp;
} media_frame_state_t;

typedef struct {
  int connected;
  char *candidates[MAX_TEST_CANDIDATES];
  int candidate_count;
  int candidates_applied_to_sfu;
  int sfu_candidates_applied_to_peer;
  turbo_media_track_t *remote_track;
  media_frame_state_t frame_state;
} media_peer_state_t;

static json_value_t *json_object_field(const json_value_t *obj, const char *key) {
  json_value_t *value;

  if (!obj || !key) {
    return NULL;
  }

  value = turbo_json_object_get(obj, key);
  if (!value || turbo_json_type(value) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  return value;
}

static const char *json_string_value(const json_value_t *obj, const char *key) {
  json_value_t *value;

  if (!obj || !key) {
    return NULL;
  }

  value = turbo_json_object_get(obj, key);
  if (!value || turbo_json_type(value) != TURBO_JSON_STRING) {
    return NULL;
  }

  return turbo_json_string(value);
}

static int json_bool_value(const json_value_t *obj, const char *key, int def) {
  json_value_t *value;

  if (!obj || !key) {
    return def;
  }

  value = turbo_json_object_get(obj, key);
  if (!value || turbo_json_type(value) != TURBO_JSON_BOOL) {
    return def;
  }

  return turbo_json_bool(value) ? 1 : 0;
}

static int json_int_value(const json_value_t *obj, const char *key, int def) {
  json_value_t *value;

  if (!obj || !key) {
    return def;
  }

  value = turbo_json_object_get(obj, key);
  if (!value || turbo_json_type(value) != TURBO_JSON_NUMBER) {
    return def;
  }

  return (int)turbo_json_number(value);
}

static size_t json_array_count(const json_value_t *obj, const char *key) {
  json_value_t *value;

  if (!obj || !key) {
    return 0;
  }

  value = turbo_json_object_get(obj, key);
  if (!value || turbo_json_type(value) != TURBO_JSON_ARRAY) {
    return 0;
  }

  return turbo_json_array_size(value);
}

static const char *json_array_string_at(const json_value_t *obj, const char *key,
                                        size_t index) {
  json_value_t *array;
  json_value_t *item;

  if (!obj || !key) {
    return NULL;
  }

  array = turbo_json_object_get(obj, key);
  if (!array || turbo_json_type(array) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(array)) {
    return NULL;
  }

  item = turbo_json_array_get(array, index);
  if (!item || turbo_json_type(item) != TURBO_JSON_STRING) {
    return NULL;
  }

  return turbo_json_string(item);
}

static int parse_json_text(const char *json_text, json_value_t **out_root) {
  json_value_t *root = NULL;

  if (out_root) {
    *out_root = NULL;
  }
  if (!json_text || !out_root) {
    return -1;
  }

  if (turbo_parse_json((const uint8_t *)json_text, strlen(json_text), &root) != 0 ||
      !root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
    turbo_free_json(&root);
    return -1;
  }

  *out_root = root;
  return 0;
}

static json_value_t *http_get_json(const char *base_url, const char *path) {
  http_client_t *client;
  http_response_t *response;
  json_value_t *root = NULL;

  if (!base_url || !path) {
    return NULL;
  }

  client = http_client_create(base_url);
  if (!client) {
    return NULL;
  }

  http_client_set_timeout(client, 3000);
  response = http_get(client, path);
  if (!response || response->error_code != HTTP_ERROR_NONE ||
      response->status_code < 200 || response->status_code >= 300 ||
      !http_response_is_json(response)) {
    if (response) {
      http_response_free(response);
    }
    http_client_destroy(client);
    return NULL;
  }

  root = http_response_parse_json(response);
  http_response_free(response);
  http_client_destroy(client);
  if (!root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
    turbo_free_json(&root);
    return NULL;
  }

  return root;
}

static int http_get_status(const char *base_url, const char *path) {
  http_client_t *client;
  http_response_t *response;
  int status_code = 0;

  if (!base_url || !path) {
    return 0;
  }

  client = http_client_create(base_url);
  if (!client) {
    return 0;
  }

  http_client_set_timeout(client, 3000);
  response = http_get(client, path);
  if (response) {
    status_code = response->status_code;
    http_response_free(response);
  }
  http_client_destroy(client);

  return status_code;
}

static json_value_t *http_post_json_result(const char *base_url, const char *path,
                                           const char *body) {
  http_client_t *client;
  http_response_t *response;
  json_value_t *root = NULL;

  if (!base_url || !path || !body) {
    return NULL;
  }

  client = http_client_create(base_url);
  if (!client) {
    return NULL;
  }

  http_client_set_timeout(client, 3000);
  response = http_post_json(client, path, body);
  if (!response || response->error_code != HTTP_ERROR_NONE ||
      response->status_code < 200 || response->status_code >= 300 ||
      !http_response_is_json(response)) {
    if (response) {
      http_response_free(response);
    }
    http_client_destroy(client);
    return NULL;
  }

  root = http_response_parse_json(response);
  http_response_free(response);
  http_client_destroy(client);
  if (!root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
    turbo_free_json(&root);
    return NULL;
  }

  return root;
}

static int http_post_json_status_with_token(const char *base_url, const char *path,
                                            const char *body, const char *bearer_token) {
  http_client_t *client;
  http_response_t *response;
  int status_code = 0;

  if (!base_url || !path || !body) {
    return 0;
  }

  client = http_client_create(base_url);
  if (!client) {
    return 0;
  }

  http_client_set_timeout(client, 3000);
  if (bearer_token) {
    http_client_set_bearer_token(client, bearer_token);
  }
  response = http_post_json(client, path, body);
  if (response) {
    status_code = response->status_code;
    http_response_free(response);
  }
  http_client_destroy(client);

  return status_code;
}

static json_value_t *http_post_json_result_with_token(const char *base_url,
                                                      const char *path,
                                                      const char *body,
                                                      const char *bearer_token) {
  http_client_t *client;
  http_response_t *response;
  json_value_t *root = NULL;

  if (!base_url || !path || !body) {
    return NULL;
  }

  client = http_client_create(base_url);
  if (!client) {
    return NULL;
  }

  http_client_set_timeout(client, 3000);
  if (bearer_token) {
    http_client_set_bearer_token(client, bearer_token);
  }
  response = http_post_json(client, path, body);
  if (!response || response->error_code != HTTP_ERROR_NONE ||
      response->status_code < 200 || response->status_code >= 300 ||
      !http_response_is_json(response)) {
    if (response) {
      http_response_free(response);
    }
    http_client_destroy(client);
    return NULL;
  }

  root = http_response_parse_json(response);
  http_response_free(response);
  http_client_destroy(client);
  if (!root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
    turbo_free_json(&root);
    return NULL;
  }

  return root;
}

static int wait_for_http_status_ok(const char *base_url, const char *path, int retries,
                                   int delay_ms) {
  for (int i = 0; i < retries; ++i) {
    json_value_t *root = http_get_json(base_url, path);
    if (root) {
      turbo_free_json(&root);
      return 0;
    }
    app_test_sleep_ms((unsigned int)delay_ms);
  }

  return -1;
}

static char *escape_json_string(const char *str) {
  size_t extra = 0;
  size_t len;
  size_t i;
  size_t j;
  char *escaped;

  if (!str) {
    return NULL;
  }

  len = strlen(str);
  for (i = 0; i < len; ++i) {
    switch (str[i]) {
      case '\"':
      case '\\':
      case '\b':
      case '\f':
      case '\n':
      case '\r':
      case '\t':
        extra++;
        break;
      default:
        break;
    }
  }

  escaped = (char *)malloc(len + extra + 1);
  if (!escaped) {
    return NULL;
  }

  for (i = 0, j = 0; i < len; ++i) {
    switch (str[i]) {
      case '\"': escaped[j++] = '\\'; escaped[j++] = '\"'; break;
      case '\\': escaped[j++] = '\\'; escaped[j++] = '\\'; break;
      case '\b': escaped[j++] = '\\'; escaped[j++] = 'b'; break;
      case '\f': escaped[j++] = '\\'; escaped[j++] = 'f'; break;
      case '\n': escaped[j++] = '\\'; escaped[j++] = 'n'; break;
      case '\r': escaped[j++] = '\\'; escaped[j++] = 'r'; break;
      case '\t': escaped[j++] = '\\'; escaped[j++] = 't'; break;
      default: escaped[j++] = str[i]; break;
    }
  }

  escaped[j] = '\0';
  return escaped;
}

static void offerer_on_state_change(turbo_peer_connection_t *pc, turbo_peer_state_t state,
                                    void *user_data) {
  offerer_callback_state_t *offerer = (offerer_callback_state_t *)user_data;
  (void)pc;

  if (!offerer) {
    return;
  }

  if (state == TURBO_PEER_STATE_CONNECTED) {
    offerer->connected = 1;
  }
}

static void offerer_on_ice_candidate(turbo_peer_connection_t *pc, const char *candidate,
                                     void *user_data) {
  offerer_callback_state_t *offerer = (offerer_callback_state_t *)user_data;
  (void)pc;

  if (!offerer || !candidate || offerer->candidate_count >= MAX_TEST_CANDIDATES) {
    return;
  }

  for (int i = 0; i < offerer->candidate_count; ++i) {
    if (strcmp(offerer->candidates[i], candidate) == 0) {
      return;
    }
  }

  offerer->candidates[offerer->candidate_count] = app_strdup(candidate);
  if (offerer->candidates[offerer->candidate_count]) {
    offerer->candidate_count++;
  }
}

static void free_offerer_candidates(offerer_callback_state_t *offerer) {
  if (!offerer) {
    return;
  }

  for (int i = 0; i < offerer->candidate_count; ++i) {
    free(offerer->candidates[i]);
    offerer->candidates[i] = NULL;
  }
  offerer->candidate_count = 0;
}

static void free_media_peer_candidates(media_peer_state_t *peer) {
  if (!peer) {
    return;
  }

  for (int i = 0; i < peer->candidate_count; ++i) {
    free(peer->candidates[i]);
    peer->candidates[i] = NULL;
  }
  peer->candidate_count = 0;
}

static long file_size_bytes(const char *path) {
  FILE *fp;
  long size;

  if (!path) {
    return -1;
  }

  fp = fopen(path, "rb");
  if (!fp) {
    return -1;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }

  size = ftell(fp);
  fclose(fp);
  return size;
}

static void on_media_frame(turbo_media_track_t *track, const uint8_t *data, size_t len,
                           uint64_t timestamp, void *user_data) {
  media_frame_state_t *state = (media_frame_state_t *)user_data;
  (void)track;
  (void)data;

  if (!state) {
    return;
  }

  state->frame_count++;
  state->last_len = len;
  state->last_timestamp = timestamp;
}

static void media_peer_on_state_change(turbo_peer_connection_t *pc, turbo_peer_state_t state,
                                       void *user_data) {
  media_peer_state_t *peer = (media_peer_state_t *)user_data;
  (void)pc;

  if (!peer) {
    return;
  }

  if (state == TURBO_PEER_STATE_CONNECTED) {
    peer->connected = 1;
  }
}

static void media_peer_on_track(turbo_peer_connection_t *pc, turbo_media_track_t *track,
                                void *user_data) {
  media_peer_state_t *peer = (media_peer_state_t *)user_data;
  (void)pc;

  if (!peer || !track) {
    return;
  }

  peer->remote_track = track;
  turbo_media_track_on_frame(track, on_media_frame);
  turbo_media_track_set_user_data(track, &peer->frame_state);
}

static void media_peer_on_ice_candidate(turbo_peer_connection_t *pc, const char *candidate,
                                        void *user_data) {
  media_peer_state_t *peer = (media_peer_state_t *)user_data;
  (void)pc;

  if (!peer || !candidate || peer->candidate_count >= MAX_TEST_CANDIDATES) {
    return;
  }

  if (strstr(candidate, "127.0.0.1") == NULL) {
    return;
  }

  for (int i = 0; i < peer->candidate_count; ++i) {
    if (strcmp(peer->candidates[i], candidate) == 0) {
      return;
    }
  }

  peer->candidates[peer->candidate_count] = app_strdup(candidate);
  if (peer->candidates[peer->candidate_count]) {
    peer->candidate_count++;
  }
}

static void fill_video_i420_frame(uint8_t *frame, int width, int height, int frame_index) {
  size_t y_size = (size_t)width * (size_t)height;
  size_t uv_size = y_size / 4;

  for (size_t i = 0; i < y_size; ++i) {
    frame[i] = (uint8_t)((i + (size_t)(frame_index * 13)) & 0xFF);
  }
  for (size_t i = 0; i < uv_size; ++i) {
    frame[y_size + i] = (uint8_t)((80 + frame_index * 7 + (int)i) & 0xFF);
    frame[y_size + uv_size + i] = (uint8_t)((144 + frame_index * 5 + (int)i) & 0xFF);
  }
}

static json_value_t *build_session_root(sfu_node_app_server_t *server, const char *room_id,
                                        const char *session_id) {
  char *session_json;
  json_value_t *root = NULL;

  session_json = sfu_node_app_server_build_webrtc_session_json(server, room_id, session_id);
  if (!session_json) {
    return NULL;
  }

  if (parse_json_text(session_json, &root) != 0) {
    free(session_json);
    return NULL;
  }

  free(session_json);
  return root;
}

static char *copy_session_answer(sfu_node_app_server_t *server, const char *room_id,
                                 const char *session_id) {
  json_value_t *root = build_session_root(server, room_id, session_id);
  const char *answer_sdp;
  char *answer_copy = NULL;

  if (!root) {
    return NULL;
  }

  answer_sdp = json_string_value(root, "local_answer");
  if (answer_sdp) {
    answer_copy = app_strdup(answer_sdp);
  }
  turbo_free_json(&root);
  return answer_copy;
}

static int line_starts_with(const char *line, size_t len, const char *prefix) {
  size_t prefix_len;

  if (!line || !prefix) {
    return 0;
  }

  prefix_len = strlen(prefix);
  return len >= prefix_len && strncmp(line, prefix, prefix_len) == 0;
}

static int count_substring_occurrences(const char *text, const char *needle) {
  int count = 0;
  size_t needle_len;
  const char *cursor;

  if (!text || !needle || !needle[0]) {
    return 0;
  }

  needle_len = strlen(needle);
  cursor = text;
  while ((cursor = strstr(cursor, needle)) != NULL) {
    count++;
    cursor += needle_len;
  }
  return count;
}

static char *strip_sdp_candidates(const char *sdp) {
  const char *line_start;
  const char *cursor;
  size_t output_len = 0;
  char *result;
  char *write_ptr;

  if (!sdp) {
    return NULL;
  }

  line_start = sdp;
  cursor = sdp;
  while (*cursor) {
    const char *line_end = cursor;
    size_t line_len;

    while (*line_end && *line_end != '\n') {
      line_end++;
    }

    line_len = (size_t)(line_end - line_start);
    if (!line_starts_with(line_start, line_len, "a=candidate:") &&
        !line_starts_with(line_start, line_len, "a=end-of-candidates")) {
      output_len += line_len;
      if (*line_end == '\n') {
        output_len++;
      }
    }

    if (*line_end == '\0') {
      break;
    }

    cursor = line_end + 1;
    line_start = cursor;
  }

  result = (char *)malloc(output_len + 1);
  if (!result) {
    return NULL;
  }

  line_start = sdp;
  cursor = sdp;
  write_ptr = result;
  while (*cursor) {
    const char *line_end = cursor;
    size_t line_len;

    while (*line_end && *line_end != '\n') {
      line_end++;
    }

    line_len = (size_t)(line_end - line_start);
    if (!line_starts_with(line_start, line_len, "a=candidate:") &&
        !line_starts_with(line_start, line_len, "a=end-of-candidates")) {
      memcpy(write_ptr, line_start, line_len);
      write_ptr += line_len;
      if (*line_end == '\n') {
        *write_ptr++ = '\n';
      }
    }

    if (*line_end == '\0') {
      break;
    }

    cursor = line_end + 1;
    line_start = cursor;
  }

  *write_ptr = '\0';
  return result;
}

static void sync_peer_candidates_to_sfu(sfu_node_app_server_t *server, const char *room_id,
                                        const char *session_id, media_peer_state_t *peer) {
  if (!server || !room_id || !session_id || !peer) {
    return;
  }

  while (peer->candidates_applied_to_sfu < peer->candidate_count) {
    const char *candidate = peer->candidates[peer->candidates_applied_to_sfu];
    if (candidate) {
      TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_add_remote_ice_candidate(
                                   server, room_id, session_id, candidate));
    }
    peer->candidates_applied_to_sfu++;
  }
}

static void sync_sfu_candidates_to_peer(sfu_node_app_server_t *server, const char *room_id,
                                        const char *session_id,
                                        turbo_peer_connection_t *peer_pc,
                                        media_peer_state_t *peer_state) {
  json_value_t *root;
  size_t candidate_count;

  if (!server || !room_id || !session_id || !peer_pc || !peer_state) {
    return;
  }

  root = build_session_root(server, room_id, session_id);
  if (!root) {
    return;
  }

  candidate_count = json_array_count(root, "local_ice_candidates");
  while (peer_state->sfu_candidates_applied_to_peer < (int)candidate_count) {
    const char *candidate = json_array_string_at(
        root, "local_ice_candidates",
        (size_t)peer_state->sfu_candidates_applied_to_peer);
    if (candidate && strstr(candidate, "127.0.0.1") != NULL) {
      TEST_ASSERT_EQUAL_INT(0, turbo_peer_connection_add_ice_candidate(peer_pc, candidate));
    }
    peer_state->sfu_candidates_applied_to_peer++;
  }

  turbo_free_json(&root);
}

void test_sfu_node_webrtc_session_accepts_offer_and_generates_answer(void) {
  sfu_node_app_config_t sfu_config;
  sfu_node_app_server_t *sfu_server = NULL;
  turbo_sfu_node_t *node = NULL;
  turbo_peer_connection_t *offerer = NULL;
  turbo_media_track_t *send_track = NULL;
  turbo_peer_config_t peer_config;
  turbo_peer_callbacks_t peer_callbacks;
  offerer_callback_state_t offerer_state;
  json_value_t *root = NULL;
  json_value_t *session = NULL;
  char *session_json = NULL;
  char offer_sdp[16384];
  const char *answer_sdp = NULL;

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19411;
  sfu_config.node_id = "node-eu-1";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  TEST_ASSERT_NOT_NULL(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  TEST_ASSERT_NOT_NULL(node);

  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_attach_room(node, "room-webrtc", 4));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-webrtc", "alice",
                                                      "sess-alice", NULL));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-webrtc", "alice", "sess-alice"));

  memset(&offerer_state, 0, sizeof(offerer_state));
  memset(&peer_config, 0, sizeof(peer_config));
  memset(&peer_callbacks, 0, sizeof(peer_callbacks));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  peer_config.user_data = &offerer_state;
  peer_callbacks.on_state_change = offerer_on_state_change;
  peer_callbacks.on_ice_candidate = offerer_on_ice_candidate;

  offerer = turbo_peer_connection_create(&peer_config, &peer_callbacks);
  TEST_ASSERT_NOT_NULL(offerer);

  {
    turbo_media_track_config_t send_config;

    memset(&send_config, 0, sizeof(send_config));
    send_config.type = TURBO_MEDIA_TRACK_VIDEO;
    send_config.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
    send_config.codec = TURBO_CODEC_VP8;
    send_config.video.width = 160;
    send_config.video.height = 120;
    send_config.video.framerate = 30;
    send_config.video.bitrate = 500000;
    send_config.video.keyframe_interval = 30;
    send_track = turbo_peer_connection_add_track_ex(offerer, &send_config);
  }
  TEST_ASSERT_NOT_NULL(send_track);

  TEST_ASSERT_GREATER_THAN(0, turbo_peer_connection_create_offer(offerer, offer_sdp,
                                                                 sizeof(offer_sdp)));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_set_remote_offer(
                               sfu_server, "room-webrtc", "sess-alice", offer_sdp));
  session_json = sfu_node_app_server_build_webrtc_session_json(
      sfu_server, "room-webrtc", "sess-alice");
  TEST_ASSERT_NOT_NULL(session_json);
  TEST_ASSERT_EQUAL_INT(0, parse_json_text(session_json, &root));
  free(session_json);
  session_json = NULL;

  session = root;
  TEST_ASSERT_EQUAL_STRING("connecting", json_string_value(session, "state"));
  answer_sdp = json_string_value(session, "local_answer");
  TEST_ASSERT_NOT_NULL(answer_sdp);
  TEST_ASSERT_NOT_NULL(strstr(answer_sdp, "m=video"));
  TEST_ASSERT_NOT_NULL(strstr(answer_sdp, "a=recvonly"));
  TEST_ASSERT_EQUAL_INT(1, json_bool_value(session, "remote_description_set", 0));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(session, "remote_track_count", 0));
  TEST_ASSERT_TRUE(json_int_value(session, "local_candidate_count", -1) >= 0);
  TEST_ASSERT_EQUAL_INT(0, turbo_peer_connection_set_remote_description(
                               offerer, "answer", answer_sdp));
  turbo_free_json(&root);
  root = NULL;

  free_offerer_candidates(&offerer_state);
  turbo_peer_connection_destroy(offerer);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_webrtc_session_provisions_relay_track_before_answer(void) {
  sfu_node_app_config_t sfu_config;
  sfu_node_app_server_t *sfu_server = NULL;
  turbo_sfu_node_t *node = NULL;
  turbo_peer_connection_t *subscriber = NULL;
  turbo_media_track_t *recv_track = NULL;
  turbo_peer_config_t peer_config;
  char offer_sdp[16384];
  char *session_json = NULL;
  json_value_t *root = NULL;
  const char *answer_sdp = NULL;

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19413;
  sfu_config.node_id = "node-relay-1";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  TEST_ASSERT_NOT_NULL(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  TEST_ASSERT_NOT_NULL(node);

  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_attach_room(node, "room-relay", 4));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-relay", "alice",
                                                      "sess-alice", NULL));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-relay", "bob",
                                                      "sess-bob", NULL));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_register_published_track(
                               sfu_server, "room-relay", "alice", "track-cam",
                               0x11223344u, NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp8"));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_set_track_subscription(
                               sfu_server, "room-relay", "bob", "track-cam", 1,
                               TURBO_ROOM_VIDEO_LAYER_LOW));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-relay", "bob", "sess-bob"));

  memset(&peer_config, 0, sizeof(peer_config));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  subscriber = turbo_peer_connection_create(&peer_config, NULL);
  TEST_ASSERT_NOT_NULL(subscriber);

  {
    turbo_media_track_config_t recv_config;

    memset(&recv_config, 0, sizeof(recv_config));
    recv_config.type = TURBO_MEDIA_TRACK_VIDEO;
    recv_config.direction = TURBO_MEDIA_DIRECTION_RECVONLY;
    recv_config.codec = TURBO_CODEC_VP8;
    recv_config.video.width = 160;
    recv_config.video.height = 120;
    recv_config.video.framerate = 30;
    recv_config.video.bitrate = 500000;
    recv_config.video.keyframe_interval = 30;
    recv_track = turbo_peer_connection_add_track_ex(subscriber, &recv_config);
  }
  TEST_ASSERT_NOT_NULL(recv_track);

  TEST_ASSERT_GREATER_THAN(0, turbo_peer_connection_create_offer(subscriber, offer_sdp,
                                                                 sizeof(offer_sdp)));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_set_remote_offer(
                               sfu_server, "room-relay", "sess-bob", offer_sdp));

  session_json = sfu_node_app_server_build_webrtc_session_json(
      sfu_server, "room-relay", "sess-bob");
  TEST_ASSERT_NOT_NULL(session_json);
  TEST_ASSERT_EQUAL_INT(0, parse_json_text(session_json, &root));
  free(session_json);
  session_json = NULL;

  TEST_ASSERT_EQUAL_INT(1, json_int_value(root, "relay_track_count", -1));
  answer_sdp = json_string_value(root, "local_answer");
  TEST_ASSERT_NOT_NULL(answer_sdp);
  TEST_ASSERT_NOT_NULL(strstr(answer_sdp, "m=video"));
  TEST_ASSERT_NOT_NULL(strstr(answer_sdp, "a=sendonly"));

  turbo_free_json(&root);
  turbo_peer_connection_destroy(subscriber);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_webrtc_session_provisions_multiple_publishers_to_one_subscriber(void) {
  sfu_node_app_config_t sfu_config;
  sfu_node_app_server_t *sfu_server = NULL;
  turbo_sfu_node_t *node = NULL;
  char *session_json = NULL;
  json_value_t *root = NULL;
  const char *answer_sdp = NULL;
  const char *offer_sdp =
      "v=0\r\n"
      "o=- 1 1 IN IP4 0.0.0.0\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0 1\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=ice-ufrag:testufrag0\r\n"
      "a=ice-pwd:testpassword000000000000\r\n"
      "a=fingerprint:sha-256 "
      "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:"
      "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
      "a=setup:actpass\r\n"
      "a=mid:0\r\n"
      "a=recvonly\r\n"
      "a=rtcp-mux\r\n"
      "a=rtpmap:96 VP8/90000\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=ice-ufrag:testufrag1\r\n"
      "a=ice-pwd:testpassword111111111111\r\n"
      "a=fingerprint:sha-256 "
      "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:"
      "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF\r\n"
      "a=setup:actpass\r\n"
      "a=mid:1\r\n"
      "a=recvonly\r\n"
      "a=rtcp-mux\r\n"
      "a=rtpmap:96 VP8/90000\r\n";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19418;
  sfu_config.node_id = "node-relay-many-1";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  TEST_ASSERT_NOT_NULL(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  TEST_ASSERT_NOT_NULL(node);

  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_attach_room(node, "room-relay-many", 4));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-relay-many", "alice",
                                                      "sess-alice", NULL));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-relay-many", "carol",
                                                      "sess-carol", NULL));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-relay-many", "bob",
                                                      "sess-bob", NULL));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_register_published_track(
                               sfu_server, "room-relay-many", "alice", "track-alice-cam",
                               0x11223344u, NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp8"));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_register_published_track(
                               sfu_server, "room-relay-many", "carol", "track-carol-cam",
                               0x55667788u, NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp8"));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_set_track_subscription(
                               sfu_server, "room-relay-many", "bob", "track-alice-cam", 1,
                               TURBO_ROOM_VIDEO_LAYER_LOW));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_set_track_subscription(
                               sfu_server, "room-relay-many", "bob", "track-carol-cam", 1,
                               TURBO_ROOM_VIDEO_LAYER_LOW));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-relay-many", "bob", "sess-bob"));

  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_set_remote_offer(
                               sfu_server, "room-relay-many", "sess-bob", offer_sdp));

  session_json = sfu_node_app_server_build_webrtc_session_json(
      sfu_server, "room-relay-many", "sess-bob");
  TEST_ASSERT_NOT_NULL(session_json);
  TEST_ASSERT_EQUAL_INT(0, parse_json_text(session_json, &root));
  free(session_json);
  session_json = NULL;

  TEST_ASSERT_EQUAL_INT(2, json_int_value(root, "relay_track_count", -1));
  answer_sdp = json_string_value(root, "local_answer");
  TEST_ASSERT_NOT_NULL(answer_sdp);
  TEST_ASSERT_EQUAL_INT(2, count_substring_occurrences(answer_sdp, "m=video"));
  TEST_ASSERT_EQUAL_INT(2, count_substring_occurrences(answer_sdp, "a=sendonly"));

  turbo_free_json(&root);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_http_roundtrips_track_subscription_metadata(void) {
  sfu_node_app_config_t sfu_config;
  sfu_node_app_server_t *sfu_server = NULL;
  sfu_node_http_api_t *http_api = NULL;
  turbo_sfu_node_t *node = NULL;
  json_value_t *root = NULL;
  json_value_t *subscription = NULL;
  const char *sfu_node_base_url = "http://127.0.0.1:19416";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19416;
  sfu_config.node_id = "node-subscription-http-1";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  TEST_ASSERT_NOT_NULL(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  TEST_ASSERT_NOT_NULL(node);

  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_attach_room(node, "room-subscription-http", 4));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-subscription-http", "alice",
                                                      "sess-alice", NULL));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-subscription-http", "bob",
                                                      "sess-bob", NULL));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_register_published_track(
                               node, "room-subscription-http", "alice", "track-cam",
                               0x22113344u, NULL, 0));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_register_published_track(
                               sfu_server, "room-subscription-http", "alice", "track-cam",
                               0x22113344u, NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp9"));

  http_api = sfu_node_http_api_create(sfu_server);
  TEST_ASSERT_NOT_NULL(http_api);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_http_api_start(http_api, sfu_config.bind_host,
                                                   sfu_config.bind_port));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100));

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_track_subscription\","
      "\"room_id\":\"room-subscription-http\","
      "\"receiver_participant_id\":\"bob\","
      "\"track_id\":\"track-cam\","
      "\"enabled\":true,"
      "\"priority\":275,"
      "\"preferred_layer\":\"high\","
      "\"target_layer\":\"medium\","
      "\"muted\":false,"
      "\"policy_source\":\"conference_policy_pin\","
      "\"max_layer\":\"medium\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_track_subscription\","
      "\"room_id\":\"room-subscription-http\","
      "\"receiver_participant_id\":\"bob\","
      "\"track_id\":\"track-cam\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "track_subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_EQUAL_STRING("bob", json_string_value(subscription, "receiver_participant_id"));
  TEST_ASSERT_EQUAL_STRING("alice", json_string_value(subscription, "sender_participant_id"));
  TEST_ASSERT_TRUE(json_bool_value(subscription, "enabled", 0));
  TEST_ASSERT_EQUAL_INT(275, json_int_value(subscription, "priority", -1));
  TEST_ASSERT_EQUAL_STRING("high", json_string_value(subscription, "preferred_layer"));
  TEST_ASSERT_EQUAL_STRING("medium", json_string_value(subscription, "target_layer"));
  TEST_ASSERT_EQUAL_INT(0, json_bool_value(subscription, "muted", 1));
  TEST_ASSERT_EQUAL_STRING("conference_policy_pin",
                           json_string_value(subscription, "policy_source"));
  TEST_ASSERT_EQUAL_STRING("medium", json_string_value(subscription, "max_layer"));
  turbo_free_json(&root);

  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_http_control_token_protects_modifying_commands(void) {
  sfu_node_app_config_t sfu_config;
  sfu_node_app_server_t *sfu_server = NULL;
  sfu_node_http_api_t *http_api = NULL;
  json_value_t *root = NULL;
  const char *sfu_node_base_url = "http://127.0.0.1:19417";
  const char *attach_room_command =
      "{"
      "\"type\":\"attach_room\","
      "\"room_id\":\"room-auth\","
      "\"max_participants\":4"
      "}";
  const char *force_close_room_command =
      "{"
      "\"type\":\"force_close_room\","
      "\"room_id\":\"room-auth\""
      "}";
  const char *get_node_stats_command =
      "{"
      "\"type\":\"get_node_stats\""
      "}";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19417;
  sfu_config.node_id = "node-auth-http-1";
  sfu_config.control_token = "test-control-token";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  TEST_ASSERT_NOT_NULL(sfu_server);

  http_api = sfu_node_http_api_create(sfu_server);
  TEST_ASSERT_NOT_NULL(http_api);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_http_api_start(http_api, sfu_config.bind_host,
                                                   sfu_config.bind_port));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100));

  TEST_ASSERT_EQUAL_INT(200, http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 get_node_stats_command, NULL));
  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 attach_room_command, NULL));
  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 attach_room_command, "wrong-token"));
  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 force_close_room_command, NULL));

  root = http_post_json_result_with_token(sfu_node_base_url, "/api/v1/commands",
                                          attach_room_command, "test-control-token");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_ready_metrics_and_drain_control(void) {
  sfu_node_app_config_t sfu_config;
  sfu_node_app_server_t *sfu_server = NULL;
  sfu_node_http_api_t *http_api = NULL;
  json_value_t *root = NULL;
  json_value_t *node_stats = NULL;
  const char *sfu_node_base_url = "http://127.0.0.1:19421";
  const char *set_drain_command =
      "{"
      "\"type\":\"set_node_drain\","
      "\"draining\":true"
      "}";
  const char *clear_drain_command =
      "{"
      "\"type\":\"set_node_drain\","
      "\"draining\":false"
      "}";
  const char *attach_room_command =
      "{"
      "\"type\":\"attach_room\","
      "\"room_id\":\"room-after-drain\","
      "\"max_participants\":4"
      "}";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19421;
  sfu_config.node_id = "node-drain-http-1";
  sfu_config.control_token = "test-control-token";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  TEST_ASSERT_NOT_NULL(sfu_server);

  http_api = sfu_node_http_api_create(sfu_server);
  TEST_ASSERT_NOT_NULL(http_api);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_http_api_start(http_api, sfu_config.bind_host,
                                                   sfu_config.bind_port));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100));
  TEST_ASSERT_EQUAL_INT(200, http_get_status(sfu_node_base_url, "/ready"));
  TEST_ASSERT_EQUAL_INT(200, http_get_status(sfu_node_base_url, "/metrics"));

  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 set_drain_command, NULL));

  root = http_post_json_result_with_token(sfu_node_base_url, "/api/v1/commands",
                                          set_drain_command, "test-control-token");
  TEST_ASSERT_NOT_NULL(root);
  node_stats = json_object_field(root, "node_stats");
  TEST_ASSERT_NOT_NULL(node_stats);
  TEST_ASSERT_TRUE(json_bool_value(node_stats, "draining", 0));
  turbo_free_json(&root);

  TEST_ASSERT_EQUAL_INT(503, http_get_status(sfu_node_base_url, "/ready"));
  TEST_ASSERT_EQUAL_INT(409, http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 attach_room_command, "test-control-token"));

  root = http_post_json_result_with_token(sfu_node_base_url, "/api/v1/commands",
                                          clear_drain_command, "test-control-token");
  TEST_ASSERT_NOT_NULL(root);
  node_stats = json_object_field(root, "node_stats");
  TEST_ASSERT_NOT_NULL(node_stats);
  TEST_ASSERT_FALSE(json_bool_value(node_stats, "draining", 1));
  turbo_free_json(&root);

  TEST_ASSERT_EQUAL_INT(200, http_get_status(sfu_node_base_url, "/ready"));
  TEST_ASSERT_EQUAL_INT(200, http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 attach_room_command, "test-control-token"));

  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_config_reads_control_token_from_env(void) {
  sfu_node_app_config_t sfu_config;
  char *saved_token = app_test_save_env("TURBO_SFU_NODE_CONTROL_TOKEN");

  app_test_set_env("TURBO_SFU_NODE_CONTROL_TOKEN", "env-sfu-control-token");
  sfu_node_app_config_init(&sfu_config);
  TEST_ASSERT_EQUAL_STRING("env-sfu-control-token", sfu_config.control_token);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_config_validate(&sfu_config));

  app_test_restore_env("TURBO_SFU_NODE_CONTROL_TOKEN", saved_token);
}

void test_sfu_node_webrtc_session_http_commands_roundtrip_offer_and_query_session(void) {
  sfu_node_app_config_t sfu_config;
  sfu_node_app_server_t *sfu_server = NULL;
  sfu_node_http_api_t *http_api = NULL;
  turbo_sfu_node_t *node = NULL;
  turbo_peer_connection_t *offerer = NULL;
  turbo_media_track_t *send_track = NULL;
  turbo_peer_config_t peer_config;
  turbo_peer_callbacks_t peer_callbacks;
  offerer_callback_state_t offerer_state;
  json_value_t *root = NULL;
  json_value_t *session = NULL;
  char offer_sdp[16384];
  const char *answer_sdp = NULL;
  const char *sfu_node_base_url = "http://127.0.0.1:19412";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19412;
  sfu_config.node_id = "node-http-1";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  TEST_ASSERT_NOT_NULL(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  TEST_ASSERT_NOT_NULL(node);

  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_attach_room(node, "room-webrtc-http", 4));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-webrtc-http", "alice",
                                                      "sess-alice", NULL));

  http_api = sfu_node_http_api_create(sfu_server);
  TEST_ASSERT_NOT_NULL(http_api);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_http_api_start(http_api, sfu_config.bind_host,
                                                   sfu_config.bind_port));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100));
  fprintf(stderr, "http webrtc test: server ready\n");

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_webrtc_session\","
      "\"room_id\":\"room-webrtc-http\","
      "\"participant_id\":\"alice\","
      "\"session_id\":\"sess-alice\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  session = json_object_field(root, "webrtc_session");
  TEST_ASSERT_NOT_NULL(session);
  TEST_ASSERT_EQUAL_STRING("room-webrtc-http", json_string_value(session, "room_id"));
  TEST_ASSERT_EQUAL_STRING("alice", json_string_value(session, "participant_id"));
  TEST_ASSERT_EQUAL_STRING("sess-alice", json_string_value(session, "session_id"));
  TEST_ASSERT_EQUAL_STRING("new", json_string_value(session, "state"));
  TEST_ASSERT_EQUAL_INT(0, json_bool_value(session, "remote_description_set", 1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(session, "remote_track_count", -1));
  turbo_free_json(&root);
  root = NULL;
  fprintf(stderr, "http webrtc test: create session ok\n");

  memset(&offerer_state, 0, sizeof(offerer_state));
  memset(&peer_config, 0, sizeof(peer_config));
  memset(&peer_callbacks, 0, sizeof(peer_callbacks));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  peer_config.user_data = &offerer_state;
  peer_callbacks.on_state_change = offerer_on_state_change;
  peer_callbacks.on_ice_candidate = offerer_on_ice_candidate;

  offerer = turbo_peer_connection_create(&peer_config, &peer_callbacks);
  TEST_ASSERT_NOT_NULL(offerer);

  {
    turbo_media_track_config_t send_config;

    memset(&send_config, 0, sizeof(send_config));
    send_config.type = TURBO_MEDIA_TRACK_VIDEO;
    send_config.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
    send_config.codec = TURBO_CODEC_VP8;
    send_config.video.width = 160;
    send_config.video.height = 120;
    send_config.video.framerate = 30;
    send_config.video.bitrate = 500000;
    send_config.video.keyframe_interval = 30;
    send_track = turbo_peer_connection_add_track_ex(offerer, &send_config);
  }
  TEST_ASSERT_NOT_NULL(send_track);

  TEST_ASSERT_GREATER_THAN(0, turbo_peer_connection_create_offer(offerer, offer_sdp,
                                                                 sizeof(offer_sdp)));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_set_remote_offer(
                               sfu_server, "room-webrtc-http", "sess-alice", offer_sdp));

  root = http_get_json(sfu_node_base_url,
                       "/api/v1/rooms/room-webrtc-http/webrtc_sessions/sess-alice");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  fprintf(stderr, "http webrtc test: get session ok\n");
  session = json_object_field(root, "webrtc_session");
  TEST_ASSERT_NOT_NULL(session);
  TEST_ASSERT_EQUAL_STRING("room-webrtc-http", json_string_value(session, "room_id"));
  TEST_ASSERT_EQUAL_STRING("alice", json_string_value(session, "participant_id"));
  TEST_ASSERT_EQUAL_STRING("sess-alice", json_string_value(session, "session_id"));
  TEST_ASSERT_EQUAL_INT(1, json_bool_value(session, "remote_description_set", 0));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(session, "remote_track_count", 0));
  TEST_ASSERT_TRUE(json_array_count(session, "local_ice_candidates") >= 0);
  answer_sdp = json_string_value(session, "local_answer");
  TEST_ASSERT_NOT_NULL(answer_sdp);
  TEST_ASSERT_EQUAL_INT(0, turbo_peer_connection_set_remote_description(
                               offerer, "answer", answer_sdp));

  turbo_free_json(&root);
  free_offerer_candidates(&offerer_state);
  turbo_peer_connection_destroy(offerer);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_remove_webrtc_session(
                               sfu_server, "room-webrtc-http", "sess-alice"));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_remove_session(
                               node, "room-webrtc-http", "sess-alice"));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_detach_room(node, "room-webrtc-http"));
  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_media_bridge_forwards_video_to_subscriber(void) {
  sfu_node_app_config_t sfu_config;
  sfu_node_app_server_t *sfu_server = NULL;
  turbo_sfu_node_t *node = NULL;
  turbo_peer_connection_t *publisher = NULL;
  turbo_peer_connection_t *subscriber = NULL;
  turbo_media_track_t *publisher_track = NULL;
  turbo_media_track_t *subscriber_recv_track = NULL;
  turbo_peer_config_t peer_config;
  turbo_peer_callbacks_t publisher_callbacks;
  turbo_peer_callbacks_t subscriber_callbacks;
  media_peer_state_t publisher_state;
  media_peer_state_t subscriber_state;
  turbo_media_context_t *publisher_media = NULL;
  turbo_media_context_t *subscriber_media = NULL;
  turbo_media_track_config_t track_config;
  char publisher_offer[16384];
  char subscriber_offer[16384];
  char *publisher_answer = NULL;
  char *subscriber_answer = NULL;
  json_value_t *publisher_session = NULL;
  json_value_t *subscriber_session = NULL;
  uint32_t publisher_ssrc;
  uint8_t frame[160 * 120 + (160 * 120) / 2];
  uint64_t deadline_ms;
  int sender_started = 0;
  int sent_frames = 0;

  memset(&publisher_state, 0, sizeof(publisher_state));
  memset(&subscriber_state, 0, sizeof(subscriber_state));

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19414;
  sfu_config.node_id = "node-media-1";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  TEST_ASSERT_NOT_NULL(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  TEST_ASSERT_NOT_NULL(node);

  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_attach_room(node, "room-media", 4));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-media", "alice",
                                                      "sess-alice", NULL));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-media", "bob",
                                                      "sess-bob", NULL));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-media", "alice", "sess-alice"));

  memset(&peer_config, 0, sizeof(peer_config));
  memset(&publisher_callbacks, 0, sizeof(publisher_callbacks));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  peer_config.user_data = &publisher_state;
  publisher_callbacks.on_state_change = media_peer_on_state_change;
  publisher_callbacks.on_ice_candidate = media_peer_on_ice_candidate;
  publisher = turbo_peer_connection_create(&peer_config, &publisher_callbacks);
  TEST_ASSERT_NOT_NULL(publisher);
  publisher_media = turbo_peer_connection_get_media_context(publisher);
  TEST_ASSERT_NOT_NULL(publisher_media);

  memset(&track_config, 0, sizeof(track_config));
  track_config.type = TURBO_MEDIA_TRACK_VIDEO;
  track_config.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
  track_config.codec = TURBO_CODEC_VP8;
  track_config.video.width = 160;
  track_config.video.height = 120;
  track_config.video.framerate = 30;
  track_config.video.bitrate = 500000;
  track_config.video.keyframe_interval = 30;
  publisher_track = turbo_peer_connection_add_track_ex(publisher, &track_config);
  TEST_ASSERT_NOT_NULL(publisher_track);
  publisher_ssrc = turbo_media_track_get_ssrc(publisher_track);
  TEST_ASSERT_TRUE(publisher_ssrc != 0);

  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_register_published_track(
                               node, "room-media", "alice", "track-cam", publisher_ssrc, NULL, 0));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_register_published_track(
                               sfu_server, "room-media", "alice", "track-cam", publisher_ssrc,
                               NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp8"));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_set_receiver_bandwidth(
                               node, "room-media", "bob", 1500000u));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_set_track_subscription(
                               node, "room-media", "bob", "track-cam", 1,
                               TURBO_ROOM_VIDEO_LAYER_LOW));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_set_track_subscription(
                               sfu_server, "room-media", "bob", "track-cam", 1,
                               TURBO_ROOM_VIDEO_LAYER_LOW));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-media", "bob", "sess-bob"));

  memset(&peer_config, 0, sizeof(peer_config));
  memset(&subscriber_callbacks, 0, sizeof(subscriber_callbacks));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  peer_config.user_data = &subscriber_state;
  subscriber_callbacks.on_state_change = media_peer_on_state_change;
  subscriber_callbacks.on_track = media_peer_on_track;
  subscriber_callbacks.on_ice_candidate = media_peer_on_ice_candidate;
  subscriber = turbo_peer_connection_create(&peer_config, &subscriber_callbacks);
  TEST_ASSERT_NOT_NULL(subscriber);
  subscriber_media = turbo_peer_connection_get_media_context(subscriber);
  TEST_ASSERT_NOT_NULL(subscriber_media);

  memset(&track_config, 0, sizeof(track_config));
  track_config.type = TURBO_MEDIA_TRACK_VIDEO;
  track_config.direction = TURBO_MEDIA_DIRECTION_RECVONLY;
  track_config.codec = TURBO_CODEC_VP8;
  track_config.video.width = 160;
  track_config.video.height = 120;
  track_config.video.framerate = 30;
  track_config.video.bitrate = 500000;
  track_config.video.keyframe_interval = 30;
  subscriber_recv_track = turbo_peer_connection_add_track_ex(subscriber, &track_config);
  TEST_ASSERT_NOT_NULL(subscriber_recv_track);

  TEST_ASSERT_GREATER_THAN(0, turbo_peer_connection_create_offer(subscriber, subscriber_offer,
                                                                 sizeof(subscriber_offer)));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_set_remote_offer(
                                sfu_server, "room-media", "sess-bob",
                                subscriber_offer));
  subscriber_answer = copy_session_answer(sfu_server, "room-media", "sess-bob");
  TEST_ASSERT_NOT_NULL(subscriber_answer);
  TEST_ASSERT_EQUAL_INT(0, turbo_peer_connection_set_remote_description(
                                subscriber, "answer", subscriber_answer));
  sync_peer_candidates_to_sfu(sfu_server, "room-media", "sess-bob", &subscriber_state);
  sync_sfu_candidates_to_peer(sfu_server, "room-media", "sess-bob", subscriber,
                              &subscriber_state);

  TEST_ASSERT_GREATER_THAN(0, turbo_peer_connection_create_offer(publisher, publisher_offer,
                                                                 sizeof(publisher_offer)));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_set_remote_offer(
                                sfu_server, "room-media", "sess-alice",
                                publisher_offer));
  publisher_answer = copy_session_answer(sfu_server, "room-media", "sess-alice");
  TEST_ASSERT_NOT_NULL(publisher_answer);
  TEST_ASSERT_EQUAL_INT(0, turbo_peer_connection_set_remote_description(
                                publisher, "answer", publisher_answer));
  sync_peer_candidates_to_sfu(sfu_server, "room-media", "sess-alice", &publisher_state);
  sync_sfu_candidates_to_peer(sfu_server, "room-media", "sess-alice", publisher,
                              &publisher_state);

  deadline_ms = app_test_now_ms() + 15000ULL;
  while (app_test_now_ms() < deadline_ms) {
    sfu_node_app_server_poll_webrtc(sfu_server);
    turbo_peer_connection_poll(publisher);
    turbo_peer_connection_poll(subscriber);
    turbo_media_handle_timers(publisher_media);
    turbo_media_handle_timers(subscriber_media);
    sfu_node_app_server_poll_webrtc(sfu_server);

    sync_peer_candidates_to_sfu(sfu_server, "room-media", "sess-alice", &publisher_state);
    sync_peer_candidates_to_sfu(sfu_server, "room-media", "sess-bob", &subscriber_state);
    sync_sfu_candidates_to_peer(sfu_server, "room-media", "sess-alice", publisher,
                                &publisher_state);
    sync_sfu_candidates_to_peer(sfu_server, "room-media", "sess-bob", subscriber,
                                &subscriber_state);

    if (!sender_started && subscriber_state.remote_track &&
        turbo_media_track_get_state(subscriber_state.remote_track) == TURBO_MEDIA_STATE_ACTIVE) {
      TEST_ASSERT_EQUAL_INT(0, turbo_media_track_start(publisher_track));
      sender_started = 1;
    }

    if (sender_started && sent_frames < 16) {
      fill_video_i420_frame(frame, 160, 120, sent_frames);
      TEST_ASSERT_EQUAL_INT(0, turbo_media_track_send_frame(
                                   publisher_track, frame, sizeof(frame),
                                   90000ULL + (uint64_t)sent_frames * 3000ULL));
      sent_frames++;
    }

    if (subscriber_state.frame_state.frame_count > 0) {
      break;
    }

    app_test_sleep_ms(5);
  }

  deadline_ms = app_test_now_ms() + 1000ULL;
  while (app_test_now_ms() < deadline_ms) {
    publisher_session = build_session_root(sfu_server, "room-media", "sess-alice");
    if (publisher_session &&
        json_int_value(publisher_session, "remote_frame_count", 0) > 0) {
      break;
    }
    turbo_free_json(&publisher_session);
    publisher_session = NULL;
    sfu_node_app_server_poll_webrtc(sfu_server);
    turbo_peer_connection_poll(publisher);
    turbo_peer_connection_poll(subscriber);
    turbo_media_handle_timers(publisher_media);
    turbo_media_handle_timers(subscriber_media);
    app_test_sleep_ms(5);
  }

  if (publisher_session) {
    turbo_free_json(&publisher_session);
    publisher_session = NULL;
  }

  publisher_session = build_session_root(sfu_server, "room-media", "sess-alice");
  subscriber_session = build_session_root(sfu_server, "room-media", "sess-bob");
  TEST_ASSERT_NOT_NULL(publisher_session);
  TEST_ASSERT_NOT_NULL(subscriber_session);

  TEST_ASSERT_TRUE(sender_started);
  TEST_ASSERT_NOT_NULL(subscriber_state.remote_track);
  TEST_ASSERT_GREATER_OR_EQUAL(1, sent_frames);
  TEST_ASSERT_GREATER_OR_EQUAL(1, subscriber_state.frame_state.frame_count);
  TEST_ASSERT_GREATER_OR_EQUAL(1, json_int_value(publisher_session, "remote_frame_count", 0));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(subscriber_session, "relay_track_count", -1));
  TEST_ASSERT_GREATER_OR_EQUAL(90000, (int)subscriber_state.frame_state.last_timestamp);

  turbo_free_json(&publisher_session);
  turbo_free_json(&subscriber_session);
  free(subscriber_answer);
  free(publisher_answer);
  free_media_peer_candidates(&subscriber_state);
  free_media_peer_candidates(&publisher_state);
  turbo_peer_connection_destroy(subscriber);
  turbo_peer_connection_destroy(publisher);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_recording_archives_publisher_rtp(void) {
  sfu_node_app_config_t sfu_config;
  sfu_node_app_server_t *sfu_server = NULL;
  turbo_sfu_node_t *node = NULL;
  turbo_peer_connection_t *publisher = NULL;
  turbo_media_track_t *publisher_track = NULL;
  turbo_peer_config_t peer_config;
  turbo_peer_callbacks_t publisher_callbacks;
  media_peer_state_t publisher_state;
  turbo_media_context_t *publisher_media = NULL;
  turbo_media_track_config_t track_config;
  json_value_t *recording_status = NULL;
  char *publisher_answer = NULL;
  char *status_json = NULL;
  char *output_path_copy = NULL;
  char publisher_offer[16384];
  uint8_t frame[160 * 120 + (160 * 120) / 2];
  uint32_t publisher_ssrc;
  uint64_t deadline_ms;
  const char *output_path;
  long output_size = -1;
  int sender_started = 0;
  int sent_frames = 0;

  memset(&publisher_state, 0, sizeof(publisher_state));

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19415;
  sfu_config.node_id = "node-record-1";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  TEST_ASSERT_NOT_NULL(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  TEST_ASSERT_NOT_NULL(node);

  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_attach_room(node, "room-record", 4));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_add_session(node, "room-record", "alice",
                                                      "sess-alice", NULL));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-record", "alice", "sess-alice"));

  memset(&peer_config, 0, sizeof(peer_config));
  memset(&publisher_callbacks, 0, sizeof(publisher_callbacks));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  peer_config.user_data = &publisher_state;
  publisher_callbacks.on_state_change = media_peer_on_state_change;
  publisher_callbacks.on_ice_candidate = media_peer_on_ice_candidate;
  publisher = turbo_peer_connection_create(&peer_config, &publisher_callbacks);
  TEST_ASSERT_NOT_NULL(publisher);
  publisher_media = turbo_peer_connection_get_media_context(publisher);
  TEST_ASSERT_NOT_NULL(publisher_media);

  memset(&track_config, 0, sizeof(track_config));
  track_config.type = TURBO_MEDIA_TRACK_VIDEO;
  track_config.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
  track_config.codec = TURBO_CODEC_VP8;
  track_config.video.width = 160;
  track_config.video.height = 120;
  track_config.video.framerate = 30;
  track_config.video.bitrate = 500000;
  track_config.video.keyframe_interval = 30;
  publisher_track = turbo_peer_connection_add_track_ex(publisher, &track_config);
  TEST_ASSERT_NOT_NULL(publisher_track);
  publisher_ssrc = turbo_media_track_get_ssrc(publisher_track);
  TEST_ASSERT_TRUE(publisher_ssrc != 0);

  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_register_published_track(
                               node, "room-record", "alice", "track-cam", publisher_ssrc, NULL, 0));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_register_published_track(
                               sfu_server, "room-record", "alice", "track-cam", publisher_ssrc,
                               NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp8"));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start_recording(
                               sfu_server, "room-record", "rec-001", "archive"));

  TEST_ASSERT_GREATER_THAN(0, turbo_peer_connection_create_offer(publisher, publisher_offer,
                                                                 sizeof(publisher_offer)));
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_set_remote_offer(
                               sfu_server, "room-record", "sess-alice", publisher_offer));
  publisher_answer = copy_session_answer(sfu_server, "room-record", "sess-alice");
  TEST_ASSERT_NOT_NULL(publisher_answer);
  TEST_ASSERT_EQUAL_INT(0, turbo_peer_connection_set_remote_description(
                               publisher, "answer", publisher_answer));

  deadline_ms = app_test_now_ms() + 12000ULL;
  while (app_test_now_ms() < deadline_ms) {
    sfu_node_app_server_poll_webrtc(sfu_server);
    turbo_peer_connection_poll(publisher);
    turbo_media_handle_timers(publisher_media);
    sfu_node_app_server_poll_webrtc(sfu_server);

    sync_peer_candidates_to_sfu(sfu_server, "room-record", "sess-alice", &publisher_state);
    sync_sfu_candidates_to_peer(sfu_server, "room-record", "sess-alice", publisher,
                                &publisher_state);

    if (!sender_started && publisher_state.connected) {
      TEST_ASSERT_EQUAL_INT(0, turbo_media_track_start(publisher_track));
      sender_started = 1;
    }

    if (sender_started && sent_frames < 16) {
      fill_video_i420_frame(frame, 160, 120, sent_frames);
      TEST_ASSERT_EQUAL_INT(0, turbo_media_track_send_frame(
                                   publisher_track, frame, sizeof(frame),
                                   90000ULL + (uint64_t)sent_frames * 3000ULL));
      sent_frames++;
    }

    if (sent_frames >= 16) {
      break;
    }

    app_test_sleep_ms(5);
  }

  TEST_ASSERT_TRUE(sender_started);
  TEST_ASSERT_GREATER_OR_EQUAL(16, sent_frames);
  app_test_sleep_ms(100);

  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_stop_recording(sfu_server, "room-record"));
  status_json = sfu_node_app_server_build_recording_status_json(sfu_server, "room-record");
  TEST_ASSERT_NOT_NULL(status_json);
  TEST_ASSERT_EQUAL_INT(0, parse_json_text(status_json, &recording_status));
  output_path = json_string_value(recording_status, "output_path");
  TEST_ASSERT_NOT_NULL(output_path);
  output_path_copy = app_strdup(output_path);
  TEST_ASSERT_NOT_NULL(output_path_copy);
  TEST_ASSERT_FALSE(json_bool_value(recording_status, "active", 1));
  TEST_ASSERT_GREATER_OR_EQUAL(1, json_int_value(recording_status, "track_count", 0));
  TEST_ASSERT_GREATER_OR_EQUAL(1, json_int_value(recording_status, "packet_count", 0));
  TEST_ASSERT_GREATER_OR_EQUAL(1, json_int_value(recording_status, "total_bytes", 0));

  output_size = file_size_bytes(output_path_copy);
  TEST_ASSERT_GREATER_OR_EQUAL(1, (int)output_size);

  turbo_free_json(&recording_status);
  free(status_json);
  free(publisher_answer);
  free_media_peer_candidates(&publisher_state);
  turbo_peer_connection_destroy(publisher);
  sfu_node_app_server_destroy(sfu_server);
  if (output_path_copy) {
    remove(output_path_copy);
  }
  free(output_path_copy);
}

spec("test_sfu_node_app") {
  TT_TEST(test_sfu_node_webrtc_session_accepts_offer_and_generates_answer);
  TT_TEST(test_sfu_node_webrtc_session_provisions_relay_track_before_answer);
  TT_TEST(test_sfu_node_webrtc_session_provisions_multiple_publishers_to_one_subscriber);
  TT_TEST(test_sfu_node_http_roundtrips_track_subscription_metadata);
  TT_TEST(test_sfu_node_config_reads_control_token_from_env);
  TT_TEST(test_sfu_node_http_control_token_protects_modifying_commands);
  TT_TEST(test_sfu_node_ready_metrics_and_drain_control);
  TT_TEST(test_sfu_node_webrtc_session_http_commands_roundtrip_offer_and_query_session);
  TT_TEST(test_sfu_node_media_bridge_forwards_video_to_subscriber);
  TT_TEST(test_sfu_node_recording_archives_publisher_rtp);
}
