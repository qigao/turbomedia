#include "turbo_codec.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int run_g711_case(const char *name, int payload_type) {
    const int16_t pcm_samples[] = {
        -12000, -8000, -4000, -1000, 0, 1000, 4000, 8000, 12000
    };
    uint8_t encoded[sizeof(pcm_samples) / sizeof(pcm_samples[0])];
    uint8_t decoded[sizeof(pcm_samples)];
    size_t encoded_len = sizeof(encoded);
    size_t decoded_len = sizeof(decoded);
    turbo_encoded_frame_t frame_info;

    turbo_audio_codec_config_t config = {0};
    config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
    config.channels = 1;
    config.bitrate = 64000;
    config.frame_size_ms = 20;

    const turbo_codec_ops_t *ops = turbo_codec_find_by_name(name);
    if (!ops || ops->payload_type != payload_type) {
        printf("Failed to find %s by name.\n", name);
        return 1;
    }

    if (turbo_codec_find_by_pt(payload_type) != ops) {
        printf("Failed to find %s by payload type %d.\n", name, payload_type);
        return 1;
    }

    turbo_codec_t *encoder = turbo_codec_create_encoder(name, &config);
    turbo_codec_t *decoder = turbo_codec_create_decoder(name, &config);
    if (!encoder || !decoder) {
        printf("Failed to create %s encoder/decoder.\n", name);
        turbo_codec_destroy(encoder);
        turbo_codec_destroy(decoder);
        return 1;
    }

    memset(&frame_info, 0, sizeof(frame_info));
    int rc = turbo_codec_encode(encoder,
                                (const uint8_t *)pcm_samples,
                                sizeof(pcm_samples),
                                encoded,
                                &encoded_len,
                                &frame_info);
    if (rc != TURBO_CODEC_OK || encoded_len != sizeof(encoded) ||
        frame_info.data != encoded || frame_info.len != encoded_len) {
        printf("Failed to encode %s: rc=%d, len=%zu.\n", name, rc, encoded_len);
        turbo_codec_destroy(encoder);
        turbo_codec_destroy(decoder);
        return 1;
    }

    rc = turbo_codec_decode(decoder, encoded, encoded_len, decoded, &decoded_len);
    if (rc != TURBO_CODEC_OK || decoded_len != sizeof(decoded)) {
        printf("Failed to decode %s: rc=%d, len=%zu.\n", name, rc, decoded_len);
        turbo_codec_destroy(encoder);
        turbo_codec_destroy(decoder);
        return 1;
    }

    printf("%s OK: %zu PCM bytes -> %zu G.711 bytes -> %zu PCM bytes\n",
           name, sizeof(pcm_samples), encoded_len, decoded_len);

    turbo_codec_destroy(encoder);
    turbo_codec_destroy(decoder);
    return 0;
}

static int expect_codec_registered(const char *name, int payload_type) {
    const turbo_codec_ops_t *by_name = turbo_codec_find_by_name(name);
    const turbo_codec_ops_t *by_pt = turbo_codec_find_by_pt(payload_type);

    if (!by_name || by_name != by_pt) {
        printf("Failed to find optional codec %s at payload type %d.\n",
               name, payload_type);
        return 1;
    }

    printf("%s registered at payload type %d\n", name, payload_type);
    return 0;
}

int main(void) {
    turbo_codec_registry_init();

    int failed = 0;
    failed |= run_g711_case("pcmu", 0);
    failed |= run_g711_case("pcma", 8);
#ifdef TURBO_MEDIA_HAS_OPUS
    failed |= expect_codec_registered("opus", 111);
#endif
#ifdef TURBO_MEDIA_HAS_VPX
    failed |= expect_codec_registered("vp8", 96);
    failed |= expect_codec_registered("vp9", 98);
#endif
#ifdef TURBO_MEDIA_HAS_H264
    failed |= expect_codec_registered("h264", 102);
#endif
#ifdef TURBO_MEDIA_HAS_H265
    failed |= expect_codec_registered("h265", 103);
#endif

    turbo_codec_registry_shutdown();
    return failed ? 1 : 0;
}
