/**
 * Media Recording Implementation
 * 
 * Records audio/video streams to disk in standard container formats
 * Supports MP4 (H.264/H.265/AAC) and WebM (VP8/VP9/Opus)
 * 
 * Features:
 * - Multi-track recording (audio + video)
 * - Real-time muxing
 * - Timestamp synchronization
 * - Dynamic track addition/removal
 * - Metadata support
 */
#include "turbo_recorder.h"
#include "turbo_recorder_internal.h"
#include "turbo_muxer.h"
#include "rtp-payload.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <salts_str.h>
#include <cstl/vec.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NORPC
#define NOSERVICE
#include <platform.h>
#else
#include <time.h>
#endif

/* =============================================================================
 * Recording Configuration
 * ============================================================================= */

#define MAX_TRACKS 8
#define RECORDER_MAX_ACCESS_UNIT_BYTES (16U * 1024U * 1024U)
#define RECORDER_ANNEX_B_START_CODE_BYTES 4U

/* =============================================================================
 * Track Context
 * ============================================================================= */

typedef struct {
    int active;
    turbo_recorder_track_type_t type;
    turbo_recorder_codec_t codec;
    
    /* Configuration */
    int width;
    int height;
    int framerate;
    int sample_rate;
    int channels;
    
    /* Timing */
    int64_t start_time_us;
    int64_t last_timestamp_us;
    int64_t frame_count;
    
    int rtp_payload_type;
    int muxer_stream_id;
    vec_t extradata;
    vec_t access_unit;
    int buffers_initialized;
    
    /* Statistics */
    int64_t bytes_written;
    int64_t frames_written;
} recorder_track_t;

static uint32_t recorder_track_rtp_clock_rate(const recorder_track_t *track) {
    if (!track) {
        return 0;
    }

    if (track->type == TURBO_RECORDER_TRACK_AUDIO) {
        if (track->sample_rate > 0) {
            return (uint32_t)track->sample_rate;
        }
        return 48000;
    }

    return 90000;
}

/* =============================================================================
 * Recorder Context
 * ============================================================================= */

struct turbo_recorder_t {
    /* Configuration */
    turbo_recorder_format_t format;
    tstr filename;
    
    /* Tracks */
    recorder_track_t tracks[MAX_TRACKS];
    int track_count;
    
    /* Container */
    turbo_muxer_t *muxer;
    int recording;
    int paused;
    
    /* Timing */
    int64_t start_time_us;
    int64_t duration_us;
    int64_t pause_start_us;
    int64_t paused_total_us;
    
    /* Metadata */
    tstr title;
    tstr author;
    tstr comment;
    
    /* Statistics */
    int64_t total_bytes_written;
    
    /* Callbacks */
    void (*on_error)(void *user_data, const char *error);
    void *user_data;
};

struct rtp_recorder_ctx_t {
    turbo_recorder_t *recorder;
    int track_id;
    uint32_t base_timestamp;
    uint32_t clock_rate;
    int base_timestamp_set;
    void *decoder;
    uint32_t access_unit_timestamp;
    int access_unit_timestamp_set;
    int access_unit_keyframe;
    int access_unit_corrupt;
    int callback_error;
};

/* =============================================================================
 * Utility Functions
 * ============================================================================= */

