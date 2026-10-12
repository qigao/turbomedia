#include "tinytest.h"
#include "turbo_peer_connection.h"
#include <platform.h>
#include <salts/thread.h>
#include <string.h>

enum { CALLBACK_WAIT_MS = 3000, DRAIN_OBSERVATION_MS = 50 };

typedef struct {
    turbo_peer_connection_t *peer;
    cmeta_mutex_t mutex;
    cmeta_cond_t cond;
    cmeta_thread_t destroy_thread;
    cmeta_thread_t close_thread;
    int destroy_started;
    int close_started;
    int block_candidate;
    int callback_entered;
    int release_callback;
    int destroy_entered;
    int destroy_done;
    int handles_preserved;
    int callback_count;
} lifecycle_fixture_t;

static lifecycle_fixture_t fixture;

static int wait_flag(int *flag) {
    for (int i = 0; i < CALLBACK_WAIT_MS; ++i) {
        cmeta_mutex_lock(&fixture.mutex);
        int ready = *flag;
        cmeta_mutex_unlock(&fixture.mutex);
        if (ready) return 1;
        cmeta_sleep_ms(1);
    }
    return 0;
}

static void hold_callback(turbo_peer_connection_t *peer,
                          lifecycle_fixture_t *f) {
    cmeta_mutex_lock(&f->mutex);
    ++f->callback_count;
    if (!f->callback_entered) {
        turbo_media_context_t *media = turbo_peer_connection_get_media_context(peer);
        turbo_dc_peer_t *dc = turbo_peer_connection_get_dc_peer(peer);
        f->callback_entered = 1;
        cmeta_cond_broadcast(&f->cond);
        while (!f->release_callback) cmeta_cond_wait(&f->cond, &f->mutex);
        /* Observe the supported callback borrow; do not dereference a stale
         * saved handle even if a broken implementation released it early. */
        f->handles_preserved = media && dc &&
            turbo_peer_connection_get_media_context(peer) == media &&
            turbo_peer_connection_get_dc_peer(peer) == dc;
    }
    cmeta_mutex_unlock(&f->mutex);
}

static void on_candidate(turbo_peer_connection_t *peer, const char *candidate,
                         void *user_data) {
    lifecycle_fixture_t *f = user_data;
    (void)candidate;
    if (f->block_candidate) hold_callback(peer, f);
}

static void on_state(turbo_peer_connection_t *peer, turbo_peer_state_t state,
                     void *user_data) {
    lifecycle_fixture_t *f = user_data;
    (void)state;
    if (!f->block_candidate) hold_callback(peer, f);
}

static void destroy_peer(void *arg) {
    lifecycle_fixture_t *f = arg;
    cmeta_mutex_lock(&f->mutex);
    f->destroy_entered = 1;
    cmeta_cond_broadcast(&f->cond);
    cmeta_mutex_unlock(&f->mutex);
    turbo_peer_connection_destroy(f->peer);
    cmeta_mutex_lock(&f->mutex);
    f->peer = NULL;
    f->destroy_done = 1;
    cmeta_cond_broadcast(&f->cond);
    cmeta_mutex_unlock(&f->mutex);
}

static void close_dc(void *arg) {
    lifecycle_fixture_t *f = arg;
    turbo_dc_peer_close(turbo_peer_connection_get_dc_peer(f->peer));
}

static void create_peer(int block_candidate) {
    turbo_peer_config_t config = {0};
    turbo_peer_callbacks_t callbacks = {0};
    fixture.block_candidate = block_candidate;
    config.allow_loopback = 1;
    config.disable_datachannel = 1;
    config.user_data = &fixture;
    callbacks.on_ice_candidate = on_candidate;
    callbacks.on_state_change = on_state;
    fixture.peer = turbo_peer_connection_create(&config, &callbacks);
    check_not_null(fixture.peer);
}

static void verify_callback_drain(void) {
    check_true(wait_flag(&fixture.callback_entered));
    int rc = cmeta_thread_create(&fixture.destroy_thread, destroy_peer, &fixture);
    fixture.destroy_started = rc == 0;
    check_equal(rc, 0);
    check_true(wait_flag(&fixture.destroy_entered));

    cmeta_mutex_lock(&fixture.mutex);
    uint64_t deadline = cmeta_monotonic_ms() + DRAIN_OBSERVATION_MS;
    while (!fixture.destroy_done && cmeta_monotonic_ms() < deadline) {
        (void)cmeta_cond_timedwait(&fixture.cond, &fixture.mutex, UINT64_C(1000000));
    }
    int completed_early = fixture.destroy_done;
    fixture.release_callback = 1;
    cmeta_cond_broadcast(&fixture.cond);
    cmeta_mutex_unlock(&fixture.mutex);

    check_false(completed_early);
    check_true(wait_flag(&fixture.destroy_done));
    check_true(fixture.handles_preserved);
    check_equal(fixture.callback_count, 1);
}

spec("PeerConnection callback lifetime") {
    before_each() {
        memset(&fixture, 0, sizeof(fixture));
        cmeta_mutex_init(&fixture.mutex);
        cmeta_cond_init(&fixture.cond);
    }
    after_each() {
        cmeta_mutex_lock(&fixture.mutex);
        fixture.release_callback = 1;
        cmeta_cond_broadcast(&fixture.cond);
        cmeta_mutex_unlock(&fixture.mutex);
        if (fixture.close_started) {
            cmeta_thread_join(&fixture.close_thread);
            cmeta_thread_destroy(&fixture.close_thread);
        }
        if (fixture.destroy_started) {
            cmeta_thread_join(&fixture.destroy_thread);
            cmeta_thread_destroy(&fixture.destroy_thread);
        }
        turbo_peer_connection_destroy(fixture.peer);
        cmeta_cond_destroy(&fixture.cond);
        cmeta_mutex_destroy(&fixture.mutex);
    }

    it("keeps dependent handles alive until an ICE callback returns") {
        create_peer(1);
        verify_callback_drain();
    }

    it("drains an admitted DC state callback before releasing media") {
        create_peer(0);
        int rc = cmeta_thread_create(&fixture.close_thread, close_dc, &fixture);
        fixture.close_started = rc == 0;
        check_equal(rc, 0);
        verify_callback_drain();
    }
}
