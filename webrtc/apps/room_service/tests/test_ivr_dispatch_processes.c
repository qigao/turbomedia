/* test_ivr_dispatch_processes.c - Real RoomService/CHTTP H1 WebSocket process smoke.
 *
 * Spawns the actual room_service executable and a deterministic media-provider
 * process that runs the production ivr_worker/media-bot/DTMF core. It verifies
 * typed playback, input, cancel, close, and Iris result/event delivery across
 * HTTP, CHTTP H1 WebSocket, and process boundaries without a live speech/SFU dependency. */
#include "ivr_control_gateway.h"
#include "ivr_frame.h"
#include "ivr_thread.h"
#include "ivr_whip_transport.h"
#include "iris_control_process_peer.h"
#include "turbomedia_ivr_v1.h"
#include "tinytest.h"
#include <platform.h>
#include <chttp/chttp.h>
#include <salts/error_codes.h>
#include <salts_thread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IVR_TEST_CONTENT_ROOT
#define IVR_TEST_CONTENT_ROOT "content"
#endif
#ifndef IVR_WORKER_BIN
#define IVR_WORKER_BIN "ivr_worker.exe"
#endif
#ifndef ROOM_SERVICE_BIN
#define ROOM_SERVICE_BIN "room_service.exe"
#endif
#ifndef IVR_DISPATCH_PROBE_BIN
#define IVR_DISPATCH_PROBE_BIN "ivr_dispatch_worker_probe.exe"
#endif
#ifndef SFU_NODE_BIN
#define SFU_NODE_BIN "sfu_node.exe"
#endif
#ifndef TEST_BIN_DIR
#define TEST_BIN_DIR "."
#endif
#ifndef ROOM_SERVICE_TEST_TLS_CERT_PATH
#error "ROOM_SERVICE_TEST_TLS_CERT_PATH must identify the test certificate"
#endif
#ifndef ROOM_SERVICE_TEST_TLS_KEY_PATH
#error "ROOM_SERVICE_TEST_TLS_KEY_PATH must identify the test private key"
#endif

#define TEST_ROUTER_PORT 17823
#define TEST_ROUTER_BACKEND_PORT 17825
#define TEST_HTTP_PORT 19091
#define TEST_IRIS_TLS_PORT 19997
#define TEST_IRIS_BACKEND_TLS_PORT 19998
#define TEST_IRIS_CONTROL_PORT 19996
#define TEST_SFU_PORT 17933
#define TEST_REAL_WORKER_HEALTH_PORT 18092
#define TEST_CTRL_TOKEN "smoke-control-token"
#define TEST_PROVIDER_TOKEN "provider-process-token"
#define TEST_SPEECH_TOKEN "process-speech-token"
#define TEST_SFU_CTRL_TOKEN "sfu-process-control-token"
#define TEST_SFU_MEDIA_TOKEN "sfu-process-media-token"
#define TEST_SFU_CALLER_PARTICIPANT_ID "caller-source-42"
#define TEST_SFU_CALLER_TRACK_ID "caller-source-42-audio"
#define TEST_FIXTURE_CERTIFICATE_SHA256                                      \
    "sha256:ebd76f304bc43bc2be697fca2f054206978c0558931529a7c1b2bb7d82a7a3c4"

enum {
    TEST_CHILD_EXIT_TIMEOUT_MS = 5000,
    TEST_HTTP_IO_TIMEOUT_MS = 5000,
    TEST_IRIS_BODY_CAPACITY = 8192,
    TEST_HTTP_RESPONSE_CAPACITY = 16384,
    TEST_IRIS_DELIVERY_ATTEMPTS = 200,
    TEST_IRIS_DELIVERY_POLL_MS = 50,
    TEST_PROBE_LOG_ATTEMPTS = 20,
    TEST_PROBE_LOG_POLL_MS = 50,
    TEST_SFU_READY_ATTEMPTS = 40,
    TEST_SFU_READY_POLL_MS = 100,
    TEST_REAL_WORKER_READY_ATTEMPTS = 40,
    TEST_REAL_WORKER_READY_POLL_MS = 50,
    TEST_REAL_WORKER_READY_PROPAGATION_MS = 1000,
    TEST_RTC_TRANSITION_ATTEMPTS = 1000,
    TEST_RTC_TRANSITION_POLL_MS = 50,
    TEST_IRIS_PARTITION_RECOVERY_ATTEMPTS = 400,
    TEST_CONTROL_WS_PARTITION_RECOVERY_ATTEMPTS = 800,
    TEST_SFU_PROCESS_LOSS_ATTEMPTS = 1500,
    TEST_CALLER_CONNECT_TIMEOUT_MS = 15000,
    TEST_CALLER_SAMPLE_RATE = 16000,
    TEST_CALLER_SAMPLES_PER_FRAME = 320,
    TEST_CALLER_AUDIO_FRAME_COUNT = 30,
    TEST_CALLER_AUDIO_FRAME_MS = 20,
    TEST_TTS_RTP_ATTEMPTS = 200,
    TEST_SPEECH_PCM_SAMPLES = 4800,
    TEST_SPEECH_WAVE_HALF_PERIOD = 40,
    TEST_SPEECH_WAVE_AMPLITUDE = 12000,
    TEST_CHTTP_CONNECTION_CAPACITY = 2,
    TEST_CHTTP_QUEUE_CAPACITY = 8,
    TEST_CHTTP_MAX_HEADER_COUNT = 32,
    TEST_CHTTP_MAX_HEADER_BYTES = 16 * 1024,
    TEST_CHTTP_MAX_BODY_BYTES = 64 * 1024,
    TEST_CHTTP_MAX_BUFFERED_RESPONSE_BYTES = 32 * 1024,
    TEST_CHTTP_MAX_START_LINE_BYTES = 4 * 1024
};

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define PROCESS_HANDLE HANDLE
static void proc_sleep(unsigned int ms) { Sleep(ms); }
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#define PROCESS_HANDLE pid_t
static void proc_sleep(unsigned int ms) { usleep(ms * 1000); }
#endif

static native_io_backend_kind test_http_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static chttp_client_config test_http_client_config(void) {
    chttp_client_config config = {0};

    config.network = (cnet_client_config){
        .backend = test_http_backend(),
        .connection_capacity = TEST_CHTTP_CONNECTION_CAPACITY,
        .command_capacity = TEST_CHTTP_QUEUE_CAPACITY,
        .request_capacity = TEST_CHTTP_QUEUE_CAPACITY,
        .completion_batch_capacity = TEST_CHTTP_QUEUE_CAPACITY,
        .event_capacity = TEST_CHTTP_QUEUE_CAPACITY,
        .max_send_bytes = TEST_CHTTP_MAX_BODY_BYTES,
        .receive_buffer_bytes = TEST_CHTTP_MAX_HEADER_BYTES,
        .connect_timeout_ms = TEST_HTTP_IO_TIMEOUT_MS,
        .read_timeout_ms = TEST_HTTP_IO_TIMEOUT_MS,
        .write_timeout_ms = TEST_HTTP_IO_TIMEOUT_MS,
        .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
        .tls_handshake_timeout_ms = TEST_HTTP_IO_TIMEOUT_MS,
        .command_buffer_bytes = TEST_CHTTP_MAX_BODY_BYTES,
        .event_buffer_bytes = TEST_CHTTP_MAX_BODY_BYTES};
    config.request_capacity = TEST_CHTTP_QUEUE_CAPACITY;
    config.max_start_line_bytes = TEST_CHTTP_MAX_START_LINE_BYTES;
    config.max_header_count = TEST_CHTTP_MAX_HEADER_COUNT;
    config.max_header_bytes = TEST_CHTTP_MAX_HEADER_BYTES;
    config.max_request_body_bytes = TEST_CHTTP_MAX_BODY_BYTES;
    config.max_response_body_bytes = TEST_CHTTP_MAX_BODY_BYTES;
    config.max_informational_responses = 4u;
    return config;
}

typedef struct Req {
    const chttp_server_request_view *request;
    const void *body;
    size_t body_len;
} Req;

typedef struct Res {
    chttp_server_response *response;
    int status;
} Res;

typedef struct {
    chttp_server http;
    int http_initialized;
    atomic_int completion_calls;
    atomic_int room_completion_calls;
    atomic_int event_calls;
    atomic_int completion_valid;
    atomic_int event_valid;
    atomic_int playback_event_valid;
    atomic_int asr_event_valid;
    atomic_int dtmf_event_valid;
    atomic_int rtc_event_valid;
    atomic_int rtc_disconnected_calls;
    atomic_int rtc_reconnected_calls;
    atomic_int rtc_retry_exhausted_calls;
    atomic_ullong rtc_disconnected_generation;
    atomic_ullong rtc_reconnected_generation;
    atomic_int expected_resources_enabled;
    atomic_int expected_lease_calls;
    atomic_int expected_lease_valid;
    atomic_int expected_page_calls;
    atomic_int expected_page_valid;
    atomic_int expected_dialog_active;
    atomic_int expected_play_completed;
    atomic_int expected_resources_empty;
    atomic_int speech_tts_calls;
    atomic_int speech_tts_valid;
    int16_t speech_pcm[TEST_SPEECH_PCM_SAMPLES];
    char completion_body[TEST_IRIS_BODY_CAPACITY];
    char event_body[TEST_IRIS_BODY_CAPACITY];
} test_iris_server_t;

enum { TEST_IRIS_PROXY_CONNECTION_CAPACITY = 8 };

typedef struct {
#ifdef _WIN32
    SOCKET listener;
    SOCKET clients[TEST_IRIS_PROXY_CONNECTION_CAPACITY];
    SOCKET backends[TEST_IRIS_PROXY_CONNECTION_CAPACITY];
#endif
    salts_thread_t thread;
    atomic_int stopping;
    atomic_int available;
    int listen_port;
    int backend_port;
    int thread_started;
} test_iris_proxy_t;

static test_iris_server_t *g_iris_server;

static const char *get_headers(const Req *req, const char *name) {
    return req && req->request
               ? chttp_server_request_header(req->request, name)
               : NULL;
}

static const char *get_params(const Req *req, const char *name) {
    return req && req->request
               ? chttp_server_request_param(req->request, name)
               : NULL;
}

static void reply(Res *res, unsigned int status, const char *content_type,
                  const void *body, size_t body_size) {
    if (!res || res->status != SALTS_OK) {
        return;
    }
    res->status = chttp_server_reply(res->response, status, content_type,
                                     body, body_size);
}

static int copy_request_body(const Req *req, char *buffer, size_t capacity) {
    if (!req || !req->body || req->body_len >= capacity) return 0;
    memcpy(buffer, req->body, req->body_len);
    buffer[req->body_len] = '\0';
    return 1;
}

static int request_has_provider_token(const Req *req) {
    const char *authorization = get_headers(req, "Authorization");
    if (!authorization) authorization = get_headers(req, "authorization");
    return authorization &&
           strcmp(authorization, "Bearer " TEST_PROVIDER_TOKEN) == 0;
}

static int request_has_speech_token(const Req *req) {
    const char *authorization = get_headers(req, "Authorization");
    if (!authorization) authorization = get_headers(req, "authorization");
    return authorization &&
           strcmp(authorization, "Bearer " TEST_SPEECH_TOKEN) == 0;
}

static int extract_unsigned_json_field(const char *json, const char *field,
                                       unsigned long long *value) {
    const char *start;
    char *end;
    unsigned long long parsed;

    if (!json || !field || !value) return 0;
    start = strstr(json, field);
    if (!start) return 0;
    start += strlen(field);
    parsed = strtoull(start, &end, 10);
    if (end == start) return 0;
    *value = parsed;
    return 1;
}

static void test_iris_expected_lease_handler(Req *req, Res *res) {
    static const char lease[] =
        "{\"schemaVersion\":1,\"leaseId\":\"expected-http-lease\","
        "\"leaseRevision\":1,\"resourceCount\":1}";
    static const char empty_lease[] =
        "{\"schemaVersion\":1,\"leaseId\":\"expected-http-lease\","
        "\"leaseRevision\":1,\"resourceCount\":0}";
    test_iris_server_t *server = g_iris_server;
    const char *provider_id = get_params(req, "providerId");
    char body[256];
    int valid = server && request_has_provider_token(req) && provider_id &&
                strcmp(provider_id, "turbomedia") == 0 &&
                copy_request_body(req, body, sizeof(body)) &&
                strstr(body, "\"schemaVersion\":1") != NULL &&
                strstr(body, "\"pageSize\":") != NULL;
    if (server) {
        atomic_store_explicit(&server->expected_lease_valid, valid,
                              memory_order_release);
        atomic_fetch_add_explicit(&server->expected_lease_calls, 1,
                                  memory_order_release);
    }
    if (!valid) {
        reply(res, 400, "application/json", "{}", 2u);
    } else if (!atomic_load_explicit(&server->expected_resources_enabled,
                                     memory_order_acquire)) {
        reply(res, 503, "application/json", "{}", 2u);
    } else {
        int empty = atomic_load_explicit(&server->expected_resources_empty,
                                         memory_order_acquire);
        reply(res, 201, "application/json", empty ? empty_lease : lease,
              empty ? sizeof(empty_lease) - 1u : sizeof(lease) - 1u);
    }
}

static void test_iris_expected_page_handler(Req *req, Res *res) {
    static const char opening_page[] =
        "{\"schemaVersion\":1,\"leaseRevision\":1,\"cursor\":0,"
        "\"count\":1,\"nextCursor\":0,\"hasMore\":false,"
        "\"resources\":[{"
        "\"tenantId\":\"tenant-process\",\"providerId\":\"turbomedia\","
        "\"sessionId\":\"session-process\",\"sessionRevision\":1,"
        "\"ownerNodeId\":\"iris-process\",\"ownerEpoch\":1,"
        "\"ownerLeaseExpiresAtUnixMs\":4102444800000,"
        "\"dialogId\":\"dialog-process\",\"roomId\":\"room-42\","
        "\"callId\":\"call-42\",\"callGeneration\":1,"
        "\"operationGeneration\":1,\"state\":\"opening\","
        "\"activeCommandId\":\"command-process-start\","
        "\"activeCommandStatus\":\"pending\",\"dispatchWorkerId\":\"\","
        "\"dispatchEpoch\":0,\"dispatchLeaseExpiresAtUnixMs\":0}]}";
    static const char active_page[] =
        "{\"schemaVersion\":1,\"leaseRevision\":1,\"cursor\":0,"
        "\"count\":1,\"nextCursor\":0,\"hasMore\":false,"
        "\"resources\":[{"
        "\"tenantId\":\"tenant-process\",\"providerId\":\"turbomedia\","
        "\"sessionId\":\"session-process\",\"sessionRevision\":2,"
        "\"ownerNodeId\":\"iris-process\",\"ownerEpoch\":1,"
        "\"ownerLeaseExpiresAtUnixMs\":4102444800000,"
        "\"dialogId\":\"dialog-process\",\"roomId\":\"room-42\","
        "\"callId\":\"call-42\",\"callGeneration\":1,"
        "\"operationGeneration\":2,\"state\":\"active\","
        "\"activeCommandId\":\"command-process-play\","
        "\"activeCommandStatus\":\"pending\",\"dispatchWorkerId\":\"\","
        "\"dispatchEpoch\":0,\"dispatchLeaseExpiresAtUnixMs\":0}]}";
    static const char active_after_play_page[] =
        "{\"schemaVersion\":1,\"leaseRevision\":1,\"cursor\":0,"
        "\"count\":1,\"nextCursor\":0,\"hasMore\":false,"
        "\"resources\":[{"
        "\"tenantId\":\"tenant-process\",\"providerId\":\"turbomedia\","
        "\"sessionId\":\"session-process\",\"sessionRevision\":3,"
        "\"ownerNodeId\":\"iris-process\",\"ownerEpoch\":1,"
        "\"ownerLeaseExpiresAtUnixMs\":4102444800000,"
        "\"dialogId\":\"dialog-process\",\"roomId\":\"room-42\","
        "\"callId\":\"call-42\",\"callGeneration\":1,"
        "\"operationGeneration\":3,\"state\":\"active\","
        "\"activeCommandId\":\"command-process-collect\","
        "\"activeCommandStatus\":\"pending\",\"dispatchWorkerId\":\"\","
        "\"dispatchEpoch\":0,\"dispatchLeaseExpiresAtUnixMs\":0}]}";
    test_iris_server_t *server = g_iris_server;
    const char *provider_id = get_params(req, "providerId");
    const char *lease_id = get_params(req, "leaseId");
    const char *cursor = get_params(req, "cursor");
    int valid = server && request_has_provider_token(req) && provider_id &&
                lease_id && cursor &&
                strcmp(provider_id, "turbomedia") == 0 &&
                strcmp(lease_id, "expected-http-lease") == 0 &&
                strcmp(cursor, "0") == 0;
    if (server) {
        atomic_store_explicit(&server->expected_page_valid, valid,
                              memory_order_release);
        atomic_fetch_add_explicit(&server->expected_page_calls, 1,
                                  memory_order_release);
    }
    const int dialog_active =
        server && atomic_load_explicit(&server->expected_dialog_active,
                                       memory_order_acquire);
    const int play_completed =
        server && atomic_load_explicit(&server->expected_play_completed,
                                       memory_order_acquire);
    const char *page = play_completed
                           ? active_after_play_page
                           : (dialog_active ? active_page : opening_page);
    const size_t page_size =
        play_completed ? sizeof(active_after_play_page) - 1u
                       : (dialog_active ? sizeof(active_page) - 1u
                                        : sizeof(opening_page) - 1u);
    reply(res, valid ? 200 : 400, "application/json", valid ? page : "{}",
          valid ? page_size : 2u);
}

static void test_iris_completion_handler(Req *req, Res *res) {
    test_iris_server_t *server = g_iris_server;
    const char *session_id = get_params(req, "sessionId");
    const char *command_id = get_params(req, "commandId");
    int valid = server && request_has_provider_token(req) && session_id &&
                command_id && strcmp(session_id, "session-process") == 0 &&
                (strcmp(command_id, "command-process-start") == 0 ||
                 strcmp(command_id, "command-process-play") == 0 ||
                 strcmp(command_id, "command-process-collect") == 0 ||
                 strcmp(command_id, "command-process-cancel") == 0 ||
                 strcmp(command_id, "command-process-close") == 0 ||
                 strcmp(command_id, "command-live-close") == 0) &&
                copy_request_body(req, server->completion_body,
                                  sizeof(server->completion_body));
    if (valid) {
        valid = strstr(server->completion_body,
                       "\"terminalStatus\":\"succeeded\"") != NULL &&
            strstr(server->completion_body,
                       "\"mediaWorkerId\":\"ivr-worker-dispatch\"") != NULL &&
            strstr(server->completion_body,
                   "\"dialogId\":\"dialog-process\"") != NULL;
    }
    if (valid && strcmp(command_id, "command-process-start") == 0) {
        atomic_store_explicit(&server->expected_dialog_active, 1,
                              memory_order_release);
    } else if (valid && strcmp(command_id, "command-process-play") == 0) {
        atomic_store_explicit(&server->expected_play_completed, 1,
                              memory_order_release);
    } else if (valid && strcmp(command_id, "command-live-close") == 0) {
        atomic_store_explicit(&server->expected_dialog_active, 0,
                              memory_order_release);
    }
    if (server) {
        atomic_store_explicit(&server->completion_valid, valid,
                              memory_order_release);
        atomic_fetch_add_explicit(&server->completion_calls, 1,
                                  memory_order_release);
    }
    reply(res, valid ? 202 : 400, "application/json", "{}", 2u);
}

