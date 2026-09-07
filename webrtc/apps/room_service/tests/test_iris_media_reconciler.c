#include "iris_media_reconciler.h"

#include <tinytest.h>
#include <platform.h>
#include <salts_uuid.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct reconcile_fixture_s {
    iris_media_reconciler_t *reconciler;
    iris_expected_media_resource_t expected[4];
    size_t expected_count;
    ivr_control_worker_snapshot_t worker;
    int has_worker;
    ivr_worker_inventory_envelope_t inventory;
    int rebind_calls;
    int close_calls;
    int preserve_inventory_on_close;
    char close_message_ids[2][SALTS_UUID_STRING_SIZE];
    int complete_calls;
    int lost_calls;
    iris_expected_media_resource_t lost_resource;
    char lost_reason[64];
    atomic_int fetch_calls;
    int fetch_failures_remaining;
    int fetch_always_fails;
} reconcile_fixture_t;

static void reset_fixture(reconcile_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    atomic_init(&fixture->fetch_calls, 0);
}

static ivr_status_t mock_fetch_expected(
    void *context, iris_expected_media_resource_t *resources,
    size_t capacity, size_t *out_count) {
    reconcile_fixture_t *fixture = (reconcile_fixture_t *)context;
    atomic_fetch_add(&fixture->fetch_calls, 1);
    if (fixture->fetch_always_fails) return IVR_ESTATE;
    if (fixture->fetch_failures_remaining > 0) {
        --fixture->fetch_failures_remaining;
        return IVR_ESTATE;
    }
    if (fixture->expected_count > capacity) return IVR_ENOSPC;
    memcpy(resources, fixture->expected,
           fixture->expected_count * sizeof(*resources));
    *out_count = fixture->expected_count;
    return IVR_OK;
}

static ivr_status_t mock_list_workers(
    void *context, ivr_control_worker_snapshot_t *workers, uint32_t capacity,
    uint32_t *out_count, uint32_t *out_total) {
    reconcile_fixture_t *fixture = (reconcile_fixture_t *)context;
    *out_count = 0;
    *out_total = fixture->has_worker ? 1u : 0u;
    if (!fixture->has_worker) return IVR_OK;
    if (capacity < 1u) return IVR_ENOSPC;
    workers[0] = fixture->worker;
    *out_count = 1u;
    return IVR_OK;
}

static ivr_status_t mock_request_inventory(
    void *context, const ivr_worker_inventory_request_t *request) {
    reconcile_fixture_t *fixture = (reconcile_fixture_t *)context;
    ivr_worker_inventory_envelope_t page = fixture->inventory;
    snprintf(page.message_id, sizeof(page.message_id), "%s",
             request->message_id);
    snprintf(page.worker_id, sizeof(page.worker_id), "%s",
             request->worker_id);
    page.page.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    page.page.cursor = request->query.cursor;
    if (page.page.revision == 0u) page.page.revision = 1u;
    return iris_media_reconciler_on_inventory_page(fixture->reconciler,
                                                    &page);
}

static ivr_status_t mock_rebind(
    void *context, const ivr_worker_inventory_record_t *record) {
    reconcile_fixture_t *fixture = (reconcile_fixture_t *)context;
    (void)record;
    ++fixture->rebind_calls;
    return IVR_OK;
}

static ivr_status_t mock_close(
    void *context, const ivr_worker_inventory_record_t *record,
    const char *message_id, uint64_t deadline_timeout_ms) {
    reconcile_fixture_t *fixture = (reconcile_fixture_t *)context;
    (void)record;
    if (!message_id || !message_id[0] || deadline_timeout_ms == 0u) {
        return IVR_EINVAL;
    }
    if (fixture->close_calls < 2) {
        snprintf(fixture->close_message_ids[fixture->close_calls],
                 sizeof(fixture->close_message_ids[fixture->close_calls]),
                 "%s", message_id);
    }
    ++fixture->close_calls;
    if (!fixture->preserve_inventory_on_close) {
        fixture->inventory.page.count = 0u;
        fixture->inventory.page.total_active = 0u;
        fixture->worker.active_sessions = 0u;
    }
    return IVR_OK;
}

