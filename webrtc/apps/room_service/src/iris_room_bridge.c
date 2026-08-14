#include "iris_room_bridge.h"
#include "iris_command_fingerprint.h"

#include <platform.h>
#include <turbo_parser.h>
#include <turbo_thread.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IRIS_ROOM_PROVIDER_ID "turbomedia"
#define IRIS_ROOM_CAPABILITY "room"
#define IRIS_ROOM_SCHEMA_VERSION UINT64_C(3)
#define IRIS_ROOM_MAX_REQUEST_BYTES 16384u
#define IRIS_ROOM_INTERNAL_EVENT_TYPE "provider.command.failed"
#define IRIS_ROOM_INTERNAL_DATA                                              \
    "{\"code\":\"ROOM_EXECUTION_FAILED\",\"message\":"               \
    "\"provider room command failed internally\"}"

typedef enum iris_room_entry_state_e {
    IRIS_ROOM_ENTRY_FREE = 0,
    IRIS_ROOM_ENTRY_EXECUTING,
    IRIS_ROOM_ENTRY_COMPLETED
} iris_room_entry_state_t;

typedef struct iris_room_request_s {
    iris_room_command_t command;
    uint64_t deadline_unix_ms;
} iris_room_request_t;

typedef struct iris_room_entry_s {
    iris_room_entry_state_t state;
    iris_room_request_t request;
    iris_room_execution_t execution;
    uint64_t completed_sequence;
} iris_room_entry_t;

struct iris_room_bridge_s {
    iris_room_entry_t *entries;
    size_t capacity;
    turbo_mutex_t mutex;
    iris_room_bridge_execute_fn execute;
    void *execute_context;
    iris_room_bridge_observe_fn observe;
    void *observe_context;
    iris_room_bridge_realtime_ms_fn realtime_ms;
    void *realtime_context;
    uint64_t next_completed_sequence;
    iris_command_ledger_port_t ledger;
};

static const char *command_type_name(iris_room_command_kind_t kind) {
    switch (kind) {
        case IRIS_ROOM_COMMAND_CREATE: return "conference.create";
        case IRIS_ROOM_COMMAND_DESTROY: return "conference.destroy";
        case IRIS_ROOM_COMMAND_JOIN: return "connection.join";
        case IRIS_ROOM_COMMAND_UNJOIN: return "connection.unjoin";
        default: return "";
    }
}

static int copy_fixed(char *destination, size_t capacity,
                      const char *source, int required) {
    size_t size;
    if (!destination || capacity == 0u || !source) return 0;
    size = strlen(source);
    if ((required && size == 0u) || size >= capacity) return 0;
    memcpy(destination, source, size + 1u);
    return 1;
}

