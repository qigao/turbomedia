#ifndef ROOM_SERVICE_IRIS_MEDIA_RECONCILER_H
#define ROOM_SERVICE_IRIS_MEDIA_RECONCILER_H

#include "iris_event_outbox.h"
#include "iris_flowmq_provider.h"
#include "ivr_fmq_adapter.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IRIS_MEDIA_RECONCILE_PROVIDER_ID "turbomedia"

typedef struct iris_media_reconciler_s iris_media_reconciler_t;

typedef enum iris_media_reconcile_state_e {
    IRIS_MEDIA_RECONCILE_NOT_READY = 1,
    IRIS_MEDIA_RECONCILE_FETCHING_EXPECTED = 2,
    IRIS_MEDIA_RECONCILE_FETCHING_INVENTORY = 3,
    IRIS_MEDIA_RECONCILE_APPLYING = 4,
    IRIS_MEDIA_RECONCILE_READY = 5,
    IRIS_MEDIA_RECONCILE_FAILED = 6,
    IRIS_MEDIA_RECONCILE_DRAINING = 7
} iris_media_reconcile_state_t;

typedef enum iris_expected_media_state_e {
    IRIS_EXPECTED_MEDIA_OPENING = 1,
    IRIS_EXPECTED_MEDIA_ACTIVE = 2,
    IRIS_EXPECTED_MEDIA_CLOSING = 3
} iris_expected_media_state_t;

typedef enum iris_expected_command_state_e {
    IRIS_EXPECTED_COMMAND_PENDING = 1,
    IRIS_EXPECTED_COMMAND_DISPATCHED = 2
} iris_expected_command_state_t;

typedef struct iris_expected_media_resource_s {
    char tenant_id[128];
    char provider_id[128];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t session_revision;
    char owner_node_id[128];
    uint64_t owner_epoch;
    uint64_t owner_lease_expires_at_ms;
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t call_generation;
    uint64_t operation_generation;
    iris_expected_media_state_t state;
    char active_command_id[128];
    iris_expected_command_state_t active_command_state;
    char dispatch_worker_id[128];
    uint64_t dispatch_epoch;
    uint64_t dispatch_lease_expires_at_ms;
} iris_expected_media_resource_t;

typedef struct iris_media_reconciler_ops_s {
    void *context;
    ivr_status_t (*fetch_expected)(
        void *context, iris_expected_media_resource_t *resources,
        size_t capacity, size_t *out_count);
    ivr_status_t (*list_workers)(
        void *context, ivr_fmq_worker_snapshot_t *workers, uint32_t capacity,
        uint32_t *out_count, uint32_t *out_total);
    ivr_status_t (*request_inventory)(
        void *context, const ivr_worker_inventory_request_t *request);
    ivr_status_t (*rebind_dialog)(
        void *context, const ivr_worker_inventory_record_t *record);
    ivr_status_t (*close_orphan)(
        void *context, const ivr_worker_inventory_record_t *record,
        const char *message_id, uint64_t deadline_timeout_ms);
    ivr_status_t (*complete_worker)(
        void *context, const char *worker_id, const char *worker_instance_id,
        uint64_t worker_epoch);
    int (*reconcile_required)(void *context);
    ivr_status_t (*submit_resource_lost)(
        void *context, const iris_expected_media_resource_t *resource,
        const char *reason);
} iris_media_reconciler_ops_t;

typedef struct iris_media_reconciler_config_s {
    iris_flowmq_provider_t *provider;
    size_t resource_capacity;
    uint32_t worker_capacity;
    uint32_t inventory_queue_capacity;
    uint32_t retry_max_attempts;
    uint32_t retry_backoff_ms;
    uint32_t request_timeout_ms;
    uint32_t drain_timeout_ms;
    uint32_t inventory_page_size;
    uint64_t close_deadline_ms;
    iris_event_outbox_t *event_outbox;
    iris_media_reconciler_ops_t ops;
} iris_media_reconciler_config_t;

typedef struct iris_media_reconciler_stats_s {
    iris_media_reconcile_state_t state;
    uint64_t reconcile_cycles_total;
    uint64_t reconcile_failures_total;
    uint64_t expected_fetches_total;
    uint64_t inventory_pages_total;
    uint64_t rebound_total;
    uint64_t orphan_close_total;
    uint64_t resource_lost_total;
    uint64_t inventory_queue_full_total;
    size_t inventory_queue_items;
    size_t inventory_queue_capacity;
} iris_media_reconciler_stats_t;

iris_media_reconciler_t *iris_media_reconciler_create(
    const iris_media_reconciler_config_t *config);

/* Attach the production FlowMQ adapter before start. Not valid for an
   injected-ops reconciler or after its thread starts. */
int iris_media_reconciler_set_adapter(iris_media_reconciler_t *reconciler,
                                      ivr_fmq_adapter_t *adapter);

/* The threaded lifecycle is one-shot. stop() permanently closes inventory
   intake, interrupts retry waits and joins the owner before returning. */
int iris_media_reconciler_start(iris_media_reconciler_t *reconciler);
void iris_media_reconciler_stop(iris_media_reconciler_t *reconciler);
void iris_media_reconciler_destroy(iris_media_reconciler_t *reconciler);

/* Deterministic control-plane entry used by contract tests. It is legal only
   before start; all callbacks execute without the reconciler mutex held. */
ivr_status_t iris_media_reconciler_reconcile_once(
    iris_media_reconciler_t *reconciler, int allow_missing_loss);

/* FlowMQ owner-thread callback. It only copies into a bounded queue. */
ivr_status_t iris_media_reconciler_on_inventory_page(
    void *context, const ivr_worker_inventory_envelope_t *page);

/* Pure deterministic event construction. Identical resource/reason input
   yields the same event ID, timestamp and payload across process restarts. */
ivr_status_t iris_media_reconciler_build_resource_lost_event(
    const iris_expected_media_resource_t *resource, const char *reason,
    ivr_media_event_t *out_event);

/* Provider ingress gate. It closes the registration race by checking both the
   reconcile state and the adapter's current requires_reconcile flags. */
int iris_media_reconciler_accepting_commands(
    iris_media_reconciler_t *reconciler);

void iris_media_reconciler_get_stats(
    iris_media_reconciler_t *reconciler,
    iris_media_reconciler_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
