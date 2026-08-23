#include "turbo_media_server.h"
#include "CoroNet/turbo_coro_context.h"
#include <tinytest.h>

#include <stdint.h>
#include <string.h>

typedef struct {
    int count;
    int track_id[8];
    uint8_t first_byte[8];
    int64_t pts[8];
} runtime_capture_t;

static turbo_media_server_config_t server_config(void) {
    turbo_media_server_config_t config;
    memset(&config, 0, sizeof(config));
    config.max_sources = 4;
    config.source_config.max_tracks = 4;
    config.source_config.max_subscribers = 4;
    config.source_config.gop_capacity = 4;
    config.user_data = (void *)0x1234;
    return config;
}

static turbo_media_track_info_t h264_track(void) {
    turbo_media_track_info_t track;
    memset(&track, 0, sizeof(track));
    track.track_id = -1;
    track.type = TURBO_MEDIA_TRACK_VIDEO;
    strcpy(track.codec_name, "h264");
    track.payload_type = 96;
    track.clock_rate = 90000;
    track.width = 1280;
    track.height = 720;
    track.framerate = 25;
    return track;
}

static turbo_media_frame_t video_frame(int track_id,
                                       const uint8_t *data,
                                       size_t size,
                                       int64_t pts,
                                       int is_keyframe) {
    turbo_media_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.track_id = track_id;
    frame.data = data;
    frame.size = size;
    frame.pts = pts;
    frame.dts = pts;
    frame.duration = 40;
    frame.is_keyframe = is_keyframe;
    return frame;
}

static int runtime_capture_cb(turbo_media_source_t *source,
                              const turbo_media_frame_t *frame,
                              void *user_data) {
    runtime_capture_t *capture = (runtime_capture_t *)user_data;
    (void)source;

    if (capture->count >= 8) return TURBO_MEDIA_ERR_FULL;

    capture->track_id[capture->count] = frame->track_id;
    capture->first_byte[capture->count] = frame->size > 0 ? frame->data[0] : 0;
    capture->pts[capture->count] = frame->pts;
    capture->count++;
    return TURBO_MEDIA_OK;
}

