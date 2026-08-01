#include "turbo_rtsp.h"
#include "turbo_rtsp_parser.h"
#include "turbo_rtsp_rtp.h"
#include <tinytest.h>

#ifdef TURBO_MEDIA_HAS_RTSP

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <ctype.h>

static int g_failed = 0;
static unsigned char g_large_interleaved_payload[UINT16_MAX];
static const uint8_t RTSP_KCP_TEST_PSK[TURBO_KCP_PSK_SIZE] = {
    0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98,
    0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f, 0x10,
    0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x97, 0xa8,
    0xb9, 0xca, 0xdb, 0xec, 0xfd, 0x0e, 0x1f, 0x20
};

#define CHECK_TRUE(expr)                                                        \
    do {                                                                       \
        check(expr);                                                           \
        if (!(expr)) {                                                         \
            g_failed = 1;                                                      \
            return;                                                            \
        }                                                                      \
    } while (0)

#define CHECK_INT(expected, actual)                                             \
    do {                                                                       \
        int expected_value = (expected);                                        \
        int actual_value = (actual);                                            \
        check_int_eq(actual_value, expected_value);                             \
        if (expected_value != actual_value) {                                   \
            g_failed = 1;                                                      \
            return;                                                            \
        }                                                                      \
    } while (0)

#define UNUSED(x) (void)(x)

static turbo_kcp_config_t rtsp_kcp_test_config(void) {
    turbo_kcp_config_t config;

    turbo_kcp_config_default(&config);
    memcpy(config.pre_shared_key, RTSP_KCP_TEST_PSK, sizeof(config.pre_shared_key));
    return config;
}

typedef struct {
    coro_context_t *ctx;
    turbo_rtsp_server_t *server;
    coro_socket_t *raw_listener;
    uint32_t rtp_timestamp;
    uint32_t au_timestamp;
    uint32_t expected_h264_ssrc;
    uint16_t expected_h264_initial_sequence;
    uint32_t expected_h264_initial_timestamp;
    int use_configured_h264_rtp_state;
    int describe_called;
    int setup_called;
    int set_parameter_called;
    int interleaved_called;
    int au_interleaved_count;
    int rtcp_called;
    int announce_called;
    int record_called;
    int teardown_called;
    int bad_cseq_rejected;
    int options_called;
    int client_describe_called;
    int client_setup_called;
    int client_play_called;
    int client_pause_called;
    int client_teardown_called;
    int client_get_parameter_called;
    int client_set_parameter_called;
    int recv_frame_called;
    int redirect_called;
    int auth_challenge_sent;
    int auth_authorized;
    int auth_rejected;
    int idle_connected;
    int session_closed;
    int completed;
} rtsp_test_state_t;

static void rtsp_test_destroy_raw_listener(rtsp_test_state_t *state) {
    if (!state || !state->ctx || !state->raw_listener) {
        return;
    }

    (void)coro_socket_server_stop(state->raw_listener);
    while (!coro_socket_server_is_stopped(state->raw_listener)) {
        coro_context_run(state->ctx, TURBO_RUN_ONCE);
    }
    coro_socket_destroy(state->raw_listener);
    state->raw_listener = NULL;
}

static int rtsp_response_complete(const char *data, size_t len) {
    const char *header_end = strstr(data, "\r\n\r\n");
    const char *content_length = NULL;
    size_t header_len = 0;
    size_t body_len = 0;

    if (!header_end) {
        return 0;
    }

    header_len = (size_t)(header_end - data) + 4;
    content_length = strstr(data, "Content-Length:");
    if (!content_length || content_length > header_end) {
        return 1;
    }

    if (sscanf(content_length, "Content-Length: %zu", &body_len) != 1) {
        return 0;
    }

    return len >= header_len + body_len;
}

static int rtsp_recv_response(coro_socket_t *client,
                              char *buffer,
                              size_t buffer_size,
                              size_t *out_len) {
    size_t total = 0;

    if (!client || !buffer || buffer_size == 0 || !out_len) {
        return -1;
    }

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

        if (rtsp_response_complete(buffer, total)) {
            *out_len = total;
            return 0;
        }
    }

    return -1;
}

static int rtsp_recv_interleaved_frame(coro_socket_t *client,
                                       unsigned char *buffer,
                                       size_t buffer_size,
                                       size_t *out_len) {
    size_t total = 0;
    size_t expected = 0;

    if (!client || !buffer || buffer_size < 4 || !out_len) {
        return -1;
    }

    while (total < buffer_size) {
        char *chunk = NULL;
        size_t chunk_len = 0;

        if (coro_socket_recv(client, &chunk, &chunk_len) != 0 || !chunk) {
            return -1;
        }
        if (chunk_len > buffer_size - total) {
            coro_socket_free_recv(chunk);
            return -1;
        }

        memcpy(buffer + total, chunk, chunk_len);
        total += chunk_len;
        coro_socket_free_recv(chunk);

        if (total >= 4 && expected == 0) {
            if (buffer[0] != '$') {
                return -1;
            }
            expected = 4u + (((size_t)buffer[2] << 8) | (size_t)buffer[3]);
            if (expected > buffer_size) {
                return -1;
            }
        }
        if (expected > 0 && total >= expected) {
            *out_len = total;
            return 0;
        }
    }

    return -1;
}

static int on_describe(turbo_rtsp_session_t *session,
                       const turbo_rtsp_request_t *request,
                       turbo_rtsp_response_t *response,
                       void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    static const char sdp_body[] =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=TurboMedia\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=control:trackID=0\r\n";

    UNUSED(session);
    if (request->method != TURBO_RTSP_METHOD_DESCRIBE) {
        return -1;
    }
    if (strstr(request->uri, "rtsp://127.0.0.1:20554/live") == NULL) {
        return -1;
    }

    state->describe_called = 1;
    return turbo_rtsp_response_describe(response, sdp_body, sizeof(sdp_body) - 1);
}

static int on_setup(turbo_rtsp_session_t *session,
                    const turbo_rtsp_request_t *request,
                    turbo_rtsp_response_t *response,
                    void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_SETUP) {
        return -1;
    }
    if (request->transport_kind != TURBO_RTSP_TRANSPORT_RTP_AVP_TCP) {
        return -1;
    }
    if (request->interleaved_rtp_channel != 0 ||
        request->interleaved_rtcp_channel != 1) {
        return -1;
    }

    state->setup_called = 1;
    return turbo_rtsp_response_setup(
        response,
        "session-1",
        "RTP/AVP/TCP;unicast;interleaved=0-1");
}

static int on_set_parameter(turbo_rtsp_session_t *session,
                            const turbo_rtsp_request_t *request,
                            turbo_rtsp_response_t *response,
                            void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    const turbo_rtsp_message_t *message = NULL;
    const turbo_rtsp_header_view_t *require_header = NULL;
    if (request->method != TURBO_RTSP_METHOD_SET_PARAMETER) {
        return -1;
    }

    message = turbo_rtsp_session_get_last_message(session);
    if (!message) {
        return -1;
    }

    require_header = turbo_rtsp_message_find_header(message, "require");
    if (!require_header ||
        require_header->value_len != strlen("org.turbonet.test") ||
        memcmp(require_header->value, "org.turbonet.test", require_header->value_len) != 0) {
        return -1;
    }
    if (message->body_len != strlen("volume: 0.75\r\n") ||
        memcmp(message->body, "volume: 0.75\r\n", message->body_len) != 0) {
        return -1;
    }

    state->set_parameter_called = 1;
    return turbo_rtsp_response_set_parameter(response);
}

static int on_idle_options(turbo_rtsp_session_t *session,
                           const turbo_rtsp_request_t *request,
                           turbo_rtsp_response_t *response,
                           void *user_data) {
    UNUSED(session);
    UNUSED(request);
    UNUSED(user_data);
    return turbo_rtsp_response_options(response, NULL);
}

static void on_idle_session_close(turbo_rtsp_session_t *session, void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);
    state->session_closed = 1;
}

static void rtsp_idle_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    coro_socket_t *client = coro_socket_create_tcpv4(state->ctx);
    char response[512];
    size_t response_len = 0;
    char *chunk = NULL;
    size_t chunk_len = 0;

    UNUSED(co);
    if (!client) {
        g_failed = 1;
        return;
    }
    coro_socket_set_timeout(client, 2000);
    if (coro_socket_connect(client, "127.0.0.1", 20574) != 0 ||
        coro_socket_send(
            client,
            "OPTIONS rtsp://127.0.0.1:20574/idle RTSP/1.0\r\nCSeq: 1\r\n\r\n",
            strlen("OPTIONS rtsp://127.0.0.1:20574/idle RTSP/1.0\r\nCSeq: 1\r\n\r\n")) != 0 ||
        rtsp_recv_response(client, response, sizeof(response), &response_len) != 0) {
        g_failed = 1;
        coro_socket_destroy(client);
        return;
    }

    state->idle_connected = 1;
    if (coro_socket_recv(client, &chunk, &chunk_len) == 0 && chunk) {
        coro_socket_free_recv(chunk);
    }
    coro_socket_destroy(client);
    state->completed = 1;
}

static void rtsp_stalled_ws_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    coro_socket_t *client = coro_socket_create_tcpv4(state->ctx);
    char *chunk = NULL;
    size_t chunk_len = 0;
    static const char partial_handshake[] = "GET /rtsp HTTP/1.1\r\n";

    UNUSED(co);
    if (!client) {
        g_failed = 1;
        return;
    }
    coro_socket_set_timeout(client, 3000);
    if (coro_socket_connect(client, "127.0.0.1", 20575) != 0 ||
        coro_socket_send(client, partial_handshake, sizeof(partial_handshake) - 1) != 0) {
        g_failed = 1;
        coro_socket_destroy(client);
        return;
    }

    state->idle_connected = 1;
    if (coro_socket_recv(client, &chunk, &chunk_len) == 0 && chunk_len > 0) {
        g_failed = 1;
    }
    if (chunk) {
        coro_socket_free_recv(chunk);
    }
    coro_socket_destroy(client);
    state->completed = 1;
}

static int on_interleaved_frame(turbo_rtsp_session_t *session,
                                uint8_t channel,
                                const uint8_t *payload,
                                size_t payload_len,
                                void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    static const unsigned char expected_payload[] = {
        0x80, 0xe0, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x01, 0x02, 0x03, 0x04
    };
    UNUSED(session);

    if (channel != 0 || payload_len != sizeof(expected_payload) ||
        memcmp(payload, expected_payload, sizeof(expected_payload)) != 0) {
        return -1;
    }

    state->interleaved_called = 1;
    return turbo_rtsp_session_send_interleaved_frame(session, 1, payload, payload_len);
}

static int on_push_announce(turbo_rtsp_session_t *session,
                            const turbo_rtsp_request_t *request,
                            turbo_rtsp_response_t *response,
                            void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    const turbo_rtsp_message_t *message = NULL;
    const turbo_rtsp_header_view_t *content_type = NULL;
    static const char expected_sdp[] =
        "v=0\r\n"
        "o=- 2 2 IN IP4 127.0.0.1\r\n"
        "s=TurboMedia Push\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "a=range:npt=0-60.000\r\n"
        "a=sendonly\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=control:trackID=0\r\n"
        "m=audio 0 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=control:trackID=1\r\n";

    if (request->method != TURBO_RTSP_METHOD_ANNOUNCE) {
        return -1;
    }
    if (strcmp(request->uri, "rtsp://127.0.0.1:20555/push") != 0) {
        return -1;
    }

    message = turbo_rtsp_session_get_last_message(session);
    if (!message) {
        return -1;
    }

    content_type = turbo_rtsp_message_find_header(message, "content-type");
    if (!content_type ||
        content_type->value_len != strlen("application/sdp") ||
        memcmp(content_type->value, "application/sdp", content_type->value_len) != 0) {
        return -1;
    }
    if (message->body_len != sizeof(expected_sdp) - 1 ||
        memcmp(message->body, expected_sdp, sizeof(expected_sdp) - 1) != 0) {
        return -1;
    }

    state->announce_called = 1;
    return turbo_rtsp_response_announce(response);
}

static int on_push_setup(turbo_rtsp_session_t *session,
                         const turbo_rtsp_request_t *request,
                         turbo_rtsp_response_t *response,
                         void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_SETUP) {
        return -1;
    }
    if (strcmp(request->uri, "rtsp://127.0.0.1:20555/push/trackID=0") != 0 &&
        strcmp(request->uri, "rtsp://127.0.0.1:20555/push/trackID=1") != 0) {
        return -1;
    }
    if (request->transport_kind != TURBO_RTSP_TRANSPORT_RTP_AVP_TCP ||
        request->transport_spec.kind != TURBO_RTSP_TRANSPORT_RTP_AVP_TCP) {
        return -1;
    }
    if (request->transport_spec.mode != TURBO_RTSP_TRANSPORT_MODE_RECORD) {
        return -1;
    }
    if (request->interleaved_rtp_channel != 2 ||
        request->interleaved_rtcp_channel != 3 ||
        request->transport_spec.interleaved_rtp_channel != 2 ||
        request->transport_spec.interleaved_rtcp_channel != 3) {
        return -1;
    }

    state->setup_called = 1;
    return turbo_rtsp_response_setup(
        response,
        "push-session-1",
        "RTP/AVP/TCP;unicast;interleaved=2-3;mode=RECORD");
}

