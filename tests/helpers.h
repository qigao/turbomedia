/**
 * TurboMedia Test Helpers
 * 
 * Common utilities, fixtures, and helper functions for TDD/BDD tests
 */
#ifndef TURBOMEDIA_TEST_HELPERS_H
#define TURBOMEDIA_TEST_HELPERS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

/* 定义 M_PI，MSVC 需要 _USE_MATH_DEFINES 或手动定义 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Test Assertion Macros
 * ============================================================================ */

/**
 * Assert that condition is true, fail test otherwise
 */
#define TEST_ASSERT(condition) \
    do { \
        check(condition); \
        if (!(condition)) { \
            return; \
        } \
    } while (0)

/**
 * Assert that two integers are equal
 */
#define TEST_ASSERT_EQ(expected, actual) \
    do { \
        int exp_val = (expected); \
        int act_val = (actual); \
        check_int_eq(act_val, exp_val); \
        if (exp_val != act_val) { \
            return; \
        } \
    } while (0)

/**
 * Assert that pointer is not NULL
 */
#define TEST_ASSERT_NOT_NULL(ptr) \
    do { \
        check((ptr) != NULL); \
        if ((ptr) == NULL) { \
            return; \
        } \
    } while (0)

/**
 * Assert that pointer is NULL
 */
#define TEST_ASSERT_NULL(ptr) \
    do { \
        check((ptr) == NULL); \
        if ((ptr) != NULL) { \
            return; \
        } \
    } while (0)

/**
 * Assert that strings are equal
 */
#define TEST_ASSERT_STR_EQ(expected, actual) \
    do { \
        check_str_eq(actual, expected); \
        if (strcmp(actual, expected) != 0) { \
            return; \
        } \
    } while (0)

/**
 * Assert that memory regions are equal
 */
#define TEST_ASSERT_MEM_EQ(expected, actual, size) \
    do { \
        check_mem_eq(actual, expected, size); \
        if (memcmp(actual, expected, size) != 0) { \
            return; \
        } \
    } while (0)

/**
 * Assert that value is within range [min, max]
 */
#define TEST_ASSERT_IN_RANGE(value, min, max) \
    do { \
        int val = (value); \
        int min_val = (min); \
        int max_val = (max); \
        check(val >= min_val && val <= max_val); \
        if (val < min_val || val > max_val) { \
            return; \
        } \
    } while (0)

/**
 * Assert that floating point values are approximately equal
 */
#define TEST_ASSERT_FLOAT_EQ(expected, actual, epsilon) \
    do { \
        float exp = (expected); \
        float act = (actual); \
        float eps = (epsilon); \
        check(fabsf(act - exp) < eps); \
        if (fabsf(act - exp) >= eps) { \
            return; \
        } \
    } while (0)

/* ============================================================================
 * Test Fixture Management
 * ============================================================================ */

/**
 * Initialize random seed for test data generation
 */
void test_init_random(void);

/* ============================================================================
 * Audio Analysis Utilities
 * ============================================================================ */

/**
 * Calculate RMS (Root Mean Square) of audio samples
 */
float test_calculate_rms(const float *samples, size_t sample_count);

/**
 * Calculate RMS for int16 samples
 */
float test_calculate_rms_i16(const int16_t *samples, size_t sample_count);

/**
 * Detect silence in audio samples (RMS below threshold)
 */
int test_is_silence(const float *samples, size_t sample_count, float threshold);

/**
 * Calculate peak amplitude
 */
float test_find_peak_amplitude(const float *samples, size_t sample_count);

/**
 * Check if samples are clipping (exceeding -1.0 to 1.0 range)
 */
int test_has_clipping(const float *samples, size_t sample_count);

/* ============================================================================
 * Buffer Comparison Utilities
 * ============================================================================ */

/**
 * Compare two audio buffers with tolerance
 * Returns percentage of samples within tolerance
 */
float test_compare_audio_buffers(const float *buffer1, const float *buffer2,
                                 size_t sample_count, float tolerance);

/**
 * Calculate Mean Squared Error between buffers
 */
float test_calculate_mse(const float *buffer1, const float *buffer2,
                        size_t sample_count);

/**
 * Calculate Signal-to-Noise Ratio (SNR)
 */
float test_calculate_snr(const float *original, const float *processed,
                        size_t sample_count);

/* ============================================================================
 * File I/O Test Utilities
 * ============================================================================ */

/**
 * Create temporary test file with given content
 */
int test_create_temp_file(const char *filename, const void *data, size_t size);

/**
 * Read file content into buffer
 */
int test_read_file(const char *filename, void *buffer, size_t max_size,
                  size_t *actual_size);

/**
 * Delete temporary test file
 */
void test_delete_temp_file(const char *filename);

/* ============================================================================
 * Timing and Performance Utilities
 * ============================================================================ */

/**
 * Timer structure for performance measurements
 */
typedef struct {
    clock_t start_time;
    clock_t end_time;
} test_timer_t;

/**
 * Start timer
 */
void test_timer_start(test_timer_t *timer);

/**
 * Stop timer and return elapsed milliseconds
 */
double test_timer_stop(test_timer_t *timer);

