#include <turbo_pipeline_compositor.h>
#include <tinytest.h>
#include <threads/threads.h>

#include <libavformat/avformat.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 16, HEIGHT = 16, LUMA_BYTES = WIDTH * HEIGHT,
       FRAME_BYTES = LUMA_BYTES * 3 / 2, FRAME_COUNT = 4, RATE_TEST_INPUT = 30,
       BASE_Y = 96 };

typedef struct video_fixture {
    char *input;
    char *output;
} video_fixture_t;

static void normalize_path(char *path) {
    for (; *path; ++path)
        if (*path == '\\') *path = '/';
}

static int make_video(video_fixture_t *fixture, const char *suffix, size_t frames) {
    uint8_t input[RATE_TEST_INPUT * FRAME_BYTES];
    size_t i;
    if (!frames || frames > RATE_TEST_INPUT) return 0;
    fixture->input = tt_make_temp_file("compositor_input_", ".yuv");
    fixture->output = tt_make_temp_file("compositor_output_", suffix);
    if (!fixture->input || !fixture->output) return 0;
    normalize_path(fixture->input);
    normalize_path(fixture->output);
    memset(input, 128, sizeof(input));
    for (i = 0; i < frames; ++i) memset(input + i * FRAME_BYTES, BASE_Y, LUMA_BYTES);
    return tt_write_file(fixture->input, input, frames * FRAME_BYTES) == 0;
}

static void remove_video(video_fixture_t *fixture) {
    if (fixture->input) tt_remove_file(fixture->input);
    if (fixture->output) tt_remove_file(fixture->output);
    free(fixture->input);
    free(fixture->output);
}

static turbo_pipeline_t *make_pipeline(const video_fixture_t *fixture,
    const char *codec, const char *container, const char *filter,
    int input_rate, int output_rate,
    turbo_pipeline_error_t *error) {
    char yaml[3072];
    int used = snprintf(yaml, sizeof(yaml),
        "api_version: turbo.media.pipeline/v1\n"
        "id: compositor\n"
        "nodes:\n"
        "  - id: input\n"
        "    kind: source\n"
        "    factory: ffmpeg.input\n"
        "    config:\n"
        "      url: '%s'\n"
        "      options: { video_size: 16x16, pixel_format: yuv420p, framerate: '%d' }\n"
        "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: rawvideo } }\n"
        "  - { id: decode, kind: decoder, factory: ffmpeg.decode, config: { media: video } }\n"
        "  - { id: filter, kind: filter, factory: ffmpeg.filter, config: { media: video, filters: '%s' } }\n"
        "  - { id: encode, kind: encoder, factory: ffmpeg.encode, config: { media: video, codec: %s, frame_rate: %d } }\n"
        "  - { id: mux, kind: mux, factory: ffmpeg.mux, config: { format: %s } }\n"
        "  - { id: output, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
        "edges:\n"
        "  - { from: input.out, to: demux.in }\n"
        "  - { from: demux.video, to: decode.in }\n"
        "  - { from: decode.out, to: filter.in }\n"
        "  - { from: filter.out, to: encode.in }\n"
        "  - { from: encode.out, to: mux.video }\n"
        "  - { from: mux.out, to: output.in }\n",
        fixture->input, input_rate, filter, codec, output_rate, container, fixture->output);
    if (used <= 0 || (size_t)used >= sizeof(yaml)) return NULL;
    return turbo_pipeline_create_from_yaml(yaml, (size_t)used, error);
}

static int run(turbo_pipeline_t *pipeline, turbo_pipeline_error_t *error) {
    turbo_pipeline_status_t status = turbo_pipeline_prepare(pipeline, error);
    if (status == TURBO_PIPELINE_OK) status = turbo_pipeline_run(pipeline, error);
    if (status != TURBO_PIPELINE_OK)
        fprintf(stderr, "compositor run %d [%s]: %s\n", status, error->node_id, error->message);
    return status;
}

typedef struct compositor_run {
    turbo_pipeline_t *pipeline;
    turbo_pipeline_error_t error;
    int status;
} compositor_run_t;

