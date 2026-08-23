/* test_ivr_flowmq.c - FlowMQ DEALER gateway encoding + lifecycle.
 * The pure frame encoder is verified against the generated schema (decode the
 * BIN payload back and compare typed fields); no live ROUTER peer is needed.
 * Requires TURBO_MEDIA_HAS_FLOWMQ (FlowMQ/TurboFlow + tbe_compiler present). */
#include "ivr_flowmq_gateway.h"
#include "ivr_frame.h"
#include "ivr_room_bridge.h"
#include "turbomedia_ivr_v1.h"
#include "tinytest.h"
#include <string.h>

static DataBind *g_codec = NULL;

void setUp(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    check_equal(TurboMediaIvrV1_codec_create(&g_codec, &err), DATA_BIND_OK);
}

void tearDown(void) {
    if (g_codec) {
        data_bind_free(g_codec);
        g_codec = NULL;
    }
}

static void make_join_command(ivr_command_view_t *view) {
    static ivr_bytes_view_t mid = {"mid-1", 5};
    static ivr_bytes_view_t type = {"conference.join", 15};
    static ivr_bytes_view_t room = {"room-42", 7};
    static ivr_bytes_view_t call = {"call-42", 7};
    static ivr_bytes_view_t args = {"{}", 2};
    memset(view, 0, sizeof(*view));
    view->message_id = mid;
    view->command_type = type;
    view->call.room_id = room;
    view->call.call_id = call;
    view->call.call_generation = 3;
    view->call.expected_room_version = 17;
    view->args_json = args;
}

void test_encode_conference_join(void) {
    ivr_command_view_t cmd;
    make_join_command(&cmd);
    uint8_t frame[4096];
    size_t len = 0;
    ivr_status_t rc = ivr_flowmq_gateway_encode_command(
        g_codec, &cmd, "ivr-worker-01", frame, sizeof(frame), &len);
    check_equal(rc, IVR_OK);
    check_true(len > IVR_FRAME_HEADER_SIZE);

    /* header */
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    check_equal(ivr_frame_decode(frame, len, &info), IVR_OK);
    check_equal((int)(info.format), (int)(IVR_FMT_BIN));
    check_equal((int)(info.kind), (int)(IVR_KIND_COMMAND));
    check_equal((uint32_t)(info.schema_type_id), (uint32_t)(IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1));

    /* payload decodes back to the typed message */
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    check_equal(data_bind_object_from_bin(g_codec,
                                                "ConferenceJoinCommandV1",
                                                frame + IVR_FRAME_HEADER_SIZE,
                                                len - IVR_FRAME_HEADER_SIZE,
                                                &obj, &err), DATA_BIND_OK);
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *mid = data_bind_value_get(root, "message_id");
    check_not_null(mid);
    check_equal(data_bind_value_as_string(mid), "mid-1");
    const DataBindValue *role = data_bind_value_get(root, "participant_role");
    check_not_null(role);
    check_equal(data_bind_value_as_string(role), "caller");
    const DataBindValue *gen = data_bind_value_get(root, "call_generation");
    check_not_null(gen);
    check_equal((uint64_t)(data_bind_value_as_uint64(gen)), (uint64_t)(3u));
    const DataBindValue *ver = data_bind_value_get(root, "expected_room_version");
    check_not_null(ver);
    check_equal((uint64_t)(data_bind_value_as_uint64(ver)), (uint64_t)(17u));
    data_bind_object_free(obj);
}

void test_encode_get_snapshot(void) {
    ivr_command_view_t cmd;
    make_join_command(&cmd);
    /* GetSnapshotCommandV1 has no expected_room_version; the encoder must not
       emit it (unknown schema fields are not guessed). */
    static ivr_bytes_view_t type = {"get_snapshot", 12};
    cmd.command_type = type;
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_command(
                                  g_codec, &cmd, "ivr-worker-01", frame,
                                  sizeof(frame), &len), IVR_OK);
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    check_equal(ivr_frame_decode(frame, len, &info), IVR_OK);
    check_equal((uint32_t)(info.schema_type_id), (uint32_t)(IVR_TYPE_GET_SNAPSHOT_COMMAND_V1));
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    check_equal(data_bind_object_from_bin(g_codec, "GetSnapshotCommandV1",
                                                frame + IVR_FRAME_HEADER_SIZE,
                                                len - IVR_FRAME_HEADER_SIZE,
                                                &obj, &err), DATA_BIND_OK);
    const DataBindValue *root = data_bind_object_value(obj);
    check_null(data_bind_value_get(root, "expected_room_version"));
    const DataBindValue *mid = data_bind_value_get(root, "message_id");
    check_not_null(mid);
    check_equal(data_bind_value_as_string(mid), "mid-1");
    data_bind_object_free(obj);
}

void test_encode_worker_sync(void) {
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_worker_sync(
                                  g_codec, "ws-1", "ivr-worker-01", frame,
                                  sizeof(frame), &len), IVR_OK);
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    check_equal(ivr_frame_decode(frame, len, &info), IVR_OK);
    check_equal((uint32_t)(info.schema_type_id), (uint32_t)(IVR_TYPE_WORKER_SYNC_COMMAND_V1));
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    check_equal(data_bind_object_from_bin(g_codec, "WorkerSyncCommandV1",
                                                frame + IVR_FRAME_HEADER_SIZE,
                                                len - IVR_FRAME_HEADER_SIZE,
                                                &obj, &err), DATA_BIND_OK);
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *mid = data_bind_value_get(root, "message_id");
    check_not_null(mid);
    check_equal(data_bind_value_as_string(mid), "ws-1");
    const DataBindValue *wid = data_bind_value_get(root, "worker_id");
    check_not_null(wid);
    check_equal(data_bind_value_as_string(wid), "ivr-worker-01");
    check_null(data_bind_value_get(root, "room_id"));
    data_bind_object_free(obj);
}

