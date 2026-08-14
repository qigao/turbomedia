#include "iris_completion_dispatcher.h"

#include <turbo_http.h>
#include <turbo_parser.h>
#include <turbo_thread.h>
#include <turbo_uuid.h>
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
    char completion_event_id[TURBO_UUID_STRING_SIZE];
    char completion_occurred_at[40];
    uint64_t completed_at_ms;
    uint64_t delivery_token;
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
    turbo_mutex_t mutex;
    turbo_cond_t not_empty;
    turbo_cond_t drained;
    turbo_thread_t thread;
    turbo_http_t *client;
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
    turbo_mutex_lock(&dispatcher->mutex);
    dispatcher->stats.delivery_attempts_total++;
    if (retry) dispatcher->stats.retries_total++;
    if (fence_conflict) dispatcher->stats.fence_conflicts_total++;
    if (fence_refresh_failed) {
        dispatcher->stats.fence_refresh_failures_total++;
    }
    turbo_mutex_unlock(&dispatcher->mutex);
}

static void record_delivery_result(iris_completion_dispatcher_t *dispatcher,
                                   iris_dispatch_item_kind_t kind,
                                   int succeeded) {
    turbo_mutex_lock(&dispatcher->mutex);
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
    turbo_mutex_unlock(&dispatcher->mutex);
}

