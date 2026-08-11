/* test_ivr_session.c - per-call session core: sequence/gap, input window,
   terminal latch, bounded inbox (fake engine + mock gateway/media). */
#include "ivr_session.h"
#include "ivr_thread.h"
#include "tinytest_compat.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* mock gateway                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    ivr_mutex_t lock;
    char commands[16][64];
    size_t command_count;
} mock_gateway_t;

static ivr_status_t mock_gateway_submit(void *ctx, const ivr_command_view_t *cmd) {
    mock_gateway_t *g = (mock_gateway_t *)ctx;
    ivr_mutex_lock(&g->lock);
    if (g->command_count < 16) {
        size_t n = cmd->command_type.size < 63 ? cmd->command_type.size : 63;
        memcpy(g->commands[g->command_count], cmd->command_type.data, n);
        g->commands[g->command_count][n] = '\0';
        g->command_count++;
    }
    ivr_mutex_unlock(&g->lock);
    return IVR_OK;
}

static size_t mock_gateway_count(mock_gateway_t *g) {
    ivr_mutex_lock(&g->lock);
    size_t n = g->command_count;
    ivr_mutex_unlock(&g->lock);
    return n;
}

static const char *mock_gateway_last(mock_gateway_t *g) {
    ivr_mutex_lock(&g->lock);
    const char *s = g->command_count > 0 ? g->commands[g->command_count - 1] : "";
    static char last[64];
    snprintf(last, sizeof(last), "%s", s);
    ivr_mutex_unlock(&g->lock);
    return last;
}

/* ------------------------------------------------------------------ */
/* fake engine                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const ivr_engine_sink_t *sink;
    ivr_session_hooks_t hooks;
    ivr_mutex_t lock;
    char events[32][64];
    size_t event_count;
    int block_step; /* when set, step waits on the session terminal */
} fake_engine_t;

static ivr_xml_engine_t *fake_create(const void *content,
                                     const ivr_engine_sink_t *sink,
                                     const ivr_session_hooks_t *hooks,
                                     const ivr_call_ref_t *call) {
    (void)content;
    (void)call;
    fake_engine_t *e = (fake_engine_t *)calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    e->sink = sink;
    e->hooks = *hooks;
    ivr_mutex_init(&e->lock);
    return (ivr_xml_engine_t *)e;
}

static ivr_status_t fake_submit(ivr_xml_engine_t *engine,
                                const ivr_event_t *event) {
    fake_engine_t *e = (fake_engine_t *)engine;
    ivr_mutex_lock(&e->lock);
    if (e->event_count < 32) {
        size_t n = event->event_type.size < 63 ? event->event_type.size : 63;
        memcpy(e->events[e->event_count], event->event_type.data, n);
        e->events[e->event_count][n] = '\0';
        e->event_count++;
    }
    ivr_mutex_unlock(&e->lock);
    return IVR_OK;
}

static ivr_status_t fake_step(ivr_xml_engine_t *engine) {
    fake_engine_t *e = (fake_engine_t *)engine;
    if (e->block_step) {
        /* simulate blocking VoiceXML collect_input: wait for terminal */
        while (!e->hooks.is_terminal(e->hooks.context)) {
            /* session input wait is exercised directly in tests; here we just
               yield so the test can observe the blocked state */
            break;
        }
    }
    return IVR_OK;
}

static void fake_request_terminal(ivr_xml_engine_t *engine) {
    (void)engine;
}

static void fake_destroy(ivr_xml_engine_t *engine) {
    fake_engine_t *e = (fake_engine_t *)engine;
    if (!e) {
        return;
    }
    ivr_mutex_destroy(&e->lock);
    free(e);
}

static const ivr_xml_engine_ops_t fake_engine_ops = {
    .create = fake_create,
    .submit_event = fake_submit,
    .step = fake_step,
    .request_terminal = fake_request_terminal,
    .destroy = fake_destroy,
};

static size_t fake_event_count(ivr_xml_engine_t *engine) {
    fake_engine_t *e = (fake_engine_t *)engine;
    ivr_mutex_lock(&e->lock);
    size_t n = e->event_count;
    ivr_mutex_unlock(&e->lock);
    return n;
}

static const char *fake_event_at(ivr_xml_engine_t *engine, size_t i) {
    fake_engine_t *e = (fake_engine_t *)engine;
    static char out[64];
    ivr_mutex_lock(&e->lock);
    if (i < e->event_count) {
        snprintf(out, sizeof(out), "%s", e->events[i]);
    } else {
        out[0] = '\0';
    }
    ivr_mutex_unlock(&e->lock);
    return out;
}

