#include "tinytest.h"
#include "turbo_recognition.h"

#include <string.h>

typedef struct {
  turbo_voice_detector_provider_callbacks_t callbacks;
  void *callback_user_data;
  int write_count;
  int cancel_count;
  int destroy_count;
  int write_result;
  int emit_invalid_activity;
} mock_voice_provider_t;

typedef struct {
  int activity_count;
  int complete_count;
  int error_count;
  turbo_voice_activity_t last_activity;
} voice_observer_t;

typedef struct {
  turbo_fingerprint_provider_callbacks_t callbacks;
  void *callback_user_data;
  turbo_fingerprint_domain_t domain;
  int audio_write_count;
  int video_write_count;
  int cancel_count;
  int destroy_count;
  int audio_result;
  int emit_invalid_fingerprint;
  turbo_recognition_audio_sample_format_t last_audio_format;
  turbo_recognition_video_pixel_format_t last_video_format;
} mock_fingerprint_provider_t;

typedef struct {
  int result_count;
  int complete_count;
  int error_count;
  uint8_t fingerprint[16];
  size_t fingerprint_len;
} fingerprint_observer_t;

typedef struct {
  int compare_count;
  int destroy_count;
  float similarity;
} mock_matcher_provider_t;

static int mock_voice_start(void *context, const turbo_voice_detector_config_t *config,
                            const turbo_voice_detector_provider_callbacks_t *callbacks,
                            void *callback_user_data) {
  mock_voice_provider_t *mock = (mock_voice_provider_t *)context;
  (void)config;
  mock->callbacks = *callbacks;
  mock->callback_user_data = callback_user_data;
  return TURBO_RECOGNITION_OK;
}

static int mock_voice_write(void *context, const turbo_recognition_audio_frame_t *frame) {
  mock_voice_provider_t *mock = (mock_voice_provider_t *)context;
  turbo_voice_activity_t activity = {.state = TURBO_VOICE_ACTIVITY_SPEECH,
                                     .start_time_us = frame->timestamp_us,
                                     .end_time_us = frame->timestamp_us + 20000U,
                                     .probability = 0.95f,
                                     .is_final = 0};
  if (mock->emit_invalid_activity) activity.probability = 1.5f;
  mock->write_count++;
  if (mock->write_result == TURBO_RECOGNITION_OK) {
    mock->callbacks.on_activity(&activity, mock->callback_user_data);
  }
  return mock->write_result;
}

static int mock_voice_finish(void *context) {
  mock_voice_provider_t *mock = (mock_voice_provider_t *)context;
  mock->callbacks.on_complete(mock->callback_user_data);
  return TURBO_RECOGNITION_OK;
}

static int mock_voice_cancel(void *context) {
  mock_voice_provider_t *mock = (mock_voice_provider_t *)context;
  mock->cancel_count++;
  return TURBO_RECOGNITION_OK;
}

static void mock_voice_destroy(void *context) {
  ((mock_voice_provider_t *)context)->destroy_count++;
}

static void observe_voice_activity(turbo_voice_detector_t *detector,
                                   const turbo_voice_activity_t *activity, void *user_data) {
  voice_observer_t *observer = (voice_observer_t *)user_data;
  (void)detector;
  observer->last_activity = *activity;
  observer->activity_count++;
}

static void observe_voice_complete(turbo_voice_detector_t *detector, void *user_data) {
  voice_observer_t *observer = (voice_observer_t *)user_data;
  (void)detector;
  observer->complete_count++;
}

static void observe_voice_error(turbo_voice_detector_t *detector, int error_code,
                                const char *message, void *user_data) {
  voice_observer_t *observer = (voice_observer_t *)user_data;
  (void)detector;
  (void)error_code;
  (void)message;
  observer->error_count++;
}

