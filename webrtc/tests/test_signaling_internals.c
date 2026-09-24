/**
 * @file test_signaling_internals.c
 * @brief Regression checks for signaling room-link internals
 */

#include "tinytest.h"

#include "../src/signaling/webrtc_signaling.c"

static const char *const TEST_ACTIVE_KEY_ID = "signaling-peer-2026-07";
static const char *const TEST_ACTIVE_SECRET =
    "signaling-peer-active-secret-at-least-32-bytes";
static const char *const TEST_PREVIOUS_KEY_ID = "signaling-peer-2026-06";
static const char *const TEST_PREVIOUS_SECRET =
    "signaling-peer-previous-secret-at-least-32-bytes";

static void init_test_server(webrtc_signaling_server_t *server) {
  memset(server, 0, sizeof(*server));
  salts_mutex_init(&server->mutex);
  check_equal(
      hash_map_init_bytes(
          &server->local_peers, sizeof(tstr), CMETA_ALIGNOF(tstr),
          sizeof(webrtc_peer_t *), CMETA_ALIGNOF(webrtc_peer_t *), SIZE_MAX,
          webrtc_str_hash, webrtc_str_equal, NULL),
      STL_OK);
  check_equal(
      hash_map_init_bytes(
          &server->local_rooms, sizeof(tstr), CMETA_ALIGNOF(tstr),
          sizeof(webrtc_room_t *), CMETA_ALIGNOF(webrtc_room_t *), SIZE_MAX,
          webrtc_str_hash, webrtc_str_equal, NULL),
      STL_OK);
  check_equal(
      hash_map_init_bytes(
          &server->source_states, sizeof(signaling_source_key_t),
          CMETA_ALIGNOF(signaling_source_key_t),
          sizeof(signaling_source_state_t *),
          CMETA_ALIGNOF(signaling_source_state_t *), SIZE_MAX,
          hash_bytes, hash_key_equal, NULL),
      STL_OK);
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

  check_true(length > 0);
  if (length <= 0) {
    return NULL;
  }
  json = (char *)malloc((size_t)length + 1U);
  check_not_null(json);
  if (!json) {
    return NULL;
  }
  snprintf(json, (size_t)length + 1U,
           "{\"type\":\"join\",\"room\":\"%s\","
           "\"peer_id\":\"%s\",\"token\":\"%s\"}",
           room_id, peer_id, token);
  check_equal((int)(((root = json_parse((const char *)((const uint8_t *)json), (size_t)length)) ? 0 : -1)), (int)(0));
  free(json);
  return root;
}

static void destroy_test_server(webrtc_signaling_server_t *server) {
  hash_map_destroy(&server->local_peers);
  hash_map_destroy(&server->local_rooms);
  destroy_source_states(server);
  salts_mutex_destroy(&server->mutex);
}

static void init_test_peer(webrtc_signaling_server_t *server,
                           webrtc_peer_t *peer, const char *peer_id) {
  memset(peer, 0, sizeof(*peer));
  peer->server = server;
  peer->id = tstr_dup(peer_id);
  check_not_null(peer->id);
  check_equal((int)(hash_map_put(&server->local_peers, &peer->id, &peer)), (int)(STL_OK));
}

static void destroy_test_peer(webrtc_signaling_server_t *server,
                              webrtc_peer_t *peer) {
  if (peer->room_ptr) {
    remove_peer_from_room_locked(server, peer);
  }
  hash_map_remove(&server->local_peers, &peer->id, NULL);
  free_outbox_locked(peer);
  tstr_free(peer->id);
  memset(peer, 0, sizeof(*peer));
}

static void join_test_peer(webrtc_signaling_server_t *server,
                           webrtc_peer_t *peer, const char *room_id) {
  char message[256];
  int length = snprintf(message, sizeof(message),
                        "{\"type\":\"join\",\"room\":\"%s\"}", room_id);

  check_true(length > 0 && (size_t)length < sizeof(message));
  handle_message(server, peer, message, (size_t)length);
}

static void source_key_to_test_peer(
    const signaling_source_key_t *key, cnet_stream_peer *peer) {
  memset(peer, 0, sizeof(*peer));
  if (key->family == SIGNALING_SOURCE_FAMILY_IPV4) {
    peer->family = CNET_DATAGRAM_ADDRESS_IPV4;
    memcpy(peer->address, key->address, 4U);
  } else {
    peer->family = CNET_DATAGRAM_ADDRESS_IPV6;
    memcpy(peer->address, key->address, 16U);
    peer->scope_id = key->scope_id;
  }
}

