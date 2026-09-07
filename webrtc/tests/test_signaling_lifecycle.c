#include "http_api.h"
#include "turbo_transport.h"
#include "turbo_media_auth.h"
#include "tinytest.h"
#include "webrtc_signaling.h"
#include <salts/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIGNALING_HTTP_LIFECYCLE_STRESS_ITERATIONS 32

enum {
  SIGNALING_WS_TEST_QUEUE_CAPACITY = 16,
  SIGNALING_WS_TEST_MESSAGE_BYTES = 16 * 1024,
  SIGNALING_WS_TEST_WIRE_BYTES = SIGNALING_WS_TEST_MESSAGE_BYTES + 1024,
  SIGNALING_WS_TEST_TIMEOUT_MS = 3000
};

#ifndef TURBO_MEDIA_TEST_TLS_CERT_PATH
#error "TURBO_MEDIA_TEST_TLS_CERT_PATH must identify the test certificate"
#endif

#ifndef TURBO_MEDIA_TEST_TLS_KEY_PATH
#error "TURBO_MEDIA_TEST_TLS_KEY_PATH must identify the test private key"
#endif

static int get_status(const char *base_url, const char *path,
                      const char *bearer_token, const char *ca_file) {
  turbo_transport_config_t config = {0};
  cnet_tls_client_config tls = {0};
  turbo_transport_t *client;
  chttp_response *response;
  int status = 0;

  if (!base_url || !path ||
      turbo_transport_parse_url(base_url, &config) != 0 ||
      config.type != TURBO_TRANSPORT_HTTP ||
      (ca_file && !config.use_tls)) {
    return 0;
  }
  config.connect_timeout_ms = 3000;
  config.read_timeout_ms = 3000;
  config.write_timeout_ms = 3000;
  config.ca_cert_path = ca_file;
  if (config.use_tls) {
    tls.size = sizeof(tls);
    tls.ca_file = ca_file;
    tls.server_name = "localhost";
    config.tls = &tls;
    config.ca_cert_path = NULL;
  }
  config.auth_token = bearer_token;
  client = turbo_transport_create(&config);
  if (!client) return 0;
  response = turbo_transport_http_request(
      client, TURBO_HTTP_GET, path, NULL, 0u, NULL, 0);
  if (response) {
    status = response->status_code;
    chttp_response_destroy(response);
    free(response);
  } else {
    fprintf(stderr, "GET %s%s failed: %s\n", base_url, path,
            turbo_transport_get_error(client));
  }
  turbo_transport_destroy(client);
  return status;
}

static int delete_status(const char *base_url, const char *path,
                         const char *bearer_token) {
  turbo_transport_config_t config = {0};
  turbo_transport_t *client;
  chttp_response *response;
  int status = 0;

  if (!base_url || !path ||
      turbo_transport_parse_url(base_url, &config) != 0 ||
      config.type != TURBO_TRANSPORT_HTTP) {
    return 0;
  }
  config.connect_timeout_ms = 3000;
  config.read_timeout_ms = 3000;
  config.write_timeout_ms = 3000;
  config.auth_token = bearer_token;
  client = turbo_transport_create(&config);
  if (!client) return 0;
  response = turbo_transport_http_request(
      client, TURBO_HTTP_DELETE, path, NULL, 0u, NULL, 0);
  if (response) {
    status = response->status_code;
    chttp_response_destroy(response);
    free(response);
  }
  turbo_transport_destroy(client);
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
  webrtc_signaling_config_t signaling_config = {.connection_capacity = 4U};
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
  check_not_null(signaling_a);
  check_not_null(signaling_b);

  http_config_a.host = "0.0.0.0";
  http_config_a.port = 18081;
  http_config_b.host = "0.0.0.0";
  http_config_b.port = 18082;
  http_a = http_api_create(NULL, &http_config_a, signaling_a);
  http_b = http_api_create(NULL, &http_config_b, signaling_b);
  check_not_null(http_a);
  check_not_null(http_b);

  for (iteration = 0;
       iteration < SIGNALING_HTTP_LIFECYCLE_STRESS_ITERATIONS;
       ++iteration) {
    check_equal((int)(http_api_start(http_a)), (int)(0));
    check_equal((int)(http_api_start(http_b)), (int)(0));
    http_api_stop(http_b);
    http_api_stop(http_a);
  }

  http_api_destroy(http_a);
  http_api_destroy(http_b);
  webrtc_signaling_destroy(signaling_a);
  webrtc_signaling_destroy(signaling_b);
}