static int64_t get_time_us(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (int64_t)((counter.QuadPart * 1000000) / frequency.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

static void recorder_emit_error(turbo_recorder_t *rec, const char *error) {
    if (rec && rec->on_error && error) {
        rec->on_error(rec->user_data, error);
    }
}

typedef struct {
    int enabled;
    int remaining_calls;
} recorder_test_io_failure_t;

static recorder_test_io_failure_t g_recorder_test_io_failures[6];

static int recorder_test_should_fail_io(turbo_recorder_test_io_op_t op) {
    recorder_test_io_failure_t *failure;

    if (op < TURBO_RECORDER_TEST_IO_OPEN || op > TURBO_RECORDER_TEST_IO_CLOSE) {
        return 0;
    }

    failure = &g_recorder_test_io_failures[(int)op];
    if (!failure->enabled) {
        return 0;
    }

    if (failure->remaining_calls > 0) {
        failure->remaining_calls--;
    }
    if (failure->remaining_calls == 0) {
        failure->enabled = 0;
        return 1;
    }

    return 0;
}

static int recorder_format_supported(turbo_recorder_format_t format) {
    switch (format) {
        case TURBO_RECORDER_FORMAT_MP4:
        case TURBO_RECORDER_FORMAT_WEBM:
        case TURBO_RECORDER_FORMAT_MKV:
            return 1;
        default:
            return 0;
    }
}

static turbo_muxer_format_t recorder_muxer_format(turbo_recorder_format_t format) {
    switch (format) {
        case TURBO_RECORDER_FORMAT_MP4: return TURBO_MUXER_MP4;
        case TURBO_RECORDER_FORMAT_WEBM: return TURBO_MUXER_WEBM;
        case TURBO_RECORDER_FORMAT_MKV: return TURBO_MUXER_MKV;
        default: return (turbo_muxer_format_t)-1;
    }
}

static const char *recorder_codec_name(turbo_recorder_codec_t codec) {
    switch (codec) {
        case TURBO_RECORDER_CODEC_OPUS: return "opus";
        case TURBO_RECORDER_CODEC_AAC: return "aac";
        case TURBO_RECORDER_CODEC_H264: return "h264";
        case TURBO_RECORDER_CODEC_H265: return "h265";
        case TURBO_RECORDER_CODEC_VP8: return "vp8";
        case TURBO_RECORDER_CODEC_VP9: return "vp9";
        case TURBO_RECORDER_CODEC_PCMU: return "pcmu";
        case TURBO_RECORDER_CODEC_PCMA: return "pcma";
        default: return NULL;
    }
}

static int recorder_default_payload_type(turbo_recorder_codec_t codec) {
    switch (codec) {
        case TURBO_RECORDER_CODEC_PCMU: return 0;
        case TURBO_RECORDER_CODEC_PCMA: return 8;
        case TURBO_RECORDER_CODEC_OPUS: return 111;
        case TURBO_RECORDER_CODEC_AAC: return 97;
        case TURBO_RECORDER_CODEC_H264: return 102;
        case TURBO_RECORDER_CODEC_H265: return 103;
        case TURBO_RECORDER_CODEC_VP8: return 96;
        case TURBO_RECORDER_CODEC_VP9: return 98;
        default: return -1;
    }
}

static int recorder_track_supported(const turbo_recorder_t *rec,
                                    const turbo_recorder_track_config_t *config) {
    if (!rec || !config || !recorder_codec_name(config->codec)) {
        return 0;
    }
    if (config->type == TURBO_RECORDER_TRACK_VIDEO) {
        if (config->width <= 0 || config->height <= 0 || config->framerate <= 0) {
            return 0;
        }
        if ((config->codec == TURBO_RECORDER_CODEC_H264 ||
             config->codec == TURBO_RECORDER_CODEC_H265) &&
            (!config->extradata || config->extradata_size == 0)) {
            return 0;
        }
    } else if (config->type == TURBO_RECORDER_TRACK_AUDIO) {
        if (config->sample_rate <= 0 || config->channels <= 0) {
            return 0;
        }
    } else {
        return 0;
    }
    if (rec->format == TURBO_RECORDER_FORMAT_WEBM &&
        config->codec != TURBO_RECORDER_CODEC_VP8 &&
        config->codec != TURBO_RECORDER_CODEC_VP9 &&
        config->codec != TURBO_RECORDER_CODEC_OPUS) {
        return 0;
    }
    if (rec->format == TURBO_RECORDER_FORMAT_MKV &&
        (config->codec == TURBO_RECORDER_CODEC_PCMU ||
         config->codec == TURBO_RECORDER_CODEC_PCMA)) {
        return 0;
    }
    return 1;
}

static int64_t recorder_media_duration_us(const turbo_recorder_t *rec) {
    int64_t media_duration_us = 0;
    int have_media_timestamps = 0;

    if (!rec) {
        return -1;
    }

    for (int i = 0; i < rec->track_count; ++i) {
        if (rec->tracks[i].frames_written > 0) {
            have_media_timestamps = 1;
        }
        if (rec->tracks[i].last_timestamp_us > media_duration_us) {
            media_duration_us = rec->tracks[i].last_timestamp_us;
        }
    }

    return have_media_timestamps ? media_duration_us : -1;
}

static int64_t recorder_elapsed_wallclock_us(const turbo_recorder_t *rec) {
    int64_t now_us;
    int64_t paused_total_us;

    if (!rec) {
        return 0;
    }

    now_us = get_time_us();
    paused_total_us = rec->paused_total_us;
    if (rec->paused && rec->pause_start_us > 0 && now_us > rec->pause_start_us) {
        paused_total_us += now_us - rec->pause_start_us;
    }

    if (now_us <= rec->start_time_us) {
        return 0;
    }

    if (paused_total_us >= (now_us - rec->start_time_us)) {
        return 0;
    }

    return now_us - rec->start_time_us - paused_total_us;
}

/* =============================================================================
 * Recorder Management
 * ============================================================================= */

turbo_recorder_t *turbo_recorder_create(const turbo_recorder_config_t *config) {
    turbo_recorder_t *rec;

    if (!config || !config->filename || !config->filename[0] ||
        !recorder_format_supported(config->format)) {
        return NULL;
    }

    rec = (turbo_recorder_t *)calloc(1, sizeof(turbo_recorder_t));
    if (!rec) return NULL;

    rec->format = config->format;
    rec->filename = tstr_dup(config->filename);
    if (!rec->filename) {
        free(rec);
        return NULL;
    }

    if (config->title) {
        rec->title = tstr_dup(config->title);
    }
    if (config->author) {
        rec->author = tstr_dup(config->author);
    }
    if (config->comment) {
        rec->comment = tstr_dup(config->comment);
    }
    
    return rec;
}

void turbo_recorder_destroy(turbo_recorder_t *rec) {
    if (!rec) return;
    
    /* Stop recording if active */
    if (rec->recording) {
        turbo_recorder_stop(rec);
    }
    
    for (int i = 0; i < rec->track_count; i++) {
        if (rec->tracks[i].buffers_initialized) {
            vec_destroy(&rec->tracks[i].extradata);
            vec_destroy(&rec->tracks[i].access_unit);
        }
    }

    turbo_muxer_destroy(rec->muxer);
    tstr_free(rec->filename);
    tstr_free(rec->title);
    tstr_free(rec->author);
    tstr_free(rec->comment);
    
    free(rec);
}

int turbo_recorder_add_track(turbo_recorder_t *rec,
                             const turbo_recorder_track_config_t *config) {
    recorder_track_t *track;
    int payload_type;

    if (!rec || !config || rec->track_count >= MAX_TRACKS) {
        return -1;
    }
    if (rec->recording || !recorder_track_supported(rec, config) ||
        config->extradata_size > RECORDER_MAX_ACCESS_UNIT_BYTES ||
        (config->extradata_size > 0 && !config->extradata)) {
        return -1;
    }

    payload_type = config->rtp_payload_type;
    if (config->codec != TURBO_RECORDER_CODEC_PCMU && payload_type == 0) {
        payload_type = recorder_default_payload_type(config->codec);
    }
    if (payload_type < 0 || payload_type > 127) {
        return -1;
    }

    track = &rec->tracks[rec->track_count];
    memset(track, 0, sizeof(*track));
    if (vec_init_bytes(&track->extradata, sizeof(uint8_t), CMETA_ALIGNOF(uint8_t),
                       config->extradata_size) != STL_OK ||
        vec_init_bytes(&track->access_unit, sizeof(uint8_t), CMETA_ALIGNOF(uint8_t),
                       RECORDER_MAX_ACCESS_UNIT_BYTES) != STL_OK) {
        vec_destroy(&track->extradata);
        vec_destroy(&track->access_unit);
        return -1;
    }
    track->buffers_initialized = 1;
    if (config->extradata_size > 0) {
        if (vec_resize(&track->extradata, config->extradata_size) != 0) {
            vec_destroy(&track->extradata);
            vec_destroy(&track->access_unit);
            memset(track, 0, sizeof(*track));
            return -1;
        }
        memcpy(vec_data(&track->extradata), config->extradata,
               config->extradata_size);
    }

    track->active = 1;
    track->type = config->type;
    track->codec = config->codec;
    track->rtp_payload_type = payload_type;
    track->muxer_stream_id = -1;

    if (config->type == TURBO_RECORDER_TRACK_VIDEO) {
        track->width = config->width;
        track->height = config->height;
        track->framerate = config->framerate;
    } else {
        track->sample_rate = config->sample_rate;
        track->channels = config->channels;
    }

    return rec->track_count++;
}

int turbo_recorder_start(turbo_recorder_t *rec) {
    turbo_muxer_config_t muxer_config;
    int i;

    if (!rec || rec->recording || rec->track_count == 0) {
        return -1;
    }

    if (!recorder_format_supported(rec->format)) {
        recorder_emit_error(rec, "Unsupported recording format");
        return -1;
    }

    if (recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_OPEN) ||
        (rec->format == TURBO_RECORDER_FORMAT_MP4 &&
         recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_TELL))) {
        recorder_emit_error(rec, "Failed to open file");
        return -1;
    }

    turbo_muxer_registry_init();
    memset(&muxer_config, 0, sizeof(muxer_config));
    muxer_config.format = recorder_muxer_format(rec->format);
    muxer_config.output_path = rec->filename;
    muxer_config.write_duration = 1;
    muxer_config.faststart = rec->format == TURBO_RECORDER_FORMAT_MP4;
    rec->muxer = turbo_muxer_create(&muxer_config);
    if (!rec->muxer) {
        recorder_emit_error(rec, "Failed to create container muxer");
        return -1;
    }

    for (i = 0; i < rec->track_count; ++i) {
        recorder_track_t *track = &rec->tracks[i];
        turbo_stream_info_t stream_info;

        memset(&stream_info, 0, sizeof(stream_info));
        stream_info.type = track->type == TURBO_RECORDER_TRACK_VIDEO
                               ? TURBO_CODEC_TYPE_VIDEO
                               : TURBO_CODEC_TYPE_AUDIO;
        stream_info.codec_name = recorder_codec_name(track->codec);
        stream_info.extradata =
            (const uint8_t *)vec_data_const(&track->extradata);
        stream_info.extradata_size = vec_size(&track->extradata);
        stream_info.width = track->width;
        stream_info.height = track->height;
        stream_info.framerate = track->framerate;
        stream_info.sample_rate = track->sample_rate;
        stream_info.channels = track->channels;
        if (turbo_muxer_add_stream(rec->muxer, &stream_info,
                                   &track->muxer_stream_id) != 0) {
            turbo_muxer_destroy(rec->muxer);
            rec->muxer = NULL;
            remove(rec->filename);
            recorder_emit_error(rec, "Failed to add recording stream");
            return -1;
        }
    }
    if (recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_WRITE) ||
        turbo_muxer_write_header(rec->muxer) != 0) {
        turbo_muxer_destroy(rec->muxer);
        rec->muxer = NULL;
        remove(rec->filename);
        recorder_emit_error(rec, "Failed to write recording header");
        return -1;
    }

    rec->total_bytes_written = 0;
    rec->paused = 0;
    rec->pause_start_us = 0;
    rec->paused_total_us = 0;
    rec->start_time_us = get_time_us();
    rec->recording = 1;

    for (i = 0; i < rec->track_count; i++) {
        rec->tracks[i].start_time_us = rec->start_time_us;
        rec->tracks[i].last_timestamp_us = 0;
    }
    
    return 0;
}

