#include "turbo_media_server.h"
#include "salts_uuid.h"
#include <salts_thread.h>
#include <tinytest.h>

#ifdef TURBO_MEDIA_HAS_RTSP

#include <stdint.h>
#include <string.h>

enum {
    RTSP_ADAPTER_LIFECYCLE_PORT = 20610,
    RTSP_ADAPTER_E2E_PORT = 20611,
    RTSP_ADAPTER_TIMEOUT_MS = 3000,
    RTSP_ADAPTER_WAIT_ATTEMPTS = 1000
};

#define RTSP_ADAPTER_E2E_URI "rtsp://127.0.0.1:20611/live/cam"
#define RTSP_ADAPTER_SESSION_PREFIX "adapter-e2e"

static turbo_media_server_config_t adapter_server_config(void) {
    turbo_media_server_config_t config;
    memset(&config, 0, sizeof(config));
    config.max_sources = 4;
    config.source_config.max_tracks = 8;
    config.source_config.max_subscribers = 4;
    config.source_config.gop_capacity = 4;
    return config;
}

static turbo_rtsp_server_config_t adapter_rtsp_config(int port) {
    turbo_rtsp_server_config_t config;
    memset(&config, 0, sizeof(config));
    config.bind_host = "127.0.0.1";
    config.port = port;
    config.client_timeout_ms = RTSP_ADAPTER_TIMEOUT_MS;
    config.connection_capacity = 8u;
    config.send_queue_capacity = 32u;
    config.send_queue_bytes = 512u * 1024u;
    return config;
}

static turbo_media_rtsp_server_adapter_config_t adapter_config(const char *session_id) {
    turbo_media_rtsp_server_adapter_config_t config;
    memset(&config, 0, sizeof(config));
    config.vhost = "default";
    config.session_id = session_id;
    config.default_rtp_channel_count = 2u;
    config.remove_source_on_close = 1;
    config.replay_cached = 0;
    return config;
}

static turbo_rtsp_client_t *adapter_client_connect(int port) {
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client;
    memset(&config, 0, sizeof(config));
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = RTSP_ADAPTER_TIMEOUT_MS;
    config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_TCP;
    client = turbo_rtsp_client_create(&config);
    if (!client || turbo_rtsp_client_connect(client) != 0) {
        turbo_rtsp_client_destroy(client);
        return NULL;
    }
    return client;
}

static int adapter_session_has_uuid_suffix(
    const turbo_rtsp_session_header_t *session,
    const char *prefix) {
    salts_uuid_t uuid;
    const size_t prefix_len = prefix ? strlen(prefix) : 0u;
    if (!session || !prefix || strncmp(session->id, prefix, prefix_len) != 0 ||
        session->id[prefix_len] != '-') {
        return 0;
    }
    return salts_uuid_parse(session->id + prefix_len + 1u, &uuid) == SALTS_OK;
}

static int adapter_wait_for_frames(
    turbo_media_server_runtime_t *runtime,
    uint64_t expected,
    turbo_media_server_stats_t *stats) {
    int attempts;
    for (attempts = 0; attempts < RTSP_ADAPTER_WAIT_ATTEMPTS; ++attempts) {
        if (turbo_media_server_runtime_get_stats(runtime, stats) == TURBO_MEDIA_OK &&
            stats->frames_published >= expected) {
            return 0;
        }
        salts_sleep_ms(1u);
    }
    return -1;
}

