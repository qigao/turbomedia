#include "tinytest.h"
#include "room_service/config.h"
#include "room_service/http_api.h"
#include "room_service/server.h"
#include "sfu_node/config.h"
#include "sfu_node/server.h"
#include "turbo_transport.h"
#include "turbo_media_auth.h"
#include <json_parser.h>
#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
#include "ivr_room_bridge.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ROOM_SERVICE_HTTP_LIFECYCLE_STRESS_ITERATIONS 32
#define ROOM_SERVICE_PROVIDER_LIFECYCLE_HTTP_PORT 19435
#define ROOM_SERVICE_PROVIDER_LIFECYCLE_CONTROL_WS_PORT 19436
#define ROOM_SERVICE_PROVIDER_LIFECYCLE_PUB_PORT 19437
#define ROOM_SERVICE_PROVIDER_LIFECYCLE_IRIS_PORT 19438
#define ROOM_SERVICE_PROVIDER_LIFECYCLE_WAIT_ATTEMPTS 1000
#define ROOM_SERVICE_PROVIDER_LIFECYCLE_WAIT_MS 5

#ifndef ROOM_SERVICE_TEST_TLS_CERT_PATH
#error "ROOM_SERVICE_TEST_TLS_CERT_PATH must identify the test certificate"
#endif

#ifndef ROOM_SERVICE_TEST_TLS_KEY_PATH
#error "ROOM_SERVICE_TEST_TLS_KEY_PATH must identify the test private key"
#endif

#ifdef _WIN32
#include <windows.h>
static void app_test_sleep_ms(unsigned int ms) { Sleep(ms); }
#define app_strdup _strdup
#else
#include <unistd.h>
static void app_test_sleep_ms(unsigned int ms) { usleep(ms * 1000); }
#define app_strdup strdup
#endif

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

#ifdef TURBO_MEDIA_HAS_IVR_CONTROL
extern ivr_status_t room_service_app_server_test_submit_iris_event(
    room_service_app_server_t *server, const ivr_media_event_t *event);

static void app_test_normalize_config_path(char *path) {
  if (!path) {
    return;
  }
  for (; *path; ++path) {
    if (*path == '\\') {
      *path = '/';
    }
  }
}

static int app_test_wait_for_iris_event_in_flight(
    room_service_app_server_t *server) {
  room_service_ivr_metrics_t metrics;
  int attempt;

  for (attempt = 0;
       attempt < ROOM_SERVICE_PROVIDER_LIFECYCLE_WAIT_ATTEMPTS; ++attempt) {
    if (room_service_app_server_get_ivr_metrics(server, &metrics) == 0 &&
        metrics.iris_outbox_persisted_total > 0u &&
        metrics.iris_delivery_attempts_total > 0u &&
        (metrics.iris_queue_items > 0u || metrics.iris_in_flight > 0u)) {
      return 0;
    }
    app_test_sleep_ms(ROOM_SERVICE_PROVIDER_LIFECYCLE_WAIT_MS);
  }
  return -1;
}
#endif

void test_room_service_rejects_identifiers_that_do_not_fit_storage(void) {
  turbo_room_service_t *service = NULL;
  turbo_room_config_t room_config;
  turbo_room_participant_config_t participant_config;
  turbo_room_track_config_t track_config;
  turbo_call_center_queue_entry_config_t queue_config;
  char valid_room_id[TURBO_ROOM_ID_MAX];
  char long_id_a[TURBO_ROOM_ID_MAX + 1];
  char long_id_b[TURBO_ROOM_ID_MAX + 1];

  memset(valid_room_id, 'r', sizeof(valid_room_id) - 1);
  valid_room_id[sizeof(valid_room_id) - 1] = '\0';
  memset(long_id_a, 'x', sizeof(long_id_a) - 1);
  long_id_a[sizeof(long_id_a) - 1] = '\0';
  memcpy(long_id_b, long_id_a, sizeof(long_id_a));
  long_id_b[sizeof(long_id_b) - 2] = 'y';

  service = turbo_room_service_create();
  check_not_null(service);

  memset(&room_config, 0, sizeof(room_config));
  room_config.room_type = TURBO_ROOM_TYPE_CONFERENCE;
  room_config.room_id = long_id_a;
  check_equal((int)(turbo_room_service_create_room(service, &room_config)), (int)(-1));
  room_config.room_id = long_id_b;
  check_equal((int)(turbo_room_service_create_room(service, &room_config)), (int)(-1));
  room_config.room_id = valid_room_id;
  check_equal((int)(turbo_room_service_create_room(service, &room_config)), (int)(0));

  check_equal((int)(turbo_room_service_assign_sfu_node(service, valid_room_id,
                                                           long_id_a)), (int)(-1));

  memset(&participant_config, 0, sizeof(participant_config));
  participant_config.participant_id = long_id_a;
  participant_config.role = TURBO_PARTICIPANT_ROLE_HOST;
  check_equal((int)(turbo_room_service_add_participant(
                                service, valid_room_id, &participant_config)), (int)(-1));
  participant_config.participant_id = "alice";
  check_equal((int)(turbo_room_service_add_participant(
                               service, valid_room_id, &participant_config)), (int)(0));

  memset(&track_config, 0, sizeof(track_config));
  track_config.track_id = long_id_a;
  track_config.owner_participant_id = "alice";
  track_config.kind = TURBO_ROOM_TRACK_VIDEO;
  track_config.codec_name = "vp8";
  check_equal((int)(turbo_room_service_publish_track(
                                service, valid_room_id, &track_config)), (int)(-1));

  check_equal((int)(turbo_room_service_start_recording(
                                service, valid_room_id, long_id_a, "archive")), (int)(-1));

  memset(&queue_config, 0, sizeof(queue_config));
  queue_config.queue_id = long_id_a;
  queue_config.side = TURBO_CALL_CENTER_QUEUE_CALLER;
  queue_config.entry_id = "entry-1";
  queue_config.endpoint_id = "endpoint-1";
  check_equal((int)(turbo_room_service_enqueue_call_center_queue_entry(
                                service, &queue_config)), (int)(-1));

  turbo_room_service_destroy(service);
}

static char *issue_room_control_token(
    const char *key_id, const char *secret, const char *scope,
    const char *room_id, const char *participant_id,
    int64_t issued_at, int64_t expires_at) {
  turbo_media_auth_config_t config = {
      .issuer = "turbomedia",
      .active_key_id = key_id,
      .active_secret = secret,
      .clock_skew_seconds = 0,
      .max_ttl_seconds = 3600,
  };
  turbo_media_auth_claims_t claims = {
      .subject = "room-control-test",
      .audience = "turbomedia-room-control",
      .scope = scope,
      .room_id = room_id,
      .participant_id = participant_id,
      .issued_at = issued_at,
      .expires_at = expires_at,
  };

  return turbo_media_auth_issue(&config, &claims);
}

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

static json_value_t *json_array_field(const json_value_t *obj, const char *key) {
  json_value_t *value;

  if (!obj || !key) {
    return NULL;
  }

  value = json_object_get(obj, key);
  if (!value || json_type(value) != JSON_ARRAY) {
    return NULL;
  }

  return value;
}

static json_value_t *json_array_object_at(const json_value_t *array, size_t index) {
  json_value_t *value;

  if (!array || json_type(array) != JSON_ARRAY ||
      index >= json_array_size(array)) {
    return NULL;
  }

  value = json_array_get(array, index);
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

static int json_is_null_field(const json_value_t *obj, const char *key) {
  json_value_t *value;

  if (!obj || !key) {
    return 0;
  }

  value = json_object_get(obj, key);
  return value && json_type(value) == JSON_NULL;
}

static json_value_t *find_call_center_event(const json_value_t *events_view,
                                            const char *event_type,
                                            const char *participant_id,
                                            const char *state) {
  json_value_t *events;
  size_t i;

  events = json_array_field(events_view, "events");
  if (!events || !event_type) {
    return NULL;
  }

  for (i = 0; i < json_array_size(events); ++i) {
    json_value_t *event = json_array_object_at(events, i);
    const char *current_type;

    if (!event) {
      continue;
    }

    current_type = json_string_value(event, "event_type");
    if (!current_type || strcmp(current_type, event_type) != 0) {
      continue;
    }
    if (participant_id) {
      const char *current_participant = json_string_value(event, "participant_id");

      if (!current_participant || strcmp(current_participant, participant_id) != 0) {
        continue;
      }
    }
    if (state) {
      const char *current_state = json_string_value(event, "state");

      if (!current_state || strcmp(current_state, state) != 0) {
        continue;
      }
    }

    return event;
  }

  return NULL;
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

static void test_http_response_free(chttp_response *response) {
  if (!response) {
    return;
  }
  chttp_response_destroy(response);
  free(response);
}

static int test_http_response_is_json(const chttp_response *response) {
  static const char json_type[] = "application/json";
  const char *content_type =
      response ? chttp_response_header(response, "Content-Type") : NULL;
  size_t index;

  if (!content_type) {
    return 0;
  }
  for (index = 0u; index + 1u < sizeof(json_type); ++index) {
    unsigned char actual = (unsigned char)content_type[index];
    unsigned char expected = (unsigned char)json_type[index];
    if (actual >= 'A' && actual <= 'Z') {
      actual = (unsigned char)(actual - 'A' + 'a');
    }
    if (actual != expected) {
      return 0;
    }
  }
  return content_type[index] == '\0' || content_type[index] == ';' ||
         content_type[index] == ' ' || content_type[index] == '\t';
}

static json_value_t *test_http_response_parse_json(
    const chttp_response *response) {
  if (!response || !response->body || response->body_size == 0u) {
    return NULL;
  }
  return json_parse((const char *)response->body, response->body_size);
}

static chttp_response *test_http_request(
    const char *base_url, const char *path, turbo_http_method_t method,
    const char *json_body, const char *ca_file, const char *bearer_token,
    int timeout_ms) {
  const char *json_headers[] = {
      "Content-Type", "application/json"
  };
  turbo_transport_config_t config = {0};
  turbo_transport_t *transport;
  chttp_response *response;
  const uint8_t *body = (const uint8_t *)json_body;
  size_t body_size = json_body ? strlen(json_body) : 0u;

  if (!base_url || !path || timeout_ms <= 0 ||
      turbo_transport_parse_url(base_url, &config) != 0 ||
      config.type != TURBO_TRANSPORT_HTTP ||
      (ca_file && !config.use_tls)) {
    return NULL;
  }
  config.connect_timeout_ms = timeout_ms;
  config.read_timeout_ms = timeout_ms;
  config.write_timeout_ms = timeout_ms;
  config.ca_cert_path = ca_file;
  config.user_agent = "TurboRoomServiceTest/0.1";
  config.auth_token = bearer_token;
  transport = turbo_transport_create(&config);
  if (!transport) {
    return NULL;
  }
  response = turbo_transport_http_request(
      transport, method, path, body, body_size,
      json_body ? json_headers : NULL, json_body ? 2 : 0);
  turbo_transport_destroy(transport);
  return response;
}

static json_value_t *http_get_json(const char *base_url, const char *path) {
  chttp_response *response;
  json_value_t *root = NULL;

  if (!base_url || !path) {
    return NULL;
  }

  response = test_http_request(base_url, path, TURBO_HTTP_GET, NULL, NULL,
                               NULL, 3000);
  if (!response ||
      response->status_code < 200 || response->status_code >= 300 ||
      !test_http_response_is_json(response)) {
    test_http_response_free(response);
    return NULL;
  }

  root = test_http_response_parse_json(response);
  test_http_response_free(response);
  if (!root || json_type(root) != JSON_OBJECT) {
    json_free(root);
    root = NULL;
    return NULL;
  }

  return root;
}

static char *http_get_text(const char *base_url, const char *path) {
  chttp_response *response;
  char *body = NULL;

  if (!base_url || !path) {
    return NULL;
  }
  response = test_http_request(base_url, path, TURBO_HTTP_GET, NULL, NULL,
                               NULL, 3000);
  if (response &&
      response->status_code >= 200 && response->status_code < 300 &&
      response->body && response->body_size < SIZE_MAX) {
    body = (char *)malloc(response->body_size + 1u);
    if (body) {
      memcpy(body, response->body, response->body_size);
      body[response->body_size] = '\0';
    }
  }
  test_http_response_free(response);
  return body;
}

static int wait_for_http_status_ok(const char *base_url, const char *path,
                                   int attempts, unsigned int sleep_ms) {
  int i;

  if (!base_url || !path || attempts <= 0) {
    return -1;
  }

  for (i = 0; i < attempts; ++i) {
    chttp_response *response = test_http_request(
        base_url, path, TURBO_HTTP_GET, NULL, NULL, NULL, 1000);
    int ok = response && response->status_code >= 200 &&
             response->status_code < 300;

    test_http_response_free(response);
    if (ok) {
      return 0;
    }

    app_test_sleep_ms(sleep_ms);
  }

  return -1;
}

static int wait_for_https_status_ok(const char *base_url, const char *path,
                                    const char *ca_file, int attempts,
                                    unsigned int sleep_ms) {
  int i;

  if (!base_url || !path || !ca_file || attempts <= 0) {
    return -1;
  }

  for (i = 0; i < attempts; ++i) {
    chttp_response *response = test_http_request(
        base_url, path, TURBO_HTTP_GET, NULL, ca_file, NULL, 1000);
    int ok = response && response->status_code >= 200 &&
             response->status_code < 300;

    test_http_response_free(response);
    if (ok) {
      return 0;
    }
    app_test_sleep_ms(sleep_ms);
  }

  return -1;
}

static json_value_t *https_post_json_result(
    const char *base_url, const char *path, const char *json_body,
    const char *ca_file) {
  chttp_response *response;
  json_value_t *root = NULL;

  if (!base_url || !path || !json_body || !ca_file) {
    return NULL;
  }
  response = test_http_request(base_url, path, TURBO_HTTP_POST, json_body,
                               ca_file, NULL, 3000);
  if (response &&
      response->status_code >= 200 && response->status_code < 300 &&
      test_http_response_is_json(response)) {
    root = test_http_response_parse_json(response);
  }
  test_http_response_free(response);
  return root;
}

static json_value_t *http_post_json_result(const char *base_url, const char *path,
                                           const char *json_body) {
  chttp_response *response;
  json_value_t *root = NULL;

  if (!base_url || !path || !json_body) {
    return NULL;
  }

  response = test_http_request(base_url, path, TURBO_HTTP_POST, json_body,
                               NULL, NULL, 3000);
  if (!response ||
      response->status_code < 200 || response->status_code >= 300 ||
      !test_http_response_is_json(response)) {
    test_http_response_free(response);
    return NULL;
  }

  root = test_http_response_parse_json(response);
  test_http_response_free(response);
  if (!root || json_type(root) != JSON_OBJECT) {
    json_free(root);
    root = NULL;
    return NULL;
  }

  return root;
}

static int http_post_json_status(const char *base_url, const char *path,
                                 const char *json_body, json_value_t **out_root) {
  chttp_response *response;
  json_value_t *root = NULL;
  int status = -1;

  if (out_root) {
    *out_root = NULL;
  }
  if (!base_url || !path || !json_body) {
    return -1;
  }

  response = test_http_request(base_url, path, TURBO_HTTP_POST, json_body,
                               NULL, NULL, 3000);
  if (response && test_http_response_is_json(response)) {
    status = response->status_code;
    root = test_http_response_parse_json(response);
  }

  test_http_response_free(response);

  if (root && json_type(root) == JSON_OBJECT && out_root) {
    *out_root = root;
  } else {
    json_free(root);
    root = NULL;
  }

  return status;
}

static int http_post_json_status_with_token(const char *base_url, const char *path,
                                            const char *json_body,
                                            const char *bearer_token,
                                            json_value_t **out_root) {
  chttp_response *response;
  json_value_t *root = NULL;
  int status = -1;

  if (out_root) {
    *out_root = NULL;
  }
  if (!base_url || !path || !json_body) {
    return -1;
  }

  response = test_http_request(base_url, path, TURBO_HTTP_POST, json_body,
                               NULL, bearer_token, 3000);
  if (response && test_http_response_is_json(response)) {
    status = response->status_code;
    root = test_http_response_parse_json(response);
  }

  test_http_response_free(response);

  if (root && json_type(root) == JSON_OBJECT && out_root) {
    *out_root = root;
  } else {
    json_free(root);
    root = NULL;
  }

  return status;
}

static void seed_room_runtime(room_service_app_server_t *room_server,
                              turbo_room_service_t *service) {
  turbo_room_config_t room = {
      .room_id = "room-replay",
      .room_type = TURBO_ROOM_TYPE_CONFERENCE,
      .created_by = "host-1",
  };
  turbo_room_participant_config_t alice = {
      .participant_id = "alice",
      .user_id = "u-alice",
      .display_name = "Alice",
      .role = TURBO_PARTICIPANT_ROLE_HOST,
  };
  turbo_room_participant_config_t bob = {
      .participant_id = "bob",
      .user_id = "u-bob",
      .display_name = "Bob",
      .role = TURBO_PARTICIPANT_ROLE_GUEST,
  };
  turbo_room_track_config_t cam = {
      .track_id = "track-cam",
      .owner_participant_id = "alice",
      .kind = TURBO_ROOM_TRACK_VIDEO,
      .source = TURBO_ROOM_SOURCE_CAMERA,
      .codec_name = "vp9",
      .simulcast_enabled = 1,
      .main_ssrc = 8195,
      .layer_ssrcs = (uint32_t[]){8193, 8194, 8195},
      .layer_count = 3,
  };
  turbo_room_subscription_config_t cam_sub = {
      .subscriber_participant_id = "bob",
      .track_id = "track-cam",
      .enabled = 1,
      .priority = 80,
      .target_layer = TURBO_ROOM_VIDEO_LAYER_MEDIUM,
      .policy_source = "layout_speaker",
  };

  check_not_null(room_server);
  check_not_null(service);
  check_equal((int)(turbo_room_service_create_room(service, &room)), (int)(0));
  check_equal((int)(turbo_room_service_add_participant(service, "room-replay", &alice)), (int)(0));
  check_equal((int)(turbo_room_service_add_participant(service, "room-replay", &bob)), (int)(0));
  check_equal((int)(turbo_room_service_publish_track(service, "room-replay", &cam)), (int)(0));
  check_equal((int)(turbo_room_service_set_subscription(service, "room-replay", &cam_sub)), (int)(0));
  check_equal((int)(turbo_room_service_assign_sfu_node(service, "room-replay",
                                                              "node-eu-1")), (int)(0));
  {
    room_service_sfu_replay_stats_t replay_stats;
    memset(&replay_stats, 0, sizeof(replay_stats));
    check_equal((int)(room_service_app_server_sync_replay_room_state(
                                 room_server, "room-replay", &replay_stats)), (int)(0));
    check_equal((int)(replay_stats.participants_replayed), (int)(2));
    check_equal((int)(replay_stats.receiver_bandwidths_replayed), (int)(2));
    check_equal((int)(replay_stats.tracks_replayed), (int)(1));
    check_equal((int)(replay_stats.subscriptions_replayed), (int)(1));
    check_equal((int)(replay_stats.skipped_items), (int)(0));
    check_equal((int)(room_service_app_server_record_room_sync(
                                 room_server, "room-replay", "replay", &replay_stats,
                                 NULL, NULL)), (int)(0));
  }
}

void test_room_service_assign_replays_existing_state_and_closed_room_diag_stays_green(void) {
  sfu_node_app_config_t sfu_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_server = NULL;
  room_service_app_server_t *room_server = NULL;
  turbo_room_service_t *service = NULL;
  json_value_t *root = NULL;
  json_value_t *diag = NULL;
  json_value_t *last_sync = NULL;
  json_value_t *last_sync_stats = NULL;
  char *diag_json = NULL;

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19331;
  sfu_config.node_id = "node-eu-1";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19332;
  room_config.node_id = "room-service-test";
  room_config.sfu_control_url = "http://127.0.0.1:19331";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  check_equal((int)(sfu_node_app_server_start(sfu_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_config.sfu_control_url, "/health", 30, 100)), (int)(0));

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  service = room_service_app_server_get_service(room_server);
  check_not_null(service);

  seed_room_runtime(room_server, service);
  app_test_sleep_ms(100);

  diag_json = room_service_http_api_build_room_diagnostic(room_server, "room-replay");
  check_not_null(diag_json);
  check_equal((int)(parse_json_text(diag_json, &root)), (int)(0));
  free(diag_json);
  diag_json = NULL;
  diag = root;
  check_not_null(diag);
  check_true(json_bool_value(diag, "runtime_sync_expected", 0));
  check_true(json_bool_value(diag, "in_sync", 0));
  check_equal((int)(json_int_value(diag, "participant_mismatch_count", -1)), (int)(0));
  check_equal((int)(json_int_value(diag, "subscription_mismatch_count", -1)), (int)(0));
  check_equal((size_t)(json_array_count(diag, "participant_bandwidth_diagnostics")), (size_t)(2));
  check_equal((size_t)(json_array_count(diag, "subscription_diagnostics")), (size_t)(1));
  last_sync = json_object_field(diag, "last_sfu_sync");
  check_not_null(last_sync);
  check_equal(json_string_value(last_sync, "operation"), "replay");
  check_false(json_bool_value(last_sync, "had_warning", 1));
  last_sync_stats = json_object_field(last_sync, "sfu_replay_stats");
  check_not_null(last_sync_stats);
  check_equal((int)(json_int_value(last_sync_stats, "participants_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(last_sync_stats, "receiver_bandwidths_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(last_sync_stats, "tracks_replayed", -1)), (int)(1));
  check_equal((int)(json_int_value(last_sync_stats, "subscriptions_replayed", -1)), (int)(1));
  check_equal((int)(json_int_value(last_sync_stats, "skipped_items", -1)), (int)(0));
  json_free(root);
  root = NULL;

  check_equal((int)(turbo_room_service_close_room(service, "room-replay")), (int)(0));
  check_equal((int)(room_service_app_server_sync_force_close_room(room_server,
                                                                         "room-replay")), (int)(0));
  app_test_sleep_ms(100);

  diag_json = room_service_http_api_build_room_diagnostic(room_server, "room-replay");
  check_not_null(diag_json);
  check_equal((int)(parse_json_text(diag_json, &root)), (int)(0));
  free(diag_json);
  diag_json = NULL;
  diag = root;
  check_not_null(diag);
  check_false(json_bool_value(diag, "runtime_sync_expected", 1));
  check_true(json_bool_value(diag, "in_sync", 0));
  check_equal((int)(json_int_value(diag, "participant_mismatch_count", -1)), (int)(0));
  check_equal((int)(json_int_value(diag, "subscription_mismatch_count", -1)), (int)(0));
  check_equal((size_t)(json_array_count(diag, "participant_bandwidth_diagnostics")), (size_t)(0));
  check_equal((size_t)(json_array_count(diag, "subscription_diagnostics")), (size_t)(0));
  last_sync = json_object_field(diag, "last_sfu_sync");
  check_not_null(last_sync);
  check_equal(json_string_value(last_sync, "operation"), "replay");
  json_free(root);
  root = NULL;

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
  sfu_node_app_server_stop(sfu_server);
  sfu_node_app_server_destroy(sfu_server);
}

void test_room_sync_diagnostic_reports_null_when_room_has_no_sync_history(void) {
  room_service_app_config_t room_config;
  room_service_app_server_t *room_server = NULL;
  turbo_room_service_t *service = NULL;
  turbo_room_config_t room = {
      .room_id = "room-no-sync",
      .room_type = TURBO_ROOM_TYPE_CONFERENCE,
      .created_by = "host-1",
  };
  json_value_t *root = NULL;
  json_value_t *diag = NULL;
  char *diag_json = NULL;

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19335;
  room_config.node_id = "room-service-test";

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  service = room_service_app_server_get_service(room_server);
  check_not_null(service);
  check_equal((int)(turbo_room_service_create_room(service, &room)), (int)(0));

  diag_json = room_service_http_api_build_room_sync_diagnostic(room_server, "room-no-sync");
  check_not_null(diag_json);
  check_equal((int)(parse_json_text(diag_json, &root)), (int)(0));
  free(diag_json);
  diag_json = NULL;
  diag = root;

  check_not_null(diag);
  check_false(json_bool_value(diag, "has_last_sfu_sync", 1));
  check_true(json_is_null_field(diag, "last_sfu_sync"));
  json_free(root);
  root = NULL;

  room_service_app_server_destroy(room_server);
}

void test_room_sync_diagnostic_http_endpoints_expose_latest_sync_state(void) {
  sfu_node_app_config_t sfu_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_server = NULL;
  room_service_app_server_t *room_server = NULL;
  turbo_room_service_t *service = NULL;
  json_value_t *get_root = NULL;
  json_value_t *post_root = NULL;
  json_value_t *post_diag = NULL;
  json_value_t *last_sync = NULL;
  json_value_t *stats = NULL;
  const char *sfu_node_base_url = "http://127.0.0.1:19351";
  const char *room_service_base_url = "http://127.0.0.1:19352";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19351;
  sfu_config.node_id = "node-eu-1";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19352;
  room_config.node_id = "room-service-test";
  room_config.sfu_control_url = "http://127.0.0.1:19351";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  check_equal((int)(sfu_node_app_server_start(sfu_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100)), (int)(0));

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  service = room_service_app_server_get_service(room_server);
  check_not_null(service);

  seed_room_runtime(room_server, service);
  app_test_sleep_ms(100);

  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30, 100)), (int)(0));

  get_root = http_get_json(room_service_base_url,
                           "/api/v1/rooms/room-replay/room_sync_diagnostic");
  check_not_null(get_root);
  check_true(json_bool_value(get_root, "has_last_sfu_sync", 0));
  last_sync = json_object_field(get_root, "last_sfu_sync");
  check_not_null(last_sync);
  check_equal(json_string_value(last_sync, "operation"), "replay");
  stats = json_object_field(last_sync, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "participants_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(1));

  post_root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{\"type\":\"get_room_sync_diagnostic\",\"room_id\":\"room-replay\"}");
  check_not_null(post_root);
  post_diag = json_object_field(post_root, "room_sync_diagnostic");
  check_not_null(post_diag);
  check_true(json_bool_value(post_diag, "has_last_sfu_sync", 0));
  last_sync = json_object_field(post_diag, "last_sfu_sync");
  check_not_null(last_sync);
  check_equal(json_string_value(last_sync, "operation"), "replay");
  stats = json_object_field(last_sync, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "participants_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "receiver_bandwidths_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "subscriptions_replayed", -1)), (int)(1));

  json_free(post_root);

  post_root = NULL;
  json_free(get_root);
  get_root = NULL;
  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
  sfu_node_app_server_stop(sfu_server);
  sfu_node_app_server_destroy(sfu_server);
}