int turbo_recorder_write_frame(turbo_recorder_t *rec, int track_id,
                               const uint8_t *data, size_t len,
                               int64_t timestamp_us, int is_keyframe) {
    recorder_track_t *track;
    turbo_muxer_packet_t packet;

    if (!rec || !rec->recording || track_id < 0 || track_id >= rec->track_count) {
        return -1;
    }
    if (rec->paused) {
        return -1;
    }
    
    if (!data || len == 0 || len > RECORDER_MAX_ACCESS_UNIT_BYTES ||
        timestamp_us < 0 || recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_WRITE)) {
        return -1;
    }

    track = &rec->tracks[track_id];
    memset(&packet, 0, sizeof(packet));
    packet.stream_id = track->muxer_stream_id;
    packet.data = data;
    packet.size = len;
    packet.pts = timestamp_us;
    packet.dts = timestamp_us;
    packet.is_keyframe = is_keyframe ? 1 : 0;
    if (track->type == TURBO_RECORDER_TRACK_VIDEO && track->framerate > 0) {
        packet.duration = 1000000 / track->framerate;
    }
    if (!rec->muxer || turbo_muxer_write_packet(rec->muxer, &packet) != 0) {
        recorder_emit_error(rec, "Failed to mux recording frame");
        return -1;
    }

    if (track->frames_written == 0 || timestamp_us > track->last_timestamp_us) {
        track->last_timestamp_us = timestamp_us;
    }
    track->bytes_written += (int64_t)len;
    track->frames_written++;
    rec->total_bytes_written += (int64_t)len;

    return 0;
}