static ivr_status_t mock_complete(
    void *context, const char *worker_id, const char *worker_instance_id,
    uint64_t worker_epoch) {
    reconcile_fixture_t *fixture = (reconcile_fixture_t *)context;
    if (strcmp(worker_id, fixture->worker.worker_id) != 0 ||
        strcmp(worker_instance_id, fixture->worker.instance_id) != 0 ||
        worker_epoch != fixture->worker.connection_generation ||
        fixture->worker.active_sessions != fixture->inventory.page.count) {
        return IVR_EVERSION;
    }
    ++fixture->complete_calls;
    fixture->worker.requires_reconcile = 0;
    fixture->worker.state = IVR_CONTROL_WORKER_READY;
    return IVR_OK;
}

static int mock_required(void *context) {
    reconcile_fixture_t *fixture = (reconcile_fixture_t *)context;
    return fixture->has_worker && fixture->worker.requires_reconcile;
}

static ivr_status_t mock_submit_lost(
    void *context, const iris_expected_media_resource_t *resource,
    const char *reason) {
    reconcile_fixture_t *fixture = (reconcile_fixture_t *)context;
    ++fixture->lost_calls;
    fixture->lost_resource = *resource;
    snprintf(fixture->lost_reason, sizeof(fixture->lost_reason), "%s",
             reason);
    return IVR_OK;
}

static iris_media_reconciler_t *create_reconciler(
    reconcile_fixture_t *fixture, uint32_t queue_capacity) {
    iris_media_reconciler_config_t config;
    memset(&config, 0, sizeof(config));
    config.resource_capacity = 4u;
    config.worker_capacity = 2u;
    config.inventory_queue_capacity = queue_capacity;
    config.retry_max_attempts = 2u;
    config.retry_backoff_ms = 1u;
    config.request_timeout_ms = 20u;
    config.drain_timeout_ms = 20u;
    config.inventory_page_size = 2u;
    config.close_deadline_ms = 100u;
    config.ops.context = fixture;
    config.ops.fetch_expected = mock_fetch_expected;
    config.ops.list_workers = mock_list_workers;
    config.ops.request_inventory = mock_request_inventory;
    config.ops.rebind_dialog = mock_rebind;
    config.ops.close_orphan = mock_close;
    config.ops.complete_worker = mock_complete;
    config.ops.reconcile_required = mock_required;
    config.ops.submit_resource_lost = mock_submit_lost;
    fixture->reconciler = iris_media_reconciler_create(&config);
    return fixture->reconciler;
}

static int wait_until_accepting(reconcile_fixture_t *fixture,
                                uint32_t timeout_ms) {
    uint32_t elapsed_ms = 0u;
    while (elapsed_ms < timeout_ms) {
        if (iris_media_reconciler_accepting_commands(fixture->reconciler)) {
            return 1;
        }
        salts_sleep_ms(1u);
        ++elapsed_ms;
    }
    return iris_media_reconciler_accepting_commands(fixture->reconciler);
}

static int wait_for_fetch_calls(reconcile_fixture_t *fixture, int expected,
                                uint32_t timeout_ms) {
    uint32_t elapsed_ms = 0u;
    while (elapsed_ms < timeout_ms) {
        if (atomic_load(&fixture->fetch_calls) >= expected) return 1;
        salts_sleep_ms(1u);
        ++elapsed_ms;
    }
    return atomic_load(&fixture->fetch_calls) >= expected;
}