static int on_push_record(turbo_rtsp_session_t *session,
                          const turbo_rtsp_request_t *request,
                          turbo_rtsp_response_t *response,
                          void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_RECORD) {
        return -1;
    }
    if (strcmp(request->uri, "rtsp://127.0.0.1:20555/push") != 0) {
        return -1;
    }
    if (strcmp(request->session_id, "push-session-1") != 0 ||
        strcmp(request->session.id, "push-session-1") != 0) {
        return -1;
    }
    if (strcmp(request->range, "npt=0-") != 0 ||
        request->range_spec.type != TURBO_RTSP_RANGE_NPT ||
        !request->range_spec.has_start ||
        request->range_spec.start_ms != 0 ||
        request->range_spec.has_end) {
        return -1;
    }

    state->record_called = 1;
    return turbo_rtsp_response_record(response, "npt=0-");
}

static int on_push_teardown(turbo_rtsp_session_t *session,
                            const turbo_rtsp_request_t *request,
                            turbo_rtsp_response_t *response,
                            void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_TEARDOWN) {
        return -1;
    }
    if (strcmp(request->uri, "rtsp://127.0.0.1:20555/push") != 0) {
        return -1;
    }
    if (strcmp(request->session_id, "push-session-1") != 0 ||
        strcmp(request->session.id, "push-session-1") != 0) {
        return -1;
    }

    state->teardown_called = 1;
    return turbo_rtsp_response_teardown(response);
}

static int on_push_interleaved_frame(turbo_rtsp_session_t *session,
                                     uint8_t channel,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    turbo_rtsp_rtp_header_t header;
    size_t header_len = 0;
    static const unsigned char expected_nal[] = {0x65, 0x88, 0x84, 0x21};
    static const unsigned char expected_sps[] = {0x67, 0x42, 0x00, 0x1f};
    static const unsigned char expected_pps[] = {0x68, 0xce, 0x06, 0xe2};
    static const unsigned char expected_slice[] = {0x65, 0x11, 0x22, 0x33};
    const unsigned char *expected_au_payload = NULL;
    size_t expected_au_payload_len = 0;
    int expected_au_marker = 0;
    uint32_t expected_ssrc = state->use_configured_h264_rtp_state
                                 ? state->expected_h264_ssrc
                                 : 0x54525000u;
    uint16_t expected_initial_sequence = state->use_configured_h264_rtp_state
                                             ? state->expected_h264_initial_sequence
                                             : 1u;
    uint32_t expected_initial_timestamp = state->use_configured_h264_rtp_state
                                              ? state->expected_h264_initial_timestamp
                                              : 0u;

    if (channel == 3) {
        turbo_rtsp_rtcp_header_t rtcp_header;
        turbo_rtsp_rtcp_sender_info_t sender_info;
        uint32_t sdes_ssrc = 0;
        const char *cname = NULL;
        size_t cname_len = 0;
        size_t consumed = 0;
        size_t packet_count = 0;
        size_t block_count = 0;

        if (turbo_rtsp_rtcp_validate_compound(payload, payload_len, &packet_count) != 0 ||
            packet_count != 2 ||
            turbo_rtsp_rtcp_next_packet(payload, payload_len, &rtcp_header, &consumed) != 0 ||
            rtcp_header.packet_type != TURBO_RTSP_RTCP_SR ||
            turbo_rtsp_rtcp_parse_sender_report(
                payload,
                consumed,
                &sender_info,
                NULL,
                0,
                &block_count) != 0 ||
            block_count != 0 ||
            sender_info.ssrc != expected_ssrc ||
            sender_info.ntp_timestamp != 0x1112131421222324ull ||
            sender_info.rtp_timestamp != state->au_timestamp ||
            sender_info.packet_count != 4 ||
            sender_info.octet_count != sizeof(expected_nal) + sizeof(expected_sps) +
                                           sizeof(expected_pps) + sizeof(expected_slice) ||
            turbo_rtsp_rtcp_parse_sdes_cname(
                payload + consumed,
                payload_len - consumed,
                &sdes_ssrc,
                &cname,
                &cname_len) != 0 ||
            sdes_ssrc != expected_ssrc ||
            cname_len != strlen("push-cname") ||
            memcmp(cname, "push-cname", strlen("push-cname")) != 0) {
            return -1;
        }

        state->rtcp_called = 1;
        return 0;
    }

    if (channel == 2 &&
        turbo_rtsp_rtp_parse_header(payload, payload_len, &header, &header_len) == 0 &&
        header.sequence_number >= (uint16_t)(expected_initial_sequence + 1u) &&
        header.sequence_number <= (uint16_t)(expected_initial_sequence + 3u)) {
        if (header.sequence_number == (uint16_t)(expected_initial_sequence + 1u)) {
            expected_au_payload = expected_sps;
            expected_au_payload_len = sizeof(expected_sps);
            expected_au_marker = 0;
        } else if (header.sequence_number == (uint16_t)(expected_initial_sequence + 2u)) {
            expected_au_payload = expected_pps;
            expected_au_payload_len = sizeof(expected_pps);
            expected_au_marker = 0;
        } else {
            expected_au_payload = expected_slice;
            expected_au_payload_len = sizeof(expected_slice);
            expected_au_marker = 1;
        }

        if (header.payload_len != expected_au_payload_len ||
            memcmp(header.payload, expected_au_payload, expected_au_payload_len) != 0 ||
            header.version != 2 ||
            header.payload_type != 96 ||
            header.timestamp != state->au_timestamp ||
            header.marker != expected_au_marker ||
            header.ssrc != expected_ssrc ||
            header_len != TURBO_RTSP_RTP_HEADER_SIZE) {
            return -1;
        }

        ++state->au_interleaved_count;
        return 0;
    }

    if (channel != 2 ||
        turbo_rtsp_rtp_parse_header(payload, payload_len, &header, &header_len) != 0 ||
        header.payload_len != sizeof(expected_nal) ||
        memcmp(header.payload, expected_nal, sizeof(expected_nal)) != 0 ||
        header.version != 2 ||
        header.payload_type != 96 ||
        header.sequence_number != expected_initial_sequence ||
        header.timestamp != expected_initial_timestamp ||
        header.marker != 1 ||
        header.ssrc != expected_ssrc ||
        header_len != TURBO_RTSP_RTP_HEADER_SIZE) {
        return -1;
    }

    state->interleaved_called = 1;
    return turbo_rtsp_session_send_interleaved_frame(session, 3, payload, payload_len);
}

static int on_client_options(turbo_rtsp_session_t *session,
                             const turbo_rtsp_request_t *request,
                             turbo_rtsp_response_t *response,
                             void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_OPTIONS ||
        strcmp(request->uri, "rtsp://127.0.0.1:20557/live") != 0) {
        return -1;
    }

    state->options_called = 1;
    return turbo_rtsp_response_options(
        response,
        "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN");
}

static int on_url_open_options(turbo_rtsp_session_t *session,
                               const turbo_rtsp_request_t *request,
                               turbo_rtsp_response_t *response,
                               void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_OPTIONS ||
        strcmp(request->uri, "rtsp://127.0.0.1:20568/live") != 0) {
        return -1;
    }

    state->options_called = 1;
    return turbo_rtsp_response_options(response, "OPTIONS");
}

static int on_redirect_location(turbo_rtsp_session_t *session,
                                const turbo_rtsp_request_t *request,
                                turbo_rtsp_response_t *response,
                                void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_REDIRECT ||
        strcmp(request->uri, "rtsp://127.0.0.1:20569/live") != 0) {
        return -1;
    }

    state->redirect_called = 1;
    return turbo_rtsp_response_redirect(
        response,
        302,
        "rtsp://127.0.0.1:20569/redirected");
}

static int on_ws_control_options(turbo_rtsp_session_t *session,
                                 const turbo_rtsp_request_t *request,
                                 turbo_rtsp_response_t *response,
                                 void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_OPTIONS ||
        strcmp(request->uri, "rtsp://127.0.0.1:20562/live") != 0) {
        return -1;
    }

    state->options_called = 1;
    return turbo_rtsp_response_options(response, "OPTIONS, DESCRIBE");
}

static int on_kcp_control_options(turbo_rtsp_session_t *session,
                                  const turbo_rtsp_request_t *request,
                                  turbo_rtsp_response_t *response,
                                  void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_OPTIONS ||
        strcmp(request->uri, "rtsp://127.0.0.1:20563/live") != 0) {
        return -1;
    }

    state->options_called = 1;
    return turbo_rtsp_response_options(response, "OPTIONS, DESCRIBE");
}

static int on_client_describe(turbo_rtsp_session_t *session,
                              const turbo_rtsp_request_t *request,
                              turbo_rtsp_response_t *response,
                              void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    const turbo_rtsp_message_t *message = NULL;
    const turbo_rtsp_header_view_t *accept = NULL;
    static const char sdp_body[] =
        "v=0\r\n"
        "o=- 4 4 IN IP4 127.0.0.1\r\n"
        "s=TurboMedia Pull\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "a=range:npt=0-60.000\r\n"
        "a=recvonly\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "c=IN IP4 198.51.100.20\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=control:trackID=0\r\n";

    if (request->method != TURBO_RTSP_METHOD_DESCRIBE ||
        strcmp(request->uri, "rtsp://127.0.0.1:20557/live") != 0) {
        return -1;
    }

    message = turbo_rtsp_session_get_last_message(session);
    if (!message) {
        return -1;
    }
    accept = turbo_rtsp_message_find_header(message, "Accept");
    if (!accept ||
        accept->value_len != strlen("application/sdp") ||
        memcmp(accept->value, "application/sdp", accept->value_len) != 0) {
        return -1;
    }

    state->client_describe_called = 1;
    return turbo_rtsp_response_describe(response, sdp_body, sizeof(sdp_body) - 1);
}

static int on_client_setup(turbo_rtsp_session_t *session,
                           const turbo_rtsp_request_t *request,
                           turbo_rtsp_response_t *response,
                           void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_SETUP ||
        strcmp(request->uri, "rtsp://127.0.0.1:20557/live/trackID=0") != 0) {
        return -1;
    }
    if (request->transport_kind != TURBO_RTSP_TRANSPORT_RTP_AVP_TCP ||
        request->transport_spec.mode != TURBO_RTSP_TRANSPORT_MODE_PLAY ||
        request->transport_spec.interleaved_rtp_channel != 0 ||
        request->transport_spec.interleaved_rtcp_channel != 1) {
        return -1;
    }

    state->client_setup_called = 1;
    return turbo_rtsp_response_setup(
        response,
        "pull-session-1;timeout=60",
        "RTP/AVP/TCP;unicast;interleaved=0-1;mode=PLAY");
}

static int on_udp_describe(turbo_rtsp_session_t *session,
                           const turbo_rtsp_request_t *request,
                           turbo_rtsp_response_t *response,
                           void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    static const char sdp_body[] =
        "v=0\r\n"
        "o=- 5 5 IN IP4 127.0.0.1\r\n"
        "s=TurboMedia UDP Pull\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "a=range:npt=0-60.000\r\n"
        "a=recvonly\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=control:trackID=0\r\n"
        "m=audio 0 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=control:trackID=1\r\n";

    UNUSED(session);
    if (request->method != TURBO_RTSP_METHOD_DESCRIBE ||
        strcmp(request->uri, "rtsp://127.0.0.1:20564/live") != 0) {
        return -1;
    }

    state->client_describe_called = 1;
    return turbo_rtsp_response_describe(response, sdp_body, sizeof(sdp_body) - 1);
}

static int on_udp_setup(turbo_rtsp_session_t *session,
                        const turbo_rtsp_request_t *request,
                        turbo_rtsp_response_t *response,
                        void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;

    if (request->method != TURBO_RTSP_METHOD_SETUP ||
        strcmp(request->uri, "rtsp://127.0.0.1:20564/live/trackID=0") != 0) {
        return -1;
    }
    if (request->transport_spec.kind != TURBO_RTSP_TRANSPORT_RTP_AVP_UDP ||
        request->transport_spec.delivery != TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST ||
        request->transport_spec.mode != TURBO_RTSP_TRANSPORT_MODE_PLAY ||
        request->transport_spec.client_rtp_port <= 0 ||
        request->transport_spec.client_rtcp_port <= 0) {
        return -1;
    }

    if (turbo_rtsp_session_setup_udp_transport(
            session,
            response,
            "udp-session-1",
            "127.0.0.1",
            "127.0.0.1") != 0) {
        return -1;
    }

    state->client_setup_called = 1;
    return 0;
}

static int on_udp_play(turbo_rtsp_session_t *session,
                       const turbo_rtsp_request_t *request,
                       turbo_rtsp_response_t *response,
                       void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    turbo_rtsp_rtp_header_t parsed_header;
    uint8_t rtp_recv[128];
    static const uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};
    size_t header_len = 0;
    size_t recv_len = 0;

    if (request->method != TURBO_RTSP_METHOD_PLAY ||
        strcmp(request->uri, "rtsp://127.0.0.1:20564/live") != 0 ||
        strcmp(request->session_id, "udp-session-1") != 0) {
        return -1;
    }

    if (turbo_rtsp_session_recv_rtp_udp(session, rtp_recv, sizeof(rtp_recv), &recv_len) != 0 ||
        turbo_rtsp_rtp_parse_header(rtp_recv, recv_len, &parsed_header, &header_len) != 0 ||
        parsed_header.sequence_number != 123 ||
        parsed_header.payload_len != sizeof(payload) ||
        memcmp(parsed_header.payload, payload, sizeof(payload)) != 0) {
        return -1;
    }

    state->client_play_called = 1;
    return turbo_rtsp_response_play(response, "npt=0-", NULL);
}

