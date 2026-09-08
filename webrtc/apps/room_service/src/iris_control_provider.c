#include "iris_control_provider.h"

#include "iris_control_provider_codec.h"
#include "iris_provider_protocol.h"
#include "ivr_control_ws.h"

#include <turbo_crypto.h>
#include <salts_error.h>
#include <salts_str.h>
#include <salts_thread.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IRIS_CONTROL_MAX_QUEUE_CAPACITY (1024u * 1024u)
#define IRIS_CONTROL_MAX_BUFFER_BYTES (1024u * 1024u * 1024u)
#define IRIS_CONTROL_DEFAULT_FRAME_BYTES (1024u * 1024u)
#define IRIS_CONTROL_DEFAULT_INGRESS_MESSAGES 1024u
#define IRIS_CONTROL_DEFAULT_INGRESS_BYTES (8u * 1024u * 1024u)
#define IRIS_CONTROL_DEFAULT_START_TIMEOUT_MS 5000u
#define IRIS_CONTROL_CALLBACK_QUIESCE_ATTEMPTS 5000u
#define IRIS_CONTROL_CALLBACK_QUIESCE_STEP_MS 1u
#define IRIS_CONTROL_MAX_ACK_TIMEOUT_MS (5u * 60u * 1000u)

typedef struct iris_control_work_item_s {
    struct iris_control_provider_s *owner;
    uint64_t frame_message_id;
    size_t payload_size;
    unsigned char payload[];
} iris_control_work_item_t;

