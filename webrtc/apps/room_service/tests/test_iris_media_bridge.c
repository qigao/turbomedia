#include "iris_media_bridge.h"

#include <tinytest.h>

#include <stdio.h>
#include <string.h>

#define TEST_LEDGER_RECORD_CAPACITY 4u

typedef enum test_ledger_state_e {
    TEST_LEDGER_EMPTY = 0,
    TEST_LEDGER_INTENT,
    TEST_LEDGER_ACCEPTED,
    TEST_LEDGER_TERMINAL,
    TEST_LEDGER_UNKNOWN
} test_ledger_state_t;

typedef struct test_ledger_record_s {
    test_ledger_state_t state;
    iris_command_identity_t identity;
    char provider_resource_id[IRIS_COMMAND_RESOURCE_ID_CAPACITY];
    iris_command_terminal_outcome_t terminal;
} test_ledger_record_t;

typedef struct test_sender_s {
    int calls;
    ivr_status_t status;
    ivr_media_command_t last_command;
    test_ledger_record_t ledger[TEST_LEDGER_RECORD_CAPACITY];
    iris_command_claim_disposition_t forced_disposition;
    ivr_status_t claim_status;
    ivr_status_t commit_accepted_status;
    ivr_status_t commit_terminal_status;
    int claim_calls;
    int commit_accepted_calls;
    int commit_terminal_calls;
    int mark_unknown_calls;
    int abort_calls;
    int resource_seen;
    ivr_status_t observation_status;
    iris_resource_observation_state_t observation_state;
    char observed_media_worker_id[IRIS_COMMAND_RESOURCE_ID_CAPACITY];
    int observe_calls;
} test_sender_t;

static test_ledger_record_t *find_ledger_record(
    test_sender_t *sender, const iris_command_identity_t *identity,
    int reserve) {
    test_ledger_record_t *free_record = NULL;
    for (size_t i = 0u; i < TEST_LEDGER_RECORD_CAPACITY; ++i) {
        test_ledger_record_t *record = &sender->ledger[i];
        if (record->state == TEST_LEDGER_EMPTY) {
            if (!free_record) free_record = record;
            continue;
        }
        if (strcmp(record->identity.command_id, identity->command_id) == 0) {
            return record;
        }
    }
    return reserve ? free_record : NULL;
}