void test_trusted_proxy_source_identity_ignores_untrusted_spoofed_headers(void) {
  webrtc_signaling_server_t server;
  signaling_source_key_t direct_key;
  signaling_source_key_t spoofed_key;
  signaling_source_key_t resolved;
  cnet_stream_peer peer;

  memset(&server, 0, sizeof(server));
  check_equal(signaling_trusted_proxy_set_parse(
                  "10.0.0.10,2001:db8::10", &server.trusted_proxies), 0);
  check_equal(signaling_source_key_from_text("198.51.100.20", &direct_key), 0);
  check_equal(signaling_source_key_from_text("203.0.113.99", &spoofed_key), 0);
  source_key_to_test_peer(&direct_key, &peer);

  check_equal(signaling_source_identity_resolve(
                  &server.trusted_proxies, &peer, "203.0.113.99", &resolved), 0);
  check_true(signaling_source_key_equal(&resolved, &direct_key));
  check_false(signaling_source_key_equal(&resolved, &spoofed_key));
}

void test_trusted_proxy_source_identity_requires_one_valid_forwarded_ip(void) {
  webrtc_signaling_server_t server;
  signaling_source_key_t proxy_key;
  signaling_source_key_t client_key;
  signaling_source_key_t resolved;
  cnet_stream_peer peer;

  memset(&server, 0, sizeof(server));
  check_equal(signaling_trusted_proxy_set_parse(
                  "10.0.0.10", &server.trusted_proxies), 0);
  check_equal(signaling_source_key_from_text("10.0.0.10", &proxy_key), 0);
  check_equal(signaling_source_key_from_text("203.0.113.7", &client_key), 0);
  source_key_to_test_peer(&proxy_key, &peer);

  check_equal(signaling_source_identity_resolve(
                  &server.trusted_proxies, &peer, "203.0.113.7", &resolved), 0);
  check_true(signaling_source_key_equal(&resolved, &client_key));

  check_equal(signaling_source_identity_resolve(
                  &server.trusted_proxies, &peer, NULL, &resolved), -1);
  check_equal(signaling_source_identity_resolve(
                  &server.trusted_proxies, &peer,
                  "203.0.113.7, 198.51.100.1", &resolved), -1);
  check_equal(signaling_source_identity_resolve(
                  &server.trusted_proxies, &peer, "client.example", &resolved), -1);
  check_equal(signaling_source_identity_resolve(
                  &server.trusted_proxies, &peer, "[2001:db8::1]", &resolved), -1);
}

void test_trusted_proxy_allowlist_is_exact_bounded_and_normalized(void) {
  webrtc_signaling_server_t server;
  signaling_source_key_t resolved;
  cnet_stream_peer mapped_peer = {0};
  uint8_t mapped[16] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff,
      10, 0, 0, 10};

  memset(&server, 0, sizeof(server));
  check_equal(signaling_trusted_proxy_set_parse(
                  "10.0.0.10", &server.trusted_proxies), 0);
  mapped_peer.family = CNET_DATAGRAM_ADDRESS_IPV6;
  memcpy(mapped_peer.address, mapped, sizeof(mapped));
  check_equal(signaling_source_identity_resolve(
                  &server.trusted_proxies, &mapped_peer,
                  "2001:db8::55", &resolved), 0);
  check_equal((int)resolved.family, SIGNALING_SOURCE_FAMILY_IPV6);

  check_equal(signaling_trusted_proxy_set_parse(
                  "10.0.0.10,10.0.0.10", &server.trusted_proxies), -1);
  check_equal(signaling_trusted_proxy_set_parse(
                  "10.0.0.10/32", &server.trusted_proxies), -1);
  check_equal(signaling_trusted_proxy_set_parse(
                  "2001:db8::1%eth0", &server.trusted_proxies), -1);
}

