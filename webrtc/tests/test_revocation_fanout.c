#include "tinytest.h"
#include "turbo_media_revocation_fanout.h"

#include <stdint.h>
#include <string.h>

static const char DIGEST_ZERO[] =
    "0000000000000000000000000000000000000000000000000000000000000000";
static const char DIGEST_ONE[] =
    "1111111111111111111111111111111111111111111111111111111111111111";
static const char DIGEST_UPPER[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

typedef enum fake_mode_e {
    FAKE_APPLIED = 0,
    FAKE_RETRY_ONCE,
    FAKE_GAP,
    FAKE_ALWAYS_RETRY,
    FAKE_STALE_BAD_COVERAGE
} fake_mode_t;

typedef struct fake_target_s {
    fake_mode_t event_mode;
    fake_mode_t snapshot_mode;
    unsigned int event_calls;
    unsigned int snapshot_calls;
} fake_target_t;

static turbo_media_revocation_fanout_transport_result_t fake_result(
    fake_mode_t mode, unsigned int attempt,
    uint64_t epoch, uint64_t sequence,
    turbo_media_revocation_fanout_response_t *response) {
    if (response) {
        memset(response, 0, sizeof(*response));
    }
    if (mode == FAKE_RETRY_ONCE && attempt == 1U) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE;
    }
    if (mode == FAKE_ALWAYS_RETRY) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE;
    }
    if (mode == FAKE_GAP) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_GAP;
    }
    if (mode == FAKE_STALE_BAD_COVERAGE) {
        if (response) {
            response->synchronized = 0;
            response->epoch = epoch;
            response->sequence = sequence;
        }
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_STALE;
    }
    if (response) {
        response->synchronized = 1;
        response->epoch = epoch;
        response->sequence = sequence;
    }
    return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_APPLIED;
}

static turbo_media_revocation_fanout_transport_result_t fake_snapshot(
    void *transport_context, void *target_context, const char *target_id,
    uint64_t epoch, uint64_t sequence,
    const char *const *sha256_hex, size_t count, unsigned int attempt,
    turbo_media_revocation_fanout_response_t *response) {
    fake_target_t *target = (fake_target_t *)target_context;
    (void)transport_context;
    (void)target_id;
    (void)sha256_hex;
    (void)count;
    target->snapshot_calls++;
    return fake_result(target->snapshot_mode, attempt,
                       epoch, sequence, response);
}

static turbo_media_revocation_fanout_transport_result_t fake_revoke(
    void *transport_context, void *target_context, const char *target_id,
    uint64_t epoch, uint64_t sequence, const char *sha256_hex,
    unsigned int attempt,
    turbo_media_revocation_fanout_response_t *response) {
    fake_target_t *target = (fake_target_t *)target_context;
    (void)transport_context;
    (void)target_id;
    (void)sha256_hex;
    target->event_calls++;
    return fake_result(target->event_mode, attempt,
                       epoch, sequence, response);
}

static turbo_media_revocation_fanout_t *make_fanout(
    fake_target_t *fake, size_t count, unsigned int attempts) {
    turbo_media_revocation_fanout_config_t config = {
        .max_attempts = attempts,
        .send_snapshot = fake_snapshot,
        .send_revoke = fake_revoke};
    turbo_media_revocation_fanout_target_t targets[4] = {
        {"node-a", &fake[0]},
        {"node-b", &fake[1]},
        {"node-c", &fake[2]},
        {"node-d", &fake[3]},
    };

    return turbo_media_revocation_fanout_create(
        &config, targets, count);
}

static void reset_calls(fake_target_t *fake, size_t count) {
    for (size_t index = 0U; index < count; ++index) {
        fake[index].event_calls = 0U;
        fake[index].snapshot_calls = 0U;
    }
}

