#include "turbo_speech.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct turbo_asr_s {
  turbo_asr_provider_t provider;
  turbo_asr_callbacks_t callbacks;
  void *user_data;
  _Atomic int state;
  turbo_speech_audio_format_t format;
  _Atomic int last_result;
  _Atomic uint64_t rejected_frame_count;
};

struct turbo_tts_s {
  turbo_tts_provider_t provider;
  turbo_tts_callbacks_t callbacks;
  void *user_data;
  _Atomic int state;
  _Atomic int last_result;
};

static int speech_format_valid(const turbo_speech_audio_format_t *format) {
  if (!format) return 0;
  if (format->sample_rate != 8000 && format->sample_rate != 16000 && format->sample_rate != 24000 &&
      format->sample_rate != 48000) {
    return 0;
  }
  return (format->channels == 1 || format->channels == 2) &&
         (format->bits_per_sample == 16 || format->bits_per_sample == 32);
}

static int speech_frame_valid(const turbo_speech_audio_frame_t *frame) {
  size_t bytes_per_frame;
  if (!frame || !frame->data || frame->len == 0 || !speech_format_valid(&frame->format)) {
    return 0;
  }
  bytes_per_frame = (size_t)frame->format.channels * (size_t)(frame->format.bits_per_sample / 8);
  return bytes_per_frame != 0 && frame->len % bytes_per_frame == 0;
}

static int speech_format_equal(const turbo_speech_audio_format_t *left,
                               const turbo_speech_audio_format_t *right) {
  return left->sample_rate == right->sample_rate && left->channels == right->channels &&
         left->bits_per_sample == right->bits_per_sample;
}

static void asr_provider_result(const turbo_asr_result_t *result, void *user_data) {
  turbo_asr_t *asr = (turbo_asr_t *)user_data;
  int state;
  if (!asr || !result || !result->text || result->text_len == 0) return;
  state = atomic_load(&asr->state);
  if (state != TURBO_SPEECH_STATE_RUNNING && state != TURBO_SPEECH_STATE_FINISHING) {
    return;
  }
  if (asr->callbacks.on_result) {
    asr->callbacks.on_result(asr, result, asr->user_data);
  }
}

static void asr_provider_complete(void *user_data) {
  turbo_asr_t *asr = (turbo_asr_t *)user_data;
  if (!asr) return;
  if (atomic_load(&asr->state) != TURBO_SPEECH_STATE_RUNNING &&
      atomic_load(&asr->state) != TURBO_SPEECH_STATE_FINISHING) {
    return;
  }
  atomic_store(&asr->state, TURBO_SPEECH_STATE_STOPPED);
  atomic_store(&asr->last_result, TURBO_SPEECH_OK);
  if (asr->callbacks.on_complete) {
    asr->callbacks.on_complete(asr, asr->user_data);
  }
}

static void asr_provider_error(int error_code, const char *message, void *user_data) {
  turbo_asr_t *asr = (turbo_asr_t *)user_data;
  int state;
  if (!asr) return;
  state = atomic_load(&asr->state);
  if (state != TURBO_SPEECH_STATE_RUNNING && state != TURBO_SPEECH_STATE_FINISHING) {
    return;
  }
  atomic_store(&asr->state, TURBO_SPEECH_STATE_ERROR);
  atomic_store(&asr->last_result,
               error_code == TURBO_SPEECH_OK ? TURBO_SPEECH_ERR_PROVIDER : error_code);
  if (asr->callbacks.on_error) {
    asr->callbacks.on_error(asr, atomic_load(&asr->last_result), message, asr->user_data);
  }
}

static const turbo_asr_provider_callbacks_t asr_provider_callbacks = {
    asr_provider_result, asr_provider_complete, asr_provider_error};

turbo_asr_t *turbo_asr_create(const turbo_asr_provider_t *provider,
                              const turbo_asr_callbacks_t *callbacks, void *user_data) {
  turbo_asr_t *asr;
  if (!provider || provider->abi_version != TURBO_SPEECH_PROVIDER_ABI_VERSION || !provider->start ||
      !provider->write || !provider->finish || !provider->cancel || !provider->destroy) {
    return NULL;
  }
  asr = (turbo_asr_t *)calloc(1, sizeof(*asr));
  if (!asr) return NULL;
  asr->provider = *provider;
  if (callbacks) asr->callbacks = *callbacks;
  asr->user_data = user_data;
  atomic_init(&asr->state, TURBO_SPEECH_STATE_IDLE);
  atomic_init(&asr->last_result, TURBO_SPEECH_OK);
  atomic_init(&asr->rejected_frame_count, 0U);
  return asr;
}