void test_trusted_proxy_create_rejects_malformed_or_duplicate_allowlists(void) {
  webrtc_signaling_config_t config = {
      .connection_capacity = 4U,
      .use_tls = 1,
      .cert_file = "server.crt",
      .key_file = "server.key",
      .max_connections_per_source = 2,
      .max_source_states = 8U,
      .source_state_ttl_ms = 1000,
      .trusted_proxy_ca_file = "proxy-ca.pem"};
  webrtc_signaling_server_t *server;

  config.trusted_proxy_addresses = "not-an-ip";
  check_null(webrtc_signaling_create(NULL, &config));

  config.trusted_proxy_addresses = "10.0.0.10,10.0.0.10";
  check_null(webrtc_signaling_create(NULL, &config));

  config.trusted_proxy_addresses = "10.0.0.10";
  server = webrtc_signaling_create(NULL, &config);
  check_not_null(server);
  if (server) {
    check_equal((size_t)server->trusted_proxies.count, (size_t)1);
    webrtc_signaling_destroy(server);
  }

  config.trusted_proxy_ca_file = NULL;
  check_null(webrtc_signaling_create(NULL, &config));
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
  check_not_null(room);

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

  check_null(peer_a.room_ptr);
  check_null(peer_a.room);
  check_null(peer_a.prev_in_room);
  check_null(peer_a.next_in_room);
  check_true(room->peers_head == &peer_b);
  check_true(room->peers_tail == &peer_b);
  check_equal((int)(room->peer_count), (int)(1));

  remove_peer_from_room_locked(&server, &peer_b);
  check_equal((int)(server.room_count), (int)(0));

  destroy_test_server(&server);
}

void test_json_string_maybe_escape_skips_plain_candidate_strings(void) {
  const char *candidate = "candidate:1 1 UDP 2130706431 127.0.0.1 5000 typ host";
  const char *json_str = NULL;
  tstr owned = NULL;

  json_str = json_string_maybe_escape(candidate, &owned);

  check_true(json_str == candidate);
  check_null(owned);
}

void test_json_string_maybe_escape_escapes_room_names(void) {
  const char *room = "sales\"tier\\1\n";
  const char *json_str = NULL;
  tstr owned = NULL;

  json_str = json_string_maybe_escape(room, &owned);

  check_not_null(json_str);
  check_not_null(owned);
  check_equal(json_str, "sales\\\"tier\\\\1\\n");
  tstr_free(owned);
}

void test_directed_signaling_rejects_cross_room_messages(void) {
  static const char *const messages[] = {
      "{\"type\":\"offer\",\"to\":\"bob\",\"sdp\":\"offer\"}",
      "{\"type\":\"answer\",\"to\":\"bob\",\"sdp\":\"answer\"}",
      "{\"type\":\"candidate\",\"to\":\"bob\",\"candidate\":\"candidate:1\"}",
      "{\"type\":\"end-of-candidates\",\"to\":\"bob\"}"};
  webrtc_signaling_server_t server;
  webrtc_peer_t alice;
  webrtc_peer_t bob;

  init_test_server(&server);
  init_test_peer(&server, &alice, "alice");
  init_test_peer(&server, &bob, "bob");
  join_test_peer(&server, &alice, "room-a");
  join_test_peer(&server, &bob, "room-b");
  free_outbox_locked(&alice);
  free_outbox_locked(&bob);

  for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); ++i) {
    handle_message(&server, &alice, messages[i], strlen(messages[i]));
    check_equal((size_t)(bob.outbox_message_count), (size_t)(0));
    check_not_null(alice.outbox_tail);
    check_not_null(strstr(alice.outbox_tail->json,
                                "Peer not available in room"));
    free_outbox_locked(&alice);
  }

  destroy_test_peer(&server, &alice);
  destroy_test_peer(&server, &bob);
  destroy_test_server(&server);
}

void test_directed_signaling_routes_messages_within_room(void) {
  static const char *const messages[] = {
      "{\"type\":\"offer\",\"to\":\"bob\",\"sdp\":\"offer\"}",
      "{\"type\":\"answer\",\"to\":\"bob\",\"sdp\":\"answer\"}",
      "{\"type\":\"candidate\",\"to\":\"bob\",\"candidate\":\"candidate:1\"}",
      "{\"type\":\"end-of-candidates\",\"to\":\"bob\"}"};
  webrtc_signaling_server_t server;
  webrtc_peer_t alice;
  webrtc_peer_t bob;

  init_test_server(&server);
  init_test_peer(&server, &alice, "alice");
  init_test_peer(&server, &bob, "bob");
  join_test_peer(&server, &alice, "room-a");
  join_test_peer(&server, &bob, "room-a");
  free_outbox_locked(&alice);
  free_outbox_locked(&bob);

  for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); ++i) {
    handle_message(&server, &alice, messages[i], strlen(messages[i]));
    check_equal((size_t)(bob.outbox_message_count), (size_t)(1));
    free_outbox_locked(&bob);
  }

  destroy_test_peer(&server, &alice);
  destroy_test_peer(&server, &bob);
  destroy_test_server(&server);
}

