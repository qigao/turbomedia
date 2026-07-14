#include "tinytest_compat.h"
#include "turbo_datachannel.h"
#include "turbo_media_engine.h"

static void test_media_context_attach_tracks_and_detach(void) {
  turbo_dc_config_t dc_config = {
      .is_server = 0,
      .transport = TURBO_DC_TRANSPORT_ICE,
  };
  turbo_media_track_config_t track_config = {
      .type = TURBO_RTC_MEDIA_TRACK_AUDIO,
      .direction = TURBO_MEDIA_DIRECTION_RECVONLY,
      .codec = TURBO_CODEC_PCMU,
      .audio =
          {
              .sample_rate = 8000,
              .channels = 1,
              .frame_size_ms = 20,
          },
  };
  int user_value = 42;
  turbo_dc_context_t *dc = turbo_dc_context_create(&dc_config);
  turbo_dc_peer_t *peer;
  turbo_media_context_t *media;
  turbo_media_track_t *track;

  TEST_ASSERT_NOT_NULL(dc);
  peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
  TEST_ASSERT_NOT_NULL(peer);
  media = turbo_media_create(peer, &user_value);
  TEST_ASSERT_NOT_NULL(media);
  TEST_ASSERT_TRUE(turbo_media_get_user_data(media) == &user_value);
  TEST_ASSERT_EQUAL_INT(0, turbo_media_get_track_count(media));

  track = turbo_media_add_track(media, &track_config);
  TEST_ASSERT_NOT_NULL(track);
  TEST_ASSERT_EQUAL_INT(1, turbo_media_get_track_count(media));
  TEST_ASSERT_TRUE(turbo_media_get_track(media, 0) == track);
  TEST_ASSERT_EQUAL_INT(TURBO_RTC_MEDIA_TRACK_AUDIO, turbo_media_track_get_type(track));
  TEST_ASSERT_EQUAL_INT(-1, turbo_media_setup_srtp(media));

  turbo_media_destroy(media);
  turbo_dc_peer_destroy(peer);
  turbo_dc_context_destroy(dc);
}

static void test_media_track_rejects_invalid_configuration(void) {
  turbo_dc_config_t dc_config = {
      .is_server = 0,
      .transport = TURBO_DC_TRANSPORT_ICE,
  };
  turbo_media_track_config_t track_config = {
      .type = (turbo_rtc_media_track_type_t)99,
      .direction = TURBO_MEDIA_DIRECTION_RECVONLY,
      .codec = TURBO_CODEC_PCMU,
  };
  turbo_dc_context_t *dc = turbo_dc_context_create(&dc_config);
  turbo_dc_peer_t *peer;
  turbo_media_context_t *media;

  TEST_ASSERT_NOT_NULL(dc);
  peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
  TEST_ASSERT_NOT_NULL(peer);
  media = turbo_media_create(peer, NULL);
  TEST_ASSERT_NOT_NULL(media);

  TEST_ASSERT_NULL(turbo_media_add_track(media, &track_config));
  TEST_ASSERT_EQUAL_INT(0, turbo_media_get_track_count(media));

  track_config.type = TURBO_RTC_MEDIA_TRACK_AUDIO;
  track_config.direction = (turbo_media_direction_t)0;
  TEST_ASSERT_NULL(turbo_media_add_track(media, &track_config));
  TEST_ASSERT_EQUAL_INT(0, turbo_media_get_track_count(media));

  turbo_media_destroy(media);
  turbo_dc_peer_destroy(peer);
  turbo_dc_context_destroy(dc);
}

spec("test_media_engine") {
  TT_TEST(test_media_context_attach_tracks_and_detach);
  TT_TEST(test_media_track_rejects_invalid_configuration);
}
