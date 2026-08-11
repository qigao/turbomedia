#include "ivr_session.h"
#include "ivr/ivr_worker.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int ivr_bytes_view_empty(const ivr_bytes_view_t *v) {
    return !v || v->size == 0 || !v->data;
}

static int ivr_event_type_equals(const ivr_event_t *event,
                                 const char *event_type,
                                 size_t event_type_size) {
    return event && event->event_type.size == event_type_size &&
           memcmp(event->event_type.data, event_type, event_type_size) == 0;
}

static void ivr_session_lock(ivr_session_t *s) { ivr_mutex_lock(&s->lock); }
static void ivr_session_unlock(ivr_session_t *s) { ivr_mutex_unlock(&s->lock); }

/* Approximate live memory of an owned event (strings + fixed overhead). */
static size_t ivr_event_mem_size(const ivr_event_t *e) {
    size_t total = sizeof(*e);
    total += e->event_id.size + e->event_type.size + e->room_id.size +
             e->call_id.size + e->causation_id.size + e->payload_json.size +
             e->input_id.size + e->input_value.size;
    return total;
}

/* Pop the oldest owned event from the inbox ring (single consumer).
   Ownership of the strings moves to `out`; the source slot is reset so the
   ring never frees the same allocation twice. */
static void ivr_session_inbox_pop(ivr_session_t *s, ivr_event_t *out) {
    *out = s->inbox[s->inbox_head];
    ivr_event_init(&s->inbox[s->inbox_head]);
    s->inbox_head = (s->inbox_head + 1) % s->inbox_capacity;
    s->inbox_count--;
    s->inbox_bytes -= ivr_event_mem_size(out);
}

/* ------------------------------------------------------------------ */
/* engine sink -> gateway / media port forwarding                      */
/* ------------------------------------------------------------------ */

static ivr_status_t ivr_session_sink_command(void *context,
                                             const ivr_command_t *command) {
    ivr_session_t *s = (ivr_session_t *)context;
    if (!s || !command || !s->gateway.submit_copy) {
        return IVR_EINVAL;
    }
    /* Idempotency key: when the engine did not supply one, derive a stable id
       from the call and the intent so retries reuse the same key. */
    ivr_str_t derived_message_id;
    ivr_str_init(&derived_message_id);
    const char *message_id_data = command->message_id.data;
    size_t message_id_size = command->message_id.size;
    if (message_id_size == 0) {
        if (ivr_str_append(&derived_message_id, s->call_id.data,
                           s->call_id.size) < 0 ||
            ivr_str_append(&derived_message_id, ":", 1) < 0 ||
            ivr_str_append(&derived_message_id, command->command_type.data,
                           command->command_type.size) < 0) {
            ivr_str_free(&derived_message_id);
            return IVR_ENOSPC;
        }
        message_id_data = derived_message_id.data;
        message_id_size = derived_message_id.size;
    }
    ivr_command_view_t view;
    view.message_id.data = message_id_data;
    view.message_id.size = message_id_size;
    view.command_type.data = command->command_type.data;
    view.command_type.size = command->command_type.size;
    view.call.room_id.data = command->room_id.size ? command->room_id.data
                                                   : s->room_id.data;
    view.call.room_id.size = command->room_id.size ? command->room_id.size
                                                   : s->room_id.size;
    view.call.call_id.data = command->call_id.size ? command->call_id.data
                                                   : s->call_id.data;
    view.call.call_id.size = command->call_id.size ? command->call_id.size
                                                   : s->call_id.size;
    view.call.call_generation = command->call_generation
                                    ? command->call_generation
                                    : s->call_generation;
    view.call.expected_room_version = command->expected_room_version
                                          ? command->expected_room_version
                                          : s->room_version;
    view.args_json.data = command->args_json.data;
    view.args_json.size = command->args_json.size;
    ivr_status_t rc = s->gateway.submit_copy(s->gateway.context, &view);
    ivr_str_free(&derived_message_id);
    return rc;
}

