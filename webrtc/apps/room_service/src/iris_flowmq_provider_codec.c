#include "iris_flowmq_provider_codec.h"

#include <flowmq_media_provider.h>
#include <turbo_crypto.h>
#include <turbo_error.h>
#include <turbo_parser.h>
#include <turbo_str.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IRIS_FLOWMQ_BRIDGE_SCHEMA_VERSION UINT64_C(2)

static int add_member(json_value_t *object, const char *name,
                      json_value_t *value) {
    if (!value || !turbo_json_object_add_checked(object, name, value)) {
        turbo_free_json(&value);
        return 0;
    }
    return 1;
}

static json_value_t *wire_string(tstr value) {
    return turbo_json_create_string_n(value ? value : "",
                                      value ? tstr_len(value) : 0u);
}

static int hash_field(turbo_crypto_sha256_ctx_t *context, tstr value) {
    uint64_t length = value ? (uint64_t)tstr_len(value) : 0u;
    uint8_t encoded_length[sizeof(length)];
    size_t index;
    for (index = 0u; index < sizeof(encoded_length); ++index) {
        encoded_length[index] = (uint8_t)(length >> (index * 8u));
    }
    return turbo_crypto_sha256_update(context, encoded_length,
                                      sizeof(encoded_length)) == 0 &&
           (length == 0u ||
            turbo_crypto_sha256_update(context, value, (size_t)length) == 0);
}

static int command_fingerprint_valid(const ProviderCommandV1_t *command) {
    static const char hex[] = "0123456789abcdef";
    turbo_crypto_sha256_ctx_t context;
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    char expected[sizeof("sha256:") - 1u + TURBO_CRYPTO_SHA256_SIZE * 2u + 1u];
    size_t index;
    if (!command || !command->semantic_fingerprint ||
        turbo_crypto_sha256_init(&context) != 0 ||
        !hash_field(&context, command->tenant_id) ||
        !hash_field(&context, command->session_id) ||
        !hash_field(&context, command->command_type) ||
        !hash_field(&context, command->provider_id) ||
        !hash_field(&context, command->correlation_id) ||
        !hash_field(&context, command->causation_id) ||
        !hash_field(&context, command->deadline_at) ||
        !hash_field(&context, command->payload_json) ||
        turbo_crypto_sha256_final(&context, digest) != 0) {
        return 0;
    }
    memcpy(expected, "sha256:", sizeof("sha256:") - 1u);
    for (index = 0u; index < sizeof(digest); ++index) {
        expected[sizeof("sha256:") - 1u + index * 2u] =
            hex[digest[index] >> 4u];
        expected[sizeof("sha256:") + index * 2u] =
            hex[digest[index] & 0x0fu];
    }
    expected[sizeof(expected) - 1u] = '\0';
    return tstr_len(command->semantic_fingerprint) == sizeof(expected) - 1u &&
           turbo_crypto_verify(expected, command->semantic_fingerprint,
                               sizeof(expected) - 1u) == 0;
}

static char *build_bridge_json(const ProviderCommandV1_t *command,
                               uint64_t dispatch_epoch, size_t *out_size) {
    json_value_t *root = NULL;
    json_value_t *payload = NULL;
    char *json = NULL;
    if (turbo_parse_json((const uint8_t *)command->payload_json,
                         tstr_len(command->payload_json), &payload) != 0 ||
        !payload || turbo_json_type(payload) != TURBO_JSON_OBJECT) {
        turbo_free_json(&payload);
        return NULL;
    }
    root = turbo_json_create_object();
    if (!root ||
        !add_member(root, "schemaVersion",
                    turbo_json_create_uint64(IRIS_FLOWMQ_BRIDGE_SCHEMA_VERSION)) ||
        !add_member(root, "commandId", wire_string(command->command_id)) ||
        !add_member(root, "tenantId", wire_string(command->tenant_id)) ||
        !add_member(root, "sessionId", wire_string(command->session_id)) ||
        !add_member(root, "type", wire_string(command->command_type)) ||
        !add_member(root, "provider", wire_string(command->provider_id)) ||
        !add_member(root, "correlationId", wire_string(command->correlation_id)) ||
        !add_member(root, "causationId", wire_string(command->causation_id)) ||
        !add_member(root, "deadline", wire_string(command->deadline_at)) ||
        !add_member(root, "workerId", wire_string(command->worker_id)) ||
        !add_member(root, "dispatchEpoch",
                    turbo_json_create_uint64(dispatch_epoch)) ||
        !add_member(root, "data", payload)) {
        turbo_free_json(&payload);
        turbo_free_json(&root);
        return NULL;
    }
    payload = NULL;
    json = turbo_json_serialize(root, out_size);
    turbo_free_json(&root);
    return json;
}

