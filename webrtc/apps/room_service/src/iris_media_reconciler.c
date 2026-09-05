#include "iris_media_reconciler.h"

#include <turbo_crypto.h>
#include <json_parser.h>
#include <salts_thread.h>
#include <salts_uuid.h>

#include <openssl/digest.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IRIS_RECONCILE_IDLE_POLL_MS 25u
#define IRIS_RECONCILE_EXPECTED_QUERY_TYPE "expected_media_resources"
#define IRIS_RECONCILE_EXPECTED_QUERY_PAYLOAD "{\"schemaVersion\":1}"

static int make_uuid(char *out, size_t capacity);

struct iris_media_reconciler_s {
    iris_flowmq_provider_t *provider;
    iris_expected_media_resource_t *expected;
    unsigned char *matched;
    ivr_fmq_worker_snapshot_t *workers;
    ivr_worker_inventory_envelope_t *inventory_queue;
    size_t resource_capacity;
    uint32_t worker_capacity;
    uint32_t inventory_queue_capacity;
    uint32_t inventory_head;
    uint32_t inventory_count;
    uint32_t retry_max_attempts;
    uint32_t retry_backoff_ms;
    uint32_t request_timeout_ms;
    uint32_t drain_timeout_ms;
    uint32_t inventory_page_size;
    uint64_t close_deadline_ms;
    iris_event_outbox_t *event_outbox;
    ivr_fmq_adapter_t *adapter;
    iris_media_reconciler_ops_t ops;
    int injected_ops;
    salts_mutex_t mutex;
    salts_cond_t wake;
    salts_thread_t thread;
    atomic_int running;
    int thread_started;
    int intake_closed;
    iris_media_reconciler_stats_t stats;
};

static void set_state(iris_media_reconciler_t *reconciler,
                      iris_media_reconcile_state_t state) {
    salts_mutex_lock(&reconciler->mutex);
    if (state == IRIS_MEDIA_RECONCILE_DRAINING ||
        reconciler->stats.state != IRIS_MEDIA_RECONCILE_DRAINING) {
        reconciler->stats.state = state;
    }
    salts_mutex_unlock(&reconciler->mutex);
}

static int copy_json_string(const json_value_t *object, const char *key,
                            char *out, size_t capacity, int required) {
    const char *value = json_get_string(object, key);
    size_t size = value ? strlen(value) : 0u;
    if (!out || capacity == 0u || size >= capacity ||
        (required && size == 0u)) {
        return 0;
    }
    if (size) memcpy(out, value, size);
    out[size] = '\0';
    return 1;
}

static int json_u64(const json_value_t *object, const char *key,
                    uint64_t *out) {
    const json_value_t *value;
    const char *text;
    size_t size = 0;
    char buffer[32];
    char *end = NULL;
    unsigned long long parsed;
    if (!object || !key || !out) return 0;
    value = json_object_get(object, key);
    if (!value || json_type(value) != JSON_NUMBER) return 0;
    text = json_number_text(value, &size);
    if (!text || size == 0u || size >= sizeof(buffer)) return 0;
    memcpy(buffer, text, size);
    buffer[size] = '\0';
    if (buffer[0] == '-' || strchr(buffer, '.') || strchr(buffer, 'e') ||
        strchr(buffer, 'E')) {
        return 0;
    }
    errno = 0;
    parsed = strtoull(buffer, &end, 10);
    if (errno == ERANGE || !end || *end != '\0') return 0;
    *out = (uint64_t)parsed;
    return 1;
}

static iris_expected_media_state_t expected_state(const char *value) {
    if (!value) return 0;
    if (strcmp(value, "opening") == 0) return IRIS_EXPECTED_MEDIA_OPENING;
    if (strcmp(value, "active") == 0) return IRIS_EXPECTED_MEDIA_ACTIVE;
    if (strcmp(value, "closing") == 0) return IRIS_EXPECTED_MEDIA_CLOSING;
    return 0;
}

static iris_expected_command_state_t expected_command_state(
    const char *value) {
    if (!value) return 0;
    if (strcmp(value, "pending") == 0) return IRIS_EXPECTED_COMMAND_PENDING;
    if (strcmp(value, "dispatched") == 0) {
        return IRIS_EXPECTED_COMMAND_DISPATCHED;
    }
    return 0;
}

