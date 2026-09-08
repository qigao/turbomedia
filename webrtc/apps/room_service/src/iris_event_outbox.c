#include "iris_event_outbox.h"

#include "iris_event_outbox_v1.h"
#include "iris_orm_store.h"
#include "platform.h"
#include "salts_error.h"
#include "salts_thread.h"
#include "salts_str.h"
#include "tlog.h"
#include <salts/clock.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IRIS_OUTBOX_SCHEMA_VERSION 4u
#define IRIS_OUTBOX_STATE_PENDING "pending"
#define IRIS_OUTBOX_STATE_IN_FLIGHT "in_flight"
#define IRIS_OUTBOX_STATE_DEAD "dead"
#define IRIS_OUTBOX_STATE_ARCHIVED "archived"
#define IRIS_OUTBOX_EVENT_ID_CAPACITY 128u
#define IRIS_OUTBOX_MIN_VALUE_SIZE 16384u
#define IRIS_OUTBOX_MIN_SWEEP_INTERVAL_MS 1000u

typedef enum iris_outbox_request_kind_e {
    IRIS_OUTBOX_PERSIST = 1,
    IRIS_OUTBOX_SETTLE,
    IRIS_OUTBOX_REPLAY,
    IRIS_OUTBOX_REPLAY_DEAD_BATCH,
    IRIS_OUTBOX_LIST_DEAD,
    IRIS_OUTBOX_LIST_ARCHIVED,
    IRIS_OUTBOX_RUN_RETENTION
} iris_outbox_request_kind_t;

typedef struct iris_outbox_candidate_s iris_outbox_candidate_t;

typedef struct iris_outbox_request_s {
    iris_outbox_request_kind_t kind;
    ivr_media_event_t event;
    char event_id[IRIS_OUTBOX_EVENT_ID_CAPACITY];
    uint64_t revision;
    int succeeded;
    int http_status;
    iris_event_dead_letter_t *dead_letters;
    size_t dead_letter_capacity;
    size_t dead_letter_count;
    size_t dead_letter_total;
    iris_event_archive_t *archives;
    size_t archive_capacity;
    size_t archive_count;
    size_t archive_total;
    iris_outbox_candidate_t *replay_candidates;
    size_t replay_candidate_capacity;
    iris_event_replay_batch_result_t replay_batch_result;
    iris_event_retention_result_t retention_result;
    int result;
    int completed;
    salts_mutex_t mutex;
    salts_cond_t condition;
} iris_outbox_request_t;

struct iris_outbox_candidate_s {
    ivr_media_event_t event;
    uint64_t revision;
    uint32_t attempts;
    int http_status;
    uint64_t state_changed_at_ms;
    uint64_t archived_at_ms;
    char state[16];
};

typedef struct iris_outbox_scan_s {
    DataBind *codec;
    iris_outbox_candidate_t *items;
    size_t capacity;
    size_t count;
    const char *event_id;
    int only_in_flight;
    int only_pending;
    int only_dead;
    int decode_status;
} iris_outbox_scan_t;

struct iris_event_outbox_s {
    iris_record_store_t *store;
    iris_orm_store_owner_t *orm_store_owner;
    DataBind *codec;
    iris_event_outbox_deliver_fn deliver;
    void *deliver_context;
    iris_event_outbox_retention_config_t retention;
    uint64_t next_sweep_monotonic_ms;
    int retention_delete_first;
    iris_outbox_request_t **requests;
    size_t capacity;
    size_t head;
    size_t count;
    int accepting;
    int running;
    int thread_started;
    int startup_completed;
    int startup_result;
    salts_mutex_t mutex;
    salts_cond_t not_empty;
    salts_cond_t startup;
    salts_thread_t thread;
    iris_event_outbox_stats_t stats;
};

static int decode_record(DataBind *codec, const uint8_t *value,
                         size_t value_size,
                         IrisMediaEventOutboxRecordV1_t *record);

static void stats_increment(iris_event_outbox_t *outbox, uint64_t *value) {
    salts_mutex_lock(&outbox->mutex);
    (*value)++;
    salts_mutex_unlock(&outbox->mutex);
}

static void stats_transition(iris_event_outbox_t *outbox, size_t *from,
                             size_t *to) {
    salts_mutex_lock(&outbox->mutex);
    if (from && *from > 0u) (*from)--;
    if (to) (*to)++;
    salts_mutex_unlock(&outbox->mutex);
}

static void stats_retain_payload(iris_event_outbox_t *outbox,
                                 size_t payload_bytes) {
    salts_mutex_lock(&outbox->mutex);
    outbox->stats.retained_payload_bytes += payload_bytes;
    if (outbox->stats.retained_payload_bytes >
        outbox->stats.peak_retained_payload_bytes) {
        outbox->stats.peak_retained_payload_bytes =
            outbox->stats.retained_payload_bytes;
    }
    salts_mutex_unlock(&outbox->mutex);
}

static void stats_release_payload(iris_event_outbox_t *outbox,
                                  size_t payload_bytes) {
    salts_mutex_lock(&outbox->mutex);
    if (outbox->stats.retained_payload_bytes >= payload_bytes) {
        outbox->stats.retained_payload_bytes -= payload_bytes;
    } else {
        outbox->stats.retained_payload_bytes = 0u;
    }
    salts_mutex_unlock(&outbox->mutex);
}

typedef struct iris_outbox_count_s {
    DataBind *codec;
    size_t pending;
    size_t in_flight;
    size_t dead;
    size_t archived;
    size_t payload_bytes;
} iris_outbox_count_t;

static int count_visit(void *context,
                       const iris_record_view_t *view) {
    iris_outbox_count_t *count = (iris_outbox_count_t *)context;
    IrisMediaEventOutboxRecordV1_t record;
    int rc = decode_record(count->codec, view->value, view->value_size,
                           &record);
    if (rc != SALTS_OK) return rc;
    {
        size_t payload_bytes = record.payload_json
                                   ? strlen(record.payload_json)
                                   : 0u;
        if (SIZE_MAX - count->payload_bytes < payload_bytes) {
            IrisMediaEventOutboxRecordV1_clear(&record);
            return SALTS_ENOSPC;
        }
        count->payload_bytes += payload_bytes;
    }
    if (strcmp(record.delivery_state, IRIS_OUTBOX_STATE_PENDING) == 0) {
        count->pending++;
    } else if (strcmp(record.delivery_state,
                      IRIS_OUTBOX_STATE_IN_FLIGHT) == 0) {
        count->in_flight++;
    } else if (strcmp(record.delivery_state, IRIS_OUTBOX_STATE_DEAD) == 0) {
        count->dead++;
    } else if (strcmp(record.delivery_state, IRIS_OUTBOX_STATE_ARCHIVED) == 0) {
        count->archived++;
    } else {
        rc = SALTS_EPROTO;
    }
    IrisMediaEventOutboxRecordV1_clear(&record);
    return rc;
}

static int recount_states(iris_event_outbox_t *outbox) {
    iris_outbox_count_t count;
    int rc;
    memset(&count, 0, sizeof(count));
    count.codec = outbox->codec;
    rc = outbox->store->scan(outbox->store->ctx, count_visit, &count);
    if (rc != SALTS_OK) {
        if (rc == SALTS_EPROTO) {
            stats_increment(outbox, &outbox->stats.decode_failure_total);
        }
        return rc;
    }
    salts_mutex_lock(&outbox->mutex);
    outbox->stats.pending_records = count.pending;
    outbox->stats.in_flight_records = count.in_flight;
    outbox->stats.dead_records = count.dead;
    outbox->stats.archived_records = count.archived;
    outbox->stats.retained_payload_bytes = count.payload_bytes;
    if (count.payload_bytes > outbox->stats.peak_retained_payload_bytes) {
        outbox->stats.peak_retained_payload_bytes = count.payload_bytes;
    }
    salts_mutex_unlock(&outbox->mutex);
    return SALTS_OK;
}

