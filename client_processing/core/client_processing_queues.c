#include "internal/client_processing_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void turbo_client_processing_ring_copy_in(
    uint8_t *storage, size_t capacity, size_t offset,
    const void *source, size_t size) {
    size_t first;
    if (size == 0u) return;
    first = capacity - offset;
    if (first > size) first = size;
    memcpy(storage + offset, source, first);
    if (first < size)
        memcpy(storage, (const uint8_t *)source + first, size - first);
}

static void turbo_client_processing_ring_copy_out(
    void *destination, const uint8_t *storage,
    size_t capacity, size_t offset, size_t size) {
    size_t first;
    if (size == 0u) return;
    first = capacity - offset;
    if (first > size) first = size;
    memcpy(destination, storage + offset, first);
    if (first < size)
        memcpy((uint8_t *)destination + first, storage, size - first);
}

static void turbo_client_processing_queue_recompute_duration_locked(
    turbo_client_processing_t *processing) {
    size_t tail_index;
    uint64_t first_timestamp;
    uint64_t last_timestamp;
    if (processing->queued_frames <= 1u) {
        processing->queued_duration_us = 0u;
        return;
    }
    tail_index = (processing->frame_head + processing->queued_frames - 1u) %
                 processing->config.frame_queue_capacity;
    first_timestamp =
        processing->frame_slots[processing->frame_head].timestamp_us;
    last_timestamp = processing->frame_slots[tail_index].timestamp_us;
    processing->queued_duration_us =
        last_timestamp >= first_timestamp
            ? last_timestamp - first_timestamp
            : 0u;
}

void turbo_client_processing_queue_clear_locked(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return;
    }
    memset(processing->frame_slots, 0,
           processing->config.frame_queue_capacity *
               sizeof(*processing->frame_slots));
    processing->frame_head = 0u;
    processing->frame_storage_tail = 0u;
    processing->queued_frames = 0u;
    processing->queued_bytes = 0u;
    processing->queued_duration_us = 0u;
}

static void turbo_client_processing_audio_recompute_duration_locked(
    turbo_client_processing_t *processing) {
    size_t tail_index;
    uint64_t first_timestamp;
    uint64_t last_end;
    turbo_client_processing_audio_slot_t *tail;

    if (processing->queued_audio_frames == 0u) {
        processing->queued_audio_duration_us = 0u;
        return;
    }
    tail_index =
        (processing->audio_head + processing->queued_audio_frames - 1u) %
        processing->audio_config.queue_capacity;
    first_timestamp =
        processing->audio_slots[processing->audio_head].timestamp_us;
    tail = &processing->audio_slots[tail_index];
    if (tail->duration_us > UINT64_MAX - tail->timestamp_us) {
        processing->queued_audio_duration_us = UINT64_MAX;
        return;
    }
    last_end = tail->timestamp_us + tail->duration_us;
    processing->queued_audio_duration_us =
        last_end >= first_timestamp
            ? last_end - first_timestamp
            : UINT64_MAX;
}

void turbo_client_processing_audio_clear_locked(
    turbo_client_processing_t *processing) {
    if (processing == NULL || !processing->audio_configured) {
        return;
    }
    memset(processing->audio_slots, 0,
           processing->audio_config.queue_capacity *
               sizeof(*processing->audio_slots));
    processing->audio_head = 0u;
    processing->audio_storage_tail = 0u;
    processing->queued_audio_frames = 0u;
    processing->queued_audio_bytes = 0u;
    processing->queued_audio_duration_us = 0u;
}

static int turbo_client_processing_audio_config_valid(
    const turbo_client_processing_audio_capture_config_t *config) {
    return config != NULL &&
           config->size == sizeof(*config) &&
           config->queue_capacity > 0u &&
           config->queue_capacity <=
               SIZE_MAX / sizeof(turbo_client_processing_audio_slot_t) &&
           config->queue_max_bytes > 0u &&
           config->queue_max_duration_us > 0u &&
           (config->sample_rate == 8000u ||
            config->sample_rate == 16000u ||
            config->sample_rate == 24000u ||
            config->sample_rate == 48000u) &&
           (config->channels == 1u || config->channels == 2u) &&
           (config->bits_per_sample == 16u ||
            config->bits_per_sample == 32u);
}

static size_t turbo_client_processing_audio_frame_bytes(
    const turbo_client_processing_audio_capture_config_t *config) {
    return (size_t)config->channels * (size_t)(config->bits_per_sample / 8u);
}

