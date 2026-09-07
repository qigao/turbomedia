#ifndef ROOM_SERVICE_IRIS_CONTROL_PROVIDER_H
#define ROOM_SERVICE_IRIS_CONTROL_PROVIDER_H

#include "iris_control_provider_codec.h"

#include <cnet/cnet.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iris_control_provider_s iris_control_provider_t;

typedef iris_media_bridge_result_t (*iris_control_provider_dispatch_fn)(
    void *context, const char *idempotency_key, const char *body,
    size_t body_size);
typedef int (*iris_control_provider_now_fn)(void *context, char *out,
                                           size_t capacity);

typedef struct iris_control_provider_query_s {
    const char *tenant_id;
    const char *query_id;
    const char *query_type;
    const char *created_at;
    const char *deadline_at;
    uint64_t expected_revision;
    uint64_t cursor;
    uint32_t limit;
    const char *payload_json;
} iris_control_provider_query_t;

typedef struct iris_control_provider_config_s {
    int use_tls;
    const char *host;
    uint16_t port;
    const char *path;
    const char *provider_id;
    const char *provider_instance_id;
    const char *iris_identity;
    const char *ca_file;
    const char *certificate_file;
    const char *private_key_file;
    const char *private_key_password;
    const char *server_name;
    int allow_insecure_development_loopback;
    size_t maximum_frame_bytes;
    size_t maximum_ingress_messages;
    size_t maximum_ingress_bytes;
    size_t send_queue_capacity;
    size_t send_queue_bytes;
    uint32_t start_timeout_ms;
    uint64_t reconnect_initial_ms;
    uint64_t reconnect_max_ms;
    iris_control_provider_dispatch_fn dispatch;
    void *dispatch_context;
    iris_control_provider_now_fn now;
    void *now_context;
} iris_control_provider_config_t;

void iris_control_provider_config_init(iris_control_provider_config_t *config);
int iris_control_provider_config_validate(
    const iris_control_provider_config_t *config);

iris_control_provider_t *iris_control_provider_create(
    const iris_control_provider_config_t *config);
int iris_control_provider_start(iris_control_provider_t *provider);
int iris_control_provider_stop(iris_control_provider_t *provider);
int iris_control_provider_destroy(iris_control_provider_t *provider);
int iris_control_provider_running(const iris_control_provider_t *provider);

/* One owning caller may wait for an application-level completion ack at a
   time. The transport send completion is not a durable Iris acknowledgement. */
ivr_status_t iris_control_provider_send_completion(
    iris_control_provider_t *provider,
    const iris_media_completion_t *completion,
    const ivr_media_command_result_t *result, const char *message_id,
    const char *completed_at, uint64_t completed_at_unix_ms,
    uint64_t ack_timeout_ms, iris_control_completion_ack_t *out_ack);

/* One owning caller may wait for a durable event ack at a time. The event and
   message identifiers must remain stable across retries. */
ivr_status_t iris_control_provider_send_event(
    iris_control_provider_t *provider, const ivr_media_event_t *event,
    const char *message_id, const char *occurred_at, uint64_t ack_timeout_ms,
    iris_control_event_ack_t *out_ack);

/* One owner may issue one revision-fenced query at a time. The returned
   observation owns generated strings and must be cleared by the caller. */
ivr_status_t iris_control_provider_send_query(
    iris_control_provider_t *provider,
    const iris_control_provider_query_t *query, uint64_t timeout_ms,
    iris_control_provider_observation_t *out_observation);

/* One signaling owner may wait for one first-call binding at a time. Timeout,
   disconnect and shutdown leave the outcome ambiguous; retry the unchanged
   owning offer to recover Iris's durable binding. */
ivr_status_t iris_control_provider_send_call_offer(
    iris_control_provider_t *provider,
    const iris_control_call_offer_t *offer, uint64_t timeout_ms,
    iris_control_session_bound_t *out_bound);

#ifdef __cplusplus
}
#endif

#endif
