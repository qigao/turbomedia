#include "tinytest.h"
#include "sfu_node/config.h"
#include "sfu_node/http_api.h"
#include "sfu_node/server.h"
#include "ivr_http_media_client.h"
#include "turbo_media_auth.h"
#include "turbo_recorder_internal.h"
#include <turbo_crypto.h>
#include "turbo_demuxer.h"
#include <json_parser.h>
#include "turbo_peer_connection.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define SFU_NODE_HTTP_LIFECYCLE_STRESS_ITERATIONS 32

#ifndef SFU_NODE_TEST_TLS_CERT_PATH
#error "SFU_NODE_TEST_TLS_CERT_PATH must identify the test certificate"
#endif

#ifndef SFU_NODE_TEST_TLS_KEY_PATH
#error "SFU_NODE_TEST_TLS_KEY_PATH must identify the test private key"
#endif

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
#define SFU_NODE_TEST_WHIP_CONNECT_TIMEOUT_MS 15000ULL
#define SFU_NODE_TEST_MEDIA_REQUEST_TIMEOUT_MS 15000ULL
#define SFU_NODE_TEST_ICE_POLL_INTERVAL_MS 1U

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

void test_sfu_node_rejects_identifiers_that_do_not_fit_storage(void) {
  turbo_sfu_node_config_t config;
  turbo_sfu_node_t *node = NULL;
  char valid_room_id[TURBO_ROOM_ID_MAX];
  char long_id[TURBO_ROOM_ID_MAX + 1];

  memset(valid_room_id, 'r', sizeof(valid_room_id) - 1);
  valid_room_id[sizeof(valid_room_id) - 1] = '\0';
  memset(long_id, 'x', sizeof(long_id) - 1);
  long_id[sizeof(long_id) - 1] = '\0';

  memset(&config, 0, sizeof(config));
  config.node_id = long_id;
  check_null(turbo_sfu_node_create(&config));

  config.node_id = "node-bounds";
  node = turbo_sfu_node_create(&config);
  check_not_null(node);
  check_equal((int)(turbo_sfu_node_attach_room(node, long_id, 4)), (int)(-1));
  check_equal((int)(turbo_sfu_node_attach_room(node, valid_room_id, 4)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(
                                node, valid_room_id, long_id, "session-1", NULL)), (int)(-1));
  check_equal((int)(turbo_sfu_node_add_session(
                               node, valid_room_id, "alice", "session-alice", NULL)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(
                               node, valid_room_id, "bob", "session-bob", NULL)), (int)(0));
  check_equal((int)(turbo_sfu_node_register_published_track(
                                node, valid_room_id, "alice", long_id,
                                0x10203040u, NULL, 0)), (int)(-1));
  check_equal((int)(turbo_sfu_node_register_published_track(
                               node, valid_room_id, "alice", "track-cam",
                               0x10203040u, NULL, 0)), (int)(0));
  check_equal((int)(turbo_sfu_node_set_track_subscription(
                                node, valid_room_id, long_id, "track-cam", 1,
                                TURBO_ROOM_VIDEO_LAYER_LOW)), (int)(-1));

  turbo_sfu_node_destroy(node);
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

  value = json_object_get(obj, key);
  if (!value || json_type(value) != JSON_OBJECT) {
    return NULL;
  }

  return value;
}

static const char *json_string_value(const json_value_t *obj, const char *key) {
  json_value_t *value;

  if (!obj || !key) {
    return NULL;
  }

  value = json_object_get(obj, key);
  if (!value || json_type(value) != JSON_STRING) {
    return NULL;
  }

  return json_string(value);
}

static int json_bool_value(const json_value_t *obj, const char *key, int def) {
  json_value_t *value;

  if (!obj || !key) {
    return def;
  }

  value = json_object_get(obj, key);
  if (!value || json_type(value) != JSON_BOOL) {
    return def;
  }

  return json_bool(value) ? 1 : 0;
}

static int json_int_value(const json_value_t *obj, const char *key, int def) {
  json_value_t *value;

  if (!obj || !key) {
    return def;
  }

  value = json_object_get(obj, key);
  if (!value || json_type(value) != JSON_NUMBER) {
    return def;
  }

  return (int)json_number(value);
}

static size_t json_array_count(const json_value_t *obj, const char *key) {
  json_value_t *value;

  if (!obj || !key) {
    return 0;
  }

  value = json_object_get(obj, key);
  if (!value || json_type(value) != JSON_ARRAY) {
    return 0;
  }

  return json_array_size(value);
}

static const char *json_array_string_at(const json_value_t *obj, const char *key,
                                        size_t index) {
  json_value_t *array;
  json_value_t *item;

  if (!obj || !key) {
    return NULL;
  }

  array = json_object_get(obj, key);
  if (!array || json_type(array) != JSON_ARRAY ||
      index >= json_array_size(array)) {
    return NULL;
  }

  item = json_array_get(array, index);
  if (!item || json_type(item) != JSON_STRING) {
    return NULL;
  }

  return json_string(item);
}

static int parse_json_text(const char *json_text, json_value_t **out_root) {
  json_value_t *root = NULL;

  if (out_root) {
    *out_root = NULL;
  }
  if (!json_text || !out_root) {
    return -1;
  }

  if (((root = json_parse((const char *)((const uint8_t *)json_text), strlen(json_text))) ? 0 : -1) != 0 ||
      !root || json_type(root) != JSON_OBJECT) {
    json_free(root);
    root = NULL;
    return -1;
  }

  *out_root = root;
  return 0;
}

typedef struct sfu_test_http_response_s {
  int status_code;
  size_t body_len;
  char location[sizeof(((ivr_http_media_response_t *)0)->location)];
  char etag[sizeof(((ivr_http_media_response_t *)0)->etag)];
  char content_type[sizeof(((ivr_http_media_response_t *)0)->content_type)];
  char body[sizeof(((ivr_http_media_response_t *)0)->body)];
} sfu_test_http_response_t;

static void sfu_test_http_response_free(sfu_test_http_response_t *response) {
  free(response);
}

static char *sfu_test_http_response_get_header(
    const sfu_test_http_response_t *response, const char *name) {
  const char *value = NULL;
  if (!response || !name) {
    return NULL;
  }
  if (strcmp(name, "Location") == 0) {
    value = response->location;
  } else if (strcmp(name, "ETag") == 0) {
    value = response->etag;
  } else if (strcmp(name, "Content-Type") == 0) {
    value = response->content_type;
  }
  return value && value[0] ? app_strdup(value) : NULL;
}

static sfu_test_http_response_t *sfu_test_http_request(
    const char *base_url, const char *method, const char *path,
    const char *content_type, const char *bearer_token, const char *if_match,
    const char *body, size_t body_len, const char *ca_file,
    const char *server_name, uint64_t timeout_ms) {
  ivr_http_media_client_config_t config = IVR_HTTP_MEDIA_CLIENT_CONFIG_INIT;
  ivr_http_media_client_t *client = NULL;
  ivr_http_media_response_t response;
  sfu_test_http_response_t *copy;
  int status;

  if (!base_url || !method || !path ||
      (body && strlen(body) != body_len)) {
    return NULL;
  }
  config.base_url = base_url;
  config.media_token = bearer_token;
  config.ca_file = ca_file;
  config.server_name = server_name;
  config.timeout_ms = timeout_ms;
  config.allow_plaintext_loopback = strncmp(base_url, "http://", 7u) == 0;
  if (ivr_http_media_client_create(&config, &client) != 0) {
    return NULL;
  }
  status = ivr_http_media_request(client, method, path, content_type,
                                  if_match, body, &response);
  ivr_http_media_client_destroy(client);
  if (status != 0) {
    return NULL;
  }
  copy = (sfu_test_http_response_t *)calloc(1u, sizeof(*copy));
  if (!copy) {
    return NULL;
  }
  copy->status_code = response.status;
  copy->body_len = strlen(response.body);
  memcpy(copy->location, response.location, sizeof(copy->location));
  memcpy(copy->etag, response.etag, sizeof(copy->etag));
  memcpy(copy->content_type, response.content_type,
         sizeof(copy->content_type));
  memcpy(copy->body, response.body, sizeof(copy->body));
  return copy;
}

static json_value_t *http_get_json(const char *base_url, const char *path) {
  sfu_test_http_response_t *response;
  json_value_t *root = NULL;

  if (!base_url || !path) {
    return NULL;
  }
  response = sfu_test_http_request(base_url, "GET", path, NULL, NULL, NULL,
                                   NULL, 0u, NULL, NULL, 3000u);
  if (!response ||
      response->status_code < 200 || response->status_code >= 300 ||
      !strstr(response->content_type, "application/json")) {
    sfu_test_http_response_free(response);
    return NULL;
  }
  root = json_parse(response->body, response->body_len);
  sfu_test_http_response_free(response);
  if (!root || json_type(root) != JSON_OBJECT) {
    json_free(root);
    root = NULL;
    return NULL;
  }

  return root;
}

static int http_get_status(const char *base_url, const char *path) {
  sfu_test_http_response_t *response;
  int status_code = 0;

  if (!base_url || !path) {
    return 0;
  }

  response = sfu_test_http_request(base_url, "GET", path, NULL, NULL, NULL,
                                   NULL, 0u, NULL, NULL, 3000u);
  if (response) {
    status_code = response->status_code;
    sfu_test_http_response_free(response);
  }

  return status_code;
}

static json_value_t *http_post_json_result(const char *base_url, const char *path,
                                           const char *body) {
  sfu_test_http_response_t *response;
  json_value_t *root = NULL;

  if (!base_url || !path || !body) {
    return NULL;
  }

  response = sfu_test_http_request(
      base_url, "POST", path, "application/json", NULL, NULL, body,
      strlen(body), NULL, NULL, 3000u);
  if (!response ||
      response->status_code < 200 || response->status_code >= 300 ||
      !strstr(response->content_type, "application/json")) {
    sfu_test_http_response_free(response);
    return NULL;
  }
  root = json_parse(response->body, response->body_len);
  sfu_test_http_response_free(response);
  if (!root || json_type(root) != JSON_OBJECT) {
    json_free(root);
    root = NULL;
    return NULL;
  }

  return root;
}

static int http_post_json_status_with_token(const char *base_url, const char *path,
                                            const char *body, const char *bearer_token) {
  sfu_test_http_response_t *response;
  int status_code = 0;

  if (!base_url || !path || !body) {
    return 0;
  }

  response = sfu_test_http_request(
      base_url, "POST", path, "application/json", bearer_token, NULL, body,
      strlen(body), NULL, NULL, 3000u);
  if (response) {
    status_code = response->status_code;
    sfu_test_http_response_free(response);
  }

  return status_code;
}

static json_value_t *http_post_json_result_with_token(const char *base_url,
                                                      const char *path,
                                                      const char *body,
                                                      const char *bearer_token) {
  sfu_test_http_response_t *response;
  json_value_t *root = NULL;

  if (!base_url || !path || !body) {
    return NULL;
  }

  response = sfu_test_http_request(
      base_url, "POST", path, "application/json", bearer_token, NULL, body,
      strlen(body), NULL, NULL, 3000u);
  if (!response ||
      response->status_code < 200 || response->status_code >= 300 ||
      !strstr(response->content_type, "application/json")) {
    sfu_test_http_response_free(response);
    return NULL;
  }
  root = json_parse(response->body, response->body_len);
  sfu_test_http_response_free(response);
  if (!root || json_type(root) != JSON_OBJECT) {
    json_free(root);
    root = NULL;
    return NULL;
  }

  return root;
}

