#include "ivr_media_supervisor.h"

#include "ivr_thread.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

#define IVR_MEDIA_SUPERVISOR_QUEUE_CAPACITY 16u
#define IVR_MEDIA_SUPERVISOR_IDLE_POLL_MS 50u

typedef struct {
    ivr_media_link_kind_t link;
    uint64_t attempt_generation;
    ivr_media_link_state_t state;
    int error_code;
    uint64_t occurred_at_ms;
} ivr_media_supervisor_state_t;

struct ivr_media_supervisor_s {
    ivr_media_supervisor_config_t config;
    ivr_media_reconnect_t *reconnect;
    ivr_mutex_t lock;
    ivr_cond_t cond;
    ivr_thread_t thread;
    int lock_initialized;
    int cond_initialized;
    int thread_started;
    int running;
    int stop_requested;
    int initial_attempt_pending;
    ivr_media_supervisor_state_t queue[IVR_MEDIA_SUPERVISOR_QUEUE_CAPACITY];
    uint32_t queue_head;
    uint32_t queue_count;
    uint64_t queue_overflows;
};

static void supervisor_emit(ivr_media_supervisor_t *supervisor,
                            ivr_media_reconnect_event_t event,
                            uint64_t generation) {
    if (event != IVR_MEDIA_RECONNECT_EVENT_NONE &&
        supervisor->config.on_event) {
        supervisor->config.on_event(supervisor->config.event_context, event,
                                    generation);
    }
}

static void supervisor_process_state(
    ivr_media_supervisor_t *supervisor,
    const ivr_media_supervisor_state_t *state) {
    ivr_media_reconnect_event_t event = IVR_MEDIA_RECONNECT_EVENT_NONE;
    ivr_media_reconnect_snapshot_t snapshot;
    ivr_status_t status;

    ivr_mutex_lock(&supervisor->lock);
    status = ivr_media_reconnect_on_state(
        supervisor->reconnect, state->link, state->attempt_generation,
        state->state, state->error_code, state->occurred_at_ms, &event);
    if (status == IVR_ESTATE) {
        ivr_mutex_unlock(&supervisor->lock);
        return;
    }
    if (status != IVR_OK) {
        ivr_mutex_unlock(&supervisor->lock);
        supervisor_emit(supervisor,
                        IVR_MEDIA_RECONNECT_EVENT_RETRY_EXHAUSTED,
                        state->attempt_generation);
        return;
    }
    if (event == IVR_MEDIA_RECONNECT_EVENT_RECONNECTED &&
        ivr_media_reconnect_snapshot(supervisor->reconnect, &snapshot) ==
            IVR_OK &&
        snapshot.attempts_started == 1u) {
        ivr_mutex_unlock(&supervisor->lock);
        return;
    }
    ivr_mutex_unlock(&supervisor->lock);
    supervisor_emit(supervisor, event, state->attempt_generation);
    if (event == IVR_MEDIA_RECONNECT_EVENT_INPUT_STALLED) {
        supervisor_emit(supervisor, IVR_MEDIA_RECONNECT_EVENT_DISCONNECTED,
                        state->attempt_generation);
    }
}

static void supervisor_process_restart(ivr_media_supervisor_t *supervisor,
                                       uint64_t generation) {
    uint32_t failed = supervisor->config.restart(
        supervisor->config.restart_context, generation);
    ivr_media_supervisor_state_t state;

    memset(&state, 0, sizeof(state));
    state.attempt_generation = generation;
    state.state = IVR_MEDIA_LINK_FAILED;
    state.error_code = IVR_MEDIA_ERROR_PEER_FAILED;
    state.occurred_at_ms = turbo_monotonic_ms();
    if ((failed & IVR_MEDIA_RESTART_WHIP_FAILED) != 0) {
        state.link = IVR_MEDIA_LINK_WHIP;
        supervisor_process_state(supervisor, &state);
    }
    if ((failed & IVR_MEDIA_RESTART_WHEP_FAILED) != 0) {
        state.link = IVR_MEDIA_LINK_WHEP;
        supervisor_process_state(supervisor, &state);
    }
}

