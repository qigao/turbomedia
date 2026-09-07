#include "turbo_pipeline.h"

#include <chttp/chttp.h>
#include <rtp-packet.h>
#include <salts/error_codes.h>
#include <tinytest.h>
#include <turbo_codec.h>
#include <turbo_media_server.h>
#include <salts_thread.h>

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIPELINE_RTSP_E2E_PORT 20612
#define PIPELINE_RTSP_E2E_URI "rtsp://127.0.0.1:20612/live/cam"
#define PIPELINE_RTSP_PAYLOAD_TYPE 96
#define PIPELINE_RTSP_CLOCK_RATE 90000
#define PIPELINE_RTSP_TIMESTAMP_STEP 3000
#define PIPELINE_RTSP_SSRC UINT32_C(0x10203040)
#define PIPELINE_RTSP_CLIENT_TIMEOUT_MS 10000

enum {
    PIPELINE_HLS_FIXTURE_FRAME_COUNT = 90,
    PIPELINE_HLS_PATH_CAPACITY = 1024,
    PIPELINE_HLS_YAML_CAPACITY = 8192,
    PIPELINE_HLS_URI_CAPACITY = 256,
    PIPELINE_HLS_PLAYLIST_CAPACITY = 2048,
    PIPELINE_HLS_WAIT_LIMIT = 10000,
    PIPELINE_HLS_STABLE_POLL_COUNT = 50,
    PIPELINE_HLS_SEGMENT_COUNT = 3,
    PIPELINE_HLS_PLAYLIST_REVISION_COUNT = 2,
    PIPELINE_HLS_HTTP_CONNECTION_CAPACITY = 8,
    PIPELINE_HLS_HTTP_ROUTE_CAPACITY = PIPELINE_HLS_SEGMENT_COUNT + 1,
    PIPELINE_HLS_HTTP_MAX_TARGET_BYTES = 1024,
    PIPELINE_HLS_HTTP_MAX_HEADER_COUNT = 16,
    PIPELINE_HLS_HTTP_MAX_HEADER_BYTES = 8 * 1024,
    PIPELINE_HLS_HTTP_MAX_REQUEST_BODY_BYTES = 1024,
    PIPELINE_HLS_HTTP_MAX_BODY_BYTES = 64 * 1024,
    PIPELINE_HLS_HTTP_RESPONSE_WIRE_OVERHEAD_BYTES = 1024,
    PIPELINE_HLS_HTTP_BUFFER_BYTES = 1024 * 1024,
    PIPELINE_HLS_HTTP_POLL_SLICE_MS = 5,
    PIPELINE_HLS_HTTP_STOP_TIMEOUT_MS = 5000
};

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

static int make_test_path(char *path, size_t capacity, const char *root,
                          const char *name) {
    int path_size;
    if (!path || capacity == 0 || !root || !name) return -1;
    path_size = snprintf(path, capacity, "%s/%s", root, name);
    if (path_size <= 0 || (size_t)path_size >= capacity) return -1;
    normalize_path(path);
    return 0;
}

typedef struct pipeline_run_result {
    turbo_pipeline_t *pipeline;
    turbo_pipeline_status_t status;
    turbo_pipeline_error_t error;
} pipeline_run_result_t;

typedef struct pipeline_execute_result {
    turbo_pipeline_t *pipeline;
    atomic_int done;
    turbo_pipeline_status_t status;
    turbo_pipeline_error_t error;
} pipeline_execute_result_t;

typedef struct pipeline_hls_http_server {
    chttp_server http;
    int http_initialized;
    int http_started;
    atomic_int playlist_revision;
    atomic_int playlist_requests;
    atomic_int segment_requests[PIPELINE_HLS_SEGMENT_COUNT];
    atomic_int failed;
    char playlists[PIPELINE_HLS_PLAYLIST_REVISION_COUNT]
                  [PIPELINE_HLS_PLAYLIST_CAPACITY];
    size_t playlist_sizes[PIPELINE_HLS_PLAYLIST_REVISION_COUNT];
    char segment_uris[PIPELINE_HLS_SEGMENT_COUNT][PIPELINE_HLS_URI_CAPACITY];
    char *segment_data[PIPELINE_HLS_SEGMENT_COUNT];
    size_t segment_sizes[PIPELINE_HLS_SEGMENT_COUNT];
} pipeline_hls_http_server_t;

#ifdef TURBO_MEDIA_HAS_RTSP
typedef struct pipeline_rtsp_publisher {
    turbo_media_source_t *source;
    const uint8_t *access_unit;
    size_t access_unit_size;
    int track_id;
    int done;
    int failed;
    int packets_published;
} pipeline_rtsp_publisher_t;
#endif

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

static void execute_pipeline_thread(void *parameter) {
    pipeline_execute_result_t *result =
        (pipeline_execute_result_t *)parameter;
    result->status = turbo_pipeline_prepare(result->pipeline, &result->error);
    if (result->status == TURBO_PIPELINE_OK)
        result->status = turbo_pipeline_run(result->pipeline, &result->error);
    atomic_store_explicit(&result->done, 1, memory_order_release);
}