static void copy_text(char *dest, size_t capacity, const char *source) {
    if (!dest || capacity == 0u) return;
    (void)snprintf(dest, capacity, "%s", source ? source : "");
}

static uint64_t outbox_realtime_ms(const iris_event_outbox_t *outbox) {
    return outbox->retention.realtime_ms
               ? outbox->retention.realtime_ms(
                     outbox->retention.realtime_context)
               : salts_realtime_ms();
}

static int retention_expired(uint64_t changed_at_ms, uint64_t ttl_ms,
                             uint64_t now_ms) {
    return changed_at_ms > 0u && now_ms >= changed_at_ms &&
           now_ms - changed_at_ms >= ttl_ms;
}

static int fixed_text_valid(const char *value, size_t capacity) {
    return value && memchr(value, '\0', capacity) != NULL;
}

static int event_valid(const ivr_media_event_t *event) {
    return event && fixed_text_valid(event->event_id, sizeof(event->event_id)) &&
           event->event_id[0] &&
           strlen(event->event_id) < IRIS_OUTBOX_EVENT_ID_CAPACITY &&
           fixed_text_valid(event->tenant_id, sizeof(event->tenant_id)) &&
           event->tenant_id[0] && event->sequence != 0u &&
           fixed_text_valid(event->provider_session_id,
                            sizeof(event->provider_session_id)) &&
           event->provider_session_id[0] &&
           fixed_text_valid(event->dialog_id, sizeof(event->dialog_id)) &&
           event->dialog_id[0] &&
           fixed_text_valid(event->worker_id, sizeof(event->worker_id)) &&
           fixed_text_valid(event->room_id, sizeof(event->room_id)) &&
           fixed_text_valid(event->call_id, sizeof(event->call_id)) &&
           fixed_text_valid(event->event_type, sizeof(event->event_type)) &&
           event->event_type[0] &&
           fixed_text_valid(event->input_id, sizeof(event->input_id)) &&
           fixed_text_valid(event->input_value, sizeof(event->input_value)) &&
           fixed_text_valid(event->payload_json, sizeof(event->payload_json));
}

static int set_owned(tstr *field, const char *value) {
    tstr next;
    if (!field) return 0;
    next = tstr_cpy(*field, value ? value : "");
    if (!next) return 0;
    *field = next;
    return 1;
}

static int record_from_event(IrisMediaEventOutboxRecordV1_t *record,
                             const ivr_media_event_t *event,
                             const char *state, uint32_t attempts,
                             int http_status, uint64_t state_changed_at_ms,
                             uint64_t archived_at_ms) {
    IrisMediaEventOutboxRecordV1_init(record);
    record->schema_version = IRIS_OUTBOX_SCHEMA_VERSION;
    record->delivery_attempts = attempts;
    record->last_http_status = http_status;
    record->state_changed_at_ms = state_changed_at_ms;
    record->archived_at_ms = archived_at_ms;
    record->call_generation = event->call_generation;
    record->sequence = event->sequence;
    record->occurred_at_ms = event->occurred_at_ms;
    if (!set_owned(&record->delivery_state, state) ||
        !set_owned(&record->event_id, event->event_id) ||
        !set_owned(&record->tenant_id, event->tenant_id) ||
        !set_owned(&record->provider_session_id, event->provider_session_id) ||
        !set_owned(&record->dialog_id, event->dialog_id) ||
        !set_owned(&record->worker_id, event->worker_id) ||
        !set_owned(&record->room_id, event->room_id) ||
        !set_owned(&record->call_id, event->call_id) ||
        !set_owned(&record->event_type, event->event_type) ||
        !set_owned(&record->input_id, event->input_id) ||
        !set_owned(&record->input_value, event->input_value) ||
        !set_owned(&record->payload_json, event->payload_json)) {
        IrisMediaEventOutboxRecordV1_clear(record);
        return SALTS_ENOMEM;
    }
    return SALTS_OK;
}

static void event_from_record(const IrisMediaEventOutboxRecordV1_t *record,
                              ivr_media_event_t *event) {
    memset(event, 0, sizeof(*event));
    copy_text(event->event_id, sizeof(event->event_id), record->event_id);
    copy_text(event->tenant_id, sizeof(event->tenant_id), record->tenant_id);
    copy_text(event->provider_session_id, sizeof(event->provider_session_id),
              record->provider_session_id);
    copy_text(event->dialog_id, sizeof(event->dialog_id), record->dialog_id);
    copy_text(event->worker_id, sizeof(event->worker_id), record->worker_id);
    copy_text(event->room_id, sizeof(event->room_id), record->room_id);
    copy_text(event->call_id, sizeof(event->call_id), record->call_id);
    event->call_generation = record->call_generation;
    event->sequence = record->sequence;
    copy_text(event->event_type, sizeof(event->event_type), record->event_type);
    event->occurred_at_ms = record->occurred_at_ms;
    copy_text(event->input_id, sizeof(event->input_id), record->input_id);
    copy_text(event->input_value, sizeof(event->input_value), record->input_value);
    copy_text(event->payload_json, sizeof(event->payload_json), record->payload_json);
}

static int event_equal(const ivr_media_event_t *left,
                       const ivr_media_event_t *right) {
    return left->call_generation == right->call_generation &&
           left->sequence == right->sequence &&
           left->occurred_at_ms == right->occurred_at_ms &&
           strcmp(left->event_id, right->event_id) == 0 &&
           strcmp(left->tenant_id, right->tenant_id) == 0 &&
           strcmp(left->provider_session_id, right->provider_session_id) == 0 &&
           strcmp(left->dialog_id, right->dialog_id) == 0 &&
           strcmp(left->worker_id, right->worker_id) == 0 &&
           strcmp(left->room_id, right->room_id) == 0 &&
           strcmp(left->call_id, right->call_id) == 0 &&
           strcmp(left->event_type, right->event_type) == 0 &&
           strcmp(left->input_id, right->input_id) == 0 &&
           strcmp(left->input_value, right->input_value) == 0 &&
           strcmp(left->payload_json, right->payload_json) == 0;
}

static int encode_record(DataBind *codec,
                         const IrisMediaEventOutboxRecordV1_t *record,
                         uint8_t **value, size_t *value_size) {
    DataBindError error = DATA_BIND_ERROR_INIT;
    char *json = NULL;
    size_t json_size = 0u;
    DataBindStatus status = IrisMediaEventOutboxRecordV1_to_json(
        codec, record, &json, &json_size, &error);
    if (status != DATA_BIND_OK || !json) return SALTS_EPROTO;
    *value = (uint8_t *)json;
    *value_size = json_size;
    return SALTS_OK;
}

static int record_text_fits(const char *value, size_t capacity) {
    return !value || strlen(value) < capacity;
}

