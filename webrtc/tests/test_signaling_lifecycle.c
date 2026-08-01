#include "http_api.h"
#include "http_client.h"
#include "turbo_media_auth.h"
#include "tinytest_compat.h"
#include "webrtc_signaling.h"
#include <stdlib.h>
#include <time.h>

#define SIGNALING_HTTP_LIFECYCLE_STRESS_ITERATIONS 32

#ifndef TURBO_MEDIA_TEST_TLS_CERT_PATH
#error "TURBO_MEDIA_TEST_TLS_CERT_PATH must identify the test certificate"
#endif

#ifndef TURBO_MEDIA_TEST_TLS_KEY_PATH
#error "TURBO_MEDIA_TEST_TLS_KEY_PATH must identify the test private key"
#endif

static int get_status(const char *base_url, const char *path,
                      const char *bearer_token, const char *ca_file) {
  http_client_t *client = http_client_create(base_url);
  http_response_t *response;
  int status = 0;

  if (!client) {
    return 0;
  }
  if (ca_file) {
    turbo_tls_client_config_t tls_config = {
        .ca_file = ca_file,
        .verify_peer = 1
    };
    if (http_client_set_tls_client_config(client, &tls_config) != 0) {
      http_client_destroy(client);
      return 0;
    }
  }
  http_client_set_timeout(client, 3000);
  if (bearer_token) {
    http_client_set_bearer_token(client, bearer_token);
  }
  response = http_get(client, path);
  if (response) {
    status = response->status_code;
    http_response_free(response);
  }
  http_client_destroy(client);
  return status;
}

static int delete_status(const char *base_url, const char *path,
                         const char *bearer_token) {
  http_client_t *client = http_client_create(base_url);
  http_response_t *response;
  int status = 0;

  if (!client) {
    return 0;
  }
  http_client_set_timeout(client, 3000);
  if (bearer_token) {
    http_client_set_bearer_token(client, bearer_token);
  }
  response = http_del(client, path);
  if (response) {
    status = response->status_code;
    http_response_free(response);
  }
  http_client_destroy(client);
  return status;
}

static char *issue_signaling_management_token(
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
      .subject = "signaling-management-test",
      .audience = "turbomedia-signaling-management",
      .scope = scope,
      .room_id = room_id,
      .participant_id = participant_id,
      .issued_at = issued_at,
      .expires_at = expires_at,
  };

  return turbo_media_auth_issue(&config, &claims);
}

void test_signaling_and_http_api_instances_have_independent_lifecycles(void) {
  webrtc_signaling_config_t signaling_config = {0};
  http_api_config_t http_config_a = {0};
  http_api_config_t http_config_b = {0};
  webrtc_signaling_server_t *signaling_a = NULL;
  webrtc_signaling_server_t *signaling_b = NULL;
  http_api_server_t *http_a = NULL;
  http_api_server_t *http_b = NULL;
  int iteration;

  signaling_config.host = "127.0.0.1";
  signaling_a = webrtc_signaling_create(NULL, &signaling_config);
  signaling_b = webrtc_signaling_create(NULL, &signaling_config);
  TEST_ASSERT_NOT_NULL(signaling_a);
  TEST_ASSERT_NOT_NULL(signaling_b);

  http_config_a.host = "0.0.0.0";
  http_config_a.port = 18081;
  http_config_b.host = "0.0.0.0";
  http_config_b.port = 18082;
  http_a = http_api_create(NULL, &http_config_a, signaling_a);
  http_b = http_api_create(NULL, &http_config_b, signaling_b);
  TEST_ASSERT_NOT_NULL(http_a);
  TEST_ASSERT_NOT_NULL(http_b);

  for (iteration = 0;
       iteration < SIGNALING_HTTP_LIFECYCLE_STRESS_ITERATIONS;
       ++iteration) {
    TEST_ASSERT_EQUAL_INT(0, http_api_start(http_a));
    TEST_ASSERT_EQUAL_INT(0, http_api_start(http_b));
    http_api_stop(http_b);
    http_api_stop(http_a);
  }

  http_api_destroy(http_a);
  http_api_destroy(http_b);
  webrtc_signaling_destroy(signaling_a);
  webrtc_signaling_destroy(signaling_b);
}

