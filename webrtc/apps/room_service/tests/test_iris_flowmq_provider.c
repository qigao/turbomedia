#include "iris_flowmq_provider.h"

#include <flowmq.h>
#include <tinytest.h>
#include <turbo_error.h>
#include <turbo_parser.h>
#include <turbo_str.h>
#include <turbo_thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define TEST_START_TIMEOUT_NS UINT64_C(5000000000)
#define TEST_WAIT_ATTEMPTS 1000
#define TEST_WAIT_STEP_MS 5u
#define TEST_FINGERPRINT                                                   \
    "sha256:710227af291421e97abf6de1cc8af21e2a50d653bb2855ecab4f549425fdeb4a"

typedef struct provider_harness_s {
    flowmq_router_endpoint_t *router;
    flowmq_router_route_t route;
    atomic_int connected;
    atomic_int receipts;
    atomic_int completions;
    atomic_int events;
    atomic_int queries;
    atomic_int queries_received;
    atomic_int call_offers;
    atomic_int dispatches;
    atomic_int callback_status;
    unsigned query_response_delay_ms;
    int drop_session_bound;
    int reject_session_bound;
    char receipt_command_id[128];
    char receipt_worker_id[128];
    char receipt_epoch[32];
    char bridge_body[2048];
} provider_harness_t;

typedef struct query_send_context_s {
    iris_flowmq_provider_t *provider;
    const iris_flowmq_provider_query_t *query;
    iris_flowmq_provider_observation_t observation;
    ivr_status_t status;
} query_send_context_t;

static int assign_string(tstr *target, const char *value) {
    *target = tstr_dup(value ? value : "");
    return *target != NULL;
}

static unsigned short available_port(void) {
    struct sockaddr_in address;
    unsigned short port = 0u;
#ifdef _WIN32
    SOCKET handle;
    int address_size = (int)sizeof(address);
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0u;
    handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
        WSACleanup();
        return 0u;
    }
#else
    int handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    socklen_t address_size = (socklen_t)sizeof(address);
    if (handle < 0) return 0u;
#endif
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(handle, (struct sockaddr *)&address, sizeof(address)) == 0 &&
        getsockname(handle, (struct sockaddr *)&address, &address_size) == 0) {
        port = ntohs(address.sin_port);
    }
#ifdef _WIN32
    closesocket(handle);
    WSACleanup();
#else
    close(handle);
#endif
    return port;
}

static int copy_text(char *out, size_t capacity, const char *value) {
    int written = snprintf(out, capacity, "%s", value ? value : "");
    return written >= 0 && (size_t)written < capacity;
}

static void router_event(void *context,
                         const flowmq_router_endpoint_event_t *event) {
    provider_harness_t *harness = (provider_harness_t *)context;
    if (!harness || !event ||
        event->kind != FLOWMQ_ROUTER_EVENT_PEER_CONNECTED) {
        return;
    }
    harness->route = event->route;
    atomic_store_explicit(&harness->connected, 1, memory_order_release);
}

