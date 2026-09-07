#include "iris_completion_dispatcher.h"

#include <turbo_transport.h>
#include <json_parser.h>
#include <salts_thread.h>
#include <salts/clock.h>
#include <salts_uuid.h>
#include <turbo_crypto.h>
#include <tlog.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#define IRIS_PROVIDER_ID "turbomedia"
#define IRIS_COMPLETION_URL_CAPACITY 1024u
#define IRIS_AUTH_HEADER_CAPACITY 4608u

typedef enum iris_dispatch_item_kind_e {
    IRIS_DISPATCH_COMPLETION = 1,
    IRIS_DISPATCH_EVENT = 2
} iris_dispatch_item_kind_t;

typedef struct iris_dispatch_item_s {
    iris_dispatch_item_kind_t kind;
    iris_media_completion_t completion;
    ivr_media_command_result_t result;
    ivr_media_event_t event;
    char completion_event_id[SALTS_UUID_STRING_SIZE];
    char completion_occurred_at[40];
    uint64_t completed_at_ms;
    uint64_t delivery_token;
    int settle_media_bridge;
} iris_dispatch_item_t;

struct iris_completion_dispatcher_s {
    char *base_url;
    char *provider_token;
    iris_dispatch_item_t *items;
    size_t capacity;
    size_t head;
    size_t count;
    size_t in_flight;
    int accepting;
    int running;
    int thread_started;
    int retry_max_attempts;
    int retry_backoff_ms;
    int request_timeout_ms;
    int drain_timeout_ms;
    salts_mutex_t mutex;
    salts_cond_t not_empty;
    salts_cond_t drained;
    salts_thread_t thread;
    turbo_transport_t *client;
    iris_media_bridge_t *bridge;
    iris_completion_post_fn post;
    void *post_context;
    iris_completion_deliver_fn deliver_completion;
    iris_event_deliver_fn deliver_event;
    void *deliver_context;
    iris_event_delivery_result_fn event_delivery_result;
    void *event_delivery_context;
    atomic_uint_fast64_t stop_deadline_ms;
    iris_completion_dispatcher_stats_t stats;
};

static void record_delivery_attempt(iris_completion_dispatcher_t *dispatcher,
                                    int retry, int fence_conflict,
                                    int fence_refresh_failed) {
    salts_mutex_lock(&dispatcher->mutex);
    dispatcher->stats.delivery_attempts_total++;
    if (retry) dispatcher->stats.retries_total++;
    if (fence_conflict) dispatcher->stats.fence_conflicts_total++;
    if (fence_refresh_failed) {
        dispatcher->stats.fence_refresh_failures_total++;
    }
    salts_mutex_unlock(&dispatcher->mutex);
}

static void record_delivery_result(iris_completion_dispatcher_t *dispatcher,
                                   iris_dispatch_item_kind_t kind,
                                   int succeeded) {
    salts_mutex_lock(&dispatcher->mutex);
    if (kind == IRIS_DISPATCH_COMPLETION) {
        if (succeeded) {
            dispatcher->stats.completion_success_total++;
        } else {
            dispatcher->stats.completion_failure_total++;
        }
    } else if (succeeded) {
        dispatcher->stats.event_success_total++;
    } else {
        dispatcher->stats.event_failure_total++;
    }
    salts_mutex_unlock(&dispatcher->mutex);
}

static void retry_wait(iris_completion_dispatcher_t *dispatcher,
                       uint64_t delay_ms) {
    while (delay_ms > 0u) {
        uint64_t deadline = atomic_load(&dispatcher->stop_deadline_ms);
        uint64_t now = salts_monotonic_ms();
        uint64_t slice = delay_ms > 10u ? 10u : delay_ms;
        if (deadline) {
            if (now >= deadline) return;
            if (slice > deadline - now) slice = deadline - now;
        }
        if (slice == 0u) return;
        salts_sleep_ms((uint32_t)slice);
        delay_ms -= slice;
    }
}

static char *copy_string(const char *value) {
    size_t size;
    char *copy;
    if (!value) return NULL;
    size = strlen(value);
    copy = (char *)malloc(size + 1u);
    if (copy) memcpy(copy, value, size + 1u);
    return copy;
}

