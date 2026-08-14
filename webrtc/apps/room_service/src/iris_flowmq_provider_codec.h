#ifndef ROOM_SERVICE_IRIS_FLOWMQ_PROVIDER_CODEC_H
#define ROOM_SERVICE_IRIS_FLOWMQ_PROVIDER_CODEC_H

#include "iris_media_bridge.h"

#include <data_bind.h>
#include <flowmq_media_provider_v1.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iris_flowmq_provider_command_s {
    ProviderCommandV1_t wire;
    char *bridge_json;
    size_t bridge_json_size;
    uint64_t dispatch_epoch;
} iris_flowmq_provider_command_t;

typedef struct iris_flowmq_completion_ack_s {
    ProviderCompletionAckDisposition_t disposition;
    uint64_t committed_sequence;
} iris_flowmq_completion_ack_t;

typedef struct iris_flowmq_event_ack_s {
    ProviderEventAckDisposition_t disposition;
    uint64_t committed_sequence;
} iris_flowmq_event_ack_t;

typedef struct iris_flowmq_provider_observation_s {
    ProviderObservationV1_t wire;
    uint64_t revision;
    uint64_t cursor;
    uint64_t next_cursor;
} iris_flowmq_provider_observation_t;

#define IRIS_FLOWMQ_CALL_ID_CAPACITY 256u
#define IRIS_FLOWMQ_CALL_TENANT_CAPACITY 128u
#define IRIS_FLOWMQ_CALL_TIME_CAPACITY 32u
#define IRIS_FLOWMQ_CALL_TRANSPORT_CAPACITY 64u
#define IRIS_FLOWMQ_CALL_ADDRESS_CAPACITY 256u
#define IRIS_FLOWMQ_CALL_PAYLOAD_CAPACITY 1024u
#define IRIS_FLOWMQ_SESSION_ERROR_CAPACITY 512u

/* Owning, bounded first-call envelope. Retries must reuse every field. */
typedef struct iris_flowmq_call_offer_s {
    char ingress_event_id[IRIS_FLOWMQ_CALL_ID_CAPACITY];
    char tenant_id[IRIS_FLOWMQ_CALL_TENANT_CAPACITY];
    char call_id[IRIS_FLOWMQ_CALL_ID_CAPACITY];
    char created_at[IRIS_FLOWMQ_CALL_TIME_CAPACITY];
    char deadline_at[IRIS_FLOWMQ_CALL_TIME_CAPACITY];
    char transport[IRIS_FLOWMQ_CALL_TRANSPORT_CAPACITY];
    char source[IRIS_FLOWMQ_CALL_ADDRESS_CAPACITY];
    char destination[IRIS_FLOWMQ_CALL_ADDRESS_CAPACITY];
    char payload_json[IRIS_FLOWMQ_CALL_PAYLOAD_CAPACITY];
    uint64_t call_generation;
} iris_flowmq_call_offer_t;

typedef struct iris_flowmq_session_bound_s {
    int accepted;
    char message_id[IRIS_FLOWMQ_CALL_ID_CAPACITY];
    char bound_session_id[IRIS_FLOWMQ_CALL_ID_CAPACITY];
    char error_code[IRIS_FLOWMQ_CALL_TRANSPORT_CAPACITY];
    char error_message[IRIS_FLOWMQ_SESSION_ERROR_CAPACITY];
} iris_flowmq_session_bound_t;

void iris_flowmq_provider_command_init(
    iris_flowmq_provider_command_t *command);
void iris_flowmq_provider_command_clear(
    iris_flowmq_provider_command_t *command);

/* Decodes and owns one canonical ProviderCommandV1. The generated wire fields
   and bridge_json remain valid until clear(). */
ivr_status_t iris_flowmq_provider_decode_command(
    DataBind *codec, const void *encoded, size_t encoded_size,
    iris_flowmq_provider_command_t *out);

/* Encodes the durable admission result. The caller owns *out and releases it
   with tbe_typed_serialized_free(). */
ivr_status_t iris_flowmq_provider_encode_receipt(
    const iris_flowmq_provider_command_t *command,
    const iris_media_bridge_result_t *result, const char *producer_id,
    const char *created_at, uint8_t **out, size_t *out_size);

ivr_status_t iris_flowmq_provider_encode_completion(
    const iris_media_completion_t *completion,
    const ivr_media_command_result_t *result, const char *provider_id,
    const char *producer_id, const char *message_id, const char *completed_at,
    uint64_t completed_at_unix_ms, uint8_t **out, size_t *out_size);

ivr_status_t iris_flowmq_provider_decode_completion_ack(
    DataBind *codec, const void *encoded, size_t encoded_size,
    const iris_media_completion_t *completion, const char *provider_id,
    const char *iris_identity, const char *completion_message_id,
    iris_flowmq_completion_ack_t *out);

ivr_status_t iris_flowmq_provider_encode_event(
    const ivr_media_event_t *event, const char *provider_id,
    const char *producer_id, const char *message_id, const char *created_at,
    const char *occurred_at, uint8_t **out, size_t *out_size);

ivr_status_t iris_flowmq_provider_decode_event_ack(
    DataBind *codec, const void *encoded, size_t encoded_size,
    const ivr_media_event_t *event, const char *provider_id,
    const char *iris_identity, const char *event_message_id,
    iris_flowmq_event_ack_t *out);

void iris_flowmq_provider_observation_init(
    iris_flowmq_provider_observation_t *observation);
void iris_flowmq_provider_observation_clear(
    iris_flowmq_provider_observation_t *observation);

ivr_status_t iris_flowmq_provider_encode_query(
    const char *tenant_id, const char *provider_id, const char *producer_id,
    const char *query_id, const char *query_type, const char *created_at,
    const char *deadline_at, uint64_t expected_revision, uint64_t cursor,
    uint32_t limit, const char *payload_json, uint8_t **out,
    size_t *out_size);

/* Decodes one owning Observation and fences it to the outstanding query. The
   generated wire fields remain valid until observation_clear(). */
ivr_status_t iris_flowmq_provider_decode_observation(
    DataBind *codec, const void *encoded, size_t encoded_size,
    const char *tenant_id, const char *provider_id, const char *iris_identity,
    const char *query_id, const char *query_type, uint64_t expected_cursor,
    iris_flowmq_provider_observation_t *out);

ivr_status_t iris_flowmq_provider_encode_call_offer(
    const iris_flowmq_call_offer_t *offer, const char *provider_id,
    const char *producer_id, uint8_t **out, size_t *out_size);

ivr_status_t iris_flowmq_provider_decode_session_bound(
    DataBind *codec, const void *encoded, size_t encoded_size,
    const iris_flowmq_call_offer_t *offer, const char *provider_id,
    const char *iris_identity, iris_flowmq_session_bound_t *out);

#ifdef __cplusplus
}
#endif

#endif
