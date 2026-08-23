#include "iris_room_bridge.h"

#include <tinytest.h>

#include <stdio.h>
#include <string.h>

#define TEST_CAUSATION_ID "incoming-a"
#define TEST_DEADLINE "2099-01-01T00:00:00Z"

typedef struct room_executor_s {
    int calls;
    iris_room_terminal_status_t terminal_status;
    int return_invalid_result;
    int return_absent;
} room_executor_t;

static int g_ledger_context;
static int g_resource_seen;
static iris_command_claim_disposition_t g_claim_disposition;
static iris_resource_observation_state_t g_observation_state;
static int g_observe_calls;

static ivr_status_t ledger_claim(
    void *context, const iris_command_identity_t *identity,
    iris_command_claim_result_t *result) {
    (void)context;
    (void)identity;
    memset(result, 0, sizeof(*result));
    result->disposition = g_claim_disposition != 0
                              ? g_claim_disposition
                              : IRIS_COMMAND_CLAIM_EXECUTE;
    return IVR_OK;
}

static ivr_status_t ledger_commit_terminal(
    void *context, const iris_command_identity_t *identity,
    const iris_command_terminal_outcome_t *outcome) {
    (void)context;
    (void)identity;
    (void)outcome;
    return IVR_OK;
}

static ivr_status_t ledger_identity_only(
    void *context, const iris_command_identity_t *identity) {
    (void)context;
    (void)identity;
    return IVR_OK;
}

static ivr_status_t ledger_resource_seen(
    void *context, const iris_command_identity_t *identity, int *seen) {
    (void)context;
    (void)identity;
    *seen = g_resource_seen;
    return IVR_OK;
}

static uint64_t test_now_ms(void *context) {
    return *(const uint64_t *)context;
}

static int execute_room(void *context, const iris_room_command_t *command,
                        iris_room_execution_t *result) {
    room_executor_t *executor = (room_executor_t *)context;
    executor->calls++;
    if (executor->return_invalid_result) return -1;
    memset(result, 0, sizeof(*result));
    result->terminal_status = executor->return_absent
                                  ? IRIS_ROOM_TERMINAL_FAILED
                                  : executor->terminal_status;
    result->resource_absent = executor->return_absent;
    snprintf(result->event_type, sizeof(result->event_type), "%s",
             executor->terminal_status == IRIS_ROOM_TERMINAL_SUCCEEDED
                 ? "provider.conference.created"
                 : "provider.conference.failed");
    snprintf(result->data, sizeof(result->data),
             "{\"roomId\":\"%s\",\"call\":%d}",
             command->room_id, executor->calls);
    return 0;
}

static ivr_status_t observe_room(
    void *context, const iris_room_command_t *command,
    iris_resource_observation_t *observation) {
    (void)context;
    (void)command;
    g_observe_calls++;
    memset(observation, 0, sizeof(*observation));
    observation->state = g_observation_state;
    return IVR_OK;
}

static iris_room_bridge_t *create_bridge(room_executor_t *executor,
                                         const uint64_t *now_ms,
                                         size_t capacity) {
    iris_room_bridge_config_t config;
    memset(&config, 0, sizeof(config));
    config.capacity = capacity;
    config.execute = execute_room;
    config.execute_context = executor;
    config.observe = observe_room;
    config.observe_context = executor;
    config.realtime_ms = test_now_ms;
    config.realtime_context = (void *)now_ms;
    config.ledger.context = &g_ledger_context;
    config.ledger.claim = ledger_claim;
    config.ledger.commit_terminal = ledger_commit_terminal;
    config.ledger.mark_unknown = ledger_identity_only;
    config.ledger.resource_seen = ledger_resource_seen;
    config.ledger.abort_intent = ledger_identity_only;
    return iris_room_bridge_create(&config);
}

static void make_create_request(char *buffer, size_t capacity,
                                const char *command_id,
                                const char *worker_id,
                                const char *dispatch_epoch,
                                const char *room_id,
                                const char *causation_id,
                                const char *deadline) {
    snprintf(buffer, capacity,
             "{"
             "\"schemaVersion\":3,"
             "\"commandId\":\"%s\","
             "\"tenantId\":\"tenant-a\","
             "\"sessionId\":\"session-a\","
             "\"type\":\"conference.create\","
             "\"provider\":\"turbomedia\","
             "\"correlationId\":\"%s\","
             "\"causationId\":\"%s\","
             "\"deadline\":\"%s\","
             "\"workerId\":\"%s\","
             "\"dispatchEpoch\":%s,"
             "\"data\":{\"capability\":\"room\","
             "\"roomId\":\"%s\",\"roomGeneration\":1}}",
             command_id, room_id, causation_id, deadline, worker_id,
             dispatch_epoch, room_id);
}

