/**
 * Unit tests for Simulcast
 */
#include "tinytest_compat.h"
#include "turbo_codec.h"
#include "turbo_rtp.h"
#include "turbo_simulcast.h"
#include <string.h>

typedef struct {
  int packet_count;
  size_t total_bytes;
  simulcast_layer_t last_layer;
  rtp_packet_t last_packet;
  uint8_t last_packet_buf[RTP_MAX_PACKET];
} simulcast_packet_capture_t;

static void on_simulcast_packet(void *user_data, simulcast_layer_t layer, const uint8_t *packet,
                                size_t len) {
  simulcast_packet_capture_t *capture = (simulcast_packet_capture_t *)user_data;
  if (!capture || !packet || len > sizeof(capture->last_packet_buf)) {
    return;
  }

  capture->packet_count++;
  capture->total_bytes += len;
  capture->last_layer = layer;
  memcpy(capture->last_packet_buf, packet, len);
  TEST_ASSERT_EQUAL_INT(0, rtp_packet_parse(&capture->last_packet, capture->last_packet_buf, len));
}

void setUp(void) { turbo_codec_registry_init(); }

void tearDown(void) { turbo_codec_registry_shutdown(); }

/* =============================================================================
 * Simulcast Creation Tests
 * ============================================================================= */

void test_simulcast_create(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    TEST_IGNORE_MESSAGE("VP8 codec not available");
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  TEST_ASSERT_NOT_NULL(ctx);

  turbo_simulcast_destroy(ctx);
}

void test_simulcast_create_invalid_params(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    TEST_IGNORE_MESSAGE("VP8 codec not available");
    return;
  }

  /* Invalid dimensions */
  simulcast_ctx_t *ctx1 = turbo_simulcast_create(0, 720, 30, codec);
  TEST_ASSERT_NULL(ctx1);

  /* Invalid framerate */
  simulcast_ctx_t *ctx2 = turbo_simulcast_create(1280, 720, 0, codec);
  TEST_ASSERT_NULL(ctx2);

  /* NULL codec */
  simulcast_ctx_t *ctx3 = turbo_simulcast_create(1280, 720, 30, NULL);
  TEST_ASSERT_NULL(ctx3);
}

/* =============================================================================
 * Layer Management Tests
 * ============================================================================= */

void test_simulcast_enable_disable_layers(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    TEST_IGNORE_MESSAGE("VP8 codec not available");
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  TEST_ASSERT_NOT_NULL(ctx);

  /* Enable all layers */
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_LOW, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_MEDIUM, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_HIGH, 1);

  /* Disable medium layer */
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_MEDIUM, 0);

  turbo_simulcast_destroy(ctx);
}

void test_simulcast_layer_stats(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    TEST_IGNORE_MESSAGE("VP8 codec not available");
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  TEST_ASSERT_NOT_NULL(ctx);

  int width, height, bitrate, frames_encoded;
  int64_t bytes_sent;

  /* Get low layer stats */
  turbo_simulcast_get_layer_stats(ctx, SIMULCAST_LAYER_LOW, &width, &height, &bitrate,
                                  &frames_encoded, &bytes_sent);
  TEST_ASSERT_GREATER_THAN(0, width);
  TEST_ASSERT_GREATER_THAN(0, height);

  /* Get high layer stats */
  turbo_simulcast_get_layer_stats(ctx, SIMULCAST_LAYER_HIGH, &width, &height, &bitrate,
                                  &frames_encoded, &bytes_sent);
  TEST_ASSERT_GREATER_THAN(0, width);
  TEST_ASSERT_GREATER_THAN(0, height);

  turbo_simulcast_destroy(ctx);
}

/* =============================================================================
 * Bandwidth Adaptation Tests
 * ============================================================================= */

void test_simulcast_set_bandwidth(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    TEST_IGNORE_MESSAGE("VP8 codec not available");
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  TEST_ASSERT_NOT_NULL(ctx);

  /* Set different bandwidth levels */
  turbo_simulcast_set_bandwidth(ctx, 200000);  /* Low */
  turbo_simulcast_set_bandwidth(ctx, 600000);  /* Medium */
  turbo_simulcast_set_bandwidth(ctx, 2000000); /* High */

  turbo_simulcast_destroy(ctx);
}

void test_simulcast_bandwidth_layer_selection(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    TEST_IGNORE_MESSAGE("VP8 codec not available");
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  TEST_ASSERT_NOT_NULL(ctx);

  /* Enable all layers first */
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_LOW, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_MEDIUM, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_HIGH, 1);

  /* Verify layers are enabled */
  int low_enabled = turbo_simulcast_is_layer_active(ctx, SIMULCAST_LAYER_LOW);
  int medium_enabled = turbo_simulcast_is_layer_active(ctx, SIMULCAST_LAYER_MEDIUM);
  int high_enabled = turbo_simulcast_is_layer_active(ctx, SIMULCAST_LAYER_HIGH);

  /* At least one layer should be active after enabling */
  TEST_ASSERT_TRUE(low_enabled || medium_enabled || high_enabled);

  /* Test bandwidth setting (doesn't guarantee specific layer activation) */
  turbo_simulcast_set_bandwidth(ctx, 200000);
  turbo_simulcast_set_bandwidth(ctx, 2000000);

  turbo_simulcast_destroy(ctx);
}

/* =============================================================================
 * Encoding Tests
 * ============================================================================= */

