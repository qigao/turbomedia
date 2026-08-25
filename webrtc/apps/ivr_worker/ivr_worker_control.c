#include "ivr_worker_control.h"

#include "disruptor.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct ivr_worker_control_queue_s {
    disruptor_t *events;
    atomic_int accepting;
};

static int control_event_is_valid(const ivr_worker_control_event_t *event) {
    if (!event) return 0;
    if (event->kind == IVR_WORKER_CONTROL_DRAIN) return 1;
    return event->kind == IVR_WORKER_CONTROL_CONNECTION &&
           (event->connected == 0 || event->connected == 1);
}

int ivr_worker_control_queue_create(size_t capacity,
                                    ivr_worker_control_queue_t **out_queue) {
    ivr_worker_control_queue_t *queue;
    disruptor_config_t config;
    if (!out_queue || capacity == 0u) return -1;
    *out_queue = NULL;
    queue = (ivr_worker_control_queue_t *)calloc(1, sizeof(*queue));
    if (!queue) return -1;
    memset(&config, 0, sizeof(config));
    config.entry_size = sizeof(ivr_worker_control_event_t);
    config.capacity = capacity;
    config.consumer_capacity = 1u;
    config.mode = DISRUPTOR_MODE_WORKER_POOL;
    queue->events = disruptor_create(&config);
    if (!queue->events) {
        free(queue);
        return -1;
    }
    atomic_init(&queue->accepting, 1);
    *out_queue = queue;
    return 0;
}

void ivr_worker_control_queue_close(ivr_worker_control_queue_t *queue) {
    if (queue) {
        atomic_store_explicit(&queue->accepting, 0, memory_order_release);
    }
}

int ivr_worker_control_queue_publish(
    ivr_worker_control_queue_t *queue,
    const ivr_worker_control_event_t *event) {
    disruptor_cursor_t cursor;
    ivr_worker_control_event_t *entry;
    if (!queue || !control_event_is_valid(event) ||
        !atomic_load_explicit(&queue->accepting, memory_order_acquire) ||
        !disruptor_publisher_try_claim(queue->events, &cursor)) {
        return -1;
    }
    entry = (ivr_worker_control_event_t *)disruptor_acquire_entry(queue->events,
                                                                  &cursor);
    *entry = *event;
    if (!disruptor_publisher_publish(queue->events, &cursor)) {
        return -1;
    }
    return 0;
}

int ivr_worker_control_queue_try_pop(ivr_worker_control_queue_t *queue,
                                     ivr_worker_control_event_t *out_event) {
    disruptor_cursor_t cursor;
    const ivr_worker_control_event_t *entry;
    if (!queue || !out_event) return -1;
    if (!disruptor_worker_try_claim(queue->events, &cursor)) return 0;
    entry = (const ivr_worker_control_event_t *)disruptor_show_entry(
        queue->events, &cursor);
    *out_event = *entry;
    disruptor_worker_release_entry(queue->events, &cursor);
    return 1;
}

void ivr_worker_control_queue_destroy(ivr_worker_control_queue_t *queue) {
    if (!queue) return;
    ivr_worker_control_queue_close(queue);
    disruptor_destroy(queue->events);
    free(queue);
}

void ivr_worker_control_state_init(ivr_worker_control_state_t *state,
                                   uint64_t connection_generation) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->connection_generation = connection_generation;
}

int ivr_worker_control_apply(ivr_worker_control_state_t *state,
                             const ivr_worker_control_event_t *event,
                             ivr_worker_control_advance_epoch_fn advance_epoch,
                             void *advance_context) {
    uint64_t next;
    if (!state || !control_event_is_valid(event)) return -1;
    if (event->kind == IVR_WORKER_CONTROL_DRAIN) {
        state->drain_requested = 1;
        return 0;
    }
    if (event->connected == state->connected) return 0;
    if (event->connected && state->ever_connected) {
        if (!advance_epoch || state->connection_generation == UINT64_MAX) {
            return -1;
        }
        next = state->connection_generation + 1u;
        if (advance_epoch(advance_context, next) != 0) return -1;
        state->connection_generation = next;
    }
    state->connected = event->connected;
    if (event->connected) state->ever_connected = 1;
    return 0;
}
