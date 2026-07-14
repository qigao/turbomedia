/**
 * Simulcast Implementation
 *
 * Enables sending multiple quality levels of the same video stream
 * Essential for conferencing systems where receivers have different bandwidth
 *
 * Features:
 * - 3 spatial layers (high/medium/low resolution)
 * - Independent encoding per layer
 * - Bandwidth-based layer selection
 * - RTP stream synchronization
 * - Automatic layer switching
 */
#include "turbo_codec.h"
#include "turbo_rtp.h"
#include "turbo_simulcast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Simulcast Layer Configuration
 * ============================================================================= */

typedef struct {
  simulcast_layer_t layer;

  /* Resolution */
  int width;
  int height;

  /* Bitrate */
  int target_bitrate;
  int min_bitrate;
  int max_bitrate;

  /* Framerate */
  int framerate;

  /* RTP */
  uint32_t ssrc;
  uint16_t seq_num;
  uint32_t timestamp;

  /* Encoder */
  void *encoder_ctx;
  const turbo_codec_ops_t *codec;

  /* State */
  int active;
  int64_t last_encode_time;
  int64_t bytes_sent;
  int frames_encoded;

  /* Buffers */
  uint8_t *encode_buffer;
  size_t encode_buffer_size;
  uint8_t *rtp_buffer;
  size_t rtp_buffer_size;
} simulcast_layer_ctx_t;

struct simulcast_ctx_t {
  /* Layers */
  simulcast_layer_ctx_t layers[SIMULCAST_LAYER_COUNT];

  /* Source configuration */
  int source_width;
  int source_height;
  int source_framerate;

  /* Active layers */
  int active_layer_mask; /* Bitmask of active layers */

  /* Bandwidth management */
  int available_bandwidth;

  /* Callbacks */
  void (*on_rtp_packet)(void *user_data, simulcast_layer_t layer, const uint8_t *packet,
                        size_t len);
  void *user_data;

  /* Stats */
  int64_t total_bytes_sent;
  int64_t total_frames_encoded;
};

/* =============================================================================
 * Layer Configuration Presets
 * ============================================================================= */

static void simulcast_configure_layer(simulcast_layer_ctx_t *layer, simulcast_layer_t layer_id,
                                      int source_width, int source_height, int source_framerate) {
  layer->layer = layer_id;

  switch (layer_id) {
  case SIMULCAST_LAYER_LOW:
    /* Low: 320x180 @ 15fps, ~150 kbps */
    layer->width = source_width / 4;
    layer->height = source_height / 4;
    layer->framerate = source_framerate / 2;
    layer->target_bitrate = 150000;
    layer->min_bitrate = 100000;
    layer->max_bitrate = 250000;
    break;

  case SIMULCAST_LAYER_MEDIUM:
    /* Medium: 640x360 @ 30fps, ~500 kbps */
    layer->width = source_width / 2;
    layer->height = source_height / 2;
    layer->framerate = source_framerate;
    layer->target_bitrate = 500000;
    layer->min_bitrate = 300000;
    layer->max_bitrate = 800000;
    break;

  case SIMULCAST_LAYER_HIGH:
    /* High: 1280x720 @ 30fps, ~1500 kbps */
    layer->width = source_width;
    layer->height = source_height;
    layer->framerate = source_framerate;
    layer->target_bitrate = 1500000;
    layer->min_bitrate = 800000;
    layer->max_bitrate = 2500000;
    break;

  default:
    break;
  }

  /* Allocate encode buffer (assume max 1MB per frame) */
  layer->encode_buffer_size = 1024 * 1024;
  layer->encode_buffer = (uint8_t *)malloc(layer->encode_buffer_size);
  layer->rtp_buffer_size = RTP_MAX_PACKET;
  layer->rtp_buffer = (uint8_t *)malloc(layer->rtp_buffer_size);

  layer->active = 0;
  layer->seq_num = 0;
  layer->timestamp = 0;
  layer->last_encode_time = 0;
  layer->bytes_sent = 0;
  layer->frames_encoded = 0;
}

/* =============================================================================
 * Simulcast Context Management
 * ============================================================================= */

