#include "tinytest.h"
#include "turbo_datachannel.h"
#include "turbo_media_engine.h"
#include "turbo_peer_connection.h"
#include "turbo_rtp.h"
#include <salts_thread.h>

enum {
  TEST_TOO_MANY_ICE_SERVERS = 5,
  TEST_SDP_BUFFER_SIZE = 8192,
  TEST_ICE_CREDENTIAL_SIZE = 128,
  TEST_PEER_PUMP_ITERATIONS = 128,
  TEST_PEER_PUMP_INTERVAL_MS = 1
};

#define TEST_SHA256_FINGERPRINT \
  "00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:" \
  "10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F"

#define TEST_PEER_SDP_PREFIX \
  "v=0\r\n" \
  "o=- 1 1 IN IP4 127.0.0.1\r\n" \
  "s=-\r\n" \
  "t=0 0\r\n" \
  "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n" \
  "c=IN IP4 0.0.0.0\r\n" \
  "a=mid:0\r\n" \
  "a=ice-ufrag:test\r\n" \
  "a=ice-pwd:test-remote-password-123\r\n"

typedef struct {
  int call_count;
  turbo_peer_state_t last_state;
} test_peer_state_context_t;

typedef struct {
  int call_count;
  turbo_media_track_t *last_track;
} test_remote_track_context_t;

static void on_test_peer_state(turbo_peer_connection_t *peer,
                               turbo_peer_state_t state,
                               void *user_data) {
  test_peer_state_context_t *context =
      (test_peer_state_context_t *)user_data;
  (void)peer;

  context->call_count++;
  context->last_state = state;
}

static void on_test_remote_track(turbo_peer_connection_t *peer,
                                 turbo_media_track_t *track,
                                 void *user_data) {
  test_remote_track_context_t *context =
      (test_remote_track_context_t *)user_data;
  (void)peer;

  context->call_count++;
  context->last_track = track;
}

static int copy_sdp_attribute(const char *sdp, const char *prefix,
                              char *value, size_t value_size) {
  const char *start;
  const char *end;
  size_t length;

  if (!sdp || !prefix || !value || value_size == 0) return -1;
  start = strstr(sdp, prefix);
  if (!start) return -1;
  start += strlen(prefix);
  end = strstr(start, "\r\n");
  if (!end) return -1;
  length = (size_t)(end - start);
  if (length == 0 || length >= value_size) return -1;
  memcpy(value, start, length);
  value[length] = '\0';
  return 0;
}

static void count_transport_send(void *transport, const void *data, size_t len) {
  int *send_count = (int *)transport;
  (void)data;
  (void)len;
  (*send_count)++;
}

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

  check_not_null(dc);
  peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
  check_not_null(peer);
  media = turbo_media_create(peer, &user_value);
  check_not_null(media);
  check_true(turbo_media_get_user_data(media) == &user_value);
  check_equal((int)(turbo_media_get_track_count(media)), (int)(0));

  track = turbo_media_add_track(media, &track_config);
  check_not_null(track);
  check_equal((int)(turbo_media_get_track_count(media)), (int)(1));
  check_true(turbo_media_get_track(media, 0) == track);
  check_equal((int)(turbo_media_track_get_type(track)), (int)(TURBO_RTC_MEDIA_TRACK_AUDIO));
  check_equal((int)(turbo_media_setup_srtp(media)), (int)(-1));

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

  check_not_null(dc);
  peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
  check_not_null(peer);
  media = turbo_media_create(peer, NULL);
  check_not_null(media);

  check_null(turbo_media_add_track(media, &track_config));
  check_equal((int)(turbo_media_get_track_count(media)), (int)(0));

  track_config.type = TURBO_RTC_MEDIA_TRACK_AUDIO;
  track_config.direction = (turbo_media_direction_t)0;
  check_null(turbo_media_add_track(media, &track_config));
  check_equal((int)(turbo_media_get_track_count(media)), (int)(0));

  turbo_media_destroy(media);
  turbo_dc_peer_destroy(peer);
  turbo_dc_context_destroy(dc);
}

