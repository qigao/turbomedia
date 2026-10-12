#include "turbo_rtsp.h"
#include "turbo_rtsp_rtp.h"
#include "turbo_rtsp_sdp.h"

#include <http_client/http.h>
#include <http_server/http.h>
#include <salts/thread.h>
#include <salts/clock.h>
#include <cmeta_buffer.h>
#include <stdatomic.h>
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
    RTSP_TEST_RECONNECT_PORT = 20559,
    RTSP_TEST_TIMEOUT_MS = 5000
};

static native_io_backend_kind rtsp_test_chttp_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config rtsp_test_chttp_network(size_t connections) {
    cnet_client_config config;
    memset(&config, 0, sizeof(config));
    config.backend = rtsp_test_chttp_backend();
    config.connection_capacity = connections;
    config.command_capacity = 16u;
    config.request_capacity = 8u;
    config.completion_batch_capacity = 8u;
    config.event_capacity = 16u;
    config.max_send_bytes = 4096u;
    config.receive_buffer_bytes = 4096u;
    config.connect_timeout_ms = RTSP_TEST_TIMEOUT_MS;
    config.read_timeout_ms = RTSP_TEST_TIMEOUT_MS;
    config.write_timeout_ms = RTSP_TEST_TIMEOUT_MS;
    return config;
}

static int rtsp_test_chttp_websocket_open(
    void *user,
    chttp_websocket *websocket,
    const chttp_server_request_view *request,
    chttp_server_response *response) {
    int *opened = (int *)user;
    int status;
    (void)websocket;
    status = chttp_server_response_select_websocket_subprotocol(
        response, request, "rtsp");
    if (status == SALTS_OK && opened) {
        *opened = 1;
    }
    return status;
}

static void rtsp_test_chttp_websocket_event(
    void *user,
    chttp_websocket *websocket,
    const chttp_websocket_event *event) {
    (void)user;
    (void)websocket;
    (void)event;
}

static void rtsp_test_chttp_websocket_smoke(void) {
    chttp_server server = {0};
    chttp_server_config server_config = {0};
    chttp_server_websocket_options route = {0};
    chttp_websocket_client client = {0};
    chttp_websocket_client_config client_config = {0};
    chttp_websocket_connect_options options = {0};
    unsigned int http_status = 0u;
    uint16_t port = 0u;
    char uri[128];
    int opened = 0;
    int status;
    int server_initialized = 0;
    int client_initialized = 0;

    server_config.host = "127.0.0.1";
    server_config.port = 0u;
    server_config.backlog = 8u;
    server_config.network = rtsp_test_chttp_network(4u);
    server_config.route_capacity = 4u;
    server_config.middleware_capacity = 2u;
    server_config.max_route_middleware_count = 2u;
    server_config.max_route_param_count = 2u;
    server_config.max_route_param_bytes = 64u;
    server_config.max_target_bytes = 256u;
    server_config.max_header_count = 16u;
    server_config.max_header_bytes = 2048u;
    server_config.max_request_body_bytes = 256u;
    server_config.max_response_header_count = 16u;
    server_config.max_response_header_bytes = 1024u;
    server_config.max_response_body_bytes = 256u;
    server_config.poll_slice_ms = 2u;

    status = chttp_server_init(&server, &server_config);
    check_equal(status, SALTS_OK);
    if (status != SALTS_OK) {
        return;
    }
    server_initialized = 1;

    route.size = sizeof(route);
    route.path = "/rtsp";
    route.max_frame_bytes = 1024u;
    route.max_message_bytes = 1024u;
    route.max_buffered_input_bytes = 2048u;
    route.on_open = rtsp_test_chttp_websocket_open;
    route.on_event = rtsp_test_chttp_websocket_event;
    route.user = &opened;
    status = chttp_server_websocket_with(&server, &route);
    check_equal(status, SALTS_OK);
    if (status != SALTS_OK) {
        goto cleanup;
    }
    status = chttp_server_start(&server);
    check_equal(status, SALTS_OK);
    if (status != SALTS_OK) {
        goto cleanup;
    }
    status = chttp_server_port(&server, &port);
    check_equal(status, SALTS_OK);
    if (status != SALTS_OK) {
        goto cleanup;
    }

    client_config.size = sizeof(client_config);
    client_config.network = rtsp_test_chttp_network(1u);
    client_config.max_handshake_header_bytes = 4096u;
    client_config.event_capacity = 8u;
    status = chttp_websocket_client_init(&client, &client_config);
    check_equal(status, SALTS_OK);
    if (status != SALTS_OK) {
        goto cleanup;
    }
    client_initialized = 1;

    {
        int uri_length = snprintf(uri, sizeof(uri), "ws://127.0.0.1:%u/rtsp",
                                  (unsigned int)port);
        check_true(uri_length > 0 && (size_t)uri_length < sizeof(uri));
    }
    options.size = sizeof(options);
    options.uri = uri;
    options.timeout_ms = RTSP_TEST_TIMEOUT_MS;
    options.subprotocol = "rtsp";
    status = chttp_websocket_client_connect(&client, &options, &http_status);
    if (status != SALTS_OK) {
        fprintf(stderr,
                "TurboMedia direct CHTTP WebSocket smoke failed: status=%d http_status=%u opened=%d\n",
                status, http_status, opened);
    }
    check_equal(status, SALTS_OK);
    if (status == SALTS_OK) {
        check_equal((int)http_status, 101);
        check_equal(opened, 1);
        (void)chttp_websocket_client_close(
            &client, 1000u, NULL, 0u, RTSP_TEST_TIMEOUT_MS);
    }

cleanup:
    if (client_initialized) {
        (void)chttp_websocket_client_destroy(&client, RTSP_TEST_TIMEOUT_MS);
    }
    if (server_initialized) {
        (void)chttp_server_destroy(&server);
    }
}

