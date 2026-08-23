#include <tinytest.h>
#include <turbo_streamer.h>

static void test_rtmp_metadata_configuration(void) {
    turbo_streamer_config_t config = {0};
    turbo_streamer_t *streamer;

    turbo_streamer_registry_init();
    config.protocol = TURBO_STREAMER_RTMP;
    config.url = "rtmp://127.0.0.1/live/metadata-test";
    streamer = turbo_streamer_create(&config);

    check_not_null(streamer);
    if (streamer) {
        check_equal(turbo_streamer_rtmp_set_metadata(streamer, "title", "TurboMedia"), 0);
        check_equal(turbo_streamer_rtmp_set_metadata(streamer, "author", "TurboNet"), 0);
        check_equal(turbo_streamer_rtmp_set_metadata(streamer, "unsupported", "value"), -1);
        check_equal(turbo_streamer_rtmp_set_metadata(streamer, NULL, "value"), -1);
        turbo_streamer_destroy(streamer);
    }
    turbo_streamer_registry_shutdown();
}

suite("RTMP metadata") {
    it("stores supported metadata and rejects unknown keys") {
        test_rtmp_metadata_configuration();
    }
}
