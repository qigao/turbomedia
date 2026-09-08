/**
 * Provider-neutral voice activity, biometric voiceprint, and perceptual media
 * fingerprint interfaces.
 *
 * Frames, labels, model identifiers, and fingerprint blobs are borrowed views
 * valid only for the duration of the call. Asynchronous providers copy them
 * into provider-owned bounded storage before returning.
 *
 * A successful create transfers ownership of provider.context to the created
 * handle; provider.destroy is called exactly once. Ownership remains with the
 * caller when create fails. Provider callbacks for one handle must be
 * serialized. The application serializes lifecycle calls and treats cancel or
 * a completed finish as the quiescence boundary before destroying the handle.
 * Matcher compare is synchronous and must likewise be serialized by its owner.
 */
#ifndef TURBO_RECOGNITION_H
#define TURBO_RECOGNITION_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>
#if defined(TURBO_MEDIA_PRODUCT_CLIENT)
#include <turbo_player.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_RECOGNITION_PROVIDER_ABI_VERSION 1U
#define TURBO_RECOGNITION_MIN_SAMPLE_RATE 8000
#define TURBO_RECOGNITION_MAX_SAMPLE_RATE 192000
#define TURBO_RECOGNITION_MAX_AUDIO_CHANNELS 8
#define TURBO_RECOGNITION_MAX_VIDEO_DIMENSION 32768
#define TURBO_FINGERPRINT_MAX_ALGORITHM_LENGTH 63U
#define TURBO_FINGERPRINT_MAX_MODEL_VERSION_LENGTH 63U
#define TURBO_FINGERPRINT_MAX_BYTES 65536U
#define TURBO_FINGERPRINT_MAX_DURATION_US UINT64_C(86400000000)

typedef struct turbo_voice_detector_s turbo_voice_detector_t;
typedef struct turbo_fingerprint_extractor_s turbo_fingerprint_extractor_t;
typedef struct turbo_fingerprint_matcher_s turbo_fingerprint_matcher_t;
typedef struct salts_capture_s salts_capture_t;

typedef enum {
  TURBO_RECOGNITION_OK = 0,
  TURBO_RECOGNITION_ERR_INVALID = -1,
  TURBO_RECOGNITION_ERR_NOMEM = -2,
  TURBO_RECOGNITION_ERR_STATE = -3,
  TURBO_RECOGNITION_ERR_FORMAT = -4,
  TURBO_RECOGNITION_ERR_BUSY = -5,
  TURBO_RECOGNITION_ERR_PROVIDER = -6,
  TURBO_RECOGNITION_ERR_INCOMPATIBLE = -7,
  TURBO_RECOGNITION_ERR_LIMIT = -8
} turbo_recognition_result_t;

typedef enum {
  TURBO_RECOGNITION_STATE_IDLE = 0,
  TURBO_RECOGNITION_STATE_RUNNING,
  TURBO_RECOGNITION_STATE_FINISHING,
  TURBO_RECOGNITION_STATE_STOPPED,
  TURBO_RECOGNITION_STATE_ERROR
} turbo_recognition_state_t;

typedef enum {
  TURBO_RECOGNITION_AUDIO_S16 = 1,
  TURBO_RECOGNITION_AUDIO_S32,
  TURBO_RECOGNITION_AUDIO_F32
} turbo_recognition_audio_sample_format_t;

typedef struct {
  int sample_rate;
  int channels;
  turbo_recognition_audio_sample_format_t sample_format;
} turbo_recognition_audio_format_t;

typedef struct {
  const uint8_t *data;
  size_t len;
  turbo_recognition_audio_format_t format;
  uint64_t timestamp_us;
} turbo_recognition_audio_frame_t;

typedef enum {
  TURBO_RECOGNITION_VIDEO_I420 = 1,
  TURBO_RECOGNITION_VIDEO_NV12,
  TURBO_RECOGNITION_VIDEO_RGBA,
  TURBO_RECOGNITION_VIDEO_BGRA
} turbo_recognition_video_pixel_format_t;

typedef struct {
  const uint8_t *data;
  size_t len;
  int width;
  int height;
  int stride[4];
  turbo_recognition_video_pixel_format_t pixel_format;
  uint64_t timestamp_us;
} turbo_recognition_video_frame_t;

typedef struct {
  turbo_recognition_audio_format_t format;
  uint32_t min_speech_ms;
  uint32_t min_silence_ms;
} turbo_voice_detector_config_t;

typedef enum {
  TURBO_VOICE_ACTIVITY_SILENCE = 0,
  TURBO_VOICE_ACTIVITY_SPEECH = 1
} turbo_voice_activity_state_t;

typedef struct {
  turbo_voice_activity_state_t state;
  uint64_t start_time_us;
  uint64_t end_time_us;
  float probability;
  int is_final;
} turbo_voice_activity_t;

typedef void (*turbo_voice_activity_cb)(turbo_voice_detector_t *detector,
                                        const turbo_voice_activity_t *activity, void *user_data);
