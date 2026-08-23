#include "tinytest.h"
#include "turbo_speech.h"

#include <string.h>

typedef struct {
  turbo_asr_provider_callbacks_t callbacks;
  void *callback_user_data;
  int start_count;
  int write_count;
  int finish_count;
  int cancel_count;
  int destroy_count;
  int start_error;
  int write_result;
  size_t last_len;
  uint64_t last_timestamp_us;
  uint8_t first_byte;
} mock_asr_provider_t;

typedef struct {
  int result_count;
  int complete_count;
  int error_count;
  char text[32];
} asr_observer_t;

typedef struct {
  int synthesize_count;
  int cancel_count;
  int destroy_count;
  int audio_result;
  int synthesize_error;
} mock_tts_provider_t;

typedef struct {
  int audio_count;
  int complete_count;
  int error_count;
  size_t last_len;
  uint8_t first_byte;
  int sink_result;
} tts_observer_t;

static int mock_asr_start(void *context, const turbo_asr_config_t *config,
                          const turbo_asr_provider_callbacks_t *callbacks,
                          void *callback_user_data) {
  mock_asr_provider_t *mock = (mock_asr_provider_t *)context;
  (void)config;
  mock->callbacks = *callbacks;
  mock->callback_user_data = callback_user_data;
  mock->start_count++;
  if (mock->start_error != TURBO_SPEECH_OK) {
    callbacks->on_error(mock->start_error, "ASR start failed", callback_user_data);
  }
  return TURBO_SPEECH_OK;
}

static int mock_asr_write(void *context, const turbo_speech_audio_frame_t *frame) {
  mock_asr_provider_t *mock = (mock_asr_provider_t *)context;
  mock->write_count++;
  mock->last_len = frame->len;
  mock->last_timestamp_us = frame->timestamp_us;
  mock->first_byte = frame->data[0];
  return mock->write_result;
}

static int mock_asr_finish(void *context) {
  static const char transcript[] = "hello";
  mock_asr_provider_t *mock = (mock_asr_provider_t *)context;
  turbo_asr_result_t result = {.text = transcript,
                               .text_len = sizeof(transcript) - 1U,
                               .start_time_us = 1000U,
                               .end_time_us = 2000U,
                               .confidence = 0.9f,
                               .is_final = 1};
  mock->finish_count++;
  mock->callbacks.on_result(&result, mock->callback_user_data);
  mock->callbacks.on_complete(mock->callback_user_data);
  return TURBO_SPEECH_OK;
}

static int mock_asr_cancel(void *context) {
  mock_asr_provider_t *mock = (mock_asr_provider_t *)context;
  mock->cancel_count++;
  return TURBO_SPEECH_OK;
}

static void mock_asr_destroy(void *context) {
  mock_asr_provider_t *mock = (mock_asr_provider_t *)context;
  mock->destroy_count++;
}

static void observe_asr_result(turbo_asr_t *asr, const turbo_asr_result_t *result,
                               void *user_data) {
  asr_observer_t *observer = (asr_observer_t *)user_data;
  size_t copy_len = result->text_len < sizeof(observer->text) - 1U ? result->text_len
                                                                   : sizeof(observer->text) - 1U;
  (void)asr;
  memcpy(observer->text, result->text, copy_len);
  observer->text[copy_len] = '\0';
  observer->result_count++;
}

static void observe_complete(turbo_asr_t *asr, void *user_data) {
  asr_observer_t *observer = (asr_observer_t *)user_data;
  (void)asr;
  observer->complete_count++;
}

static void observe_asr_error(turbo_asr_t *asr, int error_code, const char *message,
                              void *user_data) {
  asr_observer_t *observer = (asr_observer_t *)user_data;
  (void)asr;
  (void)error_code;
  (void)message;
  observer->error_count++;
}

static turbo_asr_provider_t make_asr_provider(mock_asr_provider_t *mock) {
  turbo_asr_provider_t provider = {.abi_version = TURBO_SPEECH_PROVIDER_ABI_VERSION,
                                   .context = mock,
                                   .start = mock_asr_start,
                                   .write = mock_asr_write,
                                   .finish = mock_asr_finish,
                                   .cancel = mock_asr_cancel,
                                   .destroy = mock_asr_destroy};
  return provider;
}

static turbo_asr_config_t make_asr_config(void) {
  turbo_asr_config_t config = {
      .format = {.sample_rate = 16000, .channels = 1, .bits_per_sample = 16},
      .language = "en-US",
      .enable_interim_results = 1};
  return config;
}

static int mock_tts_synthesize(void *context, const turbo_tts_request_t *request,
                               const turbo_tts_provider_callbacks_t *callbacks,
                               void *callback_user_data) {
  static const uint8_t pcm[] = {1U, 2U, 3U, 4U};
  mock_tts_provider_t *mock = (mock_tts_provider_t *)context;
  turbo_speech_audio_frame_t frame = {
      .data = pcm,
      .len = sizeof(pcm),
      .format = {.sample_rate = 16000, .channels = 1, .bits_per_sample = 16},
      .timestamp_us = 3000U};
  int result;
  (void)request;
  mock->synthesize_count++;
  if (mock->synthesize_error != TURBO_SPEECH_OK) {
    callbacks->on_error(mock->synthesize_error, "TTS synthesis failed", callback_user_data);
    return TURBO_SPEECH_OK;
  }
  result = callbacks->on_audio(&frame, callback_user_data);
  mock->audio_result = result;
  if (result != TURBO_SPEECH_OK) return result;
  callbacks->on_complete(callback_user_data);
  return TURBO_SPEECH_OK;
}

