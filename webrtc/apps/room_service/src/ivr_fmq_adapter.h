#ifndef ROOM_SERVICE_IVR_FMQ_ADAPTER_H
#define ROOM_SERVICE_IVR_FMQ_ADAPTER_H

/**
 * @file ivr_fmq_adapter.h
 * @brief RoomService-side host adapter for the IVR FlowMQ bridge.
 *
 * Implements the ivr_room_bridge host handler (get_room_version / on_command)
 * over the authoritative turbo_room_service_t aggregate: conference.join maps
 * to add_participant, conference.leave to remove_participant, get_snapshot to a
 * read-only version/sequence report. Per-call domain sequences (the aggregate
 * has no per-call counter) are tracked here in a bounded table, derived from
 * the aggregate's authoritative state transitions. The bridge stays generic;
 * RoomService owns the state.
 *
 * Command authorization: when worker_acls are configured, every command and
 * every dispatch selection is checked against the worker's tenant/room/call
 * scope and content capability allowlist. A connected worker cannot bypass
 * this check (connection success != command authorization). When no ACL is
 * configured the adapter keeps the legacy allow-all behavior for registered
 * workers.
 */

#include "ivr/ivr_worker.h"
#include "ivr_room_bridge.h"
#include "turbo_room_service.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IVR_FMQ_ADAPTER_MAX_WORKER_ACLS 32u

typedef struct ivr_fmq_adapter_s ivr_fmq_adapter_t;

typedef enum {
    IVR_FMQ_WORKER_SYNCED = 1,
    IVR_FMQ_WORKER_READY = 2,
    IVR_FMQ_WORKER_DRAINING = 3,
    IVR_FMQ_WORKER_EXPIRED = 4
} ivr_fmq_worker_state_t;

typedef struct {
    char worker_id[128];
    uint32_t protocol_version;
    char instance_id[128];
    uint64_t connection_generation;
    ivr_fmq_worker_state_t state;
    uint32_t max_sessions;
    uint32_t active_sessions;
    uint32_t reserved_sessions;
    uint64_t lease_expires_at_ms;
    uint64_t health_generation;
    int health_ready;
    char capabilities[256];
} ivr_fmq_worker_snapshot_t;

/* One configured worker authorization entry (from RoomService [fmq.workers]).
   Borrowed strings; the adapter copies them during create. */
typedef struct {
    const char *worker_id;
    /* Optional tenant id. When set, the worker is only authorized for
       room_ids equal to the tenant or prefixed "<tenant>/". */
    const char *tenant_id;
    /* "*" or comma-separated exact room ids. */
    const char *room_scope;
    /* "*" or comma-separated exact call ids. */
    const char *call_scope;
    /* "*" or comma-separated content package/capability tokens the worker is
       allowed to serve. */
    const char *content_capabilities;
} ivr_fmq_worker_acl_entry_t;

typedef struct {
    void *context;
    uint64_t (*now_ms)(void *context);
} ivr_fmq_clock_ops_t;

typedef struct {
    void *context;
    /* Prepare the authoritative caller-audio subscription before dispatching
       the IVR session. The implementation must select a real track and return
       a specific error instead of guessing a track identifier. */
    ivr_status_t (*prepare_caller_audio)(void *context, const char *room_id,
                                         const char *call_id, char *error,
                                         size_t error_capacity);
    /* Disable/remove the IVR receiver subscription before conference.leave
       removes the caller and its published tracks. Must be idempotent. */
    ivr_status_t (*release_caller_audio)(void *context, const char *room_id,
                                         const char *call_id, char *error,
                                         size_t error_capacity);
} ivr_fmq_media_ops_t;

typedef struct {
    const char *bind_host; /* ROUTER bind address; NULL = "127.0.0.1" */
    int bind_port;         /* ROUTER port; <= 0 disables the FMQ endpoint */
    int pub_port;          /* PUB port; <= 0 disables domain event publishing */
    const char *pub_topic; /* PUB topic; NULL = "room.events" */
    uint64_t timeout_ms;   /* 0 = worker lease duration */
    uint32_t queue_capacity; /* bridge cloned-request queue; 0 = 64 */
    uint32_t dedup_capacity; /* bridge message_id cache; 0 = 64 */
    uint64_t dedup_retention_ms; /* bridge replay retention window; 0 = 60000 */
    uint32_t seq_capacity;   /* bounded per-call sequence table; 0 = 256 */
    uint32_t worker_capacity; /* bounded worker.sync registration table; 0 = 64 */
    uint32_t assignment_capacity; /* dispatch ACK table; 0 = 256 */
    uint32_t legacy_worker_max_sessions; /* V1 compatibility; 0 = 1 */
    uint64_t worker_lease_ms; /* authoritative lease duration; 0 = 15000 */
    uint64_t dispatch_deadline_ms; /* pending ACK deadline; 0 = 5000 */
    uint32_t dispatch_max_attempts; /* bounded V2 attempts; 0 = 3, max = 8 */
    const char *default_content_package; /* dispatched calls; NULL = "conference-greeting" */
    const ivr_fmq_worker_acl_entry_t *worker_acls; /* NULL = legacy allow-all */
    size_t worker_acl_count;
    ivr_fmq_media_ops_t media;
    ivr_fmq_clock_ops_t clock;
    int transport; /* turbo_flow_fmq_transport_t; 0 = TCP */
    const char *path;
    const turbo_flow_fmq_tls_config_t *tls;
    const turbo_flow_fmq_security_binding_t *security;
} ivr_fmq_adapter_config_t;

