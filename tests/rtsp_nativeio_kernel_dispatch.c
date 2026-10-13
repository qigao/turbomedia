/* M15d/#192: a REAL RTSP/CNet server supplies bytes to a separate,
 * PRIVATE-source Salts NativeIO endpoint. The private hook runs on the
 * original NativeIO Owner AFTER epoll_wait returned genuine readiness,
 * BEFORE NativeIO dispatches the original receive terminal.
 *
 * This is a test-only linkage to Salts #1168/#1176, never a public hook in
 * Salts.Native or TurboMedia::RTSP. CNet still uses the installed SDK.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <turbo_rtsp.h>
#include <salts/native_io.h>
#include <salts/clock.h>
#include <salts/error_codes.h>
#include "native_io_readiness.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    M15D_PORT = 21577, M15D_ROUNDS = 12, M15D_TIMEOUT_MS = 5000,
    M15D_CANCEL_AT_READY = 1, M15D_CANCEL_BEFORE_REPLY = 2
};

typedef struct {
    _Atomic unsigned calls;
    _Atomic unsigned closes;
    _Atomic unsigned entered;
    _Atomic unsigned release;
    _Atomic unsigned exited;
    _Atomic unsigned wrong_cseq;
    unsigned expected_cseq; /* immutable while its Server Worker is live */
    int hold;
} rtsp_fixture;

typedef struct {
    native_io_backend *owner;
    native_io_endpoint endpoint;
    native_io_request request;
    unsigned armed;
    unsigned entered;
    unsigned wrong_token;
    int cancel_status;
    int early_release;
} kernel_gate;

static kernel_gate gate;

static int within_deadline(_Atomic unsigned *value, unsigned target) {
    const uint64_t start = cmeta_monotonic_ms();
    while (atomic_load_explicit(value, memory_order_acquire) < target) {
        if (cmeta_monotonic_ms() - start >= M15D_TIMEOUT_MS) return 0;
        sched_yield();
    }
    return 1;
}

static int on_options(
    turbo_rtsp_session_t *session, const turbo_rtsp_request_t *request,
    turbo_rtsp_response_t *response, void *context) {
    rtsp_fixture *fixture = (rtsp_fixture *)context;
    (void)session;
    if (request->cseq != fixture->expected_cseq)
        atomic_fetch_add_explicit(&fixture->wrong_cseq, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&fixture->calls, 1u, memory_order_release);
    if (fixture->hold) {
        const uint64_t start = cmeta_monotonic_ms();
        atomic_store_explicit(&fixture->entered, 1u, memory_order_release);
        while (!atomic_load_explicit(&fixture->release, memory_order_acquire)) {
            if (cmeta_monotonic_ms() - start >= M15D_TIMEOUT_MS) {
                atomic_fetch_add_explicit(&fixture->wrong_cseq, 1u,
                                          memory_order_relaxed);
                break;
            }
            sched_yield();
        }
        atomic_store_explicit(&fixture->exited, 1u, memory_order_release);
    }
    return turbo_rtsp_response_options(response, NULL);
}

static void on_close(turbo_rtsp_session_t *session, void *context) {
    rtsp_fixture *fixture = (rtsp_fixture *)context;
    (void)session;
    atomic_fetch_add_explicit(&fixture->closes, 1u, memory_order_release);
}

/* The Salts original backend invokes this only for an ACTUAL kernel
 * readiness batch. This is the same-Owner stack that owns observe/cancel.
 * We do not cancel from a peer or from a CNet/RTSP callback. */
void salts_native_io_test_before_ready_dispatch(
    cmeta_io_impl *owner, const cmeta_io_ready_event *events, size_t count) {
    const uint64_t token = ((uint64_t)gate.endpoint.generation << 32u) |
                           (uint64_t)gate.endpoint.slot;
    size_t i;
    if (!gate.armed || !gate.owner || gate.owner->impl != owner) return;
    for (i = 0; i < count; ++i) {
        if (!(events[i].interests & SALTS_IO_READY_READ)) continue;
        if (events[i].token != token) {
            ++gate.wrong_token;
            continue;
        }
        if (++gate.entered != 1u) continue;
        gate.cancel_status = native_io_backend_cancel(gate.owner, gate.request);
        /* A cancelled request STILL borrows its original endpoint and
         * receive buffer until the one terminal has been observed. */
        gate.early_release =
            native_io_backend_release_socket(gate.owner, gate.endpoint);
    }
}