/* ------------------------------------------------------------------ */
/* test scaffolding                                                    */
/* ------------------------------------------------------------------ */

static mock_gateway_t g_gateway;
static ivr_command_gateway_ops_t g_gateway_ops;
static ivr_media_port_ops_t g_media_ops;
static ivr_call_ref_t g_call;
static ivr_session_t g_session;
static struct {
    unsigned begin_calls;
    unsigned end_calls;
    ivr_status_t begin_status;
    ivr_status_t end_status;
    char last_begin_id[32];
    char last_end_id[32];
} g_input_hooks;

static ivr_status_t mock_begin_input(void *ctx, const ivr_call_ref_t *call,
                                     const ivr_bytes_view_t *input_id,
                                     uint64_t input_generation) {
    (void)ctx;
    (void)call;
    TEST_ASSERT_TRUE(input_generation > 0);
    g_input_hooks.begin_calls++;
    snprintf(g_input_hooks.last_begin_id,
             sizeof(g_input_hooks.last_begin_id), "%.*s", (int)input_id->size,
             input_id->data);
    return g_input_hooks.begin_status;
}

static ivr_status_t mock_end_input(void *ctx, const ivr_call_ref_t *call,
                                   const ivr_bytes_view_t *input_id,
                                   uint64_t input_generation) {
    (void)ctx;
    (void)call;
    TEST_ASSERT_TRUE(input_generation > 0);
    g_input_hooks.end_calls++;
    snprintf(g_input_hooks.last_end_id, sizeof(g_input_hooks.last_end_id),
             "%.*s", (int)input_id->size, input_id->data);
    return g_input_hooks.end_status;
}

static void *terminal_delay_helper(void *opaque) {
    (void)opaque;
    ivr_thread_sleep_ms(30);
    ivr_session_request_terminal(&g_session);
    return NULL;
}

static void make_event(ivr_event_view_t *view, const char *type,
                       const char *payload, uint64_t seq,
                       uint64_t generation, const char *input_id,
                       const char *input_value) {
    static ivr_bytes_view_t ev_id = {"ev-1", 4};
    static ivr_bytes_view_t payload_view;
    memset(view, 0, sizeof(*view));
    view->event_id = ev_id;
    view->event_type.data = type;
    view->event_type.size = strlen(type);
    payload_view.data = payload;
    payload_view.size = payload ? strlen(payload) : 0;
    view->payload_json = payload_view;
    view->call = g_call;
    view->call.call_generation = generation;
    view->sequence = seq;
    view->input_id.data = input_id;
    view->input_id.size = input_id ? strlen(input_id) : 0;
    view->input_value.data = input_value;
    view->input_value.size = input_value ? strlen(input_value) : 0;
}

void setUp(void) {
    memset(&g_gateway, 0, sizeof(g_gateway));
    ivr_mutex_init(&g_gateway.lock);
    g_gateway_ops.abi_version = 1;
    g_gateway_ops.context = &g_gateway;
    g_gateway_ops.submit_copy = mock_gateway_submit;
    memset(&g_media_ops, 0, sizeof(g_media_ops));
    memset(&g_input_hooks, 0, sizeof(g_input_hooks));
    g_media_ops.abi_version = IVR_WORKER_ABI_VERSION;
    g_media_ops.begin_input = mock_begin_input;
    g_media_ops.end_input = mock_end_input;

    g_call.room_id.data = "room-42";
    g_call.room_id.size = 7;
    g_call.call_id.data = "call-42";
    g_call.call_id.size = 7;
    g_call.call_generation = 1;
    g_call.expected_room_version = 10;

    int rc = ivr_session_create(&g_session, &g_call, 8, 4096, 4096,
                                &fake_engine_ops, NULL, &g_gateway_ops,
                                &g_media_ops);
    TEST_ASSERT_EQUAL(IVR_OK, rc);
}

void tearDown(void) {
    ivr_session_request_terminal(&g_session);
    ivr_session_join(&g_session);
    ivr_session_free(&g_session);
    ivr_mutex_destroy(&g_gateway.lock);
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

void test_sequence_contiguous_delivered(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_start(&g_session));
    ivr_event_view_t ev;
    make_event(&ev, "room.assigned", "{}", 1, 1, NULL, NULL);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_submit_event_copy(&g_session, &ev));
    make_event(&ev, "rtc.connected", "{}", 2, 1, NULL, NULL);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_submit_event_copy(&g_session, &ev));
    /* wait for the control thread to consume */
    for (int i = 0; i < 200 && fake_event_count(g_session.engine) < 2; i++) {
        ivr_thread_sleep_ms(10);
    }
    TEST_ASSERT_EQUAL_UINT64(2u, (uint64_t)fake_event_count(g_session.engine));
    TEST_ASSERT_EQUAL_STRING("room.assigned", fake_event_at(g_session.engine, 0));
    TEST_ASSERT_EQUAL_STRING("rtc.connected", fake_event_at(g_session.engine, 1));
}

