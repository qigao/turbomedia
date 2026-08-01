/**
 * @file test_signaling_internals.c
 * @brief Regression checks for signaling room-link internals
 */

#include "tinytest_compat.h"

#include "../src/signaling/webrtc_signaling.c"

static const char *const TEST_ACTIVE_KEY_ID = "signaling-peer-2026-07";
static const char *const TEST_ACTIVE_SECRET =
    "signaling-peer-active-secret-at-least-32-bytes";
static const char *const TEST_PREVIOUS_KEY_ID = "signaling-peer-2026-06";
static const char *const TEST_PREVIOUS_SECRET =
    "signaling-peer-previous-secret-at-least-32-bytes";

static void init_test_server(webrtc_signaling_server_t *server) {
  memset(server, 0, sizeof(*server));
  turbo_mutex_init(&server->mutex);
  TEST_ASSERT_EQUAL_INT(
      TURBO_OK,
      turbo_hash_map_init(&server->local_peers, sizeof(tstr_t), sizeof(webrtc_peer_t *),
                          webrtc_str_hash, webrtc_str_equal, NULL));
  TEST_ASSERT_EQUAL_INT(
      TURBO_OK,
      turbo_hash_map_init(&server->local_rooms, sizeof(tstr_t), sizeof(webrtc_room_t *),
                          webrtc_str_hash, webrtc_str_equal, NULL));
  TEST_ASSERT_EQUAL_INT(
      TURBO_OK,
      turbo_hash_map_init(&server->source_states,
                          sizeof(signaling_source_key_t),
                          sizeof(signaling_source_state_t *), turbo_hash_bytes,
                          turbo_hash_key_equal, NULL));
}

static void configure_peer_auth(webrtc_signaling_server_t *server) {
  server->config.jwt_enabled = 1;
  server->config.jwt_issuer = "turbomedia";
  server->config.jwt_active_key_id = TEST_ACTIVE_KEY_ID;
  server->config.jwt_secret = TEST_ACTIVE_SECRET;
  server->config.jwt_previous_key_id = TEST_PREVIOUS_KEY_ID;
  server->config.jwt_previous_secret = TEST_PREVIOUS_SECRET;
  server->config.jwt_clock_skew_seconds = 0;
  server->config.jwt_max_ttl_seconds = 3600;
  server->config.jwt_algo = "HS256";
}

static char *issue_peer_join_token(const char *key_id, const char *secret,
                                   const char *scope, const char *room_id,
                                   const char *peer_id, int64_t issued_at,
                                   int64_t expires_at) {
  turbo_media_auth_config_t config = {
      .issuer = "turbomedia",
      .active_key_id = key_id,
      .active_secret = secret,
      .clock_skew_seconds = 0,
      .max_ttl_seconds = 3600};
  turbo_media_auth_claims_t claims = {
      .subject = "signaling-peer-test",
      .audience = SIGNALING_PEER_AUTH_AUDIENCE,
      .scope = scope,
      .room_id = room_id,
      .participant_id = peer_id,
      .issued_at = issued_at,
      .expires_at = expires_at};

  return turbo_media_auth_issue(&config, &claims);
}

static json_value_t *parse_join_message(const char *room_id,
                                        const char *peer_id,
                                        const char *token) {
  char *json = NULL;
  json_value_t *root = NULL;
  int length = snprintf(NULL, 0,
                        "{\"type\":\"join\",\"room\":\"%s\","
                        "\"peer_id\":\"%s\",\"token\":\"%s\"}",
                        room_id, peer_id, token);

  TEST_ASSERT_TRUE(length > 0);
  if (length <= 0) {
    return NULL;
  }
  json = (char *)malloc((size_t)length + 1U);
  TEST_ASSERT_NOT_NULL(json);
  if (!json) {
    return NULL;
  }
  snprintf(json, (size_t)length + 1U,
           "{\"type\":\"join\",\"room\":\"%s\","
           "\"peer_id\":\"%s\",\"token\":\"%s\"}",
           room_id, peer_id, token);
  TEST_ASSERT_EQUAL_INT(
      0, turbo_parse_json((const uint8_t *)json, (size_t)length, &root));
  free(json);
  return root;
}