static int router_frame(void *context, const flowmq_router_route_t *route,
                        vstr peer_identity, vstr peer_topic,
                        const flowmq_protocol_frame_t *frame) {
    provider_harness_t *harness = (provider_harness_t *)context;
    ProviderMessageKind_t kind = ProviderMessageKind_Command;
    DataBind *codec = NULL;
    DataBindError error = DATA_BIND_ERROR_INIT;
    int status = TURBO_EPROTO;
    (void)peer_topic;
    if (!harness || !frame || frame->kind != FLOWMQ_PROTOCOL_FRAME_DATA ||
        frame->pattern != FLOWMQ_PROTOCOL_DEALER ||
        !vstr_eq(peer_identity, vstr_from_cstr("turbomedia-a")) ||
        flowmq_media_provider_peek_kind(frame->payload.data,
                                        frame->payload.len, &kind) != TURBO_OK) {
        atomic_store_explicit(&harness->callback_status, status,
                              memory_order_release);
        return status;
    }
    if (FlowMqMediaProviderV1_codec_create(&codec, &error) != DATA_BIND_OK) {
        atomic_store_explicit(&harness->callback_status, status,
                              memory_order_release);
        return status;
    }
    if (kind == ProviderMessageKind_Receipt) {
        ProviderReceiptV1_t receipt;
        ProviderReceiptV1_init(&receipt);
        if (ProviderReceiptV1_from_bin(codec, &receipt, frame->payload.data,
                                       frame->payload.len, &error) ==
                DATA_BIND_OK &&
            copy_text(harness->receipt_command_id,
                      sizeof(harness->receipt_command_id),
                      receipt.command_id) &&
            copy_text(harness->receipt_worker_id,
                      sizeof(harness->receipt_worker_id), receipt.worker_id) &&
            copy_text(harness->receipt_epoch, sizeof(harness->receipt_epoch),
                      receipt.dispatch_epoch) &&
            receipt.disposition ==
                ProviderReceiptDisposition_DurableAccepted) {
            status = TURBO_OK;
            atomic_fetch_add_explicit(&harness->receipts, 1,
                                      memory_order_acq_rel);
        }
        ProviderReceiptV1_clear(&receipt);
    } else if (kind == ProviderMessageKind_Completion) {
        ProviderCompletionV1_t completion;
        ProviderCompletionAckV1_t ack;
        uint8_t *application = NULL;
        size_t application_size = 0u;
        tstr encoded = NULL;
        flowmq_protocol_frame_t response;
        ProviderCompletionV1_init(&completion);
        ProviderCompletionAckV1_init(&ack);
        if (ProviderCompletionV1_from_bin(codec, &completion,
                                          frame->payload.data,
                                          frame->payload.len, &error) ==
                DATA_BIND_OK) {
            ack.schema_version = 1u;
            ack.message_kind = ProviderMessageKind_CompletionAck;
            ack.disposition =
                ProviderCompletionAckDisposition_CompletionCommitted;
            if (assign_string(&ack.message_id,
                              "completion-a.completion-ack") &&
                assign_string(&ack.correlation_id,
                              completion.correlation_id) &&
                assign_string(&ack.causation_id, completion.message_id) &&
                assign_string(&ack.tenant_id, completion.tenant_id) &&
                assign_string(&ack.provider_id, completion.provider_id) &&
                assign_string(&ack.session_id, completion.session_id) &&
                assign_string(&ack.partition_key, completion.session_id) &&
                assign_string(&ack.producer_id, "iris-a") &&
                assign_string(&ack.created_at,
                              "2026-08-14T00:00:03Z") &&
                assign_string(&ack.deadline_at, "") &&
                assign_string(&ack.command_id, completion.command_id) &&
                assign_string(&ack.dispatch_epoch,
                              completion.dispatch_epoch) &&
                assign_string(&ack.committed_sequence, "29") &&
                assign_string(&ack.error_code, "") &&
                assign_string(&ack.error_message, "") &&
                ProviderCompletionAckV1_to_bin(
                    &ack, &application, &application_size, &error) ==
                    DATA_BIND_OK) {
                memset(&response, 0, sizeof(response));
                response.kind = FLOWMQ_PROTOCOL_FRAME_DATA;
                response.pattern = FLOWMQ_PROTOCOL_ROUTER;
                response.message_id = frame->message_id;
                response.payload = vstr_from_buf(
                    (const char *)application, application_size);
                status = flowmq_protocol_encode_frame(
                    &response, FLOWMQ_ROUTER_ENDPOINT_DEFAULT_MAX_FRAME_SIZE,
                    &encoded);
                if (status == TURBO_OK) {
                    status = flowmq_router_endpoint_send(
                        harness->router, *route, encoded, tstr_len(encoded));
                }
                if (status == TURBO_OK) {
                    atomic_fetch_add_explicit(&harness->completions, 1,
                                              memory_order_acq_rel);
                }
            }
        }
        tstr_freep(&encoded);
        tbe_typed_serialized_free(application);
        ProviderCompletionAckV1_clear(&ack);
        ProviderCompletionV1_clear(&completion);
    } else if (kind == ProviderMessageKind_Event) {
        ProviderEventV1_t event;
        ProviderEventAckV1_t ack;
        uint8_t *application = NULL;
        size_t application_size = 0u;
        tstr encoded = NULL;
        flowmq_protocol_frame_t response;
        ProviderEventV1_init(&event);
        ProviderEventAckV1_init(&ack);
        if (ProviderEventV1_from_bin(codec, &event, frame->payload.data,
                                     frame->payload.len, &error) ==
                DATA_BIND_OK) {
            ack.schema_version = 1u;
            ack.message_kind = ProviderMessageKind_EventAck;
            ack.disposition = ProviderEventAckDisposition_Committed;
            if (assign_string(&ack.message_id, "event-a.event-ack") &&
                assign_string(&ack.correlation_id, event.correlation_id) &&
                assign_string(&ack.causation_id, event.message_id) &&
                assign_string(&ack.tenant_id, event.tenant_id) &&
                assign_string(&ack.provider_id, event.provider_id) &&
                assign_string(&ack.session_id, event.session_id) &&
                assign_string(&ack.partition_key, event.session_id) &&
                assign_string(&ack.producer_id, "iris-a") &&
                assign_string(&ack.created_at,
                              "2026-08-14T00:00:05Z") &&
                assign_string(&ack.deadline_at, "") &&
                assign_string(&ack.event_id, event.event_id) &&
                assign_string(&ack.committed_sequence, "37") &&
                assign_string(&ack.error_code, "") &&
                assign_string(&ack.error_message, "") &&
                ProviderEventAckV1_to_bin(
                    &ack, &application, &application_size, &error) ==
                    DATA_BIND_OK) {
                memset(&response, 0, sizeof(response));
                response.kind = FLOWMQ_PROTOCOL_FRAME_DATA;
                response.pattern = FLOWMQ_PROTOCOL_ROUTER;
                response.message_id = frame->message_id;
                response.payload = vstr_from_buf(
                    (const char *)application, application_size);
                status = flowmq_protocol_encode_frame(
                    &response, FLOWMQ_ROUTER_ENDPOINT_DEFAULT_MAX_FRAME_SIZE,
                    &encoded);
                if (status == TURBO_OK) {
                    status = flowmq_router_endpoint_send(
                        harness->router, *route, encoded, tstr_len(encoded));
                }
                if (status == TURBO_OK) {
                    atomic_fetch_add_explicit(&harness->events, 1,
                                              memory_order_acq_rel);
                }
            }
        }
        tstr_freep(&encoded);
        tbe_typed_serialized_free(application);
        ProviderEventAckV1_clear(&ack);
        ProviderEventV1_clear(&event);
    } else if (kind == ProviderMessageKind_Query) {
        ProviderQueryV1_t query;
        ProviderObservationV1_t observation;
        uint8_t *application = NULL;
        size_t application_size = 0u;
        tstr encoded = NULL;
        flowmq_protocol_frame_t response;
        ProviderQueryV1_init(&query);
        ProviderObservationV1_init(&observation);
        if (ProviderQueryV1_from_bin(codec, &query, frame->payload.data,
                                     frame->payload.len, &error) ==
                DATA_BIND_OK) {
            atomic_fetch_add_explicit(&harness->queries_received, 1,
                                      memory_order_acq_rel);
            if (harness->query_response_delay_ms > 0u) {
                turbo_sleep_ms(harness->query_response_delay_ms);
            }
            observation.schema_version = 1u;
            observation.message_kind = ProviderMessageKind_Observation;
            observation.status = ProviderQueryStatus_QueryOk;
            observation.has_more = false;
            if (assign_string(&observation.message_id, "query-a.observation") &&
                assign_string(&observation.correlation_id, query.query_id) &&
                assign_string(&observation.causation_id, query.message_id) &&
                assign_string(&observation.tenant_id, query.tenant_id) &&
                assign_string(&observation.provider_id, query.provider_id) &&
                assign_string(&observation.session_id, "") &&
                assign_string(&observation.partition_key, query.tenant_id) &&
                assign_string(&observation.producer_id, "iris-a") &&
                assign_string(&observation.created_at,
                              "2026-08-14T00:00:07Z") &&
                assign_string(&observation.deadline_at, "") &&
                assign_string(&observation.query_id, query.query_id) &&
                assign_string(&observation.observation_type,
                              query.query_type) &&
                assign_string(&observation.revision, "47") &&
                assign_string(&observation.cursor, query.cursor) &&
                assign_string(&observation.next_cursor, "0") &&
                assign_string(&observation.payload_json,
                              "{\"schemaVersion\":1,\"resourceCount\":0,"
                              "\"resources\":[]}") &&
                assign_string(&observation.error_code, "") &&
                assign_string(&observation.error_message, "") &&
                ProviderObservationV1_to_bin(
                    &observation, &application, &application_size, &error) ==
                    DATA_BIND_OK) {
                memset(&response, 0, sizeof(response));
                response.kind = FLOWMQ_PROTOCOL_FRAME_DATA;
                response.pattern = FLOWMQ_PROTOCOL_ROUTER;
                response.message_id = frame->message_id;
                response.payload = vstr_from_buf(
                    (const char *)application, application_size);
                status = flowmq_protocol_encode_frame(
                    &response, FLOWMQ_ROUTER_ENDPOINT_DEFAULT_MAX_FRAME_SIZE,
                    &encoded);
                if (status == TURBO_OK) {
                    status = flowmq_router_endpoint_send(
                        harness->router, *route, encoded, tstr_len(encoded));
                }
                if (status == TURBO_OK) {
                    atomic_fetch_add_explicit(&harness->queries, 1,
                                              memory_order_acq_rel);
                }
            }
        }
        tstr_freep(&encoded);
        tbe_typed_serialized_free(application);
        ProviderObservationV1_clear(&observation);
        ProviderQueryV1_clear(&query);
    } else if (kind == ProviderMessageKind_CallOffer) {
        ProviderCallOfferV1_t offer;
        ProviderSessionBoundV1_t bound;
        uint8_t *application = NULL;
        size_t application_size = 0u;
        tstr encoded = NULL;
        flowmq_protocol_frame_t response;
        ProviderCallOfferV1_init(&offer);
        ProviderSessionBoundV1_init(&bound);
        if (ProviderCallOfferV1_from_bin(codec, &offer, frame->payload.data,
                                         frame->payload.len, &error) ==
            DATA_BIND_OK) {
            atomic_fetch_add_explicit(&harness->call_offers, 1,
                                      memory_order_acq_rel);
            if (harness->drop_session_bound) {
                status = TURBO_OK;
            } else {
                bound.schema_version = 1u;
                bound.message_kind = ProviderMessageKind_SessionBound;
                bound.accepted = harness->reject_session_bound ? false : true;
                if (assign_string(&bound.message_id, "session-bound-a") &&
                    assign_string(&bound.correlation_id, offer.call_id) &&
                    assign_string(&bound.causation_id, offer.message_id) &&
                    assign_string(&bound.tenant_id, offer.tenant_id) &&
                    assign_string(&bound.provider_id, offer.provider_id) &&
                    assign_string(&bound.session_id,
                                  harness->reject_session_bound
                                      ? ""
                                      : "session-call-a") &&
                    assign_string(&bound.partition_key, offer.call_id) &&
                    assign_string(&bound.producer_id, "iris-a") &&
                    assign_string(&bound.created_at,
                                  "2026-08-14T00:00:09Z") &&
                    assign_string(&bound.deadline_at, offer.deadline_at) &&
                    assign_string(&bound.ingress_event_id,
                                  offer.ingress_event_id) &&
                    assign_string(&bound.call_id, offer.call_id) &&
                    assign_string(&bound.call_generation,
                                  offer.call_generation) &&
                    assign_string(&bound.bound_session_id,
                                  harness->reject_session_bound
                                      ? ""
                                      : "session-call-a") &&
                    assign_string(&bound.error_code,
                                  harness->reject_session_bound
                                      ? "capacity_exceeded"
                                      : "") &&
                    assign_string(&bound.error_message,
                                  harness->reject_session_bound
                                      ? "session capacity exhausted"
                                      : "") &&
                    ProviderSessionBoundV1_to_bin(
                        &bound, &application, &application_size, &error) ==
                        DATA_BIND_OK) {
                    memset(&response, 0, sizeof(response));
                    response.kind = FLOWMQ_PROTOCOL_FRAME_DATA;
                    response.pattern = FLOWMQ_PROTOCOL_ROUTER;
                    response.message_id = frame->message_id;
                    response.payload = vstr_from_buf(
                        (const char *)application, application_size);
                    status = flowmq_protocol_encode_frame(
                        &response,
                        FLOWMQ_ROUTER_ENDPOINT_DEFAULT_MAX_FRAME_SIZE,
                        &encoded);
                    if (status == TURBO_OK) {
                        status = flowmq_router_endpoint_send(
                            harness->router, *route, encoded,
                            tstr_len(encoded));
                    }
                }
            }
        }
        tstr_freep(&encoded);
        tbe_typed_serialized_free(application);
        ProviderSessionBoundV1_clear(&bound);
        ProviderCallOfferV1_clear(&offer);
    }
    data_bind_free(codec);
    atomic_store_explicit(&harness->callback_status, status,
                          memory_order_release);
    return status;
}

