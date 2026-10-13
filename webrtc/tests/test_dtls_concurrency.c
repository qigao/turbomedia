/* Exercise real DTLS transactions and timer-owner callbacks, without an SFU. */
#include "turbo_datachannel_internal.h"
#include "turbo_srtp_defs.h"
#include "tinytest.h"
#include <cstl/deque.h>
#include <string.h>
#include <stdlib.h>
#ifdef TURBO_MEDIA_TEST_TSAN_LOG_ISOLATION
#include <tlog.h>
/* Only the dedicated focused TSAN executable uses this test configuration.
 * Salts.Native rc.15 ships as an UNSANITIZED binary. Its async logger
 * transfers buffers with release/acquire atomics, but those SDK-internal
 * synchronization edges are invisible to the consumer's TSan runtime.
 * Keep all SCTP/DTLS assertions and the real provider unchanged; suppress
 * only nonfatal log *generation* for this test process, not any TSan report. */
static void isolate_uninstrumented_salts_logger(void) {
    tlog_t *logger = tlog_get_default();
    if (logger) tlog_set_level(logger, SALTS_LOG_LEVEL_FATAL);
}
#else
static void isolate_uninstrumented_salts_logger(void) {}
#endif

typedef struct {
    turbo_dc_context_t *context;
    turbo_dc_peer_t *peer;
    cmeta_mutex_t mutex;
    cmeta_cond_t cond;
    cmeta_thread_t connect_thread;
    cmeta_thread_t other_thread;
    int connect_started;
    int other_started;
    int engine_entered;
    int release_engine;
    int other_done;
    int block_retransmit;
    int retransmit_entered;
    int release_retransmit;
    int sends;
    int callback_export_returned;
    int callback_peer_created;
    int connect_result;
    uint16_t exported_profile;
} dtls_fixture_t;

static dtls_fixture_t fixture;

static int wait_flag(int *flag) {
    int result = 0;
    /* Bounded waits keep failed barriers diagnosable; teardown opens them. */
    for (int i = 0; i < 3000; ++i) {
        cmeta_mutex_lock(&fixture.mutex);
        result = *flag;
        cmeta_mutex_unlock(&fixture.mutex);
        if (result) break;
        cmeta_sleep_ms(1);
    }
    return result;
}

static void discard_transport(void *transport, const void *data, size_t len) {
    dtls_fixture_t *f = (dtls_fixture_t *)transport;
    srtp_keying_material_t keys;
    (void)data; (void)len;
    /* This public reentry must not run under DTLS admission. */
    (void)turbo_dc_peer_get_srtp_keys(f->peer, &keys);
    cmeta_mutex_lock(&f->mutex);
    f->callback_export_returned = 1;
    ++f->sends;
    if (f->block_retransmit && f->sends == 2) {
        cmeta_mutex_unlock(&f->mutex);
        /* Timer dispatch must release the context list mutex before calling
         * user transport code. This also exercises insertion/removal while
         * the worker retains its current list node. */
        turbo_dc_peer_t *sibling = turbo_dc_peer_create(f->context, NULL, 0, NULL);
        int created = sibling != NULL;
        turbo_dc_peer_destroy(sibling);
        cmeta_mutex_lock(&f->mutex);
        f->callback_peer_created = created;
        f->retransmit_entered = 1;
        cmeta_cond_broadcast(&f->cond);
        while (!f->release_retransmit) cmeta_cond_wait(&f->cond, &f->mutex);
    }
    cmeta_mutex_unlock(&f->mutex);
}

/* Exercise a real engine transaction using the production admission/lease
 * protocol, then hold its commit point for concurrent public API callers. */
static void admitted_handshake(void *arg) {
    dtls_fixture_t *f = arg;
    if (dc_peer_acquire(f->peer) != 0) return;
    dtls_session_lock(f->peer);
    uint8_t pin[32];
    const uint16_t profiles[] = {1, 2, 7, 8};
    f->connect_result = turbo_gdtls_context_fingerprint(f->context->dtls_context, pin);
    f->peer->dtls.engine = turbo_gdtls_create(f->context->dtls_context, 0,
        f->context->dtls_mtu, pin, profiles, 4, 0);
    if (!f->connect_result && f->peer->dtls.engine) {
        f->peer->dtls.handshake_started = 1;
        f->connect_result = turbo_gdtls_start(f->peer->dtls.engine, cmeta_monotonic_ms());
    } else f->connect_result = -1;
    cmeta_mutex_lock(&f->mutex);
    f->engine_entered = 1;
    cmeta_cond_broadcast(&f->cond);
    while (!f->release_engine) cmeta_cond_wait(&f->cond, &f->mutex);
    cmeta_mutex_unlock(&f->mutex);
    dtls_session_unlock(f->peer);
    dc_peer_release(f->peer);
}

static void export_keys(void *arg) {
    dtls_fixture_t *f = (dtls_fixture_t *)arg;
    srtp_keying_material_t keys;
    f->exported_profile = turbo_dc_peer_get_srtp_keys(f->peer, &keys);
    cmeta_mutex_lock(&f->mutex);
    f->other_done = 1;
    cmeta_mutex_unlock(&f->mutex);
}

/* Simulate an ICE owner delivering its first DTLS record while the poll
 * owner is still inside the handshake admission. This is the historical
 * #160 collision shape, but the input here is deliberately non-mutating: the
 * test proves serial admission, not the root cause of the old flaky run. */
