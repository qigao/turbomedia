#include "iris_command_ledger.h"

#include "iris_command_ledger_v2.h"
#include "iris_orm_store.h"

#include <data_bind.h>
#include <platform.h>
#include <salts_error.h>
#include <salts_thread.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IRIS_COMMAND_LEDGER_SCHEMA_VERSION 2u
#define IRIS_COMMAND_LEDGER_MIN_VALUE_SIZE 4096u
#define IRIS_COMMAND_STATE_INTENT "intent"
#define IRIS_COMMAND_STATE_ACCEPTED "accepted"
#define IRIS_COMMAND_STATE_TERMINAL "terminal"
#define IRIS_COMMAND_STATE_UNKNOWN "unknown"

typedef enum ledger_request_kind_e {
    LEDGER_REQUEST_CLAIM = 1,
    LEDGER_REQUEST_ACCEPTED,
    LEDGER_REQUEST_TERMINAL,
    LEDGER_REQUEST_UNKNOWN,
    LEDGER_REQUEST_RESOURCE_SEEN,
    LEDGER_REQUEST_ABORT,
    LEDGER_REQUEST_RETENTION
} ledger_request_kind_t;

typedef struct ledger_request_s {
    ledger_request_kind_t kind;
    iris_command_identity_t identity;
    char provider_resource_id[IRIS_COMMAND_RESOURCE_ID_CAPACITY];
    iris_command_terminal_outcome_t terminal;
    iris_command_claim_result_t claim_result;
    int resource_seen;
    iris_command_retention_result_t retention_result;
    ivr_status_t result;
    salts_mutex_t mutex;
    salts_cond_t condition;
    int completed;
} ledger_request_t;

typedef struct ledger_candidate_s {
    IrisProviderCommandRecordV2_t record;
    uint64_t revision;
} ledger_candidate_t;

struct iris_command_ledger_s {
    iris_record_store_t *store;
    iris_orm_store_owner_t *orm_store_owner;
    DataBind *codec;
    ledger_request_t **requests;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    size_t retention_batch_size;
    uint64_t terminal_retention_ms;
    uint64_t retention_sweep_interval_ms;
    uint64_t next_sweep_monotonic_ms;
    iris_command_ledger_realtime_ms_fn realtime_ms;
    void *realtime_context;
    salts_mutex_t mutex;
    salts_cond_t not_empty;
    salts_cond_t startup;
    salts_thread_t thread;
    int thread_started;
    int running;
    int accepting;
    int startup_completed;
    int startup_result;
    iris_command_ledger_stats_t stats;
};

static uint64_t system_realtime_ms(void *context) {
    time_t seconds;
    (void)context;
    seconds = time(NULL);
    return seconds < 0 ? 0u : (uint64_t)seconds * UINT64_C(1000);
}

static uint64_t ledger_now_ms(const iris_command_ledger_t *ledger) {
    return ledger->realtime_ms(ledger->realtime_context);
}

static int copy_text(char *destination, size_t capacity, const char *source,
                     int required) {
    size_t size;
    if (!destination || capacity == 0u || !source) return 0;
    size = strlen(source);
    if ((required && size == 0u) || size >= capacity) return 0;
    memcpy(destination, source, size + 1u);
    return 1;
}

static int set_owned(tstr *field, const char *value) {
    tstr next;
    if (!field || !value) return 0;
    next = tstr_cpy(*field, value);
    if (!next) return 0;
    *field = next;
    return 1;
}

static int fingerprint_valid(const char *fingerprint) {
    size_t i;
    if (!fingerprint || strlen(fingerprint) != 64u) return 0;
    for (i = 0u; i < 64u; ++i) {
        if (!isxdigit((unsigned char)fingerprint[i]) ||
            (fingerprint[i] >= 'A' && fingerprint[i] <= 'F')) {
            return 0;
        }
    }
    return 1;
}

static int identity_valid(const iris_command_identity_t *identity) {
    return identity && identity->command_id[0] != '\0' &&
           strlen(identity->command_id) < sizeof(identity->command_id) &&
           fingerprint_valid(identity->semantic_fingerprint) &&
           identity->provider_session_id[0] != '\0' &&
           strlen(identity->provider_session_id) <
               sizeof(identity->provider_session_id) &&
           identity->command_type[0] != '\0' &&
           strlen(identity->command_type) < sizeof(identity->command_type) &&
           identity->resource_id[0] != '\0' &&
           strlen(identity->resource_id) < sizeof(identity->resource_id) &&
           identity->resource_generation != 0u &&
           strlen(identity->resource_scope_id) <
               sizeof(identity->resource_scope_id) &&
           ((identity->resource_scope_id[0] == '\0' &&
             identity->resource_scope_generation == 0u) ||
            (identity->resource_scope_id[0] != '\0' &&
             identity->resource_scope_generation != 0u));
}

static int state_valid(const char *state) {
    return state &&
           (strcmp(state, IRIS_COMMAND_STATE_INTENT) == 0 ||
            strcmp(state, IRIS_COMMAND_STATE_ACCEPTED) == 0 ||
            strcmp(state, IRIS_COMMAND_STATE_TERMINAL) == 0 ||
            strcmp(state, IRIS_COMMAND_STATE_UNKNOWN) == 0);
}

static int terminal_status_valid(const char *status) {
    return status && (strcmp(status, "succeeded") == 0 ||
                      strcmp(status, "failed") == 0);
}

static int record_identity_matches(
    const IrisProviderCommandRecordV2_t *record,
    const iris_command_identity_t *identity) {
    return strcmp(record->command_id, identity->command_id) == 0 &&
           strcmp(record->semantic_fingerprint,
                  identity->semantic_fingerprint) == 0 &&
           strcmp(record->provider_session_id,
                  identity->provider_session_id) == 0 &&
           strcmp(record->command_type, identity->command_type) == 0 &&
           strcmp(record->resource_scope_id,
                  identity->resource_scope_id) == 0 &&
           record->resource_scope_generation ==
               identity->resource_scope_generation &&
           strcmp(record->resource_id, identity->resource_id) == 0 &&
           record->resource_generation == identity->resource_generation;
}

