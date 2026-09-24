#include "tinytest.h"
#include "turbo_media_tenant_quota_allocator.h"

#include <string.h>

static turbo_media_tenant_quota_policy_t policy(
    const char *tenant_id, uint32_t connections,
    uint32_t rooms, uint32_t participants,
    uint32_t media_sessions, uint32_t tracks) {
    turbo_media_tenant_quota_policy_t value;
    memset(&value, 0, sizeof(value));
    value.tenant_id = tenant_id;
    value.limits[TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS] =
        connections;
    value.limits[TURBO_MEDIA_TENANT_QUOTA_ROOMS] = rooms;
    value.limits[TURBO_MEDIA_TENANT_QUOTA_PARTICIPANTS] = participants;
    value.limits[TURBO_MEDIA_TENANT_QUOTA_MEDIA_SESSIONS] =
        media_sessions;
    value.limits[TURBO_MEDIA_TENANT_QUOTA_PUBLISHED_TRACKS] = tracks;
    return value;
}

static turbo_media_tenant_quota_lease_t lease(
    const char *tenant_id, const char *node_id,
    uint64_t expires_at_unix_ms, uint32_t connections) {
    turbo_media_tenant_quota_lease_t value;
    memset(&value, 0, sizeof(value));
    value.tenant_id = tenant_id;
    value.node_id = node_id;
    value.expires_at_unix_ms = expires_at_unix_ms;
    value.limits[TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS] =
        connections;
    return value;
}

