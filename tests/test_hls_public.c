#include <tinytest.h>
#include <turbo_player.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HLS_TEST_URL_CAPACITY = 2048 };

static const char HLS_TEST_RUN_ENV[] = "TURBO_MEDIA_RUN_PUBLIC_HLS_TESTS";
static const char HLS_TEST_ROOT_ENV[] = "TURBO_MEDIA_PUBLIC_HLS_ROOT";

static int public_hls_tests_enabled(void) {
    const char *enabled = getenv(HLS_TEST_RUN_ENV);
    return enabled && strcmp(enabled, "1") == 0;
}

static void check_public_hls_stream(const char *relative_path) {
    const char *root;
    char url[HLS_TEST_URL_CAPACITY];
    int length;
    turbo_player_config_t config = {0};
    turbo_player_t *player;

    if (!public_hls_tests_enabled()) {
        check_true(1);
        return;
    }

    root = getenv(HLS_TEST_ROOT_ENV);
    if (!root) {
        info("%s=1 requires %s", HLS_TEST_RUN_ENV, HLS_TEST_ROOT_ENV);
    }
    check_not_null(root);
    if (!root) {
        return;
    }
    check_greater(strlen(root), 0);
    if (root[0] == '\0') {
        return;
    }

    length = snprintf(url, sizeof(url), "%s/%s", root, relative_path);
    check_greater(length, 0);
    check_less((size_t)length, sizeof(url));
    if (length <= 0 || (size_t)length >= sizeof(url)) {
        return;
    }

    config.play_audio = 0;
    config.video_format = TURBO_PLAYER_VIDEO_RGBA;
    player = turbo_player_open(url, &config);
    check_not_null(player);
    if (player) {
        turbo_player_close(player);
    }
}

suite("public HLS compatibility") {
    it("opens a stream with program date time and discontinuities") {
        check_public_hls_stream("test-program-time/playlist.m3u8");
    }

    it("opens a stream with fMP4 WebVTT subtitles") {
        check_public_hls_stream("test-vtt-fmp4-segments/playlist.m3u8");
    }
}
