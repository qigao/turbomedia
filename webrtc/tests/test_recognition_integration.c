#include "tinytest_compat.h"
#include "turbo_datachannel.h"
#include "turbo_media_engine.h"

typedef struct {
  int cancel_count;
  int destroy_count;
} recognition_provider_context_t;

static int detector_start(void *context, const turbo_voice_detector_config_t *config,
                          const turbo_voice_detector_provider_callbacks_t *callbacks,
                          void *callback_user_data) {
  (void)context;
  (void)config;
  (void)callbacks;
  (void)callback_user_data;
  return TURBO_RECOGNITION_OK;
}

static int detector_write(void *context, const turbo_recognition_audio_frame_t *frame) {
  (void)context;
  (void)frame;
  return TURBO_RECOGNITION_OK;
}

static int provider_finish(void *context) {
  (void)context;
  return TURBO_RECOGNITION_OK;
}

static int provider_cancel(void *context) {
  ((recognition_provider_context_t *)context)->cancel_count++;
  return TURBO_RECOGNITION_OK;
}

static void provider_destroy(void *context) {
  ((recognition_provider_context_t *)context)->destroy_count++;
}

static int fingerprint_start(void *context, const turbo_fingerprint_config_t *config,
                             const turbo_fingerprint_provider_callbacks_t *callbacks,
                             void *callback_user_data) {
  (void)context;
  (void)config;
  (void)callbacks;
  (void)callback_user_data;
  return TURBO_RECOGNITION_OK;
}

static int fingerprint_write_audio(void *context, const turbo_recognition_audio_frame_t *frame) {
  (void)context;
  (void)frame;
  return TURBO_RECOGNITION_OK;
}

static turbo_voice_detector_t *create_detector(recognition_provider_context_t *context,
                                               int sample_rate) {
  turbo_voice_detector_provider_t provider = {.abi_version = TURBO_RECOGNITION_PROVIDER_ABI_VERSION,
                                              .context = context,
                                              .start = detector_start,
                                              .write = detector_write,
                                              .finish = provider_finish,
                                              .cancel = provider_cancel,
                                              .destroy = provider_destroy};
  turbo_voice_detector_config_t config = {.format = {.sample_rate = sample_rate,
                                                     .channels = 1,
                                                     .sample_format = TURBO_RECOGNITION_AUDIO_S16}};
  turbo_voice_detector_t *detector = turbo_voice_detector_create(&provider, NULL, NULL);
  if (!detector) return NULL;
  if (turbo_voice_detector_start(detector, &config) != TURBO_RECOGNITION_OK) {
    turbo_voice_detector_destroy(detector);
    return NULL;
  }
  return detector;
}

static turbo_fingerprint_extractor_t *
create_voice_fingerprint(recognition_provider_context_t *context, turbo_fingerprint_domain_t domain,
                         int sample_rate) {
  turbo_fingerprint_provider_t provider = {.abi_version = TURBO_RECOGNITION_PROVIDER_ABI_VERSION,
                                           .context = context,
                                           .start = fingerprint_start,
                                           .write_audio = fingerprint_write_audio,
                                           .finish = provider_finish,
                                           .cancel = provider_cancel,
                                           .destroy = provider_destroy};
  turbo_fingerprint_config_t config = {
      .domain = domain,
      .audio_format = {.sample_rate = sample_rate,
                       .channels = 1,
                       .sample_format = TURBO_RECOGNITION_AUDIO_S16},
      .max_duration_us = 30000000U};
  turbo_fingerprint_extractor_t *extractor =
      turbo_fingerprint_extractor_create(&provider, NULL, NULL);
  if (!extractor) return NULL;
  if (turbo_fingerprint_extractor_start(extractor, &config) != TURBO_RECOGNITION_OK) {
    turbo_fingerprint_extractor_destroy(extractor);
    return NULL;
  }
  return extractor;
}