static sfu_test_http_response_t *http_media_request(
    const char *base_url, const char *method, const char *path,
    const char *content_type, const char *bearer_token, const char *if_match,
    const char *body, size_t body_len) {
  return sfu_test_http_request(base_url, method, path, content_type,
                               bearer_token, if_match, body, body_len, NULL,
                               NULL, SFU_NODE_TEST_MEDIA_REQUEST_TIMEOUT_MS);
}

static int copy_sdp_attribute_value(const char *sdp, const char *prefix,
                                    char *output, size_t capacity) {
  const char *start;
  const char *end;
  size_t length;

  if (!sdp || !prefix || !output || capacity == 0) {
    return -1;
  }
  start = strstr(sdp, prefix);
  if (!start) {
    return -1;
  }
  start += strlen(prefix);
  end = strstr(start, "\r\n");
  if (!end) {
    end = strchr(start, '\n');
  }
  if (!end) {
    end = start + strlen(start);
  }
  length = (size_t)(end - start);
  if (length == 0 || length >= capacity) {
    return -1;
  }
  memcpy(output, start, length);
  output[length] = '\0';
  return 0;
}

static int wait_for_http_status_ok(const char *base_url, const char *path, int retries,
                                   int delay_ms) {
  for (int i = 0; i < retries; ++i) {
    json_value_t *root = http_get_json(base_url, path);
    if (root) {
      json_free(root);
      root = NULL;
      return 0;
    }
    app_test_sleep_ms((unsigned int)delay_ms);
  }

  return -1;
}

static int https_get_status(const char *base_url, const char *path,
                            const char *ca_file) {
  sfu_test_http_response_t *response;
  int status = 0;

  response = sfu_test_http_request(base_url, "GET", path, NULL, NULL, NULL,
                                   NULL, 0u, ca_file, "localhost", 3000u);
  if (response) {
    status = response->status_code;
  }
  sfu_test_http_response_free(response);
  return status;
}

static int https_post_status_with_token(
    const char *base_url, const char *path, const char *content_type,
    const char *bearer_token, const char *body) {
  sfu_test_http_response_t *response;
  int status = 0;

  if (!base_url || !path || !body) {
    return 0;
  }
  response = sfu_test_http_request(
      base_url, "POST", path, content_type, bearer_token, NULL,
      body, strlen(body), SFU_NODE_TEST_TLS_CERT_PATH, "localhost", 3000u);
  if (response) {
    status = response->status_code;
  }
  sfu_test_http_response_free(response);
  return status;
}

static void token_sha256_hex(const char *token, char output[65]) {
  static const char hex[] = "0123456789abcdef";
  uint8_t digest[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];

  check_not_null(token);
  check_equal(turbo_crypto_sha256(token, strlen(token), digest),
              TURBO_CRYPTO_OK);
  for (size_t index = 0U;
       index < TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES; ++index) {
    output[index * 2U] = hex[digest[index] >> 4U];
    output[index * 2U + 1U] = hex[digest[index] & 0x0fU];
  }
  output[64] = '\0';
  memset(digest, 0, sizeof(digest));
}

