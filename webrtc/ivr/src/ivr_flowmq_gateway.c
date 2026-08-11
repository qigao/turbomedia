#include "ivr_flowmq_gateway.h"
#include "ivr_internal.h"
#include "ivr_frame.h"
#include "turbomedia_ivr_v1.h"
#include "turbo_flow_fmq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ivr_flowmq_gateway_s {
    turbo_flow_fmq_app_t *app;
    DataBind *codec;
    char worker_id[128];
    ivr_command_gateway_ops_t ops;
    void (*on_reply)(void *ctx, const uint8_t *frame, size_t len);
    void *reply_ctx;
    void (*on_connection)(void *ctx, int connected);
    void *connection_ctx;
    uint64_t sent_count;
    uint64_t rejected_count;
};

/* ------------------------------------------------------------------ */
/* command -> RoomService schema message mapping                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *command;
    const char *type_name;
    uint16_t type_id;
    const char *participant_role; /* NULL when the message has no role */
    int has_expected_version;     /* message schema carries expected_room_version */
} ivr_cmd_map_t;

static const ivr_cmd_map_t kCmdMap[] = {
    {"conference.join", "ConferenceJoinCommandV1",
     IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1, "caller", 1},
    {"conference.leave", "ConferenceLeaveCommandV1",
     IVR_TYPE_CONFERENCE_LEAVE_COMMAND_V1, NULL, 1},
    {"get_snapshot", "GetSnapshotCommandV1",
     IVR_TYPE_GET_SNAPSHOT_COMMAND_V1, NULL, 0},
};
#define IVR_CMD_MAP_COUNT (sizeof(kCmdMap) / sizeof(kCmdMap[0]))

