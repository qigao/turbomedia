/**
 * TurboMedia Test Helpers Implementation
 * 
 * Implementation of complex test utilities
 */
#include "helpers.h"
#include <stdio.h>
#include <time.h>

/* ============================================================================
 * Test Fixture Management
 * ============================================================================ */

/**
 * Initialize random seed for test data generation
 */
void test_init_random(void) {
    static int initialized = 0;
    if (!initialized) {
        srand((unsigned int)time(NULL));
        initialized = 1;
    }
}

/* ============================================================================
 * Audio Analysis Utilities
 * ============================================================================ */

/**
 * Calculate RMS (Root Mean Square) of audio samples
 */
float test_calculate_rms(const float *samples, size_t sample_count) {
    if (!samples || sample_count == 0) return 0.0f;
    
    double sum_squares = 0.0;
    for (size_t i = 0; i < sample_count; i++) {
        sum_squares += (double)samples[i] * samples[i];
    }
    
    return (float)sqrt(sum_squares / sample_count);
}

/**
 * Calculate RMS for int16 samples
 */
float test_calculate_rms_i16(const int16_t *samples, size_t sample_count) {
    if (!samples || sample_count == 0) return 0.0f;
    
    double sum_squares = 0.0;
    for (size_t i = 0; i < sample_count; i++) {
        double normalized = (double)samples[i] / 32768.0;
        sum_squares += normalized * normalized;
    }
    
    return (float)sqrt(sum_squares / sample_count);
}

/**
 * Detect silence in audio samples (RMS below threshold)
 */
int test_is_silence(const float *samples, size_t sample_count, float threshold) {
    float rms = test_calculate_rms(samples, sample_count);
    return rms < threshold;
}

/**
 * Calculate peak amplitude
 */
float test_find_peak_amplitude(const float *samples, size_t sample_count) {
    if (!samples || sample_count == 0) return 0.0f;
    
    float peak = 0.0f;
    for (size_t i = 0; i < sample_count; i++) {
        float abs_val = fabsf(samples[i]);
        if (abs_val > peak) {
            peak = abs_val;
        }
    }
    
    return peak;
}

/**
 * Check if samples are clipping (exceeding -1.0 to 1.0 range)
 */
int test_has_clipping(const float *samples, size_t sample_count) {
    if (!samples || sample_count == 0) return 0;
    
    for (size_t i = 0; i < sample_count; i++) {
        if (samples[i] > 1.0f || samples[i] < -1.0f) {
            return 1;
        }
    }
    
    return 0;
}

/* ============================================================================
 * Buffer Comparison Utilities
 * ============================================================================ */

/**
 * Compare two audio buffers with tolerance
 * Returns percentage of samples within tolerance
 */
float test_compare_audio_buffers(const float *buffer1, const float *buffer2,
                                 size_t sample_count, float tolerance) {
    if (!buffer1 || !buffer2 || sample_count == 0) return 0.0f;
    
    size_t matches = 0;
    for (size_t i = 0; i < sample_count; i++) {
        if (fabsf(buffer1[i] - buffer2[i]) <= tolerance) {
            matches++;
        }
    }
    
    return (float)matches / sample_count * 100.0f;
}

/**
 * Calculate Mean Squared Error between buffers
 */
float test_calculate_mse(const float *buffer1, const float *buffer2,
                        size_t sample_count) {
    if (!buffer1 || !buffer2 || sample_count == 0) return 0.0f;
    
    double sum_squared_error = 0.0;
    for (size_t i = 0; i < sample_count; i++) {
        double diff = buffer1[i] - buffer2[i];
        sum_squared_error += diff * diff;
    }
    
    return (float)(sum_squared_error / sample_count);
}

/**
 * Calculate Signal-to-Noise Ratio (SNR)
 */
float test_calculate_snr(const float *original, const float *processed,
                        size_t sample_count) {
    if (!original || !processed || sample_count == 0) return 0.0f;
    
    double signal_power = 0.0;
    double noise_power = 0.0;
    
    for (size_t i = 0; i < sample_count; i++) {
        signal_power += (double)original[i] * original[i];
        double noise = original[i] - processed[i];
        noise_power += noise * noise;
    }
    
    if (noise_power == 0.0) return INFINITY;
    
    return (float)(10.0 * log10(signal_power / noise_power));
}

