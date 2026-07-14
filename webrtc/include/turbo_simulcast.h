/**
 * Simulcast API
 *
 * Multi-quality video streaming for conferencing systems
 */
#ifndef TURBO_SIMULCAST_H
#define TURBO_SIMULCAST_H

#include "turbo_codec.h"
#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct simulcast_ctx_t simulcast_ctx_t;

/* Simulcast layer identifiers */
typedef enum {
  SIMULCAST_LAYER_LOW = 0,    /* Low resolution (320x180 @ 15fps, ~150 kbps) */
  SIMULCAST_LAYER_MEDIUM = 1, /* Medium resolution (640x360 @ 30fps, ~500 kbps) */
  SIMULCAST_LAYER_HIGH = 2,   /* High resolution (1280x720 @ 30fps, ~1500 kbps) */
  SIMULCAST_LAYER_COUNT = 3
} simulcast_layer_t;

/**
 * Create simulcast context
 *
 * @param width Source video width
 * @param height Source video height
 * @param framerate Source video framerate
 * @param codec Video codec to use for all layers
 * @return Simulcast context or NULL on error
 */
CXX_C_API simulcast_ctx_t *turbo_simulcast_create(int width, int height, int framerate,
                                                  const turbo_codec_ops_t *codec);

/**
 * Destroy simulcast context
 */
CXX_C_API void turbo_simulcast_destroy(simulcast_ctx_t *ctx);

/**
 * Encode a frame for all active layers
 *
 * @param ctx Simulcast context
 * @param frame_data Raw video frame (I420 format)
 * @param frame_len Frame data length
 * @param timestamp_us Frame timestamp in microseconds
 * @return Number of layers encoded, or -1 on error
 */
CXX_C_API int turbo_simulcast_encode_frame(simulcast_ctx_t *ctx, const uint8_t *frame_data,
                                           size_t frame_len, int64_t timestamp_us);

/**
 * Update available bandwidth (triggers layer selection)
 *
 * @param ctx Simulcast context
 * @param bandwidth_bps Available bandwidth in bits per second
 */
CXX_C_API void turbo_simulcast_set_bandwidth(simulcast_ctx_t *ctx, int bandwidth_bps);

/**
 * Enable or disable a specific layer
 *
 * @param ctx Simulcast context
 * @param layer Layer to control
 * @param enable 1 to enable, 0 to disable
 */
CXX_C_API void turbo_simulcast_enable_layer(simulcast_ctx_t *ctx, simulcast_layer_t layer,
                                            int enable);

/**
 * Check if a layer is currently active
 *
 * @param ctx Simulcast context
 * @param layer Layer to check
 * @return 1 if active, 0 if inactive
 */
CXX_C_API int turbo_simulcast_is_layer_active(simulcast_ctx_t *ctx, simulcast_layer_t layer);

/**
 * Request keyframe for a specific layer
 *
 * @param ctx Simulcast context
 * @param layer Layer to request keyframe for
 */
CXX_C_API void turbo_simulcast_request_keyframe(simulcast_ctx_t *ctx, simulcast_layer_t layer);

/**
 * Set target bitrate for a specific layer
 *
 * @param ctx Simulcast context
 * @param layer Layer to configure
 * @param bitrate_bps Target bitrate in bits per second
 */
CXX_C_API void turbo_simulcast_set_layer_bitrate(simulcast_ctx_t *ctx, simulcast_layer_t layer,
                                                 int bitrate_bps);

/**
 * Set RTP packet callback
 *
 * Called when encoded packets are ready to send
 *
 * @param ctx Simulcast context
 * @param callback Callback function
 * @param user_data User data passed to callback
 */
CXX_C_API void turbo_simulcast_set_rtp_callback(simulcast_ctx_t *ctx,
                                                void (*callback)(void *user_data,
                                                                 simulcast_layer_t layer,
                                                                 const uint8_t *packet, size_t len),
                                                void *user_data);

/**
 * Get layer statistics
 *
 * @param ctx Simulcast context
 * @param layer Layer to query
 * @param width Output: layer width (can be NULL)
 * @param height Output: layer height (can be NULL)
 * @param bitrate Output: layer bitrate (can be NULL)
 * @param frames_encoded Output: frames encoded (can be NULL)
 * @param bytes_sent Output: bytes sent (can be NULL)
 */
CXX_C_API void turbo_simulcast_get_layer_stats(simulcast_ctx_t *ctx, simulcast_layer_t layer,
                                               int *width, int *height, int *bitrate,
                                               int *frames_encoded, int64_t *bytes_sent);

/**
 * Get SSRC for a specific layer
 *
 * @param ctx Simulcast context
 * @param layer Layer to query
 * @return SSRC value
 */
CXX_C_API uint32_t turbo_simulcast_get_layer_ssrc(simulcast_ctx_t *ctx, simulcast_layer_t layer);

/**
 * Set SSRC for a specific layer
 *
 * @param ctx Simulcast context
 * @param layer Layer to configure
 * @param ssrc SSRC value
 */
CXX_C_API void turbo_simulcast_set_layer_ssrc(simulcast_ctx_t *ctx, simulcast_layer_t layer,
                                              uint32_t ssrc);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SIMULCAST_H */
