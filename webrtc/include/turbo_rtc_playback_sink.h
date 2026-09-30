#ifndef TURBO_RTC_PLAYBACK_SINK_H
#define TURBO_RTC_PLAYBACK_SINK_H

#include <stddef.h>
#include <stdint.h>

#include <salts_playback.h>

#include "turbo_export.h"
#include "turbo_rtc_client.h"
#include "turbo_rtc_subscriber.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_rtc_playback_sink_s turbo_rtc_playback_sink_t;

typedef enum turbo_rtc_playback_sink_state_t {
    TURBO_RTC_PLAYBACK_SINK_CREATED = 0,
    TURBO_RTC_PLAYBACK_SINK_PREPARED,
    TURBO_RTC_PLAYBACK_SINK_STARTED,
    TURBO_RTC_PLAYBACK_SINK_STOPPED,
    TURBO_RTC_PLAYBACK_SINK_FAILED
} turbo_rtc_playback_sink_state_t;

typedef struct turbo_rtc_playback_sink_config_t {
    size_t size;

    /*
     * Optional Salts playback device. create() copies the complete enumeration
     * result. NULL selects the Salts default device during prepare().
     */
    const salts_playback_device_t *device;

    uint32_t sample_rate;
    uint32_t channels;
    uint32_t buffer_duration_ms;
    uint32_t drain_timeout_ms;
} turbo_rtc_playback_sink_config_t;

typedef struct turbo_rtc_playback_sink_snapshot_t {
    size_t size;
    turbo_rtc_playback_sink_state_t state;
    salts_playback_state_t playback_state;

    uint64_t frames_written;
    uint64_t bytes_written;
    uint64_t backpressure_events;
    uint64_t write_failures;

    uint64_t buffered_bytes;
    uint64_t available_bytes;
} turbo_rtc_playback_sink_snapshot_t;

TURBO_MEDIA_C_API void turbo_rtc_playback_sink_config_init(
    turbo_rtc_playback_sink_config_t *config);

TURBO_MEDIA_C_API void turbo_rtc_playback_sink_snapshot_init(
    turbo_rtc_playback_sink_snapshot_t *snapshot);

/*
 * Copies configuration and the optional Salts device identity.
 * No device is opened by create().
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_playback_sink_create(
    const turbo_rtc_playback_sink_config_t *config,
    turbo_rtc_playback_sink_t **out_sink);

/*
 * Creates the bounded Salts S16 playback sink. This is the device-ownership
 * boundary and may fail when the selected device is unavailable.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_playback_sink_prepare(
    turbo_rtc_playback_sink_t *sink);

/*
 * Starts native playback. start() is valid from PREPARED or STOPPED.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_playback_sink_start(
    turbo_rtc_playback_sink_t *sink);

/*
 * Callback adapter for turbo_rtc_subscriber_config_t::on_audio.
 *
 * The PCM payload remains borrowed. If the bounded Salts ring cannot accept
 * the whole frame yet, this returns TURBO_RTC_SUBSCRIBER_AUDIO_RETRY without
 * consuming any bytes; the subscriber retains the frame at its queue head.
 * Format/device/write errors return TURBO_RTC_SUBSCRIBER_AUDIO_FATAL.
 */
TURBO_MEDIA_C_API int turbo_rtc_playback_sink_on_audio(
    void *context,
    const void *pcm,
    size_t bytes,
    uint32_t sample_rate,
    uint32_t channels,
    uint64_t rtp_timestamp);

/*
 * Performs a bounded drain using config.drain_timeout_ms and then stops the
 * device. A drain timeout is surfaced as ETIMEDOUT after the device is stopped.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_playback_sink_stop(
    turbo_rtc_playback_sink_t *sink);

TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_playback_sink_snapshot(
    const turbo_rtc_playback_sink_t *sink,
    turbo_rtc_playback_sink_snapshot_t *snapshot);

/*
 * Destroy is rejected while playback is STARTED. The subscriber producer must
 * already be quiescent before destroy(), matching Salts::Playback ownership.
 */
TURBO_MEDIA_C_API turbo_rtc_client_status_t turbo_rtc_playback_sink_destroy(
    turbo_rtc_playback_sink_t *sink);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_RTC_PLAYBACK_SINK_H */