static int on_client_play(turbo_rtsp_session_t *session,
                          const turbo_rtsp_request_t *request,
                          turbo_rtsp_response_t *response,
                          void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    const turbo_rtsp_message_t *message = NULL;
    const turbo_rtsp_header_view_t *speed = NULL;

    if (request->method != TURBO_RTSP_METHOD_PLAY ||
        strcmp(request->uri, "rtsp://127.0.0.1:20557/live") != 0 ||
        strcmp(request->session_id, "pull-session-1") != 0 ||
        strcmp(request->range, "npt=5-") != 0) {
        return -1;
    }

    message = turbo_rtsp_session_get_last_message(session);
    speed = message ? turbo_rtsp_message_find_header(message, "Speed") : NULL;
    if (!speed ||
        speed->value_len != strlen("1.0") ||
        memcmp(speed->value, "1.0", speed->value_len) != 0) {
        return -1;
    }

    state->client_play_called = 1;
    return turbo_rtsp_response_play(
        response,
        "npt=5-",
        "url=rtsp://127.0.0.1:20557/live/trackID=0;seq=10;rtptime=20");
}

static int on_client_pause(turbo_rtsp_session_t *session,
                           const turbo_rtsp_request_t *request,
                           turbo_rtsp_response_t *response,
                           void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_PAUSE ||
        strcmp(request->uri, "rtsp://127.0.0.1:20557/live") != 0 ||
        strcmp(request->session_id, "pull-session-1") != 0) {
        return -1;
    }

    state->client_pause_called = 1;
    return turbo_rtsp_response_pause(response);
}

static int on_client_get_parameter(turbo_rtsp_session_t *session,
                                   const turbo_rtsp_request_t *request,
                                   turbo_rtsp_response_t *response,
                                   void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_GET_PARAMETER ||
        strcmp(request->uri, "rtsp://127.0.0.1:20557/live") != 0 ||
        strcmp(request->session_id, "pull-session-1") != 0) {
        return -1;
    }

    state->client_get_parameter_called = 1;
    return turbo_rtsp_response_get_parameter(
        response,
        "text/parameters",
        "packets_received: 10\r\n",
        strlen("packets_received: 10\r\n"));
}

static int on_client_set_parameter(turbo_rtsp_session_t *session,
                                   const turbo_rtsp_request_t *request,
                                   turbo_rtsp_response_t *response,
                                   void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    const turbo_rtsp_message_t *message = NULL;
    const turbo_rtsp_header_view_t *content_type = NULL;
    const turbo_rtsp_header_view_t *require = NULL;

    if (request->method != TURBO_RTSP_METHOD_SET_PARAMETER ||
        strcmp(request->uri, "rtsp://127.0.0.1:20557/live") != 0 ||
        strcmp(request->session_id, "pull-session-1") != 0) {
        return -1;
    }

    message = turbo_rtsp_session_get_last_message(session);
    if (!message ||
        message->body_len != strlen("volume: 0.75\r\n") ||
        memcmp(message->body, "volume: 0.75\r\n", message->body_len) != 0) {
        return -1;
    }
    content_type = turbo_rtsp_message_find_header(message, "Content-Type");
    if (!content_type ||
        content_type->value_len != strlen("text/parameters") ||
        memcmp(content_type->value, "text/parameters", content_type->value_len) != 0) {
        return -1;
    }
    require = turbo_rtsp_message_find_header(message, "Require");
    if (!require ||
        require->value_len != strlen("org.turbonet.parameters") ||
        memcmp(require->value, "org.turbonet.parameters", require->value_len) != 0) {
        return -1;
    }

    state->client_set_parameter_called = 1;
    return turbo_rtsp_response_set_parameter(response);
}

static int on_client_teardown(turbo_rtsp_session_t *session,
                              const turbo_rtsp_request_t *request,
                              turbo_rtsp_response_t *response,
                              void *user_data) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user_data;
    UNUSED(session);

    if (request->method != TURBO_RTSP_METHOD_TEARDOWN ||
        strcmp(request->uri, "rtsp://127.0.0.1:20557/live") != 0 ||
        strcmp(request->session_id, "pull-session-1") != 0) {
        return -1;
    }

    state->client_teardown_called = 1;
    return turbo_rtsp_response_teardown(response);
}