static void deliver_initial_dtls(void *arg) {
    dtls_fixture_t *f = (dtls_fixture_t *)arg;
    static const uint8_t empty_handshake_record[13] = {
        22, 0xfe, 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    turbo_dc_peer_feed_transport_data(
        f->peer, empty_handshake_record, sizeof(empty_handshake_record));
    cmeta_mutex_lock(&f->mutex);
    f->other_done = 1;
    cmeta_cond_broadcast(&f->cond);
    cmeta_mutex_unlock(&f->mutex);
}

static void close_peer(void *arg) {
    dtls_fixture_t *f = (dtls_fixture_t *)arg;
    turbo_dc_peer_close(f->peer);
    cmeta_mutex_lock(&f->mutex);
    f->other_done = 1;
    cmeta_mutex_unlock(&f->mutex);
}

static void destroy_peer(void *arg) {
    dtls_fixture_t *f = (dtls_fixture_t *)arg;
    turbo_dc_peer_destroy(f->peer);
    f->peer = NULL;
    cmeta_mutex_lock(&f->mutex);
    f->other_done = 1;
    cmeta_mutex_unlock(&f->mutex);
}

static void setup_fixture(void) {
    isolate_uninstrumented_salts_logger();
    turbo_dc_config_t config = {0};
    memset(&fixture, 0, sizeof(fixture));
    cmeta_mutex_init(&fixture.mutex);
    cmeta_cond_init(&fixture.cond);
    config.transport = TURBO_DC_TRANSPORT_ICE;
    config.disable_sctp = 1;
    fixture.context = turbo_dc_context_create(&config);
    check_not_null(fixture.context);
    fixture.peer = turbo_dc_peer_create(fixture.context, NULL, 0, NULL);
    check_not_null(fixture.peer);
    check_equal(turbo_dc_peer_set_remote_fingerprint(fixture.peer, "sha-256",
        fixture.context->local_fingerprint), 0);
    check_equal(turbo_dc_peer_set_external_transport(
        fixture.peer, &fixture, discard_transport), 0);
}

static void cleanup_fixture(void) {
    cmeta_mutex_lock(&fixture.mutex);
    fixture.release_engine = 1;
    fixture.release_retransmit = 1;
    cmeta_cond_broadcast(&fixture.cond);
    cmeta_mutex_unlock(&fixture.mutex);
    if (fixture.connect_started) cmeta_thread_join(&fixture.connect_thread);
    if (fixture.other_started) cmeta_thread_join(&fixture.other_thread);
    turbo_dc_peer_destroy(fixture.peer);
    turbo_dc_context_destroy(fixture.context);
    cmeta_cond_destroy(&fixture.cond);
    cmeta_mutex_destroy(&fixture.mutex);
}

static void start_paused_handshake(void) {
    int rc = cmeta_thread_create(&fixture.connect_thread, admitted_handshake, &fixture);
    fixture.connect_started = rc == 0;
    check_equal(rc, 0);
    check_true(wait_flag(&fixture.engine_entered));
    check_equal(fixture.connect_result, 0);
}

static void start_other(cmeta_thread_cb callback) {
    int rc = cmeta_thread_create(&fixture.other_thread, callback, &fixture);
    fixture.other_started = rc == 0;
    check_equal(rc, 0);
}

static void release_handshake(void) {
    cmeta_mutex_lock(&fixture.mutex);
    fixture.release_engine = 1;
    cmeta_cond_broadcast(&fixture.cond);
    cmeta_mutex_unlock(&fixture.mutex);
}

spec("DTLS concurrent entry and timer drain") {
    before_each() { setup_fixture(); }
    after_each() { cleanup_fixture(); }

    it("rejects a missing signaling pin before sending a ClientHello") {
        tstr_free(fixture.peer->remote_fingerprint);
        fixture.peer->remote_fingerprint = NULL;
        check_equal(turbo_dc_peer_connect(fixture.peer), 0);
        check_equal(turbo_dc_peer_get_state(fixture.peer), TURBO_DC_STATE_FAILED);
        check_equal(turbo_dc_peer_get_error(fixture.peer).code, TURBO_DC_ERROR_DTLS_FINGERPRINT);
        check_equal(fixture.sends, 0);
    }

    it("freezes the admitted pin and role while permitting an identical SDP pin") {
        check_equal(turbo_dc_peer_connect(fixture.peer), 0);
        check_equal(turbo_dc_peer_set_remote_fingerprint(fixture.peer, "sha-256",
            fixture.context->local_fingerprint), 0);
        char other[96];
        memcpy(other, fixture.context->local_fingerprint, sizeof(other));
        other[0] = other[0] == '0' ? '1' : '0';
        check_equal(turbo_dc_peer_set_remote_fingerprint(fixture.peer, "sha-256", other), -5);
        check_equal(turbo_dc_peer_set_dtls_role(fixture.peer, 1), -2);
    }

    it("propagates invalid datagram admission as terminal and unregisters its timer") {
        static const unsigned char oversized[65536] = {22};
        check_equal(turbo_dc_peer_connect(fixture.peer), 0);
        turbo_dc_peer_feed_transport_data(fixture.peer, oversized, sizeof(oversized));
        check_equal(turbo_dc_peer_get_state(fixture.peer), TURBO_DC_STATE_FAILED);
        srtp_keying_material_t keys;
        check_equal(turbo_dc_peer_get_srtp_keys(fixture.peer, &keys), (uint16_t)0);
        cmeta_mutex_lock(&fixture.context->transport_mutex);
        unsigned timers = fixture.context->active_dtls_timers;
        cmeta_mutex_unlock(&fixture.context->transport_mutex);
        check_equal(timers, 0u);
    }

    it("serializes key export with an in-progress DTLS transaction") {
        start_paused_handshake();
        start_other(export_keys);
        int both_admitted = 0;
        for (int i = 0; i < 3000 && !both_admitted; ++i) {
            cmeta_mutex_lock(&fixture.peer->operation_mutex);
            both_admitted = fixture.peer->active_operations == 2;
            cmeta_mutex_unlock(&fixture.peer->operation_mutex);
            if (!both_admitted) cmeta_sleep_ms(1);
        }
        /* The export caller has entered the peer lifetime protocol but must
         * not observe DTLS until the actual handshake transaction exits. */
        check_true(both_admitted);
        cmeta_mutex_lock(&fixture.mutex);
        int completed_early = fixture.other_done;
        cmeta_mutex_unlock(&fixture.mutex);
        check_false(completed_early);
        release_handshake();
        check_true(wait_flag(&fixture.other_done));
        check_equal(fixture.exported_profile, (uint16_t)0);
    }

    it("serializes first ICE datagram delivery behind an admitted handshake") {
        start_paused_handshake();
        start_other(deliver_initial_dtls);
        int both_admitted = 0;
        for (int i = 0; i < 3000 && !both_admitted; ++i) {
            cmeta_mutex_lock(&fixture.peer->operation_mutex);
            both_admitted = fixture.peer->active_operations == 2;
            cmeta_mutex_unlock(&fixture.peer->operation_mutex);
            if (!both_admitted) cmeta_sleep_ms(1);
        }
        check_true(both_admitted);
        cmeta_mutex_lock(&fixture.mutex);
        int completed_early = fixture.other_done;
        cmeta_mutex_unlock(&fixture.mutex);
        check_false(completed_early);
        release_handshake();
        check_true(wait_flag(&fixture.other_done));
        /* A datagram from the ICE owner must not overwrite a concurrent
         * handshake, produce keys or fail an otherwise live peer. */
        dtls_session_lock(fixture.peer);
        int live = fixture.peer->dtls.handshake_started &&
                   !fixture.peer->dtls.stopped &&
                   fixture.peer->dtls.engine != NULL;
        dtls_session_unlock(fixture.peer);
        check_true(live);
        srtp_keying_material_t keys;
        check_equal(turbo_dc_peer_get_srtp_keys(fixture.peer, &keys), (uint16_t)0);
    }

    it("drains the live handshake before destroying the engine") {
        start_paused_handshake();
        start_other(destroy_peer);
        int destruction_admitted = 0;
        for (int i = 0; i < 3000 && !destruction_admitted; ++i) {
            cmeta_mutex_lock(&fixture.peer->operation_mutex);
            destruction_admitted = fixture.peer->destroying;
            cmeta_mutex_unlock(&fixture.peer->operation_mutex);
            if (!destruction_admitted) cmeta_sleep_ms(1);
        }
        check_true(destruction_admitted);
        release_handshake();
        check_true(wait_flag(&fixture.other_done));
    }

    it("retransmits lost flights and closes while the timer sender is blocked") {
        fixture.block_retransmit = 1;
        check_equal(turbo_dc_peer_connect(fixture.peer), 0);
        check_true(wait_flag(&fixture.retransmit_entered));
        start_other(close_peer);
        /* close must not wait for the timer-owner callback: it may be
         * waiting on the caller's transport owner, as it is here. */
        check_true(wait_flag(&fixture.other_done));
        check_true(fixture.callback_export_returned);
        check_true(fixture.callback_peer_created);
        check_equal(turbo_dc_peer_get_state(fixture.peer), TURBO_DC_STATE_CLOSED);
    }
}

typedef struct {
    turbo_dc_context_t *context;
    turbo_dc_peer_t *peer;
    cmeta_mutex_t mutex;
    deque_t outgoing;
    size_t max_packet;
    unsigned packets;
    unsigned ready_packets;
    unsigned dropped_packets;
    unsigned connected_events;
    unsigned failed_events;
    int nested_timer_armed;
    unsigned nested_timer_calls;
    /* Held real native timer-output callback across foreign close/destroy.
     * Both mutex and condition are per test endpoint, never global owners. */
    cmeta_cond_t timer_cond;
    int hold_timer_callback;
    int timer_callback_entered;
    int timer_callback_release;
    int timer_callback_finished;
    unsigned timer_callback_count;
    int timer_pump_finished;
    int lifecycle_started;
    int lifecycle_finished;
    int drop_first_ready_packet;
    int send_failed;
    turbo_dc_channel_t *channel; /* Borrowed from the peer. */
    size_t max_application_packet;
    uint8_t message[TURBO_DC_MAX_MESSAGE_SIZE];
    int partial_observed;
    size_t message_size;
    int message_binary;
    unsigned messages;
    int message_invalid;
} packet_endpoint_t;

static packet_endpoint_t endpoints[2];
/* Set only while the test itself calls the real native timer pump. An
 * observed callback with this flag belongs to usrsctp_handle_timers(). */
static SALTS_THREAD_LOCAL int g_test_timer_dispatching = 0;

static void queue_packet(void *transport, const void *data, size_t len) {
    packet_endpoint_t *endpoint = transport;
    /* A transport callback may reenter DTLS APIs; production must release
     * admission before invoking it. Do not assert on a worker thread. */
    srtp_keying_material_t keys;
    uint16_t profile = turbo_dc_peer_get_srtp_keys(endpoint->peer, &keys);
    tstr packet = tstr_new_len(data, len);
    int nested_timer_call = 0;
    cmeta_mutex_lock(&endpoint->mutex);
    if (len > endpoint->max_packet) endpoint->max_packet = len;
    if (len && ((const uint8_t *)data)[0] == 23 && len > endpoint->max_application_packet)
        endpoint->max_application_packet = len;
    ++endpoint->packets;
    if (profile) ++endpoint->ready_packets;
    /* The server publishes readiness before sending its final flight. Drop
     * that first ready packet once, without parsing or modifying DTLS bytes. */
    if (profile && endpoint->drop_first_ready_packet && !endpoint->dropped_packets) {
        ++endpoint->dropped_packets;
        tstr_free(packet);
        cmeta_mutex_unlock(&endpoint->mutex);
        return;
    }
    if (!packet || deque_push_back(&endpoint->outgoing, &packet) != STL_OK) {
        endpoint->send_failed = 1;
        tstr_free(packet);
    }
    if (g_test_timer_dispatching && endpoint->nested_timer_armed &&
        endpoint->nested_timer_calls == 0) {
        ++endpoint->nested_timer_calls;
        nested_timer_call = 1;
    }
    if (g_test_timer_dispatching && endpoint->hold_timer_callback) {
        /* sctp_outbound_packet_cb has acquired a real peer lease BEFORE this
         * GmSSL transport callback. Signal only after native timer dispatch
         * has entered application transport code, and keep the callback
         * admitted until the foreign owner has published close/destroy. */
        ++endpoint->timer_callback_count;
        endpoint->timer_callback_entered = 1;
        cmeta_cond_broadcast(&endpoint->timer_cond);
        while (!endpoint->timer_callback_release)
            cmeta_cond_wait(&endpoint->timer_cond, &endpoint->mutex);
        endpoint->timer_callback_finished = 1;
        cmeta_cond_broadcast(&endpoint->timer_cond);
    }
    cmeta_mutex_unlock(&endpoint->mutex);
    /* An actual usrsctp timer-triggered transport callback reenters the
     * exported pump on its own thread, after releasing all fixture locks.
     * The outer pump must not try to recursively lock the global queue. */
    if (nested_timer_call) turbo_dc_handle_timers();
}

static void record_packet_endpoint_state(turbo_dc_peer_t *peer,
                                         turbo_dc_state_t old_state,
                                         turbo_dc_state_t state, void *user) {
    packet_endpoint_t *endpoint = user;
    (void)peer; (void)old_state;
    cmeta_mutex_lock(&endpoint->mutex);
    if (state == TURBO_DC_STATE_CONNECTED) ++endpoint->connected_events;
    if (state == TURBO_DC_STATE_FAILED) ++endpoint->failed_events;
    cmeta_mutex_unlock(&endpoint->mutex);
}

static void setup_packet_endpoints(int disable_sctp, uint16_t dtls_mtu, uint16_t sctp_mtu) {
    isolate_uninstrumented_salts_logger();
    memset(endpoints, 0, sizeof(endpoints));
    for (int i = 0; i < 2; ++i) {
        cmeta_mutex_init(&endpoints[i].mutex);
        cmeta_cond_init(&endpoints[i].timer_cond);
    }
    for (int i = 0; i < 2; ++i) {
        packet_endpoint_t *endpoint = &endpoints[i];
        turbo_dc_config_t config = {0};
        config.transport = TURBO_DC_TRANSPORT_ICE;
        config.disable_sctp = disable_sctp;
        config.is_server = i;
        config.dtls_mtu = dtls_mtu;
        config.sctp_mtu = sctp_mtu;
        check_equal(deque_init_bytes(&endpoint->outgoing, sizeof(tstr),
                    _Alignof(tstr), 256u), STL_OK);
        endpoint->context = turbo_dc_context_create(&config);
        check_not_null(endpoint->context);
        endpoint->peer = turbo_dc_peer_create(endpoint->context, NULL, 0, endpoint);
        check_not_null(endpoint->peer);
        turbo_dc_peer_on_state(endpoint->peer, record_packet_endpoint_state);
        check_equal(turbo_dc_peer_set_external_transport(
                    endpoint->peer, endpoint, queue_packet), 0);
    }
    for (int i = 0; i < 2; ++i) {
        char hash[16], fingerprint[96];
        check_equal(turbo_dc_context_get_local_fingerprint(endpoints[1 - i].context,
                    hash, sizeof(hash), fingerprint, sizeof(fingerprint)), 0);
        check_equal(turbo_dc_peer_set_remote_fingerprint(
                    endpoints[i].peer, hash, fingerprint), 0);
    }
}

static void cleanup_packet_endpoints(void) {
    /* Callbacks only queue bytes; no worker can feed the opposite peer while
     * destruction drains its DTLS transactions and transport callbacks. */
    for (int i = 0; i < 2; ++i) {
        turbo_dc_peer_destroy(endpoints[i].peer);
        turbo_dc_context_destroy(endpoints[i].context);
        tstr packet;
        while (deque_pop_front(&endpoints[i].outgoing, &packet) == STL_OK)
            tstr_free(packet);
        deque_destroy(&endpoints[i].outgoing);
        cmeta_cond_destroy(&endpoints[i].timer_cond);
        cmeta_mutex_destroy(&endpoints[i].mutex);
    }
}

static void deliver_packets(int sender) {
    packet_endpoint_t *endpoint = &endpoints[sender];
    for (;;) {
        tstr packet = NULL;
        cmeta_mutex_lock(&endpoint->mutex);
        (void)deque_pop_front(&endpoint->outgoing, &packet);
        cmeta_mutex_unlock(&endpoint->mutex);
        if (!packet) return;
        turbo_dc_peer_t *receiver = endpoints[1 - sender].peer;
        turbo_dc_peer_feed_transport_data(receiver, packet, tstr_len(packet));
        cmeta_mutex_lock(&receiver->operation_mutex);
        for (size_t i = 0; i < SCTP_RECEIVE_SLOTS; ++i)
            if (receiver->sctp.receive[i].active) endpoints[1 - sender].partial_observed = 1;
        cmeta_mutex_unlock(&receiver->operation_mutex);
        tstr_free(packet);
    }
}

spec("DTLS datagram transport handshake") {
    before_each() { setup_packet_endpoints(1, 256, 0); }
    after_each() { cleanup_packet_endpoints(); }

    it("rejects the wrong signaling identity without publishing SRTP keys") {
        char wrong[96];
        memcpy(wrong, endpoints[1].context->local_fingerprint, sizeof(wrong));
        wrong[0] = wrong[0] == '0' ? '1' : '0';
        check_equal(turbo_dc_peer_set_remote_fingerprint(endpoints[0].peer, "sha-256", wrong), 0);
        check_equal(turbo_dc_peer_connect(endpoints[1].peer), 0);
        check_equal(turbo_dc_peer_connect(endpoints[0].peer), 0);
        /* DTLS owner callbacks may complete after the immediate deque
         * drains. Rendezvous on the *actual* FAILED state callback instead
         * of treating eight fast polling turns as a scheduling oracle.
         * Never relax the expected fingerprint error or key rejection. */
        int fingerprint_rejected = 0;
        uint64_t deadline = cmeta_monotonic_ms() + 2000u;
        while (!fingerprint_rejected && cmeta_monotonic_ms() < deadline) {
            deliver_packets(0);
            deliver_packets(1);
            cmeta_mutex_lock(&endpoints[0].mutex);
            fingerprint_rejected = endpoints[0].failed_events != 0;
            cmeta_mutex_unlock(&endpoints[0].mutex);
            if (!fingerprint_rejected) cmeta_sleep_ms(1);
        }
        check_true(fingerprint_rejected);
        check_equal(turbo_dc_peer_get_state(endpoints[0].peer), TURBO_DC_STATE_FAILED);
        check_equal(turbo_dc_peer_get_error(endpoints[0].peer).code, TURBO_DC_ERROR_DTLS_FINGERPRINT);
        srtp_keying_material_t keys, sentinel;
        memset(&sentinel, 0x5a, sizeof(sentinel));
        keys = sentinel;
        check_equal(turbo_dc_peer_get_srtp_keys(endpoints[0].peer, &keys), (uint16_t)0);
        check_equal(memcmp(&keys, &sentinel, sizeof(keys)), 0);
    }

    it("recovers a lost final server flight after the server has become ready") {
        srtp_keying_material_t keys[2] = {0};
        uint16_t profiles[2] = {0};
        cmeta_mutex_lock(&endpoints[1].mutex);
        endpoints[1].drop_first_ready_packet = 1;
        cmeta_mutex_unlock(&endpoints[1].mutex);
        check_equal(turbo_dc_peer_connect(endpoints[1].peer), 0);
        check_equal(turbo_dc_peer_connect(endpoints[0].peer), 0);
        /* Includes the cookie exchange and asynchronous DTLS timer owner.
         * Eight immediate deque drains can outrun that owner on macOS;
         * rendezvous on the actual one-packet drop, without relaxing the
         * dropped-flight, identity, SRTP or retry assertions below. This is
         * test-only scheduling and does not alter protocol timeouts. */
        uint64_t initial_deadline = cmeta_monotonic_ms() + 1000;
        while (cmeta_monotonic_ms() < initial_deadline) {
            deliver_packets(0);
            deliver_packets(1);
            cmeta_mutex_lock(&endpoints[1].mutex);
            unsigned observed_drop = endpoints[1].dropped_packets;
            cmeta_mutex_unlock(&endpoints[1].mutex);
            if (observed_drop == 1u &&
                turbo_dc_peer_get_srtp_keys(endpoints[1].peer, &keys[1])) break;
            cmeta_sleep_ms(1);
        }
        cmeta_mutex_lock(&endpoints[1].mutex);
        unsigned dropped = endpoints[1].dropped_packets;
        cmeta_mutex_unlock(&endpoints[1].mutex);
        check_equal(dropped, 1u);
        check_equal(turbo_dc_peer_get_state(endpoints[1].peer), TURBO_DC_STATE_CONNECTED);
        check_not_equal(turbo_dc_peer_get_srtp_keys(endpoints[1].peer, &keys[1]), (uint16_t)0);
        check_equal(turbo_dc_peer_get_srtp_keys(endpoints[0].peer, &keys[0]), (uint16_t)0);

        /* The client retries at its deadline. The ready server retains its
         * final flight but does not proactively retransmit application data. */
        uint64_t deadline = cmeta_monotonic_ms() + 3000;
        do {
            deliver_packets(0);
            deliver_packets(1);
            for (int i = 0; i < 2; ++i)
                profiles[i] = turbo_dc_peer_get_srtp_keys(endpoints[i].peer, &keys[i]);
            if (profiles[0] && profiles[1]) break;
            cmeta_sleep_ms(1);
        } while (cmeta_monotonic_ms() < deadline);

        check_not_equal(profiles[0], (uint16_t)0);
        check_equal(profiles[0], profiles[1]);
        check_equal(keys[0].key_len, keys[1].key_len);
        check_equal(keys[0].salt_len, keys[1].salt_len);
        check_equal(memcmp(keys[0].client_key, keys[1].client_key, keys[0].key_len), 0);
        check_equal(memcmp(keys[0].server_key, keys[1].server_key, keys[0].key_len), 0);
        check_equal(memcmp(keys[0].client_salt, keys[1].client_salt, keys[0].salt_len), 0);
        check_equal(memcmp(keys[0].server_salt, keys[1].server_salt, keys[0].salt_len), 0);
        for (int i = 0; i < 2; ++i) {
            cmeta_mutex_lock(&endpoints[i].mutex);
            unsigned connected = endpoints[i].connected_events;
            unsigned ready_packets = endpoints[i].ready_packets;
            int failed = endpoints[i].send_failed;
            cmeta_mutex_unlock(&endpoints[i].mutex);
            check_false(failed);
            check_equal(connected, 1u);
            check_equal(turbo_dc_peer_get_state(endpoints[i].peer), TURBO_DC_STATE_CONNECTED);
            if (i == 1) check_true(ready_packets >= 2);
        }
    }

    it("preserves small-MTU flights through authenticated handshake and SRTP export") {
        srtp_keying_material_t keys[2] = {0};
        uint16_t profiles[2] = {0};
        check_equal(turbo_dc_peer_connect(endpoints[1].peer), 0);
        check_equal(turbo_dc_peer_connect(endpoints[0].peer), 0);
        for (int step = 0; step < 1000; ++step) {
            deliver_packets(0);
            deliver_packets(1);
            for (int i = 0; i < 2; ++i)
                profiles[i] = turbo_dc_peer_get_srtp_keys(endpoints[i].peer, &keys[i]);
            if (profiles[0] && profiles[1]) break;
            cmeta_sleep_ms(1);
        }
        for (int i = 0; i < 2; ++i) {
            cmeta_mutex_lock(&endpoints[i].mutex);
            int failed = endpoints[i].send_failed;
            size_t largest = endpoints[i].max_packet;
            unsigned packets = endpoints[i].packets;
            cmeta_mutex_unlock(&endpoints[i].mutex);
            check_false(failed);
            check_true(packets > 1);
            check_true(largest <= 256);
            check_not_equal(profiles[i], (uint16_t)0);
            check_equal(turbo_dc_peer_get_state(endpoints[i].peer), TURBO_DC_STATE_CONNECTED);
        }
        check_equal(profiles[0], profiles[1]);
        check_equal(keys[0].key_len, keys[1].key_len);
        check_equal(keys[0].salt_len, keys[1].salt_len);
        check_equal(memcmp(keys[0].client_key, keys[1].client_key, keys[0].key_len), 0);
        check_equal(memcmp(keys[0].server_key, keys[1].server_key, keys[0].key_len), 0);
        check_equal(memcmp(keys[0].client_salt, keys[1].client_salt, keys[0].salt_len), 0);
        check_equal(memcmp(keys[0].server_salt, keys[1].server_salt, keys[0].salt_len), 0);
        /* Shutdown revokes export and unregisters even the ready server's
         * retained final flight. Context teardown must not retain its timer. */
        for (int i = 0; i < 2; ++i) {
            turbo_dc_peer_close(endpoints[i].peer);
            check_equal(turbo_dc_peer_get_srtp_keys(endpoints[i].peer, &keys[i]), (uint16_t)0);
            cmeta_mutex_lock(&endpoints[i].context->transport_mutex);
            unsigned timers = endpoints[i].context->active_dtls_timers;
            cmeta_mutex_unlock(&endpoints[i].context->transport_mutex);
            check_equal(timers, 0u);
        }
    }
}

static void record_channel_message(turbo_dc_channel_t *channel, const void *data,
                                   size_t size, int binary, void *user) {
    packet_endpoint_t *endpoint = user;
    (void)channel;
    cmeta_mutex_lock(&endpoint->mutex);
    ++endpoint->messages;
    endpoint->message_size = size;
    endpoint->message_binary = binary;
    if (size > sizeof(endpoint->message)) endpoint->message_invalid = 1;
    else memcpy(endpoint->message, data, size);
    cmeta_mutex_unlock(&endpoint->mutex);
}

static void record_incoming_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel, void *user) {
    packet_endpoint_t *endpoint = user;
    (void)peer;
    turbo_dc_channel_set_user_data(channel, endpoint);
    turbo_dc_channel_on_message(channel, record_channel_message);
    cmeta_mutex_lock(&endpoint->mutex);
    endpoint->channel = channel;
    cmeta_mutex_unlock(&endpoint->mutex);
}