static int record_from_identity(
    IrisProviderCommandRecordV2_t *record,
    const iris_command_identity_t *identity, const char *state,
    uint64_t changed_at_ms) {
    IrisProviderCommandRecordV2_init(record);
    record->schema_version = IRIS_COMMAND_LEDGER_SCHEMA_VERSION;
    record->state_changed_at_ms = changed_at_ms;
    if (!set_owned(&record->command_state, state) ||
        !set_owned(&record->command_id, identity->command_id) ||
        !set_owned(&record->semantic_fingerprint,
                   identity->semantic_fingerprint) ||
        !set_owned(&record->provider_session_id,
                   identity->provider_session_id) ||
        !set_owned(&record->command_type, identity->command_type) ||
        !set_owned(&record->resource_scope_id,
                   identity->resource_scope_id) ||
        !set_owned(&record->resource_id, identity->resource_id) ||
        !set_owned(&record->provider_resource_id, "") ||
        !set_owned(&record->terminal_status, "") ||
        !set_owned(&record->event_type, "") ||
        !set_owned(&record->result_json, "")) {
        IrisProviderCommandRecordV2_clear(record);
        return SALTS_ENOMEM;
    }
    record->resource_scope_generation =
        identity->resource_scope_generation;
    record->resource_generation = identity->resource_generation;
    return SALTS_OK;
}

static int encode_record(DataBind *codec,
                         const IrisProviderCommandRecordV2_t *record,
                         uint8_t **value, size_t *value_size) {
    DataBindError error = DATA_BIND_ERROR_INIT;
    char *json = NULL;
    size_t json_size = 0u;
    if (IrisProviderCommandRecordV2_to_json(
            codec, record, &json, &json_size, &error) != DATA_BIND_OK ||
        !json) {
        return SALTS_EPROTO;
    }
    *value = (uint8_t *)json;
    *value_size = json_size;
    return SALTS_OK;
}

static int decode_record(DataBind *codec, const uint8_t *value,
                         size_t value_size,
                         IrisProviderCommandRecordV2_t *record) {
    DataBindError error = DATA_BIND_ERROR_INIT;
    IrisProviderCommandRecordV2_init(record);
    if (IrisProviderCommandRecordV2_from_json(
            codec, record, (const char *)value, value_size, &error) !=
            DATA_BIND_OK ||
        record->schema_version != IRIS_COMMAND_LEDGER_SCHEMA_VERSION ||
        !record->command_state || !state_valid(record->command_state) ||
        !record->command_id || record->command_id[0] == '\0' ||
        strlen(record->command_id) >= IRIS_COMMAND_ID_CAPACITY ||
        !record->semantic_fingerprint ||
        !fingerprint_valid(record->semantic_fingerprint) ||
        !record->provider_session_id ||
        record->provider_session_id[0] == '\0' ||
        strlen(record->provider_session_id) >=
            IRIS_COMMAND_SESSION_ID_CAPACITY ||
        !record->command_type || record->command_type[0] == '\0' ||
        strlen(record->command_type) >= IRIS_COMMAND_TYPE_CAPACITY ||
        !record->resource_scope_id ||
        strlen(record->resource_scope_id) >=
            IRIS_COMMAND_RESOURCE_ID_CAPACITY ||
        ((record->resource_scope_id[0] == '\0' &&
          record->resource_scope_generation != 0u) ||
         (record->resource_scope_id[0] != '\0' &&
          record->resource_scope_generation == 0u)) ||
        !record->resource_id || record->resource_id[0] == '\0' ||
        strlen(record->resource_id) >= IRIS_COMMAND_RESOURCE_ID_CAPACITY ||
        record->resource_generation == 0u ||
        !record->provider_resource_id ||
        strlen(record->provider_resource_id) >=
            IRIS_COMMAND_RESOURCE_ID_CAPACITY ||
        !record->terminal_status || strlen(record->terminal_status) >= 16u ||
        !record->event_type ||
        strlen(record->event_type) >= IRIS_COMMAND_EVENT_TYPE_CAPACITY ||
        !record->result_json ||
        strlen(record->result_json) >= IRIS_COMMAND_RESULT_JSON_CAPACITY ||
        record->state_changed_at_ms == 0u ||
        (strcmp(record->command_state, IRIS_COMMAND_STATE_TERMINAL) == 0 &&
         (record->terminal_at_ms == 0u ||
          !terminal_status_valid(record->terminal_status) ||
          record->event_type[0] == '\0' || record->result_json[0] == '\0')) ||
        (strcmp(record->command_state, IRIS_COMMAND_STATE_TERMINAL) != 0 &&
         record->terminal_at_ms != 0u)) {
        IrisProviderCommandRecordV2_clear(record);
        return SALTS_EPROTO;
    }
    return SALTS_OK;
}

static int commit_record(iris_command_ledger_t *ledger,
                         const IrisProviderCommandRecordV2_t *record,
                         uint64_t expected_revision,
                         uint64_t next_revision) {
    iris_record_mutation_t mutation = IRIS_RECORD_MUTATION_INIT;
    uint8_t *value = NULL;
    size_t value_size = 0u;
    int rc;
    if (next_revision == 0u || next_revision <= expected_revision) {
        return SALTS_ERANGE;
    }
    rc = encode_record(ledger->codec, record, &value, &value_size);
    if (rc == SALTS_OK) {
        mutation.kind = IRIS_RECORD_PUT;
        mutation.key = (const uint8_t *)record->command_id;
        mutation.key_size = strlen(record->command_id);
        mutation.expected_revision = expected_revision;
        mutation.next_revision = next_revision;
        mutation.value = value;
        mutation.value_size = value_size;
        rc = ledger->store->commit(ledger->store->ctx, &mutation, 1u);
    }
    tbe_typed_serialized_free(value);
    return rc;
}

typedef struct find_context_s {
    DataBind *codec;
    const char *command_id;
    ledger_candidate_t *candidate;
    int found;
} find_context_t;