simulcast_ctx_t *turbo_simulcast_create(int width, int height, int framerate,
                                        const turbo_codec_ops_t *codec) {
  if (!codec || width <= 0 || height <= 0 || framerate <= 0) {
    return NULL;
  }

  simulcast_ctx_t *ctx = (simulcast_ctx_t *)calloc(1, sizeof(simulcast_ctx_t));
  if (!ctx) return NULL;

  ctx->source_width = width;
  ctx->source_height = height;
  ctx->source_framerate = framerate;
  ctx->active_layer_mask = 0x07;      /* All layers active by default */
  ctx->available_bandwidth = 2500000; /* Start with 2.5 Mbps */

  /* Configure all layers */
  for (int i = 0; i < SIMULCAST_LAYER_COUNT; i++) {
    simulcast_configure_layer(&ctx->layers[i], (simulcast_layer_t)i, width, height, framerate);

    /* Create encoder for each layer */
    turbo_video_codec_config_t config = {.width = ctx->layers[i].width,
                                         .height = ctx->layers[i].height,
                                         .framerate = ctx->layers[i].framerate,
                                         .bitrate = ctx->layers[i].target_bitrate,
                                         .keyframe_interval =
                                             framerate * 2, /* Keyframe every 2 seconds */
                                         .threads = 2};

    ctx->layers[i].encoder_ctx = codec->create_encoder(&config);
    ctx->layers[i].codec = codec;

    if (!ctx->layers[i].encoder_ctx) {
      /* Cleanup on failure */
      for (int j = 0; j < i; j++) {
        if (ctx->layers[j].encoder_ctx) {
          codec->destroy(ctx->layers[j].encoder_ctx);
        }
        free(ctx->layers[j].encode_buffer);
        free(ctx->layers[j].rtp_buffer);
      }
      free(ctx);
      return NULL;
    }

    /* Generate unique SSRC for each layer */
    ctx->layers[i].ssrc = 0x12345678 + i;
  }

  return ctx;
}

void turbo_simulcast_destroy(simulcast_ctx_t *ctx) {
  if (!ctx) return;

  for (int i = 0; i < SIMULCAST_LAYER_COUNT; i++) {
    if (ctx->layers[i].encoder_ctx && ctx->layers[i].codec) {
      ctx->layers[i].codec->destroy(ctx->layers[i].encoder_ctx);
    }
    free(ctx->layers[i].encode_buffer);
    free(ctx->layers[i].rtp_buffer);
  }

  free(ctx);
}

/* =============================================================================
 * Bandwidth-Based Layer Selection
 * ============================================================================= */

static void simulcast_update_active_layers(simulcast_ctx_t *ctx) {
  int bw = ctx->available_bandwidth;
  int new_mask = 0;

  /* Decide which layers to enable based on available bandwidth */
  if (bw >= 2000000) {
    /* High bandwidth: All layers */
    new_mask = 0x07; /* 111 binary */
  } else if (bw >= 1000000) {
    /* Medium bandwidth: Medium + Low */
    new_mask = 0x03; /* 011 binary */
  } else if (bw >= 300000) {
    /* Low bandwidth: Low only */
    new_mask = 0x01; /* 001 binary */
  } else {
    /* Very low: Disable all (audio only) */
    new_mask = 0x00;
  }

  /* Update layer active state */
  for (int i = 0; i < SIMULCAST_LAYER_COUNT; i++) {
    int should_be_active = (new_mask & (1 << i)) != 0;

    if (should_be_active && !ctx->layers[i].active) {
      /* Layer becoming active - request keyframe */
      if (ctx->layers[i].codec->request_keyframe) {
        ctx->layers[i].codec->request_keyframe(ctx->layers[i].encoder_ctx);
      }
    }

    ctx->layers[i].active = should_be_active;
  }

  ctx->active_layer_mask = new_mask;
}

void turbo_simulcast_set_bandwidth(simulcast_ctx_t *ctx, int bandwidth_bps) {
  if (!ctx) return;

  ctx->available_bandwidth = bandwidth_bps;

  simulcast_update_active_layers(ctx);
}

/* =============================================================================
 * Frame Scaling and Encoding
 * ============================================================================= */