void iris_flowmq_provider_command_init(
    iris_flowmq_provider_command_t *command) {
    if (!command) return;
    memset(command, 0, sizeof(*command));
    ProviderCommandV1_init(&command->wire);
}

void iris_flowmq_provider_command_clear(
    iris_flowmq_provider_command_t *command) {
    if (!command) return;
    turbo_json_serialize_free(command->bridge_json);
    command->bridge_json = NULL;
    command->bridge_json_size = 0u;
    command->dispatch_epoch = 0u;
    ProviderCommandV1_clear(&command->wire);
}

ivr_status_t iris_flowmq_provider_decode_command(
    DataBind *codec, const void *encoded, size_t encoded_size,
    iris_flowmq_provider_command_t *out) {
    flowmq_media_provider_limits_t limits = FLOWMQ_MEDIA_PROVIDER_LIMITS_INIT;
    ProviderMessageKind_t kind = ProviderMessageKind_Receipt;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint64_t dispatch_epoch = 0u;
    if (!codec || !encoded || encoded_size == 0u || !out) return IVR_EINVAL;
    iris_flowmq_provider_command_init(out);
    if (flowmq_media_provider_peek_kind(encoded, encoded_size, &kind) !=
            TURBO_OK ||
        kind != ProviderMessageKind_Command ||
        ProviderCommandV1_from_bin(codec, &out->wire, encoded, encoded_size,
                                   &error) != DATA_BIND_OK ||
        flowmq_media_provider_validate_command(&out->wire, &limits) != TURBO_OK ||
        strcmp(out->wire.message_id, out->wire.command_id) != 0 ||
        strcmp(out->wire.partition_key, out->wire.session_id) != 0 ||
        flowmq_media_provider_parse_u64(out->wire.dispatch_epoch,
                                        &dispatch_epoch) != TURBO_OK ||
        dispatch_epoch == 0u || !command_fingerprint_valid(&out->wire)) {
        iris_flowmq_provider_command_clear(out);
        return IVR_ESTATE;
    }
    out->bridge_json =
        build_bridge_json(&out->wire, dispatch_epoch, &out->bridge_json_size);
    if (!out->bridge_json) {
        iris_flowmq_provider_command_clear(out);
        return IVR_ENOSPC;
    }
    out->dispatch_epoch = dispatch_epoch;
    return IVR_OK;
}

static ProviderReceiptDisposition_t receipt_disposition(
    iris_media_bridge_status_t status) {
    switch (status) {
        case IRIS_MEDIA_BRIDGE_ACCEPTED:
        case IRIS_MEDIA_BRIDGE_TERMINAL:
            return ProviderReceiptDisposition_DurableAccepted;
        case IRIS_MEDIA_BRIDGE_DUPLICATE:
        case IRIS_MEDIA_BRIDGE_TERMINAL_REPLAY:
            return ProviderReceiptDisposition_Duplicate;
        case IRIS_MEDIA_BRIDGE_CONFLICT:
            return ProviderReceiptDisposition_Conflict;
        case IRIS_MEDIA_BRIDGE_FULL:
            return ProviderReceiptDisposition_CapacityExceeded;
        case IRIS_MEDIA_BRIDGE_UNAVAILABLE:
            return ProviderReceiptDisposition_NotReady;
        case IRIS_MEDIA_BRIDGE_INVALID:
        case IRIS_MEDIA_BRIDGE_EXPIRED:
        case IRIS_MEDIA_BRIDGE_INTERNAL:
        default:
            return ProviderReceiptDisposition_Rejected;
    }
}

static int assign(tstr *target, const char *value) {
    *target = tstr_dup(value ? value : "");
    return *target != NULL;
}

static int format_u64(uint64_t value, char *out, size_t capacity) {
    int written = snprintf(out, capacity, "%llu",
                           (unsigned long long)value);
    return written > 0 && (size_t)written < capacity;
}