static ivr_status_t ivr_session_sink_play_tts(void *context,
                                              const ivr_call_ref_t *call,
                                              const ivr_bytes_view_t *text) {
    ivr_session_t *s = (ivr_session_t *)context;
    if (!s || !s->media.play_pcm) {
        return IVR_EINVAL;
    }
    return s->media.play_pcm(s->media.context, call, text);
}

static ivr_status_t ivr_session_sink_cancel_input(void *context,
                                                  const ivr_call_ref_t *call) {
    ivr_session_t *s = (ivr_session_t *)context;
    if (!s || !s->media.cancel_input) {
        return IVR_EINVAL;
    }
    return s->media.cancel_input(s->media.context, call);
}

static ivr_status_t ivr_session_sink_stop_bot(void *context,
                                              const ivr_call_ref_t *call) {
    ivr_session_t *s = (ivr_session_t *)context;
    if (!s || !s->media.stop_bot) {
        return IVR_EINVAL;
    }
    return s->media.stop_bot(s->media.context, call);
}

/* ------------------------------------------------------------------ */
/* event handling (control thread)                                     */
/* ------------------------------------------------------------------ */

static void ivr_session_emit_get_snapshot(ivr_session_t *s) {
    if (!s->gateway.submit_copy) {
        return;
    }
    ivr_command_t cmd;
    ivr_command_init(&cmd);
    ivr_str_assign(&cmd.command_type, "get_snapshot", 12);
    ivr_str_assign(&cmd.args_json, "{}", 2);
    (void)ivr_session_sink_command(s, &cmd);
    ivr_command_free(&cmd);
}

static void ivr_session_handle_state_event(ivr_session_t *s, ivr_event_t *ev) {
    if (ev->call_generation != s->call_generation) {
        s->dropped_generation++;
        return;
    }
    if (ev->sequence == 0) {
        /* State events must carry a per-call sequence; refuse silent holes. */
        s->dropped_zero_sequence++;
        return;
    }
    if (ev->sequence <= s->last_sequence) {
        s->dropped_duplicates++;
        return;
    }
    if (ev->sequence == s->last_sequence + 1) {
        s->last_sequence = ev->sequence;
        s->room_version = ev->room_version;
        (void)s->engine_ops->submit_event(s->engine, ev);
        (void)s->engine_ops->step(s->engine);
        return;
    }
    /* gap: stop advancing this call until the authoritative snapshot lands. */
    if (!s->awaiting_snapshot) {
        s->awaiting_snapshot = 1;
        s->snapshot_requests++;
        ivr_session_emit_get_snapshot(s);
    }
}

static void ivr_session_handle_event(ivr_session_t *s, ivr_event_t *ev) {
    if (ev->kind == IVR_EVENT_KIND_COMMAND_RESULT) {
        /* Command replies are correlated control-plane messages. Their
           sequence reports authoritative state but is not itself a domain
           event, so it must not create/close a sequence gap. */
        (void)s->engine_ops->submit_event(s->engine, ev);
        (void)s->engine_ops->step(s->engine);
        return;
    }
    if (ev->kind == IVR_EVENT_KIND_MEDIA) {
        if (ev->call_generation != s->call_generation) {
            s->dropped_generation++;
            return;
        }
        (void)s->engine_ops->submit_event(s->engine, ev);
        (void)s->engine_ops->step(s->engine);
        return;
    }
    if (ev->kind == IVR_EVENT_KIND_SNAPSHOT) {
        if (ev->sequence < s->last_sequence) {
            s->dropped_duplicates++;
            return;
        }
        s->last_sequence = ev->sequence;
        s->room_version = ev->room_version;
        s->awaiting_snapshot = 0;
        (void)s->engine_ops->submit_event(s->engine, ev);
        (void)s->engine_ops->step(s->engine);
        return;
    }
    if (s->awaiting_snapshot) {
        /* hold state events until snapshot confirms continuity */
        return;
    }
    ivr_session_handle_state_event(s, ev);
}

/* ------------------------------------------------------------------ */
/* input window (session control thread)                               */
/* ------------------------------------------------------------------ */

