/* Exercise real DTLS transactions and timer-owner callbacks, without an SFU. */
#include "turbo_datachannel_internal.h"
#include "turbo_srtp_defs.h"
#include "tinytest.h"
#include <cstl/deque.h>
#include <string.h>
#include <stdlib.h>

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

static void queue_packet(void *transport, const void *data, size_t len) {
    packet_endpoint_t *endpoint = transport;
    /* A transport callback may reenter DTLS APIs; production must release
     * admission before invoking it. Do not assert on a worker thread. */
    srtp_keying_material_t keys;
    uint16_t profile = turbo_dc_peer_get_srtp_keys(endpoint->peer, &keys);
    tstr packet = tstr_new_len(data, len);
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
    cmeta_mutex_unlock(&endpoint->mutex);
}

static void record_packet_endpoint_state(turbo_dc_peer_t *peer,
                                         turbo_dc_state_t old_state,
                                         turbo_dc_state_t state, void *user) {
    packet_endpoint_t *endpoint = user;
    (void)peer; (void)old_state;
    cmeta_mutex_lock(&endpoint->mutex);
    if (state == TURBO_DC_STATE_CONNECTED) ++endpoint->connected_events;
    cmeta_mutex_unlock(&endpoint->mutex);
}

static void setup_packet_endpoints(int disable_sctp, uint16_t dtls_mtu, uint16_t sctp_mtu) {
    memset(endpoints, 0, sizeof(endpoints));
    for (int i = 0; i < 2; ++i) cmeta_mutex_init(&endpoints[i].mutex);
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
        for (int step = 0; step < 8; ++step) {
            deliver_packets(0);
            deliver_packets(1);
        }
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
        /* Includes the cookie exchange; do not couple this ownership test to
         * a provider-specific number of handshake flights. */
        for (int step = 0; step < 8; ++step) {
            deliver_packets(0);
            deliver_packets(1);
            if (turbo_dc_peer_get_srtp_keys(endpoints[1].peer, &keys[1])) break;
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

spec("SCTP complete-message receive adapter") {
    before_each() { setup_receive_channel(); }
    after_each() { cleanup_packet_endpoints(); }

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
