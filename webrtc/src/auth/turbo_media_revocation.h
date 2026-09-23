#ifndef TURBO_MEDIA_REVOCATION_H
#define TURBO_MEDIA_REVOCATION_H

#include "turbo_media_auth.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MEDIA_REVOCATION_SHA256_HEX_BYTES 64

typedef struct turbo_media_revocation_state_s turbo_media_revocation_state_t;

typedef enum turbo_media_revocation_apply_result_e {
    TURBO_MEDIA_REVOCATION_APPLY_ERROR = -1,
    TURBO_MEDIA_REVOCATION_APPLY_APPLIED = 0,
    TURBO_MEDIA_REVOCATION_APPLY_STALE = 1,
    TURBO_MEDIA_REVOCATION_APPLY_GAP = 2,
    TURBO_MEDIA_REVOCATION_APPLY_LIMIT = 3
} turbo_media_revocation_apply_result_t;

/**
 * Create a bounded in-memory revocation view.
 *
 * A newly created state is intentionally UNSYNCHRONIZED. Signed-token
 * authorization using this state must therefore fail closed until a full
 * snapshot is applied.
 */
turbo_media_revocation_state_t *turbo_media_revocation_state_create(
    size_t max_entries);
void turbo_media_revocation_state_destroy(
    turbo_media_revocation_state_t *state);

/**
 * Replace the complete revocation set with one versioned snapshot.
 *
 * epoch must be non-zero. A snapshot newer than the last required gap version
 * restores synchronization. Older/equal snapshots are stale no-ops.
 * If an admissible newer snapshot cannot be parsed or installed, the state
 * becomes UNSYNCHRONIZED and authorization must remain fail-closed until a
 * covering valid snapshot is applied.
 */
turbo_media_revocation_apply_result_t turbo_media_revocation_apply_snapshot(
    turbo_media_revocation_state_t *state,
    uint64_t epoch,
    uint64_t sequence,
    const char *const *sha256_hex,
    size_t count);

/**
 * Apply one ordered revoke event.
 *
 * Events are accepted only for the current epoch and exact next sequence.
 * A sequence gap or epoch jump marks the view UNSYNCHRONIZED and returns GAP.
 * Capacity exhaustion also marks the view UNSYNCHRONIZED and returns LIMIT.
 * If the exact-next event cannot be parsed/applied, the view likewise becomes
 * UNSYNCHRONIZED and requires a covering snapshot.
 */
turbo_media_revocation_apply_result_t turbo_media_revocation_apply_revoke(
    turbo_media_revocation_state_t *state,
    uint64_t epoch,
    uint64_t sequence,
    const char *sha256_hex);

int turbo_media_revocation_is_synchronized(
    const turbo_media_revocation_state_t *state);
uint64_t turbo_media_revocation_epoch(
    const turbo_media_revocation_state_t *state);
uint64_t turbo_media_revocation_sequence(
    const turbo_media_revocation_state_t *state);
size_t turbo_media_revocation_count(
    const turbo_media_revocation_state_t *state);

/**
 * Auth callback adapter. UNKNOWN and malformed inputs remain UNKNOWN.
 */
turbo_media_auth_revocation_status_t turbo_media_revocation_check_digest(
    void *context,
    const uint8_t *sha256,
    size_t sha256_size);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_REVOCATION_H */
