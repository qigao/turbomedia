#include "iris_media_bridge.h"
#include "iris_command_fingerprint.h"

#include <platform.h>
#include <json_parser.h>
#include <datetime_parser.h>
#include <salts_thread.h>
#include <salts/clock.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IRIS_MEDIA_PROVIDER_ID "turbomedia"
#define IRIS_MEDIA_CAPABILITY "ivr"
#define IRIS_MEDIA_SCHEMA_VERSION UINT64_C(2)
#define IRIS_MEDIA_MAX_REQUEST_BYTES 16384u
#define IRIS_MEDIA_ENVELOPE_ID_CAPACITY 128u

typedef enum iris_media_entry_state_e {
    IRIS_MEDIA_ENTRY_FREE = 0,
    IRIS_MEDIA_ENTRY_DISPATCHING,
    IRIS_MEDIA_ENTRY_ACCEPTING,
    IRIS_MEDIA_ENTRY_ACCEPTED,
    IRIS_MEDIA_ENTRY_TERMINAL_COMMITTING,
    IRIS_MEDIA_ENTRY_COMPLETING,
    IRIS_MEDIA_ENTRY_COMPLETED
} iris_media_entry_state_t;

typedef struct iris_media_request_s {
    ivr_media_command_t command;
    char tenant_id[IRIS_MEDIA_ENVELOPE_ID_CAPACITY];
    char correlation_id[IRIS_MEDIA_ENVELOPE_ID_CAPACITY];
    char causation_id[IRIS_MEDIA_ENVELOPE_ID_CAPACITY];
    char iris_worker_id[IRIS_MEDIA_ENVELOPE_ID_CAPACITY];
    uint64_t deadline_unix_ms;
    uint64_t dispatch_epoch;
} iris_media_request_t;

typedef struct iris_media_entry_s {
    iris_media_entry_state_t state;
    iris_media_request_t request;
    char media_worker_id[IRIS_MEDIA_ENVELOPE_ID_CAPACITY];
    uint64_t completed_sequence;
    iris_command_identity_t identity;
} iris_media_entry_t;

struct iris_media_bridge_s {
    iris_media_entry_t *entries;
    size_t capacity;
    salts_mutex_t mutex;
    salts_cond_t state_changed;
    iris_media_bridge_send_fn send;
    void *send_context;
    iris_media_bridge_observe_fn observe;
    void *observe_context;
    iris_media_bridge_realtime_ms_fn realtime_ms;
    void *realtime_context;
    uint64_t next_completed_sequence;
    iris_command_ledger_port_t ledger;
};

static iris_media_bridge_result_t bridge_result(
    iris_media_bridge_status_t status, const iris_media_request_t *request,
    const char *media_worker_id, const char *code, const char *message) {
    iris_media_bridge_result_t result;

    memset(&result, 0, sizeof(result));
    result.status = status;
    result.error_code = code;
    result.error_message = message;
    if (request) {
        memcpy(result.command_id, request->command.message_id,
               sizeof(result.command_id));
        memcpy(result.iris_worker_id, request->iris_worker_id,
               sizeof(result.iris_worker_id));
        result.dispatch_epoch = request->dispatch_epoch;
    }
    if (media_worker_id) {
        size_t size = strlen(media_worker_id);
        if (size < sizeof(result.media_worker_id)) {
            memcpy(result.media_worker_id, media_worker_id, size + 1u);
        }
    }
    return result;
}

static iris_media_bridge_result_t terminal_replay_result(
    const iris_media_request_t *request,
    const iris_command_claim_result_t *claim) {
    iris_media_bridge_result_t result = bridge_result(
        IRIS_MEDIA_BRIDGE_TERMINAL_REPLAY, request, NULL, NULL, NULL);
    size_t terminal_size = strlen(claim->terminal_status);
    size_t event_size = strlen(claim->event_type);
    size_t data_size = strlen(claim->result_json);
    if (terminal_size >= sizeof(result.terminal_status) ||
        event_size >= sizeof(result.event_type) ||
        data_size >= sizeof(result.data)) {
        return bridge_result(IRIS_MEDIA_BRIDGE_INTERNAL, request, NULL,
                             "COMMAND_LEDGER_RECORD_INVALID",
                             "provider command ledger result is invalid");
    }
    memcpy(result.terminal_status, claim->terminal_status,
           terminal_size + 1u);
    memcpy(result.event_type, claim->event_type, event_size + 1u);
    memcpy(result.data, claim->result_json, data_size + 1u);
    return result;
}

static iris_media_bridge_result_t terminal_outcome_result(
    iris_media_bridge_status_t status,
    const iris_media_request_t *request,
    const iris_command_terminal_outcome_t *outcome) {
    iris_media_bridge_result_t result =
        bridge_result(status, request, NULL, NULL, NULL);
    size_t terminal_size = strlen(outcome->terminal_status);
    size_t event_size = strlen(outcome->event_type);
    size_t data_size = strlen(outcome->result_json);
    if (terminal_size >= sizeof(result.terminal_status) ||
        event_size >= sizeof(result.event_type) ||
        data_size >= sizeof(result.data)) {
        return bridge_result(IRIS_MEDIA_BRIDGE_INTERNAL, request, NULL,
                             "COMMAND_LEDGER_RECORD_INVALID",
                             "provider command terminal result is invalid");
    }
    memcpy(result.terminal_status, outcome->terminal_status,
           terminal_size + 1u);
    memcpy(result.event_type, outcome->event_type, event_size + 1u);
    memcpy(result.data, outcome->result_json, data_size + 1u);
    return result;
}

static const char *command_type_name(ivr_media_command_kind_t kind) {
    switch (kind) {
        case IVR_MEDIA_COMMAND_SESSION_OPEN: return "dialog.start";
        case IVR_MEDIA_COMMAND_SESSION_CLOSE: return "dialog.terminate";
        case IVR_MEDIA_COMMAND_PLAY: return "media.play";
        case IVR_MEDIA_COMMAND_INPUT_START: return "media.collect";
        case IVR_MEDIA_COMMAND_CANCEL: return "media.cancel";
        default: return "";
    }
}

static int copy_fixed(char *destination, size_t capacity,
                      const char *source) {
    size_t size;
    if (!destination || capacity == 0u || !source) return 0;
    size = strlen(source);
    if (size == 0u || size >= capacity) return 0;
    memcpy(destination, source, size + 1u);
    return 1;
}

