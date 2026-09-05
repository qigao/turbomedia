#include "iris_flowmq_provider_codec.h"

#include <flowmq_media_provider.h>
#include <tinytest.h>
#include <salts_error.h>
#include <json_parser.h>
#include <salts_str.h>

#include <stdio.h>
#include <string.h>

#define TEST_SEMANTIC_FINGERPRINT                                           \
    "sha256:710227af291421e97abf6de1cc8af21e2a50d653bb2855ecab4f549425fdeb4a"
#define TEST_ROOM_SEMANTIC_FINGERPRINT                                      \
    "sha256:77266bd0dee0f61e6bb3ce63fcaeaba7114c9b940838c4b495bc3d9424d0664a"

static int assign_string(tstr *target, const char *value) {
    *target = tstr_dup(value);
    return *target != NULL;
}

static int make_command(ProviderCommandV1_t *command, const char *payload,
                        const char *partition_key, const char *epoch,
                        const char *fingerprint) {
    ProviderCommandV1_init(command);
    command->schema_version = 1u;
    command->message_kind = ProviderMessageKind_Command;
    return assign_string(&command->message_id, "command-a") &&
           assign_string(&command->correlation_id, "correlation-a") &&
           assign_string(&command->causation_id, "causation-a") &&
           assign_string(&command->tenant_id, "tenant-a") &&
           assign_string(&command->provider_id, "turbomedia") &&
           assign_string(&command->session_id, "session-a") &&
           assign_string(&command->partition_key, partition_key) &&
           assign_string(&command->producer_id, "iris-a") &&
           assign_string(&command->created_at, "2026-08-14T00:00:00Z") &&
           assign_string(&command->deadline_at, "2099-01-01T00:00:00Z") &&
           assign_string(&command->command_id, "command-a") &&
           assign_string(&command->command_type, "media.play") &&
           assign_string(&command->worker_id, "iris-worker-a") &&
           assign_string(&command->dispatch_epoch, epoch) &&
           assign_string(&command->semantic_fingerprint, fingerprint) &&
           assign_string(&command->payload_json, payload);
}

static uint8_t *encode_command(const char *payload, const char *partition_key,
                               const char *epoch, const char *fingerprint,
                               size_t *encoded_size) {
    ProviderCommandV1_t command;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint8_t *encoded = NULL;
    if (!make_command(&command, payload, partition_key, epoch, fingerprint) ||
        ProviderCommandV1_to_bin(&command, &encoded, encoded_size, &error) !=
            DATA_BIND_OK) {
        tbe_typed_serialized_free(encoded);
        encoded = NULL;
        *encoded_size = 0u;
    }
    ProviderCommandV1_clear(&command);
    return encoded;
}

static const char *json_string_field(const json_value_t *object,
                                     const char *name) {
    json_value_t *value = json_object_get(object, name);
    return value && json_type(value) == JSON_STRING
               ? json_string(value)
               : NULL;
}

