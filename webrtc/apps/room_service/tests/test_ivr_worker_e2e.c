/* test_ivr_worker_e2e.c - Full IVR loop over the real RoomService aggregate
 * including call dispatch:
 *
 *   client DEALER --conference.join--> RoomService adapter/bridge
 *        -> add_participant (aggregate)
 *        -> bridge pushes CallDispatchCommandV1 to the registered worker
 *           (route captured at worker.sync)
 *        -> worker assigns a session
 *        -> bridge publishes room.participant.joined -> SUB subscriber
 *        -> ivr_worker_submit_event_copy -> the new per-call session.
 *
 * Uses the real turbo_room_service_t aggregate, the real ivr_fmq_adapter
 * (ROUTER + PUB), real FlowMQ DEALER gateways and SUB subscriber, and the
 * real ivr_worker/session (TurboXML) with a shadow command gateway. */
#include "ivr/ivr_worker.h"
#include "ivr_flowmq_gateway.h"
#include "ivr_flowmq_subscriber.h"
#include "ivr_fmq_adapter.h"
#include "ivr_frame.h"
#include "ivr_session.h"
#include "ivr_thread.h"
#include "turbomedia_ivr_v1.h"
#include "tinytest_compat.h"
#include <string.h>

#ifndef IVR_TEST_CONTENT_ROOT
#define IVR_TEST_CONTENT_ROOT "content"
#endif

#define TEST_ROUTER_PORT 17716
#define TEST_PUB_PORT 17717

static DataBind *g_codec = NULL;

/* ---- client DEALER reply capture (conference.join result) ---- */
static ivr_mutex_t g_reply_lock;
static uint8_t g_reply[8192];
static size_t g_reply_len = 0;
static int g_reply_ready = 0;

