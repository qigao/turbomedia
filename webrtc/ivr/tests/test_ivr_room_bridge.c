/* test_ivr_room_bridge.c - FlowMQ DEALER->ROUTER end-to-end.
 * Spins the ivr_room_bridge (ROUTER BIND) + the flowmq DEALER gateway and
 * verifies: command frame round-trip, IvrCommandResultV1 reply, message_id
 * idempotency, and stale expected_room_version rejection. */
#include "ivr_flowmq_gateway.h"
#include "ivr_room_bridge.h"
#include "ivr_frame.h"
#include "ivr_thread.h"
#include "turbomedia_ivr_v1.h"
#include "tinytest_compat.h"
#include <stdatomic.h>
#include <string.h>

#define TEST_PORT 17713

static DataBind *g_codec = NULL;

/* ---- mock RoomService handler ---- */
static uint64_t g_room_version = 0;
static uint64_t g_sequence = 0;
static uint64_t g_applied = 0;
static char g_last_message_id[128] = {0};

static uint64_t mock_get_room_version(void *ctx, const char *room_id) {
    (void)ctx;
    (void)room_id;
    return g_room_version;
}

static ivr_status_t mock_on_command(void *ctx, const ivr_room_command_t *cmd,
                                    ivr_room_command_result_t *result) {
    (void)ctx;
    if (strcmp(cmd->command, "worker.sync") == 0) {
        TEST_ASSERT_EQUAL_STRING("ivr-worker-test", cmd->worker_id);
        result->status_code = 0;
        return IVR_OK;
    }
    TEST_ASSERT_EQUAL_STRING("conference.join", cmd->command);
    TEST_ASSERT_EQUAL_STRING("mid-1", cmd->message_id);
    snprintf(g_last_message_id, sizeof(g_last_message_id), "%s",
             cmd->message_id);
    g_applied++;
    g_room_version++;
    g_sequence++;
    result->status_code = 0;
    result->room_version = g_room_version;
    result->sequence = g_sequence;
    return IVR_OK;
}

static atomic_int g_result_handler_entered;
static atomic_int g_result_handler_release;

static ivr_status_t blocking_on_dispatch_result(
    void *ctx, const ivr_dispatch_result_t *result) {
    (void)ctx;
    (void)result;
    atomic_store(&g_result_handler_entered, 1);
    while (!atomic_load(&g_result_handler_release)) {
        ivr_thread_sleep_ms(5);
    }
    return IVR_OK;
}

/* ---- reply capture on the DEALER ---- */
static ivr_mutex_t g_reply_lock;
static uint8_t g_reply[8192];
static size_t g_reply_len = 0;
static int g_reply_ready = 0;

static void on_reply_cb(void *ctx, const uint8_t *frame, size_t len) {
    (void)ctx;
    ivr_mutex_lock(&g_reply_lock);
    if (len <= sizeof(g_reply)) {
        memcpy(g_reply, frame, len);
        g_reply_len = len;
        g_reply_ready = 1;
    }
    ivr_mutex_unlock(&g_reply_lock);
}

static int wait_reply(int timeout_ms) {
    for (int i = 0; i < timeout_ms / 20; i++) {
        ivr_mutex_lock(&g_reply_lock);
        int ready = g_reply_ready;
        ivr_mutex_unlock(&g_reply_lock);
        if (ready) {
            return 1;
        }
        ivr_thread_sleep_ms(20);
    }
    return 0;
}

/* ---- decoded result helpers ---- */
static uint64_t result_u64(const char *field) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_bin(g_codec, "IvrCommandResultV1",
                                  g_reply + IVR_FRAME_HEADER_SIZE,
                                  g_reply_len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return UINT64_MAX;
    }
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *v = data_bind_value_get(root, field);
    uint64_t out = v ? data_bind_value_as_uint64(v) : UINT64_MAX;
    data_bind_object_free(obj);
    return out;
}

static int result_i32(const char *field) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_bin(g_codec, "IvrCommandResultV1",
                                  g_reply + IVR_FRAME_HEADER_SIZE,
                                  g_reply_len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return INT32_MAX;
    }
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *v = data_bind_value_get(root, field);
    int out = v ? data_bind_value_as_int(v) : INT32_MAX;
    data_bind_object_free(obj);
    return out;
}

