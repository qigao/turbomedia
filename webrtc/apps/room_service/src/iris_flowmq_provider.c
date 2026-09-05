#include "iris_flowmq_provider.h"

#include "iris_flowmq_provider_codec.h"

#include <flowmq.h>
#include <turbo_crypto.h>
#include <salts_error.h>
#include <salts_str.h>
#include <salts_thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IRIS_FLOWMQ_MAX_QUEUE_CAPACITY (1024u * 1024u)
#define IRIS_FLOWMQ_MAX_BUFFER_BYTES (1024u * 1024u * 1024u)
#define IRIS_FLOWMQ_DEFAULT_FRAME_BYTES (1024u * 1024u)
#define IRIS_FLOWMQ_DEFAULT_INGRESS_MESSAGES 1024u
#define IRIS_FLOWMQ_DEFAULT_INGRESS_BYTES (8u * 1024u * 1024u)
#define IRIS_FLOWMQ_DEFAULT_START_TIMEOUT_NS UINT64_C(5000000000)
#define IRIS_FLOWMQ_CALLBACK_QUIESCE_ATTEMPTS 5000u
#define IRIS_FLOWMQ_CALLBACK_QUIESCE_STEP_MS 1u
#define IRIS_FLOWMQ_MAX_ACK_TIMEOUT_MS (5u * 60u * 1000u)

typedef struct iris_flowmq_work_item_s {
    struct iris_flowmq_provider_s *owner;
    uint64_t frame_message_id;
    size_t payload_size;
    unsigned char payload[];
} iris_flowmq_work_item_t;

struct iris_flowmq_provider_s {
    flowmq_connect_endpoint_t *endpoint;
    salts_threadpool_t *worker;
    DataBind *codec;
    tstr provider_id;
    tstr provider_instance_id;
    tstr iris_identity;
    tstr iris_certificate_sha256;
    size_t maximum_frame_bytes;
    size_t maximum_ingress_bytes;
    uint64_t start_timeout_ns;
    iris_flowmq_provider_dispatch_fn dispatch;
    void *dispatch_context;
    iris_flowmq_provider_now_fn now;
    void *now_context;
    salts_mutex_t delivery_mutex;
    salts_mutex_t query_mutex;
    salts_mutex_t offer_mutex;
    salts_mutex_t ack_mutex;
    salts_cond_t ack_changed;
    iris_media_completion_t pending_completion;
    iris_flowmq_completion_ack_t pending_completion_ack;
    char pending_completion_message_id[256];
    ivr_media_event_t pending_event;
    iris_flowmq_event_ack_t pending_event_ack;
    char pending_event_message_id[256];
    char pending_query_tenant_id[128];
    char pending_query_id[256];
    char pending_query_type[128];
    uint64_t pending_query_cursor;
    iris_flowmq_provider_observation_t pending_observation;
    iris_flowmq_call_offer_t pending_offer;
    iris_flowmq_session_bound_t pending_session_bound;
    uint64_t next_pending_generation;
    uint64_t pending_delivery_generation;
    uint64_t pending_query_generation;
    uint64_t pending_offer_generation;
    int pending_completion_active;
    int pending_completion_done;
    int pending_event_active;
    int pending_event_done;
    int pending_query_active;
    int pending_query_done;
    int pending_offer_active;
    int pending_offer_done;
    atomic_size_t pending_ingress_bytes;
    atomic_uint_fast64_t next_send_completion_id;
    atomic_uint callback_count;
    atomic_int accepting;
    atomic_int running;
    atomic_int stopped;
};

static int nonempty(const char *value) { return value && value[0]; }

static int loopback(const char *host) {
    return host && (strcmp(host, "127.0.0.1") == 0 ||
                    strcmp(host, "::1") == 0 ||
                    strcmp(host, "localhost") == 0);
}

static int secure_transport(flowmq_coronet_transport_t transport) {
    return transport == FLOWMQ_TRANSPORT_TLS ||
           transport == FLOWMQ_TRANSPORT_WSS;
}

static int development_transport(flowmq_coronet_transport_t transport) {
    return transport == FLOWMQ_TRANSPORT_TCP ||
           transport == FLOWMQ_TRANSPORT_WS;
}

void iris_flowmq_provider_config_init(iris_flowmq_provider_config_t *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->transport = FLOWMQ_TRANSPORT_TLS;
    config->host = "127.0.0.1";
    config->path = "";
    config->topic = "media-provider-v1";
    config->provider_id = "turbomedia";
    config->maximum_frame_bytes = IRIS_FLOWMQ_DEFAULT_FRAME_BYTES;
    config->maximum_ingress_messages = IRIS_FLOWMQ_DEFAULT_INGRESS_MESSAGES;
    config->maximum_ingress_bytes = IRIS_FLOWMQ_DEFAULT_INGRESS_BYTES;
    config->send_queue_capacity = IRIS_FLOWMQ_DEFAULT_INGRESS_MESSAGES;
    config->send_queue_bytes = IRIS_FLOWMQ_DEFAULT_INGRESS_BYTES;
    config->start_timeout_ns = IRIS_FLOWMQ_DEFAULT_START_TIMEOUT_NS;
    config->reconnect_initial_ms =
        FLOWMQ_CONNECT_ENDPOINT_DEFAULT_RECONNECT_INITIAL_MS;
    config->reconnect_max_ms = FLOWMQ_CONNECT_ENDPOINT_DEFAULT_RECONNECT_MAX_MS;
}

