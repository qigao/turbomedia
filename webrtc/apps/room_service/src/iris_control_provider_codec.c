#include "iris_control_provider_codec.h"

#include "iris_provider_protocol.h"
#include <json_parser.h>
#include <salts_error.h>
#include <turbo_crypto.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IRIS_CONTROL_MEDIA_BRIDGE_SCHEMA_VERSION UINT64_C(2)
#define IRIS_CONTROL_ROOM_BRIDGE_SCHEMA_VERSION UINT64_C(3)

#define SET_TEXT(record, builder, member, value) \
    record##_##member##_set(&(builder), (value), strlen(value))

static int add_member(json_value_t *object, const char *name,
                      json_value_t *value) {
    if (!value || !json_object_add_checked(object, name, value)) {
        json_free(value);
        return 0;
    }
    return 1;
}

static json_value_t *wire_string(tbe_var_data_t value) {
    return json_create_string_n((const char *)value.data, value.size);
}

static int wire_text_equal(tbe_var_data_t value, const char *expected) {
    size_t expected_size;
    if (!expected) return 0;
    expected_size = strlen(expected);
    return value.size == expected_size &&
           (value.size == 0u || memcmp(value.data, expected, value.size) == 0);
}

static int wire_text_same(tbe_var_data_t left, tbe_var_data_t right) {
    return left.size == right.size &&
           (left.size == 0u || memcmp(left.data, right.data, left.size) == 0);
}

static int wire_text_empty(tbe_var_data_t value) { return value.size == 0u; }

static int copy_wire_text(char *out, size_t capacity, tbe_var_data_t value) {
    if (!out || capacity == 0u || value.size >= capacity) return 0;
    if (value.size != 0u) memcpy(out, value.data, value.size);
    out[value.size] = '\0';
    return 1;
}

static int duplicate_wire_text(char **out, tbe_var_data_t value) {
    char *copy;
    if (!out || value.size == SIZE_MAX) return 0;
    copy = (char *)malloc(value.size + 1u);
    if (!copy) return 0;
    if (value.size != 0u) memcpy(copy, value.data, value.size);
    copy[value.size] = '\0';
    *out = copy;
    return 1;
}

static int parse_wire_u64(tbe_var_data_t value, uint64_t *out) {
    char text[IRIS_CONTROL_U64_CAPACITY];
    return copy_wire_text(text, sizeof(text), value) &&
           iris_provider_parse_u64(text, out) == SALTS_OK;
}

static int allocate_wire(size_t block_length, const char *const *fields,
                         size_t field_count, uint8_t **out,
                         size_t *out_size) {
    size_t size = block_length;
    size_t index;
    uint8_t *buffer;
    if (!fields || !out || !out_size) return 0;
    for (index = 0u; index < field_count; ++index) {
        size_t length;
        if (!fields[index]) return 0;
        length = strlen(fields[index]);
        if (size > SIZE_MAX - sizeof(uint32_t) ||
            length > SIZE_MAX - size - sizeof(uint32_t)) {
            return 0;
        }
        size += sizeof(uint32_t) + length;
    }
    buffer = (uint8_t *)malloc(size);
    if (!buffer) return 0;
    memset(buffer, 0, size);
    *out = buffer;
    *out_size = size;
    return 1;
}

static int hash_field(turbo_crypto_sha256_ctx_t *context,
                      tbe_var_data_t value) {
    uint64_t length = (uint64_t)value.size;
    uint8_t encoded_length[sizeof(length)];
    size_t index;
    for (index = 0u; index < sizeof(encoded_length); ++index) {
        encoded_length[index] = (uint8_t)(length >> (index * 8u));
    }
    return turbo_crypto_sha256_update(context, encoded_length,
                                      sizeof(encoded_length)) == 0 &&
           (value.size == 0u ||
            turbo_crypto_sha256_update(context, value.data, value.size) == 0);
}

static int command_fingerprint_valid(const ProviderCommandV1_view_t *command) {
    static const char hex[] = "0123456789abcdef";
    turbo_crypto_sha256_ctx_t context;
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    char expected[sizeof("sha256:") - 1u + TURBO_CRYPTO_SHA256_SIZE * 2u + 1u];
    tbe_var_data_t tenant_id, session_id, command_type, provider_id;
    tbe_var_data_t correlation_id, causation_id, deadline_at, payload_json;
    tbe_var_data_t fingerprint;
    size_t index;
    if (!command ||
        !ProviderCommandV1_tenant_id(command, &tenant_id) ||
        !ProviderCommandV1_session_id(command, &session_id) ||
        !ProviderCommandV1_command_type(command, &command_type) ||
        !ProviderCommandV1_provider_id(command, &provider_id) ||
        !ProviderCommandV1_correlation_id(command, &correlation_id) ||
        !ProviderCommandV1_causation_id(command, &causation_id) ||
        !ProviderCommandV1_deadline_at(command, &deadline_at) ||
        !ProviderCommandV1_payload_json(command, &payload_json) ||
        !ProviderCommandV1_semantic_fingerprint(command, &fingerprint) ||
        turbo_crypto_sha256_init(&context) != 0 ||
        !hash_field(&context, tenant_id) || !hash_field(&context, session_id) ||
        !hash_field(&context, command_type) || !hash_field(&context, provider_id) ||
        !hash_field(&context, correlation_id) || !hash_field(&context, causation_id) ||
        !hash_field(&context, deadline_at) || !hash_field(&context, payload_json) ||
        turbo_crypto_sha256_final(&context, digest) != 0) {
        return 0;
    }
    memcpy(expected, "sha256:", sizeof("sha256:") - 1u);
    for (index = 0u; index < sizeof(digest); ++index) {
        expected[sizeof("sha256:") - 1u + index * 2u] = hex[digest[index] >> 4u];
        expected[sizeof("sha256:") + index * 2u] = hex[digest[index] & 0x0fu];
    }
    expected[sizeof(expected) - 1u] = '\0';
    return fingerprint.size == sizeof(expected) - 1u &&
           turbo_crypto_verify(expected, fingerprint.data,
                               sizeof(expected) - 1u) == 0;
}

