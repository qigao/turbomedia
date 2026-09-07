#include "turbo_recognition.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct turbo_voice_detector_s {
  turbo_voice_detector_provider_t provider;
  turbo_voice_detector_callbacks_t callbacks;
  void *user_data;
  turbo_recognition_audio_format_t format;
  _Atomic int state;
  _Atomic int last_result;
  _Atomic uint64_t rejected_frame_count;
};

struct turbo_fingerprint_extractor_s {
  turbo_fingerprint_provider_t provider;
  turbo_fingerprint_callbacks_t callbacks;
  void *user_data;
  turbo_fingerprint_config_t config;
  uint64_t consumed_duration_us;
  uint64_t first_video_timestamp_us;
  int has_video_timestamp;
  _Atomic int state;
  _Atomic int last_result;
  _Atomic uint64_t rejected_frame_count;
};

struct turbo_fingerprint_matcher_s {
  turbo_fingerprint_matcher_provider_t provider;
};

static int audio_format_valid(const turbo_recognition_audio_format_t *format) {
  if (!format || format->sample_rate < TURBO_RECOGNITION_MIN_SAMPLE_RATE ||
      format->sample_rate > TURBO_RECOGNITION_MAX_SAMPLE_RATE || format->channels < 1 ||
      format->channels > TURBO_RECOGNITION_MAX_AUDIO_CHANNELS) {
    return 0;
  }
  return format->sample_format >= TURBO_RECOGNITION_AUDIO_S16 &&
         format->sample_format <= TURBO_RECOGNITION_AUDIO_F32;
}

static size_t audio_sample_size(turbo_recognition_audio_sample_format_t format) {
  return format == TURBO_RECOGNITION_AUDIO_S16 ? 2U : 4U;
}

static int audio_format_equal(const turbo_recognition_audio_format_t *left,
                              const turbo_recognition_audio_format_t *right) {
  return left->sample_rate == right->sample_rate && left->channels == right->channels &&
         left->sample_format == right->sample_format;
}

static int audio_frame_valid(const turbo_recognition_audio_frame_t *frame) {
  size_t frame_bytes;
  if (!frame || !frame->data || frame->len == 0 || !audio_format_valid(&frame->format)) return 0;
  frame_bytes = audio_sample_size(frame->format.sample_format) * (size_t)frame->format.channels;
  return frame_bytes != 0 && frame->len % frame_bytes == 0;
}

static int video_format_valid(turbo_recognition_video_pixel_format_t format) {
  return format >= TURBO_RECOGNITION_VIDEO_I420 && format <= TURBO_RECOGNITION_VIDEO_BGRA;
}

static int video_frame_valid(const turbo_recognition_video_frame_t *frame) {
  if (!frame || !frame->data || frame->len == 0 || frame->width <= 0 || frame->height <= 0 ||
      frame->width > TURBO_RECOGNITION_MAX_VIDEO_DIMENSION ||
      frame->height > TURBO_RECOGNITION_MAX_VIDEO_DIMENSION ||
      !video_format_valid(frame->pixel_format)) {
    return 0;
  }
  return frame->stride[0] > 0;
}

static uint64_t timestamp_ms_to_us(int64_t timestamp_ms) {
  if (timestamp_ms <= 0) return 0U;
  if ((uint64_t)timestamp_ms > UINT64_MAX / 1000U) return UINT64_MAX;
  return (uint64_t)timestamp_ms * 1000U;
}

static int session_active(int state) {
  return state == TURBO_RECOGNITION_STATE_RUNNING || state == TURBO_RECOGNITION_STATE_FINISHING;
}

static int bounded_string_valid(const char *value, size_t max_length, int optional) {
  size_t length = 0;
  if (!value) return optional;
  while (length <= max_length && value[length])
    ++length;
  return (optional || length > 0) && length <= max_length;
}

static int voice_reject(turbo_voice_detector_t *detector, int result) {
  atomic_fetch_add(&detector->rejected_frame_count, 1U);
  atomic_store(&detector->last_result, result);
  return result;
}

static int fingerprint_reject(turbo_fingerprint_extractor_t *extractor, int result) {
  atomic_fetch_add(&extractor->rejected_frame_count, 1U);
  atomic_store(&extractor->last_result, result);
  return result;
}