void test_room_sync_http_assign_and_resync_results_match_room_sync_diagnostic(void) {
  sfu_node_app_config_t sfu_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_server = NULL;
  room_service_app_server_t *room_server = NULL;
  json_value_t *root = NULL;
  json_value_t *sync_diag_root = NULL;
  json_value_t *room_sync_diag = NULL;
  json_value_t *stats = NULL;
  json_value_t *last_sync = NULL;
  const char *sfu_node_base_url = "http://127.0.0.1:19361";
  const char *room_service_base_url = "http://127.0.0.1:19362";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19361;
  sfu_config.node_id = "node-eu-1";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19362;
  room_config.node_id = "room-service-test";
  room_config.sfu_control_url = "http://127.0.0.1:19361";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  check_equal((int)(sfu_node_app_server_start(sfu_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100)), (int)(0));

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30, 100)), (int)(0));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-http-sync\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-http-sync\","
      "\"participant\":{"
      "\"participant_id\":\"alice\","
      "\"user_id\":\"u-alice\","
      "\"display_name\":\"Alice\","
      "\"role\":\"host\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-http-sync\","
      "\"participant\":{"
      "\"participant_id\":\"bob\","
      "\"user_id\":\"u-bob\","
      "\"display_name\":\"Bob\","
      "\"role\":\"guest\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-http-sync\","
      "\"track\":{"
      "\"track_id\":\"track-cam\","
      "\"owner_participant_id\":\"alice\","
      "\"kind\":\"video\","
      "\"source\":\"camera\","
      "\"codec_name\":\"vp9\","
      "\"simulcast_enabled\":true,"
      "\"main_ssrc\":8195,"
      "\"layer_ssrcs\":[8193,8194,8195]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_subscription\","
      "\"room_id\":\"room-http-sync\","
      "\"subscription\":{"
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"track-cam\","
      "\"enabled\":true,"
      "\"priority\":80,"
      "\"target_layer\":\"medium\","
      "\"policy_source\":\"layout_speaker\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-http-sync\","
      "\"node_id\":\"node-eu-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  stats = json_object_field(root, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "participants_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "receiver_bandwidths_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(1));
  check_equal((int)(json_int_value(stats, "subscriptions_replayed", -1)), (int)(1));
  check_equal((int)(json_int_value(stats, "skipped_items", -1)), (int)(0));
  json_free(root);
  root = NULL;
  root = NULL;

  sync_diag_root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_sync_diagnostic\","
      "\"room_id\":\"room-http-sync\""
      "}");
  check_not_null(sync_diag_root);
  room_sync_diag = json_object_field(sync_diag_root, "room_sync_diagnostic");
  check_not_null(room_sync_diag);
  check_true(json_bool_value(room_sync_diag, "has_last_sfu_sync", 0));
  last_sync = json_object_field(room_sync_diag, "last_sfu_sync");
  check_not_null(last_sync);
  check_equal(json_string_value(last_sync, "operation"), "assign");
  stats = json_object_field(last_sync, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "participants_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "receiver_bandwidths_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(1));
  check_equal((int)(json_int_value(stats, "subscriptions_replayed", -1)), (int)(1));
  json_free(sync_diag_root);
  sync_diag_root = NULL;
  sync_diag_root = NULL;

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_receiver_bandwidth\","
      "\"room_id\":\"room-http-sync\","
      "\"participant_id\":\"bob\","
      "\"bandwidth_bps\":123000"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_track_subscription\","
      "\"room_id\":\"room-http-sync\","
      "\"receiver_participant_id\":\"bob\","
      "\"track_id\":\"track-cam\","
      "\"enabled\":false,"
      "\"max_layer\":\"none\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"resync_room\","
      "\"room_id\":\"room-http-sync\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  stats = json_object_field(root, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "participants_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "receiver_bandwidths_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(1));
  check_equal((int)(json_int_value(stats, "subscriptions_replayed", -1)), (int)(1));
  check_equal((int)(json_int_value(stats, "skipped_items", -1)), (int)(0));
  json_free(root);
  root = NULL;
  root = NULL;

  sync_diag_root = http_get_json(room_service_base_url,
                                 "/api/v1/rooms/room-http-sync/room_sync_diagnostic");
  check_not_null(sync_diag_root);
  check_true(json_bool_value(sync_diag_root, "has_last_sfu_sync", 0));
  last_sync = json_object_field(sync_diag_root, "last_sfu_sync");
  check_not_null(last_sync);
  check_equal(json_string_value(last_sync, "operation"), "resync");
  stats = json_object_field(last_sync, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "participants_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "receiver_bandwidths_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(1));
  check_equal((int)(json_int_value(stats, "subscriptions_replayed", -1)), (int)(1));
  json_free(sync_diag_root);
  sync_diag_root = NULL;

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
  sfu_node_app_server_stop(sfu_server);
  sfu_node_app_server_destroy(sfu_server);
}

void test_room_service_issues_scoped_sfu_command_tokens(void) {
  sfu_node_app_config_t sfu_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_server = NULL;
  room_service_app_server_t *room_server = NULL;
  json_value_t *root = NULL;
  json_value_t *stats = NULL;
  const char *sfu_node_base_url = "https://localhost:19418";
  const char *room_service_base_url = "http://127.0.0.1:19419";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "::1";
  sfu_config.bind_port = 19418;
  sfu_config.node_id = "node-auth-forward";
  sfu_config.control_token = "legacy-sfu-token";
  sfu_config.auth_issuer = "turbomedia";
  sfu_config.auth_active_key_id = "sfu-command-2026-07";
  sfu_config.auth_active_secret =
      "sfu-command-active-secret-at-least-32-bytes";
  sfu_config.use_tls = 1;
  sfu_config.tls_cert_file = ROOM_SERVICE_TEST_TLS_CERT_PATH;
  sfu_config.tls_key_file = ROOM_SERVICE_TEST_TLS_KEY_PATH;

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19419;
  room_config.node_id = "room-service-auth-forward";
  room_config.sfu_control_url = "https://localhost:19418";
  room_config.sfu_control_token = "wrong-legacy-token";
  room_config.sfu_ca_file = ROOM_SERVICE_TEST_TLS_CERT_PATH;
  room_config.sfu_auth_issuer = "turbomedia";
  room_config.sfu_auth_key_id = "sfu-command-2026-07";
  room_config.sfu_auth_secret =
      "sfu-command-active-secret-at-least-32-bytes";
  room_config.sfu_auth_ttl_seconds = 30;

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  check_equal((int)(sfu_node_app_server_start(sfu_server)), (int)(0));
  check_equal((int)(wait_for_https_status_ok(
             sfu_node_base_url, "/health", ROOM_SERVICE_TEST_TLS_CERT_PATH,
             30, 100)), (int)(0));

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30, 100)), (int)(0));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-auth-forward\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-auth-forward\","
      "\"node_id\":\"node-auth-forward\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  stats = json_object_field(root, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "skipped_items", -1)), (int)(0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = https_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_stats\","
      "\"room_id\":\"room-auth-forward\""
      "}",
      ROOM_SERVICE_TEST_TLS_CERT_PATH);
  check_not_null(root);
  stats = json_object_field(root, "room_stats");
  check_not_null(stats);
  check_equal(json_string_value(stats, "room_id"), "room-auth-forward");
  check_equal((int)(json_int_value(stats, "participant_count", -1)), (int)(0));
  json_free(root);
  root = NULL;

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
  sfu_node_app_server_stop(sfu_server);
  sfu_node_app_server_destroy(sfu_server);
}

