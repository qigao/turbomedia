/**
 * TurboMedia Performance Benchmark Tests
 * 
 * 性能基准测试套件，测量关键操作的吞吐量、延迟和资源使用
 * 
 * 测试覆盖：
 * - 音频编解码性能
 * - 视频编解码性能
 * - 捕获设备性能
 * - 播放设备性能
 * - 内存分配性能
 * - 端到端管道性能
 */

#include "helpers.h"
#include <turbo_codec.h>
#include <salts_capture.h>
#include <salts_playback.h>
#include <tinytest.h>
#include <stdio.h>
#include <time.h>

/* ============================================================================
 * 性能测试配置
 * ============================================================================ */

#define BENCHMARK_ITERATIONS 1000
#define BENCHMARK_WARM_UP 100
#define BENCHMARK_AUDIO_DURATION_MS 10
#define BENCHMARK_VIDEO_WIDTH 1280
#define BENCHMARK_VIDEO_HEIGHT 720
#define BENCHMARK_VIDEO_FPS 30

/* 性能阈值（微秒） */
#define PERF_THRESHOLD_AUDIO_ENCODE_US 500
#define PERF_THRESHOLD_AUDIO_DECODE_US 300
#define PERF_THRESHOLD_VIDEO_ENCODE_US 5000
#define PERF_THRESHOLD_VIDEO_DECODE_US 3000

/* ============================================================================
 * 基准测试辅助函数
 * ============================================================================ */

/**
 * 测量操作耗时（微秒）
 */
static double measure_operation_us(void (*operation)(void *), void *context) {
    clock_t start = clock();
    operation(context);
    clock_t end = clock();
    return ((double)(end - start) / CLOCKS_PER_SEC) * 1000000.0;
}

/**
 * 运行基准测试并报告统计信息
 */
typedef struct {
    double min_us;
    double max_us;
    double avg_us;
    double median_us;
    double p95_us;
    double p99_us;
    double throughput;  // 操作/秒
} benchmark_stats_t;

static void run_benchmark(const char *name, 
                         void (*operation)(void *), 
                         void *context,
                         int iterations,
                         benchmark_stats_t *stats) {
    double *timings = malloc(sizeof(double) * iterations);
    if (!timings) return;
    
    // 预热
    for (int i = 0; i < BENCHMARK_WARM_UP; i++) {
        operation(context);
    }
    
    // 实际测量
    for (int i = 0; i < iterations; i++) {
        timings[i] = measure_operation_us(operation, context);
    }
    
    // 计算统计信息
    double sum = 0.0;
    stats->min_us = timings[0];
    stats->max_us = timings[0];
    
    for (int i = 0; i < iterations; i++) {
        sum += timings[i];
        if (timings[i] < stats->min_us) stats->min_us = timings[i];
        if (timings[i] > stats->max_us) stats->max_us = timings[i];
    }
    
    stats->avg_us = sum / iterations;
    stats->throughput = 1000000.0 / stats->avg_us;
    
    // 排序计算百分位数
    for (int i = 0; i < iterations - 1; i++) {
        for (int j = i + 1; j < iterations; j++) {
            if (timings[j] < timings[i]) {
                double temp = timings[i];
                timings[i] = timings[j];
                timings[j] = temp;
            }
        }
    }
    
    stats->median_us = timings[iterations / 2];
    stats->p95_us = timings[(int)(iterations * 0.95)];
    stats->p99_us = timings[(int)(iterations * 0.99)];
    
    // 打印报告
    printf("\n=== Benchmark: %s ===\n", name);
    printf("  Iterations: %d\n", iterations);
    printf("  Min:        %.2f µs\n", stats->min_us);
    printf("  Max:        %.2f µs\n", stats->max_us);
    printf("  Avg:        %.2f µs\n", stats->avg_us);
    printf("  Median:     %.2f µs\n", stats->median_us);
    printf("  P95:        %.2f µs\n", stats->p95_us);
    printf("  P99:        %.2f µs\n", stats->p99_us);
    printf("  Throughput: %.2f ops/sec\n", stats->throughput);
    
    free(timings);
}

/* ============================================================================
 * 音频编解码基准测试
 * ============================================================================ */

