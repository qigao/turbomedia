/* test_ivr_worker.c - worker lifecycle, admission, drain (real TurboXML) */
#include "ivr/ivr_worker.h"
#include "ivr_session.h"
#include "ivr_thread.h"
#include "tinytest_compat.h"
#include <stdlib.h>
#include <string.h>

#ifndef IVR_TEST_CONTENT_ROOT
#define IVR_TEST_CONTENT_ROOT "content"
#endif

typedef struct {
    ivr_mutex_t lock;
    int stop_bot_calls;
    int active_instances;
} mock_media_t;

static ivr_status_t mock_start_bot(void *ctx, const ivr_call_ref_t *call) {
    (void)ctx;
    (void)call;
    return IVR_OK;
}
static ivr_status_t mock_play_pcm(void *ctx, const ivr_call_ref_t *call,
                                  const ivr_bytes_view_t *text) {
    (void)ctx;
    (void)call;
    (void)text;
    return IVR_OK;
}
static ivr_status_t mock_cancel_input(void *ctx, const ivr_call_ref_t *call) {
    (void)ctx;
    (void)call;
    return IVR_OK;
}
static ivr_status_t mock_stop_bot(void *ctx, const ivr_call_ref_t *call) {
    mock_media_t *m = (mock_media_t *)ctx;
    ivr_mutex_lock(&m->lock);
    m->stop_bot_calls++;
    ivr_mutex_unlock(&m->lock);
    (void)call;
    return IVR_OK;
}
static ivr_status_t mock_begin_input(void *ctx, const ivr_call_ref_t *call,
                                     const ivr_bytes_view_t *input_id,
                                     uint64_t input_generation) {
    (void)ctx;
    (void)call;
    (void)input_id;
    (void)input_generation;
    return IVR_OK;
}
static ivr_status_t mock_end_input(void *ctx, const ivr_call_ref_t *call,
                                   const ivr_bytes_view_t *input_id,
                                   uint64_t input_generation) {
    (void)ctx;
    (void)call;
    (void)input_id;
    (void)input_generation;
    return IVR_OK;
}

static ivr_status_t mock_gateway_submit(void *ctx, const ivr_command_view_t *cmd) {
    (void)ctx;
    (void)cmd;
    return IVR_OK;
}

static ivr_worker_t *g_worker = NULL;
static mock_media_t g_media;
static ivr_worker_config_t g_config;
static ivr_command_gateway_ops_t g_gateway_ops;
static ivr_media_port_ops_t g_media_ops;
static ivr_media_port_factory_ops_t g_media_factory;
static ivr_call_ref_t g_call;

static ivr_status_t mock_media_create(void *ctx, const ivr_call_ref_t *call,
                                      ivr_media_port_ops_t *out_media,
                                      void **out_instance) {
    (void)call;
    if (!ctx || !out_media || !out_instance) {
        return IVR_EINVAL;
    }
    *out_media = g_media_ops;
    *out_instance = ctx;
    ivr_mutex_lock(&g_media.lock);
    g_media.active_instances++;
    ivr_mutex_unlock(&g_media.lock);
    return IVR_OK;
}

static void mock_media_destroy(void *ctx, void *instance) {
    mock_media_t *media = (mock_media_t *)ctx;
    (void)instance;
    ivr_mutex_lock(&media->lock);
    media->active_instances--;
    ivr_mutex_unlock(&media->lock);
}

void setUp(void) {
    memset(&g_media, 0, sizeof(g_media));
    ivr_mutex_init(&g_media.lock);
    g_gateway_ops.abi_version = 1;
    g_gateway_ops.context = NULL;
    g_gateway_ops.submit_copy = mock_gateway_submit;
    g_media_ops.abi_version = IVR_WORKER_ABI_VERSION;
    g_media_ops.context = &g_media;
    g_media_ops.start_bot = mock_start_bot;
    g_media_ops.play_pcm = mock_play_pcm;
    g_media_ops.cancel_input = mock_cancel_input;
    g_media_ops.stop_bot = mock_stop_bot;
    g_media_ops.begin_input = mock_begin_input;
    g_media_ops.end_input = mock_end_input;
    g_media_factory.abi_version = IVR_WORKER_ABI_VERSION;
    g_media_factory.context = &g_media;
    g_media_factory.create = mock_media_create;
    g_media_factory.destroy = mock_media_destroy;

    memset(&g_config, 0, sizeof(g_config));
    g_config.abi_version = IVR_WORKER_ABI_VERSION;
    g_config.worker_id = "ivr-worker-test";
    g_config.max_sessions_per_worker = 2;
    g_config.session_inbox_capacity = 8;
    g_config.max_event_bytes = 65536;
    g_config.max_command_bytes = 16384;
    g_config.content_root = IVR_TEST_CONTENT_ROOT;
    g_config.drain_deadline_ms = 5000;

    g_call.room_id.data = "room-42";
    g_call.room_id.size = 7;
    g_call.call_id.data = "call-42";
    g_call.call_id.size = 7;
    g_call.call_generation = 1;
    g_call.expected_room_version = 10;
}