static int find_visit(void *context, const iris_record_view_t *view) {
    find_context_t *find = (find_context_t *)context;
    IrisProviderCommandRecordV2_t record;
    size_t command_id_size = strlen(find->command_id);
    int rc;
    if (find->found || command_id_size != view->key_size ||
        memcmp(find->command_id, view->key, view->key_size) != 0) {
        return SALTS_OK;
    }
    rc = decode_record(find->codec, view->value, view->value_size, &record);
    if (rc != SALTS_OK) return rc;
    if (strcmp(record.command_id, find->command_id) != 0) {
        IrisProviderCommandRecordV2_clear(&record);
        return SALTS_EPROTO;
    }
    find->candidate->record = record;
    find->candidate->revision = view->revision;
    find->found = 1;
    return SALTS_OK;
}

static int find_record(iris_command_ledger_t *ledger, const char *command_id,
                       ledger_candidate_t *candidate, int *found) {
    find_context_t context;
    int rc;
    memset(candidate, 0, sizeof(*candidate));
    memset(&context, 0, sizeof(context));
    context.codec = ledger->codec;
    context.command_id = command_id;
    context.candidate = candidate;
    rc = ledger->store->scan(ledger->store->ctx, find_visit, &context);
    *found = context.found;
    return rc;
}

static void stats_add(iris_command_ledger_t *ledger, uint64_t *counter,
                      uint64_t amount);
static ivr_status_t storage_failure(iris_command_ledger_t *ledger);

typedef struct resource_history_context_s {
    DataBind *codec;
    const iris_command_identity_t *identity;
    int seen;
} resource_history_context_t;

static int same_resource_incarnation(
    const IrisProviderCommandRecordV2_t *record,
    const iris_command_identity_t *identity) {
    return strcmp(record->provider_session_id,
                  identity->provider_session_id) == 0 &&
           strcmp(record->resource_scope_id,
                  identity->resource_scope_id) == 0 &&
           record->resource_scope_generation ==
               identity->resource_scope_generation &&
           strcmp(record->resource_id, identity->resource_id) == 0 &&
           record->resource_generation == identity->resource_generation;
}

static int resource_history_visit(
    void *context, const iris_record_view_t *view) {
    resource_history_context_t *history =
        (resource_history_context_t *)context;
    IrisProviderCommandRecordV2_t record;
    int rc;
    int durable_fact;
    if (history->seen) return SALTS_OK;
    rc = decode_record(history->codec, view->value, view->value_size,
                       &record);
    if (rc != SALTS_OK) return rc;
    durable_fact =
        strcmp(record.command_state, IRIS_COMMAND_STATE_ACCEPTED) == 0 ||
        (strcmp(record.command_state, IRIS_COMMAND_STATE_TERMINAL) == 0 &&
         strcmp(record.terminal_status, "succeeded") == 0);
    history->seen = durable_fact &&
                    same_resource_incarnation(&record, history->identity);
    IrisProviderCommandRecordV2_clear(&record);
    return SALTS_OK;
}

static ivr_status_t process_resource_seen(
    iris_command_ledger_t *ledger, ledger_request_t *request) {
    resource_history_context_t context;
    int rc;
    memset(&context, 0, sizeof(context));
    context.codec = ledger->codec;
    context.identity = &request->identity;
    rc = ledger->store->scan(ledger->store->ctx, resource_history_visit,
                             &context);
    stats_add(ledger, &ledger->stats.resource_queries_total, 1u);
    if (rc != SALTS_OK) return storage_failure(ledger);
    request->resource_seen = context.seen;
    if (context.seen) {
        stats_add(ledger, &ledger->stats.resource_seen_total, 1u);
    }
    return IVR_OK;
}

static void stats_add(iris_command_ledger_t *ledger, uint64_t *counter,
                      uint64_t amount) {
    salts_mutex_lock(&ledger->mutex);
    *counter += amount;
    salts_mutex_unlock(&ledger->mutex);
}

static ivr_status_t storage_failure(iris_command_ledger_t *ledger) {
    stats_add(ledger, &ledger->stats.storage_failures_total, 1u);
    return IVR_ESTATE;
}

static ivr_status_t process_claim(iris_command_ledger_t *ledger,
                                  ledger_request_t *request) {
    ledger_candidate_t candidate;
    IrisProviderCommandRecordV2_t record;
    uint64_t now_ms = ledger_now_ms(ledger);
    int found = 0;
    int rc;
    memset(&request->claim_result, 0, sizeof(request->claim_result));
    if (now_ms == 0u) return IVR_ESTATE;
    rc = find_record(ledger, request->identity.command_id, &candidate, &found);
    if (rc != SALTS_OK) return storage_failure(ledger);
    stats_add(ledger, &ledger->stats.claims_total, 1u);
    if (!found) {
        rc = record_from_identity(&record, &request->identity,
                                  IRIS_COMMAND_STATE_INTENT, now_ms);
        if (rc == SALTS_OK) {
            rc = commit_record(ledger, &record,
                               IRIS_RECORD_REVISION_ABSENT, 1u);
            IrisProviderCommandRecordV2_clear(&record);
        }
        if (rc != SALTS_OK) return storage_failure(ledger);
        request->claim_result.disposition = IRIS_COMMAND_CLAIM_EXECUTE;
        request->claim_result.store_revision = 1u;
        return IVR_OK;
    }
    if (!record_identity_matches(&candidate.record, &request->identity)) {
        request->claim_result.disposition = IRIS_COMMAND_CLAIM_CONFLICT;
        request->claim_result.store_revision = candidate.revision;
        IrisProviderCommandRecordV2_clear(&candidate.record);
        stats_add(ledger, &ledger->stats.conflicts_total, 1u);
        return IVR_OK;
    }
    request->claim_result.store_revision = candidate.revision;
    if (strcmp(candidate.record.command_state, IRIS_COMMAND_STATE_INTENT) == 0) {
        request->claim_result.disposition = IRIS_COMMAND_CLAIM_IN_PROGRESS;
    } else if (strcmp(candidate.record.command_state,
                      IRIS_COMMAND_STATE_ACCEPTED) == 0) {
        request->claim_result.disposition = IRIS_COMMAND_CLAIM_REPLAY_ACCEPTED;
        (void)copy_text(request->claim_result.provider_resource_id,
                        sizeof(request->claim_result.provider_resource_id),
                        candidate.record.provider_resource_id, 0);
    } else if (strcmp(candidate.record.command_state,
                      IRIS_COMMAND_STATE_TERMINAL) == 0) {
        request->claim_result.disposition = IRIS_COMMAND_CLAIM_REPLAY_TERMINAL;
        (void)copy_text(request->claim_result.terminal_status,
                        sizeof(request->claim_result.terminal_status),
                        candidate.record.terminal_status, 1);
        (void)copy_text(request->claim_result.event_type,
                        sizeof(request->claim_result.event_type),
                        candidate.record.event_type, 1);
        (void)copy_text(request->claim_result.result_json,
                        sizeof(request->claim_result.result_json),
                        candidate.record.result_json, 1);
    } else {
        request->claim_result.disposition = IRIS_COMMAND_CLAIM_OUTCOME_UNKNOWN;
        stats_add(ledger, &ledger->stats.unknown_total, 1u);
    }
    IrisProviderCommandRecordV2_clear(&candidate.record);
    stats_add(ledger, &ledger->stats.duplicates_total, 1u);
    return IVR_OK;
}

