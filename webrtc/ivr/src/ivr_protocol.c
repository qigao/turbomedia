#include "ivr_protocol.h"
#include <stdint.h>
#include <string.h>

static ivr_status_t ivr_protocol_bind_status(DataBindStatus status) {
    return status == DATA_BIND_ERR_OOM ? IVR_ENOSPC : IVR_EINVAL;
}

static int ivr_protocol_info_matches_type(const ivr_frame_info_t *info,
                                          const char *type_name) {
    uint16_t type_id = 0;
    uint8_t kind = 0;
    return info && type_name &&
           ivr_frame_type_id_by_name(type_name, &type_id) == IVR_OK &&
           ivr_frame_type_kind(type_id, &kind) == IVR_OK &&
           type_id == info->schema_type_id && kind == info->kind;
}

ivr_status_t ivr_protocol_decode(DataBind *codec, const uint8_t *frame,
                                 size_t frame_len,
                                 DataBindObject **out_object,
                                 ivr_frame_info_t *out_info) {
    ivr_frame_info_t info;
    DataBindObject *object = NULL;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    const char *type_name;
    const uint8_t *payload;
    size_t payload_len;

    if (out_object) {
        *out_object = NULL;
    }
    if (!codec || !frame || !out_object) {
        return IVR_EINVAL;
    }
    ivr_status_t rc = (ivr_status_t)ivr_frame_decode(frame, frame_len, &info);
    if (rc != IVR_OK) {
        return rc;
    }
    type_name = ivr_frame_type_name(info.schema_type_id);
    if (!type_name) {
        return IVR_EINVAL;
    }
    payload = frame + IVR_FRAME_HEADER_SIZE;
    payload_len = frame_len - IVR_FRAME_HEADER_SIZE;
    if (info.format == IVR_FMT_BIN) {
        status = data_bind_object_from_bin(codec, type_name, payload,
                                           payload_len, &object, &error);
    } else if (info.format == IVR_FMT_TEXT) {
        status = data_bind_object_from_json(codec, type_name,
                                            (const char *)payload, payload_len,
                                            &object, &error);
    } else {
        return IVR_ESTATE;
    }
    if (status != DATA_BIND_OK) {
        return ivr_protocol_bind_status(status);
    }
    if (!ivr_protocol_info_matches_type(&info,
                                        data_bind_object_type_name(object))) {
        data_bind_object_free(object);
        return IVR_ESTATE;
    }
    *out_object = object;
    if (out_info) {
        *out_info = info;
    }
    return IVR_OK;
}

ivr_status_t ivr_protocol_encode(DataBind *codec,
                                 const ivr_frame_info_t *info,
                                 const DataBindObject *object, uint8_t *frame,
                                 size_t frame_capacity, size_t *out_len) {
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindStatus status;
    const char *type_name;
    size_t payload_len = 0;

    if (out_len) {
        *out_len = 0;
    }
    if (!codec || !info || !object || !frame || !out_len) {
        return IVR_EINVAL;
    }
    if (frame_capacity < IVR_FRAME_HEADER_SIZE) {
        return IVR_ENOSPC;
    }
    type_name = data_bind_object_type_name(object);
    if (!ivr_protocol_info_matches_type(info, type_name)) {
        return IVR_EINVAL;
    }
    if (info->format == IVR_FMT_BIN) {
        status = data_bind_object_serialize_bin_into(
            codec, object, frame + IVR_FRAME_HEADER_SIZE,
            frame_capacity - IVR_FRAME_HEADER_SIZE, &payload_len, &error);
        if (status != DATA_BIND_OK) {
            if (payload_len > frame_capacity - IVR_FRAME_HEADER_SIZE) {
                return IVR_ENOSPC;
            }
            return status == DATA_BIND_ERR_OOM ? IVR_ENOSPC : IVR_ESTATE;
        }
    } else if (info->format == IVR_FMT_TEXT) {
        char *text = NULL;
        status = data_bind_object_serialize_json(codec, object, &text,
                                                 &payload_len, &error);
        if (status != DATA_BIND_OK) {
            return status == DATA_BIND_ERR_OOM ? IVR_ENOSPC : IVR_ESTATE;
        }
        if (payload_len > SIZE_MAX - IVR_FRAME_HEADER_SIZE ||
            IVR_FRAME_HEADER_SIZE + payload_len > frame_capacity) {
            data_bind_serialized_free(text);
            return IVR_ENOSPC;
        }
        memcpy(frame + IVR_FRAME_HEADER_SIZE, text, payload_len);
        data_bind_serialized_free(text);
    } else {
        return IVR_EINVAL;
    }
    if (ivr_frame_encode(frame, info) != IVR_OK) {
        return IVR_EINVAL;
    }
    *out_len = IVR_FRAME_HEADER_SIZE + payload_len;
    return IVR_OK;
}
