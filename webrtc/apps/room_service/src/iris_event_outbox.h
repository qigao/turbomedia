#ifndef TURBO_ROOM_SERVICE_IRIS_EVENT_OUTBOX_H
#define TURBO_ROOM_SERVICE_IRIS_EVENT_OUTBOX_H

#include "ivr_room_bridge.h"
#include "iris_record_store.h"

#include <stddef.h>
#include <stdint.h>

typedef struct iris_event_outbox_s iris_event_outbox_t;

#define IRIS_EVENT_OUTBOX_LIST_MAX 256u
#define IRIS_EVENT_OUTBOX_BATCH_REPLAY_MAX 256u

typedef ivr_status_t (*iris_event_outbox_deliver_fn)(
    void *context, const ivr_media_event_t *event, uint64_t store_revision);

typedef uint64_t (*iris_event_outbox_realtime_ms_fn)(void *context);

typedef struct iris_event_outbox_retention_config_s {
    uint64_t dead_retention_ms;
    uint64_t archive_retention_ms;
    uint32_t sweep_interval_ms;
    size_t sweep_batch_size;
    iris_event_outbox_realtime_ms_fn realtime_ms;
    void *realtime_context;
} iris_event_outbox_retention_config_t;

typedef struct iris_event_outbox_config_s {
    size_t request_queue_capacity;
    iris_record_store_t *store;
    iris_event_outbox_deliver_fn deliver;
    void *deliver_context;
    iris_event_outbox_retention_config_t retention;
} iris_event_outbox_config_t;

typedef struct iris_event_outbox_stats_s {
    size_t request_queue_items;
    size_t request_queue_capacity;
    size_t request_queue_high_water;
    size_t pending_records;
    size_t in_flight_records;
    size_t dead_records;
    size_t archived_records;
    size_t record_capacity;
    size_t retained_payload_bytes;
    size_t peak_retained_payload_bytes;
    uint64_t persisted_total;
    uint64_t duplicate_total;
    uint64_t conflict_total;
    uint64_t persist_failure_total;
    uint64_t capacity_rejection_total;
    uint64_t scheduled_total;
    uint64_t schedule_rejection_total;
    uint64_t delivered_total;
    uint64_t dead_lettered_total;
    uint64_t settlement_failure_total;
    uint64_t stale_settlement_total;
    uint64_t decode_failure_total;
    uint64_t recovered_total;
    uint64_t replayed_total;
    uint64_t archived_total;
    uint64_t archive_deleted_total;
    uint64_t retention_failure_total;
} iris_event_outbox_stats_t;

typedef struct iris_event_dead_letter_s {
    char event_id[128];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char event_type[128];
    uint64_t occurred_at_ms;
    uint64_t dead_at_ms;
    uint64_t store_revision;
    uint32_t delivery_attempts;
    int last_http_status;
} iris_event_dead_letter_t;

typedef struct iris_event_archive_s {
    char event_id[128];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char event_type[128];
    uint64_t occurred_at_ms;
    uint64_t archived_at_ms;
    uint64_t store_revision;
    uint32_t delivery_attempts;
    int last_http_status;
} iris_event_archive_t;

typedef struct iris_event_retention_result_s {
    size_t archived;
    size_t deleted;
    size_t remaining_dead;
    size_t remaining_archived;
} iris_event_retention_result_t;

typedef struct iris_event_replay_batch_result_s {
    size_t selected;
    size_t replayed;
    size_t remaining_dead;
    int backpressured;
} iris_event_replay_batch_result_t;

/**
 * Create an outbox over a borrowed caller-serialized RecordStore.
 *
 * The outbox becomes the sole scan/commit caller until destroy. The store and
 * delivery context must outlive the outbox. Durable and atomic-batch
 * capabilities are mandatory; no volatile fallback is accepted.
 */
iris_event_outbox_t *iris_event_outbox_create(
    const iris_event_outbox_config_t *config);

/**
 * Resolve a TurboDB ORM YAML record_store channel and own its lifecycle.
 * TurboMedia deploys the SQLite-only ORM runtime by default; a PostgreSQL
 * channel requires replacing it with TurboDB's PG-enabled build output. Redis
 * remains unsupported. SQLite is rejected unless allow_development_sqlite is
 * explicitly nonzero.
 */
iris_event_outbox_t *iris_event_outbox_create_record_store(
    const char *yaml_path, const char *channel_name,
    int allow_development_sqlite, size_t request_queue_capacity,
    const iris_event_outbox_retention_config_t *retention,
    iris_event_outbox_deliver_fn deliver,
    void *deliver_context, char *error, size_t error_capacity);

int iris_event_outbox_start(iris_event_outbox_t *outbox);
void iris_event_outbox_stop(iris_event_outbox_t *outbox);
void iris_event_outbox_destroy(iris_event_outbox_t *outbox);

ivr_status_t iris_event_outbox_on_media_event(
    void *context, const ivr_media_event_t *event);

/** Called by the HTTP dispatcher after its terminal retry decision. */
void iris_event_outbox_on_delivery_result(
    void *context, const ivr_media_event_t *event, uint64_t store_revision,
    int succeeded, int http_status);

/** Move one dead letter back to pending and schedule it. */
ivr_status_t iris_event_outbox_replay(iris_event_outbox_t *outbox,
                                      const char *event_id);

/**
 * Replay at most limit dead letters from one provider-defined stable scan.
 *
 * The owner thread applies each dead -> pending CAS in scan order and stops
 * when the HTTP dispatcher applies backpressure. Successfully transitioned
 * records remain replayed even if a later storage operation fails; callers may
 * safely repeat the command because non-dead records are not selected.
 */
ivr_status_t iris_event_outbox_replay_dead_letters(
    iris_event_outbox_t *outbox, size_t limit,
    iris_event_replay_batch_result_t *result);

/** Return a bounded provider-ordered dead-letter snapshot and the full total. */
ivr_status_t iris_event_outbox_list_dead_letters(
    iris_event_outbox_t *outbox, iris_event_dead_letter_t *items,
    size_t capacity, size_t *count, size_t *total);

/**
 * Return a bounded provider-ordered, metadata-only archive snapshot.
 *
 * items must own capacity slots and remains caller-owned. count is the number
 * copied; total is the full bounded-store total. Returns IVR_OK, IVR_EINVAL,
 * IVR_ENOSPC, IVR_ECLOSED, or IVR_ESTATE without mutating records.
 */
ivr_status_t iris_event_outbox_list_archived(
    iris_event_outbox_t *outbox, iris_event_archive_t *items,
    size_t capacity, size_t *count, size_t *total);

/**
 * Run one bounded archive/delete retention batch on the owner thread.
 *
 * result always reports transitions committed before a later failure plus the
 * remaining counters. Returns IVR_OK only when the complete selected batch was
 * processed; storage/CAS failure returns IVR_ESTATE and retains its source
 * record for a later sweep.
 */
ivr_status_t iris_event_outbox_run_retention(
    iris_event_outbox_t *outbox, iris_event_retention_result_t *result);

void iris_event_outbox_get_stats(iris_event_outbox_t *outbox,
                                 iris_event_outbox_stats_t *stats);

#endif