static void make_expected(iris_expected_media_resource_t *resource,
                          iris_expected_command_state_t command_state) {
    memset(resource, 0, sizeof(*resource));
    snprintf(resource->tenant_id, sizeof(resource->tenant_id), "tenant-a");
    snprintf(resource->provider_id, sizeof(resource->provider_id),
             IRIS_MEDIA_RECONCILE_PROVIDER_ID);
    snprintf(resource->provider_session_id,
             sizeof(resource->provider_session_id), "session-a");
    resource->session_revision = 7u;
    snprintf(resource->owner_node_id, sizeof(resource->owner_node_id),
             "iris-a");
    resource->owner_epoch = 3u;
    resource->owner_lease_expires_at_ms = 10000u;
    snprintf(resource->dialog_id, sizeof(resource->dialog_id), "dialog-a");
    snprintf(resource->room_id, sizeof(resource->room_id), "room-a");
    snprintf(resource->call_id, sizeof(resource->call_id), "call-a");
    resource->call_generation = 2u;
    resource->operation_generation = 5u;
    resource->state = IRIS_EXPECTED_MEDIA_ACTIVE;
    snprintf(resource->active_command_id,
             sizeof(resource->active_command_id), "command-a");
    resource->active_command_state = command_state;
    if (command_state == IRIS_EXPECTED_COMMAND_DISPATCHED) {
        snprintf(resource->dispatch_worker_id,
                 sizeof(resource->dispatch_worker_id), "iris-provider-1");
        resource->dispatch_epoch = 4u;
        resource->dispatch_lease_expires_at_ms = 12000u;
    }
}

static void make_worker_inventory(reconcile_fixture_t *fixture,
                                  uint64_t operation_generation) {
    ivr_worker_inventory_record_t *record;
    memset(&fixture->worker, 0, sizeof(fixture->worker));
    snprintf(fixture->worker.worker_id, sizeof(fixture->worker.worker_id),
             "worker-a");
    snprintf(fixture->worker.instance_id, sizeof(fixture->worker.instance_id),
             "instance-a");
    fixture->worker.connection_generation = 9u;
    fixture->worker.state = IVR_CONTROL_WORKER_RECONCILING;
    fixture->worker.max_sessions = 4u;
    fixture->worker.active_sessions = 1u;
    fixture->worker.requires_reconcile = 1;
    fixture->has_worker = 1;

    memset(&fixture->inventory, 0, sizeof(fixture->inventory));
    fixture->inventory.status_code = IVR_OK;
    fixture->inventory.page.revision = 11u;
    fixture->inventory.page.total_active = 1u;
    fixture->inventory.page.count = 1u;
    record = &fixture->inventory.page.records[0];
    snprintf(record->worker_id, sizeof(record->worker_id), "worker-a");
    snprintf(record->worker_instance_id, sizeof(record->worker_instance_id),
             "instance-a");
    record->worker_epoch = 9u;
    snprintf(record->tenant_id, sizeof(record->tenant_id), "tenant-a");
    snprintf(record->provider_session_id,
             sizeof(record->provider_session_id), "session-a");
    snprintf(record->dialog_id, sizeof(record->dialog_id), "dialog-a");
    snprintf(record->room_id, sizeof(record->room_id), "room-a");
    snprintf(record->call_id, sizeof(record->call_id), "call-a");
    record->call_generation = 2u;
    record->operation_generation = operation_generation;
    record->state = IVR_WORKER_RESOURCE_ACTIVE;
    record->rebindable = 1;
}