static void voice_provider_activity(const turbo_voice_activity_t *activity, void *user_data) {
  turbo_voice_detector_t *detector = (turbo_voice_detector_t *)user_data;
  int state;
  if (!detector) return;
  state = atomic_load(&detector->state);
  if (!session_active(state)) return;
  if (!activity || activity->state < TURBO_VOICE_ACTIVITY_SILENCE ||
      activity->state > TURBO_VOICE_ACTIVITY_SPEECH || activity->probability < 0.0f ||
      activity->probability > 1.0f || !isfinite(activity->probability) ||
      activity->end_time_us < activity->start_time_us) {
    atomic_store(&detector->state, TURBO_RECOGNITION_STATE_ERROR);
    atomic_store(&detector->last_result, TURBO_RECOGNITION_ERR_PROVIDER);
    if (detector->callbacks.on_error) {
      detector->callbacks.on_error(detector, TURBO_RECOGNITION_ERR_PROVIDER,
                                   "provider returned invalid voice activity", detector->user_data);
    }
    return;
  }
  if (detector->callbacks.on_activity) {
    detector->callbacks.on_activity(detector, activity, detector->user_data);
  }
}

static void voice_provider_complete(void *user_data) {
  turbo_voice_detector_t *detector = (turbo_voice_detector_t *)user_data;
  if (!detector || !session_active(atomic_load(&detector->state))) return;
  atomic_store(&detector->state, TURBO_RECOGNITION_STATE_STOPPED);
  atomic_store(&detector->last_result, TURBO_RECOGNITION_OK);
  if (detector->callbacks.on_complete) {
    detector->callbacks.on_complete(detector, detector->user_data);
  }
}

static void voice_provider_error(int error_code, const char *message, void *user_data) {
  turbo_voice_detector_t *detector = (turbo_voice_detector_t *)user_data;
  int result;
  if (!detector || !session_active(atomic_load(&detector->state))) return;
  result = error_code == TURBO_RECOGNITION_OK ? TURBO_RECOGNITION_ERR_PROVIDER : error_code;
  atomic_store(&detector->state, TURBO_RECOGNITION_STATE_ERROR);
  atomic_store(&detector->last_result, result);
  if (detector->callbacks.on_error) {
    detector->callbacks.on_error(detector, result, message, detector->user_data);
  }
}

static const turbo_voice_detector_provider_callbacks_t voice_provider_callbacks = {
    voice_provider_activity, voice_provider_complete, voice_provider_error};

turbo_voice_detector_t *
turbo_voice_detector_create(const turbo_voice_detector_provider_t *provider,
                            const turbo_voice_detector_callbacks_t *callbacks, void *user_data) {
  turbo_voice_detector_t *detector;
  if (!provider || provider->abi_version != TURBO_RECOGNITION_PROVIDER_ABI_VERSION ||
      !provider->start || !provider->write || !provider->finish || !provider->cancel ||
      !provider->destroy) {
    return NULL;
  }
  detector = (turbo_voice_detector_t *)calloc(1, sizeof(*detector));
  if (!detector) return NULL;
  detector->provider = *provider;
  if (callbacks) detector->callbacks = *callbacks;
  detector->user_data = user_data;
  atomic_init(&detector->state, TURBO_RECOGNITION_STATE_IDLE);
  atomic_init(&detector->last_result, TURBO_RECOGNITION_OK);
  atomic_init(&detector->rejected_frame_count, 0U);
  return detector;
}

void turbo_voice_detector_destroy(turbo_voice_detector_t *detector) {
  int state;
  if (!detector) return;
  state = atomic_load(&detector->state);
  if (session_active(state) || state == TURBO_RECOGNITION_STATE_ERROR) {
    (void)detector->provider.cancel(detector->provider.context);
  }
  detector->provider.destroy(detector->provider.context);
  free(detector);
}

