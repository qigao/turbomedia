#include "ivr/ivr_worker.h"
#include "ivr_internal.h"
#include "tinytest_compat.h"

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
    TEST_ASSERT_EQUAL(IVR_OK, create_worker(1));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL_INT(1, g_media.create_calls);
    TEST_ASSERT_EQUAL_INT(1, g_media.start_calls);
    TEST_ASSERT_EQUAL_UINT32(1, ivr_worker_active_sessions(g_worker));
    TEST_ASSERT_EQUAL(IVR_ENOSPC,
                      ivr_worker_open_media_call(g_worker, &second));
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

    TEST_ASSERT_EQUAL(IVR_OK, create_worker(1));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(
        IVR_OK, ivr_worker_open_media_operation(g_worker, &operation));
    TEST_ASSERT_EQUAL(
        IVR_OK, ivr_worker_open_media_operation(g_worker, &operation));
    TEST_ASSERT_EQUAL_INT(1, g_media.create_calls);
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    TEST_ASSERT_EQUAL_UINT32(1, page.count);
    TEST_ASSERT_EQUAL_UINT64(7, page.records[0].operation_generation);

    operation.operation_generation = 6u;
    TEST_ASSERT_EQUAL(
        IVR_ESTALE, ivr_worker_open_media_operation(g_worker, &operation));
    operation.operation_generation = 8u;
    TEST_ASSERT_EQUAL(
        IVR_ESTATE, ivr_worker_open_media_operation(g_worker, &operation));
}

void test_operations_are_explicit_and_idempotent(void) {
    ivr_media_operation_t operation;
    ivr_bytes_view_t text = {"hello", 5};
    memset(&operation, 0, sizeof(operation));
    operation.call = g_call;
    operation.operation_generation = 1;

    TEST_ASSERT_EQUAL(IVR_OK, create_worker(1));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_play(g_worker, &operation, &text));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_play(g_worker, &operation, &text));
    TEST_ASSERT_EQUAL_INT(1, g_media.play_calls);

    operation.operation_generation = 2;
    operation.deadline_ms = 99;
    TEST_ASSERT_EQUAL(IVR_ESTALE,
                      ivr_worker_play(g_worker, &operation, &text));
    TEST_ASSERT_EQUAL_INT(1, g_media.play_calls);
}

void test_input_window_rejects_stale_completion(void) {
    ivr_media_operation_t operation;
    ivr_bytes_view_t input_id = {"input-1", 7};
    ivr_bytes_view_t stale_id = {"input-old", 9};
    memset(&operation, 0, sizeof(operation));
    operation.call = g_call;
    operation.operation_generation = 1;

    TEST_ASSERT_EQUAL(IVR_OK, create_worker(1));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_begin_input(
                                  g_worker, &operation, &input_id, 7));
    operation.operation_generation = 2;
    TEST_ASSERT_EQUAL(IVR_ESTALE, ivr_worker_end_input(
                                     g_worker, &operation, &stale_id, 7));
    TEST_ASSERT_EQUAL_INT(0, g_media.end_input_calls);
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_end_input(
                                  g_worker, &operation, &input_id, 7));
    TEST_ASSERT_EQUAL_INT(1, g_media.end_input_calls);
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

    TEST_ASSERT_EQUAL(IVR_OK, create_worker(1));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_begin_input(
                                  g_worker, &operation, &input_id, 7));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    TEST_ASSERT_EQUAL_UINT32(1, page.count);
    TEST_ASSERT_TRUE(page.records[0].input_active);
    TEST_ASSERT_EQUAL_STRING("input-1", page.records[0].input_id);
    TEST_ASSERT_EQUAL_UINT64(7, page.records[0].input_generation);

    operation.operation_generation = 2;
    TEST_ASSERT_EQUAL(IVR_ESTALE, ivr_worker_cancel_input(
                                     g_worker, &operation, &stale_id, 7));
    TEST_ASSERT_EQUAL(IVR_ESTALE, ivr_worker_cancel_input(
                                     g_worker, &operation, &input_id, 8));
    TEST_ASSERT_EQUAL_INT(0, g_media.cancel_calls);

    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_cancel_input(
                                  g_worker, &operation, &input_id, 7));
    TEST_ASSERT_EQUAL_INT(1, g_media.cancel_calls);
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    TEST_ASSERT_FALSE(page.records[0].input_active);
    TEST_ASSERT_EQUAL_STRING("", page.records[0].input_id);
    TEST_ASSERT_EQUAL_UINT64(0, page.records[0].input_generation);

    operation.operation_generation = 3;
    TEST_ASSERT_EQUAL(IVR_ESTALE, ivr_worker_cancel_input(
                                     g_worker, &operation, &input_id, 7));
    TEST_ASSERT_EQUAL_INT(1, g_media.cancel_calls);
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

    TEST_ASSERT_EQUAL(IVR_OK, create_worker(1));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_publish_event_copy(g_worker, &event));
    TEST_ASSERT_EQUAL_INT(1, g_media.event_calls);
}

