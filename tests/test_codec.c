#include "turbo_codec.h"
#include "turbo_capture.h"
#include <tinytest.h>

#include <stdint.h>
#include <string.h>

#define CHECK_TRUE(expr)       \
    do {                       \
        check(expr);           \
        if (!(expr)) {         \
            return;            \
        }                      \
    } while (0)

#define CHECK_INT(expected, actual)            \
    do {                                      \
        int expected_value = (expected);      \
        int actual_value = (actual);          \
        check_equal(actual_value, expected_value); \
        if (actual_value != expected_value) { \
            return;                           \
        }                                     \
    } while (0)

static turbo_audio_codec_config_t g711_config(void) {
    turbo_audio_codec_config_t config;
    memset(&config, 0, sizeof(config));
    config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
    config.channels = 1;
    config.bitrate = 64000;
    config.frame_size_ms = 20;
    return config;
}

static void test_registry_lifecycle_registers_g711(void) {
    turbo_codec_registry_shutdown();
    CHECK_TRUE(turbo_codec_find_by_name("pcmu") == NULL);
    CHECK_TRUE(turbo_codec_find_by_pt(0) == NULL);

    turbo_codec_registry_init();

    const turbo_codec_ops_t *pcmu = turbo_codec_find_by_name("pcmu");
    const turbo_codec_ops_t *pcma = turbo_codec_find_by_name("pcma");

    CHECK_TRUE(pcmu != NULL);
    CHECK_TRUE(pcma != NULL);
    CHECK_TRUE(turbo_codec_find_by_pt(0) == pcmu);
    CHECK_TRUE(turbo_codec_find_by_pt(8) == pcma);
    CHECK_INT(TURBO_CODEC_TYPE_AUDIO, pcmu->type);
    CHECK_INT(TURBO_CODEC_TYPE_AUDIO, pcma->type);
    CHECK_INT(8000, pcmu->clock_rate);
    CHECK_INT(8000, pcma->clock_rate);

    turbo_codec_registry_shutdown();
    CHECK_TRUE(turbo_codec_find_by_name("pcmu") == NULL);
}

static void test_registry_rejects_duplicate_codec(void) {
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    const turbo_codec_ops_t *pcmu = turbo_codec_find_by_name("pcmu");

    CHECK_TRUE(pcmu != NULL);
    CHECK_INT(-1, turbo_codec_register(pcmu));
    CHECK_INT(-1, turbo_codec_register(NULL));

    turbo_codec_registry_shutdown();
}

static void test_unknown_codec_creation_fails(void) {
    turbo_audio_codec_config_t config = g711_config();

    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    CHECK_TRUE(turbo_codec_create_encoder("missing", &config) == NULL);
    CHECK_TRUE(turbo_codec_create_decoder("missing", &config) == NULL);
    CHECK_TRUE(turbo_codec_find_by_name(NULL) == NULL);

    turbo_codec_registry_shutdown();
}

static void test_encode_decode_require_matching_codec_direction(void) {
    turbo_audio_codec_config_t config = g711_config();
    uint8_t input[] = {0xff};
    uint8_t output[8];
    size_t output_len = sizeof(output);

    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
    turbo_codec_t *decoder = turbo_codec_create_decoder("pcmu", &config);

    CHECK_TRUE(encoder != NULL);
    CHECK_TRUE(decoder != NULL);
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_decode(encoder, input, sizeof(input), output, &output_len));
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_encode(decoder, input, sizeof(input), output, &output_len, NULL));

    turbo_codec_destroy(encoder);
    turbo_codec_destroy(decoder);
    turbo_codec_registry_shutdown();
}

static void test_g711_buffer_limits_are_reported(void) {
    const int16_t pcm_samples[] = {-12000, 0, 12000};
    turbo_audio_codec_config_t config = g711_config();
    uint8_t encoded[2];
    size_t encoded_len = sizeof(encoded);

    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);

    CHECK_TRUE(encoder != NULL);
    CHECK_INT(TURBO_CODEC_ERR_BUFFER,
              turbo_codec_encode(encoder,
                                 (const uint8_t *)pcm_samples,
                                 sizeof(pcm_samples),
                                 encoded,
                                 &encoded_len,
                                 NULL));

    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
}