static char *build_bridge_json(const ProviderCommandV1_view_t *command,
                               uint64_t dispatch_epoch, size_t *out_size) {
    json_value_t *root = NULL;
    json_value_t *payload = NULL;
    json_value_t *capability_value;
    const char *capability;
    uint64_t bridge_schema_version;
    char *json = NULL;
    tbe_var_data_t command_id, tenant_id, session_id, command_type, provider_id;
    tbe_var_data_t correlation_id, causation_id, deadline_at, worker_id;
    tbe_var_data_t payload_json;
    if (!ProviderCommandV1_command_id(command, &command_id) ||
        !ProviderCommandV1_tenant_id(command, &tenant_id) ||
        !ProviderCommandV1_session_id(command, &session_id) ||
        !ProviderCommandV1_command_type(command, &command_type) ||
        !ProviderCommandV1_provider_id(command, &provider_id) ||
        !ProviderCommandV1_correlation_id(command, &correlation_id) ||
        !ProviderCommandV1_causation_id(command, &causation_id) ||
        !ProviderCommandV1_deadline_at(command, &deadline_at) ||
        !ProviderCommandV1_worker_id(command, &worker_id) ||
        !ProviderCommandV1_payload_json(command, &payload_json) ||
        !(payload = json_parse((const char *)payload_json.data,
                               payload_json.size)) ||
        json_type(payload) != JSON_OBJECT) {
        json_free(payload);
        return NULL;
    }
    capability_value = json_object_get(payload, "capability");
    capability = capability_value && json_type(capability_value) == JSON_STRING
                     ? json_string(capability_value)
                     : NULL;
    bridge_schema_version =
        capability && strcmp(capability, "room") == 0
            ? IRIS_CONTROL_ROOM_BRIDGE_SCHEMA_VERSION
            : IRIS_CONTROL_MEDIA_BRIDGE_SCHEMA_VERSION;
    root = json_create_object();
    if (!root ||
        !add_member(root, "schemaVersion", json_create_uint64(bridge_schema_version)) ||
        !add_member(root, "commandId", wire_string(command_id)) ||
        !add_member(root, "tenantId", wire_string(tenant_id)) ||
        !add_member(root, "sessionId", wire_string(session_id)) ||
        !add_member(root, "type", wire_string(command_type)) ||
        !add_member(root, "provider", wire_string(provider_id)) ||
        !add_member(root, "correlationId", wire_string(correlation_id)) ||
        !add_member(root, "causationId", wire_string(causation_id)) ||
        !add_member(root, "deadline", wire_string(deadline_at)) ||
        !add_member(root, "workerId", wire_string(worker_id)) ||
        !add_member(root, "dispatchEpoch", json_create_uint64(dispatch_epoch)) ||
        !add_member(root, "data", payload)) {
        json_free(payload);
        json_free(root);
        return NULL;
    }
    payload = NULL;
    json = json_serialize(root, out_size);
    json_free(root);
    return json;
}

void iris_control_provider_command_init(iris_control_provider_command_t *command) {
    if (command) memset(command, 0, sizeof(*command));
}

void iris_control_provider_command_clear(iris_control_provider_command_t *command) {
    if (!command) return;
    json_serialize_free(command->bridge_json);
    memset(command, 0, sizeof(*command));
}

ivr_status_t iris_control_provider_decode_command(
    const void *encoded, size_t encoded_size,
    iris_control_provider_command_t *out) {
    iris_provider_limits_t limits = IRIS_PROVIDER_LIMITS_INIT;
    ProviderCommandV1_view_t view;
    ProviderMessageKind_t kind = ProviderMessageKind_Receipt;
    tbe_var_data_t message_id, command_id, tenant_id, provider_id, session_id;
    tbe_var_data_t partition_key, producer_id, deadline_at, worker_id;
    tbe_var_data_t dispatch_epoch_text, fingerprint;
    uint64_t dispatch_epoch = 0u;
    if (!encoded || encoded_size == 0u || !out) return IVR_EINVAL;
    iris_control_provider_command_init(out);
    if (iris_provider_peek_kind(encoded, encoded_size, &kind) != SALTS_OK ||
        kind != ProviderMessageKind_Command ||
        !ProviderCommandV1_view_bind(&view, encoded, encoded_size) ||
        iris_provider_validate_command(&view, &limits) != SALTS_OK ||
        !ProviderCommandV1_message_id(&view, &message_id) ||
        !ProviderCommandV1_command_id(&view, &command_id) ||
        !ProviderCommandV1_tenant_id(&view, &tenant_id) ||
        !ProviderCommandV1_provider_id(&view, &provider_id) ||
        !ProviderCommandV1_session_id(&view, &session_id) ||
        !ProviderCommandV1_partition_key(&view, &partition_key) ||
        !ProviderCommandV1_producer_id(&view, &producer_id) ||
        !ProviderCommandV1_deadline_at(&view, &deadline_at) ||
        !ProviderCommandV1_worker_id(&view, &worker_id) ||
        !ProviderCommandV1_dispatch_epoch(&view, &dispatch_epoch_text) ||
        !ProviderCommandV1_semantic_fingerprint(&view, &fingerprint) ||
        !wire_text_same(message_id, command_id) ||
        !wire_text_same(partition_key, session_id) ||
        !parse_wire_u64(dispatch_epoch_text, &dispatch_epoch) ||
        dispatch_epoch == 0u || !command_fingerprint_valid(&view) ||
        !copy_wire_text(out->wire.message_id, sizeof(out->wire.message_id), message_id) ||
        !copy_wire_text(out->wire.command_id, sizeof(out->wire.command_id), command_id) ||
        !copy_wire_text(out->wire.tenant_id, sizeof(out->wire.tenant_id), tenant_id) ||
        !copy_wire_text(out->wire.provider_id, sizeof(out->wire.provider_id), provider_id) ||
        !copy_wire_text(out->wire.session_id, sizeof(out->wire.session_id), session_id) ||
        !copy_wire_text(out->wire.producer_id, sizeof(out->wire.producer_id), producer_id) ||
        !copy_wire_text(out->wire.deadline_at, sizeof(out->wire.deadline_at), deadline_at) ||
        !copy_wire_text(out->wire.worker_id, sizeof(out->wire.worker_id), worker_id) ||
        !copy_wire_text(out->wire.dispatch_epoch, sizeof(out->wire.dispatch_epoch),
                        dispatch_epoch_text) ||
        !copy_wire_text(out->wire.semantic_fingerprint,
                        sizeof(out->wire.semantic_fingerprint), fingerprint)) {
        iris_control_provider_command_clear(out);
        return IVR_ESTATE;
    }
    out->bridge_json = build_bridge_json(&view, dispatch_epoch, &out->bridge_json_size);
    if (!out->bridge_json) {
        iris_control_provider_command_clear(out);
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
        default:
            return ProviderReceiptDisposition_Rejected;
    }
}

