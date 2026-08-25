#include "iris_flowmq_process_peer.h"

#include <turbo_crypto.h>
#include <turbo_parser.h>
#include <turbo_str.h>
#include <turbo_thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TEST_IRIS_FLOWMQ_MAX_FRAME_BYTES = 256 * 1024,
    TEST_IRIS_FLOWMQ_WAIT_STEP_MS = 5
};
#define TEST_IRIS_FLOWMQ_START_TIMEOUT_NS UINT64_C(5000000000)

struct test_iris_flowmq_peer_s {
    flowmq_router_endpoint_t *router;
    flowmq_router_route_t route;
    DataBind *codec;
    atomic_int connected;
    atomic_uint_fast64_t receipt_generation;
    atomic_uint_fast64_t next_message_id;
    test_iris_flowmq_peer_config_t config;
    turbo_mutex_t receipt_mutex;
    test_iris_flowmq_receipt_t receipt;
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

static const char *json_string(const json_value_t *object, const char *name) {
    json_value_t *value = object ? turbo_json_object_get(object, name) : NULL;
    return value && turbo_json_type(value) == TURBO_JSON_STRING
               ? turbo_json_string(value)
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

static uint8_t *encode_command(test_iris_flowmq_peer_t *peer,
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
        turbo_parse_json((const uint8_t *)bridge_json, strlen(bridge_json),
                         &root) != 0 ||
        !root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
        turbo_free_json(&root);
        return NULL;
    }
    command_id = json_string(root, "commandId");
    tenant_id = json_string(root, "tenantId");
    session_id = json_string(root, "sessionId");
    command_type = json_string(root, "type");
    provider_id = json_string(root, "provider");
    correlation_id = json_string(root, "correlationId");
    causation_id = json_string(root, "causationId");
    deadline_at = json_string(root, "deadline");
    worker_id = json_string(root, "workerId");
    epoch_value = turbo_json_object_get(root, "dispatchEpoch");
    data = turbo_json_object_get(root, "data");
    dispatch_epoch = epoch_value && turbo_json_type(epoch_value) == TURBO_JSON_NUMBER
                         ? (uint64_t)turbo_json_number(epoch_value)
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
        !data || turbo_json_type(data) != TURBO_JSON_OBJECT || written <= 0 ||
        (size_t)written >= sizeof(epoch)) {
        turbo_free_json(&root);
        return NULL;
    }
    payload_json = turbo_json_serialize(data, &payload_size);
    if (!payload_json || payload_size == 0u) {
        turbo_json_serialize_free(payload_json);
        turbo_free_json(&root);
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
    turbo_json_serialize_free(payload_json);
    turbo_free_json(&root);
    return encoded;
}

static int send_application(test_iris_flowmq_peer_t *peer,
                            const flowmq_router_route_t *route,
                            uint64_t message_id, const uint8_t *payload,
                            size_t payload_size) {
    flowmq_protocol_frame_t frame;
    tstr encoded = NULL;
    int status;
    memset(&frame, 0, sizeof(frame));
    frame.kind = FLOWMQ_PROTOCOL_FRAME_DATA;
    frame.pattern = FLOWMQ_PROTOCOL_ROUTER;
    frame.message_id = message_id;
    frame.payload = vstr_from_buf((const char *)payload, payload_size);
    status = flowmq_protocol_encode_frame(
        &frame, TEST_IRIS_FLOWMQ_MAX_FRAME_BYTES, &encoded);
    if (status == TURBO_OK) {
        status = flowmq_router_endpoint_send_copy(
            peer->router, *route, message_id, encoded, tstr_len(encoded));
    }
    tstr_freep(&encoded);
    return status;
}

static int send_completion_ack(test_iris_flowmq_peer_t *peer,
                               const flowmq_router_route_t *route,
                               const flowmq_protocol_frame_t *frame,
                               const ProviderCompletionV1_t *completion) {
    ProviderCompletionAckV1_t ack;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0u;
    int status = TURBO_ENOMEM;
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
        status = send_application(peer, route, frame->message_id, encoded,
                                  encoded_size);
    }
    tbe_typed_serialized_free(encoded);
    ProviderCompletionAckV1_clear(&ack);
    return status;
}

static int send_event_ack(test_iris_flowmq_peer_t *peer,
                          const flowmq_router_route_t *route,
                          const flowmq_protocol_frame_t *frame,
                          const ProviderEventV1_t *event) {
    ProviderEventAckV1_t ack;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0u;
    int status = TURBO_ENOMEM;
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
        status = send_application(peer, route, frame->message_id, encoded,
                                  encoded_size);
    }
    tbe_typed_serialized_free(encoded);
    ProviderEventAckV1_clear(&ack);
    return status;
}

static int send_observation(test_iris_flowmq_peer_t *peer,
                            const flowmq_router_route_t *route,
                            const flowmq_protocol_frame_t *frame,
                            const ProviderQueryV1_t *query) {
    ProviderObservationV1_t observation;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint8_t *encoded = NULL;
    size_t encoded_size = 0u;
    char payload[4096];
    int available = 0;
    int status = TURBO_EPROTO;
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
        status = send_application(peer, route, frame->message_id, encoded,
                                  encoded_size);
    }
    tbe_typed_serialized_free(encoded);
    ProviderObservationV1_clear(&observation);
    return status;
}