static int decode_record(DataBind *codec, const uint8_t *value,
                         size_t value_size,
                         IrisMediaEventOutboxRecordV1_t *record) {
    DataBindError error = DATA_BIND_ERROR_INIT;
    IrisMediaEventOutboxRecordV1_init(record);
    if (IrisMediaEventOutboxRecordV1_from_json(
            codec, record, (const char *)value, value_size, &error) !=
            DATA_BIND_OK ||
        record->schema_version != IRIS_OUTBOX_SCHEMA_VERSION ||
        !record->event_id || !record->event_id[0] ||
        !record->tenant_id || !record->tenant_id[0] ||
        record->sequence == 0u ||
        !record->provider_session_id || !record->provider_session_id[0] ||
        !record->dialog_id || !record->dialog_id[0] ||
        !record->event_type || !record->event_type[0] ||
        !record_text_fits(record->event_id,
                          sizeof(((ivr_media_event_t *)0)->event_id)) ||
        !record_text_fits(record->tenant_id,
                          sizeof(((ivr_media_event_t *)0)->tenant_id)) ||
        !record_text_fits(record->provider_session_id,
                          sizeof(((ivr_media_event_t *)0)->provider_session_id)) ||
        !record_text_fits(record->dialog_id,
                          sizeof(((ivr_media_event_t *)0)->dialog_id)) ||
        !record_text_fits(record->worker_id,
                          sizeof(((ivr_media_event_t *)0)->worker_id)) ||
        !record_text_fits(record->room_id,
                          sizeof(((ivr_media_event_t *)0)->room_id)) ||
        !record_text_fits(record->call_id,
                          sizeof(((ivr_media_event_t *)0)->call_id)) ||
        !record_text_fits(record->event_type,
                          sizeof(((ivr_media_event_t *)0)->event_type)) ||
        !record_text_fits(record->input_id,
                          sizeof(((ivr_media_event_t *)0)->input_id)) ||
        !record_text_fits(record->input_value,
                          sizeof(((ivr_media_event_t *)0)->input_value)) ||
        !record_text_fits(record->payload_json,
                          sizeof(((ivr_media_event_t *)0)->payload_json)) ||
        !record->delivery_state ||
        (strcmp(record->delivery_state, IRIS_OUTBOX_STATE_PENDING) != 0 &&
         strcmp(record->delivery_state, IRIS_OUTBOX_STATE_IN_FLIGHT) != 0 &&
         strcmp(record->delivery_state, IRIS_OUTBOX_STATE_DEAD) != 0 &&
         strcmp(record->delivery_state, IRIS_OUTBOX_STATE_ARCHIVED) != 0) ||
        record->state_changed_at_ms == 0u ||
        ((strcmp(record->delivery_state, IRIS_OUTBOX_STATE_ARCHIVED) == 0) !=
         (record->archived_at_ms > 0u))) {
        IrisMediaEventOutboxRecordV1_clear(record);
        return SALTS_EPROTO;
    }
    return SALTS_OK;
}

static int commit_record(iris_event_outbox_t *outbox,
                         const ivr_media_event_t *event, const char *state,
                         uint32_t attempts, int http_status,
                         uint64_t expected_revision,
                         uint64_t next_revision) {
    IrisMediaEventOutboxRecordV1_t record;
    iris_record_mutation_t mutation = IRIS_RECORD_MUTATION_INIT;
    uint8_t *value = NULL;
    size_t value_size = 0u;
    uint64_t changed_at_ms = outbox_realtime_ms(outbox);
    uint64_t archived_at_ms =
        strcmp(state, IRIS_OUTBOX_STATE_ARCHIVED) == 0 ? changed_at_ms : 0u;
    int rc;
    if (changed_at_ms == 0u) return SALTS_EINVAL;
    rc = record_from_event(&record, event, state, attempts, http_status,
                           changed_at_ms, archived_at_ms);
    if (rc != SALTS_OK) return rc;
    rc = encode_record(outbox->codec, &record, &value, &value_size);
    if (rc == SALTS_OK) {
        mutation.key = (const uint8_t *)event->event_id;
        mutation.key_size = strlen(event->event_id);
        mutation.expected_revision = expected_revision;
        mutation.next_revision = next_revision;
        mutation.value = value;
        mutation.value_size = value_size;
        rc = outbox->store->commit(outbox->store->ctx, &mutation, 1u);
    }
    tbe_typed_serialized_free(value);
    IrisMediaEventOutboxRecordV1_clear(&record);
    return rc;
}

static int delete_record(iris_event_outbox_t *outbox, const char *event_id,
                         uint64_t revision) {
    iris_record_mutation_t mutation = IRIS_RECORD_MUTATION_INIT;
    mutation.kind = IRIS_RECORD_DELETE;
    mutation.key = (const uint8_t *)event_id;
    mutation.key_size = strlen(event_id);
    mutation.expected_revision = revision;
    return outbox->store->commit(outbox->store->ctx, &mutation, 1u);
}

static int scan_visit(void *context,
                      const iris_record_view_t *view) {
    iris_outbox_scan_t *scan = (iris_outbox_scan_t *)context;
    IrisMediaEventOutboxRecordV1_t record;
    iris_outbox_candidate_t *candidate;
    if (scan->count >= scan->capacity) return SALTS_OK;
    if (scan->event_id &&
        (strlen(scan->event_id) != view->key_size ||
         memcmp(scan->event_id, view->key, view->key_size) != 0)) {
        return SALTS_OK;
    }
    if (decode_record(scan->codec, view->value, view->value_size, &record) !=
        SALTS_OK) {
        scan->decode_status = SALTS_EPROTO;
        return SALTS_EPROTO;
    }
    if ((scan->only_in_flight &&
         strcmp(record.delivery_state, IRIS_OUTBOX_STATE_IN_FLIGHT) != 0) ||
        (scan->only_pending &&
         strcmp(record.delivery_state, IRIS_OUTBOX_STATE_PENDING) != 0) ||
        (scan->only_dead &&
         strcmp(record.delivery_state, IRIS_OUTBOX_STATE_DEAD) != 0)) {
        IrisMediaEventOutboxRecordV1_clear(&record);
        return SALTS_OK;
    }
    candidate = &scan->items[scan->count++];
    memset(candidate, 0, sizeof(*candidate));
    event_from_record(&record, &candidate->event);
    candidate->revision = view->revision;
    candidate->attempts = record.delivery_attempts;
    candidate->http_status = record.last_http_status;
    candidate->state_changed_at_ms = record.state_changed_at_ms;
    candidate->archived_at_ms = record.archived_at_ms;
    copy_text(candidate->state, sizeof(candidate->state),
              record.delivery_state);
    IrisMediaEventOutboxRecordV1_clear(&record);
    return SALTS_OK;
}

static int scan_records(iris_event_outbox_t *outbox,
                        iris_outbox_candidate_t *items, size_t capacity,
                        const char *event_id, int only_in_flight,
                        int only_pending, int only_dead, size_t *count) {
    iris_outbox_scan_t scan;
    int rc;
    memset(&scan, 0, sizeof(scan));
    scan.codec = outbox->codec;
    scan.items = items;
    scan.capacity = capacity;
    scan.event_id = event_id;
    scan.only_in_flight = only_in_flight;
    scan.only_pending = only_pending;
    scan.only_dead = only_dead;
    rc = outbox->store->scan(outbox->store->ctx, scan_visit, &scan);
    *count = scan.count;
    rc = rc == SALTS_OK ? scan.decode_status : rc;
    if (rc == SALTS_EPROTO) {
        stats_increment(outbox, &outbox->stats.decode_failure_total);
    }
    return rc;
}

static int schedule_one(iris_event_outbox_t *outbox,
                        iris_outbox_candidate_t *candidate) {
    uint64_t in_flight_revision;
    int rc;
    if (candidate->revision >= IRIS_RECORD_REVISION_MAX - 1u ||
        candidate->attempts == UINT32_MAX) {
        return SALTS_ENOSPC;
    }
    in_flight_revision = candidate->revision + 1u;
    rc = commit_record(outbox, &candidate->event,
                       IRIS_OUTBOX_STATE_IN_FLIGHT,
                       candidate->attempts + 1u, candidate->http_status,
                       candidate->revision, in_flight_revision);
    if (rc != SALTS_OK) return rc;
    stats_transition(outbox, &outbox->stats.pending_records,
                     &outbox->stats.in_flight_records);
    if (outbox->deliver(outbox->deliver_context, &candidate->event,
                        in_flight_revision) == IVR_OK) {
        stats_increment(outbox, &outbox->stats.scheduled_total);
        return SALTS_OK;
    }
    stats_increment(outbox, &outbox->stats.schedule_rejection_total);
    rc = commit_record(outbox, &candidate->event, IRIS_OUTBOX_STATE_PENDING,
                       candidate->attempts + 1u, candidate->http_status,
                       in_flight_revision, in_flight_revision + 1u);
    if (rc == SALTS_OK) {
        stats_transition(outbox, &outbox->stats.in_flight_records,
                         &outbox->stats.pending_records);
    }
    return rc == SALTS_OK ? SALTS_ENOSPC : rc;
}