int iris_flowmq_provider_config_validate(
    const iris_flowmq_provider_config_t *config) {
    int secure;
    if (!config || !nonempty(config->host) || config->port == 0u ||
        !nonempty(config->topic) || !nonempty(config->provider_id) ||
        !nonempty(config->provider_instance_id) ||
        !nonempty(config->iris_identity) || !config->dispatch ||
        config->maximum_frame_bytes == 0u ||
        config->maximum_frame_bytes > IRIS_FLOWMQ_MAX_BUFFER_BYTES ||
        config->maximum_ingress_messages == 0u ||
        config->maximum_ingress_messages > IRIS_FLOWMQ_MAX_QUEUE_CAPACITY ||
        config->maximum_ingress_bytes == 0u ||
        config->maximum_ingress_bytes > IRIS_FLOWMQ_MAX_BUFFER_BYTES ||
        config->send_queue_capacity == 0u ||
        config->send_queue_capacity > IRIS_FLOWMQ_MAX_QUEUE_CAPACITY ||
        config->send_queue_bytes == 0u ||
        config->send_queue_bytes > IRIS_FLOWMQ_MAX_BUFFER_BYTES ||
        config->start_timeout_ns == 0u ||
        config->reconnect_initial_ms > config->reconnect_max_ms) {
        return SALTS_EINVAL;
    }
    secure = secure_transport(config->transport);
    if (secure) {
        if (!nonempty(config->iris_certificate_sha256) ||
            !nonempty(config->ca_file) || !nonempty(config->certificate_file) ||
            !nonempty(config->private_key_file) ||
            !nonempty(config->server_name)) {
            return SALTS_EINVAL;
        }
    } else if (!development_transport(config->transport) ||
               !config->allow_insecure_development_loopback ||
               !loopback(config->host) ||
               nonempty(config->iris_certificate_sha256) ||
               nonempty(config->ca_file) || nonempty(config->certificate_file) ||
               nonempty(config->private_key_file) ||
               nonempty(config->private_key_password) ||
               nonempty(config->server_name)) {
        return SALTS_EINVAL;
    }
    return SALTS_OK;
}

static int same_view(vstr view, const char *value) {
    size_t length = value ? strlen(value) : 0u;
    return view.len == length &&
           (length == 0u || memcmp(view.data, value, length) == 0);
}

static int verify_iris(void *context, const char *certificate_sha256,
                       vstr claimed_identity) {
    iris_flowmq_provider_t *provider = (iris_flowmq_provider_t *)context;
    if (!provider || !certificate_sha256 ||
        !same_view(claimed_identity, provider->iris_identity)) {
        return SALTS_EPERM;
    }
    return strcmp(certificate_sha256,
                  provider->iris_certificate_sha256) == 0
               ? SALTS_OK
               : SALTS_EPERM;
}

static int reserve_bytes(iris_flowmq_provider_t *provider, size_t size) {
    size_t current = atomic_load_explicit(&provider->pending_ingress_bytes,
                                          memory_order_acquire);
    for (;;) {
        if (size > provider->maximum_ingress_bytes ||
            current > provider->maximum_ingress_bytes - size) {
            return 0;
        }
        if (atomic_compare_exchange_weak_explicit(
                &provider->pending_ingress_bytes, &current, current + size,
                memory_order_acq_rel, memory_order_acquire)) {
            return 1;
        }
    }
}

static int utc_now(void *context, char *out, size_t capacity) {
    time_t current;
    struct tm value;
    int written;
    (void)context;
    if (!out || capacity < 21u) return SALTS_EINVAL;
    current = time(NULL);
#ifdef _WIN32
    if (gmtime_s(&value, &current) != 0) return SALTS_EIO;
#else
    if (!gmtime_r(&current, &value)) return SALTS_EIO;
#endif
    written = snprintf(out, capacity, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                       value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
                       value.tm_hour, value.tm_min, value.tm_sec);
    return written > 0 && (size_t)written < capacity ? SALTS_OK : SALTS_ENOSPC;
}

static int send_application(iris_flowmq_provider_t *provider,
                            uint64_t message_id, const uint8_t *payload,
                            size_t payload_size) {
    flowmq_protocol_frame_t frame;
    tstr encoded = NULL;
    uint64_t completion_id;
    int status;
    memset(&frame, 0, sizeof(frame));
    frame.kind = FLOWMQ_PROTOCOL_FRAME_DATA;
    frame.pattern = FLOWMQ_PROTOCOL_DEALER;
    frame.message_id = message_id;
    frame.payload = vstr_from_buf((const char *)payload, payload_size);
    status = flowmq_protocol_encode_frame(&frame, provider->maximum_frame_bytes,
                                          &encoded);
    if (status == SALTS_OK) {
        completion_id = atomic_fetch_add_explicit(
            &provider->next_send_completion_id, 1u, memory_order_relaxed);
        status = flowmq_connect_endpoint_send_copy(
            provider->endpoint, completion_id, encoded, tstr_len(encoded));
    }
    tstr_freep(&encoded);
    return status;
}

static void process_completion_ack(iris_flowmq_provider_t *provider,
                                   const unsigned char *payload,
                                   size_t payload_size) {
    iris_media_completion_t completion;
    iris_flowmq_completion_ack_t ack;
    char message_id[256];
    uint64_t generation;
    int active;
    salts_mutex_lock(&provider->ack_mutex);
    active = provider->pending_completion_active;
    completion = provider->pending_completion;
    memcpy(message_id, provider->pending_completion_message_id,
           sizeof(message_id));
    generation = provider->pending_delivery_generation;
    salts_mutex_unlock(&provider->ack_mutex);
    if (!active ||
        iris_flowmq_provider_decode_completion_ack(
            provider->codec, payload, payload_size, &completion,
            provider->provider_id, provider->iris_identity, message_id,
            &ack) != IVR_OK) {
        return;
    }
    salts_mutex_lock(&provider->ack_mutex);
    if (provider->pending_completion_active &&
        provider->pending_delivery_generation == generation &&
        strcmp(provider->pending_completion_message_id, message_id) == 0) {
        provider->pending_completion_ack = ack;
        provider->pending_completion_done = 1;
        salts_cond_broadcast(&provider->ack_changed);
    }
    salts_mutex_unlock(&provider->ack_mutex);
}