suite("turbo_media_server_runtime") {
    group("lifecycle") {
        it("creates a server facade that owns runtime and default coro context") {
            turbo_media_server_config_t config = server_config();
            turbo_media_server_t *server = turbo_media_server_create(&config);
            turbo_media_server_runtime_t *runtime;

            check_not_null(server);
            runtime = turbo_media_server_get_runtime(server);
            check_not_null(runtime);

            check_equal((const void *)turbo_media_server_runtime_user_data(runtime),
                        (const void *)config.user_data);
            check_not_null(turbo_media_server_runtime_coro_context(runtime));

            check_equal(turbo_media_server_start(server), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_start(server), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_stop(server), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_stop(server), TURBO_MEDIA_OK);

            turbo_media_server_destroy(server);
        }

        it("reuses caller provided coro context without taking ownership") {
            turbo_media_server_config_t config = server_config();
            coro_context_t *coro_ctx = coro_context_create(NULL);
            turbo_media_server_t *server;
            turbo_media_server_runtime_t *runtime;

            check_not_null(coro_ctx);
            config.coro_context = (struct coro_context_s *)coro_ctx;
            server = turbo_media_server_create(&config);
            check_not_null(server);
            runtime = turbo_media_server_get_runtime(server);
            check_not_null(runtime);

            check_equal((const void *)turbo_media_server_runtime_coro_context(runtime),
                        (const void *)coro_ctx);
            check_equal(turbo_media_server_start(server), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_stop(server), TURBO_MEDIA_OK);

            turbo_media_server_destroy(server);
            coro_context_destroy(coro_ctx);
        }

        it("owns one media registry and keeps runtime context") {
            turbo_media_server_config_t config = server_config();
            turbo_media_source_key_t key;
            turbo_media_source_t *first = NULL;
            turbo_media_source_t *second = NULL;
            turbo_media_server_stats_t stats;

            check_equal(turbo_media_source_key_init(&key, "default", "live", "cam"), TURBO_MEDIA_OK);
            turbo_media_server_runtime_t *runtime = turbo_media_server_runtime_create(&config);
            check_not_null(runtime);

            check_equal((const void *)turbo_media_server_runtime_user_data(runtime),
                        (const void *)config.user_data);
            check_null(turbo_media_server_runtime_coro_context(runtime));

            check_equal(turbo_media_server_runtime_get_or_create_source(runtime, &key, &first), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_get_or_create_source(runtime, &key, &second), TURBO_MEDIA_OK);
            check_equal((const void *)first, (const void *)second);

            check_equal(turbo_media_server_runtime_get_stats(runtime, &stats), TURBO_MEDIA_OK);
            check_equal(stats.source_count, 1);

            turbo_media_server_runtime_destroy(runtime);
        }
    }

    group("protocol entry") {
        it("routes publisher and player protocol calls through the same source") {
            turbo_media_server_config_t config = server_config();
            turbo_media_source_key_t key;
            turbo_media_track_info_t track = h264_track();
            turbo_media_source_t *source = NULL;
            turbo_media_source_t *lookup = NULL;
            turbo_media_server_stats_t stats;
            runtime_capture_t capture;
            uint64_t subscription_id = 0;
            int track_id = -1;
            uint8_t keyframe[] = {0x17, 0x01};
            uint8_t delta[] = {0x27, 0x01};
            turbo_media_frame_t frame;

            memset(&capture, 0, sizeof(capture));
            check_equal(turbo_media_source_key_init(&key, "default", "live", "cam"), TURBO_MEDIA_OK);
            turbo_media_server_runtime_t *runtime = turbo_media_server_runtime_create(&config);
            check_not_null(runtime);

            check_equal(turbo_media_server_runtime_add_track(runtime, &key, &track, &track_id), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_find_source(runtime, &key, &source), TURBO_MEDIA_OK);

            check_equal(turbo_media_server_runtime_subscribe(runtime,
                                                            &key,
                                                            runtime_capture_cb,
                                                            &capture,
                                                            0,
                                                            &subscription_id), TURBO_MEDIA_OK);
            frame = video_frame(track_id, keyframe, sizeof(keyframe), 100, 1);
            check_equal(turbo_media_server_runtime_publish(runtime, &key, &frame), TURBO_MEDIA_OK);
            frame = video_frame(track_id, delta, sizeof(delta), 140, 0);
            check_equal(turbo_media_server_runtime_publish(runtime, &key, &frame), TURBO_MEDIA_OK);

            check_equal(capture.count, 2);
            check_equal(capture.first_byte[0], 0x17);
            check_equal(capture.first_byte[1], 0x27);

            check_equal(turbo_media_server_runtime_get_or_create_source(runtime, &key, &lookup), TURBO_MEDIA_OK);
            check_equal((const void *)lookup, (const void *)source);

            check_equal(turbo_media_server_runtime_unsubscribe(runtime, &key, subscription_id), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_get_stats(runtime, &stats), TURBO_MEDIA_OK);
            check_equal(stats.source_count, 1);
            check_equal(stats.tracks_registered, 1);
            check_equal(stats.frames_published, 2);
            check_equal(stats.subscriptions_created, 1);
            check_equal(stats.subscriptions_removed, 1);

            turbo_media_server_runtime_destroy(runtime);
        }

        it("replays cached GOP to a late player protocol entry") {
            turbo_media_server_config_t config = server_config();
            turbo_media_source_key_t key;
            turbo_media_track_info_t track = h264_track();
            runtime_capture_t replay;
            int track_id = -1;
            uint8_t prekey[] = {0x01};
            uint8_t keyframe[] = {0x02};
            uint8_t delta[] = {0x03};
            turbo_media_frame_t frame;

            memset(&replay, 0, sizeof(replay));
            check_equal(turbo_media_source_key_init(&key, "default", "live", "cam"), TURBO_MEDIA_OK);
            turbo_media_server_runtime_t *runtime = turbo_media_server_runtime_create(&config);
            check_not_null(runtime);

            check_equal(turbo_media_server_runtime_add_track(runtime, &key, &track, &track_id), TURBO_MEDIA_OK);
            frame = video_frame(track_id, prekey, sizeof(prekey), 10, 0);
            check_equal(turbo_media_server_runtime_publish(runtime, &key, &frame), TURBO_MEDIA_OK);
            frame = video_frame(track_id, keyframe, sizeof(keyframe), 20, 1);
            check_equal(turbo_media_server_runtime_publish(runtime, &key, &frame), TURBO_MEDIA_OK);
            frame = video_frame(track_id, delta, sizeof(delta), 60, 0);
            check_equal(turbo_media_server_runtime_publish(runtime, &key, &frame), TURBO_MEDIA_OK);

            check_equal(turbo_media_server_runtime_subscribe(runtime,
                                                            &key,
                                                            runtime_capture_cb,
                                                            &replay,
                                                            1,
                                                            NULL), TURBO_MEDIA_OK);
            check_equal(replay.count, 2);
            check_equal(replay.first_byte[0], 0x02);
            check_equal(replay.first_byte[1], 0x03);
            check_equal(replay.pts[1], 60);

            turbo_media_server_runtime_destroy(runtime);
        }

        it("rejects protocol publish and play before a source exists") {
            turbo_media_server_config_t config = server_config();
            turbo_media_source_key_t key;
            runtime_capture_t capture;
            uint8_t data[] = {0x01};
            turbo_media_frame_t frame = video_frame(0, data, sizeof(data), 1, 1);

            memset(&capture, 0, sizeof(capture));
            check_equal(turbo_media_source_key_init(&key, "default", "live", "missing"), TURBO_MEDIA_OK);
            turbo_media_server_runtime_t *runtime = turbo_media_server_runtime_create(&config);
            check_not_null(runtime);

            check_equal(turbo_media_server_runtime_publish(runtime, &key, &frame),
                         TURBO_MEDIA_ERR_NOT_FOUND);
            check_equal(turbo_media_server_runtime_subscribe(runtime,
                                                              &key,
                                                              runtime_capture_cb,
                                                              &capture,
                                                              0,
                                                              NULL),
                         TURBO_MEDIA_ERR_NOT_FOUND);

            turbo_media_server_runtime_destroy(runtime);
        }

        it("removes source state through the runtime") {
            turbo_media_server_config_t config = server_config();
            turbo_media_source_key_t key;
            turbo_media_track_info_t track = h264_track();
            turbo_media_server_stats_t stats;
            turbo_media_source_t *source = NULL;
            int track_id = -1;

            check_equal(turbo_media_source_key_init(&key, "default", "live", "remove"), TURBO_MEDIA_OK);
            turbo_media_server_runtime_t *runtime = turbo_media_server_runtime_create(&config);
            check_not_null(runtime);

            check_equal(turbo_media_server_runtime_add_track(runtime, &key, &track, &track_id), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_find_source(runtime, &key, &source), TURBO_MEDIA_OK);
            check_not_null(source);

            check_equal(turbo_media_server_runtime_remove_source(runtime, &key), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_find_source(runtime, &key, &source),
                         TURBO_MEDIA_ERR_NOT_FOUND);

            check_equal(turbo_media_server_runtime_get_stats(runtime, &stats), TURBO_MEDIA_OK);
            check_equal(stats.source_count, 0);
            check_equal(stats.sources_removed, 1);

            turbo_media_server_runtime_destroy(runtime);
        }

        it("uses protocol sessions as RTMP publish and RTSP play entry points") {
            turbo_media_server_config_t config = server_config();
            turbo_media_source_key_t key;
            turbo_media_track_info_t track = h264_track();
            turbo_media_protocol_session_config_t publish_config;
            turbo_media_protocol_session_config_t play_config;
            turbo_media_protocol_session_t *publisher = NULL;
            turbo_media_protocol_session_t *player = NULL;
            turbo_media_server_stats_t stats;
            runtime_capture_t capture;
            uint8_t keyframe[] = {0x17};
            turbo_media_frame_t frame;

            memset(&capture, 0, sizeof(capture));
            check_equal(turbo_media_source_key_init(&key, "default", "live", "session"), TURBO_MEDIA_OK);
            turbo_media_server_runtime_t *runtime = turbo_media_server_runtime_create(&config);
            check_not_null(runtime);

            memset(&publish_config, 0, sizeof(publish_config));
            publish_config.protocol = TURBO_MEDIA_PROTOCOL_RTMP;
            publish_config.role = TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER;
            publish_config.key = key;
            publish_config.tracks = &track;
            publish_config.track_count = 1;
            publish_config.remove_source_on_close = 1;
            check_equal(turbo_media_server_protocol_session_open(
                runtime,
                &publish_config,
                &publisher), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_protocol_session_protocol(publisher),
                         TURBO_MEDIA_PROTOCOL_RTMP);
            check_equal(turbo_media_server_protocol_session_role(publisher),
                         TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER);
            check_equal(turbo_media_server_protocol_session_key(publisher)->stream, "session");

            memset(&play_config, 0, sizeof(play_config));
            play_config.protocol = TURBO_MEDIA_PROTOCOL_RTSP;
            play_config.role = TURBO_MEDIA_PROTOCOL_ROLE_PLAYER;
            play_config.key = key;
            play_config.callback = runtime_capture_cb;
            play_config.callback_user_data = &capture;
            check_equal(turbo_media_server_protocol_session_open(runtime, &play_config, &player), TURBO_MEDIA_OK);

            frame = video_frame(0, keyframe, sizeof(keyframe), 100, 1);
            check_equal(turbo_media_server_protocol_session_publish(publisher, &frame), TURBO_MEDIA_OK);
            check_equal(capture.count, 1);
            check_equal(capture.first_byte[0], 0x17);
            check_equal(turbo_media_server_protocol_session_publish(player, &frame),
                         TURBO_MEDIA_ERR_STATE);

            /* Publisher-first teardown destroys the source and its subscription. */
            turbo_media_server_protocol_session_close(publisher);
            turbo_media_server_protocol_session_close(player);

            check_equal(turbo_media_server_runtime_get_stats(runtime, &stats), TURBO_MEDIA_OK);
            check_equal(stats.source_count, 0);
            check_equal(stats.frames_published, 1);
            check_equal(stats.subscriptions_created, 1);
            check_equal(stats.subscriptions_removed, 1);
            check_equal(stats.sources_removed, 1);

            turbo_media_server_runtime_destroy(runtime);
        }

        it("does not let a removed source subscription target its replacement") {
            turbo_media_server_config_t config = server_config();
            turbo_media_source_key_t key;
            turbo_media_track_info_t track = h264_track();
            turbo_media_server_stats_t stats;
            runtime_capture_t capture;
            uint64_t removed_subscription_id = 0;
            uint64_t replacement_subscription_id = 0;
            int track_id = -1;

            memset(&capture, 0, sizeof(capture));
            check_equal(turbo_media_source_key_init(&key, "default", "live", "replacement"), TURBO_MEDIA_OK);
            turbo_media_server_runtime_t *runtime = turbo_media_server_runtime_create(&config);
            check_not_null(runtime);

            check_equal(turbo_media_server_runtime_add_track(runtime, &key, &track, &track_id), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_subscribe(runtime,
                                                            &key,
                                                            runtime_capture_cb,
                                                            &capture,
                                                            0,
                                                            &removed_subscription_id), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_remove_source(runtime, &key), TURBO_MEDIA_OK);

            track_id = -1;
            check_equal(turbo_media_server_runtime_add_track(runtime, &key, &track, &track_id), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_runtime_subscribe(runtime,
                                                            &key,
                                                            runtime_capture_cb,
                                                            &capture,
                                                            0,
                                                            &replacement_subscription_id), TURBO_MEDIA_OK);
            check_true(removed_subscription_id != replacement_subscription_id);
            check_equal(turbo_media_server_runtime_unsubscribe(
                             runtime, &key, removed_subscription_id),
                         TURBO_MEDIA_ERR_NOT_FOUND);
            check_equal(turbo_media_server_runtime_unsubscribe(
                runtime, &key, replacement_subscription_id), TURBO_MEDIA_OK);

            check_equal(turbo_media_server_runtime_get_stats(runtime, &stats), TURBO_MEDIA_OK);
            check_equal(stats.subscriptions_created, 2);
            check_equal(stats.subscriptions_removed, 2);

            turbo_media_server_runtime_destroy(runtime);
        }

        it("rejects player protocol sessions before publish creates a source") {
            turbo_media_server_config_t config = server_config();
            turbo_media_source_key_t key;
            turbo_media_protocol_session_config_t play_config;
            turbo_media_protocol_session_t *player = NULL;
            runtime_capture_t capture;

            memset(&capture, 0, sizeof(capture));
            check_equal(turbo_media_source_key_init(&key, "default", "live", "late"), TURBO_MEDIA_OK);
            turbo_media_server_runtime_t *runtime = turbo_media_server_runtime_create(&config);
            check_not_null(runtime);

            memset(&play_config, 0, sizeof(play_config));
            play_config.protocol = TURBO_MEDIA_PROTOCOL_RTSP;
            play_config.role = TURBO_MEDIA_PROTOCOL_ROLE_PLAYER;
            play_config.key = key;
            play_config.callback = runtime_capture_cb;
            play_config.callback_user_data = &capture;
            check_equal(turbo_media_server_protocol_session_open(runtime, &play_config, &player),
                         TURBO_MEDIA_ERR_NOT_FOUND);
            check_null(player);

            turbo_media_server_runtime_destroy(runtime);
        }

        it("adapts RTMP publish and play callbacks to protocol sessions") {
            turbo_media_server_config_t config = server_config();
            turbo_media_protocol_session_t *publisher = NULL;
            turbo_media_protocol_session_t *player = NULL;
            turbo_media_source_key_t key;
            turbo_media_server_stats_t stats;
            runtime_capture_t capture;
            uint8_t video_tag[] = {0x17, 0x01, 0x00};
            uint8_t audio_tag[] = {0xaf, 0x01, 0x55};

            memset(&capture, 0, sizeof(capture));
            check_equal(turbo_media_server_rtmp_source_key(NULL, "live", "cam", &key), TURBO_MEDIA_OK);
            check_equal(key.vhost, "default");
            check_equal(key.app, "live");
            check_equal(key.stream, "cam");

            turbo_media_server_runtime_t *runtime = turbo_media_server_runtime_create(&config);
            check_not_null(runtime);

            check_equal(turbo_media_server_rtmp_open_publish(
                runtime,
                NULL,
                "live",
                "cam",
                1,
                &publisher), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_rtmp_open_play(
                runtime,
                NULL,
                "live",
                "cam",
                runtime_capture_cb,
                &capture,
                0,
                &player), TURBO_MEDIA_OK);

            check_equal(turbo_media_server_rtmp_publish_video(
                publisher,
                video_tag,
                sizeof(video_tag),
                40), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_rtmp_publish_audio(
                publisher,
                audio_tag,
                sizeof(audio_tag),
                42), TURBO_MEDIA_OK);

            check_equal(capture.count, 2);
            check_equal(capture.track_id[0], 0);
            check_equal(capture.first_byte[0], 0x17);
            check_equal(capture.track_id[1], 1);
            check_equal(capture.first_byte[1], 0xaf);
            check_equal(capture.pts[1], 42);

            turbo_media_server_protocol_session_close(player);
            turbo_media_server_protocol_session_close(publisher);
            check_equal(turbo_media_server_runtime_get_stats(runtime, &stats), TURBO_MEDIA_OK);
            check_equal(stats.source_count, 0);
            check_equal(stats.frames_published, 2);

            turbo_media_server_runtime_destroy(runtime);
        }

        it("adapts RTSP record and play callbacks to protocol sessions") {
            turbo_media_server_config_t config = server_config();
            turbo_media_protocol_session_t *recorder = NULL;
            turbo_media_protocol_session_t *player = NULL;
            turbo_media_source_key_t key;
            runtime_capture_t capture;
            uint8_t rtp_payload[] = {0x80, 0x60, 0x00, 0x01};

            memset(&capture, 0, sizeof(capture));
            check_equal(turbo_media_server_rtsp_source_key(
                NULL,
                "rtsp://127.0.0.1/live/cam",
                &key), TURBO_MEDIA_OK);
            check_equal(key.vhost, "default");
            check_equal(key.app, "live");
            check_equal(key.stream, "cam");

            turbo_media_server_runtime_t *runtime = turbo_media_server_runtime_create(&config);
            check_not_null(runtime);

            check_equal(turbo_media_server_rtsp_open_record(
                runtime,
                NULL,
                "rtsp://127.0.0.1/live/cam",
                2,
                1,
                &recorder), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_rtsp_open_play(
                runtime,
                NULL,
                "/live/cam",
                runtime_capture_cb,
                &capture,
                0,
                &player), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_rtsp_publish_interleaved(
                recorder,
                1,
                rtp_payload,
                sizeof(rtp_payload),
                90000), TURBO_MEDIA_OK);

            check_equal(capture.count, 1);
            check_equal(capture.track_id[0], 1);
            check_equal(capture.first_byte[0], 0x80);
            check_equal(capture.pts[0], 90000);

            turbo_media_server_protocol_session_close(player);
            turbo_media_server_protocol_session_close(recorder);
            turbo_media_server_runtime_destroy(runtime);
        }
    }
}