void test_allocator_never_overissues_tenant_cap_across_nodes(void) {
    turbo_media_tenant_quota_allocator_t *allocator =
        turbo_media_tenant_quota_allocator_create(4U, 8U);
    turbo_media_tenant_quota_policy_t tenant =
        policy("tenant-a", 10U, 0U, 0U, 0U, 0U);
    turbo_media_tenant_quota_lease_t node_a =
        lease("tenant-a", "node-a", 10000U, 6U);
    turbo_media_tenant_quota_lease_t node_b =
        lease("tenant-a", "node-b", 10000U, 4U);
    turbo_media_tenant_quota_lease_t node_c =
        lease("tenant-a", "node-c", 10000U, 1U);
    uint32_t cap[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
    uint32_t issued[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
    int synchronized = 0;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;
    size_t policy_count = 0U;
    size_t lease_count = 0U;

    check_not_null(allocator);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 1U, 0U, &tenant, 1U, NULL, 0U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);

    check_equal(
        (int)turbo_media_tenant_quota_allocator_grant(
            allocator, 1U, 1U, &node_a, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_grant(
            allocator, 1U, 2U, &node_b, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);

    check_equal(
        (int)turbo_media_tenant_quota_allocator_grant(
            allocator, 1U, 3U, &node_c, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_OVERCOMMITTED);
    check_equal(
        turbo_media_tenant_quota_allocator_status(
            allocator, &synchronized, &epoch, &sequence,
            &policy_count, &lease_count), 0);
    check_true(synchronized);
    check_equal((int)sequence, 2);
    check_equal((int)lease_count, 2);

    node_a.limits[TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS] = 5U;
    check_equal(
        (int)turbo_media_tenant_quota_allocator_grant(
            allocator, 1U, 3U, &node_a, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_grant(
            allocator, 1U, 4U, &node_c, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);

    check_equal(
        turbo_media_tenant_quota_allocator_commitment(
            allocator, "tenant-a", 1000U, cap, issued), 0);
    check_equal(
        (int)cap[TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS], 10);
    check_equal(
        (int)issued[TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS], 10);

    turbo_media_tenant_quota_allocator_destroy(allocator);
}

void test_allocator_expiry_and_replacement_reclaim_budget(void) {
    turbo_media_tenant_quota_allocator_t *allocator =
        turbo_media_tenant_quota_allocator_create(2U, 4U);
    turbo_media_tenant_quota_policy_t tenant =
        policy("tenant-a", 10U, 0U, 0U, 0U, 0U);
    turbo_media_tenant_quota_lease_t leases[2] = {
        lease("tenant-a", "node-a", 10000U, 5U),
        lease("tenant-a", "node-b", 2000U, 5U)
    };
    turbo_media_tenant_quota_lease_t node_c =
        lease("tenant-a", "node-c", 10000U, 5U);
    uint32_t cap[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
    uint32_t issued[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];

    check_not_null(allocator);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 2U, 5U, &tenant, 1U, leases, 2U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);

    check_equal(
        (int)turbo_media_tenant_quota_allocator_grant(
            allocator, 2U, 6U, &node_c, 1999U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_OVERCOMMITTED);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_grant(
            allocator, 2U, 6U, &node_c, 2000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);

    check_equal(
        turbo_media_tenant_quota_allocator_commitment(
            allocator, "tenant-a", 2000U, cap, issued), 0);
    check_equal(
        (int)issued[TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS], 10);

    turbo_media_tenant_quota_allocator_destroy(allocator);
}

void test_allocator_policy_shrink_requires_live_commitment_reduction(void) {
    turbo_media_tenant_quota_allocator_t *allocator =
        turbo_media_tenant_quota_allocator_create(2U, 4U);
    turbo_media_tenant_quota_policy_t tenant =
        policy("tenant-a", 10U, 0U, 0U, 0U, 0U);
    turbo_media_tenant_quota_policy_t smaller =
        policy("tenant-a", 5U, 0U, 0U, 0U, 0U);
    turbo_media_tenant_quota_lease_t node_a =
        lease("tenant-a", "node-a", 10000U, 5U);
    turbo_media_tenant_quota_lease_t node_b =
        lease("tenant-a", "node-b", 10000U, 5U);
    int synchronized = 0;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;
    size_t policies = 0U;
    size_t leases = 0U;

    check_not_null(allocator);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 1U, 0U, &tenant, 1U, NULL, 0U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_grant(
            allocator, 1U, 1U, &node_a, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_grant(
            allocator, 1U, 2U, &node_b, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);

    check_equal(
        (int)turbo_media_tenant_quota_allocator_set_policy(
            allocator, 1U, 3U, &smaller, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_OVERCOMMITTED);
    check_equal(
        turbo_media_tenant_quota_allocator_status(
            allocator, &synchronized, &epoch, &sequence,
            &policies, &leases), 0);
    check_true(synchronized);
    check_equal((int)sequence, 2);

    check_equal(
        (int)turbo_media_tenant_quota_allocator_revoke(
            allocator, 1U, 3U, "tenant-a", "node-b", 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_set_policy(
            allocator, 1U, 4U, &smaller, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);

    turbo_media_tenant_quota_allocator_destroy(allocator);
}

void test_allocator_gap_requires_complete_snapshot_recovery(void) {
    turbo_media_tenant_quota_allocator_t *allocator =
        turbo_media_tenant_quota_allocator_create(2U, 4U);
    turbo_media_tenant_quota_policy_t tenant =
        policy("tenant-a", 5U, 0U, 0U, 0U, 0U);
    turbo_media_tenant_quota_lease_t node_a =
        lease("tenant-a", "node-a", 10000U, 5U);
    turbo_media_tenant_quota_lease_t node_b =
        lease("tenant-a", "node-b", 10000U, 1U);
    turbo_media_tenant_quota_lease_t out[2];
    size_t out_count = 0U;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;

    check_not_null(allocator);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 1U, 0U, &tenant, 1U, &node_a, 1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);

    check_equal(
        (int)turbo_media_tenant_quota_allocator_grant(
            allocator, 1U, 2U, &node_b, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_GAP);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_grant(
            allocator, 1U, 1U, &node_a, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_UNKNOWN);
    check_equal(
        turbo_media_tenant_quota_allocator_build_node_snapshot(
            allocator, "node-a", 1000U, out, 2U,
            &out_count, &epoch, &sequence), -1);

    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 1U, 1U, &tenant, 1U, &node_a, 1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_STALE);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 1U, 2U, &tenant, 1U, &node_a, 1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);
    check_equal(
        turbo_media_tenant_quota_allocator_build_node_snapshot(
            allocator, "node-a", 1000U, out, 2U,
            &out_count, &epoch, &sequence), 0);
    check_equal((int)out_count, 1);
    check_equal((int)epoch, 1);
    check_equal((int)sequence, 2);
    check_equal(strcmp(out[0].tenant_id, "tenant-a"), 0);
    check_equal(strcmp(out[0].node_id, "node-a"), 0);

    turbo_media_tenant_quota_allocator_destroy(allocator);
}

void test_allocator_unusable_newer_snapshot_fails_closed_until_recovery(void) {
    turbo_media_tenant_quota_allocator_t *allocator =
        turbo_media_tenant_quota_allocator_create(2U, 4U);
    turbo_media_tenant_quota_policy_t tenant =
        policy("tenant-a", 10U, 0U, 0U, 0U, 0U);
    turbo_media_tenant_quota_lease_t initial =
        lease("tenant-a", "node-a", 10000U, 5U);
    turbo_media_tenant_quota_lease_t over[2] = {
        lease("tenant-a", "node-a", 10000U, 6U),
        lease("tenant-a", "node-b", 10000U, 6U)
    };
    turbo_media_tenant_quota_lease_t out[2];
    size_t out_count = 0U;
    int synchronized = 0;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;
    size_t policy_count = 0U;
    size_t lease_count = 0U;

    check_not_null(allocator);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 1U, 0U, &tenant, 1U,
            &initial, 1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);

    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 1U, 1U, &tenant, 1U,
            over, 2U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR);
    check_equal(
        turbo_media_tenant_quota_allocator_status(
            allocator, &synchronized, &epoch, &sequence,
            &policy_count, &lease_count), 0);
    check_false(synchronized);
    check_equal((int)epoch, 1);
    check_equal((int)sequence, 0);
    check_equal(
        turbo_media_tenant_quota_allocator_build_node_snapshot(
            allocator, "node-a", 1000U, out, 2U,
            &out_count, &epoch, &sequence), -1);

    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 1U, 0U, &tenant, 1U,
            &initial, 1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_STALE);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 1U, 1U, &tenant, 1U,
            &initial, 1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED);
    check_equal(
        turbo_media_tenant_quota_allocator_status(
            allocator, &synchronized, &epoch, &sequence,
            &policy_count, &lease_count), 0);
    check_true(synchronized);
    check_equal((int)sequence, 1);

    turbo_media_tenant_quota_allocator_destroy(allocator);
}

void test_allocator_rejects_overissued_or_ambiguous_snapshots(void) {
    turbo_media_tenant_quota_allocator_t *allocator =
        turbo_media_tenant_quota_allocator_create(2U, 4U);
    turbo_media_tenant_quota_policy_t tenant =
        policy("tenant-a", 5U, 0U, 0U, 0U, 0U);
    turbo_media_tenant_quota_lease_t over[2] = {
        lease("tenant-a", "node-a", 10000U, 3U),
        lease("tenant-a", "node-b", 10000U, 3U)
    };
    turbo_media_tenant_quota_lease_t duplicate[2] = {
        lease("tenant-a", "node-a", 10000U, 2U),
        lease("tenant-a", "node-a", 10000U, 2U)
    };
    int synchronized = 1;
    uint64_t epoch = 99U;
    uint64_t sequence = 99U;
    size_t policies = 99U;
    size_t leases = 99U;

    check_not_null(allocator);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 1U, 0U, &tenant, 1U, over, 2U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR);
    check_equal(
        (int)turbo_media_tenant_quota_allocator_apply_snapshot(
            allocator, 1U, 0U, &tenant, 1U, duplicate, 2U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR);
    check_equal(
        turbo_media_tenant_quota_allocator_status(
            allocator, &synchronized, &epoch, &sequence,
            &policies, &leases), 0);
    check_false(synchronized);
    check_equal((int)epoch, 0);
    check_equal((int)sequence, 0);

    turbo_media_tenant_quota_allocator_destroy(allocator);
}

spec("test_tenant_quota_allocator") {
    it("test_allocator_never_overissues_tenant_cap_across_nodes") {
        test_allocator_never_overissues_tenant_cap_across_nodes();
    };
    it("test_allocator_expiry_and_replacement_reclaim_budget") {
        test_allocator_expiry_and_replacement_reclaim_budget();
    };
    it("test_allocator_policy_shrink_requires_live_commitment_reduction") {
        test_allocator_policy_shrink_requires_live_commitment_reduction();
    };
    it("test_allocator_gap_requires_complete_snapshot_recovery") {
        test_allocator_gap_requires_complete_snapshot_recovery();
    };
    it("test_allocator_unusable_newer_snapshot_fails_closed_until_recovery") {
        test_allocator_unusable_newer_snapshot_fails_closed_until_recovery();
    };
    it("test_allocator_rejects_overissued_or_ambiguous_snapshots") {
        test_allocator_rejects_overissued_or_ambiguous_snapshots();
    };
}