static char *completion_result_json(const ivr_media_command_result_t *result,
                                    size_t *out_size) {
    json_value_t *root = turbo_json_create_object();
    char *json = NULL;
    if (!root ||
        !add_member(root, "status",
                    turbo_json_create_string(result->status_code == IVR_OK
                                                 ? "completed"
                                                 : "failed")) ||
        !add_member(root, "mediaWorkerId",
                    turbo_json_create_string(result->worker_id)) ||
        !add_member(root, "dialogId",
                    turbo_json_create_string(result->dialog_id)) ||
        !add_member(root, "roomId", turbo_json_create_string(result->room_id)) ||
        !add_member(root, "callId", turbo_json_create_string(result->call_id)) ||
        !add_member(root, "callGeneration",
                    turbo_json_create_uint64(result->call_generation)) ||
        !add_member(root, "operationGeneration",
                    turbo_json_create_uint64(result->operation_generation)) ||
        (result->error_code[0] &&
         !add_member(root, "errorCode",
                     turbo_json_create_string(result->error_code))) ||
        (result->error_message[0] &&
         !add_member(root, "errorMessage",
                     turbo_json_create_string(result->error_message)))) {
        turbo_free_json(&root);
        return NULL;
    }
    json = turbo_json_serialize(root, out_size);
    turbo_free_json(&root);
    return json;
}

ivr_status_t iris_flowmq_provider_encode_receipt(
    const iris_flowmq_provider_command_t *command,
    const iris_media_bridge_result_t *result, const char *producer_id,
    const char *created_at, uint8_t **out, size_t *out_size) {
    flowmq_media_provider_limits_t limits = FLOWMQ_MEDIA_PROVIDER_LIMITS_INIT;
    ProviderReceiptV1_t receipt;
    DataBindError error = DATA_BIND_ERROR_INIT;
    char message_id[320];
    int written;
    if (!command || !result || !producer_id || !producer_id[0] ||
        !created_at || !created_at[0] || !out || !out_size) {
        return IVR_EINVAL;
    }
    *out = NULL;
    *out_size = 0u;
    written = snprintf(message_id, sizeof(message_id), "%s.receipt",
                       command->wire.command_id);
    if (written <= 0 || (size_t)written >= sizeof(message_id)) return IVR_ENOSPC;
    ProviderReceiptV1_init(&receipt);
    receipt.schema_version = 1u;
    receipt.message_kind = ProviderMessageKind_Receipt;
    receipt.disposition = receipt_disposition(result->status);
    receipt.status_code = (int32_t)result->status;
    receipt.retry_after_ms = 0u;
    if (!assign(&receipt.message_id, message_id) ||
        !assign(&receipt.correlation_id, command->wire.command_id) ||
        !assign(&receipt.causation_id, command->wire.message_id) ||
        !assign(&receipt.tenant_id, command->wire.tenant_id) ||
        !assign(&receipt.provider_id, command->wire.provider_id) ||
        !assign(&receipt.session_id, command->wire.session_id) ||
        !assign(&receipt.partition_key, command->wire.session_id) ||
        !assign(&receipt.producer_id, producer_id) ||
        !assign(&receipt.created_at, created_at) ||
        !assign(&receipt.deadline_at, command->wire.deadline_at) ||
        !assign(&receipt.command_id, command->wire.command_id) ||
        !assign(&receipt.worker_id, command->wire.worker_id) ||
        !assign(&receipt.dispatch_epoch, command->wire.dispatch_epoch) ||
        !assign(&receipt.error_code, result->error_code) ||
        !assign(&receipt.error_message, result->error_message) ||
        flowmq_media_provider_validate_receipt(&receipt, &limits) != TURBO_OK ||
        ProviderReceiptV1_to_bin(&receipt, out, out_size, &error) != DATA_BIND_OK) {
        ProviderReceiptV1_clear(&receipt);
        tbe_typed_serialized_free(*out);
        *out = NULL;
        *out_size = 0u;
        return IVR_ENOSPC;
    }
    ProviderReceiptV1_clear(&receipt);
    return IVR_OK;
}

