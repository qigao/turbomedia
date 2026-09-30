#include "tinytest.h"
#include "turbo_rtc_client.h"

#include <stdint.h>
#include <string.h>

spec("TurboMedia RTCClient WHIP audio core") {
  it("initializes an explicit bounded WHIP configuration") {
    turbo_rtc_client_config_t config;
    turbo_rtc_client_snapshot_t snapshot;

    turbo_rtc_client_config_init(&config);
    check_equal(config.size, sizeof(config));
    check_equal(config.request_timeout_ms, 5000u);
    check_equal(config.connect_timeout_ms, 10000u);
    check_equal(config.sample_rate, 48000u);
    check_equal(config.channels, 1u);
    check_equal(config.frame_size_ms, 20u);

    turbo_rtc_client_snapshot_init(&snapshot);
    check_equal(snapshot.size, sizeof(snapshot));
    check_equal(snapshot.state, TURBO_RTC_CLIENT_CREATED);
  }

  it("rejects incomplete and unbounded configuration") {
    turbo_rtc_client_config_t config;
    turbo_rtc_client_t *client = (turbo_rtc_client_t *)1;
    const char *servers[TURBO_RTC_CLIENT_MAX_ICE_SERVERS + 1u] = {0};
    size_t i;

    turbo_rtc_client_config_init(&config);
    check_equal(turbo_rtc_client_create(&config, &client),
                TURBO_RTC_CLIENT_EINVAL);
    check_null(client);

    config.whip_base_url = "http://127.0.0.1:19000";
    config.whip_path = "/whip";
    config.allow_plaintext_loopback = 1;
    config.allow_loopback = 1;
    config.sample_rate = 44100u;
    check_equal(turbo_rtc_client_create(&config, &client),
                TURBO_RTC_CLIENT_EINVAL);
    check_null(client);

    config.sample_rate = 48000u;
    for (i = 0u; i < TURBO_RTC_CLIENT_MAX_ICE_SERVERS + 1u; ++i) {
      servers[i] = "stun:127.0.0.1:3478";
    }
    config.stun_servers = servers;
    config.stun_server_count = TURBO_RTC_CLIENT_MAX_ICE_SERVERS + 1u;
    check_equal(turbo_rtc_client_create(&config, &client),
                TURBO_RTC_CLIENT_EINVAL);
    check_null(client);
  }

  it("prepares locally without starting a WHIP session") {
    turbo_rtc_client_config_t config;
    turbo_rtc_client_snapshot_t snapshot;
    turbo_rtc_client_t *client = NULL;
    const int16_t pcm[2] = {0, 0};

    turbo_rtc_client_config_init(&config);
    config.whip_base_url = "http://127.0.0.1:1";
    config.whip_path = "/whip";
    config.allow_plaintext_loopback = 1;
    config.allow_loopback = 1;

    check_equal(turbo_rtc_client_create(&config, &client),
                TURBO_RTC_CLIENT_OK);
    check_not_null(client);
    check_equal(turbo_rtc_client_prepare(client), TURBO_RTC_CLIENT_OK);

    turbo_rtc_client_snapshot_init(&snapshot);
    check_equal(turbo_rtc_client_snapshot(client, &snapshot),
                TURBO_RTC_CLIENT_OK);
    check_equal(snapshot.state, TURBO_RTC_CLIENT_PREPARED);
    check_equal(snapshot.session_active, 0);
    check_equal(snapshot.connected, 0);
    check_equal(snapshot.frames_sent, 0u);

    check_equal(turbo_rtc_client_send_audio(
                    client, pcm, sizeof(pcm), 0u),
                TURBO_RTC_CLIENT_ESTATE);
    check_equal(turbo_rtc_client_stop(client), TURBO_RTC_CLIENT_OK);
    turbo_rtc_client_snapshot_init(&snapshot);
    check_equal(turbo_rtc_client_snapshot(client, &snapshot),
                TURBO_RTC_CLIENT_OK);
    check_equal(snapshot.state, TURBO_RTC_CLIENT_CLOSED);
    check_equal(turbo_rtc_client_destroy(client), TURBO_RTC_CLIENT_OK);
  }

  it("fails insecure non-loopback signaling during prepare") {
    turbo_rtc_client_config_t config;
    turbo_rtc_client_snapshot_t snapshot;
    turbo_rtc_client_t *client = NULL;

    turbo_rtc_client_config_init(&config);
    config.whip_base_url = "http://example.invalid";
    config.whip_path = "/whip";
    config.allow_plaintext_loopback = 0;

    check_equal(turbo_rtc_client_create(&config, &client),
                TURBO_RTC_CLIENT_OK);
    check_not_null(client);
    check_equal(turbo_rtc_client_prepare(client), TURBO_RTC_CLIENT_EHTTP);

    turbo_rtc_client_snapshot_init(&snapshot);
    check_equal(turbo_rtc_client_snapshot(client, &snapshot),
                TURBO_RTC_CLIENT_OK);
    check_equal(snapshot.state, TURBO_RTC_CLIENT_FAILED);
    check_equal(turbo_rtc_client_destroy(client), TURBO_RTC_CLIENT_OK);
  }
}