static int format_u64(uint64_t value, char *out, size_t capacity) {
    int written = snprintf(out, capacity, "%llu", (unsigned long long)value);
    return written > 0 && (size_t)written < capacity;
}

static char *completion_result_json(const ivr_media_command_result_t *result,
                                    size_t *out_size) {
    json_value_t *root = json_create_object();
    char *json = NULL;
    if (!root ||
        !add_member(root, "status", json_create_string(
                                        result->status_code == IVR_OK ? "completed" : "failed")) ||
        !add_member(root, "mediaWorkerId", json_create_string(result->worker_id)) ||
        !add_member(root, "dialogId", json_create_string(result->dialog_id)) ||
        !add_member(root, "roomId", json_create_string(result->room_id)) ||
        !add_member(root, "callId", json_create_string(result->call_id)) ||
        !add_member(root, "callGeneration", json_create_uint64(result->call_generation)) ||
        !add_member(root, "operationGeneration",
                    json_create_uint64(result->operation_generation)) ||
        (result->error_code[0] &&
         !add_member(root, "errorCode", json_create_string(result->error_code))) ||
        (result->error_message[0] &&
         !add_member(root, "errorMessage", json_create_string(result->error_message)))) {
        json_free(root);
        return NULL;
    }
    json = json_serialize(root, out_size);
    json_free(root);
    return json;
}

ivr_status_t iris_control_provider_encode_receipt(
    const iris_control_provider_command_t *command,
    const iris_media_bridge_result_t *result, const char *producer_id,
    const char *created_at, uint8_t **out, size_t *out_size) {
    iris_provider_limits_t limits = IRIS_PROVIDER_LIMITS_INIT;
    ProviderReceiptV1_builder_t builder;
    ProviderReceiptV1_view_t view;
    char message_id[320];
    const char *fields[15];
    int written;
    if (!command || !result || !producer_id || !producer_id[0] ||
        !created_at || !created_at[0] || !out || !out_size) return IVR_EINVAL;
    *out = NULL;
    *out_size = 0u;
    written = snprintf(message_id, sizeof(message_id), "%s.receipt",
                       command->wire.command_id);
    if (written <= 0 || (size_t)written >= sizeof(message_id)) return IVR_ENOSPC;
    fields[0] = message_id;
    fields[1] = command->wire.command_id;
    fields[2] = command->wire.message_id;
    fields[3] = command->wire.tenant_id;
    fields[4] = command->wire.provider_id;
    fields[5] = command->wire.session_id;
    fields[6] = command->wire.session_id;
    fields[7] = producer_id;
    fields[8] = created_at;
    fields[9] = command->wire.deadline_at;
    fields[10] = command->wire.command_id;
    fields[11] = command->wire.worker_id;
    fields[12] = command->wire.dispatch_epoch;
    fields[13] = result->error_code ? result->error_code : "";
    fields[14] = result->error_message ? result->error_message : "";
    if (!allocate_wire(ProviderReceiptV1_BLOCK_LENGTH, fields, 15u, out, out_size) ||
        !ProviderReceiptV1_builder_bind(&builder, *out, *out_size) ||
        !ProviderReceiptV1_schema_version_set(&builder,
                                              IRIS_PROVIDER_SCHEMA_VERSION) ||
        !ProviderReceiptV1_message_kind_set(&builder, ProviderMessageKind_Receipt) ||
        !ProviderReceiptV1_disposition_set(&builder, receipt_disposition(result->status)) ||
        !ProviderReceiptV1_status_code_set(&builder, (int32_t)result->status) ||
        !ProviderReceiptV1_retry_after_ms_set(&builder, 0u) ||
        !SET_TEXT(ProviderReceiptV1, builder, message_id, fields[0]) ||
        !SET_TEXT(ProviderReceiptV1, builder, correlation_id, fields[1]) ||
        !SET_TEXT(ProviderReceiptV1, builder, causation_id, fields[2]) ||
        !SET_TEXT(ProviderReceiptV1, builder, tenant_id, fields[3]) ||
        !SET_TEXT(ProviderReceiptV1, builder, provider_id, fields[4]) ||
        !SET_TEXT(ProviderReceiptV1, builder, session_id, fields[5]) ||
        !SET_TEXT(ProviderReceiptV1, builder, partition_key, fields[6]) ||
        !SET_TEXT(ProviderReceiptV1, builder, producer_id, fields[7]) ||
        !SET_TEXT(ProviderReceiptV1, builder, created_at, fields[8]) ||
        !SET_TEXT(ProviderReceiptV1, builder, deadline_at, fields[9]) ||
        !SET_TEXT(ProviderReceiptV1, builder, command_id, fields[10]) ||
        !SET_TEXT(ProviderReceiptV1, builder, worker_id, fields[11]) ||
        !SET_TEXT(ProviderReceiptV1, builder, dispatch_epoch, fields[12]) ||
        !SET_TEXT(ProviderReceiptV1, builder, error_code, fields[13]) ||
        !SET_TEXT(ProviderReceiptV1, builder, error_message, fields[14]) ||
        !ProviderReceiptV1_view_bind(&view, *out, *out_size) ||
        iris_provider_validate_receipt(&view, &limits) != SALTS_OK) {
        free(*out);
        *out = NULL;
        *out_size = 0u;
        return IVR_ENOSPC;
    }
    return IVR_OK;
}

