#include <tinytest.h>
#include <turbo_streamer.h>
#include <stdlib.h>

typedef struct destroy_probe_s {
    int calls;
} destroy_probe_t;

static int destroy_probe_fail_once(void *context) {
    destroy_probe_t *probe = (destroy_probe_t *)context;
    ++probe->calls;
    return probe->calls == 1 ? -1 : 0;
}

static void test_streamer_destroy_retains_owner_on_failure(void) {
    static const turbo_streamer_ops_t ops = {
        .name = "destroy-probe",
        .protocol = TURBO_STREAMER_RTMP,
        .destroy = destroy_probe_fail_once,
    };
    destroy_probe_t probe = {0};
    turbo_streamer_t *streamer =
        (turbo_streamer_t *)calloc(1, sizeof(*streamer));

    check_not_null(streamer);
    streamer->ops = &ops;
    streamer->ctx = &probe;
    check_equal(turbo_streamer_destroy(streamer), -1);
    check_equal(probe.calls, 1);
    check_equal(streamer->ctx, &probe);
    check_equal(turbo_streamer_destroy(streamer), 0);
    check_equal(probe.calls, 2);
}

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
        check_equal(turbo_streamer_destroy(streamer), 0);
    }
    turbo_streamer_registry_shutdown();
}

suite("RTMP metadata") {
    it("stores supported metadata and rejects unknown keys") {
        test_rtmp_metadata_configuration();
    }

    it("retains ownership when context destruction fails") {
        test_streamer_destroy_retains_owner_on_failure();
    }
}
