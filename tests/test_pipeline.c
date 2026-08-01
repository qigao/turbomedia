#include "turbo_pipeline.h"

#include <rtp-packet.h>
#include <tinytest.h>
#include <turbo_codec.h>
#include <turbo_media_server.h>
#include <turbo_thread.h>

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char VALID_COPY_YAML[] =
    "api_version: turbo.media.pipeline/v1\n"
    "id: copy\n"
    "nodes:\n"
    "  - id: source\n"
    "    kind: source\n"
    "    factory: ffmpeg.input\n"
    "    config:\n"
    "      url: input.ts\n"
    "  - id: demux\n"
    "    kind: demux\n"
    "    factory: ffmpeg.demux\n"
    "  - id: mux\n"
    "    kind: mux\n"
    "    factory: ffmpeg.mux\n"
    "    config:\n"
    "      format: mpegts\n"
    "  - id: sink\n"
    "    kind: sink\n"
    "    factory: ffmpeg.output\n"
    "    config:\n"
    "      url: output.ts\n"
    "edges:\n"
    "  - from: source.out\n"
    "    to: demux.in\n"
    "  - from: demux.video\n"
    "    to: mux.video\n"
    "  - from: mux.out\n"
    "    to: sink.in\n";

static void write_u16_le(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)(value & 0xffu);
    destination[1] = (uint8_t)(value >> 8u);
}

static void write_u32_le(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)(value & 0xffu);
    destination[1] = (uint8_t)((value >> 8u) & 0xffu);
    destination[2] = (uint8_t)((value >> 16u) & 0xffu);
    destination[3] = (uint8_t)(value >> 24u);
}

static int write_test_wav(const char *path) {
    enum { SAMPLE_RATE = 8000, SAMPLE_COUNT = 800, HEADER_SIZE = 44 };
    const size_t data_size = SAMPLE_COUNT * sizeof(int16_t);
    const size_t file_size = HEADER_SIZE + data_size;
    uint8_t *wav = (uint8_t *)calloc(1, file_size);
    int result;
    if (!wav) return -1;
    memcpy(wav, "RIFF", 4);
    write_u32_le(wav + 4, (uint32_t)(file_size - 8u));
    memcpy(wav + 8, "WAVEfmt ", 8);
    write_u32_le(wav + 16, 16);
    write_u16_le(wav + 20, 1);
    write_u16_le(wav + 22, 1);
    write_u32_le(wav + 24, SAMPLE_RATE);
    write_u32_le(wav + 28, SAMPLE_RATE * sizeof(int16_t));
    write_u16_le(wav + 32, sizeof(int16_t));
    write_u16_le(wav + 34, 16);
    memcpy(wav + 36, "data", 4);
    write_u32_le(wav + 40, (uint32_t)data_size);
    result = tt_write_file(path, wav, file_size);
    free(wav);
    return result;
}

static int write_test_yuv420p(const char *path) {
    enum { WIDTH = 16, HEIGHT = 16, FRAME_COUNT = 2 };
    uint8_t frames[WIDTH * HEIGHT * 3 / 2 * FRAME_COUNT];
    size_t frame_size = WIDTH * HEIGHT * 3u / 2u;
    size_t i;
    memset(frames, 128, sizeof(frames));
    for (i = 0; i < WIDTH * HEIGHT; ++i) frames[i] = (uint8_t)i;
    for (i = 0; i < WIDTH * HEIGHT; ++i)
        frames[frame_size + i] = (uint8_t)(255u - i);
    return tt_write_file(path, frames, sizeof(frames));
}

static void normalize_path(char *path) {
    char *cursor;
    for (cursor = path; *cursor; ++cursor)
        if (*cursor == '\\') *cursor = '/';
}

typedef struct pipeline_run_result {
    turbo_pipeline_t *pipeline;
    turbo_pipeline_status_t status;
    turbo_pipeline_error_t error;
} pipeline_run_result_t;

typedef struct runtime_rtp_capture {
    atomic_int count;
    uint8_t packets[2][2048];
    size_t sizes[2];
    int track_ids[2];
} runtime_rtp_capture_t;

static void run_pipeline_thread(void *parameter) {
    pipeline_run_result_t *result = (pipeline_run_result_t *)parameter;
    result->status = turbo_pipeline_run(result->pipeline, &result->error);
}

static int capture_runtime_rtp(turbo_media_source_t *source,
                               const turbo_media_frame_t *frame,
                               void *user_data) {
    runtime_rtp_capture_t *capture = (runtime_rtp_capture_t *)user_data;
    int index = frame->track_id == 10 ? 0 : frame->track_id == 20 ? 1 : -1;
    (void)source;
    if (index < 0 || frame->size > sizeof(capture->packets[index]))
        return TURBO_MEDIA_ERR_INVALID;
    memcpy(capture->packets[index], frame->data, frame->size);
    capture->sizes[index] = frame->size;
    capture->track_ids[index] = frame->track_id;
    atomic_fetch_add_explicit(&capture->count, 1, memory_order_release);
    return TURBO_MEDIA_OK;
}

static int make_rtp_packet(uint8_t *buffer,
                           size_t capacity,
                           uint8_t payload_type,
                           uint16_t sequence,
                           uint32_t timestamp,
                           uint32_t ssrc,
                           int marker,
                           const uint8_t *payload,
                           size_t payload_size) {
    struct rtp_packet_t packet;
    memset(&packet, 0, sizeof(packet));
    packet.rtp.v = RTP_VERSION;
    packet.rtp.pt = payload_type;
    packet.rtp.seq = sequence;
    packet.rtp.timestamp = timestamp;
    packet.rtp.ssrc = ssrc;
    packet.rtp.m = marker ? 1u : 0u;
    packet.payload = payload;
    packet.payloadlen = (int)payload_size;
    return rtp_packet_serialize(&packet, buffer, (int)capacity);
}

