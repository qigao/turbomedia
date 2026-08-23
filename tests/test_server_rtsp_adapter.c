#include "turbo_media_server.h"
#include "CoroNet/turbo_coro_context.h"
#include "turbo_uuid.h"
#include <tinytest.h>

#ifdef TURBO_MEDIA_HAS_RTSP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RTSP_ADAPTER_E2E_PORT 20611
#define RTSP_ADAPTER_E2E_URI "rtsp://127.0.0.1:20611/live/cam"
#define RTSP_ADAPTER_INTERLEAVED_HEADER_SIZE 4u

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
    uint8_t received_channel;
    char publisher_session[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
    char player_session[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
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
                                 size_t buffer_size,
                                 int expected_status) {
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

        {
            char *header_end = strstr(buffer, "\r\n\r\n");
            size_t content_length = 0;
            char *content_length_header;
            if (!header_end) continue;
            content_length_header = strstr(buffer, "Content-Length:");
            if (content_length_header && content_length_header < header_end) {
                content_length = (size_t)strtoul(
                    content_length_header + strlen("Content-Length:"),
                    NULL,
                    10);
            }
            if (total >= (size_t)(header_end + 4 - buffer) + content_length) {
                int status = 0;
                return sscanf(buffer, "RTSP/1.0 %d", &status) == 1 &&
                               status == expected_status
                           ? 0
                           : -1;
            }
        }
    }

    return -1;
}

static int adapter_extract_session(const char *response,
                                   char *session_id,
                                   size_t session_id_size) {
    const char *start = strstr(response, "\r\nSession:");
    const char *end;
    size_t length;

    if (!start || !session_id || session_id_size == 0) return -1;
    start += strlen("\r\nSession:");
    while (*start == ' ' || *start == '\t') start++;
    end = strstr(start, "\r\n");
    if (!end) return -1;
    length = (size_t)(end - start);
    if (length == 0 || length >= session_id_size) return -1;
    memcpy(session_id, start, length);
    session_id[length] = '\0';
    return 0;
}

static int adapter_session_has_uuid_suffix(const char *session_id,
                                           const char *prefix) {
    turbo_uuid_t uuid;
    size_t prefix_len;

    if (!session_id || !prefix) return 0;
    prefix_len = strlen(prefix);
    if (strncmp(session_id, prefix, prefix_len) != 0 ||
        session_id[prefix_len] != '-') {
        return 0;
    }
    return turbo_uuid_parse(session_id + prefix_len + 1, &uuid) == TURBO_OK;
}

