/* Independent installed Salts 2.3 CNet Manager ABI consumer.
 * This tests the real runtime and intentionally has no CHttp/SaltsNet fixture.
 */
#include <cnet/manager.h>
#include <salts/error_codes.h>
#include <salts/clock.h>
#include <stdint.h>
#include <stdio.h>

#if CNET_MANAGER_VERSION != 1u
#error "TurboMedia requires the reviewed CNet 2.3 Manager ABI"
#endif

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s failed\\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

typedef struct smoke_probe {
    unsigned connected;
    unsigned terminal;
    unsigned recycled;
} smoke_probe;

static void on_state(void *user, cnet_connection connection,
                     cnet_connection_state state, const cnet_error *error) {
    smoke_probe *probe = (smoke_probe *)user;
    (void)connection;
    (void)error;
    if (state == CNET_CONNECTION_CONNECTED) ++probe->connected;
    if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED)
        ++probe->terminal;
}

static void on_recycle(void *user) {
    ++((smoke_probe *)user)->recycled;
}

int main(void) {
    cnet_client client = {0};
    cnet_manager manager = {0};
    cnet_client_config client_config = {0};
    cnet_manager_config manager_config = {0};
    cnet_manager_attachment attachment = {0};
    cnet_managed_connection first = {0}, second = {0};
    cnet_manager_entry entry = {0};
    cnet_manager_snapshot snapshot = {0};
    smoke_probe probe = {0};
    cnet_listener listener = {0};
    cnet_listener_config listener_config = {0};
    cnet_accepted_stream accepted = CNET_ACCEPTED_STREAM_INIT;
    cnet_connect_options options = {0};
    cnet_connection connection = {0};
    uint16_t port = 0u;
    char uri[96];
    size_t events = 0u;
    uint64_t started_ms;
    int status;
    size_t work = 0u;

#if defined(_WIN32)
    client_config.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    client_config.backend = NATIVE_IO_BACKEND_EPOLL;
#else
    client_config.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
    client_config.connection_capacity = 2u;
    client_config.command_capacity = 8u;
    client_config.request_capacity = 8u;
    client_config.completion_batch_capacity = 8u;
    client_config.event_capacity = 8u;
    client_config.max_send_bytes = 4096u;
    client_config.receive_buffer_bytes = 4096u;
    client_config.connect_timeout_ms = 1000u;
    client_config.read_timeout_ms = 1000u;
    client_config.write_timeout_ms = 1000u;

    REQUIRE(cnet_client_init(&client, &client_config) == SALTS_OK);
    manager_config.size = sizeof(manager_config);
    manager_config.version = CNET_MANAGER_VERSION;
    manager_config.client = &client;
    manager_config.record_capacity = 1u;
    manager_config.connection_capacity = 1u;
    REQUIRE(cnet_manager_init(&manager, &manager_config) == SALTS_OK);
    attachment.observer.on_state = on_state;
    attachment.observer.user = &probe;
    attachment.on_recycle = on_recycle;

    REQUIRE(cnet_manager_reserve(&manager, &attachment, &first) == SALTS_OK);
    REQUIRE(first.slot != 0u);
    REQUIRE(cnet_manager_reserve(&manager, &attachment, &second) == SALTS_ENOBUFS);
    REQUIRE(second.slot == 0u);
    REQUIRE(cnet_manager_cancel(&manager, first) == SALTS_OK);
    REQUIRE(cnet_manager_cancel(&manager, first) == SALTS_EALREADY);
    REQUIRE(cnet_manager_advance(&manager, 1u, &work) == SALTS_OK);
    REQUIRE(work == 1u && probe.recycled == 1u);
    REQUIRE(cnet_manager_get_snapshot(&manager, &snapshot) == SALTS_OK);
    REQUIRE(snapshot.drained);

    REQUIRE(cnet_manager_reserve(&manager, &attachment, &second) == SALTS_OK);
    REQUIRE(cnet_manager_lookup(&manager, first, &entry) == SALTS_ENOENT);
    REQUIRE(second.generation != first.generation);
    REQUIRE(cnet_manager_cancel(&manager, second) == SALTS_OK);
    REQUIRE(cnet_manager_advance(&manager, 1u, &work) == SALTS_OK);
    REQUIRE(probe.recycled == 2u);

    /* Real TCP test of installed CNet's adopted owner-local lifetime. The
     * Manager is reused after cancellation; no new backend or worker exists.
     */
    listener_config.backend = client_config.backend;
    listener_config.host = "127.0.0.1";
    listener_config.port = 0u;
    listener_config.backlog = 4u;
    REQUIRE(cnet_listener_init(&listener, &listener_config) == SALTS_OK);
    REQUIRE(cnet_listener_port(&listener, &port) == SALTS_OK);
    status = snprintf(uri, sizeof(uri), "tcp://127.0.0.1:%u", (unsigned)port);
    REQUIRE(status > 0 && (size_t)status < sizeof(uri));
    options.uri = uri;
    REQUIRE(cnet_manager_reserve(&manager, &attachment, &second) == SALTS_OK);
    REQUIRE(cnet_manager_connect(&manager, second, &options, &connection) == SALTS_OK);

    started_ms = cmeta_monotonic_ms();
    while (probe.connected == 0u && probe.terminal == 0u &&
           cmeta_monotonic_ms() - started_ms < 3000u) {
        REQUIRE(cnet_client_poll(&client, 10u, &events) == SALTS_OK);
    }
    REQUIRE(probe.connected == 1u && probe.terminal == 0u);
    started_ms = cmeta_monotonic_ms();
    for (;;) {
        status = cnet_listener_accept_detached(&listener, &accepted);
        if (status == SALTS_OK) break;
        REQUIRE(status == SALTS_ETIMEDOUT);
        REQUIRE(cmeta_monotonic_ms() - started_ms < 3000u);
        REQUIRE(cnet_client_poll(&client, 10u, &events) == SALTS_OK);
    }
    REQUIRE(cnet_accepted_stream_close(&accepted) == SALTS_OK);
    REQUIRE(cnet_close(&client, connection) == SALTS_OK);
    started_ms = cmeta_monotonic_ms();
    while (probe.terminal == 0u && cmeta_monotonic_ms() - started_ms < 3000u) {
        REQUIRE(cnet_client_poll(&client, 10u, &events) == SALTS_OK);
    }
    REQUIRE(probe.terminal == 1u);
    REQUIRE(cnet_manager_advance(&manager, 1u, &work) == SALTS_OK);
    REQUIRE(probe.recycled == 3u);
    REQUIRE(cnet_manager_get_snapshot(&manager, &snapshot) == SALTS_OK);
    REQUIRE(snapshot.drained);
    REQUIRE(cnet_listener_close(&listener) == SALTS_OK);
    REQUIRE(cnet_listener_destroy(&listener) == SALTS_OK);
    REQUIRE(cnet_manager_destroy(&manager) == SALTS_OK);
    REQUIRE(cnet_client_stop(&client, 1000u) == SALTS_OK);
    REQUIRE(cnet_client_destroy(&client) == SALTS_OK);
    return 0;
}