static int parse_expected_resource(
    const json_value_t *item, iris_expected_media_resource_t *out) {
    const char *state;
    const char *command_state;
    if (!item || !out || json_type(item) != JSON_OBJECT) return 0;
    memset(out, 0, sizeof(*out));
    state = json_get_string(item, "state");
    command_state = json_get_string(item, "activeCommandStatus");
    out->state = expected_state(state);
    out->active_command_state = expected_command_state(command_state);
    if (!copy_json_string(item, "tenantId", out->tenant_id,
                          sizeof(out->tenant_id), 1) ||
        !copy_json_string(item, "providerId", out->provider_id,
                          sizeof(out->provider_id), 1) ||
        !copy_json_string(item, "sessionId", out->provider_session_id,
                          sizeof(out->provider_session_id), 1) ||
        !copy_json_string(item, "ownerNodeId", out->owner_node_id,
                          sizeof(out->owner_node_id), 1) ||
        !copy_json_string(item, "dialogId", out->dialog_id,
                          sizeof(out->dialog_id), 1) ||
        !copy_json_string(item, "roomId", out->room_id,
                          sizeof(out->room_id), 1) ||
        !copy_json_string(item, "callId", out->call_id,
                          sizeof(out->call_id), 1) ||
        !copy_json_string(item, "activeCommandId", out->active_command_id,
                          sizeof(out->active_command_id), 1) ||
        !copy_json_string(item, "dispatchWorkerId",
                          out->dispatch_worker_id,
                          sizeof(out->dispatch_worker_id), 0) ||
        !json_u64(item, "sessionRevision", &out->session_revision) ||
        !json_u64(item, "ownerEpoch", &out->owner_epoch) ||
        !json_u64(item, "ownerLeaseExpiresAtUnixMs",
                  &out->owner_lease_expires_at_ms) ||
        !json_u64(item, "callGeneration", &out->call_generation) ||
        !json_u64(item, "operationGeneration",
                  &out->operation_generation) ||
        !json_u64(item, "dispatchEpoch", &out->dispatch_epoch) ||
        !json_u64(item, "dispatchLeaseExpiresAtUnixMs",
                  &out->dispatch_lease_expires_at_ms) ||
        strcmp(out->provider_id, IRIS_MEDIA_RECONCILE_PROVIDER_ID) != 0 ||
        out->session_revision == 0u || out->owner_epoch == 0u ||
        out->call_generation == 0u || out->operation_generation == 0u ||
        out->state == 0 || out->active_command_state == 0 ||
        (out->active_command_state == IRIS_EXPECTED_COMMAND_DISPATCHED &&
         (out->dispatch_epoch == 0u || !out->dispatch_worker_id[0]))) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    return 1;
}

static ivr_status_t default_fetch_expected(
    void *context, iris_expected_media_resource_t *resources,
    size_t capacity, size_t *out_count) {
    iris_media_reconciler_t *reconciler =
        (iris_media_reconciler_t *)context;
    iris_flowmq_provider_query_t query;
    iris_flowmq_provider_observation_t observation;
    char query_id[SALTS_UUID_STRING_SIZE];
    char created_at[32];
    uint64_t resource_count = 0;
    uint64_t revision = 0;
    uint64_t cursor = 0;
    size_t count = 0;
    json_value_t *root = NULL;
    if (!reconciler || !resources || !out_count || capacity == 0u) {
        return IVR_EINVAL;
    }
    *out_count = 0;
    for (;;) {
        json_value_t *items;
        uint64_t page_count = 0;
        if (!make_uuid(query_id, sizeof(query_id))) return IVR_ESTATE;
        {
            time_t current = time(NULL);
            struct tm value;
#ifdef _WIN32
            if (gmtime_s(&value, &current) != 0) return IVR_ESTATE;
#else
            if (!gmtime_r(&current, &value)) return IVR_ESTATE;
#endif
            if (snprintf(created_at, sizeof(created_at),
                         "%04d-%02d-%02dT%02d:%02d:%02dZ",
                         value.tm_year + 1900, value.tm_mon + 1,
                         value.tm_mday, value.tm_hour, value.tm_min,
                         value.tm_sec) <= 0) {
                return IVR_ESTATE;
            }
        }
        memset(&query, 0, sizeof(query));
        query.tenant_id = "*";
        query.query_id = query_id;
        query.query_type = IRIS_RECONCILE_EXPECTED_QUERY_TYPE;
        query.created_at = created_at;
        query.deadline_at = created_at;
        query.expected_revision = revision;
        query.cursor = cursor;
        query.limit = reconciler->inventory_page_size;
        query.payload_json = IRIS_RECONCILE_EXPECTED_QUERY_PAYLOAD;
        if (iris_flowmq_provider_send_query(
                reconciler->provider, &query, reconciler->request_timeout_ms,
                &observation) != IVR_OK) {
            return IVR_EBUSY;
        }
        if (observation.wire.status != ProviderQueryStatus_QueryOk ||
            (revision != 0u && observation.revision != revision) ||
            observation.cursor != cursor ||
            ((root = json_parse((const char *)((const uint8_t *)observation.wire.payload_json), strlen(observation.wire.payload_json))) ? 0 : -1) != 0 ||
            !root || json_type(root) != JSON_OBJECT ||
            !json_u64(root, "resourceCount", &resource_count)) {
            json_free(root);
            root = NULL;
            iris_flowmq_provider_observation_clear(&observation);
            return IVR_ESTATE;
        }
        if (revision == 0u) revision = observation.revision;
        items = json_object_get(root, "resources");
        page_count = items && json_type(items) == JSON_ARRAY
                         ? (uint64_t)json_array_size(items)
                         : UINT64_MAX;
        if (resource_count > capacity || page_count == UINT64_MAX ||
            page_count > capacity - count ||
            (observation.wire.has_more && page_count == 0u) ||
            count + page_count > resource_count) {
            json_free(root);
            root = NULL;
            iris_flowmq_provider_observation_clear(&observation);
            return resource_count > capacity ? IVR_ENOSPC : IVR_ESTATE;
        }
        for (size_t i = 0; i < (size_t)page_count; ++i) {
            if (!parse_expected_resource(json_array_get(items, i),
                                         &resources[count + i])) {
                json_free(root);
                root = NULL;
                iris_flowmq_provider_observation_clear(&observation);
                return IVR_ESTATE;
            }
        }
        count += (size_t)page_count;
        cursor = observation.next_cursor;
        {
            int has_more = observation.wire.has_more ? 1 : 0;
            iris_flowmq_provider_observation_clear(&observation);
            json_free(root);
            root = NULL;
            root = NULL;
            if (!has_more) break;
        }
    }
    if (count != (size_t)resource_count || cursor != 0u) return IVR_ESTATE;
    *out_count = count;
    return IVR_OK;
}