static void open_packet_channel(void) {
    turbo_dc_peer_on_channel(endpoints[1].peer, record_incoming_channel);
    check_equal(turbo_dc_peer_connect(endpoints[1].peer), 0);
    check_equal(turbo_dc_peer_connect(endpoints[0].peer), 0);
    uint64_t deadline = cmeta_monotonic_ms() + 5000;
    int connected = 0;
    do {
        deliver_packets(0); deliver_packets(1);
        connected = 1;
        for (int i = 0; i < 2; ++i) {
            cmeta_mutex_lock(&endpoints[i].mutex);
            connected &= endpoints[i].connected_events != 0;
            cmeta_mutex_unlock(&endpoints[i].mutex);
        }
        if (connected) break;
        cmeta_sleep_ms(1);
    } while (cmeta_monotonic_ms() < deadline);
    check_true(connected);
    endpoints[0].channel = turbo_dc_channel_create(endpoints[0].peer, "message-test", NULL);
    check_not_null(endpoints[0].channel);
    turbo_dc_channel_set_user_data(endpoints[0].channel, &endpoints[0]);
    turbo_dc_channel_on_message(endpoints[0].channel, record_channel_message);
    check_equal(turbo_dc_channel_open(endpoints[0].channel), 0);
    deadline = cmeta_monotonic_ms() + 5000;
    while (!turbo_dc_channel_is_open(endpoints[0].channel) && cmeta_monotonic_ms() < deadline) {
        deliver_packets(0); deliver_packets(1);
        cmeta_sleep_ms(1);
    }
    check_true(turbo_dc_channel_is_open(endpoints[0].channel));
    check_not_null(endpoints[1].channel);
    check_true(turbo_dc_channel_is_open(endpoints[1].channel));
}