static void rtsp_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    coro_socket_t *client = NULL;
    char response[4096];
    size_t response_len = 0;
    const char *describe_req =
        "DESCRIBE rtsp://127.0.0.1:20554/live RTSP/1.0\r\n"
        "CSeq: 1\r\n"
        "Accept: application/sdp\r\n"
        "\r\n";
    const char *setup_req =
        "SETUP rtsp://127.0.0.1:20554/live/trackID=0 RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
        "\r\n";
    const char *set_parameter_req =
        "SET_PARAMETER rtsp://127.0.0.1:20554/live RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "Require: org.turbonet.test\r\n"
        "Content-Length: 14\r\n"
        "\r\n"
        "volume: 0.75\r\n";
    const unsigned char interleaved_frame[] = {
        '$', 0, 0, 12,
        0x80, 0xe0, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x01, 0x02, 0x03, 0x04
    };
    unsigned char interleaved_echo[32];

    UNUSED(co);

    client = coro_socket_create_tcpv4(state->ctx);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    coro_socket_set_timeout(client, 2000);
    if (coro_socket_connect(client, "127.0.0.1", 20554) != 0) {
        g_failed = 1;
        coro_socket_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    if (coro_socket_send(client, describe_req, strlen(describe_req)) != 0 ||
        rtsp_recv_response(client, response, sizeof(response), &response_len) != 0) {
        g_failed = 1;
        coro_socket_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    if (strstr(response, "RTSP/1.0 200 OK") == NULL) {
        printf("DESCRIBE response:\n%.*s\n", (int)response_len, response);
    }
    CHECK_TRUE(strstr(response, "RTSP/1.0 200 OK") != NULL);
    CHECK_TRUE(strstr(response, "Content-Type: application/sdp") != NULL);
    CHECK_TRUE(strstr(response, "a=control:trackID=0") != NULL);
    response_len = 0;

    if (coro_socket_send(client, (const char *)interleaved_frame, sizeof(interleaved_frame)) != 0 ||
        rtsp_recv_interleaved_frame(client, interleaved_echo, sizeof(interleaved_echo), &response_len) != 0) {
        g_failed = 1;
        coro_socket_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    CHECK_INT((int)sizeof(interleaved_frame), (int)response_len);
    CHECK_INT('$', interleaved_echo[0]);
    CHECK_INT(1, interleaved_echo[1]);
    CHECK_TRUE(memcmp(interleaved_echo + 4, interleaved_frame + 4, sizeof(interleaved_frame) - 4) == 0);
    response_len = 0;

    if (coro_socket_send(client, setup_req, strlen(setup_req)) != 0 ||
        rtsp_recv_response(client, response, sizeof(response), &response_len) != 0) {
        g_failed = 1;
        coro_socket_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    CHECK_TRUE(strstr(response, "Session: session-1") != NULL);
    CHECK_TRUE(strstr(response, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1") != NULL);
    response_len = 0;

    if (coro_socket_send(client, set_parameter_req, strlen(set_parameter_req)) != 0 ||
        rtsp_recv_response(client, response, sizeof(response), &response_len) != 0) {
        g_failed = 1;
        coro_socket_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    CHECK_TRUE(strstr(response, "RTSP/1.0 200 OK") != NULL);

    coro_socket_destroy(client);
    state->completed = 1;
    turbo_rtsp_server_stop(state->server);
    coro_context_stop(state->ctx);
}

static void rtsp_push_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    turbo_rtsp_push_announce_t announce;
    const turbo_rtsp_client_media_track_t *media_track = NULL;
    uint8_t echo_channel = 0;
    uint8_t echo_payload[32];
    size_t echo_payload_len = 0;
    static const char sdp[] =
        "v=0\r\n"
        "o=- 2 2 IN IP4 127.0.0.1\r\n"
        "s=TurboMedia Push\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        "a=range:npt=0-60.000\r\n"
        "a=sendonly\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f\r\n"
        "a=control:trackID=0\r\n"
        "m=audio 0 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=control:trackID=1\r\n";
    static const unsigned char nal[] = {0x65, 0x88, 0x84, 0x21};
    static const unsigned char sps[] = {0x67, 0x42, 0x00, 0x1f};
    static const unsigned char pps[] = {0x68, 0xce, 0x06, 0xe2};
    static const unsigned char slice[] = {0x65, 0x11, 0x22, 0x33};
    static const unsigned char invalid_nal[] = {0x80, 0x88, 0x84, 0x21};

    UNUSED(co);
    memset(&config, 0, sizeof(config));
    memset(&announce, 0, sizeof(announce));

    config.host = "127.0.0.1";
    config.port = 20555;
    config.timeout_ms = 2000;
    config.user_agent = "TurboMedia RTSP push test";
    if (state->use_configured_h264_rtp_state) {
        config.h264_rtp_ssrc_seed = state->expected_h264_ssrc;
        config.has_h264_rtp_ssrc_seed = 1;
        config.h264_rtp_initial_sequence = state->expected_h264_initial_sequence;
        config.has_h264_rtp_initial_sequence = 1;
        config.h264_rtp_initial_timestamp = state->expected_h264_initial_timestamp;
        config.has_h264_rtp_initial_timestamp = 1;
    }
    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    announce.uri = "rtsp://127.0.0.1:20555/push";
    announce.sdp = sdp;
    announce.sdp_len = sizeof(sdp) - 1;

    if (turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_announce(client, &announce) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    media_track = turbo_rtsp_client_get_media_track(client, 0);
    if (turbo_rtsp_client_get_media_track_count(client) != 2 ||
        !media_track ||
        strcmp(media_track->control_uri, "rtsp://127.0.0.1:20555/push/trackID=0") != 0 ||
        strcmp(media_track->media, "video") != 0 ||
        strcmp(media_track->encoding_name, "H264") != 0 ||
        strcmp(media_track->fmtp, "packetization-mode=1;profile-level-id=42e01f") != 0 ||
        strcmp(media_track->range, "npt=0-60.000") != 0 ||
        strcmp(media_track->direction, "sendonly") != 0 ||
        strcmp(media_track->connection_address, "127.0.0.1") != 0 ||
        media_track->payload_type != 96 ||
        media_track->clock_rate != 90000 ||
        turbo_rtsp_client_send_h264_nal_interleaved_track_index(
            client,
            0,
            nal,
            sizeof(nal),
            3000,
            NULL) != -1 ||
        turbo_rtsp_client_setup_record_interleaved_track_index(client, 2, 2, 3) != -1) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    state->au_timestamp = (state->use_configured_h264_rtp_state
                               ? state->expected_h264_initial_timestamp
                               : 0u) +
                          3000u;
    if (turbo_rtsp_client_setup_record_interleaved_track_index(client, 1, 2, 3) != 0 ||
        turbo_rtsp_client_setup_record_interleaved_track_index(client, 0, 2, 3) != 0 ||
        turbo_rtsp_client_record(client) != 0 ||
        turbo_rtsp_client_send_h264_nal_interleaved_track_index(
            client,
            1,
            nal,
            sizeof(nal),
            3000,
            NULL) != -1 ||
        turbo_rtsp_client_send_h264_nal_interleaved_track_index(
            client,
            0,
            invalid_nal,
            sizeof(invalid_nal),
            3000,
            NULL) != -1 ||
        turbo_rtsp_client_send_h264_nal_interleaved_track_index(
            client,
            0,
            nal,
            sizeof(nal),
            3000,
            &state->rtp_timestamp) != 0 ||
        turbo_rtsp_client_send_h264_nal_interleaved_track_index_ex(
            client,
            0,
            sps,
            sizeof(sps),
            0,
            0,
            &state->au_timestamp) != 0 ||
        turbo_rtsp_client_send_h264_nal_interleaved_track_index_ex(
            client,
            0,
            pps,
            sizeof(pps),
            0,
            0,
            NULL) != 0 ||
        turbo_rtsp_client_send_h264_nal_interleaved_track_index_ex(
            client,
            0,
            slice,
            sizeof(slice),
            1,
            3000,
            NULL) != 0 ||
        turbo_rtsp_client_send_sender_rtcp_compound_interleaved_track_index(
            client,
            0,
            0x1112131421222324ull,
            state->au_timestamp,
            "push-cname",
            strlen("push-cname")) != 0 ||
        turbo_rtsp_client_teardown(client) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    if (state->use_configured_h264_rtp_state &&
        (state->rtp_timestamp != state->expected_h264_initial_timestamp ||
         state->au_timestamp != state->expected_h264_initial_timestamp + 3000u)) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    CHECK_INT(
        0,
        turbo_rtsp_client_recv_interleaved_frame(
            client,
            &echo_channel,
            echo_payload,
            sizeof(echo_payload),
            &echo_payload_len));
    CHECK_INT(3, echo_channel);
    CHECK_INT((int)(TURBO_RTSP_RTP_HEADER_SIZE + sizeof(nal)), (int)echo_payload_len);

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    turbo_rtsp_server_stop(state->server);
    coro_context_stop(state->ctx);
}

static void rtsp_bad_cseq_handler(coro_socket_t *client, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    char *request = NULL;
    size_t request_len = 0;
    const char *response =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 999\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    UNUSED(request_len);
    coro_socket_free_recv(request);

    if (coro_socket_send(client, response, strlen(response)) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
    }
}

static void rtsp_bad_cseq_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_push_announce_t announce;
    turbo_rtsp_client_t *client = NULL;
    static const char sdp[] =
        "v=0\r\n"
        "o=- 3 3 IN IP4 127.0.0.1\r\n"
        "s=Bad CSeq\r\n"
        "t=0 0\r\n";

    UNUSED(co);
    memset(&config, 0, sizeof(config));
    memset(&announce, 0, sizeof(announce));

    config.host = "127.0.0.1";
    config.port = 20556;
    config.timeout_ms = 2000;
    config.user_agent = "TurboMedia RTSP bad-cseq test";

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    announce.uri = "rtsp://127.0.0.1:20556/push";
    announce.sdp = sdp;
    announce.sdp_len = sizeof(sdp) - 1;

    if (turbo_rtsp_client_connect(client) == 0 &&
        turbo_rtsp_client_announce(client, &announce) != 0) {
        state->bad_cseq_rejected = 1;
    } else {
        g_failed = 1;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static int rtsp_send_basic_challenge(coro_socket_t *client, uint32_t cseq) {
    char response[256];
    int len = snprintf(
        response,
        sizeof(response),
        "RTSP/1.0 401 Unauthorized\r\n"
        "CSeq: %u\r\n"
        "WWW-Authenticate: Basic realm=\"TurboMedia\"\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        cseq);

    if (len < 0 || (size_t)len >= sizeof(response)) {
        return -1;
    }
    return coro_socket_send(client, response, (size_t)len);
}

static int rtsp_send_digest_challenge(coro_socket_t *client, uint32_t cseq) {
    char response[512];
    int len = snprintf(
        response,
        sizeof(response),
        "RTSP/1.0 401 Unauthorized\r\n"
        "CSeq: %u\r\n"
        "WWW-Authenticate: Digest realm=\"TurboMedia\", nonce=\"abcdef0123456789\", opaque=\"opaque-token\", algorithm=MD5, qop=\"auth\"\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        cseq);

    if (len < 0 || (size_t)len >= sizeof(response)) {
        return -1;
    }
    return coro_socket_send(client, response, (size_t)len);
}

static int rtsp_send_basic_then_digest_challenge(coro_socket_t *client, uint32_t cseq) {
    char response[768];
    int len = snprintf(
        response,
        sizeof(response),
        "RTSP/1.0 401 Unauthorized\r\n"
        "CSeq: %u\r\n"
        "WWW-Authenticate: Basic realm=\"TurboMedia\"\r\n"
        "WWW-Authenticate: Digest realm=\"TurboMedia\", nonce=\"abcdef0123456789\", opaque=\"opaque-token\", algorithm=MD5, qop=\"auth\"\r\n"
        "Content-Length: 0\r\n"
        "\r\n",
        cseq);

    if (len < 0 || (size_t)len >= sizeof(response)) {
        return -1;
    }
    return coro_socket_send(client, response, (size_t)len);
}

static const char *rtsp_mem_find(const char *data, size_t data_len, const char *needle) {
    size_t needle_len;
    size_t offset;

    if (!data || !needle) return NULL;
    needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > data_len) return NULL;
    for (offset = 0; offset <= data_len - needle_len; ++offset) {
        if (memcmp(data + offset, needle, needle_len) == 0) return data + offset;
    }
    return NULL;
}

static int rtsp_digest_parameter(const char *request,
                                 size_t request_len,
                                 const char *name,
                                 char *value,
                                 size_t value_size) {
    const char *start;
    const char *end;
    const char *request_end;
    size_t length;
    int pattern_len;
    char pattern[64];

    if (!request || !name || !value || value_size == 0 ||
        (pattern_len = snprintf(pattern, sizeof(pattern), "%s=", name)) < 0 ||
        (size_t)pattern_len >= sizeof(pattern)) return -1;
    request_end = request + request_len;
    start = rtsp_mem_find(request, request_len, pattern);
    if (!start) return -1;
    start += (size_t)pattern_len;
    if (start >= request_end) return -1;
    if (*start == '"') {
        start++;
        end = memchr(start, '"', (size_t)(request_end - start));
    } else {
        end = start;
        while (end < request_end && *end != ',' && *end != '\r' && *end != ' ') end++;
    }
    if (!end) return -1;
    length = (size_t)(end - start);
    if (length == 0 || length >= value_size) return -1;
    memcpy(value, start, length);
    value[length] = '\0';
    return 0;
}

static int rtsp_test_md5_hex(const char *input, char output[33]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    size_t i;

    if (!ctx) return -1;
    if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, input, strlen(input)) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1 || digest_len != 16) {
        EVP_MD_CTX_free(ctx);
        return -1;
    }
    EVP_MD_CTX_free(ctx);
    for (i = 0; i < digest_len; ++i) {
        static const char hex[] = "0123456789abcdef";
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    output[32] = '\0';
    return 0;
}

static int rtsp_request_has_digest_auth(const char *request,
                                        size_t request_len,
                                        const char *uri,
                                        const char *password,
                                        const char *expected_nc,
                                        char cnonce[33]) {
    char response[33];
    char ha1[33];
    char ha2[33];
    char expected[33];
    char input[512];
    size_t i;

    if (!request || !uri || !password || !expected_nc || !cnonce ||
        rtsp_digest_parameter(request, request_len, "cnonce", cnonce, 33) != 0 ||
        rtsp_digest_parameter(request, request_len, "response", response, sizeof(response)) != 0 ||
        strlen(cnonce) != 32) return 0;
    for (i = 0; i < 32; ++i) {
        if (!isxdigit((unsigned char)cnonce[i])) return 0;
    }
    if (snprintf(input, sizeof(input), "user:TurboMedia:%s", password) < 0 ||
        rtsp_test_md5_hex(input, ha1) != 0 ||
        snprintf(input, sizeof(input), "OPTIONS:%s", uri) < 0 ||
        rtsp_test_md5_hex(input, ha2) != 0 ||
        snprintf(input,
                 sizeof(input),
                 "%s:abcdef0123456789:%s:%s:auth:%s",
                 ha1,
                 expected_nc,
                 cnonce,
                 ha2) < 0 ||
        rtsp_test_md5_hex(input, expected) != 0) return 0;

    return rtsp_mem_find(request, request_len, "Authorization: Digest ") != NULL &&
           rtsp_mem_find(request, request_len, "username=\"user\"") != NULL &&
           rtsp_mem_find(request, request_len, "realm=\"TurboMedia\"") != NULL &&
           rtsp_mem_find(request, request_len, "nonce=\"abcdef0123456789\"") != NULL &&
           rtsp_mem_find(request, request_len, "algorithm=MD5") != NULL &&
           rtsp_mem_find(request, request_len, "qop=auth") != NULL &&
           rtsp_mem_find(request, request_len, "opaque=\"opaque-token\"") != NULL &&
           strcmp(response, expected) == 0;
}

static void rtsp_basic_auth_handler(coro_socket_t *client, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    char *request = NULL;
    size_t request_len = 0;
    const char *ok_response =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 2\r\n"
        "Public: OPTIONS\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "Authorization:") != NULL) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);

    if (rtsp_send_basic_challenge(client, 1) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    state->auth_challenge_sent = 1;

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "CSeq: 2\r\n") == NULL ||
        rtsp_mem_find(request, request_len, "Authorization: Basic dXNlcjpwYXNz\r\n") == NULL) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);

    if (coro_socket_send(client, ok_response, strlen(ok_response)) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    state->auth_authorized = 1;
}

static void rtsp_basic_auth_reject_handler(coro_socket_t *client, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    char *request = NULL;
    size_t request_len = 0;

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    UNUSED(request_len);
    coro_socket_free_recv(request);

    if (rtsp_send_basic_challenge(client, 1) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    state->auth_challenge_sent = 1;

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "Authorization: Basic dXNlcjp3cm9uZw==\r\n") == NULL) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);

    state->auth_rejected = 1;
    if (rtsp_send_basic_challenge(client, 2) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
    }
}

static void rtsp_digest_auth_handler(coro_socket_t *client, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    char *request = NULL;
    size_t request_len = 0;
    char first_cnonce[33];
    char second_cnonce[33];
    const char *ok_response =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 2\r\n"
        "Public: OPTIONS\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "Authorization:") != NULL) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);

    if (rtsp_send_digest_challenge(client, 1) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    state->auth_challenge_sent = 1;

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "CSeq: 2\r\n") == NULL ||
        !rtsp_request_has_digest_auth(
            request,
            request_len,
            "rtsp://127.0.0.1:20570/live",
            "pass",
            "00000001",
            first_cnonce)) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);

    if (coro_socket_send(client, ok_response, strlen(ok_response)) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "Authorization:") != NULL) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);
    request = NULL;

    if (rtsp_send_digest_challenge(client, 3) != 0 ||
        coro_socket_recv(client, &request, &request_len) != 0 || !request ||
        !rtsp_request_has_digest_auth(
            request,
            request_len,
            "rtsp://127.0.0.1:20570/live",
            "pass",
            "00000002",
            second_cnonce) ||
        strcmp(first_cnonce, second_cnonce) == 0) {
        g_failed = 1;
        if (request) coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);
    if (coro_socket_send(
            client,
            "RTSP/1.0 200 OK\r\nCSeq: 4\r\nPublic: OPTIONS\r\nContent-Length: 0\r\n\r\n",
            strlen("RTSP/1.0 200 OK\r\nCSeq: 4\r\nPublic: OPTIONS\r\nContent-Length: 0\r\n\r\n")) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    state->auth_authorized = 1;
}

static void rtsp_digest_auth_reject_handler(coro_socket_t *client, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    char *request = NULL;
    size_t request_len = 0;

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    UNUSED(request_len);
    coro_socket_free_recv(request);

    if (rtsp_send_digest_challenge(client, 1) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    state->auth_challenge_sent = 1;

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "CSeq: 2\r\n") == NULL ||
        !rtsp_request_has_digest_auth(
            request,
            request_len,
            "rtsp://127.0.0.1:20571/live",
            "wrong",
            "00000001",
            (char[33]){0})) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);

    state->auth_rejected = 1;
    if (rtsp_send_digest_challenge(client, 2) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
    }
}

static void rtsp_digest_auth_after_basic_header_handler(coro_socket_t *client, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    char *request = NULL;
    size_t request_len = 0;
    const char *ok_response =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 2\r\n"
        "Public: OPTIONS\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "Authorization:") != NULL) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);

    if (rtsp_send_basic_then_digest_challenge(client, 1) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    state->auth_challenge_sent = 1;

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "CSeq: 2\r\n") == NULL ||
        rtsp_mem_find(request, request_len, "Authorization: Digest ") == NULL ||
        rtsp_mem_find(request, request_len, "Authorization: Basic ") != NULL) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);

    if (coro_socket_send(client, ok_response, strlen(ok_response)) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    state->auth_authorized = 1;
}

static void rtsp_stale_401_then_bad_cseq_handler(coro_socket_t *client, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    char *request = NULL;
    size_t request_len = 0;
    const char *bad_cseq_response =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 999\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    UNUSED(request_len);
    coro_socket_free_recv(request);

    if (rtsp_send_basic_challenge(client, 1) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    state->auth_challenge_sent = 1;

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "Authorization:") != NULL) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);

    if (coro_socket_send(client, bad_cseq_response, strlen(bad_cseq_response)) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
    }
}

