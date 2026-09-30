#include "tinytest.h"
#include "turbo_rtc_capture_source.h"

#include <stdint.h>
#include <string.h>

spec("TurboMedia RTCClient Salts capture source") {
  it("initializes an explicit bounded S16 capture contract") {
    turbo_rtc_capture_source_config_t config;
    turbo_rtc_capture_source_snapshot_t snapshot;

    turbo_rtc_capture_source_config_init(&config);
    check_equal(config.size, sizeof(config));
    check_equal(config.sample_rate, 48000u);
    check_equal(config.channels, 1u);
    check_equal(config.frame_size_ms, 20u);
    check_equal(config.frame_queue_capacity, 8u);
    check_equal(config.max_frame_bytes, 16384u);

    turbo_rtc_capture_source_snapshot_init(&snapshot);
    check_equal(snapshot.size, sizeof(snapshot));
    check_equal(snapshot.state, TURBO_RTC_CAPTURE_SOURCE_CREATED);
  }

  it("rejects unsupported and unbounded source configuration") {
    turbo_rtc_capture_source_config_t config;
    turbo_rtc_capture_source_t *source =
        (turbo_rtc_capture_source_t *)(uintptr_t)1u;
    char long_device[TURBO_RTC_CAPTURE_SOURCE_MAX_DEVICE_ID + 1u];

    turbo_rtc_capture_source_config_init(&config);
    config.sample_rate = 44100u;
    check_equal(
        turbo_rtc_capture_source_create(&config, &source),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(source);

    turbo_rtc_capture_source_config_init(&config);
    config.channels = 3u;
    check_equal(
        turbo_rtc_capture_source_create(&config, &source),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(source);

    turbo_rtc_capture_source_config_init(&config);
    config.frame_size_ms = 15u;
    check_equal(
        turbo_rtc_capture_source_create(&config, &source),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(source);

    turbo_rtc_capture_source_config_init(&config);
    config.frame_queue_capacity =
        TURBO_RTC_CAPTURE_SOURCE_MAX_QUEUE + 1u;
    check_equal(
        turbo_rtc_capture_source_create(&config, &source),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(source);

    turbo_rtc_capture_source_config_init(&config);
    config.max_frame_bytes = 100u;
    check_equal(
        turbo_rtc_capture_source_create(&config, &source),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(source);

    memset(long_device, 'x', sizeof(long_device));
    long_device[sizeof(long_device) - 1u] = '\0';
    turbo_rtc_capture_source_config_init(&config);
    config.device_id = long_device;
    check_equal(
        turbo_rtc_capture_source_create(&config, &source),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(source);
  }

  it("creates without opening a device") {
    turbo_rtc_capture_source_config_t config;
    turbo_rtc_capture_source_snapshot_t snapshot;
    turbo_rtc_capture_source_t *source = NULL;
    turbo_rtc_client_t *client = NULL;

    turbo_rtc_capture_source_config_init(&config);
    config.device_id = "exact-device-id";

    check_equal(
        turbo_rtc_capture_source_create(&config, &source),
        TURBO_RTC_CLIENT_OK);
    check_not_null(source);

    turbo_rtc_capture_source_snapshot_init(&snapshot);
    check_equal(
        turbo_rtc_capture_source_snapshot(source, &snapshot),
        TURBO_RTC_CLIENT_OK);
    check_equal(snapshot.state, TURBO_RTC_CAPTURE_SOURCE_CREATED);
    check_equal(snapshot.frames_captured, 0u);
    check_equal(snapshot.frames_sent, 0u);
    check_equal(snapshot.frames_rejected, 0u);
    check_equal(snapshot.queue_items, 0u);

    check_equal(
        turbo_rtc_capture_source_poll(source, client),
        TURBO_RTC_CLIENT_EINVAL);
    check_equal(
        turbo_rtc_capture_source_stop(source),
        TURBO_RTC_CLIENT_ESTATE);
    check_equal(
        turbo_rtc_capture_source_destroy(source),
        TURBO_RTC_CLIENT_OK);
  }
}
