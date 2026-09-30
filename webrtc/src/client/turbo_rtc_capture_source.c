#include "turbo_rtc_capture_source.h"

#include <salts/thread.h>
#include <salts_capture.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_RTC_CAPTURE_DEFAULT_QUEUE 8u
#define TURBO_RTC_CAPTURE_DEFAULT_MAX_FRAME_BYTES 16384u

struct turbo_rtc_capture_source_s {
    turbo_rtc_capture_source_state_t state;

    char *device_id;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t frame_size_ms;
    uint32_t frame_queue_capacity;
    uint32_t max_frame_bytes;

    salts_capture_t *capture;

    salts_mutex_t lock;
    int lock_initialized;
    int accepting;
    int queue_overflowed;
    int frame_error;

    uint8_t *frame_storage;
    uint8_t *delivery_buffer;
    size_t *frame_lengths;
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;
    size_t queue_bytes;
    size_t queue_high_water;
    size_t queue_bytes_high_water;

    uint64_t frames_captured;
    uint64_t frames_sent;
    uint64_t frames_rejected;
};

static char *capture_source_strdup(const char *value) {
    size_t size;
    char *copy;
    if (!value || value[0] == '\0') {
        return NULL;
    }
    size = strlen(value) + 1u;
    copy = (char *)malloc(size);
    if (copy) {
        memcpy(copy, value, size);
    }
    return copy;
}

static int capture_source_sample_rate_valid(uint32_t sample_rate) {
    return sample_rate == 8000u || sample_rate == 16000u ||
           sample_rate == 24000u || sample_rate == 48000u;
}

static int capture_source_frame_size_valid(uint32_t frame_size_ms) {
    return frame_size_ms == 10u || frame_size_ms == 20u ||
           frame_size_ms == 40u || frame_size_ms == 60u;
}

static int capture_source_config_valid(
    const turbo_rtc_capture_source_config_t *config) {
    size_t expected_frame_bytes;
    size_t alignment;

    if (!config || config->size < sizeof(*config) ||
        !capture_source_sample_rate_valid(config->sample_rate) ||
        (config->channels != 1u && config->channels != 2u) ||
        !capture_source_frame_size_valid(config->frame_size_ms) ||
        config->frame_queue_capacity == 0u ||
        config->frame_queue_capacity > TURBO_RTC_CAPTURE_SOURCE_MAX_QUEUE ||
        config->max_frame_bytes == 0u ||
        config->max_frame_bytes > TURBO_RTC_CAPTURE_SOURCE_MAX_FRAME_BYTES ||
        (config->device_id &&
         strlen(config->device_id) >= TURBO_RTC_CAPTURE_SOURCE_MAX_DEVICE_ID)) {
        return 0;
    }

    alignment = (size_t)config->channels * sizeof(int16_t);
    expected_frame_bytes =
        (size_t)config->sample_rate * config->frame_size_ms / 1000u *
        alignment;
    if (alignment == 0u || expected_frame_bytes == 0u ||
        config->max_frame_bytes < expected_frame_bytes ||
        (config->max_frame_bytes % alignment) != 0u) {
        return 0;
    }
    return 1;
}

static void capture_source_reset_queue_locked(
    turbo_rtc_capture_source_t *source) {
    source->queue_head = 0u;
    source->queue_tail = 0u;
    source->queue_count = 0u;
    source->queue_bytes = 0u;
    source->queue_overflowed = 0;
    source->frame_error = 0;
}