void test_http_api_rejects_invalid_configuration(void) {
  webrtc_signaling_config_t signaling_config = {.connection_capacity = 4U};
  http_api_config_t http_config = {0};
  webrtc_signaling_server_t *signaling = NULL;

  signaling = webrtc_signaling_create(NULL, &signaling_config);
  check_not_null(signaling);

  check_null(http_api_create(NULL, NULL, signaling));
  check_null(http_api_create(NULL, &http_config, signaling));
  http_config.port = 8080;
  check_null(http_api_create(NULL, &http_config, NULL));
  http_config.host = "";
  check_null(http_api_create(NULL, &http_config, signaling));
  http_config.host = "127.0.0.1";
  http_config.auth_enabled = 1;
  check_null(http_api_create(NULL, &http_config, signaling));
  http_config.auth_active_key_id = "weak-key";
  http_config.auth_active_secret = "too-short";
  http_config.auth_issuer = "turbomedia";
  http_config.auth_max_ttl_seconds = 3600;
  check_null(http_api_create(NULL, &http_config, signaling));
  http_config.auth_active_secret =
      "management-active-secret-at-least-32-bytes";
  http_config.auth_revoked_token_sha256 = "invalid";
  check_null(http_api_create(NULL, &http_config, signaling));
  http_config.auth_revoked_token_sha256 =
      "0000000000000000000000000000000000000000000000000000000000000000";
  {
    http_api_server_t *authenticated =
        http_api_create(NULL, &http_config, signaling);
    check_not_null(authenticated);
    http_api_destroy(authenticated);
  }
  http_config.auth_active_key_id = NULL;
  http_config.auth_active_secret = NULL;
  http_config.auth_revoked_token_sha256 = NULL;
  http_config.admin_token = "test-admin-token";
  {
    http_api_server_t *authenticated =
        http_api_create(NULL, &http_config, signaling);
    check_not_null(authenticated);
    http_api_destroy(authenticated);
  }

  webrtc_signaling_destroy(signaling);
}

void test_signaling_native_websocket_listener_stops_and_restarts(void) {
  webrtc_signaling_config_t config = {.connection_capacity = 4U};
  webrtc_signaling_server_t *server = NULL;

  config.host = "127.0.0.1";
  config.port = 0;
  config.peer_timeout_ms = 1000;

  server = webrtc_signaling_create(NULL, &config);
  check_not_null(server);
  check_equal((int)(webrtc_signaling_start(server)), (int)(0));
  webrtc_signaling_stop(server);
  check_equal((int)(webrtc_signaling_start(server)), (int)(0));
  webrtc_signaling_stop(server);
  webrtc_signaling_destroy(server);
}

static native_io_backend_kind signaling_test_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_websocket_client_config signaling_test_client_config(void) {
  chttp_websocket_client_config config = {0};
  config.size = sizeof(config);
  config.network.backend = signaling_test_backend();
  config.network.connection_capacity = 1U;
  config.network.command_capacity = SIGNALING_WS_TEST_QUEUE_CAPACITY;
  config.network.request_capacity = SIGNALING_WS_TEST_QUEUE_CAPACITY;
  config.network.completion_batch_capacity = SIGNALING_WS_TEST_QUEUE_CAPACITY;
  config.network.event_capacity = SIGNALING_WS_TEST_QUEUE_CAPACITY;
  config.network.max_send_bytes = SIGNALING_WS_TEST_WIRE_BYTES;
  config.network.receive_buffer_bytes = SIGNALING_WS_TEST_WIRE_BYTES;
  config.network.connect_timeout_ms = SIGNALING_WS_TEST_TIMEOUT_MS;
  config.network.read_timeout_ms = SIGNALING_WS_TEST_TIMEOUT_MS;
  config.network.write_timeout_ms = SIGNALING_WS_TEST_TIMEOUT_MS;
  config.network.command_buffer_bytes = SIGNALING_WS_TEST_WIRE_BYTES;
  config.network.event_buffer_bytes = SIGNALING_WS_TEST_WIRE_BYTES;
  config.max_frame_bytes = SIGNALING_WS_TEST_MESSAGE_BYTES;
  config.max_message_bytes = SIGNALING_WS_TEST_MESSAGE_BYTES;
  config.max_buffered_input_bytes = SIGNALING_WS_TEST_WIRE_BYTES;
  config.max_handshake_header_bytes = SIGNALING_WS_TEST_MESSAGE_BYTES;
  config.event_capacity = SIGNALING_WS_TEST_QUEUE_CAPACITY;
  return config;
}