static int wait_for_https_status_ok(const char *base_url, const char *path,
                                    const char *ca_file, int retries,
                                    int delay_ms) {
  for (int i = 0; i < retries; ++i) {
    if (https_get_status(base_url, path, ca_file) == 200) {
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
  json_free(root);
  root = NULL;
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
      check_equal((int)(sfu_node_app_server_add_remote_ice_candidate(
                                   server, room_id, session_id, candidate)), (int)(0));
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
      check_equal((int)(turbo_peer_connection_add_ice_candidate(peer_pc, candidate)), (int)(0));
    }
    peer_state->sfu_candidates_applied_to_peer++;
  }

  json_free(root);

  root = NULL;
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
  turbo_sfu_node_room_stats_t room_stats;

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19411;
  sfu_config.node_id = "node-eu-1";
  sfu_config.ice_allow_loopback = 1;

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  check_not_null(node);

  check_equal((int)(turbo_sfu_node_attach_room(node, "room-webrtc", 4)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-webrtc", "alice",
                                                      "sess-alice", NULL)), (int)(0));
  check_equal((int)(sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-webrtc", "alice", "sess-alice")), (int)(0));

  memset(&offerer_state, 0, sizeof(offerer_state));
  memset(&peer_config, 0, sizeof(peer_config));
  memset(&peer_callbacks, 0, sizeof(peer_callbacks));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  peer_config.user_data = &offerer_state;
  peer_callbacks.on_state_change = offerer_on_state_change;
  peer_callbacks.on_ice_candidate = offerer_on_ice_candidate;

  offerer = turbo_peer_connection_create(&peer_config, &peer_callbacks);
  check_not_null(offerer);

  {
    turbo_media_track_config_t send_config;

    memset(&send_config, 0, sizeof(send_config));
    send_config.type = TURBO_RTC_MEDIA_TRACK_VIDEO;
    send_config.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
    send_config.codec = TURBO_CODEC_VP8;
    send_config.video.width = 160;
    send_config.video.height = 120;
    send_config.video.framerate = 30;
    send_config.video.bitrate = 500000;
    send_config.video.keyframe_interval = 30;
    send_track = turbo_peer_connection_add_track_ex(offerer, &send_config);
  }
  check_not_null(send_track);

  check_greater(turbo_peer_connection_create_offer(offerer, offer_sdp,
                                                                 sizeof(offer_sdp)), 0);
  check_equal((int)(sfu_node_app_server_set_remote_offer(
                               sfu_server, "room-webrtc", "sess-alice", offer_sdp)), (int)(0));
  session_json = sfu_node_app_server_build_webrtc_session_json(
      sfu_server, "room-webrtc", "sess-alice");
  check_not_null(session_json);
  check_equal((int)(parse_json_text(session_json, &root)), (int)(0));
  free(session_json);
  session_json = NULL;

  session = root;
  check_equal(json_string_value(session, "state"), "connecting");
  answer_sdp = json_string_value(session, "local_answer");
  check_not_null(answer_sdp);
  check_not_null(strstr(answer_sdp, "m=video"));
  check_not_null(strstr(answer_sdp, "a=recvonly"));
  check_equal((int)(json_bool_value(session, "remote_description_set", 0)), (int)(1));
  check_equal((int)(json_int_value(session, "remote_track_count", 0)), (int)(1));
  memset(&room_stats, 0, sizeof(room_stats));
  check_equal((int)(turbo_sfu_node_get_room_stats(node, "room-webrtc", &room_stats)), (int)(0));
  check_equal((int)(room_stats.published_track_count), (int)(1));
  check_true(json_int_value(session, "local_candidate_count", -1) >= 0);
  check_equal((int)(turbo_peer_connection_set_remote_description(
                               offerer, "answer", answer_sdp)), (int)(0));
  json_free(root);
  root = NULL;
  root = NULL;

  check_equal((int)(sfu_node_app_server_remove_webrtc_session(
             sfu_server, "room-webrtc", "sess-alice")), (int)(0));
  memset(&room_stats, 0, sizeof(room_stats));
  check_equal((int)(turbo_sfu_node_get_room_stats(node, "room-webrtc", &room_stats)), (int)(0));
  check_equal((int)(room_stats.published_track_count), (int)(0));

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
  sfu_config.ice_allow_loopback = 1;

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  check_not_null(node);

  check_equal((int)(turbo_sfu_node_attach_room(node, "room-relay", 4)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-relay", "alice",
                                                      "sess-alice", NULL)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-relay", "bob",
                                                      "sess-bob", NULL)), (int)(0));
  check_equal((int)(sfu_node_app_server_register_published_track(
                               sfu_server, "room-relay", "alice", "track-cam",
                               0x11223344u, NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp8")), (int)(0));
  check_equal((int)(sfu_node_app_server_set_track_subscription(
                               sfu_server, "room-relay", "bob", "track-cam", 1,
                               TURBO_ROOM_VIDEO_LAYER_LOW)), (int)(0));
  check_equal((int)(sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-relay", "bob", "sess-bob")), (int)(0));

  memset(&peer_config, 0, sizeof(peer_config));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  subscriber = turbo_peer_connection_create(&peer_config, NULL);
  check_not_null(subscriber);

  {
    turbo_media_track_config_t recv_config;

    memset(&recv_config, 0, sizeof(recv_config));
    recv_config.type = TURBO_RTC_MEDIA_TRACK_VIDEO;
    recv_config.direction = TURBO_MEDIA_DIRECTION_RECVONLY;
    recv_config.codec = TURBO_CODEC_VP8;
    recv_config.video.width = 160;
    recv_config.video.height = 120;
    recv_config.video.framerate = 30;
    recv_config.video.bitrate = 500000;
    recv_config.video.keyframe_interval = 30;
    recv_track = turbo_peer_connection_add_track_ex(subscriber, &recv_config);
  }
  check_not_null(recv_track);

  check_greater(turbo_peer_connection_create_offer(subscriber, offer_sdp,
                                                                 sizeof(offer_sdp)), 0);
  check_equal((int)(sfu_node_app_server_set_remote_offer(
                               sfu_server, "room-relay", "sess-bob", offer_sdp)), (int)(0));

  session_json = sfu_node_app_server_build_webrtc_session_json(
      sfu_server, "room-relay", "sess-bob");
  check_not_null(session_json);
  check_equal((int)(parse_json_text(session_json, &root)), (int)(0));
  free(session_json);
  session_json = NULL;

  check_equal((int)(json_int_value(root, "relay_track_count", -1)), (int)(1));
  answer_sdp = json_string_value(root, "local_answer");
  check_not_null(answer_sdp);
  check_not_null(strstr(answer_sdp, "m=video"));
  check_not_null(strstr(answer_sdp, "a=sendonly"));

  json_free(root);

  root = NULL;
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
  sfu_config.ice_allow_loopback = 1;

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  check_not_null(node);

  check_equal((int)(turbo_sfu_node_attach_room(node, "room-relay-many", 4)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-relay-many", "alice",
                                                      "sess-alice", NULL)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-relay-many", "carol",
                                                      "sess-carol", NULL)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-relay-many", "bob",
                                                      "sess-bob", NULL)), (int)(0));
  check_equal((int)(sfu_node_app_server_register_published_track(
                               sfu_server, "room-relay-many", "alice", "track-alice-cam",
                               0x11223344u, NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp8")), (int)(0));
  check_equal((int)(sfu_node_app_server_register_published_track(
                               sfu_server, "room-relay-many", "carol", "track-carol-cam",
                               0x55667788u, NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp8")), (int)(0));
  check_equal((int)(sfu_node_app_server_set_track_subscription(
                               sfu_server, "room-relay-many", "bob", "track-alice-cam", 1,
                               TURBO_ROOM_VIDEO_LAYER_LOW)), (int)(0));
  check_equal((int)(sfu_node_app_server_set_track_subscription(
                               sfu_server, "room-relay-many", "bob", "track-carol-cam", 1,
                               TURBO_ROOM_VIDEO_LAYER_LOW)), (int)(0));
  check_equal((int)(sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-relay-many", "bob", "sess-bob")), (int)(0));

  check_equal((int)(sfu_node_app_server_set_remote_offer(
                               sfu_server, "room-relay-many", "sess-bob", offer_sdp)), (int)(0));

  session_json = sfu_node_app_server_build_webrtc_session_json(
      sfu_server, "room-relay-many", "sess-bob");
  check_not_null(session_json);
  check_equal((int)(parse_json_text(session_json, &root)), (int)(0));
  free(session_json);
  session_json = NULL;

  check_equal((int)(json_int_value(root, "relay_track_count", -1)), (int)(2));
  answer_sdp = json_string_value(root, "local_answer");
  check_not_null(answer_sdp);
  check_equal((int)(count_substring_occurrences(answer_sdp, "m=video")), (int)(2));
  check_equal((int)(count_substring_occurrences(answer_sdp, "a=sendonly")), (int)(2));

  json_free(root);

  root = NULL;
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
  sfu_config.ice_allow_loopback = 1;

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  check_not_null(node);

  check_equal((int)(turbo_sfu_node_attach_room(node, "room-subscription-http", 4)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-subscription-http", "alice",
                                                      "sess-alice", NULL)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-subscription-http", "bob",
                                                      "sess-bob", NULL)), (int)(0));
  check_equal((int)(turbo_sfu_node_register_published_track(
                               node, "room-subscription-http", "alice", "track-cam",
                               0x22113344u, NULL, 0)), (int)(0));
  check_equal((int)(sfu_node_app_server_register_published_track(
                               sfu_server, "room-subscription-http", "alice", "track-cam",
                               0x22113344u, NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp9")), (int)(0));

  http_api = sfu_node_http_api_create(sfu_server);
  check_not_null(http_api);
  check_equal((int)(sfu_node_http_api_start(http_api, sfu_config.bind_host,
                                                   sfu_config.bind_port)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100)), (int)(0));

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
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_track_subscription\","
      "\"room_id\":\"room-subscription-http\","
      "\"receiver_participant_id\":\"bob\","
      "\"track_id\":\"track-cam\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "track_subscription");
  check_not_null(subscription);
  check_equal(json_string_value(subscription, "receiver_participant_id"), "bob");
  check_equal(json_string_value(subscription, "sender_participant_id"), "alice");
  check_true(json_bool_value(subscription, "enabled", 0));
  check_equal((int)(json_int_value(subscription, "priority", -1)), (int)(275));
  check_equal(json_string_value(subscription, "preferred_layer"), "high");
  check_equal(json_string_value(subscription, "target_layer"), "medium");
  check_equal((int)(json_bool_value(subscription, "muted", 1)), (int)(0));
  check_equal(json_string_value(subscription, "policy_source"), "conference_policy_pin");
  check_equal(json_string_value(subscription, "max_layer"), "medium");
  json_free(root);
  root = NULL;

  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_https_uses_explicit_identity_and_verified_client(void) {
  sfu_node_app_config_t config;
  sfu_node_app_server_t *server = NULL;
  sfu_node_http_api_t *http_api = NULL;
  const char *base_url = "https://127.0.0.1:19430";

  sfu_node_app_config_init(&config);
  config.bind_host = "127.0.0.1";
  config.bind_port = 19430;
  config.node_id = "sfu-node-tls";
  config.use_tls = 1;
  config.tls_cert_file = SFU_NODE_TEST_TLS_CERT_PATH;
  config.tls_key_file = SFU_NODE_TEST_TLS_KEY_PATH;
  config.ice_allow_loopback = 1;

  server = sfu_node_app_server_create(&config);
  check_not_null(server);
  http_api = sfu_node_http_api_create(server);
  check_not_null(http_api);
  check_equal((int)(sfu_node_http_api_start(http_api, config.bind_host, config.bind_port)), (int)(0));
  check_equal((int)(wait_for_https_status_ok(base_url, "/health",
                                  SFU_NODE_TEST_TLS_CERT_PATH, 30, 100)), (int)(0));
  check_equal((int)(https_get_status(base_url, "/health", NULL)), (int)(0));

  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(server);
}

void test_sfu_node_dynamic_revocation_controls_control_and_media_auth(void) {
  sfu_node_app_config_t config;
  sfu_node_app_server_t *server = NULL;
  sfu_node_http_api_t *http_api = NULL;
  turbo_media_auth_config_t issuer;
  turbo_media_auth_claims_t claims;
  char *control_token = NULL;
  char *media_token = NULL;
  char *security_token = NULL;
  char control_digest[65];
  char media_digest[65];
  char revoke_control[256];
  char revoke_media[256];
  int length;
  int status;
  int64_t now = (int64_t)time(NULL);
  const char *base_url = "https://127.0.0.1:19431";
  const char *attach_room =
      "{\"type\":\"attach_room\",\"room_id\":\"room-dynamic\","
      "\"max_participants\":4}";
  const char *snapshot =
      "{\"schema_version\":1,\"epoch\":1,\"sequence\":0,"
      "\"revoked_sha256\":\"\"}";
  const char *sdp_probe = "v=0\r\n";

  sfu_node_app_config_init(&config);
  config.bind_host = "127.0.0.1";
  config.bind_port = 19431;
  config.node_id = "sfu-dynamic-revocation";
  config.use_tls = 1;
  config.tls_cert_file = SFU_NODE_TEST_TLS_CERT_PATH;
  config.tls_key_file = SFU_NODE_TEST_TLS_KEY_PATH;
  config.media_access_token = "static-media-token";
  config.auth_issuer = "turbomedia";
  config.auth_active_key_id = "sfu-security-2026-09";
  config.auth_active_secret =
      "sfu-security-active-secret-at-least-32-bytes";
  config.auth_dynamic_revocation_capacity = 4;
  config.auth_max_ttl_seconds = 300;
  config.ice_allow_loopback = 1;

  issuer = (turbo_media_auth_config_t){
      .issuer = config.auth_issuer,
      .active_key_id = config.auth_active_key_id,
      .active_secret = config.auth_active_secret,
      .clock_skew_seconds = config.auth_clock_skew_seconds,
      .max_ttl_seconds = config.auth_max_ttl_seconds};

  claims = (turbo_media_auth_claims_t){
      .subject = "room-service",
      .audience = "turbomedia-sfu-control",
      .scope = "sfu.control.write",
      .room_id = "room-dynamic",
      .issued_at = now,
      .expires_at = now + 120};
  control_token = turbo_media_auth_issue(&issuer, &claims);
  check_not_null(control_token);

  claims.subject = "media-client";
  claims.audience = "turbomedia-sfu-media";
  claims.scope = "sfu.media.publish";
  claims.room_id = "room-media-missing";
  claims.participant_id = "alice";
  media_token = turbo_media_auth_issue(&issuer, &claims);
  check_not_null(media_token);

  claims.subject = "security-control";
  claims.audience = "turbomedia-security-control";
  claims.scope = "security.revocation.write";
  claims.room_id = NULL;
  claims.participant_id = NULL;
  security_token = turbo_media_auth_issue(&issuer, &claims);
  check_not_null(security_token);

  token_sha256_hex(control_token, control_digest);
  token_sha256_hex(media_token, media_digest);

  server = sfu_node_app_server_create(&config);
  check_not_null(server);
  http_api = sfu_node_http_api_create(server);
  check_not_null(http_api);
  check_equal(sfu_node_http_api_start(
                  http_api, config.bind_host, config.bind_port),
              0);
  check_equal(wait_for_https_status_ok(
                  base_url, "/health", SFU_NODE_TEST_TLS_CERT_PATH,
                  30, 100),
              0);

  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/commands", "application/json",
                  control_token, attach_room),
              401);
  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/commands", "application/json",
                  "static-control-token", attach_room),
              401);
  check_equal(https_post_status_with_token(
                  base_url, "/whip/room-media-missing/alice",
                  "application/sdp", media_token, sdp_probe),
              401);
  check_equal(https_post_status_with_token(
                  base_url, "/whip/room-media-missing/alice",
                  "application/sdp", "static-media-token", sdp_probe),
              401);

  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/revocations/snapshot",
                  "application/json", "static-control-token", snapshot),
              401);
  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/revocations/snapshot",
                  "application/json", control_token, snapshot),
              401);
  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/revocations/snapshot",
                  "application/json", security_token, snapshot),
              200);

  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/commands", "application/json",
                  control_token, attach_room),
              200);
  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/commands", "application/json",
                  "static-control-token", attach_room),
              401);
  status = https_post_status_with_token(
      base_url, "/whip/room-media-missing/alice",
      "application/sdp", media_token, sdp_probe);
  check_equal(status, 404);
  check_equal(https_post_status_with_token(
                  base_url, "/whip/room-media-missing/alice",
                  "application/sdp", "static-media-token", sdp_probe),
              401);

  length = snprintf(
      revoke_control, sizeof(revoke_control),
      "{\"schema_version\":1,\"epoch\":1,\"sequence\":1,"
      "\"sha256\":\"%s\"}",
      control_digest);
  check_true(length > 0 && (size_t)length < sizeof(revoke_control));
  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/revocations/revoke",
                  "application/json", security_token, revoke_control),
              200);
  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/commands", "application/json",
                  control_token, attach_room),
              401);
  check_equal(https_post_status_with_token(
                  base_url, "/whip/room-media-missing/alice",
                  "application/sdp", media_token, sdp_probe),
              404);

  length = snprintf(
      revoke_media, sizeof(revoke_media),
      "{\"schema_version\":1,\"epoch\":1,\"sequence\":2,"
      "\"sha256\":\"%s\"}",
      media_digest);
  check_true(length > 0 && (size_t)length < sizeof(revoke_media));
  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/revocations/revoke",
                  "application/json", security_token, revoke_media),
              200);
  check_equal(https_post_status_with_token(
                  base_url, "/whip/room-media-missing/alice",
                  "application/sdp", media_token, sdp_probe),
              401);

  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(server);
  free(security_token);
  free(media_token);
  free(control_token);
}