int ivr_session_begin_input_window(ivr_session_t *session,
                                   const ivr_bytes_view_t *input_id) {
    ivr_str_t previous_id;
    uint64_t previous_generation;
    ivr_call_ref_t call;
    ivr_status_t media_status = IVR_OK;
    uint64_t generation;
    if (!session || ivr_bytes_view_empty(input_id)) {
        return IVR_EINVAL;
    }
    ivr_str_init(&previous_id);
    ivr_mutex_lock(&session->input_mutex);
    previous_id = session->input_window_id;
    ivr_str_init(&session->input_window_id);
    previous_generation = session->input_generation;
    if (ivr_str_assign(&session->input_window_id, input_id->data,
                       input_id->size) < 0) {
        session->input_window_id = previous_id;
        ivr_str_init(&previous_id);
        ivr_mutex_unlock(&session->input_mutex);
        return IVR_ENOSPC;
    }
    generation = session->input_generation + 1u;
    if (generation == 0) {
        generation = 1;
    }
    session->input_generation = generation;
    session->input_has_value = 0;
    session->input_timeout = 0;
    session->input_observation_pending = 0;
    ivr_mutex_unlock(&session->input_mutex);
    memset(&call, 0, sizeof(call));
    call.room_id.data = session->room_id.data;
    call.room_id.size = session->room_id.size;
    call.call_id.data = session->call_id.data;
    call.call_id.size = session->call_id.size;
    call.call_generation = session->call_generation;
    if (previous_id.size > 0 && session->media.end_input) {
        ivr_bytes_view_t old_view = {previous_id.data, previous_id.size};
        media_status = session->media.end_input(
            session->media.context, &call, &old_view, previous_generation);
    }
    ivr_str_free(&previous_id);
    if (media_status == IVR_OK && session->media.begin_input) {
        ivr_bytes_view_t current_view = {input_id->data, input_id->size};
        media_status = session->media.begin_input(
            session->media.context, &call, &current_view, generation);
    }
    if (media_status != IVR_OK) {
        /* The media hook may have failed after the session committed the new
           generation. Close the local window fail-fast; the monotonic
           generation remains consumed so a late callback cannot match a
           future window. */
        ivr_mutex_lock(&session->input_mutex);
        if (session->input_generation == generation) {
            ivr_str_free(&session->input_window_id);
            ivr_str_init(&session->input_window_id);
        }
        ivr_mutex_unlock(&session->input_mutex);
        return media_status;
    }
    return IVR_OK;
}

static ivr_status_t ivr_session_deliver_input(ivr_session_t *s, ivr_event_t *ev) {
    ivr_mutex_lock(&s->input_mutex);
    if (s->input_window_id.size == 0 ||
        (ev->input_id.size > 0 &&
         (ev->input_id.size != s->input_window_id.size ||
          memcmp(ev->input_id.data, s->input_window_id.data,
                 ev->input_id.size) != 0))) {
        /* stale/late final for a window that is not active */
        s->dropped_stale_inputs++;
        ivr_mutex_unlock(&s->input_mutex);
        return IVR_ESTATE;
    }
    if (ev->kind == IVR_EVENT_KIND_TIMEOUT) {
        /* window deadline: wake the waiter with a noinput result */
        s->input_timeout = 1;
        ivr_cond_signal(&s->input_cond);
        ivr_mutex_unlock(&s->input_mutex);
        return IVR_OK;
    }
    if (s->input_has_value) {
        /* first final wins; later finals in the same window are dropped */
        s->dropped_stale_inputs++;
        ivr_mutex_unlock(&s->input_mutex);
        return IVR_ESTATE;
    }
    if (ivr_str_assign(&s->input_value, ev->input_value.data,
                       ev->input_value.size) < 0) {
        ivr_mutex_unlock(&s->input_mutex);
        return IVR_ENOSPC;
    }
    if (ivr_event_type_equals(ev, "dtmf.final", sizeof("dtmf.final") - 1u)) {
        s->input_latency_kind = IVR_SESSION_LATENCY_DTMF_TO_CANCEL;
        s->input_observation_pending = 1;
    } else if (ivr_event_type_equals(ev, "asr.final",
                                     sizeof("asr.final") - 1u)) {
        s->input_latency_kind = IVR_SESSION_LATENCY_ASR_TO_COMMAND;
        s->input_observation_pending = 1;
    } else {
        s->input_observation_pending = 0;
    }
    if (s->input_observation_pending) {
        s->input_accepted_at_ms = turbo_monotonic_ms();
        s->input_observation_generation = s->input_generation;
    }
    s->input_has_value = 1;
    ivr_cond_signal(&s->input_cond);
    ivr_mutex_unlock(&s->input_mutex);
    return IVR_OK;
}

