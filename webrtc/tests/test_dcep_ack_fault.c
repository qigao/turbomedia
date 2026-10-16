/*
 * #232 PRIVATE source-level native send admission failure fixture.
 * The production DCEP source is included verbatim below, with only its
 * usrsctp_sendv call intercepted in THIS test translation unit. Production
 * DataChannel, SCTP, DTLS, public SDK and ABI never see this replacement.
 *
 * This explicitly tests a synthetic native -1/EAGAIN result, not an actual
 * full SCTP queue, remote receipt or browser interoperability. A successful
 * send is NEVER fabricated.
 */
#include "turbo_datachannel_internal.h"
#include "tinytest.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <stdio.h>
/* Test-process-only breadcrumbs: constant phase names, never addresses,
 * stack memory, ICE credentials, SDP, network packets or raw dumps. */
static void tm_ack_stage(const char *stage) {
    fprintf(stderr, "[dcep-ack-close-phase] %s\n", stage);
    fflush(stderr);
}
#else
static void tm_ack_stage(const char *stage) { (void)stage; }
#endif

static unsigned g_send_calls;
static unsigned g_invalid_send_shape;
static int g_injected_errno;

/* One private native-send admission barrier shared by this test's worker and
 * the foreign peer-close worker. The actual usrsctp Socket remains borrowed
 * for the entire wait; no mock send success, free or second Owner is used. */
typedef struct tm_ack_send_close_race {
    cmeta_mutex_t mutex;
    cmeta_cond_t cond;
    turbo_dc_peer_t *peer;
    int send_entered;
    int send_release;
    int parser_done;
    int close_started;
    int close_finished;
} tm_ack_send_close_race;
static tm_ack_send_close_race *g_ack_send_close_race;

static ssize_t tm_test_sendv_eagain(struct socket *socket,
                                    const void *data, size_t length,
                                    struct sockaddr *address, int address_count,
                                    void *info, socklen_t info_length,
                                    unsigned int info_type, int flags) {
    ++g_send_calls;
    if (!socket || !data || length != 1u ||
        ((const uint8_t *)data)[0] != DCEP_DATA_CHANNEL_ACK ||
        address != NULL || address_count != 0 || !info ||
        info_length != sizeof(struct sctp_sendv_spa) ||
        info_type != SCTP_SENDV_SPA || flags != 0) {
        ++g_invalid_send_shape;
    } else {
        const struct sctp_sendv_spa *spa = (const struct sctp_sendv_spa *)info;
        if (!(spa->sendv_flags & SCTP_SEND_SNDINFO_VALID) ||
            spa->sendv_sndinfo.snd_sid != 0 ||
            ntohl(spa->sendv_sndinfo.snd_ppid) != SCTP_PPID_DCEP)
            ++g_invalid_send_shape;
    }
    tm_ack_send_close_race *race = g_ack_send_close_race;
    if (race) {
        cmeta_mutex_lock(&race->mutex);
        race->send_entered = 1;
        tm_ack_stage("sendv-entered");
        cmeta_cond_broadcast(&race->cond);
        /* This actual borrowed-Socket ACK sendv cannot complete until the
         * foreign close thread has published terminal native admission. */
        while (!race->send_release)
            cmeta_cond_wait(&race->cond, &race->mutex);
        cmeta_mutex_unlock(&race->mutex);
        tm_ack_stage("sendv-resumed-for-eagain");
    }
    errno = EAGAIN;
    g_injected_errno = errno;
    return -1;
}

/* Windows test-only wrappers around the *same* native/Channel functions:
 * no changed return values, mock success, or alternate ownership. These
 * constants identify the exact function interval if the native process
 * fails after sendv EAGAIN and before DCEP parser return. */