void test_room_service_http_control_token_protects_modifying_commands(void) {
  room_service_app_config_t room_config;
  room_service_app_server_t *room_server = NULL;
  json_value_t *root = NULL;
  char *write_token = NULL;
  char *cross_room_token = NULL;
  char *expired_token = NULL;
  char *previous_dangerous_token = NULL;
  char *alice_token = NULL;
  char *bob_token = NULL;
  int64_t now = (int64_t)time(NULL);
  const char *room_service_base_url = "http://127.0.0.1:19420";
  const char *get_queue_depth_command =
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}";
  const char *create_room_command =
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-control-auth\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}";
  const char *create_static_room_command =
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-static-auth\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}";
  const char *close_room_command =
      "{"
      "\"type\":\"close_room\","
      "\"room_id\":\"room-control-auth\""
      "}";
  const char *join_alice_request =
      "{"
      "\"room_id\":\"room-static-auth\","
      "\"participant\":{"
      "\"participant_id\":\"alice\","
      "\"user_id\":\"u-alice\","
      "\"display_name\":\"Alice\","
      "\"role\":\"guest\""
      "}"
      "}";
  const char *peek_queue_command =
      "{"
      "\"type\":\"peek_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"caller\""
      "}";
  const char *pop_queue_command =
      "{"
      "\"type\":\"pop_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"caller\""
      "}";
  const char *unknown_get_command =
      "{"
      "\"type\":\"get_mutating_like_name\""
      "}";
  const char *replay_iris_dead_letters_command =
      "{"
      "\"type\":\"replay_iris_dead_letters\","
      "\"limit\":16"
      "}";
  const char *replay_iris_dead_letters_invalid_limit_command =
      "{"
      "\"type\":\"replay_iris_dead_letters\","
      "\"limit\":257"
      "}";
  const char *list_iris_archived_events_command =
      "{"
      "\"type\":\"list_iris_archived_events\","
      "\"limit\":16"
      "}";
  const char *run_iris_event_retention_command =
      "{"
      "\"type\":\"run_iris_event_retention\""
      "}";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19420;
  room_config.node_id = "room-service-control-auth";
  room_config.control_token = "room-service-control-token";
  room_config.auth_active_key_id = "room-control-2026-07";
  room_config.auth_active_secret =
      "room-control-active-secret-at-least-32-bytes";
  room_config.auth_previous_key_id = "room-control-2026-06";
  room_config.auth_previous_secret =
      "room-control-previous-secret-at-least-32-bytes";

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30, 100)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/metrics", 30, 100)), (int)(0));
  write_token = issue_room_control_token(
      "room-control-2026-07",
      "room-control-active-secret-at-least-32-bytes",
      "room.control.write", "room-control-auth", NULL, now, now + 60);
  cross_room_token = issue_room_control_token(
      "room-control-2026-07",
      "room-control-active-secret-at-least-32-bytes",
      "room.control.write", "another-room", NULL, now, now + 60);
  expired_token = issue_room_control_token(
      "room-control-2026-07",
      "room-control-active-secret-at-least-32-bytes",
      "room.control.write", "room-control-auth", NULL, now - 120, now - 60);
  previous_dangerous_token = issue_room_control_token(
      "room-control-2026-06",
      "room-control-previous-secret-at-least-32-bytes",
      "room.control.dangerous", "room-control-auth", NULL, now, now + 60);
  alice_token = issue_room_control_token(
      "room-control-2026-07",
      "room-control-active-secret-at-least-32-bytes",
      "room.control.write", "room-static-auth", "alice", now, now + 60);
  bob_token = issue_room_control_token(
      "room-control-2026-07",
      "room-control-active-secret-at-least-32-bytes",
      "room.control.write", "room-static-auth", "bob", now, now + 60);
  check_not_null(write_token);
  check_not_null(cross_room_token);
  check_not_null(expired_token);
  check_not_null(previous_dangerous_token);
  check_not_null(alice_token);
  check_not_null(bob_token);

  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 get_queue_depth_command, NULL, NULL)), (int)(200));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 peek_queue_command, NULL, NULL)), (int)(404));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 pop_queue_command, NULL, NULL)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 unknown_get_command, NULL, NULL)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_room_command, NULL, NULL)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_room_command, "wrong-token", NULL)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_room_command, cross_room_token, NULL)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_room_command, expired_token, NULL)), (int)(401));

  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_room_command, write_token, &root)), (int)(200));
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 close_room_command, write_token, NULL)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 replay_iris_dead_letters_command,
                                 write_token, NULL)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 list_iris_archived_events_command,
                                 write_token, NULL)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 run_iris_event_retention_command,
                                 write_token, NULL)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 replay_iris_dead_letters_command,
                                 "room-service-control-token", NULL)), (int)(404));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 list_iris_archived_events_command,
                                 "room-service-control-token", NULL)), (int)(404));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 run_iris_event_retention_command,
                                 "room-service-control-token", NULL)), (int)(404));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 replay_iris_dead_letters_invalid_limit_command,
                                 "room-service-control-token", NULL)), (int)(400));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 close_room_command, previous_dangerous_token, NULL)), (int)(200));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_static_room_command,
                                 "room-service-control-token", &root)), (int)(200));
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/join",
                                 join_alice_request, bob_token, NULL)), (int)(401));
  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/join",
                                 join_alice_request, alice_token, NULL)), (int)(200));

  free(bob_token);
  free(alice_token);
  free(previous_dangerous_token);
  free(expired_token);
  free(cross_room_token);
  free(write_token);

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
}

void test_room_service_facade_join_publish_and_subscribe(void) {
  room_service_app_config_t room_config;
  room_service_app_server_t *room_server = NULL;
  json_value_t *root = NULL;
  json_value_t *room = NULL;
  json_value_t *participant = NULL;
  json_value_t *track = NULL;
  json_value_t *subscription = NULL;
  const char *room_service_base_url = "http://127.0.0.1:19422";
  const char *join_alice =
      "{"
      "\"room_id\":\"room-facade\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"alice\","
      "\"participant\":{"
      "\"participant_id\":\"alice\","
      "\"user_id\":\"u-alice\","
      "\"display_name\":\"Alice\","
      "\"role\":\"host\""
      "}"
      "}";
  const char *join_bob =
      "{"
      "\"room_id\":\"room-facade\","
      "\"participant\":{"
      "\"participant_id\":\"bob\","
      "\"user_id\":\"u-bob\","
      "\"display_name\":\"Bob\","
      "\"role\":\"guest\""
      "}"
      "}";
  const char *publish_track =
      "{"
      "\"room_id\":\"room-facade\","
      "\"track\":{"
      "\"track_id\":\"track-cam\","
      "\"owner_participant_id\":\"alice\","
      "\"kind\":\"video\","
      "\"source\":\"camera\","
      "\"codec_name\":\"vp8\","
      "\"main_ssrc\":1234"
      "}"
      "}";
  const char *subscribe_track =
      "{"
      "\"room_id\":\"room-facade\","
      "\"subscription\":{"
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"track-cam\","
      "\"enabled\":true,"
      "\"preferred_layer\":\"high\","
      "\"policy_source\":\"facade\""
      "}"
      "}";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19422;
  room_config.node_id = "room-service-facade";
  room_config.control_token = "room-service-control-token";

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30, 100)), (int)(0));

  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/join",
                                 join_alice, NULL, NULL)), (int)(401));

  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/join",
                                 join_alice, "room-service-control-token", &root)), (int)(200));
  check_not_null(root);
  room = json_object_field(root, "room");
  participant = json_object_field(root, "participant");
  check_not_null(room);
  check_not_null(participant);
  check_equal(json_string_value(room, "room_id"), "room-facade");
  check_equal(json_string_value(participant, "participant_id"), "alice");
  check_equal((int)(json_int_value(room, "participant_count", 0)), (int)(1));
  json_free(root);
  root = NULL;

  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/join",
                                 join_bob, "room-service-control-token", &root)), (int)(200));
  check_not_null(root);
  room = json_object_field(root, "room");
  participant = json_object_field(root, "participant");
  check_not_null(room);
  check_not_null(participant);
  check_equal(json_string_value(participant, "participant_id"), "bob");
  check_equal((int)(json_int_value(room, "participant_count", 0)), (int)(2));
  json_free(root);
  root = NULL;

  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/publish",
                                 publish_track, "room-service-control-token", &root)), (int)(200));
  check_not_null(root);
  room = json_object_field(root, "room");
  track = json_object_field(root, "track");
  check_not_null(room);
  check_not_null(track);
  check_equal(json_string_value(track, "track_id"), "track-cam");
  check_equal((int)(json_int_value(room, "published_track_count", 0)), (int)(1));
  json_free(root);
  root = NULL;

  check_equal((int)(http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/subscribe",
                                 subscribe_track, "room-service-control-token", &root)), (int)(200));
  check_not_null(root);
  room = json_object_field(root, "room");
  subscription = json_object_field(root, "subscription");
  check_not_null(room);
  check_not_null(subscription);
  check_equal(json_string_value(subscription, "subscriber_participant_id"), "bob");
  check_equal(json_string_value(subscription, "track_id"), "track-cam");
  check_true(json_bool_value(subscription, "enabled", 0));
  check_equal((int)(json_int_value(room, "subscription_count", 0)), (int)(1));
  json_free(root);
  root = NULL;

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
}

void test_room_service_revocation_fanout_tracks_live_sfu_membership(void) {
  static const char digest_zero[] =
      "0000000000000000000000000000000000000000000000000000000000000000";
  static const char digest_one[] =
      "1111111111111111111111111111111111111111111111111111111111111111";
  const char *covering_one[] = {digest_zero};
  const char *covering_two[] = {digest_zero, digest_one};
  sfu_node_app_config_t sfu_a_config;
  sfu_node_app_config_t sfu_b_config;
  sfu_node_app_config_t sfu_c_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_a = NULL;
  sfu_node_app_server_t *sfu_b = NULL;
  sfu_node_app_server_t *sfu_c = NULL;
  room_service_app_server_t *room = NULL;
  turbo_media_revocation_fanout_report_t report;
  int synchronized = 0;
  uint64_t epoch = 0U;
  uint64_t sequence = 0U;
  size_t count = 0U;
  uint64_t membership_before;

  sfu_node_app_config_init(&sfu_a_config);
  sfu_a_config.bind_host = "127.0.0.1";
  sfu_a_config.bind_port = 19440;
  sfu_a_config.node_id = "sfu-rev-a";
  sfu_a_config.use_tls = 1;
  sfu_a_config.tls_cert_file = ROOM_SERVICE_TEST_TLS_CERT_PATH;
  sfu_a_config.tls_key_file = ROOM_SERVICE_TEST_TLS_KEY_PATH;
  sfu_a_config.auth_issuer = "turbomedia";
  sfu_a_config.auth_active_key_id = "sfu-security-2026-09";
  sfu_a_config.auth_active_secret =
      "sfu-security-secret-at-least-32-bytes";
  sfu_a_config.auth_dynamic_revocation_capacity = 8;
  sfu_a_config.auth_max_ttl_seconds = 120;
  sfu_a_config.ice_allow_loopback = 1;

  sfu_b_config = sfu_a_config;
  sfu_b_config.bind_port = 19441;
  sfu_b_config.node_id = "sfu-rev-b";
  sfu_c_config = sfu_a_config;
  sfu_c_config.bind_port = 19442;
  sfu_c_config.node_id = "sfu-rev-c";

  sfu_a = sfu_node_app_server_create(&sfu_a_config);
  sfu_b = sfu_node_app_server_create(&sfu_b_config);
  check_not_null(sfu_a);
  check_not_null(sfu_b);
  check_equal(sfu_node_app_server_start(sfu_a), 0);
  check_equal(sfu_node_app_server_start(sfu_b), 0);
  check_equal(wait_for_https_status_ok(
                  "https://127.0.0.1:19440", "/health",
                  ROOM_SERVICE_TEST_TLS_CERT_PATH, 30, 100), 0);
  check_equal(wait_for_https_status_ok(
                  "https://127.0.0.1:19441", "/health",
                  ROOM_SERVICE_TEST_TLS_CERT_PATH, 30, 100), 0);

  room_service_app_config_init(&room_config);
  room_config.sfu_nodes =
      "sfu-rev-a=https://127.0.0.1:19440,"
      "sfu-rev-b=https://127.0.0.1:19441";
  room_config.sfu_revocation_server_names =
      "sfu-rev-a=localhost,sfu-rev-b=localhost";
  room_config.sfu_ca_file = ROOM_SERVICE_TEST_TLS_CERT_PATH;
  room_config.sfu_auth_issuer = "turbomedia";
  room_config.sfu_auth_key_id = "sfu-security-2026-09";
  room_config.sfu_auth_secret =
      "sfu-security-secret-at-least-32-bytes";
  room_config.sfu_auth_ttl_seconds = 30;
  room_config.sfu_revocation_timeout_ms = 3000;
  room_config.sfu_revocation_max_attempts = 2;

  room = room_service_app_server_create(&room_config);
  check_not_null(room);
  membership_before =
      room_service_app_server_sfu_membership_version(room);
  check_true(membership_before >= 2U);

  memset(&report, 0, sizeof(report));
  check_equal(room_service_app_server_publish_sfu_revocation_snapshot(
                  room, 1U, 0U, NULL, 0U, &report), 0);
  check_equal((int)report.target_count, 2);
  check_equal((int)report.synchronized_count, 2);

  memset(&report, 0, sizeof(report));
  check_equal(room_service_app_server_publish_sfu_revocation(
                  room, 1U, 1U, digest_zero,
                  covering_one, 1U, &report), 0);
  check_equal((int)report.target_count, 2);
  check_equal((int)report.synchronized_count, 2);

  check_equal(sfu_node_app_server_get_revocation_status(
                  sfu_a, &synchronized, &epoch, &sequence, &count), 0);
  check_true(synchronized);
  check_equal((int)epoch, 1);
  check_equal((int)sequence, 1);
  check_equal((int)count, 1);
  check_equal(sfu_node_app_server_get_revocation_status(
                  sfu_b, &synchronized, &epoch, &sequence, &count), 0);
  check_true(synchronized);
  check_equal((int)sequence, 1);
  check_equal((int)count, 1);

  sfu_c = sfu_node_app_server_create(&sfu_c_config);
  check_not_null(sfu_c);
  check_equal(sfu_node_app_server_start(sfu_c), 0);
  check_equal(wait_for_https_status_ok(
                  "https://127.0.0.1:19442", "/health",
                  ROOM_SERVICE_TEST_TLS_CERT_PATH, 30, 100), 0);
  check_equal(room_service_app_server_register_sfu_node_secure(
                  room, "sfu-rev-c", "https://127.0.0.1:19442",
                  NULL, "localhost"), 0);
  check_true(room_service_app_server_sfu_membership_version(room) >
             membership_before);

  /*
   * The target set changed. A malformed covering snapshot must be rejected
   * before rebuilding/sending anything, so existing targets stay at seq=1
   * and the newly added target stays UNKNOWN.
   */
  memset(&report, 0, sizeof(report));
  check_equal(room_service_app_server_publish_sfu_revocation(
                  room, 1U, 2U, digest_one,
                  covering_one, 1U, &report), -1);
  check_equal(sfu_node_app_server_get_revocation_status(
                  sfu_a, &synchronized, &epoch, &sequence, &count), 0);
  check_true(synchronized);
  check_equal((int)sequence, 1);
  check_equal((int)count, 1);
  check_equal(sfu_node_app_server_get_revocation_status(
                  sfu_c, &synchronized, &epoch, &sequence, &count), 0);
  check_false(synchronized);

  /*
   * With a valid covering snapshot, sequence-2 rebuilds the adapter and
   * reconciles all three targets instead of sending an incremental event to
   * the newly UNKNOWN node.
   */
  memset(&report, 0, sizeof(report));
  check_equal(room_service_app_server_publish_sfu_revocation(
                  room, 1U, 2U, digest_one,
                  covering_two, 2U, &report), 0);
  check_equal((int)report.target_count, 3);
  check_equal((int)report.synchronized_count, 3);

  check_equal(sfu_node_app_server_get_revocation_status(
                  sfu_a, &synchronized, &epoch, &sequence, &count), 0);
  check_true(synchronized);
  check_equal((int)sequence, 2);
  check_equal((int)count, 2);
  check_equal(sfu_node_app_server_get_revocation_status(
                  sfu_b, &synchronized, &epoch, &sequence, &count), 0);
  check_true(synchronized);
  check_equal((int)sequence, 2);
  check_equal((int)count, 2);
  check_equal(sfu_node_app_server_get_revocation_status(
                  sfu_c, &synchronized, &epoch, &sequence, &count), 0);
  check_true(synchronized);
  check_equal((int)epoch, 1);
  check_equal((int)sequence, 2);
  check_equal((int)count, 2);

  check_equal(room_service_app_server_destroy(room), 0);
  sfu_node_app_server_stop(sfu_c);
  sfu_node_app_server_destroy(sfu_c);
  sfu_node_app_server_stop(sfu_b);
  sfu_node_app_server_destroy(sfu_b);
  sfu_node_app_server_stop(sfu_a);
  sfu_node_app_server_destroy(sfu_a);
}