static int command_identity(const iris_room_request_t *request,
                            iris_command_identity_t *identity) {
    iris_command_fingerprint_t fingerprint;
    const char *type = command_type_name(request->command.kind);
    memset(identity, 0, sizeof(*identity));
    if (!type[0] ||
        !copy_fixed(identity->command_id, sizeof(identity->command_id),
                    request->command.command_id, 1) ||
        !copy_fixed(identity->provider_session_id,
                    sizeof(identity->provider_session_id),
                    request->command.provider_session_id, 1) ||
        !copy_fixed(identity->command_type, sizeof(identity->command_type),
                    type, 1) ||
        !iris_command_fingerprint_init(&fingerprint)) {
        return 0;
    }
    if (request->command.kind == IRIS_ROOM_COMMAND_CREATE ||
        request->command.kind == IRIS_ROOM_COMMAND_DESTROY) {
        if (!copy_fixed(identity->resource_id,
                        sizeof(identity->resource_id),
                        request->command.room_id, 1)) {
            return 0;
        }
        identity->resource_generation = request->command.room_generation;
    } else {
        if (!copy_fixed(identity->resource_scope_id,
                        sizeof(identity->resource_scope_id),
                        request->command.room_id, 1) ||
            !copy_fixed(identity->resource_id,
                        sizeof(identity->resource_id),
                        request->command.call_id, 1)) {
            return 0;
        }
        identity->resource_scope_generation =
            request->command.room_generation;
        identity->resource_generation = request->command.call_generation;
    }
    return iris_command_fingerprint_add_u64(
               &fingerprint, IRIS_ROOM_SCHEMA_VERSION) &&
           iris_command_fingerprint_add_u64(
               &fingerprint, (uint64_t)request->command.kind) &&
           iris_command_fingerprint_add_u64(
               &fingerprint, request->deadline_unix_ms) &&
           iris_command_fingerprint_add_text(
               &fingerprint, IRIS_ROOM_PROVIDER_ID) &&
           iris_command_fingerprint_add_text(
               &fingerprint, IRIS_ROOM_CAPABILITY) &&
           iris_command_fingerprint_add_text(
               &fingerprint, request->command.tenant_id) &&
           iris_command_fingerprint_add_text(
               &fingerprint, request->command.provider_session_id) &&
           iris_command_fingerprint_add_text(&fingerprint, type) &&
           iris_command_fingerprint_add_text(
               &fingerprint, request->command.correlation_id) &&
           iris_command_fingerprint_add_text(
               &fingerprint, request->command.causation_id) &&
           iris_command_fingerprint_add_text(
               &fingerprint, request->command.room_id) &&
           iris_command_fingerprint_add_u64(
               &fingerprint, request->command.room_generation) &&
           iris_command_fingerprint_add_text(
               &fingerprint, request->command.call_id) &&
           iris_command_fingerprint_add_u64(
               &fingerprint, request->command.call_generation) &&
           iris_command_fingerprint_add_text(&fingerprint,
                                             request->command.role) &&
           iris_command_fingerprint_add_text(&fingerprint,
                                             request->command.user_id) &&
           iris_command_fingerprint_add_text(
               &fingerprint, request->command.display_name) &&
           iris_command_fingerprint_final(
               &fingerprint, identity->semantic_fingerprint);
}

static uint64_t system_realtime_ms(void *context) {
    time_t seconds;
    (void)context;
    seconds = time(NULL);
    return seconds < 0 ? 0u : (uint64_t)seconds * UINT64_C(1000);
}

static iris_room_bridge_result_t make_result(
    iris_room_bridge_status_t status, const char *command_id,
    const iris_room_execution_t *execution, const char *code,
    const char *message) {
    iris_room_bridge_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = status;
    result.error_code = code;
    result.error_message = message;
    if (command_id) {
        memcpy(result.command_id, command_id, strlen(command_id) + 1u);
    }
    if (execution) {
        result.terminal_status = execution->terminal_status;
        memcpy(result.event_type, execution->event_type,
               sizeof(result.event_type));
        memcpy(result.data, execution->data, sizeof(result.data));
    }
    return result;
}

static iris_room_bridge_result_t invalid_result(const char *code,
                                                 const char *message) {
    return make_result(IRIS_ROOM_BRIDGE_INVALID, NULL, NULL, code, message);
}

static iris_room_execution_t internal_failure_execution(void) {
    iris_room_execution_t execution;
    memset(&execution, 0, sizeof(execution));
    execution.terminal_status = IRIS_ROOM_TERMINAL_FAILED;
    memcpy(execution.event_type, IRIS_ROOM_INTERNAL_EVENT_TYPE,
           sizeof(IRIS_ROOM_INTERNAL_EVENT_TYPE));
    memcpy(execution.data, IRIS_ROOM_INTERNAL_DATA,
           sizeof(IRIS_ROOM_INTERNAL_DATA));
    return execution;
}

static int add_owned_json(json_value_t *object, const char *name,
                          json_value_t *value) {
    if (!value || !turbo_json_object_add_checked(object, name, value)) {
        turbo_free_json(&value);
        return 0;
    }
    return 1;
}