int turbo_voice_detector_start(turbo_voice_detector_t *detector,
                               const turbo_voice_detector_config_t *config) {
  int result;
  int state;
  if (!detector || !config || !audio_format_valid(&config->format) || config->format.channels > 2 ||
      config->min_speech_ms > 60000U || config->min_silence_ms > 60000U) {
    return TURBO_RECOGNITION_ERR_INVALID;
  }
  state = atomic_load(&detector->state);
  if (state != TURBO_RECOGNITION_STATE_IDLE && state != TURBO_RECOGNITION_STATE_STOPPED) {
    return TURBO_RECOGNITION_ERR_STATE;
  }
  detector->format = config->format;
  atomic_store(&detector->rejected_frame_count, 0U);
  atomic_store(&detector->state, TURBO_RECOGNITION_STATE_RUNNING);
  result = detector->provider.start(detector->provider.context, config, &voice_provider_callbacks,
                                    detector);
  if (result != TURBO_RECOGNITION_OK) {
    atomic_store(&detector->last_result, result);
    if (atomic_load(&detector->state) == TURBO_RECOGNITION_STATE_RUNNING) {
      atomic_store(&detector->state, TURBO_RECOGNITION_STATE_IDLE);
    }
  } else {
    state = atomic_load(&detector->state);
    if (state == TURBO_RECOGNITION_STATE_ERROR) {
      result = atomic_load(&detector->last_result);
    } else if (state == TURBO_RECOGNITION_STATE_RUNNING) {
      atomic_store(&detector->last_result, TURBO_RECOGNITION_OK);
    }
  }
  return result;
}

int turbo_voice_detector_write(turbo_voice_detector_t *detector,
                               const turbo_recognition_audio_frame_t *frame) {
  int result;
  int state;
  if (!detector) return TURBO_RECOGNITION_ERR_INVALID;
  if (!audio_frame_valid(frame)) {
    return voice_reject(detector, TURBO_RECOGNITION_ERR_INVALID);
  }
  if (atomic_load(&detector->state) != TURBO_RECOGNITION_STATE_RUNNING) {
    return voice_reject(detector, TURBO_RECOGNITION_ERR_STATE);
  }
  if (!audio_format_equal(&detector->format, &frame->format)) {
    return voice_reject(detector, TURBO_RECOGNITION_ERR_FORMAT);
  }
  result = detector->provider.write(detector->provider.context, frame);
  state = atomic_load(&detector->state);
  if (state == TURBO_RECOGNITION_STATE_ERROR) {
    return atomic_load(&detector->last_result);
  }
  if (result != TURBO_RECOGNITION_OK) {
    atomic_fetch_add(&detector->rejected_frame_count, 1U);
  }
  atomic_store(&detector->last_result, result);
  return result;
}

int turbo_voice_detector_finish(turbo_voice_detector_t *detector) {
  int result;
  if (!detector) return TURBO_RECOGNITION_ERR_INVALID;
  if (atomic_load(&detector->state) != TURBO_RECOGNITION_STATE_RUNNING) {
    return TURBO_RECOGNITION_ERR_STATE;
  }
  atomic_store(&detector->state, TURBO_RECOGNITION_STATE_FINISHING);
  result = detector->provider.finish(detector->provider.context);
  if (result != TURBO_RECOGNITION_OK) {
    atomic_store(&detector->last_result, result);
    if (atomic_load(&detector->state) == TURBO_RECOGNITION_STATE_FINISHING) {
      atomic_store(&detector->state, TURBO_RECOGNITION_STATE_RUNNING);
    }
  } else if (atomic_load(&detector->state) == TURBO_RECOGNITION_STATE_ERROR) {
    result = atomic_load(&detector->last_result);
  }
  return result;
}

int turbo_voice_detector_cancel(turbo_voice_detector_t *detector) {
  int result;
  int state;
  if (!detector) return TURBO_RECOGNITION_ERR_INVALID;
  state = atomic_load(&detector->state);
  if (!session_active(state) && state != TURBO_RECOGNITION_STATE_ERROR) {
    return TURBO_RECOGNITION_ERR_STATE;
  }
  result = detector->provider.cancel(detector->provider.context);
  atomic_store(&detector->last_result, result);
  if (result == TURBO_RECOGNITION_OK) {
    atomic_store(&detector->state, TURBO_RECOGNITION_STATE_STOPPED);
  }
  return result;
}

turbo_recognition_state_t turbo_voice_detector_get_state(const turbo_voice_detector_t *detector) {
  return detector ? (turbo_recognition_state_t)atomic_load(&detector->state)
                  : TURBO_RECOGNITION_STATE_ERROR;
}

int turbo_voice_detector_get_last_result(const turbo_voice_detector_t *detector) {
  return detector ? atomic_load(&detector->last_result) : TURBO_RECOGNITION_ERR_INVALID;
}

uint64_t turbo_voice_detector_get_rejected_frame_count(const turbo_voice_detector_t *detector) {
  return detector ? atomic_load(&detector->rejected_frame_count) : 0U;
}