static ivr_status_t default_list_workers(
    void *context, ivr_fmq_worker_snapshot_t *workers, uint32_t capacity,
    uint32_t *out_count, uint32_t *out_total) {
    iris_media_reconciler_t *reconciler =
        (iris_media_reconciler_t *)context;
    return ivr_fmq_adapter_list_workers(reconciler->adapter, workers, capacity,
                                        out_count, out_total);
}

static ivr_status_t default_request_inventory(
    void *context, const ivr_worker_inventory_request_t *request) {
    iris_media_reconciler_t *reconciler =
        (iris_media_reconciler_t *)context;
    return ivr_fmq_adapter_request_inventory(reconciler->adapter, request);
}

static ivr_status_t default_rebind(
    void *context, const ivr_worker_inventory_record_t *record) {
    iris_media_reconciler_t *reconciler =
        (iris_media_reconciler_t *)context;
    return ivr_fmq_adapter_rebind_dialog(reconciler->adapter, record);
}

static ivr_status_t default_close(
    void *context, const ivr_worker_inventory_record_t *record,
    const char *message_id, uint64_t deadline_timeout_ms) {
    iris_media_reconciler_t *reconciler =
        (iris_media_reconciler_t *)context;
    return ivr_fmq_adapter_close_orphan(reconciler->adapter, record,
                                        message_id, deadline_timeout_ms);
}

static ivr_status_t default_complete(
    void *context, const char *worker_id, const char *worker_instance_id,
    uint64_t worker_epoch) {
    iris_media_reconciler_t *reconciler =
        (iris_media_reconciler_t *)context;
    return ivr_fmq_adapter_complete_worker_reconcile(
        reconciler->adapter, worker_id, worker_instance_id, worker_epoch);
}

static int default_required(void *context) {
    iris_media_reconciler_t *reconciler =
        (iris_media_reconciler_t *)context;
    return ivr_fmq_adapter_reconcile_required(reconciler->adapter);
}

static int stable_resource_lost_id(
    const iris_expected_media_resource_t *resource, char *out,
    size_t capacity) {
    char input[1024];
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned digest_size = 0;
    int written = snprintf(
        input, sizeof(input),
        "provider.media.resource_lost|%s|%s|%s|%s|%s|%llu|%llu",
        resource->provider_id, resource->provider_session_id,
        resource->dialog_id, resource->room_id, resource->call_id,
        (unsigned long long)resource->call_generation,
        (unsigned long long)resource->operation_generation);
    if (written <= 0 || (size_t)written >= sizeof(input) || capacity < 68u ||
        !EVP_Digest(input, (size_t)written, digest, &digest_size,
                    EVP_sha256(), NULL) || digest_size != 32u) {
        return 0;
    }
    memcpy(out, "rl-", 3u);
    for (unsigned i = 0; i < digest_size; ++i) {
        static const char hex[] = "0123456789abcdef";
        out[3u + i * 2u] = hex[digest[i] >> 4u];
        out[4u + i * 2u] = hex[digest[i] & 0x0fu];
    }
    out[67] = '\0';
    return 1;
}

static ivr_status_t default_submit_lost(
    void *context, const iris_expected_media_resource_t *resource,
    const char *reason) {
    iris_media_reconciler_t *reconciler =
        (iris_media_reconciler_t *)context;
    ivr_media_event_t event;
    if (!reconciler || !reconciler->event_outbox || !resource || !reason) {
        return IVR_EINVAL;
    }
    ivr_status_t status = iris_media_reconciler_build_resource_lost_event(
        resource, reason, &event);
    if (status != IVR_OK) return status;
    return iris_event_outbox_on_media_event(reconciler->event_outbox, &event);
}