static void destroy_test_server(webrtc_signaling_server_t *server) {
  turbo_hash_map_destroy(&server->local_peers);
  turbo_hash_map_destroy(&server->local_rooms);
  destroy_source_states(server);
  turbo_mutex_destroy(&server->mutex);
}

void test_remove_peer_from_room_clears_room_links(void) {
  webrtc_signaling_server_t server;
  webrtc_room_t *room = NULL;
  webrtc_peer_t peer_a;
  webrtc_peer_t peer_b;

  memset(&peer_a, 0, sizeof(peer_a));
  memset(&peer_b, 0, sizeof(peer_b));
  init_test_server(&server);

  room = create_room_locked(&server, "room-a");
  TEST_ASSERT_NOT_NULL(room);

  peer_b.room = tstr_dup("room-a");
  peer_b.room_ptr = room;
  room->peers_head = &peer_b;
  room->peers_tail = &peer_b;
  room->peer_count = 1;

  peer_a.room = tstr_dup("room-a");
  peer_a.room_ptr = room;
  peer_a.prev_in_room = &peer_b;
  peer_b.next_in_room = &peer_a;
  room->peers_tail = &peer_a;
  room->peer_count = 2;

  remove_peer_from_room_locked(&server, &peer_a);

  TEST_ASSERT_NULL(peer_a.room_ptr);
  TEST_ASSERT_NULL(peer_a.room);
  TEST_ASSERT_NULL(peer_a.prev_in_room);
  TEST_ASSERT_NULL(peer_a.next_in_room);
  TEST_ASSERT_TRUE(room->peers_head == &peer_b);
  TEST_ASSERT_TRUE(room->peers_tail == &peer_b);
  TEST_ASSERT_EQUAL_INT(1, room->peer_count);

  remove_peer_from_room_locked(&server, &peer_b);
  TEST_ASSERT_EQUAL_INT(0, server.room_count);

  destroy_test_server(&server);
}

void test_json_string_maybe_escape_skips_plain_candidate_strings(void) {
  const char *candidate = "candidate:1 1 UDP 2130706431 127.0.0.1 5000 typ host";
  const char *json_str = NULL;
  tstr_t owned = NULL;

  json_str = json_string_maybe_escape(candidate, &owned);

  TEST_ASSERT_TRUE(json_str == candidate);
  TEST_ASSERT_NULL(owned);
}

void test_json_string_maybe_escape_escapes_room_names(void) {
  const char *room = "sales\"tier\\1\n";
  const char *json_str = NULL;
  tstr_t owned = NULL;

  json_str = json_string_maybe_escape(room, &owned);

  TEST_ASSERT_NOT_NULL(json_str);
  TEST_ASSERT_NOT_NULL(owned);
  TEST_ASSERT_EQUAL_STRING("sales\\\"tier\\\\1\\n", json_str);
  tstr_free(owned);
}

