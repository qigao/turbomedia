#ifndef ROOM_SERVICE_IRIS_PROVIDER_PROTOCOL_H
#define ROOM_SERVICE_IRIS_PROVIDER_PROTOCOL_H

#include "turbomedia_iris_provider_v1.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IRIS_PROVIDER_SCHEMA_VERSION 1u

typedef struct iris_provider_limits_s {
    size_t maximum_id_bytes;
    size_t maximum_type_bytes;
    size_t maximum_timestamp_bytes;
    size_t maximum_payload_bytes;
    size_t maximum_error_bytes;
    size_t maximum_cursor_bytes;
} iris_provider_limits_t;

#define IRIS_PROVIDER_LIMITS_INIT {256u, 128u, 64u, 1024u * 1024u, 4096u, 1024u}

int iris_provider_parse_u64(const char *text, uint64_t *value);
int iris_provider_peek_kind(const void *encoded, size_t encoded_size,
                            ProviderMessageKind_t *kind);
int iris_provider_validate_command(const ProviderCommandV1_view_t *message,
                                   const iris_provider_limits_t *limits);
int iris_provider_validate_receipt(const ProviderReceiptV1_view_t *message,
                                   const iris_provider_limits_t *limits);
int iris_provider_validate_completion(const ProviderCompletionV1_view_t *message,
                                      const iris_provider_limits_t *limits);
int iris_provider_validate_completion_ack(
    const ProviderCompletionAckV1_view_t *message,
    const iris_provider_limits_t *limits);
int iris_provider_validate_event(const ProviderEventV1_view_t *message,
                                 const iris_provider_limits_t *limits);
int iris_provider_validate_event_ack(const ProviderEventAckV1_view_t *message,
                                     const iris_provider_limits_t *limits);
int iris_provider_validate_query(const ProviderQueryV1_view_t *message,
                                 const iris_provider_limits_t *limits);
int iris_provider_validate_observation(
    const ProviderObservationV1_view_t *message,
    const iris_provider_limits_t *limits);
int iris_provider_validate_call_offer(const ProviderCallOfferV1_view_t *message,
                                      const iris_provider_limits_t *limits);
int iris_provider_validate_session_bound(
    const ProviderSessionBoundV1_view_t *message,
    const iris_provider_limits_t *limits);

#ifdef __cplusplus
}
#endif

#endif