void test_sequence_gap_emits_get_snapshot(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_start(&g_session));
    ivr_event_view_t ev;
    make_event(&ev, "room.assigned", "{}", 1, 1, NULL, NULL);
    ivr_session_submit_event_copy(&g_session, &ev);
    /* gap: sequence jumps from 1 to 3 */
    make_event(&ev, "rtc.connected", "{}", 3, 1, NULL, NULL);
    ivr_session_submit_event_copy(&g_session, &ev);
    for (int i = 0; i < 200 && mock_gateway_count(&g_gateway) == 0; i++) {
        ivr_thread_sleep_ms(10);
    }
    TEST_ASSERT_TRUE(mock_gateway_count(&g_gateway) >= 1);
    TEST_ASSERT_EQUAL_STRING("get_snapshot", mock_gateway_last(&g_gateway));
    /* no event delivered across the gap */
    TEST_ASSERT_EQUAL_UINT64(1u, (uint64_t)fake_event_count(g_session.engine));
    TEST_ASSERT_EQUAL_UINT64(1u, g_session.snapshot_requests);
}

void test_snapshot_resumes_after_gap(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_start(&g_session));
    ivr_event_view_t ev;
    make_event(&ev, "room.assigned", "{}", 1, 1, NULL, NULL);
    ivr_session_submit_event_copy(&g_session, &ev);
    make_event(&ev, "rtc.connected", "{}", 3, 1, NULL, NULL);
    ivr_session_submit_event_copy(&g_session, &ev);
    for (int i = 0; i < 200 && mock_gateway_count(&g_gateway) == 0; i++) {
        ivr_thread_sleep_ms(10);
    }
    /* authoritative snapshot lands at sequence 3 */
    make_event(&ev, "room.snapshot.loaded", "{}", 3, 1, NULL, NULL);
    ivr_session_submit_event_copy(&g_session, &ev);
    for (int i = 0; i < 200 && fake_event_count(g_session.engine) < 2; i++) {
        ivr_thread_sleep_ms(10);
    }
    TEST_ASSERT_EQUAL_UINT64(2u, (uint64_t)fake_event_count(g_session.engine));
    TEST_ASSERT_EQUAL_STRING("room.snapshot.loaded",
                             fake_event_at(g_session.engine, 1));
    TEST_ASSERT_EQUAL(0, g_session.awaiting_snapshot);
}

void test_duplicate_sequence_dropped(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_start(&g_session));
    ivr_event_view_t ev;
    make_event(&ev, "room.assigned", "{}", 1, 1, NULL, NULL);
    ivr_session_submit_event_copy(&g_session, &ev);
    for (int i = 0; i < 200 && fake_event_count(g_session.engine) < 1; i++) {
        ivr_thread_sleep_ms(10);
    }
    make_event(&ev, "room.assigned", "{}", 1, 1, NULL, NULL);
    ivr_session_submit_event_copy(&g_session, &ev);
    ivr_thread_sleep_ms(50);
    TEST_ASSERT_EQUAL_UINT64(1u, (uint64_t)fake_event_count(g_session.engine));
    TEST_ASSERT_EQUAL_UINT64(1u, g_session.dropped_duplicates);
}

void test_command_result_delivered_without_advancing_domain_sequence(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_start(&g_session));
    ivr_event_view_t ev;
    make_event(&ev, "command.result",
               "{\"message_id\":\"mid-1\",\"status_code\":-4}", 0, 1,
               NULL, NULL);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_submit_event_copy(&g_session, &ev));
    for (int i = 0; i < 200 && fake_event_count(g_session.engine) < 1; i++) {
        ivr_thread_sleep_ms(10);
    }
    TEST_ASSERT_EQUAL_UINT64(1u, (uint64_t)fake_event_count(g_session.engine));
    TEST_ASSERT_EQUAL_STRING("command.result",
                             fake_event_at(g_session.engine, 0));
    TEST_ASSERT_EQUAL_UINT64(0u, g_session.last_sequence);
    TEST_ASSERT_EQUAL_UINT64(0u, g_session.dropped_zero_sequence);
}