void test_peer_join_auth_binds_room_and_identity(void) {
  webrtc_signaling_server_t server;
  int64_t now = (int64_t)time(NULL);
  char *active_token = NULL;
  char *previous_token = NULL;
  char *wrong_room_token = NULL;
  char *wrong_scope_token = NULL;
  char *expired_token = NULL;
  json_value_t *root = NULL;
  tstr_t authorized_peer_id = NULL;

  init_test_server(&server);
  configure_peer_auth(&server);
  active_token = issue_peer_join_token(
      TEST_ACTIVE_KEY_ID, TEST_ACTIVE_SECRET, SIGNALING_PEER_JOIN_SCOPE,
      "room-a", "alice", now, now + 60);
  previous_token = issue_peer_join_token(
      TEST_PREVIOUS_KEY_ID, TEST_PREVIOUS_SECRET, SIGNALING_PEER_JOIN_SCOPE,
      "room-a", "alice", now, now + 60);
  wrong_room_token = issue_peer_join_token(
      TEST_ACTIVE_KEY_ID, TEST_ACTIVE_SECRET, SIGNALING_PEER_JOIN_SCOPE,
      "room-b", "alice", now, now + 60);
  wrong_scope_token = issue_peer_join_token(
      TEST_ACTIVE_KEY_ID, TEST_ACTIVE_SECRET, "signaling.peer.observe",
      "room-a", "alice", now, now + 60);
  expired_token = issue_peer_join_token(
      TEST_ACTIVE_KEY_ID, TEST_ACTIVE_SECRET, SIGNALING_PEER_JOIN_SCOPE,
      "room-a", "alice", now - 120, now - 60);
  TEST_ASSERT_NOT_NULL(active_token);
  TEST_ASSERT_NOT_NULL(previous_token);
  TEST_ASSERT_NOT_NULL(wrong_room_token);
  TEST_ASSERT_NOT_NULL(wrong_scope_token);
  TEST_ASSERT_NOT_NULL(expired_token);

  root = parse_join_message("room-a", "alice", active_token);
  TEST_ASSERT_NOT_NULL(root);
  TEST_ASSERT_EQUAL_INT(
      0, authorize_join_message(&server, root, &authorized_peer_id));
  TEST_ASSERT_EQUAL_STRING("alice", authorized_peer_id);
  tstr_free(authorized_peer_id);
  authorized_peer_id = NULL;
  turbo_free_json(&root);

  root = parse_join_message("room-a", "alice", previous_token);
  TEST_ASSERT_EQUAL_INT(
      0, authorize_join_message(&server, root, &authorized_peer_id));
  tstr_free(authorized_peer_id);
  authorized_peer_id = NULL;
  turbo_free_json(&root);

  root = parse_join_message("room-a", "alice", wrong_room_token);
  TEST_ASSERT_EQUAL_INT(
      -1, authorize_join_message(&server, root, &authorized_peer_id));
  turbo_free_json(&root);
  root = parse_join_message("room-a", "alice", wrong_scope_token);
  TEST_ASSERT_EQUAL_INT(
      -1, authorize_join_message(&server, root, &authorized_peer_id));
  turbo_free_json(&root);
  root = parse_join_message("room-a", "alice", expired_token);
  TEST_ASSERT_EQUAL_INT(
      -1, authorize_join_message(&server, root, &authorized_peer_id));
  turbo_free_json(&root);

  free(expired_token);
  free(wrong_scope_token);
  free(wrong_room_token);
  free(previous_token);
  free(active_token);
  destroy_test_server(&server);
}

void test_peer_identity_binding_is_atomic_and_immutable(void) {
  webrtc_signaling_server_t server;
  webrtc_peer_t peer_a;
  webrtc_peer_t peer_b;
  tstr_t requested_id = NULL;

  memset(&peer_a, 0, sizeof(peer_a));
  memset(&peer_b, 0, sizeof(peer_b));
  init_test_server(&server);
  configure_peer_auth(&server);
  peer_a.id = tstr_dup("temporary-a");
  peer_b.id = tstr_dup("temporary-b");
  TEST_ASSERT_EQUAL_INT(
      TURBO_OK, turbo_hash_map_put(&server.local_peers, &peer_a.id, &peer_a));
  TEST_ASSERT_EQUAL_INT(
      TURBO_OK, turbo_hash_map_put(&server.local_peers, &peer_b.id, &peer_b));

  requested_id = tstr_dup("alice");
  TEST_ASSERT_EQUAL_INT(
      0, bind_peer_identity_locked(&server, &peer_a, &requested_id));
  TEST_ASSERT_NULL(requested_id);
  TEST_ASSERT_TRUE(peer_a.identity_bound);
  TEST_ASSERT_EQUAL_STRING("alice", peer_a.id);
  TEST_ASSERT_NULL(find_peer_by_id_locked(&server, "temporary-a"));
  TEST_ASSERT_TRUE(find_peer_by_id_locked(&server, "alice") == &peer_a);

  requested_id = tstr_dup("mallory");
  TEST_ASSERT_EQUAL_INT(
      -1, bind_peer_identity_locked(&server, &peer_a, &requested_id));
  TEST_ASSERT_EQUAL_STRING("alice", peer_a.id);
  tstr_free(requested_id);
  requested_id = tstr_dup("alice");
  TEST_ASSERT_EQUAL_INT(
      -2, bind_peer_identity_locked(&server, &peer_b, &requested_id));
  TEST_ASSERT_EQUAL_STRING("temporary-b", peer_b.id);
  tstr_free(requested_id);

  turbo_hash_map_remove(&server.local_peers, &peer_a.id, NULL);
  turbo_hash_map_remove(&server.local_peers, &peer_b.id, NULL);
  tstr_free(peer_a.id);
  tstr_free(peer_b.id);
  destroy_test_server(&server);
}