static void result_string(const char *field, char *out, size_t out_size) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_bin(g_codec, "IvrCommandResultV1",
                                  g_reply + IVR_FRAME_HEADER_SIZE,
                                  g_reply_len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) == DATA_BIND_OK) {
        const DataBindValue *root = data_bind_object_value(obj);
        const DataBindValue *v = data_bind_value_get(root, field);
        const char *s = v ? data_bind_value_as_string(v) : "";
        snprintf(out, out_size, "%s", s ? s : "");
        data_bind_object_free(obj);
    } else {
        out[0] = '\0';
    }
}

/* ---- scaffolding ---- */
static ivr_room_bridge_t *g_bridge = NULL;
static ivr_flowmq_gateway_t *g_gateway = NULL;
static ivr_command_gateway_ops_t g_ops;

static ivr_status_t send_join(const char *message_id, uint64_t expected_version) {
    static ivr_bytes_view_t type = {"conference.join", 15};
    static ivr_bytes_view_t room = {"room-42", 7};
    static ivr_bytes_view_t call = {"call-42", 7};
    static ivr_bytes_view_t args = {"{}", 2};
    ivr_command_view_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.message_id.data = message_id;
    cmd.message_id.size = strlen(message_id);
    cmd.command_type = type;
    cmd.call.room_id = room;
    cmd.call.call_id = call;
    cmd.call.call_generation = 1;
    cmd.call.expected_room_version = expected_version;
    cmd.args_json = args;
    return g_ops.submit_copy(g_ops.context, &cmd);
}

void setUp(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    TEST_ASSERT_EQUAL(DATA_BIND_OK, TurboMediaIvrV1_codec_create(&g_codec, &err));
    ivr_mutex_init(&g_reply_lock);
    g_reply_ready = 0;
    g_reply_len = 0;
    g_room_version = 10;
    g_sequence = 90;
    g_applied = 0;
    g_last_message_id[0] = '\0';

    ivr_room_command_handler_t handler;
    memset(&handler, 0, sizeof(handler));
    handler.get_room_version = mock_get_room_version;
    handler.on_command = mock_on_command;

    ivr_room_bridge_config_t bcfg;
    memset(&bcfg, 0, sizeof(bcfg));
    bcfg.host = "127.0.0.1";
    bcfg.port = TEST_PORT;
    bcfg.timeout_ms = 5000;
    bcfg.queue_capacity = 8;
    bcfg.dedup_capacity = 8;
    bcfg.handler = handler;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_room_bridge_create(&bcfg, &g_bridge));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_room_bridge_start(g_bridge));

    ivr_flowmq_gateway_config_t gcfg;
    memset(&gcfg, 0, sizeof(gcfg));
    gcfg.worker_id = "ivr-worker-test";
    gcfg.host = "127.0.0.1";
    gcfg.port = TEST_PORT;
    gcfg.timeout_ms = 5000;
    gcfg.on_reply = on_reply_cb;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_create(&gcfg, &g_ops,
                                                        &g_gateway));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_start(g_gateway));
    ivr_thread_sleep_ms(800); /* let the DEALER connect to the ROUTER */
}

void tearDown(void) {
    if (g_gateway) {
        ivr_flowmq_gateway_destroy(g_gateway);
        g_gateway = NULL;
    }
    if (g_bridge) {
        ivr_room_bridge_stop(g_bridge);
        ivr_room_bridge_destroy(g_bridge);
        g_bridge = NULL;
    }
    if (g_codec) {
        data_bind_free(g_codec);
        g_codec = NULL;
    }
    ivr_mutex_destroy(&g_reply_lock);
}

void test_roundtrip_and_reply(void) {
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-1", 10));
    TEST_ASSERT_TRUE(wait_reply(8000));
    char mid[128];
    result_string("message_id", mid, sizeof(mid));
    TEST_ASSERT_EQUAL_STRING("mid-1", mid);
    TEST_ASSERT_EQUAL_UINT64(0u, result_u64("status_code"));
    TEST_ASSERT_EQUAL_UINT64(11u, result_u64("room_version"));
    TEST_ASSERT_EQUAL_UINT64(91u, result_u64("sequence"));
    TEST_ASSERT_EQUAL_UINT64(1u, g_applied);
}

void test_idempotent_retry(void) {
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-1", 10));
    TEST_ASSERT_TRUE(wait_reply(8000));
    /* same message_id retransmitted: must not re-apply */
    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-1", 10));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_UINT64(1u, g_applied); /* handler called once */
    TEST_ASSERT_EQUAL_UINT64(11u, result_u64("room_version"));
}

