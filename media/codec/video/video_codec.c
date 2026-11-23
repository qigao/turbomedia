#include "turbo_video_codec.h"
#include "turbo_codec.h"
#include "../codec_instance.h"

#include <string.h>

static const turbo_codec_ops_t *turbo_video_codec_find(const char *name) {
    static const turbo_codec_ops_t *const codecs[] = {
        &turbo_vp8_codec_ops,
        &turbo_vp9_codec_ops,
        &turbo_h264_codec_ops,
        &turbo_h265_codec_ops,
    };

    if (!name) return NULL;

    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); ++i) {
        if (strcmp(codecs[i]->name, name) == 0)
            return codecs[i];
    }
    return NULL;
}

turbo_video_codec_t *turbo_video_codec_create_encoder(
    const char *name, const turbo_video_codec_config_t *config) {
    return (turbo_video_codec_t *)codec_instance_create_encoder(turbo_video_codec_find(name), config);
}

turbo_video_codec_t *turbo_video_codec_create_decoder(
    const char *name, const turbo_video_codec_config_t *config) {
    return (turbo_video_codec_t *)codec_instance_create_decoder(turbo_video_codec_find(name), config);
}

void turbo_video_codec_destroy(turbo_video_codec_t *codec) {
    codec_instance_destroy((turbo_codec_t *)codec);
}

int turbo_video_codec_encode(turbo_video_codec_t *codec,
    const uint8_t *input, size_t input_len, uint8_t *output, size_t *output_len,
    turbo_encoded_frame_t *info) {
    return codec_instance_encode((turbo_codec_t *)codec, input, input_len, output, output_len, info);
}

int turbo_video_codec_decode(turbo_video_codec_t *codec,
    const uint8_t *input, size_t input_len, uint8_t *output, size_t *output_len) {
    return codec_instance_decode((turbo_codec_t *)codec, input, input_len, output, output_len);
}

void turbo_video_codec_set_bitrate(turbo_video_codec_t *codec, int bitrate_bps) {
    codec_instance_set_bitrate((turbo_codec_t *)codec, bitrate_bps);
}

void turbo_video_codec_request_keyframe(turbo_video_codec_t *codec) {
    codec_instance_request_keyframe((turbo_codec_t *)codec);
}