void test_http_api_rejects_invalid_configuration(void) {
  webrtc_signaling_config_t signaling_config = {0};
  http_api_config_t http_config = {0};
  webrtc_signaling_server_t *signaling = NULL;

  signaling = webrtc_signaling_create(NULL, &signaling_config);
  TEST_ASSERT_NOT_NULL(signaling);

  TEST_ASSERT_NULL(http_api_create(NULL, NULL, signaling));
  TEST_ASSERT_NULL(http_api_create(NULL, &http_config, signaling));
  http_config.port = 8080;
  TEST_ASSERT_NULL(http_api_create(NULL, &http_config, NULL));
  http_config.host = "";
  TEST_ASSERT_NULL(http_api_create(NULL, &http_config, signaling));
  http_config.host = "127.0.0.1";
  http_config.auth_enabled = 1;
  TEST_ASSERT_NULL(http_api_create(NULL, &http_config, signaling));
  http_config.auth_active_key_id = "weak-key";
  http_config.auth_active_secret = "too-short";
  http_config.auth_issuer = "turbomedia";
  http_config.auth_max_ttl_seconds = 3600;
  TEST_ASSERT_NULL(http_api_create(NULL, &http_config, signaling));
  http_config.auth_active_secret =
      "management-active-secret-at-least-32-bytes";
  http_config.auth_revoked_token_sha256 = "invalid";
  TEST_ASSERT_NULL(http_api_create(NULL, &http_config, signaling));
  http_config.auth_revoked_token_sha256 =
      "0000000000000000000000000000000000000000000000000000000000000000";
  {
    http_api_server_t *authenticated =
        http_api_create(NULL, &http_config, signaling);
    TEST_ASSERT_NOT_NULL(authenticated);
    http_api_destroy(authenticated);
  }
  http_config.auth_active_key_id = NULL;
  http_config.auth_active_secret = NULL;
  http_config.auth_revoked_token_sha256 = NULL;
  http_config.admin_token = "test-admin-token";
  {
    http_api_server_t *authenticated =
        http_api_create(NULL, &http_config, signaling);
    TEST_ASSERT_NOT_NULL(authenticated);
    http_api_destroy(authenticated);
  }

  webrtc_signaling_destroy(signaling);
}

void test_signaling_native_websocket_listener_stops_and_restarts(void) {
  webrtc_signaling_config_t config = {0};
  webrtc_signaling_server_t *server = NULL;

  config.host = "127.0.0.1";
  config.port = 0;
  config.peer_timeout_ms = 1000;

  server = webrtc_signaling_create(NULL, &config);
  TEST_ASSERT_NOT_NULL(server);
  TEST_ASSERT_EQUAL_INT(0, webrtc_signaling_start(server));
  webrtc_signaling_stop(server);
  TEST_ASSERT_EQUAL_INT(0, webrtc_signaling_start(server));
  webrtc_signaling_stop(server);
  webrtc_signaling_destroy(server);
}

void test_signaling_peer_auth_configuration_fails_fast(void) {
  webrtc_signaling_config_t config = {0};
  webrtc_signaling_server_t *server = NULL;

  config.host = "127.0.0.1";
  config.port = 0;
  config.jwt_enabled = 1;
  config.jwt_issuer = "turbomedia";
  config.jwt_active_key_id = "signaling-peer-2026-07";
  config.jwt_secret = "too-short";
  config.jwt_algo = "HS256";
  TEST_ASSERT_NULL(webrtc_signaling_create(NULL, &config));

  config.jwt_secret =
      "signaling-peer-active-secret-at-least-32-bytes";
  config.jwt_previous_key_id = "signaling-peer-2026-06";
  TEST_ASSERT_NULL(webrtc_signaling_create(NULL, &config));

  config.jwt_previous_secret =
      "signaling-peer-previous-secret-at-least-32-bytes";
  config.jwt_revoked_token_sha256 = "invalid";
  TEST_ASSERT_NULL(webrtc_signaling_create(NULL, &config));

  config.jwt_revoked_token_sha256 =
      "0000000000000000000000000000000000000000000000000000000000000000";
  server = webrtc_signaling_create(NULL, &config);
  TEST_ASSERT_NOT_NULL(server);
  TEST_ASSERT_EQUAL_INT(0, webrtc_signaling_start(server));
  webrtc_signaling_stop(server);
  webrtc_signaling_destroy(server);
}