static void router_event(void *context,
                         const flowmq_router_endpoint_event_t *event) {
    test_iris_flowmq_peer_t *peer = (test_iris_flowmq_peer_t *)context;
    if (!peer || !event) return;
    if (event->kind == FLOWMQ_ROUTER_EVENT_PEER_CONNECTED) {
        peer->route = event->route;
        atomic_store_explicit(&peer->connected, 1, memory_order_release);
    } else if (event->kind == FLOWMQ_ROUTER_EVENT_PEER_DISCONNECTED) {
        atomic_store_explicit(&peer->connected, 0, memory_order_release);
    }
}

static int router_frame(void *context, const flowmq_router_route_t *route,
                        vstr peer_identity, vstr peer_topic,
                        const flowmq_protocol_frame_t *frame) {
    test_iris_flowmq_peer_t *peer = (test_iris_flowmq_peer_t *)context;
    ProviderMessageKind_t kind = ProviderMessageKind_Command;
    DataBindError error = DATA_BIND_ERROR_INIT;
    int status = TURBO_EPROTO;
    (void)peer_topic;
    if (!peer || !route || !frame ||
        frame->kind != FLOWMQ_PROTOCOL_FRAME_DATA ||
        frame->pattern != FLOWMQ_PROTOCOL_DEALER ||
        !vstr_eq(peer_identity, vstr_from_cstr("turbomedia-process")) ||
        flowmq_media_provider_peek_kind(frame->payload.data,
                                        frame->payload.len, &kind) != TURBO_OK) {
        fprintf(stderr,
                "process Iris FlowMQ rejected frame kind=%d pattern=%d "
                "peer=%.*s payload=%zu\n",
                frame ? (int)frame->kind : -1,
                frame ? (int)frame->pattern : -1,
                (int)peer_identity.len,
                peer_identity.data ? peer_identity.data : "",
                frame ? frame->payload.len : 0u);
        return status;
    }
    if (kind == ProviderMessageKind_Receipt) {
        ProviderReceiptV1_t receipt;
        ProviderReceiptV1_init(&receipt);
        if (ProviderReceiptV1_from_bin(peer->codec, &receipt,
                                       frame->payload.data, frame->payload.len,
                                       &error) == DATA_BIND_OK) {
            turbo_mutex_lock(&peer->receipt_mutex);
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
                         ? TURBO_OK
                         : TURBO_ENOSPC;
            turbo_mutex_unlock(&peer->receipt_mutex);
            if (status == TURBO_OK) {
                atomic_fetch_add_explicit(&peer->receipt_generation, 1u,
                                          memory_order_release);
            }
        } else {
            fprintf(stderr,
                    "process Iris FlowMQ failed to decode receipt bytes=%zu\n",
                    frame->payload.len);
        }
        ProviderReceiptV1_clear(&receipt);
    } else if (kind == ProviderMessageKind_Completion) {
        ProviderCompletionV1_t completion;
        ProviderCompletionV1_init(&completion);
        if (ProviderCompletionV1_from_bin(peer->codec, &completion,
                                          frame->payload.data,
                                          frame->payload.len, &error) ==
            DATA_BIND_OK) {
            if (peer->config.completion) {
                peer->config.completion(peer->config.context, &completion);
            }
            status = send_completion_ack(peer, route, frame, &completion);
        }
        ProviderCompletionV1_clear(&completion);
    } else if (kind == ProviderMessageKind_Event) {
        ProviderEventV1_t event;
        ProviderEventV1_init(&event);
        if (ProviderEventV1_from_bin(peer->codec, &event, frame->payload.data,
                                     frame->payload.len, &error) ==
            DATA_BIND_OK) {
            if (peer->config.event) {
                peer->config.event(peer->config.context, &event);
            }
            status = send_event_ack(peer, route, frame, &event);
        }
        ProviderEventV1_clear(&event);
    } else if (kind == ProviderMessageKind_Query) {
        ProviderQueryV1_t query;
        ProviderQueryV1_init(&query);
        if (ProviderQueryV1_from_bin(peer->codec, &query, frame->payload.data,
                                     frame->payload.len, &error) ==
            DATA_BIND_OK) {
            status = send_observation(peer, route, frame, &query);
        }
        ProviderQueryV1_clear(&query);
    }
    return status;
}

