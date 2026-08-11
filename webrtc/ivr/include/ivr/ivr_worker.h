#ifndef TURBO_MEDIA_IVR_WORKER_H
#define TURBO_MEDIA_IVR_WORKER_H

/**
 * @file ivr_worker.h
 * @brief Public C ABI of the TurboMedia independent IVR worker (Phase 1).
 *
 * Ownership contract:
 * - Every `ivr_*_view_t` is a borrowed view valid only for the duration of the
 *   call that received it. Callers must copy before storing or crossing a
 *   thread/queue boundary.
 * - `ivr_session_submit_event_copy()` and `ivr_command_gateway_ops_t::submit_copy`
 *   own a private copy once they return success; on failure the caller keeps
 *   ownership of the source data.
 * - `ivr_session_t` is owned by its session control thread. The only public
 *   entry points callable from FlowMQ/ASR/DTMF callback threads are
 *   `ivr_session_submit_event_copy()` and `ivr_session_request_terminal()`.
 *
 * Threading model: one bounded, blocking-capable control slot per active call
 * (VoiceXML `collect_input` blocks). The worker enforces
 * `ivr_worker_config_t::max_sessions_per_worker` and rejects excess
 * assignments with `IVR_ENOSPC`; it never spawns unbounded threads.
 */

#include <stddef.h>
#include <stdint.h>

#define IVR_WORKER_ABI_VERSION 3u
#define IVR_SESSION_OBSERVER_ABI_VERSION 1u

typedef struct ivr_worker_s ivr_worker_t;
typedef struct ivr_session_s ivr_session_t;

typedef enum {
    IVR_OK = 0,
    IVR_EINVAL = -1,
    IVR_ENOSPC = -2,
    IVR_ECLOSED = -3,
    IVR_ESTATE = -4,
    IVR_EAUTH = -5,
    IVR_EVERSION = -6,
    /* A mutation/command is stale: past its deadline, or a replay outside the
       dedup retention window. Fail fast; never re-apply. */
    IVR_ESTALE = -7
} ivr_status_t;

/* Borrowed UTF-8 bytes; not NUL-terminated by contract. */
typedef struct {
    const char *data;
    size_t size;
} ivr_bytes_view_t;

typedef struct {
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
    /* input routing (dtmf.final / asr.final): the id of the input window the
       result belongs to and the recognized value. Empty when not applicable. */
    ivr_bytes_view_t input_id;
    ivr_bytes_view_t input_value;
} ivr_event_view_t; /* borrowed for this function call only */

typedef struct {
    ivr_bytes_view_t message_id;
    ivr_bytes_view_t command_type;
    ivr_call_ref_t call;
    ivr_bytes_view_t args_json;
} ivr_command_view_t; /* gateway must copy before asynchronous use */

typedef struct {
    uint32_t abi_version;
    void *context;
    /* Copy `command`; on success the callee owns the copy, on failure the
       caller keeps ownership. Must never block the session control thread. */
    ivr_status_t (*submit_copy)(void *context,
                                const ivr_command_view_t *command);
} ivr_command_gateway_ops_t;

typedef struct {
    uint32_t abi_version;
    void *context;
    ivr_status_t (*start_bot)(void *context, const ivr_call_ref_t *call);
    ivr_status_t (*play_pcm)(void *context, const ivr_call_ref_t *call,
                             const ivr_bytes_view_t *text);
    ivr_status_t (*cancel_input)(void *context, const ivr_call_ref_t *call);
    ivr_status_t (*stop_bot)(void *context, const ivr_call_ref_t *call);
    /* Version-2 input-window hooks. Implementations copy input_id before
       returning; callbacks may arrive concurrently from RTP ingress. */
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
    /* Create one media port for exactly one call. On success the worker owns
       out_instance and calls destroy() only after the session control thread
       has stopped using out_media. */
    ivr_status_t (*create)(void *context, const ivr_call_ref_t *call,
                           ivr_media_port_ops_t *out_media,
                           void **out_instance);
    void (*destroy)(void *context, void *instance);
} ivr_media_port_factory_ops_t;

/* Per-call input window id: matches a final result to the input window that
   started it. Stale or duplicated finals are rejected and counted. */
typedef struct {
    ivr_bytes_view_t input_id;
    ivr_bytes_view_t input_value; /* "1", "2", ... or recognized text */
} ivr_input_result_t;

typedef enum {
    IVR_SESSION_LATENCY_DTMF_TO_CANCEL = 0,
    IVR_SESSION_LATENCY_ASR_TO_COMMAND = 1
} ivr_session_latency_kind_t;

typedef struct {
    uint32_t abi_version;
    void *context;
    /* Optional borrowed observer. The callback runs without a session lock on
       the session owner thread after the corresponding side effect succeeds.
       It must be nonblocking and context must outlive the worker. */
    void (*on_latency)(void *context, ivr_session_latency_kind_t kind,
                       uint64_t duration_ms);
} ivr_session_observer_ops_t;

