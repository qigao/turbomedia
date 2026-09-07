#include "iris_control_process_peer.h"
#include "iris_provider_protocol.h"
#include "ivr_control_ws.h"

#include <turbo_crypto.h>
#include <json_parser.h>
#include <salts_str.h>
#include <salts_thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TEST_IRIS_CONTROL_MAX_FRAME_BYTES = 256 * 1024,
    TEST_IRIS_CONTROL_WAIT_STEP_MS = 5
};
struct test_iris_control_peer_s {
    ivr_control_ws_server_t *server;
    ivr_control_ws_route_t route;
    DataBind *codec;
    atomic_int connected;
    atomic_uint_fast64_t receipt_generation;
    test_iris_control_peer_config_t config;
    salts_mutex_t receipt_mutex;
    test_iris_control_receipt_t receipt;
};

static int copy_text(char *out, size_t capacity, const char *value) {
    size_t size;
    if (!out || capacity == 0u || !value) return 0;
    size = strlen(value);
    if (size >= capacity) return 0;
    memcpy(out, value, size + 1u);
    return 1;
}

static int assign_text(tstr *out, const char *value) {
    *out = tstr_dup(value ? value : "");
    return *out != NULL;
}

static const char *object_text(const json_value_t *object, const char *name) {
    json_value_t *value = object ? json_object_get(object, name) : NULL;
    return value && json_type(value) == JSON_STRING
               ? json_string(value)
               : NULL;
}

static int command_fingerprint(const char *const fields[8], char out[72]) {
    static const char hex[] = "0123456789abcdef";
    turbo_crypto_sha256_ctx_t hash;
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    size_t field_index;
    if (turbo_crypto_sha256_init(&hash) != 0) return 0;
    for (field_index = 0u; field_index < 8u; ++field_index) {
        uint64_t length = (uint64_t)strlen(fields[field_index]);
        uint8_t encoded_length[8];
        size_t byte_index;
        for (byte_index = 0u; byte_index < sizeof(encoded_length);
             ++byte_index) {
            encoded_length[byte_index] =
                (uint8_t)(length >> (byte_index * 8u));
        }
        if (turbo_crypto_sha256_update(&hash, encoded_length,
                                       sizeof(encoded_length)) != 0 ||
            (length > 0u &&
             turbo_crypto_sha256_update(&hash, fields[field_index],
                                        (size_t)length) != 0)) {
            return 0;
        }
    }
    if (turbo_crypto_sha256_final(&hash, digest) != 0) return 0;
    memcpy(out, "sha256:", 7u);
    for (field_index = 0u; field_index < sizeof(digest); ++field_index) {
        out[7u + field_index * 2u] = hex[digest[field_index] >> 4u];
        out[8u + field_index * 2u] = hex[digest[field_index] & 0x0fu];
    }
    out[71] = '\0';
    return 1;
}