static void test_rtcp_parser_accepts_minimum_packet(void) {
  uint8_t receiver_report[] = {0x80, RTCP_RR, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01};
  check_equal((int)(rtcp_compound_parse(receiver_report, sizeof(receiver_report), NULL, NULL)), (int)(1));
}

static void test_media_rejects_unprotected_rtp_send(void) {
  turbo_dc_config_t dc_config = {
      .is_server = 0,
      .transport = TURBO_DC_TRANSPORT_ICE,
  };
  turbo_media_track_config_t track_config = {
      .type = TURBO_RTC_MEDIA_TRACK_AUDIO,
      .direction = TURBO_MEDIA_DIRECTION_SENDONLY,
      .codec = TURBO_CODEC_PCMU,
      .audio =
          {
              .sample_rate = 8000,
              .channels = 1,
              .bitrate = 64000,
              .frame_size_ms = 20,
          },
  };
  static const uint8_t rtp_packet[] = {
      0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x12, 0x34, 0x56, 0x78, 0x7f,
  };
  int send_count = 0;
  turbo_dc_context_t *dc = turbo_dc_context_create(&dc_config);
  turbo_dc_peer_t *peer;
  turbo_media_context_t *media;
  turbo_media_track_t *track;

  check_not_null(dc);
  peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
  check_not_null(peer);
  check_equal((int)(turbo_dc_peer_set_external_transport(peer, &send_count, count_transport_send)), (int)(0));
  media = turbo_media_create(peer, NULL);
  check_not_null(media);
  track = turbo_media_add_track(media, &track_config);
  check_not_null(track);
  check_equal((int)(turbo_media_track_start(track)), (int)(0));

  check_equal((int)(turbo_media_track_send_rtp_packet(track, rtp_packet, sizeof(rtp_packet))), (int)(-1));
  check_equal((int)(send_count), (int)(0));

  turbo_media_destroy(media);
  check_equal((int)(turbo_dc_peer_set_external_transport(peer, NULL, NULL)), (int)(0));
  turbo_dc_peer_destroy(peer);
  turbo_dc_context_destroy(dc);
}

static void test_media_rejects_unprotected_rtp_receive(void) {
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
  static const uint8_t rtp_packet[] = {
      0x80, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x12, 0x34, 0x56, 0x78, 0x7f,
  };
  turbo_media_stats_t stats = {0};
  turbo_dc_context_t *dc = turbo_dc_context_create(&dc_config);
  turbo_dc_peer_t *peer;
  turbo_media_context_t *media;
  turbo_media_track_t *track;

  check_not_null(dc);
  peer = turbo_dc_peer_create(dc, NULL, 0, NULL);
  check_not_null(peer);
  media = turbo_media_create(peer, NULL);
  check_not_null(media);
  track = turbo_media_add_track(media, &track_config);
  check_not_null(track);
  check_equal((int)(turbo_media_track_start(track)), (int)(0));

  check_equal((int)(turbo_media_feed_data(media, rtp_packet, sizeof(rtp_packet))), (int)(-1));
  turbo_media_track_get_stats(track, &stats);
  check_equal((uint64_t)(stats.packets_recv), (uint64_t)(0));

  turbo_media_destroy(media);
  turbo_dc_peer_destroy(peer);
  turbo_dc_context_destroy(dc);
}

static void test_peer_connection_rejects_invalid_turbonet_ice_configuration(void) {
  const char *stun_servers[] = {"stun:127.0.0.1:3478"};
  const char *turn_servers[] = {"turn:user:password@127.0.0.1:9"};
  turbo_peer_config_t config = {0};

  config.stun_server_count = TEST_TOO_MANY_ICE_SERVERS;
  config.stun_servers = stun_servers;
  check_null(turbo_peer_connection_create(&config, NULL));

  config.stun_server_count = 1;
  config.stun_servers = NULL;
  check_null(turbo_peer_connection_create(&config, NULL));

  config.stun_server_count = 0;
  config.turn_server_count = 1;
  config.turn_servers = NULL;
  check_null(turbo_peer_connection_create(&config, NULL));

  config.turn_servers = turn_servers;
  config.turn_servers[0] = "turn:missing-credentials@127.0.0.1:9";
  check_null(turbo_peer_connection_create(&config, NULL));
}

