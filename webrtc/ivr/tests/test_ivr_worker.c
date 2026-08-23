#include "ivr/ivr_worker.h"
#include "ivr_internal.h"
#include "tinytest.h"

#include <string.h>

typedef struct {
    int create_calls;
    int destroy_calls;
    int start_calls;
    int stop_calls;
    int play_calls;
    int begin_input_calls;
    int end_input_calls;
    int cancel_calls;
    int event_calls;
    uint64_t now_ms;
} mock_media_t;

static mock_media_t g_media;
static ivr_worker_t *g_worker;
static ivr_call_ref_t g_call;

static ivr_status_t mock_start(void *context, const ivr_call_ref_t *call) {
    mock_media_t *mock = (mock_media_t *)context;
    (void)call;
    ++mock->start_calls;
    return IVR_OK;
}

static ivr_status_t mock_play(void *context, const ivr_call_ref_t *call,
                              const ivr_bytes_view_t *text) {
    mock_media_t *mock = (mock_media_t *)context;
    (void)call;
    (void)text;
    ++mock->play_calls;
    return IVR_OK;
}

static ivr_status_t mock_cancel(void *context, const ivr_call_ref_t *call) {
    mock_media_t *mock = (mock_media_t *)context;
    (void)call;
    ++mock->cancel_calls;
    return IVR_OK;
}

static ivr_status_t mock_stop(void *context, const ivr_call_ref_t *call) {
    mock_media_t *mock = (mock_media_t *)context;
    (void)call;
    ++mock->stop_calls;
    return IVR_OK;
}

static ivr_status_t mock_begin_input(void *context,
                                     const ivr_call_ref_t *call,
                                     const ivr_bytes_view_t *input_id,
                                     uint64_t input_generation) {
    mock_media_t *mock = (mock_media_t *)context;
    (void)call;
    (void)input_id;
    (void)input_generation;
    ++mock->begin_input_calls;
    return IVR_OK;
}

static ivr_status_t mock_end_input(void *context,
                                   const ivr_call_ref_t *call,
                                   const ivr_bytes_view_t *input_id,
                                   uint64_t input_generation) {
    mock_media_t *mock = (mock_media_t *)context;
    (void)call;
    (void)input_id;
    (void)input_generation;
    ++mock->end_input_calls;
    return IVR_OK;
}

static ivr_status_t mock_create(void *context, const ivr_call_ref_t *call,
                                ivr_media_port_ops_t *out_media,
                                void **out_instance) {
    mock_media_t *mock = (mock_media_t *)context;
    (void)call;
    memset(out_media, 0, sizeof(*out_media));
    out_media->abi_version = IVR_WORKER_ABI_VERSION;
    out_media->context = mock;
    out_media->start_bot = mock_start;
    out_media->play_pcm = mock_play;
    out_media->cancel_input = mock_cancel;
    out_media->stop_bot = mock_stop;
    out_media->begin_input = mock_begin_input;
    out_media->end_input = mock_end_input;
    *out_instance = mock;
    ++mock->create_calls;
    return IVR_OK;
}

static void mock_destroy(void *context, void *instance) {
    mock_media_t *mock = (mock_media_t *)context;
    (void)instance;
    ++mock->destroy_calls;
}

static ivr_status_t mock_publish(void *context,
                                 const ivr_event_view_t *event) {
    mock_media_t *mock = (mock_media_t *)context;
    (void)event;
    ++mock->event_calls;
    return IVR_OK;
}

static uint64_t mock_now(void *context) {
    return ((mock_media_t *)context)->now_ms;
}

static ivr_call_ref_t make_call(const char *call_id) {
    ivr_call_ref_t call;
    memset(&call, 0, sizeof(call));
    call.provider_session_id.data = call_id;
    call.provider_session_id.size = strlen(call_id);
    call.dialog_id.data = call_id;
    call.dialog_id.size = strlen(call_id);
    call.room_id.data = "room-42";
    call.room_id.size = strlen(call.room_id.data);
    call.call_id.data = call_id;
    call.call_id.size = strlen(call_id);
    call.call_generation = 1;
    call.expected_room_version = 9;
    return call;
}

