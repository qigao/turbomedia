#ifndef TURBO_CLIENT_PROCESSING_H
#define TURBO_CLIENT_PROCESSING_H

#include <stddef.h>
#include <stdint.h>
#include <salts_capture.h>
#include <salts_playback.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_client_processing_s turbo_client_processing_t;

typedef enum turbo_client_processing_status_t {
    TURBO_CLIENT_PROCESSING_OK = 0,
    TURBO_CLIENT_PROCESSING_EINVAL = -1,
    TURBO_CLIENT_PROCESSING_ENOMEM = -2,
    TURBO_CLIENT_PROCESSING_ESTATE = -3,
    TURBO_CLIENT_PROCESSING_EFULL = -4,
    TURBO_CLIENT_PROCESSING_EOPEN = -5,
    TURBO_CLIENT_PROCESSING_ECODEC = -6,
    TURBO_CLIENT_PROCESSING_EIO = -7,
    TURBO_CLIENT_PROCESSING_EUNSUPPORTED = -8
} turbo_client_processing_status_t;

typedef enum turbo_client_processing_state_t {
    TURBO_CLIENT_PROCESSING_CREATED = 0,
    TURBO_CLIENT_PROCESSING_PREPARED,
    TURBO_CLIENT_PROCESSING_RUNNING,
    TURBO_CLIENT_PROCESSING_PAUSED,
    TURBO_CLIENT_PROCESSING_DRAINING,
    TURBO_CLIENT_PROCESSING_STOPPED,
    TURBO_CLIENT_PROCESSING_FAILED
} turbo_client_processing_state_t;

typedef struct turbo_client_processing_config_t {
    size_t size;
    size_t frame_queue_capacity;
    size_t frame_queue_max_bytes;
    uint64_t frame_queue_max_duration_us;
} turbo_client_processing_config_t;

typedef struct turbo_client_processing_file_plan_t {
    size_t size;
    const char *input_path;
    const char *output_path;
    const char *output_format;
    const char *video_codec;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t frame_rate_num;
    uint32_t frame_rate_den;
    uint64_t bitrate;
} turbo_client_processing_file_plan_t;

typedef struct turbo_client_processing_audio_capture_config_t {
    size_t size;
    size_t queue_capacity;
    size_t queue_max_bytes;
    uint64_t queue_max_duration_us;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
} turbo_client_processing_audio_capture_config_t;

typedef struct turbo_client_processing_audio_frame_info_t {
    size_t size;
    size_t data_size;
    uint64_t timestamp_us;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
} turbo_client_processing_audio_frame_info_t;

typedef struct turbo_client_processing_audio_snapshot_t {
    size_t size;
    size_t queue_capacity;
    size_t queue_max_bytes;
    uint64_t queue_max_duration_us;
    size_t queued_frames;
    size_t queued_bytes;
    uint64_t queued_duration_us;
    uint64_t admitted_frames;
    uint64_t rejected_frames;
} turbo_client_processing_audio_snapshot_t;

typedef struct turbo_client_processing_video_frame_info_t {
    size_t size;
    size_t data_size;
    int width;
    int height;
    uint64_t timestamp_us;
} turbo_client_processing_video_frame_info_t;

typedef struct turbo_client_processing_snapshot_t {
    size_t size;
    turbo_client_processing_state_t state;
    size_t frame_queue_capacity;
    size_t frame_queue_max_bytes;
    uint64_t frame_queue_max_duration_us;
    size_t queued_frames;
    size_t queued_bytes;
    uint64_t queued_duration_us;
    uint64_t admitted_frames;
    uint64_t rejected_frames;
} turbo_client_processing_snapshot_t;

/*
 * ClientProcessing is CLIENT-only and single-owner. These lifecycle calls must
 * be serialized by the caller. The initial core owns no worker thread.
 *
 * Queue limits are independent hard bounds. Future asynchronous adapters must
 * reject admission rather than silently dropping frames when any bound is hit.
 */
TURBO_MEDIA_C_API void turbo_client_processing_config_init(
    turbo_client_processing_config_t *config);

TURBO_MEDIA_C_API void turbo_client_processing_snapshot_init(
    turbo_client_processing_snapshot_t *snapshot);

TURBO_MEDIA_C_API void turbo_client_processing_video_frame_info_init(
    turbo_client_processing_video_frame_info_t *info);

TURBO_MEDIA_C_API void turbo_client_processing_audio_capture_config_init(
    turbo_client_processing_audio_capture_config_t *config);