int turbo_voice_detector_get_audio_format(const turbo_voice_detector_t *detector,
                                          turbo_recognition_audio_format_t *format) {
  if (!detector || !format || !session_active(atomic_load(&detector->state))) {
    return detector && format ? TURBO_RECOGNITION_ERR_STATE : TURBO_RECOGNITION_ERR_INVALID;
  }
  *format = detector->format;
  return TURBO_RECOGNITION_OK;
}

void turbo_voice_detector_capture_callback(salts_capture_t *capture, const uint8_t *samples,
                                           size_t len, uint64_t timestamp_us, void *user_data) {
  turbo_voice_detector_t *detector = (turbo_voice_detector_t *)user_data;
  turbo_recognition_audio_frame_t frame;
  (void)capture;
  if (!detector ||
      turbo_voice_detector_get_audio_format(detector, &frame.format) != TURBO_RECOGNITION_OK) {
    return;
  }
  frame.data = samples;
  frame.len = len;
  frame.timestamp_us = timestamp_us;
  (void)turbo_voice_detector_write(detector, &frame);
}

void turbo_voice_detector_player_audio_callback(turbo_player_t *player, const float *samples,
                                                size_t frame_count, int sample_rate, int channels,
                                                int64_t pts_ms, void *user_data) {
  turbo_recognition_audio_frame_t frame;
  (void)player;
  if (!samples || frame_count == 0 || channels <= 0 ||
      frame_count > SIZE_MAX / (size_t)channels / sizeof(float)) {
    return;
  }
  frame.data = (const uint8_t *)samples;
  frame.len = frame_count * (size_t)channels * sizeof(float);
  frame.format.sample_rate = sample_rate;
  frame.format.channels = channels;
  frame.format.sample_format = TURBO_RECOGNITION_AUDIO_F32;
  frame.timestamp_us = timestamp_ms_to_us(pts_ms);
  (void)turbo_voice_detector_write((turbo_voice_detector_t *)user_data, &frame);
}

static int fingerprint_domain_valid(turbo_fingerprint_domain_t domain) {
  return domain >= TURBO_FINGERPRINT_VOICE && domain <= TURBO_FINGERPRINT_VIDEO_CONTENT;
}

static int fingerprint_value_valid(const turbo_fingerprint_t *fingerprint) {
  return fingerprint && fingerprint_domain_valid(fingerprint->domain) &&
         bounded_string_valid(fingerprint->algorithm, TURBO_FINGERPRINT_MAX_ALGORITHM_LENGTH, 0) &&
         bounded_string_valid(fingerprint->model_version,
                              TURBO_FINGERPRINT_MAX_MODEL_VERSION_LENGTH, 1) &&
         fingerprint->data && fingerprint->len > 0 &&
         fingerprint->len <= TURBO_FINGERPRINT_MAX_BYTES;
}

static void fingerprint_provider_result(const turbo_fingerprint_t *fingerprint, void *user_data) {
  turbo_fingerprint_extractor_t *extractor = (turbo_fingerprint_extractor_t *)user_data;
  if (!extractor || !session_active(atomic_load(&extractor->state))) return;
  if (!fingerprint_value_valid(fingerprint) || fingerprint->domain != extractor->config.domain) {
    atomic_store(&extractor->state, TURBO_RECOGNITION_STATE_ERROR);
    atomic_store(&extractor->last_result, TURBO_RECOGNITION_ERR_PROVIDER);
    if (extractor->callbacks.on_error) {
      extractor->callbacks.on_error(extractor, TURBO_RECOGNITION_ERR_PROVIDER,
                                    "provider returned an invalid fingerprint",
                                    extractor->user_data);
    }
    return;
  }
  if (extractor->callbacks.on_result) {
    extractor->callbacks.on_result(extractor, fingerprint, extractor->user_data);
  }
}

static void fingerprint_provider_complete(void *user_data) {
  turbo_fingerprint_extractor_t *extractor = (turbo_fingerprint_extractor_t *)user_data;
  if (!extractor || !session_active(atomic_load(&extractor->state))) return;
  atomic_store(&extractor->state, TURBO_RECOGNITION_STATE_STOPPED);
  atomic_store(&extractor->last_result, TURBO_RECOGNITION_OK);
  if (extractor->callbacks.on_complete) {
    extractor->callbacks.on_complete(extractor, extractor->user_data);
  }
}

