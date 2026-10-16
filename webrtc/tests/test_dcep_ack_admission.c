/* Dedicated, byte-identical #232 RED/GREEN fixture: only production DCEP
 * source may change the outcome. No test fake of a successful SCTP ACK. */
#include "turbo_datachannel.h"
#include "turbo_datachannel_internal.h"
#include "tinytest.h"
#include <stdint.h>

/* #232 RED: exercise the unchanged production DCEP parser when ACK cannot
 * be admitted because there is no SCTP Socket. A missing native Socket is
 * one real reject path from dcep_send_ack, NOT a simulated successful ACK.
 * The native socket remains owned by the peer and is restored before teardown.
 * Later gates must independently cover actual sendv EAGAIN/queue exhaustion. */
typedef struct dcep_ack_reject_observation {
    unsigned on_channel;
    unsigned on_open;
    unsigned observed_false_open;
} dcep_ack_reject_observation;

static void dcep_unexpected_open(turbo_dc_channel_t *channel, void *user_data) {
    dcep_ack_reject_observation *observation = user_data;
    (void)channel;
    ++observation->on_open;
}

static void dcep_unexpected_channel(turbo_dc_peer_t *peer,
                                    turbo_dc_channel_t *channel, void *user_data) {
    dcep_ack_reject_observation *observation = user_data;
    (void)peer;
    ++observation->on_channel;
    if (turbo_dc_channel_is_open(channel)) ++observation->observed_false_open;
    turbo_dc_channel_set_user_data(channel, observation);
    turbo_dc_channel_on_open(channel, dcep_unexpected_open);
}

spec("DCEP ACK admission rejection #232 RED") {
    it("does not publish an incoming channel on failed ACK admission") {
        turbo_dc_config_t config = {0};
        config.transport = TURBO_DC_TRANSPORT_ICE;
        config.is_server = 1;
        dcep_ack_reject_observation obs = {0};
        turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
        check_not_null(ctx);
        turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, NULL, 0, &obs);
        check_not_null(peer);
        turbo_dc_peer_on_channel(peer, dcep_unexpected_channel);

        const uint8_t open[] = {
            DCEP_DATA_CHANNEL_OPEN, DCEP_CHANNEL_RELIABLE_ORDERED,
            0, 0, 0, 0, 0, 0,
            0, 1, 0, 0, 'x'
        };
        /* Test-only deterministic native Socket absence: the same
         * dcep_send_ack() production path returns failure without injecting
         * any success result or running a fake SCTP backend. */
        struct socket *owned_socket = peer->sctp.socket;
        peer->sctp.socket = NULL;
        const int ack_rc = dcep_send_ack(peer, 0);
        dcep_handle_message(peer, 0, open, sizeof(open));
        int channel_retained = peer_get_channel(peer, 0) != NULL;
        int id_retained = roaring_bitmap_contains(peer->channel_ids, 0);
        unsigned observed_channel = obs.on_channel;
        unsigned observed_open = obs.on_open;
        unsigned observed_false_open = obs.observed_false_open;
        peer->sctp.socket = owned_socket;
        turbo_dc_peer_destroy(peer);
        turbo_dc_context_destroy(ctx);

        check_equal(ack_rc, -1);
        check_equal(observed_channel, 0u);
        check_equal(observed_open, 0u);
        check_equal(observed_false_open, 0u);
        check_equal(channel_retained, 0);
        check_equal(id_retained, 0);
    }
}

