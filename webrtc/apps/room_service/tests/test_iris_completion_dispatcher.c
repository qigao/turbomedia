#include "iris_completion_dispatcher.h"

#include <tinytest.h>
#include <turbo_thread.h>
#include <iris/iris_app.h>
#include <iris/server.h>
#include <platform.h>
#include <turbo_coro_context.h>
#include <turbo_coro_socket.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef ROOM_SERVICE_TEST_TLS_CERT_PATH
#error "ROOM_SERVICE_TEST_TLS_CERT_PATH must identify the test certificate"
#endif

#ifndef ROOM_SERVICE_TEST_TLS_KEY_PATH
#error "ROOM_SERVICE_TEST_TLS_KEY_PATH must identify the test private key"
#endif

enum {
    TEST_TLS_LOOPBACK_PORT = 19998,
    TEST_TLS_BODY_CAPACITY = 8192,
    TEST_EVENT_ID_CAPACITY =
        sizeof(((ivr_media_event_t *)0)->event_id)
};

typedef enum test_tls_server_state_e {
    TEST_TLS_SERVER_STARTING = 0,
    TEST_TLS_SERVER_RUNNING,
    TEST_TLS_SERVER_FAILED,
    TEST_TLS_SERVER_STOPPED
} test_tls_server_state_t;

typedef struct test_tls_server_s {
    iris_app_t *app;
    coro_context_t *context;
    coro_socket_t *listener;
    turbo_thread_t thread;
    turbo_mutex_t mutex;
    turbo_cond_t lifecycle;
    test_tls_server_state_t state;
    int thread_started;
    atomic_int calls;
    atomic_int request_valid;
    char body[TEST_TLS_BODY_CAPACITY];
} test_tls_server_t;

static test_tls_server_t *g_test_tls_server = NULL;

static void test_tls_event_handler(Req *req, Res *res) {
    test_tls_server_t *server = g_test_tls_server;
    const char *authorization;
    const char *session_id;
    int valid;

    if (!server) {
        reply(res, 500, "application/json", "{}", 2u);
        return;
    }
    authorization = get_headers(req, "Authorization");
    session_id = get_params(req, "sessionId");
    valid = authorization && session_id &&
                strcmp(authorization, "Bearer provider-token") == 0 &&
                strcmp(session_id, "session-a") == 0 &&
                req->body && req->body_len < sizeof(server->body);

    if (valid) {
        memcpy(server->body, req->body, req->body_len);
        server->body[req->body_len] = '\0';
        valid = strstr(server->body, "\"eventId\":\"event-tls\"") != NULL &&
                strstr(server->body, "\"source\":\"turbomedia\"") != NULL &&
                strstr(server->body,
                       "\"correlationId\":\"dialog-a\"") != NULL &&
                strstr(server->body,
                       "\"dialogId\":\"dialog-a\"") != NULL &&
                strstr(server->body, "\"value\":\"tls\"") != NULL;
    }
    atomic_store(&server->request_valid, valid ? 1 : 0);
    atomic_fetch_add(&server->calls, 1);
    reply(res, valid ? 202 : 400, "application/json",
          valid ? "{}" : "{\"error\":\"invalid request\"}",
          valid ? 2u : sizeof("{\"error\":\"invalid request\"}") - 1u);
}

static void test_tls_server_thread(void *context) {
    test_tls_server_t *server = (test_tls_server_t *)context;
    turbo_tls_server_config_t tls;
    coro_context_t *coro_context = coro_context_create(NULL);
    coro_socket_t *listener = NULL;

    memset(&tls, 0, sizeof(tls));
    tls.size = sizeof(tls);
    tls.cert_file = ROOM_SERVICE_TEST_TLS_CERT_PATH;
    tls.key_file = ROOM_SERVICE_TEST_TLS_KEY_PATH;
    tls.client_auth = TURBO_TLS_CLIENT_AUTH_NONE;
    if (coro_context) {
        listener = iris_server_start_tls_on(
            server->app, coro_context, "127.0.0.1", TEST_TLS_LOOPBACK_PORT,
            &tls);
    }

    turbo_mutex_lock(&server->mutex);
    server->context = coro_context;
    server->listener = listener;
    server->state = listener ? TEST_TLS_SERVER_RUNNING : TEST_TLS_SERVER_FAILED;
    turbo_cond_broadcast(&server->lifecycle);
    turbo_mutex_unlock(&server->mutex);

    if (listener) {
        coro_context_set_persistent(coro_context, 1);
        coro_context_run(coro_context, TURBO_RUN_DEFAULT);
        coro_context_set_persistent(coro_context, 0);
        coro_socket_destroy(listener);
    }
    if (coro_context) coro_context_destroy(coro_context);

    turbo_mutex_lock(&server->mutex);
    server->context = NULL;
    server->listener = NULL;
    server->state = TEST_TLS_SERVER_STOPPED;
    turbo_cond_broadcast(&server->lifecycle);
    turbo_mutex_unlock(&server->mutex);
}

static int test_tls_server_start(test_tls_server_t *server) {
    memset(server, 0, sizeof(*server));
    server->app = iris_app_create();
    if (!server->app) return -1;
    turbo_mutex_init(&server->mutex);
    turbo_cond_init(&server->lifecycle);
    server->state = TEST_TLS_SERVER_STARTING;
    g_test_tls_server = server;
    iris_app_post(server->app, "/v1/sessions/:sessionId/events",
                  test_tls_event_handler);
    if (turbo_thread_create(&server->thread, test_tls_server_thread, server) !=
        0) {
        g_test_tls_server = NULL;
        turbo_cond_destroy(&server->lifecycle);
        turbo_mutex_destroy(&server->mutex);
        iris_app_destroy(server->app);
        server->app = NULL;
        return -1;
    }
    server->thread_started = 1;
    turbo_mutex_lock(&server->mutex);
    while (server->state == TEST_TLS_SERVER_STARTING) {
        turbo_cond_wait(&server->lifecycle, &server->mutex);
    }
    turbo_mutex_unlock(&server->mutex);
    if (server->state == TEST_TLS_SERVER_RUNNING) return 0;
    turbo_thread_join(&server->thread);
    server->thread_started = 0;
    g_test_tls_server = NULL;
    turbo_cond_destroy(&server->lifecycle);
    turbo_mutex_destroy(&server->mutex);
    iris_app_destroy(server->app);
    server->app = NULL;
    return -1;
}