void test_max_peers_is_enforced_per_room(void) {
  webrtc_signaling_server_t server;
  webrtc_peer_t alice;
  webrtc_peer_t bob;
  webrtc_peer_t carol;

  init_test_server(&server);
  server.config.max_peers = 1;
  init_test_peer(&server, &alice, "alice");
  init_test_peer(&server, &bob, "bob");
  init_test_peer(&server, &carol, "carol");

  join_test_peer(&server, &alice, "room-a");
  check_not_null(alice.room_ptr);
  check_equal((int)(alice.room_ptr->peer_count), (int)(1));

  join_test_peer(&server, &bob, "room-a");
  check_null(bob.room_ptr);
  check_not_null(bob.outbox_tail);
  check_not_null(strstr(bob.outbox_tail->json,
                              "Room peer limit reached"));

  join_test_peer(&server, &carol, "room-b");
  check_not_null(carol.room_ptr);
  check_equal((int)(carol.room_ptr->peer_count), (int)(1));

  join_test_peer(&server, &alice, "room-a");
  check_not_null(alice.room_ptr);
  check_equal((int)(alice.room_ptr->peer_count), (int)(1));
  check_true(alice.room_ptr->peers_head == &alice);
  check_true(alice.room_ptr->peers_tail == &alice);

  destroy_test_peer(&server, &alice);
  destroy_test_peer(&server, &bob);
  destroy_test_peer(&server, &carol);
  destroy_test_server(&server);
}

void test_management_operations_enqueue_without_external_event_loop(void) {
  webrtc_signaling_server_t server;
  webrtc_peer_t alice;

  init_test_server(&server);
  init_test_peer(&server, &alice, "alice");
  join_test_peer(&server, &alice, "room-a");
  free_outbox_locked(&alice);

  check_equal((int)(webrtc_signaling_broadcast(&server, "room-a", NULL, "message")), (int)(1));
  check_equal((int)(webrtc_signaling_kick_peer(&server, "room-a", "alice", "reason")), (int)(0));
  check_equal((size_t)(alice.outbox_message_count), (size_t)(2));
  check_true(alice.closing);

  destroy_test_peer(&server, &alice);
  destroy_test_server(&server);
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
  tstr authorized_peer_id = NULL;

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
  check_not_null(active_token);
  check_not_null(previous_token);
  check_not_null(wrong_room_token);
  check_not_null(wrong_scope_token);
  check_not_null(expired_token);

  root = parse_join_message("room-a", "alice", active_token);
  check_not_null(root);
  check_equal((int)(authorize_join_message(&server, root, &authorized_peer_id)), (int)(0));
  check_equal(authorized_peer_id, "alice");
  tstr_free(authorized_peer_id);
  authorized_peer_id = NULL;
  json_free(root);
  root = NULL;

  root = parse_join_message("room-a", "alice", previous_token);
  check_equal((int)(authorize_join_message(&server, root, &authorized_peer_id)), (int)(0));
  tstr_free(authorized_peer_id);
  authorized_peer_id = NULL;
  json_free(root);
  root = NULL;

  root = parse_join_message("room-a", "alice", wrong_room_token);
  check_equal((int)(authorize_join_message(&server, root, &authorized_peer_id)), (int)(-1));
  json_free(root);
  root = NULL;
  root = parse_join_message("room-a", "alice", wrong_scope_token);
  check_equal((int)(authorize_join_message(&server, root, &authorized_peer_id)), (int)(-1));
  json_free(root);
  root = NULL;
  root = parse_join_message("room-a", "alice", expired_token);
  check_equal((int)(authorize_join_message(&server, root, &authorized_peer_id)), (int)(-1));
  json_free(root);
  root = NULL;

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
  tstr requested_id = NULL;

  memset(&peer_a, 0, sizeof(peer_a));
  memset(&peer_b, 0, sizeof(peer_b));
  init_test_server(&server);
  configure_peer_auth(&server);
  peer_a.id = tstr_dup("temporary-a");
  peer_b.id = tstr_dup("temporary-b");
  check_equal((int)(hash_map_put(&server.local_peers, &peer_a.id, &peer_a)), (int)(STL_OK));
  check_equal((int)(hash_map_put(&server.local_peers, &peer_b.id, &peer_b)), (int)(STL_OK));

  requested_id = tstr_dup("alice");
  check_equal((int)(bind_peer_identity_locked(&server, &peer_a, &requested_id)), (int)(0));
  check_null(requested_id);
  check_true(peer_a.identity_bound);
  check_equal(peer_a.id, "alice");
  check_null(find_peer_by_id_locked(&server, "temporary-a"));
  check_true(find_peer_by_id_locked(&server, "alice") == &peer_a);

  requested_id = tstr_dup("mallory");
  check_equal((int)(bind_peer_identity_locked(&server, &peer_a, &requested_id)), (int)(-1));
  check_equal(peer_a.id, "alice");
  tstr_free(requested_id);
  requested_id = tstr_dup("alice");
  check_equal((int)(bind_peer_identity_locked(&server, &peer_b, &requested_id)), (int)(-2));
  check_equal(peer_b.id, "temporary-b");
  tstr_free(requested_id);

  hash_map_remove(&server.local_peers, &peer_a.id, NULL);
  hash_map_remove(&server.local_peers, &peer_b.id, NULL);
  tstr_free(peer_a.id);
  tstr_free(peer_b.id);
  destroy_test_server(&server);
}