void test_room_service_routes_rooms_to_registered_sfu_nodes(void) {
  sfu_node_app_config_t sfu_a_config;
  sfu_node_app_config_t sfu_b_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_a_server = NULL;
  sfu_node_app_server_t *sfu_b_server = NULL;
  room_service_app_server_t *room_server = NULL;
  json_value_t *root = NULL;
  json_value_t *room = NULL;
  json_value_t *node_stats = NULL;
  const char *sfu_a_base_url = "http://127.0.0.1:19431";
  const char *sfu_b_base_url = "http://127.0.0.1:19432";
  const char *room_service_base_url = "http://127.0.0.1:19433";

  sfu_node_app_config_init(&sfu_a_config);
  sfu_a_config.bind_host = "0.0.0.0";
  sfu_a_config.bind_port = 19431;
  sfu_a_config.node_id = "sfu-a";

  sfu_node_app_config_init(&sfu_b_config);
  sfu_b_config.bind_host = "0.0.0.0";
  sfu_b_config.bind_port = 19432;
  sfu_b_config.node_id = "sfu-b";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19433;
  room_config.node_id = "room-service-registry";
  room_config.sfu_nodes =
      "sfu-a=http://127.0.0.1:19431,sfu-b=http://127.0.0.1:19432";

  sfu_a_server = sfu_node_app_server_create(&sfu_a_config);
  check_not_null(sfu_a_server);
  check_equal((int)(sfu_node_app_server_start(sfu_a_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_a_base_url, "/health", 30, 100)), (int)(0));

  sfu_b_server = sfu_node_app_server_create(&sfu_b_config);
  check_not_null(sfu_b_server);
  check_equal((int)(sfu_node_app_server_start(sfu_b_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_b_base_url, "/health", 30, 100)), (int)(0));

  root = http_get_json(sfu_a_base_url, "/health");
  check_not_null(root);
  node_stats = json_object_field(root, "node_stats");
  check_not_null(node_stats);
  check_equal(json_string_value(node_stats, "node_id"), "sfu-a");
  json_free(root);
  root = NULL;

  root = http_get_json(sfu_b_base_url, "/health");
  check_not_null(root);
  node_stats = json_object_field(root, "node_stats");
  check_not_null(node_stats);
  check_equal(json_string_value(node_stats, "node_id"), "sfu-b");
  json_free(root);
  root = NULL;

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  check_true(room_service_app_server_has_sfu_node(room_server, "sfu-a"));
  check_true(room_service_app_server_has_sfu_node(room_server, "sfu-b"));
  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30, 100)), (int)(0));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-on-a\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-a\""
      "}");
  check_not_null(root);
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-on-b\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-b\""
      "}");
  check_not_null(root);
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-on-a\""
      "}");
  check_not_null(root);
  room = json_object_field(root, "room");
  check_not_null(room);
  check_equal(json_string_value(room, "assigned_sfu_node"), "sfu-a");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-on-b\""
      "}");
  check_not_null(root);
  room = json_object_field(root, "room");
  check_not_null(room);
  check_equal(json_string_value(room, "assigned_sfu_node"), "sfu-b");
  json_free(root);
  root = NULL;

  check_equal((int)(http_post_json_status(
                                 sfu_a_base_url, "/api/v1/commands",
                                 "{"
                                 "\"type\":\"get_room_stats\","
                                 "\"room_id\":\"room-on-a\""
                                 "}",
                                 NULL)), (int)(200));
  check_equal((int)(http_post_json_status(
                                 sfu_a_base_url, "/api/v1/commands",
                                 "{"
                                 "\"type\":\"get_room_stats\","
                                 "\"room_id\":\"room-on-b\""
                                 "}",
                                 NULL)), (int)(404));
  check_equal((int)(http_post_json_status(
                                 sfu_b_base_url, "/api/v1/commands",
                                 "{"
                                 "\"type\":\"get_room_stats\","
                                 "\"room_id\":\"room-on-a\""
                                 "}",
                                 NULL)), (int)(404));
  check_equal((int)(http_post_json_status(
                                 sfu_b_base_url, "/api/v1/commands",
                                 "{"
                                 "\"type\":\"get_room_stats\","
                                 "\"room_id\":\"room-on-b\""
                                 "}",
                                 NULL)), (int)(200));

  check_equal((int)(http_post_json_status(
                                 room_service_base_url, "/api/v1/commands",
                                 "{"
                                 "\"type\":\"assign_sfu_node\","
                                 "\"room_id\":\"room-on-a\","
                                 "\"node_id\":\"missing-sfu\""
                                 "}",
                                 NULL)), (int)(404));

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
  sfu_node_app_server_stop(sfu_a_server);
  sfu_node_app_server_destroy(sfu_a_server);
  sfu_node_app_server_stop(sfu_b_server);
  sfu_node_app_server_destroy(sfu_b_server);
}

void test_room_service_config_reads_control_tokens_from_env(void) {
  room_service_app_config_t room_config;
  char *saved_control_token = app_test_save_env("TURBO_ROOM_SERVICE_CONTROL_TOKEN");
  char *saved_sfu_control_token =
      app_test_save_env("TURBO_ROOM_SERVICE_SFU_CONTROL_TOKEN");
  char *saved_sfu_nodes = app_test_save_env("TURBO_ROOM_SERVICE_SFU_NODES");
  char *saved_sfu_revocation_names =
      app_test_save_env("TURBO_ROOM_SERVICE_SFU_REVOCATION_SERVER_NAMES");
  char *saved_sfu_revocation_timeout =
      app_test_save_env("TURBO_ROOM_SERVICE_SFU_REVOCATION_TIMEOUT_MS");
  char *saved_sfu_revocation_attempts =
      app_test_save_env("TURBO_ROOM_SERVICE_SFU_REVOCATION_MAX_ATTEMPTS");
  char *saved_use_tls = app_test_save_env("TURBO_ROOM_SERVICE_USE_TLS");
  char *saved_tls_cert =
      app_test_save_env("TURBO_ROOM_SERVICE_TLS_CERT_FILE");
  char *saved_tls_key =
      app_test_save_env("TURBO_ROOM_SERVICE_TLS_KEY_FILE");
  char *saved_sfu_ca =
      app_test_save_env("TURBO_ROOM_SERVICE_SFU_CA_FILE");
  char *saved_auth_key_id =
      app_test_save_env("TURBO_ROOM_SERVICE_AUTH_ACTIVE_KEY_ID");
  char *saved_auth_secret =
      app_test_save_env("TURBO_ROOM_SERVICE_AUTH_ACTIVE_SECRET");
  char *saved_sfu_auth_key_id =
      app_test_save_env("TURBO_ROOM_SERVICE_SFU_AUTH_KEY_ID");
  char *saved_sfu_auth_secret =
      app_test_save_env("TURBO_ROOM_SERVICE_SFU_AUTH_SECRET");
  char *saved_sfu_auth_ttl =
      app_test_save_env("TURBO_ROOM_SERVICE_SFU_AUTH_TTL_SECONDS");

  app_test_set_env("TURBO_ROOM_SERVICE_CONTROL_TOKEN", "env-room-control-token");
  app_test_set_env("TURBO_ROOM_SERVICE_SFU_CONTROL_TOKEN", "env-sfu-control-token");
  app_test_set_env("TURBO_ROOM_SERVICE_SFU_NODES",
                   "node-a=https://127.0.0.1:19423,node-b=https://127.0.0.1:19424");
  app_test_set_env("TURBO_ROOM_SERVICE_SFU_REVOCATION_SERVER_NAMES",
                   "node-a=localhost,node-b=localhost");
  app_test_set_env("TURBO_ROOM_SERVICE_SFU_REVOCATION_TIMEOUT_MS", "2500");
  app_test_set_env("TURBO_ROOM_SERVICE_SFU_REVOCATION_MAX_ATTEMPTS", "4");
  app_test_set_env("TURBO_ROOM_SERVICE_USE_TLS", "true");
  app_test_set_env("TURBO_ROOM_SERVICE_TLS_CERT_FILE",
                   ROOM_SERVICE_TEST_TLS_CERT_PATH);
  app_test_set_env("TURBO_ROOM_SERVICE_TLS_KEY_FILE",
                   ROOM_SERVICE_TEST_TLS_KEY_PATH);
  app_test_set_env("TURBO_ROOM_SERVICE_SFU_CA_FILE",
                   ROOM_SERVICE_TEST_TLS_CERT_PATH);
  app_test_set_env("TURBO_ROOM_SERVICE_AUTH_ACTIVE_KEY_ID",
                   "env-room-key");
  app_test_set_env("TURBO_ROOM_SERVICE_AUTH_ACTIVE_SECRET",
                   "env-room-active-secret-at-least-32-bytes");
  app_test_set_env("TURBO_ROOM_SERVICE_SFU_AUTH_KEY_ID",
                   "env-sfu-key");
  app_test_set_env("TURBO_ROOM_SERVICE_SFU_AUTH_SECRET",
                   "env-sfu-command-secret-at-least-32-bytes");
  app_test_set_env("TURBO_ROOM_SERVICE_SFU_AUTH_TTL_SECONDS", "45");

  room_service_app_config_init(&room_config);
  room_service_app_config_apply_environment(&room_config);
  check_equal(room_config.control_token, "env-room-control-token");
  check_equal(room_config.sfu_control_token, "env-sfu-control-token");
  check_equal(room_config.sfu_nodes, "node-a=https://127.0.0.1:19423,node-b=https://127.0.0.1:19424");
  check_equal(room_config.sfu_revocation_server_names,
              "node-a=localhost,node-b=localhost");
  check_equal((int)(room_config.sfu_revocation_timeout_ms), (int)(2500));
  check_equal((int)(room_config.sfu_revocation_max_attempts), (int)(4));
  check_equal((int)(room_config.use_tls), (int)(1));
  check_equal(room_config.tls_cert_file, ROOM_SERVICE_TEST_TLS_CERT_PATH);
  check_equal(room_config.tls_key_file, ROOM_SERVICE_TEST_TLS_KEY_PATH);
  check_equal(room_config.sfu_ca_file, ROOM_SERVICE_TEST_TLS_CERT_PATH);
  check_equal(room_config.auth_active_key_id, "env-room-key");
  check_equal(room_config.auth_active_secret, "env-room-active-secret-at-least-32-bytes");
  check_equal(room_config.sfu_auth_key_id, "env-sfu-key");
  check_equal(room_config.sfu_auth_secret, "env-sfu-command-secret-at-least-32-bytes");
  check_equal((int)(room_config.sfu_auth_ttl_seconds), (int)(45));
  check_equal((int)(room_service_app_config_validate(&room_config)), (int)(0));

  app_test_restore_env("TURBO_ROOM_SERVICE_CONTROL_TOKEN", saved_control_token);
  app_test_restore_env("TURBO_ROOM_SERVICE_SFU_CONTROL_TOKEN", saved_sfu_control_token);
  app_test_restore_env("TURBO_ROOM_SERVICE_SFU_NODES", saved_sfu_nodes);
  app_test_restore_env("TURBO_ROOM_SERVICE_SFU_REVOCATION_SERVER_NAMES",
                       saved_sfu_revocation_names);
  app_test_restore_env("TURBO_ROOM_SERVICE_SFU_REVOCATION_TIMEOUT_MS",
                       saved_sfu_revocation_timeout);
  app_test_restore_env("TURBO_ROOM_SERVICE_SFU_REVOCATION_MAX_ATTEMPTS",
                       saved_sfu_revocation_attempts);
  app_test_restore_env("TURBO_ROOM_SERVICE_USE_TLS", saved_use_tls);
  app_test_restore_env("TURBO_ROOM_SERVICE_TLS_CERT_FILE", saved_tls_cert);
  app_test_restore_env("TURBO_ROOM_SERVICE_TLS_KEY_FILE", saved_tls_key);
  app_test_restore_env("TURBO_ROOM_SERVICE_SFU_CA_FILE", saved_sfu_ca);
  app_test_restore_env("TURBO_ROOM_SERVICE_AUTH_ACTIVE_KEY_ID",
                       saved_auth_key_id);
  app_test_restore_env("TURBO_ROOM_SERVICE_AUTH_ACTIVE_SECRET",
                       saved_auth_secret);
  app_test_restore_env("TURBO_ROOM_SERVICE_SFU_AUTH_KEY_ID",
                       saved_sfu_auth_key_id);
  app_test_restore_env("TURBO_ROOM_SERVICE_SFU_AUTH_SECRET",
                       saved_sfu_auth_secret);
  app_test_restore_env("TURBO_ROOM_SERVICE_SFU_AUTH_TTL_SECONDS",
                       saved_sfu_auth_ttl);
}

void test_room_sync_http_warning_paths_surface_skipped_replay_state(void) {
  sfu_node_app_config_t sfu_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_server = NULL;
  room_service_app_server_t *room_server = NULL;
  json_value_t *root = NULL;
  json_value_t *sync_diag_root = NULL;
  json_value_t *room_sync_diag = NULL;
  json_value_t *stats = NULL;
  json_value_t *last_sync = NULL;
  const char *sfu_node_base_url = "http://127.0.0.1:19371";
  const char *room_service_base_url = "http://127.0.0.1:19372";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19371;
  sfu_config.node_id = "node-eu-1";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19372;
  room_config.node_id = "room-service-test";
  room_config.sfu_control_url = "http://127.0.0.1:19371";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  check_equal((int)(sfu_node_app_server_start(sfu_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100)), (int)(0));

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30, 100)), (int)(0));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-http-warning\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-http-warning\","
      "\"participant\":{"
      "\"participant_id\":\"alice\","
      "\"user_id\":\"u-alice\","
      "\"display_name\":\"Alice\","
      "\"role\":\"host\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-http-warning\","
      "\"participant\":{"
      "\"participant_id\":\"bob\","
      "\"user_id\":\"u-bob\","
      "\"display_name\":\"Bob\","
      "\"role\":\"guest\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-http-warning\","
      "\"track\":{"
      "\"track_id\":\"track-cam\","
      "\"owner_participant_id\":\"alice\","
      "\"kind\":\"video\","
      "\"source\":\"camera\","
      "\"codec_name\":\"vp9\","
      "\"simulcast_enabled\":false"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_null(json_string_value(root, "warning_code"));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_subscription\","
      "\"room_id\":\"room-http-warning\","
      "\"subscription\":{"
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"track-cam\","
      "\"enabled\":true,"
      "\"priority\":80,"
      "\"target_layer\":\"medium\","
      "\"policy_source\":\"layout_speaker\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_null(json_string_value(root, "warning_code"));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-http-warning\","
      "\"node_id\":\"node-eu-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal(json_string_value(root, "warning_code"), "SFU_SYNC_SKIPPED");
  stats = json_object_field(root, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "participants_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "receiver_bandwidths_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "subscriptions_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "skipped_items", -1)), (int)(2));
  json_free(root);
  root = NULL;
  root = NULL;

  sync_diag_root = http_get_json(room_service_base_url,
                                 "/api/v1/rooms/room-http-warning/room_sync_diagnostic");
  check_not_null(sync_diag_root);
  check_true(json_bool_value(sync_diag_root, "has_last_sfu_sync", 0));
  last_sync = json_object_field(sync_diag_root, "last_sfu_sync");
  check_not_null(last_sync);
  check_equal(json_string_value(last_sync, "operation"), "assign");
  check_true(json_bool_value(last_sync, "had_warning", 0));
  check_equal(json_string_value(last_sync, "warning_code"), "SFU_SYNC_SKIPPED");
  stats = json_object_field(last_sync, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "subscriptions_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "skipped_items", -1)), (int)(2));
  json_free(sync_diag_root);
  sync_diag_root = NULL;
  sync_diag_root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"resync_room\","
      "\"room_id\":\"room-http-warning\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal(json_string_value(root, "warning_code"), "SFU_SYNC_SKIPPED");
  stats = json_object_field(root, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "participants_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "receiver_bandwidths_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "subscriptions_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "skipped_items", -1)), (int)(2));
  json_free(root);
  root = NULL;
  root = NULL;

  sync_diag_root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_sync_diagnostic\","
      "\"room_id\":\"room-http-warning\""
      "}");
  check_not_null(sync_diag_root);
  room_sync_diag = json_object_field(sync_diag_root, "room_sync_diagnostic");
  check_not_null(room_sync_diag);
  check_true(json_bool_value(room_sync_diag, "has_last_sfu_sync", 0));
  last_sync = json_object_field(room_sync_diag, "last_sfu_sync");
  check_not_null(last_sync);
  check_equal(json_string_value(last_sync, "operation"), "resync");
  check_true(json_bool_value(last_sync, "had_warning", 0));
  check_equal(json_string_value(last_sync, "warning_code"), "SFU_SYNC_SKIPPED");
  stats = json_object_field(last_sync, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "subscriptions_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "skipped_items", -1)), (int)(2));
  json_free(sync_diag_root);
  sync_diag_root = NULL;

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
  sfu_node_app_server_stop(sfu_server);
  sfu_node_app_server_destroy(sfu_server);
}

void test_room_sync_http_failed_replay_is_reflected_in_room_sync_diagnostic(
    void) {
  room_service_app_config_t room_config;
  room_service_app_server_t *room_server = NULL;
  json_value_t *root = NULL;
  json_value_t *sync_diag_root = NULL;
  json_value_t *stats = NULL;
  json_value_t *last_sync = NULL;
  const char *room_service_base_url = "http://127.0.0.1:19382";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19382;
  room_config.node_id = "room-service-test";
  room_config.sfu_control_url = "http://127.0.0.1:19381";

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30, 100)), (int)(0));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-http-failed\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-http-failed\","
      "\"participant\":{"
      "\"participant_id\":\"alice\","
      "\"user_id\":\"u-alice\","
      "\"display_name\":\"Alice\","
      "\"role\":\"host\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-http-failed\","
      "\"participant\":{"
      "\"participant_id\":\"bob\","
      "\"user_id\":\"u-bob\","
      "\"display_name\":\"Bob\","
      "\"role\":\"guest\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-http-failed\","
      "\"track\":{"
      "\"track_id\":\"track-cam\","
      "\"owner_participant_id\":\"alice\","
      "\"kind\":\"video\","
      "\"source\":\"camera\","
      "\"codec_name\":\"vp9\","
      "\"simulcast_enabled\":true,"
      "\"main_ssrc\":8195,"
      "\"layer_ssrcs\":[8193,8194,8195]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_subscription\","
      "\"room_id\":\"room-http-failed\","
      "\"subscription\":{"
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"track-cam\","
      "\"enabled\":true,"
      "\"priority\":80,"
      "\"target_layer\":\"medium\","
      "\"policy_source\":\"layout_speaker\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-http-failed\","
      "\"node_id\":\"node-eu-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal(json_string_value(root, "warning_code"), "SFU_SYNC_FAILED");
  stats = json_object_field(root, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "participants_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "receiver_bandwidths_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "subscriptions_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "skipped_items", -1)), (int)(0));
  json_free(root);
  root = NULL;
  root = NULL;

  sync_diag_root = http_get_json(room_service_base_url,
                                 "/api/v1/rooms/room-http-failed/room_sync_diagnostic");
  check_not_null(sync_diag_root);
  check_true(json_bool_value(sync_diag_root, "has_last_sfu_sync", 0));
  last_sync = json_object_field(sync_diag_root, "last_sfu_sync");
  check_not_null(last_sync);
  check_equal(json_string_value(last_sync, "operation"), "assign");
  check_true(json_bool_value(last_sync, "had_warning", 0));
  check_equal(json_string_value(last_sync, "warning_code"), "SFU_SYNC_FAILED");
  stats = json_object_field(last_sync, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "participants_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "receiver_bandwidths_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "subscriptions_replayed", -1)), (int)(0));
  check_equal((int)(json_int_value(stats, "skipped_items", -1)), (int)(0));
  json_free(sync_diag_root);
  sync_diag_root = NULL;

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
}

