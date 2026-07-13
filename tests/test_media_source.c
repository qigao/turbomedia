#include "turbo_media_source.h"
#include <tinytest.h>

#include <stdint.h>
#include <string.h>

#define REQUIRE_OK(expr)                    \
    do {                                    \
        int rc__ = (expr);                  \
        check_int_eq(rc__, TURBO_MEDIA_OK); \
        if (rc__ != TURBO_MEDIA_OK) return; \
    } while (0)

#define REQUIRE_NOT_NULL(expr) \
    do {                       \
        check_not_null(expr);  \
        if (!(expr)) return;   \
    } while (0)

typedef struct {
    int count;
    uint8_t first_byte[8];
    int keyframe[8];
    int64_t pts[8];
} frame_capture_t;

static turbo_media_source_config_t small_source_config(void) {
    turbo_media_source_config_t config;
    memset(&config, 0, sizeof(config));
    config.max_tracks = 4;
    config.max_subscribers = 4;
    config.gop_capacity = 4;
    return config;
}

static turbo_media_track_info_t video_track(void) {
    turbo_media_track_info_t track;
    memset(&track, 0, sizeof(track));
    track.track_id = -1;
    track.type = TURBO_MEDIA_TRACK_VIDEO;
    strcpy(track.codec_name, "h264");
    track.payload_type = 96;
    track.clock_rate = 90000;
    track.width = 1920;
    track.height = 1080;
    track.framerate = 30;
    return track;
}

static turbo_media_track_info_t audio_track(void) {
    turbo_media_track_info_t track;
    memset(&track, 0, sizeof(track));
    track.track_id = -1;
    track.type = TURBO_MEDIA_TRACK_AUDIO;
    strcpy(track.codec_name, "pcmu");
    track.payload_type = 0;
    track.clock_rate = 8000;
    track.sample_rate = 8000;
    track.channels = 1;
    return track;
}

static turbo_media_frame_t media_frame(int track_id,
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
    frame.duration = 33;
    frame.is_keyframe = is_keyframe;
    return frame;
}

static int capture_frame_cb(turbo_media_source_t *source,
                            const turbo_media_frame_t *frame,
                            void *user_data) {
    frame_capture_t *capture = (frame_capture_t *)user_data;
    (void)source;

    if (capture->count >= 8) return TURBO_MEDIA_ERR_FULL;

    capture->first_byte[capture->count] = frame->size > 0 ? frame->data[0] : 0;
    capture->keyframe[capture->count] = frame->is_keyframe;
    capture->pts[capture->count] = frame->pts;
    capture->count++;
    return TURBO_MEDIA_OK;
}

static int failing_frame_cb(turbo_media_source_t *source,
                            const turbo_media_frame_t *frame,
                            void *user_data) {
    (void)source;
    (void)frame;
    (void)user_data;
    return -77;
}