int turbo_recorder_stop(turbo_recorder_t *rec) {
    int64_t media_duration_us;
    int rc = 0;

    if (!rec || !rec->recording) {
        return -1;
    }

    media_duration_us = recorder_media_duration_us(rec);
    rec->duration_us = media_duration_us >= 0 ? media_duration_us : recorder_elapsed_wallclock_us(rec);
    
    if (recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_FLUSH) ||
        (rec->format == TURBO_RECORDER_FORMAT_MP4 &&
         recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_SEEK)) ||
        !rec->muxer || turbo_muxer_write_trailer(rec->muxer) != 0) {
        rc = -1;
        recorder_emit_error(rec, "Failed to finalize recording container");
    }
    if (recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_CLOSE)) {
        rc = -1;
        recorder_emit_error(rec, "Failed to close recording file");
    }
    turbo_muxer_destroy(rec->muxer);
    rec->muxer = NULL;
    rec->recording = 0;
    rec->paused = 0;
    rec->pause_start_us = 0;

    return rc;
}

int turbo_recorder_pause(turbo_recorder_t *rec) {
    if (!rec || !rec->recording || rec->paused) {
        return -1;
    }
    
    if (recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_FLUSH)) {
        recorder_emit_error(rec, "Failed to flush recording file");
        return -1;
    }

    rec->paused = 1;
    rec->pause_start_us = get_time_us();
    
    return 0;
}