static void *supervisor_thread_main(void *opaque) {
    ivr_media_supervisor_t *supervisor =
        (ivr_media_supervisor_t *)opaque;

    for (;;) {
        ivr_media_supervisor_state_t state;
        ivr_media_reconnect_event_t event = IVR_MEDIA_RECONNECT_EVENT_NONE;
        ivr_media_reconnect_snapshot_t snapshot;
        uint64_t generation = 0;
        uint64_t wait_ms = IVR_MEDIA_SUPERVISOR_IDLE_POLL_MS;
        int have_state = 0;
        int restart_due = 0;

        ivr_mutex_lock(&supervisor->lock);
        if (supervisor->stop_requested) {
            ivr_mutex_unlock(&supervisor->lock);
            break;
        }
        if (supervisor->initial_attempt_pending) {
            supervisor->initial_attempt_pending = 0;
            restart_due = 1;
            (void)ivr_media_reconnect_snapshot(supervisor->reconnect,
                                               &snapshot);
            generation = snapshot.attempt_generation;
        } else if (supervisor->queue_count > 0) {
            state = supervisor->queue[supervisor->queue_head];
            supervisor->queue_head =
                (supervisor->queue_head + 1u) %
                IVR_MEDIA_SUPERVISOR_QUEUE_CAPACITY;
            supervisor->queue_count--;
            have_state = 1;
        } else {
            uint64_t now = turbo_monotonic_ms();
            if (ivr_media_reconnect_snapshot(supervisor->reconnect,
                                             &snapshot) == IVR_OK &&
                snapshot.retry_pending) {
                wait_ms = snapshot.next_retry_at_ms > now
                              ? snapshot.next_retry_at_ms - now
                              : 0;
            }
            if (wait_ms > IVR_MEDIA_SUPERVISOR_IDLE_POLL_MS) {
                wait_ms = IVR_MEDIA_SUPERVISOR_IDLE_POLL_MS;
            }
            if (wait_ms > 0) {
                (void)ivr_cond_timedwait(&supervisor->cond,
                                         &supervisor->lock, wait_ms);
            }
        }
        ivr_mutex_unlock(&supervisor->lock);

        if (restart_due) {
            supervisor_process_restart(supervisor, generation);
            continue;
        }
        if (have_state) {
            supervisor_process_state(supervisor, &state);
            continue;
        }
        ivr_mutex_lock(&supervisor->lock);
        ivr_status_t poll_status = ivr_media_reconnect_poll(
            supervisor->reconnect, turbo_monotonic_ms(), &generation,
            &event);
        ivr_mutex_unlock(&supervisor->lock);
        if (poll_status == IVR_OK) {
            if (event == IVR_MEDIA_RECONNECT_EVENT_RETRY_DUE) {
                supervisor_process_restart(supervisor, generation);
            } else {
                supervisor_emit(supervisor, event, generation);
            }
        }
    }
    ivr_mutex_lock(&supervisor->lock);
    supervisor->running = 0;
    ivr_mutex_unlock(&supervisor->lock);
    return NULL;
}

ivr_status_t ivr_media_supervisor_create(
    const ivr_media_supervisor_config_t *config,
    ivr_media_supervisor_t **out_supervisor) {
    ivr_media_supervisor_t *supervisor;
    if (!config || !config->restart || !out_supervisor) {
        return IVR_EINVAL;
    }
    supervisor = (ivr_media_supervisor_t *)calloc(1, sizeof(*supervisor));
    if (!supervisor) {
        return IVR_ENOSPC;
    }
    supervisor->config = *config;
    if (ivr_media_reconnect_create(&config->reconnect,
                                   &supervisor->reconnect) != IVR_OK ||
        ivr_mutex_init(&supervisor->lock) != 0) {
        ivr_media_supervisor_destroy(supervisor);
        return IVR_ENOSPC;
    }
    supervisor->lock_initialized = 1;
    if (ivr_cond_init(&supervisor->cond) != 0) {
        ivr_media_supervisor_destroy(supervisor);
        return IVR_ENOSPC;
    }
    supervisor->cond_initialized = 1;
    *out_supervisor = supervisor;
    return IVR_OK;
}