void test_worker_sync_roundtrip(void) {
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_send_worker_sync(g_gateway, "ws-1"));
    TEST_ASSERT_TRUE(wait_reply(8000));
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_frame_decode(g_reply, g_reply_len, &info));
    TEST_ASSERT_EQUAL_UINT32(IVR_TYPE_WORKER_SYNC_RESULT_V1,
                             info.schema_type_id);
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_bin(g_codec, "WorkerSyncResultV1",
                                                g_reply + IVR_FRAME_HEADER_SIZE,
                                                g_reply_len -
                                                    IVR_FRAME_HEADER_SIZE,
                                                &obj, &err));
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *sc = data_bind_value_get(root, "status_code");
    TEST_ASSERT_NOT_NULL(sc);
    TEST_ASSERT_EQUAL_INT(0, data_bind_value_as_int(sc));
    const DataBindValue *mid = data_bind_value_get(root, "message_id");
    TEST_ASSERT_NOT_NULL(mid);
    TEST_ASSERT_EQUAL_STRING("ws-1", data_bind_value_as_string(mid));
    const DataBindValue *wid = data_bind_value_get(root, "worker_id");
    TEST_ASSERT_NOT_NULL(wid);
    TEST_ASSERT_EQUAL_STRING("ivr-worker-test", data_bind_value_as_string(wid));
    data_bind_object_free(obj);
}

static int worker_sync_status(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_bin(g_codec, "WorkerSyncResultV1",
                                  g_reply + IVR_FRAME_HEADER_SIZE,
                                  g_reply_len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return INT32_MAX;
    }
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *v = data_bind_value_get(root, "status_code");
    int out = v ? data_bind_value_as_int(v) : INT32_MAX;
    data_bind_object_free(obj);
    return out;
}

void test_worker_sync_unconnected_identity_rejected(void) {
    /* registration is bound to a live connection: a DEALER claiming an
       identity that has no connected peer is rejected (IVR_EAUTH) before the
       command handler runs. (Full anti-spoofing needs transport-level mTLS
       identity: the facade exposes no per-message peer identity on ROUTER
       ingress, so a claim that matches ANY live connection is accepted.) */
    ivr_flowmq_gateway_config_t acfg;
    memset(&acfg, 0, sizeof(acfg));
    acfg.worker_id = "attacker-1";
    acfg.host = "127.0.0.1";
    acfg.port = TEST_PORT;
    acfg.timeout_ms = 5000;
    acfg.on_reply = on_reply_cb;
    ivr_command_gateway_ops_t aops;
    ivr_flowmq_gateway_t *attacker = NULL;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_create(&acfg, &aops, &attacker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_start(attacker));
    ivr_thread_sleep_ms(800); /* let the DEALER connect + PEER_CONNECTED fire */

    uint8_t frame[4096];
    size_t len = 0;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_encode_worker_sync(
                          g_codec, "ws-spoof", "ghost-worker", frame,
                          sizeof(frame), &len));
    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_send_frame(attacker, frame, len));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL(IVR_EAUTH, worker_sync_status());

    /* the legitimate worker (connected as its claimed id) still registers */
    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_send_worker_sync(g_gateway, "ws-2"));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL(0, worker_sync_status());

    ivr_flowmq_gateway_destroy(attacker);
}

void test_stale_version_rejected(void) {
    /* expected=9 while the room is at version 10: IVR_EVERSION, no apply */
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-2", 9));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL(IVR_EVERSION, result_i32("status_code"));
    TEST_ASSERT_EQUAL_UINT64(0u, g_applied);
}

void test_double_start_rejected(void) {
    /* repeated start() must not spawn a second worker thread over the same
       queue/thread handle */
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_room_bridge_start(g_bridge));
    /* the running bridge still serves commands after the rejected start */
    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(IVR_OK, send_join("mid-9", 10));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_UINT64(0u, result_u64("status_code"));
}