typedef void (*turbo_voice_detector_complete_cb)(turbo_voice_detector_t *detector, void *user_data);
typedef void (*turbo_voice_detector_error_cb)(turbo_voice_detector_t *detector, int error_code,
                                              const char *message, void *user_data);

typedef struct {
  turbo_voice_activity_cb on_activity;
  turbo_voice_detector_complete_cb on_complete;
  turbo_voice_detector_error_cb on_error;
} turbo_voice_detector_callbacks_t;

typedef void (*turbo_voice_provider_activity_cb)(const turbo_voice_activity_t *activity,
                                                 void *user_data);
typedef void (*turbo_recognition_provider_complete_cb)(void *user_data);
typedef void (*turbo_recognition_provider_error_cb)(int error_code, const char *message,
                                                    void *user_data);

typedef struct {
  turbo_voice_provider_activity_cb on_activity;
  turbo_recognition_provider_complete_cb on_complete;
  turbo_recognition_provider_error_cb on_error;
} turbo_voice_detector_provider_callbacks_t;

typedef struct {
  uint32_t abi_version;
  void *context;
  int (*start)(void *context, const turbo_voice_detector_config_t *config,
               const turbo_voice_detector_provider_callbacks_t *callbacks,
               void *callback_user_data);
  int (*write)(void *context, const turbo_recognition_audio_frame_t *frame);
  int (*finish)(void *context);
  int (*cancel)(void *context);
  void (*destroy)(void *context);
} turbo_voice_detector_provider_t;

TURBO_MEDIA_API turbo_voice_detector_t *
turbo_voice_detector_create(const turbo_voice_detector_provider_t *provider,
                            const turbo_voice_detector_callbacks_t *callbacks, void *user_data);
TURBO_MEDIA_API void turbo_voice_detector_destroy(turbo_voice_detector_t *detector);
TURBO_MEDIA_API int turbo_voice_detector_start(turbo_voice_detector_t *detector,
                                         const turbo_voice_detector_config_t *config);
TURBO_MEDIA_API int turbo_voice_detector_write(turbo_voice_detector_t *detector,
                                         const turbo_recognition_audio_frame_t *frame);
TURBO_MEDIA_API int turbo_voice_detector_finish(turbo_voice_detector_t *detector);
TURBO_MEDIA_API int turbo_voice_detector_cancel(turbo_voice_detector_t *detector);
TURBO_MEDIA_API turbo_recognition_state_t
turbo_voice_detector_get_state(const turbo_voice_detector_t *detector);
TURBO_MEDIA_API int turbo_voice_detector_get_last_result(const turbo_voice_detector_t *detector);
TURBO_MEDIA_API uint64_t
turbo_voice_detector_get_rejected_frame_count(const turbo_voice_detector_t *detector);
TURBO_MEDIA_API int turbo_voice_detector_get_audio_format(const turbo_voice_detector_t *detector,
                                                    turbo_recognition_audio_format_t *format);

/** salts_audio_capture_cb-compatible adapter for detector-only capture. */
TURBO_MEDIA_API void turbo_voice_detector_capture_callback(salts_capture_t *capture,
                                                     const uint8_t *samples, size_t len,
                                                     uint64_t timestamp_us, void *user_data);

#if defined(TURBO_MEDIA_PRODUCT_CLIENT)
/** turbo_player_audio_cb-compatible adapter for offline/VOD analysis. */
TURBO_MEDIA_API void turbo_voice_detector_player_audio_callback(turbo_player_t *player,
                                                          const float *samples, size_t frame_count,
                                                          int sample_rate, int channels,
                                                          int64_t pts_ms, void *user_data);
#endif

typedef enum {
  TURBO_FINGERPRINT_VOICE = 1,
  TURBO_FINGERPRINT_AUDIO_CONTENT,
  TURBO_FINGERPRINT_VIDEO_CONTENT
} turbo_fingerprint_domain_t;

typedef struct {
  turbo_fingerprint_domain_t domain;
  turbo_recognition_audio_format_t audio_format;
  turbo_recognition_video_pixel_format_t video_format;
  const char *algorithm_hint;
  uint64_t max_duration_us;
} turbo_fingerprint_config_t;

typedef struct {
  turbo_fingerprint_domain_t domain;
  const char *algorithm;
  const char *model_version;
  const uint8_t *data;
  size_t len;
  uint64_t duration_us;
} turbo_fingerprint_t;

typedef void (*turbo_fingerprint_result_cb)(turbo_fingerprint_extractor_t *extractor,
                                            const turbo_fingerprint_t *fingerprint,
                                            void *user_data);
typedef void (*turbo_fingerprint_complete_cb)(turbo_fingerprint_extractor_t *extractor,
                                              void *user_data);
typedef void (*turbo_fingerprint_error_cb)(turbo_fingerprint_extractor_t *extractor, int error_code,
                                           const char *message, void *user_data);

