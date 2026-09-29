#ifndef TURBO_CLIENT_PROCESSING_H
#define TURBO_CLIENT_PROCESSING_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_client_processing_s turbo_client_processing_t;

typedef enum turbo_client_processing_status_t {
    TURBO_CLIENT_PROCESSING_OK = 0,
    TURBO_CLIENT_PROCESSING_EINVAL = -1,
    TURBO_CLIENT_PROCESSING_ENOMEM = -2,
    TURBO_CLIENT_PROCESSING_ESTATE = -3,
    TURBO_CLIENT_PROCESSING_EFULL = -4
} turbo_client_processing_status_t;

typedef enum turbo_client_processing_state_t {
    TURBO_CLIENT_PROCESSING_CREATED = 0,
    TURBO_CLIENT_PROCESSING_PREPARED,
    TURBO_CLIENT_PROCESSING_RUNNING,
    TURBO_CLIENT_PROCESSING_PAUSED,
    TURBO_CLIENT_PROCESSING_DRAINING,
    TURBO_CLIENT_PROCESSING_STOPPED,
    TURBO_CLIENT_PROCESSING_FAILED
} turbo_client_processing_state_t;

typedef struct turbo_client_processing_config_t {
    size_t size;
    size_t frame_queue_capacity;
    size_t frame_queue_max_bytes;
    uint64_t frame_queue_max_duration_us;
} turbo_client_processing_config_t;

typedef struct turbo_client_processing_snapshot_t {
    size_t size;
    turbo_client_processing_state_t state;
    size_t frame_queue_capacity;
    size_t frame_queue_max_bytes;
    uint64_t frame_queue_max_duration_us;
    size_t queued_frames;
    size_t queued_bytes;
    uint64_t queued_duration_us;
    uint64_t admitted_frames;
    uint64_t rejected_frames;
} turbo_client_processing_snapshot_t;

/*
 * ClientProcessing is CLIENT-only and single-owner. These lifecycle calls must
 * be serialized by the caller. The initial core owns no worker thread.
 *
 * Queue limits are independent hard bounds. Future asynchronous adapters must
 * reject admission rather than silently dropping frames when any bound is hit.
 */
TURBO_MEDIA_C_API void turbo_client_processing_config_init(
    turbo_client_processing_config_t *config);

TURBO_MEDIA_C_API void turbo_client_processing_snapshot_init(
    turbo_client_processing_snapshot_t *snapshot);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_create(
    const turbo_client_processing_config_t *config,
    turbo_client_processing_t **out_processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_prepare(turbo_client_processing_t *processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_start(turbo_client_processing_t *processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_pause(turbo_client_processing_t *processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_resume(turbo_client_processing_t *processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_request_stop(turbo_client_processing_t *processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_drain(turbo_client_processing_t *processing);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_snapshot(
    const turbo_client_processing_t *processing,
    turbo_client_processing_snapshot_t *snapshot);

TURBO_MEDIA_C_API turbo_client_processing_status_t
turbo_client_processing_destroy(turbo_client_processing_t *processing);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CLIENT_PROCESSING_H */