void test_room_service_resync_room_repairs_sfu_drift(void) {
  sfu_node_app_config_t sfu_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_server = NULL;
  room_service_app_server_t *room_server = NULL;
  turbo_room_service_t *service = NULL;
  turbo_sfu_node_t *node = NULL;
  json_value_t *root = NULL;
  json_value_t *diag = NULL;
  json_value_t *last_sync = NULL;
  json_value_t *last_sync_stats = NULL;
  char *diag_json = NULL;
  room_service_sfu_replay_stats_t replay_stats;

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19341;
  sfu_config.node_id = "node-eu-1";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19342;
  room_config.node_id = "room-service-test";
  room_config.sfu_control_url = "http://127.0.0.1:19341";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  check_equal((int)(sfu_node_app_server_start(sfu_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_config.sfu_control_url, "/health", 30, 100)), (int)(0));

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  service = room_service_app_server_get_service(room_server);
  check_not_null(service);
  node = sfu_node_app_server_get_node(sfu_server);
  check_not_null(node);

  seed_room_runtime(room_server, service);
  app_test_sleep_ms(100);

  check_equal((int)(turbo_sfu_node_set_receiver_bandwidth(node, "room-replay", "bob",
                                                                 123000)), (int)(0));
  check_equal((int)(turbo_sfu_node_set_track_subscription(
                               node, "room-replay", "bob", "track-cam", 0,
                               TURBO_ROOM_VIDEO_LAYER_NONE)), (int)(0));

  diag_json = room_service_http_api_build_room_diagnostic(room_server, "room-replay");
  check_not_null(diag_json);
  check_equal((int)(parse_json_text(diag_json, &root)), (int)(0));
  free(diag_json);
  diag_json = NULL;
  diag = root;
  check_false(json_bool_value(diag, "in_sync", 1));
  check_equal((int)(json_int_value(diag, "participant_mismatch_count", -1)), (int)(1));
  check_equal((int)(json_int_value(diag, "subscription_mismatch_count", -1)), (int)(1));
  json_free(root);
  root = NULL;

  memset(&replay_stats, 0, sizeof(replay_stats));
  check_equal((int)(room_service_app_server_sync_resync_room(
                               room_server, "room-replay", &replay_stats)), (int)(0));
  check_equal((int)(replay_stats.participants_replayed), (int)(2));
  check_equal((int)(replay_stats.receiver_bandwidths_replayed), (int)(2));
  check_equal((int)(replay_stats.tracks_replayed), (int)(1));
  check_equal((int)(replay_stats.subscriptions_replayed), (int)(1));
  check_equal((int)(replay_stats.skipped_items), (int)(0));
  check_equal((int)(room_service_app_server_record_room_sync(
                               room_server, "room-replay", "resync", &replay_stats,
                               NULL, NULL)), (int)(0));
  app_test_sleep_ms(100);

  diag_json = room_service_http_api_build_room_diagnostic(room_server, "room-replay");
  check_not_null(diag_json);
  check_equal((int)(parse_json_text(diag_json, &root)), (int)(0));
  free(diag_json);
  diag_json = NULL;
  diag = root;
  check_true(json_bool_value(diag, "in_sync", 0));
  check_equal((int)(json_int_value(diag, "participant_mismatch_count", -1)), (int)(0));
  check_equal((int)(json_int_value(diag, "subscription_mismatch_count", -1)), (int)(0));
  last_sync = json_object_field(diag, "last_sfu_sync");
  check_not_null(last_sync);
  check_equal(json_string_value(last_sync, "operation"), "resync");
  check_false(json_bool_value(last_sync, "had_warning", 1));
  last_sync_stats = json_object_field(last_sync, "sfu_replay_stats");
  check_not_null(last_sync_stats);
  check_equal((int)(json_int_value(last_sync_stats, "participants_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(last_sync_stats, "receiver_bandwidths_replayed", -1)), (int)(2));
  check_equal((int)(json_int_value(last_sync_stats, "tracks_replayed", -1)), (int)(1));
  check_equal((int)(json_int_value(last_sync_stats, "subscriptions_replayed", -1)), (int)(1));
  check_equal((int)(json_int_value(last_sync_stats, "skipped_items", -1)), (int)(0));
  json_free(root);
  root = NULL;

  room_service_app_server_destroy(room_server);
  sfu_node_app_server_stop(sfu_server);
  sfu_node_app_server_destroy(sfu_server);
}

void test_room_service_conference_policy_materializes_and_syncs_screen_pin_and_active_speaker(
    void) {
  sfu_node_app_config_t sfu_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_server = NULL;
  room_service_app_server_t *room_server = NULL;
  json_value_t *root = NULL;
  json_value_t *room = NULL;
  json_value_t *policy = NULL;
  json_value_t *subscription = NULL;
  json_value_t *diag = NULL;
  json_value_t *conference_policy = NULL;
  json_value_t *subscription_diag = NULL;
  json_value_t *room_state = NULL;
  json_value_t *state_room = NULL;
  json_value_t *state_policy = NULL;
  json_value_t *sfu_track_subscription = NULL;
  const char *sfu_node_base_url = "http://127.0.0.1:19391";
  const char *room_service_base_url = "http://127.0.0.1:19392";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19391;
  sfu_config.node_id = "node-eu-1";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19392;
  room_config.node_id = "room-service-test";
  room_config.sfu_control_url = "http://127.0.0.1:19391";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  check_equal((int)(sfu_node_app_server_start(sfu_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100)), (int)(0));

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30, 100)), (int)(0));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-policy\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-policy\","
      "\"participant\":{"
      "\"participant_id\":\"alice\","
      "\"user_id\":\"u-alice\","
      "\"display_name\":\"Alice\","
      "\"role\":\"host\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-policy\","
      "\"participant\":{"
      "\"participant_id\":\"bob\","
      "\"user_id\":\"u-bob\","
      "\"display_name\":\"Bob\","
      "\"role\":\"guest\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-policy\","
      "\"participant\":{"
      "\"participant_id\":\"charlie\","
      "\"user_id\":\"u-charlie\","
      "\"display_name\":\"Charlie\","
      "\"role\":\"guest\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-policy\","
      "\"participant\":{"
      "\"participant_id\":\"dave\","
      "\"user_id\":\"u-dave\","
      "\"display_name\":\"Dave\","
      "\"role\":\"guest\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-policy\","
      "\"track\":{"
      "\"track_id\":\"alice-audio\","
      "\"owner_participant_id\":\"alice\","
      "\"kind\":\"audio\","
      "\"source\":\"mic\","
      "\"codec_name\":\"opus\","
      "\"simulcast_enabled\":false,"
      "\"main_ssrc\":7101"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-policy\","
      "\"track\":{"
      "\"track_id\":\"alice-cam\","
      "\"owner_participant_id\":\"alice\","
      "\"kind\":\"video\","
      "\"source\":\"camera\","
      "\"codec_name\":\"vp9\","
      "\"simulcast_enabled\":true,"
      "\"main_ssrc\":7203,"
      "\"layer_ssrcs\":[7201,7202,7203]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-policy\","
      "\"track\":{"
      "\"track_id\":\"charlie-cam\","
      "\"owner_participant_id\":\"charlie\","
      "\"kind\":\"video\","
      "\"source\":\"camera\","
      "\"codec_name\":\"vp9\","
      "\"simulcast_enabled\":true,"
      "\"main_ssrc\":7303,"
      "\"layer_ssrcs\":[7301,7302,7303]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-policy\","
      "\"track\":{"
      "\"track_id\":\"dave-screen\","
      "\"owner_participant_id\":\"dave\","
      "\"kind\":\"video\","
      "\"source\":\"screen\","
      "\"codec_name\":\"vp9\","
      "\"simulcast_enabled\":true,"
      "\"main_ssrc\":7403,"
      "\"layer_ssrcs\":[7401,7402,7403]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-policy\","
      "\"node_id\":\"node-eu-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  room = json_object_field(root, "room");
  check_not_null(room);
  check_equal(json_string_value(room, "assigned_sfu_node"), "node-eu-1");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_layout_mode\","
      "\"room_id\":\"room-policy\","
      "\"layout_mode\":\"speaker\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  policy = json_object_field(root, "conference_policy");
  check_not_null(policy);
  check_equal(json_string_value(policy, "layout_mode"), "speaker");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_active_speaker\","
      "\"room_id\":\"room-policy\","
      "\"participant_id\":\"alice\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  policy = json_object_field(root, "conference_policy");
  check_not_null(policy);
  check_equal(json_string_value(policy, "active_speaker_participant_id"), "alice");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_pin\","
      "\"room_id\":\"room-policy\","
      "\"participant_id\":\"charlie\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  policy = json_object_field(root, "conference_policy");
  check_not_null(policy);
  check_equal(json_string_value(policy, "pinned_participant_id"), "charlie");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"apply_conference_policy\","
      "\"room_id\":\"room-policy\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_null(json_string_value(root, "warning_code"));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"alice-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_true(json_bool_value(subscription, "enabled", 0));
  check_equal((int)(json_int_value(subscription, "priority", -1)), (int)(400));
  check_equal(json_string_value(subscription, "target_layer"), "low");
  check_equal(json_string_value(subscription, "policy_source"), "conference_policy_audio");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"charlie-cam\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_true(json_bool_value(subscription, "enabled", 0));
  check_equal((int)(json_int_value(subscription, "priority", -1)), (int)(250));
  check_equal(json_string_value(subscription, "target_layer"), "high");
  check_equal(json_string_value(subscription, "policy_source"), "conference_policy_pin");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"alice-cam\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_true(json_bool_value(subscription, "enabled", 0));
  check_equal((int)(json_int_value(subscription, "priority", -1)), (int)(100));
  check_equal(json_string_value(subscription, "target_layer"), "low");
  check_equal(json_string_value(subscription, "policy_source"), "conference_policy_camera");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"dave-screen\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_true(json_bool_value(subscription, "enabled", 0));
  check_equal((int)(json_int_value(subscription, "priority", -1)), (int)(300));
  check_equal(json_string_value(subscription, "target_layer"), "high");
  check_equal(json_string_value(subscription, "policy_source"), "conference_policy_screen");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription_diagnostic\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"dave-screen\""
      "}");
  check_not_null(root);
  subscription_diag = json_object_field(root, "subscription_diagnostic");
  check_not_null(subscription_diag);
  check_equal(json_string_value(subscription_diag, "sfu_mirror_status"), "ok");
  check_true(json_bool_value(subscription_diag, "in_sync", 0));
  sfu_track_subscription = json_object_field(subscription_diag, "sfu_track_subscription");
  check_not_null(sfu_track_subscription);
  check_equal((int)(json_int_value(sfu_track_subscription, "priority", -1)), (int)(300));
  check_equal(json_string_value(sfu_track_subscription, "preferred_layer"), "high");
  check_equal(json_string_value(sfu_track_subscription, "target_layer"), "high");
  check_equal((int)(json_bool_value(sfu_track_subscription, "muted", 1)), (int)(0));
  check_equal(json_string_value(sfu_track_subscription, "policy_source"), "conference_policy_screen");
  json_free(root);
  root = NULL;

  root = http_get_json(room_service_base_url, "/api/v1/rooms/room-policy/state");
  check_not_null(root);
  room_state = root;
  state_room = json_object_field(room_state, "room");
  state_policy = json_object_field(room_state, "conference_policy");
  check_not_null(state_room);
  check_not_null(state_policy);
  check_equal(json_string_value(state_room, "room_id"), "room-policy");
  check_equal(json_string_value(state_policy, "layout_mode"), "speaker");
  check_equal(json_string_value(state_policy,
                                             "active_speaker_participant_id"), "alice");
  check_equal(json_string_value(state_policy,
                                             "pinned_participant_id"), "charlie");
  check_equal((int)((int)json_array_count(room_state, "participants")), (int)(4));
  check_equal((int)((int)json_array_count(room_state, "published_tracks")), (int)(4));
  check_equal((int)((int)json_array_count(room_state, "subscriptions")), (int)(12));
  json_free(root);
  root = NULL;
  room_state = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_state\","
      "\"room_id\":\"room-policy\""
      "}");
  check_not_null(root);
  room_state = json_object_field(root, "room_state");
  check_not_null(room_state);
  state_room = json_object_field(room_state, "room");
  state_policy = json_object_field(room_state, "conference_policy");
  check_not_null(state_room);
  check_not_null(state_policy);
  check_equal(json_string_value(state_room, "room_id"), "room-policy");
  check_equal(json_string_value(state_policy, "layout_mode"), "speaker");
  check_equal((int)((int)json_array_count(room_state, "participants")), (int)(4));
  check_equal((int)((int)json_array_count(room_state, "published_tracks")), (int)(4));
  check_equal((int)((int)json_array_count(room_state, "subscriptions")), (int)(12));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_diagnostic\","
      "\"room_id\":\"room-policy\""
      "}");
  check_not_null(root);
  diag = json_object_field(root, "room_diagnostic");
  check_not_null(diag);
  conference_policy = json_object_field(diag, "conference_policy");
  check_not_null(conference_policy);
  check_equal(json_string_value(conference_policy, "layout_mode"), "speaker");
  check_equal(json_string_value(conference_policy,
                                             "active_speaker_participant_id"), "alice");
  check_equal(json_string_value(conference_policy,
                                             "pinned_participant_id"), "charlie");
  check_true(json_bool_value(diag, "in_sync", 0));
  check_equal((int)(json_int_value(diag, "subscription_mismatch_count", -1)), (int)(0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_pin\","
      "\"room_id\":\"room-policy\","
      "\"participant_id\":\"\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  policy = json_object_field(root, "conference_policy");
  check_not_null(policy);
  check_equal(json_string_value(policy, "pinned_participant_id"), "");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"apply_conference_policy\","
      "\"room_id\":\"room-policy\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_null(json_string_value(root, "warning_code"));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"alice-cam\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_equal((int)(json_int_value(subscription, "priority", -1)), (int)(200));
  check_equal(json_string_value(subscription, "target_layer"), "high");
  check_equal(json_string_value(subscription, "policy_source"), "conference_policy_active");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"charlie-cam\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_equal((int)(json_int_value(subscription, "priority", -1)), (int)(100));
  check_equal(json_string_value(subscription, "target_layer"), "low");
  check_equal(json_string_value(subscription, "policy_source"), "conference_policy_camera");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_diagnostic\","
      "\"room_id\":\"room-policy\""
      "}");
  check_not_null(root);
  diag = json_object_field(root, "room_diagnostic");
  check_not_null(diag);
  conference_policy = json_object_field(diag, "conference_policy");
  check_not_null(conference_policy);
  check_equal(json_string_value(conference_policy,
                                             "active_speaker_participant_id"), "alice");
  check_equal(json_string_value(conference_policy,
                                                  "pinned_participant_id"), "");
  check_true(json_bool_value(diag, "in_sync", 0));
  json_free(root);
  root = NULL;

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
  sfu_node_app_server_stop(sfu_server);
  sfu_node_app_server_destroy(sfu_server);
}