static void test_peer_connection_accepts_turbonet_turn_configuration(void) {
  const char *turn_servers[] = {"turn:user:password@127.0.0.1:9"};
  turbo_peer_config_t config = {0};
  turbo_peer_connection_t *peer;

  config.allow_loopback = 1;
  config.disable_datachannel = 1;
  config.turn_servers = turn_servers;
  config.turn_server_count = 1;

  peer = turbo_peer_connection_create(&config, NULL);
  check_not_null(peer);
  turbo_peer_connection_destroy(peer);
}

static void test_peer_connection_requires_valid_remote_fingerprint(void) {
  static const char valid_sdp[] =
      TEST_PEER_SDP_PREFIX
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  static const char missing_fingerprint_sdp[] =
      TEST_PEER_SDP_PREFIX
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  static const char malformed_fingerprint_sdp[] =
      TEST_PEER_SDP_PREFIX
      "a=fingerprint:sha-256 AA:BB:CC:DD\r\n"
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  static const char unsupported_hash_sdp[] =
      TEST_PEER_SDP_PREFIX
      "a=fingerprint:sha-1 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  static const char unsupported_multi_transport_sdp[] =
      TEST_PEER_SDP_PREFIX
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:1\r\n"
      "a=ice-ufrag:other\r\n"
      "a=ice-pwd:other-remote-password-456\r\n"
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=recvonly\r\n"
      "a=rtpmap:111 opus/48000/2\r\n";
  static const char bundled_distinct_transport_sdp[] =
      "v=0\r\n"
      "o=- 1 1 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "a=group:BUNDLE 0 1\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:0\r\n"
      "a=ice-ufrag:video\r\n"
      "a=ice-pwd:video-remote-password-123\r\n"
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n"
      "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:1\r\n"
      "a=ice-ufrag:audio\r\n"
      "a=ice-pwd:audio-remote-password-456\r\n"
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=recvonly\r\n"
      "a=rtpmap:111 opus/48000/2\r\n";
  turbo_peer_config_t config = {0};
  turbo_peer_connection_t *peer;

  config.allow_loopback = 1;
  config.disable_datachannel = 1;
  peer = turbo_peer_connection_create(&config, NULL);
  check_not_null(peer);

  check_equal((int)(turbo_peer_connection_set_remote_description(
              peer, "offer", missing_fingerprint_sdp)), (int)(-1));
  check_equal((int)(turbo_peer_connection_set_remote_description(
              peer, "offer", malformed_fingerprint_sdp)), (int)(-1));
  check_equal((int)(turbo_peer_connection_set_remote_description(
              peer, "offer", unsupported_hash_sdp)), (int)(-1));
  check_equal((int)(turbo_peer_connection_set_remote_description(
              peer, "offer", unsupported_multi_transport_sdp)), (int)(-1));
  check_equal((int)(turbo_peer_connection_set_remote_description(
              peer, "pranswer", valid_sdp)), (int)(-1));
  check_equal((int)(turbo_peer_connection_set_remote_description(
             peer, "offer", bundled_distinct_transport_sdp)), (int)(0));
  check_equal((int)(turbo_peer_connection_set_remote_description(
             peer, "offer", valid_sdp)), (int)(0));

  turbo_peer_connection_destroy(peer);
}

static void test_peer_connection_propagates_datachannel_close(void) {
  test_peer_state_context_t state_context = {0};
  turbo_peer_callbacks_t callbacks = {
      .on_state_change = on_test_peer_state
  };
  turbo_peer_config_t config = {
      .allow_loopback = 1,
      .user_data = &state_context
  };
  turbo_peer_connection_t *peer =
      turbo_peer_connection_create(&config, &callbacks);
  turbo_dc_peer_t *dc_peer;

  check_not_null(peer);
  dc_peer = turbo_peer_connection_get_dc_peer(peer);
  check_not_null(dc_peer);

  turbo_dc_peer_close(dc_peer);
  check_equal((int)(state_context.call_count), (int)(1));
  check_equal((int)(state_context.last_state), (int)(TURBO_PEER_STATE_DISCONNECTED));

  turbo_dc_peer_close(dc_peer);
  check_equal((int)(state_context.call_count), (int)(1));

  turbo_peer_connection_destroy(peer);
}