static const uint8_t RTSP_TEST_KCP_PSK[CNET_KCP_PSK_BYTES] = {
    0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98,
    0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f, 0x10,
    0x31, 0x42, 0x53, 0x64, 0x75, 0x86, 0x97, 0xa8,
    0xb9, 0xca, 0xdb, 0xec, 0xfd, 0x0e, 0x1f, 0x20};

typedef struct {
    cmeta_mutex_t mutex;
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
    int require_mpvideo_sdp; /* immutable test-server policy before start */
    int accepted_mpvideo_sdp,rejected_mpvideo_sdp;
    /* M15d test-only: one actual CNet RTSP handler remains on the server's
     * callback stack until the Media Owner closes the old client Session.
     * These flags do not replace or modify CNet's original completion. */
    _Atomic unsigned hold_interleaved;
    _Atomic unsigned interleaved_entered;
    _Atomic unsigned interleaved_release;
    _Atomic unsigned interleaved_exited;
    /* Old CNet Client receive is cancelled AFTER real server OPTIONS
     * admission, but BEFORE the server generates its response. */
    _Atomic unsigned hold_options;
    _Atomic unsigned options_entered;
    _Atomic unsigned options_release;
    _Atomic unsigned options_exited;
    _Atomic unsigned options_timed_out;
    int auth_required;
    int auth_digest;
    int auth_accepted;
} rtsp_test_state_t;

static void rtsp_test_state_init(rtsp_test_state_t *state) {
    memset(state, 0, sizeof(*state));
    atomic_init(&state->hold_interleaved,0u);
    atomic_init(&state->interleaved_entered,0u);
    atomic_init(&state->interleaved_release,0u);
    atomic_init(&state->interleaved_exited,0u);
    atomic_init(&state->hold_options,0u);
    atomic_init(&state->options_entered,0u);
    atomic_init(&state->options_release,0u);
    atomic_init(&state->options_exited,0u);
    atomic_init(&state->options_timed_out,0u);
    cmeta_mutex_init(&state->mutex);
    state->mutex_initialized = 1;
}

static void rtsp_test_state_destroy(rtsp_test_state_t *state) {
    if (state && state->mutex_initialized) {
        cmeta_mutex_destroy(&state->mutex);
        state->mutex_initialized = 0;
    }
}

static void rtsp_test_increment(rtsp_test_state_t *state, int *counter) {
    cmeta_mutex_lock(&state->mutex);
    ++*counter;
    cmeta_mutex_unlock(&state->mutex);
}

