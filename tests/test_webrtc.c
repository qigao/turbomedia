#include "turbo_media_rtc.h"
#include "../rtc/rtc_peer_backend.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

#ifdef TURBO_MEDIA_HAS_RTC

#define CHECK_KEY(key, expected_vhost, expected_app, expected_stream) \
    do {                                                             \
        check_str_eq((key).vhost, (expected_vhost));                 \
        check_str_eq((key).app, (expected_app));                     \
        check_str_eq((key).stream, (expected_stream));               \
    } while (0)

#define RTC_TEST_ANSWER "v=0\r\n"
#define RTC_TEST_OFFER "v=0\r\n"

typedef struct {
    int mode;
    int emit_connected;
    int fail_send;
    int create_count;
    int destroy_count;
    int add_send_track_count;
    int start_track_count;
    int send_count;
    int pump_count;
    int ice_count;
    int last_sent_track_id;
    size_t last_sent_size;
    uint8_t last_sent_byte;
} rtc_fake_state_t;

struct turbo_media_rtc_backend_track_s {
    turbo_media_rtc_backend_track_info_t info;
};

struct turbo_media_rtc_backend_peer_s {
    turbo_media_rtc_backend_config_t config;
    turbo_media_rtc_backend_track_t tracks[8];
    size_t track_count;
};

static rtc_fake_state_t rtc_fake;
static turbo_media_rtc_backend_peer_t *rtc_fake_peer;

static turbo_media_rtc_backend_peer_t *rtc_fake_create(
    const turbo_media_rtc_backend_config_t *config) {
    turbo_media_rtc_backend_peer_t *peer;

    if (!config) return NULL;
    peer = (turbo_media_rtc_backend_peer_t *)calloc(1, sizeof(*peer));
    if (!peer) return NULL;
    peer->config = *config;
    rtc_fake.create_count++;
    rtc_fake_peer = peer;
    return peer;
}

static void rtc_fake_destroy(turbo_media_rtc_backend_peer_t *peer) {
    if (!peer) return;
    rtc_fake.destroy_count++;
    if (rtc_fake_peer == peer) rtc_fake_peer = NULL;
    free(peer);
}

static int rtc_fake_add_send_track(
    turbo_media_rtc_backend_peer_t *peer,
    const turbo_media_rtc_backend_track_info_t *info,
    turbo_media_rtc_backend_track_t **track) {
    if (!peer || !info || !track || peer->track_count >= 8) return -1;
    peer->tracks[peer->track_count].info = *info;
    *track = &peer->tracks[peer->track_count++];
    rtc_fake.add_send_track_count++;
    return 0;
}

static int rtc_fake_set_remote_offer(turbo_media_rtc_backend_peer_t *peer,
                                     const char *offer_sdp) {
    turbo_media_rtc_backend_track_t *track;

    if (!peer || !offer_sdp || strcmp(offer_sdp, RTC_TEST_OFFER) != 0) return -1;
    if (rtc_fake.mode != TURBO_MEDIA_RTC_ROLE_PUBLISHER) return 0;
    if (peer->track_count >= 8) return -1;

    track = &peer->tracks[peer->track_count++];
    memset(track, 0, sizeof(*track));
    track->info.track_id = 0;
    track->info.type = TURBO_MEDIA_RTC_BACKEND_TRACK_VIDEO;
    strcpy(track->info.codec_name, "h264");
    track->info.payload_type = 102;
    track->info.clock_rate = 90000;
    return peer->config.on_remote_track(
        peer->config.user_data, track, &track->info);
}

static int rtc_fake_create_answer(turbo_media_rtc_backend_peer_t *peer,
                                  char *answer_sdp,
                                  size_t answer_sdp_capacity,
                                  size_t *answer_sdp_length) {
    size_t length = strlen(RTC_TEST_ANSWER);

    if (!peer || !answer_sdp || answer_sdp_capacity <= length) return -1;
    memcpy(answer_sdp, RTC_TEST_ANSWER, length + 1);
    if (answer_sdp_length) *answer_sdp_length = length;
    if (peer->config.on_ice_candidate) {
        peer->config.on_ice_candidate(
            peer->config.user_data, "candidate:test");
    }
    return 0;
}

