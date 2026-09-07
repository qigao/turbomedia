#ifndef TURBO_MEDIA_IVR_WORKER_H
#define TURBO_MEDIA_IVR_WORKER_H

/**
 * @file ivr_worker.h
 * @brief Pure media-call executor used by an Iris-owned IVR workflow.
 *
 * Iris owns XML/JavaScript execution and all business state. This component
 * owns only bounded media-call slots and executes explicit media operations.
 * It never loads content packages, interprets workflow events, or creates a
 * per-call business thread.
 *
 * All views are borrowed for the duration of a call. A media factory must
 * copy the call identity if it retains it. A successful event-sink call means
 * the sink copied the event before returning.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IVR_WORKER_ABI_VERSION 7u
#define IVR_MEDIA_ID_CAPACITY 128u
#define IVR_WORKER_INVENTORY_VERSION 2u
#define IVR_WORKER_INVENTORY_MAX_PAGE_SIZE 32u

typedef struct ivr_worker_s ivr_worker_t;

typedef enum {
    IVR_OK = 0,
    IVR_EINVAL = -1,
    IVR_ENOSPC = -2,
    IVR_ECLOSED = -3,
    IVR_ESTATE = -4,
    IVR_EAUTH = -5,
    IVR_EVERSION = -6,
    IVR_ESTALE = -7,
    IVR_EBUSY = -8,
    IVR_ENOTFOUND = -9
} ivr_status_t;

typedef struct {
    const char *data;
    size_t size;
} ivr_bytes_view_t;

typedef struct {
    /* Opaque Iris routing metadata. Media code carries but never interprets
       tenant identity. */
    ivr_bytes_view_t tenant_id;
    /* Iris/provider session identity. This is independent of the SIP/WebRTC
       call id and is the primary workflow correlation key. */
    ivr_bytes_view_t provider_session_id;
    /* Iris-owned VoiceXML/IVR dialog identity. Media operations from another
       dialog must never reuse this dialog's worker slot. */
    ivr_bytes_view_t dialog_id;
    ivr_bytes_view_t room_id;
    ivr_bytes_view_t call_id;
    uint64_t call_generation;
    uint64_t expected_room_version;
} ivr_call_ref_t;

typedef struct {
    ivr_bytes_view_t event_id;
    ivr_bytes_view_t event_type;
    ivr_call_ref_t call;
    uint64_t sequence;
    ivr_bytes_view_t payload_json;
    ivr_bytes_view_t input_id;
    ivr_bytes_view_t input_value;
} ivr_event_view_t;

typedef struct {
    uint32_t abi_version;
    void *context;
    /* The callee must copy event data before returning IVR_OK. */
    ivr_status_t (*publish_copy)(void *context,
                                 const ivr_event_view_t *event);
} ivr_media_event_sink_ops_t;

typedef struct {
    uint32_t abi_version;
    void *context;
    ivr_status_t (*start_bot)(void *context, const ivr_call_ref_t *call);
    ivr_status_t (*play_pcm)(void *context, const ivr_call_ref_t *call,
                             const ivr_bytes_view_t *text);
    ivr_status_t (*cancel_input)(void *context, const ivr_call_ref_t *call);
    ivr_status_t (*stop_bot)(void *context, const ivr_call_ref_t *call);
    ivr_status_t (*begin_input)(void *context, const ivr_call_ref_t *call,
                                const ivr_bytes_view_t *input_id,
                                uint64_t input_generation);
    ivr_status_t (*end_input)(void *context, const ivr_call_ref_t *call,
                              const ivr_bytes_view_t *input_id,
                              uint64_t input_generation);
} ivr_media_port_ops_t;

typedef struct {
    uint32_t abi_version;
    void *context;
    ivr_status_t (*create)(void *context, const ivr_call_ref_t *call,
                           ivr_media_port_ops_t *out_media,
                           void **out_instance);
    void (*destroy)(void *context, void *instance);
} ivr_media_port_factory_ops_t;

typedef struct {
    uint32_t abi_version;
    const char *worker_id;
    /* Process-instance fence stamped on every inventory record. A new worker
       process must use a new instance id; reconnects advance worker_epoch. */
    const char *worker_instance_id;
    uint64_t worker_epoch;
    uint32_t max_sessions_per_worker;
    /* Optional injectable monotonic clock. A nonzero operation deadline is
       rejected when no clock is configured. */
    uint64_t (*now_ms)(void *context);
    void *now_context;
} ivr_worker_config_t;

