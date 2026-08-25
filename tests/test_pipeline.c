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

static int write_test_yuv420p_frames(const char *path, size_t frame_count) {
    enum { WIDTH = 16, HEIGHT = 16 };
    const size_t frame_size = WIDTH * HEIGHT * 3u / 2u;
    uint8_t *frames;
    size_t frame_index;
    size_t pixel_index;
    int result;
    if (!path || frame_count == 0 || frame_count > SIZE_MAX / frame_size) return -1;
    frames = (uint8_t *)malloc(frame_size * frame_count);
    if (!frames) return -1;
    memset(frames, 128, frame_size * frame_count);
    for (frame_index = 0; frame_index < frame_count; ++frame_index) {
        uint8_t *luma = frames + frame_index * frame_size;
        for (pixel_index = 0; pixel_index < WIDTH * HEIGHT; ++pixel_index)
            luma[pixel_index] = frame_index % 2u == 0
                                    ? (uint8_t)pixel_index
                                    : (uint8_t)(255u - pixel_index);
    }
    result = tt_write_file(path, frames, frame_size * frame_count);
    free(frames);
    return result;
}

static int write_test_yuv420p(const char *path) {
    enum { FRAME_COUNT = 2 };
    return write_test_yuv420p_frames(path, FRAME_COUNT);
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
                check_equal(turbo_pipeline_state(pipeline),
                             TURBO_PIPELINE_STATE_CREATED);
                turbo_pipeline_destroy(pipeline);
            }
        }

        it("accepts one FFmpeg media branch fanned out to two outputs") {
            static const char yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: copy-fanout\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: input.wav } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux }\n"
                "  - { id: live_mux, kind: mux, factory: ffmpeg.mux, config: { format: matroska } }\n"
                "  - { id: live_sink, kind: sink, factory: ffmpeg.output, config: { url: live.mka } }\n"
                "  - { id: archive_mux, kind: mux, factory: ffmpeg.mux, config: { format: matroska } }\n"
                "  - { id: archive_sink, kind: sink, factory: ffmpeg.output, config: { url: archive.mka } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.audio, to: live_mux.audio }\n"
                "  - { from: demux.audio, to: archive_mux.audio }\n"
                "  - { from: live_mux.out, to: live_sink.in }\n"
                "  - { from: archive_mux.out, to: archive_sink.in }\n";
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline =
                turbo_pipeline_create_from_yaml(yaml, sizeof(yaml) - 1u, &error);

            if (!pipeline) fprintf(stderr, "multi-output config error: %s\n", error.message);
            check_not_null(pipeline);
            turbo_pipeline_destroy(pipeline);
        }

        it("rejects mismatched FFmpeg mux and sink counts") {
            static const char yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: mismatched-outputs\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: input.wav } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux }\n"
                "  - { id: mux_a, kind: mux, factory: ffmpeg.mux }\n"
                "  - { id: mux_b, kind: mux, factory: ffmpeg.mux }\n"
                "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: output.mka } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.audio, to: mux_a.audio }\n"
                "  - { from: demux.audio, to: mux_b.audio }\n"
                "  - { from: mux_a.out, to: sink.in }\n";
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline =
                turbo_pipeline_create_from_yaml(yaml, sizeof(yaml) - 1u, &error);

            check_null(pipeline);
            check_equal(error.code, TURBO_PIPELINE_EGRAPH);
        }

        it("rejects more than eight FFmpeg outputs") {
            enum { OUTPUT_COUNT_OVER_LIMIT = 9 };
            char yaml[8192];
            size_t offset = 0;
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline = NULL;
            int written;
            int output_index;

            written = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: too-many-outputs\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: input.wav } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux }\n");
            check_true(written > 0 && (size_t)written < sizeof(yaml));
            if (written <= 0 || (size_t)written >= sizeof(yaml))
                goto cleanup_output_limit;
            offset = (size_t)written;
            for (output_index = 0; output_index < OUTPUT_COUNT_OVER_LIMIT;
                 ++output_index) {
                written = snprintf(
                    yaml + offset, sizeof(yaml) - offset,
                    "  - { id: mux_%d, kind: mux, factory: ffmpeg.mux }\n"
                    "  - { id: sink_%d, kind: sink, factory: ffmpeg.output, config: { url: output_%d.mka } }\n",
                    output_index, output_index, output_index);
                check_true(written > 0 && (size_t)written < sizeof(yaml) - offset);
                if (written <= 0 || (size_t)written >= sizeof(yaml) - offset)
                    goto cleanup_output_limit;
                offset += (size_t)written;
            }
            written = snprintf(yaml + offset, sizeof(yaml) - offset,
                               "edges:\n"
                               "  - { from: source.out, to: demux.in }\n");
            check_true(written > 0 && (size_t)written < sizeof(yaml) - offset);
            if (written <= 0 || (size_t)written >= sizeof(yaml) - offset)
                goto cleanup_output_limit;
            offset += (size_t)written;
            for (output_index = 0; output_index < OUTPUT_COUNT_OVER_LIMIT;
                 ++output_index) {
                written = snprintf(
                    yaml + offset, sizeof(yaml) - offset,
                    "  - { from: demux.audio, to: mux_%d.audio }\n"
                    "  - { from: mux_%d.out, to: sink_%d.in }\n",
                    output_index, output_index, output_index);
                check_true(written > 0 && (size_t)written < sizeof(yaml) - offset);
                if (written <= 0 || (size_t)written >= sizeof(yaml) - offset)
                    goto cleanup_output_limit;
                offset += (size_t)written;
            }

            pipeline = turbo_pipeline_create_from_yaml(yaml, offset, &error);

            check_null(pipeline);
            check_equal(error.code, TURBO_PIPELINE_EGRAPH);
            check_contains(error.message, "limit of 8");

        cleanup_output_limit:
            turbo_pipeline_destroy(pipeline);
        }

        it("rejects a media branch that omits one FFmpeg output") {
            static const char yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: partial-fanout\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: input.wav } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux }\n"
                "  - { id: mux_a, kind: mux, factory: ffmpeg.mux }\n"
                "  - { id: sink_a, kind: sink, factory: ffmpeg.output, config: { url: a.mka } }\n"
                "  - { id: mux_b, kind: mux, factory: ffmpeg.mux }\n"
                "  - { id: sink_b, kind: sink, factory: ffmpeg.output, config: { url: b.mka } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.audio, to: mux_a.audio }\n"
                "  - { from: mux_a.out, to: sink_a.in }\n"
                "  - { from: mux_b.out, to: sink_b.in }\n";
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline =
                turbo_pipeline_create_from_yaml(yaml, sizeof(yaml) - 1u, &error);

            check_null(pipeline);
            check_equal(error.code, TURBO_PIPELINE_EGRAPH);
        }

        it("keeps Runtime RTP restricted to one output") {
            static const char yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: runtime-multi-output\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: media.runtime_source, config: { url: default/live/whip } }\n"
                "  - { id: depay, kind: demux, factory: rtp.depacketize }\n"
                "  - { id: pay_a, kind: mux, factory: rtp.packetize }\n"
                "  - { id: sink_a, kind: sink, factory: media.runtime_sink, config: { url: default/live/a } }\n"
                "  - { id: pay_b, kind: mux, factory: rtp.packetize }\n"
                "  - { id: sink_b, kind: sink, factory: media.runtime_sink, config: { url: default/live/b } }\n"
                "edges:\n"
                "  - { from: source.out, to: depay.in }\n"
                "  - { from: depay.audio, to: pay_a.audio }\n"
                "  - { from: depay.audio, to: pay_b.audio }\n"
                "  - { from: pay_a.out, to: sink_a.in }\n"
                "  - { from: pay_b.out, to: sink_b.in }\n";
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline =
                turbo_pipeline_create_from_yaml(yaml, sizeof(yaml) - 1u, &error);

            check_null(pipeline);
            check_equal(error.code, TURBO_PIPELINE_EGRAPH);
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
            check_equal(error.code, TURBO_PIPELINE_ECONFIG);
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
            check_equal(error.code, TURBO_PIPELINE_EGRAPH);
        }

        it("rejects unknown YAML fields before graph compilation") {
            char yaml[sizeof(VALID_COPY_YAML) + 32u];
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline;
            int length = snprintf(yaml, sizeof(yaml), "%sunknown: true\n",
                                  VALID_COPY_YAML);

            pipeline = turbo_pipeline_create_from_yaml(yaml, (size_t)length, &error);

            check_null(pipeline);
            check_equal(error.code, TURBO_PIPELINE_ECONFIG);
            check_contains(error.message, "unknown field");
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
            check_equal(error.code, TURBO_PIPELINE_EGRAPH);
            check_contains(error.message, "cycle");
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
            check_equal(error.code, TURBO_PIPELINE_ECONFIG);
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
            check_equal(tt_remove_file(missing_path), 0);
            check_equal(tt_remove_file(output_path), 0);
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

            check_equal(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_EFFMPEG);
            check_equal(turbo_pipeline_state(pipeline),
                         TURBO_PIPELINE_STATE_FAILED);
            check_equal(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_ESTATE);
            check_equal(turbo_pipeline_request_stop(pipeline),
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
            check_equal(write_test_wav(input_path), 0);
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
            check_equal(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_request_stop(pipeline),
                         TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_run(pipeline, &error),
                         TURBO_PIPELINE_ESTOPPED);
            check_equal(turbo_pipeline_state(pipeline),
                         TURBO_PIPELINE_STATE_STOPPED);
            check_equal(turbo_pipeline_stats(pipeline, &stats),
                         TURBO_PIPELINE_OK);
            check_equal((int)stats.packets_read, 0);
            check_equal((int)stats.packets_written, 0);

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
            check_equal(write_test_wav(input_path), 0);
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
            check_equal(turbo_pipeline_prepare(pipeline, &error), TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED)
                goto cleanup_copy;
            check_equal(turbo_pipeline_run(pipeline, &error), TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_stats(pipeline, &stats), TURBO_PIPELINE_OK);
            check_true(stats.packets_read > 0);
            check_true(stats.packets_written > 0);
            check_equal((int)stats.frames_decoded, 0);
            output_data = tt_read_file(output_path, &output_size);
            check_not_null(output_data);
            check_true(output_size > 4u);
            if (output_data && output_size >= 4u) {
                check_equal((unsigned char)output_data[0], 0x1a);
                check_equal((unsigned char)output_data[1], 0x45);
                check_equal((unsigned char)output_data[2], 0xdf);
                check_equal((unsigned char)output_data[3], 0xa3);
            }

        cleanup_copy:
            free(output_data);
            turbo_pipeline_destroy(pipeline);
            if (input_path) tt_remove_file(input_path);
            if (output_path) tt_remove_file(output_path);
            free(input_path);
            free(output_path);
        }

        it("stream-copies one audio branch to two FFmpeg outputs") {
            char *input_path = tt_make_temp_file("turbo_pipeline_fanout_in_", ".wav");
            char *live_path = tt_make_temp_file("turbo_pipeline_fanout_live_", ".mka");
            char *archive_path =
                tt_make_temp_file("turbo_pipeline_fanout_archive_", ".mka");
            char yaml[4096];
            turbo_pipeline_error_t error;
            turbo_pipeline_stats_t stats;
            turbo_pipeline_t *pipeline = NULL;
            char *live_data = NULL;
            char *archive_data = NULL;
            size_t live_size = 0;
            size_t archive_size = 0;
            int yaml_size;

            check_not_null(input_path);
            check_not_null(live_path);
            check_not_null(archive_path);
            if (!input_path || !live_path || !archive_path) goto cleanup_fanout_copy;
            normalize_path(input_path);
            normalize_path(live_path);
            normalize_path(archive_path);
            check_equal(write_test_wav(input_path), 0);
            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: wav-fanout\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: '%s' } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: wav } }\n"
                "  - { id: live_mux, kind: mux, factory: ffmpeg.mux, config: { format: matroska } }\n"
                "  - { id: live_sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "  - { id: archive_mux, kind: mux, factory: ffmpeg.mux, config: { format: matroska } }\n"
                "  - { id: archive_sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.audio, to: live_mux.audio }\n"
                "  - { from: demux.audio, to: archive_mux.audio }\n"
                "  - { from: live_mux.out, to: live_sink.in }\n"
                "  - { from: archive_mux.out, to: archive_sink.in }\n",
                input_path, live_path, archive_path);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml))
                goto cleanup_fanout_copy;
            pipeline = turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, &error);
            if (!pipeline) fprintf(stderr, "fanout config error: %s\n", error.message);
            check_not_null(pipeline);
            if (!pipeline) goto cleanup_fanout_copy;
            check_equal(turbo_pipeline_prepare(pipeline, &error), TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED) {
                fprintf(stderr, "fanout prepare error: %s\n", error.message);
                goto cleanup_fanout_copy;
            }
            check_equal(turbo_pipeline_run(pipeline, &error), TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_stats(pipeline, &stats), TURBO_PIPELINE_OK);
            check_true(stats.packets_read > 0);
            check_equal(stats.packets_written, stats.packets_read * 2u);
            live_data = tt_read_file(live_path, &live_size);
            archive_data = tt_read_file(archive_path, &archive_size);
            check_not_null(live_data);
            check_not_null(archive_data);
            check_true(live_size > 4u);
            check_true(archive_size > 4u);

        cleanup_fanout_copy:
            free(live_data);
            free(archive_data);
            turbo_pipeline_destroy(pipeline);
            if (input_path) tt_remove_file(input_path);
            if (live_path) tt_remove_file(live_path);
            if (archive_path) tt_remove_file(archive_path);
            free(input_path);
            free(live_path);
            free(archive_path);
        }

        it("reports the exact sink when opening a later output fails") {
            char *input_path = tt_make_temp_file("turbo_pipeline_sink_fail_in_", ".wav");
            char *good_path = tt_make_temp_file("turbo_pipeline_sink_good_", ".mka");
            char *blocking_path =
                tt_make_temp_file("turbo_pipeline_sink_block_", ".tmp");
            char bad_path[2048];
            char yaml[4096];
            turbo_pipeline_error_t error;
            turbo_pipeline_t *pipeline = NULL;
            int bad_path_size;
            int yaml_size;

            check_not_null(input_path);
            check_not_null(good_path);
            check_not_null(blocking_path);
            if (!input_path || !good_path || !blocking_path)
                goto cleanup_sink_failure;
            normalize_path(input_path);
            normalize_path(good_path);
            normalize_path(blocking_path);
            check_equal(write_test_wav(input_path), 0);
            bad_path_size = snprintf(bad_path, sizeof(bad_path), "%s/missing.mka",
                                     blocking_path);
            check_true(bad_path_size > 0 &&
                       (size_t)bad_path_size < sizeof(bad_path));
            if (bad_path_size <= 0 || (size_t)bad_path_size >= sizeof(bad_path))
                goto cleanup_sink_failure;
            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: later-sink-failure\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: '%s' } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: wav } }\n"
                "  - { id: good_mux, kind: mux, factory: ffmpeg.mux, config: { format: matroska } }\n"
                "  - { id: good_sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "  - { id: bad_mux, kind: mux, factory: ffmpeg.mux, config: { format: matroska } }\n"
                "  - { id: bad_sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.audio, to: good_mux.audio }\n"
                "  - { from: demux.audio, to: bad_mux.audio }\n"
                "  - { from: good_mux.out, to: good_sink.in }\n"
                "  - { from: bad_mux.out, to: bad_sink.in }\n",
                input_path, good_path, bad_path);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml))
                goto cleanup_sink_failure;
            pipeline = turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, &error);
            check_not_null(pipeline);
            if (!pipeline) goto cleanup_sink_failure;
            check_equal(turbo_pipeline_prepare(pipeline, &error),
                        TURBO_PIPELINE_EFFMPEG);
            check_equal(turbo_pipeline_state(pipeline),
                        TURBO_PIPELINE_STATE_FAILED);
            check_equal(error.node_id, "bad_sink");

        cleanup_sink_failure:
            turbo_pipeline_destroy(pipeline);
            if (input_path) tt_remove_file(input_path);
            if (good_path) tt_remove_file(good_path);
            if (blocking_path) tt_remove_file(blocking_path);
            free(input_path);
            free(good_path);
            free(blocking_path);
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
            check_equal(write_test_wav(input_path), 0);

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
            check_equal(turbo_pipeline_prepare(pipeline, &error), TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED)
                goto cleanup;
            check_equal(turbo_pipeline_run(pipeline, &error), TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_state(pipeline), TURBO_PIPELINE_STATE_STOPPED);
            check_equal(turbo_pipeline_stats(pipeline, &stats), TURBO_PIPELINE_OK);
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

        it("transcodes one audio branch to two FFmpeg outputs") {
            char *input_path = tt_make_temp_file("turbo_pipeline_transcode_in_", ".wav");
            char *first_path = tt_make_temp_file("turbo_pipeline_transcode_a_", ".wav");
            char *second_path = tt_make_temp_file("turbo_pipeline_transcode_b_", ".wav");
            char yaml[4096];
            turbo_pipeline_error_t error;
            turbo_pipeline_stats_t stats;
            turbo_pipeline_t *pipeline = NULL;
            char *first_data = NULL;
            char *second_data = NULL;
            size_t first_size = 0;
            size_t second_size = 0;
            int yaml_size;

            check_not_null(input_path);
            check_not_null(first_path);
            check_not_null(second_path);
            if (!input_path || !first_path || !second_path)
                goto cleanup_fanout_transcode;
            normalize_path(input_path);
            normalize_path(first_path);
            normalize_path(second_path);
            check_equal(write_test_wav(input_path), 0);
            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: wav-transcode-fanout\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: '%s' } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: wav } }\n"
                "  - { id: decoder, kind: decoder, factory: ffmpeg.decode, config: { media: audio } }\n"
                "  - { id: filter, kind: filter, factory: ffmpeg.filter, config: { media: audio, filters: volume=0.5 } }\n"
                "  - { id: encoder, kind: encoder, factory: ffmpeg.encode, config: { media: audio, codec: pcm_s16le, sample_rate: 16000, channels: 1 } }\n"
                "  - { id: mux_a, kind: mux, factory: ffmpeg.mux, config: { format: wav } }\n"
                "  - { id: sink_a, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "  - { id: mux_b, kind: mux, factory: ffmpeg.mux, config: { format: wav } }\n"
                "  - { id: sink_b, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.audio, to: decoder.in }\n"
                "  - { from: decoder.out, to: filter.in }\n"
                "  - { from: filter.out, to: encoder.in }\n"
                "  - { from: encoder.out, to: mux_a.audio }\n"
                "  - { from: encoder.out, to: mux_b.audio }\n"
                "  - { from: mux_a.out, to: sink_a.in }\n"
                "  - { from: mux_b.out, to: sink_b.in }\n",
                input_path, first_path, second_path);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml))
                goto cleanup_fanout_transcode;
            pipeline = turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, &error);
            check_not_null(pipeline);
            if (!pipeline) goto cleanup_fanout_transcode;
            check_equal(turbo_pipeline_prepare(pipeline, &error), TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED)
                goto cleanup_fanout_transcode;
            check_equal(turbo_pipeline_run(pipeline, &error), TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_stats(pipeline, &stats), TURBO_PIPELINE_OK);
            check_true(stats.frames_decoded > 0);
            check_true(stats.frames_encoded > 0);
            check_true(stats.packets_written > 0);
            check_equal(stats.packets_written % 2u, 0);
            first_data = tt_read_file(first_path, &first_size);
            second_data = tt_read_file(second_path, &second_size);
            check_not_null(first_data);
            check_not_null(second_data);
            check_true(first_size > 44u);
            check_true(second_size > 44u);
            if (first_data && second_data && first_size >= 12u &&
                second_size >= 12u) {
                check_true(memcmp(first_data, "RIFF", 4) == 0);
                check_true(memcmp(first_data + 8, "WAVE", 4) == 0);
                check_true(memcmp(second_data, "RIFF", 4) == 0);
                check_true(memcmp(second_data + 8, "WAVE", 4) == 0);
            }

        cleanup_fanout_transcode:
            free(first_data);
            free(second_data);
            turbo_pipeline_destroy(pipeline);
            if (input_path) tt_remove_file(input_path);
            if (first_path) tt_remove_file(first_path);
            if (second_path) tt_remove_file(second_path);
            free(input_path);
            free(first_path);
            free(second_path);
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
            check_equal(write_test_yuv420p(input_path), 0);
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
            check_equal(turbo_pipeline_prepare(pipeline, &error), TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED)
                goto cleanup_video;
            check_equal(turbo_pipeline_run(pipeline, &error), TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_stats(pipeline, &stats), TURBO_PIPELINE_OK);
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

        it("reads a local HLS playlist and remuxes its video stream") {
            enum {
                HLS_TEST_FRAME_COUNT = 30,
                HLS_TEST_PATH_CAPACITY = 1024,
                HLS_TEST_YAML_CAPACITY = 8192
            };
            char *root = tt_make_temp_dir("turbomedia-pipeline-hls");
            char raw_path[HLS_TEST_PATH_CAPACITY] = {0};
            char playlist_path[HLS_TEST_PATH_CAPACITY] = {0};
            char output_path[HLS_TEST_PATH_CAPACITY] = {0};
            char yaml[HLS_TEST_YAML_CAPACITY];
            turbo_pipeline_error_t error;
            turbo_pipeline_t *writer = NULL;
            turbo_pipeline_t *reader = NULL;
            char *playlist_data = NULL;
            char *output_data = NULL;
            size_t playlist_size = 0;
            size_t output_size = 0;
            int path_size;
            int yaml_size;

            check_not_null(root);
            if (!root) goto cleanup_hls_input;
            path_size = snprintf(raw_path, sizeof(raw_path), "%s/input.yuv", root);
            check_true(path_size > 0 && (size_t)path_size < sizeof(raw_path));
            if (path_size <= 0 || (size_t)path_size >= sizeof(raw_path))
                goto cleanup_hls_input;
            path_size = snprintf(playlist_path, sizeof(playlist_path),
                                 "%s/playlist.m3u8", root);
            check_true(path_size > 0 && (size_t)path_size < sizeof(playlist_path));
            if (path_size <= 0 || (size_t)path_size >= sizeof(playlist_path))
                goto cleanup_hls_input;
            path_size = snprintf(output_path, sizeof(output_path), "%s/output.mkv", root);
            check_true(path_size > 0 && (size_t)path_size < sizeof(output_path));
            if (path_size <= 0 || (size_t)path_size >= sizeof(output_path))
                goto cleanup_hls_input;
            normalize_path(raw_path);
            normalize_path(playlist_path);
            normalize_path(output_path);
            check_equal(write_test_yuv420p_frames(raw_path, HLS_TEST_FRAME_COUNT), 0);

            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: local-hls-writer\n"
                "nodes:\n"
                "  - id: source\n"
                "    kind: source\n"
                "    factory: ffmpeg.input\n"
                "    config:\n"
                "      url: '%s'\n"
                "      options: { video_size: 16x16, pixel_format: yuv420p, framerate: '10' }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: rawvideo } }\n"
                "  - { id: decoder, kind: decoder, factory: ffmpeg.decode, config: { media: video } }\n"
                "  - { id: encoder, kind: encoder, factory: ffmpeg.encode, config: { media: video, codec: libopenh264, width: 16, height: 16, frame_rate: 10, bitrate: 100000, gop_frames: 1 } }\n"
                "  - id: mux\n"
                "    kind: mux\n"
                "    factory: ffmpeg.mux\n"
                "    config:\n"
                "      format: hls\n"
                "      options: { hls_time: '0.1', hls_list_size: '0', hls_playlist_type: vod }\n"
                "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.video, to: decoder.in }\n"
                "  - { from: decoder.out, to: encoder.in }\n"
                "  - { from: encoder.out, to: mux.video }\n"
                "  - { from: mux.out, to: sink.in }\n",
                raw_path, playlist_path);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml))
                goto cleanup_hls_input;
            writer = turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, &error);
            check_not_null(writer);
            if (!writer) goto cleanup_hls_input;
            check_equal(turbo_pipeline_prepare(writer, &error), TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(writer) != TURBO_PIPELINE_STATE_PREPARED)
                goto cleanup_hls_input;
            check_equal(turbo_pipeline_run(writer, &error), TURBO_PIPELINE_OK);
            turbo_pipeline_destroy(writer);
            writer = NULL;

            playlist_data = tt_read_file(playlist_path, &playlist_size);
            check_not_null(playlist_data);
            check_true(playlist_size > sizeof("#EXTM3U") - 1u);
            if (!playlist_data) goto cleanup_hls_input;
            check_true(memcmp(playlist_data, "#EXTM3U", sizeof("#EXTM3U") - 1u) == 0);

            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: local-hls-reader\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: '%s' } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: hls } }\n"
                "  - { id: mux, kind: mux, factory: ffmpeg.mux, config: { format: matroska } }\n"
                "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.video, to: mux.video }\n"
                "  - { from: mux.out, to: sink.in }\n",
                playlist_path, output_path);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml))
                goto cleanup_hls_input;
            reader = turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, &error);
            check_not_null(reader);
            if (!reader) goto cleanup_hls_input;
            check_equal(turbo_pipeline_prepare(reader, &error), TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(reader) != TURBO_PIPELINE_STATE_PREPARED)
                goto cleanup_hls_input;
            check_equal(turbo_pipeline_run(reader, &error), TURBO_PIPELINE_OK);

            output_data = tt_read_file(output_path, &output_size);
            check_not_null(output_data);
            check_true(output_size > 4u);
            if (output_data && output_size >= 4u) {
                static const unsigned char ebml_header[] = {0x1a, 0x45, 0xdf, 0xa3};
                check_equal(output_data, ebml_header, sizeof(ebml_header));
            }

        cleanup_hls_input:
            free(output_data);
            free(playlist_data);
            turbo_pipeline_destroy(reader);
            turbo_pipeline_destroy(writer);
            if (root) check_equal(tt_remove_tree(root), 0);
            free(root);
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
            check_equal(turbo_media_source_key_init(
                             &input_key, "default", "live", "whip"),
                         TURBO_MEDIA_OK);
            check_equal(turbo_media_source_key_init(
                             &output_key, "default", "live", "whep"),
                         TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_get_or_create_source(
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
            check_equal(turbo_media_source_add_track(input_source, &video, NULL),
                         TURBO_MEDIA_OK);
            check_equal(turbo_media_source_add_track(input_source, &audio, NULL),
                         TURBO_MEDIA_OK);

            pipeline = turbo_pipeline_create_from_yaml(
                yaml, sizeof(yaml) - 1u, &error);
            if (!pipeline)
                fprintf(stderr, "Runtime/RTP config error: %s\n", error.message);
            check_not_null(pipeline);
            if (!pipeline) goto runtime_cleanup;
            check_equal(turbo_pipeline_bind_server_runtime(
                             pipeline, runtime, &error),
                         TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED)
                goto runtime_cleanup;
            check_equal(turbo_media_server_runtime_find_source(
                             runtime, &output_key, &output_source),
                         TURBO_MEDIA_OK);
            check_not_null(output_source);
            if (!output_source) goto runtime_cleanup;
            check_equal(turbo_media_source_subscribe(
                             output_source, capture_runtime_rtp, &capture, 0,
                             &output_subscription),
                         TURBO_MEDIA_OK);

            run_result.pipeline = pipeline;
            check_equal(turbo_thread_create(
                             &thread, run_pipeline_thread, &run_result),
                         0);
            for (wait_count = 0; wait_count < 100 &&
                                 turbo_pipeline_state(pipeline) !=
                                     TURBO_PIPELINE_STATE_RUNNING;
                 ++wait_count)
                turbo_sleep_ms(1);
            check_equal(turbo_pipeline_state(pipeline),
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
            check_equal(turbo_media_source_publish(input_source, &frame),
                         TURBO_MEDIA_OK);
            frame.track_id = 20;
            frame.data = opus_rtp;
            frame.size = (size_t)opus_size;
            check_equal(turbo_media_source_publish(input_source, &frame),
                         TURBO_MEDIA_OK);
            for (wait_count = 0;
                 wait_count < 1000 &&
                 atomic_load_explicit(&capture.count, memory_order_acquire) < 2;
                 ++wait_count)
                turbo_sleep_ms(1);
            check_equal(
                atomic_load_explicit(&capture.count, memory_order_acquire), 2);

            check_equal(turbo_pipeline_request_stop(pipeline),
                         TURBO_PIPELINE_OK);
            check_equal(turbo_thread_join(&thread), 0);
            turbo_thread_destroy(&thread);
            thread = NULL;
            check_equal(run_result.status, TURBO_PIPELINE_ESTOPPED);
            check_equal(turbo_pipeline_stats(pipeline, &stats),
                         TURBO_PIPELINE_OK);
            check_equal((int)stats.packets_read, 2);
            check_equal((int)stats.packets_written, 2);
            check_equal((int)stats.frames_decoded, 2);
            check_equal((int)stats.frames_encoded, 2);

            check_equal(rtp_packet_deserialize(
                             &parsed, capture.packets[0],
                             (int)capture.sizes[0]),
                         0);
            check_equal((int)parsed.rtp.pt, 96);
            check_equal((int)parsed.rtp.timestamp, 90000);
            check_equal((int)parsed.rtp.ssrc, (int)0x10203040u);
            check_equal(parsed.payloadlen, (int)sizeof(h264_payload));
            check_true(memcmp(parsed.payload, h264_payload,
                              sizeof(h264_payload)) == 0);
            check_equal(rtp_packet_deserialize(
                             &parsed, capture.packets[1],
                             (int)capture.sizes[1]),
                         0);
            check_equal((int)parsed.rtp.pt, 111);
            check_equal((int)parsed.rtp.timestamp, 48000);
            check_equal((int)parsed.rtp.ssrc, (int)0x50607080u);
            check_equal(parsed.payloadlen, (int)sizeof(opus_payload));
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
            check_equal(turbo_media_source_key_init(
                             &input_key, "default", "live", "opus-in"),
                         TURBO_MEDIA_OK);
            check_equal(turbo_media_source_key_init(
                             &output_key, "default", "live", "opus-out"),
                         TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_get_or_create_source(
                             runtime, &input_key, &input_source),
                         TURBO_MEDIA_OK);
            audio.track_id = 20;
            audio.type = TURBO_MEDIA_TRACK_AUDIO;
            memcpy(audio.codec_name, "opus", sizeof("opus"));
            audio.payload_type = 111;
            audio.clock_rate = SAMPLE_RATE;
            audio.sample_rate = SAMPLE_RATE;
            audio.channels = CHANNELS;
            check_equal(turbo_media_source_add_track(input_source, &audio, NULL),
                         TURBO_MEDIA_OK);
            pipeline = turbo_pipeline_create_from_yaml(
                yaml, sizeof(yaml) - 1u, &error);
            if (!pipeline)
                fprintf(stderr, "Runtime Opus config error: %s\n",
                        error.message);
            check_not_null(pipeline);
            if (!pipeline) goto transcode_cleanup;
            check_equal(turbo_pipeline_bind_server_runtime(
                             pipeline, runtime, &error),
                         TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) !=
                TURBO_PIPELINE_STATE_PREPARED) {
                fprintf(stderr, "Runtime Opus prepare error: %s\n",
                        error.message);
                goto transcode_cleanup;
            }
            check_equal(turbo_media_server_runtime_find_source(
                             runtime, &output_key, &output_source),
                         TURBO_MEDIA_OK);
            check_not_null(output_source);
            if (!output_source) goto transcode_cleanup;
            check_equal(turbo_media_source_get_track_at(
                             output_source, 0, &output_track),
                         TURBO_MEDIA_OK);
            check_true(strcmp(output_track.codec_name, "opus") == 0);
            check_equal(output_track.clock_rate, SAMPLE_RATE);
            check_equal(output_track.sample_rate, SAMPLE_RATE);
            check_equal(output_track.channels, CHANNELS);
            check_equal(turbo_media_source_subscribe(
                             output_source, capture_runtime_rtp, &capture, 0,
                             &output_subscription),
                         TURBO_MEDIA_OK);
            run_result.pipeline = pipeline;
            check_equal(turbo_thread_create(
                             &thread, run_pipeline_thread, &run_result),
                         0);
            for (wait_count = 0;
                 wait_count < 100 &&
                 turbo_pipeline_state(pipeline) !=
                     TURBO_PIPELINE_STATE_RUNNING;
                 ++wait_count)
                turbo_sleep_ms(1);
            check_equal(turbo_pipeline_state(pipeline),
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
                check_equal(turbo_codec_encode(
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
                check_equal(turbo_media_source_publish(input_source, &frame),
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
                check_equal(turbo_pipeline_request_stop(pipeline),
                             TURBO_PIPELINE_OK);
            check_equal(turbo_thread_join(&thread), 0);
            turbo_thread_destroy(&thread);
            thread = NULL;
            if (run_result.status != TURBO_PIPELINE_ESTOPPED)
                fprintf(stderr, "Runtime Opus run error: %s\n",
                        run_result.error.message);
            check_true(atomic_load_explicit(&capture.count,
                                            memory_order_acquire) >= 1);
            check_equal(run_result.status, TURBO_PIPELINE_ESTOPPED);
            check_equal(turbo_pipeline_stats(pipeline, &stats),
                         TURBO_PIPELINE_OK);
            check_true(stats.frames_decoded >= 1u);
            check_true(stats.frames_encoded >= 1u);
            check_true(stats.packets_written >= 1u);
            check_equal(rtp_packet_deserialize(
                             &parsed, capture.packets[1],
                             (int)capture.sizes[1]),
                         0);
            check_equal((int)parsed.rtp.pt, 111);
            check_equal((int)parsed.rtp.ssrc, (int)0x55667788u);
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
            check_equal(turbo_media_source_key_init(
                             &input_key, "default", "live", "video-in"),
                         TURBO_MEDIA_OK);
            check_equal(turbo_media_source_key_init(
                             &output_key, "default", "live", "video-out"),
                         TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_get_or_create_source(
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
            check_equal(turbo_media_source_add_track(
                             input_source, &input_track, NULL),
                         TURBO_MEDIA_OK);
            pipeline = turbo_pipeline_create_from_yaml(
                yaml, sizeof(yaml) - 1u, &error);
            check_not_null(pipeline);
            if (!pipeline) goto metadata_cleanup;
            check_equal(turbo_pipeline_bind_server_runtime(
                             pipeline, runtime, &error),
                         TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) !=
                TURBO_PIPELINE_STATE_PREPARED)
                goto metadata_cleanup;
            check_equal(turbo_media_server_runtime_find_source(
                             runtime, &output_key, &output_source),
                         TURBO_MEDIA_OK);
            check_equal(turbo_media_source_get_track_at(
                             output_source, 0, &output_track),
                         TURBO_MEDIA_OK);
            check_true(strcmp(output_track.codec_name, "H264") == 0);
            check_equal(output_track.payload_type, 96);
            check_equal(output_track.clock_rate, 90000);
            check_equal(output_track.width, 1280);
            check_equal(output_track.height, 720);
            check_equal(output_track.framerate, 30);
            check_null(output_track.extradata);
            check_equal(output_track.extradata_size, 0);

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
            check_equal(turbo_media_source_key_init(
                             &input_key, "default", "live", "input"),
                         TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_get_or_create_source(
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
            check_equal(turbo_media_source_add_track(input_source, &audio, NULL),
                         TURBO_MEDIA_OK);
            pipeline = turbo_pipeline_create_from_yaml(
                yaml, sizeof(yaml) - 1u, &error);
            check_not_null(pipeline);
            if (!pipeline) goto backpressure_cleanup;
            check_equal(turbo_pipeline_bind_server_runtime(
                             pipeline, runtime, &error),
                         TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_prepare(pipeline, &error),
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
            check_equal(turbo_media_source_publish(input_source, &frame),
                         TURBO_MEDIA_OK);
            packet[3]++;
            check_equal(turbo_media_source_publish(input_source, &frame),
                         TURBO_MEDIA_ERR_FULL);
            check_equal(turbo_pipeline_run(pipeline, &error),
                         TURBO_PIPELINE_EBACKPRESSURE);
            check_equal(turbo_pipeline_state(pipeline),
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
            check_equal(turbo_media_source_key_init(
                             &input_key, "default", "live", "input"),
                         TURBO_MEDIA_OK);
            check_equal(turbo_media_source_key_init(
                             &output_key, "default", "live", "output"),
                         TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_get_or_create_source(
                             runtime, &input_key, &input_source),
                         TURBO_MEDIA_OK);
            memset(&video, 0, sizeof(video));
            video.track_id = 0;
            video.type = TURBO_MEDIA_TRACK_VIDEO;
            memcpy(video.codec_name, "VP8", sizeof("VP8"));
            video.payload_type = 96;
            video.clock_rate = 90000;
            check_equal(turbo_media_source_add_track(input_source, &video, NULL),
                         TURBO_MEDIA_OK);
            pipeline = turbo_pipeline_create_from_yaml(
                yaml, sizeof(yaml) - 1u, &error);
            check_not_null(pipeline);
            if (!pipeline) goto rollback_cleanup;
            check_equal(turbo_pipeline_bind_server_runtime(
                             pipeline, runtime, &error),
                         TURBO_PIPELINE_OK);
            check_equal(turbo_pipeline_prepare(pipeline, &error),
                         TURBO_PIPELINE_ECONFIG);
            check_equal(turbo_media_server_runtime_find_source(
                             runtime, &output_key, &output_source),
                         TURBO_MEDIA_ERR_NOT_FOUND);

        rollback_cleanup:
            turbo_pipeline_destroy(pipeline);
            turbo_media_server_runtime_destroy(runtime);
        }
    }
}