static int schedule_pending(iris_event_outbox_t *outbox) {
    size_t capacity = outbox->store->max_batch_size;
    iris_outbox_candidate_t *items;
    size_t count = 0u;
    int rc;
    if (capacity == 0u || capacity > outbox->capacity) capacity = outbox->capacity;
    items = (iris_outbox_candidate_t *)calloc(capacity, sizeof(*items));
    if (!items) return SALTS_ENOMEM;
    do {
        count = 0u;
        rc = scan_records(outbox, items, capacity, NULL, 0, 1, 0, &count);
        for (size_t i = 0u; rc == SALTS_OK && i < count; ++i) {
            rc = schedule_one(outbox, &items[i]);
            if (rc == SALTS_ENOSPC) break;
        }
    } while (rc == SALTS_OK && count == capacity);
    free(items);
    return rc == SALTS_ENOSPC ? SALTS_OK : rc;
}

static int normalize_in_flight(iris_event_outbox_t *outbox) {
    size_t capacity = outbox->store->max_batch_size;
    iris_outbox_candidate_t *items;
    int rc = SALTS_OK;
    if (capacity == 0u || capacity > outbox->capacity) capacity = outbox->capacity;
    items = (iris_outbox_candidate_t *)calloc(capacity, sizeof(*items));
    if (!items) return SALTS_ENOMEM;
    for (;;) {
        size_t count = 0u;
        rc = scan_records(outbox, items, capacity, NULL, 1, 0, 0, &count);
        if (rc != SALTS_OK || count == 0u) break;
        for (size_t i = 0u; i < count; ++i) {
            if (items[i].revision >= IRIS_RECORD_REVISION_MAX) {
                rc = SALTS_ENOSPC;
                break;
            }
            rc = commit_record(outbox, &items[i].event,
                               IRIS_OUTBOX_STATE_PENDING, items[i].attempts,
                               items[i].http_status, items[i].revision,
                               items[i].revision + 1u);
            if (rc != SALTS_OK) break;
            stats_transition(outbox, &outbox->stats.in_flight_records,
                             &outbox->stats.pending_records);
            stats_increment(outbox, &outbox->stats.recovered_total);
        }
        if (rc != SALTS_OK) break;
    }
    free(items);
    return rc;
}

static int process_persist(iris_event_outbox_t *outbox,
                           iris_outbox_request_t *request) {
    iris_outbox_candidate_t existing;
    size_t count = 0u;
    int rc = commit_record(outbox, &request->event,
                           IRIS_OUTBOX_STATE_PENDING, 0u, 0,
                           IRIS_RECORD_REVISION_ABSENT, 1u);
    if (rc == SALTS_OK) {
        memset(&existing, 0, sizeof(existing));
        existing.event = request->event;
        existing.revision = 1u;
        stats_retain_payload(outbox, strlen(request->event.payload_json));
        stats_transition(outbox, NULL, &outbox->stats.pending_records);
        stats_increment(outbox, &outbox->stats.persisted_total);
        rc = schedule_one(outbox, &existing);
        return rc == SALTS_ENOSPC ? SALTS_OK : rc;
    }
    if (rc != SALTS_EBUSY) {
        if (rc == SALTS_ENOSPC) {
            stats_increment(outbox,
                            &outbox->stats.capacity_rejection_total);
        }
        stats_increment(outbox, &outbox->stats.persist_failure_total);
        return rc;
    }
    memset(&existing, 0, sizeof(existing));
    rc = scan_records(outbox, &existing, 1u, request->event.event_id,
                      0, 0, 0, &count);
    if (rc != SALTS_OK || count != 1u) {
        stats_increment(outbox, &outbox->stats.persist_failure_total);
        return rc == SALTS_OK ? SALTS_EPROTO : rc;
    }
    if (!event_equal(&existing.event, &request->event)) {
        stats_increment(outbox, &outbox->stats.conflict_total);
        return SALTS_EBUSY;
    }
    stats_increment(outbox, &outbox->stats.duplicate_total);
    if (strcmp(existing.state, IRIS_OUTBOX_STATE_PENDING) == 0) {
        rc = schedule_one(outbox, &existing);
        if (rc == SALTS_ENOSPC) rc = SALTS_OK;
    }
    return rc;
}

static int process_settle(iris_event_outbox_t *outbox,
                          iris_outbox_request_t *request) {
    int rc;
    if (request->succeeded > 0) {
        rc = delete_record(outbox, request->event.event_id,
                           request->revision);
        if (rc == SALTS_OK) {
            stats_release_payload(outbox,
                                  strlen(request->event.payload_json));
            stats_transition(outbox, &outbox->stats.in_flight_records, NULL);
            stats_increment(outbox, &outbox->stats.delivered_total);
        }
    } else {
        iris_outbox_candidate_t current;
        size_t count = 0u;
        memset(&current, 0, sizeof(current));
        rc = scan_records(outbox, &current, 1u, request->event.event_id,
                          0, 0, 0, &count);
        if (rc == SALTS_OK && count == 1u &&
            current.revision == request->revision &&
            request->revision < IRIS_RECORD_REVISION_MAX) {
            rc = commit_record(
                outbox, &request->event,
                request->succeeded < 0 ? IRIS_OUTBOX_STATE_PENDING
                                       : IRIS_OUTBOX_STATE_DEAD,
                current.attempts, request->http_status, request->revision,
                request->revision + 1u);
        } else if (rc == SALTS_OK) {
            rc = SALTS_EBUSY;
        }
        if (rc == SALTS_OK && request->succeeded == 0) {
            stats_transition(outbox, &outbox->stats.in_flight_records,
                             &outbox->stats.dead_records);
            stats_increment(outbox, &outbox->stats.dead_lettered_total);
        } else if (rc == SALTS_OK && request->succeeded < 0) {
            stats_transition(outbox, &outbox->stats.in_flight_records,
                             &outbox->stats.pending_records);
        }
    }
    if (rc == SALTS_EBUSY) {
        stats_increment(outbox, &outbox->stats.stale_settlement_total);
    }
    if (rc != SALTS_OK) {
        stats_increment(outbox, &outbox->stats.settlement_failure_total);
        TLOG_ERRORF("Iris event outbox settlement failed: event_id={}, status={}. "
                   "The durable record is retained for recovery.",
                   request->event.event_id, rc);
    }
    if (rc == SALTS_OK) (void)schedule_pending(outbox);
    return rc;
}

static int replay_candidate(iris_event_outbox_t *outbox,
                            iris_outbox_candidate_t *candidate,
                            int *replayed, int *backpressured) {
    int rc;
    if (replayed) *replayed = 0;
    if (backpressured) *backpressured = 0;
    if (strcmp(candidate->state, IRIS_OUTBOX_STATE_DEAD) != 0) {
        return SALTS_EBUSY;
    }
    if (candidate->revision >= IRIS_RECORD_REVISION_MAX - 2u ||
        candidate->attempts == UINT32_MAX) {
        return SALTS_ENOSPC;
    }
    rc = commit_record(outbox, &candidate->event, IRIS_OUTBOX_STATE_PENDING,
                       candidate->attempts, 0, candidate->revision,
                       candidate->revision + 1u);
    if (rc != SALTS_OK) return rc;
    stats_transition(outbox, &outbox->stats.dead_records,
                     &outbox->stats.pending_records);
    candidate->revision++;
    candidate->http_status = 0;
    copy_text(candidate->state, sizeof(candidate->state),
              IRIS_OUTBOX_STATE_PENDING);
    stats_increment(outbox, &outbox->stats.replayed_total);
    if (replayed) *replayed = 1;
    rc = schedule_one(outbox, candidate);
    if (rc == SALTS_ENOSPC) {
        if (backpressured) *backpressured = 1;
        return SALTS_OK;
    }
    return rc;
}