static void test_iris_event_handler(Req *req, Res *res) {
    test_iris_server_t *server = g_iris_server;
    const char *session_id = get_params(req, "sessionId");
    int valid = server && request_has_provider_token(req) && session_id &&
                strcmp(session_id, "session-process") == 0 &&
                copy_request_body(req, server->event_body,
                                  sizeof(server->event_body));
    if (valid) {
        unsigned long long attempt_generation = 0;
        int has_attempt_generation = extract_unsigned_json_field(
            server->event_body, "\"attempt_generation\":",
            &attempt_generation);
        int common =
            strstr(server->event_body, "\"eventId\":\"media-") !=
                NULL &&
            strstr(server->event_body, "\"source\":\"turbomedia\"") !=
                NULL &&
                strstr(server->event_body,
                       "\"correlationId\":\"dialog-process\"") != NULL &&
            strstr(server->event_body, "\"roomId\":\"room-42\"") != NULL &&
            strstr(server->event_body, "\"callId\":\"call-42\"") != NULL;
        int playback =
            common && strstr(server->event_body,
                             "\"type\":\"playback.finished\"") != NULL;
        int asr = common &&
            strstr(server->event_body, "\"type\":\"asr.final\"") != NULL &&
            strstr(server->event_body, "\"inputId\":\"input-process\"") !=
                NULL &&
            strstr(server->event_body,
                   "\"inputValue\":\"fixture-asr\"") != NULL;
        int dtmf = common &&
            strstr(server->event_body, "\"type\":\"dtmf.final\"") != NULL &&
            strstr(server->event_body, "\"inputId\":\"input-process\"") !=
                NULL &&
            strstr(server->event_body, "\"inputValue\":\"5\"") != NULL;
        int rtc_disconnected =
            common && strstr(server->event_body,
                             "\"type\":\"rtc.disconnected\"") != NULL &&
            has_attempt_generation && attempt_generation > 0;
        int rtc_reconnected =
            common && strstr(server->event_body,
                             "\"type\":\"rtc.reconnected\"") != NULL &&
            has_attempt_generation && attempt_generation > 0;
        int rtc_retry_exhausted =
            common && strstr(server->event_body,
                             "\"type\":\"rtc.retry_exhausted\"") != NULL &&
            has_attempt_generation && attempt_generation > 0;
        if (playback) {
            atomic_store_explicit(&server->playback_event_valid, 1,
                                  memory_order_release);
        }
        if (asr) {
            atomic_store_explicit(&server->asr_event_valid, 1,
                                  memory_order_release);
        }
        if (dtmf) {
            atomic_store_explicit(&server->dtmf_event_valid, 1,
                                  memory_order_release);
        }
        if (rtc_disconnected) {
            atomic_store_explicit(&server->rtc_disconnected_generation,
                                  attempt_generation, memory_order_release);
            atomic_fetch_add_explicit(&server->rtc_disconnected_calls, 1,
                                      memory_order_release);
        }
        if (rtc_reconnected) {
            atomic_store_explicit(&server->rtc_reconnected_generation,
                                  attempt_generation, memory_order_release);
            atomic_fetch_add_explicit(&server->rtc_reconnected_calls, 1,
                                      memory_order_release);
        }
        if (rtc_retry_exhausted) {
            atomic_fetch_add_explicit(&server->rtc_retry_exhausted_calls, 1,
                                      memory_order_release);
        }
        if (rtc_disconnected || rtc_reconnected || rtc_retry_exhausted) {
            atomic_store_explicit(&server->rtc_event_valid, 1,
                                  memory_order_release);
        }
        valid = playback || asr || dtmf || rtc_disconnected ||
                rtc_reconnected || rtc_retry_exhausted;
    }
    if (server) {
        atomic_store_explicit(&server->event_valid, valid,
                              memory_order_release);
        atomic_fetch_add_explicit(&server->event_calls, 1,
                                  memory_order_release);
    }
    reply(res, valid ? 202 : 400, "application/json", "{}", 2u);
}

static void test_speech_tts_handler(Req *req, Res *res) {
    test_iris_server_t *server = g_iris_server;
    char body[512];
    int valid = server && request_has_speech_token(req) &&
                copy_request_body(req, body, sizeof(body)) &&
                strstr(body, "\"model\":\"tts-1\"") != NULL &&
                strstr(body, "\"input\":\"process e2e\"") != NULL &&
                strstr(body, "\"voice\":\"alloy\"") != NULL &&
                strstr(body, "\"response_format\":\"pcm\"") != NULL;
    if (server) {
        atomic_store_explicit(&server->speech_tts_valid, valid,
                              memory_order_release);
        atomic_fetch_add_explicit(&server->speech_tts_calls, 1,
                                  memory_order_release);
    }
    if (!valid) {
        reply(res, 400, "application/json", "{}", 2u);
        return;
    }
    reply(res, 200, "application/octet-stream",
          (const char *)server->speech_pcm, sizeof(server->speech_pcm));
}

typedef void (*test_chttp_handler_fn)(Req *req, Res *res);

static int test_chttp_dispatch(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response, test_chttp_handler_fn handler) {
    Req req = {.request = request,
               .body = request ? request->body : NULL,
               .body_len = request ? request->body_size : 0u};
    Res res = {.response = response, .status = SALTS_OK};

    if (!user || !request || !response || !handler) {
        return SALTS_EINVAL;
    }
    handler(&req, &res);
    return res.status;
}

#define TEST_CHTTP_ROUTE_ADAPTER(name)                                      \
    static int name##_route(void *user,                                     \
                            const chttp_server_request_view *request,        \
                            chttp_server_response *response) {               \
        return test_chttp_dispatch(user, request, response, name);          \
    }

TEST_CHTTP_ROUTE_ADAPTER(test_iris_completion_handler)
TEST_CHTTP_ROUTE_ADAPTER(test_iris_event_handler)
TEST_CHTTP_ROUTE_ADAPTER(test_speech_tts_handler)
TEST_CHTTP_ROUTE_ADAPTER(test_iris_expected_lease_handler)
TEST_CHTTP_ROUTE_ADAPTER(test_iris_expected_page_handler)

static chttp_server_config test_backend_http_server_config(
    const cnet_tls_server_config *tls) {
    cnet_client_config network = test_http_client_config().network;
    network.connection_capacity = TEST_IRIS_PROXY_CONNECTION_CAPACITY;
    const chttp_server_config config = {
        .host = "127.0.0.1",
        .port = TEST_IRIS_BACKEND_TLS_PORT,
        .backlog = TEST_IRIS_PROXY_CONNECTION_CAPACITY,
        .network = network,
        .route_capacity = 8u,
        .max_route_param_count = 3u,
        .max_route_param_bytes = 1024u,
        .max_target_bytes = TEST_CHTTP_MAX_START_LINE_BYTES,
        .max_header_count = TEST_CHTTP_MAX_HEADER_COUNT,
        .max_header_bytes = TEST_CHTTP_MAX_HEADER_BYTES,
        .max_request_body_bytes = TEST_CHTTP_MAX_BODY_BYTES,
        .max_response_header_count = TEST_CHTTP_MAX_HEADER_COUNT,
        .max_response_header_bytes = TEST_CHTTP_MAX_HEADER_BYTES,
        .max_response_body_bytes = TEST_CHTTP_MAX_BODY_BYTES,
        .poll_slice_ms = 10u,
        .tls = tls,
        .max_buffered_response_body_bytes =
            TEST_CHTTP_MAX_BUFFERED_RESPONSE_BYTES,
        .buffer_capacity_bytes = 1024u * 1024u};
    return config;
}

static int test_iris_register_routes(test_iris_server_t *server) {
    int status = chttp_server_post(
        &server->http,
        "/v1/sessions/:sessionId/commands/:commandId/completions",
        test_iris_completion_handler_route, server);
    if (status == SALTS_OK) {
        status = chttp_server_post(
            &server->http, "/v1/sessions/:sessionId/events",
            test_iris_event_handler_route, server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_post(&server->http, "/v1/audio/speech",
                                   test_speech_tts_handler_route, server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_post(
            &server->http,
            "/v1/providers/:providerId/expected-resources/leases",
            test_iris_expected_lease_handler_route, server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_get(
            &server->http,
            "/v1/providers/:providerId/expected-resources/leases/:leaseId/pages/:cursor",
            test_iris_expected_page_handler_route, server);
    }
    return status;
}

static int test_iris_server_start(test_iris_server_t *server) {
    cnet_tls_server_config tls = {0};
    chttp_server_config config;
    int status;

    memset(server, 0, sizeof(*server));
    for (size_t index = 0; index < TEST_SPEECH_PCM_SAMPLES; ++index) {
        server->speech_pcm[index] =
            ((index / TEST_SPEECH_WAVE_HALF_PERIOD) & 1u)
                ? -TEST_SPEECH_WAVE_AMPLITUDE
                : TEST_SPEECH_WAVE_AMPLITUDE;
    }
    tls.size = sizeof(tls);
    tls.cert_file = ROOM_SERVICE_TEST_TLS_CERT_PATH;
    tls.key_file = ROOM_SERVICE_TEST_TLS_KEY_PATH;
    tls.client_auth = CNET_TLS_CLIENT_AUTH_NONE;
    config = test_backend_http_server_config(&tls);
    status = chttp_server_init(&server->http, &config);
    if (status != SALTS_OK) {
        return -1;
    }
    server->http_initialized = 1;
    g_iris_server = server;
    status = test_iris_register_routes(server);
    if (status == SALTS_OK) {
        status = chttp_server_start(&server->http);
    }
    if (status != SALTS_OK) {
        g_iris_server = NULL;
        (void)chttp_server_destroy(&server->http);
        server->http_initialized = 0;
        return -1;
    }
    return 0;
}

static void test_iris_server_stop(test_iris_server_t *server) {
    if (!server || !server->http_initialized) {
        return;
    }
    (void)chttp_server_stop(&server->http, 0u);
    (void)chttp_server_destroy(&server->http);
    server->http_initialized = 0;
    g_iris_server = NULL;
}

#ifdef _WIN32
static void test_iris_proxy_close_pair(test_iris_proxy_t *proxy,
                                       size_t index) {
    if (proxy->clients[index] != INVALID_SOCKET) {
        shutdown(proxy->clients[index], SD_BOTH);
        closesocket(proxy->clients[index]);
        proxy->clients[index] = INVALID_SOCKET;
    }
    if (proxy->backends[index] != INVALID_SOCKET) {
        shutdown(proxy->backends[index], SD_BOTH);
        closesocket(proxy->backends[index]);
        proxy->backends[index] = INVALID_SOCKET;
    }
}

static int test_iris_proxy_send_all(SOCKET socket, const char *data, int size) {
    int sent = 0;
    while (sent < size) {
        int written = send(socket, data + sent, size - sent, 0);
        if (written <= 0) return 0;
        sent += written;
    }
    return 1;
}

static void test_iris_proxy_thread(void *context) {
    test_iris_proxy_t *proxy = (test_iris_proxy_t *)context;
    while (!atomic_load_explicit(&proxy->stopping, memory_order_acquire)) {
        fd_set readable;
        struct timeval timeout;
        SOCKET maximum = proxy->listener;
        if (!atomic_load_explicit(&proxy->available, memory_order_acquire)) {
            for (size_t index = 0u;
                 index < TEST_IRIS_PROXY_CONNECTION_CAPACITY; ++index) {
                test_iris_proxy_close_pair(proxy, index);
            }
        }
        FD_ZERO(&readable);
        if (proxy->listener != INVALID_SOCKET) FD_SET(proxy->listener, &readable);
        for (size_t index = 0u; index < TEST_IRIS_PROXY_CONNECTION_CAPACITY;
             ++index) {
            if (proxy->clients[index] != INVALID_SOCKET) {
                FD_SET(proxy->clients[index], &readable);
                if (proxy->clients[index] > maximum) maximum = proxy->clients[index];
            }
            if (proxy->backends[index] != INVALID_SOCKET) {
                FD_SET(proxy->backends[index], &readable);
                if (proxy->backends[index] > maximum) maximum = proxy->backends[index];
            }
        }
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        if (select((int)maximum + 1, &readable, NULL, NULL, &timeout) <= 0) {
            continue;
        }
        if (proxy->listener != INVALID_SOCKET &&
            FD_ISSET(proxy->listener, &readable)) {
            SOCKET client = accept(proxy->listener, NULL, NULL);
            if (client != INVALID_SOCKET) {
                SOCKET backend = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                struct sockaddr_in address;
                size_t slot = TEST_IRIS_PROXY_CONNECTION_CAPACITY;
                for (size_t index = 0u;
                     index < TEST_IRIS_PROXY_CONNECTION_CAPACITY; ++index) {
                    if (proxy->clients[index] == INVALID_SOCKET) {
                        slot = index;
                        break;
                    }
                }
                memset(&address, 0, sizeof(address));
                address.sin_family = AF_INET;
                address.sin_port = htons((uint16_t)proxy->backend_port);
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (!atomic_load_explicit(&proxy->available,
                                          memory_order_acquire) ||
                    slot == TEST_IRIS_PROXY_CONNECTION_CAPACITY ||
                    backend == INVALID_SOCKET ||
                    connect(backend, (struct sockaddr *)&address,
                            sizeof(address)) != 0) {
                    closesocket(client);
                    if (backend != INVALID_SOCKET) closesocket(backend);
                } else {
                    proxy->clients[slot] = client;
                    proxy->backends[slot] = backend;
                }
            }
        }
        for (size_t index = 0u; index < TEST_IRIS_PROXY_CONNECTION_CAPACITY;
             ++index) {
            char buffer[16384];
            SOCKET source;
            SOCKET target;
            int received;
            if (proxy->clients[index] == INVALID_SOCKET) continue;
            if (FD_ISSET(proxy->clients[index], &readable)) {
                source = proxy->clients[index];
                target = proxy->backends[index];
            } else if (FD_ISSET(proxy->backends[index], &readable)) {
                source = proxy->backends[index];
                target = proxy->clients[index];
            } else {
                continue;
            }
            received = recv(source, buffer, sizeof(buffer), 0);
            if (received <= 0 ||
                !test_iris_proxy_send_all(target, buffer, received)) {
                test_iris_proxy_close_pair(proxy, index);
            }
        }
    }
    for (size_t index = 0u; index < TEST_IRIS_PROXY_CONNECTION_CAPACITY;
         ++index) {
        test_iris_proxy_close_pair(proxy, index);
    }
}

static int test_iris_proxy_start(test_iris_proxy_t *proxy, int listen_port,
                                 int backend_port) {
    WSADATA wsa;
    BOOL reuse = TRUE;
    struct sockaddr_in address;
    if (!proxy || listen_port <= 0 || listen_port > 65535 ||
        backend_port <= 0 || backend_port > 65535) {
        return -1;
    }
    memset(proxy, 0, sizeof(*proxy));
    proxy->listen_port = listen_port;
    proxy->backend_port = backend_port;
    proxy->listener = INVALID_SOCKET;
    for (size_t index = 0u; index < TEST_IRIS_PROXY_CONNECTION_CAPACITY;
         ++index) {
        proxy->clients[index] = INVALID_SOCKET;
        proxy->backends[index] = INVALID_SOCKET;
    }
    atomic_init(&proxy->stopping, 0);
    atomic_init(&proxy->available, 1);
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    proxy->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (proxy->listener == INVALID_SOCKET) goto fail;
    (void)setsockopt(proxy->listener, SOL_SOCKET, SO_REUSEADDR,
                     (const char *)&reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)proxy->listen_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(proxy->listener, (struct sockaddr *)&address, sizeof(address)) !=
            0 ||
        listen(proxy->listener, SOMAXCONN) != 0) {
        goto fail;
    }
    if (salts_thread_create(&proxy->thread, test_iris_proxy_thread, proxy) !=
        0) {
        goto fail;
    }
    proxy->thread_started = 1;
    return 0;
fail:
    if (proxy->listener != INVALID_SOCKET) closesocket(proxy->listener);
    proxy->listener = INVALID_SOCKET;
    WSACleanup();
    return -1;
}

static void test_iris_proxy_stop(test_iris_proxy_t *proxy) {
    if (!proxy || !proxy->thread_started) return;
    atomic_store_explicit(&proxy->stopping, 1, memory_order_release);
    shutdown(proxy->listener, SD_BOTH);
    closesocket(proxy->listener);
    proxy->listener = INVALID_SOCKET;
    salts_thread_join(&proxy->thread);
    proxy->thread_started = 0;
    WSACleanup();
}

static void test_iris_proxy_set_available(test_iris_proxy_t *proxy,
                                          int available) {
    if (!proxy || !proxy->thread_started) return;
    atomic_store_explicit(&proxy->available, available ? 1 : 0,
                          memory_order_release);
}
#else
static int test_iris_proxy_start(test_iris_proxy_t *proxy, int listen_port,
                                 int backend_port) {
    (void)proxy;
    (void)listen_port;
    (void)backend_port;
    return -1;
}
static void test_iris_proxy_stop(test_iris_proxy_t *proxy) { (void)proxy; }
static void test_iris_proxy_set_available(test_iris_proxy_t *proxy,
                                          int available) {
    (void)proxy;
    (void)available;
}
#endif

static char *copy_environment(const char *name) {
    const char *value = getenv(name);
    size_t size;
    char *copy;
    if (!value) return NULL;
    size = strlen(value) + 1u;
    copy = (char *)malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}

static int set_environment(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value ? value : "");
#else
    return value ? setenv(name, value, 1) : unsetenv(name);
#endif
}

/* ------------------------------------------------------------------ */
/* process spawn with stdout redirect                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    PROCESS_HANDLE handle;
    char *stdout_path;
} child_t;

static int spawn_with_stdout(const char *exe, const char *const *args,
                             const char *stdout_path, child_t *out) {
#ifdef _WIN32
    char cmdline[4096];
    size_t off = 0;
    off += (size_t)snprintf(cmdline + off, sizeof(cmdline) - off, "\"%s\"",
                            exe);
    for (int i = 0; args[i] && off < sizeof(cmdline); i++) {
        off += (size_t)snprintf(cmdline + off, sizeof(cmdline) - off, " \"%s\"",
                                args[i]);
    }
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out_file = CreateFileA(stdout_path, GENERIC_WRITE, FILE_SHARE_READ,
                                  &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                  NULL);
    if (out_file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        fprintf(stderr, "CreateFile failed error=%lu path=%s\n",
                (unsigned long)error, stdout_path);
        return -1;
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_file;
    si.hStdError = out_file;
    BOOL ok = CreateProcessA(exe, cmdline, NULL, NULL, TRUE, 0, NULL, NULL,
                             &si, &pi);
    DWORD create_error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(out_file);
    if (!ok) {
        fprintf(stderr, "CreateProcess failed error=%lu command=%s\n",
                (unsigned long)create_error, cmdline);
        return -1;
    }
    CloseHandle(pi.hThread);
    out->handle = pi.hProcess;
    out->stdout_path = _strdup(stdout_path);
    return 0;
#else
    (void)exe;
    (void)args;
    (void)stdout_path;
    (void)out;
    return -1; /* POSIX spawn not exercised by this host */
#endif
}