static ivr_status_t process_settle(iris_command_ledger_t *ledger,
                                   ledger_request_t *request,
                                   const char *next_state) {
    ledger_candidate_t candidate;
    uint64_t now_ms = ledger_now_ms(ledger);
    int found = 0;
    int rc;
    if (now_ms == 0u) return IVR_ESTATE;
    rc = find_record(ledger, request->identity.command_id, &candidate, &found);
    if (rc != SALTS_OK) return storage_failure(ledger);
    if (!found ||
        !record_identity_matches(&candidate.record, &request->identity)) {
        if (found) IrisProviderCommandRecordV2_clear(&candidate.record);
        return IVR_ESTATE;
    }
    if (strcmp(next_state, IRIS_COMMAND_STATE_ACCEPTED) == 0) {
        if (strcmp(candidate.record.command_state,
                   IRIS_COMMAND_STATE_ACCEPTED) == 0 &&
            strcmp(candidate.record.provider_resource_id,
                   request->provider_resource_id) == 0) {
            IrisProviderCommandRecordV2_clear(&candidate.record);
            return IVR_OK;
        }
        if (strcmp(candidate.record.command_state,
                   IRIS_COMMAND_STATE_INTENT) != 0 &&
            strcmp(candidate.record.command_state,
                   IRIS_COMMAND_STATE_UNKNOWN) != 0) {
            IrisProviderCommandRecordV2_clear(&candidate.record);
            return IVR_ESTATE;
        }
        if (!set_owned(&candidate.record.provider_resource_id,
                       request->provider_resource_id)) {
            IrisProviderCommandRecordV2_clear(&candidate.record);
            return IVR_ENOSPC;
        }
    } else if (strcmp(next_state, IRIS_COMMAND_STATE_TERMINAL) == 0) {
        if (strcmp(candidate.record.command_state,
                   IRIS_COMMAND_STATE_TERMINAL) == 0 &&
            strcmp(candidate.record.terminal_status,
                   request->terminal.terminal_status) == 0 &&
            strcmp(candidate.record.event_type,
                   request->terminal.event_type) == 0 &&
            strcmp(candidate.record.result_json,
                   request->terminal.result_json) == 0) {
            IrisProviderCommandRecordV2_clear(&candidate.record);
            return IVR_OK;
        }
        if (strcmp(candidate.record.command_state, IRIS_COMMAND_STATE_INTENT) !=
                0 &&
            strcmp(candidate.record.command_state,
                   IRIS_COMMAND_STATE_ACCEPTED) != 0 &&
            strcmp(candidate.record.command_state,
                   IRIS_COMMAND_STATE_UNKNOWN) != 0) {
            IrisProviderCommandRecordV2_clear(&candidate.record);
            return IVR_ESTATE;
        }
        if (!set_owned(&candidate.record.terminal_status,
                       request->terminal.terminal_status) ||
            !set_owned(&candidate.record.event_type,
                       request->terminal.event_type) ||
            !set_owned(&candidate.record.result_json,
                       request->terminal.result_json)) {
            IrisProviderCommandRecordV2_clear(&candidate.record);
            return IVR_ENOSPC;
        }
        candidate.record.terminal_at_ms = now_ms;
    } else {
        if (strcmp(candidate.record.command_state,
                   IRIS_COMMAND_STATE_UNKNOWN) == 0) {
            IrisProviderCommandRecordV2_clear(&candidate.record);
            return IVR_OK;
        }
        if (strcmp(candidate.record.command_state, IRIS_COMMAND_STATE_INTENT) !=
                0 &&
            strcmp(candidate.record.command_state,
                   IRIS_COMMAND_STATE_ACCEPTED) != 0) {
            IrisProviderCommandRecordV2_clear(&candidate.record);
            return IVR_ESTATE;
        }
        candidate.record.terminal_at_ms = 0u;
    }
    if (!set_owned(&candidate.record.command_state, next_state)) {
        IrisProviderCommandRecordV2_clear(&candidate.record);
        return IVR_ENOSPC;
    }
    candidate.record.state_changed_at_ms = now_ms;
    rc = commit_record(ledger, &candidate.record, candidate.revision,
                       candidate.revision + 1u);
    IrisProviderCommandRecordV2_clear(&candidate.record);
    return rc == SALTS_OK ? IVR_OK : storage_failure(ledger);
}

typedef struct collect_context_s {
    DataBind *codec;
    ledger_candidate_t *items;
    size_t capacity;
    size_t count;
    size_t terminal_total;
    uint64_t now_ms;
    uint64_t terminal_retention_ms;
    int intents_only;
} collect_context_t;