static uint8_t *encode_command(test_iris_control_peer_t *peer,
                               const char *idempotency_key,
                               const char *bridge_json, size_t *out_size) {
    json_value_t *root = NULL;
    json_value_t *data;
    json_value_t *epoch_value;
    char *payload_json = NULL;
    size_t payload_size = 0u;
    char epoch[32];
    char fingerprint[72];
    const char *command_id;
    const char *tenant_id;
    const char *session_id;
    const char *command_type;
    const char *provider_id;
    const char *correlation_id;
    const char *causation_id;
    const char *deadline_at;
    const char *worker_id;
    const char *fields[8];
    ProviderCommandV1_t command;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint8_t *encoded = NULL;
    uint64_t dispatch_epoch;
    int written;
    if (!peer || !idempotency_key || !bridge_json || !out_size ||
        ((root = json_parse((const char *)((const uint8_t *)bridge_json), strlen(bridge_json))) ? 0 : -1) != 0 ||
        !root || json_type(root) != JSON_OBJECT) {
        json_free(root);
        root = NULL;
        return NULL;
    }
    command_id = object_text(root, "commandId");
    tenant_id = object_text(root, "tenantId");
    session_id = object_text(root, "sessionId");
    command_type = object_text(root, "type");
    provider_id = object_text(root, "provider");
    correlation_id = object_text(root, "correlationId");
    causation_id = object_text(root, "causationId");
    deadline_at = object_text(root, "deadline");
    worker_id = object_text(root, "workerId");
    epoch_value = json_object_get(root, "dispatchEpoch");
    data = json_object_get(root, "data");
    dispatch_epoch = epoch_value && json_type(epoch_value) == JSON_NUMBER
                         ? (uint64_t)json_number(epoch_value)
                         : 1u;
    command_id = command_id ? command_id : idempotency_key;
    tenant_id = tenant_id ? tenant_id : "tenant-process";
    session_id = session_id ? session_id : "session-process";
    command_type = command_type ? command_type : "test.readiness";
    provider_id = provider_id ? provider_id : "turbomedia";
    correlation_id = correlation_id ? correlation_id : "readiness-process";
    causation_id = causation_id ? causation_id : idempotency_key;
    deadline_at = deadline_at ? deadline_at : "2099-01-01T00:00:00Z";
    worker_id = worker_id ? worker_id : "iris-worker-process";
    written = snprintf(epoch, sizeof(epoch), "%llu",
                       (unsigned long long)dispatch_epoch);
    if (strcmp(command_id, idempotency_key) != 0 || dispatch_epoch == 0u ||
        !data || json_type(data) != JSON_OBJECT || written <= 0 ||
        (size_t)written >= sizeof(epoch)) {
        json_free(root);
        root = NULL;
        return NULL;
    }
    payload_json = json_serialize(data, &payload_size);
    if (!payload_json || payload_size == 0u) {
        json_serialize_free(payload_json);
        json_free(root);
        root = NULL;
        return NULL;
    }
    fields[0] = tenant_id;
    fields[1] = session_id;
    fields[2] = command_type;
    fields[3] = provider_id;
    fields[4] = correlation_id;
    fields[5] = causation_id;
    fields[6] = deadline_at;
    fields[7] = payload_json;
    ProviderCommandV1_init(&command);
    command.schema_version = 1u;
    command.message_kind = ProviderMessageKind_Command;
    if (!command_fingerprint(fields, fingerprint) ||
        !assign_text(&command.message_id, command_id) ||
        !assign_text(&command.correlation_id, correlation_id) ||
        !assign_text(&command.causation_id, causation_id) ||
        !assign_text(&command.tenant_id, tenant_id) ||
        !assign_text(&command.provider_id, provider_id) ||
        !assign_text(&command.session_id, session_id) ||
        !assign_text(&command.partition_key, session_id) ||
        !assign_text(&command.producer_id, "iris-process") ||
        !assign_text(&command.created_at, "2026-08-25T00:00:00Z") ||
        !assign_text(&command.deadline_at, deadline_at) ||
        !assign_text(&command.command_id, command_id) ||
        !assign_text(&command.command_type, command_type) ||
        !assign_text(&command.worker_id, worker_id) ||
        !assign_text(&command.dispatch_epoch, epoch) ||
        !assign_text(&command.semantic_fingerprint, fingerprint) ||
        !assign_text(&command.payload_json, payload_json) ||
        ProviderCommandV1_to_bin(&command, &encoded, out_size, &error) !=
            DATA_BIND_OK) {
        tbe_typed_serialized_free(encoded);
        encoded = NULL;
        *out_size = 0u;
    }
    ProviderCommandV1_clear(&command);
    json_serialize_free(payload_json);
    json_free(root);
    root = NULL;
    return encoded;
}

static int send_application(test_iris_control_peer_t *peer,
                            const ivr_control_ws_route_t *route,
                            const uint8_t *payload,
                            size_t payload_size) {
    return ivr_control_ws_server_send_copy(peer->server, route, payload,
                                            payload_size);
}

static int send_completion_ack(test_iris_control_peer_t *peer,
                               const ivr_control_ws_route_t *route,
                               const ProviderCompletionV1_t *completion) {
    ProviderCompletionAckV1_t ack;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0u;
    int status = SALTS_ENOMEM;
    ProviderCompletionAckV1_init(&ack);
    ack.schema_version = 1u;
    ack.message_kind = ProviderMessageKind_CompletionAck;
    ack.disposition = ProviderCompletionAckDisposition_CompletionCommitted;
    if (assign_text(&ack.message_id, "process-completion-ack") &&
        assign_text(&ack.correlation_id, completion->correlation_id) &&
        assign_text(&ack.causation_id, completion->message_id) &&
        assign_text(&ack.tenant_id, completion->tenant_id) &&
        assign_text(&ack.provider_id, completion->provider_id) &&
        assign_text(&ack.session_id, completion->session_id) &&
        assign_text(&ack.partition_key, completion->session_id) &&
        assign_text(&ack.producer_id, "iris-process") &&
        assign_text(&ack.created_at, "2026-08-25T00:00:01Z") &&
        assign_text(&ack.deadline_at, "") &&
        assign_text(&ack.command_id, completion->command_id) &&
        assign_text(&ack.dispatch_epoch, completion->dispatch_epoch) &&
        assign_text(&ack.committed_sequence, "1") &&
        assign_text(&ack.error_code, "") &&
        assign_text(&ack.error_message, "") &&
        ProviderCompletionAckV1_to_bin(&ack, &encoded, &encoded_size,
                                       &error) == DATA_BIND_OK) {
        status = send_application(peer, route, encoded, encoded_size);
    }
    tbe_typed_serialized_free(encoded);
    ProviderCompletionAckV1_clear(&ack);
    return status;
}