void test_signaling_websocket_protocol_round_trip(void) {
  static const char join_message[] =
      "{\"type\":\"join\",\"room\":\"lifecycle-room\"}";
  webrtc_signaling_config_t config = {.connection_capacity = 4U};
  webrtc_signaling_server_t *server = NULL;
  chttp_websocket_client client = {0};
  chttp_websocket_client_config client_config =
      signaling_test_client_config();
  chttp_websocket_connect_options options = {0};
  chttp_websocket_event event = {0};
  unsigned int http_status = 0U;
  uint16_t port = 0U;
  char uri[128];
  int client_initialized = 0;
  int joined_seen = 0;
  int peers_seen = 0;
  int index;
  int status;

  config.host = "127.0.0.1";
  config.port = 0U;
  config.max_message_size = SIGNALING_WS_TEST_MESSAGE_BYTES;
  config.max_outbox_messages = SIGNALING_WS_TEST_QUEUE_CAPACITY;
  config.max_outbox_bytes = SIGNALING_WS_TEST_MESSAGE_BYTES;
  server = webrtc_signaling_create(NULL, &config);
  check_not_null(server);
  if (!server) {
    return;
  }
  status = webrtc_signaling_start(server);
  check_equal(status, 0);
  if (status != 0) {
    webrtc_signaling_destroy(server);
    return;
  }
  status = webrtc_signaling_get_port(server, &port);
  check_equal(status, 0);
  check_true(port != 0U);

  status = chttp_websocket_client_init(&client, &client_config);
  check_equal(status, SALTS_OK);
  if (status != SALTS_OK) {
    webrtc_signaling_destroy(server);
    return;
  }
  client_initialized = 1;
  snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/", (unsigned int)port);
  options.size = sizeof(options);
  options.uri = uri;
  options.timeout_ms = SIGNALING_WS_TEST_TIMEOUT_MS;
  options.protocol = CHTTP_HTTP_1_1;
  status = chttp_websocket_client_connect(&client, &options, &http_status);
  check_equal(status, SALTS_OK);
  if (status != SALTS_OK) {
    goto cleanup;
  }
  check_equal((int)http_status, 101);
  check_equal(webrtc_signaling_get_peer_count(server), 1);
  status = chttp_websocket_client_send_text(
      &client, join_message, sizeof(join_message) - 1U,
      SIGNALING_WS_TEST_TIMEOUT_MS);
  check_equal(status, SALTS_OK);
  if (status != SALTS_OK) {
    goto cleanup;
  }

  for (index = 0; index < 2; ++index) {
    char message[SIGNALING_WS_TEST_MESSAGE_BYTES + 1U];
    status = chttp_websocket_client_receive(
        &client, SIGNALING_WS_TEST_TIMEOUT_MS, &event);
    check_equal(status, SALTS_OK);
    if (status != SALTS_OK) {
      goto cleanup;
    }
    check_equal((int)event.kind, (int)CHTTP_WEBSOCKET_EVENT_MESSAGE);
    check_equal((int)event.message_type, (int)CHTTP_WEBSOCKET_MESSAGE_TEXT);
    check_true(event.size <= SIGNALING_WS_TEST_MESSAGE_BYTES);
    if (event.size <= SIGNALING_WS_TEST_MESSAGE_BYTES) {
      memcpy(message, event.data, event.size);
      message[event.size] = '\0';
      joined_seen |= strstr(message, "\"type\":\"joined\"") != NULL;
      peers_seen |= strstr(message, "\"type\":\"peers\"") != NULL;
    }
  }
  check_true(joined_seen);
  check_true(peers_seen);

  check_equal(chttp_websocket_client_close(
                  &client, 1000U, NULL, 0U, SIGNALING_WS_TEST_TIMEOUT_MS),
              SALTS_OK);
  for (index = 0; index < 100 &&
                  webrtc_signaling_get_peer_count(server) != 0;
       ++index) {
    salts_sleep_ms(10U);
  }
  check_equal(webrtc_signaling_get_peer_count(server), 0);

cleanup:
  if (client_initialized) {
    status = chttp_websocket_client_destroy(
        &client, SIGNALING_WS_TEST_TIMEOUT_MS);
    check_equal(status, SALTS_OK);
  }
  webrtc_signaling_destroy(server);
}