static void on_client_reply(void *ctx, const uint8_t *frame, size_t len) {
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

static int reply_status(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_bin(g_codec, "IvrCommandResultV1",
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

/* ---- worker DEALER: worker.sync ack + dispatch -> assign ---- */
static volatile int g_worker_synced = 0;
static ivr_session_t *g_worker_session = NULL;
static ivr_worker_t *g_worker; /* forward decl; defined in components */
static ivr_flowmq_gateway_t *g_worker_gw;
static ivr_flowmq_gateway_t *g_worker_gw_b;
static ivr_mutex_t g_dispatch_lock;
static ivr_call_dispatch_t g_pending_dispatch;
static int g_dispatch_pending = 0;
static ivr_call_release_t g_pending_release;
static int g_release_pending = 0;
static int g_drop_release_result_once = 0;
static int g_force_dispatch_reject = 0;
static int g_corrupt_dispatch_result_generation = 0;

static int take_pending_dispatch(ivr_call_dispatch_t *out) {
    int found;
    ivr_mutex_lock(&g_dispatch_lock);
    found = g_dispatch_pending;
    if (found) {
        *out = g_pending_dispatch;
        g_dispatch_pending = 0;
    }
    ivr_mutex_unlock(&g_dispatch_lock);
    return found;
}

static int take_pending_release(ivr_call_release_t *out) {
    int found;
    ivr_mutex_lock(&g_dispatch_lock);
    found = g_release_pending;
    if (found) {
        *out = g_pending_release;
        g_release_pending = 0;
    }
    ivr_mutex_unlock(&g_dispatch_lock);
    return found;
}

static void on_worker_reply(void *ctx, const uint8_t *frame, size_t len) {
    (void)ctx;
    ivr_frame_info_t info;
    if (ivr_frame_decode(frame, len, &info) != IVR_OK) {
        return;
    }
    if (info.kind == IVR_KIND_COMMAND &&
        (info.schema_type_id == IVR_TYPE_CALL_DISPATCH_COMMAND_V1 ||
         info.schema_type_id == IVR_TYPE_CALL_DISPATCH_COMMAND_V2)) {
        ivr_call_dispatch_t dispatch;
        if (ivr_flowmq_gateway_decode_dispatch(g_codec, frame, len,
                                               &dispatch) != IVR_OK) {
            return;
        }
        ivr_mutex_lock(&g_dispatch_lock);
        if (!g_dispatch_pending) {
            g_pending_dispatch = dispatch;
            g_dispatch_pending = 1;
        }
        ivr_mutex_unlock(&g_dispatch_lock);
        return;
    }
    if (info.kind == IVR_KIND_COMMAND &&
        info.schema_type_id == IVR_TYPE_CALL_RELEASE_COMMAND_V1) {
        ivr_call_release_t release;
        if (ivr_flowmq_gateway_decode_release(g_codec, frame, len, &release) !=
            IVR_OK) {
            return;
        }
        ivr_mutex_lock(&g_dispatch_lock);
        if (!g_release_pending) {
            g_pending_release = release;
            g_release_pending = 1;
        }
        ivr_mutex_unlock(&g_dispatch_lock);
        return;
    }
    if (info.schema_type_id == IVR_TYPE_WORKER_SYNC_RESULT_V1) {
        g_worker_synced = 1;
    }
}

/* Main-loop equivalent: the FlowMQ callback above only copies the frame. */
static void process_worker_dispatch(void) {
    ivr_call_dispatch_t dispatch;
    ivr_call_release_t release;
    int has_dispatch;
    int has_release;
    ivr_mutex_lock(&g_dispatch_lock);
    has_dispatch = g_dispatch_pending;
    if (has_dispatch) {
        dispatch = g_pending_dispatch;
        g_dispatch_pending = 0;
    }
    has_release = g_release_pending;
    if (has_release) {
        release = g_pending_release;
        g_release_pending = 0;
    }
    ivr_mutex_unlock(&g_dispatch_lock);

    if (has_dispatch) {
        ivr_call_ref_t call;
        memset(&call, 0, sizeof(call));
        call.room_id.data = dispatch.room_id;
        call.room_id.size = strlen(dispatch.room_id);
        call.call_id.data = dispatch.call_id;
        call.call_id.size = strlen(dispatch.call_id);
        call.call_generation = dispatch.call_generation;
        call.expected_room_version = dispatch.expected_room_version;
        ivr_status_t rc = IVR_OK;
        if (g_force_dispatch_reject > 0) {
            rc = IVR_ENOSPC;
            g_force_dispatch_reject--;
        } else if (!ivr_worker_has_session(g_worker, &call)) {
            rc = ivr_worker_assign_session(
                g_worker, &call,
                dispatch.content_package[0] ? dispatch.content_package
                                            : "conference-greeting",
                &g_worker_session);
        }
        if (dispatch.wire_version == 2u) {
            if (g_corrupt_dispatch_result_generation) {
                dispatch.worker_connection_generation++;
            }
            ivr_flowmq_gateway_t *result_gateway =
                strcmp(dispatch.worker_id, "ivr-worker-b") == 0
                    ? g_worker_gw_b
                    : g_worker_gw;
            TEST_ASSERT_EQUAL(
                IVR_OK, ivr_flowmq_gateway_send_dispatch_result_v2(
                            result_gateway, &dispatch, rc,
                            ivr_worker_active_sessions(g_worker), 2,
                            rc == IVR_OK ? "" : "assignment_failed",
                            rc == IVR_OK ? "" : "test assignment failed"));
        } else {
            TEST_ASSERT_EQUAL(
                IVR_OK, ivr_flowmq_gateway_send_dispatch_result(
                            g_worker_gw, &dispatch, rc,
                            rc == IVR_OK ? "" : "assignment_failed",
                            rc == IVR_OK ? "" : "test assignment failed"));
        }
    }
    if (has_release) {
        ivr_call_ref_t call;
        memset(&call, 0, sizeof(call));
        call.room_id.data = release.room_id;
        call.room_id.size = strlen(release.room_id);
        call.call_id.data = release.call_id;
        call.call_id.size = strlen(release.call_id);
        call.call_generation = release.call_generation;
        ivr_status_t rc = ivr_worker_release_call(g_worker, &call);
        if (rc == IVR_OK) {
            g_worker_session = NULL;
        }
        if (g_drop_release_result_once) {
            g_drop_release_result_once = 0;
        } else {
            TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_release_result(
                                          g_worker_gw, &release, rc,
                                          rc == IVR_OK ? "" : "release_failed",
                                          rc == IVR_OK
                                              ? ""
                                              : "test release failed"));
        }
    }
}

/* ---- shadow command gateway for the worker (capture, never send) ---- */
static ivr_mutex_t g_cmd_lock;
static char g_cmd_buf[16][64];
static int g_cmd_count;

static ivr_status_t shadow_gateway_submit(void *ctx,
                                          const ivr_command_view_t *cmd) {
    (void)ctx;
    ivr_mutex_lock(&g_cmd_lock);
    if (g_cmd_count < 16) {
        int n = (int)cmd->command_type.size;
        if (n > 63) {
            n = 63;
        }
        memcpy(g_cmd_buf[g_cmd_count], cmd->command_type.data, (size_t)n);
        g_cmd_buf[g_cmd_count][n] = '\0';
        g_cmd_count++;
    }
    ivr_mutex_unlock(&g_cmd_lock);
    return IVR_OK;
}

/* ---- media stubs (bot media is P2) ---- */
static ivr_status_t stub_start_bot(void *ctx, const ivr_call_ref_t *call) {
    (void)ctx;
    (void)call;
    return IVR_OK;
}
static ivr_status_t stub_play_pcm(void *ctx, const ivr_call_ref_t *call,
                                  const ivr_bytes_view_t *text) {
    (void)ctx;
    (void)call;
    (void)text;
    return IVR_OK;
}
static ivr_status_t stub_cancel_input(void *ctx, const ivr_call_ref_t *call) {
    (void)ctx;
    (void)call;
    return IVR_OK;
}
static ivr_status_t stub_stop_bot(void *ctx, const ivr_call_ref_t *call) {
    (void)ctx;
    (void)call;
    return IVR_OK;
}
static ivr_status_t stub_begin_input(void *ctx, const ivr_call_ref_t *call,
                                     const ivr_bytes_view_t *input_id,
                                     uint64_t input_generation) {
    (void)ctx; (void)call; (void)input_id; (void)input_generation;
    return IVR_OK;
}
static ivr_status_t stub_end_input(void *ctx, const ivr_call_ref_t *call,
                                   const ivr_bytes_view_t *input_id,
                                   uint64_t input_generation) {
    (void)ctx; (void)call; (void)input_id; (void)input_generation;
    return IVR_OK;
}

static ivr_status_t stub_media_create(void *ctx, const ivr_call_ref_t *call,
                                      ivr_media_port_ops_t *out_media,
                                      void **out_instance) {
    (void)call;
    if (!ctx || !out_media || !out_instance) {
        return IVR_EINVAL;
    }
    *out_media = *(const ivr_media_port_ops_t *)ctx;
    *out_instance = ctx;
    return IVR_OK;
}

static void stub_media_destroy(void *ctx, void *instance) {
    (void)ctx;
    (void)instance;
}

/* ---- components ---- */
static turbo_room_service_t *g_service = NULL;
static ivr_fmq_adapter_t *g_adapter = NULL;
static ivr_flowmq_gateway_t *g_client = NULL;
static ivr_command_gateway_ops_t g_client_ops;
static ivr_flowmq_gateway_t *g_worker_gw = NULL;
static ivr_command_gateway_ops_t g_dummy_ops;
static ivr_flowmq_subscriber_t *g_subscriber = NULL;
static ivr_worker_t *g_worker = NULL;
static ivr_media_port_ops_t g_stub_media_ops;
static ivr_media_port_factory_ops_t g_stub_media_factory;
static int g_worker_lost_events;
static int g_room_media_release_calls;

static int worker_lost_event_count(void) {
    int count;
    ivr_mutex_lock(&g_dispatch_lock);
    count = g_worker_lost_events;
    ivr_mutex_unlock(&g_dispatch_lock);
    return count;
}

static int room_media_release_count(void) {
    int count;
    ivr_mutex_lock(&g_dispatch_lock);
    count = g_room_media_release_calls;
    ivr_mutex_unlock(&g_dispatch_lock);
    return count;
}

static void sub_on_event(void *ctx, const ivr_event_view_t *event) {
    (void)ctx;
    if (event->event_type.size == strlen("call.terminal") &&
        memcmp(event->event_type.data, "call.terminal",
               event->event_type.size) == 0 &&
        strstr(event->payload_json.data, "\"reason\":\"worker_lost\"")) {
        ivr_mutex_lock(&g_dispatch_lock);
        g_worker_lost_events++;
        ivr_mutex_unlock(&g_dispatch_lock);
    }
    (void)ivr_worker_submit_event_copy(g_worker, event);
}

static ivr_status_t room_media_release(void *ctx, const char *room_id,
                                       const char *call_id, char *error,
                                       size_t error_capacity) {
    (void)ctx;
    (void)room_id;
    (void)call_id;
    (void)error;
    (void)error_capacity;
    ivr_mutex_lock(&g_dispatch_lock);
    g_room_media_release_calls++;
    ivr_mutex_unlock(&g_dispatch_lock);
    return IVR_OK;
}

static ivr_status_t send_join_from_client(const char *message_id,
                                          uint64_t expected_version) {
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
    return g_client_ops.submit_copy(g_client_ops.context, &cmd);
}

static ivr_status_t send_leave_from_client(const char *message_id,
                                           uint64_t expected_version) {
    static ivr_bytes_view_t type = {"conference.leave", 16};
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
    return g_client_ops.submit_copy(g_client_ops.context, &cmd);
}

static int wait_worker_session_seq(uint64_t seq, int timeout_ms) {
    for (int i = 0; i < timeout_ms / 20; i++) {
        process_worker_dispatch();
        if (g_worker_session && g_worker_session->last_sequence == seq) {
            return 1;
        }
        ivr_thread_sleep_ms(20);
    }
    return 0;
}

static int wait_assignment(const char *message_id,
                           ivr_fmq_assignment_state_t state, int timeout_ms) {
    for (int i = 0; i < timeout_ms / 20; i++) {
        ivr_fmq_assignment_t assignment;
        process_worker_dispatch();
        if (ivr_fmq_adapter_get_assignment(g_adapter, message_id,
                                           &assignment) == IVR_OK &&
            assignment.state == state) {
            return 1;
        }
        ivr_thread_sleep_ms(20);
    }
    return 0;
}

static void start_second_worker(void) {
    ivr_flowmq_gateway_config_t config;
    ivr_command_gateway_ops_t unused_ops;
    ivr_worker_status_view_t status;
    memset(&config, 0, sizeof(config));
    memset(&unused_ops, 0, sizeof(unused_ops));
    config.worker_id = "ivr-worker-b";
    config.host = "127.0.0.1";
    config.port = TEST_ROUTER_PORT;
    config.timeout_ms = 5000;
    config.on_reply = on_worker_reply;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_create(
                                  &config, &unused_ops, &g_worker_gw_b));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_start(g_worker_gw_b));
    ivr_thread_sleep_ms(500);

    memset(&status, 0, sizeof(status));
    status.instance_id = "e2e-instance-b";
    status.connection_generation = 8;
    status.max_sessions = 2;
    status.lease_duration_ms = 15000;
    status.capabilities = "turboxml,flowmq,dispatch-v2,health.ready";
    for (int i = 0; i < 20 && !ivr_fmq_adapter_worker_registered(
                                      g_adapter, "ivr-worker-b");
         i++) {
        char message_id[32];
        snprintf(message_id, sizeof(message_id), "ws-e2e-b-%d", i);
        (void)ivr_flowmq_gateway_send_worker_sync_v2(
            g_worker_gw_b, message_id, &status);
        ivr_thread_sleep_ms(100);
    }
    TEST_ASSERT_TRUE(
        ivr_fmq_adapter_worker_registered(g_adapter, "ivr-worker-b"));
}