static turbo_peer_connection_t *create_test_peer(void) {
  turbo_peer_config_t config = {
      .allow_loopback = 1,
      .disable_datachannel = 1
  };
  return turbo_peer_connection_create(&config, NULL);
}

static void pump_test_peer(turbo_peer_connection_t *peer) {
  for (int i = 0; i < TEST_PEER_PUMP_ITERATIONS; ++i) {
    turbo_peer_connection_poll(peer);
    salts_sleep_ms(TEST_PEER_PUMP_INTERVAL_MS);
  }
}

static void test_peer_connection_negotiates_dtls_setup_role(void) {
  static const char offer_actpass[] =
      TEST_PEER_SDP_PREFIX
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  static const char answer_active[] =
      TEST_PEER_SDP_PREFIX
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:active\r\n"
      "a=recvonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  static const char answer_passive[] =
      TEST_PEER_SDP_PREFIX
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:passive\r\n"
      "a=recvonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  turbo_peer_connection_t *peer = create_test_peer();

  check_not_null(peer);
  check_equal((int)(turbo_peer_connection_set_remote_description(
             peer, "offer", offer_actpass)), (int)(0));
  check_true(turbo_dc_peer_is_dtls_server(
      turbo_peer_connection_get_dc_peer(peer)));
  turbo_peer_connection_destroy(peer);

  peer = create_test_peer();
  check_not_null(peer);
  check_equal((int)(turbo_peer_connection_set_remote_description(
             peer, "answer", answer_active)), (int)(0));
  check_true(turbo_dc_peer_is_dtls_server(
      turbo_peer_connection_get_dc_peer(peer)));
  turbo_peer_connection_destroy(peer);

  peer = create_test_peer();
  check_not_null(peer);
  check_equal((int)(turbo_peer_connection_set_remote_description(
             peer, "answer", answer_passive)), (int)(0));
  check_false(turbo_dc_peer_is_dtls_server(
      turbo_peer_connection_get_dc_peer(peer)));
  check_equal((int)(turbo_peer_connection_set_remote_description(
              peer, "answer", offer_actpass)), (int)(-1));
  check_equal((int)(turbo_peer_connection_set_remote_description(
              peer, "offer", answer_active)), (int)(-1));
  turbo_peer_connection_destroy(peer);
}

static void test_peer_connection_answer_uses_offered_payload_type(void) {
  static const char offer[] =
      "v=0\r\n"
      "o=- 1 1 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 120\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:video-main\r\n"
      "a=ice-ufrag:test\r\n"
      "a=ice-pwd:test-remote-password-123\r\n"
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:120 VP8/90000\r\n";
  test_remote_track_context_t track_context = {0};
  turbo_peer_config_t peer_config = {
      .allow_loopback = 1,
      .disable_datachannel = 1,
      .user_data = &track_context
  };
  turbo_peer_callbacks_t callbacks = {
      .on_track = on_test_remote_track
  };
  turbo_peer_connection_t *peer =
      turbo_peer_connection_create(&peer_config, &callbacks);
  turbo_media_context_t *media;
  turbo_media_track_t *receive_track;
  char answer[8192];

  check_not_null(peer);
  receive_track = turbo_peer_connection_add_track(
      peer, TURBO_RTC_MEDIA_TRACK_VIDEO, TURBO_MEDIA_DIRECTION_RECVONLY);
  check_not_null(receive_track);
  media = turbo_peer_connection_get_media_context(peer);
  check_not_null(media);
  check_equal((int)(turbo_media_get_track_count(media)), (int)(1));

  check_equal((int)(turbo_peer_connection_set_remote_description(peer, "offer", offer)), (int)(0));
  check_equal((int)(turbo_media_get_track_count(media)), (int)(1));
  check_equal((int)(track_context.call_count), (int)(1));
  check_true(track_context.last_track == receive_track);
  check_equal((int)(turbo_peer_connection_set_remote_description(peer, "offer", offer)), (int)(0));
  check_equal((int)(track_context.call_count), (int)(1));
  check_equal((int)(turbo_media_get_track_count(media)), (int)(1));
  check_greater(turbo_peer_connection_create_answer(peer, answer, sizeof(answer)), 0);
  check_not_null(strstr(
      answer, "m=video 9 UDP/TLS/RTP/SAVPF 120\r\n"));
  check_not_null(strstr(answer, "a=rtpmap:120 VP8/90000\r\n"));

  turbo_peer_connection_destroy(peer);
}