static void run_on_thread(void *context) {
    compositor_run_t *runner = context;
    runner->status = run(runner->pipeline, &runner->error);
}

static turbo_compositor_layer_t image_layer(uint8_t rgba[16]) {
    const turbo_compositor_layer_t layer = {
        .kind = TURBO_COMPOSITOR_RGBA_IMAGE,
        .rect = {0, 0, 2, 2},
        .opacity = 255,
        /* The first logical row is the last row in memory. */
        .input.image = {2, 2, {rgba, 16, 8, -8}}
    };
    return layer;
}

static turbo_compositor_config_t configuration(const turbo_compositor_layer_t *layers,
                                                size_t count) {
    const turbo_compositor_config_t config = {
        .width = WIDTH, .height = HEIGHT,
        .main_color = TURBO_COMPOSITOR_COLOR_BT709_LIMITED,
        .layers = layers, .layer_count = count,
        .memory_limit = 1024 * 1024
    };
    return config;
}

suite("Pipeline video compositor") {
    it("converts a 30 fps input to ten video packets in MP4") {
        video_fixture_t fixture = {0};
        turbo_pipeline_t *pipeline = NULL;
        AVFormatContext *reader = NULL;
        AVPacket *packet = NULL;
        turbo_pipeline_error_t error;
        turbo_pipeline_stats_t stats;
        int64_t previous_dts = INT64_MIN;
        int packets = 0;

        if (!make_video(&fixture, ".mp4", RATE_TEST_INPUT)) {
            check_true(0);
            goto cleanup_rate;
        }
        pipeline = make_pipeline(&fixture, "libopenh264", "mp4", "null", 30, 10, &error);
        check_not_null(pipeline);
        if (!pipeline) goto cleanup_rate;
        int result = run(pipeline, &error);
        check_equal(result, TURBO_PIPELINE_OK);
        if (result != TURBO_PIPELINE_OK) goto cleanup_rate;
        check_equal(turbo_pipeline_stats(pipeline, &stats), TURBO_PIPELINE_OK);
        check_equal(stats.frames_decoded, 30u);
        check_equal(stats.frames_encoded, 10u);
        result = avformat_open_input(&reader, fixture.output, NULL, NULL);
        check_equal(result, 0);
        if (result < 0) goto cleanup_rate;
        packet = av_packet_alloc();
        check_not_null(packet);
        if (!packet) goto cleanup_rate;
        while (av_read_frame(reader, packet) >= 0) {
            check_true(packet->dts > previous_dts);
            previous_dts = packet->dts;
            ++packets;
            av_packet_unref(packet);
        }
        check_equal(packets, 10);
cleanup_rate:
        av_packet_free(&packet);
        avformat_close_input(&reader);
        turbo_pipeline_destroy(pipeline);
        remove_video(&fixture);
    }

    it("rejects invalid layer bounds, short planes and insufficient memory") {
        uint8_t rgba[16] = {0};
        turbo_compositor_layer_t layer = image_layer(rgba);
        turbo_compositor_config_t config = configuration(&layer, 1);
        turbo_pipeline_error_t error;
        turbo_pipeline_compositor_t *compositor;
        layer.rect.x = 1;
        compositor = turbo_pipeline_compositor_create(&config, &error);
        check_null(compositor);
        check_equal(error.code, TURBO_PIPELINE_ECONFIG);
        turbo_pipeline_compositor_destroy(compositor);
        layer.rect.x = 0;
        layer.input.image.rgba.size = sizeof(rgba) - 1;
        compositor = turbo_pipeline_compositor_create(&config, &error);
        check_null(compositor);
        check_equal(error.code, TURBO_PIPELINE_EINVAL);
        turbo_pipeline_compositor_destroy(compositor);
        layer.input.image.rgba.size = sizeof(rgba);
        config.memory_limit = 1;
        compositor = turbo_pipeline_compositor_create(&config, &error);
        check_null(compositor);
        check_equal(error.code, TURBO_PIPELINE_ENOMEM);
        turbo_pipeline_compositor_destroy(compositor);
    }

    it("blends an RGBA image into each encoded pixel plane") {
        video_fixture_t fixture = {0};
        turbo_pipeline_t *pipeline = NULL;
        turbo_pipeline_compositor_t *compositor = NULL;
        turbo_pipeline_error_t error;
        uint8_t rgba[] = {0, 255, 0, 0, 0, 255, 0, 0,
                          255, 0, 0, 255, 0, 255, 0, 0};
        turbo_compositor_layer_t layer = image_layer(rgba);
        turbo_compositor_config_t config = configuration(&layer, 1);
        char *output = NULL;
        size_t size = 0, i;

        compositor = turbo_pipeline_compositor_create(&config, &error);
        check_not_null(compositor);
        if (!compositor) goto cleanup_image;
        memset(rgba, 0, sizeof(rgba)); /* Creation owns the prepared image. */
        if (!make_video(&fixture, ".yuv", FRAME_COUNT)) {
            check_true(0);
            goto cleanup_image;
        }
        pipeline = make_pipeline(&fixture, "rawvideo", "rawvideo", "null", 10, 10, &error);
        check_not_null(pipeline);
        if (!pipeline) goto cleanup_image;
        check_equal(turbo_pipeline_bind_compositor(pipeline, compositor, &error),
                    TURBO_PIPELINE_OK);
        check_equal(turbo_pipeline_compositor_destroy(compositor), TURBO_PIPELINE_ESTATE);
        check_equal(turbo_pipeline_set_video_frame_editor(pipeline, NULL, NULL, &error),
                    TURBO_PIPELINE_ESTATE);
        check_equal(run(pipeline, &error), TURBO_PIPELINE_OK);
        output = tt_read_file(fixture.output, &size);
        check_not_null(output);
        check_equal(size, (size_t)(FRAME_COUNT * FRAME_BYTES));
        if (output && size == FRAME_COUNT * FRAME_BYTES) {
            for (i = 0; i < FRAME_COUNT; ++i) {
                const uint8_t *frame = (const uint8_t *)output + i * FRAME_BYTES;
                check_equal(frame[0], 63);
                check_equal(frame[1], BASE_Y);
                check_equal(frame[LUMA_BYTES], 122);
                check_equal(frame[LUMA_BYTES + LUMA_BYTES / 4], 156);
                check_equal(frame[LUMA_BYTES + 1], 128);
            }
        }
cleanup_image:
        free(output);
        turbo_pipeline_destroy(pipeline);
        check_equal(turbo_pipeline_compositor_destroy(compositor), TURBO_PIPELINE_OK);
        remove_video(&fixture);
    }

    it("analyzes every visible luma sample and matches CPU rendering on available D3D11 hardware") {
        enum { CANVAS_WIDTH = 14, CANVAS_LUMA_BYTES = CANVAS_WIDTH * HEIGHT,
               CANVAS_FRAME_BYTES = CANVAS_LUMA_BYTES * 3 / 2 };
        uint8_t rgba[] = {0, 255, 0, 64, 0, 0, 255, 255,
                          255, 0, 0, 255, 0, 0, 0, 0};
        turbo_compositor_layer_t layers[] = {
            {.kind = TURBO_COMPOSITOR_MAIN_VIDEO, .rect = {0, 0, 10, 12}, .opacity = 218},
            {.kind = TURBO_COMPOSITOR_AUX_VIDEO, .rect = {4, 2, 10, 10}, .opacity = 161,
             .input.source = 0},
            image_layer(rgba)
        };
        const turbo_compositor_source_t source = {4, 2, 2, 1000000, 0};
        const uint8_t y[] = {170, 180, 190, 200, 130, 140, 150, 160};
        const uint8_t u[] = {110, 126}, v[] = {195, 130};
        const turbo_compositor_plane_t planes[3] = {
            {y, sizeof(y), 4, -4}, {u, sizeof(u), 0, 2}, {v, sizeof(v), 0, 2}
        };
        const turbo_compositor_position_t positions[] = {{2, 2}, {4, 2}, {8, 8}};
        bool gpu_available = true;
        layers[2].rect = (turbo_compositor_rect_t){6, 6, 2, 2};
        layers[2].opacity = 213;
        for (unsigned first = 0; first < 2; ++first) {
            turbo_compositor_config_t config = configuration(layers + first, 3 - first);
            uint8_t reference[FRAME_COUNT * CANVAS_FRAME_BYTES];
            bool reference_ready = false;
            config.width = CANVAS_WIDTH;
            config.sources = &source;
            config.source_count = 1;
            for (int backend = TURBO_COMPOSITOR_CPU;
                 backend <= (gpu_available ? TURBO_COMPOSITOR_D3D11 : TURBO_COMPOSITOR_CPU);
                 ++backend) {
                const turbo_compositor_options_t options = {sizeof(options), backend, 1};
                turbo_pipeline_error_t error;
                video_fixture_t fixture = {0};
                turbo_pipeline_t *pipeline = NULL;
                turbo_pipeline_compositor_t *compositor =
                    turbo_pipeline_compositor_create_ex(&config, &options, &error);
                char *output = NULL;
                size_t size = 0;
                turbo_compositor_luma_t sample;
                if (!compositor && backend == TURBO_COMPOSITOR_D3D11 &&
                    error.code == TURBO_PIPELINE_ECONFIG) {
                    gpu_available = false;
                    continue;
                }
                if (!compositor) fprintf(stderr, "compositor create %d: %s\n", error.code, error.message);
                check_not_null(compositor);
                if (!compositor) goto cleanup_analysis;
                check_equal(turbo_pipeline_compositor_luma(compositor, &sample), TURBO_PIPELINE_ESTATE);
                check_equal(turbo_pipeline_compositor_submit(compositor, 0, planes, 100000),
                            TURBO_PIPELINE_OK);
                check_equal(turbo_pipeline_compositor_set_positions(compositor, positions + first,
                            3 - first), TURBO_PIPELINE_OK);
                if (!make_video(&fixture, ".yuv", FRAME_COUNT)) {
                    check_true(0);
                    goto cleanup_analysis;
                }
                pipeline = make_pipeline(&fixture, "rawvideo", "rawvideo", "null", 10, 10, &error);
                check_not_null(pipeline);
                if (!pipeline) goto cleanup_analysis;
                check_equal(turbo_pipeline_bind_compositor(pipeline, compositor, &error), TURBO_PIPELINE_OK);
                check_equal(run(pipeline, &error), TURBO_PIPELINE_OK);
                output = tt_read_file(fixture.output, &size);
                check_not_null(output);
                check_equal(size, sizeof(reference));
                if (output && size == sizeof(reference)) {
                    const uint8_t *last = (const uint8_t *)output + (FRAME_COUNT - 1) * CANVAS_FRAME_BYTES;
                    uint32_t expected[256] = {0};
                    for (size_t pixel = 0; pixel < CANVAS_LUMA_BYTES; ++pixel) ++expected[last[pixel]];
                    turbo_pipeline_status_t status = turbo_pipeline_compositor_luma(compositor, &sample);
                    check_equal(status, TURBO_PIPELINE_OK);
                    if (status == TURBO_PIPELINE_OK) {
                        check_equal(memcmp(sample.bins, expected, sizeof(expected)), 0);
                        check_equal(sample.pts * sample.time_base_num * 10,
                                    (int64_t)(FRAME_COUNT - 1) * sample.time_base_den);
                    }
                    if (backend == TURBO_COMPOSITOR_CPU) {
                        memcpy(reference, output, sizeof(reference));
                        reference_ready = true;
                    } else if (reference_ready) {
                        check_equal(memcmp(reference, output, sizeof(reference)), 0);
                    }
                }
cleanup_analysis:
                free(output);
                turbo_pipeline_destroy(pipeline);
                check_equal(turbo_pipeline_compositor_destroy(compositor), TURBO_PIPELINE_OK);
                remove_video(&fixture);
            }
        }
    }

    it("moves all layers together between frames while encoding") {
        video_fixture_t fixture = {0};
        turbo_pipeline_t *pipeline = NULL;
        turbo_pipeline_compositor_t *compositor = NULL;
        compositor_run_t runner = {0};
        sync_thread_t thread = NULL;
        turbo_pipeline_stats_t stats = {0};
        turbo_pipeline_error_t error;
        uint8_t rgba[] = {255, 0, 0, 255, 255, 0, 0, 255,
                          255, 0, 0, 255, 255, 0, 0, 255};
        turbo_compositor_layer_t layers[] = {image_layer(rgba), image_layer(rgba)};
        turbo_compositor_config_t config = configuration(layers, 2);
        turbo_compositor_position_t positions[] = {{4, 2}, {12, 2}};
        char *output = NULL;
        size_t size = 0, i;
        unsigned wait;

        layers[1].rect.x = 2;
        compositor = turbo_pipeline_compositor_create(&config, &error);
        check_not_null(compositor);
        if (!compositor) goto cleanup_positions;
        check_equal(turbo_pipeline_compositor_set_positions(compositor, positions, 1),
                    TURBO_PIPELINE_EINVAL);
        check_equal(turbo_pipeline_compositor_set_positions(compositor, positions, 2),
                    TURBO_PIPELINE_OK);
        positions[0] = (turbo_compositor_position_t){4, 14};
        positions[1] = (turbo_compositor_position_t){16, 2};
        check_equal(turbo_pipeline_compositor_set_positions(compositor, positions, 2),
                    TURBO_PIPELINE_EINVAL);
        if (!make_video(&fixture, ".yuv", FRAME_COUNT)) {
            check_true(0);
            goto cleanup_positions;
        }
        /* Pace source timestamps at one frame per second so the control thread
         * can act after the first complete frame. */
        pipeline = make_pipeline(&fixture, "rawvideo", "rawvideo", "setpts=10*PTS,realtime",
                                 10, 0, &error);
        check_not_null(pipeline);
        if (!pipeline) goto cleanup_positions;
        check_equal(turbo_pipeline_bind_compositor(pipeline, compositor, &error),
                    TURBO_PIPELINE_OK);
        runner.pipeline = pipeline;
        check_equal(sync_thread_create(&thread, run_on_thread, &runner), 0);
        if (!thread) goto cleanup_positions;
        for (wait = 0; wait < 5000; ++wait) {
            turbo_pipeline_stats(pipeline, &stats);
            if (stats.frames_encoded || turbo_pipeline_state(pipeline) == TURBO_PIPELINE_STATE_FAILED)
                break;
            sleep_ms(1);
        }
        check_true(stats.frames_encoded >= 1 && stats.frames_encoded < FRAME_COUNT);
        positions[1] = (turbo_compositor_position_t){12, 14};
        check_equal(turbo_pipeline_compositor_set_positions(compositor, positions, 2),
                    TURBO_PIPELINE_OK);
        turbo_pipeline_compositor_close(compositor);
        positions[0].y = positions[1].y = 8;
        check_equal(turbo_pipeline_compositor_set_positions(compositor, positions, 2),
                    TURBO_PIPELINE_ESTATE);
        check_equal(sync_thread_join(&thread), 0);
        sync_thread_destroy(&thread);
        check_equal(runner.status, TURBO_PIPELINE_OK);
        output = tt_read_file(fixture.output, &size);
        check_not_null(output);
        check_equal(size, (size_t)(FRAME_COUNT * FRAME_BYTES));
        if (output && size == FRAME_COUNT * FRAME_BYTES) {
            for (i = 0; i < FRAME_COUNT; ++i) {
                const uint8_t *frame = (const uint8_t *)output + i * FRAME_BYTES;
                int moved = frame[14 * WIDTH + 4] == 63;
                check_equal(frame[14 * WIDTH + 4], moved ? 63 : BASE_Y);
                check_equal(frame[14 * WIDTH + 12], moved ? 63 : BASE_Y);
                check_equal(frame[2 * WIDTH + 4], moved ? BASE_Y : 63);
                check_equal(frame[2 * WIDTH + 12], moved ? BASE_Y : 63);
                check_equal(frame[0], BASE_Y);
                if (i == 0) check_false(moved);
                if (i == FRAME_COUNT - 1) check_true(moved);
            }
        }
cleanup_positions:
        free(output);
        turbo_pipeline_destroy(pipeline);
        check_equal(turbo_pipeline_compositor_destroy(compositor), TURBO_PIPELINE_OK);
        remove_video(&fixture);
    }

    it("selects the latest due auxiliary frame for a split layout") {
        video_fixture_t fixture = {0};
        turbo_pipeline_t *pipeline = NULL;
        turbo_pipeline_compositor_t *compositor = NULL;
        turbo_pipeline_error_t error;
        turbo_compositor_stats_t stats;
        const turbo_compositor_layer_t layers[] = {
            {.kind = TURBO_COMPOSITOR_MAIN_VIDEO, .rect = {0, 0, 8, 8}, .opacity = 255},
            {.kind = TURBO_COMPOSITOR_AUX_VIDEO, .rect = {8, 8, 8, 8}, .opacity = 255,
             .input.source = 0},
            {.kind = TURBO_COMPOSITOR_AUX_VIDEO, .rect = {8, 0, 8, 8}, .opacity = 255,
             .input.source = 1}
        };
        const turbo_compositor_source_t sources[] = {
            {4, 4, 2, 50000, 0}, {4, 4, 2, 1000000, 1}
        };
        turbo_compositor_config_t config = configuration(layers, 3);
        uint8_t y[16], u[4], v[4];
        turbo_compositor_plane_t planes[3] = {
            {y, sizeof(y), 0, 4}, {u, sizeof(u), 0, 2}, {v, sizeof(v), 0, 2}
        };
        char *output = NULL;
        size_t size = 0, i;
        const uint8_t expected[] = {16, 130, 200, 16};

        config.sources = sources;
        config.source_count = 2;
        compositor = turbo_pipeline_compositor_create(&config, &error);
        check_not_null(compositor);
        if (!compositor) goto cleanup_layout;
        memset(u, 128, sizeof(u));
        memset(v, 128, sizeof(v));
        memset(y, 130, sizeof(y));
        check_equal(turbo_pipeline_compositor_submit(compositor, 0, planes, 100000),
                    TURBO_PIPELINE_OK);
        memset(y, 200, sizeof(y));
        check_equal(turbo_pipeline_compositor_submit(compositor, 0, planes, 200000),
                    TURBO_PIPELINE_OK);
        check_equal(turbo_pipeline_compositor_submit(compositor, 0, planes, 300000),
                    TURBO_PIPELINE_EBACKPRESSURE);
        check_equal(turbo_pipeline_compositor_submit(compositor, 0, planes, 150000),
                    TURBO_PIPELINE_EINVAL);
        memset(y, 175, sizeof(y));
        check_equal(turbo_pipeline_compositor_submit(compositor, 1, planes, 0),
                    TURBO_PIPELINE_OK);
        memset(y, 16, sizeof(y));
        if (!make_video(&fixture, ".yuv", FRAME_COUNT)) {
            check_true(0);
            goto cleanup_layout;
        }
        pipeline = make_pipeline(&fixture, "rawvideo", "rawvideo", "null", 10, 10, &error);
        check_not_null(pipeline);
        if (!pipeline) goto cleanup_layout;
        check_equal(turbo_pipeline_bind_compositor(pipeline, compositor, &error),
                    TURBO_PIPELINE_OK);
        check_equal(run(pipeline, &error), TURBO_PIPELINE_OK);
        output = tt_read_file(fixture.output, &size);
        check_not_null(output);
        check_equal(size, (size_t)(FRAME_COUNT * FRAME_BYTES));
        if (output && size == FRAME_COUNT * FRAME_BYTES) {
            for (i = 0; i < FRAME_COUNT; ++i) {
                const uint8_t *frame = (const uint8_t *)output + i * FRAME_BYTES;
                check_equal(frame[0], BASE_Y);
                check_equal(frame[8], 175);
                check_equal(frame[8 * WIDTH], 16);
                check_equal(frame[8 * WIDTH + 8], expected[i]);
                check_equal(frame[15 * WIDTH + 15], expected[i]);
            }
        }
        check_equal(turbo_pipeline_compositor_stats(compositor, 0, &stats), TURBO_PIPELINE_OK);
        check_equal(stats.submitted, 2u);
        check_equal(stats.full, 1u);
        check_equal(stats.invalid, 1u);
        check_equal(stats.expired, 1u);
        check_equal(stats.missing, 2u);
        check_equal(stats.peak_slots, 2u);
        turbo_pipeline_compositor_close(compositor);
        check_equal(turbo_pipeline_compositor_submit(compositor, 0, planes, 300000),
                    TURBO_PIPELINE_ESTATE);
cleanup_layout:
        free(output);
        turbo_pipeline_destroy(pipeline);
        check_equal(turbo_pipeline_compositor_destroy(compositor), TURBO_PIPELINE_OK);
        remove_video(&fixture);
    }

    it("composes four video feeds into one canvas with independently sized tiles") {
        enum { CANVAS_W = 32, CANVAS_H = 24, CANVAS_BYTES = CANVAS_W * CANVAS_H * 3 / 2 };
        const turbo_compositor_layer_t layers[] = {
            {.kind = TURBO_COMPOSITOR_MAIN_VIDEO, .rect = {2, 2, 12, 12}, .opacity = 255},
            {.kind = TURBO_COMPOSITOR_AUX_VIDEO, .rect = {16, 2, 14, 8}, .opacity = 255,
             .input.source = 0},
            {.kind = TURBO_COMPOSITOR_AUX_VIDEO, .rect = {2, 16, 12, 8}, .opacity = 255,
             .input.source = 1},
            {.kind = TURBO_COMPOSITOR_AUX_VIDEO, .rect = {18, 12, 10, 10}, .opacity = 255,
             .input.source = 2}
        };
        const turbo_compositor_source_t sources[] = {
            {8, 4, FRAME_COUNT, 200000, 1},
            {4, 8, FRAME_COUNT, 200000, 1},
            {4, 4, FRAME_COUNT, 200000, 1}
        };
        turbo_compositor_config_t config = configuration(layers, 4);
        video_fixture_t fixture = {0};
        turbo_pipeline_t *pipeline = NULL;
        turbo_pipeline_compositor_t *compositor = NULL;
        turbo_pipeline_error_t error;
        char *output = NULL;
        size_t source, size = 0;
        int i;

        config.width = CANVAS_W;
        config.height = CANVAS_H;
        config.sources = sources;
        config.source_count = sizeof(sources) / sizeof(sources[0]);
        compositor = turbo_pipeline_compositor_create(&config, &error);
        check_not_null(compositor);
        if (!compositor) goto cleanup_four_feeds;
        if (!make_video(&fixture, ".yuv", FRAME_COUNT)) {
            check_true(0);
            goto cleanup_four_feeds;
        }
        pipeline = make_pipeline(&fixture, "rawvideo", "rawvideo", "crop=16:8:0:0", 10, 10, &error);
        check_not_null(pipeline);
        if (!pipeline) goto cleanup_four_feeds;
        check_equal(turbo_pipeline_bind_compositor(pipeline, compositor, &error),
                    TURBO_PIPELINE_OK);
        for (source = 0; source < config.source_count; ++source) {
            int width = sources[source].width;
            size_t luma = (size_t)width * sources[source].height;
            uint8_t pixels[8 * 8 * 3 / 2];
            turbo_compositor_plane_t planes[] = {
                {pixels, luma, 0, width},
                {pixels + luma, luma / 4, 0, width / 2},
                {pixels + luma * 5 / 4, luma / 4, 0, width / 2}
            };
            memset(pixels + luma, 128, luma / 2);
            for (i = 0; i < FRAME_COUNT; ++i) {
                memset(pixels, 120 + 30 * (int)source + i, luma);
                check_equal(turbo_pipeline_compositor_submit(compositor, (uint32_t)source,
                                                           planes, 100000 * (int64_t)i),
                            TURBO_PIPELINE_OK);
            }
        }
        check_equal(run(pipeline, &error), TURBO_PIPELINE_OK);
        output = tt_read_file(fixture.output, &size);
        check_not_null(output);
        check_equal(size, (size_t)(FRAME_COUNT * CANVAS_BYTES));
        if (output && size == FRAME_COUNT * CANVAS_BYTES) {
            for (i = 0; i < FRAME_COUNT; ++i) {
                const uint8_t *frame = (const uint8_t *)output + (size_t)i * CANVAS_BYTES;
                check_equal(frame[2 * CANVAS_W + 2], 16);
                check_equal(frame[4 * CANVAS_W + 2], BASE_Y);
                check_equal(frame[9 * CANVAS_W + 13], BASE_Y);
                check_equal(frame[10 * CANVAS_W + 13], 16);
                check_equal(frame[2 * CANVAS_W + 16], 120 + i);
                check_equal(frame[7 * CANVAS_W + 29], 120 + i);
                check_equal(frame[8 * CANVAS_W + 16], 16);
                check_equal(frame[16 * CANVAS_W + 5], 16);
                check_equal(frame[16 * CANVAS_W + 6], 150 + i);
                check_equal(frame[23 * CANVAS_W + 9], 150 + i);
                check_equal(frame[23 * CANVAS_W + 10], 16);
                check_equal(frame[12 * CANVAS_W + 18], 180 + i);
                check_equal(frame[21 * CANVAS_W + 27], 180 + i);
                check_equal(frame[22 * CANVAS_W + 27], 16);
                check_equal(frame[23 * CANVAS_W + 31], 16);
            }
        }
cleanup_four_feeds:
        free(output);
        turbo_pipeline_destroy(pipeline);
        check_equal(turbo_pipeline_compositor_destroy(compositor), TURBO_PIPELINE_OK);
        remove_video(&fixture);
    }

    it("signals normalized BT.709 color in a compressed output") {
        video_fixture_t fixture = {0};
        turbo_pipeline_t *pipeline = NULL;
        turbo_pipeline_compositor_t *compositor = NULL;
        turbo_pipeline_error_t error;
        uint8_t rgba[16] = {0};
        turbo_compositor_layer_t layer = image_layer(rgba);
        turbo_compositor_config_t config = configuration(&layer, 1);
        AVFormatContext *reader = NULL;
        int stream;

        config.main_color = TURBO_COMPOSITOR_COLOR_BT601_LIMITED;
        compositor = turbo_pipeline_compositor_create(&config, &error);
        check_not_null(compositor);
        if (!compositor) goto cleanup_color;
        if (!make_video(&fixture, ".mkv", FRAME_COUNT)) {
            check_true(0);
            goto cleanup_color;
        }
        pipeline = make_pipeline(&fixture, "libopenh264", "matroska", "null", 10, 10, &error);
        check_not_null(pipeline);
        if (!pipeline) goto cleanup_color;
        check_equal(turbo_pipeline_bind_compositor(pipeline, compositor, &error),
                    TURBO_PIPELINE_OK);
        check_equal(run(pipeline, &error), TURBO_PIPELINE_OK);
        check_equal(avformat_open_input(&reader, fixture.output, NULL, NULL), 0);
        if (!reader) goto cleanup_color;
        check_true(avformat_find_stream_info(reader, NULL) >= 0);
        stream = av_find_best_stream(reader, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        check_true(stream >= 0);
        if (stream >= 0) {
            const AVCodecParameters *video = reader->streams[stream]->codecpar;
            check_equal(video->color_space, AVCOL_SPC_BT709);
            check_equal(video->color_primaries, AVCOL_PRI_BT709);
            check_equal(video->color_trc, AVCOL_TRC_BT709);
            check_equal(video->color_range, AVCOL_RANGE_MPEG);
        }
cleanup_color:
        avformat_close_input(&reader);
        turbo_pipeline_destroy(pipeline);
        check_equal(turbo_pipeline_compositor_destroy(compositor), TURBO_PIPELINE_OK);
        remove_video(&fixture);
    }
}
