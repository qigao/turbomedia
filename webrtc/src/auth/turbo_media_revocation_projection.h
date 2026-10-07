#ifndef TURBO_MEDIA_REVOCATION_PROJECTION_H
#define TURBO_MEDIA_REVOCATION_PROJECTION_H

#include "turbo_media_revocation.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_media_revocation_projection_s
    turbo_media_revocation_projection_t;

turbo_media_revocation_projection_t *
turbo_media_revocation_projection_create(size_t max_entries);

void turbo_media_revocation_projection_destroy(
    turbo_media_revocation_projection_t *projection);

turbo_media_auth_revocation_status_t
turbo_media_revocation_projection_check_digest(
    void *context, const uint8_t *sha256, size_t sha256_size);

turbo_media_revocation_apply_result_t
turbo_media_revocation_projection_apply_snapshot(
    turbo_media_revocation_projection_t *projection,
    uint64_t epoch, uint64_t sequence,
    const char *const *sha256_hex, size_t count);

turbo_media_revocation_apply_result_t
turbo_media_revocation_projection_apply_revoke(
    turbo_media_revocation_projection_t *projection,
    uint64_t epoch, uint64_t sequence, const char *sha256_hex);

int turbo_media_revocation_projection_status(
    turbo_media_revocation_projection_t *projection,
    int *out_synchronized, uint64_t *out_epoch,
    uint64_t *out_sequence, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif
