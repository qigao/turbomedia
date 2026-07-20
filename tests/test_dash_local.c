#include "helpers.h"

#include <tinytest.h>
#include <turbo_codec.h>
#include <turbo_fs.h>
#include <turbo_streamer.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    DASH_TEST_WIDTH = 160,
    DASH_TEST_HEIGHT = 96,
    DASH_TEST_FPS = 30,
    DASH_TEST_FRAME_COUNT = 45,
    DASH_TEST_KEYFRAME_INTERVAL = 15,
    DASH_TEST_SEGMENT_DURATION_MS = 500,
    DASH_TEST_PLAYLIST_SIZE = 2,
    DASH_TEST_BITRATE = 200000
};

static void test_dash_h264_pipeline(void) {
    turbo_video_codec_config_t codec_config = {0};
    turbo_streamer_config_t streamer_config = {0};
    turbo_stream_info_t stream_info = {0};
    turbo_encoded_frame_t frame_info = {0};
    turbo_muxer_packet_t packet = {0};
    turbo_codec_t *encoder = NULL;
    turbo_streamer_t *streamer = NULL;
    uint8_t *raw_frame = NULL;
    uint8_t *encoded_frame = NULL;
    char *output_dir = NULL;
    char *manifest = NULL;
    size_t raw_frame_size;
    size_t encoded_size;
    size_t manifest_size = 0;
    char manifest_path[TURBO_FS_MAX_PATH];
    char init_path[TURBO_FS_MAX_PATH];
    char segment_path[TURBO_FS_MAX_PATH];
    int stream_id = -1;
    int connected = 0;
    int result;

    output_dir = tt_make_temp_dir("turbomedia-dash");
    check_not_null(output_dir);
    if (!output_dir) goto cleanup;

    raw_frame_size = test_calculate_i420_frame_size(DASH_TEST_WIDTH, DASH_TEST_HEIGHT);
    raw_frame = (uint8_t *)malloc(raw_frame_size);
    encoded_frame = (uint8_t *)malloc(TURBO_CODEC_MAX_FRAME_SIZE);
    check_not_null(raw_frame);
    check_not_null(encoded_frame);
    if (!raw_frame || !encoded_frame) goto cleanup;

    turbo_codec_registry_init();
    turbo_streamer_registry_init();
    check_not_null(turbo_streamer_find_by_protocol(TURBO_STREAMER_DASH));

    codec_config.width = DASH_TEST_WIDTH;
    codec_config.height = DASH_TEST_HEIGHT;
    codec_config.framerate = DASH_TEST_FPS;
    codec_config.bitrate = DASH_TEST_BITRATE;
    codec_config.keyframe_interval = DASH_TEST_KEYFRAME_INTERVAL;
    codec_config.threads = 1;
    encoder = turbo_codec_create_encoder("h264", &codec_config);
    check_not_null(encoder);
    if (!encoder) goto cleanup;

    test_generate_i420_solid(raw_frame, DASH_TEST_WIDTH, DASH_TEST_HEIGHT, 16, 128,
                             128);
    encoded_size = TURBO_CODEC_MAX_FRAME_SIZE;
    result = turbo_codec_encode(encoder, raw_frame, raw_frame_size, encoded_frame,
                                &encoded_size, &frame_info);
    check_int_eq(result, TURBO_CODEC_OK);
    check_true(frame_info.is_keyframe);
    if (result != TURBO_CODEC_OK || !frame_info.is_keyframe) goto cleanup;

    streamer_config.protocol = TURBO_STREAMER_DASH;
    streamer_config.segment_duration_ms = DASH_TEST_SEGMENT_DURATION_MS;
    streamer_config.playlist_size = DASH_TEST_PLAYLIST_SIZE;
    streamer_config.output_dir = output_dir;
    streamer_config.base_url = "https://media.example/live";
    streamer = turbo_streamer_create(&streamer_config);
    check_not_null(streamer);
    if (!streamer) goto cleanup;

    stream_info.type = TURBO_CODEC_TYPE_VIDEO;
    stream_info.codec_name = "h264";
    stream_info.extradata = encoded_frame;
    stream_info.extradata_size = encoded_size;
    stream_info.width = DASH_TEST_WIDTH;
    stream_info.height = DASH_TEST_HEIGHT;
    stream_info.framerate = DASH_TEST_FPS;
    result = turbo_streamer_add_stream(streamer, &stream_info, &stream_id);
    check_int_eq(result, 0);
    if (result != 0) goto cleanup;

    result = turbo_streamer_connect(streamer);
    check_int_eq(result, 0);
    if (result != 0) goto cleanup;
    connected = 1;

    for (int frame_index = 0; frame_index < DASH_TEST_FRAME_COUNT; ++frame_index) {
        if (frame_index > 0) {
            test_generate_i420_solid(raw_frame, DASH_TEST_WIDTH, DASH_TEST_HEIGHT,
                                     (uint8_t)(16 + (frame_index * 5) % 200),
                                     (uint8_t)(96 + frame_index % 64),
                                     (uint8_t)(160 - frame_index % 64));
            encoded_size = TURBO_CODEC_MAX_FRAME_SIZE;
            memset(&frame_info, 0, sizeof(frame_info));
            result = turbo_codec_encode(encoder, raw_frame, raw_frame_size,
                                        encoded_frame, &encoded_size, &frame_info);
            check_int_eq(result, TURBO_CODEC_OK);
            if (result != TURBO_CODEC_OK) goto cleanup;
        }

        packet.stream_id = stream_id;
        packet.data = encoded_frame;
        packet.size = encoded_size;
        packet.pts = (int64_t)frame_index * 1000000 / DASH_TEST_FPS;
        packet.dts = packet.pts;
        packet.duration = 1000000 / DASH_TEST_FPS;
        packet.is_keyframe = frame_info.is_keyframe;
        result = turbo_streamer_write_packet(streamer, &packet);
        check_int_eq(result, 0);
        if (result != 0) goto cleanup;
    }

    result = turbo_streamer_disconnect(streamer);
    connected = 0;
    check_int_eq(result, 0);
    if (result != 0) goto cleanup;

    result = turbo_fs_path_join(manifest_path, sizeof(manifest_path), output_dir,
                                "manifest.mpd");
    check_int_eq(result, 0);
    if (result != 0) goto cleanup;
    manifest = tt_read_file(manifest_path, &manifest_size);
    check_not_null(manifest);
    check_size_gt(manifest_size, 0);
    if (!manifest) goto cleanup;
    check_str_contains(manifest, "type=\"dynamic\"");
    check_str_contains(manifest,
                       "<BaseURL>https://media.example/live/</BaseURL>");
    check_str_contains(manifest, "initialization=\"video-init.m4v\"");
    check_str_contains(manifest, "frameRate=\"30\"");
    check_true(strstr(manifest, "<S t=\"0\"") == NULL);
    check_str_contains(manifest, "<S t=\"500\"");
    check_str_contains(manifest, "<S t=\"1000\"");
    check_str_contains(manifest, "d=\"500\"");

    result = turbo_fs_path_join(init_path, sizeof(init_path), output_dir,
                                "video-init.m4v");
    check_int_eq(result, 0);
    if (result == 0)
        check_int_eq(turbo_fs_access(init_path, TURBO_FS_ACCESS_EXISTS), 0);
    result = turbo_fs_path_join(segment_path, sizeof(segment_path), output_dir,
                                "video-1000.m4v");
    check_int_eq(result, 0);
    if (result == 0)
        check_int_eq(turbo_fs_access(segment_path, TURBO_FS_ACCESS_EXISTS), 0);

cleanup:
    if (connected && streamer) turbo_streamer_disconnect(streamer);
    turbo_streamer_destroy(streamer);
    turbo_codec_destroy(encoder);
    turbo_streamer_registry_shutdown();
    turbo_codec_registry_shutdown();
    free(manifest);
    free(encoded_frame);
    free(raw_frame);
    if (output_dir) {
        check_int_eq(tt_remove_tree(output_dir), 0);
        free(output_dir);
    }
}

suite("local MPEG-DASH fMP4 pipeline") {
    it("honors configured segmentation and live window") {
        test_dash_h264_pipeline();
    }
}
