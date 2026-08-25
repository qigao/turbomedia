#include "ivr_worker_control.h"
#include "tinytest.h"

typedef struct {
    unsigned calls;
    uint64_t last_epoch;
} advance_probe_t;

static int record_advance(void *context, uint64_t next_epoch) {
    advance_probe_t *probe = (advance_probe_t *)context;
    probe->calls++;
    probe->last_epoch = next_epoch;
    return 0;
}

void test_reconnect_is_applied_only_when_owner_drains_queue(void) {
    ivr_worker_control_queue_t *queue = NULL;
    ivr_worker_control_state_t state;
    ivr_worker_control_event_t event;
    advance_probe_t probe = {0};
    const ivr_worker_control_event_t events[] = {
        {IVR_WORKER_CONTROL_CONNECTION, 1},
        {IVR_WORKER_CONTROL_CONNECTION, 0},
        {IVR_WORKER_CONTROL_CONNECTION, 1}};

    check_equal(ivr_worker_control_queue_create(4, &queue), 0);
    ivr_worker_control_state_init(&state, 7);
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        check_equal(ivr_worker_control_queue_publish(queue, &events[i]), 0);
    }

    check_equal(probe.calls, 0u);
    check_equal(state.connection_generation, (uint64_t)7u);
    check_false(state.connected);

    check_equal(ivr_worker_control_queue_try_pop(queue, &event), 1);
    check_equal(ivr_worker_control_apply(&state, &event, record_advance,
                                         &probe), 0);
    check_true(state.connected);
    check_equal(probe.calls, 0u);

    check_equal(ivr_worker_control_queue_try_pop(queue, &event), 1);
    check_equal(ivr_worker_control_apply(&state, &event, record_advance,
                                         &probe), 0);
    check_false(state.connected);

    check_equal(ivr_worker_control_queue_try_pop(queue, &event), 1);
    check_equal(ivr_worker_control_apply(&state, &event, record_advance,
                                         &probe), 0);
    check_true(state.connected);
    check_equal(probe.calls, 1u);
    check_equal(probe.last_epoch, (uint64_t)8u);
    check_equal(state.connection_generation, (uint64_t)8u);
    check_equal(ivr_worker_control_queue_try_pop(queue, &event), 0);
    ivr_worker_control_queue_destroy(queue);
}

void test_queue_full_and_closed_are_explicit(void) {
    ivr_worker_control_queue_t *queue = NULL;
    const ivr_worker_control_event_t event = {
        IVR_WORKER_CONTROL_CONNECTION, 1};

    check_equal(ivr_worker_control_queue_create(2, &queue), 0);
    check_equal(ivr_worker_control_queue_publish(queue, &event), 0);
    check_equal(ivr_worker_control_queue_publish(queue, &event), 0);
    check_equal(ivr_worker_control_queue_publish(queue, &event), -1);
    ivr_worker_control_queue_close(queue);
    check_equal(ivr_worker_control_queue_publish(queue, &event), -1);
    ivr_worker_control_queue_destroy(queue);
}

void test_drain_is_an_owner_event(void) {
    ivr_worker_control_state_t state;
    const ivr_worker_control_event_t event = {IVR_WORKER_CONTROL_DRAIN, 0};
    ivr_worker_control_state_init(&state, 1);
    check_equal(ivr_worker_control_apply(&state, &event, NULL, NULL), 0);
    check_true(state.drain_requested);
}

spec("test_ivr_worker_control") {
    it("queues reconnect transitions for the owner") {
        test_reconnect_is_applied_only_when_owner_drains_queue();
    };
    it("rejects full and closed publication") {
        test_queue_full_and_closed_are_explicit();
    };
    it("models drain as an owner event") { test_drain_is_an_owner_event(); };
}
