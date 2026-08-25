#ifndef TURBO_MEDIA_IVR_WORKER_CONTROL_H
#define TURBO_MEDIA_IVR_WORKER_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IVR_WORKER_CONTROL_CONNECTION = 1,
    IVR_WORKER_CONTROL_DRAIN = 2
} ivr_worker_control_kind_t;

typedef struct {
    ivr_worker_control_kind_t kind;
    int connected;
} ivr_worker_control_event_t;

typedef struct ivr_worker_control_queue_s ivr_worker_control_queue_t;

typedef struct {
    int connected;
    int ever_connected;
    int drain_requested;
    uint64_t connection_generation;
} ivr_worker_control_state_t;

typedef int (*ivr_worker_control_advance_epoch_fn)(void *context,
                                                   uint64_t next_epoch);

int ivr_worker_control_queue_create(size_t capacity,
                                    ivr_worker_control_queue_t **out_queue);
void ivr_worker_control_queue_close(ivr_worker_control_queue_t *queue);
int ivr_worker_control_queue_publish(
    ivr_worker_control_queue_t *queue,
    const ivr_worker_control_event_t *event);
int ivr_worker_control_queue_try_pop(ivr_worker_control_queue_t *queue,
                                     ivr_worker_control_event_t *out_event);
void ivr_worker_control_queue_destroy(ivr_worker_control_queue_t *queue);

void ivr_worker_control_state_init(ivr_worker_control_state_t *state,
                                   uint64_t connection_generation);
int ivr_worker_control_apply(ivr_worker_control_state_t *state,
                             const ivr_worker_control_event_t *event,
                             ivr_worker_control_advance_epoch_fn advance_epoch,
                             void *advance_context);

#ifdef __cplusplus
}
#endif

#endif
