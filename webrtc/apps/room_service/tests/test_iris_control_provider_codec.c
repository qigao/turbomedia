#include "iris_control_provider_codec.h"
#include "iris_provider_protocol.h"

#include <json_parser.h>
#include <tinytest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SEMANTIC_FINGERPRINT                                           \
    "sha256:710227af291421e97abf6de1cc8af21e2a50d653bb2855ecab4f549425fdeb4a"
#define TEST_ROOM_SEMANTIC_FINGERPRINT                                      \
    "sha256:77266bd0dee0f61e6bb3ce63fcaeaba7114c9b940838c4b495bc3d9424d0664a"

#define SET_TEXT(record, builder, member, value) \
    record##_##member##_set(&(builder), (value), strlen(value))

#define CHECK_TEXT(record, view, member, expected)                         \
    do {                                                                    \
        tbe_var_data_t actual_;                                             \
        check_true(record##_##member(&(view), &actual_));                   \
        check_equal(actual_.size, strlen(expected));                        \
        check_true(actual_.size == 0u ||                                    \
                   memcmp(actual_.data, (expected), actual_.size) == 0);     \
    } while (0)

static uint8_t *allocate_wire(size_t block_length, const char *const *fields,
                              size_t field_count, size_t *out_size) {
    size_t size = block_length;
    size_t index;
    uint8_t *buffer;
    for (index = 0u; index < field_count; ++index) {
        size += sizeof(uint32_t) + strlen(fields[index]);
    }
    buffer = (uint8_t *)malloc(size);
    if (!buffer) return NULL;
    memset(buffer, 0, size);
    *out_size = size;
    return buffer;
}

static uint8_t *encode_command(const char *payload, const char *partition_key,
                               const char *epoch, const char *fingerprint,
                               size_t *encoded_size) {
    const char *fields[] = {
        "command-a", "correlation-a", "causation-a", "tenant-a",
        "turbomedia", "session-a", partition_key, "iris-a",
        "2026-08-14T00:00:00Z", "2099-01-01T00:00:00Z", "command-a",
        "media.play", "iris-worker-a", epoch, fingerprint, payload};
    ProviderCommandV1_builder_t builder;
    uint8_t *encoded = allocate_wire(ProviderCommandV1_BLOCK_LENGTH, fields,
                                     sizeof(fields) / sizeof(fields[0]),
                                     encoded_size);
    if (!encoded ||
        !ProviderCommandV1_builder_bind(&builder, encoded, *encoded_size) ||
        !ProviderCommandV1_schema_version_set(
            &builder, IRIS_PROVIDER_SCHEMA_VERSION) ||
        !ProviderCommandV1_message_kind_set(&builder,
                                            ProviderMessageKind_Command) ||
        !SET_TEXT(ProviderCommandV1, builder, message_id, fields[0]) ||
        !SET_TEXT(ProviderCommandV1, builder, correlation_id, fields[1]) ||
        !SET_TEXT(ProviderCommandV1, builder, causation_id, fields[2]) ||
        !SET_TEXT(ProviderCommandV1, builder, tenant_id, fields[3]) ||
        !SET_TEXT(ProviderCommandV1, builder, provider_id, fields[4]) ||
        !SET_TEXT(ProviderCommandV1, builder, session_id, fields[5]) ||
        !SET_TEXT(ProviderCommandV1, builder, partition_key, fields[6]) ||
        !SET_TEXT(ProviderCommandV1, builder, producer_id, fields[7]) ||
        !SET_TEXT(ProviderCommandV1, builder, created_at, fields[8]) ||
        !SET_TEXT(ProviderCommandV1, builder, deadline_at, fields[9]) ||
        !SET_TEXT(ProviderCommandV1, builder, command_id, fields[10]) ||
        !SET_TEXT(ProviderCommandV1, builder, command_type, fields[11]) ||
        !SET_TEXT(ProviderCommandV1, builder, worker_id, fields[12]) ||
        !SET_TEXT(ProviderCommandV1, builder, dispatch_epoch, fields[13]) ||
        !SET_TEXT(ProviderCommandV1, builder, semantic_fingerprint, fields[14]) ||
        !SET_TEXT(ProviderCommandV1, builder, payload_json, fields[15])) {
        free(encoded);
        *encoded_size = 0u;
        return NULL;
    }
    return encoded;
}

static uint8_t *encode_completion_ack(size_t *encoded_size) {
    const char *fields[] = {
        "completion-a.completion-ack", "correlation-a", "completion-a",
        "tenant-a", "turbomedia", "session-a", "session-a", "iris-a",
        "2026-08-14T00:00:03Z", "", "command-a", "17", "23", "", ""};
    ProviderCompletionAckV1_builder_t builder;
    uint8_t *encoded = allocate_wire(ProviderCompletionAckV1_BLOCK_LENGTH,
                                     fields, 15u, encoded_size);
    if (!encoded ||
        !ProviderCompletionAckV1_builder_bind(&builder, encoded, *encoded_size) ||
        !ProviderCompletionAckV1_schema_version_set(
            &builder, IRIS_PROVIDER_SCHEMA_VERSION) ||
        !ProviderCompletionAckV1_message_kind_set(
            &builder, ProviderMessageKind_CompletionAck) ||
        !ProviderCompletionAckV1_disposition_set(
            &builder,
            ProviderCompletionAckDisposition_CompletionCommitted) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, message_id, fields[0]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, correlation_id, fields[1]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, causation_id, fields[2]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, tenant_id, fields[3]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, provider_id, fields[4]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, session_id, fields[5]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, partition_key, fields[6]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, producer_id, fields[7]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, created_at, fields[8]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, deadline_at, fields[9]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, command_id, fields[10]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, dispatch_epoch, fields[11]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, committed_sequence, fields[12]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, error_code, fields[13]) ||
        !SET_TEXT(ProviderCompletionAckV1, builder, error_message, fields[14])) {
        free(encoded);
        *encoded_size = 0u;
        return NULL;
    }
    return encoded;
}