static int send_event_ack(test_iris_control_peer_t *peer,
                          const ivr_control_ws_route_t *route,
                          const ProviderEventV1_t *event) {
    ProviderEventAckV1_t ack;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0u;
    int status = SALTS_ENOMEM;
    ProviderEventAckV1_init(&ack);
    ack.schema_version = 1u;
    ack.message_kind = ProviderMessageKind_EventAck;
    ack.disposition = ProviderEventAckDisposition_Committed;
    if (assign_text(&ack.message_id, "process-event-ack") &&
        assign_text(&ack.correlation_id, event->correlation_id) &&
        assign_text(&ack.causation_id, event->message_id) &&
        assign_text(&ack.tenant_id, event->tenant_id) &&
        assign_text(&ack.provider_id, event->provider_id) &&
        assign_text(&ack.session_id, event->session_id) &&
        assign_text(&ack.partition_key, event->session_id) &&
        assign_text(&ack.producer_id, "iris-process") &&
        assign_text(&ack.created_at, "2026-08-25T00:00:02Z") &&
        assign_text(&ack.deadline_at, "") &&
        assign_text(&ack.event_id, event->event_id) &&
        assign_text(&ack.committed_sequence, "1") &&
        assign_text(&ack.error_code, "") &&
        assign_text(&ack.error_message, "") &&
        ProviderEventAckV1_to_bin(&ack, &encoded, &encoded_size, &error) ==
            DATA_BIND_OK) {
        status = send_application(peer, route, encoded, encoded_size);
    }
    tbe_typed_serialized_free(encoded);
    ProviderEventAckV1_clear(&ack);
    return status;
}

static int send_observation(test_iris_control_peer_t *peer,
                            const ivr_control_ws_route_t *route,
                            const ProviderQueryV1_t *query) {
    ProviderObservationV1_t observation;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0u;
    char payload[4096];
    int available = 0;
    int status = SALTS_EPROTO;
    if (!peer->config.query ||
        !peer->config.query(peer->config.context, payload, sizeof(payload),
                            &available)) {
        return status;
    }
    ProviderObservationV1_init(&observation);
    observation.schema_version = 1u;
    observation.message_kind = ProviderMessageKind_Observation;
    observation.status = available ? ProviderQueryStatus_QueryOk
                                   : ProviderQueryStatus_QueryNotReady;
    observation.has_more = false;
    if (assign_text(&observation.message_id, "process-observation") &&
        assign_text(&observation.correlation_id, query->query_id) &&
        assign_text(&observation.causation_id, query->message_id) &&
        assign_text(&observation.tenant_id, query->tenant_id) &&
        assign_text(&observation.provider_id, query->provider_id) &&
        assign_text(&observation.session_id, "") &&
        assign_text(&observation.partition_key, query->tenant_id) &&
        assign_text(&observation.producer_id, "iris-process") &&
        assign_text(&observation.created_at, "2026-08-25T00:00:03Z") &&
        assign_text(&observation.deadline_at, "") &&
        assign_text(&observation.query_id, query->query_id) &&
        assign_text(&observation.observation_type, query->query_type) &&
        assign_text(&observation.revision, "1") &&
        assign_text(&observation.cursor, query->cursor) &&
        assign_text(&observation.next_cursor, "0") &&
        assign_text(&observation.payload_json, payload) &&
        assign_text(&observation.error_code,
                    available ? "" : "EXPECTED_RESOURCES_UNAVAILABLE") &&
        assign_text(&observation.error_message,
                    available ? "" : "expected resources are unavailable") &&
        ProviderObservationV1_to_bin(&observation, &encoded, &encoded_size,
                                     &error) == DATA_BIND_OK) {
        status = send_application(peer, route, encoded, encoded_size);
    }
    tbe_typed_serialized_free(encoded);
    ProviderObservationV1_clear(&observation);
    return status;
}

