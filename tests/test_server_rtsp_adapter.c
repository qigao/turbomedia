#include "turbo_media_server.h"
#include "CoroNet/turbo_coro_context.h"
#include <tinytest.h>

#ifdef TURBO_MEDIA_HAS_RTSP

#include <stdio.h>
#include <string.h>

#define RTSP_ADAPTER_E2E_PORT 20611
#define RTSP_ADAPTER_E2E_URI "rtsp://127.0.0.1:20611/live/cam"
#define RTSP_ADAPTER_INTERLEAVED_HEADER_SIZE 4u

#define REQUIRE_OK(expr)                    \
    do {                                    \
        int rc__ = (expr);                  \
        check_int_eq(rc__, TURBO_MEDIA_OK); \
        if (rc__ != TURBO_MEDIA_OK) return; \
    } while (0)

#define REQUIRE_NOT_NULL(expr) \
    do {                       \
        check_not_null(expr);  \
        if (!(expr)) return;   \
    } while (0)

typedef struct {
    coro_context_t *ctx;
    turbo_media_server_runtime_t *runtime;
    int publisher_ready;
    int player_ready;
    int player_received;
    int publisher_done;
    int player_done;
    int failed;
    int failed_line;
    uint64_t source_count_during_stream;
    uint64_t source_count_after_close;
    uint8_t received_payload[16];
    size_t received_payload_len;
} rtsp_adapter_e2e_state_t;

static turbo_media_server_config_t adapter_server_config(void) {
    turbo_media_server_config_t config;
    memset(&config, 0, sizeof(config));
    config.max_sources = 4;
    config.source_config.max_tracks = 8;
    config.source_config.max_subscribers = 4;
    config.source_config.gop_capacity = 4;
    return config;
}

static int adapter_recv_response(coro_socket_t *client,
                                 char *buffer,
                                 size_t buffer_size) {
    size_t total = 0;

    if (!client || !buffer || buffer_size == 0) return -1;

    while (total + 1 < buffer_size) {
        char *chunk = NULL;
        size_t chunk_len = 0;

        if (coro_socket_recv(client, &chunk, &chunk_len) != 0 || !chunk) {
            return -1;
        }
        if (chunk_len >= buffer_size - total) {
            coro_socket_free_recv(chunk);
            return -1;
        }

        memcpy(buffer + total, chunk, chunk_len);
        total += chunk_len;
        buffer[total] = '\0';
        coro_socket_free_recv(chunk);

        if (strstr(buffer, "\r\n\r\n")) {
            return strstr(buffer, "RTSP/1.0 200 OK") ? 0 : -1;
        }
    }

    return -1;
}

static int adapter_recv_interleaved(coro_socket_t *client,
                                    uint8_t *payload,
                                    size_t payload_size,
                                    size_t *payload_len) {
    uint8_t frame[64];
    size_t total = 0;

    if (!client || !payload || !payload_len) return -1;
    *payload_len = 0;

    while (total < sizeof(frame)) {
        char *chunk = NULL;
        size_t chunk_len = 0;
        size_t needed = 0;

        if (coro_socket_recv(client, &chunk, &chunk_len) != 0 || !chunk) {
            return -1;
        }
        if (chunk_len > sizeof(frame) - total) {
            coro_socket_free_recv(chunk);
            return -1;
        }

        memcpy(frame + total, chunk, chunk_len);
        total += chunk_len;
        coro_socket_free_recv(chunk);

        if (total >= RTSP_ADAPTER_INTERLEAVED_HEADER_SIZE) {
            if (frame[0] != '$') return -1;
            needed = RTSP_ADAPTER_INTERLEAVED_HEADER_SIZE +
                     (((size_t)frame[2] << 8) | (size_t)frame[3]);
            if (needed > sizeof(frame) || needed - RTSP_ADAPTER_INTERLEAVED_HEADER_SIZE > payload_size) {
                return -1;
            }
            if (total >= needed) {
                *payload_len = needed - RTSP_ADAPTER_INTERLEAVED_HEADER_SIZE;
                memcpy(payload, frame + RTSP_ADAPTER_INTERLEAVED_HEADER_SIZE, *payload_len);
                return 0;
            }
        }
    }

    return -1;
}

static int adapter_wait_flag(coro_context_t *ctx, const int *flag) {
    int wait_iters = 3000;

    while (!*flag && wait_iters-- > 0) {
        coro_sleep(ctx, 1);
    }

    return *flag ? 0 : -1;
}

static int adapter_send_checked(coro_socket_t *client, const char *data) {
    return coro_socket_send(client, data, strlen(data));
}

static void adapter_e2e_fail(rtsp_adapter_e2e_state_t *state, int line) {
    state->failed = 1;
    state->failed_line = line;
    coro_context_stop(state->ctx);
}

