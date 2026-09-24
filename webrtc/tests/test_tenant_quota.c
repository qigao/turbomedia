#include "tinytest.h"
#include "turbo_media_tenant_quota.h"

#include <string.h>

static turbo_media_tenant_quota_lease_t make_lease(
    const char *tenant_id, const char *node_id, uint64_t expires_at_unix_ms,
    uint32_t connections, uint32_t rooms, uint32_t participants,
    uint32_t media_sessions, uint32_t published_tracks) {
    turbo_media_tenant_quota_lease_t lease;
    memset(&lease, 0, sizeof(lease));
    lease.tenant_id = tenant_id;
    lease.node_id = node_id;
    lease.expires_at_unix_ms = expires_at_unix_ms;
    lease.limits[TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS] =
        connections;
    lease.limits[TURBO_MEDIA_TENANT_QUOTA_ROOMS] = rooms;
    lease.limits[TURBO_MEDIA_TENANT_QUOTA_PARTICIPANTS] = participants;
    lease.limits[TURBO_MEDIA_TENANT_QUOTA_MEDIA_SESSIONS] =
        media_sessions;
    lease.limits[TURBO_MEDIA_TENANT_QUOTA_PUBLISHED_TRACKS] =
        published_tracks;
    return lease;
}

void test_tenant_quota_starts_unknown_and_enforces_local_lease(void) {
    turbo_media_tenant_quota_projection_t *projection =
        turbo_media_tenant_quota_projection_create("node-a", 4U);
    turbo_media_tenant_quota_lease_t lease =
        make_lease("tenant-a", "node-a", 2000U, 2U, 1U, 4U, 2U, 4U);
    uint32_t used = 0U;
    uint32_t limit = 0U;
    uint64_t expires = 0U;
    int has_lease = 0;

    check_not_null(projection);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_UNKNOWN);

    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 1U, 0U, &lease, 1U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);

    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_OK);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_OK);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_LIMIT);

    check_equal(
        turbo_media_tenant_quota_usage(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            &used, &limit, &expires, &has_lease),
        0);
    check_equal((int)used, 2);
    check_equal((int)limit, 2);
    check_equal((int)expires, 2000);
    check_true(has_lease);

    check_equal(
        turbo_media_tenant_quota_release(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS, 1U),
        0);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_ROOMS, 1U, 1999U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_OK);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_PARTICIPANTS, 1U, 2000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_EXPIRED);

    turbo_media_tenant_quota_projection_destroy(projection);
}

void test_tenant_quota_lease_shrink_never_drops_live_usage(void) {
    turbo_media_tenant_quota_projection_t *projection =
        turbo_media_tenant_quota_projection_create("node-a", 2U);
    turbo_media_tenant_quota_lease_t initial =
        make_lease("tenant-a", "node-a", 5000U, 3U, 0U, 0U, 0U, 0U);
    turbo_media_tenant_quota_lease_t shrink =
        make_lease("tenant-a", "node-a", 6000U, 1U, 0U, 0U, 0U, 0U);
    uint32_t used = 0U;
    uint32_t limit = 0U;
    uint64_t expires = 0U;
    int has_lease = 0;

    check_not_null(projection);
    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 1U, 0U, &initial, 1U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            2U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_OK);

    check_equal(
        (int)turbo_media_tenant_quota_apply_update(
            projection, 1U, 1U, &shrink),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);
    check_equal(
        turbo_media_tenant_quota_usage(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            &used, &limit, &expires, &has_lease),
        0);
    check_equal((int)used, 2);
    check_equal((int)limit, 1);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 2000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_LIMIT);

    check_equal(
        turbo_media_tenant_quota_release(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS, 1U),
        0);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 2000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_LIMIT);
    check_equal(
        turbo_media_tenant_quota_release(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS, 1U),
        0);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 2000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_OK);

    turbo_media_tenant_quota_projection_destroy(projection);
}

void test_tenant_quota_gap_fails_closed_and_covering_snapshot_preserves_usage(void) {
    turbo_media_tenant_quota_projection_t *projection =
        turbo_media_tenant_quota_projection_create("node-a", 2U);
    turbo_media_tenant_quota_lease_t lease =
        make_lease("tenant-a", "node-a", 5000U, 3U, 0U, 0U, 0U, 0U);
    int synchronized = 0;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;
    size_t count = 0U;
    uint32_t used = 0U;
    uint32_t limit = 0U;
    uint64_t expires = 0U;
    int has_lease = 0;

    check_not_null(projection);
    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 1U, 0U, &lease, 1U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_OK);

    check_equal(
        (int)turbo_media_tenant_quota_apply_update(
            projection, 1U, 2U, &lease),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_GAP);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_UNKNOWN);

    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 1U, 1U, &lease, 1U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_STALE);
    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 1U, 2U, &lease, 1U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);
    check_equal(
        turbo_media_tenant_quota_status(
            projection, &synchronized, &epoch, &sequence, &count),
        0);
    check_true(synchronized);
    check_equal((int)epoch, 1);
    check_equal((int)sequence, 2);
    check_equal((int)count, 1);
    check_equal(
        turbo_media_tenant_quota_usage(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            &used, &limit, &expires, &has_lease),
        0);
    check_equal((int)used, 1);

    turbo_media_tenant_quota_projection_destroy(projection);
}