static uint8_t *encode_event_ack(size_t *encoded_size) {
    const char *fields[] = {
        "event-msg-a.event-ack", "dialog-a", "event-msg-a", "tenant-a",
        "turbomedia", "session-a", "session-a", "iris-a",
        "2026-08-14T00:00:05Z", "", "event-a", "37", "", ""};
    ProviderEventAckV1_builder_t builder;
    uint8_t *encoded = allocate_wire(ProviderEventAckV1_BLOCK_LENGTH, fields,
                                     14u, encoded_size);
    if (!encoded ||
        !ProviderEventAckV1_builder_bind(&builder, encoded, *encoded_size) ||
        !ProviderEventAckV1_schema_version_set(
            &builder, IRIS_PROVIDER_SCHEMA_VERSION) ||
        !ProviderEventAckV1_message_kind_set(&builder,
                                             ProviderMessageKind_EventAck) ||
        !ProviderEventAckV1_disposition_set(
            &builder, ProviderEventAckDisposition_Committed) ||
        !SET_TEXT(ProviderEventAckV1, builder, message_id, fields[0]) ||
        !SET_TEXT(ProviderEventAckV1, builder, correlation_id, fields[1]) ||
        !SET_TEXT(ProviderEventAckV1, builder, causation_id, fields[2]) ||
        !SET_TEXT(ProviderEventAckV1, builder, tenant_id, fields[3]) ||
        !SET_TEXT(ProviderEventAckV1, builder, provider_id, fields[4]) ||
        !SET_TEXT(ProviderEventAckV1, builder, session_id, fields[5]) ||
        !SET_TEXT(ProviderEventAckV1, builder, partition_key, fields[6]) ||
        !SET_TEXT(ProviderEventAckV1, builder, producer_id, fields[7]) ||
        !SET_TEXT(ProviderEventAckV1, builder, created_at, fields[8]) ||
        !SET_TEXT(ProviderEventAckV1, builder, deadline_at, fields[9]) ||
        !SET_TEXT(ProviderEventAckV1, builder, event_id, fields[10]) ||
        !SET_TEXT(ProviderEventAckV1, builder, committed_sequence, fields[11]) ||
        !SET_TEXT(ProviderEventAckV1, builder, error_code, fields[12]) ||
        !SET_TEXT(ProviderEventAckV1, builder, error_message, fields[13])) {
        free(encoded);
        *encoded_size = 0u;
        return NULL;
    }
    return encoded;
}