/**
 * Get elapsed time without stopping timer
 */
double test_timer_elapsed(const test_timer_t *timer);

/* ============================================================================
 * Test Data Validation
 * ============================================================================ */

/**
 * Validate audio configuration parameters
 */
int test_validate_audio_config(int sample_rate, int channels, int bits_per_sample);

/**
 * Validate video configuration parameters
 */
int test_validate_video_config(int width, int height, int framerate);

/**
 * Calculate expected buffer size for audio
 */
size_t test_calculate_audio_buffer_size(int sample_rate, int channels,
                                       int bits_per_sample, int duration_ms);

/**
 * Calculate expected frame size for I420 video
 */
size_t test_calculate_i420_frame_size(int width, int height);

/* ============================================================================
 * Memory Testing Utilities
 * ============================================================================ */

/**
 * Fill memory with pattern for leak detection
 */
void test_fill_pattern(void *buffer, size_t size, uint8_t pattern);

/**
 * Verify memory pattern (for leak detection)
 */
int test_verify_pattern(const void *buffer, size_t size, uint8_t pattern);

/**
 * Simple allocation counter for memory leak detection
 */
void test_reset_allocation_counter(void);
void test_increment_allocation_counter(void);
void test_decrement_allocation_counter(void);
int test_get_allocation_count(void);

/* ============================================================================
 * Audio Test Data Generators
 * ============================================================================ */

/**
 * Generate sine wave samples
 * 
 * @param samples       Output buffer for samples
 * @param sample_count  Number of samples to generate
 * @param sample_rate   Sample rate in Hz
 * @param frequency     Sine wave frequency in Hz
 * @param amplitude     Peak amplitude (0.0 to 1.0)
 */
static inline void test_generate_sine_wave(float *samples, size_t sample_count,
                                           int sample_rate, float frequency,
                                           float amplitude) {
    for (size_t i = 0; i < sample_count; i++) {
        double t = (double)i / sample_rate;
        samples[i] = (float)(amplitude * sin(2.0 * M_PI * frequency * t));
    }
}

/**
 * Generate sine wave in int16 format
 */
static inline void test_generate_sine_wave_i16(int16_t *samples, size_t sample_count,
                                               int sample_rate, float frequency,
                                               float amplitude) {
    for (size_t i = 0; i < sample_count; i++) {
        double t = (double)i / sample_rate;
        float value = (float)(amplitude * sin(2.0 * M_PI * frequency * t));
        samples[i] = (int16_t)(value * 32767.0f);
    }
}

/**
 * Generate silence (zeros)
 */
static inline void test_generate_silence(void *samples, size_t size) {
    memset(samples, 0, size);
}

/**
 * Generate white noise
 */
static inline void test_generate_white_noise(float *samples, size_t sample_count,
                                             float amplitude) {
    for (size_t i = 0; i < sample_count; i++) {
        float random_val = (float)rand() / RAND_MAX * 2.0f - 1.0f;
        samples[i] = random_val * amplitude;
    }
}

/**
 * Generate square wave
 */
static inline void test_generate_square_wave(float *samples, size_t sample_count,
                                             int sample_rate, float frequency,
                                             float amplitude) {
    for (size_t i = 0; i < sample_count; i++) {
        double t = (double)i / sample_rate;
        float phase = fmodf((float)(frequency * t), 1.0f);
        samples[i] = (phase < 0.5f) ? amplitude : -amplitude;
    }
}

/* ============================================================================
 * Video Test Data Generators
 * ============================================================================ */

/**
 * Generate I420 test pattern (gradient)
 */
static inline void test_generate_i420_gradient(uint8_t *frame, int width, int height) {
    int y_size = width * height;
    int uv_size = (width * height) / 4;
    
    // Y plane - horizontal gradient
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            frame[y * width + x] = (uint8_t)((x * 255) / width);
        }
    }
    
    // U plane - constant blue
    memset(frame + y_size, 128, uv_size);
    
    // V plane - constant red
    memset(frame + y_size + uv_size, 128, uv_size);
}

/**
 * Generate solid color I420 frame
 */
static inline void test_generate_i420_solid(uint8_t *frame, int width, int height,
                                           uint8_t y, uint8_t u, uint8_t v) {
    int y_size = width * height;
    int uv_size = (width * height) / 4;
    
    memset(frame, y, y_size);
    memset(frame + y_size, u, uv_size);
    memset(frame + y_size + uv_size, v, uv_size);
}

/**
 * Generate checkerboard pattern
 */
static inline void test_generate_i420_checkerboard(uint8_t *frame, int width, int height,
                                                   int square_size) {
    int y_size = width * height;
    int uv_size = (width * height) / 4;
    
    // Y plane - checkerboard
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int checker = ((x / square_size) + (y / square_size)) % 2;
            frame[y * width + x] = checker ? 255 : 0;
        }
    }
    
    // U and V planes - gray
    memset(frame + y_size, 128, uv_size);
    memset(frame + y_size + uv_size, 128, uv_size);
}

#ifdef __cplusplus
}
#endif

#endif /* TURBOMEDIA_TEST_HELPERS_H */