/* Issue #208: usrsctp_init_nothreads requires a caller to advance native
 * SCTP elapsed time explicitly. Do not use sleep as a thread ordering oracle:
 * this fixture asserts functional retransmission liveness when an actual
 * SCTP DATA packet is intentionally withheld from the receiver. */
spec("SCTP manual timer elapsed progression") {
    before_each() { setup_packet_endpoints(0, 1200, 1000); }
    after_each() { cleanup_packet_endpoints(); }

    it("advances the native retransmission clock without replaying application data") {
        /* Make a future association's initial RTO short but nonzero so this
         * liveness test is bounded without changing production deadlines.
         * SCTP_RTOINFO is applied before association creation. */
        struct socket *socket = sctp_native_borrow(endpoints[0].peer);
        check_not_null(socket);
        struct sctp_rtoinfo rto = {0};
        rto.srto_assoc_id = SCTP_FUTURE_ASSOC;
        rto.srto_initial = 250;
        rto.srto_min = 200;
        rto.srto_max = 1000;
        int configured = socket
            ? usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_RTOINFO,
                                 &rto, sizeof(rto)) : -1;
        if (socket) sctp_native_return(endpoints[0].peer);
        check_equal(configured, 0);

        open_packet_channel();
        const uint8_t payload = 0xa7;
        cmeta_mutex_lock(&endpoints[0].mutex);
        unsigned before = endpoints[0].packets;
        cmeta_mutex_unlock(&endpoints[0].mutex);
        check_equal(turbo_dc_channel_send(endpoints[0].channel,
                                         &payload, sizeof(payload), 1), 0);
        cmeta_mutex_lock(&endpoints[0].mutex);
        unsigned first_send = endpoints[0].packets;
        cmeta_mutex_unlock(&endpoints[0].mutex);
        check_true(first_send > before);
        cmeta_mutex_lock(&endpoints[0].mutex);
        endpoints[0].nested_timer_armed = 1;
        cmeta_mutex_unlock(&endpoints[0].mutex);

        /* Do not deliver the DATA packet or call send again. Only a native
         * SCTP timer can produce another outbound packet for this peer.
         * The 10 ms sleep provides real elapsed time, never sequencing of
         * concurrent operations. CI must see retransmission by 4 seconds. */
        uint64_t deadline = cmeta_monotonic_ms() + 4000u;
        unsigned later = first_send;
        while (later <= first_send && cmeta_monotonic_ms() < deadline) {
            g_test_timer_dispatching = 1;
            turbo_dc_handle_timers();
            g_test_timer_dispatching = 0;
            cmeta_mutex_lock(&endpoints[0].mutex);
            later = endpoints[0].packets;
            cmeta_mutex_unlock(&endpoints[0].mutex);
            if (later <= first_send) cmeta_sleep_ms(10);
        }
        check_true(later > first_send);
        cmeta_mutex_lock(&endpoints[0].mutex);
        unsigned nested = endpoints[0].nested_timer_calls;
        cmeta_mutex_unlock(&endpoints[0].mutex);
        check_equal(nested, 1u);
    }
}

/* Issue #208: run the real SCTP timer engine on one thread, and hold an
 * admitted outbound application transport callback while a DIFFERENT owner
 * closes or destroys the peer. This does not insert a fake usrsctp timer or
 * bypass the installed native library. It is the "callback admitted first"
 * half of the global timer retirement contract, NOT proof that a callback
 * dequeued but still before dc_peer_acquire can safely reference userdata. */
typedef struct {
    packet_endpoint_t *endpoint;
    turbo_dc_peer_t *peer;
    int destroy;
} timer_foreign_lifecycle_fixture_t;

static int wait_real_timer_flag(packet_endpoint_t *endpoint, int *flag,
                                uint64_t timeout_ms) {
    uint64_t until = cmeta_monotonic_ms() + timeout_ms;
    int ready;
    cmeta_mutex_lock(&endpoint->mutex);
    while (!*flag && cmeta_monotonic_ms() < until)
        (void)cmeta_cond_timedwait(&endpoint->timer_cond, &endpoint->mutex,
                                   UINT64_C(10000000));
    ready = *flag;
    cmeta_mutex_unlock(&endpoint->mutex);
    return ready;
}

static void pump_until_real_timer_callback(void *arg) {
    timer_foreign_lifecycle_fixture_t *fixture = arg;
    packet_endpoint_t *endpoint = fixture->endpoint;
    uint64_t until = cmeta_monotonic_ms() + 4000u;
    for (;;) {
        g_test_timer_dispatching = 1;
        turbo_dc_handle_timers();
        g_test_timer_dispatching = 0;
        cmeta_mutex_lock(&endpoint->mutex);
        int entered = endpoint->timer_callback_entered;
        cmeta_mutex_unlock(&endpoint->mutex);
        if (entered || cmeta_monotonic_ms() >= until) break;
        /* Permit a real monotonic interval to elapse; not an ordering
         * oracle. The callback and closing owner use condition variables. */
        cmeta_sleep_ms(10);
    }
    cmeta_mutex_lock(&endpoint->mutex);
    endpoint->timer_pump_finished = 1;
    cmeta_cond_broadcast(&endpoint->timer_cond);
    cmeta_mutex_unlock(&endpoint->mutex);
}

static void close_or_destroy_real_timer_peer(void *arg) {
    timer_foreign_lifecycle_fixture_t *fixture = arg;
    packet_endpoint_t *endpoint = fixture->endpoint;
    cmeta_mutex_lock(&endpoint->mutex);
    endpoint->lifecycle_started = 1;
    cmeta_cond_broadcast(&endpoint->timer_cond);
    cmeta_mutex_unlock(&endpoint->mutex);
    if (fixture->destroy)
        turbo_dc_peer_destroy(fixture->peer);
    else
        turbo_dc_peer_close(fixture->peer);
    cmeta_mutex_lock(&endpoint->mutex);
    endpoint->lifecycle_finished = 1;
    cmeta_cond_broadcast(&endpoint->timer_cond);
    cmeta_mutex_unlock(&endpoint->mutex);
}

static void test_admitted_real_timer_vs_foreign_close(int destroy) {
    packet_endpoint_t *endpoint = &endpoints[0];
    struct socket *native = sctp_native_borrow(endpoint->peer);
    check_not_null(native);
    struct sctp_rtoinfo rto = {0};
    rto.srto_assoc_id = SCTP_FUTURE_ASSOC;
    rto.srto_initial = 250;
    rto.srto_min = 200;
    rto.srto_max = 1000;
    int configured = native
        ? usrsctp_setsockopt(native, IPPROTO_SCTP, SCTP_RTOINFO,
                             &rto, sizeof(rto)) : -1;
    if (native) sctp_native_return(endpoint->peer);
    check_equal(configured, 0);
    open_packet_channel();

    const uint8_t payload = 0xa7;
    cmeta_mutex_lock(&endpoint->mutex);
    unsigned before = endpoint->packets;
    cmeta_mutex_unlock(&endpoint->mutex);
    check_equal(turbo_dc_channel_send(endpoint->channel,
                                      &payload, sizeof(payload), 1), 0);
    cmeta_mutex_lock(&endpoint->mutex);
    int sent_one = endpoint->packets > before;
    endpoint->hold_timer_callback = 1;
    cmeta_mutex_unlock(&endpoint->mutex);
    check_true(sent_one);

    timer_foreign_lifecycle_fixture_t fixture = {
        .endpoint = endpoint, .peer = endpoint->peer, .destroy = destroy
    };
    cmeta_thread_t timer_thread = NULL, lifecycle_thread = NULL;
    int timer_created = cmeta_thread_create(&timer_thread,
        pump_until_real_timer_callback, &fixture) == 0;
    int entered = timer_created &&
        wait_real_timer_flag(endpoint, &endpoint->timer_callback_entered, 4000u);
    int lifecycle_created = entered && cmeta_thread_create(&lifecycle_thread,
        close_or_destroy_real_timer_peer, &fixture) == 0;
    int lifecycle_started = lifecycle_created &&
        wait_real_timer_flag(endpoint, &endpoint->lifecycle_started, 2000u);

    /* The genuine outbound callback keeps an already-admitted peer lease.
     * The destroy path MUST NOT reclaim peer storage while this callback
     * remains held. The ordinary close path instead seals native admission.
     * Observe that exact gate under its actual operation mutex. */
    int terminal_observed = 0, prior_operations = 0, close_early = 0;
    if (lifecycle_started) {
        uint64_t until = cmeta_monotonic_ms() + 2000u;
        cmeta_mutex_lock(&fixture.peer->operation_mutex);
        while (cmeta_monotonic_ms() < until) {
            terminal_observed = destroy
                ? fixture.peer->destroying
                : fixture.peer->sctp.native_close_requested;
            if (terminal_observed) break;
            (void)cmeta_cond_timedwait(&fixture.peer->operation_cond,
                &fixture.peer->operation_mutex, UINT64_C(10000000));
        }
        prior_operations = (int)fixture.peer->active_operations;
        cmeta_mutex_unlock(&fixture.peer->operation_mutex);
        cmeta_mutex_lock(&endpoint->mutex);
        close_early = endpoint->lifecycle_finished;
        cmeta_mutex_unlock(&endpoint->mutex);
    }

    /* On failed barriers, unblock the timer anyway so CTest can report a
     * bounded assertion instead of hanging with a test-held callback. */
    cmeta_mutex_lock(&endpoint->mutex);
    endpoint->timer_callback_release = 1;
    cmeta_cond_broadcast(&endpoint->timer_cond);
    cmeta_mutex_unlock(&endpoint->mutex);

    if (timer_created) {
        (void)cmeta_thread_join(&timer_thread);
        cmeta_thread_destroy(&timer_thread);
    }
    if (lifecycle_created) {
        (void)cmeta_thread_join(&lifecycle_thread);
        cmeta_thread_destroy(&lifecycle_thread);
    }
    if (destroy) {
        /* The peer has been reclaimed only after its admitted callback
         * returns. Prevent fixture cleanup from destroying it twice. */
        if (lifecycle_created) {
            endpoint->peer = NULL;
            endpoint->channel = NULL;
        }
    }

    int pump_done, callback_finished, lifecycle_finished;
    unsigned callbacks_before;
    cmeta_mutex_lock(&endpoint->mutex);
    pump_done = endpoint->timer_pump_finished;
    callback_finished = endpoint->timer_callback_finished;
    lifecycle_finished = endpoint->lifecycle_finished;
    callbacks_before = endpoint->timer_callback_count;
    cmeta_mutex_unlock(&endpoint->mutex);

    /* No still-live timer callback may be published for this peer after its
     * final close/destroy. Run the PUBLIC timer pump again in a new elapsed
     * interval; do not inspect any freed socket or peer pointer. */
    cmeta_sleep_ms(300);
    g_test_timer_dispatching = 1;
    turbo_dc_handle_timers();
    g_test_timer_dispatching = 0;
    cmeta_mutex_lock(&endpoint->mutex);
    unsigned callbacks_after = endpoint->timer_callback_count;
    cmeta_mutex_unlock(&endpoint->mutex);

    check_true(timer_created);
    check_true(entered);
    check_true(lifecycle_created);
    check_true(lifecycle_started);
    check_true(terminal_observed);
    check_true(prior_operations > 0);
    if (destroy) check_false(close_early);
    check_true(pump_done);
    check_true(callback_finished);
    check_true(lifecycle_finished);
    check_true(callbacks_before > 0u);
    check_equal(callbacks_after, callbacks_before);
    if (!destroy) {
        check_equal(turbo_dc_peer_get_state(endpoint->peer),
                    TURBO_DC_STATE_CLOSED);
        cmeta_mutex_lock(&endpoint->peer->operation_mutex);
        int native_closed = endpoint->peer->sctp.native_closed;
        unsigned remaining = endpoint->peer->sctp.native_borrows;
        cmeta_mutex_unlock(&endpoint->peer->operation_mutex);
        check_true(native_closed);
        check_equal(remaining, 0u);
    }
}

