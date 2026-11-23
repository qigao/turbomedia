#ifndef TURBO_AUDIO_CODEC_H
#define TURBO_AUDIO_CODEC_H

#include <turbo_codec_types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_audio_codec_s turbo_audio_codec_t;

/* Names are `pcmu`, `pcma`, and `opus`. PCM input and output are interleaved signed 16-bit samples.
 * Factories return a caller-owned instance, or NULL if unavailable. */
TURBO_MEDIA_API turbo_audio_codec_t *turbo_audio_codec_create_encoder(
    const char *name, const turbo_audio_codec_config_t *config);
TURBO_MEDIA_API turbo_audio_codec_t *turbo_audio_codec_create_decoder(
    const char *name, const turbo_audio_codec_config_t *config);

/* Release the instance. NULL is accepted. */
TURBO_MEDIA_API void turbo_audio_codec_destroy(turbo_audio_codec_t *codec);

/* Encode one input buffer. Set *output_len to capacity before calling. */
TURBO_MEDIA_API int turbo_audio_codec_encode(turbo_audio_codec_t *codec,
    const uint8_t *input, size_t input_len, uint8_t *output, size_t *output_len,
    turbo_encoded_frame_t *info);

/* Decode one input buffer. Set *output_len to capacity before calling. */
TURBO_MEDIA_API int turbo_audio_codec_decode(turbo_audio_codec_t *codec,
    const uint8_t *input, size_t input_len, uint8_t *output, size_t *output_len);

/* Update an encoder's target bitrate when supported. */
TURBO_MEDIA_API void turbo_audio_codec_set_bitrate(turbo_audio_codec_t *codec, int bitrate_bps);

/* Conceal one lost audio packet. */
TURBO_MEDIA_API int turbo_audio_codec_plc(turbo_audio_codec_t *codec, uint8_t *output, size_t *output_len);

/* Return the RTP clock in Hz, or zero for an unavailable codec. */
TURBO_MEDIA_API int turbo_audio_codec_rtp_clock_rate(const char *name);

#ifdef __cplusplus
}
#endif

#endif
