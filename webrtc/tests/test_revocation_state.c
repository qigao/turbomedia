#include "tinytest.h"
#include "turbo_media_revocation.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char DIGEST_ZERO[] =
    "0000000000000000000000000000000000000000000000000000000000000000";
static const char DIGEST_ONE[] =
    "1111111111111111111111111111111111111111111111111111111111111111";
static const char DIGEST_TWO[] =
    "2222222222222222222222222222222222222222222222222222222222222222";

static uint8_t test_hex_nibble(unsigned char value) {
    if (value >= (unsigned char)'0' && value <= (unsigned char)'9') {
        return (uint8_t)(value - (unsigned char)'0');
    }
    return (uint8_t)(value - (unsigned char)'a' + 10U);
}

static void digest_from_hex(const char *value,
                            uint8_t output[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES]) {
    size_t index;
    for (index = 0; index < TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES; ++index) {
        uint8_t high = test_hex_nibble((unsigned char)value[index * 2U]);
        uint8_t low = test_hex_nibble((unsigned char)value[index * 2U + 1U]);
        output[index] = (uint8_t)((uint8_t)(high << 4U) | low);
    }
}

void test_revocation_state_starts_unknown_and_applies_ordered_events(void) {
    turbo_media_revocation_state_t *state =
        turbo_media_revocation_state_create(4);
    uint8_t zero[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];
    uint8_t one[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];
    const char *snapshot[] = { DIGEST_ZERO };

    check_not_null(state);
    digest_from_hex(DIGEST_ZERO, zero);
    digest_from_hex(DIGEST_ONE, one);

    check_false(turbo_media_revocation_is_synchronized(state));
    check_equal((int)(turbo_media_revocation_check_digest(
                    state, zero, sizeof(zero))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_UNKNOWN));

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 1U, 0U, snapshot, 1U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_true(turbo_media_revocation_is_synchronized(state));
    check_equal((int)turbo_media_revocation_epoch(state), (int)1);
    check_equal((int)turbo_media_revocation_sequence(state), (int)0);
    check_equal((int)turbo_media_revocation_count(state), (int)1);
    check_equal((int)(turbo_media_revocation_check_digest(
                    state, zero, sizeof(zero))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_REVOKED));
    check_equal((int)(turbo_media_revocation_check_digest(
                    state, one, sizeof(one))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_CLEAR));

    check_equal((int)(turbo_media_revocation_apply_revoke(
                    state, 1U, 1U, DIGEST_ONE)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_equal((int)turbo_media_revocation_sequence(state), (int)1);
    check_equal((int)turbo_media_revocation_count(state), (int)2);
    check_equal((int)(turbo_media_revocation_check_digest(
                    state, one, sizeof(one))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_REVOKED));
    check_equal((int)(turbo_media_revocation_apply_revoke(
                    state, 1U, 1U, DIGEST_TWO)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_STALE));
    check_equal((int)turbo_media_revocation_count(state), (int)2);

    turbo_media_revocation_state_destroy(state);
}

void test_revocation_state_gap_requires_covering_snapshot(void) {
    turbo_media_revocation_state_t *state =
        turbo_media_revocation_state_create(4);
    uint8_t zero[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];
    const char *empty_snapshot[] = { NULL };
    const char *recovered[] = { DIGEST_ZERO };

    check_not_null(state);
    digest_from_hex(DIGEST_ZERO, zero);
    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 7U, 3U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));

    check_equal((int)(turbo_media_revocation_apply_revoke(
                    state, 7U, 5U, DIGEST_ZERO)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_GAP));
    check_false(turbo_media_revocation_is_synchronized(state));
    check_equal((int)(turbo_media_revocation_check_digest(
                    state, zero, sizeof(zero))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_UNKNOWN));

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 7U, 4U, empty_snapshot, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_STALE));
    check_false(turbo_media_revocation_is_synchronized(state));

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 7U, 5U, recovered, 1U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_true(turbo_media_revocation_is_synchronized(state));
    check_equal((int)turbo_media_revocation_sequence(state), (int)5);
    check_equal((int)(turbo_media_revocation_check_digest(
                    state, zero, sizeof(zero))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_REVOKED));

    turbo_media_revocation_state_destroy(state);
}

void test_revocation_state_epoch_jump_requires_new_epoch_snapshot(void) {
    turbo_media_revocation_state_t *state =
        turbo_media_revocation_state_create(4);
    uint8_t one[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];

    check_not_null(state);
    digest_from_hex(DIGEST_ONE, one);
    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 3U, 10U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_equal((int)(turbo_media_revocation_apply_revoke(
                    state, 4U, 1U, DIGEST_ONE)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_GAP));
    check_false(turbo_media_revocation_is_synchronized(state));

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 3U, 99U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_STALE));
    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 4U, 0U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_STALE));
    check_false(turbo_media_revocation_is_synchronized(state));

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 4U, 1U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_true(turbo_media_revocation_is_synchronized(state));
    check_equal((int)turbo_media_revocation_epoch(state), (int)4);
    check_equal((int)(turbo_media_revocation_check_digest(
                    state, one, sizeof(one))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_CLEAR));

    turbo_media_revocation_state_destroy(state);
}