static char *encode_path_segment(const char *value) {
    static const char hex[] = "0123456789ABCDEF";
    const unsigned char *input = (const unsigned char *)value;
    size_t input_size;
    size_t output_size = 0u;
    char *encoded;
    if (!value) return NULL;
    input_size = strlen(value);
    if (input_size > (SIZE_MAX - 1u) / 3u) return NULL;
    encoded = (char *)malloc(input_size * 3u + 1u);
    if (!encoded) return NULL;
    for (size_t index = 0u; index < input_size; ++index) {
        unsigned char byte = input[index];
        int unreserved = (byte >= 'A' && byte <= 'Z') ||
                         (byte >= 'a' && byte <= 'z') ||
                         (byte >= '0' && byte <= '9') || byte == '-' ||
                         byte == '.' || byte == '_' || byte == '~';
        if (byte < 0x20u || byte == 0x7fu) {
            free(encoded);
            return NULL;
        }
        if (unreserved) {
            encoded[output_size++] = (char)byte;
        } else {
            encoded[output_size++] = '%';
            encoded[output_size++] = hex[byte >> 4u];
            encoded[output_size++] = hex[byte & 0x0fu];
        }
    }
    encoded[output_size] = '\0';
    return encoded;
}

static int format_rfc3339(uint64_t unix_ms, char *out, size_t capacity) {
    time_t seconds;
    struct tm value;
    int written;
    if (!out || capacity < 25u || unix_ms / UINT64_C(1000) > (uint64_t)INT64_MAX) {
        return -1;
    }
    seconds = (time_t)(unix_ms / UINT64_C(1000));
#ifdef _WIN32
    if (gmtime_s(&value, &seconds) != 0) return -1;
#else
    if (!gmtime_r(&seconds, &value)) return -1;
#endif
    written = snprintf(out, capacity, "%04d-%02d-%02dT%02d:%02d:%02d.%03lluZ",
                       value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
                       value.tm_hour, value.tm_min, value.tm_sec,
                       (unsigned long long)(unix_ms % UINT64_C(1000)));
    return written > 0 && (size_t)written < capacity ? 0 : -1;
}

static int add_json(json_value_t *object, const char *name,
                    json_value_t *value) {
    if (!value || !json_object_add_checked(object, name, value)) {
        json_free(value);
        value = NULL;
        return 0;
    }
    return 1;
}

static char *serialize_completion(const iris_dispatch_item_t *item,
                                  size_t *out_size) {
    json_value_t *root = NULL;
    json_value_t *event = NULL;
    json_value_t *data = NULL;
    char occurred_at[40];
    char *json = NULL;
    const char *terminal = item->result.status_code == IVR_OK
                               ? "succeeded" : "failed";
    const char *status = item->result.status_code == IVR_OK
                             ? "completed" : "failed";
    if (item->completed_at_ms == 0u ||
        item->completion_event_id[0] == '\0' ||
        item->completion_occurred_at[0] == '\0') {
        return NULL;
    }
    memcpy(occurred_at, item->completion_occurred_at,
           sizeof(occurred_at));
    root = json_create_object();
    event = json_create_object();
    data = json_create_object();
    if (!root || !event || !data ||
        !add_json(root, "workerId", json_create_string(
                                        item->completion.iris_worker_id)) ||
        !add_json(root, "expectedDispatchEpoch", json_create_uint64(
                                                   item->completion.dispatch_epoch)) ||
        !add_json(root, "terminalStatus", json_create_string(terminal)) ||
        !add_json(root, "completedAtUnixMs",
                  json_create_uint64(item->completed_at_ms)) ||
        !add_json(event, "eventId", json_create_string(
                                      item->completion_event_id)) ||
        !add_json(event, "type", json_create_string(
                                   item->result.status_code == IVR_OK
                                       ? "provider.media.completed"
                                       : "provider.media.failed")) ||
        !add_json(event, "correlationId", json_create_string(
                                            item->completion.correlation_id)) ||
        !add_json(event, "occurredAt", json_create_string(occurred_at)) ||
        !add_json(data, "status", json_create_string(status)) ||
        !add_json(data, "mediaWorkerId", json_create_string(
                                           item->result.worker_id)) ||
        !add_json(data, "dialogId", json_create_string(
                                      item->result.dialog_id)) ||
        !add_json(data, "roomId", json_create_string(item->result.room_id)) ||
        !add_json(data, "callId", json_create_string(item->result.call_id)) ||
        !add_json(data, "callGeneration", json_create_uint64(
                                             item->result.call_generation)) ||
        !add_json(data, "operationGeneration", json_create_uint64(
                                                  item->result.operation_generation))) {
        goto cleanup;
    }
    if (item->result.error_code[0] &&
        !add_json(data, "errorCode",
                  json_create_string(item->result.error_code))) goto cleanup;
    if (item->result.error_message[0] &&
        !add_json(data, "errorMessage",
                  json_create_string(item->result.error_message))) goto cleanup;
    if (!add_json(event, "data", data)) goto cleanup;
    data = NULL;
    if (!add_json(root, "event", event)) goto cleanup;
    event = NULL;
    json = json_serialize(root, out_size);
cleanup:
    json_free(data);
    data = NULL;
    json_free(event);
    event = NULL;
    json_free(root);
    root = NULL;
    return json;
}