static void scale_frame_simple(const uint8_t *src, int src_w, int src_h, uint8_t *dst, int dst_w,
                               int dst_h) {
  /* Simple nearest-neighbor scaling for I420 format */
  /* Y plane */
  for (int y = 0; y < dst_h; y++) {
    int src_y = (y * src_h) / dst_h;
    for (int x = 0; x < dst_w; x++) {
      int src_x = (x * src_w) / dst_w;
      dst[y * dst_w + x] = src[src_y * src_w + src_x];
    }
  }

  /* U and V planes (half resolution) */
  int src_uv_w = src_w / 2;
  int src_uv_h = src_h / 2;
  int dst_uv_w = dst_w / 2;
  int dst_uv_h = dst_h / 2;

  size_t src_y_size = src_w * src_h;
  size_t dst_y_size = dst_w * dst_h;

  for (int plane = 0; plane < 2; plane++) {
    const uint8_t *src_uv = src + src_y_size + (plane * src_uv_w * src_uv_h);
    uint8_t *dst_uv = dst + dst_y_size + (plane * dst_uv_w * dst_uv_h);

    for (int y = 0; y < dst_uv_h; y++) {
      int src_y = (y * src_uv_h) / dst_uv_h;
      for (int x = 0; x < dst_uv_w; x++) {
        int src_x = (x * src_uv_w) / dst_uv_w;
        dst_uv[y * dst_uv_w + x] = src_uv[src_y * src_uv_w + src_x];
      }
    }
  }
}

int turbo_simulcast_encode_frame(simulcast_ctx_t *ctx, const uint8_t *frame_data, size_t frame_len,
                                 int64_t timestamp_us) {
  if (!ctx || !frame_data) return -1;

  int frames_encoded = 0;

  /* Encode each active layer */
  for (int i = 0; i < SIMULCAST_LAYER_COUNT; i++) {
    simulcast_layer_ctx_t *layer = &ctx->layers[i];

    if (!layer->active) continue;

    /* Check if we should encode this frame (framerate control) */
    int64_t frame_interval_us = 1000000 / layer->framerate;
    if (timestamp_us - layer->last_encode_time < frame_interval_us) {
      continue;
    }

    /* Scale frame if needed */
    uint8_t *scaled_frame = NULL;
    size_t scaled_len = 0;

    if (layer->width == ctx->source_width && layer->height == ctx->source_height) {
      /* No scaling needed */
      scaled_frame = (uint8_t *)frame_data;
      scaled_len = frame_len;
    } else {
      /* Scale frame */
      scaled_len = layer->width * layer->height * 3 / 2; /* I420 format */
      scaled_frame = (uint8_t *)malloc(scaled_len);
      if (!scaled_frame) continue;

      scale_frame_simple(frame_data, ctx->source_width, ctx->source_height, scaled_frame,
                         layer->width, layer->height);
    }

    /* Encode frame */
    size_t encoded_len = layer->encode_buffer_size;
    turbo_encoded_frame_t frame_info;
    uint32_t rtp_timestamp;
    uint32_t timestamp_step =
        (uint32_t)(RTP_CLOCK_VIDEO / (layer->framerate > 0 ? layer->framerate : 30));

    int ret = layer->codec->encode(layer->encoder_ctx, scaled_frame, scaled_len,
                                   layer->encode_buffer, &encoded_len, &frame_info);

    if (scaled_frame != frame_data) {
      free(scaled_frame);
    }

    if (ret != TURBO_CODEC_OK || encoded_len == 0) {
      continue;
    }

    if (timestamp_us > 0) {
      uint64_t scaled_ts = ((uint64_t)timestamp_us * RTP_CLOCK_VIDEO) / 1000000ULL;
      rtp_timestamp = (uint32_t)scaled_ts;
      layer->timestamp = rtp_timestamp + timestamp_step;
    } else {
      rtp_timestamp = layer->timestamp;
      layer->timestamp += timestamp_step;
    }

    /* Update layer stats */
    layer->last_encode_time = timestamp_us;
    layer->frames_encoded++;

    /* Packetize and send as real RTP packets so downstream consumers see
         * the same wire
     * shape they would get from an actual sender. */
    if (ctx->on_rtp_packet) {
      turbo_rtp_fragment_t fragments[TURBO_CODEC_MAX_PACKETS];
      int fragment_count = 0;

      memset(fragments, 0, sizeof(fragments));

      if (layer->codec->packetize) {
        fragment_count =
            layer->codec->packetize(layer->encoder_ctx, &frame_info, fragments,
                                    TURBO_CODEC_MAX_PACKETS, RTP_MAX_PACKET - RTP_HEADER_SIZE);
        if (fragment_count < 0) {
          fragment_count = 0;
        }
      }

      if (fragment_count == 0) {
        fragments[0].data = layer->encode_buffer;
        fragments[0].len = encoded_len;
        fragments[0].marker = 1;
        fragments[0].fragment_start = 1;
        fragments[0].fragment_end = 1;
        fragment_count = 1;
      }

      for (int frag_idx = 0; frag_idx < fragment_count; ++frag_idx) {
        rtp_packet_t packet;
        int packet_len = rtp_packet_build(
            &packet, (uint8_t)layer->codec->payload_type, layer->seq_num++, rtp_timestamp,
            layer->ssrc, fragments[frag_idx].marker || fragments[frag_idx].fragment_end,
            fragments[frag_idx].data, fragments[frag_idx].len, layer->rtp_buffer,
            layer->rtp_buffer_size);

        if (packet_len <= 0) {
          continue;
        }

        layer->bytes_sent += packet_len;
        ctx->total_bytes_sent += packet_len;
        ctx->on_rtp_packet(ctx->user_data, layer->layer, layer->rtp_buffer, (size_t)packet_len);
      }
    }

    frames_encoded++;
  }

  ctx->total_frames_encoded += frames_encoded;

  return frames_encoded;
}

