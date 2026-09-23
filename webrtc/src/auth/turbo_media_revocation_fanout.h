#ifndef TURBO_MEDIA_REVOCATION_FANOUT_H
#define TURBO_MEDIA_REVOCATION_FANOUT_H

#include "turbo_media_auth.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MEDIA_REVOCATION_FANOUT_MAX_TARGETS 64U
#define TURBO_MEDIA_REVOCATION_FANOUT_TARGET_ID_BYTES 64U
#define TURBO_MEDIA_REVOCATION_FANOUT_MAX_ATTEMPTS 8U

typedef struct turbo_media_revocation_fanout_s
    turbo_media_revocation_fanout_t;

typedef enum turbo_media_revocation_fanout_transport_result_e {
    TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_APPLIED = 0,
    TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_STALE = 1,
    TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_GAP = 2,
    TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_LIMIT = 3,
    TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_ERROR = 4,
    TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE = 5,
    TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL = 6
} turbo_media_revocation_fanout_transport_result_t;

typedef struct turbo_media_revocation_fanout_response_s {
    int synchronized;
    uint64_t epoch;
    uint64_t sequence;
} turbo_media_revocation_fanout_response_t;

typedef turbo_media_revocation_fanout_transport_result_t
(*turbo_media_revocation_fanout_snapshot_fn)(
    void *transport_context,
    void *target_context,
    const char *target_id,
    uint64_t epoch,
    uint64_t sequence,
    const char *const *sha256_hex,
    size_t count,
    unsigned int attempt,
    turbo_media_revocation_fanout_response_t *response);

typedef turbo_media_revocation_fanout_transport_result_t
(*turbo_media_revocation_fanout_revoke_fn)(
    void *transport_context,
    void *target_context,
    const char *target_id,
    uint64_t epoch,
    uint64_t sequence,
    const char *sha256_hex,
    unsigned int attempt,
    turbo_media_revocation_fanout_response_t *response);

typedef struct turbo_media_revocation_fanout_target_s {
    const char *target_id;
    void *target_context;
} turbo_media_revocation_fanout_target_t;

typedef struct turbo_media_revocation_fanout_config_s {
    unsigned int max_attempts;
    turbo_media_revocation_fanout_snapshot_fn send_snapshot;
    turbo_media_revocation_fanout_revoke_fn send_revoke;
    void *transport_context;
} turbo_media_revocation_fanout_config_t;

typedef enum turbo_media_revocation_fanout_target_state_e {
    TURBO_MEDIA_REVOCATION_FANOUT_TARGET_UNKNOWN = 0,
    TURBO_MEDIA_REVOCATION_FANOUT_TARGET_SYNCHRONIZED = 1,
    TURBO_MEDIA_REVOCATION_FANOUT_TARGET_RECOVERED = 2,
    TURBO_MEDIA_REVOCATION_FANOUT_TARGET_FAILED = 3
} turbo_media_revocation_fanout_target_state_t;

typedef struct turbo_media_revocation_fanout_target_status_s {
    char target_id[TURBO_MEDIA_REVOCATION_FANOUT_TARGET_ID_BYTES];
    turbo_media_revocation_fanout_target_state_t state;
    turbo_media_revocation_fanout_transport_result_t last_transport_result;
    uint64_t epoch;
    uint64_t sequence;
    unsigned int attempts;
} turbo_media_revocation_fanout_target_status_t;

typedef struct turbo_media_revocation_fanout_report_s {
    uint64_t epoch;
    uint64_t sequence;
    size_t target_count;
    size_t synchronized_count;
    size_t recovered_count;
    size_t failed_count;
} turbo_media_revocation_fanout_report_t;

/**
 * Create one bounded fan-out coordinator.
 *
 * The coordinator stores only target identifiers, opaque target handles, ack
 * versions, and delivery status. It never owns bearer tokens, credentials, or
 * revocation fingerprint payloads. The caller owns and serializes all calls.
 */
turbo_media_revocation_fanout_t *turbo_media_revocation_fanout_create(
    const turbo_media_revocation_fanout_config_t *config,
    const turbo_media_revocation_fanout_target_t *targets,
    size_t target_count);

void turbo_media_revocation_fanout_destroy(
    turbo_media_revocation_fanout_t *fanout);

/**
 * Fan out one canonical covering snapshot.
 *
 * A snapshot version may equal the last canonical version for reconciliation,
 * or advance it. Rollback is rejected.
 *
 * Returns 0 when every target is synchronized, 1 on partial delivery, and -1
 * for invalid/canonical-order input.
 */
int turbo_media_revocation_fanout_publish_snapshot(
    turbo_media_revocation_fanout_t *fanout,
    uint64_t epoch,
    uint64_t sequence,
    const char *const *sha256_hex,
    size_t count,
    turbo_media_revocation_fanout_report_t *report);

/**
 * Fan out one exact-next canonical revoke event.
 *
 * The caller supplies a complete covering snapshot for the same version. Any
 * target that is already behind, returns GAP/LIMIT/ERROR/FATAL, or exhausts
 * event retries is reconciled with that snapshot. Retry loops are bounded.
 *
 * Returns 0 when every target is synchronized, 1 on partial delivery, and -1
 * for invalid/canonical-order input.
 */
int turbo_media_revocation_fanout_publish_revoke(
    turbo_media_revocation_fanout_t *fanout,
    uint64_t epoch,
    uint64_t sequence,
    const char *sha256_hex,
    const char *const *covering_sha256_hex,
    size_t covering_count,
    turbo_media_revocation_fanout_report_t *report);

size_t turbo_media_revocation_fanout_target_count(
    const turbo_media_revocation_fanout_t *fanout);

int turbo_media_revocation_fanout_get_target_status(
    const turbo_media_revocation_fanout_t *fanout,
    size_t index,
    turbo_media_revocation_fanout_target_status_t *status);

int turbo_media_revocation_fanout_get_canonical_version(
    const turbo_media_revocation_fanout_t *fanout,
    int *out_initialized,
    uint64_t *out_epoch,
    uint64_t *out_sequence);

#ifdef __cplusplus
}
#endif

#endif
