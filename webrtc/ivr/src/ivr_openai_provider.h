/*
 * ivr_openai_provider.h - Remote TTS/ASR providers over an OpenAI-compatible
 * audio API (turbo_speech provider implementations).
 *
 * TTS: POST {base_url}{tts_path} with a JSON body
 *   {"model":..., "input":"<text>", "voice":..., "response_format":"pcm",
 *    "speed":...}
 * OpenAI returns raw 16-bit little-endian mono PCM at 24 kHz. The provider
 * resamples it to the configured output sample_rate (default 16000 Hz, which
 * matches the IVR media bot) and delivers it to callbacks->on_audio in
 * tts_frame_bytes chunks, then callbacks->on_complete.
 *
 * ASR: start() records the session format, write() accumulates PCM frames,
 * finish() wraps the accumulated PCM in a WAV container and POSTs it as
 * multipart/form-data (fields: file, model, optional language) to
 * {base_url}{asr_path}, then parses the JSON response {"text":"..."} into a
 * final result (is_final=1) followed by callbacks->on_complete.
 *
 * Threading model: each wrapper owns one persistent worker thread that runs
 * the blocking http_client request. Provider callbacks are invoked on that
 * worker thread (the turbo_speech contract allows provider-thread callbacks
 * as long as one session is serialized). cancel() sets the cancellation flag
 * and waits until the worker has quiesced, so no callback is in flight when
 * cancel() returns. provider.destroy() only quiesces; it intentionally keeps
 * the wrapper alive because the IVR media bot reuses the same provider struct
 * across calls (start_bot/stop_bot create and destroy turbo sessions while
 * the provider context is app-owned). Call ivr_openai_tts_free /
 * ivr_openai_asr_free once at teardown to stop the worker and free the
 * wrapper.
 *
 * Configuration strings are borrowed and must outlive the wrappers.
 */
#ifndef TURBO_MEDIA_IVR_OPENAI_PROVIDER_H
#define TURBO_MEDIA_IVR_OPENAI_PROVIDER_H

#include "turbo_speech.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_openai_tts ivr_openai_tts_t;
typedef struct ivr_openai_asr ivr_openai_asr_t;

typedef enum {
    IVR_OPENAI_REQUEST_TTS = 0,
    IVR_OPENAI_REQUEST_ASR
} ivr_openai_request_kind_t;

typedef struct {
    void *context;
    void (*on_request_complete)(void *context,
                                ivr_openai_request_kind_t kind,
                                uint64_t duration_ms);
} ivr_openai_observer_ops_t;

typedef struct {
    uint64_t active_tts_instances;
    uint64_t active_asr_instances;
    uint64_t live_provider_threads;
    uint64_t retained_input_bytes;
    uint64_t retained_response_bytes;
} ivr_openai_resource_snapshot_t;

typedef struct {
    const char *base_url;          /* required, e.g. "https://api.openai.com" */
    const char *api_key;           /* Bearer credential; NULL/"" disables auth */
    const char *tts_path;          /* NULL -> "/v1/audio/speech" */
    const char *tts_model;         /* NULL -> "tts-1" */
    const char *tts_voice;         /* NULL -> "alloy" */
    const char *asr_path;          /* NULL -> "/v1/audio/transcriptions" */
    const char *asr_model;         /* NULL -> "whisper-1" */
    const char *asr_language;      /* optional language hint; NULL = auto */
    int tts_sample_rate;           /* provider native PCM rate; 0 -> 24000 */
    int sample_rate;               /* delivered rate; 0 -> 16000 */
    int channels;                  /* 0 -> 1 */
    int bits_per_sample;           /* 0 -> 16 */
    size_t tts_frame_bytes;        /* on_audio chunk size; 0 -> 3200 */
    size_t max_tts_input_bytes;    /* queued text cap; 0 -> 64 KiB */
    size_t max_asr_buffer_bytes;   /* accumulated PCM cap; 0 -> 4 MiB */
    size_t max_response_bytes;     /* http response cap; 0 -> 16 MiB */
    int timeout_ms;                /* 0 -> 30000 */
    int connect_timeout_ms;        /* 0 -> 5000 */
    const char *ca_file;           /* optional PEM CA bundle for TLS */
    const char *server_name;       /* optional verified TLS identity/SNI */
    const char *user_agent;        /* optional */
    /* Optional borrowed observer, copied into each wrapper. It runs on the
       provider worker after request processing and callbacks have completed,
       without a provider lock held; it must be nonblocking and outlive the
       wrapper. */
    ivr_openai_observer_ops_t observer;
} ivr_openai_config_t;

/* Create a wrapper owning one persistent worker thread. config is borrowed
   and immutable for the wrapper lifetime. Returns 0 on success and transfers
   one wrapper to the caller; failure leaves the output NULL. */
int ivr_openai_tts_create(const ivr_openai_config_t *config,
                          ivr_openai_tts_t **out_tts);
int ivr_openai_asr_create(const ivr_openai_config_t *config,
                          ivr_openai_asr_t **out_asr);

/* Fill a provider struct bound to the wrapper. */
void ivr_openai_tts_get_provider(ivr_openai_tts_t *tts,
                                 turbo_tts_provider_t *out_provider);
void ivr_openai_asr_get_provider(ivr_openai_asr_t *asr,
                                 turbo_asr_provider_t *out_provider);

/* Stop the persistent worker and free the wrapper. Must be quiesced (no
   in-flight request); call cancel() on the sessions first or guarantee the
   worker is idle. NULL-safe. */
void ivr_openai_tts_free(ivr_openai_tts_t *tts);
void ivr_openai_asr_free(ivr_openai_asr_t *asr);

/* Process-wide, thread-safe diagnostic snapshot. Retained input bytes cover
   queued TTS text and allocated ASR PCM storage. Retained response bytes cover
   HTTP response bodies while provider workers are processing them. */
void ivr_openai_get_resource_snapshot(
    ivr_openai_resource_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_OPENAI_PROVIDER_H */