static uint8_t *encode_observation(size_t *encoded_size) {
    const char *fields[] = {
        "observation-a", "query-a", "query-a", "tenant-a", "turbomedia",
        "", "tenant-a", "iris-a", "2026-08-14T00:00:07Z", "", "query-a",
        "expected_media_resources", "19", "32", "48",
        "{\"schemaVersion\":1,\"resources\":[]}", "", ""};
    ProviderObservationV1_builder_t builder;
    uint8_t *encoded = allocate_wire(ProviderObservationV1_BLOCK_LENGTH,
                                     fields, 18u, encoded_size);
    if (!encoded ||
        !ProviderObservationV1_builder_bind(&builder, encoded, *encoded_size) ||
        !ProviderObservationV1_schema_version_set(
            &builder, IRIS_PROVIDER_SCHEMA_VERSION) ||
        !ProviderObservationV1_message_kind_set(
            &builder, ProviderMessageKind_Observation) ||
        !ProviderObservationV1_status_set(&builder,
                                          ProviderQueryStatus_QueryOk) ||
        !ProviderObservationV1_has_more_set(&builder, 1u) ||
        !SET_TEXT(ProviderObservationV1, builder, message_id, fields[0]) ||
        !SET_TEXT(ProviderObservationV1, builder, correlation_id, fields[1]) ||
        !SET_TEXT(ProviderObservationV1, builder, causation_id, fields[2]) ||
        !SET_TEXT(ProviderObservationV1, builder, tenant_id, fields[3]) ||
        !SET_TEXT(ProviderObservationV1, builder, provider_id, fields[4]) ||
        !SET_TEXT(ProviderObservationV1, builder, session_id, fields[5]) ||
        !SET_TEXT(ProviderObservationV1, builder, partition_key, fields[6]) ||
        !SET_TEXT(ProviderObservationV1, builder, producer_id, fields[7]) ||
        !SET_TEXT(ProviderObservationV1, builder, created_at, fields[8]) ||
        !SET_TEXT(ProviderObservationV1, builder, deadline_at, fields[9]) ||
        !SET_TEXT(ProviderObservationV1, builder, query_id, fields[10]) ||
        !SET_TEXT(ProviderObservationV1, builder, observation_type, fields[11]) ||
        !SET_TEXT(ProviderObservationV1, builder, revision, fields[12]) ||
        !SET_TEXT(ProviderObservationV1, builder, cursor, fields[13]) ||
        !SET_TEXT(ProviderObservationV1, builder, next_cursor, fields[14]) ||
        !SET_TEXT(ProviderObservationV1, builder, payload_json, fields[15]) ||
        !SET_TEXT(ProviderObservationV1, builder, error_code, fields[16]) ||
        !SET_TEXT(ProviderObservationV1, builder, error_message, fields[17])) {
        free(encoded);
        *encoded_size = 0u;
        return NULL;
    }
    return encoded;
}

static uint8_t *encode_session_bound(size_t *encoded_size) {
    const char *fields[] = {
        "session-bound-a", "call-a", "ingress-call-a", "tenant-a",
        "turbomedia", "session-a", "call-a", "iris-a",
        "2026-08-14T00:00:09Z", "2026-08-14T00:00:13Z", "ingress-call-a",
        "call-a", "7", "session-a", "", ""};
    ProviderSessionBoundV1_builder_t builder;
    uint8_t *encoded = allocate_wire(ProviderSessionBoundV1_BLOCK_LENGTH,
                                     fields, 16u, encoded_size);
    if (!encoded ||
        !ProviderSessionBoundV1_builder_bind(&builder, encoded, *encoded_size) ||
        !ProviderSessionBoundV1_schema_version_set(
            &builder, IRIS_PROVIDER_SCHEMA_VERSION) ||
        !ProviderSessionBoundV1_message_kind_set(
            &builder, ProviderMessageKind_SessionBound) ||
        !ProviderSessionBoundV1_accepted_set(&builder, 1u) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, message_id, fields[0]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, correlation_id, fields[1]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, causation_id, fields[2]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, tenant_id, fields[3]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, provider_id, fields[4]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, session_id, fields[5]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, partition_key, fields[6]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, producer_id, fields[7]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, created_at, fields[8]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, deadline_at, fields[9]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, ingress_event_id, fields[10]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, call_id, fields[11]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, call_generation, fields[12]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, bound_session_id, fields[13]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, error_code, fields[14]) ||
        !SET_TEXT(ProviderSessionBoundV1, builder, error_message, fields[15])) {
        free(encoded);
        *encoded_size = 0u;
        return NULL;
    }
    return encoded;
}