static void capture_source_on_audio(
    salts_capture_t *capture,
    const uint8_t *samples,
    size_t len,
    uint64_t timestamp,
    void *user_data) {
    turbo_rtc_capture_source_t *source =
        (turbo_rtc_capture_source_t *)user_data;
    size_t alignment;
    size_t slot_offset;

    (void)capture;
    (void)timestamp;
    if (!source || !samples || len == 0u || !source->lock_initialized) {
        return;
    }

    salts_mutex_lock(&source->lock);
    if (!source->accepting) {
        salts_mutex_unlock(&source->lock);
        return;
    }

    source->frames_captured++;
    alignment = (size_t)source->channels * sizeof(int16_t);
    {
        size_t expected_len =
            (size_t)source->sample_rate * source->frame_size_ms / 1000u *
            alignment;
        if (alignment == 0u || expected_len == 0u ||
            len != expected_len || len > source->max_frame_bytes) {
            source->frames_rejected++;
            source->frame_error = 1;
            source->accepting = 0;
            salts_mutex_unlock(&source->lock);
            return;
        }
    }

    if (source->queue_count >= source->frame_queue_capacity) {
        source->frames_rejected++;
        source->queue_overflowed = 1;
        source->accepting = 0;
        salts_mutex_unlock(&source->lock);
        return;
    }

    slot_offset = source->queue_tail * source->max_frame_bytes;
    memcpy(source->frame_storage + slot_offset, samples, len);
    source->frame_lengths[source->queue_tail] = len;
    source->queue_tail =
        (source->queue_tail + 1u) % source->frame_queue_capacity;
    source->queue_count++;
    source->queue_bytes += len;
    if (source->queue_count > source->queue_high_water) {
        source->queue_high_water = source->queue_count;
    }
    if (source->queue_bytes > source->queue_bytes_high_water) {
        source->queue_bytes_high_water = source->queue_bytes;
    }
    salts_mutex_unlock(&source->lock);
}

void turbo_rtc_capture_source_config_init(
    turbo_rtc_capture_source_config_t *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->size = sizeof(*config);
    config->sample_rate = 48000u;
    config->channels = 1u;
    config->frame_size_ms = 20u;
    config->frame_queue_capacity = TURBO_RTC_CAPTURE_DEFAULT_QUEUE;
    config->max_frame_bytes = TURBO_RTC_CAPTURE_DEFAULT_MAX_FRAME_BYTES;
}

void turbo_rtc_capture_source_snapshot_init(
    turbo_rtc_capture_source_snapshot_t *snapshot) {
    if (!snapshot) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->size = sizeof(*snapshot);
    snapshot->state = TURBO_RTC_CAPTURE_SOURCE_CREATED;
}