ivr_status_t iris_media_reconciler_build_resource_lost_event(
    const iris_expected_media_resource_t *resource, const char *reason,
    ivr_media_event_t *out_event) {
    uint64_t occurred_at;
    int written;
    if (!resource || !reason || !reason[0] || !out_event ||
        !resource->tenant_id[0] || !resource->provider_session_id[0] ||
        !resource->dialog_id[0] ||
        !resource->room_id[0] || !resource->call_id[0] ||
        !resource->active_command_id[0] || resource->call_generation == 0u ||
        resource->operation_generation == 0u || resource->owner_epoch == 0u) {
        return IVR_EINVAL;
    }
    memset(out_event, 0, sizeof(*out_event));
    if (!stable_resource_lost_id(resource, out_event->event_id,
                                 sizeof(out_event->event_id))) {
        return IVR_ESTATE;
    }
    snprintf(out_event->tenant_id, sizeof(out_event->tenant_id), "%s",
             resource->tenant_id);
    snprintf(out_event->provider_session_id,
             sizeof(out_event->provider_session_id), "%s",
             resource->provider_session_id);
    snprintf(out_event->dialog_id, sizeof(out_event->dialog_id), "%s",
             resource->dialog_id);
    snprintf(out_event->room_id, sizeof(out_event->room_id), "%s",
             resource->room_id);
    snprintf(out_event->call_id, sizeof(out_event->call_id), "%s",
             resource->call_id);
    out_event->call_generation = resource->call_generation;
    out_event->sequence = resource->operation_generation;
    snprintf(out_event->event_type, sizeof(out_event->event_type), "%s",
             "provider.media.resource_lost");
    occurred_at = resource->owner_lease_expires_at_ms;
    if (resource->dispatch_lease_expires_at_ms > occurred_at) {
        occurred_at = resource->dispatch_lease_expires_at_ms;
    }
    out_event->occurred_at_ms = occurred_at ? occurred_at : 1u;
    written = snprintf(
        out_event->payload_json, sizeof(out_event->payload_json),
        "{\"reason\":\"%s\",\"activeCommandId\":\"%s\"," 
        "\"ownerEpoch\":%llu,\"operationGeneration\":%llu}",
        "resource_not_recoverable", resource->active_command_id,
        (unsigned long long)resource->owner_epoch,
        (unsigned long long)resource->operation_generation);
    return written > 0 &&
                   (size_t)written < sizeof(out_event->payload_json)
               ? IVR_OK
               : IVR_ENOSPC;
}

static int expected_identity_matches(
    const iris_expected_media_resource_t *expected,
    const ivr_worker_inventory_record_t *inventory) {
    return strcmp(expected->tenant_id, inventory->tenant_id) == 0 &&
           strcmp(expected->provider_session_id,
                  inventory->provider_session_id) == 0 &&
           strcmp(expected->dialog_id, inventory->dialog_id) == 0 &&
           strcmp(expected->room_id, inventory->room_id) == 0 &&
           strcmp(expected->call_id, inventory->call_id) == 0 &&
           expected->call_generation == inventory->call_generation;
}

static int expected_rebind_matches(
    const iris_expected_media_resource_t *expected,
    const ivr_worker_inventory_record_t *inventory) {
    int operation_matches;
    if (!expected_identity_matches(expected, inventory) ||
        inventory->state != IVR_WORKER_RESOURCE_ACTIVE ||
        !inventory->rebindable) {
        return 0;
    }
    operation_matches =
        expected->operation_generation == inventory->operation_generation;
    if (expected->active_command_state == IRIS_EXPECTED_COMMAND_PENDING &&
        inventory->operation_generation != UINT64_MAX) {
        operation_matches =
            inventory->operation_generation + 1u ==
            expected->operation_generation;
    }
    return operation_matches &&
           (expected->state == IRIS_EXPECTED_MEDIA_ACTIVE ||
            (expected->state == IRIS_EXPECTED_MEDIA_CLOSING &&
             expected->active_command_state ==
                 IRIS_EXPECTED_COMMAND_PENDING) ||
            (expected->state == IRIS_EXPECTED_MEDIA_OPENING &&
             expected->active_command_state ==
                 IRIS_EXPECTED_COMMAND_DISPATCHED));
}

static int make_uuid(char *out, size_t capacity) {
    salts_uuid_t uuid;
    return out && capacity >= SALTS_UUID_STRING_SIZE &&
           salts_uuid_v7_generate(&uuid) == SALTS_OK &&
           salts_uuid_format(&uuid, out, capacity) == SALTS_OK;
}

static int stable_orphan_close_id(
    const ivr_worker_inventory_record_t *record, char *out,
    size_t capacity) {
    char input[1024];
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned digest_size = 0u;
    salts_uuid_t uuid;
    int written;
    if (!record || !out || capacity < SALTS_UUID_STRING_SIZE) return 0;
    written = snprintf(
        input, sizeof(input),
        "reconcile.orphan.close|%s|%s|%s|%s|%llu|%llu|%s|%s|%llu",
        record->provider_session_id, record->dialog_id, record->room_id,
        record->call_id, (unsigned long long)record->call_generation,
        (unsigned long long)record->operation_generation, record->worker_id,
        record->worker_instance_id, (unsigned long long)record->worker_epoch);
    if (written <= 0 || (size_t)written >= sizeof(input) ||
        !EVP_Digest(input, (size_t)written, digest, &digest_size,
                    EVP_sha256(), NULL) || digest_size < sizeof(uuid.bytes)) {
        return 0;
    }
    memcpy(uuid.bytes, digest, sizeof(uuid.bytes));
    /* RFC 9562 UUIDv8 marks this application-defined SHA-256 identity while
       retaining the canonical 36-byte message-id representation. */
    uuid.bytes[6] = (uint8_t)((uuid.bytes[6] & 0x0fu) | 0x80u);
    uuid.bytes[8] = (uint8_t)((uuid.bytes[8] & 0x3fu) | 0x80u);
    return salts_uuid_format(&uuid, out, capacity) == SALTS_OK;
}