static void make_worker_status(ivr_worker_status_view_t *status) {
    memset(status, 0, sizeof(*status));
    status->instance_id = "instance-7";
    status->connection_generation = 7;
    status->max_sessions = 8;
    status->active_sessions = 3;
    status->reserved_sessions = 1;
    status->lease_duration_ms = 15000;
    status->capabilities = "turboxml,flowmq,tts,asr,health.ready";
}

void test_encode_worker_sync_v2_and_heartbeat(void) {
    ivr_worker_status_view_t status;
    make_worker_status(&status);
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_worker_sync_v2(
                                  g_codec, "sync-v2-1", "ivr-worker-01",
                                  &status, frame, sizeof(frame), &len), IVR_OK);
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    check_equal(ivr_frame_decode(frame, len, &info), IVR_OK);
    check_equal((uint32_t)(info.schema_type_id), (uint32_t)(IVR_TYPE_WORKER_SYNC_COMMAND_V2));

    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    check_equal(data_bind_object_from_bin(g_codec, "WorkerSyncCommandV2",
                                                frame + IVR_FRAME_HEADER_SIZE,
                                                len - IVR_FRAME_HEADER_SIZE,
                                                &obj, &err), DATA_BIND_OK);
    const DataBindValue *root = data_bind_object_value(obj);
    check_equal(data_bind_value_as_string(data_bind_value_get(root, "instance_id")), "instance-7");
    check_equal((uint64_t)(data_bind_value_as_uint64(
                data_bind_value_get(root, "connection_generation"))), (uint64_t)(7u));
    check_equal((uint64_t)(data_bind_value_as_uint64(
                data_bind_value_get(root, "max_sessions"))), (uint64_t)(8u));
    check_equal((uint64_t)(data_bind_value_as_uint64(
                data_bind_value_get(root, "active_sessions"))), (uint64_t)(3u));
    data_bind_object_free(obj);

    check_equal(ivr_flowmq_gateway_encode_worker_heartbeat(
                                  g_codec, "heartbeat-1", "ivr-worker-01",
                                  &status, frame, sizeof(frame), &len), IVR_OK);
    check_equal(ivr_frame_decode(frame, len, &info), IVR_OK);
    check_equal((uint32_t)(info.schema_type_id), (uint32_t)(IVR_TYPE_WORKER_HEARTBEAT_V1));
}

void test_encode_worker_status_rejects_invalid_capacity(void) {
    ivr_worker_status_view_t status;
    make_worker_status(&status);
    status.active_sessions = status.max_sessions;
    status.reserved_sessions = 1;
    uint8_t frame[4096];
    size_t len = 99;
    check_equal(ivr_flowmq_gateway_encode_worker_sync_v2(
                          g_codec, "sync-v2-invalid", "ivr-worker-01",
                          &status, frame, sizeof(frame), &len), IVR_EINVAL);
}

void test_encode_unknown_command_rejected(void) {
    ivr_command_view_t cmd;
    make_join_command(&cmd);
    /* worker-local intent: must not be sent on the DEALER channel */
    static ivr_bytes_view_t type = {"rtc.join", 8};
    cmd.command_type = type;
    uint8_t frame[4096];
    size_t len = 99;
    check_equal(ivr_flowmq_gateway_encode_command(
                          g_codec, &cmd, "ivr-worker-01", frame,
                          sizeof(frame), &len), IVR_ESTATE);
    check_equal(len, 0u); /* zeroed on entry by the encoder contract */
}

void test_encode_short_buffer_rejected(void) {
    ivr_command_view_t cmd;
    make_join_command(&cmd);
    uint8_t frame[IVR_FRAME_HEADER_SIZE]; /* no room for the payload */
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_command(
                          g_codec, &cmd, "ivr-worker-01", frame,
                          sizeof(frame), &len), IVR_ENOSPC);
}

static void decode_string_field(const char *type_name, const uint8_t *frame,
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

void test_encode_command_escapes_special_chars(void) {
    /* message_id/room_id/call_id may legally contain quotes, backslashes and
       control characters: the encoder must escape them so the JSON DataBind
       parses back to the identical string (no malformed/semantic change). */
    static const char mid[] = "mid\"\\\n1";
    static const char room[] = "ro\"\\\t2";
    static const char call[] = "ca\\\"3";
    static ivr_bytes_view_t mid_v = {mid, sizeof(mid) - 1};
    static ivr_bytes_view_t room_v = {room, sizeof(room) - 1};
    static ivr_bytes_view_t call_v = {call, sizeof(call) - 1};

    ivr_command_view_t cmd;
    make_join_command(&cmd);
    cmd.message_id = mid_v;
    cmd.call.room_id = room_v;
    cmd.call.call_id = call_v;

    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_command(
                          g_codec, &cmd, "ivr-worker-01", frame,
                          sizeof(frame), &len), IVR_OK);

    char out[128];
    decode_string_field("ConferenceJoinCommandV1", frame, len, "message_id",
                        out, sizeof(out));
    check_equal(out, mid);
    decode_string_field("ConferenceJoinCommandV1", frame, len, "room_id",
                        out, sizeof(out));
    check_equal(out, room);
    decode_string_field("ConferenceJoinCommandV1", frame, len, "call_id",
                        out, sizeof(out));
    check_equal(out, call);
}