void setUp(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    TEST_ASSERT_EQUAL(DATA_BIND_OK, TurboMediaIvrV1_codec_create(&g_codec, &err));
    ivr_mutex_init(&g_reply_lock);
    ivr_mutex_init(&g_cmd_lock);
    ivr_mutex_init(&g_dispatch_lock);
    g_reply_ready = 0;
    g_cmd_count = 0;
    g_worker_synced = 0;
    g_worker_session = NULL;
    g_dispatch_pending = 0;
    g_release_pending = 0;
    g_drop_release_result_once = 0;
    g_force_dispatch_reject = 0;
    g_corrupt_dispatch_result_generation = 0;
    g_worker_lost_events = 0;
    g_room_media_release_calls = 0;

    g_service = turbo_room_service_create();
    TEST_ASSERT_NOT_NULL(g_service);
    turbo_room_config_t room_config;
    memset(&room_config, 0, sizeof(room_config));
    room_config.room_id = "room-42";
    room_config.room_type = TURBO_ROOM_TYPE_CONFERENCE;
    TEST_ASSERT_EQUAL_INT(0, turbo_room_service_create_room(g_service,
                                                            &room_config));

    ivr_fmq_adapter_config_t acfg;
    memset(&acfg, 0, sizeof(acfg));
    acfg.bind_host = "127.0.0.1";
    acfg.bind_port = TEST_ROUTER_PORT;
    acfg.pub_port = TEST_PUB_PORT;
    acfg.pub_topic = "room.events";
    acfg.timeout_ms = 5000;
    acfg.queue_capacity = 8;
    acfg.dedup_capacity = 8;
    acfg.assignment_capacity = 1; /* exercise terminal-slot reuse */
    acfg.dispatch_deadline_ms = 500;
    acfg.media.release_caller_audio = room_media_release;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_fmq_adapter_create(g_service, &acfg, &g_adapter));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_start(g_adapter));

    /* client DEALER: issues RoomService commands */
    ivr_flowmq_gateway_config_t ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.worker_id = "ivr-client";
    ccfg.host = "127.0.0.1";
    ccfg.port = TEST_ROUTER_PORT;
    ccfg.timeout_ms = 5000;
    ccfg.on_reply = on_client_reply;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_create(&ccfg, &g_client_ops,
                                                        &g_client));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_start(g_client));

    /* worker DEALER: registers (worker.sync) and receives dispatch pushes */
    ivr_flowmq_gateway_config_t wcfg_gw;
    memset(&wcfg_gw, 0, sizeof(wcfg_gw));
    wcfg_gw.worker_id = "ivr-worker-test";
    wcfg_gw.host = "127.0.0.1";
    wcfg_gw.port = TEST_ROUTER_PORT;
    wcfg_gw.timeout_ms = 5000;
    wcfg_gw.on_reply = on_worker_reply;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_gateway_create(&wcfg_gw, &g_dummy_ops,
                                                &g_worker_gw));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_start(g_worker_gw));

    ivr_flowmq_subscriber_config_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.host = "127.0.0.1";
    scfg.port = TEST_PUB_PORT;
    scfg.topic = "room.events";
    scfg.timeout_ms = 5000;
    scfg.on_event = sub_on_event;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_flowmq_subscriber_create(&scfg, &g_subscriber));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_subscriber_start(g_subscriber));

    ivr_command_gateway_ops_t shadow_ops;
    memset(&shadow_ops, 0, sizeof(shadow_ops));
    shadow_ops.abi_version = 1;
    shadow_ops.context = NULL;
    shadow_ops.submit_copy = shadow_gateway_submit;
    memset(&g_stub_media_ops, 0, sizeof(g_stub_media_ops));
    g_stub_media_ops.abi_version = IVR_WORKER_ABI_VERSION;
    g_stub_media_ops.start_bot = stub_start_bot;
    g_stub_media_ops.play_pcm = stub_play_pcm;
    g_stub_media_ops.cancel_input = stub_cancel_input;
    g_stub_media_ops.stop_bot = stub_stop_bot;
    g_stub_media_ops.begin_input = stub_begin_input;
    g_stub_media_ops.end_input = stub_end_input;
    memset(&g_stub_media_factory, 0, sizeof(g_stub_media_factory));
    g_stub_media_factory.abi_version = IVR_WORKER_ABI_VERSION;
    g_stub_media_factory.context = &g_stub_media_ops;
    g_stub_media_factory.create = stub_media_create;
    g_stub_media_factory.destroy = stub_media_destroy;

    ivr_worker_config_t wcfg;
    memset(&wcfg, 0, sizeof(wcfg));
    wcfg.abi_version = IVR_WORKER_ABI_VERSION;
    wcfg.worker_id = "ivr-worker-test";
    wcfg.max_sessions_per_worker = 2;
    wcfg.session_inbox_capacity = 8;
    wcfg.max_event_bytes = 65536;
    wcfg.max_command_bytes = 16384;
    wcfg.content_root = IVR_TEST_CONTENT_ROOT;
    wcfg.drain_deadline_ms = 5000;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_create(&wcfg, &shadow_ops,
                                                &g_stub_media_factory,
                                                &g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));

    /* let the DEALERs reach the ROUTER and the SUB register on the PUB */
    ivr_thread_sleep_ms(1200);
    /* register the worker: RoomService records the worker and captures its
       ROUTER route for later dispatch pushes */
    /* register with retry: the first attempt may race the ROUTER's
       PEER_CONNECTED event that establishes the identity binding */
    for (int attempt = 0; attempt < 20 && !g_worker_synced; attempt++) {
        char mid[32];
        ivr_worker_status_view_t status;
        snprintf(mid, sizeof(mid), "ws-e2e-%d", attempt);
        memset(&status, 0, sizeof(status));
        status.instance_id = "e2e-instance";
        status.connection_generation = 7;
        status.max_sessions = 2;
        status.lease_duration_ms = 15000;
        status.capabilities = "turboxml,flowmq,dispatch-v2,health.ready";
        (void)ivr_flowmq_gateway_send_worker_sync_v2(g_worker_gw, mid,
                                                     &status);
        for (int i = 0; i < 25 && !g_worker_synced; i++) {
            ivr_thread_sleep_ms(20);
        }
    }
    TEST_ASSERT_TRUE(g_worker_synced);
}