void test_sfu_node_tenant_quota_transport_requires_dedicated_signed_control_token(void) {
  sfu_node_app_config_t config;
  sfu_node_app_server_t *server = NULL;
  sfu_node_http_api_t *http_api = NULL;
  turbo_media_auth_config_t issuer;
  turbo_media_auth_claims_t claims;
  char *quota_token = NULL;
  char *wrong_scope_token = NULL;
  int synchronized = 0;
  uint64_t epoch = 0U;
  uint64_t sequence = 0U;
  size_t count = 0U;
  int64_t now = (int64_t)time(NULL);
  const char *base_url = "https://127.0.0.1:19432";
  static const char snapshot[] =
      "{\"schema_version\":1,\"epoch\":1,\"sequence\":0,"
      "\"node_id\":\"sfu-tenant-quota\",\"leases\":[{"
      "\"tenant_id\":\"tenant-a\",\"expires_at_unix_ms\":9999999999999,"
      "\"limits\":{\"signaling_connections\":0,\"rooms\":2,"
      "\"participants\":8,\"media_sessions\":4,"
      "\"published_tracks\":8}}]}";
  static const char wrong_node[] =
      "{\"schema_version\":1,\"epoch\":1,\"sequence\":1,"
      "\"node_id\":\"other-node\",\"lease\":{"
      "\"tenant_id\":\"tenant-a\",\"expires_at_unix_ms\":9999999999999,"
      "\"limits\":{\"signaling_connections\":0,\"rooms\":3,"
      "\"participants\":8,\"media_sessions\":4,"
      "\"published_tracks\":8}}}";
  static const char update[] =
      "{\"schema_version\":1,\"epoch\":1,\"sequence\":1,"
      "\"node_id\":\"sfu-tenant-quota\",\"lease\":{"
      "\"tenant_id\":\"tenant-a\",\"expires_at_unix_ms\":9999999999999,"
      "\"limits\":{\"signaling_connections\":0,\"rooms\":3,"
      "\"participants\":8,\"media_sessions\":4,"
      "\"published_tracks\":8}}}";
  static const char gap[] =
      "{\"schema_version\":1,\"epoch\":1,\"sequence\":3,"
      "\"node_id\":\"sfu-tenant-quota\",\"lease\":{"
      "\"tenant_id\":\"tenant-a\",\"expires_at_unix_ms\":9999999999999,"
      "\"limits\":{\"signaling_connections\":0,\"rooms\":4,"
      "\"participants\":8,\"media_sessions\":4,"
      "\"published_tracks\":8}}}";
  static const char recover[] =
      "{\"schema_version\":1,\"epoch\":1,\"sequence\":3,"
      "\"node_id\":\"sfu-tenant-quota\",\"leases\":[{"
      "\"tenant_id\":\"tenant-a\",\"expires_at_unix_ms\":9999999999999,"
      "\"limits\":{\"signaling_connections\":0,\"rooms\":4,"
      "\"participants\":8,\"media_sessions\":4,"
      "\"published_tracks\":8}}]}";
  static const char semantic_invalid[] =
      "{\"schema_version\":1,\"epoch\":1,\"sequence\":4,"
      "\"node_id\":\"sfu-tenant-quota\",\"lease\":{"
      "\"tenant_id\":\"tenant/a\",\"expires_at_unix_ms\":9999999999999,"
      "\"limits\":{\"signaling_connections\":0,\"rooms\":4,"
      "\"participants\":8,\"media_sessions\":4,"
      "\"published_tracks\":8}}}";

  sfu_node_app_config_init(&config);
  config.bind_host = "127.0.0.1";
  config.bind_port = 19432;
  config.node_id = "sfu-tenant-quota";
  config.use_tls = 1;
  config.tls_cert_file = SFU_NODE_TEST_TLS_CERT_PATH;
  config.tls_key_file = SFU_NODE_TEST_TLS_KEY_PATH;
  config.control_token = "static-control-token";
  config.auth_issuer = "turbomedia";
  config.auth_active_key_id = "sfu-security-2026-09";
  config.auth_active_secret =
      "sfu-security-active-secret-at-least-32-bytes";
  config.auth_max_ttl_seconds = 300;
  config.tenant_quota_capacity = 4;
  config.ice_allow_loopback = 1;

  issuer = (turbo_media_auth_config_t){
      .issuer = config.auth_issuer,
      .active_key_id = config.auth_active_key_id,
      .active_secret = config.auth_active_secret,
      .clock_skew_seconds = config.auth_clock_skew_seconds,
      .max_ttl_seconds = config.auth_max_ttl_seconds};

  claims = (turbo_media_auth_claims_t){
      .subject = "security-control",
      .audience = "turbomedia-security-control",
      .scope = "security.tenant_quota.write",
      .issued_at = now,
      .expires_at = now + 120};
  quota_token = turbo_media_auth_issue(&issuer, &claims);
  check_not_null(quota_token);

  claims.scope = "security.revocation.write";
  wrong_scope_token = turbo_media_auth_issue(&issuer, &claims);
  check_not_null(wrong_scope_token);

  server = sfu_node_app_server_create(&config);
  check_not_null(server);
  http_api = sfu_node_http_api_create(server);
  check_not_null(http_api);
  check_equal(sfu_node_http_api_start(
                  http_api, config.bind_host, config.bind_port), 0);
  check_equal(wait_for_https_status_ok(
                  base_url, "/health", SFU_NODE_TEST_TLS_CERT_PATH,
                  30, 100), 0);

  check_equal(sfu_node_app_server_get_tenant_quota_status(
                  server, &synchronized, &epoch, &sequence, &count), 0);
  check_false(synchronized);

  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/tenant-quotas/snapshot",
                  "application/json", "static-control-token", snapshot), 401);
  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/tenant-quotas/snapshot",
                  "application/json", wrong_scope_token, snapshot), 401);
  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/tenant-quotas/snapshot",
                  "application/json", quota_token, snapshot), 200);
  check_equal(sfu_node_app_server_get_tenant_quota_status(
                  server, &synchronized, &epoch, &sequence, &count), 0);
  check_true(synchronized);
  check_equal((int)sequence, 0);
  check_equal((int)count, 1);

  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/tenant-quotas/update",
                  "application/json", quota_token, wrong_node), 400);
  check_equal(sfu_node_app_server_get_tenant_quota_status(
                  server, &synchronized, &epoch, &sequence, &count), 0);
  check_true(synchronized);
  check_equal((int)sequence, 0);

  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/tenant-quotas/update",
                  "application/json", quota_token, update), 200);
  check_equal(sfu_node_app_server_get_tenant_quota_status(
                  server, &synchronized, &epoch, &sequence, &count), 0);
  check_true(synchronized);
  check_equal((int)sequence, 1);

  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/tenant-quotas/update",
                  "application/json", quota_token, gap), 409);
  check_equal(sfu_node_app_server_get_tenant_quota_status(
                  server, &synchronized, &epoch, &sequence, &count), 0);
  check_false(synchronized);

  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/tenant-quotas/snapshot",
                  "application/json", quota_token, recover), 200);
  check_equal(sfu_node_app_server_get_tenant_quota_status(
                  server, &synchronized, &epoch, &sequence, &count), 0);
  check_true(synchronized);
  check_equal((int)sequence, 3);

  check_equal(https_post_status_with_token(
                  base_url, "/api/v1/security/tenant-quotas/update",
                  "application/json", quota_token, semantic_invalid), 400);
  check_equal(sfu_node_app_server_get_tenant_quota_status(
                  server, &synchronized, &epoch, &sequence, &count), 0);
  check_false(synchronized);
  check_equal((int)sequence, 3);

  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(server);
  free(wrong_scope_token);
  free(quota_token);
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
  sfu_config.ice_allow_loopback = 1;

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);

  http_api = sfu_node_http_api_create(sfu_server);
  check_not_null(http_api);
  check_equal((int)(sfu_node_http_api_start(http_api, sfu_config.bind_host,
                                                   sfu_config.bind_port)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100)), (int)(0));

  check_equal((int)(http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 get_node_stats_command, NULL)), (int)(200));
  check_equal((int)(http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 attach_room_command, NULL)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 attach_room_command, "wrong-token")), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 force_close_room_command, NULL)), (int)(401));

  root = http_post_json_result_with_token(sfu_node_base_url, "/api/v1/commands",
                                          attach_room_command, "test-control-token");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_signed_control_token_enforces_scope_room_expiry_and_rotation(void) {
  sfu_node_app_config_t sfu_config;
  sfu_node_app_server_t *sfu_server = NULL;
  sfu_node_http_api_t *http_api = NULL;
  turbo_media_auth_config_t current_auth;
  turbo_media_auth_config_t previous_issuer;
  turbo_media_auth_claims_t claims;
  char *room_write_token = NULL;
  char *dangerous_token = NULL;
  char *participant_dangerous_token = NULL;
  char *expired_token = NULL;
  char *previous_token = NULL;
  int64_t now = (int64_t)time(NULL);
  const char *base_url = "http://127.0.0.1:19423";
  const char *attach_room_a =
      "{\"type\":\"attach_room\",\"room_id\":\"room-signed-a\","
      "\"max_participants\":4}";
  const char *attach_room_b =
      "{\"type\":\"attach_room\",\"room_id\":\"room-signed-b\","
      "\"max_participants\":4}";
  const char *force_close_room_a =
      "{\"type\":\"force_close_room\",\"room_id\":\"room-signed-a\"}";
  const char *disconnect_participant_a =
      "{\"type\":\"disconnect_media_participant\","
      "\"room_id\":\"room-signed-a\",\"participant_id\":\"call-a\"}";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19423;
  sfu_config.node_id = "node-signed-auth-http-1";
  sfu_config.auth_issuer = "turbomedia-test";
  sfu_config.auth_active_key_id = "active-2026-07";
  sfu_config.auth_active_secret = "0123456789abcdef0123456789abcdef";
  sfu_config.auth_previous_key_id = "previous-2026-06";
  sfu_config.auth_previous_secret = "abcdef0123456789abcdef0123456789";
  sfu_config.auth_max_ttl_seconds = 300;
  sfu_config.ice_allow_loopback = 1;
  current_auth = (turbo_media_auth_config_t){
      .issuer = sfu_config.auth_issuer,
      .active_key_id = sfu_config.auth_active_key_id,
      .active_secret = sfu_config.auth_active_secret,
      .previous_key_id = sfu_config.auth_previous_key_id,
      .previous_secret = sfu_config.auth_previous_secret,
      .clock_skew_seconds = sfu_config.auth_clock_skew_seconds,
      .max_ttl_seconds = sfu_config.auth_max_ttl_seconds};
  claims = (turbo_media_auth_claims_t){
      .subject = "room-service",
      .audience = "turbomedia-sfu-control",
      .scope = "sfu.control.write",
      .room_id = "room-signed-a",
      .issued_at = now,
      .expires_at = now + 120};
  room_write_token = turbo_media_auth_issue(&current_auth, &claims);
  check_not_null(room_write_token);
  claims.scope = "sfu.control.dangerous";
  dangerous_token = turbo_media_auth_issue(&current_auth, &claims);
  check_not_null(dangerous_token);
  claims.participant_id = "call-a";
  participant_dangerous_token = turbo_media_auth_issue(&current_auth, &claims);
  check_not_null(participant_dangerous_token);
  claims.participant_id = NULL;
  claims.scope = "sfu.control.write";
  claims.issued_at = now - 120;
  claims.expires_at = now - 31;
  expired_token = turbo_media_auth_issue(&current_auth, &claims);
  check_not_null(expired_token);

  previous_issuer = current_auth;
  previous_issuer.active_key_id = current_auth.previous_key_id;
  previous_issuer.active_secret = current_auth.previous_secret;
  previous_issuer.previous_key_id = NULL;
  previous_issuer.previous_secret = NULL;
  claims.issued_at = now;
  claims.expires_at = now + 120;
  previous_token = turbo_media_auth_issue(&previous_issuer, &claims);
  check_not_null(previous_token);

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  http_api = sfu_node_http_api_create(sfu_server);
  check_not_null(http_api);
  check_equal((int)(sfu_node_http_api_start(
             http_api, sfu_config.bind_host, sfu_config.bind_port)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(base_url, "/health", 30, 100)), (int)(0));

  check_equal((int)(http_post_json_status_with_token(
               base_url, "/api/v1/commands", attach_room_a,
               room_write_token)), (int)(200));
  check_equal((int)(http_post_json_status_with_token(
               base_url, "/api/v1/commands", attach_room_b,
               room_write_token)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
               base_url, "/api/v1/commands", force_close_room_a,
               room_write_token)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
               base_url, "/api/v1/commands", disconnect_participant_a,
               room_write_token)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
               base_url, "/api/v1/commands", disconnect_participant_a,
               participant_dangerous_token)), (int)(400));
  check_equal((int)(http_post_json_status_with_token(
               base_url, "/api/v1/commands", attach_room_a,
               expired_token)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
               base_url, "/api/v1/commands", force_close_room_a,
               dangerous_token)), (int)(200));
  check_equal((int)(http_post_json_status_with_token(
               base_url, "/api/v1/commands", attach_room_a,
               previous_token)), (int)(200));

  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(sfu_server);
  free(participant_dangerous_token);
  free(previous_token);
  free(expired_token);
  free(dangerous_token);
  free(room_write_token);
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
  sfu_config.ice_allow_loopback = 1;

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);

  http_api = sfu_node_http_api_create(sfu_server);
  check_not_null(http_api);
  check_equal((int)(sfu_node_http_api_start(http_api, sfu_config.bind_host,
                                                   sfu_config.bind_port)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100)), (int)(0));
  check_equal((int)(http_get_status(sfu_node_base_url, "/ready")), (int)(200));
  check_equal((int)(http_get_status(sfu_node_base_url, "/metrics")), (int)(200));

  check_equal((int)(http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 set_drain_command, NULL)), (int)(401));

  root = http_post_json_result_with_token(sfu_node_base_url, "/api/v1/commands",
                                          set_drain_command, "test-control-token");
  check_not_null(root);
  node_stats = json_object_field(root, "node_stats");
  check_not_null(node_stats);
  check_true(json_bool_value(node_stats, "draining", 0));
  json_free(root);
  root = NULL;

  check_equal((int)(http_get_status(sfu_node_base_url, "/ready")), (int)(503));
  check_equal((int)(http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 attach_room_command, "test-control-token")), (int)(409));

  root = http_post_json_result_with_token(sfu_node_base_url, "/api/v1/commands",
                                          clear_drain_command, "test-control-token");
  check_not_null(root);
  node_stats = json_object_field(root, "node_stats");
  check_not_null(node_stats);
  check_false(json_bool_value(node_stats, "draining", 1));
  json_free(root);
  root = NULL;

  check_equal((int)(http_get_status(sfu_node_base_url, "/ready")), (int)(200));
  check_equal((int)(http_post_json_status_with_token(
                                 sfu_node_base_url, "/api/v1/commands",
                                 attach_room_command, "test-control-token")), (int)(200));

  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_config_reads_security_and_ice_from_env(void) {
  sfu_node_app_config_t sfu_config;
  char *saved_control = app_test_save_env("TURBO_SFU_NODE_CONTROL_TOKEN");
  char *saved_media = app_test_save_env("TURBO_SFU_MEDIA_ACCESS_TOKEN");
  char *saved_stun = app_test_save_env("TURBO_SFU_STUN_SERVER");
  char *saved_turn = app_test_save_env("TURBO_SFU_TURN_SERVER");
  char *saved_use_tls = app_test_save_env("TURBO_SFU_USE_TLS");
  char *saved_tls_cert = app_test_save_env("TURBO_SFU_TLS_CERT_FILE");
  char *saved_tls_key = app_test_save_env("TURBO_SFU_TLS_KEY_FILE");
  char *saved_auth_issuer = app_test_save_env("TURBO_SFU_AUTH_ISSUER");
  char *saved_auth_key_id =
      app_test_save_env("TURBO_SFU_AUTH_ACTIVE_KEY_ID");
  char *saved_auth_secret =
      app_test_save_env("TURBO_SFU_AUTH_ACTIVE_SECRET");
  char *saved_auth_previous_key_id =
      app_test_save_env("TURBO_SFU_AUTH_PREVIOUS_KEY_ID");
  char *saved_auth_previous_secret =
      app_test_save_env("TURBO_SFU_AUTH_PREVIOUS_SECRET");
  char *saved_auth_skew =
      app_test_save_env("TURBO_SFU_AUTH_CLOCK_SKEW_SECONDS");
  char *saved_auth_ttl =
      app_test_save_env("TURBO_SFU_AUTH_MAX_TTL_SECONDS");
  char *saved_auth_dynamic =
      app_test_save_env("TURBO_SFU_AUTH_DYNAMIC_REVOCATION_CAPACITY");

  app_test_set_env("TURBO_SFU_NODE_CONTROL_TOKEN", "env-sfu-control-token");
  app_test_set_env("TURBO_SFU_MEDIA_ACCESS_TOKEN", "env-sfu-media-token");
  app_test_set_env("TURBO_SFU_STUN_SERVER", "stun:env.example:3478");
  app_test_set_env("TURBO_SFU_TURN_SERVER",
                   "turn:env-user:env-secret@env.example:3478");
  app_test_set_env("TURBO_SFU_USE_TLS", "true");
  app_test_set_env("TURBO_SFU_TLS_CERT_FILE", SFU_NODE_TEST_TLS_CERT_PATH);
  app_test_set_env("TURBO_SFU_TLS_KEY_FILE", SFU_NODE_TEST_TLS_KEY_PATH);
  app_test_set_env("TURBO_SFU_AUTH_ISSUER", "env-turbomedia");
  app_test_set_env("TURBO_SFU_AUTH_ACTIVE_KEY_ID", "env-active");
  app_test_set_env("TURBO_SFU_AUTH_ACTIVE_SECRET",
                   "0123456789abcdef0123456789abcdef");
  app_test_set_env("TURBO_SFU_AUTH_PREVIOUS_KEY_ID", "env-previous");
  app_test_set_env("TURBO_SFU_AUTH_PREVIOUS_SECRET",
                   "abcdef0123456789abcdef0123456789");
  app_test_set_env("TURBO_SFU_AUTH_CLOCK_SKEW_SECONDS", "17");
  app_test_set_env("TURBO_SFU_AUTH_MAX_TTL_SECONDS", "900");
  app_test_set_env("TURBO_SFU_AUTH_DYNAMIC_REVOCATION_CAPACITY", "32");
  sfu_node_app_config_init(&sfu_config);
  sfu_node_app_config_apply_environment(&sfu_config);
  check_equal(sfu_config.control_token, "env-sfu-control-token");
  check_equal(sfu_config.media_access_token, "env-sfu-media-token");
  check_equal((int)(sfu_config.stun_server_count), (int)(1));
  check_equal(sfu_config.stun_servers[0], "stun:env.example:3478");
  check_equal((int)(sfu_config.turn_server_count), (int)(1));
  check_equal(sfu_config.turn_servers[0], "turn:env-user:env-secret@env.example:3478");
  check_equal((int)(sfu_config.use_tls), (int)(1));
  check_equal(sfu_config.tls_cert_file, SFU_NODE_TEST_TLS_CERT_PATH);
  check_equal(sfu_config.tls_key_file, SFU_NODE_TEST_TLS_KEY_PATH);
  check_equal(sfu_config.auth_issuer, "env-turbomedia");
  check_equal(sfu_config.auth_active_key_id, "env-active");
  check_equal(sfu_config.auth_active_secret, "0123456789abcdef0123456789abcdef");
  check_equal(sfu_config.auth_previous_key_id, "env-previous");
  check_equal((int)(sfu_config.auth_clock_skew_seconds), (int)(17));
  check_equal((int)(sfu_config.auth_max_ttl_seconds), (int)(900));
  check_equal((int)(sfu_config.auth_dynamic_revocation_capacity), (int)(32));
  check_equal((int)(sfu_node_app_config_validate(&sfu_config)), (int)(0));

  app_test_restore_env("TURBO_SFU_NODE_CONTROL_TOKEN", saved_control);
  app_test_restore_env("TURBO_SFU_MEDIA_ACCESS_TOKEN", saved_media);
  app_test_restore_env("TURBO_SFU_STUN_SERVER", saved_stun);
  app_test_restore_env("TURBO_SFU_TURN_SERVER", saved_turn);
  app_test_restore_env("TURBO_SFU_USE_TLS", saved_use_tls);
  app_test_restore_env("TURBO_SFU_TLS_CERT_FILE", saved_tls_cert);
  app_test_restore_env("TURBO_SFU_TLS_KEY_FILE", saved_tls_key);
  app_test_restore_env("TURBO_SFU_AUTH_ISSUER", saved_auth_issuer);
  app_test_restore_env("TURBO_SFU_AUTH_ACTIVE_KEY_ID", saved_auth_key_id);
  app_test_restore_env("TURBO_SFU_AUTH_ACTIVE_SECRET", saved_auth_secret);
  app_test_restore_env("TURBO_SFU_AUTH_PREVIOUS_KEY_ID",
                       saved_auth_previous_key_id);
  app_test_restore_env("TURBO_SFU_AUTH_PREVIOUS_SECRET",
                       saved_auth_previous_secret);
  app_test_restore_env("TURBO_SFU_AUTH_CLOCK_SKEW_SECONDS",
                       saved_auth_skew);
  app_test_restore_env("TURBO_SFU_AUTH_MAX_TTL_SECONDS", saved_auth_ttl);
  app_test_restore_env("TURBO_SFU_AUTH_DYNAMIC_REVOCATION_CAPACITY",
                       saved_auth_dynamic);
}