static int process_replay(iris_event_outbox_t *outbox,
                          iris_outbox_request_t *request) {
    iris_outbox_candidate_t candidate;
    size_t count = 0u;
    int rc;
    memset(&candidate, 0, sizeof(candidate));
    rc = scan_records(outbox, &candidate, 1u, request->event_id,
                      0, 0, 0, &count);
    if (rc != SALTS_OK || count != 1u) {
        return rc == SALTS_OK ? SALTS_ENOENT : rc;
    }
    return replay_candidate(outbox, &candidate, NULL, NULL);
}

static int process_replay_dead_batch(iris_event_outbox_t *outbox,
                                     iris_outbox_request_t *request) {
    size_t count = 0u;
    size_t i;
    int rc = scan_records(outbox, request->replay_candidates,
                          request->replay_candidate_capacity, NULL,
                          0, 0, 1, &count);
    if (rc != SALTS_OK) return rc;
    request->replay_batch_result.selected = count;
    for (i = 0u; i < count; ++i) {
        int replayed = 0;
        int backpressured = 0;
        rc = replay_candidate(outbox, &request->replay_candidates[i],
                              &replayed, &backpressured);
        if (replayed) request->replay_batch_result.replayed++;
        if (rc != SALTS_OK) break;
        if (backpressured) {
            request->replay_batch_result.backpressured = 1;
            break;
        }
    }
    salts_mutex_lock(&outbox->mutex);
    request->replay_batch_result.remaining_dead = outbox->stats.dead_records;
    salts_mutex_unlock(&outbox->mutex);
    return rc;
}

typedef struct iris_outbox_dead_list_s {
    DataBind *codec;
    iris_event_dead_letter_t *items;
    size_t capacity;
    size_t count;
    size_t total;
} iris_outbox_dead_list_t;

static int dead_list_visit(void *context,
                           const iris_record_view_t *view) {
    iris_outbox_dead_list_t *list = (iris_outbox_dead_list_t *)context;
    IrisMediaEventOutboxRecordV1_t record;
    iris_event_dead_letter_t *item;
    int rc = decode_record(list->codec, view->value, view->value_size,
                           &record);
    if (rc != SALTS_OK) return rc;
    if (strcmp(record.delivery_state, IRIS_OUTBOX_STATE_DEAD) != 0) {
        IrisMediaEventOutboxRecordV1_clear(&record);
        return SALTS_OK;
    }
    if (list->total == SIZE_MAX) {
        IrisMediaEventOutboxRecordV1_clear(&record);
        return SALTS_ENOSPC;
    }
    list->total++;
    if (list->count < list->capacity) {
        item = &list->items[list->count++];
        memset(item, 0, sizeof(*item));
        copy_text(item->event_id, sizeof(item->event_id), record.event_id);
        copy_text(item->provider_session_id,
                  sizeof(item->provider_session_id),
                  record.provider_session_id);
        copy_text(item->dialog_id, sizeof(item->dialog_id), record.dialog_id);
        copy_text(item->event_type, sizeof(item->event_type),
                  record.event_type);
        item->occurred_at_ms = record.occurred_at_ms;
        item->dead_at_ms = record.state_changed_at_ms;
        item->store_revision = view->revision;
        item->delivery_attempts = record.delivery_attempts;
        item->last_http_status = record.last_http_status;
    }
    IrisMediaEventOutboxRecordV1_clear(&record);
    return SALTS_OK;
}

typedef struct iris_outbox_archive_list_s {
    DataBind *codec;
    iris_event_archive_t *items;
    size_t capacity;
    size_t count;
    size_t total;
} iris_outbox_archive_list_t;

static int archive_list_visit(void *context,
                              const iris_record_view_t *view) {
    iris_outbox_archive_list_t *list =
        (iris_outbox_archive_list_t *)context;
    IrisMediaEventOutboxRecordV1_t record;
    iris_event_archive_t *item;
    int rc = decode_record(list->codec, view->value, view->value_size,
                           &record);
    if (rc != SALTS_OK) return rc;
    if (strcmp(record.delivery_state, IRIS_OUTBOX_STATE_ARCHIVED) != 0) {
        IrisMediaEventOutboxRecordV1_clear(&record);
        return SALTS_OK;
    }
    if (list->total == SIZE_MAX) {
        IrisMediaEventOutboxRecordV1_clear(&record);
        return SALTS_ENOSPC;
    }
    list->total++;
    if (list->count < list->capacity) {
        item = &list->items[list->count++];
        memset(item, 0, sizeof(*item));
        copy_text(item->event_id, sizeof(item->event_id), record.event_id);
        copy_text(item->provider_session_id,
                  sizeof(item->provider_session_id),
                  record.provider_session_id);
        copy_text(item->dialog_id, sizeof(item->dialog_id), record.dialog_id);
        copy_text(item->event_type, sizeof(item->event_type),
                  record.event_type);
        item->occurred_at_ms = record.occurred_at_ms;
        item->archived_at_ms = record.archived_at_ms;
        item->store_revision = view->revision;
        item->delivery_attempts = record.delivery_attempts;
        item->last_http_status = record.last_http_status;
    }
    IrisMediaEventOutboxRecordV1_clear(&record);
    return SALTS_OK;
}

static int process_list_archived(iris_event_outbox_t *outbox,
                                 iris_outbox_request_t *request) {
    iris_outbox_archive_list_t list;
    int rc;
    memset(&list, 0, sizeof(list));
    list.codec = outbox->codec;
    list.items = request->archives;
    list.capacity = request->archive_capacity;
    rc = outbox->store->scan(outbox->store->ctx, archive_list_visit, &list);
    if (rc == SALTS_EPROTO) {
        stats_increment(outbox, &outbox->stats.decode_failure_total);
    }
    if (rc == SALTS_OK) {
        request->archive_count = list.count;
        request->archive_total = list.total;
    }
    return rc;
}

typedef struct iris_outbox_retention_scan_s {
    DataBind *codec;
    iris_outbox_candidate_t *items;
    size_t capacity;
    size_t count;
    uint64_t now_ms;
    uint64_t dead_retention_ms;
    uint64_t archive_retention_ms;
    const char *target_state;
} iris_outbox_retention_scan_t;

static int retention_visit(void *context,
                           const iris_record_view_t *view) {
    iris_outbox_retention_scan_t *scan =
        (iris_outbox_retention_scan_t *)context;
    IrisMediaEventOutboxRecordV1_t record;
    iris_outbox_candidate_t *candidate;
    int eligible;
    int rc;
    if (scan->count >= scan->capacity) return SALTS_OK;
    rc = decode_record(scan->codec, view->value, view->value_size, &record);
    if (rc != SALTS_OK) return rc;
    if (strcmp(record.delivery_state, scan->target_state) != 0) {
        IrisMediaEventOutboxRecordV1_clear(&record);
        return SALTS_OK;
    }
    eligible =
        (strcmp(scan->target_state, IRIS_OUTBOX_STATE_DEAD) == 0 &&
         retention_expired(record.state_changed_at_ms,
                           scan->dead_retention_ms, scan->now_ms)) ||
        (strcmp(scan->target_state, IRIS_OUTBOX_STATE_ARCHIVED) == 0 &&
         retention_expired(record.archived_at_ms,
                           scan->archive_retention_ms, scan->now_ms));
    if (!eligible) {
        IrisMediaEventOutboxRecordV1_clear(&record);
        return SALTS_OK;
    }
    candidate = &scan->items[scan->count++];
    memset(candidate, 0, sizeof(*candidate));
    event_from_record(&record, &candidate->event);
    candidate->revision = view->revision;
    candidate->attempts = record.delivery_attempts;
    candidate->http_status = record.last_http_status;
    candidate->state_changed_at_ms = record.state_changed_at_ms;
    candidate->archived_at_ms = record.archived_at_ms;
    copy_text(candidate->state, sizeof(candidate->state),
              record.delivery_state);
    IrisMediaEventOutboxRecordV1_clear(&record);
    return SALTS_OK;
}

