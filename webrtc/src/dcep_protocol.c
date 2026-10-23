/**
 * dcep_protocol.c - WebRTC Data Channel Establishment Protocol (DCEP)
 *
 * RFC 8832 - WebRTC Data Channel Establishment Protocol
 *
 * DCEP handles the negotiation of data channels over an existing SCTP association.
 * Messages are sent on a specific SCTP stream to open/acknowledge channels.
 */

#include "turbo_datachannel_internal.h"
#include <platform.h>
#include <tstr.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#ifdef TURBO_MEDIA_TEST_DCEP_SENDV
ssize_t turbo_media_test_dcep_sendv(
    struct socket *socket, const void *data, size_t length,
    struct sockaddr *address, int address_count, void *info,
    socklen_t info_length, unsigned int info_type, int flags);
#endif
#include "tlog.h"

/* DCEP header offsets */
#define DCEP_HEADER_LENGTH    12
#define DCEP_LABEL_LEN_OFFSET 8
#define DCEP_PROTO_LEN_OFFSET 10
#define DCEP_LABEL_OFFSET     12

/* ============================================================================
 * DCEP Message Handling
 * ============================================================================ */

void dcep_handle_message(turbo_dc_peer_t *peer, uint16_t stream_id,
                         const uint8_t *data, size_t len)
{
    if (len < 1) return;

    uint8_t msg_type = data[0];

    if (msg_type == DCEP_DATA_CHANNEL_OPEN && len >= DCEP_HEADER_LENGTH) {
        uint16_t label_len = get_be16(data + DCEP_LABEL_LEN_OFFSET);
        uint16_t proto_len = get_be16(data + DCEP_PROTO_LEN_OFFSET);
        uint32_t reliability_parameter = get_be32(data + 4);

        if (label_len > MAX_LABEL_LEN || proto_len > MAX_PROTOCOL_LEN ||
            len != (size_t)(DCEP_HEADER_LENGTH + label_len + proto_len)) {
            return;
        }

        /* RFC 8832 assigns even SIDs to the DTLS client and odd SIDs to the
         * DTLS server. This Peer is the receiver, so the remote role is the
         * opposite of peer->is_dtls_server. */
        if (((stream_id & 1u) == 0u) != (peer->is_dtls_server != 0)) return;

        /* Create incoming channel */
        turbo_dc_channel_t *channel = calloc(1, sizeof(*channel));
        if (!channel) return;

        channel->peer = peer;
        channel->id = stream_id;

        if (label_len > 0) {
            channel->label = tstr_dup_len((const char *)data + DCEP_LABEL_OFFSET, label_len);
        }
        if (proto_len > 0) {
            channel->protocol = tstr_dup_len((const char *)data + DCEP_LABEL_OFFSET + label_len, proto_len);
        }
        if ((label_len > 0 && !channel->label) ||
            (proto_len > 0 && !channel->protocol)) {
            tstr_free(channel->label);
            tstr_free(channel->protocol);
            free(channel);
            return;
        }

        /* Parse channel type */
        uint8_t channel_type = data[1];
        uint8_t reliability = channel_type & (DCEP_CHANNEL_REXMIT | DCEP_CHANNEL_TIMED);
        if (reliability == (DCEP_CHANNEL_REXMIT | DCEP_CHANNEL_TIMED) ||
            (channel_type & ~(DCEP_CHANNEL_RELIABLE_UNORDERED |
                              DCEP_CHANNEL_REXMIT | DCEP_CHANNEL_TIMED)) != 0) {
            tstr_free(channel->label);
            tstr_free(channel->protocol);
            free(channel);
            return;
        }
        channel->config.ordered = !(channel_type & DCEP_CHANNEL_RELIABLE_UNORDERED);
        if (reliability == DCEP_CHANNEL_REXMIT) {
            if (reliability_parameter > (uint32_t)INT_MAX) {
                tstr_free(channel->label);
                tstr_free(channel->protocol);
                free(channel);
                return;
            }
            channel->config.max_retransmits = (int)reliability_parameter;
        } else if (reliability == DCEP_CHANNEL_TIMED) {
            if (reliability_parameter > (uint32_t)INT_MAX) {
                tstr_free(channel->label);
                tstr_free(channel->protocol);
                free(channel);
                return;
            }
            channel->config.max_lifetime_ms = (int)reliability_parameter;
        }

        /* Reserve one callback lease before publication. on_channel may
         * close/unpublish the channel, but cannot reclaim this live scope. */
        int published = 0;
        cmeta_mutex_lock(&peer->operation_mutex);
        if (!peer->destroying &&
            !roaring_bitmap_contains(peer->channel_ids, stream_id) &&
            peer_get_channel(peer, stream_id) == NULL) {
            peer_mark_channel_id(peer, stream_id);
            if (peer_set_channel(peer, stream_id, channel) == 0) {
                channel->callback_refs = 1;
                published = 1;
            } else {
                peer_free_channel_id(peer, stream_id);
            }
        }
        cmeta_mutex_unlock(&peer->operation_mutex);
        if (!published) {
            tstr_free(channel->label);
            tstr_free(channel->protocol);
            free(channel);
            return;
        }
        TLOG_INFOF("DCEP OPEN received sid={} label='{}' proto='{}'",
                  stream_id,
                  channel->label ? channel->label : "",
                  channel->protocol ? channel->protocol : "");

        /* Commit the application-visible OPEN only after the SCTP engine
         * accepts the reliable ordered DCEP ACK for sending. Admission is
         * not evidence of remote delivery. This mirrors the upstream C11
         * cnet_sctp_dcep_input()/cnet_sctp_dcep_ack_admitted() contract. */
        if (dcep_send_ack(peer, stream_id) != 0) {
            /* The Channel was reserved only to reject duplicate OPEN while
             * admitting the ACK. Roll it back inside the already admitted
             * DCEP operation: a public Close could itself be rejected after
             * Peer destruction begins and would leave this private record
             * published. No application callback has observed the Channel. */
            int reset_stream = 0;
            cmeta_mutex_lock(&peer->operation_mutex);
            if (!channel->closing) {
                channel->closing = 1;
                channel->is_open = 0;
                if (peer_get_channel(peer, stream_id) == channel)
                    peer_remove_channel(peer, stream_id);
                reset_stream = !peer->destroying && peer->sctp.socket != NULL;
            }
            cmeta_mutex_unlock(&peer->operation_mutex);
            if (reset_stream &&
                sctp_reset_channel_stream(peer, stream_id) != 0) {
                dc_set_peer_error(peer, TURBO_DC_ERROR_SCTP_SEND,
                                  "rejected DCEP stream reset failed");
            }
            dc_channel_release(channel);
            dc_set_peer_error(peer, TURBO_DC_ERROR_DCEP_SEND_ACK,
                              "incoming DCEP OPEN ACK was not admitted");
            return;
        }

        turbo_dc_channel_cb on_channel = NULL;
        turbo_dc_open_cb on_open = NULL;
        void *callback_data;
        cmeta_mutex_lock(&peer->operation_mutex);
        if (!channel->closing && !peer->destroying) {
            channel->is_open = 1;
            on_channel = peer->on_channel;
        }
        callback_data = peer->user_data;
        cmeta_mutex_unlock(&peer->operation_mutex);
        if (on_channel) on_channel(peer, channel, callback_data);

        /* Recheck terminal admission after the reentrant application call. */
        cmeta_mutex_lock(&peer->operation_mutex);
        if (!channel->closing && !peer->destroying) on_open = channel->on_open;
        callback_data = channel->user_data;
        cmeta_mutex_unlock(&peer->operation_mutex);
        if (on_open) on_open(channel, callback_data);
        dc_channel_release(channel);
    }
    else if (msg_type == DCEP_DATA_CHANNEL_ACK) {
        turbo_dc_channel_t *channel = dc_channel_admit(peer, stream_id);
        if (channel) {
            turbo_dc_open_cb on_open = NULL;
            void *callback_data;
            cmeta_mutex_lock(&peer->operation_mutex);
            if (!channel->closing && !peer->destroying && !channel->is_open) {
                channel->is_open = 1;
                on_open = channel->on_open;
            }
            callback_data = channel->user_data;
            cmeta_mutex_unlock(&peer->operation_mutex);
            if (on_open) on_open(channel, callback_data);
            dc_channel_release(channel);
        }
    }
}