void tearDown(void) {
    if (g_worker) {
        ivr_worker_begin_drain(g_worker);
        ivr_worker_destroy(g_worker);
        g_worker = NULL;
    }
    if (g_subscriber) {
        ivr_flowmq_subscriber_destroy(g_subscriber);
        g_subscriber = NULL;
    }
    if (g_worker_gw) {
        ivr_flowmq_gateway_destroy(g_worker_gw);
        g_worker_gw = NULL;
    }
    if (g_worker_gw_b) {
        ivr_flowmq_gateway_destroy(g_worker_gw_b);
        g_worker_gw_b = NULL;
    }
    if (g_client) {
        ivr_flowmq_gateway_destroy(g_client);
        g_client = NULL;
    }
    if (g_adapter) {
        ivr_fmq_adapter_stop(g_adapter);
        ivr_fmq_adapter_destroy(g_adapter);
        g_adapter = NULL;
    }
    if (g_service) {
        turbo_room_service_destroy(g_service);
        g_service = NULL;
    }
    if (g_codec) {
        data_bind_free(g_codec);
        g_codec = NULL;
    }
    ivr_mutex_destroy(&g_reply_lock);
    ivr_mutex_destroy(&g_cmd_lock);
    ivr_mutex_destroy(&g_dispatch_lock);
}