static void server_peer(void *context, const ivr_control_ws_route_t *route,
                        const char *identity, int connected) {
    test_iris_control_peer_t *peer = (test_iris_control_peer_t *)context;
    if (!peer || !route || !identity ||
        strcmp(identity, "turbomedia-process") != 0) return;
    if (connected) {
        peer->route = *route;
        atomic_store_explicit(&peer->connected, 1, memory_order_release);
    } else {
        atomic_store_explicit(&peer->connected, 0, memory_order_release);
    }
}

static int server_message(void *context, const ivr_control_ws_route_t *route,
                          const char *peer_identity, const uint8_t *data,
                          size_t size) {
    test_iris_control_peer_t *peer = (test_iris_control_peer_t *)context;
    ProviderMessageKind_t kind = ProviderMessageKind_Command;
    DataBindError error = DATA_BIND_ERROR_INIT;
    int status = SALTS_EPROTO;
    if (!peer || !route || !peer_identity ||
        strcmp(peer_identity, "turbomedia-process") != 0 || !data || !size ||
        iris_provider_peek_kind(data, size, &kind) != SALTS_OK) {
        fprintf(stderr,
                "process Iris H1 WebSocket rejected peer=%s payload=%zu\n",
                peer_identity ? peer_identity : "", size);
        return status;
    }
    if (kind == ProviderMessageKind_Receipt) {
        ProviderReceiptV1_t receipt;
        ProviderReceiptV1_init(&receipt);
        if (ProviderReceiptV1_from_bin(peer->codec, &receipt,
                                       data, size,
                                       &error) == DATA_BIND_OK) {
            salts_mutex_lock(&peer->receipt_mutex);
            memset(&peer->receipt, 0, sizeof(peer->receipt));
            peer->receipt.disposition = (int)receipt.disposition;
            peer->receipt.status_code = receipt.status_code;
            status = copy_text(peer->receipt.command_id,
                               sizeof(peer->receipt.command_id),
                               receipt.command_id) &&
                             copy_text(peer->receipt.worker_id,
                                       sizeof(peer->receipt.worker_id),
                                       receipt.worker_id) &&
                             copy_text(peer->receipt.dispatch_epoch,
                                       sizeof(peer->receipt.dispatch_epoch),
                                       receipt.dispatch_epoch) &&
                             copy_text(peer->receipt.error_code,
                                       sizeof(peer->receipt.error_code),
                                       receipt.error_code) &&
                             copy_text(peer->receipt.error_message,
                                       sizeof(peer->receipt.error_message),
                                       receipt.error_message)
                         ? SALTS_OK
                         : SALTS_ENOSPC;
            salts_mutex_unlock(&peer->receipt_mutex);
            if (status == SALTS_OK) {
                atomic_fetch_add_explicit(&peer->receipt_generation, 1u,
                                          memory_order_release);
            }
        } else {
            fprintf(stderr,
                    "process Iris CHTTP H1 WebSocket failed to decode receipt bytes=%zu\n",
                    size);
        }
        ProviderReceiptV1_clear(&receipt);
    } else if (kind == ProviderMessageKind_Completion) {
        ProviderCompletionV1_t completion;
        ProviderCompletionV1_init(&completion);
        if (ProviderCompletionV1_from_bin(peer->codec, &completion,
                                          data, size, &error) ==
            DATA_BIND_OK) {
            if (peer->config.completion) {
                peer->config.completion(peer->config.context, &completion);
            }
            status = send_completion_ack(peer, route, &completion);
        }
        ProviderCompletionV1_clear(&completion);
    } else if (kind == ProviderMessageKind_Event) {
        ProviderEventV1_t event;
        ProviderEventV1_init(&event);
        if (ProviderEventV1_from_bin(peer->codec, &event, data, size, &error) ==
            DATA_BIND_OK) {
            if (peer->config.event) {
                peer->config.event(peer->config.context, &event);
            }
            status = send_event_ack(peer, route, &event);
        }
        ProviderEventV1_clear(&event);
    } else if (kind == ProviderMessageKind_Query) {
        ProviderQueryV1_t query;
        ProviderQueryV1_init(&query);
        if (ProviderQueryV1_from_bin(peer->codec, &query, data, size, &error) ==
            DATA_BIND_OK) {
            status = send_observation(peer, route, &query);
        }
        ProviderQueryV1_clear(&query);
    }
    return status;
}