static void kill_child(child_t *child) {
    if (!child || child->handle == NULL) {
        return;
    }
#ifdef _WIN32
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(child->handle, &exit_code) ||
        exit_code == STILL_ACTIVE) {
        (void)TerminateProcess(child->handle, 0);
    }
    DWORD wait_result =
        WaitForSingleObject(child->handle, TEST_CHILD_EXIT_TIMEOUT_MS);
    if (wait_result != WAIT_OBJECT_0) {
        fprintf(stderr, "child process did not exit, wait_result=%lu\n",
                (unsigned long)wait_result);
    }
    CloseHandle(child->handle);
#else
    kill(child->handle, SIGKILL);
#endif
    child->handle = NULL;
    free(child->stdout_path);
    child->stdout_path = NULL;
}

static int wait_child_exit(child_t *child, unsigned int timeout_ms) {
    if (!child || child->handle == NULL || timeout_ms == 0u) return 0;
#ifdef _WIN32
    if (WaitForSingleObject(child->handle, timeout_ms) != WAIT_OBJECT_0) {
        return 0;
    }
    CloseHandle(child->handle);
#else
    (void)timeout_ms;
    return 0;
#endif
    child->handle = NULL;
    free(child->stdout_path);
    child->stdout_path = NULL;
    return 1;
}

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    size_t needle_size;
    size_t retained = 0u;
    char buf[8192];
    if (!f) {
        return 0;
    }
    needle_size = needle ? strlen(needle) : 0u;
    if (needle_size == 0u || needle_size >= sizeof(buf)) {
        fclose(f);
        return 0;
    }
    for (;;) {
        size_t n = fread(buf + retained, 1,
                         sizeof(buf) - 1u - retained, f);
        size_t available = retained + n;
        buf[available] = '\0';
        if (strstr(buf, needle)) {
            fclose(f);
            return 1;
        }
        if (n == 0u) break;
        retained = needle_size - 1u;
        if (retained > available) retained = available;
        memmove(buf, buf + available - retained, retained);
    }
    fclose(f);
    return 0;
}

static void print_file_on_failure(const char *label, const char *path) {
    FILE *f = fopen(path, "rb");
    char buf[8192];
    long file_size;
    long start_offset = 0;
    size_t n;
    if (!f) {
        fprintf(stderr, "%s: cannot open %s\n", label, path);
        return;
    }
    if (fseek(f, 0, SEEK_END) == 0) {
        file_size = ftell(f);
        if (file_size > (long)(sizeof(buf) - 1u)) {
            start_offset = file_size - (long)(sizeof(buf) - 1u);
        }
    }
    if (fseek(f, start_offset, SEEK_SET) != 0) {
        rewind(f);
        start_offset = 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    fprintf(stderr, "--- %s (%s, offset=%ld) ---\n%s\n", label, path,
            start_offset, buf);
}

/* ------------------------------------------------------------------ */
/* minimal HTTP POST (control API create_room)                         */
/* ------------------------------------------------------------------ */

static int http_request_raw(const char *host, int port, const char *method,
                            const char *path, const char *token,
                            const char *body, char *resp, size_t resp_cap) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return -1;
    }
    DWORD socket_timeout = TEST_HTTP_IO_TIMEOUT_MS;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                   (const char *)&socket_timeout,
                   sizeof(socket_timeout)) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&socket_timeout,
                   sizeof(socket_timeout)) != 0) {
        closesocket(sock);
        WSACleanup();
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(sock);
        WSACleanup();
        return -1;
    }
#else
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct timeval socket_timeout;
    socket_timeout.tv_sec = TEST_HTTP_IO_TIMEOUT_MS / 1000;
    socket_timeout.tv_usec =
        (TEST_HTTP_IO_TIMEOUT_MS % 1000) * 1000;
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &socket_timeout,
                   sizeof(socket_timeout)) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout,
                   sizeof(socket_timeout)) != 0) {
        close(sock);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }
#endif
    char req[1024];
    const char *payload = body ? body : "";
    const char *authorization = token ? "Authorization: Bearer " : "";
    const char *authorization_value = token ? token : "";
    const char *authorization_end = token ? "\r\n" : "";
    const char *content_type = body ? "Content-Type: application/json\r\n" : "";
    int n = snprintf(
        req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "%s%s%s"
        "%s"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        method, path, host, port, authorization, authorization_value,
        authorization_end, content_type, strlen(payload), payload);
#ifdef _WIN32
    send(sock, req, (int)n, 0);
    size_t total = 0;
    while (total + 1 < resp_cap) {
        int r = recv(sock, resp + total, (int)(resp_cap - 1 - total), 0);
        if (r <= 0) {
            break;
        }
        total += (size_t)r;
    }
    resp[total] = '\0';
    closesocket(sock);
    WSACleanup();
#else
    write(sock, req, (size_t)n);
    size_t total = 0;
    while (total + 1 < resp_cap) {
        ssize_t r = read(sock, resp + total, resp_cap - 1 - total);
        if (r <= 0) {
            break;
        }
        total += (size_t)r;
    }
    resp[total] = '\0';
    close(sock);
#endif
    return strstr(resp, " 200 ") ? 0 : -1;
}

static int http_post_command(const char *host, int port,
                             const char *token, const char *body,
                             char *resp, size_t resp_cap) {
    return http_request_raw(host, port, "POST", "/api/v1/commands", token,
                            body, resp, resp_cap);
}

static int wait_real_worker_ready(void) {
    char response[2048];
    for (int attempt = 0; attempt < TEST_REAL_WORKER_READY_ATTEMPTS;
         ++attempt) {
        if (http_request_raw("127.0.0.1", TEST_REAL_WORKER_HEALTH_PORT, "GET",
                             "/ready", NULL, NULL, response,
                             sizeof(response)) == 0) {
            return 1;
        }
        proc_sleep(TEST_REAL_WORKER_READY_POLL_MS);
    }
    return 0;
}

static int wait_real_worker_media_links(unsigned int expected) {
    char response[4096];
    char metric[128];
    int written = snprintf(metric, sizeof(metric),
                           "turbo_ivr_worker_media_links_connected %u\n",
                           expected);
    if (written <= 0 || (size_t)written >= sizeof(metric)) {
        return 0;
    }
    for (int attempt = 0; attempt < TEST_RTC_TRANSITION_ATTEMPTS; ++attempt) {
        if (http_request_raw("127.0.0.1", TEST_REAL_WORKER_HEALTH_PORT,
                             "GET", "/metrics", NULL, NULL, response,
                             sizeof(response)) == 0 &&
            strstr(response, metric) != NULL) {
            return 1;
        }
        proc_sleep(TEST_RTC_TRANSITION_POLL_MS);
    }
    return 0;
}

static unsigned long metric_value(const char *metrics, const char *name) {
    char prefix[160];
    const char *value;
    int written;
    if (!metrics || !name || name[0] == '\0') return 0u;
    written = snprintf(prefix, sizeof(prefix), "\n%s ", name);
    if (written <= 0 || (size_t)written >= sizeof(prefix)) return 0u;
    value = strstr(metrics, prefix);
    return value ? strtoul(value + strlen(prefix), NULL, 10) : 0u;
}

static int read_real_worker_metric(const char *name,
                                   unsigned long *out_value) {
    char response[16384];
    if (!name || !out_value ||
        http_request_raw("127.0.0.1", TEST_REAL_WORKER_HEALTH_PORT,
                         "GET", "/metrics", NULL, NULL, response,
                         sizeof(response)) != 0) {
        return 0;
    }
    *out_value = metric_value(response, name);
    return 1;
}

static int read_room_service_metric(const char *name,
                                    unsigned long *out_value) {
    char response[65536];
    if (!name || !out_value ||
        http_request_raw("127.0.0.1", TEST_HTTP_PORT, "GET", "/metrics",
                         NULL, NULL, response, sizeof(response)) != 0) {
        return 0;
    }
    *out_value = metric_value(response, name);
    return 1;
}

static int wait_room_service_metric_greater_than(const char *name,
                                                 unsigned long baseline,
                                                 int attempts) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        unsigned long value = 0u;
        if (read_room_service_metric(name, &value) && value > baseline) {
            return 1;
        }
        proc_sleep(TEST_RTC_TRANSITION_POLL_MS);
    }
    return 0;
}

static int probe_restarted_iris(void) {
    chttp_client_config client_config = test_http_client_config();
    cnet_tls_client_config tls_config = {0};
    chttp_tls_profile tls_profile = {0};
    chttp_client client = {0};
    chttp_options options = {0};
    chttp_response response = {0};
    chttp_error error = {0};
    int client_initialized = 0;
    int tls_initialized = 0;
    int status = 0;

    if (chttp_client_init(&client, &client_config) != SALTS_OK) {
        goto cleanup;
    }
    client_initialized = 1;
    tls_config.size = sizeof(tls_config);
    tls_config.ca_file = ROOM_SERVICE_TEST_TLS_CERT_PATH;
    tls_config.server_name = "localhost";
    if (chttp_tls_profile_init(&tls_profile, &tls_config) != SALTS_OK) {
        goto cleanup;
    }
    tls_initialized = 1;
    options.connection_uri = "tls://127.0.0.1:19997";
    options.authority = "localhost:19997";
    options.target = "/probe";
    options.timeout_ms = 1000u;
    options.tls = &tls_profile;
    if (chttp_get(&client, &options, &response, &error) == SALTS_OK) {
        status = (int)response.status_code;
    }
cleanup:
    chttp_response_destroy(&response);
    if (client_initialized) {
        (void)chttp_client_destroy(&client, TEST_HTTP_IO_TIMEOUT_MS);
    }
    if (tls_initialized) {
        (void)chttp_tls_profile_destroy(&tls_profile);
    }
    return status;
}

static void print_room_service_iris_outbox_metrics(void) {
    static const char *const names[] = {
        "turbo_room_service_iris_outbox_pending_records",
        "turbo_room_service_iris_outbox_in_flight_records",
        "turbo_room_service_iris_outbox_dead_records",
        "turbo_room_service_iris_outbox_persisted_total",
        "turbo_room_service_iris_outbox_delivered_total",
        "turbo_room_service_iris_outbox_dead_lettered_total",
        "turbo_room_service_iris_queue_items",
        "turbo_room_service_iris_in_flight",
        "turbo_room_service_iris_delivery_attempts_total",
        "turbo_room_service_iris_retries_total",
        "turbo_room_service_iris_event_failure_total",
    };
    char response[65536];
    if (http_request_raw("127.0.0.1", TEST_HTTP_PORT, "GET", "/metrics",
                         NULL, NULL, response, sizeof(response)) != 0) {
        fprintf(stderr, "RoomService Iris metrics unavailable\n");
        return;
    }
    fprintf(stderr, "RoomService Iris outbox metrics:\n");
    for (size_t index = 0u; index < sizeof(names) / sizeof(names[0]);
         ++index) {
        fprintf(stderr, "  %s=%lu\n", names[index],
                metric_value(response, names[index]));
    }
}

/* ------------------------------------------------------------------ */
/* shared fixtures                                                     */
/* ------------------------------------------------------------------ */

static DataBind *g_codec = NULL;
static child_t g_room_service;
static child_t g_worker;
static child_t g_sfu;
static ivr_whip_transport_t *g_caller_transport;
static test_iris_server_t g_iris;
static test_iris_proxy_t g_iris_proxy;
static test_iris_proxy_t g_control_ws_proxy;
static test_iris_control_peer_t *g_iris_control_peer;
static chttp_client g_http_client;
static int g_http_client_initialized;
static char g_cfg_path[1024];
static char g_store_path[1024];
static char g_database_path[1024];
static char g_ledger_database_path[1024];
static char g_rs_out[1024];
static char g_wk_out[1024];
static char g_sfu_cfg_path[1024];
static char g_sfu_out[1024];
static char g_orphan_resume_path[1024];
static int g_seq = 0;
static int g_router_bind_port = TEST_ROUTER_PORT;

static const char TEST_PROCESS_START_COMMAND[] =
    "{"
    "\"schemaVersion\":2,"
    "\"commandId\":\"command-process-start\","
    "\"tenantId\":\"tenant-process\","
    "\"sessionId\":\"session-process\","
    "\"type\":\"dialog.start\","
    "\"provider\":\"turbomedia\","
    "\"correlationId\":\"dialog-process\","
    "\"causationId\":\"state-process\","
    "\"deadline\":\"2099-01-01T00:00:00Z\","
    "\"workerId\":\"iris-worker-process\","
    "\"dispatchEpoch\":1,"
    "\"data\":{"
    "\"capability\":\"ivr\","
    "\"dialogId\":\"dialog-process\","
    "\"roomId\":\"room-42\","
    "\"callId\":\"call-42\","
    "\"callGeneration\":1,"
    "\"operationGeneration\":1"
    "}"
    "}";

static const char TEST_PROCESS_PLAY_COMMAND[] =
    "{"
    "\"schemaVersion\":2,"
    "\"commandId\":\"command-process-play\","
    "\"tenantId\":\"tenant-process\","
    "\"sessionId\":\"session-process\","
    "\"type\":\"media.play\","
    "\"provider\":\"turbomedia\","
    "\"correlationId\":\"dialog-process\","
    "\"causationId\":\"state-process-started\","
    "\"deadline\":\"2099-01-01T00:00:00Z\","
    "\"workerId\":\"iris-worker-process\","
    "\"dispatchEpoch\":1,"
    "\"data\":{"
    "\"capability\":\"ivr\","
    "\"dialogId\":\"dialog-process\","
    "\"roomId\":\"room-42\","
    "\"callId\":\"call-42\","
    "\"callGeneration\":1,"
    "\"operationGeneration\":2,"
    "\"text\":\"process e2e\""
    "}"
    "}";

static const ivr_call_ref_t TEST_CALLER_CALL = {
    .tenant_id = {"tenant-process", 14u},
    .provider_session_id = {"caller-provider-session", 23u},
    .dialog_id = {"caller-dialog", 13u},
    .room_id = {"room-42", 7u},
    .call_id = {TEST_SFU_CALLER_PARTICIPANT_ID,
                sizeof(TEST_SFU_CALLER_PARTICIPANT_ID) - 1u},
    .call_generation = 1u,
    .expected_room_version = 0u};

static int spawn_probe_worker(const char *worker_id, const char *mode);

static void normalize_config_path(char *path) {
    if (!path) return;
    for (; *path; ++path) {
        if (*path == '\\') *path = '/';
    }
}

static int write_room_service_config(int secure_control_ws) {
    FILE *config = fopen(g_cfg_path, "wb");
    int ok;
    if (!config) {
        return 0;
    }
    ok = fprintf(
             config,
             "[server]\nhost = \"127.0.0.1\"\nport = %d\nuse_tls = false\n"
             "node_id = \"room-service-dispatch\"\n"
             "[control]\ntoken = \"%s\"\n"
             "[capacity]\nmax_rooms = 16\n"
             "[rooms]\nauto_create = true\n"
             "[runtime]\ndry_run = false\n"
             "[logging]\nlevel = \"warn\"\n"
             "[iris_provider]\n"
             "control_ws_host = \"127.0.0.1\"\n"
             "control_ws_port = %d\n"
             "control_ws_path = \"/internal/iris/control\"\n"
             "provider_instance_id = \"turbomedia-process\"\n"
             "iris_identity = \"iris-process\"\n"
             "control_ws_use_tls = false\n"
             "control_ws_allow_insecure_loopback = true\n"
             "event_store_config = \"%s\"\n"
             "event_store_channel = \"iris.media_events\"\n"
             "command_ledger_channel = \"iris.provider_commands\"\n"
             "allow_development_sqlite = true\n"
             "correlation_capacity = 16\n"
             "completion_queue_capacity = 16\n"
             "outbox_request_queue_capacity = 16\n"
             "retry_max_attempts = 8\n"
             "retry_backoff_ms = 100\n"
             "ack_timeout_ms = 3000\n"
             "drain_timeout_ms = 5000\n"
             "[control_ws]\nbind_host = \"127.0.0.1\"\nbind_port = %d\n"
             "dispatch_deadline_ms = 1000\n",
             TEST_HTTP_PORT, TEST_CTRL_TOKEN, TEST_IRIS_CONTROL_PORT,
             g_store_path, g_router_bind_port) > 0;
    if (ok && secure_control_ws) {
        ok = fprintf(
                 config,
                 "use_tls = true\n"
                 "ca_file = \"%s\"\n"
                 "cert_file = \"%s\"\n"
                 "key_file = \"%s\"\n"
                 "worker_heartbeat_ms = 500\n"
                 "worker_lease_ms = 2000\n"
                 "[[control_ws.workers]]\n"
                 "worker_id = \"ivr-worker-dispatch\"\n"
                 "active_certificate_sha256 = \"%s\"\n"
                 "generation = 1\n",
                 ROOM_SERVICE_TEST_TLS_CERT_PATH,
                 ROOM_SERVICE_TEST_TLS_CERT_PATH,
                 ROOM_SERVICE_TEST_TLS_KEY_PATH,
                 TEST_FIXTURE_CERTIFICATE_SHA256) > 0;
    } else if (ok) {
        ok = fprintf(config, "allow_insecure_loopback = true\n") > 0;
    }
    if (fclose(config) != 0) {
        ok = 0;
    }
    return ok;
}

static int provision_room_service_room(void) {
    char response[2048];
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (http_post_command(
                "127.0.0.1", TEST_HTTP_PORT, TEST_CTRL_TOKEN,
                "{\"type\":\"create_room\",\"room_id\":\"room-42\","
                "\"room_type\":\"conference\"}",
                response, sizeof(response)) == 0) {
            if (http_post_command(
                    "127.0.0.1", TEST_HTTP_PORT, TEST_CTRL_TOKEN,
                    "{\"type\":\"add_participant\",\"room_id\":\"room-42\","
                    "\"participant\":{\"participant_id\":\"call-42\","
                    "\"user_id\":\"caller-42\",\"display_name\":\"Caller\","
                    "\"role\":\"customer\"}}",
                    response, sizeof(response)) == 0 &&
                http_post_command(
                    "127.0.0.1", TEST_HTTP_PORT, TEST_CTRL_TOKEN,
                    "{\"type\":\"publish_track\",\"room_id\":\"room-42\","
                    "\"track\":{\"track_id\":\"caller-audio\","
                    "\"owner_participant_id\":\"call-42\",\"kind\":\"audio\","
                    "\"source\":\"mic\",\"codec_name\":\"opus\","
                    "\"main_ssrc\":4242}}",
                    response, sizeof(response)) == 0) {
                return 1;
            }
        }
        proc_sleep(500);
    }
    return 0;
}

static int write_sfu_config(void) {
    FILE *config = fopen(g_sfu_cfg_path, "wb");
    int ok;
    if (!config) {
        return 0;
    }
    ok = fprintf(config,
                 "[server]\nhost = \"127.0.0.1\"\nport = %d\n"
                 "use_tls = false\nnode_id = \"sfu-process-1\"\n"
                 "[capacity]\nmax_rooms = 16\ndefault_room_capacity = 8\n"
                 "[control]\ntoken = \"%s\"\n"
                 "[media]\naccess_token = \"%s\"\n"
                 "[ice]\nallow_loopback = true\n"
                 "[logging]\nlevel = \"warn\"\n",
                 TEST_SFU_PORT, TEST_SFU_CTRL_TOKEN,
                 TEST_SFU_MEDIA_TOKEN) > 0;
    if (fclose(config) != 0) {
        ok = 0;
    }
    return ok;
}