TURBO_MEDIA_C_API void turbo_client_processing_audio_frame_info_init(
    turbo_client_processing_audio_frame_info_t *info);

TURBO_MEDIA_C_API void turbo_client_processing_audio_snapshot_init(
    turbo_client_processing_audio_snapshot_t *snapshot);

TURBO_MEDIA_C_API void turbo_client_processing_file_plan_init(
    turbo_client_processing_file_plan_t *plan);

/*
 * Copies every string and scalar from plan. Valid only in CREATED state.
 * All fields are explicit: no codec, container, size, or frame-rate fallback
 * is selected by ClientProcessing.
 */
TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_set_file_plan(
    turbo_client_processing_t *processing,
    const turbo_client_processing_file_plan_t *plan);

/*
 * Configures the independent bounded audio-capture queue. Valid only in
 * CREATED state. sample_rate/channels/bits_per_sample describe the borrowed
 * Salts callback payload and are copied by value.
 */
TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_set_audio_capture_config(
    turbo_client_processing_t *processing,
    const turbo_client_processing_audio_capture_config_t *config);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_create(
    const turbo_client_processing_config_t *config,
    turbo_client_processing_t **out_processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_prepare(turbo_client_processing_t *processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_start(turbo_client_processing_t *processing);

/*
 * Executes the prepared file plan synchronously on the owner thread. Normal
 * EOF drains decoder/transform/encoder/mux resources and ends in STOPPED.
 */
TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_run_file(turbo_client_processing_t *processing);

/*
 * Copies one borrowed Salts video frame into the bounded ClientProcessing
 * queue. Admission is valid only while RUNNING. Item count, retained bytes and
 * retained timestamp span are independent hard limits; a full queue returns
 * EFULL and never drops an older frame.
 */
TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_admit_video_frame(
    turbo_client_processing_t *processing,
    const uint8_t *frame, size_t len,
    int width, int height, uint64_t timestamp_us);

/*
 * Exact salts_video_capture_cb adapter. The callback never owns or controls
 * the Salts capture handle; it only copies the borrowed callback payload into
 * processing. Admission failures are reflected in rejected_frames.
 */
TURBO_MEDIA_C_API void turbo_client_processing_video_capture_callback(
    salts_capture_t *capture,
    const uint8_t *frame, size_t len,
    int width, int height,
    uint64_t timestamp_us, void *user_data);

/*
 * Copies and removes the oldest queued video frame. If destination_capacity is
 * too small, out_size receives the required size and the queue is unchanged.
 * Returns ESTATE when no frame is queued.
 */
TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_pop_video_frame(
    turbo_client_processing_t *processing,
    void *destination, size_t destination_capacity,
    size_t *out_size,
    turbo_client_processing_video_frame_info_t *info);

/*
 * Copies one borrowed Salts audio callback payload into the configured audio
 * queue. The payload length must be sample-frame aligned. The time hard bound
 * covers the retained PCM span from the oldest frame timestamp through the
 * newest frame's sample duration, including timestamp gaps. A full queue
 * returns EFULL without dropping existing audio.
 */
TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_admit_audio_frame(
    turbo_client_processing_t *processing,
    const uint8_t *samples, size_t len, uint64_t timestamp_us);

TURBO_MEDIA_C_API void turbo_client_processing_audio_capture_callback(
    salts_capture_t *capture,
    const uint8_t *samples, size_t len,
    uint64_t timestamp_us, void *user_data);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_pop_audio_frame(
    turbo_client_processing_t *processing,
    void *destination, size_t destination_capacity,
    size_t *out_size,
    turbo_client_processing_audio_frame_info_t *info);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_audio_snapshot(
    const turbo_client_processing_t *processing,
    turbo_client_processing_audio_snapshot_t *snapshot);

/*
 * Borrows a Salts playback handle and writes PCM without taking lifecycle
 * ownership. A short successful write is surfaced as EFULL while out_written
 * preserves the bytes accepted by Salts. ClientProcessing never starts,
 * pauses, stops, drains, clears, or destroys playback.
 */
TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_write_playback(
    turbo_client_processing_t *processing,
    salts_playback_t *playback,
    const void *samples, size_t len,
    size_t *out_written);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_pause(turbo_client_processing_t *processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_resume(turbo_client_processing_t *processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_request_stop(turbo_client_processing_t *processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_drain(turbo_client_processing_t *processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_snapshot(
    const turbo_client_processing_t *processing,
    turbo_client_processing_snapshot_t *snapshot);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_destroy(turbo_client_processing_t *processing);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CLIENT_PROCESSING_H */
