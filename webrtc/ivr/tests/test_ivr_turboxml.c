/* test_ivr_turboxml.c - TurboXML-driven conference scenarios (Gate C subset)
 *
 * Verified TurboXML build behavior (see ivr_turboxml_adapter.c header):
 * CCXML + the built-in CCXML<->VXML dialog bridge work; SCXML <send> capture
 * is not supported by this build, so RTC control runs as an explicit adapter
 * state machine mirroring rtc_session.scxml.
 */
#include "ivr_session.h"
#include "ivr_turboxml_adapter.h"
#include "ivr_content.h"
#include "ivr_thread.h"
#include "tinytest_compat.h"
#include <stdlib.h>
#include <string.h>

#ifndef IVR_TEST_CONTENT_ROOT
#define IVR_TEST_CONTENT_ROOT "content"
#endif

/* ------------------------------------------------------------------ */
/* mocks                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    ivr_mutex_t lock;
    char types[32][64];
    char args[32][256];
    size_t count;
} cmd_log_t;

static ivr_status_t mock_gateway_submit(void *ctx, const ivr_command_view_t *cmd) {
    cmd_log_t *log = (cmd_log_t *)ctx;
    ivr_mutex_lock(&log->lock);
    if (log->count < 32) {
        size_t n = cmd->command_type.size < 63 ? cmd->command_type.size : 63;
        memcpy(log->types[log->count], cmd->command_type.data, n);
        log->types[log->count][n] = '\0';
        n = cmd->args_json.size < 255 ? cmd->args_json.size : 255;
        memcpy(log->args[log->count], cmd->args_json.data, n);
        log->args[log->count][n] = '\0';
        log->count++;
    }
    ivr_mutex_unlock(&log->lock);
    return IVR_OK;
}

typedef struct {
    ivr_mutex_t lock;
    char prompts[8][256];
    size_t prompt_count;
    int cancel_input_calls;
    int stop_bot_calls;
} media_log_t;

static ivr_status_t mock_start_bot(void *ctx, const ivr_call_ref_t *call) {
    (void)ctx;
    (void)call;
    return IVR_OK;
}
static ivr_status_t mock_play_pcm(void *ctx, const ivr_call_ref_t *call,
                                  const ivr_bytes_view_t *text) {
    media_log_t *m = (media_log_t *)ctx;
    ivr_mutex_lock(&m->lock);
    if (m->prompt_count < 8 && text) {
        size_t n = text->size < 255 ? text->size : 255;
        memcpy(m->prompts[m->prompt_count], text->data, n);
        m->prompts[m->prompt_count][n] = '\0';
        m->prompt_count++;
    }
    ivr_mutex_unlock(&m->lock);
    (void)call;
    return IVR_OK;
}
static ivr_status_t mock_cancel_input(void *ctx, const ivr_call_ref_t *call) {
    media_log_t *m = (media_log_t *)ctx;
    ivr_mutex_lock(&m->lock);
    m->cancel_input_calls++;
    ivr_mutex_unlock(&m->lock);
    (void)call;
    return IVR_OK;
}
static ivr_status_t mock_stop_bot(void *ctx, const ivr_call_ref_t *call) {
    media_log_t *m = (media_log_t *)ctx;
    ivr_mutex_lock(&m->lock);
    m->stop_bot_calls++;
    ivr_mutex_unlock(&m->lock);
    (void)call;
    return IVR_OK;
}
static ivr_status_t mock_begin_input(void *ctx, const ivr_call_ref_t *call,
                                     const ivr_bytes_view_t *input_id,
                                     uint64_t input_generation) {
    (void)ctx; (void)call; (void)input_id; (void)input_generation;
    return IVR_OK;
}
static ivr_status_t mock_end_input(void *ctx, const ivr_call_ref_t *call,
                                   const ivr_bytes_view_t *input_id,
                                   uint64_t input_generation) {
    (void)ctx; (void)call; (void)input_id; (void)input_generation;
    return IVR_OK;
}

/* ------------------------------------------------------------------ */
/* scaffolding                                                         */
/* ------------------------------------------------------------------ */

static cmd_log_t g_cmd;
static media_log_t g_media;
static ivr_command_gateway_ops_t g_gateway_ops;
static ivr_media_port_ops_t g_media_ops;
static ivr_call_ref_t g_call;
static ivr_session_t g_session;
static ivr_content_package_t g_content;