static int process_retention(iris_event_outbox_t *outbox,
                             iris_event_retention_result_t *result) {
    iris_outbox_retention_scan_t scan;
    iris_outbox_candidate_t *items;
    size_t i;
    int rc;
    if (result) memset(result, 0, sizeof(*result));
    items = (iris_outbox_candidate_t *)calloc(
        outbox->retention.sweep_batch_size, sizeof(*items));
    if (!items) return SALTS_ENOMEM;
    memset(&scan, 0, sizeof(scan));
    scan.codec = outbox->codec;
    scan.items = items;
    scan.capacity = outbox->retention.sweep_batch_size;
    scan.now_ms = outbox_realtime_ms(outbox);
    scan.dead_retention_ms = outbox->retention.dead_retention_ms;
    scan.archive_retention_ms = outbox->retention.archive_retention_ms;
    scan.target_state = outbox->retention_delete_first
                            ? IRIS_OUTBOX_STATE_ARCHIVED
                            : IRIS_OUTBOX_STATE_DEAD;
    outbox->retention_delete_first = !outbox->retention_delete_first;
    if (scan.now_ms == 0u) {
        free(items);
        return SALTS_EINVAL;
    }
    rc = outbox->store->scan(outbox->store->ctx, retention_visit, &scan);
    if (rc == SALTS_OK && scan.count == 0u) {
        scan.target_state =
            strcmp(scan.target_state, IRIS_OUTBOX_STATE_DEAD) == 0
                ? IRIS_OUTBOX_STATE_ARCHIVED
                : IRIS_OUTBOX_STATE_DEAD;
        rc = outbox->store->scan(outbox->store->ctx, retention_visit, &scan);
    }
    if (rc == SALTS_EPROTO) {
        stats_increment(outbox, &outbox->stats.decode_failure_total);
    }
    for (i = 0u; rc == SALTS_OK && i < scan.count; ++i) {
        iris_outbox_candidate_t *candidate = &items[i];
        if (strcmp(candidate->state, IRIS_OUTBOX_STATE_DEAD) == 0) {
            if (candidate->revision >= IRIS_RECORD_REVISION_MAX) {
                rc = SALTS_ENOSPC;
                break;
            }
            rc = commit_record(outbox, &candidate->event,
                               IRIS_OUTBOX_STATE_ARCHIVED,
                               candidate->attempts, candidate->http_status,
                               candidate->revision, candidate->revision + 1u);
            if (rc == SALTS_OK) {
                stats_transition(outbox, &outbox->stats.dead_records,
                                 &outbox->stats.archived_records);
                stats_increment(outbox, &outbox->stats.archived_total);
                if (result) result->archived++;
            }
        } else {
            rc = delete_record(outbox, candidate->event.event_id,
                               candidate->revision);
            if (rc == SALTS_OK) {
                stats_release_payload(outbox,
                                      strlen(candidate->event.payload_json));
                stats_transition(outbox, &outbox->stats.archived_records,
                                 NULL);
                stats_increment(outbox,
                                &outbox->stats.archive_deleted_total);
                if (result) result->deleted++;
                TLOG_INFOF("Iris event archive deletion audit: event_id={}, "
                          "archived_at_ms={}, deleted_at_ms={}, revision={}.",
                          candidate->event.event_id,
                          candidate->archived_at_ms, scan.now_ms,
                          candidate->revision);
            }
        }
    }
    free(items);
    if (result) {
        salts_mutex_lock(&outbox->mutex);
        result->remaining_dead = outbox->stats.dead_records;
        result->remaining_archived = outbox->stats.archived_records;
        salts_mutex_unlock(&outbox->mutex);
    }
    if (rc != SALTS_OK) {
        stats_increment(outbox, &outbox->stats.retention_failure_total);
        TLOG_ERRORF("Iris event retention sweep failed: status={}. Durable "
                   "records are retained for the next bounded sweep.", rc);
    }
    return rc;
}

static int process_list_dead(iris_event_outbox_t *outbox,
                             iris_outbox_request_t *request) {
    iris_outbox_dead_list_t list;
    int rc;
    memset(&list, 0, sizeof(list));
    list.codec = outbox->codec;
    list.items = request->dead_letters;
    list.capacity = request->dead_letter_capacity;
    rc = outbox->store->scan(outbox->store->ctx, dead_list_visit, &list);
    if (rc == SALTS_EPROTO) {
        stats_increment(outbox, &outbox->stats.decode_failure_total);
    }
    if (rc == SALTS_OK) {
        request->dead_letter_count = list.count;
        request->dead_letter_total = list.total;
    }
    return rc;
}

static void complete_request(iris_outbox_request_t *request, int result) {
    salts_mutex_lock(&request->mutex);
    request->result = result;
    request->completed = 1;
    salts_cond_signal(&request->condition);
    salts_mutex_unlock(&request->mutex);
}

static void reset_sweep_deadline(iris_event_outbox_t *outbox) {
    uint64_t now_ms = salts_monotonic_ms();
    if (UINT64_MAX - now_ms < outbox->retention.sweep_interval_ms) {
        outbox->next_sweep_monotonic_ms = UINT64_MAX;
    } else {
        outbox->next_sweep_monotonic_ms =
            now_ms + outbox->retention.sweep_interval_ms;
    }
}

static void outbox_thread(void *context) {
    iris_event_outbox_t *outbox = (iris_event_outbox_t *)context;
    int recovery = recount_states(outbox);
    if (recovery == SALTS_OK) recovery = process_retention(outbox, NULL);
    if (recovery == SALTS_OK) recovery = normalize_in_flight(outbox);
    if (recovery == SALTS_OK) recovery = schedule_pending(outbox);
    if (recovery != SALTS_OK) {
        TLOG_ERRORF("Iris event outbox recovery failed: status={}. "
                   "New media event admission is stopped.", recovery);
    }
    salts_mutex_lock(&outbox->mutex);
    outbox->startup_result = recovery;
    outbox->startup_completed = 1;
    outbox->accepting = recovery == SALTS_OK;
    salts_cond_signal(&outbox->startup);
    salts_mutex_unlock(&outbox->mutex);
    reset_sweep_deadline(outbox);
    for (;;) {
        iris_outbox_request_t *request;
        int result;
        salts_mutex_lock(&outbox->mutex);
        while (outbox->count == 0u && outbox->running) {
            uint64_t now_ms = salts_monotonic_ms();
            uint64_t wait_ms;
            if (now_ms >= outbox->next_sweep_monotonic_ms) break;
            wait_ms = outbox->next_sweep_monotonic_ms - now_ms;
            if (wait_ms > outbox->retention.sweep_interval_ms) {
                wait_ms = outbox->retention.sweep_interval_ms;
            }
            (void)salts_cond_timedwait(&outbox->not_empty, &outbox->mutex,
                                       wait_ms * UINT64_C(1000000));
        }
        if (outbox->count == 0u && !outbox->running) {
            salts_mutex_unlock(&outbox->mutex);
            break;
        }
        if (outbox->running &&
            salts_monotonic_ms() >= outbox->next_sweep_monotonic_ms) {
            salts_mutex_unlock(&outbox->mutex);
            (void)process_retention(outbox, NULL);
            reset_sweep_deadline(outbox);
            continue;
        }
        request = outbox->requests[outbox->head];
        outbox->requests[outbox->head] = NULL;
        outbox->head = (outbox->head + 1u) % outbox->capacity;
        outbox->count--;
        salts_mutex_unlock(&outbox->mutex);
        if (request->kind == IRIS_OUTBOX_PERSIST) {
            result = process_persist(outbox, request);
        } else if (request->kind == IRIS_OUTBOX_SETTLE) {
            result = process_settle(outbox, request);
        } else if (request->kind == IRIS_OUTBOX_REPLAY) {
            result = process_replay(outbox, request);
        } else if (request->kind == IRIS_OUTBOX_REPLAY_DEAD_BATCH) {
            result = process_replay_dead_batch(outbox, request);
        } else if (request->kind == IRIS_OUTBOX_LIST_DEAD) {
            result = process_list_dead(outbox, request);
        } else if (request->kind == IRIS_OUTBOX_LIST_ARCHIVED) {
            result = process_list_archived(outbox, request);
        } else if (request->kind == IRIS_OUTBOX_RUN_RETENTION) {
            result = process_retention(outbox, &request->retention_result);
            reset_sweep_deadline(outbox);
        } else {
            result = SALTS_EINVAL;
        }
        complete_request(request, result);
    }
}

