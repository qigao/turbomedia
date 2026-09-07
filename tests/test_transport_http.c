#include <tinytest.h>

#include <chttp/chttp.h>
#include <salts/error_codes.h>
#include <turbo_transport.h>

#include <stdlib.h>
#include <string.h>

enum { TRANSPORT_HTTP_TEST_PORT = 20921 };

typedef struct {
    chttp_server server;
    int request_valid;
} transport_http_test_state_t;

typedef struct {
    int connected;
    int disconnected;
} transport_event_probe_t;

static native_io_backend_kind transport_http_test_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static int transport_http_handler(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response) {
    transport_http_test_state_t *state =
        (transport_http_test_state_t *)user;
    const char *authorization =
        chttp_server_request_header(request, "Authorization");
    const char *user_agent = chttp_server_request_header(request, "User-Agent");
    const char *content_type =
        chttp_server_request_header(request, "Content-Type");

    state->request_valid =
        request->method == CHTTP_METHOD_POST &&
        strcmp(request->target, "/base/api/v1/commands") == 0 &&
        authorization && strcmp(authorization, "Bearer original-token") == 0 &&
        user_agent && strcmp(user_agent, "TurboMediaTransportTest/1") == 0 &&
        content_type && strcmp(content_type, "application/json") == 0 &&
        request->body_size == 2u && memcmp(request->body, "{}", 2u) == 0;
    return chttp_server_reply(response, state->request_valid ? 200u : 400u,
                              "application/json", "{}", 2u);
}

static int transport_http_server_start(transport_http_test_state_t *state) {
    chttp_server_config config = {
        .host = "127.0.0.1",
        .port = TRANSPORT_HTTP_TEST_PORT,
        .backlog = 4u,
        .network = {
            .backend = transport_http_test_backend(),
            .connection_capacity = 4u,
            .command_capacity = 8u,
            .request_capacity = 8u,
            .completion_batch_capacity = 4u,
            .event_capacity = 8u,
            .max_send_bytes = 16u * 1024u,
            .receive_buffer_bytes = 8u * 1024u,
            .connect_timeout_ms = 5000u,
            .read_timeout_ms = 5000u,
            .write_timeout_ms = 5000u
        },
        .route_capacity = 2u,
        .max_target_bytes = 1024u,
        .max_header_count = 16u,
        .max_header_bytes = 8u * 1024u,
        .max_request_body_bytes = 1024u,
        .max_response_header_count = 8u,
        .max_response_header_bytes = 4096u,
        .max_response_body_bytes = 1024u,
        .poll_slice_ms = 5u,
        .buffer_capacity_bytes = 64u * 1024u
    };
    if (chttp_server_init(&state->server, &config) != SALTS_OK ||
        chttp_server_post(&state->server, "/base/api/v1/commands",
                          transport_http_handler, state) != SALTS_OK ||
        chttp_server_start(&state->server) != SALTS_OK)
        return -1;
    return 0;
}

static cnet_client_config transport_external_client_config(void) {
    return (cnet_client_config){
        .backend = transport_http_test_backend(),
        .connection_capacity = 2u,
        .command_capacity = 8u,
        .request_capacity = 8u,
        .completion_batch_capacity = 8u,
        .event_capacity = 8u,
        .max_send_bytes = 16u * 1024u,
        .receive_buffer_bytes = 8u * 1024u,
        .connect_timeout_ms = 5000u,
        .read_timeout_ms = 5000u,
        .write_timeout_ms = 5000u
    };
}

static void transport_event_probe(
    turbo_transport_t *transport, turbo_transport_event_t event,
    void *event_data, void *user_data) {
    transport_event_probe_t *probe = (transport_event_probe_t *)user_data;
    (void)transport;
    (void)event_data;
    if (event == TURBO_TRANSPORT_EVENT_CONNECTED) {
        probe->connected += 1;
    } else if (event == TURBO_TRANSPORT_EVENT_DISCONNECTED) {
        probe->disconnected += 1;
    }
}

