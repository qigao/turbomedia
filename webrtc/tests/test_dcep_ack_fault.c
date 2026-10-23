/*
 * Private source-level DCEP ACK admission failure fixture.
 *
 * The exact production dcep_protocol.c is compiled into this test translation
 * unit. Only its usrsctp_sendv call is redirected to a deterministic
 * -1/EAGAIN result. The production library, ABI and native SCTP provider are
 * unchanged, and the fixture never fabricates a successful ACK. The private
 * stream-reset call is observed locally because that symbol is not exported
 * from the production Windows DLL.
 */
#include "turbo_datachannel_internal.h"
#include "tinytest.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

static unsigned g_send_calls;
static unsigned g_invalid_send_shape;
static unsigned g_reset_calls;
static unsigned g_invalid_reset_shape;
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
        const struct sctp_sendv_spa *spa =
            (const struct sctp_sendv_spa *)info;
        if (!(spa->sendv_flags & SCTP_SEND_SNDINFO_VALID) ||
            spa->sendv_sndinfo.snd_sid != 0 ||
            ntohl(spa->sendv_sndinfo.snd_ppid) != SCTP_PPID_DCEP) {
            ++g_invalid_send_shape;
        }
    }
    errno = EAGAIN;
    g_injected_errno = errno;
    return -1;
}

static int tm_test_reset_stream(turbo_dc_peer_t *peer, uint16_t stream_id) {
    ++g_reset_calls;
    if (!peer || !peer->sctp.socket || stream_id != 0)
        ++g_invalid_reset_shape;
    return 0;
}

/* These macros apply only to this test's compilation of production DCEP.
 * The ACK admission is rejected, while the internal reset call is observed
 * without requiring a private non-exported Windows DLL symbol. */
#define usrsctp_sendv tm_test_sendv_eagain
#define sctp_reset_channel_stream tm_test_reset_stream
#include "../src/dcep_protocol.c"
#undef sctp_reset_channel_stream
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
                                turbo_dc_channel_t *channel,
                                void *user_data) {
    tm_dcep_fault_observer *observation =
        (tm_dcep_fault_observer *)user_data;
    (void)peer;
    ++observation->on_channel;
    turbo_dc_channel_set_user_data(channel, observation);
    turbo_dc_channel_on_open(channel, tm_fault_on_open);
}

spec("DCEP native ACK admission") {
    it("rejects EAGAIN without callbacks or retained channel state") {
        turbo_dc_config_t config = {0};
        tm_dcep_fault_observer observation = {0};
        config.is_server = 1;
        config.transport = TURBO_DC_TRANSPORT_ICE;

        turbo_dc_context_t *context = turbo_dc_context_create(&config);
        check_not_null(context);
        turbo_dc_peer_t *peer =
            turbo_dc_peer_create(context, NULL, 0, &observation);
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
        g_reset_calls = 0;
        g_invalid_reset_shape = 0;
        g_injected_errno = 0;

        dcep_handle_message(peer, 0, open, sizeof(open));

        cmeta_mutex_lock(&peer->operation_mutex);
        int retained = peer_get_channel(peer, 0) != NULL;
        int id_reserved = roaring_bitmap_contains(peer->channel_ids, 0);
        cmeta_mutex_unlock(&peer->operation_mutex);
        turbo_dc_error_t reported = turbo_dc_peer_get_error(peer);

        turbo_dc_peer_destroy(peer);
        turbo_dc_context_destroy(context);

        check_equal(g_send_calls, 1u);
        check_equal(g_invalid_send_shape, 0u);
        check_equal(g_reset_calls, 1u);
        check_equal(g_invalid_reset_shape, 0u);
        check_equal(g_injected_errno, EAGAIN);
        check_equal(observation.on_channel, 0u);
        check_equal(observation.on_open, 0u);
        check_false(retained);
        check_false(id_reserved);
        check_equal(reported.code, TURBO_DC_ERROR_DCEP_SEND_ACK);
    }
}