static void rtsp_bad_interleaved_setup_transport_handler(coro_socket_t *client, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    char *request = NULL;
    size_t request_len = 0;
    const char *announce_ok =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 1\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    const char *bad_setup_response =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 2\r\n"
        "Session: bad-interleaved\r\n"
        "Transport: RTP/AVP;unicast;client_port=8000-8001;server_port=9000-9001\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "ANNOUNCE ") == NULL) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);
    if (coro_socket_send(client, announce_ok, strlen(announce_ok)) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    state->announce_called = 1;

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (rtsp_mem_find(request, request_len, "SETUP ") == NULL ||
        rtsp_mem_find(request,
                      request_len,
                      "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;mode=RECORD\r\n") == NULL) {
        g_failed = 1;
        coro_socket_free_recv(request);
        coro_context_stop(state->ctx);
        return;
    }
    coro_socket_free_recv(request);

    if (coro_socket_send(client, bad_setup_response, strlen(bad_setup_response)) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    state->setup_called = 1;
}

static void rtsp_basic_auth_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.host = "127.0.0.1";
    config.port = 20565;
    config.timeout_ms = 2000;

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_set_auth(client, "user", "pass", TURBO_RTSP_AUTH_AUTO) != 0 ||
        turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20565/live") != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code != 200) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void rtsp_digest_auth_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.host = "127.0.0.1";
    config.port = 20570;
    config.timeout_ms = 2000;

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_set_auth(client, "user", "pass", TURBO_RTSP_AUTH_AUTO) != 0 ||
        turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20570/live") != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20570/live") != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code != 200) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void rtsp_digest_auth_second_header_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.host = "127.0.0.1";
    config.port = 20572;
    config.timeout_ms = 2000;

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_set_auth(client, "user", "pass", TURBO_RTSP_AUTH_DIGEST) != 0 ||
        turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20572/live") != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code != 200) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void rtsp_stale_401_bad_cseq_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.host = "127.0.0.1";
    config.port = 20573;
    config.timeout_ms = 2000;

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20573/live") == 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code != 401) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_set_auth(client, "user", "pass", TURBO_RTSP_AUTH_BASIC) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20573/live") == 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code == 401) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->bad_cseq_rejected = 1;
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void rtsp_bad_interleaved_setup_transport_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_push_announce_t announce;
    turbo_rtsp_interleaved_track_t track;
    turbo_rtsp_client_t *client = NULL;
    static const char sdp[] =
        "v=0\r\n"
        "o=- 4 4 IN IP4 127.0.0.1\r\n"
        "s=Bad Interleaved Transport\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=0\r\n";

    UNUSED(co);
    memset(&config, 0, sizeof(config));
    memset(&announce, 0, sizeof(announce));
    memset(&track, 0, sizeof(track));

    config.host = "127.0.0.1";
    config.port = 20574;
    config.timeout_ms = 2000;

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    announce.uri = "rtsp://127.0.0.1:20574/push";
    announce.sdp = sdp;
    announce.sdp_len = sizeof(sdp) - 1;
    track.control_uri = "rtsp://127.0.0.1:20574/push/trackID=0";
    track.rtp_channel = 0;
    track.rtcp_channel = 1;

    if (turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_announce(client, &announce) != 0 ||
        turbo_rtsp_client_setup_interleaved_track(client, &track) == 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void rtsp_digest_auth_wrong_password_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.host = "127.0.0.1";
    config.port = 20571;
    config.timeout_ms = 2000;

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_set_auth(client, "user", "wrong", TURBO_RTSP_AUTH_DIGEST) != 0 ||
        turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20571/live") == 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code != 401) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void rtsp_basic_auth_missing_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.host = "127.0.0.1";
    config.port = 20566;
    config.timeout_ms = 2000;

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20566/live") == 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code != 401) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void rtsp_basic_auth_wrong_password_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.host = "127.0.0.1";
    config.port = 20567;
    config.timeout_ms = 2000;

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_set_auth(client, "user", "wrong", TURBO_RTSP_AUTH_BASIC) != 0 ||
        turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20567/live") == 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code != 401) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void rtsp_queued_interleaved_handler(coro_socket_t *client, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    char *request = NULL;
    size_t request_len = 0;
    static const unsigned char frame[] = {
        '$', 8, 0, 12,
        0x80, 0xe0, 0x22, 0x33,
        0x00, 0x00, 0x00, 0x44,
        0x11, 0x22, 0x33, 0x44
    };
    static const char response[] =
        "RTSP/1.0 200 OK\r\n"
        "CSeq: 1\r\n"
        "Public: OPTIONS\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    if (coro_socket_recv(client, &request, &request_len) != 0 || !request) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    UNUSED(request_len);
    coro_socket_free_recv(request);

    if (coro_socket_send(client, (const char *)frame, sizeof(frame)) != 0 ||
        coro_socket_send(client, response, strlen(response)) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
    }
}