static int rtc_fake_add_ice_candidate(turbo_media_rtc_backend_peer_t *peer,
                                      const char *candidate) {
    if (!peer || !candidate || strcmp(candidate, "candidate:remote") != 0) return -1;
    rtc_fake.ice_count++;
    return 0;
}

static int rtc_fake_start_track(turbo_media_rtc_backend_track_t *track) {
    if (!track) return -1;
    rtc_fake.start_track_count++;
    return 0;
}

static int rtc_fake_send_rtp(turbo_media_rtc_backend_track_t *track,
                             const uint8_t *packet,
                             size_t packet_len) {
    if (!track || !packet || packet_len == 0 || rtc_fake.fail_send) return -1;
    rtc_fake.send_count++;
    rtc_fake.last_sent_track_id = track->info.track_id;
    rtc_fake.last_sent_size = packet_len;
    rtc_fake.last_sent_byte = packet[packet_len - 1];
    return 0;
}

static int rtc_fake_pump(turbo_media_rtc_backend_peer_t *peer) {
    if (!peer) return -1;
    rtc_fake.pump_count++;
    if (rtc_fake.emit_connected) {
        rtc_fake.emit_connected = 0;
        peer->config.on_state(
            peer->config.user_data, TURBO_MEDIA_RTC_PEER_CONNECTED);
    }
    return 0;
}

static const turbo_media_rtc_backend_ops_t rtc_fake_ops = {
    rtc_fake_create,
    rtc_fake_destroy,
    rtc_fake_add_send_track,
    rtc_fake_set_remote_offer,
    rtc_fake_create_answer,
    rtc_fake_add_ice_candidate,
    rtc_fake_start_track,
    rtc_fake_send_rtp,
    rtc_fake_pump
};

static int rtc_fake_emit_rtp(const uint8_t *packet, size_t packet_len) {
    if (!rtc_fake_peer || rtc_fake_peer->track_count == 0) return -1;
    return rtc_fake_peer->config.on_rtp(
        rtc_fake_peer->config.user_data,
        &rtc_fake_peer->tracks[0],
        packet,
        packet_len);
}

typedef struct {
    int count;
    int track_id;
    int64_t pts;
    int is_keyframe;
} rtc_capture_t;

static int rtc_capture_frame(turbo_media_source_t *source,
                             const turbo_media_frame_t *frame,
                             void *user_data) {
    rtc_capture_t *capture = (rtc_capture_t *)user_data;

    (void)source;
    capture->count++;
    capture->track_id = frame->track_id;
    capture->pts = frame->pts;
    capture->is_keyframe = frame->is_keyframe;
    return TURBO_MEDIA_OK;
}

typedef struct {
    int ice_count;
    int state_count;
    turbo_media_rtc_peer_state_t last_state;
} rtc_event_capture_t;

static void rtc_capture_ice(turbo_media_rtc_session_t *session,
                            const char *candidate,
                            void *user_data) {
    rtc_event_capture_t *capture = (rtc_event_capture_t *)user_data;

    check_not_null(session);
    check_str_eq(candidate, "candidate:test");
    capture->ice_count++;
}

static void rtc_capture_state(turbo_media_rtc_session_t *session,
                              turbo_media_rtc_peer_state_t state,
                              void *user_data) {
    rtc_event_capture_t *capture = (rtc_event_capture_t *)user_data;

    check_not_null(session);
    capture->state_count++;
    capture->last_state = state;
}

static turbo_media_track_info_t rtc_h264_track(void) {
    turbo_media_track_info_t track;

    memset(&track, 0, sizeof(track));
    track.track_id = 0;
    track.type = TURBO_MEDIA_TRACK_VIDEO;
    strcpy(track.codec_name, "h264");
    track.payload_type = 102;
    track.clock_rate = 90000;
    return track;
}

static void rtc_reset_fake(turbo_media_rtc_role_t mode) {
    memset(&rtc_fake, 0, sizeof(rtc_fake));
    rtc_fake.mode = mode;
    rtc_fake.last_sent_track_id = -1;
    rtc_fake_peer = NULL;
}

