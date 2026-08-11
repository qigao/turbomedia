#ifndef TURBO_MEDIA_IVR_PROTOCOL_H
#define TURBO_MEDIA_IVR_PROTOCOL_H

#include "data_bind.h"
#include "ivr_frame.h"

/* Thin DataBind adapter for the TIVR envelope.
 *
 * TXT means compact UTF-8 JSON. BIN and TXT bind the same canonical schema
 * object. No format probing or fallback is performed.
 *
 * Decode returns an owned object in out_object; release it with
 * data_bind_object_free(). Encode writes into caller-owned storage and writes
 * no frame header until the complete payload fits.
 */
ivr_status_t ivr_protocol_decode(DataBind *codec, const uint8_t *frame,
                                 size_t frame_len,
                                 DataBindObject **out_object,
                                 ivr_frame_info_t *out_info);

ivr_status_t ivr_protocol_encode(DataBind *codec,
                                 const ivr_frame_info_t *info,
                                 const DataBindObject *object, uint8_t *frame,
                                 size_t frame_capacity, size_t *out_len);

#endif /* TURBO_MEDIA_IVR_PROTOCOL_H */
