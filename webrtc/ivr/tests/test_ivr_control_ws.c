#include "ivr_control_ws.h"
#include "ivr_thread.h"
#include "tinytest.h"

#include <salts/clock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum {
    TEST_WAIT_ATTEMPTS = 200,
    TEST_WAIT_SLICE_MS = 5,
    TEST_TIMEOUT_MS = 2000,
    TEST_LONG_LIVED_SHUTDOWN_MS = 100,
    TEST_LONG_LIVED_HEARTBEAT_MS = 20,
    TEST_LONG_LIVED_HEARTBEATS = 15,
    TEST_MESSAGE_CAPACITY = 8,
    TEST_MESSAGE_BYTES = 4096
};

typedef struct {
    ivr_control_ws_server_t *server;
    ivr_control_ws_route_t route;
    atomic_int connected;
    atomic_int client_connected;
    atomic_int server_messages;
    atomic_int client_messages;
    atomic_int block_server_message;
    atomic_int server_message_entered;
    atomic_int release_server_message;
    char server_payload[64];
    char client_payload[64];
} control_ws_probe_t;

static int wait_value(const atomic_int *value, int expected) {
    for (int i = 0; i < TEST_WAIT_ATTEMPTS; ++i) {
        if (atomic_load_explicit(value, memory_order_acquire) == expected) {
            return 1;
        }
        ivr_thread_sleep_ms(TEST_WAIT_SLICE_MS);
    }
    return 0;
}

static int verify_worker(void *context, const char *certificate_sha256,
                         const char *claimed_identity) {
    (void)context;
    return certificate_sha256 == NULL && claimed_identity != NULL &&
           strcmp(claimed_identity, "worker-a") == 0;
}

static void on_peer(void *context, const ivr_control_ws_route_t *route,
                    const char *identity, int connected) {
    control_ws_probe_t *probe = (control_ws_probe_t *)context;
    if (connected && route && identity && strcmp(identity, "worker-a") == 0) {
        probe->route = *route;
        atomic_store_explicit(&probe->connected, 1, memory_order_release);
    } else if (!connected) {
        atomic_store_explicit(&probe->connected, 0, memory_order_release);
    }
}

static int on_server_message(void *context,
                             const ivr_control_ws_route_t *route,
                             const char *identity, const uint8_t *data,
                             size_t size) {
    control_ws_probe_t *probe = (control_ws_probe_t *)context;
    if (!route || !identity || strcmp(identity, "worker-a") != 0 ||
        size >= sizeof(probe->server_payload)) {
        return IVR_EINVAL;
    }
    atomic_store_explicit(&probe->server_message_entered, 1,
                          memory_order_release);
    while (atomic_load_explicit(&probe->block_server_message,
                                memory_order_acquire) &&
           !atomic_load_explicit(&probe->release_server_message,
                                 memory_order_acquire)) {
        ivr_thread_sleep_ms(TEST_WAIT_SLICE_MS);
    }
    memcpy(probe->server_payload, data, size);
    probe->server_payload[size] = '\0';
    atomic_fetch_add_explicit(&probe->server_messages, 1,
                              memory_order_release);
    return ivr_control_ws_server_send_copy(probe->server, route, data, size);
}

static void on_client_message(void *context, const uint8_t *data,
                              size_t size) {
    control_ws_probe_t *probe = (control_ws_probe_t *)context;
    if (size >= sizeof(probe->client_payload)) {
        return;
    }
    memcpy(probe->client_payload, data, size);
    probe->client_payload[size] = '\0';
    atomic_fetch_add_explicit(&probe->client_messages, 1,
                              memory_order_release);
}

static void on_client_connection(void *context, int connected) {
    control_ws_probe_t *probe = (control_ws_probe_t *)context;
    atomic_store_explicit(&probe->client_connected, connected,
                          memory_order_release);
}

static void init_probe(control_ws_probe_t *probe) {
    memset(probe, 0, sizeof(*probe));
    atomic_init(&probe->connected, 0);
    atomic_init(&probe->client_connected, 0);
    atomic_init(&probe->server_messages, 0);
    atomic_init(&probe->client_messages, 0);
    atomic_init(&probe->block_server_message, 0);
    atomic_init(&probe->server_message_entered, 0);
    atomic_init(&probe->release_server_message, 0);
}