int ivr_session_input_wait(ivr_session_t *session, uint64_t timeout_ms,
                           ivr_str_t *out_value) {
    ivr_str_t ended_id;
    uint64_t ended_generation = 0;
    ivr_call_ref_t call;
    int result;
    if (!session) {
        return -1;
    }
    ivr_str_init(&ended_id);
    ivr_mutex_lock(&session->input_mutex);
    session->input_waiting = 1;
    result = 0;
    uint64_t remaining = timeout_ms;
    while (!session->input_has_value && !session->input_terminal &&
           !session->input_timeout) {
        if (remaining == 0) {
            break; /* noinput */
        }
        uint64_t wait_ms = (remaining > 1000) ? 1000 : remaining;
        int signaled = ivr_cond_timedwait(&session->input_cond,
                                          &session->input_mutex, wait_ms);
        if (signaled) {
            continue;
        }
        remaining = (remaining > wait_ms) ? remaining - wait_ms : 0;
    }
    if (session->input_terminal) {
        result = -1;
    } else if (session->input_timeout) {
        result = 0; /* noinput */
        session->input_timeout = 0;
    } else if (session->input_has_value) {
        if (out_value) {
            if (ivr_str_assign(out_value, session->input_value.data,
                               session->input_value.size) < 0) {
                result = -1;
            } else {
                result = 1;
            }
        } else {
            result = 1;
        }
    } else {
        result = 0; /* noinput */
    }
    session->input_waiting = 0;
    ended_id = session->input_window_id;
    ivr_str_init(&session->input_window_id);
    ended_generation = session->input_generation;
    ivr_mutex_unlock(&session->input_mutex);
    if (ended_id.size > 0 && session->media.end_input) {
        memset(&call, 0, sizeof(call));
        call.room_id.data = session->room_id.data;
        call.room_id.size = session->room_id.size;
        call.call_id.data = session->call_id.data;
        call.call_id.size = session->call_id.size;
        call.call_generation = session->call_generation;
        ivr_bytes_view_t ended_view = {ended_id.data, ended_id.size};
        if (session->media.end_input(session->media.context, &call,
                                    &ended_view, ended_generation) != IVR_OK) {
            result = -1;
        }
    }
    ivr_str_free(&ended_id);
    return result;
}

int ivr_session_is_terminal(const ivr_session_t *session) {
    return session && atomic_load((atomic_int *)&session->terminal);
}

/* ------------------------------------------------------------------ */
/* session hooks for the engine adapter                                */
/* ------------------------------------------------------------------ */

static int ivr_session_hook_input_wait(void *ctx, uint64_t timeout_ms,
                                       ivr_str_t *out) {
    return ivr_session_input_wait((ivr_session_t *)ctx, timeout_ms, out);
}

static int ivr_session_hook_begin_input_window(void *ctx,
                                               const ivr_bytes_view_t *id) {
    return ivr_session_begin_input_window((ivr_session_t *)ctx, id);
}

static void ivr_session_complete_input_observation(
    ivr_session_t *session, ivr_session_latency_kind_t expected_kind,
    int succeeded) {
    uint64_t accepted_at_ms = 0;
    int should_observe = 0;
    if (!session) {
        return;
    }
    ivr_mutex_lock(&session->input_mutex);
    if (session->input_observation_pending &&
        session->input_latency_kind == expected_kind &&
        session->input_observation_generation == session->input_generation) {
        accepted_at_ms = session->input_accepted_at_ms;
        session->input_observation_pending = 0;
        should_observe = succeeded;
    }
    ivr_mutex_unlock(&session->input_mutex);
    if (should_observe && session->observer.on_latency) {
        uint64_t completed_at_ms = turbo_monotonic_ms();
        session->observer.on_latency(
            session->observer.context, expected_kind,
            completed_at_ms >= accepted_at_ms ? completed_at_ms - accepted_at_ms
                                               : 0);
    }
}