static void retry_wait(iris_completion_dispatcher_t *dispatcher,
                       uint64_t delay_ms) {
    while (delay_ms > 0u) {
        uint64_t deadline = atomic_load(&dispatcher->stop_deadline_ms);
        uint64_t now = turbo_monotonic_ms();
        uint64_t slice = delay_ms > 10u ? 10u : delay_ms;
        if (deadline) {
            if (now >= deadline) return;
            if (slice > deadline - now) slice = deadline - now;
        }
        if (slice == 0u) return;
        turbo_sleep_ms((uint32_t)slice);
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
    if (!value || !turbo_json_object_add_checked(object, name, value)) {
        turbo_free_json(&value);
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
    root = turbo_json_create_object();
    event = turbo_json_create_object();
    data = turbo_json_create_object();
    if (!root || !event || !data ||
        !add_json(root, "workerId", turbo_json_create_string(
                                        item->completion.iris_worker_id)) ||
        !add_json(root, "expectedDispatchEpoch", turbo_json_create_uint64(
                                                   item->completion.dispatch_epoch)) ||
        !add_json(root, "terminalStatus", turbo_json_create_string(terminal)) ||
        !add_json(root, "completedAtUnixMs",
                  turbo_json_create_uint64(item->completed_at_ms)) ||
        !add_json(event, "eventId", turbo_json_create_string(
                                      item->completion_event_id)) ||
        !add_json(event, "type", turbo_json_create_string(
                                   item->result.status_code == IVR_OK
                                       ? "provider.media.completed"
                                       : "provider.media.failed")) ||
        !add_json(event, "correlationId", turbo_json_create_string(
                                            item->completion.correlation_id)) ||
        !add_json(event, "occurredAt", turbo_json_create_string(occurred_at)) ||
        !add_json(data, "status", turbo_json_create_string(status)) ||
        !add_json(data, "mediaWorkerId", turbo_json_create_string(
                                           item->result.worker_id)) ||
        !add_json(data, "dialogId", turbo_json_create_string(
                                      item->result.dialog_id)) ||
        !add_json(data, "roomId", turbo_json_create_string(item->result.room_id)) ||
        !add_json(data, "callId", turbo_json_create_string(item->result.call_id)) ||
        !add_json(data, "callGeneration", turbo_json_create_uint64(
                                             item->result.call_generation)) ||
        !add_json(data, "operationGeneration", turbo_json_create_uint64(
                                                  item->result.operation_generation))) {
        goto cleanup;
    }
    if (item->result.error_code[0] &&
        !add_json(data, "errorCode",
                  turbo_json_create_string(item->result.error_code))) goto cleanup;
    if (item->result.error_message[0] &&
        !add_json(data, "errorMessage",
                  turbo_json_create_string(item->result.error_message))) goto cleanup;
    if (!add_json(event, "data", data)) goto cleanup;
    data = NULL;
    if (!add_json(root, "event", event)) goto cleanup;
    event = NULL;
    json = turbo_json_serialize(root, out_size);
cleanup:
    turbo_free_json(&data);
    turbo_free_json(&event);
    turbo_free_json(&root);
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
        if (turbo_parse_json((const uint8_t *)source->payload_json,
                             strlen(source->payload_json), &payload) != 0 ||
            !payload) {
            turbo_free_json(&payload);
            return NULL;
        }
    } else {
        payload = turbo_json_create_object();
    }
    root = turbo_json_create_object();
    data = turbo_json_create_object();
    if (!root || !data || !payload ||
        !add_json(data, "dialogId", turbo_json_create_string(source->dialog_id)) ||
        !add_json(data, "roomId", turbo_json_create_string(source->room_id)) ||
        !add_json(data, "callId", turbo_json_create_string(source->call_id)) ||
        !add_json(data, "callGeneration",
                  turbo_json_create_uint64(source->call_generation)) ||
        !add_json(data, "payload", payload)) {
        goto cleanup;
    }
    payload = NULL;
    if (source->input_id[0] &&
        !add_json(data, "inputId",
                  turbo_json_create_string(source->input_id))) {
        goto cleanup;
    }
    if (source->input_value[0] &&
        !add_json(data, "inputValue",
                  turbo_json_create_string(source->input_value))) {
        goto cleanup;
    }
    if (
        !add_json(root, "eventId", turbo_json_create_string(source->event_id)) ||
        !add_json(root, "type", turbo_json_create_string(source->event_type)) ||
        !add_json(root, "source", turbo_json_create_string(IRIS_PROVIDER_ID)) ||
        !add_json(root, "correlationId",
                  turbo_json_create_string(source->dialog_id)) ||
        !add_json(root, "occurredAt", turbo_json_create_string(occurred_at)) ||
        !add_json(root, "data", data)) {
        goto cleanup;
    }
    data = NULL;
    json = turbo_json_serialize(root, out_size);
cleanup:
    turbo_free_json(&payload);
    turbo_free_json(&data);
    turbo_free_json(&root);
    return json;
}

static int default_post(void *context, const char *url,
                        const char *authorization, const char *body,
                        size_t body_size) {
    iris_completion_dispatcher_t *dispatcher =
        (iris_completion_dispatcher_t *)context;
    const char *headers[] = {"Content-Type: application/json", authorization};
    http_response_t *response = turbo_http_request_sync(
        dispatcher->client, HTTP_POST, url, headers, 2, body, body_size);
    int status = response && response->error_code == HTTP_ERROR_NONE
                     ? response->status_code : 0;
    if (response) http_response_free(response);
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
    session = turbo_url_encode(session_id);
    if (!session) return -1;
    if (item->kind == IRIS_DISPATCH_COMPLETION) {
        command = turbo_url_encode(item->completion.command_id);
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
        if (stop_deadline && turbo_monotonic_ms() >= stop_deadline) break;
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
                turbo_json_serialize_free(body);
                break;
            }
            status = dispatcher->post(dispatcher->post_context, url,
                                      authorization, body, body_size);
            terminal_status = status;
            turbo_json_serialize_free(body);
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
    if (item->kind == IRIS_DISPATCH_COMPLETION) {
        if (succeeded || terminal_delivery_failure) {
            iris_media_bridge_release_completion(dispatcher->bridge,
                                                  item->completion.command_id);
        } else {
            iris_media_bridge_restore_completion(dispatcher->bridge,
                                                  item->completion.command_id);
            TLOG_ERROR("Iris command completion failed: command_id={}, session_id={}. "
                       "The correlation remains available for a repeated media result.",
                       item->completion.command_id,
                       item->completion.provider_session_id);
        }
    } else if (!succeeded) {
        TLOG_ERROR("Iris media event delivery failed: event_id={}, session_id={}. "
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
        turbo_mutex_lock(&dispatcher->mutex);
        while (dispatcher->count == 0u && dispatcher->running) {
            turbo_cond_wait(&dispatcher->not_empty, &dispatcher->mutex);
        }
        if (dispatcher->count == 0u && !dispatcher->running) {
            turbo_mutex_unlock(&dispatcher->mutex);
            break;
        }
        item = dispatcher->items[dispatcher->head];
        memset(&dispatcher->items[dispatcher->head], 0,
               sizeof(dispatcher->items[dispatcher->head]));
        dispatcher->head = (dispatcher->head + 1u) % dispatcher->capacity;
        dispatcher->count--;
        dispatcher->in_flight++;
        turbo_mutex_unlock(&dispatcher->mutex);
        process_item(dispatcher, &item);
        turbo_mutex_lock(&dispatcher->mutex);
        dispatcher->in_flight--;
        if (dispatcher->count == 0u && dispatcher->in_flight == 0u) {
            turbo_cond_broadcast(&dispatcher->drained);
        }
        turbo_mutex_unlock(&dispatcher->mutex);
    }
}

iris_completion_dispatcher_t *iris_completion_dispatcher_create(
    const iris_completion_dispatcher_config_t *config) {
    iris_completion_dispatcher_t *dispatcher;
    turbo_http_options_t options;
    const char *tls_ca_file;
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
    turbo_mutex_init(&dispatcher->mutex);
    turbo_cond_init(&dispatcher->not_empty);
    turbo_cond_init(&dispatcher->drained);
    if (turbo_http_options_init(&options, sizeof(options)) != TURBO_OK) {
        turbo_cond_destroy(&dispatcher->drained);
        turbo_cond_destroy(&dispatcher->not_empty);
        turbo_mutex_destroy(&dispatcher->mutex);
        goto fail;
    }
    options.transport = TURBO_HTTP_TRANSPORT_AUTO;
    options.follow_redirects = 0;
    options.timeout_ms = config->request_timeout_ms;
    if (!config->post && !config->deliver_completion) {
        if (turbo_http_create_sync(&options, &dispatcher->client) != TURBO_OK) {
            turbo_cond_destroy(&dispatcher->drained);
            turbo_cond_destroy(&dispatcher->not_empty);
            turbo_mutex_destroy(&dispatcher->mutex);
            goto fail;
        }
        /* Snapshot the provider trust anchor into the facade.  Relying on a
         * process-global TLS context makes reconnect behavior depend on which
         * subsystem initialized TLS first.  Explicit facade configuration is
         * deep-copied and inherited by every fresh H1/H2 connection. */
        tls_ca_file = getenv("TURBONET_TLS_CA_FILE");
        if (strncmp(dispatcher->base_url, "https://", 8u) == 0 &&
            tls_ca_file && tls_ca_file[0] != '\0') {
            turbo_tls_client_config_t tls_config;
            memset(&tls_config, 0, sizeof(tls_config));
            tls_config.verify_peer = 1;
            tls_config.ca_file = tls_ca_file;
            if (turbo_http_set_tls_config(dispatcher->client, &tls_config) !=
                TURBO_OK) {
                turbo_http_destroy(dispatcher->client);
                dispatcher->client = NULL;
                turbo_cond_destroy(&dispatcher->drained);
                turbo_cond_destroy(&dispatcher->not_empty);
                turbo_mutex_destroy(&dispatcher->mutex);
                goto fail;
            }
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
    turbo_mutex_lock(&dispatcher->mutex);
    dispatcher->accepting = 1;
    dispatcher->running = 1;
    turbo_mutex_unlock(&dispatcher->mutex);
    if (turbo_thread_create(&dispatcher->thread, dispatcher_thread,
                            dispatcher) != 0) {
        turbo_mutex_lock(&dispatcher->mutex);
        dispatcher->accepting = 0;
        dispatcher->running = 0;
        turbo_mutex_unlock(&dispatcher->mutex);
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
    drain_started = turbo_monotonic_ms();
    turbo_mutex_lock(&dispatcher->mutex);
    dispatcher->accepting = 0;
    deadline = turbo_monotonic_ms() + (uint64_t)dispatcher->drain_timeout_ms;
    atomic_store(&dispatcher->stop_deadline_ms, deadline);
    while ((dispatcher->count > 0u || dispatcher->in_flight > 0u) &&
           turbo_monotonic_ms() < deadline) {
        uint64_t remaining = deadline - turbo_monotonic_ms();
        (void)turbo_cond_timedwait(&dispatcher->drained, &dispatcher->mutex,
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
        turbo_mutex_unlock(&dispatcher->mutex);
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
        turbo_mutex_lock(&dispatcher->mutex);
    }
    dispatcher->running = 0;
    turbo_cond_broadcast(&dispatcher->not_empty);
    turbo_mutex_unlock(&dispatcher->mutex);
    turbo_thread_join(&dispatcher->thread);
    turbo_thread_destroy(&dispatcher->thread);
    dispatcher->thread_started = 0;
    drain_duration = turbo_monotonic_ms() - drain_started;
    turbo_mutex_lock(&dispatcher->mutex);
    dispatcher->stats.last_drain_duration_ms = drain_duration;
    if (drain_duration > dispatcher->stats.max_drain_duration_ms) {
        dispatcher->stats.max_drain_duration_ms = drain_duration;
    }
    turbo_mutex_unlock(&dispatcher->mutex);
}

void iris_completion_dispatcher_get_stats(
    iris_completion_dispatcher_t *dispatcher,
    iris_completion_dispatcher_stats_t *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    if (!dispatcher) return;
    turbo_mutex_lock(&dispatcher->mutex);
    *stats = dispatcher->stats;
    stats->queue_items = dispatcher->count;
    stats->queue_capacity = dispatcher->capacity;
    stats->in_flight = dispatcher->in_flight;
    turbo_mutex_unlock(&dispatcher->mutex);
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
    turbo_http_destroy(dispatcher->client);
    turbo_cond_destroy(&dispatcher->drained);
    turbo_cond_destroy(&dispatcher->not_empty);
    turbo_mutex_destroy(&dispatcher->mutex);
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
    turbo_mutex_lock(&dispatcher->mutex);
    if (!dispatcher->accepting) {
        dispatcher->stats.closed_rejections_total++;
        turbo_mutex_unlock(&dispatcher->mutex);
        return IVR_ECLOSED;
    }
    if (dispatcher->count == dispatcher->capacity) {
        dispatcher->stats.queue_full_total++;
        turbo_mutex_unlock(&dispatcher->mutex);
        return IVR_ENOSPC;
    }
    tail = (dispatcher->head + dispatcher->count) % dispatcher->capacity;
    dispatcher->items[tail] = *item;
    dispatcher->count++;
    dispatcher->stats.enqueued_total++;
    if (dispatcher->count > dispatcher->stats.queue_high_water) {
        dispatcher->stats.queue_high_water = dispatcher->count;
    }
    turbo_cond_signal(&dispatcher->not_empty);
    turbo_mutex_unlock(&dispatcher->mutex);
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
    item.result = *result;
    item.completed_at_ms = turbo_realtime_ms();
    {
        turbo_uuid_t uuid;
        if (item.completed_at_ms == 0u ||
            turbo_uuid_v4_generate(&uuid) != TURBO_OK ||
            turbo_uuid_format(&uuid, item.completion_event_id,
                              sizeof(item.completion_event_id)) != TURBO_OK ||
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