static void tm_trace_native_return(turbo_dc_peer_t *peer) {
    if (g_ack_send_close_race) tm_ack_stage("native-return-enter");
    sctp_native_return(peer);
    if (g_ack_send_close_race) tm_ack_stage("native-return-exit");
}
static void tm_trace_channel_close(turbo_dc_channel_t *channel) {
    if (g_ack_send_close_race) tm_ack_stage("channel-close-enter");
    turbo_dc_channel_close(channel);
    if (g_ack_send_close_race) tm_ack_stage("channel-close-exit");
}
static void tm_trace_channel_release(turbo_dc_channel_t *channel) {
    if (g_ack_send_close_race) tm_ack_stage("channel-release-enter");
    dc_channel_release(channel);
    if (g_ack_send_close_race) tm_ack_stage("channel-release-exit");
}
static void tm_trace_peer_error(turbo_dc_peer_t *peer,
                                turbo_dc_error_code_t code,
                                const char *detail) {
    if (g_ack_send_close_race) tm_ack_stage("peer-error-enter");
    dc_set_peer_error(peer, code, detail);
    if (g_ack_send_close_race) tm_ack_stage("peer-error-exit");
}
/* These macros affect only the included original production DCEP TU.
 * The real call graph, native Socket Borrow, user callbacks, and public
 * product/SDK linkage are entirely unchanged. */
#define usrsctp_sendv tm_test_sendv_eagain
#define sctp_native_return tm_trace_native_return
#define turbo_dc_channel_close tm_trace_channel_close
#define dc_channel_release tm_trace_channel_release
#define dc_set_peer_error tm_trace_peer_error
#include "../src/dcep_protocol.c"
#undef dc_set_peer_error
#undef dc_channel_release
#undef turbo_dc_channel_close
#undef sctp_native_return
#undef usrsctp_sendv

typedef struct tm_dcep_fault_observer {
    unsigned on_channel;
    unsigned on_open;
} tm_dcep_fault_observer;

static void tm_fault_on_open(turbo_dc_channel_t *channel, void *user_data) {
    (void)channel;
    ++((tm_dcep_fault_observer *)user_data)->on_open;
}

static void tm_fault_on_channel(turbo_dc_peer_t *peer,
                                turbo_dc_channel_t *channel, void *user_data) {
    tm_dcep_fault_observer *obs = (tm_dcep_fault_observer *)user_data;
    (void)peer;
    ++obs->on_channel;
    turbo_dc_channel_set_user_data(channel, obs);
    turbo_dc_channel_on_open(channel, tm_fault_on_open);
}

spec("DCEP native sendv EAGAIN admission #232") {
    it("rejects native EAGAIN without callbacks or retained channel IDs") {
        turbo_dc_config_t cfg = {0};
        tm_dcep_fault_observer obs = {0};
        cfg.is_server = 1;
        cfg.transport = TURBO_DC_TRANSPORT_ICE;
        turbo_dc_context_t *ctx = turbo_dc_context_create(&cfg);
        check_not_null(ctx);
        turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, NULL, 0, &obs);
        check_not_null(peer);
        check_not_null(peer->sctp.socket);
        turbo_dc_peer_on_channel(peer, tm_fault_on_channel);

        static const uint8_t open[] = {
            DCEP_DATA_CHANNEL_OPEN, DCEP_CHANNEL_RELIABLE_ORDERED,
            0, 0, 0, 0, 0, 0,
            0, 1, 0, 0, 'x'
        };
        g_send_calls = 0;
        g_invalid_send_shape = 0;
        g_injected_errno = 0;
        /* Non-NULL native Socket exists; exactly the ACK send admission
         * itself is rejected with a deterministic EAGAIN. */
        dcep_handle_message(peer, 0, open, sizeof(open));

        cmeta_mutex_lock(&peer->operation_mutex);
        int retained = peer_get_channel(peer, 0) != NULL;
        int id_reserved = roaring_bitmap_contains(peer->channel_ids, 0);
        cmeta_mutex_unlock(&peer->operation_mutex);
        turbo_dc_error_t reported = turbo_dc_peer_get_error(peer);
        turbo_dc_peer_destroy(peer);
        turbo_dc_context_destroy(ctx);

        check_equal(g_send_calls, 1u);
        check_equal(g_invalid_send_shape, 0u);
        check_equal(g_injected_errno, EAGAIN);
        check_equal(obs.on_channel, 0u);
        check_equal(obs.on_open, 0u);
        check_false(retained);
        check_false(id_reserved);
        check_equal(reported.code, TURBO_DC_ERROR_DCEP_SEND_ACK);
    }
}

