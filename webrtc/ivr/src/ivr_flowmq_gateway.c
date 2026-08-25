#include "ivr_flowmq_gateway.h"
#include "ivr_internal.h"
#include "ivr_frame.h"
#include "turbomedia_ivr_v1.h"
#include "flowmq_connect_endpoint.h"
#include "flowmq_protocol.h"
#include "turbo_error.h"
#include "turbo_str.h"
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define IVR_INVENTORY_WIRE_JSON_CAPACITY (128u * 1024u)
#define IVR_INVENTORY_WIRE_FRAME_CAPACITY                              \
    (IVR_FRAME_HEADER_SIZE + IVR_INVENTORY_WIRE_JSON_CAPACITY)

struct ivr_flowmq_gateway_s {
    flowmq_connect_endpoint_t *endpoint;
    DataBind *codec;
    char worker_id[128];
    ivr_command_gateway_ops_t ops;
    void (*on_reply)(void *ctx, const uint8_t *frame, size_t len);
    void *reply_ctx;
    void (*on_connection)(void *ctx, int connected);
    void *connection_ctx;
    uint64_t sent_count;
    uint64_t rejected_count;
    uint64_t start_timeout_ns;
    atomic_uint_fast64_t next_completion_id;
};

static int flowmq_gateway_send_payload(ivr_flowmq_gateway_t *gateway,
                                       const uint8_t *payload,
                                       size_t payload_size) {
    flowmq_protocol_frame_t frame;
    uint64_t completion_id;
    tstr encoded = NULL;
    int rc;
    if (!gateway || !gateway->endpoint || (!payload && payload_size > 0u)) {
        return TURBO_EINVAL;
    }
    memset(&frame, 0, sizeof(frame));
    frame.kind = FLOWMQ_PROTOCOL_FRAME_DATA;
    frame.pattern = FLOWMQ_PROTOCOL_DEALER;
    completion_id = atomic_fetch_add_explicit(
        &gateway->next_completion_id, 1u, memory_order_relaxed);
    if (completion_id == 0u) {
        completion_id = atomic_fetch_add_explicit(
            &gateway->next_completion_id, 1u, memory_order_relaxed);
    }
    frame.message_id = completion_id;
    frame.payload = vstr_from_buf((const char *)payload, payload_size);
    rc = flowmq_protocol_encode_frame(
        &frame, FLOWMQ_CONNECT_ENDPOINT_DEFAULT_MAX_FRAME_SIZE, &encoded);
    if (rc == TURBO_OK) {
        rc = flowmq_connect_endpoint_send_copy(
            gateway->endpoint, completion_id, encoded, tstr_len(encoded));
    }
    tstr_free(encoded);
    return rc;
}

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

static int copy_schema_string(const DataBindValue *root, const char *field,
                              char *out, size_t out_size, int required) {
    const char *value = dispatch_field(root, field);
    size_t size = strlen(value);
    if (!out || out_size == 0 || size >= out_size || (required && size == 0)) {
        return -1;
    }
    memcpy(out, value, size + 1);
    return 0;
}

static ivr_status_t encode_typed_frame(
    DataBind *codec, const char *type_name, uint16_t type_id, uint8_t kind,
    const char *json, uint8_t *frame, size_t frame_capacity,
    size_t *out_size) {
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    uint8_t *binary = NULL;
    size_t binary_size = 0;

    if (!codec || !type_name || !json || !frame || !out_size ||
        frame_capacity < IVR_FRAME_HEADER_SIZE) {
        return IVR_EINVAL;
    }
    *out_size = 0;
    if (data_bind_object_from_json(codec, type_name, json, strlen(json),
                                   &object, &error) != DATA_BIND_OK ||
        data_bind_object_serialize_bin(codec, object, &binary, &binary_size,
                                       &error) != DATA_BIND_OK) {
        data_bind_object_free(object);
        return IVR_ESTATE;
    }
    data_bind_object_free(object);
    if (binary_size > frame_capacity - IVR_FRAME_HEADER_SIZE) {
        data_bind_binary_free(binary);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = kind;
    info.schema_type_id = type_id;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(binary);
        return IVR_ESTATE;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, binary, binary_size);
    *out_size = IVR_FRAME_HEADER_SIZE + binary_size;
    data_bind_binary_free(binary);
    return IVR_OK;
}