typedef struct {
    turbo_codec_t *encoder;
    turbo_codec_t *decoder;
    uint8_t *input_buffer;
    size_t input_size;
    uint8_t *output_buffer;
    size_t output_size;
    size_t encoded_size;
} audio_codec_bench_ctx_t;

static void bench_audio_encode_op(void *context) {
    audio_codec_bench_ctx_t *ctx = (audio_codec_bench_ctx_t *)context;
    size_t encoded_size = ctx->output_size;
    turbo_codec_encode(ctx->encoder, ctx->input_buffer, ctx->input_size,
                      ctx->output_buffer, &encoded_size, NULL);
}

static void bench_audio_decode_op(void *context) {
    audio_codec_bench_ctx_t *ctx = (audio_codec_bench_ctx_t *)context;
    size_t decoded_size = ctx->output_size;
    turbo_codec_decode(ctx->decoder, ctx->output_buffer, ctx->encoded_size,
                      ctx->input_buffer, &decoded_size);
}

suite("性能基准测试 - 音频编解码") {
    it("G.711 编码性能基准") {
        turbo_audio_codec_config_t config = {
            .sample_rate = 8000,
            .channels = 1,
            .bitrate = 64000,
            .frame_size_ms = BENCHMARK_AUDIO_DURATION_MS
        };
        
        turbo_codec_registry_init();
        audio_codec_bench_ctx_t ctx;
        ctx.encoder = turbo_codec_create_encoder("pcma", &config);
        check_not_null(ctx.encoder);
        if (!ctx.encoder) {
            turbo_codec_registry_shutdown();
            return;
        }
        
        ctx.input_size = test_calculate_audio_buffer_size(8000, 1, 16, BENCHMARK_AUDIO_DURATION_MS);
        ctx.input_buffer = malloc(ctx.input_size);
        ctx.output_size = ctx.input_size;
        ctx.output_buffer = malloc(ctx.output_size);
        
        test_generate_sine_wave_i16((int16_t *)ctx.input_buffer, 
                                   ctx.input_size / 2, 8000, 440.0f, 0.5f);
        
        benchmark_stats_t stats;
        run_benchmark("G.711 A-law Encode", bench_audio_encode_op, &ctx,
                     BENCHMARK_ITERATIONS, &stats);
        
        check(stats.avg_us < PERF_THRESHOLD_AUDIO_ENCODE_US);
        check(stats.p99_us < PERF_THRESHOLD_AUDIO_ENCODE_US * 2);
        
        free(ctx.input_buffer);
        free(ctx.output_buffer);
        turbo_codec_destroy(ctx.encoder);
        turbo_codec_registry_shutdown();
    }
    
    it("G.711 解码性能基准") {
        turbo_audio_codec_config_t enc_config = {
            .sample_rate = 8000,
            .channels = 1,
            .bitrate = 64000,
            .frame_size_ms = BENCHMARK_AUDIO_DURATION_MS
        };
        
        turbo_audio_codec_config_t dec_config = enc_config;
        
        turbo_codec_registry_init();
        audio_codec_bench_ctx_t ctx;
        ctx.encoder = turbo_codec_create_encoder("pcma", &enc_config);
        ctx.decoder = turbo_codec_create_decoder("pcma", &dec_config);
        check(ctx.encoder != NULL && ctx.decoder != NULL);
        if (!ctx.encoder || !ctx.decoder) {
            if (ctx.encoder) turbo_codec_destroy(ctx.encoder);
            if (ctx.decoder) turbo_codec_destroy(ctx.decoder);
            turbo_codec_registry_shutdown();
            return;
        }
        
        ctx.input_size = test_calculate_audio_buffer_size(8000, 1, 16, BENCHMARK_AUDIO_DURATION_MS);
        ctx.input_buffer = malloc(ctx.input_size);
        ctx.output_size = ctx.input_size;
        ctx.output_buffer = malloc(ctx.output_size);
        
        test_generate_sine_wave_i16((int16_t *)ctx.input_buffer,
                                   ctx.input_size / 2, 8000, 440.0f, 0.5f);
        
        size_t encoded_size = ctx.output_size;
        turbo_codec_encode(ctx.encoder, ctx.input_buffer, ctx.input_size,
                          ctx.output_buffer, &encoded_size, NULL);
        ctx.encoded_size = encoded_size;
        
        benchmark_stats_t stats;
        run_benchmark("G.711 A-law Decode", bench_audio_decode_op, &ctx,
                     BENCHMARK_ITERATIONS, &stats);
        
        check(stats.avg_us < PERF_THRESHOLD_AUDIO_DECODE_US);
        check(stats.p99_us < PERF_THRESHOLD_AUDIO_DECODE_US * 2);
        
        free(ctx.input_buffer);
        free(ctx.output_buffer);
        turbo_codec_destroy(ctx.encoder);
        turbo_codec_destroy(ctx.decoder);
        turbo_codec_registry_shutdown();
    }
    
    it("Opus 编码性能基准") {
        turbo_audio_codec_config_t config = {
            .sample_rate = 48000,
            .channels = 2,
            .bitrate = 64000,
            .frame_size_ms = BENCHMARK_AUDIO_DURATION_MS
        };
        
        turbo_codec_registry_init();
        audio_codec_bench_ctx_t ctx;
        ctx.encoder = turbo_codec_create_encoder("opus", &config);
        if (!ctx.encoder) {
            printf("Opus not available, skipping\n");
            turbo_codec_registry_shutdown();
            return;
        }
        
        ctx.input_size = test_calculate_audio_buffer_size(48000, 2, 16, BENCHMARK_AUDIO_DURATION_MS);
        ctx.input_buffer = malloc(ctx.input_size);
        ctx.output_size = ctx.input_size;
        ctx.output_buffer = malloc(ctx.output_size);
        
        test_generate_sine_wave_i16((int16_t *)ctx.input_buffer,
                                   ctx.input_size / 2, 48000, 440.0f, 0.5f);
        
        benchmark_stats_t stats;
        run_benchmark("Opus Encode (48kHz Stereo)", bench_audio_encode_op, &ctx,
                     BENCHMARK_ITERATIONS, &stats);
        
        printf("  Compression ratio: %.2fx\n", 
               (double)ctx.input_size / ctx.output_size);
        
        free(ctx.input_buffer);
        free(ctx.output_buffer);
        turbo_codec_destroy(ctx.encoder);
        turbo_codec_registry_shutdown();
    }
}