/* Real production DCEP parser on one Salts thread; the private test-only
 * sendv interception leaves the actual Owner borrow active until release. */
static void tm_ack_receiving_worker(void *ctx) {
    tm_ack_send_close_race *race = (tm_ack_send_close_race *)ctx;
    static const uint8_t open[] = {
        DCEP_DATA_CHANNEL_OPEN, DCEP_CHANNEL_RELIABLE_ORDERED,
        0, 0, 0, 0, 0, 0,
        0, 1, 0, 0, 'x'
    };
    tm_ack_stage("parser-enter");
    dcep_handle_message(race->peer, 0, open, sizeof(open));
    tm_ack_stage("parser-return");
    cmeta_mutex_lock(&race->mutex);
    race->parser_done = 1;
    cmeta_cond_broadcast(&race->cond);
    cmeta_mutex_unlock(&race->mutex);
}

static void tm_ack_foreign_close_worker(void *ctx) {
    tm_ack_send_close_race *race = (tm_ack_send_close_race *)ctx;
    cmeta_mutex_lock(&race->mutex);
    race->close_started = 1;
    cmeta_cond_broadcast(&race->cond);
    cmeta_mutex_unlock(&race->mutex);

    /* This is the original public peer close, not a test-only retirement. */
    tm_ack_stage("close-enter");
    turbo_dc_peer_close(race->peer);
    tm_ack_stage("close-return");
    cmeta_mutex_lock(&race->mutex);
    race->close_finished = 1;
    cmeta_cond_broadcast(&race->cond);
    cmeta_mutex_unlock(&race->mutex);
}

/* Timed waits only guard a broken test's diagnostic exit. Successful
 * ordering relies on native Owner gate and explicit condition handoffs. */
static int tm_ack_wait_until(tm_ack_send_close_race *race, int *flag) {
    const uint64_t deadline = cmeta_monotonic_ms() + 5000u;
    cmeta_mutex_lock(&race->mutex);
    while (!*flag && cmeta_monotonic_ms() < deadline) {
        (void)cmeta_cond_timedwait(&race->cond, &race->mutex,
                                   UINT64_C(10000000));
    }
    int reached = *flag;
    cmeta_mutex_unlock(&race->mutex);
    return reached;
}