static int command_identity(const iris_media_request_t *request,
                            iris_command_identity_t *identity) {
    iris_command_fingerprint_t fingerprint;
    const ivr_media_command_t *command = &request->command;
    const char *type = command_type_name(command->kind);
    memset(identity, 0, sizeof(*identity));
    if (!type[0] ||
        !copy_fixed(identity->command_id, sizeof(identity->command_id),
                    command->message_id) ||
        !copy_fixed(identity->provider_session_id,
                    sizeof(identity->provider_session_id),
                    command->provider_session_id) ||
        !copy_fixed(identity->command_type, sizeof(identity->command_type),
                    type) ||
        !iris_command_fingerprint_init(&fingerprint)) {
        return 0;
    }
    if (command->kind == IVR_MEDIA_COMMAND_INPUT_START ||
        command->kind == IVR_MEDIA_COMMAND_CANCEL) {
        if (!copy_fixed(identity->resource_scope_id,
                        sizeof(identity->resource_scope_id),
                        command->dialog_id) ||
            !copy_fixed(identity->resource_id,
                        sizeof(identity->resource_id), command->input_id)) {
            return 0;
        }
        identity->resource_scope_generation = command->call_generation;
        identity->resource_generation = command->input_generation;
    } else {
        if (!copy_fixed(identity->resource_scope_id,
                        sizeof(identity->resource_scope_id),
                        command->call_id) ||
            !copy_fixed(identity->resource_id,
                        sizeof(identity->resource_id),
                        command->dialog_id)) {
            return 0;
        }
        identity->resource_scope_generation = command->call_generation;
        /* dialogId is never reused within one call generation. Operation
           generation remains in the semantic fingerprint, while durable
           reconciliation keys the dialog incarnation itself. */
        identity->resource_generation = command->call_generation;
    }
    return iris_command_fingerprint_add_u64(
               &fingerprint, IRIS_MEDIA_SCHEMA_VERSION) &&
           iris_command_fingerprint_add_u64(&fingerprint,
                                             (uint64_t)command->kind) &&
           iris_command_fingerprint_add_u64(
               &fingerprint, request->deadline_unix_ms) &&
           iris_command_fingerprint_add_text(
               &fingerprint, IRIS_MEDIA_PROVIDER_ID) &&
           iris_command_fingerprint_add_text(
               &fingerprint, IRIS_MEDIA_CAPABILITY) &&
           iris_command_fingerprint_add_text(&fingerprint,
                                             request->tenant_id) &&
           iris_command_fingerprint_add_text(
               &fingerprint, command->provider_session_id) &&
           iris_command_fingerprint_add_text(&fingerprint, type) &&
           iris_command_fingerprint_add_text(
               &fingerprint, request->correlation_id) &&
           iris_command_fingerprint_add_text(
               &fingerprint, request->causation_id) &&
           iris_command_fingerprint_add_text(&fingerprint,
                                             command->dialog_id) &&
           iris_command_fingerprint_add_text(&fingerprint,
                                             command->room_id) &&
           iris_command_fingerprint_add_text(&fingerprint,
                                             command->call_id) &&
           iris_command_fingerprint_add_u64(
               &fingerprint, command->call_generation) &&
           iris_command_fingerprint_add_u64(
               &fingerprint, command->operation_generation) &&
           iris_command_fingerprint_add_text(&fingerprint, command->text) &&
           iris_command_fingerprint_add_text(&fingerprint,
                                             command->input_id) &&
           iris_command_fingerprint_add_u64(
               &fingerprint, command->input_generation) &&
           iris_command_fingerprint_add_text(&fingerprint,
                                             command->reason) &&
           iris_command_fingerprint_final(
               &fingerprint, identity->semantic_fingerprint);
}

static int add_json_member(json_value_t *object, const char *name,
                           json_value_t *value) {
    if (!value || !json_object_add_checked(object, name, value)) {
        json_free(value);
        value = NULL;
        return 0;
    }
    return 1;
}

static int absence_terminal_outcome(
    const iris_media_request_t *request, int already_absent,
    iris_command_terminal_outcome_t *outcome) {
    json_value_t *data = json_create_object();
    char *json = NULL;
    size_t json_size = 0u;
    const char *event_type;
    const char *terminal_status = already_absent ? "succeeded" : "failed";
    int valid;
    if (!request || !outcome || !data) {
        json_free(data);
        data = NULL;
        return 0;
    }
    event_type = request->command.kind == IVR_MEDIA_COMMAND_SESSION_CLOSE
                     ? (already_absent ? "provider.dialog.terminated"
                                       : "provider.dialog.failed")
                     : (already_absent ? "provider.media.cancelled"
                                       : "provider.media.failed");
    valid = add_json_member(
                data, "dialogId",
                json_create_string(request->command.dialog_id)) &&
            add_json_member(
                data, "roomId",
                json_create_string(request->command.room_id)) &&
            add_json_member(
                data, "callId",
                json_create_string(request->command.call_id)) &&
            add_json_member(
                data, "callGeneration",
                json_create_uint64(
                    request->command.call_generation)) &&
            add_json_member(
                data, "operationGeneration",
                json_create_uint64(
                    request->command.operation_generation)) &&
            (request->command.kind != IVR_MEDIA_COMMAND_CANCEL ||
             (add_json_member(
                  data, "inputId",
                  json_create_string(request->command.input_id)) &&
              add_json_member(
                  data, "inputGeneration",
                  json_create_uint64(
                      request->command.input_generation)))) &&
            add_json_member(data, "alreadyAbsent",
                            json_create_bool(already_absent));
    if (!already_absent) {
        valid = valid &&
                add_json_member(
                    data, "errorCode",
                    json_create_string("MEDIA_RESOURCE_NOT_FOUND")) &&
                add_json_member(
                    data, "errorMessage",
                    json_create_string(
                        "matching media resource was not found"));
    }
    if (valid) json = json_serialize(data, &json_size);
    json_free(data);
    data = NULL;
    if (!json || json_size == 0u ||
        json_size >= sizeof(outcome->result_json) ||
        strlen(terminal_status) >= sizeof(outcome->terminal_status) ||
        strlen(event_type) >= sizeof(outcome->event_type)) {
        json_serialize_free(json);
        return 0;
    }
    memset(outcome, 0, sizeof(*outcome));
    memcpy(outcome->terminal_status, terminal_status,
           strlen(terminal_status) + 1u);
    memcpy(outcome->event_type, event_type, strlen(event_type) + 1u);
    memcpy(outcome->result_json, json, json_size);
    outcome->result_json[json_size] = '\0';
    json_serialize_free(json);
    return 1;
}

