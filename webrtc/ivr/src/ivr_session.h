#ifndef TURBO_MEDIA_IVR_SESSION_H
#define TURBO_MEDIA_IVR_SESSION_H

/**
 * @file ivr_session.h
 * @brief Per-call session internals (worker <-> session, adapter hooks).
 *
 * Public entry points usable from any thread:
 *   - ivr_session_submit_event_copy()
 *   - ivr_session_request_terminal()
 * Everything else is restricted to the session control thread.
 */

#include "ivr_internal.h"
#include "ivr_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_session_s {
    ivr_worker_t *owner_worker;

    /* identity */
    ivr_str_t room_id;
    ivr_str_t call_id;
    uint64_t call_generation;
    uint64_t expected_room_version;
    uint64_t room_version; /* last authoritative version observed */
    uint64_t last_sequence; /* last contiguous state sequence delivered */
    int awaiting_snapshot;
    uint64_t snapshot_requests;

    /* engine */
    const ivr_xml_engine_ops_t *engine_ops;
    ivr_xml_engine_t *engine;
    const void *content; /* owned by worker cache; borrowed here */

    /* sink: intents -> gateway / media port (stable storage for the engine) */
    ivr_engine_sink_t sink;
    ivr_command_gateway_ops_t gateway;
    ivr_media_port_ops_t media;
    ivr_session_observer_ops_t observer;

    /* state-event inbox (ring of owned events) */
    ivr_event_t *inbox;
    uint32_t inbox_capacity;
    uint32_t inbox_head;
    uint32_t inbox_count;
    size_t inbox_bytes; /* live bytes currently in the ring */
    size_t max_event_bytes;
    size_t max_inbox_bytes;

    /* input window / waiter */
    ivr_mutex_t input_mutex;
    ivr_cond_t input_cond;
    int input_waiting;
    int input_terminal;
    int input_timeout;
    ivr_str_t input_window_id;
    uint64_t input_generation;
    int input_has_value;
    ivr_str_t input_value;
    ivr_session_latency_kind_t input_latency_kind;
    uint64_t input_accepted_at_ms;
    uint64_t input_observation_generation;
    int input_observation_pending;

    /* control loop */
    ivr_thread_t thread;
    int thread_started;
    ivr_mutex_t lock;
    int lock_initialized;
    ivr_cond_t cond;
    int cond_initialized;
    int input_mutex_initialized;
    int input_cond_initialized;
    ivr_atomic_int_t terminal;
    ivr_atomic_int_t closed;
    ivr_atomic_int_t draining;

    /* counters */
    uint64_t dropped_stale_inputs;
    uint64_t dropped_duplicates;
    uint64_t dropped_generation;
    uint64_t dropped_zero_sequence;
} ivr_session_t;

/* Create a session. `content` is borrowed from the worker's package cache and
   must outlive the session. `sink` is copied. */
int ivr_session_create(ivr_session_t *session,
                       const ivr_call_ref_t *call,
                       uint32_t inbox_capacity,
                       size_t max_event_bytes,
                       size_t max_inbox_bytes,
                       const ivr_xml_engine_ops_t *engine_ops,
                       const void *content,
                       const ivr_command_gateway_ops_t *gateway,
                       const ivr_media_port_ops_t *media);

/* Start the control thread. Returns IVR_OK; on failure IVR_ENOSPC. */
int ivr_session_start(ivr_session_t *session);

/* Join the control thread (blocking). Call after request_terminal during drain. */
void ivr_session_join(ivr_session_t *session);

/* Wait up to timeout_ms for the control thread to exit. Returns 0 when joined;
   -1 with *timed_out=1 when it is still running (caller must join later before
   freeing the session). */
int ivr_session_join_timed(ivr_session_t *session, uint64_t timeout_ms,
                           int *timed_out);

void ivr_session_free(ivr_session_t *session);

/* Owner callback used by the public ivr_session_destroy() entry point. */
void ivr_worker_release_session(ivr_worker_t *worker, ivr_session_t *session);

/* Adapter hooks (control thread only): */

/* Open an input window with a fresh monotonic id (copied). */
int ivr_session_begin_input_window(ivr_session_t *session,
                                   const ivr_bytes_view_t *input_id);

/* Block until a matching final input arrives, the window times out, or the
   terminal latch is set. Returns:
     1  input value copied to *out_value
     0  noinput (timeout)
    -1  terminal
   Stale/duplicate finals are dropped and counted inside. */
int ivr_session_input_wait(ivr_session_t *session, uint64_t timeout_ms,
                           ivr_str_t *out_value);

/* True when the terminal latch is set (adapter may use to bail early). */
int ivr_session_is_terminal(const ivr_session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_SESSION_H */