static int wait_cmd_count(size_t min_count, int timeout_ms) {
    for (int i = 0; i < timeout_ms / 10; i++) {
        ivr_mutex_lock(&g_cmd.lock);
        size_t n = g_cmd.count;
        ivr_mutex_unlock(&g_cmd.lock);
        if (n >= min_count) {
            return 1;
        }
        ivr_thread_sleep_ms(10);
    }
    return 0;
}

static const char *cmd_type_at(size_t i) {
    static char out[64];
    ivr_mutex_lock(&g_cmd.lock);
    snprintf(out, sizeof(out), "%s",
             i < g_cmd.count ? g_cmd.types[i] : "");
    ivr_mutex_unlock(&g_cmd.lock);
    return out;
}

static const char *cmd_args_at(size_t i) {
    static char out[256];
    ivr_mutex_lock(&g_cmd.lock);
    snprintf(out, sizeof(out), "%s",
             i < g_cmd.count ? g_cmd.args[i] : "");
    ivr_mutex_unlock(&g_cmd.lock);
    return out;
}

static int wait_prompt(int timeout_ms) {
    for (int i = 0; i < timeout_ms / 10; i++) {
        ivr_mutex_lock(&g_media.lock);
        size_t n = g_media.prompt_count;
        ivr_mutex_unlock(&g_media.lock);
        if (n >= 1) {
            return 1;
        }
        ivr_thread_sleep_ms(10);
    }
    return 0;
}

static int wait_input_waiting(int timeout_ms) {
    for (int i = 0; i < timeout_ms / 10; i++) {
        if (g_session.input_waiting) {
            return 1;
        }
        ivr_thread_sleep_ms(10);
    }
    return 0;
}

static int wait_media_cancel(int min_count, int timeout_ms) {
    for (int i = 0; i < timeout_ms / 10; i++) {
        ivr_mutex_lock(&g_media.lock);
        int n = g_media.cancel_input_calls;
        ivr_mutex_unlock(&g_media.lock);
        if (n >= min_count) {
            return 1;
        }
        ivr_thread_sleep_ms(10);
    }
    return 0;
}

static int media_cancel_count(void) {
    ivr_mutex_lock(&g_media.lock);
    int n = g_media.cancel_input_calls;
    ivr_mutex_unlock(&g_media.lock);
    return n;
}

static int media_stop_count(void) {
    ivr_mutex_lock(&g_media.lock);
    int n = g_media.stop_bot_calls;
    ivr_mutex_unlock(&g_media.lock);
    return n;
}

static void submit_state(const char *type, uint64_t seq) {
    static ivr_bytes_view_t ev_id = {"ev", 2};
    static ivr_bytes_view_t payload = {"{}", 2};
    ivr_event_view_t view;
    memset(&view, 0, sizeof(view));
    view.event_id = ev_id;
    view.event_type.data = type;
    view.event_type.size = strlen(type);
    view.payload_json = payload;
    view.call = g_call;
    view.sequence = seq;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_submit_event_copy(&g_session, &view));
}

static void submit_input(const char *type, const char *window,
                         const char *value) {
    static ivr_bytes_view_t ev_id = {"ev", 2};
    static ivr_bytes_view_t payload = {"{}", 2};
    ivr_event_view_t view;
    memset(&view, 0, sizeof(view));
    view.event_id = ev_id;
    view.event_type.data = type;
    view.event_type.size = strlen(type);
    view.payload_json = payload;
    view.call = g_call;
    view.sequence = 0;
    view.input_id.data = window;
    view.input_id.size = window ? strlen(window) : 0;
    view.input_value.data = value;
    view.input_value.size = value ? strlen(value) : 0;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_submit_event_copy(&g_session, &view));
}