static void test_tls_server_stop(test_tls_server_t *server) {
    coro_context_t *context;
    if (!server || !server->app) return;
    turbo_mutex_lock(&server->mutex);
    context = server->context;
    turbo_mutex_unlock(&server->mutex);
    if (context) coro_context_stop(context);
    if (server->thread_started) {
        turbo_thread_join(&server->thread);
        server->thread_started = 0;
    }
    g_test_tls_server = NULL;
    turbo_cond_destroy(&server->lifecycle);
    turbo_mutex_destroy(&server->mutex);
    iris_app_destroy(server->app);
    server->app = NULL;
}

static int test_set_environment(const char *name, const char *value) {
#ifdef _WIN32
    return _putenv_s(name, value ? value : "");
#else
    return value ? setenv(name, value, 1) : unsetenv(name);
#endif
}

static char *test_copy_environment(const char *name) {
    const char *value = getenv(name);
    size_t size;
    char *copy;
    if (!value) return NULL;
    size = strlen(value) + 1u;
    copy = (char *)malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}

static void test_restore_environment(const char *name, const char *value) {
    (void)test_set_environment(name, value);
}

typedef struct test_sender_s {
    int calls;
    int ledger_state;
    iris_command_identity_t ledger_identity;
    char ledger_resource_id[IRIS_COMMAND_RESOURCE_ID_CAPACITY];
    iris_command_terminal_outcome_t ledger_terminal;
} test_sender_t;

typedef struct test_post_s {
    atomic_int calls;
    atomic_int blocked;
    atomic_int release;
    int statuses[8];
    int status_count;
    char urls[8][1024];
    char bodies[8][8192];
} test_post_t;

typedef struct test_flowmq_delivery_s {
    atomic_int completion_calls;
    atomic_int event_calls;
    int fail_first_completion;
    int terminal_completion_conflict;
    char completion_message_ids[2][TEST_EVENT_ID_CAPACITY];
    char completion_times[2][40];
    char event_message_id[TEST_EVENT_ID_CAPACITY];
    char event_time[40];
    uint64_t ack_timeout_ms;
} test_flowmq_delivery_t;

typedef struct test_delivery_observer_s {
    iris_completion_dispatcher_t *dispatcher;
    atomic_int calls;
    atomic_int stats_read;
    atomic_int abandoned_seen;
    atomic_uint_fast64_t abandoned_token;
} test_delivery_observer_t;

typedef struct test_stop_context_s {
    iris_completion_dispatcher_t *dispatcher;
} test_stop_context_t;

static void test_delivery_result(
    void *context, const ivr_media_event_t *event, uint64_t delivery_token,
    iris_event_delivery_outcome_t outcome, int http_status) {
    test_delivery_observer_t *observer =
        (test_delivery_observer_t *)context;
    iris_completion_dispatcher_stats_t stats;
    (void)event;
    (void)http_status;
    iris_completion_dispatcher_get_stats(observer->dispatcher, &stats);
    atomic_store(&observer->stats_read, 1);
    if (outcome == IRIS_EVENT_DELIVERY_ABANDONED) {
        atomic_store(&observer->abandoned_token, delivery_token);
        atomic_store(&observer->abandoned_seen, 1);
    }
    atomic_fetch_add(&observer->calls, 1);
}

static void test_stop_dispatcher(void *context) {
    test_stop_context_t *stop = (test_stop_context_t *)context;
    iris_completion_dispatcher_stop(stop->dispatcher);
}

static uint64_t test_now_ms(void *context) {
    (void)context;
    return UINT64_C(1000);
}

static ivr_status_t test_send(void *context,
                              const ivr_media_command_t *command,
                              char *out_worker_id, size_t capacity) {
    test_sender_t *sender = (test_sender_t *)context;
    (void)command;
    sender->calls++;
    if (capacity < sizeof("media-worker-1")) return IVR_ENOSPC;
    memcpy(out_worker_id, "media-worker-1", sizeof("media-worker-1"));
    return IVR_OK;
}

static ivr_status_t test_ledger_claim(
    void *context, const iris_command_identity_t *identity,
    iris_command_claim_result_t *result) {
    test_sender_t *sender = (test_sender_t *)context;
    memset(result, 0, sizeof(*result));
    if (sender->ledger_state == 0) {
        sender->ledger_state = 1;
        sender->ledger_identity = *identity;
        result->disposition = IRIS_COMMAND_CLAIM_EXECUTE;
        return IVR_OK;
    }
    if (strcmp(sender->ledger_identity.command_id, identity->command_id) != 0) {
        return IVR_ENOSPC;
    }
    if (strcmp(sender->ledger_identity.semantic_fingerprint,
               identity->semantic_fingerprint) != 0) {
        result->disposition = IRIS_COMMAND_CLAIM_CONFLICT;
    } else if (sender->ledger_state == 1) {
        result->disposition = IRIS_COMMAND_CLAIM_IN_PROGRESS;
    } else if (sender->ledger_state == 2) {
        result->disposition = IRIS_COMMAND_CLAIM_REPLAY_ACCEPTED;
        memcpy(result->provider_resource_id, sender->ledger_resource_id,
               sizeof(result->provider_resource_id));
    } else if (sender->ledger_state == 3) {
        result->disposition = IRIS_COMMAND_CLAIM_REPLAY_TERMINAL;
        memcpy(result->terminal_status,
               sender->ledger_terminal.terminal_status,
               sizeof(result->terminal_status));
        memcpy(result->event_type, sender->ledger_terminal.event_type,
               sizeof(result->event_type));
        memcpy(result->result_json, sender->ledger_terminal.result_json,
               sizeof(result->result_json));
    } else {
        result->disposition = IRIS_COMMAND_CLAIM_OUTCOME_UNKNOWN;
    }
    return IVR_OK;
}