int dcep_send_open(turbo_dc_channel_t *channel) {
    turbo_dc_peer_t *peer = channel->peer;
    if (!peer || !peer->sctp.socket) return -1;

    size_t label_len = tstr_len(channel->label);
    size_t proto_len = tstr_len(channel->protocol);
    size_t packet_len = DCEP_HEADER_LENGTH + label_len + proto_len;

    uint8_t packet[DCEP_MAX_PACKET_SIZE];
    if (packet_len > sizeof(packet)) return -1;

    memset(packet, 0, packet_len);

    packet[0] = DCEP_DATA_CHANNEL_OPEN;

    /* Channel type */
    if (channel->config.ordered) {
        packet[1] = DCEP_CHANNEL_RELIABLE_ORDERED;
    } else {
        packet[1] = DCEP_CHANNEL_RELIABLE_UNORDERED;
    }

    if (channel->config.max_retransmits < 0 || channel->config.max_lifetime_ms < 0 ||
        (channel->config.max_retransmits > 0 && channel->config.max_lifetime_ms > 0)) {
        return -1;
    } else if (channel->config.max_retransmits > 0) {
        packet[1] |= DCEP_CHANNEL_REXMIT;
        put_be32(packet + 4, (uint32_t)channel->config.max_retransmits);
    } else if (channel->config.max_lifetime_ms > 0) {
        packet[1] |= DCEP_CHANNEL_TIMED;
        put_be32(packet + 4, (uint32_t)channel->config.max_lifetime_ms);
    }

    put_be16(packet + DCEP_LABEL_LEN_OFFSET, (uint16_t)label_len);
    put_be16(packet + DCEP_PROTO_LEN_OFFSET, (uint16_t)proto_len);

    if (label_len > 0) {
        memcpy(packet + DCEP_LABEL_OFFSET, channel->label, label_len);
    }
    if (proto_len > 0) {
        memcpy(packet + DCEP_LABEL_OFFSET + label_len, channel->protocol, proto_len);
    }

    struct sctp_sendv_spa spa;
    memset(&spa, 0, sizeof(spa));
    spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
    spa.sendv_sndinfo.snd_sid = channel->id;
    spa.sendv_sndinfo.snd_ppid = htonl(SCTP_PPID_DCEP);

#ifdef TURBO_MEDIA_TEST_DCEP_SENDV
    ssize_t sent = turbo_media_test_dcep_sendv(
        peer->sctp.socket, packet, packet_len, NULL, 0, &spa, sizeof(spa),
        SCTP_SENDV_SPA, 0);
#else
    ssize_t sent = usrsctp_sendv(peer->sctp.socket, packet, packet_len,
                                 NULL, 0, &spa, sizeof(spa),
                                 SCTP_SENDV_SPA, 0);
#endif

    TLOG_INFOF("DCEP OPEN send sid={} bytes={} ret={} errno={}",
              channel->id, packet_len, (int)sent, errno);

    return sent == (ssize_t)packet_len ? 0 : -1;
}

int dcep_send_ack(turbo_dc_peer_t *peer, uint16_t stream_id) {
    if (!peer || !peer->sctp.socket) return -1;

    uint8_t packet[1] = { DCEP_DATA_CHANNEL_ACK };

    struct sctp_sendv_spa spa;
    memset(&spa, 0, sizeof(spa));
    spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
    spa.sendv_sndinfo.snd_sid = stream_id;
    spa.sendv_sndinfo.snd_ppid = htonl(SCTP_PPID_DCEP);

#ifdef TURBO_MEDIA_TEST_DCEP_SENDV
    ssize_t sent = turbo_media_test_dcep_sendv(
        peer->sctp.socket, packet, 1, NULL, 0, &spa, sizeof(spa),
        SCTP_SENDV_SPA, 0);
#else
    ssize_t sent = usrsctp_sendv(peer->sctp.socket, packet, 1,
                                  NULL, 0, &spa, sizeof(spa),
                                  SCTP_SENDV_SPA, 0);
#endif

    TLOG_INFOF("DCEP ACK send sid={} ret={} errno={}", stream_id, (int)sent, errno);

    return (sent > 0) ? 0 : -1;
}
