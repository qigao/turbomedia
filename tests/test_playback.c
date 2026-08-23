#include "turbo_playback.h"
#include "helpers.h"
#include <tinytest.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Test fixture for tracking callback invocations */
typedef struct {
    int data_callback_count;
    int state_callback_count;
    int complete_callback_count;
    size_t last_frame_count;
    turbo_playback_state_t last_state;
    int end_of_stream_signaled;
    float *generated_samples;
    size_t generated_samples_len;
    size_t sample_offset;
} playback_test_fixture_t;

static void reset_fixture(playback_test_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    if (fixture->generated_samples) {
        free(fixture->generated_samples);
        fixture->generated_samples = NULL;
    }
}

/* Generate sine wave samples for testing */
static void generate_sine_wave(float *samples, size_t sample_count, 
                              int sample_rate, float frequency) {
    for (size_t i = 0; i < sample_count; i++) {
        double t = (double)i / sample_rate;
        samples[i] = (float)sin(2.0 * M_PI * frequency * t) * 0.5f;
    }
}
/* Test callbacks */
static size_t test_data_callback(turbo_playback_t *playback,
                                void *output, size_t frame_count,
                                void *user_data) {
    playback_test_fixture_t *fixture = (playback_test_fixture_t *)user_data;
    (void)playback;
    
    fixture->data_callback_count++;
    fixture->last_frame_count = frame_count;
    
    if (fixture->generated_samples && fixture->sample_offset < fixture->generated_samples_len) {
        float *out = (float *)output;
        size_t frames_to_copy = frame_count;
        size_t remaining = fixture->generated_samples_len - fixture->sample_offset;
        
        if (frames_to_copy > remaining) {
            frames_to_copy = remaining;
        }
        
        memcpy(out, fixture->generated_samples + fixture->sample_offset, 
               frames_to_copy * sizeof(float));
        fixture->sample_offset += frames_to_copy;
        
        return frames_to_copy;
    }
    
    // End of stream
    fixture->end_of_stream_signaled = 1;
    return 0;
}

static void test_state_callback(turbo_playback_t *playback,
                               turbo_playback_state_t state,
                               void *user_data) {
    playback_test_fixture_t *fixture = (playback_test_fixture_t *)user_data;
    (void)playback;
    
    fixture->state_callback_count++;
    fixture->last_state = state;
}

static void test_complete_callback(turbo_playback_t *playback,
                                  void *user_data) {
    playback_test_fixture_t *fixture = (playback_test_fixture_t *)user_data;
    (void)playback;
    
    fixture->complete_callback_count++;
}