spec("SCTP real timer callback versus foreign peer lifetime") {
    before_each() { setup_packet_endpoints(0, 1200, 1000); }
    after_each() { cleanup_packet_endpoints(); }

    it("keeps a timer-admitted transport callback alive while foreign close runs") {
        test_admitted_real_timer_vs_foreign_close(0);
    }

    it("blocks foreign destroy until timer-admitted transport callback completes") {
        test_admitted_real_timer_vs_foreign_close(1);
    }
}

static void expect_packet_messages(unsigned count, const void *payload, size_t size, int binary) {
    uint64_t deadline = cmeta_monotonic_ms() + 5000;
    int received;
    do {
        deliver_packets(0); deliver_packets(1);
        received = 1;
        for (int i = 0; i < 2; ++i) {
            cmeta_mutex_lock(&endpoints[i].mutex);
            received &= endpoints[i].messages >= count;
            cmeta_mutex_unlock(&endpoints[i].mutex);
        }
        if (received) break;
        cmeta_sleep_ms(1);
    } while (cmeta_monotonic_ms() < deadline);
    check_true(received);
    for (int i = 0; i < 2; ++i) {
        cmeta_mutex_lock(&endpoints[i].mutex);
        unsigned messages = endpoints[i].messages;
        size_t actual_size = endpoints[i].message_size;
        int actual_binary = endpoints[i].message_binary;
        int invalid = endpoints[i].message_invalid || endpoints[i].send_failed;
        int equal = size == 0 || memcmp(endpoints[i].message, payload, size) == 0;
        cmeta_mutex_unlock(&endpoints[i].mutex);
        check_false(invalid);
        check_equal(messages, count);
        check_equal(actual_size, size);
        check_equal(actual_binary, binary);
        check_true(equal);
    }
}

spec("SCTP over GmSSL configured datagram limits") {
    before_each() { setup_packet_endpoints(0, 1200, 1000); }
    after_each() { cleanup_packet_endpoints(); }

    it("applies the configured socket MTU and delivers fragmented binary messages in both directions") {
        for (int i = 0; i < 2; ++i) {
            struct sctp_paddrparams params = {0};
            socklen_t length = sizeof(params);
            check_equal(usrsctp_getsockopt(endpoints[i].peer->sctp.socket, IPPROTO_SCTP,
                SCTP_PEER_ADDR_PARAMS, &params, &length), 0);
            check_equal(params.spp_pathmtu, (uint32_t)1000);
            check_true((params.spp_flags & SPP_PMTUD_DISABLE) != 0);
        }
        open_packet_channel();

        uint8_t payload[4096];
        for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 17);
        for (int i = 0; i < 2; ++i)
            check_equal(turbo_dc_channel_send(endpoints[i].channel, payload, sizeof(payload), 1), 0);
        uint64_t deadline = cmeta_monotonic_ms() + 5000;
        int received = 0;
        do {
            deliver_packets(0); deliver_packets(1);
            received = 1;
            for (int i = 0; i < 2; ++i) {
                cmeta_mutex_lock(&endpoints[i].mutex);
                received &= endpoints[i].messages != 0;
                cmeta_mutex_unlock(&endpoints[i].mutex);
            }
            if (received) break;
            cmeta_sleep_ms(1);
        } while (cmeta_monotonic_ms() < deadline);
        check_true(received);
        for (int i = 0; i < 2; ++i) {
            cmeta_mutex_lock(&endpoints[i].mutex);
            unsigned messages = endpoints[i].messages;
            size_t message_size = endpoints[i].message_size;
            int binary = endpoints[i].message_binary;
            size_t max_packet = endpoints[i].max_packet;
            size_t max_application = endpoints[i].max_application_packet;
            int invalid = endpoints[i].message_invalid || endpoints[i].send_failed;
            int equal = memcmp(endpoints[i].message, payload, sizeof(payload)) == 0;
            cmeta_mutex_unlock(&endpoints[i].mutex);
            check_false(invalid);
            check_equal(messages, 1u);
            check_equal(message_size, sizeof(payload));
            check_equal(binary, 1);
            check_true(equal);
            check_true(max_packet <= 1200);
            check_true(max_application > 37);
            check_true(max_application <= 1000 + 12 + 37);
            check_equal(turbo_dc_peer_get_state(endpoints[i].peer), TURBO_DC_STATE_CONNECTED);
        }
    }
}

/* Reentrant application close under real usrsctp_conninput. The Socket
 * cannot be physically freed while conninput still owns the native operation,
 * even when close happens synchronously on that same ICE/message Owner. */
static unsigned g_reentrant_native_messages;
static int g_reentrant_native_deferred;
static int g_reentrant_native_socket_live;

static void close_peer_during_sctp_ingress(turbo_dc_channel_t *channel,
                                            const void *data, size_t size,
                                            int binary, void *user) {
    packet_endpoint_t *endpoint = user;
    (void)channel;
    if (!endpoint || size != 1 || ((const uint8_t *)data)[0] != 0x5a || !binary)
        return;
    ++g_reentrant_native_messages;
    turbo_dc_peer_close(endpoint->peer);

    cmeta_mutex_lock(&endpoint->peer->operation_mutex);
    g_reentrant_native_deferred =
        endpoint->peer->sctp.native_close_requested &&
        endpoint->peer->sctp.native_borrows != 0;
    g_reentrant_native_socket_live = endpoint->peer->sctp.socket != NULL;
    cmeta_mutex_unlock(&endpoint->peer->operation_mutex);
}

spec("SCTP native socket reentrant close") {
    before_each() { setup_packet_endpoints(0, 1200, 1000); }
    after_each() { cleanup_packet_endpoints(); }

    it("defers physical native Socket close from within on_message conninput") {
        open_packet_channel();
        g_reentrant_native_messages = 0;
        g_reentrant_native_deferred = 0;
        g_reentrant_native_socket_live = 0;
        turbo_dc_channel_on_message(endpoints[1].channel,
                                    close_peer_during_sctp_ingress);

        const uint8_t payload = 0x5a;
        check_equal(turbo_dc_channel_send(endpoints[0].channel,
                                         &payload, 1, 1), 0);
        uint64_t deadline = cmeta_monotonic_ms() + 3000u;
        while (!g_reentrant_native_messages &&
               cmeta_monotonic_ms() < deadline) {
            deliver_packets(0);
            deliver_packets(1);
            cmeta_sleep_ms(1);
        }
        check_equal(g_reentrant_native_messages, 1u);
        check_true(g_reentrant_native_deferred);
        check_true(g_reentrant_native_socket_live);
        check_equal(turbo_dc_peer_get_state(endpoints[1].peer),
                    TURBO_DC_STATE_CLOSED);
        cmeta_mutex_lock(&endpoints[1].peer->operation_mutex);
        unsigned remaining = endpoints[1].peer->sctp.native_borrows;
        int closed = endpoints[1].peer->sctp.native_closed;
        struct socket *live = endpoints[1].peer->sctp.socket;
        cmeta_mutex_unlock(&endpoints[1].peer->operation_mutex);
        check_equal(remaining, 0u);
        check_true(closed);
        check_null(live);
    }
}

spec("SCTP over GmSSL empty messages") {
    before_each() { setup_packet_endpoints(0, 1200, 1000); }
    after_each() { cleanup_packet_endpoints(); }

    it("delivers empty text and binary messages followed by nonempty messages in both directions") {
        open_packet_channel();
        unsigned count = 0;
        /* A nonzero caller byte must not become the empty message payload. */
        const uint8_t payload = 'x';
        for (int binary = 0; binary <= 1; ++binary) {
            for (int i = 0; i < 2; ++i)
                check_equal(turbo_dc_channel_send(endpoints[i].channel, &payload, 0, binary), 0);
            expect_packet_messages(++count, &payload, 0, binary);
            for (int i = 0; i < 2; ++i)
                check_equal(turbo_dc_channel_send(endpoints[i].channel, &payload, 1, binary), 0);
            expect_packet_messages(++count, &payload, 1, binary);
        }
    }

    it("ignores SCTP payload for empty PPIDs independently of the channel send adapter") {
        open_packet_channel();
        const uint8_t payload[] = {0x41, 0x42, 0x43};
        unsigned count = 0;
        for (int binary = 0; binary <= 1; ++binary) {
            for (int i = 0; i < 2; ++i) {
                struct sctp_sndinfo info = {0};
                info.snd_sid = turbo_dc_channel_get_id(endpoints[i].channel);
                info.snd_ppid = htonl(binary ? SCTP_PPID_BINARY_EMPTY : SCTP_PPID_STRING_EMPTY);
                check_equal(usrsctp_sendv(endpoints[i].peer->sctp.socket,
                    payload, sizeof(payload), NULL, 0, &info, sizeof(info),
                    SCTP_SENDV_SNDINFO, 0), (ssize_t)sizeof(payload));
            }
            expect_packet_messages(++count, payload, 0, binary);
        }
    }
}

