/**
 * Provider-neutral streaming ASR and TTS interfaces.
 *
 * Audio frame buffers and callback strings are borrowed views. They remain
 * valid only for the duration of the call. Providers that continue work on
 * another thread must copy them before returning.
 */
#ifndef TURBO_SPEECH_H
#define TURBO_SPEECH_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_SPEECH_PROVIDER_ABI_VERSION 1U

typedef struct turbo_asr_s turbo_asr_t;
typedef struct turbo_tts_s turbo_tts_t;
struct turbo_capture_s;

typedef enum {
  TURBO_SPEECH_OK = 0,
  TURBO_SPEECH_ERR_INVALID = -1,
  TURBO_SPEECH_ERR_NOMEM = -2,
  TURBO_SPEECH_ERR_STATE = -3,
  TURBO_SPEECH_ERR_FORMAT = -4,
  TURBO_SPEECH_ERR_BUSY = -5,
  TURBO_SPEECH_ERR_PROVIDER = -6
} turbo_speech_result_t;

typedef enum {
  TURBO_SPEECH_STATE_IDLE = 0,
  TURBO_SPEECH_STATE_RUNNING,
  TURBO_SPEECH_STATE_FINISHING,
  TURBO_SPEECH_STATE_STOPPED,
  TURBO_SPEECH_STATE_ERROR
} turbo_speech_state_t;

typedef struct {
  int sample_rate;     /* 8000, 16000, 24000, or 48000 Hz */
  int channels;        /* 1 or 2 */
  int bits_per_sample; /* 16 or 32 */
} turbo_speech_audio_format_t;

typedef struct {
  const uint8_t *data;
  size_t len;
  turbo_speech_audio_format_t format;
  uint64_t timestamp_us;
} turbo_speech_audio_frame_t;

typedef struct {
  turbo_speech_audio_format_t format;
  const char *language; /* Borrowed during start(); NULL selects provider default. */
  int enable_interim_results;
} turbo_asr_config_t;

typedef struct {
  const char *text;
  size_t text_len;
  uint64_t start_time_us;
  uint64_t end_time_us;
  float confidence;
  int is_final;
} turbo_asr_result_t;

typedef void (*turbo_asr_result_cb)(turbo_asr_t *asr, const turbo_asr_result_t *result,
                                    void *user_data);
typedef void (*turbo_asr_complete_cb)(turbo_asr_t *asr, void *user_data);
typedef void (*turbo_asr_error_cb)(turbo_asr_t *asr, int error_code, const char *message,
                                   void *user_data);

typedef struct {
  turbo_asr_result_cb on_result;
  turbo_asr_complete_cb on_complete;
  turbo_asr_error_cb on_error;
} turbo_asr_callbacks_t;

typedef void (*turbo_asr_provider_result_cb)(const turbo_asr_result_t *result, void *user_data);
typedef void (*turbo_speech_provider_complete_cb)(void *user_data);
typedef void (*turbo_speech_provider_error_cb)(int error_code, const char *message,
                                               void *user_data);

typedef struct {
  turbo_asr_provider_result_cb on_result;
  turbo_speech_provider_complete_cb on_complete;
  turbo_speech_provider_error_cb on_error;
} turbo_asr_provider_callbacks_t;

typedef struct {
  uint32_t abi_version;
  void *context;
  int (*start)(void *context, const turbo_asr_config_t *config,
               const turbo_asr_provider_callbacks_t *callbacks, void *callback_user_data);
  int (*write)(void *context, const turbo_speech_audio_frame_t *frame);
  int (*finish)(void *context);
  int (*cancel)(void *context);
  void (*destroy)(void *context);
} turbo_asr_provider_t;

/*
 * Provider callbacks may run on a provider thread, but a provider must
 * serialize callbacks for one session. cancel() must not return until all
 * callbacks have quiesced. Public lifecycle calls are serialized by the
 * session owner and must not be called reentrantly from a callback; callbacks
 * schedule such work onto the owner's control thread. destroy() is called only
 * after the cancellation boundary.
 */

/**
 * Creates an ASR session and assumes ownership of provider.context on success.
 * On validation or allocation failure, ownership remains with the caller.
 */
TURBO_MEDIA_API turbo_asr_t *turbo_asr_create(const turbo_asr_provider_t *provider,
                                        const turbo_asr_callbacks_t *callbacks, void *user_data);