static void make_join_request(char *buffer, size_t capacity,
                              const char *data_fields) {
    snprintf(buffer, capacity,
             "{"
             "\"schemaVersion\":3,"
             "\"commandId\":\"command-join\","
             "\"tenantId\":\"tenant-a\","
             "\"sessionId\":\"session-a\","
             "\"type\":\"connection.join\","
             "\"provider\":\"turbomedia\","
             "\"correlationId\":\"room-a\","
             "\"causationId\":\"incoming-a\","
             "\"deadline\":\"2099-01-01T00:00:00Z\","
             "\"workerId\":\"worker-a\","
             "\"dispatchEpoch\":41,"
             "\"data\":{\"capability\":\"room\",%s}}",
             data_fields);
}

static void make_destroy_request(char *buffer, size_t capacity,
                                 const char *command_id) {
    snprintf(buffer, capacity,
             "{"
             "\"schemaVersion\":3,"
             "\"commandId\":\"%s\","
             "\"tenantId\":\"tenant-a\","
             "\"sessionId\":\"session-a\","
             "\"type\":\"conference.destroy\","
             "\"provider\":\"turbomedia\","
             "\"correlationId\":\"room-a\","
             "\"causationId\":\"incoming-a\","
             "\"deadline\":\"2099-01-01T00:00:00Z\","
             "\"workerId\":\"worker-a\","
             "\"dispatchEpoch\":41,"
             "\"data\":{\"capability\":\"room\","
             "\"roomId\":\"room-a\",\"roomGeneration\":1}}",
             command_id);
}