void test_authenticated_join_dispatch_admits_only_valid_first_message(void) {
  webrtc_signaling_server_t server;
  webrtc_peer_t peer;
  webrtc_peer_t unauthenticated_peer;
  int64_t now = (int64_t)time(NULL);
  char *token = NULL;
  char *message = NULL;
  int message_length = 0;

  memset(&peer, 0, sizeof(peer));
  memset(&unauthenticated_peer, 0, sizeof(unauthenticated_peer));
  init_test_server(&server);
  configure_peer_auth(&server);
  peer.server = &server;
  unauthenticated_peer.server = &server;
  peer.id = tstr_dup("temporary-a");
  unauthenticated_peer.id = tstr_dup("temporary-b");
  TEST_ASSERT_EQUAL_INT(
      TURBO_OK, turbo_hash_map_put(&server.local_peers, &peer.id, &peer));
  TEST_ASSERT_EQUAL_INT(
      TURBO_OK,
      turbo_hash_map_put(&server.local_peers, &unauthenticated_peer.id,
                         &unauthenticated_peer));

  token = issue_peer_join_token(
      TEST_ACTIVE_KEY_ID, TEST_ACTIVE_SECRET, SIGNALING_PEER_JOIN_SCOPE,
      "room-a", "alice", now, now + 60);
  TEST_ASSERT_NOT_NULL(token);
  message_length = snprintf(
      NULL, 0,
      "{\"type\":\"join\",\"room\":\"room-a\","
      "\"peer_id\":\"alice\",\"token\":\"%s\"}",
      token);
  message = (char *)malloc((size_t)message_length + 1U);
  TEST_ASSERT_NOT_NULL(message);
  snprintf(message, (size_t)message_length + 1U,
           "{\"type\":\"join\",\"room\":\"room-a\","
           "\"peer_id\":\"alice\",\"token\":\"%s\"}",
           token);

  handle_message(&server, &peer, message, (size_t)message_length);
  TEST_ASSERT_FALSE(peer.closing);
  TEST_ASSERT_TRUE(peer.identity_bound);
  TEST_ASSERT_EQUAL_STRING("alice", peer.id);
  TEST_ASSERT_EQUAL_STRING("room-a", peer.room);
  TEST_ASSERT_NOT_NULL(peer.outbox_head);
  TEST_ASSERT_TRUE(strstr(peer.outbox_head->json, "\"type\":\"joined\"") !=
                   NULL);

  handle_message(&server, &unauthenticated_peer,
                 "{\"type\":\"list-peers\"}",
                 strlen("{\"type\":\"list-peers\"}"));
  TEST_ASSERT_TRUE(unauthenticated_peer.closing);
  TEST_ASSERT_NOT_NULL(unauthenticated_peer.outbox_head);
  TEST_ASSERT_TRUE(strstr(unauthenticated_peer.outbox_head->json,
                          "Authenticated join required") != NULL);

  free(message);
  free(token);
  free_outbox_locked(&peer);
  free_outbox_locked(&unauthenticated_peer);
  remove_peer_from_room_locked(&server, &peer);
  turbo_hash_map_remove(&server.local_peers, &peer.id, NULL);
  turbo_hash_map_remove(&server.local_peers, &unauthenticated_peer.id, NULL);
  tstr_free(peer.id);
  tstr_free(unauthenticated_peer.id);
  destroy_test_server(&server);
}