turbo_rtc_client_status_t turbo_rtc_capture_source_create(
    const turbo_rtc_capture_source_config_t *config,
    turbo_rtc_capture_source_t **out_source) {
    turbo_rtc_capture_source_t *source;
    size_t storage_bytes;

    if (!out_source) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    *out_source = NULL;
    if (!capture_source_config_valid(config)) {
        return TURBO_RTC_CLIENT_EINVAL;
    }

    storage_bytes =
        (size_t)config->frame_queue_capacity * config->max_frame_bytes;
    if (config->max_frame_bytes != 0u &&
        storage_bytes / config->max_frame_bytes !=
            config->frame_queue_capacity) {
        return TURBO_RTC_CLIENT_EINVAL;
    }

    source = (turbo_rtc_capture_source_t *)calloc(1, sizeof(*source));
    if (!source) {
        return TURBO_RTC_CLIENT_ENOMEM;
    }

    source->device_id = capture_source_strdup(config->device_id);
    if (config->device_id && config->device_id[0] && !source->device_id) {
        free(source);
        return TURBO_RTC_CLIENT_ENOMEM;
    }

    source->frame_storage = (uint8_t *)malloc(storage_bytes);
    source->delivery_buffer = (uint8_t *)malloc(config->max_frame_bytes);
    source->frame_lengths = (size_t *)calloc(
        config->frame_queue_capacity, sizeof(*source->frame_lengths));
    if (!source->frame_storage || !source->delivery_buffer ||
        !source->frame_lengths) {
        free(source->frame_lengths);
        free(source->delivery_buffer);
        free(source->frame_storage);
        free(source->device_id);
        free(source);
        return TURBO_RTC_CLIENT_ENOMEM;
    }

    source->sample_rate = config->sample_rate;
    source->channels = config->channels;
    source->frame_size_ms = config->frame_size_ms;
    source->frame_queue_capacity = config->frame_queue_capacity;
    source->max_frame_bytes = config->max_frame_bytes;
    source->state = TURBO_RTC_CAPTURE_SOURCE_CREATED;

    salts_mutex_init(&source->lock);
    source->lock_initialized = 1;

    *out_source = source;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_capture_source_prepare(
    turbo_rtc_capture_source_t *source) {
    salts_audio_capture_config_t config;

    if (!source) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (source->state != TURBO_RTC_CAPTURE_SOURCE_CREATED ||
        source->capture) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    memset(&config, 0, sizeof(config));
    config.sample_rate = (int)source->sample_rate;
    config.channels = (int)source->channels;
    config.bits_per_sample = 16;
    config.frame_size_ms = (int)source->frame_size_ms;

    source->capture = salts_audio_capture_create(
        source->device_id, &config);
    if (!source->capture) {
        source->state = TURBO_RTC_CAPTURE_SOURCE_FAILED;
        return TURBO_RTC_CLIENT_EIO;
    }

    salts_audio_capture_set_callback(
        source->capture, capture_source_on_audio, source);
    source->state = TURBO_RTC_CAPTURE_SOURCE_PREPARED;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_capture_source_start(
    turbo_rtc_capture_source_t *source) {
    if (!source) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if ((source->state != TURBO_RTC_CAPTURE_SOURCE_PREPARED &&
         source->state != TURBO_RTC_CAPTURE_SOURCE_STOPPED) ||
        !source->capture) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    salts_mutex_lock(&source->lock);
    capture_source_reset_queue_locked(source);
    source->accepting = 1;
    salts_mutex_unlock(&source->lock);

    if (salts_capture_start(source->capture) != SALTS_CAPTURE_OK) {
        salts_mutex_lock(&source->lock);
        source->accepting = 0;
        salts_mutex_unlock(&source->lock);
        source->state = TURBO_RTC_CAPTURE_SOURCE_FAILED;
        return TURBO_RTC_CLIENT_EIO;
    }

    source->state = TURBO_RTC_CAPTURE_SOURCE_STARTED;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_capture_source_poll(
    turbo_rtc_capture_source_t *source,
    turbo_rtc_client_t *client) {
    size_t budget;
    if (!source || !client) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if (source->state != TURBO_RTC_CAPTURE_SOURCE_STARTED ||
        !source->capture) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    /*
     * Bound one owner poll to the frames visible on entry. Native capture may
     * continue producing concurrently; newly queued frames wait for the next
     * owner iteration so media draining cannot starve PeerConnection polling.
     */
    salts_mutex_lock(&source->lock);
    budget = source->queue_count;
    salts_mutex_unlock(&source->lock);

    while (budget-- != 0u) {
        size_t length;
        size_t slot_offset;
        turbo_rtc_client_status_t status;
        int overflowed;
        int frame_error;

        salts_mutex_lock(&source->lock);
        overflowed = source->queue_overflowed;
        frame_error = source->frame_error;
        if (overflowed || frame_error) {
            source->accepting = 0;
            salts_mutex_unlock(&source->lock);
            source->state = TURBO_RTC_CAPTURE_SOURCE_FAILED;
            return overflowed ? TURBO_RTC_CLIENT_EQUEUE
                              : TURBO_RTC_CLIENT_EIO;
        }
        if (source->queue_count == 0u) {
            salts_mutex_unlock(&source->lock);
            return TURBO_RTC_CLIENT_OK;
        }

        length = source->frame_lengths[source->queue_head];
        slot_offset = source->queue_head * source->max_frame_bytes;
        memcpy(source->delivery_buffer,
               source->frame_storage + slot_offset, length);
        salts_mutex_unlock(&source->lock);

        status = turbo_rtc_client_send_audio(
            client, source->delivery_buffer, length);
        if (status == TURBO_RTC_CLIENT_ESTATE) {
            /*
             * CONNECTING/restart is the one recoverable RTCClient condition.
             * Keep the exact queue-head frame for a later owner poll.
             */
            return status;
        }
        if (status != TURBO_RTC_CLIENT_OK) {
            salts_mutex_lock(&source->lock);
            source->accepting = 0;
            source->frames_rejected++;
            salts_mutex_unlock(&source->lock);
            source->state = TURBO_RTC_CAPTURE_SOURCE_FAILED;
            return status;
        }

        /*
         * Dequeue only after RTCClient accepted the exact frame. ESTATE leaves
         * queue_head untouched; overflow never evicts old PCM.
         */
        salts_mutex_lock(&source->lock);
        source->queue_head =
            (source->queue_head + 1u) % source->frame_queue_capacity;
        source->queue_count--;
        source->queue_bytes -= length;
        source->frames_sent++;
        overflowed = source->queue_overflowed;
        frame_error = source->frame_error;
        if (overflowed || frame_error) {
            source->accepting = 0;
        }
        salts_mutex_unlock(&source->lock);

        if (overflowed || frame_error) {
            source->state = TURBO_RTC_CAPTURE_SOURCE_FAILED;
            return overflowed ? TURBO_RTC_CLIENT_EQUEUE
                              : TURBO_RTC_CLIENT_EIO;
        }
    }
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_capture_source_stop(
    turbo_rtc_capture_source_t *source) {
    salts_capture_state_t capture_state;

    if (!source) {
        return TURBO_RTC_CLIENT_EINVAL;
    }
    if ((source->state != TURBO_RTC_CAPTURE_SOURCE_STARTED &&
         source->state != TURBO_RTC_CAPTURE_SOURCE_FAILED) ||
        !source->capture) {
        return TURBO_RTC_CLIENT_ESTATE;
    }

    salts_mutex_lock(&source->lock);
    source->accepting = 0;
    salts_mutex_unlock(&source->lock);

    salts_capture_stop(source->capture);

    salts_mutex_lock(&source->lock);
    capture_source_reset_queue_locked(source);
    salts_mutex_unlock(&source->lock);

    capture_state = salts_capture_get_state(source->capture);
    if (capture_state == SALTS_CAPTURE_STATE_RUNNING ||
        capture_state == SALTS_CAPTURE_STATE_STARTING ||
        capture_state == SALTS_CAPTURE_STATE_STOPPING ||
        capture_state == SALTS_CAPTURE_STATE_ERROR) {
        source->state = TURBO_RTC_CAPTURE_SOURCE_FAILED;
        return TURBO_RTC_CLIENT_EIO;
    }

    source->state = TURBO_RTC_CAPTURE_SOURCE_STOPPED;
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_capture_source_snapshot(
    const turbo_rtc_capture_source_t *source,
    turbo_rtc_capture_source_snapshot_t *snapshot) {
    turbo_rtc_capture_source_t *mutable_source;

    if (!source || !snapshot || snapshot->size < sizeof(*snapshot)) {
        return TURBO_RTC_CLIENT_EINVAL;
    }

    mutable_source = (turbo_rtc_capture_source_t *)source;
    snapshot->state = source->state;

    salts_mutex_lock(&mutable_source->lock);
    snapshot->frames_captured = source->frames_captured;
    snapshot->frames_sent = source->frames_sent;
    snapshot->frames_rejected = source->frames_rejected;
    snapshot->queue_items = (uint32_t)source->queue_count;
    snapshot->queue_high_water = (uint32_t)source->queue_high_water;
    snapshot->queue_bytes = (uint64_t)source->queue_bytes;
    snapshot->queue_bytes_high_water =
        (uint64_t)source->queue_bytes_high_water;
    salts_mutex_unlock(&mutable_source->lock);
    return TURBO_RTC_CLIENT_OK;
}

turbo_rtc_client_status_t turbo_rtc_capture_source_destroy(
    turbo_rtc_capture_source_t *source) {
    salts_capture_state_t capture_state;

    if (!source) {
        return TURBO_RTC_CLIENT_EINVAL;
    }

    if (source->capture) {
        capture_state = salts_capture_get_state(source->capture);
        /*
         * Salts owns the native callback/device lifetime. Even ERROR must be
         * normalized through stop() before destroy so backend delivery is
         * quiescent under the producer contract.
         */
        if (capture_state != SALTS_CAPTURE_STATE_STOPPED ||
            source->state == TURBO_RTC_CAPTURE_SOURCE_STARTED) {
            return TURBO_RTC_CLIENT_ESTATE;
        }
        salts_capture_destroy(source->capture);
        source->capture = NULL;
    }

    if (source->lock_initialized) {
        salts_mutex_destroy(&source->lock);
        source->lock_initialized = 0;
    }
    free(source->frame_lengths);
    free(source->delivery_buffer);
    free(source->frame_storage);
    free(source->device_id);
    free(source);
    return TURBO_RTC_CLIENT_OK;
}