static int terminal_outcome_from_result(
    const ivr_media_command_result_t *result,
    iris_command_terminal_outcome_t *outcome) {
    json_value_t *data = NULL;
    char *json = NULL;
    size_t json_size = 0u;
    const char *terminal = result->status_code == IVR_OK
                               ? "succeeded"
                               : "failed";
    const char *status = result->status_code == IVR_OK
                             ? "completed"
                             : "failed";
    const char *event_type = result->status_code == IVR_OK
                                 ? "provider.media.completed"
                                 : "provider.media.failed";
    int valid = 0;

    memset(outcome, 0, sizeof(*outcome));
    data = json_create_object();
    if (!data ||
        !add_json_member(data, "status", json_create_string(status)) ||
        !add_json_member(data, "mediaWorkerId",
                         json_create_string(result->worker_id)) ||
        !add_json_member(data, "dialogId",
                         json_create_string(result->dialog_id)) ||
        !add_json_member(data, "roomId",
                         json_create_string(result->room_id)) ||
        !add_json_member(data, "callId",
                         json_create_string(result->call_id)) ||
        !add_json_member(data, "callGeneration",
                         json_create_uint64(result->call_generation)) ||
        !add_json_member(
            data, "operationGeneration",
            json_create_uint64(result->operation_generation))) {
        goto cleanup;
    }
    if (result->error_code[0] &&
        !add_json_member(data, "errorCode",
                         json_create_string(result->error_code))) {
        goto cleanup;
    }
    if (result->error_message[0] &&
        !add_json_member(data, "errorMessage",
                         json_create_string(result->error_message))) {
        goto cleanup;
    }
    json = json_serialize(data, &json_size);
    if (!json || json_size == 0u ||
        json_size >= sizeof(outcome->result_json) ||
        !copy_fixed(outcome->terminal_status,
                    sizeof(outcome->terminal_status), terminal) ||
        !copy_fixed(outcome->event_type, sizeof(outcome->event_type),
                    event_type)) {
        goto cleanup;
    }
    memcpy(outcome->result_json, json, json_size);
    outcome->result_json[json_size] = '\0';
    valid = 1;

cleanup:
    json_serialize_free(json);
    json_free(data);
    data = NULL;
    return valid;
}

static iris_media_bridge_result_t invalid_result(const char *code,
                                                  const char *message) {
    return bridge_result(IRIS_MEDIA_BRIDGE_INVALID, NULL, NULL, code, message);
}

static int copy_string_field(const json_value_t *object, const char *key,
                             char *destination, size_t capacity,
                             int required) {
    json_value_t *value;
    const char *text;
    size_t size;

    if (!object || !key || !destination || capacity == 0u) {
        return 0;
    }
    value = json_object_get(object, key);
    if (!value) {
        destination[0] = '\0';
        return !required;
    }
    if (json_type(value) != JSON_STRING) {
        return 0;
    }
    text = json_string(value);
    if (!text) {
        return 0;
    }
    size = strlen(text);
    if ((required && size == 0u) || size >= capacity) {
        return 0;
    }
    memcpy(destination, text, size + 1u);
    return 1;
}

static int parse_u64_field(const json_value_t *object, const char *key,
                           uint64_t *out, int allow_zero) {
    json_value_t *value;
    const char *text;
    size_t size = 0u;
    uint64_t parsed = 0u;

    if (!object || !key || !out) {
        return 0;
    }
    value = json_object_get(object, key);
    text = value && json_type(value) == JSON_NUMBER
               ? json_number_text(value, &size)
               : NULL;
    if (!text || size == 0u || size > 20u || text[0] == '-') {
        return 0;
    }
    for (size_t i = 0u; i < size; ++i) {
        uint64_t digit;
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        digit = (uint64_t)(text[i] - '0');
        if (parsed > (UINT64_MAX - digit) / UINT64_C(10)) {
            return 0;
        }
        parsed = parsed * UINT64_C(10) + digit;
    }
    if (!allow_zero && parsed == 0u) {
        return 0;
    }
    *out = parsed;
    return 1;
}

static int parse_deadline(const char *text, uint64_t *out_unix_ms) {
    datetime_t value;
    time_t seconds;
    uint64_t seconds_u64;

    if (!text || !out_unix_ms || strchr(text, 'T') == NULL ||
        datetime_parse(text, strlen(text), &value) != 0 ||
        !value.has_tz || value.millisecond < 0 || value.millisecond > 999) {
        return 0;
    }
    seconds = datetime_to_time(&value);
    if (seconds < 0) {
        return 0;
    }
    seconds_u64 = (uint64_t)seconds;
    if (seconds_u64 > (UINT64_MAX - (uint64_t)value.millisecond) /
                          UINT64_C(1000)) {
        return 0;
    }
    *out_unix_ms = seconds_u64 * UINT64_C(1000) +
                   (uint64_t)value.millisecond;
    return 1;
}

static int map_command_type(const char *type,
                            ivr_media_command_kind_t *out_kind) {
    if (!type || !out_kind) {
        return 0;
    }
    if (strcmp(type, "dialog.start") == 0) {
        *out_kind = IVR_MEDIA_COMMAND_SESSION_OPEN;
    } else if (strcmp(type, "dialog.terminate") == 0) {
        *out_kind = IVR_MEDIA_COMMAND_SESSION_CLOSE;
    } else if (strcmp(type, "media.play") == 0) {
        *out_kind = IVR_MEDIA_COMMAND_PLAY;
    } else if (strcmp(type, "media.collect") == 0) {
        *out_kind = IVR_MEDIA_COMMAND_INPUT_START;
    } else if (strcmp(type, "media.cancel") == 0) {
        *out_kind = IVR_MEDIA_COMMAND_CANCEL;
    } else {
        return 0;
    }
    return 1;
}