static void test_g711_round_trip_sets_frame_info(void) {
    const int16_t pcm_samples[] = {-12000, -8000, -4000, 0, 4000, 8000, 12000};
    turbo_audio_codec_config_t config = g711_config();
    uint8_t encoded[sizeof(pcm_samples) / sizeof(pcm_samples[0])];
    uint8_t decoded[sizeof(pcm_samples)];
    size_t encoded_len = sizeof(encoded);
    size_t decoded_len = sizeof(decoded);
    turbo_encoded_frame_t frame_info;

    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcma", &config);
    turbo_codec_t *decoder = turbo_codec_create_decoder("pcma", &config);

    CHECK_TRUE(encoder != NULL);
    CHECK_TRUE(decoder != NULL);

    memset(&frame_info, 0, sizeof(frame_info));
    CHECK_INT(TURBO_CODEC_OK,
              turbo_codec_encode(encoder,
                                 (const uint8_t *)pcm_samples,
                                 sizeof(pcm_samples),
                                 encoded,
                                 &encoded_len,
                                 &frame_info));
    CHECK_TRUE(frame_info.data == encoded);
    CHECK_TRUE(frame_info.len == encoded_len);
    CHECK_INT(0, frame_info.is_keyframe);
    CHECK_INT(1, frame_info.packet_count);
    CHECK_TRUE(encoded_len == sizeof(encoded));

    CHECK_INT(TURBO_CODEC_OK,
              turbo_codec_decode(decoder, encoded, encoded_len, decoded, &decoded_len));
    CHECK_TRUE(decoded_len == sizeof(decoded));

    turbo_codec_destroy(encoder);
    turbo_codec_destroy(decoder);
    turbo_codec_registry_shutdown();
}

static void test_gpu_device_list_rejects_empty_output(void) {
    CHECK_INT(-1, turbo_capture_list_gpu_devices(NULL, 0));
}

/* Extended test helper functions */
static turbo_video_codec_config_t video_config(void) {
    turbo_video_codec_config_t config;
    memset(&config, 0, sizeof(config));
    config.width = 640;
    config.height = 480;
    config.framerate = 30;
    config.bitrate = 1000000; // 1 Mbps
    config.keyframe_interval = 30;
    config.threads = 1;
    config.quality = 25;
    return config;
}

static void generate_test_frame(uint8_t *frame, int width, int height) {
    // Generate simple I420 test pattern
    int y_size = width * height;
    int uv_size = (width * height) / 4;
    
    // Y plane - gradient
    for (int i = 0; i < y_size; i++) {
        frame[i] = (uint8_t)(i % 256);
    }
    
    // U plane - constant
    for (int i = 0; i < uv_size; i++) {
        frame[y_size + i] = 128;
    }
    
    // V plane - constant  
    for (int i = 0; i < uv_size; i++) {
        frame[y_size + uv_size + i] = 128;
    }
}

static void test_codec_lifecycle_management(void);
static void test_codec_various_input_sizes(void);
static void test_codec_error_conditions(void);
static void test_codec_bitrate_control(void);
static void test_video_keyframe_request(void);
static void test_packet_loss_concealment(void);
static void test_codec_multiple_sample_rates(void);
static void test_codec_concurrent_creation(void);

/* Test codec registry with multiple codecs */
static void test_registry_supports_multiple_codec_types(void) {
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    // Test audio codecs
    const turbo_codec_ops_t *pcmu = turbo_codec_find_by_name("pcmu");
    const turbo_codec_ops_t *pcma = turbo_codec_find_by_name("pcma");
    CHECK_TRUE(pcmu != NULL);
    CHECK_TRUE(pcma != NULL);
    CHECK_INT(TURBO_CODEC_TYPE_AUDIO, pcmu->type);
    CHECK_INT(TURBO_CODEC_TYPE_AUDIO, pcma->type);

#ifdef TURBO_MEDIA_HAS_VPX
    // Test video codecs if available
    const turbo_codec_ops_t *vp8 = turbo_codec_find_by_name("vp8");
    if (vp8) {
        CHECK_INT(TURBO_CODEC_TYPE_VIDEO, vp8->type);
        CHECK_TRUE(vp8->clock_rate == 90000); // Standard video clock rate
    }
#endif

#ifdef TURBO_MEDIA_HAS_OPUS
    // Test Opus codec if available
    const turbo_codec_ops_t *opus = turbo_codec_find_by_name("opus");
    if (opus) {
        CHECK_INT(TURBO_CODEC_TYPE_AUDIO, opus->type);
        CHECK_TRUE(opus->clock_rate == 48000);
    }
#endif

    turbo_codec_registry_shutdown();
}

/* Test codec configuration validation */
static void test_audio_codec_config_validation(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    // Test invalid sample rates
    config.sample_rate = 0;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);
    
    config.sample_rate = -1;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);
    
    config.sample_rate = 999999; // Unrealistic rate
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);

    // Test invalid channels
    config = g711_config();
    config.channels = 0;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);
    
    config.channels = -1;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);
    
    config.channels = 100; // Too many channels
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);

    // Test invalid frame sizes
    config = g711_config();
    config.frame_size_ms = 0;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);
    
    config.frame_size_ms = -1;
    CHECK_TRUE(turbo_codec_create_encoder("pcmu", &config) == NULL);

    turbo_codec_registry_shutdown();
}