static int spawn_sfu(void) {
    char executable[1024];
    const char *args[] = {"--config", g_sfu_cfg_path, NULL};
    int written;
    if (g_sfu.handle != NULL) {
        return 0;
    }
    written = snprintf(executable, sizeof(executable), "%s/%s", TEST_BIN_DIR,
                       SFU_NODE_BIN);
    return written > 0 && (size_t)written < sizeof(executable) &&
           spawn_with_stdout(executable, args, g_sfu_out, &g_sfu) == 0;
}

static int provision_sfu_room(void) {
    char response[4096];
    for (int attempt = 0; attempt < TEST_SFU_READY_ATTEMPTS; ++attempt) {
        if (http_post_command(
                "127.0.0.1", TEST_SFU_PORT, TEST_SFU_CTRL_TOKEN,
                "{\"type\":\"attach_room\",\"room_id\":\"room-42\","
                "\"max_participants\":8}",
                response, sizeof(response)) == 0) {
            return 1;
        }
        proc_sleep(TEST_SFU_READY_POLL_MS);
    }
    return 0;
}

static int start_sfu_caller_transport(void) {
    ivr_whip_transport_config_t config;
    char sfu_base_url[64];

    if (g_caller_transport) {
        return 0;
    }
    memset(&config, 0, sizeof(config));
    snprintf(sfu_base_url, sizeof(sfu_base_url), "http://127.0.0.1:%d",
             TEST_SFU_PORT);
    config.sfu_base_url = sfu_base_url;
    config.media_token = TEST_SFU_MEDIA_TOKEN;
    config.allow_plaintext_loopback = 1;
    config.allow_loopback = 1;
    config.sample_rate = TEST_CALLER_SAMPLE_RATE;
    config.connect_timeout_ms = TEST_CALLER_CONNECT_TIMEOUT_MS;
    if (ivr_whip_transport_create(&config, &g_caller_transport) != IVR_OK ||
        ivr_whip_transport_start(g_caller_transport, &TEST_CALLER_CALL) !=
            IVR_OK) {
        return 0;
    }
    for (int attempt = 0; attempt < TEST_RTC_TRANSITION_ATTEMPTS; ++attempt) {
        if (ivr_whip_transport_connected(g_caller_transport) &&
            ivr_whip_transport_ssrc(g_caller_transport) != 0u) {
            return 1;
        }
        proc_sleep(TEST_RTC_TRANSITION_POLL_MS);
    }
    return 0;
}

static int provision_sfu_caller_subscription(void) {
    char command[1024];
    char response[4096];
    int written;

    if (!g_caller_transport ||
        ivr_whip_transport_ssrc(g_caller_transport) == 0u) {
        return 0;
    }
    written = snprintf(
        command, sizeof(command),
        "{\"type\":\"set_track_subscription\",\"room_id\":\"room-42\","
        "\"receiver_participant_id\":\"call-42-rx\",\"track_id\":\"%s\","
        "\"enabled\":true,\"muted\":false,\"policy_source\":\"ivr\"}",
        TEST_SFU_CALLER_TRACK_ID);
    return written > 0 && (size_t)written < sizeof(command) &&
           http_post_command("127.0.0.1", TEST_SFU_PORT,
                             TEST_SFU_CTRL_TOKEN, command, response,
                             sizeof(response)) == 0;
}

static int stop_sfu_caller_transport(void) {
    if (!g_caller_transport) {
        return 0;
    }
    ivr_whip_transport_destroy(g_caller_transport);
    g_caller_transport = NULL;
    return 1;
}

static int send_sfu_caller_audio(void) {
    ivr_media_transport_t publisher;
    static int16_t samples[TEST_CALLER_SAMPLES_PER_FRAME];

    if (!g_caller_transport) {
        return 0;
    }
    memset(&publisher, 0, sizeof(publisher));
    ivr_whip_transport_get_transport(g_caller_transport, &publisher);
    if (!publisher.play_audio) {
        return 0;
    }
    for (int index = 0; index < TEST_CALLER_SAMPLES_PER_FRAME; ++index) {
        samples[index] = (int16_t)(800 + (index % 11) * 100);
    }
    for (int frame = 0; frame < TEST_CALLER_AUDIO_FRAME_COUNT; ++frame) {
        if (publisher.play_audio(publisher.context, &TEST_CALLER_CALL,
                                 (const uint8_t *)samples, sizeof(samples),
                                 TEST_CALLER_SAMPLE_RATE) != 0) {
            return 0;
        }
        proc_sleep(TEST_CALLER_AUDIO_FRAME_MS);
    }
    return 1;
}

static int disconnect_sfu_media_participant(const char *participant_id) {
    char command[512];
    char response[2048];
    int written;
    int rc;
    if (!participant_id || participant_id[0] == '\0') return 0;
    written = snprintf(
        command, sizeof(command),
        "{\"type\":\"disconnect_media_participant\","
        "\"room_id\":\"room-42\",\"participant_id\":\"%s\"}",
        participant_id);
    if (written <= 0 || (size_t)written >= sizeof(command)) return 0;
    rc = http_post_command("127.0.0.1", TEST_SFU_PORT,
                           TEST_SFU_CTRL_TOKEN, command, response,
                           sizeof(response));
    if (rc != 0) {
        fprintf(stderr, "disconnect participant %s response:\n%s\n",
                participant_id, response);
    }
    return rc == 0;
}

static int sfu_media_participant_exists(const char *participant_id) {
    char command[512];
    char response[2048];
    int written;
    if (!participant_id || participant_id[0] == '\0') return 0;
    written = snprintf(command, sizeof(command),
                       "{\"type\":\"get_participant_stats\","
                       "\"room_id\":\"room-42\",\"participant_id\":\"%s\"}",
                       participant_id);
    if (written <= 0 || (size_t)written >= sizeof(command)) return 0;
    if (http_post_command("127.0.0.1", TEST_SFU_PORT,
                          TEST_SFU_CTRL_TOKEN, command, response,
                          sizeof(response)) != 0) {
        fprintf(stderr, "participant stats %s response:\n%s\n", participant_id,
                response);
        return 0;
    }
    return 1;
}

static int wait_sfu_room_counts(unsigned sessions, unsigned participants) {
    char response[4096];
    char session_needle[64];
    char participant_needle[64];
    int session_written = snprintf(session_needle, sizeof(session_needle),
                                   "\"session_count\":%u", sessions);
    int participant_written = snprintf(
        participant_needle, sizeof(participant_needle),
        "\"participant_count\":%u", participants);
    if (session_written <= 0 ||
        (size_t)session_written >= sizeof(session_needle) ||
        participant_written <= 0 ||
        (size_t)participant_written >= sizeof(participant_needle)) {
        return 0;
    }
    for (int attempt = 0; attempt < TEST_RTC_TRANSITION_ATTEMPTS; ++attempt) {
        if (http_post_command(
                "127.0.0.1", TEST_SFU_PORT, TEST_SFU_CTRL_TOKEN,
                "{\"type\":\"get_room_stats\",\"room_id\":\"room-42\"}",
                response, sizeof(response)) == 0 &&
            strstr(response, session_needle) &&
            strstr(response, participant_needle)) {
            return 1;
        }
        proc_sleep(TEST_RTC_TRANSITION_POLL_MS);
    }
    return 0;
}

static int read_sfu_room_packet_count(unsigned long long *out_count) {
    char response[4096];
    if (!out_count ||
        http_post_command(
            "127.0.0.1", TEST_SFU_PORT, TEST_SFU_CTRL_TOKEN,
            "{\"type\":\"get_room_stats\",\"room_id\":\"room-42\"}",
            response, sizeof(response)) != 0) {
        return 0;
    }
    return extract_unsigned_json_field(response,
                                       "\"total_packets_routed\":",
                                       out_count);
}

static int wait_sfu_room_packet_count_greater_than(
    unsigned long long baseline) {
    for (int attempt = 0; attempt < TEST_TTS_RTP_ATTEMPTS; ++attempt) {
        unsigned long long current = 0u;
        if (read_sfu_room_packet_count(&current) && current > baseline) {
            return 1;
        }
        proc_sleep(TEST_RTC_TRANSITION_POLL_MS);
    }
    return 0;
}

static int spawn_room_service(void) {
    char room_service_bin[1024];
    const char *args[] = {"--config", g_cfg_path, NULL};
    int written;
    if (g_room_service.handle != NULL) {
        return 0;
    }
    written = snprintf(room_service_bin, sizeof(room_service_bin), "%s/%s",
                       TEST_BIN_DIR, ROOM_SERVICE_BIN);
    if (written <= 0 || (size_t)written >= sizeof(room_service_bin)) {
        return 0;
    }
    return spawn_with_stdout(room_service_bin, args, g_rs_out,
                             &g_room_service) == 0;
}

static int process_peer_query(void *context, char *payload_json,
                              size_t payload_capacity, int *available) {
    static const char opening[] =
        "{\"schemaVersion\":1,\"resourceCount\":1,\"resources\":[{"
        "\"tenantId\":\"tenant-process\",\"providerId\":\"turbomedia\","
        "\"sessionId\":\"session-process\",\"sessionRevision\":1,"
        "\"ownerNodeId\":\"iris-process\",\"ownerEpoch\":1,"
        "\"ownerLeaseExpiresAtUnixMs\":4102444800000,"
        "\"dialogId\":\"dialog-process\",\"roomId\":\"room-42\","
        "\"callId\":\"call-42\",\"callGeneration\":1,"
        "\"operationGeneration\":1,\"state\":\"opening\","
        "\"activeCommandId\":\"command-process-start\","
        "\"activeCommandStatus\":\"pending\",\"dispatchWorkerId\":\"\","
        "\"dispatchEpoch\":0,\"dispatchLeaseExpiresAtUnixMs\":0}]}";
    static const char active[] =
        "{\"schemaVersion\":1,\"resourceCount\":1,\"resources\":[{"
        "\"tenantId\":\"tenant-process\",\"providerId\":\"turbomedia\","
        "\"sessionId\":\"session-process\",\"sessionRevision\":2,"
        "\"ownerNodeId\":\"iris-process\",\"ownerEpoch\":1,"
        "\"ownerLeaseExpiresAtUnixMs\":4102444800000,"
        "\"dialogId\":\"dialog-process\",\"roomId\":\"room-42\","
        "\"callId\":\"call-42\",\"callGeneration\":1,"
        "\"operationGeneration\":2,\"state\":\"active\","
        "\"activeCommandId\":\"command-process-play\","
        "\"activeCommandStatus\":\"pending\",\"dispatchWorkerId\":\"\","
        "\"dispatchEpoch\":0,\"dispatchLeaseExpiresAtUnixMs\":0}]}";
    static const char active_after_play[] =
        "{\"schemaVersion\":1,\"resourceCount\":1,\"resources\":[{"
        "\"tenantId\":\"tenant-process\",\"providerId\":\"turbomedia\","
        "\"sessionId\":\"session-process\",\"sessionRevision\":3,"
        "\"ownerNodeId\":\"iris-process\",\"ownerEpoch\":1,"
        "\"ownerLeaseExpiresAtUnixMs\":4102444800000,"
        "\"dialogId\":\"dialog-process\",\"roomId\":\"room-42\","
        "\"callId\":\"call-42\",\"callGeneration\":1,"
        "\"operationGeneration\":3,\"state\":\"active\","
        "\"activeCommandId\":\"command-process-collect\","
        "\"activeCommandStatus\":\"pending\",\"dispatchWorkerId\":\"\","
        "\"dispatchEpoch\":0,\"dispatchLeaseExpiresAtUnixMs\":0}]}";
    static const char empty[] =
        "{\"schemaVersion\":1,\"resourceCount\":0,\"resources\":[]}";
    test_iris_server_t *server = (test_iris_server_t *)context;
    const char *selected;
    size_t size;
    if (!server || !payload_json || payload_capacity == 0u || !available) {
        return 0;
    }
    *available = atomic_load_explicit(&server->expected_resources_enabled,
                                      memory_order_acquire);
    atomic_fetch_add_explicit(&server->expected_lease_calls, 1,
                              memory_order_acq_rel);
    atomic_store_explicit(&server->expected_lease_valid, 1,
                          memory_order_release);
    if (!*available) {
        selected = empty;
    } else if (atomic_load_explicit(&server->expected_resources_empty,
                                    memory_order_acquire)) {
        selected = empty;
    } else if (atomic_load_explicit(&server->expected_play_completed,
                                    memory_order_acquire)) {
        selected = active_after_play;
    } else if (atomic_load_explicit(&server->expected_dialog_active,
                                    memory_order_acquire)) {
        selected = active;
    } else {
        selected = opening;
    }
    size = strlen(selected);
    if (size >= payload_capacity) return 0;
    memcpy(payload_json, selected, size + 1u);
    if (*available) {
        atomic_fetch_add_explicit(&server->expected_page_calls, 1,
                                  memory_order_acq_rel);
        atomic_store_explicit(&server->expected_page_valid, 1,
                              memory_order_release);
    }
    return 1;
}

static void process_peer_completion(
    void *context, const ProviderCompletionV1_t *completion) {
    test_iris_server_t *server = (test_iris_server_t *)context;
    int is_media;
    int is_media_result;
    int valid;
    if (!server || !completion) return;
    snprintf(server->completion_body, sizeof(server->completion_body),
             "{\"terminalStatus\":\"%s\",\"eventType\":\"%s\","
             "\"commandId\":\"%s\",\"workerId\":\"%s\","
             "\"result\":%s}",
             completion->terminal_status == ProviderTerminalStatus_Succeeded
                 ? "succeeded"
                 : "failed",
             completion->event_type, completion->command_id,
             completion->worker_id, completion->result_json);
    is_media = strncmp(completion->event_type, "provider.conference.",
                       sizeof("provider.conference.") - 1u) != 0 &&
               strncmp(completion->event_type, "provider.connection.",
                       sizeof("provider.connection.") - 1u) != 0;
    is_media_result = strncmp(completion->event_type, "provider.media.",
                              sizeof("provider.media.") - 1u) == 0;
    valid = strcmp(completion->session_id, "session-process") == 0 &&
            completion->terminal_status == ProviderTerminalStatus_Succeeded &&
            completion->result_json && completion->result_json[0];
    if (valid && is_media_result) {
        valid = strstr(completion->result_json,
                       "\"mediaWorkerId\":\"ivr-worker-dispatch\"") != NULL &&
                strstr(completion->result_json,
                       "\"dialogId\":\"dialog-process\"") != NULL;
    }
    if (valid && strcmp(completion->command_id,
                        "command-process-start") == 0) {
        atomic_store_explicit(&server->expected_dialog_active, 1,
                              memory_order_release);
    } else if (valid && strcmp(completion->command_id,
                               "command-process-play") == 0) {
        atomic_store_explicit(&server->expected_play_completed, 1,
                              memory_order_release);
    } else if (valid && strcmp(completion->command_id,
                               "command-live-close") == 0) {
        atomic_store_explicit(&server->expected_dialog_active, 0,
                              memory_order_release);
    }
    if (is_media) {
        atomic_store_explicit(&server->completion_valid, valid,
                              memory_order_release);
        atomic_fetch_add_explicit(&server->completion_calls, 1,
                                  memory_order_release);
    } else {
        atomic_fetch_add_explicit(&server->room_completion_calls, 1,
                                  memory_order_release);
    }
}

static void process_peer_event(void *context, const ProviderEventV1_t *event) {
    test_iris_server_t *server = (test_iris_server_t *)context;
    int valid;
    unsigned long long attempt_generation = 0;
    int has_attempt_generation;
    int common;
    int playback;
    int asr;
    int dtmf;
    int rtc_disconnected;
    int rtc_reconnected;
    int rtc_retry_exhausted;
    if (!server || !event) return;
    snprintf(server->event_body, sizeof(server->event_body),
             "{\"eventId\":\"%s\",\"type\":\"%s\","
             "\"source\":\"turbomedia\",\"correlationId\":\"%s\","
             "\"data\":%s}",
             event->event_id, event->event_type, event->correlation_id,
             event->payload_json);
    valid = strcmp(event->session_id, "session-process") == 0;
    has_attempt_generation = extract_unsigned_json_field(
        server->event_body, "\"attempt_generation\":", &attempt_generation);
    common = valid && strstr(server->event_body, "\"eventId\":\"media-") &&
             strstr(server->event_body, "\"source\":\"turbomedia\"") &&
             strstr(server->event_body,
                    "\"correlationId\":\"dialog-process\"") &&
             strstr(server->event_body, "\"roomId\":\"room-42\"") &&
             strstr(server->event_body, "\"callId\":\"call-42\"");
    playback = common &&
               strstr(server->event_body,
                      "\"type\":\"playback.finished\"");
    asr = common && strstr(server->event_body, "\"type\":\"asr.final\"") &&
          strstr(server->event_body, "\"inputId\":\"input-process\"") &&
          strstr(server->event_body, "\"inputValue\":\"fixture-asr\"");
    dtmf = common &&
           strstr(server->event_body, "\"type\":\"dtmf.final\"") &&
           strstr(server->event_body, "\"inputId\":\"input-process\"") &&
           strstr(server->event_body, "\"inputValue\":\"5\"");
    rtc_disconnected = common &&
        strstr(server->event_body, "\"type\":\"rtc.disconnected\"") &&
        has_attempt_generation && attempt_generation > 0u;
    rtc_reconnected = common &&
        strstr(server->event_body, "\"type\":\"rtc.reconnected\"") &&
        has_attempt_generation && attempt_generation > 0u;
    rtc_retry_exhausted = common &&
        strstr(server->event_body, "\"type\":\"rtc.retry_exhausted\"") &&
        has_attempt_generation && attempt_generation > 0u;
    if (playback) atomic_store(&server->playback_event_valid, 1);
    if (asr) atomic_store(&server->asr_event_valid, 1);
    if (dtmf) atomic_store(&server->dtmf_event_valid, 1);
    if (rtc_disconnected) {
        atomic_store(&server->rtc_disconnected_generation,
                     attempt_generation);
        atomic_fetch_add(&server->rtc_disconnected_calls, 1);
    }
    if (rtc_reconnected) {
        atomic_store(&server->rtc_reconnected_generation,
                     attempt_generation);
        atomic_fetch_add(&server->rtc_reconnected_calls, 1);
    }
    if (rtc_retry_exhausted) {
        atomic_fetch_add(&server->rtc_retry_exhausted_calls, 1);
    }
    if (rtc_disconnected || rtc_reconnected || rtc_retry_exhausted) {
        atomic_store(&server->rtc_event_valid, 1);
    }
    valid = playback || asr || dtmf || rtc_disconnected || rtc_reconnected ||
            rtc_retry_exhausted;
    atomic_store_explicit(&server->event_valid, valid, memory_order_release);
    atomic_fetch_add_explicit(&server->event_calls, 1, memory_order_release);
}