ivr_status_t iris_flowmq_provider_encode_completion(
    const iris_media_completion_t *completion,
    const ivr_media_command_result_t *result, const char *provider_id,
    const char *producer_id, const char *message_id, const char *completed_at,
    uint64_t completed_at_unix_ms, uint8_t **out, size_t *out_size) {
    flowmq_media_provider_limits_t limits = FLOWMQ_MEDIA_PROVIDER_LIMITS_INIT;
    ProviderCompletionV1_t wire;
    DataBindError error = DATA_BIND_ERROR_INIT;
    char epoch[32];
    char completed_ms[32];
    char *result_json = NULL;
    size_t result_json_size = 0u;
    if (!completion || !result || !provider_id || !provider_id[0] ||
        !producer_id || !producer_id[0] || !message_id || !message_id[0] ||
        !completed_at || !completed_at[0] || completed_at_unix_ms == 0u ||
        !out || !out_size || completion->command_id[0] == '\0' ||
        completion->tenant_id[0] == '\0' ||
        completion->provider_session_id[0] == '\0' ||
        completion->iris_worker_id[0] == '\0' ||
        completion->dispatch_epoch == 0u ||
        !format_u64(completion->dispatch_epoch, epoch, sizeof(epoch)) ||
        !format_u64(completed_at_unix_ms, completed_ms,
                    sizeof(completed_ms))) {
        return IVR_EINVAL;
    }
    *out = NULL;
    *out_size = 0u;
    result_json = completion_result_json(result, &result_json_size);
    if (!result_json || result_json_size == 0u) return IVR_ENOSPC;
    ProviderCompletionV1_init(&wire);
    wire.schema_version = 1u;
    wire.message_kind = ProviderMessageKind_Completion;
    wire.terminal_status = result->status_code == IVR_OK
                               ? ProviderTerminalStatus_Succeeded
                               : ProviderTerminalStatus_Failed;
    if (!assign(&wire.message_id, message_id) ||
        !assign(&wire.correlation_id, completion->correlation_id) ||
        !assign(&wire.causation_id, completion->command_id) ||
        !assign(&wire.tenant_id, completion->tenant_id) ||
        !assign(&wire.provider_id, provider_id) ||
        !assign(&wire.session_id, completion->provider_session_id) ||
        !assign(&wire.partition_key, completion->provider_session_id) ||
        !assign(&wire.producer_id, producer_id) ||
        !assign(&wire.created_at, completed_at) ||
        !assign(&wire.deadline_at, "") ||
        !assign(&wire.command_id, completion->command_id) ||
        !assign(&wire.worker_id, completion->iris_worker_id) ||
        !assign(&wire.dispatch_epoch, epoch) ||
        !assign(&wire.event_id, message_id) ||
        !assign(&wire.event_type, result->status_code == IVR_OK
                                      ? "provider.media.completed"
                                      : "provider.media.failed") ||
        !assign(&wire.completed_at, completed_at) ||
        !assign(&wire.completed_at_unix_ms, completed_ms) ||
        !assign(&wire.result_json, result_json) ||
        !assign(&wire.error_code, result->error_code) ||
        !assign(&wire.error_message, result->error_message) ||
        flowmq_media_provider_validate_completion(&wire, &limits) != TURBO_OK ||
        ProviderCompletionV1_to_bin(&wire, out, out_size, &error) !=
            DATA_BIND_OK) {
        ProviderCompletionV1_clear(&wire);
        turbo_json_serialize_free(result_json);
        tbe_typed_serialized_free(*out);
        *out = NULL;
        *out_size = 0u;
        return IVR_ENOSPC;
    }
    ProviderCompletionV1_clear(&wire);
    turbo_json_serialize_free(result_json);
    return IVR_OK;
}