void test_restart_after_stop(void) {
    /* stop() must be reversible: a stopped bridge can start again and the
       queue latch from the previous stop must not kill the new thread. (The
       DEALER gateway reconnect after a ROUTER restart is the gateway's own
       concern, so no round-trip is asserted here.) */
    ivr_room_bridge_stop(g_bridge);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_room_bridge_start(g_bridge));
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_room_bridge_start(g_bridge));
    /* stop is idempotent and restart works again */
    ivr_room_bridge_stop(g_bridge);
    ivr_room_bridge_stop(g_bridge);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_room_bridge_start(g_bridge));
}

void test_destroy_without_stop(void) {
    /* destroying a started bridge must stop/join the worker thread and the
       FMQ apps before freeing queue/lock/codec/bridge (ASan catches any UAF
       from the still-running worker thread or FlowMQ callbacks) */
    ivr_room_bridge_config_t bcfg;
    memset(&bcfg, 0, sizeof(bcfg));
    bcfg.host = "127.0.0.1";
    bcfg.port = TEST_PORT + 1;
    bcfg.timeout_ms = 5000;
    bcfg.queue_capacity = 4;
    bcfg.dedup_capacity = 4;
    ivr_room_bridge_t *b2 = NULL;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_room_bridge_create(&bcfg, &b2));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_room_bridge_start(b2));
    ivr_room_bridge_destroy(b2);
}

void test_dispatch_result_queue_overflow_is_counted_and_fail_closed(void) {
    ivr_room_bridge_t *bridge = NULL;
    ivr_flowmq_gateway_t *gateway = NULL;
    ivr_command_gateway_ops_t unused_ops;
    ivr_room_command_handler_t handler;
    ivr_room_bridge_config_t bridge_config;
    ivr_flowmq_gateway_config_t gateway_config;
    memset(&unused_ops, 0, sizeof(unused_ops));
    memset(&handler, 0, sizeof(handler));
    memset(&bridge_config, 0, sizeof(bridge_config));
    memset(&gateway_config, 0, sizeof(gateway_config));
    atomic_store(&g_result_handler_entered, 0);
    atomic_store(&g_result_handler_release, 0);

    handler.get_room_version = mock_get_room_version;
    handler.on_command = mock_on_command;
    handler.on_dispatch_result = blocking_on_dispatch_result;
    bridge_config.host = "127.0.0.1";
    bridge_config.port = TEST_PORT + 2;
    bridge_config.timeout_ms = 5000;
    bridge_config.queue_capacity = 1;
    bridge_config.dedup_capacity = 4;
    bridge_config.handler = handler;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_room_bridge_create(&bridge_config, &bridge));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_room_bridge_start(bridge));

    gateway_config.worker_id = "ivr-worker-test";
    gateway_config.host = "127.0.0.1";
    gateway_config.port = TEST_PORT + 2;
    gateway_config.timeout_ms = 5000;
    gateway_config.on_reply = on_reply_cb;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_create(
                                  &gateway_config, &unused_ops, &gateway));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_start(gateway));
    ivr_thread_sleep_ms(800);
    g_reply_ready = 0;
    TEST_ASSERT_EQUAL(
        IVR_OK, ivr_flowmq_gateway_send_worker_sync(gateway, "overflow-sync"));
    TEST_ASSERT_TRUE(wait_reply(8000));

    ivr_call_dispatch_t dispatch;
    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.wire_version = 1;
    snprintf(dispatch.worker_id, sizeof(dispatch.worker_id),
             "ivr-worker-test");
    snprintf(dispatch.room_id, sizeof(dispatch.room_id), "room-42");
    snprintf(dispatch.call_id, sizeof(dispatch.call_id), "call-overflow");
    dispatch.call_generation = 1;
    snprintf(dispatch.message_id, sizeof(dispatch.message_id), "result-0");
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_dispatch_result(
                                  gateway, &dispatch, IVR_OK, "", ""));
    for (int i = 0; i < 400 && !atomic_load(&g_result_handler_entered); i++) {
        ivr_thread_sleep_ms(5);
    }

    int send_failures = 0;
    for (int i = 1; i <= 32; i++) {
        snprintf(dispatch.message_id, sizeof(dispatch.message_id),
                 "result-%d", i);
        if (ivr_flowmq_gateway_send_dispatch_result(
                gateway, &dispatch, IVR_OK, "", "") != IVR_OK) {
            send_failures++;
        }
    }
    ivr_room_bridge_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    for (int i = 0; i < 400; i++) {
        ivr_room_bridge_get_stats(bridge, &stats);
        if (stats.request_queue_drops > 0) {
            break;
        }
        ivr_thread_sleep_ms(5);
    }
    atomic_store(&g_result_handler_release, 1);
    ivr_thread_sleep_ms(100);
    ivr_flowmq_gateway_destroy(gateway);
    ivr_room_bridge_destroy(bridge);

    TEST_ASSERT_TRUE(atomic_load(&g_result_handler_entered));
    TEST_ASSERT_EQUAL_INT(0, send_failures);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.request_queue_capacity);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.request_queue_high_water);
    TEST_ASSERT_TRUE(stats.request_queue_drops > 0);
}