static ivr_status_t wait_inventory_page(
    iris_media_reconciler_t *reconciler, const char *message_id,
    ivr_worker_inventory_envelope_t *out) {
    uint64_t deadline = salts_monotonic_ms() + reconciler->request_timeout_ms;
    salts_mutex_lock(&reconciler->mutex);
    while (atomic_load(&reconciler->running) || !reconciler->thread_started) {
        while (reconciler->inventory_count > 0u) {
            ivr_worker_inventory_envelope_t page =
                reconciler->inventory_queue[reconciler->inventory_head];
            memset(&reconciler->inventory_queue[reconciler->inventory_head], 0,
                   sizeof(page));
            reconciler->inventory_head =
                (reconciler->inventory_head + 1u) %
                reconciler->inventory_queue_capacity;
            --reconciler->inventory_count;
            if (strcmp(page.message_id, message_id) == 0) {
                *out = page;
                salts_mutex_unlock(&reconciler->mutex);
                return IVR_OK;
            }
        }
        if (salts_monotonic_ms() >= deadline) break;
        (void)salts_cond_timedwait(
            &reconciler->wake, &reconciler->mutex,
            (deadline - salts_monotonic_ms()) * UINT64_C(1000000));
    }
    salts_mutex_unlock(&reconciler->mutex);
    return IVR_EBUSY;
}

static ivr_status_t process_inventory_record(
    iris_media_reconciler_t *reconciler,
    const ivr_worker_inventory_record_t *record, size_t expected_count,
    int *out_cleanup_pending) {
    size_t found = SIZE_MAX;
    for (size_t i = 0; i < expected_count; ++i) {
        if (expected_identity_matches(&reconciler->expected[i], record)) {
            found = i;
            break;
        }
    }
    if (found != SIZE_MAX && !reconciler->matched[found] &&
        expected_rebind_matches(&reconciler->expected[found], record)) {
        ivr_status_t status = reconciler->ops.rebind_dialog(
            reconciler->ops.context, record);
        if (status != IVR_OK) return status;
        reconciler->matched[found] = 1u;
        salts_mutex_lock(&reconciler->mutex);
        ++reconciler->stats.rebound_total;
        salts_mutex_unlock(&reconciler->mutex);
        return IVR_OK;
    }
    {
        char message_id[SALTS_UUID_STRING_SIZE];
        ivr_status_t status;
        if (!stable_orphan_close_id(record, message_id,
                                    sizeof(message_id))) {
            return IVR_ESTATE;
        }
        status = reconciler->ops.close_orphan(
            reconciler->ops.context, record, message_id,
            reconciler->close_deadline_ms);
        if (status != IVR_OK) return status;
        *out_cleanup_pending = 1;
        salts_mutex_lock(&reconciler->mutex);
        ++reconciler->stats.orphan_close_total;
        salts_mutex_unlock(&reconciler->mutex);
    }
    return IVR_OK;
}

static ivr_status_t reconcile_worker(
    iris_media_reconciler_t *reconciler,
    const ivr_fmq_worker_snapshot_t *worker, size_t expected_count,
    int *out_cleanup_pending) {
    ivr_worker_inventory_request_t request;
    uint64_t revision = 0;
    uint32_t cursor = 0;
    uint32_t observed = 0;
    for (;;) {
        ivr_worker_inventory_envelope_t page;
        ivr_status_t status;
        memset(&request, 0, sizeof(request));
        if (!make_uuid(request.message_id, sizeof(request.message_id))) {
            return IVR_ESTATE;
        }
        snprintf(request.worker_id, sizeof(request.worker_id), "%s",
                 worker->worker_id);
        request.query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
        request.query.expected_revision = revision;
        request.query.cursor = cursor;
        request.query.limit = reconciler->inventory_page_size;
        status = reconciler->ops.request_inventory(reconciler->ops.context,
                                                   &request);
        if (status != IVR_OK) return status;
        status = wait_inventory_page(reconciler, request.message_id, &page);
        if (status != IVR_OK) return status;
        if (page.status_code != IVR_OK ||
            strcmp(page.worker_id, worker->worker_id) != 0 ||
            page.page.cursor != cursor ||
            (revision != 0u && page.page.revision != revision) ||
            page.page.count > reconciler->inventory_page_size ||
            UINT32_MAX - observed < page.page.count) {
            return IVR_EVERSION;
        }
        if (revision == 0u) revision = page.page.revision;
        observed += page.page.count;
        salts_mutex_lock(&reconciler->mutex);
        ++reconciler->stats.inventory_pages_total;
        salts_mutex_unlock(&reconciler->mutex);
        for (uint32_t i = 0; i < page.page.count; ++i) {
            status = process_inventory_record(
                reconciler, &page.page.records[i], expected_count,
                out_cleanup_pending);
            if (status != IVR_OK) return status;
        }
        if (!page.page.has_more) {
            if (observed != page.page.total_active) return IVR_EVERSION;
            break;
        }
        cursor = page.page.next_cursor;
        if (cursor == 0u) return IVR_EVERSION;
    }
    if (*out_cleanup_pending) return IVR_EBUSY;
    return reconciler->ops.complete_worker(
        reconciler->ops.context, worker->worker_id, worker->instance_id,
        worker->connection_generation);
}