void test_legacy_join_remains_available_when_auth_is_disabled(void) {
  webrtc_signaling_server_t server;
  webrtc_peer_t peer;
  const char *message = "{\"type\":\"join\",\"room\":\"development\"}";

  memset(&peer, 0, sizeof(peer));
  init_test_server(&server);
  peer.server = &server;
  peer.id = tstr_dup("generated-peer");
  TEST_ASSERT_EQUAL_INT(
      TURBO_OK, turbo_hash_map_put(&server.local_peers, &peer.id, &peer));

  handle_message(&server, &peer, message, strlen(message));
  TEST_ASSERT_FALSE(peer.closing);
  TEST_ASSERT_FALSE(peer.identity_bound);
  TEST_ASSERT_EQUAL_STRING("generated-peer", peer.id);
  TEST_ASSERT_EQUAL_STRING("development", peer.room);
  TEST_ASSERT_NOT_NULL(peer.outbox_head);

  free_outbox_locked(&peer);
  remove_peer_from_room_locked(&server, &peer);
  turbo_hash_map_remove(&server.local_peers, &peer.id, NULL);
  tstr_free(peer.id);
  destroy_test_server(&server);
}

void test_message_rate_bucket_is_bounded_and_refills_with_time(void) {
  webrtc_signaling_server_t server;
  webrtc_peer_t peer;

  memset(&peer, 0, sizeof(peer));
  init_test_server(&server);
  server.config.messages_per_second = 2;
  server.config.message_burst = 2;
  peer.rate_last_refill_ms = 1000;
  peer.rate_tokens = 2U * SIGNALING_RATE_TOKEN_UNITS;

  TEST_ASSERT_TRUE(consume_peer_message_budget_locked(&server, &peer, 1000));
  TEST_ASSERT_TRUE(consume_peer_message_budget_locked(&server, &peer, 1000));
  TEST_ASSERT_FALSE(consume_peer_message_budget_locked(&server, &peer, 1000));
  TEST_ASSERT_TRUE(consume_peer_message_budget_locked(&server, &peer, 1500));
  TEST_ASSERT_FALSE(consume_peer_message_budget_locked(&server, &peer, 1500));
  TEST_ASSERT_TRUE(consume_peer_message_budget_locked(&server, &peer, 2000));

  destroy_test_server(&server);
}

void test_message_rate_violation_closes_peer_and_reports_rejection(void) {
  webrtc_signaling_server_t server;
  webrtc_peer_t peer;
  const char *message = "{\"type\":\"unknown\"}";

  memset(&peer, 0, sizeof(peer));
  init_test_server(&server);
  server.config.messages_per_second = 1;
  server.config.message_burst = 1;
  peer.server = &server;
  peer.rate_last_refill_ms = turbo_monotonic_ms();
  peer.rate_tokens = SIGNALING_RATE_TOKEN_UNITS;

  handle_message(&server, &peer, message, strlen(message));
  TEST_ASSERT_FALSE(peer.closing);
  handle_message(&server, &peer, message, strlen(message));
  TEST_ASSERT_TRUE(peer.closing);
  TEST_ASSERT_EQUAL_UINT64(1, server.message_rate_rejections);
  TEST_ASSERT_NOT_NULL(peer.outbox_tail);
  TEST_ASSERT_NOT_NULL(
      strstr(peer.outbox_tail->json, "Message rate limit exceeded"));

  free_outbox_locked(&peer);
  destroy_test_server(&server);
}