void test_revocation_state_capacity_failure_is_fail_closed(void) {
    turbo_media_revocation_state_t *state =
        turbo_media_revocation_state_create(1);
    uint8_t two[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];
    const char *recovered[] = { DIGEST_TWO };

    check_not_null(state);
    digest_from_hex(DIGEST_TWO, two);
    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 1U, 0U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_equal((int)(turbo_media_revocation_apply_revoke(
                    state, 1U, 1U, DIGEST_ZERO)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_equal((int)(turbo_media_revocation_apply_revoke(
                    state, 1U, 2U, DIGEST_TWO)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_LIMIT));
    check_false(turbo_media_revocation_is_synchronized(state));
    check_equal((int)(turbo_media_revocation_check_digest(
                    state, two, sizeof(two))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_UNKNOWN));

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 1U, 2U, recovered, 1U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_equal((int)(turbo_media_revocation_check_digest(
                    state, two, sizeof(two))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_REVOKED));

    turbo_media_revocation_state_destroy(state);
}

void test_revocation_state_restart_is_unknown_until_snapshot(void) {
    turbo_media_revocation_state_t *first =
        turbo_media_revocation_state_create(2);
    turbo_media_revocation_state_t *restarted;
    uint8_t zero[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];

    check_not_null(first);
    digest_from_hex(DIGEST_ZERO, zero);
    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    first, 9U, 4U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_equal((int)(turbo_media_revocation_check_digest(
                    first, zero, sizeof(zero))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_CLEAR));
    turbo_media_revocation_state_destroy(first);

    restarted = turbo_media_revocation_state_create(2);
    check_not_null(restarted);
    check_equal((int)(turbo_media_revocation_check_digest(
                    restarted, zero, sizeof(zero))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_UNKNOWN));
    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    restarted, 9U, 4U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_equal((int)(turbo_media_revocation_check_digest(
                    restarted, zero, sizeof(zero))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_CLEAR));

    turbo_media_revocation_state_destroy(restarted);
}

void test_revocation_state_unapplied_newer_updates_fail_closed(void) {
    turbo_media_revocation_state_t *state =
        turbo_media_revocation_state_create(2);
    uint8_t zero[TURBO_MEDIA_AUTH_TOKEN_SHA256_BYTES];
    const char *uppercase[] = {
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    };

    check_not_null(state);
    digest_from_hex(DIGEST_ZERO, zero);

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 5U, 10U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_true(turbo_media_revocation_is_synchronized(state));

    /* Stale malformed input cannot roll back or poison a complete view. */
    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 5U, 9U, uppercase, 1U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_STALE));
    check_true(turbo_media_revocation_is_synchronized(state));
    check_equal((int)turbo_media_revocation_sequence(state), (int)10);

    /* A newer snapshot we cannot consume means the old view is incomplete. */
    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 5U, 11U, uppercase, 1U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_ERROR));
    check_false(turbo_media_revocation_is_synchronized(state));
    check_equal((int)(turbo_media_revocation_check_digest(
                    state, zero, sizeof(zero))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_UNKNOWN));

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 5U, 11U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_true(turbo_media_revocation_is_synchronized(state));

    /* Stale malformed events are ignored before payload parsing. */
    check_equal((int)(turbo_media_revocation_apply_revoke(
                    state, 5U, 10U, "abc")),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_STALE));
    check_true(turbo_media_revocation_is_synchronized(state));

    /* An exact-next event with unusable payload must fail closed. */
    check_equal((int)(turbo_media_revocation_apply_revoke(
                    state, 5U, 12U, "abc")),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_ERROR));
    check_false(turbo_media_revocation_is_synchronized(state));
    check_equal((int)(turbo_media_revocation_check_digest(
                    state, zero, sizeof(zero))),
                (int)(TURBO_MEDIA_AUTH_REVOCATION_UNKNOWN));

    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 5U, 12U, NULL, 0U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_APPLIED));
    check_true(turbo_media_revocation_is_synchronized(state));

    turbo_media_revocation_state_destroy(state);
}

void test_revocation_state_rejects_malformed_or_duplicate_snapshot_data(void) {
    turbo_media_revocation_state_t *state =
        turbo_media_revocation_state_create(2);
    const char *duplicate[] = { DIGEST_ZERO, DIGEST_ZERO };
    const char *uppercase[] = {
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    };

    check_not_null(state);
    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 1U, 0U, uppercase, 1U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_ERROR));
    check_false(turbo_media_revocation_is_synchronized(state));
    check_equal((int)(turbo_media_revocation_apply_snapshot(
                    state, 1U, 0U, duplicate, 2U)),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_ERROR));
    check_false(turbo_media_revocation_is_synchronized(state));
    check_equal((int)(turbo_media_revocation_apply_revoke(
                    state, 1U, 1U, "abc")),
                (int)(TURBO_MEDIA_REVOCATION_APPLY_ERROR));

    turbo_media_revocation_state_destroy(state);
}

spec("test_revocation_state") {
    it("test_revocation_state_starts_unknown_and_applies_ordered_events") {
        test_revocation_state_starts_unknown_and_applies_ordered_events();
    };
    it("test_revocation_state_gap_requires_covering_snapshot") {
        test_revocation_state_gap_requires_covering_snapshot();
    };
    it("test_revocation_state_epoch_jump_requires_new_epoch_snapshot") {
        test_revocation_state_epoch_jump_requires_new_epoch_snapshot();
    };
    it("test_revocation_state_capacity_failure_is_fail_closed") {
        test_revocation_state_capacity_failure_is_fail_closed();
    };
    it("test_revocation_state_restart_is_unknown_until_snapshot") {
        test_revocation_state_restart_is_unknown_until_snapshot();
    };
    it("test_revocation_state_unapplied_newer_updates_fail_closed") {
        test_revocation_state_unapplied_newer_updates_fail_closed();
    };
    it("test_revocation_state_rejects_malformed_or_duplicate_snapshot_data") {
        test_revocation_state_rejects_malformed_or_duplicate_snapshot_data();
    };
}