static void fingerprint_provider_error(int error_code, const char *message, void *user_data) {
  turbo_fingerprint_extractor_t *extractor = (turbo_fingerprint_extractor_t *)user_data;
  int result;
  if (!extractor || !session_active(atomic_load(&extractor->state))) return;
  result = error_code == TURBO_RECOGNITION_OK ? TURBO_RECOGNITION_ERR_PROVIDER : error_code;
  atomic_store(&extractor->state, TURBO_RECOGNITION_STATE_ERROR);
  atomic_store(&extractor->last_result, result);
  if (extractor->callbacks.on_error) {
    extractor->callbacks.on_error(extractor, result, message, extractor->user_data);
  }
}

static const turbo_fingerprint_provider_callbacks_t fingerprint_provider_callbacks = {
    fingerprint_provider_result, fingerprint_provider_complete, fingerprint_provider_error};

turbo_fingerprint_extractor_t *
turbo_fingerprint_extractor_create(const turbo_fingerprint_provider_t *provider,
                                   const turbo_fingerprint_callbacks_t *callbacks,
                                   void *user_data) {
  turbo_fingerprint_extractor_t *extractor;
  if (!provider || provider->abi_version != TURBO_RECOGNITION_PROVIDER_ABI_VERSION ||
      !provider->start || !provider->finish || !provider->cancel || !provider->destroy) {
    return NULL;
  }
  extractor = (turbo_fingerprint_extractor_t *)calloc(1, sizeof(*extractor));
  if (!extractor) return NULL;
  extractor->provider = *provider;
  if (callbacks) extractor->callbacks = *callbacks;
  extractor->user_data = user_data;
  atomic_init(&extractor->state, TURBO_RECOGNITION_STATE_IDLE);
  atomic_init(&extractor->last_result, TURBO_RECOGNITION_OK);
  atomic_init(&extractor->rejected_frame_count, 0U);
  return extractor;
}

void turbo_fingerprint_extractor_destroy(turbo_fingerprint_extractor_t *extractor) {
  int state;
  if (!extractor) return;
  state = atomic_load(&extractor->state);
  if (session_active(state) || state == TURBO_RECOGNITION_STATE_ERROR) {
    (void)extractor->provider.cancel(extractor->provider.context);
  }
  extractor->provider.destroy(extractor->provider.context);
  free(extractor);
}

int turbo_fingerprint_extractor_start(turbo_fingerprint_extractor_t *extractor,
                                      const turbo_fingerprint_config_t *config) {
  int result;
  int state;
  if (!extractor || !config || !fingerprint_domain_valid(config->domain) ||
      !bounded_string_valid(config->algorithm_hint, TURBO_FINGERPRINT_MAX_ALGORITHM_LENGTH, 1) ||
      config->max_duration_us == 0 || config->max_duration_us > TURBO_FINGERPRINT_MAX_DURATION_US) {
    return TURBO_RECOGNITION_ERR_INVALID;
  }
  if ((config->domain == TURBO_FINGERPRINT_VOICE ||
       config->domain == TURBO_FINGERPRINT_AUDIO_CONTENT) &&
      (!audio_format_valid(&config->audio_format) || !extractor->provider.write_audio)) {
    return TURBO_RECOGNITION_ERR_INVALID;
  }
  if (config->domain == TURBO_FINGERPRINT_VIDEO_CONTENT &&
      (!video_format_valid(config->video_format) || !extractor->provider.write_video)) {
    return TURBO_RECOGNITION_ERR_INVALID;
  }
  state = atomic_load(&extractor->state);
  if (state != TURBO_RECOGNITION_STATE_IDLE && state != TURBO_RECOGNITION_STATE_STOPPED) {
    return TURBO_RECOGNITION_ERR_STATE;
  }
  extractor->config = *config;
  extractor->config.algorithm_hint = NULL;
  extractor->consumed_duration_us = 0U;
  extractor->first_video_timestamp_us = 0U;
  extractor->has_video_timestamp = 0;
  atomic_store(&extractor->rejected_frame_count, 0U);
  atomic_store(&extractor->state, TURBO_RECOGNITION_STATE_RUNNING);
  result = extractor->provider.start(extractor->provider.context, config,
                                     &fingerprint_provider_callbacks, extractor);
  if (result != TURBO_RECOGNITION_OK) {
    atomic_store(&extractor->last_result, result);
    if (atomic_load(&extractor->state) == TURBO_RECOGNITION_STATE_RUNNING) {
      atomic_store(&extractor->state, TURBO_RECOGNITION_STATE_IDLE);
    }
  } else {
    state = atomic_load(&extractor->state);
    if (state == TURBO_RECOGNITION_STATE_ERROR) {
      result = atomic_load(&extractor->last_result);
    } else if (state == TURBO_RECOGNITION_STATE_RUNNING) {
      atomic_store(&extractor->last_result, TURBO_RECOGNITION_OK);
    }
  }
  return result;
}