void test_signaling_peer_auth_configuration_fails_fast(void) {
  webrtc_signaling_config_t config = {.connection_capacity = 4U};
  webrtc_signaling_server_t *server = NULL;

  config.host = "127.0.0.1";
  config.port = 0;
  config.jwt_enabled = 1;
  config.jwt_issuer = "turbomedia";
  config.jwt_active_key_id = "signaling-peer-2026-07";
  config.jwt_secret = "too-short";
  config.jwt_algo = "HS256";
  check_null(webrtc_signaling_create(NULL, &config));

  config.jwt_secret =
      "signaling-peer-active-secret-at-least-32-bytes";
  config.jwt_previous_key_id = "signaling-peer-2026-06";
  check_null(webrtc_signaling_create(NULL, &config));

  config.jwt_previous_secret =
      "signaling-peer-previous-secret-at-least-32-bytes";
  config.jwt_revoked_token_sha256 = "invalid";
  check_null(webrtc_signaling_create(NULL, &config));

  config.jwt_revoked_token_sha256 =
      "0000000000000000000000000000000000000000000000000000000000000000";
  server = webrtc_signaling_create(NULL, &config);
  check_not_null(server);
  check_equal((int)(webrtc_signaling_start(server)), (int)(0));
  webrtc_signaling_stop(server);
  webrtc_signaling_destroy(server);
}

void test_signaling_resource_policy_configuration_fails_fast(void) {
  webrtc_signaling_config_t config = {0};
  webrtc_signaling_server_t *server = NULL;

  check_null(webrtc_signaling_create(NULL, &config));
  config.connection_capacity = 4U;
  config.messages_per_second = 100;
  check_null(webrtc_signaling_create(NULL, &config));

  config.message_burst = 200;
  config.join_timeout_ms = -1;
  check_null(webrtc_signaling_create(NULL, &config));

  config.join_timeout_ms = 10000;
  config.max_connections_per_source = 1;
  check_null(webrtc_signaling_create(NULL, &config));

  config.max_source_states = 16;
  config.source_state_ttl_ms = 1000;
  config.source_admissions_per_second = 10;
  check_null(webrtc_signaling_create(NULL, &config));

  config.source_admission_burst = 20;
  server = webrtc_signaling_create(NULL, &config);
  check_not_null(server);
  webrtc_signaling_destroy(server);
}

void test_signaling_wss_listener_loads_explicit_identity(void) {
  webrtc_signaling_config_t config = {.connection_capacity = 4U};
  webrtc_signaling_server_t *server = NULL;

  config.host = "127.0.0.1";
  config.port = 0;
  config.use_tls = 1;
  config.cert_file = TURBO_MEDIA_TEST_TLS_CERT_PATH;
  config.key_file = TURBO_MEDIA_TEST_TLS_KEY_PATH;
  config.peer_timeout_ms = 1000;

  server = webrtc_signaling_create(NULL, &config);
  check_not_null(server);
  check_equal((int)(webrtc_signaling_start(server)), (int)(0));
  webrtc_signaling_stop(server);
  check_equal((int)(webrtc_signaling_start(server)), (int)(0));
  webrtc_signaling_stop(server);
  webrtc_signaling_destroy(server);
}

