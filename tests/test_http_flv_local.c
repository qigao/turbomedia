#include <tinytest.h>

#include <CoroNet.h>
#include <http_client.h>
#include <turbo_streamer.h>
#include <turbostl/vec.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    HTTP_FLV_TEST_PORT = 20920,
    HTTP_FLV_TEST_QUEUE_CAPACITY = 64 * 1024,
    HTTP_FLV_TEST_PACKET_COUNT = 3,
    HTTP_FLV_TEST_PACKET_BYTES = 160
};

typedef struct {
    coro_context_t *coro_context;
    coro_socket_t *server;
    http_client_t *http_client;
    turbo_streamer_t *streamer;
    vec_t received;
    int handler_result;
    int test_result;
} http_flv_test_state_t;

static const uint8_t *http_flv_find_bytes(const uint8_t *data, size_t size,
                                          const uint8_t *needle,
                                          size_t needle_size) {
    size_t index;

    if (!data || !needle || needle_size == 0 || size < needle_size) return NULL;
    for (index = 0; index <= size - needle_size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return data + index;
    }
    return NULL;
}

static int http_flv_has_chunked_trailer(const uint8_t *data, size_t size) {
    static const uint8_t trailer[] = {'\r', '\n', '0', '\r', '\n', '\r', '\n'};
    return http_flv_find_bytes(data, size, trailer, sizeof(trailer)) != NULL;
}

static void http_flv_upload_handler(coro_socket_t *client, void *arg) {
    static const char response[] =
        "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n\r\nok";
    http_flv_test_state_t *state = (http_flv_test_state_t *)arg;
    char *chunk = NULL;
    size_t chunk_size = 0;
    size_t old_size;
    int result;

    if (!state) return;
    for (;;) {
        result = coro_socket_recv(client, &chunk, &chunk_size);
        if (result != 0 || !chunk || chunk_size == 0) {
            state->handler_result = -1;
            break;
        }
        old_size = vec_size(&state->received);
        if (chunk_size > SIZE_MAX - old_size ||
            vec_resize(&state->received, old_size + chunk_size) != STL_OK) {
            coro_socket_free_recv(chunk);
            state->handler_result = -1;
            break;
        }
        memcpy((uint8_t *)vec_data(&state->received) + old_size, chunk,
               chunk_size);
        coro_socket_free_recv(chunk);
        chunk = NULL;
        if (http_flv_has_chunked_trailer(
                (const uint8_t *)vec_data_const(&state->received),
                vec_size(&state->received))) {
            state->handler_result =
                coro_socket_send(client, response, sizeof(response) - 1);
            break;
        }
    }
}

static void http_flv_test_coro(coro_t *co, void *arg) {
    http_flv_test_state_t *state = (http_flv_test_state_t *)arg;
    turbo_streamer_config_t config = {0};
    turbo_stream_info_t stream_info = {0};
    turbo_muxer_packet_t packet = {0};
    uint8_t pcmu[HTTP_FLV_TEST_PACKET_BYTES];
    char base_url[64];
    int stream_id = -1;
    int connected = 0;
    int result;

    (void)co;
    state->server = coro_socket_create(state->coro_context, CORO_SOCKET_TCP_V4);
    if (!state->server ||
        coro_socket_listen_on(state->server, "127.0.0.1", HTTP_FLV_TEST_PORT,
                              http_flv_upload_handler, state) != 0)
        goto cleanup;
    coro_yield();

    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d",
             HTTP_FLV_TEST_PORT);
    state->http_client = http_client_create(base_url);
    if (!state->http_client) goto cleanup;

    turbo_streamer_registry_init();
    config.protocol = TURBO_STREAMER_HTTP_FLV;
    config.url = "/live.flv";
    config.buffer_size = HTTP_FLV_TEST_QUEUE_CAPACITY;
    config.coro_context = state->coro_context;
    config.http_client = state->http_client;
    state->streamer = turbo_streamer_create(&config);
    if (!state->streamer) goto cleanup;

    stream_info.type = TURBO_CODEC_TYPE_AUDIO;
    stream_info.codec_name = "pcmu";
    stream_info.sample_rate = 8000;
    stream_info.channels = 1;
    result = turbo_streamer_add_stream(state->streamer, &stream_info, &stream_id);
    if (result != 0) goto cleanup;
    result = turbo_streamer_connect(state->streamer);
    if (result != 0) goto cleanup;
    connected = 1;

    memset(pcmu, 0x7f, sizeof(pcmu));
    packet.stream_id = stream_id;
    packet.data = pcmu;
    packet.size = sizeof(pcmu);
    packet.duration = 20000;
    for (int index = 0; index < HTTP_FLV_TEST_PACKET_COUNT; ++index) {
        packet.pts = index * packet.duration;
        packet.dts = packet.pts;
        result = turbo_streamer_write_packet(state->streamer, &packet);
        if (result != 0) goto cleanup;
    }
    result = turbo_streamer_disconnect(state->streamer);
    connected = 0;
    if (result != 0 || state->handler_result != 0) goto cleanup;
    state->test_result = 1;

cleanup:
    if (connected && state->streamer) turbo_streamer_disconnect(state->streamer);
    turbo_streamer_destroy(state->streamer);
    state->streamer = NULL;
    turbo_streamer_registry_shutdown();
    http_client_destroy(state->http_client);
    state->http_client = NULL;
    if (state->server) coro_socket_destroy(state->server);
    state->server = NULL;
}

suite("local HTTP-FLV chunked push") {
    it("streams one FLV request through TurboHTTP") {
        http_flv_test_state_t state = {0};
        const uint8_t *received;
        const uint8_t *flv;
        size_t received_size;

        check_equal(vec_init_bytes(&state.received, sizeof(uint8_t), CMETA_ALIGNOF(uint8_t), SIZE_MAX / sizeof(uint8_t)), STL_OK);
        state.coro_context = coro_context_create(NULL);
        check_not_null(state.coro_context);
        if (!state.coro_context) goto cleanup;
        check_equal(coro_context_spawn(state.coro_context, http_flv_test_coro, &state),
                     0);
        check_equal(coro_context_run(state.coro_context, TURBO_RUN_DEFAULT), 0);
        check_equal(state.test_result, 1);
        check_equal(state.handler_result, 0);
        received = (const uint8_t *)vec_data_const(&state.received);
        received_size = vec_size(&state.received);
        check_not_null(received);
        check_greater(received_size, 13);
        check_not_null(http_flv_find_bytes(received, received_size,
                                           (const uint8_t *)"POST /live.flv", 14));
        check_not_null(http_flv_find_bytes(
            received, received_size, (const uint8_t *)"Transfer-Encoding: chunked", 26));
        flv = http_flv_find_bytes(received, received_size,
                                  (const uint8_t *)"FLV", 3);
        check_not_null(flv);
        if (flv && (size_t)(flv - received) + 5 <= received_size) {
            check_equal(flv, "FLV", 3);
            check_equal(flv[3], 1);
            check_true((flv[4] & 0x04) != 0);
        }

cleanup:
        if (state.server) coro_socket_destroy(state.server);
        if (state.coro_context) coro_context_destroy(state.coro_context);
        vec_destroy(&state.received);
    }
}