void test_simulcast_encode_frame(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    TEST_IGNORE_MESSAGE("VP8 codec not available");
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(640, 480, 30, codec);
  TEST_ASSERT_NOT_NULL(ctx);

  /* Enable all layers */
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_LOW, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_MEDIUM, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_HIGH, 1);

  /* Create dummy frame (I420 format) */
  size_t frame_size = 640 * 480 * 3 / 2;
  uint8_t *frame_data = (uint8_t *)calloc(1, frame_size);
  TEST_ASSERT_NOT_NULL(frame_data);

  /* Encode frame */
  int result = turbo_simulcast_encode_frame(ctx, frame_data, frame_size, 0);
  TEST_ASSERT_EQUAL_INT(0, result);

  free(frame_data);
  turbo_simulcast_destroy(ctx);
}

/* =============================================================================
 * Statistics Tests
 * ============================================================================= */

void test_simulcast_layer_stats_after_encode(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    TEST_IGNORE_MESSAGE("VP8 codec not available");
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(640, 480, 30, codec);
  TEST_ASSERT_NOT_NULL(ctx);

  /* Enable one layer */
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_MEDIUM, 1);

  /* Create and encode a dummy frame */
  size_t frame_size = 640 * 480 * 3 / 2;
  uint8_t *frame_data = (uint8_t *)calloc(1, frame_size);
  TEST_ASSERT_NOT_NULL(frame_data);

  turbo_simulcast_encode_frame(ctx, frame_data, frame_size, 0);

  /* Get stats */
  int width, height, bitrate, frames_encoded;
  int64_t bytes_sent;
  turbo_simulcast_get_layer_stats(ctx, SIMULCAST_LAYER_MEDIUM, &width, &height, &bitrate,
                                  &frames_encoded, &bytes_sent);

  TEST_ASSERT_GREATER_THAN(0, width);
  TEST_ASSERT_GREATER_THAN(0, height);

  free(frame_data);
  turbo_simulcast_destroy(ctx);
}

void test_simulcast_callback_emits_rtp_packets(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  simulcast_packet_capture_t capture = {0};
  if (!codec) {
    TEST_IGNORE_MESSAGE("VP8 codec not available");
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(640, 480, 30, codec);
  TEST_ASSERT_NOT_NULL(ctx);

  turbo_simulcast_set_rtp_callback(ctx, on_simulcast_packet, &capture);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_HIGH, 1);
  turbo_simulcast_set_layer_ssrc(ctx, SIMULCAST_LAYER_HIGH, 0x55667788);

  size_t frame_size = 640 * 480 * 3 / 2;
  uint8_t *frame_data = (uint8_t *)calloc(1, frame_size);
  TEST_ASSERT_NOT_NULL(frame_data);

  TEST_ASSERT_GREATER_OR_EQUAL(1, turbo_simulcast_encode_frame(ctx, frame_data, frame_size, 33333));
  TEST_ASSERT_GREATER_THAN(0, capture.packet_count);
  TEST_ASSERT_EQUAL_INT(SIMULCAST_LAYER_HIGH, capture.last_layer);
  TEST_ASSERT_EQUAL_UINT8(RTP_VERSION, capture.last_packet.header.version);
  TEST_ASSERT_EQUAL_UINT8(codec->payload_type, capture.last_packet.header.payload_type);
  TEST_ASSERT_EQUAL_UINT32(0x55667788, capture.last_packet.header.ssrc);
  TEST_ASSERT_GREATER_THAN(0, capture.last_packet.payload_len);

  int64_t bytes_sent = 0;
  turbo_simulcast_get_layer_stats(ctx, SIMULCAST_LAYER_HIGH, NULL, NULL, NULL, NULL, &bytes_sent);
  TEST_ASSERT_GREATER_THAN(0, bytes_sent);
  TEST_ASSERT_GREATER_OR_EQUAL(capture.total_bytes, (size_t)bytes_sent);

  free(frame_data);
  turbo_simulcast_destroy(ctx);
}

void test_simulcast_layer_switching(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    TEST_IGNORE_MESSAGE("VP8 codec not available");
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  TEST_ASSERT_NOT_NULL(ctx);

  /* Enable all layers */
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_LOW, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_MEDIUM, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_HIGH, 1);

  /* Start with high bandwidth */
  turbo_simulcast_set_bandwidth(ctx, 2000000);
  int high_active = turbo_simulcast_is_layer_active(ctx, SIMULCAST_LAYER_HIGH);

  /* Drop to low bandwidth */
  turbo_simulcast_set_bandwidth(ctx, 200000);
  int low_active = turbo_simulcast_is_layer_active(ctx, SIMULCAST_LAYER_LOW);

  /* At least one should be active */
  TEST_ASSERT_TRUE(high_active || low_active);

  turbo_simulcast_destroy(ctx);
}

/* =============================================================================
 * Main
 * ============================================================================= */

spec("test_simulcast") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  /* Creation */
  TT_TEST(test_simulcast_create);
  TT_TEST(test_simulcast_create_invalid_params);

  /* Layer Management */
  TT_TEST(test_simulcast_enable_disable_layers);
  TT_TEST(test_simulcast_layer_stats);

  /* Bandwidth Adaptation */
  TT_TEST(test_simulcast_set_bandwidth);
  TT_TEST(test_simulcast_bandwidth_layer_selection);

  /* Encoding */
  TT_TEST(test_simulcast_encode_frame);
  TT_TEST(test_simulcast_callback_emits_rtp_packets);

  /* Statistics */
  TT_TEST(test_simulcast_layer_stats_after_encode);
  TT_TEST(test_simulcast_layer_switching);
}
