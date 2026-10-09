#include <tinytest.h>

#include <http_client/http.h>
#include <http_server/http.h>
#include <salts/error_codes.h>
#include <turbo_transport.h>

#include <limits.h>
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

    {
        cnet_connection live = {0};
        check_equal(turbo_transport_get_connection(transport, &live), -1);
        check_equal(turbo_transport_connect(transport), 0);
        check_equal(turbo_transport_get_connection(transport, &live), 0);
        check_equal(turbo_transport_disconnect(transport), 0);
        check_equal(turbo_transport_get_connection(transport, &live), -1);
        check_equal(turbo_transport_connect(transport), 0);
        check_equal(turbo_transport_get_connection(transport, &live), 0);
        check_equal(turbo_transport_disconnect(transport), 0);
        check_equal(turbo_transport_get_connection(transport, &live), -1);
    }
    check_equal(probe.connected, 2);
    check_equal(probe.disconnected, 2);

    check_equal(turbo_transport_destroy(transport), 0);
    check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&state.server, 5000u), SALTS_OK);
    check_equal(chttp_server_destroy(&state.server), SALTS_OK);
  }

  it("sends retained stream bytes after rejecting an oversized write") {
    transport_http_test_state_t state = {0};
    cnet_client client = {0};
    cnet_client_config client_config = transport_external_client_config();
    turbo_transport_config_t config = {
        .type = TURBO_TRANSPORT_TCP,
        .host = "127.0.0.1",
        .port = TRANSPORT_HTTP_TEST_PORT,
        .connect_timeout_ms = 5000,
        .read_timeout_ms = 5000,
        .write_timeout_ms = 5000,
        .cnet_client = &client
    };
    uint8_t oversized[16u * 1024u + 1u] = {0};
    uint8_t request[] =
        "POST /base/api/v1/commands HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer original-token\r\n"
        "User-Agent: TurboMediaTransportTest/1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n\r\n{}";
    uint8_t *response = NULL;
    size_t response_size = 0u;
    turbo_transport_t *transport;

    check_equal(transport_http_server_start(&state), 0);
    check_equal(cnet_client_init(&client, &client_config), SALTS_OK);
    transport = turbo_transport_create(&config);
    check_not_null(transport);
    check_equal(turbo_transport_connect(transport), 0);
    check_equal(turbo_transport_send(transport, oversized, sizeof(oversized)), -1);
    check_equal(turbo_transport_send(transport, request, sizeof(request) - 1u),
                (int)(sizeof(request) - 1u));
    memset(request, 0, sizeof(request));
    check_greater(turbo_transport_recv(transport, &response, &response_size), 0);
    check_not_null(response);
    turbo_transport_free_recv(transport, response);
    check_equal(turbo_transport_destroy(transport), 0);
    check_equal(cnet_client_stop(&client, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&client), SALTS_OK);
    check_equal(chttp_server_stop(&state.server, 5000u), SALTS_OK);
    check_equal(chttp_server_destroy(&state.server), SALTS_OK);
    check_equal(state.request_valid, 1);
  }
  it("keeps a neighboring managed TCP transport live on a shared CNet owner") {
    transport_http_test_state_t state = {0};
    transport_event_probe_t first_probe = {0};
    transport_event_probe_t second_probe = {0};
    cnet_client owner = {0};
    cnet_client_config owner_config = transport_external_client_config();
    turbo_transport_config_t config = {
        .type = TURBO_TRANSPORT_TCP,
        .host = "127.0.0.1",
        .port = TRANSPORT_HTTP_TEST_PORT,
        .connect_timeout_ms = 5000,
        .read_timeout_ms = 5000,
        .write_timeout_ms = 5000,
        .cnet_client = &owner
    };
    const char request[] =
        "POST /base/api/v1/commands HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer original-token\r\n"
        "User-Agent: TurboMediaTransportTest/1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n\r\n{}";
    turbo_transport_t *first;
    turbo_transport_t *second;
    uint8_t *response = NULL;
    size_t response_size = 0u;
    size_t events = 0u;

    check_equal(transport_http_server_start(&state), 0);
    check_equal(cnet_client_init(&owner, &owner_config), SALTS_OK);
    first = turbo_transport_create(&config);
    second = turbo_transport_create(&config);
    check_not_null(first);
    check_not_null(second);
    turbo_transport_set_event_callback(first, transport_event_probe, &first_probe);
    turbo_transport_set_event_callback(second, transport_event_probe, &second_probe);
    check_equal(turbo_transport_connect(first), 0);
    check_equal(turbo_transport_connect(second), 0);
    check_equal(first_probe.connected, 1);
    check_equal(second_probe.connected, 1);
    check_equal(turbo_transport_disconnect(first), 0);
    check_equal(turbo_transport_destroy(first), 0);
    check_true(turbo_transport_is_connected(second));
    check_equal(second_probe.disconnected, 0);
    check_equal(turbo_transport_send(second, (const uint8_t *)request,
                                     sizeof(request) - 1u),
                (int)(sizeof(request) - 1u));
    check_greater(turbo_transport_recv(second, &response, &response_size), 0);
    check_not_null(response);
    turbo_transport_free_recv(second, response);
    check_equal(state.request_valid, 1);
    check_equal(turbo_transport_destroy(second), 0);
    for (int attempt = 0; attempt < 8; ++attempt)
        check_equal(cnet_client_poll(&owner, 10u, &events), SALTS_OK);
    check_equal(first_probe.disconnected, 1);
    check_equal(second_probe.disconnected, 1);
    check_equal(cnet_client_stop(&owner, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&owner), SALTS_OK);
    check_equal(chttp_server_stop(&state.server, 5000u), SALTS_OK);
    check_equal(chttp_server_destroy(&state.server), SALTS_OK);
  }

  it("retains one receive demand and the buffered response after timeout") {
    transport_http_test_state_t state = {0};
    cnet_client owner = {0};
    cnet_client_config owner_config = transport_external_client_config();
    turbo_transport_config_t config = {
        .type = TURBO_TRANSPORT_TCP,
        .host = "127.0.0.1",
        .port = TRANSPORT_HTTP_TEST_PORT,
        .connect_timeout_ms = 5000,
        .read_timeout_ms = 40,
        .write_timeout_ms = 5000,
        .cnet_client = &owner
    };
    const char request[] =
        "POST /base/api/v1/commands HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Authorization: Bearer original-token\r\n"
        "User-Agent: TurboMediaTransportTest/1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n\r\n{}";
    turbo_transport_t *transport;
    uint8_t *response = (uint8_t *)1;
    size_t response_size = 99u;

    check_equal(transport_http_server_start(&state), 0);
    check_equal(cnet_client_init(&owner, &owner_config), SALTS_OK);
    transport = turbo_transport_create(&config);
    check_not_null(transport);
    check_equal(turbo_transport_connect(transport), 0);
    /* Explicit admission is bounded and idempotent before any request bytes.
     * The following recv must reuse this exact credit after timeout.
     */
    check_equal(turbo_transport_request_receive(transport), 0);
    check_equal(turbo_transport_request_receive(transport), 0);
    /* The temporary timeout does NOT cancel CNet's one outstanding demand. */
    check_equal(turbo_transport_recv(transport, &response, &response_size), -1);
    check_null(response);
    check_equal(response_size, 0u);
    check_true(turbo_transport_is_connected(transport));

    /* The reply may arrive while send() advances the same client. The next
     * recv() must consume that retained completion without a second demand.
     */
    check_equal(turbo_transport_send(transport, (const uint8_t *)request,
                                     sizeof(request) - 1u),
                (int)(sizeof(request) - 1u));
    check_greater(turbo_transport_recv(transport, &response, &response_size), 0);
    check_not_null(response);
    turbo_transport_free_recv(transport, response);
    check_equal(state.request_valid, 1);
    check_equal(turbo_transport_destroy(transport), 0);
    check_equal(cnet_client_stop(&owner, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&owner), SALTS_OK);
    check_equal(chttp_server_stop(&state.server, 5000u), SALTS_OK);
    check_equal(chttp_server_destroy(&state.server), SALTS_OK);
  }

  it("rejects send sizes beyond the public int result before reading memory") {
    transport_http_test_state_t state = {0};
    cnet_client owner = {0};
    cnet_client_config owner_config = transport_external_client_config();
    turbo_transport_config_t config = {
        .type = TURBO_TRANSPORT_TCP,
        .host = "127.0.0.1",
        .port = TRANSPORT_HTTP_TEST_PORT,
        .connect_timeout_ms = 5000,
        .read_timeout_ms = 5000,
        .write_timeout_ms = 5000,
        .cnet_client = &owner
    };
    turbo_transport_t *transport;
    uint8_t byte = 0x5a;

    check_equal(transport_http_server_start(&state), 0);
    check_equal(cnet_client_init(&owner, &owner_config), SALTS_OK);
    transport = turbo_transport_create(&config);
    check_not_null(transport);
    check_equal(turbo_transport_connect(transport), 0);
    check_equal(turbo_transport_send(transport, &byte, (size_t)INT_MAX + 1u), -1);
    check_true(turbo_transport_is_connected(transport));
    check_equal(turbo_transport_destroy(transport), 0);
    check_equal(cnet_client_stop(&owner, 5000u), SALTS_OK);
    check_equal(cnet_client_destroy(&owner), SALTS_OK);
    check_equal(chttp_server_stop(&state.server, 5000u), SALTS_OK);
    check_equal(chttp_server_destroy(&state.server), SALTS_OK);
  }

  it("recycles rejected TLS admission without a hidden reconnect") {
    cnet_tls_client_config invalid_tls = {0};
    turbo_transport_config_t config = {
        .type = TURBO_TRANSPORT_TLS,
        .host = "127.0.0.1",
        .port = TRANSPORT_HTTP_TEST_PORT,
        .connect_timeout_ms = 250,
        .read_timeout_ms = 250,
        .write_timeout_ms = 250,
        .tls = &invalid_tls
    };
    turbo_transport_t *transport = turbo_transport_create(&config);
    cnet_connection connection = {0};

    check_not_null(transport);
    /* Invalid TLS policy is fail-closed at CNet admission. A valid Manager
     * reservation is consumed even when CNet rejects synchronously.
     */
    check_equal(turbo_transport_connect(transport), -1);
    check_not_null(turbo_transport_get_error(transport));
    check_equal(turbo_transport_get_connection(transport, &connection), -1);
    /* A second explicit call can reserve again; nothing auto-retries. */
    check_equal(turbo_transport_connect(transport), -1);
    check_equal(turbo_transport_destroy(transport), 0);
  }

}