static int generate_local_hls_fixture(
    const char *root, char *playlist_path, size_t playlist_path_capacity,
    turbo_pipeline_error_t *error) {
    char raw_path[PIPELINE_HLS_PATH_CAPACITY] = {0};
    char yaml[PIPELINE_HLS_YAML_CAPACITY];
    turbo_pipeline_t *writer = NULL;
    turbo_pipeline_status_t status;
    int yaml_size;
    int result = -1;

    if (!root || !playlist_path || !error ||
        make_test_path(raw_path, sizeof(raw_path), root, "input.yuv") != 0 ||
        make_test_path(playlist_path, playlist_path_capacity, root,
                       "fixture.m3u8") != 0 ||
        write_test_yuv420p_frames(raw_path,
                                  PIPELINE_HLS_FIXTURE_FRAME_COUNT) != 0)
        goto cleanup;

    yaml_size = snprintf(
        yaml, sizeof(yaml),
        "api_version: turbo.media.pipeline/v1\n"
        "id: local-hls-fixture-writer\n"
        "nodes:\n"
        "  - id: source\n"
        "    kind: source\n"
        "    factory: ffmpeg.input\n"
        "    config:\n"
        "      url: '%s'\n"
        "      options: { video_size: 16x16, pixel_format: yuv420p, framerate: '10' }\n"
        "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: rawvideo } }\n"
        "  - { id: decoder, kind: decoder, factory: ffmpeg.decode, config: { media: video } }\n"
        "  - { id: encoder, kind: encoder, factory: ffmpeg.encode, config: { media: video, codec: libopenh264, width: 16, height: 16, frame_rate: 10, bitrate: 100000, gop_frames: 30 } }\n"
        "  - id: mux\n"
        "    kind: mux\n"
        "    factory: ffmpeg.mux\n"
        "    config:\n"
        "      format: hls\n"
        "      options: { hls_time: '3', hls_list_size: '0', hls_playlist_type: vod }\n"
        "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
        "edges:\n"
        "  - { from: source.out, to: demux.in }\n"
        "  - { from: demux.video, to: decoder.in }\n"
        "  - { from: decoder.out, to: encoder.in }\n"
        "  - { from: encoder.out, to: mux.video }\n"
        "  - { from: mux.out, to: sink.in }\n",
        raw_path, playlist_path);
    if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml)) goto cleanup;

    writer = turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, error);
    if (!writer) goto cleanup;
    status = turbo_pipeline_prepare(writer, error);
    if (status != TURBO_PIPELINE_OK) goto cleanup;
    status = turbo_pipeline_run(writer, error);
    if (status != TURBO_PIPELINE_OK) goto cleanup;
    result = 0;

cleanup:
    turbo_pipeline_destroy(writer);
    return result;
}

static int collect_hls_segment_uris(
    const char *playlist_data, size_t playlist_size,
    char segment_uris[][PIPELINE_HLS_URI_CAPACITY], size_t required_count) {
    size_t offset = 0;
    size_t found = 0;
    if (!playlist_data || !segment_uris || required_count == 0) return -1;

    while (offset < playlist_size && found < required_count) {
        size_t line_start = offset;
        size_t line_size;
        while (offset < playlist_size && playlist_data[offset] != '\n') ++offset;
        line_size = offset - line_start;
        if (line_size > 0 && playlist_data[line_start + line_size - 1] == '\r')
            --line_size;
        if (line_size > 0 && playlist_data[line_start] != '#') {
            if (line_size >= PIPELINE_HLS_URI_CAPACITY) return -1;
            memcpy(segment_uris[found], playlist_data + line_start, line_size);
            segment_uris[found][line_size] = '\0';
            ++found;
        }
        if (offset < playlist_size) ++offset;
    }
    return found == required_count ? 0 : -1;
}

static int format_live_hls_playlist(
    char *playlist, size_t playlist_capacity,
    char segment_uris[][PIPELINE_HLS_URI_CAPACITY], size_t segment_count,
    size_t *playlist_size) {
    size_t offset = 0;
    size_t segment_index;
    int written;
    static const char header[] =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:3\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXT-X-PLAYLIST-TYPE:EVENT\n";

    if (!playlist || playlist_capacity < sizeof(header) || !segment_uris ||
        segment_count == 0 || !playlist_size)
        return -1;
    memcpy(playlist, header, sizeof(header) - 1u);
    offset = sizeof(header) - 1u;
    for (segment_index = 0; segment_index < segment_count; ++segment_index) {
        written = snprintf(playlist + offset, playlist_capacity - offset,
                           "#EXTINF:3.000000,\n%s\n",
                           segment_uris[segment_index]);
        if (written <= 0 || (size_t)written >= playlist_capacity - offset)
            return -1;
        offset += (size_t)written;
    }
    *playlist_size = offset;
    return 0;
}

static native_io_backend_kind pipeline_hls_http_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static int serve_pipeline_hls_http(
    void *user_data, const chttp_server_request_view *request,
    chttp_server_response *response) {
    pipeline_hls_http_server_t *server =
        (pipeline_hls_http_server_t *)user_data;
    const char *resource;
    const char *body = NULL;
    const char *content_type = NULL;
    size_t body_size = 0;
    int status_code = 200;
    int segment_index;

    if (!server || !request || !response ||
        request->method != CHTTP_METHOD_GET || !request->target ||
        request->target[0] != '/') {
        atomic_store_explicit(&server->failed, 1, memory_order_release);
        return chttp_server_reply(response, 400u, "text/plain", "", 0u);
    }
    resource = request->target + 1;
    if (strcmp(resource, "live.m3u8") == 0) {
        int revision = atomic_load_explicit(&server->playlist_revision,
                                            memory_order_acquire);
        if (revision < 0 ||
            revision >= PIPELINE_HLS_PLAYLIST_REVISION_COUNT) {
            atomic_store_explicit(&server->failed, 1, memory_order_release);
            return chttp_server_reply(response, 500u, "text/plain", "", 0u);
        }
        body = server->playlists[revision];
        body_size = server->playlist_sizes[revision];
        content_type = "application/vnd.apple.mpegurl";
        atomic_fetch_add_explicit(&server->playlist_requests, 1,
                                  memory_order_release);
    } else {
        for (segment_index = 0;
             segment_index < PIPELINE_HLS_SEGMENT_COUNT; ++segment_index) {
            if (strcmp(resource, server->segment_uris[segment_index]) == 0) {
                body = server->segment_data[segment_index];
                body_size = server->segment_sizes[segment_index];
                content_type = "video/mp2t";
                atomic_fetch_add_explicit(
                    &server->segment_requests[segment_index], 1,
                    memory_order_release);
                break;
            }
        }
        if (!body) {
            status_code = 404;
            body = "";
            body_size = 0;
            content_type = "text/plain";
            atomic_store_explicit(&server->failed, 1, memory_order_release);
        }
    }

    if (chttp_server_response_set_header(response, "Cache-Control",
                                         "no-cache") != SALTS_OK) {
        atomic_store_explicit(&server->failed, 1, memory_order_release);
        return SALTS_ENOBUFS;
    }
    if (chttp_server_reply(response, (unsigned int)status_code, content_type,
                           body, body_size) != SALTS_OK) {
        atomic_store_explicit(&server->failed, 1, memory_order_release);
        return SALTS_EIO;
    }
    return SALTS_OK;
}