typedef enum {
    IVR_WORKER_RESOURCE_OPENING = 1,
    IVR_WORKER_RESOURCE_ACTIVE = 2,
    IVR_WORKER_RESOURCE_CLOSING = 3
} ivr_worker_resource_state_t;

/* Owning resource observation copied from one worker slot. No field borrows
   worker storage, so callers may retain a returned page until they overwrite
   or release their own page object. */
typedef struct {
    char worker_id[IVR_MEDIA_ID_CAPACITY];
    char worker_instance_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t worker_epoch;
    char tenant_id[IVR_MEDIA_ID_CAPACITY];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t call_generation;
    /* Last operation accepted for this call. Reconciliation must use the
       next value when issuing an idempotent cleanup command. */
    uint64_t operation_generation;
    char input_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t input_generation;
    int input_active;
    ivr_worker_resource_state_t state;
    int rebindable;
} ivr_worker_inventory_record_t;

typedef struct {
    uint32_t inventory_version;
    /* Zero begins a new snapshot. Nonzero must match the worker revision
       returned by the first page or the query fails with IVR_ESTALE. */
    uint64_t expected_revision;
    /* Opaque slot cursor returned by the preceding page; zero starts. */
    uint32_t cursor;
    uint32_t limit;
} ivr_worker_inventory_query_t;

typedef struct {
    uint32_t inventory_version;
    uint64_t revision;
    uint32_t cursor;
    uint32_t next_cursor;
    uint32_t total_active;
    uint32_t count;
    int has_more;
    ivr_worker_inventory_record_t
        records[IVR_WORKER_INVENTORY_MAX_PAGE_SIZE];
} ivr_worker_inventory_page_t;

typedef struct {
    ivr_call_ref_t call;
    /* Monotonic per call. Equal generation is an idempotent replay and returns
       the cached status without repeating the media side effect. */
    uint64_t operation_generation;
    /* Absolute value in the configured monotonic clock domain. */
    uint64_t deadline_ms;
} ivr_media_operation_t;

ivr_status_t ivr_worker_create(
    const ivr_worker_config_t *config,
    const ivr_media_event_sink_ops_t *event_sink,
    const ivr_media_port_factory_ops_t *media_factory,
    ivr_worker_t **out_worker);
ivr_status_t ivr_worker_start(ivr_worker_t *worker);
/* Advance the process-local connection fence after CHTTP H1 WebSocket reconnects. The
   worker keeps its media slots, while subsequent inventory pages use the new
   epoch and a new revision so RoomService must reconcile before dispatch. */
ivr_status_t ivr_worker_advance_epoch(ivr_worker_t *worker,
                                      uint64_t worker_epoch);
ivr_status_t ivr_worker_begin_drain(ivr_worker_t *worker);
ivr_status_t ivr_worker_destroy(ivr_worker_t *worker);

ivr_status_t ivr_worker_open_media_call(ivr_worker_t *worker,
                                        const ivr_call_ref_t *call);
ivr_status_t ivr_worker_play(ivr_worker_t *worker,
                             const ivr_media_operation_t *operation,
                             const ivr_bytes_view_t *text);
ivr_status_t ivr_worker_begin_input(ivr_worker_t *worker,
                                    const ivr_media_operation_t *operation,
                                    const ivr_bytes_view_t *input_id,
                                    uint64_t input_generation);
ivr_status_t ivr_worker_end_input(ivr_worker_t *worker,
                                  const ivr_media_operation_t *operation,
                                  const ivr_bytes_view_t *input_id,
                                  uint64_t input_generation);
ivr_status_t ivr_worker_cancel_input(ivr_worker_t *worker,
                                     const ivr_media_operation_t *operation,
                                     const ivr_bytes_view_t *input_id,
                                     uint64_t input_generation);
ivr_status_t ivr_worker_close_media_call(ivr_worker_t *worker,
                                         const ivr_call_ref_t *call);

/* This forwards a media fact/data item to the injected sink. It never feeds
   a local state machine or advances business state. */
ivr_status_t ivr_worker_publish_event_copy(ivr_worker_t *worker,
                                           const ivr_event_view_t *event);

int ivr_worker_has_media_call(const ivr_worker_t *worker,
                              const ivr_call_ref_t *call);
uint32_t ivr_worker_active_sessions(const ivr_worker_t *worker);

/* Versioned, bounded active-resource inventory. The worker slot array is the
   media observation fact source. The function copies one consistent page
   under the worker lock and performs no external I/O or callback. */
ivr_status_t ivr_worker_query_inventory(
    const ivr_worker_t *worker, const ivr_worker_inventory_query_t *query,
    ivr_worker_inventory_page_t *out_page);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_WORKER_H */