static int make_already_absent_execution(
    const iris_room_command_t *command, iris_room_execution_t *execution) {
    json_value_t *data = turbo_json_create_object();
    char *json = NULL;
    size_t json_size = 0u;
    const char *event_type;
    int valid;
    if (!command || !execution || !data) {
        turbo_free_json(&data);
        return 0;
    }
    event_type = command->kind == IRIS_ROOM_COMMAND_DESTROY
                     ? "provider.conference.destroyed"
                     : "provider.connection.unjoined";
    valid = add_owned_json(
                data, "roomId",
                turbo_json_create_string(command->room_id)) &&
            add_owned_json(
                data, "roomGeneration",
                turbo_json_create_uint64(command->room_generation)) &&
            (command->kind == IRIS_ROOM_COMMAND_DESTROY ||
             (add_owned_json(
                  data, "callId",
                  turbo_json_create_string(command->call_id)) &&
              add_owned_json(
                  data, "callGeneration",
                  turbo_json_create_uint64(command->call_generation)))) &&
            add_owned_json(data, "alreadyAbsent",
                           turbo_json_create_bool(1));
    if (valid) json = turbo_json_serialize(data, &json_size);
    turbo_free_json(&data);
    if (!json || json_size == 0u || json_size >= sizeof(execution->data) ||
        strlen(event_type) >= sizeof(execution->event_type)) {
        turbo_json_serialize_free(json);
        return 0;
    }
    memset(execution, 0, sizeof(*execution));
    execution->terminal_status = IRIS_ROOM_TERMINAL_SUCCEEDED;
    memcpy(execution->event_type, event_type, strlen(event_type) + 1u);
    memcpy(execution->data, json, json_size);
    execution->data[json_size] = '\0';
    turbo_json_serialize_free(json);
    return 1;
}

static int copy_string_field(const json_value_t *object, const char *key,
                             char *destination, size_t capacity,
                             int required) {
    json_value_t *value;
    const char *text;
    size_t size;
    if (!object || !key || !destination || capacity == 0u) return 0;
    value = turbo_json_object_get(object, key);
    if (!value) {
        destination[0] = '\0';
        return !required;
    }
    if (turbo_json_type(value) != TURBO_JSON_STRING) return 0;
    text = turbo_json_string(value);
    if (!text) return 0;
    size = strlen(text);
    if ((required && size == 0u) || size >= capacity) return 0;
    memcpy(destination, text, size + 1u);
    return 1;
}

static int parse_u64_field(const json_value_t *object, const char *key,
                           uint64_t *out, int allow_zero) {
    json_value_t *value;
    const char *text;
    size_t size = 0u;
    uint64_t parsed = 0u;
    if (!object || !key || !out) return 0;
    value = turbo_json_object_get(object, key);
    text = value && turbo_json_type(value) == TURBO_JSON_NUMBER
               ? turbo_json_number_text(value, &size)
               : NULL;
    if (!text || size == 0u || size > 20u || text[0] == '-') return 0;
    for (size_t i = 0u; i < size; ++i) {
        uint64_t digit;
        if (text[i] < '0' || text[i] > '9') return 0;
        digit = (uint64_t)(text[i] - '0');
        if (parsed > (UINT64_MAX - digit) / UINT64_C(10)) return 0;
        parsed = parsed * UINT64_C(10) + digit;
    }
    if (!allow_zero && parsed == 0u) return 0;
    *out = parsed;
    return 1;
}

static int parse_deadline(const char *text, uint64_t *out_unix_ms) {
    turbo_datetime_t value;
    time_t seconds;
    uint64_t seconds_u64;
    if (!text || !out_unix_ms || strchr(text, 'T') == NULL ||
        turbo_parse_datetime(text, strlen(text), &value) != 0 ||
        !value.has_tz || value.millisecond < 0 || value.millisecond > 999) {
        return 0;
    }
    seconds = turbo_datetime_to_time(&value);
    if (seconds < 0) return 0;
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
                            iris_room_command_kind_t *kind) {
    if (!type || !kind) return 0;
    if (strcmp(type, "conference.create") == 0) {
        *kind = IRIS_ROOM_COMMAND_CREATE;
    } else if (strcmp(type, "conference.destroy") == 0) {
        *kind = IRIS_ROOM_COMMAND_DESTROY;
    } else if (strcmp(type, "connection.join") == 0) {
        *kind = IRIS_ROOM_COMMAND_JOIN;
    } else if (strcmp(type, "connection.unjoin") == 0) {
        *kind = IRIS_ROOM_COMMAND_UNJOIN;
    } else {
        return 0;
    }
    return 1;
}

static int key_in_set(const char *key, const char *const *allowed,
                      size_t allowed_count) {
    if (!key) return 0;
    for (size_t i = 0u; i < allowed_count; ++i) {
        if (strcmp(key, allowed[i]) == 0) return 1;
    }
    return 0;
}