static char *serialize_event(const ivr_media_event_t *source, size_t *out_size) {
    json_value_t *root = NULL;
    json_value_t *data = NULL;
    json_value_t *payload = NULL;
    char occurred_at[40];
    char *json = NULL;
    if (format_rfc3339(source->occurred_at_ms, occurred_at,
                       sizeof(occurred_at)) != 0) return NULL;
    if (source->payload_json[0]) {
        if (((payload = json_parse((const char *)((const uint8_t *)source->payload_json), strlen(source->payload_json))) ? 0 : -1) != 0 ||
            !payload) {
            json_free(payload);
            payload = NULL;
            return NULL;
        }
    } else {
        payload = json_create_object();
    }
    root = json_create_object();
    data = json_create_object();
    if (!root || !data || !payload ||
        !add_json(data, "dialogId", json_create_string(source->dialog_id)) ||
        !add_json(data, "roomId", json_create_string(source->room_id)) ||
        !add_json(data, "callId", json_create_string(source->call_id)) ||
        !add_json(data, "callGeneration",
                  json_create_uint64(source->call_generation)) ||
        !add_json(data, "payload", payload)) {
        goto cleanup;
    }
    payload = NULL;
    if (source->input_id[0] &&
        !add_json(data, "inputId",
                  json_create_string(source->input_id))) {
        goto cleanup;
    }
    if (source->input_value[0] &&
        !add_json(data, "inputValue",
                  json_create_string(source->input_value))) {
        goto cleanup;
    }
    if (
        !add_json(root, "eventId", json_create_string(source->event_id)) ||
        !add_json(root, "type", json_create_string(source->event_type)) ||
        !add_json(root, "source", json_create_string(IRIS_PROVIDER_ID)) ||
        !add_json(root, "correlationId",
                  json_create_string(source->dialog_id)) ||
        !add_json(root, "occurredAt", json_create_string(occurred_at)) ||
        !add_json(root, "data", data)) {
        goto cleanup;
    }
    data = NULL;
    json = json_serialize(root, out_size);
cleanup:
    json_free(payload);
    payload = NULL;
    json_free(data);
    data = NULL;
    json_free(root);
    root = NULL;
    return json;
}

static int default_post(void *context, const char *url,
                        const char *authorization, const char *body,
                        size_t body_size) {
    iris_completion_dispatcher_t *dispatcher =
        (iris_completion_dispatcher_t *)context;
    const char *headers[] = {"Content-Type", "application/json"};
    size_t base_url_size = strlen(dispatcher->base_url);
    const char *path;
    chttp_response *response;
    int status;
    (void)authorization;
    if (!url || strncmp(url, dispatcher->base_url, base_url_size) != 0 ||
        url[base_url_size] != '/')
        return 0;
    path = url + base_url_size;
    response = turbo_transport_http_request(
        dispatcher->client, TURBO_HTTP_POST, path,
        (const uint8_t *)body, body_size, headers, 2);
    status = response ? (int)response->status_code : 0;
    if (response) {
        chttp_response_destroy(response);
        free(response);
    }
    return status;
}