int turbo_recorder_resume(turbo_recorder_t *rec) {
    int64_t now_us;

    if (!rec || !rec->recording || !rec->paused) {
        return -1;
    }

    now_us = get_time_us();
    if (rec->pause_start_us > 0 && now_us > rec->pause_start_us) {
        rec->paused_total_us += now_us - rec->pause_start_us;
    }
    rec->paused = 0;
    rec->pause_start_us = 0;

    return 0;
}

/* =============================================================================
 * Statistics
 * ============================================================================= */

void turbo_recorder_get_stats(turbo_recorder_t *rec,
                              int64_t *duration_us,
                              int64_t *bytes_written,
                              int *track_count) {
    int64_t media_duration_us;

    if (!rec) return;
    
    if (duration_us) {
        if (rec->recording) {
            media_duration_us = recorder_media_duration_us(rec);
            *duration_us = media_duration_us >= 0 ? media_duration_us : recorder_elapsed_wallclock_us(rec);
        } else {
            *duration_us = rec->duration_us;
        }
    }
    
    if (bytes_written) *bytes_written = rec->total_bytes_written;
    if (track_count) *track_count = rec->track_count;
}

void turbo_recorder_get_track_stats(turbo_recorder_t *rec, int track_id,
                                   int64_t *bytes_written,
                                   int64_t *frames_written) {
    if (!rec || track_id < 0 || track_id >= rec->track_count) return;
    
    recorder_track_t *track = &rec->tracks[track_id];
    
    if (bytes_written) *bytes_written = track->bytes_written;
    if (frames_written) *frames_written = track->frames_written;
}

int turbo_recorder_is_recording(turbo_recorder_t *rec) {
    return rec && rec->recording;
}

/* =============================================================================
 * Callbacks
 * ============================================================================= */

void turbo_recorder_set_error_callback(turbo_recorder_t *rec,
                                       void (*callback)(void *user_data,
                                                       const char *error),
                                       void *user_data) {
    if (!rec) return;
    rec->on_error = callback;
    rec->user_data = user_data;
}

void turbo_recorder_test_reset_io_failures(void) {
    memset(g_recorder_test_io_failures, 0, sizeof(g_recorder_test_io_failures));
}

void turbo_recorder_test_fail_io_once(turbo_recorder_test_io_op_t op, int call_index) {
    recorder_test_io_failure_t *failure;

    if (op < TURBO_RECORDER_TEST_IO_OPEN || op > TURBO_RECORDER_TEST_IO_CLOSE) {
        return;
    }

    failure = &g_recorder_test_io_failures[(int)op];
    failure->enabled = 1;
    failure->remaining_calls = call_index > 0 ? call_index : 1;
}

/* =============================================================================
 * Utility: Recording from RTP Stream
 * ============================================================================= */

static int recorder_codec_is_h26x(turbo_recorder_codec_t codec) {
    return codec == TURBO_RECORDER_CODEC_H264 ||
           codec == TURBO_RECORDER_CODEC_H265;
}