static int connect_real_rtsp(void) {
    struct sockaddr_in address = {0};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int flags;
    if (fd < 0) return -1;
    address.sin_family = AF_INET;
    address.sin_port = htons(M15D_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(fd);
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static int fail(unsigned line, unsigned round, const char *expression, int code) {
    fprintf(stderr, "M15d REAL RTSP kernel receive/cancel round=%u line=%u: %s (%d)\n",
            round, line, expression, code);
    return 1;
}

#define CHECK(expr) do { \
    if (!(expr)) { status = fail(__LINE__, round, #expr, 0); goto end_round; } \
} while (0)
#define CHECK_STATUS(expr, expected) do { \
    const int result_ = (expr); \
    if (result_ != (expected)) { \
        status = fail(__LINE__, round, #expr, result_); goto end_round; \
    } \
} while (0)

int main(void) {
    native_io_backend backend = {0};
    native_io_backend_config backend_config = {0};
    native_io_endpoint previous_endpoint = {0};
    native_io_request previous_request = {0};
    unsigned round;
    int result;

    backend_config.kind = NATIVE_IO_BACKEND_EPOLL;
    backend_config.endpoint_capacity = 1u;
    backend_config.request_capacity = 1u;
    backend_config.completion_batch_capacity = 1u;
    result = native_io_backend_init(&backend, &backend_config);
    if (result != SALTS_OK)
        return fail(__LINE__, 0u, "native_io_backend_init(EPOLL)", result);

    for (round = 0u; round < M15D_ROUNDS; ++round) {
        rtsp_fixture fixture = {0};
        turbo_rtsp_server_config_t config = {0};
        turbo_rtsp_server_handlers_t handlers = {0};
        turbo_rtsp_server_t *server = NULL;
        native_io_endpoint endpoint = {0};
        native_io_request request = {0};
        native_io_operation op = {0};
        native_io_completion terminal = {0};
        int fd = -1;
        int attached = 0, request_live = 0;
        unsigned mode = round % 3u;
        size_t count = 0u;
        int status = 0;
        char rtsp_request[256];
        size_t request_bytes;
        unsigned char receive_buffer[2048];
        const unsigned cseq = 5000u + round;

        fixture.expected_cseq = cseq;
        fixture.hold = (mode == M15D_CANCEL_BEFORE_REPLY);
        atomic_init(&fixture.calls, 0u);
        atomic_init(&fixture.closes, 0u);
        atomic_init(&fixture.entered, 0u);
        atomic_init(&fixture.release, 0u);
        atomic_init(&fixture.exited, 0u);
        atomic_init(&fixture.wrong_cseq, 0u);
        handlers.on_options = on_options;
        handlers.on_session_close = on_close;
        config.bind_host = "127.0.0.1";
        config.port = M15D_PORT;
        config.client_timeout_ms = M15D_TIMEOUT_MS;
        config.control_transport = TURBO_RTSP_CONTROL_TRANSPORT_TCP;
        config.connection_capacity = 1u;
        config.send_queue_capacity = 8u;
        config.send_queue_bytes = 512u * 1024u;
        server = turbo_rtsp_server_create(&config, &handlers, &fixture);
        CHECK(server != NULL);
        CHECK(turbo_rtsp_server_start(server) == 0);
        fd = connect_real_rtsp();
        CHECK(fd >= 0);
        CHECK_STATUS(native_io_backend_attach_socket(
                         &backend, (uintptr_t)fd, &endpoint), SALTS_OK);
        attached = 1;
        if (round != 0u) {
            CHECK(endpoint.slot == previous_endpoint.slot);
            CHECK(endpoint.generation != previous_endpoint.generation);
        }
        memset(receive_buffer, 0xe5, sizeof(receive_buffer));
        op.kind = NATIVE_IO_OPERATION_STREAM_RECV;
        op.endpoint = endpoint;
        op.buffer = receive_buffer; /* original borrowed receive storage */
        op.length = sizeof(receive_buffer);
        op.user_data = (uintptr_t)(0x4D150000u + round);
        CHECK_STATUS(native_io_backend_submit(&backend, &op, &request), SALTS_OK);
        request_live = 1;
        if (round != 0u) {
            CHECK(request.slot == previous_request.slot);
            CHECK(request.generation != previous_request.generation);
            CHECK_STATUS(native_io_backend_cancel(&backend, previous_request),
                         SALTS_ENOENT);
        }
        memset(&gate, 0, sizeof(gate));
        gate.owner = &backend;
        gate.endpoint = endpoint;
        gate.request = request;
        gate.armed = (mode == M15D_CANCEL_AT_READY);

        result = snprintf(rtsp_request, sizeof(rtsp_request),
                          "OPTIONS rtsp://127.0.0.1:%u/m15d/kernel RTSP/1.0\r\n"
                          "CSeq: %u\r\n\r\n", (unsigned)M15D_PORT, cseq);
        CHECK(result > 0 && (size_t)result < sizeof(rtsp_request));
        request_bytes = (size_t)result;
        CHECK(send(fd, rtsp_request, request_bytes, 0) == (ssize_t)request_bytes);

        if (mode == M15D_CANCEL_BEFORE_REPLY) {
            /* Server entered the REAL RTSP/CNet on_options callback, so
             * the response cannot exist when same NativeIO Owner cancels. */
            CHECK(within_deadline(&fixture.entered, 1u));
            CHECK_STATUS(native_io_backend_cancel(&backend, request), SALTS_OK);
            CHECK_STATUS(native_io_backend_release_socket(&backend, endpoint),
                         SALTS_EBUSY);
            atomic_store_explicit(&fixture.release, 1u, memory_order_release);
            CHECK(within_deadline(&fixture.exited, 1u));
        }

        CHECK_STATUS(native_io_backend_observe(
                         &backend, &terminal, 1u, M15D_TIMEOUT_MS, &count),
                     SALTS_OK);
        request_live = 0;
        CHECK(count == 1u);
        CHECK(terminal.request.slot == request.slot &&
              terminal.request.generation == request.generation);
        CHECK(terminal.endpoint.slot == endpoint.slot &&
              terminal.endpoint.generation == endpoint.generation);
        CHECK(terminal.user_data == op.user_data);

        if (mode == 0u) {
            CHECK(terminal.kind == NATIVE_IO_COMPLETION_OK);
            CHECK(terminal.status == SALTS_OK);
            CHECK(terminal.bytes > 0u && receive_buffer[0] == 'R');
            CHECK(gate.entered == 0u);
        } else {
            CHECK(terminal.kind == NATIVE_IO_COMPLETION_CANCELLED);
            CHECK(terminal.status == SALTS_ECANCELED);
            CHECK(receive_buffer[0] == 0xe5u);
            if (mode == M15D_CANCEL_AT_READY) {
                CHECK(gate.entered == 1u && gate.wrong_token == 0u);
                CHECK(gate.cancel_status == SALTS_OK);
                CHECK(gate.early_release == SALTS_EBUSY);
            } else {
                CHECK(gate.entered == 0u);
            }
        }
        CHECK(atomic_load_explicit(&fixture.wrong_cseq,
                                   memory_order_acquire) == 0u);
        CHECK(within_deadline(&fixture.calls, 1u));
        gate.armed = 0u;
        CHECK_STATUS(native_io_backend_cancel(&backend, request), SALTS_ENOENT);
        count = 9u;
        CHECK_STATUS(native_io_backend_observe(
                         &backend, &terminal, 1u, 0u, &count), SALTS_ETIMEDOUT);
        CHECK(count == 0u);
        previous_endpoint = endpoint;
        previous_request = request;

end_round:
        /* Never allow a server callback to outlive its borrowed fixture.
         * Even a failed assertion releases the callback before stop/join. */
        atomic_store_explicit(&fixture.release, 1u, memory_order_release);
        gate.armed = 0u;
        if (request_live) {
            (void)native_io_backend_cancel(&backend, request);
            count = 0u;
            (void)native_io_backend_observe(
                &backend, &terminal, 1u, M15D_TIMEOUT_MS, &count);
        }
        if (fd >= 0) (void)close(fd);
        if (attached) {
            result = native_io_backend_release_socket(&backend, endpoint);
            if (result != SALTS_OK && status == 0)
                status = fail(__LINE__, round, "release_socket after terminal", result);
        }
        if (server) {
            turbo_rtsp_server_stop(server); /* joins the original CNet Owner */
            turbo_rtsp_server_destroy(server);
        }
        if (status != 0) {
            (void)native_io_backend_close(&backend);
            (void)native_io_backend_destroy(&backend);
            return status;
        }
    }
    result = native_io_backend_close(&backend);
    if (result != SALTS_OK)
        return fail(__LINE__, M15D_ROUNDS, "native_io_backend_close", result);
    result = native_io_backend_destroy(&backend);
    if (result != SALTS_OK)
        return fail(__LINE__, M15D_ROUNDS, "native_io_backend_destroy", result);
    printf("M15d real RTSP/CNet server + original NativeIO epoll dispatch: "
           "%u real request-generation rounds PASS\n", (unsigned)M15D_ROUNDS);
    return 0;
}