spec("IVR CHTTP H1 WebSocket control transport") {
    it("binds identity and exchanges one binary control message") {
        control_ws_probe_t probe;
        ivr_control_ws_server_config_t server_config;
        ivr_control_ws_client_config_t client_config;
        ivr_control_ws_server_t *server = NULL;
        ivr_control_ws_client_t *client = NULL;
        char uri[128];
        uint16_t port = 0;

        init_probe(&probe);
        ivr_control_ws_server_config_init(&server_config);
        server_config.host = "127.0.0.1";
        server_config.port = 0;
        server_config.path = "/internal/ivr/control";
        server_config.maximum_connections = 2;
        server_config.maximum_message_bytes = TEST_MESSAGE_BYTES;
        server_config.shutdown_timeout_ms = TEST_LONG_LIVED_SHUTDOWN_MS;
        server_config.verify_identity = verify_worker;
        server_config.on_peer = on_peer;
        server_config.on_message = on_server_message;
        server_config.callback_context = &probe;
        check_equal(ivr_control_ws_server_create(&server_config, &server),
                    IVR_OK);
        probe.server = server;
        check_equal(ivr_control_ws_server_start(server), IVR_OK);
        check_equal(ivr_control_ws_server_port(server, &port), IVR_OK);
        check_true(port != 0);
        check_true(snprintf(uri, sizeof(uri),
                            "ws://127.0.0.1:%u/internal/ivr/control",
                            (unsigned int)port) > 0);

        ivr_control_ws_client_config_init(&client_config);
        client_config.uri = uri;
        client_config.identity = "worker-a";
        client_config.maximum_queue_messages = TEST_MESSAGE_CAPACITY;
        client_config.maximum_queue_bytes = TEST_MESSAGE_BYTES;
        client_config.maximum_message_bytes = TEST_MESSAGE_BYTES;
        client_config.start_timeout_ms = TEST_TIMEOUT_MS;
        client_config.io_timeout_ms = 20;
        client_config.on_message = on_client_message;
        client_config.callback_context = &probe;
        check_equal(ivr_control_ws_client_create(&client_config, &client),
                    IVR_OK);
        check_equal(ivr_control_ws_client_start(client), IVR_OK);
        check_true(wait_value(&probe.connected, 1));

        check_equal(ivr_control_ws_client_send_copy(
                        client, (const uint8_t *)"hello", 5),
                    IVR_OK);
        check_true(wait_value(&probe.server_messages, 1));
        check_true(wait_value(&probe.client_messages, 1));
        check_equal(probe.server_payload, "hello");
        check_equal(probe.client_payload, "hello");

        for (int heartbeat = 0; heartbeat < TEST_LONG_LIVED_HEARTBEATS;
             ++heartbeat) {
            check_equal(ivr_control_ws_client_send_copy(
                            client, (const uint8_t *)"heartbeat", 9),
                        IVR_OK);
            ivr_thread_sleep_ms(TEST_LONG_LIVED_HEARTBEAT_MS);
        }
        check_equal(atomic_load_explicit(&probe.connected,
                                         memory_order_acquire),
                    1);

        check_equal(ivr_control_ws_client_stop(client), IVR_OK);
        check_equal(ivr_control_ws_client_destroy(client), IVR_OK);
        check_true(wait_value(&probe.connected, 0));
        check_not_equal(ivr_control_ws_server_send_copy(
                            server, &probe.route,
                            (const uint8_t *)"stale", 5),
                        IVR_OK);
        check_equal(ivr_control_ws_server_stop(server), IVR_OK);
        check_equal(ivr_control_ws_server_destroy(server), IVR_OK);
    }

    it("retains server ownership when shutdown times out") {
        control_ws_probe_t probe;
        ivr_control_ws_server_config_t server_config;
        ivr_control_ws_client_config_t client_config;
        ivr_control_ws_server_t *server = NULL;
        ivr_control_ws_client_t *client = NULL;
        char uri[128];
        uint16_t port = 0u;

        init_probe(&probe);
        ivr_control_ws_server_config_init(&server_config);
        server_config.host = "127.0.0.1";
        server_config.port = 0u;
        server_config.path = "/internal/ivr/control";
        server_config.maximum_connections = 2u;
        server_config.maximum_message_bytes = TEST_MESSAGE_BYTES;
        server_config.shutdown_timeout_ms = 10u;
        server_config.verify_identity = verify_worker;
        server_config.on_peer = on_peer;
        server_config.on_message = on_server_message;
        server_config.callback_context = &probe;
        check_equal(ivr_control_ws_server_create(&server_config, &server),
                    IVR_OK);
        probe.server = server;
        check_equal(ivr_control_ws_server_start(server), IVR_OK);
        check_equal(ivr_control_ws_server_port(server, &port), IVR_OK);
        check_true(snprintf(uri, sizeof(uri),
                            "ws://127.0.0.1:%u/internal/ivr/control",
                            (unsigned int)port) > 0);

        ivr_control_ws_client_config_init(&client_config);
        client_config.uri = uri;
        client_config.identity = "worker-a";
        client_config.maximum_queue_messages = TEST_MESSAGE_CAPACITY;
        client_config.maximum_queue_bytes = TEST_MESSAGE_BYTES;
        client_config.maximum_message_bytes = TEST_MESSAGE_BYTES;
        client_config.start_timeout_ms = TEST_TIMEOUT_MS;
        client_config.io_timeout_ms = 20u;
        check_equal(ivr_control_ws_client_create(&client_config, &client),
                    IVR_OK);
        check_equal(ivr_control_ws_client_start(client), IVR_OK);
        check_true(wait_value(&probe.connected, 1));

        atomic_store_explicit(&probe.block_server_message, 1,
                              memory_order_release);
        check_equal(ivr_control_ws_client_send_copy(
                        client, (const uint8_t *)"blocked", 7u),
                    IVR_OK);
        check_true(wait_value(&probe.server_message_entered, 1));
        check_equal(ivr_control_ws_server_stop(server), IVR_ESTATE);
        check_equal(ivr_control_ws_server_destroy(server), IVR_ESTATE);

        atomic_store_explicit(&probe.release_server_message, 1,
                              memory_order_release);
        ivr_thread_sleep_ms(50u);
        check_equal(ivr_control_ws_client_stop(client), IVR_OK);
        check_equal(ivr_control_ws_client_destroy(client), IVR_OK);
        check_equal(ivr_control_ws_server_destroy(server), IVR_OK);
    }

    it("interrupts reconnect backoff during client stop") {
        control_ws_probe_t probe;
        ivr_control_ws_server_config_t server_config;
        ivr_control_ws_client_config_t client_config;
        ivr_control_ws_server_t *server = NULL;
        ivr_control_ws_client_t *client = NULL;
        char uri[128];
        uint16_t port = 0u;
        uint64_t stop_started_ms;
        uint64_t stop_elapsed_ms;

        init_probe(&probe);
        ivr_control_ws_server_config_init(&server_config);
        server_config.host = "127.0.0.1";
        server_config.port = 0u;
        server_config.path = "/internal/ivr/control";
        server_config.maximum_connections = 2u;
        server_config.maximum_message_bytes = TEST_MESSAGE_BYTES;
        server_config.verify_identity = verify_worker;
        server_config.on_peer = on_peer;
        server_config.on_message = on_server_message;
        server_config.callback_context = &probe;
        check_equal(ivr_control_ws_server_create(&server_config, &server),
                    IVR_OK);
        probe.server = server;
        check_equal(ivr_control_ws_server_start(server), IVR_OK);
        check_equal(ivr_control_ws_server_port(server, &port), IVR_OK);
        check_true(snprintf(uri, sizeof(uri),
                            "ws://127.0.0.1:%u/internal/ivr/control",
                            (unsigned int)port) > 0);

        ivr_control_ws_client_config_init(&client_config);
        client_config.uri = uri;
        client_config.identity = "worker-a";
        client_config.maximum_queue_messages = TEST_MESSAGE_CAPACITY;
        client_config.maximum_queue_bytes = TEST_MESSAGE_BYTES;
        client_config.maximum_message_bytes = TEST_MESSAGE_BYTES;
        client_config.start_timeout_ms = TEST_TIMEOUT_MS;
        client_config.io_timeout_ms = 20u;
        client_config.reconnect_initial_ms = 5000u;
        client_config.reconnect_max_ms = 5000u;
        client_config.on_connection = on_client_connection;
        client_config.callback_context = &probe;
        check_equal(ivr_control_ws_client_create(&client_config, &client),
                    IVR_OK);
        check_equal(ivr_control_ws_client_start(client), IVR_OK);
        check_true(wait_value(&probe.client_connected, 1));

        check_equal(ivr_control_ws_server_stop(server), IVR_OK);
        check_true(wait_value(&probe.client_connected, 0));
        stop_started_ms = salts_monotonic_ms();
        check_equal(ivr_control_ws_client_stop(client), IVR_OK);
        stop_elapsed_ms = salts_monotonic_ms() - stop_started_ms;
        check_true(stop_elapsed_ms < 500u);

        check_equal(ivr_control_ws_client_destroy(client), IVR_OK);
        check_equal(ivr_control_ws_server_destroy(server), IVR_OK);
    }
}