static int recorder_vp9_bit(const uint8_t *data, size_t len, size_t bit) {
    if (!data || bit >= len * 8U) {
        return -1;
    }
    return (data[bit / 8U] >> (bit % 8U)) & 1U;
}

static int recorder_vp9_is_keyframe(const uint8_t *data, size_t len) {
    size_t bit = 4;
    int profile;
    int show_existing_frame;
    int frame_type;

    if (!data || len == 0 || (data[0] & 0x03U) != 0x02U) {
        return 0;
    }
    profile = recorder_vp9_bit(data, len, 2) |
              (recorder_vp9_bit(data, len, 3) << 1);
    if (profile < 0) {
        return 0;
    }
    if (profile == 3) {
        bit++;
    }
    show_existing_frame = recorder_vp9_bit(data, len, bit++);
    if (show_existing_frame != 0) {
        return 0;
    }
    frame_type = recorder_vp9_bit(data, len, bit);
    return frame_type == 0;
}

static int recorder_append_h26x_nal(rtp_recorder_ctx_t *ctx,
                                    const uint8_t *data, size_t len,
                                    uint32_t timestamp, int flags) {
    static const uint8_t start_code[RECORDER_ANNEX_B_START_CODE_BYTES] =
        {0x00, 0x00, 0x00, 0x01};
    recorder_track_t *track;
    size_t current_size;
    size_t required;
    int nal_type;

    if (!ctx || !ctx->recorder || !data || len == 0) {
        return -1;
    }
    track = &ctx->recorder->tracks[ctx->track_id];
    if (ctx->access_unit_timestamp_set &&
        ctx->access_unit_timestamp != timestamp) {
        vec_clear(&track->access_unit);
        ctx->access_unit_keyframe = 0;
        ctx->access_unit_corrupt = 0;
    }
    ctx->access_unit_timestamp = timestamp;
    ctx->access_unit_timestamp_set = 1;
    if (flags != 0) {
        ctx->access_unit_corrupt = 1;
        return 0;
    }

    current_size = vec_size(&track->access_unit);
    if (current_size > RECORDER_MAX_ACCESS_UNIT_BYTES -
                           RECORDER_ANNEX_B_START_CODE_BYTES ||
        len > RECORDER_MAX_ACCESS_UNIT_BYTES - current_size -
                  RECORDER_ANNEX_B_START_CODE_BYTES) {
        vec_clear(&track->access_unit);
        ctx->access_unit_corrupt = 1;
        return -1;
    }
    required = current_size + RECORDER_ANNEX_B_START_CODE_BYTES + len;
    if (vec_resize(&track->access_unit, required) != 0) {
        vec_clear(&track->access_unit);
        ctx->access_unit_corrupt = 1;
        return -1;
    }
    memcpy((uint8_t *)vec_data(&track->access_unit) + current_size,
           start_code, sizeof(start_code));
    memcpy((uint8_t *)vec_data(&track->access_unit) + current_size +
               sizeof(start_code),
           data, len);

    if (track->codec == TURBO_RECORDER_CODEC_H264) {
        nal_type = data[0] & 0x1f;
        if (nal_type == 5) {
            ctx->access_unit_keyframe = 1;
        }
    } else if (len >= 2) {
        nal_type = (data[0] >> 1) & 0x3f;
        if (nal_type >= 16 && nal_type <= 21) {
            ctx->access_unit_keyframe = 1;
        }
    }
    return 0;
}

static int recorder_on_depacketized_payload(void *parameter,
                                            const void *packet, int bytes,
                                            uint32_t timestamp, int flags) {
    rtp_recorder_ctx_t *ctx = (rtp_recorder_ctx_t *)parameter;
    recorder_track_t *track;
    const uint8_t *data = (const uint8_t *)packet;
    int keyframe = 1;

    if (!ctx || !ctx->recorder || !packet || bytes <= 0) {
        return -1;
    }
    track = &ctx->recorder->tracks[ctx->track_id];
    if (recorder_codec_is_h26x(track->codec)) {
        if (recorder_append_h26x_nal(ctx, data, (size_t)bytes, timestamp,
                                     flags) != 0) {
            ctx->callback_error = -1;
        }
        return 0;
    }
    if (flags != 0) {
        ctx->callback_error = -1;
        return 0;
    }
    if (track->codec == TURBO_RECORDER_CODEC_VP8) {
        keyframe = (data[0] & 0x01U) == 0;
    } else if (track->codec == TURBO_RECORDER_CODEC_VP9) {
        keyframe = recorder_vp9_is_keyframe(data, (size_t)bytes);
    }
    if (turbo_recorder_write_rtp_frame(ctx, data, (size_t)bytes,
                                       timestamp, keyframe) != 0) {
        ctx->callback_error = -1;
    }
    return 0;
}