ivr_status_t ivr_flowmq_gateway_decode_media_command(
    DataBind *codec, const uint8_t *frame, size_t len,
    ivr_media_command_t *out) {
    ivr_frame_info_t info;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    const DataBindValue *root;
    const char *type_name;

    if (!codec || !frame || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (ivr_frame_decode(frame, len, &info) != IVR_OK ||
        info.kind != IVR_KIND_COMMAND || info.format != IVR_FMT_BIN) {
        return IVR_ESTATE;
    }
    switch (info.schema_type_id) {
        case IVR_TYPE_MEDIA_SESSION_OPEN_COMMAND_V1:
            out->kind = IVR_MEDIA_COMMAND_SESSION_OPEN;
            type_name = "MediaSessionOpenCommandV1";
            break;
        case IVR_TYPE_MEDIA_PLAY_COMMAND_V1:
            out->kind = IVR_MEDIA_COMMAND_PLAY;
            type_name = "MediaPlayCommandV1";
            break;
        case IVR_TYPE_MEDIA_INPUT_START_COMMAND_V1:
            out->kind = IVR_MEDIA_COMMAND_INPUT_START;
            type_name = "MediaInputStartCommandV1";
            break;
        case IVR_TYPE_MEDIA_INPUT_STOP_COMMAND_V1:
            out->kind = IVR_MEDIA_COMMAND_INPUT_STOP;
            type_name = "MediaInputStopCommandV1";
            break;
        case IVR_TYPE_MEDIA_CANCEL_COMMAND_V2:
            out->kind = IVR_MEDIA_COMMAND_CANCEL;
            type_name = "MediaCancelCommandV2";
            break;
        case IVR_TYPE_MEDIA_SESSION_CLOSE_COMMAND_V1:
            out->kind = IVR_MEDIA_COMMAND_SESSION_CLOSE;
            type_name = "MediaSessionCloseCommandV1";
            break;
        default:
            return IVR_ESTATE;
    }
    if (data_bind_object_from_bin(codec, type_name,
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &object,
                                  &error) != DATA_BIND_OK) {
        return IVR_ESTATE;
    }
    root = data_bind_object_value(object);
    if (copy_schema_string(root, "message_id", out->message_id,
                           sizeof(out->message_id), 1) != 0 ||
        copy_schema_string(root, "tenant_id", out->tenant_id,
                           sizeof(out->tenant_id), 1) != 0 ||
        copy_schema_string(root, "provider_session_id",
                           out->provider_session_id,
                           sizeof(out->provider_session_id), 1) != 0 ||
        copy_schema_string(root, "dialog_id", out->dialog_id,
                           sizeof(out->dialog_id), 1) != 0 ||
        copy_schema_string(root, "worker_id", out->worker_id,
                           sizeof(out->worker_id), 1) != 0 ||
        copy_schema_string(root, "room_id", out->room_id,
                           sizeof(out->room_id), 1) != 0 ||
        copy_schema_string(root, "call_id", out->call_id,
                           sizeof(out->call_id), 1) != 0) {
        data_bind_object_free(object);
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    out->call_generation = dispatch_u64(root, "call_generation");
    out->operation_generation = dispatch_u64(root, "operation_generation");
    out->deadline_timeout_ms = dispatch_u64(root, "deadline_timeout_ms");
    if (out->kind == IVR_MEDIA_COMMAND_PLAY &&
        copy_schema_string(root, "text", out->text, sizeof(out->text), 1) != 0) {
        data_bind_object_free(object);
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    if ((out->kind == IVR_MEDIA_COMMAND_INPUT_START ||
         out->kind == IVR_MEDIA_COMMAND_INPUT_STOP ||
         out->kind == IVR_MEDIA_COMMAND_CANCEL) &&
        (copy_schema_string(root, "input_id", out->input_id,
                            sizeof(out->input_id), 1) != 0 ||
         (out->input_generation = dispatch_u64(root, "input_generation")) == 0)) {
        data_bind_object_free(object);
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    if (out->kind == IVR_MEDIA_COMMAND_SESSION_CLOSE &&
        copy_schema_string(root, "reason", out->reason, sizeof(out->reason),
                           0) != 0) {
        data_bind_object_free(object);
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    data_bind_object_free(object);
    if (out->call_generation == 0 || out->operation_generation == 0 ||
        out->deadline_timeout_ms == 0) {
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    return IVR_OK;
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
    if (!gateway || !gateway->endpoint || !dispatch) {
        return IVR_EINVAL;
    }
    if (ivr_flowmq_gateway_encode_dispatch_result(
            gateway->codec, dispatch, status_code, error_code, error_message,
            frame, sizeof(frame), &len) != IVR_OK) {
        return IVR_ESTATE;
    }
    return flowmq_gateway_send_payload(gateway, frame, len) == TURBO_OK
               ? IVR_OK
               : IVR_ENOSPC;
}

ivr_status_t ivr_flowmq_gateway_send_dispatch_result_v2(
    ivr_flowmq_gateway_t *gateway, const ivr_call_dispatch_t *dispatch,
    int status_code, uint32_t active_sessions, uint32_t max_sessions,
    const char *error_code, const char *error_message) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4096u];
    size_t len = 0;
    if (!gateway || !gateway->endpoint || !dispatch) {
        return IVR_EINVAL;
    }
    if (ivr_flowmq_gateway_encode_dispatch_result_v2(
            gateway->codec, dispatch, status_code, active_sessions,
            max_sessions, error_code, error_message, frame, sizeof(frame),
            &len) != IVR_OK) {
        return IVR_ESTATE;
    }
    return flowmq_gateway_send_payload(gateway, frame, len) == TURBO_OK
               ? IVR_OK
               : IVR_ENOSPC;
}

ivr_status_t ivr_flowmq_gateway_send_release_result(
    ivr_flowmq_gateway_t *gateway, const ivr_call_release_t *release,
    int status_code, const char *error_code, const char *error_message) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4096u];
    size_t len = 0;
    if (!gateway || !gateway->endpoint || !release) {
        return IVR_EINVAL;
    }
    if (ivr_flowmq_gateway_encode_release_result(
            gateway->codec, release, status_code, error_code, error_message,
            frame, sizeof(frame), &len) != IVR_OK) {
        return IVR_ESTATE;
    }
    return flowmq_gateway_send_payload(gateway, frame, len) == TURBO_OK
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

static int flowmq_on_message(void *ctx,
                             const flowmq_protocol_frame_t *message,
                             uint64_t generation) {
    ivr_flowmq_gateway_t *g = (ivr_flowmq_gateway_t *)ctx;
    (void)generation;
    if (!g || !g->on_reply || !message ||
        message->kind != FLOWMQ_PROTOCOL_FRAME_DATA ||
        message->payload.len == 0u) {
        return TURBO_OK;
    }
    g->on_reply(g->reply_ctx, (const uint8_t *)message->payload.data,
                message->payload.len);
    return TURBO_OK;
}

static void flowmq_on_connection_state(
    void *ctx, flowmq_connect_endpoint_connection_state_t state,
    int status, size_t connections_current) {
    ivr_flowmq_gateway_t *gateway = (ivr_flowmq_gateway_t *)ctx;
    (void)status;
    if (!gateway || !gateway->on_connection) return;
    gateway->on_connection(
        gateway->connection_ctx,
        state == FLOWMQ_ENDPOINT_CONNECTION_READY && connections_current > 0u);
}

/* ------------------------------------------------------------------ */
/* ivr_command_gateway_ops_t                                           */
/* ------------------------------------------------------------------ */

static ivr_status_t flowmq_submit_copy(void *context,
                                       const ivr_command_view_t *command) {
    ivr_flowmq_gateway_t *g = (ivr_flowmq_gateway_t *)context;
    if (!g || !g->endpoint || !command) {
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
    int send_rc = flowmq_gateway_send_payload(g, frame, len);
    if (send_rc != TURBO_OK) {
        g->rejected_count++;
        return IVR_ENOSPC;
    }
    g->sent_count++;
    return IVR_OK;
}

ivr_status_t ivr_flowmq_gateway_send_worker_sync(
    ivr_flowmq_gateway_t *gateway, const char *message_id) {
    if (!gateway || !gateway->endpoint || !message_id) {
        return IVR_EINVAL;
    }
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4u * 1024u];
    size_t len = 0;
    if (ivr_flowmq_gateway_encode_worker_sync(gateway->codec, message_id,
                                              gateway->worker_id, frame,
                                              sizeof(frame), &len) != IVR_OK) {
        return IVR_ESTATE;
    }
    if (flowmq_gateway_send_payload(gateway, frame, len) != TURBO_OK) {
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
    if (!gateway || !gateway->endpoint || !message_id || !status) {
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
    return flowmq_gateway_send_payload(gateway, frame, len) == TURBO_OK
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
    if (!gateway || !gateway->endpoint || (!frame && len > 0)) {
        return IVR_EINVAL;
    }
    if (flowmq_gateway_send_payload(gateway, frame, len) != TURBO_OK) {
        return IVR_ENOSPC;
    }
    return IVR_OK;
}

ivr_status_t ivr_flowmq_gateway_send_media_result(
    ivr_flowmq_gateway_t *gateway, const ivr_media_command_t *command,
    int status_code, const char *error_code, const char *error_message) {
    char json[2048];
    char number[32];
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4096u];
    size_t frame_size = 0;
    ivr_json_builder_t builder;
    ivr_status_t status;

    if (!gateway || !gateway->endpoint || !command || !error_code ||
        !error_message) {
        return IVR_EINVAL;
    }
    ivr_json_builder_init(&builder, json, sizeof(json));
#define MEDIA_RESULT_STRING(name, value)                                      \
    do {                                                                       \
        ivr_json_builder_raw(&builder, ",\"" name "\":");                \
        ivr_json_builder_string_cstr(&builder, (value));                       \
    } while (0)
    ivr_json_builder_raw(&builder, "{\"message_id\":");
    ivr_json_builder_string_cstr(&builder, command->message_id);
    MEDIA_RESULT_STRING("tenant_id", command->tenant_id);
    MEDIA_RESULT_STRING("provider_session_id", command->provider_session_id);
    MEDIA_RESULT_STRING("dialog_id", command->dialog_id);
    MEDIA_RESULT_STRING("worker_id", command->worker_id);
    MEDIA_RESULT_STRING("room_id", command->room_id);
    MEDIA_RESULT_STRING("call_id", command->call_id);
    ivr_json_builder_raw(&builder, ",\"call_generation\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)command->call_generation);
    ivr_json_builder_raw(&builder, number);
    ivr_json_builder_raw(&builder, ",\"operation_generation\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)command->operation_generation);
    ivr_json_builder_raw(&builder, number);
    ivr_json_builder_raw(&builder, ",\"status_code\":");
    snprintf(number, sizeof(number), "%d", status_code);
    ivr_json_builder_raw(&builder, number);
    MEDIA_RESULT_STRING("error_code", error_code);
    MEDIA_RESULT_STRING("error_message", error_message);
    ivr_json_builder_raw(&builder, "}");
#undef MEDIA_RESULT_STRING
    if (!ivr_json_builder_ok(&builder)) {
        return IVR_ENOSPC;
    }
    status = encode_typed_frame(
        gateway->codec, "MediaCommandResultV1",
        IVR_TYPE_MEDIA_COMMAND_RESULT_V1, IVR_KIND_RESULT, json, frame,
        sizeof(frame), &frame_size);
    if (status != IVR_OK) {
        return status;
    }
    return ivr_flowmq_gateway_send_frame(gateway, frame, frame_size);
}

ivr_status_t ivr_flowmq_gateway_send_media_event(
    ivr_flowmq_gateway_t *gateway, const char *worker_id,
    const ivr_event_view_t *event, uint64_t occurred_at_ms) {
    char json[8192];
    char number[32];
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 8192u];
    size_t frame_size = 0;
    ivr_json_builder_t builder;
    ivr_status_t status;

    if (!gateway || !gateway->endpoint || !worker_id || !worker_id[0] || !event ||
        !event->call.tenant_id.data || event->call.tenant_id.size == 0 ||
        !event->call.provider_session_id.data ||
        event->call.provider_session_id.size == 0 ||
        !event->call.dialog_id.data || event->call.dialog_id.size == 0 ||
        !event->event_type.data || event->event_type.size == 0) {
        return IVR_EINVAL;
    }
    ivr_json_builder_init(&builder, json, sizeof(json));
#define MEDIA_EVENT_VIEW(name, view)                                          \
    do {                                                                       \
        ivr_json_builder_raw(&builder, ",\"" name "\":");                \
        ivr_json_builder_string(&builder, (view).data, (view).size);           \
    } while (0)
    ivr_json_builder_raw(&builder, "{\"event_id\":");
    ivr_json_builder_string(&builder, event->event_id.data,
                            event->event_id.size);
    MEDIA_EVENT_VIEW("tenant_id", event->call.tenant_id);
    MEDIA_EVENT_VIEW("provider_session_id", event->call.provider_session_id);
    MEDIA_EVENT_VIEW("dialog_id", event->call.dialog_id);
    ivr_json_builder_raw(&builder, ",\"worker_id\":");
    ivr_json_builder_string_cstr(&builder, worker_id);
    MEDIA_EVENT_VIEW("room_id", event->call.room_id);
    MEDIA_EVENT_VIEW("call_id", event->call.call_id);
    ivr_json_builder_raw(&builder, ",\"call_generation\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)event->call.call_generation);
    ivr_json_builder_raw(&builder, number);
    ivr_json_builder_raw(&builder, ",\"sequence\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)event->sequence);
    ivr_json_builder_raw(&builder, number);
    MEDIA_EVENT_VIEW("event_type", event->event_type);
    ivr_json_builder_raw(&builder, ",\"occurred_at_ms\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)occurred_at_ms);
    ivr_json_builder_raw(&builder, number);
    MEDIA_EVENT_VIEW("input_id", event->input_id);
    MEDIA_EVENT_VIEW("input_value", event->input_value);
    MEDIA_EVENT_VIEW("payload_json", event->payload_json);
    ivr_json_builder_raw(&builder, "}");
#undef MEDIA_EVENT_VIEW
    if (!ivr_json_builder_ok(&builder)) {
        return IVR_ENOSPC;
    }
    status = encode_typed_frame(gateway->codec, "MediaEventV1",
                                IVR_TYPE_MEDIA_EVENT_V1, IVR_KIND_EVENT, json,
                                frame, sizeof(frame), &frame_size);
    if (status != IVR_OK) {
        return status;
    }
    return ivr_flowmq_gateway_send_frame(gateway, frame, frame_size);
}

ivr_status_t ivr_flowmq_gateway_decode_inventory_query(
    DataBind *codec, const uint8_t *frame, size_t len,
    ivr_worker_inventory_request_t *out) {
    ivr_frame_info_t info;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    const DataBindValue *root;
    uint64_t inventory_version;
    uint64_t cursor;
    uint64_t limit;

    if (!codec || !frame || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (ivr_frame_decode(frame, len, &info) != IVR_OK ||
        info.format != IVR_FMT_BIN || info.kind != IVR_KIND_COMMAND ||
        info.schema_type_id != IVR_TYPE_WORKER_MEDIA_INVENTORY_QUERY_V1 ||
        data_bind_object_from_bin(codec, "WorkerMediaInventoryQueryV1",
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &object,
                                  &error) != DATA_BIND_OK) {
        return IVR_ESTATE;
    }
    root = data_bind_object_value(object);
    inventory_version = dispatch_u64(root, "inventory_version");
    cursor = dispatch_u64(root, "cursor");
    limit = dispatch_u64(root, "limit");
    if (copy_schema_string(root, "message_id", out->message_id,
                           sizeof(out->message_id), 1) != 0 ||
        copy_schema_string(root, "worker_id", out->worker_id,
                           sizeof(out->worker_id), 1) != 0 ||
        inventory_version > UINT32_MAX || cursor > UINT32_MAX ||
        limit > UINT32_MAX) {
        data_bind_object_free(object);
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    out->query.inventory_version = (uint32_t)inventory_version;
    out->query.expected_revision =
        dispatch_u64(root, "expected_revision");
    out->query.cursor = (uint32_t)cursor;
    out->query.limit = (uint32_t)limit;
    data_bind_object_free(object);
    if (out->query.inventory_version != IVR_WORKER_INVENTORY_VERSION) {
        return IVR_EVERSION;
    }
    if (out->query.limit == 0 ||
        out->query.limit > IVR_WORKER_INVENTORY_MAX_PAGE_SIZE) {
        return IVR_EINVAL;
    }
    return IVR_OK;
}

static const char *inventory_state_name(ivr_worker_resource_state_t state) {
    switch (state) {
        case IVR_WORKER_RESOURCE_OPENING:
            return "opening";
        case IVR_WORKER_RESOURCE_ACTIVE:
            return "active";
        case IVR_WORKER_RESOURCE_CLOSING:
            return "closing";
        default:
            return NULL;
    }
}

static int inventory_page_valid(
    const ivr_worker_inventory_envelope_t *result) {
    if (!result || !result->message_id[0] || !result->worker_id[0] ||
        result->status_code > 0) {
        return 0;
    }
    if (result->status_code != IVR_OK) {
        return result->page.count == 0;
    }
    if (result->page.inventory_version != IVR_WORKER_INVENTORY_VERSION ||
        result->page.revision == 0 ||
        result->page.count > IVR_WORKER_INVENTORY_MAX_PAGE_SIZE ||
        result->page.total_active < result->page.count ||
        (result->page.has_more && result->page.next_cursor == 0) ||
        (!result->page.has_more && result->page.next_cursor != 0)) {
        return 0;
    }
    for (uint32_t i = 0; i < result->page.count; ++i) {
        const ivr_worker_inventory_record_t *record =
            &result->page.records[i];
        if (!record->tenant_id[0] || !record->provider_session_id[0] ||
            !record->dialog_id[0] ||
            !record->room_id[0] || !record->call_id[0] ||
            !record->worker_id[0] || !record->worker_instance_id[0] ||
            record->worker_epoch == 0 || record->call_generation == 0 ||
            (record->input_active &&
             (!record->input_id[0] || record->input_generation == 0)) ||
            (!record->input_active &&
             (record->input_id[0] || record->input_generation != 0)) ||
            !inventory_state_name(record->state) ||
            (record->rebindable != 0 && record->rebindable != 1) ||
            strcmp(record->worker_id, result->worker_id) != 0) {
            return 0;
        }
    }
    return 1;
}

ivr_status_t ivr_flowmq_gateway_encode_inventory_page(
    DataBind *codec, const ivr_worker_inventory_envelope_t *result,
    uint8_t *frame, size_t frame_capacity, size_t *out_size) {
    char *records_json = NULL;
    char *json = NULL;
    char number[32];
    ivr_json_builder_t records;
    ivr_json_builder_t builder;
    ivr_status_t status = IVR_ESTATE;

    if (!codec || !frame || !out_size ||
        frame_capacity < IVR_FRAME_HEADER_SIZE || !inventory_page_valid(result)) {
        return IVR_EINVAL;
    }
    *out_size = 0;
    records_json = (char *)calloc(1, IVR_INVENTORY_WIRE_JSON_CAPACITY);
    json = (char *)calloc(1, IVR_INVENTORY_WIRE_JSON_CAPACITY);
    if (!records_json || !json) {
        status = IVR_ENOSPC;
        goto cleanup;
    }

    ivr_json_builder_init(&records, records_json,
                          IVR_INVENTORY_WIRE_JSON_CAPACITY);
    ivr_json_builder_raw(&records, "[");
    for (uint32_t i = 0; i < result->page.count; ++i) {
        const ivr_worker_inventory_record_t *record =
            &result->page.records[i];
        if (i != 0) ivr_json_builder_raw(&records, ",");
        ivr_json_builder_raw(&records, "{\"workerId\":");
        ivr_json_builder_string_cstr(&records, record->worker_id);
        ivr_json_builder_raw(&records, ",\"workerInstanceId\":");
        ivr_json_builder_string_cstr(&records, record->worker_instance_id);
        ivr_json_builder_raw(&records, ",\"workerEpoch\":");
        snprintf(number, sizeof(number), "%llu",
                 (unsigned long long)record->worker_epoch);
        ivr_json_builder_raw(&records, number);
        ivr_json_builder_raw(&records, ",\"tenantId\":");
        ivr_json_builder_string_cstr(&records, record->tenant_id);
        ivr_json_builder_raw(&records, ",\"providerSessionId\":");
        ivr_json_builder_string_cstr(&records, record->provider_session_id);
        ivr_json_builder_raw(&records, ",\"dialogId\":");
        ivr_json_builder_string_cstr(&records, record->dialog_id);
        ivr_json_builder_raw(&records, ",\"roomId\":");
        ivr_json_builder_string_cstr(&records, record->room_id);
        ivr_json_builder_raw(&records, ",\"callId\":");
        ivr_json_builder_string_cstr(&records, record->call_id);
        ivr_json_builder_raw(&records, ",\"callGeneration\":");
        snprintf(number, sizeof(number), "%llu",
                 (unsigned long long)record->call_generation);
        ivr_json_builder_raw(&records, number);
        ivr_json_builder_raw(&records, ",\"operationGeneration\":");
        snprintf(number, sizeof(number), "%llu",
                 (unsigned long long)record->operation_generation);
        ivr_json_builder_raw(&records, number);
        ivr_json_builder_raw(&records, ",\"inputId\":");
        ivr_json_builder_string_cstr(&records, record->input_id);
        ivr_json_builder_raw(&records, ",\"inputGeneration\":");
        snprintf(number, sizeof(number), "%llu",
                 (unsigned long long)record->input_generation);
        ivr_json_builder_raw(&records, number);
        ivr_json_builder_raw(&records, ",\"inputActive\":");
        ivr_json_builder_raw(&records,
                             record->input_active ? "true" : "false");
        ivr_json_builder_raw(&records, ",\"state\":");
        ivr_json_builder_string_cstr(&records,
                                     inventory_state_name(record->state));
        ivr_json_builder_raw(&records, ",\"rebindable\":");
        ivr_json_builder_raw(&records,
                             record->rebindable ? "true" : "false");
        ivr_json_builder_raw(&records, "}");
    }
    ivr_json_builder_raw(&records, "]");
    if (!ivr_json_builder_ok(&records)) {
        status = IVR_ENOSPC;
        goto cleanup;
    }

    ivr_json_builder_init(&builder, json, IVR_INVENTORY_WIRE_JSON_CAPACITY);
    ivr_json_builder_raw(&builder, "{\"message_id\":");
    ivr_json_builder_string_cstr(&builder, result->message_id);
    ivr_json_builder_raw(&builder, ",\"worker_id\":");
    ivr_json_builder_string_cstr(&builder, result->worker_id);
#define INVENTORY_PAGE_U64(name, value)                                      \
    do {                                                                      \
        ivr_json_builder_raw(&builder, ",\"" name "\":");             \
        snprintf(number, sizeof(number), "%llu",                            \
                 (unsigned long long)(value));                                \
        ivr_json_builder_raw(&builder, number);                               \
    } while (0)
    INVENTORY_PAGE_U64("inventory_version", result->page.inventory_version);
    INVENTORY_PAGE_U64("revision", result->page.revision);
    INVENTORY_PAGE_U64("cursor", result->page.cursor);
    INVENTORY_PAGE_U64("next_cursor", result->page.next_cursor);
    INVENTORY_PAGE_U64("total_active", result->page.total_active);
    INVENTORY_PAGE_U64("count", result->page.count);
    ivr_json_builder_raw(&builder, ",\"has_more\":");
    ivr_json_builder_raw(&builder,
                         result->page.has_more ? "true" : "false");
    ivr_json_builder_raw(&builder, ",\"records_json\":");
    ivr_json_builder_string_cstr(&builder, records_json);
    ivr_json_builder_raw(&builder, ",\"status_code\":");
    snprintf(number, sizeof(number), "%d", result->status_code);
    ivr_json_builder_raw(&builder, number);
    ivr_json_builder_raw(&builder, ",\"error_code\":");
    ivr_json_builder_string_cstr(&builder, result->error_code);
    ivr_json_builder_raw(&builder, ",\"error_message\":");
    ivr_json_builder_string_cstr(&builder, result->error_message);
    ivr_json_builder_raw(&builder, "}");
#undef INVENTORY_PAGE_U64
    if (!ivr_json_builder_ok(&builder)) {
        status = IVR_ENOSPC;
        goto cleanup;
    }
    status = encode_typed_frame(
        codec, "WorkerMediaInventoryPageV1",
        IVR_TYPE_WORKER_MEDIA_INVENTORY_PAGE_V1, IVR_KIND_RESULT, json, frame,
        frame_capacity, out_size);

cleanup:
    free(json);
    free(records_json);
    return status;
}

ivr_status_t ivr_flowmq_gateway_send_inventory_page(
    ivr_flowmq_gateway_t *gateway,
    const ivr_worker_inventory_envelope_t *result) {
    uint8_t *frame;
    size_t frame_size = 0;
    ivr_status_t status;
    if (!gateway || !gateway->endpoint || !result) {
        return IVR_EINVAL;
    }
    frame = (uint8_t *)malloc(IVR_INVENTORY_WIRE_FRAME_CAPACITY);
    if (!frame) {
        return IVR_ENOSPC;
    }
    status = ivr_flowmq_gateway_encode_inventory_page(
        gateway->codec, result, frame, IVR_INVENTORY_WIRE_FRAME_CAPACITY,
        &frame_size);
    if (status == IVR_OK) {
        status = ivr_flowmq_gateway_send_frame(gateway, frame, frame_size);
    }
    free(frame);
    return status;
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

    flowmq_connect_endpoint_config_t ep;
    uint64_t timeout_ms = config->timeout_ms ? config->timeout_ms : 5000u;
    int rc;
    if (timeout_ms > UINT64_MAX / UINT64_C(1000000)) {
        data_bind_free(g->codec);
        free(g);
        return IVR_EINVAL;
    }
    flowmq_connect_endpoint_config_init(&ep);
    ep.pattern = FLOWMQ_PROTOCOL_DEALER;
    ep.transport = config->transport
                       ? (flowmq_coronet_transport_t)config->transport
                       : FLOWMQ_TRANSPORT_TCP;
    ep.host = config->host;
    ep.port = config->port;
    ep.identity = g->worker_id;
    ep.topic = "ivr.internal";
    ep.max_frame_size = FLOWMQ_CONNECT_ENDPOINT_DEFAULT_MAX_FRAME_SIZE;
    ep.timeouts.timeout_ms = timeout_ms;
    ep.timeouts.set_flags = FLOWMQ_TIMEOUT_SET_DEFAULT;
    ep.reconnect_initial_ms =
        config->reconnect_initial_ms ? config->reconnect_initial_ms : 1000;
    ep.reconnect_max_ms =
        config->reconnect_max_ms ? config->reconnect_max_ms : 30000;
    ep.tls = config->tls;
    ep.path = config->path ? config->path : "";
    ep.context = NULL;
    ep.drive_context = 1;
    ep.own_context = 1;
    ep.on_frame = flowmq_on_message;
    ep.on_state = flowmq_on_connection_state;
    ep.callback_ctx = g;

    g->on_reply = config->on_reply;
    g->reply_ctx = config->reply_ctx;
    g->on_connection = config->on_connection;
    g->connection_ctx = config->connection_ctx;
    g->start_timeout_ns = timeout_ms * UINT64_C(1000000);
    atomic_init(&g->next_completion_id, 1u);
    rc = flowmq_connect_endpoint_create(&ep, &g->endpoint);
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
    if (!gateway || !gateway->endpoint) {
        return IVR_EINVAL;
    }
    return flowmq_connect_endpoint_start(
               gateway->endpoint, gateway->start_timeout_ns) == TURBO_OK
               ? IVR_OK
               : IVR_ESTATE;
}

void ivr_flowmq_gateway_destroy(ivr_flowmq_gateway_t *gateway) {
    if (!gateway) {
        return;
    }
    if (gateway->endpoint) {
        flowmq_connect_endpoint_destroy(gateway->endpoint);
        gateway->endpoint = NULL;
    }
    if (gateway->codec) {
        data_bind_free(gateway->codec);
        gateway->codec = NULL;
    }
    free(gateway);
}
