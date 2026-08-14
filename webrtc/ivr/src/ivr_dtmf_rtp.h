#ifndef TURBO_MEDIA_IVR_DTMF_RTP_H
#define TURBO_MEDIA_IVR_DTMF_RTP_H

#include "ivr/ivr_worker.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IVR_DTMF_ID_MAX 128u

typedef struct ivr_dtmf_ingress_s ivr_dtmf_ingress_t;

typedef struct {
    uint32_t window_capacity; /* 0 = 64 */
    uint16_t max_duration;    /* RTP clock ticks; 0 = 48000 */
} ivr_dtmf_ingress_config_t;

typedef struct {
    char provider_session_id[IVR_DTMF_ID_MAX];
    char dialog_id[IVR_DTMF_ID_MAX];
    char room_id[IVR_DTMF_ID_MAX];
    char call_id[IVR_DTMF_ID_MAX];
    char input_id[IVR_DTMF_ID_MAX];
    uint64_t call_generation;
    uint64_t input_generation;
    uint64_t source_generation;
    uint32_t rtp_timestamp;
    char digit;
    uint16_t duration;
    int ended;
} ivr_dtmf_input_t;

ivr_status_t ivr_dtmf_ingress_create(
    const ivr_dtmf_ingress_config_t *config,
    ivr_dtmf_ingress_t **out_ingress);
void ivr_dtmf_ingress_destroy(ivr_dtmf_ingress_t *ingress);

ivr_status_t ivr_dtmf_ingress_begin_input(
    ivr_dtmf_ingress_t *ingress, const ivr_call_ref_t *call,
    const char *input_id, uint64_t input_generation);
ivr_status_t ivr_dtmf_ingress_end_input(
    ivr_dtmf_ingress_t *ingress, const ivr_call_ref_t *call,
    uint64_t input_generation);

/* Parses one RFC 4733 telephone-event payload. Only final (E=1) packets are
   emitted. Repeated end packets are rejected by call, source generation,
   RTP timestamp, event code, and input-window generation. */
ivr_status_t ivr_dtmf_ingress_submit_rtp(
    ivr_dtmf_ingress_t *ingress, const ivr_call_ref_t *call,
    uint64_t source_generation, uint32_t rtp_timestamp,
    const uint8_t *payload, size_t payload_length,
    ivr_dtmf_input_t *out_input);

#ifdef __cplusplus
}
#endif

#endif