TURBO_MEDIA_API void turbo_asr_destroy(turbo_asr_t *asr);
TURBO_MEDIA_API int turbo_asr_start(turbo_asr_t *asr, const turbo_asr_config_t *config);

/**
 * Submits a borrowed PCM frame. write() must not block the capture thread;
 * asynchronous providers copy the frame or return TURBO_SPEECH_ERR_BUSY.
 */
TURBO_MEDIA_API int turbo_asr_write_frame(turbo_asr_t *asr, const turbo_speech_audio_frame_t *frame);
TURBO_MEDIA_API int turbo_asr_write_pcm(turbo_asr_t *asr, const uint8_t *samples, size_t len,
                                  uint64_t timestamp_us);
TURBO_MEDIA_API int turbo_asr_finish(turbo_asr_t *asr);
TURBO_MEDIA_API int turbo_asr_cancel(turbo_asr_t *asr);
TURBO_MEDIA_API turbo_speech_state_t turbo_asr_get_state(const turbo_asr_t *asr);
TURBO_MEDIA_API int turbo_asr_get_last_result(const turbo_asr_t *asr);
TURBO_MEDIA_API uint64_t turbo_asr_get_rejected_frame_count(const turbo_asr_t *asr);
TURBO_MEDIA_API int turbo_asr_get_audio_format(const turbo_asr_t *asr,
                                         turbo_speech_audio_format_t *format);

/**
 * turbo_audio_capture_cb-compatible adapter for an ASR-only capture. For a
 * WebRTC microphone track use turbo_media_track_attach_asr(), which preserves
 * the track's primary capture callback.
 */
TURBO_MEDIA_API void turbo_asr_capture_callback(struct turbo_capture_s *capture, const uint8_t *samples,
                                          size_t len, uint64_t timestamp_us, void *user_data);

typedef struct {
  const char *text;
  size_t text_len;
  const char *voice;    /* Borrowed during synthesize(). */
  const char *language; /* Borrowed during synthesize(). */
  float rate;           /* 1.0 is provider default speed. */
  float pitch;          /* 1.0 is provider default pitch. */
} turbo_tts_request_t;

typedef int (*turbo_tts_audio_cb)(turbo_tts_t *tts, const turbo_speech_audio_frame_t *frame,
                                  void *user_data);
typedef void (*turbo_tts_complete_cb)(turbo_tts_t *tts, void *user_data);
typedef void (*turbo_tts_error_cb)(turbo_tts_t *tts, int error_code, const char *message,
                                   void *user_data);

typedef struct {
  turbo_tts_audio_cb on_audio;
  turbo_tts_complete_cb on_complete;
  turbo_tts_error_cb on_error;
} turbo_tts_callbacks_t;

typedef int (*turbo_tts_provider_audio_cb)(const turbo_speech_audio_frame_t *frame,
                                           void *user_data);

typedef struct {
  turbo_tts_provider_audio_cb on_audio;
  turbo_speech_provider_complete_cb on_complete;
  turbo_speech_provider_error_cb on_error;
} turbo_tts_provider_callbacks_t;

typedef struct {
  uint32_t abi_version;
  void *context;
  int (*synthesize)(void *context, const turbo_tts_request_t *request,
                    const turbo_tts_provider_callbacks_t *callbacks, void *callback_user_data);
  int (*cancel)(void *context);
  void (*destroy)(void *context);
} turbo_tts_provider_t;

/* TTS providers follow the same callback serialization/quiescence contract. */

/** Same provider ownership rule as turbo_asr_create(). */
TURBO_MEDIA_API turbo_tts_t *turbo_tts_create(const turbo_tts_provider_t *provider,
                                        const turbo_tts_callbacks_t *callbacks, void *user_data);
TURBO_MEDIA_API void turbo_tts_destroy(turbo_tts_t *tts);
TURBO_MEDIA_API int turbo_tts_synthesize(turbo_tts_t *tts, const turbo_tts_request_t *request);
TURBO_MEDIA_API int turbo_tts_cancel(turbo_tts_t *tts);
TURBO_MEDIA_API turbo_speech_state_t turbo_tts_get_state(const turbo_tts_t *tts);
TURBO_MEDIA_API int turbo_tts_get_last_result(const turbo_tts_t *tts);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SPEECH_H */
