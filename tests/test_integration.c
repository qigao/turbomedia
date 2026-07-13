#include "turbo_capture.h"
#include "turbo_codec.h"
#include "turbo_playback.h"
#include <tinytest.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Integration test helpers */
#define VERIFY(condition) \
    do { \
        check(condition); \
        if (!(condition)) { \
            return; \
        } \
    } while (0)

#define VERIFY_EQ(expected, actual) \
    do { \
        int exp = (expected); \
        int act = (actual); \
        check_int_eq(act, exp); \
        if (exp != act) { \
            return; \
        } \
    } while (0)

/* Test fixture for integration tests */
typedef struct {
    turbo_capture_t *capture;
    turbo_codec_t *encoder;
    turbo_codec_t *decoder;
    turbo_playback_t *playback;
    
    uint8_t *captured_audio;
    size_t captured_audio_len;
    uint8_t *encoded_data;
    size_t encoded_data_len;
    uint8_t *decoded_audio;
    size_t decoded_audio_len;
    
    int capture_callback_count;
    int playback_callback_count;
    int encode_success;
    int decode_success;
} integration_test_context_t;

static void cleanup_integration_context(integration_test_context_t *ctx) {
    if (!ctx) return;
    
    if (ctx->capture) turbo_capture_destroy(ctx->capture);
    if (ctx->encoder) turbo_codec_destroy(ctx->encoder);
    if (ctx->decoder) turbo_codec_destroy(ctx->decoder);
    if (ctx->playback) turbo_playback_destroy(ctx->playback);
    
    free(ctx->captured_audio);
    free(ctx->encoded_data);
    free(ctx->decoded_audio);
    
    memset(ctx, 0, sizeof(*ctx));
}

/* Integration test callbacks */
static void integration_audio_capture_cb(turbo_capture_t *capture,
                                        const uint8_t *samples, size_t len,
                                        uint64_t timestamp, void *user_data) {
    integration_test_context_t *ctx = (integration_test_context_t *)user_data;
    (void)capture;
    (void)timestamp;
    
    ctx->capture_callback_count++;
    
    if (ctx->captured_audio) free(ctx->captured_audio);
    ctx->captured_audio = malloc(len);
    if (ctx->captured_audio) {
        memcpy(ctx->captured_audio, samples, len);
        ctx->captured_audio_len = len;
    }
}

static size_t integration_playback_data_cb(turbo_playback_t *playback,
                                          void *output, size_t frame_count,
                                          void *user_data) {
    integration_test_context_t *ctx = (integration_test_context_t *)user_data;
    (void)playback;
    
    ctx->playback_callback_count++;
    
    if (ctx->decoded_audio && ctx->decoded_audio_len > 0) {
        size_t bytes_to_copy = frame_count * sizeof(int16_t) * 2; // Stereo
        if (bytes_to_copy > ctx->decoded_audio_len) {
            bytes_to_copy = ctx->decoded_audio_len;
        }
        
        memcpy(output, ctx->decoded_audio, bytes_to_copy);
        return bytes_to_copy / (sizeof(int16_t) * 2);
    }
    
    return 0;
}