spec("RoomService Iris FlowMQ provider codec") {
    static DataBind *codec;

    before_all() {
        DataBindError error = DATA_BIND_ERROR_INIT;
        check_equal(FlowMqMediaProviderV1_codec_create(&codec, &error),
                     DATA_BIND_OK);
        check_not_null(codec);
    }

    after_all() {
        data_bind_free(codec);
        codec = NULL;
    }

    it("decodes the canonical command and preserves its exact bridge fields") {
        static const char payload[] =
            "{\"capability\":\"ivr\",\"dialogId\":\"dialog-a\"," 
            "\"roomId\":\"room-a\",\"callId\":\"call-a\"," 
            "\"callGeneration\":7,\"operationGeneration\":9," 
            "\"text\":\"Welcome\"}";
        iris_flowmq_provider_command_t decoded;
        json_value_t *root = NULL;
        json_value_t *data;
        size_t encoded_size = 0u;
        uint8_t *encoded = encode_command(
            payload, "session-a", "17", TEST_SEMANTIC_FINGERPRINT,
            &encoded_size);
        check_not_null(encoded);
        check_equal(iris_flowmq_provider_decode_command(
                         codec, encoded, encoded_size, &decoded),
                     IVR_OK);
        check_equal(decoded.dispatch_epoch, 17u);
        check_equal(decoded.wire.semantic_fingerprint,
                     TEST_SEMANTIC_FINGERPRINT);
        check_equal(((root = json_parse((const char *)((const uint8_t *)decoded.bridge_json), decoded.bridge_json_size)) ? 0 : -1),
                     0);
        check_not_null(root);
        check_equal(json_string_field(root, "commandId"), "command-a");
        check_equal(json_string_field(root, "tenantId"), "tenant-a");
        check_equal(json_string_field(root, "workerId"), "iris-worker-a");
        check_equal((uint64_t)json_number(
                        json_object_get(root, "schemaVersion")),
                    UINT64_C(2));
        data = json_object_get(root, "data");
        check_not_null(data);
        check_equal(json_string_field(data, "dialogId"), "dialog-a");
        check_equal(json_string_field(data, "text"), "Welcome");
        json_free(root);
        root = NULL;
        iris_flowmq_provider_command_clear(&decoded);
        tbe_typed_serialized_free(encoded);
    }

    it("maps canonical room commands to the room bridge schema") {
        static const char payload[] =
            "{\"capability\":\"room\",\"roomId\":\"room-a\","
            "\"roomGeneration\":1}";
        iris_flowmq_provider_command_t decoded;
        json_value_t *root = NULL;
        size_t encoded_size = 0u;
        uint8_t *encoded = encode_command(
            payload, "session-a", "17", TEST_ROOM_SEMANTIC_FINGERPRINT,
            &encoded_size);
        check_not_null(encoded);
        check_equal(iris_flowmq_provider_decode_command(
                         codec, encoded, encoded_size, &decoded),
                    IVR_OK);
        check_equal(((root = json_parse((const char *)((const uint8_t *)decoded.bridge_json), decoded.bridge_json_size)) ? 0 : -1),
                    0);
        check_not_null(root);
        check_equal((uint64_t)json_number(
                        json_object_get(root, "schemaVersion")),
                    UINT64_C(3));
        json_free(root);
        root = NULL;
        iris_flowmq_provider_command_clear(&decoded);
        tbe_typed_serialized_free(encoded);
    }

    it("encodes a durable receipt with the exact command fence") {
        static const char payload[] =
            "{\"capability\":\"ivr\",\"dialogId\":\"dialog-a\"," 
            "\"roomId\":\"room-a\",\"callId\":\"call-a\"," 
            "\"callGeneration\":7,\"operationGeneration\":9," 
            "\"text\":\"Welcome\"}";
        iris_flowmq_provider_command_t decoded;
        iris_media_bridge_result_t result = {0};
        ProviderReceiptV1_t receipt;
        DataBindError error = DATA_BIND_ERROR_INIT;
        size_t command_size = 0u;
        size_t receipt_size = 0u;
        uint8_t *command = encode_command(
            payload, "session-a", "17", TEST_SEMANTIC_FINGERPRINT,
            &command_size);
        uint8_t *encoded_receipt = NULL;
        check_equal(iris_flowmq_provider_decode_command(
                         codec, command, command_size, &decoded),
                     IVR_OK);
        result.status = IRIS_MEDIA_BRIDGE_ACCEPTED;
        result.error_code = "";
        result.error_message = "";
        check_equal(iris_flowmq_provider_encode_receipt(
                         &decoded, &result, "turbomedia-a",
                         "2026-08-14T00:00:01Z", &encoded_receipt,
                         &receipt_size),
                     IVR_OK);
        ProviderReceiptV1_init(&receipt);
        check_equal(ProviderReceiptV1_from_bin(codec, &receipt,
                                                encoded_receipt, receipt_size,
                                                &error),
                     DATA_BIND_OK);
        check_equal(receipt.disposition,
                     ProviderReceiptDisposition_DurableAccepted);
        check_equal(receipt.command_id, "command-a");
        check_equal(receipt.worker_id, "iris-worker-a");
        check_equal(receipt.dispatch_epoch, "17");
        check_equal(receipt.producer_id, "turbomedia-a");
        ProviderReceiptV1_clear(&receipt);
        tbe_typed_serialized_free(encoded_receipt);
        iris_flowmq_provider_command_clear(&decoded);
        tbe_typed_serialized_free(command);
    }

    it("encodes a tenant-fenced completion and accepts only its Iris ack") {
        iris_media_completion_t completion;
        ivr_media_command_result_t result;
        ProviderCompletionV1_t wire;
        ProviderCompletionAckV1_t ack;
        iris_flowmq_completion_ack_t decoded_ack;
        DataBindError error = DATA_BIND_ERROR_INIT;
        uint8_t *encoded = NULL;
        uint8_t *encoded_ack = NULL;
        size_t encoded_size = 0u;
        size_t encoded_ack_size = 0u;
        memset(&completion, 0, sizeof(completion));
        memset(&result, 0, sizeof(result));
        snprintf(completion.command_id, sizeof(completion.command_id),
                 "%s", "command-a");
        snprintf(completion.tenant_id, sizeof(completion.tenant_id),
                 "%s", "tenant-a");
        snprintf(completion.provider_session_id,
                 sizeof(completion.provider_session_id), "%s", "session-a");
        snprintf(completion.iris_worker_id,
                 sizeof(completion.iris_worker_id), "%s", "iris-worker-a");
        snprintf(completion.correlation_id,
                 sizeof(completion.correlation_id), "%s", "correlation-a");
        completion.dispatch_epoch = 17u;
        snprintf(result.worker_id, sizeof(result.worker_id), "%s",
                 "media-worker-a");
        snprintf(result.dialog_id, sizeof(result.dialog_id), "%s", "dialog-a");
        snprintf(result.room_id, sizeof(result.room_id), "%s", "room-a");
        snprintf(result.call_id, sizeof(result.call_id), "%s", "call-a");
        result.call_generation = 7u;
        result.operation_generation = 9u;
        result.status_code = IVR_OK;
        check_equal(iris_flowmq_provider_encode_completion(
                         &completion, &result, "turbomedia", "turbomedia-a",
                         "completion-a", "2026-08-14T00:00:02Z", 42u,
                         &encoded, &encoded_size),
                     IVR_OK);
        ProviderCompletionV1_init(&wire);
        check_equal(ProviderCompletionV1_from_bin(
                         codec, &wire, encoded, encoded_size, &error),
                     DATA_BIND_OK);
        check_equal(wire.tenant_id, "tenant-a");
        check_equal(wire.command_id, "command-a");
        check_equal(wire.dispatch_epoch, "17");
        check_equal(wire.result_json,
                     "{\"status\":\"completed\",\"mediaWorkerId\":"
                     "\"media-worker-a\",\"dialogId\":\"dialog-a\"," 
                     "\"roomId\":\"room-a\",\"callId\":\"call-a\"," 
                     "\"callGeneration\":7,\"operationGeneration\":9}");
        ProviderCompletionV1_clear(&wire);

        ProviderCompletionAckV1_init(&ack);
        ack.schema_version = 1u;
        ack.message_kind = ProviderMessageKind_CompletionAck;
        ack.disposition = ProviderCompletionAckDisposition_CompletionCommitted;
        check_true(assign_string(&ack.message_id, "completion-a.completion-ack"));
        check_true(assign_string(&ack.correlation_id, "correlation-a"));
        check_true(assign_string(&ack.causation_id, "completion-a"));
        check_true(assign_string(&ack.tenant_id, "tenant-a"));
        check_true(assign_string(&ack.provider_id, "turbomedia"));
        check_true(assign_string(&ack.session_id, "session-a"));
        check_true(assign_string(&ack.partition_key, "session-a"));
        check_true(assign_string(&ack.producer_id, "iris-a"));
        check_true(assign_string(&ack.created_at, "2026-08-14T00:00:03Z"));
        check_true(assign_string(&ack.deadline_at, ""));
        check_true(assign_string(&ack.command_id, "command-a"));
        check_true(assign_string(&ack.dispatch_epoch, "17"));
        check_true(assign_string(&ack.committed_sequence, "23"));
        check_true(assign_string(&ack.error_code, ""));
        check_true(assign_string(&ack.error_message, ""));
        check_equal(ProviderCompletionAckV1_to_bin(
                         &ack, &encoded_ack, &encoded_ack_size, &error),
                     DATA_BIND_OK);
        check_equal(iris_flowmq_provider_decode_completion_ack(
                         codec, encoded_ack, encoded_ack_size, &completion,
                         "turbomedia", "iris-a", "completion-a", &decoded_ack),
                     IVR_OK);
        check_equal(decoded_ack.disposition,
                     ProviderCompletionAckDisposition_CompletionCommitted);
        check_equal(decoded_ack.committed_sequence, 23u);
        completion.dispatch_epoch = 18u;
        check_equal(iris_flowmq_provider_decode_completion_ack(
                         codec, encoded_ack, encoded_ack_size, &completion,
                         "turbomedia", "iris-a", "completion-a", &decoded_ack),
                     IVR_ESTATE);
        ProviderCompletionAckV1_clear(&ack);
        tbe_typed_serialized_free(encoded_ack);
        tbe_typed_serialized_free(encoded);
    }

    it("preserves a committed room terminal in the canonical completion") {
        iris_media_completion_t completion;
        ivr_media_command_result_t result;
        ProviderCompletionV1_t wire;
        DataBindError error = DATA_BIND_ERROR_INIT;
        uint8_t *encoded = NULL;
        size_t encoded_size = 0u;
        memset(&completion, 0, sizeof(completion));
        memset(&result, 0, sizeof(result));
        snprintf(completion.command_id, sizeof(completion.command_id),
                 "command-room-a");
        snprintf(completion.tenant_id, sizeof(completion.tenant_id),
                 "tenant-a");
        snprintf(completion.provider_session_id,
                 sizeof(completion.provider_session_id), "session-a");
        snprintf(completion.iris_worker_id,
                 sizeof(completion.iris_worker_id), "iris-worker-a");
        snprintf(completion.correlation_id,
                 sizeof(completion.correlation_id), "room-a");
        completion.dispatch_epoch = 19u;
        snprintf(completion.terminal_status,
                 sizeof(completion.terminal_status), "succeeded");
        snprintf(completion.event_type, sizeof(completion.event_type),
                 "provider.conference.created");
        snprintf(completion.result_json, sizeof(completion.result_json),
                 "{\"roomId\":\"room-a\",\"roomGeneration\":1}");
        result.status_code = IVR_OK;

        check_equal(iris_flowmq_provider_encode_completion(
                         &completion, &result, "turbomedia", "turbomedia-a",
                         "room-completion-a", "2026-08-14T00:00:02Z", 42u,
                         &encoded, &encoded_size),
                     IVR_OK);
        ProviderCompletionV1_init(&wire);
        check_equal(ProviderCompletionV1_from_bin(
                         codec, &wire, encoded, encoded_size, &error),
                     DATA_BIND_OK);
        check_equal(wire.terminal_status,
                    ProviderTerminalStatus_Succeeded);
        check_equal(wire.event_type, "provider.conference.created");
        check_equal(wire.result_json,
                    "{\"roomId\":\"room-a\",\"roomGeneration\":1}");
        ProviderCompletionV1_clear(&wire);
        tbe_typed_serialized_free(encoded);
    }

    it("encodes a sequenced media event and fences its durable Iris ack") {
        ivr_media_event_t event;
        ProviderEventV1_t wire;
        ProviderEventAckV1_t ack;
        iris_flowmq_event_ack_t decoded_ack;
        DataBindError error = DATA_BIND_ERROR_INIT;
        uint8_t *encoded = NULL;
        uint8_t *encoded_ack = NULL;
        size_t encoded_size = 0u;
        size_t encoded_ack_size = 0u;
        memset(&event, 0, sizeof(event));
        snprintf(event.event_id, sizeof(event.event_id), "event-a");
        snprintf(event.tenant_id, sizeof(event.tenant_id), "tenant-a");
        snprintf(event.provider_session_id,
                 sizeof(event.provider_session_id), "session-a");
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
        check_equal(iris_flowmq_provider_encode_event(
                         &event, "turbomedia", "turbomedia-a", "event-msg-a",
                         "2026-08-14T00:00:04Z", "2026-08-14T00:00:03Z",
                         &encoded, &encoded_size),
                     IVR_OK);
        ProviderEventV1_init(&wire);
        check_equal(ProviderEventV1_from_bin(codec, &wire, encoded,
                                              encoded_size, &error),
                     DATA_BIND_OK);
        check_equal(wire.tenant_id, "tenant-a");
        check_equal(wire.session_id, "session-a");
        check_equal(wire.aggregate_id, "dialog-a");
        check_equal(wire.sequence, "31");
        check_equal(wire.payload_json,
                    "{\"dialogId\":\"dialog-a\",\"roomId\":\"room-a\","
                    "\"callId\":\"call-a\",\"callGeneration\":7,"
                    "\"inputId\":\"input-a\",\"inputValue\":\"5\","
                    "\"payload\":{\"digit\":\"5\"}}");
        ProviderEventV1_clear(&wire);

        ProviderEventAckV1_init(&ack);
        ack.schema_version = 1u;
        ack.message_kind = ProviderMessageKind_EventAck;
        ack.disposition = ProviderEventAckDisposition_Committed;
        check_true(assign_string(&ack.message_id, "event-msg-a.event-ack"));
        check_true(assign_string(&ack.correlation_id, "dialog-a"));
        check_true(assign_string(&ack.causation_id, "event-msg-a"));
        check_true(assign_string(&ack.tenant_id, "tenant-a"));
        check_true(assign_string(&ack.provider_id, "turbomedia"));
        check_true(assign_string(&ack.session_id, "session-a"));
        check_true(assign_string(&ack.partition_key, "session-a"));
        check_true(assign_string(&ack.producer_id, "iris-a"));
        check_true(assign_string(&ack.created_at, "2026-08-14T00:00:05Z"));
        check_true(assign_string(&ack.deadline_at, ""));
        check_true(assign_string(&ack.event_id, "event-a"));
        check_true(assign_string(&ack.committed_sequence, "37"));
        check_true(assign_string(&ack.error_code, ""));
        check_true(assign_string(&ack.error_message, ""));
        check_equal(ProviderEventAckV1_to_bin(
                         &ack, &encoded_ack, &encoded_ack_size, &error),
                     DATA_BIND_OK);
        check_equal(iris_flowmq_provider_decode_event_ack(
                         codec, encoded_ack, encoded_ack_size, &event,
                         "turbomedia", "iris-a", "event-msg-a", &decoded_ack),
                     IVR_OK);
        check_equal(decoded_ack.disposition,
                     ProviderEventAckDisposition_Committed);
        check_equal(decoded_ack.committed_sequence, 37u);
        snprintf(event.tenant_id, sizeof(event.tenant_id), "tenant-b");
        check_equal(iris_flowmq_provider_decode_event_ack(
                         codec, encoded_ack, encoded_ack_size, &event,
                         "turbomedia", "iris-a", "event-msg-a", &decoded_ack),
                     IVR_ESTATE);
        ProviderEventAckV1_clear(&ack);
        tbe_typed_serialized_free(encoded_ack);
        tbe_typed_serialized_free(encoded);
    }

    it("encodes a revision-fenced query and accepts only its exact observation") {
        ProviderQueryV1_t query;
        ProviderObservationV1_t response;
        iris_flowmq_provider_observation_t decoded;
        DataBindError error = DATA_BIND_ERROR_INIT;
        uint8_t *encoded = NULL;
        uint8_t *encoded_response = NULL;
        size_t encoded_size = 0u;
        size_t encoded_response_size = 0u;
        check_equal(iris_flowmq_provider_encode_query(
                         "tenant-a", "turbomedia", "turbomedia-a", "query-a",
                         "expected_media_resources", "2026-08-14T00:00:06Z",
                         "2026-08-14T00:00:10Z", 19u, 32u, 16u,
                         "{\"schemaVersion\":1}", &encoded, &encoded_size),
                     IVR_OK);
        ProviderQueryV1_init(&query);
        check_equal(ProviderQueryV1_from_bin(codec, &query, encoded,
                                              encoded_size, &error),
                     DATA_BIND_OK);
        check_equal(query.message_id, "query-a");
        check_equal(query.partition_key, "tenant-a");
        check_equal(query.expected_revision, "19");
        check_equal(query.cursor, "32");
        ProviderQueryV1_clear(&query);

        ProviderObservationV1_init(&response);
        response.schema_version = 1u;
        response.message_kind = ProviderMessageKind_Observation;
        response.status = ProviderQueryStatus_QueryOk;
        response.has_more = true;
        check_true(assign_string(&response.message_id, "observation-a"));
        check_true(assign_string(&response.correlation_id, "query-a"));
        check_true(assign_string(&response.causation_id, "query-a"));
        check_true(assign_string(&response.tenant_id, "tenant-a"));
        check_true(assign_string(&response.provider_id, "turbomedia"));
        check_true(assign_string(&response.session_id, ""));
        check_true(assign_string(&response.partition_key, "tenant-a"));
        check_true(assign_string(&response.producer_id, "iris-a"));
        check_true(assign_string(&response.created_at, "2026-08-14T00:00:07Z"));
        check_true(assign_string(&response.deadline_at, ""));
        check_true(assign_string(&response.query_id, "query-a"));
        check_true(assign_string(&response.observation_type,
                                 "expected_media_resources"));
        check_true(assign_string(&response.revision, "19"));
        check_true(assign_string(&response.cursor, "32"));
        check_true(assign_string(&response.next_cursor, "48"));
        check_true(assign_string(&response.payload_json,
                                 "{\"schemaVersion\":1,\"resources\":[]}"));
        check_true(assign_string(&response.error_code, ""));
        check_true(assign_string(&response.error_message, ""));
        check_equal(ProviderObservationV1_to_bin(
                         &response, &encoded_response, &encoded_response_size,
                         &error),
                     DATA_BIND_OK);
        check_equal(iris_flowmq_provider_decode_observation(
                         codec, encoded_response, encoded_response_size,
                         "tenant-a", "turbomedia", "iris-a", "query-a",
                         "expected_media_resources", 32u, &decoded),
                     IVR_OK);
        check_equal(decoded.wire.status, ProviderQueryStatus_QueryOk);
        check_equal(decoded.revision, 19u);
        check_equal(decoded.next_cursor, 48u);
        check_contains(decoded.wire.payload_json, "\"resources\":[]");
        iris_flowmq_provider_observation_clear(&decoded);
        check_equal(iris_flowmq_provider_decode_observation(
                         codec, encoded_response, encoded_response_size,
                         "tenant-b", "turbomedia", "iris-a", "query-a",
                         "expected_media_resources", 32u, &decoded),
                     IVR_ESTATE);
        ProviderObservationV1_clear(&response);
        tbe_typed_serialized_free(encoded_response);
        tbe_typed_serialized_free(encoded);
    }

    it("encodes a stable call offer and fences its session-bound response") {
        iris_flowmq_call_offer_t offer;
        iris_flowmq_session_bound_t decoded;
        ProviderCallOfferV1_t wire;
        ProviderSessionBoundV1_t response;
        DataBindError error = DATA_BIND_ERROR_INIT;
        uint8_t *encoded = NULL;
        uint8_t *retry = NULL;
        uint8_t *encoded_response = NULL;
        size_t encoded_size = 0u;
        size_t retry_size = 0u;
        size_t encoded_response_size = 0u;
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
        snprintf(offer.source, sizeof(offer.source),
                 "sip:alice@example.test");
        snprintf(offer.destination, sizeof(offer.destination),
                 "sip:sales@example.test");
        snprintf(offer.payload_json, sizeof(offer.payload_json),
                 "{\"schemaVersion\":1,\"routeKey\":\"sales-main\"}");
        offer.call_generation = 7u;

        check_equal(iris_flowmq_provider_encode_call_offer(
                         &offer, "turbomedia", "turbomedia-a", &encoded,
                         &encoded_size),
                     IVR_OK);
        check_equal(iris_flowmq_provider_encode_call_offer(
                         &offer, "turbomedia", "turbomedia-a", &retry,
                         &retry_size),
                     IVR_OK);
        check_equal(encoded_size, retry_size);
        check_equal(memcmp(encoded, retry, encoded_size), 0);
        ProviderCallOfferV1_init(&wire);
        check_equal(ProviderCallOfferV1_from_bin(
                         codec, &wire, encoded, encoded_size, &error),
                     DATA_BIND_OK);
        check_equal(wire.message_id, "ingress-call-a");
        check_equal(wire.correlation_id, "call-a");
        check_equal(wire.partition_key, "call-a");
        check_equal(wire.call_generation, "7");
        ProviderCallOfferV1_clear(&wire);

        ProviderSessionBoundV1_init(&response);
        response.schema_version = 1u;
        response.message_kind = ProviderMessageKind_SessionBound;
        response.accepted = true;
        check_true(assign_string(&response.message_id, "session-bound-a"));
        check_true(assign_string(&response.correlation_id, "call-a"));
        check_true(assign_string(&response.causation_id, "ingress-call-a"));
        check_true(assign_string(&response.tenant_id, "tenant-a"));
        check_true(assign_string(&response.provider_id, "turbomedia"));
        check_true(assign_string(&response.session_id, "session-a"));
        check_true(assign_string(&response.partition_key, "call-a"));
        check_true(assign_string(&response.producer_id, "iris-a"));
        check_true(assign_string(&response.created_at,
                                 "2026-08-14T00:00:09Z"));
        check_true(assign_string(&response.deadline_at,
                                 "2026-08-14T00:00:13Z"));
        check_true(assign_string(&response.ingress_event_id,
                                 "ingress-call-a"));
        check_true(assign_string(&response.call_id, "call-a"));
        check_true(assign_string(&response.call_generation, "7"));
        check_true(assign_string(&response.bound_session_id, "session-a"));
        check_true(assign_string(&response.error_code, ""));
        check_true(assign_string(&response.error_message, ""));
        check_equal(ProviderSessionBoundV1_to_bin(
                         &response, &encoded_response, &encoded_response_size,
                         &error),
                     DATA_BIND_OK);
        check_equal(iris_flowmq_provider_decode_session_bound(
                         codec, encoded_response, encoded_response_size,
                         &offer, "turbomedia", "iris-a", &decoded),
                     IVR_OK);
        check_true(decoded.accepted);
        check_equal(decoded.bound_session_id, "session-a");
        offer.call_generation = 8u;
        check_equal(iris_flowmq_provider_decode_session_bound(
                         codec, encoded_response, encoded_response_size,
                         &offer, "turbomedia", "iris-a", &decoded),
                     IVR_ESTATE);
        ProviderSessionBoundV1_clear(&response);
        tbe_typed_serialized_free(encoded_response);
        tbe_typed_serialized_free(retry);
        tbe_typed_serialized_free(encoded);
    }

    it("rejects tampering, a wrong partition, and a zero dispatch epoch") {
        static const char payload[] =
            "{\"capability\":\"ivr\",\"dialogId\":\"dialog-a\"," 
            "\"roomId\":\"room-a\",\"callId\":\"call-a\"," 
            "\"callGeneration\":7,\"operationGeneration\":9," 
            "\"text\":\"Changed\"}";
        iris_flowmq_provider_command_t decoded;
        size_t encoded_size = 0u;
        uint8_t *encoded = encode_command(
            payload, "session-a", "17", TEST_SEMANTIC_FINGERPRINT,
            &encoded_size);
        check_equal(iris_flowmq_provider_decode_command(
                         codec, encoded, encoded_size, &decoded),
                     IVR_ESTATE);
        tbe_typed_serialized_free(encoded);
        encoded = encode_command(payload, "another-session", "17",
                                 TEST_SEMANTIC_FINGERPRINT, &encoded_size);
        check_equal(iris_flowmq_provider_decode_command(
                         codec, encoded, encoded_size, &decoded),
                     IVR_ESTATE);
        tbe_typed_serialized_free(encoded);
        encoded = encode_command(payload, "session-a", "0",
                                 TEST_SEMANTIC_FINGERPRINT, &encoded_size);
        check_equal(iris_flowmq_provider_decode_command(
                         codec, encoded, encoded_size, &decoded),
                     IVR_ESTATE);
        tbe_typed_serialized_free(encoded);
    }
}