static int facade_request(chttp_method method, const char *path,
                          const char *token, const char *idempotency_key,
                          const char *body, char *response_body,
                          size_t response_capacity) {
    char authorization[256];
    char idempotency[256];
    chttp_header headers[3];
    chttp_options options = {0};
    chttp_response response = {0};
    chttp_error error = {0};
    size_t header_count = 0u;
    int request_status;
    int status = 0;
    int written;

    if (!path || !response_body || response_capacity == 0u) {
        return 0;
    }
    response_body[0] = '\0';
    if (strcmp(path, "/provider/v1/commands") == 0) {
        test_iris_control_receipt_t receipt;
        const char *disposition;
        int provider_status;
        int is_room;
        int completion_before;
        if (method != CHTTP_METHOD_POST || !g_iris_control_peer ||
            !idempotency_key ||
            !body) {
            return 400;
        }
        /* The production transport authenticates the CHTTP H1 WebSocket peer identity.
           token remains only in this legacy-shaped test helper signature. */
        (void)token;
        is_room = strstr(body, "\"capability\":\"room\"") != NULL;
        completion_before = atomic_load_explicit(
            is_room ? &g_iris.room_completion_calls : &g_iris.completion_calls,
            memory_order_acquire);
        {
            int control_ws_status = test_iris_control_peer_send_command(
                g_iris_control_peer, idempotency_key, body,
                TEST_HTTP_IO_TIMEOUT_MS, &receipt);
            if (control_ws_status != SALTS_OK) {
                fprintf(stderr,
                        "process Iris CHTTP H1 WebSocket command failed status=%d id=%s\n",
                        control_ws_status, idempotency_key);
                print_file_on_failure("room_service_control_timeout", g_rs_out);
                print_file_on_failure("worker_control_timeout", g_wk_out);
                return 0;
            }
        }
        switch (receipt.status_code) {
            case 0: /* IRIS_MEDIA_BRIDGE_ACCEPTED */
                provider_status = 202;
                disposition = "accepted";
                break;
            case 1: /* IRIS_MEDIA_BRIDGE_DUPLICATE */
                provider_status = 425;
                disposition = "in_progress";
                break;
            case 2: /* IRIS_MEDIA_BRIDGE_TERMINAL */
                provider_status = 200;
                disposition = "terminal";
                break;
            case 3: /* IRIS_MEDIA_BRIDGE_TERMINAL_REPLAY */
                provider_status = 200;
                disposition = "terminal";
                break;
            case 4: /* IRIS_MEDIA_BRIDGE_INVALID */
                provider_status = 400;
                disposition = "rejected";
                break;
            case 5: /* IRIS_MEDIA_BRIDGE_CONFLICT */
                provider_status = 409;
                disposition = "conflict";
                break;
            case 6: /* IRIS_MEDIA_BRIDGE_EXPIRED */
                provider_status = 410;
                disposition = "expired";
                break;
            case 7: /* IRIS_MEDIA_BRIDGE_FULL */
                provider_status = 429;
                disposition = "full";
                break;
            case 8: /* IRIS_MEDIA_BRIDGE_UNAVAILABLE */
                provider_status = 503;
                disposition = "not_ready";
                break;
            default:
                provider_status = 500;
                disposition = "failed";
                break;
        }
        if (receipt.status_code == 2 ||
            (is_room && receipt.status_code == 3)) {
            atomic_int *completion_counter =
                is_room ? &g_iris.room_completion_calls
                        : &g_iris.completion_calls;
            for (int attempt = 0; attempt < TEST_IRIS_DELIVERY_ATTEMPTS;
                 ++attempt) {
                if (atomic_load_explicit(completion_counter,
                                         memory_order_acquire) >
                    completion_before) {
                    break;
                }
                proc_sleep(TEST_IRIS_DELIVERY_POLL_MS);
            }
        }
        if ((receipt.status_code == 2 ||
             (is_room && receipt.status_code == 3)) &&
            atomic_load_explicit(
                is_room ? &g_iris.room_completion_calls
                        : &g_iris.completion_calls,
                memory_order_acquire) > completion_before) {
            snprintf(response_body, response_capacity,
                     "{\"disposition\":\"%s\",\"duplicate\":%s,"
                     "\"statusCode\":%d,\"workerId\":\"%s\","
                     "\"dispatchEpoch\":\"%s\",\"completion\":%s}",
                     disposition, receipt.status_code == 3 ? "true" : "false",
                     receipt.status_code, receipt.worker_id,
                     receipt.dispatch_epoch, g_iris.completion_body);
        } else {
            snprintf(response_body, response_capacity,
                     "{\"disposition\":\"%s\",\"duplicate\":%s,"
                     "\"statusCode\":%d,\"workerId\":\"%s\","
                     "\"dispatchEpoch\":\"%s\",\"errorCode\":\"%s\","
                     "\"errorMessage\":\"%s\"}",
                     disposition, receipt.status_code == 3 ? "true" : "false",
                     receipt.status_code, receipt.worker_id,
                     receipt.dispatch_epoch, receipt.error_code,
                     receipt.error_message);
        }
        return provider_status;
    }
    if (!g_http_client_initialized) return 0;
    if (body) {
        headers[header_count++] =
            (chttp_header){"Content-Type", "application/json"};
    }
    if (token) {
        written = snprintf(authorization, sizeof(authorization),
                           "Bearer %s", token);
        if (written <= 0 || (size_t)written >= sizeof(authorization)) return 0;
        headers[header_count++] =
            (chttp_header){"Authorization", authorization};
    }
    if (idempotency_key) {
        written = snprintf(idempotency, sizeof(idempotency),
                           "%s", idempotency_key);
        if (written <= 0 || (size_t)written >= sizeof(idempotency)) return 0;
        headers[header_count++] =
            (chttp_header){"Idempotency-Key", idempotency};
    }
    options.connection_uri = "tcp://127.0.0.1:19091";
    options.authority = "127.0.0.1:19091";
    options.target = path;
    options.headers = header_count ? headers : NULL;
    options.header_count = header_count;
    options.body = body;
    options.body_size = body ? strlen(body) : 0u;
    options.timeout_ms = TEST_HTTP_IO_TIMEOUT_MS;
    if (method == CHTTP_METHOD_GET) {
        request_status = chttp_get(&g_http_client, &options, &response,
                                   &error);
    } else if (method == CHTTP_METHOD_POST) {
        request_status = chttp_post(&g_http_client, &options, &response,
                                    &error);
    } else {
        request_status = SALTS_EINVAL;
    }
    if (request_status == SALTS_OK) {
        size_t copy_size = response.body_size;
        status = (int)response.status_code;
        if (copy_size >= response_capacity) copy_size = response_capacity - 1u;
        if (copy_size > 0u && response.body) {
            memcpy(response_body, response.body, copy_size);
        }
        response_body[copy_size] = '\0';
    }
    chttp_response_destroy(&response);
    return status;
}

void setUp(void) {
    chttp_client_config http_config;
    int http_status;
    test_iris_control_peer_config_t iris_control_config;
    memset(&g_room_service, 0, sizeof(g_room_service));
    memset(&g_worker, 0, sizeof(g_worker));
    memset(&g_sfu, 0, sizeof(g_sfu));
    memset(&g_iris, 0, sizeof(g_iris));
    memset(&g_iris_proxy, 0, sizeof(g_iris_proxy));
    memset(&g_control_ws_proxy, 0, sizeof(g_control_ws_proxy));
    g_router_bind_port = TEST_ROUTER_PORT;
    g_caller_transport = NULL;
    memset(&g_http_client, 0, sizeof(g_http_client));
    g_http_client_initialized = 0;
    g_iris_control_peer = NULL;
    check_equal((int)(test_iris_server_start(&g_iris)), (int)(0));
    check_equal((int)(test_iris_proxy_start(&g_iris_proxy, TEST_IRIS_TLS_PORT,
                                 TEST_IRIS_BACKEND_TLS_PORT)), (int)(0));
    http_config = test_http_client_config();
    http_status = chttp_client_init(&g_http_client, &http_config);
    check_equal((int)http_status, (int)SALTS_OK);
    g_http_client_initialized = http_status == SALTS_OK;
    DataBindError err = DATA_BIND_ERROR_INIT;
    check_equal(TurboMediaIvrV1_codec_create(&g_codec, &err), DATA_BIND_OK);
    memset(&iris_control_config, 0, sizeof(iris_control_config));
    iris_control_config.port = TEST_IRIS_CONTROL_PORT;
    iris_control_config.query = process_peer_query;
    iris_control_config.completion = process_peer_completion;
    iris_control_config.event = process_peer_event;
    iris_control_config.context = &g_iris;
    check_equal(test_iris_control_peer_start(&iris_control_config,
                                            &g_iris_control_peer),
                SALTS_OK);

    /* temp config for room_service with control WebSocket and durable Iris outbox enabled */
    snprintf(g_cfg_path, sizeof(g_cfg_path), "%s/rs_dispatch_%d.toml",
             TEST_BIN_DIR, ++g_seq);
    snprintf(g_store_path, sizeof(g_store_path), "%s/rs_dispatch_%d.yaml",
             TEST_BIN_DIR, g_seq);
    snprintf(g_database_path, sizeof(g_database_path),
             "%s/rs_dispatch_%d.sqlite3", TEST_BIN_DIR, g_seq);
    snprintf(g_ledger_database_path, sizeof(g_ledger_database_path),
             "%s/rs_dispatch_ledger_%d.sqlite3", TEST_BIN_DIR, g_seq);
    normalize_config_path(g_cfg_path);
    normalize_config_path(g_store_path);
    normalize_config_path(g_database_path);
    normalize_config_path(g_ledger_database_path);
    FILE *store = fopen(g_store_path, "wb");
    check_not_null(store);
    check_true(fprintf(store,
                "version: 1\n"
                "channels:\n"
                "  iris.media_events:\n"
                "    kind: record_store\n"
                "    config:\n"
                "      backend: sqlite\n"
                "      database_path: '%s'\n"
                "      namespace_name: iris.media_events\n"
                "      busy_timeout_ms: 1000\n"
                "      max_records: 32\n"
                "      max_bytes: 1048576\n"
                "      max_item_bytes: 32768\n"
                "      max_key_size: 128\n"
                "      max_value_size: 16384\n"
                "      max_batch_size: 8\n"
                "  iris.provider_commands:\n"
                "    kind: record_store\n"
                "    config:\n"
                "      backend: sqlite\n"
                "      database_path: '%s'\n"
                "      namespace_name: iris.provider_commands\n"
                "      busy_timeout_ms: 1000\n"
                "      max_records: 32\n"
                "      max_bytes: 1048576\n"
                "      max_item_bytes: 32768\n"
                "      max_key_size: 128\n"
                "      max_value_size: 16384\n"
                "      max_batch_size: 8\n"
                "adapters: {}\n",
                g_database_path, g_ledger_database_path) > 0);
    fclose(store);
    check_true(write_room_service_config(0));

    snprintf(g_rs_out, sizeof(g_rs_out), "%s/rs_dispatch_%d.out", TEST_BIN_DIR,
             g_seq);
    snprintf(g_wk_out, sizeof(g_wk_out), "%s/wk_dispatch_%d.out", TEST_BIN_DIR,
             g_seq);
    snprintf(g_sfu_cfg_path, sizeof(g_sfu_cfg_path), "%s/sfu_dispatch_%d.toml",
             TEST_BIN_DIR, g_seq);
    snprintf(g_sfu_out, sizeof(g_sfu_out), "%s/sfu_dispatch_%d.out",
             TEST_BIN_DIR, g_seq);
    snprintf(g_orphan_resume_path, sizeof(g_orphan_resume_path),
             "%s/orphan_resume_%d.flag", TEST_BIN_DIR, g_seq);
    normalize_config_path(g_sfu_cfg_path);
    normalize_config_path(g_orphan_resume_path);
    remove(g_orphan_resume_path);

    check_true(spawn_room_service());
    proc_sleep(2000); /* let room_service bind HTTP + control WebSocket */

    {
        static const char readiness_probe[] =
            "{\"data\":{\"capability\":\"ivr\"}}";
        char response[TEST_HTTP_RESPONSE_CAPACITY];
        int status = facade_request(
            CHTTP_METHOD_POST, "/provider/v1/commands", TEST_PROVIDER_TOKEN,
            "reconcile-readiness-probe", readiness_probe, response,
            sizeof(response));
        if (status == 0) {
#ifdef _WIN32
            DWORD exit_code = 0;
            if (g_room_service.handle &&
                GetExitCodeProcess(g_room_service.handle, &exit_code)) {
                fprintf(stderr, "room_service process exit_code=%lu\n",
                        (unsigned long)exit_code);
            }
#endif
            print_file_on_failure("room_service_startup", g_rs_out);
        }
        check_equal((int)(status), (int)(503));
        if (!strstr(response, "MEDIA_PROVIDER_RECONCILING")) {
            fprintf(stderr, "readiness probe response=%s\n", response);
        }
        check_not_null(strstr(response, "MEDIA_PROVIDER_RECONCILING"));
        check_true(atomic_load_explicit(&g_iris.expected_lease_calls,
                                 memory_order_acquire) > 0);
        check_equal((int)(atomic_load_explicit(&g_iris.expected_lease_valid,
                                    memory_order_acquire)), (int)(1));

        atomic_store_explicit(&g_iris.expected_resources_enabled, 1,
                              memory_order_release);
        for (int attempt = 0; attempt < TEST_IRIS_DELIVERY_ATTEMPTS; ++attempt) {
            status = facade_request(
                CHTTP_METHOD_POST, "/provider/v1/commands", TEST_PROVIDER_TOKEN,
                "reconcile-readiness-probe", readiness_probe, response,
                sizeof(response));
            if (status == 400) break;
            proc_sleep(TEST_IRIS_DELIVERY_POLL_MS);
        }
        check_equal((int)(status), (int)(400));
        check_true(atomic_load_explicit(&g_iris.expected_page_calls,
                                 memory_order_acquire) > 0);
        check_equal((int)(atomic_load_explicit(&g_iris.expected_page_valid,
                                    memory_order_acquire)), (int)(1));
    }

    check_true(provision_room_service_room());

    check_true(spawn_probe_worker("ivr-worker-dispatch", "media-runtime"));
    {
        static const char readiness_probe[] =
            "{\"data\":{\"capability\":\"ivr\"}}";
        char response[TEST_HTTP_RESPONSE_CAPACITY];
        int status = 0;
        for (int attempt = 0; attempt < TEST_IRIS_DELIVERY_ATTEMPTS; ++attempt) {
            status = facade_request(
                CHTTP_METHOD_POST, "/provider/v1/commands", TEST_PROVIDER_TOKEN,
                "worker-reconcile-readiness-probe", readiness_probe, response,
                sizeof(response));
            if (status == 400) break;
            proc_sleep(TEST_IRIS_DELIVERY_POLL_MS);
        }
        check_equal((int)(status), (int)(400));
    }
}

void tearDown(void) {
    char sqlite_aux_path[sizeof(g_database_path) + sizeof("-wal")];
    if (g_codec) {
        data_bind_free(g_codec);
        g_codec = NULL;
    }
    kill_child(&g_worker);
    kill_child(&g_room_service);
    test_iris_control_peer_stop(g_iris_control_peer);
    g_iris_control_peer = NULL;
    if (g_caller_transport) {
        ivr_whip_transport_destroy(g_caller_transport);
        g_caller_transport = NULL;
    }
    kill_child(&g_sfu);
    test_iris_proxy_stop(&g_control_ws_proxy);
    if (g_http_client_initialized) {
        (void)chttp_client_destroy(&g_http_client,
                                   TEST_HTTP_IO_TIMEOUT_MS);
        g_http_client_initialized = 0;
    }
    test_iris_proxy_stop(&g_iris_proxy);
    test_iris_server_stop(&g_iris);
    remove(g_cfg_path);
    remove(g_store_path);
    snprintf(sqlite_aux_path, sizeof(sqlite_aux_path), "%s-wal",
             g_database_path);
    remove(sqlite_aux_path);
    snprintf(sqlite_aux_path, sizeof(sqlite_aux_path), "%s-shm",
             g_database_path);
    remove(sqlite_aux_path);
    remove(g_database_path);
    snprintf(sqlite_aux_path, sizeof(sqlite_aux_path), "%s-wal",
             g_ledger_database_path);
    remove(sqlite_aux_path);
    snprintf(sqlite_aux_path, sizeof(sqlite_aux_path), "%s-shm",
             g_ledger_database_path);
    remove(sqlite_aux_path);
    remove(g_ledger_database_path);
    remove(g_rs_out);
    remove(g_wk_out);
    remove(g_sfu_cfg_path);
    remove(g_sfu_out);
    remove(g_orphan_resume_path);
    (void)set_environment("PROBE_ORPHAN_RESUME_PATH", NULL);
    g_router_bind_port = TEST_ROUTER_PORT;
}

static int wait_file_contains(const char *path, const char *needle,
                              int attempts, unsigned int delay_ms) {
    for (int i = 0; i < attempts; ++i) {
        if (file_contains(path, needle)) {
            return 1;
        }
        proc_sleep(delay_ms);
    }
    return 0;
}

static int create_resume_marker(void) {
    FILE *marker = fopen(g_orphan_resume_path, "wb");
    if (!marker) return 0;
    if (fputs("resume\n", marker) == EOF) {
        fclose(marker);
        return 0;
    }
    return fclose(marker) == 0;
}

static int wait_iris_deliveries(int completion_target, int event_target) {
    for (int i = 0; i < TEST_IRIS_DELIVERY_ATTEMPTS; ++i) {
        if (atomic_load_explicit(&g_iris.completion_calls,
                                 memory_order_acquire) >= completion_target &&
            atomic_load_explicit(&g_iris.event_calls,
                                 memory_order_acquire) >= event_target) {
            return 1;
        }
        proc_sleep(TEST_IRIS_DELIVERY_POLL_MS);
    }
    return 0;
}

static int wait_atomic_int_greater_than(const atomic_int *value,
                                        int previous_value) {
    if (!value) return 0;
    for (int attempt = 0; attempt < TEST_IRIS_DELIVERY_ATTEMPTS; ++attempt) {
        if (atomic_load_explicit(value, memory_order_acquire) >
            previous_value) {
            return 1;
        }
        proc_sleep(TEST_IRIS_DELIVERY_POLL_MS);
    }
    return 0;
}

static int wait_atomic_int_at_least(const atomic_int *value, int target,
                                    int attempts, unsigned int delay_ms) {
    if (!value || target < 0 || attempts <= 0) {
        return 0;
    }
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (atomic_load_explicit(value, memory_order_acquire) >= target) {
            return 1;
        }
        proc_sleep(delay_ms);
    }
    return 0;
}

