/**
 * @file test_signaling_json.c
 * @brief Regression checks for signaling JSON parse/free lifecycle
 */

#include "tinytest.h"
#include <json_parser.h>

static void parse_and_free_signaling_message(const char *json_text) {
  json_value_t *root = NULL;

  check_not_null(json_text);
  check_equal((int)(((root = json_parse((const char *)((const uint8_t *)json_text), strlen(json_text))) ? 0 : -1)), (int)(0));
  check_not_null(root);
  check_equal((int)(json_type(root)), (int)(JSON_OBJECT));

  json_free(root);

  root = NULL;
  check_null(root);
}

void test_signaling_json_messages_can_be_freed(void) {
  parse_and_free_signaling_message("{\"type\":\"join\",\"room\":\"demo\"}");
  parse_and_free_signaling_message(
      "{\"type\":\"offer\",\"to\":\"peer-b\",\"sdp\":\"v=0\\r\\no=- 1 1 IN IP4 127.0.0.1\"}");
  parse_and_free_signaling_message(
      "{\"type\":\"answer\",\"to\":\"peer-a\",\"sdp\":\"v=0\\r\\no=- 2 2 IN IP4 127.0.0.1\"}");
  parse_and_free_signaling_message(
      "{\"type\":\"candidate\",\"to\":\"peer-b\",\"candidate\":\"candidate:1 1 UDP 2130706431 "
      "127.0.0.1 5000 typ host\"}");
  parse_and_free_signaling_message("{\"type\":\"end-of-candidates\",\"to\":\"peer-a\"}");
}

spec("test_signaling_json") { it("test_signaling_json_messages_can_be_freed") { test_signaling_json_messages_can_be_freed(); }; }