/* ============================================================================
 * 视频编解码基准测试
 * ============================================================================ */

typedef struct {
    turbo_codec_t *encoder;
    turbo_codec_t *decoder;
    uint8_t *frame_buffer;
    size_t frame_size;
    uint8_t *encoded_buffer;
    size_t encoded_size;
} video_codec_bench_ctx_t;

static void bench_video_encode_op(void *context) {
    video_codec_bench_ctx_t *ctx = (video_codec_bench_ctx_t *)context;
    size_t encoded_size = ctx->encoded_size;
    turbo_codec_encode(ctx->encoder, ctx->frame_buffer, ctx->frame_size,
                      ctx->encoded_buffer, &encoded_size, NULL);
}

static void bench_video_decode_op(void *context) {
    video_codec_bench_ctx_t *ctx = (video_codec_bench_ctx_t *)context;
    size_t decoded_size = ctx->frame_size;
    turbo_codec_decode(ctx->decoder, ctx->encoded_buffer, ctx->encoded_size,
                      ctx->frame_buffer, &decoded_size);
}

suite("性能基准测试 - 视频编解码") {
    it("H.264 编码性能基准") {
        turbo_video_codec_config_t config = {
            .width = BENCHMARK_VIDEO_WIDTH,
            .height = BENCHMARK_VIDEO_HEIGHT,
            .framerate = BENCHMARK_VIDEO_FPS,
            .bitrate = 2000000  // 2 Mbps
        };
        
        turbo_codec_registry_init();
        video_codec_bench_ctx_t ctx;
        ctx.encoder = turbo_codec_create_encoder("h264", &config);
        if (!ctx.encoder) {
            printf("H.264 encoder not available, skipping\n");
            turbo_codec_registry_shutdown();
            return;
        }
        
        ctx.frame_size = test_calculate_i420_frame_size(BENCHMARK_VIDEO_WIDTH,
                                                        BENCHMARK_VIDEO_HEIGHT);
        ctx.frame_buffer = malloc(ctx.frame_size);
        ctx.encoded_size = ctx.frame_size;
        ctx.encoded_buffer = malloc(ctx.encoded_size);
        
        test_generate_i420_gradient(ctx.frame_buffer, BENCHMARK_VIDEO_WIDTH,
                                   BENCHMARK_VIDEO_HEIGHT);
        
        benchmark_stats_t stats;
        run_benchmark("H.264 Encode (720p30)", bench_video_encode_op, &ctx,
                     100, &stats);  // 较少迭代，视频编码较慢
        
        printf("  Estimated realtime factor: %.2fx\n",
               (1000000.0 / BENCHMARK_VIDEO_FPS) / stats.avg_us);
        
        free(ctx.frame_buffer);
        free(ctx.encoded_buffer);
        turbo_codec_destroy(ctx.encoder);
        turbo_codec_registry_shutdown();
    }
    
    it("VP8 编码性能基准") {
        turbo_video_codec_config_t config = {
            .width = BENCHMARK_VIDEO_WIDTH,
            .height = BENCHMARK_VIDEO_HEIGHT,
            .framerate = BENCHMARK_VIDEO_FPS,
            .bitrate = 2000000
        };
        
        turbo_codec_registry_init();
        video_codec_bench_ctx_t ctx;
        ctx.encoder = turbo_codec_create_encoder("vp8", &config);
        if (!ctx.encoder) {
            printf("VP8 encoder not available, skipping\n");
            turbo_codec_registry_shutdown();
            return;
        }
        
        ctx.frame_size = test_calculate_i420_frame_size(BENCHMARK_VIDEO_WIDTH,
                                                        BENCHMARK_VIDEO_HEIGHT);
        ctx.frame_buffer = malloc(ctx.frame_size);
        ctx.encoded_size = ctx.frame_size;
        ctx.encoded_buffer = malloc(ctx.encoded_size);
        
        test_generate_i420_checkerboard(ctx.frame_buffer, BENCHMARK_VIDEO_WIDTH,
                                       BENCHMARK_VIDEO_HEIGHT, 32);
        
        benchmark_stats_t stats;
        run_benchmark("VP8 Encode (720p30)", bench_video_encode_op, &ctx,
                     100, &stats);
        
        printf("  Estimated realtime factor: %.2fx\n",
               (1000000.0 / BENCHMARK_VIDEO_FPS) / stats.avg_us);
        
        free(ctx.frame_buffer);
        free(ctx.encoded_buffer);
        turbo_codec_destroy(ctx.encoder);
        turbo_codec_registry_shutdown();
    }
}