static int rtsp_test_read(rtsp_test_state_t *state, const int *value) {
    int result;
    cmeta_mutex_lock(&state->mutex);
    result = *value;
    cmeta_mutex_unlock(&state->mutex);
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
    if (atomic_load_explicit(&state->hold_options,memory_order_acquire)) {
        uint64_t started=cmeta_monotonic_ms();
        /* Reached only with bytes delivered through the actual RTSP/CNet
         * receive pipeline, never through a fabricated completion. */
        atomic_store_explicit(&state->options_entered,1u,memory_order_release);
        while(!atomic_load_explicit(&state->options_release,memory_order_acquire)) {
            if(cmeta_monotonic_ms()-started>=RTSP_TEST_TIMEOUT_MS) {
                atomic_store_explicit(&state->options_timed_out,1u,
                                      memory_order_release);
                break;
            }
            cmeta_thread_yield();
        }
        atomic_store_explicit(&state->options_exited,1u,memory_order_release);
    }
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
        cmeta_mutex_lock(&state->mutex);
        state->auth_accepted = 1;
        cmeta_mutex_unlock(&state->mutex);
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

/* Test-only strict profile negotiation at the REAL RTSP control plane.
 * RFC2250 PT32 MPEG Video ES is never interchangeable with H264 or
 * MPEG2-TS PT33. This gate runs on the server's live CNet callback, before
 * the host admits any RTP bytes to a bounded tm_ffd Codec Instance. */
static int rtsp_test_announce(
    turbo_rtsp_session_t *session,
    const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response,
    void *user) {
    rtsp_test_state_t *state = (rtsp_test_state_t *)user;
    (void)request;
    rtsp_test_increment(state, &state->announce_count);
    if(state->require_mpvideo_sdp) {
        const turbo_rtsp_message_t *message=
            turbo_rtsp_session_get_last_message(session);
        turbo_rtsp_sdp_description_t parsed;
        int good=message && message->body && message->body_len &&
            turbo_rtsp_sdp_parse(message->body,message->body_len,&parsed)==0 &&
            parsed.media_count==1u &&
            strcmp(parsed.media[0].media,"video")==0 &&
            strcmp(parsed.media[0].proto,"RTP/AVP")==0 &&
            parsed.media[0].payload_type==32 &&
            strcmp(parsed.media[0].encoding_name,"MPV")==0 &&
            parsed.media[0].clock_rate==90000;
        if(!good) {
            rtsp_test_increment(state,&state->rejected_mpvideo_sdp);
            response->status_code=415;
            response->reason="Unsupported Media Type";
            return 0;
        }
        rtsp_test_increment(state,&state->accepted_mpvideo_sdp);
    }
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
    int result;
    rtsp_test_increment(state, &state->interleaved_count);
    if(atomic_load_explicit(&state->hold_interleaved,memory_order_acquire)) {
        /* This executes AFTER real CNet packet reception, while the
         * original RTSP handler still borrows the old Session. It is NOT
         * merely a pre-receive gate or fabricated RTP callback. */
        atomic_store_explicit(&state->interleaved_entered,1u,memory_order_release);
        while(!atomic_load_explicit(&state->interleaved_release,
                                    memory_order_acquire)) {
            cmeta_thread_yield();
        }
    }
    result=turbo_rtsp_session_send_interleaved_frame(
        session, (uint8_t)(channel + 1u), payload, payload_len);
    if(atomic_load_explicit(&state->hold_interleaved,memory_order_acquire))
        atomic_store_explicit(&state->interleaved_exited,1u,memory_order_release);
    return result;
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

/* M15d real CNet NativeIO session-retirement preflight.
 * The first RTSP RECORD session has an outstanding interleaved echo; close
 * without receiving it. The same client storage then re-connects as a fresh
 * connection and must not observe bytes from the retired session.
 *
 * The test uses the ACTUAL RTSP TCP server/client and CNet receive path,
 * not an in-memory packet or mocked Socket. This does not yet prove a
 * concurrent RTSP request/cancel crossover or Source -> Codec drain.
 */
/* The monotonic deadline only bounds a failed test. The interleaving
 * itself is established by the two explicit atomic handshake flags. */
static int rtsp_test_wait_for_callback(_Atomic unsigned *flag) {
    uint64_t start=cmeta_monotonic_ms();
    while(!atomic_load_explicit(flag,memory_order_acquire)) {
        if(cmeta_monotonic_ms()-start>=RTSP_TEST_TIMEOUT_MS)return 0;
        cmeta_thread_yield();
    }
    return 1;
}

static void rtsp_test_cancel_with_pending_interleaved_and_reopen(void) {
    static const char sdp[] =
        "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=cancel\r\nt=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
        "a=control:trackID=0\r\n";
    static const uint8_t retired_payload[] =
        {0x80,0x60,0x10,0x01,0x31,0x32,0x33,0x34};
    static const uint8_t current_payload[] =
        {0x80,0x60,0x20,0x02,0xa1,0xa2,0xa3,0xa4};
    turbo_rtsp_push_announce_t announce;
    turbo_rtsp_push_track_t track;
    rtsp_test_state_t state;
    turbo_rtsp_server_t *server=NULL;
    turbo_rtsp_client_t *client=NULL;
    uint8_t payload[64]={0};
    uint8_t channel=0xffu;
    size_t count=0u;

    rtsp_test_state_init(&state);
    atomic_store_explicit(&state.hold_interleaved,1u,memory_order_release);
    server=rtsp_test_server_start(&state,TURBO_RTSP_CONTROL_TRANSPORT_TCP,
                                  RTSP_TEST_RECONNECT_PORT,NULL);
    check_not_null(server);
    if(!server)goto cleanup;
    client=rtsp_test_client_connect(TURBO_RTSP_CONTROL_TRANSPORT_TCP,
                                    RTSP_TEST_RECONNECT_PORT,NULL);
    check_not_null(client);
    if(!client)goto cleanup;
    announce.uri="rtsp://127.0.0.1/live/cancel";
    announce.sdp=sdp;announce.sdp_len=sizeof(sdp)-1u;
    track.control_uri="rtsp://127.0.0.1/live/cancel/trackID=0";
    track.rtp_channel=0u;track.rtcp_channel=1u;

    check_equal(turbo_rtsp_client_announce(client,&announce),0);
    check_equal(turbo_rtsp_client_setup_interleaved(client,&track),0);
    check_equal(turbo_rtsp_client_record(client),0);
    check_equal(turbo_rtsp_client_send_interleaved_frame(
                    client,0u,retired_payload,sizeof(retired_payload)),0);
    /* Actual CNet server callback MUST have entered with the old RTP
     * bytes. Hold its stack while the Owner closes the old client. The
     * callback's eventual echo (whether queued or rejected) belongs only
     * to that retired Session, never to the next connection. */
    if(!rtsp_test_wait_for_callback(&state.interleaved_entered)) {
        atomic_store_explicit(&state.interleaved_release,1u,memory_order_release);
        check_true(0);
        goto cleanup;
    }
    check_equal(rtsp_test_read(&state,&state.interleaved_count),1);
    turbo_rtsp_client_close(client);
    atomic_store_explicit(&state.interleaved_release,1u,memory_order_release);
    if(!rtsp_test_wait_for_callback(&state.interleaved_exited)) {
        check_true(0);
        goto cleanup;
    }
    /* Abort does not trigger TEARDOWN or a hidden Codec Flush. RTSP
     * client receive/send on the closed NativeIO session must refuse. */
    count=0u;
    check_equal(turbo_rtsp_client_recv_interleaved_frame(
                    client,&channel,payload,sizeof(payload),&count),-1);
    check_equal(turbo_rtsp_client_send_interleaved_frame(
                    client,0u,current_payload,sizeof(current_payload)),-1);
    check_equal(turbo_rtsp_client_teardown(client),-1);
    atomic_store_explicit(&state.hold_interleaved,0u,memory_order_release);
    check_equal(rtsp_test_read(&state,&state.interleaved_count),1);

    /* Fresh real CNet connection on the SAME client storage. New
     * RTSP Setup/Record is a distinct NativeIO session, not a retry
     * of the old RTP frame or an implicit Codec continuation. */
    check_equal(turbo_rtsp_client_connect(client),0);
    check_equal(turbo_rtsp_client_announce(client,&announce),0);
    check_equal(turbo_rtsp_client_setup_interleaved(client,&track),0);
    check_equal(turbo_rtsp_client_record(client),0);
    check_equal(turbo_rtsp_client_send_interleaved_frame(
                    client,0u,current_payload,sizeof(current_payload)),0);
    count=0u;
    check_equal(turbo_rtsp_client_recv_interleaved_frame(
                    client,&channel,payload,sizeof(payload),&count),0);
    check_equal(channel,1u);
    check_equal((int)count,(int)sizeof(current_payload));
    check_equal(memcmp(payload,current_payload,sizeof(current_payload)),0);
    check_equal(turbo_rtsp_client_teardown(client),0);
    check_equal(rtsp_test_read(&state,&state.announce_count),2);
    check_equal(rtsp_test_read(&state,&state.record_count),2);
    check_equal(rtsp_test_read(&state,&state.interleaved_count),2);
cleanup:
    /* Always release a blocked handler before destroying its owner. */
    atomic_store_explicit(&state.interleaved_release,1u,memory_order_release);
    if(client) {
        turbo_rtsp_client_close(client);
        turbo_rtsp_client_destroy(client);
    }
    if(server)turbo_rtsp_server_destroy(server);
    if(server)check(rtsp_test_read(&state,&state.close_count)>=1);
    rtsp_test_state_destroy(&state);
}


/* A separate real CNet owner exercises the actual pending receive DEMAND
 * against the real RTSP server. No CNet operation overlaps another on this
 * owner. The receipt from the CNet pending-demand API proves outstanding
 * demand before the Owner submits close; time only bounds terminal polling.
 *
 * This is stronger than an idle TCP socket smoke, but it is NOT a
 * concurrent same-CNet-object poll/close race: that is disallowed by CNet.
 */
typedef struct rtsp_cnet_pending_fixture {
    cnet_connection handle;
    unsigned connected,closed,failed,received,wrong_identity,overflow;
    /* Only the same CNet progress Owner can use owner. Called from an
     * actual inline receive callback to verify callback-issued close is
     * deferred without early recycled request slots / session storage. */
    cnet_client *owner;
    unsigned close_on_receive,close_issued,inside_receive,closed_inside_receive;
    /* Test-only callback order: receive must precede the nonrecursive CLOSED. */
    unsigned callback_sequence,receive_order,closed_order;
    int inline_close_status;
    size_t bytes;
    char reply[1024];
} rtsp_cnet_pending_fixture;

static void rtsp_cnet_pending_state(void *user,cnet_connection conn,
                                    cnet_connection_state state,
                                    const cnet_error *error) {
    rtsp_cnet_pending_fixture *f=(rtsp_cnet_pending_fixture *)user;
    (void)error;
    if(conn.slot!=f->handle.slot || conn.generation!=f->handle.generation) {
        ++f->wrong_identity;
        return;
    }
    if(state==CNET_CONNECTION_CONNECTED)++f->connected;
    else if(state==CNET_CONNECTION_CLOSED) {
        f->closed_order=++f->callback_sequence;
        ++f->closed;
        if(f->inside_receive)++f->closed_inside_receive;
    } else if(state==CNET_CONNECTION_FAILED)++f->failed;
}
static void rtsp_cnet_pending_receive(void *user,cnet_connection conn,
                                      const cnet_receive_view *view) {
    rtsp_cnet_pending_fixture *f=(rtsp_cnet_pending_fixture *)user;
    if(conn.slot!=f->handle.slot || conn.generation!=f->handle.generation) {
        ++f->wrong_identity;
        return;
    }
    f->receive_order=++f->callback_sequence;
    ++f->received;
    if(!view || !view->data || view->size>sizeof(f->reply)-1u-f->bytes) {
        ++f->overflow;
        return;
    }
    memcpy(f->reply+f->bytes,view->data,view->size);
    f->bytes+=view->size;
    f->reply[f->bytes]='\0';
    if(f->close_on_receive && !f->close_issued && f->owner) {
        /* True CNet callback-issued close: the actual TCP OPTIONS reply
         * has already reached this callback. CNet's callback guard must
         * defer close admission until its current event batch is safe. */
        f->inside_receive=1u;
        f->inline_close_status=cnet_close(f->owner,conn);
        ++f->close_issued;
        f->inside_receive=0u;
    }
}
static void rtsp_cnet_pending_owned_probe(void *user,cnet_connection conn,
                                           mem_slice_t slice,
                                           cnet_message_kind kind) {
    /* No callback is allowed: replacing an in-flight demand must fail
     * SALTS_EBUSY. Release defensively if a broken implementation calls it. */
    (void)user;(void)conn;(void)kind;
    mem_slice_release(&slice);
}
static int rtsp_cnet_poll_to(cnet_client *owner,unsigned *counter) {
    uint64_t start=cmeta_monotonic_ms();
    while(!*counter) {
        size_t events=0u;
        if(cnet_client_poll(owner,10u,&events)!=SALTS_OK)return 0;
        if(cmeta_monotonic_ms()-start>=RTSP_TEST_TIMEOUT_MS)return 0;
    }
    return 1;
}
static void rtsp_test_same_cnet_owner_pending_receive_cancel(void) {
    static const char uri[]="tcp://127.0.0.1:20560";
    static const char retired_request[]=
        "OPTIONS rtsp://127.0.0.1/live/retired RTSP/1.0\r\n"
        "CSeq: 193\r\n\r\n";
    static const char request[]=
        "OPTIONS rtsp://127.0.0.1/live RTSP/1.0\r\n"
        "CSeq: 194\r\n\r\n";
    rtsp_test_state_t state;
    turbo_rtsp_server_t *server=NULL;
    cnet_client client={0};
    cnet_client_config cfg;
    cnet_connect_options options={0};
    cnet_connection old_handle={0},new_handle={0};
    rtsp_cnet_pending_fixture old={0},current={0};
    mem_buffer_t *send_buffer=NULL;
    int client_initialized=0;
    int result;
    size_t events=0u;

    rtsp_test_state_init(&state);
    atomic_store_explicit(&state.hold_options,1u,memory_order_release);
    server=rtsp_test_server_start(&state,TURBO_RTSP_CONTROL_TRANSPORT_TCP,
                                  20560,NULL);
    check_not_null(server);
    if(!server)goto cleanup;
    cfg=rtsp_test_chttp_network(1u);
    result=cnet_client_init(&client,&cfg);
    check_equal(result,SALTS_OK);
    if(result!=SALTS_OK)goto cleanup;
    client_initialized=1;
    options.uri=uri;
    options.observer.on_state=rtsp_cnet_pending_state;
    options.observer.on_receive=rtsp_cnet_pending_receive;
    options.observer.user=&old;
    result=cnet_connect(&client,&options,&old_handle);
    check_equal(result,SALTS_OK);
    if(result!=SALTS_OK)goto cleanup;
    old.handle=old_handle;
    if(!rtsp_cnet_poll_to(&client,&old.connected)) {
        check_true(0);goto cleanup;
    }
    check_equal(old.failed,0u);
    /* An accepted receive request with no remote response is truly
     * outstanding: CNet refuses changing its handler while demand exists. */
    check_equal(cnet_receive(&client,old_handle,1u),SALTS_OK);
    check_equal(cnet_client_poll(&client,0u,&events),SALTS_OK);
    check_equal(cnet_set_receive_slice_handler(
        &client,old_handle,rtsp_cnet_pending_owned_probe,&old),SALTS_EBUSY);
    check_equal(old.received,0u);
    /* The real RTSP server receives the old OPTIONS request and stops
     * INSIDE its server callback before any old response is generated. */
    send_buffer=mem_get_buffer(mem_global(),sizeof(retired_request)-1u);
    check_not_null(send_buffer);
    if(!send_buffer)goto cleanup;
    memcpy(mem_buffer_data(send_buffer),retired_request,
           sizeof(retired_request)-1u);
    mem_set_used(send_buffer,sizeof(retired_request)-1u);
    result=cnet_send_buffer(&client,old_handle,send_buffer);
    mem_buffer_release(send_buffer);send_buffer=NULL;
    check_equal(result,SALTS_OK);
    if(result!=SALTS_OK)goto cleanup;
    if(!rtsp_test_wait_for_callback(&state.options_entered)) {
        check_true(0);goto cleanup;
    }
    check_equal(old.received,0u);
    /* Only the Client Owner operates this CNet instance. NativeIO is
     * cancelled while the server still owns the old request callback. */
    check_equal(cnet_close(&client,old_handle),SALTS_OK);
    if(!rtsp_cnet_poll_to(&client,&old.closed)) {
        check_true(0);goto cleanup;
    }
    check_equal(old.closed,1u);
    check_equal(old.received,0u);
    check_equal(old.wrong_identity,0u);
    check_equal(old.overflow,0u);
    atomic_store_explicit(&state.options_release,1u,memory_order_release);
    if(!rtsp_test_wait_for_callback(&state.options_exited)) {
        check_true(0);goto cleanup;
    }
    check_equal(atomic_load_explicit(&state.options_timed_out,
                                    memory_order_acquire),0u);
    atomic_store_explicit(&state.hold_options,0u,memory_order_release);
    /* Reuse the ONE CNet connection slot; this is a different checked
     * Generation, never an old packet delivered to a new observer. */
    options.observer.user=&current;
    result=cnet_connect(&client,&options,&new_handle);
    check_equal(result,SALTS_OK);
    if(result!=SALTS_OK)goto cleanup;
    current.handle=new_handle;
    check_equal(new_handle.slot,old_handle.slot);
    check_true(new_handle.generation!=old_handle.generation);
    if(!rtsp_cnet_poll_to(&client,&current.connected)) {
        check_true(0);goto cleanup;
    }
    check_equal(cnet_receive(&client,old_handle,1u),SALTS_ENOENT);
    check_equal(cnet_close(&client,old_handle),SALTS_ENOENT);

    check_equal(cnet_receive(&client,new_handle,1u),SALTS_OK);
    send_buffer=mem_get_buffer(mem_global(),sizeof(request)-1u);
    check_not_null(send_buffer);
    if(!send_buffer)goto cleanup;
    memcpy(mem_buffer_data(send_buffer),request,sizeof(request)-1u);
    mem_set_used(send_buffer,sizeof(request)-1u);
    result=cnet_send_buffer(&client,new_handle,send_buffer);
    mem_buffer_release(send_buffer);send_buffer=NULL;
    check_equal(result,SALTS_OK);
    if(result!=SALTS_OK)goto cleanup;
    if(!rtsp_cnet_poll_to(&client,&current.received)) {
        check_true(0);goto cleanup;
    }
    check_true(current.bytes>=strlen("RTSP/1.0 200 OK"));
    check_true(strstr(current.reply,"RTSP/1.0 200 OK")==current.reply);
    check_equal(rtsp_test_read(&state,&state.options_count),2);
    check_equal(old.received,0u);
    check_equal(current.failed,0u);
    check_equal(current.wrong_identity,0u);
    check_equal(current.overflow,0u);
    check_equal(cnet_close(&client,new_handle),SALTS_OK);
    if(!rtsp_cnet_poll_to(&client,&current.closed))check_true(0);
    check_equal(current.closed,1u);
cleanup:
    /* No blocked server callback may survive owner/fixture destruction. */
    atomic_store_explicit(&state.options_release,1u,memory_order_release);
    if(send_buffer)mem_buffer_release(send_buffer);
    if(client_initialized) {
        /* Stop drives concrete terminal progress before freeing observers. */
        check_equal(cnet_client_stop(&client,RTSP_TEST_TIMEOUT_MS),SALTS_OK);
        check_equal(cnet_client_destroy(&client),SALTS_OK);
    }
    if(server)turbo_rtsp_server_destroy(server);
    rtsp_test_state_destroy(&state);
}


/* Actual CNet receive completion -> callback-issued close -> terminal ->
 * recycled connection slot. Unlike the earlier no-response demand test,
 * this exercises CNet's deferred callback admission path with a real
 * RTSP server reply already being delivered by NativeIO.
 */
static void rtsp_test_cnet_receive_callback_requests_close(void) {
    static const char uri[]="tcp://127.0.0.1:20561";
    static const char old_request[]=
        "OPTIONS rtsp://127.0.0.1/live/retired RTSP/1.0\r\n"
        "CSeq: 195\r\n\r\n";
    static const char new_request[]=
        "OPTIONS rtsp://127.0.0.1/live/current RTSP/1.0\r\n"
        "CSeq: 196\r\n\r\n";
    rtsp_test_state_t state;
    turbo_rtsp_server_t *server=NULL;
    cnet_client owner={0};
    cnet_client_config cfg=rtsp_test_chttp_network(1u);
    cnet_connect_options connect={0};
    cnet_connection old_handle={0},new_handle={0};
    rtsp_cnet_pending_fixture retired={0},current={0};
    mem_buffer_t *payload=NULL;
    int initialized=0;
    int result;

    rtsp_test_state_init(&state);
    server=rtsp_test_server_start(&state,TURBO_RTSP_CONTROL_TRANSPORT_TCP,
                                  20561,NULL);
    check_not_null(server);
    if(!server)goto cleanup;
    result=cnet_client_init(&owner,&cfg);
    check_equal(result,SALTS_OK);
    if(result!=SALTS_OK)goto cleanup;
    initialized=1;
    connect.uri=uri;
    connect.observer.on_state=rtsp_cnet_pending_state;
    connect.observer.on_receive=rtsp_cnet_pending_receive;
    connect.observer.user=&retired;
    result=cnet_connect(&owner,&connect,&old_handle);
    check_equal(result,SALTS_OK);
    if(result!=SALTS_OK)goto cleanup;
    retired.handle=old_handle;retired.owner=&owner;
    retired.close_on_receive=1u;
    retired.inline_close_status=SALTS_EINVAL;
    if(!rtsp_cnet_poll_to(&owner,&retired.connected)) {
        check_true(0);goto cleanup;
    }
    check_equal(cnet_receive(&owner,old_handle,1u),SALTS_OK);
    payload=mem_get_buffer(mem_global(),sizeof(old_request)-1u);
    check_not_null(payload);
    if(!payload)goto cleanup;
    memcpy(mem_buffer_data(payload),old_request,sizeof(old_request)-1u);
    mem_set_used(payload,sizeof(old_request)-1u);
    result=cnet_send_buffer(&owner,old_handle,payload);
    mem_buffer_release(payload);payload=NULL;
    check_equal(result,SALTS_OK);
    if(result!=SALTS_OK)goto cleanup;
    /* The old CNet receive callback itself issues close while the
     * matching NativeIO completion is being dispatched inline. */
    if(!rtsp_cnet_poll_to(&owner,&retired.closed)) {
        check_true(0);goto cleanup;
    }
    check_equal(retired.close_issued,1u);
    check_equal(retired.inline_close_status,SALTS_OK);
    check_equal(retired.received,1u);
    check_equal(retired.closed,1u);
    check_equal(retired.closed_inside_receive,0u);
    check_equal(retired.callback_sequence,2u);
    check_equal(retired.receive_order,1u);
    check_equal(retired.closed_order,2u);
    check_equal(retired.wrong_identity,0u);
    check_equal(retired.failed,0u);
    check_equal(retired.overflow,0u);
    check_true(strstr(retired.reply,"RTSP/1.0 200 OK")==retired.reply);
    check_not_null(strstr(retired.reply,"CSeq: 195"));

    /* Same bounded CNet owner and connection slot, new Generation. No
     * old on_receive/on_state callback or bytes may be routed to it. */
    connect.observer.user=&current;
    result=cnet_connect(&owner,&connect,&new_handle);
    check_equal(result,SALTS_OK);
    if(result!=SALTS_OK)goto cleanup;
    current.handle=new_handle;current.owner=&owner;
    check_equal(new_handle.slot,old_handle.slot);
    check_true(new_handle.generation!=old_handle.generation);
    if(!rtsp_cnet_poll_to(&owner,&current.connected)) {
        check_true(0);goto cleanup;
    }
    check_equal(cnet_receive(&owner,old_handle,1u),SALTS_ENOENT);
    check_equal(cnet_close(&owner,old_handle),SALTS_ENOENT);
    check_equal(cnet_receive(&owner,new_handle,1u),SALTS_OK);
    payload=mem_get_buffer(mem_global(),sizeof(new_request)-1u);
    check_not_null(payload);
    if(!payload)goto cleanup;
    memcpy(mem_buffer_data(payload),new_request,sizeof(new_request)-1u);
    mem_set_used(payload,sizeof(new_request)-1u);
    result=cnet_send_buffer(&owner,new_handle,payload);
    mem_buffer_release(payload);payload=NULL;
    check_equal(result,SALTS_OK);
    if(result!=SALTS_OK)goto cleanup;
    if(!rtsp_cnet_poll_to(&owner,&current.received)) {
        check_true(0);goto cleanup;
    }
    check_equal(current.received,1u);
    check_true(strstr(current.reply,"RTSP/1.0 200 OK")==current.reply);
    check_not_null(strstr(current.reply,"CSeq: 196"));
    check_equal(current.close_issued,0u);
    check_equal(current.closed,0u);
    check_equal(current.failed,0u);
    check_equal(current.overflow,0u);
    check_equal(current.wrong_identity,0u);
    check_equal(retired.received,1u);
    check_equal(retired.closed,1u);
    check_equal(retired.callback_sequence,2u);
    check_equal(retired.wrong_identity,0u);
    check_equal(rtsp_test_read(&state,&state.options_count),2);
    check_equal(cnet_close(&owner,new_handle),SALTS_OK);
    if(!rtsp_cnet_poll_to(&owner,&current.closed))check_true(0);
    check_equal(current.closed,1u);
cleanup:
    if(payload)mem_buffer_release(payload);
    if(initialized) {
        check_equal(cnet_client_stop(&owner,RTSP_TEST_TIMEOUT_MS),SALTS_OK);
        check_equal(cnet_client_destroy(&owner),SALTS_OK);
    }
    if(server)turbo_rtsp_server_destroy(server);
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

#ifdef TM_TEST_RTSP_MPEG2_CODEC_GATE
#include "test_rtsp_mpeg_video_codec.inc"
#ifdef TM_TEST_RTSP_MPEG2_PROVIDER_DSO
#include "test_rtsp_mpeg_video_component.inc"
#endif
#endif

suite("TurboMedia RTSP over Salts") {
    it("runs the complete play control flow over CNet TCP") {
        rtsp_test_play_flow();
    }

    it("runs RECORD and interleaved media over CNet TCP") {
        rtsp_test_record_flow(
            TURBO_RTSP_CONTROL_TRANSPORT_TCP, RTSP_TEST_TCP_PORT, NULL);
    }

    it("runs a direct CHTTP WebSocket smoke contract") {
        rtsp_test_chttp_websocket_smoke();
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
    it("abandons pending interleaved RTP on CNet close and fences reconnection") {
        rtsp_test_cancel_with_pending_interleaved_and_reopen();
    }
    it("cancels CNet receive before delayed real RTSP server response and fences reused generation") {
        rtsp_test_same_cnet_owner_pending_receive_cancel();
    }
    it("closes from real CNet receive callback before generation reuse") {
        rtsp_test_cnet_receive_callback_requests_close();
    }
#ifdef TM_TEST_RTSP_MPEG2_CODEC_GATE
    it("decodes real RFC2250 MPEG-2 RTSP RTP and aborts without Codec flush") {
        rtsp_test_mpeg2_video_real_rtp_to_delayed_codec();
    }
#ifdef TM_TEST_RTSP_MPEG2_PROVIDER_DSO
    it("settles real RTSP MPEG-2 packets through Salts CMeta DSO Portal Generations") {
        rtsp_test_mpeg2_real_rtp_to_salts_generation();
    }
#endif
#endif

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