typedef struct {
    uint32_t abi_version;
    /* Worker identity; must match the FlowMQ/mTLS identity later. */
    const char *worker_id;
    /* Upper bound on concurrent sessions; assignments beyond this return
       IVR_ENOSPC. Never create unbounded threads. */
    uint32_t max_sessions_per_worker;
    /* Per-call state-event inbox capacity in entries. */
    uint32_t session_inbox_capacity;
    /* Per-event / per-command payload caps. */
    size_t max_event_bytes;
    size_t max_command_bytes;
    /* Total per-call inbox byte budget. 0 derives
       max_event_bytes * session_inbox_capacity. */
    size_t max_inbox_bytes;
    /* Root directory that contains versioned content packages. */
    const char *content_root;
    /* Drain deadline in milliseconds for begin_drain(). */
    uint64_t drain_deadline_ms;
    /* Optional versioned observer. The worker copies the ops; context remains
       borrowed. Leave zeroed to disable observations. */
    ivr_session_observer_ops_t observer;
} ivr_worker_config_t;

/* Worker lifecycle. All functions are thread-safe except destroy(). destroy()
   performs an idempotent drain before releasing worker-owned resources. */
ivr_status_t ivr_worker_create(const ivr_worker_config_t *config,
                               const ivr_command_gateway_ops_t *gateway,
                               const ivr_media_port_factory_ops_t *media_factory,
                               ivr_worker_t **out_worker);

/* Starts the worker: sessions can then be assigned. Returns IVR_ESTATE if the
   worker is already started or is draining. */
ivr_status_t ivr_worker_start(ivr_worker_t *worker);

/* Stops accepting new assignments and drains active sessions within the
   configured deadline. Idempotent. */
ivr_status_t ivr_worker_begin_drain(ivr_worker_t *worker);

/* Number of sessions whose control threads were still running when the drain
   deadline elapsed. Those sessions are joined again by ivr_worker_destroy()
   before any memory is freed. */
uint64_t ivr_worker_drain_timed_out(const ivr_worker_t *worker);

/* Assign a new call to this worker. `content_package` selects a versioned
   content package under content_root. The session handle is valid until
   ivr_session_destroy(), worker drain, or worker destroy, whichever occurs
   first. Returns IVR_ENOSPC when at capacity. */
ivr_status_t ivr_worker_assign_session(ivr_worker_t *worker,
                                       const ivr_call_ref_t *call,
                                       const char *content_package,
                                       ivr_session_t **out_session);

ivr_status_t ivr_worker_destroy(ivr_worker_t *worker);

/* Session: submit a copied domain/RTC/ASR/DTMF event. The event JSON is
   normalized by the session before it reaches the XML engine. May be called
   from any thread. Returns IVR_ENOSPC when the per-call inbox is full; the
   caller keeps ownership of `event` then. */
ivr_status_t ivr_session_submit_event_copy(ivr_session_t *session,
                                           const ivr_event_view_t *event);

/* Route one copied domain event to the matching per-call session (matched by
   room_id + call_id + call_generation). Thread-safe; callable from FlowMQ
   SUB/ASR/DTMF callback threads (the worker copies synchronously). Returns
   IVR_OK when routed, IVR_ESTATE when no matching session is assigned to this
   worker (the event is dropped and counted), or the session submit error. */
ivr_status_t ivr_worker_submit_event_copy(ivr_worker_t *worker,
                                          const ivr_event_view_t *event);

/* True when a session for this call (room_id + call_id + call_generation) is
   already assigned. Lets dispatch handlers stay idempotent: a re-dispatched
   call must not create a duplicate session. Thread-safe. */
int ivr_worker_has_session(const ivr_worker_t *worker,
                           const ivr_call_ref_t *call);

/* Thread-safe count of currently assigned session slots. */
uint32_t ivr_worker_active_sessions(const ivr_worker_t *worker);

/* Idempotently terminate and release the session matching `call`. Returns
   IVR_OK when the slot is absent or fully released. */
ivr_status_t ivr_worker_release_call(ivr_worker_t *worker,
                                     const ivr_call_ref_t *call);

/* Request terminal: sets the terminal latch and wakes blocked input waits.
   Must not run TurboXML or destroy media from the calling thread. */
void ivr_session_request_terminal(ivr_session_t *session);

/* Terminates and releases one worker-owned session. Do not use the handle
   after this call returns. */
void ivr_session_destroy(ivr_session_t *session);

/* Input window: register the currently active input window. Session validates
   incoming final results against this id (adapter calls this from the session
   control thread when an input window opens). */
ivr_status_t ivr_session_begin_input_window(ivr_session_t *session,
                                            const ivr_bytes_view_t *input_id);

#endif /* TURBO_MEDIA_IVR_WORKER_H */