ivr_status_t iris_media_reconciler_reconcile_once(
    iris_media_reconciler_t *reconciler, int allow_missing_loss) {
    size_t expected_count = 0;
    uint32_t worker_count = 0;
    uint32_t worker_total = 0;
    int cleanup_pending = 0;
    int has_reconciling_worker = 0;
    ivr_status_t status;
    if (!reconciler || (allow_missing_loss != 0 && allow_missing_loss != 1)) {
        return IVR_EINVAL;
    }
    salts_mutex_lock(&reconciler->mutex);
    if (reconciler->intake_closed) {
        salts_mutex_unlock(&reconciler->mutex);
        return IVR_ECLOSED;
    }
    salts_mutex_unlock(&reconciler->mutex);
    if (reconciler->thread_started && !atomic_load(&reconciler->running)) {
        return IVR_ECLOSED;
    }
    memset(reconciler->expected, 0,
           reconciler->resource_capacity * sizeof(*reconciler->expected));
    memset(reconciler->matched, 0, reconciler->resource_capacity);
    set_state(reconciler, IRIS_MEDIA_RECONCILE_FETCHING_EXPECTED);
    status = reconciler->ops.fetch_expected(
        reconciler->ops.context, reconciler->expected,
        reconciler->resource_capacity, &expected_count);
    salts_mutex_lock(&reconciler->mutex);
    ++reconciler->stats.expected_fetches_total;
    salts_mutex_unlock(&reconciler->mutex);
    if (status != IVR_OK || expected_count > reconciler->resource_capacity) {
        return status == IVR_OK ? IVR_EVERSION : status;
    }
    status = reconciler->ops.list_workers(
        reconciler->ops.context, reconciler->workers,
        reconciler->worker_capacity, &worker_count, &worker_total);
    if (status != IVR_OK || worker_total != worker_count) {
        return status == IVR_OK ? IVR_EVERSION : status;
    }
    set_state(reconciler, IRIS_MEDIA_RECONCILE_FETCHING_INVENTORY);
    for (uint32_t i = 0; i < worker_count; ++i) {
        if (!reconciler->workers[i].requires_reconcile) continue;
        has_reconciling_worker = 1;
        status = reconcile_worker(reconciler, &reconciler->workers[i],
                                  expected_count, &cleanup_pending);
        if (status != IVR_OK && status != IVR_EBUSY) return status;
    }
    set_state(reconciler, IRIS_MEDIA_RECONCILE_APPLYING);
    for (size_t i = 0; i < expected_count; ++i) {
        if (reconciler->matched[i]) continue;
        if (reconciler->expected[i].active_command_state ==
                IRIS_EXPECTED_COMMAND_PENDING ||
            reconciler->expected[i].state == IRIS_EXPECTED_MEDIA_OPENING) {
            continue;
        }
        if (!allow_missing_loss) return IVR_EBUSY;
        status = reconciler->ops.submit_resource_lost(
            reconciler->ops.context, &reconciler->expected[i],
            has_reconciling_worker ? "generation_or_owner_conflict"
                                   : "inventory_missing");
        if (status != IVR_OK) return status;
        salts_mutex_lock(&reconciler->mutex);
        ++reconciler->stats.resource_lost_total;
        salts_mutex_unlock(&reconciler->mutex);
        cleanup_pending = 1;
    }
    if (cleanup_pending) return IVR_EBUSY;
    set_state(reconciler, IRIS_MEDIA_RECONCILE_READY);
    return IVR_OK;
}

ivr_status_t iris_media_reconciler_on_inventory_page(
    void *context, const ivr_worker_inventory_envelope_t *page) {
    iris_media_reconciler_t *reconciler =
        (iris_media_reconciler_t *)context;
    uint32_t tail;
    int queue_full;
    int intake_closed;
    if (!reconciler || !page) return IVR_EINVAL;
    salts_mutex_lock(&reconciler->mutex);
    intake_closed = reconciler->intake_closed;
    queue_full = reconciler->inventory_count ==
                 reconciler->inventory_queue_capacity;
    if (intake_closed ||
        (!atomic_load(&reconciler->running) && reconciler->thread_started) ||
        queue_full) {
        if (queue_full) {
            ++reconciler->stats.inventory_queue_full_total;
        }
        salts_mutex_unlock(&reconciler->mutex);
        if (intake_closed) return IVR_ECLOSED;
        return queue_full ? IVR_ENOSPC : IVR_ECLOSED;
    }
    tail = (reconciler->inventory_head + reconciler->inventory_count) %
           reconciler->inventory_queue_capacity;
    reconciler->inventory_queue[tail] = *page;
    ++reconciler->inventory_count;
    salts_cond_broadcast(&reconciler->wake);
    salts_mutex_unlock(&reconciler->mutex);
    return IVR_OK;
}

