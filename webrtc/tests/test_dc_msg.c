/**
 * test_dc_msg.c - Unit tests for WebRTC DataChannel LTV messaging
 *
 * Tests the turbo_dc_msg API (wrapper around turbo_ltv_*)
 */

#include "turbo_dc_msg.h"
#include "tinytest.h"
#include <string.h>
#include <stdlib.h>

void setUp(void) {
}

void tearDown(void) {
}

/* ============================================================================
 * turbo_dc_is_ltv Tests
 * ============================================================================ */

void test_is_ltv_null(void) {
    int result = turbo_dc_is_ltv(NULL, 0);
    check_equal(result, 0);
}

void test_is_ltv_too_short(void) {
    uint8_t buf[] = {0x01};
    int result = turbo_dc_is_ltv(buf, 1);
    check_equal(result, 0);
}

void test_is_ltv_empty_message(void) {
    /* LTV: length=1 (varint 0x01), type=0x00, no value */
    uint8_t buf[] = {0x01, 0x00};
    int result = turbo_dc_is_ltv(buf, sizeof(buf));
    check_equal(result, 1);
}

void test_is_ltv_with_payload(void) {
    /* LTV: length=6 (varint 0x06), type=0x42, value="hello" */
    uint8_t buf[] = {0x06, 0x42, 'h', 'e', 'l', 'l', 'o'};
    int result = turbo_dc_is_ltv(buf, sizeof(buf));
    check_equal(result, 1);
}

void test_is_ltv_truncated(void) {
    /* LTV header says 6 bytes but only 3 provided */
    uint8_t buf[] = {0x06, 0x42, 'h'};
    int result = turbo_dc_is_ltv(buf, sizeof(buf));
    check_equal(result, 0);
}

/* ============================================================================
 * turbo_dc_parse_ltv Tests
 * ============================================================================ */

void test_parse_ltv_null_data(void) {
    ltv_message_t *msg = NULL;
    int rc = turbo_dc_parse_ltv(NULL, 0, &msg);
    check_equal(rc, -1);
}

void test_parse_ltv_null_output(void) {
    uint8_t buf[] = {0x01, 0x00};
    int rc = turbo_dc_parse_ltv(buf, sizeof(buf), NULL);
    check_equal(rc, -1);
}

void test_parse_ltv_empty_message(void) {
    /* LTV: length=1, type=0x00, no value */
    uint8_t buf[] = {0x01, 0x00};
    ltv_message_t *msg = NULL;

    int rc = turbo_dc_parse_ltv(buf, sizeof(buf), &msg);
    check_equal(rc, 0);
    check_not_null(msg);

    /* Use accessor functions */
    check_equal(turbo_ltv_type(msg), 0x00);
    check_equal(turbo_ltv_value_len(msg), 0);

    turbo_free_ltv(&msg);
}

void test_parse_ltv_with_payload(void) {
    /* LTV: length=6, type=0x42, value="hello" */
    uint8_t buf[] = {0x06, 0x42, 'h', 'e', 'l', 'l', 'o'};
    ltv_message_t *msg = NULL;

    int rc = turbo_dc_parse_ltv(buf, sizeof(buf), &msg);
    check_equal(rc, 0);
    check_not_null(msg);

    check_equal(turbo_ltv_type(msg), 0x42);
    check_equal(turbo_ltv_value_len(msg), 5);
    check_equal(turbo_ltv_value(msg), "hello", 5);

    turbo_free_ltv(&msg);
}

/* ============================================================================
 * Stream Parser Tests
 * ============================================================================ */

void test_stream_create_destroy(void) {
    turbo_dc_msg_stream_t *stream = turbo_dc_msg_stream_create(0);
    check_not_null(stream);
    turbo_dc_msg_stream_destroy(stream);
}

void test_stream_destroy_null(void) {
    turbo_dc_msg_stream_destroy(NULL);  /* Should not crash */
}

void test_stream_feed_null(void) {
    ltv_message_t *msg = NULL;
    int rc = turbo_dc_msg_stream_feed(NULL, "data", 4, &msg);
    check_equal(rc, -1);
}