static ivr_status_t test_ledger_commit_accepted(
    void *context, const iris_command_identity_t *identity,
    const char *provider_resource_id) {
    test_sender_t *sender = (test_sender_t *)context;
    if (sender->ledger_state != 1 ||
        strcmp(sender->ledger_identity.command_id, identity->command_id) != 0 ||
        strlen(provider_resource_id) >= sizeof(sender->ledger_resource_id)) {
        return IVR_ESTATE;
    }
    memcpy(sender->ledger_resource_id, provider_resource_id,
           strlen(provider_resource_id) + 1u);
    sender->ledger_state = 2;
    return IVR_OK;
}

static ivr_status_t test_ledger_commit_terminal(
    void *context, const iris_command_identity_t *identity,
    const iris_command_terminal_outcome_t *outcome) {
    test_sender_t *sender = (test_sender_t *)context;
    if (strcmp(sender->ledger_identity.command_id, identity->command_id) != 0) {
        return IVR_ESTATE;
    }
    if (sender->ledger_state == 3) {
        return strcmp(sender->ledger_terminal.terminal_status,
                      outcome->terminal_status) == 0 &&
                       strcmp(sender->ledger_terminal.event_type,
                              outcome->event_type) == 0 &&
                       strcmp(sender->ledger_terminal.result_json,
                              outcome->result_json) == 0
                   ? IVR_OK
                   : IVR_ESTATE;
    }
    if (sender->ledger_state != 2) return IVR_ESTATE;
    sender->ledger_terminal = *outcome;
    sender->ledger_state = 3;
    return IVR_OK;
}

static ivr_status_t test_ledger_mark_unknown(
    void *context, const iris_command_identity_t *identity) {
    test_sender_t *sender = (test_sender_t *)context;
    if (sender->ledger_state == 0 ||
        strcmp(sender->ledger_identity.command_id, identity->command_id) != 0) {
        return IVR_ESTATE;
    }
    sender->ledger_state = 4;
    return IVR_OK;
}

static ivr_status_t test_ledger_abort(
    void *context, const iris_command_identity_t *identity) {
    test_sender_t *sender = (test_sender_t *)context;
    if (sender->ledger_state != 1 ||
        strcmp(sender->ledger_identity.command_id, identity->command_id) != 0) {
        return IVR_ESTATE;
    }
    sender->ledger_state = 0;
    memset(&sender->ledger_identity, 0, sizeof(sender->ledger_identity));
    return IVR_OK;
}

static ivr_status_t test_ledger_resource_seen(
    void *context, const iris_command_identity_t *identity, int *seen) {
    (void)context;
    (void)identity;
    *seen = 0;
    return IVR_OK;
}

static int test_post(void *context, const char *url,
                     const char *authorization, const char *body,
                     size_t body_size) {
    test_post_t *post = (test_post_t *)context;
    int index = atomic_fetch_add(&post->calls, 1);
    (void)authorization;
    if (index < 8) {
        snprintf(post->urls[index], sizeof(post->urls[index]), "%s", url);
        snprintf(post->bodies[index], sizeof(post->bodies[index]), "%.*s",
                 (int)body_size, body);
    }
    if (atomic_load(&post->blocked)) {
        while (!atomic_load(&post->release)) turbo_sleep_ms(1u);
    }
    return index < post->status_count ? post->statuses[index] : 200;
}

static ivr_status_t test_deliver_completion(
    void *context, const iris_media_completion_t *completion,
    const ivr_media_command_result_t *result, const char *message_id,
    const char *completed_at, uint64_t completed_at_unix_ms,
    uint64_t ack_timeout_ms) {
    test_flowmq_delivery_t *delivery = (test_flowmq_delivery_t *)context;
    int index = atomic_load_explicit(&delivery->completion_calls,
                                     memory_order_relaxed);
    (void)completion;
    (void)result;
    (void)completed_at_unix_ms;
    if (index < 2) {
        snprintf(delivery->completion_message_ids[index],
                 sizeof(delivery->completion_message_ids[index]), "%s",
                 message_id);
        snprintf(delivery->completion_times[index],
                 sizeof(delivery->completion_times[index]), "%s",
                 completed_at);
    }
    delivery->ack_timeout_ms = ack_timeout_ms;
    atomic_store_explicit(&delivery->completion_calls, index + 1,
                          memory_order_release);
    if (delivery->terminal_completion_conflict) return IVR_ESTALE;
    return delivery->fail_first_completion && index == 0 ? IVR_EBUSY
                                                         : IVR_OK;
}

static ivr_status_t test_deliver_event(
    void *context, const ivr_media_event_t *event, const char *message_id,
    const char *occurred_at, uint64_t ack_timeout_ms) {
    test_flowmq_delivery_t *delivery = (test_flowmq_delivery_t *)context;
    (void)event;
    snprintf(delivery->event_message_id,
             sizeof(delivery->event_message_id), "%s", message_id);
    snprintf(delivery->event_time, sizeof(delivery->event_time), "%s",
             occurred_at);
    delivery->ack_timeout_ms = ack_timeout_ms;
    atomic_fetch_add(&delivery->event_calls, 1);
    return IVR_OK;
}

static ivr_status_t test_observe(
    void *context, const ivr_media_command_t *command,
    iris_resource_observation_t *observation) {
    (void)context;
    (void)command;
    memset(observation, 0, sizeof(*observation));
    observation->state = IRIS_RESOURCE_OBSERVATION_UNKNOWN;
    return IVR_OK;
}

static iris_media_bridge_t *create_bridge(test_sender_t *sender,
                                          size_t capacity) {
    iris_media_bridge_config_t config;
    memset(&config, 0, sizeof(config));
    config.correlation_capacity = capacity;
    config.send = test_send;
    config.send_context = sender;
    config.observe = test_observe;
    config.observe_context = sender;
    config.realtime_ms = test_now_ms;
    config.ledger.context = sender;
    config.ledger.claim = test_ledger_claim;
    config.ledger.commit_accepted = test_ledger_commit_accepted;
    config.ledger.commit_terminal = test_ledger_commit_terminal;
    config.ledger.mark_unknown = test_ledger_mark_unknown;
    config.ledger.resource_seen = test_ledger_resource_seen;
    config.ledger.abort_intent = test_ledger_abort;
    return iris_media_bridge_create(&config);
}