typedef struct {
  turbo_fingerprint_result_cb on_result;
  turbo_fingerprint_complete_cb on_complete;
  turbo_fingerprint_error_cb on_error;
} turbo_fingerprint_callbacks_t;

typedef void (*turbo_fingerprint_provider_result_cb)(const turbo_fingerprint_t *fingerprint,
                                                     void *user_data);

typedef struct {
  turbo_fingerprint_provider_result_cb on_result;
  turbo_recognition_provider_complete_cb on_complete;
  turbo_recognition_provider_error_cb on_error;
} turbo_fingerprint_provider_callbacks_t;

typedef struct {
  uint32_t abi_version;
  void *context;
  int (*start)(void *context, const turbo_fingerprint_config_t *config,
               const turbo_fingerprint_provider_callbacks_t *callbacks, void *callback_user_data);
  int (*write_audio)(void *context, const turbo_recognition_audio_frame_t *frame);
  int (*write_video)(void *context, const turbo_recognition_video_frame_t *frame);
  int (*finish)(void *context);
  int (*cancel)(void *context);
  void (*destroy)(void *context);
} turbo_fingerprint_provider_t;

TURBO_MEDIA_API turbo_fingerprint_extractor_t *
turbo_fingerprint_extractor_create(const turbo_fingerprint_provider_t *provider,
                                   const turbo_fingerprint_callbacks_t *callbacks, void *user_data);
TURBO_MEDIA_API void turbo_fingerprint_extractor_destroy(turbo_fingerprint_extractor_t *extractor);
TURBO_MEDIA_API int turbo_fingerprint_extractor_start(turbo_fingerprint_extractor_t *extractor,
                                                const turbo_fingerprint_config_t *config);
TURBO_MEDIA_API int turbo_fingerprint_extractor_write_audio(turbo_fingerprint_extractor_t *extractor,
                                                      const turbo_recognition_audio_frame_t *frame);
TURBO_MEDIA_API int turbo_fingerprint_extractor_write_video(turbo_fingerprint_extractor_t *extractor,
                                                      const turbo_recognition_video_frame_t *frame);
TURBO_MEDIA_API int turbo_fingerprint_extractor_finish(turbo_fingerprint_extractor_t *extractor);
TURBO_MEDIA_API int turbo_fingerprint_extractor_cancel(turbo_fingerprint_extractor_t *extractor);
TURBO_MEDIA_API turbo_recognition_state_t
turbo_fingerprint_extractor_get_state(const turbo_fingerprint_extractor_t *extractor);
TURBO_MEDIA_API int
turbo_fingerprint_extractor_get_last_result(const turbo_fingerprint_extractor_t *extractor);
TURBO_MEDIA_API uint64_t turbo_fingerprint_extractor_get_rejected_frame_count(
    const turbo_fingerprint_extractor_t *extractor);
TURBO_MEDIA_API turbo_fingerprint_domain_t
turbo_fingerprint_extractor_get_domain(const turbo_fingerprint_extractor_t *extractor);
TURBO_MEDIA_API int
turbo_fingerprint_extractor_get_audio_format(const turbo_fingerprint_extractor_t *extractor,
                                             turbo_recognition_audio_format_t *format);

/** salts_audio_capture_cb-compatible adapter for voice/audio fingerprints. */
TURBO_MEDIA_API void turbo_fingerprint_capture_callback(salts_capture_t *capture,
                                                  const uint8_t *samples, size_t len,
                                                  uint64_t timestamp_us, void *user_data);

#if defined(TURBO_MEDIA_PRODUCT_CLIENT)
/** turbo_player callbacks for offline/VOD audio or video fingerprinting. */
TURBO_MEDIA_API void turbo_fingerprint_player_audio_callback(turbo_player_t *player, const float *samples,
                                                       size_t frame_count, int sample_rate,
                                                       int channels, int64_t pts_ms,
                                                       void *user_data);
TURBO_MEDIA_API void turbo_fingerprint_player_video_callback(turbo_player_t *player,
                                                       const turbo_player_video_frame_t *frame,
                                                       void *user_data);
#endif

typedef struct {
  uint32_t abi_version;
  void *context;
  int (*compare)(void *context, const turbo_fingerprint_t *left, const turbo_fingerprint_t *right,
                 float *similarity);
  void (*destroy)(void *context);
} turbo_fingerprint_matcher_provider_t;

typedef struct {
  float similarity;
  float threshold;
  int is_match;
} turbo_fingerprint_match_result_t;

TURBO_MEDIA_API turbo_fingerprint_matcher_t *
turbo_fingerprint_matcher_create(const turbo_fingerprint_matcher_provider_t *provider);
TURBO_MEDIA_API void turbo_fingerprint_matcher_destroy(turbo_fingerprint_matcher_t *matcher);
TURBO_MEDIA_API int turbo_fingerprint_match(turbo_fingerprint_matcher_t *matcher,
                                      const turbo_fingerprint_t *left,
                                      const turbo_fingerprint_t *right, float threshold,
                                      turbo_fingerprint_match_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_RECOGNITION_H */