static int collect_visit(void *context,
                         const iris_record_view_t *view) {
    collect_context_t *collect = (collect_context_t *)context;
    IrisProviderCommandRecordV2_t record;
    int selected = 0;
    int rc = decode_record(collect->codec, view->value, view->value_size,
                           &record);
    if (rc != SALTS_OK) return rc;
    if (collect->intents_only) {
        selected = strcmp(record.command_state,
                          IRIS_COMMAND_STATE_INTENT) == 0;
    } else if (strcmp(record.command_state,
                      IRIS_COMMAND_STATE_TERMINAL) == 0) {
        collect->terminal_total++;
        selected = collect->now_ms >= record.terminal_at_ms &&
                   collect->now_ms - record.terminal_at_ms >=
                       collect->terminal_retention_ms;
    }
    if (selected && collect->count < collect->capacity) {
        collect->items[collect->count].record = record;
        collect->items[collect->count].revision = view->revision;
        collect->count++;
        return SALTS_OK;
    }
    IrisProviderCommandRecordV2_clear(&record);
    return SALTS_OK;
}

static int normalize_interrupted_intents(iris_command_ledger_t *ledger) {
    ledger_candidate_t *items;
    collect_context_t collect;
    uint64_t now_ms = ledger_now_ms(ledger);
    size_t i;
    int rc;
    if (now_ms == 0u || ledger->store->max_records >
                            SIZE_MAX / sizeof(*items)) {
        return SALTS_ERANGE;
    }
    items = (ledger_candidate_t *)calloc(ledger->store->max_records,
                                         sizeof(*items));
    if (!items) return SALTS_ENOMEM;
    memset(&collect, 0, sizeof(collect));
    collect.codec = ledger->codec;
    collect.items = items;
    collect.capacity = ledger->store->max_records;
    collect.intents_only = 1;
    rc = ledger->store->scan(ledger->store->ctx, collect_visit, &collect);
    for (i = 0u; rc == SALTS_OK && i < collect.count; ++i) {
        if (!set_owned(&items[i].record.command_state,
                       IRIS_COMMAND_STATE_UNKNOWN)) {
            rc = SALTS_ENOMEM;
            break;
        }
        items[i].record.state_changed_at_ms = now_ms;
        rc = commit_record(ledger, &items[i].record, items[i].revision,
                           items[i].revision + 1u);
        if (rc == SALTS_OK) {
            stats_add(ledger, &ledger->stats.recovered_unknown_total, 1u);
        }
    }
    for (i = 0u; i < collect.count; ++i) {
        IrisProviderCommandRecordV2_clear(&items[i].record);
    }
    free(items);
    return rc;
}

static ivr_status_t process_retention(iris_command_ledger_t *ledger,
                                      ledger_request_t *request) {
    ledger_candidate_t *items;
    collect_context_t collect;
    uint64_t now_ms = ledger_now_ms(ledger);
    size_t i;
    int rc;
    memset(&request->retention_result, 0,
           sizeof(request->retention_result));
    if (now_ms == 0u || ledger->retention_batch_size >
                            SIZE_MAX / sizeof(*items)) {
        return IVR_ESTATE;
    }
    items = (ledger_candidate_t *)calloc(ledger->retention_batch_size,
                                         sizeof(*items));
    if (!items) return IVR_ENOSPC;
    memset(&collect, 0, sizeof(collect));
    collect.codec = ledger->codec;
    collect.items = items;
    collect.capacity = ledger->retention_batch_size;
    collect.now_ms = now_ms;
    collect.terminal_retention_ms = ledger->terminal_retention_ms;
    rc = ledger->store->scan(ledger->store->ctx, collect_visit, &collect);
    request->retention_result.selected = collect.count;
    request->retention_result.remaining_terminal = collect.terminal_total;
    for (i = 0u; rc == SALTS_OK && i < collect.count; ++i) {
        iris_record_mutation_t mutation =
            IRIS_RECORD_MUTATION_INIT;
        mutation.kind = IRIS_RECORD_DELETE;
        mutation.key = (const uint8_t *)items[i].record.command_id;
        mutation.key_size = strlen(items[i].record.command_id);
        mutation.expected_revision = items[i].revision;
        rc = ledger->store->commit(ledger->store->ctx, &mutation, 1u);
        if (rc == SALTS_OK) {
            request->retention_result.deleted++;
            request->retention_result.remaining_terminal--;
        }
    }
    for (i = 0u; i < collect.count; ++i) {
        IrisProviderCommandRecordV2_clear(&items[i].record);
    }
    free(items);
    stats_add(ledger, &ledger->stats.retention_sweeps_total, 1u);
    if (rc != SALTS_OK) {
        stats_add(ledger, &ledger->stats.retention_failures_total, 1u);
        return storage_failure(ledger);
    }
    stats_add(ledger, &ledger->stats.retained_deleted_total,
              request->retention_result.deleted);
    return IVR_OK;
}

static ivr_status_t process_abort(iris_command_ledger_t *ledger,
                                  ledger_request_t *request) {
    ledger_candidate_t candidate;
    iris_record_mutation_t mutation = IRIS_RECORD_MUTATION_INIT;
    int found = 0;
    int rc = find_record(ledger, request->identity.command_id, &candidate,
                         &found);
    if (rc != SALTS_OK) return storage_failure(ledger);
    if (!found ||
        !record_identity_matches(&candidate.record, &request->identity) ||
        strcmp(candidate.record.command_state, IRIS_COMMAND_STATE_INTENT) !=
            0) {
        if (found) IrisProviderCommandRecordV2_clear(&candidate.record);
        return IVR_ESTATE;
    }
    mutation.kind = IRIS_RECORD_DELETE;
    mutation.key = (const uint8_t *)candidate.record.command_id;
    mutation.key_size = strlen(candidate.record.command_id);
    mutation.expected_revision = candidate.revision;
    rc = ledger->store->commit(ledger->store->ctx, &mutation, 1u);
    IrisProviderCommandRecordV2_clear(&candidate.record);
    return rc == SALTS_OK ? IVR_OK : storage_failure(ledger);
}

