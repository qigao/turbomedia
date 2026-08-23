/**
 * TurboMedia Mobile Hardware Codec Staging API
 *
 * Narrow mobile-only wrapper for platform hardware codecs. This is intentionally
 * separate from any WebRTC/RTP codec registry.
 */
#ifndef TURBO_MOBILE_CODEC_H
#define TURBO_MOBILE_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "turbo_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TURBO_MOBILE_PIXEL_FORMAT_I420 = 0,
    TURBO_MOBILE_PIXEL_FORMAT_NV12 = 1
} turbo_mobile_pixel_format_t;

typedef struct {
    int width;
    int height;
    turbo_mobile_pixel_format_t format;
    uint8_t *data[4];
    int linesize[4];
    size_t data_len[4];
    uint64_t timestamp;
} turbo_mobile_video_frame_t;

typedef struct turbo_mobile_codec_s turbo_mobile_codec_t;

struct turbo_mobile_codec_s {
    int (*encode)(turbo_mobile_codec_t *codec,
                  const turbo_mobile_video_frame_t *frame,
                  uint8_t *output,
                  size_t *output_size);
    int (*decode)(turbo_mobile_codec_t *codec,
                  const uint8_t *data,
                  size_t size,
                  turbo_mobile_video_frame_t *frame);
    void (*destroy)(turbo_mobile_codec_t *codec);
};

TURBO_MEDIA_API turbo_mobile_codec_t *turbo_mobile_codec_android_hw_h264_create(bool is_encoder,
                                                                          int width,
                                                                          int height,
                                                                          int bitrate);

TURBO_MEDIA_API turbo_mobile_codec_t *turbo_mobile_codec_ios_hw_h264_create(bool is_encoder,
                                                                      int width,
                                                                      int height,
                                                                      int bitrate,
                                                                      int fps);

TURBO_MEDIA_API turbo_mobile_codec_t *turbo_mobile_codec_ios_hw_hevc_create(bool is_encoder,
                                                                      int width,
                                                                      int height,
                                                                      int bitrate,
                                                                      int fps);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MOBILE_CODEC_H */