static void adapter_publisher_task(coro_t *co, void *arg) {
    static const uint8_t rtp_payload[] = {0x80, 0x60, 0x00, 0x01, 0xaa, 0xbb, 0xcc, 0xdd};
    rtsp_adapter_e2e_state_t *state = (rtsp_adapter_e2e_state_t *)arg;
    coro_socket_t *client = NULL;
    turbo_media_server_stats_t stats;
    char response[1024];
    char request[1024];
    int sdp_len;

    (void)co;
    client = coro_socket_create_tcpv4(state->ctx);
    if (!client) {
        adapter_e2e_fail(state, __LINE__);
        return;
    }
    coro_socket_set_timeout(client, 3000);

    if (coro_socket_connect(client, "127.0.0.1", RTSP_ADAPTER_E2E_PORT) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }

    sdp_len = (int)strlen(
        "v=0\r\n"
        "s=TurboMedia\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=0\r\n");
    snprintf(
        request,
        sizeof(request),
        "ANNOUNCE " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "v=0\r\n"
        "s=TurboMedia\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=0\r\n",
        sdp_len);

    if (adapter_send_checked(client, request) != 0 ||
        adapter_recv_response(client, response, sizeof(response)) != 0 ||
        adapter_send_checked(
            client,
            "SETUP " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
            "CSeq: 2\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response)) != 0 ||
        adapter_send_checked(
            client,
            "RECORD " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
            "CSeq: 3\r\n"
            "Session: adapter-e2e\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response)) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }

    state->publisher_ready = 1;
    if (adapter_wait_flag(state->ctx, &state->player_ready) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }

    if (coro_socket_send(client, "$\x00\x00\x08", RTSP_ADAPTER_INTERLEAVED_HEADER_SIZE) != 0 ||
        coro_socket_send(client, (const char *)rtp_payload, sizeof(rtp_payload)) != 0 ||
        adapter_wait_flag(state->ctx, &state->player_received) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }

    if (turbo_media_server_runtime_get_stats(state->runtime, &stats) == TURBO_MEDIA_OK) {
        state->source_count_during_stream = stats.source_count;
    }

    if (adapter_send_checked(
            client,
            "TEARDOWN " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
            "CSeq: 4\r\n"
            "Session: adapter-e2e\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response)) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }

    coro_socket_destroy(client);
    state->publisher_done = 1;
}

static void adapter_player_task(coro_t *co, void *arg) {
    rtsp_adapter_e2e_state_t *state = (rtsp_adapter_e2e_state_t *)arg;
    coro_socket_t *client = NULL;
    char response[2048];

    (void)co;
    if (adapter_wait_flag(state->ctx, &state->publisher_ready) != 0) {
        adapter_e2e_fail(state, __LINE__);
        return;
    }

    client = coro_socket_create_tcpv4(state->ctx);
    if (!client) {
        adapter_e2e_fail(state, __LINE__);
        return;
    }
    coro_socket_set_timeout(client, 3000);

    if (coro_socket_connect(client, "127.0.0.1", RTSP_ADAPTER_E2E_PORT) != 0 ||
        adapter_send_checked(
            client,
            "DESCRIBE " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
            "CSeq: 1\r\n"
            "Accept: application/sdp\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response)) != 0 ||
        strstr(response, "m=application 0 RTP/AVP 0") == NULL ||
        adapter_send_checked(
            client,
            "SETUP " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
            "CSeq: 2\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=play\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response)) != 0 ||
        adapter_send_checked(
            client,
            "PLAY " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
            "CSeq: 3\r\n"
            "Session: adapter-e2e\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response)) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }

    state->player_ready = 1;
    if (adapter_recv_interleaved(
            client,
            state->received_payload,
            sizeof(state->received_payload),
            &state->received_payload_len) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }

    state->player_received = 1;
    (void)adapter_send_checked(
        client,
        "TEARDOWN " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
        "CSeq: 4\r\n"
        "Session: adapter-e2e\r\n"
        "\r\n");
    (void)adapter_recv_response(client, response, sizeof(response));
    coro_socket_destroy(client);
    state->player_done = 1;
}

suite("turbo_media_server_rtsp_adapter") {
    section("lifecycle") {
        it("creates and starts an RTSP adapter on top of ServerRuntime") {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_media_server_config_t server_config = adapter_server_config();
            turbo_media_server_runtime_t *runtime;
            turbo_rtsp_server_config_t rtsp_config;
            turbo_media_rtsp_server_adapter_config_t adapter_config;
            turbo_media_rtsp_server_adapter_t *adapter;

            REQUIRE_NOT_NULL(ctx);
            server_config.coro_context = (struct coro_context_s *)ctx;
            runtime = turbo_media_server_runtime_create(&server_config);
            REQUIRE_NOT_NULL(runtime);

            memset(&rtsp_config, 0, sizeof(rtsp_config));
            rtsp_config.bind_host = "127.0.0.1";
            rtsp_config.port = 20610;
            rtsp_config.client_timeout_ms = 1000;

            memset(&adapter_config, 0, sizeof(adapter_config));
            adapter_config.vhost = "default";
            adapter_config.session_id = "adapter-test";
            adapter_config.default_rtp_channel_count = 2;
            adapter_config.remove_source_on_close = 1;
            adapter_config.replay_cached = 1;

            adapter = turbo_media_server_rtsp_adapter_create(
                runtime,
                (struct coro_context_s *)ctx,
                &rtsp_config,
                &adapter_config);
            REQUIRE_NOT_NULL(adapter);
            check_not_null(turbo_media_server_rtsp_adapter_rtsp_server(adapter));

            REQUIRE_OK(turbo_media_server_rtsp_adapter_start(adapter));
            REQUIRE_OK(turbo_media_server_rtsp_adapter_stop(adapter));
            REQUIRE_OK(turbo_media_server_rtsp_adapter_stop(adapter));

            turbo_media_server_rtsp_adapter_destroy(adapter);
            turbo_media_server_runtime_destroy(runtime);
            coro_context_destroy(ctx);
        }
    }

    section("end-to-end") {
        it("publishes and plays one RTP frame through MediaRegistry") {
            static const uint8_t expected_payload[] = {0x80, 0x60, 0x00, 0x01, 0xaa, 0xbb, 0xcc, 0xdd};
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_media_server_config_t server_config = adapter_server_config();
            turbo_media_server_runtime_t *runtime;
            turbo_rtsp_server_config_t rtsp_config;
            turbo_media_rtsp_server_adapter_config_t adapter_config;
            turbo_media_rtsp_server_adapter_t *adapter;
            rtsp_adapter_e2e_state_t state;
            turbo_media_server_stats_t stats;
            int wait_iters = 8000;

            memset(&state, 0, sizeof(state));
            memset(&stats, 0, sizeof(stats));
            REQUIRE_NOT_NULL(ctx);
            server_config.coro_context = (struct coro_context_s *)ctx;
            runtime = turbo_media_server_runtime_create(&server_config);
            REQUIRE_NOT_NULL(runtime);

            memset(&rtsp_config, 0, sizeof(rtsp_config));
            rtsp_config.bind_host = "127.0.0.1";
            rtsp_config.port = RTSP_ADAPTER_E2E_PORT;
            rtsp_config.client_timeout_ms = 1000;

            memset(&adapter_config, 0, sizeof(adapter_config));
            adapter_config.vhost = "default";
            adapter_config.session_id = "adapter-e2e";
            adapter_config.default_rtp_channel_count = 2;
            adapter_config.remove_source_on_close = 1;
            adapter_config.replay_cached = 0;

            adapter = turbo_media_server_rtsp_adapter_create(
                runtime,
                (struct coro_context_s *)ctx,
                &rtsp_config,
                &adapter_config);
            REQUIRE_NOT_NULL(adapter);
            REQUIRE_OK(turbo_media_server_rtsp_adapter_start(adapter));

            state.ctx = ctx;
            state.runtime = runtime;
            check_int_eq(coro_context_spawn(ctx, adapter_publisher_task, &state), 0);
            check_int_eq(coro_context_spawn(ctx, adapter_player_task, &state), 0);

            while (!state.failed &&
                   !(state.publisher_done && state.player_done) &&
                   wait_iters-- > 0) {
                coro_context_run(ctx, TURBO_RUN_ONCE);
            }

            (void)turbo_media_server_runtime_get_stats(runtime, &stats);
            state.source_count_after_close = stats.source_count;

            check_int_eq(state.failed_line, 0);
            check_false(state.failed);
            check_true(state.publisher_done);
            check_true(state.player_done);
            check_size_eq(state.received_payload_len, sizeof(expected_payload));
            check_mem_eq(state.received_payload, expected_payload, sizeof(expected_payload));
            check_int_eq((int)state.source_count_during_stream, 1);
            check_int_eq((int)state.source_count_after_close, 0);
            check_int_eq((int)stats.frames_published, 1);
            check_int_eq((int)stats.subscriptions_created, 1);
            check_int_eq((int)stats.subscriptions_removed, 1);
            check_int_eq((int)stats.sources_removed, 1);

            turbo_media_server_rtsp_adapter_destroy(adapter);
            turbo_media_server_runtime_destroy(runtime);
            coro_context_destroy(ctx);
        }
    }
}

#else

suite("turbo_media_server_rtsp_adapter") {
    section("disabled") {
        it("is not built when RTSP is disabled") {
            check_true(1);
        }
    }
}

#endif
