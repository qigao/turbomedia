#ifndef TURBO_MEDIA_IVR_WORKER_HEALTH_H
#define TURBO_MEDIA_IVR_WORKER_HEALTH_H

#include <stddef.h>
#include <stdint.h>

#include "platform.h"
#include "turbo_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IVR_WORKER_HEALTH_CAPABILITIES_MAX 256u
#define IVR_WORKER_HEALTH_REASON_MAX 128u

typedef struct ivr_worker_health_s ivr_worker_health_t;

typedef struct {
    uint64_t generation;
    uint32_t max_sessions;
    uint32_t active_sessions;
    uint32_t reserved_sessions;
    int content_ready;
    int schema_ready;
    int command_channel_ready;
    int event_channel_ready;
    int sync_ready;
    int speech_ready;
    int sfu_ready;
    int draining;
    int ready;
    char capabilities[IVR_WORKER_HEALTH_CAPABILITIES_MAX];
    char reason[IVR_WORKER_HEALTH_REASON_MAX];
} ivr_worker_health_snapshot_t;

struct ivr_worker_health_s {
    turbo_mutex_t mutex;
    ivr_worker_health_snapshot_t value;
};

int ivr_worker_health_init(ivr_worker_health_t *health,
                           uint32_t max_sessions);
void ivr_worker_health_destroy(ivr_worker_health_t *health);
int ivr_worker_health_update(ivr_worker_health_t *health,
                             const ivr_worker_health_snapshot_t *value);
/* update uses generation as an optimistic concurrency token and derives
   effective readiness from dependencies, draining state, and capacity. */
int ivr_worker_health_snapshot(const ivr_worker_health_t *health,
                               ivr_worker_health_snapshot_t *out);
int ivr_worker_health_json(const ivr_worker_health_t *health,
                           char *out, size_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif
