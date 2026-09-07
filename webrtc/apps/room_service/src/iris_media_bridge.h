#ifndef TURBO_ROOM_SERVICE_IRIS_MEDIA_BRIDGE_H
#define TURBO_ROOM_SERVICE_IRIS_MEDIA_BRIDGE_H

#include "iris_command_ledger_port.h"
#include "iris_resource_observer.h"
#include "ivr_control_gateway.h"
#include "ivr_room_bridge.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iris_media_bridge_s iris_media_bridge_t;

typedef enum iris_media_bridge_status_e {
    IRIS_MEDIA_BRIDGE_ACCEPTED = 0,
    IRIS_MEDIA_BRIDGE_DUPLICATE,
    IRIS_MEDIA_BRIDGE_TERMINAL,
    IRIS_MEDIA_BRIDGE_TERMINAL_REPLAY,
    IRIS_MEDIA_BRIDGE_INVALID,
    IRIS_MEDIA_BRIDGE_CONFLICT,
    IRIS_MEDIA_BRIDGE_EXPIRED,
    IRIS_MEDIA_BRIDGE_FULL,
    IRIS_MEDIA_BRIDGE_UNAVAILABLE,
    IRIS_MEDIA_BRIDGE_INTERNAL
} iris_media_bridge_status_t;

typedef struct iris_media_bridge_result_s {
    iris_media_bridge_status_t status;
    char command_id[128];
    char iris_worker_id[128];
    char media_worker_id[128];
    uint64_t dispatch_epoch;
    char terminal_status[16];
    char event_type[96];
    char data[2048];
    const char *error_code;
    const char *error_message;
} iris_media_bridge_result_t;

typedef ivr_status_t (*iris_media_bridge_send_fn)(
    void *context, const ivr_media_command_t *command,
    char *out_media_worker_id, size_t out_media_worker_id_capacity);
typedef ivr_status_t (*iris_media_bridge_observe_fn)(
    void *context, const ivr_media_command_t *command,
    iris_resource_observation_t *observation);

typedef uint64_t (*iris_media_bridge_realtime_ms_fn)(void *context);

typedef struct iris_media_bridge_config_s {
    size_t correlation_capacity;
    iris_media_bridge_send_fn send;
    void *send_context;
    iris_media_bridge_observe_fn observe;
    void *observe_context;
    iris_media_bridge_realtime_ms_fn realtime_ms;
    void *realtime_context;
    iris_command_ledger_port_t ledger;
} iris_media_bridge_config_t;

typedef struct iris_media_completion_s {
    char command_id[128];
    char tenant_id[128];
    char provider_session_id[128];
    char iris_worker_id[128];
    char correlation_id[128];
    uint64_t dispatch_epoch;
    /* Optional canonical terminal fields for non-media commands sharing the
       provider completion lane. Empty fields retain the media defaults. */
    char terminal_status[16];
    char event_type[96];
    char result_json[2048];
} iris_media_completion_t;

iris_media_bridge_t *iris_media_bridge_create(
    const iris_media_bridge_config_t *config);
void iris_media_bridge_destroy(iris_media_bridge_t *bridge);

/* Parses one owning Iris command envelope and dispatches at most one media
   side effect for each immutable commandId. idempotency_key is a borrowed
   request-header view and must equal commandId. */
iris_media_bridge_result_t iris_media_bridge_dispatch_json(
    iris_media_bridge_t *bridge, const char *idempotency_key,
    const char *body, size_t body_size);

/* Claims an accepted correlation for one terminal media result. The result is
   an owning value. A claimed entry must be released on Iris success or restored
   when enqueue fails. */
ivr_status_t iris_media_bridge_claim_completion(
    iris_media_bridge_t *bridge, const ivr_media_command_result_t *result,
    iris_media_completion_t *out);
ivr_status_t iris_media_bridge_refresh_completion(
    iris_media_bridge_t *bridge, const char *command_id,
    iris_media_completion_t *out);
void iris_media_bridge_restore_completion(iris_media_bridge_t *bridge,
                                          const char *command_id);
void iris_media_bridge_release_completion(iris_media_bridge_t *bridge,
                                          const char *command_id);

#ifdef __cplusplus
}
#endif

#endif