static void process_event_ack(iris_flowmq_provider_t *provider,
                              const unsigned char *payload,
                              size_t payload_size) {
    ivr_media_event_t event;
    iris_flowmq_event_ack_t ack;
    char message_id[256];
    uint64_t generation;
    int active;
    salts_mutex_lock(&provider->ack_mutex);
    active = provider->pending_event_active;
    event = provider->pending_event;
    memcpy(message_id, provider->pending_event_message_id,
           sizeof(message_id));
    generation = provider->pending_delivery_generation;
    salts_mutex_unlock(&provider->ack_mutex);
    if (!active ||
        iris_flowmq_provider_decode_event_ack(
            provider->codec, payload, payload_size, &event,
            provider->provider_id, provider->iris_identity, message_id,
            &ack) != IVR_OK) {
        return;
    }
    salts_mutex_lock(&provider->ack_mutex);
    if (provider->pending_event_active &&
        provider->pending_delivery_generation == generation &&
        strcmp(provider->pending_event_message_id, message_id) == 0) {
        provider->pending_event_ack = ack;
        provider->pending_event_done = 1;
        salts_cond_broadcast(&provider->ack_changed);
    }
    salts_mutex_unlock(&provider->ack_mutex);
}

static void process_observation(iris_flowmq_provider_t *provider,
                                const unsigned char *payload,
                                size_t payload_size) {
    iris_flowmq_provider_observation_t observation;
    char tenant_id[128];
    char query_id[256];
    char query_type[128];
    uint64_t cursor;
    uint64_t generation;
    int active;
    salts_mutex_lock(&provider->ack_mutex);
    active = provider->pending_query_active;
    memcpy(tenant_id, provider->pending_query_tenant_id, sizeof(tenant_id));
    memcpy(query_id, provider->pending_query_id, sizeof(query_id));
    memcpy(query_type, provider->pending_query_type, sizeof(query_type));
    cursor = provider->pending_query_cursor;
    generation = provider->pending_query_generation;
    salts_mutex_unlock(&provider->ack_mutex);
    if (!active ||
        iris_flowmq_provider_decode_observation(
            provider->codec, payload, payload_size, tenant_id,
            provider->provider_id, provider->iris_identity, query_id,
            query_type, cursor, &observation) != IVR_OK) {
        return;
    }
    salts_mutex_lock(&provider->ack_mutex);
    if (provider->pending_query_active &&
        provider->pending_query_generation == generation &&
        strcmp(provider->pending_query_id, query_id) == 0) {
        iris_flowmq_provider_observation_clear(
            &provider->pending_observation);
        provider->pending_observation = observation;
        iris_flowmq_provider_observation_init(&observation);
        provider->pending_query_done = 1;
        salts_cond_broadcast(&provider->ack_changed);
    }
    salts_mutex_unlock(&provider->ack_mutex);
    iris_flowmq_provider_observation_clear(&observation);
}

static void process_session_bound(iris_flowmq_provider_t *provider,
                                  const unsigned char *payload,
                                  size_t payload_size) {
    iris_flowmq_call_offer_t offer;
    iris_flowmq_session_bound_t bound;
    uint64_t generation;
    int active;
    salts_mutex_lock(&provider->ack_mutex);
    active = provider->pending_offer_active;
    offer = provider->pending_offer;
    generation = provider->pending_offer_generation;
    salts_mutex_unlock(&provider->ack_mutex);
    if (!active ||
        iris_flowmq_provider_decode_session_bound(
            provider->codec, payload, payload_size, &offer,
            provider->provider_id, provider->iris_identity, &bound) != IVR_OK) {
        return;
    }
    salts_mutex_lock(&provider->ack_mutex);
    if (provider->pending_offer_active &&
        provider->pending_offer_generation == generation &&
        strcmp(provider->pending_offer.ingress_event_id,
               offer.ingress_event_id) == 0) {
        provider->pending_session_bound = bound;
        provider->pending_offer_done = 1;
        salts_cond_broadcast(&provider->ack_changed);
    }
    salts_mutex_unlock(&provider->ack_mutex);
}

static iris_media_bridge_result_t rejected_result(const char *code,
                                                  const char *message) {
    iris_media_bridge_result_t result;
    memset(&result, 0, sizeof(result));
    result.status = IRIS_MEDIA_BRIDGE_INVALID;
    result.error_code = code;
    result.error_message = message;
    return result;
}

static void process_command(void *context) {
    iris_flowmq_work_item_t *item = (iris_flowmq_work_item_t *)context;
    iris_flowmq_provider_t *provider;
    iris_flowmq_provider_command_t command;
    iris_media_bridge_result_t result;
    uint8_t *receipt = NULL;
    size_t receipt_size = 0u;
    char created_at[32];
    int decoded = 0;
    ProviderMessageKind_t kind = ProviderMessageKind_Receipt;
    if (!item || !item->owner) {
        free(item);
        return;
    }
    provider = item->owner;
    if (flowmq_media_provider_peek_kind(item->payload, item->payload_size,
                                        &kind) == SALTS_OK &&
        kind == ProviderMessageKind_CompletionAck) {
        process_completion_ack(provider, item->payload, item->payload_size);
    } else if (kind == ProviderMessageKind_EventAck) {
        process_event_ack(provider, item->payload, item->payload_size);
    } else if (kind == ProviderMessageKind_Observation) {
        process_observation(provider, item->payload, item->payload_size);
    } else if (kind == ProviderMessageKind_SessionBound) {
        process_session_bound(provider, item->payload, item->payload_size);
    } else if (kind == ProviderMessageKind_Command &&
               iris_flowmq_provider_decode_command(
            provider->codec, item->payload, item->payload_size, &command) ==
        IVR_OK) {
        decoded = 1;
        if (strcmp(command.wire.provider_id, provider->provider_id) != 0 ||
            strcmp(command.wire.producer_id, provider->iris_identity) != 0) {
            result = rejected_result("PROVIDER_IDENTITY_MISMATCH",
                                     "command identity does not match the authenticated route");
        } else {
            result = provider->dispatch(provider->dispatch_context,
                                        command.wire.command_id,
                                        command.bridge_json,
                                        command.bridge_json_size);
        }
        if ((provider->now ? provider->now : utc_now)(
                provider->now_context, created_at, sizeof(created_at)) ==
                SALTS_OK &&
            iris_flowmq_provider_encode_receipt(
                &command, &result, provider->provider_instance_id, created_at,
                &receipt, &receipt_size) == IVR_OK) {
            (void)send_application(provider, item->frame_message_id, receipt,
                                   receipt_size);
        }
    }
    if (decoded) iris_flowmq_provider_command_clear(&command);
    tbe_typed_serialized_free(receipt);
    atomic_fetch_sub_explicit(&provider->pending_ingress_bytes,
                              item->payload_size, memory_order_acq_rel);
    free(item);
}