void test_outbox_limits_close_slow_consumers(void) {
  webrtc_signaling_server_t server;
  webrtc_peer_t count_limited;
  webrtc_peer_t byte_limited;

  memset(&count_limited, 0, sizeof(count_limited));
  memset(&byte_limited, 0, sizeof(byte_limited));
  init_test_server(&server);
  server.config.max_outbox_messages = 2;
  server.config.max_outbox_bytes = 1024;
  count_limited.server = &server;

  TEST_ASSERT_EQUAL_INT(0, enqueue_message_locked(&count_limited, "one"));
  TEST_ASSERT_EQUAL_INT(0, enqueue_message_locked(&count_limited, "two"));
  TEST_ASSERT_EQUAL_INT(
      -1, enqueue_message_locked(&count_limited, "overflow"));
  TEST_ASSERT_TRUE(count_limited.closing);
  TEST_ASSERT_EQUAL_UINT64(1, server.outbox_overflow_rejections);
  TEST_ASSERT_EQUAL_size_t(2, count_limited.outbox_message_count);
  TEST_ASSERT_EQUAL_size_t(6, count_limited.outbox_bytes);
  free_outbox_locked(&count_limited);

  server.config.max_outbox_messages = 10;
  server.config.max_outbox_bytes = 5;
  byte_limited.server = &server;
  TEST_ASSERT_EQUAL_INT(0, enqueue_message_locked(&byte_limited, "1234"));
  TEST_ASSERT_EQUAL_INT(-1, enqueue_message_locked(&byte_limited, "56"));
  TEST_ASSERT_TRUE(byte_limited.closing);
  TEST_ASSERT_EQUAL_UINT64(2, server.outbox_overflow_rejections);
  TEST_ASSERT_EQUAL_size_t(1, byte_limited.outbox_message_count);
  TEST_ASSERT_EQUAL_size_t(4, byte_limited.outbox_bytes);
  free_outbox_locked(&byte_limited);

  destroy_test_server(&server);
}

void test_join_deadline_is_fixed_and_idle_timeout_remains_separate(void) {
  webrtc_signaling_server_t server;
  webrtc_peer_t unjoined;
  webrtc_peer_t joined;
  webrtc_room_t room;

  memset(&unjoined, 0, sizeof(unjoined));
  memset(&joined, 0, sizeof(joined));
  memset(&room, 0, sizeof(room));
  init_test_server(&server);
  server.config.join_timeout_ms = 1000;
  server.config.peer_timeout_ms = 5000;
  unjoined.server = &server;
  unjoined.connected_at = 1000;
  unjoined.last_activity = 1900;
  unjoined.next = &joined;
  joined.server = &server;
  joined.connected_at = 1000;
  joined.last_activity = 1900;
  joined.room_ptr = &room;
  joined.prev = &unjoined;
  server.peers_head = &unjoined;
  server.peers_tail = &joined;

  expire_peers_locked(&server, 2000);
  TEST_ASSERT_TRUE(unjoined.closing);
  TEST_ASSERT_FALSE(joined.closing);
  TEST_ASSERT_EQUAL_UINT64(1, server.join_timeout_rejections);

  expire_peers_locked(&server, 7000);
  TEST_ASSERT_TRUE(joined.closing);
  TEST_ASSERT_EQUAL_UINT64(1, server.join_timeout_rejections);

  destroy_test_server(&server);
}