void tearDown(void) {
    if (g_worker) {
        ivr_worker_begin_drain(g_worker);
        ivr_worker_destroy(g_worker);
        g_worker = NULL;
    }
    ivr_mutex_destroy(&g_media.lock);
}

void test_create_start_assign_drain(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_NOT_NULL(g_worker);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));

    ivr_session_t *s1 = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting", &s1));
    TEST_ASSERT_NOT_NULL(s1);
    ivr_thread_sleep_ms(50);
    /* drain stops the session and tears down the bot */
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_begin_drain(g_worker));
    ivr_mutex_lock(&g_media.lock);
    int stops = g_media.stop_bot_calls;
    ivr_mutex_unlock(&g_media.lock);
    TEST_ASSERT_TRUE(stops >= 1);
}

void test_old_worker_abi_rejected_with_version_error(void) {
    g_config.abi_version = IVR_WORKER_ABI_VERSION - 1u;
    TEST_ASSERT_EQUAL(IVR_EVERSION,
                      ivr_worker_create(&g_config, &g_gateway_ops,
                                        &g_media_factory, &g_worker));
    TEST_ASSERT_NULL(g_worker);
}

static void noop_latency_observer(void *context,
                                  ivr_session_latency_kind_t kind,
                                  uint64_t duration_ms) {
    (void)context;
    (void)kind;
    (void)duration_ms;
}

void test_old_observer_abi_rejected_with_version_error(void) {
    g_config.observer.abi_version = IVR_SESSION_OBSERVER_ABI_VERSION + 1u;
    g_config.observer.on_latency = noop_latency_observer;
    TEST_ASSERT_EQUAL(IVR_EVERSION,
                      ivr_worker_create(&g_config, &g_gateway_ops,
                                        &g_media_factory, &g_worker));
    TEST_ASSERT_NULL(g_worker);
}

void test_assign_before_start_rejected(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    ivr_session_t *s = NULL;
    TEST_ASSERT_EQUAL(IVR_ECLOSED,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting", &s));
}

void test_admission_at_capacity(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    ivr_session_t *s1 = NULL, *s2 = NULL, *s3 = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting", &s1));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting", &s2));
    /* capacity is 2; the third assignment must be explicitly rejected */
    TEST_ASSERT_EQUAL(IVR_ENOSPC,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting", &s3));
    TEST_ASSERT_NULL(s3);
}

void test_120_percent_burst_rejects_only_excess_call(void) {
    enum { C_TARGET = 4, BURST_ATTEMPTS = 5 };
    static const char *const call_ids[BURST_ATTEMPTS] = {
        "burst-1", "burst-2", "burst-3", "burst-4", "burst-5"};
    ivr_call_ref_t calls[BURST_ATTEMPTS];
    ivr_session_t *sessions[BURST_ATTEMPTS] = {0};
    ivr_event_view_t event;
    static const ivr_bytes_view_t event_type = {"rtc.connected", 13};
    static const ivr_bytes_view_t payload = {"{}", 2};
    int active_instances;

    g_config.max_sessions_per_worker = C_TARGET;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));

    for (int i = 0; i < BURST_ATTEMPTS; ++i) {
        calls[i] = g_call;
        calls[i].call_id.data = call_ids[i];
        calls[i].call_id.size = strlen(call_ids[i]);
    }
    for (int i = 0; i < C_TARGET; ++i) {
        TEST_ASSERT_EQUAL(
            IVR_OK,
            ivr_worker_assign_session(g_worker, &calls[i],
                                      "conference-greeting", &sessions[i]));
        TEST_ASSERT_NOT_NULL(sessions[i]);
    }
    TEST_ASSERT_EQUAL(
        IVR_ENOSPC,
        ivr_worker_assign_session(g_worker, &calls[C_TARGET],
                                  "conference-greeting", &sessions[C_TARGET]));
    TEST_ASSERT_NULL(sessions[C_TARGET]);
    TEST_ASSERT_EQUAL_UINT32(C_TARGET,
                             ivr_worker_active_sessions(g_worker));

    memset(&event, 0, sizeof(event));
    event.event_type = event_type;
    event.call = calls[0];
    event.sequence = 0;
    event.payload_json = payload;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_submit_event_copy(g_worker, &event));
    TEST_ASSERT_EQUAL_UINT32(C_TARGET,
                             ivr_worker_active_sessions(g_worker));

    ivr_mutex_lock(&g_media.lock);
    active_instances = g_media.active_instances;
    ivr_mutex_unlock(&g_media.lock);
    TEST_ASSERT_EQUAL_INT(C_TARGET, active_instances);

    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_begin_drain(g_worker));
    TEST_ASSERT_EQUAL_UINT32(0u, ivr_worker_active_sessions(g_worker));
    ivr_mutex_lock(&g_media.lock);
    active_instances = g_media.active_instances;
    ivr_mutex_unlock(&g_media.lock);
    TEST_ASSERT_EQUAL_INT(0, active_instances);
}

void test_unknown_content_rejected(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    ivr_session_t *s = NULL;
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "no-such-package", &s));
}