static void interruptible_wait(iris_media_reconciler_t *reconciler,
                               uint32_t milliseconds) {
    salts_mutex_lock(&reconciler->mutex);
    if (atomic_load(&reconciler->running)) {
        (void)salts_cond_timedwait(&reconciler->wake, &reconciler->mutex,
                                   (uint64_t)milliseconds * UINT64_C(1000000));
    }
    salts_mutex_unlock(&reconciler->mutex);
}

static void reconcile_thread(void *context) {
    iris_media_reconciler_t *reconciler =
        (iris_media_reconciler_t *)context;
    uint32_t attempts = 0;
    while (atomic_load(&reconciler->running)) {
        ivr_status_t status;
        salts_mutex_lock(&reconciler->mutex);
        iris_media_reconcile_state_t state = reconciler->stats.state;
        salts_mutex_unlock(&reconciler->mutex);
        if (state == IRIS_MEDIA_RECONCILE_READY &&
            !reconciler->ops.reconcile_required(reconciler->ops.context)) {
            interruptible_wait(reconciler, IRIS_RECONCILE_IDLE_POLL_MS);
            continue;
        }
        set_state(reconciler, IRIS_MEDIA_RECONCILE_NOT_READY);
        status = iris_media_reconciler_reconcile_once(
            reconciler, attempts + 1u >= reconciler->retry_max_attempts);
        salts_mutex_lock(&reconciler->mutex);
        ++reconciler->stats.reconcile_cycles_total;
        if (status != IVR_OK && status != IVR_EBUSY) {
            ++reconciler->stats.reconcile_failures_total;
        }
        if (reconciler->stats.state != IRIS_MEDIA_RECONCILE_DRAINING) {
            if (status == IVR_OK) {
                reconciler->stats.state = IRIS_MEDIA_RECONCILE_READY;
            } else if (status != IVR_EBUSY) {
                reconciler->stats.state = IRIS_MEDIA_RECONCILE_FAILED;
            }
        }
        salts_mutex_unlock(&reconciler->mutex);
        if (status == IVR_OK) {
            attempts = 0;
        } else if (attempts < reconciler->retry_max_attempts) {
            ++attempts;
        }
        if (status != IVR_OK) {
            interruptible_wait(reconciler, reconciler->retry_backoff_ms);
        }
    }
}

static int ops_valid(const iris_media_reconciler_ops_t *ops) {
    return ops && ops->fetch_expected && ops->list_workers &&
           ops->request_inventory && ops->rebind_dialog &&
           ops->close_orphan && ops->complete_worker &&
           ops->reconcile_required && ops->submit_resource_lost;
}

