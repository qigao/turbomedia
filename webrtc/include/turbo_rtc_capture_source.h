#ifndef TURBO_RTC_CAPTURE_SOURCE_H
#define TURBO_RTC_CAPTURE_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "turbo_export.h"
#include "turbo_rtc_client.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_RTC_CAPTURE_SOURCE_MAX_QUEUE 64u
#define TURBO_RTC_CAPTURE_SOURCE_MAX_FRAME_BYTES 65536u
#define TURBO_RTC_CAPTURE_SOURCE_MAX_DEVICE_ID 256u

typedef struct turbo_rtc_capture_source_s turbo_rtc_capture_source_t;

typedef enum turbo_rtc_capture_source_state_t {
    TURBO_RTC_CAPTURE_SOURCE_CREATED = 0,
    TURBO_RTC_CAPTURE_SOURCE_PREPARED,
    TURBO_RTC_CAPTURE_SOURCE_STARTED,
    TURBO_RTC_CAPTURE_SOURCE_STOPPED,
    TURBO_RTC_CAPTURE_SOURCE_FAILED
} turbo_rtc_capture_source_state_t;

typedef struct turbo_rtc_capture_source_config_t {
    size_t size;

    /*
     * Exact Salts audio device identity, copied by create().
     * NULL/empty selects the Salts default device.
     */
    const char *device_id;

    uint32_t sample_rate;
    uint32_t channels;
    uint32_t frame_size_ms;

    uint32_t frame_queue_capacity;
    uint32_t max_frame_bytes;
} turbo_rtc_capture_source_config_t;

typedef struct turbo_rtc_capture_source_snapshot_t {
    size_t size;
    turbo_rtc_capture_source_state_t state;

    uint64_t frames_captured;
    uint64_t frames_sent;
    uint64_t frames_rejected;

    uint32_t queue_items;
    uint32_t queue_high_water;
    uint64_t queue_bytes;
    uint64_t queue_bytes_high_water;
} turbo_rtc_capture_source_snapshot_t;

TURBO_MEDIA_C_API void turbo_rtc_capture_source_config_init(
    turbo_rtc_capture_source_config_t *config);

TURBO_MEDIA_C_API void turbo_rtc_capture_source_snapshot_init(
    turbo_rtc_capture_source_snapshot_t *snapshot);

/*
 * Copies config and allocates the fixed-capacity PCM queue.
 * No capture device is opened by create().
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_capture_source_create(
    const turbo_rtc_capture_source_config_t *config,
    turbo_rtc_capture_source_t **out_source);

/*
 * Opens one Salts audio capture device using exact S16 PCM parameters.
 * The capture callback only copies borrowed PCM into the bounded queue.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_capture_source_prepare(
    turbo_rtc_capture_source_t *source);

/*
 * Starts Salts capture. start() is valid from PREPARED or STOPPED.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_capture_source_start(
    turbo_rtc_capture_source_t *source);

/*
 * Owner-thread bridge from captured PCM to WHIP.
 *
 * The Salts callback never calls PeerConnection or RTCClient. poll() copies the
 * current queue-head frame out of the callback-owned queue, then calls
 * turbo_rtc_client_send_audio() on the owner thread.
 *
 * A disconnected/connecting client returns ESTATE and retains the exact
 * queue-head frame. Continued capture remains bounded and will fail with
 * EQUEUE if the fixed queue fills. Any other RTCClient send failure is fatal
 * for the source and stops new capture admission. Each poll processes at most
 * the frames queued at entry, so continuous capture cannot monopolize the
 * owner loop. No DROP_OLDEST or unbounded buffering is used.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_capture_source_poll(
    turbo_rtc_capture_source_t *source,
    turbo_rtc_client_t *client);

/*
 * Fences callback admission, stops Salts capture (which quiesces the native
 * callback), then clears retained PCM. Valid from STARTED or FAILED.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_capture_source_stop(
    turbo_rtc_capture_source_t *source);

TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_capture_source_snapshot(
    const turbo_rtc_capture_source_t *source,
    turbo_rtc_capture_source_snapshot_t *snapshot);

/*
 * Destroy is valid only after the Salts capture handle is STOPPED. A FAILED
 * source with an ERROR/RUNNING backend must call stop() first so native
 * callbacks are quiescent before device destruction.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_capture_source_destroy(
    turbo_rtc_capture_source_t *source);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_RTC_CAPTURE_SOURCE_H */
