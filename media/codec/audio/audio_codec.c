#include "turbo_audio_codec.h"
#include "turbo_codec.h"
#include "../codec_instance.h"

#include <string.h>

static const turbo_codec_ops_t *turbo_audio_codec_find(const char *name) {
    static const turbo_codec_ops_t *const codecs[] = {
        &turbo_g711_pcmu_codec_ops,
        &turbo_g711_pcma_codec_ops,
        &turbo_opus_codec_ops,
    };

    if (!name) return NULL;

    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); ++i) {
        if (strcmp(codecs[i]->name, name) == 0)
            return codecs[i];
    }
    return NULL;
}

turbo_audio_codec_t *turbo_audio_codec_create_encoder(
    const char *name, const turbo_audio_codec_config_t *config) {
    return (turbo_audio_codec_t *)codec_instance_create_encoder(turbo_audio_codec_find(name), config);
}

turbo_audio_codec_t *turbo_audio_codec_create_decoder(
    const char *name, const turbo_audio_codec_config_t *config) {
    return (turbo_audio_codec_t *)codec_instance_create_decoder(turbo_audio_codec_find(name), config);
}

void turbo_audio_codec_destroy(turbo_audio_codec_t *codec) {
    codec_instance_destroy((turbo_codec_t *)codec);
}

int turbo_audio_codec_encode(turbo_audio_codec_t *codec,
    const uint8_t *input, size_t input_len, uint8_t *output, size_t *output_len,
    turbo_encoded_frame_t *info) {
    return codec_instance_encode((turbo_codec_t *)codec, input, input_len, output, output_len, info);
}

int turbo_audio_codec_decode(turbo_audio_codec_t *codec,
    const uint8_t *input, size_t input_len, uint8_t *output, size_t *output_len) {
    return codec_instance_decode((turbo_codec_t *)codec, input, input_len, output, output_len);
}

void turbo_audio_codec_set_bitrate(turbo_audio_codec_t *codec, int bitrate_bps) {
    codec_instance_set_bitrate((turbo_codec_t *)codec, bitrate_bps);
}

int turbo_audio_codec_plc(turbo_audio_codec_t *codec, uint8_t *output, size_t *output_len) {
    return codec_instance_plc((turbo_codec_t *)codec, output, output_len);
}

int turbo_audio_codec_rtp_clock_rate(const char *name) {
    const turbo_codec_ops_t *ops = turbo_audio_codec_find(name);
    return ops ? ops->clock_rate : 0;
}