void turbo_asr_destroy(turbo_asr_t *asr) {
  if (!asr) return;
  if (atomic_load(&asr->state) == TURBO_SPEECH_STATE_RUNNING ||
      atomic_load(&asr->state) == TURBO_SPEECH_STATE_FINISHING ||
      atomic_load(&asr->state) == TURBO_SPEECH_STATE_ERROR) {
    (void)asr->provider.cancel(asr->provider.context);
  }
  asr->provider.destroy(asr->provider.context);
  free(asr);
}

int turbo_asr_start(turbo_asr_t *asr, const turbo_asr_config_t *config) {
  int result;
  if (!asr || !config || !speech_format_valid(&config->format)) {
    return TURBO_SPEECH_ERR_INVALID;
  }
  if (atomic_load(&asr->state) != TURBO_SPEECH_STATE_IDLE &&
      atomic_load(&asr->state) != TURBO_SPEECH_STATE_STOPPED) {
    return TURBO_SPEECH_ERR_STATE;
  }
  asr->format = config->format;
  atomic_store(&asr->rejected_frame_count, 0U);
  atomic_store(&asr->state, TURBO_SPEECH_STATE_RUNNING);
  result = asr->provider.start(asr->provider.context, config, &asr_provider_callbacks, asr);
  if (result != TURBO_SPEECH_OK) {
    atomic_store(&asr->last_result, result);
    if (atomic_load(&asr->state) == TURBO_SPEECH_STATE_RUNNING) {
      atomic_store(&asr->state, TURBO_SPEECH_STATE_IDLE);
    }
  } else {
    int state = atomic_load(&asr->state);
    if (state == TURBO_SPEECH_STATE_ERROR) {
      result = atomic_load(&asr->last_result);
    } else if (state == TURBO_SPEECH_STATE_RUNNING) {
      atomic_store(&asr->last_result, TURBO_SPEECH_OK);
    }
  }
  return result;
}

int turbo_asr_write_frame(turbo_asr_t *asr, const turbo_speech_audio_frame_t *frame) {
  int result;
  if (!asr) return TURBO_SPEECH_ERR_INVALID;
  if (!speech_frame_valid(frame)) {
    atomic_fetch_add(&asr->rejected_frame_count, 1U);
    return TURBO_SPEECH_ERR_INVALID;
  }
  if (atomic_load(&asr->state) != TURBO_SPEECH_STATE_RUNNING) {
    atomic_fetch_add(&asr->rejected_frame_count, 1U);
    return TURBO_SPEECH_ERR_STATE;
  }
  if (!speech_format_equal(&asr->format, &frame->format)) {
    atomic_fetch_add(&asr->rejected_frame_count, 1U);
    return TURBO_SPEECH_ERR_FORMAT;
  }
  result = asr->provider.write(asr->provider.context, frame);
  if (result != TURBO_SPEECH_OK) {
    atomic_fetch_add(&asr->rejected_frame_count, 1U);
  }
  atomic_store(&asr->last_result, result);
  return result;
}

int turbo_asr_write_pcm(turbo_asr_t *asr, const uint8_t *samples, size_t len,
                        uint64_t timestamp_us) {
  turbo_speech_audio_frame_t frame;
  if (!asr) return TURBO_SPEECH_ERR_INVALID;
  frame.data = samples;
  frame.len = len;
  frame.format = asr->format;
  frame.timestamp_us = timestamp_us;
  return turbo_asr_write_frame(asr, &frame);
}