static iris_completion_dispatcher_t *create_dispatcher(
    iris_media_bridge_t *bridge, test_post_t *post, size_t capacity,
    int retry_attempts) {
    iris_completion_dispatcher_config_t config;
    memset(&config, 0, sizeof(config));
    config.base_url = "http://127.0.0.1:19999";
    config.provider_token = "provider-token";
    config.queue_capacity = capacity;
    config.retry_max_attempts = retry_attempts;
    config.retry_backoff_ms = 1;
    config.request_timeout_ms = 100;
    config.drain_timeout_ms = 1000;
    config.bridge = bridge;
    config.post = test_post;
    config.post_context = post;
    return iris_completion_dispatcher_create(&config);
}

static iris_completion_dispatcher_t *create_flowmq_dispatcher(
    iris_media_bridge_t *bridge, test_flowmq_delivery_t *delivery,
    int retry_attempts) {
    iris_completion_dispatcher_config_t config;
    memset(&config, 0, sizeof(config));
    config.queue_capacity = 2u;
    config.retry_max_attempts = retry_attempts;
    config.retry_backoff_ms = 1;
    config.request_timeout_ms = 250;
    config.drain_timeout_ms = 1000;
    config.bridge = bridge;
    config.deliver_completion = test_deliver_completion;
    config.deliver_event = test_deliver_event;
    config.deliver_context = delivery;
    return iris_completion_dispatcher_create(&config);
}

static void dispatch_command(iris_media_bridge_t *bridge,
                             const char *command_id, const char *epoch) {
    char body[2048];
    iris_media_bridge_result_t result;
    snprintf(body, sizeof(body),
             "{\"schemaVersion\":2,\"commandId\":\"%s\","
             "\"tenantId\":\"tenant-a\",\"sessionId\":\"session-a\","
             "\"type\":\"media.play\",\"provider\":\"turbomedia\","
             "\"correlationId\":\"call-a\",\"causationId\":\"cause-a\","
             "\"deadline\":\"2099-01-01T00:00:00Z\","
             "\"workerId\":\"iris-worker-a\",\"dispatchEpoch\":%s,"
             "\"data\":{\"capability\":\"ivr\",\"dialogId\":\"dialog-a\","
             "\"roomId\":\"room-a\","
             "\"callId\":\"call-a\",\"callGeneration\":1,"
             "\"operationGeneration\":1,\"text\":\"Welcome\"}}",
             command_id, epoch);
    result = iris_media_bridge_dispatch_json(bridge, command_id, body,
                                             strlen(body));
    check_true(result.status == IRIS_MEDIA_BRIDGE_ACCEPTED ||
               result.status == IRIS_MEDIA_BRIDGE_DUPLICATE ||
               result.status == IRIS_MEDIA_BRIDGE_TERMINAL_REPLAY);
}

static void reclaim_command(iris_media_bridge_t *bridge,
                            const char *command_id, const char *worker,
                            const char *epoch) {
    char body[2048];
    iris_media_bridge_result_t result;
    snprintf(body, sizeof(body),
             "{\"schemaVersion\":2,\"commandId\":\"%s\","
             "\"tenantId\":\"tenant-a\",\"sessionId\":\"session-a\","
             "\"type\":\"media.play\",\"provider\":\"turbomedia\","
             "\"correlationId\":\"call-a\",\"causationId\":\"cause-a\","
             "\"deadline\":\"2099-01-01T00:00:00Z\","
             "\"workerId\":\"%s\",\"dispatchEpoch\":%s,"
             "\"data\":{\"capability\":\"ivr\",\"dialogId\":\"dialog-a\","
             "\"roomId\":\"room-a\","
             "\"callId\":\"call-a\",\"callGeneration\":1,"
             "\"operationGeneration\":1,\"text\":\"Welcome\"}}",
             command_id, worker, epoch);
    result = iris_media_bridge_dispatch_json(bridge, command_id, body,
                                             strlen(body));
    check_int_eq(result.status, IRIS_MEDIA_BRIDGE_TERMINAL_REPLAY);
}

static ivr_media_command_result_t make_result(const char *command_id) {
    ivr_media_command_result_t result;
    memset(&result, 0, sizeof(result));
    snprintf(result.message_id, sizeof(result.message_id), "%s", command_id);
    snprintf(result.tenant_id, sizeof(result.tenant_id), "tenant-a");
    snprintf(result.provider_session_id, sizeof(result.provider_session_id),
             "session-a");
    snprintf(result.dialog_id, sizeof(result.dialog_id), "dialog-a");
    snprintf(result.worker_id, sizeof(result.worker_id), "media-worker-1");
    snprintf(result.room_id, sizeof(result.room_id), "room-a");
    snprintf(result.call_id, sizeof(result.call_id), "call-a");
    result.call_generation = 1u;
    result.operation_generation = 1u;
    result.status_code = IVR_OK;
    return result;
}

static int wait_calls(test_post_t *post, int expected) {
    for (int i = 0; i < 1000; ++i) {
        if (atomic_load(&post->calls) >= expected) return 1;
        turbo_sleep_ms(1u);
    }
    return 0;
}