void test_source_key_ignores_port_and_normalizes_mapped_ipv4(void) {
  struct sockaddr_in ipv4_a;
  struct sockaddr_in ipv4_b;
  struct sockaddr_in6 mapped_ipv4;
  signaling_source_key_t key_a;
  signaling_source_key_t key_b;
  signaling_source_key_t mapped_key;
  uint8_t *mapped_bytes;
  const uint8_t address_bytes[4] = {192U, 0U, 2U, 1U};

  memset(&ipv4_a, 0, sizeof(ipv4_a));
  memset(&ipv4_b, 0, sizeof(ipv4_b));
  memset(&mapped_ipv4, 0, sizeof(mapped_ipv4));
  ipv4_a.sin_family = AF_INET;
  ipv4_a.sin_port = 10000;
  memcpy(&ipv4_a.sin_addr, address_bytes, sizeof(address_bytes));
  ipv4_b = ipv4_a;
  ipv4_b.sin_port = 20000;
  mapped_ipv4.sin6_family = AF_INET6;
  mapped_ipv4.sin6_port = 30000;
  mapped_bytes = (uint8_t *)&mapped_ipv4.sin6_addr;
  mapped_bytes[10] = 0xffU;
  mapped_bytes[11] = 0xffU;
  memcpy(mapped_bytes + 12U, &ipv4_a.sin_addr, 4U);

  TEST_ASSERT_EQUAL_INT(
      0, source_key_from_sockaddr(
             (const struct sockaddr_storage *)&ipv4_a, &key_a));
  TEST_ASSERT_EQUAL_INT(
      0, source_key_from_sockaddr(
             (const struct sockaddr_storage *)&ipv4_b, &key_b));
  TEST_ASSERT_EQUAL_INT(
      0, source_key_from_sockaddr(
             (const struct sockaddr_storage *)&mapped_ipv4, &mapped_key));
  TEST_ASSERT_EQUAL_INT(0, memcmp(&key_a, &key_b, sizeof(key_a)));
  TEST_ASSERT_EQUAL_INT(0, memcmp(&key_a, &mapped_key, sizeof(key_a)));
  TEST_ASSERT_EQUAL_INT(SIGNALING_SOURCE_FAMILY_IPV4, key_a.family);
}

void test_source_concurrency_releases_and_expires_state(void) {
  webrtc_signaling_server_t server;
  signaling_source_key_t key;

  memset(&key, 0, sizeof(key));
  key.family = SIGNALING_SOURCE_FAMILY_IPV4;
  key.address[0] = 192U;
  key.address[1] = 0U;
  key.address[2] = 2U;
  key.address[3] = 10U;
  init_test_server(&server);
  server.config.max_connections_per_source = 2;
  server.config.max_source_states = 2U;
  server.config.source_state_ttl_ms = 1000;

  TEST_ASSERT_EQUAL_INT(
      SIGNALING_SOURCE_ADMITTED, admit_source_locked(&server, &key, 1000));
  TEST_ASSERT_EQUAL_INT(
      SIGNALING_SOURCE_ADMITTED, admit_source_locked(&server, &key, 1000));
  TEST_ASSERT_EQUAL_INT(SIGNALING_SOURCE_REJECT_CONCURRENCY,
                        admit_source_locked(&server, &key, 1000));
  release_source_key_locked(&server, &key, 1000);
  TEST_ASSERT_EQUAL_INT(
      SIGNALING_SOURCE_ADMITTED, admit_source_locked(&server, &key, 1000));
  release_source_key_locked(&server, &key, 1000);
  release_source_key_locked(&server, &key, 1000);
  TEST_ASSERT_EQUAL_size_t(1, turbo_hash_map_size(&server.source_states));

  expire_source_states_locked(&server, 1999);
  TEST_ASSERT_EQUAL_size_t(1, turbo_hash_map_size(&server.source_states));
  expire_source_states_locked(&server, 2000);
  TEST_ASSERT_EQUAL_size_t(0, turbo_hash_map_size(&server.source_states));

  destroy_test_server(&server);
}