static const char *json_string_field(const json_value_t *object,
                                     const char *name) {
    json_value_t *value = json_object_get(object, name);
    return value && json_type(value) == JSON_STRING ? json_string(value) : NULL;
}

spec("RoomService Iris CHTTP H1 WebSocket provider codec") {
    it("decodes the canonical command and preserves its exact bridge fields") {
        static const char payload[] =
            "{\"capability\":\"ivr\",\"dialogId\":\"dialog-a\","
            "\"roomId\":\"room-a\",\"callId\":\"call-a\","
            "\"callGeneration\":7,\"operationGeneration\":9,"
            "\"text\":\"Welcome\"}";
        iris_control_provider_command_t decoded;
        json_value_t *root;
        json_value_t *data;
        size_t encoded_size = 0u;
        uint8_t *encoded = encode_command(
            payload, "session-a", "17", TEST_SEMANTIC_FINGERPRINT,
            &encoded_size);
        check_not_null(encoded);
        check_equal(iris_control_provider_decode_command(
                        encoded, encoded_size, &decoded),
                    IVR_OK);
        check_equal(decoded.dispatch_epoch, 17u);
        check_equal(decoded.wire.semantic_fingerprint,
                    TEST_SEMANTIC_FINGERPRINT);
        root = json_parse(decoded.bridge_json, decoded.bridge_json_size);
        check_not_null(root);
        check_equal(json_string_field(root, "commandId"), "command-a");
        check_equal(json_string_field(root, "tenantId"), "tenant-a");
        check_equal(json_string_field(root, "workerId"), "iris-worker-a");
        check_equal((uint64_t)json_number(json_object_get(root, "schemaVersion")),
                    UINT64_C(2));
        data = json_object_get(root, "data");
        check_not_null(data);
        check_equal(json_string_field(data, "dialogId"), "dialog-a");
        check_equal(json_string_field(data, "text"), "Welcome");
        json_free(root);
        iris_control_provider_command_clear(&decoded);
        free(encoded);
    }

    it("rejects embedded NUL bytes in canonical text fields") {
        static const char payload[] =
            "{\"capability\":\"ivr\",\"dialogId\":\"dialog-a\","
            "\"roomId\":\"room-a\",\"callId\":\"call-a\","
            "\"callGeneration\":7,\"operationGeneration\":9,"
            "\"text\":\"Welcome\"}";
        iris_provider_limits_t limits = IRIS_PROVIDER_LIMITS_INIT;
        ProviderCommandV1_view_t view;
        tbe_var_data_t message_id;
        size_t encoded_size = 0u;
        uint8_t *encoded = encode_command(
            payload, "session-a", "17", TEST_SEMANTIC_FINGERPRINT,
            &encoded_size);

        check_not_null(encoded);
        check_true(ProviderCommandV1_view_bind(&view, encoded, encoded_size));
        check_true(ProviderCommandV1_message_id(&view, &message_id));
        check_equal(message_id.size, strlen("command-a"));
        ((uint8_t *)message_id.data)[7] = '\0';
        check_equal(iris_provider_validate_command(&view, &limits),
                    SALTS_EPROTO);
        ((uint8_t *)message_id.data)[7] = 0xffu;
        check_equal(iris_provider_validate_command(&view, &limits),
                    SALTS_ECHARSET);
        free(encoded);
    }

    it("maps canonical room commands to the room bridge schema") {
        static const char payload[] =
            "{\"capability\":\"room\",\"roomId\":\"room-a\","
            "\"roomGeneration\":1}";
        iris_control_provider_command_t decoded;
        json_value_t *root;
        size_t encoded_size = 0u;
        uint8_t *encoded = encode_command(
            payload, "session-a", "17", TEST_ROOM_SEMANTIC_FINGERPRINT,
            &encoded_size);
        check_not_null(encoded);
        check_equal(iris_control_provider_decode_command(
                        encoded, encoded_size, &decoded),
                    IVR_OK);
        root = json_parse(decoded.bridge_json, decoded.bridge_json_size);
        check_not_null(root);
        check_equal((uint64_t)json_number(json_object_get(root, "schemaVersion")),
                    UINT64_C(3));
        json_free(root);
        iris_control_provider_command_clear(&decoded);
        free(encoded);
    }

    it("encodes a durable receipt with the exact command fence") {
        static const char payload[] =
            "{\"capability\":\"ivr\",\"dialogId\":\"dialog-a\","
            "\"roomId\":\"room-a\",\"callId\":\"call-a\","
            "\"callGeneration\":7,\"operationGeneration\":9,"
            "\"text\":\"Welcome\"}";
        iris_control_provider_command_t decoded;
        iris_media_bridge_result_t result = {0};
        ProviderReceiptV1_view_t receipt;
        size_t command_size = 0u, receipt_size = 0u;
        uint8_t *command = encode_command(
            payload, "session-a", "17", TEST_SEMANTIC_FINGERPRINT,
            &command_size);
        uint8_t *encoded_receipt = NULL;
        check_equal(iris_control_provider_decode_command(
                        command, command_size, &decoded),
                    IVR_OK);
        result.status = IRIS_MEDIA_BRIDGE_ACCEPTED;
        check_equal(iris_control_provider_encode_receipt(
                        &decoded, &result, "turbomedia-a",
                        "2026-08-14T00:00:01Z", &encoded_receipt,
                        &receipt_size),
                    IVR_OK);
        check_true(ProviderReceiptV1_view_bind(&receipt, encoded_receipt,
                                               receipt_size));
        check_equal(ProviderReceiptV1_disposition_get(&receipt),
                    ProviderReceiptDisposition_DurableAccepted);
        CHECK_TEXT(ProviderReceiptV1, receipt, command_id, "command-a");
        CHECK_TEXT(ProviderReceiptV1, receipt, worker_id, "iris-worker-a");
        CHECK_TEXT(ProviderReceiptV1, receipt, dispatch_epoch, "17");
        CHECK_TEXT(ProviderReceiptV1, receipt, producer_id, "turbomedia-a");
        CHECK_TEXT(ProviderReceiptV1, receipt, error_code, "");
        CHECK_TEXT(ProviderReceiptV1, receipt, error_message, "");
        free(encoded_receipt);
        iris_control_provider_command_clear(&decoded);
        free(command);
    }

    it("encodes a tenant-fenced completion and accepts only its Iris ack") {
        iris_media_completion_t completion;
        ivr_media_command_result_t result;
        ProviderCompletionV1_view_t wire;
        iris_control_completion_ack_t decoded_ack;
        uint8_t *encoded = NULL;
        size_t encoded_size = 0u, encoded_ack_size = 0u;
        uint8_t *encoded_ack;
        memset(&completion, 0, sizeof(completion));
        memset(&result, 0, sizeof(result));
        snprintf(completion.command_id, sizeof(completion.command_id), "command-a");
        snprintf(completion.tenant_id, sizeof(completion.tenant_id), "tenant-a");
        snprintf(completion.provider_session_id,
                 sizeof(completion.provider_session_id), "session-a");
        snprintf(completion.iris_worker_id, sizeof(completion.iris_worker_id),
                 "iris-worker-a");
        snprintf(completion.correlation_id, sizeof(completion.correlation_id),
                 "correlation-a");
        completion.dispatch_epoch = 17u;
        snprintf(result.worker_id, sizeof(result.worker_id), "media-worker-a");
        snprintf(result.dialog_id, sizeof(result.dialog_id), "dialog-a");
        snprintf(result.room_id, sizeof(result.room_id), "room-a");
        snprintf(result.call_id, sizeof(result.call_id), "call-a");
        result.call_generation = 7u;
        result.operation_generation = 9u;
        result.status_code = IVR_OK;
        check_equal(iris_control_provider_encode_completion(
                        &completion, &result, "turbomedia", "turbomedia-a",
                        "completion-a", "2026-08-14T00:00:02Z", 42u,
                        &encoded, &encoded_size),
                    IVR_OK);
        check_true(ProviderCompletionV1_view_bind(&wire, encoded, encoded_size));
        CHECK_TEXT(ProviderCompletionV1, wire, tenant_id, "tenant-a");
        CHECK_TEXT(ProviderCompletionV1, wire, command_id, "command-a");
        CHECK_TEXT(ProviderCompletionV1, wire, dispatch_epoch, "17");
        CHECK_TEXT(ProviderCompletionV1, wire, result_json,
                   "{\"status\":\"completed\",\"mediaWorkerId\":"
                   "\"media-worker-a\",\"dialogId\":\"dialog-a\","
                   "\"roomId\":\"room-a\",\"callId\":\"call-a\","
                   "\"callGeneration\":7,\"operationGeneration\":9}");
        encoded_ack = encode_completion_ack(&encoded_ack_size);
        check_not_null(encoded_ack);
        check_equal(iris_control_provider_decode_completion_ack(
                        encoded_ack, encoded_ack_size, &completion,
                        "turbomedia", "iris-a", "completion-a", &decoded_ack),
                    IVR_OK);
        check_equal(decoded_ack.disposition,
                    ProviderCompletionAckDisposition_CompletionCommitted);
        check_equal(decoded_ack.committed_sequence, 23u);
        completion.dispatch_epoch = 18u;
        check_equal(iris_control_provider_decode_completion_ack(
                        encoded_ack, encoded_ack_size, &completion,
                        "turbomedia", "iris-a", "completion-a", &decoded_ack),
                    IVR_ESTATE);
        free(encoded_ack);
        free(encoded);
    }

    it("preserves a committed room terminal in the canonical completion") {
        iris_media_completion_t completion;
        ivr_media_command_result_t result;
        ProviderCompletionV1_view_t wire;
        uint8_t *encoded = NULL;
        size_t encoded_size = 0u;
        memset(&completion, 0, sizeof(completion));
        memset(&result, 0, sizeof(result));
        snprintf(completion.command_id, sizeof(completion.command_id),
                 "command-room-a");
        snprintf(completion.tenant_id, sizeof(completion.tenant_id), "tenant-a");
        snprintf(completion.provider_session_id,
                 sizeof(completion.provider_session_id), "session-a");
        snprintf(completion.iris_worker_id, sizeof(completion.iris_worker_id),
                 "iris-worker-a");
        snprintf(completion.correlation_id, sizeof(completion.correlation_id),
                 "room-a");
        completion.dispatch_epoch = 19u;
        snprintf(completion.terminal_status, sizeof(completion.terminal_status),
                 "succeeded");
        snprintf(completion.event_type, sizeof(completion.event_type),
                 "provider.conference.created");
        snprintf(completion.result_json, sizeof(completion.result_json),
                 "{\"roomId\":\"room-a\",\"roomGeneration\":1}");
        result.status_code = IVR_OK;
        check_equal(iris_control_provider_encode_completion(
                        &completion, &result, "turbomedia", "turbomedia-a",
                        "room-completion-a", "2026-08-14T00:00:02Z", 42u,
                        &encoded, &encoded_size),
                    IVR_OK);
        check_true(ProviderCompletionV1_view_bind(&wire, encoded, encoded_size));
        check_equal(ProviderCompletionV1_terminal_status_get(&wire),
                    ProviderTerminalStatus_Succeeded);
        CHECK_TEXT(ProviderCompletionV1, wire, event_type,
                   "provider.conference.created");
        CHECK_TEXT(ProviderCompletionV1, wire, result_json,
                   "{\"roomId\":\"room-a\",\"roomGeneration\":1}");
        free(encoded);
    }

    it("encodes a sequenced media event and fences its durable Iris ack") {
        ivr_media_event_t event;
        ProviderEventV1_view_t wire;
        iris_control_event_ack_t decoded_ack;
        uint8_t *encoded = NULL;
        size_t encoded_size = 0u, encoded_ack_size = 0u;
        uint8_t *encoded_ack;
        memset(&event, 0, sizeof(event));
        snprintf(event.event_id, sizeof(event.event_id), "event-a");
        snprintf(event.tenant_id, sizeof(event.tenant_id), "tenant-a");
        snprintf(event.provider_session_id, sizeof(event.provider_session_id),
                 "session-a");
        snprintf(event.dialog_id, sizeof(event.dialog_id), "dialog-a");
        snprintf(event.room_id, sizeof(event.room_id), "room-a");
        snprintf(event.call_id, sizeof(event.call_id), "call-a");
        event.call_generation = 7u;
        snprintf(event.input_id, sizeof(event.input_id), "input-a");
        snprintf(event.input_value, sizeof(event.input_value), "5");
        snprintf(event.event_type, sizeof(event.event_type), "dtmf.final");
        snprintf(event.payload_json, sizeof(event.payload_json),
                 "{\"digit\":\"5\"}");
        event.sequence = 31u;
        event.occurred_at_ms = 42u;
        check_equal(iris_control_provider_encode_event(
                        &event, "turbomedia", "turbomedia-a", "event-msg-a",
                        "2026-08-14T00:00:04Z", "2026-08-14T00:00:03Z",
                        &encoded, &encoded_size),
                    IVR_OK);
        check_true(ProviderEventV1_view_bind(&wire, encoded, encoded_size));
        CHECK_TEXT(ProviderEventV1, wire, tenant_id, "tenant-a");
        CHECK_TEXT(ProviderEventV1, wire, session_id, "session-a");
        CHECK_TEXT(ProviderEventV1, wire, aggregate_id, "dialog-a");
        CHECK_TEXT(ProviderEventV1, wire, sequence, "31");
        CHECK_TEXT(ProviderEventV1, wire, payload_json,
                   "{\"dialogId\":\"dialog-a\",\"roomId\":\"room-a\","
                   "\"callId\":\"call-a\",\"callGeneration\":7,"
                   "\"inputId\":\"input-a\",\"inputValue\":\"5\","
                   "\"payload\":{\"digit\":\"5\"}}");
        encoded_ack = encode_event_ack(&encoded_ack_size);
        check_not_null(encoded_ack);
        check_equal(iris_control_provider_decode_event_ack(
                        encoded_ack, encoded_ack_size, &event,
                        "turbomedia", "iris-a", "event-msg-a", &decoded_ack),
                    IVR_OK);
        check_equal(decoded_ack.disposition,
                    ProviderEventAckDisposition_Committed);
        check_equal(decoded_ack.committed_sequence, 37u);
        snprintf(event.tenant_id, sizeof(event.tenant_id), "tenant-b");
        check_equal(iris_control_provider_decode_event_ack(
                        encoded_ack, encoded_ack_size, &event,
                        "turbomedia", "iris-a", "event-msg-a", &decoded_ack),
                    IVR_ESTATE);
        free(encoded_ack);
        free(encoded);
    }

    it("encodes a revision-fenced query and accepts only its exact observation") {
        ProviderQueryV1_view_t query;
        iris_control_provider_observation_t decoded;
        uint8_t *encoded = NULL;
        size_t encoded_size = 0u, encoded_response_size = 0u;
        uint8_t *encoded_response;
        check_equal(iris_control_provider_encode_query(
                        "tenant-a", "turbomedia", "turbomedia-a", "query-a",
                        "expected_media_resources", "2026-08-14T00:00:06Z",
                        "2026-08-14T00:00:10Z", 19u, 32u, 16u,
                        "{\"schemaVersion\":1}", &encoded, &encoded_size),
                    IVR_OK);
        check_true(ProviderQueryV1_view_bind(&query, encoded, encoded_size));
        CHECK_TEXT(ProviderQueryV1, query, message_id, "query-a");
        CHECK_TEXT(ProviderQueryV1, query, partition_key, "tenant-a");
        CHECK_TEXT(ProviderQueryV1, query, expected_revision, "19");
        CHECK_TEXT(ProviderQueryV1, query, cursor, "32");
        encoded_response = encode_observation(&encoded_response_size);
        check_not_null(encoded_response);
        check_equal(iris_control_provider_decode_observation(
                        encoded_response, encoded_response_size,
                        "tenant-a", "turbomedia", "iris-a", "query-a",
                        "expected_media_resources", 32u, &decoded),
                    IVR_OK);
        check_equal(decoded.wire.status, ProviderQueryStatus_QueryOk);
        check_equal(decoded.revision, 19u);
        check_equal(decoded.next_cursor, 48u);
        check_contains(decoded.wire.payload_json, "\"resources\":[]");
        iris_control_provider_observation_clear(&decoded);
        check_equal(iris_control_provider_decode_observation(
                        encoded_response, encoded_response_size,
                        "tenant-b", "turbomedia", "iris-a", "query-a",
                        "expected_media_resources", 32u, &decoded),
                    IVR_ESTATE);
        free(encoded_response);
        free(encoded);
    }

    it("encodes a stable call offer and fences its session-bound response") {
        iris_control_call_offer_t offer;
        iris_control_session_bound_t decoded;
        ProviderCallOfferV1_view_t wire;
        uint8_t *encoded = NULL, *retry = NULL;
        size_t encoded_size = 0u, retry_size = 0u, response_size = 0u;
        uint8_t *response;
        memset(&offer, 0, sizeof(offer));
        snprintf(offer.ingress_event_id, sizeof(offer.ingress_event_id),
                 "ingress-call-a");
        snprintf(offer.tenant_id, sizeof(offer.tenant_id), "tenant-a");
        snprintf(offer.call_id, sizeof(offer.call_id), "call-a");
        snprintf(offer.created_at, sizeof(offer.created_at),
                 "2026-08-14T00:00:08Z");
        snprintf(offer.deadline_at, sizeof(offer.deadline_at),
                 "2026-08-14T00:00:13Z");
        snprintf(offer.transport, sizeof(offer.transport), "sip");
        snprintf(offer.source, sizeof(offer.source), "sip:alice@example.test");
        snprintf(offer.destination, sizeof(offer.destination),
                 "sip:sales@example.test");
        snprintf(offer.payload_json, sizeof(offer.payload_json),
                 "{\"schemaVersion\":1,\"routeKey\":\"sales-main\"}");
        offer.call_generation = 7u;
        check_equal(iris_control_provider_encode_call_offer(
                        &offer, "turbomedia", "turbomedia-a", &encoded,
                        &encoded_size),
                    IVR_OK);
        check_equal(iris_control_provider_encode_call_offer(
                        &offer, "turbomedia", "turbomedia-a", &retry,
                        &retry_size),
                    IVR_OK);
        check_equal(encoded_size, retry_size);
        check_equal(memcmp(encoded, retry, encoded_size), 0);
        check_true(ProviderCallOfferV1_view_bind(&wire, encoded, encoded_size));
        CHECK_TEXT(ProviderCallOfferV1, wire, message_id, "ingress-call-a");
        CHECK_TEXT(ProviderCallOfferV1, wire, correlation_id, "call-a");
        CHECK_TEXT(ProviderCallOfferV1, wire, partition_key, "call-a");
        CHECK_TEXT(ProviderCallOfferV1, wire, call_generation, "7");
        response = encode_session_bound(&response_size);
        check_not_null(response);
        check_equal(iris_control_provider_decode_session_bound(
                        response, response_size, &offer, "turbomedia",
                        "iris-a", &decoded),
                    IVR_OK);
        check_true(decoded.accepted);
        check_equal(decoded.bound_session_id, "session-a");
        offer.call_generation = 8u;
        check_equal(iris_control_provider_decode_session_bound(
                        response, response_size, &offer, "turbomedia",
                        "iris-a", &decoded),
                    IVR_ESTATE);
        free(response);
        free(retry);
        free(encoded);
    }

    it("rejects tampering, a wrong partition, and a zero dispatch epoch") {
        static const char payload[] =
            "{\"capability\":\"ivr\",\"dialogId\":\"dialog-a\","
            "\"roomId\":\"room-a\",\"callId\":\"call-a\","
            "\"callGeneration\":7,\"operationGeneration\":9,"
            "\"text\":\"Changed\"}";
        iris_control_provider_command_t decoded;
        size_t encoded_size = 0u;
        uint8_t *encoded = encode_command(
            payload, "session-a", "17", TEST_SEMANTIC_FINGERPRINT,
            &encoded_size);
        check_equal(iris_control_provider_decode_command(
                        encoded, encoded_size, &decoded),
                    IVR_ESTATE);
        free(encoded);
        encoded = encode_command(payload, "another-session", "17",
                                 TEST_SEMANTIC_FINGERPRINT, &encoded_size);
        check_equal(iris_control_provider_decode_command(
                        encoded, encoded_size, &decoded),
                    IVR_ESTATE);
        free(encoded);
        encoded = encode_command(payload, "session-a", "0",
                                 TEST_SEMANTIC_FINGERPRINT, &encoded_size);
        check_equal(iris_control_provider_decode_command(
                        encoded, encoded_size, &decoded),
                    IVR_ESTATE);
        free(encoded);
    }
}

#undef CHECK_TEXT
#undef SET_TEXT