static int on_frame(void *context, const flowmq_protocol_frame_t *frame,
                    uint64_t generation) {
    iris_flowmq_provider_t *provider = (iris_flowmq_provider_t *)context;
    iris_flowmq_work_item_t *item = NULL;
    size_t allocation_size;
    int status = SALTS_OK;
    (void)generation;
    if (!provider) return SALTS_EINVAL;
    atomic_fetch_add_explicit(&provider->callback_count, 1u,
                              memory_order_acq_rel);
    if (!atomic_load_explicit(&provider->accepting, memory_order_acquire)) {
        status = SALTS_ESHUTDOWN;
    } else if (!frame || frame->kind != FLOWMQ_PROTOCOL_FRAME_DATA ||
               frame->pattern != FLOWMQ_PROTOCOL_ROUTER ||
               !frame->payload.data || frame->payload.len == 0u ||
               frame->payload.len > provider->maximum_frame_bytes) {
        status = SALTS_EPROTO;
    } else if (!reserve_bytes(provider, frame->payload.len)) {
        status = SALTS_ENOSPC;
    } else if (frame->payload.len > SIZE_MAX - sizeof(*item)) {
        atomic_fetch_sub_explicit(&provider->pending_ingress_bytes,
                                  frame->payload.len, memory_order_acq_rel);
        status = SALTS_ENOSPC;
    } else {
        allocation_size = sizeof(*item) + frame->payload.len;
        item = (iris_flowmq_work_item_t *)malloc(allocation_size);
        if (!item) {
            atomic_fetch_sub_explicit(&provider->pending_ingress_bytes,
                                      frame->payload.len,
                                      memory_order_acq_rel);
            status = SALTS_ENOMEM;
        } else {
            item->owner = provider;
            item->frame_message_id = frame->message_id;
            item->payload_size = frame->payload.len;
            memcpy(item->payload, frame->payload.data, frame->payload.len);
            if (salts_threadpool_try_submit(provider->worker, process_command,
                                            item) != 0) {
                atomic_fetch_sub_explicit(&provider->pending_ingress_bytes,
                                          frame->payload.len,
                                          memory_order_acq_rel);
                free(item);
                status = SALTS_ENOSPC;
            }
        }
    }
    atomic_fetch_sub_explicit(&provider->callback_count, 1u,
                              memory_order_acq_rel);
    return status;
}

static void on_state(void *context,
                     flowmq_connect_endpoint_connection_state_t state,
                     int status, size_t connections_current) {
    iris_flowmq_provider_t *provider = (iris_flowmq_provider_t *)context;
    (void)status;
    (void)connections_current;
    if (!provider) return;
    atomic_store_explicit(&provider->running,
                          state == FLOWMQ_ENDPOINT_CONNECTION_READY,
                          memory_order_release);
    salts_mutex_lock(&provider->ack_mutex);
    salts_cond_broadcast(&provider->ack_changed);
    salts_mutex_unlock(&provider->ack_mutex);
}

