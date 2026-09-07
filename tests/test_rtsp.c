#include "turbo_rtsp.h"
#include "turbo_rtsp_rtp.h"

#include <salts/thread.h>
#include <tinytest.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef TURBO_MEDIA_HAS_RTSP

enum {
    RTSP_TEST_TCP_PORT = 20554,
    RTSP_TEST_WS_PORT = 20555,
    RTSP_TEST_KCP_PORT = 20556,
    RTSP_TEST_AUTH_PORT = 20557,
    RTSP_TEST_UDP_PORT = 20558,
    RTSP_TEST_TIMEOUT_MS = 5000
};

static const uint8_t RTSP_TEST_KCP_PSK[CNET_KCP_PSK_BYTES] = {
    0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98,
    0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f, 0x10,
    0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x97, 0xa8,
    0xb9, 0xca, 0xdb, 0xec, 0xfd, 0x0e, 0x1f, 0x20};

typedef struct {
    salts_mutex_t mutex;
    int mutex_initialized;
    int options_count;
    int describe_count;
    int announce_count;
    int setup_count;
    int play_count;
    int pause_count;
    int record_count;
    int parameter_count;
    int teardown_count;
    int interleaved_count;
    int close_count;
    int auth_required;
    int auth_digest;
    int auth_accepted;
} rtsp_test_state_t;

static void rtsp_test_state_init(rtsp_test_state_t *state) {
    memset(state, 0, sizeof(*state));
    salts_mutex_init(&state->mutex);
    state->mutex_initialized = 1;
}

static void rtsp_test_state_destroy(rtsp_test_state_t *state) {
    if (state && state->mutex_initialized) {
        salts_mutex_destroy(&state->mutex);
        state->mutex_initialized = 0;
    }
}

static void rtsp_test_increment(rtsp_test_state_t *state, int *counter) {
    salts_mutex_lock(&state->mutex);
    ++*counter;
    salts_mutex_unlock(&state->mutex);
}

static int rtsp_test_read(rtsp_test_state_t *state, const int *value) {
    int result;
    salts_mutex_lock(&state->mutex);
    result = *value;
    salts_mutex_unlock(&state->mutex);
    return result;
}

static turbo_rtsp_kcp_config_t rtsp_test_kcp_config(void) {
    turbo_rtsp_kcp_config_t config;
    turbo_rtsp_kcp_config_init(&config);
    memcpy(config.security.pre_shared_key,
           RTSP_TEST_KCP_PSK,
           sizeof(config.security.pre_shared_key));
    config.transport.max_message_bytes = 128u * 1024u;
    return config;
}

static int rtsp_test_options(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user) {
    static const turbo_rtsp_header_t basic_challenge = {
        "WWW-Authenticate", "Basic realm=\"TurboMedia\""};
    static const turbo_rtsp_header_t digest_challenge = {
        "WWW-Authenticate",
        "Digest realm=\"TurboMedia\", nonce=\"rtsp-test-nonce\", algorithm=MD5, qop=\"auth\""};
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    const turbo_rtsp_message_t *message = turbo_rtsp_session_get_last_message(session);
    const turbo_rtsp_header_view_t *authorization =
        turbo_rtsp_message_find_header(message, "Authorization");
    (void)request;

    rtsp_test_increment(state, &state->options_count);
    if (!state->auth_required) {
        return turbo_rtsp_response_options(response, NULL);
    }
    if (!authorization || !authorization->value || authorization->value_len == 0) {
        memset(response, 0, sizeof(*response));
        response->status_code = 401;
        response->reason = "Unauthorized";
        response->headers = state->auth_digest ? &digest_challenge : &basic_challenge;
        response->header_count = 1;
        return 0;
    }
    if ((state->auth_digest && authorization->value_len >= 7u &&
         memcmp(authorization->value, "Digest ", 7u) == 0) ||
        (!state->auth_digest && authorization->value_len >= 6u &&
         memcmp(authorization->value, "Basic ", 6u) == 0)) {
        salts_mutex_lock(&state->mutex);
        state->auth_accepted = 1;
        salts_mutex_unlock(&state->mutex);
        return turbo_rtsp_response_options(response, NULL);
    }
    return turbo_rtsp_response_status(response, 403, "Forbidden");
}

