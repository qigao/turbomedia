/**
 * test_dc_msg.c - Unit tests for WebRTC DataChannel LTV messaging
 *
 * Tests the turbo_dc_msg API (wrapper around turbo_ltv_*)
 */

#include "turbo_dc_msg.h"
#include "tinytest_compat.h"
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
    TEST_ASSERT_EQUAL(0, result);
}

void test_is_ltv_too_short(void) {
    uint8_t buf[] = {0x01};
    int result = turbo_dc_is_ltv(buf, 1);
    TEST_ASSERT_EQUAL(0, result);
}

void test_is_ltv_empty_message(void) {
    /* LTV: length=1 (varint 0x01), type=0x00, no value */
    uint8_t buf[] = {0x01, 0x00};
    int result = turbo_dc_is_ltv(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(1, result);
}

void test_is_ltv_with_payload(void) {
    /* LTV: length=6 (varint 0x06), type=0x42, value="hello" */
    uint8_t buf[] = {0x06, 0x42, 'h', 'e', 'l', 'l', 'o'};
    int result = turbo_dc_is_ltv(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(1, result);
}

void test_is_ltv_truncated(void) {
    /* LTV header says 6 bytes but only 3 provided */
    uint8_t buf[] = {0x06, 0x42, 'h'};
    int result = turbo_dc_is_ltv(buf, sizeof(buf));
    TEST_ASSERT_EQUAL(0, result);
}

/* ============================================================================
 * turbo_dc_parse_ltv Tests
 * ============================================================================ */

void test_parse_ltv_null_data(void) {
    ltv_message_t *msg = NULL;
    int rc = turbo_dc_parse_ltv(NULL, 0, &msg);
    TEST_ASSERT_EQUAL(-1, rc);
}

void test_parse_ltv_null_output(void) {
    uint8_t buf[] = {0x01, 0x00};
    int rc = turbo_dc_parse_ltv(buf, sizeof(buf), NULL);
    TEST_ASSERT_EQUAL(-1, rc);
}

void test_parse_ltv_empty_message(void) {
    /* LTV: length=1, type=0x00, no value */
    uint8_t buf[] = {0x01, 0x00};
    ltv_message_t *msg = NULL;

    int rc = turbo_dc_parse_ltv(buf, sizeof(buf), &msg);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_NOT_NULL(msg);

    /* Use accessor functions */
    TEST_ASSERT_EQUAL(0x00, turbo_ltv_type(msg));
    TEST_ASSERT_EQUAL(0, turbo_ltv_value_len(msg));

    free(msg);
}

void test_parse_ltv_with_payload(void) {
    /* LTV: length=6, type=0x42, value="hello" */
    uint8_t buf[] = {0x06, 0x42, 'h', 'e', 'l', 'l', 'o'};
    ltv_message_t *msg = NULL;

    int rc = turbo_dc_parse_ltv(buf, sizeof(buf), &msg);
    TEST_ASSERT_EQUAL(0, rc);
    TEST_ASSERT_NOT_NULL(msg);

    TEST_ASSERT_EQUAL(0x42, turbo_ltv_type(msg));
    TEST_ASSERT_EQUAL(5, turbo_ltv_value_len(msg));
    TEST_ASSERT_EQUAL_MEMORY("hello", turbo_ltv_value(msg), 5);

    free(msg);
}

/* ============================================================================
 * Stream Parser Tests
 * ============================================================================ */

void test_stream_create_destroy(void) {
    turbo_dc_msg_stream_t *stream = turbo_dc_msg_stream_create(0);
    TEST_ASSERT_NOT_NULL(stream);
    turbo_dc_msg_stream_destroy(stream);
}

void test_stream_destroy_null(void) {
    turbo_dc_msg_stream_destroy(NULL);  /* Should not crash */
}

void test_stream_feed_null(void) {
    ltv_message_t *msg = NULL;
    int rc = turbo_dc_msg_stream_feed(NULL, "data", 4, &msg);
    TEST_ASSERT_EQUAL(-1, rc);
}

void test_stream_feed_complete(void) {
    turbo_dc_msg_stream_t *stream = turbo_dc_msg_stream_create(0);
    TEST_ASSERT_NOT_NULL(stream);

    /* LTV: length=5, type=0x33, value="test" */
    uint8_t buf[] = {0x05, 0x33, 't', 'e', 's', 't'};
    ltv_message_t *msg = NULL;

    int rc = turbo_dc_msg_stream_feed(stream, buf, sizeof(buf), &msg);
    TEST_ASSERT_EQUAL(1, rc);  /* Complete */
    TEST_ASSERT_NOT_NULL(msg);
    TEST_ASSERT_EQUAL(0x33, turbo_ltv_type(msg));
    TEST_ASSERT_EQUAL(4, turbo_ltv_value_len(msg));

    free(msg);
    turbo_dc_msg_stream_destroy(stream);
}

void test_stream_feed_fragmented(void) {
    turbo_dc_msg_stream_t *stream = turbo_dc_msg_stream_create(0);
    TEST_ASSERT_NOT_NULL(stream);

    /* LTV: length=5, type=0x44, value="data" */
    uint8_t buf[] = {0x05, 0x44, 'd', 'a', 't', 'a'};
    ltv_message_t *msg = NULL;

    /* Feed first 3 bytes */
    int rc = turbo_dc_msg_stream_feed(stream, buf, 3, &msg);
    TEST_ASSERT_EQUAL(0, rc);  /* Need more */

    /* Feed remaining bytes */
    rc = turbo_dc_msg_stream_feed(stream, buf + 3, sizeof(buf) - 3, &msg);
    TEST_ASSERT_EQUAL(1, rc);  /* Complete */
    TEST_ASSERT_NOT_NULL(msg);
    TEST_ASSERT_EQUAL(0x44, turbo_ltv_type(msg));

    free(msg);
    turbo_dc_msg_stream_destroy(stream);
}

void test_stream_reset(void) {
    turbo_dc_msg_stream_t *stream = turbo_dc_msg_stream_create(0);
    TEST_ASSERT_NOT_NULL(stream);

    uint8_t buf[] = {0x05, 0x01, 't', 'e', 's', 't'};
    ltv_message_t *msg = NULL;

    /* Feed partial */
    turbo_dc_msg_stream_feed(stream, buf, 2, &msg);

    /* Reset */
    turbo_dc_msg_stream_reset(stream);

    /* Feed complete message */
    int rc = turbo_dc_msg_stream_feed(stream, buf, sizeof(buf), &msg);
    TEST_ASSERT_EQUAL(1, rc);
    TEST_ASSERT_NOT_NULL(msg);

    free(msg);
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
  TT_TEST(test_is_ltv_null);
  TT_TEST(test_is_ltv_too_short);
  TT_TEST(test_is_ltv_empty_message);
  TT_TEST(test_is_ltv_with_payload);
  TT_TEST(test_is_ltv_truncated);

    /* parse_ltv tests */
  TT_TEST(test_parse_ltv_null_data);
  TT_TEST(test_parse_ltv_null_output);
  TT_TEST(test_parse_ltv_empty_message);
  TT_TEST(test_parse_ltv_with_payload);

    /* Stream tests */
  TT_TEST(test_stream_create_destroy);
  TT_TEST(test_stream_destroy_null);
  TT_TEST(test_stream_feed_null);
  TT_TEST(test_stream_feed_complete);
  TT_TEST(test_stream_feed_fragmented);
  TT_TEST(test_stream_reset);
  TT_TEST(test_stream_reset_null);
}