void test_room_service_call_center_policy_materializes_and_syncs_supervisor_modes(void) {
  sfu_node_app_config_t sfu_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_server = NULL;
  room_service_app_server_t *room_server = NULL;
  turbo_room_service_t *service = NULL;
  room_service_conference_policy_apply_result_t apply_result;
  room_service_sfu_replay_stats_t replay_stats;
  room_service_sfu_track_subscription_t sfu_subscription;
  const char *sfu_node_base_url = "http://127.0.0.1:19395";
  turbo_room_config_t room = {
      .room_id = "room-call-center",
      .room_type = TURBO_ROOM_TYPE_CALL,
      .created_by = "router-1",
  };
  turbo_room_participant_config_t customer = {
      .participant_id = "customer",
      .user_id = "u-customer",
      .display_name = "Customer",
      .role = TURBO_PARTICIPANT_ROLE_CUSTOMER,
  };
  turbo_room_participant_config_t agent = {
      .participant_id = "agent",
      .user_id = "u-agent",
      .display_name = "Agent",
      .role = TURBO_PARTICIPANT_ROLE_AGENT,
  };
  turbo_room_participant_config_t supervisor = {
      .participant_id = "supervisor",
      .user_id = "u-supervisor",
      .display_name = "Supervisor",
      .role = TURBO_PARTICIPANT_ROLE_SUPERVISOR,
  };
  turbo_room_track_config_t customer_audio = {
      .track_id = "customer-audio",
      .owner_participant_id = "customer",
      .kind = TURBO_ROOM_TRACK_AUDIO,
      .source = TURBO_ROOM_SOURCE_MIC,
      .codec_name = "opus",
      .main_ssrc = 9101,
      .layer_ssrcs = (uint32_t[]){9101},
      .layer_count = 1,
  };
  turbo_room_track_config_t agent_audio = {
      .track_id = "agent-audio",
      .owner_participant_id = "agent",
      .kind = TURBO_ROOM_TRACK_AUDIO,
      .source = TURBO_ROOM_SOURCE_MIC,
      .codec_name = "opus",
      .main_ssrc = 9201,
      .layer_ssrcs = (uint32_t[]){9201},
      .layer_count = 1,
  };
  turbo_room_track_config_t supervisor_audio = {
      .track_id = "supervisor-audio",
      .owner_participant_id = "supervisor",
      .kind = TURBO_ROOM_TRACK_AUDIO,
      .source = TURBO_ROOM_SOURCE_MIC,
      .codec_name = "opus",
      .main_ssrc = 9301,
      .layer_ssrcs = (uint32_t[]){9301},
      .layer_count = 1,
  };

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19395;
  sfu_config.node_id = "node-eu-1";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19396;
  room_config.node_id = "room-service-test";
  room_config.sfu_control_url = "http://127.0.0.1:19395";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  check_equal((int)(sfu_node_app_server_start(sfu_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100)), (int)(0));

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  service = room_service_app_server_get_service(room_server);
  check_not_null(service);

  check_equal((int)(turbo_room_service_create_room(service, &room)), (int)(0));
  check_equal((int)(turbo_room_service_add_participant(service, "room-call-center",
                                                              &customer)), (int)(0));
  check_equal((int)(turbo_room_service_add_participant(service, "room-call-center",
                                                              &agent)), (int)(0));
  check_equal((int)(turbo_room_service_add_participant(service, "room-call-center",
                                                              &supervisor)), (int)(0));
  check_equal((int)(turbo_room_service_publish_track(service, "room-call-center",
                                                            &customer_audio)), (int)(0));
  check_equal((int)(turbo_room_service_publish_track(service, "room-call-center",
                                                            &agent_audio)), (int)(0));
  check_equal((int)(turbo_room_service_publish_track(service, "room-call-center",
                                                            &supervisor_audio)), (int)(0));
  check_equal((int)(turbo_room_service_assign_sfu_node(service, "room-call-center",
                                                              "node-eu-1")), (int)(0));
  memset(&replay_stats, 0, sizeof(replay_stats));
  check_equal((int)(room_service_app_server_sync_replay_room_state(
                               room_server, "room-call-center", &replay_stats)), (int)(0));
  check_equal((int)(replay_stats.participants_replayed), (int)(3));
  check_equal((int)(replay_stats.tracks_replayed), (int)(3));

  memset(&apply_result, 0, sizeof(apply_result));
  check_equal((int)(room_service_app_server_apply_call_center_policy(
                               room_server, "room-call-center",
                               TURBO_CALL_CENTER_SUPERVISOR_MONITOR,
                               &apply_result)), (int)(0));
  check_false(apply_result.had_warning);
  check_equal((int)(apply_result.subscriptions_applied), (int)(4));
  check_equal((int)(room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "supervisor",
                               "customer-audio", &sfu_subscription)), (int)(0));
  check_true(sfu_subscription.found);
  check_true(sfu_subscription.enabled);
  check_equal(sfu_subscription.policy_source, "call_center_monitor");
  check_equal((int)(room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "customer",
                               "supervisor-audio", &sfu_subscription)), (int)(0));
  check_false(sfu_subscription.found);

  memset(&apply_result, 0, sizeof(apply_result));
  check_equal((int)(room_service_app_server_apply_call_center_policy(
                               room_server, "room-call-center",
                               TURBO_CALL_CENTER_SUPERVISOR_WHISPER,
                               &apply_result)), (int)(0));
  check_false(apply_result.had_warning);
  check_equal((int)(apply_result.subscriptions_applied), (int)(5));
  check_equal((int)(room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "agent",
                               "supervisor-audio", &sfu_subscription)), (int)(0));
  check_true(sfu_subscription.found);
  check_true(sfu_subscription.enabled);
  check_equal(sfu_subscription.policy_source, "call_center_whisper");

  memset(&apply_result, 0, sizeof(apply_result));
  check_equal((int)(room_service_app_server_apply_call_center_policy(
                               room_server, "room-call-center",
                               TURBO_CALL_CENTER_SUPERVISOR_BARGE,
                               &apply_result)), (int)(0));
  check_false(apply_result.had_warning);
  check_equal((int)(apply_result.subscriptions_applied), (int)(6));
  check_equal((int)(room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "customer",
                               "supervisor-audio", &sfu_subscription)), (int)(0));
  check_true(sfu_subscription.found);
  check_true(sfu_subscription.enabled);
  check_equal(sfu_subscription.policy_source, "call_center_barge");

  memset(&apply_result, 0, sizeof(apply_result));
  check_equal((int)(room_service_app_server_apply_call_center_policy(
                               room_server, "room-call-center",
                               TURBO_CALL_CENTER_SUPERVISOR_NONE,
                               &apply_result)), (int)(0));
  check_false(apply_result.had_warning);
  check_equal((int)(apply_result.subscriptions_applied), (int)(2));
  check_equal((int)(apply_result.subscriptions_removed), (int)(4));
  check_equal((int)(room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "customer",
                               "supervisor-audio", &sfu_subscription)), (int)(0));
  check_true(sfu_subscription.found);
  check_false(sfu_subscription.enabled);
  check_true(sfu_subscription.muted);
  check_equal((int)(room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "supervisor",
                               "customer-audio", &sfu_subscription)), (int)(0));
  check_true(sfu_subscription.found);
  check_false(sfu_subscription.enabled);
  check_true(sfu_subscription.muted);

  room_service_app_server_destroy(room_server);
  sfu_node_app_server_stop(sfu_server);
  sfu_node_app_server_destroy(sfu_server);
}

void test_room_service_http_call_center_policy_command_syncs_to_sfu_node(void) {
  sfu_node_app_config_t sfu_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_server = NULL;
  room_service_app_server_t *room_server = NULL;
  json_value_t *root = NULL;
  json_value_t *participant = NULL;
  json_value_t *subscription = NULL;
  json_value_t *policy = NULL;
  json_value_t *apply_result = NULL;
  json_value_t *subscription_diag = NULL;
  json_value_t *sfu_track_subscription = NULL;
  const char *sfu_node_base_url = "http://127.0.0.1:19397";
  const char *room_service_base_url = "http://127.0.0.1:19398";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19397;
  sfu_config.node_id = "node-eu-1";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19398;
  room_config.node_id = "room-service-test";
  room_config.sfu_control_url = "http://127.0.0.1:19397";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  check_equal((int)(sfu_node_app_server_start(sfu_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100)), (int)(0));

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30, 100)), (int)(0));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-http-call-center\","
      "\"room_type\":\"call\","
      "\"created_by\":\"router-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-http-call-center\","
      "\"participant\":{"
      "\"participant_id\":\"customer\","
      "\"user_id\":\"u-customer\","
      "\"display_name\":\"Customer\","
      "\"role\":\"customer\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-http-call-center\","
      "\"participant\":{"
      "\"participant_id\":\"agent\","
      "\"user_id\":\"u-agent\","
      "\"display_name\":\"Agent\","
      "\"role\":\"agent\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-http-call-center\","
      "\"participant\":{"
      "\"participant_id\":\"supervisor\","
      "\"user_id\":\"u-supervisor\","
      "\"display_name\":\"Supervisor\","
      "\"role\":\"supervisor\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-http-call-center\","
      "\"track\":{"
      "\"track_id\":\"customer-audio\","
      "\"owner_participant_id\":\"customer\","
      "\"kind\":\"audio\","
      "\"source\":\"mic\","
      "\"codec_name\":\"opus\","
      "\"main_ssrc\":9501,"
      "\"layer_ssrcs\":[9501]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-http-call-center\","
      "\"track\":{"
      "\"track_id\":\"agent-audio\","
      "\"owner_participant_id\":\"agent\","
      "\"kind\":\"audio\","
      "\"source\":\"mic\","
      "\"codec_name\":\"opus\","
      "\"main_ssrc\":9601,"
      "\"layer_ssrcs\":[9601]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-http-call-center\","
      "\"track\":{"
      "\"track_id\":\"supervisor-audio\","
      "\"owner_participant_id\":\"supervisor\","
      "\"kind\":\"audio\","
      "\"source\":\"mic\","
      "\"codec_name\":\"opus\","
      "\"main_ssrc\":9701,"
      "\"layer_ssrcs\":[9701]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-http-call-center\","
      "\"node_id\":\"node-eu-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_participant\","
      "\"room_id\":\"room-http-call-center\","
      "\"participant_id\":\"customer\""
      "}");
  check_not_null(root);
  participant = json_object_field(root, "participant");
  check_not_null(participant);
  check_equal(json_string_value(participant, "role"), "customer");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"apply_call_center_policy\","
      "\"room_id\":\"room-http-call-center\","
      "\"supervisor_mode\":\"barge\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_null(json_string_value(root, "warning_code"));
  policy = json_object_field(root, "call_center_policy");
  check_not_null(policy);
  check_equal(json_string_value(policy, "supervisor_mode"), "barge");
  apply_result = json_object_field(root, "policy_apply_result");
  check_not_null(apply_result);
  check_equal((int)(json_int_value(apply_result, "subscriptions_applied", -1)), (int)(6));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-http-call-center\","
      "\"subscriber_participant_id\":\"customer\","
      "\"track_id\":\"supervisor-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_true(json_bool_value(subscription, "enabled", 0));
  check_equal(json_string_value(subscription, "policy_source"), "call_center_barge");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription_diagnostic\","
      "\"room_id\":\"room-http-call-center\","
      "\"subscriber_participant_id\":\"customer\","
      "\"track_id\":\"supervisor-audio\""
      "}");
  check_not_null(root);
  subscription_diag = json_object_field(root, "subscription_diagnostic");
  check_not_null(subscription_diag);
  check_true(json_bool_value(subscription_diag, "in_sync", 0));
  sfu_track_subscription = json_object_field(subscription_diag, "sfu_track_subscription");
  check_not_null(sfu_track_subscription);
  check_equal(json_string_value(sfu_track_subscription, "policy_source"), "call_center_barge");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"apply_call_center_policy\","
      "\"room_id\":\"room-http-call-center\","
      "\"supervisor_mode\":\"none\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_null(json_string_value(root, "warning_code"));
  policy = json_object_field(root, "call_center_policy");
  check_not_null(policy);
  check_equal(json_string_value(policy, "supervisor_mode"), "none");
  apply_result = json_object_field(root, "policy_apply_result");
  check_not_null(apply_result);
  check_equal((int)(json_int_value(apply_result, "subscriptions_removed", -1)), (int)(4));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_track_subscription\","
      "\"room_id\":\"room-http-call-center\","
      "\"receiver_participant_id\":\"customer\","
      "\"track_id\":\"supervisor-audio\""
      "}");
  check_not_null(root);
  sfu_track_subscription = json_object_field(root, "track_subscription");
  check_not_null(sfu_track_subscription);
  check_false(json_bool_value(sfu_track_subscription, "enabled", 1));
  check_true(json_bool_value(sfu_track_subscription, "muted", 0));
  json_free(root);
  root = NULL;

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
  sfu_node_app_server_stop(sfu_server);
  sfu_node_app_server_destroy(sfu_server);
}