static int rtsp_test_describe(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user) {
    static const char sdp[] =
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=TurboMedia\r\n"
        "t=0 0\r\n"
        "a=control:*\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=0\r\n";
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    (void)session;
    (void)request;
    rtsp_test_increment(state, &state->describe_count);
    return turbo_rtsp_response_describe(response, sdp, sizeof(sdp) - 1u);
}

static int rtsp_test_announce(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    (void)session;
    (void)request;
    rtsp_test_increment(state, &state->announce_count);
    return turbo_rtsp_response_announce(response);
}

static int rtsp_test_setup(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    rtsp_test_increment(state, &state->setup_count);
    if (request->transport_kind == TURBO_RTSP_TRANSPORT_RTP_AVP_UDP) {
        return turbo_rtsp_session_setup_udp_transport(
            session, response, "session-udp", "127.0.0.1", "127.0.0.1");
    }
    return turbo_rtsp_response_setup(response, "session-1", request->transport);
}

static int rtsp_test_play(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    (void)session;
    (void)request;
    rtsp_test_increment(state, &state->play_count);
    return turbo_rtsp_response_play(response, "npt=0-", NULL);
}

static int rtsp_test_pause(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    (void)session;
    (void)request;
    rtsp_test_increment(state, &state->pause_count);
    return turbo_rtsp_response_pause(response);
}

static int rtsp_test_record(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    (void)session;
    (void)request;
    rtsp_test_increment(state, &state->record_count);
    return turbo_rtsp_response_record(response, "npt=0-");
}

static int rtsp_test_parameter(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    (void)session;
    (void)request;
    rtsp_test_increment(state, &state->parameter_count);
    return turbo_rtsp_response_set_parameter(response);
}

static int rtsp_test_teardown(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    (void)session;
    (void)request;
    rtsp_test_increment(state, &state->teardown_count);
    return turbo_rtsp_response_teardown(response);
}

static int rtsp_test_interleaved(
    turbo_rtsp_session_t *session,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_len,
    void *user) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    rtsp_test_increment(state, &state->interleaved_count);
    return turbo_rtsp_session_send_interleaved_frame(
        session, (uint8_t)(channel + 1u), payload, payload_len);
}

static void rtsp_test_close(turbo_rtsp_session_t *session, void *user) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    (void)session;
    rtsp_test_increment(state, &state->close_count);
}

static turbo_rtsp_server_handlers_t rtsp_test_handlers(void) {
    turbo_rtsp_server_handlers_t handlers;
    memset(&handlers, 0, sizeof(handlers));
    handlers.on_options = rtsp_test_options;
    handlers.on_describe = rtsp_test_describe;
    handlers.on_announce = rtsp_test_announce;
    handlers.on_setup = rtsp_test_setup;
    handlers.on_play = rtsp_test_play;
    handlers.on_pause = rtsp_test_pause;
    handlers.on_record = rtsp_test_record;
    handlers.on_set_parameter = rtsp_test_parameter;
    handlers.on_teardown = rtsp_test_teardown;
    handlers.on_interleaved_frame = rtsp_test_interleaved;
    handlers.on_session_close = rtsp_test_close;
    return handlers;
}

static turbo_rtsp_server_t *rtsp_test_server_start(
    rtsp_test_state_t *state,
    turbo_rtsp_control_transport_t transport,
    int port,
    const turbo_rtsp_kcp_config_t *kcp) {
    turbo_rtsp_server_config_t config;
    turbo_rtsp_server_handlers_t handlers = rtsp_test_handlers();
    turbo_rtsp_server_t *server;
    memset(&config, 0, sizeof(config));
    config.bind_host = "127.0.0.1";
    config.port = port;
    config.client_timeout_ms = RTSP_TEST_TIMEOUT_MS;
    config.control_transport = transport;
    config.connection_capacity = 8u;
    config.send_queue_capacity = 32u;
    config.send_queue_bytes = 512u * 1024u;
    config.ws_path = "/rtsp";
    config.ws_subprotocol = "rtsp";
    config.kcp_config = kcp;
    server = turbo_rtsp_server_create(&config, &handlers, state);
    if (!server || turbo_rtsp_server_start(server) != 0) {
        turbo_rtsp_server_destroy(server);
        return NULL;
    }
    return server;
}