ivr_status_t ivr_media_supervisor_start(ivr_media_supervisor_t *supervisor) {
    uint64_t generation;
    if (!supervisor) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&supervisor->lock);
    if (supervisor->running || supervisor->thread_started) {
        ivr_mutex_unlock(&supervisor->lock);
        return IVR_ESTATE;
    }
    if (ivr_media_reconnect_start(supervisor->reconnect,
                                  turbo_monotonic_ms(), &generation) !=
        IVR_OK) {
        ivr_mutex_unlock(&supervisor->lock);
        return IVR_ESTATE;
    }
    supervisor->stop_requested = 0;
    supervisor->initial_attempt_pending = 1;
    supervisor->running = 1;
    if (ivr_thread_create(&supervisor->thread, supervisor_thread_main,
                          supervisor) != 0) {
        supervisor->running = 0;
        supervisor->initial_attempt_pending = 0;
        ivr_mutex_unlock(&supervisor->lock);
        return IVR_ENOSPC;
    }
    supervisor->thread_started = 1;
    ivr_mutex_unlock(&supervisor->lock);
    return IVR_OK;
}

ivr_status_t ivr_media_supervisor_submit_state(
    ivr_media_supervisor_t *supervisor, ivr_media_link_kind_t link,
    uint64_t attempt_generation, ivr_media_link_state_t state,
    int error_code) {
    uint32_t tail;
    if (!supervisor ||
        (link != IVR_MEDIA_LINK_WHIP && link != IVR_MEDIA_LINK_WHEP) ||
        attempt_generation == 0) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&supervisor->lock);
    if (!supervisor->running || supervisor->stop_requested) {
        ivr_mutex_unlock(&supervisor->lock);
        return IVR_ECLOSED;
    }
    if (supervisor->queue_count >= IVR_MEDIA_SUPERVISOR_QUEUE_CAPACITY) {
        supervisor->queue_overflows++;
        supervisor->stop_requested = 1;
        ivr_cond_broadcast(&supervisor->cond);
        ivr_mutex_unlock(&supervisor->lock);
        return IVR_ENOSPC;
    }
    tail = (supervisor->queue_head + supervisor->queue_count) %
           IVR_MEDIA_SUPERVISOR_QUEUE_CAPACITY;
    supervisor->queue[tail].link = link;
    supervisor->queue[tail].attempt_generation = attempt_generation;
    supervisor->queue[tail].state = state;
    supervisor->queue[tail].error_code = error_code;
    supervisor->queue[tail].occurred_at_ms = turbo_monotonic_ms();
    supervisor->queue_count++;
    ivr_cond_signal(&supervisor->cond);
    ivr_mutex_unlock(&supervisor->lock);
    return IVR_OK;
}

void ivr_media_supervisor_stop(ivr_media_supervisor_t *supervisor) {
    if (!supervisor || !supervisor->lock_initialized) {
        return;
    }
    ivr_mutex_lock(&supervisor->lock);
    supervisor->stop_requested = 1;
    if (supervisor->cond_initialized) {
        ivr_cond_broadcast(&supervisor->cond);
    }
    ivr_mutex_unlock(&supervisor->lock);
    if (supervisor->thread_started) {
        (void)ivr_thread_join(&supervisor->thread);
        supervisor->thread_started = 0;
    }
}

void ivr_media_supervisor_destroy(ivr_media_supervisor_t *supervisor) {
    if (!supervisor) {
        return;
    }
    ivr_media_supervisor_stop(supervisor);
    if (supervisor->cond_initialized) {
        ivr_cond_destroy(&supervisor->cond);
    }
    if (supervisor->lock_initialized) {
        ivr_mutex_destroy(&supervisor->lock);
    }
    ivr_media_reconnect_destroy(supervisor->reconnect);
    free(supervisor);
}

ivr_status_t ivr_media_supervisor_snapshot(
    ivr_media_supervisor_t *supervisor,
    ivr_media_reconnect_snapshot_t *out_snapshot,
    uint64_t *out_queue_overflows) {
    ivr_status_t status;
    if (!supervisor || !out_snapshot || !out_queue_overflows) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&supervisor->lock);
    status = ivr_media_reconnect_snapshot(supervisor->reconnect, out_snapshot);
    *out_queue_overflows = supervisor->queue_overflows;
    ivr_mutex_unlock(&supervisor->lock);
    return status;
}
