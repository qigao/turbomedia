/* test_ivr_flowmq_subscriber.c - FlowMQ SUB domain-event subscriber.
 * Two layers:
 *  1. Pure decode unit tests: a ConferenceParticipantJoinedEventV1 frame
 *     decodes into the expected ivr_event_view_t; result/command frames and
 *     malformed buffers are rejected (fail fast, no partial decode).
 *  2. Live PUB/SUB integration: bridge (ROUTER + PUB) -> subscriber (SUB) ->
 *     gateway (DEALER); a conference.join applied by the RoomService mock is
 *     observed as "room.participant.joined" with room_version/sequence. */
#include "ivr_flowmq_gateway.h"
#include "ivr_flowmq_subscriber.h"
#include "ivr_frame.h"
#include "ivr_room_bridge.h"
#include "ivr_thread.h"
#include "turbomedia_ivr_v1.h"
#include "tinytest.h"
#include <string.h>

#define TEST_ROUTER_PORT 17713
#define TEST_PUB_PORT 17714

static DataBind *g_codec = NULL;

/* ---- mock RoomService handler (same semantics as test_ivr_room_bridge) ---- */
static uint64_t g_room_version = 0;
static uint64_t g_sequence = 0;
static uint64_t g_applied = 0;

static uint64_t mock_get_room_version(void *ctx, const char *room_id) {
    (void)ctx;
    (void)room_id;
    return g_room_version;
}

static ivr_status_t mock_on_command(void *ctx, const ivr_room_command_t *cmd,
                                    ivr_room_command_result_t *result) {
    (void)ctx;
    if (strcmp(cmd->command, "get_snapshot") == 0) {
        /* authoritative read-only report: current version/sequence, no bump */
        result->status_code = 0;
        result->room_version = g_room_version;
        result->sequence = g_sequence;
        return IVR_OK;
    }
    g_applied++;
    g_room_version++;
    g_sequence++;
    result->status_code = 0;
    result->room_version = g_room_version;
    result->sequence = g_sequence;
    return IVR_OK;
}

/* ---- event capture (subscriber on_event runs on the SUB receive thread) ---- */
static ivr_mutex_t g_ev_lock;
static char g_ev_type[64];
static char g_ev_room[128];
static char g_ev_call[128];
static char g_ev_id[128];
static char g_ev_payload[4096];
static uint64_t g_ev_generation;
static uint64_t g_ev_room_version;
static uint64_t g_ev_sequence;
static int g_ev_ready;

static void on_event_cb(void *ctx, const ivr_event_view_t *event) {
    (void)ctx;
    if (!event) {
        return;
    }
    ivr_mutex_lock(&g_ev_lock);
    snprintf(g_ev_type, sizeof(g_ev_type), "%.*s",
             (int)event->event_type.size, event->event_type.data);
    snprintf(g_ev_room, sizeof(g_ev_room), "%.*s",
             (int)event->call.room_id.size, event->call.room_id.data);
    snprintf(g_ev_call, sizeof(g_ev_call), "%.*s",
             (int)event->call.call_id.size, event->call.call_id.data);
    snprintf(g_ev_id, sizeof(g_ev_id), "%.*s", (int)event->event_id.size,
             event->event_id.data);
    snprintf(g_ev_payload, sizeof(g_ev_payload), "%.*s",
             (int)event->payload_json.size, event->payload_json.data);
    g_ev_generation = event->call.call_generation;
    g_ev_room_version = event->call.expected_room_version;
    g_ev_sequence = event->sequence;
    g_ev_ready = 1;
    ivr_mutex_unlock(&g_ev_lock);
}

static int wait_event(int timeout_ms) {
    for (int i = 0; i < timeout_ms / 20; i++) {
        ivr_mutex_lock(&g_ev_lock);
        int ready = g_ev_ready;
        ivr_mutex_unlock(&g_ev_lock);
        if (ready) {
            return 1;
        }
        ivr_thread_sleep_ms(20);
    }
    return 0;
}

/* ---- pure decode unit tests ---- */

