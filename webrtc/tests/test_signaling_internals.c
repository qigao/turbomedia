/**
 * @file test_signaling_internals.c
 * @brief Regression checks for signaling room-link internals
 */

#include "tinytest_compat.h"

#include "../src/signaling/webrtc_signaling.c"

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
}

static void destroy_test_server(webrtc_signaling_server_t *server) {
  turbo_hash_map_destroy(&server->local_peers);
  turbo_hash_map_destroy(&server->local_rooms);
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

void test_ws_build_accept_key_matches_rfc_example(void) {
  unsigned char accept_key[64];

  memset(accept_key, 0, sizeof(accept_key));

  TEST_ASSERT_EQUAL_INT(0, ws_build_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept_key,
                                               sizeof(accept_key)));
  TEST_ASSERT_EQUAL_STRING("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", (const char *)accept_key);
}

spec("test_signaling_internals") {
  TT_TEST(test_remove_peer_from_room_clears_room_links);
  TT_TEST(test_json_string_maybe_escape_skips_plain_candidate_strings);
  TT_TEST(test_json_string_maybe_escape_escapes_room_names);
  TT_TEST(test_ws_build_accept_key_matches_rfc_example);
}