static ivr_status_t process_request(iris_command_ledger_t *ledger,
                                    ledger_request_t *request) {
    switch (request->kind) {
        case LEDGER_REQUEST_CLAIM:
            return process_claim(ledger, request);
        case LEDGER_REQUEST_ACCEPTED:
            return process_settle(ledger, request,
                                  IRIS_COMMAND_STATE_ACCEPTED);
        case LEDGER_REQUEST_TERMINAL:
            return process_settle(ledger, request,
                                  IRIS_COMMAND_STATE_TERMINAL);
        case LEDGER_REQUEST_UNKNOWN:
            return process_settle(ledger, request,
                                  IRIS_COMMAND_STATE_UNKNOWN);
        case LEDGER_REQUEST_RESOURCE_SEEN:
            return process_resource_seen(ledger, request);
        case LEDGER_REQUEST_ABORT:
            return process_abort(ledger, request);
        case LEDGER_REQUEST_RETENTION:
            return process_retention(ledger, request);
        default:
            return IVR_EINVAL;
    }
}

static void complete_request(ledger_request_t *request, ivr_status_t result) {
    salts_mutex_lock(&request->mutex);
    request->result = result;
    request->completed = 1;
    salts_cond_signal(&request->condition);
    salts_mutex_unlock(&request->mutex);
}

static void reset_sweep_deadline(iris_command_ledger_t *ledger) {
    uint64_t now_ms = salts_monotonic_ms();
    if (UINT64_MAX - now_ms < ledger->retention_sweep_interval_ms) {
        ledger->next_sweep_monotonic_ms = UINT64_MAX;
    } else {
        ledger->next_sweep_monotonic_ms =
            now_ms + ledger->retention_sweep_interval_ms;
    }
}

static void ledger_thread(void *context) {
    iris_command_ledger_t *ledger = (iris_command_ledger_t *)context;
    int startup_result = normalize_interrupted_intents(ledger);
    ledger_request_t retention_request;
    memset(&retention_request, 0, sizeof(retention_request));
    retention_request.kind = LEDGER_REQUEST_RETENTION;
    if (startup_result == SALTS_OK) {
        startup_result = process_retention(ledger, &retention_request);
    }
    salts_mutex_lock(&ledger->mutex);
    ledger->startup_result = startup_result;
    ledger->startup_completed = 1;
    ledger->accepting = startup_result == SALTS_OK;
    salts_cond_broadcast(&ledger->startup);
    salts_mutex_unlock(&ledger->mutex);
    if (startup_result != SALTS_OK) return;
    reset_sweep_deadline(ledger);
    for (;;) {
        ledger_request_t *request;
        salts_mutex_lock(&ledger->mutex);
        while (ledger->count == 0u && ledger->running) {
            uint64_t now_ms = salts_monotonic_ms();
            uint64_t wait_ms;
            if (now_ms >= ledger->next_sweep_monotonic_ms) break;
            wait_ms = ledger->next_sweep_monotonic_ms - now_ms;
            if (wait_ms > ledger->retention_sweep_interval_ms) {
                wait_ms = ledger->retention_sweep_interval_ms;
            }
            (void)salts_cond_timedwait(&ledger->not_empty, &ledger->mutex,
                                       wait_ms * UINT64_C(1000000));
        }
        if (ledger->count == 0u && !ledger->running) {
            salts_mutex_unlock(&ledger->mutex);
            break;
        }
        if (ledger->running &&
            salts_monotonic_ms() >= ledger->next_sweep_monotonic_ms) {
            salts_mutex_unlock(&ledger->mutex);
            memset(&retention_request, 0, sizeof(retention_request));
            retention_request.kind = LEDGER_REQUEST_RETENTION;
            (void)process_retention(ledger, &retention_request);
            reset_sweep_deadline(ledger);
            continue;
        }
        request = ledger->requests[ledger->head];
        ledger->requests[ledger->head] = NULL;
        ledger->head = (ledger->head + 1u) % ledger->capacity;
        ledger->count--;
        ledger->stats.request_queue_items = ledger->count;
        salts_mutex_unlock(&ledger->mutex);
        complete_request(request, process_request(ledger, request));
        if (request->kind == LEDGER_REQUEST_RETENTION) {
            reset_sweep_deadline(ledger);
        }
    }
}

static void request_init(ledger_request_t *request,
                         ledger_request_kind_t kind) {
    memset(request, 0, sizeof(*request));
    request->kind = kind;
    salts_mutex_init(&request->mutex);
    salts_cond_init(&request->condition);
}

static void request_clear(ledger_request_t *request) {
    salts_cond_destroy(&request->condition);
    salts_mutex_destroy(&request->mutex);
}

static ivr_status_t submit(iris_command_ledger_t *ledger,
                           ledger_request_t *request) {
    ivr_status_t result;
    salts_mutex_lock(&ledger->mutex);
    if (!ledger->accepting) {
        salts_mutex_unlock(&ledger->mutex);
        return IVR_ECLOSED;
    }
    if (ledger->count == ledger->capacity) {
        ledger->stats.queue_rejections_total++;
        salts_mutex_unlock(&ledger->mutex);
        return IVR_ENOSPC;
    }
    ledger->requests[ledger->tail] = request;
    ledger->tail = (ledger->tail + 1u) % ledger->capacity;
    ledger->count++;
    ledger->stats.request_queue_items = ledger->count;
    if (ledger->count > ledger->stats.request_queue_high_water) {
        ledger->stats.request_queue_high_water = ledger->count;
    }
    salts_cond_signal(&ledger->not_empty);
    salts_mutex_unlock(&ledger->mutex);
    salts_mutex_lock(&request->mutex);
    while (!request->completed) {
        salts_cond_wait(&request->condition, &request->mutex);
    }
    result = request->result;
    salts_mutex_unlock(&request->mutex);
    return result;
}