static int spawn_real_worker(void) {
    typedef struct {
        const char *name;
        const char *value;
        char *saved;
    } environment_override_t;
    char executable[1024];
    char router_port[16];
    char health_port[16];
    char sfu_base_url[64];
    char speech_url[64];
    environment_override_t overrides[] = {
        {"IVR_CONTROL_USE_TLS", "1", NULL},
        {"IVR_CONTROL_CA_FILE", ROOM_SERVICE_TEST_TLS_CERT_PATH, NULL},
        {"IVR_CONTROL_CERT_FILE", ROOM_SERVICE_TEST_TLS_CERT_PATH, NULL},
        {"IVR_CONTROL_KEY_FILE", ROOM_SERVICE_TEST_TLS_KEY_PATH, NULL},
        {"IVR_CONTROL_SERVER_NAME", "localhost", NULL},
        {"IVR_OPENAI_BASE_URL", speech_url, NULL},
        {"OPENAI_API_KEY", TEST_SPEECH_TOKEN, NULL},
        {"IVR_OPENAI_CA_FILE", ROOM_SERVICE_TEST_TLS_CERT_PATH, NULL},
        {"IVR_OPENAI_SERVER_NAME", "localhost", NULL},
        {"IVR_OPENAI_TIMEOUT_MS", "5000", NULL},
        {"IVR_SFU_BASE_URL", sfu_base_url, NULL},
        {"IVR_SFU_MEDIA_TOKEN", TEST_SFU_MEDIA_TOKEN, NULL},
        {"IVR_SFU_ALLOW_PLAINTEXT_LOOPBACK", "1", NULL},
        {"IVR_SFU_ALLOW_LOOPBACK", "1", NULL},
        {"IVR_MEDIA_INPUT_INACTIVITY_TIMEOUT_MS", "120000", NULL}};
    const size_t override_count = sizeof(overrides) / sizeof(overrides[0]);
    size_t prepared_count = 0u;
    const char *args[] = {
        "--worker-id", "ivr-worker-dispatch", "--router-host", "127.0.0.1",
        "--router-port", router_port, "--health-host", "127.0.0.1",
        "--health-port", health_port, "--max-sessions", "4",
        "--heartbeat-ms", "500", "--lease-ms", "2000", NULL};
    int result = 0;
    int values_valid =
        snprintf(router_port, sizeof(router_port), "%d", TEST_ROUTER_PORT) > 0 &&
        snprintf(health_port, sizeof(health_port), "%d",
                 TEST_REAL_WORKER_HEALTH_PORT) > 0 &&
        snprintf(sfu_base_url, sizeof(sfu_base_url),
                 "http://127.0.0.1:%d", TEST_SFU_PORT) > 0;
    values_valid =
        values_valid &&
        snprintf(speech_url, sizeof(speech_url), "https://127.0.0.1:%d",
                 TEST_IRIS_BACKEND_TLS_PORT) > 0;
    int executable_length = snprintf(executable, sizeof(executable), "%s/%s",
                                     TEST_BIN_DIR, IVR_WORKER_BIN);

    if (!values_valid || executable_length <= 0 ||
        (size_t)executable_length >= sizeof(executable)) {
        return 0;
    }
    for (size_t index = 0; index < override_count; ++index) {
        const char *existing = getenv(overrides[index].name);
        overrides[index].saved = copy_environment(overrides[index].name);
        if (existing && !overrides[index].saved) {
            goto restore;
        }
        prepared_count++;
    }
    for (size_t index = 0; index < override_count; ++index) {
        if (set_environment(overrides[index].name, overrides[index].value) !=
            0) {
            goto restore;
        }
    }
    kill_child(&g_worker);
    result = spawn_with_stdout(executable, args, g_wk_out, &g_worker) == 0;

restore:
    for (size_t index = 0; index < prepared_count; ++index) {
        (void)set_environment(overrides[index].name, overrides[index].saved);
        free(overrides[index].saved);
    }
    return result;
}

static int spawn_probe_worker(const char *worker_id, const char *mode) {
    char probe_bin[1024];
    const char *args[] = {worker_id, mode, NULL};
    kill_child(&g_worker);
    snprintf(probe_bin, sizeof(probe_bin), "%s/%s", TEST_BIN_DIR,
             IVR_DISPATCH_PROBE_BIN);
    if (spawn_with_stdout(probe_bin, args, g_wk_out, &g_worker) != 0) {
        fprintf(stderr, "could not spawn dispatch worker probe: %s\n",
                probe_bin);
        return 0;
    }
    if (!wait_file_contains(g_wk_out, "probe sync acknowledged", 80, 50)) {
        print_file_on_failure("room_service", g_rs_out);
        print_file_on_failure("dispatch_worker_probe", g_wk_out);
        return 0;
    }
    return 1;
}

static int wait_provider_ready(void) {
    static const char readiness_probe[] =
        "{\"data\":{\"capability\":\"ivr\"}}";
    char response[TEST_HTTP_RESPONSE_CAPACITY];
    int status = 0;
    for (int attempt = 0; attempt < TEST_IRIS_DELIVERY_ATTEMPTS; ++attempt) {
        status = facade_request(
            CHTTP_METHOD_POST, "/provider/v1/commands", TEST_PROVIDER_TOKEN,
            "fault-readiness-probe", readiness_probe, response,
            sizeof(response));
        if (status == 400) return 1;
        proc_sleep(TEST_IRIS_DELIVERY_POLL_MS);
    }
    return 0;
}

static int wait_rebound_ready_with_attempts(char *response,
                                            size_t response_capacity,
                                            int attempts) {
    int status = 0;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        status = facade_request(CHTTP_METHOD_GET, "/metrics", NULL, NULL, NULL,
                                response, response_capacity);
        if (status == 200 &&
            strstr(response,
                   "turbo_room_service_iris_reconcile_state 5\n") &&
            strstr(response,
                   "turbo_room_service_iris_reconcile_accepting_commands "
                   "1\n") &&
            strstr(response,
                   "turbo_room_service_iris_reconcile_rebound_total 1\n")) {
            return 1;
        }
        proc_sleep(TEST_IRIS_DELIVERY_POLL_MS);
    }
    return 0;
}

static int wait_rebound_ready(char *response, size_t response_capacity) {
    return wait_rebound_ready_with_attempts(
        response, response_capacity, TEST_IRIS_DELIVERY_ATTEMPTS);
}

static int wait_reconcile_ready(char *response, size_t response_capacity) {
    for (int attempt = 0; attempt < TEST_IRIS_DELIVERY_ATTEMPTS; ++attempt) {
        int status = facade_request(CHTTP_METHOD_GET, "/metrics", NULL, NULL, NULL,
                                    response, response_capacity);
        if (status == 200 &&
            strstr(response,
                   "turbo_room_service_iris_reconcile_state 5\n") &&
            strstr(response,
                   "turbo_room_service_iris_reconcile_accepting_commands "
                   "1\n")) {
            return 1;
        }
        proc_sleep(TEST_IRIS_DELIVERY_POLL_MS);
    }
    return 0;
}

static int submit_fault_dialog_start(const char *command_id,
                                     const char *dialog_id, char *response,
                                     size_t response_capacity) {
    char body[2048];
    int written = snprintf(
        body, sizeof(body),
        "{"
        "\"schemaVersion\":2,\"commandId\":\"%s\","
        "\"tenantId\":\"tenant-process\","
        "\"sessionId\":\"session-process\","
        "\"type\":\"dialog.start\",\"provider\":\"turbomedia\","
        "\"correlationId\":\"%s\",\"causationId\":\"state-fault\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\",\"dispatchEpoch\":1,"
        "\"data\":{\"capability\":\"ivr\",\"dialogId\":\"%s\","
        "\"roomId\":\"room-42\",\"callId\":\"call-42\","
        "\"callGeneration\":1,\"operationGeneration\":1}}",
        command_id, dialog_id, dialog_id);
    if (written <= 0 || (size_t)written >= sizeof(body)) return 0;
    return facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                          TEST_PROVIDER_TOKEN, command_id, body, response,
                          response_capacity);
}

void test_worker_exit_before_dispatch_rejects_without_reservation(void) {
    char response[TEST_HTTP_RESPONSE_CAPACITY];
    int status = 0;
    check_true(file_contains(g_wk_out, "probe sync acknowledged"));
    kill_child(&g_worker);
    /* The server disconnect callback is copied onto the bridge owner queue.
       Let that bounded owner-side event invalidate the route before proving
       that no command reservation can be created. */
    proc_sleep(500u);
    for (int attempt = 0; attempt < TEST_IRIS_DELIVERY_ATTEMPTS; ++attempt) {
        status = submit_fault_dialog_start(
            "command-fault-before", "dialog-fault-before", response,
            sizeof(response));
        if (status == 503) break;
        proc_sleep(TEST_IRIS_DELIVERY_POLL_MS);
    }
    if (status != 503) {
        fprintf(stderr, "worker-exit-before status=%d response=%s\n", status,
                response);
        print_file_on_failure("worker_exit_before_room_service", g_rs_out);
        print_file_on_failure("worker_exit_before_worker", g_wk_out);
    }
    check_equal((int)(status), (int)(503));
    check_true(strstr(response, "MEDIA_ROUTE_UNAVAILABLE") != NULL ||
                     strstr(response, "MEDIA_PROVIDER_RECONCILING") != NULL);
}

void test_worker_exit_during_dialog_open_emits_loss_and_replacement_recovers(
    void) {
    char response[TEST_HTTP_RESPONSE_CAPACITY];
    kill_child(&g_worker);
    check_true(spawn_probe_worker("ivr-worker-creating", "drop-before-ack"));
    check_true(wait_provider_ready());
    check_equal((int)(submit_fault_dialog_start(
                 "command-fault-creating", "dialog-fault-creating", response,
                 sizeof(response))), (int)(202));
    if (!wait_file_contains(g_wk_out,
                            "probe media received command-fault-creating",
                            100, 50)) {
        print_file_on_failure("room_service", g_rs_out);
        print_file_on_failure("creating_worker", g_wk_out);
        check(0, "%s", ("creating worker did not receive dialog.open"));
    }
    check_true(wait_iris_deliveries(0, 1));

    check_true(spawn_probe_worker("ivr-worker-replacement", "media-runtime"));
    check_true(wait_provider_ready());
    check_equal((int)(submit_fault_dialog_start(
                 "command-fault-replacement", "dialog-fault-replacement",
                 response, sizeof(response))), (int)(202));
    check_true(wait_iris_deliveries(1, 1));
}

void test_worker_exit_after_result_preserves_room_membership(void) {
    char response[8192];

    kill_child(&g_worker);
    check_true(spawn_probe_worker("ivr-worker-acked", "ack-exit"));
    check_true(wait_provider_ready());
    check_equal((int)(submit_fault_dialog_start(
                 "command-fault-acked", "dialog-fault-acked", response,
                 sizeof(response))), (int)(202));
    if (!wait_file_contains(g_wk_out,
                            "probe media completed command-fault-acked", 100,
                            50)) {
        print_file_on_failure("room_service", g_rs_out);
        print_file_on_failure("acked_worker", g_wk_out);
        check(0, "%s", ("worker did not return dialog.open result"));
    }
    check_true(wait_iris_deliveries(1, 1));
    memset(response, 0, sizeof(response));
    check_equal((int)(http_post_command(
               "127.0.0.1", TEST_HTTP_PORT, TEST_CTRL_TOKEN,
               "{\"type\":\"get_room_state\",\"room_id\":\"room-42\"}",
               response, sizeof(response))), (int)(0));
    check_not_null(strstr(response, "call-42"));
}

void test_room_service_restart_rebinds_active_worker_dialog(void) {
    static const char readiness_probe[] =
        "{\"data\":{\"capability\":\"ivr\"}}";
    char response[TEST_HTTP_RESPONSE_CAPACITY];
    int expected_lease_calls_before_restart;
    int expected_page_calls_before_restart;
    int status = 0;

    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-start",
                            TEST_PROCESS_START_COMMAND, response,
                            sizeof(response))), (int)(202));
    check_true(wait_iris_deliveries(1, 0));
    check_true(wait_file_contains(
        g_wk_out, "media runtime completed command-process-start",
        TEST_PROBE_LOG_ATTEMPTS, TEST_PROBE_LOG_POLL_MS));

    expected_lease_calls_before_restart = atomic_load_explicit(
        &g_iris.expected_lease_calls, memory_order_acquire);
    expected_page_calls_before_restart = atomic_load_explicit(
        &g_iris.expected_page_calls, memory_order_acquire);
    atomic_store_explicit(&g_iris.expected_resources_enabled, 0,
                          memory_order_release);
    kill_child(&g_room_service);
    check_true(spawn_room_service());

    for (int attempt = 0; attempt < TEST_IRIS_DELIVERY_ATTEMPTS; ++attempt) {
        status = facade_request(
            CHTTP_METHOD_POST, "/provider/v1/commands", TEST_PROVIDER_TOKEN,
            "restart-reconcile-readiness-probe", readiness_probe, response,
            sizeof(response));
        if (status == 503 &&
            strstr(response, "MEDIA_PROVIDER_RECONCILING")) {
            break;
        }
        proc_sleep(TEST_IRIS_DELIVERY_POLL_MS);
    }
    if (status != 503) {
        print_file_on_failure("restarted_room_service", g_rs_out);
    }
    check_equal((int)(status), (int)(503));
    check_not_null(strstr(response, "MEDIA_PROVIDER_RECONCILING"));
    check_true(wait_atomic_int_greater_than(
        &g_iris.expected_lease_calls, expected_lease_calls_before_restart));

    atomic_store_explicit(&g_iris.expected_resources_enabled, 1,
                          memory_order_release);
    if (!wait_rebound_ready(response, sizeof(response))) {
        print_file_on_failure("restarted_room_service", g_rs_out);
        print_file_on_failure("active_worker", g_wk_out);
        check(0, "%s", ("active dialog did not reach rebound READY state"));
    }
    check_true(wait_atomic_int_greater_than(
        &g_iris.expected_page_calls, expected_page_calls_before_restart));
    check_not_null(strstr(
        response, "turbo_room_service_iris_reconcile_inventory_pages_total "));

    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-play",
                            TEST_PROCESS_PLAY_COMMAND, response,
                            sizeof(response))), (int)(202));
    check_true(wait_iris_deliveries(2, 1));
    check_true(wait_file_contains(
        g_wk_out, "media runtime completed command-process-play",
        TEST_PROBE_LOG_ATTEMPTS, TEST_PROBE_LOG_POLL_MS));
    if (!wait_atomic_int_at_least(
            &g_iris.playback_event_valid, 1, TEST_IRIS_DELIVERY_ATTEMPTS,
            TEST_IRIS_DELIVERY_POLL_MS)) {
        print_file_on_failure("playback_event_worker", g_wk_out);
        print_file_on_failure("playback_event_room_service", g_rs_out);
        check(0, "%s", ("playback ProviderEvent was not delivered"));
    }
    if (!atomic_load_explicit(&g_iris.playback_event_valid,
                              memory_order_acquire)) {
        fprintf(stderr, "invalid playback ProviderEvent=%s\n",
                g_iris.event_body);
        print_file_on_failure("playback_event_worker", g_wk_out);
        print_file_on_failure("playback_event_room_service", g_rs_out);
    }
    check_equal((int)(atomic_load_explicit(&g_iris.completion_valid,
                                memory_order_acquire)), (int)(1));
    if (!atomic_load_explicit(&g_iris.playback_event_valid,
                              memory_order_acquire)) {
        fprintf(stderr, "invalid playback event:\n%s\n", g_iris.event_body);
        check(0, "%s", ("playback event did not satisfy provider contract"));
    }
}

static void run_orphan_close_crash_window(const char *mode,
                                          const char *phase_needle,
                                          int expect_fenced_retry) {
    char response[TEST_HTTP_RESPONSE_CAPACITY];
    int completion_target;
    int start_status = 0;

    kill_child(&g_worker);
    remove(g_orphan_resume_path);
    check_equal((int)(set_environment("PROBE_ORPHAN_RESUME_PATH",
                           g_orphan_resume_path)), (int)(0));
    check_true(spawn_probe_worker("ivr-worker-dispatch", mode));
    check_true(wait_provider_ready());

    completion_target =
        atomic_load_explicit(&g_iris.completion_calls,
                             memory_order_acquire) + 1;
    for (int attempt = 0; attempt < TEST_IRIS_DELIVERY_ATTEMPTS; ++attempt) {
        start_status = facade_request(
            CHTTP_METHOD_POST, "/provider/v1/commands", TEST_PROVIDER_TOKEN,
            "command-process-start", TEST_PROCESS_START_COMMAND, response,
            sizeof(response));
        if (start_status == 202) break;
        check_true(start_status == 503 || start_status == 425);
        proc_sleep(TEST_IRIS_DELIVERY_POLL_MS);
    }
    if (start_status != 202) {
        print_file_on_failure("orphan_start_room_service", g_rs_out);
        print_file_on_failure("orphan_start_worker", g_wk_out);
    }
    check_equal((int)(start_status), (int)(202));
    check_true(wait_atomic_int_at_least(
        &g_iris.completion_calls, completion_target,
        TEST_IRIS_DELIVERY_ATTEMPTS, TEST_IRIS_DELIVERY_POLL_MS));
    check_true(wait_file_contains(
        g_wk_out, "media runtime completed command-process-start",
        TEST_PROBE_LOG_ATTEMPTS, TEST_PROBE_LOG_POLL_MS));

    atomic_store_explicit(&g_iris.expected_resources_empty, 1,
                          memory_order_release);
    kill_child(&g_room_service);
    check_true(spawn_room_service());
    if (!wait_file_contains(g_wk_out, phase_needle,
                            TEST_IRIS_DELIVERY_ATTEMPTS,
                            TEST_IRIS_DELIVERY_POLL_MS)) {
        print_file_on_failure("orphan_window_room_service", g_rs_out);
        print_file_on_failure("orphan_window_worker", g_wk_out);
        check(0, "%s", ("orphan close did not enter crash window"));
    }

    /* The action is deliberately paused before its side effect, or after the
       side effect but before its result. Kill only RoomService; the worker and
       its media observation remain the restart facts. */
    kill_child(&g_room_service);
    check_true(create_resume_marker());
    check_true(spawn_room_service());
    if (!wait_reconcile_ready(response, sizeof(response))) {
        print_file_on_failure("restarted_orphan_room_service", g_rs_out);
        print_file_on_failure("orphan_worker", g_wk_out);
        check(0, "%s", ("orphan crash window did not converge to READY"));
    }
    if (expect_fenced_retry) {
        check_true(wait_file_contains(
            g_wk_out, "probe orphan close retry stable=0",
            TEST_IRIS_DELIVERY_ATTEMPTS, TEST_IRIS_DELIVERY_POLL_MS));
    }
    check_true(wait_file_contains(
        g_wk_out, "probe media destroy count=1",
        TEST_IRIS_DELIVERY_ATTEMPTS, TEST_IRIS_DELIVERY_POLL_MS));
    proc_sleep(500);
    check_false(file_contains(g_wk_out,
                                    "probe media destroy count=2"));
}

void test_reconcile_orphan_close_recovers_when_room_service_dies_before_action(
    void) {
    run_orphan_close_crash_window("orphan-close-before",
                                  "probe orphan close before id=", 1);
}

void test_reconcile_orphan_close_recovers_when_room_service_dies_after_action(
    void) {
    run_orphan_close_crash_window("orphan-close-after",
                                  "probe orphan close after applied id=", 0);
}