void test_decode_participant_joined(void) {
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_room_bridge_encode_participant_joined(
            g_codec, "ev-1", "mid-1", "ivr-worker-test", "room-42", "call-42",
            1, 11, 91, 0, "call-42", "caller", frame, sizeof(frame), &len), IVR_OK);
    ivr_subscriber_decoded_t *decoded = NULL;
    check_equal(ivr_flowmq_subscriber_decode(
                                  g_codec, frame, len, &decoded), IVR_OK);
    check_not_null(decoded);
    const ivr_event_view_t *v = ivr_subscriber_decoded_view(decoded);
    check_not_null(v);
    check_equal(v->event_type.data, "room.participant.joined");
    check_equal(v->event_id.data, "ev-1");
    check_equal(v->call.room_id.data, "room-42");
    check_equal(v->call.call_id.data, "call-42");
    check_equal((uint64_t)(v->call.call_generation), (uint64_t)(1u));
    /* session maps call.expected_room_version -> room_version */
    check_equal((uint64_t)(v->call.expected_room_version), (uint64_t)(11u));
    check_equal((uint64_t)(v->sequence), (uint64_t)(91u));
    check_not_null(strstr(v->payload_json.data, "\"participant_role\":\"caller\""));
    check_not_null(strstr(v->payload_json.data, "\"room_version\":11"));
    ivr_subscriber_decoded_free(decoded);
}

void test_decode_rejects_result_frame(void) {
    ivr_room_command_result_t result;
    memset(&result, 0, sizeof(result));
    result.status_code = 0;
    result.room_version = 11;
    result.sequence = 91;
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_room_encode_result(g_codec, "mid-1", "ivr-worker-test",
                                             "room-42", "call-42", 1, &result,
                                             frame, sizeof(frame), &len), IVR_OK);
    ivr_subscriber_decoded_t *decoded = (ivr_subscriber_decoded_t *)1;
    check_equal(ivr_flowmq_subscriber_decode(
                                      g_codec, frame, len, &decoded), IVR_ESTATE);
    check_null(decoded);
}

void test_decode_rejects_command_frame(void) {
    static ivr_bytes_view_t mid = {"mid-1", 5};
    static ivr_bytes_view_t type = {"conference.join", 15};
    static ivr_bytes_view_t room = {"room-42", 7};
    static ivr_bytes_view_t call = {"call-42", 7};
    static ivr_bytes_view_t args = {"{}", 2};
    ivr_command_view_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.message_id = mid;
    cmd.command_type = type;
    cmd.call.room_id = room;
    cmd.call.call_id = call;
    cmd.call.call_generation = 1;
    cmd.call.expected_room_version = 10;
    cmd.args_json = args;
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_command(
                          g_codec, &cmd, "ivr-worker-test", frame,
                          sizeof(frame), &len), IVR_OK);
    ivr_subscriber_decoded_t *decoded = (ivr_subscriber_decoded_t *)1;
    check_equal(ivr_flowmq_subscriber_decode(
                                      g_codec, frame, len, &decoded), IVR_ESTATE);
    check_null(decoded);
}

void test_decode_rejects_short_frame(void) {
    const uint8_t garbage[] = {'T', 'I', 'V', 'R', 1};
    ivr_subscriber_decoded_t *decoded = (ivr_subscriber_decoded_t *)1;
    check_equal(ivr_flowmq_subscriber_decode(
                                      g_codec, garbage, sizeof(garbage),
                                      &decoded), IVR_ESTATE);
    check_null(decoded);
}

void test_decode_snapshot(void) {
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_room_bridge_encode_snapshot(g_codec, "room-42", "call-42", 1,
                                        5, 90, "caller", "active", frame,
                                        sizeof(frame), &len), IVR_OK);
    ivr_subscriber_decoded_t *decoded = NULL;
    check_equal(ivr_flowmq_subscriber_decode(
                                  g_codec, frame, len, &decoded), IVR_OK);
    check_not_null(decoded);
    const ivr_event_view_t *v = ivr_subscriber_decoded_view(decoded);
    check_not_null(v);
    check_equal(v->event_type.data, "room.snapshot.loaded");
    check_equal(v->call.room_id.data, "room-42");
    check_equal(v->call.call_id.data, "call-42");
    check_equal((uint64_t)(v->call.call_generation), (uint64_t)(1u));
    check_equal((uint64_t)(v->call.expected_room_version), (uint64_t)(5u));
    check_equal((uint64_t)(v->sequence), (uint64_t)(90u));
    ivr_subscriber_decoded_free(decoded);
}

