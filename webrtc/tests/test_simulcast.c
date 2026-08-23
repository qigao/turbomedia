/**
 * Unit tests for Simulcast
 */
#include "tinytest.h"
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
  check_equal((int)(rtp_packet_parse(&capture->last_packet, capture->last_packet_buf, len)), (int)(0));
}

void setUp(void) { turbo_codec_registry_init(); }

void tearDown(void) { turbo_codec_registry_shutdown(); }

/* =============================================================================
 * Simulcast Creation Tests
 * ============================================================================= */

void test_simulcast_create(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    check(0, "%s", ("VP8 codec not available"));
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  check_not_null(ctx);

  turbo_simulcast_destroy(ctx);
}

void test_simulcast_create_invalid_params(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    check(0, "%s", ("VP8 codec not available"));
    return;
  }

  /* Invalid dimensions */
  simulcast_ctx_t *ctx1 = turbo_simulcast_create(0, 720, 30, codec);
  check_null(ctx1);

  /* Invalid framerate */
  simulcast_ctx_t *ctx2 = turbo_simulcast_create(1280, 720, 0, codec);
  check_null(ctx2);

  /* NULL codec */
  simulcast_ctx_t *ctx3 = turbo_simulcast_create(1280, 720, 30, NULL);
  check_null(ctx3);
}

/* =============================================================================
 * Layer Management Tests
 * ============================================================================= */

void test_simulcast_enable_disable_layers(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    check(0, "%s", ("VP8 codec not available"));
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  check_not_null(ctx);

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
    check(0, "%s", ("VP8 codec not available"));
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  check_not_null(ctx);

  int width, height, bitrate, frames_encoded;
  int64_t bytes_sent;

  /* Get low layer stats */
  turbo_simulcast_get_layer_stats(ctx, SIMULCAST_LAYER_LOW, &width, &height, &bitrate,
                                  &frames_encoded, &bytes_sent);
  check_greater(width, 0);
  check_greater(height, 0);

  /* Get high layer stats */
  turbo_simulcast_get_layer_stats(ctx, SIMULCAST_LAYER_HIGH, &width, &height, &bitrate,
                                  &frames_encoded, &bytes_sent);
  check_greater(width, 0);
  check_greater(height, 0);

  turbo_simulcast_destroy(ctx);
}

/* =============================================================================
 * Bandwidth Adaptation Tests
 * ============================================================================= */

void test_simulcast_set_bandwidth(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    check(0, "%s", ("VP8 codec not available"));
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  check_not_null(ctx);

  /* Set different bandwidth levels */
  turbo_simulcast_set_bandwidth(ctx, 200000);  /* Low */
  turbo_simulcast_set_bandwidth(ctx, 600000);  /* Medium */
  turbo_simulcast_set_bandwidth(ctx, 2000000); /* High */

  turbo_simulcast_destroy(ctx);
}

void test_simulcast_bandwidth_layer_selection(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    check(0, "%s", ("VP8 codec not available"));
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  check_not_null(ctx);

  /* Enable all layers first */
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_LOW, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_MEDIUM, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_HIGH, 1);

  /* Verify layers are enabled */
  int low_enabled = turbo_simulcast_is_layer_active(ctx, SIMULCAST_LAYER_LOW);
  int medium_enabled = turbo_simulcast_is_layer_active(ctx, SIMULCAST_LAYER_MEDIUM);
  int high_enabled = turbo_simulcast_is_layer_active(ctx, SIMULCAST_LAYER_HIGH);

  /* At least one layer should be active after enabling */
  check_true(low_enabled || medium_enabled || high_enabled);

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
    check(0, "%s", ("VP8 codec not available"));
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(640, 480, 30, codec);
  check_not_null(ctx);

  /* Enable all layers */
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_LOW, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_MEDIUM, 1);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_HIGH, 1);

  /* Create dummy frame (I420 format) */
  size_t frame_size = 640 * 480 * 3 / 2;
  uint8_t *frame_data = (uint8_t *)calloc(1, frame_size);
  check_not_null(frame_data);

  /* Encode frame */
  int result = turbo_simulcast_encode_frame(ctx, frame_data, frame_size, 0);
  check_equal((int)(result), (int)(0));

  free(frame_data);
  turbo_simulcast_destroy(ctx);
}

/* =============================================================================
 * Statistics Tests
 * ============================================================================= */