suite("WebRTC recognition integration") {
  it("attaches voice detector and voiceprint only while the track is quiescent") {
    turbo_dc_config_t dc_config = {.is_server = 0, .transport = TURBO_DC_TRANSPORT_ICE};
    turbo_media_track_config_t track_config = {
        .type = TURBO_RTC_MEDIA_TRACK_AUDIO,
        .direction = TURBO_MEDIA_DIRECTION_SENDONLY,
        .codec = TURBO_CODEC_PCMU,
        .audio = {.sample_rate = 8000, .channels = 1, .bitrate = 64000, .frame_size_ms = 20}};
    recognition_provider_context_t detector_context = {0};
    recognition_provider_context_t fingerprint_context = {0};
    turbo_dc_context_t *dc = turbo_dc_context_create(&dc_config);
    turbo_dc_peer_t *peer;
    turbo_media_context_t *media;
    turbo_media_track_t *track;
    turbo_voice_detector_t *detector;
    turbo_fingerprint_extractor_t *fingerprint;

    check_not_null(dc);
    peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
    check_not_null(peer);
    media = turbo_media_create(peer, NULL);
    check_not_null(media);
    track = turbo_media_add_track(media, &track_config);
    check_not_null(track);
    detector = create_detector(&detector_context, 8000);
    fingerprint = create_voice_fingerprint(&fingerprint_context, TURBO_FINGERPRINT_VOICE, 8000);
    check_not_null(detector);
    check_not_null(fingerprint);

    check_int_eq(turbo_media_track_attach_voice_detector(track, detector), 0);
    check_int_eq(turbo_media_track_attach_voice_fingerprint(track, fingerprint), 0);
    check_int_eq(turbo_media_track_start(track), 0);
    check_int_eq(turbo_media_track_detach_voice_detector(track, detector), -1);
    check_int_eq(turbo_media_track_detach_voice_fingerprint(track, fingerprint), -1);
    turbo_media_track_stop(track);
    check_int_eq(turbo_media_track_detach_voice_detector(track, detector), 0);
    check_int_eq(turbo_media_track_detach_voice_fingerprint(track, fingerprint), 0);

    check_int_eq(turbo_voice_detector_cancel(detector), TURBO_RECOGNITION_OK);
    check_int_eq(turbo_fingerprint_extractor_cancel(fingerprint), TURBO_RECOGNITION_OK);
    turbo_voice_detector_destroy(detector);
    turbo_fingerprint_extractor_destroy(fingerprint);
    turbo_media_destroy(media);
    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(dc);
    check_int_eq(detector_context.destroy_count, 1);
    check_int_eq(fingerprint_context.destroy_count, 1);
  }

  it("rejects mismatched formats and non-voice fingerprint domains") {
    turbo_dc_config_t dc_config = {.is_server = 0, .transport = TURBO_DC_TRANSPORT_ICE};
    turbo_media_track_config_t track_config = {
        .type = TURBO_RTC_MEDIA_TRACK_AUDIO,
        .direction = TURBO_MEDIA_DIRECTION_SENDONLY,
        .codec = TURBO_CODEC_PCMU,
        .audio = {.sample_rate = 8000, .channels = 1, .bitrate = 64000, .frame_size_ms = 20}};
    recognition_provider_context_t detector_context = {0};
    recognition_provider_context_t fingerprint_context = {0};
    turbo_dc_context_t *dc = turbo_dc_context_create(&dc_config);
    turbo_dc_peer_t *peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
    turbo_media_context_t *media = turbo_media_create(peer, NULL);
    turbo_media_track_t *track = turbo_media_add_track(media, &track_config);
    turbo_voice_detector_t *detector = create_detector(&detector_context, 16000);
    turbo_fingerprint_extractor_t *fingerprint =
        create_voice_fingerprint(&fingerprint_context, TURBO_FINGERPRINT_AUDIO_CONTENT, 8000);

    check_int_eq(turbo_media_track_attach_voice_detector(track, detector), -1);
    check_int_eq(turbo_media_track_attach_voice_fingerprint(track, fingerprint), -1);
    check_int_eq(turbo_voice_detector_cancel(detector), TURBO_RECOGNITION_OK);
    check_int_eq(turbo_fingerprint_extractor_cancel(fingerprint), TURBO_RECOGNITION_OK);
    turbo_voice_detector_destroy(detector);
    turbo_fingerprint_extractor_destroy(fingerprint);
    turbo_media_destroy(media);
    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(dc);
  }
}