static int fingerprint_audio_duration(const turbo_recognition_audio_frame_t *frame,
                                      uint64_t *duration_us) {
  size_t bytes_per_frame =
      audio_sample_size(frame->format.sample_format) * (size_t)frame->format.channels;
  uint64_t sample_count = (uint64_t)(frame->len / bytes_per_frame);
  if (sample_count > UINT64_MAX / 1000000U) return 0;
  *duration_us = sample_count * 1000000U / (uint64_t)frame->format.sample_rate;
  return 1;
}

int turbo_fingerprint_extractor_write_audio(turbo_fingerprint_extractor_t *extractor,
                                            const turbo_recognition_audio_frame_t *frame) {
  int result;
  uint64_t duration_us;
  if (!extractor) return TURBO_RECOGNITION_ERR_INVALID;
  if (!audio_frame_valid(frame)) {
    return fingerprint_reject(extractor, TURBO_RECOGNITION_ERR_INVALID);
  }
  if (atomic_load(&extractor->state) != TURBO_RECOGNITION_STATE_RUNNING) {
    return fingerprint_reject(extractor, TURBO_RECOGNITION_ERR_STATE);
  }
  if ((extractor->config.domain != TURBO_FINGERPRINT_VOICE &&
       extractor->config.domain != TURBO_FINGERPRINT_AUDIO_CONTENT) ||
      !audio_format_equal(&extractor->config.audio_format, &frame->format)) {
    return fingerprint_reject(extractor, TURBO_RECOGNITION_ERR_FORMAT);
  }
  if (!fingerprint_audio_duration(frame, &duration_us) ||
      UINT64_MAX - extractor->consumed_duration_us < duration_us ||
      (extractor->config.max_duration_us > 0 &&
       extractor->consumed_duration_us + duration_us > extractor->config.max_duration_us)) {
    return fingerprint_reject(extractor, TURBO_RECOGNITION_ERR_LIMIT);
  }
  result = extractor->provider.write_audio(extractor->provider.context, frame);
  if (atomic_load(&extractor->state) == TURBO_RECOGNITION_STATE_ERROR) {
    return atomic_load(&extractor->last_result);
  }
  if (result != TURBO_RECOGNITION_OK) {
    return fingerprint_reject(extractor, result);
  }
  extractor->consumed_duration_us += duration_us;
  atomic_store(&extractor->last_result, result);
  return result;
}

int turbo_fingerprint_extractor_write_video(turbo_fingerprint_extractor_t *extractor,
                                            const turbo_recognition_video_frame_t *frame) {
  int result;
  int set_first_timestamp = 0;
  if (!extractor) return TURBO_RECOGNITION_ERR_INVALID;
  if (!video_frame_valid(frame)) {
    return fingerprint_reject(extractor, TURBO_RECOGNITION_ERR_INVALID);
  }
  if (atomic_load(&extractor->state) != TURBO_RECOGNITION_STATE_RUNNING) {
    return fingerprint_reject(extractor, TURBO_RECOGNITION_ERR_STATE);
  }
  if (extractor->config.domain != TURBO_FINGERPRINT_VIDEO_CONTENT ||
      extractor->config.video_format != frame->pixel_format) {
    return fingerprint_reject(extractor, TURBO_RECOGNITION_ERR_FORMAT);
  }
  if (!extractor->has_video_timestamp) {
    set_first_timestamp = 1;
  } else if (frame->timestamp_us < extractor->first_video_timestamp_us) {
    return fingerprint_reject(extractor, TURBO_RECOGNITION_ERR_INVALID);
  } else if (extractor->config.max_duration_us > 0 &&
             frame->timestamp_us - extractor->first_video_timestamp_us >
                 extractor->config.max_duration_us) {
    return fingerprint_reject(extractor, TURBO_RECOGNITION_ERR_LIMIT);
  }
  result = extractor->provider.write_video(extractor->provider.context, frame);
  if (atomic_load(&extractor->state) == TURBO_RECOGNITION_STATE_ERROR) {
    return atomic_load(&extractor->last_result);
  }
  if (result != TURBO_RECOGNITION_OK) {
    return fingerprint_reject(extractor, result);
  }
  if (set_first_timestamp) {
    extractor->first_video_timestamp_us = frame->timestamp_us;
    extractor->has_video_timestamp = 1;
  }
  atomic_store(&extractor->last_result, result);
  return result;
}