static ivr_status_t create_worker(uint32_t capacity) {
    ivr_worker_config_t config;
    ivr_media_event_sink_ops_t sink;
    ivr_media_port_factory_ops_t factory;

    memset(&config, 0, sizeof(config));
    config.abi_version = IVR_WORKER_ABI_VERSION;
    config.worker_id = "media-worker-test";
    config.worker_instance_id = "media-worker-instance";
    config.worker_epoch = 7;
    config.max_sessions_per_worker = capacity;
    config.now_ms = mock_now;
    config.now_context = &g_media;

    memset(&sink, 0, sizeof(sink));
    sink.abi_version = IVR_WORKER_ABI_VERSION;
    sink.context = &g_media;
    sink.publish_copy = mock_publish;

    memset(&factory, 0, sizeof(factory));
    factory.abi_version = IVR_WORKER_ABI_VERSION;
    factory.context = &g_media;
    factory.create = mock_create;
    factory.destroy = mock_destroy;
    return ivr_worker_create(&config, &sink, &factory, &g_worker);
}

void setUp(void) {
    memset(&g_media, 0, sizeof(g_media));
    g_media.now_ms = 100;
    g_worker = NULL;
    g_call = make_call("call-42");
}

void tearDown(void) {
    if (g_worker) {
        (void)ivr_worker_destroy(g_worker);
        g_worker = NULL;
    }
}

void test_open_is_bounded_and_idempotent(void) {
    ivr_call_ref_t second = make_call("call-43");
    check_equal(create_worker(1), IVR_OK);
    check_equal(ivr_worker_start(g_worker), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &g_call), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &g_call), IVR_OK);
    check_equal((int)(g_media.create_calls), (int)(1));
    check_equal((int)(g_media.start_calls), (int)(1));
    check_equal((uint32_t)(ivr_worker_active_sessions(g_worker)), (uint32_t)(1));
    check_equal(ivr_worker_open_media_call(g_worker, &second), IVR_ENOSPC);
}

void test_typed_open_records_generation_for_restart_inventory(void) {
    ivr_media_operation_t operation;
    ivr_worker_inventory_query_t query;
    ivr_worker_inventory_page_t page;

    memset(&operation, 0, sizeof(operation));
    operation.call = g_call;
    operation.operation_generation = 7u;
    memset(&query, 0, sizeof(query));
    query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    query.limit = 1u;

    check_equal(create_worker(1), IVR_OK);
    check_equal(ivr_worker_start(g_worker), IVR_OK);
    check_equal(ivr_worker_open_media_operation(g_worker, &operation), IVR_OK);
    check_equal(ivr_worker_open_media_operation(g_worker, &operation), IVR_OK);
    check_equal((int)(g_media.create_calls), (int)(1));
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_OK);
    check_equal((uint32_t)(page.count), (uint32_t)(1));
    check_equal((uint64_t)(page.records[0].operation_generation), (uint64_t)(7));

    operation.operation_generation = 6u;
    check_equal(ivr_worker_open_media_operation(g_worker, &operation), IVR_ESTALE);
    operation.operation_generation = 8u;
    check_equal(ivr_worker_open_media_operation(g_worker, &operation), IVR_ESTATE);
}

void test_operations_are_explicit_and_idempotent(void) {
    ivr_media_operation_t operation;
    ivr_bytes_view_t text = {"hello", 5};
    memset(&operation, 0, sizeof(operation));
    operation.call = g_call;
    operation.operation_generation = 1;

    check_equal(create_worker(1), IVR_OK);
    check_equal(ivr_worker_start(g_worker), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &g_call), IVR_OK);
    check_equal(ivr_worker_play(g_worker, &operation, &text), IVR_OK);
    check_equal(ivr_worker_play(g_worker, &operation, &text), IVR_OK);
    check_equal((int)(g_media.play_calls), (int)(1));

    operation.operation_generation = 2;
    operation.deadline_ms = 99;
    check_equal(ivr_worker_play(g_worker, &operation, &text), IVR_ESTALE);
    check_equal((int)(g_media.play_calls), (int)(1));
}

