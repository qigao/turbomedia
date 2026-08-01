#include "tinytest_compat.h"
#include "room_service/config.h"
#include "room_service/http_api.h"
#include "room_service/server.h"
#include "sfu_node/config.h"
#include "sfu_node/server.h"
#include "http_client.h"
#include "turbo_media_auth.h"
#include "turbo_parser.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ROOM_SERVICE_HTTP_LIFECYCLE_STRESS_ITERATIONS 32

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

  value = turbo_json_object_get(obj, key);
  if (!value || turbo_json_type(value) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  return value;
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

static json_value_t *json_array_field(const json_value_t *obj, const char *key) {
  json_value_t *value;

  if (!obj || !key) {
    return NULL;
  }

  value = turbo_json_object_get(obj, key);
  if (!value || turbo_json_type(value) != TURBO_JSON_ARRAY) {
    return NULL;
  }

  return value;
}

static json_value_t *json_array_object_at(const json_value_t *array, size_t index) {
  json_value_t *value;

  if (!array || turbo_json_type(array) != TURBO_JSON_ARRAY ||
      index >= turbo_json_array_size(array)) {
    return NULL;
  }

  value = turbo_json_array_get(array, index);
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

static int json_is_null_field(const json_value_t *obj, const char *key) {
  json_value_t *value;

  if (!obj || !key) {
    return 0;
  }

  value = turbo_json_object_get(obj, key);
  return value && turbo_json_type(value) == TURBO_JSON_NULL;
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

  for (i = 0; i < turbo_json_array_size(events); ++i) {
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
  http_client_set_user_agent(client, "TurboRoomServiceTest/0.1");
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

static int wait_for_http_status_ok(const char *base_url, const char *path,
                                   int attempts, unsigned int sleep_ms) {
  int i;

  if (!base_url || !path || attempts <= 0) {
    return -1;
  }

  for (i = 0; i < attempts; ++i) {
    http_client_t *client = http_client_create(base_url);
    http_response_t *response = NULL;
    int ok = 0;

    if (client) {
      http_client_set_timeout(client, 1000);
      http_client_set_user_agent(client, "TurboRoomServiceTest/0.1");
      response = http_get(client, path);
      ok = response && response->error_code == HTTP_ERROR_NONE &&
           response->status_code >= 200 && response->status_code < 300;
    }

    if (response) {
      http_response_free(response);
    }
    if (client) {
      http_client_destroy(client);
    }
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
    http_client_t *client = http_client_create(base_url);
    http_response_t *response = NULL;
    turbo_tls_client_config_t tls_config = {
        .ca_file = ca_file,
        .verify_peer = 1
    };
    int ok = 0;

    if (client &&
        http_client_set_tls_client_config(client, &tls_config) == 0) {
      http_client_set_timeout(client, 1000);
      response = http_get(client, path);
      ok = response && response->error_code == HTTP_ERROR_NONE &&
           response->status_code >= 200 && response->status_code < 300;
    }
    if (response) {
      http_response_free(response);
    }
    http_client_destroy(client);
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
  http_client_t *client;
  http_response_t *response;
  turbo_tls_client_config_t tls_config = {
      .ca_file = ca_file,
      .verify_peer = 1
  };
  json_value_t *root = NULL;

  if (!base_url || !path || !json_body || !ca_file) {
    return NULL;
  }
  client = http_client_create(base_url);
  if (!client ||
      http_client_set_tls_client_config(client, &tls_config) != 0) {
    http_client_destroy(client);
    return NULL;
  }
  http_client_set_timeout(client, 3000);
  response = http_post_json(client, path, json_body);
  if (response && response->error_code == HTTP_ERROR_NONE &&
      response->status_code >= 200 && response->status_code < 300 &&
      http_response_is_json(response)) {
    root = http_response_parse_json(response);
  }
  if (response) {
    http_response_free(response);
  }
  http_client_destroy(client);
  return root;
}

static json_value_t *http_post_json_result(const char *base_url, const char *path,
                                           const char *json_body) {
  http_client_t *client;
  http_response_t *response;
  json_value_t *root = NULL;

  if (!base_url || !path || !json_body) {
    return NULL;
  }

  client = http_client_create(base_url);
  if (!client) {
    return NULL;
  }

  http_client_set_timeout(client, 3000);
  http_client_set_user_agent(client, "TurboRoomServiceTest/0.1");
  response = http_post_json(client, path, json_body);
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

static int http_post_json_status(const char *base_url, const char *path,
                                 const char *json_body, json_value_t **out_root) {
  http_client_t *client;
  http_response_t *response;
  json_value_t *root = NULL;
  int status = -1;

  if (out_root) {
    *out_root = NULL;
  }
  if (!base_url || !path || !json_body) {
    return -1;
  }

  client = http_client_create(base_url);
  if (!client) {
    return -1;
  }

  http_client_set_timeout(client, 3000);
  http_client_set_user_agent(client, "TurboRoomServiceTest/0.1");
  response = http_post_json(client, path, json_body);
  if (response && response->error_code == HTTP_ERROR_NONE &&
      http_response_is_json(response)) {
    status = response->status_code;
    root = http_response_parse_json(response);
  }

  if (response) {
    http_response_free(response);
  }
  http_client_destroy(client);

  if (root && turbo_json_type(root) == TURBO_JSON_OBJECT && out_root) {
    *out_root = root;
  } else {
    turbo_free_json(&root);
  }

  return status;
}

static int http_post_json_status_with_token(const char *base_url, const char *path,
                                            const char *json_body,
                                            const char *bearer_token,
                                            json_value_t **out_root) {
  http_client_t *client;
  http_response_t *response;
  json_value_t *root = NULL;
  int status = -1;

  if (out_root) {
    *out_root = NULL;
  }
  if (!base_url || !path || !json_body) {
    return -1;
  }

  client = http_client_create(base_url);
  if (!client) {
    return -1;
  }

  http_client_set_timeout(client, 3000);
  http_client_set_user_agent(client, "TurboRoomServiceTest/0.1");
  if (bearer_token) {
    http_client_set_bearer_token(client, bearer_token);
  }
  response = http_post_json(client, path, json_body);
  if (response && response->error_code == HTTP_ERROR_NONE &&
      http_response_is_json(response)) {
    status = response->status_code;
    root = http_response_parse_json(response);
  }

  if (response) {
    http_response_free(response);
  }
  http_client_destroy(client);

  if (root && turbo_json_type(root) == TURBO_JSON_OBJECT && out_root) {
    *out_root = root;
  } else {
    turbo_free_json(&root);
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

  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_NOT_NULL(service);
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_create_room(service, &room));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_add_participant(service, "room-replay", &alice));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_add_participant(service, "room-replay", &bob));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_publish_track(service, "room-replay", &cam));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_set_subscription(service, "room-replay", &cam_sub));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_assign_sfu_node(service, "room-replay",
                                                              "node-eu-1"));
  {
    room_service_sfu_replay_stats_t replay_stats;
    memset(&replay_stats, 0, sizeof(replay_stats));
    TEST_ASSERT_EQUAL_INT(0, room_service_app_server_sync_replay_room_state(
                                 room_server, "room-replay", &replay_stats));
    TEST_ASSERT_EQUAL_INT(2, replay_stats.participants_replayed);
    TEST_ASSERT_EQUAL_INT(2, replay_stats.receiver_bandwidths_replayed);
    TEST_ASSERT_EQUAL_INT(1, replay_stats.tracks_replayed);
    TEST_ASSERT_EQUAL_INT(1, replay_stats.subscriptions_replayed);
    TEST_ASSERT_EQUAL_INT(0, replay_stats.skipped_items);
    TEST_ASSERT_EQUAL_INT(0, room_service_app_server_record_room_sync(
                                 room_server, "room-replay", "replay", &replay_stats,
                                 NULL, NULL));
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
  TEST_ASSERT_NOT_NULL(sfu_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_server));
  TEST_ASSERT_EQUAL_INT(
      0, wait_for_http_status_ok(room_config.sfu_control_url, "/health", 30, 100));

  room_server = room_service_app_server_create(&room_config);
  TEST_ASSERT_NOT_NULL(room_server);
  service = room_service_app_server_get_service(room_server);
  TEST_ASSERT_NOT_NULL(service);

  seed_room_runtime(room_server, service);
  app_test_sleep_ms(100);

  diag_json = room_service_http_api_build_room_diagnostic(room_server, "room-replay");
  TEST_ASSERT_NOT_NULL(diag_json);
  TEST_ASSERT_EQUAL_INT(0, parse_json_text(diag_json, &root));
  free(diag_json);
  diag_json = NULL;
  diag = root;
  TEST_ASSERT_NOT_NULL(diag);
  TEST_ASSERT_TRUE(json_bool_value(diag, "runtime_sync_expected", 0));
  TEST_ASSERT_TRUE(json_bool_value(diag, "in_sync", 0));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(diag, "participant_mismatch_count", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(diag, "subscription_mismatch_count", -1));
  TEST_ASSERT_EQUAL_size_t(2, json_array_count(diag, "participant_bandwidth_diagnostics"));
  TEST_ASSERT_EQUAL_size_t(1, json_array_count(diag, "subscription_diagnostics"));
  last_sync = json_object_field(diag, "last_sfu_sync");
  TEST_ASSERT_NOT_NULL(last_sync);
  TEST_ASSERT_EQUAL_STRING("replay", json_string_value(last_sync, "operation"));
  TEST_ASSERT_FALSE(json_bool_value(last_sync, "had_warning", 1));
  last_sync_stats = json_object_field(last_sync, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(last_sync_stats);
  TEST_ASSERT_EQUAL_INT(2, json_int_value(last_sync_stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2,
                        json_int_value(last_sync_stats, "receiver_bandwidths_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(last_sync_stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(last_sync_stats, "subscriptions_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(last_sync_stats, "skipped_items", -1));
  turbo_free_json(&root);

  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_close_room(service, "room-replay"));
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_sync_force_close_room(room_server,
                                                                         "room-replay"));
  app_test_sleep_ms(100);

  diag_json = room_service_http_api_build_room_diagnostic(room_server, "room-replay");
  TEST_ASSERT_NOT_NULL(diag_json);
  TEST_ASSERT_EQUAL_INT(0, parse_json_text(diag_json, &root));
  free(diag_json);
  diag_json = NULL;
  diag = root;
  TEST_ASSERT_NOT_NULL(diag);
  TEST_ASSERT_FALSE(json_bool_value(diag, "runtime_sync_expected", 1));
  TEST_ASSERT_TRUE(json_bool_value(diag, "in_sync", 0));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(diag, "participant_mismatch_count", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(diag, "subscription_mismatch_count", -1));
  TEST_ASSERT_EQUAL_size_t(0, json_array_count(diag, "participant_bandwidth_diagnostics"));
  TEST_ASSERT_EQUAL_size_t(0, json_array_count(diag, "subscription_diagnostics"));
  last_sync = json_object_field(diag, "last_sfu_sync");
  TEST_ASSERT_NOT_NULL(last_sync);
  TEST_ASSERT_EQUAL_STRING("replay", json_string_value(last_sync, "operation"));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(room_server);
  service = room_service_app_server_get_service(room_server);
  TEST_ASSERT_NOT_NULL(service);
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_create_room(service, &room));

  diag_json = room_service_http_api_build_room_sync_diagnostic(room_server, "room-no-sync");
  TEST_ASSERT_NOT_NULL(diag_json);
  TEST_ASSERT_EQUAL_INT(0, parse_json_text(diag_json, &root));
  free(diag_json);
  diag_json = NULL;
  diag = root;

  TEST_ASSERT_NOT_NULL(diag);
  TEST_ASSERT_FALSE(json_bool_value(diag, "has_last_sfu_sync", 1));
  TEST_ASSERT_TRUE(json_is_null_field(diag, "last_sfu_sync"));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(sfu_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_server));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100));

  room_server = room_service_app_server_create(&room_config);
  TEST_ASSERT_NOT_NULL(room_server);
  service = room_service_app_server_get_service(room_server);
  TEST_ASSERT_NOT_NULL(service);

  seed_room_runtime(room_server, service);
  app_test_sleep_ms(100);

  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/health", 30, 100));

  get_root = http_get_json(room_service_base_url,
                           "/api/v1/rooms/room-replay/room_sync_diagnostic");
  TEST_ASSERT_NOT_NULL(get_root);
  TEST_ASSERT_TRUE(json_bool_value(get_root, "has_last_sfu_sync", 0));
  last_sync = json_object_field(get_root, "last_sfu_sync");
  TEST_ASSERT_NOT_NULL(last_sync);
  TEST_ASSERT_EQUAL_STRING("replay", json_string_value(last_sync, "operation"));
  stats = json_object_field(last_sync, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "tracks_replayed", -1));

  post_root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{\"type\":\"get_room_sync_diagnostic\",\"room_id\":\"room-replay\"}");
  TEST_ASSERT_NOT_NULL(post_root);
  post_diag = json_object_field(post_root, "room_sync_diagnostic");
  TEST_ASSERT_NOT_NULL(post_diag);
  TEST_ASSERT_TRUE(json_bool_value(post_diag, "has_last_sfu_sync", 0));
  last_sync = json_object_field(post_diag, "last_sfu_sync");
  TEST_ASSERT_NOT_NULL(last_sync);
  TEST_ASSERT_EQUAL_STRING("replay", json_string_value(last_sync, "operation"));
  stats = json_object_field(last_sync, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2,
                        json_int_value(stats, "receiver_bandwidths_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "subscriptions_replayed", -1));

  turbo_free_json(&post_root);
  turbo_free_json(&get_root);
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
  TEST_ASSERT_NOT_NULL(sfu_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_server));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100));

  room_server = room_service_app_server_create(&room_config);
  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/health", 30, 100));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-http-sync\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-http-sync\","
      "\"node_id\":\"node-eu-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  stats = json_object_field(root, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "receiver_bandwidths_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "subscriptions_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "skipped_items", -1));
  turbo_free_json(&root);
  root = NULL;

  sync_diag_root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_sync_diagnostic\","
      "\"room_id\":\"room-http-sync\""
      "}");
  TEST_ASSERT_NOT_NULL(sync_diag_root);
  room_sync_diag = json_object_field(sync_diag_root, "room_sync_diagnostic");
  TEST_ASSERT_NOT_NULL(room_sync_diag);
  TEST_ASSERT_TRUE(json_bool_value(room_sync_diag, "has_last_sfu_sync", 0));
  last_sync = json_object_field(room_sync_diag, "last_sfu_sync");
  TEST_ASSERT_NOT_NULL(last_sync);
  TEST_ASSERT_EQUAL_STRING("assign", json_string_value(last_sync, "operation"));
  stats = json_object_field(last_sync, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "receiver_bandwidths_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "subscriptions_replayed", -1));
  turbo_free_json(&sync_diag_root);
  sync_diag_root = NULL;

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_receiver_bandwidth\","
      "\"room_id\":\"room-http-sync\","
      "\"participant_id\":\"bob\","
      "\"bandwidth_bps\":123000"
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"resync_room\","
      "\"room_id\":\"room-http-sync\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  stats = json_object_field(root, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "receiver_bandwidths_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "subscriptions_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "skipped_items", -1));
  turbo_free_json(&root);
  root = NULL;

  sync_diag_root = http_get_json(room_service_base_url,
                                 "/api/v1/rooms/room-http-sync/room_sync_diagnostic");
  TEST_ASSERT_NOT_NULL(sync_diag_root);
  TEST_ASSERT_TRUE(json_bool_value(sync_diag_root, "has_last_sfu_sync", 0));
  last_sync = json_object_field(sync_diag_root, "last_sfu_sync");
  TEST_ASSERT_NOT_NULL(last_sync);
  TEST_ASSERT_EQUAL_STRING("resync", json_string_value(last_sync, "operation"));
  stats = json_object_field(last_sync, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "receiver_bandwidths_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "subscriptions_replayed", -1));
  turbo_free_json(&sync_diag_root);

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
  sfu_config.bind_host = "0.0.0.0";
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
  TEST_ASSERT_NOT_NULL(sfu_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_server));
  TEST_ASSERT_EQUAL_INT(
      0, wait_for_https_status_ok(
             sfu_node_base_url, "/health", ROOM_SERVICE_TEST_TLS_CERT_PATH,
             30, 100));

  room_server = room_service_app_server_create(&room_config);
  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/health", 30, 100));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-auth-forward\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-auth-forward\","
      "\"node_id\":\"node-auth-forward\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  stats = json_object_field(root, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "skipped_items", -1));
  turbo_free_json(&root);
  root = NULL;

  root = https_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_stats\","
      "\"room_id\":\"room-auth-forward\""
      "}",
      ROOM_SERVICE_TEST_TLS_CERT_PATH);
  TEST_ASSERT_NOT_NULL(root);
  stats = json_object_field(root, "room_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_STRING("room-auth-forward", json_string_value(stats, "room_id"));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "participant_count", -1));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/health", 30, 100));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/metrics", 30, 100));
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
  TEST_ASSERT_NOT_NULL(write_token);
  TEST_ASSERT_NOT_NULL(cross_room_token);
  TEST_ASSERT_NOT_NULL(expired_token);
  TEST_ASSERT_NOT_NULL(previous_dangerous_token);
  TEST_ASSERT_NOT_NULL(alice_token);
  TEST_ASSERT_NOT_NULL(bob_token);

  TEST_ASSERT_EQUAL_INT(200, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 get_queue_depth_command, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(404, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 peek_queue_command, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 pop_queue_command, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 unknown_get_command, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_room_command, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_room_command, "wrong-token", NULL));
  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_room_command, cross_room_token, NULL));
  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_room_command, expired_token, NULL));

  TEST_ASSERT_EQUAL_INT(200, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_room_command, write_token, &root));
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
  root = NULL;
  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 close_room_command, write_token, NULL));
  TEST_ASSERT_EQUAL_INT(200, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 close_room_command, previous_dangerous_token, NULL));
  TEST_ASSERT_EQUAL_INT(200, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/commands",
                                 create_static_room_command,
                                 "room-service-control-token", &root));
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/join",
                                 join_alice_request, bob_token, NULL));
  TEST_ASSERT_EQUAL_INT(200, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/join",
                                 join_alice_request, alice_token, NULL));

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
  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/health", 30, 100));

  TEST_ASSERT_EQUAL_INT(401, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/join",
                                 join_alice, NULL, NULL));

  TEST_ASSERT_EQUAL_INT(200, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/join",
                                 join_alice, "room-service-control-token", &root));
  TEST_ASSERT_NOT_NULL(root);
  room = json_object_field(root, "room");
  participant = json_object_field(root, "participant");
  TEST_ASSERT_NOT_NULL(room);
  TEST_ASSERT_NOT_NULL(participant);
  TEST_ASSERT_EQUAL_STRING("room-facade", json_string_value(room, "room_id"));
  TEST_ASSERT_EQUAL_STRING("alice", json_string_value(participant, "participant_id"));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(room, "participant_count", 0));
  turbo_free_json(&root);

  TEST_ASSERT_EQUAL_INT(200, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/join",
                                 join_bob, "room-service-control-token", &root));
  TEST_ASSERT_NOT_NULL(root);
  room = json_object_field(root, "room");
  participant = json_object_field(root, "participant");
  TEST_ASSERT_NOT_NULL(room);
  TEST_ASSERT_NOT_NULL(participant);
  TEST_ASSERT_EQUAL_STRING("bob", json_string_value(participant, "participant_id"));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(room, "participant_count", 0));
  turbo_free_json(&root);

  TEST_ASSERT_EQUAL_INT(200, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/publish",
                                 publish_track, "room-service-control-token", &root));
  TEST_ASSERT_NOT_NULL(root);
  room = json_object_field(root, "room");
  track = json_object_field(root, "track");
  TEST_ASSERT_NOT_NULL(room);
  TEST_ASSERT_NOT_NULL(track);
  TEST_ASSERT_EQUAL_STRING("track-cam", json_string_value(track, "track_id"));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(room, "published_track_count", 0));
  turbo_free_json(&root);

  TEST_ASSERT_EQUAL_INT(200, http_post_json_status_with_token(
                                 room_service_base_url, "/api/v1/subscribe",
                                 subscribe_track, "room-service-control-token", &root));
  TEST_ASSERT_NOT_NULL(root);
  room = json_object_field(root, "room");
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(room);
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_EQUAL_STRING("bob",
                           json_string_value(subscription, "subscriber_participant_id"));
  TEST_ASSERT_EQUAL_STRING("track-cam", json_string_value(subscription, "track_id"));
  TEST_ASSERT_TRUE(json_bool_value(subscription, "enabled", 0));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(room, "subscription_count", 0));
  turbo_free_json(&root);

  room_service_app_server_stop(room_server);
  room_service_app_server_destroy(room_server);
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
  TEST_ASSERT_NOT_NULL(sfu_a_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_a_server));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_a_base_url, "/health", 30, 100));

  sfu_b_server = sfu_node_app_server_create(&sfu_b_config);
  TEST_ASSERT_NOT_NULL(sfu_b_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_b_server));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_b_base_url, "/health", 30, 100));

  root = http_get_json(sfu_a_base_url, "/health");
  TEST_ASSERT_NOT_NULL(root);
  node_stats = json_object_field(root, "node_stats");
  TEST_ASSERT_NOT_NULL(node_stats);
  TEST_ASSERT_EQUAL_STRING("sfu-a", json_string_value(node_stats, "node_id"));
  turbo_free_json(&root);

  root = http_get_json(sfu_b_base_url, "/health");
  TEST_ASSERT_NOT_NULL(root);
  node_stats = json_object_field(root, "node_stats");
  TEST_ASSERT_NOT_NULL(node_stats);
  TEST_ASSERT_EQUAL_STRING("sfu-b", json_string_value(node_stats, "node_id"));
  turbo_free_json(&root);

  room_server = room_service_app_server_create(&room_config);
  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_TRUE(room_service_app_server_has_sfu_node(room_server, "sfu-a"));
  TEST_ASSERT_TRUE(room_service_app_server_has_sfu_node(room_server, "sfu-b"));
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/health", 30, 100));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-on-a\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-a\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-on-b\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-b\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-on-a\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  room = json_object_field(root, "room");
  TEST_ASSERT_NOT_NULL(room);
  TEST_ASSERT_EQUAL_STRING("sfu-a", json_string_value(room, "assigned_sfu_node"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-on-b\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  room = json_object_field(root, "room");
  TEST_ASSERT_NOT_NULL(room);
  TEST_ASSERT_EQUAL_STRING("sfu-b", json_string_value(room, "assigned_sfu_node"));
  turbo_free_json(&root);

  TEST_ASSERT_EQUAL_INT(200, http_post_json_status(
                                 sfu_a_base_url, "/api/v1/commands",
                                 "{"
                                 "\"type\":\"get_room_stats\","
                                 "\"room_id\":\"room-on-a\""
                                 "}",
                                 NULL));
  TEST_ASSERT_EQUAL_INT(404, http_post_json_status(
                                 sfu_a_base_url, "/api/v1/commands",
                                 "{"
                                 "\"type\":\"get_room_stats\","
                                 "\"room_id\":\"room-on-b\""
                                 "}",
                                 NULL));
  TEST_ASSERT_EQUAL_INT(404, http_post_json_status(
                                 sfu_b_base_url, "/api/v1/commands",
                                 "{"
                                 "\"type\":\"get_room_stats\","
                                 "\"room_id\":\"room-on-a\""
                                 "}",
                                 NULL));
  TEST_ASSERT_EQUAL_INT(200, http_post_json_status(
                                 sfu_b_base_url, "/api/v1/commands",
                                 "{"
                                 "\"type\":\"get_room_stats\","
                                 "\"room_id\":\"room-on-b\""
                                 "}",
                                 NULL));

  TEST_ASSERT_EQUAL_INT(404, http_post_json_status(
                                 room_service_base_url, "/api/v1/commands",
                                 "{"
                                 "\"type\":\"assign_sfu_node\","
                                 "\"room_id\":\"room-on-a\","
                                 "\"node_id\":\"missing-sfu\""
                                 "}",
                                 NULL));

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
                   "node-a=http://127.0.0.1:19423,node-b=http://127.0.0.1:19424");
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
  TEST_ASSERT_EQUAL_STRING("env-room-control-token", room_config.control_token);
  TEST_ASSERT_EQUAL_STRING("env-sfu-control-token", room_config.sfu_control_token);
  TEST_ASSERT_EQUAL_STRING("node-a=http://127.0.0.1:19423,node-b=http://127.0.0.1:19424",
                           room_config.sfu_nodes);
  TEST_ASSERT_EQUAL_INT(1, room_config.use_tls);
  TEST_ASSERT_EQUAL_STRING(ROOM_SERVICE_TEST_TLS_CERT_PATH,
                           room_config.tls_cert_file);
  TEST_ASSERT_EQUAL_STRING(ROOM_SERVICE_TEST_TLS_KEY_PATH,
                           room_config.tls_key_file);
  TEST_ASSERT_EQUAL_STRING(ROOM_SERVICE_TEST_TLS_CERT_PATH,
                           room_config.sfu_ca_file);
  TEST_ASSERT_EQUAL_STRING("env-room-key", room_config.auth_active_key_id);
  TEST_ASSERT_EQUAL_STRING("env-room-active-secret-at-least-32-bytes",
                           room_config.auth_active_secret);
  TEST_ASSERT_EQUAL_STRING("env-sfu-key", room_config.sfu_auth_key_id);
  TEST_ASSERT_EQUAL_STRING("env-sfu-command-secret-at-least-32-bytes",
                           room_config.sfu_auth_secret);
  TEST_ASSERT_EQUAL_INT(45, room_config.sfu_auth_ttl_seconds);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_config_validate(&room_config));

  app_test_restore_env("TURBO_ROOM_SERVICE_CONTROL_TOKEN", saved_control_token);
  app_test_restore_env("TURBO_ROOM_SERVICE_SFU_CONTROL_TOKEN", saved_sfu_control_token);
  app_test_restore_env("TURBO_ROOM_SERVICE_SFU_NODES", saved_sfu_nodes);
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
  TEST_ASSERT_NOT_NULL(sfu_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_server));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100));

  room_server = room_service_app_server_create(&room_config);
  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/health", 30, 100));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-http-warning\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_NULL(json_string_value(root, "warning_code"));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_NULL(json_string_value(root, "warning_code"));
  turbo_free_json(&root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-http-warning\","
      "\"node_id\":\"node-eu-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_STRING("SFU_SYNC_SKIPPED", json_string_value(root, "warning_code"));
  stats = json_object_field(root, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "receiver_bandwidths_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "subscriptions_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "skipped_items", -1));
  turbo_free_json(&root);
  root = NULL;

  sync_diag_root = http_get_json(room_service_base_url,
                                 "/api/v1/rooms/room-http-warning/room_sync_diagnostic");
  TEST_ASSERT_NOT_NULL(sync_diag_root);
  TEST_ASSERT_TRUE(json_bool_value(sync_diag_root, "has_last_sfu_sync", 0));
  last_sync = json_object_field(sync_diag_root, "last_sfu_sync");
  TEST_ASSERT_NOT_NULL(last_sync);
  TEST_ASSERT_EQUAL_STRING("assign", json_string_value(last_sync, "operation"));
  TEST_ASSERT_TRUE(json_bool_value(last_sync, "had_warning", 0));
  TEST_ASSERT_EQUAL_STRING("SFU_SYNC_SKIPPED",
                           json_string_value(last_sync, "warning_code"));
  stats = json_object_field(last_sync, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "subscriptions_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "skipped_items", -1));
  turbo_free_json(&sync_diag_root);
  sync_diag_root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"resync_room\","
      "\"room_id\":\"room-http-warning\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_STRING("SFU_SYNC_SKIPPED", json_string_value(root, "warning_code"));
  stats = json_object_field(root, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "receiver_bandwidths_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "subscriptions_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "skipped_items", -1));
  turbo_free_json(&root);
  root = NULL;

  sync_diag_root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_sync_diagnostic\","
      "\"room_id\":\"room-http-warning\""
      "}");
  TEST_ASSERT_NOT_NULL(sync_diag_root);
  room_sync_diag = json_object_field(sync_diag_root, "room_sync_diagnostic");
  TEST_ASSERT_NOT_NULL(room_sync_diag);
  TEST_ASSERT_TRUE(json_bool_value(room_sync_diag, "has_last_sfu_sync", 0));
  last_sync = json_object_field(room_sync_diag, "last_sfu_sync");
  TEST_ASSERT_NOT_NULL(last_sync);
  TEST_ASSERT_EQUAL_STRING("resync", json_string_value(last_sync, "operation"));
  TEST_ASSERT_TRUE(json_bool_value(last_sync, "had_warning", 0));
  TEST_ASSERT_EQUAL_STRING("SFU_SYNC_SKIPPED",
                           json_string_value(last_sync, "warning_code"));
  stats = json_object_field(last_sync, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "subscriptions_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(stats, "skipped_items", -1));
  turbo_free_json(&sync_diag_root);

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
  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/health", 30, 100));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-http-failed\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);
  root = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-http-failed\","
      "\"node_id\":\"node-eu-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_STRING("SFU_SYNC_FAILED", json_string_value(root, "warning_code"));
  stats = json_object_field(root, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "receiver_bandwidths_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "subscriptions_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "skipped_items", -1));
  turbo_free_json(&root);
  root = NULL;

  sync_diag_root = http_get_json(room_service_base_url,
                                 "/api/v1/rooms/room-http-failed/room_sync_diagnostic");
  TEST_ASSERT_NOT_NULL(sync_diag_root);
  TEST_ASSERT_TRUE(json_bool_value(sync_diag_root, "has_last_sfu_sync", 0));
  last_sync = json_object_field(sync_diag_root, "last_sfu_sync");
  TEST_ASSERT_NOT_NULL(last_sync);
  TEST_ASSERT_EQUAL_STRING("assign", json_string_value(last_sync, "operation"));
  TEST_ASSERT_TRUE(json_bool_value(last_sync, "had_warning", 0));
  TEST_ASSERT_EQUAL_STRING("SFU_SYNC_FAILED",
                           json_string_value(last_sync, "warning_code"));
  stats = json_object_field(last_sync, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "receiver_bandwidths_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "subscriptions_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(stats, "skipped_items", -1));
  turbo_free_json(&sync_diag_root);

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
  TEST_ASSERT_NOT_NULL(sfu_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_server));
  TEST_ASSERT_EQUAL_INT(
      0, wait_for_http_status_ok(room_config.sfu_control_url, "/health", 30, 100));

  room_server = room_service_app_server_create(&room_config);
  TEST_ASSERT_NOT_NULL(room_server);
  service = room_service_app_server_get_service(room_server);
  TEST_ASSERT_NOT_NULL(service);
  node = sfu_node_app_server_get_node(sfu_server);
  TEST_ASSERT_NOT_NULL(node);

  seed_room_runtime(room_server, service);
  app_test_sleep_ms(100);

  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_set_receiver_bandwidth(node, "room-replay", "bob",
                                                                 123000));
  TEST_ASSERT_EQUAL_INT(0, turbo_sfu_node_set_track_subscription(
                               node, "room-replay", "bob", "track-cam", 0,
                               TURBO_ROOM_VIDEO_LAYER_NONE));

  diag_json = room_service_http_api_build_room_diagnostic(room_server, "room-replay");
  TEST_ASSERT_NOT_NULL(diag_json);
  TEST_ASSERT_EQUAL_INT(0, parse_json_text(diag_json, &root));
  free(diag_json);
  diag_json = NULL;
  diag = root;
  TEST_ASSERT_FALSE(json_bool_value(diag, "in_sync", 1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(diag, "participant_mismatch_count", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(diag, "subscription_mismatch_count", -1));
  turbo_free_json(&root);

  memset(&replay_stats, 0, sizeof(replay_stats));
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_sync_resync_room(
                               room_server, "room-replay", &replay_stats));
  TEST_ASSERT_EQUAL_INT(2, replay_stats.participants_replayed);
  TEST_ASSERT_EQUAL_INT(2, replay_stats.receiver_bandwidths_replayed);
  TEST_ASSERT_EQUAL_INT(1, replay_stats.tracks_replayed);
  TEST_ASSERT_EQUAL_INT(1, replay_stats.subscriptions_replayed);
  TEST_ASSERT_EQUAL_INT(0, replay_stats.skipped_items);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_record_room_sync(
                               room_server, "room-replay", "resync", &replay_stats,
                               NULL, NULL));
  app_test_sleep_ms(100);

  diag_json = room_service_http_api_build_room_diagnostic(room_server, "room-replay");
  TEST_ASSERT_NOT_NULL(diag_json);
  TEST_ASSERT_EQUAL_INT(0, parse_json_text(diag_json, &root));
  free(diag_json);
  diag_json = NULL;
  diag = root;
  TEST_ASSERT_TRUE(json_bool_value(diag, "in_sync", 0));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(diag, "participant_mismatch_count", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(diag, "subscription_mismatch_count", -1));
  last_sync = json_object_field(diag, "last_sfu_sync");
  TEST_ASSERT_NOT_NULL(last_sync);
  TEST_ASSERT_EQUAL_STRING("resync", json_string_value(last_sync, "operation"));
  TEST_ASSERT_FALSE(json_bool_value(last_sync, "had_warning", 1));
  last_sync_stats = json_object_field(last_sync, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(last_sync_stats);
  TEST_ASSERT_EQUAL_INT(2, json_int_value(last_sync_stats, "participants_replayed", -1));
  TEST_ASSERT_EQUAL_INT(2,
                        json_int_value(last_sync_stats, "receiver_bandwidths_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(last_sync_stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(last_sync_stats, "subscriptions_replayed", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(last_sync_stats, "skipped_items", -1));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(sfu_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_server));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100));

  room_server = room_service_app_server_create(&room_config);
  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/health", 30, 100));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-policy\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-policy\","
      "\"node_id\":\"node-eu-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  room = json_object_field(root, "room");
  TEST_ASSERT_NOT_NULL(room);
  TEST_ASSERT_EQUAL_STRING("node-eu-1", json_string_value(room, "assigned_sfu_node"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_layout_mode\","
      "\"room_id\":\"room-policy\","
      "\"layout_mode\":\"speaker\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  policy = json_object_field(root, "conference_policy");
  TEST_ASSERT_NOT_NULL(policy);
  TEST_ASSERT_EQUAL_STRING("speaker", json_string_value(policy, "layout_mode"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_active_speaker\","
      "\"room_id\":\"room-policy\","
      "\"participant_id\":\"alice\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  policy = json_object_field(root, "conference_policy");
  TEST_ASSERT_NOT_NULL(policy);
  TEST_ASSERT_EQUAL_STRING("alice",
                           json_string_value(policy, "active_speaker_participant_id"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_pin\","
      "\"room_id\":\"room-policy\","
      "\"participant_id\":\"charlie\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  policy = json_object_field(root, "conference_policy");
  TEST_ASSERT_NOT_NULL(policy);
  TEST_ASSERT_EQUAL_STRING("charlie", json_string_value(policy, "pinned_participant_id"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"apply_conference_policy\","
      "\"room_id\":\"room-policy\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_NULL(json_string_value(root, "warning_code"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"alice-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_TRUE(json_bool_value(subscription, "enabled", 0));
  TEST_ASSERT_EQUAL_INT(400, json_int_value(subscription, "priority", -1));
  TEST_ASSERT_EQUAL_STRING("low", json_string_value(subscription, "target_layer"));
  TEST_ASSERT_EQUAL_STRING("conference_policy_audio",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"charlie-cam\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_TRUE(json_bool_value(subscription, "enabled", 0));
  TEST_ASSERT_EQUAL_INT(250, json_int_value(subscription, "priority", -1));
  TEST_ASSERT_EQUAL_STRING("high", json_string_value(subscription, "target_layer"));
  TEST_ASSERT_EQUAL_STRING("conference_policy_pin",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"alice-cam\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_TRUE(json_bool_value(subscription, "enabled", 0));
  TEST_ASSERT_EQUAL_INT(100, json_int_value(subscription, "priority", -1));
  TEST_ASSERT_EQUAL_STRING("low", json_string_value(subscription, "target_layer"));
  TEST_ASSERT_EQUAL_STRING("conference_policy_camera",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"dave-screen\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_TRUE(json_bool_value(subscription, "enabled", 0));
  TEST_ASSERT_EQUAL_INT(300, json_int_value(subscription, "priority", -1));
  TEST_ASSERT_EQUAL_STRING("high", json_string_value(subscription, "target_layer"));
  TEST_ASSERT_EQUAL_STRING("conference_policy_screen",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription_diagnostic\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"dave-screen\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription_diag = json_object_field(root, "subscription_diagnostic");
  TEST_ASSERT_NOT_NULL(subscription_diag);
  TEST_ASSERT_EQUAL_STRING("ok", json_string_value(subscription_diag, "sfu_mirror_status"));
  TEST_ASSERT_TRUE(json_bool_value(subscription_diag, "in_sync", 0));
  sfu_track_subscription = json_object_field(subscription_diag, "sfu_track_subscription");
  TEST_ASSERT_NOT_NULL(sfu_track_subscription);
  TEST_ASSERT_EQUAL_INT(300, json_int_value(sfu_track_subscription, "priority", -1));
  TEST_ASSERT_EQUAL_STRING("high", json_string_value(sfu_track_subscription, "preferred_layer"));
  TEST_ASSERT_EQUAL_STRING("high", json_string_value(sfu_track_subscription, "target_layer"));
  TEST_ASSERT_EQUAL_INT(0, json_bool_value(sfu_track_subscription, "muted", 1));
  TEST_ASSERT_EQUAL_STRING("conference_policy_screen",
                           json_string_value(sfu_track_subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_get_json(room_service_base_url, "/api/v1/rooms/room-policy/state");
  TEST_ASSERT_NOT_NULL(root);
  room_state = root;
  state_room = json_object_field(room_state, "room");
  state_policy = json_object_field(room_state, "conference_policy");
  TEST_ASSERT_NOT_NULL(state_room);
  TEST_ASSERT_NOT_NULL(state_policy);
  TEST_ASSERT_EQUAL_STRING("room-policy", json_string_value(state_room, "room_id"));
  TEST_ASSERT_EQUAL_STRING("speaker", json_string_value(state_policy, "layout_mode"));
  TEST_ASSERT_EQUAL_STRING("alice",
                           json_string_value(state_policy,
                                             "active_speaker_participant_id"));
  TEST_ASSERT_EQUAL_STRING("charlie",
                           json_string_value(state_policy,
                                             "pinned_participant_id"));
  TEST_ASSERT_EQUAL_INT(4, (int)json_array_count(room_state, "participants"));
  TEST_ASSERT_EQUAL_INT(4, (int)json_array_count(room_state, "published_tracks"));
  TEST_ASSERT_EQUAL_INT(12, (int)json_array_count(room_state, "subscriptions"));
  turbo_free_json(&root);
  room_state = NULL;

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_state\","
      "\"room_id\":\"room-policy\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  room_state = json_object_field(root, "room_state");
  TEST_ASSERT_NOT_NULL(room_state);
  state_room = json_object_field(room_state, "room");
  state_policy = json_object_field(room_state, "conference_policy");
  TEST_ASSERT_NOT_NULL(state_room);
  TEST_ASSERT_NOT_NULL(state_policy);
  TEST_ASSERT_EQUAL_STRING("room-policy", json_string_value(state_room, "room_id"));
  TEST_ASSERT_EQUAL_STRING("speaker", json_string_value(state_policy, "layout_mode"));
  TEST_ASSERT_EQUAL_INT(4, (int)json_array_count(room_state, "participants"));
  TEST_ASSERT_EQUAL_INT(4, (int)json_array_count(room_state, "published_tracks"));
  TEST_ASSERT_EQUAL_INT(12, (int)json_array_count(room_state, "subscriptions"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_diagnostic\","
      "\"room_id\":\"room-policy\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  diag = json_object_field(root, "room_diagnostic");
  TEST_ASSERT_NOT_NULL(diag);
  conference_policy = json_object_field(diag, "conference_policy");
  TEST_ASSERT_NOT_NULL(conference_policy);
  TEST_ASSERT_EQUAL_STRING("speaker", json_string_value(conference_policy, "layout_mode"));
  TEST_ASSERT_EQUAL_STRING("alice",
                           json_string_value(conference_policy,
                                             "active_speaker_participant_id"));
  TEST_ASSERT_EQUAL_STRING("charlie",
                           json_string_value(conference_policy,
                                             "pinned_participant_id"));
  TEST_ASSERT_TRUE(json_bool_value(diag, "in_sync", 0));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(diag, "subscription_mismatch_count", -1));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_pin\","
      "\"room_id\":\"room-policy\","
      "\"participant_id\":\"\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  policy = json_object_field(root, "conference_policy");
  TEST_ASSERT_NOT_NULL(policy);
  TEST_ASSERT_EQUAL_STRING("", json_string_value(policy, "pinned_participant_id"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"apply_conference_policy\","
      "\"room_id\":\"room-policy\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_NULL(json_string_value(root, "warning_code"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"alice-cam\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_EQUAL_INT(200, json_int_value(subscription, "priority", -1));
  TEST_ASSERT_EQUAL_STRING("high", json_string_value(subscription, "target_layer"));
  TEST_ASSERT_EQUAL_STRING("conference_policy_active",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-policy\","
      "\"subscriber_participant_id\":\"bob\","
      "\"track_id\":\"charlie-cam\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_EQUAL_INT(100, json_int_value(subscription, "priority", -1));
  TEST_ASSERT_EQUAL_STRING("low", json_string_value(subscription, "target_layer"));
  TEST_ASSERT_EQUAL_STRING("conference_policy_camera",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_room_diagnostic\","
      "\"room_id\":\"room-policy\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  diag = json_object_field(root, "room_diagnostic");
  TEST_ASSERT_NOT_NULL(diag);
  conference_policy = json_object_field(diag, "conference_policy");
  TEST_ASSERT_NOT_NULL(conference_policy);
  TEST_ASSERT_EQUAL_STRING("alice",
                           json_string_value(conference_policy,
                                             "active_speaker_participant_id"));
  TEST_ASSERT_EQUAL_STRING("", json_string_value(conference_policy,
                                                  "pinned_participant_id"));
  TEST_ASSERT_TRUE(json_bool_value(diag, "in_sync", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(sfu_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_server));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100));

  room_server = room_service_app_server_create(&room_config);
  TEST_ASSERT_NOT_NULL(room_server);
  service = room_service_app_server_get_service(room_server);
  TEST_ASSERT_NOT_NULL(service);

  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_create_room(service, &room));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_add_participant(service, "room-call-center",
                                                              &customer));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_add_participant(service, "room-call-center",
                                                              &agent));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_add_participant(service, "room-call-center",
                                                              &supervisor));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_publish_track(service, "room-call-center",
                                                            &customer_audio));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_publish_track(service, "room-call-center",
                                                            &agent_audio));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_publish_track(service, "room-call-center",
                                                            &supervisor_audio));
  TEST_ASSERT_EQUAL_INT(0, turbo_room_service_assign_sfu_node(service, "room-call-center",
                                                              "node-eu-1"));
  memset(&replay_stats, 0, sizeof(replay_stats));
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_sync_replay_room_state(
                               room_server, "room-call-center", &replay_stats));
  TEST_ASSERT_EQUAL_INT(3, replay_stats.participants_replayed);
  TEST_ASSERT_EQUAL_INT(3, replay_stats.tracks_replayed);

  memset(&apply_result, 0, sizeof(apply_result));
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_apply_call_center_policy(
                               room_server, "room-call-center",
                               TURBO_CALL_CENTER_SUPERVISOR_MONITOR,
                               &apply_result));
  TEST_ASSERT_FALSE(apply_result.had_warning);
  TEST_ASSERT_EQUAL_INT(4, apply_result.subscriptions_applied);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "supervisor",
                               "customer-audio", &sfu_subscription));
  TEST_ASSERT_TRUE(sfu_subscription.found);
  TEST_ASSERT_TRUE(sfu_subscription.enabled);
  TEST_ASSERT_EQUAL_STRING("call_center_monitor", sfu_subscription.policy_source);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "customer",
                               "supervisor-audio", &sfu_subscription));
  TEST_ASSERT_FALSE(sfu_subscription.found);

  memset(&apply_result, 0, sizeof(apply_result));
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_apply_call_center_policy(
                               room_server, "room-call-center",
                               TURBO_CALL_CENTER_SUPERVISOR_WHISPER,
                               &apply_result));
  TEST_ASSERT_FALSE(apply_result.had_warning);
  TEST_ASSERT_EQUAL_INT(5, apply_result.subscriptions_applied);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "agent",
                               "supervisor-audio", &sfu_subscription));
  TEST_ASSERT_TRUE(sfu_subscription.found);
  TEST_ASSERT_TRUE(sfu_subscription.enabled);
  TEST_ASSERT_EQUAL_STRING("call_center_whisper", sfu_subscription.policy_source);

  memset(&apply_result, 0, sizeof(apply_result));
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_apply_call_center_policy(
                               room_server, "room-call-center",
                               TURBO_CALL_CENTER_SUPERVISOR_BARGE,
                               &apply_result));
  TEST_ASSERT_FALSE(apply_result.had_warning);
  TEST_ASSERT_EQUAL_INT(6, apply_result.subscriptions_applied);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "customer",
                               "supervisor-audio", &sfu_subscription));
  TEST_ASSERT_TRUE(sfu_subscription.found);
  TEST_ASSERT_TRUE(sfu_subscription.enabled);
  TEST_ASSERT_EQUAL_STRING("call_center_barge", sfu_subscription.policy_source);

  memset(&apply_result, 0, sizeof(apply_result));
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_apply_call_center_policy(
                               room_server, "room-call-center",
                               TURBO_CALL_CENTER_SUPERVISOR_NONE,
                               &apply_result));
  TEST_ASSERT_FALSE(apply_result.had_warning);
  TEST_ASSERT_EQUAL_INT(2, apply_result.subscriptions_applied);
  TEST_ASSERT_EQUAL_INT(4, apply_result.subscriptions_removed);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "customer",
                               "supervisor-audio", &sfu_subscription));
  TEST_ASSERT_TRUE(sfu_subscription.found);
  TEST_ASSERT_FALSE(sfu_subscription.enabled);
  TEST_ASSERT_TRUE(sfu_subscription.muted);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_fetch_track_subscription(
                               room_server, "room-call-center", "supervisor",
                               "customer-audio", &sfu_subscription));
  TEST_ASSERT_TRUE(sfu_subscription.found);
  TEST_ASSERT_FALSE(sfu_subscription.enabled);
  TEST_ASSERT_TRUE(sfu_subscription.muted);

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
  TEST_ASSERT_NOT_NULL(sfu_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_server));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100));

  room_server = room_service_app_server_create(&room_config);
  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(room_service_base_url, "/health", 30, 100));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-http-call-center\","
      "\"room_type\":\"call\","
      "\"created_by\":\"router-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-http-call-center\","
      "\"node_id\":\"node-eu-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_participant\","
      "\"room_id\":\"room-http-call-center\","
      "\"participant_id\":\"customer\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  participant = json_object_field(root, "participant");
  TEST_ASSERT_NOT_NULL(participant);
  TEST_ASSERT_EQUAL_STRING("customer", json_string_value(participant, "role"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"apply_call_center_policy\","
      "\"room_id\":\"room-http-call-center\","
      "\"supervisor_mode\":\"barge\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_NULL(json_string_value(root, "warning_code"));
  policy = json_object_field(root, "call_center_policy");
  TEST_ASSERT_NOT_NULL(policy);
  TEST_ASSERT_EQUAL_STRING("barge", json_string_value(policy, "supervisor_mode"));
  apply_result = json_object_field(root, "policy_apply_result");
  TEST_ASSERT_NOT_NULL(apply_result);
  TEST_ASSERT_EQUAL_INT(6, json_int_value(apply_result, "subscriptions_applied", -1));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-http-call-center\","
      "\"subscriber_participant_id\":\"customer\","
      "\"track_id\":\"supervisor-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_TRUE(json_bool_value(subscription, "enabled", 0));
  TEST_ASSERT_EQUAL_STRING("call_center_barge",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription_diagnostic\","
      "\"room_id\":\"room-http-call-center\","
      "\"subscriber_participant_id\":\"customer\","
      "\"track_id\":\"supervisor-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription_diag = json_object_field(root, "subscription_diagnostic");
  TEST_ASSERT_NOT_NULL(subscription_diag);
  TEST_ASSERT_TRUE(json_bool_value(subscription_diag, "in_sync", 0));
  sfu_track_subscription = json_object_field(subscription_diag, "sfu_track_subscription");
  TEST_ASSERT_NOT_NULL(sfu_track_subscription);
  TEST_ASSERT_EQUAL_STRING("call_center_barge",
                           json_string_value(sfu_track_subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"apply_call_center_policy\","
      "\"room_id\":\"room-http-call-center\","
      "\"supervisor_mode\":\"none\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_NULL(json_string_value(root, "warning_code"));
  policy = json_object_field(root, "call_center_policy");
  TEST_ASSERT_NOT_NULL(policy);
  TEST_ASSERT_EQUAL_STRING("none", json_string_value(policy, "supervisor_mode"));
  apply_result = json_object_field(root, "policy_apply_result");
  TEST_ASSERT_NOT_NULL(apply_result);
  TEST_ASSERT_EQUAL_INT(4, json_int_value(apply_result, "subscriptions_removed", -1));
  turbo_free_json(&root);

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_track_subscription\","
      "\"room_id\":\"room-http-call-center\","
      "\"receiver_participant_id\":\"customer\","
      "\"track_id\":\"supervisor-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  sfu_track_subscription = json_object_field(root, "track_subscription");
  TEST_ASSERT_NOT_NULL(sfu_track_subscription);
  TEST_ASSERT_FALSE(json_bool_value(sfu_track_subscription, "enabled", 1));
  TEST_ASSERT_TRUE(json_bool_value(sfu_track_subscription, "muted", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/health", 30,
                                                100));

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(root, "queue_depth", -1));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(root, "queue_depth", -1));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(root, "queue_depth", -1));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"peek_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"side\":\"caller\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  queue_entry = json_object_field(root, "queue_entry");
  TEST_ASSERT_NOT_NULL(queue_entry);
  TEST_ASSERT_EQUAL_STRING("caller-vip", json_string_value(queue_entry, "entry_id"));
  TEST_ASSERT_EQUAL_STRING("customer-vip", json_string_value(queue_entry, "endpoint_id"));
  TEST_ASSERT_EQUAL_INT(80, json_int_value(queue_entry, "priority", -1));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"match_call_center_queue\","
      "\"queue_id\":\"support\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  match = json_object_field(root, "call_center_match");
  TEST_ASSERT_NOT_NULL(match);
  TEST_ASSERT_TRUE(json_bool_value(match, "matched", 0));
  caller = json_object_field(match, "caller");
  callee = json_object_field(match, "callee");
  TEST_ASSERT_NOT_NULL(caller);
  TEST_ASSERT_NOT_NULL(callee);
  TEST_ASSERT_EQUAL_STRING("caller-vip", json_string_value(caller, "entry_id"));
  TEST_ASSERT_EQUAL_STRING("agent-primary", json_string_value(callee, "entry_id"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(root, "caller_depth", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(root, "callee_depth", -1));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  route = json_object_field(root, "call_center_route");
  TEST_ASSERT_NOT_NULL(route);
  TEST_ASSERT_EQUAL_STRING("room-routed-call", json_string_value(route, "room_id"));
  match = json_object_field(route, "match");
  TEST_ASSERT_NOT_NULL(match);
  TEST_ASSERT_TRUE(json_bool_value(match, "matched", 0));
  caller = json_object_field(match, "caller");
  callee = json_object_field(match, "callee");
  TEST_ASSERT_NOT_NULL(caller);
  TEST_ASSERT_NOT_NULL(callee);
  TEST_ASSERT_EQUAL_STRING("caller-regular", json_string_value(caller, "entry_id"));
  TEST_ASSERT_EQUAL_STRING("agent-secondary", json_string_value(callee, "entry_id"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_participant\","
      "\"room_id\":\"room-routed-call\","
      "\"participant_id\":\"customer-regular\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  participant = json_object_field(root, "participant");
  TEST_ASSERT_NOT_NULL(participant);
  TEST_ASSERT_EQUAL_STRING("customer", json_string_value(participant, "role"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_participant\","
      "\"room_id\":\"room-routed-call\","
      "\"participant_id\":\"agent-b\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  participant = json_object_field(root, "participant");
  TEST_ASSERT_NOT_NULL(participant);
  TEST_ASSERT_EQUAL_STRING("agent", json_string_value(participant, "role"));
  turbo_free_json(&root);

  root = http_get_json(room_service_base_url, "/api/v1/rooms/room-routed-call");
  TEST_ASSERT_NOT_NULL(root);
  room = root;
  TEST_ASSERT_EQUAL_STRING("active", json_string_value(room, "recording_state"));
  TEST_ASSERT_EQUAL_STRING("rec-routed-call", json_string_value(room, "recording_id"));
  TEST_ASSERT_EQUAL_STRING("compliance", json_string_value(room, "recording_mode"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_room\","
      "\"room_id\":\"room-routed-call\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  call_center_room = json_object_field(root, "call_center_room");
  TEST_ASSERT_NOT_NULL(call_center_room);
  TEST_ASSERT_EQUAL_STRING("active", json_string_value(call_center_room, "state"));
  TEST_ASSERT_EQUAL_STRING("customer-regular",
                           json_string_value(call_center_room,
                                             "customer_participant_id"));
  TEST_ASSERT_EQUAL_STRING("agent-b",
                           json_string_value(call_center_room,
                                             "agent_participant_id"));
  TEST_ASSERT_EQUAL_STRING("",
                           json_string_value(call_center_room,
                                             "consult_agent_participant_id"));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"apply_call_center_policy\","
      "\"room_id\":\"room-routed-call\","
      "\"supervisor_mode\":\"monitor\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"supervisor-1\","
      "\"track_id\":\"track-customer-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_TRUE(json_bool_value(subscription, "enabled", 0));
  TEST_ASSERT_EQUAL_STRING("call_center_monitor",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"supervisor-1\","
      "\"track_id\":\"track-agent-b-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_TRUE(json_bool_value(subscription, "enabled", 0));
  TEST_ASSERT_EQUAL_STRING("call_center_monitor",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"qa-1\","
      "\"track_id\":\"track-customer-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_TRUE(json_bool_value(subscription, "enabled", 0));
  TEST_ASSERT_EQUAL_STRING("call_center_observe",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"bot-1\","
      "\"track_id\":\"track-agent-b-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_TRUE(json_bool_value(subscription, "enabled", 0));
  TEST_ASSERT_EQUAL_STRING("call_center_observe",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_call_center_consult_agent\","
      "\"room_id\":\"room-routed-call\","
      "\"consult_agent_participant_id\":\"agent-c\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  call_center_room = json_object_field(root, "call_center_room");
  TEST_ASSERT_NOT_NULL(call_center_room);
  TEST_ASSERT_EQUAL_STRING("agent-c",
                           json_string_value(call_center_room,
                                             "consult_agent_participant_id"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-c\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  TEST_ASSERT_NOT_NULL(agent_state);
  TEST_ASSERT_EQUAL_STRING("busy", json_string_value(agent_state, "state"));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"agent-b\","
      "\"track_id\":\"track-agent-c-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_EQUAL_STRING("call_center_consult",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"agent-c\","
      "\"track_id\":\"track-customer-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_EQUAL_STRING("call_center_consult",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"qa-1\","
      "\"track_id\":\"track-agent-c-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_EQUAL_STRING("call_center_observe",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"bot-1\","
      "\"track_id\":\"track-agent-c-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_EQUAL_STRING("call_center_observe",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"remove_participant\","
      "\"room_id\":\"room-routed-call\","
      "\"participant_id\":\"agent-c\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_room\","
      "\"room_id\":\"room-routed-call\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  call_center_room = json_object_field(root, "call_center_room");
  TEST_ASSERT_NOT_NULL(call_center_room);
  TEST_ASSERT_EQUAL_STRING("",
                           json_string_value(call_center_room,
                                             "consult_agent_participant_id"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-c\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  TEST_ASSERT_NOT_NULL(agent_state);
  TEST_ASSERT_EQUAL_STRING("available", json_string_value(agent_state, "state"));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_call_center_consult_agent\","
      "\"room_id\":\"room-routed-call\","
      "\"consult_agent_participant_id\":\"agent-c\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  call_center_room = json_object_field(root, "call_center_room");
  TEST_ASSERT_NOT_NULL(call_center_room);
  TEST_ASSERT_EQUAL_STRING("agent-c",
                           json_string_value(call_center_room,
                                             "consult_agent_participant_id"));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"agent-b\","
      "\"track_id\":\"track-agent-c-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_EQUAL_STRING("call_center_consult",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = NULL;
  TEST_ASSERT_EQUAL_INT(
      404, http_post_json_status(
               room_service_base_url, "/api/v1/commands",
               "{"
               "\"type\":\"get_subscription\","
               "\"room_id\":\"room-routed-call\","
               "\"subscriber_participant_id\":\"customer-regular\","
               "\"track_id\":\"track-agent-c-audio\""
                "}",
               &root));
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_EQUAL_STRING("SUBSCRIPTION_NOT_FOUND", json_string_value(root, "code"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"complete_call_center_transfer\","
      "\"room_id\":\"room-routed-call\","
      "\"released_agent_state\":\"wrap_up\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  call_center_room = json_object_field(root, "call_center_room");
  TEST_ASSERT_NOT_NULL(call_center_room);
  TEST_ASSERT_EQUAL_STRING("agent-c",
                           json_string_value(call_center_room,
                                             "agent_participant_id"));
  TEST_ASSERT_EQUAL_STRING("",
                           json_string_value(call_center_room,
                                             "consult_agent_participant_id"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-b\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  TEST_ASSERT_NOT_NULL(agent_state);
  TEST_ASSERT_EQUAL_STRING("wrap_up", json_string_value(agent_state, "state"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_subscription\","
      "\"room_id\":\"room-routed-call\","
      "\"subscriber_participant_id\":\"customer-regular\","
      "\"track_id\":\"track-agent-c-audio\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  subscription = json_object_field(root, "subscription");
  TEST_ASSERT_NOT_NULL(subscription);
  TEST_ASSERT_EQUAL_STRING("call_center_call",
                           json_string_value(subscription, "policy_source"));
  turbo_free_json(&root);

  root = NULL;
  TEST_ASSERT_EQUAL_INT(
      404, http_post_json_status(
               room_service_base_url, "/api/v1/commands",
               "{"
               "\"type\":\"get_subscription\","
               "\"room_id\":\"room-routed-call\","
               "\"subscriber_participant_id\":\"customer-regular\","
               "\"track_id\":\"track-agent-b-audio\""
                "}",
               &root));
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_EQUAL_STRING("SUBSCRIPTION_NOT_FOUND", json_string_value(root, "code"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"finalize_call_center_room\","
      "\"room_id\":\"room-routed-call\","
      "\"state\":\"wrap_up\","
      "\"disposition_code\":\"resolved\","
      "\"agent_state\":\"wrap_up\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  call_center_room = json_object_field(root, "call_center_room");
  TEST_ASSERT_NOT_NULL(call_center_room);
  TEST_ASSERT_EQUAL_STRING("wrap_up", json_string_value(call_center_room, "state"));
  TEST_ASSERT_EQUAL_STRING("resolved",
                           json_string_value(call_center_room, "disposition_code"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-c\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  TEST_ASSERT_NOT_NULL(agent_state);
  TEST_ASSERT_EQUAL_STRING("wrap_up", json_string_value(agent_state, "state"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"finalize_call_center_room\","
      "\"room_id\":\"room-routed-call\","
      "\"state\":\"completed\","
      "\"disposition_code\":\"resolved\","
      "\"agent_state\":\"available\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  call_center_room = json_object_field(root, "call_center_room");
  TEST_ASSERT_NOT_NULL(call_center_room);
  TEST_ASSERT_EQUAL_STRING("completed", json_string_value(call_center_room, "state"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-c\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  TEST_ASSERT_NOT_NULL(agent_state);
  TEST_ASSERT_EQUAL_STRING("available", json_string_value(agent_state, "state"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_events\","
      "\"room_id\":\"room-routed-call\","
      "\"after_sequence\":0,"
      "\"limit\":16"
      "}");
  TEST_ASSERT_NOT_NULL(root);
  call_center_events = json_object_field(root, "call_center_events");
  TEST_ASSERT_NOT_NULL(call_center_events);
  TEST_ASSERT_EQUAL_STRING("room-routed-call",
                           json_string_value(call_center_events, "room_id"));
  TEST_ASSERT_EQUAL_size_t(10, json_array_count(call_center_events, "events"));
  latest_event_sequence = json_int_value(call_center_events, "latest_sequence", 0);
  TEST_ASSERT_TRUE(latest_event_sequence > 0);

  event = find_call_center_event(call_center_events, "room_routed", "customer-regular",
                                 "active");
  TEST_ASSERT_NOT_NULL(event);
  TEST_ASSERT_EQUAL_STRING("agent-b", json_string_value(event, "peer_participant_id"));
  TEST_ASSERT_EQUAL_STRING("support", json_string_value(event, "detail"));

  event = find_call_center_event(call_center_events, "policy_applied", NULL, "monitor");
  TEST_ASSERT_NOT_NULL(event);

  event = find_call_center_event(call_center_events, "observer_joined", "supervisor-1",
                                 "supervisor");
  TEST_ASSERT_NOT_NULL(event);
  event = find_call_center_event(call_center_events, "observer_joined", "qa-1",
                                 "qa_observer");
  TEST_ASSERT_NOT_NULL(event);
  event = find_call_center_event(call_center_events, "observer_joined", "bot-1", "bot");
  TEST_ASSERT_NOT_NULL(event);
  event = find_call_center_event(call_center_events, "consult_started", "agent-c",
                                 "active");
  TEST_ASSERT_NOT_NULL(event);
  event = find_call_center_event(call_center_events, "transfer_completed", "agent-c",
                                 "active");
  TEST_ASSERT_NOT_NULL(event);
  TEST_ASSERT_EQUAL_STRING("wrap_up", json_string_value(event, "detail"));
  event = find_call_center_event(call_center_events, "room_finalized", NULL,
                                 "wrap_up");
  TEST_ASSERT_NOT_NULL(event);
  TEST_ASSERT_EQUAL_STRING("resolved", json_string_value(event, "detail"));
  event = find_call_center_event(call_center_events, "room_finalized", NULL,
                                 "completed");
  TEST_ASSERT_NOT_NULL(event);
  TEST_ASSERT_EQUAL_STRING("resolved", json_string_value(event, "detail"));
  turbo_free_json(&root);

  snprintf(event_path, sizeof(event_path),
           "/api/v1/rooms/room-routed-call/call_center_events?after_sequence=%d&limit=4",
           latest_event_sequence - 1);
  root = http_get_json(room_service_base_url, event_path);
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_EQUAL_STRING("room-routed-call", json_string_value(root, "room_id"));
  TEST_ASSERT_EQUAL_size_t(1, json_array_count(root, "events"));
  event = json_array_object_at(json_array_field(root, "events"), 0);
  TEST_ASSERT_NOT_NULL(event);
  TEST_ASSERT_EQUAL_STRING("room_finalized", json_string_value(event, "event_type"));
  TEST_ASSERT_EQUAL_STRING("completed", json_string_value(event, "state"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(root, "caller_depth", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(root, "callee_depth", -1));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"route_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"route_room_id\":\"room-routed-retry\","
      "\"test_failure_stage\":\"after_callee_join\""
      "}");
  TEST_ASSERT_NULL(root);

  root = http_get_json(room_service_base_url, "/api/v1/rooms/room-routed-retry");
  TEST_ASSERT_NULL(root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(root, "caller_depth", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(root, "callee_depth", -1));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"route_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"route_room_id\":\"room-routed-retry\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  route = json_object_field(root, "call_center_route");
  TEST_ASSERT_NOT_NULL(route);
  TEST_ASSERT_EQUAL_STRING("room-routed-retry", json_string_value(route, "room_id"));
  match = json_object_field(route, "match");
  TEST_ASSERT_NOT_NULL(match);
  TEST_ASSERT_TRUE(json_bool_value(match, "matched", 0));
  caller = json_object_field(match, "caller");
  callee = json_object_field(match, "callee");
  TEST_ASSERT_NOT_NULL(caller);
  TEST_ASSERT_NOT_NULL(callee);
  TEST_ASSERT_EQUAL_STRING("caller-retry", json_string_value(caller, "entry_id"));
  TEST_ASSERT_EQUAL_STRING("agent-retry", json_string_value(callee, "entry_id"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"set_call_center_agent_state\","
      "\"endpoint_id\":\"agent-a\","
      "\"state\":\"offline\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  TEST_ASSERT_NOT_NULL(agent_state);
  TEST_ASSERT_EQUAL_STRING("offline", json_string_value(agent_state, "state"));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"route_call_center_queue\","
      "\"queue_id\":\"availability\","
      "\"route_room_id\":\"room-agent-availability\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  route = json_object_field(root, "call_center_route");
  TEST_ASSERT_NOT_NULL(route);
  match = json_object_field(route, "match");
  TEST_ASSERT_NOT_NULL(match);
  callee = json_object_field(match, "callee");
  TEST_ASSERT_NOT_NULL(callee);
  TEST_ASSERT_EQUAL_STRING("agent-ready", json_string_value(callee, "entry_id"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-a\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  TEST_ASSERT_NOT_NULL(agent_state);
  TEST_ASSERT_EQUAL_STRING("offline", json_string_value(agent_state, "state"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_agent_state\","
      "\"endpoint_id\":\"agent-c\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  agent_state = json_object_field(root, "call_center_agent_state");
  TEST_ASSERT_NOT_NULL(agent_state);
  TEST_ASSERT_EQUAL_STRING("busy", json_string_value(agent_state, "state"));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"route_call_center_queue\","
      "\"queue_id\":\"support\","
      "\"route_room_id\":\"room-invalid-route\""
      "}");
  TEST_ASSERT_NULL(root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(root, "caller_depth", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(root, "callee_depth", -1));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"claim_call_center_queue\","
      "\"queue_id\":\"support\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  match = json_object_field(root, "call_center_match");
  TEST_ASSERT_NOT_NULL(match);
  TEST_ASSERT_TRUE(json_bool_value(match, "matched", 0));
  caller = json_object_field(match, "caller");
  callee = json_object_field(match, "callee");
  TEST_ASSERT_NOT_NULL(caller);
  TEST_ASSERT_NOT_NULL(callee);
  TEST_ASSERT_EQUAL_STRING("caller-loop", json_string_value(caller, "entry_id"));
  TEST_ASSERT_EQUAL_STRING("agent-loop", json_string_value(callee, "entry_id"));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(root, "caller_depth", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(root, "callee_depth", -1));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"recover_call_center_queue_claims\","
      "\"queue_id\":\"support\","
      "\"lease_ms\":0"
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(root, "recovered", -1));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"claim_call_center_queue\","
      "\"queue_id\":\"support\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  match = json_object_field(root, "call_center_match");
  TEST_ASSERT_NOT_NULL(match);
  TEST_ASSERT_TRUE(json_bool_value(match, "matched", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"rollback_call_center_queue_match\","
      "\"queue_id\":\"support\","
      "\"caller_entry_id\":\"caller-loop\","
      "\"callee_entry_id\":\"agent-loop\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"claim_call_center_queue\","
      "\"queue_id\":\"support\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  match = json_object_field(root, "call_center_match");
  TEST_ASSERT_NOT_NULL(match);
  TEST_ASSERT_TRUE(json_bool_value(match, "matched", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"complete_call_center_queue_match\","
      "\"queue_id\":\"support\","
      "\"caller_entry_id\":\"caller-loop\","
      "\"callee_entry_id\":\"agent-loop\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_call_center_queue_depth\","
      "\"queue_id\":\"support\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(root, "caller_depth", -1));
  TEST_ASSERT_EQUAL_INT(0, json_int_value(root, "callee_depth", -1));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(sfu_server);
  TEST_ASSERT_EQUAL_INT(0, sfu_node_app_server_start(sfu_server));
  TEST_ASSERT_EQUAL_INT(0, wait_for_http_status_ok(sfu_node_base_url, "/health", 30, 100));

  room_server = room_service_app_server_create(&room_config);
  TEST_ASSERT_NOT_NULL(room_server);
  TEST_ASSERT_EQUAL_INT(0, room_service_app_server_start(room_server));
  TEST_ASSERT_EQUAL_INT(0,
                        wait_for_http_status_ok(room_service_base_url, "/health", 30, 100));

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-recording\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-recording\","
      "\"node_id\":\"node-eu-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"start_recording\","
      "\"room_id\":\"room-recording\","
      "\"recording_id\":\"rec-001\","
      "\"mode\":\"archive\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_NULL(json_string_value(root, "warning_code"));
  turbo_free_json(&root);

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording\""
      "}");
  TEST_ASSERT_NOT_NULL(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  TEST_ASSERT_NOT_NULL(recording_status);
  TEST_ASSERT_TRUE(json_bool_value(recording_status, "active", 0));
  TEST_ASSERT_EQUAL_STRING("rec-001", json_string_value(recording_status, "recording_id"));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(recording_status, "track_count", -1));
  turbo_free_json(&recording_root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"stop_recording\","
      "\"room_id\":\"room-recording\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_NULL(json_string_value(root, "warning_code"));
  turbo_free_json(&root);

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording\""
      "}");
  TEST_ASSERT_NOT_NULL(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  TEST_ASSERT_NOT_NULL(recording_status);
  TEST_ASSERT_FALSE(json_bool_value(recording_status, "active", 1));
  output_path = json_string_value(recording_status, "output_path");
  TEST_ASSERT_NOT_NULL(output_path);
  remove(output_path);
  turbo_free_json(&recording_root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-recording-replay\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"start_recording\","
      "\"room_id\":\"room-recording-replay\","
      "\"recording_id\":\"rec-replay\","
      "\"mode\":\"archive\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-recording-replay\","
      "\"node_id\":\"node-eu-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  stats = json_object_field(root, "sfu_replay_stats");
  TEST_ASSERT_NOT_NULL(stats);
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "tracks_replayed", -1));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(stats, "recordings_replayed", -1));
  turbo_free_json(&root);

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording-replay\""
      "}");
  TEST_ASSERT_NOT_NULL(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  TEST_ASSERT_NOT_NULL(recording_status);
  TEST_ASSERT_TRUE(json_bool_value(recording_status, "active", 0));
  TEST_ASSERT_EQUAL_STRING("rec-replay", json_string_value(recording_status, "recording_id"));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(recording_status, "track_count", -1));
  turbo_free_json(&recording_root);

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"stop_recording\","
      "\"room_id\":\"room-recording-replay\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  recording_status = json_object_field(root, "recording_status");
  TEST_ASSERT_NOT_NULL(recording_status);
  output_path = json_string_value(recording_status, "output_path");
  TEST_ASSERT_NOT_NULL(output_path);
  remove(output_path);
  turbo_free_json(&root);

  root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"start_recording\","
      "\"room_id\":\"room-recording-replay\","
      "\"recording_id\":\"rec-stale\","
      "\"mode\":\"webm\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  recording_status = json_object_field(root, "recording_status");
  TEST_ASSERT_NOT_NULL(recording_status);
  TEST_ASSERT_TRUE(json_bool_value(recording_status, "active", 0));
  TEST_ASSERT_EQUAL_STRING("rec-stale", json_string_value(recording_status, "recording_id"));
  TEST_ASSERT_EQUAL_STRING("webm", json_string_value(recording_status, "mode"));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_NULL(json_string_value(root, "warning_code"));
  turbo_free_json(&root);
  remove("recording_room-recording-replay_rec-stale.webm");

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording-replay\""
      "}");
  TEST_ASSERT_NOT_NULL(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  TEST_ASSERT_NOT_NULL(recording_status);
  TEST_ASSERT_TRUE(json_bool_value(recording_status, "active", 0));
  TEST_ASSERT_EQUAL_STRING("rec-replay", json_string_value(recording_status, "recording_id"));
  TEST_ASSERT_EQUAL_STRING("archive", json_string_value(recording_status, "mode"));
  TEST_ASSERT_EQUAL_INT(2, json_int_value(recording_status, "track_count", -1));
  turbo_free_json(&recording_root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"stop_recording\","
      "\"room_id\":\"room-recording-replay\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording-replay\""
      "}");
  TEST_ASSERT_NOT_NULL(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  TEST_ASSERT_NOT_NULL(recording_status);
  output_path = json_string_value(recording_status, "output_path");
  TEST_ASSERT_NOT_NULL(output_path);
  remove(output_path);
  turbo_free_json(&recording_root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"create_room\","
      "\"room_id\":\"room-recording-late-track\","
      "\"room_type\":\"conference\","
      "\"created_by\":\"host-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"assign_sfu_node\","
      "\"room_id\":\"room-recording-late-track\","
      "\"node_id\":\"node-eu-1\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"start_recording\","
      "\"room_id\":\"room-recording-late-track\","
      "\"recording_id\":\"rec-late-track\","
      "\"mode\":\"archive\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_EQUAL_STRING("SFU_SYNC_FAILED", json_string_value(root, "warning_code"));
  turbo_free_json(&root);

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
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  TEST_ASSERT_NULL(json_string_value(root, "warning_code"));
  turbo_free_json(&root);

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording-late-track\""
      "}");
  TEST_ASSERT_NOT_NULL(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  TEST_ASSERT_NOT_NULL(recording_status);
  TEST_ASSERT_TRUE(json_bool_value(recording_status, "active", 0));
  TEST_ASSERT_EQUAL_STRING("rec-late-track",
                           json_string_value(recording_status, "recording_id"));
  TEST_ASSERT_EQUAL_INT(1, json_int_value(recording_status, "track_count", -1));
  turbo_free_json(&recording_root);

  root = http_post_json_result(
      room_service_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"stop_recording\","
      "\"room_id\":\"room-recording-late-track\""
      "}");
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_TRUE(json_bool_value(root, "ok", 0));
  turbo_free_json(&root);

  recording_root = http_post_json_result(
      sfu_node_base_url, "/api/v1/commands",
      "{"
      "\"type\":\"get_recording_status\","
      "\"room_id\":\"room-recording-late-track\""
      "}");
  TEST_ASSERT_NOT_NULL(recording_root);
  recording_status = json_object_field(recording_root, "recording_status");
  TEST_ASSERT_NOT_NULL(recording_status);
  output_path = json_string_value(recording_status, "output_path");
  TEST_ASSERT_NOT_NULL(output_path);
  remove(output_path);
  turbo_free_json(&recording_root);

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
  TEST_ASSERT_NOT_NULL(server);
  http_api = room_service_http_api_create(server);
  TEST_ASSERT_NOT_NULL(http_api);

  for (iteration = 0;
       iteration < ROOM_SERVICE_HTTP_LIFECYCLE_STRESS_ITERATIONS;
       ++iteration) {
    TEST_ASSERT_EQUAL_INT(
        0, room_service_http_api_start(http_api, config.bind_host,
                                       config.bind_port));
    TEST_ASSERT_EQUAL_INT(
        0, wait_for_http_status_ok(base_url, "/health", 3, 10));
    if (iteration + 1 < ROOM_SERVICE_HTTP_LIFECYCLE_STRESS_ITERATIONS) {
      room_service_http_api_stop(http_api);
    }
  }

  room_service_http_api_destroy(http_api);
  room_service_app_server_destroy(server);
}

spec("test_room_service_app") {
  TT_TEST(test_room_service_http_lifecycle_repeated_start_stop);
  TT_TEST(test_room_service_assign_replays_existing_state_and_closed_room_diag_stays_green);
  TT_TEST(test_room_sync_diagnostic_reports_null_when_room_has_no_sync_history);
  TT_TEST(test_room_sync_diagnostic_http_endpoints_expose_latest_sync_state);
  TT_TEST(test_room_sync_http_assign_and_resync_results_match_room_sync_diagnostic);
  TT_TEST(test_room_service_issues_scoped_sfu_command_tokens);
  TT_TEST(test_room_service_config_reads_control_tokens_from_env);
  TT_TEST(test_room_service_http_control_token_protects_modifying_commands);
  TT_TEST(test_room_service_facade_join_publish_and_subscribe);
  TT_TEST(test_room_service_routes_rooms_to_registered_sfu_nodes);
  TT_TEST(test_room_sync_http_warning_paths_surface_skipped_replay_state);
  TT_TEST(test_room_sync_http_failed_replay_is_reflected_in_room_sync_diagnostic);
  TT_TEST(test_room_service_resync_room_repairs_sfu_drift);
  TT_TEST(test_room_service_conference_policy_materializes_and_syncs_screen_pin_and_active_speaker);
  TT_TEST(test_room_service_call_center_policy_materializes_and_syncs_supervisor_modes);
  TT_TEST(test_room_service_http_call_center_policy_command_syncs_to_sfu_node);
  TT_TEST(test_room_service_http_call_center_priority_queue_commands);
  TT_TEST(test_room_service_recording_commands_sync_to_sfu_node);
}