void test_stream_feed_complete(void) {
    turbo_dc_msg_stream_t *stream = turbo_dc_msg_stream_create(0);
    check_not_null(stream);

    /* LTV: length=5, type=0x33, value="test" */
    uint8_t buf[] = {0x05, 0x33, 't', 'e', 's', 't'};
    ltv_message_t *msg = NULL;

    int rc = turbo_dc_msg_stream_feed(stream, buf, sizeof(buf), &msg);
    check_equal(rc, 1);  /* Complete */
    check_not_null(msg);
    check_equal(turbo_ltv_type(msg), 0x33);
    check_equal(turbo_ltv_value_len(msg), 4);

    turbo_free_ltv(&msg);
    turbo_dc_msg_stream_destroy(stream);
}

void test_stream_feed_fragmented(void) {
    turbo_dc_msg_stream_t *stream = turbo_dc_msg_stream_create(0);
    check_not_null(stream);

    /* LTV: length=5, type=0x44, value="data" */
    uint8_t buf[] = {0x05, 0x44, 'd', 'a', 't', 'a'};
    ltv_message_t *msg = NULL;

    /* Feed first 3 bytes */
    int rc = turbo_dc_msg_stream_feed(stream, buf, 3, &msg);
    check_equal(rc, 0);  /* Need more */

    /* Feed remaining bytes */
    rc = turbo_dc_msg_stream_feed(stream, buf + 3, sizeof(buf) - 3, &msg);
    check_equal(rc, 1);  /* Complete */
    check_not_null(msg);
    check_equal(turbo_ltv_type(msg), 0x44);

    turbo_free_ltv(&msg);
    turbo_dc_msg_stream_destroy(stream);
}

void test_stream_reset(void) {
    turbo_dc_msg_stream_t *stream = turbo_dc_msg_stream_create(0);
    check_not_null(stream);

    uint8_t buf[] = {0x05, 0x01, 't', 'e', 's', 't'};
    ltv_message_t *msg = NULL;

    /* Feed partial */
    turbo_dc_msg_stream_feed(stream, buf, 2, &msg);

    /* Reset */
    turbo_dc_msg_stream_reset(stream);

    /* Feed complete message */
    int rc = turbo_dc_msg_stream_feed(stream, buf, sizeof(buf), &msg);
    check_equal(rc, 1);
    check_not_null(msg);

    turbo_free_ltv(&msg);
    turbo_dc_msg_stream_destroy(stream);
}

void test_stream_reset_null(void) {
    turbo_dc_msg_stream_reset(NULL);  /* Should not crash */
}

/* ============================================================================
 * Main
 * ============================================================================ */

spec("test_dc_msg") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

    /* is_ltv tests */
  it("test_is_ltv_null") { test_is_ltv_null(); };
  it("test_is_ltv_too_short") { test_is_ltv_too_short(); };
  it("test_is_ltv_empty_message") { test_is_ltv_empty_message(); };
  it("test_is_ltv_with_payload") { test_is_ltv_with_payload(); };
  it("test_is_ltv_truncated") { test_is_ltv_truncated(); };

    /* parse_ltv tests */
  it("test_parse_ltv_null_data") { test_parse_ltv_null_data(); };
  it("test_parse_ltv_null_output") { test_parse_ltv_null_output(); };
  it("test_parse_ltv_empty_message") { test_parse_ltv_empty_message(); };
  it("test_parse_ltv_with_payload") { test_parse_ltv_with_payload(); };

    /* Stream tests */
  it("test_stream_create_destroy") { test_stream_create_destroy(); };
  it("test_stream_destroy_null") { test_stream_destroy_null(); };
  it("test_stream_feed_null") { test_stream_feed_null(); };
  it("test_stream_feed_complete") { test_stream_feed_complete(); };
  it("test_stream_feed_fragmented") { test_stream_feed_fragmented(); };
  it("test_stream_reset") { test_stream_reset(); };
  it("test_stream_reset_null") { test_stream_reset_null(); };
}