static int adapter_recv_interleaved(coro_socket_t *client,
                                    uint8_t *channel,
                                    uint8_t *payload,
                                    size_t payload_size,
                                    size_t *payload_len) {
    uint8_t frame[64];
    size_t total = 0;

    if (!client || !channel || !payload || !payload_len) return -1;
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
                *channel = frame[1];
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
    static const uint8_t rtcp_payload[] = {0x80, 0xc8, 0x00, 0x01};
    static const uint8_t rtp_payload[] = {0x80, 0x61, 0x00, 0x01, 0xaa, 0xbb, 0xcc, 0xdd};
    rtsp_adapter_e2e_state_t *state = (rtsp_adapter_e2e_state_t *)arg;
    coro_socket_t *client = NULL;
    turbo_media_server_stats_t stats;
    char response[1024];
    char request[1024];
    char session_id[TURBO_RTSP_MAX_HEADER_VALUE_LEN];
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

    if (adapter_send_checked(
            client,
            "RECORD " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
            "CSeq: 1\r\n"
            "Session: missing\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response), 455) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }

    sdp_len = (int)strlen(
        "v=0\r\n"
        "s=TurboMedia\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=0\r\n"
        "m=audio 0 RTP/AVP 97\r\n"
        "a=rtpmap:97 OPUS/48000\r\n"
        "a=control:trackID=1\r\n");
    snprintf(
        request,
        sizeof(request),
        "ANNOUNCE " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "v=0\r\n"
        "s=TurboMedia\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=0\r\n"
        "m=audio 0 RTP/AVP 97\r\n"
        "a=rtpmap:97 OPUS/48000\r\n"
        "a=control:trackID=1\r\n",
        sdp_len);

    if (adapter_send_checked(client, request) != 0 ||
        adapter_recv_response(client, response, sizeof(response), 200) != 0 ||
        adapter_send_checked(
            client,
            "SETUP " RTSP_ADAPTER_E2E_URI "/trackID=0 RTSP/1.0\r\n"
            "CSeq: 3\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=record\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response), 200) != 0 ||
        adapter_extract_session(response, session_id, sizeof(session_id)) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }
    snprintf(request,
             sizeof(request),
             "SETUP " RTSP_ADAPTER_E2E_URI "/trackID=1 RTSP/1.0\r\n"
             "CSeq: 4\r\n"
             "Session: wrong-session\r\n"
             "Transport: RTP/AVP/TCP;unicast;interleaved=2-3;mode=record\r\n"
             "\r\n");
    if (adapter_send_checked(client, request) != 0 ||
        adapter_recv_response(client, response, sizeof(response), 454) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }
    snprintf(request,
             sizeof(request),
             "SETUP " RTSP_ADAPTER_E2E_URI "/trackID=1 RTSP/1.0\r\n"
             "CSeq: 5\r\n"
             "Session: %s\r\n"
             "Transport: RTP/AVP/TCP;unicast;interleaved=2-3;mode=record\r\n"
             "\r\n",
             session_id);
    if (adapter_send_checked(client, request) != 0 ||
        adapter_recv_response(client, response, sizeof(response), 200) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }
    snprintf(state->publisher_session,
             sizeof(state->publisher_session),
             "%s",
             session_id);
    snprintf(request,
             sizeof(request),
             "RECORD " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
             "CSeq: 6\r\n"
             "Session: %s\r\n"
             "\r\n",
             session_id);
    if (adapter_send_checked(client, request) != 0 ||
        adapter_recv_response(client, response, sizeof(response), 200) != 0) {
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

    if (coro_socket_send(client, "$\x01\x00\x04", RTSP_ADAPTER_INTERLEAVED_HEADER_SIZE) != 0 ||
        coro_socket_send(client, (const char *)rtcp_payload, sizeof(rtcp_payload)) != 0 ||
        coro_socket_send(client, "$\x02\x00\x08", RTSP_ADAPTER_INTERLEAVED_HEADER_SIZE) != 0 ||
        coro_socket_send(client, (const char *)rtp_payload, sizeof(rtp_payload)) != 0 ||
        adapter_wait_flag(state->ctx, &state->player_received) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }

    if (turbo_media_server_runtime_get_stats(state->runtime, &stats) == TURBO_MEDIA_OK) {
        state->source_count_during_stream = stats.source_count;
    }

    snprintf(request,
             sizeof(request),
             "TEARDOWN " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
             "CSeq: 7\r\n"
             "Session: %s\r\n"
             "\r\n",
             session_id);
    if (adapter_send_checked(client, request) != 0 ||
        adapter_recv_response(client, response, sizeof(response), 200) != 0) {
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
    char request[1024];
    char session_id[TURBO_RTSP_MAX_HEADER_VALUE_LEN];

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
            "PLAY " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
            "CSeq: 1\r\n"
            "Session: missing\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response), 455) != 0 ||
        adapter_send_checked(
            client,
            "SETUP " RTSP_ADAPTER_E2E_URI "/trackID=0 RTSP/1.0\r\n"
            "CSeq: 2\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=4-5;mode=play\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response), 455) != 0 ||
        adapter_send_checked(
            client,
            "DESCRIBE " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
            "CSeq: 3\r\n"
            "Accept: application/sdp\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response), 200) != 0 ||
        strstr(response, "m=video 0 RTP/AVP 96") == NULL ||
        strstr(response, "a=rtpmap:96 H264/90000") == NULL ||
        strstr(response, "m=audio 0 RTP/AVP 97") == NULL ||
        strstr(response, "a=rtpmap:97 OPUS/48000") == NULL ||
        adapter_send_checked(
            client,
            "SETUP " RTSP_ADAPTER_E2E_URI "/trackID=0 RTSP/1.0\r\n"
            "CSeq: 4\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=4-5;mode=play\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response), 200) != 0 ||
        adapter_extract_session(response, session_id, sizeof(session_id)) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }
    snprintf(request,
             sizeof(request),
             "SETUP " RTSP_ADAPTER_E2E_URI "/trackID=1 RTSP/1.0\r\n"
             "CSeq: 5\r\n"
             "Session: wrong-session\r\n"
             "Transport: RTP/AVP/TCP;unicast;interleaved=6-7;mode=play\r\n"
             "\r\n");
    if (adapter_send_checked(client, request) != 0 ||
        adapter_recv_response(client, response, sizeof(response), 454) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }
    snprintf(request,
             sizeof(request),
             "SETUP " RTSP_ADAPTER_E2E_URI "/trackID=1 RTSP/1.0\r\n"
             "CSeq: 6\r\n"
             "Session: %s\r\n"
             "Transport: RTP/AVP/TCP;unicast;interleaved=6-7;mode=play\r\n"
             "\r\n",
             session_id);
    if (adapter_send_checked(client, request) != 0 ||
        adapter_recv_response(client, response, sizeof(response), 200) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }
    snprintf(state->player_session,
             sizeof(state->player_session),
             "%s",
             session_id);
    if (adapter_send_checked(
            client,
            "PLAY " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
            "CSeq: 7\r\n"
            "Session: wrong-session\r\n"
            "\r\n") != 0 ||
        adapter_recv_response(client, response, sizeof(response), 454) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }
    snprintf(request,
             sizeof(request),
             "PLAY " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
             "CSeq: 8\r\n"
             "Session: %s\r\n"
             "\r\n",
             session_id);
    if (adapter_send_checked(client, request) != 0 ||
        adapter_recv_response(client, response, sizeof(response), 200) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }

    state->player_ready = 1;
    if (adapter_recv_interleaved(
            client,
            &state->received_channel,
            state->received_payload,
            sizeof(state->received_payload),
            &state->received_payload_len) != 0) {
        adapter_e2e_fail(state, __LINE__);
        coro_socket_destroy(client);
        return;
    }

    state->player_received = 1;
    snprintf(request,
             sizeof(request),
             "TEARDOWN " RTSP_ADAPTER_E2E_URI " RTSP/1.0\r\n"
             "CSeq: 9\r\n"
             "Session: %s\r\n"
             "\r\n",
             session_id);
    (void)adapter_send_checked(client, request);
    (void)adapter_recv_response(client, response, sizeof(response), 200);
    coro_socket_destroy(client);
    state->player_done = 1;
}

suite("turbo_media_server_rtsp_adapter") {
    group("lifecycle") {
        it("creates and starts an RTSP adapter on top of ServerRuntime") {
            coro_context_t *ctx = coro_context_create(NULL);
            turbo_media_server_config_t server_config = adapter_server_config();
            turbo_media_server_runtime_t *runtime;
            turbo_rtsp_server_config_t rtsp_config;
            turbo_media_rtsp_server_adapter_config_t adapter_config;
            turbo_media_rtsp_server_adapter_t *adapter;

            check_not_null(ctx);
            server_config.coro_context = (struct coro_context_s *)ctx;
            runtime = turbo_media_server_runtime_create(&server_config);
            check_not_null(runtime);

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
            check_not_null(adapter);
            check_not_null(turbo_media_server_rtsp_adapter_rtsp_server(adapter));

            check_equal(turbo_media_server_rtsp_adapter_start(adapter), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_rtsp_adapter_stop(adapter), TURBO_MEDIA_OK);
            check_equal(turbo_media_server_rtsp_adapter_stop(adapter), TURBO_MEDIA_OK);

            turbo_media_server_rtsp_adapter_destroy(adapter);
            turbo_media_server_runtime_destroy(runtime);
            coro_context_destroy(ctx);
        }
    }

    group("end-to-end") {
        it("publishes and plays one RTP frame through MediaRegistry") {
            static const uint8_t expected_payload[] = {0x80, 0x61, 0x00, 0x01, 0xaa, 0xbb, 0xcc, 0xdd};
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
            check_not_null(ctx);
            server_config.coro_context = (struct coro_context_s *)ctx;
            runtime = turbo_media_server_runtime_create(&server_config);
            check_not_null(runtime);

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
            check_not_null(adapter);
            check_equal(turbo_media_server_rtsp_adapter_start(adapter), TURBO_MEDIA_OK);

            state.ctx = ctx;
            state.runtime = runtime;
            check_equal(coro_context_spawn(ctx, adapter_publisher_task, &state), 0);
            check_equal(coro_context_spawn(ctx, adapter_player_task, &state), 0);

            while (!state.failed && wait_iters-- > 0) {
                if (state.publisher_done && state.player_done) {
                    (void)turbo_media_server_runtime_get_stats(runtime, &stats);
                    if (stats.subscriptions_removed == 1 &&
                        stats.sources_removed == 1) {
                        break;
                    }
                }
                coro_context_run(ctx, TURBO_RUN_ONCE);
            }

            (void)turbo_media_server_runtime_get_stats(runtime, &stats);
            state.source_count_after_close = stats.source_count;

            check_equal(state.failed_line, 0);
            check_false(state.failed);
            check_true(state.publisher_done);
            check_true(state.player_done);
            check_equal(state.received_payload_len, sizeof(expected_payload));
            check_equal(state.received_payload, expected_payload, sizeof(expected_payload));
            check_equal(state.received_channel, 6);
            check_true(state.publisher_session[0] != '\0');
            check_true(state.player_session[0] != '\0');
            check_true(adapter_session_has_uuid_suffix(
                state.publisher_session,
                "adapter-e2e"));
            check_true(adapter_session_has_uuid_suffix(
                state.player_session,
                "adapter-e2e"));
            check_true(strcmp(state.publisher_session, state.player_session) != 0);
            check_equal((int)state.source_count_during_stream, 1);
            check_equal((int)state.source_count_after_close, 0);
            check_equal((int)stats.frames_published, 1);
            check_equal((int)stats.tracks_registered, 2);
            check_equal((int)stats.subscriptions_created, 1);
            check_equal((int)stats.subscriptions_removed, 1);
            check_equal((int)stats.sources_removed, 1);

            turbo_media_server_rtsp_adapter_destroy(adapter);
            turbo_media_server_runtime_destroy(runtime);
            coro_context_destroy(ctx);
        }
    }
}

#else

suite("turbo_media_server_rtsp_adapter") {
    group("disabled") {
        it("is not built when RTSP is disabled") {
            check_true(1);
        }
    }
}

#endif