static void rtsp_queued_interleaved_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;
    uint8_t channel = 0;
    uint8_t small_payload[4];
    uint8_t payload[16];
    size_t payload_len = 0;
    static const uint8_t expected_payload[] = {
        0x80, 0xe0, 0x22, 0x33,
        0x00, 0x00, 0x00, 0x44,
        0x11, 0x22, 0x33, 0x44
    };

    UNUSED(co);
    memset(&config, 0, sizeof(config));
    memset(small_payload, 0, sizeof(small_payload));
    memset(payload, 0, sizeof(payload));

    config.host = "127.0.0.1";
    config.port = 20560;
    config.timeout_ms = 2000;

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20560/live") != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code != 200) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    CHECK_INT(
        -1,
        turbo_rtsp_client_recv_interleaved_frame(
            client,
            &channel,
            small_payload,
            sizeof(small_payload),
            &payload_len));
    CHECK_INT((int)sizeof(expected_payload), (int)payload_len);

    payload_len = 0;
    CHECK_INT(
        0,
        turbo_rtsp_client_recv_interleaved_frame(
            client,
            &channel,
            payload,
            sizeof(payload),
            &payload_len));
    CHECK_INT(8, channel);
    CHECK_INT((int)sizeof(expected_payload), (int)payload_len);
    CHECK_TRUE(memcmp(payload, expected_payload, sizeof(expected_payload)) == 0);

    turbo_rtsp_client_destroy(client);
    state->recv_frame_called = 1;
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void rtsp_options_describe_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;
    const turbo_rtsp_header_view_t *public_header = NULL;
    const turbo_rtsp_header_view_t *transport_header = NULL;
    const turbo_rtsp_header_view_t *rtp_info_header = NULL;
    const turbo_rtsp_client_media_track_t *media_track = NULL;
    turbo_rtsp_rtp_info_t rtp_info;
    turbo_rtsp_session_header_t session;
    turbo_rtsp_header_t play_headers[1];
    turbo_rtsp_header_t set_parameter_headers[1];
    size_t rtp_info_count = 0;

    UNUSED(co);
    memset(&config, 0, sizeof(config));
    memset(&rtp_info, 0, sizeof(rtp_info));
    memset(&session, 0, sizeof(session));

    config.host = "127.0.0.1";
    config.port = 20557;
    config.timeout_ms = 2000;
    config.user_agent = "TurboMedia RTSP pull test";

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20557/live") != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code != 200 ||
        last_response->body || last_response->body_len != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    public_header = turbo_rtsp_client_response_find_header(last_response, "Public");
    if (!public_header ||
        public_header->value_len != strlen("OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN") ||
        memcmp(
            public_header->value,
            "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN",
            public_header->value_len) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_describe(client, "rtsp://127.0.0.1:20557/live") != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code != 200 ||
        !last_response->content_type ||
        strcmp(last_response->content_type, "application/sdp") != 0 ||
        !last_response->body ||
        strstr(last_response->body, "s=TurboMedia Pull\r\n") == NULL ||
        last_response->body_len != strlen(last_response->body)) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_get_media_track_count(client) != 1) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    media_track = turbo_rtsp_client_get_media_track(client, 0);
    if (!media_track ||
        strcmp(media_track->control_uri, "rtsp://127.0.0.1:20557/live/trackID=0") != 0 ||
        strcmp(media_track->media, "video") != 0 ||
        strcmp(media_track->encoding_name, "H264") != 0 ||
        strcmp(media_track->fmtp, "packetization-mode=1;profile-level-id=42e01f") != 0 ||
        strcmp(media_track->range, "npt=0-60.000") != 0 ||
        strcmp(media_track->direction, "recvonly") != 0 ||
        strcmp(media_track->connection_address, "198.51.100.20") != 0 ||
        media_track->payload_type != 96 ||
        media_track->clock_rate != 90000) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_setup_play_interleaved_track_index(client, 1, 0, 1) != -1) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_setup_play_interleaved_track_index(client, 0, 0, 1) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_get_session(client, &session) != 0 ||
        strcmp(session.id, "pull-session-1") != 0 ||
        !session.has_timeout ||
        session.timeout_seconds != 60) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    last_response = turbo_rtsp_client_get_last_response(client);
    transport_header = last_response ? turbo_rtsp_client_response_find_header(last_response, "Transport") : NULL;
    if (!transport_header ||
        transport_header->value_len != strlen("RTP/AVP/TCP;unicast;interleaved=0-1;mode=PLAY") ||
        memcmp(
            transport_header->value,
            "RTP/AVP/TCP;unicast;interleaved=0-1;mode=PLAY",
            transport_header->value_len) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    play_headers[0].name = "Speed";
    play_headers[0].value = "1.0";
    if (turbo_rtsp_client_play_ex(
            client,
            "npt=5-",
            play_headers,
            sizeof(play_headers) / sizeof(play_headers[0])) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    last_response = turbo_rtsp_client_get_last_response(client);
    rtp_info_header = last_response ? turbo_rtsp_client_response_find_header(last_response, "RTP-Info") : NULL;
    if (!rtp_info_header ||
        rtp_info_header->value_len != strlen("url=rtsp://127.0.0.1:20557/live/trackID=0;seq=10;rtptime=20") ||
        memcmp(
            rtp_info_header->value,
            "url=rtsp://127.0.0.1:20557/live/trackID=0;seq=10;rtptime=20",
            rtp_info_header->value_len) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_parse_rtp_info(
            rtp_info_header->value,
            rtp_info_header->value_len,
            &rtp_info,
            1,
            &rtp_info_count) != 0 ||
        rtp_info_count != 1 ||
        strcmp(rtp_info.url, "rtsp://127.0.0.1:20557/live/trackID=0") != 0 ||
        !rtp_info.has_seq ||
        rtp_info.seq != 10 ||
        !rtp_info.has_rtptime ||
        rtp_info.rtptime != 20) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_get_parameter(client, NULL, NULL, 0) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }
    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response ||
        last_response->status_code != 200 ||
        !last_response->content_type ||
        strcmp(last_response->content_type, "text/parameters") != 0 ||
        !last_response->body ||
        strcmp(last_response->body, "packets_received: 10\r\n") != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    set_parameter_headers[0].name = "Require";
    set_parameter_headers[0].value = "org.turbonet.parameters";
    if (turbo_rtsp_client_set_parameter_ex(
            client,
            "text/parameters",
            set_parameter_headers,
            sizeof(set_parameter_headers) / sizeof(set_parameter_headers[0]),
            "volume: 0.75\r\n",
            strlen("volume: 0.75\r\n")) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    if (
        turbo_rtsp_client_pause(client) != 0 ||
        turbo_rtsp_client_teardown(client) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    turbo_rtsp_server_stop(state->server);
    coro_context_stop(state->ctx);
}

static void rtsp_url_open_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;
    const turbo_rtsp_header_view_t *public_header = NULL;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.timeout_ms = 2000;
    config.user_agent = "TurboMedia RTSP URL open test";

    client = turbo_rtsp_client_open_url(
        state->ctx,
        "rtsp://ignored:ignored@127.0.0.1:20568/live",
        &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20568/live") != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    public_header = last_response ? turbo_rtsp_client_response_find_header(last_response, "Public") : NULL;
    if (!last_response ||
        last_response->status_code != 200 ||
        !public_header ||
        public_header->value_len != strlen("OPTIONS") ||
        memcmp(public_header->value, "OPTIONS", public_header->value_len) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    turbo_rtsp_server_stop(state->server);
    coro_context_stop(state->ctx);
}

static void rtsp_redirect_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;
    const turbo_rtsp_header_view_t *location_header = NULL;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.host = "127.0.0.1";
    config.port = 20569;
    config.timeout_ms = 2000;
    config.user_agent = "TurboMedia RTSP redirect test";

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_connect(client) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    (void)turbo_rtsp_client_redirect(client, "rtsp://127.0.0.1:20569/live");
    last_response = turbo_rtsp_client_get_last_response(client);
    location_header = last_response
        ? turbo_rtsp_client_response_find_header(last_response, "Location")
        : NULL;
    if (!last_response ||
        last_response->status_code != 302 ||
        !location_header ||
        location_header->value_len != strlen("rtsp://127.0.0.1:20569/redirected") ||
        memcmp(
            location_header->value,
            "rtsp://127.0.0.1:20569/redirected",
            location_header->value_len) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    turbo_rtsp_server_stop(state->server);
    coro_context_stop(state->ctx);
}

static void rtsp_ws_control_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;
    const turbo_rtsp_header_view_t *public_header = NULL;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.host = "127.0.0.1";
    config.port = 20562;
    config.timeout_ms = 2000;
    config.user_agent = "TurboMedia RTSP WS control test";
    config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_WS;
    config.ws_path = "/rtsp";

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20562/live") != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    public_header = last_response ? turbo_rtsp_client_response_find_header(last_response, "Public") : NULL;
    if (!last_response ||
        last_response->status_code != 200 ||
        !public_header ||
        public_header->value_len != strlen("OPTIONS, DESCRIBE") ||
        memcmp(public_header->value, "OPTIONS, DESCRIBE", public_header->value_len) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    turbo_rtsp_server_stop(state->server);
    coro_context_stop(state->ctx);
}

static void rtsp_kcp_control_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_kcp_config_t kcp_config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;
    const turbo_rtsp_header_view_t *public_header = NULL;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.host = "127.0.0.1";
    config.port = 20563;
    config.timeout_ms = 2000;
    config.user_agent = "TurboMedia RTSP KCP control test";
    config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_KCP;
    kcp_config = rtsp_kcp_test_config();
    config.kcp_config = &kcp_config;

    client = turbo_rtsp_client_create(state->ctx, &config);
    turbo_kcp_config_wipe(&kcp_config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_options(client, "rtsp://127.0.0.1:20563/live") != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    public_header = last_response ? turbo_rtsp_client_response_find_header(last_response, "Public") : NULL;
    if (!last_response ||
        last_response->status_code != 200 ||
        !public_header ||
        public_header->value_len != strlen("OPTIONS, DESCRIBE") ||
        memcmp(public_header->value, "OPTIONS, DESCRIBE", public_header->value_len) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    turbo_rtsp_server_stop(state->server);
    coro_context_stop(state->ctx);
}

static void rtsp_udp_setup_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t client_config;
    turbo_rtsp_rtp_udp_pair_config_t udp_config;
    turbo_rtsp_client_t *client = NULL;
    turbo_rtsp_rtp_udp_pair_t *udp_pair = NULL;
    turbo_rtsp_transport_spec_t setup_transport;
    turbo_rtsp_rtp_header_t header;
    uint8_t rtp_packet[128];
    static const uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};
    int rtp_port = 0;
    int rtcp_port = 0;
    int rtp_len = 0;

    UNUSED(co);
    memset(&client_config, 0, sizeof(client_config));
    memset(&udp_config, 0, sizeof(udp_config));
    memset(&setup_transport, 0, sizeof(setup_transport));
    memset(&header, 0, sizeof(header));

    udp_config.local_host = "127.0.0.1";
    udp_config.timeout_ms = 2000;
    udp_pair = turbo_rtsp_rtp_udp_pair_create(state->ctx, &udp_config);
    if (!udp_pair ||
        turbo_rtsp_rtp_udp_pair_get_local_ports(udp_pair, &rtp_port, &rtcp_port) != 0) {
        g_failed = 1;
        goto done;
    }

    client_config.host = "127.0.0.1";
    client_config.port = 20564;
    client_config.timeout_ms = 2000;
    client_config.user_agent = "TurboMedia RTSP UDP setup test";
    client = turbo_rtsp_client_create(state->ctx, &client_config);
    if (!client) {
        g_failed = 1;
        goto done;
    }

    if (turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_describe(client, "rtsp://127.0.0.1:20564/live") != 0) {
        g_failed = 1;
        goto done;
    }
    if (turbo_rtsp_client_get_media_track_count(client) != 2 ||
        turbo_rtsp_client_setup_play_udp_track_index(client, 2, udp_pair) != -1 ||
        turbo_rtsp_client_setup_play_udp_track_index(client, 0, NULL) != -1 ||
        turbo_rtsp_client_setup_play_udp_ports_track_index(client, 2, 30000, 0) != -1 ||
        turbo_rtsp_client_setup_play_udp_ports_track_index(client, 0, 0, 0) != -1) {
        g_failed = 1;
        goto done;
    }
    if (turbo_rtsp_client_setup_play_udp_ports_track_index(client, 0, 30000, 0) != 0 ||
        turbo_rtsp_client_get_last_setup_transport(client, &setup_transport) != 0) {
        g_failed = 1;
        goto done;
    }
    check_int_eq(setup_transport.client_rtp_port, 30000);
    check_int_eq(setup_transport.client_rtcp_port, 30001);
    if (setup_transport.client_rtp_port != 30000 ||
        setup_transport.client_rtcp_port != 30001) {
        g_failed = 1;
        goto done;
    }

    memset(&setup_transport, 0, sizeof(setup_transport));
    if (turbo_rtsp_client_setup_play_udp_track_index(client, 0, udp_pair) != 0 ||
        turbo_rtsp_client_get_last_setup_transport(client, &setup_transport) != 0) {
        g_failed = 1;
        goto done;
    }

    check_int_eq(setup_transport.kind, TURBO_RTSP_TRANSPORT_RTP_AVP_UDP);
    check_int_eq(setup_transport.delivery, TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST);
    check_int_eq(setup_transport.client_rtp_port, rtp_port);
    check_int_eq(setup_transport.client_rtcp_port, rtcp_port);
    check(setup_transport.server_rtp_port > 0);
    check(setup_transport.server_rtcp_port > 0);
    check_int_eq(setup_transport.mode, TURBO_RTSP_TRANSPORT_MODE_PLAY);
    if (setup_transport.kind != TURBO_RTSP_TRANSPORT_RTP_AVP_UDP ||
        setup_transport.delivery != TURBO_RTSP_TRANSPORT_DELIVERY_UNICAST ||
        setup_transport.client_rtp_port != rtp_port ||
        setup_transport.client_rtcp_port != rtcp_port ||
        setup_transport.server_rtp_port <= 0 ||
        setup_transport.server_rtcp_port <= 0 ||
        setup_transport.mode != TURBO_RTSP_TRANSPORT_MODE_PLAY) {
        g_failed = 1;
    } else {
        header.version = 2;
        header.payload_type = 96;
        header.sequence_number = 123;
        header.timestamp = 45678;
        header.ssrc = 0x0a0b0c0du;
        rtp_len = turbo_rtsp_rtp_write_packet(
            rtp_packet,
            sizeof(rtp_packet),
            &header,
            payload,
            sizeof(payload));
        if (rtp_len <= 0 ||
            turbo_rtsp_rtp_udp_pair_send_rtp(udp_pair, rtp_packet, (size_t)rtp_len) != 0 ||
            turbo_rtsp_client_play(client, "npt=0-") != 0) {
            g_failed = 1;
        } else {
            state->completed = 1;
        }
    }

done:
    turbo_rtsp_client_destroy(client);
    turbo_rtsp_rtp_udp_pair_destroy(udp_pair);
    if (state->server) {
        turbo_rtsp_server_stop(state->server);
    }
    coro_context_stop(state->ctx);
}

static void rtsp_rtp_udp_pair_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_rtp_udp_pair_config_t config;
    turbo_rtsp_rtp_udp_pair_t *sender = NULL;
    turbo_rtsp_rtp_udp_pair_t *receiver = NULL;
    turbo_rtsp_rtp_header_t header;
    turbo_rtsp_rtp_header_t parsed_header;
    turbo_rtsp_rtcp_header_t parsed_rtcp;
    uint8_t rtp_packet[128];
    uint8_t rtp_recv[128];
    uint8_t small_rtp_recv[4];
    uint8_t rtcp_packet[64];
    uint8_t rtcp_recv[64];
    uint8_t small_rtcp_recv[4];
    static const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44};
    uint32_t bye_ssrc = 0x10203040u;
    size_t header_len = 0;
    size_t recv_len = 0;
    int sender_rtp_port = 0;
    int sender_rtcp_port = 0;
    int receiver_rtp_port = 0;
    int receiver_rtcp_port = 0;
    int rtp_len = 0;
    int rtcp_len = 0;

    UNUSED(co);
    memset(&config, 0, sizeof(config));
    memset(&header, 0, sizeof(header));

    config.local_host = "127.0.0.1";
    config.timeout_ms = 2000;
    sender = turbo_rtsp_rtp_udp_pair_create(state->ctx, &config);
    receiver = turbo_rtsp_rtp_udp_pair_create(state->ctx, &config);
    if (!sender || !receiver) {
        check(0);
        goto fail;
    }

    if (turbo_rtsp_rtp_udp_pair_get_local_ports(
            sender,
            &sender_rtp_port,
            &sender_rtcp_port) != 0 ||
        turbo_rtsp_rtp_udp_pair_get_local_ports(
            receiver,
            &receiver_rtp_port,
            &receiver_rtcp_port) != 0) {
        check(0);
        goto fail;
    }

    if (turbo_rtsp_rtp_udp_pair_set_peer(
            sender,
            "127.0.0.1",
            receiver_rtp_port,
            receiver_rtcp_port) != 0 ||
        turbo_rtsp_rtp_udp_pair_set_peer(
            receiver,
            "127.0.0.1",
            sender_rtp_port,
            sender_rtcp_port) != 0) {
        check(0);
        goto fail;
    }

    header.version = 2;
    header.payload_type = 96;
    header.sequence_number = 7;
    header.timestamp = 9000;
    header.ssrc = 0x01020304u;
    rtp_len = turbo_rtsp_rtp_write_packet(
        rtp_packet,
        sizeof(rtp_packet),
        &header,
        payload,
        sizeof(payload));
    if (rtp_len <= 0 ||
        turbo_rtsp_rtp_udp_pair_send_rtp(sender, rtp_packet, (size_t)rtp_len) != 0 ||
        turbo_rtsp_rtp_udp_pair_recv_rtp(receiver, rtp_recv, sizeof(rtp_recv), &recv_len) != 0) {
        check(0);
        goto fail;
    }

    check_int_eq((int)recv_len, rtp_len);
    check_int_eq(turbo_rtsp_rtp_parse_header(rtp_recv, recv_len, &parsed_header, &header_len), 0);
    check_int_eq(parsed_header.sequence_number, 7);
    check_int_eq(parsed_header.payload_type, 96);
    check_int_eq(parsed_header.ssrc, 0x01020304u);
    check_int_eq((int)parsed_header.payload_len, (int)sizeof(payload));
    check(memcmp(parsed_header.payload, payload, sizeof(payload)) == 0);

    recv_len = 0;
    if (turbo_rtsp_rtp_udp_pair_send_rtp(sender, rtp_packet, (size_t)rtp_len) != 0) {
        check(0);
        goto fail;
    }
    check_int_eq(
        turbo_rtsp_rtp_udp_pair_recv_rtp(
            receiver,
            small_rtp_recv,
            sizeof(small_rtp_recv),
            &recv_len),
        -1);
    check_int_eq((int)recv_len, rtp_len);

    rtcp_len = turbo_rtsp_rtcp_write_bye(
        rtcp_packet,
        sizeof(rtcp_packet),
        &bye_ssrc,
        1,
        NULL,
        0);
    recv_len = 0;
    if (rtcp_len <= 0 ||
        turbo_rtsp_rtp_udp_pair_send_rtcp(receiver, rtcp_packet, (size_t)rtcp_len) != 0 ||
        turbo_rtsp_rtp_udp_pair_recv_rtcp(sender, rtcp_recv, sizeof(rtcp_recv), &recv_len) != 0) {
        check(0);
        goto fail;
    }

    check_int_eq((int)recv_len, rtcp_len);
    check_int_eq(turbo_rtsp_rtcp_parse_header(rtcp_recv, recv_len, &parsed_rtcp), 0);
    check_int_eq(parsed_rtcp.packet_type, TURBO_RTSP_RTCP_BYE);
    check_int_eq(parsed_rtcp.count, 1);

    recv_len = 0;
    if (turbo_rtsp_rtp_udp_pair_send_rtcp(receiver, rtcp_packet, (size_t)rtcp_len) != 0) {
        check(0);
        goto fail;
    }
    check_int_eq(
        turbo_rtsp_rtp_udp_pair_recv_rtcp(
            sender,
            small_rtcp_recv,
            sizeof(small_rtcp_recv),
            &recv_len),
        -1);
    check_int_eq((int)recv_len, rtcp_len);

    state->completed = 1;

fail:
    if (!state->completed) {
        g_failed = 1;
    }
    turbo_rtsp_rtp_udp_pair_destroy(sender);
    turbo_rtsp_rtp_udp_pair_destroy(receiver);
    coro_context_stop(state->ctx);
}

static void rtsp_raw_interleaved_handler(coro_socket_t *client, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    static const unsigned char frame[] = {
        '$', 4, 0, 12,
        0x80, 0xe0, 0x22, 0x33,
        0x00, 0x00, 0x00, 0x44,
        0x11, 0x22, 0x33, 0x44
    };

    if (coro_socket_send(client, (const char *)frame, sizeof(frame)) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
    }
}

static void rtsp_large_interleaved_handler(coro_socket_t *client, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    unsigned char header[4];
    size_t i = 0;

    if (turbo_rtsp_interleaved_write_header(header, sizeof(header), 6, UINT16_MAX) != 4) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    for (i = 0; i < sizeof(g_large_interleaved_payload); ++i) {
        g_large_interleaved_payload[i] = (unsigned char)(i & 0xffu);
    }

    if (coro_socket_send(client, (const char *)header, sizeof(header)) != 0 ||
        coro_socket_send(
            client,
            (const char *)g_large_interleaved_payload,
            sizeof(g_large_interleaved_payload)) != 0) {
        g_failed = 1;
        coro_context_stop(state->ctx);
    }
}