void test_revocation_fanout_retries_and_recovers_with_covering_snapshot(void) {
    fake_target_t fake[4] = {0};
    turbo_media_revocation_fanout_t *fanout =
        make_fanout(fake, 4U, 3U);
    turbo_media_revocation_fanout_report_t report;
    turbo_media_revocation_fanout_target_status_t status;
    const char *covering_one[] = {DIGEST_ZERO};
    const char *covering_two[] = {DIGEST_ZERO, DIGEST_ONE};

    check_not_null(fanout);
    check_equal(turbo_media_revocation_fanout_publish_snapshot(
                    fanout, 1U, 0U, NULL, 0U, &report),
                0);
    check_equal((int)report.synchronized_count, 4);
    check_equal((int)report.failed_count, 0);

    reset_calls(fake, 4U);
    fake[0].event_mode = FAKE_APPLIED;
    fake[1].event_mode = FAKE_RETRY_ONCE;
    fake[2].event_mode = FAKE_GAP;
    fake[2].snapshot_mode = FAKE_APPLIED;
    fake[3].event_mode = FAKE_ALWAYS_RETRY;
    fake[3].snapshot_mode = FAKE_ALWAYS_RETRY;

    check_equal(turbo_media_revocation_fanout_publish_revoke(
                    fanout, 1U, 1U, DIGEST_ZERO,
                    covering_one, 1U, &report),
                1);
    check_equal((int)report.synchronized_count, 3);
    check_equal((int)report.recovered_count, 1);
    check_equal((int)report.failed_count, 1);
    check_equal((int)fake[0].event_calls, 1);
    check_equal((int)fake[1].event_calls, 2);
    check_equal((int)fake[2].event_calls, 1);
    check_equal((int)fake[2].snapshot_calls, 1);
    check_equal((int)fake[3].event_calls, 3);
    check_equal((int)fake[3].snapshot_calls, 3);

    check_equal(turbo_media_revocation_fanout_get_target_status(
                    fanout, 2U, &status),
                0);
    check_equal((int)status.state,
                (int)TURBO_MEDIA_REVOCATION_FANOUT_TARGET_RECOVERED);
    check_equal((int)status.epoch, 1);
    check_equal((int)status.sequence, 1);

    check_equal(turbo_media_revocation_fanout_get_target_status(
                    fanout, 3U, &status),
                0);
    check_equal((int)status.state,
                (int)TURBO_MEDIA_REVOCATION_FANOUT_TARGET_FAILED);

    reset_calls(fake, 4U);
    fake[0].event_mode = FAKE_APPLIED;
    fake[1].event_mode = FAKE_APPLIED;
    fake[2].event_mode = FAKE_APPLIED;
    fake[3].event_mode = FAKE_APPLIED;
    fake[3].snapshot_mode = FAKE_APPLIED;

    check_equal(turbo_media_revocation_fanout_publish_revoke(
                    fanout, 1U, 2U, DIGEST_ONE,
                    covering_two, 2U, &report),
                0);
    check_equal((int)report.synchronized_count, 4);
    check_equal((int)report.recovered_count, 1);
    check_equal((int)report.failed_count, 0);
    check_equal((int)fake[3].event_calls, 0);
    check_equal((int)fake[3].snapshot_calls, 1);

    turbo_media_revocation_fanout_destroy(fanout);
}

void test_revocation_fanout_bad_stale_ack_is_not_trusted(void) {
    fake_target_t fake[1] = {0};
    turbo_media_revocation_fanout_t *fanout =
        make_fanout(fake, 1U, 2U);
    turbo_media_revocation_fanout_report_t report;
    const char *covering[] = {DIGEST_ZERO};

    check_not_null(fanout);
    check_equal(turbo_media_revocation_fanout_publish_snapshot(
                    fanout, 4U, 10U, NULL, 0U, &report),
                0);

    fake[0].event_mode = FAKE_STALE_BAD_COVERAGE;
    fake[0].snapshot_mode = FAKE_STALE_BAD_COVERAGE;
    reset_calls(fake, 1U);
    check_equal(turbo_media_revocation_fanout_publish_revoke(
                    fanout, 4U, 11U, DIGEST_ZERO,
                    covering, 1U, &report),
                1);
    check_equal((int)report.synchronized_count, 0);
    check_equal((int)report.failed_count, 1);
    check_equal((int)fake[0].event_calls, 1);
    check_equal((int)fake[0].snapshot_calls, 1);

    turbo_media_revocation_fanout_destroy(fanout);
}