void test_close_and_drain_release_media_slots(void) {
    ivr_call_ref_t second = make_call("call-43");
    TEST_ASSERT_EQUAL(IVR_OK, create_worker(2));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &second));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_close_media_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_close_media_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL_UINT32(1, ivr_worker_active_sessions(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_begin_drain(g_worker));
    TEST_ASSERT_EQUAL_UINT32(0, ivr_worker_active_sessions(g_worker));
    TEST_ASSERT_EQUAL_INT(2, g_media.stop_calls);
    TEST_ASSERT_EQUAL_INT(2, g_media.destroy_calls);
    TEST_ASSERT_EQUAL(IVR_ECLOSED,
                      ivr_worker_open_media_call(g_worker, &g_call));
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
    TEST_ASSERT_EQUAL(IVR_EVERSION,
                      ivr_worker_create(&config, &sink, &factory, &g_worker));
    TEST_ASSERT_NULL(g_worker);
}

void test_inventory_is_owned_versioned_and_bounded(void) {
    ivr_call_ref_t second = make_call("call-43");
    ivr_call_ref_t third = make_call("call-44");
    ivr_worker_inventory_query_t query;
    ivr_worker_inventory_page_t page;
    uint64_t revision;
    uint32_t next_cursor;

    TEST_ASSERT_EQUAL(IVR_OK, create_worker(3));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &second));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &third));

    memset(&query, 0, sizeof(query));
    query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    query.limit = 2;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    TEST_ASSERT_EQUAL_UINT32(2, page.count);
    TEST_ASSERT_EQUAL_UINT32(3, page.total_active);
    TEST_ASSERT_TRUE(page.has_more);
    TEST_ASSERT_TRUE(page.next_cursor > 0);
    TEST_ASSERT_EQUAL_STRING("media-worker-test", page.records[0].worker_id);
    TEST_ASSERT_EQUAL_STRING("media-worker-instance",
                             page.records[0].worker_instance_id);
    TEST_ASSERT_EQUAL_UINT64(7, page.records[0].worker_epoch);
    TEST_ASSERT_EQUAL_UINT64(0,
                             page.records[0].operation_generation);
    TEST_ASSERT_EQUAL_STRING("call-42",
                             page.records[0].provider_session_id);
    TEST_ASSERT_EQUAL_STRING("call-42", page.records[0].dialog_id);
    TEST_ASSERT_EQUAL_STRING("room-42", page.records[0].room_id);
    TEST_ASSERT_EQUAL_STRING("call-42", page.records[0].call_id);
    TEST_ASSERT_EQUAL_UINT64(1, page.records[0].call_generation);
    TEST_ASSERT_EQUAL_INT(IVR_WORKER_RESOURCE_ACTIVE,
                          page.records[0].state);
    TEST_ASSERT_TRUE(page.records[0].rebindable);
    revision = page.revision;
    next_cursor = page.next_cursor;

    /* The page owns its strings; closing the worker slot does not mutate it. */
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_close_media_call(g_worker, &g_call));
    TEST_ASSERT_EQUAL_STRING("call-42", page.records[0].call_id);

    query.expected_revision = revision;
    query.cursor = next_cursor;
    TEST_ASSERT_EQUAL(IVR_ESTALE,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    TEST_ASSERT_EQUAL_UINT32(0, page.count);

    query.expected_revision = 0;
    query.cursor = 0;
    query.limit = IVR_WORKER_INVENTORY_MAX_PAGE_SIZE;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    TEST_ASSERT_EQUAL_UINT32(2, page.count);
    TEST_ASSERT_EQUAL_UINT32(2, page.total_active);
    TEST_ASSERT_FALSE(page.has_more);
    TEST_ASSERT_EQUAL_UINT32(0, page.next_cursor);
}