/* ============================================================================
 * 捕获设备性能基准测试
 * ============================================================================ */

suite("性能基准测试 - 捕获设备") {
    it("音频捕获设备枚举性能") {
        clock_t start = clock();
        
        salts_capture_device_t devices[SALTS_CAPTURE_MAX_DEVICES];
        int count = salts_capture_list_audio_devices(devices, SALTS_CAPTURE_MAX_DEVICES);
        
        clock_t end = clock();
        double elapsed_ms = ((double)(end - start) / CLOCKS_PER_SEC) * 1000.0;
        
        printf("\n枚举 %d 个音频捕获设备耗时: %.2f ms\n", count, elapsed_ms);
        check(elapsed_ms < 100.0);  // 应在 100ms 内完成
        
    }
    
    it("视频捕获设备枚举性能") {
        clock_t start = clock();
        
        salts_capture_device_t devices[SALTS_CAPTURE_MAX_DEVICES];
        int count = salts_capture_list_video_devices(devices, SALTS_CAPTURE_MAX_DEVICES);
        
        clock_t end = clock();
        double elapsed_ms = ((double)(end - start) / CLOCKS_PER_SEC) * 1000.0;
        
        printf("\n枚举 %d 个视频捕获设备耗时: %.2f ms\n", count, elapsed_ms);
        check(elapsed_ms < 200.0);  // 应在 200ms 内完成
        
    }
}

/* ============================================================================
 * 播放设备性能基准测试
 * ============================================================================ */