int turbo_fingerprint_extractor_finish(turbo_fingerprint_extractor_t *extractor) {
  int result;
  if (!extractor) return TURBO_RECOGNITION_ERR_INVALID;
  if (atomic_load(&extractor->state) != TURBO_RECOGNITION_STATE_RUNNING) {
    return TURBO_RECOGNITION_ERR_STATE;
  }
  atomic_store(&extractor->state, TURBO_RECOGNITION_STATE_FINISHING);
  result = extractor->provider.finish(extractor->provider.context);
  if (result != TURBO_RECOGNITION_OK) {
    atomic_store(&extractor->last_result, result);
    if (atomic_load(&extractor->state) == TURBO_RECOGNITION_STATE_FINISHING) {
      atomic_store(&extractor->state, TURBO_RECOGNITION_STATE_RUNNING);
    }
  } else if (atomic_load(&extractor->state) == TURBO_RECOGNITION_STATE_ERROR) {
    result = atomic_load(&extractor->last_result);
  }
  return result;
}

int turbo_fingerprint_extractor_cancel(turbo_fingerprint_extractor_t *extractor) {
  int result;
  int state;
  if (!extractor) return TURBO_RECOGNITION_ERR_INVALID;
  state = atomic_load(&extractor->state);
  if (!session_active(state) && state != TURBO_RECOGNITION_STATE_ERROR) {
    return TURBO_RECOGNITION_ERR_STATE;
  }
  result = extractor->provider.cancel(extractor->provider.context);
  atomic_store(&extractor->last_result, result);
  if (result == TURBO_RECOGNITION_OK) {
    atomic_store(&extractor->state, TURBO_RECOGNITION_STATE_STOPPED);
  }
  return result;
}

turbo_recognition_state_t
turbo_fingerprint_extractor_get_state(const turbo_fingerprint_extractor_t *extractor) {
  return extractor ? (turbo_recognition_state_t)atomic_load(&extractor->state)
                   : TURBO_RECOGNITION_STATE_ERROR;
}

int turbo_fingerprint_extractor_get_last_result(const turbo_fingerprint_extractor_t *extractor) {
  return extractor ? atomic_load(&extractor->last_result) : TURBO_RECOGNITION_ERR_INVALID;
}

uint64_t turbo_fingerprint_extractor_get_rejected_frame_count(
    const turbo_fingerprint_extractor_t *extractor) {
  return extractor ? atomic_load(&extractor->rejected_frame_count) : 0U;
}

turbo_fingerprint_domain_t
turbo_fingerprint_extractor_get_domain(const turbo_fingerprint_extractor_t *extractor) {
  return extractor ? extractor->config.domain : (turbo_fingerprint_domain_t)0;
}

int turbo_fingerprint_extractor_get_audio_format(const turbo_fingerprint_extractor_t *extractor,
                                                 turbo_recognition_audio_format_t *format) {
  if (!extractor || !format) return TURBO_RECOGNITION_ERR_INVALID;
  if (!session_active(atomic_load(&extractor->state))) return TURBO_RECOGNITION_ERR_STATE;
  if (extractor->config.domain != TURBO_FINGERPRINT_VOICE &&
      extractor->config.domain != TURBO_FINGERPRINT_AUDIO_CONTENT) {
    return TURBO_RECOGNITION_ERR_FORMAT;
  }
  *format = extractor->config.audio_format;
  return TURBO_RECOGNITION_OK;
}

void turbo_fingerprint_capture_callback(salts_capture_t *capture, const uint8_t *samples,
                                        size_t len, uint64_t timestamp_us, void *user_data) {
  turbo_fingerprint_extractor_t *extractor = (turbo_fingerprint_extractor_t *)user_data;
  turbo_recognition_audio_frame_t frame;
  (void)capture;
  if (!extractor || turbo_fingerprint_extractor_get_audio_format(extractor, &frame.format) !=
                        TURBO_RECOGNITION_OK) {
    return;
  }
  frame.data = samples;
  frame.len = len;
  frame.timestamp_us = timestamp_us;
  (void)turbo_fingerprint_extractor_write_audio(extractor, &frame);
}

