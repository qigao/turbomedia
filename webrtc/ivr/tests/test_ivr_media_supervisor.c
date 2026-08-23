#include "ivr_media_supervisor.h"
#include "ivr_thread.h"
#include "tinytest.h"

#include <string.h>

typedef struct {
    ivr_mutex_t lock;
    unsigned restart_count;
    uint64_t last_restart_generation;
    unsigned disconnected;
    unsigned reconnected;
    unsigned exhausted;
    unsigned input_stalled;
} supervisor_probe_t;

static ivr_media_supervisor_t *g_supervisor;
static supervisor_probe_t g_probe;

static uint32_t probe_restart(void *context, uint64_t generation) {
    supervisor_probe_t *probe = (supervisor_probe_t *)context;
    ivr_mutex_lock(&probe->lock);
    probe->restart_count++;
    probe->last_restart_generation = generation;
    ivr_mutex_unlock(&probe->lock);
    return 0;
}

static void probe_event(void *context, ivr_media_reconnect_event_t event,
                        uint64_t generation) {
    supervisor_probe_t *probe = (supervisor_probe_t *)context;
    (void)generation;
    ivr_mutex_lock(&probe->lock);
    if (event == IVR_MEDIA_RECONNECT_EVENT_DISCONNECTED) {
        probe->disconnected++;
    } else if (event == IVR_MEDIA_RECONNECT_EVENT_RECONNECTED) {
        probe->reconnected++;
    } else if (event == IVR_MEDIA_RECONNECT_EVENT_RETRY_EXHAUSTED) {
        probe->exhausted++;
    } else if (event == IVR_MEDIA_RECONNECT_EVENT_INPUT_STALLED) {
        probe->input_stalled++;
    }
    ivr_mutex_unlock(&probe->lock);
}

static int wait_for(unsigned *field, unsigned expected, uint64_t timeout_ms) {
    uint64_t waited = 0;
    while (waited < timeout_ms) {
        unsigned value;
        ivr_mutex_lock(&g_probe.lock);
        value = *field;
        ivr_mutex_unlock(&g_probe.lock);
        if (value >= expected) {
            return 1;
        }
        ivr_thread_sleep_ms(10);
        waited += 10;
    }
    return 0;
}

void setUp(void) {
    ivr_media_supervisor_config_t config;
    memset(&g_probe, 0, sizeof(g_probe));
    check_equal((int)(ivr_mutex_init(&g_probe.lock)), (int)(0));
    memset(&config, 0, sizeof(config));
    config.reconnect.max_attempts = 3;
    config.reconnect.initial_backoff_ms = 20;
    config.reconnect.max_backoff_ms = 40;
    config.reconnect.total_deadline_ms = 500;
    config.restart = probe_restart;
    config.restart_context = &g_probe;
    config.on_event = probe_event;
    config.event_context = &g_probe;
    check_equal(ivr_media_supervisor_create(&config, &g_supervisor), IVR_OK);
    check_equal(ivr_media_supervisor_start(g_supervisor), IVR_OK);
    check_true(wait_for(&g_probe.restart_count, 1, 500));
}

void tearDown(void) {
    ivr_media_supervisor_destroy(g_supervisor);
    g_supervisor = NULL;
    ivr_mutex_destroy(&g_probe.lock);
}

void test_callbacks_are_copied_and_retry_runs_on_owner(void) {
    check_equal(ivr_media_supervisor_submit_state(
                    g_supervisor, IVR_MEDIA_LINK_WHIP, 1,
                    IVR_MEDIA_LINK_CONNECTED, IVR_MEDIA_ERROR_NONE), IVR_OK);
    check_equal(ivr_media_supervisor_submit_state(
                    g_supervisor, IVR_MEDIA_LINK_WHEP, 1,
                    IVR_MEDIA_LINK_CONNECTED, IVR_MEDIA_ERROR_NONE), IVR_OK);
    check_equal(ivr_media_supervisor_submit_state(
                    g_supervisor, IVR_MEDIA_LINK_WHIP, 1,
                    IVR_MEDIA_LINK_DISCONNECTED, IVR_MEDIA_ERROR_NONE), IVR_OK);
    check_true(wait_for(&g_probe.disconnected, 1, 500));
    check_true(wait_for(&g_probe.restart_count, 2, 500));

    ivr_mutex_lock(&g_probe.lock);
    check_equal((uint64_t)(g_probe.last_restart_generation), (uint64_t)(2u));
    ivr_mutex_unlock(&g_probe.lock);
    check_equal(ivr_media_supervisor_submit_state(
                    g_supervisor, IVR_MEDIA_LINK_WHIP, 2,
                    IVR_MEDIA_LINK_CONNECTED, IVR_MEDIA_ERROR_NONE), IVR_OK);
    check_equal(ivr_media_supervisor_submit_state(
                    g_supervisor, IVR_MEDIA_LINK_WHEP, 2,
                    IVR_MEDIA_LINK_CONNECTED, IVR_MEDIA_ERROR_NONE), IVR_OK);
    check_true(wait_for(&g_probe.reconnected, 1, 500));
}

void test_input_stall_is_distinct_and_retried(void) {
    check_equal(ivr_media_supervisor_submit_state(
                    g_supervisor, IVR_MEDIA_LINK_WHEP, 1,
                    IVR_MEDIA_LINK_DISCONNECTED,
                    IVR_MEDIA_ERROR_INPUT_STALLED), IVR_OK);
    check_true(wait_for(&g_probe.input_stalled, 1, 500));
    check_true(wait_for(&g_probe.disconnected, 1, 500));
    check_true(wait_for(&g_probe.restart_count, 2, 500));
}

void test_stop_is_callback_barrier(void) {
    ivr_media_supervisor_stop(g_supervisor);
    check_equal(ivr_media_supervisor_submit_state(
                         g_supervisor, IVR_MEDIA_LINK_WHIP, 1,
                         IVR_MEDIA_LINK_FAILED,
                         IVR_MEDIA_ERROR_PEER_FAILED), IVR_ECLOSED);
}

spec("test_ivr_media_supervisor") {
  before_each() { setUp(); }
  after_each() { tearDown(); }
  it("test_callbacks_are_copied_and_retry_runs_on_owner") { test_callbacks_are_copied_and_retry_runs_on_owner(); };
  it("test_input_stall_is_distinct_and_retried") { test_input_stall_is_distinct_and_retried(); };
  it("test_stop_is_callback_barrier") { test_stop_is_callback_barrier(); };
}
