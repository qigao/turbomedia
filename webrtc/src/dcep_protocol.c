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
#include <turbo_str.h>
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

        if (len < (size_t)(DCEP_HEADER_LENGTH + label_len + proto_len)) {
            return;
        }

        /* Create incoming channel */
        turbo_dc_channel_t *channel = calloc(1, sizeof(*channel));
        if (!channel) return;

        channel->peer = peer;
        channel->id = stream_id;

        if (label_len > 0 && label_len <= MAX_LABEL_LEN) {
            channel->label = tstr_dup_len((const char *)data + DCEP_LABEL_OFFSET, label_len);
        }
        if (proto_len > 0 && proto_len <= MAX_PROTOCOL_LEN) {
            channel->protocol = tstr_dup_len((const char *)data + DCEP_LABEL_OFFSET + label_len, proto_len);
        }

        /* Parse channel type */
        uint8_t channel_type = data[1];
        channel->config.ordered = !(channel_type & DCEP_CHANNEL_RELIABLE_UNORDERED);

        /* Validate stream_id before storing */
        if (stream_id >= MAX_CHANNELS) {
            free(channel);
            return;
        }

        /* Check if channel already exists (duplicate OPEN) */
        if (peer->channels[stream_id] != NULL) {
            free(channel);
            return;
        }

        /* Mark this ID as used (remote-initiated channel) */
        peer_mark_channel_id(peer, stream_id);

        peer->channels[stream_id] = channel;
        TLOG_INFO("DCEP OPEN received sid={} label='{}' proto='{}'",
                  stream_id,
                  channel->label ? channel->label : "",
                  channel->protocol ? channel->protocol : "");

        /* Send ACK */
        dcep_send_ack(peer, stream_id);

        channel->is_open = 1;

        /* Notify application */
        if (peer->on_channel) {
            peer->on_channel(peer, channel, peer->user_data);
        }
        if (channel->on_open) {
            channel->on_open(channel, channel->user_data);
        }
    }
    else if (msg_type == DCEP_DATA_CHANNEL_ACK) {
        TLOG_INFO("DCEP ACK received sid={}", stream_id);
        turbo_dc_channel_t *channel = NULL;
        if (stream_id < MAX_CHANNELS) {
            channel = peer->channels[stream_id];
        }

        if (channel && !channel->is_open) {
            channel->is_open = 1;
            if (channel->on_open) {
                channel->on_open(channel, channel->user_data);
            }
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

    if (channel->config.max_retransmits > 0) {
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

    ssize_t sent = usrsctp_sendv(peer->sctp.socket, packet, packet_len,
                                  NULL, 0, &spa, sizeof(spa),
                                  SCTP_SENDV_SPA, 0);

    TLOG_INFO("DCEP OPEN send sid={} bytes={} ret={} errno={}",
              channel->id, packet_len, (int)sent, errno);

    return (sent > 0) ? 0 : -1;
}

int dcep_send_ack(turbo_dc_peer_t *peer, uint16_t stream_id) {
    if (!peer || !peer->sctp.socket) return -1;

    uint8_t packet[1] = { DCEP_DATA_CHANNEL_ACK };

    struct sctp_sendv_spa spa;
    memset(&spa, 0, sizeof(spa));
    spa.sendv_flags = SCTP_SEND_SNDINFO_VALID;
    spa.sendv_sndinfo.snd_sid = stream_id;
    spa.sendv_sndinfo.snd_ppid = htonl(SCTP_PPID_DCEP);

    ssize_t sent = usrsctp_sendv(peer->sctp.socket, packet, 1,
                                  NULL, 0, &spa, sizeof(spa),
                                  SCTP_SENDV_SPA, 0);

    TLOG_INFO("DCEP ACK send sid={} ret={} errno={}", stream_id, (int)sent, errno);

    return (sent > 0) ? 0 : -1;
}