suite("性能基准测试 - 播放设备") {
    it("播放设备枚举性能") {
        clock_t start = clock();
        
        salts_playback_device_t devices[SALTS_PLAYBACK_MAX_DEVICES];
        size_t count = 0;
        int list_result = salts_playback_list_devices(
            devices, SALTS_PLAYBACK_MAX_DEVICES, &count);
        
        clock_t end = clock();
        double elapsed_ms = ((double)(end - start) / CLOCKS_PER_SEC) * 1000.0;
        
        printf("\n枚举 %zu 个播放设备耗时: %.2f ms\n", count, elapsed_ms);
        check(list_result == SALTS_PLAYBACK_OK);
        check(elapsed_ms < 100.0);
        
    }
    
    it("播放队列操作性能") {
        salts_playback_config_t config = {
            .sample_rate = 48000,
            .channels = 2,
            .format = SALTS_PLAYBACK_FORMAT_S16,
            .buffer_duration_ms = 100
        };
        
        salts_playback_t *playback = NULL;
        if (salts_playback_create(NULL, &config, &playback) != SALTS_PLAYBACK_OK) {
            printf("无法创建播放设备，跳过测试\n");
            return;
        }
        
        size_t buffer_size = test_calculate_audio_buffer_size(48000, 2, 16, 10);
        uint8_t *buffer = malloc(buffer_size);
        test_generate_silence(buffer, buffer_size);
        
        clock_t start = clock();
        
        for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
            size_t written = 0;
            if (salts_playback_write(playback, buffer, buffer_size, &written) !=
                SALTS_PLAYBACK_OK) {
                break;
            }
        }
        
        clock_t end = clock();
        double elapsed_ms = ((double)(end - start) / CLOCKS_PER_SEC) * 1000.0;
        double avg_us = (elapsed_ms * 1000.0) / BENCHMARK_ITERATIONS;
        
        printf("\n队列 %d 次音频缓冲耗时: %.2f ms (平均 %.2f µs/次)\n",
               BENCHMARK_ITERATIONS, elapsed_ms, avg_us);
        check(avg_us < 50.0);  // 单次队列操作应在 50µs 内
        
        free(buffer);
        salts_playback_destroy(playback);
    }
}

/* ============================================================================
 * 内存操作性能基准测试
 * ============================================================================ */

suite("性能基准测试 - 内存操作") {
    it("音频缓冲区分配性能") {
        size_t buffer_size = test_calculate_audio_buffer_size(48000, 2, 16, 100);
        
        clock_t start = clock();
        
        for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
            void *buffer = malloc(buffer_size);
            check_not_null(buffer);
            free(buffer);
        }
        
        clock_t end = clock();
        double elapsed_ms = ((double)(end - start) / CLOCKS_PER_SEC) * 1000.0;
        double avg_us = (elapsed_ms * 1000.0) / BENCHMARK_ITERATIONS;
        
        printf("\n分配/释放 %zu 字节音频缓冲 %d 次耗时: %.2f ms (平均 %.2f µs/次)\n",
               buffer_size, BENCHMARK_ITERATIONS, elapsed_ms, avg_us);
        if (avg_us >= 10.0) {
            printf("  提示: 分配器耗时超过参考阈值 10.00 µs/次，此指标受运行环境影响，不作为失败条件\n");
        }
    }
    
    it("视频帧缓冲区分配性能") {
        size_t frame_size = test_calculate_i420_frame_size(1920, 1080);
        
        clock_t start = clock();
        
        for (int i = 0; i < 100; i++) {  // 视频帧较大，减少迭代次数
            void *buffer = malloc(frame_size);
            check_not_null(buffer);
            free(buffer);
        }
        
        clock_t end = clock();
        double elapsed_ms = ((double)(end - start) / CLOCKS_PER_SEC) * 1000.0;
        double avg_ms = elapsed_ms / 100;
        
        printf("\n分配/释放 %zu 字节视频帧缓冲 100 次耗时: %.2f ms (平均 %.2f ms/次)\n",
               frame_size, elapsed_ms, avg_ms);
        if (avg_ms >= 1.0) {
            printf("  提示: 分配器耗时超过参考阈值 1.00 ms/次，此指标受运行环境影响，不作为失败条件\n");
        }
    }
    
    it("内存拷贝性能 (音频)") {
        size_t buffer_size = test_calculate_audio_buffer_size(48000, 2, 16, 100);
        uint8_t *src = malloc(buffer_size);
        uint8_t *dst = malloc(buffer_size);
        
        test_generate_silence(src, buffer_size);
        
        clock_t start = clock();
        
        for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
            memcpy(dst, src, buffer_size);
        }
        
        clock_t end = clock();
        double elapsed_ms = ((double)(end - start) / CLOCKS_PER_SEC) * 1000.0;
        double bandwidth_mbps = (buffer_size * BENCHMARK_ITERATIONS / (1024.0 * 1024.0)) / 
                               (elapsed_ms / 1000.0);
        
        printf("\n拷贝 %zu 字节 %d 次耗时: %.2f ms (带宽: %.2f MB/s)\n",
               buffer_size, BENCHMARK_ITERATIONS, elapsed_ms, bandwidth_mbps);
        
        free(src);
        free(dst);
    }
}