typedef enum {
    IVR_FMQ_ASSIGNMENT_PENDING = 1,
    IVR_FMQ_ASSIGNMENT_ACCEPTED = 2,
    IVR_FMQ_ASSIGNMENT_REJECTED = 3,
    IVR_FMQ_ASSIGNMENT_RELEASING = 4,
    IVR_FMQ_ASSIGNMENT_RECOVERING = 5
} ivr_fmq_assignment_state_t;

typedef struct {
    char message_id[128];
    char assignment_id[128];
    char attempt_id[128];
    uint32_t attempt_count;
    char worker_id[128];
    char room_id[128];
    char call_id[128];
    uint64_t call_generation;
    ivr_fmq_assignment_state_t state;
    int status_code;
    char error_code[64];
    char error_message[128];
    char worker_instance_id[128];
    uint64_t worker_connection_generation;
    char release_message_id[128];
    uint64_t dispatch_deadline_at_ms;
    uint64_t release_deadline_at_ms;
} ivr_fmq_assignment_t;

typedef struct {
    uint32_t workers;
    uint32_t worker_capacity;
    uint32_t worker_high_water;
    uint32_t assignments;
    uint32_t assignment_capacity;
    uint32_t assignment_high_water;
    uint64_t lease_expired_total;
    uint64_t dispatch_timeout_total;
    uint64_t release_timeout_total;
    uint64_t acl_rejects; /* scope/capability authorization rejections */
    ivr_room_bridge_stats_t bridge;
} ivr_fmq_adapter_stats_t;

/* Create the adapter. When bind_port <= 0 no FMQ endpoint is created and the
   adapter only hosts the aggregate apply logic (used by tests). */
ivr_status_t ivr_fmq_adapter_create(turbo_room_service_t *service,
                                    const ivr_fmq_adapter_config_t *config,
                                    ivr_fmq_adapter_t **out_adapter);
ivr_status_t ivr_fmq_adapter_start(ivr_fmq_adapter_t *adapter);
void ivr_fmq_adapter_stop(ivr_fmq_adapter_t *adapter);
void ivr_fmq_adapter_destroy(ivr_fmq_adapter_t *adapter);

/* Apply one decoded IVR command to the authoritative aggregate and fill the
   result (status_code 0 = applied; negative ivr_status_t error). Pure host
   logic: no FlowMQ is touched, so it is unit-testable without a peer. */
ivr_status_t ivr_fmq_adapter_apply(ivr_fmq_adapter_t *adapter,
                                   const ivr_room_command_t *command,
                                   ivr_room_command_result_t *result);

/* True when the worker_id has completed a worker.sync registration. */
int ivr_fmq_adapter_worker_registered(const ivr_fmq_adapter_t *adapter,
                                      const char *worker_id);

/* Read-only copy of one bounded dispatch assignment observation. */
ivr_status_t ivr_fmq_adapter_get_assignment(
    const ivr_fmq_adapter_t *adapter, const char *message_id,
    ivr_fmq_assignment_t *out);

/* Thread-safe read-only registry snapshot. Expired leases are reported as
   EXPIRED even before the next owner command arrives. */
ivr_status_t ivr_fmq_adapter_get_worker(
    const ivr_fmq_adapter_t *adapter, const char *worker_id,
    ivr_fmq_worker_snapshot_t *out);

/* Advance deadline state using the configured monotonic clock. Production
   calls this from the bridge owner tick; exposed for deterministic tests. */
void ivr_fmq_adapter_poll(ivr_fmq_adapter_t *adapter);

/* Thread-safe, read-only operational snapshot. It does not advance leases or
   assignment deadlines. */
void ivr_fmq_adapter_get_stats(const ivr_fmq_adapter_t *adapter,
                               ivr_fmq_adapter_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ROOM_SERVICE_IVR_FMQ_ADAPTER_H */