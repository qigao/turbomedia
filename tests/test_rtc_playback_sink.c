#include "tinytest.h"
#include "turbo_rtc_playback_sink.h"

#include <stdint.h>
#include <string.h>

spec("TurboMedia RTCClient Salts playback sink") {
  it("initializes bounded S16 playback defaults") {
    turbo_rtc_playback_sink_config_t config;
    turbo_rtc_playback_sink_snapshot_t snapshot;

    turbo_rtc_playback_sink_config_init(&config);
    check_equal(config.size, sizeof(config));
    check_equal(config.sample_rate, 48000u);
    check_equal(config.channels, 1u);
    check_equal(config.buffer_duration_ms, 120u);
    check_equal(config.drain_timeout_ms, 500u);

    turbo_rtc_playback_sink_snapshot_init(&snapshot);
    check_equal(snapshot.size, sizeof(snapshot));
    check_equal(snapshot.state, TURBO_RTC_PLAYBACK_SINK_CREATED);
    check_equal(snapshot.playback_state, SALTS_PLAYBACK_STATE_STOPPED);
  }

  it("validates format and bounded buffer before owning a device") {
    turbo_rtc_playback_sink_config_t config;
    turbo_rtc_playback_sink_t *sink =
        (turbo_rtc_playback_sink_t *)(uintptr_t)1u;

    turbo_rtc_playback_sink_config_init(&config);
    config.sample_rate = 44100u;
    check_equal(
        turbo_rtc_playback_sink_create(&config, &sink),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(sink);

    turbo_rtc_playback_sink_config_init(&config);
    config.channels = 3u;
    check_equal(
        turbo_rtc_playback_sink_create(&config, &sink),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(sink);

    turbo_rtc_playback_sink_config_init(&config);
    config.buffer_duration_ms = SALTS_PLAYBACK_MIN_BUFFER_MS - 1u;
    check_equal(
        turbo_rtc_playback_sink_create(&config, &sink),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(sink);

    turbo_rtc_playback_sink_config_init(&config);
    config.drain_timeout_ms = 0u;
    check_equal(
        turbo_rtc_playback_sink_create(&config, &sink),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(sink);
  }

  it("creates without opening playback and rejects producer use before start") {
    turbo_rtc_playback_sink_config_t config;
    turbo_rtc_playback_sink_snapshot_t snapshot;
    turbo_rtc_playback_sink_t *sink = NULL;
    int16_t pcm[480] = {0};

    turbo_rtc_playback_sink_config_init(&config);
    check_equal(
        turbo_rtc_playback_sink_create(&config, &sink),
        TURBO_RTC_CLIENT_OK);
    check_not_null(sink);

    turbo_rtc_playback_sink_snapshot_init(&snapshot);
    check_equal(
        turbo_rtc_playback_sink_snapshot(sink, &snapshot),
        TURBO_RTC_CLIENT_OK);
    check_equal(snapshot.state, TURBO_RTC_PLAYBACK_SINK_CREATED);
    check_equal(snapshot.buffered_bytes, 0u);
    check_equal(snapshot.available_bytes, 0u);

    check_equal(
        turbo_rtc_playback_sink_on_audio(
            sink, pcm, sizeof(pcm), 48000u, 1u, 0u),
        TURBO_RTC_SUBSCRIBER_AUDIO_FATAL);
    check_equal(
        turbo_rtc_playback_sink_destroy(sink),
        TURBO_RTC_CLIENT_OK);
  }

  it("surfaces an invalid Salts device at prepare without fallback") {
    salts_playback_device_t invalid_device;
    turbo_rtc_playback_sink_config_t config;
    turbo_rtc_playback_sink_snapshot_t snapshot;
    turbo_rtc_playback_sink_t *sink = NULL;

    memset(&invalid_device, 0, sizeof(invalid_device));
    invalid_device.id_size = 1u;

    turbo_rtc_playback_sink_config_init(&config);
    config.device = &invalid_device;
    check_equal(
        turbo_rtc_playback_sink_create(&config, &sink),
        TURBO_RTC_CLIENT_OK);
    check_not_null(sink);
    check_equal(
        turbo_rtc_playback_sink_prepare(sink),
        TURBO_RTC_CLIENT_EIO);

    turbo_rtc_playback_sink_snapshot_init(&snapshot);
    check_equal(
        turbo_rtc_playback_sink_snapshot(sink, &snapshot),
        TURBO_RTC_CLIENT_OK);
    check_equal(snapshot.state, TURBO_RTC_PLAYBACK_SINK_FAILED);
    check_equal(
        turbo_rtc_playback_sink_destroy(sink),
        TURBO_RTC_CLIENT_OK);
  }
}
