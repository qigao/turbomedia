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

        if (label_len > MAX_LABEL_LEN || proto_len > MAX_PROTOCOL_LEN ||
            len < (size_t)(DCEP_HEADER_LENGTH + label_len + proto_len)) {
            return;
        }

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
            channel->config.max_retransmits = get_be16(data + 4);
        } else if (reliability == DCEP_CHANNEL_TIMED) {
            channel->config.max_lifetime_ms = get_be16(data + 4);
        }

        /* Publish under the same lock used for SCTP message lookup and
         * channel close. Keep one admission lease across both user callbacks:
         * on_channel is allowed to call channel_close reentrantly. */
        int published = 0;
        cmeta_mutex_lock(&peer->operation_mutex);
        if (!peer->destroying && !peer->sctp.receive_stopped &&
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

        /* Send ACK outside admission: a native SCTP/DTLS callback can
         * reenter DCEP. Admission success means the native engine accepted
         * the ACK for sending; it does not assert remote delivery. */
        if (dcep_send_ack(peer, stream_id) != 0) {
            /* Do not notify applications of an OPEN that was never ACKed.
             * Reuse the canonical Channel close/ID retirement path from
             * #195: its already-held DCEP lease keeps 'channel' alive while
             * close unpublishes it and requests a native Stream Reset. No
             * user-visible Channel was published before this point, so
             * there is no on_close callback to emit for this rejection. */
            turbo_dc_channel_close(channel);
            dc_channel_release(channel);
            dc_set_peer_error(peer, TURBO_DC_ERROR_DCEP_SEND_ACK,
                              "incoming DCEP OPEN ACK was not admitted");
            return;
        }
        cmeta_mutex_lock(&peer->operation_mutex);
        if (!channel->closing) channel->is_open = 1;
        cmeta_mutex_unlock(&peer->operation_mutex);

        /* Application callbacks execute without holding operation_mutex. */
        if (peer->on_channel)
            peer->on_channel(peer, channel, peer->user_data);

        cmeta_mutex_lock(&peer->operation_mutex);
        turbo_dc_open_cb callback = !channel->closing ? channel->on_open : NULL;
        void *callback_data = channel->user_data;
        cmeta_mutex_unlock(&peer->operation_mutex);
        if (callback)
            callback(channel, callback_data);
        dc_channel_release(channel);
    }
    else if (msg_type == DCEP_DATA_CHANNEL_ACK) {
        TLOG_INFOF("DCEP ACK received sid={}", stream_id);
        turbo_dc_channel_t *channel = dc_channel_admit(peer, stream_id);
        if (channel) {
            cmeta_mutex_lock(&peer->operation_mutex);
            turbo_dc_open_cb callback = NULL;
            void *callback_data = NULL;
            if (!channel->closing && !channel->is_open) {
                channel->is_open = 1;
                callback = channel->on_open;
                callback_data = channel->user_data;
            }
            cmeta_mutex_unlock(&peer->operation_mutex);
            if (callback)
                callback(channel, callback_data);
            dc_channel_release(channel);
        }
    }
}

int dcep_send_open(turbo_dc_channel_t *channel) {
    turbo_dc_peer_t *peer = channel->peer;
    if (!peer) return -1;

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
    } else if (channel->config.max_retransmits > UINT16_MAX ||
               channel->config.max_lifetime_ms > UINT16_MAX) {
        return -1;
    } else if (channel->config.max_retransmits > 0) {
        packet[1] |= DCEP_CHANNEL_REXMIT;
        put_be16(packet + 4, (uint16_t)channel->config.max_retransmits);
    } else if (channel->config.max_lifetime_ms > 0) {
        packet[1] |= DCEP_CHANNEL_TIMED;
        put_be16(packet + 4, (uint16_t)channel->config.max_lifetime_ms);
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

    struct socket *socket = sctp_native_borrow(peer);
    if (!socket) return -1;
    ssize_t sent = usrsctp_sendv(socket, packet, packet_len,
                                 NULL, 0, &spa, sizeof(spa),
                                 SCTP_SENDV_SPA, 0);
    sctp_native_return(peer);

    TLOG_INFOF("DCEP OPEN send sid={} bytes={} ret={} errno={}",
              channel->id, packet_len, (int)sent, errno);

    return (sent > 0) ? 0 : -1;
}

int dcep_send_ack(turbo_dc_peer_t *peer, uint16_t stream_id) {
    if (!peer) return -1;

    uint8_t packet[1] = { DCEP_DATA_CHANNEL_ACK };

    struct sctp_sendv_spa spa;
    memset(&spa, 0, sizeof(spa));
    spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
    spa.sendv_sndinfo.snd_sid = stream_id;
    spa.sendv_sndinfo.snd_ppid = htonl(SCTP_PPID_DCEP);

    struct socket *socket = sctp_native_borrow(peer);
    if (!socket) return -1;
    ssize_t sent = usrsctp_sendv(socket, packet, 1,
                                 NULL, 0, &spa, sizeof(spa),
                                 SCTP_SENDV_SPA, 0);
    sctp_native_return(peer);

    TLOG_INFOF("DCEP ACK send sid={} ret={} errno={}", stream_id, (int)sent, errno);

    return (sent > 0) ? 0 : -1;
}