void test_drain_deadline_ok(void) {
    g_config.drain_deadline_ms = 5000;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    ivr_session_t *s1 = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting", &s1));
    /* terminal wakes the session quickly, so the deadline must not trip */
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_begin_drain(g_worker));
    TEST_ASSERT_EQUAL_UINT64(0u, ivr_worker_drain_timed_out(g_worker));
}

void test_submit_event_routes_to_matching_session(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    ivr_session_t *s1 = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting", &s1));
    TEST_ASSERT_NOT_NULL(s1);

    /* sequence gap: a state event at sequence 2 while the session is at 0
       must stop the call and emit get_snapshot (routed through the worker) */
    static ivr_bytes_view_t type = {"room.participant.joined", 22};
    static ivr_bytes_view_t payload = {"{}", 2};
    ivr_event_view_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event_type = type;
    ev.call = g_call;
    ev.sequence = 2;
    ev.payload_json = payload;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_submit_event_copy(g_worker, &ev));
    ivr_thread_sleep_ms(100);
    TEST_ASSERT_EQUAL_INT(1, s1->awaiting_snapshot);
    TEST_ASSERT_EQUAL_UINT64(1u, s1->snapshot_requests);

    /* authoritative snapshot at sequence 2 resumes the call */
    static ivr_bytes_view_t snap_type = {"room.snapshot.loaded", 21};
    ev.event_type = snap_type;
    ev.call.expected_room_version = 5;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_submit_event_copy(g_worker, &ev));
    ivr_thread_sleep_ms(100);
    TEST_ASSERT_EQUAL_INT(0, s1->awaiting_snapshot);
    TEST_ASSERT_EQUAL_UINT64(2u, s1->last_sequence);
    TEST_ASSERT_EQUAL_UINT64(5u, s1->room_version);
}

void test_submit_event_unrouted_call_dropped(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    ivr_session_t *s1 = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting", &s1));

    static ivr_bytes_view_t type = {"rtc.connected", 12};
    static ivr_bytes_view_t payload = {"{}", 2};
    static ivr_bytes_view_t other_room = {"room-99", 7};
    static ivr_bytes_view_t other_call = {"call-99", 7};
    ivr_event_view_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event_type = type;
    ev.call.room_id = other_room;
    ev.call.call_id = other_call;
    ev.call.call_generation = 1;
    ev.sequence = 1;
    ev.payload_json = payload;
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      ivr_worker_submit_event_copy(g_worker, &ev));
    /* the assigned session must not have seen the event */
    TEST_ASSERT_EQUAL_UINT64(0u, s1->last_sequence);
}

void test_start_twice_rejected(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_worker_start(g_worker));
}

void test_start_while_draining_rejected(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_begin_drain(g_worker));
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_worker_start(g_worker));
}

void test_destroy_without_explicit_drain(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    ivr_session_t *session = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting", &session));
    TEST_ASSERT_NOT_NULL(session);

    /* destroy() owns the drain transition and must not free a live session. */
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_destroy(g_worker));
    g_worker = NULL;
}

void test_session_destroy_releases_worker_slot(void) {
    ivr_session_t *session = NULL;
    ivr_session_t *replacement = NULL;

    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting", &session));
    TEST_ASSERT_TRUE(ivr_worker_has_session(g_worker, &g_call));

    ivr_session_destroy(session);
    TEST_ASSERT_FALSE(ivr_worker_has_session(g_worker, &g_call));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting", &replacement));
    TEST_ASSERT_NOT_NULL(replacement);
}

void test_release_call_is_idempotent_and_releases_capacity(void) {
    ivr_session_t *session = NULL;

    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&g_config, &g_gateway_ops,
                                                &g_media_factory, &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_assign_session(g_worker, &g_call,
                                                "conference-greeting",
                                                &session));
    TEST_ASSERT_NOT_NULL(session);
    TEST_ASSERT_EQUAL_UINT32(1u, ivr_worker_active_sessions(g_worker));

    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_release_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL_UINT32(0u, ivr_worker_active_sessions(g_worker));
    TEST_ASSERT_FALSE(ivr_worker_has_session(g_worker, &g_call));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_release_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL_UINT32(0u, ivr_worker_active_sessions(g_worker));
}

spec("test_ivr_worker") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_create_start_assign_drain);
  TT_TEST(test_old_worker_abi_rejected_with_version_error);
  TT_TEST(test_old_observer_abi_rejected_with_version_error);
  TT_TEST(test_assign_before_start_rejected);
  TT_TEST(test_admission_at_capacity);
  TT_TEST(test_120_percent_burst_rejects_only_excess_call);
  TT_TEST(test_unknown_content_rejected);
  TT_TEST(test_start_twice_rejected);
  TT_TEST(test_start_while_draining_rejected);
  TT_TEST(test_destroy_without_explicit_drain);
  TT_TEST(test_session_destroy_releases_worker_slot);
  TT_TEST(test_release_call_is_idempotent_and_releases_capacity);
  TT_TEST(test_drain_deadline_ok);
  TT_TEST(test_submit_event_routes_to_matching_session);
  TT_TEST(test_submit_event_unrouted_call_dropped);
}