void test_peer_join_dynamic_revocation_starts_unknown_and_recovers(void) {
  webrtc_signaling_config_t config = {
      .connection_capacity = 4U,
      .jwt_enabled = 1,
      .jwt_issuer = "turbomedia",
      .jwt_active_key_id = TEST_ACTIVE_KEY_ID,
      .jwt_secret = TEST_ACTIVE_SECRET,
      .jwt_dynamic_revocation_capacity = 4U,
      .jwt_clock_skew_seconds = 0,
      .jwt_max_ttl_seconds = 3600,
      .jwt_algo = "HS256"};
  webrtc_signaling_server_t *server =
      webrtc_signaling_create(NULL, &config);
  int64_t now = (int64_t)time(NULL);
  char *token = NULL;
  json_value_t *root = NULL;
  tstr authorized_peer_id = NULL;
  uint8_t digest[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];
  char digest_hex[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES * 2U + 1U];
  int synchronized = 0;
  uint64_t epoch = 0U;
  uint64_t sequence = 0U;
  size_t count = 0U;
  static const char hex[] = "0123456789abcdef";

  check_not_null(server);
  token = issue_peer_join_token(
      TEST_ACTIVE_KEY_ID, TEST_ACTIVE_SECRET, SIGNALING_PEER_JOIN_SCOPE,
      "room-a", "alice", now, now + 60);
  check_not_null(token);

  root = parse_join_message("room-a", "alice", token);
  check_not_null(root);
  check_equal((int)(authorize_join_message(
                  server, root, &authorized_peer_id)),
              (int)(-1));
  check_null(authorized_peer_id);
  json_free(root);
  root = NULL;

  check_equal(webrtc_signaling_get_revocation_status(
                  server, &synchronized, &epoch, &sequence, &count),
              0);
  check_false(synchronized);
  check_equal((int)epoch, 0);
  check_equal((int)sequence, 0);
  check_equal((int)count, 0);

  check_equal((int)webrtc_signaling_apply_revocation_snapshot(
                  server, 1U, 0U, NULL, 0U),
              (int)WEBRTC_SIGNALING_REVOCATION_APPLY_APPLIED);
  root = parse_join_message("room-a", "alice", token);
  check_not_null(root);
  check_equal((int)(authorize_join_message(
                  server, root, &authorized_peer_id)),
              (int)(0));
  check_equal(authorized_peer_id, "alice");
  tstr_free(authorized_peer_id);
  authorized_peer_id = NULL;
  json_free(root);
  root = NULL;

  check_equal(turbo_crypto_sha256(
                  token, strlen(token), digest),
              TURBO_CRYPTO_OK);
  for (size_t index = 0U;
       index < TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES; ++index) {
    digest_hex[index * 2U] = hex[digest[index] >> 4U];
    digest_hex[index * 2U + 1U] = hex[digest[index] & 0x0fU];
  }
  digest_hex[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES * 2U] = '\0';
  memset(digest, 0, sizeof(digest));

  check_equal((int)webrtc_signaling_apply_revocation(
                  server, 1U, 1U, digest_hex),
              (int)WEBRTC_SIGNALING_REVOCATION_APPLY_APPLIED);
  root = parse_join_message("room-a", "alice", token);
  check_not_null(root);
  check_equal((int)(authorize_join_message(
                  server, root, &authorized_peer_id)),
              (int)(-1));
  check_null(authorized_peer_id);
  json_free(root);
  root = NULL;

  check_equal((int)webrtc_signaling_apply_revocation_snapshot(
                  server, 2U, 0U, NULL, 0U),
              (int)WEBRTC_SIGNALING_REVOCATION_APPLY_APPLIED);
  root = parse_join_message("room-a", "alice", token);
  check_not_null(root);
  check_equal((int)(authorize_join_message(
                  server, root, &authorized_peer_id)),
              (int)(0));
  tstr_free(authorized_peer_id);
  json_free(root);

  free(token);
  webrtc_signaling_destroy(server);
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
  check_equal((int)(hash_map_put(&server.local_peers, &peer.id, &peer)), (int)(STL_OK));
  check_equal((int)(hash_map_put(&server.local_peers, &unauthenticated_peer.id,
                         &unauthenticated_peer)), (int)(STL_OK));

  token = issue_peer_join_token(
      TEST_ACTIVE_KEY_ID, TEST_ACTIVE_SECRET, SIGNALING_PEER_JOIN_SCOPE,
      "room-a", "alice", now, now + 60);
  check_not_null(token);
  message_length = snprintf(
      NULL, 0,
      "{\"type\":\"join\",\"room\":\"room-a\","
      "\"peer_id\":\"alice\",\"token\":\"%s\"}",
      token);
  message = (char *)malloc((size_t)message_length + 1U);
  check_not_null(message);
  snprintf(message, (size_t)message_length + 1U,
           "{\"type\":\"join\",\"room\":\"room-a\","
           "\"peer_id\":\"alice\",\"token\":\"%s\"}",
           token);

  handle_message(&server, &peer, message, (size_t)message_length);
  check_false(peer.closing);
  check_true(peer.identity_bound);
  check_equal(peer.id, "alice");
  check_equal(peer.room, "room-a");
  check_not_null(peer.outbox_head);
  check_true(strstr(peer.outbox_head->json, "\"type\":\"joined\"") !=
                   NULL);

  handle_message(&server, &unauthenticated_peer,
                 "{\"type\":\"list-peers\"}",
                 strlen("{\"type\":\"list-peers\"}"));
  check_true(unauthenticated_peer.closing);
  check_not_null(unauthenticated_peer.outbox_head);
  check_true(strstr(unauthenticated_peer.outbox_head->json,
                          "Authenticated join required") != NULL);

  free(message);
  free(token);
  free_outbox_locked(&peer);
  free_outbox_locked(&unauthenticated_peer);
  remove_peer_from_room_locked(&server, &peer);
  hash_map_remove(&server.local_peers, &peer.id, NULL);
  hash_map_remove(&server.local_peers, &unauthenticated_peer.id, NULL);
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
  check_equal((int)(hash_map_put(&server.local_peers, &peer.id, &peer)), (int)(STL_OK));

  handle_message(&server, &peer, message, strlen(message));
  check_false(peer.closing);
  check_false(peer.identity_bound);
  check_equal(peer.id, "generated-peer");
  check_equal(peer.room, "development");
  check_not_null(peer.outbox_head);

  free_outbox_locked(&peer);
  remove_peer_from_room_locked(&server, &peer);
  hash_map_remove(&server.local_peers, &peer.id, NULL);
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

  check_true(consume_peer_message_budget_locked(&server, &peer, 1000));
  check_true(consume_peer_message_budget_locked(&server, &peer, 1000));
  check_false(consume_peer_message_budget_locked(&server, &peer, 1000));
  check_true(consume_peer_message_budget_locked(&server, &peer, 1500));
  check_false(consume_peer_message_budget_locked(&server, &peer, 1500));
  check_true(consume_peer_message_budget_locked(&server, &peer, 2000));

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
  peer.rate_last_refill_ms = salts_monotonic_ms();
  peer.rate_tokens = SIGNALING_RATE_TOKEN_UNITS;

  handle_message(&server, &peer, message, strlen(message));
  check_false(peer.closing);
  handle_message(&server, &peer, message, strlen(message));
  check_true(peer.closing);
  check_equal((uint64_t)(server.message_rate_rejections), (uint64_t)(1));
  check_not_null(peer.outbox_tail);
  check_not_null(strstr(peer.outbox_tail->json, "Message rate limit exceeded"));

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

  check_equal((int)(send_json_message_locked(&count_limited, "one")), (int)(0));
  check_equal((int)(send_json_message_locked(&count_limited, "two")), (int)(0));
  check_equal((int)(send_json_message_locked(&count_limited, "overflow")), (int)(-1));
  check_true(count_limited.closing);
  check_equal((uint64_t)(server.outbox_overflow_rejections), (uint64_t)(1));
  check_equal((size_t)(count_limited.outbox_message_count), (size_t)(2));
  check_equal((size_t)(count_limited.outbox_bytes), (size_t)(6));
  free_outbox_locked(&count_limited);

  server.config.max_outbox_messages = 10;
  server.config.max_outbox_bytes = 5;
  byte_limited.server = &server;
  check_equal((int)(send_json_message_locked(&byte_limited, "1234")), (int)(0));
  check_equal((int)(send_json_message_locked(&byte_limited, "56")), (int)(-1));
  check_true(byte_limited.closing);
  check_equal((uint64_t)(server.outbox_overflow_rejections), (uint64_t)(2));
  check_equal((size_t)(byte_limited.outbox_message_count), (size_t)(1));
  check_equal((size_t)(byte_limited.outbox_bytes), (size_t)(4));
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
  check_true(unjoined.closing);
  check_false(joined.closing);
  check_equal((uint64_t)(server.join_timeout_rejections), (uint64_t)(1));

  expire_peers_locked(&server, 7000);
  check_true(joined.closing);
  check_equal((uint64_t)(server.join_timeout_rejections), (uint64_t)(1));

  destroy_test_server(&server);
}

