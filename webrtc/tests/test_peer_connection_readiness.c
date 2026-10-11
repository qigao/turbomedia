#include "tinytest.h"

/* Like test_signaling_internals, compile the implementation into the test to
 * script private transport events without exporting production test hooks.
 * Media/SRTP operations still use the real RTC implementation. */
#include "../src/rtc/media/turbo_peer_connection.c"

static turbo_peer_connection_t *test_peer;
static int connected_events;
static int failed_events;
static turbo_peer_state_t last_event;

static void record_peer_state(turbo_peer_connection_t *pc,
                              turbo_peer_state_t state, void *user_data) {
    (void)pc;
    (void)user_data;
    last_event = state;
    if (state == TURBO_PEER_STATE_CONNECTED) ++connected_events;
    if (state == TURBO_PEER_STATE_FAILED) ++failed_events;
}

static void setup_peer(void) {
    turbo_peer_config_t config = {.allow_loopback = 1, .disable_datachannel = 1};
    turbo_peer_callbacks_t callbacks = {.on_state_change = record_peer_state};
    test_peer = turbo_peer_connection_create(&config, &callbacks);
    check_not_null(test_peer);

    /* These are event-order unit cases, not a network handshake. Stop real
     * ICE production before supplying the callback events below. */
    cmeta_mutex_lock(&test_peer->ice_mutex);
    test_peer->ice_stop_requested = 1;
    cmeta_cond_broadcast(&test_peer->ice_cond);
    cmeta_mutex_unlock(&test_peer->ice_mutex);
    turbo_ice_owner_close(test_peer->ice_owner);
    cmeta_thread_join(&test_peer->ice_worker);
    cmeta_thread_destroy(&test_peer->ice_worker);
    test_peer->ice_worker_started = 0;
    turbo_ice_owner_destroy(test_peer->ice_owner);
    test_peer->ice_owner = NULL;
    connected_events = 0;
    failed_events = 0;
    last_event = TURBO_PEER_STATE_NEW;
    test_peer->state = TURBO_PEER_STATE_NEW;
}

static void ice_event(ice_state_t state) {
    on_ice_state_change(NULL, ICE_STATE_NEW, state, test_peer);
}

static void dc_event(turbo_dc_state_t state) {
    on_dc_state(test_peer->dc_peer, TURBO_DC_STATE_CONNECTING, state, test_peer);
}

/* Establish the prerequisite facts for state-publication cases only. Real
 * handshake/key export/media delivery remain covered by the integration suite. */
static void established_media(void) {
    cmeta_mutex_lock(&test_peer->ice_mutex);
    test_peer->dtls_connected = 1;
    test_peer->media_ready = 1;
    cmeta_mutex_unlock(&test_peer->ice_mutex);
}

spec("PeerConnection readiness publication") {
    before_each() { setup_peer(); }
    after_each() {
        turbo_peer_connection_destroy(test_peer);
        test_peer = NULL;
    }

    it("does not publish readiness after SRTP setup fails and ICE recovers") {
        ice_event(ICE_STATE_CONNECTED);
        /* No handshake/keys: the real SRTP setup must fail. */
        dc_event(TURBO_DC_STATE_CONNECTED);
        check_equal(failed_events, 1);
        check_equal(connected_events, 0);
        check_equal(last_event, TURBO_PEER_STATE_FAILED);
        /* Transport identity remains established for fingerprint protection. */
        check_equal(atomic_load(&test_peer->dtls_connected), 1);

        ice_event(ICE_STATE_DISCONNECTED);
        ice_event(ICE_STATE_COMPLETED);
        check_equal(connected_events, 0);
        check_equal(atomic_load(&test_peer->dtls_connect_pending), 0);
    }

    it("requires ICE readiness and retains initialized media across ICE loss") {
        established_media();
        notify_state_change(test_peer, TURBO_PEER_STATE_CONNECTED);
        check_equal(connected_events, 0);

        ice_event(ICE_STATE_CONNECTED);
        check_equal(connected_events, 1);
        ice_event(ICE_STATE_COMPLETED);
        check_equal(connected_events, 1);
        ice_event(ICE_STATE_DISCONNECTED);
        /* A delayed completion cannot overwrite committed transport loss. */
        notify_state_change(test_peer, TURBO_PEER_STATE_CONNECTED);
        check_equal(last_event, TURBO_PEER_STATE_DISCONNECTED);
        check_equal(connected_events, 1);

        ice_event(ICE_STATE_COMPLETED);
        check_equal(connected_events, 2);
        check_equal(atomic_load(&test_peer->dtls_connect_pending), 0);
    }

    it("invalidates media readiness when DC closes") {
        established_media();
        ice_event(ICE_STATE_COMPLETED);
        check_equal(connected_events, 1);
        dc_event(TURBO_DC_STATE_CLOSED);
        notify_state_change(test_peer, TURBO_PEER_STATE_CONNECTED);
        check_equal(last_event, TURBO_PEER_STATE_DISCONNECTED);
        check_equal(connected_events, 1);
    }

    it("invalidates media readiness when DC fails") {
        established_media();
        ice_event(ICE_STATE_COMPLETED);
        dc_event(TURBO_DC_STATE_FAILED);
        notify_state_change(test_peer, TURBO_PEER_STATE_CONNECTED);
        check_equal(last_event, TURBO_PEER_STATE_FAILED);
        check_equal(connected_events, 1);
        check_equal(failed_events, 1);
    }
}
