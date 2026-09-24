#ifndef TURBO_MEDIA_TENANT_QUOTA_H
#define TURBO_MEDIA_TENANT_QUOTA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS = 256,
    TURBO_MEDIA_TENANT_QUOTA_TENANT_ID_BYTES = 256,
    TURBO_MEDIA_TENANT_QUOTA_NODE_ID_BYTES = 128
};

typedef enum turbo_media_tenant_quota_resource_e {
    TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS = 0,
    TURBO_MEDIA_TENANT_QUOTA_ROOMS = 1,
    TURBO_MEDIA_TENANT_QUOTA_PARTICIPANTS = 2,
    TURBO_MEDIA_TENANT_QUOTA_MEDIA_SESSIONS = 3,
    TURBO_MEDIA_TENANT_QUOTA_PUBLISHED_TRACKS = 4,
    TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT = 5
} turbo_media_tenant_quota_resource_t;

typedef struct turbo_media_tenant_quota_lease_s {
    const char *tenant_id;
    const char *node_id;
    uint64_t expires_at_unix_ms;
    uint32_t limits[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
} turbo_media_tenant_quota_lease_t;

typedef enum turbo_media_tenant_quota_apply_result_e {
    TURBO_MEDIA_TENANT_QUOTA_APPLY_ERROR = -1,
    TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED = 0,
    TURBO_MEDIA_TENANT_QUOTA_APPLY_STALE = 1,
    TURBO_MEDIA_TENANT_QUOTA_APPLY_GAP = 2,
    TURBO_MEDIA_TENANT_QUOTA_APPLY_LIMIT = 3
} turbo_media_tenant_quota_apply_result_t;

typedef enum turbo_media_tenant_quota_reserve_result_e {
    TURBO_MEDIA_TENANT_QUOTA_RESERVE_ERROR = -1,
    TURBO_MEDIA_TENANT_QUOTA_RESERVE_OK = 0,
    TURBO_MEDIA_TENANT_QUOTA_RESERVE_UNKNOWN = 1,
    TURBO_MEDIA_TENANT_QUOTA_RESERVE_NO_LEASE = 2,
    TURBO_MEDIA_TENANT_QUOTA_RESERVE_EXPIRED = 3,
    TURBO_MEDIA_TENANT_QUOTA_RESERVE_LIMIT = 4
} turbo_media_tenant_quota_reserve_result_t;

typedef struct turbo_media_tenant_quota_projection_s
    turbo_media_tenant_quota_projection_t;

/**
 * Creates one caller-serialized per-node tenant quota projection.
 *
 * The node id is copied. max_tenants is a hard memory bound and must be
 * 1..TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS.
 *
 * The projection starts UNSYNCHRONIZED. New reservations fail closed until a
 * covering snapshot is applied.
 */
turbo_media_tenant_quota_projection_t *
turbo_media_tenant_quota_projection_create(
    const char *node_id, size_t max_tenants);

void turbo_media_tenant_quota_projection_destroy(
    turbo_media_tenant_quota_projection_t *projection);

/**
 * Applies a complete lease snapshot for this node.
 *
 * Snapshot entries are unique by tenant id and must all target this node.
 * Existing local usage is preserved. A tenant omitted from the snapshot loses
 * its lease immediately, but any already-reserved usage is retained solely so
 * release can drain it safely.
 */
turbo_media_tenant_quota_apply_result_t
turbo_media_tenant_quota_apply_snapshot(
    turbo_media_tenant_quota_projection_t *projection,
    uint64_t epoch,
    uint64_t sequence,
    const turbo_media_tenant_quota_lease_t *leases,
    size_t lease_count);

/**
 * Applies one exact-next lease update for one tenant.
 *
 * A gap, epoch jump, capacity failure, or unusable exact-next update marks the
 * projection UNSYNCHRONIZED until a covering snapshot is applied.
 */
turbo_media_tenant_quota_apply_result_t
turbo_media_tenant_quota_apply_update(
    turbo_media_tenant_quota_projection_t *projection,
    uint64_t epoch,
    uint64_t sequence,
    const turbo_media_tenant_quota_lease_t *lease);

/**
 * Reserves local usage from the node lease assigned to tenant_id.
 *
 * expires_at_unix_ms and now_unix_ms use Unix epoch milliseconds.
 * UNKNOWN/UNSYNCHRONIZED, absent lease, expired lease, and limit exhaustion
 * deny the reservation without partial mutation.
 */
turbo_media_tenant_quota_reserve_result_t
turbo_media_tenant_quota_reserve(
    turbo_media_tenant_quota_projection_t *projection,
    const char *tenant_id,
    turbo_media_tenant_quota_resource_t resource,
    uint32_t amount,
    uint64_t now_unix_ms);

/**
 * Releases already-reserved usage.
 *
 * Release is allowed even while the projection is UNSYNCHRONIZED or the lease
 * is absent/expired so resource ownership can always drain. Over-release is an
 * error and does not mutate state.
 */
int turbo_media_tenant_quota_release(
    turbo_media_tenant_quota_projection_t *projection,
    const char *tenant_id,
    turbo_media_tenant_quota_resource_t resource,
    uint32_t amount);

int turbo_media_tenant_quota_status(
    const turbo_media_tenant_quota_projection_t *projection,
    int *out_synchronized,
    uint64_t *out_epoch,
    uint64_t *out_sequence,
    size_t *out_lease_count);

int turbo_media_tenant_quota_usage(
    const turbo_media_tenant_quota_projection_t *projection,
    const char *tenant_id,
    turbo_media_tenant_quota_resource_t resource,
    uint32_t *out_used,
    uint32_t *out_limit,
    uint64_t *out_expires_at_unix_ms,
    int *out_has_lease);

#ifdef __cplusplus
}
#endif

#endif