void test_generation_mismatch_dropped(void) {
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_start(&g_session));
    ivr_event_view_t ev;
    make_event(&ev, "room.assigned", "{}", 1, 2 /* wrong generation */, NULL, NULL);
    ivr_session_submit_event_copy(&g_session, &ev);
    ivr_thread_sleep_ms(50);
    TEST_ASSERT_EQUAL_UINT64(0u, (uint64_t)fake_event_count(g_session.engine));
    TEST_ASSERT_EQUAL_UINT64(1u, g_session.dropped_generation);
}

void test_input_first_final_wins(void) {
    ivr_bytes_view_t wid = {"w1", 2};
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_session_begin_input_window(&g_session, &wid));
    ivr_event_view_t ev;
    make_event(&ev, "dtmf.final", "{}", 0, 1, "w1", "1");
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_submit_event_copy(&g_session, &ev));
    ivr_str_t value;
    ivr_str_init(&value);
    TEST_ASSERT_EQUAL(1, ivr_session_input_wait(&g_session, 100, &value));
    TEST_ASSERT_EQUAL_STRING("1", value.data);
    /* second final in the same window is stale */
    make_event(&ev, "dtmf.final", "{}", 0, 1, "w1", "2");
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      ivr_session_submit_event_copy(&g_session, &ev));
    TEST_ASSERT_EQUAL_UINT64(1u, g_session.dropped_stale_inputs);
    ivr_str_free(&value);
}

void test_stale_input_window_dropped(void) {
    ivr_bytes_view_t wid = {"w1", 2};
    ivr_session_begin_input_window(&g_session, &wid);
    ivr_event_view_t ev;
    make_event(&ev, "asr.final", "{}", 0, 1, "w0", "hello");
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      ivr_session_submit_event_copy(&g_session, &ev));
    TEST_ASSERT_EQUAL_UINT64(1u, g_session.dropped_stale_inputs);
}

void test_replacing_input_window_ends_previous_first(void) {
    ivr_bytes_view_t first = {"w1", 2};
    ivr_bytes_view_t second = {"w2", 2};
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_session_begin_input_window(&g_session, &first));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_session_begin_input_window(&g_session, &second));
    TEST_ASSERT_EQUAL_UINT64(2u, g_input_hooks.begin_calls);
    TEST_ASSERT_EQUAL_UINT64(1u, g_input_hooks.end_calls);
    TEST_ASSERT_EQUAL_STRING("w1", g_input_hooks.last_end_id);
    TEST_ASSERT_EQUAL_STRING("w2", g_input_hooks.last_begin_id);
}

void test_begin_input_hook_failure_closes_local_window(void) {
    ivr_bytes_view_t window = {"w1", 2};
    g_input_hooks.begin_status = IVR_ESTATE;
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      ivr_session_begin_input_window(&g_session, &window));
    TEST_ASSERT_EQUAL_UINT64(1u, g_input_hooks.begin_calls);
    TEST_ASSERT_EQUAL_UINT64(0u, g_session.input_window_id.size);
}

void test_end_input_hook_failure_blocks_replacement(void) {
    ivr_bytes_view_t first = {"w1", 2};
    ivr_bytes_view_t second = {"w2", 2};
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_session_begin_input_window(&g_session, &first));
    g_input_hooks.end_status = IVR_ESTATE;
    TEST_ASSERT_EQUAL(IVR_ESTATE,
                      ivr_session_begin_input_window(&g_session, &second));
    TEST_ASSERT_EQUAL_UINT64(1u, g_input_hooks.begin_calls);
    TEST_ASSERT_EQUAL_UINT64(1u, g_input_hooks.end_calls);
    TEST_ASSERT_EQUAL_UINT64(0u, g_session.input_window_id.size);
}

void test_end_input_hook_failure_fails_wait_cleanup(void) {
    ivr_bytes_view_t window = {"w1", 2};
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_session_begin_input_window(&g_session, &window));
    g_input_hooks.end_status = IVR_ESTATE;
    TEST_ASSERT_EQUAL(-1, ivr_session_input_wait(&g_session, 0, NULL));
    TEST_ASSERT_EQUAL_UINT64(1u, g_input_hooks.end_calls);
    TEST_ASSERT_EQUAL_UINT64(0u, g_session.input_window_id.size);
}

void test_terminal_wakes_input_wait(void) {
    ivr_bytes_view_t wid = {"w1", 2};
    ivr_session_begin_input_window(&g_session, &wid);
    /* ask terminal from another thread while input_wait blocks */
    ivr_thread_t t;
    TEST_ASSERT_EQUAL(0, ivr_thread_create(&t, terminal_delay_helper,
                                           &g_session));
    ivr_str_t value;
    ivr_str_init(&value);
    int rc = ivr_session_input_wait(&g_session, 5000, &value);
    TEST_ASSERT_EQUAL(-1, rc);
    ivr_thread_join(&t);
    ivr_str_free(&value);
}