static int turbo_client_processing_audio_payload_duration_us(
    const turbo_client_processing_audio_capture_config_t *config,
    size_t len, uint64_t *out_duration_us) {
    size_t frame_bytes;
    size_t sample_frames;
    uint64_t whole_seconds;
    uint64_t remainder_frames;
    uint64_t remainder_us;
    uint64_t duration_us;

    if (config == NULL || out_duration_us == NULL || config->sample_rate == 0u) {
        return 0;
    }
    frame_bytes = turbo_client_processing_audio_frame_bytes(config);
    if (frame_bytes == 0u || len == 0u || len % frame_bytes != 0u) {
        return 0;
    }

    sample_frames = len / frame_bytes;
    whole_seconds = (uint64_t)(sample_frames / config->sample_rate);
    remainder_frames = (uint64_t)(sample_frames % config->sample_rate);
    if (whole_seconds > UINT64_MAX / UINT64_C(1000000)) {
        return 0;
    }
    remainder_us =
        (remainder_frames * UINT64_C(1000000) +
         (uint64_t)config->sample_rate - UINT64_C(1)) /
        (uint64_t)config->sample_rate;
    duration_us = whole_seconds * UINT64_C(1000000);
    if (duration_us > UINT64_MAX - remainder_us) {
        return 0;
    }
    *out_duration_us = duration_us + remainder_us;
    return 1;
}