void test_source_key_ignores_port_and_normalizes_mapped_ipv4(void) {
  cnet_stream_peer ipv4_a;
  cnet_stream_peer ipv4_b;
  cnet_stream_peer mapped_ipv4;
  signaling_source_key_t key_a;
  signaling_source_key_t key_b;
  signaling_source_key_t mapped_key;
  const uint8_t address_bytes[4] = {192U, 0U, 2U, 1U};

  memset(&ipv4_a, 0, sizeof(ipv4_a));
  memset(&ipv4_b, 0, sizeof(ipv4_b));
  memset(&mapped_ipv4, 0, sizeof(mapped_ipv4));
  ipv4_a.family = CNET_DATAGRAM_ADDRESS_IPV4;
  ipv4_a.port = 10000;
  memcpy(ipv4_a.address, address_bytes, sizeof(address_bytes));
  ipv4_b = ipv4_a;
  ipv4_b.port = 20000;
  mapped_ipv4.family = CNET_DATAGRAM_ADDRESS_IPV6;
  mapped_ipv4.port = 30000;
  mapped_ipv4.address[10] = 0xffU;
  mapped_ipv4.address[11] = 0xffU;
  memcpy(mapped_ipv4.address + 12U, ipv4_a.address, 4U);

  check_equal((int)(source_key_from_peer(&ipv4_a, &key_a)), (int)(0));
  check_equal((int)(source_key_from_peer(&ipv4_b, &key_b)), (int)(0));
  check_equal((int)(source_key_from_peer(&mapped_ipv4, &mapped_key)), (int)(0));
  check_equal((int)(memcmp(&key_a, &key_b, sizeof(key_a))), (int)(0));
  check_equal((int)(memcmp(&key_a, &mapped_key, sizeof(key_a))), (int)(0));
  check_equal((int)(key_a.family), (int)(SIGNALING_SOURCE_FAMILY_IPV4));
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

  check_equal((int)(admit_source_locked(&server, &key, 1000)), (int)(SIGNALING_SOURCE_ADMITTED));
  check_equal((int)(admit_source_locked(&server, &key, 1000)), (int)(SIGNALING_SOURCE_ADMITTED));
  check_equal((int)(admit_source_locked(&server, &key, 1000)), (int)(SIGNALING_SOURCE_REJECT_CONCURRENCY));
  release_source_key_locked(&server, &key, 1000);
  check_equal((int)(admit_source_locked(&server, &key, 1000)), (int)(SIGNALING_SOURCE_ADMITTED));
  release_source_key_locked(&server, &key, 1000);
  release_source_key_locked(&server, &key, 1000);
  check_equal((size_t)(hash_map_size(&server.source_states)), (size_t)(1));

  expire_source_states_locked(&server, 1999);
  check_equal((size_t)(hash_map_size(&server.source_states)), (size_t)(1));
  expire_source_states_locked(&server, 2000);
  check_equal((size_t)(hash_map_size(&server.source_states)), (size_t)(0));

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

  check_equal((int)(admit_source_locked(&server, &key_a, 1000)), (int)(SIGNALING_SOURCE_ADMITTED));
  check_equal((int)(admit_source_locked(&server, &key_a, 1000)), (int)(SIGNALING_SOURCE_ADMITTED));
  check_equal((int)(admit_source_locked(&server, &key_a, 1000)), (int)(SIGNALING_SOURCE_REJECT_RATE));
  release_source_key_locked(&server, &key_a, 1000);
  release_source_key_locked(&server, &key_a, 1000);
  check_equal((int)(admit_source_locked(&server, &key_b, 1000)), (int)(SIGNALING_SOURCE_REJECT_CAPACITY));
  check_equal((int)(admit_source_locked(&server, &key_a, 1500)), (int)(SIGNALING_SOURCE_ADMITTED));
  release_source_key_locked(&server, &key_a, 1500);
  expire_source_states_locked(&server, 2500);
  check_equal((int)(admit_source_locked(&server, &key_b, 2500)), (int)(SIGNALING_SOURCE_ADMITTED));
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
  check_not_null(json);
  check_not_null(strstr(json, "\"source_state_count\":0"));
  check_not_null(strstr(json, "\"authentication\":1"));
  check_not_null(strstr(json, "\"join_timeout\":2"));
  check_not_null(strstr(json, "\"message_rate\":3"));
  check_not_null(strstr(json, "\"outbox_overflow\":4"));
  check_not_null(strstr(json, "\"source_address\":5"));
  check_not_null(strstr(json, "\"source_capacity\":6"));
  check_not_null(strstr(json, "\"source_rate\":7"));
  check_not_null(strstr(json, "\"source_concurrency\":8"));
  free(json);

  destroy_test_server(&server);
}