void test_oversized_event_rejected(void) {
    /* max_event_bytes is 4096 in setUp */
    char big[5000];
    memset(big, 'x', sizeof(big));
    big[4999] = '\0';
    ivr_event_view_t ev;
    make_event(&ev, "rtc.failed", big, 1, 1, NULL, NULL);
    TEST_ASSERT_EQUAL(IVR_ENOSPC,
                      ivr_session_submit_event_copy(&g_session, &ev));
}

void test_inbox_byte_budget_rejected(void) {
    /* max_inbox_bytes is 4096 in setUp; each ~1500-byte payload keeps two
       events under the budget and pushes the third past it. */
    char big[1500];
    memset(big, 'x', sizeof(big));
    big[1499] = '\0';
    ivr_event_view_t ev;
    make_event(&ev, "rtc.failed", big, 1, 1, NULL, NULL);
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_session_submit_event_copy(&g_session, &ev));
    make_event(&ev, "rtc.failed", big, 2, 1, NULL, NULL);
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_session_submit_event_copy(&g_session, &ev));
    make_event(&ev, "rtc.failed", big, 3, 1, NULL, NULL);
    TEST_ASSERT_EQUAL(IVR_ENOSPC,
                      ivr_session_submit_event_copy(&g_session, &ev));
}

static void *thread_sleep_helper(void *opaque) {
    ivr_thread_sleep_ms((uint64_t)(uintptr_t)opaque);
    return NULL;
}

void test_thread_timedjoin_timeout(void) {
    ivr_thread_t t;
    TEST_ASSERT_EQUAL(0, ivr_thread_create(&t, thread_sleep_helper,
                                           (void *)(uintptr_t)200));
    int timed_out = 0;
    TEST_ASSERT_EQUAL(-1, ivr_thread_timedjoin(&t, 50, &timed_out));
    TEST_ASSERT_TRUE(timed_out == 1);
    TEST_ASSERT_EQUAL(0, ivr_thread_join(&t));
}

void test_thread_timedjoin_complete(void) {
    ivr_thread_t t;
    TEST_ASSERT_EQUAL(0, ivr_thread_create(&t, thread_sleep_helper,
                                           (void *)(uintptr_t)50));
    int timed_out = 1;
    TEST_ASSERT_EQUAL(0, ivr_thread_timedjoin(&t, 2000, &timed_out));
    TEST_ASSERT_TRUE(timed_out == 0);
}

void test_inbox_full_returns_enospc(void) {
    /* Without starting the control thread the inbox fills up deterministically.
       Capacity is 8; submit 8 then expect ENOSPC on the 9th. */
    ivr_event_view_t ev;
    for (int i = 0; i < 8; i++) {
        make_event(&ev, "rtc.failed", "{}", (uint64_t)(i + 1), 1, NULL, NULL);
        TEST_ASSERT_EQUAL(IVR_OK, ivr_session_submit_event_copy(&g_session, &ev));
    }
    make_event(&ev, "rtc.failed", "{}", 9, 1, NULL, NULL);
    TEST_ASSERT_EQUAL(IVR_ENOSPC, ivr_session_submit_event_copy(&g_session, &ev));
}

spec("test_ivr_session") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_sequence_contiguous_delivered);
  TT_TEST(test_sequence_gap_emits_get_snapshot);
  TT_TEST(test_snapshot_resumes_after_gap);
  TT_TEST(test_duplicate_sequence_dropped);
  TT_TEST(test_command_result_delivered_without_advancing_domain_sequence);
  TT_TEST(test_generation_mismatch_dropped);
  TT_TEST(test_input_first_final_wins);
  TT_TEST(test_stale_input_window_dropped);
  TT_TEST(test_replacing_input_window_ends_previous_first);
  TT_TEST(test_begin_input_hook_failure_closes_local_window);
  TT_TEST(test_end_input_hook_failure_blocks_replacement);
  TT_TEST(test_end_input_hook_failure_fails_wait_cleanup);
  TT_TEST(test_terminal_wakes_input_wait);
  TT_TEST(test_inbox_full_returns_enospc);
  TT_TEST(test_oversized_event_rejected);
  TT_TEST(test_inbox_byte_budget_rejected);
  TT_TEST(test_thread_timedjoin_timeout);
  TT_TEST(test_thread_timedjoin_complete);
}