static void test_peer_connection_answer_preserves_rejected_media_sections(void) {
  static const char unsupported_video_offer[] =
      "v=0\r\n"
      "o=- 1 1 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 120\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:video-x\r\n"
      "a=ice-ufrag:test\r\n"
      "a=ice-pwd:test-remote-password-123\r\n"
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:120 AV1/90000\r\n";
  static const char datachannel_offer[] =
      "v=0\r\n"
      "o=- 1 1 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:data\r\n"
      "a=ice-ufrag:test\r\n"
      "a=ice-pwd:test-remote-password-123\r\n"
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=sctp-port:5000\r\n";
  turbo_peer_connection_t *peer = create_test_peer();
  char answer[8192];

  check_not_null(peer);
  check_equal((int)(turbo_peer_connection_set_remote_description(
             peer, "offer", unsupported_video_offer)), (int)(0));
  check_greater(turbo_peer_connection_create_answer(peer, answer, sizeof(answer)), 0);
  check_not_null(strstr(
      answer, "m=video 0 UDP/TLS/RTP/SAVPF 120\r\n"));
  check_not_null(strstr(answer, "a=mid:video-x\r\n"));
  turbo_peer_connection_destroy(peer);

  peer = create_test_peer();
  check_not_null(peer);
  check_equal((int)(turbo_peer_connection_set_remote_description(
             peer, "offer", datachannel_offer)), (int)(0));
  check_greater(turbo_peer_connection_create_answer(peer, answer, sizeof(answer)), 0);
  check_not_null(strstr(
      answer,
      "m=application 0 UDP/DTLS/SCTP webrtc-datachannel\r\n"));
  check_not_null(strstr(answer, "a=mid:data\r\n"));
  turbo_peer_connection_destroy(peer);
}

static void test_peer_connection_local_ice_restart_changes_credentials(void) {
  turbo_peer_connection_t *peer = create_test_peer();
  turbo_media_track_t *track;
  char first_offer[TEST_SDP_BUFFER_SIZE];
  char restart_offer[TEST_SDP_BUFFER_SIZE];
  char first_ufrag[TEST_ICE_CREDENTIAL_SIZE];
  char restart_ufrag[TEST_ICE_CREDENTIAL_SIZE];

  check_not_null(peer);
  track = turbo_peer_connection_add_track(
      peer, TURBO_RTC_MEDIA_TRACK_VIDEO, TURBO_MEDIA_DIRECTION_SENDONLY);
  check_not_null(track);
  check_greater(turbo_peer_connection_create_offer(
             peer, first_offer, sizeof(first_offer)), 0);
  check_equal((int)(copy_sdp_attribute(
             first_offer, "a=ice-ufrag:", first_ufrag, sizeof(first_ufrag))), (int)(0));

  pump_test_peer(peer);
  check_equal((int)(turbo_peer_connection_restart_ice(peer)), (int)(0));
  check_greater(turbo_peer_connection_create_offer(
             peer, restart_offer, sizeof(restart_offer)), 0);
  check_equal((int)(copy_sdp_attribute(
             restart_offer, "a=ice-ufrag:", restart_ufrag,
             sizeof(restart_ufrag))), (int)(0));
  check_true(strcmp(first_ufrag, restart_ufrag) != 0);

  turbo_peer_connection_destroy(peer);
}