void turbo_fingerprint_player_audio_callback(turbo_player_t *player, const float *samples,
                                             size_t frame_count, int sample_rate, int channels,
                                             int64_t pts_ms, void *user_data) {
  turbo_recognition_audio_frame_t frame;
  (void)player;
  if (!samples || frame_count == 0 || channels <= 0 ||
      frame_count > SIZE_MAX / (size_t)channels / sizeof(float)) {
    return;
  }
  frame.data = (const uint8_t *)samples;
  frame.len = frame_count * (size_t)channels * sizeof(float);
  frame.format.sample_rate = sample_rate;
  frame.format.channels = channels;
  frame.format.sample_format = TURBO_RECOGNITION_AUDIO_F32;
  frame.timestamp_us = timestamp_ms_to_us(pts_ms);
  (void)turbo_fingerprint_extractor_write_audio((turbo_fingerprint_extractor_t *)user_data, &frame);
}

void turbo_fingerprint_player_video_callback(turbo_player_t *player,
                                             const turbo_player_video_frame_t *player_frame,
                                             void *user_data) {
  turbo_recognition_video_frame_t frame;
  (void)player;
  if (!player_frame) return;
  frame.data = player_frame->data;
  frame.len = player_frame->len;
  frame.width = player_frame->width;
  frame.height = player_frame->height;
  memcpy(frame.stride, player_frame->stride, sizeof(frame.stride));
  frame.pixel_format = player_frame->format == TURBO_PLAYER_VIDEO_I420
                           ? TURBO_RECOGNITION_VIDEO_I420
                           : TURBO_RECOGNITION_VIDEO_RGBA;
  frame.timestamp_us = timestamp_ms_to_us(player_frame->pts_ms);
  (void)turbo_fingerprint_extractor_write_video((turbo_fingerprint_extractor_t *)user_data, &frame);
}

turbo_fingerprint_matcher_t *
turbo_fingerprint_matcher_create(const turbo_fingerprint_matcher_provider_t *provider) {
  turbo_fingerprint_matcher_t *matcher;
  if (!provider || provider->abi_version != TURBO_RECOGNITION_PROVIDER_ABI_VERSION ||
      !provider->compare || !provider->destroy) {
    return NULL;
  }
  matcher = (turbo_fingerprint_matcher_t *)calloc(1, sizeof(*matcher));
  if (!matcher) return NULL;
  matcher->provider = *provider;
  return matcher;
}

void turbo_fingerprint_matcher_destroy(turbo_fingerprint_matcher_t *matcher) {
  if (!matcher) return;
  matcher->provider.destroy(matcher->provider.context);
  free(matcher);
}

static int optional_string_equal(const char *left, const char *right) {
  if (!left || !left[0]) return !right || !right[0];
  return right && strcmp(left, right) == 0;
}

int turbo_fingerprint_match(turbo_fingerprint_matcher_t *matcher, const turbo_fingerprint_t *left,
                            const turbo_fingerprint_t *right, float threshold,
                            turbo_fingerprint_match_result_t *result) {
  float similarity;
  int provider_result;
  if (!matcher || !fingerprint_value_valid(left) || !fingerprint_value_valid(right) || !result ||
      threshold < 0.0f || threshold > 1.0f || !isfinite(threshold)) {
    return TURBO_RECOGNITION_ERR_INVALID;
  }
  if (left->domain != right->domain || strcmp(left->algorithm, right->algorithm) != 0 ||
      !optional_string_equal(left->model_version, right->model_version)) {
    return TURBO_RECOGNITION_ERR_INCOMPATIBLE;
  }
  provider_result = matcher->provider.compare(matcher->provider.context, left, right, &similarity);
  if (provider_result != TURBO_RECOGNITION_OK) return provider_result;
  if (similarity < 0.0f || similarity > 1.0f || !isfinite(similarity)) {
    return TURBO_RECOGNITION_ERR_PROVIDER;
  }
  result->similarity = similarity;
  result->threshold = threshold;
  result->is_match = similarity >= threshold;
  return TURBO_RECOGNITION_OK;
}