ivr_status_t iris_control_provider_encode_completion(
    const iris_media_completion_t *completion,
    const ivr_media_command_result_t *result, const char *provider_id,
    const char *producer_id, const char *message_id, const char *completed_at,
    uint64_t completed_at_unix_ms, uint8_t **out, size_t *out_size) {
    iris_provider_limits_t limits = IRIS_PROVIDER_LIMITS_INIT;
    ProviderCompletionV1_builder_t builder;
    ProviderCompletionV1_view_t view;
    char epoch[32], completed_ms[32];
    char *generated_result_json = NULL;
    const char *result_json;
    size_t result_json_size = 0u;
    ProviderTerminalStatus_t terminal_status;
    const char *event_type;
    const char *fields[20];
    int ok;
    if (!completion || !result || !provider_id || !provider_id[0] ||
        !producer_id || !producer_id[0] || !message_id || !message_id[0] ||
        !completed_at || !completed_at[0] || completed_at_unix_ms == 0u ||
        !out || !out_size || !completion->command_id[0] ||
        !completion->tenant_id[0] || !completion->provider_session_id[0] ||
        !completion->iris_worker_id[0] || completion->dispatch_epoch == 0u ||
        !format_u64(completion->dispatch_epoch, epoch, sizeof(epoch)) ||
        !format_u64(completed_at_unix_ms, completed_ms, sizeof(completed_ms))) {
        return IVR_EINVAL;
    }
    *out = NULL;
    *out_size = 0u;
    if (completion->result_json[0]) {
        result_json = completion->result_json;
        result_json_size = strlen(result_json);
    } else {
        generated_result_json = completion_result_json(result, &result_json_size);
        result_json = generated_result_json;
    }
    if (!result_json || result_json_size == 0u) {
        json_serialize_free(generated_result_json);
        return IVR_ENOSPC;
    }
    terminal_status = completion->terminal_status[0]
                          ? (strcmp(completion->terminal_status, "succeeded") == 0
                                 ? ProviderTerminalStatus_Succeeded
                                 : ProviderTerminalStatus_Failed)
                          : (result->status_code == IVR_OK
                                 ? ProviderTerminalStatus_Succeeded
                                 : ProviderTerminalStatus_Failed);
    event_type = completion->event_type[0]
                     ? completion->event_type
                     : (result->status_code == IVR_OK
                            ? "provider.media.completed"
                            : "provider.media.failed");
    fields[0] = message_id;
    fields[1] = completion->correlation_id;
    fields[2] = completion->command_id;
    fields[3] = completion->tenant_id;
    fields[4] = provider_id;
    fields[5] = completion->provider_session_id;
    fields[6] = completion->provider_session_id;
    fields[7] = producer_id;
    fields[8] = completed_at;
    fields[9] = "";
    fields[10] = completion->command_id;
    fields[11] = completion->iris_worker_id;
    fields[12] = epoch;
    fields[13] = message_id;
    fields[14] = event_type;
    fields[15] = completed_at;
    fields[16] = completed_ms;
    fields[17] = result_json;
    fields[18] = result->error_code;
    fields[19] = result->error_message;
    ok = allocate_wire(ProviderCompletionV1_BLOCK_LENGTH, fields, 20u, out,
                       out_size) &&
         ProviderCompletionV1_builder_bind(&builder, *out, *out_size) &&
         ProviderCompletionV1_schema_version_set(
             &builder, IRIS_PROVIDER_SCHEMA_VERSION) &&
         ProviderCompletionV1_message_kind_set(&builder,
                                               ProviderMessageKind_Completion) &&
         ProviderCompletionV1_terminal_status_set(&builder, terminal_status) &&
         SET_TEXT(ProviderCompletionV1, builder, message_id, fields[0]) &&
         SET_TEXT(ProviderCompletionV1, builder, correlation_id, fields[1]) &&
         SET_TEXT(ProviderCompletionV1, builder, causation_id, fields[2]) &&
         SET_TEXT(ProviderCompletionV1, builder, tenant_id, fields[3]) &&
         SET_TEXT(ProviderCompletionV1, builder, provider_id, fields[4]) &&
         SET_TEXT(ProviderCompletionV1, builder, session_id, fields[5]) &&
         SET_TEXT(ProviderCompletionV1, builder, partition_key, fields[6]) &&
         SET_TEXT(ProviderCompletionV1, builder, producer_id, fields[7]) &&
         SET_TEXT(ProviderCompletionV1, builder, created_at, fields[8]) &&
         SET_TEXT(ProviderCompletionV1, builder, deadline_at, fields[9]) &&
         SET_TEXT(ProviderCompletionV1, builder, command_id, fields[10]) &&
         SET_TEXT(ProviderCompletionV1, builder, worker_id, fields[11]) &&
         SET_TEXT(ProviderCompletionV1, builder, dispatch_epoch, fields[12]) &&
         SET_TEXT(ProviderCompletionV1, builder, event_id, fields[13]) &&
         SET_TEXT(ProviderCompletionV1, builder, event_type, fields[14]) &&
         SET_TEXT(ProviderCompletionV1, builder, completed_at, fields[15]) &&
         SET_TEXT(ProviderCompletionV1, builder, completed_at_unix_ms,
                  fields[16]) &&
         SET_TEXT(ProviderCompletionV1, builder, result_json, fields[17]) &&
         SET_TEXT(ProviderCompletionV1, builder, error_code, fields[18]) &&
         SET_TEXT(ProviderCompletionV1, builder, error_message, fields[19]) &&
         ProviderCompletionV1_view_bind(&view, *out, *out_size) &&
         iris_provider_validate_completion(&view, &limits) == SALTS_OK;
    json_serialize_free(generated_result_json);
    if (!ok) {
        free(*out);
        *out = NULL;
        *out_size = 0u;
        return IVR_ENOSPC;
    }
    return IVR_OK;
}

