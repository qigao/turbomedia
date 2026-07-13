/**
 * test_datachannel.c - Unit tests for turbo_datachannel
 *
 * Tests the netcore-based WebRTC DataChannel implementation
 */

#include "turbo_datachannel.h"
#include "turbo_datachannel_errors.h"
#include "tinytest_compat.h"
#include <string.h>

void setUp(void) {
}

void tearDown(void) {
}

/* ============================================================================
 * Context Tests
 * ============================================================================ */

void test_context_create_null_config(void) {
    turbo_dc_context_t *ctx = turbo_dc_context_create(NULL);
    TEST_ASSERT_NULL(ctx);
}

void test_context_create_default(void) {
    /* Default config should work - async_client/server handle threading internally */
    turbo_dc_config_t config = {0};

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    TEST_ASSERT_NOT_NULL(ctx);

    turbo_dc_context_destroy(ctx);
}

void test_context_create_client_mode(void) {
    turbo_dc_config_t config = {
        .is_server = 0,
        .cert_pem = NULL,
        .key_pem = NULL
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    TEST_ASSERT_NOT_NULL(ctx);

    turbo_dc_context_destroy(ctx);
}

void test_context_create_server_mode(void) {
    turbo_dc_config_t config = {
        .is_server = 1,
        .cert_pem = NULL,
        .key_pem = NULL
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    TEST_ASSERT_NOT_NULL(ctx);

    turbo_dc_context_destroy(ctx);
}

void test_context_destroy_null(void) {
    /* Should not crash */
    turbo_dc_context_destroy(NULL);
}

/* ============================================================================
 * Peer Tests
 * ============================================================================ */

void test_peer_create_null_context(void) {
    turbo_dc_peer_t *peer = turbo_dc_peer_create(NULL, "127.0.0.1", 5000, NULL);
    TEST_ASSERT_NULL(peer);
}

void test_peer_create_client(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    TEST_ASSERT_NOT_NULL(ctx);

    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);
    TEST_ASSERT_NOT_NULL(peer);

    TEST_ASSERT_EQUAL(TURBO_DC_STATE_NEW, turbo_dc_peer_get_state(peer));

    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_peer_state_initial(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);

    TEST_ASSERT_EQUAL(TURBO_DC_STATE_NEW, turbo_dc_peer_get_state(peer));

    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_peer_get_state_null(void) {
    TEST_ASSERT_EQUAL(TURBO_DC_STATE_CLOSED, turbo_dc_peer_get_state(NULL));
}

void test_peer_callbacks_null_peer(void) {
    /* Should not crash */
    turbo_dc_peer_on_state(NULL, NULL);
    turbo_dc_peer_on_channel(NULL, NULL);
    turbo_dc_peer_on_error(NULL, NULL);
}

void test_peer_close_null(void) {
    /* Should not crash */
    turbo_dc_peer_close(NULL);
}

void test_peer_destroy_null(void) {
    /* Should not crash */
    turbo_dc_peer_destroy(NULL);
}

/* ============================================================================
 * Channel Tests
 * ============================================================================ */

void test_channel_create_null_peer(void) {
    turbo_dc_channel_t *channel = turbo_dc_channel_create(NULL, "test", NULL);
    TEST_ASSERT_NULL(channel);
}

void test_channel_create_null_label(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);

    turbo_dc_channel_t *channel = turbo_dc_channel_create(peer, NULL, NULL);
    TEST_ASSERT_NULL(channel);

    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_channel_create_with_label(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);

    turbo_dc_channel_t *channel = turbo_dc_channel_create(peer, "my-channel", NULL);
    TEST_ASSERT_NOT_NULL(channel);

    TEST_ASSERT_EQUAL_STRING("my-channel", turbo_dc_channel_get_label(channel));
    TEST_ASSERT_EQUAL(0, turbo_dc_channel_get_id(channel));
    TEST_ASSERT_FALSE(turbo_dc_channel_is_open(channel));
    TEST_ASSERT_EQUAL(0, turbo_dc_channel_buffered_amount(channel));

    turbo_dc_channel_close(channel);
    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_channel_create_multiple(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);

    turbo_dc_channel_t *ch1 = turbo_dc_channel_create(peer, "channel-1", NULL);
    turbo_dc_channel_t *ch2 = turbo_dc_channel_create(peer, "channel-2", NULL);
    turbo_dc_channel_t *ch3 = turbo_dc_channel_create(peer, "channel-3", NULL);

    TEST_ASSERT_NOT_NULL(ch1);
    TEST_ASSERT_NOT_NULL(ch2);
    TEST_ASSERT_NOT_NULL(ch3);

    {
        int ch1_id = (int)turbo_dc_channel_get_id(ch1);
        int ch2_id = (int)turbo_dc_channel_get_id(ch2);
        int ch3_id = (int)turbo_dc_channel_get_id(ch3);
        check_int_eq(ch1_id, 0);
        check_int_eq(ch2_id, 2);
        check_int_eq(ch3_id, 4);
    }

    turbo_dc_channel_close(ch1);
    turbo_dc_channel_close(ch2);
    turbo_dc_channel_close(ch3);
    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_channel_with_config(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);

    turbo_dc_channel_config_t ch_config = {
        .ordered = 0,
        .max_retransmits = 3,
        .max_lifetime_ms = 1000,
        .protocol = "my-protocol"
    };

    turbo_dc_channel_t *channel = turbo_dc_channel_create(peer, "config-test", &ch_config);
    TEST_ASSERT_NOT_NULL(channel);

    turbo_dc_channel_close(channel);
    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_channel_get_label_null(void) {
    TEST_ASSERT_NULL(turbo_dc_channel_get_label(NULL));
}

void test_channel_get_id_null(void) {
    TEST_ASSERT_EQUAL(0, turbo_dc_channel_get_id(NULL));
}

void test_channel_is_open_null(void) {
    TEST_ASSERT_FALSE(turbo_dc_channel_is_open(NULL));
}

void test_channel_buffered_amount_null(void) {
    TEST_ASSERT_EQUAL(0, turbo_dc_channel_buffered_amount(NULL));
}

void test_channel_close_null(void) {
    /* Should not crash */
    turbo_dc_channel_close(NULL);
}

void test_channel_callbacks_null(void) {
    /* Should not crash */
    turbo_dc_channel_on_open(NULL, NULL);
    turbo_dc_channel_on_message(NULL, NULL);
    turbo_dc_channel_on_close(NULL, NULL);
}

/* ============================================================================
 * Default Config Tests
 * ============================================================================ */

void test_default_channel_config(void) {
    turbo_dc_channel_config_t config = turbo_dc_default_channel_config();

    TEST_ASSERT_TRUE(config.ordered);
    TEST_ASSERT_EQUAL(0, config.max_retransmits);
    TEST_ASSERT_EQUAL(0, config.max_lifetime_ms);
    TEST_ASSERT_NULL(config.protocol);
}

/* ============================================================================
 * Error Handling Tests
 * ============================================================================ */

void test_error_string(void) {
    TEST_ASSERT_NOT_NULL(turbo_dc_error_string(TURBO_DC_ERROR_NONE));
    TEST_ASSERT_NOT_NULL(turbo_dc_error_string(TURBO_DC_ERROR_NULL_LOOP));
}

void test_channel_send_null_channel(void) {
    int result = turbo_dc_channel_send(NULL, "test", 4, 0);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_channel_send_null_data(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);
    turbo_dc_channel_t *channel = turbo_dc_channel_create(peer, "test", NULL);

    int result = turbo_dc_channel_send(channel, NULL, 0, 0);
    TEST_ASSERT_EQUAL(-1, result);

    turbo_dc_channel_close(channel);
    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_channel_send_not_open(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);
    turbo_dc_channel_t *channel = turbo_dc_channel_create(peer, "test", NULL);

    /* Channel not open yet */
    int result = turbo_dc_channel_send(channel, "test", 4, 0);
    TEST_ASSERT_EQUAL(-1, result);

    turbo_dc_channel_close(channel);
    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

/* ============================================================================
 * Main
 * ============================================================================ */

spec("test_datachannel") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

    /* Context tests */
  TT_TEST(test_context_create_null_config);
  TT_TEST(test_context_create_default);
  TT_TEST(test_context_create_client_mode);
  TT_TEST(test_context_create_server_mode);
  TT_TEST(test_context_destroy_null);

    /* Peer tests */
  TT_TEST(test_peer_create_null_context);
  TT_TEST(test_peer_create_client);
  TT_TEST(test_peer_state_initial);
  TT_TEST(test_peer_get_state_null);
  TT_TEST(test_peer_callbacks_null_peer);
  TT_TEST(test_peer_close_null);
  TT_TEST(test_peer_destroy_null);

    /* Channel tests */
  TT_TEST(test_channel_create_null_peer);
  TT_TEST(test_channel_create_null_label);
  TT_TEST(test_channel_create_with_label);
  TT_TEST(test_channel_create_multiple);
  TT_TEST(test_channel_with_config);
  TT_TEST(test_channel_get_label_null);
  TT_TEST(test_channel_get_id_null);
  TT_TEST(test_channel_is_open_null);
  TT_TEST(test_channel_buffered_amount_null);
  TT_TEST(test_channel_close_null);
  TT_TEST(test_channel_callbacks_null);

    /* Default config tests */
  TT_TEST(test_default_channel_config);

    /* Error handling tests */
  TT_TEST(test_error_string);
  TT_TEST(test_channel_send_null_channel);
  TT_TEST(test_channel_send_null_data);
  TT_TEST(test_channel_send_not_open);
}