void test_control_ws_worker_partition_rebinds_and_deduplicates_media_command(void) {
    char response[TEST_HTTP_RESPONSE_CAPACITY];
    int completion_target;
    int event_target;
    int partition_status = 0;

    kill_child(&g_worker);
    kill_child(&g_room_service);
    g_router_bind_port = TEST_ROUTER_BACKEND_PORT;
    check_true(write_room_service_config(0));
    check_equal((int)(test_iris_proxy_start(&g_control_ws_proxy, TEST_ROUTER_PORT,
                                 TEST_ROUTER_BACKEND_PORT)), (int)(0));
    check_true(spawn_room_service());
    check_true(spawn_probe_worker("ivr-worker-dispatch", "media-runtime"));
    check_true(wait_provider_ready());

    completion_target =
        atomic_load_explicit(&g_iris.completion_calls,
                             memory_order_acquire) + 1;
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-start",
                            TEST_PROCESS_START_COMMAND, response,
                            sizeof(response))), (int)(202));
    check_true(wait_atomic_int_at_least(
        &g_iris.completion_calls, completion_target,
        TEST_IRIS_DELIVERY_ATTEMPTS, TEST_IRIS_DELIVERY_POLL_MS));

    test_iris_proxy_set_available(&g_control_ws_proxy, 0);
    proc_sleep(16000u); /* worker-advertised lease is 15 seconds */
    for (int attempt = 0; attempt < 3; ++attempt) {
        partition_status = facade_request(
            CHTTP_METHOD_POST, "/provider/v1/commands", TEST_PROVIDER_TOKEN,
            "command-process-play", TEST_PROCESS_PLAY_COMMAND, response,
            sizeof(response));
        if (partition_status != 0) break;
    }
    check_equal((int)(partition_status), (int)(503));
    check_false(file_contains(g_wk_out, "probe media play count=1"));

    test_iris_proxy_set_available(&g_control_ws_proxy, 1);
    /* CHTTP H1 WebSocket may already be in the 8-16 second jittered reconnect step after
       the 16-second partition. Cover that step plus the reconcile exchange. */
    if (!wait_rebound_ready_with_attempts(
            response, sizeof(response),
            TEST_CONTROL_WS_PARTITION_RECOVERY_ATTEMPTS)) {
        fprintf(stderr, "control WS partition metrics response:\n%s\n",
                response);
        print_file_on_failure("control_ws_partition_room_service", g_rs_out);
        print_file_on_failure("control_ws_partition_worker", g_wk_out);
        check(0, "%s", ("CHTTP H1 WebSocket worker partition did not rebind dialog"));
    }

    completion_target =
        atomic_load_explicit(&g_iris.completion_calls,
                             memory_order_acquire) + 1;
    event_target = atomic_load_explicit(&g_iris.event_calls,
                                        memory_order_acquire) + 1;
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-play",
                            TEST_PROCESS_PLAY_COMMAND, response,
                            sizeof(response))), (int)(202));
    check_true(wait_atomic_int_at_least(
        &g_iris.completion_calls, completion_target,
        TEST_IRIS_DELIVERY_ATTEMPTS, TEST_IRIS_DELIVERY_POLL_MS));
    check_true(wait_atomic_int_at_least(
        &g_iris.event_calls, event_target, TEST_IRIS_DELIVERY_ATTEMPTS,
        TEST_IRIS_DELIVERY_POLL_MS));
    check_true(wait_file_contains(
        g_wk_out, "probe media play count=1", TEST_PROBE_LOG_ATTEMPTS,
        TEST_PROBE_LOG_POLL_MS));

    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-play",
                            TEST_PROCESS_PLAY_COMMAND, response,
                            sizeof(response))), (int)(200));
    check_not_null(strstr(response, "\"disposition\":\"terminal\""));
    check_not_null(strstr(response, "\"duplicate\":true"));
    proc_sleep(500u);
    check_false(file_contains(g_wk_out, "probe media play count=2"));
    check_equal((int)(atomic_load_explicit(&g_iris.completion_calls,
                             memory_order_acquire)), (int)(completion_target));
    check_equal((int)(atomic_load_explicit(&g_iris.event_calls,
                                               memory_order_acquire)), (int)(event_target));
}

void test_real_worker_recovers_live_transport_and_releases_media_resources(void) {
    char response[TEST_HTTP_RESPONSE_CAPACITY];
    int completion_target;
    int event_target;
    int disconnected_before_partition;
    int reconnected_before_partition;
    unsigned long long participant_recovery_generation;
    unsigned long long packets_before_playback;
    unsigned long tts_observations;
    unsigned long outbox_persisted_before_partition;

    kill_child(&g_worker);
    kill_child(&g_room_service);
    g_router_bind_port = TEST_ROUTER_BACKEND_PORT;
    check_true(write_room_service_config(1));
    check_equal((int)(test_iris_proxy_start(&g_control_ws_proxy, TEST_ROUTER_PORT,
                                 TEST_ROUTER_BACKEND_PORT)), (int)(0));
    check_true(spawn_room_service());
    if (!provision_room_service_room()) {
        print_file_on_failure("secure_room_service", g_rs_out);
        check(0, "%s", ("secure RoomService did not accept control commands"));
    }
    check_true(wait_provider_ready());

    check_true(write_sfu_config());
    check_true(spawn_sfu());
    check_true(provision_sfu_room());
    if (!start_sfu_caller_transport()) {
        print_file_on_failure("caller_transport_room_service", g_rs_out);
        print_file_on_failure("caller_transport_sfu", g_sfu_out);
        check(0, "%s", ("SFU caller ICE/DTLS transport did not connect"));
    }
    check_true(provision_sfu_caller_subscription());
    check_true(spawn_real_worker());
    if (!wait_file_contains(g_wk_out, "worker.sync acknowledged", 200, 50)) {
        print_file_on_failure("secure_room_service", g_rs_out);
        print_file_on_failure("real_ivr_worker", g_wk_out);
        print_file_on_failure("live_sfu", g_sfu_out);
        check(0, "%s", ("real ivr_worker did not become ready over mTLS"));
    }
    check_true(wait_real_worker_ready());
    /* A successful sync makes the worker locally ready; the next bounded
       heartbeat publishes that state to the RoomService scheduler. */
    proc_sleep(TEST_REAL_WORKER_READY_PROPAGATION_MS);
    check_true(wait_provider_ready());

    {
        int status = facade_request(
            CHTTP_METHOD_POST, "/provider/v1/commands", TEST_PROVIDER_TOKEN,
            "command-process-start", TEST_PROCESS_START_COMMAND, response,
            sizeof(response));
        if (status != 202) {
            fprintf(stderr, "live dialog.start status=%d body=%s\n", status,
                    response);
            print_file_on_failure("secure_room_service", g_rs_out);
            print_file_on_failure("real_ivr_worker", g_wk_out);
            print_file_on_failure("live_sfu", g_sfu_out);
        }
        check_equal((int)(status), (int)(202));
    }
    check_true(wait_iris_deliveries(1, 0));
    if (!wait_sfu_room_counts(3u, 3u)) {
        print_file_on_failure("real_ivr_worker", g_wk_out);
        print_file_on_failure("live_sfu", g_sfu_out);
        check(0, "%s", ("initial WHIP/WHEP sessions did not reach SFU"));
    }
    check_true(sfu_media_participant_exists("call-42"));
    check_true(sfu_media_participant_exists("call-42-rx"));
    check_true(send_sfu_caller_audio());
    if (!wait_real_worker_media_links(2u)) {
        char metrics[4096] = {0};
        (void)http_request_raw("127.0.0.1", TEST_REAL_WORKER_HEALTH_PORT,
                               "GET", "/metrics", NULL, NULL, metrics,
                               sizeof(metrics));
        fprintf(stderr, "initial worker metrics:\n%s\n", metrics);
        print_file_on_failure("real_ivr_worker", g_wk_out);
        print_file_on_failure("live_sfu", g_sfu_out);
        check(0, "%s", ("worker WHIP/WHEP links did not both connect"));
    }

    check_true(read_sfu_room_packet_count(&packets_before_playback));
    completion_target =
        atomic_load_explicit(&g_iris.completion_calls, memory_order_acquire) + 1;
    event_target =
        atomic_load_explicit(&g_iris.event_calls, memory_order_acquire) + 1;
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-play",
                            TEST_PROCESS_PLAY_COMMAND, response,
                            sizeof(response))), (int)(202));
    check_true(wait_atomic_int_at_least(
        &g_iris.completion_calls, completion_target,
        TEST_IRIS_DELIVERY_ATTEMPTS, TEST_IRIS_DELIVERY_POLL_MS));
    check_true(wait_atomic_int_at_least(
        &g_iris.event_calls, event_target, TEST_IRIS_DELIVERY_ATTEMPTS,
        TEST_IRIS_DELIVERY_POLL_MS));
    if (!atomic_load_explicit(&g_iris.playback_event_valid,
                              memory_order_acquire)) {
        fprintf(stderr, "invalid live playback ProviderEvent=%s\n",
                g_iris.event_body);
        print_file_on_failure("live_playback_worker", g_wk_out);
        print_file_on_failure("live_playback_room_service", g_rs_out);
    }
    check_equal((int)(atomic_load_explicit(&g_iris.playback_event_valid,
                                memory_order_acquire)), (int)(1));
    check_equal((int)(atomic_load_explicit(&g_iris.speech_tts_calls,
                                memory_order_acquire)), (int)(1));
    check_equal((int)(atomic_load_explicit(&g_iris.speech_tts_valid,
                                memory_order_acquire)), (int)(1));
    if (!wait_sfu_room_packet_count_greater_than(packets_before_playback)) {
        char worker_metrics[16384] = {0};
        char room_stats[4096] = {0};
        char participant_stats[4096] = {0};
        (void)http_request_raw("127.0.0.1",
                               TEST_REAL_WORKER_HEALTH_PORT, "GET",
                               "/metrics", NULL, NULL, worker_metrics,
                               sizeof(worker_metrics));
        (void)http_post_command(
            "127.0.0.1", TEST_SFU_PORT, TEST_SFU_CTRL_TOKEN,
            "{\"type\":\"get_room_stats\",\"room_id\":\"room-42\"}",
            room_stats, sizeof(room_stats));
        (void)http_post_command(
            "127.0.0.1", TEST_SFU_PORT, TEST_SFU_CTRL_TOKEN,
            "{\"type\":\"get_participant_stats\",\"room_id\":\"room-42\","
            "\"participant_id\":\"call-42\"}",
            participant_stats, sizeof(participant_stats));
        fprintf(stderr,
                "TTS RTP did not reach SFU; baseline=%llu\n"
                "worker metrics:\n%s\nroom stats:\n%s\n"
                "publisher stats:\n%s\n",
                packets_before_playback, worker_metrics, room_stats,
                participant_stats);
        print_file_on_failure("real_ivr_worker", g_wk_out);
        print_file_on_failure("live_sfu", g_sfu_out);
        check(0, "%s", ("remote TTS PCM did not reach the live SFU"));
    }
    check_true(read_real_worker_metric(
        "turbo_ivr_worker_tts_provider_duration_seconds_count",
        &tts_observations));
    check_equal((uint64_t)(tts_observations), (uint64_t)(1u));

    /* The durable ledger must replay the terminal outcome without repeating
       the remote speech side effect or emitting another terminal fact. */
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-play",
                            TEST_PROCESS_PLAY_COMMAND, response,
                            sizeof(response))), (int)(200));
    check_not_null(strstr(response, "\"disposition\":\"terminal\""));
    check_not_null(strstr(response, "\"duplicate\":true"));
    proc_sleep(500u);
    check_equal((int)(atomic_load_explicit(&g_iris.speech_tts_calls,
                                memory_order_acquire)), (int)(1));
    check_equal((int)(atomic_load_explicit(&g_iris.completion_calls,
                             memory_order_acquire)), (int)(completion_target));
    check_equal((int)(atomic_load_explicit(&g_iris.event_calls, memory_order_acquire)), (int)(event_target));

    check_true(disconnect_sfu_media_participant("call-42-rx"));
    if (!wait_atomic_int_at_least(
            &g_iris.rtc_disconnected_calls, 1,
            TEST_RTC_TRANSITION_ATTEMPTS, TEST_RTC_TRANSITION_POLL_MS)) {
        fprintf(stderr,
                "Iris media events=%d valid=%d disconnected=%d last_body=%s\n",
                atomic_load_explicit(&g_iris.event_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.event_valid,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.rtc_disconnected_calls,
                                     memory_order_acquire),
                g_iris.event_body);
        print_file_on_failure("secure_room_service", g_rs_out);
        print_file_on_failure("real_ivr_worker", g_wk_out);
        print_file_on_failure("live_sfu", g_sfu_out);
        check(0, "%s", ("live media fault did not reach Iris as rtc.disconnected"));
    }
    if (!wait_atomic_int_at_least(
            &g_iris.rtc_reconnected_calls, 1,
            TEST_RTC_TRANSITION_ATTEMPTS, TEST_RTC_TRANSITION_POLL_MS)) {
        char metrics[16384] = {0};
        char stats[4096] = {0};
        (void)http_request_raw("127.0.0.1", TEST_REAL_WORKER_HEALTH_PORT,
                               "GET", "/metrics", NULL, NULL, metrics,
                               sizeof(metrics));
        (void)http_post_command(
            "127.0.0.1", TEST_SFU_PORT, TEST_SFU_CTRL_TOKEN,
            "{\"type\":\"get_room_stats\",\"room_id\":\"room-42\"}",
            stats, sizeof(stats));
        fprintf(stderr,
                "participant recovery events disconnected=%d reconnected=%d "
                "exhausted=%d generations=%llu/%llu\nmetrics:\n%s\n"
                "SFU stats:\n%s\n",
                atomic_load_explicit(&g_iris.rtc_disconnected_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.rtc_reconnected_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.rtc_retry_exhausted_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.rtc_disconnected_generation,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.rtc_reconnected_generation,
                                     memory_order_acquire),
                metrics, stats);
        print_file_on_failure("secure_room_service", g_rs_out);
        print_file_on_failure("real_ivr_worker", g_wk_out);
        print_file_on_failure("live_sfu", g_sfu_out);
        check(0, "%s", ("live media retry did not reach Iris as rtc.reconnected"));
    }
    check_true(wait_sfu_room_counts(3u, 3u));
    proc_sleep(500);
    check_equal((int)(atomic_load_explicit(&g_iris.rtc_event_valid,
                                memory_order_acquire)), (int)(1));
    check_equal((int)(atomic_load_explicit(&g_iris.rtc_disconnected_calls,
                                memory_order_acquire)), (int)(1));
    check_equal((int)(atomic_load_explicit(&g_iris.rtc_reconnected_calls,
                                memory_order_acquire)), (int)(1));
    check_equal((int)(atomic_load_explicit(&g_iris.rtc_retry_exhausted_calls,
                                memory_order_acquire)), (int)(0));
    check_true(atomic_load_explicit(&g_iris.rtc_reconnected_generation,
                             memory_order_acquire) >
        atomic_load_explicit(&g_iris.rtc_disconnected_generation,
                             memory_order_acquire));
    participant_recovery_generation = atomic_load_explicit(
        &g_iris.rtc_reconnected_generation, memory_order_acquire);

    /* Exercise a whole-SFU process loss while the dialog and its media
       resources remain active. The worker supervisor owns recovery of its
       WHIP/WHEP links; the caller fixture is explicitly recreated with the
       same stable call identity after the new SFU is ready. */
    check_true(stop_sfu_caller_transport());
    check_true(wait_sfu_room_counts(2u, 2u));
    check_true(read_room_service_metric(
        "turbo_room_service_iris_outbox_persisted_total",
        &outbox_persisted_before_partition));
    disconnected_before_partition = atomic_load_explicit(
        &g_iris.rtc_disconnected_calls, memory_order_acquire);
    reconnected_before_partition = atomic_load_explicit(
        &g_iris.rtc_reconnected_calls, memory_order_acquire);
    test_iris_proxy_set_available(&g_iris_proxy, 0);
    kill_child(&g_sfu);
    if (!wait_room_service_metric_greater_than(
            "turbo_room_service_iris_outbox_persisted_total",
            outbox_persisted_before_partition,
            TEST_SFU_PROCESS_LOSS_ATTEMPTS)) {
        print_file_on_failure("secure_room_service", g_rs_out);
        print_file_on_failure("real_ivr_worker", g_wk_out);
        check(0, "%s", ("whole-SFU loss did not persist while Iris was unavailable"));
    }
    test_iris_proxy_set_available(&g_iris_proxy, 1);
    check_equal((int)(probe_restarted_iris()), (int)(404));
    atomic_store_explicit(&g_iris.expected_resources_enabled, 1,
                          memory_order_release);
    if (!wait_atomic_int_at_least(
            &g_iris.rtc_disconnected_calls,
            disconnected_before_partition + 1,
            TEST_IRIS_PARTITION_RECOVERY_ATTEMPTS,
            TEST_RTC_TRANSITION_POLL_MS)) {
        fprintf(stderr,
                "Iris delivery after restart: events=%d valid=%d "
                "disconnected=%d expected_leases=%d expected_pages=%d "
                "last_body=%s\n",
                atomic_load_explicit(&g_iris.event_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.event_valid,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.rtc_disconnected_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.expected_lease_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.expected_page_calls,
                                     memory_order_acquire),
                g_iris.event_body);
        print_room_service_iris_outbox_metrics();
        print_file_on_failure("secure_room_service", g_rs_out);
        print_file_on_failure("real_ivr_worker", g_wk_out);
        check(0, "%s", ("durable whole-SFU disconnect was not delivered after Iris restart"));
    }

    check_true(spawn_sfu());
    check_true(provision_sfu_room());
    check_true(start_sfu_caller_transport());
    check_true(provision_sfu_caller_subscription());
    check_true(send_sfu_caller_audio());
    if (!wait_atomic_int_at_least(
            &g_iris.rtc_reconnected_calls, reconnected_before_partition + 1,
            TEST_RTC_TRANSITION_ATTEMPTS, TEST_RTC_TRANSITION_POLL_MS)) {
        char metrics[4096] = {0};
        char stats[4096] = {0};
        (void)http_request_raw("127.0.0.1", TEST_REAL_WORKER_HEALTH_PORT,
                               "GET", "/metrics", NULL, NULL, metrics,
                               sizeof(metrics));
        (void)http_post_command(
            "127.0.0.1", TEST_SFU_PORT, TEST_SFU_CTRL_TOKEN,
            "{\"type\":\"get_room_stats\",\"room_id\":\"room-42\"}",
            stats, sizeof(stats));
        fprintf(stderr,
                "whole-SFU recovery events disconnected=%d reconnected=%d "
                "exhausted=%d generations=%llu/%llu\nmetrics:\n%s\n"
                "SFU stats:\n%s\n",
                atomic_load_explicit(&g_iris.rtc_disconnected_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.rtc_reconnected_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.rtc_retry_exhausted_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.rtc_disconnected_generation,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.rtc_reconnected_generation,
                                     memory_order_acquire),
                metrics, stats);
        print_file_on_failure("secure_room_service", g_rs_out);
        print_file_on_failure("real_ivr_worker", g_wk_out);
        print_file_on_failure("restarted_live_sfu", g_sfu_out);
        check(0, "%s", ("whole SFU restart did not reach Iris as rtc.reconnected"));
    }
    check_true(wait_real_worker_media_links(2u));
    check_true(wait_sfu_room_counts(3u, 3u));
    check_true(sfu_media_participant_exists("call-42"));
    check_true(sfu_media_participant_exists("call-42-rx"));
    proc_sleep(500);
    check_equal((int)(atomic_load_explicit(&g_iris.rtc_disconnected_calls,
                             memory_order_acquire)), (int)(disconnected_before_partition + 1));
    check_equal((int)(atomic_load_explicit(&g_iris.rtc_reconnected_calls,
                             memory_order_acquire)), (int)(reconnected_before_partition + 1));
    check_equal((int)(atomic_load_explicit(&g_iris.rtc_retry_exhausted_calls,
                                memory_order_acquire)), (int)(0));
    check_equal((uint64_t)(atomic_load_explicit(&g_iris.rtc_disconnected_generation,
                             memory_order_acquire)), (uint64_t)(participant_recovery_generation));
    check_true(atomic_load_explicit(&g_iris.rtc_reconnected_generation,
                             memory_order_acquire) >
        atomic_load_explicit(&g_iris.rtc_disconnected_generation,
                             memory_order_acquire));

    /* Reconcile the recovered media fact through a fresh RoomService. The
       active slot remains worker-owned; CHTTP H1 WebSocket reconnect advances its epoch,
       and exact expected/inventory rebind must finish before READY. */
    kill_child(&g_room_service);
    check_true(spawn_room_service());
    if (!wait_rebound_ready_with_attempts(
            response, sizeof(response),
            TEST_CONTROL_WS_PARTITION_RECOVERY_ATTEMPTS)) {
        fprintf(stderr, "whole-SFU reconcile metrics response:\n%s\n",
                response);
        print_file_on_failure("whole_sfu_restarted_room_service", g_rs_out);
        print_file_on_failure("whole_sfu_active_worker", g_wk_out);
        check(0, "%s", ("recovered whole-SFU dialog did not reconcile after RoomService restart"));
    }

    event_target =
        atomic_load_explicit(&g_iris.event_calls, memory_order_acquire) + 1;
    (void)http_request_raw("127.0.0.1", TEST_REAL_WORKER_HEALTH_PORT,
                           "POST", "/drain", NULL, NULL, response,
                           sizeof(response));
    check_not_null(strstr(response, " 202 "));
    check_true(wait_child_exit(&g_worker, 30000u));
    if (!wait_file_contains(g_wk_out, "ivr_worker: draining",
                            TEST_IRIS_DELIVERY_ATTEMPTS,
                            TEST_IRIS_DELIVERY_POLL_MS)) {
        print_file_on_failure("worker_drain", g_wk_out);
        print_file_on_failure("worker_drain_sfu", g_sfu_out);
        check(0, "%s", ("worker exited without the graceful drain marker"));
    }
    check_true(wait_atomic_int_at_least(
        &g_iris.event_calls, event_target, TEST_IRIS_DELIVERY_ATTEMPTS,
        TEST_IRIS_DELIVERY_POLL_MS));
    check_not_null(strstr(g_iris.event_body,
                                "provider.media.worker_lost"));
    if (!wait_sfu_room_counts(1u, 1u)) {
        print_file_on_failure("real_ivr_worker", g_wk_out);
        print_file_on_failure("live_sfu", g_sfu_out);
        check(0, "%s", ("worker drain did not release live SFU resources"));
    }
    /* Worker drain may already remove the last subscription and the source
       track. The transport destroy below is idempotent; 0/0 is the fact. */
    (void)stop_sfu_caller_transport();
    if (!wait_sfu_room_counts(0u, 0u)) {
        char room_stats[4096] = {0};
        char caller_stats[2048] = {0};
        (void)http_post_command(
            "127.0.0.1", TEST_SFU_PORT, TEST_SFU_CTRL_TOKEN,
            "{\"type\":\"get_room_stats\",\"room_id\":\"room-42\"}",
            room_stats, sizeof(room_stats));
        (void)http_post_command(
            "127.0.0.1", TEST_SFU_PORT, TEST_SFU_CTRL_TOKEN,
            "{\"type\":\"get_participant_stats\",\"room_id\":\"room-42\","
            "\"participant_id\":\"" TEST_SFU_CALLER_PARTICIPANT_ID "\"}",
            caller_stats, sizeof(caller_stats));
        fprintf(stderr,
                "caller teardown did not release SFU resource\n"
                "room stats:\n%s\ncaller stats:\n%s\n",
                room_stats, caller_stats);
        print_file_on_failure("caller_teardown_sfu", g_sfu_out);
        check(0, "%s", ("caller teardown did not release live SFU resource"));
    }
    proc_sleep(TEST_REAL_WORKER_READY_PROPAGATION_MS);
    check_equal((int)(atomic_load_explicit(&g_iris.event_calls, memory_order_acquire)), (int)(event_target));
}