static int build_request(const iris_completion_dispatcher_t *dispatcher,
                         iris_dispatch_item_t *item, char *url,
                         size_t url_capacity, char **out_body,
                         size_t *out_body_size) {
    char *session = NULL;
    char *command = NULL;
    int written;
    const char *session_id = item->kind == IRIS_DISPATCH_COMPLETION
                                 ? item->completion.provider_session_id
                                 : item->event.provider_session_id;
    session = encode_path_segment(session_id);
    if (!session) return -1;
    if (item->kind == IRIS_DISPATCH_COMPLETION) {
        command = encode_path_segment(item->completion.command_id);
        if (!command) {
            free(session);
            return -1;
        }
        written = snprintf(url, url_capacity,
                           "%s/v1/sessions/%s/commands/%s/completions",
                           dispatcher->base_url, session, command);
        *out_body = serialize_completion(item, out_body_size);
    } else {
        written = snprintf(url, url_capacity, "%s/v1/sessions/%s/events",
                           dispatcher->base_url, session);
        *out_body = serialize_event(&item->event, out_body_size);
    }
    free(command);
    free(session);
    return written > 0 && (size_t)written < url_capacity && *out_body ? 0 : -1;
}

static void process_item(iris_completion_dispatcher_t *dispatcher,
                         iris_dispatch_item_t *item) {
    char url[IRIS_COMPLETION_URL_CAPACITY];
    char authorization[IRIS_AUTH_HEADER_CAPACITY];
    int auth_size = snprintf(authorization, sizeof(authorization),
                             "Authorization: Bearer %s",
                             dispatcher->provider_token);
    int succeeded = 0;
    int terminal_delivery_failure = 0;
    int terminal_status = 0;
    for (int attempt = 1; attempt <= dispatcher->retry_max_attempts; ++attempt) {
        char *body = NULL;
        size_t body_size = 0u;
        int status;
        uint64_t stop_deadline = atomic_load(&dispatcher->stop_deadline_ms);
        if (stop_deadline && salts_monotonic_ms() >= stop_deadline) break;
        if (dispatcher->deliver_completion) {
            if (item->kind == IRIS_DISPATCH_COMPLETION) {
                status = dispatcher->deliver_completion(
                    dispatcher->deliver_context, &item->completion,
                    &item->result, item->completion_event_id,
                    item->completion_occurred_at, item->completed_at_ms,
                    (uint64_t)dispatcher->request_timeout_ms);
            } else {
                char occurred_at[40];
                status = format_rfc3339(item->event.occurred_at_ms,
                                        occurred_at,
                                        sizeof(occurred_at)) == 0
                             ? dispatcher->deliver_event(
                                   dispatcher->deliver_context, &item->event,
                                   item->event.event_id, occurred_at,
                                   (uint64_t)dispatcher->request_timeout_ms)
                             : IVR_ESTATE;
            }
            terminal_status = status == IVR_OK ? 200 : 0;
            if (status == IVR_OK) {
                record_delivery_attempt(dispatcher, 0, 0, 0);
                succeeded = 1;
                break;
            }
            if (status == IVR_ESTALE || status == IVR_EAUTH) {
                terminal_delivery_failure = 1;
                record_delivery_attempt(dispatcher, 0,
                                        status == IVR_ESTALE, 0);
                break;
            }
            record_delivery_attempt(
                dispatcher, attempt < dispatcher->retry_max_attempts, 0, 0);
        } else {
            if (auth_size <= 0 ||
                (size_t)auth_size >= sizeof(authorization) ||
                build_request(dispatcher, item, url, sizeof(url), &body,
                              &body_size) != 0) {
                json_serialize_free(body);
                break;
            }
            status = dispatcher->post(dispatcher->post_context, url,
                                      authorization, body, body_size);
            terminal_status = status;
            json_serialize_free(body);
            body = NULL;
            if (status >= 200 && status < 300) {
                record_delivery_attempt(dispatcher, 0, 0, 0);
                succeeded = 1;
                break;
            }
            if (status == 409 && item->kind == IRIS_DISPATCH_COMPLETION) {
                int refresh_failed = iris_media_bridge_refresh_completion(
                                         dispatcher->bridge,
                                         item->completion.command_id,
                                         &item->completion) != IVR_OK;
                record_delivery_attempt(
                    dispatcher, attempt < dispatcher->retry_max_attempts, 1,
                    refresh_failed);
            } else if (status >= 400 && status < 500 && status != 408 &&
                       status != 429) {
                record_delivery_attempt(dispatcher, 0, 0, 0);
                break;
            } else {
                record_delivery_attempt(
                    dispatcher, attempt < dispatcher->retry_max_attempts, 0,
                    0);
            }
        }
        if (attempt < dispatcher->retry_max_attempts) {
            uint64_t delay = (uint64_t)dispatcher->retry_backoff_ms;
            unsigned int shift = (unsigned int)(attempt - 1);
            if (shift < 8u) delay <<= shift;
            if (delay > 60000u) delay = 60000u;
            retry_wait(dispatcher, delay);
        }
    }
    record_delivery_result(dispatcher, item->kind, succeeded);
    if (item->kind == IRIS_DISPATCH_COMPLETION &&
        item->settle_media_bridge) {
        if (succeeded || terminal_delivery_failure) {
            iris_media_bridge_release_completion(dispatcher->bridge,
                                                  item->completion.command_id);
        } else {
            iris_media_bridge_restore_completion(dispatcher->bridge,
                                                  item->completion.command_id);
            TLOG_ERRORF("Iris command completion failed: command_id={}, session_id={}. "
                       "The correlation remains available for a repeated media result.",
                       item->completion.command_id,
                       item->completion.provider_session_id);
        }
    } else if (!succeeded) {
        TLOG_ERRORF("Iris media event delivery failed: event_id={}, session_id={}. "
                   "Check Iris availability and provider credentials.",
                   item->event.event_id, item->event.provider_session_id);
    }
    if (item->kind == IRIS_DISPATCH_EVENT &&
        dispatcher->event_delivery_result) {
        dispatcher->event_delivery_result(
            dispatcher->event_delivery_context, &item->event,
            item->delivery_token,
            succeeded ? IRIS_EVENT_DELIVERY_SUCCEEDED
                      : IRIS_EVENT_DELIVERY_FAILED,
            terminal_status);
    }
}