void test_full_loop_dispatch_and_event(void) {
    /* client joins the conference: aggregate applies, worker is dispatched and
       assigns a session, then the joined event routes into that session */
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-join-1", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(0, reply_status());
    TEST_ASSERT_TRUE(wait_assignment("dispatch-mid-join-1",
                                     IVR_FMQ_ASSIGNMENT_ACCEPTED, 8000));
    /* dispatch -> session created; joined event -> session sequence advances */
    TEST_ASSERT_TRUE(wait_worker_session_seq(1, 8000));
    TEST_ASSERT_NOT_NULL(g_worker_session);
    TEST_ASSERT_EQUAL_UINT64(1u, g_worker_session->last_sequence);
    TEST_ASSERT_EQUAL_UINT64(2u, g_worker_session->room_version);
    /* the aggregate actually holds the participant */
    turbo_room_participant_summary_t summary;
    TEST_ASSERT_EQUAL_INT(0, turbo_room_service_get_participant_summary(
                                 g_service, "room-42", "call-42", &summary));
    TEST_ASSERT_EQUAL_INT(TURBO_PARTICIPANT_ROLE_CUSTOMER, summary.role);

    /* An idempotent join reuses the active assignment. Accepted is not a
       terminal observation and must not be evicted or counted twice. */
    ivr_mutex_lock(&g_reply_lock);
    g_reply_ready = 0;
    g_reply_len = 0;
    ivr_mutex_unlock(&g_reply_lock);
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-join-reuse", 2));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_status());
    ivr_fmq_assignment_t duplicate;
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_fmq_adapter_get_assignment(
                                      g_adapter, "dispatch-mid-join-reuse",
                                      &duplicate));
    ivr_fmq_assignment_t active;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_assignment(
                                  g_adapter, "dispatch-mid-join-1", &active));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_ASSIGNMENT_ACCEPTED, active.state);
    ivr_fmq_worker_snapshot_t worker;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(1u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(0u, worker.reserved_sessions);
    TEST_ASSERT_EQUAL_UINT32(2u, worker.protocol_version);
}

void test_stale_generation_ack_does_not_advance_assignment(void) {
    g_corrupt_dispatch_result_generation = 1;
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-stale-ack", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_status());

    int dispatch_pending = 0;
    for (int i = 0; i < 100 && !dispatch_pending; i++) {
        ivr_mutex_lock(&g_dispatch_lock);
        dispatch_pending = g_dispatch_pending;
        ivr_mutex_unlock(&g_dispatch_lock);
        ivr_thread_sleep_ms(10);
    }
    TEST_ASSERT_TRUE(dispatch_pending);
    process_worker_dispatch();
    ivr_thread_sleep_ms(100);

    ivr_fmq_assignment_t assignment;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_assignment(
                                  g_adapter, "dispatch-mid-stale-ack",
                                  &assignment));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_ASSIGNMENT_PENDING, assignment.state);
    ivr_fmq_worker_snapshot_t worker;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(0u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(1u, worker.reserved_sessions);
}

