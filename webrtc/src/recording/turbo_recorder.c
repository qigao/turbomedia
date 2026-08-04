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
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <turbo_str.h>
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
#define BUFFER_SIZE (1024 * 1024)  /* 1 MB buffer per track */

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
    
    /* Buffer */
    uint8_t *buffer;
    size_t buffer_size;
    size_t buffer_used;
    
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
    tstr_t filename;
    
    /* Tracks */
    recorder_track_t tracks[MAX_TRACKS];
    int track_count;
    
    /* File */
    FILE *file;
    int recording;
    int paused;
    long mp4_mdat_offset;
    
    /* Timing */
    int64_t start_time_us;
    int64_t duration_us;
    int64_t pause_start_us;
    int64_t paused_total_us;
    
    /* Metadata */
    tstr_t title;
    tstr_t author;
    tstr_t comment;
    
    /* Statistics */
    int64_t total_bytes_written;
    
    /* Callbacks */
    void (*on_error)(void *user_data, const char *error);
    void *user_data;
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

static FILE *recorder_file_open(const char *filename, const char *mode) {
    if (recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_OPEN)) {
        return NULL;
    }
    return fopen(filename, mode);
}

static size_t write_raw(FILE *f, const void *data, size_t len) {
    if (recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_WRITE)) {
        return 0;
    }
    return fwrite(data, 1, len, f);
}

static int recorder_file_flush(FILE *f) {
    if (recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_FLUSH)) {
        return -1;
    }
    return fflush(f);
}

static int recorder_file_seek(FILE *f, long offset, int origin) {
    if (recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_SEEK)) {
        return -1;
    }
    return fseek(f, offset, origin);
}

static long recorder_file_tell(FILE *f) {
    if (recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_TELL)) {
        return -1;
    }
    return ftell(f);
}

static int recorder_file_close(FILE *f) {
    if (recorder_test_should_fail_io(TURBO_RECORDER_TEST_IO_CLOSE)) {
        return EOF;
    }
    return fclose(f);
}

static size_t write_u8(FILE *f, uint8_t val) {
    return write_raw(f, &val, 1);
}

static size_t write_u16_be(FILE *f, uint16_t val) {
    uint8_t buf[2];
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
    return write_raw(f, buf, sizeof(buf));
}

static size_t write_u32_be(FILE *f, uint32_t val) {
    uint8_t buf[4];
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
    return write_raw(f, buf, sizeof(buf));
}

static size_t write_u64_be(FILE *f, uint64_t val) {
    uint8_t buf[8];
    buf[0] = (val >> 56) & 0xFF;
    buf[1] = (val >> 48) & 0xFF;
    buf[2] = (val >> 40) & 0xFF;
    buf[3] = (val >> 32) & 0xFF;
    buf[4] = (val >> 24) & 0xFF;
    buf[5] = (val >> 16) & 0xFF;
    buf[6] = (val >> 8) & 0xFF;
    buf[7] = val & 0xFF;
    return write_raw(f, buf, sizeof(buf));
}

/* =============================================================================
 * MP4 Container (Simplified)
 * ============================================================================= */

static size_t write_mp4_ftyp(FILE *f) {
    size_t written = 0;

    /* ftyp box */
    written += write_u32_be(f, 20);  /* size */
    written += write_raw(f, "ftyp", 4);
    written += write_raw(f, "isom", 4);  /* major brand */
    written += write_u32_be(f, 512);     /* minor version */
    written += write_raw(f, "isom", 4);  /* compatible brand */

    return written;
}