iris_media_reconciler_t *iris_media_reconciler_create(
    const iris_media_reconciler_config_t *config) {
    iris_media_reconciler_t *reconciler;
    int injected;
    if (!config || config->resource_capacity == 0u ||
        config->worker_capacity == 0u ||
        config->inventory_queue_capacity == 0u ||
        config->retry_max_attempts == 0u || config->retry_backoff_ms == 0u ||
        config->request_timeout_ms == 0u || config->drain_timeout_ms == 0u ||
        config->drain_timeout_ms < config->request_timeout_ms ||
        config->inventory_page_size == 0u ||
        config->inventory_page_size > IVR_WORKER_INVENTORY_MAX_PAGE_SIZE ||
        config->close_deadline_ms == 0u ||
        config->resource_capacity > SIZE_MAX /
                                        sizeof(iris_expected_media_resource_t) ||
        config->worker_capacity > SIZE_MAX /
                                      sizeof(ivr_fmq_worker_snapshot_t) ||
        config->inventory_queue_capacity >
            SIZE_MAX / sizeof(ivr_worker_inventory_envelope_t)) {
        return NULL;
    }
    injected = ops_valid(&config->ops);
    if (!injected && (!config->provider || !config->event_outbox)) {
        return NULL;
    }
    reconciler = (iris_media_reconciler_t *)calloc(1, sizeof(*reconciler));
    if (!reconciler) return NULL;
    reconciler->resource_capacity = config->resource_capacity;
    reconciler->worker_capacity = config->worker_capacity;
    reconciler->inventory_queue_capacity = config->inventory_queue_capacity;
    reconciler->retry_max_attempts = config->retry_max_attempts;
    reconciler->retry_backoff_ms = config->retry_backoff_ms;
    reconciler->request_timeout_ms = config->request_timeout_ms;
    reconciler->drain_timeout_ms = config->drain_timeout_ms;
    reconciler->inventory_page_size = config->inventory_page_size;
    reconciler->close_deadline_ms = config->close_deadline_ms;
    reconciler->event_outbox = config->event_outbox;
    reconciler->provider = config->provider;
    reconciler->injected_ops = injected;
    reconciler->expected = (iris_expected_media_resource_t *)calloc(
        config->resource_capacity, sizeof(*reconciler->expected));
    reconciler->matched =
        (unsigned char *)calloc(config->resource_capacity, 1u);
    reconciler->workers = (ivr_fmq_worker_snapshot_t *)calloc(
        config->worker_capacity, sizeof(*reconciler->workers));
    reconciler->inventory_queue =
        (ivr_worker_inventory_envelope_t *)calloc(
            config->inventory_queue_capacity,
            sizeof(*reconciler->inventory_queue));
    if (!reconciler->expected || !reconciler->matched ||
        !reconciler->workers || !reconciler->inventory_queue) {
        goto fail;
    }
    salts_mutex_init(&reconciler->mutex);
    salts_cond_init(&reconciler->wake);
    atomic_init(&reconciler->running, 0);
    reconciler->stats.state = IRIS_MEDIA_RECONCILE_NOT_READY;
    reconciler->stats.inventory_queue_capacity =
        config->inventory_queue_capacity;
    if (injected) {
        reconciler->ops = config->ops;
        return reconciler;
    }
    reconciler->ops.context = reconciler;
    reconciler->ops.fetch_expected = default_fetch_expected;
    reconciler->ops.list_workers = default_list_workers;
    reconciler->ops.request_inventory = default_request_inventory;
    reconciler->ops.rebind_dialog = default_rebind;
    reconciler->ops.close_orphan = default_close;
    reconciler->ops.complete_worker = default_complete;
    reconciler->ops.reconcile_required = default_required;
    reconciler->ops.submit_resource_lost = default_submit_lost;
    return reconciler;

fail:
    free(reconciler->inventory_queue);
    free(reconciler->workers);
    free(reconciler->matched);
    free(reconciler->expected);
    free(reconciler);
    return NULL;
}

int iris_media_reconciler_set_adapter(iris_media_reconciler_t *reconciler,
                                      ivr_fmq_adapter_t *adapter) {
    if (!reconciler || !adapter || reconciler->injected_ops ||
        reconciler->thread_started || reconciler->adapter) {
        return -1;
    }
    reconciler->adapter = adapter;
    return 0;
}

int iris_media_reconciler_start(iris_media_reconciler_t *reconciler) {
    if (!reconciler || reconciler->thread_started ||
        reconciler->intake_closed ||
        (!reconciler->injected_ops && !reconciler->adapter)) {
        return -1;
    }
    atomic_store(&reconciler->running, 1);
    set_state(reconciler, IRIS_MEDIA_RECONCILE_NOT_READY);
    if (salts_thread_create(&reconciler->thread, reconcile_thread,
                            reconciler) != 0) {
        atomic_store(&reconciler->running, 0);
        return -1;
    }
    reconciler->thread_started = 1;
    return 0;
}

void iris_media_reconciler_stop(iris_media_reconciler_t *reconciler) {
    if (!reconciler || !reconciler->thread_started) return;
    set_state(reconciler, IRIS_MEDIA_RECONCILE_DRAINING);
    atomic_store(&reconciler->running, 0);
    salts_mutex_lock(&reconciler->mutex);
    reconciler->intake_closed = 1;
    salts_cond_broadcast(&reconciler->wake);
    salts_mutex_unlock(&reconciler->mutex);
    salts_thread_join(&reconciler->thread);
    salts_thread_destroy(&reconciler->thread);
    reconciler->thread_started = 0;
}

int iris_media_reconciler_accepting_commands(
    iris_media_reconciler_t *reconciler) {
    iris_media_reconcile_state_t state;
    if (!reconciler) return 0;
    salts_mutex_lock(&reconciler->mutex);
    state = reconciler->stats.state;
    salts_mutex_unlock(&reconciler->mutex);
    return state == IRIS_MEDIA_RECONCILE_READY &&
           !reconciler->ops.reconcile_required(reconciler->ops.context);
}

void iris_media_reconciler_get_stats(
    iris_media_reconciler_t *reconciler,
    iris_media_reconciler_stats_t *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    if (!reconciler) return;
    salts_mutex_lock(&reconciler->mutex);
    *stats = reconciler->stats;
    stats->inventory_queue_items = reconciler->inventory_count;
    salts_mutex_unlock(&reconciler->mutex);
}

void iris_media_reconciler_destroy(iris_media_reconciler_t *reconciler) {
    if (!reconciler) return;
    iris_media_reconciler_stop(reconciler);
    salts_cond_destroy(&reconciler->wake);
    salts_mutex_destroy(&reconciler->mutex);
    free(reconciler->inventory_queue);
    free(reconciler->workers);
    free(reconciler->matched);
    free(reconciler->expected);
    free(reconciler);
}