void test_sfu_node_config_reads_signed_only_tenant_quota_from_env(void) {
  sfu_node_app_config_t config;
  char *saved_use_tls = app_test_save_env("TURBO_SFU_USE_TLS");
  char *saved_tls_cert = app_test_save_env("TURBO_SFU_TLS_CERT_FILE");
  char *saved_tls_key = app_test_save_env("TURBO_SFU_TLS_KEY_FILE");
  char *saved_auth_key =
      app_test_save_env("TURBO_SFU_AUTH_ACTIVE_KEY_ID");
  char *saved_auth_secret =
      app_test_save_env("TURBO_SFU_AUTH_ACTIVE_SECRET");
  char *saved_quota =
      app_test_save_env("TURBO_SFU_TENANT_QUOTA_CAPACITY");

  app_test_set_env("TURBO_SFU_USE_TLS", "true");
  app_test_set_env("TURBO_SFU_TLS_CERT_FILE", SFU_NODE_TEST_TLS_CERT_PATH);
  app_test_set_env("TURBO_SFU_TLS_KEY_FILE", SFU_NODE_TEST_TLS_KEY_PATH);
  app_test_set_env("TURBO_SFU_AUTH_ACTIVE_KEY_ID", "quota-active");
  app_test_set_env("TURBO_SFU_AUTH_ACTIVE_SECRET",
                   "quota-active-secret-at-least-32-bytes");
  app_test_set_env("TURBO_SFU_TENANT_QUOTA_CAPACITY", "16");

  sfu_node_app_config_init(&config);
  sfu_node_app_config_apply_environment(&config);
  check_equal((int)config.tenant_quota_capacity, 16);
  check_null(config.control_token);
  check_null(config.media_access_token);
  check_equal((int)sfu_node_app_config_validate(&config), 0);
  sfu_node_app_config_cleanup(&config);

  app_test_restore_env("TURBO_SFU_USE_TLS", saved_use_tls);
  app_test_restore_env("TURBO_SFU_TLS_CERT_FILE", saved_tls_cert);
  app_test_restore_env("TURBO_SFU_TLS_KEY_FILE", saved_tls_key);
  app_test_restore_env("TURBO_SFU_AUTH_ACTIVE_KEY_ID", saved_auth_key);
  app_test_restore_env("TURBO_SFU_AUTH_ACTIVE_SECRET", saved_auth_secret);
  app_test_restore_env("TURBO_SFU_TENANT_QUOTA_CAPACITY", saved_quota);
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
  sfu_config.ice_allow_loopback = 1;

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  check_not_null(node);

  check_equal((int)(turbo_sfu_node_attach_room(node, "room-webrtc-http", 4)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-webrtc-http", "alice",
                                                      "sess-alice", NULL)), (int)(0));

  http_api = sfu_node_http_api_create(sfu_server);
  check_not_null(http_api);
  check_equal((int)(sfu_node_http_api_start(http_api, sfu_config.bind_host,
                                                   sfu_config.bind_port)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100)), (int)(0));
  fprintf(stderr, "http webrtc test: server ready\n");

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_webrtc_session\","
      "\"room_id\":\"room-webrtc-http\","
      "\"participant_id\":\"alice\","
      "\"session_id\":\"sess-alice\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  session = json_object_field(root, "webrtc_session");
  check_not_null(session);
  check_equal(json_string_value(session, "room_id"), "room-webrtc-http");
  check_equal(json_string_value(session, "participant_id"), "alice");
  check_equal(json_string_value(session, "session_id"), "sess-alice");
  check_equal(json_string_value(session, "state"), "new");
  check_equal((int)(json_bool_value(session, "remote_description_set", 1)), (int)(0));
  check_equal((int)(json_int_value(session, "remote_track_count", -1)), (int)(0));
  json_free(root);
  root = NULL;
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
  check_not_null(offerer);

  {
    turbo_media_track_config_t send_config;

    memset(&send_config, 0, sizeof(send_config));
    send_config.type = TURBO_RTC_MEDIA_TRACK_VIDEO;
    send_config.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
    send_config.codec = TURBO_CODEC_VP8;
    send_config.video.width = 160;
    send_config.video.height = 120;
    send_config.video.framerate = 30;
    send_config.video.bitrate = 500000;
    send_config.video.keyframe_interval = 30;
    send_track = turbo_peer_connection_add_track_ex(offerer, &send_config);
  }
  check_not_null(send_track);

  check_greater(turbo_peer_connection_create_offer(offerer, offer_sdp,
                                                                 sizeof(offer_sdp)), 0);
  check_equal((int)(sfu_node_app_server_set_remote_offer(
                               sfu_server, "room-webrtc-http", "sess-alice", offer_sdp)), (int)(0));

  root = http_get_json(sfu_node_base_url,
                       "/api/v1/rooms/room-webrtc-http/webrtc_sessions/sess-alice");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  fprintf(stderr, "http webrtc test: get session ok\n");
  session = json_object_field(root, "webrtc_session");
  check_not_null(session);
  check_equal(json_string_value(session, "room_id"), "room-webrtc-http");
  check_equal(json_string_value(session, "participant_id"), "alice");
  check_equal(json_string_value(session, "session_id"), "sess-alice");
  check_equal((int)(json_bool_value(session, "remote_description_set", 0)), (int)(1));
  check_equal((int)(json_int_value(session, "remote_track_count", 0)), (int)(1));
  {
    json_value_t *local_ice_candidates =
        json_object_get(session, "local_ice_candidates");
    check_not_null(local_ice_candidates);
    check_equal((int)(json_type(local_ice_candidates)), (int)(JSON_ARRAY));
  }
  answer_sdp = json_string_value(session, "local_answer");
  check_not_null(answer_sdp);
  check_equal((int)(turbo_peer_connection_set_remote_description(
                               offerer, "answer", answer_sdp)), (int)(0));

  json_free(root);

  root = NULL;
  free_offerer_candidates(&offerer_state);
  turbo_peer_connection_destroy(offerer);
  check_equal((int)(sfu_node_app_server_remove_webrtc_session(
                               sfu_server, "room-webrtc-http", "sess-alice")), (int)(0));
  check_equal((int)(turbo_sfu_node_remove_session(
                               node, "room-webrtc-http", "sess-alice")), (int)(0));
  check_equal((int)(turbo_sfu_node_detach_room(node, "room-webrtc-http")), (int)(0));
  sfu_node_http_api_stop(http_api);
  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(sfu_server);
}

