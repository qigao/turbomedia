#include <tinytest.h>

#include <chttp_app/application.h>
#include <core/error_codes.h>
#include <turbo_streamer.h>
#include <cstl/vec.h>

#include <stdint.h>
#include <string.h>

enum {
    HTTP_FLV_TEST_PORT = 20920,
    HTTP_FLV_TEST_QUEUE_CAPACITY = 64 * 1024,
    HTTP_FLV_TEST_PACKET_COUNT = 3,
    HTTP_FLV_TEST_PACKET_BYTES = 160
};

typedef struct {
    chttp_application server;
    salts_configurator_provider_binding provider;
    salts_configurator_deployment deployment;
    chttp_controller controller;
    vec_t received;
    int saw_chunked;
    int handler_result;
} http_flv_test_state_t;

static native_io_backend_kind http_flv_test_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

cmeta_reflect_data(http_flv_test_state_t, "turbomedia.test.HttpFlv",
                   cmeta_field(int, handler_result));
cmeta_component_empty(TurboHttpFlvTest);

static cmeta_status http_flv_test_provider(
    void *user, const cmeta_data_desc *data, const void *config,
    const salts_configurator_dependency *dependencies, size_t count,
    cmeta_object_ref *out) {
    (void)data;
    (void)config;
    (void)dependencies;
    if (count != 0u) {
        return CMETA_INVALID_ARGUMENT;
    }
    return cmeta_object_borrow(out, user, cmeta_reflected_data(http_flv_test_state_t),
                               NULL);
}

CHTTP_ACTION(http_flv_upload_handler);
int http_flv_upload_handler(void *user, const chttp_invocation *invocation) {
    const chttp_server_request_view *request = chttp_invocation_request(invocation);
    http_flv_test_state_t *state = (http_flv_test_state_t *)user;
    const char *transfer_encoding;
    if (!state || !request || !request->body || request->body_size == 0 ||
        vec_resize(&state->received, request->body_size) != STL_OK) {
        if (state) state->handler_result = -1;
        return chttp_invocation_respond(invocation, 500u, "text/plain", "error", 5u);
    }
    memcpy(vec_data(&state->received), request->body, request->body_size);
    transfer_encoding = chttp_server_request_header(request, "Transfer-Encoding");
    state->saw_chunked = transfer_encoding &&
                         strcmp(transfer_encoding, "chunked") == 0;
    state->handler_result = 0;
    return chttp_invocation_respond(invocation, 200u, "text/plain", "ok", 2u);
}

static int http_flv_test_server_start(http_flv_test_state_t *state) {
    chttp_server_config config = {
        .host = "127.0.0.1",
        .port = HTTP_FLV_TEST_PORT,
        .backlog = 4u,
        .network = {
            .backend = http_flv_test_backend(),
            .connection_capacity = 4u,
            .command_capacity = 8u,
            .request_capacity = 8u,
            .completion_batch_capacity = 4u,
            .event_capacity = 8u,
            .max_send_bytes = 64u * 1024u,
            .receive_buffer_bytes = 8u * 1024u,
            .connect_timeout_ms = 5000u,
            .read_timeout_ms = 5000u,
            .write_timeout_ms = 5000u
        },
        .route_capacity = 2u,
        .max_target_bytes = 1024u,
        .max_header_count = 16u,
        .max_header_bytes = 8u * 1024u,
        .max_request_body_bytes = HTTP_FLV_TEST_QUEUE_CAPACITY,
        .max_response_header_count = 8u,
        .max_response_header_bytes = 4096u,
        .max_response_body_bytes = 1024u,
        .poll_slice_ms = 5u,
        .buffer_capacity_bytes = 256u * 1024u
    };
    static const chttp_action action = CHTTP_POST("/live.flv", http_flv_upload_handler);
    chttp_application_config app = chttp_application_config_default();
    int status;

    state->provider = (salts_configurator_provider_binding){
        sizeof(state->provider), SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
        cmeta_component_meta(TurboHttpFlvTest), state, NULL,
        http_flv_test_provider, NULL, NULL};
    state->deployment = (salts_configurator_deployment){&state->provider, NULL, NULL};
    state->controller = (chttp_controller){
        .component_id = "TurboHttpFlvTest",
        .receiver = cmeta_reflected_data(http_flv_test_state_t),
        .actions = &action, .action_count = 1u};
    app.http = config;
    app.workers = (chttp_worker_config){
        .workers = 1u, .capacity = 4u, .retained_bytes = config.buffer_capacity_bytes,
        .reply_bytes = config.max_response_body_bytes,
        .header_capacity = config.max_response_header_count,
        .header_bytes = config.max_response_header_bytes};
    app.components = &state->deployment;
    app.component_count = 1u;
    app.dependency_capacity = 1u;
    app.controllers = &state->controller;
    app.controller_count = 1u;
    status = chttp_application_init(&state->server, &app);
    if (status == SALTS_OK) {
        status = chttp_application_start(&state->server);
    }
    if (status != SALTS_OK && state->server.impl) {
        (void)chttp_application_close(&state->server, 0u);
    }
    return status == SALTS_OK ? 0 : -1;
}

suite("local HTTP-FLV chunked push") {
  it("streams one FLV request through CNet") {
    http_flv_test_state_t state = {0};
    turbo_streamer_config_t config = {0};
    turbo_stream_info_t stream_info = {0};
    turbo_muxer_packet_t packet = {0};
    turbo_streamer_t *streamer = NULL;
    uint8_t pcmu[HTTP_FLV_TEST_PACKET_BYTES];
    const uint8_t *flv;
    int stream_id = -1;

    check_equal(vec_init_bytes(&state.received, sizeof(uint8_t),
                               CMETA_ALIGNOF(uint8_t),
                               HTTP_FLV_TEST_QUEUE_CAPACITY), STL_OK);
    check_equal(http_flv_test_server_start(&state), 0);
    turbo_streamer_registry_init();
    config.protocol = TURBO_STREAMER_HTTP_FLV;
    config.url = "http://127.0.0.1:20920/live.flv";
    config.buffer_size = HTTP_FLV_TEST_QUEUE_CAPACITY;
    streamer = turbo_streamer_create(&config);
    check_not_null(streamer);

    stream_info.type = TURBO_CODEC_TYPE_AUDIO;
    stream_info.codec_name = "pcmu";
    stream_info.sample_rate = 8000;
    stream_info.channels = 1;
    check_equal(turbo_streamer_add_stream(streamer, &stream_info, &stream_id), 0);
    check_equal(turbo_streamer_connect(streamer), 0);
    memset(pcmu, 0x7f, sizeof(pcmu));
    packet.stream_id = stream_id;
    packet.data = pcmu;
    packet.size = sizeof(pcmu);
    packet.duration = 20000;
    for (int index = 0; index < HTTP_FLV_TEST_PACKET_COUNT; ++index) {
      packet.pts = index * packet.duration;
      packet.dts = packet.pts;
      check_equal(turbo_streamer_write_packet(streamer, &packet), 0);
    }
    check_equal(turbo_streamer_disconnect(streamer), 0);
    check_equal(chttp_application_close(&state.server, 5000u), SALTS_OK);
    check_equal(state.handler_result, 0);
    check_equal(state.saw_chunked, 1);
    check_greater(vec_size(&state.received), 5);
    flv = (const uint8_t *)vec_data_const(&state.received);
    check_equal(flv, "FLV", 3);
    check_equal(flv[3], 1);
    check_true((flv[4] & 0x04) != 0);

    check_equal(turbo_streamer_destroy(streamer), 0);
    turbo_streamer_registry_shutdown();
    vec_destroy(&state.received);
  }
}