spec("Iris room provider bridge") {
    it("caches a terminal success across a new dispatch fence") {
        const uint64_t now_ms = UINT64_C(1000);
        room_executor_t executor = {0, IRIS_ROOM_TERMINAL_SUCCEEDED};
        char first[2048];
        char retry[2048];
        iris_room_bridge_t *bridge = create_bridge(&executor, &now_ms, 2u);
        iris_room_bridge_result_t result;

        check_not_null(bridge);
        make_create_request(first, sizeof(first), "command-a", "worker-a",
                            "41", "room-a", TEST_CAUSATION_ID,
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", first, strlen(first));
        check_equal(result.status, IRIS_ROOM_BRIDGE_TERMINAL);
        check_equal(result.terminal_status, IRIS_ROOM_TERMINAL_SUCCEEDED);
        check_equal(result.event_type, "provider.conference.created");
        check_contains(result.data, "\"call\":1");

        make_create_request(retry, sizeof(retry), "command-a", "worker-b",
                            "42", "room-a", TEST_CAUSATION_ID,
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", retry, strlen(retry));
        check_equal(result.status, IRIS_ROOM_BRIDGE_DUPLICATE);
        check_contains(result.data, "\"call\":1");
        check_equal(executor.calls, 1);
        iris_room_bridge_destroy(bridge);
    }

    it("caches a terminal domain failure") {
        const uint64_t now_ms = UINT64_C(1000);
        room_executor_t executor = {0, IRIS_ROOM_TERMINAL_FAILED};
        char body[2048];
        iris_room_bridge_t *bridge = create_bridge(&executor, &now_ms, 1u);
        iris_room_bridge_result_t result;

        make_create_request(body, sizeof(body), "command-a", "worker-a",
                            "41", "room-a", TEST_CAUSATION_ID,
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_TERMINAL);
        check_equal(result.terminal_status, IRIS_ROOM_TERMINAL_FAILED);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_DUPLICATE);
        check_equal(executor.calls, 1);
        iris_room_bridge_destroy(bridge);
    }

    it("settles an admitted command when its executor result is invalid") {
        const uint64_t now_ms = UINT64_C(1000);
        room_executor_t executor = {0, IRIS_ROOM_TERMINAL_SUCCEEDED, 1};
        char body[2048];
        iris_room_bridge_t *bridge = create_bridge(&executor, &now_ms, 1u);
        iris_room_bridge_result_t result;

        make_create_request(body, sizeof(body), "command-a", "worker-a",
                            "41", "room-a", TEST_CAUSATION_ID,
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_TERMINAL);
        check_equal(result.terminal_status, IRIS_ROOM_TERMINAL_FAILED);
        check_equal(result.event_type, "provider.command.failed");
        check_contains(result.data, "ROOM_EXECUTION_FAILED");

        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_DUPLICATE);
        check_equal(executor.calls, 1);
        iris_room_bridge_destroy(bridge);
    }

    it("rejects command id reuse with different room data") {
        const uint64_t now_ms = UINT64_C(1000);
        room_executor_t executor = {0, IRIS_ROOM_TERMINAL_SUCCEEDED};
        char body[2048];
        iris_room_bridge_t *bridge = create_bridge(&executor, &now_ms, 1u);
        iris_room_bridge_result_t result;

        make_create_request(body, sizeof(body), "command-a", "worker-a",
                            "41", "room-a", TEST_CAUSATION_ID,
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_TERMINAL);
        make_create_request(body, sizeof(body), "command-a", "worker-b",
                            "42", "room-b", TEST_CAUSATION_ID,
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_INVALID);
        check_equal(result.error_code, "IDEMPOTENCY_CONFLICT");
        check_equal(executor.calls, 1);
        iris_room_bridge_destroy(bridge);
    }

    it("rejects command id reuse with a changed immutable envelope") {
        const uint64_t now_ms = UINT64_C(1000);
        room_executor_t executor = {0, IRIS_ROOM_TERMINAL_SUCCEEDED};
        char body[2048];
        iris_room_bridge_t *bridge = create_bridge(&executor, &now_ms, 2u);
        iris_room_bridge_result_t result;

        make_create_request(body, sizeof(body), "command-a", "worker-a",
                            "41", "room-a", TEST_CAUSATION_ID,
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_TERMINAL);
        make_create_request(body, sizeof(body), "command-a", "worker-b",
                            "42", "room-a", "changed-causation",
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_INVALID);
        check_equal(result.error_code, "IDEMPOTENCY_CONFLICT");

        make_create_request(body, sizeof(body), "command-b", "worker-a",
                            "43", "room-b", TEST_CAUSATION_ID,
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-b", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_TERMINAL);
        make_create_request(body, sizeof(body), "command-b", "worker-b",
                            "44", "room-b", TEST_CAUSATION_ID,
                            "2099-01-02T00:00:00Z");
        result = iris_room_bridge_dispatch_json(
            bridge, "command-b", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_INVALID);
        check_equal(result.error_code, "IDEMPOTENCY_CONFLICT");
        check_equal(executor.calls, 2);
        iris_room_bridge_destroy(bridge);
    }

    it("reuses the oldest completed tombstone at bounded capacity") {
        const uint64_t now_ms = UINT64_C(1000);
        room_executor_t executor = {0, IRIS_ROOM_TERMINAL_SUCCEEDED};
        char body[2048];
        iris_room_bridge_t *bridge = create_bridge(&executor, &now_ms, 1u);
        iris_room_bridge_result_t result;

        make_create_request(body, sizeof(body), "command-a", "worker-a",
                            "41", "room-a", TEST_CAUSATION_ID,
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_TERMINAL);
        make_create_request(body, sizeof(body), "command-b", "worker-a",
                            "42", "room-b", TEST_CAUSATION_ID,
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "command-b", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_TERMINAL);
        check_equal(executor.calls, 2);
        iris_room_bridge_destroy(bridge);
    }

    it("rejects unknown data and mismatched idempotency keys") {
        const uint64_t now_ms = UINT64_C(1000);
        room_executor_t executor = {0, IRIS_ROOM_TERMINAL_SUCCEEDED};
        char body[2048];
        char *end;
        iris_room_bridge_t *bridge = create_bridge(&executor, &now_ms, 1u);
        iris_room_bridge_result_t result;

        make_create_request(body, sizeof(body), "command-a", "worker-a",
                            "41", "room-a", TEST_CAUSATION_ID,
                            TEST_DEADLINE);
        result = iris_room_bridge_dispatch_json(
            bridge, "other-command", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_INVALID);
        end = strstr(body, "}}");
        check_not_null(end);
        if (end) {
            memmove(end + strlen(",\"unknown\":true"), end,
                    strlen(end) + 1u);
            memcpy(end, ",\"unknown\":true",
                   strlen(",\"unknown\":true"));
        }
        result = iris_room_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_INVALID);
        check_equal(executor.calls, 0);
        iris_room_bridge_destroy(bridge);
    }

    it("rejects legacy membership identifier pairs") {
        const uint64_t now_ms = UINT64_C(1000);
        room_executor_t executor = {0, IRIS_ROOM_TERMINAL_SUCCEEDED};
        char body[2048];
        iris_room_bridge_t *bridge = create_bridge(&executor, &now_ms, 1u);
        iris_room_bridge_result_t result;

        make_join_request(
            body, sizeof(body),
            "\"roomId\":\"room-a\",\"roomGeneration\":1,"
            "\"callId\":\"call-a\",\"callGeneration\":1,"
            "\"id1\":\"call-a\",\"id2\":\"room-a\"");
        result = iris_room_bridge_dispatch_json(
            bridge, "command-join", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_INVALID);
        check_equal(result.error_code, "INVALID_COMMAND_ENVELOPE");
        check_equal(executor.calls, 0);
        iris_room_bridge_destroy(bridge);
    }

    it("returns alreadyAbsent only with durable generation evidence") {
        const uint64_t now_ms = UINT64_C(1000);
        room_executor_t executor = {
            0, IRIS_ROOM_TERMINAL_FAILED, 0, 1};
        char body[2048];
        iris_room_bridge_result_t result;
        iris_room_bridge_t *bridge;

        g_resource_seen = 0;
        bridge = create_bridge(&executor, &now_ms, 1u);
        make_destroy_request(body, sizeof(body), "destroy-unproven");
        result = iris_room_bridge_dispatch_json(
            bridge, "destroy-unproven", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_TERMINAL);
        check_equal(result.terminal_status, IRIS_ROOM_TERMINAL_FAILED);
        check_false(strstr(result.data, "alreadyAbsent") != NULL);
        iris_room_bridge_destroy(bridge);

        g_resource_seen = 1;
        bridge = create_bridge(&executor, &now_ms, 1u);
        make_destroy_request(body, sizeof(body), "destroy-proven");
        result = iris_room_bridge_dispatch_json(
            bridge, "destroy-proven", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_TERMINAL);
        check_equal(result.terminal_status, IRIS_ROOM_TERMINAL_SUCCEEDED);
        check_equal(result.event_type, "provider.conference.destroyed");
        check_contains(result.data, "\"alreadyAbsent\":true");
        check_contains(result.data, "\"roomGeneration\":1");
        iris_room_bridge_destroy(bridge);
        g_resource_seen = 0;
    }


    it("reconciles an unknown destroy only from observed durable absence") {
        const uint64_t now_ms = UINT64_C(1000);
        room_executor_t executor = {0, IRIS_ROOM_TERMINAL_SUCCEEDED};
        char body[2048];
        iris_room_bridge_result_t result;
        iris_room_bridge_t *bridge;

        g_claim_disposition = IRIS_COMMAND_CLAIM_OUTCOME_UNKNOWN;
        g_observation_state = IRIS_RESOURCE_OBSERVATION_ABSENT;
        g_resource_seen = 1;
        g_observe_calls = 0;
        bridge = create_bridge(&executor, &now_ms, 1u);
        make_destroy_request(body, sizeof(body), "destroy-unknown");
        result = iris_room_bridge_dispatch_json(
            bridge, "destroy-unknown", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_TERMINAL);
        check_equal(result.terminal_status, IRIS_ROOM_TERMINAL_SUCCEEDED);
        check_contains(result.data, "\"alreadyAbsent\":true");
        check_equal(g_observe_calls, 1);
        check_equal(executor.calls, 0);
        iris_room_bridge_destroy(bridge);

        g_observation_state = IRIS_RESOURCE_OBSERVATION_ACTIVE;
        bridge = create_bridge(&executor, &now_ms, 1u);
        result = iris_room_bridge_dispatch_json(
            bridge, "destroy-unknown", body, strlen(body));
        check_equal(result.status, IRIS_ROOM_BRIDGE_UNAVAILABLE);
        check_equal(result.error_code, "PROVIDER_OUTCOME_UNKNOWN");
        check_equal(executor.calls, 0);
        iris_room_bridge_destroy(bridge);

        g_claim_disposition = 0;
        g_observation_state = IRIS_RESOURCE_OBSERVATION_UNKNOWN;
        g_resource_seen = 0;
    }
}