void test_inventory_rejects_unknown_version_limit_and_cursor(void) {
    ivr_worker_inventory_query_t query;
    ivr_worker_inventory_page_t page;

    TEST_ASSERT_EQUAL(IVR_OK, create_worker(1));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    memset(&query, 0, sizeof(query));
    query.inventory_version = IVR_WORKER_INVENTORY_VERSION + 1u;
    query.limit = 1;
    TEST_ASSERT_EQUAL(IVR_EVERSION,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    query.limit = 0;
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    query.limit = IVR_WORKER_INVENTORY_MAX_PAGE_SIZE + 1u;
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    query.limit = 1;
    query.cursor = 2;
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_worker_query_inventory(g_worker, &query, &page));
}

void test_reconnect_epoch_invalidates_inventory_and_preserves_media(void) {
    ivr_worker_inventory_query_t query;
    ivr_worker_inventory_page_t page;
    uint64_t old_revision;

    TEST_ASSERT_EQUAL(IVR_OK, create_worker(1));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_start(g_worker));
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_open_media_call(g_worker, &g_call));
    memset(&query, 0, sizeof(query));
    query.inventory_version = IVR_WORKER_INVENTORY_VERSION;
    query.limit = 1;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    old_revision = page.revision;
    TEST_ASSERT_EQUAL_UINT64(7, page.records[0].worker_epoch);

    TEST_ASSERT_EQUAL(IVR_EINVAL, ivr_worker_advance_epoch(g_worker, 0));
    TEST_ASSERT_EQUAL(IVR_ESTALE, ivr_worker_advance_epoch(g_worker, 7));
    TEST_ASSERT_EQUAL(IVR_OK, ivr_worker_advance_epoch(g_worker, 8));
    TEST_ASSERT_EQUAL_UINT32(1, ivr_worker_active_sessions(g_worker));

    query.expected_revision = old_revision;
    TEST_ASSERT_EQUAL(IVR_ESTALE,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    query.expected_revision = 0;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_worker_query_inventory(g_worker, &query, &page));
    TEST_ASSERT_EQUAL_UINT32(1, page.count);
    TEST_ASSERT_EQUAL_UINT64(8, page.records[0].worker_epoch);
    TEST_ASSERT_TRUE(page.revision > old_revision);
}

spec("test_ivr_worker") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_open_is_bounded_and_idempotent);
  TT_TEST(test_typed_open_records_generation_for_restart_inventory);
  TT_TEST(test_operations_are_explicit_and_idempotent);
  TT_TEST(test_input_window_rejects_stale_completion);
  TT_TEST(test_cancel_is_fenced_to_the_exact_active_input);
  TT_TEST(test_media_event_is_forwarded_without_workflow_processing);
  TT_TEST(test_close_and_drain_release_media_slots);
  TT_TEST(test_old_abi_is_rejected);
  TT_TEST(test_inventory_is_owned_versioned_and_bounded);
  TT_TEST(test_inventory_rejects_unknown_version_limit_and_cursor);
  TT_TEST(test_reconnect_epoch_invalidates_inventory_and_preserves_media);
}