static turbo_rtsp_client_t *rtsp_test_client_connect(
    turbo_rtsp_control_transport_t transport,
    int port,
    const turbo_rtsp_kcp_config_t *kcp) {
    turbo_rtsp_client_config_t config;
    turbo_rtsp_client_t *client;
    memset(&config, 0, sizeof(config));
    config.host = "127.0.0.1";
    config.port = port;
    config.timeout_ms = RTSP_TEST_TIMEOUT_MS;
    config.user_agent = "TurboMedia RTSP test";
    config.control_transport = transport;
    config.ws_path = "/rtsp";
    config.ws_subprotocol = "rtsp";
    config.kcp_config = kcp;
    client = turbo_rtsp_client_create(&config);
    if (!client || turbo_rtsp_client_connect(client) != 0) {
        turbo_rtsp_client_destroy(client);
        return NULL;
    }
    return client;
}

static void rtsp_test_play_flow(void) {
    rtsp_test_state_t state;
    turbo_rtsp_server_t *server;
    turbo_rtsp_client_t *client;
    const turbo_rtsp_client_response_t *response;

    rtsp_test_state_init(&state);
    server = rtsp_test_server_start(
        &state, TURBO_RTSP_CONTROL_TRANSPORT_TCP, RTSP_TEST_TCP_PORT, NULL);
    check_not_null(server);
    client = rtsp_test_client_connect(
        TURBO_RTSP_CONTROL_TRANSPORT_TCP, RTSP_TEST_TCP_PORT, NULL);
    check_not_null(client);
    check_equal(turbo_rtsp_client_options(client, "rtsp://127.0.0.1/live"), 0);
    check_equal(turbo_rtsp_client_describe(client, "rtsp://127.0.0.1/live"), 0);
    check_equal((int)turbo_rtsp_client_get_media_track_count(client), 1);
    check_equal(turbo_rtsp_client_setup_play_interleaved_track_index(client, 0u, 0u, 1u), 0);
    check_equal(turbo_rtsp_client_play(client, "npt=0-"), 0);
    check_equal(turbo_rtsp_client_pause(client), 0);
    check_equal(turbo_rtsp_client_teardown(client), 0);
    response = turbo_rtsp_client_get_last_response(client);
    check_not_null(response);
    check_equal(response->status_code, 200);
    check_equal(rtsp_test_read(&state, &state.options_count), 1);
    check_equal(rtsp_test_read(&state, &state.describe_count), 1);
    check_equal(rtsp_test_read(&state, &state.setup_count), 1);
    check_equal(rtsp_test_read(&state, &state.play_count), 1);
    check_equal(rtsp_test_read(&state, &state.pause_count), 1);
    check_equal(rtsp_test_read(&state, &state.teardown_count), 1);
    turbo_rtsp_client_destroy(client);
    turbo_rtsp_server_destroy(server);
    rtsp_test_state_destroy(&state);
}