static void dispatcher_thread(void *context) {
    iris_completion_dispatcher_t *dispatcher =
        (iris_completion_dispatcher_t *)context;
    for (;;) {
        iris_dispatch_item_t item;
        salts_mutex_lock(&dispatcher->mutex);
        while (dispatcher->count == 0u && dispatcher->running) {
            salts_cond_wait(&dispatcher->not_empty, &dispatcher->mutex);
        }
        if (dispatcher->count == 0u && !dispatcher->running) {
            salts_mutex_unlock(&dispatcher->mutex);
            break;
        }
        item = dispatcher->items[dispatcher->head];
        memset(&dispatcher->items[dispatcher->head], 0,
               sizeof(dispatcher->items[dispatcher->head]));
        dispatcher->head = (dispatcher->head + 1u) % dispatcher->capacity;
        dispatcher->count--;
        dispatcher->in_flight++;
        salts_mutex_unlock(&dispatcher->mutex);
        process_item(dispatcher, &item);
        salts_mutex_lock(&dispatcher->mutex);
        dispatcher->in_flight--;
        if (dispatcher->count == 0u && dispatcher->in_flight == 0u) {
            salts_cond_broadcast(&dispatcher->drained);
        }
        salts_mutex_unlock(&dispatcher->mutex);
    }
}