static int pipeline_hls_http_server_start(
    pipeline_hls_http_server_t *server, uint16_t *bound_port) {
    chttp_server_config config = {
        .host = "127.0.0.1",
        .port = 0u,
        .backlog = PIPELINE_HLS_HTTP_CONNECTION_CAPACITY,
        .network = {
            .backend = pipeline_hls_http_backend(),
            .connection_capacity = PIPELINE_HLS_HTTP_CONNECTION_CAPACITY,
            .command_capacity = PIPELINE_HLS_HTTP_CONNECTION_CAPACITY * 2u,
            .request_capacity = PIPELINE_HLS_HTTP_CONNECTION_CAPACITY * 2u,
            .completion_batch_capacity = PIPELINE_HLS_HTTP_CONNECTION_CAPACITY,
            .event_capacity = PIPELINE_HLS_HTTP_CONNECTION_CAPACITY * 2u,
            .max_send_bytes = PIPELINE_HLS_HTTP_MAX_BODY_BYTES +
                              PIPELINE_HLS_HTTP_MAX_HEADER_BYTES +
                              PIPELINE_HLS_HTTP_RESPONSE_WIRE_OVERHEAD_BYTES,
            .receive_buffer_bytes = PIPELINE_HLS_HTTP_MAX_HEADER_BYTES,
            .connect_timeout_ms = PIPELINE_HLS_HTTP_STOP_TIMEOUT_MS,
            .read_timeout_ms = PIPELINE_HLS_HTTP_STOP_TIMEOUT_MS,
            .write_timeout_ms = PIPELINE_HLS_HTTP_STOP_TIMEOUT_MS
        },
        .route_capacity = PIPELINE_HLS_HTTP_ROUTE_CAPACITY,
        .max_target_bytes = PIPELINE_HLS_HTTP_MAX_TARGET_BYTES,
        .max_header_count = PIPELINE_HLS_HTTP_MAX_HEADER_COUNT,
        .max_header_bytes = PIPELINE_HLS_HTTP_MAX_HEADER_BYTES,
        .max_request_body_bytes = PIPELINE_HLS_HTTP_MAX_REQUEST_BODY_BYTES,
        .max_response_header_count = PIPELINE_HLS_HTTP_MAX_HEADER_COUNT,
        .max_response_header_bytes = PIPELINE_HLS_HTTP_MAX_HEADER_BYTES,
        .max_response_body_bytes = PIPELINE_HLS_HTTP_MAX_BODY_BYTES,
        .max_buffered_response_body_bytes = PIPELINE_HLS_HTTP_MAX_BODY_BYTES,
        .poll_slice_ms = PIPELINE_HLS_HTTP_POLL_SLICE_MS,
        .buffer_capacity_bytes = PIPELINE_HLS_HTTP_BUFFER_BYTES
    };
    char route[PIPELINE_HLS_URI_CAPACITY + 2u];
    const char *stage = "validate";
    int status;
    int segment_index;

    if (!server || !bound_port) return SALTS_EINVAL;
    for (segment_index = 0;
         segment_index < PIPELINE_HLS_PLAYLIST_REVISION_COUNT;
         ++segment_index) {
        if (server->playlist_sizes[segment_index] >
            PIPELINE_HLS_HTTP_MAX_BODY_BYTES)
            return SALTS_EMSGSIZE;
    }
    for (segment_index = 0;
         segment_index < PIPELINE_HLS_SEGMENT_COUNT;
         ++segment_index) {
        if (!server->segment_data[segment_index] ||
            server->segment_sizes[segment_index] >
                PIPELINE_HLS_HTTP_MAX_BODY_BYTES)
            return SALTS_EMSGSIZE;
    }
    stage = "init";
    status = chttp_server_init(&server->http, &config);
    if (status != SALTS_OK) {
        fprintf(stderr, "pipeline HLS CHTTP %s failed: %d\n", stage, status);
        return status;
    }
    server->http_initialized = 1;
    stage = "playlist route";
    status = chttp_server_get(&server->http, "/live.m3u8",
                              serve_pipeline_hls_http, server);
    for (segment_index = 0;
         status == SALTS_OK && segment_index < PIPELINE_HLS_SEGMENT_COUNT;
         ++segment_index) {
        int route_size = snprintf(route, sizeof(route), "/%s",
                                  server->segment_uris[segment_index]);
        if (route_size <= 1 || (size_t)route_size >= sizeof(route)) {
            status = SALTS_EMSGSIZE;
            break;
        }
        stage = "segment route";
        status = chttp_server_get(&server->http, route,
                                  serve_pipeline_hls_http, server);
    }
    if (status == SALTS_OK) {
        stage = "start";
        status = chttp_server_start(&server->http);
    }
    if (status == SALTS_OK) {
        server->http_started = 1;
        stage = "bound port";
        status = chttp_server_port(&server->http, bound_port);
    }
    if (status != SALTS_OK) {
        fprintf(stderr, "pipeline HLS CHTTP %s failed: %d\n", stage, status);
        if (server->http_started)
            (void)chttp_server_stop(&server->http,
                                    PIPELINE_HLS_HTTP_STOP_TIMEOUT_MS);
        (void)chttp_server_destroy(&server->http);
        server->http_started = 0;
        server->http_initialized = 0;
        return status;
    }
    return 0;
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

#ifdef TURBO_MEDIA_HAS_RTSP
static int encode_test_h264_access_unit(uint8_t *output,
                                        size_t *output_size) {
    enum { WIDTH = 16, HEIGHT = 16, YUV_SIZE = WIDTH * HEIGHT * 3 / 2 };
    turbo_video_codec_config_t config;
    turbo_codec_t *encoder;
    turbo_encoded_frame_t frame_info;
    uint8_t yuv[YUV_SIZE];
    int result;

    if (!output || !output_size) return TURBO_CODEC_ERR_INVALID;
    memset(&config, 0, sizeof(config));
    memset(&frame_info, 0, sizeof(frame_info));
    memset(yuv, 128, sizeof(yuv));
    config.width = WIDTH;
    config.height = HEIGHT;
    config.framerate = 30;
    config.bitrate = 100000;
    config.keyframe_interval = 1;
    config.threads = 1;

    turbo_codec_registry_init();
    encoder = turbo_codec_create_encoder("h264", &config);
    if (!encoder) {
        *output_size = 0;
        turbo_codec_registry_shutdown();
        return TURBO_CODEC_ERR_CODEC;
    }
    result = turbo_codec_encode(encoder, yuv, sizeof(yuv), output,
                                output_size, &frame_info);
    if (result != TURBO_CODEC_OK) *output_size = 0;
    turbo_codec_destroy(encoder);
    turbo_codec_registry_shutdown();
    return result;
}

static size_t annexb_start_code_size(const uint8_t *data,
                                     size_t size,
                                     size_t offset) {
    if (!data || offset >= size || size - offset < 3u) return 0;
    if (data[offset] != 0 || data[offset + 1u] != 0) return 0;
    if (data[offset + 2u] == 1u) return 3u;
    if (size - offset >= 4u && data[offset + 2u] == 0u &&
        data[offset + 3u] == 1u)
        return 4u;
    return 0;
}

static void publish_pipeline_rtsp_video(void *parameter) {
    enum {
        MAX_ACCESS_UNITS = 16,
        RTP_PACKET_CAPACITY = 2048,
        PUBLISH_INTERVAL_MS = 5
    };
    pipeline_rtsp_publisher_t *publisher =
        (pipeline_rtsp_publisher_t *)parameter;
    uint8_t packet[RTP_PACKET_CAPACITY];
    uint16_t sequence = 1;
    int access_unit;
    for (access_unit = 0; access_unit < MAX_ACCESS_UNITS; ++access_unit) {
        uint32_t timestamp = PIPELINE_RTSP_CLOCK_RATE +
                             (uint32_t)access_unit *
                                 PIPELINE_RTSP_TIMESTAMP_STEP;
        size_t position = 0;
        while (position < publisher->access_unit_size) {
            turbo_media_frame_t frame;
            size_t start_code_size = annexb_start_code_size(
                publisher->access_unit, publisher->access_unit_size, position);
            size_t nal_start;
            size_t nal_end;
            size_t scan;
            int marker;
            int keyframe;
            if (start_code_size == 0) {
                position++;
                continue;
            }
            nal_start = position + start_code_size;
            nal_end = publisher->access_unit_size;
            for (scan = nal_start; scan < publisher->access_unit_size; ++scan) {
                if (annexb_start_code_size(publisher->access_unit,
                                           publisher->access_unit_size,
                                           scan) != 0) {
                    nal_end = scan;
                    break;
                }
            }
            if (nal_start >= nal_end) {
                position = nal_end;
                continue;
            }
            marker = nal_end == publisher->access_unit_size;
            keyframe = (publisher->access_unit[nal_start] & 0x1fu) == 5u;
            int packet_size = make_rtp_packet(
                packet, sizeof(packet), PIPELINE_RTSP_PAYLOAD_TYPE, sequence++,
                timestamp, PIPELINE_RTSP_SSRC, marker,
                publisher->access_unit + nal_start, nal_end - nal_start);
            if (packet_size <= 0) {
                publisher->failed = 1;
                publisher->done = 1;
                return;
            }
            memset(&frame, 0, sizeof(frame));
            frame.track_id = publisher->track_id;
            frame.data = packet;
            frame.size = (size_t)packet_size;
            frame.pts = timestamp;
            frame.dts = timestamp;
            frame.duration = PIPELINE_RTSP_TIMESTAMP_STEP;
            frame.is_keyframe = keyframe;
            if (turbo_media_source_publish(publisher->source, &frame) !=
                TURBO_MEDIA_OK) {
                publisher->failed = 1;
                publisher->done = 1;
                return;
            }
            publisher->packets_published++;
            position = nal_end;
        }
        salts_sleep_ms(PUBLISH_INTERVAL_MS);
    }
    publisher->done = 1;
}
#endif

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
            char *root = tt_make_temp_dir("turbomedia-pipeline-hls");
            char playlist_path[PIPELINE_HLS_PATH_CAPACITY] = {0};
            char output_path[PIPELINE_HLS_PATH_CAPACITY] = {0};
            char yaml[PIPELINE_HLS_YAML_CAPACITY];
            turbo_pipeline_error_t error;
            turbo_pipeline_t *reader = NULL;
            char *playlist_data = NULL;
            char *output_data = NULL;
            size_t playlist_size = 0;
            size_t output_size = 0;
            int fixture_status;
            int yaml_size;

            check_not_null(root);
            if (!root) goto cleanup_hls_input;
            check_equal(make_test_path(output_path, sizeof(output_path), root,
                                       "output.mkv"),
                        0);
            if (output_path[0] == '\0') goto cleanup_hls_input;
            fixture_status = generate_local_hls_fixture(
                root, playlist_path, sizeof(playlist_path), &error);
            check_equal(fixture_status, 0);
            if (fixture_status != 0) goto cleanup_hls_input;

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
            if (root) check_equal(tt_remove_tree(root), 0);
            free(root);
        }

        it("waits for a local live HLS playlist revision") {
            char *root = tt_make_temp_dir("turbomedia-pipeline-live-hls");
            char fixture_playlist_path[PIPELINE_HLS_PATH_CAPACITY] = {0};
            char output_path[PIPELINE_HLS_PATH_CAPACITY] = {0};
            char segment_path[PIPELINE_HLS_PATH_CAPACITY] = {0};
            char yaml[PIPELINE_HLS_YAML_CAPACITY];
            turbo_pipeline_error_t error;
            turbo_pipeline_t *reader = NULL;
            pipeline_execute_result_t execution;
            pipeline_hls_http_server_t hls_server;
            turbo_pipeline_stats_t stats;
            salts_thread_t reader_thread = NULL;
            char *fixture_playlist_data = NULL;
            char *output_data = NULL;
            size_t fixture_playlist_size = 0;
            size_t output_size = 0;
            uint64_t last_packets = 0;
            uint64_t initial_packets = 0;
            uint16_t server_port = 0;
            int fixture_status;
            int segment_index;
            int yaml_size;
            int wait_count;
            int stable_poll_count = 0;
            int thread_started = 0;
            int stop_requested = 0;

            memset(&execution, 0, sizeof(execution));
            memset(&hls_server, 0, sizeof(hls_server));
            memset(&stats, 0, sizeof(stats));
            atomic_init(&execution.done, 0);
            atomic_init(&hls_server.playlist_revision, 0);
            atomic_init(&hls_server.playlist_requests, 0);
            atomic_init(&hls_server.failed, 0);
            for (segment_index = 0;
                 segment_index < PIPELINE_HLS_SEGMENT_COUNT; ++segment_index)
                atomic_init(&hls_server.segment_requests[segment_index], 0);
            check_not_null(root);
            if (!root) goto cleanup_live_hls_input;
            check_equal(make_test_path(output_path, sizeof(output_path), root,
                                       "live-output.mkv"),
                        0);
            if (output_path[0] == '\0')
                goto cleanup_live_hls_input;

            fixture_status = generate_local_hls_fixture(
                root, fixture_playlist_path, sizeof(fixture_playlist_path),
                &error);
            check_equal(fixture_status, 0);
            if (fixture_status != 0) goto cleanup_live_hls_input;
            fixture_playlist_data =
                tt_read_file(fixture_playlist_path, &fixture_playlist_size);
            check_not_null(fixture_playlist_data);
            if (!fixture_playlist_data) goto cleanup_live_hls_input;
            check_equal(collect_hls_segment_uris(
                            fixture_playlist_data, fixture_playlist_size,
                            hls_server.segment_uris,
                            PIPELINE_HLS_SEGMENT_COUNT),
                        0);
            for (segment_index = 0;
                 segment_index < PIPELINE_HLS_SEGMENT_COUNT; ++segment_index) {
                if (hls_server.segment_uris[segment_index][0] == '\0')
                    goto cleanup_live_hls_input;
                check_equal(make_test_path(
                                segment_path, sizeof(segment_path), root,
                                hls_server.segment_uris[segment_index]),
                            0);
                hls_server.segment_data[segment_index] = tt_read_file(
                    segment_path, &hls_server.segment_sizes[segment_index]);
                check_not_null(hls_server.segment_data[segment_index]);
                if (!hls_server.segment_data[segment_index])
                    goto cleanup_live_hls_input;
            }
            check_equal(format_live_hls_playlist(
                            hls_server.playlists[0],
                            sizeof(hls_server.playlists[0]),
                            hls_server.segment_uris, 2,
                            &hls_server.playlist_sizes[0]),
                        0);
            check_equal(format_live_hls_playlist(
                            hls_server.playlists[1],
                            sizeof(hls_server.playlists[1]),
                            hls_server.segment_uris, 3,
                            &hls_server.playlist_sizes[1]),
                        0);

            check_equal(pipeline_hls_http_server_start(&hls_server,
                                                       &server_port),
                        0);
            check_true(server_port > 0u);
            if (!hls_server.http_started || server_port == 0u)
                goto cleanup_live_hls_input;

            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: local-live-hls-reader\n"
                "limits: { open_timeout_ms: 10000, io_timeout_ms: 10000 }\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: ffmpeg.input, config: { url: 'http://127.0.0.1:%d/live.m3u8' } }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: hls } }\n"
                "  - { id: mux, kind: mux, factory: ffmpeg.mux, config: { format: matroska } }\n"
                "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: '%s' } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.video, to: mux.video }\n"
                "  - { from: mux.out, to: sink.in }\n",
                (int)server_port, output_path);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml))
                goto cleanup_live_hls_input;
            reader =
                turbo_pipeline_create_from_yaml(yaml, (size_t)yaml_size, &error);
            check_not_null(reader);
            if (!reader) goto cleanup_live_hls_input;

            execution.pipeline = reader;
            check_equal(salts_thread_create(&reader_thread,
                                            execute_pipeline_thread, &execution),
                        0);
            if (!reader_thread) goto cleanup_live_hls_input;
            thread_started = 1;

            for (wait_count = 0; wait_count < PIPELINE_HLS_WAIT_LIMIT;
                 ++wait_count) {
                check_equal(turbo_pipeline_stats(reader, &stats),
                            TURBO_PIPELINE_OK);
                if (atomic_load_explicit(&execution.done,
                                         memory_order_acquire))
                    break;
                if (turbo_pipeline_state(reader) ==
                        TURBO_PIPELINE_STATE_RUNNING &&
                    stats.packets_read > 0) {
                    if (stats.packets_read == last_packets)
                        ++stable_poll_count;
                    else
                        stable_poll_count = 0;
                    last_packets = stats.packets_read;
                    if (stable_poll_count >= PIPELINE_HLS_STABLE_POLL_COUNT)
                        break;
                }
                salts_sleep_ms(1);
            }
            if (atomic_load_explicit(&execution.done, memory_order_acquire))
                fprintf(stderr, "local live HLS reader exited early: %s\n",
                        execution.error.message);
            check_false(
                atomic_load_explicit(&execution.done, memory_order_acquire));
            check_equal(turbo_pipeline_state(reader),
                        TURBO_PIPELINE_STATE_RUNNING);
            check_true(stats.packets_read > 0);
            check_true(stable_poll_count >= PIPELINE_HLS_STABLE_POLL_COUNT);
            if (atomic_load_explicit(&execution.done, memory_order_acquire) ||
                stats.packets_read == 0 ||
                stable_poll_count < PIPELINE_HLS_STABLE_POLL_COUNT)
                goto cleanup_live_hls_input;
            initial_packets = stats.packets_read;
            check_true(atomic_load_explicit(&hls_server.playlist_requests,
                                            memory_order_acquire) > 0);
            check_true(atomic_load_explicit(&hls_server.segment_requests[0],
                                            memory_order_acquire) > 0);
            check_true(atomic_load_explicit(&hls_server.segment_requests[1],
                                            memory_order_acquire) > 0);

            atomic_store_explicit(&hls_server.playlist_revision, 1,
                                  memory_order_release);
            for (wait_count = 0; wait_count < PIPELINE_HLS_WAIT_LIMIT;
                 ++wait_count) {
                check_equal(turbo_pipeline_stats(reader, &stats),
                            TURBO_PIPELINE_OK);
                if (stats.packets_read > initial_packets ||
                    atomic_load_explicit(&execution.done,
                                         memory_order_acquire))
                    break;
                salts_sleep_ms(1);
            }
            if (atomic_load_explicit(&execution.done, memory_order_acquire))
                fprintf(
                    stderr,
                    "local live HLS revision failed: status=%d error=%s "
                    "playlist_requests=%d segment_requests=%d/%d/%d "
                    "packets=%llu initial=%llu\n",
                    (int)execution.status, execution.error.message,
                    atomic_load_explicit(&hls_server.playlist_requests,
                                         memory_order_acquire),
                    atomic_load_explicit(&hls_server.segment_requests[0],
                                         memory_order_acquire),
                    atomic_load_explicit(&hls_server.segment_requests[1],
                                         memory_order_acquire),
                    atomic_load_explicit(&hls_server.segment_requests[2],
                                         memory_order_acquire),
                    (unsigned long long)stats.packets_read,
                    (unsigned long long)initial_packets);
            check_false(
                atomic_load_explicit(&execution.done, memory_order_acquire));
            check_true(stats.packets_read > initial_packets);
            check_true(atomic_load_explicit(&hls_server.playlist_requests,
                                            memory_order_acquire) > 1);
            check_true(atomic_load_explicit(&hls_server.segment_requests[2],
                                            memory_order_acquire) > 0);
            check_false(atomic_load_explicit(&hls_server.failed,
                                             memory_order_acquire));
            if (atomic_load_explicit(&execution.done, memory_order_acquire) ||
                stats.packets_read <= initial_packets)
                goto cleanup_live_hls_input;

            check_equal(turbo_pipeline_request_stop(reader),
                        TURBO_PIPELINE_OK);
            stop_requested = 1;
            for (wait_count = 0;
                 wait_count < PIPELINE_HLS_WAIT_LIMIT &&
                 !atomic_load_explicit(&execution.done, memory_order_acquire);
                 ++wait_count) {
                salts_sleep_ms(1);
            }
            check_true(
                atomic_load_explicit(&execution.done, memory_order_acquire));
            if (!atomic_load_explicit(&execution.done, memory_order_acquire))
                goto cleanup_live_hls_input;
            check_equal(salts_thread_join(&reader_thread), 0);
            salts_thread_destroy(&reader_thread);
            thread_started = 0;
            check_equal(execution.status, TURBO_PIPELINE_ESTOPPED);
            check_equal(turbo_pipeline_state(reader),
                        TURBO_PIPELINE_STATE_STOPPED);

            output_data = tt_read_file(output_path, &output_size);
            check_not_null(output_data);
            check_true(output_size > 4u);
            if (output_data && output_size >= 4u) {
                static const unsigned char ebml_header[] = {0x1a, 0x45, 0xdf,
                                                            0xa3};
                check_equal(output_data, ebml_header, sizeof(ebml_header));
            }

        cleanup_live_hls_input:
            if (reader && thread_started &&
                !atomic_load_explicit(&execution.done, memory_order_acquire)) {
                for (wait_count = 0;
                     wait_count < PIPELINE_HLS_WAIT_LIMIT &&
                     !atomic_load_explicit(&execution.done,
                                           memory_order_acquire);
                     ++wait_count) {
                    turbo_pipeline_state_t state =
                        turbo_pipeline_state(reader);
                    if (!stop_requested &&
                        (state == TURBO_PIPELINE_STATE_PREPARED ||
                         state == TURBO_PIPELINE_STATE_RUNNING ||
                         state == TURBO_PIPELINE_STATE_STOPPING) &&
                        turbo_pipeline_request_stop(reader) ==
                            TURBO_PIPELINE_OK)
                        stop_requested = 1;
                    salts_sleep_ms(1);
                }
            }
            if (thread_started) {
                check_equal(salts_thread_join(&reader_thread), 0);
                salts_thread_destroy(&reader_thread);
                if (stop_requested)
                    check_equal(execution.status, TURBO_PIPELINE_ESTOPPED);
            }
            free(output_data);
            free(fixture_playlist_data);
            turbo_pipeline_destroy(reader);
            if (hls_server.http_started) {
                check_equal(chttp_server_stop(
                                &hls_server.http,
                                PIPELINE_HLS_HTTP_STOP_TIMEOUT_MS),
                            SALTS_OK);
                hls_server.http_started = 0;
            }
            if (hls_server.http_initialized) {
                check_equal(chttp_server_destroy(&hls_server.http), SALTS_OK);
                hls_server.http_initialized = 0;
            }
            for (segment_index = 0;
                 segment_index < PIPELINE_HLS_SEGMENT_COUNT; ++segment_index)
                free(hls_server.segment_data[segment_index]);
            if (root) check_equal(tt_remove_tree(root), 0);
            free(root);
        }