rtp_recorder_ctx_t *turbo_recorder_create_rtp_context(turbo_recorder_t *rec,
                                                       int track_id) {
    struct rtp_payload_t handler;
    recorder_track_t *track;
    rtp_recorder_ctx_t *ctx;

    if (!rec || track_id < 0 || track_id >= rec->track_count) {
        return NULL;
    }

    track = &rec->tracks[track_id];
    ctx = (rtp_recorder_ctx_t *)calloc(1, sizeof(rtp_recorder_ctx_t));
    if (!ctx) return NULL;

    ctx->recorder = rec;
    ctx->track_id = track_id;
    ctx->clock_rate = recorder_track_rtp_clock_rate(track);
    memset(&handler, 0, sizeof(handler));
    handler.packet = recorder_on_depacketized_payload;
    ctx->decoder = rtp_payload_decode_create(
        track->rtp_payload_type, recorder_codec_name(track->codec),
        &handler, ctx);
    if (!ctx->decoder || ctx->clock_rate == 0) {
        if (ctx->decoder) {
            rtp_payload_decode_destroy(ctx->decoder);
        }
        free(ctx);
        return NULL;
    }

    return ctx;
}

void turbo_recorder_destroy_rtp_context(rtp_recorder_ctx_t *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->decoder) {
        rtp_payload_decode_destroy(ctx->decoder);
    }
    free(ctx);
}

int turbo_recorder_write_rtp_frame(rtp_recorder_ctx_t *ctx,
                                   const uint8_t *data, size_t len,
                                   uint32_t rtp_timestamp, int is_keyframe) {
    int64_t timestamp_us;

    if (!ctx || !data || len == 0) return -1;
    if (!ctx->recorder || ctx->track_id < 0 || ctx->track_id >= ctx->recorder->track_count) {
        return -1;
    }
    if (ctx->clock_rate == 0) {
        return -1;
    }
    
    if (!ctx->base_timestamp_set) {
        ctx->base_timestamp = rtp_timestamp;
        ctx->base_timestamp_set = 1;
        timestamp_us = 0;
    } else {
        uint32_t diff = rtp_timestamp - ctx->base_timestamp;
        timestamp_us = ((int64_t)diff * 1000000) / ctx->clock_rate;
    }
    return turbo_recorder_write_frame(ctx->recorder, ctx->track_id,
                                     data, len, timestamp_us, is_keyframe);
}

int turbo_recorder_write_rtp_packet(rtp_recorder_ctx_t *ctx,
                                    const uint8_t *packet, size_t len) {
    recorder_track_t *track;
    int marker;
    int decode_rc;
    int write_rc = 0;

    if (!ctx || !ctx->recorder || !ctx->decoder || !packet || len < 12 ||
        len > INT_MAX || ctx->track_id < 0 ||
        ctx->track_id >= ctx->recorder->track_count) {
        return -1;
    }

    track = &ctx->recorder->tracks[ctx->track_id];
    marker = (packet[1] & 0x80U) != 0;
    ctx->callback_error = 0;
    decode_rc = rtp_payload_decode_input(ctx->decoder, packet, (int)len);
    if (decode_rc <= 0 || ctx->callback_error != 0) {
        write_rc = -1;
        if (recorder_codec_is_h26x(track->codec)) {
            vec_clear(&track->access_unit);
            ctx->access_unit_corrupt = 1;
        }
    }

    if (recorder_codec_is_h26x(track->codec) && marker) {
        size_t access_unit_size = vec_size(&track->access_unit);
        if (write_rc == 0 && !ctx->access_unit_corrupt &&
            ctx->access_unit_timestamp_set && access_unit_size > 0) {
            write_rc = turbo_recorder_write_rtp_frame(
                ctx, (const uint8_t *)vec_data_const(&track->access_unit),
                access_unit_size, ctx->access_unit_timestamp,
                ctx->access_unit_keyframe);
        } else {
            write_rc = -1;
        }
        vec_clear(&track->access_unit);
        ctx->access_unit_timestamp_set = 0;
        ctx->access_unit_keyframe = 0;
        ctx->access_unit_corrupt = 0;
    }

    return write_rc;
}