iris_command_ledger_t *iris_command_ledger_create(
    const iris_command_ledger_config_t *config) {
    iris_command_ledger_t *ledger;
    DataBindError error = DATA_BIND_ERROR_INIT;
    if (!config || !config->store || config->request_queue_capacity == 0u ||
        config->retention_batch_size == 0u ||
        config->terminal_retention_ms == 0u ||
        config->retention_sweep_interval_ms == 0u ||
        config->retention_sweep_interval_ms >
            UINT64_MAX / UINT64_C(1000000) ||
        config->request_queue_capacity > SIZE_MAX / sizeof(void *) ||
        (config->store->capabilities & IRIS_RECORD_STORE_DURABLE) == 0u ||
        (config->store->capabilities &
         IRIS_RECORD_STORE_ATOMIC_BATCH) == 0u ||
        !config->store->scan || !config->store->commit ||
        config->store->max_key_size < IRIS_COMMAND_ID_CAPACITY - 1u ||
        config->store->max_value_size < IRIS_COMMAND_LEDGER_MIN_VALUE_SIZE ||
        config->store->max_records == 0u) {
        return NULL;
    }
    ledger = (iris_command_ledger_t *)calloc(1u, sizeof(*ledger));
    if (!ledger) return NULL;
    ledger->requests = (ledger_request_t **)calloc(
        config->request_queue_capacity, sizeof(*ledger->requests));
    if (!ledger->requests ||
        IrisCommandLedgerV2_codec_create(&ledger->codec, &error) !=
            DATA_BIND_OK ||
        !ledger->codec) {
        data_bind_free(ledger->codec);
        free(ledger->requests);
        free(ledger);
        return NULL;
    }
    ledger->store = config->store;
    ledger->capacity = config->request_queue_capacity;
    ledger->retention_batch_size = config->retention_batch_size;
    ledger->terminal_retention_ms = config->terminal_retention_ms;
    ledger->retention_sweep_interval_ms =
        config->retention_sweep_interval_ms;
    ledger->realtime_ms = config->realtime_ms
                                  ? config->realtime_ms
                                  : system_realtime_ms;
    ledger->realtime_context = config->realtime_context;
    ledger->stats.request_queue_capacity = config->request_queue_capacity;
    ledger->stats.record_capacity = config->store->max_records;
    salts_mutex_init(&ledger->mutex);
    salts_cond_init(&ledger->not_empty);
    salts_cond_init(&ledger->startup);
    return ledger;
}

iris_command_ledger_t *iris_command_ledger_create_record_store(
    const char *yaml_path, const char *channel_name,
    size_t request_queue_capacity,
    size_t retention_batch_size, uint64_t terminal_retention_ms,
    uint64_t retention_sweep_interval_ms,
    char *error, size_t error_capacity) {
    iris_orm_store_owner_t *owner;
    iris_command_ledger_config_t config;
    iris_command_ledger_t *ledger;
    owner = iris_orm_store_owner_create(yaml_path, channel_name, error,
                                        error_capacity);
    if (!owner) return NULL;
    memset(&config, 0, sizeof(config));
    config.request_queue_capacity = request_queue_capacity;
    config.retention_batch_size = retention_batch_size;
    config.terminal_retention_ms = terminal_retention_ms;
    config.retention_sweep_interval_ms = retention_sweep_interval_ms;
    config.store = iris_orm_store_owner_store(owner);
    ledger = iris_command_ledger_create(&config);
    if (!ledger) {
        iris_orm_store_owner_destroy(owner);
        return NULL;
    }
    ledger->orm_store_owner = owner;
    return ledger;
}

int iris_command_ledger_start(iris_command_ledger_t *ledger) {
    int startup_result;
    if (!ledger || ledger->thread_started) return SALTS_EINVAL;
    salts_mutex_lock(&ledger->mutex);
    ledger->running = 1;
    ledger->accepting = 0;
    ledger->startup_completed = 0;
    ledger->startup_result = SALTS_EINVAL;
    salts_mutex_unlock(&ledger->mutex);
    if (salts_thread_create(&ledger->thread, ledger_thread, ledger) !=
        SALTS_OK) {
        salts_mutex_lock(&ledger->mutex);
        ledger->running = 0;
        salts_mutex_unlock(&ledger->mutex);
        return SALTS_EIO;
    }
    ledger->thread_started = 1;
    salts_mutex_lock(&ledger->mutex);
    while (!ledger->startup_completed) {
        salts_cond_wait(&ledger->startup, &ledger->mutex);
    }
    startup_result = ledger->startup_result;
    if (startup_result != SALTS_OK) ledger->running = 0;
    salts_mutex_unlock(&ledger->mutex);
    if (startup_result != SALTS_OK) {
        salts_thread_join(&ledger->thread);
        salts_thread_destroy(&ledger->thread);
        ledger->thread_started = 0;
    }
    return startup_result;
}

void iris_command_ledger_stop(iris_command_ledger_t *ledger) {
    if (!ledger || !ledger->thread_started) return;
    salts_mutex_lock(&ledger->mutex);
    ledger->accepting = 0;
    ledger->running = 0;
    salts_cond_broadcast(&ledger->not_empty);
    salts_mutex_unlock(&ledger->mutex);
    salts_thread_join(&ledger->thread);
    salts_thread_destroy(&ledger->thread);
    ledger->thread_started = 0;
}

void iris_command_ledger_destroy(iris_command_ledger_t *ledger) {
    if (!ledger) return;
    iris_command_ledger_stop(ledger);
    salts_cond_destroy(&ledger->startup);
    salts_cond_destroy(&ledger->not_empty);
    salts_mutex_destroy(&ledger->mutex);
    data_bind_free(ledger->codec);
    free(ledger->requests);
    iris_orm_store_owner_destroy(ledger->orm_store_owner);
    free(ledger);
}

