#ifndef TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_H
#define TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_H

#include "turbo_media_tenant_quota.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MEDIA_TENANT_QUOTA_MAX_ISSUED_LEASES 4096U

typedef struct turbo_media_tenant_quota_policy_s {
    const char *tenant_id;
    uint32_t limits[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
} turbo_media_tenant_quota_policy_t;

typedef enum turbo_media_tenant_quota_allocator_result_e {
    TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR = -1,
    TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED = 0,
    TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_STALE = 1,
    TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_GAP = 2,
    TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_LIMIT = 3,
    TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_OVERCOMMITTED = 4,
    TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_NOT_FOUND = 5,
    TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_UNKNOWN = 6
} turbo_media_tenant_quota_allocator_result_t;

typedef struct turbo_media_tenant_quota_allocator_s
    turbo_media_tenant_quota_allocator_t;

/**
 * Shared-control-plane allocator.
 *
 * The allocator owns tenant-wide caps and the bounded set of currently issued
 * per-node leases. It is caller-serialized and starts UNSYNCHRONIZED.
 */
turbo_media_tenant_quota_allocator_t *
turbo_media_tenant_quota_allocator_create(
    size_t max_tenants, size_t max_issued_leases);

void turbo_media_tenant_quota_allocator_destroy(
    turbo_media_tenant_quota_allocator_t *allocator);

/**
 * Restore one complete canonical allocator snapshot.
 *
 * Snapshot leases must be unexpired at now_unix_ms. Every lease must reference
 * a policy tenant and the sum of all lease limits for each tenant/resource
 * must not exceed that policy's global cap.
 */
turbo_media_tenant_quota_allocator_result_t
turbo_media_tenant_quota_allocator_apply_snapshot(
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t epoch,
    uint64_t sequence,
    const turbo_media_tenant_quota_policy_t *policies,
    size_t policy_count,
    const turbo_media_tenant_quota_lease_t *leases,
    size_t lease_count,
    uint64_t now_unix_ms);

/**
 * Replace one tenant global policy at the exact-next canonical version.
 *
 * A tighter policy is rejected as OVERCOMMITTED while live issued leases
 * exceed the requested cap. No state/version changes in that case.
 */
turbo_media_tenant_quota_allocator_result_t
turbo_media_tenant_quota_allocator_set_policy(
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t epoch,
    uint64_t sequence,
    const turbo_media_tenant_quota_policy_t *policy,
    uint64_t now_unix_ms);

/**
 * Issue or replace one node lease at the exact-next canonical version.
 *
 * The allocator prunes expired leases before checking the global invariant.
 * Replacement excludes the previous same tenant/node lease from the sum.
 */
turbo_media_tenant_quota_allocator_result_t
turbo_media_tenant_quota_allocator_grant(
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t epoch,
    uint64_t sequence,
    const turbo_media_tenant_quota_lease_t *lease,
    uint64_t now_unix_ms);

/**
 * Revoke one currently issued tenant/node lease at the exact-next version.
 */
turbo_media_tenant_quota_allocator_result_t
turbo_media_tenant_quota_allocator_revoke(
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t epoch,
    uint64_t sequence,
    const char *tenant_id,
    const char *node_id,
    uint64_t now_unix_ms);

/**
 * Build one node's complete unexpired lease snapshot at allocator version.
 *
 * Returned lease strings borrow allocator storage and remain valid only until
 * the next allocator mutation.
 */
int turbo_media_tenant_quota_allocator_build_node_snapshot(
    turbo_media_tenant_quota_allocator_t *allocator,
    const char *node_id,
    uint64_t now_unix_ms,
    turbo_media_tenant_quota_lease_t *out_leases,
    size_t capacity,
    size_t *out_count,
    uint64_t *out_epoch,
    uint64_t *out_sequence);

/**
 * Inspect current tenant global cap and aggregate unexpired issued limits.
 */
int turbo_media_tenant_quota_allocator_commitment(
    turbo_media_tenant_quota_allocator_t *allocator,
    const char *tenant_id,
    uint64_t now_unix_ms,
    uint32_t out_policy[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT],
    uint32_t out_issued[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT]);

int turbo_media_tenant_quota_allocator_status(
    const turbo_media_tenant_quota_allocator_t *allocator,
    int *out_synchronized,
    uint64_t *out_epoch,
    uint64_t *out_sequence,
    size_t *out_policy_count,
    size_t *out_lease_count);

#ifdef __cplusplus
}
#endif

#endif