void test_input_window_rejects_stale_completion(void) {
    ivr_media_operation_t operation;
    ivr_bytes_view_t input_id = {"input-1", 7};
    ivr_bytes_view_t stale_id = {"input-old", 9};
    memset(&operation, 0, sizeof(operation));
    operation.call = g_call;
    operation.operation_generation = 1;

    check_equal(create_worker(1), IVR_OK);
    check_equal(ivr_worker_start(g_worker), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &g_call), IVR_OK);
    check_equal(ivr_worker_begin_input(
                                  g_worker, &operation, &input_id, 7), IVR_OK);
    operation.operation_generation = 2;
    check_equal(ivr_worker_end_input(
                                     g_worker, &operation, &stale_id, 7), IVR_ESTALE);
    check_equal((int)(g_media.end_input_calls), (int)(0));
    check_equal(ivr_worker_end_input(
                                  g_worker, &operation, &input_id, 7), IVR_OK);
    check_equal((int)(g_media.end_input_calls), (int)(1));
}

void test_cancel_is_fenced_to_the_exact_active_input(void) {
    ivr_media_operation_t operation;
    ivr_bytes_view_t input_id = {"input-1", 7};
    ivr_bytes_view_t stale_id = {"input-old", 9};
    ivr_worker_inventory_query_t query;
    ivr_worker_inventory_page_t page;
    memset(&operation, 0, sizeof(operation));
    memset(&query, 0, sizeof(query));
    operation.call = g_call;
    operation.operation_generation = 1;
    query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    query.limit = 1;

    check_equal(create_worker(1), IVR_OK);
    check_equal(ivr_worker_start(g_worker), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &g_call), IVR_OK);
    check_equal(ivr_worker_begin_input(
                                  g_worker, &operation, &input_id, 7), IVR_OK);
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_OK);
    check_equal((uint32_t)(page.count), (uint32_t)(1));
    check_true(page.records[0].input_active);
    check_equal(page.records[0].input_id, "input-1");
    check_equal((uint64_t)(page.records[0].input_generation), (uint64_t)(7));

    operation.operation_generation = 2;
    check_equal(ivr_worker_cancel_input(
                                     g_worker, &operation, &stale_id, 7), IVR_ESTALE);
    check_equal(ivr_worker_cancel_input(
                                     g_worker, &operation, &input_id, 8), IVR_ESTALE);
    check_equal((int)(g_media.cancel_calls), (int)(0));

    check_equal(ivr_worker_cancel_input(
                                  g_worker, &operation, &input_id, 7), IVR_OK);
    check_equal((int)(g_media.cancel_calls), (int)(1));
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_OK);
    check_false(page.records[0].input_active);
    check_equal(page.records[0].input_id, "");
    check_equal((uint64_t)(page.records[0].input_generation), (uint64_t)(0));

    operation.operation_generation = 3;
    check_equal(ivr_worker_cancel_input(
                                     g_worker, &operation, &input_id, 7), IVR_ESTALE);
    check_equal((int)(g_media.cancel_calls), (int)(1));
}

void test_media_event_is_forwarded_without_workflow_processing(void) {
    ivr_event_view_t event;
    memset(&event, 0, sizeof(event));
    event.event_type.data = "asr.final";
    event.event_type.size = 9;
    event.call = g_call;
    event.input_id.data = "input-1";
    event.input_id.size = 7;
    event.input_value.data = "sales";
    event.input_value.size = 5;

    check_equal(create_worker(1), IVR_OK);
    check_equal(ivr_worker_start(g_worker), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &g_call), IVR_OK);
    check_equal(ivr_worker_publish_event_copy(g_worker, &event), IVR_OK);
    check_equal((int)(g_media.event_calls), (int)(1));
}

