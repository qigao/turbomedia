#include "turbo_rtc_playback_sink.h"

#include <stdlib.h>
#include <string.h>

#define TURBO_RTC_PLAYBACK_DEFAULT_BUFFER_MS 120u
#define TURBO_RTC_PLAYBACK_DEFAULT_DRAIN_TIMEOUT_MS 500u

struct turbo_rtc_playback_sink_s {
    turbo_rtc_playback_sink_state_t state;

    salts_playback_device_t device;
    int has_device;

    uint32_t sample_rate;
    uint32_t channels;
    uint32_t buffer_duration_ms;
    uint32_t drain_timeout_ms;

    salts_playback_t *playback;

    uint64_t frames_written;
    uint64_t bytes_written;
    uint64_t backpressure_events;
    uint64_t write_failures;
};

static int playback_sink_config_valid(
    const turbo_rtc_playback_sink_config_t *config) {
    if (!config || config->size < sizeof(*config) ||
        (config->sample_rate != 8000u &&
         config->sample_rate != 16000u &&
         config->sample_rate != 24000u &&
         config->sample_rate != 48000u) ||
        (config->channels != 1u && config->channels != 2u) ||
        config->buffer_duration_ms < SALTS_PLAYBACK_MIN_BUFFER_MS ||
        config->buffer_duration_ms > SALTS_PLAYBACK_MAX_BUFFER_MS ||
        config->drain_timeout_ms == 0u) {
        return 0;
    }
    if (config->device &&
        (config->device->id_size == 0u ||
         config->device->id_size > SALTS_PLAYBACK_DEVICE_ID_BYTES)) {
        return 0;
    }
    return 1;
}

void turbo_rtc_playback_sink_config_init(
    turbo_rtc_playback_sink_config_t *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->size = sizeof(*config);
    config->sample_rate = 48000u;
    config->channels = 1u;
    config->buffer_duration_ms =
        TURBO_RTC_PLAYBACK_DEFAULT_BUFFER_MS;
    config->drain_timeout_ms =
        TURBO_RTC_PLAYBACK_DEFAULT_DRAIN_TIMEOUT_MS;
}

void turbo_rtc_playback_sink_snapshot_init(
    turbo_rtc_playback_sink_snapshot_t *snapshot) {
    if (!snapshot) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->size = sizeof(*snapshot);
    snapshot->playback_state = SALTS_PLAYBACK_STATE_STOPPED;
}