static ivr_status_t test_ledger_claim(
    void *context, const iris_command_identity_t *identity,
    iris_command_claim_result_t *result) {
    test_sender_t *sender = (test_sender_t *)context;
    test_ledger_record_t *record;
    sender->claim_calls++;
    if (sender->claim_status != IVR_OK) return sender->claim_status;
    memset(result, 0, sizeof(*result));
    if (sender->forced_disposition != 0) {
        if (sender->forced_disposition ==
            IRIS_COMMAND_CLAIM_OUTCOME_UNKNOWN) {
            record = find_ledger_record(sender, identity, 1);
            if (!record) return IVR_ENOSPC;
            record->identity = *identity;
            record->state = TEST_LEDGER_UNKNOWN;
        }
        result->disposition = sender->forced_disposition;
        return IVR_OK;
    }
    record = find_ledger_record(sender, identity, 1);
    if (!record) return IVR_ENOSPC;
    if (record->state == TEST_LEDGER_EMPTY) {
        record->state = TEST_LEDGER_INTENT;
        record->identity = *identity;
        result->disposition = IRIS_COMMAND_CLAIM_EXECUTE;
        return IVR_OK;
    }
    if (strcmp(record->identity.semantic_fingerprint,
               identity->semantic_fingerprint) != 0) {
        result->disposition = IRIS_COMMAND_CLAIM_CONFLICT;
        return IVR_OK;
    }
    if (record->state == TEST_LEDGER_INTENT) {
        result->disposition = IRIS_COMMAND_CLAIM_IN_PROGRESS;
    } else if (record->state == TEST_LEDGER_ACCEPTED) {
        result->disposition = IRIS_COMMAND_CLAIM_REPLAY_ACCEPTED;
        memcpy(result->provider_resource_id, record->provider_resource_id,
               sizeof(result->provider_resource_id));
    } else if (record->state == TEST_LEDGER_TERMINAL) {
        result->disposition = IRIS_COMMAND_CLAIM_REPLAY_TERMINAL;
        memcpy(result->terminal_status, record->terminal.terminal_status,
               sizeof(result->terminal_status));
        memcpy(result->event_type, record->terminal.event_type,
               sizeof(result->event_type));
        memcpy(result->result_json, record->terminal.result_json,
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
    test_ledger_record_t *record = find_ledger_record(sender, identity, 0);
    sender->commit_accepted_calls++;
    if (sender->commit_accepted_status != IVR_OK) {
        return sender->commit_accepted_status;
    }
    if (!record || strlen(provider_resource_id) >=
                       sizeof(record->provider_resource_id)) {
        return IVR_ESTATE;
    }
    memcpy(record->provider_resource_id, provider_resource_id,
           strlen(provider_resource_id) + 1u);
    record->state = TEST_LEDGER_ACCEPTED;
    return IVR_OK;
}

static ivr_status_t test_ledger_commit_terminal(
    void *context, const iris_command_identity_t *identity,
    const iris_command_terminal_outcome_t *outcome) {
    test_sender_t *sender = (test_sender_t *)context;
    test_ledger_record_t *record = find_ledger_record(sender, identity, 0);
    sender->commit_terminal_calls++;
    if (sender->commit_terminal_status != IVR_OK) {
        return sender->commit_terminal_status;
    }
    if (!record) return IVR_ESTATE;
    record->terminal = *outcome;
    record->state = TEST_LEDGER_TERMINAL;
    return IVR_OK;
}

static ivr_status_t test_ledger_mark_unknown(
    void *context, const iris_command_identity_t *identity) {
    test_sender_t *sender = (test_sender_t *)context;
    test_ledger_record_t *record = find_ledger_record(sender, identity, 0);
    sender->mark_unknown_calls++;
    if (!record) return IVR_ESTATE;
    record->state = TEST_LEDGER_UNKNOWN;
    return IVR_OK;
}

static ivr_status_t test_ledger_abort(
    void *context, const iris_command_identity_t *identity) {
    test_sender_t *sender = (test_sender_t *)context;
    test_ledger_record_t *record = find_ledger_record(sender, identity, 0);
    sender->abort_calls++;
    if (!record || record->state != TEST_LEDGER_INTENT) return IVR_ESTATE;
    memset(record, 0, sizeof(*record));
    return IVR_OK;
}

static ivr_status_t test_ledger_resource_seen(
    void *context, const iris_command_identity_t *identity, int *seen) {
    test_sender_t *sender = (test_sender_t *)context;
    (void)identity;
    *seen = sender->resource_seen;
    return IVR_OK;
}

static uint64_t test_now_ms(void *context) {
    return *(const uint64_t *)context;
}

static ivr_status_t test_send(void *context,
                              const ivr_media_command_t *command,
                              char *out_worker_id,
                              size_t out_worker_id_capacity) {
    test_sender_t *sender = (test_sender_t *)context;
    sender->calls++;
    sender->last_command = *command;
    if (sender->status != IVR_OK) {
        return sender->status;
    }
    if (out_worker_id_capacity < sizeof("media-worker-1")) {
        return IVR_ENOSPC;
    }
    memcpy(out_worker_id, "media-worker-1", sizeof("media-worker-1"));
    return IVR_OK;
}

static ivr_status_t test_observe(
    void *context, const ivr_media_command_t *command,
    iris_resource_observation_t *observation) {
    test_sender_t *sender = (test_sender_t *)context;
    (void)command;
    sender->observe_calls++;
    memset(observation, 0, sizeof(*observation));
    observation->state = sender->observation_state;
    if (sender->observed_media_worker_id[0]) {
        memcpy(observation->provider_resource_id,
               sender->observed_media_worker_id,
               strlen(sender->observed_media_worker_id) + 1u);
    }
    return sender->observation_status;
}

static iris_media_bridge_t *create_bridge(test_sender_t *sender,
                                          const uint64_t *now_ms,
                                          size_t capacity) {
    iris_media_bridge_config_t config;
    memset(&config, 0, sizeof(config));
    config.correlation_capacity = capacity;
    config.send = test_send;
    config.send_context = sender;
    config.observe = test_observe;
    config.observe_context = sender;
    config.realtime_ms = test_now_ms;
    config.realtime_context = (void *)now_ms;
    config.ledger.context = sender;
    config.ledger.claim = test_ledger_claim;
    config.ledger.commit_accepted = test_ledger_commit_accepted;
    config.ledger.commit_terminal = test_ledger_commit_terminal;
    config.ledger.mark_unknown = test_ledger_mark_unknown;
    config.ledger.resource_seen = test_ledger_resource_seen;
    config.ledger.abort_intent = test_ledger_abort;
    return iris_media_bridge_create(&config);
}

static void make_request(char *buffer, size_t capacity, const char *command_id,
                         const char *worker_id, const char *epoch,
                         const char *text) {
    snprintf(
        buffer, capacity,
        "{"
        "\"schemaVersion\":2,"
        "\"commandId\":\"%s\","
        "\"tenantId\":\"tenant-a\","
        "\"sessionId\":\"session-a\","
        "\"type\":\"media.play\","
        "\"provider\":\"turbomedia\","
        "\"correlationId\":\"correlation-a\","
        "\"causationId\":\"causation-a\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"%s\","
        "\"dispatchEpoch\":%s,"
        "\"data\":{"
        "\"capability\":\"ivr\","
        "\"dialogId\":\"dialog-a\","
        "\"roomId\":\"room-a\","
        "\"callId\":\"call-a\","
        "\"callGeneration\":18446744073709551614,"
        "\"operationGeneration\":9007199254740993,"
        "\"text\":\"%s\"}}",
        command_id, worker_id, epoch, text);
}

static void make_close_request(char *buffer, size_t capacity,
                               const char *command_id) {
    snprintf(
        buffer, capacity,
        "{"
        "\"schemaVersion\":2,\"commandId\":\"%s\","
        "\"tenantId\":\"tenant-a\",\"sessionId\":\"session-a\","
        "\"type\":\"dialog.terminate\",\"provider\":\"turbomedia\","
        "\"correlationId\":\"correlation-a\","
        "\"causationId\":\"causation-a\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-a\",\"dispatchEpoch\":41,"
        "\"data\":{\"capability\":\"ivr\","
        "\"dialogId\":\"dialog-a\",\"roomId\":\"room-a\","
        "\"callId\":\"call-a\",\"callGeneration\":7,"
        "\"operationGeneration\":9,\"reason\":\"test\"}}",
        command_id);
}

static void make_cancel_request(char *buffer, size_t capacity,
                                const char *command_id,
                                const char *input_id,
                                const char *input_generation) {
    snprintf(
        buffer, capacity,
        "{"
        "\"schemaVersion\":2,\"commandId\":\"%s\","
        "\"tenantId\":\"tenant-a\",\"sessionId\":\"session-a\","
        "\"type\":\"media.cancel\",\"provider\":\"turbomedia\","
        "\"correlationId\":\"correlation-a\","
        "\"causationId\":\"causation-a\","
        "\"deadline\":\"2099-01-01T00:00:00Z\","
        "\"workerId\":\"iris-worker-a\",\"dispatchEpoch\":41,"
        "\"data\":{\"capability\":\"ivr\","
        "\"dialogId\":\"dialog-a\",\"roomId\":\"room-a\","
        "\"callId\":\"call-a\",\"callGeneration\":7,"
        "\"operationGeneration\":9,\"inputId\":\"%s\","
        "\"inputGeneration\":%s}}",
        command_id, input_id, input_generation);
}

static ivr_media_command_result_t make_media_result(const char *command_id,
                                                     int status_code) {
    ivr_media_command_result_t result;
    memset(&result, 0, sizeof(result));
    snprintf(result.message_id, sizeof(result.message_id), "%s", command_id);
    snprintf(result.tenant_id, sizeof(result.tenant_id), "%s", "tenant-a");
    snprintf(result.provider_session_id, sizeof(result.provider_session_id),
             "%s", "session-a");
    snprintf(result.dialog_id, sizeof(result.dialog_id), "%s", "dialog-a");
    snprintf(result.worker_id, sizeof(result.worker_id), "%s",
             "media-worker-1");
    snprintf(result.room_id, sizeof(result.room_id), "%s", "room-a");
    snprintf(result.call_id, sizeof(result.call_id), "%s", "call-a");
    result.call_generation = UINT64_C(18446744073709551614);
    result.operation_generation = UINT64_C(9007199254740993);
    result.status_code = status_code;
    if (status_code != IVR_OK) {
        snprintf(result.error_code, sizeof(result.error_code), "%s",
                 "MEDIA_FAILED");
        snprintf(result.error_message, sizeof(result.error_message), "%s",
                 "media operation failed");
    }
    return result;
}

spec("Iris media provider bridge") {
    it("settles a missing dialog only when its generation was seen durably") {
        const uint64_t now_ms = UINT64_C(1000);
        char body[2048];
        iris_media_bridge_result_t result;
        test_sender_t unproven = {0};
        test_sender_t proven = {0};
        iris_media_bridge_t *bridge;

        make_close_request(body, sizeof(body), "close-unproven");
        unproven.status = IVR_ENOTFOUND;
        bridge = create_bridge(&unproven, &now_ms, 1u);
        result = iris_media_bridge_dispatch_json(
            bridge, "close-unproven", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_TERMINAL);
        check_equal(result.terminal_status, "failed");
        check_contains(result.data, "MEDIA_RESOURCE_NOT_FOUND");
        check_contains(result.data, "\"alreadyAbsent\":false");
        check_equal(unproven.commit_terminal_calls, 1);
        iris_media_bridge_destroy(bridge);

        make_close_request(body, sizeof(body), "close-proven");
        proven.status = IVR_ENOTFOUND;
        proven.resource_seen = 1;
        bridge = create_bridge(&proven, &now_ms, 1u);
        result = iris_media_bridge_dispatch_json(
            bridge, "close-proven", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_TERMINAL);
        check_equal(result.terminal_status, "succeeded");
        check_equal(result.event_type, "provider.dialog.terminated");
        check_contains(result.data, "\"alreadyAbsent\":true");
        check_contains(result.data, "\"callGeneration\":7");
        check_equal(proven.commit_terminal_calls, 1);
        iris_media_bridge_destroy(bridge);
    }

    it("dispatches a valid command with exact uint64 values") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 2u);
        iris_media_bridge_result_t result;

        check_not_null(bridge);
        make_request(body, sizeof(body), "command-a", "iris-worker-a",
                     "18446744073709551615", "Welcome");
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));

        check_equal(result.status, IRIS_MEDIA_BRIDGE_ACCEPTED);
        check_equal(sender.calls, 1);
        check_equal(result.command_id, "command-a");
        check_equal(result.iris_worker_id, "iris-worker-a");
        check_equal(result.media_worker_id, "media-worker-1");
        check_true(result.dispatch_epoch == UINT64_MAX);
        check_true(sender.last_command.call_generation ==
                   UINT64_C(18446744073709551614));
        check_true(sender.last_command.operation_generation ==
                   UINT64_C(9007199254740993));
        check_equal(sender.last_command.dialog_id, "dialog-a");
        check_equal(sender.last_command.worker_id, "");
        iris_media_bridge_destroy(bridge);
    }

    it("keys media cancel to the exact input incarnation") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 1u);
        iris_media_bridge_result_t result;

        make_cancel_request(body, sizeof(body), "cancel-a", "input-a", "11");
        result = iris_media_bridge_dispatch_json(
            bridge, "cancel-a", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_ACCEPTED);
        check_equal(sender.calls, 1);
        check_equal(sender.last_command.input_id, "input-a");
        check_true(sender.last_command.input_generation == UINT64_C(11));
        check_equal(sender.ledger[0].identity.resource_scope_id, "dialog-a");
        check_true(sender.ledger[0].identity.resource_scope_generation ==
                   UINT64_C(7));
        check_equal(sender.ledger[0].identity.resource_id, "input-a");
        check_true(sender.ledger[0].identity.resource_generation ==
                   UINT64_C(11));
        iris_media_bridge_destroy(bridge);
    }

    it("rejects media cancel without a complete input fence") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 1u);
        iris_media_bridge_result_t result;

        make_cancel_request(body, sizeof(body), "cancel-empty", "", "11");
        result = iris_media_bridge_dispatch_json(
            bridge, "cancel-empty", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_INVALID);
        make_cancel_request(body, sizeof(body), "cancel-zero", "input-a", "0");
        result = iris_media_bridge_dispatch_json(
            bridge, "cancel-zero", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_INVALID);
        check_equal(sender.calls, 0);
        check_equal(sender.claim_calls, 0);
        iris_media_bridge_destroy(bridge);
    }

    it("replays acceptance without repeating the media side effect") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char first[2048];
        char reclaimed[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 2u);
        iris_media_bridge_result_t result;

        make_request(first, sizeof(first), "command-a", "iris-worker-a",
                     "41", "Welcome");
        make_request(reclaimed, sizeof(reclaimed), "command-a",
                     "iris-worker-b", "42", "Welcome");
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", first, strlen(first));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_ACCEPTED);
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", first, strlen(first));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_DUPLICATE);
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", reclaimed, strlen(reclaimed));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_DUPLICATE);
        check_equal(sender.calls, 1);
        check_equal(result.iris_worker_id, "iris-worker-b");
        check_true(result.dispatch_epoch == UINT64_C(42));
        iris_media_bridge_destroy(bridge);
    }

    it("rejects stale fences and command data conflicts") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char first[2048];
        char stale[2048];
        char changed[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 2u);
        iris_media_bridge_result_t result;

        make_request(first, sizeof(first), "command-a", "iris-worker-a",
                     "41", "Welcome");
        make_request(stale, sizeof(stale), "command-a", "iris-worker-a",
                     "40", "Welcome");
        make_request(changed, sizeof(changed), "command-a", "iris-worker-a",
                     "42", "Changed");
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", first, strlen(first));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_ACCEPTED);
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", stale, strlen(stale));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_CONFLICT);
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", changed, strlen(changed));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_CONFLICT);
        check_equal(sender.calls, 1);
        iris_media_bridge_destroy(bridge);
    }

    it("rolls back a failed send so Iris can retry") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 1u);
        iris_media_bridge_result_t result;

        make_request(body, sizeof(body), "command-a", "iris-worker-a",
                     "41", "Welcome");
        sender.status = IVR_ESTATE;
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_UNAVAILABLE);
        sender.status = IVR_OK;
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_ACCEPTED);
        check_equal(sender.calls, 2);
        iris_media_bridge_destroy(bridge);
    }

    it("rejects capacity exhaustion without dispatching another command") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char first[2048];
        char second[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 1u);
        iris_media_bridge_result_t result;

        make_request(first, sizeof(first), "command-a", "iris-worker-a",
                     "41", "First");
        make_request(second, sizeof(second), "command-b", "iris-worker-a",
                     "42", "Second");
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", first, strlen(first));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_ACCEPTED);
        result = iris_media_bridge_dispatch_json(
            bridge, "command-b", second, strlen(second));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_FULL);
        check_equal(sender.calls, 1);
        iris_media_bridge_destroy(bridge);
    }

    it("replays a durable terminal outcome without another media side effect") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 2u);
        iris_media_bridge_result_t dispatch;
        iris_media_completion_t completion;
        ivr_media_command_result_t media_result =
            make_media_result("command-a", IVR_OK);

        make_request(body, sizeof(body), "command-a", "iris-worker-a",
                     "41", "Welcome");
        dispatch = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(dispatch.status, IRIS_MEDIA_BRIDGE_ACCEPTED);
        check_equal(iris_media_bridge_claim_completion(
                         bridge, &media_result, &completion),
                     IVR_OK);
        check_equal(completion.command_id, "command-a");
        iris_media_bridge_release_completion(bridge, "command-a");

        dispatch = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(dispatch.status, IRIS_MEDIA_BRIDGE_TERMINAL_REPLAY);
        check_equal(dispatch.terminal_status, "succeeded");
        check_equal(dispatch.event_type, "provider.media.completed");
        check_contains(dispatch.data, "\"status\":\"completed\"");
        check_contains(dispatch.data, "\"mediaWorkerId\":\"media-worker-1\"");
        check_equal(sender.calls, 1);
        check_equal(sender.commit_terminal_calls, 1);
        iris_media_bridge_destroy(bridge);
    }

    it("marks an accepted media side effect unknown when durable acceptance fails") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 1u);
        iris_media_bridge_result_t result;

        make_request(body, sizeof(body), "command-a", "iris-worker-a",
                     "41", "Welcome");
        sender.commit_accepted_status = IVR_ESTATE;
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_UNAVAILABLE);
        check_equal(result.error_code, "PROVIDER_OUTCOME_UNKNOWN");
        check_equal(sender.calls, 1);
        check_equal(sender.mark_unknown_calls, 1);

        sender.commit_accepted_status = IVR_OK;
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_UNAVAILABLE);
        check_equal(result.error_code, "PROVIDER_OUTCOME_UNKNOWN");
        check_equal(sender.calls, 1);
        iris_media_bridge_destroy(bridge);
    }

    it("recovers unknown acceptance from the generation-aware media route") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 1u);
        iris_media_bridge_result_t result;

        make_request(body, sizeof(body), "command-a", "iris-worker-a",
                     "41", "Welcome");
        sender.commit_accepted_status = IVR_ESTATE;
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_UNAVAILABLE);
        check_equal(sender.calls, 1);

        sender.commit_accepted_status = IVR_OK;
        sender.observation_state = IRIS_RESOURCE_OBSERVATION_ACTIVE;
        snprintf(sender.observed_media_worker_id,
                 sizeof(sender.observed_media_worker_id), "%s",
                 "media-worker-1");
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_DUPLICATE);
        check_equal(result.media_worker_id, "media-worker-1");
        check_equal(sender.observe_calls, 1);
        check_equal(sender.calls, 1);
        check_equal(sender.commit_accepted_calls, 2);
        iris_media_bridge_destroy(bridge);
    }

    it("settles an unknown close only from observed durable absence") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge;
        iris_media_bridge_result_t result;

        sender.forced_disposition = IRIS_COMMAND_CLAIM_OUTCOME_UNKNOWN;
        sender.observation_state = IRIS_RESOURCE_OBSERVATION_ABSENT;
        sender.resource_seen = 1;
        bridge = create_bridge(&sender, &now_ms, 1u);
        make_close_request(body, sizeof(body), "close-unknown");
        result = iris_media_bridge_dispatch_json(
            bridge, "close-unknown", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_TERMINAL);
        check_equal(result.terminal_status, "succeeded");
        check_contains(result.data, "\"alreadyAbsent\":true");
        check_equal(sender.observe_calls, 1);
        check_equal(sender.calls, 0);
        iris_media_bridge_destroy(bridge);
    }

    it("settles an unknown cancel only for the same absent input generation") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge;
        iris_media_bridge_result_t result;

        sender.forced_disposition = IRIS_COMMAND_CLAIM_OUTCOME_UNKNOWN;
        sender.observation_state = IRIS_RESOURCE_OBSERVATION_ABSENT;
        sender.resource_seen = 1;
        bridge = create_bridge(&sender, &now_ms, 1u);
        make_cancel_request(body, sizeof(body), "cancel-unknown", "input-a",
                            "11");
        result = iris_media_bridge_dispatch_json(
            bridge, "cancel-unknown", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_TERMINAL);
        check_equal(result.terminal_status, "succeeded");
        check_equal(result.event_type, "provider.media.cancelled");
        check_contains(result.data, "\"inputId\":\"input-a\"");
        check_contains(result.data, "\"inputGeneration\":11");
        check_contains(result.data, "\"alreadyAbsent\":true");
        check_equal(sender.ledger[0].identity.resource_id, "input-a");
        check_true(sender.ledger[0].identity.resource_generation ==
                   UINT64_C(11));
        check_equal(sender.observe_calls, 1);
        check_equal(sender.calls, 0);
        iris_media_bridge_destroy(bridge);
    }

    it("does not expose a completion before the terminal outcome is durable") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 1u);
        iris_media_bridge_result_t dispatch;
        iris_media_completion_t completion;
        ivr_media_command_result_t media_result =
            make_media_result("command-a", IVR_ESTATE);

        make_request(body, sizeof(body), "command-a", "iris-worker-a",
                     "41", "Welcome");
        dispatch = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(dispatch.status, IRIS_MEDIA_BRIDGE_ACCEPTED);
        sender.commit_terminal_status = IVR_ESTATE;
        check_equal(iris_media_bridge_claim_completion(
                         bridge, &media_result, &completion),
                     IVR_ESTATE);
        check_equal(completion.command_id, "");
        check_equal(sender.mark_unknown_calls, 1);

        sender.commit_terminal_status = IVR_OK;
        check_equal(iris_media_bridge_claim_completion(
                         bridge, &media_result, &completion),
                     IVR_OK);
        check_equal(completion.command_id, "command-a");
        check_equal(sender.commit_terminal_calls, 2);
        iris_media_bridge_destroy(bridge);
    }

    it("rejects a durable semantic conflict before sending to CHTTP H1 WebSocket") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 1u);
        iris_media_bridge_result_t result;

        make_request(body, sizeof(body), "command-a", "iris-worker-a",
                     "41", "Welcome");
        sender.forced_disposition = IRIS_COMMAND_CLAIM_CONFLICT;
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_CONFLICT);
        check_equal(sender.calls, 0);
        iris_media_bridge_destroy(bridge);
    }

    it("rejects malformed integers and mismatched idempotency keys") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 1u);
        iris_media_bridge_result_t result;

        make_request(body, sizeof(body), "command-a", "iris-worker-a",
                     "4.1", "Welcome");
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_INVALID);
        make_request(body, sizeof(body), "command-a", "iris-worker-a",
                     "41", "Welcome");
        result = iris_media_bridge_dispatch_json(
            bridge, "different-command", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_INVALID);
        check_equal(sender.calls, 0);
        iris_media_bridge_destroy(bridge);
    }

    it("rejects unknown command data instead of weakening idempotency") {
        const uint64_t now_ms = UINT64_C(1000);
        test_sender_t sender = {0};
        char body[2048];
        char *data_end;
        iris_media_bridge_t *bridge = create_bridge(&sender, &now_ms, 1u);
        iris_media_bridge_result_t result;

        make_request(body, sizeof(body), "command-a", "iris-worker-a",
                     "41", "Welcome");
        data_end = strstr(body, "}}");
        check_not_null(data_end);
        if (data_end) {
            memmove(data_end + strlen(",\"ignored\":true"), data_end,
                    strlen(data_end) + 1u);
            memcpy(data_end, ",\"ignored\":true",
                   strlen(",\"ignored\":true"));
        }
        result = iris_media_bridge_dispatch_json(
            bridge, "command-a", body, strlen(body));
        check_equal(result.status, IRIS_MEDIA_BRIDGE_INVALID);
        check_equal(sender.calls, 0);
        iris_media_bridge_destroy(bridge);
    }
}