/* =============================================================================
 * Layer Control
 * ============================================================================= */

void turbo_simulcast_enable_layer(simulcast_ctx_t *ctx, simulcast_layer_t layer, int enable) {
  if (!ctx || layer >= SIMULCAST_LAYER_COUNT) return;

  if (enable) {
    ctx->active_layer_mask |= (1 << layer);
  } else {
    ctx->active_layer_mask &= ~(1 << layer);
  }

  ctx->layers[layer].active = enable;
}

int turbo_simulcast_is_layer_active(simulcast_ctx_t *ctx, simulcast_layer_t layer) {
  if (!ctx || layer >= SIMULCAST_LAYER_COUNT) return 0;
  return ctx->layers[layer].active;
}

void turbo_simulcast_request_keyframe(simulcast_ctx_t *ctx, simulcast_layer_t layer) {
  if (!ctx || layer >= SIMULCAST_LAYER_COUNT) return;

  simulcast_layer_ctx_t *l = &ctx->layers[layer];
  if (l->active && l->codec->request_keyframe) {
    l->codec->request_keyframe(l->encoder_ctx);
  }
}

void turbo_simulcast_set_layer_bitrate(simulcast_ctx_t *ctx, simulcast_layer_t layer,
                                       int bitrate_bps) {
  if (!ctx || layer >= SIMULCAST_LAYER_COUNT) return;

  simulcast_layer_ctx_t *l = &ctx->layers[layer];

  /* Clamp to min/max */
  if (bitrate_bps < l->min_bitrate) bitrate_bps = l->min_bitrate;
  if (bitrate_bps > l->max_bitrate) bitrate_bps = l->max_bitrate;

  l->target_bitrate = bitrate_bps;

  if (l->codec->set_bitrate) {
    l->codec->set_bitrate(l->encoder_ctx, bitrate_bps);
  }
}

/* =============================================================================
 * Callbacks
 * ============================================================================= */

void turbo_simulcast_set_rtp_callback(simulcast_ctx_t *ctx,
                                      void (*callback)(void *user_data, simulcast_layer_t layer,
                                                       const uint8_t *packet, size_t len),
                                      void *user_data) {
  if (!ctx) return;
  ctx->on_rtp_packet = callback;
  ctx->user_data = user_data;
}

/* =============================================================================
 * Statistics
 * ============================================================================= */

void turbo_simulcast_get_layer_stats(simulcast_ctx_t *ctx, simulcast_layer_t layer, int *width,
                                     int *height, int *bitrate, int *frames_encoded,
                                     int64_t *bytes_sent) {
  if (!ctx || layer >= SIMULCAST_LAYER_COUNT) return;

  simulcast_layer_ctx_t *l = &ctx->layers[layer];

  if (width) *width = l->width;
  if (height) *height = l->height;
  if (bitrate) *bitrate = l->target_bitrate;
  if (frames_encoded) *frames_encoded = l->frames_encoded;
  if (bytes_sent) *bytes_sent = l->bytes_sent;
}

uint32_t turbo_simulcast_get_layer_ssrc(simulcast_ctx_t *ctx, simulcast_layer_t layer) {
  if (!ctx || layer >= SIMULCAST_LAYER_COUNT) return 0;
  return ctx->layers[layer].ssrc;
}

void turbo_simulcast_set_layer_ssrc(simulcast_ctx_t *ctx, simulcast_layer_t layer, uint32_t ssrc) {
  if (!ctx || layer >= SIMULCAST_LAYER_COUNT) return;
  ctx->layers[layer].ssrc = ssrc;
}