static const ivr_cmd_map_t *ivr_cmd_map_find(const char *command) {
    for (size_t i = 0; i < IVR_CMD_MAP_COUNT; i++) {
        if (strcmp(kCmdMap[i].command, command) == 0) {
            return &kCmdMap[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* frame encoding (pure, testable without a peer)                      */
/* ------------------------------------------------------------------ */

static const char *view_text(const ivr_bytes_view_t *view) {
    return (view && view->data) ? view->data : "";
}

static size_t view_len(const ivr_bytes_view_t *view) {
    return (view && view->data) ? view->size : 0;
}

static int ivr_build_command_json(const ivr_cmd_map_t *map, char *buf,
                                  size_t cap, const ivr_command_view_t *cmd,
                                  const char *worker_id) {
    char num[32];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, buf, cap);
    ivr_json_builder_raw(&jb, "{\"message_id\":");
    ivr_json_builder_string(&jb, view_text(&cmd->message_id),
                            view_len(&cmd->message_id));
    ivr_json_builder_raw(&jb, ",\"worker_id\":");
    ivr_json_builder_string(&jb, worker_id, strlen(worker_id));
    ivr_json_builder_raw(&jb, ",\"room_id\":");
    ivr_json_builder_string(&jb, view_text(&cmd->call.room_id),
                            view_len(&cmd->call.room_id));
    ivr_json_builder_raw(&jb, ",\"call_id\":");
    ivr_json_builder_string(&jb, view_text(&cmd->call.call_id),
                            view_len(&cmd->call.call_id));
    ivr_json_builder_raw(&jb, ",\"call_generation\":");
    snprintf(num, sizeof(num), "%llu",
             (unsigned long long)cmd->call.call_generation);
    ivr_json_builder_raw(&jb, num);
    if (map->has_expected_version) {
        ivr_json_builder_raw(&jb, ",\"expected_room_version\":");
        snprintf(num, sizeof(num), "%llu",
                 (unsigned long long)cmd->call.expected_room_version);
        ivr_json_builder_raw(&jb, num);
    }
    if (map->participant_role) {
        ivr_json_builder_raw(&jb, ",\"participant_role\":\"caller\"");
    }
    ivr_json_builder_raw(&jb, "}");
    return ivr_json_builder_ok(&jb) ? 0 : -1;
}

ivr_status_t ivr_flowmq_gateway_encode_command(
    DataBind *codec, const ivr_command_view_t *command, const char *worker_id,
    uint8_t *frame, size_t frame_cap, size_t *out_len) {
    if (!codec || !command || !worker_id || !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    const ivr_cmd_map_t *map = ivr_cmd_map_find(view_text(&command->command_type));
    if (!map) {
        /* worker-local intent (rtc.*, accept, disconnect): not a RoomService
           command, so it must not be sent on the DEALER channel */
        return IVR_ESTATE;
    }
    char json[1024];
    if (ivr_build_command_json(map, json, sizeof(json), command, worker_id) != 0) {
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_json(codec, map->type_name, json, strlen(json),
                                   &obj, &err) != DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = map->type_id;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

ivr_status_t ivr_flowmq_gateway_encode_worker_sync(
    DataBind *codec, const char *message_id, const char *worker_id,
    uint8_t *frame, size_t frame_cap, size_t *out_len) {
    if (!codec || !message_id || !worker_id || !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    char json[512];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, json, sizeof(json));
    ivr_json_builder_raw(&jb, "{\"message_id\":");
    ivr_json_builder_string(&jb, message_id, strlen(message_id));
    ivr_json_builder_raw(&jb, ",\"worker_id\":");
    ivr_json_builder_string(&jb, worker_id, strlen(worker_id));
    ivr_json_builder_raw(&jb, "}");
    if (!ivr_json_builder_ok(&jb)) {
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_json(codec, "WorkerSyncCommandV1", json,
                                   strlen(json), &obj, &err) != DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_WORKER_SYNC_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

static ivr_status_t ivr_flowmq_gateway_encode_worker_status(
    DataBind *codec, const char *type_name, uint16_t type_id,
    const char *message_id, const char *worker_id,
    const ivr_worker_status_view_t *status, uint8_t *frame, size_t frame_cap,
    size_t *out_len) {
    char json[1024];
    char number[32];
    uint64_t health_generation;
    int health_ready;
    ivr_json_builder_t jb;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    uint8_t *bin = NULL;
    size_t bin_len = 0;

    if (!codec || !type_name || !message_id || !worker_id || !status ||
        !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    if (!status->instance_id || status->instance_id[0] == '\0' ||
        status->connection_generation == 0 || status->max_sessions == 0 ||
        status->lease_duration_ms == 0 ||
        status->active_sessions > status->max_sessions ||
        status->reserved_sessions >
            status->max_sessions - status->active_sessions ||
        (status->draining != 0 && status->draining != 1) ||
        (status->health_ready != 0 && status->health_ready != 1)) {
        return IVR_EINVAL;
    }
    ivr_json_builder_init(&jb, json, sizeof(json));
    ivr_json_builder_raw(&jb, "{\"message_id\":");
    ivr_json_builder_string_cstr(&jb, message_id);
    ivr_json_builder_raw(&jb, ",\"worker_id\":");
    ivr_json_builder_string_cstr(&jb, worker_id);
    ivr_json_builder_raw(&jb, ",\"instance_id\":");
    ivr_json_builder_string_cstr(&jb, status->instance_id);
    ivr_json_builder_raw(&jb, ",\"connection_generation\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)status->connection_generation);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"max_sessions\":");
    snprintf(number, sizeof(number), "%u", status->max_sessions);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"active_sessions\":");
    snprintf(number, sizeof(number), "%u", status->active_sessions);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"reserved_sessions\":");
    snprintf(number, sizeof(number), "%u", status->reserved_sessions);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"lease_duration_ms\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)status->lease_duration_ms);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"draining\":");
    ivr_json_builder_raw(&jb, status->draining ? "true" : "false");
    ivr_json_builder_raw(&jb, ",\"health_generation\":");
    health_generation = status->health_generation ? status->health_generation : 1u;
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)health_generation);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"health_ready\":");
    health_ready = status->health_ready ||
                   (status->capabilities &&
                    strstr(status->capabilities, "health.ready") != NULL);
    ivr_json_builder_raw(&jb, health_ready ? "true" : "false");
    ivr_json_builder_raw(&jb, ",\"capabilities\":");
    ivr_json_builder_string_cstr(
        &jb, status->capabilities ? status->capabilities : "");
    ivr_json_builder_raw(&jb, "}");
    if (!ivr_json_builder_ok(&jb)) {
        return IVR_ENOSPC;
    }
    if (data_bind_object_from_json(codec, type_name, json, strlen(json), &obj,
                                   &err) != DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = type_id;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

ivr_status_t ivr_flowmq_gateway_encode_worker_sync_v2(
    DataBind *codec, const char *message_id, const char *worker_id,
    const ivr_worker_status_view_t *status, uint8_t *frame, size_t frame_cap,
    size_t *out_len) {
    return ivr_flowmq_gateway_encode_worker_status(
        codec, "WorkerSyncCommandV2", IVR_TYPE_WORKER_SYNC_COMMAND_V2,
        message_id, worker_id, status, frame, frame_cap, out_len);
}

ivr_status_t ivr_flowmq_gateway_encode_worker_heartbeat(
    DataBind *codec, const char *message_id, const char *worker_id,
    const ivr_worker_status_view_t *status, uint8_t *frame, size_t frame_cap,
    size_t *out_len) {
    return ivr_flowmq_gateway_encode_worker_status(
        codec, "WorkerHeartbeatV1", IVR_TYPE_WORKER_HEARTBEAT_V1, message_id,
        worker_id, status, frame, frame_cap, out_len);
}

static const char *dispatch_field(const DataBindValue *root,
                                  const char *field) {
    const DataBindValue *v = data_bind_value_get(root, field);
    const char *s = v ? data_bind_value_as_string(v) : NULL;
    return s ? s : "";
}

static uint64_t dispatch_u64(const DataBindValue *root, const char *field) {
    const DataBindValue *v = data_bind_value_get(root, field);
    return v ? data_bind_value_as_uint64(v) : 0;
}

static int dispatch_i32(const DataBindValue *root, const char *field) {
    const DataBindValue *v = data_bind_value_get(root, field);
    return v ? data_bind_value_as_int(v) : 0;
}

ivr_status_t ivr_flowmq_gateway_decode_dispatch(DataBind *codec,
                                                const uint8_t *frame,
                                                size_t len,
                                                ivr_call_dispatch_t *out) {
    if (!codec || !frame || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    ivr_frame_info_t info;
    if (ivr_frame_decode(frame, len, &info) != IVR_OK) {
        return IVR_ESTATE;
    }
    if (info.kind != IVR_KIND_COMMAND ||
        (info.schema_type_id != IVR_TYPE_CALL_DISPATCH_COMMAND_V1 &&
         info.schema_type_id != IVR_TYPE_CALL_DISPATCH_COMMAND_V2)) {
        return IVR_ESTATE; /* not a dispatch command */
    }
    const char *type_name =
        info.schema_type_id == IVR_TYPE_CALL_DISPATCH_COMMAND_V2
            ? "CallDispatchCommandV2"
            : "CallDispatchCommandV1";
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_bin(codec, type_name,
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return IVR_ESTATE;
    }
    const DataBindValue *root = data_bind_object_value(obj);
    out->wire_version =
        info.schema_type_id == IVR_TYPE_CALL_DISPATCH_COMMAND_V2 ? 2u : 1u;
    snprintf(out->message_id, sizeof(out->message_id), "%s",
             dispatch_field(root, "message_id"));
    snprintf(out->worker_id, sizeof(out->worker_id), "%s",
             dispatch_field(root, "worker_id"));
    snprintf(out->room_id, sizeof(out->room_id), "%s",
             dispatch_field(root, "room_id"));
    snprintf(out->call_id, sizeof(out->call_id), "%s",
             dispatch_field(root, "call_id"));
    out->call_generation = dispatch_u64(root, "call_generation");
    out->expected_room_version = dispatch_u64(root, "expected_room_version");
    snprintf(out->content_package, sizeof(out->content_package), "%s",
             dispatch_field(root, "content_package"));
    if (out->wire_version == 2u) {
        snprintf(out->assignment_id, sizeof(out->assignment_id), "%s",
                 dispatch_field(root, "assignment_id"));
        snprintf(out->attempt_id, sizeof(out->attempt_id), "%s",
                 dispatch_field(root, "attempt_id"));
        snprintf(out->worker_instance_id,
                 sizeof(out->worker_instance_id), "%s",
                 dispatch_field(root, "worker_instance_id"));
        out->worker_connection_generation =
            dispatch_u64(root, "worker_connection_generation");
        snprintf(out->content_version, sizeof(out->content_version), "%s",
                 dispatch_field(root, "content_version"));
        out->deadline_timeout_ms =
            dispatch_u64(root, "deadline_timeout_ms");
    }
    data_bind_object_free(obj);
    if (!out->message_id[0] || !out->worker_id[0] || !out->room_id[0] ||
        !out->call_id[0] || out->call_generation == 0 ||
        !out->content_package[0] ||
        (out->wire_version == 2u &&
         (!out->assignment_id[0] || !out->attempt_id[0] ||
          !out->worker_instance_id[0] ||
          out->worker_connection_generation == 0 ||
          out->deadline_timeout_ms == 0))) {
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    return IVR_OK;
}

int ivr_flowmq_gateway_dispatch_deadline_ok(
    const ivr_call_dispatch_t *dispatch, uint64_t received_at_ms,
    uint64_t now_ms) {
    uint64_t elapsed;
    if (!dispatch || dispatch->deadline_timeout_ms == 0) {
        return 0;
    }
    if (now_ms < received_at_ms) {
        /* Clock went backwards on the same clock source: fail closed. */
        return 0;
    }
    elapsed = now_ms - received_at_ms;
    return elapsed < dispatch->deadline_timeout_ms;
}

ivr_status_t ivr_flowmq_gateway_decode_release(DataBind *codec,
                                               const uint8_t *frame,
                                               size_t len,
                                               ivr_call_release_t *out) {
    ivr_frame_info_t info;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (!codec || !frame || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (ivr_frame_decode(frame, len, &info) != IVR_OK ||
        info.kind != IVR_KIND_COMMAND ||
        info.schema_type_id != IVR_TYPE_CALL_RELEASE_COMMAND_V1 ||
        data_bind_object_from_bin(codec, "CallReleaseCommandV1",
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return IVR_ESTATE;
    }
    const DataBindValue *root = data_bind_object_value(obj);
    snprintf(out->message_id, sizeof(out->message_id), "%s",
             dispatch_field(root, "message_id"));
    snprintf(out->worker_id, sizeof(out->worker_id), "%s",
             dispatch_field(root, "worker_id"));
    snprintf(out->room_id, sizeof(out->room_id), "%s",
             dispatch_field(root, "room_id"));
    snprintf(out->call_id, sizeof(out->call_id), "%s",
             dispatch_field(root, "call_id"));
    out->call_generation = dispatch_u64(root, "call_generation");
    snprintf(out->reason, sizeof(out->reason), "%s",
             dispatch_field(root, "reason"));
    data_bind_object_free(obj);
    return IVR_OK;
}

static ivr_status_t ivr_flowmq_gateway_encode_call_result(
    DataBind *codec, const char *type_name, uint16_t type_id,
    const char *message_id, const char *worker_id, const char *room_id,
    const char *call_id, uint64_t call_generation, int status_code,
    const char *error_code, const char *error_message, uint8_t *frame,
    size_t frame_cap, size_t *out_len) {
    if (!codec || !type_name || !message_id || !worker_id || !room_id ||
        !call_id || !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    char json[1024];
    char number[32];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, json, sizeof(json));
    ivr_json_builder_raw(&jb, "{\"message_id\":");
    ivr_json_builder_string_cstr(&jb, message_id);
    ivr_json_builder_raw(&jb, ",\"worker_id\":");
    ivr_json_builder_string_cstr(&jb, worker_id);
    ivr_json_builder_raw(&jb, ",\"room_id\":");
    ivr_json_builder_string_cstr(&jb, room_id);
    ivr_json_builder_raw(&jb, ",\"call_id\":");
    ivr_json_builder_string_cstr(&jb, call_id);
    ivr_json_builder_raw(&jb, ",\"call_generation\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)call_generation);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"status_code\":");
    snprintf(number, sizeof(number), "%d", status_code);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"error_code\":");
    ivr_json_builder_string_cstr(&jb, error_code ? error_code : "");
    ivr_json_builder_raw(&jb, ",\"error_message\":");
    ivr_json_builder_string_cstr(&jb, error_message ? error_message : "");
    ivr_json_builder_raw(&jb, "}");
    if (!ivr_json_builder_ok(&jb)) {
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_json(codec, type_name, json,
                                   strlen(json), &obj, &err) != DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_RESULT;
    info.schema_type_id = type_id;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

ivr_status_t ivr_flowmq_gateway_encode_dispatch_result(
    DataBind *codec, const ivr_call_dispatch_t *dispatch, int status_code,
    const char *error_code, const char *error_message, uint8_t *frame,
    size_t frame_cap, size_t *out_len) {
    if (!dispatch) {
        return IVR_EINVAL;
    }
    return ivr_flowmq_gateway_encode_call_result(
        codec, "CallDispatchResultV1", IVR_TYPE_CALL_DISPATCH_RESULT_V1,
        dispatch->message_id, dispatch->worker_id, dispatch->room_id,
        dispatch->call_id, dispatch->call_generation, status_code, error_code,
        error_message, frame, frame_cap, out_len);
}

ivr_status_t ivr_flowmq_gateway_encode_dispatch_result_v2(
    DataBind *codec, const ivr_call_dispatch_t *dispatch, int status_code,
    uint32_t active_sessions, uint32_t max_sessions,
    const char *error_code, const char *error_message, uint8_t *frame,
    size_t frame_cap, size_t *out_len) {
    if (!codec || !dispatch || dispatch->wire_version != 2u ||
        !dispatch->assignment_id[0] || !dispatch->attempt_id[0] ||
        !dispatch->worker_instance_id[0] ||
        dispatch->worker_connection_generation == 0 || max_sessions == 0 ||
        active_sessions > max_sessions || !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    char json[2048];
    char number[32];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, json, sizeof(json));
#define IVR_V2_RESULT_STRING(name, value)                                      \
    do {                                                                        \
        ivr_json_builder_raw(&jb, ",\"" name "\":");                        \
        ivr_json_builder_string_cstr(&jb, (value));                             \
    } while (0)
    ivr_json_builder_raw(&jb, "{\"message_id\":");
    ivr_json_builder_string_cstr(&jb, dispatch->message_id);
    IVR_V2_RESULT_STRING("assignment_id", dispatch->assignment_id);
    IVR_V2_RESULT_STRING("attempt_id", dispatch->attempt_id);
    IVR_V2_RESULT_STRING("worker_id", dispatch->worker_id);
    IVR_V2_RESULT_STRING("worker_instance_id", dispatch->worker_instance_id);
    ivr_json_builder_raw(&jb, ",\"worker_connection_generation\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)dispatch->worker_connection_generation);
    ivr_json_builder_raw(&jb, number);
    IVR_V2_RESULT_STRING("room_id", dispatch->room_id);
    IVR_V2_RESULT_STRING("call_id", dispatch->call_id);
    ivr_json_builder_raw(&jb, ",\"call_generation\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)dispatch->call_generation);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"status_code\":");
    snprintf(number, sizeof(number), "%d", status_code);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"active_sessions\":");
    snprintf(number, sizeof(number), "%u", active_sessions);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"max_sessions\":");
    snprintf(number, sizeof(number), "%u", max_sessions);
    ivr_json_builder_raw(&jb, number);
    IVR_V2_RESULT_STRING("error_code", error_code ? error_code : "");
    IVR_V2_RESULT_STRING("error_message", error_message ? error_message : "");
    ivr_json_builder_raw(&jb, "}");
#undef IVR_V2_RESULT_STRING
    if (!ivr_json_builder_ok(&jb)) {
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_json(codec, "CallDispatchResultV2", json,
                                   strlen(json), &obj, &err) != DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_RESULT;
    info.schema_type_id = IVR_TYPE_CALL_DISPATCH_RESULT_V2;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

ivr_status_t ivr_flowmq_gateway_encode_release_result(
    DataBind *codec, const ivr_call_release_t *release, int status_code,
    const char *error_code, const char *error_message, uint8_t *frame,
    size_t frame_cap, size_t *out_len) {
    if (!release) {
        return IVR_EINVAL;
    }
    return ivr_flowmq_gateway_encode_call_result(
        codec, "CallReleaseResultV1", IVR_TYPE_CALL_RELEASE_RESULT_V1,
        release->message_id, release->worker_id, release->room_id,
        release->call_id, release->call_generation, status_code, error_code,
        error_message, frame, frame_cap, out_len);
}

ivr_status_t ivr_flowmq_gateway_send_dispatch_result(
    ivr_flowmq_gateway_t *gateway, const ivr_call_dispatch_t *dispatch,
    int status_code, const char *error_code, const char *error_message) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4096u];
    size_t len = 0;
    if (!gateway || !gateway->app || !dispatch) {
        return IVR_EINVAL;
    }
    if (ivr_flowmq_gateway_encode_dispatch_result(
            gateway->codec, dispatch, status_code, error_code, error_message,
            frame, sizeof(frame), &len) != IVR_OK) {
        return IVR_ESTATE;
    }
    return turbo_flow_fmq_app_send(gateway->app, frame, len) == TURBO_OK
               ? IVR_OK
               : IVR_ENOSPC;
}

ivr_status_t ivr_flowmq_gateway_send_dispatch_result_v2(
    ivr_flowmq_gateway_t *gateway, const ivr_call_dispatch_t *dispatch,
    int status_code, uint32_t active_sessions, uint32_t max_sessions,
    const char *error_code, const char *error_message) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4096u];
    size_t len = 0;
    if (!gateway || !gateway->app || !dispatch) {
        return IVR_EINVAL;
    }
    if (ivr_flowmq_gateway_encode_dispatch_result_v2(
            gateway->codec, dispatch, status_code, active_sessions,
            max_sessions, error_code, error_message, frame, sizeof(frame),
            &len) != IVR_OK) {
        return IVR_ESTATE;
    }
    return turbo_flow_fmq_app_send(gateway->app, frame, len) == TURBO_OK
               ? IVR_OK
               : IVR_ENOSPC;
}

ivr_status_t ivr_flowmq_gateway_send_release_result(
    ivr_flowmq_gateway_t *gateway, const ivr_call_release_t *release,
    int status_code, const char *error_code, const char *error_message) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4096u];
    size_t len = 0;
    if (!gateway || !gateway->app || !release) {
        return IVR_EINVAL;
    }
    if (ivr_flowmq_gateway_encode_release_result(
            gateway->codec, release, status_code, error_code, error_message,
            frame, sizeof(frame), &len) != IVR_OK) {
        return IVR_ESTATE;
    }
    return turbo_flow_fmq_app_send(gateway->app, frame, len) == TURBO_OK
               ? IVR_OK
               : IVR_ENOSPC;
}

ivr_status_t ivr_flowmq_gateway_decode_result(
    DataBind *codec, const uint8_t *frame, size_t len,
    ivr_command_result_envelope_t *out) {
    ivr_frame_info_t info;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    const DataBindValue *root;

    if (!codec || !frame || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (ivr_frame_decode(frame, len, &info) != IVR_OK ||
        info.kind != IVR_KIND_RESULT ||
        info.schema_type_id != IVR_TYPE_IVR_COMMAND_RESULT_V1) {
        return IVR_ESTATE;
    }
    if (data_bind_object_from_bin(codec, "IvrCommandResultV1",
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return IVR_ESTATE;
    }
    root = data_bind_object_value(obj);
    snprintf(out->message_id, sizeof(out->message_id), "%s",
             dispatch_field(root, "message_id"));
    snprintf(out->worker_id, sizeof(out->worker_id), "%s",
             dispatch_field(root, "worker_id"));
    snprintf(out->room_id, sizeof(out->room_id), "%s",
             dispatch_field(root, "room_id"));
    snprintf(out->call_id, sizeof(out->call_id), "%s",
             dispatch_field(root, "call_id"));
    out->call_generation = dispatch_u64(root, "call_generation");
    out->status_code = dispatch_i32(root, "status_code");
    out->room_version = dispatch_u64(root, "room_version");
    out->sequence = dispatch_u64(root, "sequence");
    snprintf(out->error_code, sizeof(out->error_code), "%s",
             dispatch_field(root, "error_code"));
    snprintf(out->error_message, sizeof(out->error_message), "%s",
             dispatch_field(root, "error_message"));
    data_bind_object_free(obj);
    return IVR_OK;
}

/* ------------------------------------------------------------------ */
/* DEALER reply ingress                                                 */
/* ------------------------------------------------------------------ */

static int flowmq_on_message(turbo_flow_fmq_app_t *app,
                             turbo_flow_msg_t *message, void *ctx) {
    ivr_flowmq_gateway_t *g = (ivr_flowmq_gateway_t *)ctx;
    (void)app;
    if (!g || !g->on_reply || !message || message->payload.len == 0) {
        return TURBO_OK;
    }
    g->on_reply(g->reply_ctx, (const uint8_t *)message->payload.data,
                message->payload.len);
    return TURBO_OK;
}

static void flowmq_on_connection_event(
    void *ctx, const turbo_flow_fmq_event_t *event) {
    ivr_flowmq_gateway_t *gateway = (ivr_flowmq_gateway_t *)ctx;
    int connected;
    if (!gateway || !gateway->on_connection || !event) {
        return;
    }
    switch (event->kind) {
    case TURBO_FLOW_FMQ_EVENT_PEER_CONNECTED:
    case TURBO_FLOW_FMQ_EVENT_RECONNECT_SUCCEEDED:
        connected = 1;
        break;
    case TURBO_FLOW_FMQ_EVENT_PEER_DISCONNECTED:
    case TURBO_FLOW_FMQ_EVENT_RECONNECT_SCHEDULED:
    case TURBO_FLOW_FMQ_EVENT_RECONNECT_FAILED:
    case TURBO_FLOW_FMQ_EVENT_HEARTBEAT_TIMEOUT:
    case TURBO_FLOW_FMQ_EVENT_AUTHENTICATION_FAILED:
        connected = 0;
        break;
    default:
        return;
    }
    gateway->on_connection(gateway->connection_ctx, connected);
}

/* ------------------------------------------------------------------ */
/* ivr_command_gateway_ops_t                                           */
/* ------------------------------------------------------------------ */

static ivr_status_t flowmq_submit_copy(void *context,
                                       const ivr_command_view_t *command) {
    ivr_flowmq_gateway_t *g = (ivr_flowmq_gateway_t *)context;
    if (!g || !g->app || !command) {
        return IVR_EINVAL;
    }
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 64u * 1024u];
    size_t len = 0;
    ivr_status_t rc = ivr_flowmq_gateway_encode_command(
        g->codec, command, g->worker_id, frame, sizeof(frame), &len);
    if (rc != IVR_OK) {
        g->rejected_count++;
        return rc;
    }
    int send_rc = turbo_flow_fmq_app_send(g->app, frame, len);
    if (send_rc != TURBO_OK) {
        g->rejected_count++;
        return IVR_ENOSPC;
    }
    g->sent_count++;
    return IVR_OK;
}

ivr_status_t ivr_flowmq_gateway_send_worker_sync(
    ivr_flowmq_gateway_t *gateway, const char *message_id) {
    if (!gateway || !gateway->app || !message_id) {
        return IVR_EINVAL;
    }
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4u * 1024u];
    size_t len = 0;
    if (ivr_flowmq_gateway_encode_worker_sync(gateway->codec, message_id,
                                              gateway->worker_id, frame,
                                              sizeof(frame), &len) != IVR_OK) {
        return IVR_ESTATE;
    }
    if (turbo_flow_fmq_app_send(gateway->app, frame, len) != TURBO_OK) {
        return IVR_ENOSPC;
    }
    return IVR_OK;
}

static ivr_status_t ivr_flowmq_gateway_send_worker_status(
    ivr_flowmq_gateway_t *gateway, const char *message_id,
    const ivr_worker_status_view_t *status, int heartbeat) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4u * 1024u];
    size_t len = 0;
    ivr_status_t rc;
    if (!gateway || !gateway->app || !message_id || !status) {
        return IVR_EINVAL;
    }
    rc = heartbeat ? ivr_flowmq_gateway_encode_worker_heartbeat(
                         gateway->codec, message_id, gateway->worker_id,
                         status, frame, sizeof(frame), &len)
                   : ivr_flowmq_gateway_encode_worker_sync_v2(
                         gateway->codec, message_id, gateway->worker_id,
                         status, frame, sizeof(frame), &len);
    if (rc != IVR_OK) {
        return rc;
    }
    return turbo_flow_fmq_app_send(gateway->app, frame, len) == TURBO_OK
               ? IVR_OK
               : IVR_ENOSPC;
}

ivr_status_t ivr_flowmq_gateway_send_worker_sync_v2(
    ivr_flowmq_gateway_t *gateway, const char *message_id,
    const ivr_worker_status_view_t *status) {
    return ivr_flowmq_gateway_send_worker_status(gateway, message_id, status,
                                                  0);
}

ivr_status_t ivr_flowmq_gateway_send_worker_heartbeat(
    ivr_flowmq_gateway_t *gateway, const char *message_id,
    const ivr_worker_status_view_t *status) {
    return ivr_flowmq_gateway_send_worker_status(gateway, message_id, status,
                                                  1);
}

ivr_status_t ivr_flowmq_gateway_send_frame(ivr_flowmq_gateway_t *gateway,
                                            const uint8_t *frame, size_t len) {
    if (!gateway || !gateway->app || (!frame && len > 0)) {
        return IVR_EINVAL;
    }
    if (turbo_flow_fmq_app_send(gateway->app, frame, len) != TURBO_OK) {
        return IVR_ENOSPC;
    }
    return IVR_OK;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

ivr_status_t ivr_flowmq_gateway_create(const ivr_flowmq_gateway_config_t *config,
                                       ivr_command_gateway_ops_t *ops,
                                       ivr_flowmq_gateway_t **out_gateway) {
    if (!config || !config->worker_id || !config->host || config->port <= 0 ||
        !ops || !out_gateway) {
        return IVR_EINVAL;
    }
    ivr_flowmq_gateway_t *g =
        (ivr_flowmq_gateway_t *)calloc(1, sizeof(*g));
    if (!g) {
        return IVR_ENOSPC;
    }
    if (strlen(config->worker_id) >= sizeof(g->worker_id)) {
        free(g);
        return IVR_EINVAL;
    }
    snprintf(g->worker_id, sizeof(g->worker_id), "%s", config->worker_id);
    DataBindError err = DATA_BIND_ERROR_INIT;
    if (TurboMediaIvrV1_codec_create(&g->codec, &err) != DATA_BIND_OK) {
        free(g);
        return IVR_ENOSPC;
    }

    turbo_flow_fmq_config_t ep = TURBO_FLOW_FMQ_CONFIG_INIT;
    ep.pattern = TURBO_FLOW_FMQ_DEALER;
    ep.mode = TURBO_FLOW_FMQ_CONNECT;
    ep.transport = config->transport ? config->transport : TURBO_FLOW_FMQ_TCP;
    ep.host = config->host;
    ep.port = config->port;
    ep.identity = g->worker_id; /* required, unique DEALER identity */
    ep.max_frame_size = TURBO_FLOW_FMQ_DEFAULT_MAX_FRAME_SIZE;
    ep.timeout_ms = config->timeout_ms ? config->timeout_ms : 5000;
    ep.reconnect_initial_ms =
        config->reconnect_initial_ms ? config->reconnect_initial_ms : 1000;
    ep.reconnect_max_ms =
        config->reconnect_max_ms ? config->reconnect_max_ms : 30000;
    ep.tls = config->tls;
    ep.path = config->path ? config->path : "/";
    ep.event_callback = flowmq_on_connection_event;
    ep.event_ctx = g;

    g->on_reply = config->on_reply;
    g->reply_ctx = config->reply_ctx;
    g->on_connection = config->on_connection;
    g->connection_ctx = config->connection_ctx;
    turbo_flow_fmq_app_options_t opt = TURBO_FLOW_FMQ_APP_OPTIONS_INIT;
    /* bidirectional facade: ingress (DEALER replies) is forwarded to the
       optional on_reply callback when provided */
    opt.on_message = flowmq_on_message;
    opt.message_ctx = g;
    int rc = config->security
                 ? turbo_flow_fmq_app_create_secure(&ep, &opt, config->security,
                                                    &g->app)
                 : turbo_flow_fmq_app_create(&ep, &opt, &g->app);
    if (rc != TURBO_OK) {
        data_bind_free(g->codec);
        free(g);
        return IVR_ENOSPC;
    }

    g->ops.abi_version = 1;
    g->ops.context = g;
    g->ops.submit_copy = flowmq_submit_copy;
    *ops = g->ops;
    *out_gateway = g;
    return IVR_OK;
}

ivr_status_t ivr_flowmq_gateway_start(ivr_flowmq_gateway_t *gateway) {
    if (!gateway || !gateway->app) {
        return IVR_EINVAL;
    }
    return turbo_flow_fmq_app_start(gateway->app) == TURBO_OK ? IVR_OK
                                                               : IVR_ESTATE;
}

void ivr_flowmq_gateway_destroy(ivr_flowmq_gateway_t *gateway) {
    if (!gateway) {
        return;
    }
    if (gateway->app) {
        turbo_flow_fmq_app_destroy(gateway->app);
        gateway->app = NULL;
    }
    if (gateway->codec) {
        data_bind_free(gateway->codec);
        gateway->codec = NULL;
    }
    free(gateway);
}