suite("turbo_media_server_rtsp_adapter") {
    group("lifecycle") {
        it("creates, starts, and idempotently stops the Salts RTSP adapter") {
            turbo_media_server_config_t server_config = adapter_server_config();
            turbo_rtsp_server_config_t rtsp_config =
                adapter_rtsp_config(RTSP_ADAPTER_LIFECYCLE_PORT);
            turbo_media_rtsp_server_adapter_config_t bridge_config =
                adapter_config("adapter-test");
            turbo_media_server_runtime_t *runtime =
                turbo_media_server_runtime_create(&server_config);
            turbo_media_rtsp_server_adapter_t *adapter;

            check_not_null(runtime);
            adapter = turbo_media_server_rtsp_adapter_create(
                runtime, &rtsp_config, &bridge_config);
            check_not_null(adapter);
            check_not_null(turbo_media_server_rtsp_adapter_rtsp_server(adapter));
            check_equal(turbo_media_server_rtsp_adapter_start(adapter), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_rtsp_adapter_stop(adapter), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_rtsp_adapter_stop(adapter), TURBO_MEDIA_OK);

            turbo_media_server_rtsp_adapter_destroy(adapter);
            turbo_media_server_runtime_destroy(runtime);
        }
    }

    group("end-to-end") {
        it("publishes and plays one interleaved RTP frame through MediaRegistry") {
            static const char sdp[] =
                "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=push\r\nt=0 0\r\n"
                "m=audio 0 RTP/AVP 111\r\na=rtpmap:111 opus/48000/2\r\n"
                "a=control:trackID=0\r\n"
                "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                "a=control:trackID=1\r\n";
            static const uint8_t expected_payload[] = {
                0x80, 0x61, 0x00, 0x01, 0xaa, 0xbb, 0xcc, 0xdd};
            turbo_media_server_config_t server_config = adapter_server_config();
            turbo_rtsp_server_config_t rtsp_config =
                adapter_rtsp_config(RTSP_ADAPTER_E2E_PORT);
            turbo_media_rtsp_server_adapter_config_t bridge_config =
                adapter_config(RTSP_ADAPTER_SESSION_PREFIX);
            turbo_media_server_runtime_t *runtime =
                turbo_media_server_runtime_create(&server_config);
            turbo_media_rtsp_server_adapter_t *adapter;
            turbo_rtsp_client_t *publisher;
            turbo_rtsp_client_t *player;
            turbo_rtsp_push_announce_t announce;
            turbo_rtsp_push_track_t publish_track;
            turbo_rtsp_session_header_t publisher_session;
            turbo_rtsp_session_header_t player_session;
            turbo_media_server_stats_t stats;
            uint8_t received[32];
            uint8_t received_channel = 0u;
            size_t received_size = 0u;

            check_not_null(runtime);
            adapter = turbo_media_server_rtsp_adapter_create(
                runtime, &rtsp_config, &bridge_config);
            check_not_null(adapter);
            check_equal(turbo_media_server_rtsp_adapter_start(adapter), TURBO_MEDIA_OK);

            publisher = adapter_client_connect(RTSP_ADAPTER_E2E_PORT);
            check_not_null(publisher);
            memset(&announce, 0, sizeof(announce));
            announce.uri = RTSP_ADAPTER_E2E_URI;
            announce.sdp = sdp;
            announce.sdp_len = sizeof(sdp) - 1u;
            check_equal(turbo_rtsp_client_announce(publisher, &announce), 0);
            memset(&publish_track, 0, sizeof(publish_track));
            publish_track.control_uri = RTSP_ADAPTER_E2E_URI "/trackID=1";
            publish_track.rtp_channel = 2u;
            publish_track.rtcp_channel = 3u;
            check_equal(turbo_rtsp_client_setup_interleaved(publisher, &publish_track), 0);
            check_equal(turbo_rtsp_client_record(publisher), 0);
            check_equal(turbo_rtsp_client_get_session(publisher, &publisher_session), 0);

            player = adapter_client_connect(RTSP_ADAPTER_E2E_PORT);
            check_not_null(player);
            check_equal(turbo_rtsp_client_describe(player, RTSP_ADAPTER_E2E_URI), 0);
            check_equal(turbo_rtsp_client_get_media_track_count(player), 2u);
            check_equal(turbo_rtsp_client_setup_play_interleaved_track_index(
                            player, 1u, 6u, 7u), 0);
            check_equal(turbo_rtsp_client_play(player, "npt=0-"), 0);
            check_equal(turbo_rtsp_client_get_session(player, &player_session), 0);

            check_equal(turbo_rtsp_client_send_interleaved_frame(
                            publisher, 2u, expected_payload, sizeof(expected_payload)), 0);
            check_equal(turbo_rtsp_client_recv_interleaved_frame(
                            player, &received_channel, received,
                            sizeof(received), &received_size), 0);
            check_equal(received_channel, 6u);
            check_equal(received_size, sizeof(expected_payload));
            check_equal(received, expected_payload, sizeof(expected_payload));
            check_equal(adapter_wait_for_frames(runtime, 1u, &stats), 0);

            check_true(adapter_session_has_uuid_suffix(
                &publisher_session, RTSP_ADAPTER_SESSION_PREFIX));
            check_true(adapter_session_has_uuid_suffix(
                &player_session, RTSP_ADAPTER_SESSION_PREFIX));
            check_true(strcmp(publisher_session.id, player_session.id) != 0);
            check_equal(stats.source_count, 1u);
            check_equal(stats.frames_published, 1u);
            check_equal(stats.tracks_registered, 2u);
            check_equal(stats.subscriptions_created, 1u);

            check_equal(turbo_rtsp_client_teardown(player), 0);
            check_equal(turbo_rtsp_client_teardown(publisher), 0);
            turbo_rtsp_client_destroy(player);
            turbo_rtsp_client_destroy(publisher);
            check_equal(turbo_media_server_runtime_get_stats(runtime, &stats), TURBO_MEDIA_OK);
            check_equal(stats.source_count, 0u);
            check_equal(stats.subscriptions_removed, 1u);
            check_equal(stats.sources_removed, 1u);

            turbo_media_server_rtsp_adapter_destroy(adapter);
            turbo_media_server_runtime_destroy(runtime);
        }
    }
}

#else

suite("turbo_media_server_rtsp_adapter") {
    group("disabled") {
        it("is not built when RTSP is disabled") { check_true(1); }
    }
}

#endif