spec("Iris completion dispatcher") {
    it("does not retry a terminal FlowMQ completion conflict") {
        test_sender_t sender = {0};
        test_flowmq_delivery_t delivery;
        ivr_media_command_result_t result = make_result("command-conflict");
        iris_completion_dispatcher_stats_t stats;
        iris_media_bridge_t *bridge = create_bridge(&sender, 1u);
        iris_completion_dispatcher_t *dispatcher;
        memset(&delivery, 0, sizeof(delivery));
        delivery.terminal_completion_conflict = 1;
        dispatcher = create_flowmq_dispatcher(bridge, &delivery, 5);
        dispatch_command(bridge, "command-conflict", "41");
        check_not_null(dispatcher);
        check_int_eq(iris_completion_dispatcher_start(dispatcher), 0);
        check_int_eq(iris_completion_dispatcher_on_media_result(dispatcher,
                                                                &result),
                     IVR_OK);
        for (int i = 0;
             i < 1000 && atomic_load(&delivery.completion_calls) < 1; ++i) {
            turbo_sleep_ms(1u);
        }
        iris_completion_dispatcher_stop(dispatcher);
        check_int_eq(atomic_load(&delivery.completion_calls), 1);
        iris_completion_dispatcher_get_stats(dispatcher, &stats);
        check_ull_eq(stats.retries_total, 0u);
        check_ull_eq(stats.fence_conflicts_total, 1u);
        check_ull_eq(stats.completion_failure_total, 1u);
        dispatch_command(bridge, "command-conflict", "41");
        check_int_eq(sender.calls, 1);
        iris_completion_dispatcher_destroy(dispatcher);
        iris_media_bridge_destroy(bridge);
    }

    it("delivers stable completion and event identities through FlowMQ callbacks") {
        test_sender_t sender = {0};
        test_flowmq_delivery_t delivery;
        ivr_media_command_result_t result = make_result("command-flowmq");
        ivr_media_event_t event;
        iris_media_bridge_t *bridge = create_bridge(&sender, 1u);
        iris_completion_dispatcher_t *dispatcher;
        memset(&delivery, 0, sizeof(delivery));
        delivery.fail_first_completion = 1;
        memset(&event, 0, sizeof(event));
        snprintf(event.event_id, sizeof(event.event_id), "event-flowmq");
        snprintf(event.tenant_id, sizeof(event.tenant_id), "tenant-a");
        snprintf(event.provider_session_id,
                 sizeof(event.provider_session_id), "session-a");
        snprintf(event.dialog_id, sizeof(event.dialog_id), "dialog-a");
        snprintf(event.event_type, sizeof(event.event_type),
                 "provider.media.input");
        event.sequence = 7u;
        event.occurred_at_ms = UINT64_C(2000);
        dispatcher = create_flowmq_dispatcher(bridge, &delivery, 2);
        dispatch_command(bridge, "command-flowmq", "41");
        check_not_null(dispatcher);
        check_int_eq(iris_completion_dispatcher_start(dispatcher), 0);
        check_int_eq(iris_completion_dispatcher_on_media_result(dispatcher,
                                                                &result),
                     IVR_OK);
        for (int i = 0;
             i < 1000 && atomic_load(&delivery.completion_calls) < 2; ++i) {
            turbo_sleep_ms(1u);
        }
        check_int_eq(atomic_load(&delivery.completion_calls), 2);
        check_str_eq(delivery.completion_message_ids[0],
                     delivery.completion_message_ids[1]);
        check_str_eq(delivery.completion_times[0],
                     delivery.completion_times[1]);
        check_int_eq(iris_completion_dispatcher_on_media_event(dispatcher,
                                                               &event),
                     IVR_OK);
        for (int i = 0;
             i < 1000 && atomic_load(&delivery.event_calls) < 1; ++i) {
            turbo_sleep_ms(1u);
        }
        iris_completion_dispatcher_stop(dispatcher);
        check_int_eq(atomic_load(&delivery.event_calls), 1);
        check_str_eq(delivery.event_message_id, "event-flowmq");
        check_true(delivery.event_time[0] != '\0');
        check_ull_eq(delivery.ack_timeout_ms, UINT64_C(250));
        iris_completion_dispatcher_destroy(dispatcher);
        iris_media_bridge_destroy(bridge);
    }

    it("posts an owning command completion and keeps a duplicate tombstone") {
        test_sender_t sender = {0};
        test_post_t post;
        ivr_media_command_result_t result = make_result("command-a");
        memset(&post, 0, sizeof(post));
        post.statuses[0] = 200;
        post.status_count = 1;
        iris_media_bridge_t *bridge = create_bridge(&sender, 1u);
        iris_completion_dispatcher_t *dispatcher =
            create_dispatcher(bridge, &post, 1u, 1);
        dispatch_command(bridge, "command-a", "41");
        check_not_null(dispatcher);
        check_int_eq(iris_completion_dispatcher_start(dispatcher), 0);
        check_int_eq(iris_completion_dispatcher_on_media_result(dispatcher,
                                                                &result), IVR_OK);
        memset(&result, 0, sizeof(result));
        check_true(wait_calls(&post, 1));
        iris_completion_dispatcher_stop(dispatcher);
        check_str_contains(post.urls[0], "/commands/command-a/completions");
        check_str_contains(post.bodies[0], "\"workerId\":\"iris-worker-a\"");
        check_str_contains(post.bodies[0], "\"expectedDispatchEpoch\":41");
        check_str_contains(post.bodies[0], "\"mediaWorkerId\":\"media-worker-1\"");
        dispatch_command(bridge, "command-a", "41");
        check_int_eq(sender.calls, 1);
        iris_completion_dispatcher_destroy(dispatcher);
        iris_media_bridge_destroy(bridge);
    }

    it("retries a stable completion body after a retryable response") {
        test_sender_t sender = {0};
        test_post_t post;
        ivr_media_command_result_t result = make_result("command-a");
        memset(&post, 0, sizeof(post));
        post.statuses[0] = 500;
        post.statuses[1] = 200;
        post.status_count = 2;
        iris_media_bridge_t *bridge = create_bridge(&sender, 1u);
        iris_completion_dispatcher_t *dispatcher =
            create_dispatcher(bridge, &post, 1u, 2);
        dispatch_command(bridge, "command-a", "41");
        check_int_eq(iris_completion_dispatcher_start(dispatcher), 0);
        check_int_eq(iris_completion_dispatcher_on_media_result(dispatcher,
                                                                &result), IVR_OK);
        check_true(wait_calls(&post, 2));
        iris_completion_dispatcher_stop(dispatcher);
        check_str_eq(post.bodies[0], post.bodies[1]);
        {
            iris_completion_dispatcher_stats_t stats;
            iris_completion_dispatcher_get_stats(dispatcher, &stats);
            check_size_eq(stats.queue_capacity, 1u);
            check_size_eq(stats.queue_high_water, 1u);
            check_ull_eq(stats.enqueued_total, 1u);
            check_ull_eq(stats.delivery_attempts_total, 2u);
            check_ull_eq(stats.retries_total, 1u);
            check_ull_eq(stats.completion_success_total, 1u);
            check_ull_eq(stats.completion_failure_total, 0u);
        }
        iris_completion_dispatcher_destroy(dispatcher);
        iris_media_bridge_destroy(bridge);
    }

    it("refreshes the Iris fence before retrying a stale completion") {
        test_sender_t sender = {0};
        test_post_t post;
        ivr_media_command_result_t result = make_result("command-a");
        memset(&post, 0, sizeof(post));
        post.statuses[0] = 409;
        post.statuses[1] = 200;
        post.status_count = 2;
        atomic_store(&post.blocked, 1);
        iris_media_bridge_t *bridge = create_bridge(&sender, 1u);
        iris_completion_dispatcher_t *dispatcher =
            create_dispatcher(bridge, &post, 1u, 2);
        dispatch_command(bridge, "command-a", "41");
        check_int_eq(iris_completion_dispatcher_start(dispatcher), 0);
        check_int_eq(iris_completion_dispatcher_on_media_result(dispatcher,
                                                                &result), IVR_OK);
        check_true(wait_calls(&post, 1));
        reclaim_command(bridge, "command-a", "iris-worker-b", "42");
        atomic_store(&post.release, 1);
        check_true(wait_calls(&post, 2));
        iris_completion_dispatcher_stop(dispatcher);
        check_str_contains(post.bodies[0], "\"expectedDispatchEpoch\":41");
        check_str_contains(post.bodies[1], "\"expectedDispatchEpoch\":42");
        check_str_contains(post.bodies[1], "\"workerId\":\"iris-worker-b\"");
        {
            iris_completion_dispatcher_stats_t stats;
            iris_completion_dispatcher_get_stats(dispatcher, &stats);
            check_ull_eq(stats.delivery_attempts_total, 2u);
            check_ull_eq(stats.retries_total, 1u);
            check_ull_eq(stats.fence_conflicts_total, 1u);
            check_ull_eq(stats.fence_refresh_failures_total, 0u);
        }
        iris_completion_dispatcher_destroy(dispatcher);
        iris_media_bridge_destroy(bridge);
    }

    it("restores the correlation after retry exhaustion") {
        test_sender_t sender = {0};
        test_post_t post;
        ivr_media_command_result_t result = make_result("command-a");
        memset(&post, 0, sizeof(post));
        post.statuses[0] = 503;
        post.statuses[1] = 200;
        post.status_count = 2;
        iris_media_bridge_t *bridge = create_bridge(&sender, 1u);
        iris_completion_dispatcher_t *dispatcher =
            create_dispatcher(bridge, &post, 1u, 1);
        dispatch_command(bridge, "command-a", "41");
        check_int_eq(iris_completion_dispatcher_start(dispatcher), 0);
        check_int_eq(iris_completion_dispatcher_on_media_result(dispatcher,
                                                                &result), IVR_OK);
        check_true(wait_calls(&post, 1));
        turbo_sleep_ms(5u);
        check_int_eq(iris_completion_dispatcher_on_media_result(dispatcher,
                                                                &result), IVR_OK);
        check_true(wait_calls(&post, 2));
        iris_completion_dispatcher_stop(dispatcher);
        {
            iris_completion_dispatcher_stats_t stats;
            iris_completion_dispatcher_get_stats(dispatcher, &stats);
            check_ull_eq(stats.enqueued_total, 2u);
            check_ull_eq(stats.delivery_attempts_total, 2u);
            check_ull_eq(stats.completion_success_total, 1u);
            check_ull_eq(stats.completion_failure_total, 1u);
        }
        iris_completion_dispatcher_destroy(dispatcher);
        iris_media_bridge_destroy(bridge);
    }

    it("rejects a full queue and drains accepted owning events") {
        test_sender_t sender = {0};
        test_post_t post;
        ivr_media_event_t first;
        ivr_media_event_t second;
        ivr_media_event_t third;
        memset(&post, 0, sizeof(post));
        atomic_store(&post.blocked, 1);
        memset(&first, 0, sizeof(first));
        snprintf(first.event_id, sizeof(first.event_id), "event-1");
        snprintf(first.provider_session_id, sizeof(first.provider_session_id),
                 "session-a");
        snprintf(first.dialog_id, sizeof(first.dialog_id), "dialog-a");
        snprintf(first.event_type, sizeof(first.event_type),
                 "provider.media.input");
        snprintf(first.call_id, sizeof(first.call_id), "call-a");
        snprintf(first.input_id, sizeof(first.input_id), "input-a");
        snprintf(first.input_value, sizeof(first.input_value), "5");
        first.occurred_at_ms = UINT64_C(2000);
        snprintf(first.payload_json, sizeof(first.payload_json),
                 "{\"value\":\"1\"}");
        second = first;
        third = first;
        snprintf(second.event_id, sizeof(second.event_id), "event-2");
        snprintf(third.event_id, sizeof(third.event_id), "event-3");
        iris_media_bridge_t *bridge = create_bridge(&sender, 1u);
        iris_completion_dispatcher_t *dispatcher =
            create_dispatcher(bridge, &post, 1u, 1);
        check_int_eq(iris_completion_dispatcher_start(dispatcher), 0);
        check_int_eq(iris_completion_dispatcher_on_media_event(dispatcher, &first),
                     IVR_OK);
        check_true(wait_calls(&post, 1));
        check_int_eq(iris_completion_dispatcher_on_media_event(dispatcher, &second),
                     IVR_OK);
        check_int_eq(iris_completion_dispatcher_on_media_event(dispatcher, &third),
                     IVR_ENOSPC);
        memset(&second, 0, sizeof(second));
        atomic_store(&post.release, 1);
        iris_completion_dispatcher_stop(dispatcher);
        check_int_eq(atomic_load(&post.calls), 2);
        check_str_contains(post.bodies[1], "\"eventId\":\"event-2\"");
        check_str_contains(post.bodies[1], "\"inputId\":\"input-a\"");
        check_str_contains(post.bodies[1], "\"inputValue\":\"5\"");
        {
            iris_completion_dispatcher_stats_t stats;
            iris_completion_dispatcher_get_stats(dispatcher, &stats);
            check_size_eq(stats.queue_items, 0u);
            check_size_eq(stats.queue_capacity, 1u);
            check_size_eq(stats.queue_high_water, 1u);
            check_ull_eq(stats.enqueued_total, 2u);
            check_ull_eq(stats.queue_full_total, 1u);
            check_ull_eq(stats.event_success_total, 2u);
        }
        iris_completion_dispatcher_destroy(dispatcher);
        iris_media_bridge_destroy(bridge);
    }

    it("reports shutdown-abandoned durable events outside the queue lock") {
        test_sender_t sender = {0};
        test_post_t post;
        test_delivery_observer_t observer;
        test_stop_context_t stop_context;
        turbo_thread_t stop_thread;
        ivr_media_event_t first;
        ivr_media_event_t second;
        iris_media_bridge_t *bridge;
        iris_completion_dispatcher_t *dispatcher;
        memset(&post, 0, sizeof(post));
        memset(&observer, 0, sizeof(observer));
        memset(&stop_context, 0, sizeof(stop_context));
        memset(&first, 0, sizeof(first));
        snprintf(first.event_id, sizeof(first.event_id), "event-stop-1");
        snprintf(first.provider_session_id,
                 sizeof(first.provider_session_id), "session-a");
        snprintf(first.dialog_id, sizeof(first.dialog_id), "dialog-a");
        snprintf(first.event_type, sizeof(first.event_type),
                 "provider.media.input");
        first.occurred_at_ms = UINT64_C(2000);
        second = first;
        snprintf(second.event_id, sizeof(second.event_id), "event-stop-2");
        atomic_store(&post.blocked, 1);
        bridge = create_bridge(&sender, 1u);
        dispatcher = create_dispatcher(bridge, &post, 1u, 1);
        observer.dispatcher = dispatcher;
        stop_context.dispatcher = dispatcher;
        check_int_eq(iris_completion_dispatcher_set_event_delivery_observer(
                         dispatcher, test_delivery_result, &observer),
                     0);
        check_int_eq(iris_completion_dispatcher_start(dispatcher), 0);
        check_int_eq(iris_completion_dispatcher_enqueue_event(
                         dispatcher, &first, UINT64_C(11)),
                     IVR_OK);
        check_true(wait_calls(&post, 1));
        check_int_eq(iris_completion_dispatcher_enqueue_event(
                         dispatcher, &second, UINT64_C(22)),
                     IVR_OK);
        check_int_eq(turbo_thread_create(&stop_thread, test_stop_dispatcher,
                                         &stop_context),
                     0);
        for (int i = 0; i < 2000 && !atomic_load(&observer.abandoned_seen);
             ++i) {
            turbo_sleep_ms(1u);
        }
        check_true(atomic_load(&observer.abandoned_seen));
        check_true(atomic_load(&observer.stats_read));
        check_true(atomic_load(&observer.abandoned_token) == UINT64_C(22));
        atomic_store(&post.release, 1);
        turbo_thread_join(&stop_thread);
        turbo_thread_destroy(&stop_thread);
        check_int_eq(atomic_load(&observer.calls), 2);
        iris_completion_dispatcher_destroy(dispatcher);
        iris_media_bridge_destroy(bridge);
    }

    it("resets the stop deadline when restarted") {
        test_sender_t sender = {0};
        test_post_t post;
        ivr_media_event_t event;
        iris_completion_dispatcher_stats_t stats;
        memset(&post, 0, sizeof(post));
        memset(&event, 0, sizeof(event));
        snprintf(event.event_id, sizeof(event.event_id), "event-restart");
        snprintf(event.provider_session_id, sizeof(event.provider_session_id),
                 "session-a");
        snprintf(event.dialog_id, sizeof(event.dialog_id), "dialog-a");
        snprintf(event.event_type, sizeof(event.event_type),
                 "provider.media.input");
        event.occurred_at_ms = UINT64_C(2000);
        iris_media_bridge_t *bridge = create_bridge(&sender, 1u);
        iris_completion_dispatcher_t *dispatcher =
            create_dispatcher(bridge, &post, 1u, 1);
        check_int_eq(iris_completion_dispatcher_on_media_event(dispatcher, &event),
                     IVR_ECLOSED);
        check_int_eq(iris_completion_dispatcher_start(dispatcher), 0);
        iris_completion_dispatcher_stop(dispatcher);
        check_int_eq(iris_completion_dispatcher_start(dispatcher), 0);
        check_int_eq(iris_completion_dispatcher_on_media_event(dispatcher, &event),
                     IVR_OK);
        check_true(wait_calls(&post, 1));
        iris_completion_dispatcher_stop(dispatcher);
        iris_completion_dispatcher_get_stats(dispatcher, &stats);
        check_ull_eq(stats.event_success_total, 1u);
        check_ull_eq(stats.event_failure_total, 0u);
        check_ull_eq(stats.closed_rejections_total, 1u);
        iris_completion_dispatcher_destroy(dispatcher);
        iris_media_bridge_destroy(bridge);
    }

    it("classifies an exhausted media event delivery") {
        test_sender_t sender = {0};
        test_post_t post;
        ivr_media_event_t event;
        iris_completion_dispatcher_stats_t stats;
        memset(&post, 0, sizeof(post));
        post.statuses[0] = 503;
        post.status_count = 1;
        memset(&event, 0, sizeof(event));
        snprintf(event.event_id, sizeof(event.event_id), "event-failed");
        snprintf(event.provider_session_id, sizeof(event.provider_session_id),
                 "session-a");
        snprintf(event.dialog_id, sizeof(event.dialog_id), "dialog-a");
        snprintf(event.event_type, sizeof(event.event_type),
                 "provider.media.input");
        event.occurred_at_ms = UINT64_C(2000);
        iris_media_bridge_t *bridge = create_bridge(&sender, 1u);
        iris_completion_dispatcher_t *dispatcher =
            create_dispatcher(bridge, &post, 1u, 1);
        check_int_eq(iris_completion_dispatcher_start(dispatcher), 0);
        check_int_eq(iris_completion_dispatcher_on_media_event(dispatcher, &event),
                     IVR_OK);
        check_true(wait_calls(&post, 1));
        iris_completion_dispatcher_stop(dispatcher);
        iris_completion_dispatcher_get_stats(dispatcher, &stats);
        check_ull_eq(stats.delivery_attempts_total, 1u);
        check_ull_eq(stats.event_success_total, 0u);
        check_ull_eq(stats.event_failure_total, 1u);
        iris_completion_dispatcher_destroy(dispatcher);
        iris_media_bridge_destroy(bridge);
    }

    it("delivers an authenticated event through the default TurboHTTP TLS path") {
        test_tls_server_t tls_server;
        test_sender_t sender = {0};
        iris_media_bridge_t *bridge = NULL;
        iris_completion_dispatcher_t *wrong_host_dispatcher = NULL;
        iris_completion_dispatcher_t *dispatcher = NULL;
        iris_completion_dispatcher_config_t config;
        iris_completion_dispatcher_stats_t stats;
        ivr_media_event_t event;
        char base_url[128];
        char *saved_ca_file =
            test_copy_environment("TURBONET_TLS_CA_FILE");
        char *saved_ca_path =
            test_copy_environment("TURBONET_TLS_CA_PATH");
        int server_started = 0;
        int dispatcher_started = 0;

        check_int_eq(test_set_environment("TURBONET_TLS_CA_FILE",
                                          ROOM_SERVICE_TEST_TLS_CERT_PATH),
                     0);
        check_int_eq(test_set_environment("TURBONET_TLS_CA_PATH", NULL), 0);
        server_started = test_tls_server_start(&tls_server) == 0;
        check_true(server_started);
        if (server_started) {
            bridge = create_bridge(&sender, 1u);
            check_not_null(bridge);
        }
        if (bridge) {
            memset(&config, 0, sizeof(config));
            snprintf(base_url, sizeof(base_url), "https://127.0.0.1:%u",
                     TEST_TLS_LOOPBACK_PORT);
            config.base_url = base_url;
            config.provider_token = "provider-token";
            config.queue_capacity = 1u;
            config.retry_max_attempts = 1;
            config.retry_backoff_ms = 1;
            config.request_timeout_ms = 3000;
            config.drain_timeout_ms = 5000;
            config.bridge = bridge;
            wrong_host_dispatcher =
                iris_completion_dispatcher_create(&config);
            check_not_null(wrong_host_dispatcher);
        }
        if (wrong_host_dispatcher) {
            check_int_eq(
                iris_completion_dispatcher_start(wrong_host_dispatcher), 0);
            memset(&event, 0, sizeof(event));
            snprintf(event.event_id, sizeof(event.event_id),
                     "event-wrong-host");
            snprintf(event.provider_session_id,
                     sizeof(event.provider_session_id), "session-a");
            snprintf(event.dialog_id, sizeof(event.dialog_id), "dialog-a");
            snprintf(event.event_type, sizeof(event.event_type),
                     "provider.media.input");
            snprintf(event.call_id, sizeof(event.call_id), "call-a");
            event.occurred_at_ms = UINT64_C(2000);
            check_int_eq(iris_completion_dispatcher_on_media_event(
                             wrong_host_dispatcher, &event),
                         IVR_OK);
            for (int i = 0; i < 3000; ++i) {
                iris_completion_dispatcher_get_stats(wrong_host_dispatcher,
                                                     &stats);
                if (stats.event_failure_total == 1u) break;
                turbo_sleep_ms(1u);
            }
            iris_completion_dispatcher_stop(wrong_host_dispatcher);
            iris_completion_dispatcher_get_stats(wrong_host_dispatcher,
                                                 &stats);
            check_ull_eq(stats.delivery_attempts_total, 1u);
            check_ull_eq(stats.event_success_total, 0u);
            check_ull_eq(stats.event_failure_total, 1u);
            check_int_eq(atomic_load(&tls_server.calls), 0);
            iris_completion_dispatcher_destroy(wrong_host_dispatcher);
            wrong_host_dispatcher = NULL;

            snprintf(base_url, sizeof(base_url), "https://localhost:%u",
                     TEST_TLS_LOOPBACK_PORT);
            dispatcher = iris_completion_dispatcher_create(&config);
            check_not_null(dispatcher);
        }
        if (dispatcher) {
            dispatcher_started =
                iris_completion_dispatcher_start(dispatcher) == 0;
            check_true(dispatcher_started);
        }
        if (dispatcher_started) {
            memset(&event, 0, sizeof(event));
            snprintf(event.event_id, sizeof(event.event_id), "event-tls");
            snprintf(event.provider_session_id,
                     sizeof(event.provider_session_id), "session-a");
            snprintf(event.dialog_id, sizeof(event.dialog_id), "dialog-a");
            snprintf(event.event_type, sizeof(event.event_type),
                     "provider.media.input");
            snprintf(event.call_id, sizeof(event.call_id), "call-a");
            event.occurred_at_ms = UINT64_C(2000);
            snprintf(event.payload_json, sizeof(event.payload_json),
                     "{\"value\":\"tls\"}");
            check_int_eq(
                iris_completion_dispatcher_on_media_event(dispatcher, &event),
                IVR_OK);
            for (int i = 0; i < 3000 && atomic_load(&tls_server.calls) < 1;
                 ++i) {
                turbo_sleep_ms(1u);
            }
            check_int_eq(atomic_load(&tls_server.calls), 1);
            iris_completion_dispatcher_stop(dispatcher);
            dispatcher_started = 0;
            check_int_eq(atomic_load(&tls_server.request_valid), 1);
            iris_completion_dispatcher_get_stats(dispatcher, &stats);
            check_ull_eq(stats.delivery_attempts_total, 1u);
            check_ull_eq(stats.event_success_total, 1u);
            check_ull_eq(stats.event_failure_total, 0u);
        }

        if (dispatcher_started) iris_completion_dispatcher_stop(dispatcher);
        iris_completion_dispatcher_destroy(wrong_host_dispatcher);
        iris_completion_dispatcher_destroy(dispatcher);
        iris_media_bridge_destroy(bridge);
        test_tls_server_stop(&tls_server);
        test_restore_environment("TURBONET_TLS_CA_FILE", saved_ca_file);
        test_restore_environment("TURBONET_TLS_CA_PATH", saved_ca_path);
        free(saved_ca_path);
        free(saved_ca_file);
    }
}
