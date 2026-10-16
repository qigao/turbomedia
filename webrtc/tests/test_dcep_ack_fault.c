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

static unsigned g_send_calls;
static unsigned g_invalid_send_shape;
static int g_injected_errno;

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
    errno = EAGAIN;
    g_injected_errno = errno;
    return -1;
}

/* This macro applies only to this test's compilation of the exact source.
 * The native usrsctp.h has already been included above, and the production
 * CMake target compiles dcep_protocol.c normally without this macro. */
#define usrsctp_sendv tm_test_sendv_eagain
#include "../src/dcep_protocol.c"
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
