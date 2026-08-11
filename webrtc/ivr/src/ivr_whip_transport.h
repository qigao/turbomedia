#ifndef TURBO_MEDIA_IVR_WHIP_TRANSPORT_H
#define TURBO_MEDIA_IVR_WHIP_TRANSPORT_H

/**
 * @file ivr_whip_transport.h
 * @brief Real WHIP audio transport for the IVR media bot (publish side).
 *
 * Implements the audio transport boundary (ivr_media_transport_t from
 * ivr_media_bot.h) over a real WebRTC HTTP Ingestion (WHIP) connection to the
 * SFU node: an audio send-only track carries the TTS PCM produced by the bot.
 * The transport drives the turbo_peer_connection (ICE/DTLS/SRTP), POSTs the
 * offer to /whip/:room/:participant, sets the SFU answer, trickles ICE via
 * PATCH (If-Match), and starts the send track once connected.
 */

#include "ivr/ivr_worker.h"
#include "ivr_media_bot.h"
#include "ivr_media_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_whip_transport_s ivr_whip_transport_t;

typedef struct {
    const char *sfu_host;   /* 127.0.0.1 */
    int sfu_port;           /* SFU HTTP port (WHIP endpoint) */
    const char *media_token; /* WHIP/WHEP bearer token */
    int allow_loopback;     /* allow host ICE candidates on loopback */
    uint32_t sample_rate;   /* 0 = 16000 */
    uint64_t connect_timeout_ms; /* 0 = 10000 */
    ivr_media_state_fn on_state;
    void *state_context;
} ivr_whip_transport_config_t;

ivr_status_t ivr_whip_transport_create(const ivr_whip_transport_config_t *config,
                                       ivr_whip_transport_t **out_transport);

/* Fill an ivr_media_transport_t bound to the transport (play_audio/stop). */
void ivr_whip_transport_get_transport(ivr_whip_transport_t *transport,
                                      ivr_media_transport_t *out);

/* Open the WHIP publish connection for one call (offer -> answer -> ICE ->
   DTLS). The app calls this when the bot joins the room. */
ivr_status_t ivr_whip_transport_start(ivr_whip_transport_t *transport,
                                      const ivr_call_ref_t *call);

int ivr_whip_transport_connected(const ivr_whip_transport_t *transport);
/* Local RTP SSRC of the active audio publish track; 0 before start/after stop. */
uint32_t ivr_whip_transport_ssrc(const ivr_whip_transport_t *transport);
uint64_t ivr_whip_transport_frames_sent(const ivr_whip_transport_t *transport);

/* Forward one RTP packet through the active WHIP send track. Sequence and
   SSRC are transport-owned; timestamp, marker and payload type are retained. */
int ivr_whip_transport_send_rtp_packet(ivr_whip_transport_t *transport,
                                       const uint8_t *packet, size_t length);

void ivr_whip_transport_destroy(ivr_whip_transport_t *transport);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_WHIP_TRANSPORT_H */