void test_close_and_drain_release_media_slots(void) {
    ivr_call_ref_t second = make_call("call-43");
    check_equal(create_worker(2), IVR_OK);
    check_equal(ivr_worker_start(g_worker), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &g_call), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &second), IVR_OK);
    check_equal(ivr_worker_close_media_call(g_worker, &g_call), IVR_OK);
    check_equal(ivr_worker_close_media_call(g_worker, &g_call), IVR_OK);
    check_equal((uint32_t)(ivr_worker_active_sessions(g_worker)), (uint32_t)(1));
    check_equal(ivr_worker_begin_drain(g_worker), IVR_OK);
    check_equal((uint32_t)(ivr_worker_active_sessions(g_worker)), (uint32_t)(0));
    check_equal((int)(g_media.stop_calls), (int)(2));
    check_equal((int)(g_media.destroy_calls), (int)(2));
    check_equal(ivr_worker_open_media_call(g_worker, &g_call), IVR_ECLOSED);
}

void test_old_abi_is_rejected(void) {
    ivr_worker_config_t config;
    ivr_media_event_sink_ops_t sink;
    ivr_media_port_factory_ops_t factory;
    memset(&config, 0, sizeof(config));
    memset(&sink, 0, sizeof(sink));
    memset(&factory, 0, sizeof(factory));
    config.abi_version = IVR_WORKER_ABI_VERSION - 1;
    config.worker_id = "old-worker";
    config.worker_instance_id = "old-worker-instance";
    config.worker_epoch = 1;
    config.max_sessions_per_worker = 1;
    sink.abi_version = IVR_WORKER_ABI_VERSION;
    sink.publish_copy = mock_publish;
    factory.abi_version = IVR_WORKER_ABI_VERSION;
    factory.create = mock_create;
    factory.destroy = mock_destroy;
    check_equal(ivr_worker_create(&config, &sink, &factory, &g_worker), IVR_EVERSION);
    check_null(g_worker);
}

void test_inventory_is_owned_versioned_and_bounded(void) {
    ivr_call_ref_t second = make_call("call-43");
    ivr_call_ref_t third = make_call("call-44");
    ivr_worker_inventory_query_t query;
    ivr_worker_inventory_page_t page;
    uint64_t revision;
    uint32_t next_cursor;

    check_equal(create_worker(3), IVR_OK);
    check_equal(ivr_worker_start(g_worker), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &g_call), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &second), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &third), IVR_OK);

    memset(&query, 0, sizeof(query));
    query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    query.limit = 2;
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_OK);
    check_equal((uint32_t)(page.count), (uint32_t)(2));
    check_equal((uint32_t)(page.total_active), (uint32_t)(3));
    check_true(page.has_more);
    check_true(page.next_cursor > 0);
    check_equal(page.records[0].worker_id, "media-worker-test");
    check_equal(page.records[0].worker_instance_id, "media-worker-instance");
    check_equal((uint64_t)(page.records[0].worker_epoch), (uint64_t)(7));
    check_equal((uint64_t)(page.records[0].operation_generation), (uint64_t)(0));
    check_equal(page.records[0].provider_session_id, "call-42");
    check_equal(page.records[0].dialog_id, "call-42");
    check_equal(page.records[0].room_id, "room-42");
    check_equal(page.records[0].call_id, "call-42");
    check_equal((uint64_t)(page.records[0].call_generation), (uint64_t)(1));
    check_equal((int)(page.records[0].state), (int)(IVR_WORKER_RESOURCE_ACTIVE));
    check_true(page.records[0].rebindable);
    revision = page.revision;
    next_cursor = page.next_cursor;

    /* The page owns its strings; closing the worker slot does not mutate it. */
    check_equal(ivr_worker_close_media_call(g_worker, &g_call), IVR_OK);
    check_equal(page.records[0].call_id, "call-42");

    query.expected_revision = revision;
    query.cursor = next_cursor;
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_ESTALE);
    check_equal((uint32_t)(page.count), (uint32_t)(0));

    query.expected_revision = 0;
    query.cursor = 0;
    query.limit = IVR_WORKER_INVENTORY_MAX_PAGE_SIZE;
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_OK);
    check_equal((uint32_t)(page.count), (uint32_t)(2));
    check_equal((uint32_t)(page.total_active), (uint32_t)(2));
    check_false(page.has_more);
    check_equal((uint32_t)(page.next_cursor), (uint32_t)(0));
}