iris_flowmq_provider_t *iris_flowmq_provider_create(
    const iris_flowmq_provider_config_t *config) {
    iris_flowmq_provider_t *provider = NULL;
    flowmq_connect_endpoint_config_t endpoint_config;
    flowmq_coronet_tls_client_config_t tls;
    salts_threadpool_config_t worker_config;
    DataBindError error = DATA_BIND_ERROR_INIT;
    int status;
    if (iris_flowmq_provider_config_validate(config) != SALTS_OK) return NULL;
    provider = (iris_flowmq_provider_t *)calloc(1u, sizeof(*provider));
    if (!provider) return NULL;
    salts_mutex_init(&provider->delivery_mutex);
    salts_mutex_init(&provider->query_mutex);
    salts_mutex_init(&provider->offer_mutex);
    salts_mutex_init(&provider->ack_mutex);
    salts_cond_init(&provider->ack_changed);
    atomic_init(&provider->pending_ingress_bytes, 0u);
    atomic_init(&provider->callback_count, 0u);
    atomic_init(&provider->running, 0);
    atomic_init(&provider->stopped, 0);
    atomic_init(&provider->accepting, 1);
    atomic_init(&provider->next_send_completion_id, 1u);
    iris_flowmq_provider_observation_init(&provider->pending_observation);
    provider->provider_id = tstr_dup(config->provider_id);
    provider->provider_instance_id = tstr_dup(config->provider_instance_id);
    provider->iris_identity = tstr_dup(config->iris_identity);
    provider->iris_certificate_sha256 =
        tstr_dup(config->iris_certificate_sha256
                     ? config->iris_certificate_sha256
                     : "");
    if (!provider->provider_id || !provider->provider_instance_id ||
        !provider->iris_identity || !provider->iris_certificate_sha256 ||
        FlowMqMediaProviderV1_codec_create(&provider->codec, &error) !=
            DATA_BIND_OK) {
        iris_flowmq_provider_destroy(provider);
        return NULL;
    }
    provider->maximum_frame_bytes = config->maximum_frame_bytes;
    provider->maximum_ingress_bytes = config->maximum_ingress_bytes;
    provider->start_timeout_ns = config->start_timeout_ns;
    provider->dispatch = config->dispatch;
    provider->dispatch_context = config->dispatch_context;
    provider->now = config->now;
    provider->now_context = config->now_context;
    memset(&worker_config, 0, sizeof(worker_config));
    worker_config.num_threads = 1;
    worker_config.queue_capacity = config->maximum_ingress_messages;
    provider->worker = salts_threadpool_create_with_config(&worker_config);
    if (!provider->worker) {
        iris_flowmq_provider_destroy(provider);
        return NULL;
    }

    memset(&tls, 0, sizeof(tls));
    flowmq_connect_endpoint_config_init(&endpoint_config);
    endpoint_config.transport = config->transport;
    endpoint_config.pattern = FLOWMQ_PROTOCOL_DEALER;
    endpoint_config.host = config->host;
    endpoint_config.path = config->path ? config->path : "";
    endpoint_config.topic = config->topic;
    endpoint_config.identity = config->provider_instance_id;
    endpoint_config.port = (int)config->port;
    endpoint_config.max_frame_size = config->maximum_frame_bytes;
    endpoint_config.reconnect_initial_ms = config->reconnect_initial_ms;
    endpoint_config.reconnect_max_ms = config->reconnect_max_ms;
    endpoint_config.context = NULL;
    endpoint_config.drive_context = 1;
    endpoint_config.own_context = 1;
    endpoint_config.on_frame = on_frame;
    endpoint_config.on_state = on_state;
    endpoint_config.callback_ctx = provider;
    endpoint_config.send_admission.capacity = config->send_queue_capacity;
    endpoint_config.send_admission.capacity_bytes = config->send_queue_bytes;
    if (secure_transport(config->transport)) {
        tls.ca_file = config->ca_file;
        tls.cert_file = config->certificate_file;
        tls.key_file = config->private_key_file;
        tls.key_password = nonempty(config->private_key_password)
                               ? config->private_key_password
                               : NULL;
        tls.server_name = config->server_name;
        tls.verify_peer = 1;
        endpoint_config.tls = &tls;
        endpoint_config.verify_peer_identity = verify_iris;
        endpoint_config.verify_peer_identity_ctx = provider;
    }
    status = flowmq_connect_endpoint_create(&endpoint_config,
                                             &provider->endpoint);
    if (status != SALTS_OK || !provider->endpoint) {
        iris_flowmq_provider_destroy(provider);
        return NULL;
    }
    return provider;
}

int iris_flowmq_provider_start(iris_flowmq_provider_t *provider) {
    int status;
    if (!provider || !provider->endpoint ||
        atomic_load_explicit(&provider->stopped, memory_order_acquire)) {
        return SALTS_EINVAL;
    }
    status = flowmq_connect_endpoint_start(provider->endpoint,
                                            provider->start_timeout_ns);
    if (status == SALTS_OK) {
        atomic_store_explicit(&provider->running, 1, memory_order_release);
    }
    return status;
}

void iris_flowmq_provider_stop(iris_flowmq_provider_t *provider) {
    unsigned attempt;
    int endpoint_stopped = 0;
    if (!provider || atomic_exchange_explicit(&provider->stopped, 1,
                                               memory_order_acq_rel)) {
        return;
    }
    atomic_store_explicit(&provider->accepting, 0, memory_order_release);
    salts_mutex_lock(&provider->ack_mutex);
    salts_cond_broadcast(&provider->ack_changed);
    salts_mutex_unlock(&provider->ack_mutex);
    salts_mutex_lock(&provider->delivery_mutex);
    salts_mutex_unlock(&provider->delivery_mutex);
    salts_mutex_lock(&provider->query_mutex);
    salts_mutex_unlock(&provider->query_mutex);
    salts_mutex_lock(&provider->offer_mutex);
    salts_mutex_unlock(&provider->offer_mutex);
    for (attempt = 0u; attempt < IRIS_FLOWMQ_CALLBACK_QUIESCE_ATTEMPTS;
         ++attempt) {
        if (atomic_load_explicit(&provider->callback_count,
                                 memory_order_acquire) == 0u) {
            break;
        }
        salts_sleep_ms(IRIS_FLOWMQ_CALLBACK_QUIESCE_STEP_MS);
    }
    if (atomic_load_explicit(&provider->callback_count,
                             memory_order_acquire) != 0u) {
        flowmq_connect_endpoint_stop(provider->endpoint);
        endpoint_stopped = 1;
    }
    if (provider->worker) {
        salts_threadpool_shutdown(provider->worker);
        salts_threadpool_wait(provider->worker);
        salts_threadpool_destroy(provider->worker);
        provider->worker = NULL;
    }
    if (!endpoint_stopped) flowmq_connect_endpoint_stop(provider->endpoint);
    atomic_store_explicit(&provider->running, 0, memory_order_release);
}

void iris_flowmq_provider_destroy(iris_flowmq_provider_t *provider) {
    if (!provider) return;
    iris_flowmq_provider_stop(provider);
    flowmq_connect_endpoint_destroy(provider->endpoint);
    provider->endpoint = NULL;
    data_bind_free(provider->codec);
    provider->codec = NULL;
    iris_flowmq_provider_observation_clear(&provider->pending_observation);
    turbo_crypto_wipe(provider->iris_certificate_sha256,
                      provider->iris_certificate_sha256
                          ? tstr_len(provider->iris_certificate_sha256)
                          : 0u);
    tstr_freep(&provider->provider_id);
    tstr_freep(&provider->provider_instance_id);
    tstr_freep(&provider->iris_identity);
    tstr_freep(&provider->iris_certificate_sha256);
    salts_cond_destroy(&provider->ack_changed);
    salts_mutex_destroy(&provider->ack_mutex);
    salts_mutex_destroy(&provider->delivery_mutex);
    salts_mutex_destroy(&provider->query_mutex);
    salts_mutex_destroy(&provider->offer_mutex);
    free(provider);
}