iris_completion_dispatcher_t *iris_completion_dispatcher_create(
    const iris_completion_dispatcher_config_t *config) {
    iris_completion_dispatcher_t *dispatcher;
    turbo_transport_config_t http_config = {0};
    cnet_tls_client_config tls_config = {0};
    const char *tls_ca_file;
    const char *tls_ca_path;
    if (!config ||
        ((!config->deliver_completion || !config->deliver_event) &&
         (!config->base_url || !config->provider_token)) ||
        ((config->deliver_completion || config->deliver_event) &&
         (!config->deliver_completion || !config->deliver_event)) ||
        !config->queue_capacity || !config->bridge ||
        config->retry_max_attempts < 1 || config->retry_backoff_ms < 1 ||
        config->request_timeout_ms < 1 || config->drain_timeout_ms < 1 ||
        config->queue_capacity > SIZE_MAX / sizeof(iris_dispatch_item_t)) {
        return NULL;
    }
    dispatcher = (iris_completion_dispatcher_t *)calloc(1, sizeof(*dispatcher));
    if (!dispatcher) return NULL;
    dispatcher->base_url = copy_string(config->base_url ? config->base_url : "");
    dispatcher->provider_token =
        copy_string(config->provider_token ? config->provider_token : "");
    dispatcher->items = (iris_dispatch_item_t *)calloc(
        config->queue_capacity, sizeof(*dispatcher->items));
    if (!dispatcher->base_url || !dispatcher->provider_token ||
        !dispatcher->items) goto fail;
    while (strlen(dispatcher->base_url) > 0u &&
           dispatcher->base_url[strlen(dispatcher->base_url) - 1u] == '/') {
        dispatcher->base_url[strlen(dispatcher->base_url) - 1u] = '\0';
    }
    dispatcher->capacity = config->queue_capacity;
    dispatcher->retry_max_attempts = config->retry_max_attempts;
    dispatcher->retry_backoff_ms = config->retry_backoff_ms;
    dispatcher->request_timeout_ms = config->request_timeout_ms;
    dispatcher->drain_timeout_ms = config->drain_timeout_ms;
    dispatcher->bridge = config->bridge;
    dispatcher->post = config->post ? config->post : default_post;
    dispatcher->post_context = config->post ? config->post_context : dispatcher;
    dispatcher->deliver_completion = config->deliver_completion;
    dispatcher->deliver_event = config->deliver_event;
    dispatcher->deliver_context = config->deliver_context;
    dispatcher->event_delivery_result = config->event_delivery_result;
    dispatcher->event_delivery_context = config->event_delivery_context;
    atomic_init(&dispatcher->stop_deadline_ms, 0u);
    salts_mutex_init(&dispatcher->mutex);
    salts_cond_init(&dispatcher->not_empty);
    salts_cond_init(&dispatcher->drained);
    if (!config->post && !config->deliver_completion) {
        if (turbo_transport_parse_url(dispatcher->base_url, &http_config) != 0 ||
            http_config.type != TURBO_TRANSPORT_HTTP) {
            free((void *)http_config.host);
            free((void *)http_config.path);
            salts_cond_destroy(&dispatcher->drained);
            salts_cond_destroy(&dispatcher->not_empty);
            salts_mutex_destroy(&dispatcher->mutex);
            goto fail;
        }
        http_config.connect_timeout_ms = config->request_timeout_ms;
        http_config.read_timeout_ms = config->request_timeout_ms;
        http_config.write_timeout_ms = config->request_timeout_ms;
        http_config.user_agent = "TurboMediaIrisDispatcher/1";
        http_config.auth_token = dispatcher->provider_token;
        if (http_config.use_tls) {
            tls_ca_file = getenv("SALTS_TLS_CA_FILE");
            tls_ca_path = getenv("SALTS_TLS_CA_PATH");
            tls_config.size = sizeof(tls_config);
            tls_config.ca_file = tls_ca_file;
            tls_config.ca_path = tls_ca_path;
            http_config.tls = &tls_config;
        }
        dispatcher->client = turbo_transport_create(&http_config);
        free((void *)http_config.host);
        free((void *)http_config.path);
        if (!dispatcher->client) {
            salts_cond_destroy(&dispatcher->drained);
            salts_cond_destroy(&dispatcher->not_empty);
            salts_mutex_destroy(&dispatcher->mutex);
            goto fail;
        }
    }
    return dispatcher;
fail:
    free(dispatcher->items);
    free(dispatcher->provider_token);
    free(dispatcher->base_url);
    free(dispatcher);
    return NULL;
}

int iris_completion_dispatcher_start(iris_completion_dispatcher_t *dispatcher) {
    if (!dispatcher || dispatcher->thread_started) return -1;
    atomic_store(&dispatcher->stop_deadline_ms, 0u);
    salts_mutex_lock(&dispatcher->mutex);
    dispatcher->accepting = 1;
    dispatcher->running = 1;
    salts_mutex_unlock(&dispatcher->mutex);
    if (salts_thread_create(&dispatcher->thread, dispatcher_thread,
                            dispatcher) != 0) {
        salts_mutex_lock(&dispatcher->mutex);
        dispatcher->accepting = 0;
        dispatcher->running = 0;
        salts_mutex_unlock(&dispatcher->mutex);
        return -1;
    }
    dispatcher->thread_started = 1;
    return 0;
}