void test_tenant_quota_snapshot_removal_keeps_only_drain_tombstone(void) {
    turbo_media_tenant_quota_projection_t *projection =
        turbo_media_tenant_quota_projection_create("node-a", 3U);
    turbo_media_tenant_quota_lease_t initial[2] = {
        make_lease("tenant-a", "node-a", 5000U, 2U, 0U, 0U, 0U, 0U),
        make_lease("tenant-b", "node-a", 5000U, 2U, 0U, 0U, 0U, 0U)
    };
    turbo_media_tenant_quota_lease_t remaining =
        make_lease("tenant-b", "node-a", 6000U, 2U, 0U, 0U, 0U, 0U);
    uint32_t used = 0U;
    uint32_t limit = 0U;
    uint64_t expires = 0U;
    int has_lease = 0;

    check_not_null(projection);
    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 1U, 0U, initial, 2U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_OK);

    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 1U, 1U, &remaining, 1U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);
    check_equal(
        turbo_media_tenant_quota_usage(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            &used, &limit, &expires, &has_lease),
        0);
    check_equal((int)used, 1);
    check_false(has_lease);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_NO_LEASE);

    check_equal(
        turbo_media_tenant_quota_release(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS, 1U),
        0);
    check_equal(
        turbo_media_tenant_quota_usage(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            &used, &limit, &expires, &has_lease),
        -1);

    turbo_media_tenant_quota_projection_destroy(projection);
}

void test_tenant_quota_capacity_failure_recovers_after_drain(void) {
    turbo_media_tenant_quota_projection_t *projection =
        turbo_media_tenant_quota_projection_create("node-a", 1U);
    turbo_media_tenant_quota_lease_t tenant_a =
        make_lease("tenant-a", "node-a", 5000U, 1U, 0U, 0U, 0U, 0U);
    turbo_media_tenant_quota_lease_t tenant_b =
        make_lease("tenant-b", "node-a", 5000U, 1U, 0U, 0U, 0U, 0U);

    check_not_null(projection);
    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 1U, 0U, &tenant_a, 1U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_OK);

    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 1U, 1U, &tenant_b, 1U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_LIMIT);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-b",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_UNKNOWN);

    check_equal(
        turbo_media_tenant_quota_release(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS, 1U),
        0);
    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 1U, 1U, &tenant_b, 1U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-b",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_OK);

    turbo_media_tenant_quota_projection_destroy(projection);
}

void test_tenant_quota_stale_invalid_input_does_not_poison_newer_exact_update_does(void) {
    turbo_media_tenant_quota_projection_t *projection =
        turbo_media_tenant_quota_projection_create("node-a", 3U);
    turbo_media_tenant_quota_lease_t valid =
        make_lease("tenant-a", "node-a", 5000U, 2U, 0U, 0U, 0U, 0U);
    turbo_media_tenant_quota_lease_t invalid =
        make_lease("tenant/a", "wrong-node", 5000U, 2U, 0U, 0U, 0U, 0U);
    turbo_media_tenant_quota_lease_t duplicate[2] = {valid, valid};
    int synchronized = 0;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;
    size_t count = 0U;

    check_not_null(projection);
    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 2U, 5U, &valid, 1U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);

    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 2U, 4U, duplicate, 2U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_STALE);
    check_equal(
        turbo_media_tenant_quota_status(
            projection, &synchronized, &epoch, &sequence, &count),
        0);
    check_true(synchronized);
    check_equal((int)sequence, 5);

    check_equal(
        (int)turbo_media_tenant_quota_apply_update(
            projection, 2U, 6U, &invalid),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_ERROR);
    check_equal(
        turbo_media_tenant_quota_status(
            projection, &synchronized, &epoch, &sequence, &count),
        0);
    check_false(synchronized);
    check_equal((int)sequence, 5);
    check_equal(
        (int)turbo_media_tenant_quota_reserve(
            projection, "tenant-a",
            TURBO_MEDIA_TENANT_QUOTA_SIGNALING_CONNECTIONS,
            1U, 1000U),
        (int)TURBO_MEDIA_TENANT_QUOTA_RESERVE_UNKNOWN);

    check_equal(
        (int)turbo_media_tenant_quota_apply_snapshot(
            projection, 2U, 6U, &valid, 1U),
        (int)TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED);

    turbo_media_tenant_quota_projection_destroy(projection);
}

spec("test_tenant_quota") {
    it("test_tenant_quota_starts_unknown_and_enforces_local_lease") {
        test_tenant_quota_starts_unknown_and_enforces_local_lease();
    };
    it("test_tenant_quota_lease_shrink_never_drops_live_usage") {
        test_tenant_quota_lease_shrink_never_drops_live_usage();
    };
    it("test_tenant_quota_gap_fails_closed_and_covering_snapshot_preserves_usage") {
        test_tenant_quota_gap_fails_closed_and_covering_snapshot_preserves_usage();
    };
    it("test_tenant_quota_snapshot_removal_keeps_only_drain_tombstone") {
        test_tenant_quota_snapshot_removal_keeps_only_drain_tombstone();
    };
    it("test_tenant_quota_capacity_failure_recovers_after_drain") {
        test_tenant_quota_capacity_failure_recovers_after_drain();
    };
    it("test_tenant_quota_stale_invalid_input_does_not_poison_newer_exact_update_does") {
        test_tenant_quota_stale_invalid_input_does_not_poison_newer_exact_update_does();
    };
}