ivr_status_t iris_command_ledger_claim(
    iris_command_ledger_t *ledger, const iris_command_identity_t *identity,
    iris_command_claim_result_t *result) {
    ledger_request_t request;
    ivr_status_t status;
    if (!ledger || !identity_valid(identity) || !result) return IVR_EINVAL;
    request_init(&request, LEDGER_REQUEST_CLAIM);
    request.identity = *identity;
    status = submit(ledger, &request);
    if (status == IVR_OK) *result = request.claim_result;
    request_clear(&request);
    return status;
}

ivr_status_t iris_command_ledger_commit_accepted(
    iris_command_ledger_t *ledger, const iris_command_identity_t *identity,
    const char *provider_resource_id) {
    ledger_request_t request;
    ivr_status_t status;
    if (!ledger || !identity_valid(identity) || !provider_resource_id ||
        provider_resource_id[0] == '\0' ||
        strlen(provider_resource_id) >= IRIS_COMMAND_RESOURCE_ID_CAPACITY) {
        return IVR_EINVAL;
    }
    request_init(&request, LEDGER_REQUEST_ACCEPTED);
    request.identity = *identity;
    (void)copy_text(request.provider_resource_id,
                    sizeof(request.provider_resource_id), provider_resource_id,
                    1);
    status = submit(ledger, &request);
    request_clear(&request);
    return status;
}

ivr_status_t iris_command_ledger_commit_terminal(
    iris_command_ledger_t *ledger, const iris_command_identity_t *identity,
    const iris_command_terminal_outcome_t *outcome) {
    ledger_request_t request;
    ivr_status_t status;
    if (!ledger || !identity_valid(identity) || !outcome ||
        !terminal_status_valid(outcome->terminal_status) ||
        outcome->event_type[0] == '\0' || outcome->result_json[0] == '\0') {
        return IVR_EINVAL;
    }
    request_init(&request, LEDGER_REQUEST_TERMINAL);
    request.identity = *identity;
    request.terminal = *outcome;
    status = submit(ledger, &request);
    request_clear(&request);
    return status;
}

ivr_status_t iris_command_ledger_mark_unknown(
    iris_command_ledger_t *ledger, const iris_command_identity_t *identity) {
    ledger_request_t request;
    ivr_status_t status;
    if (!ledger || !identity_valid(identity)) return IVR_EINVAL;
    request_init(&request, LEDGER_REQUEST_UNKNOWN);
    request.identity = *identity;
    status = submit(ledger, &request);
    request_clear(&request);
    return status;
}

ivr_status_t iris_command_ledger_resource_seen(
    iris_command_ledger_t *ledger, const iris_command_identity_t *identity,
    int *seen) {
    ledger_request_t request;
    ivr_status_t status;
    if (!ledger || !identity_valid(identity) || !seen) return IVR_EINVAL;
    request_init(&request, LEDGER_REQUEST_RESOURCE_SEEN);
    request.identity = *identity;
    status = submit(ledger, &request);
    if (status == IVR_OK) *seen = request.resource_seen;
    request_clear(&request);
    return status;
}

ivr_status_t iris_command_ledger_abort_intent(
    iris_command_ledger_t *ledger, const iris_command_identity_t *identity) {
    ledger_request_t request;
    ivr_status_t status;
    if (!ledger || !identity_valid(identity)) return IVR_EINVAL;
    request_init(&request, LEDGER_REQUEST_ABORT);
    request.identity = *identity;
    status = submit(ledger, &request);
    request_clear(&request);
    return status;
}

ivr_status_t iris_command_ledger_run_retention(
    iris_command_ledger_t *ledger, iris_command_retention_result_t *result) {
    ledger_request_t request;
    ivr_status_t status;
    if (!ledger || !result) return IVR_EINVAL;
    request_init(&request, LEDGER_REQUEST_RETENTION);
    status = submit(ledger, &request);
    if (status == IVR_OK) *result = request.retention_result;
    request_clear(&request);
    return status;
}

void iris_command_ledger_get_stats(iris_command_ledger_t *ledger,
                                   iris_command_ledger_stats_t *stats) {
    if (!ledger || !stats) return;
    salts_mutex_lock(&ledger->mutex);
    *stats = ledger->stats;
    salts_mutex_unlock(&ledger->mutex);
}

static ivr_status_t port_claim(
    void *context, const iris_command_identity_t *identity,
    iris_command_claim_result_t *result) {
    return iris_command_ledger_claim((iris_command_ledger_t *)context,
                                     identity, result);
}

static ivr_status_t port_commit_accepted(
    void *context, const iris_command_identity_t *identity,
    const char *provider_resource_id) {
    return iris_command_ledger_commit_accepted(
        (iris_command_ledger_t *)context, identity, provider_resource_id);
}

static ivr_status_t port_commit_terminal(
    void *context, const iris_command_identity_t *identity,
    const iris_command_terminal_outcome_t *outcome) {
    return iris_command_ledger_commit_terminal(
        (iris_command_ledger_t *)context, identity, outcome);
}

static ivr_status_t port_mark_unknown(
    void *context, const iris_command_identity_t *identity) {
    return iris_command_ledger_mark_unknown((iris_command_ledger_t *)context,
                                            identity);
}

static ivr_status_t port_resource_seen(
    void *context, const iris_command_identity_t *identity, int *seen) {
    return iris_command_ledger_resource_seen(
        (iris_command_ledger_t *)context, identity, seen);
}

static ivr_status_t port_abort_intent(
    void *context, const iris_command_identity_t *identity) {
    return iris_command_ledger_abort_intent(
        (iris_command_ledger_t *)context, identity);
}

iris_command_ledger_port_t iris_command_ledger_port(
    iris_command_ledger_t *ledger) {
    iris_command_ledger_port_t port;
    memset(&port, 0, sizeof(port));
    if (!ledger) return port;
    port.context = ledger;
    port.claim = port_claim;
    port.commit_accepted = port_commit_accepted;
    port.commit_terminal = port_commit_terminal;
    port.mark_unknown = port_mark_unknown;
    port.resource_seen = port_resource_seen;
    port.abort_intent = port_abort_intent;
    return port;
}