suite("turbo_media_source") {
    section("source identity and tracks") {
        it("creates a typed stream key and rejects invalid identity") {
            turbo_media_source_key_t key;
            char too_long[TURBO_MEDIA_MAX_STREAM_LEN + 1];
            memset(too_long, 'x', sizeof(too_long));
            too_long[sizeof(too_long) - 1] = '\0';

            REQUIRE_OK(turbo_media_source_key_init(&key, "default", "live", "camera"));
            check_str_eq(key.vhost, "default");
            check_str_eq(key.app, "live");
            check_str_eq(key.stream, "camera");
            check_int_eq(turbo_media_source_key_init(&key, "default", "live", too_long),
                         TURBO_MEDIA_ERR_INVALID);
        }

        it("owns track metadata and extradata") {
            turbo_media_source_key_t key;
            turbo_media_source_config_t config = small_source_config();
            turbo_media_track_info_t track = video_track();
            turbo_media_track_info_t copied;
            uint8_t extradata[] = {0x01, 0x64, 0x00, 0x1f};
            int track_id = -1;

            REQUIRE_OK(turbo_media_source_key_init(&key, "default", "live", "camera"));
            turbo_media_source_t *source = turbo_media_source_create(&key, &config);
            REQUIRE_NOT_NULL(source);

            track.extradata = extradata;
            track.extradata_size = sizeof(extradata);
            REQUIRE_OK(turbo_media_source_add_track(source, &track, &track_id));
            check_int_eq(track_id, 0);
            check_size_eq(turbo_media_source_track_count(source), 1);

            extradata[1] = 0xff;
            memset(&copied, 0, sizeof(copied));
            REQUIRE_OK(turbo_media_source_get_track(source, track_id, &copied));
            check_str_eq(copied.codec_name, "h264");
            check_int_eq(copied.type, TURBO_MEDIA_TRACK_VIDEO);
            check_size_eq(copied.extradata_size, 4);
            check(copied.extradata != extradata);
            check_int_eq(copied.extradata[1], 0x64);

            turbo_media_source_destroy(source);
        }
    }

    section("publish and subscribe") {
        it("fanouts published frames through the source") {
            turbo_media_source_key_t key;
            turbo_media_source_config_t config = small_source_config();
            turbo_media_track_info_t track = audio_track();
            turbo_media_source_stats_t stats;
            frame_capture_t capture;
            uint64_t subscription_id = 0;
            int track_id = -1;
            uint8_t data[] = {0xaa, 0xbb, 0xcc};
            turbo_media_frame_t frame;

            memset(&capture, 0, sizeof(capture));
            REQUIRE_OK(turbo_media_source_key_init(&key, "default", "live", "audio"));
            turbo_media_source_t *source = turbo_media_source_create(&key, &config);
            REQUIRE_NOT_NULL(source);

            REQUIRE_OK(turbo_media_source_add_track(source, &track, &track_id));
            REQUIRE_OK(turbo_media_source_subscribe(source,
                                                    capture_frame_cb,
                                                    &capture,
                                                    0,
                                                    &subscription_id));
            frame = media_frame(track_id, data, sizeof(data), 10, 0);
            REQUIRE_OK(turbo_media_source_publish(source, &frame));

            check_int_eq(capture.count, 1);
            check_int_eq(capture.first_byte[0], 0xaa);
            check_int_eq(turbo_media_source_state(source), TURBO_MEDIA_SOURCE_PUBLISHING);

            REQUIRE_OK(turbo_media_source_get_stats(source, &stats));
            check_uint_eq(stats.frames_published, 1);
            check_uint_eq(stats.bytes_published, sizeof(data));
            check_uint_eq(stats.frames_delivered, 1);
            check_uint_eq(stats.subscriber_count, 1);

            REQUIRE_OK(turbo_media_source_unsubscribe(source, subscription_id));
            REQUIRE_OK(turbo_media_source_get_stats(source, &stats));
            check_uint_eq(stats.subscriber_count, 0);

            turbo_media_source_destroy(source);
        }

        it("replays cached GOP from the latest video keyframe") {
            turbo_media_source_key_t key;
            turbo_media_source_config_t config = small_source_config();
            turbo_media_track_info_t track = video_track();
            frame_capture_t replay;
            int track_id = -1;
            uint8_t prekey[] = {0x10};
            uint8_t keyframe[] = {0x20};
            uint8_t delta[] = {0x30};
            turbo_media_frame_t frame;

            memset(&replay, 0, sizeof(replay));
            REQUIRE_OK(turbo_media_source_key_init(&key, "default", "live", "video"));
            turbo_media_source_t *source = turbo_media_source_create(&key, &config);
            REQUIRE_NOT_NULL(source);

            REQUIRE_OK(turbo_media_source_add_track(source, &track, &track_id));
            frame = media_frame(track_id, prekey, sizeof(prekey), 10, 0);
            REQUIRE_OK(turbo_media_source_publish(source, &frame));
            frame = media_frame(track_id, keyframe, sizeof(keyframe), 20, 1);
            REQUIRE_OK(turbo_media_source_publish(source, &frame));
            frame = media_frame(track_id, delta, sizeof(delta), 30, 0);
            REQUIRE_OK(turbo_media_source_publish(source, &frame));

            REQUIRE_OK(turbo_media_source_subscribe(source, capture_frame_cb, &replay, 1, NULL));
            check_int_eq(replay.count, 2);
            check_int_eq(replay.first_byte[0], 0x20);
            check_int_eq(replay.keyframe[0], 1);
            check_int_eq(replay.first_byte[1], 0x30);
            check_int_eq(replay.pts[1], 30);

            turbo_media_source_destroy(source);
        }

        it("does not leave a subscriber when cached replay fails") {
            turbo_media_source_key_t key;
            turbo_media_source_config_t config = small_source_config();
            turbo_media_track_info_t track = audio_track();
            turbo_media_source_stats_t stats;
            int track_id = -1;
            uint8_t data[] = {0x44};
            turbo_media_frame_t frame;

            REQUIRE_OK(turbo_media_source_key_init(&key, "default", "live", "fail"));
            turbo_media_source_t *source = turbo_media_source_create(&key, &config);
            REQUIRE_NOT_NULL(source);

            REQUIRE_OK(turbo_media_source_add_track(source, &track, &track_id));
            frame = media_frame(track_id, data, sizeof(data), 1, 0);
            REQUIRE_OK(turbo_media_source_publish(source, &frame));
            check_int_eq(turbo_media_source_subscribe(source, failing_frame_cb, NULL, 1, NULL), -77);

            REQUIRE_OK(turbo_media_source_get_stats(source, &stats));
            check_uint_eq(stats.subscriber_count, 0);

            turbo_media_source_destroy(source);
        }
    }

    section("registry") {
        it("keeps one source per stream key") {
            turbo_media_source_key_t key;
            turbo_media_source_config_t config = small_source_config();
            turbo_media_source_t *first = NULL;
            turbo_media_source_t *second = NULL;

            REQUIRE_OK(turbo_media_source_key_init(&key, "default", "live", "camera"));
            turbo_media_registry_t *registry = turbo_media_registry_create(2);
            REQUIRE_NOT_NULL(registry);

            REQUIRE_OK(turbo_media_registry_get_or_create(registry, &key, &config, &first));
            REQUIRE_OK(turbo_media_registry_get_or_create(registry, &key, &config, &second));
            check_ptr_eq(first, second);
            check_size_eq(turbo_media_registry_count(registry), 1);
            check_ptr_eq(turbo_media_registry_find(registry, &key), first);

            REQUIRE_OK(turbo_media_registry_remove(registry, &key));
            check_size_eq(turbo_media_registry_count(registry), 0);
            check_null(turbo_media_registry_find(registry, &key));

            turbo_media_registry_destroy(registry);
        }
    }
}