void test_http_api_bearer_auth_protects_management_routes(void) {
  webrtc_signaling_config_t signaling_config = {.connection_capacity = 4U};
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

  check_not_null(signaling);
  check_not_null(read_token);
  check_not_null(previous_read_token);
  check_not_null(expired_token);
  check_not_null(room_read_token);
  check_not_null(write_token);
  check_not_null(dangerous_token);
  http = http_api_create(NULL, &http_config, signaling);
  check_not_null(http);
  check_equal((int)(http_api_start(http)), (int)(0));

  check_equal((int)(get_status("http://127.0.0.1:18083",
                                        "/health", NULL, NULL)), (int)(200));
  check_equal((int)(get_status("http://127.0.0.1:18083",
                                        "/api/v1/status", NULL, NULL)), (int)(401));
  check_equal((int)(get_status("http://127.0.0.1:18083",
                                        "/api/v1/status", "wrong-token", NULL)), (int)(401));
  check_equal((int)(get_status("http://127.0.0.1:18083", "/api/v1/status",
                      "test-admin-token", NULL)), (int)(200));
  check_equal((int)(get_status("http://127.0.0.1:18083", "/api/v1/status",
                      read_token, NULL)), (int)(200));
  check_equal((int)(get_status("http://127.0.0.1:18083", "/api/v1/status",
                      previous_read_token, NULL)), (int)(200));
  check_equal((int)(get_status("http://127.0.0.1:18083", "/api/v1/status",
                      expired_token, NULL)), (int)(401));
  check_equal((int)(get_status("http://127.0.0.1:18083",
                      "/api/v1/rooms/room-a/peers", room_read_token, NULL)), (int)(404));
  check_equal((int)(get_status("http://127.0.0.1:18083",
                      "/api/v1/rooms/room-b/peers", room_read_token, NULL)), (int)(401));
  check_equal((int)(delete_status("http://127.0.0.1:18083",
                         "/api/v1/rooms/room-a/peers/peer-a", write_token)), (int)(401));
  check_equal((int)(delete_status("http://127.0.0.1:18083",
                         "/api/v1/rooms/room-a/peers/peer-a",
                         dangerous_token)), (int)(404));

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
  webrtc_signaling_config_t signaling_config = {.connection_capacity = 4U};
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

  check_not_null(signaling);
  http = http_api_create(NULL, &http_config, signaling);
  check_not_null(http);
  check_equal((int)(http_api_start(http)), (int)(0));

  check_equal((int)(get_status("https://127.0.0.1:18084", "/health", NULL, NULL)), (int)(0));
  check_equal((int)(get_status("https://127.0.0.1:18084", "/health", NULL,
                      TURBO_MEDIA_TEST_TLS_CERT_PATH)), (int)(200));

  http_api_destroy(http);
  webrtc_signaling_destroy(signaling);
}

spec("test_signaling_lifecycle") {
  it("test_signaling_and_http_api_instances_have_independent_lifecycles") { test_signaling_and_http_api_instances_have_independent_lifecycles(); };
  it("test_http_api_rejects_invalid_configuration") { test_http_api_rejects_invalid_configuration(); };
  it("test_signaling_native_websocket_listener_stops_and_restarts") { test_signaling_native_websocket_listener_stops_and_restarts(); };
  it("test_signaling_websocket_protocol_round_trip") { test_signaling_websocket_protocol_round_trip(); };
  it("test_signaling_peer_auth_configuration_fails_fast") { test_signaling_peer_auth_configuration_fails_fast(); };
  it("test_signaling_resource_policy_configuration_fails_fast") { test_signaling_resource_policy_configuration_fails_fast(); };
  it("test_signaling_wss_listener_loads_explicit_identity") { test_signaling_wss_listener_loads_explicit_identity(); };
  it("test_http_api_bearer_auth_protects_management_routes") { test_http_api_bearer_auth_protects_management_routes(); };
  it("test_https_management_api_requires_trusted_identity") { test_https_management_api_requires_trusted_identity(); };
}
