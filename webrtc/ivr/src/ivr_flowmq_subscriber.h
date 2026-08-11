#ifndef TURBO_MEDIA_IVR_FLOWMQ_SUBSCRIBER_H
#define TURBO_MEDIA_IVR_FLOWMQ_SUBSCRIBER_H

/**
 * @file ivr_flowmq_subscriber.h
 * @brief Worker-side FlowMQ SUB domain-event subscriber.
 *
 * Subscribes to the RoomService PUB topic, decodes TIVR event frames
 * (generated schema typed messages) into an ivr_event_view_t and forwards it
 * to on_event. The view is borrowed for the callback only (the decoded object
 * is released when the callback returns); the callback must copy to retain.
 * Workers feed these events into the matching per-call session inbox.
 */

#include "ivr/ivr_worker.h"
#include "data_bind.h"

typedef struct turbo_flow_fmq_security_binding_s
    turbo_flow_fmq_security_binding_t;
typedef struct turbo_flow_fmq_tls_config_s turbo_flow_fmq_tls_config_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ivr_flowmq_subscriber_s ivr_flowmq_subscriber_t;

typedef struct {
    /* CONNECT authentication identity. Required when security is non-NULL and
       must match the worker identity used by the command DEALER. */
    const char *identity;
    const char *host; /* PUB host */
    int port;
    int transport;    /* turbo_flow_fmq_transport_t; 0 = TCP */
    const char *path; /* WS/WSS path; NULL = "/" */
    const char *topic; /* SUB prefix; NULL/"" subscribes to all topics */
    uint64_t timeout_ms;
    /* Borrowed object-level TLS/WSS material; FlowMQ copies it during create. */
    const turbo_flow_fmq_tls_config_t *tls;
    /* Borrowed secure binding; NULL is reserved for explicit trusted/test
       deployments and selects the legacy facade. */
    const turbo_flow_fmq_security_binding_t *security;
    void (*on_event)(void *ctx, const ivr_event_view_t *event);
    void *event_ctx;
    void (*on_connection)(void *ctx, int connected);
    void *connection_ctx;
} ivr_flowmq_subscriber_config_t;

ivr_status_t ivr_flowmq_subscriber_create(
    const ivr_flowmq_subscriber_config_t *config,
    ivr_flowmq_subscriber_t **out_subscriber);
ivr_status_t ivr_flowmq_subscriber_start(ivr_flowmq_subscriber_t *subscriber);
void ivr_flowmq_subscriber_destroy(ivr_flowmq_subscriber_t *subscriber);

/* One self-contained decoded event: the event view (ivr_subscriber_decoded_view)
   is valid until ivr_subscriber_decoded_free(). */
typedef struct ivr_subscriber_decoded_s ivr_subscriber_decoded_t;

/* Pure decode of one TIVR event frame. Returns IVR_OK and *out_decoded on
   success; IVR_ESTATE for non-event frames, unknown event types, or
   malformed/unsupported payloads (fail fast, never partially decodes). */
ivr_status_t ivr_flowmq_subscriber_decode(DataBind *codec, const uint8_t *frame,
                                          size_t len,
                                          ivr_subscriber_decoded_t **out_decoded);

/* Borrowed event view; valid until ivr_subscriber_decoded_free(). */
const ivr_event_view_t *ivr_subscriber_decoded_view(
    const ivr_subscriber_decoded_t *decoded);

void ivr_subscriber_decoded_free(ivr_subscriber_decoded_t *decoded);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_IVR_FLOWMQ_SUBSCRIBER_H */