suite("Salts CHTTP transport") {
  it("dispatches generic transport APIs without crossing the HTTP layout") {
    turbo_transport_config_t config = {0};
    transport_event_probe_t probe = {0};
    turbo_transport_t *transport;
    cnet_connection connection = {0};
    uint8_t byte = 0u;
    uint8_t *received = NULL;
    size_t received_size = 0u;

    check_equal(turbo_transport_parse_url(
                    "http://127.0.0.1:20921/base", &config), 0);
    transport = turbo_transport_create(&config);
    free((void *)config.host);
    free((void *)config.path);
    check_not_null(transport);

    turbo_transport_set_event_callback(transport, transport_event_probe,
                                       &probe);
    check_equal(turbo_transport_connect(transport), 0);
    check_true(turbo_transport_is_connected(transport));
    check_equal(probe.connected, 1);
    check_equal(turbo_transport_send(transport, &byte, 1u), -1);
    check_equal(turbo_transport_recv(transport, &received, &received_size), -1);
    check_equal(turbo_transport_get_connection(transport, &connection), -1);
    check_equal(turbo_transport_disconnect(transport), 0);
    check_false(turbo_transport_is_connected(transport));
    check_equal(probe.disconnected, 1);

    check_equal(turbo_transport_destroy(transport), 0);
  }

  it("retains request configuration and prefixes the base URL path") {
    transport_http_test_state_t state = {0};
    turbo_transport_config_t config = {0};
    turbo_transport_t *transport;
    chttp_response *response;
    char token[] = "original-token";
    char user_agent[] = "TurboMediaTransportTest/1";
    const char *headers[] = {"Content-Type", "application/json"};

    check_equal(transport_http_server_start(&state), 0);
    check_equal(turbo_transport_parse_url(
                    "http://127.0.0.1:20921/base", &config), 0);
    config.auth_token = token;
    config.user_agent = user_agent;
    config.connect_timeout_ms = 5000;
    config.read_timeout_ms = 5000;
    config.write_timeout_ms = 5000;
    transport = turbo_transport_create(&config);
    free((void *)config.host);
    free((void *)config.path);
    check_not_null(transport);

    memset(token, 'x', sizeof(token) - 1u);
    memset(user_agent, 'y', sizeof(user_agent) - 1u);
    response = turbo_transport_http_request(
        transport, TURBO_HTTP_POST, "/api/v1/commands",
        (const uint8_t *)"{}", 2u, headers, 2);
    check_not_null(response);
    check_equal((int)response->status_code, 200);
    check_equal(state.request_valid, 1);

    chttp_response_destroy(response);
    free(response);
    check_equal(turbo_transport_destroy(transport), 0);
    check_equal(chttp_server_stop(&state.server, 5000u), SALTS_OK);
    check_equal(chttp_server_destroy(&state.server), SALTS_OK);
  }

  it("drains an external CNet connection before releasing callback state") {
    transport_http_test_state_t state = {0};
    transport_event_probe_t probe = {0};
    cnet_client client = {0};
    cnet_client_config client_config = transport_external_client_config();
    turbo_transport_config_t config = {
        .type = TURBO_TRANSPORT_TCP,
        .host = "127.0.0.1",
        .port = TRANSPORT_HTTP_TEST_PORT,
        .connect_timeout_ms = 5000,
        .read_timeout_ms = 100,
        .write_timeout_ms = 5000,
        .cnet_client = &client
    };
    turbo_transport_t *transport;
    size_t events = 0u;

    check_equal(transport_http_server_start(&state), 0);
    check_equal(cnet_client_init(&client, &client_config), SALTS_OK);
    transport = turbo_transport_create(&config);
    check_not_null(transport);
    turbo_transport_set_event_callback(transport, transport_event_probe,
                                       &probe);
    check_equal(turbo_transport_connect(transport), 0);
    check_equal(probe.connected, 1);

    check_equal(turbo_transport_destroy(transport), 0);
    for (int attempt = 0; attempt < 8; ++attempt) {
        check_equal(cnet_client_poll(&client, 10u, &events), SALTS_OK);
    }
    check_equal(probe.disconnected, 1);

    check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&state.server, 5000u), SALTS_OK);
    check_equal(chttp_server_destroy(&state.server), SALTS_OK);
  }

  it("reconnects a disconnected CNet transport without stale terminal state") {
    transport_http_test_state_t state = {0};
    transport_event_probe_t probe = {0};
    cnet_client client = {0};
    cnet_client_config client_config = transport_external_client_config();
    turbo_transport_config_t config = {
        .type = TURBO_TRANSPORT_TCP,
        .host = "127.0.0.1",
        .port = TRANSPORT_HTTP_TEST_PORT,
        .connect_timeout_ms = 5000,
        .read_timeout_ms = 100,
        .write_timeout_ms = 5000,
        .cnet_client = &client
    };
    turbo_transport_t *transport;

    check_equal(transport_http_server_start(&state), 0);
    check_equal(cnet_client_init(&client, &client_config), SALTS_OK);
    transport = turbo_transport_create(&config);
    check_not_null(transport);
    turbo_transport_set_event_callback(transport, transport_event_probe,
                                       &probe);

    check_equal(turbo_transport_connect(transport), 0);
    check_equal(turbo_transport_disconnect(transport), 0);
    check_equal(turbo_transport_connect(transport), 0);
    check_equal(turbo_transport_disconnect(transport), 0);
    check_equal(probe.connected, 2);
    check_equal(probe.disconnected, 2);

    check_equal(turbo_transport_destroy(transport), 0);
    check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&state.server, 5000u), SALTS_OK);
    check_equal(chttp_server_destroy(&state.server), SALTS_OK);
  }
}