void iris_completion_dispatcher_stop(iris_completion_dispatcher_t *dispatcher) {
    uint64_t deadline;
    uint64_t drain_started;
    uint64_t drain_duration;
    if (!dispatcher || !dispatcher->thread_started) return;
    drain_started = salts_monotonic_ms();
    salts_mutex_lock(&dispatcher->mutex);
    dispatcher->accepting = 0;
    deadline = salts_monotonic_ms() + (uint64_t)dispatcher->drain_timeout_ms;
    atomic_store(&dispatcher->stop_deadline_ms, deadline);
    while ((dispatcher->count > 0u || dispatcher->in_flight > 0u) &&
           salts_monotonic_ms() < deadline) {
        uint64_t remaining = deadline - salts_monotonic_ms();
        (void)salts_cond_timedwait(&dispatcher->drained, &dispatcher->mutex,
                                  remaining * UINT64_C(1000000));
    }
    while (dispatcher->count > 0u) {
        iris_dispatch_item_t item = dispatcher->items[dispatcher->head];
        memset(&dispatcher->items[dispatcher->head], 0,
               sizeof(dispatcher->items[dispatcher->head]));
        dispatcher->head = (dispatcher->head + 1u) % dispatcher->capacity;
        dispatcher->count--;
        if (item.kind == IRIS_DISPATCH_COMPLETION) {
            dispatcher->stats.shutdown_restored_completions_total++;
        } else if (item.kind == IRIS_DISPATCH_EVENT) {
            dispatcher->stats.shutdown_dropped_events_total++;
        }
        salts_mutex_unlock(&dispatcher->mutex);
        if (item.kind == IRIS_DISPATCH_COMPLETION) {
            iris_media_bridge_restore_completion(
                dispatcher->bridge, item.completion.command_id);
        } else if (item.kind == IRIS_DISPATCH_EVENT) {
            if (dispatcher->event_delivery_result) {
                dispatcher->event_delivery_result(
                    dispatcher->event_delivery_context, &item.event,
                    item.delivery_token, IRIS_EVENT_DELIVERY_ABANDONED, 0);
            }
        }
        salts_mutex_lock(&dispatcher->mutex);
    }
    dispatcher->running = 0;
    salts_cond_broadcast(&dispatcher->not_empty);
    salts_mutex_unlock(&dispatcher->mutex);
    salts_thread_join(&dispatcher->thread);
    salts_thread_destroy(&dispatcher->thread);
    dispatcher->thread_started = 0;
    drain_duration = salts_monotonic_ms() - drain_started;
    salts_mutex_lock(&dispatcher->mutex);
    dispatcher->stats.last_drain_duration_ms = drain_duration;
    if (drain_duration > dispatcher->stats.max_drain_duration_ms) {
        dispatcher->stats.max_drain_duration_ms = drain_duration;
    }
    salts_mutex_unlock(&dispatcher->mutex);
}

void iris_completion_dispatcher_get_stats(
    iris_completion_dispatcher_t *dispatcher,
    iris_completion_dispatcher_stats_t *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    if (!dispatcher) return;
    salts_mutex_lock(&dispatcher->mutex);
    *stats = dispatcher->stats;
    stats->queue_items = dispatcher->count;
    stats->queue_capacity = dispatcher->capacity;
    stats->in_flight = dispatcher->in_flight;
    salts_mutex_unlock(&dispatcher->mutex);
}

int iris_completion_dispatcher_set_event_delivery_observer(
    iris_completion_dispatcher_t *dispatcher,
    iris_event_delivery_result_fn observer, void *observer_context) {
    if (!dispatcher || dispatcher->thread_started) return -1;
    dispatcher->event_delivery_result = observer;
    dispatcher->event_delivery_context = observer_context;
    return 0;
}

void iris_completion_dispatcher_destroy(iris_completion_dispatcher_t *dispatcher) {
    if (!dispatcher) return;
    iris_completion_dispatcher_stop(dispatcher);
    turbo_transport_destroy(dispatcher->client);
    salts_cond_destroy(&dispatcher->drained);
    salts_cond_destroy(&dispatcher->not_empty);
    salts_mutex_destroy(&dispatcher->mutex);
    free(dispatcher->items);
    if (dispatcher->provider_token) {
        turbo_crypto_wipe(dispatcher->provider_token,
                          strlen(dispatcher->provider_token));
        free(dispatcher->provider_token);
    }
    free(dispatcher->base_url);
    free(dispatcher);
}