static iris_outbox_request_t *request_create(iris_outbox_request_kind_t kind) {
    iris_outbox_request_t *request =
        (iris_outbox_request_t *)calloc(1u, sizeof(*request));
    if (!request) return NULL;
    request->kind = kind;
    salts_mutex_init(&request->mutex);
    salts_cond_init(&request->condition);
    return request;
}

static void request_destroy(iris_outbox_request_t *request) {
    if (!request) return;
    free(request->dead_letters);
    free(request->archives);
    free(request->replay_candidates);
    salts_cond_destroy(&request->condition);
    salts_mutex_destroy(&request->mutex);
    free(request);
}

static int submit(iris_event_outbox_t *outbox,
                  iris_outbox_request_t *request) {
    size_t tail;
    int result;
    salts_mutex_lock(&outbox->mutex);
    if (!outbox->accepting) {
        salts_mutex_unlock(&outbox->mutex);
        return SALTS_ESHUTDOWN;
    }
    if (outbox->count == outbox->capacity) {
        salts_mutex_unlock(&outbox->mutex);
        return SALTS_ENOSPC;
    }
    tail = (outbox->head + outbox->count) % outbox->capacity;
    outbox->requests[tail] = request;
    outbox->count++;
    if (outbox->count > outbox->stats.request_queue_high_water) {
        outbox->stats.request_queue_high_water = outbox->count;
    }
    salts_cond_signal(&outbox->not_empty);
    salts_mutex_unlock(&outbox->mutex);
    salts_mutex_lock(&request->mutex);
    while (!request->completed) {
        salts_cond_wait(&request->condition, &request->mutex);
    }
    result = request->result;
    salts_mutex_unlock(&request->mutex);
    return result;
}

static iris_event_outbox_t *create_common(
    const iris_event_outbox_config_t *config) {
    iris_event_outbox_t *outbox;
    DataBindError error = DATA_BIND_ERROR_INIT;
    if (!config || !config->store || !config->deliver ||
        config->request_queue_capacity == 0u ||
        config->retention.dead_retention_ms == 0u ||
        config->retention.archive_retention_ms == 0u ||
        config->retention.sweep_interval_ms <
            IRIS_OUTBOX_MIN_SWEEP_INTERVAL_MS ||
        config->retention.sweep_batch_size == 0u ||
        config->retention.sweep_batch_size > IRIS_EVENT_OUTBOX_LIST_MAX ||
        config->request_queue_capacity > SIZE_MAX / sizeof(void *) ||
        (config->store->capabilities & IRIS_RECORD_STORE_DURABLE) == 0u ||
        (config->store->capabilities & IRIS_RECORD_STORE_ATOMIC_BATCH) == 0u ||
        !config->store->scan || !config->store->commit ||
        config->store->max_key_size < IRIS_OUTBOX_EVENT_ID_CAPACITY - 1u ||
        config->store->max_value_size < IRIS_OUTBOX_MIN_VALUE_SIZE ||
        config->store->max_records == 0u ||
        config->store->max_records >
            SIZE_MAX /
                (sizeof(((ivr_media_event_t *)0)->payload_json) - 1u)) {
        return NULL;
    }
    outbox = (iris_event_outbox_t *)calloc(1u, sizeof(*outbox));
    if (!outbox) return NULL;
    outbox->requests = (iris_outbox_request_t **)calloc(
        config->request_queue_capacity, sizeof(*outbox->requests));
    if (!outbox->requests ||
        IrisEventOutboxV1_codec_create(&outbox->codec, &error) != DATA_BIND_OK ||
        !outbox->codec) {
        data_bind_free(outbox->codec);
        free(outbox->requests);
        free(outbox);
        return NULL;
    }
    salts_mutex_init(&outbox->mutex);
    salts_cond_init(&outbox->not_empty);
    salts_cond_init(&outbox->startup);
    outbox->store = config->store;
    outbox->deliver = config->deliver;
    outbox->deliver_context = config->deliver_context;
    outbox->retention = config->retention;
    outbox->capacity = config->request_queue_capacity;
    outbox->stats.request_queue_capacity = config->request_queue_capacity;
    outbox->stats.record_capacity = config->store->max_records;
    return outbox;
}

iris_event_outbox_t *iris_event_outbox_create(
    const iris_event_outbox_config_t *config) {
    return create_common(config);
}

iris_event_outbox_t *iris_event_outbox_create_record_store(
    const char *yaml_path, const char *channel_name,
    size_t request_queue_capacity,
    const iris_event_outbox_retention_config_t *retention,
    iris_event_outbox_deliver_fn deliver,
    void *deliver_context, char *error_text, size_t error_capacity) {
    iris_orm_store_owner_t *owner;
    iris_record_store_t *store;
    iris_event_outbox_config_t config;
    iris_event_outbox_t *outbox = NULL;
    if (!yaml_path || !yaml_path[0] || !channel_name || !channel_name[0] ||
        !retention || !deliver) {
        if (error_text && error_capacity > 0u) {
            (void)snprintf(error_text, error_capacity,
                           "invalid outbox configuration");
        }
        return NULL;
    }
    owner = iris_orm_store_owner_create(yaml_path, channel_name, error_text,
                                        error_capacity);
    if (!owner) return NULL;
    store = iris_orm_store_owner_store(owner);
    memset(&config, 0, sizeof(config));
    config.request_queue_capacity = request_queue_capacity;
    config.store = store;
    config.deliver = deliver;
    config.deliver_context = deliver_context;
    config.retention = *retention;
    outbox = create_common(&config);
    if (!outbox) {
        if (error_text && error_capacity > 0u) {
            (void)snprintf(
                error_text, error_capacity,
                "FlowStore channel is not durable, atomic, or sufficiently bounded");
        }
        iris_orm_store_owner_destroy(owner);
        return NULL;
    }
    outbox->orm_store_owner = owner;
    return outbox;
}

int iris_event_outbox_start(iris_event_outbox_t *outbox) {
    int startup_result;
    if (!outbox || outbox->thread_started) return SALTS_EINVAL;
    salts_mutex_lock(&outbox->mutex);
    outbox->running = 1;
    outbox->accepting = 0;
    outbox->startup_completed = 0;
    outbox->startup_result = SALTS_EINVAL;
    salts_mutex_unlock(&outbox->mutex);
    if (salts_thread_create(&outbox->thread, outbox_thread, outbox) != SALTS_OK) {
        salts_mutex_lock(&outbox->mutex);
        outbox->running = 0;
        outbox->accepting = 0;
        salts_mutex_unlock(&outbox->mutex);
        return SALTS_EIO;
    }
    outbox->thread_started = 1;
    salts_mutex_lock(&outbox->mutex);
    while (!outbox->startup_completed) {
        salts_cond_wait(&outbox->startup, &outbox->mutex);
    }
    startup_result = outbox->startup_result;
    if (startup_result != SALTS_OK) {
        outbox->running = 0;
        salts_cond_broadcast(&outbox->not_empty);
    }
    salts_mutex_unlock(&outbox->mutex);
    if (startup_result != SALTS_OK) {
        salts_thread_join(&outbox->thread);
        salts_thread_destroy(&outbox->thread);
        outbox->thread_started = 0;
    }
    return startup_result;
}

