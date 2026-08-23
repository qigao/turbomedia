#include "tinytest.h"
#include "turbo_datachannel.h"
#include "turbo_media_engine.h"

#include <string.h>

typedef struct {
  int start_count;
  int cancel_count;
  int destroy_count;
} integration_asr_provider_t;

static int integration_asr_start(void *context, const turbo_asr_config_t *config,
                                 const turbo_asr_provider_callbacks_t *callbacks,
                                 void *callback_user_data) {
  integration_asr_provider_t *provider = (integration_asr_provider_t *)context;
  (void)config;
  (void)callbacks;
  (void)callback_user_data;
  provider->start_count++;
  return TURBO_SPEECH_OK;
}

static int integration_asr_write(void *context, const turbo_speech_audio_frame_t *frame) {
  (void)context;
  (void)frame;
  return TURBO_SPEECH_OK;
}

static int integration_asr_finish(void *context) {
  (void)context;
  return TURBO_SPEECH_OK;
}

static int integration_asr_cancel(void *context) {
  integration_asr_provider_t *provider = (integration_asr_provider_t *)context;
  provider->cancel_count++;
  return TURBO_SPEECH_OK;
}

static void integration_asr_destroy(void *context) {
  integration_asr_provider_t *provider = (integration_asr_provider_t *)context;
  provider->destroy_count++;
}

static turbo_asr_t *create_integration_asr(integration_asr_provider_t *provider_context,
                                           int sample_rate) {
  turbo_asr_provider_t provider = {.abi_version = TURBO_SPEECH_PROVIDER_ABI_VERSION,
                                   .context = provider_context,
                                   .start = integration_asr_start,
                                   .write = integration_asr_write,
                                   .finish = integration_asr_finish,
                                   .cancel = integration_asr_cancel,
                                   .destroy = integration_asr_destroy};
  turbo_asr_config_t config = {
      .format = {.sample_rate = sample_rate, .channels = 1, .bits_per_sample = 16}};
  turbo_asr_t *asr = turbo_asr_create(&provider, NULL, NULL);
  if (!asr) return NULL;
  if (turbo_asr_start(asr, &config) != TURBO_SPEECH_OK) {
    turbo_asr_destroy(asr);
    return NULL;
  }
  return asr;
}

suite("WebRTC speech integration") {
  it("binds ASR only while the audio track control plane is quiescent") {
    turbo_dc_config_t dc_config = {.is_server = 0, .transport = TURBO_DC_TRANSPORT_ICE};
    turbo_media_track_config_t track_config = {
        .type = TURBO_RTC_MEDIA_TRACK_AUDIO,
        .direction = TURBO_MEDIA_DIRECTION_SENDONLY,
        .codec = TURBO_CODEC_PCMU,
        .audio = {.sample_rate = 8000, .channels = 1, .bitrate = 64000, .frame_size_ms = 20}};
    integration_asr_provider_t provider_context = {0};
    turbo_dc_context_t *dc = turbo_dc_context_create(&dc_config);
    turbo_dc_peer_t *peer;
    turbo_media_context_t *media;
    turbo_media_track_t *track;
    turbo_asr_t *asr;

    check_not_null(dc);
    peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
    check_not_null(peer);
    media = turbo_media_create(peer, NULL);
    check_not_null(media);
    track = turbo_media_add_track(media, &track_config);
    check_not_null(track);
    asr = create_integration_asr(&provider_context, 8000);
    check_not_null(asr);

    check_equal(turbo_media_track_attach_asr(track, asr), 0);
    check_equal(turbo_media_track_start(track), 0);
    check_equal(turbo_media_track_detach_asr(track, asr), -1);
    turbo_media_track_stop(track);
    check_equal(turbo_media_track_detach_asr(track, asr), 0);

    check_equal(turbo_asr_cancel(asr), TURBO_SPEECH_OK);
    check_equal(turbo_media_track_attach_asr(track, asr), -1);
    turbo_asr_destroy(asr);
    turbo_media_destroy(media);
    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(dc);
    check_equal(provider_context.start_count, 1);
    check_equal(provider_context.cancel_count, 1);
    check_equal(provider_context.destroy_count, 1);
  }

  it("requires SRTP before sending matching TTS PCM") {
    enum { PCM_SAMPLES = 160 };
    int16_t pcm[PCM_SAMPLES];
    turbo_speech_audio_frame_t frame = {
        .data = (const uint8_t *)pcm,
        .len = sizeof(pcm),
        .format = {.sample_rate = 8000, .channels = 1, .bits_per_sample = 16}};
    turbo_dc_config_t dc_config = {.is_server = 0, .transport = TURBO_DC_TRANSPORT_ICE};
    turbo_media_track_config_t track_config = {
        .type = TURBO_RTC_MEDIA_TRACK_AUDIO,
        .direction = TURBO_MEDIA_DIRECTION_SENDONLY,
        .codec = TURBO_CODEC_PCMU,
        .audio = {.sample_rate = 8000, .channels = 1, .bitrate = 64000, .frame_size_ms = 20}};
    turbo_dc_context_t *dc = turbo_dc_context_create(&dc_config);
    turbo_dc_peer_t *peer;
    turbo_media_context_t *media;
    turbo_media_track_t *track;

    memset(pcm, 0, sizeof(pcm));
    check_not_null(dc);
    peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
    check_not_null(peer);
    media = turbo_media_create(peer, NULL);
    check_not_null(media);
    track = turbo_media_add_track(media, &track_config);
    check_not_null(track);
    check_equal(turbo_media_track_start(track), 0);
    check_equal(turbo_media_track_send_speech_frame(track, &frame), -1);

    frame.format.sample_rate = 16000;
    check_equal(turbo_media_track_send_speech_frame(track, &frame), -1);
    frame.format.sample_rate = 8000;
    frame.len -= sizeof(int16_t);
    check_equal(turbo_media_track_send_speech_frame(track, &frame), -1);
    turbo_media_destroy(media);
    turbo_dc_peer_destroy(peer);
    turbo_dc_context_destroy(dc);
  }
}