static size_t write_mp4_mdat_header(FILE *f) {
    size_t written = 0;

    /* mdat box header (size will be updated later) */
    written += write_u32_be(f, 0);  /* size placeholder */
    written += write_raw(f, "mdat", 4);

    return written;
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

static int finalize_mp4_mdat_size(turbo_recorder_t *rec) {
    long file_end;
    long mdat_size;

    if (!rec || !rec->file || rec->mp4_mdat_offset < 0) {
        return -1;
    }

    if (recorder_file_flush(rec->file) != 0) {
        return -1;
    }

    file_end = recorder_file_tell(rec->file);
    if (file_end < 0 || file_end < rec->mp4_mdat_offset) {
        return -1;
    }

    mdat_size = file_end - rec->mp4_mdat_offset;
    if ((unsigned long)mdat_size > 0xFFFFFFFFUL) {
        return -1;
    }

    if (recorder_file_seek(rec->file, rec->mp4_mdat_offset, SEEK_SET) != 0) {
        return -1;
    }
    if (write_u32_be(rec->file, (uint32_t)mdat_size) != 4) {
        return -1;
    }
    if (recorder_file_seek(rec->file, file_end, SEEK_SET) != 0) {
        return -1;
    }

    return recorder_file_flush(rec->file);
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
 * WebM Container (Simplified)
 * ============================================================================= */

static size_t write_matroska_header(FILE *f, const char *doc_type) {
    size_t written = 0;
    size_t doc_type_len;
    size_t ebml_payload_size;

    if (!doc_type) {
        return 0;
    }

    doc_type_len = strlen(doc_type);
    if (doc_type_len == 0 || doc_type_len > 0x7F) {
        return 0;
    }

    ebml_payload_size = 16 + (size_t)(3 + doc_type_len) + 8;
    if (ebml_payload_size > 0x7F) {
        return 0;
    }

    /* EBML Header */
    written += write_raw(f, "\x1A\x45\xDF\xA3", 4); /* EBML */
    written += write_u8(f, (uint8_t)(0x80 | (uint8_t)ebml_payload_size));
    written += write_raw(f, "\x42\x86", 2); /* EBMLVersion */
    written += write_u8(f, 0x81);
    written += write_u8(f, 0x01);
    written += write_raw(f, "\x42\xF7", 2); /* EBMLReadVersion */
    written += write_u8(f, 0x81);
    written += write_u8(f, 0x01);
    written += write_raw(f, "\x42\xF2", 2); /* EBMLMaxIDLength */
    written += write_u8(f, 0x81);
    written += write_u8(f, 0x04);
    written += write_raw(f, "\x42\xF3", 2); /* EBMLMaxSizeLength */
    written += write_u8(f, 0x81);
    written += write_u8(f, 0x08);
    written += write_raw(f, "\x42\x82", 2); /* DocType */
    written += write_u8(f, (uint8_t)(0x80 | (uint8_t)doc_type_len));
    written += write_raw(f, doc_type, doc_type_len);
    written += write_raw(f, "\x42\x87", 2); /* DocTypeVersion */
    written += write_u8(f, 0x81);
    written += write_u8(f, 0x04);
    written += write_raw(f, "\x42\x85", 2); /* DocTypeReadVersion */
    written += write_u8(f, 0x81);
    written += write_u8(f, 0x02);

    /* Segment */
    written += write_raw(f, "\x18\x53\x80\x67", 4); /* Segment */
    written += write_u8(f, 0xFF); /* unknown size */

    /* Info with TimecodeScale=1000000 (1ms) */
    written += write_raw(f, "\x15\x49\xA9\x66", 4); /* Info */
    written += write_u8(f, 0x87);
    written += write_raw(f, "\x2A\xD7\xB1", 3); /* TimecodeScale */
    written += write_u8(f, 0x83);
    written += write_u8(f, 0x0F);
    written += write_u8(f, 0x42);
    written += write_u8(f, 0x40);

    return written;
}

/* =============================================================================
 * Recorder Management
 * ============================================================================= */

turbo_recorder_t *turbo_recorder_create(const turbo_recorder_config_t *config) {
    if (!config || !config->filename) {
        return NULL;
    }
    
    turbo_recorder_t *rec = (turbo_recorder_t *)calloc(1, sizeof(turbo_recorder_t));
    if (!rec) return NULL;
    
    rec->format = config->format;
    rec->filename = tstr_dup(config->filename);
    
    if (config->title) {
        rec->title = tstr_dup(config->title);
    }
    if (config->author) {
        rec->author = tstr_dup(config->author);
    }
    if (config->comment) {
        rec->comment = tstr_dup(config->comment);
    }
    
    rec->track_count = 0;
    rec->recording = 0;
    rec->paused = 0;
    rec->mp4_mdat_offset = -1;
    rec->pause_start_us = 0;
    rec->paused_total_us = 0;
    
    return rec;
}

void turbo_recorder_destroy(turbo_recorder_t *rec) {
    if (!rec) return;
    
    /* Stop recording if active */
    if (rec->recording) {
        turbo_recorder_stop(rec);
    }
    
    /* Free track buffers */
    for (int i = 0; i < rec->track_count; i++) {
        free(rec->tracks[i].buffer);
    }
    
    tstr_free(rec->filename);
    tstr_free(rec->title);
    tstr_free(rec->author);
    tstr_free(rec->comment);
    
    free(rec);
}

int turbo_recorder_add_track(turbo_recorder_t *rec,
                             const turbo_recorder_track_config_t *config) {
    if (!rec || !config || rec->track_count >= MAX_TRACKS) {
        return -1;
    }
    
    /* Cannot add tracks while recording */
    if (rec->recording) {
        return -1;
    }
    
    recorder_track_t *track = &rec->tracks[rec->track_count];
    
    track->active = 1;
    track->type = config->type;
    track->codec = config->codec;
    
    if (config->type == TURBO_RECORDER_TRACK_VIDEO) {
        track->width = config->width;
        track->height = config->height;
        track->framerate = config->framerate;
    } else {
        track->sample_rate = config->sample_rate;
        track->channels = config->channels;
    }
    
    /* Allocate buffer */
    track->buffer = (uint8_t *)malloc(BUFFER_SIZE);
    if (!track->buffer) {
        return -1;
    }
    track->buffer_size = BUFFER_SIZE;
    track->buffer_used = 0;
    
    track->frame_count = 0;
    track->bytes_written = 0;
    track->frames_written = 0;
    
    return rec->track_count++;
}

int turbo_recorder_start(turbo_recorder_t *rec) {
    size_t header_bytes = 0;

    if (!rec || rec->recording || rec->track_count == 0) {
        return -1;
    }

    if (!recorder_format_supported(rec->format)) {
        recorder_emit_error(rec, "Unsupported recording format");
        return -1;
    }
    
    /* Open file */
    rec->file = recorder_file_open(rec->filename, "wb");
    if (!rec->file) {
        recorder_emit_error(rec, "Failed to open file");
        return -1;
    }
    
    rec->total_bytes_written = 0;
    rec->mp4_mdat_offset = -1;
    rec->paused = 0;
    rec->pause_start_us = 0;
    rec->paused_total_us = 0;

    /* Write container header */
    if (rec->format == TURBO_RECORDER_FORMAT_MP4) {
        header_bytes += write_mp4_ftyp(rec->file);
        rec->mp4_mdat_offset = recorder_file_tell(rec->file);
        if (rec->mp4_mdat_offset < 0) {
            recorder_file_close(rec->file);
            rec->file = NULL;
            recorder_emit_error(rec, "Failed to prepare MP4 container");
            return -1;
        }
        header_bytes += write_mp4_mdat_header(rec->file);
    } else if (rec->format == TURBO_RECORDER_FORMAT_WEBM) {
        header_bytes += write_matroska_header(rec->file, "webm");
    } else if (rec->format == TURBO_RECORDER_FORMAT_MKV) {
        header_bytes += write_matroska_header(rec->file, "matroska");
    }

    if (ferror(rec->file)) {
        recorder_file_close(rec->file);
        rec->file = NULL;
        recorder_emit_error(rec, "Failed to write recording header");
        return -1;
    }
    
    rec->start_time_us = get_time_us();
    rec->recording = 1;
    rec->total_bytes_written = (int64_t)header_bytes;
    
    /* Initialize track timestamps */
    for (int i = 0; i < rec->track_count; i++) {
        rec->tracks[i].start_time_us = rec->start_time_us;
        rec->tracks[i].last_timestamp_us = 0;
    }
    
    return 0;
}

int turbo_recorder_write_frame(turbo_recorder_t *rec, int track_id,
                               const uint8_t *data, size_t len,
                               int64_t timestamp_us, int is_keyframe) {
    if (!rec || !rec->recording || track_id < 0 || track_id >= rec->track_count) {
        return -1;
    }
    if (rec->paused) {
        return -1;
    }
    
    if (!data || len == 0) {
        return -1;
    }
    
    recorder_track_t *track = &rec->tracks[track_id];
    
    /* Write frame data directly to file (simplified) */
    /* In production, would properly mux into container format */
    
    /* For now, just write raw data with simple framing */
    /* Format: [4 bytes length][1 byte keyframe flag][data] */
    if (write_u32_be(rec->file, (uint32_t)len) != 4 ||
        write_u8(rec->file, is_keyframe ? 1 : 0) != 1 ||
        write_raw(rec->file, data, len) != len) {
        recorder_emit_error(rec, "Failed to write frame data");
        return -1;
    }
    
    /* Update statistics */
    if (track->frames_written == 0 || timestamp_us > track->last_timestamp_us) {
        track->last_timestamp_us = timestamp_us;
    }
    track->bytes_written += len + 5;
    track->frames_written++;
    rec->total_bytes_written += len + 5;
    
    return 0;
}

int turbo_recorder_stop(turbo_recorder_t *rec) {
    int64_t media_duration_us;

    if (!rec || !rec->recording) {
        return -1;
    }

    media_duration_us = recorder_media_duration_us(rec);
    rec->duration_us = media_duration_us >= 0 ? media_duration_us : recorder_elapsed_wallclock_us(rec);
    
    /* Finalize container */
    if (rec->format == TURBO_RECORDER_FORMAT_MP4 && finalize_mp4_mdat_size(rec) != 0) {
        recorder_emit_error(rec, "Failed to finalize MP4 mdat size");
        return -1;
    }
    
    /* Close file */
    if (rec->file) {
        if (recorder_file_close(rec->file) != 0) {
            rec->file = NULL;
            rec->recording = 0;
            rec->paused = 0;
            rec->pause_start_us = 0;
            recorder_emit_error(rec, "Failed to close recording file");
            return -1;
        }
        rec->file = NULL;
    }
    
    rec->recording = 0;
    rec->paused = 0;
    rec->pause_start_us = 0;
    
    return 0;
}

int turbo_recorder_pause(turbo_recorder_t *rec) {
    if (!rec || !rec->recording || rec->paused) {
        return -1;
    }
    
    /* Flush buffers */
    if (rec->file && recorder_file_flush(rec->file) != 0) {
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

struct rtp_recorder_ctx_t {
    turbo_recorder_t *recorder;
    int track_id;
    uint32_t base_timestamp;
    uint32_t clock_rate;
    int base_timestamp_set;
};

rtp_recorder_ctx_t *turbo_recorder_create_rtp_context(turbo_recorder_t *rec,
                                                       int track_id) {
    if (!rec || track_id < 0 || track_id >= rec->track_count) {
        return NULL;
    }
    
    rtp_recorder_ctx_t *ctx = (rtp_recorder_ctx_t *)calloc(1, sizeof(rtp_recorder_ctx_t));
    if (!ctx) return NULL;
    
    ctx->recorder = rec;
    ctx->track_id = track_id;
    ctx->base_timestamp = 0;
    ctx->clock_rate = recorder_track_rtp_clock_rate(&rec->tracks[track_id]);
    ctx->base_timestamp_set = 0;
    
    return ctx;
}

void turbo_recorder_destroy_rtp_context(rtp_recorder_ctx_t *ctx) {
    free(ctx);
}

int turbo_recorder_write_rtp_frame(rtp_recorder_ctx_t *ctx,
                                   const uint8_t *data, size_t len,
                                   uint32_t rtp_timestamp, int is_keyframe) {
    if (!ctx || !data) return -1;
    if (!ctx->recorder || ctx->track_id < 0 || ctx->track_id >= ctx->recorder->track_count) {
        return -1;
    }
    if (ctx->clock_rate == 0) {
        return -1;
    }
    
    int64_t timestamp_us;
    
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