/* ============================================================================
 * 端到端性能基准测试
 * ============================================================================ */

suite("性能基准测试 - 端到端管道") {
    it("音频捕获-编码-解码-播放完整管道延迟") {
        // 配置捕获
        salts_audio_capture_config_t capture_config = {
            .sample_rate = 48000,
            .channels = 2,
            .bits_per_sample = 16,
            .frame_size_ms = 10
        };
        (void)capture_config;
        
        // 配置编解码
        turbo_audio_codec_config_t enc_config = {
            .sample_rate = 48000,
            .channels = 2,
            .bitrate = 64000,
            .frame_size_ms = 10
        };
        
        turbo_audio_codec_config_t dec_config = enc_config;
        
        // 配置播放
        salts_playback_config_t playback_config = {
            .sample_rate = 48000,
            .channels = 2,
            .format = SALTS_PLAYBACK_FORMAT_S16,
            .buffer_duration_ms = 100
        };
        (void)playback_config;
        
        // 创建组件
        turbo_codec_registry_init();
        turbo_codec_t *encoder = turbo_codec_create_encoder("opus", &enc_config);
        turbo_codec_t *decoder = turbo_codec_create_decoder("opus", &dec_config);
        
        if (!encoder || !decoder) {
            printf("Opus 不可用，跳过端到端测试\n");
            if (encoder) turbo_codec_destroy(encoder);
            if (decoder) turbo_codec_destroy(decoder);
            turbo_codec_registry_shutdown();
            return;
        }
        
        // 准备缓冲区
        size_t pcm_size = test_calculate_audio_buffer_size(48000, 2, 16, 10);
        uint8_t *pcm_input = malloc(pcm_size);
        uint8_t *encoded = malloc(TURBO_CODEC_MAX_FRAME_SIZE);
        uint8_t *pcm_output = malloc(pcm_size);
        
        test_generate_sine_wave_i16((int16_t *)pcm_input, pcm_size / 2,
                                   48000, 440.0f, 0.5f);
        
        // 测量端到端延迟
        clock_t start = clock();
        
        size_t encoded_size = TURBO_CODEC_MAX_FRAME_SIZE;
        int ret = turbo_codec_encode(encoder, pcm_input, pcm_size,
                                     encoded, &encoded_size, NULL);
        check_equal(ret, TURBO_CODEC_OK);
        
        size_t decoded_size = pcm_size;
        ret = turbo_codec_decode(decoder, encoded, encoded_size,
                                pcm_output, &decoded_size);
        check_equal(ret, TURBO_CODEC_OK);
        
        clock_t end = clock();
        double latency_ms = ((double)(end - start) / CLOCKS_PER_SEC) * 1000.0;
        
        printf("\n端到端管道延迟 (编码+解码): %.2f ms\n", latency_ms);
        printf("  音频块大小: %zu 字节 (%.2f ms)\n", 
               pcm_size, (double)pcm_size / (48000 * 2 * 2) * 1000.0);
        printf("  压缩率: %.2fx\n", (double)pcm_size / encoded_size);
        
        check(latency_ms < 5.0);  // 端到端延迟应小于 5ms
        
        free(pcm_input);
        free(encoded);
        free(pcm_output);
        turbo_codec_destroy(encoder);
        turbo_codec_destroy(decoder);
        turbo_codec_registry_shutdown();
    }
}