void test_reject_retries_next_worker_with_new_attempt(void) {
    start_second_worker();

    g_force_dispatch_reject = 1;
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-retry-next", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_status());
    TEST_ASSERT_TRUE(wait_assignment("dispatch-mid-retry-next",
                                     IVR_FMQ_ASSIGNMENT_ACCEPTED, 8000));

    ivr_fmq_assignment_t assignment;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_assignment(
                                  g_adapter, "dispatch-mid-retry-next",
                                  &assignment));
    TEST_ASSERT_EQUAL_UINT32(2u, assignment.attempt_count);
    TEST_ASSERT_EQUAL_STRING("ivr-worker-b", assignment.worker_id);
    TEST_ASSERT_EQUAL_STRING("dispatch-mid-retry-next-a2",
                             assignment.attempt_id);
    ivr_fmq_worker_snapshot_t first;
    ivr_fmq_worker_snapshot_t second;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &first));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-b", &second));
    TEST_ASSERT_EQUAL_UINT32(0u, first.reserved_sessions);
    TEST_ASSERT_EQUAL_UINT32(0u, first.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(0u, second.reserved_sessions);
    TEST_ASSERT_EQUAL_UINT32(1u, second.active_sessions);
}

void test_timeout_retries_next_worker_and_cleans_duplicate_late_accept(void) {
    start_second_worker();
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-timeout-next", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_status());

    ivr_call_dispatch_t stale_dispatch;
    memset(&stale_dispatch, 0, sizeof(stale_dispatch));
    for (int i = 0; i < 200 && !take_pending_dispatch(&stale_dispatch); i++) {
        ivr_thread_sleep_ms(10);
    }
    TEST_ASSERT_EQUAL_STRING("ivr-worker-test", stale_dispatch.worker_id);
    TEST_ASSERT_EQUAL_STRING("dispatch-mid-timeout-next",
                             stale_dispatch.attempt_id);

    TEST_ASSERT_TRUE(wait_assignment("dispatch-mid-timeout-next",
                                     IVR_FMQ_ASSIGNMENT_ACCEPTED, 8000));
    ivr_fmq_assignment_t assignment;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_assignment(
                                  g_adapter, "dispatch-mid-timeout-next",
                                  &assignment));
    TEST_ASSERT_EQUAL_UINT32(2u, assignment.attempt_count);
    TEST_ASSERT_EQUAL_STRING("ivr-worker-b", assignment.worker_id);
    TEST_ASSERT_EQUAL_STRING("dispatch-mid-timeout-next-a2",
                             assignment.attempt_id);

    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL(
            IVR_OK, ivr_flowmq_gateway_send_dispatch_result_v2(
                        g_worker_gw, &stale_dispatch, IVR_OK, 1, 2, "", ""));
    }

    ivr_call_release_t cleanup;
    memset(&cleanup, 0, sizeof(cleanup));
    for (int i = 0; i < 200 && !take_pending_release(&cleanup); i++) {
        ivr_thread_sleep_ms(10);
    }
    TEST_ASSERT_EQUAL_STRING("release-late-dispatch-mid-timeout-next",
                             cleanup.message_id);
    TEST_ASSERT_EQUAL_STRING("ivr-worker-test", cleanup.worker_id);
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL(IVR_OK, ivr_flowmq_gateway_send_release_result(
                                      g_worker_gw, &cleanup, IVR_OK, "", ""));
    }

    TEST_ASSERT_EQUAL(
        IVR_OK, ivr_flowmq_gateway_send_dispatch_result_v2(
                    g_worker_gw, &stale_dispatch, IVR_OK, 1, 2, "", ""));
    TEST_ASSERT_EQUAL(
        IVR_OK, ivr_flowmq_gateway_send_dispatch_result_v2(
                    g_worker_gw, &stale_dispatch, IVR_ENOSPC, 0, 2,
                    "late_reject", "out-of-order late reject"));
    ivr_thread_sleep_ms(200);

    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_assignment(
                                  g_adapter, "dispatch-mid-timeout-next",
                                  &assignment));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_ASSIGNMENT_ACCEPTED, assignment.state);
    TEST_ASSERT_EQUAL_STRING("ivr-worker-b", assignment.worker_id);
    ivr_fmq_worker_snapshot_t first;
    ivr_fmq_worker_snapshot_t second;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &first));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-b", &second));
    TEST_ASSERT_EQUAL_UINT32(0u, first.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(0u, first.reserved_sessions);
    TEST_ASSERT_EQUAL_UINT32(1u, second.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(0u, second.reserved_sessions);
}

void test_snapshot_keeps_continuity_after_dispatch(void) {
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-join-2", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_TRUE(wait_worker_session_seq(1, 8000));
    TEST_ASSERT_EQUAL_UINT64(2u, g_worker_session->room_version);

    /* get_snapshot -> authoritative snapshot -> session continuity kept */
    static ivr_bytes_view_t type = {"get_snapshot", 12};
    static ivr_bytes_view_t room = {"room-42", 7};
    static ivr_bytes_view_t call = {"call-42", 7};
    static ivr_bytes_view_t args = {"{}", 2};
    ivr_command_view_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.message_id.data = "mid-snap-2";
    cmd.message_id.size = 10;
    cmd.command_type = type;
    cmd.call.room_id = room;
    cmd.call.call_id = call;
    cmd.call.call_generation = 1;
    cmd.args_json = args;
    TEST_ASSERT_EQUAL(IVR_OK, g_client_ops.submit_copy(g_client_ops.context,
                                                       &cmd));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(0, reply_status());
    ivr_thread_sleep_ms(300);
    TEST_ASSERT_EQUAL_UINT64(1u, g_worker_session->last_sequence);
    TEST_ASSERT_EQUAL_UINT64(2u, g_worker_session->room_version);
}

void test_rejected_dispatch_is_observed_without_starting_session(void) {
    g_force_dispatch_reject = 1;
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-join-reject", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_status());
    TEST_ASSERT_TRUE(wait_assignment("dispatch-mid-join-reject",
                                     IVR_FMQ_ASSIGNMENT_REJECTED, 8000));

    ivr_fmq_assignment_t assignment;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_assignment(
                                  g_adapter, "dispatch-mid-join-reject",
                                  &assignment));
    TEST_ASSERT_EQUAL_INT(IVR_ENOSPC, assignment.status_code);
    TEST_ASSERT_NULL(g_worker_session);

    /* Compatibility phase: join already returned success, so a rejected ACK
       is observable but does not silently roll back the participant. */
    turbo_room_participant_summary_t summary;
    TEST_ASSERT_EQUAL_INT(0, turbo_room_service_get_participant_summary(
                                 g_service, "room-42", "call-42", &summary));
}