static void test_peer_connection_completes_local_ice_restart_with_new_answer(void) {
  static const char first_answer[] =
      TEST_PEER_SDP_PREFIX
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:passive\r\n"
      "a=recvonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  static const char restart_answer[] =
      "v=0\r\n"
      "o=- 2 2 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:0\r\n"
      "a=ice-ufrag:restart-remote\r\n"
      "a=ice-pwd:restart-remote-password-456\r\n"
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:passive\r\n"
      "a=recvonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  turbo_peer_connection_t *peer = create_test_peer();
  char offer[TEST_SDP_BUFFER_SIZE];

  check_not_null(peer);
  check_not_null(turbo_peer_connection_add_track(
      peer, TURBO_RTC_MEDIA_TRACK_VIDEO, TURBO_MEDIA_DIRECTION_SENDONLY));
  check_greater(turbo_peer_connection_create_offer(peer, offer, sizeof(offer)), 0);
  check_equal((int)(turbo_peer_connection_set_remote_description(
             peer, "answer", first_answer)), (int)(0));

  pump_test_peer(peer);
  check_equal((int)(turbo_peer_connection_restart_ice(peer)), (int)(0));
  check_greater(turbo_peer_connection_create_offer(peer, offer, sizeof(offer)), 0);
  check_equal((int)(turbo_peer_connection_set_remote_description(
              peer, "answer", first_answer)), (int)(-1));
  check_equal((int)(turbo_peer_connection_set_remote_description(
             peer, "answer", restart_answer)), (int)(0));

  turbo_peer_connection_destroy(peer);
}

static void test_peer_connection_answers_remote_ice_restart(void) {
  static const char first_offer[] =
      TEST_PEER_SDP_PREFIX
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  static const char restart_offer[] =
      "v=0\r\n"
      "o=- 2 2 IN IP4 127.0.0.1\r\n"
      "s=-\r\n"
      "t=0 0\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "a=mid:0\r\n"
      "a=ice-ufrag:remote-restart\r\n"
      "a=ice-pwd:remote-restart-password-456\r\n"
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  turbo_peer_connection_t *peer = create_test_peer();
  char first_answer[TEST_SDP_BUFFER_SIZE];
  char restart_answer[TEST_SDP_BUFFER_SIZE];
  char first_ufrag[TEST_ICE_CREDENTIAL_SIZE];
  char restart_ufrag[TEST_ICE_CREDENTIAL_SIZE];

  check_not_null(peer);
  check_not_null(turbo_peer_connection_add_track(
      peer, TURBO_RTC_MEDIA_TRACK_VIDEO, TURBO_MEDIA_DIRECTION_RECVONLY));
  check_equal((int)(turbo_peer_connection_set_remote_description(
             peer, "offer", first_offer)), (int)(0));
  check_greater(turbo_peer_connection_create_answer(
             peer, first_answer, sizeof(first_answer)), 0);
  check_equal((int)(copy_sdp_attribute(
             first_answer, "a=ice-ufrag:", first_ufrag, sizeof(first_ufrag))), (int)(0));

  pump_test_peer(peer);
  check_equal((int)(turbo_peer_connection_set_remote_description(
             peer, "offer", restart_offer)), (int)(0));
  check_greater(turbo_peer_connection_create_answer(
             peer, restart_answer, sizeof(restart_answer)), 0);
  check_equal((int)(copy_sdp_attribute(
             restart_answer, "a=ice-ufrag:", restart_ufrag,
             sizeof(restart_ufrag))), (int)(0));
  check_true(strcmp(first_ufrag, restart_ufrag) != 0);

  turbo_peer_connection_destroy(peer);
}

