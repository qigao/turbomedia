#include "internal/client_processing_internal.h"

#include <stdlib.h>
#include <string.h>

enum { TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_CAPACITY = 8 };
#define TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_BYTES (8u * 1024u * 1024u)
#define TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_DURATION_US UINT64_C(500000)

#define TURBO_CLIENT_PROCESSING_CAPTURE_BLOCKERS                              \
    (TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_CAPTURE_PERMISSION_REVOKED |      \
     TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_CAPTURE_DEVICE_LOST |             \
     TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_BACKGROUND_DENIED)

static int turbo_client_processing_config_valid(
    const turbo_client_processing_config_t *config) {
    return config != NULL &&
           config->size == sizeof(*config) &&
           config->frame_queue_capacity > 0u &&
           config->frame_queue_capacity <=
               SIZE_MAX / sizeof(turbo_client_processing_video_slot_t) &&
           config->frame_queue_max_bytes > 0u &&
           config->frame_queue_max_duration_us > 0u;
}

void turbo_client_processing_config_init(
    turbo_client_processing_config_t *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->size = sizeof(*config);
    config->frame_queue_capacity =
        TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_CAPACITY;
    config->frame_queue_max_bytes =
        TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_BYTES;
    config->frame_queue_max_duration_us =
        TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_DURATION_US;
}

void turbo_client_processing_snapshot_init(
    turbo_client_processing_snapshot_t *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->size = sizeof(*snapshot);
}

void turbo_client_processing_video_frame_info_init(
    turbo_client_processing_video_frame_info_t *info) {
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
}

void turbo_client_processing_audio_capture_config_init(
    turbo_client_processing_audio_capture_config_t *config) {
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->size = sizeof(*config);
    config->queue_capacity = 16u;
    config->queue_max_bytes = 1024u * 1024u;
    config->queue_max_duration_us = UINT64_C(500000);
}

void turbo_client_processing_audio_frame_info_init(
    turbo_client_processing_audio_frame_info_t *info) {
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
}

void turbo_client_processing_audio_snapshot_init(
    turbo_client_processing_audio_snapshot_t *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->size = sizeof(*snapshot);
}

void turbo_client_processing_file_plan_init(
    turbo_client_processing_file_plan_t *plan) {
    if (plan == NULL) {
        return;
    }
    memset(plan, 0, sizeof(*plan));
    plan->size = sizeof(*plan);
}