static int mock_tts_cancel(void *context) {
  mock_tts_provider_t *mock = (mock_tts_provider_t *)context;
  mock->cancel_count++;
  return TURBO_SPEECH_OK;
}

static void mock_tts_destroy(void *context) {
  mock_tts_provider_t *mock = (mock_tts_provider_t *)context;
  mock->destroy_count++;
}

static int observe_tts_audio(turbo_tts_t *tts, const turbo_speech_audio_frame_t *frame,
                             void *user_data) {
  tts_observer_t *observer = (tts_observer_t *)user_data;
  (void)tts;
  observer->audio_count++;
  observer->last_len = frame->len;
  observer->first_byte = frame->data[0];
  return observer->sink_result;
}

static void observe_tts_complete(turbo_tts_t *tts, void *user_data) {
  tts_observer_t *observer = (tts_observer_t *)user_data;
  (void)tts;
  observer->complete_count++;
}

static void observe_tts_error(turbo_tts_t *tts, int error_code, const char *message,
                              void *user_data) {
  tts_observer_t *observer = (tts_observer_t *)user_data;
  (void)tts;
  (void)error_code;
  (void)message;
  observer->error_count++;
}

static turbo_tts_provider_t make_tts_provider(mock_tts_provider_t *mock) {
  turbo_tts_provider_t provider = {.abi_version = TURBO_SPEECH_PROVIDER_ABI_VERSION,
                                   .context = mock,
                                   .synthesize = mock_tts_synthesize,
                                   .cancel = mock_tts_cancel,
                                   .destroy = mock_tts_destroy};
  return provider;
}

