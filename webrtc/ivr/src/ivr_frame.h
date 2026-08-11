#ifndef TURBO_MEDIA_IVR_FRAME_H
#define TURBO_MEDIA_IVR_FRAME_H

/**
 * @file ivr_frame.h
 * @brief TIVR 12-byte frame header (self-describing wire envelope).
 *
 * The header only answers "which codec/type decodes this payload". It must be
 * encoded/decoded field by field; casting network bytes to a C struct or using
 * #pragma pack is forbidden. FlowMQ provides message boundaries, so the header
 * does not carry a payload length; receivers first enforce the message byte
 * cap and size >= IVR_FRAME_HEADER_SIZE.
 *
 * Layout (little-endian):
 *   offset  size  field
 *   0       4     magic = "TIVR"
 *   4       1     frame_version = 1
 *   5       1     format: 1=BIN, 2=TEXT (compact UTF-8 JSON)
 *   6       1     kind: 1=command, 2=result, 3=event, 4=snapshot
 *   7       1     flags; v1 must be zero
 *   8       2     schema_type_id
 *   10      1     schema_major
 *   11      1     schema_minor
 *   12      N     DataBind payload
 */

#include "ivr/ivr_worker.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IVR_FRAME_HEADER_SIZE 12u
#define IVR_FRAME_MAGIC_0 'T'
#define IVR_FRAME_MAGIC_1 'I'
#define IVR_FRAME_MAGIC_2 'V'
#define IVR_FRAME_MAGIC_3 'R'
#define IVR_FRAME_VERSION 1u
#define IVR_SCHEMA_MAJOR 1u
#define IVR_SCHEMA_MINOR 0u

typedef enum {
    IVR_FMT_BIN = 1,
    IVR_FMT_TEXT = 2
} ivr_frame_format_t;

typedef enum {
    IVR_KIND_COMMAND = 1,
    IVR_KIND_RESULT = 2,
    IVR_KIND_EVENT = 3,
    IVR_KIND_SNAPSHOT = 4
} ivr_frame_kind_t;

/* Published schema_type_id values. IDs are never reused once published. */
#define IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1 1001u
#define IVR_TYPE_CONFERENCE_LEAVE_COMMAND_V1 1002u
#define IVR_TYPE_TRANSFER_COMMAND_V1 1003u
#define IVR_TYPE_WORKER_SYNC_COMMAND_V1 1004u
#define IVR_TYPE_GET_SNAPSHOT_COMMAND_V1 1005u
#define IVR_TYPE_CALL_DISPATCH_COMMAND_V1 1006u
#define IVR_TYPE_WORKER_SYNC_COMMAND_V2 1007u
#define IVR_TYPE_WORKER_HEARTBEAT_V1 1008u
#define IVR_TYPE_CALL_RELEASE_COMMAND_V1 1009u
#define IVR_TYPE_CALL_DISPATCH_COMMAND_V2 1010u
#define IVR_TYPE_IVR_COMMAND_RESULT_V1 2001u
#define IVR_TYPE_WORKER_SYNC_RESULT_V1 2002u
#define IVR_TYPE_ROOM_SNAPSHOT_V1 2003u
#define IVR_TYPE_CALL_DISPATCH_RESULT_V1 2004u
#define IVR_TYPE_CALL_RELEASE_RESULT_V1 2005u
#define IVR_TYPE_CALL_DISPATCH_RESULT_V2 2006u
#define IVR_TYPE_CONFERENCE_PARTICIPANT_JOINED_EVENT_V1 3001u
#define IVR_TYPE_DTMF_FINAL_EVENT_V1 3002u
#define IVR_TYPE_ASR_FINAL_EVENT_V1 3003u
#define IVR_TYPE_INPUT_TIMEOUT_EVENT_V1 3004u
#define IVR_TYPE_PLAYBACK_FINISHED_EVENT_V1 3005u
#define IVR_TYPE_IVR_WORKER_LOST_EVENT_V1 3006u

typedef struct {
    uint8_t format;
    uint8_t kind;
    uint16_t schema_type_id;
    uint8_t schema_major;
    uint8_t schema_minor;
} ivr_frame_info_t;

/* Encode the header into `out` (must be >= IVR_FRAME_HEADER_SIZE bytes).
   Returns IVR_OK or IVR_EINVAL. */
int ivr_frame_encode(uint8_t *out, const ivr_frame_info_t *info);

/* Decode and fully validate a frame. `size` is the whole message size
   (header + payload). Returns IVR_OK and fills `info`, or a distinct error:
   IVR_EINVAL (size/short frame), IVR_ESTATE (unknown magic/version/format/
   kind/flags), IVR_EVERSION (unknown schema version). */
int ivr_frame_decode(const uint8_t *buf, size_t size, ivr_frame_info_t *out);

/* Versioned registry: schema_type_id <-> canonical type name. */
const char *ivr_frame_type_name(uint16_t schema_type_id);
int ivr_frame_type_id_by_name(const char *name, uint16_t *out_id);
int ivr_frame_type_kind(uint16_t schema_type_id, uint8_t *out_kind);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_FRAME_H */