static iris_media_bridge_result_t dispatch_command(
    void *context, const char *idempotency_key, const char *body,
    size_t body_size) {
    provider_harness_t *harness = (provider_harness_t *)context;
    iris_media_bridge_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = IRIS_MEDIA_BRIDGE_INVALID;
    result.error_code = "TEST_CAPTURE_FAILED";
    result.error_message = "test command capture failed";
    if (!harness || strcmp(idempotency_key, "command-a") != 0 ||
        body_size >= sizeof(harness->bridge_body)) {
        return result;
    }
    memcpy(harness->bridge_body, body, body_size);
    harness->bridge_body[body_size] = '\0';
    atomic_fetch_add_explicit(&harness->dispatches, 1, memory_order_acq_rel);
    result.status = IRIS_MEDIA_BRIDGE_ACCEPTED;
    result.error_code = "";
    result.error_message = "";
    return result;
}

static int fixed_now(void *context, char *out, size_t capacity) {
    (void)context;
    return copy_text(out, capacity, "2026-08-14T00:00:01Z") ? TURBO_OK
                                                              : TURBO_ENOSPC;
}

static int wait_atomic(const atomic_int *value, int expected) {
    int attempt;
    for (attempt = 0; attempt < TEST_WAIT_ATTEMPTS; ++attempt) {
        if (atomic_load_explicit(value, memory_order_acquire) == expected) {
            return TURBO_OK;
        }
        turbo_sleep_ms(TEST_WAIT_STEP_MS);
    }
    return TURBO_ETIMEDOUT;
}