turbo_client_processing_status_t turbo_client_processing_create(
    const turbo_client_processing_config_t *config,
    turbo_client_processing_t **out_processing) {
    turbo_client_processing_t *processing;

    if (out_processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    *out_processing = NULL;
    if (!turbo_client_processing_config_valid(config)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    processing = (turbo_client_processing_t *)calloc(1u, sizeof(*processing));
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }
    processing->frame_slots = (turbo_client_processing_video_slot_t *)calloc(
        config->frame_queue_capacity, sizeof(*processing->frame_slots));
    processing->frame_storage = (uint8_t *)malloc(config->frame_queue_max_bytes);
    cmeta_mutex_init(&processing->frame_mutex);
    cmeta_mutex_init(&processing->audio_mutex);
    if (processing->frame_slots == NULL || processing->frame_storage == NULL ||
        processing->frame_mutex == NULL || processing->audio_mutex == NULL) {
        cmeta_mutex_destroy(&processing->audio_mutex);
        cmeta_mutex_destroy(&processing->frame_mutex);
        free(processing->frame_storage);
        free(processing->frame_slots);
        free(processing);
        return TURBO_CLIENT_PROCESSING_ENOMEM;
    }
    processing->config = *config;
    atomic_init(&processing->state, TURBO_CLIENT_PROCESSING_CREATED);
    atomic_init(&processing->lifecycle_flags,
                TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_NONE);
    processing->video_stream_index = -1;
    processing->source_format = AV_PIX_FMT_NONE;
    *out_processing = processing;
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_prepare(
    turbo_client_processing_t *processing) {
    turbo_client_processing_status_t status;
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_CREATED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    if (processing->file_plan_set) {
        status = turbo_client_processing_file_prepare(processing);
        if (status != TURBO_CLIENT_PROCESSING_OK) {
            turbo_client_processing_file_runtime_clear(processing);
            turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_FAILED);
            return status;
        }
    }
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_PREPARED);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_start(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_PREPARED ||
        (turbo_client_processing_lifecycle_flags_get(processing) &
         TURBO_CLIENT_PROCESSING_CAPTURE_BLOCKERS) != 0u) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_RUNNING);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_pause(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_RUNNING) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_PAUSED);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_resume(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_PAUSED ||
        (turbo_client_processing_lifecycle_flags_get(processing) &
         TURBO_CLIENT_PROCESSING_CAPTURE_BLOCKERS) != 0u) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_RUNNING);
    return TURBO_CLIENT_PROCESSING_OK;
}
turbo_client_processing_status_t
turbo_client_processing_handle_lifecycle_event(
    turbo_client_processing_t *processing,
    turbo_client_processing_lifecycle_event_t event) {
    turbo_client_processing_state_t state;
    uint32_t set_flags = 0u;
    uint32_t clear_flags = 0u;
    int pause_admission = 0;

    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (event == TURBO_CLIENT_PROCESSING_LIFECYCLE_APP_PAUSE) {
        return turbo_client_processing_pause(processing);
    }
    if (event == TURBO_CLIENT_PROCESSING_LIFECYCLE_APP_RESUME) {
        return turbo_client_processing_resume(processing);
    }

    state = turbo_client_processing_state_get(processing);
    if (state == TURBO_CLIENT_PROCESSING_DRAINING ||
        state == TURBO_CLIENT_PROCESSING_STOPPED ||
        state == TURBO_CLIENT_PROCESSING_FAILED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    switch (event) {
        case TURBO_CLIENT_PROCESSING_LIFECYCLE_CAPTURE_PERMISSION_REVOKED:
            set_flags =
                TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_CAPTURE_PERMISSION_REVOKED;
            pause_admission = 1;
            break;
        case TURBO_CLIENT_PROCESSING_LIFECYCLE_CAPTURE_PERMISSION_RESTORED:
            clear_flags =
                TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_CAPTURE_PERMISSION_REVOKED;
            break;
        case TURBO_CLIENT_PROCESSING_LIFECYCLE_CAPTURE_DEVICE_LOST:
            set_flags =
                TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_CAPTURE_DEVICE_LOST;
            pause_admission = 1;
            break;
        case TURBO_CLIENT_PROCESSING_LIFECYCLE_CAPTURE_DEVICE_REVALIDATED:
            clear_flags =
                TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_CAPTURE_DEVICE_LOST;
            break;
        case TURBO_CLIENT_PROCESSING_LIFECYCLE_PLAYBACK_DEVICE_LOST:
            set_flags =
                TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_PLAYBACK_DEVICE_LOST;
            break;
        case TURBO_CLIENT_PROCESSING_LIFECYCLE_PLAYBACK_DEVICE_REVALIDATED:
            clear_flags =
                TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_PLAYBACK_DEVICE_LOST;
            break;
        case TURBO_CLIENT_PROCESSING_LIFECYCLE_VIDEO_SURFACE_LOST:
            set_flags =
                TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_VIDEO_SURFACE_LOST;
            break;
        case TURBO_CLIENT_PROCESSING_LIFECYCLE_VIDEO_SURFACE_REPLACED:
            clear_flags =
                TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_VIDEO_SURFACE_LOST;
            break;
        case TURBO_CLIENT_PROCESSING_LIFECYCLE_BACKGROUND_DENIED:
            set_flags =
                TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_BACKGROUND_DENIED;
            pause_admission = 1;
            break;
        case TURBO_CLIENT_PROCESSING_LIFECYCLE_BACKGROUND_ALLOWED:
            clear_flags =
                TURBO_CLIENT_PROCESSING_LIFECYCLE_FLAG_BACKGROUND_DENIED;
            break;
        default:
            return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    if (set_flags != 0u) {
        atomic_fetch_or_explicit(&processing->lifecycle_flags, set_flags,
                                 memory_order_acq_rel);
    }
    if (clear_flags != 0u) {
        atomic_fetch_and_explicit(&processing->lifecycle_flags, ~clear_flags,
                                  memory_order_acq_rel);
    }
    if (pause_admission && state == TURBO_CLIENT_PROCESSING_RUNNING) {
        turbo_client_processing_state_set(processing,
                                          TURBO_CLIENT_PROCESSING_PAUSED);
    }
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_request_stop(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_PREPARED &&
        turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_RUNNING &&
        turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_PAUSED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_DRAINING);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_drain(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) != TURBO_CLIENT_PROCESSING_DRAINING) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    cmeta_mutex_lock(&processing->frame_mutex);
    turbo_client_processing_queue_clear_locked(processing);
    cmeta_mutex_unlock(&processing->frame_mutex);
    cmeta_mutex_lock(&processing->audio_mutex);
    turbo_client_processing_audio_clear_locked(processing);
    cmeta_mutex_unlock(&processing->audio_mutex);
    turbo_client_processing_file_runtime_clear(processing);
    turbo_client_processing_state_set(processing, TURBO_CLIENT_PROCESSING_STOPPED);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_snapshot(
    const turbo_client_processing_t *processing,
    turbo_client_processing_snapshot_t *snapshot) {
    if (processing == NULL || snapshot == NULL ||
        snapshot->size != sizeof(*snapshot)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    snapshot->state = turbo_client_processing_state_get(processing);
    snapshot->frame_queue_capacity = processing->config.frame_queue_capacity;
    snapshot->frame_queue_max_bytes =
        processing->config.frame_queue_max_bytes;
    snapshot->frame_queue_max_duration_us =
        processing->config.frame_queue_max_duration_us;
    cmeta_mutex_lock((cmeta_mutex_t *)&processing->frame_mutex);
    snapshot->queued_frames = processing->queued_frames;
    snapshot->queued_bytes = processing->queued_bytes;
    snapshot->queued_duration_us = processing->queued_duration_us;
    snapshot->admitted_frames = processing->admitted_frames;
    snapshot->rejected_frames = processing->rejected_frames;
    snapshot->lifecycle_flags =
        turbo_client_processing_lifecycle_flags_get(processing);
    cmeta_mutex_unlock((cmeta_mutex_t *)&processing->frame_mutex);
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_destroy(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) == TURBO_CLIENT_PROCESSING_RUNNING ||
        turbo_client_processing_state_get(processing) == TURBO_CLIENT_PROCESSING_PAUSED ||
        turbo_client_processing_state_get(processing) == TURBO_CLIENT_PROCESSING_DRAINING) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    turbo_client_processing_file_runtime_clear(processing);
    turbo_client_processing_file_plan_clear(processing);
    cmeta_mutex_lock(&processing->frame_mutex);
    turbo_client_processing_queue_clear_locked(processing);
    cmeta_mutex_unlock(&processing->frame_mutex);
    cmeta_mutex_destroy(&processing->frame_mutex);
    cmeta_mutex_lock(&processing->audio_mutex);
    turbo_client_processing_audio_clear_locked(processing);
    cmeta_mutex_unlock(&processing->audio_mutex);
    cmeta_mutex_destroy(&processing->audio_mutex);
    free(processing->audio_storage);
    free(processing->audio_slots);
    free(processing->frame_storage);
    free(processing->frame_slots);
    free(processing);
    return TURBO_CLIENT_PROCESSING_OK;
}
