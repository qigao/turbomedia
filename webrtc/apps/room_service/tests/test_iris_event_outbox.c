#include "iris_event_outbox.h"

#include <tinytest.h>
#include <salts_error.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TEST_STORE_CAPACITY = 8 };

static uint64_t g_test_now_ms;

static uint64_t test_realtime_ms(void *context) {
    (void)context;
    return g_test_now_ms;
}

typedef struct test_record_s {
    uint8_t key[128];
    size_t key_size;
    uint8_t *value;
    size_t value_size;
    uint64_t revision;
    int used;
} test_record_t;

typedef struct test_store_s {
    iris_record_store_t api;
    test_record_t records[TEST_STORE_CAPACITY];
    int scan_status;
    int commit_calls;
    int fail_commit_call;
} test_store_t;

typedef struct test_delivery_s {
    int calls;
    ivr_status_t status;
    ivr_media_event_t last_event;
    uint64_t last_revision;
} test_delivery_t;

static int test_find_record(test_store_t *store, const uint8_t *key,
                            size_t key_size) {
    size_t i;
    for (i = 0u; i < TEST_STORE_CAPACITY; ++i) {
        if (store->records[i].used && store->records[i].key_size == key_size &&
            memcmp(store->records[i].key, key, key_size) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int test_scan(void *context, iris_record_visit_fn visit,
                     void *visit_context) {
    test_store_t *store = (test_store_t *)context;
    size_t i;
    if (!store || !visit) return SALTS_EINVAL;
    if (store->scan_status != SALTS_OK) return store->scan_status;
    for (i = 0u; i < TEST_STORE_CAPACITY; ++i) {
        iris_record_view_t view = IRIS_RECORD_VIEW_INIT;
        int rc;
        if (!store->records[i].used) continue;
        view.key = store->records[i].key;
        view.key_size = store->records[i].key_size;
        view.revision = store->records[i].revision;
        view.value = store->records[i].value;
        view.value_size = store->records[i].value_size;
        rc = visit(visit_context, &view);
        if (rc != SALTS_OK) return rc;
    }
    return SALTS_OK;
}

static int test_commit(void *context,
                       const iris_record_mutation_t *mutations,
                       size_t mutation_count) {
    test_store_t *store = (test_store_t *)context;
    const iris_record_mutation_t *mutation;
    int index;
    size_t i;
    uint8_t *value = NULL;
    if (!store || !mutations || mutation_count != 1u) return SALTS_EINVAL;
    store->commit_calls++;
    if (store->fail_commit_call > 0 &&
        store->commit_calls == store->fail_commit_call) {
        return SALTS_EIO;
    }
    mutation = &mutations[0];
    if (!mutation->key || mutation->key_size == 0u ||
        mutation->key_size > sizeof(store->records[0].key)) {
        return SALTS_EINVAL;
    }
    index = test_find_record(store, mutation->key, mutation->key_size);
    if ((index < 0 && mutation->expected_revision !=
                        IRIS_RECORD_REVISION_ABSENT) ||
        (index >= 0 && store->records[index].revision !=
                           mutation->expected_revision)) {
        return SALTS_EBUSY;
    }
    if (mutation->kind == IRIS_RECORD_DELETE) {
        if (index < 0) return SALTS_EBUSY;
        free(store->records[index].value);
        memset(&store->records[index], 0, sizeof(store->records[index]));
        return SALTS_OK;
    }
    if (mutation->kind != IRIS_RECORD_PUT || !mutation->value ||
        mutation->value_size == 0u ||
        mutation->value_size > store->api.max_value_size ||
        mutation->next_revision <= mutation->expected_revision) {
        return SALTS_EINVAL;
    }
    if (index < 0) {
        for (i = 0u; i < TEST_STORE_CAPACITY; ++i) {
            if (!store->records[i].used) {
                index = (int)i;
                break;
            }
        }
        if (index < 0) return SALTS_ENOSPC;
    }
    value = (uint8_t *)malloc(mutation->value_size);
    if (!value) return SALTS_ENOMEM;
    memcpy(value, mutation->value, mutation->value_size);
    free(store->records[index].value);
    memset(&store->records[index], 0, sizeof(store->records[index]));
    memcpy(store->records[index].key, mutation->key, mutation->key_size);
    store->records[index].key_size = mutation->key_size;
    store->records[index].value = value;
    store->records[index].value_size = mutation->value_size;
    store->records[index].revision = mutation->next_revision;
    store->records[index].used = 1;
    return SALTS_OK;
}

static void test_store_init(test_store_t *store) {
    memset(store, 0, sizeof(*store));
    store->api = (iris_record_store_t)IRIS_RECORD_STORE_INIT;
    store->api.capabilities = IRIS_RECORD_STORE_DURABLE |
                              IRIS_RECORD_STORE_ATOMIC_BATCH;
    store->api.max_key_size = 128u;
    store->api.max_value_size = 16384u;
    store->api.max_batch_size = 2u;
    store->api.max_records = TEST_STORE_CAPACITY;
    store->api.ctx = store;
    store->api.scan = test_scan;
    store->api.commit = test_commit;
    g_test_now_ms = UINT64_C(10000);
}

static void test_store_cleanup(test_store_t *store) {
    size_t i;
    for (i = 0u; i < TEST_STORE_CAPACITY; ++i) {
        free(store->records[i].value);
    }
}

static size_t test_store_count(const test_store_t *store) {
    size_t count = 0u;
    size_t i;
    for (i = 0u; i < TEST_STORE_CAPACITY; ++i) {
        if (store->records[i].used) count++;
    }
    return count;
}

static ivr_status_t test_deliver(void *context,
                                 const ivr_media_event_t *event,
                                 uint64_t store_revision) {
    test_delivery_t *delivery = (test_delivery_t *)context;
    delivery->calls++;
    delivery->last_event = *event;
    delivery->last_revision = store_revision;
    return delivery->status;
}

static iris_event_outbox_retention_config_t test_retention_config(void) {
    iris_event_outbox_retention_config_t retention;
    memset(&retention, 0, sizeof(retention));
    retention.dead_retention_ms = UINT64_C(1000);
    retention.archive_retention_ms = UINT64_C(1000);
    retention.sweep_interval_ms = 3600000u;
    retention.sweep_batch_size = 2u;
    retention.realtime_ms = test_realtime_ms;
    return retention;
}

static iris_event_outbox_t *test_create_outbox(test_store_t *store,
                                                test_delivery_t *delivery) {
    iris_event_outbox_config_t config;
    memset(&config, 0, sizeof(config));
    config.request_queue_capacity = 4u;
    config.store = &store->api;
    config.deliver = test_deliver;
    config.deliver_context = delivery;
    config.retention = test_retention_config();
    return iris_event_outbox_create(&config);
}

static ivr_media_event_t test_event(const char *event_id,
                                    const char *payload) {
    ivr_media_event_t event;
    memset(&event, 0, sizeof(event));
    snprintf(event.event_id, sizeof(event.event_id), "%s", event_id);
    snprintf(event.tenant_id, sizeof(event.tenant_id), "tenant-a");
    snprintf(event.provider_session_id, sizeof(event.provider_session_id),
             "session-a");
    snprintf(event.dialog_id, sizeof(event.dialog_id), "dialog-a");
    snprintf(event.worker_id, sizeof(event.worker_id), "worker-a");
    snprintf(event.room_id, sizeof(event.room_id), "room-a");
    snprintf(event.call_id, sizeof(event.call_id), "call-a");
    event.call_generation = 7u;
    event.sequence = 11u;
    snprintf(event.event_type, sizeof(event.event_type),
             "provider.media.input");
    event.occurred_at_ms = 1234u;
    snprintf(event.input_id, sizeof(event.input_id), "dtmf");
    snprintf(event.input_value, sizeof(event.input_value), "1");
    snprintf(event.payload_json, sizeof(event.payload_json), "%s", payload);
    return event;
}

static void test_normalize_yaml_path(char *path) {
    if (!path) return;
    for (; *path; ++path) {
        if (*path == '\\') *path = '/';
    }
}

spec("Iris durable media event outbox") {
    it("rejects a volatile RecordStore") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        store.api.capabilities = IRIS_RECORD_STORE_ATOMIC_BATCH;
        outbox = test_create_outbox(&store, &delivery);
        check_null(outbox);
        test_store_cleanup(&store);
    }

    it("persists before delivery and deletes only after success") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_outbox_stats_t stats;
        ivr_media_event_t event = test_event("event-a", "{\"value\":\"1\"}");
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        check_equal(delivery.calls, 1);
        check_equal(test_store_count(&store), 1u);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.pending_records, 0u);
        check_equal(stats.in_flight_records, 1u);
        check_equal(stats.dead_records, 0u);
        check_equal(stats.retained_payload_bytes,
                      strlen(event.payload_json));
        check_equal(stats.peak_retained_payload_bytes,
                      strlen(event.payload_json));
        iris_event_outbox_on_delivery_result(outbox, &event,
                                             delivery.last_revision, 1, 202);
        check_equal(test_store_count(&store), 0u);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.persisted_total, 1u);
        check_equal(stats.delivered_total, 1u);
        check_equal(stats.retained_payload_bytes, 0u);
        check_equal(stats.peak_retained_payload_bytes,
                      strlen(event.payload_json));
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("accepts identical duplicates and rejects changed content") {
        test_store_t store;
        test_delivery_t delivery = {0};
        ivr_media_event_t event = test_event("event-b", "{\"value\":\"1\"}");
        ivr_media_event_t conflict = event;
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        snprintf(conflict.payload_json, sizeof(conflict.payload_json),
                 "{\"value\":\"2\"}");
        check_equal(iris_event_outbox_on_media_event(outbox, &conflict),
                     IVR_EBUSY);
        check_equal(delivery.calls, 1);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("rejects a RecordStore capacity whose payload budget can overflow") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        store.api.max_records = SIZE_MAX;
        outbox = test_create_outbox(&store, &delivery);
        check_null(outbox);
        test_store_cleanup(&store);
    }

    it("counts stale settlement without deleting retained payload") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_outbox_stats_t stats;
        ivr_media_event_t event = test_event("event-stale-settle", "{\"v\":1}");
        iris_event_outbox_t *outbox;
        uint64_t revision;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        revision = delivery.last_revision;
        iris_event_outbox_on_delivery_result(
            outbox, &event, revision + 1u, 1, 202);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(test_store_count(&store), 1u);
        check_equal(stats.stale_settlement_total, 1u);
        check_equal(stats.settlement_failure_total, 1u);
        check_equal(stats.retained_payload_bytes,
                      strlen(event.payload_json));
        iris_event_outbox_on_delivery_result(
            outbox, &event, revision, 1, 202);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(test_store_count(&store), 0u);
        check_equal(stats.retained_payload_bytes, 0u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("counts corrupt durable records when a control scan cannot decode") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_outbox_stats_t stats;
        iris_event_dead_letter_t dead[1];
        ivr_media_event_t event = test_event("event-corrupt", "{}");
        iris_event_outbox_t *outbox;
        size_t count = 0u;
        size_t total = 0u;
        int index;
        test_store_init(&store);
        delivery.status = IVR_ENOSPC;
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        index = test_find_record(&store, (const uint8_t *)event.event_id,
                                 strlen(event.event_id));
        check_true(index >= 0);
        if (index >= 0) store.records[index].value[0] = '!';
        check_equal(iris_event_outbox_list_dead_letters(
                         outbox, dead, 1u, &count, &total),
                     IVR_ESTATE);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.decode_failure_total, 1u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("moves terminal failure to dead and explicitly replays it") {
        test_store_t store;
        test_delivery_t delivery = {0};
        ivr_media_event_t event = test_event("event-c", "{}");
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        iris_event_outbox_on_delivery_result(outbox, &event,
                                             delivery.last_revision, 0, 503);
        check_equal(test_store_count(&store), 1u);
        {
            iris_event_outbox_stats_t stats;
            iris_event_dead_letter_t dead[1];
            size_t dead_count = 0u;
            size_t dead_total = 0u;
            iris_event_outbox_get_stats(outbox, &stats);
            check_equal(stats.dead_records, 1u);
            check_equal(stats.in_flight_records, 0u);
            check_equal(iris_event_outbox_list_dead_letters(
                             outbox, dead, 1u, &dead_count, &dead_total),
                         IVR_OK);
            check_equal(dead_count, 1u);
            check_equal(dead_total, 1u);
            check_equal(dead[0].event_id, "event-c");
            check_equal(dead[0].delivery_attempts, 1u);
            check_equal(dead[0].last_http_status, 503);
        }
        check_equal(iris_event_outbox_replay(outbox, event.event_id), IVR_OK);
        check_equal(delivery.calls, 2);
        {
            iris_event_outbox_stats_t stats;
            iris_event_outbox_get_stats(outbox, &stats);
            check_equal(stats.dead_records, 0u);
            check_equal(stats.in_flight_records, 1u);
        }
        iris_event_outbox_on_delivery_result(outbox, &event,
                                             delivery.last_revision, 1, 202);
        check_equal(test_store_count(&store), 0u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("replays a bounded dead-letter batch and reports the remainder") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_replay_batch_result_t result;
        iris_event_outbox_stats_t stats;
        iris_event_outbox_t *outbox;
        size_t i;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        for (i = 0u; i < 3u; ++i) {
            char event_id[32];
            ivr_media_event_t event;
            snprintf(event_id, sizeof(event_id), "event-batch-%u",
                     (unsigned)i);
            event = test_event(event_id, "{}");
            check_equal(iris_event_outbox_on_media_event(outbox, &event),
                         IVR_OK);
            iris_event_outbox_on_delivery_result(
                outbox, &event, delivery.last_revision, 0, 503);
        }
        memset(&result, 0, sizeof(result));
        check_equal(iris_event_outbox_replay_dead_letters(
                         outbox, 2u, &result),
                     IVR_OK);
        check_equal(result.selected, 2u);
        check_equal(result.replayed, 2u);
        check_equal(result.remaining_dead, 1u);
        check_false(result.backpressured);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.dead_records, 1u);
        check_equal(stats.in_flight_records, 2u);
        check_equal(stats.replayed_total, 2u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("rejects batch replay limits outside the public bound") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_replay_batch_result_t result;
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_replay_dead_letters(
                         outbox, 0u, &result),
                     IVR_EINVAL);
        check_equal(iris_event_outbox_replay_dead_letters(
                         outbox, IRIS_EVENT_OUTBOX_BATCH_REPLAY_MAX + 1u,
                         &result),
                     IVR_EINVAL);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("stops batch replay when the dispatcher applies backpressure") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_replay_batch_result_t result;
        iris_event_outbox_stats_t stats;
        iris_event_outbox_t *outbox;
        size_t i;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        for (i = 0u; i < 2u; ++i) {
            char event_id[32];
            ivr_media_event_t event;
            snprintf(event_id, sizeof(event_id), "event-pressure-%u",
                     (unsigned)i);
            event = test_event(event_id, "{}");
            check_equal(iris_event_outbox_on_media_event(outbox, &event),
                         IVR_OK);
            iris_event_outbox_on_delivery_result(
                outbox, &event, delivery.last_revision, 0, 503);
        }
        delivery.status = IVR_ENOSPC;
        memset(&result, 0, sizeof(result));
        check_equal(iris_event_outbox_replay_dead_letters(
                         outbox, 2u, &result),
                     IVR_OK);
        check_equal(result.selected, 2u);
        check_equal(result.replayed, 1u);
        check_equal(result.remaining_dead, 1u);
        check_true(result.backpressured);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.dead_records, 1u);
        check_equal(stats.pending_records, 1u);
        check_equal(stats.in_flight_records, 0u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("reports a replayed record when later scheduling persistence fails") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_replay_batch_result_t result;
        iris_event_outbox_stats_t stats;
        ivr_media_event_t event = test_event("event-partial-batch", "{}");
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        iris_event_outbox_on_delivery_result(
            outbox, &event, delivery.last_revision, 0, 503);
        store.fail_commit_call = store.commit_calls + 2;
        memset(&result, 0, sizeof(result));
        check_equal(iris_event_outbox_replay_dead_letters(
                         outbox, 1u, &result),
                     IVR_ESTATE);
        check_equal(result.selected, 1u);
        check_equal(result.replayed, 1u);
        check_equal(result.remaining_dead, 0u);
        check_false(result.backpressured);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.dead_records, 0u);
        check_equal(stats.pending_records, 1u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("exports record capacity and counts durable capacity rejection") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_outbox_stats_t stats;
        iris_event_outbox_t *outbox;
        size_t i;
        test_store_init(&store);
        delivery.status = IVR_ENOSPC;
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        for (i = 0u; i < TEST_STORE_CAPACITY; ++i) {
            char event_id[32];
            ivr_media_event_t event;
            snprintf(event_id, sizeof(event_id), "event-capacity-%u",
                     (unsigned)i);
            event = test_event(event_id, "{}");
            check_equal(iris_event_outbox_on_media_event(outbox, &event),
                         IVR_OK);
        }
        {
            ivr_media_event_t rejected =
                test_event("event-capacity-rejected", "{}");
            check_equal(iris_event_outbox_on_media_event(outbox, &rejected),
                         IVR_ENOSPC);
        }
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.record_capacity, TEST_STORE_CAPACITY);
        check_equal(stats.pending_records, TEST_STORE_CAPACITY);
        check_equal(stats.capacity_rejection_total, 1u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("retains rejected delivery and recovers it after restart") {
        test_store_t store;
        test_delivery_t delivery = {0};
        ivr_media_event_t event = test_event("event-d", "{}");
        iris_event_outbox_stats_t stats;
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        delivery.status = IVR_ENOSPC;
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        check_equal(test_store_count(&store), 1u);
        iris_event_outbox_destroy(outbox);

        delivery.status = IVR_OK;
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(delivery.calls, 2);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.retained_payload_bytes,
                      strlen(event.payload_json));
        check_equal(stats.peak_retained_payload_bytes,
                      strlen(event.payload_json));
        iris_event_outbox_on_delivery_result(outbox, &event,
                                             delivery.last_revision, 1, 202);
        check_equal(test_store_count(&store), 0u);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.retained_payload_bytes, 0u);
        check_equal(stats.peak_retained_payload_bytes,
                      strlen(event.payload_json));
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("normalizes an in-flight record after process restart") {
        test_store_t store;
        test_delivery_t delivery = {0};
        ivr_media_event_t event = test_event("event-e", "{}");
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        iris_event_outbox_destroy(outbox);

        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(delivery.calls, 2);
        check_true(delivery.last_revision > 2u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("fails startup when durable recovery cannot scan") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        store.scan_status = SALTS_EIO;
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_EIO);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("rejects unterminated fixed-width media event fields") {
        test_store_t store;
        test_delivery_t delivery = {0};
        ivr_media_event_t event = test_event("event-invalid", "{}");
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        memset(event.event_type, 'x', sizeof(event.event_type));
        check_equal(iris_event_outbox_on_media_event(outbox, &event),
                     IVR_EINVAL);
        check_equal(delivery.calls, 0);
        check_equal(test_store_count(&store), 0u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("archives dead letters before retention deletion") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_outbox_stats_t stats;
        iris_event_retention_result_t result;
        iris_event_archive_t archived[1];
        ivr_media_event_t event = test_event("event-retention", "{\"v\":1}");
        iris_event_outbox_t *outbox;
        size_t count = 0u;
        size_t total = 0u;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        iris_event_outbox_on_delivery_result(outbox, &event,
                                             delivery.last_revision, 0, 503);

        g_test_now_ms = UINT64_C(10999);
        check_equal(iris_event_outbox_run_retention(outbox, &result), IVR_OK);
        check_equal(result.archived, 0u);
        check_equal(result.deleted, 0u);
        check_equal(result.remaining_dead, 1u);

        g_test_now_ms = UINT64_C(11000);
        check_equal(iris_event_outbox_run_retention(outbox, &result), IVR_OK);
        check_equal(result.archived, 1u);
        check_equal(result.deleted, 0u);
        check_equal(result.remaining_dead, 0u);
        check_equal(result.remaining_archived, 1u);
        check_equal(iris_event_outbox_list_archived(
                         outbox, archived, 1u, &count, &total),
                     IVR_OK);
        check_equal(count, 1u);
        check_equal(total, 1u);
        check_equal(archived[0].event_id, event.event_id);
        check_equal(archived[0].archived_at_ms, UINT64_C(11000));
        check_equal(iris_event_outbox_replay(outbox, event.event_id),
                     IVR_EBUSY);

        g_test_now_ms = UINT64_C(11999);
        check_equal(iris_event_outbox_run_retention(outbox, &result), IVR_OK);
        check_equal(result.deleted, 0u);
        check_equal(test_store_count(&store), 1u);
        g_test_now_ms = UINT64_C(12000);
        check_equal(iris_event_outbox_run_retention(outbox, &result), IVR_OK);
        check_equal(result.deleted, 1u);
        check_equal(result.remaining_archived, 0u);
        check_equal(test_store_count(&store), 0u);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.archived_total, 1u);
        check_equal(stats.archive_deleted_total, 1u);
        check_equal(stats.retention_failure_total, 0u);
        check_equal(stats.retained_payload_bytes, 0u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("retains dead state when an archive CAS fails") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_outbox_stats_t stats;
        iris_event_retention_result_t result;
        ivr_media_event_t event = test_event("event-retention-fail", "{}");
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        iris_event_outbox_on_delivery_result(outbox, &event,
                                             delivery.last_revision, 0, 503);
        g_test_now_ms = UINT64_C(11000);
        store.fail_commit_call = store.commit_calls + 1;
        check_equal(iris_event_outbox_run_retention(outbox, &result),
                     IVR_ESTATE);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.dead_records, 1u);
        check_equal(stats.archived_records, 0u);
        check_equal(stats.retention_failure_total, 1u);
        store.fail_commit_call = 0;
        check_equal(iris_event_outbox_run_retention(outbox, &result), IVR_OK);
        check_equal(result.archived, 1u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("recovers archive and deletion retention across restarts") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_outbox_stats_t stats;
        ivr_media_event_t event = test_event("event-retention-restart", "{}");
        iris_event_outbox_t *outbox;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        check_equal(iris_event_outbox_on_media_event(outbox, &event), IVR_OK);
        iris_event_outbox_on_delivery_result(outbox, &event,
                                             delivery.last_revision, 0, 503);
        iris_event_outbox_destroy(outbox);

        g_test_now_ms = UINT64_C(11000);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.dead_records, 0u);
        check_equal(stats.archived_records, 1u);
        iris_event_outbox_destroy(outbox);

        g_test_now_ms = UINT64_C(12000);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        iris_event_outbox_get_stats(outbox, &stats);
        check_equal(stats.archived_records, 0u);
        check_equal(test_store_count(&store), 0u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

    it("limits each retention sweep to the configured batch") {
        test_store_t store;
        test_delivery_t delivery = {0};
        iris_event_retention_result_t result;
        iris_event_outbox_t *outbox;
        size_t i;
        test_store_init(&store);
        outbox = test_create_outbox(&store, &delivery);
        check_not_null(outbox);
        check_equal(iris_event_outbox_start(outbox), SALTS_OK);
        for (i = 0u; i < 3u; ++i) {
            char event_id[32];
            ivr_media_event_t event;
            snprintf(event_id, sizeof(event_id), "event-retention-%u",
                     (unsigned)i);
            event = test_event(event_id, "{}");
            check_equal(iris_event_outbox_on_media_event(outbox, &event),
                         IVR_OK);
            iris_event_outbox_on_delivery_result(
                outbox, &event, delivery.last_revision, 0, 503);
        }
        g_test_now_ms = UINT64_C(11000);
        check_equal(iris_event_outbox_run_retention(outbox, &result), IVR_OK);
        check_equal(result.archived, 2u);
        check_equal(result.remaining_dead, 1u);
        check_equal(result.remaining_archived, 2u);
        check_equal(iris_event_outbox_run_retention(outbox, &result), IVR_OK);
        check_equal(result.archived, 1u);
        check_equal(result.remaining_dead, 0u);
        check_equal(result.remaining_archived, 3u);
        iris_event_outbox_destroy(outbox);
        test_store_cleanup(&store);
    }

}