void test_inventory_rejects_unknown_version_limit_and_cursor(void) {
    ivr_worker_inventory_query_t query;
    ivr_worker_inventory_page_t page;

    check_equal(create_worker(1), IVR_OK);
    check_equal(ivr_worker_start(g_worker), IVR_OK);
    memset(&query, 0, sizeof(query));
    query.inventory_version = IVR_WORKER_INVENTORY_VERSION + 1u;
    query.limit = 1;
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_EVERSION);
    query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    query.limit = 0;
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_EINVAL);
    query.limit = IVR_WORKER_INVENTORY_MAX_PAGE_SIZE + 1u;
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_EINVAL);
    query.limit = 1;
    query.cursor = 2;
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_EINVAL);
}

void test_reconnect_epoch_invalidates_inventory_and_preserves_media(void) {
    ivr_worker_inventory_query_t query;
    ivr_worker_inventory_page_t page;
    uint64_t old_revision;

    check_equal(create_worker(1), IVR_OK);
    check_equal(ivr_worker_start(g_worker), IVR_OK);
    check_equal(ivr_worker_open_media_call(g_worker, &g_call), IVR_OK);
    memset(&query, 0, sizeof(query));
    query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    query.limit = 1;
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_OK);
    old_revision = page.revision;
    check_equal((uint64_t)(page.records[0].worker_epoch), (uint64_t)(7));

    check_equal(ivr_worker_advance_epoch(g_worker, 0), IVR_EINVAL);
    check_equal(ivr_worker_advance_epoch(g_worker, 7), IVR_ESTALE);
    check_equal(ivr_worker_advance_epoch(g_worker, 8), IVR_OK);
    check_equal((uint32_t)(ivr_worker_active_sessions(g_worker)), (uint32_t)(1));

    query.expected_revision = old_revision;
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_ESTALE);
    query.expected_revision = 0;
    check_equal(ivr_worker_query_inventory(g_worker, &query, &page), IVR_OK);
    check_equal((uint32_t)(page.count), (uint32_t)(1));
    check_equal((uint64_t)(page.records[0].worker_epoch), (uint64_t)(8));
    check_true(page.revision > old_revision);
}

spec("test_ivr_worker") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  it("test_open_is_bounded_and_idempotent") { test_open_is_bounded_and_idempotent(); };
  it("test_typed_open_records_generation_for_restart_inventory") { test_typed_open_records_generation_for_restart_inventory(); };
  it("test_operations_are_explicit_and_idempotent") { test_operations_are_explicit_and_idempotent(); };
  it("test_input_window_rejects_stale_completion") { test_input_window_rejects_stale_completion(); };
  it("test_cancel_is_fenced_to_the_exact_active_input") { test_cancel_is_fenced_to_the_exact_active_input(); };
  it("test_media_event_is_forwarded_without_workflow_processing") { test_media_event_is_forwarded_without_workflow_processing(); };
  it("test_close_and_drain_release_media_slots") { test_close_and_drain_release_media_slots(); };
  it("test_old_abi_is_rejected") { test_old_abi_is_rejected(); };
  it("test_inventory_is_owned_versioned_and_bounded") { test_inventory_is_owned_versioned_and_bounded(); };
  it("test_inventory_rejects_unknown_version_limit_and_cursor") { test_inventory_rejects_unknown_version_limit_and_cursor(); };
  it("test_reconnect_epoch_invalidates_inventory_and_preserves_media") { test_reconnect_epoch_invalidates_inventory_and_preserves_media(); };
}