spec("iris_media_reconciler") {
    it("becomes ready when no durable or media resources exist") {
        reconcile_fixture_t fixture;
        iris_media_reconciler_stats_t stats;
        reset_fixture(&fixture);
        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 0), IVR_OK);
        check_true(iris_media_reconciler_accepting_commands(
            fixture.reconciler));
        iris_media_reconciler_get_stats(fixture.reconciler, &stats);
        check_equal(stats.state, IRIS_MEDIA_RECONCILE_READY);
        iris_media_reconciler_destroy(fixture.reconciler);
    }

    it("rebinds an exact dispatched active resource and promotes its worker") {
        reconcile_fixture_t fixture;
        reset_fixture(&fixture);
        make_expected(&fixture.expected[0],
                      IRIS_EXPECTED_COMMAND_DISPATCHED);
        fixture.expected_count = 1u;
        make_worker_inventory(&fixture, 5u);
        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 0), IVR_OK);
        check_equal(fixture.rebind_calls, 1);
        check_equal(fixture.close_calls, 0);
        check_equal(fixture.complete_calls, 1);
        check_true(iris_media_reconciler_accepting_commands(
            fixture.reconciler));
        iris_media_reconciler_destroy(fixture.reconciler);
    }

    it("rebinds an active resource before its next pending operation") {
        reconcile_fixture_t fixture;
        reset_fixture(&fixture);
        make_expected(&fixture.expected[0], IRIS_EXPECTED_COMMAND_PENDING);
        fixture.expected_count = 1u;
        make_worker_inventory(&fixture, 4u);
        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 0), IVR_OK);
        check_equal(fixture.rebind_calls, 1);
        check_equal(fixture.complete_calls, 1);
        check_equal(fixture.lost_calls, 0);
        iris_media_reconciler_destroy(fixture.reconciler);
    }

    it("closes an orphan and confirms an empty inventory before ready") {
        reconcile_fixture_t fixture;
        reset_fixture(&fixture);
        make_worker_inventory(&fixture, 8u);
        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 0), IVR_EBUSY);
        check_equal(fixture.close_calls, 1);
        check_equal(fixture.complete_calls, 0);
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 0), IVR_OK);
        check_equal(fixture.complete_calls, 1);
        iris_media_reconciler_destroy(fixture.reconciler);
    }

    it("reuses the orphan close id across a restart before inventory settles") {
        reconcile_fixture_t fixture;
        reset_fixture(&fixture);
        make_worker_inventory(&fixture, 8u);
        fixture.preserve_inventory_on_close = 1;
        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 0), IVR_EBUSY);
        check_equal(fixture.close_calls, 1);
        iris_media_reconciler_destroy(fixture.reconciler);

        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 0), IVR_EBUSY);
        check_equal(fixture.close_calls, 2);
        check_equal(fixture.close_message_ids[0],
                     fixture.close_message_ids[1]);
        iris_media_reconciler_destroy(fixture.reconciler);
    }

    it("defers a missing dispatched resource then submits one terminal fact") {
        reconcile_fixture_t fixture;
        reset_fixture(&fixture);
        make_expected(&fixture.expected[0],
                      IRIS_EXPECTED_COMMAND_DISPATCHED);
        fixture.expected_count = 1u;
        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 0), IVR_EBUSY);
        check_equal(fixture.lost_calls, 0);
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 1), IVR_EBUSY);
        check_equal(fixture.lost_calls, 1);
        check_equal(fixture.lost_reason, "inventory_missing");
        check_equal(fixture.lost_resource.active_command_id, "command-a");
        iris_media_reconciler_destroy(fixture.reconciler);
    }

    it("admits a dispatched opening command before its media resource exists") {
        reconcile_fixture_t fixture;
        reset_fixture(&fixture);
        make_expected(&fixture.expected[0],
                      IRIS_EXPECTED_COMMAND_DISPATCHED);
        fixture.expected[0].state = IRIS_EXPECTED_MEDIA_OPENING;
        fixture.expected_count = 1u;
        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 0), IVR_OK);
        check_true(iris_media_reconciler_accepting_commands(
            fixture.reconciler));
        check_equal(fixture.lost_calls, 0);
        iris_media_reconciler_destroy(fixture.reconciler);
    }

    it("does not fail a pending command that has no media side effect yet") {
        reconcile_fixture_t fixture;
        reset_fixture(&fixture);
        make_expected(&fixture.expected[0], IRIS_EXPECTED_COMMAND_PENDING);
        fixture.expected_count = 1u;
        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 1), IVR_OK);
        check_equal(fixture.lost_calls, 0);
        iris_media_reconciler_destroy(fixture.reconciler);
    }

    it("builds a restart-stable resource-lost event") {
        iris_expected_media_resource_t resource;
        ivr_media_event_t first;
        ivr_media_event_t second;
        make_expected(&resource, IRIS_EXPECTED_COMMAND_DISPATCHED);
        check_equal(iris_media_reconciler_build_resource_lost_event(
                         &resource, "inventory_missing", &first), IVR_OK);
        check_equal(iris_media_reconciler_build_resource_lost_event(
                         &resource, "generation_or_owner_conflict", &second),
                     IVR_OK);
        check_true(memcmp(&first, &second, sizeof(first)) == 0);
        check_equal(first.event_type, "provider.media.resource_lost");
        check_true(strstr(first.payload_json,
                          "resource_not_recoverable") != NULL);
        check_equal(first.occurred_at_ms, 12000u);
    }

    it("returns explicit backpressure when the inventory queue is full") {
        reconcile_fixture_t fixture;
        ivr_worker_inventory_envelope_t page;
        reset_fixture(&fixture);
        memset(&page, 0, sizeof(page));
        snprintf(page.message_id, sizeof(page.message_id), "page-a");
        check_not_null(create_reconciler(&fixture, 1u));
        check_equal(iris_media_reconciler_on_inventory_page(
                         fixture.reconciler, &page), IVR_OK);
        check_equal(iris_media_reconciler_on_inventory_page(
                         fixture.reconciler, &page), IVR_ENOSPC);
        iris_media_reconciler_destroy(fixture.reconciler);
    }

    it("fail-closes a stale operation generation and emits one stable loss") {
        reconcile_fixture_t fixture;
        reset_fixture(&fixture);
        make_expected(&fixture.expected[0],
                      IRIS_EXPECTED_COMMAND_DISPATCHED);
        fixture.expected_count = 1u;
        make_worker_inventory(&fixture, 4u);
        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 1), IVR_EBUSY);
        check_equal(fixture.rebind_calls, 0);
        check_equal(fixture.close_calls, 1);
        check_equal(fixture.lost_calls, 1);
        check_equal(fixture.lost_reason,
                     "generation_or_owner_conflict");
        check_false(iris_media_reconciler_accepting_commands(
            fixture.reconciler));
        iris_media_reconciler_destroy(fixture.reconciler);
    }

    it("recovers its command gate after a temporary Iris outage") {
        reconcile_fixture_t fixture;
        iris_media_reconciler_stats_t stats;
        reset_fixture(&fixture);
        fixture.fetch_failures_remaining = 2;
        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_start(fixture.reconciler), 0);
        check_true(wait_until_accepting(&fixture, 500u));
        iris_media_reconciler_get_stats(fixture.reconciler, &stats);
        check_greater_equal(atomic_load(&fixture.fetch_calls), 3);
        check_greater_equal(stats.reconcile_failures_total, 2);
        check_equal(stats.state, IRIS_MEDIA_RECONCILE_READY);
        iris_media_reconciler_stop(fixture.reconciler);
        iris_media_reconciler_destroy(fixture.reconciler);
    }

    it("interrupts retry waiting and closes inventory intake on shutdown") {
        reconcile_fixture_t fixture;
        iris_media_reconciler_stats_t stats;
        ivr_worker_inventory_envelope_t page;
        int fetch_calls_after_stop;
        reset_fixture(&fixture);
        memset(&page, 0, sizeof(page));
        fixture.fetch_always_fails = 1;
        check_not_null(create_reconciler(&fixture, 2u));
        check_equal(iris_media_reconciler_start(fixture.reconciler), 0);
        check_true(wait_for_fetch_calls(&fixture, 1, 500u));
        iris_media_reconciler_stop(fixture.reconciler);
        fetch_calls_after_stop = atomic_load(&fixture.fetch_calls);
        salts_sleep_ms(5u);
        check_equal(atomic_load(&fixture.fetch_calls),
                     fetch_calls_after_stop);
        check_equal(iris_media_reconciler_on_inventory_page(
                         fixture.reconciler, &page), IVR_ECLOSED);
        check_equal(iris_media_reconciler_reconcile_once(
                         fixture.reconciler, 0), IVR_ECLOSED);
        check_equal(iris_media_reconciler_start(fixture.reconciler), -1);
        check_false(iris_media_reconciler_accepting_commands(
            fixture.reconciler));
        iris_media_reconciler_get_stats(fixture.reconciler, &stats);
        check_equal(stats.state, IRIS_MEDIA_RECONCILE_DRAINING);
        iris_media_reconciler_destroy(fixture.reconciler);
    }
}
