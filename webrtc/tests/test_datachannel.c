/**
 * test_datachannel.c - Unit tests for turbo_datachannel
 *
 * Tests the CNet-based WebRTC DataChannel implementation
 */

#include "turbo_datachannel.h"
#include "turbo_datachannel_errors.h"
#include "tinytest.h"
#include <salts_thread.h>
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
    check_null(ctx);
}

void test_context_create_default(void) {
    /* Default config should work - async_client/server handle threading internally */
    turbo_dc_config_t config = {0};

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    check_not_null(ctx);

    turbo_dc_context_destroy(ctx);
}

void test_context_create_client_mode(void) {
    turbo_dc_config_t config = {
        .is_server = 0,
        .cert_pem = NULL,
        .key_pem = NULL
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    check_not_null(ctx);

    turbo_dc_context_destroy(ctx);
}

void test_context_create_server_mode(void) {
    turbo_dc_config_t config = {
        .is_server = 1,
        .cert_pem = NULL,
        .key_pem = NULL
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    check_not_null(ctx);

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
    check_null(peer);
}

void test_peer_create_client(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    check_not_null(ctx);

    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);
    check_not_null(peer);

    check_equal(turbo_dc_peer_get_state(peer), TURBO_DC_STATE_NEW);

    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_peer_state_initial(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);

    check_equal(turbo_dc_peer_get_state(peer), TURBO_DC_STATE_NEW);

    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_peer_get_state_null(void) {
    check_equal(turbo_dc_peer_get_state(NULL), TURBO_DC_STATE_CLOSED);
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

    check_not_null(ctx);
    peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);
    check_not_null(peer);

    check_equal((int)(turbo_dc_peer_set_remote_fingerprint(
                NULL, "sha-256", TEST_SHA256_FINGERPRINT)), (int)(-1));
    check_equal((int)(turbo_dc_peer_set_remote_fingerprint(
                peer, "sha-1", TEST_SHA256_FINGERPRINT)), (int)(-2));
    check_equal((int)(turbo_dc_peer_set_remote_fingerprint(
                peer, "sha-256", "AA:BB:CC:DD")), (int)(-3));
    check_equal((int)(turbo_dc_peer_set_remote_fingerprint(
               peer, "SHA-256", TEST_SHA256_FINGERPRINT)), (int)(0));

    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_context_destroy_reclaims_live_peer(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };
    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer;

    check_not_null(ctx);
    peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);
    check_not_null(peer);

    /* The context is the owner: destroying it must close and free peers that
       were not explicitly destroyed by the caller. */
    turbo_dc_context_destroy(ctx);
}

typedef struct {
    salts_mutex_t mutex;
    salts_cond_t cond;
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

    salts_mutex_lock(&context->mutex);
    context->callback_entered = 1;
    salts_cond_broadcast(&context->cond);
    while (!context->release_callback) {
        salts_cond_wait(&context->cond, &context->mutex);
    }
    salts_mutex_unlock(&context->mutex);
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

    salts_mutex_lock(&context->mutex);
    context->detach_entered = 1;
    salts_cond_broadcast(&context->cond);
    salts_mutex_unlock(&context->mutex);

    turbo_dc_peer_set_transport_data_handler(context->peer, NULL, NULL);

    salts_mutex_lock(&context->mutex);
    context->detach_completed = 1;
    salts_cond_broadcast(&context->cond);
    salts_mutex_unlock(&context->mutex);
}

void test_transport_data_handler_detach_waits_for_callback(void) {
    turbo_dc_config_t config = {
        .is_server = 0,
        .transport = TURBO_DC_TRANSPORT_ICE,
    };
    transport_detach_test_context_t test_context = {0};
    turbo_dc_context_t *dc = turbo_dc_context_create(&config);
    salts_thread_t feed_thread;
    salts_thread_t detach_thread;

    check_not_null(dc);
    test_context.peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
    check_not_null(test_context.peer);
    salts_mutex_init(&test_context.mutex);
    salts_cond_init(&test_context.cond);
    turbo_dc_peer_set_transport_data_handler(
        test_context.peer, blocking_transport_data_callback, &test_context);

    check_equal((int)(salts_thread_create(
        &feed_thread, feed_transport_data_thread, &test_context)), (int)(0));

    salts_mutex_lock(&test_context.mutex);
    while (!test_context.callback_entered) {
        salts_cond_wait(&test_context.cond, &test_context.mutex);
    }
    salts_mutex_unlock(&test_context.mutex);

    check_equal((int)(salts_thread_create(
        &detach_thread, detach_transport_data_thread, &test_context)), (int)(0));
    salts_mutex_lock(&test_context.mutex);
    while (!test_context.detach_entered) {
        salts_cond_wait(&test_context.cond, &test_context.mutex);
    }
    check_equal((int)(test_context.detach_completed), (int)(0));
    test_context.release_callback = 1;
    salts_cond_broadcast(&test_context.cond);
    salts_mutex_unlock(&test_context.mutex);

    check_equal((int)(salts_thread_join(&feed_thread)), (int)(0));
    check_equal((int)(salts_thread_join(&detach_thread)), (int)(0));
    check_equal((int)(test_context.detach_completed), (int)(1));

    salts_cond_destroy(&test_context.cond);
    salts_mutex_destroy(&test_context.mutex);
    turbo_dc_peer_destroy(test_context.peer);
    turbo_dc_context_destroy(dc);
}