static void rtsp_test_record_flow(
    turbo_rtsp_control_transport_t transport,
    int port,
    turbo_rtsp_kcp_config_t *kcp) {
    static const char sdp[] =
        "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=push\r\nt=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\na=control:trackID=0\r\n";
    const uint8_t payload[] = {0x80, 0x60, 0x00, 0x01, 0x11, 0x22, 0x33};
    uint8_t received[64];
    uint8_t channel = 0;
    size_t received_size = 0;
    turbo_rtsp_push_announce_t announce;
    turbo_rtsp_push_track_t track;
    rtsp_test_state_t state;
    turbo_rtsp_server_t *server;
    turbo_rtsp_client_t *client;

    rtsp_test_state_init(&state);
    server = rtsp_test_server_start(&state, transport, port, kcp);
    check_not_null(server);
    client = rtsp_test_client_connect(transport, port, kcp);
    check_not_null(client);
    announce.uri = "rtsp://127.0.0.1/live/push";
    announce.sdp = sdp;
    announce.sdp_len = sizeof(sdp) - 1u;
    track.control_uri = "rtsp://127.0.0.1/live/push/trackID=0";
    track.rtp_channel = 0u;
    track.rtcp_channel = 1u;
    check_equal(turbo_rtsp_client_announce(client, &announce), 0);
    check_equal(turbo_rtsp_client_setup_interleaved(client, &track), 0);
    check_equal(turbo_rtsp_client_record(client), 0);
    check_equal(turbo_rtsp_client_set_parameter(
                    client, "text/parameters", "ping", 4u), 0);
    check_equal(turbo_rtsp_client_send_interleaved_frame(
                    client, 0u, payload, sizeof(payload)), 0);
    check_equal(turbo_rtsp_client_recv_interleaved_frame(
                    client, &channel, received, 1u, &received_size), -1);
    check_equal((int)received_size, (int)sizeof(payload));
    check_equal(turbo_rtsp_client_recv_interleaved_frame(
                    client, &channel, received, sizeof(received), &received_size), 0);
    check_equal(channel, 1u);
    check_equal((int)received_size, (int)sizeof(payload));
    check_equal(memcmp(received, payload, sizeof(payload)), 0);
    check_equal(turbo_rtsp_client_teardown(client), 0);
    check_equal(rtsp_test_read(&state, &state.announce_count), 1);
    check_equal(rtsp_test_read(&state, &state.record_count), 1);
    check_equal(rtsp_test_read(&state, &state.parameter_count), 1);
    check_equal(rtsp_test_read(&state, &state.interleaved_count), 1);
    turbo_rtsp_client_destroy(client);
    turbo_rtsp_server_destroy(server);
    rtsp_test_state_destroy(&state);
}

static void rtsp_test_auth(int digest) {
    rtsp_test_state_t state;
    turbo_rtsp_server_t *server;
    turbo_rtsp_client_t *client;
    rtsp_test_state_init(&state);
    state.auth_required = 1;
    state.auth_digest = digest;
    server = rtsp_test_server_start(
        &state, TURBO_RTSP_CONTROL_TRANSPORT_TCP, RTSP_TEST_AUTH_PORT, NULL);
    check_not_null(server);
    client = rtsp_test_client_connect(
        TURBO_RTSP_CONTROL_TRANSPORT_TCP, RTSP_TEST_AUTH_PORT, NULL);
    check_not_null(client);
    check_equal(turbo_rtsp_client_set_auth(
                    client, "alice", "secret",
                    digest ? TURBO_RTSP_AUTH_DIGEST : TURBO_RTSP_AUTH_BASIC), 0);
    check_equal(turbo_rtsp_client_options(client, "rtsp://127.0.0.1/live"), 0);
    check_equal(rtsp_test_read(&state, &state.options_count), 2);
    check_equal(rtsp_test_read(&state, &state.auth_accepted), 1);
    turbo_rtsp_client_destroy(client);
    turbo_rtsp_server_destroy(server);
    rtsp_test_state_destroy(&state);
}

