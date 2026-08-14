#ifndef TURBO_MEDIA_IVR_MEDIA_RECONNECT_H
#define TURBO_MEDIA_IVR_MEDIA_RECONNECT_H

#include "ivr/ivr_worker.h"
#include "ivr_media_state.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_media_reconnect_s ivr_media_reconnect_t;

typedef enum {
    IVR_MEDIA_LINK_WHIP = 1,
    IVR_MEDIA_LINK_WHEP = 2
} ivr_media_link_kind_t;

typedef enum {
    IVR_MEDIA_RECONNECT_EVENT_NONE = 0,
    IVR_MEDIA_RECONNECT_EVENT_DISCONNECTED = 1,
    IVR_MEDIA_RECONNECT_EVENT_RETRY_DUE = 2,
    IVR_MEDIA_RECONNECT_EVENT_RECONNECTED = 3,
    IVR_MEDIA_RECONNECT_EVENT_RETRY_EXHAUSTED = 4,
    IVR_MEDIA_RECONNECT_EVENT_INPUT_STALLED = 5
} ivr_media_reconnect_event_t;

typedef struct {
    uint32_t max_attempts;          /* 0 = 3 */
    uint64_t initial_backoff_ms;    /* 0 = 100 */
    uint64_t max_backoff_ms;        /* 0 = 2000 */
    uint64_t total_deadline_ms;     /* Per recovery episode; 0 = 10000 */
} ivr_media_reconnect_config_t;

typedef struct {
    uint64_t attempt_generation;
    uint32_t attempts_started;
    int whip_connected;
    int whep_connected;
    int retry_pending;
    int exhausted;
    uint64_t next_retry_at_ms;
    uint64_t deadline_at_ms;
    uint64_t stale_callbacks;
    uint64_t input_stalls;
} ivr_media_reconnect_snapshot_t;

ivr_status_t ivr_media_reconnect_create(
    const ivr_media_reconnect_config_t *config,
    ivr_media_reconnect_t **out_reconnect);
void ivr_media_reconnect_destroy(ivr_media_reconnect_t *reconnect);

ivr_status_t ivr_media_reconnect_start(ivr_media_reconnect_t *reconnect,
                                       uint64_t now_ms,
                                       uint64_t *out_generation);
ivr_status_t ivr_media_reconnect_on_state(
    ivr_media_reconnect_t *reconnect, ivr_media_link_kind_t link,
    uint64_t attempt_generation, ivr_media_link_state_t state,
    int error_code, uint64_t now_ms,
    ivr_media_reconnect_event_t *out_event);
ivr_status_t ivr_media_reconnect_poll(
    ivr_media_reconnect_t *reconnect, uint64_t now_ms,
    uint64_t *out_generation, ivr_media_reconnect_event_t *out_event);
ivr_status_t ivr_media_reconnect_snapshot(
    const ivr_media_reconnect_t *reconnect,
    ivr_media_reconnect_snapshot_t *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif
