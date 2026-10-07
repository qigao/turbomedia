#ifndef TURBO_CLIENT_PROCESSING_INTERNAL_H
#define TURBO_CLIENT_PROCESSING_INTERNAL_H

#include "turbo_client_processing.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <salts/thread.h>
#include <stdatomic.h>

#if defined(__GNUC__) || defined(__clang__)
#define TURBO_CLIENT_PROCESSING_INTERNAL __attribute__((visibility("hidden")))
#else
#define TURBO_CLIENT_PROCESSING_INTERNAL
#endif

typedef struct turbo_client_processing_video_slot_t {
    size_t offset;
    size_t size;
    int width;
    int height;
    uint64_t timestamp_us;
} turbo_client_processing_video_slot_t;

typedef struct turbo_client_processing_audio_slot_t {
    size_t offset;
    size_t size;
    uint64_t timestamp_us;
    uint64_t duration_us;
} turbo_client_processing_audio_slot_t;

struct turbo_client_processing_s {
    turbo_client_processing_config_t config;
    atomic_int state;
    atomic_uint lifecycle_flags;
    cmeta_mutex_t frame_mutex;
    turbo_client_processing_video_slot_t *frame_slots;
    uint8_t *frame_storage;
    size_t frame_head;
    size_t frame_storage_tail;
    size_t queued_frames;
    size_t queued_bytes;
    uint64_t queued_duration_us;
    uint64_t admitted_frames;
    uint64_t rejected_frames;

    cmeta_mutex_t audio_mutex;
    turbo_client_processing_audio_capture_config_t audio_config;
    turbo_client_processing_audio_slot_t *audio_slots;
    uint8_t *audio_storage;
    size_t audio_head;
    size_t audio_storage_tail;
    size_t queued_audio_frames;
    size_t queued_audio_bytes;
    uint64_t queued_audio_duration_us;
    uint64_t admitted_audio_frames;
    uint64_t rejected_audio_frames;
    int audio_configured;

    char *input_path;
    char *output_path;
    char *output_format;
    char *video_codec;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t frame_rate_num;
    uint32_t frame_rate_den;
    uint64_t bitrate;
    int file_plan_set;
    int file_prepared;

    AVFormatContext *input;
    AVFormatContext *output;
    AVCodecContext *decoder;
    AVCodecContext *encoder;
    AVStream *input_stream;
    AVStream *output_stream;
    AVPacket *input_packet;
    AVPacket *output_packet;
    AVFrame *decoded_frame;
    AVFrame *converted_frame;
    struct SwsContext *sws;
    int video_stream_index;
    int source_width;
    int source_height;
    enum AVPixelFormat source_format;
    int64_t next_pts;
    int output_header_written;
};

static inline turbo_client_processing_state_t
turbo_client_processing_state_get(const turbo_client_processing_t *processing) {
    return (turbo_client_processing_state_t)atomic_load_explicit(
        &processing->state, memory_order_acquire);
}

static inline void turbo_client_processing_state_set(
    turbo_client_processing_t *processing,
    turbo_client_processing_state_t state) {
    atomic_store_explicit(&processing->state, (int)state, memory_order_release);
}

static inline uint32_t turbo_client_processing_lifecycle_flags_get(
    const turbo_client_processing_t *processing) {
    return (uint32_t)atomic_load_explicit(
        &processing->lifecycle_flags, memory_order_acquire);
}

TURBO_CLIENT_PROCESSING_INTERNAL
void turbo_client_processing_queue_clear_locked(
    turbo_client_processing_t *processing);
TURBO_CLIENT_PROCESSING_INTERNAL
void turbo_client_processing_audio_clear_locked(
    turbo_client_processing_t *processing);
TURBO_CLIENT_PROCESSING_INTERNAL
turbo_client_processing_status_t turbo_client_processing_file_prepare(
    turbo_client_processing_t *processing);
TURBO_CLIENT_PROCESSING_INTERNAL
void turbo_client_processing_file_runtime_clear(
    turbo_client_processing_t *processing);
TURBO_CLIENT_PROCESSING_INTERNAL
void turbo_client_processing_file_plan_clear(
    turbo_client_processing_t *processing);

#endif