void test_signaling_resource_policy_configuration_fails_fast(void) {
  webrtc_signaling_config_t config = {0};
  webrtc_signaling_server_t *server = NULL;

  config.messages_per_second = 100;
  TEST_ASSERT_NULL(webrtc_signaling_create(NULL, &config));

  config.message_burst = 200;
  config.join_timeout_ms = -1;
  TEST_ASSERT_NULL(webrtc_signaling_create(NULL, &config));

  config.join_timeout_ms = 10000;
  config.max_connections_per_source = 1;
  TEST_ASSERT_NULL(webrtc_signaling_create(NULL, &config));

  config.max_source_states = 16;
  config.source_state_ttl_ms = 1000;
  config.source_admissions_per_second = 10;
  TEST_ASSERT_NULL(webrtc_signaling_create(NULL, &config));

  config.source_admission_burst = 20;
  server = webrtc_signaling_create(NULL, &config);
  TEST_ASSERT_NOT_NULL(server);
  webrtc_signaling_destroy(server);
}

void test_signaling_wss_listener_loads_explicit_identity(void) {
  webrtc_signaling_config_t config = {0};
  webrtc_signaling_server_t *server = NULL;

  config.host = "127.0.0.1";
  config.port = 0;
  config.use_tls = 1;
  config.cert_file = TURBO_MEDIA_TEST_TLS_CERT_PATH;
  config.key_file = TURBO_MEDIA_TEST_TLS_KEY_PATH;
  config.peer_timeout_ms = 1000;

  server = webrtc_signaling_create(NULL, &config);
  TEST_ASSERT_NOT_NULL(server);
  TEST_ASSERT_EQUAL_INT(0, webrtc_signaling_start(server));
  webrtc_signaling_stop(server);
  TEST_ASSERT_EQUAL_INT(0, webrtc_signaling_start(server));
  webrtc_signaling_stop(server);
  webrtc_signaling_destroy(server);
}

void test_http_api_bearer_auth_protects_management_routes(void) {
  webrtc_signaling_config_t signaling_config = {0};
  http_api_config_t http_config = {
      .host = "0.0.0.0",
      .port = 18083,
      .auth_enabled = 1,
      .admin_token = "test-admin-token",
      .auth_issuer = "turbomedia",
      .auth_active_key_id = "signaling-management-2026-07",
      .auth_active_secret =
          "signaling-management-active-secret-at-least-32-bytes",
      .auth_previous_key_id = "signaling-management-2026-06",
      .auth_previous_secret =
          "signaling-management-previous-secret-at-least-32-bytes",
      .auth_clock_skew_seconds = 0,
      .auth_max_ttl_seconds = 3600
  };
  webrtc_signaling_server_t *signaling =
      webrtc_signaling_create(NULL, &signaling_config);
  http_api_server_t *http;
  int64_t now = (int64_t)time(NULL);
  char *read_token = issue_signaling_management_token(
      "signaling-management-2026-07",
      "signaling-management-active-secret-at-least-32-bytes",
      "signaling.management.read", NULL, NULL, now, now + 60);
  char *previous_read_token = issue_signaling_management_token(
      "signaling-management-2026-06",
      "signaling-management-previous-secret-at-least-32-bytes",
      "signaling.management.read", NULL, NULL, now, now + 60);
  char *expired_token = issue_signaling_management_token(
      "signaling-management-2026-07",
      "signaling-management-active-secret-at-least-32-bytes",
      "signaling.management.read", NULL, NULL, now - 120, now - 60);
  char *room_read_token = issue_signaling_management_token(
      "signaling-management-2026-07",
      "signaling-management-active-secret-at-least-32-bytes",
      "signaling.management.read", "room-a", NULL, now, now + 60);
  char *write_token = issue_signaling_management_token(
      "signaling-management-2026-07",
      "signaling-management-active-secret-at-least-32-bytes",
      "signaling.management.write", "room-a", "peer-a", now, now + 60);
  char *dangerous_token = issue_signaling_management_token(
      "signaling-management-2026-07",
      "signaling-management-active-secret-at-least-32-bytes",
      "signaling.management.dangerous", "room-a", "peer-a",
      now, now + 60);

  TEST_ASSERT_NOT_NULL(signaling);
  TEST_ASSERT_NOT_NULL(read_token);
  TEST_ASSERT_NOT_NULL(previous_read_token);
  TEST_ASSERT_NOT_NULL(expired_token);
  TEST_ASSERT_NOT_NULL(room_read_token);
  TEST_ASSERT_NOT_NULL(write_token);
  TEST_ASSERT_NOT_NULL(dangerous_token);
  http = http_api_create(NULL, &http_config, signaling);
  TEST_ASSERT_NOT_NULL(http);
  TEST_ASSERT_EQUAL_INT(0, http_api_start(http));

  TEST_ASSERT_EQUAL_INT(200, get_status("http://127.0.0.1:18083",
                                        "/health", NULL, NULL));
  TEST_ASSERT_EQUAL_INT(401, get_status("http://127.0.0.1:18083",
                                        "/api/v1/status", NULL, NULL));
  TEST_ASSERT_EQUAL_INT(401, get_status("http://127.0.0.1:18083",
                                        "/api/v1/status", "wrong-token", NULL));
  TEST_ASSERT_EQUAL_INT(
      200, get_status("http://127.0.0.1:18083", "/api/v1/status",
                      "test-admin-token", NULL));
  TEST_ASSERT_EQUAL_INT(
      200, get_status("http://127.0.0.1:18083", "/api/v1/status",
                      read_token, NULL));
  TEST_ASSERT_EQUAL_INT(
      200, get_status("http://127.0.0.1:18083", "/api/v1/status",
                      previous_read_token, NULL));
  TEST_ASSERT_EQUAL_INT(
      401, get_status("http://127.0.0.1:18083", "/api/v1/status",
                      expired_token, NULL));
  TEST_ASSERT_EQUAL_INT(
      404, get_status("http://127.0.0.1:18083",
                      "/api/v1/rooms/room-a/peers", room_read_token, NULL));
  TEST_ASSERT_EQUAL_INT(
      401, get_status("http://127.0.0.1:18083",
                      "/api/v1/rooms/room-b/peers", room_read_token, NULL));
  TEST_ASSERT_EQUAL_INT(
      401, delete_status("http://127.0.0.1:18083",
                         "/api/v1/rooms/room-a/peers/peer-a", write_token));
  TEST_ASSERT_EQUAL_INT(
      404, delete_status("http://127.0.0.1:18083",
                         "/api/v1/rooms/room-a/peers/peer-a",
                         dangerous_token));

  http_api_destroy(http);
  webrtc_signaling_destroy(signaling);
  free(dangerous_token);
  free(write_token);
  free(room_read_token);
  free(expired_token);
  free(previous_read_token);
  free(read_token);
}