/* ---- live PUB/SUB integration ---- */

static ivr_room_bridge_t *g_bridge = NULL;
static ivr_flowmq_subscriber_t *g_subscriber = NULL;
static ivr_flowmq_gateway_t *g_gateway = NULL;
static ivr_command_gateway_ops_t g_ops;
static ivr_atomic_int_t g_gateway_connected;
static ivr_atomic_int_t g_subscriber_connected;

static void on_gateway_connection(void *ctx, int connected) {
    (void)ctx;
    atomic_store_explicit(&g_gateway_connected, connected,
                          memory_order_release);
}

static void on_subscriber_connection(void *ctx, int connected) {
    (void)ctx;
    atomic_store_explicit(&g_subscriber_connected, connected,
                          memory_order_release);
}

static ivr_status_t send_get_snapshot(void) {
    static ivr_bytes_view_t type = {"get_snapshot", 12};
    static ivr_bytes_view_t room = {"room-42", 7};
    static ivr_bytes_view_t call = {"call-42", 7};
    static ivr_bytes_view_t args = {"{}", 2};
    ivr_command_view_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.message_id.data = "mid-snap";
    cmd.message_id.size = 8;
    cmd.command_type = type;
    cmd.call.room_id = room;
    cmd.call.call_id = call;
    cmd.call.call_generation = 1;
    cmd.args_json = args;
    return g_ops.submit_copy(g_ops.context, &cmd);
}

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
    check_equal(TurboMediaIvrV1_codec_create(&g_codec, &err), DATA_BIND_OK);
    ivr_mutex_init(&g_ev_lock);
    g_ev_ready = 0;
    g_ev_type[0] = '\0';
    g_ev_room[0] = '\0';
    g_ev_call[0] = '\0';
    g_ev_id[0] = '\0';
    g_ev_payload[0] = '\0';
    g_room_version = 10;
    g_sequence = 90;
    g_applied = 0;
    atomic_store_explicit(&g_gateway_connected, 0, memory_order_release);
    atomic_store_explicit(&g_subscriber_connected, 0, memory_order_release);

    ivr_room_command_handler_t handler;
    memset(&handler, 0, sizeof(handler));
    handler.get_room_version = mock_get_room_version;
    handler.on_command = mock_on_command;

    ivr_room_bridge_config_t bcfg;
    memset(&bcfg, 0, sizeof(bcfg));
    bcfg.host = "127.0.0.1";
    bcfg.port = TEST_ROUTER_PORT;
    bcfg.timeout_ms = 5000;
    bcfg.queue_capacity = 8;
    bcfg.dedup_capacity = 8;
    bcfg.handler = handler;
    bcfg.pub_host = "127.0.0.1";
    bcfg.pub_port = TEST_PUB_PORT;
    bcfg.pub_topic = "room.events";
    check_equal(ivr_room_bridge_create(&bcfg, &g_bridge), IVR_OK);
    check_equal(ivr_room_bridge_start(g_bridge), IVR_OK);

    ivr_flowmq_subscriber_config_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.host = "127.0.0.1";
    scfg.port = TEST_PUB_PORT;
    scfg.topic = "room.events";
    scfg.timeout_ms = 5000;
    scfg.on_event = on_event_cb;
    scfg.on_connection = on_subscriber_connection;
    check_equal(ivr_flowmq_subscriber_create(&scfg, &g_subscriber), IVR_OK);
    check_equal(ivr_flowmq_subscriber_start(g_subscriber), IVR_OK);

    ivr_flowmq_gateway_config_t gcfg;
    memset(&gcfg, 0, sizeof(gcfg));
    gcfg.worker_id = "ivr-worker-test";
    gcfg.host = "127.0.0.1";
    gcfg.port = TEST_ROUTER_PORT;
    gcfg.timeout_ms = 5000;
    gcfg.on_connection = on_gateway_connection;
    check_equal(ivr_flowmq_gateway_create(&gcfg, &g_ops,
                                                        &g_gateway), IVR_OK);
    check_equal(ivr_flowmq_gateway_start(g_gateway), IVR_OK);
    /* let the DEALER reach the ROUTER and the SUB register on the PUB */
    ivr_thread_sleep_ms(1200);
    check_true(atomic_load_explicit(&g_gateway_connected,
                                         memory_order_acquire));
    check_true(atomic_load_explicit(&g_subscriber_connected,
                                         memory_order_acquire));
}