void test_encode_worker_sync_escapes_special_chars(void) {
    static const char mid[] = "ws\"\\\n1";
    static const char wid[] = "worker\\\"2";
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_worker_sync(
                          g_codec, mid, wid, frame, sizeof(frame), &len), IVR_OK);
    char out[128];
    decode_string_field("WorkerSyncCommandV1", frame, len, "message_id",
                        out, sizeof(out));
    check_equal(out, mid);
    decode_string_field("WorkerSyncCommandV1", frame, len, "worker_id",
                        out, sizeof(out));
    check_equal(out, wid);
}

static void make_dispatch(ivr_call_dispatch_t *dispatch) {
    memset(dispatch, 0, sizeof(*dispatch));
    snprintf(dispatch->message_id, sizeof(dispatch->message_id), "%s",
             "dispatch-1");
    snprintf(dispatch->worker_id, sizeof(dispatch->worker_id), "%s",
             "ivr-worker-01");
    snprintf(dispatch->room_id, sizeof(dispatch->room_id), "%s", "room-42");
    snprintf(dispatch->call_id, sizeof(dispatch->call_id), "%s", "call-42");
    dispatch->call_generation = 7;
}

static void make_dispatch_v2(ivr_call_dispatch_t *dispatch) {
    make_dispatch(dispatch);
    dispatch->wire_version = 2;
    snprintf(dispatch->assignment_id, sizeof(dispatch->assignment_id), "%s",
             "assignment-1");
    snprintf(dispatch->attempt_id, sizeof(dispatch->attempt_id), "%s",
             "attempt-1");
    snprintf(dispatch->worker_instance_id,
             sizeof(dispatch->worker_instance_id), "%s", "instance-1");
    dispatch->worker_connection_generation = 9;
    dispatch->expected_room_version = 11;
    snprintf(dispatch->content_package, sizeof(dispatch->content_package),
             "%s", "conference-greeting");
    snprintf(dispatch->content_version, sizeof(dispatch->content_version),
             "%s", "2026-08-09");
    dispatch->deadline_timeout_ms = 5000;
}

void test_dispatch_v2_command_roundtrip(void) {
    ivr_call_dispatch_t dispatch;
    ivr_call_dispatch_t decoded;
    uint8_t frame[4096];
    size_t len = 0;
    make_dispatch_v2(&dispatch);

    check_equal(ivr_room_bridge_encode_dispatch_v2(
                                  g_codec, &dispatch, frame, sizeof(frame),
                                  &len), IVR_OK);
    check_equal(ivr_flowmq_gateway_decode_dispatch(
                                  g_codec, frame, len, &decoded), IVR_OK);
    check_equal((uint32_t)(decoded.wire_version), (uint32_t)(2u));
    check_equal(decoded.message_id, dispatch.message_id);
    check_equal(decoded.assignment_id, dispatch.assignment_id);
    check_equal(decoded.attempt_id, dispatch.attempt_id);
    check_equal(decoded.worker_instance_id, dispatch.worker_instance_id);
    check_equal((uint64_t)(decoded.worker_connection_generation), (uint64_t)(dispatch.worker_connection_generation));
    check_equal((uint64_t)(decoded.expected_room_version), (uint64_t)(dispatch.expected_room_version));
    check_equal(decoded.content_package, dispatch.content_package);
    check_equal(decoded.content_version, dispatch.content_version);
    check_equal((uint64_t)(decoded.deadline_timeout_ms), (uint64_t)(dispatch.deadline_timeout_ms));
}

void test_dispatch_v2_rejects_missing_fence_and_deadline(void) {
    ivr_call_dispatch_t dispatch;
    uint8_t frame[4096];
    size_t len = 0;
    make_dispatch_v2(&dispatch);
    dispatch.assignment_id[0] = '\0';
    check_equal(ivr_room_bridge_encode_dispatch_v2(
                                      g_codec, &dispatch, frame,
                                      sizeof(frame), &len), IVR_EINVAL);

    make_dispatch_v2(&dispatch);
    dispatch.attempt_id[0] = '\0';
    check_equal(ivr_room_bridge_encode_dispatch_v2(
                                      g_codec, &dispatch, frame,
                                      sizeof(frame), &len), IVR_EINVAL);

    make_dispatch_v2(&dispatch);
    dispatch.deadline_timeout_ms = 0;
    check_equal(ivr_room_bridge_encode_dispatch_v2(
                                      g_codec, &dispatch, frame,
                                      sizeof(frame), &len), IVR_EINVAL);
}