spec("DCEP sendv failure versus foreign native peer close #201 #232") {
    it("drains borrowed ACK sendv before closing and never publishes OPEN") {
        turbo_dc_config_t cfg = {0};
        tm_dcep_fault_observer obs = {0};
        cfg.is_server = 1;
        cfg.transport = TURBO_DC_TRANSPORT_ICE;
        turbo_dc_context_t *ctx = turbo_dc_context_create(&cfg);
        check_not_null(ctx);
        turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, NULL, 0, &obs);
        check_not_null(peer);
        check_not_null(peer->sctp.socket);
        tm_ack_stage("peer-created");
        turbo_dc_peer_on_channel(peer, tm_fault_on_channel);

        tm_ack_send_close_race race = {0};
        race.peer = peer;
        cmeta_mutex_init(&race.mutex);
        cmeta_cond_init(&race.cond);
        g_send_calls = 0;
        g_invalid_send_shape = 0;
        g_injected_errno = 0;
        g_ack_send_close_race = &race;

        cmeta_thread_t sending = NULL;
        cmeta_thread_t closing = NULL;
        int sender_created = cmeta_thread_create(
            &sending, tm_ack_receiving_worker, &race) == 0;
        tm_ack_stage("sender-started");
        int entered = sender_created &&
            tm_ack_wait_until(&race, &race.send_entered);
        int close_created = entered && cmeta_thread_create(
            &closing, tm_ack_foreign_close_worker, &race) == 0;
        int close_started = close_created &&
            tm_ack_wait_until(&race, &race.close_started);

        int owner_terminal = 0;
        unsigned owner_borrows = 0;
        int socket_still_live = 0;
        if (close_started) {
            const uint64_t deadline = cmeta_monotonic_ms() + 5000u;
            cmeta_mutex_lock(&peer->operation_mutex);
            while (!peer->sctp.native_close_requested &&
                   cmeta_monotonic_ms() < deadline) {
                (void)cmeta_cond_timedwait(&peer->operation_cond,
                    &peer->operation_mutex, UINT64_C(10000000));
            }
            owner_terminal = peer->sctp.native_close_requested;
            owner_borrows = peer->sctp.native_borrows;
            socket_still_live = peer->sctp.socket != NULL;
            cmeta_mutex_unlock(&peer->operation_mutex);
        }

        tm_ack_stage("owner-gate-sampled");
        cmeta_mutex_lock(&race.mutex);
        int prematurely_closed = race.close_finished;
        race.send_release = 1;
        cmeta_cond_broadcast(&race.cond);
        cmeta_mutex_unlock(&race.mutex);

        /* Always unwind joined worker threads before asserting or freeing
         * the peer: even a failed barrier must not manufacture an unsafe UAF. */
        if (sender_created) {
            (void)cmeta_thread_join(&sending);
            cmeta_thread_destroy(&sending);
        }
        if (close_created) {
            (void)cmeta_thread_join(&closing);
            cmeta_thread_destroy(&closing);
        }
        g_ack_send_close_race = NULL;
        tm_ack_stage("all-workers-joined");

        cmeta_mutex_lock(&race.mutex);
        int parser_done = race.parser_done;
        int close_done = race.close_finished;
        cmeta_mutex_unlock(&race.mutex);
        cmeta_mutex_lock(&peer->operation_mutex);
        unsigned remaining_borrows = peer->sctp.native_borrows;
        int physical_closed = peer->sctp.native_closed;
        int close_in_flight = peer->sctp.native_closing;
        struct socket *remaining_socket = peer->sctp.socket;
        int channel_retained = peer_get_channel(peer, 0) != NULL;
        int sid_reserved = roaring_bitmap_contains(peer->channel_ids, 0);
        cmeta_mutex_unlock(&peer->operation_mutex);
        turbo_dc_error_t reported = turbo_dc_peer_get_error(peer);
        tm_ack_stage("peer-destroy-enter");
        turbo_dc_peer_destroy(peer);
        tm_ack_stage("peer-destroy-exit");
        turbo_dc_context_destroy(ctx);
        cmeta_cond_destroy(&race.cond);
        cmeta_mutex_destroy(&race.mutex);

        check_true(sender_created);
        check_true(entered);
        check_true(close_created);
        check_true(close_started);
        check_true(owner_terminal);
        check_equal(owner_borrows, 1u);
        check_true(socket_still_live);
        check_false(prematurely_closed);
        check_true(parser_done);
        check_true(close_done);
        check_equal(g_send_calls, 1u);
        check_equal(g_invalid_send_shape, 0u);
        check_equal(g_injected_errno, EAGAIN);
        check_equal(obs.on_channel, 0u);
        check_equal(obs.on_open, 0u);
        check_false(channel_retained);
        check_false(sid_reserved);
        check_equal(remaining_borrows, 0u);
        check_true(physical_closed);
        check_false(close_in_flight);
        check_null(remaining_socket);
        check_equal(reported.code, TURBO_DC_ERROR_DCEP_SEND_ACK);
    }
}