static void rtsp_recv_interleaved_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    uint8_t channel = 0;
    uint8_t small_payload[4];
    uint8_t payload[16];
    size_t payload_len = 0;
    static const uint8_t expected_payload[] = {
        0x80, 0xe0, 0x22, 0x33,
        0x00, 0x00, 0x00, 0x44,
        0x11, 0x22, 0x33, 0x44
    };

    UNUSED(co);
    memset(&config, 0, sizeof(config));
    memset(small_payload, 0, sizeof(small_payload));
    memset(payload, 0, sizeof(payload));

    config.host = "127.0.0.1";
    config.port = 20558;
    config.timeout_ms = 2000;

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_connect(client) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    CHECK_INT(
        -1,
        turbo_rtsp_client_recv_interleaved_frame(
            client,
            &channel,
            small_payload,
            sizeof(small_payload),
            &payload_len));
    CHECK_INT((int)sizeof(expected_payload), (int)payload_len);

    payload_len = 0;
    CHECK_INT(
        0,
        turbo_rtsp_client_recv_interleaved_frame(
            client,
            &channel,
            payload,
            sizeof(payload),
            &payload_len));
    CHECK_INT(4, channel);
    CHECK_INT((int)sizeof(expected_payload), (int)payload_len);
    CHECK_TRUE(memcmp(payload, expected_payload, sizeof(expected_payload)) == 0);

    turbo_rtsp_client_destroy(client);
    state->recv_frame_called = 1;
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void rtsp_recv_large_interleaved_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    uint8_t channel = 0;
    uint8_t *payload = NULL;
    size_t payload_len = 0;

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    config.host = "127.0.0.1";
    config.port = 20559;
    config.timeout_ms = 2000;

    payload = (uint8_t *)malloc(sizeof(g_large_interleaved_payload));
    if (!payload) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    client = turbo_rtsp_client_create(state->ctx, &config);
    if (!client) {
        free(payload);
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }
    if (turbo_rtsp_client_connect(client) != 0 ||
        turbo_rtsp_client_recv_interleaved_frame(
            client,
            &channel,
            payload,
            sizeof(g_large_interleaved_payload),
            &payload_len) != 0) {
        free(payload);
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    CHECK_INT(6, channel);
    CHECK_INT((int)sizeof(g_large_interleaved_payload), (int)payload_len);
    CHECK_TRUE(memcmp(payload, g_large_interleaved_payload, sizeof(g_large_interleaved_payload)) == 0);

    free(payload);
    turbo_rtsp_client_destroy(client);
    state->recv_frame_called = 1;
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void test_rtsp_server_handles_describe_and_setup(void) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_server_config_t config;
    turbo_rtsp_server_handlers_t handlers;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&config, 0, sizeof(config));
    memset(&handlers, 0, sizeof(handlers));
    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    config.bind_host = "127.0.0.1";
    config.port = 20554;
    config.client_timeout_ms = 2000;
    handlers.on_describe = on_describe;
    handlers.on_setup = on_setup;
    handlers.on_set_parameter = on_set_parameter;
    handlers.on_interleaved_frame = on_interleaved_frame;

    state.ctx = ctx;
    state.server = turbo_rtsp_server_create(ctx, &config, &handlers, &state);
    CHECK_TRUE(state.server != NULL);
    CHECK_INT(0, turbo_rtsp_server_start(state.server));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.describe_called);
    CHECK_TRUE(state.setup_called);
    CHECK_TRUE(state.set_parameter_called);
    CHECK_TRUE(state.interleaved_called);
    CHECK_TRUE(state.completed);

    turbo_rtsp_server_destroy(state.server);
    coro_context_destroy(ctx);
}

static void test_rtsp_server_destroy_closes_idle_session(void) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_server_config_t config;
    turbo_rtsp_server_handlers_t handlers;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&config, 0, sizeof(config));
    memset(&handlers, 0, sizeof(handlers));
    memset(&state, 0, sizeof(state));
    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    config.bind_host = "127.0.0.1";
    config.port = 20574;
    config.client_timeout_ms = 5000;
    handlers.on_options = on_idle_options;
    handlers.on_session_close = on_idle_session_close;
    state.ctx = ctx;
    state.server = turbo_rtsp_server_create(ctx, &config, &handlers, &state);
    CHECK_TRUE(state.server != NULL);
    CHECK_INT(0, turbo_rtsp_server_start(state.server));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_idle_client_task, &state));

    while (!state.idle_connected && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }
    CHECK_TRUE(state.idle_connected);
    turbo_rtsp_server_destroy(state.server);
    state.server = NULL;

    wait_iters = 50000;
    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }
    CHECK_TRUE(state.session_closed);
    CHECK_TRUE(state.completed);
    coro_context_destroy(ctx);
}

static void test_rtsp_server_destroy_cancels_stalled_ws_admission(void) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_server_config_t config;
    turbo_rtsp_server_handlers_t handlers;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&config, 0, sizeof(config));
    memset(&handlers, 0, sizeof(handlers));
    memset(&state, 0, sizeof(state));
    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    config.bind_host = "127.0.0.1";
    config.port = 20575;
    config.client_timeout_ms = 5000;
    config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_WS;
    handlers.on_options = on_idle_options;
    handlers.on_session_close = on_idle_session_close;
    state.ctx = ctx;
    state.server = turbo_rtsp_server_create(ctx, &config, &handlers, &state);
    CHECK_TRUE(state.server != NULL);
    CHECK_INT(0, turbo_rtsp_server_start(state.server));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_stalled_ws_client_task, &state));

    while (!state.idle_connected && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }
    CHECK_TRUE(state.idle_connected);
    coro_context_run(ctx, TURBO_RUN_ONCE);
    turbo_rtsp_server_destroy(state.server);
    state.server = NULL;

    wait_iters = 50000;
    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }
    CHECK_TRUE(state.completed);
    CHECK_TRUE(!state.session_closed);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_pushes_interleaved_record_session_with_config(
    int configure_h264_rtp_state) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_server_config_t config;
    turbo_rtsp_server_handlers_t handlers;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&config, 0, sizeof(config));
    memset(&handlers, 0, sizeof(handlers));
    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    config.bind_host = "127.0.0.1";
    config.port = 20555;
    config.client_timeout_ms = 2000;
    handlers.on_announce = on_push_announce;
    handlers.on_setup = on_push_setup;
    handlers.on_record = on_push_record;
    handlers.on_teardown = on_push_teardown;
    handlers.on_interleaved_frame = on_push_interleaved_frame;

    state.ctx = ctx;
    if (configure_h264_rtp_state) {
        state.use_configured_h264_rtp_state = 1;
        state.expected_h264_ssrc = 0x11223344u;
        state.expected_h264_initial_sequence = 0x1234u;
        state.expected_h264_initial_timestamp = 0x01020304u;
    }
    state.server = turbo_rtsp_server_create(ctx, &config, &handlers, &state);
    CHECK_TRUE(state.server != NULL);
    CHECK_INT(0, turbo_rtsp_server_start(state.server));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_push_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.announce_called);
    CHECK_TRUE(state.setup_called);
    CHECK_TRUE(state.record_called);
    CHECK_TRUE(state.interleaved_called);
    CHECK_INT(3, state.au_interleaved_count);
    CHECK_TRUE(state.rtcp_called);
    CHECK_TRUE(state.teardown_called);
    CHECK_TRUE(state.completed);

    turbo_rtsp_server_destroy(state.server);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_pushes_interleaved_record_session(void) {
    test_rtsp_client_pushes_interleaved_record_session_with_config(0);
}

static void test_rtsp_client_uses_configured_h264_rtp_initial_state(void) {
    test_rtsp_client_pushes_interleaved_record_session_with_config(1);
}

static void test_rtsp_client_rejects_response_cseq_mismatch(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20556,
            rtsp_bad_cseq_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_bad_cseq_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.bad_cseq_rejected);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_retries_basic_auth_after_challenge(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20565,
            rtsp_basic_auth_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_basic_auth_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.auth_challenge_sent);
    CHECK_TRUE(state.auth_authorized);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_keeps_401_when_auth_is_not_configured(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20566,
            rtsp_basic_auth_reject_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_basic_auth_missing_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.auth_challenge_sent);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_fails_after_rejected_basic_auth_retry(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20567,
            rtsp_basic_auth_reject_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_basic_auth_wrong_password_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.auth_challenge_sent);
    CHECK_TRUE(state.auth_rejected);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_retries_digest_auth_after_challenge(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20570,
            rtsp_digest_auth_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_digest_auth_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.auth_challenge_sent);
    CHECK_TRUE(state.auth_authorized);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_retries_digest_auth_from_second_authenticate_header(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20572,
            rtsp_digest_auth_after_basic_header_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_digest_auth_second_header_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.auth_challenge_sent);
    CHECK_TRUE(state.auth_authorized);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_does_not_reuse_stale_401_after_bad_cseq(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20573,
            rtsp_stale_401_then_bad_cseq_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_stale_401_bad_cseq_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.auth_challenge_sent);
    CHECK_TRUE(state.bad_cseq_rejected);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_fails_after_rejected_digest_auth_retry(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20571,
            rtsp_digest_auth_reject_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_digest_auth_wrong_password_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.auth_challenge_sent);
    CHECK_TRUE(state.auth_rejected);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_rejects_mismatched_interleaved_setup_transport(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20574,
            rtsp_bad_interleaved_setup_transport_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_bad_interleaved_setup_transport_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.announce_called);
    CHECK_TRUE(state.setup_called);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_rejects_push_methods_before_connect(void) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_push_announce_t announce;
    turbo_rtsp_interleaved_track_t track;
    turbo_rtsp_session_header_t session;
    turbo_rtsp_client_t *client = NULL;
    static const char sdp[] = "v=0\r\n";
    static const uint8_t payload[] = {0x80, 0xe0};

    memset(&config, 0, sizeof(config));
    memset(&announce, 0, sizeof(announce));
    memset(&track, 0, sizeof(track));
    memset(&session, 0, sizeof(session));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    config.host = "127.0.0.1";
    config.port = 20557;
    config.timeout_ms = 1;
    client = turbo_rtsp_client_create(ctx, &config);
    CHECK_TRUE(client != NULL);

    announce.uri = "rtsp://127.0.0.1:20557/push";
    announce.sdp = sdp;
    announce.sdp_len = sizeof(sdp) - 1;
    track.control_uri = "rtsp://127.0.0.1:20557/push/trackID=0";
    track.rtp_channel = 0;
    track.rtcp_channel = 1;

    CHECK_INT(-1, turbo_rtsp_client_announce(client, &announce));
    CHECK_INT(-1, turbo_rtsp_client_setup_interleaved_track(client, &track));
    CHECK_INT(-1, turbo_rtsp_client_get_session(client, &session));
    CHECK_INT(0, turbo_rtsp_client_set_auth(client, "user", "pass", TURBO_RTSP_AUTH_DIGEST));
    CHECK_INT(-1, turbo_rtsp_client_get_parameter(client, NULL, NULL, 0));
    CHECK_INT(-1, turbo_rtsp_client_set_parameter(client, "text/parameters", "x: y\r\n", 6));
    CHECK_INT(-1, turbo_rtsp_client_record(client));
    CHECK_INT(-1, turbo_rtsp_client_send_interleaved_frame(client, 0, payload, sizeof(payload)));
    CHECK_INT(
        -1,
        turbo_rtsp_client_send_h264_nal_interleaved_track_index_ex(
            client,
            0,
            payload,
            sizeof(payload),
            1,
            3000,
            NULL));
    CHECK_INT(
        -1,
        turbo_rtsp_client_send_sender_rtcp_compound_interleaved_track_index(
            client,
            0,
            0,
            0,
            "push-cname",
            strlen("push-cname")));
    CHECK_INT(-1, turbo_rtsp_client_teardown(client));

    turbo_rtsp_client_destroy(client);
    coro_context_destroy(ctx);
}

static void test_rtsp_url_parse(void) {
    turbo_rtsp_url_t parsed;

    memset(&parsed, 0, sizeof(parsed));
    CHECK_INT(0, turbo_rtsp_url_parse("rtsp://camera.local/live/track", &parsed));
    CHECK_TRUE(strcmp(parsed.host, "camera.local") == 0);
    CHECK_INT(554, parsed.port);
    CHECK_TRUE(strcmp(parsed.path, "/live/track") == 0);

    memset(&parsed, 0, sizeof(parsed));
    CHECK_INT(0, turbo_rtsp_url_parse("rtsp://camera.local:8554/live?stream=1", &parsed));
    CHECK_TRUE(strcmp(parsed.host, "camera.local") == 0);
    CHECK_INT(8554, parsed.port);
    CHECK_TRUE(strcmp(parsed.path, "/live?stream=1") == 0);

    memset(&parsed, 0, sizeof(parsed));
    CHECK_INT(0, turbo_rtsp_url_parse("rtsp://user:pass@[2001:db8::1]:10554/media", &parsed));
    CHECK_TRUE(strcmp(parsed.host, "2001:db8::1") == 0);
    CHECK_INT(10554, parsed.port);
    CHECK_TRUE(strcmp(parsed.path, "/media") == 0);

    CHECK_INT(-1, turbo_rtsp_url_parse("http://camera.local/live", &parsed));
    CHECK_INT(-1, turbo_rtsp_url_parse("rtsp://camera.local:bad/live", &parsed));
    CHECK_INT(-1, turbo_rtsp_url_parse("rtsp://[2001:db8::1/live", &parsed));
}

