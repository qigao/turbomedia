#include "helpers.h"

#include <tinytest.h>
#include <turbo_codec.h>
#include <turbo_fs.h>
#include <turbo_player.h>
#include <turbo_streamer.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    HLS_TEST_WIDTH = 160,
    HLS_TEST_HEIGHT = 96,
    HLS_TEST_FPS = 30,
    HLS_TEST_FRAME_COUNT = 45,
    HLS_TEST_KEYFRAME_INTERVAL = 15,
    HLS_TEST_SEGMENT_DURATION_MS = 500,
    HLS_TEST_PLAYLIST_SIZE = 3,
    HLS_TEST_BITRATE = 200000
};

typedef struct {
    size_t video_frames;
} hls_player_capture_t;

static void hls_capture_video(turbo_player_t *player,
                              const turbo_player_video_frame_t *frame,
                              void *user_data) {
    hls_player_capture_t *capture = (hls_player_capture_t *)user_data;
    (void)player;
    if (frame && frame->data && frame->len > 0) {
        ++capture->video_frames;
    }
}

static void test_hls_fmp4_round_trip(const char *codec_name) {
    turbo_video_codec_config_t codec_config = {0};
    turbo_streamer_config_t streamer_config = {0};
    turbo_stream_info_t stream_info = {0};
    turbo_player_config_t player_config = {0};
    turbo_encoded_frame_t frame_info = {0};
    turbo_muxer_packet_t packet = {0};
    hls_player_capture_t capture = {0};
    turbo_codec_t *encoder = NULL;
    turbo_streamer_t *streamer = NULL;
    turbo_player_t *player = NULL;
    uint8_t *raw_frame = NULL;
    uint8_t *encoded_frame = NULL;
    char *output_dir = NULL;
    char *playlist = NULL;
    char *playlist_file = NULL;
    size_t raw_frame_size;
    size_t encoded_size;
    size_t playlist_size = 0;
    size_t playlist_file_size = 0;
    char playlist_path[TURBO_FS_MAX_PATH];
    int stream_id = -1;
    int connected = 0;
    int result;

    output_dir = tt_make_temp_dir("turbomedia-hls");
    check_not_null(output_dir);
    if (!output_dir) goto cleanup;

    raw_frame_size = test_calculate_i420_frame_size(HLS_TEST_WIDTH, HLS_TEST_HEIGHT);
    raw_frame = (uint8_t *)malloc(raw_frame_size);
    encoded_frame = (uint8_t *)malloc(TURBO_CODEC_MAX_FRAME_SIZE);
    check_not_null(raw_frame);
    check_not_null(encoded_frame);
    if (!raw_frame || !encoded_frame) goto cleanup;

    turbo_codec_registry_init();
    turbo_streamer_registry_init();

    codec_config.width = HLS_TEST_WIDTH;
    codec_config.height = HLS_TEST_HEIGHT;
    codec_config.framerate = HLS_TEST_FPS;
    codec_config.bitrate = HLS_TEST_BITRATE;
    codec_config.keyframe_interval = HLS_TEST_KEYFRAME_INTERVAL;
    codec_config.threads = 1;
    encoder = turbo_codec_create_encoder(codec_name, &codec_config);
    check_not_null(encoder);
    if (!encoder) goto cleanup;

    test_generate_i420_solid(raw_frame, HLS_TEST_WIDTH, HLS_TEST_HEIGHT, 16, 128,
                             128);
    encoded_size = TURBO_CODEC_MAX_FRAME_SIZE;
    result = turbo_codec_encode(encoder, raw_frame, raw_frame_size, encoded_frame,
                                &encoded_size, &frame_info);
    check_int_eq(result, TURBO_CODEC_OK);
    check_size_gt(encoded_size, 0);
    check_true(frame_info.is_keyframe);
    if (result != TURBO_CODEC_OK || encoded_size == 0 || !frame_info.is_keyframe)
        goto cleanup;

    streamer_config.protocol = TURBO_STREAMER_HLS;
    streamer_config.segment_duration_ms = HLS_TEST_SEGMENT_DURATION_MS;
    streamer_config.playlist_size = HLS_TEST_PLAYLIST_SIZE;
    streamer_config.output_dir = output_dir;
    streamer = turbo_streamer_create(&streamer_config);
    check_not_null(streamer);
    if (!streamer) goto cleanup;
    turbo_streamer_hls_set_playlist_type(streamer, TURBO_HLS_VOD);

    stream_info.type = TURBO_CODEC_TYPE_VIDEO;
    stream_info.codec_name = codec_name;
    stream_info.extradata = encoded_frame;
    stream_info.extradata_size = encoded_size;
    stream_info.width = HLS_TEST_WIDTH;
    stream_info.height = HLS_TEST_HEIGHT;
    stream_info.framerate = HLS_TEST_FPS;
    result = turbo_streamer_add_stream(streamer, &stream_info, &stream_id);
    check_int_eq(result, 0);
    if (result != 0) goto cleanup;

    result = turbo_streamer_connect(streamer);
    check_int_eq(result, 0);
    if (result != 0) goto cleanup;
    connected = 1;

    for (int frame_index = 0; frame_index < HLS_TEST_FRAME_COUNT; ++frame_index) {
        if (frame_index > 0) {
            test_generate_i420_solid(raw_frame, HLS_TEST_WIDTH, HLS_TEST_HEIGHT,
                                     (uint8_t)(16 + (frame_index * 5) % 200),
                                     (uint8_t)(96 + frame_index % 64),
                                     (uint8_t)(160 - frame_index % 64));
            encoded_size = TURBO_CODEC_MAX_FRAME_SIZE;
            memset(&frame_info, 0, sizeof(frame_info));
            result = turbo_codec_encode(encoder, raw_frame, raw_frame_size,
                                        encoded_frame, &encoded_size, &frame_info);
            check_int_eq(result, TURBO_CODEC_OK);
            check_size_gt(encoded_size, 0);
            if (result != TURBO_CODEC_OK || encoded_size == 0) goto cleanup;
        }

        packet.stream_id = stream_id;
        packet.data = encoded_frame;
        packet.size = encoded_size;
        packet.pts = (int64_t)frame_index * 1000000 / HLS_TEST_FPS;
        packet.dts = packet.pts;
        packet.duration = 1000000 / HLS_TEST_FPS;
        packet.is_keyframe = frame_info.is_keyframe;
        result = turbo_streamer_write_packet(streamer, &packet);
        check_int_eq(result, 0);
        if (result != 0) goto cleanup;
    }

    result = turbo_streamer_disconnect(streamer);
    connected = 0;
    check_int_eq(result, 0);
    if (result != 0) goto cleanup;

    result = turbo_streamer_hls_get_playlist(streamer, &playlist, &playlist_size);
    check_int_eq(result, 0);
    check_not_null(playlist);
    check_size_gt(playlist_size, 0);
    if (result != 0 || !playlist) goto cleanup;
    check_str_contains(playlist, "#EXT-X-MAP:URI=\"init.mp4\"");
    check_str_contains(playlist, "segment_0.m4s");
    check_str_contains(playlist, "segment_1.m4s");
    check_str_contains(playlist, "#EXT-X-ENDLIST");

    result = turbo_fs_path_join(playlist_path, sizeof(playlist_path), output_dir,
                                "playlist.m3u8");
    check_int_eq(result, 0);
    if (result != 0) goto cleanup;
    playlist_file = tt_read_file(playlist_path, &playlist_file_size);
    check_not_null(playlist_file);
    check_size_eq(playlist_file_size, playlist_size);
    if (!playlist_file) goto cleanup;
    check_mem_eq(playlist_file, playlist, playlist_size);

    player_config.play_audio = 0;
    player_config.video_format = TURBO_PLAYER_VIDEO_I420;
    player = turbo_player_open(playlist_path, &player_config);
    check_not_null(player);
    if (!player) goto cleanup;
    turbo_player_set_video_callback(player, hls_capture_video, &capture);
    result = turbo_player_play_to_end(player);
    check_int_eq(result, TURBO_PLAYER_OK);
    check_size_eq(capture.video_frames, HLS_TEST_FRAME_COUNT);

cleanup:
    if (player) turbo_player_close(player);
    if (connected && streamer) turbo_streamer_disconnect(streamer);
    turbo_streamer_destroy(streamer);
    turbo_codec_destroy(encoder);
    turbo_streamer_registry_shutdown();
    turbo_codec_registry_shutdown();
    free(playlist_file);
    free(playlist);
    free(encoded_frame);
    free(raw_frame);
    if (output_dir) {
        check_int_eq(tt_remove_tree(output_dir), 0);
        free(output_dir);
    }
}

suite("local HLS fMP4 pipeline") {
    it("round trips encoded H264 through generated VOD segments") {
        test_hls_fmp4_round_trip("h264");
    }

    it("round trips encoded H265 through generated VOD segments") {
        test_hls_fmp4_round_trip("h265");
    }
}