static void send_query_thread(void *context) {
    query_send_context_t *send = (query_send_context_t *)context;
    send->status = iris_flowmq_provider_send_query(
        send->provider, send->query, 2000u, &send->observation);
}

static uint8_t *encode_command(DataBind *codec, size_t *out_size) {
    static const char json[] =
        "{\"schema_version\":1,\"message_kind\":\"Command\"," 
        "\"message_id\":\"command-a\","
        "\"correlation_id\":\"correlation-a\","
        "\"causation_id\":\"causation-a\","
        "\"tenant_id\":\"tenant-a\","
        "\"provider_id\":\"turbomedia\","
        "\"session_id\":\"session-a\","
        "\"partition_key\":\"session-a\","
        "\"producer_id\":\"iris-a\","
        "\"created_at\":\"2026-08-14T00:00:00Z\","
        "\"deadline_at\":\"2099-01-01T00:00:00Z\","
        "\"command_id\":\"command-a\","
        "\"command_type\":\"media.play\","
        "\"worker_id\":\"iris-worker-a\","
        "\"dispatch_epoch\":\"17\","
        "\"semantic_fingerprint\":\"" TEST_FINGERPRINT "\","
        "\"payload_json\":\"{\\\"capability\\\":\\\"ivr\\\"," 
        "\\\"dialogId\\\":\\\"dialog-a\\\","
        "\\\"roomId\\\":\\\"room-a\\\","
        "\\\"callId\\\":\\\"call-a\\\","
        "\\\"callGeneration\\\":7,"
        "\\\"operationGeneration\\\":9,"
        "\\\"text\\\":\\\"Welcome\\\"}\"}";
    ProviderCommandV1_t command;
    DataBindError error = DATA_BIND_ERROR_INIT;
    uint8_t *encoded = NULL;
    ProviderCommandV1_init(&command);
    if (ProviderCommandV1_from_json(codec, &command, json, sizeof(json) - 1u,
                                    &error) != DATA_BIND_OK ||
        ProviderCommandV1_to_bin(&command, &encoded, out_size, &error) !=
            DATA_BIND_OK) {
        tbe_typed_serialized_free(encoded);
        encoded = NULL;
        *out_size = 0u;
    }
    ProviderCommandV1_clear(&command);
    return encoded;
}