void test_sfu_node_whip_whep_resources_auth_restart_and_delete(void) {
  sfu_node_app_config_t sfu_config;
  sfu_node_app_server_t *sfu_server = NULL;
  sfu_node_http_api_t *http_api = NULL;
  turbo_sfu_node_t *node = NULL;
  turbo_peer_connection_t *publisher = NULL;
  turbo_peer_connection_t *viewer = NULL;
  turbo_peer_config_t peer_config;
  turbo_peer_callbacks_t peer_callbacks;
  offerer_callback_state_t publisher_state;
  char offer[16384];
  char restart_offer[16384];
  char fragment[2048];
  char ufrag[64];
  char pwd[96];
  char *location = NULL;
  char *etag = NULL;
  char *next_etag = NULL;
  char *content_type = NULL;
  char *signed_publish_token = NULL;
  char *signed_subscribe_token = NULL;
  sfu_test_http_response_t *response = NULL;
  turbo_media_auth_config_t signed_auth;
  turbo_media_auth_claims_t signed_claims;
  uint64_t connect_deadline_ms;
  int64_t auth_now = (int64_t)time(NULL);
  const char *base_url = "http://127.0.0.1:19422";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19422;
  sfu_config.node_id = "node-whip-whep-http-1";
  sfu_config.media_access_token = "test-media-token";
  sfu_config.auth_issuer = "turbomedia-test";
  sfu_config.auth_active_key_id = "media-2026-07";
  sfu_config.auth_active_secret = "0123456789abcdef0123456789abcdef";
  sfu_config.auth_max_ttl_seconds = 300;
  sfu_config.ice_allow_loopback = 1;
  signed_auth = (turbo_media_auth_config_t){
      .issuer = sfu_config.auth_issuer,
      .active_key_id = sfu_config.auth_active_key_id,
      .active_secret = sfu_config.auth_active_secret,
      .clock_skew_seconds = sfu_config.auth_clock_skew_seconds,
      .max_ttl_seconds = sfu_config.auth_max_ttl_seconds};
  signed_claims = (turbo_media_auth_claims_t){
      .subject = "media-client",
      .audience = "turbomedia-sfu-media",
      .scope = "sfu.media.publish",
      .room_id = "room-media-http",
      .participant_id = "alice",
      .issued_at = auth_now,
      .expires_at = auth_now + 120};
  signed_publish_token =
      turbo_media_auth_issue(&signed_auth, &signed_claims);
  check_not_null(signed_publish_token);
  signed_claims.scope = "sfu.media.subscribe";
  signed_subscribe_token =
      turbo_media_auth_issue(&signed_auth, &signed_claims);
  check_not_null(signed_subscribe_token);

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  check_not_null(node);
  check_equal((int)(turbo_sfu_node_attach_room(node, "room-media-http", 4)), (int)(0));

  memset(&peer_config, 0, sizeof(peer_config));
  memset(&peer_callbacks, 0, sizeof(peer_callbacks));
  memset(&publisher_state, 0, sizeof(publisher_state));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  peer_config.user_data = &publisher_state;
  peer_callbacks.on_state_change = offerer_on_state_change;
  publisher = turbo_peer_connection_create(&peer_config, &peer_callbacks);
  check_not_null(publisher);
  check_not_null(turbo_peer_connection_add_track(
      publisher, TURBO_RTC_MEDIA_TRACK_VIDEO, TURBO_MEDIA_DIRECTION_SENDONLY));
  check_greater(turbo_peer_connection_create_offer(publisher, offer, sizeof(offer)), 0);

  http_api = sfu_node_http_api_create(sfu_server);
  check_not_null(http_api);
  check_equal((int)(sfu_node_http_api_start(
             http_api, sfu_config.bind_host, sfu_config.bind_port)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(base_url, "/health", 30, 100)), (int)(0));

  response = http_media_request(
      base_url, "POST", "/whip/room-media-http/alice",
      "application/sdp", NULL, NULL, offer, strlen(offer));
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(401));
  sfu_test_http_response_free(response);
  response = NULL;

  response = http_media_request(
      base_url, "POST", "/whip/room-media-http/bob",
      "application/sdp", signed_publish_token, NULL, offer, strlen(offer));
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(401));
  sfu_test_http_response_free(response);
  response = NULL;

  response = http_media_request(
      base_url, "POST", "/whip/room-media-http/alice",
      "application/sdp", signed_subscribe_token, NULL, offer, strlen(offer));
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(401));
  sfu_test_http_response_free(response);
  response = NULL;

  response = http_media_request(
      base_url, "POST", "/whip/room-media-http/alice",
      "application/sdp", "test-media-token", NULL, offer, strlen(offer));
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(201));
  check_greater((int)response->body_len, 0);
  location = sfu_test_http_response_get_header(response, "Location");
  etag = sfu_test_http_response_get_header(response, "ETag");
  content_type = sfu_test_http_response_get_header(response, "Content-Type");
  check_not_null(location);
  check_not_null(etag);
  check_not_null(content_type);
  check_not_null(strstr(location, "/whip/room-media-http/alice/sessions/"));
  check_not_null(strstr(content_type, "application/sdp"));
  check_equal((int)(turbo_peer_connection_set_remote_description(
             publisher, "answer", response->body)), (int)(0));
  sfu_test_http_response_free(response);
  response = NULL;
  free(content_type);
  content_type = NULL;

  check_equal((int)(copy_sdp_attribute_value(
             offer, "a=ice-ufrag:", ufrag, sizeof(ufrag))), (int)(0));
  check_equal((int)(copy_sdp_attribute_value(
             offer, "a=ice-pwd:", pwd, sizeof(pwd))), (int)(0));
  check_greater(snprintf(fragment, sizeof(fragment),
                  "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\n", ufrag, pwd), 0);

  response = http_media_request(
      base_url, "PATCH", location,
      "application/trickle-ice-sdpfrag", "test-media-token", "\"999\"",
      fragment, strlen(fragment));
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(412));
  sfu_test_http_response_free(response);
  response = NULL;

  response = http_media_request(
      base_url, "PATCH", location,
      "application/trickle-ice-sdpfrag", "test-media-token", etag,
      fragment, strlen(fragment));
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(204));
  sfu_test_http_response_free(response);
  response = NULL;

  connect_deadline_ms =
      app_test_now_ms() + SFU_NODE_TEST_WHIP_CONNECT_TIMEOUT_MS;
  while (!publisher_state.connected &&
         app_test_now_ms() < connect_deadline_ms) {
    turbo_peer_connection_poll(publisher);
    app_test_sleep_ms(SFU_NODE_TEST_ICE_POLL_INTERVAL_MS);
  }
  check_true(publisher_state.connected);
  check_equal((int)(turbo_peer_connection_restart_ice(publisher)), (int)(0));
  check_greater(turbo_peer_connection_create_offer(
             publisher, restart_offer, sizeof(restart_offer)), 0);
  check_equal((int)(copy_sdp_attribute_value(
             restart_offer, "a=ice-ufrag:", ufrag, sizeof(ufrag))), (int)(0));
  check_equal((int)(copy_sdp_attribute_value(
             restart_offer, "a=ice-pwd:", pwd, sizeof(pwd))), (int)(0));
  check_greater(snprintf(fragment, sizeof(fragment),
                  "a=ice-ufrag:%s\r\n"
                  "a=ice-pwd:%s\r\n"
                  "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
                  "a=mid:0\r\n"
                  "a=end-of-candidates\r\n",
                  ufrag, pwd), 0);

  response = http_media_request(
      base_url, "PATCH", location,
      "application/trickle-ice-sdpfrag", "test-media-token", etag,
      fragment, strlen(fragment));
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(200));
  check_greater((int)response->body_len, 0);
  next_etag = sfu_test_http_response_get_header(response, "ETag");
  check_not_null(next_etag);
  check_true(strcmp(etag, next_etag) != 0);
  check_equal((int)(turbo_peer_connection_apply_remote_ice_sdpfrag(
             publisher, response->body, response->body_len)), (int)(1));
  sfu_test_http_response_free(response);
  response = NULL;

  response = http_media_request(
      base_url, "PATCH", location,
      "application/trickle-ice-sdpfrag", "test-media-token", etag,
      fragment, strlen(fragment));
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(412));
  sfu_test_http_response_free(response);
  response = NULL;

  response = http_media_request(
      base_url, "DELETE", location, NULL, "test-media-token", NULL, NULL, 0);
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(204));
  sfu_test_http_response_free(response);
  response = NULL;
  response = http_media_request(
      base_url, "DELETE", location, NULL, "test-media-token", NULL, NULL, 0);
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(404));
  sfu_test_http_response_free(response);
  response = NULL;

  peer_config.user_data = NULL;
  viewer = turbo_peer_connection_create(&peer_config, NULL);
  check_not_null(viewer);
  check_not_null(turbo_peer_connection_add_track(
      viewer, TURBO_RTC_MEDIA_TRACK_VIDEO, TURBO_MEDIA_DIRECTION_RECVONLY));
  check_greater(turbo_peer_connection_create_offer(viewer, offer, sizeof(offer)), 0);
  response = http_media_request(
      base_url, "POST", "/whep/room-media-http/bob",
      "application/sdp", "test-media-token", NULL, offer, strlen(offer));
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(201));
  free(location);
  location = sfu_test_http_response_get_header(response, "Location");
  check_not_null(location);
  check_not_null(strstr(location, "/whep/room-media-http/bob/sessions/"));
  sfu_test_http_response_free(response);
  response = NULL;
  response = http_media_request(
      base_url, "DELETE", location, NULL, "test-media-token", NULL, NULL, 0);
  check_not_null(response);
  check_equal((int)(response->status_code), (int)(204));
  sfu_test_http_response_free(response);

  free(next_etag);
  free(etag);
  free(location);
  free(signed_subscribe_token);
  free(signed_publish_token);
  turbo_peer_connection_destroy(viewer);
  turbo_peer_connection_destroy(publisher);
  check_equal((int)(turbo_sfu_node_detach_room(node, "room-media-http")), (int)(0));
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
  sfu_config.ice_allow_loopback = 1;

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  check_not_null(node);

  check_equal((int)(turbo_sfu_node_attach_room(node, "room-media", 4)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-media", "alice",
                                                      "sess-alice", NULL)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-media", "bob",
                                                      "sess-bob", NULL)), (int)(0));
  check_equal((int)(sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-media", "alice", "sess-alice")), (int)(0));

  memset(&peer_config, 0, sizeof(peer_config));
  memset(&publisher_callbacks, 0, sizeof(publisher_callbacks));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  peer_config.user_data = &publisher_state;
  publisher_callbacks.on_state_change = media_peer_on_state_change;
  publisher_callbacks.on_ice_candidate = media_peer_on_ice_candidate;
  publisher = turbo_peer_connection_create(&peer_config, &publisher_callbacks);
  check_not_null(publisher);
  publisher_media = turbo_peer_connection_get_media_context(publisher);
  check_not_null(publisher_media);

  memset(&track_config, 0, sizeof(track_config));
  track_config.type = TURBO_RTC_MEDIA_TRACK_VIDEO;
  track_config.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
  track_config.codec = TURBO_CODEC_VP8;
  track_config.video.width = 160;
  track_config.video.height = 120;
  track_config.video.framerate = 30;
  track_config.video.bitrate = 500000;
  track_config.video.keyframe_interval = 30;
  publisher_track = turbo_peer_connection_add_track_ex(publisher, &track_config);
  check_not_null(publisher_track);
  publisher_ssrc = turbo_media_track_get_ssrc(publisher_track);
  check_true(publisher_ssrc != 0);

  check_equal((int)(turbo_sfu_node_register_published_track(
                               node, "room-media", "alice", "track-cam", publisher_ssrc, NULL, 0)), (int)(0));
  check_equal((int)(sfu_node_app_server_register_published_track(
                               sfu_server, "room-media", "alice", "track-cam", publisher_ssrc,
                               NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp8")), (int)(0));
  check_equal((int)(turbo_sfu_node_set_receiver_bandwidth(
                               node, "room-media", "bob", 1500000u)), (int)(0));
  check_equal((int)(turbo_sfu_node_set_track_subscription(
                               node, "room-media", "bob", "track-cam", 1,
                               TURBO_ROOM_VIDEO_LAYER_LOW)), (int)(0));
  check_equal((int)(sfu_node_app_server_set_track_subscription(
                               sfu_server, "room-media", "bob", "track-cam", 1,
                               TURBO_ROOM_VIDEO_LAYER_LOW)), (int)(0));
  check_equal((int)(sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-media", "bob", "sess-bob")), (int)(0));

  memset(&peer_config, 0, sizeof(peer_config));
  memset(&subscriber_callbacks, 0, sizeof(subscriber_callbacks));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  peer_config.user_data = &subscriber_state;
  subscriber_callbacks.on_state_change = media_peer_on_state_change;
  subscriber_callbacks.on_track = media_peer_on_track;
  subscriber_callbacks.on_ice_candidate = media_peer_on_ice_candidate;
  subscriber = turbo_peer_connection_create(&peer_config, &subscriber_callbacks);
  check_not_null(subscriber);
  subscriber_media = turbo_peer_connection_get_media_context(subscriber);
  check_not_null(subscriber_media);

  memset(&track_config, 0, sizeof(track_config));
  track_config.type = TURBO_RTC_MEDIA_TRACK_VIDEO;
  track_config.direction = TURBO_MEDIA_DIRECTION_RECVONLY;
  track_config.codec = TURBO_CODEC_VP8;
  track_config.video.width = 160;
  track_config.video.height = 120;
  track_config.video.framerate = 30;
  track_config.video.bitrate = 500000;
  track_config.video.keyframe_interval = 30;
  subscriber_recv_track = turbo_peer_connection_add_track_ex(subscriber, &track_config);
  check_not_null(subscriber_recv_track);

  check_greater(turbo_peer_connection_create_offer(subscriber, subscriber_offer,
                                                                 sizeof(subscriber_offer)), 0);
  check_equal((int)(sfu_node_app_server_set_remote_offer(
                                sfu_server, "room-media", "sess-bob",
                                subscriber_offer)), (int)(0));
  subscriber_answer = copy_session_answer(sfu_server, "room-media", "sess-bob");
  check_not_null(subscriber_answer);
  check_equal((int)(turbo_peer_connection_set_remote_description(
                                subscriber, "answer", subscriber_answer)), (int)(0));
  sync_peer_candidates_to_sfu(sfu_server, "room-media", "sess-bob", &subscriber_state);
  sync_sfu_candidates_to_peer(sfu_server, "room-media", "sess-bob", subscriber,
                              &subscriber_state);

  check_greater(turbo_peer_connection_create_offer(publisher, publisher_offer,
                                                                 sizeof(publisher_offer)), 0);
  check_equal((int)(sfu_node_app_server_set_remote_offer(
                                sfu_server, "room-media", "sess-alice",
                                publisher_offer)), (int)(0));
  publisher_answer = copy_session_answer(sfu_server, "room-media", "sess-alice");
  check_not_null(publisher_answer);
  check_equal((int)(turbo_peer_connection_set_remote_description(
                                publisher, "answer", publisher_answer)), (int)(0));
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

    if (!sender_started && publisher_state.connected &&
        subscriber_state.connected && subscriber_state.remote_track &&
        turbo_media_track_get_state(subscriber_state.remote_track) == TURBO_MEDIA_STATE_ACTIVE) {
      check_equal((int)(turbo_media_track_start(publisher_track)), (int)(0));
      sender_started = 1;
    }

    if (sender_started && sent_frames < 16) {
      fill_video_i420_frame(frame, 160, 120, sent_frames);
      check_equal((int)(turbo_media_track_send_frame(
                                   publisher_track, frame, sizeof(frame),
                                   90000ULL + (uint64_t)sent_frames * 3000ULL)), (int)(0));
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
    json_free(publisher_session);
    publisher_session = NULL;
    publisher_session = NULL;
    sfu_node_app_server_poll_webrtc(sfu_server);
    turbo_peer_connection_poll(publisher);
    turbo_peer_connection_poll(subscriber);
    turbo_media_handle_timers(publisher_media);
    turbo_media_handle_timers(subscriber_media);
    app_test_sleep_ms(5);
  }

  if (publisher_session) {
    json_free(publisher_session);
    publisher_session = NULL;
    publisher_session = NULL;
  }

  publisher_session = build_session_root(sfu_server, "room-media", "sess-alice");
  subscriber_session = build_session_root(sfu_server, "room-media", "sess-bob");
  check_not_null(publisher_session);
  check_not_null(subscriber_session);

  check_true(sender_started);
  check_not_null(subscriber_state.remote_track);
  check_greater_equal(sent_frames, 1);
  check_greater_equal(subscriber_state.frame_state.frame_count, 1);
  check_greater_equal(json_int_value(publisher_session, "remote_frame_count", 0), 1);
  check_equal((int)(json_int_value(subscriber_session, "relay_track_count", -1)), (int)(1));
  check_greater_equal((int)subscriber_state.frame_state.last_timestamp, 90000);

  json_free(publisher_session);

  publisher_session = NULL;
  json_free(subscriber_session);
  subscriber_session = NULL;
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
  turbo_demuxer_config_t demuxer_config;
  turbo_demuxer_packet_t demuxed_packet;
  turbo_stream_info_t demuxed_stream;
  turbo_demuxer_t *demuxer = NULL;
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
  memset(&demuxer_config, 0, sizeof(demuxer_config));
  memset(&demuxed_packet, 0, sizeof(demuxed_packet));
  memset(&demuxed_stream, 0, sizeof(demuxed_stream));

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19415;
  sfu_config.node_id = "node-record-1";
  sfu_config.ice_allow_loopback = 1;

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  node = sfu_node_app_server_get_node(sfu_server);
  check_not_null(node);

  check_equal((int)(turbo_sfu_node_attach_room(node, "room-record", 4)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(node, "room-record", "alice",
                                                      "sess-alice", NULL)), (int)(0));
  check_equal((int)(sfu_node_app_server_create_webrtc_session(
                               sfu_server, "room-record", "alice", "sess-alice")), (int)(0));

  memset(&peer_config, 0, sizeof(peer_config));
  memset(&publisher_callbacks, 0, sizeof(publisher_callbacks));
  peer_config.allow_loopback = 1;
  peer_config.disable_datachannel = 1;
  peer_config.user_data = &publisher_state;
  publisher_callbacks.on_state_change = media_peer_on_state_change;
  publisher_callbacks.on_ice_candidate = media_peer_on_ice_candidate;
  publisher = turbo_peer_connection_create(&peer_config, &publisher_callbacks);
  check_not_null(publisher);
  publisher_media = turbo_peer_connection_get_media_context(publisher);
  check_not_null(publisher_media);

  memset(&track_config, 0, sizeof(track_config));
  track_config.type = TURBO_RTC_MEDIA_TRACK_VIDEO;
  track_config.direction = TURBO_MEDIA_DIRECTION_SENDONLY;
  track_config.codec = TURBO_CODEC_VP8;
  track_config.video.width = 160;
  track_config.video.height = 120;
  track_config.video.framerate = 30;
  track_config.video.bitrate = 500000;
  track_config.video.keyframe_interval = 30;
  publisher_track = turbo_peer_connection_add_track_ex(publisher, &track_config);
  check_not_null(publisher_track);
  publisher_ssrc = turbo_media_track_get_ssrc(publisher_track);
  check_true(publisher_ssrc != 0);

  check_equal((int)(turbo_sfu_node_register_published_track(
                               node, "room-record", "alice", "track-cam", publisher_ssrc, NULL, 0)), (int)(0));
  check_equal((int)(sfu_node_app_server_register_published_track(
                               sfu_server, "room-record", "alice", "track-cam", publisher_ssrc,
                               NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp8")), (int)(0));
  check_equal((int)(sfu_node_app_server_start_recording(
                               sfu_server, "room-record", "rec-001", "archive")), (int)(0));

  check_greater(turbo_peer_connection_create_offer(publisher, publisher_offer,
                                                                 sizeof(publisher_offer)), 0);
  check_equal((int)(sfu_node_app_server_set_remote_offer(
                               sfu_server, "room-record", "sess-alice", publisher_offer)), (int)(0));
  publisher_answer = copy_session_answer(sfu_server, "room-record", "sess-alice");
  check_not_null(publisher_answer);
  check_equal((int)(turbo_peer_connection_set_remote_description(
                               publisher, "answer", publisher_answer)), (int)(0));

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
      check_equal((int)(turbo_media_track_start(publisher_track)), (int)(0));
      sender_started = 1;
    }

    if (sender_started && sent_frames < 16) {
      fill_video_i420_frame(frame, 160, 120, sent_frames);
      check_equal((int)(turbo_media_track_send_frame(
                                   publisher_track, frame, sizeof(frame),
                                   90000ULL + (uint64_t)sent_frames * 3000ULL)), (int)(0));
      sent_frames++;
    }

    if (sent_frames >= 16) {
      break;
    }

    app_test_sleep_ms(5);
  }

  check_true(sender_started);
  check_greater_equal(sent_frames, 16);
  app_test_sleep_ms(100);

  check_equal((int)(sfu_node_app_server_stop_recording(sfu_server, "room-record")), (int)(0));
  status_json = sfu_node_app_server_build_recording_status_json(sfu_server, "room-record");
  check_not_null(status_json);
  check_equal((int)(parse_json_text(status_json, &recording_status)), (int)(0));
  output_path = json_string_value(recording_status, "output_path");
  check_not_null(output_path);
  output_path_copy = app_strdup(output_path);
  check_not_null(output_path_copy);
  check_false(json_bool_value(recording_status, "active", 1));
  check_greater_equal(json_int_value(recording_status, "track_count", 0), 1);
  check_greater_equal(json_int_value(recording_status, "packet_count", 0), 1);
  check_greater_equal(json_int_value(recording_status, "total_bytes", 0), 1);

  output_size = file_size_bytes(output_path_copy);
  check_greater_equal((int)output_size, 1);

  turbo_demuxer_registry_init();
  demuxer_config.input_path = output_path_copy;
  demuxer = turbo_demuxer_create_by_name("mkv", &demuxer_config);
  check_not_null(demuxer);
  check_equal((int)(turbo_demuxer_open(demuxer)), (int)(0));
  check_equal((int)(turbo_demuxer_get_stream_count(demuxer)), (int)(1));
  check_equal((int)(turbo_demuxer_get_stream_info(
                               demuxer, 0, &demuxed_stream)), (int)(0));
  check_equal(demuxed_stream.codec_name, "vp8");
  check_equal((int)(turbo_demuxer_read_packet(demuxer,
                                                      &demuxed_packet)), (int)(1));
  check_not_null(demuxed_packet.data);
  check_greater_equal((int)demuxed_packet.size, 1);

  turbo_demuxer_free_packet(&demuxed_packet);
  turbo_demuxer_destroy(demuxer);
  turbo_demuxer_registry_shutdown();
  json_free(recording_status);
  recording_status = NULL;
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

void test_sfu_node_recording_stop_failure_still_closes_runtime(void) {
  sfu_node_app_config_t config;
  sfu_node_app_server_t *server;
  turbo_sfu_node_t *node;
  json_value_t *status = NULL;
  char *status_json = NULL;
  char *output_path = NULL;

  sfu_node_app_config_init(&config);
  config.bind_host = "127.0.0.1";
  config.bind_port = 19416;
  config.node_id = "node-record-stop-failure";
  config.ice_allow_loopback = 1;

  server = sfu_node_app_server_create(&config);
  check_not_null(server);
  node = sfu_node_app_server_get_node(server);
  check_not_null(node);
  check_equal((int)(turbo_sfu_node_attach_room(node, "room-stop-failure", 2)), (int)(0));
  check_equal((int)(turbo_sfu_node_add_session(
                               node, "room-stop-failure", "alice",
                               "session-stop-failure", NULL)), (int)(0));
  check_equal((int)(turbo_sfu_node_register_published_track(
                               node, "room-stop-failure", "alice", "track-cam",
                               0x10203040U, NULL, 0)), (int)(0));
  check_equal((int)(sfu_node_app_server_register_published_track(
                               server, "room-stop-failure", "alice", "track-cam",
                               0x10203040U, NULL, 0, TURBO_ROOM_TRACK_VIDEO, "vp8")), (int)(0));
  check_equal((int)(sfu_node_app_server_start_recording(
                               server, "room-stop-failure", "rec-stop-failure",
                               "archive")), (int)(0));

  turbo_recorder_test_reset_io_failures();
  turbo_recorder_test_fail_io_once(TURBO_RECORDER_TEST_IO_FLUSH, 1);
  check_equal((int)(sfu_node_app_server_stop_recording(
                                server, "room-stop-failure")), (int)(-1));
  turbo_recorder_test_reset_io_failures();

  status_json = sfu_node_app_server_build_recording_status_json(
      server, "room-stop-failure");
  check_not_null(status_json);
  check_equal((int)(parse_json_text(status_json, &status)), (int)(0));
  check_false(json_bool_value(status, "active", 1));
  output_path = app_strdup(json_string_value(status, "output_path"));
  check_not_null(output_path);
  check_equal((int)(sfu_node_app_server_stop_recording(
                                server, "room-stop-failure")), (int)(-1));

  json_free(status);

  status = NULL;
  free(status_json);
  sfu_node_app_server_destroy(server);
  remove(output_path);
  free(output_path);
}

void test_sfu_node_http_lifecycle_repeated_start_stop(void) {
  sfu_node_app_config_t config;
  sfu_node_app_server_t *server = NULL;
  sfu_node_http_api_t *http_api = NULL;
  const char *base_url = "http://127.0.0.1:19434";
  int iteration;

  sfu_node_app_config_init(&config);
  config.bind_host = "127.0.0.1";
  config.bind_port = 19434;
  config.node_id = "sfu-node-http-lifecycle";
  config.ice_allow_loopback = 1;

  server = sfu_node_app_server_create(&config);
  check_not_null(server);
  http_api = sfu_node_http_api_create(server);
  check_not_null(http_api);

  for (iteration = 0; iteration < SFU_NODE_HTTP_LIFECYCLE_STRESS_ITERATIONS;
       ++iteration) {
    check_equal((int)(sfu_node_http_api_start(http_api, config.bind_host,
                                   config.bind_port)), (int)(0));
    check_equal((int)(wait_for_http_status_ok(base_url, "/health", 3, 10)), (int)(0));
    if (iteration + 1 < SFU_NODE_HTTP_LIFECYCLE_STRESS_ITERATIONS) {
      sfu_node_http_api_stop(http_api);
    }
  }

  sfu_node_http_api_destroy(http_api);
  sfu_node_app_server_destroy(server);
}

spec("test_sfu_node_app") {
  it("test_sfu_node_rejects_identifiers_that_do_not_fit_storage") { test_sfu_node_rejects_identifiers_that_do_not_fit_storage(); };
  it("test_sfu_node_http_lifecycle_repeated_start_stop") { test_sfu_node_http_lifecycle_repeated_start_stop(); };
  it("test_sfu_node_webrtc_session_accepts_offer_and_generates_answer") { test_sfu_node_webrtc_session_accepts_offer_and_generates_answer(); };
  it("test_sfu_node_webrtc_session_provisions_relay_track_before_answer") { test_sfu_node_webrtc_session_provisions_relay_track_before_answer(); };
  it("test_sfu_node_webrtc_session_provisions_multiple_publishers_to_one_subscriber") { test_sfu_node_webrtc_session_provisions_multiple_publishers_to_one_subscriber(); };
  it("test_sfu_node_http_roundtrips_track_subscription_metadata") { test_sfu_node_http_roundtrips_track_subscription_metadata(); };
  it("test_sfu_node_https_uses_explicit_identity_and_verified_client") { test_sfu_node_https_uses_explicit_identity_and_verified_client(); };
  it("test_sfu_node_config_reads_security_and_ice_from_env") { test_sfu_node_config_reads_security_and_ice_from_env(); };
  it("test_sfu_node_config_reads_signed_only_tenant_quota_from_env") { test_sfu_node_config_reads_signed_only_tenant_quota_from_env(); };
  it("test_sfu_node_dynamic_revocation_controls_control_and_media_auth") { test_sfu_node_dynamic_revocation_controls_control_and_media_auth(); };
  it("test_sfu_node_tenant_quota_transport_requires_dedicated_signed_control_token") { test_sfu_node_tenant_quota_transport_requires_dedicated_signed_control_token(); };
  it("test_sfu_node_http_control_token_protects_modifying_commands") { test_sfu_node_http_control_token_protects_modifying_commands(); };
  it("test_sfu_node_signed_control_token_enforces_scope_room_expiry_and_rotation") { test_sfu_node_signed_control_token_enforces_scope_room_expiry_and_rotation(); };
  it("test_sfu_node_ready_metrics_and_drain_control") { test_sfu_node_ready_metrics_and_drain_control(); };
  it("test_sfu_node_webrtc_session_http_commands_roundtrip_offer_and_query_session") { test_sfu_node_webrtc_session_http_commands_roundtrip_offer_and_query_session(); };
  it("test_sfu_node_whip_whep_resources_auth_restart_and_delete") { test_sfu_node_whip_whep_resources_auth_restart_and_delete(); };
  it("test_sfu_node_media_bridge_forwards_video_to_subscriber") { test_sfu_node_media_bridge_forwards_video_to_subscriber(); };
  it("test_sfu_node_recording_archives_publisher_rtp") { test_sfu_node_recording_archives_publisher_rtp(); };
  it("test_sfu_node_recording_stop_failure_still_closes_runtime") { test_sfu_node_recording_stop_failure_still_closes_runtime(); };
}
