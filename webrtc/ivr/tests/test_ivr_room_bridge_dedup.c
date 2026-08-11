/* test_ivr_room_bridge_dedup.c - replay/dedup retention window (P0-04.6).
 * Verifies that a message_id replay inside the retention window returns the
 * cached result, and a replay outside the window is explicitly rejected with
 * IVR_ESTALE instead of being silently re-applied. */
#include "ivr_flowmq_gateway.h"
#include "ivr_room_bridge.h"
#include "ivr_frame.h"
#include "ivr_thread.h"
#include "turbomedia_ivr_v1.h"
#include "tinytest_compat.h"
#include <string.h>

#define TEST_PORT 17813

static DataBind *g_codec = NULL;
static uint64_t g_fake_now_ms = 1000u;
static uint64_t g_applied = 0;
static uint64_t g_room_version = 0;

static uint64_t fake_now(void *ctx) {
    (void)ctx;
    return g_fake_now_ms;
}

static uint64_t get_room_version(void *ctx, const char *room_id) {
    (void)ctx;
    (void)room_id;
    return g_room_version;
}

static ivr_status_t on_command(void *ctx, const ivr_room_command_t *cmd,
                               ivr_room_command_result_t *result) {
    (void)ctx;
    (void)cmd;
    g_applied++;
    g_room_version++;
    result->status_code = 0;
    result->room_version = g_room_version;
    result->sequence = g_applied;
    return IVR_OK;
}

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

static int reply_i32(const char *field) {
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


static ivr_command_gateway_ops_t g_ops;

static void send_join(const char *message_id, uint64_t expected_version) {
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
    ivr_mutex_lock(&g_reply_lock);
    g_reply_ready = 0;
    ivr_mutex_unlock(&g_reply_lock);
    TEST_ASSERT_EQUAL(IVR_OK, g_ops.submit_copy(g_ops.context, &cmd));
}


void test_replay_within_window_is_idempotent(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    TEST_ASSERT_EQUAL(DATA_BIND_OK, TurboMediaIvrV1_codec_create(&g_codec, &err));
    ivr_mutex_init(&g_reply_lock);
    g_reply_ready = 0;
    g_reply_len = 0;
    g_applied = 0;
    g_room_version = 0;
    g_fake_now_ms = 1000u;

    ivr_room_command_handler_t handler;
    memset(&handler, 0, sizeof(handler));
    handler.get_room_version = get_room_version;
    handler.on_command = on_command;

    ivr_room_bridge_config_t bcfg;
    memset(&bcfg, 0, sizeof(bcfg));
    bcfg.host = "127.0.0.1";
    bcfg.port = TEST_PORT;
    bcfg.timeout_ms = 5000;
    bcfg.queue_capacity = 8;
    bcfg.dedup_capacity = 8;
    bcfg.dedup_retention_ms = 500;
    bcfg.now_ms = fake_now;
    bcfg.handler = handler;
    ivr_room_bridge_t *bridge = NULL;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_room_bridge_create(&bcfg, &bridge));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_room_bridge_start(bridge));

    ivr_flowmq_gateway_config_t gcfg;
    memset(&gcfg, 0, sizeof(gcfg));
    gcfg.worker_id = "ivr-worker-dedup";
    gcfg.host = "127.0.0.1";
    gcfg.port = TEST_PORT;
    gcfg.timeout_ms = 5000;
    gcfg.on_reply = on_reply_cb;
    ivr_flowmq_gateway_t *gateway = NULL;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_create(&gcfg, &g_ops,
                                                        &gateway));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_start(gateway));
    ivr_thread_sleep_ms(800); /* let the DEALER connect to the ROUTER */

    /* first send applies the mutation */
    send_join("mid-ret-1", 0);
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_UINT64(1u, g_applied);
    TEST_ASSERT_EQUAL_INT(0, reply_i32("status_code"));

    /* replay inside the retention window returns the cached result */
    g_fake_now_ms += 100u;
    send_join("mid-ret-1", 0);
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_UINT64(1u, g_applied);
    TEST_ASSERT_EQUAL_INT(0, reply_i32("status_code"));

    /* replay after the retention window is explicitly rejected */
    g_fake_now_ms += 1000u;
    send_join("mid-ret-1", 0);
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_UINT64(1u, g_applied);
    TEST_ASSERT_EQUAL_INT(IVR_ESTALE, reply_i32("status_code"));

    /* a fresh message_id after the window is applied normally */
    send_join("mid-ret-2", 0);
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_UINT64(2u, g_applied);
    TEST_ASSERT_EQUAL_INT(0, reply_i32("status_code"));

    ivr_flowmq_gateway_destroy(gateway);
    ivr_room_bridge_stop(bridge);
    ivr_room_bridge_destroy(bridge);
    data_bind_free(g_codec);
    g_codec = NULL;
    ivr_mutex_destroy(&g_reply_lock);
}

spec("test_ivr_room_bridge_dedup") {
  TT_TEST(test_replay_within_window_is_idempotent);
}