/* ============================================================================
 * Channel Tests
 * ============================================================================ */

void test_channel_create_null_peer(void) {
    turbo_dc_channel_t *channel = turbo_dc_channel_create(NULL, "test", NULL);
    check_null(channel);
}

void test_channel_create_null_label(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);

    turbo_dc_channel_t *channel = turbo_dc_channel_create(peer, NULL, NULL);
    check_null(channel);

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
    check_not_null(channel);

    check_equal(turbo_dc_channel_get_label(channel), "my-channel");
    check_equal(turbo_dc_channel_get_id(channel), 0);
    check_false(turbo_dc_channel_is_open(channel));
    check_equal(turbo_dc_channel_buffered_amount(channel), 0);

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

    check_not_null(ch1);
    check_not_null(ch2);
    check_not_null(ch3);

    {
        int ch1_id = (int)turbo_dc_channel_get_id(ch1);
        int ch2_id = (int)turbo_dc_channel_get_id(ch2);
        int ch3_id = (int)turbo_dc_channel_get_id(ch3);
        check_equal(ch1_id, 0);
        check_equal(ch2_id, 2);
        check_equal(ch3_id, 4);
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
    check_not_null(channel);

    turbo_dc_channel_close(channel);
    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(ctx);
}

void test_channel_get_label_null(void) {
    check_null(turbo_dc_channel_get_label(NULL));
}

void test_channel_get_id_null(void) {
    check_equal(turbo_dc_channel_get_id(NULL), 0);
}

void test_channel_is_open_null(void) {
    check_false(turbo_dc_channel_is_open(NULL));
}

void test_channel_buffered_amount_null(void) {
    check_equal(turbo_dc_channel_buffered_amount(NULL), 0);
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

    check_true(config.ordered);
    check_equal(config.max_retransmits, 0);
    check_equal(config.max_lifetime_ms, 0);
    check_null(config.protocol);
}

/* ============================================================================
 * Error Handling Tests
 * ============================================================================ */

void test_error_string(void) {
    check_not_null(turbo_dc_error_string(TURBO_DC_ERROR_NONE));
    check_not_null(turbo_dc_error_string(TURBO_DC_ERROR_NULL_LOOP));
}

void test_channel_send_null_channel(void) {
    int result = turbo_dc_channel_send(NULL, "test", 4, 0);
    check_equal(result, -1);
}

void test_channel_send_null_data(void) {
    turbo_dc_config_t config = {
        .is_server = 0
    };

    turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
    turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);
    turbo_dc_channel_t *channel = turbo_dc_channel_create(peer, "test", NULL);

    int result = turbo_dc_channel_send(channel, NULL, 0, 0);
    check_equal(result, -1);

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
    check_equal(result, -1);

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
  it("test_context_create_null_config") { test_context_create_null_config(); };
  it("test_context_create_default") { test_context_create_default(); };
  it("test_context_create_client_mode") { test_context_create_client_mode(); };
  it("test_context_create_server_mode") { test_context_create_server_mode(); };
  it("test_context_destroy_null") { test_context_destroy_null(); };

    /* Peer tests */
  it("test_peer_create_null_context") { test_peer_create_null_context(); };
  it("test_peer_create_client") { test_peer_create_client(); };
  it("test_context_destroy_reclaims_live_peer") { test_context_destroy_reclaims_live_peer(); };
  it("test_peer_state_initial") { test_peer_state_initial(); };
  it("test_peer_get_state_null") { test_peer_get_state_null(); };
  it("test_peer_callbacks_null_peer") { test_peer_callbacks_null_peer(); };
  it("test_peer_close_null") { test_peer_close_null(); };
  it("test_peer_destroy_null") { test_peer_destroy_null(); };
  it("test_peer_remote_fingerprint_validation") { test_peer_remote_fingerprint_validation(); };
  it("test_transport_data_handler_detach_waits_for_callback") { test_transport_data_handler_detach_waits_for_callback(); };

    /* Channel tests */
  it("test_channel_create_null_peer") { test_channel_create_null_peer(); };
  it("test_channel_create_null_label") { test_channel_create_null_label(); };
  it("test_channel_create_with_label") { test_channel_create_with_label(); };
  it("test_channel_create_multiple") { test_channel_create_multiple(); };
  it("test_channel_with_config") { test_channel_with_config(); };
  it("test_channel_get_label_null") { test_channel_get_label_null(); };
  it("test_channel_get_id_null") { test_channel_get_id_null(); };
  it("test_channel_is_open_null") { test_channel_is_open_null(); };
  it("test_channel_buffered_amount_null") { test_channel_buffered_amount_null(); };
  it("test_channel_close_null") { test_channel_close_null(); };
  it("test_channel_callbacks_null") { test_channel_callbacks_null(); };

    /* Default config tests */
  it("test_default_channel_config") { test_default_channel_config(); };

    /* Error handling tests */
  it("test_error_string") { test_error_string(); };
  it("test_channel_send_null_channel") { test_channel_send_null_channel(); };
  it("test_channel_send_null_data") { test_channel_send_null_data(); };
  it("test_channel_send_not_open") { test_channel_send_not_open(); };
}
