#include "ivr_media_reconnect.h"
#include "tinytest.h"
#include <string.h>

static ivr_media_reconnect_t *g_reconnect;

void setUp(void) {
    ivr_media_reconnect_config_t config;
    memset(&config, 0, sizeof(config));
    config.max_attempts = 3;
    config.initial_backoff_ms = 100;
    config.max_backoff_ms = 400;
    config.total_deadline_ms = 1000;
    check_equal(ivr_media_reconnect_create(&config, &g_reconnect), IVR_OK);
}

void tearDown(void) {
    ivr_media_reconnect_destroy(g_reconnect);
    g_reconnect = NULL;
}

void test_both_links_reconnect(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    check_equal(ivr_media_reconnect_start(g_reconnect, 1000,
                                                &generation), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
                          IVR_MEDIA_LINK_CONNECTED, 0, 1010, &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_NONE));
    check_equal(ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHEP, generation,
                          IVR_MEDIA_LINK_CONNECTED, 0, 1020, &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_RECONNECTED));
}

void test_retry_generation_and_stale_callback(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    ivr_media_reconnect_snapshot_t snapshot;
    check_equal(ivr_media_reconnect_start(g_reconnect, 0, &generation), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
                          IVR_MEDIA_LINK_FAILED, IVR_MEDIA_ERROR_PEER_FAILED,
                          10, &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_DISCONNECTED));
    check_equal(ivr_media_reconnect_poll(g_reconnect, 110, &generation,
                                               &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_RETRY_DUE));
    check_equal((uint64_t)(generation), (uint64_t)(2u));
    check_equal(ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHEP, 1,
                          IVR_MEDIA_LINK_CONNECTED, 0, 120, &event), IVR_ESTATE);
    check_equal(ivr_media_reconnect_snapshot(g_reconnect, &snapshot), IVR_OK);
    check_equal((uint64_t)(snapshot.stale_callbacks), (uint64_t)(1u));
}

void test_retry_exhaustion(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    check_equal(ivr_media_reconnect_start(g_reconnect, 0, &generation), IVR_OK);
    for (int attempt = 0; attempt < 3; ++attempt) {
        check_equal(ivr_media_reconnect_on_state(
                              g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
                              IVR_MEDIA_LINK_FAILED,
                              IVR_MEDIA_ERROR_PEER_FAILED,
                              (uint64_t)(attempt * 200), &event), IVR_OK);
        check_equal((int)(event), (int)(attempt == 0
                ? IVR_MEDIA_RECONNECT_EVENT_DISCONNECTED
                : attempt == 2
                      ? IVR_MEDIA_RECONNECT_EVENT_RETRY_EXHAUSTED
                      : IVR_MEDIA_RECONNECT_EVENT_NONE));
        if (attempt < 2) {
            check_equal(ivr_media_reconnect_poll(
                                  g_reconnect, (uint64_t)(attempt * 200 + 200),
                                  &generation, &event), IVR_OK);
            check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_RETRY_DUE));
        }
    }
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_RETRY_EXHAUSTED));
}

void test_failed_retry_does_not_duplicate_disconnected_event(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;

    check_equal(ivr_media_reconnect_start(g_reconnect, 0, &generation), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
            g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
            IVR_MEDIA_LINK_FAILED, IVR_MEDIA_ERROR_PEER_FAILED, 10, &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_DISCONNECTED));
    check_equal(ivr_media_reconnect_poll(g_reconnect, 110, &generation, &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_RETRY_DUE));
    check_equal(ivr_media_reconnect_on_state(
            g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
            IVR_MEDIA_LINK_FAILED, IVR_MEDIA_ERROR_PEER_FAILED, 120, &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_NONE));
}

void test_input_stall_is_distinct(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    ivr_media_reconnect_snapshot_t snapshot;
    check_equal(ivr_media_reconnect_start(g_reconnect, 0, &generation), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHEP, generation,
                          IVR_MEDIA_LINK_DISCONNECTED,
                          IVR_MEDIA_ERROR_INPUT_STALLED, 500, &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_INPUT_STALLED));
    check_equal(ivr_media_reconnect_snapshot(g_reconnect, &snapshot), IVR_OK);
    check_equal((uint64_t)(snapshot.input_stalls), (uint64_t)(1u));
    check_true(snapshot.retry_pending);
}

void test_duplicate_failure_does_not_move_retry_deadline(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    ivr_media_reconnect_snapshot_t first;
    ivr_media_reconnect_snapshot_t duplicate;
    check_equal(ivr_media_reconnect_start(g_reconnect, 0, &generation), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
                          IVR_MEDIA_LINK_DISCONNECTED, IVR_MEDIA_ERROR_NONE,
                          10, &event), IVR_OK);
    check_equal(ivr_media_reconnect_snapshot(g_reconnect, &first), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHEP, generation,
                          IVR_MEDIA_LINK_FAILED, IVR_MEDIA_ERROR_PEER_FAILED,
                          90, &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_NONE));
    check_equal(ivr_media_reconnect_snapshot(g_reconnect, &duplicate), IVR_OK);
    check_equal((uint64_t)(duplicate.next_retry_at_ms), (uint64_t)(first.next_retry_at_ms));
}

