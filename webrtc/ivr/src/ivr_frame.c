#include "ivr_frame.h"
#include "ivr/ivr_worker.h"
#include <string.h>

typedef struct {
    uint16_t id;
    const char *name;
    uint8_t kind;
} ivr_type_entry_t;

/* Versioned constant table. IDs published here are never reused. */
static const ivr_type_entry_t kTypes[] = {
    {IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1, "ConferenceJoinCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_CONFERENCE_LEAVE_COMMAND_V1, "ConferenceLeaveCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_TRANSFER_COMMAND_V1, "TransferCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_WORKER_SYNC_COMMAND_V1, "WorkerSyncCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_GET_SNAPSHOT_COMMAND_V1, "GetSnapshotCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_CALL_DISPATCH_COMMAND_V1, "CallDispatchCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_WORKER_SYNC_COMMAND_V2, "WorkerSyncCommandV2", IVR_KIND_COMMAND},
    {IVR_TYPE_WORKER_HEARTBEAT_V1, "WorkerHeartbeatV1", IVR_KIND_COMMAND},
    {IVR_TYPE_CALL_RELEASE_COMMAND_V1, "CallReleaseCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_CALL_DISPATCH_COMMAND_V2, "CallDispatchCommandV2", IVR_KIND_COMMAND},
    {IVR_TYPE_MEDIA_SESSION_OPEN_COMMAND_V1, "MediaSessionOpenCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_MEDIA_PLAY_COMMAND_V1, "MediaPlayCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_MEDIA_INPUT_START_COMMAND_V1, "MediaInputStartCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_MEDIA_INPUT_STOP_COMMAND_V1, "MediaInputStopCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_MEDIA_CANCEL_COMMAND_V1, "MediaCancelCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_MEDIA_SESSION_CLOSE_COMMAND_V1, "MediaSessionCloseCommandV1", IVR_KIND_COMMAND},
    {IVR_TYPE_WORKER_MEDIA_INVENTORY_QUERY_V1, "WorkerMediaInventoryQueryV1", IVR_KIND_COMMAND},
    {IVR_TYPE_MEDIA_CANCEL_COMMAND_V2, "MediaCancelCommandV2", IVR_KIND_COMMAND},
    {IVR_TYPE_IVR_COMMAND_RESULT_V1, "IvrCommandResultV1", IVR_KIND_RESULT},
    {IVR_TYPE_WORKER_SYNC_RESULT_V1, "WorkerSyncResultV1", IVR_KIND_RESULT},
    {IVR_TYPE_ROOM_SNAPSHOT_V1, "RoomSnapshotV1", IVR_KIND_SNAPSHOT},
    {IVR_TYPE_CALL_DISPATCH_RESULT_V1, "CallDispatchResultV1", IVR_KIND_RESULT},
    {IVR_TYPE_CALL_RELEASE_RESULT_V1, "CallReleaseResultV1", IVR_KIND_RESULT},
    {IVR_TYPE_CALL_DISPATCH_RESULT_V2, "CallDispatchResultV2", IVR_KIND_RESULT},
    {IVR_TYPE_MEDIA_COMMAND_RESULT_V1, "MediaCommandResultV1", IVR_KIND_RESULT},
    {IVR_TYPE_WORKER_MEDIA_INVENTORY_PAGE_V1, "WorkerMediaInventoryPageV1", IVR_KIND_RESULT},
    {IVR_TYPE_CONFERENCE_PARTICIPANT_JOINED_EVENT_V1,
     "ConferenceParticipantJoinedEventV1", IVR_KIND_EVENT},
    {IVR_TYPE_DTMF_FINAL_EVENT_V1, "DtmfFinalEventV1", IVR_KIND_EVENT},
    {IVR_TYPE_ASR_FINAL_EVENT_V1, "AsrFinalEventV1", IVR_KIND_EVENT},
    {IVR_TYPE_INPUT_TIMEOUT_EVENT_V1, "InputTimeoutEventV1", IVR_KIND_EVENT},
    {IVR_TYPE_PLAYBACK_FINISHED_EVENT_V1, "PlaybackFinishedEventV1", IVR_KIND_EVENT},
    {IVR_TYPE_IVR_WORKER_LOST_EVENT_V1, "IvrWorkerLostEventV1", IVR_KIND_EVENT},
    {IVR_TYPE_MEDIA_EVENT_V1, "MediaEventV1", IVR_KIND_EVENT},
};

#define IVR_TYPE_COUNT (sizeof(kTypes) / sizeof(kTypes[0]))

static const ivr_type_entry_t *ivr_frame_find(uint16_t id) {
    for (size_t i = 0; i < IVR_TYPE_COUNT; i++) {
        if (kTypes[i].id == id) {
            return &kTypes[i];
        }
    }
    return NULL;
}

int ivr_frame_encode(uint8_t *out, const ivr_frame_info_t *info) {
    if (!out || !info) {
        return IVR_EINVAL;
    }
    if (info->format < IVR_FMT_BIN || info->format > IVR_FMT_TEXT ||
        info->kind < IVR_KIND_COMMAND || info->kind > IVR_KIND_SNAPSHOT) {
        return IVR_EINVAL;
    }
    if (info->schema_major != IVR_SCHEMA_MAJOR ||
        info->schema_minor != IVR_SCHEMA_MINOR) {
        return IVR_EVERSION;
    }
    const ivr_type_entry_t *type = ivr_frame_find(info->schema_type_id);
    if (!type || type->kind != info->kind) {
        return IVR_EINVAL;
    }
    out[0] = IVR_FRAME_MAGIC_0;
    out[1] = IVR_FRAME_MAGIC_1;
    out[2] = IVR_FRAME_MAGIC_2;
    out[3] = IVR_FRAME_MAGIC_3;
    out[4] = IVR_FRAME_VERSION;
    out[5] = info->format;
    out[6] = info->kind;
    out[7] = 0; /* reserved flags; v1 must be zero */
    out[8] = (uint8_t)(info->schema_type_id & 0xFFu);
    out[9] = (uint8_t)((info->schema_type_id >> 8) & 0xFFu);
    out[10] = info->schema_major;
    out[11] = info->schema_minor;
    return IVR_OK;
}

int ivr_frame_decode(const uint8_t *buf, size_t size, ivr_frame_info_t *out) {
    if (!buf) {
        return IVR_EINVAL;
    }
    if (size < IVR_FRAME_HEADER_SIZE) {
        return IVR_EINVAL; /* short frame */
    }
    if (buf[0] != IVR_FRAME_MAGIC_0 || buf[1] != IVR_FRAME_MAGIC_1 ||
        buf[2] != IVR_FRAME_MAGIC_2 || buf[3] != IVR_FRAME_MAGIC_3) {
        return IVR_ESTATE; /* unknown magic */
    }
    if (buf[4] != IVR_FRAME_VERSION) {
        return IVR_ESTATE; /* unknown frame version */
    }
    uint8_t format = buf[5];
    uint8_t kind = buf[6];
    uint8_t flags = buf[7];
    uint16_t type_id = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
    uint8_t major = buf[10];
    uint8_t minor = buf[11];
    if (flags != 0) {
        return IVR_ESTATE; /* reserved flags must be zero */
    }
    if (format < IVR_FMT_BIN || format > IVR_FMT_TEXT) {
        return IVR_ESTATE; /* unknown format */
    }
    if (kind < IVR_KIND_COMMAND || kind > IVR_KIND_SNAPSHOT) {
        return IVR_ESTATE; /* unknown kind */
    }
    if (major != IVR_SCHEMA_MAJOR || minor != IVR_SCHEMA_MINOR) {
        return IVR_EVERSION; /* unknown schema version */
    }
    const ivr_type_entry_t *type = ivr_frame_find(type_id);
    if (!type || type->kind != kind) {
        return IVR_EINVAL; /* unknown type id */
    }
    if (out) {
        out->format = format;
        out->kind = kind;
        out->schema_type_id = type_id;
        out->schema_major = major;
        out->schema_minor = minor;
    }
    return IVR_OK;
}

const char *ivr_frame_type_name(uint16_t schema_type_id) {
    const ivr_type_entry_t *e = ivr_frame_find(schema_type_id);
    return e ? e->name : NULL;
}

int ivr_frame_type_id_by_name(const char *name, uint16_t *out_id) {
    if (!name || !out_id) {
        return IVR_EINVAL;
    }
    for (size_t i = 0; i < IVR_TYPE_COUNT; i++) {
        if (strcmp(kTypes[i].name, name) == 0) {
            *out_id = kTypes[i].id;
            return IVR_OK;
        }
    }
    return IVR_EINVAL;
}

int ivr_frame_type_kind(uint16_t schema_type_id, uint8_t *out_kind) {
    const ivr_type_entry_t *type = ivr_frame_find(schema_type_id);
    if (!type || !out_kind) {
        return IVR_EINVAL;
    }
    *out_kind = type->kind;
    return IVR_OK;
}