turbo_client_processing_status_t
turbo_client_processing_set_audio_capture_config(
    turbo_client_processing_t *processing,
    const turbo_client_processing_audio_capture_config_t *config) {
    turbo_client_processing_audio_slot_t *slots;
    uint8_t *storage;

    if (processing == NULL || !turbo_client_processing_audio_config_valid(config)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) !=
        TURBO_CLIENT_PROCESSING_CREATED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    slots = (turbo_client_processing_audio_slot_t *)calloc(
        config->queue_capacity, sizeof(*slots));
    storage = (uint8_t *)malloc(config->queue_max_bytes);
    if (slots == NULL || storage == NULL) {
        free(storage);
        free(slots);
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }

    cmeta_mutex_lock(&processing->audio_mutex);
    free(processing->audio_slots);
    free(processing->audio_storage);
    processing->audio_slots = slots;
    processing->audio_storage = storage;
    processing->audio_config = *config;
    processing->audio_head = 0u;
    processing->audio_storage_tail = 0u;
    processing->queued_audio_frames = 0u;
    processing->queued_audio_bytes = 0u;
    processing->queued_audio_duration_us = 0u;
    processing->admitted_audio_frames = 0u;
    processing->rejected_audio_frames = 0u;
    processing->audio_configured = 1;
    cmeta_mutex_unlock(&processing->audio_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t
turbo_client_processing_admit_video_frame(
    turbo_client_processing_t *processing,
    const uint8_t *frame, size_t len,
    int width, int height, uint64_t timestamp_us) {
    turbo_client_processing_video_slot_t *slot;
    size_t slot_index;
    size_t write_offset;
    size_t storage_capacity;
    uint64_t oldest_timestamp = 0u;
    uint64_t newest_timestamp = 0u;
    uint64_t prospective_duration = 0u;

    if (processing == NULL || frame == NULL || len == 0u ||
        width <= 0 || height <= 0) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    cmeta_mutex_lock(&processing->frame_mutex);
    if (turbo_client_processing_state_get(processing) !=
        TURBO_CLIENT_PROCESSING_RUNNING) {
        processing->rejected_frames++;
        cmeta_mutex_unlock(&processing->frame_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    if (processing->queued_frames >= processing->config.frame_queue_capacity ||
        len > processing->config.frame_queue_max_bytes -
                  processing->queued_bytes) {
        processing->rejected_frames++;
        cmeta_mutex_unlock(&processing->frame_mutex);
        return TURBO_CLIENT_PROCESSING_EFULL;
    }

    if (processing->queued_frames != 0u) {
        size_t newest_index =
            (processing->frame_head + processing->queued_frames - 1u) %
            processing->config.frame_queue_capacity;
        oldest_timestamp =
            processing->frame_slots[processing->frame_head].timestamp_us;
        newest_timestamp =
            processing->frame_slots[newest_index].timestamp_us;
        if (timestamp_us < newest_timestamp) {
            processing->rejected_frames++;
            cmeta_mutex_unlock(&processing->frame_mutex);
            return TURBO_CLIENT_PROCESSING_EINVAL;
        }
        prospective_duration = timestamp_us - oldest_timestamp;
        if (prospective_duration >
            processing->config.frame_queue_max_duration_us) {
            processing->rejected_frames++;
            cmeta_mutex_unlock(&processing->frame_mutex);
            return TURBO_CLIENT_PROCESSING_EFULL;
        }
    }

    storage_capacity = processing->config.frame_queue_max_bytes;
    write_offset = processing->frame_storage_tail;
    turbo_client_processing_ring_copy_in(
        processing->frame_storage, storage_capacity, write_offset, frame, len);
    slot_index =
        (processing->frame_head + processing->queued_frames) %
        processing->config.frame_queue_capacity;
    slot = &processing->frame_slots[slot_index];
    slot->offset = write_offset;
    slot->size = len;
    slot->width = width;
    slot->height = height;
    slot->timestamp_us = timestamp_us;

    processing->frame_storage_tail =
        (write_offset + len) % storage_capacity;
    processing->queued_frames++;
    processing->queued_bytes += len;
    processing->queued_duration_us = prospective_duration;
    processing->admitted_frames++;
    cmeta_mutex_unlock(&processing->frame_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_pop_video_frame(
    turbo_client_processing_t *processing,
    void *destination, size_t destination_capacity,
    size_t *out_size,
    turbo_client_processing_video_frame_info_t *info) {
    turbo_client_processing_video_slot_t *slot;

    if (processing == NULL || out_size == NULL || info == NULL ||
        info->size != sizeof(*info)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    cmeta_mutex_lock(&processing->frame_mutex);
    if (processing->queued_frames == 0u) {
        *out_size = 0u;
        cmeta_mutex_unlock(&processing->frame_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    slot = &processing->frame_slots[processing->frame_head];
    *out_size = slot->size;
    if (destination == NULL || destination_capacity < slot->size) {
        cmeta_mutex_unlock(&processing->frame_mutex);
        return TURBO_CLIENT_PROCESSING_EFULL;
    }

    turbo_client_processing_ring_copy_out(
        destination, processing->frame_storage,
        processing->config.frame_queue_max_bytes,
        slot->offset, slot->size);
    info->data_size = slot->size;
    info->width = slot->width;
    info->height = slot->height;
    info->timestamp_us = slot->timestamp_us;

    processing->queued_bytes -= slot->size;
    memset(slot, 0, sizeof(*slot));
    processing->frame_head =
        (processing->frame_head + 1u) %
        processing->config.frame_queue_capacity;
    processing->queued_frames--;
    if (processing->queued_frames == 0u) {
        processing->frame_head = 0u;
        processing->frame_storage_tail = 0u;
    }
    turbo_client_processing_queue_recompute_duration_locked(processing);
    cmeta_mutex_unlock(&processing->frame_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t
turbo_client_processing_admit_audio_frame(
    turbo_client_processing_t *processing,
    const uint8_t *samples, size_t len, uint64_t timestamp_us) {
    turbo_client_processing_audio_slot_t *slot;
    size_t slot_index;
    size_t write_offset;
    size_t storage_capacity;
    size_t frame_bytes;
    uint64_t frame_duration_us;
    uint64_t frame_end_us;
    uint64_t oldest_timestamp;
    uint64_t newest_timestamp = 0u;
    uint64_t prospective_duration;

    if (processing == NULL || samples == NULL || len == 0u) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    cmeta_mutex_lock(&processing->audio_mutex);
    if (!processing->audio_configured) {
        processing->rejected_audio_frames++;
        cmeta_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    frame_bytes =
        turbo_client_processing_audio_frame_bytes(&processing->audio_config);
    if (frame_bytes == 0u || len % frame_bytes != 0u) {
        processing->rejected_audio_frames++;
        cmeta_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) !=
        TURBO_CLIENT_PROCESSING_RUNNING) {
        processing->rejected_audio_frames++;
        cmeta_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    if (processing->queued_audio_frames >=
            processing->audio_config.queue_capacity ||
        len > processing->audio_config.queue_max_bytes -
                  processing->queued_audio_bytes) {
        processing->rejected_audio_frames++;
        cmeta_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_EFULL;
    }
    if (!turbo_client_processing_audio_payload_duration_us(
            &processing->audio_config, len, &frame_duration_us) ||
        frame_duration_us > UINT64_MAX - timestamp_us) {
        processing->rejected_audio_frames++;
        cmeta_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    oldest_timestamp = timestamp_us;
    if (processing->queued_audio_frames != 0u) {
        size_t newest_index =
            (processing->audio_head + processing->queued_audio_frames - 1u) %
            processing->audio_config.queue_capacity;
        oldest_timestamp =
            processing->audio_slots[processing->audio_head].timestamp_us;
        newest_timestamp =
            processing->audio_slots[newest_index].timestamp_us;
        if (timestamp_us < newest_timestamp) {
            processing->rejected_audio_frames++;
            cmeta_mutex_unlock(&processing->audio_mutex);
            return TURBO_CLIENT_PROCESSING_EINVAL;
        }
    }

    frame_end_us = timestamp_us + frame_duration_us;
    prospective_duration = frame_end_us - oldest_timestamp;
    if (prospective_duration >
        processing->audio_config.queue_max_duration_us) {
        processing->rejected_audio_frames++;
        cmeta_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_EFULL;
    }

    storage_capacity = processing->audio_config.queue_max_bytes;
    write_offset = processing->audio_storage_tail;
    turbo_client_processing_ring_copy_in(
        processing->audio_storage, storage_capacity, write_offset, samples, len);
    slot_index =
        (processing->audio_head + processing->queued_audio_frames) %
        processing->audio_config.queue_capacity;
    slot = &processing->audio_slots[slot_index];
    slot->offset = write_offset;
    slot->size = len;
    slot->timestamp_us = timestamp_us;
    slot->duration_us = frame_duration_us;

    processing->audio_storage_tail =
        (write_offset + len) % storage_capacity;
    processing->queued_audio_frames++;
    processing->queued_audio_bytes += len;
    processing->queued_audio_duration_us = prospective_duration;
    processing->admitted_audio_frames++;
    cmeta_mutex_unlock(&processing->audio_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_pop_audio_frame(
    turbo_client_processing_t *processing,
    void *destination, size_t destination_capacity,
    size_t *out_size,
    turbo_client_processing_audio_frame_info_t *info) {
    turbo_client_processing_audio_slot_t *slot;

    if (processing == NULL || out_size == NULL || info == NULL ||
        info->size != sizeof(*info)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    cmeta_mutex_lock(&processing->audio_mutex);
    if (!processing->audio_configured || processing->queued_audio_frames == 0u) {
        *out_size = 0u;
        cmeta_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    slot = &processing->audio_slots[processing->audio_head];
    *out_size = slot->size;
    if (destination == NULL || destination_capacity < slot->size) {
        cmeta_mutex_unlock(&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_EFULL;
    }

    turbo_client_processing_ring_copy_out(
        destination, processing->audio_storage,
        processing->audio_config.queue_max_bytes,
        slot->offset, slot->size);
    info->data_size = slot->size;
    info->timestamp_us = slot->timestamp_us;
    info->sample_rate = processing->audio_config.sample_rate;
    info->channels = processing->audio_config.channels;
    info->bits_per_sample = processing->audio_config.bits_per_sample;

    processing->queued_audio_bytes -= slot->size;
    memset(slot, 0, sizeof(*slot));
    processing->audio_head =
        (processing->audio_head + 1u) %
        processing->audio_config.queue_capacity;
    processing->queued_audio_frames--;
    if (processing->queued_audio_frames == 0u) {
        processing->audio_head = 0u;
        processing->audio_storage_tail = 0u;
    }
    turbo_client_processing_audio_recompute_duration_locked(processing);
    cmeta_mutex_unlock(&processing->audio_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_audio_snapshot(
    const turbo_client_processing_t *processing,
    turbo_client_processing_audio_snapshot_t *snapshot) {
    if (processing == NULL || snapshot == NULL ||
        snapshot->size != sizeof(*snapshot)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    cmeta_mutex_lock((cmeta_mutex_t *)&processing->audio_mutex);
    if (!processing->audio_configured) {
        cmeta_mutex_unlock((cmeta_mutex_t *)&processing->audio_mutex);
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    snapshot->queue_capacity = processing->audio_config.queue_capacity;
    snapshot->queue_max_bytes = processing->audio_config.queue_max_bytes;
    snapshot->queue_max_duration_us =
        processing->audio_config.queue_max_duration_us;
    snapshot->queued_frames = processing->queued_audio_frames;
    snapshot->queued_bytes = processing->queued_audio_bytes;
    snapshot->queued_duration_us = processing->queued_audio_duration_us;
    snapshot->admitted_frames = processing->admitted_audio_frames;
    snapshot->rejected_frames = processing->rejected_audio_frames;
    cmeta_mutex_unlock((cmeta_mutex_t *)&processing->audio_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}