static turbo_voice_detector_provider_t make_voice_provider(mock_voice_provider_t *mock) {
  turbo_voice_detector_provider_t provider = {.abi_version = TURBO_RECOGNITION_PROVIDER_ABI_VERSION,
                                              .context = mock,
                                              .start = mock_voice_start,
                                              .write = mock_voice_write,
                                              .finish = mock_voice_finish,
                                              .cancel = mock_voice_cancel,
                                              .destroy = mock_voice_destroy};
  return provider;
}

static turbo_voice_detector_config_t
make_voice_config(turbo_recognition_audio_sample_format_t sample_format) {
  turbo_voice_detector_config_t config = {
      .format = {.sample_rate = 16000, .channels = 1, .sample_format = sample_format},
      .min_speech_ms = 100U,
      .min_silence_ms = 300U};
  return config;
}

static int mock_fingerprint_start(void *context, const turbo_fingerprint_config_t *config,
                                  const turbo_fingerprint_provider_callbacks_t *callbacks,
                                  void *callback_user_data) {
  mock_fingerprint_provider_t *mock = (mock_fingerprint_provider_t *)context;
  mock->callbacks = *callbacks;
  mock->callback_user_data = callback_user_data;
  mock->domain = config->domain;
  return TURBO_RECOGNITION_OK;
}

static int mock_fingerprint_write_audio(void *context,
                                        const turbo_recognition_audio_frame_t *frame) {
  mock_fingerprint_provider_t *mock = (mock_fingerprint_provider_t *)context;
  mock->audio_write_count++;
  mock->last_audio_format = frame->format.sample_format;
  return mock->audio_result;
}

static int mock_fingerprint_write_video(void *context,
                                        const turbo_recognition_video_frame_t *frame) {
  mock_fingerprint_provider_t *mock = (mock_fingerprint_provider_t *)context;
  mock->video_write_count++;
  mock->last_video_format = frame->pixel_format;
  return TURBO_RECOGNITION_OK;
}

static int mock_fingerprint_finish(void *context) {
  static const uint8_t value[] = {0x10U, 0x20U, 0x30U, 0x40U};
  mock_fingerprint_provider_t *mock = (mock_fingerprint_provider_t *)context;
  turbo_fingerprint_t fingerprint = {.domain = mock->domain,
                                     .algorithm =
                                         mock->emit_invalid_fingerprint ? NULL : "mock-embedding",
                                     .model_version = "v1",
                                     .data = value,
                                     .len = sizeof(value),
                                     .duration_us = 20000U};
  mock->callbacks.on_result(&fingerprint, mock->callback_user_data);
  mock->callbacks.on_complete(mock->callback_user_data);
  return TURBO_RECOGNITION_OK;
}

static int mock_fingerprint_cancel(void *context) {
  ((mock_fingerprint_provider_t *)context)->cancel_count++;
  return TURBO_RECOGNITION_OK;
}

static void mock_fingerprint_destroy(void *context) {
  ((mock_fingerprint_provider_t *)context)->destroy_count++;
}

static void observe_fingerprint(turbo_fingerprint_extractor_t *extractor,
                                const turbo_fingerprint_t *fingerprint, void *user_data) {
  fingerprint_observer_t *observer = (fingerprint_observer_t *)user_data;
  size_t len = fingerprint->len < sizeof(observer->fingerprint) ? fingerprint->len
                                                                : sizeof(observer->fingerprint);
  (void)extractor;
  memcpy(observer->fingerprint, fingerprint->data, len);
  observer->fingerprint_len = len;
  observer->result_count++;
}

static void observe_fingerprint_complete(turbo_fingerprint_extractor_t *extractor,
                                         void *user_data) {
  fingerprint_observer_t *observer = (fingerprint_observer_t *)user_data;
  (void)extractor;
  observer->complete_count++;
}

static void observe_fingerprint_error(turbo_fingerprint_extractor_t *extractor, int error_code,
                                      const char *message, void *user_data) {
  fingerprint_observer_t *observer = (fingerprint_observer_t *)user_data;
  (void)extractor;
  (void)error_code;
  (void)message;
  observer->error_count++;
}

