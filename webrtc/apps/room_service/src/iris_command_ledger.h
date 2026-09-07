#ifndef TURBO_ROOM_SERVICE_IRIS_COMMAND_LEDGER_H
#define TURBO_ROOM_SERVICE_IRIS_COMMAND_LEDGER_H

#include "iris_command_ledger_port.h"
#include "iris_record_store.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iris_command_ledger_s iris_command_ledger_t;

typedef uint64_t (*iris_command_ledger_realtime_ms_fn)(void *context);

typedef struct iris_command_ledger_config_s {
    size_t request_queue_capacity;
    size_t retention_batch_size;
    uint64_t terminal_retention_ms;
    uint64_t retention_sweep_interval_ms;
    iris_record_store_t *store;
    iris_command_ledger_realtime_ms_fn realtime_ms;
    void *realtime_context;
} iris_command_ledger_config_t;

typedef struct iris_command_ledger_stats_s {
    size_t request_queue_items;
    size_t request_queue_capacity;
    size_t request_queue_high_water;
    size_t record_capacity;
    uint64_t claims_total;
    uint64_t duplicates_total;
    uint64_t conflicts_total;
    uint64_t unknown_total;
    uint64_t storage_failures_total;
    uint64_t queue_rejections_total;
    uint64_t recovered_unknown_total;
    uint64_t resource_queries_total;
    uint64_t resource_seen_total;
    uint64_t retained_deleted_total;
    uint64_t retention_sweeps_total;
    uint64_t retention_failures_total;
} iris_command_ledger_stats_t;

typedef struct iris_command_retention_result_s {
    size_t selected;
    size_t deleted;
    size_t remaining_terminal;
} iris_command_retention_result_t;

/**
 * Create a ledger over a borrowed, caller-serialized RecordStore.
 *
 * The ledger is the sole scan/commit caller until destroy. The store must
 * outlive the ledger and advertise durable plus atomic-batch capabilities.
 */
iris_command_ledger_t *
iris_command_ledger_create(const iris_command_ledger_config_t *config);

/** Resolve a dedicated PostgreSQL TurboDB ORM channel and own its lifecycle. */
iris_command_ledger_t *iris_command_ledger_create_record_store(
    const char *yaml_path, const char *channel_name,
    size_t request_queue_capacity, size_t retention_batch_size,
    uint64_t terminal_retention_ms, uint64_t retention_sweep_interval_ms,
    char *error, size_t error_capacity);

int iris_command_ledger_start(iris_command_ledger_t *ledger);
void iris_command_ledger_stop(iris_command_ledger_t *ledger);
void iris_command_ledger_destroy(iris_command_ledger_t *ledger);

/**
 * Durably claim an immutable command before executing an external side effect.
 *
 * IRIS_COMMAND_CLAIM_EXECUTE is the only disposition authorizing execution.
 * All input is copied before crossing to the owner thread.
 */
ivr_status_t iris_command_ledger_claim(iris_command_ledger_t *ledger,
                                       const iris_command_identity_t *identity,
                                       iris_command_claim_result_t *result);

/** Persist that an asynchronous provider resource accepted the command. */
ivr_status_t
iris_command_ledger_commit_accepted(iris_command_ledger_t *ledger,
                                    const iris_command_identity_t *identity,
                                    const char *provider_resource_id);

/** Persist the terminal response before returning a terminal HTTP response. */
ivr_status_t iris_command_ledger_commit_terminal(
    iris_command_ledger_t *ledger, const iris_command_identity_t *identity,
    const iris_command_terminal_outcome_t *outcome);

/** Persist an unresolved external outcome; retries must query/reconcile. */
ivr_status_t
iris_command_ledger_mark_unknown(iris_command_ledger_t *ledger,
                                 const iris_command_identity_t *identity);

/**
 * Query whether this exact session-scoped resource incarnation has a durable
 * accepted or successful terminal fact. The result is observation evidence;
 * it does not by itself mutate command state or execute a provider side effect.
 */
ivr_status_t
iris_command_ledger_resource_seen(iris_command_ledger_t *ledger,
                                  const iris_command_identity_t *identity,
                                  int *seen);

/** Remove an INTENT only when no external side effect was attempted. */
ivr_status_t
iris_command_ledger_abort_intent(iris_command_ledger_t *ledger,
                                 const iris_command_identity_t *identity);

/** Delete one bounded batch of expired terminal tombstones. */
ivr_status_t
iris_command_ledger_run_retention(iris_command_ledger_t *ledger,
                                  iris_command_retention_result_t *result);

void iris_command_ledger_get_stats(iris_command_ledger_t *ledger,
                                   iris_command_ledger_stats_t *stats);

/** Return a borrowed dependency-injection port valid until ledger destroy. */
iris_command_ledger_port_t
iris_command_ledger_port(iris_command_ledger_t *ledger);

#ifdef __cplusplus
}
#endif

#endif