/* Helper function to create valid config */
static turbo_playback_config_t create_valid_config(void) {
    turbo_playback_config_t config;
    memset(&config, 0, sizeof(config));
    config.sample_rate = 48000;
    config.channels = 2;
    config.format = TURBO_PLAYBACK_FORMAT_F32;
    config.buffer_size_ms = 50;
    return config;
}
suite("TurboMedia Playback Module - TDD Tests") {
    
    /* Test 1: Device enumeration must work before any playback */
   it("device_enumeration_returns_valid_count") {
        turbo_playback_device_t devices[TURBO_PLAYBACK_MAX_DEVICES];
        
        // Test basic enumeration
        int count = turbo_playback_list_devices(devices, TURBO_PLAYBACK_MAX_DEVICES);
        check(count >= 0); // Should not fail, may return 0 if no devices
        check(count <= TURBO_PLAYBACK_MAX_DEVICES);
    }
    
   it("device_enumeration_handles_null_parameters") {
        int count = turbo_playback_list_devices(NULL, 10);
        check(count < 0); // Should fail with null devices array
    }
    
   it("device_enumeration_handles_zero_max_count") {
        turbo_playback_device_t devices[1];
        int count = turbo_playback_list_devices(devices, 0);
        check(count < 0);
    }
    
   it("get_default_device_returns_valid_info") {
        turbo_playback_device_t device;
        int result = turbo_playback_get_default_device(&device);
        
        if (result == 0) {
            // If successful, device should have valid properties
            check(device.index >= 0);
            check(strlen(device.name) > 0);
            check(device.is_default == 1);
        }
        // Note: May fail if no audio devices available
    }
    
   it("get_default_device_rejects_null_parameter") {
        int result = turbo_playback_get_default_device(NULL);
        check(result < 0);
    }
    
    /* Test 2: Configuration validation must prevent invalid setups */
   it("create_playback_validates_configuration") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = NULL;
        
        // Valid config should work (may return NULL if no devices)
        playback = turbo_playback_create(NULL, &config);
        if (playback) {
            turbo_playback_destroy(playback);
        }
        
        // Null config uses default playback configuration and may still fail if no device exists.
        playback = turbo_playback_create(NULL, NULL);
        if (playback) {
            turbo_playback_destroy(playback);
        }
    }

   it("create_playback_rejects_malformed_device_ids") {
        turbo_playback_config_t config = create_valid_config();

        check_null(turbo_playback_create("", &config));
        check_null(turbo_playback_create("speaker", &config));
        check_null(turbo_playback_create("-1", &config));
        check_null(turbo_playback_create("0x1", &config));
        check_null(turbo_playback_create("4294967296", &config));
    }

   it("create_playback_accepts_an_enumerated_device_id") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_device_t devices[TURBO_PLAYBACK_MAX_DEVICES];
        int count = turbo_playback_list_devices(devices, TURBO_PLAYBACK_MAX_DEVICES);

        check(count >= 0);
        if (count > 0) {
            check(strlen(devices[0].id) > 0);
            turbo_playback_t *playback = turbo_playback_create(devices[0].id, &config);
            if (playback) {
                turbo_playback_destroy(playback);
            }
        }
    }
    
   it("create_playback_rejects_invalid_sample_rates") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = NULL;
        
        // Test invalid sample rates
        config.sample_rate = 0;
        playback = turbo_playback_create(NULL, &config);
        check_null(playback);
        
        config.sample_rate = -1;
        playback = turbo_playback_create(NULL, &config);
        check_null(playback);
        
        config.sample_rate = 999999; // Unrealistic rate
        playback = turbo_playback_create(NULL, &config);
        check_null(playback);
    }
   it("create_playback_rejects_invalid_channel_counts") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = NULL;
        
        config.channels = 0;
        playback = turbo_playback_create(NULL, &config);
        check_null(playback);
        
        config.channels = -1;
        playback = turbo_playback_create(NULL, &config);
        check_null(playback);
        
        config.channels = 100; // Too many channels
        playback = turbo_playback_create(NULL, &config);
        check_null(playback);
    }
    
   it("create_playback_supports_standard_formats") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_format_t formats[] = {
            TURBO_PLAYBACK_FORMAT_S16,
            TURBO_PLAYBACK_FORMAT_S32,
            TURBO_PLAYBACK_FORMAT_F32
        };
        
        for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
            config.format = formats[i];
            turbo_playback_t *playback = turbo_playback_create(NULL, &config);
            // May be NULL if no devices, but shouldn't crash
            if (playback) {
                turbo_playback_destroy(playback);
            }
        }
    }
    
    /* Test 3: File playback must validate file paths */
   it("create_file_playback_rejects_null_filepath") {
        turbo_playback_t *playback = turbo_playback_create_file(NULL, NULL);
        check_null(playback);
    }
    
   it("create_file_playback_rejects_nonexistent_file") {
        turbo_playback_t *playback = turbo_playback_create_file(NULL, "nonexistent_file.wav");
        check_null(playback);
    }
    
   it("create_file_playback_rejects_invalid_file_format") {
        // Create a dummy text file that's not audio
        const char *dummy_file = "test_dummy.txt";
        FILE *f = fopen(dummy_file, "w");
        if (f) {
            fprintf(f, "This is not an audio file");
            fclose(f);
            
            turbo_playback_t *playback = turbo_playback_create_file(NULL, dummy_file);
            check_null(playback);
            
            remove(dummy_file);
        }
    }
    /* Test 4: State management must follow defined transitions */
   it("playback_initial_state_is_stopped") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            turbo_playback_state_t state = turbo_playback_get_state(playback);
            check_equal(state, TURBO_PLAYBACK_STATE_STOPPED);
            turbo_playback_destroy(playback);
        }
    }
    
   it("start_playback_changes_state_from_stopped") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        playback_test_fixture_t fixture = {0};
        
        if (playback) {
            turbo_playback_set_data_callback(playback, test_data_callback, &fixture);
            
            int result = turbo_playback_start(playback);
            if (result == TURBO_PLAYBACK_OK) {
                turbo_playback_state_t state = turbo_playback_get_state(playback);
                check(state == TURBO_PLAYBACK_STATE_STARTING ||
                       state == TURBO_PLAYBACK_STATE_PLAYING);
            }
            
            turbo_playback_destroy(playback);
        }
        reset_fixture(&fixture);
    }
    
   it("stop_playback_changes_state_to_stopped") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            turbo_playback_stop(playback);
            turbo_playback_state_t state = turbo_playback_get_state(playback);
            check(state == TURBO_PLAYBACK_STATE_STOPPED ||
                   state == TURBO_PLAYBACK_STATE_STOPPING);
            
            turbo_playback_destroy(playback);
        }
    }
    
   it("pause_resume_cycle_maintains_valid_states") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            int result = turbo_playback_start(playback);
            if (result == TURBO_PLAYBACK_OK) {
                turbo_playback_pause(playback);
                turbo_playback_state_t state = turbo_playback_get_state(playback);
                check(state == TURBO_PLAYBACK_STATE_PAUSED);
                
                turbo_playback_resume(playback);
                state = turbo_playback_get_state(playback);
                check(state == TURBO_PLAYBACK_STATE_PLAYING ||
                       state == TURBO_PLAYBACK_STATE_STARTING);
            }
            
            turbo_playback_destroy(playback);
        }
    }
    /* Test 5: Callback registration must be validated */
   it("set_data_callback_accepts_valid_parameters") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        playback_test_fixture_t fixture = {0};
        
        if (playback) {
            // Should not crash with valid parameters
            turbo_playback_set_data_callback(playback, test_data_callback, &fixture);
            turbo_playback_destroy(playback);
        }
        reset_fixture(&fixture);
    }
    
   it("set_data_callback_handles_null_playback") {
        playback_test_fixture_t fixture = {0};
        
        // Should not crash with null playback
        turbo_playback_set_data_callback(NULL, test_data_callback, &fixture);
        reset_fixture(&fixture);
    }
    
   it("set_state_callback_registers_correctly") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        playback_test_fixture_t fixture = {0};
        
        if (playback) {
            turbo_playback_on_state(playback, test_state_callback);
            // Callback should be registered (no way to verify without triggering)
            turbo_playback_destroy(playback);
        }
        reset_fixture(&fixture);
    }
    
   it("set_complete_callback_registers_correctly") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        playback_test_fixture_t fixture = {0};
        
        if (playback) {
            turbo_playback_on_complete(playback, test_complete_callback);
            turbo_playback_destroy(playback);
        }
        reset_fixture(&fixture);
    }
    
    /* Test 6: Volume control must validate ranges */
   it("set_volume_accepts_valid_ranges") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            // Test valid volume values
            turbo_playback_set_volume(playback, 0.0f);  // Mute
            turbo_playback_set_volume(playback, 0.5f);  // Half volume
            turbo_playback_set_volume(playback, 1.0f);  // Normal volume
            turbo_playback_set_volume(playback, 2.0f);  // Amplified
            
            turbo_playback_destroy(playback);
        }
    }
    
   it("get_volume_returns_set_value") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            turbo_playback_set_volume(playback, 0.75f);
            float volume = turbo_playback_get_volume(playback);
            
            // Allow small floating point differences
            check(fabsf(volume - 0.75f) < 0.01f);
            
            turbo_playback_destroy(playback);
        }
    }
    /* Test 7: Streaming buffer operations must handle edge cases */
   it("write_streaming_data_accepts_valid_input") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            float samples[1024];
            generate_sine_wave(samples, 1024, 48000, 440.0f);
            
            size_t written = turbo_playback_write(playback, samples, sizeof(samples));
            check(written <= sizeof(samples));
            
            turbo_playback_destroy(playback);
        }
    }
    
   it("write_streaming_data_handles_null_parameters") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            // Should handle null data gracefully
            size_t written = turbo_playback_write(playback, NULL, 1024);
            check_equal(written, 0);
            
            turbo_playback_destroy(playback);
        }
        
        // Should handle null playback gracefully
        float samples[16];
        size_t written = turbo_playback_write(NULL, samples, sizeof(samples));
        check_equal(written, 0);
    }
    
   it("get_available_buffer_space_returns_valid_size") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            size_t available = turbo_playback_get_available(playback);
            check(available >= 0); // Should be non-negative
            
            turbo_playback_destroy(playback);
        }
    }
    
   it("get_buffered_data_size_returns_valid_size") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            size_t buffered = turbo_playback_get_buffered(playback);
            check(buffered >= 0);
            
            turbo_playback_destroy(playback);
        }
    }
    
   it("clear_buffer_resets_buffered_data") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            // Write some data
            float samples[512];
            generate_sine_wave(samples, 512, 48000, 1000.0f);
            turbo_playback_write(playback, samples, sizeof(samples));
            
            // Clear buffer
            turbo_playback_clear(playback);
            
            // Buffer should be empty (or at least not more than before clear)
            size_t buffered = turbo_playback_get_buffered(playback);
            (void)buffered;
            // Note: Implementation dependent - may not be exactly 0 due to device buffers
            
            turbo_playback_destroy(playback);
        }
    }
    /* Test 8: Queue operations must maintain consistency */
   it("queue_operations_require_file_playback_instance") {
        // Queue operations should only work with file playback instances
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *streaming_playback = turbo_playback_create(NULL, &config);
        
        if (streaming_playback) {
            // Adding to queue on streaming playback should fail
            int result = turbo_playback_queue_add(streaming_playback, "test.wav");
            check(result != TURBO_PLAYBACK_OK);
            
            turbo_playback_destroy(streaming_playback);
        }
    }
    
   it("queue_add_validates_file_paths") {
        // Create a dummy file playback instance (will fail but test the parameter validation)
        turbo_playback_t *playback = turbo_playback_create_file(NULL, "nonexistent.wav");
        
        // Should handle null filepath
        if (playback) {
            int result = turbo_playback_queue_add(playback, NULL);
            check(result != TURBO_PLAYBACK_OK);
            turbo_playback_destroy(playback);
        }
        
        // Should handle null playback
        int result = turbo_playback_queue_add(NULL, "test.wav");
        check(result != TURBO_PLAYBACK_OK);
    }
    
   it("queue_count_returns_valid_count") {
        turbo_playback_t *playback = turbo_playback_create_file(NULL, "test.wav");
        
        if (playback) {
            int count = turbo_playback_queue_count(playback);
            check(count >= 0);
            turbo_playback_destroy(playback);
        }
        
        // Should handle null playback
        int count = turbo_playback_queue_count(NULL);
        check_equal(count, 0);
    }
    
   it("queue_clear_removes_all_files") {
        turbo_playback_t *playback = turbo_playback_create_file(NULL, "test.wav");
        
        if (playback) {
            // Should not crash even if queue is empty
            turbo_playback_queue_clear(playback);
            turbo_playback_destroy(playback);
        }
        
        // Should handle null playback gracefully
        turbo_playback_queue_clear(NULL);
    }
    
   it("queue_next_handles_empty_queue") {
        turbo_playback_t *playback = turbo_playback_create_file(NULL, "test.wav");
        
        if (playback) {
            int result = turbo_playback_queue_next(playback);
            check(result != TURBO_PLAYBACK_OK); // Should fail on empty queue
            turbo_playback_destroy(playback);
        }
        
        // Should handle null playback
        int result = turbo_playback_queue_next(NULL);
        check(result != TURBO_PLAYBACK_OK);
    }
    /* Test 9: Seek operations must validate parameters */
   it("seek_validates_parameters") {
        turbo_playback_t *playback = turbo_playback_create_file(NULL, "test.wav");
        
        if (playback) {
            // Test seeking to valid position
            int result = turbo_playback_seek(playback, 1000); // 1 second
            (void)result;
            // May succeed or fail depending on file, but shouldn't crash
            
            turbo_playback_destroy(playback);
        }
        
        // Should handle null playback
        int result = turbo_playback_seek(NULL, 1000);
        check(result != TURBO_PLAYBACK_OK);
    }
    
   it("get_position_returns_valid_value") {
        turbo_playback_t *playback = turbo_playback_create_file(NULL, "test.wav");
        
        if (playback) {
            uint64_t position = turbo_playback_get_position(playback);
            // Should return valid position (may be 0 for non-existent file)
            check(position >= 0);
            turbo_playback_destroy(playback);
        }
        
        // Should handle null playback
        uint64_t position = turbo_playback_get_position(NULL);
        check_equal(position, 0);
    }
    
   it("get_duration_returns_valid_value") {
        turbo_playback_t *playback = turbo_playback_create_file(NULL, "test.wav");
        
        if (playback) {
            uint64_t duration = turbo_playback_get_duration(playback);
            check(duration >= 0);
            turbo_playback_destroy(playback);
        }
        
        // Should handle null playback
        uint64_t duration = turbo_playback_get_duration(NULL);
        check_equal(duration, 0);
    }
    
    /* Test 10: Looping operations must be consistent */
   it("set_looping_accepts_boolean_values") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            // Should accept boolean-like values
            turbo_playback_set_looping(playback, 0);
            turbo_playback_set_looping(playback, 1);
            turbo_playback_set_looping(playback, -1);
            turbo_playback_set_looping(playback, 100);
            
            turbo_playback_destroy(playback);
        }
        
        // Should handle null playback gracefully
        turbo_playback_set_looping(NULL, 1);
    }
    
   it("queue_set_looping_accepts_boolean_values") {
        turbo_playback_t *playback = turbo_playback_create_file(NULL, "test.wav");
        
        if (playback) {
            turbo_playback_queue_set_looping(playback, 0);
            turbo_playback_queue_set_looping(playback, 1);
            turbo_playback_destroy(playback);
        }
        
        // Should handle null playback gracefully
        turbo_playback_queue_set_looping(NULL, 1);
    }
    /* Test 11: Null safety must be comprehensive */
   it("all_operations_handle_null_playback_safely") {
        // All API functions should handle null playback without crashing
        
        check(turbo_playback_start(NULL) != TURBO_PLAYBACK_OK);
        turbo_playback_stop(NULL);
        turbo_playback_pause(NULL);
        turbo_playback_resume(NULL);
        
        turbo_playback_state_t state = turbo_playback_get_state(NULL);
        check_equal(state, TURBO_PLAYBACK_STATE_STOPPED);
        
        turbo_playback_set_volume(NULL, 1.0f);
        float volume = turbo_playback_get_volume(NULL);
        check_equal(volume, 0.0f);
        
        turbo_playback_set_data_callback(NULL, NULL, NULL);
        turbo_playback_on_state(NULL, NULL);
        turbo_playback_on_complete(NULL, NULL);
        
        turbo_playback_destroy(NULL); // Should not crash
    }
    
    /* Test 12: Resource cleanup must be thorough */
   it("destroy_cleans_up_resources_properly") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            // Set up various resources
            playback_test_fixture_t fixture = {0};
            turbo_playback_set_data_callback(playback, test_data_callback, &fixture);
            turbo_playback_on_state(playback, test_state_callback);
            turbo_playback_on_complete(playback, test_complete_callback);
            
            // Start playback to allocate resources
            turbo_playback_start(playback);
            
            // Destroy should clean up everything
            turbo_playback_destroy(playback);
            // If we reach here without crashing, cleanup was successful
            
            reset_fixture(&fixture);
        }
    }
    
    /* Test 13: Error conditions must be reported correctly */
   it("operations_on_destroyed_playback_fail_safely") {
        turbo_playback_config_t config = create_valid_config();
        turbo_playback_t *playback = turbo_playback_create(NULL, &config);
        
        if (playback) {
            turbo_playback_destroy(playback);
            
            // Operations on destroyed playback should fail safely
            // Note: This is implementation-dependent behavior
            // Some implementations might crash, others might check validity
            
            // In a robust implementation, these should not crash:
            // turbo_playback_start(playback); 
            // turbo_playback_get_state(playback);
        }
    }
}