turbo_rtc_client_status_t turbo_rtc_playback_sink_create(
    const turbo_rtc_playback_sink_config_t *config,
    turbo_rtc_playback_sink_t **out_sink) {
    turbo_rtc_playback_sink_t *sink;

    if (!out_sink) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    *out_sink = NULL;
    if (!playback_sink_config_valid(config)) {
        return TURBO_RTC_CLIENT_EINVAL;
    }

    sink = (turbo_rtc_playback_sink_t *)calloc(1, sizeof(*sink));
    if (!sink) {
        return TURBO_RTC_CLIENT_ENOMEM;
    }

    if (config->device) {
        sink->device = *config->device;
        sink->has_device = 1;
    }
    sink->sample_rate = config->sample_rate;
    sink->channels = config->channels;
    sink->buffer_duration_ms = config->buffer_duration_ms;
    sink->drain_timeout_ms = config->drain_timeout_ms;
    sink->state = TURBO_RTC_PLAYBACK_SINK_CREATED;

    *out_sink = sink;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_playback_sink_prepare(
    turbo_rtc_playback_sink_t *sink) {
    salts_playback_config_t config;

    if (!sink) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (sink->state != TURBO_RTC_PLAYBACK_SINK_CREATED ||
        sink->playback) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    memset(&config, 0, sizeof(config));
    config.sample_rate = sink->sample_rate;
    config.channels = sink->channels;
    config.format = SALTS_PLAYBACK_FORMAT_S16;
    config.buffer_duration_ms = sink->buffer_duration_ms;

    if (salts_playback_create(
            sink->has_device ? &sink->device : NULL,
            &config,
            &sink->playback) != SALTS_PLAYBACK_OK) {
        sink->state = TURBO_RTC_PLAYBACK_SINK_FAILED;
        return TURBO_RTC_CLIENT_EIO;
    }

    sink->state = TURBO_RTC_PLAYBACK_SINK_PREPARED;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_playback_sink_start(
    turbo_rtc_playback_sink_t *sink) {
    if (!sink) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if ((sink->state != TURBO_RTC_PLAYBACK_SINK_PREPARED &&
         sink->state != TURBO_RTC_PLAYBACK_SINK_STOPPED) ||
        !sink->playback) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    if (salts_playback_start(sink->playback) != SALTS_PLAYBACK_OK) {
        sink->state = TURBO_RTC_PLAYBACK_SINK_FAILED;
        return TURBO_RTC_CLIENT_EIO;
    }

    sink->state = TURBO_RTC_PLAYBACK_SINK_STARTED;
    return TURBO_RTC_CLIENT_OK;
}

int turbo_rtc_playback_sink_on_audio(
    void *context,
    const void *pcm,
    size_t bytes,
    uint32_t sample_rate,
    uint32_t channels,
    uint64_t rtp_timestamp) {
    turbo_rtc_playback_sink_t *sink =
        (turbo_rtc_playback_sink_t *)context;
    size_t written = 0u;
    size_t alignment;
    int result;

    (void)rtp_timestamp;

    if (!sink || !pcm || bytes == 0u ||
        sink->state != TURBO_RTC_PLAYBACK_SINK_STARTED ||
        !sink->playback ||
        sample_rate != sink->sample_rate ||
        channels != sink->channels) {
        if (sink) {
            sink->write_failures++;
            sink->state = TURBO_RTC_PLAYBACK_SINK_FAILED;
        }
        return TURBO_RTC_SUBSCRIBER_AUDIO_FATAL;
    }

    alignment = (size_t)channels * sizeof(int16_t);
    if (alignment == 0u || (bytes % alignment) != 0u ||
        salts_playback_get_state(sink->playback) ==
            SALTS_PLAYBACK_STATE_ERROR) {
        sink->write_failures++;
        sink->state = TURBO_RTC_PLAYBACK_SINK_FAILED;
        return TURBO_RTC_SUBSCRIBER_AUDIO_FATAL;
    }

    if (salts_playback_get_available(sink->playback) < bytes) {
        sink->backpressure_events++;
        return TURBO_RTC_SUBSCRIBER_AUDIO_RETRY;
    }

    result = salts_playback_write(
        sink->playback, pcm, bytes, &written);
    if (result != SALTS_PLAYBACK_OK || written != bytes) {
        sink->write_failures++;
        sink->state = TURBO_RTC_PLAYBACK_SINK_FAILED;
        return TURBO_RTC_SUBSCRIBER_AUDIO_FATAL;
    }

    sink->frames_written++;
    sink->bytes_written += bytes;
    return TURBO_RTC_SUBSCRIBER_AUDIO_CONSUMED;
}

turbo_rtc_client_status_t turbo_rtc_playback_sink_stop(
    turbo_rtc_playback_sink_t *sink) {
    int drain_result;
    int stop_result;

    if (!sink) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (sink->state != TURBO_RTC_PLAYBACK_SINK_STARTED ||
        !sink->playback) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    drain_result = salts_playback_drain(
        sink->playback, sink->drain_timeout_ms);
    stop_result = salts_playback_stop(sink->playback);
    if (stop_result != SALTS_PLAYBACK_OK) {
        sink->state = TURBO_RTC_PLAYBACK_SINK_FAILED;
        return TURBO_RTC_CLIENT_EIO;
    }

    sink->state = TURBO_RTC_PLAYBACK_SINK_STOPPED;
    if (drain_result == SALTS_PLAYBACK_ERR_TIMEOUT) {
        return TURBO_RTC_CLIENT_ETIMEDOUT;
    }
    if (drain_result != SALTS_PLAYBACK_OK) {
        return TURBO_RTC_CLIENT_EIO;
    }
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_playback_sink_snapshot(
    const turbo_rtc_playback_sink_t *sink,
    turbo_rtc_playback_sink_snapshot_t *snapshot) {
    if (!sink || !snapshot ||
        snapshot->size < sizeof(*snapshot)) {
        return TURBO_RTC_CLIENT_EINVAL;
    }

    snapshot->state = sink->state;
    snapshot->frames_written = sink->frames_written;
    snapshot->bytes_written = sink->bytes_written;
    snapshot->backpressure_events = sink->backpressure_events;
    snapshot->write_failures = sink->write_failures;
    if (sink->playback) {
        snapshot->playback_state =
            salts_playback_get_state(sink->playback);
        snapshot->buffered_bytes =
            salts_playback_get_buffered(sink->playback);
        snapshot->available_bytes =
            salts_playback_get_available(sink->playback);
    } else {
        snapshot->playback_state = SALTS_PLAYBACK_STATE_STOPPED;
        snapshot->buffered_bytes = 0u;
        snapshot->available_bytes = 0u;
    }
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_playback_sink_destroy(
    turbo_rtc_playback_sink_t *sink) {
    if (!sink) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (sink->state == TURBO_RTC_PLAYBACK_SINK_STARTED) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    if (sink->playback) {
        salts_playback_destroy(sink->playback);
        sink->playback = NULL;
    }
    free(sink);
    return TURBO_RTC_CLIENT_OK;
}