void test_iris_control_completion_and_event(void) {
    static const char create_room_command[] =
        "{"
        "\"schemaVersion\":3,\"commandId\":\"command-room-create\","
        "\"tenantId\":\"tenant-process\",\"sessionId\":\"session-process\","
        "\"type\":\"conference.create\",\"provider\":\"turbomedia\","
        "\"correlationId\":\"room-process\",\"causationId\":\"state-room\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\",\"dispatchEpoch\":1,"
        "\"data\":{\"capability\":\"room\",\"roomId\":\"room-process\","
        "\"roomGeneration\":1}}";
    static const char retry_create_room_command[] =
        "{"
        "\"schemaVersion\":3,\"commandId\":\"command-room-create\","
        "\"tenantId\":\"tenant-process\",\"sessionId\":\"session-process\","
        "\"type\":\"conference.create\",\"provider\":\"turbomedia\","
        "\"correlationId\":\"room-process\",\"causationId\":\"state-room\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-reclaimed\",\"dispatchEpoch\":2,"
        "\"data\":{\"capability\":\"room\",\"roomId\":\"room-process\","
        "\"roomGeneration\":1}}";
    static const char join_room_command[] =
        "{"
        "\"schemaVersion\":3,\"commandId\":\"command-room-join\","
        "\"tenantId\":\"tenant-process\",\"sessionId\":\"session-process\","
        "\"type\":\"connection.join\",\"provider\":\"turbomedia\","
        "\"correlationId\":\"room-process\",\"causationId\":\"command-room-create\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\",\"dispatchEpoch\":1,"
        "\"data\":{\"capability\":\"room\",\"roomId\":\"room-process\","
        "\"roomGeneration\":1,\"callId\":\"call-process\","
        "\"callGeneration\":1,\"role\":\"guest\"}}";
    static const char unjoin_room_command[] =
        "{"
        "\"schemaVersion\":3,\"commandId\":\"command-room-unjoin\","
        "\"tenantId\":\"tenant-process\",\"sessionId\":\"session-process\","
        "\"type\":\"connection.unjoin\",\"provider\":\"turbomedia\","
        "\"correlationId\":\"room-process\",\"causationId\":\"command-room-join\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\",\"dispatchEpoch\":1,"
        "\"data\":{\"capability\":\"room\",\"roomId\":\"room-process\","
        "\"roomGeneration\":1,\"callId\":\"call-process\","
        "\"callGeneration\":1}}";
    static const char unjoin_room_again_command[] =
        "{"
        "\"schemaVersion\":3,\"commandId\":\"command-room-unjoin-again\","
        "\"tenantId\":\"tenant-process\",\"sessionId\":\"session-process\","
        "\"type\":\"connection.unjoin\",\"provider\":\"turbomedia\","
        "\"correlationId\":\"room-process\","
        "\"causationId\":\"command-room-unjoin\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\",\"dispatchEpoch\":1,"
        "\"data\":{\"capability\":\"room\",\"roomId\":\"room-process\","
        "\"roomGeneration\":1,\"callId\":\"call-process\","
        "\"callGeneration\":1}}";
    static const char destroy_nonempty_room_command[] =
        "{"
        "\"schemaVersion\":3,\"commandId\":\"command-room-destroy-nonempty\","
        "\"tenantId\":\"tenant-process\",\"sessionId\":\"session-process\","
        "\"type\":\"conference.destroy\",\"provider\":\"turbomedia\","
        "\"correlationId\":\"room-process\",\"causationId\":\"command-room-join\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\",\"dispatchEpoch\":1,"
        "\"data\":{\"capability\":\"room\",\"roomId\":\"room-process\","
        "\"roomGeneration\":1}}";
    static const char destroy_room_command[] =
        "{"
        "\"schemaVersion\":3,\"commandId\":\"command-room-destroy\","
        "\"tenantId\":\"tenant-process\",\"sessionId\":\"session-process\","
        "\"type\":\"conference.destroy\",\"provider\":\"turbomedia\","
        "\"correlationId\":\"room-process\",\"causationId\":\"command-room-unjoin\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\",\"dispatchEpoch\":1,"
        "\"data\":{\"capability\":\"room\",\"roomId\":\"room-process\","
        "\"roomGeneration\":1}}";
    static const char destroy_room_again_command[] =
        "{"
        "\"schemaVersion\":3,\"commandId\":\"command-room-destroy-again\","
        "\"tenantId\":\"tenant-process\",\"sessionId\":\"session-process\","
        "\"type\":\"conference.destroy\",\"provider\":\"turbomedia\","
        "\"correlationId\":\"room-process\","
        "\"causationId\":\"command-room-destroy\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\",\"dispatchEpoch\":1,"
        "\"data\":{\"capability\":\"room\",\"roomId\":\"room-process\","
        "\"roomGeneration\":1}}";
    static const char collect_command[] =
        "{"
        "\"schemaVersion\":2,"
        "\"commandId\":\"command-process-collect\","
        "\"tenantId\":\"tenant-process\","
        "\"sessionId\":\"session-process\","
        "\"type\":\"media.collect\","
        "\"provider\":\"turbomedia\","
        "\"correlationId\":\"dialog-process\","
        "\"causationId\":\"state-process-playing\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\","
        "\"dispatchEpoch\":1,"
        "\"data\":{"
        "\"capability\":\"ivr\","
        "\"dialogId\":\"dialog-process\","
        "\"roomId\":\"room-42\","
        "\"callId\":\"call-42\","
        "\"callGeneration\":1,"
        "\"operationGeneration\":3,"
        "\"inputId\":\"input-process\","
        "\"inputGeneration\":1"
        "}"
        "}";
    static const char cancel_command[] =
        "{"
        "\"schemaVersion\":2,"
        "\"commandId\":\"command-process-cancel\","
        "\"tenantId\":\"tenant-process\","
        "\"sessionId\":\"session-process\","
        "\"type\":\"media.cancel\","
        "\"provider\":\"turbomedia\","
        "\"correlationId\":\"dialog-process\","
        "\"causationId\":\"state-process-collected\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\","
        "\"dispatchEpoch\":1,"
        "\"data\":{"
        "\"capability\":\"ivr\","
        "\"dialogId\":\"dialog-process\","
        "\"roomId\":\"room-42\","
        "\"callId\":\"call-42\","
        "\"callGeneration\":1,"
        "\"operationGeneration\":4,"
        "\"inputId\":\"input-process\","
        "\"inputGeneration\":1"
        "}"
        "}";
    static const char close_command[] =
        "{"
        "\"schemaVersion\":2,"
        "\"commandId\":\"command-process-close\","
        "\"tenantId\":\"tenant-process\","
        "\"sessionId\":\"session-process\","
        "\"type\":\"dialog.terminate\","
        "\"provider\":\"turbomedia\","
        "\"correlationId\":\"dialog-process\","
        "\"causationId\":\"state-process-cancelled\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\","
        "\"dispatchEpoch\":1,"
        "\"data\":{"
        "\"capability\":\"ivr\","
        "\"dialogId\":\"dialog-process\","
        "\"roomId\":\"room-42\","
        "\"callId\":\"call-42\","
        "\"callGeneration\":1,"
        "\"operationGeneration\":5,"
        "\"reason\":\"test.complete\""
        "}"
        "}";
    static const char close_again_command[] =
        "{"
        "\"schemaVersion\":2,"
        "\"commandId\":\"command-process-close-again\","
        "\"tenantId\":\"tenant-process\","
        "\"sessionId\":\"session-process\","
        "\"type\":\"dialog.terminate\","
        "\"provider\":\"turbomedia\","
        "\"correlationId\":\"dialog-process\","
        "\"causationId\":\"command-process-close\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-process\","
        "\"dispatchEpoch\":1,"
        "\"data\":{"
        "\"capability\":\"ivr\","
        "\"dialogId\":\"dialog-process\","
        "\"roomId\":\"room-42\","
        "\"callId\":\"call-42\","
        "\"callGeneration\":1,"
        "\"operationGeneration\":6,"
        "\"reason\":\"test.repeat\""
        "}"
        "}";
    char response[TEST_HTTP_RESPONSE_CAPACITY];
    int completed = 0;
    int request_status;

    request_status = facade_request(
        CHTTP_METHOD_POST, "/provider/v1/commands", TEST_PROVIDER_TOKEN,
        "command-room-create", create_room_command, response,
        sizeof(response));
    if (request_status != 200) {
        fprintf(stderr, "room create rejected status=%d response=%s\n",
                request_status, response);
    }
    check_equal((int)(request_status), (int)(200));
    check_not_null(strstr(response, "\"terminalStatus\":\"succeeded\""));
    check_not_null(strstr(response, "\"eventType\":\"provider.conference.created\""));
    check_not_null(strstr(response, "\"duplicate\":false"));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-room-create",
                            retry_create_room_command, response,
                            sizeof(response))), (int)(200));
    check_not_null(strstr(response, "\"duplicate\":true"));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-room-join",
                            join_room_command, response, sizeof(response))), (int)(200));
    check_not_null(strstr(response, "\"eventType\":\"provider.connection.joined\""));
    check_not_null(strstr(response, "\"callId\":\"call-process\""));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN,
                            "command-room-destroy-nonempty",
                            destroy_nonempty_room_command, response,
                            sizeof(response))), (int)(200));
    check_not_null(strstr(response, "\"terminalStatus\":\"failed\""));
    check_not_null(strstr(response, "\"code\":\"ROOM_NOT_EMPTY\""));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-room-unjoin",
                            unjoin_room_command, response, sizeof(response))), (int)(200));
    check_not_null(strstr(response, "\"eventType\":\"provider.connection.unjoined\""));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-room-unjoin-again",
                            unjoin_room_again_command, response,
                            sizeof(response))), (int)(200));
    check_not_null(strstr(response, "\"terminalStatus\":\"succeeded\""));
    check_not_null(strstr(response, "\"alreadyAbsent\":true"));
    check_not_null(strstr(response, "\"callGeneration\":1"));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-room-destroy",
                            destroy_room_command, response, sizeof(response))), (int)(200));
    check_not_null(strstr(response, "\"eventType\":\"provider.conference.destroyed\""));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-room-destroy-again",
                            destroy_room_again_command, response,
                            sizeof(response))), (int)(200));
    check_not_null(strstr(response, "\"terminalStatus\":\"succeeded\""));
    check_not_null(strstr(response, "\"alreadyAbsent\":true"));
    check_not_null(strstr(response, "\"roomGeneration\":1"));

    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-start",
                            TEST_PROCESS_START_COMMAND, response, sizeof(response))), (int)(202));
    check_not_null(strstr(response, "\"disposition\":\"accepted\""));
    check_not_null(strstr(response, "\"workerId\":\"iris-worker-process\""));

    check_true(wait_iris_deliveries(1, 0));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-play",
                            TEST_PROCESS_PLAY_COMMAND, response, sizeof(response))), (int)(202));
    check_true(wait_iris_deliveries(2, 1));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-collect",
                            collect_command, response, sizeof(response))), (int)(202));
    check_true(wait_iris_deliveries(3, 3));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-cancel",
                            cancel_command, response, sizeof(response))), (int)(202));
    check_true(wait_iris_deliveries(4, 3));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN, "command-process-close",
                            close_command, response, sizeof(response))), (int)(202));
    completed = wait_iris_deliveries(5, 3);
    if (!completed) {
        char metrics[TEST_HTTP_RESPONSE_CAPACITY];
        int metrics_status = http_request_raw(
            "127.0.0.1", TEST_HTTP_PORT, "GET", "/metrics", NULL, NULL,
            metrics, sizeof(metrics));
        fprintf(stderr,
                "Iris deliveries: completions=%d events=%d completion_valid=%d "
                "event_valid=%d\nlast completion=%s\nlast event=%s\n"
                "metrics_status=%d\n%s\n",
                atomic_load_explicit(&g_iris.completion_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.event_calls,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.completion_valid,
                                     memory_order_acquire),
                atomic_load_explicit(&g_iris.event_valid,
                                     memory_order_acquire),
                g_iris.completion_body, g_iris.event_body, metrics_status,
                metrics);
        print_file_on_failure("room_service", g_rs_out);
        print_file_on_failure("dispatch_worker_probe", g_wk_out);
    }
    check_true(completed);
    check_equal((int)(atomic_load_explicit(&g_iris.completion_valid,
                                memory_order_acquire)), (int)(1));
    check_equal((int)(atomic_load_explicit(&g_iris.event_valid, memory_order_acquire)), (int)(1));
    check_equal((int)(atomic_load_explicit(&g_iris.playback_event_valid,
                                memory_order_acquire)), (int)(1));
    check_equal((int)(atomic_load_explicit(&g_iris.asr_event_valid,
                                memory_order_acquire)), (int)(1));
    check_equal((int)(atomic_load_explicit(&g_iris.dtmf_event_valid,
                                memory_order_acquire)), (int)(1));
    check_true(wait_file_contains(g_wk_out,
                           "media runtime completed command-process-close",
                           TEST_PROBE_LOG_ATTEMPTS,
                           TEST_PROBE_LOG_POLL_MS));
    check_equal((int)(facade_request(CHTTP_METHOD_POST, "/provider/v1/commands",
                            TEST_PROVIDER_TOKEN,
                            "command-process-close-again",
                            close_again_command, response,
                            sizeof(response))), (int)(200));
    if (!strstr(response, "\"terminalStatus\":\"succeeded\"")) {
        fprintf(stderr, "repeat close response=%s\n", response);
        print_file_on_failure("repeat_close_room_service", g_rs_out);
    }
    check_not_null(strstr(response, "\"terminalStatus\":\"succeeded\""));
    check_not_null(strstr(response, "\"eventType\":\"provider.dialog.terminated\""));
    check_not_null(strstr(response, "\"alreadyAbsent\":true"));
    check_not_null(strstr(response, "\"duplicate\":false"));

    check_equal((int)(facade_request(CHTTP_METHOD_GET, "/metrics", NULL, NULL, NULL, response,
                            sizeof(response))), (int)(200));
    check_not_null(strstr(
        response, "turbo_room_service_iris_completion_success_total "));
    check_not_null(strstr(
        response, "turbo_room_service_iris_event_success_total 3\n"));
    check_not_null(strstr(
        response, "turbo_room_service_iris_outbox_dead_records 0\n"));
    check_not_null(strstr(
        response, "turbo_room_service_iris_reconcile_state 5\n"));
    check_not_null(strstr(
        response,
        "turbo_room_service_iris_reconcile_accepting_commands 1\n"));
    check_not_null(strstr(
        response,
        "turbo_room_service_iris_reconcile_expected_fetches_total "));
    check_not_null(strstr(
        response,
        "turbo_room_service_iris_reconcile_inventory_pages_total "));
}

spec("test_ivr_dispatch_processes") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  it("test_worker_exit_before_dispatch_rejects_without_reservation") { test_worker_exit_before_dispatch_rejects_without_reservation(); };
  it("test_worker_exit_during_dialog_open_emits_loss_and_replacement_recovers") { test_worker_exit_during_dialog_open_emits_loss_and_replacement_recovers(); };
  it("test_worker_exit_after_result_preserves_room_membership") { test_worker_exit_after_result_preserves_room_membership(); };
  it("test_room_service_restart_rebinds_active_worker_dialog") { test_room_service_restart_rebinds_active_worker_dialog(); };
  it("test_reconcile_orphan_close_recovers_when_room_service_dies_before_action") { test_reconcile_orphan_close_recovers_when_room_service_dies_before_action(); };
  it("test_reconcile_orphan_close_recovers_when_room_service_dies_after_action") { test_reconcile_orphan_close_recovers_when_room_service_dies_after_action(); };
  it("test_control_ws_worker_partition_rebinds_and_deduplicates_media_command") { test_control_ws_worker_partition_rebinds_and_deduplicates_media_command(); };
  it("test_real_worker_recovers_live_transport_and_releases_media_resources") { test_real_worker_recovers_live_transport_and_releases_media_resources(); };
  it("test_iris_control_completion_and_event") { test_iris_control_completion_and_event(); };
}