void tearDown(void) {
    if (g_gateway) {
        ivr_flowmq_gateway_destroy(g_gateway);
        g_gateway = NULL;
    }
    if (g_subscriber) {
        ivr_flowmq_subscriber_destroy(g_subscriber);
        g_subscriber = NULL;
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
    ivr_mutex_destroy(&g_ev_lock);
}

void test_live_pubsub_participant_joined(void) {
    check_equal(send_join("mid-1", 10), IVR_OK);
    for (int i = 0; i < 400 && g_applied == 0; i++) {
        ivr_thread_sleep_ms(20);
    }
    check_equal((uint64_t)(g_applied), (uint64_t)(1u));
    check_equal(ivr_room_bridge_publish_participant_joined(
                    g_bridge, "event-mid-1", "mid-1", "ivr-worker-test",
                    "room-42", "call-42", 1, 11, 91, "call-42", "caller"), IVR_OK);
    check_true(wait_event(8000));
    check_equal(g_ev_type, "room.participant.joined");
    check_equal(g_ev_room, "room-42");
    check_equal(g_ev_call, "call-42");
    check_equal((uint64_t)(g_ev_generation), (uint64_t)(1u));
    check_equal((uint64_t)(g_ev_room_version), (uint64_t)(11u));
    check_equal((uint64_t)(g_ev_sequence), (uint64_t)(91u));
    check_not_null(strstr(g_ev_payload, "\"participant_role\":\"caller\""));
}

void test_live_snapshot_event(void) {
    check_equal(send_get_snapshot(), IVR_OK);
    check_true(wait_event(8000));
    check_equal(g_ev_type, "room.snapshot.loaded");
    check_equal(g_ev_room, "room-42");
    check_equal(g_ev_call, "call-42");
    check_equal((uint64_t)(g_ev_generation), (uint64_t)(1u));
    check_equal((uint64_t)(g_ev_room_version), (uint64_t)(10u));
    check_equal((uint64_t)(g_ev_sequence), (uint64_t)(90u));
    check_not_null(strstr(g_ev_payload, "\"last_sequence\":90"));
}

void test_live_connection_callbacks_report_disconnect(void) {
    ivr_room_bridge_stop(g_bridge);
    ivr_room_bridge_destroy(g_bridge);
    g_bridge = NULL;
    for (int i = 0; i < 200; ++i) {
        if (!atomic_load_explicit(&g_gateway_connected,
                                  memory_order_acquire) &&
            !atomic_load_explicit(&g_subscriber_connected,
                                  memory_order_acquire)) {
            break;
        }
        ivr_thread_sleep_ms(20);
    }
    check_false(atomic_load_explicit(&g_gateway_connected,
                                           memory_order_acquire));
    check_false(atomic_load_explicit(&g_subscriber_connected,
                                           memory_order_acquire));
}

spec("test_ivr_flowmq_subscriber") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  it("test_decode_participant_joined") { test_decode_participant_joined(); };
  it("test_decode_rejects_result_frame") { test_decode_rejects_result_frame(); };
  it("test_decode_rejects_command_frame") { test_decode_rejects_command_frame(); };
  it("test_decode_rejects_short_frame") { test_decode_rejects_short_frame(); };
  it("test_decode_snapshot") { test_decode_snapshot(); };
  it("test_live_pubsub_participant_joined") { test_live_pubsub_participant_joined(); };
  it("test_live_snapshot_event") { test_live_snapshot_event(); };
  it("test_live_connection_callbacks_report_disconnect") { test_live_connection_callbacks_report_disconnect(); };
}
