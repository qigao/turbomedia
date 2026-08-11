#include "ivr_media_reconnect.h"
#include "tinytest_compat.h"
#include <string.h>

static ivr_media_reconnect_t *g_reconnect;

void setUp(void) {
    ivr_media_reconnect_config_t config;
    memset(&config, 0, sizeof(config));
    config.max_attempts = 3;
    config.initial_backoff_ms = 100;
    config.max_backoff_ms = 400;
    config.total_deadline_ms = 1000;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_create(&config, &g_reconnect));
}

void tearDown(void) {
    ivr_media_reconnect_destroy(g_reconnect);
    g_reconnect = NULL;
}

void test_both_links_reconnect(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_start(g_reconnect, 1000,
                                                &generation));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
                          IVR_MEDIA_LINK_CONNECTED, 0, 1010, &event));
    TEST_ASSERT_EQUAL_INT(IVR_MEDIA_RECONNECT_EVENT_NONE, event);
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHEP, generation,
                          IVR_MEDIA_LINK_CONNECTED, 0, 1020, &event));
    TEST_ASSERT_EQUAL_INT(IVR_MEDIA_RECONNECT_EVENT_RECONNECTED, event);
}

void test_retry_generation_and_stale_callback(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    ivr_media_reconnect_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_start(g_reconnect, 0, &generation));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
                          IVR_MEDIA_LINK_FAILED, IVR_MEDIA_ERROR_PEER_FAILED,
                          10, &event));
    TEST_ASSERT_EQUAL_INT(IVR_MEDIA_RECONNECT_EVENT_DISCONNECTED, event);
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_poll(g_reconnect, 110, &generation,
                                               &event));
    TEST_ASSERT_EQUAL_INT(IVR_MEDIA_RECONNECT_EVENT_RETRY_DUE, event);
    TEST_ASSERT_EQUAL_UINT64(2u, generation);
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHEP, 1,
                          IVR_MEDIA_LINK_CONNECTED, 0, 120, &event));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_snapshot(g_reconnect, &snapshot));
    TEST_ASSERT_EQUAL_UINT64(1u, snapshot.stale_callbacks);
}

void test_retry_exhaustion(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_start(g_reconnect, 0, &generation));
    for (int attempt = 0; attempt < 3; ++attempt) {
        TEST_ASSERT_EQUAL(IVR_OK,
                          ivr_media_reconnect_on_state(
                              g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
                              IVR_MEDIA_LINK_FAILED,
                              IVR_MEDIA_ERROR_PEER_FAILED,
                              (uint64_t)(attempt * 200), &event));
        if (attempt < 2) {
            TEST_ASSERT_EQUAL(IVR_OK,
                              ivr_media_reconnect_poll(
                                  g_reconnect, (uint64_t)(attempt * 200 + 200),
                                  &generation, &event));
            TEST_ASSERT_EQUAL_INT(IVR_MEDIA_RECONNECT_EVENT_RETRY_DUE, event);
        }
    }
    TEST_ASSERT_EQUAL_INT(IVR_MEDIA_RECONNECT_EVENT_RETRY_EXHAUSTED, event);
}

void test_input_stall_is_distinct(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    ivr_media_reconnect_snapshot_t snapshot;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_start(g_reconnect, 0, &generation));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHEP, generation,
                          IVR_MEDIA_LINK_DISCONNECTED,
                          IVR_MEDIA_ERROR_INPUT_STALLED, 500, &event));
    TEST_ASSERT_EQUAL_INT(IVR_MEDIA_RECONNECT_EVENT_INPUT_STALLED, event);
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_snapshot(g_reconnect, &snapshot));
    TEST_ASSERT_EQUAL_UINT64(1u, snapshot.input_stalls);
    TEST_ASSERT_TRUE(snapshot.retry_pending);
}

void test_duplicate_failure_does_not_move_retry_deadline(void) {
    uint64_t generation;
    ivr_media_reconnect_event_t event;
    ivr_media_reconnect_snapshot_t first;
    ivr_media_reconnect_snapshot_t duplicate;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_start(g_reconnect, 0, &generation));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHIP, generation,
                          IVR_MEDIA_LINK_DISCONNECTED, IVR_MEDIA_ERROR_NONE,
                          10, &event));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_snapshot(g_reconnect, &first));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_on_state(
                          g_reconnect, IVR_MEDIA_LINK_WHEP, generation,
                          IVR_MEDIA_LINK_FAILED, IVR_MEDIA_ERROR_PEER_FAILED,
                          90, &event));
    TEST_ASSERT_EQUAL_INT(IVR_MEDIA_RECONNECT_EVENT_NONE, event);
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_media_reconnect_snapshot(g_reconnect, &duplicate));
    TEST_ASSERT_EQUAL_UINT64(first.next_retry_at_ms,
                             duplicate.next_retry_at_ms);
}

spec("test_ivr_media_reconnect") {
  before_each() { setUp(); }
  after_each() { tearDown(); }
  TT_TEST(test_both_links_reconnect);
  TT_TEST(test_retry_generation_and_stale_callback);
  TT_TEST(test_retry_exhaustion);
  TT_TEST(test_input_stall_is_distinct);
  TT_TEST(test_duplicate_failure_does_not_move_retry_deadline);
}
