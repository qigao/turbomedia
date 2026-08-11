#ifndef TURBO_MEDIA_IVR_MEDIA_SUPERVISOR_H
#define TURBO_MEDIA_IVR_MEDIA_SUPERVISOR_H

#include "ivr_media_reconnect.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_media_supervisor_s ivr_media_supervisor_t;

enum {
    IVR_MEDIA_RESTART_WHIP_FAILED = 1u << 0,
    IVR_MEDIA_RESTART_WHEP_FAILED = 1u << 1
};

typedef uint32_t (*ivr_media_restart_fn)(void *context,
                                         uint64_t attempt_generation);
typedef void (*ivr_media_supervisor_event_fn)(
    void *context, ivr_media_reconnect_event_t event,
    uint64_t attempt_generation);

typedef struct {
    ivr_media_reconnect_config_t reconnect;
    ivr_media_restart_fn restart;
    void *restart_context;
    ivr_media_supervisor_event_fn on_event;
    void *event_context;
} ivr_media_supervisor_config_t;

ivr_status_t ivr_media_supervisor_create(
    const ivr_media_supervisor_config_t *config,
    ivr_media_supervisor_t **out_supervisor);

/* Starts the owner thread and queues the first transport attempt. */
ivr_status_t ivr_media_supervisor_start(ivr_media_supervisor_t *supervisor);

/* Any-thread observer entry. The state fact is copied into a bounded queue. */
ivr_status_t ivr_media_supervisor_submit_state(
    ivr_media_supervisor_t *supervisor, ivr_media_link_kind_t link,
    uint64_t attempt_generation, ivr_media_link_state_t state,
    int error_code);

/* Idempotent callback barrier. No restart/on_event callback runs after return. */
void ivr_media_supervisor_stop(ivr_media_supervisor_t *supervisor);
void ivr_media_supervisor_destroy(ivr_media_supervisor_t *supervisor);

ivr_status_t ivr_media_supervisor_snapshot(
    ivr_media_supervisor_t *supervisor,
    ivr_media_reconnect_snapshot_t *out_snapshot,
    uint64_t *out_queue_overflows);

#ifdef __cplusplus
}
#endif

#endif