int iris_flowmq_provider_running(const iris_flowmq_provider_t *provider) {
    return provider &&
           atomic_load_explicit(&provider->running, memory_order_acquire);
}

ivr_status_t iris_flowmq_provider_send_completion(
    iris_flowmq_provider_t *provider,
    const iris_media_completion_t *completion,
    const ivr_media_command_result_t *result, const char *message_id,
    const char *completed_at, uint64_t completed_at_unix_ms,
    uint64_t ack_timeout_ms, iris_flowmq_completion_ack_t *out_ack) {
    uint8_t *application = NULL;
    size_t application_size = 0u;
    uint64_t deadline;
    ivr_status_t status = IVR_ESTATE;
    if (!provider || !completion || !result || !message_id || !message_id[0] ||
        !completed_at || !completed_at[0] || completed_at_unix_ms == 0u ||
        ack_timeout_ms == 0u ||
        ack_timeout_ms > IRIS_FLOWMQ_MAX_ACK_TIMEOUT_MS || !out_ack) {
        return IVR_EINVAL;
    }
    memset(out_ack, 0, sizeof(*out_ack));
    if (!atomic_load_explicit(&provider->running, memory_order_acquire) ||
        atomic_load_explicit(&provider->stopped, memory_order_acquire)) {
        return IVR_ECLOSED;
    }
    if (iris_flowmq_provider_encode_completion(
            completion, result, provider->provider_id,
            provider->provider_instance_id, message_id, completed_at,
            completed_at_unix_ms, &application, &application_size) != IVR_OK) {
        return IVR_EINVAL;
    }
    salts_mutex_lock(&provider->delivery_mutex);
    salts_mutex_lock(&provider->ack_mutex);
    if (provider->pending_completion_active) {
        salts_mutex_unlock(&provider->ack_mutex);
        salts_mutex_unlock(&provider->delivery_mutex);
        tbe_typed_serialized_free(application);
        return IVR_EBUSY;
    }
    provider->pending_completion = *completion;
    if (snprintf(provider->pending_completion_message_id,
                 sizeof(provider->pending_completion_message_id), "%s",
                 message_id) <= 0 ||
        strlen(message_id) >=
            sizeof(provider->pending_completion_message_id)) {
        salts_mutex_unlock(&provider->ack_mutex);
        salts_mutex_unlock(&provider->delivery_mutex);
        tbe_typed_serialized_free(application);
        return IVR_ENOSPC;
    }
    provider->next_pending_generation++;
    if (provider->next_pending_generation == 0u) {
        provider->next_pending_generation = 1u;
    }
    provider->pending_delivery_generation = provider->next_pending_generation;
    provider->pending_completion_active = 1;
    provider->pending_completion_done = 0;
    memset(&provider->pending_completion_ack, 0,
           sizeof(provider->pending_completion_ack));
    salts_mutex_unlock(&provider->ack_mutex);

    if (send_application(provider, provider->pending_delivery_generation,
                         application,
                         application_size) != SALTS_OK) {
        status = IVR_EBUSY;
    } else {
        deadline = salts_monotonic_ms();
        deadline = UINT64_MAX - deadline < ack_timeout_ms
                       ? UINT64_MAX
                       : deadline + ack_timeout_ms;
        salts_mutex_lock(&provider->ack_mutex);
        while (!provider->pending_completion_done &&
               !atomic_load_explicit(&provider->stopped,
                                     memory_order_acquire)) {
            uint64_t now = salts_monotonic_ms();
            uint64_t remaining;
            if (now >= deadline) break;
            remaining = deadline - now;
            (void)salts_cond_timedwait(&provider->ack_changed,
                                       &provider->ack_mutex,
                                       remaining * UINT64_C(1000000));
        }
        if (provider->pending_completion_done) {
            *out_ack = provider->pending_completion_ack;
            status = IVR_OK;
        } else if (atomic_load_explicit(&provider->stopped,
                                        memory_order_acquire)) {
            status = IVR_ECLOSED;
        } else {
            status = IVR_EBUSY;
        }
        provider->pending_completion_active = 0;
        provider->pending_completion_done = 0;
        salts_mutex_unlock(&provider->ack_mutex);
    }
    if (status != IVR_OK) {
        salts_mutex_lock(&provider->ack_mutex);
        provider->pending_completion_active = 0;
        provider->pending_completion_done = 0;
        salts_mutex_unlock(&provider->ack_mutex);
    }
    salts_mutex_unlock(&provider->delivery_mutex);
    tbe_typed_serialized_free(application);
    return status;
}