void test_revocation_fanout_rejects_bad_source_order_and_payloads(void) {
    fake_target_t fake[1] = {0};
    turbo_media_revocation_fanout_t *fanout =
        make_fanout(fake, 1U, 2U);
    turbo_media_revocation_fanout_report_t report;
    const char *duplicate[] = {DIGEST_ZERO, DIGEST_ZERO};
    const char *uppercase[] = {DIGEST_UPPER};
    int initialized = 0;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;

    check_not_null(fanout);
    check_equal(turbo_media_revocation_fanout_publish_revoke(
                    fanout, 1U, 1U, DIGEST_ZERO,
                    NULL, 0U, &report),
                -1);
    check_equal(turbo_media_revocation_fanout_publish_snapshot(
                    fanout, 1U, 0U, duplicate, 2U, &report),
                -1);
    check_equal(turbo_media_revocation_fanout_publish_snapshot(
                    fanout, 1U, 0U, uppercase, 1U, &report),
                -1);
    check_equal((int)fake[0].snapshot_calls, 0);

    check_equal(turbo_media_revocation_fanout_publish_snapshot(
                    fanout, 2U, 5U, NULL, 0U, &report),
                0);
    check_equal(turbo_media_revocation_fanout_publish_revoke(
                    fanout, 2U, 6U, DIGEST_ZERO,
                    NULL, 0U, &report),
                -1);
    check_equal((int)fake[0].event_calls, 0);
    check_equal((int)fake[0].snapshot_calls, 1);
    check_equal(turbo_media_revocation_fanout_publish_revoke(
                    fanout, 2U, 7U, DIGEST_ZERO,
                    NULL, 0U, &report),
                -1);
    check_equal(turbo_media_revocation_fanout_publish_snapshot(
                    fanout, 2U, 4U, NULL, 0U, &report),
                -1);
    check_equal(turbo_media_revocation_fanout_get_canonical_version(
                    fanout, &initialized, &epoch, &sequence),
                0);
    check_true(initialized);
    check_equal((int)epoch, 2);
    check_equal((int)sequence, 5);

    turbo_media_revocation_fanout_destroy(fanout);
}

void test_revocation_fanout_rejects_ambiguous_target_configuration(void) {
    fake_target_t fake[2] = {0};
    turbo_media_revocation_fanout_config_t config = {
        .max_attempts = 2U,
        .send_snapshot = fake_snapshot,
        .send_revoke = fake_revoke};
    turbo_media_revocation_fanout_target_t duplicate[2] = {
        {"node-a", &fake[0]}, {"node-a", &fake[1]}};
    turbo_media_revocation_fanout_target_t invalid[1] = {
        {"node/a", &fake[0]}};

    check_null(turbo_media_revocation_fanout_create(
        &config, duplicate, 2U));
    check_null(turbo_media_revocation_fanout_create(
        &config, invalid, 1U));
    config.max_attempts = 0U;
    check_null(turbo_media_revocation_fanout_create(
        &config, invalid, 1U));
}

spec("test_revocation_fanout") {
    it("test_revocation_fanout_retries_and_recovers_with_covering_snapshot") {
        test_revocation_fanout_retries_and_recovers_with_covering_snapshot();
    };
    it("test_revocation_fanout_bad_stale_ack_is_not_trusted") {
        test_revocation_fanout_bad_stale_ack_is_not_trusted();
    };
    it("test_revocation_fanout_rejects_bad_source_order_and_payloads") {
        test_revocation_fanout_rejects_bad_source_order_and_payloads();
    };
    it("test_revocation_fanout_rejects_ambiguous_target_configuration") {
        test_revocation_fanout_rejects_ambiguous_target_configuration();
    };
}