void test_room_service_http_call_center_priority_queue_commands(void) {
  room_service_app_config_t room_config;
  room_service_app_server_t *room_server = NULL;
  const char *room_service_base_url = "http://127.0.0.1:19408";
  json_value_t *root = NULL;
  json_value_t *queue_entry = NULL;
  json_value_t *match = NULL;
  json_value_t *caller = NULL;
  json_value_t *callee = NULL;
  json_value_t *route = NULL;
  json_value_t *agent_state = NULL;
  json_value_t *call_center_room = NULL;
  json_value_t *call_center_events = NULL;
  json_value_t *event = NULL;
  json_value_t *participant = NULL;
  json_value_t *subscription = NULL;
  json_value_t *room = NULL;
  int latest_event_sequence = 0;
  char event_path[160];

  room_service_app_config_init(&room_config);
  room_config.bind_port = 19408;
  room_config.node_id = "room-service-call-center-queue";

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30,
                                                100)), (int)(0));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"enqueue_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"caller\","
      "\"entry_id\":\"caller-regular\","
      "\"endpoint_id\":\"customer-regular\","
      "\"priority\":10"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal((int)(json_int_value(root, "queue_depth", -1)), (int)(1));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"enqueue_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"caller\","
      "\"entry_id\":\"caller-vip\","
      "\"endpoint_id\":\"customer-vip\","
      "\"priority\":80"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal((int)(json_int_value(root, "queue_depth", -1)), (int)(2));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"enqueue_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"callee\","
      "\"entry_id\":\"agent-primary\","
      "\"endpoint_id\":\"agent-a\","
      "\"priority\":30"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal((int)(json_int_value(root, "queue_depth", -1)), (int)(1));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"peek_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"caller\""
      "}");
  check_not_null(root);
  queue_entry = json_object_field(root, "queue_entry");
  check_not_null(queue_entry);
  check_equal(json_string_value(queue_entry, "entry_id"), "caller-vip");
  check_equal(json_string_value(queue_entry, "endpoint_id"), "customer-vip");
  check_equal((int)(json_int_value(queue_entry, "priority", -1)), (int)(80));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"match_call_center_queue\","
      "\"queue_id\":\"support\""
      "}");
  check_not_null(root);
  match = json_object_field(root, "call_center_match");
  check_not_null(match);
  check_true(json_bool_value(match, "matched", 0));
  caller = json_object_field(match, "caller");
  callee = json_object_field(match, "callee");
  check_not_null(caller);
  check_not_null(callee);
  check_equal(json_string_value(caller, "entry_id"), "caller-vip");
  check_equal(json_string_value(callee, "entry_id"), "agent-primary");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal((int)(json_int_value(root, "caller_depth", -1)), (int)(1));
  check_equal((int)(json_int_value(root, "callee_depth", -1)), (int)(0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"enqueue_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"callee\","
      "\"entry_id\":\"agent-secondary\","
      "\"endpoint_id\":\"agent-b\","
      "\"priority\":20"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"route_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"route_room_id\":\"room-routed-call\","
      "\"created_by\":\"router-test\","
      "\"recording_id\":\"rec-routed-call\","
      "\"recording_mode\":\"compliance\""
      "}");
  check_not_null(root);
  route = json_object_field(root, "call_center_route");
  check_not_null(route);
  check_equal(json_string_value(route, "room_id"), "room-routed-call");
  match = json_object_field(route, "match");
  check_not_null(match);
  check_true(json_bool_value(match, "matched", 0));
  caller = json_object_field(match, "caller");
  callee = json_object_field(match, "callee");
  check_not_null(caller);
  check_not_null(callee);
  check_equal(json_string_value(caller, "entry_id"), "caller-regular");
  check_equal(json_string_value(callee, "entry_id"), "agent-secondary");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_participant\","
      "\"room_id\":\"room-routed-call\","
      "\"participant_id\":\"customer-regular\""
      "}");
  check_not_null(root);
  participant = json_object_field(root, "participant");
  check_not_null(participant);
  check_equal(json_string_value(participant, "role"), "customer");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_participant\","
      "\"room_id\":\"room-routed-call\","
      "\"participant_id\":\"agent-b\""
      "}");
  check_not_null(root);
  participant = json_object_field(root, "participant");
  check_not_null(participant);
  check_equal(json_string_value(participant, "role"), "agent");
  json_free(root);
  root = NULL;

  root = http_get_json(room_service_base_url, "/api/v1/rooms/room-routed-call");
  check_not_null(root);
  room = root;
  check_equal(json_string_value(room, "recording_state"), "active");
  check_equal(json_string_value(room, "recording_id"), "rec-routed-call");
  check_equal(json_string_value(room, "recording_mode"), "compliance");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_room\","
      "\"room_id\":\"room-routed-call\""
      "}");
  check_not_null(root);
  call_center_room = json_object_field(root, "call_center_room");
  check_not_null(call_center_room);
  check_equal(json_string_value(call_center_room, "state"), "active");
  check_equal(json_string_value(call_center_room,
                                             "customer_participant_id"), "customer-regular");
  check_equal(json_string_value(call_center_room,
                                             "agent_participant_id"), "agent-b");
  check_equal(json_string_value(call_center_room,
                                             "consult_agent_participant_id"), "");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-routed-call\","
      "\"participant\":{"
      "\"participant_id\":\"agent-c\","
      "\"user_id\":\"agent-c\","
      "\"display_name\":\"Agent C\","
      "\"role\":\"agent\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-routed-call\","
      "\"track\":{"
      "\"track_id\":\"track-customer-audio\","
      "\"owner_participant_id\":\"customer-regular\","
      "\"kind\":\"audio\","
      "\"source\":\"mic\","
      "\"codec_name\":\"opus\","
      "\"main_ssrc\":9801,"
      "\"layer_ssrcs\":[9801]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-routed-call\","
      "\"track\":{"
      "\"track_id\":\"track-agent-b-audio\","
      "\"owner_participant_id\":\"agent-b\","
      "\"kind\":\"audio\","
      "\"source\":\"mic\","
      "\"codec_name\":\"opus\","
      "\"main_ssrc\":9802,"
      "\"layer_ssrcs\":[9802]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"apply_call_center_policy\","
      "\"room_id\":\"room-routed-call\","
      "\"supervisor_mode\":\"monitor\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-routed-call\","
      "\"participant\":{"
      "\"participant_id\":\"supervisor-1\","
      "\"user_id\":\"supervisor-1\","
      "\"display_name\":\"Supervisor 1\","
      "\"role\":\"supervisor\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-routed-call\","
      "\"participant\":{"
      "\"participant_id\":\"qa-1\","
      "\"user_id\":\"qa-1\","
      "\"display_name\":\"QA 1\","
      "\"role\":\"qa_observer\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-routed-call\","
      "\"participant\":{"
      "\"participant_id\":\"bot-1\","
      "\"user_id\":\"bot-1\","
      "\"display_name\":\"Bot 1\","
      "\"role\":\"bot\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"supervisor-1\","
      "\"track_id\":\"track-customer-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_true(json_bool_value(subscription, "enabled", 0));
  check_equal(json_string_value(subscription, "policy_source"), "call_center_monitor");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"supervisor-1\","
      "\"track_id\":\"track-agent-b-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_true(json_bool_value(subscription, "enabled", 0));
  check_equal(json_string_value(subscription, "policy_source"), "call_center_monitor");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"qa-1\","
      "\"track_id\":\"track-customer-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_true(json_bool_value(subscription, "enabled", 0));
  check_equal(json_string_value(subscription, "policy_source"), "call_center_observe");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"bot-1\","
      "\"track_id\":\"track-agent-b-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_true(json_bool_value(subscription, "enabled", 0));
  check_equal(json_string_value(subscription, "policy_source"), "call_center_observe");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_call_center_consult_agent\","
      "\"room_id\":\"room-routed-call\","
      "\"consult_agent_participant_id\":\"agent-c\""
      "}");
  check_not_null(root);
  call_center_room = json_object_field(root, "call_center_room");
  check_not_null(call_center_room);
  check_equal(json_string_value(call_center_room,
                                             "consult_agent_participant_id"), "agent-c");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-c\""
      "}");
  check_not_null(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  check_not_null(agent_state);
  check_equal(json_string_value(agent_state, "state"), "busy");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-routed-call\","
      "\"track\":{"
      "\"track_id\":\"track-agent-c-audio\","
      "\"owner_participant_id\":\"agent-c\","
      "\"kind\":\"audio\","
      "\"source\":\"mic\","
      "\"codec_name\":\"opus\","
      "\"main_ssrc\":9803,"
      "\"layer_ssrcs\":[9803]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"agent-b\","
      "\"track_id\":\"track-agent-c-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_equal(json_string_value(subscription, "policy_source"), "call_center_consult");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"agent-c\","
      "\"track_id\":\"track-customer-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_equal(json_string_value(subscription, "policy_source"), "call_center_consult");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"qa-1\","
      "\"track_id\":\"track-agent-c-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_equal(json_string_value(subscription, "policy_source"), "call_center_observe");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"bot-1\","
      "\"track_id\":\"track-agent-c-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_equal(json_string_value(subscription, "policy_source"), "call_center_observe");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"remove_participant\","
      "\"room_id\":\"room-routed-call\","
      "\"participant_id\":\"agent-c\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_room\","
      "\"room_id\":\"room-routed-call\""
      "}");
  check_not_null(root);
  call_center_room = json_object_field(root, "call_center_room");
  check_not_null(call_center_room);
  check_equal(json_string_value(call_center_room,
                                             "consult_agent_participant_id"), "");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-c\""
      "}");
  check_not_null(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  check_not_null(agent_state);
  check_equal(json_string_value(agent_state, "state"), "available");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-routed-call\","
      "\"participant\":{"
      "\"participant_id\":\"agent-c\","
      "\"user_id\":\"agent-c\","
      "\"display_name\":\"Agent C\","
      "\"role\":\"agent\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_call_center_consult_agent\","
      "\"room_id\":\"room-routed-call\","
      "\"consult_agent_participant_id\":\"agent-c\""
      "}");
  check_not_null(root);
  call_center_room = json_object_field(root, "call_center_room");
  check_not_null(call_center_room);
  check_equal(json_string_value(call_center_room,
                                             "consult_agent_participant_id"), "agent-c");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-routed-call\","
      "\"track\":{"
      "\"track_id\":\"track-agent-c-audio\","
      "\"owner_participant_id\":\"agent-c\","
      "\"kind\":\"audio\","
      "\"source\":\"mic\","
      "\"codec_name\":\"opus\","
      "\"main_ssrc\":9803,"
      "\"layer_ssrcs\":[9803]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"agent-b\","
      "\"track_id\":\"track-agent-c-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_equal(json_string_value(subscription, "policy_source"), "call_center_consult");
  json_free(root);
  root = NULL;

  root = NULL;
  check_equal((int)(http_post_json_status(
               room_service_base_url, "/api/v1/commands",
               "{"
               "\"type\":\"get_subscription\","
               "\"room_id\":\"room-routed-call\","
               "\"subscriber_participant_id\":\"customer-regular\","
               "\"track_id\":\"track-agent-c-audio\""
                "}",
               &root)), (int)(404));
  check_not_null(root);
  check_equal(json_string_value(root, "code"), "SUBSCRIPTION_NOT_FOUND");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"complete_call_center_transfer\","
      "\"room_id\":\"room-routed-call\","
      "\"released_agent_state\":\"wrap_up\""
      "}");
  check_not_null(root);
  call_center_room = json_object_field(root, "call_center_room");
  check_not_null(call_center_room);
  check_equal(json_string_value(call_center_room,
                                             "agent_participant_id"), "agent-c");
  check_equal(json_string_value(call_center_room,
                                             "consult_agent_participant_id"), "");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-b\""
      "}");
  check_not_null(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  check_not_null(agent_state);
  check_equal(json_string_value(agent_state, "state"), "wrap_up");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"customer-regular\","
      "\"track_id\":\"track-agent-c-audio\""
      "}");
  check_not_null(root);
  subscription = json_object_field(root, "subscription");
  check_not_null(subscription);
  check_equal(json_string_value(subscription, "policy_source"), "call_center_call");
  json_free(root);
  root = NULL;

  root = NULL;
  check_equal((int)(http_post_json_status(
               room_service_base_url, "/api/v1/commands",
               "{"
               "\"type\":\"get_subscription\","
               "\"room_id\":\"room-routed-call\","
               "\"subscriber_participant_id\":\"customer-regular\","
               "\"track_id\":\"track-agent-b-audio\""
                "}",
               &root)), (int)(404));
  check_not_null(root);
  check_equal(json_string_value(root, "code"), "SUBSCRIPTION_NOT_FOUND");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"finalize_call_center_room\","
      "\"room_id\":\"room-routed-call\","
      "\"state\":\"wrap_up\","
      "\"disposition_code\":\"resolved\","
      "\"agent_state\":\"wrap_up\""
      "}");
  check_not_null(root);
  call_center_room = json_object_field(root, "call_center_room");
  check_not_null(call_center_room);
  check_equal(json_string_value(call_center_room, "state"), "wrap_up");
  check_equal(json_string_value(call_center_room, "disposition_code"), "resolved");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-c\""
      "}");
  check_not_null(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  check_not_null(agent_state);
  check_equal(json_string_value(agent_state, "state"), "wrap_up");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"finalize_call_center_room\","
      "\"room_id\":\"room-routed-call\","
      "\"state\":\"completed\","
      "\"disposition_code\":\"resolved\","
      "\"agent_state\":\"available\""
      "}");
  check_not_null(root);
  call_center_room = json_object_field(root, "call_center_room");
  check_not_null(call_center_room);
  check_equal(json_string_value(call_center_room, "state"), "completed");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-c\""
      "}");
  check_not_null(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  check_not_null(agent_state);
  check_equal(json_string_value(agent_state, "state"), "available");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_events\","
      "\"room_id\":\"room-routed-call\","
      "\"after_sequence\":0,"
      "\"limit\":16"
      "}");
  check_not_null(root);
  call_center_events = json_object_field(root, "call_center_events");
  check_not_null(call_center_events);
  check_equal(json_string_value(call_center_events, "room_id"), "room-routed-call");
  check_equal((size_t)(json_array_count(call_center_events, "events")), (size_t)(10));
  latest_event_sequence = json_int_value(call_center_events, "latest_sequence", 0);
  check_true(latest_event_sequence > 0);

  event = find_call_center_event(call_center_events, "room_routed", "customer-regular",
                                 "active");
  check_not_null(event);
  check_equal(json_string_value(event, "peer_participant_id"), "agent-b");
  check_equal(json_string_value(event, "detail"), "support");

  event = find_call_center_event(call_center_events, "policy_applied", NULL, "monitor");
  check_not_null(event);

  event = find_call_center_event(call_center_events, "observer_joined", "supervisor-1",
                                 "supervisor");
  check_not_null(event);
  event = find_call_center_event(call_center_events, "observer_joined", "qa-1",
                                 "qa_observer");
  check_not_null(event);
  event = find_call_center_event(call_center_events, "observer_joined", "bot-1", "bot");
  check_not_null(event);
  event = find_call_center_event(call_center_events, "consult_started", "agent-c",
                                 "active");
  check_not_null(event);
  event = find_call_center_event(call_center_events, "transfer_completed", "agent-c",
                                 "active");
  check_not_null(event);
  check_equal(json_string_value(event, "detail"), "wrap_up");
  event = find_call_center_event(call_center_events, "room_finalized", NULL,
                                 "wrap_up");
  check_not_null(event);
  check_equal(json_string_value(event, "detail"), "resolved");
  event = find_call_center_event(call_center_events, "room_finalized", NULL,
                                 "completed");
  check_not_null(event);
  check_equal(json_string_value(event, "detail"), "resolved");
  json_free(root);
  root = NULL;

  snprintf(event_path, sizeof(event_path),
           "/api/v1/rooms/room-routed-call/call_center_events?after_sequence=%d&limit=4",
           latest_event_sequence - 1);
  root = http_get_json(room_service_base_url, event_path);
  check_not_null(root);
  check_equal(json_string_value(root, "room_id"), "room-routed-call");
  check_equal((size_t)(json_array_count(root, "events")), (size_t)(1));
  event = json_array_object_at(json_array_field(root, "events"), 0);
  check_not_null(event);
  check_equal(json_string_value(event, "event_type"), "room_finalized");
  check_equal(json_string_value(event, "state"), "completed");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal((int)(json_int_value(root, "caller_depth", -1)), (int)(0));
  check_equal((int)(json_int_value(root, "callee_depth", -1)), (int)(0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"enqueue_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"caller\","
      "\"entry_id\":\"caller-retry\","
      "\"endpoint_id\":\"customer-retry\","
      "\"priority\":50"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"enqueue_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"callee\","
      "\"entry_id\":\"agent-retry\","
      "\"endpoint_id\":\"agent-retry\","
      "\"priority\":50"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"route_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"route_room_id\":\"room-routed-retry\","
      "\"test_failure_stage\":\"after_callee_join\""
      "}");
  check_null(root);

  root = http_get_json(room_service_base_url, "/api/v1/rooms/room-routed-retry");
  check_null(root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal((int)(json_int_value(root, "caller_depth", -1)), (int)(1));
  check_equal((int)(json_int_value(root, "callee_depth", -1)), (int)(1));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"route_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"route_room_id\":\"room-routed-retry\""
      "}");
  check_not_null(root);
  route = json_object_field(root, "call_center_route");
  check_not_null(route);
  check_equal(json_string_value(route, "room_id"), "room-routed-retry");
  match = json_object_field(route, "match");
  check_not_null(match);
  check_true(json_bool_value(match, "matched", 0));
  caller = json_object_field(match, "caller");
  callee = json_object_field(match, "callee");
  check_not_null(caller);
  check_not_null(callee);
  check_equal(json_string_value(caller, "entry_id"), "caller-retry");
  check_equal(json_string_value(callee, "entry_id"), "agent-retry");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_call_center_agent_state\","
      "\"endpoint_id\":\"agent-a\","
      "\"state\":\"offline\""
      "}");
  check_not_null(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  check_not_null(agent_state);
  check_equal(json_string_value(agent_state, "state"), "offline");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"enqueue_call_center_queue\","
      "\"queue_id\":\"availability\","
      "\"side\":\"caller\","
      "\"entry_id\":\"caller-availability\","
      "\"endpoint_id\":\"customer-availability\","
      "\"priority\":60"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"enqueue_call_center_queue\","
      "\"queue_id\":\"availability\","
      "\"side\":\"callee\","
      "\"entry_id\":\"agent-offline\","
      "\"endpoint_id\":\"agent-a\","
      "\"priority\":80"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"enqueue_call_center_queue\","
      "\"queue_id\":\"availability\","
      "\"side\":\"callee\","
      "\"entry_id\":\"agent-ready\","
      "\"endpoint_id\":\"agent-c\","
      "\"priority\":20"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"route_call_center_queue\","
      "\"queue_id\":\"availability\","
      "\"route_room_id\":\"room-agent-availability\""
      "}");
  check_not_null(root);
  route = json_object_field(root, "call_center_route");
  check_not_null(route);
  match = json_object_field(route, "match");
  check_not_null(match);
  callee = json_object_field(match, "callee");
  check_not_null(callee);
  check_equal(json_string_value(callee, "entry_id"), "agent-ready");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-a\""
      "}");
  check_not_null(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  check_not_null(agent_state);
  check_equal(json_string_value(agent_state, "state"), "offline");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-c\""
      "}");
  check_not_null(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  check_not_null(agent_state);
  check_equal(json_string_value(agent_state, "state"), "busy");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"enqueue_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"caller\","
      "\"entry_id\":\"caller-loop\","
      "\"endpoint_id\":\"endpoint-loop\","
      "\"priority\":40"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"enqueue_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"callee\","
      "\"entry_id\":\"agent-loop\","
      "\"endpoint_id\":\"endpoint-loop\","
      "\"priority\":40"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"route_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"route_room_id\":\"room-invalid-route\""
      "}");
  check_null(root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal((int)(json_int_value(root, "caller_depth", -1)), (int)(1));
  check_equal((int)(json_int_value(root, "callee_depth", -1)), (int)(1));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"claim_call_center_queue\","
      "\"queue_id\":\"support\""
      "}");
  check_not_null(root);
  match = json_object_field(root, "call_center_match");
  check_not_null(match);
  check_true(json_bool_value(match, "matched", 0));
  caller = json_object_field(match, "caller");
  callee = json_object_field(match, "callee");
  check_not_null(caller);
  check_not_null(callee);
  check_equal(json_string_value(caller, "entry_id"), "caller-loop");
  check_equal(json_string_value(callee, "entry_id"), "agent-loop");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal((int)(json_int_value(root, "caller_depth", -1)), (int)(0));
  check_equal((int)(json_int_value(root, "callee_depth", -1)), (int)(0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"recover_call_center_queue_claims\","
      "\"queue_id\":\"support\","
      "\"lease_ms\":0"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal((int)(json_int_value(root, "recovered", -1)), (int)(2));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"claim_call_center_queue\","
      "\"queue_id\":\"support\""
      "}");
  check_not_null(root);
  match = json_object_field(root, "call_center_match");
  check_not_null(match);
  check_true(json_bool_value(match, "matched", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"rollback_call_center_queue_match\","
      "\"queue_id\":\"support\","
      "\"caller_entry_id\":\"caller-loop\","
      "\"callee_entry_id\":\"agent-loop\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"claim_call_center_queue\","
      "\"queue_id\":\"support\""
      "}");
  check_not_null(root);
  match = json_object_field(root, "call_center_match");
  check_not_null(match);
  check_true(json_bool_value(match, "matched", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"complete_call_center_queue_match\","
      "\"queue_id\":\"support\","
      "\"caller_entry_id\":\"caller-loop\","
      "\"callee_entry_id\":\"agent-loop\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal((int)(json_int_value(root, "caller_depth", -1)), (int)(0));
  check_equal((int)(json_int_value(root, "callee_depth", -1)), (int)(0));
  json_free(root);
  root = NULL;

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
}

void test_room_service_recording_commands_sync_to_sfu_node(void) {
  sfu_node_app_config_t sfu_config;
  room_service_app_config_t room_config;
  sfu_node_app_server_t *sfu_server = NULL;
  room_service_app_server_t *room_server = NULL;
  json_value_t *root = NULL;
  json_value_t *recording_root = NULL;
  json_value_t *recording_status = NULL;
  json_value_t *stats = NULL;
  const char *output_path = NULL;
  const char *sfu_node_base_url = "http://127.0.0.1:19393";
  const char *room_service_base_url = "http://127.0.0.1:19394";

  sfu_node_app_config_init(&sfu_config);
  sfu_config.bind_host = "0.0.0.0";
  sfu_config.bind_port = 19393;
  sfu_config.node_id = "node-eu-1";

  room_service_app_config_init(&room_config);
  room_config.bind_host = "0.0.0.0";
  room_config.bind_port = 19394;
  room_config.node_id = "room-service-test";
  room_config.sfu_control_url = "http://127.0.0.1:19393";

  sfu_server = sfu_node_app_server_create(&sfu_config);
  check_not_null(sfu_server);
  check_equal((int)(sfu_node_app_server_start(sfu_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100)), (int)(0));

  room_server = room_service_app_server_create(&room_config);
  check_not_null(room_server);
  check_equal((int)(room_service_app_server_start(room_server)), (int)(0));
  check_equal((int)(wait_for_http_status_ok(room_service_base_url, "/health", 30, 100)), (int)(0));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-recording\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-recording\","
      "\"participant\":{"
      "\"participant_id\":\"alice\","
      "\"user_id\":\"u-alice\","
      "\"display_name\":\"Alice\","
      "\"role\":\"host\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-recording\","
      "\"track\":{"
      "\"track_id\":\"track-cam\","
      "\"owner_participant_id\":\"alice\","
      "\"kind\":\"video\","
      "\"source\":\"camera\","
      "\"codec_name\":\"vp8\","
      "\"simulcast_enabled\":false,"
      "\"main_ssrc\":8195,"
      "\"layer_ssrcs\":[8195]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-recording\","
      "\"node_id\":\"node-eu-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"start_recording\","
      "\"room_id\":\"room-recording\","
      "\"recording_id\":\"rec-001\","
      "\"mode\":\"archive\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_null(json_string_value(root, "warning_code"));
  json_free(root);
  root = NULL;

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording\""
      "}");
  check_not_null(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  check_not_null(recording_status);
  check_true(json_bool_value(recording_status, "active", 0));
  check_equal(json_string_value(recording_status, "recording_id"), "rec-001");
  check_equal((int)(json_int_value(recording_status, "track_count", -1)), (int)(1));
  json_free(recording_root);
  recording_root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"stop_recording\","
      "\"room_id\":\"room-recording\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_null(json_string_value(root, "warning_code"));
  json_free(root);
  root = NULL;

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording\""
      "}");
  check_not_null(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  check_not_null(recording_status);
  check_false(json_bool_value(recording_status, "active", 1));
  output_path = json_string_value(recording_status, "output_path");
  check_not_null(output_path);
  remove(output_path);
  json_free(recording_root);
  recording_root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-recording-replay\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-recording-replay\","
      "\"participant\":{"
      "\"participant_id\":\"carol\","
      "\"user_id\":\"u-carol\","
      "\"display_name\":\"Carol\","
      "\"role\":\"host\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-recording-replay\","
      "\"track\":{"
      "\"track_id\":\"track-replay-cam\","
      "\"owner_participant_id\":\"carol\","
      "\"kind\":\"video\","
      "\"source\":\"camera\","
      "\"codec_name\":\"vp8\","
      "\"simulcast_enabled\":false,"
      "\"main_ssrc\":8295,"
      "\"layer_ssrcs\":[8295]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"start_recording\","
      "\"room_id\":\"room-recording-replay\","
      "\"recording_id\":\"rec-replay\","
      "\"mode\":\"archive\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-recording-replay\","
      "\"node_id\":\"node-eu-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  stats = json_object_field(root, "sfu_replay_stats");
  check_not_null(stats);
  check_equal((int)(json_int_value(stats, "tracks_replayed", -1)), (int)(1));
  check_equal((int)(json_int_value(stats, "recordings_replayed", -1)), (int)(1));
  json_free(root);
  root = NULL;

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording-replay\""
      "}");
  check_not_null(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  check_not_null(recording_status);
  check_true(json_bool_value(recording_status, "active", 0));
  check_equal(json_string_value(recording_status, "recording_id"), "rec-replay");
  check_equal((int)(json_int_value(recording_status, "track_count", -1)), (int)(1));
  json_free(recording_root);
  recording_root = NULL;

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"stop_recording\","
      "\"room_id\":\"room-recording-replay\""
      "}");
  check_not_null(root);
  recording_status = json_object_field(root, "recording_status");
  check_not_null(recording_status);
  output_path = json_string_value(recording_status, "output_path");
  check_not_null(output_path);
  remove(output_path);
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"start_recording\","
      "\"room_id\":\"room-recording-replay\","
      "\"recording_id\":\"rec-stale\","
      "\"mode\":\"webm\""
      "}");
  check_not_null(root);
  recording_status = json_object_field(root, "recording_status");
  check_not_null(recording_status);
  check_true(json_bool_value(recording_status, "active", 0));
  check_equal(json_string_value(recording_status, "recording_id"), "rec-stale");
  check_equal(json_string_value(recording_status, "mode"), "webm");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-recording-replay\","
      "\"track\":{"
      "\"track_id\":\"track-replay-screen\","
      "\"owner_participant_id\":\"carol\","
      "\"kind\":\"video\","
      "\"source\":\"screen\","
      "\"codec_name\":\"vp8\","
      "\"simulcast_enabled\":false,"
      "\"main_ssrc\":8296,"
      "\"layer_ssrcs\":[8296]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_null(json_string_value(root, "warning_code"));
  json_free(root);
  root = NULL;
  remove("recording_room-recording-replay_rec-stale.webm");

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording-replay\""
      "}");
  check_not_null(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  check_not_null(recording_status);
  check_true(json_bool_value(recording_status, "active", 0));
  check_equal(json_string_value(recording_status, "recording_id"), "rec-replay");
  check_equal(json_string_value(recording_status, "mode"), "archive");
  check_equal((int)(json_int_value(recording_status, "track_count", -1)), (int)(2));
  json_free(recording_root);
  recording_root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"stop_recording\","
      "\"room_id\":\"room-recording-replay\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording-replay\""
      "}");
  check_not_null(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  check_not_null(recording_status);
  output_path = json_string_value(recording_status, "output_path");
  check_not_null(output_path);
  remove(output_path);
  json_free(recording_root);
  recording_root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-recording-late-track\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"add_participant\","
      "\"room_id\":\"room-recording-late-track\","
      "\"participant\":{"
      "\"participant_id\":\"dave\","
      "\"user_id\":\"u-dave\","
      "\"display_name\":\"Dave\","
      "\"role\":\"host\""
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-recording-late-track\","
      "\"node_id\":\"node-eu-1\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"start_recording\","
      "\"room_id\":\"room-recording-late-track\","
      "\"recording_id\":\"rec-late-track\","
      "\"mode\":\"archive\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_equal(json_string_value(root, "warning_code"), "SFU_SYNC_FAILED");
  json_free(root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"publish_track\","
      "\"room_id\":\"room-recording-late-track\","
      "\"track\":{"
      "\"track_id\":\"track-late-cam\","
      "\"owner_participant_id\":\"dave\","
      "\"kind\":\"video\","
      "\"source\":\"camera\","
      "\"codec_name\":\"vp8\","
      "\"simulcast_enabled\":false,"
      "\"main_ssrc\":8395,"
      "\"layer_ssrcs\":[8395]"
      "}"
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  check_null(json_string_value(root, "warning_code"));
  json_free(root);
  root = NULL;

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording-late-track\""
      "}");
  check_not_null(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  check_not_null(recording_status);
  check_true(json_bool_value(recording_status, "active", 0));
  check_equal(json_string_value(recording_status, "recording_id"), "rec-late-track");
  check_equal((int)(json_int_value(recording_status, "track_count", -1)), (int)(1));
  json_free(recording_root);
  recording_root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"stop_recording\","
      "\"room_id\":\"room-recording-late-track\""
      "}");
  check_not_null(root);
  check_true(json_bool_value(root, "ok", 0));
  json_free(root);
  root = NULL;

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording-late-track\""
      "}");
  check_not_null(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  check_not_null(recording_status);
  output_path = json_string_value(recording_status, "output_path");
  check_not_null(output_path);
  remove(output_path);
  json_free(recording_root);
  recording_root = NULL;

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
  sfu_node_app_server_stop(sfu_server);
  sfu_node_app_server_destroy(sfu_server);
}

void test_room_service_http_lifecycle_repeated_start_stop(void) {
  room_service_app_config_t config;
  room_service_app_server_t *server = NULL;
  room_service_http_api_t *http_api = NULL;
  const char *base_url = "http://127.0.0.1:19434";
  int iteration;

  room_service_app_config_init(&config);
  config.bind_host = "127.0.0.1";
  config.bind_port = 19434;
  config.node_id = "room-service-http-lifecycle";

  server = room_service_app_server_create(&config);
  check_not_null(server);
  http_api = room_service_http_api_create(server);
  check_not_null(http_api);

  for (iteration = 0;
       iteration < ROOM_SERVICE_HTTP_LIFECYCLE_STRESS_ITERATIONS;
       ++iteration) {
    check_equal((int)(room_service_http_api_start(http_api, config.bind_host,
                                       config.bind_port)), (int)(0));
    check_equal((int)(wait_for_http_status_ok(base_url, "/health", 3, 10)), (int)(0));
    if (iteration == 0) {
      char *metrics = http_get_text(base_url, "/metrics");
      check_not_null(metrics);
      check_not_null(strstr(metrics, "turbo_room_service_ivr_enabled 0\n"));
      check_not_null(strstr(metrics, "turbo_room_service_ivr_workers 0\n"));
      check_not_null(strstr(
          metrics, "turbo_room_service_ivr_request_queue_high_water 0\n"));
      check_not_null(strstr(
          metrics, "turbo_room_service_iris_provider_enabled 0\n"));
      check_not_null(strstr(
          metrics, "turbo_room_service_iris_queue_capacity 0\n"));
      check_not_null(strstr(
          metrics, "turbo_room_service_iris_delivery_attempts_total 0\n"));
      check_not_null(strstr(
          metrics,
          "turbo_room_service_iris_ledger_resource_queries_total 0\n"));
      check_not_null(strstr(
          metrics,
          "turbo_room_service_iris_ledger_resource_seen_total 0\n"));
      check_not_null(strstr(
          metrics, "turbo_room_service_iris_reconcile_state 0\n"));
      check_not_null(strstr(
          metrics,
          "turbo_room_service_iris_reconcile_accepting_commands 0\n"));
      free(metrics);
    }
    if (iteration + 1 < ROOM_SERVICE_HTTP_LIFECYCLE_STRESS_ITERATIONS) {
      room_service_http_api_stop(http_api);
    }
  }

  room_service_http_api_destroy(http_api);
  check_equal(room_service_app_server_destroy(server), 0);
}

spec("test_room_service_app") {
  it("test_room_service_rejects_identifiers_that_do_not_fit_storage") { test_room_service_rejects_identifiers_that_do_not_fit_storage(); };
  it("test_room_service_http_lifecycle_repeated_start_stop") { test_room_service_http_lifecycle_repeated_start_stop(); };
  it("test_room_service_assign_replays_existing_state_and_closed_room_diag_stays_green") { test_room_service_assign_replays_existing_state_and_closed_room_diag_stays_green(); };
  it("test_room_sync_diagnostic_reports_null_when_room_has_no_sync_history") { test_room_sync_diagnostic_reports_null_when_room_has_no_sync_history(); };
  it("test_room_sync_diagnostic_http_endpoints_expose_latest_sync_state") { test_room_sync_diagnostic_http_endpoints_expose_latest_sync_state(); };
  it("test_room_sync_http_assign_and_resync_results_match_room_sync_diagnostic") { test_room_sync_http_assign_and_resync_results_match_room_sync_diagnostic(); };
  it("test_room_service_issues_scoped_sfu_command_tokens") { test_room_service_issues_scoped_sfu_command_tokens(); };
  it("test_room_service_config_reads_control_tokens_from_env") { test_room_service_config_reads_control_tokens_from_env(); };
  it("test_room_service_http_control_token_protects_modifying_commands") { test_room_service_http_control_token_protects_modifying_commands(); };
  it("test_room_service_facade_join_publish_and_subscribe") { test_room_service_facade_join_publish_and_subscribe(); };
  it("test_room_service_revocation_fanout_tracks_live_sfu_membership") { test_room_service_revocation_fanout_tracks_live_sfu_membership(); };
  it("test_room_service_routes_rooms_to_registered_sfu_nodes") { test_room_service_routes_rooms_to_registered_sfu_nodes(); };
  it("test_room_sync_http_warning_paths_surface_skipped_replay_state") { test_room_sync_http_warning_paths_surface_skipped_replay_state(); };
  it("test_room_sync_http_failed_replay_is_reflected_in_room_sync_diagnostic") { test_room_sync_http_failed_replay_is_reflected_in_room_sync_diagnostic(); };
  it("test_room_service_resync_room_repairs_sfu_drift") { test_room_service_resync_room_repairs_sfu_drift(); };
  it("test_room_service_conference_policy_materializes_and_syncs_screen_pin_and_active_speaker") { test_room_service_conference_policy_materializes_and_syncs_screen_pin_and_active_speaker(); };
  it("test_room_service_call_center_policy_materializes_and_syncs_supervisor_modes") { test_room_service_call_center_policy_materializes_and_syncs_supervisor_modes(); };
  it("test_room_service_http_call_center_policy_command_syncs_to_sfu_node") { test_room_service_http_call_center_policy_command_syncs_to_sfu_node(); };
  it("test_room_service_http_call_center_priority_queue_commands") { test_room_service_http_call_center_priority_queue_commands(); };
  it("test_room_service_recording_commands_sync_to_sfu_node") { test_room_service_recording_commands_sync_to_sfu_node(); };
}