static int send_command(provider_harness_t *harness, DataBind *codec) {
    flowmq_protocol_frame_t frame;
    uint8_t *application = NULL;
    size_t application_size = 0u;
    tstr encoded = NULL;
    int status;
    application = encode_command(codec, &application_size);
    if (!application) return TURBO_EPROTO;
    memset(&frame, 0, sizeof(frame));
    frame.kind = FLOWMQ_PROTOCOL_FRAME_DATA;
    frame.pattern = FLOWMQ_PROTOCOL_ROUTER;
    frame.message_id = 41u;
    frame.payload =
        vstr_from_buf((const char *)application, application_size);
    status = flowmq_protocol_encode_frame(
        &frame, FLOWMQ_ROUTER_ENDPOINT_DEFAULT_MAX_FRAME_SIZE, &encoded);
    if (status == TURBO_OK) {
        status = flowmq_router_endpoint_send_copy(
            harness->router, harness->route, 1u, encoded, tstr_len(encoded));
    }
    tstr_freep(&encoded);
    tbe_typed_serialized_free(application);
    return status;
}

spec("RoomService Iris FlowMQ provider DEALER") {
    it("allows plaintext only for an explicitly enabled loopback") {
        iris_flowmq_provider_config_t config;
        iris_flowmq_provider_config_init(&config);
        config.transport = FLOWMQ_TRANSPORT_TCP;
        config.port = 22000u;
        config.provider_instance_id = "turbomedia-a";
        config.iris_identity = "iris-a";
        config.dispatch = dispatch_command;
        config.allow_insecure_development_loopback = 1;
        check_equal(iris_flowmq_provider_config_validate(&config), TURBO_OK);
        config.allow_insecure_development_loopback = 0;
        check_equal(iris_flowmq_provider_config_validate(&config),
                     TURBO_EINVAL);
        config.allow_insecure_development_loopback = 1;
        config.host = "0.0.0.0";
        check_equal(iris_flowmq_provider_config_validate(&config),
                     TURBO_EINVAL);
    }

    it("dispatches one canonical command and returns its exact durable receipt") {
        provider_harness_t harness;
        flowmq_router_endpoint_config_t router_config;
        iris_flowmq_provider_config_t provider_config;
        iris_flowmq_provider_t *provider = NULL;
        iris_media_completion_t completion;
        ivr_media_command_result_t media_result;
        iris_flowmq_completion_ack_t completion_ack;
        ivr_media_event_t event;
        iris_flowmq_event_ack_t event_ack;
        iris_flowmq_provider_query_t query;
        iris_flowmq_provider_observation_t observation;
        iris_flowmq_call_offer_t call_offer;
        iris_flowmq_session_bound_t session_bound;
        query_send_context_t concurrent_query;
        iris_flowmq_completion_ack_t concurrent_completion_ack;
        turbo_thread_t query_thread;
        DataBind *codec = NULL;
        DataBindError error = DATA_BIND_ERROR_INIT;
        unsigned short port = available_port();
        memset(&harness, 0, sizeof(harness));
        atomic_init(&harness.callback_status, TURBO_EALREADY);
        memset(&completion, 0, sizeof(completion));
        memset(&media_result, 0, sizeof(media_result));
        memset(&event, 0, sizeof(event));
        memset(&query, 0, sizeof(query));
        memset(&call_offer, 0, sizeof(call_offer));
        check_not_equal(port, 0u);

        flowmq_router_endpoint_config_init(&router_config);
        router_config.transport = FLOWMQ_TRANSPORT_TCP;
        router_config.host = "127.0.0.1";
        router_config.path = "";
        router_config.topic = "media-provider-v1";
        router_config.identity = "iris-a";
        router_config.port = (int)port;
        router_config.max_connections = 1u;
        router_config.context = NULL;
        router_config.drive_context = 1;
        router_config.own_context = 1;
        router_config.on_frame = router_frame;
        router_config.on_event = router_event;
        router_config.callback_ctx = &harness;
        check_equal(flowmq_router_endpoint_create(&router_config,
                                                   &harness.router),
                     TURBO_OK);
        check_equal(flowmq_router_endpoint_start(harness.router,
                                                  TEST_START_TIMEOUT_NS),
                     TURBO_OK);

        iris_flowmq_provider_config_init(&provider_config);
        provider_config.transport = FLOWMQ_TRANSPORT_TCP;
        provider_config.host = "127.0.0.1";
        provider_config.port = port;
        provider_config.provider_instance_id = "turbomedia-a";
        provider_config.iris_identity = "iris-a";
        provider_config.allow_insecure_development_loopback = 1;
        provider_config.maximum_ingress_messages = 8u;
        provider_config.maximum_ingress_bytes = 64u * 1024u;
        provider_config.send_queue_capacity = 8u;
        provider_config.send_queue_bytes = 64u * 1024u;
        provider_config.reconnect_initial_ms = 0u;
        provider_config.reconnect_max_ms = 0u;
        provider_config.dispatch = dispatch_command;
        provider_config.dispatch_context = &harness;
        provider_config.now = fixed_now;
        provider = iris_flowmq_provider_create(&provider_config);
        check_not_null(provider);
        if (provider) {
            check_equal(iris_flowmq_provider_start(provider), TURBO_OK);
            check_equal(wait_atomic(&harness.connected, 1), TURBO_OK);
            check_equal(FlowMqMediaProviderV1_codec_create(&codec, &error),
                         DATA_BIND_OK);
            if (codec) check_equal(send_command(&harness, codec), TURBO_OK);
            check_equal(wait_atomic(&harness.receipts, 1), TURBO_OK);
            check_equal(atomic_load_explicit(&harness.callback_status,
                                              memory_order_acquire),
                         TURBO_OK);
            check_equal(atomic_load_explicit(&harness.dispatches,
                                              memory_order_acquire),
                         1);
            check_equal(harness.receipt_command_id, "command-a");
            check_equal(harness.receipt_worker_id, "iris-worker-a");
            check_equal(harness.receipt_epoch, "17");
            check_contains(harness.bridge_body,
                               "\"dialogId\":\"dialog-a\"");
            check_contains(harness.bridge_body,
                               "\"dispatchEpoch\":17");
            copy_text(completion.command_id, sizeof(completion.command_id),
                      "command-a");
            copy_text(completion.tenant_id, sizeof(completion.tenant_id),
                      "tenant-a");
            copy_text(completion.provider_session_id,
                      sizeof(completion.provider_session_id), "session-a");
            copy_text(completion.iris_worker_id,
                      sizeof(completion.iris_worker_id), "iris-worker-a");
            copy_text(completion.correlation_id,
                      sizeof(completion.correlation_id), "correlation-a");
            completion.dispatch_epoch = 17u;
            copy_text(media_result.worker_id,
                      sizeof(media_result.worker_id), "media-worker-a");
            copy_text(media_result.dialog_id,
                      sizeof(media_result.dialog_id), "dialog-a");
            copy_text(media_result.room_id, sizeof(media_result.room_id),
                      "room-a");
            copy_text(media_result.call_id, sizeof(media_result.call_id),
                      "call-a");
            media_result.call_generation = 7u;
            media_result.operation_generation = 9u;
            media_result.status_code = IVR_OK;
            check_equal(iris_flowmq_provider_send_completion(
                             provider, &completion, &media_result,
                             "completion-a", "2026-08-14T00:00:02Z", 42u,
                             1000u, &completion_ack),
                         IVR_OK);
            check_equal(completion_ack.disposition,
                         ProviderCompletionAckDisposition_CompletionCommitted);
            check_equal(completion_ack.committed_sequence, 29u);
            check_equal(wait_atomic(&harness.completions, 1), TURBO_OK);
            copy_text(event.event_id, sizeof(event.event_id), "media-event-a");
            copy_text(event.tenant_id, sizeof(event.tenant_id), "tenant-a");
            copy_text(event.provider_session_id,
                      sizeof(event.provider_session_id), "session-a");
            copy_text(event.dialog_id, sizeof(event.dialog_id), "dialog-a");
            copy_text(event.event_type, sizeof(event.event_type), "dtmf.final");
            copy_text(event.payload_json, sizeof(event.payload_json),
                      "{\"digit\":\"5\"}");
            event.sequence = 31u;
            event.occurred_at_ms = 42u;
            check_equal(iris_flowmq_provider_send_event(
                             provider, &event, "event-a",
                             "2026-08-14T00:00:04Z", 1000u, &event_ack),
                         IVR_OK);
            check_equal(event_ack.disposition,
                         ProviderEventAckDisposition_Committed);
            check_equal(event_ack.committed_sequence, 37u);
            check_equal(wait_atomic(&harness.events, 1), TURBO_OK);
            query.tenant_id = "tenant-a";
            query.query_id = "query-a";
            query.query_type = "expected_media_resources";
            query.created_at = "2026-08-14T00:00:06Z";
            query.deadline_at = "2026-08-14T00:00:10Z";
            query.expected_revision = 0u;
            query.cursor = 0u;
            query.limit = 16u;
            query.payload_json = "{\"schemaVersion\":1}";
            check_equal(iris_flowmq_provider_send_query(
                             provider, &query, 1000u, &observation),
                         IVR_OK);
            check_equal(observation.wire.status,
                         ProviderQueryStatus_QueryOk);
            check_equal(observation.revision, 47u);
            check_equal(observation.cursor, 0u);
            check_contains(observation.wire.payload_json,
                               "\"resourceCount\":0");
            iris_flowmq_provider_observation_clear(&observation);
            check_equal(wait_atomic(&harness.queries, 1), TURBO_OK);

            memset(&concurrent_query, 0, sizeof(concurrent_query));
            harness.query_response_delay_ms = 250u;
            query.query_id = "query-concurrent";
            concurrent_query.provider = provider;
            concurrent_query.query = &query;
            check_equal(turbo_thread_create(&query_thread, send_query_thread,
                                             &concurrent_query),
                         0);
            check_equal(wait_atomic(&harness.queries_received, 2), TURBO_OK);
            check_equal(iris_flowmq_provider_send_completion(
                             provider, &completion, &media_result,
                             "completion-concurrent",
                             "2026-08-14T00:00:08Z", 43u, 2000u,
                             &concurrent_completion_ack),
                         IVR_OK);
            turbo_thread_join(&query_thread);
            turbo_thread_destroy(&query_thread);
            check_equal(concurrent_query.status, IVR_OK);
            check_equal(concurrent_query.observation.revision, 47u);
            check_equal(concurrent_completion_ack.disposition,
                         ProviderCompletionAckDisposition_CompletionCommitted);
            iris_flowmq_provider_observation_clear(
                &concurrent_query.observation);

            copy_text(call_offer.ingress_event_id,
                      sizeof(call_offer.ingress_event_id), "ingress-call-a");
            copy_text(call_offer.tenant_id, sizeof(call_offer.tenant_id),
                      "tenant-a");
            copy_text(call_offer.call_id, sizeof(call_offer.call_id),
                      "call-a");
            copy_text(call_offer.created_at, sizeof(call_offer.created_at),
                      "2026-08-14T00:00:08Z");
            copy_text(call_offer.deadline_at, sizeof(call_offer.deadline_at),
                      "2026-08-14T00:00:13Z");
            copy_text(call_offer.transport, sizeof(call_offer.transport),
                      "sip");
            copy_text(call_offer.source, sizeof(call_offer.source),
                      "sip:alice@example.test");
            copy_text(call_offer.destination,
                      sizeof(call_offer.destination),
                      "sip:sales@example.test");
            copy_text(call_offer.payload_json,
                      sizeof(call_offer.payload_json),
                      "{\"schemaVersion\":1,\"routeKey\":\"sales-main\"}");
            call_offer.call_generation = 7u;
            check_equal(iris_flowmq_provider_send_call_offer(
                             provider, &call_offer, 1000u, &session_bound),
                         IVR_OK);
            check_true(session_bound.accepted);
            check_equal(session_bound.bound_session_id, "session-call-a");

            copy_text(call_offer.ingress_event_id,
                      sizeof(call_offer.ingress_event_id), "ingress-call-b");
            copy_text(call_offer.call_id, sizeof(call_offer.call_id),
                      "call-b");
            harness.drop_session_bound = 1;
            check_equal(iris_flowmq_provider_send_call_offer(
                             provider, &call_offer, 25u, &session_bound),
                         IVR_EBUSY);
            harness.drop_session_bound = 0;
            check_equal(iris_flowmq_provider_send_call_offer(
                             provider, &call_offer, 1000u, &session_bound),
                         IVR_OK);
            check_true(session_bound.accepted);
            check_equal(session_bound.bound_session_id, "session-call-a");
            check_equal(wait_atomic(&harness.call_offers, 3), TURBO_OK);

            copy_text(call_offer.ingress_event_id,
                      sizeof(call_offer.ingress_event_id), "ingress-call-c");
            copy_text(call_offer.call_id, sizeof(call_offer.call_id),
                      "call-c");
            harness.reject_session_bound = 1;
            check_equal(iris_flowmq_provider_send_call_offer(
                             provider, &call_offer, 1000u, &session_bound),
                         IVR_OK);
            check_false(session_bound.accepted);
            check_equal(session_bound.bound_session_id, "");
            check_equal(session_bound.error_code, "capacity_exceeded");
            harness.reject_session_bound = 0;

            iris_flowmq_provider_stop(provider);
            check_equal(iris_flowmq_provider_send_call_offer(
                             provider, &call_offer, 100u, &session_bound),
                         IVR_ECLOSED);
        }
        data_bind_free(codec);
        iris_flowmq_provider_destroy(provider);
        flowmq_router_endpoint_stop(harness.router);
        flowmq_router_endpoint_destroy(harness.router);
    }
}