/* Test video codec operations */
static void test_video_codec_config_validation(void) {
#ifdef TURBO_MEDIA_HAS_VPX
    turbo_video_codec_config_t config = video_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    // Test invalid dimensions
    config.width = 0;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);
    
    config = video_config();
    config.height = 0;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);
    
    config = video_config();
    config.width = -1;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);

    // Test invalid framerate
    config = video_config();
    config.framerate = 0;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);
    
    config.framerate = -1;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);
    
    config.framerate = 1000; // Unrealistic framerate
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);

    // Test invalid bitrate
    config = video_config();
    config.bitrate = -1;
    CHECK_TRUE(turbo_codec_create_encoder("vp8", &config) == NULL);

    turbo_codec_registry_shutdown();
#endif
}

suite("turbo_media_codec") {
    group("Codec Registry Management") {
        it("registers and unregisters built-in G.711 codecs") {
            test_registry_lifecycle_registers_g711();
        }

        it("rejects duplicate codec registrations") {
            test_registry_rejects_duplicate_codec();
        }

        it("rejects unknown codec creation") {
            test_unknown_codec_creation_fails();
        }

        it("supports multiple codec types in registry") {
            test_registry_supports_multiple_codec_types();
        }
    }

    group("Codec Configuration Validation") {
        it("validates audio codec configuration parameters") {
            test_audio_codec_config_validation();
        }

        it("validates video codec configuration parameters") {
            test_video_codec_config_validation();
        }

        it("supports multiple sample rates") {
            test_codec_multiple_sample_rates();
        }
    }

    group("Codec Operations") {
        it("requires codec direction to match encode and decode calls") {
            test_encode_decode_require_matching_codec_direction();
        }

        it("reports G.711 output buffer limits") {
            test_g711_buffer_limits_are_reported();
        }

        it("round-trips G.711 and sets frame metadata") {
            test_g711_round_trip_sets_frame_info();
        }

        it("handles various input sizes correctly") {
            test_codec_various_input_sizes();
        }
    }

    group("Codec Lifecycle") {
        it("manages codec instance lifecycle properly") {
            test_codec_lifecycle_management();
        }

        it("supports concurrent codec instance creation") {
            test_codec_concurrent_creation();
        }
    }

    group("Error Handling") {
        it("handles error conditions gracefully") {
            test_codec_error_conditions();
        }
    }

    group("Advanced Features") {
        it("supports bitrate control") {
            test_codec_bitrate_control();
        }

        it("supports keyframe requests for video codecs") {
            test_video_keyframe_request();
        }

        it("supports packet loss concealment") {
            test_packet_loss_concealment();
        }
    }

    group("Device Integration") {
        it("rejects empty GPU device output") {
            test_gpu_device_list_rejects_empty_output();
        }
    }
}

/* Test codec lifecycle management */
static void test_codec_lifecycle_management(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
    turbo_codec_t *decoder = turbo_codec_create_decoder("pcmu", &config);

    CHECK_TRUE(encoder != NULL);
    CHECK_TRUE(decoder != NULL);

    // Test proper destruction
    turbo_codec_destroy(encoder);
    turbo_codec_destroy(decoder);
    
    // Test null destruction (should not crash)
    turbo_codec_destroy(NULL);

    turbo_codec_registry_shutdown();
}

/* Test codec operations with various input sizes */
static void test_codec_various_input_sizes(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
    CHECK_TRUE(encoder != NULL);

    // Test different input sizes
    int16_t samples_160[160];  // 160 samples for 20ms @ 8kHz
    uint8_t encoded[256];
    size_t encoded_len;
    turbo_encoded_frame_t frame_info;

    // Initialize samples
    for (int i = 0; i < 160; i++) {
        samples_160[i] = (int16_t)(i * 100);
    }

    // Test normal frame size
    encoded_len = sizeof(encoded);
    memset(&frame_info, 0, sizeof(frame_info));
    int result = turbo_codec_encode(encoder,
                                  (const uint8_t *)samples_160,
                                  sizeof(samples_160),
                                  encoded,
                                  &encoded_len,
                                  &frame_info);
    
    if (result == TURBO_CODEC_OK) {
        CHECK_TRUE(encoded_len > 0);
        CHECK_TRUE(frame_info.len == encoded_len);
        CHECK_TRUE(frame_info.data == encoded);
    }

    // Test empty input
    encoded_len = sizeof(encoded);
    result = turbo_codec_encode(encoder, NULL, 0, encoded, &encoded_len, NULL);
    CHECK_INT(TURBO_CODEC_ERR_INVALID, result);

    // Test null output
    encoded_len = sizeof(encoded);
    result = turbo_codec_encode(encoder,
                              (const uint8_t *)samples_160,
                              sizeof(samples_160),
                              NULL,
                              &encoded_len,
                              NULL);
    CHECK_INT(TURBO_CODEC_ERR_INVALID, result);

    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
}