ivr_status_t iris_control_provider_decode_completion_ack(
    const void *encoded, size_t encoded_size,
    const iris_media_completion_t *completion, const char *provider_id,
    const char *iris_identity, const char *completion_message_id,
    iris_control_completion_ack_t *out) {
    iris_provider_limits_t limits = IRIS_PROVIDER_LIMITS_INIT;
    ProviderCompletionAckV1_view_t ack;
    ProviderMessageKind_t kind = ProviderMessageKind_Command;
    tbe_var_data_t causation_id, correlation_id, tenant_id, wire_provider_id;
    tbe_var_data_t session_id, partition_key, producer_id, command_id;
    tbe_var_data_t epoch_text, sequence_text;
    uint64_t epoch = 0u, sequence = 0u;
    if (!encoded || encoded_size == 0u || !completion ||
        !provider_id || !iris_identity || !completion_message_id || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (iris_provider_peek_kind(encoded, encoded_size, &kind) != SALTS_OK ||
        kind != ProviderMessageKind_CompletionAck ||
        !ProviderCompletionAckV1_view_bind(&ack, encoded, encoded_size) ||
        iris_provider_validate_completion_ack(&ack, &limits) != SALTS_OK ||
        !ProviderCompletionAckV1_causation_id(&ack, &causation_id) ||
        !ProviderCompletionAckV1_correlation_id(&ack, &correlation_id) ||
        !ProviderCompletionAckV1_tenant_id(&ack, &tenant_id) ||
        !ProviderCompletionAckV1_provider_id(&ack, &wire_provider_id) ||
        !ProviderCompletionAckV1_session_id(&ack, &session_id) ||
        !ProviderCompletionAckV1_partition_key(&ack, &partition_key) ||
        !ProviderCompletionAckV1_producer_id(&ack, &producer_id) ||
        !ProviderCompletionAckV1_command_id(&ack, &command_id) ||
        !ProviderCompletionAckV1_dispatch_epoch(&ack, &epoch_text) ||
        !ProviderCompletionAckV1_committed_sequence(&ack, &sequence_text) ||
        !parse_wire_u64(epoch_text, &epoch) ||
        !parse_wire_u64(sequence_text, &sequence) ||
        !wire_text_equal(causation_id, completion_message_id) ||
        !wire_text_equal(correlation_id, completion->correlation_id) ||
        !wire_text_equal(tenant_id, completion->tenant_id) ||
        !wire_text_equal(wire_provider_id, provider_id) ||
        !wire_text_equal(session_id, completion->provider_session_id) ||
        !wire_text_equal(partition_key, completion->provider_session_id) ||
        !wire_text_equal(producer_id, iris_identity) ||
        !wire_text_equal(command_id, completion->command_id) ||
        epoch != completion->dispatch_epoch) return IVR_ESTATE;
    out->disposition = ProviderCompletionAckV1_disposition_get(&ack);
    out->committed_sequence = sequence;
    return IVR_OK;
}

static char *canonical_event_payload(const ivr_media_event_t *event,
                                     size_t *out_size) {
    json_value_t *root = NULL;
    json_value_t *payload = NULL;
    char *json = NULL;
    if (event->payload_json[0]) {
        payload = json_parse(event->payload_json, strlen(event->payload_json));
        if (!payload) return NULL;
    } else {
        payload = json_create_object();
    }
    root = json_create_object();
    if (!root || !payload ||
        !add_member(root, "dialogId", json_create_string(event->dialog_id)) ||
        !add_member(root, "roomId", json_create_string(event->room_id)) ||
        !add_member(root, "callId", json_create_string(event->call_id)) ||
        !add_member(root, "callGeneration", json_create_uint64(event->call_generation)) ||
        (event->input_id[0] &&
         !add_member(root, "inputId", json_create_string(event->input_id))) ||
        (event->input_value[0] &&
         !add_member(root, "inputValue", json_create_string(event->input_value))) ||
        !add_member(root, "payload", payload)) {
        json_free(payload);
        json_free(root);
        return NULL;
    }
    payload = NULL;
    json = json_serialize(root, out_size);
    json_free(root);
    return json;
}

ivr_status_t iris_control_provider_encode_event(
    const ivr_media_event_t *event, const char *provider_id,
    const char *producer_id, const char *message_id, const char *created_at,
    const char *occurred_at, uint8_t **out, size_t *out_size) {
    iris_provider_limits_t limits = IRIS_PROVIDER_LIMITS_INIT;
    ProviderEventV1_builder_t builder;
    ProviderEventV1_view_t view;
    char sequence[32];
    char *payload_json;
    size_t payload_json_size = 0u;
    const char *fields[16];
    int ok;
    if (!event || !provider_id || !provider_id[0] || !producer_id ||
        !producer_id[0] || !message_id || !message_id[0] || !created_at ||
        !created_at[0] || !occurred_at || !occurred_at[0] || !out ||
        !out_size || !event->event_id[0] || !event->tenant_id[0] ||
        !event->provider_session_id[0] || !event->dialog_id[0] ||
        !event->event_type[0] || event->sequence == 0u ||
        !format_u64(event->sequence, sequence, sizeof(sequence))) return IVR_EINVAL;
    *out = NULL;
    *out_size = 0u;
    payload_json = canonical_event_payload(event, &payload_json_size);
    if (!payload_json || payload_json_size == 0u) {
        json_serialize_free(payload_json);
        return IVR_ENOSPC;
    }
    fields[0] = message_id;
    fields[1] = event->dialog_id;
    fields[2] = "";
    fields[3] = event->tenant_id;
    fields[4] = provider_id;
    fields[5] = event->provider_session_id;
    fields[6] = event->provider_session_id;
    fields[7] = producer_id;
    fields[8] = created_at;
    fields[9] = "";
    fields[10] = event->event_id;
    fields[11] = event->event_type;
    fields[12] = event->dialog_id;
    fields[13] = sequence;
    fields[14] = occurred_at;
    fields[15] = payload_json;
    ok = allocate_wire(ProviderEventV1_BLOCK_LENGTH, fields, 16u, out, out_size) &&
         ProviderEventV1_builder_bind(&builder, *out, *out_size) &&
         ProviderEventV1_schema_version_set(&builder,
                                            IRIS_PROVIDER_SCHEMA_VERSION) &&
         ProviderEventV1_message_kind_set(&builder, ProviderMessageKind_Event) &&
         SET_TEXT(ProviderEventV1, builder, message_id, fields[0]) &&
         SET_TEXT(ProviderEventV1, builder, correlation_id, fields[1]) &&
         SET_TEXT(ProviderEventV1, builder, causation_id, fields[2]) &&
         SET_TEXT(ProviderEventV1, builder, tenant_id, fields[3]) &&
         SET_TEXT(ProviderEventV1, builder, provider_id, fields[4]) &&
         SET_TEXT(ProviderEventV1, builder, session_id, fields[5]) &&
         SET_TEXT(ProviderEventV1, builder, partition_key, fields[6]) &&
         SET_TEXT(ProviderEventV1, builder, producer_id, fields[7]) &&
         SET_TEXT(ProviderEventV1, builder, created_at, fields[8]) &&
         SET_TEXT(ProviderEventV1, builder, deadline_at, fields[9]) &&
         SET_TEXT(ProviderEventV1, builder, event_id, fields[10]) &&
         SET_TEXT(ProviderEventV1, builder, event_type, fields[11]) &&
         SET_TEXT(ProviderEventV1, builder, aggregate_id, fields[12]) &&
         SET_TEXT(ProviderEventV1, builder, sequence, fields[13]) &&
         SET_TEXT(ProviderEventV1, builder, occurred_at, fields[14]) &&
         SET_TEXT(ProviderEventV1, builder, payload_json, fields[15]) &&
         ProviderEventV1_view_bind(&view, *out, *out_size) &&
         iris_provider_validate_event(&view, &limits) == SALTS_OK;
    json_serialize_free(payload_json);
    if (!ok) {
        free(*out);
        *out = NULL;
        *out_size = 0u;
        return IVR_ENOSPC;
    }
    return IVR_OK;
}

ivr_status_t iris_control_provider_decode_event_ack(
    const void *encoded, size_t encoded_size,
    const ivr_media_event_t *event, const char *provider_id,
    const char *iris_identity, const char *event_message_id,
    iris_control_event_ack_t *out) {
    iris_provider_limits_t limits = IRIS_PROVIDER_LIMITS_INIT;
    ProviderEventAckV1_view_t ack;
    ProviderMessageKind_t kind = ProviderMessageKind_Command;
    ProviderEventAckDisposition_t disposition;
    tbe_var_data_t causation_id, correlation_id, tenant_id, wire_provider_id;
    tbe_var_data_t session_id, partition_key, producer_id, event_id, sequence_text;
    uint64_t sequence = 0u;
    if (!encoded || encoded_size == 0u || !event || !provider_id ||
        !iris_identity || !event_message_id || !out) return IVR_EINVAL;
    memset(out, 0, sizeof(*out));
    if (iris_provider_peek_kind(encoded, encoded_size, &kind) != SALTS_OK ||
        kind != ProviderMessageKind_EventAck ||
        !ProviderEventAckV1_view_bind(&ack, encoded, encoded_size) ||
        iris_provider_validate_event_ack(&ack, &limits) != SALTS_OK ||
        !ProviderEventAckV1_causation_id(&ack, &causation_id) ||
        !ProviderEventAckV1_correlation_id(&ack, &correlation_id) ||
        !ProviderEventAckV1_tenant_id(&ack, &tenant_id) ||
        !ProviderEventAckV1_provider_id(&ack, &wire_provider_id) ||
        !ProviderEventAckV1_session_id(&ack, &session_id) ||
        !ProviderEventAckV1_partition_key(&ack, &partition_key) ||
        !ProviderEventAckV1_producer_id(&ack, &producer_id) ||
        !ProviderEventAckV1_event_id(&ack, &event_id) ||
        !ProviderEventAckV1_committed_sequence(&ack, &sequence_text) ||
        !parse_wire_u64(sequence_text, &sequence) ||
        !wire_text_equal(causation_id, event_message_id) ||
        !wire_text_equal(correlation_id, event->dialog_id) ||
        !wire_text_equal(tenant_id, event->tenant_id) ||
        !wire_text_equal(wire_provider_id, provider_id) ||
        !wire_text_equal(session_id, event->provider_session_id) ||
        !wire_text_equal(partition_key, event->provider_session_id) ||
        !wire_text_equal(producer_id, iris_identity) ||
        !wire_text_equal(event_id, event->event_id)) return IVR_ESTATE;
    disposition = ProviderEventAckV1_disposition_get(&ack);
    if (((disposition == ProviderEventAckDisposition_Committed ||
          disposition == ProviderEventAckDisposition_DuplicateEvent) &&
         sequence == 0u) ||
        ((disposition == ProviderEventAckDisposition_EventConflict ||
          disposition == ProviderEventAckDisposition_EventRejected) &&
         sequence != 0u)) return IVR_ESTATE;
    out->disposition = disposition;
    out->committed_sequence = sequence;
    return IVR_OK;
}

void iris_control_provider_observation_init(
    iris_control_provider_observation_t *observation) {
    if (observation) memset(observation, 0, sizeof(*observation));
}

void iris_control_provider_observation_clear(
    iris_control_provider_observation_t *observation) {
    if (!observation) return;
    free(observation->wire.payload_json);
    memset(observation, 0, sizeof(*observation));
}

ivr_status_t iris_control_provider_encode_query(
    const char *tenant_id, const char *provider_id, const char *producer_id,
    const char *query_id, const char *query_type, const char *created_at,
    const char *deadline_at, uint64_t expected_revision, uint64_t cursor,
    uint32_t limit, const char *payload_json, uint8_t **out,
    size_t *out_size) {
    iris_provider_limits_t limits = IRIS_PROVIDER_LIMITS_INIT;
    ProviderQueryV1_builder_t builder;
    ProviderQueryV1_view_t view;
    char revision[32], cursor_text[32];
    const char *fields[15];
    if (!tenant_id || !tenant_id[0] || !provider_id || !provider_id[0] ||
        !producer_id || !producer_id[0] || !query_id || !query_id[0] ||
        !query_type || !query_type[0] || !created_at || !created_at[0] ||
        !deadline_at || !deadline_at[0] || limit == 0u || !payload_json ||
        !payload_json[0] || !out || !out_size ||
        !format_u64(expected_revision, revision, sizeof(revision)) ||
        !format_u64(cursor, cursor_text, sizeof(cursor_text))) return IVR_EINVAL;
    *out = NULL;
    *out_size = 0u;
    fields[0] = query_id;
    fields[1] = "";
    fields[2] = "";
    fields[3] = tenant_id;
    fields[4] = provider_id;
    fields[5] = "";
    fields[6] = tenant_id;
    fields[7] = producer_id;
    fields[8] = created_at;
    fields[9] = deadline_at;
    fields[10] = query_id;
    fields[11] = query_type;
    fields[12] = revision;
    fields[13] = cursor_text;
    fields[14] = payload_json;
    if (!allocate_wire(ProviderQueryV1_BLOCK_LENGTH, fields, 15u, out, out_size) ||
        !ProviderQueryV1_builder_bind(&builder, *out, *out_size) ||
        !ProviderQueryV1_schema_version_set(&builder,
                                            IRIS_PROVIDER_SCHEMA_VERSION) ||
        !ProviderQueryV1_message_kind_set(&builder, ProviderMessageKind_Query) ||
        !ProviderQueryV1_limit_set(&builder, limit) ||
        !SET_TEXT(ProviderQueryV1, builder, message_id, fields[0]) ||
        !SET_TEXT(ProviderQueryV1, builder, correlation_id, fields[1]) ||
        !SET_TEXT(ProviderQueryV1, builder, causation_id, fields[2]) ||
        !SET_TEXT(ProviderQueryV1, builder, tenant_id, fields[3]) ||
        !SET_TEXT(ProviderQueryV1, builder, provider_id, fields[4]) ||
        !SET_TEXT(ProviderQueryV1, builder, session_id, fields[5]) ||
        !SET_TEXT(ProviderQueryV1, builder, partition_key, fields[6]) ||
        !SET_TEXT(ProviderQueryV1, builder, producer_id, fields[7]) ||
        !SET_TEXT(ProviderQueryV1, builder, created_at, fields[8]) ||
        !SET_TEXT(ProviderQueryV1, builder, deadline_at, fields[9]) ||
        !SET_TEXT(ProviderQueryV1, builder, query_id, fields[10]) ||
        !SET_TEXT(ProviderQueryV1, builder, query_type, fields[11]) ||
        !SET_TEXT(ProviderQueryV1, builder, expected_revision, fields[12]) ||
        !SET_TEXT(ProviderQueryV1, builder, cursor, fields[13]) ||
        !SET_TEXT(ProviderQueryV1, builder, payload_json, fields[14]) ||
        !ProviderQueryV1_view_bind(&view, *out, *out_size) ||
        iris_provider_validate_query(&view, &limits) != SALTS_OK) {
        free(*out);
        *out = NULL;
        *out_size = 0u;
        return IVR_ENOSPC;
    }
    return IVR_OK;
}

ivr_status_t iris_control_provider_decode_observation(
    const void *encoded, size_t encoded_size,
    const char *tenant_id, const char *provider_id, const char *iris_identity,
    const char *query_id, const char *query_type, uint64_t expected_cursor,
    iris_control_provider_observation_t *out) {
    iris_provider_limits_t limits = IRIS_PROVIDER_LIMITS_INIT;
    ProviderObservationV1_view_t view;
    ProviderMessageKind_t kind = ProviderMessageKind_Command;
    ProviderQueryStatus_t status;
    uint8_t has_more;
    tbe_var_data_t correlation_id, causation_id, wire_tenant_id, wire_provider_id;
    tbe_var_data_t session_id, partition_key, producer_id, wire_query_id;
    tbe_var_data_t observation_type, revision_text, cursor_text, next_cursor_text;
    tbe_var_data_t payload_json, error_code, error_message;
    uint64_t revision = 0u, cursor = 0u, next_cursor = 0u;
    if (!encoded || encoded_size == 0u || !tenant_id ||
        !provider_id || !iris_identity || !query_id || !query_type || !out) {
        return IVR_EINVAL;
    }
    iris_control_provider_observation_init(out);
    if (iris_provider_peek_kind(encoded, encoded_size, &kind) != SALTS_OK ||
        kind != ProviderMessageKind_Observation ||
        !ProviderObservationV1_view_bind(&view, encoded, encoded_size) ||
        iris_provider_validate_observation(&view, &limits) != SALTS_OK ||
        !ProviderObservationV1_correlation_id(&view, &correlation_id) ||
        !ProviderObservationV1_causation_id(&view, &causation_id) ||
        !ProviderObservationV1_tenant_id(&view, &wire_tenant_id) ||
        !ProviderObservationV1_provider_id(&view, &wire_provider_id) ||
        !ProviderObservationV1_session_id(&view, &session_id) ||
        !ProviderObservationV1_partition_key(&view, &partition_key) ||
        !ProviderObservationV1_producer_id(&view, &producer_id) ||
        !ProviderObservationV1_query_id(&view, &wire_query_id) ||
        !ProviderObservationV1_observation_type(&view, &observation_type) ||
        !ProviderObservationV1_revision(&view, &revision_text) ||
        !ProviderObservationV1_cursor(&view, &cursor_text) ||
        !ProviderObservationV1_next_cursor(&view, &next_cursor_text) ||
        !ProviderObservationV1_payload_json(&view, &payload_json) ||
        !ProviderObservationV1_error_code(&view, &error_code) ||
        !ProviderObservationV1_error_message(&view, &error_message) ||
        !parse_wire_u64(revision_text, &revision) ||
        !parse_wire_u64(cursor_text, &cursor) ||
        !parse_wire_u64(next_cursor_text, &next_cursor) ||
        !wire_text_equal(correlation_id, query_id) ||
        !wire_text_equal(causation_id, query_id) ||
        !wire_text_equal(wire_tenant_id, tenant_id) ||
        !wire_text_equal(wire_provider_id, provider_id) ||
        !wire_text_empty(session_id) || !wire_text_equal(partition_key, tenant_id) ||
        !wire_text_equal(producer_id, iris_identity) ||
        !wire_text_equal(wire_query_id, query_id) ||
        !wire_text_equal(observation_type, query_type)) return IVR_ESTATE;
    status = ProviderObservationV1_status_get(&view);
    has_more = ProviderObservationV1_has_more_get(&view);
    if ((has_more && next_cursor <= cursor) || (!has_more && next_cursor != 0u) ||
        (status == ProviderQueryStatus_QueryOk &&
         (!wire_text_empty(error_code) || !wire_text_empty(error_message))) ||
        (status != ProviderQueryStatus_QueryOk && wire_text_empty(error_code))) {
        return IVR_ESTATE;
    }
    if (!duplicate_wire_text(&out->wire.payload_json, payload_json)) return IVR_ENOSPC;
    out->wire.status = status;
    out->wire.has_more = has_more;
    out->revision = revision;
    out->cursor = cursor;
    out->next_cursor = next_cursor;
    return IVR_OK;
}

static int bounded_nonempty(const char *value, size_t capacity) {
    return value && capacity > 1u && value[0] && memchr(value, '\0', capacity);
}

ivr_status_t iris_control_provider_encode_call_offer(
    const iris_control_call_offer_t *offer, const char *provider_id,
    const char *producer_id, uint8_t **out, size_t *out_size) {
    iris_provider_limits_t limits = IRIS_PROVIDER_LIMITS_INIT;
    ProviderCallOfferV1_builder_t builder;
    ProviderCallOfferV1_view_t view;
    char generation[32];
    const char *fields[17];
    if (!offer || !provider_id || !provider_id[0] || !producer_id ||
        !producer_id[0] || !out || !out_size ||
        !bounded_nonempty(offer->ingress_event_id, sizeof(offer->ingress_event_id)) ||
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
    fields[0] = offer->ingress_event_id;
    fields[1] = offer->call_id;
    fields[2] = "";
    fields[3] = offer->tenant_id;
    fields[4] = provider_id;
    fields[5] = "";
    fields[6] = offer->call_id;
    fields[7] = producer_id;
    fields[8] = offer->created_at;
    fields[9] = offer->deadline_at;
    fields[10] = offer->ingress_event_id;
    fields[11] = offer->call_id;
    fields[12] = generation;
    fields[13] = offer->transport;
    fields[14] = offer->source;
    fields[15] = offer->destination;
    fields[16] = offer->payload_json;
    if (!allocate_wire(ProviderCallOfferV1_BLOCK_LENGTH, fields, 17u, out, out_size) ||
        !ProviderCallOfferV1_builder_bind(&builder, *out, *out_size) ||
        !ProviderCallOfferV1_schema_version_set(
            &builder, IRIS_PROVIDER_SCHEMA_VERSION) ||
        !ProviderCallOfferV1_message_kind_set(&builder,
                                              ProviderMessageKind_CallOffer) ||
        !SET_TEXT(ProviderCallOfferV1, builder, message_id, fields[0]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, correlation_id, fields[1]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, causation_id, fields[2]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, tenant_id, fields[3]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, provider_id, fields[4]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, session_id, fields[5]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, partition_key, fields[6]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, producer_id, fields[7]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, created_at, fields[8]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, deadline_at, fields[9]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, ingress_event_id, fields[10]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, call_id, fields[11]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, call_generation, fields[12]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, transport, fields[13]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, source, fields[14]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, destination, fields[15]) ||
        !SET_TEXT(ProviderCallOfferV1, builder, payload_json, fields[16]) ||
        !ProviderCallOfferV1_view_bind(&view, *out, *out_size) ||
        iris_provider_validate_call_offer(&view, &limits) != SALTS_OK) {
        free(*out);
        *out = NULL;
        *out_size = 0u;
        return IVR_ENOSPC;
    }
    return IVR_OK;
}

ivr_status_t iris_control_provider_decode_session_bound(
    const void *encoded, size_t encoded_size,
    const iris_control_call_offer_t *offer, const char *provider_id,
    const char *iris_identity, iris_control_session_bound_t *out) {
    iris_provider_limits_t limits = IRIS_PROVIDER_LIMITS_INIT;
    ProviderSessionBoundV1_view_t view;
    ProviderMessageKind_t kind = ProviderMessageKind_Command;
    tbe_var_data_t message_id, correlation_id, causation_id, tenant_id;
    tbe_var_data_t wire_provider_id, session_id, partition_key, producer_id;
    tbe_var_data_t ingress_event_id, call_id, generation_text;
    tbe_var_data_t bound_session_id, error_code, error_message;
    uint64_t generation = 0u;
    int accepted;
    if (!encoded || encoded_size == 0u || !offer || !provider_id ||
        !iris_identity || !out) return IVR_EINVAL;
    memset(out, 0, sizeof(*out));
    if (iris_provider_peek_kind(encoded, encoded_size, &kind) != SALTS_OK ||
        kind != ProviderMessageKind_SessionBound ||
        !ProviderSessionBoundV1_view_bind(&view, encoded, encoded_size) ||
        iris_provider_validate_session_bound(&view, &limits) != SALTS_OK ||
        !ProviderSessionBoundV1_message_id(&view, &message_id) ||
        !ProviderSessionBoundV1_correlation_id(&view, &correlation_id) ||
        !ProviderSessionBoundV1_causation_id(&view, &causation_id) ||
        !ProviderSessionBoundV1_tenant_id(&view, &tenant_id) ||
        !ProviderSessionBoundV1_provider_id(&view, &wire_provider_id) ||
        !ProviderSessionBoundV1_session_id(&view, &session_id) ||
        !ProviderSessionBoundV1_partition_key(&view, &partition_key) ||
        !ProviderSessionBoundV1_producer_id(&view, &producer_id) ||
        !ProviderSessionBoundV1_ingress_event_id(&view, &ingress_event_id) ||
        !ProviderSessionBoundV1_call_id(&view, &call_id) ||
        !ProviderSessionBoundV1_call_generation(&view, &generation_text) ||
        !ProviderSessionBoundV1_bound_session_id(&view, &bound_session_id) ||
        !ProviderSessionBoundV1_error_code(&view, &error_code) ||
        !ProviderSessionBoundV1_error_message(&view, &error_message) ||
        !parse_wire_u64(generation_text, &generation) ||
        !wire_text_equal(correlation_id, offer->call_id) ||
        !wire_text_equal(causation_id, offer->ingress_event_id) ||
        !wire_text_equal(tenant_id, offer->tenant_id) ||
        !wire_text_equal(wire_provider_id, provider_id) ||
        !wire_text_equal(partition_key, offer->call_id) ||
        !wire_text_equal(producer_id, iris_identity) ||
        !wire_text_equal(ingress_event_id, offer->ingress_event_id) ||
        !wire_text_equal(call_id, offer->call_id) ||
        generation != offer->call_generation) return IVR_ESTATE;
    accepted = ProviderSessionBoundV1_accepted_get(&view) != 0u;
    if ((accepted &&
         (wire_text_empty(session_id) || !wire_text_same(session_id, bound_session_id) ||
          !wire_text_empty(error_code) || !wire_text_empty(error_message))) ||
        (!accepted &&
         (!wire_text_empty(session_id) || !wire_text_empty(bound_session_id) ||
          wire_text_empty(error_code))) ||
        !copy_wire_text(out->message_id, sizeof(out->message_id), message_id) ||
        !copy_wire_text(out->bound_session_id, sizeof(out->bound_session_id),
                        bound_session_id) ||
        !copy_wire_text(out->error_code, sizeof(out->error_code), error_code) ||
        !copy_wire_text(out->error_message, sizeof(out->error_message), error_message)) {
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    out->accepted = accepted;
    return IVR_OK;
}

#undef SET_TEXT
