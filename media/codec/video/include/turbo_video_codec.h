#ifndef TURBO_VIDEO_CODEC_H
#define TURBO_VIDEO_CODEC_H

#include <turbo_codec_types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_video_codec_s turbo_video_codec_t;

/* Names are `vp8`, `vp9`, `h264`, and `h265`. Raw frames use contiguous YUV420P data.
 * Factories return a caller-owned instance, or NULL if unavailable. */
TURBO_MEDIA_API turbo_video_codec_t *turbo_video_codec_create_encoder(
    const char *name, const turbo_video_codec_config_t *config);
TURBO_MEDIA_API turbo_video_codec_t *turbo_video_codec_create_decoder(
    const char *name, const turbo_video_codec_config_t *config);

/* Release the instance. NULL is accepted. */
TURBO_MEDIA_API void turbo_video_codec_destroy(turbo_video_codec_t *codec);

/* Encode one input buffer. Set *output_len to capacity before calling. */
TURBO_MEDIA_API int turbo_video_codec_encode(turbo_video_codec_t *codec,
    const uint8_t *input, size_t input_len, uint8_t *output, size_t *output_len,
    turbo_encoded_frame_t *info);

/* Decode one input buffer. Set *output_len to capacity before calling. */
TURBO_MEDIA_API int turbo_video_codec_decode(turbo_video_codec_t *codec,
    const uint8_t *input, size_t input_len, uint8_t *output, size_t *output_len);

/* Update an encoder's target bitrate when supported. */
TURBO_MEDIA_API void turbo_video_codec_set_bitrate(turbo_video_codec_t *codec, int bitrate_bps);

/* Request a keyframe from the encoder. */
TURBO_MEDIA_API void turbo_video_codec_request_keyframe(turbo_video_codec_t *codec);

#ifdef __cplusplus
}
#endif

#endif