static void decode_str(const char *type_name, const uint8_t *frame,
                       size_t len, const char *field, char *out,
                       size_t out_size) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    out[0] = '\0';
    if (data_bind_object_from_bin(g_codec, type_name,
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return;
    }
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *v = data_bind_value_get(root, field);
    const char *s = v ? data_bind_value_as_string(v) : "";
    snprintf(out, out_size, "%s", s ? s : "");
    data_bind_object_free(obj);
}

void test_encode_dispatch_escapes_special_chars(void) {
    static const char mid[] = "mid\"\\\n1";
    static const char wid[] = "worker\"\\\n1";
    static const char pkg[] = "pkg\\\"\t1";
    uint8_t frame[8192];
    size_t len = 0;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_room_bridge_encode_dispatch(
                          g_codec, mid, wid, "room-42", "call-42", 1, 2, pkg,
                          frame, sizeof(frame), &len));
    char out[128];
    decode_str("CallDispatchCommandV1", frame, len, "message_id", out,
               sizeof(out));
    TEST_ASSERT_EQUAL_STRING(mid, out);
    decode_str("CallDispatchCommandV1", frame, len, "worker_id", out,
               sizeof(out));
    TEST_ASSERT_EQUAL_STRING(wid, out);
    decode_str("CallDispatchCommandV1", frame, len, "content_package", out,
               sizeof(out));
    TEST_ASSERT_EQUAL_STRING(pkg, out);
}

void test_encode_result_escapes_special_chars(void) {
    static const char mid[] = "mid\"\\\n1";
    ivr_room_command_result_t result;
    memset(&result, 0, sizeof(result));
    result.status_code = IVR_ESTATE;
    snprintf(result.error_message, sizeof(result.error_message),
             "err\"\\\n1");
    uint8_t frame[8192];
    size_t len = 0;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_room_encode_result(g_codec, mid, "worker-1",
                                             "room-1", "call-1", 1, &result,
                                             frame, sizeof(frame), &len));
    char out[128];
    decode_str("IvrCommandResultV1", frame, len, "message_id", out,
               sizeof(out));
    TEST_ASSERT_EQUAL_STRING(mid, out);
    decode_str("IvrCommandResultV1", frame, len, "error_message", out,
               sizeof(out));
    TEST_ASSERT_EQUAL_STRING(result.error_message, out);
}

void test_encode_participant_joined_escapes_special_chars(void) {
    static const char event_id[] = "ev\"\\\n1";
    static const char role[] = "caller\"\\\n1";
    uint8_t frame[8192];
    size_t len = 0;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_room_bridge_encode_participant_joined(
                          g_codec, event_id, "cause-1", "worker-1", "room-1",
                          "call-1", 1, 2, 3, 4, "p-1", role, frame,
                          sizeof(frame), &len));
    char out[128];
    decode_str("ConferenceParticipantJoinedEventV1", frame, len, "event_id",
               out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(event_id, out);
    decode_str("ConferenceParticipantJoinedEventV1", frame, len,
               "participant_role", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(role, out);
}

spec("test_ivr_room_bridge") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_roundtrip_and_reply);
  TT_TEST(test_idempotent_retry);
  TT_TEST(test_worker_sync_roundtrip);
  TT_TEST(test_worker_sync_unconnected_identity_rejected);
  TT_TEST(test_stale_version_rejected);
  TT_TEST(test_double_start_rejected);
  TT_TEST(test_restart_after_stop);
  TT_TEST(test_destroy_without_stop);
  TT_TEST(test_dispatch_result_queue_overflow_is_counted_and_fail_closed);
  TT_TEST(test_encode_dispatch_escapes_special_chars);
  TT_TEST(test_encode_result_escapes_special_chars);
  TT_TEST(test_encode_participant_joined_escapes_special_chars);
}
