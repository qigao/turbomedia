#ifndef TURBO_MEDIA_IVR_WHEP_TRANSPORT_H
#define TURBO_MEDIA_IVR_WHEP_TRANSPORT_H

#include "ivr/ivr_worker.h"
#include "ivr_media_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_whep_transport_s ivr_whep_transport_t;

typedef int (*ivr_whep_audio_cb)(void *context, const ivr_call_ref_t *call,
                                 const uint8_t *pcm, size_t length,
                                 uint32_t sample_rate,
                                 uint64_t rtp_timestamp);

/* Raw authenticated RTP auxiliary payload (for example RFC 4733
   telephone-event). The packet bytes are borrowed for this callback only. */
typedef int (*ivr_whep_rtp_cb)(void *context, const ivr_call_ref_t *call,
                               uint8_t payload_type, uint32_t rtp_timestamp,
                               const uint8_t *payload, size_t payload_length,
                               uint64_t source_generation);

typedef struct {
    const char *sfu_base_url;
    const char *media_token;
    const char *ca_file;
    const char *cert_file;
    const char *key_file;
    const char *key_password;
    int allow_plaintext_loopback;
    uint32_t sample_rate;
    int allow_loopback;
    uint64_t http_timeout_ms;
    uint64_t connect_timeout_ms;
    uint64_t input_inactivity_timeout_ms; /* 0 = 5000 */
    ivr_media_state_fn on_state;
    void *state_context;
    ivr_whep_audio_cb on_audio;
    void *audio_context;
    uint8_t telephone_event_payload_type; /* 0 = 126 */
    ivr_whep_rtp_cb on_rtp;
    void *rtp_context;
} ivr_whep_transport_config_t;

ivr_status_t ivr_whep_transport_create(
    const ivr_whep_transport_config_t *config,
    ivr_whep_transport_t **out_transport);

ivr_status_t ivr_whep_transport_start(ivr_whep_transport_t *transport,
                                      const ivr_call_ref_t *call,
                                      const char *participant_id);

int ivr_whep_transport_stop(ivr_whep_transport_t *transport,
                            const ivr_call_ref_t *call);

int ivr_whep_transport_connected(const ivr_whep_transport_t *transport);
uint64_t ivr_whep_transport_frames_received(
    const ivr_whep_transport_t *transport);
uint64_t ivr_whep_transport_frames_rejected(
    const ivr_whep_transport_t *transport);

void ivr_whep_transport_destroy(ivr_whep_transport_t *transport);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_WHEP_TRANSPORT_H */