int test_iris_flowmq_peer_start(
    const test_iris_flowmq_peer_config_t *config,
    test_iris_flowmq_peer_t **out_peer) {
    test_iris_flowmq_peer_t *peer;
    flowmq_router_endpoint_config_t router_config;
    DataBindError error = DATA_BIND_ERROR_INIT;
    if (!config || !out_peer || config->port == 0u || !config->query) {
        return TURBO_EINVAL;
    }
    *out_peer = NULL;
    peer = (test_iris_flowmq_peer_t *)calloc(1, sizeof(*peer));
    if (!peer) return TURBO_ENOMEM;
    peer->config = *config;
    turbo_mutex_init(&peer->receipt_mutex);
    atomic_init(&peer->connected, 0);
    atomic_init(&peer->receipt_generation, 0u);
    atomic_init(&peer->next_message_id, 1u);
    if (FlowMqMediaProviderV1_codec_create(&peer->codec, &error) !=
        DATA_BIND_OK) {
        test_iris_flowmq_peer_stop(peer);
        return TURBO_EPROTO;
    }
    flowmq_router_endpoint_config_init(&router_config);
    router_config.transport = FLOWMQ_TRANSPORT_TCP;
    router_config.host = "127.0.0.1";
    router_config.path = "";
    router_config.topic = "media-provider-v1";
    router_config.identity = "iris-process";
    router_config.port = (int)config->port;
    router_config.max_connections = 1u;
    router_config.max_frame_size = TEST_IRIS_FLOWMQ_MAX_FRAME_BYTES;
    router_config.context = NULL;
    router_config.drive_context = 1;
    router_config.own_context = 1;
    router_config.on_frame = router_frame;
    router_config.on_event = router_event;
    router_config.callback_ctx = peer;
    if (flowmq_router_endpoint_create(&router_config, &peer->router) !=
            TURBO_OK ||
        flowmq_router_endpoint_start(peer->router,
                                     TEST_IRIS_FLOWMQ_START_TIMEOUT_NS) !=
            TURBO_OK) {
        test_iris_flowmq_peer_stop(peer);
        return TURBO_EPROTO;
    }
    *out_peer = peer;
    return TURBO_OK;
}

void test_iris_flowmq_peer_stop(test_iris_flowmq_peer_t *peer) {
    if (!peer) return;
    flowmq_router_endpoint_stop(peer->router);
    flowmq_router_endpoint_destroy(peer->router);
    data_bind_free(peer->codec);
    turbo_mutex_destroy(&peer->receipt_mutex);
    free(peer);
}

int test_iris_flowmq_peer_send_command(
    test_iris_flowmq_peer_t *peer, const char *idempotency_key,
    const char *bridge_json, uint64_t timeout_ms,
    test_iris_flowmq_receipt_t *out_receipt) {
    flowmq_protocol_frame_t frame;
    uint8_t *application = NULL;
    size_t application_size = 0u;
    tstr encoded = NULL;
    uint64_t baseline;
    uint64_t message_id;
    uint64_t waited_ms = 0u;
    int status;
    if (!peer || !idempotency_key || !bridge_json || !out_receipt ||
        timeout_ms == 0u) {
        return TURBO_EINVAL;
    }
    while (waited_ms < timeout_ms &&
           !atomic_load_explicit(&peer->connected, memory_order_acquire)) {
        turbo_sleep_ms(TEST_IRIS_FLOWMQ_WAIT_STEP_MS);
        waited_ms += TEST_IRIS_FLOWMQ_WAIT_STEP_MS;
    }
    if (!atomic_load_explicit(&peer->connected, memory_order_acquire)) {
        return TURBO_ENOTCONN;
    }
    application = encode_command(peer, idempotency_key, bridge_json,
                                 &application_size);
    if (!application) return TURBO_EPROTO;
    baseline = atomic_load_explicit(&peer->receipt_generation,
                                    memory_order_acquire);
    message_id = atomic_fetch_add_explicit(&peer->next_message_id, 1u,
                                           memory_order_relaxed);
    memset(&frame, 0, sizeof(frame));
    frame.kind = FLOWMQ_PROTOCOL_FRAME_DATA;
    frame.pattern = FLOWMQ_PROTOCOL_ROUTER;
    frame.message_id = message_id;
    frame.payload =
        vstr_from_buf((const char *)application, application_size);
    status = flowmq_protocol_encode_frame(
        &frame, TEST_IRIS_FLOWMQ_MAX_FRAME_BYTES, &encoded);
    if (status == TURBO_OK) {
        status = flowmq_router_endpoint_send_copy(
            peer->router, peer->route, message_id, encoded, tstr_len(encoded));
    }
    tstr_freep(&encoded);
    tbe_typed_serialized_free(application);
    if (status != TURBO_OK) return status;
    while (waited_ms < timeout_ms &&
           atomic_load_explicit(&peer->receipt_generation,
                                memory_order_acquire) == baseline) {
        turbo_sleep_ms(TEST_IRIS_FLOWMQ_WAIT_STEP_MS);
        waited_ms += TEST_IRIS_FLOWMQ_WAIT_STEP_MS;
    }
    if (atomic_load_explicit(&peer->receipt_generation,
                             memory_order_acquire) == baseline) {
        return TURBO_ETIMEDOUT;
    }
    turbo_mutex_lock(&peer->receipt_mutex);
    *out_receipt = peer->receipt;
    turbo_mutex_unlock(&peer->receipt_mutex);
    return strcmp(out_receipt->command_id, idempotency_key) == 0
               ? TURBO_OK
               : TURBO_EPROTO;
}
