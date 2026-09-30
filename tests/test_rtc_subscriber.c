#include "tinytest.h"
#include "turbo_rtc_subscriber.h"

#include <stddef.h>
#include <stdint.h>

static int consume_audio(
    void *context,
    const void *pcm,
    size_t bytes,
    uint32_t sample_rate,
    uint32_t channels,
    uint64_t rtp_timestamp) {
    (void)context;
    (void)pcm;
    (void)bytes;
    (void)sample_rate;
    (void)channels;
    (void)rtp_timestamp;
    return 0;
}

spec("TurboMedia RTCClient WHEP audio core") {
  it("initializes explicit bounded subscriber configuration") {
    turbo_rtc_subscriber_config_t config;
    turbo_rtc_subscriber_snapshot_t snapshot;

    turbo_rtc_subscriber_config_init(&config);
    check_equal(config.size, sizeof(config));
    check_equal(config.request_timeout_ms, 5000u);
    check_equal(config.connect_timeout_ms, 10000u);
    check_equal(config.sample_rate, 48000u);
    check_equal(config.channels, 1u);
    check_equal(config.frame_queue_capacity, 8u);
    check_equal(config.max_frame_bytes, 11520u);

    turbo_rtc_subscriber_snapshot_init(&snapshot);
    check_equal(snapshot.size, sizeof(snapshot));
    check_equal(snapshot.state, TURBO_RTC_CLIENT_CREATED);
  }

  it("rejects incomplete and unbounded configuration") {
    turbo_rtc_subscriber_config_t config;
    turbo_rtc_subscriber_t *subscriber =
        (turbo_rtc_subscriber_t *)1;
    const char *servers[TURBO_RTC_SUBSCRIBER_MAX_ICE_SERVERS + 1u] = {0};
    size_t i;

    turbo_rtc_subscriber_config_init(&config);
    check_equal(
        turbo_rtc_subscriber_create(&config, &subscriber),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(subscriber);

    config.whep_base_url = "http://127.0.0.1:19000";
    config.whep_path = "/whep";
    config.allow_plaintext_loopback = 1;
    config.allow_loopback = 1;
    config.on_audio = consume_audio;

    config.frame_queue_capacity =
        TURBO_RTC_SUBSCRIBER_MAX_FRAME_QUEUE + 1u;
    check_equal(
        turbo_rtc_subscriber_create(&config, &subscriber),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(subscriber);

    config.frame_queue_capacity = 8u;
    config.max_frame_bytes =
        TURBO_RTC_SUBSCRIBER_MAX_FRAME_BYTES + 2u;
    check_equal(
        turbo_rtc_subscriber_create(&config, &subscriber),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(subscriber);

    config.max_frame_bytes = 11520u;
    config.sample_rate = 44100u;
    check_equal(
        turbo_rtc_subscriber_create(&config, &subscriber),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(subscriber);

    config.sample_rate = 48000u;
    for (i = 0u;
         i < TURBO_RTC_SUBSCRIBER_MAX_ICE_SERVERS + 1u;
         ++i) {
      servers[i] = "stun:127.0.0.1:3478";
    }
    config.stun_servers = servers;
    config.stun_server_count =
        TURBO_RTC_SUBSCRIBER_MAX_ICE_SERVERS + 1u;
    check_equal(
        turbo_rtc_subscriber_create(&config, &subscriber),
        TURBO_RTC_CLIENT_EINVAL);
    check_null(subscriber);
  }

  it("prepares locally without starting a WHEP session") {
    turbo_rtc_subscriber_config_t config;
    turbo_rtc_subscriber_snapshot_t snapshot;
    turbo_rtc_subscriber_t *subscriber = NULL;

    turbo_rtc_subscriber_config_init(&config);
    config.whep_base_url = "http://127.0.0.1:1";
    config.whep_path = "/whep";
    config.allow_plaintext_loopback = 1;
    config.allow_loopback = 1;
    config.on_audio = consume_audio;

    check_equal(
        turbo_rtc_subscriber_create(&config, &subscriber),
        TURBO_RTC_CLIENT_OK);
    check_not_null(subscriber);
    check_equal(
        turbo_rtc_subscriber_prepare(subscriber),
        TURBO_RTC_CLIENT_OK);

    turbo_rtc_subscriber_snapshot_init(&snapshot);
    check_equal(
        turbo_rtc_subscriber_snapshot(subscriber, &snapshot),
        TURBO_RTC_CLIENT_OK);
    check_equal(snapshot.state, TURBO_RTC_CLIENT_PREPARED);
    check_equal(snapshot.connected, 0);
    check_equal(snapshot.session_active, 0);
    check_equal(snapshot.frames_received, 0u);
    check_equal(snapshot.frames_delivered, 0u);
    check_equal(snapshot.frames_rejected, 0u);
    check_equal(snapshot.queue_items, 0u);

    check_equal(
        turbo_rtc_subscriber_poll(subscriber),
        TURBO_RTC_CLIENT_ESTATE);
    check_equal(
        turbo_rtc_subscriber_stop(subscriber),
        TURBO_RTC_CLIENT_OK);
    check_equal(
        turbo_rtc_subscriber_destroy(subscriber),
        TURBO_RTC_CLIENT_OK);
  }

  it("fails insecure non-loopback signaling during prepare") {
    turbo_rtc_subscriber_config_t config;
    turbo_rtc_subscriber_snapshot_t snapshot;
    turbo_rtc_subscriber_t *subscriber = NULL;

    turbo_rtc_subscriber_config_init(&config);
    config.whep_base_url = "http://example.invalid";
    config.whep_path = "/whep";
    config.on_audio = consume_audio;

    check_equal(
        turbo_rtc_subscriber_create(&config, &subscriber),
        TURBO_RTC_CLIENT_OK);
    check_not_null(subscriber);
    check_equal(
        turbo_rtc_subscriber_prepare(subscriber),
        TURBO_RTC_CLIENT_EHTTP);

    turbo_rtc_subscriber_snapshot_init(&snapshot);
    check_equal(
        turbo_rtc_subscriber_snapshot(subscriber, &snapshot),
        TURBO_RTC_CLIENT_OK);
    check_equal(snapshot.state, TURBO_RTC_CLIENT_FAILED);
    check_equal(
        turbo_rtc_subscriber_destroy(subscriber),
        TURBO_RTC_CLIENT_OK);
  }
}