int test_iris_control_peer_start(
    const test_iris_control_peer_config_t *config,
    test_iris_control_peer_t **out_peer) {
    test_iris_control_peer_t *peer;
    ivr_control_ws_server_config_t server_config;
    DataBindError error = DATA_BIND_ERROR_INIT;
    if (!config || !out_peer || config->port == 0u || !config->query) {
        return SALTS_EINVAL;
    }
    *out_peer = NULL;
    peer = (test_iris_control_peer_t *)calloc(1, sizeof(*peer));
    if (!peer) return SALTS_ENOMEM;
    peer->config = *config;
    salts_mutex_init(&peer->receipt_mutex);
    atomic_init(&peer->connected, 0);
    atomic_init(&peer->receipt_generation, 0u);
    if (TurboMediaIrisProviderV1_codec_create(&peer->codec, &error) !=
        DATA_BIND_OK) {
        test_iris_control_peer_stop(peer);
        return SALTS_EPROTO;
    }
    ivr_control_ws_server_config_init(&server_config);
    server_config.host = "127.0.0.1";
    server_config.path = "/internal/iris/control";
    server_config.port = config->port;
    server_config.maximum_connections = 1u;
    server_config.maximum_message_bytes = TEST_IRIS_CONTROL_MAX_FRAME_BYTES;
    server_config.on_message = server_message;
    server_config.on_peer = server_peer;
    server_config.callback_context = peer;
    if (ivr_control_ws_server_create(&server_config, &peer->server) != IVR_OK ||
        ivr_control_ws_server_start(peer->server) != IVR_OK) {
        test_iris_control_peer_stop(peer);
        return SALTS_EPROTO;
    }
    *out_peer = peer;
    return SALTS_OK;
}

void test_iris_control_peer_stop(test_iris_control_peer_t *peer) {
    if (!peer) return;
    ivr_control_ws_server_stop(peer->server);
    ivr_control_ws_server_destroy(peer->server);
    data_bind_free(peer->codec);
    salts_mutex_destroy(&peer->receipt_mutex);
    free(peer);
}

int test_iris_control_peer_send_command(
    test_iris_control_peer_t *peer, const char *idempotency_key,
    const char *bridge_json, uint64_t timeout_ms,
    test_iris_control_receipt_t *out_receipt) {
    uint8_t *application = NULL;
    size_t application_size = 0u;
    uint64_t baseline;
    uint64_t waited_ms = 0u;
    int status;
    if (!peer || !idempotency_key || !bridge_json || !out_receipt ||
        timeout_ms == 0u) {
        return SALTS_EINVAL;
    }
    while (waited_ms < timeout_ms &&
           !atomic_load_explicit(&peer->connected, memory_order_acquire)) {
        salts_sleep_ms(TEST_IRIS_CONTROL_WAIT_STEP_MS);
        waited_ms += TEST_IRIS_CONTROL_WAIT_STEP_MS;
    }
    if (!atomic_load_explicit(&peer->connected, memory_order_acquire)) {
        return SALTS_ENOTCONN;
    }
    application = encode_command(peer, idempotency_key, bridge_json,
                                 &application_size);
    if (!application) return SALTS_EPROTO;
    baseline = atomic_load_explicit(&peer->receipt_generation,
                                    memory_order_acquire);
    status = ivr_control_ws_server_send_copy(
        peer->server, &peer->route, application, application_size);
    tbe_typed_serialized_free(application);
    if (status != SALTS_OK) return status;
    while (waited_ms < timeout_ms &&
           atomic_load_explicit(&peer->receipt_generation,
                                memory_order_acquire) == baseline) {
        salts_sleep_ms(TEST_IRIS_CONTROL_WAIT_STEP_MS);
        waited_ms += TEST_IRIS_CONTROL_WAIT_STEP_MS;
    }
    if (atomic_load_explicit(&peer->receipt_generation,
                             memory_order_acquire) == baseline) {
        return SALTS_ETIMEDOUT;
    }
    salts_mutex_lock(&peer->receipt_mutex);
    *out_receipt = peer->receipt;
    salts_mutex_unlock(&peer->receipt_mutex);
    return strcmp(out_receipt->command_id, idempotency_key) == 0
               ? SALTS_OK
               : SALTS_EPROTO;
}