suite("TurboMedia speech") {
  group("ASR") {
    it("streams microphone PCM and completes with a final transcript") {
      static const uint8_t pcm[] = {0x34U, 0x12U, 0x78U, 0x56U};
      mock_asr_provider_t mock = {0};
      asr_observer_t observer = {0};
      turbo_asr_provider_t provider = make_asr_provider(&mock);
      turbo_asr_callbacks_t callbacks = {.on_result = observe_asr_result,
                                         .on_complete = observe_complete};
      turbo_asr_config_t config = make_asr_config();
      turbo_asr_t *asr = turbo_asr_create(&provider, &callbacks, &observer);

      check_not_null(asr);
      check_equal(turbo_asr_start(asr, &config), TURBO_SPEECH_OK);
      turbo_asr_capture_callback(NULL, pcm, sizeof(pcm), 1234U, asr);
      check_equal(mock.write_count, 1);
      check_equal(mock.last_len, sizeof(pcm));
      check_equal((long)mock.last_timestamp_us, 1234L);
      check_equal(mock.first_byte, pcm[0]);
      check_equal(turbo_asr_finish(asr), TURBO_SPEECH_OK);
      check_equal(observer.result_count, 1);
      check_equal(observer.text, "hello");
      check_equal(observer.complete_count, 1);
      check_equal(turbo_asr_get_state(asr), TURBO_SPEECH_STATE_STOPPED);

      turbo_asr_destroy(asr);
      check_equal(mock.destroy_count, 1);
    }

    it("rejects format mismatches without changing the running session") {
      static const uint8_t pcm[] = {0U, 0U, 0U, 0U};
      mock_asr_provider_t mock = {0};
      turbo_asr_provider_t provider = make_asr_provider(&mock);
      turbo_asr_config_t config = make_asr_config();
      turbo_speech_audio_frame_t frame = {
          .data = pcm,
          .len = sizeof(pcm),
          .format = {.sample_rate = 48000, .channels = 1, .bits_per_sample = 16}};
      turbo_asr_t *asr = turbo_asr_create(&provider, NULL, NULL);

      check_not_null(asr);
      check_equal(turbo_asr_start(asr, &config), TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_frame(asr, &frame), TURBO_SPEECH_ERR_FORMAT);
      check_equal(mock.write_count, 0);
      check_equal(turbo_asr_get_state(asr), TURBO_SPEECH_STATE_RUNNING);
      check_equal(turbo_asr_cancel(asr), TURBO_SPEECH_OK);
      turbo_asr_destroy(asr);
    }

    it("surfaces provider backpressure without dropping session state") {
      static const uint8_t pcm[] = {0U, 0U};
      mock_asr_provider_t mock = {.write_result = TURBO_SPEECH_ERR_BUSY};
      turbo_asr_provider_t provider = make_asr_provider(&mock);
      turbo_asr_config_t config = make_asr_config();
      turbo_asr_t *asr = turbo_asr_create(&provider, NULL, NULL);

      check_not_null(asr);
      check_equal(turbo_asr_start(asr, &config), TURBO_SPEECH_OK);
      check_equal(turbo_asr_write_pcm(asr, pcm, sizeof(pcm), 0U), TURBO_SPEECH_ERR_BUSY);
      check_equal(turbo_asr_get_last_result(asr), TURBO_SPEECH_ERR_BUSY);
      check_equal((long)turbo_asr_get_rejected_frame_count(asr), 1L);
      check_equal(turbo_asr_get_state(asr), TURBO_SPEECH_STATE_RUNNING);
      check_equal(turbo_asr_cancel(asr), TURBO_SPEECH_OK);
      turbo_asr_destroy(asr);
    }

    it("returns a provider error reported synchronously by start") {
      mock_asr_provider_t mock = {.start_error = TURBO_SPEECH_ERR_PROVIDER};
      asr_observer_t observer = {0};
      turbo_asr_provider_t provider = make_asr_provider(&mock);
      turbo_asr_callbacks_t callbacks = {.on_error = observe_asr_error};
      turbo_asr_config_t config = make_asr_config();
      turbo_asr_t *asr = turbo_asr_create(&provider, &callbacks, &observer);

      check_not_null(asr);
      check_equal(turbo_asr_start(asr, &config), TURBO_SPEECH_ERR_PROVIDER);
      check_equal(turbo_asr_get_state(asr), TURBO_SPEECH_STATE_ERROR);
      check_equal(turbo_asr_get_last_result(asr), TURBO_SPEECH_ERR_PROVIDER);
      check_equal(observer.error_count, 1);
      turbo_asr_destroy(asr);
      check_equal(mock.cancel_count, 1);
      check_equal(mock.destroy_count, 1);
    }
  }

  group("TTS") {
    it("delivers synthesized PCM and completion synchronously") {
      mock_tts_provider_t mock = {0};
      tts_observer_t observer = {0};
      turbo_tts_provider_t provider = make_tts_provider(&mock);
      turbo_tts_callbacks_t callbacks = {.on_audio = observe_tts_audio,
                                         .on_complete = observe_tts_complete};
      turbo_tts_request_t request = {.text = "hello", .text_len = 5U, .rate = 1.0f, .pitch = 1.0f};
      turbo_tts_t *tts = turbo_tts_create(&provider, &callbacks, &observer);

      check_not_null(tts);
      check_equal(turbo_tts_synthesize(tts, &request), TURBO_SPEECH_OK);
      check_equal(observer.audio_count, 1);
      check_equal(observer.last_len, 4U);
      check_equal(observer.first_byte, 1);
      check_equal(observer.complete_count, 1);
      check_equal(turbo_tts_get_state(tts), TURBO_SPEECH_STATE_STOPPED);
      turbo_tts_destroy(tts);
      check_equal(mock.destroy_count, 1);
    }

    it("propagates sink backpressure to the provider") {
      mock_tts_provider_t mock = {0};
      tts_observer_t observer = {.sink_result = TURBO_SPEECH_ERR_BUSY};
      turbo_tts_provider_t provider = make_tts_provider(&mock);
      turbo_tts_callbacks_t callbacks = {.on_audio = observe_tts_audio};
      turbo_tts_request_t request = {.text = "busy", .text_len = 4U, .rate = 1.0f, .pitch = 1.0f};
      turbo_tts_t *tts = turbo_tts_create(&provider, &callbacks, &observer);

      check_not_null(tts);
      check_equal(turbo_tts_synthesize(tts, &request), TURBO_SPEECH_ERR_BUSY);
      check_equal(mock.audio_result, TURBO_SPEECH_ERR_BUSY);
      check_equal(turbo_tts_get_last_result(tts), TURBO_SPEECH_ERR_BUSY);
      check_equal(turbo_tts_get_state(tts), TURBO_SPEECH_STATE_IDLE);
      turbo_tts_destroy(tts);
    }

    it("returns a provider error reported synchronously by synthesize") {
      mock_tts_provider_t mock = {.synthesize_error = TURBO_SPEECH_ERR_PROVIDER};
      tts_observer_t observer = {0};
      turbo_tts_provider_t provider = make_tts_provider(&mock);
      turbo_tts_callbacks_t callbacks = {.on_audio = observe_tts_audio,
                                         .on_error = observe_tts_error};
      turbo_tts_request_t request = {.text = "error", .text_len = 5U, .rate = 1.0f, .pitch = 1.0f};
      turbo_tts_t *tts = turbo_tts_create(&provider, &callbacks, &observer);

      check_not_null(tts);
      check_equal(turbo_tts_synthesize(tts, &request), TURBO_SPEECH_ERR_PROVIDER);
      check_equal(turbo_tts_get_state(tts), TURBO_SPEECH_STATE_ERROR);
      check_equal(turbo_tts_get_last_result(tts), TURBO_SPEECH_ERR_PROVIDER);
      check_equal(observer.error_count, 1);
      turbo_tts_destroy(tts);
      check_equal(mock.cancel_count, 1);
      check_equal(mock.destroy_count, 1);
    }
  }
}