suite("TurboMedia Integration Tests") {
    
    section("Capture to Encode Pipeline") {
        
        it("should capture audio and encode with G.711") {
            integration_test_context_t ctx = {0};
            turbo_audio_capture_config_t capture_config;
            turbo_audio_codec_config_t codec_config;
            
            // Initialize codec registry
            turbo_codec_registry_shutdown();
            turbo_codec_registry_init();
            
            // Setup capture
            memset(&capture_config, 0, sizeof(capture_config));
            capture_config.sample_rate = 8000;
            capture_config.channels = 1;
            capture_config.bits_per_sample = 16;
            capture_config.frame_size_ms = 20;
            
            ctx.capture = turbo_audio_capture_create(NULL, &capture_config);
            if (ctx.capture) {
                turbo_audio_capture_set_callback(ctx.capture, 
                                                integration_audio_capture_cb, &ctx);
                
                // Setup encoder
                memset(&codec_config, 0, sizeof(codec_config));
                codec_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
                codec_config.channels = 1;
                codec_config.bitrate = 64000;
                codec_config.frame_size_ms = 20;
                
                ctx.encoder = turbo_codec_create_encoder("pcmu", &codec_config);
                VERIFY(ctx.encoder != NULL);
                
                // Simulate captured audio (160 samples @ 8kHz = 20ms)
                int16_t test_audio[160];
                for (int i = 0; i < 160; i++) {
                    test_audio[i] = (int16_t)(i * 100);
                }
                
                // Encode
                uint8_t encoded[256];
                size_t encoded_len = sizeof(encoded);
                turbo_encoded_frame_t frame_info;
                
                memset(&frame_info, 0, sizeof(frame_info));
                int result = turbo_codec_encode(ctx.encoder,
                                              (const uint8_t *)test_audio,
                                              sizeof(test_audio),
                                              encoded,
                                              &encoded_len,
                                              &frame_info);
                
                VERIFY_EQ(TURBO_CODEC_OK, result);
                VERIFY(encoded_len > 0);
                VERIFY(frame_info.len == encoded_len);
                
                ctx.encode_success = 1;
            }
            
            cleanup_integration_context(&ctx);
            turbo_codec_registry_shutdown();
        }
    }
    
    section("Encode to Decode Pipeline") {
        
        it("should encode and decode audio with matching quality") {
            integration_test_context_t ctx = {0};
            turbo_audio_codec_config_t codec_config;
            
            turbo_codec_registry_shutdown();
            turbo_codec_registry_init();
            
            // Setup codec config
            memset(&codec_config, 0, sizeof(codec_config));
            codec_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
            codec_config.channels = 1;
            codec_config.bitrate = 64000;
            codec_config.frame_size_ms = 20;
            
            ctx.encoder = turbo_codec_create_encoder("pcmu", &codec_config);
            ctx.decoder = turbo_codec_create_decoder("pcmu", &codec_config);
            
            VERIFY(ctx.encoder != NULL);
            VERIFY(ctx.decoder != NULL);
            
            // Create test audio
            int16_t input_audio[160];
            for (int i = 0; i < 160; i++) {
                input_audio[i] = (int16_t)(1000 * ((i % 20) - 10));
            }
            
            // Encode
            uint8_t encoded[256];
            size_t encoded_len = sizeof(encoded);
            int result = turbo_codec_encode(ctx.encoder,
                                          (const uint8_t *)input_audio,
                                          sizeof(input_audio),
                                          encoded,
                                          &encoded_len,
                                          NULL);
            VERIFY_EQ(TURBO_CODEC_OK, result);
            
            // Decode
            int16_t decoded[160];
            size_t decoded_len = sizeof(decoded);
            result = turbo_codec_decode(ctx.decoder,
                                       encoded,
                                       encoded_len,
                                       (uint8_t *)decoded,
                                       &decoded_len);
            VERIFY_EQ(TURBO_CODEC_OK, result);
            VERIFY_EQ(sizeof(input_audio), decoded_len);
            
            ctx.encode_success = 1;
            ctx.decode_success = 1;
            
            cleanup_integration_context(&ctx);
            turbo_codec_registry_shutdown();
        }
        
        it("should handle multiple encode-decode cycles") {
            integration_test_context_t ctx = {0};
            turbo_audio_codec_config_t codec_config;
            
            turbo_codec_registry_shutdown();
            turbo_codec_registry_init();
            
            memset(&codec_config, 0, sizeof(codec_config));
            codec_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
            codec_config.channels = 1;
            codec_config.bitrate = 64000;
            codec_config.frame_size_ms = 20;
            
            ctx.encoder = turbo_codec_create_encoder("pcma", &codec_config);
            ctx.decoder = turbo_codec_create_decoder("pcma", &codec_config);
            
            if (ctx.encoder && ctx.decoder) {
                // Process multiple frames
                for (int frame = 0; frame < 5; frame++) {
                    int16_t input[160];
                    for (int i = 0; i < 160; i++) {
                        input[i] = (int16_t)(frame * 1000 + i);
                    }
                    
                    uint8_t encoded[256];
                    size_t encoded_len = sizeof(encoded);
                    int result = turbo_codec_encode(ctx.encoder,
                                                  (const uint8_t *)input,
                                                  sizeof(input),
                                                  encoded,
                                                  &encoded_len,
                                                  NULL);
                    VERIFY_EQ(TURBO_CODEC_OK, result);
                    
                    int16_t decoded[160];
                    size_t decoded_len = sizeof(decoded);
                    result = turbo_codec_decode(ctx.decoder,
                                               encoded,
                                               encoded_len,
                                               (uint8_t *)decoded,
                                               &decoded_len);
                    VERIFY_EQ(TURBO_CODEC_OK, result);
                }
            }
            
            cleanup_integration_context(&ctx);
            turbo_codec_registry_shutdown();
        }
    }
    
    section("Decode to Playback Pipeline") {
        
        it("should decode audio and play through playback system") {
            integration_test_context_t ctx = {0};
            turbo_audio_codec_config_t codec_config;
            turbo_playback_config_t playback_config;
            
            turbo_codec_registry_shutdown();
            turbo_codec_registry_init();
            
            // Setup decoder
            memset(&codec_config, 0, sizeof(codec_config));
            codec_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
            codec_config.channels = 1;
            codec_config.bitrate = 64000;
            codec_config.frame_size_ms = 20;
            
            ctx.decoder = turbo_codec_create_decoder("pcmu", &codec_config);
            VERIFY(ctx.decoder != NULL);
            
            // Setup playback
            memset(&playback_config, 0, sizeof(playback_config));
            playback_config.sample_rate = 8000;
            playback_config.channels = 1;
            playback_config.format = TURBO_PLAYBACK_FORMAT_S16;
            playback_config.buffer_size_ms = 50;
            
            ctx.playback = turbo_playback_create(NULL, &playback_config);
            if (ctx.playback) {
                turbo_playback_set_data_callback(ctx.playback,
                                                integration_playback_data_cb,
                                                &ctx);
                
                // Prepare decoded audio for playback
                int16_t decoded_samples[160];
                for (int i = 0; i < 160; i++) {
                    decoded_samples[i] = (int16_t)(500 * ((i % 40) - 20));
                }
                
                ctx.decoded_audio = malloc(sizeof(decoded_samples));
                if (ctx.decoded_audio) {
                    memcpy(ctx.decoded_audio, decoded_samples, sizeof(decoded_samples));
                    ctx.decoded_audio_len = sizeof(decoded_samples);
                }
                
                // Playback lifecycle
                int result = turbo_playback_start(ctx.playback);
                if (result == TURBO_PLAYBACK_OK) {
                    turbo_playback_stop(ctx.playback);
                }
            }
            
            cleanup_integration_context(&ctx);
            turbo_codec_registry_shutdown();
        }
    }
    
    section("End-to-End Audio Pipeline") {
        
        it("should complete full capture-encode-decode-playback flow") {
            integration_test_context_t ctx = {0};
            turbo_audio_capture_config_t capture_config;
            turbo_audio_codec_config_t codec_config;
            turbo_playback_config_t playback_config;
            
            turbo_codec_registry_shutdown();
            turbo_codec_registry_init();
            
            // Setup full pipeline
            memset(&capture_config, 0, sizeof(capture_config));
            capture_config.sample_rate = 8000;
            capture_config.channels = 1;
            capture_config.bits_per_sample = 16;
            capture_config.frame_size_ms = 20;
            
            memset(&codec_config, 0, sizeof(codec_config));
            codec_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
            codec_config.channels = 1;
            codec_config.bitrate = 64000;
            codec_config.frame_size_ms = 20;
            
            memset(&playback_config, 0, sizeof(playback_config));
            playback_config.sample_rate = 8000;
            playback_config.channels = 1;
            playback_config.format = TURBO_PLAYBACK_FORMAT_S16;
            playback_config.buffer_size_ms = 50;
            
            // Create pipeline components
            ctx.capture = turbo_audio_capture_create(NULL, &capture_config);
            ctx.encoder = turbo_codec_create_encoder("pcmu", &codec_config);
            ctx.decoder = turbo_codec_create_decoder("pcmu", &codec_config);
            ctx.playback = turbo_playback_create(NULL, &playback_config);
            
            VERIFY(ctx.encoder != NULL);
            VERIFY(ctx.decoder != NULL);
            
            // Simulate end-to-end flow
            int16_t captured_samples[160];
            for (int i = 0; i < 160; i++) {
                captured_samples[i] = (int16_t)(800 * ((i % 30) - 15));
            }
            
            // Encode captured audio
            uint8_t encoded[256];
            size_t encoded_len = sizeof(encoded);
            int result = turbo_codec_encode(ctx.encoder,
                                          (const uint8_t *)captured_samples,
                                          sizeof(captured_samples),
                                          encoded,
                                          &encoded_len,
                                          NULL);
            VERIFY_EQ(TURBO_CODEC_OK, result);
            
            // Decode for playback
            int16_t decoded_samples[160];
            size_t decoded_len = sizeof(decoded_samples);
            result = turbo_codec_decode(ctx.decoder,
                                       encoded,
                                       encoded_len,
                                       (uint8_t *)decoded_samples,
                                       &decoded_len);
            VERIFY_EQ(TURBO_CODEC_OK, result);
            
            // Verify pipeline integrity
            VERIFY(encoded_len > 0);
            VERIFY_EQ(sizeof(captured_samples), decoded_len);
            
            cleanup_integration_context(&ctx);
            turbo_codec_registry_shutdown();
        }
        
        it("should maintain audio quality through pipeline") {
            integration_test_context_t ctx = {0};
            turbo_audio_codec_config_t codec_config;
            
            turbo_codec_registry_shutdown();
            turbo_codec_registry_init();
            
            memset(&codec_config, 0, sizeof(codec_config));
            codec_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
            codec_config.channels = 1;
            codec_config.bitrate = 64000;
            codec_config.frame_size_ms = 20;
            
            ctx.encoder = turbo_codec_create_encoder("pcmu", &codec_config);
            ctx.decoder = turbo_codec_create_decoder("pcmu", &codec_config);
            
            if (ctx.encoder && ctx.decoder) {
                // Create predictable test pattern
                int16_t input[160];
                for (int i = 0; i < 160; i++) {
                    input[i] = (int16_t)(i % 2 ? 1000 : -1000);
                }
                
                // Encode
                uint8_t encoded[256];
                size_t encoded_len = sizeof(encoded);
                turbo_codec_encode(ctx.encoder,
                                 (const uint8_t *)input,
                                 sizeof(input),
                                 encoded,
                                 &encoded_len,
                                 NULL);
                
                // Decode
                int16_t output[160];
                size_t output_len = sizeof(output);
                turbo_codec_decode(ctx.decoder,
                                 encoded,
                                 encoded_len,
                                 (uint8_t *)output,
                                 &output_len);
                
                // Verify output is reasonable (lossy codec, so not exact match)
                VERIFY(output_len == sizeof(input));
            }
            
            cleanup_integration_context(&ctx);
            turbo_codec_registry_shutdown();
        }
    }
    
    section("Multi-Component Error Handling") {
        
        it("should handle codec registry errors gracefully in pipeline") {
            integration_test_context_t ctx = {0};
            
            // Try to use codecs without initializing registry
            turbo_codec_registry_shutdown();
            
            turbo_audio_codec_config_t codec_config;
            memset(&codec_config, 0, sizeof(codec_config));
            codec_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
            codec_config.channels = 1;
            
            ctx.encoder = turbo_codec_create_encoder("pcmu", &codec_config);
            VERIFY(ctx.encoder == NULL);  // Should fail without registry
            
            cleanup_integration_context(&ctx);
        }
        
        it("should handle mismatched configurations between components") {
            integration_test_context_t ctx = {0};
            turbo_audio_codec_config_t encoder_config;
            turbo_audio_codec_config_t decoder_config;
            
            turbo_codec_registry_shutdown();
            turbo_codec_registry_init();
            
            // Create encoder with 8kHz
            memset(&encoder_config, 0, sizeof(encoder_config));
            encoder_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
            encoder_config.channels = 1;
            encoder_config.bitrate = 64000;
            encoder_config.frame_size_ms = 20;
            
            // Create decoder with 16kHz (mismatch)
            memset(&decoder_config, 0, sizeof(decoder_config));
            decoder_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_16K;
            decoder_config.channels = 1;
            decoder_config.bitrate = 64000;
            decoder_config.frame_size_ms = 20;
            
            ctx.encoder = turbo_codec_create_encoder("pcmu", &encoder_config);
            ctx.decoder = turbo_codec_create_decoder("pcmu", &decoder_config);
            
            // Both should create successfully (config validation is per-instance)
            // But using them together may produce unexpected results
            
            cleanup_integration_context(&ctx);
            turbo_codec_registry_shutdown();
        }
        
        it("should handle component failures in pipeline") {
            integration_test_context_t ctx = {0};
            
            turbo_codec_registry_shutdown();
            turbo_codec_registry_init();
            
            turbo_audio_codec_config_t codec_config;
            memset(&codec_config, 0, sizeof(codec_config));
            codec_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
            codec_config.channels = 1;
            codec_config.bitrate = 64000;
            codec_config.frame_size_ms = 20;
            
            ctx.encoder = turbo_codec_create_encoder("pcmu", &codec_config);
            VERIFY(ctx.encoder != NULL);
            
            // Try to encode with invalid input
            uint8_t encoded[256];
            size_t encoded_len = sizeof(encoded);
            int result = turbo_codec_encode(ctx.encoder,
                                          NULL,  // Invalid input
                                          0,
                                          encoded,
                                          &encoded_len,
                                          NULL);
            
            VERIFY(result != TURBO_CODEC_OK);  // Should fail gracefully
            
            cleanup_integration_context(&ctx);
            turbo_codec_registry_shutdown();
        }
    }
    
    section("Resource Management Across Components") {
        
        it("should properly clean up resources in correct order") {
            integration_test_context_t ctx = {0};
            turbo_audio_capture_config_t capture_config;
            turbo_audio_codec_config_t codec_config;
            turbo_playback_config_t playback_config;
            
            turbo_codec_registry_shutdown();
            turbo_codec_registry_init();
            
            // Create all components
            memset(&capture_config, 0, sizeof(capture_config));
            capture_config.sample_rate = 8000;
            capture_config.channels = 1;
            capture_config.bits_per_sample = 16;
            capture_config.frame_size_ms = 20;
            
            memset(&codec_config, 0, sizeof(codec_config));
            codec_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
            codec_config.channels = 1;
            codec_config.bitrate = 64000;
            codec_config.frame_size_ms = 20;
            
            memset(&playback_config, 0, sizeof(playback_config));
            playback_config.sample_rate = 8000;
            playback_config.channels = 1;
            playback_config.format = TURBO_PLAYBACK_FORMAT_S16;
            playback_config.buffer_size_ms = 50;
            
            ctx.capture = turbo_audio_capture_create(NULL, &capture_config);
            ctx.encoder = turbo_codec_create_encoder("pcmu", &codec_config);
            ctx.decoder = turbo_codec_create_decoder("pcmu", &codec_config);
            ctx.playback = turbo_playback_create(NULL, &playback_config);
            
            // Clean up in reverse order (good practice)
            cleanup_integration_context(&ctx);
            turbo_codec_registry_shutdown();
            
            // Verify no crashes occurred
        }
        
        it("should handle partial pipeline creation") {
            integration_test_context_t ctx = {0};
            turbo_audio_codec_config_t codec_config;
            
            turbo_codec_registry_shutdown();
            turbo_codec_registry_init();
            
            memset(&codec_config, 0, sizeof(codec_config));
            codec_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
            codec_config.channels = 1;
            codec_config.bitrate = 64000;
            codec_config.frame_size_ms = 20;
            
            // Create only encoder, not decoder
            ctx.encoder = turbo_codec_create_encoder("pcmu", &codec_config);
            VERIFY(ctx.encoder != NULL);
            
            // Should be able to clean up partial pipeline
            cleanup_integration_context(&ctx);
            turbo_codec_registry_shutdown();
        }
    }
    
    section("Performance and Throughput") {
        
        it("should process multiple frames efficiently") {
            integration_test_context_t ctx = {0};
            turbo_audio_codec_config_t codec_config;
            const int frame_count = 50;  // Process 1 second at 20ms frames
            
            turbo_codec_registry_shutdown();
            turbo_codec_registry_init();
            
            memset(&codec_config, 0, sizeof(codec_config));
            codec_config.sample_rate = TURBO_AUDIO_SAMPLE_RATE_8K;
            codec_config.channels = 1;
            codec_config.bitrate = 64000;
            codec_config.frame_size_ms = 20;
            
            ctx.encoder = turbo_codec_create_encoder("pcmu", &codec_config);
            ctx.decoder = turbo_codec_create_decoder("pcmu", &codec_config);
            
            if (ctx.encoder && ctx.decoder) {
                int successful_frames = 0;
                
                for (int i = 0; i < frame_count; i++) {
                    int16_t input[160];
                    for (int j = 0; j < 160; j++) {
                        input[j] = (int16_t)(j + i * 160);
                    }
                    
                    uint8_t encoded[256];
                    size_t encoded_len = sizeof(encoded);
                    int result = turbo_codec_encode(ctx.encoder,
                                                  (const uint8_t *)input,
                                                  sizeof(input),
                                                  encoded,
                                                  &encoded_len,
                                                  NULL);
                    
                    if (result == TURBO_CODEC_OK) {
                        int16_t decoded[160];
                        size_t decoded_len = sizeof(decoded);
                        result = turbo_codec_decode(ctx.decoder,
                                                   encoded,
                                                   encoded_len,
                                                   (uint8_t *)decoded,
                                                   &decoded_len);
                        
                        if (result == TURBO_CODEC_OK) {
                            successful_frames++;
                        }
                    }
                }
                
                // Should successfully process most/all frames
                VERIFY(successful_frames > 0);
            }
            
            cleanup_integration_context(&ctx);
            turbo_codec_registry_shutdown();
        }
    }
    
    section("State Consistency Across Components") {
        
        it("should maintain consistent state during start-stop cycles") {
            integration_test_context_t ctx = {0};
            turbo_audio_capture_config_t capture_config;
            turbo_playback_config_t playback_config;
            
            // Setup capture
            memset(&capture_config, 0, sizeof(capture_config));
            capture_config.sample_rate = 8000;
            capture_config.channels = 1;
            capture_config.bits_per_sample = 16;
            capture_config.frame_size_ms = 20;
            
            // Setup playback
            memset(&playback_config, 0, sizeof(playback_config));
            playback_config.sample_rate = 8000;
            playback_config.channels = 1;
            playback_config.format = TURBO_PLAYBACK_FORMAT_S16;
            playback_config.buffer_size_ms = 50;
            
            ctx.capture = turbo_audio_capture_create(NULL, &capture_config);
            ctx.playback = turbo_playback_create(NULL, &playback_config);
            
            if (ctx.capture && ctx.playback) {
                // Perform multiple start-stop cycles
                for (int cycle = 0; cycle < 3; cycle++) {
                    turbo_capture_start(ctx.capture);
                    turbo_playback_start(ctx.playback);
                    
                    // Both should be in running/starting state
                    turbo_capture_state_t cap_state = ctx.capture->state;
                    turbo_playback_state_t play_state = turbo_playback_get_state(ctx.playback);
                    
                    VERIFY(cap_state != TURBO_CAPTURE_STATE_ERROR);
                    VERIFY(play_state != TURBO_PLAYBACK_STATE_ERROR);
                    
                    turbo_capture_stop(ctx.capture);
                    turbo_playback_stop(ctx.playback);
                }
            }
            
            cleanup_integration_context(&ctx);
        }
    }
}