static void ivr_session_hook_input_cancelled(void *ctx, int succeeded) {
    ivr_session_complete_input_observation(
        (ivr_session_t *)ctx, IVR_SESSION_LATENCY_DTMF_TO_CANCEL, succeeded);
}

static void ivr_session_hook_input_command_completed(void *ctx, int submitted) {
    ivr_session_complete_input_observation(
        (ivr_session_t *)ctx, IVR_SESSION_LATENCY_ASR_TO_COMMAND, submitted);
}

static int ivr_session_hook_is_terminal(void *ctx) {
    return ivr_session_is_terminal((const ivr_session_t *)ctx);
}

/* ------------------------------------------------------------------ */
/* public entries                                                      */
/* ------------------------------------------------------------------ */

int ivr_session_submit_event_copy(ivr_session_t *session,
                                  const ivr_event_view_t *view) {
    if (!session || !view) {
        return IVR_EINVAL;
    }
    if (atomic_load((atomic_int *)&session->terminal) ||
        atomic_load((atomic_int *)&session->draining)) {
        return IVR_ECLOSED;
    }
    ivr_event_t ev;
    ivr_event_init(&ev);
    if (ivr_event_copy_from_view(&ev, view) < 0) {
        ivr_event_free(&ev);
        return IVR_ENOSPC;
    }
    if (ivr_event_classify(&ev) < 0) {
        ivr_event_free(&ev);
        return IVR_EINVAL;
    }
    if (ev.kind == IVR_EVENT_KIND_INPUT ||
        ev.kind == IVR_EVENT_KIND_TIMEOUT) {
        ivr_status_t rc = ivr_session_deliver_input(session, &ev);
        ivr_event_free(&ev);
        return rc;
    }
    ivr_session_lock(session);
    if (ev.payload_json.size > session->max_event_bytes ||
        session->inbox_count >= session->inbox_capacity) {
        ivr_session_unlock(session);
        ivr_event_free(&ev);
        return IVR_ENOSPC;
    }
    size_t ev_size = ivr_event_mem_size(&ev);
    if (ev_size > session->max_inbox_bytes ||
        session->inbox_bytes + ev_size > session->max_inbox_bytes) {
        ivr_session_unlock(session);
        ivr_event_free(&ev);
        return IVR_ENOSPC;
    }
    uint32_t tail = (session->inbox_head + session->inbox_count) %
                    session->inbox_capacity;
    session->inbox[tail] = ev;
    session->inbox_count++;
    session->inbox_bytes += ev_size;
    ivr_cond_signal(&session->cond);
    ivr_session_unlock(session);
    return IVR_OK;
}

void ivr_session_request_terminal(ivr_session_t *session) {
    if (!session) {
        return;
    }
    atomic_store((atomic_int *)&session->terminal, 1);
    ivr_mutex_lock(&session->input_mutex);
    session->input_terminal = 1;
    ivr_cond_broadcast(&session->input_cond);
    ivr_mutex_unlock(&session->input_mutex);
    ivr_session_lock(session);
    ivr_cond_broadcast(&session->cond);
    ivr_session_unlock(session);
}

/* ------------------------------------------------------------------ */
/* control thread                                                      */
/* ------------------------------------------------------------------ */