void test_conference_leave_releases_worker_and_capacity(void) {
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-join-release", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_status());
    TEST_ASSERT_TRUE(wait_assignment("dispatch-mid-join-release",
                                     IVR_FMQ_ASSIGNMENT_ACCEPTED, 8000));
    TEST_ASSERT_TRUE(wait_worker_session_seq(1, 8000));
    TEST_ASSERT_EQUAL_UINT32(1u, ivr_worker_active_sessions(g_worker));

    ivr_mutex_lock(&g_reply_lock);
    g_reply_ready = 0;
    g_reply_len = 0;
    ivr_mutex_unlock(&g_reply_lock);
    TEST_ASSERT_EQUAL(IVR_OK,
                      send_leave_from_client("mid-leave-release", 2));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_status());

    for (int i = 0; i < 400; i++) {
        ivr_fmq_assignment_t assignment;
        process_worker_dispatch();
        if (ivr_fmq_adapter_get_assignment(
                g_adapter, "dispatch-mid-join-release", &assignment) ==
            IVR_ESTATE) {
            break;
        }
        ivr_thread_sleep_ms(20);
    }
    ivr_fmq_assignment_t assignment;
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_fmq_adapter_get_assignment(
                                      g_adapter,
                                      "dispatch-mid-join-release",
                                      &assignment));
    TEST_ASSERT_EQUAL_UINT32(0u, ivr_worker_active_sessions(g_worker));
    ivr_fmq_worker_snapshot_t worker;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(0u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(0u, worker.reserved_sessions);
    turbo_room_participant_summary_t participant;
    TEST_ASSERT_TRUE(turbo_room_service_get_participant_summary(
                         g_service, "room-42", "call-42", &participant) != 0);
}

void test_late_dispatch_accept_is_released_fail_closed(void) {
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-late-accept", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_status());

    ivr_thread_sleep_ms(800);
    ivr_fmq_assignment_t assignment;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_assignment(
                                  g_adapter, "dispatch-mid-late-accept",
                                  &assignment));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_ASSIGNMENT_REJECTED, assignment.state);
    TEST_ASSERT_EQUAL_STRING("dispatch_timeout", assignment.error_code);

    for (int i = 0; i < 400; i++) {
        process_worker_dispatch();
        if (ivr_fmq_adapter_get_assignment(
                g_adapter, "dispatch-mid-late-accept", &assignment) ==
            IVR_ESTATE) {
            break;
        }
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_fmq_adapter_get_assignment(
                                      g_adapter,
                                      "dispatch-mid-late-accept",
                                      &assignment));
    TEST_ASSERT_EQUAL_UINT32(0u, ivr_worker_active_sessions(g_worker));
    ivr_fmq_worker_snapshot_t worker;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(0u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(0u, worker.reserved_sessions);
}

void test_leave_cancels_pending_dispatch_and_cleans_late_accept(void) {
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-cancel-join", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_status());
    ivr_fmq_assignment_t assignment;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_assignment(
                                  g_adapter, "dispatch-mid-cancel-join",
                                  &assignment));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_ASSIGNMENT_PENDING, assignment.state);

    ivr_mutex_lock(&g_reply_lock);
    g_reply_ready = 0;
    g_reply_len = 0;
    ivr_mutex_unlock(&g_reply_lock);
    TEST_ASSERT_EQUAL(IVR_OK, send_leave_from_client("mid-cancel-leave", 2));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_status());

    for (int i = 0; i < 400; i++) {
        process_worker_dispatch();
        if (ivr_fmq_adapter_get_assignment(
                g_adapter, "dispatch-mid-cancel-join", &assignment) ==
            IVR_ESTATE) {
            break;
        }
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_fmq_adapter_get_assignment(
                                      g_adapter,
                                      "dispatch-mid-cancel-join",
                                      &assignment));
    TEST_ASSERT_EQUAL_UINT32(0u, ivr_worker_active_sessions(g_worker));
    ivr_fmq_worker_snapshot_t worker;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_UINT32(0u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(0u, worker.reserved_sessions);
    turbo_room_participant_summary_t participant;
    TEST_ASSERT_TRUE(turbo_room_service_get_participant_summary(
                         g_service, "room-42", "call-42", &participant) != 0);
}