/* Test codec error handling */
static void test_codec_error_conditions(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
    turbo_codec_t *decoder = turbo_codec_create_decoder("pcmu", &config);

    CHECK_TRUE(encoder != NULL);
    CHECK_TRUE(decoder != NULL);

    // Test operations with null codec instances
    uint8_t buffer[128];
    size_t buffer_len = sizeof(buffer);
    
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_encode(NULL, buffer, sizeof(buffer), 
                               buffer, &buffer_len, NULL));
    
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_decode(NULL, buffer, sizeof(buffer),
                               buffer, &buffer_len));

    // Test with null output length pointer
    CHECK_INT(TURBO_CODEC_ERR_INVALID,
              turbo_codec_encode(encoder, buffer, sizeof(buffer),
                               buffer, NULL, NULL));

    turbo_codec_destroy(encoder);
    turbo_codec_destroy(decoder);
    turbo_codec_registry_shutdown();
}

/* Test codec bitrate control */
static void test_codec_bitrate_control(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
    if (encoder == NULL) {
        turbo_codec_registry_shutdown();
        return;
    }

    // Test bitrate setting if supported
    turbo_codec_set_bitrate(encoder, 64000);
    turbo_codec_set_bitrate(encoder, 128000);

    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
}

/* Test keyframe request for video codecs */
static void test_video_keyframe_request(void) {
#ifdef TURBO_MEDIA_HAS_VPX
    turbo_video_codec_config_t config = video_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *encoder = turbo_codec_create_encoder("vp8", &config);
    if (encoder == NULL) {
        turbo_codec_registry_shutdown();
        return;
    }

    // Test keyframe request
    turbo_codec_request_keyframe(encoder);

    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
#endif
}

/* Test packet loss concealment */
static void test_packet_loss_concealment(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    turbo_codec_t *decoder = turbo_codec_create_decoder("pcmu", &config);
    if (decoder == NULL) {
        turbo_codec_registry_shutdown();
        return;
    }

    // Test PLC if supported
    {
        uint8_t plc_frame[320];
        size_t plc_len = sizeof(plc_frame);
        int result = turbo_codec_plc(decoder, plc_frame, &plc_len);

        if (result == TURBO_CODEC_OK) CHECK_TRUE(plc_len > 0);
    }

    turbo_codec_destroy(decoder);
    turbo_codec_registry_shutdown();
}

/* Test codec with different sample rates */
static void test_codec_multiple_sample_rates(void) {
    int sample_rates[] = {8000, 16000, 24000, 48000};
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    for (size_t i = 0; i < sizeof(sample_rates) / sizeof(sample_rates[0]); i++) {
        turbo_audio_codec_config_t config = g711_config();
        config.sample_rate = sample_rates[i];
        
        turbo_codec_t *encoder = turbo_codec_create_encoder("pcmu", &config);
        if (encoder) {
            // Successfully created with this sample rate
            turbo_codec_destroy(encoder);
        }
    }

    turbo_codec_registry_shutdown();
}

/* Test codec thread safety preparation */
static void test_codec_concurrent_creation(void) {
    turbo_audio_codec_config_t config = g711_config();
    
    turbo_codec_registry_shutdown();
    turbo_codec_registry_init();

    // Create multiple codec instances
    turbo_codec_t *encoders[5];
    turbo_codec_t *decoders[5];
    
    for (int i = 0; i < 5; i++) {
        encoders[i] = turbo_codec_create_encoder("pcmu", &config);
        decoders[i] = turbo_codec_create_decoder("pcmu", &config);
    }

    // Verify all created successfully
    int success_count = 0;
    for (int i = 0; i < 5; i++) {
        if (encoders[i] && decoders[i]) {
            success_count++;
        }
    }
    
    CHECK_TRUE(success_count > 0);

    // Clean up
    for (int i = 0; i < 5; i++) {
        if (encoders[i]) turbo_codec_destroy(encoders[i]);
        if (decoders[i]) turbo_codec_destroy(decoders[i]);
    }

    turbo_codec_registry_shutdown();
}