struct iris_control_provider_s {
    ivr_control_ws_client_t *endpoint;
    salts_threadpool_t *worker;
    tstr provider_id;
    tstr provider_instance_id;
    tstr iris_identity;
    size_t maximum_frame_bytes;
    size_t maximum_ingress_bytes;
    iris_control_provider_dispatch_fn dispatch;
    void *dispatch_context;
    iris_control_provider_now_fn now;
    void *now_context;
    salts_mutex_t delivery_mutex;
    salts_mutex_t query_mutex;
    salts_mutex_t offer_mutex;
    salts_mutex_t ack_mutex;
    salts_cond_t ack_changed;
    iris_media_completion_t pending_completion;
    iris_control_completion_ack_t pending_completion_ack;
    char pending_completion_message_id[256];
    ivr_media_event_t pending_event;
    iris_control_event_ack_t pending_event_ack;
    char pending_event_message_id[256];
    char pending_query_tenant_id[128];
    char pending_query_id[256];
    char pending_query_type[128];
    uint64_t pending_query_cursor;
    iris_control_provider_observation_t pending_observation;
    iris_control_call_offer_t pending_offer;
    iris_control_session_bound_t pending_session_bound;
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

void iris_control_provider_config_init(iris_control_provider_config_t *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->use_tls = 1;
    config->host = "127.0.0.1";
    config->path = "/internal/iris/control";
    config->provider_id = "turbomedia";
    config->maximum_frame_bytes = IRIS_CONTROL_DEFAULT_FRAME_BYTES;
    config->maximum_ingress_messages = IRIS_CONTROL_DEFAULT_INGRESS_MESSAGES;
    config->maximum_ingress_bytes = IRIS_CONTROL_DEFAULT_INGRESS_BYTES;
    config->send_queue_capacity = IRIS_CONTROL_DEFAULT_INGRESS_MESSAGES;
    config->send_queue_bytes = IRIS_CONTROL_DEFAULT_INGRESS_BYTES;
    config->start_timeout_ms = IRIS_CONTROL_DEFAULT_START_TIMEOUT_MS;
    config->reconnect_initial_ms = 100u;
    config->reconnect_max_ms = 5000u;
}

int iris_control_provider_config_validate(
    const iris_control_provider_config_t *config) {
    int secure;
    if (!config || !nonempty(config->host) || config->port == 0u ||
        !nonempty(config->path) || config->path[0] != '/' ||
        !nonempty(config->provider_id) ||
        !nonempty(config->provider_instance_id) ||
        !nonempty(config->iris_identity) || !config->dispatch ||
        config->maximum_frame_bytes == 0u ||
        config->maximum_frame_bytes > IRIS_CONTROL_MAX_BUFFER_BYTES ||
        config->maximum_ingress_messages == 0u ||
        config->maximum_ingress_messages > IRIS_CONTROL_MAX_QUEUE_CAPACITY ||
        config->maximum_ingress_bytes == 0u ||
        config->maximum_ingress_bytes > IRIS_CONTROL_MAX_BUFFER_BYTES ||
        config->send_queue_capacity == 0u ||
        config->send_queue_capacity > IRIS_CONTROL_MAX_QUEUE_CAPACITY ||
        config->send_queue_bytes == 0u ||
        config->send_queue_bytes > IRIS_CONTROL_MAX_BUFFER_BYTES ||
        config->maximum_frame_bytes > config->maximum_ingress_bytes ||
        config->maximum_frame_bytes > config->send_queue_bytes ||
        config->start_timeout_ms == 0u ||
        config->reconnect_initial_ms > UINT32_MAX ||
        config->reconnect_max_ms > UINT32_MAX ||
        config->reconnect_initial_ms > config->reconnect_max_ms) {
        return SALTS_EINVAL;
    }
    secure = config->use_tls != 0;
    if (secure) {
        if (!nonempty(config->ca_file) || !nonempty(config->certificate_file) ||
            !nonempty(config->private_key_file) ||
            !nonempty(config->server_name)) {
            return SALTS_EINVAL;
        }
    } else if (!config->allow_insecure_development_loopback ||
               !loopback(config->host) ||
               nonempty(config->ca_file) || nonempty(config->certificate_file) ||
               nonempty(config->private_key_file) ||
               nonempty(config->private_key_password) ||
               nonempty(config->server_name)) {
        return SALTS_EINVAL;
    }
    return SALTS_OK;
}

static int reserve_bytes(iris_control_provider_t *provider, size_t size) {
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

static int send_application(iris_control_provider_t *provider,
                            uint64_t message_id, const uint8_t *payload,
                            size_t payload_size) {
    (void)message_id;
    if (!provider || !provider->endpoint || !payload || payload_size == 0u ||
        payload_size > provider->maximum_frame_bytes) {
        return SALTS_EINVAL;
    }
    return ivr_control_ws_client_send_copy(provider->endpoint, payload,
                                           payload_size) == IVR_OK
               ? SALTS_OK
               : SALTS_EIO;
}

static void process_completion_ack(iris_control_provider_t *provider,
                                   const unsigned char *payload,
                                   size_t payload_size) {
    iris_media_completion_t completion;
    iris_control_completion_ack_t ack;
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
        iris_control_provider_decode_completion_ack(
            payload, payload_size, &completion,
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

static void process_event_ack(iris_control_provider_t *provider,
                              const unsigned char *payload,
                              size_t payload_size) {
    ivr_media_event_t event;
    iris_control_event_ack_t ack;
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
        iris_control_provider_decode_event_ack(
            payload, payload_size, &event,
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

static void process_observation(iris_control_provider_t *provider,
                                const unsigned char *payload,
                                size_t payload_size) {
    iris_control_provider_observation_t observation;
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
        iris_control_provider_decode_observation(
            payload, payload_size, tenant_id,
            provider->provider_id, provider->iris_identity, query_id,
            query_type, cursor, &observation) != IVR_OK) {
        return;
    }
    salts_mutex_lock(&provider->ack_mutex);
    if (provider->pending_query_active &&
        provider->pending_query_generation == generation &&
        strcmp(provider->pending_query_id, query_id) == 0) {
        iris_control_provider_observation_clear(
            &provider->pending_observation);
        provider->pending_observation = observation;
        iris_control_provider_observation_init(&observation);
        provider->pending_query_done = 1;
        salts_cond_broadcast(&provider->ack_changed);
    }
    salts_mutex_unlock(&provider->ack_mutex);
    iris_control_provider_observation_clear(&observation);
}

static void process_session_bound(iris_control_provider_t *provider,
                                  const unsigned char *payload,
                                  size_t payload_size) {
    iris_control_call_offer_t offer;
    iris_control_session_bound_t bound;
    uint64_t generation;
    int active;
    salts_mutex_lock(&provider->ack_mutex);
    active = provider->pending_offer_active;
    offer = provider->pending_offer;
    generation = provider->pending_offer_generation;
    salts_mutex_unlock(&provider->ack_mutex);
    if (!active ||
        iris_control_provider_decode_session_bound(
            payload, payload_size, &offer,
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
    iris_control_work_item_t *item = (iris_control_work_item_t *)context;
    iris_control_provider_t *provider;
    iris_control_provider_command_t command;
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
    if (iris_provider_peek_kind(item->payload, item->payload_size,
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
               iris_control_provider_decode_command(
            item->payload, item->payload_size, &command) ==
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
            iris_control_provider_encode_receipt(
                &command, &result, provider->provider_instance_id, created_at,
                &receipt, &receipt_size) == IVR_OK) {
            (void)send_application(provider, item->frame_message_id, receipt,
                                   receipt_size);
        }
    }
    if (decoded) iris_control_provider_command_clear(&command);
    free(receipt);
    atomic_fetch_sub_explicit(&provider->pending_ingress_bytes,
                              item->payload_size, memory_order_acq_rel);
    free(item);
}

static void on_message(void *context, const uint8_t *payload,
                       size_t payload_size) {
    iris_control_provider_t *provider = (iris_control_provider_t *)context;
    iris_control_work_item_t *item = NULL;
    size_t allocation_size;
    if (!provider) return;
    atomic_fetch_add_explicit(&provider->callback_count, 1u,
                              memory_order_acq_rel);
    if (!atomic_load_explicit(&provider->accepting, memory_order_acquire)) {
        goto done;
    } else if (!payload || payload_size == 0u ||
               payload_size > provider->maximum_frame_bytes) {
        goto done;
    } else if (!reserve_bytes(provider, payload_size)) {
        goto done;
    } else if (payload_size > SIZE_MAX - sizeof(*item)) {
        atomic_fetch_sub_explicit(&provider->pending_ingress_bytes,
                                  payload_size, memory_order_acq_rel);
    } else {
        allocation_size = sizeof(*item) + payload_size;
        item = (iris_control_work_item_t *)malloc(allocation_size);
        if (!item) {
            atomic_fetch_sub_explicit(&provider->pending_ingress_bytes,
                                      payload_size,
                                      memory_order_acq_rel);
        } else {
            item->owner = provider;
            item->frame_message_id = 0u;
            item->payload_size = payload_size;
            memcpy(item->payload, payload, payload_size);
            if (salts_threadpool_try_submit(provider->worker, process_command,
                                            item) != 0) {
                atomic_fetch_sub_explicit(&provider->pending_ingress_bytes,
                                          payload_size,
                                          memory_order_acq_rel);
                free(item);
            }
        }
    }
done:
    atomic_fetch_sub_explicit(&provider->callback_count, 1u,
                              memory_order_acq_rel);
}

static void on_connection(void *context, int connected) {
    iris_control_provider_t *provider = (iris_control_provider_t *)context;
    if (!provider) return;
    atomic_store_explicit(&provider->running, connected != 0,
                          memory_order_release);
    salts_mutex_lock(&provider->ack_mutex);
    salts_cond_broadcast(&provider->ack_changed);
    salts_mutex_unlock(&provider->ack_mutex);
}

iris_control_provider_t *iris_control_provider_create(
    const iris_control_provider_config_t *config) {
    iris_control_provider_t *provider = NULL;
    ivr_control_ws_client_config_t endpoint_config;
    cnet_tls_client_config tls;
    salts_threadpool_config_t worker_config;
    char uri[512];
    int written;
    if (iris_control_provider_config_validate(config) != SALTS_OK) return NULL;
    provider = (iris_control_provider_t *)calloc(1u, sizeof(*provider));
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
    iris_control_provider_observation_init(&provider->pending_observation);
    provider->provider_id = tstr_dup(config->provider_id);
    provider->provider_instance_id = tstr_dup(config->provider_instance_id);
    provider->iris_identity = tstr_dup(config->iris_identity);
    if (!provider->provider_id || !provider->provider_instance_id ||
        !provider->iris_identity) {
        iris_control_provider_destroy(provider);
        return NULL;
    }
    provider->maximum_frame_bytes = config->maximum_frame_bytes;
    provider->maximum_ingress_bytes = config->maximum_ingress_bytes;
    provider->dispatch = config->dispatch;
    provider->dispatch_context = config->dispatch_context;
    provider->now = config->now;
    provider->now_context = config->now_context;
    memset(&worker_config, 0, sizeof(worker_config));
    worker_config.num_threads = 1;
    worker_config.queue_capacity = config->maximum_ingress_messages;
    provider->worker = salts_threadpool_create_with_config(&worker_config);
    if (!provider->worker) {
        iris_control_provider_destroy(provider);
        return NULL;
    }

    written = snprintf(uri, sizeof(uri), "%s://%s:%u%s",
                       config->use_tls ? "wss" : "ws", config->host,
                       (unsigned)config->port, config->path);
    if (written <= 0 || (size_t)written >= sizeof(uri)) {
        iris_control_provider_destroy(provider);
        return NULL;
    }
    memset(&tls, 0, sizeof(tls));
    ivr_control_ws_client_config_init(&endpoint_config);
    endpoint_config.uri = uri;
    endpoint_config.identity = config->provider_instance_id;
    endpoint_config.maximum_message_bytes = config->maximum_frame_bytes;
    endpoint_config.maximum_queue_messages = config->send_queue_capacity;
    endpoint_config.maximum_queue_bytes = config->send_queue_bytes;
    endpoint_config.start_timeout_ms = config->start_timeout_ms;
    endpoint_config.reconnect_initial_ms = (uint32_t)config->reconnect_initial_ms;
    endpoint_config.reconnect_max_ms = (uint32_t)config->reconnect_max_ms;
    endpoint_config.on_message = on_message;
    endpoint_config.on_connection = on_connection;
    endpoint_config.callback_context = provider;
    if (config->use_tls) {
        tls.size = sizeof(tls);
        tls.ca_file = config->ca_file;
        tls.cert_file = config->certificate_file;
        tls.key_file = config->private_key_file;
        tls.key_password = nonempty(config->private_key_password)
                               ? config->private_key_password
                               : NULL;
        tls.server_name = config->server_name;
        endpoint_config.tls = &tls;
    }
    if (ivr_control_ws_client_create(&endpoint_config, &provider->endpoint) !=
            IVR_OK ||
        !provider->endpoint) {
        iris_control_provider_destroy(provider);
        return NULL;
    }
    return provider;
}

int iris_control_provider_start(iris_control_provider_t *provider) {
    ivr_status_t status;
    if (!provider || !provider->endpoint ||
        atomic_load_explicit(&provider->stopped, memory_order_acquire)) {
        return SALTS_EINVAL;
    }
    status = ivr_control_ws_client_start(provider->endpoint);
    if (status == IVR_OK) {
        atomic_store_explicit(&provider->running, 1, memory_order_release);
    }
    return status == IVR_OK ? SALTS_OK : SALTS_EIO;
}

int iris_control_provider_stop(iris_control_provider_t *provider) {
    unsigned attempt;
    int endpoint_status;
    if (!provider) return 0;
    if (atomic_load_explicit(&provider->stopped, memory_order_acquire)) return 0;
    atomic_store_explicit(&provider->accepting, 0, memory_order_release);
    endpoint_status = provider->endpoint
        ? ivr_control_ws_client_stop(provider->endpoint)
        : IVR_OK;
    atomic_store_explicit(&provider->running, 0, memory_order_release);
    if (endpoint_status != IVR_OK) return -1;
    salts_mutex_lock(&provider->ack_mutex);
    salts_cond_broadcast(&provider->ack_changed);
    salts_mutex_unlock(&provider->ack_mutex);
    salts_mutex_lock(&provider->delivery_mutex);
    salts_mutex_unlock(&provider->delivery_mutex);
    salts_mutex_lock(&provider->query_mutex);
    salts_mutex_unlock(&provider->query_mutex);
    salts_mutex_lock(&provider->offer_mutex);
    salts_mutex_unlock(&provider->offer_mutex);
    for (attempt = 0u; attempt < IRIS_CONTROL_CALLBACK_QUIESCE_ATTEMPTS;
         ++attempt) {
        if (atomic_load_explicit(&provider->callback_count,
                                 memory_order_acquire) == 0u) {
            break;
        }
        salts_sleep_ms(IRIS_CONTROL_CALLBACK_QUIESCE_STEP_MS);
    }
    if (atomic_load_explicit(&provider->callback_count,
                             memory_order_acquire) != 0u) {
        return -1;
    }
    if (provider->worker) {
        salts_threadpool_shutdown(provider->worker);
        salts_threadpool_wait(provider->worker);
        salts_threadpool_destroy(provider->worker);
        provider->worker = NULL;
    }
    atomic_store_explicit(&provider->stopped, 1, memory_order_release);
    return 0;
}

int iris_control_provider_destroy(iris_control_provider_t *provider) {
    if (!provider) return 0;
    if (iris_control_provider_stop(provider) != 0) return -1;
    if (provider->endpoint) {
        if (ivr_control_ws_client_destroy(provider->endpoint) != IVR_OK)
            return -1;
        provider->endpoint = NULL;
    }
    iris_control_provider_observation_clear(&provider->pending_observation);
    tstr_freep(&provider->provider_id);
    tstr_freep(&provider->provider_instance_id);
    tstr_freep(&provider->iris_identity);
    salts_cond_destroy(&provider->ack_changed);
    salts_mutex_destroy(&provider->ack_mutex);
    salts_mutex_destroy(&provider->delivery_mutex);
    salts_mutex_destroy(&provider->query_mutex);
    salts_mutex_destroy(&provider->offer_mutex);
    free(provider);
    return 0;
}

int iris_control_provider_running(const iris_control_provider_t *provider) {
    return provider &&
           atomic_load_explicit(&provider->running, memory_order_acquire);
}

ivr_status_t iris_control_provider_send_completion(
    iris_control_provider_t *provider,
    const iris_media_completion_t *completion,
    const ivr_media_command_result_t *result, const char *message_id,
    const char *completed_at, uint64_t completed_at_unix_ms,
    uint64_t ack_timeout_ms, iris_control_completion_ack_t *out_ack) {
    uint8_t *application = NULL;
    size_t application_size = 0u;
    uint64_t deadline;
    ivr_status_t status = IVR_ESTATE;
    if (!provider || !completion || !result || !message_id || !message_id[0] ||
        !completed_at || !completed_at[0] || completed_at_unix_ms == 0u ||
        ack_timeout_ms == 0u ||
        ack_timeout_ms > IRIS_CONTROL_MAX_ACK_TIMEOUT_MS || !out_ack) {
        return IVR_EINVAL;
    }
    memset(out_ack, 0, sizeof(*out_ack));
    if (!atomic_load_explicit(&provider->running, memory_order_acquire) ||
        atomic_load_explicit(&provider->stopped, memory_order_acquire)) {
        return IVR_ECLOSED;
    }
    if (iris_control_provider_encode_completion(
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
        free(application);
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
        free(application);
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
    free(application);
    return status;
}

ivr_status_t iris_control_provider_send_event(
    iris_control_provider_t *provider, const ivr_media_event_t *event,
    const char *message_id, const char *occurred_at, uint64_t ack_timeout_ms,
    iris_control_event_ack_t *out_ack) {
    uint8_t *application = NULL;
    size_t application_size = 0u;
    uint64_t deadline;
    ivr_status_t status = IVR_ESTATE;
    if (!provider || !event || !message_id || !message_id[0] ||
        !occurred_at || !occurred_at[0] || ack_timeout_ms == 0u ||
        ack_timeout_ms > IRIS_CONTROL_MAX_ACK_TIMEOUT_MS || !out_ack) {
        return IVR_EINVAL;
    }
    memset(out_ack, 0, sizeof(*out_ack));
    if (!atomic_load_explicit(&provider->running, memory_order_acquire) ||
        atomic_load_explicit(&provider->stopped, memory_order_acquire)) {
        return IVR_ECLOSED;
    }
    if (iris_control_provider_encode_event(
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
        free(application);
        return IVR_EBUSY;
    }
    provider->pending_event = *event;
    if (snprintf(provider->pending_event_message_id,
                 sizeof(provider->pending_event_message_id), "%s",
                 message_id) <= 0 ||
        strlen(message_id) >= sizeof(provider->pending_event_message_id)) {
        salts_mutex_unlock(&provider->ack_mutex);
        salts_mutex_unlock(&provider->delivery_mutex);
        free(application);
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
    free(application);
    return status;
}

static int copy_pending_text(char *out, size_t capacity, const char *value) {
    size_t length = value ? strlen(value) : 0u;
    if (!out || capacity == 0u || length == 0u || length >= capacity) return 0;
    memcpy(out, value, length + 1u);
    return 1;
}

ivr_status_t iris_control_provider_send_query(
    iris_control_provider_t *provider,
    const iris_control_provider_query_t *query, uint64_t timeout_ms,
    iris_control_provider_observation_t *out_observation) {
    uint8_t *application = NULL;
    size_t application_size = 0u;
    uint64_t deadline;
    ivr_status_t status = IVR_ESTATE;
    if (!provider || !query || !out_observation || !query->tenant_id ||
        !query->query_id || !query->query_type || !query->created_at ||
        !query->deadline_at || !query->payload_json || query->limit == 0u ||
        timeout_ms == 0u || timeout_ms > IRIS_CONTROL_MAX_ACK_TIMEOUT_MS) {
        return IVR_EINVAL;
    }
    iris_control_provider_observation_init(out_observation);
    if (!atomic_load_explicit(&provider->running, memory_order_acquire) ||
        atomic_load_explicit(&provider->stopped, memory_order_acquire)) {
        return IVR_ECLOSED;
    }
    if (iris_control_provider_encode_query(
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
        free(application);
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
    iris_control_provider_observation_clear(&provider->pending_observation);
    iris_control_provider_observation_init(&provider->pending_observation);
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
            iris_control_provider_observation_init(
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
        iris_control_provider_observation_clear(
            &provider->pending_observation);
        iris_control_provider_observation_init(&provider->pending_observation);
        salts_mutex_unlock(&provider->ack_mutex);
    }
    salts_mutex_unlock(&provider->query_mutex);
    free(application);
    return status;
}

ivr_status_t iris_control_provider_send_call_offer(
    iris_control_provider_t *provider,
    const iris_control_call_offer_t *offer, uint64_t timeout_ms,
    iris_control_session_bound_t *out_bound) {
    uint8_t *application = NULL;
    size_t application_size = 0u;
    uint64_t deadline;
    int send_status;
    ivr_status_t status = IVR_ESTATE;
    if (!provider || !offer || !out_bound || timeout_ms == 0u ||
        timeout_ms > IRIS_CONTROL_MAX_ACK_TIMEOUT_MS) {
        return IVR_EINVAL;
    }
    memset(out_bound, 0, sizeof(*out_bound));
    if (!atomic_load_explicit(&provider->running, memory_order_acquire) ||
        atomic_load_explicit(&provider->stopped, memory_order_acquire)) {
        return IVR_ECLOSED;
    }
    if (iris_control_provider_encode_call_offer(
            offer, provider->provider_id, provider->provider_instance_id,
            &application, &application_size) != IVR_OK) {
        return IVR_EINVAL;
    }

    salts_mutex_lock(&provider->offer_mutex);
    salts_mutex_lock(&provider->ack_mutex);
    if (provider->pending_offer_active) {
        salts_mutex_unlock(&provider->ack_mutex);
        salts_mutex_unlock(&provider->offer_mutex);
        free(application);
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
    free(application);
    return status;
}
