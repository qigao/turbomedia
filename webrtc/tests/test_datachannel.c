/**
 * test_datachannel.c - Unit tests for turbo_datachannel
 *
 * Tests the CoroNet-based WebRTC DataChannel implementation
 */

#include "turbo_datachannel.h"
#include "turbo_datachannel_errors.h"
#include "tinytest_compat.h"
#include <turbo_thread.h>
#include <string.h>

#define TEST_SHA256_FINGERPRINT \
    "00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:" \
    "10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F"

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

void test_peer_remote_fingerprint_validation(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };
    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer;

    TEST_ASSERT_NOT_NULL(ctx);
    peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);
    TEST_ASSERT_NOT_NULL(peer);

    TEST_ASSERT_EQUAL_INT(
        -1, turbo_dc_peer_set_remote_fingerprint(
                NULL, "sha-256", TEST_SHA256_FINGERPRINT));
    TEST_ASSERT_EQUAL_INT(
        -2, turbo_dc_peer_set_remote_fingerprint(
                peer, "sha-1", TEST_SHA256_FINGERPRINT));
    TEST_ASSERT_EQUAL_INT(
        -3, turbo_dc_peer_set_remote_fingerprint(
                peer, "sha-256", "AA:BB:CC:DD"));
    TEST_ASSERT_EQUAL_INT(
        0, turbo_dc_peer_set_remote_fingerprint(
               peer, "SHA-256", TEST_SHA256_FINGERPRINT));

    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_context_destroy_reclaims_live_peer(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };
    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer;

    TEST_ASSERT_NOT_NULL(ctx);
    peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);
    TEST_ASSERT_NOT_NULL(peer);

    /* The context is the owner: destroying it must close and free peers that
       were not explicitly destroyed by the caller. */
    turbo_dc_context_destroy(ctx);
}

typedef struct {
    turbo_mutex_t mutex;
    turbo_cond_t cond;
    int callback_entered;
    int release_callback;
    int detach_entered;
    int detach_completed;
    turbo_dc_peer_t *peer;
} transport_detach_test_context_t;

static void blocking_transport_data_callback(void *user_data, const uint8_t *data, size_t len) {
    transport_detach_test_context_t *context =
        (transport_detach_test_context_t *)user_data;
    (void)data;
    (void)len;

    turbo_mutex_lock(&context->mutex);
    context->callback_entered = 1;
    turbo_cond_broadcast(&context->cond);
    while (!context->release_callback) {
        turbo_cond_wait(&context->cond, &context->mutex);
    }
    turbo_mutex_unlock(&context->mutex);
}

static void feed_transport_data_thread(void *arg) {
    transport_detach_test_context_t *context =
        (transport_detach_test_context_t *)arg;
    static const uint8_t rtp_packet[12] = {
        0x80, 0x60, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02};

    turbo_dc_peer_feed_transport_data(context->peer, rtp_packet, sizeof(rtp_packet));
}

static void detach_transport_data_thread(void *arg) {
    transport_detach_test_context_t *context =
        (transport_detach_test_context_t *)arg;

    turbo_mutex_lock(&context->mutex);
    context->detach_entered = 1;
    turbo_cond_broadcast(&context->cond);
    turbo_mutex_unlock(&context->mutex);

    turbo_dc_peer_set_transport_data_handler(context->peer, NULL, NULL);

    turbo_mutex_lock(&context->mutex);
    context->detach_completed = 1;
    turbo_cond_broadcast(&context->cond);
    turbo_mutex_unlock(&context->mutex);
}

void test_transport_data_handler_detach_waits_for_callback(void) {
    turbo_dc_config_t config = {
        .is_server = 0,
        .transport = TURBO_DC_TRANSPORT_ICE,
    };
    transport_detach_test_context_t test_context = {0};
    turbo_dc_context_t *dc = turbo_dc_context_create(&config);
    turbo_thread_t feed_thread;
    turbo_thread_t detach_thread;

    TEST_ASSERT_NOT_NULL(dc);
    test_context.peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
    TEST_ASSERT_NOT_NULL(test_context.peer);
    turbo_mutex_init(&test_context.mutex);
    turbo_cond_init(&test_context.cond);
    turbo_dc_peer_set_transport_data_handler(
        test_context.peer, blocking_transport_data_callback, &test_context);

    TEST_ASSERT_EQUAL_INT(0, turbo_thread_create(
        &feed_thread, feed_transport_data_thread, &test_context));

    turbo_mutex_lock(&test_context.mutex);
    while (!test_context.callback_entered) {
        turbo_cond_wait(&test_context.cond, &test_context.mutex);
    }
    turbo_mutex_unlock(&test_context.mutex);

    TEST_ASSERT_EQUAL_INT(0, turbo_thread_create(
        &detach_thread, detach_transport_data_thread, &test_context));
    turbo_mutex_lock(&test_context.mutex);
    while (!test_context.detach_entered) {
        turbo_cond_wait(&test_context.cond, &test_context.mutex);
    }
    TEST_ASSERT_EQUAL_INT(0, test_context.detach_completed);
    test_context.release_callback = 1;
    turbo_cond_broadcast(&test_context.cond);
    turbo_mutex_unlock(&test_context.mutex);

    TEST_ASSERT_EQUAL_INT(0, turbo_thread_join(&feed_thread));
    TEST_ASSERT_EQUAL_INT(0, turbo_thread_join(&detach_thread));
    TEST_ASSERT_EQUAL_INT(1, test_context.detach_completed);

    turbo_cond_destroy(&test_context.cond);
    turbo_mutex_destroy(&test_context.mutex);
    turbo_dc_peer_destroy(test_context.peer);
    turbo_dc_context_destroy(dc);
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
        .max_lifetime_ms = 0,
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
  TT_TEST(test_context_destroy_reclaims_live_peer);
  TT_TEST(test_peer_state_initial);
  TT_TEST(test_peer_get_state_null);
  TT_TEST(test_peer_callbacks_null_peer);
  TT_TEST(test_peer_close_null);
  TT_TEST(test_peer_destroy_null);
  TT_TEST(test_peer_remote_fingerprint_validation);
  TT_TEST(test_transport_data_handler_detach_waits_for_callback);

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
