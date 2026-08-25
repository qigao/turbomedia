#ifndef TURBO_ROOM_SERVICE_IRIS_COMPLETION_DISPATCHER_H
#define TURBO_ROOM_SERVICE_IRIS_COMPLETION_DISPATCHER_H

#include "iris_media_bridge.h"
#include "ivr_room_bridge.h"
#include <stddef.h>
#include <stdint.h>

typedef struct iris_completion_dispatcher_s iris_completion_dispatcher_t;

typedef int (*iris_completion_post_fn)(void *context, const char *url,
                                       const char *authorization,
                                       const char *body, size_t body_size);

typedef ivr_status_t (*iris_completion_deliver_fn)(
    void *context, const iris_media_completion_t *completion,
    const ivr_media_command_result_t *result, const char *message_id,
    const char *completed_at, uint64_t completed_at_unix_ms,
    uint64_t ack_timeout_ms);

/* Delivery callback result contract: IVR_OK means committed or duplicate;
   IVR_ESTALE means a terminal identity/content conflict; IVR_EAUTH means a
   terminal peer rejection. Other failures are retryable within the configured
   bounded attempt/deadline policy. */

typedef ivr_status_t (*iris_event_deliver_fn)(
    void *context, const ivr_media_event_t *event, const char *message_id,
    const char *occurred_at, uint64_t ack_timeout_ms);

typedef enum iris_event_delivery_outcome_e {
    IRIS_EVENT_DELIVERY_FAILED = 0,
    IRIS_EVENT_DELIVERY_SUCCEEDED = 1,
    IRIS_EVENT_DELIVERY_ABANDONED = -1
} iris_event_delivery_outcome_t;

typedef void (*iris_event_delivery_result_fn)(
    void *context, const ivr_media_event_t *event, uint64_t delivery_token,
    iris_event_delivery_outcome_t outcome, int http_status);

typedef struct iris_completion_dispatcher_config_s {
    const char *base_url;
    const char *provider_token;
    size_t queue_capacity;
    int retry_max_attempts;
    int retry_backoff_ms;
    int request_timeout_ms;
    int drain_timeout_ms;
    iris_media_bridge_t *bridge;
    iris_completion_post_fn post;
    void *post_context;
    iris_completion_deliver_fn deliver_completion;
    iris_event_deliver_fn deliver_event;
    void *deliver_context;
    iris_event_delivery_result_fn event_delivery_result;
    void *event_delivery_context;
} iris_completion_dispatcher_config_t;

typedef struct iris_completion_dispatcher_stats_s {
    size_t queue_items;
    size_t queue_capacity;
    size_t queue_high_water;
    size_t in_flight;
    uint64_t enqueued_total;
    uint64_t queue_full_total;
    uint64_t closed_rejections_total;
    uint64_t delivery_attempts_total;
    uint64_t retries_total;
    uint64_t fence_conflicts_total;
    uint64_t fence_refresh_failures_total;
    uint64_t completion_success_total;
    uint64_t completion_failure_total;
    uint64_t event_success_total;
    uint64_t event_failure_total;
    uint64_t shutdown_restored_completions_total;
    uint64_t shutdown_dropped_events_total;
    uint64_t last_drain_duration_ms;
    uint64_t max_drain_duration_ms;
} iris_completion_dispatcher_stats_t;

iris_completion_dispatcher_t *iris_completion_dispatcher_create(
    const iris_completion_dispatcher_config_t *config);
int iris_completion_dispatcher_start(iris_completion_dispatcher_t *dispatcher);
void iris_completion_dispatcher_stop(iris_completion_dispatcher_t *dispatcher);
void iris_completion_dispatcher_destroy(iris_completion_dispatcher_t *dispatcher);
void iris_completion_dispatcher_get_stats(
    iris_completion_dispatcher_t *dispatcher,
    iris_completion_dispatcher_stats_t *stats);

/** Control-plane binding; call only before start. */
int iris_completion_dispatcher_set_event_delivery_observer(
    iris_completion_dispatcher_t *dispatcher,
    iris_event_delivery_result_fn observer, void *observer_context);

ivr_status_t iris_completion_dispatcher_on_media_result(
    void *context, const ivr_media_command_result_t *result);

/** Enqueue a terminal fact already committed by a non-media bridge. The
    completion must carry terminal_status, event_type and result_json. */
ivr_status_t iris_completion_dispatcher_enqueue_terminal(
    iris_completion_dispatcher_t *dispatcher,
    const iris_media_completion_t *completion,
    const ivr_media_command_result_t *result, const char *stable_event_id);
ivr_status_t iris_completion_dispatcher_on_media_event(
    void *context, const ivr_media_event_t *event);

/** Enqueue a durable event and preserve its opaque store revision. */
ivr_status_t iris_completion_dispatcher_enqueue_event(
    iris_completion_dispatcher_t *dispatcher, const ivr_media_event_t *event,
    uint64_t delivery_token);

#endif