void test_dispatch_v2_result_roundtrip_and_capacity_validation(void) {
    ivr_call_dispatch_t dispatch;
    ivr_dispatch_result_t decoded;
    uint8_t frame[4096];
    size_t len = 0;
    make_dispatch_v2(&dispatch);

    check_equal(ivr_flowmq_gateway_encode_dispatch_result_v2(
                                  g_codec, &dispatch, IVR_OK, 3, 8, "", "",
                                  frame, sizeof(frame), &len), IVR_OK);
    check_equal(ivr_room_decode_dispatch_result(
                                  g_codec, frame, len, &decoded), IVR_OK);
    check_equal((uint32_t)(decoded.wire_version), (uint32_t)(2u));
    check_equal(decoded.assignment_id, dispatch.assignment_id);
    check_equal(decoded.attempt_id, dispatch.attempt_id);
    check_equal(decoded.worker_instance_id, dispatch.worker_instance_id);
    check_equal((uint64_t)(decoded.worker_connection_generation), (uint64_t)(dispatch.worker_connection_generation));
    check_equal((uint32_t)(decoded.active_sessions), (uint32_t)(3u));
    check_equal((uint32_t)(decoded.max_sessions), (uint32_t)(8u));

    len = 99;
    check_equal(ivr_flowmq_gateway_encode_dispatch_result_v2(
                          g_codec, &dispatch, IVR_OK, 9, 8, "", "", frame,
                          sizeof(frame), &len), IVR_EINVAL);
}

void test_encode_dispatch_result_accepted(void) {
    ivr_call_dispatch_t dispatch;
    make_dispatch(&dispatch);
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_dispatch_result(
                                  g_codec, &dispatch, IVR_OK, "", "", frame,
                                  sizeof(frame), &len), IVR_OK);

    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    check_equal(ivr_frame_decode(frame, len, &info), IVR_OK);
    check_equal((int)(info.kind), (int)(IVR_KIND_RESULT));
    check_equal((uint32_t)(info.schema_type_id), (uint32_t)(IVR_TYPE_CALL_DISPATCH_RESULT_V1));

    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    check_equal(data_bind_object_from_bin(g_codec, "CallDispatchResultV1",
                                                frame + IVR_FRAME_HEADER_SIZE,
                                                len - IVR_FRAME_HEADER_SIZE,
                                                &obj, &err), DATA_BIND_OK);
    const DataBindValue *root = data_bind_object_value(obj);
    check_equal(data_bind_value_as_string(data_bind_value_get(root, "message_id")), "dispatch-1");
    check_equal(data_bind_value_as_string(data_bind_value_get(root, "worker_id")), "ivr-worker-01");
    check_equal((uint64_t)(data_bind_value_as_uint64(
                data_bind_value_get(root, "call_generation"))), (uint64_t)(7u));
    check_equal((int)(data_bind_value_as_int(data_bind_value_get(root, "status_code"))), (int)(IVR_OK));
    data_bind_object_free(obj);
}

void test_encode_dispatch_result_rejected_and_escaped(void) {
    ivr_call_dispatch_t dispatch;
    make_dispatch(&dispatch);
    snprintf(dispatch.message_id, sizeof(dispatch.message_id), "%s",
             "dispatch\"\\\n2");
    uint8_t frame[4096];
    size_t len = 0;
    static const char message[] = "session \"capacity\" exhausted\\retry";
    check_equal(ivr_flowmq_gateway_encode_dispatch_result(
                                  g_codec, &dispatch, IVR_ENOSPC,
                                  "worker_capacity", message, frame,
                                  sizeof(frame), &len), IVR_OK);

    char out[128];
    decode_string_field("CallDispatchResultV1", frame, len, "message_id", out,
                        sizeof(out));
    check_equal(out, dispatch.message_id);
    decode_string_field("CallDispatchResultV1", frame, len, "error_code", out,
                        sizeof(out));
    check_equal(out, "worker_capacity");
    decode_string_field("CallDispatchResultV1", frame, len, "error_message",
                        out, sizeof(out));
    check_equal(out, message);
}

void test_encode_dispatch_result_short_buffer_rejected(void) {
    ivr_call_dispatch_t dispatch;
    make_dispatch(&dispatch);
    uint8_t frame[IVR_FRAME_HEADER_SIZE];
    size_t len = 99;
    check_equal(ivr_flowmq_gateway_encode_dispatch_result(
                          g_codec, &dispatch, IVR_OK, "", "", frame,
                          sizeof(frame), &len), IVR_ENOSPC);
    check_equal(len, 0u);
}

void test_decode_dispatch_result(void) {
    ivr_call_dispatch_t dispatch;
    make_dispatch(&dispatch);
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_dispatch_result(
                                  g_codec, &dispatch, IVR_ENOSPC,
                                  "worker_capacity", "at capacity", frame,
                                  sizeof(frame), &len), IVR_OK);
    ivr_dispatch_result_t decoded;
    check_equal(ivr_room_decode_dispatch_result(
                                  g_codec, frame, len, &decoded), IVR_OK);
    check_equal(decoded.message_id, "dispatch-1");
    check_equal(decoded.worker_id, "ivr-worker-01");
    check_equal(decoded.room_id, "room-42");
    check_equal(decoded.call_id, "call-42");
    check_equal((uint64_t)(decoded.call_generation), (uint64_t)(7u));
    check_equal((int)(decoded.status_code), (int)(IVR_ENOSPC));
    check_equal(decoded.error_code, "worker_capacity");
    check_equal(decoded.error_message, "at capacity");
}

void test_decode_dispatch_result_rejects_wrong_type_and_short_frame(void) {
    ivr_command_view_t command;
    make_join_command(&command);
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_command(
                                  g_codec, &command, "ivr-worker-01", frame,
                                  sizeof(frame), &len), IVR_OK);
    ivr_dispatch_result_t decoded;
    check_equal(ivr_room_decode_dispatch_result(
                                      g_codec, frame, len, &decoded), IVR_ESTATE);
    check_equal(ivr_room_decode_dispatch_result(
                                      g_codec, frame,
                                      IVR_FRAME_HEADER_SIZE - 1, &decoded), IVR_ESTATE);
}

