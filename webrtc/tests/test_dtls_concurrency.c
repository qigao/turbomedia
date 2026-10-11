/* Exercise real SSL transactions and timer-owner callbacks, without an SFU. */
#include "turbo_datachannel_internal.h"
#include "turbo_srtp_defs.h"
#include "tinytest.h"
#include <cstl/deque.h>
#include <string.h>

typedef struct {
    turbo_dc_context_t *context;
    turbo_dc_peer_t *peer;
    cmeta_mutex_t mutex;
    cmeta_cond_t cond;
    cmeta_thread_t connect_thread;
    cmeta_thread_t other_thread;
    int connect_started;
    int other_started;
    int ssl_entered;
    int release_ssl;
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

static void pause_ssl_message(int writing, int version, int type,
                              const void *data, size_t len, SSL *ssl, void *arg) {
    dtls_fixture_t *f = (dtls_fixture_t *)arg;
    (void)version; (void)type; (void)data; (void)len; (void)ssl;
    if (!writing) return;
    cmeta_mutex_lock(&f->mutex);
    if (!f->ssl_entered) {
        f->ssl_entered = 1;
        cmeta_cond_broadcast(&f->cond);
        while (!f->release_ssl) cmeta_cond_wait(&f->cond, &f->mutex);
    }
    cmeta_mutex_unlock(&f->mutex);
}

static void discard_transport(void *transport, const void *data, size_t len) {
    dtls_fixture_t *f = (dtls_fixture_t *)transport;
    srtp_keying_material_t keys;
    (void)data; (void)len;
    /* This public reentry must not run under SSL admission. */
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

static void connect_peer(void *arg) {
    dtls_fixture_t *f = (dtls_fixture_t *)arg;
    f->connect_result = turbo_dc_peer_connect(f->peer);
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
    check_equal(turbo_dc_peer_set_external_transport(
        fixture.peer, &fixture, discard_transport), 0);
}

static void cleanup_fixture(void) {
    cmeta_mutex_lock(&fixture.mutex);
    fixture.release_ssl = 1;
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
    /* Set only before publishing the peer to the connect worker. */
    SSL_set_msg_callback(fixture.peer->dtls.ssl, pause_ssl_message);
    SSL_set_msg_callback_arg(fixture.peer->dtls.ssl, &fixture);
    int rc = cmeta_thread_create(&fixture.connect_thread, connect_peer, &fixture);
    fixture.connect_started = rc == 0;
    check_equal(rc, 0);
    check_true(wait_flag(&fixture.ssl_entered));
}

static void start_other(cmeta_thread_cb callback) {
    int rc = cmeta_thread_create(&fixture.other_thread, callback, &fixture);
    fixture.other_started = rc == 0;
    check_equal(rc, 0);
}

static void release_handshake(void) {
    cmeta_mutex_lock(&fixture.mutex);
    fixture.release_ssl = 1;
    cmeta_cond_broadcast(&fixture.cond);
    cmeta_mutex_unlock(&fixture.mutex);
}

spec("DTLS concurrent entry and timer drain") {
    before_each() { setup_fixture(); }
    after_each() { cleanup_fixture(); }

    it("copies incoming packets and reads exactly one packet at a time") {
        /* No handshake is active, so the fixture owns both BIOs exclusively. */
        BIO *bio = fixture.peer->dtls.read_bio;
        char first[] = "first";
        char output[32] = {0};
        check_equal(BIO_write(bio, first, 5), 5);
        check_equal(BIO_write(bio, "second", 6), 6);
        memset(first, 'x', 5);
        check_equal(BIO_pending(bio), (size_t)5);
        check_equal(BIO_read(bio, output, sizeof(output)), 5);
        check_equal(memcmp(output, "first", 5), 0);
        check_equal(BIO_read(bio, output, sizeof(output)), 6);
        check_equal(memcmp(output, "second", 6), 0);
        check_equal(BIO_read(bio, output, sizeof(output)), -1);
        check_true(BIO_should_retry(bio));
    }

    it("discards only the remainder of a short datagram read") {
        BIO *bio = fixture.peer->dtls.read_bio;
        char output[16] = {0};
        check_equal(BIO_write(bio, "first", 5), 5);
        check_equal(BIO_write(bio, "second", 6), 6);
        check_equal(BIO_read(bio, output, 2), 2);
        check_equal(memcmp(output, "fi", 2), 0);
        check_equal(BIO_read(bio, output, sizeof(output)), 6);
        check_equal(memcmp(output, "second", 6), 0);
    }

    it("rejects full packet storage without changing already queued packets") {
        BIO *bio = fixture.peer->dtls.write_bio;
        char output;
        for (unsigned i = 0; i < DTLS_BIO_MAX_PACKETS; ++i)
            check_equal(BIO_write(bio, "a", 1), 1);
        check_equal(BIO_write(bio, "b", 1), -1);
        check_false(BIO_should_retry(bio));
        for (unsigned i = 0; i < DTLS_BIO_MAX_PACKETS; ++i) {
            check_equal(BIO_read(bio, &output, 1), 1);
            check_equal(output, 'a');
        }
        check_equal(BIO_write(bio, "b", 1), 1);
        check_equal(BIO_read(bio, &output, 1), 1);
        check_equal(output, 'b');
    }

    it("bounds queued bytes and releases their budget on reset") {
        BIO *bio = fixture.peer->dtls.write_bio;
        static const char payload[32768] = {0};
        for (size_t bytes = 0; bytes < DTLS_BIO_MAX_BYTES; bytes += sizeof(payload))
            check_equal(BIO_write(bio, payload, sizeof(payload)), (int)sizeof(payload));
        check_equal(BIO_write(bio, "a", 1), -1);
        check_false(BIO_should_retry(bio));
        check_equal(BIO_reset(bio), 1);
        check_equal(BIO_pending(bio), (size_t)0);
        check_equal(BIO_write(bio, payload, sizeof(payload)), (int)sizeof(payload));
        /* Teardown must also release packets left in a nonempty BIO. */
    }

    it("propagates full output storage as a terminal handshake failure") {
        BIO *bio = fixture.peer->dtls.write_bio;
        for (unsigned i = 0; i < DTLS_BIO_MAX_PACKETS; ++i)
            check_equal(BIO_write(bio, "a", 1), 1);
        /* Error classification must happen inside the DataChannel library:
         * the executable may link a distinct BoringSSL error-queue instance. */
        check_equal(turbo_dc_peer_connect(fixture.peer), 0);
        check_equal(turbo_dc_peer_get_state(fixture.peer), TURBO_DC_STATE_FAILED);
        srtp_keying_material_t keys;
        check_equal(turbo_dc_peer_get_srtp_keys(fixture.peer, &keys), (uint16_t)0);
        check_equal(fixture.sends, 0);
    }

    it("serializes key export with an in-progress SSL handshake") {
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
         * not observe SSL until the actual handshake transaction exits. */
        check_true(both_admitted);
        cmeta_mutex_lock(&fixture.mutex);
        int completed_early = fixture.other_done;
        cmeta_mutex_unlock(&fixture.mutex);
        check_false(completed_early);
        release_handshake();
        check_true(wait_flag(&fixture.other_done));
        check_equal(fixture.exported_profile, (uint16_t)0);
    }

    it("drains the live handshake before destroying SSL") {
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
    int send_failed;
} packet_endpoint_t;

static packet_endpoint_t endpoints[2];

static void queue_packet(void *transport, const void *data, size_t len) {
    packet_endpoint_t *endpoint = transport;
    /* A transport callback may reenter SSL APIs; production must release
     * admission before invoking it. Do not assert on a worker thread. */
    srtp_keying_material_t keys;
    (void)turbo_dc_peer_get_srtp_keys(endpoint->peer, &keys);
    tstr packet = tstr_new_len(data, len);
    cmeta_mutex_lock(&endpoint->mutex);
    if (len > endpoint->max_packet) endpoint->max_packet = len;
    ++endpoint->packets;
    if (!packet || deque_push_back(&endpoint->outgoing, &packet) != STL_OK) {
        endpoint->send_failed = 1;
        tstr_free(packet);
    }
    cmeta_mutex_unlock(&endpoint->mutex);
}

static void setup_packet_endpoints(void) {
    memset(endpoints, 0, sizeof(endpoints));
    for (int i = 0; i < 2; ++i) cmeta_mutex_init(&endpoints[i].mutex);
    for (int i = 0; i < 2; ++i) {
        packet_endpoint_t *endpoint = &endpoints[i];
        turbo_dc_config_t config = {0};
        config.transport = TURBO_DC_TRANSPORT_ICE;
        config.disable_sctp = 1;
        config.is_server = i;
        config.dtls_mtu = 256;
        check_equal(deque_init_bytes(&endpoint->outgoing, sizeof(tstr),
                    _Alignof(tstr), DTLS_BIO_MAX_PACKETS), STL_OK);
        endpoint->context = turbo_dc_context_create(&config);
        check_not_null(endpoint->context);
        endpoint->peer = turbo_dc_peer_create(endpoint->context, NULL, 0, NULL);
        check_not_null(endpoint->peer);
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
     * destruction drains its SSL transactions and transport callbacks. */
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
        turbo_dc_peer_feed_transport_data(endpoints[1 - sender].peer, packet, tstr_len(packet));
        tstr_free(packet);
    }
}

spec("DTLS datagram transport handshake") {
    before_each() { setup_packet_endpoints(); }
    after_each() { cleanup_packet_endpoints(); }

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
    }
}
