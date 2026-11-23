#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef TEST_AudioCodec
#include <turbo_audio_codec.h>

static int test_audio(const char *name, int rate) {
    const turbo_audio_codec_config_t config = {
        .sample_rate = rate, .channels = 1, .bitrate = 64000, .frame_size_ms = 20
    };
    turbo_audio_codec_t *encoder = turbo_audio_codec_create_encoder(name, &config);
    turbo_audio_codec_t *decoder = turbo_audio_codec_create_decoder(name, &config);
    int16_t pcm[960];
    const size_t samples = (size_t)rate / 50;
    for (size_t i = 0; i < samples; ++i) pcm[i] = (int16_t)(((int)(i % 41) - 20) * 64);
    uint8_t encoded[4096];
    int16_t decoded[960];
    size_t encoded_size = sizeof(encoded), decoded_size = sizeof(decoded);
    turbo_encoded_frame_t info = {0};
    const int ok = encoder && decoder && turbo_audio_codec_rtp_clock_rate(name) == rate &&
        turbo_audio_codec_encode(encoder, (const uint8_t *)pcm, samples * sizeof(*pcm),
                                 encoded, &encoded_size, &info) == 0 && encoded_size > 0 &&
        turbo_audio_codec_decode(decoder, encoded, encoded_size, (uint8_t *)decoded,
                                 &decoded_size) == 0 && decoded_size == samples * sizeof(*decoded);
    turbo_audio_codec_destroy(decoder);
    turbo_audio_codec_destroy(encoder);
    if (!ok) fprintf(stderr, "%s audio codec round trip failed\n", name);
    return ok;
}
#endif

#ifdef TEST_VideoCodec
#include <turbo_video_codec.h>

static int test_video(void) {
    const turbo_video_codec_config_t config = {
        .width = 32, .height = 32, .framerate = 10, .bitrate = 100000,
        .keyframe_interval = 10, .threads = 1, .quality = 20
    };
    turbo_video_codec_t *encoder = turbo_video_codec_create_encoder("vp8", &config);
    turbo_video_codec_t *decoder = turbo_video_codec_create_decoder("vp8", &config);
    uint8_t frame[32 * 32 * 3 / 2], decoded[sizeof(frame)], encoded[4096];
    memset(frame, 128, sizeof(frame));
    size_t encoded_size = sizeof(encoded), decoded_size = sizeof(decoded);
    turbo_encoded_frame_t info = {0};
    const int ok = encoder && decoder &&
        turbo_video_codec_encode(encoder, frame, sizeof(frame), encoded, &encoded_size, &info) == 0 &&
        encoded_size > 0 && turbo_video_codec_decode(decoder, encoded, encoded_size, decoded,
                                                     &decoded_size) == 0 && decoded_size == sizeof(frame);
    turbo_video_codec_destroy(decoder);
    turbo_video_codec_destroy(encoder);
    if (!ok) fprintf(stderr, "VP8 video codec round trip failed\n");
    return ok;
}
#endif

int main(void) {
    int ok = 1;
#ifdef TEST_AudioCodec
    ok &= test_audio("pcmu", 8000);
    ok &= test_audio("opus", 48000);
#endif
#ifdef TEST_VideoCodec
    ok &= test_video();
#endif
    return ok ? 0 : 1;
}