void test_release_command_and_result_roundtrip(void) {
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_room_bridge_encode_release(
                                  g_codec, "release-1", "ivr-worker-01",
                                  "room-42", "call-42", 7,
                                  "conference.leave", frame, sizeof(frame),
                                  &len), IVR_OK);
    ivr_call_release_t release;
    check_equal(ivr_flowmq_gateway_decode_release(
                                  g_codec, frame, len, &release), IVR_OK);
    check_equal(release.message_id, "release-1");
    check_equal(release.worker_id, "ivr-worker-01");
    check_equal(release.room_id, "room-42");
    check_equal(release.call_id, "call-42");
    check_equal((uint64_t)(release.call_generation), (uint64_t)(7u));
    check_equal(release.reason, "conference.leave");

    check_equal(ivr_flowmq_gateway_encode_release_result(
                                  g_codec, &release, IVR_ESTATE,
                                  "release_failed", "session busy", frame,
                                  sizeof(frame), &len), IVR_OK);
    ivr_release_result_t result;
    check_equal(ivr_room_decode_release_result(
                                  g_codec, frame, len, &result), IVR_OK);
    check_equal(result.message_id, "release-1");
    check_equal(result.worker_id, "ivr-worker-01");
    check_equal((int)(result.status_code), (int)(IVR_ESTATE));
    check_equal(result.error_code, "release_failed");
    check_equal(result.error_message, "session busy");
}

void test_release_decoders_reject_other_call_types(void) {
    ivr_call_dispatch_t dispatch;
    make_dispatch(&dispatch);
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_dispatch_result(
                                  g_codec, &dispatch, IVR_OK, "", "", frame,
                                  sizeof(frame), &len), IVR_OK);
    ivr_release_result_t result;
    check_equal(ivr_room_decode_release_result(
                                      g_codec, frame, len, &result), IVR_ESTATE);
    ivr_call_release_t release;
    check_equal(ivr_flowmq_gateway_decode_release(
                                      g_codec, frame, len, &release), IVR_ESTATE);
}

void test_decode_command_result(void) {
    ivr_room_command_result_t source;
    memset(&source, 0, sizeof(source));
    source.status_code = IVR_ESTATE;
    source.room_version = 23;
    source.sequence = 0;
    snprintf(source.error_message, sizeof(source.error_message),
             "room state conflict");
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_room_encode_result(g_codec, "mid-result-1", "ivr-worker-01",
                               "room-42", "call-42", 7, &source, frame,
                               sizeof(frame), &len), IVR_OK);

    ivr_command_result_envelope_t decoded;
    check_equal(ivr_flowmq_gateway_decode_result(
                                  g_codec, frame, len, &decoded), IVR_OK);
    check_equal(decoded.message_id, "mid-result-1");
    check_equal(decoded.worker_id, "ivr-worker-01");
    check_equal(decoded.room_id, "room-42");
    check_equal(decoded.call_id, "call-42");
    check_equal((uint64_t)(decoded.call_generation), (uint64_t)(7u));
    check_equal((int)(decoded.status_code), (int)(IVR_ESTATE));
    check_equal((uint64_t)(decoded.room_version), (uint64_t)(23u));
    check_equal((uint64_t)(decoded.sequence), (uint64_t)(0u));
    check_equal(decoded.error_message, "room state conflict");
}

void test_decode_result_rejects_command_frame(void) {
    ivr_command_view_t command;
    make_join_command(&command);
    uint8_t frame[4096];
    size_t len = 0;
    check_equal(ivr_flowmq_gateway_encode_command(
                                  g_codec, &command, "ivr-worker-01", frame,
                                  sizeof(frame), &len), IVR_OK);
    ivr_command_result_envelope_t decoded;
    check_equal(ivr_flowmq_gateway_decode_result(
                                      g_codec, frame, len, &decoded), IVR_ESTATE);
}


void test_dispatch_deadline_ok(void) {
    ivr_call_dispatch_t dispatch;
    make_dispatch_v2(&dispatch);
    dispatch.deadline_timeout_ms = 5000;

    /* inside the TTL */
    check_true(ivr_flowmq_gateway_dispatch_deadline_ok(
        &dispatch, 1000u, 1000u));
    check_true(ivr_flowmq_gateway_dispatch_deadline_ok(
        &dispatch, 1000u, 5999u));
    /* exactly at the deadline is expired */
    check_false(ivr_flowmq_gateway_dispatch_deadline_ok(
        &dispatch, 1000u, 6000u));
    check_false(ivr_flowmq_gateway_dispatch_deadline_ok(
        &dispatch, 1000u, 7000u));
    /* clock going backwards fails closed */
    check_false(ivr_flowmq_gateway_dispatch_deadline_ok(
        &dispatch, 5000u, 1000u));
    /* a zero deadline is never "ok" (V2 decode already rejects it) */
    dispatch.deadline_timeout_ms = 0;
    check_false(ivr_flowmq_gateway_dispatch_deadline_ok(
        &dispatch, 1000u, 1000u));
    check_false(ivr_flowmq_gateway_dispatch_deadline_ok(
        NULL, 1000u, 1000u));
}