spec("test_signaling_internals") {
  it("test_trusted_proxy_source_identity_ignores_untrusted_spoofed_headers") { test_trusted_proxy_source_identity_ignores_untrusted_spoofed_headers(); };
  it("test_trusted_proxy_source_identity_requires_one_valid_forwarded_ip") { test_trusted_proxy_source_identity_requires_one_valid_forwarded_ip(); };
  it("test_trusted_proxy_allowlist_is_exact_bounded_and_normalized") { test_trusted_proxy_allowlist_is_exact_bounded_and_normalized(); };
  it("test_trusted_proxy_create_rejects_malformed_or_duplicate_allowlists") { test_trusted_proxy_create_rejects_malformed_or_duplicate_allowlists(); };
  it("test_remove_peer_from_room_clears_room_links") { test_remove_peer_from_room_clears_room_links(); };
  it("test_json_string_maybe_escape_skips_plain_candidate_strings") { test_json_string_maybe_escape_skips_plain_candidate_strings(); };
  it("test_json_string_maybe_escape_escapes_room_names") { test_json_string_maybe_escape_escapes_room_names(); };
  it("test_directed_signaling_rejects_cross_room_messages") { test_directed_signaling_rejects_cross_room_messages(); };
  it("test_directed_signaling_routes_messages_within_room") { test_directed_signaling_routes_messages_within_room(); };
  it("test_max_peers_is_enforced_per_room") { test_max_peers_is_enforced_per_room(); };
  it("test_management_operations_enqueue_without_external_event_loop") { test_management_operations_enqueue_without_external_event_loop(); };
  it("test_peer_join_auth_binds_room_and_identity") { test_peer_join_auth_binds_room_and_identity(); };
  it("test_peer_identity_binding_is_atomic_and_immutable") { test_peer_identity_binding_is_atomic_and_immutable(); };
  it("test_peer_join_dynamic_revocation_starts_unknown_and_recovers") { test_peer_join_dynamic_revocation_starts_unknown_and_recovers(); };
  it("test_authenticated_join_dispatch_admits_only_valid_first_message") { test_authenticated_join_dispatch_admits_only_valid_first_message(); };
  it("test_legacy_join_remains_available_when_auth_is_disabled") { test_legacy_join_remains_available_when_auth_is_disabled(); };
  it("test_message_rate_bucket_is_bounded_and_refills_with_time") { test_message_rate_bucket_is_bounded_and_refills_with_time(); };
  it("test_message_rate_violation_closes_peer_and_reports_rejection") { test_message_rate_violation_closes_peer_and_reports_rejection(); };
  it("test_outbox_limits_close_slow_consumers") { test_outbox_limits_close_slow_consumers(); };
  it("test_join_deadline_is_fixed_and_idle_timeout_remains_separate") { test_join_deadline_is_fixed_and_idle_timeout_remains_separate(); };
  it("test_source_key_ignores_port_and_normalizes_mapped_ipv4") { test_source_key_ignores_port_and_normalizes_mapped_ipv4(); };
  it("test_source_concurrency_releases_and_expires_state") { test_source_concurrency_releases_and_expires_state(); };
  it("test_source_admission_rate_and_state_capacity_are_bounded") { test_source_admission_rate_and_state_capacity_are_bounded(); };
  it("test_status_reports_resource_rejection_counters") { test_status_reports_resource_rejection_counters(); };
}