#ifdef TURBO_MEDIA_HAS_RTSP
        it("pulls local RTSP TCP video through FFmpeg mux and sink") {
            enum {
                WAIT_LIMIT = 8000,
                YAML_CAPACITY = 2048,
                H264_ACCESS_UNIT_CAPACITY = 4096,
                SOURCE_CAPACITY = 2,
                GOP_CAPACITY = 4,
                RTP_CHANNEL_COUNT = 2
            };
            char yaml[YAML_CAPACITY];
            uint8_t h264_access_unit[H264_ACCESS_UNIT_CAPACITY];
            size_t h264_access_unit_size = sizeof(h264_access_unit);
            turbo_media_server_config_t server_config;
            turbo_media_server_runtime_t *runtime = NULL;
            turbo_media_source_key_t source_key;
            turbo_media_source_t *source = NULL;
            turbo_media_track_info_t track;
            turbo_media_source_stats_t source_stats;
            turbo_rtsp_server_config_t rtsp_config;
            turbo_media_rtsp_server_adapter_config_t adapter_config;
            turbo_media_rtsp_server_adapter_t *adapter = NULL;
            turbo_pipeline_t *pipeline = NULL;
            pipeline_execute_result_t execution;
            pipeline_rtsp_publisher_t publisher;
            salts_thread_t pipeline_thread = NULL;
            turbo_pipeline_error_t create_error;
            turbo_pipeline_stats_t stats;
            int track_id = -1;
            int yaml_size;
            int wait_count;
            int thread_started = 0;

            memset(&server_config, 0, sizeof(server_config));
            memset(&source_key, 0, sizeof(source_key));
            memset(&track, 0, sizeof(track));
            memset(&source_stats, 0, sizeof(source_stats));
            memset(&rtsp_config, 0, sizeof(rtsp_config));
            memset(&adapter_config, 0, sizeof(adapter_config));
            memset(&execution, 0, sizeof(execution));
            memset(&publisher, 0, sizeof(publisher));
            memset(&stats, 0, sizeof(stats));
            atomic_init(&execution.done, 0);

            check_equal(encode_test_h264_access_unit(
                            h264_access_unit, &h264_access_unit_size),
                        TURBO_CODEC_OK);
            check_true(h264_access_unit_size > 0);
            if (h264_access_unit_size == 0) goto cleanup_local_rtsp;

            server_config.max_sources = SOURCE_CAPACITY;
            server_config.source_config.max_tracks = SOURCE_CAPACITY;
            server_config.source_config.max_subscribers = SOURCE_CAPACITY;
            server_config.source_config.gop_capacity = GOP_CAPACITY;
            runtime = turbo_media_server_runtime_create(&server_config);
            check_not_null(runtime);
            if (!runtime) goto cleanup_local_rtsp;

            check_equal(turbo_media_source_key_init(
                            &source_key, "default", "live", "cam"),
                        TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_get_or_create_source(
                            runtime, &source_key, &source),
                        TURBO_MEDIA_OK);
            check_not_null(source);
            if (!source) goto cleanup_local_rtsp;
            track.track_id = -1;
            track.type = TURBO_MEDIA_TRACK_VIDEO;
            memcpy(track.codec_name, "H264", sizeof("H264"));
            track.payload_type = PIPELINE_RTSP_PAYLOAD_TYPE;
            track.clock_rate = PIPELINE_RTSP_CLOCK_RATE;
            track.width = 16;
            track.height = 16;
            track.framerate = 30;
            check_equal(turbo_media_source_add_track(source, &track, &track_id),
                        TURBO_MEDIA_OK);
            check_equal(track_id, 0);

            rtsp_config.bind_host = "127.0.0.1";
            rtsp_config.port = PIPELINE_RTSP_E2E_PORT;
            rtsp_config.client_timeout_ms = PIPELINE_RTSP_CLIENT_TIMEOUT_MS;
            adapter_config.vhost = "default";
            adapter_config.session_id = "pipeline-rtsp-e2e";
            adapter_config.default_rtp_channel_count = RTP_CHANNEL_COUNT;
            adapter_config.replay_cached = 0;
            adapter = turbo_media_server_rtsp_adapter_create(
                runtime, &rtsp_config, &adapter_config);
            check_not_null(adapter);
            if (!adapter) goto cleanup_local_rtsp;
            check_equal(turbo_media_server_rtsp_adapter_start(adapter),
                        TURBO_MEDIA_OK);

            yaml_size = snprintf(
                yaml, sizeof(yaml),
                "api_version: turbo.media.pipeline/v1\n"
                "id: local-rtsp-input\n"
                "limits: { open_timeout_ms: 3000, io_timeout_ms: 3000 }\n"
                "nodes:\n"
                "  - id: source\n"
                "    kind: source\n"
                "    factory: ffmpeg.input\n"
                "    config:\n"
                "      url: '%s'\n"
                "      options: { rtsp_transport: tcp }\n"
                "  - { id: demux, kind: demux, factory: ffmpeg.demux, config: { format: rtsp } }\n"
                "  - { id: mux, kind: mux, factory: ffmpeg.mux, config: { format: 'null' } }\n"
                "  - { id: sink, kind: sink, factory: ffmpeg.output, config: { url: '-' } }\n"
                "edges:\n"
                "  - { from: source.out, to: demux.in }\n"
                "  - { from: demux.video, to: mux.video }\n"
                "  - { from: mux.out, to: sink.in }\n",
                PIPELINE_RTSP_E2E_URI);
            check_true(yaml_size > 0 && (size_t)yaml_size < sizeof(yaml));
            if (yaml_size <= 0 || (size_t)yaml_size >= sizeof(yaml))
                goto cleanup_local_rtsp;
            pipeline = turbo_pipeline_create_from_yaml(
                yaml, (size_t)yaml_size, &create_error);
            if (!pipeline)
                fprintf(stderr, "local RTSP config error: %s\n",
                        create_error.message);
            check_not_null(pipeline);
            if (!pipeline) goto cleanup_local_rtsp;

            execution.pipeline = pipeline;
            check_equal(salts_thread_create(
                            &pipeline_thread, execute_pipeline_thread, &execution),
                        0);
            if (!pipeline_thread) goto cleanup_local_rtsp;
            thread_started = 1;

            for (wait_count = 0; wait_count < WAIT_LIMIT; ++wait_count) {
                check_equal(turbo_media_source_get_stats(source, &source_stats),
                            TURBO_MEDIA_OK);
                if (source_stats.subscriber_count > 0 ||
                    atomic_load_explicit(&execution.done, memory_order_acquire))
                    break;
                salts_sleep_ms(1);
            }
            if (atomic_load_explicit(&execution.done, memory_order_acquire) &&
                source_stats.subscriber_count == 0)
                fprintf(stderr, "local RTSP prepare failed: %s\n",
                        execution.error.message);
            check_equal((int)source_stats.subscriber_count, 1);
            if (source_stats.subscriber_count == 0) goto cleanup_local_rtsp;

            publisher.source = source;
            publisher.access_unit = h264_access_unit;
            publisher.access_unit_size = h264_access_unit_size;
            publisher.track_id = track_id;
            publish_pipeline_rtsp_video(&publisher);
            for (wait_count = 0; wait_count < WAIT_LIMIT; ++wait_count) {
                check_equal(turbo_pipeline_stats(pipeline, &stats),
                            TURBO_PIPELINE_OK);
                if (stats.packets_written > 0 ||
                    atomic_load_explicit(&execution.done, memory_order_acquire))
                    break;
                salts_sleep_ms(1);
            }
            check_false(publisher.failed);
            check_true(publisher.done);
            check_true(publisher.packets_published > 0);
            check_true(stats.packets_read > 0);
            check_true(stats.packets_written > 0);

        cleanup_local_rtsp:
            if (pipeline && thread_started &&
                !atomic_load_explicit(&execution.done, memory_order_acquire)) {
                turbo_pipeline_state_t state = turbo_pipeline_state(pipeline);
                if (state == TURBO_PIPELINE_STATE_PREPARED ||
                    state == TURBO_PIPELINE_STATE_RUNNING ||
                    state == TURBO_PIPELINE_STATE_STOPPING)
                    check_equal(turbo_pipeline_request_stop(pipeline),
                                TURBO_PIPELINE_OK);
                for (wait_count = 0; wait_count < WAIT_LIMIT &&
                                     !atomic_load_explicit(
                                         &execution.done, memory_order_acquire);
                     ++wait_count) {
                    salts_sleep_ms(1);
                }
            }
            if (thread_started) {
                check_equal(salts_thread_join(&pipeline_thread), 0);
                salts_thread_destroy(&pipeline_thread);
                check_equal(execution.status, TURBO_PIPELINE_ESTOPPED);
            }
            if (adapter) {
                check_equal(turbo_media_server_rtsp_adapter_stop(adapter),
                            TURBO_MEDIA_OK);
                turbo_media_server_rtsp_adapter_destroy(adapter);
            }
            turbo_pipeline_destroy(pipeline);
            turbo_media_server_runtime_destroy(runtime);
        }