int turbo_asr_finish(turbo_asr_t *asr) {
  int result;
  if (!asr) return TURBO_SPEECH_ERR_INVALID;
  if (atomic_load(&asr->state) != TURBO_SPEECH_STATE_RUNNING) {
    return TURBO_SPEECH_ERR_STATE;
  }
  atomic_store(&asr->state, TURBO_SPEECH_STATE_FINISHING);
  result = asr->provider.finish(asr->provider.context);
  if (result != TURBO_SPEECH_OK) {
    atomic_store(&asr->last_result, result);
    if (atomic_load(&asr->state) == TURBO_SPEECH_STATE_FINISHING) {
      atomic_store(&asr->state, TURBO_SPEECH_STATE_RUNNING);
    }
  } else if (atomic_load(&asr->state) == TURBO_SPEECH_STATE_ERROR) {
    result = atomic_load(&asr->last_result);
  }
  return result;
}

int turbo_asr_cancel(turbo_asr_t *asr) {
  int result;
  if (!asr) return TURBO_SPEECH_ERR_INVALID;
  if (atomic_load(&asr->state) != TURBO_SPEECH_STATE_RUNNING &&
      atomic_load(&asr->state) != TURBO_SPEECH_STATE_FINISHING &&
      atomic_load(&asr->state) != TURBO_SPEECH_STATE_ERROR) {
    return TURBO_SPEECH_ERR_STATE;
  }
  result = asr->provider.cancel(asr->provider.context);
  atomic_store(&asr->last_result, result);
  if (result == TURBO_SPEECH_OK) {
    atomic_store(&asr->state, TURBO_SPEECH_STATE_STOPPED);
  }
  return result;
}

turbo_speech_state_t turbo_asr_get_state(const turbo_asr_t *asr) {
  return asr ? (turbo_speech_state_t)atomic_load(&asr->state) : TURBO_SPEECH_STATE_ERROR;
}

int turbo_asr_get_last_result(const turbo_asr_t *asr) {
  return asr ? atomic_load(&asr->last_result) : TURBO_SPEECH_ERR_INVALID;
}

uint64_t turbo_asr_get_rejected_frame_count(const turbo_asr_t *asr) {
  return asr ? atomic_load(&asr->rejected_frame_count) : 0U;
}

int turbo_asr_get_audio_format(const turbo_asr_t *asr, turbo_speech_audio_format_t *format) {
  if (!asr || !format) return TURBO_SPEECH_ERR_INVALID;
  if (atomic_load(&asr->state) != TURBO_SPEECH_STATE_RUNNING &&
      atomic_load(&asr->state) != TURBO_SPEECH_STATE_FINISHING) {
    return TURBO_SPEECH_ERR_STATE;
  }
  *format = asr->format;
  return TURBO_SPEECH_OK;
}

void turbo_asr_capture_callback(salts_capture_t *capture, const uint8_t *samples, size_t len,
                                uint64_t timestamp_us, void *user_data) {
  (void)capture;
  (void)turbo_asr_write_pcm((turbo_asr_t *)user_data, samples, len, timestamp_us);
}

static int tts_provider_audio(const turbo_speech_audio_frame_t *frame, void *user_data) {
  turbo_tts_t *tts = (turbo_tts_t *)user_data;
  int result;
  if (!tts || atomic_load(&tts->state) != TURBO_SPEECH_STATE_RUNNING) {
    return TURBO_SPEECH_ERR_STATE;
  }
  if (!speech_frame_valid(frame)) {
    atomic_store(&tts->last_result, TURBO_SPEECH_ERR_FORMAT);
    return TURBO_SPEECH_ERR_FORMAT;
  }
  result = tts->callbacks.on_audio(tts, frame, tts->user_data);
  atomic_store(&tts->last_result, result);
  return result;
}

static void tts_provider_complete(void *user_data) {
  turbo_tts_t *tts = (turbo_tts_t *)user_data;
  if (!tts) return;
  if (atomic_load(&tts->state) != TURBO_SPEECH_STATE_RUNNING) return;
  atomic_store(&tts->state, TURBO_SPEECH_STATE_STOPPED);
  atomic_store(&tts->last_result, TURBO_SPEECH_OK);
  if (tts->callbacks.on_complete) {
    tts->callbacks.on_complete(tts, tts->user_data);
  }
}

static void tts_provider_error(int error_code, const char *message, void *user_data) {
  turbo_tts_t *tts = (turbo_tts_t *)user_data;
  if (!tts) return;
  if (atomic_load(&tts->state) != TURBO_SPEECH_STATE_RUNNING) return;
  atomic_store(&tts->state, TURBO_SPEECH_STATE_ERROR);
  atomic_store(&tts->last_result,
               error_code == TURBO_SPEECH_OK ? TURBO_SPEECH_ERR_PROVIDER : error_code);
  if (tts->callbacks.on_error) {
    tts->callbacks.on_error(tts, atomic_load(&tts->last_result), message, tts->user_data);
  }
}