static void make_media_command(ivr_media_command_t *command,
                               ivr_media_command_kind_t kind) {
    memset(command, 0, sizeof(*command));
    command->kind = kind;
    snprintf(command->message_id, sizeof(command->message_id), "media-1");
    snprintf(command->provider_session_id,
             sizeof(command->provider_session_id), "session-1");
    snprintf(command->dialog_id, sizeof(command->dialog_id), "dialog-1");
    snprintf(command->worker_id, sizeof(command->worker_id), "worker-1");
    snprintf(command->room_id, sizeof(command->room_id), "room-1");
    snprintf(command->call_id, sizeof(command->call_id), "call-1");
    command->call_generation = 2;
    command->operation_generation = 3;
    command->deadline_timeout_ms = 5000;
    snprintf(command->text, sizeof(command->text), "hello");
    snprintf(command->input_id, sizeof(command->input_id), "input-1");
    command->input_generation = 4;
    snprintf(command->reason, sizeof(command->reason), "completed");
}

static ivr_status_t encode_legacy_cancel_v1(uint8_t *frame,
                                             size_t frame_capacity,
                                             size_t *frame_size) {
    static const char json[] =
        "{\"message_id\":\"legacy-cancel\","
        "\"provider_session_id\":\"session-1\","
        "\"dialog_id\":\"dialog-1\",\"worker_id\":\"worker-1\","
        "\"room_id\":\"room-1\",\"call_id\":\"call-1\","
        "\"call_generation\":2,\"operation_generation\":3,"
        "\"deadline_timeout_ms\":5000}";
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    uint8_t *binary = NULL;
    size_t binary_size = 0;
    ivr_frame_info_t info;

    if (!frame || !frame_size || frame_capacity < IVR_FRAME_HEADER_SIZE) {
        return IVR_EINVAL;
    }
    *frame_size = 0;
    if (data_bind_object_from_json(g_codec, "MediaCancelCommandV1", json,
                                   strlen(json), &object,
                                   &error) != DATA_BIND_OK ||
        data_bind_object_serialize_bin(g_codec, object, &binary,
                                       &binary_size,
                                       &error) != DATA_BIND_OK) {
        data_bind_object_free(object);
        return IVR_ESTATE;
    }
    data_bind_object_free(object);
    if (binary_size > frame_capacity - IVR_FRAME_HEADER_SIZE) {
        data_bind_binary_free(binary);
        return IVR_ENOSPC;
    }
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_MEDIA_CANCEL_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(binary);
        return IVR_ESTATE;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, binary, binary_size);
    *frame_size = IVR_FRAME_HEADER_SIZE + binary_size;
    data_bind_binary_free(binary);
    return IVR_OK;
}

void test_media_commands_roundtrip(void) {
    static const ivr_media_command_kind_t kinds[] = {
        IVR_MEDIA_COMMAND_SESSION_OPEN, IVR_MEDIA_COMMAND_PLAY,
        IVR_MEDIA_COMMAND_INPUT_START, IVR_MEDIA_COMMAND_INPUT_STOP,
        IVR_MEDIA_COMMAND_CANCEL, IVR_MEDIA_COMMAND_SESSION_CLOSE};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        ivr_media_command_t source;
        ivr_media_command_t decoded;
        uint8_t frame[8192];
        size_t frame_size = 0;
        make_media_command(&source, kinds[i]);
        check_equal(ivr_room_bridge_encode_media_command(
                                      g_codec, &source, frame, sizeof(frame),
                                      &frame_size), IVR_OK);
        check_equal(ivr_flowmq_gateway_decode_media_command(
                                      g_codec, frame, frame_size, &decoded), IVR_OK);
        check_equal((int)(decoded.kind), (int)(source.kind));
        check_equal(decoded.message_id, source.message_id);
        check_equal(decoded.provider_session_id, source.provider_session_id);
        check_equal(decoded.dialog_id, source.dialog_id);
        check_equal(decoded.worker_id, source.worker_id);
        check_equal((uint64_t)(decoded.call_generation), (uint64_t)(source.call_generation));
        check_equal((uint64_t)(decoded.operation_generation), (uint64_t)(source.operation_generation));
        check_equal((uint64_t)(decoded.deadline_timeout_ms), (uint64_t)(source.deadline_timeout_ms));
        if (source.kind == IVR_MEDIA_COMMAND_INPUT_START ||
            source.kind == IVR_MEDIA_COMMAND_INPUT_STOP ||
            source.kind == IVR_MEDIA_COMMAND_CANCEL) {
            check_equal(decoded.input_id, source.input_id);
            check_equal((uint64_t)(decoded.input_generation), (uint64_t)(source.input_generation));
        }
    }
}

void test_media_cancel_v1_is_rejected_without_fallback(void) {
    ivr_media_command_t decoded;
    uint8_t frame[8192];
    size_t frame_size = 0;

    check_equal(encode_legacy_cancel_v1(
                                  frame, sizeof(frame), &frame_size), IVR_OK);
    check_equal(ivr_flowmq_gateway_decode_media_command(
                          g_codec, frame, frame_size, &decoded), IVR_ESTATE);
}