static void ivr_session_terminal_cleanup(ivr_session_t *s) {
    /* Let the engine cancel blocking input and unwind the dialog. */
    if (s->engine_ops && s->engine_ops->request_terminal && s->engine) {
        s->engine_ops->request_terminal(s->engine);
        (void)s->engine_ops->step(s->engine);
    }
    ivr_call_ref_t call;
    call.room_id.data = s->room_id.data;
    call.room_id.size = s->room_id.size;
    call.call_id.data = s->call_id.data;
    call.call_id.size = s->call_id.size;
    call.call_generation = s->call_generation;
    call.expected_room_version = s->room_version;
    (void)ivr_session_sink_cancel_input(s, &call);
    (void)ivr_session_sink_stop_bot(s, &call);
    atomic_store((atomic_int *)&s->closed, 1);
}

static void *ivr_session_thread_main(void *opaque) {
    ivr_session_t *s = (ivr_session_t *)opaque;
    ivr_session_lock(s);
    for (;;) {
        while (s->inbox_count == 0 &&
               !atomic_load((atomic_int *)&s->terminal) &&
               !atomic_load((atomic_int *)&s->draining)) {
            ivr_cond_wait(&s->cond, &s->lock);
        }
        if (atomic_load((atomic_int *)&s->terminal) ||
            atomic_load((atomic_int *)&s->draining)) {
            break;
        }
        while (s->inbox_count > 0) {
            ivr_event_t ev;
            ivr_session_inbox_pop(s, &ev);
            ivr_session_unlock(s);
            ivr_session_handle_event(s, &ev);
            ivr_event_free(&ev);
            ivr_session_lock(s);
            if (atomic_load((atomic_int *)&s->terminal) ||
                atomic_load((atomic_int *)&s->draining)) {
                break;
            }
        }
        if (atomic_load((atomic_int *)&s->terminal) ||
            atomic_load((atomic_int *)&s->draining)) {
            break;
        }
    }
    ivr_session_unlock(s);
    ivr_session_terminal_cleanup(s);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

int ivr_session_create(ivr_session_t *session,
                       const ivr_call_ref_t *call,
                       uint32_t inbox_capacity,
                       size_t max_event_bytes,
                       size_t max_inbox_bytes,
                       const ivr_xml_engine_ops_t *engine_ops,
                       const void *content,
                       const ivr_command_gateway_ops_t *gateway,
                       const ivr_media_port_ops_t *media) {
    if (!session || !call || inbox_capacity == 0 || max_event_bytes == 0 ||
        max_inbox_bytes == 0 || !engine_ops || !engine_ops->create ||
        !engine_ops->submit_event || !engine_ops->step ||
        !engine_ops->request_terminal || !engine_ops->destroy) {
        return IVR_EINVAL;
    }
    memset(session, 0, sizeof(*session));
    ivr_str_init(&session->room_id);
    ivr_str_init(&session->call_id);
    ivr_str_init(&session->input_window_id);
    ivr_str_init(&session->input_value);
    if (ivr_str_assign(&session->room_id, call->room_id.data,
                       call->room_id.size) < 0 ||
        ivr_str_assign(&session->call_id, call->call_id.data,
                       call->call_id.size) < 0) {
        ivr_str_free(&session->room_id);
        ivr_str_free(&session->call_id);
        return IVR_ENOSPC;
    }
    session->call_generation = call->call_generation;
    session->expected_room_version = call->expected_room_version;
    session->room_version = call->expected_room_version;
    session->engine_ops = engine_ops;
    session->content = content;
    session->inbox_capacity = inbox_capacity;
    session->max_event_bytes = max_event_bytes;
    session->max_inbox_bytes = max_inbox_bytes;
    session->inbox = (ivr_event_t *)calloc(inbox_capacity, sizeof(ivr_event_t));
    if (!session->inbox) {
        ivr_str_free(&session->room_id);
        ivr_str_free(&session->call_id);
        return IVR_ENOSPC;
    }
    if (ivr_mutex_init(&session->lock) < 0) {
        ivr_session_free(session);
        return IVR_ENOSPC;
    }
    session->lock_initialized = 1;
    if (ivr_mutex_init(&session->input_mutex) < 0) {
        ivr_session_free(session);
        return IVR_ENOSPC;
    }
    session->input_mutex_initialized = 1;
    if (ivr_cond_init(&session->cond) < 0) {
        ivr_session_free(session);
        return IVR_ENOSPC;
    }
    session->cond_initialized = 1;
    if (ivr_cond_init(&session->input_cond) < 0) {
        ivr_session_free(session);
        return IVR_ENOSPC;
    }
    session->input_cond_initialized = 1;
    if (gateway) {
        session->gateway = *gateway;
    }
    if (media) {
        session->media = *media;
    }
    /* Stable sink storage: the engine borrows &session->sink for its whole
       lifetime, so it must live inside the session, not a temporary. */
    session->sink.context = session;
    session->sink.command = ivr_session_sink_command;
    session->sink.play_tts = ivr_session_sink_play_tts;
    session->sink.cancel_input = ivr_session_sink_cancel_input;
    session->sink.stop_bot = ivr_session_sink_stop_bot;
    ivr_session_hooks_t hooks;
    hooks.context = session;
    hooks.input_wait = ivr_session_hook_input_wait;
    hooks.begin_input_window = ivr_session_hook_begin_input_window;
    hooks.input_cancelled = ivr_session_hook_input_cancelled;
    hooks.input_command_completed = ivr_session_hook_input_command_completed;
    hooks.is_terminal = ivr_session_hook_is_terminal;
    session->engine = engine_ops->create(content, &session->sink, &hooks, call);
    if (!session->engine) {
        /* engine creation failed: release what was already allocated */
        ivr_session_free(session);
        return IVR_ENOSPC;
    }
    return IVR_OK;
}

int ivr_session_start(ivr_session_t *session) {
    if (!session) {
        return IVR_EINVAL;
    }
    if (session->thread_started) {
        return IVR_ESTATE;
    }
    if (ivr_thread_create(&session->thread, ivr_session_thread_main, session) < 0) {
        return IVR_ENOSPC;
    }
    session->thread_started = 1;
    return IVR_OK;
}

void ivr_session_join(ivr_session_t *session) {
    if (!session) {
        return;
    }
    if (session->thread_started) {
        (void)ivr_thread_join(&session->thread);
        session->thread_started = 0;
    }
}

int ivr_session_join_timed(ivr_session_t *session, uint64_t timeout_ms,
                           int *timed_out) {
    if (!session || !timed_out) {
        return -1;
    }
    if (!session->thread_started) {
        *timed_out = 0;
        return 0;
    }
    int rc = ivr_thread_timedjoin(&session->thread, timeout_ms, timed_out);
    if (rc == 0) {
        session->thread_started = 0;
    }
    return rc;
}

void ivr_session_free(ivr_session_t *session) {
    if (!session) {
        return;
    }
    if (session->engine && session->engine_ops && session->engine_ops->destroy) {
        session->engine_ops->destroy(session->engine);
        session->engine = NULL;
    }
    if (session->inbox) {
        for (uint32_t i = 0; i < session->inbox_capacity; i++) {
            ivr_event_free(&session->inbox[i]);
        }
    }
    free(session->inbox);
    session->inbox = NULL;
    ivr_str_free(&session->room_id);
    ivr_str_free(&session->call_id);
    ivr_str_free(&session->input_window_id);
    ivr_str_free(&session->input_value);
    if (session->lock_initialized) {
        ivr_mutex_destroy(&session->lock);
        session->lock_initialized = 0;
    }
    if (session->input_mutex_initialized) {
        ivr_mutex_destroy(&session->input_mutex);
        session->input_mutex_initialized = 0;
    }
    if (session->cond_initialized) {
        ivr_cond_destroy(&session->cond);
        session->cond_initialized = 0;
    }
    if (session->input_cond_initialized) {
        ivr_cond_destroy(&session->input_cond);
        session->input_cond_initialized = 0;
    }
    session->owner_worker = NULL;
}

void ivr_session_destroy(ivr_session_t *session) {
    ivr_worker_t *owner;

    if (!session) {
        return;
    }
    owner = session->owner_worker;
    if (owner) {
        ivr_worker_release_session(owner, session);
        return;
    }
    ivr_session_request_terminal(session);
    ivr_session_join(session);
    ivr_session_free(session);
}