static void rtsp_test_udp_pair(void) {
    turbo_rtsp_rtp_udp_pair_config_t config;
    turbo_rtsp_rtp_udp_pair_t *sender;
    turbo_rtsp_rtp_udp_pair_t *receiver;
    const uint8_t packet[] = {0x80, 0x60, 0x00, 0x01, 0xaa, 0xbb};
    uint8_t received[32];
    size_t received_size = 0;
    int sender_rtp = 0;
    int sender_rtcp = 0;
    int receiver_rtp = 0;
    int receiver_rtcp = 0;
    memset(&config, 0, sizeof(config));
    config.local_host = "127.0.0.1";
    config.timeout_ms = RTSP_TEST_TIMEOUT_MS;
    config.send_capacity = 4u;
    config.max_datagram_bytes = 1500u;
    sender = turbo_rtsp_rtp_udp_pair_create(&config);
    receiver = turbo_rtsp_rtp_udp_pair_create(&config);
    check_not_null(sender);
    check_not_null(receiver);
    check_equal(turbo_rtsp_rtp_udp_pair_get_local_ports(
                    sender, &sender_rtp, &sender_rtcp), 0);
    check_equal(turbo_rtsp_rtp_udp_pair_get_local_ports(
                    receiver, &receiver_rtp, &receiver_rtcp), 0);
    check_equal(turbo_rtsp_rtp_udp_pair_set_peer(
                    sender, "127.0.0.1", receiver_rtp, receiver_rtcp), 0);
    check_equal(turbo_rtsp_rtp_udp_pair_set_peer(
                    receiver, "127.0.0.1", sender_rtp, sender_rtcp), 0);
    check_equal(turbo_rtsp_rtp_udp_pair_send_rtp(
                    sender, packet, sizeof(packet)), 0);
    check_equal(turbo_rtsp_rtp_udp_pair_recv_rtp(
                    receiver, received, sizeof(received), &received_size), 0);
    check_equal((int)received_size, (int)sizeof(packet));
    check_equal(memcmp(received, packet, sizeof(packet)), 0);
    check_equal(turbo_rtsp_rtp_udp_pair_send_rtcp(
                    receiver, packet, sizeof(packet)), 0);
    check_equal(turbo_rtsp_rtp_udp_pair_recv_rtcp(
                    sender, received, sizeof(received), &received_size), 0);
    turbo_rtsp_rtp_udp_pair_destroy(receiver);
    turbo_rtsp_rtp_udp_pair_destroy(sender);
}

static void rtsp_test_udp_setup(void) {
    rtsp_test_state_t state;
    turbo_rtsp_server_t *server;
    turbo_rtsp_client_t *client;
    turbo_rtsp_rtp_udp_pair_t *pair;
    turbo_rtsp_rtp_udp_pair_config_t pair_config;
    turbo_rtsp_transport_spec_t transport;
    rtsp_test_state_init(&state);
    server = rtsp_test_server_start(
        &state, TURBO_RTSP_CONTROL_TRANSPORT_TCP, RTSP_TEST_UDP_PORT, NULL);
    check_not_null(server);
    client = rtsp_test_client_connect(
        TURBO_RTSP_CONTROL_TRANSPORT_TCP, RTSP_TEST_UDP_PORT, NULL);
    check_not_null(client);
    check_equal(turbo_rtsp_client_describe(client, "rtsp://127.0.0.1/live"), 0);
    memset(&pair_config, 0, sizeof(pair_config));
    pair_config.local_host = "127.0.0.1";
    pair_config.timeout_ms = RTSP_TEST_TIMEOUT_MS;
    pair = turbo_rtsp_rtp_udp_pair_create(&pair_config);
    check_not_null(pair);
    check_equal(turbo_rtsp_client_setup_play_udp_track_index(client, 0u, pair), 0);
    memset(&transport, 0, sizeof(transport));
    check_equal(turbo_rtsp_client_get_last_setup_transport(client, &transport), 0);
    check_equal(transport.kind, TURBO_RTSP_TRANSPORT_RTP_AVP_UDP);
    check(transport.server_rtp_port > 0);
    check(transport.server_rtcp_port > 0);
    turbo_rtsp_client_destroy(client);
    turbo_rtsp_rtp_udp_pair_destroy(pair);
    turbo_rtsp_server_destroy(server);
    rtsp_test_state_destroy(&state);
}