ivr_status_t iris_flowmq_provider_send_event(
    iris_flowmq_provider_t *provider, const ivr_media_event_t *event,
    const char *message_id, const char *occurred_at, uint64_t ack_timeout_ms,
    iris_flowmq_event_ack_t *out_ack) {
    uint8_t *application = NULL;
    size_t application_size = 0u;
    uint64_t deadline;
    ivr_status_t status = IVR_ESTATE;
    if (!provider || !event || !message_id || !message_id[0] ||
        !occurred_at || !occurred_at[0] || ack_timeout_ms == 0u ||
        ack_timeout_ms > IRIS_FLOWMQ_MAX_ACK_TIMEOUT_MS || !out_ack) {
        return IVR_EINVAL;
    }
    memset(out_ack, 0, sizeof(*out_ack));
    if (!atomic_load_explicit(&provider->running, memory_order_acquire) ||
        atomic_load_explicit(&provider->stopped, memory_order_acquire)) {
        return IVR_ECLOSED;
    }
    if (iris_flowmq_provider_encode_event(
            event, provider->provider_id, provider->provider_instance_id,
            message_id, occurred_at, occurred_at, &application,
            &application_size) != IVR_OK) {
        return IVR_EINVAL;
    }
    salts_mutex_lock(&provider->delivery_mutex);
    salts_mutex_lock(&provider->ack_mutex);
    if (provider->pending_event_active || provider->pending_completion_active) {
        salts_mutex_unlock(&provider->ack_mutex);
        salts_mutex_unlock(&provider->delivery_mutex);
        tbe_typed_serialized_free(application);
        return IVR_EBUSY;
    }
    provider->pending_event = *event;
    if (snprintf(provider->pending_event_message_id,
                 sizeof(provider->pending_event_message_id), "%s",
                 message_id) <= 0 ||
        strlen(message_id) >= sizeof(provider->pending_event_message_id)) {
        salts_mutex_unlock(&provider->ack_mutex);
        salts_mutex_unlock(&provider->delivery_mutex);
        tbe_typed_serialized_free(application);
        return IVR_ENOSPC;
    }
    provider->next_pending_generation++;
    if (provider->next_pending_generation == 0u) {
        provider->next_pending_generation = 1u;
    }
    provider->pending_delivery_generation = provider->next_pending_generation;
    provider->pending_event_active = 1;
    provider->pending_event_done = 0;
    memset(&provider->pending_event_ack, 0,
           sizeof(provider->pending_event_ack));
    salts_mutex_unlock(&provider->ack_mutex);

    if (send_application(provider, provider->pending_delivery_generation,
                         application,
                         application_size) != SALTS_OK) {
        status = IVR_EBUSY;
    } else {
        deadline = salts_monotonic_ms();
        deadline = UINT64_MAX - deadline < ack_timeout_ms
                       ? UINT64_MAX
                       : deadline + ack_timeout_ms;
        salts_mutex_lock(&provider->ack_mutex);
        while (!provider->pending_event_done &&
               !atomic_load_explicit(&provider->stopped,
                                     memory_order_acquire)) {
            uint64_t now = salts_monotonic_ms();
            uint64_t remaining;
            if (now >= deadline) break;
            remaining = deadline - now;
            (void)salts_cond_timedwait(&provider->ack_changed,
                                       &provider->ack_mutex,
                                       remaining * UINT64_C(1000000));
        }
        if (provider->pending_event_done) {
            *out_ack = provider->pending_event_ack;
            status = IVR_OK;
        } else if (atomic_load_explicit(&provider->stopped,
                                        memory_order_acquire)) {
            status = IVR_ECLOSED;
        } else {
            status = IVR_EBUSY;
        }
        provider->pending_event_active = 0;
        provider->pending_event_done = 0;
        salts_mutex_unlock(&provider->ack_mutex);
    }
    if (status != IVR_OK) {
        salts_mutex_lock(&provider->ack_mutex);
        provider->pending_event_active = 0;
        provider->pending_event_done = 0;
        salts_mutex_unlock(&provider->ack_mutex);
    }
    salts_mutex_unlock(&provider->delivery_mutex);
    tbe_typed_serialized_free(application);
    return status;
}

static int copy_pending_text(char *out, size_t capacity, const char *value) {
    size_t length = value ? strlen(value) : 0u;
    if (!out || capacity == 0u || length == 0u || length >= capacity) return 0;
    memcpy(out, value, length + 1u);
    return 1;
}

ivr_status_t iris_flowmq_provider_send_query(
    iris_flowmq_provider_t *provider,
    const iris_flowmq_provider_query_t *query, uint64_t timeout_ms,
    iris_flowmq_provider_observation_t *out_observation) {
    uint8_t *application = NULL;
    size_t application_size = 0u;
    uint64_t deadline;
    ivr_status_t status = IVR_ESTATE;
    if (!provider || !query || !out_observation || !query->tenant_id ||
        !query->query_id || !query->query_type || !query->created_at ||
        !query->deadline_at || !query->payload_json || query->limit == 0u ||
        timeout_ms == 0u || timeout_ms > IRIS_FLOWMQ_MAX_ACK_TIMEOUT_MS) {
        return IVR_EINVAL;
    }
    iris_flowmq_provider_observation_init(out_observation);
    if (!atomic_load_explicit(&provider->running, memory_order_acquire) ||
        atomic_load_explicit(&provider->stopped, memory_order_acquire)) {
        return IVR_ECLOSED;
    }
    if (iris_flowmq_provider_encode_query(
            query->tenant_id, provider->provider_id,
            provider->provider_instance_id, query->query_id,
            query->query_type, query->created_at, query->deadline_at,
            query->expected_revision, query->cursor, query->limit,
            query->payload_json, &application, &application_size) != IVR_OK) {
        return IVR_EINVAL;
    }
    salts_mutex_lock(&provider->query_mutex);
    salts_mutex_lock(&provider->ack_mutex);
    if (provider->pending_query_active ||
        !copy_pending_text(provider->pending_query_tenant_id,
                           sizeof(provider->pending_query_tenant_id),
                           query->tenant_id) ||
        !copy_pending_text(provider->pending_query_id,
                           sizeof(provider->pending_query_id), query->query_id) ||
        !copy_pending_text(provider->pending_query_type,
                           sizeof(provider->pending_query_type),
                           query->query_type)) {
        salts_mutex_unlock(&provider->ack_mutex);
        salts_mutex_unlock(&provider->query_mutex);
        tbe_typed_serialized_free(application);
        return IVR_EBUSY;
    }
    provider->next_pending_generation++;
    if (provider->next_pending_generation == 0u) {
        provider->next_pending_generation = 1u;
    }
    provider->pending_query_generation = provider->next_pending_generation;
    provider->pending_query_cursor = query->cursor;
    provider->pending_query_active = 1;
    provider->pending_query_done = 0;
    iris_flowmq_provider_observation_clear(&provider->pending_observation);
    iris_flowmq_provider_observation_init(&provider->pending_observation);
    salts_mutex_unlock(&provider->ack_mutex);

    if (send_application(provider, provider->pending_query_generation,
                         application,
                         application_size) != SALTS_OK) {
        status = IVR_EBUSY;
    } else {
        deadline = salts_monotonic_ms();
        deadline = UINT64_MAX - deadline < timeout_ms
                       ? UINT64_MAX
                       : deadline + timeout_ms;
        salts_mutex_lock(&provider->ack_mutex);
        while (!provider->pending_query_done &&
               !atomic_load_explicit(&provider->stopped,
                                     memory_order_acquire)) {
            uint64_t now = salts_monotonic_ms();
            uint64_t remaining;
            if (now >= deadline) break;
            remaining = deadline - now;
            (void)salts_cond_timedwait(&provider->ack_changed,
                                       &provider->ack_mutex,
                                       remaining * UINT64_C(1000000));
        }
        if (provider->pending_query_done) {
            *out_observation = provider->pending_observation;
            iris_flowmq_provider_observation_init(
                &provider->pending_observation);
            status = IVR_OK;
        } else if (atomic_load_explicit(&provider->stopped,
                                        memory_order_acquire)) {
            status = IVR_ECLOSED;
        } else {
            status = IVR_EBUSY;
        }
        provider->pending_query_active = 0;
        provider->pending_query_done = 0;
        salts_mutex_unlock(&provider->ack_mutex);
    }
    if (status != IVR_OK) {
        salts_mutex_lock(&provider->ack_mutex);
        provider->pending_query_active = 0;
        provider->pending_query_done = 0;
        iris_flowmq_provider_observation_clear(
            &provider->pending_observation);
        iris_flowmq_provider_observation_init(&provider->pending_observation);
        salts_mutex_unlock(&provider->ack_mutex);
    }
    salts_mutex_unlock(&provider->query_mutex);
    tbe_typed_serialized_free(application);
    return status;
}