/* ============================================================================
 * File I/O Test Utilities
 * ============================================================================ */

/**
 * Create temporary test file with given content
 */
int test_create_temp_file(const char *filename, const void *data, size_t size) {
    FILE *f = fopen(filename, "wb");
    if (!f) return -1;
    
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    
    return (written == size) ? 0 : -1;
}

/**
 * Read file content into buffer
 */
int test_read_file(const char *filename, void *buffer, size_t max_size,
                  size_t *actual_size) {
    FILE *f = fopen(filename, "rb");
    if (!f) return -1;
    
    size_t read_size = fread(buffer, 1, max_size, f);
    fclose(f);
    
    if (actual_size) {
        *actual_size = read_size;
    }
    
    return 0;
}

/**
 * Delete temporary test file
 */
void test_delete_temp_file(const char *filename) {
    remove(filename);
}

/* ============================================================================
 * Timing and Performance Utilities
 * ============================================================================ */

/**
 * Start timer
 */
void test_timer_start(test_timer_t *timer) {
    if (timer) {
        timer->start_time = clock();
    }
}

/**
 * Stop timer and return elapsed milliseconds
 */
double test_timer_stop(test_timer_t *timer) {
    if (!timer) return 0.0;
    
    timer->end_time = clock();
    double elapsed = (double)(timer->end_time - timer->start_time) / CLOCKS_PER_SEC;
    return elapsed * 1000.0;  // Convert to milliseconds
}

/**
 * Get elapsed time without stopping timer
 */
double test_timer_elapsed(const test_timer_t *timer) {
    if (!timer) return 0.0;
    
    clock_t current = clock();
    double elapsed = (double)(current - timer->start_time) / CLOCKS_PER_SEC;
    return elapsed * 1000.0;  // Convert to milliseconds
}

/* ============================================================================
 * Test Data Validation
 * ============================================================================ */

/**
 * Validate audio configuration parameters
 */
int test_validate_audio_config(int sample_rate, int channels, int bits_per_sample) {
    // Check sample rate
    if (sample_rate != 8000 && sample_rate != 16000 && 
        sample_rate != 24000 && sample_rate != 48000) {
        return 0;
    }
    
    // Check channels
    if (channels < 1 || channels > 8) {
        return 0;
    }
    
    // Check bits per sample
    if (bits_per_sample != 16 && bits_per_sample != 32) {
        return 0;
    }
    
    return 1;
}

/**
 * Validate video configuration parameters
 */
int test_validate_video_config(int width, int height, int framerate) {
    // Check dimensions
    if (width <= 0 || width > 3840 || height <= 0 || height > 2160) {
        return 0;
    }
    
    // Check framerate
    if (framerate <= 0 || framerate > 120) {
        return 0;
    }
    
    return 1;
}

/**
 * Calculate expected buffer size for audio
 */
size_t test_calculate_audio_buffer_size(int sample_rate, int channels,
                                       int bits_per_sample, int duration_ms) {
    size_t samples_per_channel = (size_t)sample_rate * duration_ms / 1000;
    size_t bytes_per_sample = bits_per_sample / 8;
    return samples_per_channel * channels * bytes_per_sample;
}

/**
 * Calculate expected frame size for I420 video
 */
size_t test_calculate_i420_frame_size(int width, int height) {
    return (size_t)width * height * 3 / 2;  // Y + U/4 + V/4
}

/* ============================================================================
 * Memory Testing Utilities
 * ============================================================================ */

/**
 * Fill memory with pattern for leak detection
 */
void test_fill_pattern(void *buffer, size_t size, uint8_t pattern) {
    if (buffer) {
        memset(buffer, pattern, size);
    }
}

/**
 * Verify memory pattern (for leak detection)
 */
int test_verify_pattern(const void *buffer, size_t size, uint8_t pattern) {
    if (!buffer) return 0;
    
    const uint8_t *bytes = (const uint8_t *)buffer;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] != pattern) {
            return 0;
        }
    }
    
    return 1;
}

/**
 * Simple allocation counter for memory leak detection
 */
static int g_allocation_count = 0;

void test_reset_allocation_counter(void) {
    g_allocation_count = 0;
}

void test_increment_allocation_counter(void) {
    g_allocation_count++;
}

void test_decrement_allocation_counter(void) {
    g_allocation_count--;
}

int test_get_allocation_count(void) {
    return g_allocation_count;
}