static void rtsp_test_restart_and_drain(void) {
    rtsp_test_state_t state;
    turbo_rtsp_server_t *server;
    turbo_rtsp_client_t *client;
    turbo_rtsp_server_config_t config;
    turbo_rtsp_server_handlers_t handlers = rtsp_test_handlers();
    rtsp_test_state_init(&state);
    memset(&config, 0, sizeof(config));
    config.bind_host = "127.0.0.1";
    config.port = RTSP_TEST_TCP_PORT;
    config.client_timeout_ms = RTSP_TEST_TIMEOUT_MS;
    config.connection_capacity = 4u;
    server = turbo_rtsp_server_create(&config, &handlers, &state);
    check_not_null(server);
    check_equal(turbo_rtsp_server_start(server), 0);
    client = rtsp_test_client_connect(
        TURBO_RTSP_CONTROL_TRANSPORT_TCP, RTSP_TEST_TCP_PORT, NULL);
    check_not_null(client);
    turbo_rtsp_server_stop(server);
    check_equal(turbo_rtsp_client_options(client, "rtsp://127.0.0.1/live"), -1);
    turbo_rtsp_client_destroy(client);
    check_equal(turbo_rtsp_server_start(server), 0);
    client = rtsp_test_client_connect(
        TURBO_RTSP_CONTROL_TRANSPORT_TCP, RTSP_TEST_TCP_PORT, NULL);
    check_not_null(client);
    check_equal(turbo_rtsp_client_options(client, "rtsp://127.0.0.1/live"), 0);
    turbo_rtsp_client_destroy(client);
    turbo_rtsp_server_destroy(server);
    check(rtsp_test_read(&state, &state.close_count) >= 1);
    rtsp_test_state_destroy(&state);
}

suite("TurboMedia RTSP over Salts") {
    it("runs the complete play control flow over CNet TCP") {
        rtsp_test_play_flow();
    }

    it("runs RECORD and interleaved media over CNet TCP") {
        rtsp_test_record_flow(
            TURBO_RTSP_CONTROL_TRANSPORT_TCP, RTSP_TEST_TCP_PORT, NULL);
    }

    it("runs RECORD and interleaved media over CHTTP WebSocket") {
        rtsp_test_record_flow(
            TURBO_RTSP_CONTROL_TRANSPORT_WS, RTSP_TEST_WS_PORT, NULL);
    }

    it("runs RECORD and interleaved media over authenticated CNet KCP") {
        turbo_rtsp_kcp_config_t kcp = rtsp_test_kcp_config();
        rtsp_test_record_flow(
            TURBO_RTSP_CONTROL_TRANSPORT_KCP, RTSP_TEST_KCP_PORT, &kcp);
        turbo_rtsp_kcp_config_wipe(&kcp);
    }

    it("rejects absent or zero-key KCP security") {
        turbo_rtsp_server_config_t server_config;
        turbo_rtsp_client_config_t client_config;
        turbo_rtsp_kcp_config_t kcp;
        memset(&server_config, 0, sizeof(server_config));
        memset(&client_config, 0, sizeof(client_config));
        server_config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_KCP;
        client_config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_KCP;
        check_null(turbo_rtsp_server_create(&server_config, NULL, NULL));
        check_null(turbo_rtsp_client_create(&client_config));
        turbo_rtsp_kcp_config_init(&kcp);
        server_config.kcp_config = &kcp;
        client_config.kcp_config = &kcp;
        check_null(turbo_rtsp_server_create(&server_config, NULL, NULL));
        check_null(turbo_rtsp_client_create(&client_config));
        turbo_rtsp_kcp_config_wipe(&kcp);
    }

    it("retries Basic authentication after a 401 challenge") {
        rtsp_test_auth(0);
    }

    it("retries Digest authentication after a 401 challenge") {
        rtsp_test_auth(1);
    }

    it("negotiates an RTP/AVP UDP track") {
        rtsp_test_udp_setup();
    }

    it("moves RTP and RTCP datagrams through bounded CNet owners") {
        rtsp_test_udp_pair();
    }

    it("stops active sessions and restarts cleanly") {
        rtsp_test_restart_and_drain();
    }

    it("parses RTSP URLs") {
        turbo_rtsp_url_t parsed;
        memset(&parsed, 0, sizeof(parsed));
        check_equal(turbo_rtsp_url_parse(
                        "rtsp://example.com:8554/live/camera", &parsed), 0);
        check_equal(strcmp(parsed.host, "example.com"), 0);
        check_equal(parsed.port, 8554);
        check_equal(strcmp(parsed.path, "/live/camera"), 0);
    }
}

#else

suite("TurboMedia RTSP over Salts") {
    it("is disabled when RTSP support is not built") {
        check_true(1);
    }
}

#endif