void setUp(void) {
    memset(&g_cmd, 0, sizeof(g_cmd));
    memset(&g_media, 0, sizeof(g_media));
    ivr_mutex_init(&g_cmd.lock);
    ivr_mutex_init(&g_media.lock);

    g_gateway_ops.abi_version = 1;
    g_gateway_ops.context = &g_cmd;
    g_gateway_ops.submit_copy = mock_gateway_submit;

    g_media_ops.abi_version = IVR_WORKER_ABI_VERSION;
    g_media_ops.context = &g_media;
    g_media_ops.start_bot = mock_start_bot;
    g_media_ops.play_pcm = mock_play_pcm;
    g_media_ops.cancel_input = mock_cancel_input;
    g_media_ops.stop_bot = mock_stop_bot;
    g_media_ops.begin_input = mock_begin_input;
    g_media_ops.end_input = mock_end_input;

    g_call.room_id.data = "room-42";
    g_call.room_id.size = 7;
    g_call.call_id.data = "call-42";
    g_call.call_id.size = 7;
    g_call.call_generation = 1;
    g_call.expected_room_version = 10;

    int rc = ivr_content_package_load(IVR_TEST_CONTENT_ROOT,
                                      "conference-greeting", &g_content);
    TEST_ASSERT_EQUAL(IVR_OK, rc);

    rc = ivr_session_create(&g_session, &g_call, 16, 64 * 1024,
                            64 * 1024 * 16, &ivr_turboxml_engine_ops,
                            &g_content, &g_gateway_ops, &g_media_ops);
    TEST_ASSERT_EQUAL(IVR_OK, rc);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_session_start(&g_session));
}

void tearDown(void) {
    ivr_session_request_terminal(&g_session);
    ivr_session_join(&g_session);
    ivr_session_free(&g_session);
    ivr_content_package_free(&g_content);
    ivr_mutex_destroy(&g_cmd.lock);
    ivr_mutex_destroy(&g_media.lock);
}

/* ------------------------------------------------------------------ */
/* scenarios                                                           */
/* ------------------------------------------------------------------ */

void test_conference_happy_path(void) {
    submit_state("room.assigned", 1);
    TEST_ASSERT_TRUE(wait_cmd_count(1, 5000));
    TEST_ASSERT_EQUAL_STRING("rtc.join", cmd_type_at(0));

    submit_state("rtc.connected", 2);
    ivr_thread_sleep_ms(50);

    submit_state("connection.alerting", 3);
    TEST_ASSERT_TRUE(wait_prompt(5000));
    TEST_ASSERT_TRUE(wait_input_waiting(5000));

    /* caller presses 1: first final wins -> conference.join, prompt cancelled.
       Command order: rtc.join (idle->joining), accept (CCXML accept), then
       conference.join (dialog input mapped via command_map). */
    submit_input("dtmf.final", "w1", "1");
    TEST_ASSERT_TRUE(wait_cmd_count(3, 5000));
    TEST_ASSERT_EQUAL_STRING("rtc.join", cmd_type_at(0));
    TEST_ASSERT_EQUAL_STRING("accept", cmd_type_at(1));
    TEST_ASSERT_EQUAL_STRING("conference.join", cmd_type_at(2));
    TEST_ASSERT_TRUE(wait_media_cancel(1, 5000));
}

void test_barge_in_cancels_prompt(void) {
    submit_state("room.assigned", 1);
    TEST_ASSERT_TRUE(wait_cmd_count(1, 5000));
    submit_state("rtc.connected", 2);
    ivr_thread_sleep_ms(50);
    submit_state("connection.alerting", 3);
    TEST_ASSERT_TRUE(wait_prompt(5000));
    TEST_ASSERT_TRUE(wait_input_waiting(5000));
    submit_input("dtmf.final", "w1", "1");
    TEST_ASSERT_TRUE(wait_cmd_count(3, 5000));
    TEST_ASSERT_EQUAL_STRING("conference.join", cmd_type_at(2));
    /* barge-in: the prompt must be cancelled when the final lands */
    TEST_ASSERT_TRUE(wait_media_cancel(1, 5000));
}

void test_noinput_closes_window(void) {
    submit_state("room.assigned", 1);
    TEST_ASSERT_TRUE(wait_cmd_count(1, 5000));
    submit_state("rtc.connected", 2);
    ivr_thread_sleep_ms(50);
    submit_state("connection.alerting", 3);
    TEST_ASSERT_TRUE(wait_input_waiting(5000));
    submit_input("input.timeout", "w1", NULL);
    /* noinput: ASR window closes, no business command is emitted */
    TEST_ASSERT_TRUE(wait_media_cancel(1, 5000));
    TEST_ASSERT_EQUAL_STRING("rtc.join", cmd_type_at(0));
    TEST_ASSERT_EQUAL_STRING("accept", cmd_type_at(1));
    ivr_thread_sleep_ms(100);
    ivr_mutex_lock(&g_cmd.lock);
    size_t cmd_count = g_cmd.count;
    ivr_mutex_unlock(&g_cmd.lock);
    TEST_ASSERT_EQUAL_UINT64(2u, (uint64_t)cmd_count);
}