void test_simulcast_layer_stats_after_encode(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    check(0, "%s", ("VP8 codec not available"));
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(640, 480, 30, codec);
  check_not_null(ctx);

  /* Enable one layer */
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_MEDIUM, 1);

  /* Create and encode a dummy frame */
  size_t frame_size = 640 * 480 * 3 / 2;
  uint8_t *frame_data = (uint8_t *)calloc(1, frame_size);
  check_not_null(frame_data);

  turbo_simulcast_encode_frame(ctx, frame_data, frame_size, 0);

  /* Get stats */
  int width, height, bitrate, frames_encoded;
  int64_t bytes_sent;
  turbo_simulcast_get_layer_stats(ctx, SIMULCAST_LAYER_MEDIUM, &width, &height, &bitrate,
                                  &frames_encoded, &bytes_sent);

  check_greater(width, 0);
  check_greater(height, 0);

  free(frame_data);
  turbo_simulcast_destroy(ctx);
}

void test_simulcast_callback_emits_rtp_packets(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  simulcast_packet_capture_t capture = {0};
  if (!codec) {
    check(0, "%s", ("VP8 codec not available"));
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(640, 480, 30, codec);
  check_not_null(ctx);

  turbo_simulcast_set_rtp_callback(ctx, on_simulcast_packet, &capture);
  turbo_simulcast_enable_layer(ctx, SIMULCAST_LAYER_HIGH, 1);
  turbo_simulcast_set_layer_ssrc(ctx, SIMULCAST_LAYER_HIGH, 0x55667788);

  size_t frame_size = 640 * 480 * 3 / 2;
  uint8_t *frame_data = (uint8_t *)calloc(1, frame_size);
  check_not_null(frame_data);

  check_greater_equal(turbo_simulcast_encode_frame(ctx, frame_data, frame_size, 33333), 1);
  check_greater(capture.packet_count, 0);
  check_equal((int)(capture.last_layer), (int)(SIMULCAST_LAYER_HIGH));
  check_equal((uint8_t)(capture.last_packet.header.version), (uint8_t)(RTP_VERSION));
  check_equal((uint8_t)(capture.last_packet.header.payload_type), (uint8_t)(codec->payload_type));
  check_equal((uint32_t)(capture.last_packet.header.ssrc), (uint32_t)(0x55667788));
  check_greater(capture.last_packet.payload_len, 0);

  int64_t bytes_sent = 0;
  turbo_simulcast_get_layer_stats(ctx, SIMULCAST_LAYER_HIGH, NULL, NULL, NULL, NULL, &bytes_sent);
  check_greater(bytes_sent, 0);
  check_greater_equal((size_t)bytes_sent, capture.total_bytes);

  free(frame_data);
  turbo_simulcast_destroy(ctx);
}

void test_simulcast_layer_switching(void) {
  const turbo_codec_ops_t *codec = turbo_codec_find_by_name("vp8");
  if (!codec) {
    check(0, "%s", ("VP8 codec not available"));
    return;
  }

  simulcast_ctx_t *ctx = turbo_simulcast_create(1280, 720, 30, codec);
  check_not_null(ctx);

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
  check_true(high_active || low_active);

  turbo_simulcast_destroy(ctx);
}

/* =============================================================================
 * Main
 * ============================================================================= */

spec("test_simulcast") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  /* Creation */
  it("test_simulcast_create") { test_simulcast_create(); };
  it("test_simulcast_create_invalid_params") { test_simulcast_create_invalid_params(); };

  /* Layer Management */
  it("test_simulcast_enable_disable_layers") { test_simulcast_enable_disable_layers(); };
  it("test_simulcast_layer_stats") { test_simulcast_layer_stats(); };

  /* Bandwidth Adaptation */
  it("test_simulcast_set_bandwidth") { test_simulcast_set_bandwidth(); };
  it("test_simulcast_bandwidth_layer_selection") { test_simulcast_bandwidth_layer_selection(); };

  /* Encoding */
  it("test_simulcast_encode_frame") { test_simulcast_encode_frame(); };
  it("test_simulcast_callback_emits_rtp_packets") { test_simulcast_callback_emits_rtp_packets(); };

  /* Statistics */
  it("test_simulcast_layer_stats_after_encode") { test_simulcast_layer_stats_after_encode(); };
  it("test_simulcast_layer_switching") { test_simulcast_layer_switching(); };
}