void test_release_result_loss_retries_stable_release(void) {
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-retry-join", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_TRUE(wait_assignment("dispatch-mid-retry-join",
                                     IVR_FMQ_ASSIGNMENT_ACCEPTED, 8000));
    TEST_ASSERT_TRUE(wait_worker_session_seq(1, 8000));

    g_drop_release_result_once = 1;
    ivr_mutex_lock(&g_reply_lock);
    g_reply_ready = 0;
    g_reply_len = 0;
    ivr_mutex_unlock(&g_reply_lock);
    TEST_ASSERT_EQUAL(IVR_OK, send_leave_from_client("mid-retry-leave", 2));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_EQUAL_INT(IVR_OK, reply_status());
    process_worker_dispatch();

    ivr_fmq_assignment_t assignment;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_assignment(
                                  g_adapter, "dispatch-mid-retry-join",
                                  &assignment));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_ASSIGNMENT_RELEASING, assignment.state);
    for (int i = 0; i < 400; i++) {
        process_worker_dispatch();
        if (ivr_fmq_adapter_get_assignment(
                g_adapter, "dispatch-mid-retry-join", &assignment) ==
            IVR_ESTATE) {
            break;
        }
        ivr_thread_sleep_ms(20);
    }
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_fmq_adapter_get_assignment(
                                      g_adapter, "dispatch-mid-retry-join",
                                      &assignment));
    TEST_ASSERT_EQUAL_UINT32(0u, ivr_worker_active_sessions(g_worker));
}

void test_worker_disconnect_fail_closed_cleans_aggregate_and_emits_terminal(void) {
    TEST_ASSERT_EQUAL(IVR_OK, send_join_from_client("mid-worker-loss", 1));
    TEST_ASSERT_TRUE(wait_reply(8000));
    TEST_ASSERT_TRUE(wait_assignment("dispatch-mid-worker-loss",
                                     IVR_FMQ_ASSIGNMENT_ACCEPTED, 8000));
    TEST_ASSERT_TRUE(wait_worker_session_seq(1, 8000));

    ivr_flowmq_gateway_destroy(g_worker_gw);
    g_worker_gw = NULL;
    for (int i = 0; i < 500; i++) {
        ivr_fmq_assignment_t assignment;
        turbo_room_participant_summary_t participant;
        int assignment_gone =
            ivr_fmq_adapter_get_assignment(g_adapter,
                                           "dispatch-mid-worker-loss",
                                           &assignment) == IVR_ESTATE;
        int participant_gone =
            turbo_room_service_get_participant_summary(
                g_service, "room-42", "call-42", &participant) != 0;
        if (assignment_gone && participant_gone &&
            worker_lost_event_count() == 1) {
            break;
        }
        ivr_thread_sleep_ms(20);
    }

    ivr_fmq_assignment_t assignment;
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_fmq_adapter_get_assignment(
                                      g_adapter, "dispatch-mid-worker-loss",
                                      &assignment));
    turbo_room_participant_summary_t participant;
    TEST_ASSERT_TRUE(turbo_room_service_get_participant_summary(
                         g_service, "room-42", "call-42", &participant) != 0);
    TEST_ASSERT_EQUAL_INT(1, room_media_release_count());
    TEST_ASSERT_EQUAL_INT(1, worker_lost_event_count());
    ivr_fmq_worker_snapshot_t worker;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_fmq_adapter_get_worker(
                                  g_adapter, "ivr-worker-test", &worker));
    TEST_ASSERT_EQUAL_INT(IVR_FMQ_WORKER_EXPIRED, worker.state);
    TEST_ASSERT_EQUAL_UINT32(0u, worker.active_sessions);
    TEST_ASSERT_EQUAL_UINT32(0u, worker.reserved_sessions);
}

spec("test_ivr_worker_e2e") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_full_loop_dispatch_and_event);
  TT_TEST(test_stale_generation_ack_does_not_advance_assignment);
  TT_TEST(test_reject_retries_next_worker_with_new_attempt);
  TT_TEST(test_timeout_retries_next_worker_and_cleans_duplicate_late_accept);
  TT_TEST(test_snapshot_keeps_continuity_after_dispatch);
  TT_TEST(test_rejected_dispatch_is_observed_without_starting_session);
  TT_TEST(test_conference_leave_releases_worker_and_capacity);
  TT_TEST(test_late_dispatch_accept_is_released_fail_closed);
  TT_TEST(test_leave_cancels_pending_dispatch_and_cleans_late_accept);
  TT_TEST(test_release_result_loss_retries_stable_release);
  TT_TEST(test_worker_disconnect_fail_closed_cleans_aggregate_and_emits_terminal);
}
