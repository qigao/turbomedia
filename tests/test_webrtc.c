#include "turbo_media_webrtc.h"
#include "turbo_media_webrtc_backend.h"
#include "turbo_pipeline.h"
#include <tinytest.h>
#include <turbo_thread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TURBO_MEDIA_HAS_WEBRTC

#define CHECK_KEY(key, expected_vhost, expected_app, expected_stream) \
    do {                                                             \
        check_str_eq((key).vhost, (expected_vhost));                 \
        check_str_eq((key).app, (expected_app));                     \
        check_str_eq((key).stream, (expected_stream));               \
    } while (0)

#define RTC_TEST_ANSWER "v=0\r\n"
#define RTC_TEST_OFFER "v=0\r\n"
#define RTC_TEST_H264_FMTP \
    "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1"

typedef struct {
    int mode;
    int emit_connected;
    int fail_send;
    int create_count;
    int destroy_count;
    int add_send_track_count;
    int start_track_count;
    atomic_int send_count;
    int pump_count;
    int ice_count;
    int last_sent_track_id;
    size_t last_sent_size;
    uint8_t last_sent_byte;
} rtc_fake_state_t;

struct turbo_media_webrtc_backend_track_s {
    turbo_media_webrtc_backend_track_info_t info;
};

struct turbo_media_webrtc_backend_peer_s {
    turbo_media_webrtc_backend_config_t config;
    turbo_media_webrtc_backend_track_t tracks[8];
    size_t track_count;
};

static rtc_fake_state_t rtc_fake;
static turbo_media_webrtc_backend_peer_t *rtc_fake_peer;

static char *rtc_real_h264_offer(const char *direction) {
    static const char format[] =
        "v=0\r\n"
        "o=- 1 1 IN IP4 0.0.0.0\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "a=group:BUNDLE 0\r\n"
        "a=ice-options:trickle\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=mid:0\r\n"
        "a=%s\r\n"
        "a=rtcp-mux\r\n"
        "a=ice-ufrag:remote-test\r\n"
        "a=ice-pwd:remote-test-password\r\n"
        "a=fingerprint:sha-256 "
        "00:01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:"
        "10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F\r\n"
        "a=setup:actpass\r\n"
        "a=rtpmap:102 H264/90000\r\n"
        "a=fmtp:102 " RTC_TEST_H264_FMTP "\r\n"
        "a=ssrc:270544960 cname:turbo-test\r\n";
    char *offer;
    int required;

    if (!direction) return NULL;
    required = snprintf(NULL, 0, format, direction);
    if (required < 0) return NULL;
    offer = (char *)malloc((size_t)required + 1);
    if (!offer) return NULL;
    if (snprintf(offer, (size_t)required + 1, format, direction) != required) {
        free(offer);
        return NULL;
    }
    return offer;
}

static turbo_media_webrtc_backend_peer_t *rtc_fake_create(
    const turbo_media_webrtc_backend_config_t *config) {
    turbo_media_webrtc_backend_peer_t *peer;

    if (!config) return NULL;
    peer = (turbo_media_webrtc_backend_peer_t *)calloc(1, sizeof(*peer));
    if (!peer) return NULL;
    peer->config = *config;
    rtc_fake.create_count++;
    rtc_fake_peer = peer;
    return peer;
}

static void rtc_fake_destroy(turbo_media_webrtc_backend_peer_t *peer) {
    if (!peer) return;
    rtc_fake.destroy_count++;
    if (rtc_fake_peer == peer) rtc_fake_peer = NULL;
    free(peer);
}

static int rtc_fake_add_send_track(
    turbo_media_webrtc_backend_peer_t *peer,
    const turbo_media_webrtc_backend_track_info_t *info,
    turbo_media_webrtc_backend_track_t **track) {
    if (!peer || !info || !track || peer->track_count >= 8) return -1;
    peer->tracks[peer->track_count].info = *info;
    *track = &peer->tracks[peer->track_count++];
    rtc_fake.add_send_track_count++;
    return 0;
}