suite("turbo_media_pipeline") {
    group("configuration") {
        it("accepts a valid stream-copy DAG without performing I/O") {
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline = turbo_pipeline_create_from_yaml(
                VALID_COPY_YAML, sizeof(VALID_COPY_YAML) - 1u, &error);

            if (!pipeline) fprintf(stderr, "pipeline config error: %s\n", error.message);
            check_not_null(pipeline);
            if (pipeline) {
                check_int_eq(turbo_pipeline_state(pipeline),
                             TURBO_PIPELINE_STATE_CREATED);
                turbo_pipeline_destroy(pipeline);
            }
        }

        it("rejects an unsupported API version") {
            char yaml[sizeof(VALID_COPY_YAML)];
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline;
            memcpy(yaml, VALID_COPY_YAML, sizeof(VALID_COPY_YAML));
            memcpy(strstr(yaml, "turbo.media.pipeline/v1"),
                   "turbo.media.pipeline/v2", strlen("turbo.media.pipeline/v2"));

            pipeline = turbo_pipeline_create_from_yaml(
                yaml, sizeof(yaml) - 1u, &error);

            check_null(pipeline);
            check_int_eq(error.code, TURBO_PIPELINE_ECONFIG);
        }

        it("rejects duplicate node identifiers") {
            const char yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: duplicate\n"
                "nodes:\n"
                "  - { id: same, kind: source, factory: ffmpeg.input, config: { url: in.ts } }\n"
                "  - { id: same, kind: demux, factory: ffmpeg.demux }\n"
                "  - { id: mux, kind: mux, factory: ffmpeg.mux }\n"
                "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: out.ts } }\n"
                "edges:\n"
                "  - { from: same.out, to: same.in }\n"
                "  - { from: same.video, to: mux.video }\n"
                "  - { from: mux.out, to: sink.in }\n";
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline =
                turbo_pipeline_create_from_yaml(yaml, sizeof(yaml) - 1u, &error);

            check_null(pipeline);
            check_int_eq(error.code, TURBO_PIPELINE_EGRAPH);
        }

        it("rejects unknown YAML fields before graph compilation") {
            char yaml[sizeof(VALID_COPY_YAML) + 32u];
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline;
            int length = snprintf(yaml, sizeof(yaml), "%sunknown: true\n",
                                  VALID_COPY_YAML);

            pipeline = turbo_pipeline_create_from_yaml(yaml, (size_t)length, &error);

            check_null(pipeline);
            check_int_eq(error.code, TURBO_PIPELINE_ECONFIG);
            check_str_contains(error.message, "unknown field");
        }

        it("rejects graph cycles") {
            char yaml[sizeof(VALID_COPY_YAML) + 64u];
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline;
            int length = snprintf(yaml, sizeof(yaml),
                                  "%s  - from: mux.loop\n"
                                  "    to: source.in\n",
                                  VALID_COPY_YAML);

            pipeline = turbo_pipeline_create_from_yaml(yaml, (size_t)length, &error);

            check_null(pipeline);
            check_int_eq(error.code, TURBO_PIPELINE_EGRAPH);
            check_str_contains(error.message, "cycle");
        }

        it("rejects labeled multi-endpoint filter syntax") {
            const char yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: filter-label\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: in.wav } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux }\n"
                "  - { id: decoder, kind: decoder, factory: ffmpeg.decode, config: { media: audio } }\n"
                "  - { id: filter, kind: filter, factory: ffmpeg.filter, config: { media: audio, filters: '[in]anull[out]' } }\n"
                "  - { id: encoder, kind: encoder, factory: ffmpeg.encode, config: { media: audio, codec: pcm_s16le } }\n"
                "  - { id: mux, kind: mux, factory: ffmpeg.mux, config: { format: wav } }\n"
                "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: out.wav } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.audio, to: decoder.in }\n"
                "  - { from: decoder.out, to: filter.in }\n"
                "  - { from: filter.out, to: encoder.in }\n"
                "  - { from: encoder.out, to: mux.audio }\n"
                "  - { from: mux.out, to: sink.in }\n";
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline =
                turbo_pipeline_create_from_yaml(yaml, sizeof(yaml) - 1u, &error);

            check_null(pipeline);
            check_int_eq(error.code, TURBO_PIPELINE_ECONFIG);
        }
    }

    group("FFmpeg runtime") {
        it("enters FAILED and releases resources when input opening fails") {
            char *missing_path =
                tt_make_temp_file("turbo_pipeline_missing_input_", ".wav");
            char *output_path =
                tt_make_temp_file("turbo_pipeline_failed_output_", ".mka");
            char yaml[3072];
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline = NULL;
            int yaml_size;

            check_not_null(missing_path);
            check_not_null(output_path);
            if (!missing_path || !output_path) goto cleanup_failed_prepare;
            check_int_eq(tt_remove_file(missing_path), 0);
            check_int_eq(tt_remove_file(output_path), 0);
            normalize_path(missing_path);
            normalize_path(output_path);
            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: missing-input\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: '%s' } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: wav } }\n"
                "  - { id: mux, kind: mux, factory: ffmpeg.mux, config: { format: matroska } }\n"
                "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.audio, to: mux.audio }\n"
                "  - { from: mux.out, to: sink.in }\n",
                missing_path, output_path);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml))
                goto cleanup_failed_prepare;
            pipeline =
                turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, &error);
            check_not_null(pipeline);
            if (!pipeline) goto cleanup_failed_prepare;

            check_int_eq(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_EFFMPEG);
            check_int_eq(turbo_pipeline_state(pipeline),
                         TURBO_PIPELINE_STATE_FAILED);
            check_int_eq(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_ESTATE);
            check_int_eq(turbo_pipeline_request_stop(pipeline),
                         TURBO_PIPELINE_ESTATE);

        cleanup_failed_prepare:
            turbo_pipeline_destroy(pipeline);
            if (missing_path) tt_remove_file(missing_path);
            if (output_path) tt_remove_file(output_path);
            free(missing_path);
            free(output_path);
        }

        it("honors a stop request made after prepare") {
            char *input_path =
                tt_make_temp_file("turbo_pipeline_stop_input_", ".wav");
            char *output_path =
                tt_make_temp_file("turbo_pipeline_stop_output_", ".mka");
            char yaml[3072];
            turbo_pipeline_error_t error;
            turbo_pipeline_stats_t stats;
            turbo_pipeline_t *pipeline = NULL;
            int yaml_size;

            check_not_null(input_path);
            check_not_null(output_path);
            if (!input_path || !output_path) goto cleanup_stop;
            normalize_path(input_path);
            normalize_path(output_path);
            check_int_eq(write_test_wav(input_path), 0);
            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: stop-before-run\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: '%s' } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: wav } }\n"
                "  - { id: mux, kind: mux, factory: ffmpeg.mux, config: { format: matroska } }\n"
                "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.audio, to: mux.audio }\n"
                "  - { from: mux.out, to: sink.in }\n",
                input_path, output_path);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml))
                goto cleanup_stop;
            pipeline =
                turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, &error);
            check_not_null(pipeline);
            if (!pipeline) goto cleanup_stop;
            check_int_eq(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_OK);
            check_int_eq(turbo_pipeline_request_stop(pipeline),
                         TURBO_PIPELINE_OK);
            check_int_eq(turbo_pipeline_run(pipeline, &error),
                         TURBO_PIPELINE_ESTOPPED);
            check_int_eq(turbo_pipeline_state(pipeline),
                         TURBO_PIPELINE_STATE_STOPPED);
            check_int_eq(turbo_pipeline_stats(pipeline, &stats),
                         TURBO_PIPELINE_OK);
            check_int_eq((int)stats.packets_read, 0);
            check_int_eq((int)stats.packets_written, 0);

        cleanup_stop:
            turbo_pipeline_destroy(pipeline);
            if (input_path) tt_remove_file(input_path);
            if (output_path) tt_remove_file(output_path);
            free(input_path);
            free(output_path);
        }

        it("stream-copies audio while changing the container") {
            char *input_path = tt_make_temp_file("turbo_pipeline_copy_in_", ".wav");
            char *output_path = tt_make_temp_file("turbo_pipeline_copy_out_", ".mka");
            char yaml[3072];
            turbo_pipeline_error_t error;
            turbo_pipeline_stats_t stats;
            turbo_pipeline_t *pipeline = NULL;
            char *output_data = NULL;
            size_t output_size = 0;
            int yaml_size;

            check_not_null(input_path);
            check_not_null(output_path);
            if (!input_path || !output_path) goto cleanup_copy;
            normalize_path(input_path);
            normalize_path(output_path);
            check_int_eq(write_test_wav(input_path), 0);
            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: wav-to-matroska\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: '%s' } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: wav } }\n"
                "  - { id: mux, kind: mux, factory: ffmpeg.mux, config: { format: matroska } }\n"
                "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.audio, to: mux.audio }\n"
                "  - { from: mux.out, to: sink.in }\n",
                input_path, output_path);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml)) goto cleanup_copy;
            pipeline = turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, &error);
            check_not_null(pipeline);
            if (!pipeline) goto cleanup_copy;
            check_int_eq(turbo_pipeline_prepare(pipeline, &error), TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED)
                goto cleanup_copy;
            check_int_eq(turbo_pipeline_run(pipeline, &error), TURBO_PIPELINE_OK);
            check_int_eq(turbo_pipeline_stats(pipeline, &stats), TURBO_PIPELINE_OK);
            check_true(stats.packets_read > 0);
            check_true(stats.packets_written > 0);
            check_int_eq((int)stats.frames_decoded, 0);
            output_data = tt_read_file(output_path, &output_size);
            check_not_null(output_data);
            check_true(output_size > 4u);
            if (output_data && output_size >= 4u) {
                check_int_eq((unsigned char)output_data[0], 0x1a);
                check_int_eq((unsigned char)output_data[1], 0x45);
                check_int_eq((unsigned char)output_data[2], 0xdf);
                check_int_eq((unsigned char)output_data[3], 0xa3);
            }

        cleanup_copy:
            free(output_data);
            turbo_pipeline_destroy(pipeline);
            if (input_path) tt_remove_file(input_path);
            if (output_path) tt_remove_file(output_path);
            free(input_path);
            free(output_path);
        }

        it("resamples and transcodes a local WAV through the graph") {
            char *input_path = tt_make_temp_file("turbo_pipeline_input_", ".wav");
            char *output_path = tt_make_temp_file("turbo_pipeline_output_", ".wav");
            char yaml[4096];
            turbo_pipeline_error_t error;
            turbo_pipeline_stats_t stats;
            turbo_pipeline_t *pipeline = NULL;
            char *output_data = NULL;
            size_t output_size = 0;
            int yaml_size;

            check_not_null(input_path);
            check_not_null(output_path);
            if (!input_path || !output_path) goto cleanup;
            normalize_path(input_path);
            normalize_path(output_path);
            check_int_eq(write_test_wav(input_path), 0);

            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: wav-resample\n"
                "nodes:\n"
                "  - id: source\n"
                "    kind: source\n"
                "    factory: ffmpeg.input\n"
                "    config: { url: '%s' }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: wav } }\n"
                "  - { id: decoder, kind: decoder, factory: ffmpeg.decode, config: { media: audio } }\n"
                "  - { id: filter, kind: filter, factory: ffmpeg.filter, config: { media: audio, filters: volume=0.5 } }\n"
                "  - { id: encoder, kind: encoder, factory: ffmpeg.encode, config: { media: audio, codec: pcm_s16le, sample_rate: 16000, channels: 1 } }\n"
                "  - { id: mux, kind: mux, factory: ffmpeg.mux, config: { format: wav } }\n"
                "  - id: sink\n"
                "    kind: sink\n"
                "    factory: ffmpeg.output\n"
                "    config: { url: '%s' }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.audio, to: decoder.in }\n"
                "  - { from: decoder.out, to: filter.in }\n"
                "  - { from: filter.out, to: encoder.in }\n"
                "  - { from: encoder.out, to: mux.audio }\n"
                "  - { from: mux.out, to: sink.in }\n",
                input_path, output_path);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml)) goto cleanup;

            pipeline = turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, &error);
            if (!pipeline) fprintf(stderr, "pipeline runtime config error: %s\n", error.message);
            check_not_null(pipeline);
            if (!pipeline) goto cleanup;
            check_int_eq(turbo_pipeline_prepare(pipeline, &error), TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED)
                goto cleanup;
            check_int_eq(turbo_pipeline_run(pipeline, &error), TURBO_PIPELINE_OK);
            check_int_eq(turbo_pipeline_state(pipeline), TURBO_PIPELINE_STATE_STOPPED);
            check_int_eq(turbo_pipeline_stats(pipeline, &stats), TURBO_PIPELINE_OK);
            check_true(stats.packets_read > 0);
            check_true(stats.frames_decoded > 0);
            check_true(stats.frames_encoded > 0);
            check_true(stats.bytes_written > 0);

            output_data = tt_read_file(output_path, &output_size);
            check_not_null(output_data);
            check_true(output_size > 44u);
            if (output_data && output_size >= 12u) {
                check_true(memcmp(output_data, "RIFF", 4) == 0);
                check_true(memcmp(output_data + 8, "WAVE", 4) == 0);
            }

        cleanup:
            free(output_data);
            turbo_pipeline_destroy(pipeline);
            if (input_path) tt_remove_file(input_path);
            if (output_path) tt_remove_file(output_path);
            free(input_path);
            free(output_path);
        }

        it("scales and transcodes raw video through the graph") {
            char *input_path = tt_make_temp_file("turbo_pipeline_video_in_", ".yuv");
            char *output_path = tt_make_temp_file("turbo_pipeline_video_out_", ".avi");
            char yaml[4096];
            turbo_pipeline_error_t error;
            turbo_pipeline_stats_t stats;
            turbo_pipeline_t *pipeline = NULL;
            char *output_data = NULL;
            size_t output_size = 0;
            int yaml_size;

            check_not_null(input_path);
            check_not_null(output_path);
            if (!input_path || !output_path) goto cleanup_video;
            normalize_path(input_path);
            normalize_path(output_path);
            check_int_eq(write_test_yuv420p(input_path), 0);
            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: raw-video-scale\n"
                "nodes:\n"
                "  - id: source\n"
                "    kind: source\n"
                "    factory: ffmpeg.input\n"
                "    config:\n"
                "      url: '%s'\n"
                "      options:\n"
                "        video_size: 16x16\n"
                "        pixel_format: yuv420p\n"
                "        framerate: '10'\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: rawvideo } }\n"
                "  - { id: decoder, kind: decoder, factory: ffmpeg.decode, config: { media: video } }\n"
                "  - { id: filter, kind: filter, factory: ffmpeg.filter, config: { media: video, filters: scale=32:32 } }\n"
                "  - { id: encoder, kind: encoder, factory: ffmpeg.encode, config: { media: video, codec: mpeg4, width: 32, height: 32, frame_rate: 10, bitrate: 100000, gop_frames: 10 } }\n"
                "  - { id: mux, kind: mux, factory: ffmpeg.mux, config: { format: avi } }\n"
                "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.video, to: decoder.in }\n"
                "  - { from: decoder.out, to: filter.in }\n"
                "  - { from: filter.out, to: encoder.in }\n"
                "  - { from: encoder.out, to: mux.video }\n"
                "  - { from: mux.out, to: sink.in }\n",
                input_path, output_path);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml)) goto cleanup_video;
            pipeline = turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, &error);
            check_not_null(pipeline);
            if (!pipeline) goto cleanup_video;
            check_int_eq(turbo_pipeline_prepare(pipeline, &error), TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED)
                goto cleanup_video;
            check_int_eq(turbo_pipeline_run(pipeline, &error), TURBO_PIPELINE_OK);
            check_int_eq(turbo_pipeline_stats(pipeline, &stats), TURBO_PIPELINE_OK);
            check_true(stats.frames_decoded >= 2u);
            check_true(stats.frames_encoded >= 2u);
            check_true(stats.packets_written > 0);
            output_data = tt_read_file(output_path, &output_size);
            check_not_null(output_data);
            check_true(output_size > 12u);
            if (output_data && output_size >= 12u) {
                check_true(memcmp(output_data, "RIFF", 4) == 0);
                check_true(memcmp(output_data + 8, "AVI ", 4) == 0);
            }

        cleanup_video:
            free(output_data);
            turbo_pipeline_destroy(pipeline);
            if (input_path) tt_remove_file(input_path);
            if (output_path) tt_remove_file(output_path);
            free(input_path);
            free(output_path);
        }
    }

    group("Runtime RTP") {
        it("relays H264 and Opus RTP through bounded depay/pay adapters") {
            static const char yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: rtc-relay\n"
                "nodes:\n"
                "  - id: source\n"
                "    kind: source\n"
                "    factory: media.runtime_source\n"
                "    config:\n"
                "      url: default/live/whip\n"
                "      options: { queue_capacity: '8', max_packet_bytes: '2048' }\n"
                "  - { id: depay, kind: demux, factory: rtp.depacketize }\n"
                "  - { id: pay, kind: mux, factory: rtp.packetize }\n"
                "  - { id: sink, kind: sink, factory: media.runtime_sink, config: { url: default/live/whep } }\n"
                "edges:\n"
                "  - { from: source.out, to: depay.in }\n"
                "  - { from: depay.audio, to: pay.audio }\n"
                "  - { from: depay.video, to: pay.video }\n"
                "  - { from: pay.out, to: sink.in }\n";
            static const uint8_t h264_payload[] = {0x65, 0x88, 0x84, 0x21};
            static const uint8_t opus_payload[] = {0xf8, 0xff, 0xfe};
            turbo_media_server_runtime_t *runtime = NULL;
            turbo_pipeline_t *pipeline = NULL;
            turbo_media_source_t *input_source = NULL;
            turbo_media_source_t *output_source = NULL;
            turbo_media_source_key_t input_key;
            turbo_media_source_key_t output_key;
            turbo_media_track_info_t video;
            turbo_media_track_info_t audio;
            turbo_media_frame_t frame;
            runtime_rtp_capture_t capture;
            pipeline_run_result_t run_result;
            turbo_thread_t thread = NULL;
            turbo_pipeline_error_t error;
            turbo_pipeline_stats_t stats;
            uint64_t output_subscription = 0;
            uint8_t h264_rtp[256];
            uint8_t opus_rtp[256];
            struct rtp_packet_t parsed;
            int h264_size;
            int opus_size;
            int wait_count;

            memset(&capture, 0, sizeof(capture));
            atomic_init(&capture.count, 0);
            memset(&run_result, 0, sizeof(run_result));
            memset(&video, 0, sizeof(video));
            memset(&audio, 0, sizeof(audio));
            runtime = turbo_media_server_runtime_create(NULL);
            check_not_null(runtime);
            if (!runtime) goto runtime_cleanup;
            check_int_eq(turbo_media_source_key_init(
                             &input_key, "default", "live", "whip"),
                         TURBO_MEDIA_OK);
            check_int_eq(turbo_media_source_key_init(
                             &output_key, "default", "live", "whep"),
                         TURBO_MEDIA_OK);
            check_int_eq(turbo_media_server_runtime_get_or_create_source(
                             runtime, &input_key, &input_source),
                         TURBO_MEDIA_OK);
            video.track_id = 10;
            video.type = TURBO_MEDIA_TRACK_VIDEO;
            memcpy(video.codec_name, "H264", sizeof("H264"));
            video.payload_type = 96;
            video.clock_rate = 90000;
            audio.track_id = 20;
            audio.type = TURBO_MEDIA_TRACK_AUDIO;
            memcpy(audio.codec_name, "opus", sizeof("opus"));
            audio.payload_type = 111;
            audio.clock_rate = 48000;
            audio.sample_rate = 48000;
            audio.channels = 2;
            check_int_eq(turbo_media_source_add_track(input_source, &video, NULL),
                         TURBO_MEDIA_OK);
            check_int_eq(turbo_media_source_add_track(input_source, &audio, NULL),
                         TURBO_MEDIA_OK);

            pipeline = turbo_pipeline_create_from_yaml(
                yaml, sizeof(yaml) - 1u, &error);
            if (!pipeline)
                fprintf(stderr, "Runtime/RTP config error: %s\n", error.message);
            check_not_null(pipeline);
            if (!pipeline) goto runtime_cleanup;
            check_int_eq(turbo_pipeline_bind_server_runtime(
                             pipeline, runtime, &error),
                         TURBO_PIPELINE_OK);
            check_int_eq(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED)
                goto runtime_cleanup;
            check_int_eq(turbo_media_server_runtime_find_source(
                             runtime, &output_key, &output_source),
                         TURBO_MEDIA_OK);
            check_not_null(output_source);
            if (!output_source) goto runtime_cleanup;
            check_int_eq(turbo_media_source_subscribe(
                             output_source, capture_runtime_rtp, &capture, 0,
                             &output_subscription),
                         TURBO_MEDIA_OK);

            run_result.pipeline = pipeline;
            check_int_eq(turbo_thread_create(
                             &thread, run_pipeline_thread, &run_result),
                         0);
            for (wait_count = 0; wait_count < 100 &&
                                 turbo_pipeline_state(pipeline) !=
                                     TURBO_PIPELINE_STATE_RUNNING;
                 ++wait_count)
                turbo_sleep_ms(1);
            check_int_eq(turbo_pipeline_state(pipeline),
                         TURBO_PIPELINE_STATE_RUNNING);

            h264_size = make_rtp_packet(
                h264_rtp, sizeof(h264_rtp), 96, 100, 90000, 0x10203040u, 1,
                h264_payload, sizeof(h264_payload));
            opus_size = make_rtp_packet(
                opus_rtp, sizeof(opus_rtp), 111, 200, 48000, 0x50607080u, 1,
                opus_payload, sizeof(opus_payload));
            check_true(h264_size > 0);
            check_true(opus_size > 0);
            memset(&frame, 0, sizeof(frame));
            frame.track_id = 10;
            frame.data = h264_rtp;
            frame.size = (size_t)h264_size;
            check_int_eq(turbo_media_source_publish(input_source, &frame),
                         TURBO_MEDIA_OK);
            frame.track_id = 20;
            frame.data = opus_rtp;
            frame.size = (size_t)opus_size;
            check_int_eq(turbo_media_source_publish(input_source, &frame),
                         TURBO_MEDIA_OK);
            for (wait_count = 0;
                 wait_count < 1000 &&
                 atomic_load_explicit(&capture.count, memory_order_acquire) < 2;
                 ++wait_count)
                turbo_sleep_ms(1);
            check_int_eq(
                atomic_load_explicit(&capture.count, memory_order_acquire), 2);

            check_int_eq(turbo_pipeline_request_stop(pipeline),
                         TURBO_PIPELINE_OK);
            check_int_eq(turbo_thread_join(&thread), 0);
            turbo_thread_destroy(&thread);
            thread = NULL;
            check_int_eq(run_result.status, TURBO_PIPELINE_ESTOPPED);
            check_int_eq(turbo_pipeline_stats(pipeline, &stats),
                         TURBO_PIPELINE_OK);
            check_int_eq((int)stats.packets_read, 2);
            check_int_eq((int)stats.packets_written, 2);
            check_int_eq((int)stats.frames_decoded, 2);
            check_int_eq((int)stats.frames_encoded, 2);

            check_int_eq(rtp_packet_deserialize(
                             &parsed, capture.packets[0],
                             (int)capture.sizes[0]),
                         0);
            check_int_eq((int)parsed.rtp.pt, 96);
            check_int_eq((int)parsed.rtp.timestamp, 90000);
            check_int_eq((int)parsed.rtp.ssrc, (int)0x10203040u);
            check_int_eq(parsed.payloadlen, (int)sizeof(h264_payload));
            check_true(memcmp(parsed.payload, h264_payload,
                              sizeof(h264_payload)) == 0);
            check_int_eq(rtp_packet_deserialize(
                             &parsed, capture.packets[1],
                             (int)capture.sizes[1]),
                         0);
            check_int_eq((int)parsed.rtp.pt, 111);
            check_int_eq((int)parsed.rtp.timestamp, 48000);
            check_int_eq((int)parsed.rtp.ssrc, (int)0x50607080u);
            check_int_eq(parsed.payloadlen, (int)sizeof(opus_payload));
            check_true(memcmp(parsed.payload, opus_payload,
                              sizeof(opus_payload)) == 0);

        runtime_cleanup:
            if (thread) {
                (void)turbo_pipeline_request_stop(pipeline);
                (void)turbo_thread_join(&thread);
                turbo_thread_destroy(&thread);
            }
            if (output_source && output_subscription)
                (void)turbo_media_source_unsubscribe(
                    output_source, output_subscription);
            turbo_pipeline_destroy(pipeline);
            turbo_media_server_runtime_destroy(runtime);
        }

        it("decodes filters and re-encodes Opus before RTP packetization") {
            static const char yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: rtc-opus-transcode\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: media.runtime_source, config: { url: default/live/opus-in, options: { queue_capacity: '32', max_packet_bytes: '2048' } } }\n"
                "  - { id: depay, kind: demux, factory: rtp.depacketize }\n"
                "  - { id: decoder, kind: decoder, factory: ffmpeg.decode, config: { media: audio } }\n"
                "  - { id: filter, kind: filter, factory: ffmpeg.filter, config: { media: audio, filters: volume=0.5 } }\n"
                "  - { id: encoder, kind: encoder, factory: ffmpeg.encode, config: { media: audio, codec: libopus, bitrate: 32000, sample_rate: 48000, channels: 2 } }\n"
                "  - { id: pay, kind: mux, factory: rtp.packetize }\n"
                "  - { id: sink, kind: sink, factory: media.runtime_sink, config: { url: default/live/opus-out } }\n"
                "edges:\n"
                "  - { from: source.out, to: depay.in }\n"
                "  - { from: depay.audio, to: decoder.in }\n"
                "  - { from: decoder.out, to: filter.in }\n"
                "  - { from: filter.out, to: encoder.in }\n"
                "  - { from: encoder.out, to: pay.audio }\n"
                "  - { from: pay.out, to: sink.in }\n";
            enum {
                SAMPLE_RATE = 48000,
                CHANNELS = 2,
                FRAME_SAMPLES = 960,
                FRAME_COUNT = 20
            };
            turbo_audio_codec_config_t codec_config;
            turbo_codec_t *input_encoder = NULL;
            turbo_media_server_runtime_t *runtime = NULL;
            turbo_pipeline_t *pipeline = NULL;
            turbo_media_source_t *input_source = NULL;
            turbo_media_source_t *output_source = NULL;
            turbo_media_source_key_t input_key;
            turbo_media_source_key_t output_key;
            turbo_media_track_info_t audio;
            turbo_media_track_info_t output_track;
            turbo_media_frame_t frame;
            runtime_rtp_capture_t capture;
            pipeline_run_result_t run_result;
            turbo_thread_t thread = NULL;
            turbo_pipeline_error_t error;
            turbo_pipeline_stats_t stats;
            uint64_t output_subscription = 0;
            int16_t pcm[FRAME_SAMPLES * CHANNELS];
            uint8_t opus_payload[TURBO_CODEC_MAX_FRAME_SIZE];
            uint8_t rtp[2048];
            struct rtp_packet_t parsed;
            int frame_index;
            int wait_count;

            memset(&codec_config, 0, sizeof(codec_config));
            codec_config.sample_rate = SAMPLE_RATE;
            codec_config.channels = CHANNELS;
            codec_config.bitrate = 64000;
            codec_config.frame_size_ms = 20;
            codec_config.complexity = 5;
            memset(&capture, 0, sizeof(capture));
            atomic_init(&capture.count, 0);
            memset(&run_result, 0, sizeof(run_result));
            memset(&audio, 0, sizeof(audio));
            turbo_codec_registry_init();
            input_encoder = turbo_codec_create_encoder("opus", &codec_config);
            check_not_null(input_encoder);
            if (!input_encoder) goto transcode_cleanup;
            runtime = turbo_media_server_runtime_create(NULL);
            check_not_null(runtime);
            if (!runtime) goto transcode_cleanup;
            check_int_eq(turbo_media_source_key_init(
                             &input_key, "default", "live", "opus-in"),
                         TURBO_MEDIA_OK);
            check_int_eq(turbo_media_source_key_init(
                             &output_key, "default", "live", "opus-out"),
                         TURBO_MEDIA_OK);
            check_int_eq(turbo_media_server_runtime_get_or_create_source(
                             runtime, &input_key, &input_source),
                         TURBO_MEDIA_OK);
            audio.track_id = 20;
            audio.type = TURBO_MEDIA_TRACK_AUDIO;
            memcpy(audio.codec_name, "opus", sizeof("opus"));
            audio.payload_type = 111;
            audio.clock_rate = SAMPLE_RATE;
            audio.sample_rate = SAMPLE_RATE;
            audio.channels = CHANNELS;
            check_int_eq(turbo_media_source_add_track(input_source, &audio, NULL),
                         TURBO_MEDIA_OK);
            pipeline = turbo_pipeline_create_from_yaml(
                yaml, sizeof(yaml) - 1u, &error);
            if (!pipeline)
                fprintf(stderr, "Runtime Opus config error: %s\n",
                        error.message);
            check_not_null(pipeline);
            if (!pipeline) goto transcode_cleanup;
            check_int_eq(turbo_pipeline_bind_server_runtime(
                             pipeline, runtime, &error),
                         TURBO_PIPELINE_OK);
            check_int_eq(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) !=
                TURBO_PIPELINE_STATE_PREPARED) {
                fprintf(stderr, "Runtime Opus prepare error: %s\n",
                        error.message);
                goto transcode_cleanup;
            }
            check_int_eq(turbo_media_server_runtime_find_source(
                             runtime, &output_key, &output_source),
                         TURBO_MEDIA_OK);
            check_not_null(output_source);
            if (!output_source) goto transcode_cleanup;
            check_int_eq(turbo_media_source_get_track_at(
                             output_source, 0, &output_track),
                         TURBO_MEDIA_OK);
            check_true(strcmp(output_track.codec_name, "opus") == 0);
            check_int_eq(output_track.clock_rate, SAMPLE_RATE);
            check_int_eq(output_track.sample_rate, SAMPLE_RATE);
            check_int_eq(output_track.channels, CHANNELS);
            check_int_eq(turbo_media_source_subscribe(
                             output_source, capture_runtime_rtp, &capture, 0,
                             &output_subscription),
                         TURBO_MEDIA_OK);
            run_result.pipeline = pipeline;
            check_int_eq(turbo_thread_create(
                             &thread, run_pipeline_thread, &run_result),
                         0);
            for (wait_count = 0;
                 wait_count < 100 &&
                 turbo_pipeline_state(pipeline) !=
                     TURBO_PIPELINE_STATE_RUNNING;
                 ++wait_count)
                turbo_sleep_ms(1);
            check_int_eq(turbo_pipeline_state(pipeline),
                         TURBO_PIPELINE_STATE_RUNNING);

            for (frame_index = 0; frame_index < FRAME_COUNT; ++frame_index) {
                size_t sample_index;
                size_t opus_size = sizeof(opus_payload);
                int rtp_size;
                for (sample_index = 0;
                     sample_index < FRAME_SAMPLES * CHANNELS;
                     ++sample_index)
                    pcm[sample_index] =
                        (int16_t)(((sample_index + (size_t)frame_index * 97u) %
                                  2000u) -
                                 1000);
                check_int_eq(turbo_codec_encode(
                                 input_encoder, (const uint8_t *)pcm,
                                 sizeof(pcm), opus_payload, &opus_size, NULL),
                             TURBO_CODEC_OK);
                rtp_size = make_rtp_packet(
                    rtp, sizeof(rtp), 111,
                    (uint16_t)(300 + frame_index),
                    (uint32_t)(96000 + frame_index * FRAME_SAMPLES),
                    0x55667788u, 1, opus_payload, opus_size);
                check_true(rtp_size > 0);
                memset(&frame, 0, sizeof(frame));
                frame.track_id = 20;
                frame.data = rtp;
                frame.size = (size_t)rtp_size;
                check_int_eq(turbo_media_source_publish(input_source, &frame),
                             TURBO_MEDIA_OK);
            }
            for (wait_count = 0;
                 wait_count < 1000 &&
                 atomic_load_explicit(&capture.count,
                                      memory_order_acquire) < 1 &&
                 turbo_pipeline_state(pipeline) ==
                     TURBO_PIPELINE_STATE_RUNNING;
                 ++wait_count)
                turbo_sleep_ms(1);
            if (turbo_pipeline_state(pipeline) ==
                TURBO_PIPELINE_STATE_RUNNING)
                check_int_eq(turbo_pipeline_request_stop(pipeline),
                             TURBO_PIPELINE_OK);
            check_int_eq(turbo_thread_join(&thread), 0);
            turbo_thread_destroy(&thread);
            thread = NULL;
            if (run_result.status != TURBO_PIPELINE_ESTOPPED)
                fprintf(stderr, "Runtime Opus run error: %s\n",
                        run_result.error.message);
            check_true(atomic_load_explicit(&capture.count,
                                            memory_order_acquire) >= 1);
            check_int_eq(run_result.status, TURBO_PIPELINE_ESTOPPED);
            check_int_eq(turbo_pipeline_stats(pipeline, &stats),
                         TURBO_PIPELINE_OK);
            check_true(stats.frames_decoded >= 1u);
            check_true(stats.frames_encoded >= 1u);
            check_true(stats.packets_written >= 1u);
            check_int_eq(rtp_packet_deserialize(
                             &parsed, capture.packets[1],
                             (int)capture.sizes[1]),
                         0);
            check_int_eq((int)parsed.rtp.pt, 111);
            check_int_eq((int)parsed.rtp.ssrc, (int)0x55667788u);
            check_true(parsed.payloadlen > 0);

        transcode_cleanup:
            if (thread) {
                (void)turbo_pipeline_request_stop(pipeline);
                (void)turbo_thread_join(&thread);
                turbo_thread_destroy(&thread);
            }
            if (output_source && output_subscription)
                (void)turbo_media_source_unsubscribe(
                    output_source, output_subscription);
            turbo_pipeline_destroy(pipeline);
            turbo_media_server_runtime_destroy(runtime);
            turbo_codec_destroy(input_encoder);
            turbo_codec_registry_shutdown();
        }

        it("publishes normalized H264 metadata for a video transcode branch") {
            static const char yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: rtc-h264-metadata\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: media.runtime_source, config: { url: default/live/video-in } }\n"
                "  - { id: depay, kind: demux, factory: rtp.depacketize }\n"
                "  - { id: decoder, kind: decoder, factory: ffmpeg.decode, config: { media: video } }\n"
                "  - { id: filter, kind: filter, factory: ffmpeg.filter, config: { media: video, filters: scale=1280:720 } }\n"
                "  - { id: encoder, kind: encoder, factory: ffmpeg.encode, config: { media: video, codec: libopenh264, bitrate: 2500000, width: 1280, height: 720, frame_rate: 30, gop_frames: 60 } }\n"
                "  - { id: pay, kind: mux, factory: rtp.packetize }\n"
                "  - { id: sink, kind: sink, factory: media.runtime_sink, config: { url: default/live/video-out } }\n"
                "edges:\n"
                "  - { from: source.out, to: depay.in }\n"
                "  - { from: depay.video, to: decoder.in }\n"
                "  - { from: decoder.out, to: filter.in }\n"
                "  - { from: filter.out, to: encoder.in }\n"
                "  - { from: encoder.out, to: pay.video }\n"
                "  - { from: pay.out, to: sink.in }\n";
            static const uint8_t input_extradata[] = {1, 2, 3, 4};
            turbo_media_server_runtime_t *runtime = NULL;
            turbo_pipeline_t *pipeline = NULL;
            turbo_media_source_t *input_source = NULL;
            turbo_media_source_t *output_source = NULL;
            turbo_media_source_key_t input_key;
            turbo_media_source_key_t output_key;
            turbo_media_track_info_t input_track;
            turbo_media_track_info_t output_track;
            turbo_pipeline_error_t error;

            runtime = turbo_media_server_runtime_create(NULL);
            check_not_null(runtime);
            if (!runtime) goto metadata_cleanup;
            check_int_eq(turbo_media_source_key_init(
                             &input_key, "default", "live", "video-in"),
                         TURBO_MEDIA_OK);
            check_int_eq(turbo_media_source_key_init(
                             &output_key, "default", "live", "video-out"),
                         TURBO_MEDIA_OK);
            check_int_eq(turbo_media_server_runtime_get_or_create_source(
                             runtime, &input_key, &input_source),
                         TURBO_MEDIA_OK);
            memset(&input_track, 0, sizeof(input_track));
            input_track.track_id = 10;
            input_track.type = TURBO_MEDIA_TRACK_VIDEO;
            memcpy(input_track.codec_name, "H264", sizeof("H264"));
            input_track.payload_type = 96;
            input_track.clock_rate = 90000;
            input_track.width = 1920;
            input_track.height = 1080;
            input_track.framerate = 60;
            input_track.extradata = input_extradata;
            input_track.extradata_size = sizeof(input_extradata);
            check_int_eq(turbo_media_source_add_track(
                             input_source, &input_track, NULL),
                         TURBO_MEDIA_OK);
            pipeline = turbo_pipeline_create_from_yaml(
                yaml, sizeof(yaml) - 1u, &error);
            check_not_null(pipeline);
            if (!pipeline) goto metadata_cleanup;
            check_int_eq(turbo_pipeline_bind_server_runtime(
                             pipeline, runtime, &error),
                         TURBO_PIPELINE_OK);
            check_int_eq(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) !=
                TURBO_PIPELINE_STATE_PREPARED)
                goto metadata_cleanup;
            check_int_eq(turbo_media_server_runtime_find_source(
                             runtime, &output_key, &output_source),
                         TURBO_MEDIA_OK);
            check_int_eq(turbo_media_source_get_track_at(
                             output_source, 0, &output_track),
                         TURBO_MEDIA_OK);
            check_true(strcmp(output_track.codec_name, "H264") == 0);
            check_int_eq(output_track.payload_type, 96);
            check_int_eq(output_track.clock_rate, 90000);
            check_int_eq(output_track.width, 1280);
            check_int_eq(output_track.height, 720);
            check_int_eq(output_track.framerate, 30);
            check_null(output_track.extradata);
            check_size_eq(output_track.extradata_size, 0);

        metadata_cleanup:
            turbo_pipeline_destroy(pipeline);
            turbo_media_server_runtime_destroy(runtime);
        }

        it("reports bounded queue overflow to publisher and runner") {
            static const char yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: rtc-backpressure\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: media.runtime_source, config: { url: default/live/input, options: { queue_capacity: '1', max_packet_bytes: '64' } } }\n"
                "  - { id: depay, kind: demux, factory: rtp.depacketize }\n"
                "  - { id: pay, kind: mux, factory: rtp.packetize }\n"
                "  - { id: sink, kind: sink, factory: media.runtime_sink, config: { url: default/live/output } }\n"
                "edges:\n"
                "  - { from: source.out, to: depay.in }\n"
                "  - { from: depay.audio, to: pay.audio }\n"
                "  - { from: pay.out, to: sink.in }\n";
            static const uint8_t opus_payload[] = {0xf8, 0xff, 0xfe};
            turbo_media_server_runtime_t *runtime = NULL;
            turbo_media_source_t *input_source = NULL;
            turbo_media_source_key_t input_key;
            turbo_media_track_info_t audio;
            turbo_media_frame_t frame;
            turbo_pipeline_t *pipeline = NULL;
            turbo_pipeline_error_t error;
            uint8_t packet[64];
            int packet_size;

            runtime = turbo_media_server_runtime_create(NULL);
            check_not_null(runtime);
            if (!runtime) goto backpressure_cleanup;
            check_int_eq(turbo_media_source_key_init(
                             &input_key, "default", "live", "input"),
                         TURBO_MEDIA_OK);
            check_int_eq(turbo_media_server_runtime_get_or_create_source(
                             runtime, &input_key, &input_source),
                         TURBO_MEDIA_OK);
            memset(&audio, 0, sizeof(audio));
            audio.track_id = 0;
            audio.type = TURBO_MEDIA_TRACK_AUDIO;
            memcpy(audio.codec_name, "opus", sizeof("opus"));
            audio.payload_type = 111;
            audio.clock_rate = 48000;
            audio.sample_rate = 48000;
            audio.channels = 2;
            check_int_eq(turbo_media_source_add_track(input_source, &audio, NULL),
                         TURBO_MEDIA_OK);
            pipeline = turbo_pipeline_create_from_yaml(
                yaml, sizeof(yaml) - 1u, &error);
            check_not_null(pipeline);
            if (!pipeline) goto backpressure_cleanup;
            check_int_eq(turbo_pipeline_bind_server_runtime(
                             pipeline, runtime, &error),
                         TURBO_PIPELINE_OK);
            check_int_eq(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED)
                goto backpressure_cleanup;
            packet_size = make_rtp_packet(
                packet, sizeof(packet), 111, 1, 48000, 0x11223344u, 1,
                opus_payload, sizeof(opus_payload));
            check_true(packet_size > 0);
            memset(&frame, 0, sizeof(frame));
            frame.track_id = 0;
            frame.data = packet;
            frame.size = (size_t)packet_size;
            check_int_eq(turbo_media_source_publish(input_source, &frame),
                         TURBO_MEDIA_OK);
            packet[3]++;
            check_int_eq(turbo_media_source_publish(input_source, &frame),
                         TURBO_MEDIA_ERR_FULL);
            check_int_eq(turbo_pipeline_run(pipeline, &error),
                         TURBO_PIPELINE_EBACKPRESSURE);
            check_int_eq(turbo_pipeline_state(pipeline),
                         TURBO_PIPELINE_STATE_FAILED);

        backpressure_cleanup:
            turbo_pipeline_destroy(pipeline);
            turbo_media_server_runtime_destroy(runtime);
        }

        it("rolls back a newly created sink source when prepare fails") {
            static const char yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: rtc-rollback\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: media.runtime_source, config: { url: default/live/input } }\n"
                "  - { id: depay, kind: demux, factory: rtp.depacketize }\n"
                "  - { id: pay, kind: mux, factory: rtp.packetize }\n"
                "  - { id: sink, kind: sink, factory: media.runtime_sink, config: { url: default/live/output } }\n"
                "edges:\n"
                "  - { from: source.out, to: depay.in }\n"
                "  - { from: depay.video, to: pay.video }\n"
                "  - { from: pay.out, to: sink.in }\n";
            turbo_media_server_runtime_t *runtime = NULL;
            turbo_media_source_t *input_source = NULL;
            turbo_media_source_t *output_source = NULL;
            turbo_media_source_key_t input_key;
            turbo_media_source_key_t output_key;
            turbo_media_track_info_t video;
            turbo_pipeline_t *pipeline = NULL;
            turbo_pipeline_error_t error;

            runtime = turbo_media_server_runtime_create(NULL);
            check_not_null(runtime);
            if (!runtime) goto rollback_cleanup;
            check_int_eq(turbo_media_source_key_init(
                             &input_key, "default", "live", "input"),
                         TURBO_MEDIA_OK);
            check_int_eq(turbo_media_source_key_init(
                             &output_key, "default", "live", "output"),
                         TURBO_MEDIA_OK);
            check_int_eq(turbo_media_server_runtime_get_or_create_source(
                             runtime, &input_key, &input_source),
                         TURBO_MEDIA_OK);
            memset(&video, 0, sizeof(video));
            video.track_id = 0;
            video.type = TURBO_MEDIA_TRACK_VIDEO;
            memcpy(video.codec_name, "VP8", sizeof("VP8"));
            video.payload_type = 96;
            video.clock_rate = 90000;
            check_int_eq(turbo_media_source_add_track(input_source, &video, NULL),
                         TURBO_MEDIA_OK);
            pipeline = turbo_pipeline_create_from_yaml(
                yaml, sizeof(yaml) - 1u, &error);
            check_not_null(pipeline);
            if (!pipeline) goto rollback_cleanup;
            check_int_eq(turbo_pipeline_bind_server_runtime(
                             pipeline, runtime, &error),
                         TURBO_PIPELINE_OK);
            check_int_eq(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_ECONFIG);
            check_int_eq(turbo_media_server_runtime_find_source(
                             runtime, &output_key, &output_source),
                         TURBO_MEDIA_ERR_NOT_FOUND);

        rollback_cleanup:
            turbo_pipeline_destroy(pipeline);
            turbo_media_server_runtime_destroy(runtime);
        }
    }
}