void test_media_command_encoder_rejects_invalid_fields(void) {
    ivr_media_command_t command;
    uint8_t frame[8192];
    size_t frame_size = 99;
    make_media_command(&command, IVR_MEDIA_COMMAND_PLAY);
    command.text[0] = '\0';
    check_equal(ivr_room_bridge_encode_media_command(
                                      g_codec, &command, frame, sizeof(frame),
                                      &frame_size), IVR_EINVAL);
    make_media_command(&command, IVR_MEDIA_COMMAND_INPUT_START);
    command.input_generation = 0;
    check_equal(ivr_room_bridge_encode_media_command(
                                      g_codec, &command, frame, sizeof(frame),
                                      &frame_size), IVR_EINVAL);
    make_media_command(&command, IVR_MEDIA_COMMAND_CANCEL);
    command.input_id[0] = '\0';
    check_equal(ivr_room_bridge_encode_media_command(
                                      g_codec, &command, frame, sizeof(frame),
                                      &frame_size), IVR_EINVAL);
    make_media_command(&command, IVR_MEDIA_COMMAND_CANCEL);
    command.input_generation = 0;
    check_equal(ivr_room_bridge_encode_media_command(
                                      g_codec, &command, frame, sizeof(frame),
                                      &frame_size), IVR_EINVAL);
    make_media_command(&command, IVR_MEDIA_COMMAND_CANCEL);
    command.deadline_timeout_ms = 0;
    check_equal(ivr_room_bridge_encode_media_command(
                                      g_codec, &command, frame, sizeof(frame),
                                      &frame_size), IVR_EINVAL);
    make_media_command(&command, IVR_MEDIA_COMMAND_CANCEL);
    check_equal(ivr_room_bridge_encode_media_command(
                                      g_codec, &command, frame,
                                      IVR_FRAME_HEADER_SIZE, &frame_size), IVR_ENOSPC);
}

void test_worker_inventory_query_and_page_roundtrip(void) {
    ivr_worker_inventory_request_t request;
    ivr_worker_inventory_request_t decoded_request;
    ivr_worker_inventory_envelope_t result;
    ivr_worker_inventory_envelope_t decoded_result;
    uint8_t frame[64u * 1024u];
    size_t frame_size = 0;

    memset(&request, 0, sizeof(request));
    snprintf(request.message_id, sizeof(request.message_id),
             "inventory-query-1");
    snprintf(request.worker_id, sizeof(request.worker_id), "worker-1");
    request.query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    request.query.expected_revision = 77;
    request.query.cursor = 4;
    request.query.limit = 2;
    check_equal(ivr_room_bridge_encode_inventory_query(
                                  g_codec, &request, frame, sizeof(frame),
                                  &frame_size), IVR_OK);
    check_equal(ivr_flowmq_gateway_decode_inventory_query(
                                  g_codec, frame, frame_size,
                                  &decoded_request), IVR_OK);
    check_equal(decoded_request.message_id, request.message_id);
    check_equal(decoded_request.worker_id, request.worker_id);
    check_equal((uint32_t)(decoded_request.query.inventory_version), (uint32_t)(IVR_WORKER_INVENTORY_VERSION));
    check_equal((uint64_t)(decoded_request.query.expected_revision), (uint64_t)(77));
    check_equal((uint32_t)(decoded_request.query.cursor), (uint32_t)(4));
    check_equal((uint32_t)(decoded_request.query.limit), (uint32_t)(2));

    memset(&result, 0, sizeof(result));
    snprintf(result.message_id, sizeof(result.message_id), "%s",
             request.message_id);
    snprintf(result.worker_id, sizeof(result.worker_id), "%s",
             request.worker_id);
    result.status_code = IVR_OK;
    result.page.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    result.page.revision = 77;
    result.page.cursor = 4;
    result.page.next_cursor = 9;
    result.page.total_active = 3;
    result.page.count = 2;
    result.page.has_more = 1;
    for (uint32_t i = 0; i < result.page.count; ++i) {
        ivr_worker_inventory_record_t *record = &result.page.records[i];
        snprintf(record->worker_id, sizeof(record->worker_id), "worker-1");
        snprintf(record->worker_instance_id,
                 sizeof(record->worker_instance_id), "instance-7");
        record->worker_epoch = 7;
        snprintf(record->provider_session_id,
                 sizeof(record->provider_session_id), "session-%u", i);
        snprintf(record->dialog_id, sizeof(record->dialog_id), "dialog-%u",
                 i);
        snprintf(record->room_id, sizeof(record->room_id), "room-%u", i);
        snprintf(record->call_id, sizeof(record->call_id), "call-%u", i);
        record->call_generation = i + 1u;
        record->operation_generation = i + 10u;
        if (i == 1u) {
            snprintf(record->input_id, sizeof(record->input_id),
                     "input-1");
            record->input_generation = 21;
            record->input_active = 1;
        }
        record->state = IVR_WORKER_RESOURCE_ACTIVE;
        record->rebindable = 1;
    }
    check_equal(ivr_flowmq_gateway_encode_inventory_page(
                                  g_codec, &result, frame, sizeof(frame),
                                  &frame_size), IVR_OK);
    check_equal(ivr_room_decode_inventory_page(
                                  g_codec, frame, frame_size,
                                  &decoded_result), IVR_OK);
    check_equal(decoded_result.message_id, result.message_id);
    check_equal(decoded_result.worker_id, result.worker_id);
    check_equal((uint64_t)(decoded_result.page.revision), (uint64_t)(77));
    check_equal((uint32_t)(decoded_result.page.cursor), (uint32_t)(4));
    check_equal((uint32_t)(decoded_result.page.next_cursor), (uint32_t)(9));
    check_equal((uint32_t)(decoded_result.page.total_active), (uint32_t)(3));
    check_equal((uint32_t)(decoded_result.page.count), (uint32_t)(2));
    check_true(decoded_result.page.has_more);
    check_equal(decoded_result.page.records[1]
                                 .worker_instance_id, "instance-7");
    check_equal(decoded_result.page.records[1]
                                 .provider_session_id, "session-1");
    check_equal((uint64_t)(decoded_result.page.records[1].worker_epoch), (uint64_t)(7));
    check_equal((uint64_t)(decoded_result.page.records[1].call_generation), (uint64_t)(2));
    check_equal((uint64_t)(decoded_result.page.records[1].operation_generation), (uint64_t)(11));
    check_equal((int)(decoded_result.page.records[1].state), (int)(IVR_WORKER_RESOURCE_ACTIVE));
    check_true(decoded_result.page.records[1].rebindable);
    check_true(decoded_result.page.records[1].input_active);
    check_equal(decoded_result.page.records[1].input_id, "input-1");
    check_equal((uint64_t)(decoded_result.page.records[1].input_generation), (uint64_t)(21));
}