static int key_in_set(const char *key, const char *const *allowed,
                      size_t allowed_count) {
    if (!key) {
        return 0;
    }
    for (size_t i = 0u; i < allowed_count; ++i) {
        if (strcmp(key, allowed[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int object_has_only_keys(const json_value_t *object,
                                const char *const *allowed,
                                size_t allowed_count) {
    size_t count;

    if (!object || json_type(object) != JSON_OBJECT) {
        return 0;
    }
    count = json_object_size(object);
    for (size_t i = 0u; i < count; ++i) {
        if (!key_in_set(json_object_key(object, i), allowed,
                        allowed_count)) {
            return 0;
        }
    }
    return 1;
}

static int request_keys_valid(const json_value_t *root,
                              const json_value_t *data,
                              ivr_media_command_kind_t kind) {
    static const char *const root_keys[] = {
        "schemaVersion", "commandId", "tenantId", "sessionId", "type",
        "provider", "correlationId", "causationId", "deadline", "workerId",
        "dispatchEpoch", "data"
    };
    static const char *const common_data_keys[] = {
        "capability", "dialogId", "roomId", "callId", "callGeneration",
        "operationGeneration"
    };
    const char *allowed_data[8];
    size_t data_key_count =
        sizeof(common_data_keys) / sizeof(common_data_keys[0]);

    for (size_t i = 0u; i < data_key_count; ++i) {
        allowed_data[i] = common_data_keys[i];
    }
    if (kind == IVR_MEDIA_COMMAND_PLAY) {
        allowed_data[data_key_count++] = "text";
    } else if (kind == IVR_MEDIA_COMMAND_INPUT_START ||
               kind == IVR_MEDIA_COMMAND_CANCEL) {
        allowed_data[data_key_count++] = "inputId";
        allowed_data[data_key_count++] = "inputGeneration";
    } else if (kind == IVR_MEDIA_COMMAND_SESSION_CLOSE) {
        allowed_data[data_key_count++] = "reason";
    }
    return object_has_only_keys(
               root, root_keys, sizeof(root_keys) / sizeof(root_keys[0])) &&
           object_has_only_keys(data, allowed_data, data_key_count);
}

static int parse_request(const char *idempotency_key, const char *body,
                         size_t body_size, uint64_t now_ms,
                         iris_media_request_t *out,
                         iris_media_bridge_result_t *error) {
    json_value_t *root = NULL;
    json_value_t *data;
    char schema_provider[32];
    char type[64];
    char capability[32];
    char deadline[64];
    uint64_t schema_version = 0u;
    int valid = 0;

    if (!idempotency_key || idempotency_key[0] == '\0') {
        *error = invalid_result("IDEMPOTENCY_KEY_REQUIRED",
                                "Idempotency-Key is required");
        return 0;
    }
    if (!body || body_size == 0u || body_size > IRIS_MEDIA_MAX_REQUEST_BYTES) {
        *error = invalid_result("INVALID_REQUEST_SIZE",
                                "provider command body size is invalid");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (((root = json_parse((const char *)((const uint8_t *)body), body_size)) ? 0 : -1) != 0 ||
        !root || json_type(root) != JSON_OBJECT) {
        *error = invalid_result("INVALID_JSON",
                                "provider command must be a JSON object");
        goto cleanup;
    }
    data = json_object_get(root, "data");
    if (!parse_u64_field(root, "schemaVersion", &schema_version, 0) ||
        schema_version != IRIS_MEDIA_SCHEMA_VERSION ||
        !copy_string_field(root, "commandId", out->command.message_id,
                           sizeof(out->command.message_id), 1) ||
        strcmp(idempotency_key, out->command.message_id) != 0 ||
        !copy_string_field(root, "tenantId", out->tenant_id,
                           sizeof(out->tenant_id), 1) ||
        !copy_string_field(root, "sessionId",
                           out->command.provider_session_id,
                           sizeof(out->command.provider_session_id), 1) ||
        !copy_string_field(root, "type", type, sizeof(type), 1) ||
        !map_command_type(type, &out->command.kind) ||
        !copy_string_field(root, "provider", schema_provider,
                           sizeof(schema_provider), 1) ||
        strcmp(schema_provider, IRIS_MEDIA_PROVIDER_ID) != 0 ||
        !copy_string_field(root, "correlationId", out->correlation_id,
                           sizeof(out->correlation_id), 0) ||
        !copy_string_field(root, "causationId", out->causation_id,
                           sizeof(out->causation_id), 0) ||
        !copy_string_field(root, "deadline", deadline, sizeof(deadline), 1) ||
        !parse_deadline(deadline, &out->deadline_unix_ms) ||
        !copy_string_field(root, "workerId", out->iris_worker_id,
                           sizeof(out->iris_worker_id), 1) ||
        !parse_u64_field(root, "dispatchEpoch", &out->dispatch_epoch, 0) ||
        !data || json_type(data) != JSON_OBJECT ||
        !request_keys_valid(root, data, out->command.kind) ||
        !copy_string_field(data, "capability", capability,
                           sizeof(capability), 1) ||
        strcmp(capability, IRIS_MEDIA_CAPABILITY) != 0 ||
        !copy_string_field(data, "dialogId", out->command.dialog_id,
                           sizeof(out->command.dialog_id), 1) ||
        !copy_string_field(data, "roomId", out->command.room_id,
                           sizeof(out->command.room_id), 1) ||
        !copy_string_field(data, "callId", out->command.call_id,
                           sizeof(out->command.call_id), 1) ||
        !parse_u64_field(data, "callGeneration",
                         &out->command.call_generation, 0) ||
        !parse_u64_field(data, "operationGeneration",
                         &out->command.operation_generation, 0)) {
        *error = invalid_result(
            "INVALID_COMMAND_ENVELOPE",
            "provider command envelope or IVR routing data is invalid");
        goto cleanup;
    }
    if (out->deadline_unix_ms <= now_ms) {
        *error = bridge_result(IRIS_MEDIA_BRIDGE_EXPIRED, out, NULL,
                               "COMMAND_DEADLINE_EXPIRED",
                               "provider command deadline has expired");
        goto cleanup;
    }
    memcpy(out->command.tenant_id, out->tenant_id,
           sizeof(out->command.tenant_id));
    out->command.deadline_timeout_ms = out->deadline_unix_ms - now_ms;
    if ((out->command.kind == IVR_MEDIA_COMMAND_PLAY &&
         !copy_string_field(data, "text", out->command.text,
                            sizeof(out->command.text), 1)) ||
        ((out->command.kind == IVR_MEDIA_COMMAND_INPUT_START ||
          out->command.kind == IVR_MEDIA_COMMAND_CANCEL) &&
         (!copy_string_field(data, "inputId", out->command.input_id,
                             sizeof(out->command.input_id), 1) ||
          !parse_u64_field(data, "inputGeneration",
                           &out->command.input_generation, 0))) ||
        (out->command.kind == IVR_MEDIA_COMMAND_SESSION_CLOSE &&
         !copy_string_field(data, "reason", out->command.reason,
                            sizeof(out->command.reason), 0))) {
        *error = invalid_result("INVALID_COMMAND_DATA",
                                "command-specific IVR data is invalid");
        goto cleanup;
    }
    valid = 1;

cleanup:
    json_free(root);
    root = NULL;
    return valid;
}

static int same_request(const iris_media_request_t *left,
                        const iris_media_request_t *right) {
    const ivr_media_command_t *a = &left->command;
    const ivr_media_command_t *b = &right->command;

    return a->kind == b->kind &&
           strcmp(a->message_id, b->message_id) == 0 &&
           strcmp(a->tenant_id, b->tenant_id) == 0 &&
           strcmp(a->provider_session_id, b->provider_session_id) == 0 &&
           strcmp(a->dialog_id, b->dialog_id) == 0 &&
           strcmp(a->room_id, b->room_id) == 0 &&
           strcmp(a->call_id, b->call_id) == 0 &&
           a->call_generation == b->call_generation &&
           a->operation_generation == b->operation_generation &&
           strcmp(a->text, b->text) == 0 &&
           strcmp(a->input_id, b->input_id) == 0 &&
           a->input_generation == b->input_generation &&
           strcmp(a->reason, b->reason) == 0 &&
           strcmp(left->tenant_id, right->tenant_id) == 0 &&
           strcmp(left->correlation_id, right->correlation_id) == 0 &&
           strcmp(left->causation_id, right->causation_id) == 0 &&
           left->deadline_unix_ms == right->deadline_unix_ms;
}

static int refresh_terminal_replay_fence(
    iris_media_bridge_t *bridge, const iris_media_request_t *request) {
    int valid = 1;
    salts_mutex_lock(&bridge->mutex);
    for (size_t i = 0u; i < bridge->capacity; ++i) {
        iris_media_entry_t *entry = &bridge->entries[i];
        if (entry->state == IRIS_MEDIA_ENTRY_FREE ||
            strcmp(entry->request.command.message_id,
                   request->command.message_id) != 0) {
            continue;
        }
        if (!same_request(&entry->request, request) ||
            request->dispatch_epoch < entry->request.dispatch_epoch ||
            (request->dispatch_epoch == entry->request.dispatch_epoch &&
             strcmp(request->iris_worker_id,
                    entry->request.iris_worker_id) != 0)) {
            valid = 0;
            break;
        }
        if (request->dispatch_epoch > entry->request.dispatch_epoch) {
            entry->request.dispatch_epoch = request->dispatch_epoch;
            memcpy(entry->request.iris_worker_id, request->iris_worker_id,
                   sizeof(entry->request.iris_worker_id));
        }
        break;
    }
    salts_mutex_unlock(&bridge->mutex);
    return valid;
}

static uint64_t default_realtime_ms(void *context) {
    (void)context;
    return salts_realtime_ms();
}

iris_media_bridge_t *iris_media_bridge_create(
    const iris_media_bridge_config_t *config) {
    iris_media_bridge_t *bridge;

    if (!config || config->correlation_capacity == 0u || !config->send ||
        !config->observe ||
        !config->ledger.context || !config->ledger.claim ||
        !config->ledger.commit_accepted ||
        !config->ledger.commit_terminal || !config->ledger.mark_unknown ||
        !config->ledger.resource_seen ||
        !config->ledger.abort_intent ||
        config->correlation_capacity > SIZE_MAX / sizeof(iris_media_entry_t)) {
        return NULL;
    }
    bridge = (iris_media_bridge_t *)calloc(1, sizeof(*bridge));
    if (!bridge) {
        return NULL;
    }
    bridge->entries = (iris_media_entry_t *)calloc(
        config->correlation_capacity, sizeof(*bridge->entries));
    if (!bridge->entries) {
        free(bridge);
        return NULL;
    }
    bridge->capacity = config->correlation_capacity;
    bridge->send = config->send;
    bridge->send_context = config->send_context;
    bridge->observe = config->observe;
    bridge->observe_context = config->observe_context;
    bridge->realtime_ms = config->realtime_ms
                              ? config->realtime_ms
                              : default_realtime_ms;
    bridge->realtime_context = config->realtime_context;
    bridge->ledger = config->ledger;
    salts_mutex_init(&bridge->mutex);
    salts_cond_init(&bridge->state_changed);
    return bridge;
}

void iris_media_bridge_destroy(iris_media_bridge_t *bridge) {
    if (!bridge) {
        return;
    }
    salts_cond_destroy(&bridge->state_changed);
    salts_mutex_destroy(&bridge->mutex);
    free(bridge->entries);
    free(bridge);
}

iris_media_bridge_result_t iris_media_bridge_dispatch_json(
    iris_media_bridge_t *bridge, const char *idempotency_key,
    const char *body, size_t body_size) {
    iris_media_request_t request;
    iris_media_bridge_result_t parse_error;
    iris_command_identity_t identity;
    iris_command_claim_result_t claim;
    iris_media_entry_t *slot = NULL;
    iris_media_entry_t *oldest_completed = NULL;
    char media_worker_id[IRIS_MEDIA_ENVELOPE_ID_CAPACITY] = {0};
    uint64_t now_ms;
    ivr_status_t ledger_status;
    ivr_status_t send_status;
    int execute;

    if (!bridge) {
        return bridge_result(IRIS_MEDIA_BRIDGE_INTERNAL, NULL, NULL,
                             "BRIDGE_NOT_INITIALIZED",
                             "Iris media bridge is not initialized");
    }
    now_ms = bridge->realtime_ms(bridge->realtime_context);
    if (now_ms == 0u ||
        !parse_request(idempotency_key, body, body_size, now_ms, &request,
                       &parse_error)) {
        return now_ms == 0u
                   ? bridge_result(IRIS_MEDIA_BRIDGE_INTERNAL, NULL, NULL,
                                   "CLOCK_UNAVAILABLE",
                                   "realtime clock is unavailable")
                   : parse_error;
    }
    if (!command_identity(&request, &identity)) {
        return bridge_result(IRIS_MEDIA_BRIDGE_INTERNAL, &request, NULL,
                             "COMMAND_FINGERPRINT_FAILED",
                             "provider media command fingerprint failed");
    }

    memset(&claim, 0, sizeof(claim));
    ledger_status = bridge->ledger.claim(bridge->ledger.context, &identity,
                                         &claim);
    if (ledger_status != IVR_OK) {
        return bridge_result(
            ledger_status == IVR_ENOSPC ? IRIS_MEDIA_BRIDGE_FULL
                                        : IRIS_MEDIA_BRIDGE_UNAVAILABLE,
            &request, NULL,
            ledger_status == IVR_ENOSPC ? "COMMAND_LEDGER_FULL"
                                        : "COMMAND_LEDGER_UNAVAILABLE",
            ledger_status == IVR_ENOSPC
                ? "provider command ledger queue is full"
                : "provider command ledger is unavailable");
    }
    if (claim.disposition == IRIS_COMMAND_CLAIM_CONFLICT) {
        return bridge_result(IRIS_MEDIA_BRIDGE_CONFLICT, &request, NULL,
                             "IDEMPOTENCY_CONFLICT",
                             "commandId was reused with different media data");
    }
    if (claim.disposition == IRIS_COMMAND_CLAIM_IN_PROGRESS) {
        return bridge_result(IRIS_MEDIA_BRIDGE_UNAVAILABLE, &request, NULL,
                             "COMMAND_IN_PROGRESS",
                             "provider media command is still executing");
    }
    if (claim.disposition == IRIS_COMMAND_CLAIM_OUTCOME_UNKNOWN) {
        iris_resource_observation_t observation;

        memset(&observation, 0, sizeof(observation));
        observation.state = IRIS_RESOURCE_OBSERVATION_UNKNOWN;
        if (bridge->observe(bridge->observe_context, &request.command,
                            &observation) == IVR_OK &&
            observation.state == IRIS_RESOURCE_OBSERVATION_ACTIVE &&
            observation.provider_resource_id[0] != '\0' &&
            bridge->ledger.commit_accepted(
                bridge->ledger.context, &identity,
                observation.provider_resource_id) == IVR_OK) {
            claim.disposition = IRIS_COMMAND_CLAIM_REPLAY_ACCEPTED;
            memcpy(claim.provider_resource_id,
                   observation.provider_resource_id,
                   sizeof(claim.provider_resource_id));
        } else if (observation.state == IRIS_RESOURCE_OBSERVATION_ABSENT &&
                   (request.command.kind ==
                        IVR_MEDIA_COMMAND_SESSION_CLOSE ||
                    request.command.kind == IVR_MEDIA_COMMAND_CANCEL)) {
            int seen = 0;
            iris_command_terminal_outcome_t absence;
            if (bridge->ledger.resource_seen(
                    bridge->ledger.context, &identity, &seen) == IVR_OK &&
                seen && absence_terminal_outcome(&request, 1, &absence) &&
                bridge->ledger.commit_terminal(
                    bridge->ledger.context, &identity, &absence) == IVR_OK) {
                return terminal_outcome_result(IRIS_MEDIA_BRIDGE_TERMINAL,
                                               &request, &absence);
            }
            return bridge_result(
                IRIS_MEDIA_BRIDGE_UNAVAILABLE, &request, NULL,
                "PROVIDER_OUTCOME_UNKNOWN",
                "observed media absence could not be verified durably");
        } else {
            return bridge_result(
                IRIS_MEDIA_BRIDGE_UNAVAILABLE, &request, NULL,
                "PROVIDER_OUTCOME_UNKNOWN",
                "provider media outcome requires query or reconciliation");
        }
    }
    if (claim.disposition == IRIS_COMMAND_CLAIM_REPLAY_TERMINAL) {
        if (!refresh_terminal_replay_fence(bridge, &request)) {
            return bridge_result(
                IRIS_MEDIA_BRIDGE_CONFLICT, &request, NULL,
                "COMMAND_FENCE_CONFLICT",
                "commandId was reused with different data or a stale fence");
        }
        return terminal_replay_result(&request, &claim);
    }
    execute = claim.disposition == IRIS_COMMAND_CLAIM_EXECUTE;
    if (!execute &&
        claim.disposition != IRIS_COMMAND_CLAIM_REPLAY_ACCEPTED) {
        return bridge_result(IRIS_MEDIA_BRIDGE_INTERNAL, &request, NULL,
                             "COMMAND_LEDGER_STATE_INVALID",
                             "provider command ledger state is invalid");
    }
    if (!execute && claim.provider_resource_id[0] == '\0') {
        return bridge_result(IRIS_MEDIA_BRIDGE_INTERNAL, &request, NULL,
                             "COMMAND_LEDGER_RECORD_INVALID",
                             "accepted provider command has no media worker");
    }

    salts_mutex_lock(&bridge->mutex);
    for (size_t i = 0u; i < bridge->capacity; ++i) {
        iris_media_entry_t *entry = &bridge->entries[i];
        if (entry->state == IRIS_MEDIA_ENTRY_FREE) {
            if (!slot) {
                slot = entry;
            }
            continue;
        }
        if (entry->state == IRIS_MEDIA_ENTRY_COMPLETED &&
            (!oldest_completed || entry->completed_sequence <
                                      oldest_completed->completed_sequence)) {
            oldest_completed = entry;
        }
        if (strcmp(entry->request.command.message_id,
                   request.command.message_id) != 0) {
            continue;
        }
        if (!same_request(&entry->request, &request) ||
            request.dispatch_epoch < entry->request.dispatch_epoch ||
            (request.dispatch_epoch == entry->request.dispatch_epoch &&
             strcmp(request.iris_worker_id,
                    entry->request.iris_worker_id) != 0)) {
            salts_mutex_unlock(&bridge->mutex);
            return bridge_result(IRIS_MEDIA_BRIDGE_CONFLICT, &request, NULL,
                                 "COMMAND_FENCE_CONFLICT",
                                 "commandId was reused with different data or a stale fence");
        }
        if (execute) {
            salts_mutex_unlock(&bridge->mutex);
            (void)bridge->ledger.abort_intent(bridge->ledger.context,
                                              &identity);
            return bridge_result(
                IRIS_MEDIA_BRIDGE_CONFLICT, &request, NULL,
                "COMMAND_RETENTION_CONFLICT",
                "commandId is still retained by the media correlation cache");
        }
        if (request.dispatch_epoch > entry->request.dispatch_epoch) {
            entry->request.dispatch_epoch = request.dispatch_epoch;
            memcpy(entry->request.iris_worker_id, request.iris_worker_id,
                   sizeof(entry->request.iris_worker_id));
        }
        request.dispatch_epoch = entry->request.dispatch_epoch;
        memcpy(request.iris_worker_id, entry->request.iris_worker_id,
               sizeof(request.iris_worker_id));
        memcpy(entry->media_worker_id, claim.provider_resource_id,
               sizeof(entry->media_worker_id));
        entry->request = request;
        entry->identity = identity;
        entry->state = IRIS_MEDIA_ENTRY_ACCEPTED;
        memcpy(media_worker_id, claim.provider_resource_id,
               sizeof(media_worker_id));
        salts_cond_broadcast(&bridge->state_changed);
        salts_mutex_unlock(&bridge->mutex);
        return bridge_result(IRIS_MEDIA_BRIDGE_DUPLICATE, &request,
                             media_worker_id, NULL, NULL);
    }
    if (!slot) slot = oldest_completed;
    if (!slot) {
        salts_mutex_unlock(&bridge->mutex);
        if (execute) {
            (void)bridge->ledger.abort_intent(bridge->ledger.context,
                                              &identity);
        }
        return bridge_result(IRIS_MEDIA_BRIDGE_FULL, &request, NULL,
                             "CORRELATION_CAPACITY_EXHAUSTED",
                             "Iris media correlation capacity is exhausted");
    }
    memset(slot, 0, sizeof(*slot));
    slot->request = request;
    slot->identity = identity;
    if (!execute) {
        memcpy(slot->media_worker_id, claim.provider_resource_id,
               sizeof(slot->media_worker_id));
        slot->state = IRIS_MEDIA_ENTRY_ACCEPTED;
        memcpy(media_worker_id, claim.provider_resource_id,
               sizeof(media_worker_id));
        salts_cond_broadcast(&bridge->state_changed);
        salts_mutex_unlock(&bridge->mutex);
        return bridge_result(IRIS_MEDIA_BRIDGE_DUPLICATE, &request,
                             media_worker_id, NULL, NULL);
    }
    slot->state = IRIS_MEDIA_ENTRY_DISPATCHING;
    salts_mutex_unlock(&bridge->mutex);

    send_status = bridge->send(bridge->send_context, &request.command,
                               media_worker_id, sizeof(media_worker_id));

    salts_mutex_lock(&bridge->mutex);
    if (slot->state != IRIS_MEDIA_ENTRY_DISPATCHING ||
        strcmp(slot->request.command.message_id,
               request.command.message_id) != 0) {
        salts_mutex_unlock(&bridge->mutex);
        return bridge_result(IRIS_MEDIA_BRIDGE_INTERNAL, &request, NULL,
                             "CORRELATION_STATE_INVALID",
                             "media correlation state changed unexpectedly");
    }
    if (send_status != IVR_OK) {
        memset(slot, 0, sizeof(*slot));
        salts_cond_broadcast(&bridge->state_changed);
        salts_mutex_unlock(&bridge->mutex);
        if (send_status == IVR_ENOTFOUND &&
            (request.command.kind == IVR_MEDIA_COMMAND_SESSION_CLOSE ||
             request.command.kind == IVR_MEDIA_COMMAND_CANCEL)) {
            int seen = 0;
            iris_command_terminal_outcome_t absence;
            ledger_status = bridge->ledger.resource_seen(
                bridge->ledger.context, &identity, &seen);
            if (ledger_status != IVR_OK ||
                !absence_terminal_outcome(&request, seen, &absence) ||
                bridge->ledger.commit_terminal(
                    bridge->ledger.context, &identity, &absence) != IVR_OK) {
                (void)bridge->ledger.mark_unknown(
                    bridge->ledger.context, &identity);
                return bridge_result(
                    IRIS_MEDIA_BRIDGE_UNAVAILABLE, &request, NULL,
                    "PROVIDER_OUTCOME_UNKNOWN",
                    "media resource absence could not be committed durably");
            }
            return terminal_outcome_result(IRIS_MEDIA_BRIDGE_TERMINAL,
                                           &request, &absence);
        }
        (void)bridge->ledger.abort_intent(bridge->ledger.context, &identity);
        return bridge_result(
            send_status == IVR_ENOSPC ? IRIS_MEDIA_BRIDGE_FULL
                                      : IRIS_MEDIA_BRIDGE_UNAVAILABLE,
            &request, NULL,
            send_status == IVR_ENOSPC ? "MEDIA_QUEUE_FULL"
                                      : "MEDIA_ROUTE_UNAVAILABLE",
            send_status == IVR_ENOSPC
                ? "media command queue capacity is exhausted"
                : "no owning IVR media route is available");
    }
    if (media_worker_id[0] == '\0') {
        memset(slot, 0, sizeof(*slot));
        salts_cond_broadcast(&bridge->state_changed);
        salts_mutex_unlock(&bridge->mutex);
        (void)bridge->ledger.mark_unknown(bridge->ledger.context, &identity);
        return bridge_result(
            IRIS_MEDIA_BRIDGE_UNAVAILABLE, &request, NULL,
            "PROVIDER_OUTCOME_UNKNOWN",
            "media command was sent without a durable worker identity");
    }
    memcpy(slot->media_worker_id, media_worker_id,
           sizeof(slot->media_worker_id));
    slot->state = IRIS_MEDIA_ENTRY_ACCEPTING;
    salts_mutex_unlock(&bridge->mutex);

    ledger_status = bridge->ledger.commit_accepted(
        bridge->ledger.context, &identity, media_worker_id);
    if (ledger_status != IVR_OK) {
        (void)bridge->ledger.mark_unknown(bridge->ledger.context, &identity);
        salts_mutex_lock(&bridge->mutex);
        if (slot->state == IRIS_MEDIA_ENTRY_ACCEPTING &&
            strcmp(slot->request.command.message_id,
                   request.command.message_id) == 0) {
            memset(slot, 0, sizeof(*slot));
        }
        salts_cond_broadcast(&bridge->state_changed);
        salts_mutex_unlock(&bridge->mutex);
        return bridge_result(
            IRIS_MEDIA_BRIDGE_UNAVAILABLE, &request, NULL,
            "PROVIDER_OUTCOME_UNKNOWN",
            "media command was sent but durable acceptance failed");
    }

    salts_mutex_lock(&bridge->mutex);
    if (slot->state != IRIS_MEDIA_ENTRY_ACCEPTING ||
        strcmp(slot->request.command.message_id,
               request.command.message_id) != 0) {
        salts_cond_broadcast(&bridge->state_changed);
        salts_mutex_unlock(&bridge->mutex);
        (void)bridge->ledger.mark_unknown(bridge->ledger.context, &identity);
        return bridge_result(IRIS_MEDIA_BRIDGE_INTERNAL, &request, NULL,
                             "CORRELATION_STATE_INVALID",
                             "media acceptance correlation was lost");
    }
    slot->state = IRIS_MEDIA_ENTRY_ACCEPTED;
    salts_cond_broadcast(&bridge->state_changed);
    salts_mutex_unlock(&bridge->mutex);
    return bridge_result(IRIS_MEDIA_BRIDGE_ACCEPTED, &request,
                         media_worker_id, NULL, NULL);
}

static void copy_completion(const iris_media_entry_t *entry,
                            iris_media_completion_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->command_id, entry->request.command.message_id,
           sizeof(out->command_id));
    memcpy(out->tenant_id, entry->request.tenant_id,
           sizeof(out->tenant_id));
    memcpy(out->provider_session_id,
           entry->request.command.provider_session_id,
           sizeof(out->provider_session_id));
    memcpy(out->iris_worker_id, entry->request.iris_worker_id,
           sizeof(out->iris_worker_id));
    memcpy(out->correlation_id, entry->request.correlation_id,
           sizeof(out->correlation_id));
    out->dispatch_epoch = entry->request.dispatch_epoch;
}

ivr_status_t iris_media_bridge_claim_completion(
    iris_media_bridge_t *bridge, const ivr_media_command_result_t *result,
    iris_media_completion_t *out) {
    ivr_status_t status = IVR_ESTATE;
    iris_media_entry_t *claimed = NULL;
    iris_command_identity_t identity;
    iris_command_terminal_outcome_t terminal;
    iris_media_completion_t completion;

    if (!bridge || !result || !out || result->message_id[0] == '\0') {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    memset(&identity, 0, sizeof(identity));
    memset(&completion, 0, sizeof(completion));
    salts_mutex_lock(&bridge->mutex);
retry_locked:
    for (size_t i = 0u; i < bridge->capacity; ++i) {
        iris_media_entry_t *entry = &bridge->entries[i];
        const ivr_media_command_t *command = &entry->request.command;
        if (entry->state == IRIS_MEDIA_ENTRY_FREE ||
            strcmp(command->message_id, result->message_id) != 0) {
            continue;
        }
        if (entry->state == IRIS_MEDIA_ENTRY_DISPATCHING ||
            entry->state == IRIS_MEDIA_ENTRY_ACCEPTING) {
            salts_cond_wait(&bridge->state_changed, &bridge->mutex);
            goto retry_locked;
        }
        if (entry->state == IRIS_MEDIA_ENTRY_TERMINAL_COMMITTING ||
            entry->state == IRIS_MEDIA_ENTRY_COMPLETING ||
            entry->state == IRIS_MEDIA_ENTRY_COMPLETED) {
            status = IVR_OK;
            break;
        }
        if (entry->state != IRIS_MEDIA_ENTRY_ACCEPTED ||
            strcmp(command->tenant_id, result->tenant_id) != 0 ||
            strcmp(command->provider_session_id,
                   result->provider_session_id) != 0 ||
            strcmp(command->dialog_id, result->dialog_id) != 0 ||
            strcmp(entry->media_worker_id, result->worker_id) != 0 ||
            strcmp(command->room_id, result->room_id) != 0 ||
            strcmp(command->call_id, result->call_id) != 0 ||
            command->call_generation != result->call_generation ||
            command->operation_generation != result->operation_generation) {
            status = IVR_ESTATE;
            break;
        }
        entry->state = IRIS_MEDIA_ENTRY_TERMINAL_COMMITTING;
        identity = entry->identity;
        copy_completion(entry, &completion);
        claimed = entry;
        status = IVR_OK;
        break;
    }
    salts_mutex_unlock(&bridge->mutex);
    if (status != IVR_OK || !claimed) return status;

    if (!terminal_outcome_from_result(result, &terminal)) {
        salts_mutex_lock(&bridge->mutex);
        if (claimed->state == IRIS_MEDIA_ENTRY_TERMINAL_COMMITTING &&
            strcmp(claimed->request.command.message_id,
                   result->message_id) == 0) {
            claimed->state = IRIS_MEDIA_ENTRY_ACCEPTED;
        }
        salts_cond_broadcast(&bridge->state_changed);
        salts_mutex_unlock(&bridge->mutex);
        return IVR_ENOSPC;
    }
    status = bridge->ledger.commit_terminal(
        bridge->ledger.context, &identity, &terminal);
    if (status != IVR_OK) {
        (void)bridge->ledger.mark_unknown(bridge->ledger.context, &identity);
        salts_mutex_lock(&bridge->mutex);
        if (claimed->state == IRIS_MEDIA_ENTRY_TERMINAL_COMMITTING &&
            strcmp(claimed->request.command.message_id,
                   result->message_id) == 0) {
            claimed->state = IRIS_MEDIA_ENTRY_ACCEPTED;
        }
        salts_cond_broadcast(&bridge->state_changed);
        salts_mutex_unlock(&bridge->mutex);
        return status;
    }

    salts_mutex_lock(&bridge->mutex);
    if (claimed->state != IRIS_MEDIA_ENTRY_TERMINAL_COMMITTING ||
        strcmp(claimed->request.command.message_id,
               result->message_id) != 0) {
        salts_cond_broadcast(&bridge->state_changed);
        salts_mutex_unlock(&bridge->mutex);
        return IVR_ESTATE;
    }
    claimed->state = IRIS_MEDIA_ENTRY_COMPLETING;
    *out = completion;
    salts_cond_broadcast(&bridge->state_changed);
    salts_mutex_unlock(&bridge->mutex);
    return IVR_OK;
}

ivr_status_t iris_media_bridge_refresh_completion(
    iris_media_bridge_t *bridge, const char *command_id,
    iris_media_completion_t *out) {
    ivr_status_t status = IVR_ESTATE;
    if (!bridge || !command_id || !command_id[0] || !out) {
        return IVR_EINVAL;
    }
    salts_mutex_lock(&bridge->mutex);
    for (size_t i = 0u; i < bridge->capacity; ++i) {
        iris_media_entry_t *entry = &bridge->entries[i];
        if (entry->state == IRIS_MEDIA_ENTRY_COMPLETING &&
            strcmp(entry->request.command.message_id, command_id) == 0) {
            copy_completion(entry, out);
            status = IVR_OK;
            break;
        }
    }
    salts_mutex_unlock(&bridge->mutex);
    return status;
}

void iris_media_bridge_restore_completion(iris_media_bridge_t *bridge,
                                          const char *command_id) {
    if (!bridge || !command_id) return;
    salts_mutex_lock(&bridge->mutex);
    for (size_t i = 0u; i < bridge->capacity; ++i) {
        iris_media_entry_t *entry = &bridge->entries[i];
        if (entry->state == IRIS_MEDIA_ENTRY_COMPLETING &&
            strcmp(entry->request.command.message_id, command_id) == 0) {
            entry->state = IRIS_MEDIA_ENTRY_ACCEPTED;
            break;
        }
    }
    salts_mutex_unlock(&bridge->mutex);
}

void iris_media_bridge_release_completion(iris_media_bridge_t *bridge,
                                          const char *command_id) {
    if (!bridge || !command_id) return;
    salts_mutex_lock(&bridge->mutex);
    for (size_t i = 0u; i < bridge->capacity; ++i) {
        iris_media_entry_t *entry = &bridge->entries[i];
        if (entry->state == IRIS_MEDIA_ENTRY_COMPLETING &&
            strcmp(entry->request.command.message_id, command_id) == 0) {
            entry->state = IRIS_MEDIA_ENTRY_COMPLETED;
            entry->completed_sequence = ++bridge->next_completed_sequence;
            if (bridge->next_completed_sequence == 0u) {
                bridge->next_completed_sequence = 1u;
                entry->completed_sequence = 1u;
            }
            break;
        }
    }
    salts_mutex_unlock(&bridge->mutex);
}