ivr_status_t iris_flowmq_provider_decode_completion_ack(
    DataBind *codec, const void *encoded, size_t encoded_size,
    const iris_media_completion_t *completion, const char *provider_id,
    const char *iris_identity, const char *completion_message_id,
    iris_flowmq_completion_ack_t *out) {
    flowmq_media_provider_limits_t limits = FLOWMQ_MEDIA_PROVIDER_LIMITS_INIT;
    ProviderCompletionAckV1_t ack;
    ProviderMessageKind_t kind = ProviderMessageKind_Command;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint64_t epoch = 0u;
    uint64_t sequence = 0u;
    if (!codec || !encoded || encoded_size == 0u || !completion ||
        !provider_id || !iris_identity || !completion_message_id || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    ProviderCompletionAckV1_init(&ack);
    if (flowmq_media_provider_peek_kind(encoded, encoded_size, &kind) !=
            TURBO_OK ||
        kind != ProviderMessageKind_CompletionAck ||
        ProviderCompletionAckV1_from_bin(codec, &ack, encoded, encoded_size,
                                         &error) != DATA_BIND_OK ||
        flowmq_media_provider_validate_completion_ack(&ack, &limits) !=
            TURBO_OK ||
        flowmq_media_provider_parse_u64(ack.dispatch_epoch, &epoch) !=
            TURBO_OK ||
        flowmq_media_provider_parse_u64(ack.committed_sequence, &sequence) !=
            TURBO_OK ||
        strcmp(ack.causation_id, completion_message_id) != 0 ||
        strcmp(ack.correlation_id, completion->correlation_id) != 0 ||
        strcmp(ack.tenant_id, completion->tenant_id) != 0 ||
        strcmp(ack.provider_id, provider_id) != 0 ||
        strcmp(ack.session_id, completion->provider_session_id) != 0 ||
        strcmp(ack.partition_key, completion->provider_session_id) != 0 ||
        strcmp(ack.producer_id, iris_identity) != 0 ||
        strcmp(ack.command_id, completion->command_id) != 0 ||
        epoch != completion->dispatch_epoch) {
        ProviderCompletionAckV1_clear(&ack);
        return IVR_ESTATE;
    }
    out->disposition = ack.disposition;
    out->committed_sequence = sequence;
    ProviderCompletionAckV1_clear(&ack);
    return IVR_OK;
}

ivr_status_t iris_flowmq_provider_encode_event(
    const ivr_media_event_t *event, const char *provider_id,
    const char *producer_id, const char *message_id, const char *created_at,
    const char *occurred_at, uint8_t **out, size_t *out_size) {
    flowmq_media_provider_limits_t limits = FLOWMQ_MEDIA_PROVIDER_LIMITS_INIT;
    ProviderEventV1_t wire;
    DataBindError error = DATA_BIND_ERROR_INIT;
    char sequence[32];
    if (!event || !provider_id || !provider_id[0] || !producer_id ||
        !producer_id[0] || !message_id || !message_id[0] || !created_at ||
        !created_at[0] || !occurred_at || !occurred_at[0] || !out ||
        !out_size || !event->event_id[0] || !event->tenant_id[0] ||
        !event->provider_session_id[0] || !event->dialog_id[0] ||
        !event->event_type[0] || event->sequence == 0u ||
        !format_u64(event->sequence, sequence, sizeof(sequence))) {
        return IVR_EINVAL;
    }
    *out = NULL;
    *out_size = 0u;
    ProviderEventV1_init(&wire);
    wire.schema_version = 1u;
    wire.message_kind = ProviderMessageKind_Event;
    if (!assign(&wire.message_id, message_id) ||
        !assign(&wire.correlation_id, event->dialog_id) ||
        !assign(&wire.causation_id, "") ||
        !assign(&wire.tenant_id, event->tenant_id) ||
        !assign(&wire.provider_id, provider_id) ||
        !assign(&wire.session_id, event->provider_session_id) ||
        !assign(&wire.partition_key, event->provider_session_id) ||
        !assign(&wire.producer_id, producer_id) ||
        !assign(&wire.created_at, created_at) ||
        !assign(&wire.deadline_at, "") ||
        !assign(&wire.event_id, event->event_id) ||
        !assign(&wire.event_type, event->event_type) ||
        !assign(&wire.aggregate_id, event->dialog_id) ||
        !assign(&wire.sequence, sequence) ||
        !assign(&wire.occurred_at, occurred_at) ||
        !assign(&wire.payload_json,
                event->payload_json[0] ? event->payload_json : "{}") ||
        flowmq_media_provider_validate_event(&wire, &limits) != TURBO_OK ||
        ProviderEventV1_to_bin(&wire, out, out_size, &error) != DATA_BIND_OK) {
        ProviderEventV1_clear(&wire);
        tbe_typed_serialized_free(*out);
        *out = NULL;
        *out_size = 0u;
        return IVR_ENOSPC;
    }
    ProviderEventV1_clear(&wire);
    return IVR_OK;
}

ivr_status_t iris_flowmq_provider_decode_event_ack(
    DataBind *codec, const void *encoded, size_t encoded_size,
    const ivr_media_event_t *event, const char *provider_id,
    const char *iris_identity, const char *event_message_id,
    iris_flowmq_event_ack_t *out) {
    flowmq_media_provider_limits_t limits = FLOWMQ_MEDIA_PROVIDER_LIMITS_INIT;
    ProviderEventAckV1_t ack;
    ProviderMessageKind_t kind = ProviderMessageKind_Command;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint64_t sequence = 0u;
    if (!codec || !encoded || encoded_size == 0u || !event || !provider_id ||
        !iris_identity || !event_message_id || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    ProviderEventAckV1_init(&ack);
    if (flowmq_media_provider_peek_kind(encoded, encoded_size, &kind) !=
            TURBO_OK ||
        kind != ProviderMessageKind_EventAck ||
        ProviderEventAckV1_from_bin(codec, &ack, encoded, encoded_size,
                                    &error) != DATA_BIND_OK ||
        flowmq_media_provider_validate_event_ack(&ack, &limits) != TURBO_OK ||
        flowmq_media_provider_parse_u64(ack.committed_sequence, &sequence) !=
            TURBO_OK ||
        strcmp(ack.causation_id, event_message_id) != 0 ||
        strcmp(ack.correlation_id, event->dialog_id) != 0 ||
        strcmp(ack.tenant_id, event->tenant_id) != 0 ||
        strcmp(ack.provider_id, provider_id) != 0 ||
        strcmp(ack.session_id, event->provider_session_id) != 0 ||
        strcmp(ack.partition_key, event->provider_session_id) != 0 ||
        strcmp(ack.producer_id, iris_identity) != 0 ||
        strcmp(ack.event_id, event->event_id) != 0 ||
        (((ack.disposition == ProviderEventAckDisposition_Committed ||
           ack.disposition == ProviderEventAckDisposition_DuplicateEvent) &&
          sequence == 0u) ||
         ((ack.disposition == ProviderEventAckDisposition_EventConflict ||
           ack.disposition == ProviderEventAckDisposition_EventRejected) &&
          sequence != 0u))) {
        ProviderEventAckV1_clear(&ack);
        return IVR_ESTATE;
    }
    out->disposition = ack.disposition;
    out->committed_sequence = sequence;
    ProviderEventAckV1_clear(&ack);
    return IVR_OK;
}

void iris_flowmq_provider_observation_init(
    iris_flowmq_provider_observation_t *observation) {
    if (!observation) return;
    memset(observation, 0, sizeof(*observation));
    ProviderObservationV1_init(&observation->wire);
}

void iris_flowmq_provider_observation_clear(
    iris_flowmq_provider_observation_t *observation) {
    if (!observation) return;
    ProviderObservationV1_clear(&observation->wire);
    memset(observation, 0, sizeof(*observation));
}

ivr_status_t iris_flowmq_provider_encode_query(
    const char *tenant_id, const char *provider_id, const char *producer_id,
    const char *query_id, const char *query_type, const char *created_at,
    const char *deadline_at, uint64_t expected_revision, uint64_t cursor,
    uint32_t limit, const char *payload_json, uint8_t **out,
    size_t *out_size) {
    flowmq_media_provider_limits_t limits = FLOWMQ_MEDIA_PROVIDER_LIMITS_INIT;
    ProviderQueryV1_t query;
    DataBindError error = DATA_BIND_ERROR_INIT;
    char revision[32];
    char cursor_text[32];
    if (!tenant_id || !tenant_id[0] || !provider_id || !provider_id[0] ||
        !producer_id || !producer_id[0] || !query_id || !query_id[0] ||
        !query_type || !query_type[0] || !created_at || !created_at[0] ||
        !deadline_at || !deadline_at[0] || limit == 0u || !payload_json ||
        !payload_json[0] || !out || !out_size ||
        !format_u64(expected_revision, revision, sizeof(revision)) ||
        !format_u64(cursor, cursor_text, sizeof(cursor_text))) {
        return IVR_EINVAL;
    }
    *out = NULL;
    *out_size = 0u;
    ProviderQueryV1_init(&query);
    query.schema_version = 1u;
    query.message_kind = ProviderMessageKind_Query;
    query.limit = limit;
    if (!assign(&query.message_id, query_id) ||
        !assign(&query.correlation_id, "") ||
        !assign(&query.causation_id, "") ||
        !assign(&query.tenant_id, tenant_id) ||
        !assign(&query.provider_id, provider_id) ||
        !assign(&query.session_id, "") ||
        !assign(&query.partition_key, tenant_id) ||
        !assign(&query.producer_id, producer_id) ||
        !assign(&query.created_at, created_at) ||
        !assign(&query.deadline_at, deadline_at) ||
        !assign(&query.query_id, query_id) ||
        !assign(&query.query_type, query_type) ||
        !assign(&query.expected_revision, revision) ||
        !assign(&query.cursor, cursor_text) ||
        !assign(&query.payload_json, payload_json) ||
        flowmq_media_provider_validate_query(&query, &limits) != TURBO_OK ||
        ProviderQueryV1_to_bin(&query, out, out_size, &error) != DATA_BIND_OK) {
        ProviderQueryV1_clear(&query);
        tbe_typed_serialized_free(*out);
        *out = NULL;
        *out_size = 0u;
        return IVR_ENOSPC;
    }
    ProviderQueryV1_clear(&query);
    return IVR_OK;
}

ivr_status_t iris_flowmq_provider_decode_observation(
    DataBind *codec, const void *encoded, size_t encoded_size,
    const char *tenant_id, const char *provider_id, const char *iris_identity,
    const char *query_id, const char *query_type, uint64_t expected_cursor,
    iris_flowmq_provider_observation_t *out) {
    flowmq_media_provider_limits_t limits = FLOWMQ_MEDIA_PROVIDER_LIMITS_INIT;
    ProviderMessageKind_t kind = ProviderMessageKind_Command;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint64_t revision = 0u;
    uint64_t cursor = 0u;
    uint64_t next_cursor = 0u;
    if (!codec || !encoded || encoded_size == 0u || !tenant_id ||
        !provider_id || !iris_identity || !query_id || !query_type || !out) {
        return IVR_EINVAL;
    }
    iris_flowmq_provider_observation_init(out);
    if (flowmq_media_provider_peek_kind(encoded, encoded_size, &kind) !=
            TURBO_OK ||
        kind != ProviderMessageKind_Observation ||
        ProviderObservationV1_from_bin(codec, &out->wire, encoded,
                                       encoded_size, &error) != DATA_BIND_OK ||
        flowmq_media_provider_validate_observation(&out->wire, &limits) !=
            TURBO_OK ||
        flowmq_media_provider_parse_u64(out->wire.revision, &revision) !=
            TURBO_OK ||
        flowmq_media_provider_parse_u64(out->wire.cursor, &cursor) != TURBO_OK ||
        flowmq_media_provider_parse_u64(out->wire.next_cursor, &next_cursor) !=
            TURBO_OK ||
        strcmp(out->wire.correlation_id, query_id) != 0 ||
        strcmp(out->wire.causation_id, query_id) != 0 ||
        strcmp(out->wire.tenant_id, tenant_id) != 0 ||
        strcmp(out->wire.provider_id, provider_id) != 0 ||
        out->wire.session_id[0] != '\0' ||
        strcmp(out->wire.partition_key, tenant_id) != 0 ||
        strcmp(out->wire.producer_id, iris_identity) != 0 ||
        strcmp(out->wire.query_id, query_id) != 0 ||
        strcmp(out->wire.observation_type, query_type) != 0 ||
        cursor != expected_cursor ||
        (out->wire.has_more && next_cursor <= cursor) ||
        (!out->wire.has_more && next_cursor != 0u) ||
        (out->wire.status == ProviderQueryStatus_QueryOk &&
         (out->wire.error_code[0] || out->wire.error_message[0])) ||
        (out->wire.status != ProviderQueryStatus_QueryOk &&
         !out->wire.error_code[0])) {
        iris_flowmq_provider_observation_clear(out);
        return IVR_ESTATE;
    }
    out->revision = revision;
    out->cursor = cursor;
    out->next_cursor = next_cursor;
    return IVR_OK;
}

static int bounded_nonempty(const char *value, size_t capacity) {
    return value && capacity > 1u && value[0] != '\0' &&
           memchr(value, '\0', capacity) != NULL;
}

static int copy_bounded(char *out, size_t capacity, const char *value) {
    size_t length;
    if (!out || capacity == 0u || !value) return 0;
    length = strlen(value);
    if (length >= capacity) return 0;
    memcpy(out, value, length + 1u);
    return 1;
}

ivr_status_t iris_flowmq_provider_encode_call_offer(
    const iris_flowmq_call_offer_t *offer, const char *provider_id,
    const char *producer_id, uint8_t **out, size_t *out_size) {
    flowmq_media_provider_limits_t limits = FLOWMQ_MEDIA_PROVIDER_LIMITS_INIT;
    ProviderCallOfferV1_t wire;
    DataBindError error = DATA_BIND_ERROR_INIT;
    char generation[32];
    if (!offer || !provider_id || !provider_id[0] || !producer_id ||
        !producer_id[0] || !out || !out_size ||
        !bounded_nonempty(offer->ingress_event_id,
                          sizeof(offer->ingress_event_id)) ||
        !bounded_nonempty(offer->tenant_id, sizeof(offer->tenant_id)) ||
        !bounded_nonempty(offer->call_id, sizeof(offer->call_id)) ||
        !bounded_nonempty(offer->created_at, sizeof(offer->created_at)) ||
        !bounded_nonempty(offer->deadline_at, sizeof(offer->deadline_at)) ||
        !bounded_nonempty(offer->transport, sizeof(offer->transport)) ||
        !bounded_nonempty(offer->source, sizeof(offer->source)) ||
        !bounded_nonempty(offer->destination, sizeof(offer->destination)) ||
        !bounded_nonempty(offer->payload_json, sizeof(offer->payload_json)) ||
        offer->call_generation == 0u ||
        !format_u64(offer->call_generation, generation, sizeof(generation))) {
        return IVR_EINVAL;
    }
    *out = NULL;
    *out_size = 0u;
    ProviderCallOfferV1_init(&wire);
    wire.schema_version = 1u;
    wire.message_kind = ProviderMessageKind_CallOffer;
    if (!assign(&wire.message_id, offer->ingress_event_id) ||
        !assign(&wire.correlation_id, offer->call_id) ||
        !assign(&wire.causation_id, "") ||
        !assign(&wire.tenant_id, offer->tenant_id) ||
        !assign(&wire.provider_id, provider_id) ||
        !assign(&wire.session_id, "") ||
        !assign(&wire.partition_key, offer->call_id) ||
        !assign(&wire.producer_id, producer_id) ||
        !assign(&wire.created_at, offer->created_at) ||
        !assign(&wire.deadline_at, offer->deadline_at) ||
        !assign(&wire.ingress_event_id, offer->ingress_event_id) ||
        !assign(&wire.call_id, offer->call_id) ||
        !assign(&wire.call_generation, generation) ||
        !assign(&wire.transport, offer->transport) ||
        !assign(&wire.source, offer->source) ||
        !assign(&wire.destination, offer->destination) ||
        !assign(&wire.payload_json, offer->payload_json) ||
        flowmq_media_provider_validate_call_offer(&wire, &limits) != TURBO_OK ||
        ProviderCallOfferV1_to_bin(&wire, out, out_size, &error) !=
            DATA_BIND_OK) {
        ProviderCallOfferV1_clear(&wire);
        tbe_typed_serialized_free(*out);
        *out = NULL;
        *out_size = 0u;
        return IVR_ENOSPC;
    }
    ProviderCallOfferV1_clear(&wire);
    return IVR_OK;
}

ivr_status_t iris_flowmq_provider_decode_session_bound(
    DataBind *codec, const void *encoded, size_t encoded_size,
    const iris_flowmq_call_offer_t *offer, const char *provider_id,
    const char *iris_identity, iris_flowmq_session_bound_t *out) {
    flowmq_media_provider_limits_t limits = FLOWMQ_MEDIA_PROVIDER_LIMITS_INIT;
    ProviderSessionBoundV1_t wire;
    ProviderMessageKind_t kind = ProviderMessageKind_Command;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint64_t generation = 0u;
    int accepted;
    if (!codec || !encoded || encoded_size == 0u || !offer || !provider_id ||
        !iris_identity || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    ProviderSessionBoundV1_init(&wire);
    if (flowmq_media_provider_peek_kind(encoded, encoded_size, &kind) !=
            TURBO_OK ||
        kind != ProviderMessageKind_SessionBound ||
        ProviderSessionBoundV1_from_bin(codec, &wire, encoded, encoded_size,
                                        &error) != DATA_BIND_OK ||
        flowmq_media_provider_validate_session_bound(&wire, &limits) !=
            TURBO_OK ||
        flowmq_media_provider_parse_u64(wire.call_generation, &generation) !=
            TURBO_OK ||
        strcmp(wire.correlation_id, offer->call_id) != 0 ||
        strcmp(wire.causation_id, offer->ingress_event_id) != 0 ||
        strcmp(wire.tenant_id, offer->tenant_id) != 0 ||
        strcmp(wire.provider_id, provider_id) != 0 ||
        strcmp(wire.partition_key, offer->call_id) != 0 ||
        strcmp(wire.producer_id, iris_identity) != 0 ||
        strcmp(wire.ingress_event_id, offer->ingress_event_id) != 0 ||
        strcmp(wire.call_id, offer->call_id) != 0 ||
        generation != offer->call_generation) {
        ProviderSessionBoundV1_clear(&wire);
        return IVR_ESTATE;
    }
    accepted = wire.accepted != 0;
    if ((accepted &&
         (!wire.session_id[0] ||
          strcmp(wire.session_id, wire.bound_session_id) != 0 ||
          wire.error_code[0] || wire.error_message[0])) ||
        (!accepted &&
         (wire.session_id[0] || wire.bound_session_id[0] ||
          !wire.error_code[0])) ||
        !copy_bounded(out->message_id, sizeof(out->message_id),
                      wire.message_id) ||
        !copy_bounded(out->bound_session_id, sizeof(out->bound_session_id),
                      wire.bound_session_id) ||
        !copy_bounded(out->error_code, sizeof(out->error_code),
                      wire.error_code) ||
        !copy_bounded(out->error_message, sizeof(out->error_message),
                      wire.error_message)) {
        ProviderSessionBoundV1_clear(&wire);
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    out->accepted = accepted;
    ProviderSessionBoundV1_clear(&wire);
    return IVR_OK;
}