static turbo_fingerprint_provider_t make_fingerprint_provider(mock_fingerprint_provider_t *mock) {
  turbo_fingerprint_provider_t provider = {.abi_version = TURBO_RECOGNITION_PROVIDER_ABI_VERSION,
                                           .context = mock,
                                           .start = mock_fingerprint_start,
                                           .write_audio = mock_fingerprint_write_audio,
                                           .write_video = mock_fingerprint_write_video,
                                           .finish = mock_fingerprint_finish,
                                           .cancel = mock_fingerprint_cancel,
                                           .destroy = mock_fingerprint_destroy};
  return provider;
}

static int mock_compare(void *context, const turbo_fingerprint_t *left,
                        const turbo_fingerprint_t *right, float *similarity) {
  mock_matcher_provider_t *mock = (mock_matcher_provider_t *)context;
  (void)left;
  (void)right;
  mock->compare_count++;
  *similarity = mock->similarity;
  return TURBO_RECOGNITION_OK;
}

static void mock_matcher_destroy(void *context) {
  ((mock_matcher_provider_t *)context)->destroy_count++;
}

suite("TurboMedia recognition") {
  group("voice activity") {
    it("feeds capture PCM and emits a bounded voice activity event") {
      static const uint8_t pcm[] = {0U, 0U, 1U, 0U};
      mock_voice_provider_t mock = {0};
      voice_observer_t observer = {0};
      turbo_voice_detector_provider_t provider = make_voice_provider(&mock);
      turbo_voice_detector_callbacks_t callbacks = {.on_activity = observe_voice_activity,
                                                    .on_complete = observe_voice_complete,
                                                    .on_error = observe_voice_error};
      turbo_voice_detector_config_t config = make_voice_config(TURBO_RECOGNITION_AUDIO_S16);
      turbo_voice_detector_t *detector =
          turbo_voice_detector_create(&provider, &callbacks, &observer);

      check_not_null(detector);
      check_equal(turbo_voice_detector_start(detector, &config), TURBO_RECOGNITION_OK);
      turbo_voice_detector_capture_callback(NULL, pcm, sizeof(pcm), 1000U, detector);
      check_equal(mock.write_count, 1);
      check_equal(observer.activity_count, 1);
      check_equal(observer.last_activity.state, TURBO_VOICE_ACTIVITY_SPEECH);
      check_equal((long)observer.last_activity.start_time_us, 1000L);
      check_equal(turbo_voice_detector_finish(detector), TURBO_RECOGNITION_OK);
      check_equal(observer.complete_count, 1);
      turbo_voice_detector_destroy(detector);
      check_equal(mock.destroy_count, 1);
    }

    it("reports capture-path backpressure without stopping the detector") {
      static const uint8_t pcm[] = {0U, 0U};
      mock_voice_provider_t mock = {.write_result = TURBO_RECOGNITION_ERR_BUSY};
      turbo_voice_detector_provider_t provider = make_voice_provider(&mock);
      turbo_voice_detector_config_t config = make_voice_config(TURBO_RECOGNITION_AUDIO_S16);
      turbo_voice_detector_t *detector = turbo_voice_detector_create(&provider, NULL, NULL);

      check_not_null(detector);
      check_equal(turbo_voice_detector_start(detector, &config), TURBO_RECOGNITION_OK);
      turbo_voice_detector_capture_callback(NULL, pcm, sizeof(pcm), 0U, detector);
      check_equal(turbo_voice_detector_get_last_result(detector), TURBO_RECOGNITION_ERR_BUSY);
      check_equal((long)turbo_voice_detector_get_rejected_frame_count(detector), 1L);
      check_equal(turbo_voice_detector_get_state(detector), TURBO_RECOGNITION_STATE_RUNNING);
      check_equal(turbo_voice_detector_cancel(detector), TURBO_RECOGNITION_OK);
      turbo_voice_detector_destroy(detector);
    }

    it("fails fast when a provider emits an invalid probability") {
      static const uint8_t pcm[] = {0U, 0U};
      mock_voice_provider_t mock = {.emit_invalid_activity = 1};
      voice_observer_t observer = {0};
      turbo_voice_detector_provider_t provider = make_voice_provider(&mock);
      turbo_voice_detector_callbacks_t callbacks = {.on_activity = observe_voice_activity,
                                                    .on_error = observe_voice_error};
      turbo_voice_detector_config_t config = make_voice_config(TURBO_RECOGNITION_AUDIO_S16);
      turbo_voice_detector_t *detector =
          turbo_voice_detector_create(&provider, &callbacks, &observer);

      check_not_null(detector);
      check_equal(turbo_voice_detector_start(detector, &config), TURBO_RECOGNITION_OK);
      turbo_voice_detector_capture_callback(NULL, pcm, sizeof(pcm), 0U, detector);
      check_equal(observer.activity_count, 0);
      check_equal(observer.error_count, 1);
      check_equal(turbo_voice_detector_get_state(detector), TURBO_RECOGNITION_STATE_ERROR);
      check_equal(turbo_voice_detector_get_last_result(detector), TURBO_RECOGNITION_ERR_PROVIDER);
      turbo_voice_detector_destroy(detector);
    }
  }

  group("fingerprint extraction") {
    it("extracts a borrowed voiceprint from capture PCM") {
      static const uint8_t pcm[] = {0U, 0U, 1U, 0U};
      mock_fingerprint_provider_t mock = {0};
      fingerprint_observer_t observer = {0};
      turbo_fingerprint_provider_t provider = make_fingerprint_provider(&mock);
      turbo_fingerprint_callbacks_t callbacks = {.on_result = observe_fingerprint,
                                                 .on_complete = observe_fingerprint_complete,
                                                 .on_error = observe_fingerprint_error};
      turbo_fingerprint_config_t config = {
          .domain = TURBO_FINGERPRINT_VOICE,
          .audio_format = {.sample_rate = 16000,
                           .channels = 1,
                           .sample_format = TURBO_RECOGNITION_AUDIO_S16},
          .max_duration_us = 1000000U};
      turbo_fingerprint_extractor_t *extractor =
          turbo_fingerprint_extractor_create(&provider, &callbacks, &observer);

      check_not_null(extractor);
      check_equal(turbo_fingerprint_extractor_start(extractor, &config), TURBO_RECOGNITION_OK);
      turbo_fingerprint_capture_callback(NULL, pcm, sizeof(pcm), 1000U, extractor);
      check_equal(mock.audio_write_count, 1);
      check_equal(turbo_fingerprint_extractor_finish(extractor), TURBO_RECOGNITION_OK);
      check_equal(observer.result_count, 1);
      check_equal(observer.fingerprint_len, 4U);
      check_equal(observer.fingerprint[0], 0x10);
      check_equal(observer.complete_count, 1);
      turbo_fingerprint_extractor_destroy(extractor);
    }

    it("rejects an invalid provider fingerprint without reporting completion") {
      mock_fingerprint_provider_t mock = {.emit_invalid_fingerprint = 1};
      fingerprint_observer_t observer = {0};
      turbo_fingerprint_provider_t provider = make_fingerprint_provider(&mock);
      turbo_fingerprint_callbacks_t callbacks = {.on_result = observe_fingerprint,
                                                 .on_complete = observe_fingerprint_complete,
                                                 .on_error = observe_fingerprint_error};
      turbo_fingerprint_config_t config = {
          .domain = TURBO_FINGERPRINT_VOICE,
          .audio_format = {.sample_rate = 16000,
                           .channels = 1,
                           .sample_format = TURBO_RECOGNITION_AUDIO_S16},
          .max_duration_us = 1000000U};
      turbo_fingerprint_extractor_t *extractor =
          turbo_fingerprint_extractor_create(&provider, &callbacks, &observer);

      check_not_null(extractor);
      check_equal(turbo_fingerprint_extractor_start(extractor, &config), TURBO_RECOGNITION_OK);
      check_equal(turbo_fingerprint_extractor_finish(extractor), TURBO_RECOGNITION_ERR_PROVIDER);
      check_equal(observer.result_count, 0);
      check_equal(observer.complete_count, 0);
      check_equal(observer.error_count, 1);
      check_equal(turbo_fingerprint_extractor_get_state(extractor), TURBO_RECOGNITION_STATE_ERROR);
      turbo_fingerprint_extractor_destroy(extractor);
    }

    it("adapts decoded VOD float audio without copying") {
      float samples[160] = {0};
      mock_fingerprint_provider_t mock = {0};
      turbo_fingerprint_provider_t provider = make_fingerprint_provider(&mock);
      turbo_fingerprint_config_t config = {
          .domain = TURBO_FINGERPRINT_AUDIO_CONTENT,
          .audio_format = {.sample_rate = 16000,
                           .channels = 1,
                           .sample_format = TURBO_RECOGNITION_AUDIO_F32},
          .max_duration_us = 1000000U};
      turbo_fingerprint_extractor_t *extractor =
          turbo_fingerprint_extractor_create(&provider, NULL, NULL);

      check_not_null(extractor);
      config.max_duration_us = 0U;
      check_equal(turbo_fingerprint_extractor_start(extractor, &config),
                   TURBO_RECOGNITION_ERR_INVALID);
      config.max_duration_us = 1000000U;
      check_equal(turbo_fingerprint_extractor_start(extractor, &config), TURBO_RECOGNITION_OK);
      turbo_fingerprint_player_audio_callback(NULL, samples, 160U, 16000, 1, 25, extractor);
      check_equal(mock.audio_write_count, 1);
      check_equal(mock.last_audio_format, TURBO_RECOGNITION_AUDIO_F32);
      check_equal(turbo_fingerprint_extractor_cancel(extractor), TURBO_RECOGNITION_OK);
      turbo_fingerprint_extractor_destroy(extractor);
    }

    it("does not consume the duration budget when a provider is busy") {
      uint8_t pcm[320] = {0};
      mock_fingerprint_provider_t mock = {.audio_result = TURBO_RECOGNITION_ERR_BUSY};
      turbo_fingerprint_provider_t provider = make_fingerprint_provider(&mock);
      turbo_fingerprint_config_t config = {
          .domain = TURBO_FINGERPRINT_VOICE,
          .audio_format = {.sample_rate = 16000,
                           .channels = 1,
                           .sample_format = TURBO_RECOGNITION_AUDIO_S16},
          .max_duration_us = 10000U};
      turbo_recognition_audio_frame_t frame = {
          .data = pcm, .len = sizeof(pcm), .format = config.audio_format};
      turbo_fingerprint_extractor_t *extractor =
          turbo_fingerprint_extractor_create(&provider, NULL, NULL);

      check_not_null(extractor);
      check_equal(turbo_fingerprint_extractor_start(extractor, &config), TURBO_RECOGNITION_OK);
      check_equal(turbo_fingerprint_extractor_write_audio(extractor, &frame),
                   TURBO_RECOGNITION_ERR_BUSY);
      mock.audio_result = TURBO_RECOGNITION_OK;
      check_equal(turbo_fingerprint_extractor_write_audio(extractor, &frame),
                   TURBO_RECOGNITION_OK);
      check_equal(mock.audio_write_count, 2);
      check_equal(turbo_fingerprint_extractor_cancel(extractor), TURBO_RECOGNITION_OK);
      turbo_fingerprint_extractor_destroy(extractor);
    }

    it("adapts decoded VOD video and enforces the configured duration limit") {
      uint8_t pixels[16] = {0};
      mock_fingerprint_provider_t mock = {0};
      turbo_fingerprint_provider_t provider = make_fingerprint_provider(&mock);
      turbo_fingerprint_config_t config = {.domain = TURBO_FINGERPRINT_VIDEO_CONTENT,
                                           .video_format = TURBO_RECOGNITION_VIDEO_RGBA,
                                           .max_duration_us = 100000U};
      turbo_player_video_frame_t frame = {.data = pixels,
                                          .len = sizeof(pixels),
                                          .width = 2,
                                          .height = 2,
                                          .stride = {8, 0, 0, 0},
                                          .pts_ms = 10,
                                          .format = TURBO_PLAYER_VIDEO_RGBA};
      turbo_fingerprint_extractor_t *extractor =
          turbo_fingerprint_extractor_create(&provider, NULL, NULL);

      check_not_null(extractor);
      check_equal(turbo_fingerprint_extractor_start(extractor, &config), TURBO_RECOGNITION_OK);
      turbo_fingerprint_player_video_callback(NULL, &frame, extractor);
      check_equal(mock.video_write_count, 1);
      check_equal(mock.last_video_format, TURBO_RECOGNITION_VIDEO_RGBA);
      frame.pts_ms = 111;
      turbo_fingerprint_player_video_callback(NULL, &frame, extractor);
      check_equal(mock.video_write_count, 1);
      check_equal((long)turbo_fingerprint_extractor_get_rejected_frame_count(extractor), 1L);
      check_equal(turbo_fingerprint_extractor_get_last_result(extractor),
                   TURBO_RECOGNITION_ERR_LIMIT);
      check_equal(turbo_fingerprint_extractor_cancel(extractor), TURBO_RECOGNITION_OK);
      turbo_fingerprint_extractor_destroy(extractor);
    }
  }

  group("fingerprint matching") {
    it("matches only fingerprints from the same domain, algorithm, and model") {
      static const uint8_t left_value[] = {1U, 2U};
      static const uint8_t right_value[] = {3U, 4U};
      mock_matcher_provider_t mock = {.similarity = 0.82f};
      turbo_fingerprint_matcher_provider_t provider = {.abi_version =
                                                           TURBO_RECOGNITION_PROVIDER_ABI_VERSION,
                                                       .context = &mock,
                                                       .compare = mock_compare,
                                                       .destroy = mock_matcher_destroy};
      turbo_fingerprint_t left = {.domain = TURBO_FINGERPRINT_VOICE,
                                  .algorithm = "speaker-model",
                                  .model_version = "v2",
                                  .data = left_value,
                                  .len = sizeof(left_value)};
      turbo_fingerprint_t right = {.domain = TURBO_FINGERPRINT_VOICE,
                                   .algorithm = "speaker-model",
                                   .model_version = "v2",
                                   .data = right_value,
                                   .len = sizeof(right_value)};
      turbo_fingerprint_match_result_t result = {0};
      turbo_fingerprint_matcher_t *matcher = turbo_fingerprint_matcher_create(&provider);

      check_not_null(matcher);
      check_equal(turbo_fingerprint_match(matcher, &left, &right, 0.80f, &result),
                   TURBO_RECOGNITION_OK);
      check_equal(result.is_match, 1);
      check_true(result.similarity > 0.81f && result.similarity < 0.83f);
      check_equal(mock.compare_count, 1);
      right.model_version = "v3";
      check_equal(turbo_fingerprint_match(matcher, &left, &right, 0.80f, &result),
                   TURBO_RECOGNITION_ERR_INCOMPATIBLE);
      check_equal(mock.compare_count, 1);
      right.model_version = "v2";
      mock.similarity = 1.5f;
      check_equal(turbo_fingerprint_match(matcher, &left, &right, 0.80f, &result),
                   TURBO_RECOGNITION_ERR_PROVIDER);
      check_equal(mock.compare_count, 2);
      turbo_fingerprint_matcher_destroy(matcher);
      check_equal(mock.destroy_count, 1);
    }
  }
}