static void feed_sctp_fragment(uint16_t stream, uint16_t sequence, uint32_t ppid,
                               const void *payload, size_t size, int flags) {
    void *owned = malloc(size ? size : 1);
    check_not_null(owned);
    if (size) memcpy(owned, payload, size);
    struct sctp_rcvinfo info = {0};
    union sctp_sockstore address = {0};
    info.rcv_sid = stream;
    info.rcv_ssn = sequence;
    info.rcv_ppid = htonl(ppid);
    check_equal(sctp_inbound_packet_cb(NULL, address, owned, size, info, flags, endpoints[0].peer), 1);
}

static void close_receiving_channel(turbo_dc_channel_t *channel, const void *data,
                                    size_t size, int binary, void *user) {
    packet_endpoint_t *endpoint = user;
    turbo_dc_channel_close(channel);
    endpoint->channel = NULL;
    /* The completed buffer remains borrowed even after the channel closes. */
    record_channel_message(NULL, data, size, binary, user);
    cmeta_mutex_lock(&endpoint->peer->operation_mutex);
    if (endpoint->peer->sctp.receive_buffers != 1) endpoint->message_invalid = 1;
    cmeta_mutex_unlock(&endpoint->peer->operation_mutex);
}

static void setup_receive_channel(void) {
    setup_packet_endpoints(0, 1200, 1000);
    endpoints[0].channel = turbo_dc_channel_create(endpoints[0].peer, "partial", NULL);
    check_not_null(endpoints[0].channel);
    /* The adapter tests supply usrsctp-owned callback buffers directly;
     * the paired test below separately exercises association and wire delivery. */
    endpoints[0].channel->is_open = 1;
    turbo_dc_channel_set_user_data(endpoints[0].channel, &endpoints[0]);
    turbo_dc_channel_on_message(endpoints[0].channel, record_channel_message);
}

/* An accepted SCTP callback may outlive an application's close call.
 * Explicit condition rendezvous is used instead of a sleep-based ordering
 * oracle: the callback holds its channel while a different thread closes it. */
typedef struct {
    cmeta_mutex_t mutex;
    cmeta_cond_t cond;
    turbo_dc_peer_t *peer;
    turbo_dc_channel_t *channel;
    uint16_t sid;
    int callback_entered;
    int callback_release;
    int callback_finished;
    int callback_valid;
    unsigned callback_count;
    int close_started;
    int close_completed;
    int inbound_done;
    int inbound_result;
} channel_close_race_fixture_t;

static channel_close_race_fixture_t channel_close_race;

static int wait_channel_close_flag(int *flag) {
    int ready;
    const uint64_t until = cmeta_monotonic_ms() + 2000u;
    cmeta_mutex_lock(&channel_close_race.mutex);
    while (!*flag && cmeta_monotonic_ms() < until) {
        (void)cmeta_cond_timedwait(&channel_close_race.cond,
                                    &channel_close_race.mutex,
                                    UINT64_C(10000000));
    }
    ready = *flag;
    cmeta_mutex_unlock(&channel_close_race.mutex);
    return ready;
}

static void held_channel_message(turbo_dc_channel_t *channel, const void *data,
                                 size_t len, int binary, void *user_data) {
    channel_close_race_fixture_t *race = user_data;
    cmeta_mutex_lock(&race->mutex);
    ++race->callback_count;
    race->callback_entered = 1;
    cmeta_cond_broadcast(&race->cond);
    while (!race->callback_release)
        cmeta_cond_wait(&race->cond, &race->mutex);
    cmeta_mutex_unlock(&race->mutex);

    /* The callback was admitted before close. Its channel label must stay
     * readable until it finishes even after another thread returned close. */
    const char *label = turbo_dc_channel_get_label(channel);
    int valid = label && strcmp(label, "partial") == 0 &&
                len == 1 && ((const uint8_t *)data)[0] == 0x5a && binary;
    cmeta_mutex_lock(&race->mutex);
    race->callback_valid = valid;
    race->callback_finished = 1;
    cmeta_cond_broadcast(&race->cond);
    cmeta_mutex_unlock(&race->mutex);
}

static void feed_held_sctp_message(void *arg) {
    channel_close_race_fixture_t *race = arg;
    struct sctp_rcvinfo info = {0};
    union sctp_sockstore address = {0};
    uint8_t *data = malloc(1);
    if (data) {
        data[0] = 0x5a;
        info.rcv_sid = race->sid;
        info.rcv_ppid = htonl(SCTP_PPID_BINARY);
    }
    int rc = data ? sctp_inbound_packet_cb(
        NULL, address, data, 1, info, MSG_EOR, race->peer) : -1;
    cmeta_mutex_lock(&race->mutex);
    race->inbound_result = rc;
    race->inbound_done = 1;
    cmeta_cond_broadcast(&race->cond);
    cmeta_mutex_unlock(&race->mutex);
}

static void close_held_sctp_channel(void *arg) {
    channel_close_race_fixture_t *race = arg;
    cmeta_mutex_lock(&race->mutex);
    race->close_started = 1;
    cmeta_cond_broadcast(&race->cond);
    cmeta_mutex_unlock(&race->mutex);

    turbo_dc_channel_close(race->channel);

    cmeta_mutex_lock(&race->mutex);
    race->close_completed = 1;
    cmeta_cond_broadcast(&race->cond);
    cmeta_mutex_unlock(&race->mutex);
}

static void verify_held_sctp_close_interleaving(void) {
    channel_close_race_fixture_t *race = &channel_close_race;
    memset(race, 0, sizeof(*race));
    cmeta_mutex_init(&race->mutex);
    cmeta_cond_init(&race->cond);
    race->peer = endpoints[0].peer;
    race->channel = endpoints[0].channel;
    race->sid = turbo_dc_channel_get_id(race->channel);
    turbo_dc_channel_set_user_data(race->channel, race);
    turbo_dc_channel_on_message(race->channel, held_channel_message);

    cmeta_thread_t inbound = NULL;
    cmeta_thread_t closer = NULL;
    int inbound_created = cmeta_thread_create(&inbound, feed_held_sctp_message, race) == 0;
    int entered = inbound_created && wait_channel_close_flag(&race->callback_entered);
    int close_created = entered &&
        cmeta_thread_create(&closer, close_held_sctp_channel, race) == 0;
    int close_started = close_created && wait_channel_close_flag(&race->close_started);
    int close_completed = close_started && wait_channel_close_flag(&race->close_completed);

    if (close_completed) {
        /* No new callback may be admitted after the close publication, even
         * while the previously admitted application callback is still held. */
        const uint8_t after = 0x5b;
        feed_sctp_fragment(race->sid, 1, SCTP_PPID_BINARY,
                           &after, sizeof(after), MSG_EOR);
        endpoints[0].channel = NULL;
    }
    cmeta_mutex_lock(&race->mutex);
    race->callback_release = 1;
    cmeta_cond_broadcast(&race->cond);
    cmeta_mutex_unlock(&race->mutex);

    if (inbound_created) {
        (void)cmeta_thread_join(&inbound);
        cmeta_thread_destroy(&inbound);
    }
    if (close_created) {
        (void)cmeta_thread_join(&closer);
        cmeta_thread_destroy(&closer);
        /* Closing thread ran to completion, even if it required releasing
         * the callback after a failed bounded-close check. */
        endpoints[0].channel = NULL;
    }

    check_true(inbound_created);
    check_true(entered);
    check_true(close_created);
    check_true(close_started);
    check_true(close_completed);
    check_true(race->inbound_done);
    check_equal(race->inbound_result, 1);
    check_equal(race->callback_count, 1u);
    check_true(race->callback_finished);
    check_true(race->callback_valid);
    check_null(peer_get_channel(race->peer, race->sid));
    cmeta_cond_destroy(&race->cond);
    cmeta_mutex_destroy(&race->mutex);
}

/* ============================================================================
 * Deterministic native SCTP STATUS query vs peer close (Issue #198)
 * ============================================================================ */
typedef struct {
    cmeta_mutex_t lock;
    cmeta_cond_t cond;
    turbo_dc_peer_t *peer;
    int operation;
    int notification_dispatched;
    int borrowed;
    int allow_query;
    int query_authorized;
    int status_attempted;
    int worker_completed;
    int close_started;
    int close_finished;
} sctp_status_close_fixture_t;

static sctp_status_close_fixture_t status_close_fixture;

static struct sockaddr_conn native_close_fixture_address(turbo_dc_peer_t *peer) {
    struct sockaddr_conn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sconn_family = AF_CONN;
    addr.sconn_port = htons(SCTP_ASSOCIATION_PORT);
    addr.sconn_addr = peer;
#ifdef HAVE_SCONN_LEN
    addr.sconn_len = sizeof(addr);
#endif
    return addr;
}