void test_worker_inventory_rejects_unknown_version_and_oversize(void) {
    ivr_worker_inventory_request_t request;
    ivr_worker_inventory_envelope_t result;
    uint8_t frame[64u * 1024u];
    size_t frame_size = 99;

    memset(&request, 0, sizeof(request));
    snprintf(request.message_id, sizeof(request.message_id), "query-invalid");
    snprintf(request.worker_id, sizeof(request.worker_id), "worker-1");
    request.query.inventory_version =
        IVR_WORKER_INVENTORY_VERSION + 1u;
    request.query.limit = 1;
    check_equal(ivr_room_bridge_encode_inventory_query(
                          g_codec, &request, frame, sizeof(frame),
                          &frame_size), IVR_EVERSION);
    request.query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    request.query.limit = IVR_WORKER_INVENTORY_MAX_PAGE_SIZE + 1u;
    check_equal(ivr_room_bridge_encode_inventory_query(
                          g_codec, &request, frame, sizeof(frame),
                          &frame_size), IVR_EINVAL);

    memset(&result, 0, sizeof(result));
    snprintf(result.message_id, sizeof(result.message_id), "page-invalid");
    snprintf(result.worker_id, sizeof(result.worker_id), "worker-1");
    result.status_code = IVR_OK;
    result.page.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    result.page.revision = 1;
    result.page.count = IVR_WORKER_INVENTORY_MAX_PAGE_SIZE + 1u;
    check_equal(ivr_flowmq_gateway_encode_inventory_page(
                          g_codec, &result, frame, sizeof(frame),
                          &frame_size), IVR_EINVAL);
}

spec("test_ivr_flowmq") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  it("test_encode_conference_join") { test_encode_conference_join(); };
  it("test_encode_get_snapshot") { test_encode_get_snapshot(); };
  it("test_encode_worker_sync") { test_encode_worker_sync(); };
  it("test_encode_worker_sync_v2_and_heartbeat") { test_encode_worker_sync_v2_and_heartbeat(); };
  it("test_encode_worker_status_rejects_invalid_capacity") { test_encode_worker_status_rejects_invalid_capacity(); };
  it("test_encode_unknown_command_rejected") { test_encode_unknown_command_rejected(); };
  it("test_encode_short_buffer_rejected") { test_encode_short_buffer_rejected(); };
  it("test_encode_command_escapes_special_chars") { test_encode_command_escapes_special_chars(); };
  it("test_encode_worker_sync_escapes_special_chars") { test_encode_worker_sync_escapes_special_chars(); };
  it("test_encode_dispatch_result_accepted") { test_encode_dispatch_result_accepted(); };
  it("test_dispatch_v2_command_roundtrip") { test_dispatch_v2_command_roundtrip(); };
  it("test_dispatch_v2_rejects_missing_fence_and_deadline") { test_dispatch_v2_rejects_missing_fence_and_deadline(); };
  it("test_dispatch_v2_result_roundtrip_and_capacity_validation") { test_dispatch_v2_result_roundtrip_and_capacity_validation(); };
  it("test_encode_dispatch_result_rejected_and_escaped") { test_encode_dispatch_result_rejected_and_escaped(); };
  it("test_encode_dispatch_result_short_buffer_rejected") { test_encode_dispatch_result_short_buffer_rejected(); };
  it("test_decode_dispatch_result") { test_decode_dispatch_result(); };
  it("test_decode_dispatch_result_rejects_wrong_type_and_short_frame") { test_decode_dispatch_result_rejects_wrong_type_and_short_frame(); };
  it("test_release_command_and_result_roundtrip") { test_release_command_and_result_roundtrip(); };
  it("test_release_decoders_reject_other_call_types") { test_release_decoders_reject_other_call_types(); };
  it("test_decode_command_result") { test_decode_command_result(); };
  it("test_decode_result_rejects_command_frame") { test_decode_result_rejects_command_frame(); };
  it("test_dispatch_deadline_ok") { test_dispatch_deadline_ok(); };
  it("test_media_commands_roundtrip") { test_media_commands_roundtrip(); };
  it("test_media_cancel_v1_is_rejected_without_fallback") { test_media_cancel_v1_is_rejected_without_fallback(); };
  it("test_media_command_encoder_rejects_invalid_fields") { test_media_command_encoder_rejects_invalid_fields(); };
  it("test_worker_inventory_query_and_page_roundtrip") { test_worker_inventory_query_and_page_roundtrip(); };
  it("test_worker_inventory_rejects_unknown_version_and_oversize") { test_worker_inventory_rejects_unknown_version_and_oversize(); };
}