#endif
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
            salts_thread_t thread = NULL;
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
            check_equal(salts_thread_create(
                             &thread, run_pipeline_thread, &run_result),
                         0);
            for (wait_count = 0; wait_count < 100 &&
                                 turbo_pipeline_state(pipeline) !=
                                     TURBO_PIPELINE_STATE_RUNNING;
                 ++wait_count)
                salts_sleep_ms(1);
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
                salts_sleep_ms(1);
            check_equal(
                atomic_load_explicit(&capture.count, memory_order_acquire), 2);

            check_equal(turbo_pipeline_request_stop(pipeline),
                         TURBO_PIPELINE_OK);
            check_equal(salts_thread_join(&thread), 0);
            salts_thread_destroy(&thread);
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
                (void)salts_thread_join(&thread);
                salts_thread_destroy(&thread);
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
            salts_thread_t thread = NULL;
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
            check_equal(salts_thread_create(
                             &thread, run_pipeline_thread, &run_result),
                         0);
            for (wait_count = 0;
                 wait_count < 100 &&
                 turbo_pipeline_state(pipeline) !=
                     TURBO_PIPELINE_STATE_RUNNING;
                 ++wait_count)
                salts_sleep_ms(1);
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
                salts_sleep_ms(1);
            if (turbo_pipeline_state(pipeline) ==
                TURBO_PIPELINE_STATE_RUNNING)
                check_equal(turbo_pipeline_request_stop(pipeline),
                             TURBO_PIPELINE_OK);
            check_equal(salts_thread_join(&thread), 0);
            salts_thread_destroy(&thread);
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
                (void)salts_thread_join(&thread);
                salts_thread_destroy(&thread);
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