void test_source_admission_rate_and_state_capacity_are_bounded(void) {
  webrtc_signaling_server_t server;
  signaling_source_key_t key_a;
  signaling_source_key_t key_b;

  memset(&key_a, 0, sizeof(key_a));
  memset(&key_b, 0, sizeof(key_b));
  key_a.family = SIGNALING_SOURCE_FAMILY_IPV4;
  key_a.address[3] = 1U;
  key_b.family = SIGNALING_SOURCE_FAMILY_IPV4;
  key_b.address[3] = 2U;
  init_test_server(&server);
  server.config.source_admissions_per_second = 2;
  server.config.source_admission_burst = 2;
  server.config.max_source_states = 1U;
  server.config.source_state_ttl_ms = 1000;

  TEST_ASSERT_EQUAL_INT(
      SIGNALING_SOURCE_ADMITTED, admit_source_locked(&server, &key_a, 1000));
  TEST_ASSERT_EQUAL_INT(
      SIGNALING_SOURCE_ADMITTED, admit_source_locked(&server, &key_a, 1000));
  TEST_ASSERT_EQUAL_INT(SIGNALING_SOURCE_REJECT_RATE,
                        admit_source_locked(&server, &key_a, 1000));
  release_source_key_locked(&server, &key_a, 1000);
  release_source_key_locked(&server, &key_a, 1000);
  TEST_ASSERT_EQUAL_INT(SIGNALING_SOURCE_REJECT_CAPACITY,
                        admit_source_locked(&server, &key_b, 1000));
  TEST_ASSERT_EQUAL_INT(
      SIGNALING_SOURCE_ADMITTED, admit_source_locked(&server, &key_a, 1500));
  release_source_key_locked(&server, &key_a, 1500);
  expire_source_states_locked(&server, 2500);
  TEST_ASSERT_EQUAL_INT(
      SIGNALING_SOURCE_ADMITTED, admit_source_locked(&server, &key_b, 2500));
  release_source_key_locked(&server, &key_b, 2500);

  destroy_test_server(&server);
}

void test_status_reports_resource_rejection_counters(void) {
  webrtc_signaling_server_t server;
  char *json = NULL;

  init_test_server(&server);
  server.authentication_rejections = 1;
  server.join_timeout_rejections = 2;
  server.message_rate_rejections = 3;
  server.outbox_overflow_rejections = 4;
  server.source_address_rejections = 4;
  server.source_capacity_rejections = 5;
  server.source_rate_rejections = 6;
  server.source_concurrency_rejections = 7;
  record_source_rejection_locked(&server, SIGNALING_SOURCE_REJECT_ADDRESS);
  record_source_rejection_locked(&server, SIGNALING_SOURCE_REJECT_CAPACITY);
  record_source_rejection_locked(&server, SIGNALING_SOURCE_REJECT_RATE);
  record_source_rejection_locked(&server,
                                 SIGNALING_SOURCE_REJECT_CONCURRENCY);

  json = webrtc_signaling_get_status_json(&server);
  TEST_ASSERT_NOT_NULL(json);
  TEST_ASSERT_NOT_NULL(strstr(json, "\"source_state_count\":0"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"authentication\":1"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"join_timeout\":2"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"message_rate\":3"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"outbox_overflow\":4"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"source_address\":5"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"source_capacity\":6"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"source_rate\":7"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"source_concurrency\":8"));
  free(json);

  destroy_test_server(&server);
}

spec("test_signaling_internals") {
  TT_TEST(test_remove_peer_from_room_clears_room_links);
  TT_TEST(test_json_string_maybe_escape_skips_plain_candidate_strings);
  TT_TEST(test_json_string_maybe_escape_escapes_room_names);
  TT_TEST(test_peer_join_auth_binds_room_and_identity);
  TT_TEST(test_peer_identity_binding_is_atomic_and_immutable);
  TT_TEST(test_authenticated_join_dispatch_admits_only_valid_first_message);
  TT_TEST(test_legacy_join_remains_available_when_auth_is_disabled);
  TT_TEST(test_message_rate_bucket_is_bounded_and_refills_with_time);
  TT_TEST(test_message_rate_violation_closes_peer_and_reports_rejection);
  TT_TEST(test_outbox_limits_close_slow_consumers);
  TT_TEST(test_join_deadline_is_fixed_and_idle_timeout_remains_separate);
  TT_TEST(test_source_key_ignores_port_and_normalizes_mapped_ipv4);
  TT_TEST(test_source_concurrency_releases_and_expires_state);
  TT_TEST(test_source_admission_rate_and_state_capacity_are_bounded);
  TT_TEST(test_status_reports_resource_rejection_counters);
}
