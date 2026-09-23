#include "turbo_media_revocation_projection.h"

#include <salts/thread.h>
#include <stdlib.h>

struct turbo_media_revocation_projection_s {
    turbo_media_revocation_state_t *state;
    salts_mutex_t mutex;
    int mutex_initialized;
};

turbo_media_revocation_projection_t *
turbo_media_revocation_projection_create(size_t max_entries) {
    turbo_media_revocation_projection_t *projection;

    projection = (turbo_media_revocation_projection_t *)calloc(
        1U, sizeof(*projection));
    if (!projection) {
        return NULL;
    }
    salts_mutex_init(&projection->mutex);
    projection->mutex_initialized = 1;
    projection->state = turbo_media_revocation_state_create(max_entries);
    if (!projection->state) {
        salts_mutex_destroy(&projection->mutex);
        free(projection);
        return NULL;
    }
    return projection;
}

void turbo_media_revocation_projection_destroy(
    turbo_media_revocation_projection_t *projection) {
    if (!projection) {
        return;
    }
    turbo_media_revocation_state_destroy(projection->state);
    projection->state = NULL;
    if (projection->mutex_initialized) {
        salts_mutex_destroy(&projection->mutex);
        projection->mutex_initialized = 0;
    }
    free(projection);
}

turbo_media_auth_revocation_status_t
turbo_media_revocation_projection_check_digest(
    void *context, const uint8_t *sha256, size_t sha256_size) {
    turbo_media_revocation_projection_t *projection =
        (turbo_media_revocation_projection_t *)context;
    turbo_media_auth_revocation_status_t result;

    if (!projection || !projection->state ||
        !projection->mutex_initialized) {
        return TURBO_MEDIA_AUTH_REVOCATION_UNKNOWN;
    }
    salts_mutex_lock(&projection->mutex);
    result = turbo_media_revocation_check_digest(
        projection->state, sha256, sha256_size);
    salts_mutex_unlock(&projection->mutex);
    return result;
}

turbo_media_revocation_apply_result_t
turbo_media_revocation_projection_apply_snapshot(
    turbo_media_revocation_projection_t *projection,
    uint64_t epoch, uint64_t sequence,
    const char *const *sha256_hex, size_t count) {
    turbo_media_revocation_apply_result_t result;

    if (!projection || !projection->state ||
        !projection->mutex_initialized) {
        return TURBO_MEDIA_REVOCATION_APPLY_ERROR;
    }
    salts_mutex_lock(&projection->mutex);
    result = turbo_media_revocation_apply_snapshot(
        projection->state, epoch, sequence, sha256_hex, count);
    salts_mutex_unlock(&projection->mutex);
    return result;
}

turbo_media_revocation_apply_result_t
turbo_media_revocation_projection_apply_revoke(
    turbo_media_revocation_projection_t *projection,
    uint64_t epoch, uint64_t sequence, const char *sha256_hex) {
    turbo_media_revocation_apply_result_t result;

    if (!projection || !projection->state ||
        !projection->mutex_initialized) {
        return TURBO_MEDIA_REVOCATION_APPLY_ERROR;
    }
    salts_mutex_lock(&projection->mutex);
    result = turbo_media_revocation_apply_revoke(
        projection->state, epoch, sequence, sha256_hex);
    salts_mutex_unlock(&projection->mutex);
    return result;
}

int turbo_media_revocation_projection_status(
    turbo_media_revocation_projection_t *projection,
    int *out_synchronized, uint64_t *out_epoch,
    uint64_t *out_sequence, size_t *out_count) {
    if (!projection || !projection->state ||
        !projection->mutex_initialized || !out_synchronized ||
        !out_epoch || !out_sequence || !out_count) {
        return -1;
    }

    salts_mutex_lock(&projection->mutex);
    *out_synchronized =
        turbo_media_revocation_is_synchronized(projection->state);
    *out_epoch = turbo_media_revocation_epoch(projection->state);
    *out_sequence = turbo_media_revocation_sequence(projection->state);
    *out_count = turbo_media_revocation_count(projection->state);
    salts_mutex_unlock(&projection->mutex);
    return 0;
}