ivr_status_t iris_flowmq_provider_send_call_offer(
    iris_flowmq_provider_t *provider,
    const iris_flowmq_call_offer_t *offer, uint64_t timeout_ms,
    iris_flowmq_session_bound_t *out_bound) {
    uint8_t *application = NULL;
    size_t application_size = 0u;
    uint64_t deadline;
    int send_status;
    ivr_status_t status = IVR_ESTATE;
    if (!provider || !offer || !out_bound || timeout_ms == 0u ||
        timeout_ms > IRIS_FLOWMQ_MAX_ACK_TIMEOUT_MS) {
        return IVR_EINVAL;
    }
    memset(out_bound, 0, sizeof(*out_bound));
    if (!atomic_load_explicit(&provider->running, memory_order_acquire) ||
        atomic_load_explicit(&provider->stopped, memory_order_acquire)) {
        return IVR_ECLOSED;
    }
    if (iris_flowmq_provider_encode_call_offer(
            offer, provider->provider_id, provider->provider_instance_id,
            &application, &application_size) != IVR_OK) {
        return IVR_EINVAL;
    }

    salts_mutex_lock(&provider->offer_mutex);
    salts_mutex_lock(&provider->ack_mutex);
    if (provider->pending_offer_active) {
        salts_mutex_unlock(&provider->ack_mutex);
        salts_mutex_unlock(&provider->offer_mutex);
        tbe_typed_serialized_free(application);
        return IVR_EBUSY;
    }
    provider->next_pending_generation++;
    if (provider->next_pending_generation == 0u) {
        provider->next_pending_generation = 1u;
    }
    provider->pending_offer_generation = provider->next_pending_generation;
    provider->pending_offer = *offer;
    memset(&provider->pending_session_bound, 0,
           sizeof(provider->pending_session_bound));
    provider->pending_offer_active = 1;
    provider->pending_offer_done = 0;
    salts_mutex_unlock(&provider->ack_mutex);

    send_status = send_application(provider, provider->pending_offer_generation,
                                   application, application_size);
    if (send_status != SALTS_OK) {
        status = send_status == SALTS_ENOSPC ? IVR_ENOSPC : IVR_EBUSY;
    } else {
        deadline = salts_monotonic_ms();
        deadline = UINT64_MAX - deadline < timeout_ms
                       ? UINT64_MAX
                       : deadline + timeout_ms;
        salts_mutex_lock(&provider->ack_mutex);
        while (!provider->pending_offer_done &&
               !atomic_load_explicit(&provider->stopped,
                                     memory_order_acquire) &&
               atomic_load_explicit(&provider->running,
                                    memory_order_acquire)) {
            uint64_t now = salts_monotonic_ms();
            uint64_t remaining;
            if (now >= deadline) break;
            remaining = deadline - now;
            (void)salts_cond_timedwait(&provider->ack_changed,
                                       &provider->ack_mutex,
                                       remaining * UINT64_C(1000000));
        }
        if (provider->pending_offer_done) {
            *out_bound = provider->pending_session_bound;
            status = IVR_OK;
        } else if (atomic_load_explicit(&provider->stopped,
                                        memory_order_acquire)) {
            status = IVR_ECLOSED;
        } else {
            status = IVR_EBUSY;
        }
        provider->pending_offer_active = 0;
        provider->pending_offer_done = 0;
        salts_mutex_unlock(&provider->ack_mutex);
    }
    if (status != IVR_OK) {
        salts_mutex_lock(&provider->ack_mutex);
        provider->pending_offer_active = 0;
        provider->pending_offer_done = 0;
        salts_mutex_unlock(&provider->ack_mutex);
    }
    salts_mutex_unlock(&provider->offer_mutex);
    tbe_typed_serialized_free(application);
    return status;
}