void iris_event_outbox_stop(iris_event_outbox_t *outbox) {
    if (!outbox || !outbox->thread_started) return;
    salts_mutex_lock(&outbox->mutex);
    outbox->accepting = 0;
    while (outbox->count > 0u) {
        iris_outbox_request_t *request = outbox->requests[outbox->head];
        outbox->requests[outbox->head] = NULL;
        outbox->head = (outbox->head + 1u) % outbox->capacity;
        outbox->count--;
        complete_request(request, SALTS_ESHUTDOWN);
    }
    outbox->running = 0;
    salts_cond_broadcast(&outbox->not_empty);
    salts_mutex_unlock(&outbox->mutex);
    salts_thread_join(&outbox->thread);
    salts_thread_destroy(&outbox->thread);
    outbox->thread_started = 0;
}

void iris_event_outbox_destroy(iris_event_outbox_t *outbox) {
    if (!outbox) return;
    iris_event_outbox_stop(outbox);
    data_bind_free(outbox->codec);
    salts_cond_destroy(&outbox->startup);
    salts_cond_destroy(&outbox->not_empty);
    salts_mutex_destroy(&outbox->mutex);
    free(outbox->requests);
    iris_orm_store_owner_destroy(outbox->orm_store_owner);
    free(outbox);
}

static ivr_status_t map_status(int status) {
    switch (status) {
    case SALTS_OK: return IVR_OK;
    case SALTS_ENOSPC: return IVR_ENOSPC;
    case SALTS_ESHUTDOWN: return IVR_ECLOSED;
    case SALTS_EBUSY: return IVR_EBUSY;
    case SALTS_ENOENT: return IVR_EINVAL;
    default: return IVR_ESTATE;
    }
}

ivr_status_t iris_event_outbox_on_media_event(
    void *context, const ivr_media_event_t *event) {
    iris_event_outbox_t *outbox = (iris_event_outbox_t *)context;
    iris_outbox_request_t *request;
    int result;
    if (!outbox || !event_valid(event)) return IVR_EINVAL;
    request = request_create(IRIS_OUTBOX_PERSIST);
    if (!request) return IVR_ENOSPC;
    request->event = *event;
    result = submit(outbox, request);
    request_destroy(request);
    return map_status(result);
}

void iris_event_outbox_on_delivery_result(
    void *context, const ivr_media_event_t *event, uint64_t store_revision,
    int succeeded, int http_status) {
    iris_event_outbox_t *outbox = (iris_event_outbox_t *)context;
    iris_outbox_request_t *request;
    if (!outbox || !event_valid(event) || store_revision == 0u) return;
    request = request_create(IRIS_OUTBOX_SETTLE);
    if (!request) {
        TLOG_ERRORF("Iris event outbox settlement allocation failed: "
                   "event_id={}. The durable record is retained.",
                   event->event_id);
        return;
    }
    request->event = *event;
    request->revision = store_revision;
    request->succeeded = succeeded;
    request->http_status = http_status;
    {
        int result = submit(outbox, request);
        if (result != SALTS_OK) {
            TLOG_ERRORF("Iris event outbox settlement submission failed: "
                       "event_id={}, status={}. The durable record is retained.",
                       event->event_id, result);
        }
    }
    request_destroy(request);
}

ivr_status_t iris_event_outbox_replay(iris_event_outbox_t *outbox,
                                      const char *event_id) {
    iris_outbox_request_t *request;
    int result;
    if (!outbox || !event_id || !event_id[0] ||
        strlen(event_id) >= IRIS_OUTBOX_EVENT_ID_CAPACITY) return IVR_EINVAL;
    request = request_create(IRIS_OUTBOX_REPLAY);
    if (!request) return IVR_ENOSPC;
    copy_text(request->event_id, sizeof(request->event_id), event_id);
    result = submit(outbox, request);
    request_destroy(request);
    return map_status(result);
}

ivr_status_t iris_event_outbox_replay_dead_letters(
    iris_event_outbox_t *outbox, size_t limit,
    iris_event_replay_batch_result_t *result) {
    iris_outbox_request_t *request;
    int submit_result;
    if (result) memset(result, 0, sizeof(*result));
    if (!outbox || !result || limit == 0u ||
        limit > IRIS_EVENT_OUTBOX_BATCH_REPLAY_MAX ||
        limit > SIZE_MAX / sizeof(*request->replay_candidates)) {
        return IVR_EINVAL;
    }
    request = request_create(IRIS_OUTBOX_REPLAY_DEAD_BATCH);
    if (!request) return IVR_ENOSPC;
    request->replay_candidates = (iris_outbox_candidate_t *)calloc(
        limit, sizeof(*request->replay_candidates));
    if (!request->replay_candidates) {
        request_destroy(request);
        return IVR_ENOSPC;
    }
    request->replay_candidate_capacity = limit;
    submit_result = submit(outbox, request);
    *result = request->replay_batch_result;
    request_destroy(request);
    return map_status(submit_result);
}

ivr_status_t iris_event_outbox_list_dead_letters(
    iris_event_outbox_t *outbox, iris_event_dead_letter_t *items,
    size_t capacity, size_t *count, size_t *total) {
    iris_outbox_request_t *request;
    int result;
    if (count) *count = 0u;
    if (total) *total = 0u;
    if (!outbox || !items || !count || !total || capacity == 0u ||
        capacity > IRIS_EVENT_OUTBOX_LIST_MAX ||
        capacity > SIZE_MAX / sizeof(*items)) {
        return IVR_EINVAL;
    }
    request = request_create(IRIS_OUTBOX_LIST_DEAD);
    if (!request) return IVR_ENOSPC;
    request->dead_letters = (iris_event_dead_letter_t *)calloc(
        capacity, sizeof(*request->dead_letters));
    if (!request->dead_letters) {
        request_destroy(request);
        return IVR_ENOSPC;
    }
    request->dead_letter_capacity = capacity;
    result = submit(outbox, request);
    if (result == SALTS_OK) {
        memcpy(items, request->dead_letters,
               request->dead_letter_count * sizeof(*items));
        *count = request->dead_letter_count;
        *total = request->dead_letter_total;
    }
    request_destroy(request);
    return map_status(result);
}

ivr_status_t iris_event_outbox_list_archived(
    iris_event_outbox_t *outbox, iris_event_archive_t *items,
    size_t capacity, size_t *count, size_t *total) {
    iris_outbox_request_t *request;
    int result;
    if (count) *count = 0u;
    if (total) *total = 0u;
    if (!outbox || !items || !count || !total || capacity == 0u ||
        capacity > IRIS_EVENT_OUTBOX_LIST_MAX ||
        capacity > SIZE_MAX / sizeof(*items)) {
        return IVR_EINVAL;
    }
    request = request_create(IRIS_OUTBOX_LIST_ARCHIVED);
    if (!request) return IVR_ENOSPC;
    request->archives = (iris_event_archive_t *)calloc(
        capacity, sizeof(*request->archives));
    if (!request->archives) {
        request_destroy(request);
        return IVR_ENOSPC;
    }
    request->archive_capacity = capacity;
    result = submit(outbox, request);
    if (result == SALTS_OK) {
        memcpy(items, request->archives,
               request->archive_count * sizeof(*items));
        *count = request->archive_count;
        *total = request->archive_total;
    }
    request_destroy(request);
    return map_status(result);
}

ivr_status_t iris_event_outbox_run_retention(
    iris_event_outbox_t *outbox, iris_event_retention_result_t *result) {
    iris_outbox_request_t *request;
    int submit_result;
    if (result) memset(result, 0, sizeof(*result));
    if (!outbox || !result) return IVR_EINVAL;
    request = request_create(IRIS_OUTBOX_RUN_RETENTION);
    if (!request) return IVR_ENOSPC;
    submit_result = submit(outbox, request);
    *result = request->retention_result;
    request_destroy(request);
    return map_status(submit_result);
}

void iris_event_outbox_get_stats(iris_event_outbox_t *outbox,
                                 iris_event_outbox_stats_t *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    if (!outbox) return;
    salts_mutex_lock(&outbox->mutex);
    *stats = outbox->stats;
    stats->request_queue_items = outbox->count;
    salts_mutex_unlock(&outbox->mutex);
}