void test_https_management_api_requires_trusted_identity(void) {
  webrtc_signaling_config_t signaling_config = {0};
  http_api_config_t http_config = {
      .host = "127.0.0.1",
      .port = 18084,
      .use_tls = 1,
      .cert_file = TURBO_MEDIA_TEST_TLS_CERT_PATH,
      .key_file = TURBO_MEDIA_TEST_TLS_KEY_PATH
  };
  webrtc_signaling_server_t *signaling =
      webrtc_signaling_create(NULL, &signaling_config);
  http_api_server_t *http;

  TEST_ASSERT_NOT_NULL(signaling);
  http = http_api_create(NULL, &http_config, signaling);
  TEST_ASSERT_NOT_NULL(http);
  TEST_ASSERT_EQUAL_INT(0, http_api_start(http));

  TEST_ASSERT_EQUAL_INT(
      0, get_status("https://localhost:18084", "/health", NULL, NULL));
  TEST_ASSERT_EQUAL_INT(
      200, get_status("https://localhost:18084", "/health", NULL,
                      TURBO_MEDIA_TEST_TLS_CERT_PATH));

  http_api_destroy(http);
  webrtc_signaling_destroy(signaling);
}

spec("test_signaling_lifecycle") {
  TT_TEST(test_signaling_and_http_api_instances_have_independent_lifecycles);
  TT_TEST(test_http_api_rejects_invalid_configuration);
  TT_TEST(test_signaling_native_websocket_listener_stops_and_restarts);
  TT_TEST(test_signaling_peer_auth_configuration_fails_fast);
  TT_TEST(test_signaling_resource_policy_configuration_fails_fast);
  TT_TEST(test_signaling_wss_listener_loads_explicit_identity);
  TT_TEST(test_http_api_bearer_auth_protects_management_routes);
  TT_TEST(test_https_management_api_requires_trusted_identity);
}