static int object_has_only_keys(const json_value_t *object,
                                const char *const *allowed,
                                size_t allowed_count) {
    size_t count;
    if (!object || turbo_json_type(object) != TURBO_JSON_OBJECT) return 0;
    count = turbo_json_object_size(object);
    for (size_t i = 0u; i < count; ++i) {
        if (!key_in_set(turbo_json_object_key(object, i), allowed,
                        allowed_count)) return 0;
    }
    return 1;
}

static int request_keys_valid(const json_value_t *root,
                              const json_value_t *data) {
    static const char *const root_keys[] = {
        "schemaVersion", "commandId", "tenantId", "sessionId", "type",
        "provider", "correlationId", "causationId", "deadline", "workerId",
        "dispatchEpoch", "data"
    };
    static const char *const data_keys[] = {
        "capability", "roomId", "roomGeneration", "callId",
        "callGeneration", "role", "userId", "displayName", "roomType",
        "createdBy", "confname", "region", "reason", "duplex"
    };
    return object_has_only_keys(
               root, root_keys, sizeof(root_keys) / sizeof(root_keys[0])) &&
           object_has_only_keys(
               data, data_keys, sizeof(data_keys) / sizeof(data_keys[0]));
}

static int parse_request(const char *idempotency_key, const char *body,
                         size_t body_size, iris_room_request_t *out,
                         iris_room_bridge_result_t *error) {
    json_value_t *root = NULL;
    json_value_t *data;
    char provider[32];
    char capability[32];
    char type[64];
    char deadline[64];
    char ignored_worker[128];
    uint64_t schema_version = 0u;
    uint64_t ignored_epoch = 0u;
    int valid = 0;
    if (!idempotency_key || idempotency_key[0] == '\0') {
        *error = invalid_result("IDEMPOTENCY_KEY_REQUIRED",
                                "Idempotency-Key is required");
        return 0;
    }
    if (!body || body_size == 0u || body_size > IRIS_ROOM_MAX_REQUEST_BYTES) {
        *error = invalid_result("INVALID_REQUEST_SIZE",
                                "provider command body size is invalid");
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (turbo_parse_json((const uint8_t *)body, body_size, &root) != 0 ||
        !root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
        *error = invalid_result("INVALID_JSON",
                                "provider command must be a JSON object");
        goto cleanup;
    }
    data = turbo_json_object_get(root, "data");
    if (!parse_u64_field(root, "schemaVersion", &schema_version, 0) ||
        schema_version != IRIS_ROOM_SCHEMA_VERSION ||
        !copy_string_field(root, "commandId", out->command.command_id,
                           sizeof(out->command.command_id), 1) ||
        strcmp(idempotency_key, out->command.command_id) != 0 ||
        !copy_string_field(root, "tenantId", out->command.tenant_id,
                           sizeof(out->command.tenant_id), 1) ||
        !copy_string_field(root, "sessionId",
                           out->command.provider_session_id,
                           sizeof(out->command.provider_session_id), 1) ||
        !copy_string_field(root, "type", type, sizeof(type), 1) ||
        !map_command_type(type, &out->command.kind) ||
        !copy_string_field(root, "provider", provider, sizeof(provider), 1) ||
        strcmp(provider, IRIS_ROOM_PROVIDER_ID) != 0 ||
        !copy_string_field(root, "correlationId",
                           out->command.correlation_id,
                           sizeof(out->command.correlation_id), 0) ||
        !copy_string_field(root, "causationId",
                           out->command.causation_id,
                           sizeof(out->command.causation_id), 0) ||
        !copy_string_field(root, "deadline", deadline, sizeof(deadline), 1) ||
        !parse_deadline(deadline, &out->deadline_unix_ms) ||
        !copy_string_field(root, "workerId", ignored_worker,
                           sizeof(ignored_worker), 1) ||
        !parse_u64_field(root, "dispatchEpoch", &ignored_epoch, 0) ||
        !data || turbo_json_type(data) != TURBO_JSON_OBJECT ||
        !request_keys_valid(root, data) ||
        !copy_string_field(data, "capability", capability,
                           sizeof(capability), 1) ||
        strcmp(capability, IRIS_ROOM_CAPABILITY) != 0 ||
        !copy_string_field(data, "roomId", out->command.room_id,
                           sizeof(out->command.room_id), 0) ||
        !parse_u64_field(data, "roomGeneration",
                         &out->command.room_generation, 0) ||
        !copy_string_field(data, "callId", out->command.call_id,
                           sizeof(out->command.call_id), 0) ||
        !copy_string_field(data, "role", out->command.role,
                           sizeof(out->command.role), 0) ||
        !copy_string_field(data, "userId", out->command.user_id,
                           sizeof(out->command.user_id), 0) ||
        !copy_string_field(data, "displayName", out->command.display_name,
                           sizeof(out->command.display_name), 0)) {
        *error = invalid_result("INVALID_COMMAND_ENVELOPE",
                                "provider room command envelope is invalid");
        goto cleanup;
    }
    if ((out->command.kind == IRIS_ROOM_COMMAND_CREATE ||
         out->command.kind == IRIS_ROOM_COMMAND_DESTROY) &&
        out->command.room_id[0] == '\0') {
        *error = invalid_result("ROOM_ID_REQUIRED",
                                "conference command requires roomId");
        goto cleanup;
    }
    if (out->command.kind == IRIS_ROOM_COMMAND_JOIN ||
        out->command.kind == IRIS_ROOM_COMMAND_UNJOIN) {
        if (out->command.room_id[0] == '\0' ||
            out->command.call_id[0] == '\0' ||
            !parse_u64_field(data, "callGeneration",
                             &out->command.call_generation, 0)) {
            *error = invalid_result(
                "MEMBERSHIP_IDS_REQUIRED",
                "membership command requires roomId/roomGeneration and callId/callGeneration");
            goto cleanup;
        }
    }
    valid = 1;
cleanup:
    turbo_free_json(&root);
    return valid;
}

static int same_command(const iris_room_request_t *left,
                        const iris_room_request_t *right) {
    return left->command.kind == right->command.kind &&
           strcmp(left->command.tenant_id, right->command.tenant_id) == 0 &&
           strcmp(left->command.provider_session_id,
                  right->command.provider_session_id) == 0 &&
           strcmp(left->command.correlation_id,
                  right->command.correlation_id) == 0 &&
           strcmp(left->command.causation_id,
                  right->command.causation_id) == 0 &&
           left->deadline_unix_ms == right->deadline_unix_ms &&
           strcmp(left->command.room_id, right->command.room_id) == 0 &&
           left->command.room_generation ==
               right->command.room_generation &&
           strcmp(left->command.call_id, right->command.call_id) == 0 &&
           left->command.call_generation ==
               right->command.call_generation &&
           strcmp(left->command.role, right->command.role) == 0 &&
           strcmp(left->command.user_id, right->command.user_id) == 0 &&
           strcmp(left->command.display_name,
                  right->command.display_name) == 0;
}

static iris_room_entry_t *find_entry(iris_room_bridge_t *bridge,
                                     const char *command_id) {
    for (size_t i = 0u; i < bridge->capacity; ++i) {
        if (bridge->entries[i].state != IRIS_ROOM_ENTRY_FREE &&
            strcmp(bridge->entries[i].request.command.command_id,
                   command_id) == 0) {
            return &bridge->entries[i];
        }
    }
    return NULL;
}

static iris_room_entry_t *reserve_entry(iris_room_bridge_t *bridge) {
    iris_room_entry_t *oldest = NULL;
    for (size_t i = 0u; i < bridge->capacity; ++i) {
        iris_room_entry_t *entry = &bridge->entries[i];
        if (entry->state == IRIS_ROOM_ENTRY_FREE) return entry;
        if (entry->state == IRIS_ROOM_ENTRY_COMPLETED &&
            (!oldest || entry->completed_sequence < oldest->completed_sequence)) {
            oldest = entry;
        }
    }
    return oldest;
}

iris_room_bridge_t *iris_room_bridge_create(
    const iris_room_bridge_config_t *config) {
    iris_room_bridge_t *bridge;
    if (!config || config->capacity == 0u || !config->execute ||
        !config->observe ||
        !config->ledger.context || !config->ledger.claim ||
        !config->ledger.commit_terminal || !config->ledger.mark_unknown ||
        !config->ledger.resource_seen ||
        !config->ledger.abort_intent ||
        config->capacity > SIZE_MAX / sizeof(iris_room_entry_t)) return NULL;
    bridge = (iris_room_bridge_t *)calloc(1, sizeof(*bridge));
    if (!bridge) return NULL;
    bridge->entries = (iris_room_entry_t *)calloc(
        config->capacity, sizeof(*bridge->entries));
    if (!bridge->entries) {
        free(bridge);
        return NULL;
    }
    bridge->capacity = config->capacity;
    bridge->execute = config->execute;
    bridge->execute_context = config->execute_context;
    bridge->observe = config->observe;
    bridge->observe_context = config->observe_context;
    bridge->realtime_ms = config->realtime_ms
                              ? config->realtime_ms
                              : system_realtime_ms;
    bridge->realtime_context = config->realtime_context;
    bridge->ledger = config->ledger;
    turbo_mutex_init(&bridge->mutex);
    return bridge;
}

void iris_room_bridge_destroy(iris_room_bridge_t *bridge) {
    if (!bridge) return;
    turbo_mutex_destroy(&bridge->mutex);
    free(bridge->entries);
    free(bridge);
}

iris_room_bridge_result_t iris_room_bridge_dispatch_json(
    iris_room_bridge_t *bridge, const char *idempotency_key,
    const char *body, size_t body_size) {
    iris_room_request_t request;
    iris_room_bridge_result_t error;
    iris_room_entry_t *entry;
    iris_room_execution_t execution;
    iris_command_identity_t identity;
    iris_command_claim_result_t claim;
    iris_command_terminal_outcome_t terminal;
    ivr_status_t ledger_status;
    uint64_t now_ms;
    if (!bridge) {
        return make_result(IRIS_ROOM_BRIDGE_UNAVAILABLE, NULL, NULL,
                           "ROOM_PROVIDER_UNAVAILABLE",
                           "Iris room provider is not configured");
    }
    if (!parse_request(idempotency_key, body, body_size, &request, &error)) {
        return error;
    }
    if (!command_identity(&request, &identity)) {
        return make_result(IRIS_ROOM_BRIDGE_INTERNAL,
                           request.command.command_id, NULL,
                           "COMMAND_FINGERPRINT_FAILED",
                           "provider room command fingerprint failed");
    }
    now_ms = bridge->realtime_ms(bridge->realtime_context);

    memset(&claim, 0, sizeof(claim));
    ledger_status = bridge->ledger.claim(bridge->ledger.context, &identity,
                                         &claim);
    if (ledger_status != IVR_OK) {
        return make_result(
            ledger_status == IVR_ENOSPC ? IRIS_ROOM_BRIDGE_FULL
                                        : IRIS_ROOM_BRIDGE_UNAVAILABLE,
            request.command.command_id, NULL,
            ledger_status == IVR_ENOSPC ? "COMMAND_LEDGER_FULL"
                                        : "COMMAND_LEDGER_UNAVAILABLE",
            ledger_status == IVR_ENOSPC
                ? "provider command ledger queue is full"
                : "provider command ledger is unavailable");
    }
    if (claim.disposition == IRIS_COMMAND_CLAIM_CONFLICT) {
        return make_result(IRIS_ROOM_BRIDGE_CONFLICT,
                           request.command.command_id, NULL,
                           "IDEMPOTENCY_CONFLICT",
                           "commandId was reused with different room data");
    }
    if (claim.disposition == IRIS_COMMAND_CLAIM_IN_PROGRESS) {
        return make_result(IRIS_ROOM_BRIDGE_IN_PROGRESS,
                           request.command.command_id, NULL,
                           "COMMAND_IN_PROGRESS",
                           "provider room command is still executing");
    }
    if (claim.disposition == IRIS_COMMAND_CLAIM_OUTCOME_UNKNOWN) {
        iris_resource_observation_t observation;
        int seen = 0;

        memset(&observation, 0, sizeof(observation));
        observation.state = IRIS_RESOURCE_OBSERVATION_UNKNOWN;
        if ((request.command.kind != IRIS_ROOM_COMMAND_DESTROY &&
             request.command.kind != IRIS_ROOM_COMMAND_UNJOIN) ||
            bridge->observe(bridge->observe_context, &request.command,
                            &observation) != IVR_OK ||
            observation.state != IRIS_RESOURCE_OBSERVATION_ABSENT ||
            bridge->ledger.resource_seen(bridge->ledger.context, &identity,
                                         &seen) != IVR_OK ||
            !seen ||
            !make_already_absent_execution(&request.command, &execution)) {
            return make_result(
                IRIS_ROOM_BRIDGE_UNAVAILABLE,
                request.command.command_id, NULL,
                "PROVIDER_OUTCOME_UNKNOWN",
                "provider command outcome requires query or reconciliation");
        }
        memset(&terminal, 0, sizeof(terminal));
        if (!copy_fixed(terminal.terminal_status,
                        sizeof(terminal.terminal_status), "succeeded", 1) ||
            !copy_fixed(terminal.event_type, sizeof(terminal.event_type),
                        execution.event_type, 1) ||
            !copy_fixed(terminal.result_json, sizeof(terminal.result_json),
                        execution.data, 1) ||
            bridge->ledger.commit_terminal(
                bridge->ledger.context, &identity, &terminal) != IVR_OK) {
            return make_result(
                IRIS_ROOM_BRIDGE_UNAVAILABLE,
                request.command.command_id, NULL,
                "PROVIDER_OUTCOME_UNKNOWN",
                "observed room absence could not be committed durably");
        }
        return make_result(IRIS_ROOM_BRIDGE_TERMINAL,
                           request.command.command_id, &execution, NULL,
                           NULL);
    }
    if (claim.disposition == IRIS_COMMAND_CLAIM_REPLAY_ACCEPTED) {
        return make_result(
            IRIS_ROOM_BRIDGE_UNAVAILABLE, request.command.command_id, NULL,
            "PROVIDER_OUTCOME_UNKNOWN",
            "provider command outcome requires query or reconciliation");
    }
    if (claim.disposition == IRIS_COMMAND_CLAIM_REPLAY_TERMINAL) {
        memset(&execution, 0, sizeof(execution));
        execution.terminal_status =
            strcmp(claim.terminal_status, "succeeded") == 0
                ? IRIS_ROOM_TERMINAL_SUCCEEDED
                : IRIS_ROOM_TERMINAL_FAILED;
        if (!copy_fixed(execution.event_type, sizeof(execution.event_type),
                        claim.event_type, 1) ||
            !copy_fixed(execution.data, sizeof(execution.data),
                        claim.result_json, 1)) {
            return make_result(IRIS_ROOM_BRIDGE_INTERNAL,
                               request.command.command_id, NULL,
                               "COMMAND_LEDGER_RECORD_INVALID",
                               "provider command ledger result is invalid");
        }
        return make_result(IRIS_ROOM_BRIDGE_DUPLICATE,
                           request.command.command_id, &execution, NULL, NULL);
    }
    if (claim.disposition != IRIS_COMMAND_CLAIM_EXECUTE) {
        return make_result(IRIS_ROOM_BRIDGE_INTERNAL,
                           request.command.command_id, NULL,
                           "COMMAND_LEDGER_STATE_INVALID",
                           "provider command ledger state is invalid");
    }

    turbo_mutex_lock(&bridge->mutex);
    entry = find_entry(bridge, request.command.command_id);
    if (entry) {
        if (!same_command(&entry->request, &request)) {
            turbo_mutex_unlock(&bridge->mutex);
            return make_result(IRIS_ROOM_BRIDGE_INVALID,
                               request.command.command_id, NULL,
                               "IDEMPOTENCY_CONFLICT",
                               "commandId was reused with different room data");
        }
        if (entry->state == IRIS_ROOM_ENTRY_EXECUTING) {
            turbo_mutex_unlock(&bridge->mutex);
            return make_result(IRIS_ROOM_BRIDGE_IN_PROGRESS,
                               request.command.command_id, NULL,
                               "COMMAND_IN_PROGRESS",
                               "provider room command is still executing");
        }
        execution = entry->execution;
        turbo_mutex_unlock(&bridge->mutex);
        return make_result(IRIS_ROOM_BRIDGE_DUPLICATE,
                           request.command.command_id, &execution, NULL, NULL);
    }
    if (request.deadline_unix_ms <= now_ms) {
        turbo_mutex_unlock(&bridge->mutex);
        (void)bridge->ledger.abort_intent(bridge->ledger.context, &identity);
        return make_result(IRIS_ROOM_BRIDGE_EXPIRED,
                           request.command.command_id, NULL,
                           "COMMAND_DEADLINE_EXPIRED",
                           "provider room command deadline has expired");
    }
    entry = reserve_entry(bridge);
    if (!entry) {
        turbo_mutex_unlock(&bridge->mutex);
        (void)bridge->ledger.abort_intent(bridge->ledger.context, &identity);
        return make_result(IRIS_ROOM_BRIDGE_FULL,
                           request.command.command_id, NULL,
                           "ROOM_CORRELATION_FULL",
                           "provider room command capacity is exhausted");
    }
    memset(entry, 0, sizeof(*entry));
    entry->state = IRIS_ROOM_ENTRY_EXECUTING;
    entry->request = request;
    turbo_mutex_unlock(&bridge->mutex);

    memset(&execution, 0, sizeof(execution));
    if (bridge->execute(bridge->execute_context, &request.command,
                        &execution) != 0 ||
        (execution.terminal_status != IRIS_ROOM_TERMINAL_SUCCEEDED &&
         execution.terminal_status != IRIS_ROOM_TERMINAL_FAILED) ||
        execution.event_type[0] == '\0' || execution.data[0] == '\0') {
        execution = internal_failure_execution();
    }
    if (execution.resource_absent) {
        int seen = 0;
        ledger_status = bridge->ledger.resource_seen(
            bridge->ledger.context, &identity, &seen);
        if (ledger_status != IVR_OK) {
            (void)bridge->ledger.mark_unknown(bridge->ledger.context,
                                              &identity);
            turbo_mutex_lock(&bridge->mutex);
            entry = find_entry(bridge, request.command.command_id);
            if (entry && entry->state == IRIS_ROOM_ENTRY_EXECUTING) {
                memset(entry, 0, sizeof(*entry));
            }
            turbo_mutex_unlock(&bridge->mutex);
            return make_result(
                IRIS_ROOM_BRIDGE_UNAVAILABLE,
                request.command.command_id, NULL,
                "PROVIDER_OUTCOME_UNKNOWN",
                "resource absence could not be verified durably");
        }
        if (seen &&
            !make_already_absent_execution(&request.command, &execution)) {
            execution = internal_failure_execution();
        }
    }

    memset(&terminal, 0, sizeof(terminal));
    (void)copy_fixed(terminal.terminal_status,
                     sizeof(terminal.terminal_status),
                     execution.terminal_status == IRIS_ROOM_TERMINAL_SUCCEEDED
                         ? "succeeded"
                         : "failed",
                     1);
    (void)copy_fixed(terminal.event_type, sizeof(terminal.event_type),
                     execution.event_type, 1);
    (void)copy_fixed(terminal.result_json, sizeof(terminal.result_json),
                     execution.data, 1);
    ledger_status = bridge->ledger.commit_terminal(
        bridge->ledger.context, &identity, &terminal);
    if (ledger_status != IVR_OK) {
        (void)bridge->ledger.mark_unknown(bridge->ledger.context, &identity);
        turbo_mutex_lock(&bridge->mutex);
        entry = find_entry(bridge, request.command.command_id);
        if (entry && entry->state == IRIS_ROOM_ENTRY_EXECUTING) {
            memset(entry, 0, sizeof(*entry));
        }
        turbo_mutex_unlock(&bridge->mutex);
        return make_result(
            IRIS_ROOM_BRIDGE_UNAVAILABLE, request.command.command_id, NULL,
            "PROVIDER_OUTCOME_UNKNOWN",
            "room side effect completed but durable outcome commit failed");
    }

    turbo_mutex_lock(&bridge->mutex);
    entry = find_entry(bridge, request.command.command_id);
    if (!entry || entry->state != IRIS_ROOM_ENTRY_EXECUTING) {
        turbo_mutex_unlock(&bridge->mutex);
        return make_result(IRIS_ROOM_BRIDGE_INTERNAL,
                           request.command.command_id, NULL,
                           "ROOM_CORRELATION_LOST",
                           "provider room command correlation was lost");
    }
    entry->execution = execution;
    entry->state = IRIS_ROOM_ENTRY_COMPLETED;
    entry->completed_sequence = ++bridge->next_completed_sequence;
    turbo_mutex_unlock(&bridge->mutex);
    return make_result(IRIS_ROOM_BRIDGE_TERMINAL,
                       request.command.command_id, &execution, NULL, NULL);
}
