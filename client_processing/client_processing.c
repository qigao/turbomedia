#include "turbo_client_processing.h"

#include <stdlib.h>
#include <string.h>

enum {
    TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_CAPACITY = 8
};

#define TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_BYTES (8u * 1024u * 1024u)
#define TURBO_CLIENT_PROCESSING_DEFAULT_QUEUE_DURATION_US UINT64_C(500000)

struct turbo_client_processing_s {
    turbo_client_processing_config_t config;
    turbo_client_processing_state_t state;
    size_t queued_frames;
    size_t queued_bytes;
    uint64_t queued_duration_us;
    uint64_t admitted_frames;
    uint64_t rejected_frames;
};

static int turbo_client_processing_config_valid(
    const turbo_client_processing_config_t *config) {
    return config != NULL &&
           config->size == sizeof(*config) &&
           config->frame_queue_capacity > 0u &&
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
    processing->config = *config;
    processing->state = TURBO_CLIENT_PROCESSING_CREATED;
    *out_processing = processing;
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_prepare(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (processing->state != TURBO_CLIENT_PROCESSING_CREATED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    processing->state = TURBO_CLIENT_PROCESSING_PREPARED;
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_start(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (processing->state != TURBO_CLIENT_PROCESSING_PREPARED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    processing->state = TURBO_CLIENT_PROCESSING_RUNNING;
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_pause(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (processing->state != TURBO_CLIENT_PROCESSING_RUNNING) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    processing->state = TURBO_CLIENT_PROCESSING_PAUSED;
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_resume(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (processing->state != TURBO_CLIENT_PROCESSING_PAUSED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    processing->state = TURBO_CLIENT_PROCESSING_RUNNING;
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_request_stop(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (processing->state != TURBO_CLIENT_PROCESSING_PREPARED &&
        processing->state != TURBO_CLIENT_PROCESSING_RUNNING &&
        processing->state != TURBO_CLIENT_PROCESSING_PAUSED) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    processing->state = TURBO_CLIENT_PROCESSING_DRAINING;
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_drain(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (processing->state != TURBO_CLIENT_PROCESSING_DRAINING) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    processing->queued_frames = 0u;
    processing->queued_bytes = 0u;
    processing->queued_duration_us = 0u;
    processing->state = TURBO_CLIENT_PROCESSING_STOPPED;
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_snapshot(
    const turbo_client_processing_t *processing,
    turbo_client_processing_snapshot_t *snapshot) {
    if (processing == NULL || snapshot == NULL ||
        snapshot->size != sizeof(*snapshot)) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }

    snapshot->state = processing->state;
    snapshot->frame_queue_capacity = processing->config.frame_queue_capacity;
    snapshot->frame_queue_max_bytes =
        processing->config.frame_queue_max_bytes;
    snapshot->frame_queue_max_duration_us =
        processing->config.frame_queue_max_duration_us;
    snapshot->queued_frames = processing->queued_frames;
    snapshot->queued_bytes = processing->queued_bytes;
    snapshot->queued_duration_us = processing->queued_duration_us;
    snapshot->admitted_frames = processing->admitted_frames;
    snapshot->rejected_frames = processing->rejected_frames;
    return TURBO_CLIENT_PROCESSING_OK;
}

turbo_client_processing_status_t turbo_client_processing_destroy(
    turbo_client_processing_t *processing) {
    if (processing == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (processing->state == TURBO_CLIENT_PROCESSING_RUNNING ||
        processing->state == TURBO_CLIENT_PROCESSING_PAUSED ||
        processing->state == TURBO_CLIENT_PROCESSING_DRAINING) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    free(processing);
    return TURBO_CLIENT_PROCESSING_OK;
}
