#include "iris_provider_protocol.h"

#include "salts_error.h"
#include "salts_vstr.h"

#include <stdint.h>
#include <string.h>

typedef struct iris_provider_envelope_view_s {
  uint32_t schema_version;
  ProviderMessageKind_t message_kind;
  tbe_var_data_t message_id;
  tbe_var_data_t correlation_id;
  tbe_var_data_t causation_id;
  tbe_var_data_t tenant_id;
  tbe_var_data_t provider_id;
  tbe_var_data_t session_id;
  tbe_var_data_t partition_key;
  tbe_var_data_t producer_id;
  tbe_var_data_t created_at;
  tbe_var_data_t deadline_at;
} iris_provider_envelope_view_t;

#define IRIS_REQUIRE_VIEW(record, view)                                                          \
  do {                                                                                             \
    if (!(view) || !(view)->data) return SALTS_EINVAL;                                             \
    if ((view)->size < record##_BLOCK_LENGTH) return SALTS_EPROTO;                                 \
  } while (0)

#define IRIS_READ_FIELD(record, view, member, output)                                            \
  do {                                                                                             \
    if (!record##_##member((view), &(output))) return SALTS_EPROTO;                                \
  } while (0)

#define IRIS_READ_ENVELOPE(record, view, envelope)                                               \
  do {                                                                                             \
    (envelope).schema_version = record##_schema_version_get((view));                               \
    (envelope).message_kind = record##_message_kind_get((view));                                   \
    IRIS_READ_FIELD(record, view, message_id, (envelope).message_id);                            \
    IRIS_READ_FIELD(record, view, correlation_id, (envelope).correlation_id);                    \
    IRIS_READ_FIELD(record, view, causation_id, (envelope).causation_id);                        \
    IRIS_READ_FIELD(record, view, tenant_id, (envelope).tenant_id);                              \
    IRIS_READ_FIELD(record, view, provider_id, (envelope).provider_id);                          \
    IRIS_READ_FIELD(record, view, session_id, (envelope).session_id);                            \
    IRIS_READ_FIELD(record, view, partition_key, (envelope).partition_key);                      \
    IRIS_READ_FIELD(record, view, producer_id, (envelope).producer_id);                          \
    IRIS_READ_FIELD(record, view, created_at, (envelope).created_at);                            \
    IRIS_READ_FIELD(record, view, deadline_at, (envelope).deadline_at);                          \
  } while (0)

#define IRIS_VALIDATE_TEXT(value, maximum, required)                                             \
  do {                                                                                             \
    rc = iris_provider_text((value), (maximum), (required));                               \
    if (rc != SALTS_OK) return rc;                                                                 \
  } while (0)

static int iris_provider_text(tbe_var_data_t value, size_t maximum, int required) {
  if (!value.data) return SALTS_EPROTO;
  if (required && value.size == 0u) return SALTS_EPROTO;
  if (value.size > maximum) return SALTS_EMSGSIZE;
  if (memchr(value.data, '\0', value.size)) return SALTS_EPROTO;
  return vstr_utf8_valid(
             vstr_from_buf((const char *)value.data, value.size))
             ? SALTS_OK : SALTS_ECHARSET;
}

static int iris_provider_limits_validate(const iris_provider_limits_t *limits) {
  return limits && limits->maximum_id_bytes != 0u && limits->maximum_type_bytes != 0u &&
                 limits->maximum_timestamp_bytes != 0u && limits->maximum_payload_bytes != 0u &&
                 limits->maximum_error_bytes != 0u && limits->maximum_cursor_bytes != 0u
             ? SALTS_OK
             : SALTS_EINVAL;
}

static int
iris_provider_envelope_validate(const iris_provider_envelope_view_t *envelope,
                                        const iris_provider_limits_t *limits,
                                        int require_session, ProviderMessageKind_t expected_kind) {
  int rc;
  if (!envelope || iris_provider_limits_validate(limits) != SALTS_OK) return SALTS_EINVAL;
  if (envelope->schema_version != IRIS_PROVIDER_SCHEMA_VERSION) return SALTS_ENOTSUP;
  if (!ProviderMessageKind_is_valid(envelope->message_kind) ||
      envelope->message_kind != expected_kind)
    return SALTS_EPROTO;
  IRIS_VALIDATE_TEXT(envelope->message_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(envelope->correlation_id, limits->maximum_id_bytes, 0);
  IRIS_VALIDATE_TEXT(envelope->causation_id, limits->maximum_id_bytes, 0);
  IRIS_VALIDATE_TEXT(envelope->tenant_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(envelope->provider_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(envelope->session_id, limits->maximum_id_bytes, require_session);
  IRIS_VALIDATE_TEXT(envelope->partition_key, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(envelope->producer_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(envelope->created_at, limits->maximum_timestamp_bytes, 1);
  IRIS_VALIDATE_TEXT(envelope->deadline_at, limits->maximum_timestamp_bytes, 0);
  return SALTS_OK;
}

static int iris_provider_parse_u64_slice(const uint8_t *text, size_t size,
                                                 uint64_t *value) {
  uint64_t parsed = 0u;
  size_t index;
  if (!text || !value) return SALTS_EINVAL;
  if (size == 0u || (size > 1u && text[0] == (uint8_t)'0')) return SALTS_EPROTO;
  for (index = 0u; index < size; ++index) {
    uint64_t digit;
    if (text[index] < (uint8_t)'0' || text[index] > (uint8_t)'9') return SALTS_EPROTO;
    digit = (uint64_t)(text[index] - (uint8_t)'0');
    if (parsed > (UINT64_MAX - digit) / 10u) return SALTS_ERANGE;
    parsed = parsed * 10u + digit;
  }
  *value = parsed;
  return SALTS_OK;
}

int iris_provider_parse_u64(const char *text, uint64_t *value) {
  if (!text) return SALTS_EINVAL;
  return iris_provider_parse_u64_slice((const uint8_t *)text, strlen(text), value);
}

int iris_provider_peek_kind(const void *encoded, size_t encoded_size,
                                    ProviderMessageKind_t *kind) {
  ProviderCommandV1_view_t view;
  ProviderMessageKind_t parsed;
  if (!encoded || !kind) return SALTS_EINVAL;
  if (!ProviderCommandV1_view_bind(&view, encoded, encoded_size)) return SALTS_EPROTO;
  if (ProviderCommandV1_schema_version_get(&view) != IRIS_PROVIDER_SCHEMA_VERSION)
    return SALTS_ENOTSUP;
  parsed = ProviderCommandV1_message_kind_get(&view);
  if (!ProviderMessageKind_is_valid(parsed)) return SALTS_EPROTO;
  *kind = parsed;
  return SALTS_OK;
}

int iris_provider_validate_command(const ProviderCommandV1_view_t *message,
                                           const iris_provider_limits_t *limits) {
  iris_provider_envelope_view_t envelope;
  tbe_var_data_t command_id, command_type, worker_id, dispatch_epoch, semantic_fingerprint;
  tbe_var_data_t payload_json;
  uint64_t epoch;
  int rc;
  IRIS_REQUIRE_VIEW(ProviderCommandV1, message);
  IRIS_READ_ENVELOPE(ProviderCommandV1, message, envelope);
  rc = iris_provider_envelope_validate(&envelope, limits, 1, ProviderMessageKind_Command);
  if (rc != SALTS_OK) return rc;
  IRIS_READ_FIELD(ProviderCommandV1, message, command_id, command_id);
  IRIS_READ_FIELD(ProviderCommandV1, message, command_type, command_type);
  IRIS_READ_FIELD(ProviderCommandV1, message, worker_id, worker_id);
  IRIS_READ_FIELD(ProviderCommandV1, message, dispatch_epoch, dispatch_epoch);
  IRIS_READ_FIELD(ProviderCommandV1, message, semantic_fingerprint, semantic_fingerprint);
  IRIS_READ_FIELD(ProviderCommandV1, message, payload_json, payload_json);
  IRIS_VALIDATE_TEXT(command_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(command_type, limits->maximum_type_bytes, 1);
  IRIS_VALIDATE_TEXT(worker_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(semantic_fingerprint, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(payload_json, limits->maximum_payload_bytes, 1);
  rc = iris_provider_parse_u64_slice(dispatch_epoch.data, dispatch_epoch.size, &epoch);
  return rc == SALTS_OK && epoch == 0u ? SALTS_EPROTO : rc;
}

int iris_provider_validate_receipt(const ProviderReceiptV1_view_t *message,
                                           const iris_provider_limits_t *limits) {
  iris_provider_envelope_view_t envelope;
  tbe_var_data_t command_id, worker_id, dispatch_epoch, error_code, error_message;
  uint64_t epoch;
  int rc;
  IRIS_REQUIRE_VIEW(ProviderReceiptV1, message);
  IRIS_READ_ENVELOPE(ProviderReceiptV1, message, envelope);
  rc = iris_provider_envelope_validate(&envelope, limits, 1, ProviderMessageKind_Receipt);
  if (rc != SALTS_OK) return rc;
  if (!ProviderReceiptDisposition_is_valid(ProviderReceiptV1_disposition_get(message)))
    return SALTS_EPROTO;
  IRIS_READ_FIELD(ProviderReceiptV1, message, command_id, command_id);
  IRIS_READ_FIELD(ProviderReceiptV1, message, worker_id, worker_id);
  IRIS_READ_FIELD(ProviderReceiptV1, message, dispatch_epoch, dispatch_epoch);
  IRIS_READ_FIELD(ProviderReceiptV1, message, error_code, error_code);
  IRIS_READ_FIELD(ProviderReceiptV1, message, error_message, error_message);
  IRIS_VALIDATE_TEXT(command_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(worker_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(error_code, limits->maximum_type_bytes, 0);
  IRIS_VALIDATE_TEXT(error_message, limits->maximum_error_bytes, 0);
  rc = iris_provider_parse_u64_slice(dispatch_epoch.data, dispatch_epoch.size, &epoch);
  return rc == SALTS_OK && epoch == 0u ? SALTS_EPROTO : rc;
}

int iris_provider_validate_completion(const ProviderCompletionV1_view_t *message,
                                              const iris_provider_limits_t *limits) {
  iris_provider_envelope_view_t envelope;
  tbe_var_data_t command_id, worker_id, dispatch_epoch, event_id, event_type, completed_at;
  tbe_var_data_t completed_at_unix_ms, result_json, error_code, error_message;
  uint64_t epoch, completed_at_ms;
  int rc;
  IRIS_REQUIRE_VIEW(ProviderCompletionV1, message);
  IRIS_READ_ENVELOPE(ProviderCompletionV1, message, envelope);
  rc =
      iris_provider_envelope_validate(&envelope, limits, 1, ProviderMessageKind_Completion);
  if (rc != SALTS_OK) return rc;
  if (!ProviderTerminalStatus_is_valid(ProviderCompletionV1_terminal_status_get(message)))
    return SALTS_EPROTO;
  IRIS_READ_FIELD(ProviderCompletionV1, message, command_id, command_id);
  IRIS_READ_FIELD(ProviderCompletionV1, message, worker_id, worker_id);
  IRIS_READ_FIELD(ProviderCompletionV1, message, dispatch_epoch, dispatch_epoch);
  IRIS_READ_FIELD(ProviderCompletionV1, message, event_id, event_id);
  IRIS_READ_FIELD(ProviderCompletionV1, message, event_type, event_type);
  IRIS_READ_FIELD(ProviderCompletionV1, message, completed_at, completed_at);
  IRIS_READ_FIELD(ProviderCompletionV1, message, completed_at_unix_ms, completed_at_unix_ms);
  IRIS_READ_FIELD(ProviderCompletionV1, message, result_json, result_json);
  IRIS_READ_FIELD(ProviderCompletionV1, message, error_code, error_code);
  IRIS_READ_FIELD(ProviderCompletionV1, message, error_message, error_message);
  IRIS_VALIDATE_TEXT(command_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(worker_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(event_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(event_type, limits->maximum_type_bytes, 1);
  IRIS_VALIDATE_TEXT(completed_at, limits->maximum_timestamp_bytes, 1);
  IRIS_VALIDATE_TEXT(completed_at_unix_ms, limits->maximum_timestamp_bytes, 1);
  IRIS_VALIDATE_TEXT(result_json, limits->maximum_payload_bytes, 1);
  IRIS_VALIDATE_TEXT(error_code, limits->maximum_type_bytes, 0);
  IRIS_VALIDATE_TEXT(error_message, limits->maximum_error_bytes, 0);
  rc = iris_provider_parse_u64_slice(dispatch_epoch.data, dispatch_epoch.size, &epoch);
  if (rc != SALTS_OK || epoch == 0u) return rc == SALTS_OK ? SALTS_EPROTO : rc;
  rc = iris_provider_parse_u64_slice(completed_at_unix_ms.data, completed_at_unix_ms.size,
                                             &completed_at_ms);
  return rc == SALTS_OK && completed_at_ms == 0u ? SALTS_EPROTO : rc;
}

int iris_provider_validate_completion_ack(const ProviderCompletionAckV1_view_t *message,
                                                  const iris_provider_limits_t *limits) {
  iris_provider_envelope_view_t envelope;
  tbe_var_data_t command_id, dispatch_epoch, committed_sequence, error_code, error_message;
  uint64_t epoch, sequence;
  int rc;
  IRIS_REQUIRE_VIEW(ProviderCompletionAckV1, message);
  IRIS_READ_ENVELOPE(ProviderCompletionAckV1, message, envelope);
  rc = iris_provider_envelope_validate(&envelope, limits, 1,
                                               ProviderMessageKind_CompletionAck);
  if (rc != SALTS_OK) return rc;
  if (!ProviderCompletionAckDisposition_is_valid(ProviderCompletionAckV1_disposition_get(message)))
    return SALTS_EPROTO;
  IRIS_READ_FIELD(ProviderCompletionAckV1, message, command_id, command_id);
  IRIS_READ_FIELD(ProviderCompletionAckV1, message, dispatch_epoch, dispatch_epoch);
  IRIS_READ_FIELD(ProviderCompletionAckV1, message, committed_sequence, committed_sequence);
  IRIS_READ_FIELD(ProviderCompletionAckV1, message, error_code, error_code);
  IRIS_READ_FIELD(ProviderCompletionAckV1, message, error_message, error_message);
  IRIS_VALIDATE_TEXT(command_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(error_code, limits->maximum_type_bytes, 0);
  IRIS_VALIDATE_TEXT(error_message, limits->maximum_error_bytes, 0);
  rc = iris_provider_parse_u64_slice(dispatch_epoch.data, dispatch_epoch.size, &epoch);
  if (rc != SALTS_OK || epoch == 0u) return rc == SALTS_OK ? SALTS_EPROTO : rc;
  return iris_provider_parse_u64_slice(committed_sequence.data, committed_sequence.size,
                                               &sequence);
}

int iris_provider_validate_event(const ProviderEventV1_view_t *message,
                                         const iris_provider_limits_t *limits) {
  iris_provider_envelope_view_t envelope;
  tbe_var_data_t event_id, event_type, aggregate_id, sequence_text, occurred_at, payload_json;
  uint64_t sequence;
  int rc;
  IRIS_REQUIRE_VIEW(ProviderEventV1, message);
  IRIS_READ_ENVELOPE(ProviderEventV1, message, envelope);
  rc = iris_provider_envelope_validate(&envelope, limits, 1, ProviderMessageKind_Event);
  if (rc != SALTS_OK) return rc;
  IRIS_READ_FIELD(ProviderEventV1, message, event_id, event_id);
  IRIS_READ_FIELD(ProviderEventV1, message, event_type, event_type);
  IRIS_READ_FIELD(ProviderEventV1, message, aggregate_id, aggregate_id);
  IRIS_READ_FIELD(ProviderEventV1, message, sequence, sequence_text);
  IRIS_READ_FIELD(ProviderEventV1, message, occurred_at, occurred_at);
  IRIS_READ_FIELD(ProviderEventV1, message, payload_json, payload_json);
  IRIS_VALIDATE_TEXT(event_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(event_type, limits->maximum_type_bytes, 1);
  IRIS_VALIDATE_TEXT(aggregate_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(occurred_at, limits->maximum_timestamp_bytes, 1);
  IRIS_VALIDATE_TEXT(payload_json, limits->maximum_payload_bytes, 1);
  return iris_provider_parse_u64_slice(sequence_text.data, sequence_text.size, &sequence);
}

int iris_provider_validate_event_ack(const ProviderEventAckV1_view_t *message,
                                             const iris_provider_limits_t *limits) {
  iris_provider_envelope_view_t envelope;
  tbe_var_data_t event_id, committed_sequence, error_code, error_message;
  uint64_t sequence;
  int rc;
  IRIS_REQUIRE_VIEW(ProviderEventAckV1, message);
  IRIS_READ_ENVELOPE(ProviderEventAckV1, message, envelope);
  rc = iris_provider_envelope_validate(&envelope, limits, 1, ProviderMessageKind_EventAck);
  if (rc != SALTS_OK) return rc;
  if (!ProviderEventAckDisposition_is_valid(ProviderEventAckV1_disposition_get(message)))
    return SALTS_EPROTO;
  IRIS_READ_FIELD(ProviderEventAckV1, message, event_id, event_id);
  IRIS_READ_FIELD(ProviderEventAckV1, message, committed_sequence, committed_sequence);
  IRIS_READ_FIELD(ProviderEventAckV1, message, error_code, error_code);
  IRIS_READ_FIELD(ProviderEventAckV1, message, error_message, error_message);
  IRIS_VALIDATE_TEXT(event_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(error_code, limits->maximum_type_bytes, 0);
  IRIS_VALIDATE_TEXT(error_message, limits->maximum_error_bytes, 0);
  return iris_provider_parse_u64_slice(committed_sequence.data, committed_sequence.size,
                                               &sequence);
}

int iris_provider_validate_query(const ProviderQueryV1_view_t *message,
                                         const iris_provider_limits_t *limits) {
  iris_provider_envelope_view_t envelope;
  tbe_var_data_t query_id, query_type, expected_revision, cursor, payload_json;
  uint64_t revision;
  int rc;
  IRIS_REQUIRE_VIEW(ProviderQueryV1, message);
  IRIS_READ_ENVELOPE(ProviderQueryV1, message, envelope);
  rc = iris_provider_envelope_validate(&envelope, limits, 0, ProviderMessageKind_Query);
  if (rc != SALTS_OK) return rc;
  if (ProviderQueryV1_limit_get(message) == 0u) return SALTS_EPROTO;
  IRIS_READ_FIELD(ProviderQueryV1, message, query_id, query_id);
  IRIS_READ_FIELD(ProviderQueryV1, message, query_type, query_type);
  IRIS_READ_FIELD(ProviderQueryV1, message, expected_revision, expected_revision);
  IRIS_READ_FIELD(ProviderQueryV1, message, cursor, cursor);
  IRIS_READ_FIELD(ProviderQueryV1, message, payload_json, payload_json);
  IRIS_VALIDATE_TEXT(query_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(query_type, limits->maximum_type_bytes, 1);
  IRIS_VALIDATE_TEXT(cursor, limits->maximum_cursor_bytes, 0);
  IRIS_VALIDATE_TEXT(payload_json, limits->maximum_payload_bytes, 1);
  return iris_provider_parse_u64_slice(expected_revision.data, expected_revision.size,
                                               &revision);
}

int iris_provider_validate_observation(const ProviderObservationV1_view_t *message,
                                               const iris_provider_limits_t *limits) {
  iris_provider_envelope_view_t envelope;
  tbe_var_data_t query_id, observation_type, revision_text, cursor, next_cursor, payload_json;
  tbe_var_data_t error_code, error_message;
  uint64_t revision;
  int rc;
  IRIS_REQUIRE_VIEW(ProviderObservationV1, message);
  IRIS_READ_ENVELOPE(ProviderObservationV1, message, envelope);
  rc = iris_provider_envelope_validate(&envelope, limits, 0,
                                               ProviderMessageKind_Observation);
  if (rc != SALTS_OK) return rc;
  if (!ProviderQueryStatus_is_valid(ProviderObservationV1_status_get(message)) ||
      ProviderObservationV1_has_more_get(message) > 1u)
    return SALTS_EPROTO;
  IRIS_READ_FIELD(ProviderObservationV1, message, query_id, query_id);
  IRIS_READ_FIELD(ProviderObservationV1, message, observation_type, observation_type);
  IRIS_READ_FIELD(ProviderObservationV1, message, revision, revision_text);
  IRIS_READ_FIELD(ProviderObservationV1, message, cursor, cursor);
  IRIS_READ_FIELD(ProviderObservationV1, message, next_cursor, next_cursor);
  IRIS_READ_FIELD(ProviderObservationV1, message, payload_json, payload_json);
  IRIS_READ_FIELD(ProviderObservationV1, message, error_code, error_code);
  IRIS_READ_FIELD(ProviderObservationV1, message, error_message, error_message);
  IRIS_VALIDATE_TEXT(query_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(observation_type, limits->maximum_type_bytes, 1);
  IRIS_VALIDATE_TEXT(cursor, limits->maximum_cursor_bytes, 0);
  IRIS_VALIDATE_TEXT(next_cursor, limits->maximum_cursor_bytes, 0);
  IRIS_VALIDATE_TEXT(payload_json, limits->maximum_payload_bytes, 1);
  IRIS_VALIDATE_TEXT(error_code, limits->maximum_type_bytes, 0);
  IRIS_VALIDATE_TEXT(error_message, limits->maximum_error_bytes, 0);
  return iris_provider_parse_u64_slice(revision_text.data, revision_text.size, &revision);
}

int iris_provider_validate_call_offer(const ProviderCallOfferV1_view_t *message,
                                              const iris_provider_limits_t *limits) {
  iris_provider_envelope_view_t envelope;
  tbe_var_data_t ingress_event_id, call_id, call_generation, transport, source, destination;
  tbe_var_data_t payload_json;
  uint64_t generation;
  int rc;
  IRIS_REQUIRE_VIEW(ProviderCallOfferV1, message);
  IRIS_READ_ENVELOPE(ProviderCallOfferV1, message, envelope);
  rc = iris_provider_envelope_validate(&envelope, limits, 0, ProviderMessageKind_CallOffer);
  if (rc != SALTS_OK) return rc;
  IRIS_READ_FIELD(ProviderCallOfferV1, message, ingress_event_id, ingress_event_id);
  IRIS_READ_FIELD(ProviderCallOfferV1, message, call_id, call_id);
  IRIS_READ_FIELD(ProviderCallOfferV1, message, call_generation, call_generation);
  IRIS_READ_FIELD(ProviderCallOfferV1, message, transport, transport);
  IRIS_READ_FIELD(ProviderCallOfferV1, message, source, source);
  IRIS_READ_FIELD(ProviderCallOfferV1, message, destination, destination);
  IRIS_READ_FIELD(ProviderCallOfferV1, message, payload_json, payload_json);
  IRIS_VALIDATE_TEXT(ingress_event_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(call_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(transport, limits->maximum_type_bytes, 1);
  IRIS_VALIDATE_TEXT(source, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(destination, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(payload_json, limits->maximum_payload_bytes, 1);
  return iris_provider_parse_u64_slice(call_generation.data, call_generation.size,
                                               &generation);
}

int iris_provider_validate_session_bound(const ProviderSessionBoundV1_view_t *message,
                                                 const iris_provider_limits_t *limits) {
  iris_provider_envelope_view_t envelope;
  tbe_var_data_t ingress_event_id, call_id, call_generation, bound_session_id;
  tbe_var_data_t error_code, error_message;
  uint64_t generation;
  uint8_t accepted;
  int rc;
  IRIS_REQUIRE_VIEW(ProviderSessionBoundV1, message);
  IRIS_READ_ENVELOPE(ProviderSessionBoundV1, message, envelope);
  accepted = ProviderSessionBoundV1_accepted_get(message);
  if (accepted > 1u) return SALTS_EPROTO;
  rc = iris_provider_envelope_validate(&envelope, limits, accepted != 0u,
                                               ProviderMessageKind_SessionBound);
  if (rc != SALTS_OK) return rc;
  IRIS_READ_FIELD(ProviderSessionBoundV1, message, ingress_event_id, ingress_event_id);
  IRIS_READ_FIELD(ProviderSessionBoundV1, message, call_id, call_id);
  IRIS_READ_FIELD(ProviderSessionBoundV1, message, call_generation, call_generation);
  IRIS_READ_FIELD(ProviderSessionBoundV1, message, bound_session_id, bound_session_id);
  IRIS_READ_FIELD(ProviderSessionBoundV1, message, error_code, error_code);
  IRIS_READ_FIELD(ProviderSessionBoundV1, message, error_message, error_message);
  IRIS_VALIDATE_TEXT(ingress_event_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(call_id, limits->maximum_id_bytes, 1);
  IRIS_VALIDATE_TEXT(bound_session_id, limits->maximum_id_bytes, accepted != 0u);
  IRIS_VALIDATE_TEXT(error_code, limits->maximum_type_bytes, 0);
  IRIS_VALIDATE_TEXT(error_message, limits->maximum_error_bytes, 0);
  return iris_provider_parse_u64_slice(call_generation.data, call_generation.size,
                                               &generation);
}

#undef IRIS_VALIDATE_TEXT
#undef IRIS_READ_ENVELOPE
#undef IRIS_READ_FIELD
#undef IRIS_REQUIRE_VIEW