static void test_peer_connection_applies_trickle_ice_sdpfrag_restart(void) {
  static const char offer[] =
      TEST_PEER_SDP_PREFIX
      "a=fingerprint:sha-256 " TEST_SHA256_FINGERPRINT "\r\n"
      "a=setup:actpass\r\n"
      "a=sendonly\r\n"
      "a=rtpmap:96 VP8/90000\r\n";
  static const char same_generation_fragment[] =
      "a=ice-ufrag:test\r\n"
      "a=ice-pwd:test-remote-password-123\r\n";
  static const char restart_fragment[] =
      "a=ice-ufrag:fragment-restart\r\n"
      "a=ice-pwd:fragment-restart-password-456\r\n"
      "m=video 9 UDP/TLS/RTP/SAVPF 96\r\n"
      "a=mid:0\r\n"
      "a=end-of-candidates\r\n";
  static const char incomplete_restart_fragment[] =
      "a=ice-ufrag:missing-password\r\n";
  turbo_peer_connection_t *peer = create_test_peer();
  char first_answer[TEST_SDP_BUFFER_SIZE];
  char local_fragment[TEST_SDP_BUFFER_SIZE];
  char first_ufrag[TEST_ICE_CREDENTIAL_SIZE];
  char restart_ufrag[TEST_ICE_CREDENTIAL_SIZE];

  check_not_null(peer);
  check_not_null(turbo_peer_connection_add_track(
      peer, TURBO_RTC_MEDIA_TRACK_VIDEO, TURBO_MEDIA_DIRECTION_RECVONLY));
  check_equal((int)(turbo_peer_connection_set_remote_description(peer, "offer", offer)), (int)(0));
  check_greater(turbo_peer_connection_create_answer(
             peer, first_answer, sizeof(first_answer)), 0);
  check_equal((int)(copy_sdp_attribute(
             first_answer, "a=ice-ufrag:", first_ufrag, sizeof(first_ufrag))), (int)(0));

  check_equal((int)(turbo_peer_connection_apply_remote_ice_sdpfrag(
             peer, same_generation_fragment,
             strlen(same_generation_fragment))), (int)(0));
  check_equal((int)(turbo_peer_connection_apply_remote_ice_sdpfrag(
              peer, incomplete_restart_fragment,
              strlen(incomplete_restart_fragment))), (int)(-1));
  check_equal((int)(turbo_peer_connection_apply_remote_ice_sdpfrag(
             peer, restart_fragment, strlen(restart_fragment))), (int)(1));
  check_greater(turbo_peer_connection_create_local_ice_sdpfrag(
             peer, local_fragment, sizeof(local_fragment)), 0);
  check_equal((int)(copy_sdp_attribute(
             local_fragment, "a=ice-ufrag:", restart_ufrag,
             sizeof(restart_ufrag))), (int)(0));
  check_true(strcmp(first_ufrag, restart_ufrag) != 0);
  check_not_null(strstr(local_fragment, "m=video 9 UDP/TLS/RTP/SAVPF 96"));
  check_not_null(strstr(local_fragment, "a=mid:0\r\n"));

  turbo_peer_connection_destroy(peer);
}

spec("test_media_engine") {
  it("test_media_context_attach_tracks_and_detach") { test_media_context_attach_tracks_and_detach(); };
  it("test_media_track_rejects_invalid_configuration") { test_media_track_rejects_invalid_configuration(); };
  it("test_rtcp_parser_accepts_minimum_packet") { test_rtcp_parser_accepts_minimum_packet(); };
  it("test_media_rejects_unprotected_rtp_send") { test_media_rejects_unprotected_rtp_send(); };
  it("test_media_rejects_unprotected_rtp_receive") { test_media_rejects_unprotected_rtp_receive(); };
  it("test_peer_connection_rejects_invalid_turbonet_ice_configuration") { test_peer_connection_rejects_invalid_turbonet_ice_configuration(); };
  it("test_peer_connection_accepts_turbonet_turn_configuration") { test_peer_connection_accepts_turbonet_turn_configuration(); };
  it("test_peer_connection_requires_valid_remote_fingerprint") { test_peer_connection_requires_valid_remote_fingerprint(); };
  it("test_peer_connection_propagates_datachannel_close") { test_peer_connection_propagates_datachannel_close(); };
  it("test_peer_connection_negotiates_dtls_setup_role") { test_peer_connection_negotiates_dtls_setup_role(); };
  it("test_peer_connection_answer_uses_offered_payload_type") { test_peer_connection_answer_uses_offered_payload_type(); };
  it("test_peer_connection_answer_preserves_rejected_media_sections") { test_peer_connection_answer_preserves_rejected_media_sections(); };
  it("test_peer_connection_local_ice_restart_changes_credentials") { test_peer_connection_local_ice_restart_changes_credentials(); };
  it("test_peer_connection_completes_local_ice_restart_with_new_answer") { test_peer_connection_completes_local_ice_restart_with_new_answer(); };
  it("test_peer_connection_answers_remote_ice_restart") { test_peer_connection_answers_remote_ice_restart(); };
  it("test_peer_connection_applies_trickle_ice_sdpfrag_restart") { test_peer_connection_applies_trickle_ice_sdpfrag_restart(); };
}