suite("turbo_media_rtc") {
    before_each() {
        rtc_reset_fake(TURBO_MEDIA_RTC_ROLE_PUBLISHER);
    }

    section("source keys") {
        it("maps WHIP resource path to the shared source key") {
            turbo_media_source_key_t key;

            memset(&key, 0, sizeof(key));
            check_int_eq(
                turbo_media_rtc_source_key("default", "/live/cam", NULL, &key),
                TURBO_MEDIA_OK);
            CHECK_KEY(key, "default", "live", "cam");
        }

        it("uses query app and stream before path segments") {
            turbo_media_source_key_t key;

            memset(&key, 0, sizeof(key));
            check_int_eq(
                turbo_media_rtc_source_key(
                    "default",
                    "/ignored/path",
                    "vhost=edge&app=live&stream=cam",
                    &key),
                TURBO_MEDIA_OK);
            CHECK_KEY(key, "edge", "live", "cam");
        }

        it("rejects partial query identity") {
            turbo_media_source_key_t key;

            memset(&key, 0, sizeof(key));
            check_int_eq(
                turbo_media_rtc_source_key("default", "/live/cam", "app=live", &key),
                TURBO_MEDIA_ERR_INVALID);
        }

        it("rejects paths without app and stream") {
            turbo_media_source_key_t key;

            memset(&key, 0, sizeof(key));
            check_int_eq(
                turbo_media_rtc_source_key("default", "/live", NULL, &key),
                TURBO_MEDIA_ERR_INVALID);
        }
    }

    section("session bridge") {
        it("publishes remote WebRTC RTP through ServerRuntime") {
            turbo_media_server_runtime_t *runtime;
            turbo_media_rtc_session_config_t config;
            turbo_media_rtc_session_t *session = NULL;
            turbo_media_protocol_session_t *player = NULL;
            turbo_media_protocol_session_config_t player_config;
            turbo_media_source_t *source = NULL;
            turbo_media_source_key_t key;
            turbo_media_track_info_t track;
            rtc_capture_t capture;
            rtc_event_capture_t events;
            char answer[64];
            size_t answer_length = 0;
            uint8_t idr_packet[] = {
                0x80, 0xE6, 0x00, 0x01,
                0x00, 0x01, 0x5F, 0x90,
                0x01, 0x02, 0x03, 0x04,
                0x65, 0x88
            };

            memset(&capture, 0, sizeof(capture));
            memset(&events, 0, sizeof(events));
            runtime = turbo_media_server_runtime_create(NULL);
            check_not_null(runtime);

            memset(&config, 0, sizeof(config));
            config.runtime = runtime;
            config.role = TURBO_MEDIA_RTC_ROLE_PUBLISHER;
            config.resource_path = "/live/cam";
            config.remote_offer_sdp = RTC_TEST_OFFER;
            config.remove_source_on_close = 1;
            config.on_ice_candidate = rtc_capture_ice;
            config.on_state = rtc_capture_state;
            config.user_data = &events;
            check_int_eq(turbo_media_rtc_session_create_with_backend(
                             &config, &rtc_fake_ops, answer, sizeof(answer),
                             &answer_length, &session),
                         TURBO_MEDIA_OK);
            check_not_null(session);
            check_str_eq(answer, RTC_TEST_ANSWER);
            check_size_eq(answer_length, strlen(RTC_TEST_ANSWER));
            check_int_eq(events.ice_count, 1);

            key = *turbo_media_rtc_session_key(session);
            check_int_eq(turbo_media_server_runtime_find_source(runtime, &key, &source),
                         TURBO_MEDIA_OK);
            check_size_eq(turbo_media_source_track_count(source), 1);
            check_int_eq(turbo_media_source_get_track_at(source, 0, &track),
                         TURBO_MEDIA_OK);
            check_str_eq(track.codec_name, "h264");
            check_int_eq(track.payload_type, 102);

            memset(&player_config, 0, sizeof(player_config));
            player_config.protocol = TURBO_MEDIA_PROTOCOL_RTSP;
            player_config.role = TURBO_MEDIA_PROTOCOL_ROLE_PLAYER;
            player_config.key = key;
            player_config.callback = rtc_capture_frame;
            player_config.callback_user_data = &capture;
            check_int_eq(turbo_media_server_protocol_session_open(
                             runtime, &player_config, &player),
                         TURBO_MEDIA_OK);

            check_int_eq(rtc_fake_emit_rtp(idr_packet, sizeof(idr_packet)),
                         TURBO_MEDIA_OK);
            check_int_eq(capture.count, 1);
            check_int_eq(capture.track_id, 0);
            check_long_eq(capture.pts, 90000);
            check_true(capture.is_keyframe);

            check_int_eq(turbo_media_rtc_session_add_ice_candidate(
                             session, "candidate:remote"),
                         TURBO_MEDIA_OK);
            check_int_eq(rtc_fake.ice_count, 1);
            check_int_eq(turbo_media_rtc_session_pump(session), TURBO_MEDIA_OK);
            check_int_eq(rtc_fake.pump_count, 1);

            turbo_media_server_protocol_session_close(player);
            turbo_media_rtc_session_destroy(session);
            check_int_eq(rtc_fake.destroy_count, 1);
            check_int_eq(turbo_media_server_runtime_find_source(runtime, &key, &source),
                         TURBO_MEDIA_ERR_NOT_FOUND);
            turbo_media_server_runtime_destroy(runtime);
        }

        it("subscribes a WHEP player after the peer is connected") {
            turbo_media_server_runtime_t *runtime;
            turbo_media_protocol_session_t *publisher = NULL;
            turbo_media_protocol_session_config_t publisher_config;
            turbo_media_rtc_session_config_t config;
            turbo_media_rtc_session_t *session = NULL;
            turbo_media_track_info_t track = rtc_h264_track();
            turbo_media_frame_t frame;
            turbo_media_source_key_t key;
            rtc_event_capture_t events;
            char answer[64];
            uint8_t packet[] = {
                0x80, 0xE6, 0x00, 0x01,
                0x00, 0x01, 0x5F, 0x90,
                0x01, 0x02, 0x03, 0x04,
                0x65, 0x99
            };

            rtc_reset_fake(TURBO_MEDIA_RTC_ROLE_PLAYER);
            memset(&events, 0, sizeof(events));
            runtime = turbo_media_server_runtime_create(NULL);
            check_not_null(runtime);
            check_int_eq(turbo_media_source_key_init(&key, "default", "live", "cam"),
                         TURBO_MEDIA_OK);

            memset(&publisher_config, 0, sizeof(publisher_config));
            publisher_config.protocol = TURBO_MEDIA_PROTOCOL_RTSP;
            publisher_config.role = TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER;
            publisher_config.key = key;
            publisher_config.tracks = &track;
            publisher_config.track_count = 1;
            check_int_eq(turbo_media_server_protocol_session_open(
                             runtime, &publisher_config, &publisher),
                         TURBO_MEDIA_OK);

            memset(&frame, 0, sizeof(frame));
            frame.track_id = 0;
            frame.data = packet;
            frame.size = sizeof(packet);
            frame.pts = 90000;
            frame.dts = 90000;
            frame.is_keyframe = 1;
            check_int_eq(turbo_media_server_protocol_session_publish(publisher, &frame),
                         TURBO_MEDIA_OK);

            memset(&config, 0, sizeof(config));
            config.runtime = runtime;
            config.role = TURBO_MEDIA_RTC_ROLE_PLAYER;
            config.resource_path = "/live/cam";
            config.remote_offer_sdp = RTC_TEST_OFFER;
            config.replay_cached = 1;
            config.on_state = rtc_capture_state;
            config.user_data = &events;
            check_int_eq(turbo_media_rtc_session_create_with_backend(
                             &config, &rtc_fake_ops, answer, sizeof(answer),
                             NULL, &session),
                         TURBO_MEDIA_OK);
            check_int_eq(rtc_fake.add_send_track_count, 1);
            check_int_eq(rtc_fake.send_count, 0);

            rtc_fake.emit_connected = 1;
            check_int_eq(turbo_media_rtc_session_pump(session), TURBO_MEDIA_OK);
            check_int_eq(turbo_media_rtc_session_state(session),
                         TURBO_MEDIA_RTC_PEER_CONNECTED);
            check_int_eq(events.state_count, 1);
            check_int_eq(events.last_state, TURBO_MEDIA_RTC_PEER_CONNECTED);
            check_int_eq(rtc_fake.start_track_count, 1);
            check_int_eq(rtc_fake.send_count, 1);
            check_int_eq(rtc_fake.last_sent_track_id, 0);
            check_size_eq(rtc_fake.last_sent_size, sizeof(packet));
            check_int_eq(rtc_fake.last_sent_byte, 0x99);

            packet[sizeof(packet) - 1] = 0xAA;
            check_int_eq(turbo_media_server_protocol_session_publish(publisher, &frame),
                         TURBO_MEDIA_OK);
            check_int_eq(rtc_fake.send_count, 2);
            check_int_eq(rtc_fake.last_sent_byte, 0xAA);

            turbo_media_rtc_session_destroy(session);
            packet[sizeof(packet) - 1] = 0xBB;
            check_int_eq(turbo_media_server_protocol_session_publish(publisher, &frame),
                         TURBO_MEDIA_OK);
            check_int_eq(rtc_fake.send_count, 2);

            turbo_media_server_protocol_session_close(publisher);
            turbo_media_server_runtime_destroy(runtime);
        }

        it("propagates player RTP send failure to the publisher") {
            turbo_media_server_runtime_t *runtime;
            turbo_media_protocol_session_t *publisher = NULL;
            turbo_media_protocol_session_config_t publisher_config;
            turbo_media_rtc_session_config_t config;
            turbo_media_rtc_session_t *session = NULL;
            turbo_media_track_info_t track = rtc_h264_track();
            turbo_media_frame_t frame;
            turbo_media_source_key_t key;
            char answer[64];
            uint8_t packet[14] = {0x80, 0xE6};

            rtc_reset_fake(TURBO_MEDIA_RTC_ROLE_PLAYER);
            runtime = turbo_media_server_runtime_create(NULL);
            check_not_null(runtime);
            check_int_eq(turbo_media_source_key_init(&key, "default", "live", "cam"),
                         TURBO_MEDIA_OK);
            memset(&publisher_config, 0, sizeof(publisher_config));
            publisher_config.protocol = TURBO_MEDIA_PROTOCOL_RTSP;
            publisher_config.role = TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER;
            publisher_config.key = key;
            publisher_config.tracks = &track;
            publisher_config.track_count = 1;
            check_int_eq(turbo_media_server_protocol_session_open(
                             runtime, &publisher_config, &publisher),
                         TURBO_MEDIA_OK);

            memset(&config, 0, sizeof(config));
            config.runtime = runtime;
            config.role = TURBO_MEDIA_RTC_ROLE_PLAYER;
            config.resource_path = "/live/cam";
            config.remote_offer_sdp = RTC_TEST_OFFER;
            check_int_eq(turbo_media_rtc_session_create_with_backend(
                             &config, &rtc_fake_ops, answer, sizeof(answer),
                             NULL, &session),
                         TURBO_MEDIA_OK);
            rtc_fake.emit_connected = 1;
            check_int_eq(turbo_media_rtc_session_pump(session), TURBO_MEDIA_OK);

            memset(&frame, 0, sizeof(frame));
            frame.track_id = 0;
            frame.data = packet;
            frame.size = sizeof(packet);
            rtc_fake.fail_send = 1;
            check_int_eq(turbo_media_server_protocol_session_publish(publisher, &frame),
                         TURBO_MEDIA_ERR_STATE);
            check_int_eq(turbo_media_rtc_session_last_error(session),
                         TURBO_MEDIA_ERR_STATE);

            turbo_media_rtc_session_destroy(session);
            turbo_media_server_protocol_session_close(publisher);
            turbo_media_server_runtime_destroy(runtime);
        }
    }
}

#else

suite("turbo_media_rtc") {
    section("disabled") {
        it("is not built when RTC is disabled") {
            check_true(1);
        }
    }
}

#endif