void test_terminal_during_input(void) {
    submit_state("room.assigned", 1);
    TEST_ASSERT_TRUE(wait_cmd_count(1, 5000));
    submit_state("rtc.connected", 2);
    ivr_thread_sleep_ms(50);
    submit_state("connection.alerting", 3);
    TEST_ASSERT_TRUE(wait_input_waiting(5000));
    /* worker shutdown while the dialog is blocked in collect_input */
    ivr_session_request_terminal(&g_session);
    TEST_ASSERT_TRUE(wait_cmd_count(1, 5000));
    for (int i = 0; i < 500 && media_stop_count() < 1; i++) {
        ivr_thread_sleep_ms(10);
    }
    TEST_ASSERT_TRUE(media_stop_count() >= 1);
}

void test_rtc_reconnect_flow(void) {
    submit_state("room.assigned", 1);
    TEST_ASSERT_TRUE(wait_cmd_count(1, 5000));
    TEST_ASSERT_EQUAL_STRING("rtc.join", cmd_type_at(0));
    /* the send <param name="role" expr="'ivr-bot'"/> becomes command args */
    TEST_ASSERT_EQUAL_STRING("{\"role\":\"ivr-bot\"}", cmd_args_at(0));
    submit_state("rtc.connected", 2);
    ivr_thread_sleep_ms(50);
    submit_state("rtc.disconnected", 3);
    TEST_ASSERT_TRUE(wait_cmd_count(2, 5000));
    TEST_ASSERT_EQUAL_STRING("rtc.reconnect", cmd_type_at(1));
    TEST_ASSERT_EQUAL_STRING("{}", cmd_args_at(1));
    submit_state("rtc.reconnected", 4);
    ivr_thread_sleep_ms(50);
    submit_state("call.terminal", 5);
    TEST_ASSERT_TRUE(wait_cmd_count(3, 5000));
    TEST_ASSERT_EQUAL_STRING("rtc.close", cmd_type_at(2));
    submit_state("rtc.closed", 6);
    ivr_thread_sleep_ms(50);
}

void test_media_input_stall_enters_reconnect_flow(void) {
    submit_state("room.assigned", 1);
    TEST_ASSERT_TRUE(wait_cmd_count(1, 5000));
    submit_state("rtc.connected", 2);
    ivr_thread_sleep_ms(50);
    submit_state("media.input_stalled", 0);
    TEST_ASSERT_TRUE(wait_cmd_count(2, 5000));
    TEST_ASSERT_EQUAL_STRING("rtc.reconnect", cmd_type_at(1));
}

void test_provider_error_closes_only_current_call(void) {
    submit_state("room.assigned", 1);
    TEST_ASSERT_TRUE(wait_cmd_count(1, 5000));
    submit_state("rtc.connected", 2);
    ivr_thread_sleep_ms(50);
    submit_state("provider.error", 0);
    TEST_ASSERT_TRUE(wait_cmd_count(2, 5000));
    TEST_ASSERT_EQUAL_STRING("rtc.close", cmd_type_at(1));
}

void test_snapshot_recovery_after_gap(void) {
    submit_state("room.assigned", 1);
    TEST_ASSERT_TRUE(wait_cmd_count(1, 5000));
    /* gap: 2 is missing, 3 arrives -> must not advance; get_snapshot */
    submit_state("rtc.connected", 3);
    TEST_ASSERT_TRUE(wait_cmd_count(2, 5000));
    TEST_ASSERT_EQUAL_STRING("get_snapshot", cmd_type_at(1));
    /* authoritative snapshot lands at sequence 3 */
    submit_state("room.snapshot.loaded", 3);
    for (int i = 0; i < 500 && g_session.awaiting_snapshot; i++) {
        ivr_thread_sleep_ms(10);
    }
    TEST_ASSERT_EQUAL(0, g_session.awaiting_snapshot);
}

spec("test_ivr_turboxml") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_conference_happy_path);
  TT_TEST(test_barge_in_cancels_prompt);
  TT_TEST(test_noinput_closes_window);
  TT_TEST(test_terminal_during_input);
  TT_TEST(test_rtc_reconnect_flow);
  TT_TEST(test_media_input_stall_enters_reconnect_flow);
  TT_TEST(test_provider_error_closes_only_current_call);
  TT_TEST(test_snapshot_recovery_after_gap);
}
