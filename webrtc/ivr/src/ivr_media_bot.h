#ifndef TURBO_MEDIA_IVR_MEDIA_BOT_H
#define TURBO_MEDIA_IVR_MEDIA_BOT_H

/**
 * @file ivr_media_bot.h
 * @brief Real media-port implementation for the IVR worker (P2 core).
 *
 * Implements ivr_media_port_ops_t over the provider-neutral TurboMedia speech
 * sessions (turbo_speech.h): play_pcm() synthesizes the text through a
 * turbo_tts session and forwards the PCM frames to an audio transport
 * (the WebRTC/SFU send-track boundary); caller PCM fed from the transport is
 * written into a turbo_asr session whose final results become "asr.final"
 * events delivered to on_event. Provider failures become "provider.error"
 * events. The app routes both through the per-call session inbox via
 * ivr_worker_submit_event_copy.
 *
 * Baseline constraint: one active call per bot (the conference baseline uses
 * one call per worker); start_bot() for a second call while one is active
 * fails fast with IVR_ESTATE. TTS/ASR providers are optional and pluggable
 * (the repo ships no model); without a TTS provider, play_pcm() fails fast
 * instead of faking audio.
 */

#include "ivr/ivr_worker.h"
#include "turbo_speech.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_media_bot_s ivr_media_bot_t;

/* Audio transport: the boundary to the room media plane (WebRTC/SFU in
   production; tests use a loopback). PCM is 16-bit mono at the configured
   sample rate. Borrowed call view; valid for the call only. */
typedef struct {
    void *context;
    /* Deliver TTS PCM to the bot's send track. */
    int (*play_audio)(void *ctx, const ivr_call_ref_t *call,
                      const uint8_t *pcm, size_t len, uint32_t sample_rate);
    /* Tear down the bot peer (leave the room). */
    int (*stop)(void *ctx, const ivr_call_ref_t *call);
} ivr_media_transport_t;

typedef struct {
    const turbo_tts_provider_t *tts_provider; /* NULL = play_pcm fails fast */
    const turbo_asr_provider_t *asr_provider; /* NULL = caller audio dropped */
    ivr_media_transport_t transport;
    uint32_t sample_rate; /* 0 = 16000 Hz */
    /* Events produced by the bot (asr.final, provider.error). Borrowed view
       for the call only; the consumer must copy before retaining. */
    void (*on_event)(void *ctx, const ivr_event_view_t *event);
    void *event_ctx;
} ivr_media_bot_config_t;

/* Create the bot. config is copied; providers/transport are borrowed and must
   outlive the bot. */
ivr_status_t ivr_media_bot_create(const ivr_media_bot_config_t *config,
                                  ivr_media_bot_t **out_bot);

/* Fill an ivr_media_port_ops_t bound to the bot. */
void ivr_media_bot_get_ops(ivr_media_bot_t *bot, ivr_media_port_ops_t *ops);

/* Feed caller PCM (16-bit mono) into the bot's ASR (transport receive side).
   Thread-safe; callable from the receive-track callback. */
ivr_status_t ivr_media_bot_feed_caller_audio(ivr_media_bot_t *bot,
                                             const ivr_call_ref_t *call,
                                             const uint8_t *pcm, size_t len);

void ivr_media_bot_destroy(ivr_media_bot_t *bot);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_MEDIA_BOT_H */