static const turbo_tts_provider_callbacks_t tts_provider_callbacks = {
    tts_provider_audio, tts_provider_complete, tts_provider_error};

turbo_tts_t *turbo_tts_create(const turbo_tts_provider_t *provider,
                              const turbo_tts_callbacks_t *callbacks, void *user_data) {
  turbo_tts_t *tts;
  if (!provider || provider->abi_version != TURBO_SPEECH_PROVIDER_ABI_VERSION ||
      !provider->synthesize || !provider->cancel || !provider->destroy || !callbacks ||
      !callbacks->on_audio) {
    return NULL;
  }
  tts = (turbo_tts_t *)calloc(1, sizeof(*tts));
  if (!tts) return NULL;
  tts->provider = *provider;
  tts->callbacks = *callbacks;
  tts->user_data = user_data;
  atomic_init(&tts->state, TURBO_SPEECH_STATE_IDLE);
  atomic_init(&tts->last_result, TURBO_SPEECH_OK);
  return tts;
}

void turbo_tts_destroy(turbo_tts_t *tts) {
  if (!tts) return;
  if (atomic_load(&tts->state) == TURBO_SPEECH_STATE_RUNNING ||
      atomic_load(&tts->state) == TURBO_SPEECH_STATE_FINISHING ||
      atomic_load(&tts->state) == TURBO_SPEECH_STATE_ERROR) {
    (void)tts->provider.cancel(tts->provider.context);
  }
  tts->provider.destroy(tts->provider.context);
  free(tts);
}

int turbo_tts_synthesize(turbo_tts_t *tts, const turbo_tts_request_t *request) {
  int result;
  if (!tts || !request || !request->text || request->text_len == 0 || request->rate <= 0.0f ||
      request->pitch <= 0.0f) {
    return TURBO_SPEECH_ERR_INVALID;
  }
  if (atomic_load(&tts->state) != TURBO_SPEECH_STATE_IDLE &&
      atomic_load(&tts->state) != TURBO_SPEECH_STATE_STOPPED) {
    return TURBO_SPEECH_ERR_STATE;
  }
  atomic_store(&tts->state, TURBO_SPEECH_STATE_RUNNING);
  result = tts->provider.synthesize(tts->provider.context, request, &tts_provider_callbacks, tts);
  if (result != TURBO_SPEECH_OK) {
    atomic_store(&tts->last_result, result);
    if (atomic_load(&tts->state) == TURBO_SPEECH_STATE_RUNNING) {
      atomic_store(&tts->state, TURBO_SPEECH_STATE_IDLE);
    }
  } else {
    int state = atomic_load(&tts->state);
    if (state == TURBO_SPEECH_STATE_ERROR) {
      result = atomic_load(&tts->last_result);
    } else if (state == TURBO_SPEECH_STATE_RUNNING) {
      atomic_store(&tts->last_result, TURBO_SPEECH_OK);
    }
  }
  return result;
}

int turbo_tts_cancel(turbo_tts_t *tts) {
  int result;
  if (!tts) return TURBO_SPEECH_ERR_INVALID;
  if (atomic_load(&tts->state) != TURBO_SPEECH_STATE_RUNNING &&
      atomic_load(&tts->state) != TURBO_SPEECH_STATE_ERROR) {
    return TURBO_SPEECH_ERR_STATE;
  }
  result = tts->provider.cancel(tts->provider.context);
  atomic_store(&tts->last_result, result);
  if (result == TURBO_SPEECH_OK) {
    atomic_store(&tts->state, TURBO_SPEECH_STATE_STOPPED);
  }
  return result;
}

turbo_speech_state_t turbo_tts_get_state(const turbo_tts_t *tts) {
  return tts ? (turbo_speech_state_t)atomic_load(&tts->state) : TURBO_SPEECH_STATE_ERROR;
}

int turbo_tts_get_last_result(const turbo_tts_t *tts) {
  return tts ? atomic_load(&tts->last_result) : TURBO_SPEECH_ERR_INVALID;
}