static ivr_status_t enqueue(iris_completion_dispatcher_t *dispatcher,
                            const iris_dispatch_item_t *item) {
    size_t tail;
    if (!dispatcher || !item) return IVR_EINVAL;
    salts_mutex_lock(&dispatcher->mutex);
    if (!dispatcher->accepting) {
        dispatcher->stats.closed_rejections_total++;
        salts_mutex_unlock(&dispatcher->mutex);
        return IVR_ECLOSED;
    }
    if (dispatcher->count == dispatcher->capacity) {
        dispatcher->stats.queue_full_total++;
        salts_mutex_unlock(&dispatcher->mutex);
        return IVR_ENOSPC;
    }
    tail = (dispatcher->head + dispatcher->count) % dispatcher->capacity;
    dispatcher->items[tail] = *item;
    dispatcher->count++;
    dispatcher->stats.enqueued_total++;
    if (dispatcher->count > dispatcher->stats.queue_high_water) {
        dispatcher->stats.queue_high_water = dispatcher->count;
    }
    salts_cond_signal(&dispatcher->not_empty);
    salts_mutex_unlock(&dispatcher->mutex);
    return IVR_OK;
}

ivr_status_t iris_completion_dispatcher_on_media_result(
    void *context, const ivr_media_command_result_t *result) {
    iris_completion_dispatcher_t *dispatcher =
        (iris_completion_dispatcher_t *)context;
    iris_dispatch_item_t item;
    ivr_status_t status;
    if (!dispatcher || !result) return IVR_EINVAL;
    memset(&item, 0, sizeof(item));
    item.kind = IRIS_DISPATCH_COMPLETION;
    item.settle_media_bridge = 1;
    item.result = *result;
    item.completed_at_ms = salts_realtime_ms();
    {
        salts_uuid_t uuid;
        if (item.completed_at_ms == 0u ||
            salts_uuid_v4_generate(&uuid) != SALTS_OK ||
            salts_uuid_format(&uuid, item.completion_event_id,
                              sizeof(item.completion_event_id)) != SALTS_OK ||
            format_rfc3339(item.completed_at_ms,
                           item.completion_occurred_at,
                           sizeof(item.completion_occurred_at)) != 0) {
            return IVR_ESTATE;
        }
    }
    status = iris_media_bridge_claim_completion(dispatcher->bridge, result,
                                                &item.completion);
    if (status != IVR_OK || item.completion.command_id[0] == '\0') return status;
    status = enqueue(dispatcher, &item);
    if (status != IVR_OK) {
        iris_media_bridge_restore_completion(dispatcher->bridge,
                                              item.completion.command_id);
    }
    return status;
}

ivr_status_t iris_completion_dispatcher_enqueue_terminal(
    iris_completion_dispatcher_t *dispatcher,
    const iris_media_completion_t *completion,
    const ivr_media_command_result_t *result, const char *stable_event_id) {
    iris_dispatch_item_t item;
    if (!dispatcher || !completion || !result || !stable_event_id ||
        !stable_event_id[0] ||
        strlen(stable_event_id) >= sizeof(item.completion_event_id) ||
        !completion->command_id[0] || !completion->tenant_id[0] ||
        !completion->provider_session_id[0] ||
        !completion->iris_worker_id[0] || completion->dispatch_epoch == 0u ||
        !completion->terminal_status[0] || !completion->event_type[0] ||
        !completion->result_json[0]) {
        return IVR_EINVAL;
    }
    memset(&item, 0, sizeof(item));
    item.kind = IRIS_DISPATCH_COMPLETION;
    item.completion = *completion;
    item.result = *result;
    item.completed_at_ms = salts_realtime_ms();
    if (item.completed_at_ms == 0u ||
        snprintf(item.completion_event_id,
                 sizeof(item.completion_event_id), "%s", stable_event_id) <= 0 ||
        format_rfc3339(item.completed_at_ms, item.completion_occurred_at,
                       sizeof(item.completion_occurred_at)) != 0) {
        return IVR_ESTATE;
    }
    return enqueue(dispatcher, &item);
}

ivr_status_t iris_completion_dispatcher_on_media_event(
    void *context, const ivr_media_event_t *event) {
    return iris_completion_dispatcher_enqueue_event(
        (iris_completion_dispatcher_t *)context, event, 0u);
}

ivr_status_t iris_completion_dispatcher_enqueue_event(
    iris_completion_dispatcher_t *dispatcher, const ivr_media_event_t *event,
    uint64_t delivery_token) {
    iris_dispatch_item_t item;
    if (!dispatcher || !event) return IVR_EINVAL;
    memset(&item, 0, sizeof(item));
    item.kind = IRIS_DISPATCH_EVENT;
    item.event = *event;
    item.delivery_token = delivery_token;
    return enqueue(dispatcher, &item);
}