static void test_rtsp_client_open_url_connects_to_parsed_endpoint(void) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_server_config_t config;
    turbo_rtsp_server_handlers_t handlers;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&config, 0, sizeof(config));
    memset(&handlers, 0, sizeof(handlers));
    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    config.bind_host = "127.0.0.1";
    config.port = 20568;
    config.client_timeout_ms = 2000;
    handlers.on_options = on_url_open_options;

    state.ctx = ctx;
    state.server = turbo_rtsp_server_create(ctx, &config, &handlers, &state);
    CHECK_TRUE(state.server != NULL);
    CHECK_INT(0, turbo_rtsp_server_start(state.server));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_url_open_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.options_called);
    CHECK_TRUE(state.completed);

    turbo_rtsp_server_destroy(state.server);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_stores_redirect_location_response(void) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_server_config_t config;
    turbo_rtsp_server_handlers_t handlers;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&config, 0, sizeof(config));
    memset(&handlers, 0, sizeof(handlers));
    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    config.bind_host = "127.0.0.1";
    config.port = 20569;
    config.client_timeout_ms = 2000;
    config.server_name = "TurboMedia RTSP Redirect Test";
    config.public_methods = "REDIRECT";
    handlers.on_redirect = on_redirect_location;

    state.ctx = ctx;
    state.server = turbo_rtsp_server_create(ctx, &config, &handlers, &state);
    CHECK_TRUE(state.server != NULL);
    CHECK_INT(0, turbo_rtsp_server_start(state.server));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_redirect_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.redirect_called);
    CHECK_TRUE(state.completed);

    turbo_rtsp_server_destroy(state.server);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_options_and_describe_save_last_response(void) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_server_config_t config;
    turbo_rtsp_server_handlers_t handlers;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&config, 0, sizeof(config));
    memset(&handlers, 0, sizeof(handlers));
    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    config.bind_host = "127.0.0.1";
    config.port = 20557;
    config.client_timeout_ms = 2000;
    handlers.on_options = on_client_options;
    handlers.on_describe = on_client_describe;
    handlers.on_setup = on_client_setup;
    handlers.on_play = on_client_play;
    handlers.on_pause = on_client_pause;
    handlers.on_get_parameter = on_client_get_parameter;
    handlers.on_set_parameter = on_client_set_parameter;
    handlers.on_teardown = on_client_teardown;

    state.ctx = ctx;
    state.server = turbo_rtsp_server_create(ctx, &config, &handlers, &state);
    CHECK_TRUE(state.server != NULL);
    CHECK_INT(0, turbo_rtsp_server_start(state.server));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_options_describe_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.options_called);
    CHECK_TRUE(state.client_describe_called);
    CHECK_TRUE(state.client_setup_called);
    CHECK_TRUE(state.client_play_called);
    CHECK_TRUE(state.client_pause_called);
    CHECK_TRUE(state.client_get_parameter_called);
    CHECK_TRUE(state.client_set_parameter_called);
    CHECK_TRUE(state.client_teardown_called);
    CHECK_TRUE(state.completed);

    turbo_rtsp_server_destroy(state.server);
    coro_context_destroy(ctx);
}

static void test_rtsp_control_uses_websocket_transport(void) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_server_config_t config;
    turbo_rtsp_server_handlers_t handlers;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&config, 0, sizeof(config));
    memset(&handlers, 0, sizeof(handlers));
    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    config.bind_host = "127.0.0.1";
    config.port = 20562;
    config.client_timeout_ms = 2000;
    config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_WS;
    handlers.on_options = on_ws_control_options;

    state.ctx = ctx;
    state.server = turbo_rtsp_server_create(ctx, &config, &handlers, &state);
    CHECK_TRUE(state.server != NULL);
    CHECK_INT(0, turbo_rtsp_server_start(state.server));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_ws_control_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.options_called);
    CHECK_TRUE(state.completed);

    turbo_rtsp_server_destroy(state.server);
    coro_context_destroy(ctx);
}

static void test_rtsp_control_uses_kcp_transport(void) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_server_config_t config;
    turbo_kcp_config_t kcp_config;
    turbo_rtsp_server_handlers_t handlers;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&config, 0, sizeof(config));
    memset(&handlers, 0, sizeof(handlers));
    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    config.bind_host = "127.0.0.1";
    config.port = 20563;
    config.client_timeout_ms = 2000;
    config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_KCP;
    kcp_config = rtsp_kcp_test_config();
    config.kcp_config = &kcp_config;
    handlers.on_options = on_kcp_control_options;

    state.ctx = ctx;
    state.server = turbo_rtsp_server_create(ctx, &config, &handlers, &state);
    turbo_kcp_config_wipe(&kcp_config);
    CHECK_TRUE(state.server != NULL);
    CHECK_INT(0, turbo_rtsp_server_start(state.server));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_kcp_control_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.options_called);
    CHECK_TRUE(state.completed);

    turbo_rtsp_server_destroy(state.server);
    coro_context_destroy(ctx);
}

static void test_rtsp_kcp_control_requires_secure_config(void) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_server_config_t server_config;
    turbo_rtsp_client_config_t client_config;
    turbo_kcp_config_t invalid_kcp_config;
    turbo_rtsp_server_t *server = NULL;
    turbo_rtsp_client_t *client = NULL;

    memset(&server_config, 0, sizeof(server_config));
    memset(&client_config, 0, sizeof(client_config));

    ctx = coro_context_create(NULL);
    check_not_null(ctx);
    if (!ctx) {
        return;
    }

    server_config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_KCP;
    client_config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_KCP;
    server = turbo_rtsp_server_create(ctx, &server_config, NULL, NULL);
    client = turbo_rtsp_client_create(ctx, &client_config);

    check_null(server);
    check_null(client);

    turbo_rtsp_server_destroy(server);
    turbo_rtsp_client_destroy(client);

    turbo_kcp_config_default(&invalid_kcp_config);
    server_config.kcp_config = &invalid_kcp_config;
    client_config.kcp_config = &invalid_kcp_config;
    server = turbo_rtsp_server_create(ctx, &server_config, NULL, NULL);
    client = turbo_rtsp_client_create(ctx, &client_config);
    turbo_kcp_config_wipe(&invalid_kcp_config);

    check_not_null(server);
    check_not_null(client);
    check(!server || turbo_rtsp_server_start(server) != 0);
    check(!client || turbo_rtsp_client_connect(client) != 0);

    turbo_rtsp_server_destroy(server);
    turbo_rtsp_client_destroy(client);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_negotiates_udp_setup(void) {
    coro_context_t *ctx = NULL;
    turbo_rtsp_server_config_t config;
    turbo_rtsp_server_handlers_t handlers;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&config, 0, sizeof(config));
    memset(&handlers, 0, sizeof(handlers));
    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    config.bind_host = "127.0.0.1";
    config.port = 20564;
    config.client_timeout_ms = 2000;
    handlers.on_describe = on_udp_describe;
    handlers.on_setup = on_udp_setup;
    handlers.on_play = on_udp_play;

    state.ctx = ctx;
    state.server = turbo_rtsp_server_create(ctx, &config, &handlers, &state);
    CHECK_TRUE(state.server != NULL);
    CHECK_INT(0, turbo_rtsp_server_start(state.server));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_udp_setup_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.client_describe_called);
    CHECK_TRUE(state.client_setup_called);
    CHECK_TRUE(state.client_play_called);
    CHECK_TRUE(state.completed);

    turbo_rtsp_server_destroy(state.server);
    coro_context_destroy(ctx);
}

static void test_rtsp_rtp_uses_udp_pair_transport(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_rtp_udp_pair_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.completed);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_receives_interleaved_frame(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20558,
            rtsp_raw_interleaved_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_recv_interleaved_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.recv_frame_called);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_receives_largest_interleaved_frame(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20559,
            rtsp_large_interleaved_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_recv_large_interleaved_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.recv_frame_called);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void test_rtsp_client_retries_queued_interleaved_frame_after_small_buffer(void) {
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 50000;

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    state.raw_listener = coro_socket_create_tcpv4(ctx);
    CHECK_TRUE(state.raw_listener != NULL);
    CHECK_INT(
        0,
        coro_socket_listen_on(
            state.raw_listener,
            "127.0.0.1",
            20560,
            rtsp_queued_interleaved_handler,
            &state));
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_queued_interleaved_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.recv_frame_called);
    CHECK_TRUE(state.completed);

    rtsp_test_destroy_raw_listener(&state);
    coro_context_destroy(ctx);
}

static void rtsp_public_server_client_task(coro_t *co, void *arg) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)arg;
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client = NULL;
    const turbo_rtsp_client_response_t *last_response = NULL;
    const char *url = getenv("TURBO_MEDIA_PUBLIC_RTSP_URL");

    UNUSED(co);
    memset(&config, 0, sizeof(config));

    if (!url || url[0] == '\0') {
        url = "rtsp://184.72.239.149/vod/mp4:BigBuckBunny_175k.mov";
    }

    config.timeout_ms = 5000;
    config.user_agent = "TurboMedia RTSP public probe";

    client = turbo_rtsp_client_open_url(state->ctx, url, &config);
    if (!client) {
        g_failed = 1;
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_options(client, url) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code < 200 || last_response->status_code >= 300) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    if (turbo_rtsp_client_describe(client, url) != 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    last_response = turbo_rtsp_client_get_last_response(client);
    if (!last_response || last_response->status_code < 200 || last_response->status_code >= 300 ||
        !last_response->body || last_response->body_len == 0) {
        g_failed = 1;
        turbo_rtsp_client_destroy(client);
        coro_context_stop(state->ctx);
        return;
    }

    turbo_rtsp_client_destroy(client);
    state->completed = 1;
    coro_context_stop(state->ctx);
}

static void test_rtsp_public_server_options_and_describe(void) {
    const char *enabled = getenv("TURBO_MEDIA_ENABLE_PUBLIC_RTSP_TESTS");
    coro_context_t *ctx = NULL;
    rtsp_test_state_t state;
    int wait_iters = 250000;

    if (!enabled || strcmp(enabled, "1") != 0) {
        check_true(1);
        return;
    }

    memset(&state, 0, sizeof(state));

    ctx = coro_context_create(NULL);
    CHECK_TRUE(ctx != NULL);

    state.ctx = ctx;
    CHECK_INT(0, coro_context_spawn(ctx, rtsp_public_server_client_task, &state));

    while (!state.completed && !g_failed && wait_iters-- > 0) {
        coro_context_run(ctx, TURBO_RUN_ONCE);
    }

    CHECK_TRUE(state.completed);
    coro_context_destroy(ctx);
}

suite("turbo_media_rtsp") {
    before_each() {
        g_failed = 0;
    }

    it("handles DESCRIBE, SETUP, SET_PARAMETER and interleaved echo") {
        test_rtsp_server_handles_describe_and_setup();
    }

    it("closes an active idle session before destroying the server") {
        test_rtsp_server_destroy_closes_idle_session();
    }

    it("cancels stalled WebSocket admission before destroying the server") {
        test_rtsp_server_destroy_cancels_stalled_ws_admission();
    }

    it("pushes an interleaved RECORD session") {
        test_rtsp_client_pushes_interleaved_record_session();
    }

    it("uses configured H264 RTP initial state for interleaved RECORD") {
        test_rtsp_client_uses_configured_h264_rtp_initial_state();
    }

    it("rejects response CSeq mismatches") {
        test_rtsp_client_rejects_response_cseq_mismatch();
    }

    it("retries Basic authentication after a 401 challenge") {
        test_rtsp_client_retries_basic_auth_after_challenge();
    }

    it("keeps the 401 response when authentication is not configured") {
        test_rtsp_client_keeps_401_when_auth_is_not_configured();
    }

    it("fails after a rejected Basic authentication retry") {
        test_rtsp_client_fails_after_rejected_basic_auth_retry();
    }

    it("retries Digest authentication after a 401 challenge") {
        test_rtsp_client_retries_digest_auth_after_challenge();
    }

    it("retries Digest authentication from a later WWW-Authenticate header") {
        test_rtsp_client_retries_digest_auth_from_second_authenticate_header();
    }

    it("does not reuse a stale 401 after a CSeq mismatch") {
        test_rtsp_client_does_not_reuse_stale_401_after_bad_cseq();
    }

    it("fails after a rejected Digest authentication retry") {
        test_rtsp_client_fails_after_rejected_digest_auth_retry();
    }

    it("rejects mismatched interleaved SETUP transport responses") {
        test_rtsp_client_rejects_mismatched_interleaved_setup_transport();
    }

    it("rejects push methods before connect") {
        test_rtsp_client_rejects_push_methods_before_connect();
    }

    it("parses RTSP URLs") {
        test_rtsp_url_parse();
    }

    it("opens a client from an RTSP URL") {
        test_rtsp_client_open_url_connects_to_parsed_endpoint();
    }

    it("stores REDIRECT Location responses") {
        test_rtsp_client_stores_redirect_location_response();
    }

    it("stores OPTIONS and DESCRIBE client responses") {
        test_rtsp_client_options_and_describe_save_last_response();
    }

    it("runs RTSP control over WebSocket transport") {
        test_rtsp_control_uses_websocket_transport();
    }

    it("runs RTSP control over KCP transport") {
        test_rtsp_control_uses_kcp_transport();
    }

    it("requires secure configuration for RTSP over KCP") {
        test_rtsp_kcp_control_requires_secure_config();
    }

    it("negotiates RTP/AVP UDP setup from the RTSP client") {
        test_rtsp_client_negotiates_udp_setup();
    }

    it("moves RTP and RTCP packets over UDP transport") {
        test_rtsp_rtp_uses_udp_pair_transport();
    }

    it("receives an interleaved frame") {
        test_rtsp_client_receives_interleaved_frame();
    }

    it("receives the largest interleaved frame") {
        test_rtsp_client_receives_largest_interleaved_frame();
    }

    it("retries queued interleaved frames after a small receive buffer") {
        test_rtsp_client_retries_queued_interleaved_frame_after_small_buffer();
    }

    it("can probe a public RTSP server when explicitly enabled") {
        test_rtsp_public_server_options_and_describe();
    }
}

#else

suite("turbo_media_rtsp") {
    it("is disabled when RTSP support is not built") {
        check_true(1);
    }
}

#endif /* TURBO_MEDIA_HAS_RTSP */