void test_late_failure_after_stable_connection_gets_fresh_deadline(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    ivr_media_reconnect_snapshot_t snapshot;

    check_equal(ivr_media_reconnect_start(g_reconnect, 1000, &generation), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
            g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
            IVR_MEDIA_LINK_CONNECTED, IVR_MEDIA_ERROR_NONE, 1010, &event), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
            g_reconnect, IVR_MEDIA_LINK_WHEP, generation,
            IVR_MEDIA_LINK_CONNECTED, IVR_MEDIA_ERROR_NONE, 1020, &event), IVR_OK);

    check_equal(ivr_media_reconnect_on_state(
            g_reconnect, IVR_MEDIA_LINK_WHEP, generation,
            IVR_MEDIA_LINK_FAILED, IVR_MEDIA_ERROR_PEER_FAILED, 5000,
            &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_DISCONNECTED));
    check_equal(ivr_media_reconnect_snapshot(g_reconnect, &snapshot), IVR_OK);
    check_equal((uint64_t)(snapshot.deadline_at_ms), (uint64_t)(6000u));
    check_equal((uint64_t)(snapshot.next_retry_at_ms), (uint64_t)(5100u));
    check_equal((uint32_t)(snapshot.attempts_started), (uint32_t)(1u));
    check_true(snapshot.retry_pending);

    check_equal(ivr_media_reconnect_poll(g_reconnect, 5100, &generation,
                                         &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_RETRY_DUE));
    check_equal((uint64_t)(generation), (uint64_t)(2u));
}

void test_recovered_connection_starts_independent_next_recovery(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    ivr_media_reconnect_snapshot_t snapshot;

    check_equal(ivr_media_reconnect_start(g_reconnect, 0, &generation), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
            g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
            IVR_MEDIA_LINK_CONNECTED, IVR_MEDIA_ERROR_NONE, 10, &event), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
            g_reconnect, IVR_MEDIA_LINK_WHEP, generation,
            IVR_MEDIA_LINK_CONNECTED, IVR_MEDIA_ERROR_NONE, 20, &event), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
            g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
            IVR_MEDIA_LINK_FAILED, IVR_MEDIA_ERROR_PEER_FAILED, 2000,
            &event), IVR_OK);
    check_equal(ivr_media_reconnect_poll(g_reconnect, 2100, &generation,
                                         &event), IVR_OK);
    check_equal((uint64_t)(generation), (uint64_t)(2u));
    check_equal(ivr_media_reconnect_on_state(
            g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
            IVR_MEDIA_LINK_CONNECTED, IVR_MEDIA_ERROR_NONE, 2110, &event), IVR_OK);
    check_equal(ivr_media_reconnect_on_state(
            g_reconnect, IVR_MEDIA_LINK_WHEP, generation,
            IVR_MEDIA_LINK_CONNECTED, IVR_MEDIA_ERROR_NONE, 2120, &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_RECONNECTED));

    check_equal(ivr_media_reconnect_on_state(
            g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
            IVR_MEDIA_LINK_DISCONNECTED, IVR_MEDIA_ERROR_NONE, 10000,
            &event), IVR_OK);
    check_equal((int)(event), (int)(IVR_MEDIA_RECONNECT_EVENT_DISCONNECTED));
    check_equal(ivr_media_reconnect_snapshot(g_reconnect, &snapshot), IVR_OK);
    check_equal((uint64_t)(snapshot.deadline_at_ms), (uint64_t)(11000u));
    check_equal((uint32_t)(snapshot.attempts_started), (uint32_t)(1u));
    check_equal((uint64_t)(snapshot.attempt_generation), (uint64_t)(2u));
}

spec("test_ivr_media_reconnect") {
  before_each() { setUp(); }
  after_each() { tearDown(); }
  it("test_both_links_reconnect") { test_both_links_reconnect(); };
  it("test_retry_generation_and_stale_callback") { test_retry_generation_and_stale_callback(); };
  it("test_retry_exhaustion") { test_retry_exhaustion(); };
  it("test_failed_retry_does_not_duplicate_disconnected_event") { test_failed_retry_does_not_duplicate_disconnected_event(); };
  it("test_input_stall_is_distinct") { test_input_stall_is_distinct(); };
  it("test_duplicate_failure_does_not_move_retry_deadline") { test_duplicate_failure_does_not_move_retry_deadline(); };
  it("test_late_failure_after_stable_connection_gets_fresh_deadline") { test_late_failure_after_stable_connection_gets_fresh_deadline(); };
  it("test_recovered_connection_starts_independent_next_recovery") { test_recovered_connection_starts_independent_next_recovery(); };
}