static void status_borrow_worker(void *arg) {
    sctp_status_close_fixture_t *f = arg;
    struct socket *socket = NULL;
    int acquired = dc_peer_acquire(f->peer) == 0;
    if (acquired) socket = sctp_native_borrow(f->peer);

    cmeta_mutex_lock(&f->lock);
    f->borrowed = socket != NULL;
    cmeta_cond_broadcast(&f->cond);
    while (!f->allow_query)
        cmeta_cond_wait(&f->cond, &f->lock);
    int authorized = f->query_authorized;
    cmeta_mutex_unlock(&f->lock);

    /* In a RED control without a terminal close gate, the old close may
     * have freed the socket already. Do not intentionally dereference an
     * invalid handle merely to prove a missing close-admission barrier. */
    if (socket && authorized) {
        if (f->operation == 1) {
            /* A COMM_UP notification is invoked synchronously from within
             * an already-admitted native conninput operation. Deliver the
             * real notification handler after foreign close has published
             * the terminal native gate, but before its borrower exits. The
             * user callback cannot create a fresh native borrow here. */
            struct sctp_assoc_change *notification =
                calloc(1, sizeof(*notification));
            if (notification) {
                union sctp_sockstore address = {0};
                struct sctp_rcvinfo rcv = {0};
                notification->sac_type = SCTP_ASSOC_CHANGE;
                notification->sac_length = sizeof(*notification);
                notification->sac_state = SCTP_COMM_UP;
                (void)sctp_inbound_packet_cb(socket, address, notification,
                                             sizeof(*notification), rcv,
                                             MSG_NOTIFICATION, f->peer);
                f->notification_dispatched = 1;
            }
        } else if (f->operation == 3) {
            /* Native sendv may legitimately reject an unconnected Socket.
             * Only its validity under terminal close is asserted. */
            uint8_t payload = 0x5a;
            struct sctp_sndinfo info = {0};
            info.snd_sid = 0;
            info.snd_ppid = htonl(SCTP_PPID_BINARY);
            (void)usrsctp_sendv(socket, &payload, sizeof(payload), NULL, 0,
                                &info, sizeof(info), SCTP_SENDV_SNDINFO, 0);
        } else if (f->operation == 4) {
            _Alignas(struct sctp_reset_streams)
            uint8_t bytes[sizeof(struct sctp_reset_streams) +
                          sizeof(uint16_t)] = {0};
            struct sctp_reset_streams *reset =
                (struct sctp_reset_streams *)bytes;
            reset->srs_assoc_id = SCTP_ALL_ASSOC;
            reset->srs_flags = SCTP_STREAM_RESET_OUTGOING;
            reset->srs_number_streams = 1;
            reset->srs_stream_list[0] = 0;
            (void)usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_RESET_STREAMS,
                                     bytes, sizeof(bytes));
        } else if (f->operation == 5 || f->operation == 6) {
            struct sockaddr_conn addr = native_close_fixture_address(f->peer);
            if (f->operation == 5)
                (void)usrsctp_bind(socket, (struct sockaddr *)&addr,
                                   sizeof(addr));
            else
                (void)usrsctp_connect(socket, (struct sockaddr *)&addr,
                                      sizeof(addr));
        } else {
            struct sctp_status status = {0};
            socklen_t len = sizeof(status);
            /* An unconnected native Socket may report a normal SCTP error;
             * only the borrow/close ordering is the contract. */
            (void)usrsctp_getsockopt(socket, IPPROTO_SCTP, SCTP_STATUS,
                                     &status, &len);
        }
    }
    if (socket) sctp_native_return(f->peer);
    if (acquired) dc_peer_release(f->peer);

    cmeta_mutex_lock(&f->lock);
    f->status_attempted = socket != NULL && authorized;
    f->worker_completed = 1;
    cmeta_cond_broadcast(&f->cond);
    cmeta_mutex_unlock(&f->lock);
}

static void status_close_worker(void *arg) {
    sctp_status_close_fixture_t *f = arg;
    cmeta_mutex_lock(&f->lock);
    f->close_started = 1;
    cmeta_cond_broadcast(&f->cond);
    cmeta_mutex_unlock(&f->lock);

    turbo_dc_peer_close(f->peer);

    cmeta_mutex_lock(&f->lock);
    f->close_finished = 1;
    cmeta_cond_broadcast(&f->cond);
    cmeta_mutex_unlock(&f->lock);
}

static int wait_status_fixture_flag(int *flag) {
    int found;
    const uint64_t deadline = cmeta_monotonic_ms() + 2000u;
    cmeta_mutex_lock(&status_close_fixture.lock);
    while (!*flag && cmeta_monotonic_ms() < deadline) {
        (void)cmeta_cond_timedwait(&status_close_fixture.cond,
                                   &status_close_fixture.lock,
                                   UINT64_C(10000000));
    }
    found = *flag;
    cmeta_mutex_unlock(&status_close_fixture.lock);
    return found;
}

static void run_sctp_status_close_interleaving(int operation) {
    sctp_status_close_fixture_t *f = &status_close_fixture;
    memset(f, 0, sizeof(*f));
    cmeta_mutex_init(&f->lock);
    cmeta_cond_init(&f->cond);
    f->peer = endpoints[0].peer;
    f->operation = operation;

    /* The Stream Reset RED case must retain a genuine channel on the live
     * peer before a foreign close seals SCTP native admission. */
    turbo_dc_channel_t *close_channel = operation == 2
        ? turbo_dc_channel_create(f->peer, "close-after-native-gate", NULL) : NULL;
    int channel_created = operation != 2 || close_channel != NULL;

    /* Real usrsctp_connect requires a bound socket. Satisfy only that
     * prerequisite before terminal close; the actual connect call remains
     * in the held worker after the close gate is published. */
    if (operation == 6) {
        struct socket *native = sctp_native_borrow(f->peer);
        struct sockaddr_conn addr = native_close_fixture_address(f->peer);
        int prebound = native &&
            usrsctp_bind(native, (struct sockaddr *)&addr,
                         sizeof(addr)) == 0;
        if (native) sctp_native_return(f->peer);
        if (!prebound) {
            check_true(prebound);
            cmeta_cond_destroy(&f->cond);
            cmeta_mutex_destroy(&f->lock);
            return;
        }
    }

    cmeta_thread_t query_thread = NULL;
    cmeta_thread_t closer_thread = NULL;
    int query_created = cmeta_thread_create(&query_thread,
                                            status_borrow_worker, f) == 0;
    int held = query_created && wait_status_fixture_flag(&f->borrowed);
    int close_created = held &&
        cmeta_thread_create(&closer_thread, status_close_worker, f) == 0;
    int closing_started = close_created &&
        wait_status_fixture_flag(&f->close_started);

    /* Wait on the *actual close admission bit* (condition-variable signal),
     * not an arbitrary sleep. This is the deterministic pre-release barrier. */
    int gate_observed = 0, held_count = 0, socket_live = 0;
    const uint64_t deadline = cmeta_monotonic_ms() + 2000u;
    if (closing_started) {
        cmeta_mutex_lock(&f->peer->operation_mutex);
        while (!f->peer->sctp.native_close_requested &&
               cmeta_monotonic_ms() < deadline) {
            (void)cmeta_cond_timedwait(&f->peer->operation_cond,
                                       &f->peer->operation_mutex,
                                       UINT64_C(10000000));
        }
        gate_observed = f->peer->sctp.native_close_requested;
        held_count = (int)f->peer->sctp.native_borrows;
        socket_live = f->peer->sctp.socket != NULL;
        cmeta_mutex_unlock(&f->peer->operation_mutex);
    }

    /* The terminal gate rejects any new native borrow, including from this
     * separate test owner, even as an older borrow keeps the handle alive. */
    struct socket *late = gate_observed ? sctp_native_borrow(f->peer) : NULL;
    int late_admission_rejected = gate_observed && late == NULL;
    if (late) sctp_native_return(f->peer);

    /* An admitted native Socket is still live, but terminal admission is
     * already sealed. Closing its channel must not attempt a fresh reset or
     * manufacture SCTP_SEND errors. This is deterministic even on an
     * unconnected association: no Socket memory is intentionally freed. */
    int reset_skipped_cleanly = 0;
    if (operation == 2 && close_channel) {
        turbo_dc_error_code_t before = turbo_dc_peer_get_error(f->peer).code;
        turbo_dc_channel_close(close_channel);
        close_channel = NULL;
        turbo_dc_error_code_t after = turbo_dc_peer_get_error(f->peer).code;
        reset_skipped_cleanly = before == after;
    }

    cmeta_mutex_lock(&f->lock);
    int prematurely_closed = f->close_finished;
    f->query_authorized = gate_observed && !prematurely_closed;
    f->allow_query = 1;
    cmeta_cond_broadcast(&f->cond);
    cmeta_mutex_unlock(&f->lock);

    if (query_created) {
        (void)cmeta_thread_join(&query_thread);
        cmeta_thread_destroy(&query_thread);
    }
    if (close_created) {
        (void)cmeta_thread_join(&closer_thread);
        cmeta_thread_destroy(&closer_thread);
    }

    check_true(query_created);
    check_true(held);
    check_true(close_created);
    check_true(closing_started);
    check_true(gate_observed);
    check_equal(held_count, 1);
    check_true(socket_live);
    check_true(late_admission_rejected);
    check_false(prematurely_closed);
    check_true(f->status_attempted);
    check_true(f->worker_completed);
    check_true(f->close_finished);
    cmeta_mutex_lock(&f->peer->operation_mutex);
    int physically_closed = f->peer->sctp.native_closed;
    int still_closing = f->peer->sctp.native_closing;
    struct socket *remaining_socket = f->peer->sctp.socket;
    unsigned remaining_borrows = f->peer->sctp.native_borrows;
    cmeta_mutex_unlock(&f->peer->operation_mutex);
    check_true(physically_closed);
    check_false(still_closing);
    check_null(remaining_socket);
    check_equal(remaining_borrows, 0u);
    if (operation == 2) {
        check_true(channel_created);
        /* RED: old channel-close reads peer->sctp.socket without its owner
         * lock, then diagnoses a terminal-gate borrow refusal as a native
         * stream-reset failure. Terminal close is not a send error. */
        check_true(reset_skipped_cleanly);
        check_equal(turbo_dc_peer_get_state(f->peer), TURBO_DC_STATE_CLOSED);
    }
    if (operation == 1) {
        cmeta_mutex_lock(&endpoints[0].mutex);
        unsigned connected_after_gate = endpoints[0].connected_events;
        cmeta_mutex_unlock(&endpoints[0].mutex);
        check_true(f->notification_dispatched);
        /* A delayed COMM_UP must not resurrect CONNECTED after the terminal
         * native admission gate. This fails RED if the association callback
         * ignores native_close_requested. */
        check_equal(connected_after_gate, 0u);
        check_equal(turbo_dc_peer_get_state(f->peer), TURBO_DC_STATE_CLOSED);
    }

    cmeta_cond_destroy(&f->cond);
    cmeta_mutex_destroy(&f->lock);
}

spec("SCTP native API close admission") {
    before_each() { setup_packet_endpoints(0, 1200, 1000); }
    after_each() { cleanup_packet_endpoints(); }

    it("retains a STATUS socket borrow until concurrent peer close drains it") {
        run_sctp_status_close_interleaving(0);
    }

    it("rejects delayed SCTP COMM_UP after foreign close seals native admission") {
        run_sctp_status_close_interleaving(1);
    }

    it("skips Stream Reset cleanly after native close admission is sealed") {
        run_sctp_status_close_interleaving(2);
    }

    it("retains native sendv across a foreign terminal close") {
        run_sctp_status_close_interleaving(3);
    }

    it("retains native SCTP_RESET_STREAMS across a foreign terminal close") {
        run_sctp_status_close_interleaving(4);
    }

    it("retains native bind across a foreign terminal close") {
        run_sctp_status_close_interleaving(5);
    }

    it("retains native connect across a foreign terminal close") {
        run_sctp_status_close_interleaving(6);
    }
}