static int rtc_fake_set_remote_offer(turbo_media_webrtc_backend_peer_t *peer,
                                     const char *offer_sdp) {
    turbo_media_webrtc_backend_track_t *track;

    if (!peer || !offer_sdp || strcmp(offer_sdp, RTC_TEST_OFFER) != 0) return -1;
    if (rtc_fake.mode != TURBO_MEDIA_WEBRTC_ROLE_PUBLISHER) return 0;
    if (peer->track_count >= 8) return -1;

    track = &peer->tracks[peer->track_count++];
    memset(track, 0, sizeof(*track));
    track->info.track_id = 0;
    track->info.type = TURBO_MEDIA_WEBRTC_BACKEND_TRACK_VIDEO;
    strcpy(track->info.codec_name, "h264");
    track->info.payload_type = 102;
    track->info.clock_rate = 90000;
    return peer->config.on_remote_track(
        peer->config.user_data, track, &track->info);
}

static int rtc_fake_create_answer(turbo_media_webrtc_backend_peer_t *peer,
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

static int rtc_fake_add_ice_candidate(turbo_media_webrtc_backend_peer_t *peer,
                                      const char *candidate) {
    if (!peer || !candidate || strcmp(candidate, "candidate:remote") != 0) return -1;
    rtc_fake.ice_count++;
    return 0;
}

static int rtc_fake_start_track(turbo_media_webrtc_backend_track_t *track) {
    if (!track) return -1;
    rtc_fake.start_track_count++;
    return 0;
}

static int rtc_fake_send_rtp(turbo_media_webrtc_backend_track_t *track,
                             const uint8_t *packet,
                             size_t packet_len) {
    if (!track || !packet || packet_len == 0 || rtc_fake.fail_send) return -1;
    rtc_fake.send_count++;
    rtc_fake.last_sent_track_id = track->info.track_id;
    rtc_fake.last_sent_size = packet_len;
    rtc_fake.last_sent_byte = packet[packet_len - 1];
    return 0;
}

static int rtc_fake_pump(turbo_media_webrtc_backend_peer_t *peer) {
    if (!peer) return -1;
    rtc_fake.pump_count++;
    if (rtc_fake.emit_connected) {
        rtc_fake.emit_connected = 0;
        peer->config.on_state(
            peer->config.user_data, TURBO_MEDIA_WEBRTC_PEER_CONNECTED);
    }
    return 0;
}

static const turbo_media_webrtc_backend_ops_t rtc_fake_ops = {
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
    turbo_media_webrtc_peer_state_t last_state;
} rtc_event_capture_t;

static void rtc_capture_ice(turbo_media_webrtc_session_t *session,
                            const char *candidate,
                            void *user_data) {
    rtc_event_capture_t *capture = (rtc_event_capture_t *)user_data;

    check_not_null(session);
    check_str_eq(candidate, "candidate:test");
    capture->ice_count++;
}

static void rtc_capture_state(turbo_media_webrtc_session_t *session,
                              turbo_media_webrtc_peer_state_t state,
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

static void rtc_reset_fake(turbo_media_webrtc_role_t mode) {
    memset(&rtc_fake, 0, sizeof(rtc_fake));
    atomic_init(&rtc_fake.send_count, 0);
    rtc_fake.mode = mode;
    rtc_fake.last_sent_track_id = -1;
    rtc_fake_peer = NULL;
}

typedef struct {
    turbo_pipeline_t *pipeline;
    turbo_pipeline_status_t status;
    turbo_pipeline_error_t error;
} rtc_pipeline_run_t;

static void rtc_run_pipeline(void *parameter) {
    rtc_pipeline_run_t *run = (rtc_pipeline_run_t *)parameter;
    run->status = turbo_pipeline_run(run->pipeline, &run->error);
}

suite("turbo_media_webrtc") {
    before_each() {
        rtc_reset_fake(TURBO_MEDIA_WEBRTC_ROLE_PUBLISHER);
    }

    section("source keys") {
        it("maps WHIP resource path to the shared source key") {
            turbo_media_source_key_t key;

            memset(&key, 0, sizeof(key));
            check_int_eq(
                turbo_media_webrtc_source_key("default", "/live/cam", NULL, &key),
                TURBO_MEDIA_OK);
            CHECK_KEY(key, "default", "live", "cam");
        }

        it("uses query app and stream before path segments") {
            turbo_media_source_key_t key;

            memset(&key, 0, sizeof(key));
            check_int_eq(
                turbo_media_webrtc_source_key(
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
                turbo_media_webrtc_source_key("default", "/live/cam", "app=live", &key),
                TURBO_MEDIA_ERR_INVALID);
        }

        it("rejects paths without app and stream") {
            turbo_media_source_key_t key;

            memset(&key, 0, sizeof(key));
            check_int_eq(
                turbo_media_webrtc_source_key("default", "/live", NULL, &key),
                TURBO_MEDIA_ERR_INVALID);
        }
    }

    section("session bridge") {
        it("publishes remote WebRTC RTP through ServerRuntime") {
            turbo_media_server_runtime_t *runtime;
            turbo_media_webrtc_session_config_t config;
            turbo_media_webrtc_session_t *session = NULL;
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
            config.role = TURBO_MEDIA_WEBRTC_ROLE_PUBLISHER;
            config.resource_path = "/live/cam";
            config.remote_offer_sdp = RTC_TEST_OFFER;
            config.remove_source_on_close = 1;
            config.on_ice_candidate = rtc_capture_ice;
            config.on_state = rtc_capture_state;
            config.user_data = &events;
            check_int_eq(turbo_media_webrtc_session_create_with_backend(
                             &config, &rtc_fake_ops, answer, sizeof(answer),
                             &answer_length, &session),
                         TURBO_MEDIA_OK);
            check_not_null(session);
            check_str_eq(answer, RTC_TEST_ANSWER);
            check_size_eq(answer_length, strlen(RTC_TEST_ANSWER));
            check_int_eq(events.ice_count, 1);

            key = *turbo_media_webrtc_session_key(session);
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

            check_int_eq(turbo_media_webrtc_session_add_ice_candidate(
                             session, "candidate:remote"),
                         TURBO_MEDIA_OK);
            check_int_eq(rtc_fake.ice_count, 1);
            check_int_eq(turbo_media_webrtc_session_pump(session), TURBO_MEDIA_OK);
            check_int_eq(rtc_fake.pump_count, 1);

            turbo_media_server_protocol_session_close(player);
            turbo_media_webrtc_session_destroy(session);
            check_int_eq(rtc_fake.destroy_count, 1);
            check_int_eq(turbo_media_server_runtime_find_source(runtime, &key, &source),
                         TURBO_MEDIA_ERR_NOT_FOUND);
            turbo_media_server_runtime_destroy(runtime);
        }

        it("subscribes a WHEP player after the peer is connected") {
            turbo_media_server_runtime_t *runtime;
            turbo_media_protocol_session_t *publisher = NULL;
            turbo_media_protocol_session_config_t publisher_config;
            turbo_media_webrtc_session_config_t config;
            turbo_media_webrtc_session_t *session = NULL;
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

            rtc_reset_fake(TURBO_MEDIA_WEBRTC_ROLE_PLAYER);
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
            config.role = TURBO_MEDIA_WEBRTC_ROLE_PLAYER;
            config.resource_path = "/live/cam";
            config.remote_offer_sdp = RTC_TEST_OFFER;
            config.replay_cached = 1;
            config.on_state = rtc_capture_state;
            config.user_data = &events;
            check_int_eq(turbo_media_webrtc_session_create_with_backend(
                             &config, &rtc_fake_ops, answer, sizeof(answer),
                             NULL, &session),
                         TURBO_MEDIA_OK);
            check_int_eq(rtc_fake.add_send_track_count, 1);
            check_int_eq(rtc_fake.send_count, 0);

            rtc_fake.emit_connected = 1;
            check_int_eq(turbo_media_webrtc_session_pump(session), TURBO_MEDIA_OK);
            check_int_eq(turbo_media_webrtc_session_state(session),
                         TURBO_MEDIA_WEBRTC_PEER_CONNECTED);
            check_int_eq(events.state_count, 1);
            check_int_eq(events.last_state, TURBO_MEDIA_WEBRTC_PEER_CONNECTED);
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

            turbo_media_webrtc_session_destroy(session);
            packet[sizeof(packet) - 1] = 0xBB;
            check_int_eq(turbo_media_server_protocol_session_publish(publisher, &frame),
                         TURBO_MEDIA_OK);
            check_int_eq(rtc_fake.send_count, 2);

            turbo_media_server_protocol_session_close(publisher);
            turbo_media_server_runtime_destroy(runtime);
        }

        it("relays fake WHIP RTP through Pipeline to a fake WHEP player") {
            static const char pipeline_yaml[] =
                "api_version: turbo.media.pipeline/v1\n"
                "id: whip-to-whep\n"
                "nodes:\n"
                "  - { id: source, kind: source, factory: media.runtime_source, config: { url: default/live/whip } }\n"
                "  - { id: depay, kind: demux, factory: rtp.depacketize }\n"
                "  - { id: pay, kind: mux, factory: rtp.packetize }\n"
                "  - { id: sink, kind: sink, factory: media.runtime_sink, config: { url: default/live/whep } }\n"
                "edges:\n"
                "  - { from: source.out, to: depay.in }\n"
                "  - { from: depay.video, to: pay.video }\n"
                "  - { from: pay.out, to: sink.in }\n";
            turbo_media_server_runtime_t *runtime = NULL;
            turbo_media_webrtc_session_t *whip = NULL;
            turbo_media_webrtc_session_t *whep = NULL;
            turbo_media_webrtc_backend_peer_t *whip_peer = NULL;
            turbo_media_webrtc_session_config_t config;
            turbo_pipeline_t *pipeline = NULL;
            turbo_pipeline_error_t pipeline_error;
            rtc_pipeline_run_t run;
            turbo_thread_t thread = NULL;
            char answer[64];
            int wait_count;
            uint8_t idr_packet[] = {
                0x80, 0xE6, 0x00, 0x2A,
                0x00, 0x01, 0x5F, 0x90,
                0x01, 0x02, 0x03, 0x04,
                0x65, 0x88
            };

            memset(&run, 0, sizeof(run));
            runtime = turbo_media_server_runtime_create(NULL);
            check_not_null(runtime);
            if (!runtime) goto rtc_pipeline_cleanup;

            memset(&config, 0, sizeof(config));
            config.runtime = runtime;
            config.role = TURBO_MEDIA_WEBRTC_ROLE_PUBLISHER;
            config.resource_path = "/live/whip";
            config.remote_offer_sdp = RTC_TEST_OFFER;
            check_int_eq(turbo_media_webrtc_session_create_with_backend(
                             &config, &rtc_fake_ops, answer, sizeof(answer),
                             NULL, &whip),
                         TURBO_MEDIA_OK);
            whip_peer = rtc_fake_peer;
            check_not_null(whip_peer);
            if (!whip_peer) goto rtc_pipeline_cleanup;

            pipeline = turbo_pipeline_create_from_yaml(
                pipeline_yaml, sizeof(pipeline_yaml) - 1u, &pipeline_error);
            if (!pipeline)
                fprintf(stderr, "WHIP/WHEP pipeline config error: %s\n",
                        pipeline_error.message);
            check_not_null(pipeline);
            if (!pipeline) goto rtc_pipeline_cleanup;
            check_int_eq(turbo_pipeline_bind_server_runtime(
                             pipeline, runtime, &pipeline_error),
                         TURBO_PIPELINE_OK);
            check_int_eq(turbo_pipeline_prepare(pipeline, &pipeline_error),
                         TURBO_PIPELINE_OK);
            if (turbo_pipeline_state(pipeline) != TURBO_PIPELINE_STATE_PREPARED)
                goto rtc_pipeline_cleanup;

            rtc_reset_fake(TURBO_MEDIA_WEBRTC_ROLE_PLAYER);
            memset(&config, 0, sizeof(config));
            config.runtime = runtime;
            config.role = TURBO_MEDIA_WEBRTC_ROLE_PLAYER;
            config.resource_path = "/live/whep";
            config.remote_offer_sdp = RTC_TEST_OFFER;
            check_int_eq(turbo_media_webrtc_session_create_with_backend(
                             &config, &rtc_fake_ops, answer, sizeof(answer),
                             NULL, &whep),
                         TURBO_MEDIA_OK);
            rtc_fake.emit_connected = 1;
            check_int_eq(turbo_media_webrtc_session_pump(whep), TURBO_MEDIA_OK);
            check_int_eq(turbo_media_webrtc_session_state(whep),
                         TURBO_MEDIA_WEBRTC_PEER_CONNECTED);

            run.pipeline = pipeline;
            check_int_eq(turbo_thread_create(&thread, rtc_run_pipeline, &run), 0);
            for (wait_count = 0; wait_count < 100 &&
                                 turbo_pipeline_state(pipeline) !=
                                     TURBO_PIPELINE_STATE_RUNNING;
                 ++wait_count)
                turbo_sleep_ms(1);
            check_int_eq(turbo_pipeline_state(pipeline),
                         TURBO_PIPELINE_STATE_RUNNING);
            check_int_eq(whip_peer->config.on_rtp(
                             whip_peer->config.user_data,
                             &whip_peer->tracks[0], idr_packet,
                             sizeof(idr_packet)),
                         TURBO_MEDIA_OK);
            for (wait_count = 0;
                 wait_count < 1000 &&
                 atomic_load_explicit(&rtc_fake.send_count,
                                      memory_order_acquire) < 1;
                 ++wait_count)
                turbo_sleep_ms(1);
            check_int_eq(atomic_load_explicit(&rtc_fake.send_count,
                                              memory_order_acquire),
                         1);
            check_int_eq(rtc_fake.last_sent_track_id, 0);
            check_size_eq(rtc_fake.last_sent_size, sizeof(idr_packet));
            check_int_eq(rtc_fake.last_sent_byte, 0x88);

            check_int_eq(turbo_pipeline_request_stop(pipeline),
                         TURBO_PIPELINE_OK);
            check_int_eq(turbo_thread_join(&thread), 0);
            turbo_thread_destroy(&thread);
            thread = NULL;
            check_int_eq(run.status, TURBO_PIPELINE_ESTOPPED);

        rtc_pipeline_cleanup:
            if (thread) {
                (void)turbo_pipeline_request_stop(pipeline);
                (void)turbo_thread_join(&thread);
                turbo_thread_destroy(&thread);
            }
            turbo_media_webrtc_session_destroy(whep);
            turbo_pipeline_destroy(pipeline);
            turbo_media_webrtc_session_destroy(whip);
            turbo_media_server_runtime_destroy(runtime);
        }

        it("propagates player RTP send failure to the publisher") {
            turbo_media_server_runtime_t *runtime;
            turbo_media_protocol_session_t *publisher = NULL;
            turbo_media_protocol_session_config_t publisher_config;
            turbo_media_webrtc_session_config_t config;
            turbo_media_webrtc_session_t *session = NULL;
            turbo_media_track_info_t track = rtc_h264_track();
            turbo_media_frame_t frame;
            turbo_media_source_key_t key;
            char answer[64];
            uint8_t packet[14] = {0x80, 0xE6};

            rtc_reset_fake(TURBO_MEDIA_WEBRTC_ROLE_PLAYER);
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
            config.role = TURBO_MEDIA_WEBRTC_ROLE_PLAYER;
            config.resource_path = "/live/cam";
            config.remote_offer_sdp = RTC_TEST_OFFER;
            check_int_eq(turbo_media_webrtc_session_create_with_backend(
                             &config, &rtc_fake_ops, answer, sizeof(answer),
                             NULL, &session),
                         TURBO_MEDIA_OK);
            rtc_fake.emit_connected = 1;
            check_int_eq(turbo_media_webrtc_session_pump(session), TURBO_MEDIA_OK);

            memset(&frame, 0, sizeof(frame));
            frame.track_id = 0;
            frame.data = packet;
            frame.size = sizeof(packet);
            rtc_fake.fail_send = 1;
            check_int_eq(turbo_media_server_protocol_session_publish(publisher, &frame),
                         TURBO_MEDIA_ERR_STATE);
            check_int_eq(turbo_media_webrtc_session_last_error(session),
                         TURBO_MEDIA_ERR_STATE);

            turbo_media_webrtc_session_destroy(session);
            turbo_media_server_protocol_session_close(publisher);
            turbo_media_server_runtime_destroy(runtime);
        }

        it("creates an answer with the built-in PeerConnection backend") {
            turbo_media_server_runtime_t *runtime = NULL;
            turbo_media_webrtc_session_t *session = NULL;
            turbo_media_webrtc_session_config_t config;
            turbo_media_source_t *source = NULL;
            turbo_media_track_info_t track;
            turbo_media_source_key_t key;
            char answer[32768];
            char *offer = NULL;
            size_t answer_length = 0;
            int attempt;

            offer = rtc_real_h264_offer("sendonly");
            check_not_null(offer);
            runtime = turbo_media_server_runtime_create(NULL);
            check_not_null(runtime);

            memset(&config, 0, sizeof(config));
            config.runtime = runtime;
            config.role = TURBO_MEDIA_WEBRTC_ROLE_PUBLISHER;
            config.resource_path = "/live/production";
            config.remote_offer_sdp = offer;
            config.allow_loopback = 1;
            check_int_eq(turbo_media_webrtc_session_create(
                             &config, answer, sizeof(answer), &answer_length,
                             &session),
                         TURBO_MEDIA_OK);
            check_not_null(session);
            check_size_gt(answer_length, 0);
            check_str_contains(answer, "a=mid:0");

            check_int_eq(turbo_media_source_key_init(
                             &key, "default", "live", "production"),
                         TURBO_MEDIA_OK);
            for (attempt = 0; attempt < 100; ++attempt) {
                check_int_eq(turbo_media_webrtc_session_pump(session),
                             TURBO_MEDIA_OK);
                if (turbo_media_server_runtime_find_source(
                        runtime, &key, &source) == TURBO_MEDIA_OK &&
                    turbo_media_source_track_count(source) == 1) {
                    break;
                }
                turbo_sleep_ms(1);
            }
            check_not_null(source);
            check_size_eq(turbo_media_source_track_count(source), 1);
            check_int_eq(turbo_media_source_get_track_at(source, 0, &track),
                         TURBO_MEDIA_OK);
            check_str_eq(track.codec_name, "H264");
            check_int_eq(track.payload_type, 102);
            check_int_eq(track.clock_rate, 90000);

            turbo_media_webrtc_session_destroy(session);
            turbo_media_server_runtime_destroy(runtime);
            free(offer);
        }

        it("maps a WHEP offer through the built-in PeerConnection backend") {
            turbo_media_server_runtime_t *runtime = NULL;
            turbo_media_protocol_session_t *publisher = NULL;
            turbo_media_protocol_session_config_t publisher_config;
            turbo_media_webrtc_session_t *session = NULL;
            turbo_media_webrtc_session_config_t config;
            turbo_media_track_info_t track = rtc_h264_track();
            turbo_media_source_key_t key;
            char answer[32768];
            char *offer = NULL;
            size_t answer_length = 0;

            offer = rtc_real_h264_offer("recvonly");
            check_not_null(offer);
            track.payload_type = 96;
            runtime = turbo_media_server_runtime_create(NULL);
            check_not_null(runtime);
            check_int_eq(turbo_media_source_key_init(
                             &key, "default", "live", "whep-production"),
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
            config.role = TURBO_MEDIA_WEBRTC_ROLE_PLAYER;
            config.resource_path = "/live/whep-production";
            config.remote_offer_sdp = offer;
            config.allow_loopback = 1;
            check_int_eq(turbo_media_webrtc_session_create(
                             &config, answer, sizeof(answer), &answer_length,
                             &session),
                         TURBO_MEDIA_OK);
            check_not_null(session);
            check_size_gt(answer_length, 0);
            check_str_contains(answer, "a=sendonly");
            check_str_contains(answer, "a=mid:0");
            check_str_contains(answer, "a=rtpmap:102 H264/90000");
            check_str_contains(answer, "a=fmtp:102 ");
            check_str_contains(answer, "profile-level-id=42e01f");
            check_str_contains(answer, "packetization-mode=1");
            check_str_contains(answer, "level-asymmetry-allowed=1");

            turbo_media_webrtc_session_destroy(session);
            turbo_media_server_protocol_session_close(publisher);
            turbo_media_server_runtime_destroy(runtime);
            free(offer);
        }
    }
}

#else

suite("turbo_media_webrtc") {
    section("disabled") {
        it("is not built when RTC is disabled") {
            check_true(1);
        }
    }
}

#endif