spec("SCTP complete-message receive adapter") {
    before_each() { setup_receive_channel(); }
    after_each() { cleanup_packet_endpoints(); }

    it("retains a borrowed channel across concurrent on_message and close") {
        verify_held_sctp_close_interleaving();
    }

    it("waits for EOR and isolates interleaved streams") {
        const uint8_t first[] = {1, 2}, last[] = {3, 4};
        uint16_t sid = turbo_dc_channel_get_id(endpoints[0].channel);
        feed_sctp_fragment(sid, 7, SCTP_PPID_BINARY, first, sizeof(first), 0);
        feed_sctp_fragment(sid + 2, 9, SCTP_PPID_STRING, first, sizeof(first), 0);
        check_equal(endpoints[0].messages, 0u);
        feed_sctp_fragment(sid, 7, SCTP_PPID_BINARY, last, sizeof(last), MSG_EOR);
        const uint8_t expected[] = {1, 2, 3, 4};
        check_equal(endpoints[0].messages, 1u);
        check_equal(endpoints[0].message_size, sizeof(expected));
        check_equal(memcmp(endpoints[0].message, expected, sizeof(expected)), 0);
        check_equal(endpoints[0].peer->sctp.receive_buffers, 1u);
        feed_sctp_fragment(sid + 2, 9, SCTP_PPID_STRING, last, sizeof(last), MSG_EOR);
        check_equal(endpoints[0].messages, 1u);
        check_equal(endpoints[0].peer->sctp.receive_buffers, 0u);
    }

    it("normalizes an empty message only after its final fragment") {
        const uint8_t payload = 1;
        uint16_t sid = turbo_dc_channel_get_id(endpoints[0].channel);
        feed_sctp_fragment(sid, 0, SCTP_PPID_STRING_EMPTY, &payload, 1, 0);
        check_equal(endpoints[0].messages, 0u);
        feed_sctp_fragment(sid, 0, SCTP_PPID_STRING_EMPTY, &payload, 1, MSG_EOR);
        check_equal(endpoints[0].messages, 1u);
        check_equal(endpoints[0].message_size, (size_t)0);
        check_equal(endpoints[0].message_binary, 0);
    }

    it("assembles DCEP before publishing an incoming channel") {
        const uint8_t open[] = {3, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 'x'};
        feed_sctp_fragment(1, 0, SCTP_PPID_DCEP, open, 6, 0);
        check_null(peer_get_channel(endpoints[0].peer, 1));
        feed_sctp_fragment(1, 0, SCTP_PPID_DCEP, open + 6, sizeof(open) - 6, MSG_EOR);
        turbo_dc_channel_t *channel = peer_get_channel(endpoints[0].peer, 1);
        check_not_null(channel);
        check_equal(turbo_dc_channel_get_label(channel), "x");
        check_equal(endpoints[0].peer->sctp.receive_buffers, 0u);
    }

    it("delivers exactly the advertised receive capacity") {
        static uint8_t payload[TURBO_DC_MAX_MESSAGE_SIZE];
        for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)i;
        uint16_t sid = turbo_dc_channel_get_id(endpoints[0].channel);
        feed_sctp_fragment(sid, 0, SCTP_PPID_BINARY, payload, sizeof(payload) - 1, 0);
        check_equal(endpoints[0].messages, 0u);
        feed_sctp_fragment(sid, 0, SCTP_PPID_BINARY, payload + sizeof(payload) - 1, 1, MSG_EOR);
        check_equal(endpoints[0].messages, 1u);
        check_equal(endpoints[0].message_size, sizeof(payload));
        check_equal(memcmp(endpoints[0].message, payload, sizeof(payload)), 0);
        check_equal(endpoints[0].peer->sctp.receive_buffers, 0u);
    }

    it("rejects excess retained payload without delivering a prefix") {
        static const uint8_t payload[TURBO_DC_MAX_MESSAGE_SIZE] = {0};
        uint16_t sid = turbo_dc_channel_get_id(endpoints[0].channel);
        feed_sctp_fragment(sid, 0, SCTP_PPID_BINARY, payload, sizeof(payload), 0);
        feed_sctp_fragment(sid, 0, SCTP_PPID_BINARY, payload, 1, MSG_EOR);
        check_equal(endpoints[0].messages, 0u);
        check_equal(turbo_dc_peer_get_error(endpoints[0].peer).code, TURBO_DC_ERROR_SCTP_RECEIVE);
        check_equal(turbo_dc_peer_get_state(endpoints[0].peer), TURBO_DC_STATE_FAILED);
        check_equal(endpoints[0].peer->sctp.receive_buffers, 0u);
    }

    it("bounds simultaneous partial messages and releases them on failure") {
        const uint8_t payload = 1;
        for (uint16_t i = 0; i < SCTP_RECEIVE_SLOTS; ++i)
            feed_sctp_fragment(i, 0, SCTP_PPID_BINARY, &payload, 1, 0);
        check_equal(endpoints[0].peer->sctp.receive_buffers, SCTP_RECEIVE_SLOTS);
        feed_sctp_fragment(SCTP_RECEIVE_SLOTS, 0, SCTP_PPID_BINARY, &payload, 1, 0);
        check_equal(endpoints[0].messages, 0u);
        check_equal(turbo_dc_peer_get_error(endpoints[0].peer).code, TURBO_DC_ERROR_SCTP_RECEIVE);
        check_equal(endpoints[0].peer->sctp.receive_buffers, 0u);
    }

    it("discards an aborted partial message before accepting the next message") {
        const uint8_t payload = 1, next = 2;
        uint16_t sid = turbo_dc_channel_get_id(endpoints[0].channel);
        feed_sctp_fragment(sid, 5, SCTP_PPID_BINARY, &payload, 1, 0);
        struct sctp_pdapi_event event = {0};
        event.pdapi_type = SCTP_PARTIAL_DELIVERY_EVENT;
        event.pdapi_length = sizeof(event);
        event.pdapi_indication = SCTP_PARTIAL_DELIVERY_ABORTED;
        event.pdapi_stream = sid;
        event.pdapi_seq = 5;
        feed_sctp_fragment(0, 0, 0, &event, sizeof(event), MSG_NOTIFICATION);
        check_equal(endpoints[0].peer->sctp.receive_buffers, 0u);
        feed_sctp_fragment(sid, 6, SCTP_PPID_BINARY, &next, 1, MSG_EOR);
        check_equal(endpoints[0].messages, 1u);
        check_equal(endpoints[0].message_size, (size_t)1);
        check_equal(endpoints[0].message[0], next);
    }

    it("releases a reset stream without affecting other partial messages") {
        const uint8_t payload = 1;
        uint16_t sid = turbo_dc_channel_get_id(endpoints[0].channel);
        feed_sctp_fragment(sid, 5, SCTP_PPID_BINARY, &payload, 1, 0);
        feed_sctp_fragment(sid + 2, 9, SCTP_PPID_BINARY, &payload, 1, 0);
        uint8_t notification[sizeof(struct sctp_stream_reset_event) + sizeof(uint16_t)] = {0};
        struct sctp_stream_reset_event header = {0};
        header.strreset_type = SCTP_STREAM_RESET_EVENT;
        header.strreset_flags = SCTP_STREAM_RESET_INCOMING_SSN;
        header.strreset_length = sizeof(notification);
        memcpy(notification, &header, sizeof(header));
        memcpy(notification + sizeof(header), &sid, sizeof(sid));
        feed_sctp_fragment(0, 0, 0, notification, sizeof(notification), MSG_NOTIFICATION);
        check_equal(endpoints[0].peer->sctp.receive_buffers, 1u);
        feed_sctp_fragment(sid, 0, SCTP_PPID_BINARY, &payload, 1, MSG_EOR);
        check_equal(endpoints[0].messages, 1u);
        check_equal(endpoints[0].message_size, (size_t)1);
    }

    it("rejects a changed partial-message identity instead of joining different messages") {
        const uint8_t payload = 1;
        uint16_t sid = turbo_dc_channel_get_id(endpoints[0].channel);
        feed_sctp_fragment(sid, 5, SCTP_PPID_BINARY, &payload, 1, 0);
        feed_sctp_fragment(sid, 6, SCTP_PPID_BINARY, &payload, 1, MSG_EOR);
        check_equal(endpoints[0].messages, 0u);
        check_equal(turbo_dc_peer_get_error(endpoints[0].peer).code, TURBO_DC_ERROR_SCTP_RECEIVE);
        check_equal(endpoints[0].peer->sctp.receive_buffers, 0u);
    }

    it("retains a completed buffer and its budget through callback channel close") {
        const uint8_t payload = 1;
        uint16_t sid = turbo_dc_channel_get_id(endpoints[0].channel);
        turbo_dc_channel_on_message(endpoints[0].channel, close_receiving_channel);
        feed_sctp_fragment(sid, 0, SCTP_PPID_BINARY, &payload, 1, 0);
        feed_sctp_fragment(sid, 0, SCTP_PPID_BINARY, &payload, 1, MSG_EOR);
        check_equal(endpoints[0].messages, 1u);
        check_equal(endpoints[0].message_size, (size_t)2);
        check_false(endpoints[0].message_invalid);
        check_equal(endpoints[0].message[0], payload);
        check_equal(endpoints[0].message[1], payload);
        check_equal(endpoints[0].peer->sctp.receive_buffers, 0u);
    }

    it("releases partial storage at close and never publishes the tail") {
        const uint8_t payload = 1;
        uint16_t sid = turbo_dc_channel_get_id(endpoints[0].channel);
        feed_sctp_fragment(sid, 0, SCTP_PPID_BINARY, &payload, 1, 0);
        turbo_dc_channel_close(endpoints[0].channel);
        endpoints[0].channel = NULL;
        check_equal(endpoints[0].peer->sctp.receive_buffers, 0u);
        feed_sctp_fragment(sid, 0, SCTP_PPID_BINARY, &payload, 1, MSG_EOR);
        check_equal(endpoints[0].messages, 0u);
        feed_sctp_fragment(sid, 1, SCTP_PPID_BINARY, &payload, 1, 0);
        turbo_dc_peer_close(endpoints[0].peer);
        check_equal(endpoints[0].peer->sctp.receive_buffers, 0u);
    }
}

spec("SCTP partial delivery over GmSSL") {
    before_each() { setup_packet_endpoints(0, 1200, 1000); }
    after_each() { cleanup_packet_endpoints(); }

    it("delivers one complete 64 KiB message per direction across partial callbacks") {
        open_packet_channel();
        for (int i = 0; i < 2; ++i) {
            uint32_t point = 1024;
            check_equal(usrsctp_setsockopt(endpoints[i].peer->sctp.socket, IPPROTO_SCTP,
                SCTP_PARTIAL_DELIVERY_POINT, &point, sizeof(point)), 0);
        }
        static uint8_t payload[64 * 1024];
        for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i * 17);
        for (int i = 0; i < 2; ++i)
            check_equal(turbo_dc_channel_send(endpoints[i].channel, payload, sizeof(payload), 1), 0);
        expect_packet_messages(1, payload, sizeof(payload), 1);
        for (int i = 0; i < 2; ++i) {
            check_true(endpoints[i].partial_observed);
            check_equal(endpoints[i].peer->sctp.receive_buffers, 0u);
        }
    }
}
