#include "turbo_rtsp_rtp.h"

#ifdef TURBO_MEDIA_HAS_RTSP

#include <salts/clock.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

#define TURBO_RTSP_RTP_SEQ_MOD 0x10000u
#define TURBO_RTSP_RTP_MAX_DROPOUT 3000u

static uint16_t turbo_rtsp_read_u16be(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t turbo_rtsp_read_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void turbo_rtsp_write_u16be(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xffu);
}

static void turbo_rtsp_write_u32be(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)((value >> 16) & 0xffu);
    p[2] = (uint8_t)((value >> 8) & 0xffu);
    p[3] = (uint8_t)(value & 0xffu);
}

static size_t turbo_rtsp_align4(size_t value) {
    return (value + 3u) & ~(size_t)3u;
}

static int turbo_rtsp_rtcp_sdes_cname_packet_len(
    const char *cname,
    size_t cname_len,
    size_t *packet_len) {
    size_t raw_len = 0;

    if (!cname || cname_len == 0 || cname_len > 255 || !packet_len) {
        return -1;
    }

    raw_len = TURBO_RTSP_RTCP_HEADER_SIZE + 4u + 2u + cname_len + 1u;
    *packet_len = turbo_rtsp_align4(raw_len);
    return 0;
}

int turbo_rtsp_rtp_parse_header(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtp_header_t *header,
    size_t *header_len) {
    size_t i = 0;
    size_t parsed_len = 0;
    size_t raw_payload_len = 0;

    if (!packet || !header || !header_len || packet_len < TURBO_RTSP_RTP_HEADER_SIZE) {
        return -1;
    }

    memset(header, 0, sizeof(*header));
    header->version = (uint8_t)(packet[0] >> 6);
    header->padding = (uint8_t)((packet[0] >> 5) & 0x01u);
    header->extension = (uint8_t)((packet[0] >> 4) & 0x01u);
    header->csrc_count = (uint8_t)(packet[0] & 0x0fu);
    header->marker = (uint8_t)(packet[1] >> 7);
    header->payload_type = (uint8_t)(packet[1] & 0x7fu);
    header->sequence_number = turbo_rtsp_read_u16be(packet + 2);
    header->timestamp = turbo_rtsp_read_u32be(packet + 4);
    header->ssrc = turbo_rtsp_read_u32be(packet + 8);

    if (header->version != 2) {
        return -1;
    }

    parsed_len = TURBO_RTSP_RTP_HEADER_SIZE + ((size_t)header->csrc_count * 4u);
    if (packet_len < parsed_len) {
        return -1;
    }
    for (i = 0; i < (size_t)header->csrc_count; ++i) {
        header->csrc[i] = turbo_rtsp_read_u32be(packet + TURBO_RTSP_RTP_HEADER_SIZE + (i * 4u));
    }

    if (header->extension) {
        size_t extension_len = 0;

        if (packet_len < parsed_len + 4u) {
            return -1;
        }
        header->extension_profile = turbo_rtsp_read_u16be(packet + parsed_len);
        extension_len = (size_t)turbo_rtsp_read_u16be(packet + parsed_len + 2u) * 4u;
        if (packet_len < parsed_len + 4u + extension_len) {
            return -1;
        }
        header->extension_len = extension_len;
        header->extension_data = packet + parsed_len + 4u;
        parsed_len += 4u + extension_len;
    }

    raw_payload_len = packet_len - parsed_len;
    if (header->padding) {
        if (raw_payload_len < 1u) {
            return -1;
        }
        header->padding_len = packet[packet_len - 1u];
        if (header->padding_len == 0 || raw_payload_len < (size_t)header->padding_len) {
            return -1;
        }
    }
    header->payload = packet + parsed_len;
    header->payload_len = raw_payload_len - (size_t)header->padding_len;

    *header_len = parsed_len;
    return 0;
}

int turbo_rtsp_rtp_write_header(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtp_header_t *header) {
    if (!packet || !header || packet_size < TURBO_RTSP_RTP_HEADER_SIZE) {
        return -1;
    }
    if (header->version != 2 || header->csrc_count > 15 || header->payload_type > 127) {
        return -1;
    }
    if (header->extension && ((header->extension_len % 4u) != 0 ||
                              (header->extension_len > 0 && !header->extension_data))) {
        return -1;
    }
    if (header->extension && header->extension_len > ((size_t)UINT16_MAX * 4u)) {
        return -1;
    }

    {
        size_t i = 0;
        size_t written_len = TURBO_RTSP_RTP_HEADER_SIZE + ((size_t)header->csrc_count * 4u);
        if (header->extension) {
            written_len += 4u + (size_t)header->extension_len;
        }
        if (packet_size < written_len) {
            return -1;
        }

        packet[0] = (uint8_t)((header->version << 6) |
                              ((header->padding & 0x01u) << 5) |
                              ((header->extension & 0x01u) << 4) |
                              (header->csrc_count & 0x0fu));
        packet[1] = (uint8_t)(((header->marker & 0x01u) << 7) |
                              (header->payload_type & 0x7fu));
        turbo_rtsp_write_u16be(packet + 2, header->sequence_number);
        turbo_rtsp_write_u32be(packet + 4, header->timestamp);
        turbo_rtsp_write_u32be(packet + 8, header->ssrc);
        for (i = 0; i < (size_t)header->csrc_count; ++i) {
            turbo_rtsp_write_u32be(packet + TURBO_RTSP_RTP_HEADER_SIZE + (i * 4u), header->csrc[i]);
        }
        if (header->extension) {
            uint8_t *extension = packet + TURBO_RTSP_RTP_HEADER_SIZE + ((size_t)header->csrc_count * 4u);

            turbo_rtsp_write_u16be(extension, header->extension_profile);
            turbo_rtsp_write_u16be(extension + 2, (uint16_t)(header->extension_len / 4u));
            if (header->extension_len > 0) {
                memcpy(extension + 4, header->extension_data, header->extension_len);
            }
        }
        return (int)written_len;
    }
}

int turbo_rtsp_rtp_write_packet(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtp_header_t *header,
    const uint8_t *payload,
    size_t payload_len) {
    int header_len = 0;

    if (!packet || !header || (!payload && payload_len > 0) || header->padding) {
        return -1;
    }

    header_len = turbo_rtsp_rtp_write_header(packet, packet_size, header);
    if (header_len < 0) {
        return -1;
    }
    if (payload_len > packet_size - (size_t)header_len) {
        return -1;
    }
    if (payload_len > 0) {
        memcpy(packet + header_len, payload, payload_len);
    }
    return header_len + (int)payload_len;
}

int turbo_rtsp_mpeg4_generic_payload_parse(
    const uint8_t *payload,
    size_t payload_len,
    turbo_rtsp_mpeg4_generic_payload_t *mpeg4) {
    uint16_t au_header_bits = 0;
    size_t au_size = 0;
    size_t au_fragment_len = 0;

    if (!payload || payload_len < TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE || !mpeg4) {
        return -1;
    }

    memset(mpeg4, 0, sizeof(*mpeg4));
    au_header_bits = turbo_rtsp_read_u16be(payload);
    if (au_header_bits != TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_AU_HEADER_BITS) {
        return -1;
    }

    au_size = ((size_t)payload[2] << TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_INDEX_LENGTH) |
              (size_t)(payload[3] >> TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_INDEX_LENGTH);
    au_fragment_len = payload_len - TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE;
    if (au_size == 0 ||
        au_size > TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_MAX_AU_SIZE ||
        au_fragment_len == 0 ||
        au_fragment_len > au_size) {
        return -1;
    }

    mpeg4->au_header_bits = au_header_bits;
    mpeg4->au_size = au_size;
    mpeg4->au_index = (uint8_t)(payload[3] & 0x07u);
    mpeg4->au_fragment = payload + TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE;
    mpeg4->au_fragment_len = au_fragment_len;
    mpeg4->complete = au_fragment_len == au_size ? 1 : 0;
    return 0;
}

static int turbo_rtsp_mp4a_latm_length_info_size(
    size_t latm_len,
    size_t *length_info_len) {
    size_t len = 0;

    if (latm_len == 0 || !length_info_len) {
        return -1;
    }

    len = latm_len / 255u + 1u;
    if (len > TURBO_RTSP_MP4A_LATM_MAX_LENGTH_INFO_SIZE) {
        return -1;
    }

    *length_info_len = len;
    return 0;
}

static void turbo_rtsp_mp4a_latm_write_length_info(
    uint8_t *payload,
    size_t latm_len,
    size_t length_info_len) {
    size_t i = 0;

    for (i = 0; i + 1u < length_info_len; ++i) {
        payload[i] = 255;
    }
    payload[length_info_len - 1u] = (uint8_t)(latm_len % 255u);
}

int turbo_rtsp_mp4a_latm_payload_parse(
    const uint8_t *payload,
    size_t payload_len,
    turbo_rtsp_mp4a_latm_payload_t *latm) {
    size_t offset = 0;
    size_t payload_length = 0;
    int terminated = 0;

    if (!payload || payload_len == 0 || !latm) {
        return -1;
    }

    while (offset < payload_len &&
           offset < TURBO_RTSP_MP4A_LATM_MAX_LENGTH_INFO_SIZE) {
        payload_length += payload[offset];
        ++offset;
        if (payload[offset - 1u] != 255) {
            terminated = 1;
            break;
        }
    }
    if (offset == 0 ||
        !terminated ||
        offset > TURBO_RTSP_MP4A_LATM_MAX_LENGTH_INFO_SIZE ||
        payload_length == 0 ||
        payload_len - offset == 0 ||
        payload_len - offset > payload_length) {
        return -1;
    }

    memset(latm, 0, sizeof(*latm));
    latm->payload_length = payload_length;
    latm->length_info_len = offset;
    latm->fragment = payload + offset;
    latm->fragment_len = payload_len - offset;
    latm->complete = latm->fragment_len == payload_length ? 1 : 0;
    return 0;
}

int turbo_rtsp_mpeg2_ts_payload_parse(
    const uint8_t *payload,
    size_t payload_len,
    turbo_rtsp_mpeg2_ts_payload_t *ts) {
    size_t offset = 0;

    if (!payload || payload_len == 0 || !ts ||
        payload_len % TURBO_RTSP_MPEG2_TS_PACKET_SIZE != 0) {
        return -1;
    }

    for (offset = 0; offset < payload_len; offset += TURBO_RTSP_MPEG2_TS_PACKET_SIZE) {
        if (payload[offset] != TURBO_RTSP_MPEG2_TS_SYNC_BYTE) {
            return -1;
        }
    }

    memset(ts, 0, sizeof(*ts));
    ts->packets = payload;
    ts->packets_len = payload_len;
    ts->packet_count = payload_len / TURBO_RTSP_MPEG2_TS_PACKET_SIZE;
    return 0;
}

static int turbo_rtsp_h264_is_single_nal_type(uint8_t nal_unit_type) {
    return nal_unit_type >= 1u && nal_unit_type <= 23u;
}

static uint8_t turbo_rtsp_h264_nal_type(uint8_t nal_header) {
    return (uint8_t)(nal_header & TURBO_RTSP_H264_NAL_TYPE_MASK);
}

static uint8_t turbo_rtsp_h264_nal_ref_idc(uint8_t nal_header) {
    return (uint8_t)((nal_header & TURBO_RTSP_H264_NAL_NRI_MASK) >> 5);
}

int turbo_rtsp_h264_payload_parse(
    const uint8_t *payload,
    size_t payload_len,
    turbo_rtsp_h264_payload_t *h264) {
    uint8_t nal_header = 0;
    uint8_t nal_unit_type = 0;

    if (!payload || payload_len == 0 || !h264) {
        return -1;
    }

    memset(h264, 0, sizeof(*h264));
    nal_header = payload[0];
    nal_unit_type = turbo_rtsp_h264_nal_type(nal_header);

    h264->nal_header = nal_header;
    h264->forbidden_zero_bit = (uint8_t)(nal_header >> 7);
    h264->nal_ref_idc = turbo_rtsp_h264_nal_ref_idc(nal_header);
    h264->nal_unit_type = nal_unit_type;
    h264->payload = payload;
    h264->payload_len = payload_len;

    if (h264->forbidden_zero_bit != 0) {
        return -1;
    }

    if (turbo_rtsp_h264_is_single_nal_type(nal_unit_type)) {
        h264->kind = TURBO_RTSP_H264_PAYLOAD_SINGLE_NAL;
        h264->nal = payload;
        h264->nal_len = payload_len;
        h264->reconstructed_nal_header = nal_header;
        return 0;
    }

    if (nal_unit_type == TURBO_RTSP_H264_NAL_TYPE_STAP_A) {
        if (payload_len < 2u) {
            return -1;
        }
        h264->kind = TURBO_RTSP_H264_PAYLOAD_STAP_A;
        return 0;
    }

    if (nal_unit_type == TURBO_RTSP_H264_NAL_TYPE_FU_A) {
        uint8_t fu_header = 0;
        uint8_t fu_nal_unit_type = 0;

        if (payload_len < 2u) {
            return -1;
        }

        fu_header = payload[1];
        fu_nal_unit_type = turbo_rtsp_h264_nal_type(fu_header);
        h264->fu_start = (uint8_t)((fu_header >> 7) & 0x01u);
        h264->fu_end = (uint8_t)((fu_header >> 6) & 0x01u);
        h264->fu_reserved = (uint8_t)((fu_header >> 5) & 0x01u);
        h264->fu_nal_unit_type = fu_nal_unit_type;
        h264->reconstructed_nal_header =
            (uint8_t)((nal_header & (uint8_t)0xe0u) | fu_nal_unit_type);
        h264->fu_payload = payload + 2u;
        h264->fu_payload_len = payload_len - 2u;

        if (h264->fu_reserved || (h264->fu_start && h264->fu_end) ||
            !turbo_rtsp_h264_is_single_nal_type(fu_nal_unit_type)) {
            return -1;
        }

        h264->kind = TURBO_RTSP_H264_PAYLOAD_FU_A;
        return 0;
    }

    return -1;
}

int turbo_rtsp_h264_stap_a_next(
    const turbo_rtsp_h264_payload_t *h264,
    size_t *offset,
    const uint8_t **nal,
    size_t *nal_len) {
    size_t cursor = 0;
    uint16_t current_nal_len = 0;
    uint8_t current_nal_type = 0;

    if (!h264 || !offset || !nal || !nal_len ||
        h264->kind != TURBO_RTSP_H264_PAYLOAD_STAP_A ||
        !h264->payload || h264->payload_len < 2u) {
        return -1;
    }

    cursor = *offset == 0 ? 1u : *offset;
    if (cursor == h264->payload_len) {
        *offset = cursor;
        *nal = NULL;
        *nal_len = 0;
        return 1;
    }
    if (cursor < 1u || h264->payload_len - cursor < 2u) {
        return -1;
    }

    current_nal_len = turbo_rtsp_read_u16be(h264->payload + cursor);
    cursor += 2u;
    if (current_nal_len == 0 ||
        current_nal_len > h264->payload_len - cursor) {
        return -1;
    }

    current_nal_type = turbo_rtsp_h264_nal_type(h264->payload[cursor]);
    if ((h264->payload[cursor] >> 7) != 0 ||
        !turbo_rtsp_h264_is_single_nal_type(current_nal_type)) {
        return -1;
    }

    *nal = h264->payload + cursor;
    *nal_len = current_nal_len;
    *offset = cursor + current_nal_len;
    return 0;
}

int turbo_rtsp_h264_payload_write_nal_fragment(
    uint8_t *buffer,
    size_t buffer_size,
    const turbo_rtsp_h264_payload_t *h264,
    size_t *written) {
    size_t needed = 0;

    if (!buffer || !h264 || !written) {
        return -1;
    }

    *written = 0;
    if (h264->kind == TURBO_RTSP_H264_PAYLOAD_SINGLE_NAL) {
        if (!h264->nal || h264->nal_len > buffer_size) {
            return -1;
        }
        if (h264->nal_len > 0) {
            memcpy(buffer, h264->nal, h264->nal_len);
        }
        *written = h264->nal_len;
        return 0;
    }

    if (h264->kind != TURBO_RTSP_H264_PAYLOAD_FU_A || !h264->fu_payload) {
        return -1;
    }

    needed = h264->fu_payload_len + (h264->fu_start ? 1u : 0u);
    if (needed > buffer_size) {
        return -1;
    }

    if (h264->fu_start) {
        buffer[0] = h264->reconstructed_nal_header;
        if (h264->fu_payload_len > 0) {
            memcpy(buffer + 1u, h264->fu_payload, h264->fu_payload_len);
        }
    } else if (h264->fu_payload_len > 0) {
        memcpy(buffer, h264->fu_payload, h264->fu_payload_len);
    }
    *written = needed;
    return 0;
}

int turbo_rtsp_h264_packetizer_init(
    turbo_rtsp_h264_packetizer_t *packetizer,
    const uint8_t *nal,
    size_t nal_len,
    size_t max_payload) {
    uint8_t nal_unit_type = 0;

    if (!packetizer || !nal || nal_len == 0 || max_payload == 0) {
        return -1;
    }

    nal_unit_type = turbo_rtsp_h264_nal_type(nal[0]);
    if ((nal[0] >> 7) != 0 ||
        !turbo_rtsp_h264_is_single_nal_type(nal_unit_type)) {
        return -1;
    }

    if (nal_len > max_payload && max_payload < 3u) {
        return -1;
    }

    memset(packetizer, 0, sizeof(*packetizer));
    packetizer->nal = nal;
    packetizer->nal_len = nal_len;
    packetizer->max_payload = max_payload;
    packetizer->single_nal = nal_len <= max_payload ? 1u : 0u;
    packetizer->offset = packetizer->single_nal ? 0u : 1u;
    return 0;
}

int turbo_rtsp_h264_packetizer_next(
    turbo_rtsp_h264_packetizer_t *packetizer,
    uint8_t *payload,
    size_t payload_size,
    turbo_rtsp_h264_packetized_payload_t *out) {
    size_t max_fragment_data = 0;
    size_t remaining = 0;
    size_t fragment_len = 0;
    uint8_t nal_header = 0;
    uint8_t fu_header = 0;

    if (!packetizer || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    if (packetizer->done) {
        return 1;
    }

    if (!packetizer->nal || packetizer->nal_len == 0 ||
        packetizer->max_payload == 0) {
        return -1;
    }

    if (packetizer->single_nal) {
        out->payload = packetizer->nal;
        out->payload_len = packetizer->nal_len;
        out->marker = 1;
        out->end = 1;
        packetizer->done = 1;
        return 0;
    }

    if (!payload || payload_size < 3u || packetizer->max_payload < 3u ||
        packetizer->offset == 0 || packetizer->offset >= packetizer->nal_len) {
        return -1;
    }

    max_fragment_data = packetizer->max_payload - 2u;
    remaining = packetizer->nal_len - packetizer->offset;
    fragment_len = remaining < max_fragment_data ? remaining : max_fragment_data;
    if (payload_size < fragment_len + 2u) {
        return -1;
    }

    nal_header = packetizer->nal[0];
    payload[0] = (uint8_t)((nal_header & (uint8_t)0xe0u) |
                           TURBO_RTSP_H264_NAL_TYPE_FU_A);
    fu_header = turbo_rtsp_h264_nal_type(nal_header);
    if (packetizer->offset == 1u) {
        fu_header = (uint8_t)(fu_header | 0x80u);
    }
    if (fragment_len == remaining) {
        fu_header = (uint8_t)(fu_header | 0x40u);
        out->marker = 1;
        out->end = 1;
        packetizer->done = 1;
    }
    payload[1] = fu_header;
    memcpy(payload + 2u, packetizer->nal + packetizer->offset, fragment_len);

    packetizer->offset += fragment_len;
    out->payload = payload;
    out->payload_len = fragment_len + 2u;
    return 0;
}

void turbo_rtsp_h264_reassembler_init(
    turbo_rtsp_h264_reassembler_t *reassembler) {
    if (!reassembler) {
        return;
    }
    memset(reassembler, 0, sizeof(*reassembler));
}

void turbo_rtsp_h264_reassembler_reset(
    turbo_rtsp_h264_reassembler_t *reassembler) {
    turbo_rtsp_h264_reassembler_init(reassembler);
}

static int turbo_rtsp_h264_reassembler_append(
    turbo_rtsp_h264_reassembler_t *reassembler,
    uint8_t *buffer,
    size_t buffer_size,
    const uint8_t *payload,
    size_t payload_len) {
    if (!reassembler || !buffer || (!payload && payload_len > 0) ||
        reassembler->nal_len > buffer_size ||
        payload_len > buffer_size - reassembler->nal_len) {
        return -1;
    }
    if (payload_len > 0) {
        memcpy(buffer + reassembler->nal_len, payload, payload_len);
    }
    reassembler->nal_len += payload_len;
    return 0;
}

int turbo_rtsp_h264_reassembler_push(
    turbo_rtsp_h264_reassembler_t *reassembler,
    const turbo_rtsp_rtp_header_t *header,
    uint8_t *buffer,
    size_t buffer_size,
    const uint8_t **nal,
    size_t *nal_len) {
    turbo_rtsp_h264_payload_t h264;

    if (!reassembler || !header || !nal || !nal_len) {
        return TURBO_RTSP_FRAME_ERROR;
    }

    *nal = NULL;
    *nal_len = 0;

    if (turbo_rtsp_h264_payload_parse(header->payload, header->payload_len, &h264) != 0) {
        turbo_rtsp_h264_reassembler_reset(reassembler);
        return TURBO_RTSP_FRAME_ERROR;
    }

    if (h264.kind == TURBO_RTSP_H264_PAYLOAD_SINGLE_NAL) {
        if (reassembler->started) {
            turbo_rtsp_h264_reassembler_reset(reassembler);
            return TURBO_RTSP_FRAME_ERROR;
        }
        *nal = h264.nal;
        *nal_len = h264.nal_len;
        return TURBO_RTSP_FRAME_OK;
    }

    if (h264.kind != TURBO_RTSP_H264_PAYLOAD_FU_A) {
        if (reassembler->started) {
            turbo_rtsp_h264_reassembler_reset(reassembler);
        }
        return TURBO_RTSP_FRAME_ERROR;
    }

    if (h264.fu_start) {
        if (reassembler->started || !buffer || buffer_size < 1u) {
            turbo_rtsp_h264_reassembler_reset(reassembler);
            return TURBO_RTSP_FRAME_ERROR;
        }
        reassembler->ssrc = header->ssrc;
        reassembler->timestamp = header->timestamp;
        reassembler->next_sequence_number = (uint16_t)(header->sequence_number + 1u);
        reassembler->fu_nal_unit_type = h264.fu_nal_unit_type;
        reassembler->reconstructed_nal_header = h264.reconstructed_nal_header;
        reassembler->nal_len = 0;
        reassembler->started = 1;
        buffer[0] = h264.reconstructed_nal_header;
        reassembler->nal_len = 1u;
        if (turbo_rtsp_h264_reassembler_append(
                reassembler,
                buffer,
                buffer_size,
                h264.fu_payload,
                h264.fu_payload_len) != 0) {
            turbo_rtsp_h264_reassembler_reset(reassembler);
            return TURBO_RTSP_FRAME_ERROR;
        }
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    if (!reassembler->started ||
        header->ssrc != reassembler->ssrc ||
        header->timestamp != reassembler->timestamp ||
        header->sequence_number != reassembler->next_sequence_number ||
        h264.fu_nal_unit_type != reassembler->fu_nal_unit_type ||
        h264.reconstructed_nal_header != reassembler->reconstructed_nal_header) {
        turbo_rtsp_h264_reassembler_reset(reassembler);
        return TURBO_RTSP_FRAME_ERROR;
    }

    if (turbo_rtsp_h264_reassembler_append(
            reassembler,
            buffer,
            buffer_size,
            h264.fu_payload,
            h264.fu_payload_len) != 0) {
        turbo_rtsp_h264_reassembler_reset(reassembler);
        return TURBO_RTSP_FRAME_ERROR;
    }

    reassembler->next_sequence_number = (uint16_t)(header->sequence_number + 1u);
    if (!h264.fu_end) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    *nal = buffer;
    *nal_len = reassembler->nal_len;
    turbo_rtsp_h264_reassembler_reset(reassembler);
    return TURBO_RTSP_FRAME_OK;
}

static uint8_t turbo_rtsp_h265_nal_type(const uint8_t *nal_header) {
    return (uint8_t)((nal_header[0] >> 1) & 0x3fu);
}

static uint8_t turbo_rtsp_h265_nuh_layer_id(const uint8_t *nal_header) {
    return (uint8_t)(((nal_header[0] & 0x01u) << 5) | (nal_header[1] >> 3));
}

static int turbo_rtsp_h265_is_single_nal_type(uint8_t nal_unit_type) {
    return nal_unit_type <= 47u;
}

static int turbo_rtsp_h265_header_valid(const uint8_t *nal_header) {
    return nal_header &&
           (nal_header[0] >> 7) == 0 &&
           (nal_header[1] & 0x07u) != 0;
}

static void turbo_rtsp_h265_reconstruct_nal_header(
    const uint8_t *payload_header,
    uint8_t fu_nal_unit_type,
    uint8_t reconstructed[2]) {
    reconstructed[0] = (uint8_t)((payload_header[0] & 0x81u) |
                                 (uint8_t)(fu_nal_unit_type << 1));
    reconstructed[1] = payload_header[1];
}

int turbo_rtsp_h265_payload_parse(
    const uint8_t *payload,
    size_t payload_len,
    turbo_rtsp_h265_payload_t *h265) {
    uint8_t nal_unit_type = 0;

    if (!payload || payload_len < TURBO_RTSP_H265_NAL_HEADER_SIZE || !h265) {
        return -1;
    }

    memset(h265, 0, sizeof(*h265));
    h265->nal_header[0] = payload[0];
    h265->nal_header[1] = payload[1];
    h265->forbidden_zero_bit = (uint8_t)(payload[0] >> 7);
    h265->nal_unit_type = turbo_rtsp_h265_nal_type(payload);
    h265->nuh_layer_id = turbo_rtsp_h265_nuh_layer_id(payload);
    h265->nuh_temporal_id_plus1 = (uint8_t)(payload[1] & 0x07u);
    h265->payload = payload;
    h265->payload_len = payload_len;

    if (!turbo_rtsp_h265_header_valid(payload)) {
        return -1;
    }

    nal_unit_type = h265->nal_unit_type;
    if (turbo_rtsp_h265_is_single_nal_type(nal_unit_type)) {
        h265->kind = TURBO_RTSP_H265_PAYLOAD_SINGLE_NAL;
        h265->nal = payload;
        h265->nal_len = payload_len;
        h265->reconstructed_nal_header[0] = payload[0];
        h265->reconstructed_nal_header[1] = payload[1];
        return 0;
    }

    if (nal_unit_type == TURBO_RTSP_H265_NAL_TYPE_AP) {
        if (payload_len < TURBO_RTSP_H265_NAL_HEADER_SIZE + 2u) {
            return -1;
        }
        h265->kind = TURBO_RTSP_H265_PAYLOAD_AP;
        return 0;
    }

    if (nal_unit_type == TURBO_RTSP_H265_NAL_TYPE_FU) {
        uint8_t fu_header = 0;
        uint8_t fu_nal_unit_type = 0;

        if (payload_len < TURBO_RTSP_H265_NAL_HEADER_SIZE + 2u) {
            return -1;
        }

        fu_header = payload[2];
        fu_nal_unit_type = (uint8_t)(fu_header & 0x3fu);
        h265->fu_start = (uint8_t)((fu_header >> 7) & 0x01u);
        h265->fu_end = (uint8_t)((fu_header >> 6) & 0x01u);
        h265->fu_nal_unit_type = fu_nal_unit_type;
        turbo_rtsp_h265_reconstruct_nal_header(
            payload,
            fu_nal_unit_type,
            h265->reconstructed_nal_header);
        h265->fu_payload = payload + 3u;
        h265->fu_payload_len = payload_len - 3u;

        if ((h265->fu_start && h265->fu_end) ||
            !turbo_rtsp_h265_is_single_nal_type(fu_nal_unit_type) ||
            h265->fu_payload_len == 0) {
            return -1;
        }

        h265->kind = TURBO_RTSP_H265_PAYLOAD_FU;
        return 0;
    }

    return -1;
}

int turbo_rtsp_h265_ap_next(
    const turbo_rtsp_h265_payload_t *h265,
    size_t *offset,
    const uint8_t **nal,
    size_t *nal_len) {
    size_t cursor = 0;
    uint16_t current_nal_len = 0;

    if (!h265 || !offset || !nal || !nal_len ||
        h265->kind != TURBO_RTSP_H265_PAYLOAD_AP ||
        !h265->payload ||
        h265->payload_len < TURBO_RTSP_H265_NAL_HEADER_SIZE + 2u) {
        return -1;
    }

    cursor = *offset == 0 ? TURBO_RTSP_H265_NAL_HEADER_SIZE : *offset;
    if (cursor == h265->payload_len) {
        *offset = cursor;
        *nal = NULL;
        *nal_len = 0;
        return 1;
    }
    if (cursor < TURBO_RTSP_H265_NAL_HEADER_SIZE ||
        h265->payload_len - cursor < 2u) {
        return -1;
    }

    current_nal_len = turbo_rtsp_read_u16be(h265->payload + cursor);
    cursor += 2u;
    if (current_nal_len < TURBO_RTSP_H265_NAL_HEADER_SIZE ||
        current_nal_len > h265->payload_len - cursor ||
        !turbo_rtsp_h265_header_valid(h265->payload + cursor) ||
        !turbo_rtsp_h265_is_single_nal_type(
            turbo_rtsp_h265_nal_type(h265->payload + cursor))) {
        return -1;
    }

    *nal = h265->payload + cursor;
    *nal_len = current_nal_len;
    *offset = cursor + current_nal_len;
    return 0;
}

int turbo_rtsp_h265_payload_write_nal_fragment(
    uint8_t *buffer,
    size_t buffer_size,
    const turbo_rtsp_h265_payload_t *h265,
    size_t *written) {
    size_t needed = 0;

    if (!buffer || !h265 || !written) {
        return -1;
    }

    *written = 0;
    if (h265->kind == TURBO_RTSP_H265_PAYLOAD_SINGLE_NAL) {
        if (!h265->nal || h265->nal_len > buffer_size) {
            return -1;
        }
        if (h265->nal_len > 0) {
            memcpy(buffer, h265->nal, h265->nal_len);
        }
        *written = h265->nal_len;
        return 0;
    }

    if (h265->kind != TURBO_RTSP_H265_PAYLOAD_FU || !h265->fu_payload) {
        return -1;
    }

    needed = h265->fu_payload_len +
             (h265->fu_start ? TURBO_RTSP_H265_NAL_HEADER_SIZE : 0u);
    if (needed > buffer_size) {
        return -1;
    }

    if (h265->fu_start) {
        buffer[0] = h265->reconstructed_nal_header[0];
        buffer[1] = h265->reconstructed_nal_header[1];
        if (h265->fu_payload_len > 0) {
            memcpy(
                buffer + TURBO_RTSP_H265_NAL_HEADER_SIZE,
                h265->fu_payload,
                h265->fu_payload_len);
        }
    } else if (h265->fu_payload_len > 0) {
        memcpy(buffer, h265->fu_payload, h265->fu_payload_len);
    }
    *written = needed;
    return 0;
}

int turbo_rtsp_h265_packetizer_init(
    turbo_rtsp_h265_packetizer_t *packetizer,
    const uint8_t *nal,
    size_t nal_len,
    size_t max_payload) {
    uint8_t nal_unit_type = 0;

    if (!packetizer || !nal ||
        nal_len < TURBO_RTSP_H265_NAL_HEADER_SIZE ||
        max_payload == 0) {
        return -1;
    }

    nal_unit_type = turbo_rtsp_h265_nal_type(nal);
    if (!turbo_rtsp_h265_header_valid(nal) ||
        !turbo_rtsp_h265_is_single_nal_type(nal_unit_type)) {
        return -1;
    }

    if (nal_len > max_payload && max_payload < 4u) {
        return -1;
    }

    memset(packetizer, 0, sizeof(*packetizer));
    packetizer->nal = nal;
    packetizer->nal_len = nal_len;
    packetizer->max_payload = max_payload;
    packetizer->single_nal = nal_len <= max_payload ? 1u : 0u;
    packetizer->offset = packetizer->single_nal ?
        0u : TURBO_RTSP_H265_NAL_HEADER_SIZE;
    return 0;
}

int turbo_rtsp_h265_packetizer_next(
    turbo_rtsp_h265_packetizer_t *packetizer,
    uint8_t *payload,
    size_t payload_size,
    turbo_rtsp_h265_packetized_payload_t *out) {
    size_t max_fragment_data = 0;
    size_t remaining = 0;
    size_t fragment_len = 0;
    uint8_t fu_header = 0;

    if (!packetizer || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    if (packetizer->done) {
        return 1;
    }

    if (!packetizer->nal ||
        packetizer->nal_len < TURBO_RTSP_H265_NAL_HEADER_SIZE ||
        packetizer->max_payload == 0) {
        return -1;
    }

    if (packetizer->single_nal) {
        out->payload = packetizer->nal;
        out->payload_len = packetizer->nal_len;
        out->marker = 1;
        out->end = 1;
        packetizer->done = 1;
        return 0;
    }

    if (!payload || payload_size < 4u || packetizer->max_payload < 4u ||
        packetizer->offset < TURBO_RTSP_H265_NAL_HEADER_SIZE ||
        packetizer->offset >= packetizer->nal_len) {
        return -1;
    }

    max_fragment_data = packetizer->max_payload - 3u;
    remaining = packetizer->nal_len - packetizer->offset;
    fragment_len = remaining < max_fragment_data ? remaining : max_fragment_data;
    if (payload_size < fragment_len + 3u) {
        return -1;
    }

    payload[0] = (uint8_t)((packetizer->nal[0] & 0x81u) |
                           (uint8_t)(TURBO_RTSP_H265_NAL_TYPE_FU << 1));
    payload[1] = packetizer->nal[1];
    fu_header = turbo_rtsp_h265_nal_type(packetizer->nal);
    if (packetizer->offset == TURBO_RTSP_H265_NAL_HEADER_SIZE) {
        fu_header = (uint8_t)(fu_header | 0x80u);
    }
    if (fragment_len == remaining) {
        fu_header = (uint8_t)(fu_header | 0x40u);
        out->marker = 1;
        out->end = 1;
        packetizer->done = 1;
    }
    payload[2] = fu_header;
    memcpy(payload + 3u, packetizer->nal + packetizer->offset, fragment_len);

    packetizer->offset += fragment_len;
    out->payload = payload;
    out->payload_len = fragment_len + 3u;
    return 0;
}

void turbo_rtsp_h265_reassembler_init(
    turbo_rtsp_h265_reassembler_t *reassembler) {
    if (!reassembler) {
        return;
    }
    memset(reassembler, 0, sizeof(*reassembler));
}

void turbo_rtsp_h265_reassembler_reset(
    turbo_rtsp_h265_reassembler_t *reassembler) {
    turbo_rtsp_h265_reassembler_init(reassembler);
}

static int turbo_rtsp_h265_reassembler_append(
    turbo_rtsp_h265_reassembler_t *reassembler,
    uint8_t *buffer,
    size_t buffer_size,
    const uint8_t *payload,
    size_t payload_len) {
    if (!reassembler || !buffer || (!payload && payload_len > 0) ||
        reassembler->nal_len > buffer_size ||
        payload_len > buffer_size - reassembler->nal_len) {
        return -1;
    }
    if (payload_len > 0) {
        memcpy(buffer + reassembler->nal_len, payload, payload_len);
    }
    reassembler->nal_len += payload_len;
    return 0;
}

int turbo_rtsp_h265_reassembler_push(
    turbo_rtsp_h265_reassembler_t *reassembler,
    const turbo_rtsp_rtp_header_t *header,
    uint8_t *buffer,
    size_t buffer_size,
    const uint8_t **nal,
    size_t *nal_len) {
    turbo_rtsp_h265_payload_t h265;

    if (!reassembler || !header || !nal || !nal_len) {
        return TURBO_RTSP_FRAME_ERROR;
    }

    *nal = NULL;
    *nal_len = 0;

    if (turbo_rtsp_h265_payload_parse(header->payload, header->payload_len, &h265) != 0) {
        turbo_rtsp_h265_reassembler_reset(reassembler);
        return TURBO_RTSP_FRAME_ERROR;
    }

    if (h265.kind == TURBO_RTSP_H265_PAYLOAD_SINGLE_NAL) {
        if (reassembler->started) {
            turbo_rtsp_h265_reassembler_reset(reassembler);
            return TURBO_RTSP_FRAME_ERROR;
        }
        *nal = h265.nal;
        *nal_len = h265.nal_len;
        return TURBO_RTSP_FRAME_OK;
    }

    if (h265.kind != TURBO_RTSP_H265_PAYLOAD_FU) {
        if (reassembler->started) {
            turbo_rtsp_h265_reassembler_reset(reassembler);
        }
        return TURBO_RTSP_FRAME_ERROR;
    }

    if (h265.fu_start) {
        if (reassembler->started ||
            !buffer ||
            buffer_size < TURBO_RTSP_H265_NAL_HEADER_SIZE) {
            turbo_rtsp_h265_reassembler_reset(reassembler);
            return TURBO_RTSP_FRAME_ERROR;
        }
        reassembler->ssrc = header->ssrc;
        reassembler->timestamp = header->timestamp;
        reassembler->next_sequence_number = (uint16_t)(header->sequence_number + 1u);
        reassembler->fu_nal_unit_type = h265.fu_nal_unit_type;
        reassembler->reconstructed_nal_header[0] = h265.reconstructed_nal_header[0];
        reassembler->reconstructed_nal_header[1] = h265.reconstructed_nal_header[1];
        reassembler->nal_len = 0;
        reassembler->started = 1;
        buffer[0] = h265.reconstructed_nal_header[0];
        buffer[1] = h265.reconstructed_nal_header[1];
        reassembler->nal_len = TURBO_RTSP_H265_NAL_HEADER_SIZE;
        if (turbo_rtsp_h265_reassembler_append(
                reassembler,
                buffer,
                buffer_size,
                h265.fu_payload,
                h265.fu_payload_len) != 0) {
            turbo_rtsp_h265_reassembler_reset(reassembler);
            return TURBO_RTSP_FRAME_ERROR;
        }
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    if (!reassembler->started ||
        header->ssrc != reassembler->ssrc ||
        header->timestamp != reassembler->timestamp ||
        header->sequence_number != reassembler->next_sequence_number ||
        h265.fu_nal_unit_type != reassembler->fu_nal_unit_type ||
        h265.reconstructed_nal_header[0] != reassembler->reconstructed_nal_header[0] ||
        h265.reconstructed_nal_header[1] != reassembler->reconstructed_nal_header[1]) {
        turbo_rtsp_h265_reassembler_reset(reassembler);
        return TURBO_RTSP_FRAME_ERROR;
    }

    if (turbo_rtsp_h265_reassembler_append(
            reassembler,
            buffer,
            buffer_size,
            h265.fu_payload,
            h265.fu_payload_len) != 0) {
        turbo_rtsp_h265_reassembler_reset(reassembler);
        return TURBO_RTSP_FRAME_ERROR;
    }

    reassembler->next_sequence_number = (uint16_t)(header->sequence_number + 1u);
    if (!h265.fu_end) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    *nal = buffer;
    *nal_len = reassembler->nal_len;
    turbo_rtsp_h265_reassembler_reset(reassembler);
    return TURBO_RTSP_FRAME_OK;
}

int turbo_rtsp_rtcp_parse_header(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtcp_header_t *header) {
    uint16_t length = 0;
    size_t declared_len = 0;

    if (!packet || !header || packet_len < TURBO_RTSP_RTCP_HEADER_SIZE) {
        return -1;
    }

    memset(header, 0, sizeof(*header));
    header->version = (uint8_t)(packet[0] >> 6);
    header->padding = (uint8_t)((packet[0] >> 5) & 0x01u);
    header->count = (uint8_t)(packet[0] & 0x1fu);
    header->packet_type = packet[1];
    length = turbo_rtsp_read_u16be(packet + 2);
    declared_len = ((size_t)length + 1u) * 4u;

    if (header->version != 2 || declared_len < TURBO_RTSP_RTCP_HEADER_SIZE ||
        packet_len < declared_len) {
        return -1;
    }
    if (header->padding) {
        header->padding_len = packet[declared_len - 1u];
        if (header->padding_len == 0 ||
            (size_t)header->padding_len > declared_len - TURBO_RTSP_RTCP_HEADER_SIZE) {
            return -1;
        }
    }

    header->length = length;
    header->packet_len = declared_len;
    header->payload_len = declared_len - TURBO_RTSP_RTCP_HEADER_SIZE -
                          (size_t)header->padding_len;
    return 0;
}

int turbo_rtsp_rtcp_write_header(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtcp_header_t *header) {
    if (!packet || !header || packet_size < TURBO_RTSP_RTCP_HEADER_SIZE) {
        return -1;
    }
    if (header->version != 2 || header->padding > 1 || header->count > 31) {
        return -1;
    }

    packet[0] = (uint8_t)((header->version << 6) |
                          ((header->padding & 0x01u) << 5) |
                          (header->count & 0x1fu));
    packet[1] = header->packet_type;
    turbo_rtsp_write_u16be(packet + 2, header->length);
    return TURBO_RTSP_RTCP_HEADER_SIZE;
}

int turbo_rtsp_rtcp_next_packet(
    const uint8_t *buffer,
    size_t buffer_len,
    turbo_rtsp_rtcp_header_t *header,
    size_t *consumed) {
    if (!buffer || !header || !consumed) {
        return -1;
    }
    *consumed = 0;
    if (buffer_len == 0) {
        return 1;
    }
    if (buffer_len < TURBO_RTSP_RTCP_HEADER_SIZE) {
        return 1;
    }
    if (turbo_rtsp_rtcp_parse_header(buffer, buffer_len, header) != 0) {
        uint16_t length = turbo_rtsp_read_u16be(buffer + 2);
        size_t declared_len = ((size_t)length + 1u) * 4u;

        if (((uint8_t)(buffer[0] >> 6)) == 2 &&
            declared_len >= TURBO_RTSP_RTCP_HEADER_SIZE &&
            buffer_len < declared_len) {
            return 1;
        }
        return -1;
    }

    *consumed = header->packet_len;
    return 0;
}

int turbo_rtsp_rtcp_validate_compound(
    const uint8_t *buffer,
    size_t buffer_len,
    size_t *packet_count) {
    size_t offset = 0;
    size_t count = 0;
    uint32_t first_report_ssrc = 0;
    int has_matching_cname = 0;

    if (!buffer || buffer_len == 0) {
        return -1;
    }

    while (offset < buffer_len) {
        turbo_rtsp_rtcp_header_t header;
        size_t consumed = 0;
        int rc = turbo_rtsp_rtcp_next_packet(
            buffer + offset,
            buffer_len - offset,
            &header,
            &consumed);
        if (rc != 0 || consumed == 0) {
            return -1;
        }

        if (count == 0 &&
            header.packet_type != TURBO_RTSP_RTCP_SR &&
            header.packet_type != TURBO_RTSP_RTCP_RR) {
            return -1;
        }
        if (count == 0) {
            first_report_ssrc = turbo_rtsp_read_u32be(
                buffer + offset + TURBO_RTSP_RTCP_HEADER_SIZE);
        }
        if (header.padding && offset + consumed < buffer_len) {
            return -1;
        }
        if (header.packet_type == TURBO_RTSP_RTCP_SDES) {
            uint32_t ssrc = 0;
            const char *cname = NULL;
            size_t cname_len = 0;
            if (turbo_rtsp_rtcp_parse_sdes_cname(
                    buffer + offset,
                    consumed,
                    &ssrc,
                    &cname,
                    &cname_len) == 0 &&
                cname_len > 0 &&
                ssrc == first_report_ssrc) {
                has_matching_cname = 1;
            }
        }

        offset += consumed;
        ++count;
    }

    if (!has_matching_cname) {
        return -1;
    }
    if (packet_count) {
        *packet_count = count;
    }
    return 0;
}

uint32_t turbo_rtsp_rtcp_ntp_to_lsr(
    uint64_t ntp_timestamp) {
    return (uint32_t)((ntp_timestamp >> 16) & 0xffffffffu);
}

uint32_t turbo_rtsp_rtcp_delay_us_to_dlsr(
    uint64_t delay_us) {
    const uint64_t max_delay_us = ((uint64_t)UINT32_MAX * 1000000u) / 65536u;
    uint64_t value = 0;

    if (delay_us > max_delay_us) {
        return UINT32_MAX;
    }
    value = (delay_us * 65536u) / 1000000u;
    return (uint32_t)value;
}

int turbo_rtsp_rtcp_parse_sender_info(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtcp_sender_info_t *info) {
    if (!packet || !info || packet_len < TURBO_RTSP_RTCP_SENDER_INFO_SIZE) {
        return -1;
    }

    memset(info, 0, sizeof(*info));
    info->ssrc = turbo_rtsp_read_u32be(packet);
    info->ntp_timestamp = ((uint64_t)turbo_rtsp_read_u32be(packet + 4) << 32) |
                          (uint64_t)turbo_rtsp_read_u32be(packet + 8);
    info->rtp_timestamp = turbo_rtsp_read_u32be(packet + 12);
    info->packet_count = turbo_rtsp_read_u32be(packet + 16);
    info->octet_count = turbo_rtsp_read_u32be(packet + 20);
    return 0;
}

int turbo_rtsp_rtcp_write_sender_info(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtcp_sender_info_t *info) {
    if (!packet || !info || packet_size < TURBO_RTSP_RTCP_SENDER_INFO_SIZE) {
        return -1;
    }

    turbo_rtsp_write_u32be(packet, info->ssrc);
    turbo_rtsp_write_u32be(packet + 4, (uint32_t)(info->ntp_timestamp >> 32));
    turbo_rtsp_write_u32be(packet + 8, (uint32_t)(info->ntp_timestamp & 0xffffffffu));
    turbo_rtsp_write_u32be(packet + 12, info->rtp_timestamp);
    turbo_rtsp_write_u32be(packet + 16, info->packet_count);
    turbo_rtsp_write_u32be(packet + 20, info->octet_count);
    return TURBO_RTSP_RTCP_SENDER_INFO_SIZE;
}

int turbo_rtsp_rtcp_parse_report_block(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtcp_report_block_t *block) {
    uint32_t lost = 0;

    if (!packet || !block || packet_len < TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE) {
        return -1;
    }

    memset(block, 0, sizeof(*block));
    block->ssrc = turbo_rtsp_read_u32be(packet);
    block->fraction_lost = packet[4];
    lost = ((uint32_t)packet[5] << 16) |
           ((uint32_t)packet[6] << 8) |
           (uint32_t)packet[7];
    if ((lost & 0x00800000u) != 0) {
        lost |= 0xff000000u;
    }
    block->cumulative_lost = (int32_t)lost;
    block->extended_highest_sequence_number = turbo_rtsp_read_u32be(packet + 8);
    block->jitter = turbo_rtsp_read_u32be(packet + 12);
    block->last_sender_report = turbo_rtsp_read_u32be(packet + 16);
    block->delay_since_last_sender_report = turbo_rtsp_read_u32be(packet + 20);
    return 0;
}

int turbo_rtsp_rtcp_write_report_block(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtcp_report_block_t *block) {
    uint32_t lost = 0;

    if (!packet || !block || packet_size < TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE) {
        return -1;
    }
    if (block->cumulative_lost < -8388608 || block->cumulative_lost > 8388607) {
        return -1;
    }

    lost = (uint32_t)block->cumulative_lost & 0x00ffffffu;
    turbo_rtsp_write_u32be(packet, block->ssrc);
    packet[4] = block->fraction_lost;
    packet[5] = (uint8_t)(lost >> 16);
    packet[6] = (uint8_t)((lost >> 8) & 0xffu);
    packet[7] = (uint8_t)(lost & 0xffu);
    turbo_rtsp_write_u32be(packet + 8, block->extended_highest_sequence_number);
    turbo_rtsp_write_u32be(packet + 12, block->jitter);
    turbo_rtsp_write_u32be(packet + 16, block->last_sender_report);
    turbo_rtsp_write_u32be(packet + 20, block->delay_since_last_sender_report);
    return TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE;
}

int turbo_rtsp_rtcp_parse_receiver_report(
    const uint8_t *packet,
    size_t packet_len,
    uint32_t *reporter_ssrc,
    turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_capacity,
    size_t *block_count) {
    turbo_rtsp_rtcp_header_t header;
    size_t expected_len = 0;
    size_t i = 0;

    if (!packet || !reporter_ssrc || !block_count) {
        return -1;
    }
    if (turbo_rtsp_rtcp_parse_header(packet, packet_len, &header) != 0 ||
        header.packet_type != TURBO_RTSP_RTCP_RR) {
        return -1;
    }

    expected_len = TURBO_RTSP_RTCP_HEADER_SIZE + 4u +
                   ((size_t)header.count * TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE);
    *block_count = header.count;
    if (header.packet_len != expected_len || packet_len < expected_len ||
        (header.count > 0 && (!blocks || block_capacity < header.count))) {
        return -1;
    }

    *reporter_ssrc = turbo_rtsp_read_u32be(packet + TURBO_RTSP_RTCP_HEADER_SIZE);
    for (i = 0; i < (size_t)header.count; ++i) {
        const uint8_t *block_data = packet + TURBO_RTSP_RTCP_HEADER_SIZE + 4u +
                                    (i * TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE);
        if (turbo_rtsp_rtcp_parse_report_block(
                block_data,
                TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE,
                &blocks[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

int turbo_rtsp_rtcp_write_receiver_report(
    uint8_t *packet,
    size_t packet_size,
    uint32_t reporter_ssrc,
    const turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_count) {
    turbo_rtsp_rtcp_header_t header;
    size_t packet_len = 0;
    size_t i = 0;

    if (!packet || block_count > 31 || (block_count > 0 && !blocks)) {
        return -1;
    }

    packet_len = TURBO_RTSP_RTCP_HEADER_SIZE + 4u +
                 (block_count * TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE);
    if (packet_size < packet_len) {
        return -1;
    }

    memset(&header, 0, sizeof(header));
    header.version = 2;
    header.count = (uint8_t)block_count;
    header.packet_type = TURBO_RTSP_RTCP_RR;
    header.length = (uint16_t)((packet_len / 4u) - 1u);
    if (turbo_rtsp_rtcp_write_header(packet, packet_size, &header) !=
        TURBO_RTSP_RTCP_HEADER_SIZE) {
        return -1;
    }

    turbo_rtsp_write_u32be(packet + TURBO_RTSP_RTCP_HEADER_SIZE, reporter_ssrc);
    for (i = 0; i < block_count; ++i) {
        uint8_t *block_data = packet + TURBO_RTSP_RTCP_HEADER_SIZE + 4u +
                              (i * TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE);
        if (turbo_rtsp_rtcp_write_report_block(
                block_data,
                packet_size - (size_t)(block_data - packet),
                &blocks[i]) != TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE) {
            return -1;
        }
    }

    return (int)packet_len;
}

int turbo_rtsp_rtcp_parse_sender_report(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtcp_sender_info_t *sender_info,
    turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_capacity,
    size_t *block_count) {
    turbo_rtsp_rtcp_header_t header;
    size_t expected_len = 0;
    size_t i = 0;

    if (!packet || !sender_info || !block_count) {
        return -1;
    }
    if (turbo_rtsp_rtcp_parse_header(packet, packet_len, &header) != 0 ||
        header.packet_type != TURBO_RTSP_RTCP_SR) {
        return -1;
    }

    expected_len = TURBO_RTSP_RTCP_HEADER_SIZE + TURBO_RTSP_RTCP_SENDER_INFO_SIZE +
                   ((size_t)header.count * TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE);
    *block_count = header.count;
    if (header.packet_len != expected_len || packet_len < expected_len ||
        (header.count > 0 && (!blocks || block_capacity < header.count))) {
        return -1;
    }

    if (turbo_rtsp_rtcp_parse_sender_info(
            packet + TURBO_RTSP_RTCP_HEADER_SIZE,
            TURBO_RTSP_RTCP_SENDER_INFO_SIZE,
            sender_info) != 0) {
        return -1;
    }

    for (i = 0; i < (size_t)header.count; ++i) {
        const uint8_t *block_data = packet + TURBO_RTSP_RTCP_HEADER_SIZE +
                                    TURBO_RTSP_RTCP_SENDER_INFO_SIZE +
                                    (i * TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE);
        if (turbo_rtsp_rtcp_parse_report_block(
                block_data,
                TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE,
                &blocks[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

int turbo_rtsp_rtcp_write_sender_report(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtcp_sender_info_t *sender_info,
    const turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_count) {
    turbo_rtsp_rtcp_header_t header;
    size_t packet_len = 0;
    size_t i = 0;

    if (!packet || !sender_info || block_count > 31 ||
        (block_count > 0 && !blocks)) {
        return -1;
    }

    packet_len = TURBO_RTSP_RTCP_HEADER_SIZE + TURBO_RTSP_RTCP_SENDER_INFO_SIZE +
                 (block_count * TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE);
    if (packet_size < packet_len) {
        return -1;
    }

    memset(&header, 0, sizeof(header));
    header.version = 2;
    header.count = (uint8_t)block_count;
    header.packet_type = TURBO_RTSP_RTCP_SR;
    header.length = (uint16_t)((packet_len / 4u) - 1u);
    if (turbo_rtsp_rtcp_write_header(packet, packet_size, &header) !=
        TURBO_RTSP_RTCP_HEADER_SIZE) {
        return -1;
    }
    if (turbo_rtsp_rtcp_write_sender_info(
            packet + TURBO_RTSP_RTCP_HEADER_SIZE,
            packet_size - TURBO_RTSP_RTCP_HEADER_SIZE,
            sender_info) != TURBO_RTSP_RTCP_SENDER_INFO_SIZE) {
        return -1;
    }

    for (i = 0; i < block_count; ++i) {
        uint8_t *block_data = packet + TURBO_RTSP_RTCP_HEADER_SIZE +
                              TURBO_RTSP_RTCP_SENDER_INFO_SIZE +
                              (i * TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE);
        if (turbo_rtsp_rtcp_write_report_block(
                block_data,
                packet_size - (size_t)(block_data - packet),
                &blocks[i]) != TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE) {
            return -1;
        }
    }

    return (int)packet_len;
}

int turbo_rtsp_rtcp_parse_sdes_cname(
    const uint8_t *packet,
    size_t packet_len,
    uint32_t *ssrc,
    const char **cname,
    size_t *cname_len) {
    turbo_rtsp_rtcp_header_t header;
    size_t offset = TURBO_RTSP_RTCP_HEADER_SIZE;
    size_t chunk = 0;

    if (!packet || !ssrc || !cname || !cname_len) {
        return -1;
    }
    if (turbo_rtsp_rtcp_parse_header(packet, packet_len, &header) != 0 ||
        header.packet_type != TURBO_RTSP_RTCP_SDES ||
        header.count == 0) {
        return -1;
    }

    *cname = NULL;
    *cname_len = 0;
    for (chunk = 0; chunk < (size_t)header.count; ++chunk) {
        uint32_t chunk_ssrc = 0;
        int ended = 0;

        if (offset + 4u > header.packet_len) {
            return -1;
        }
        chunk_ssrc = turbo_rtsp_read_u32be(packet + offset);
        offset += 4u;

        while (offset < header.packet_len) {
            uint8_t item_type = packet[offset++];
            uint8_t item_len = 0;

            if (item_type == 0) {
                offset = turbo_rtsp_align4(offset);
                ended = 1;
                break;
            }
            if (offset >= header.packet_len) {
                return -1;
            }
            item_len = packet[offset++];
            if (offset + (size_t)item_len > header.packet_len) {
                return -1;
            }
            if (item_type == TURBO_RTSP_RTCP_SDES_CNAME) {
                *ssrc = chunk_ssrc;
                *cname = (const char *)(packet + offset);
                *cname_len = item_len;
                return 0;
            }
            offset += (size_t)item_len;
        }

        if (!ended) {
            return -1;
        }
    }

    return -1;
}

int turbo_rtsp_rtcp_write_sdes_cname(
    uint8_t *packet,
    size_t packet_size,
    uint32_t ssrc,
    const char *cname,
    size_t cname_len) {
    turbo_rtsp_rtcp_header_t header;
    size_t packet_len = 0;

    if (!packet ||
        turbo_rtsp_rtcp_sdes_cname_packet_len(cname, cname_len, &packet_len) != 0) {
        return -1;
    }

    if (packet_size < packet_len) {
        return -1;
    }

    memset(packet, 0, packet_len);
    memset(&header, 0, sizeof(header));
    header.version = 2;
    header.count = 1;
    header.packet_type = TURBO_RTSP_RTCP_SDES;
    header.length = (uint16_t)((packet_len / 4u) - 1u);
    if (turbo_rtsp_rtcp_write_header(packet, packet_size, &header) !=
        TURBO_RTSP_RTCP_HEADER_SIZE) {
        return -1;
    }

    turbo_rtsp_write_u32be(packet + TURBO_RTSP_RTCP_HEADER_SIZE, ssrc);
    packet[TURBO_RTSP_RTCP_HEADER_SIZE + 4u] = TURBO_RTSP_RTCP_SDES_CNAME;
    packet[TURBO_RTSP_RTCP_HEADER_SIZE + 5u] = (uint8_t)cname_len;
    memcpy(packet + TURBO_RTSP_RTCP_HEADER_SIZE + 6u, cname, cname_len);
    return (int)packet_len;
}

int turbo_rtsp_rtcp_parse_bye(
    const uint8_t *packet,
    size_t packet_len,
    uint32_t *ssrcs,
    size_t ssrc_capacity,
    size_t *ssrc_count,
    const char **reason,
    size_t *reason_len) {
    turbo_rtsp_rtcp_header_t header;
    size_t offset = TURBO_RTSP_RTCP_HEADER_SIZE;
    size_t i = 0;

    if (!packet || !ssrc_count) {
        return -1;
    }
    if (turbo_rtsp_rtcp_parse_header(packet, packet_len, &header) != 0 ||
        header.packet_type != TURBO_RTSP_RTCP_BYE ||
        header.count == 0) {
        return -1;
    }

    *ssrc_count = header.count;
    if (!ssrcs || ssrc_capacity < header.count) {
        return -1;
    }
    if (offset + ((size_t)header.count * 4u) > header.packet_len) {
        return -1;
    }

    for (i = 0; i < (size_t)header.count; ++i) {
        ssrcs[i] = turbo_rtsp_read_u32be(packet + offset);
        offset += 4u;
    }

    if (reason) {
        *reason = NULL;
    }
    if (reason_len) {
        *reason_len = 0;
    }
    if (offset < header.packet_len) {
        uint8_t text_len = packet[offset++];
        if (offset + (size_t)text_len > header.packet_len) {
            return -1;
        }
        if (reason) {
            *reason = (const char *)(packet + offset);
        }
        if (reason_len) {
            *reason_len = text_len;
        }
    }

    return 0;
}

int turbo_rtsp_rtcp_write_bye(
    uint8_t *packet,
    size_t packet_size,
    const uint32_t *ssrcs,
    size_t ssrc_count,
    const char *reason,
    size_t reason_len) {
    turbo_rtsp_rtcp_header_t header;
    size_t raw_len = 0;
    size_t packet_len = 0;
    size_t i = 0;

    if (!packet || !ssrcs || ssrc_count == 0 || ssrc_count > 31 ||
        reason_len > 255 || (reason_len > 0 && !reason)) {
        return -1;
    }

    raw_len = TURBO_RTSP_RTCP_HEADER_SIZE + (ssrc_count * 4u);
    if (reason_len > 0) {
        raw_len += 1u + reason_len;
    }
    packet_len = turbo_rtsp_align4(raw_len);
    if (packet_size < packet_len) {
        return -1;
    }

    memset(packet, 0, packet_len);
    memset(&header, 0, sizeof(header));
    header.version = 2;
    header.count = (uint8_t)ssrc_count;
    header.packet_type = TURBO_RTSP_RTCP_BYE;
    header.length = (uint16_t)((packet_len / 4u) - 1u);
    if (turbo_rtsp_rtcp_write_header(packet, packet_size, &header) !=
        TURBO_RTSP_RTCP_HEADER_SIZE) {
        return -1;
    }

    for (i = 0; i < ssrc_count; ++i) {
        turbo_rtsp_write_u32be(
            packet + TURBO_RTSP_RTCP_HEADER_SIZE + (i * 4u),
            ssrcs[i]);
    }
    if (reason_len > 0) {
        size_t offset = TURBO_RTSP_RTCP_HEADER_SIZE + (ssrc_count * 4u);
        packet[offset++] = (uint8_t)reason_len;
        memcpy(packet + offset, reason, reason_len);
    }

    return (int)packet_len;
}

int turbo_rtsp_rtcp_parse_app(
    const uint8_t *packet,
    size_t packet_len,
    turbo_rtsp_rtcp_app_t *app) {
    turbo_rtsp_rtcp_header_t header;

    if (!packet || !app) {
        return -1;
    }
    if (turbo_rtsp_rtcp_parse_header(packet, packet_len, &header) != 0 ||
        header.packet_type != TURBO_RTSP_RTCP_APP ||
        header.packet_len < TURBO_RTSP_RTCP_HEADER_SIZE + 8u) {
        return -1;
    }

    memset(app, 0, sizeof(*app));
    app->subtype = header.count;
    app->ssrc = turbo_rtsp_read_u32be(packet + TURBO_RTSP_RTCP_HEADER_SIZE);
    memcpy(app->name, packet + TURBO_RTSP_RTCP_HEADER_SIZE + 4u, sizeof(app->name));
    app->data = packet + TURBO_RTSP_RTCP_HEADER_SIZE + 8u;
    app->data_len = header.packet_len - TURBO_RTSP_RTCP_HEADER_SIZE - 8u;
    return 0;
}

int turbo_rtsp_rtcp_write_app(
    uint8_t *packet,
    size_t packet_size,
    const turbo_rtsp_rtcp_app_t *app) {
    turbo_rtsp_rtcp_header_t header;
    size_t packet_len = 0;

    if (!packet || !app || app->subtype > 31 ||
        (app->data_len > 0 && !app->data) ||
        (app->data_len % 4u) != 0) {
        return -1;
    }

    packet_len = TURBO_RTSP_RTCP_HEADER_SIZE + 8u + app->data_len;
    if (packet_size < packet_len || packet_len > ((size_t)UINT16_MAX + 1u) * 4u) {
        return -1;
    }

    memset(&header, 0, sizeof(header));
    header.version = 2;
    header.count = app->subtype;
    header.packet_type = TURBO_RTSP_RTCP_APP;
    header.length = (uint16_t)((packet_len / 4u) - 1u);
    if (turbo_rtsp_rtcp_write_header(packet, packet_size, &header) !=
        TURBO_RTSP_RTCP_HEADER_SIZE) {
        return -1;
    }

    turbo_rtsp_write_u32be(packet + TURBO_RTSP_RTCP_HEADER_SIZE, app->ssrc);
    memcpy(packet + TURBO_RTSP_RTCP_HEADER_SIZE + 4u, app->name, sizeof(app->name));
    if (app->data_len > 0) {
        memcpy(packet + TURBO_RTSP_RTCP_HEADER_SIZE + 8u, app->data, app->data_len);
    }
    return (int)packet_len;
}

static int turbo_rtsp_rtcp_parse_feedback_common(
    const uint8_t *packet,
    size_t packet_len,
    uint8_t packet_type,
    uint8_t feedback_type,
    uint32_t *sender_ssrc,
    uint32_t *media_ssrc,
    turbo_rtsp_rtcp_header_t *header) {
    if (!packet || !sender_ssrc || !media_ssrc || !header) {
        return -1;
    }
    if (turbo_rtsp_rtcp_parse_header(packet, packet_len, header) != 0 ||
        header->packet_type != packet_type ||
        header->count != feedback_type ||
        header->payload_len < 8u) {
        return -1;
    }

    *sender_ssrc = turbo_rtsp_read_u32be(packet + TURBO_RTSP_RTCP_HEADER_SIZE);
    *media_ssrc = turbo_rtsp_read_u32be(packet + TURBO_RTSP_RTCP_HEADER_SIZE + 4u);
    return 0;
}

int turbo_rtsp_rtcp_parse_generic_nack(
    const uint8_t *packet,
    size_t packet_len,
    uint32_t *sender_ssrc,
    uint32_t *media_ssrc,
    turbo_rtsp_rtcp_nack_item_t *items,
    size_t item_capacity,
    size_t *item_count) {
    turbo_rtsp_rtcp_header_t header;
    size_t fci_len = 0;
    size_t count = 0;
    size_t i = 0;
    const uint8_t *fci = NULL;

    if (!item_count) {
        return -1;
    }
    *item_count = 0;
    if (turbo_rtsp_rtcp_parse_feedback_common(
            packet,
            packet_len,
            TURBO_RTSP_RTCP_RTPFB,
            TURBO_RTSP_RTCP_RTPFB_NACK,
            sender_ssrc,
            media_ssrc,
            &header) != 0 ||
        header.payload_len < 8u) {
        return -1;
    }

    fci_len = header.payload_len - 8u;
    if (fci_len == 0 || (fci_len % 4u) != 0) {
        return -1;
    }
    count = fci_len / 4u;
    *item_count = count;
    if (!items || item_capacity < count) {
        return -1;
    }

    fci = packet + TURBO_RTSP_RTCP_HEADER_SIZE + 8u;
    for (i = 0; i < count; ++i) {
        items[i].packet_id = turbo_rtsp_read_u16be(fci + (i * 4u));
        items[i].lost_packet_bitmask = turbo_rtsp_read_u16be(fci + (i * 4u) + 2u);
    }
    return 0;
}

int turbo_rtsp_rtcp_write_generic_nack(
    uint8_t *packet,
    size_t packet_size,
    uint32_t sender_ssrc,
    uint32_t media_ssrc,
    const turbo_rtsp_rtcp_nack_item_t *items,
    size_t item_count) {
    turbo_rtsp_rtcp_header_t header;
    size_t packet_len = 0;
    size_t i = 0;

    if (!packet || !items || item_count == 0) {
        return -1;
    }
    if (item_count > (SIZE_MAX - 12u) / 4u) {
        return -1;
    }
    packet_len = 12u + (item_count * 4u);
    if (packet_size < packet_len || packet_len > ((size_t)UINT16_MAX + 1u) * 4u) {
        return -1;
    }

    memset(&header, 0, sizeof(header));
    header.version = 2;
    header.count = TURBO_RTSP_RTCP_RTPFB_NACK;
    header.packet_type = TURBO_RTSP_RTCP_RTPFB;
    header.length = (uint16_t)((packet_len / 4u) - 1u);
    if (turbo_rtsp_rtcp_write_header(packet, packet_size, &header) !=
        TURBO_RTSP_RTCP_HEADER_SIZE) {
        return -1;
    }

    turbo_rtsp_write_u32be(packet + TURBO_RTSP_RTCP_HEADER_SIZE, sender_ssrc);
    turbo_rtsp_write_u32be(packet + TURBO_RTSP_RTCP_HEADER_SIZE + 4u, media_ssrc);
    for (i = 0; i < item_count; ++i) {
        uint8_t *item = packet + TURBO_RTSP_RTCP_HEADER_SIZE + 8u + (i * 4u);
        turbo_rtsp_write_u16be(item, items[i].packet_id);
        turbo_rtsp_write_u16be(item + 2u, items[i].lost_packet_bitmask);
    }
    return (int)packet_len;
}

int turbo_rtsp_rtcp_parse_pli(
    const uint8_t *packet,
    size_t packet_len,
    uint32_t *sender_ssrc,
    uint32_t *media_ssrc) {
    turbo_rtsp_rtcp_header_t header;

    if (turbo_rtsp_rtcp_parse_feedback_common(
            packet,
            packet_len,
            TURBO_RTSP_RTCP_PSFB,
            TURBO_RTSP_RTCP_PSFB_PLI,
            sender_ssrc,
            media_ssrc,
            &header) != 0 ||
        header.payload_len != 8u) {
        return -1;
    }
    return 0;
}

int turbo_rtsp_rtcp_write_pli(
    uint8_t *packet,
    size_t packet_size,
    uint32_t sender_ssrc,
    uint32_t media_ssrc) {
    turbo_rtsp_rtcp_header_t header;
    const size_t packet_len = 12u;

    if (!packet || packet_size < packet_len) {
        return -1;
    }

    memset(&header, 0, sizeof(header));
    header.version = 2;
    header.count = TURBO_RTSP_RTCP_PSFB_PLI;
    header.packet_type = TURBO_RTSP_RTCP_PSFB;
    header.length = (uint16_t)((packet_len / 4u) - 1u);
    if (turbo_rtsp_rtcp_write_header(packet, packet_size, &header) !=
        TURBO_RTSP_RTCP_HEADER_SIZE) {
        return -1;
    }

    turbo_rtsp_write_u32be(packet + TURBO_RTSP_RTCP_HEADER_SIZE, sender_ssrc);
    turbo_rtsp_write_u32be(packet + TURBO_RTSP_RTCP_HEADER_SIZE + 4u, media_ssrc);
    return (int)packet_len;
}

void turbo_rtsp_rtp_source_init(
    turbo_rtsp_rtp_source_t *source,
    uint32_t ssrc) {
    if (!source) {
        return;
    }
    memset(source, 0, sizeof(*source));
    source->ssrc = ssrc;
    source->has_ssrc = ssrc != 0 ? 1u : 0u;
}

static void turbo_rtsp_rtp_source_update_jitter(
    turbo_rtsp_rtp_source_t *source,
    const turbo_rtsp_rtp_header_t *header,
    uint32_t arrival_rtp_timestamp) {
    int32_t transit = (int32_t)(arrival_rtp_timestamp - header->timestamp);
    int32_t delta = transit - source->transit;

    if (delta < 0) {
        delta = -delta;
    }
    source->transit = transit;
    source->jitter_q4 += (uint32_t)delta - ((source->jitter_q4 + 8u) >> 4);
}

static void turbo_rtsp_rtp_source_mark_received(
    turbo_rtsp_rtp_source_t *source,
    uint32_t sequence_delta) {
    if (sequence_delta >= 64u) {
        source->received_sequence_bitmap = 1u;
    } else {
        source->received_sequence_bitmap =
            (source->received_sequence_bitmap << sequence_delta) | 1u;
    }
}

turbo_rtsp_rtp_source_update_result_t turbo_rtsp_rtp_source_update_ex(
    turbo_rtsp_rtp_source_t *source,
    const turbo_rtsp_rtp_header_t *header,
    uint32_t arrival_rtp_timestamp) {
    uint16_t sequence = 0;
    int32_t transit = 0;
    uint32_t sequence_delta = 0;
    turbo_rtsp_rtp_source_update_result_t result =
        TURBO_RTSP_RTP_SOURCE_UPDATE_OK;

    if (!source || !header || header->version != 2) {
        return TURBO_RTSP_RTP_SOURCE_UPDATE_ERROR;
    }

    sequence = header->sequence_number;
    transit = (int32_t)(arrival_rtp_timestamp - header->timestamp);

    if (!source->initialized) {
        if (source->has_ssrc && source->ssrc != header->ssrc) {
            return TURBO_RTSP_RTP_SOURCE_UPDATE_SSRC_MISMATCH;
        }
        source->ssrc = header->ssrc;
        source->has_ssrc = 1;
        source->base_sequence_number = sequence;
        source->max_sequence_number = sequence;
        source->received = 1;
        source->transit = transit;
        source->received_sequence_bitmap = 1u;
        source->initialized = 1;
        return TURBO_RTSP_RTP_SOURCE_UPDATE_OK;
    }

    if (source->ssrc != header->ssrc) {
        return TURBO_RTSP_RTP_SOURCE_UPDATE_SSRC_MISMATCH;
    }

    if (sequence == source->max_sequence_number) {
        return TURBO_RTSP_RTP_SOURCE_UPDATE_DUPLICATE;
    }

    if (sequence > source->max_sequence_number) {
        sequence_delta = (uint32_t)(sequence - source->max_sequence_number);
        if (sequence_delta >= 0x8000u) {
            uint32_t previous_cycle_delta =
                (uint32_t)source->max_sequence_number +
                (TURBO_RTSP_RTP_SEQ_MOD - (uint32_t)sequence);

            if (source->cycles > 0 &&
                previous_cycle_delta < 64u &&
                (source->received_sequence_bitmap & (1ull << previous_cycle_delta)) != 0) {
                return TURBO_RTSP_RTP_SOURCE_UPDATE_DUPLICATE;
            }
            result = TURBO_RTSP_RTP_SOURCE_UPDATE_LATE_OR_OUT_OF_ORDER;
            if (source->cycles > 0 && previous_cycle_delta < 64u) {
                source->received_sequence_bitmap |= 1ull << previous_cycle_delta;
            }
        } else {
            if (sequence_delta > TURBO_RTSP_RTP_MAX_DROPOUT) {
                result = TURBO_RTSP_RTP_SOURCE_UPDATE_DROPOUT;
            }
            source->max_sequence_number = sequence;
            turbo_rtsp_rtp_source_mark_received(source, sequence_delta);
        }
    } else {
        uint32_t reverse_delta =
            (uint32_t)(source->max_sequence_number - sequence);

        if (reverse_delta > 0x8000u) {
            sequence_delta = TURBO_RTSP_RTP_SEQ_MOD - reverse_delta;
            if (sequence_delta > TURBO_RTSP_RTP_MAX_DROPOUT) {
                result = TURBO_RTSP_RTP_SOURCE_UPDATE_DROPOUT;
            }
            source->cycles += TURBO_RTSP_RTP_SEQ_MOD;
            source->max_sequence_number = sequence;
            turbo_rtsp_rtp_source_mark_received(source, sequence_delta);
        } else {
            if (reverse_delta < 64u &&
                (source->received_sequence_bitmap & (1ull << reverse_delta)) != 0) {
                return TURBO_RTSP_RTP_SOURCE_UPDATE_DUPLICATE;
            }
            result = TURBO_RTSP_RTP_SOURCE_UPDATE_LATE_OR_OUT_OF_ORDER;
            if (reverse_delta < 64u) {
                source->received_sequence_bitmap |= 1ull << reverse_delta;
            }
        }
    }

    turbo_rtsp_rtp_source_update_jitter(source, header, arrival_rtp_timestamp);

    ++source->received;
    return result;
}

int turbo_rtsp_rtp_source_update(
    turbo_rtsp_rtp_source_t *source,
    const turbo_rtsp_rtp_header_t *header,
    uint32_t arrival_rtp_timestamp) {
    turbo_rtsp_rtp_source_update_result_t result =
        turbo_rtsp_rtp_source_update_ex(source, header, arrival_rtp_timestamp);

    if (result == TURBO_RTSP_RTP_SOURCE_UPDATE_ERROR ||
        result == TURBO_RTSP_RTP_SOURCE_UPDATE_SSRC_MISMATCH) {
        return -1;
    }
    return 0;
}

int turbo_rtsp_rtp_source_make_report(
    turbo_rtsp_rtp_source_t *source,
    uint32_t last_sender_report,
    uint32_t delay_since_last_sender_report,
    turbo_rtsp_rtcp_report_block_t *block) {
    uint32_t extended_max = 0;
    uint32_t expected = 0;
    uint32_t expected_interval = 0;
    uint32_t received_interval = 0;
    int32_t lost = 0;
    int32_t lost_interval = 0;

    if (!source || !block || !source->initialized) {
        return -1;
    }

    extended_max = source->cycles + (uint32_t)source->max_sequence_number;
    expected = extended_max - (uint32_t)source->base_sequence_number + 1u;
    lost = (int32_t)(expected - source->received);
    if (lost < -8388608) {
        lost = -8388608;
    } else if (lost > 8388607) {
        lost = 8388607;
    }

    expected_interval = expected - source->expected_prior;
    received_interval = source->received - source->received_prior;
    lost_interval = (int32_t)(expected_interval - received_interval);

    memset(block, 0, sizeof(*block));
    block->ssrc = source->ssrc;
    if (expected_interval > 0 && lost_interval > 0) {
        block->fraction_lost = (uint8_t)(((uint32_t)lost_interval << 8) / expected_interval);
    }
    block->cumulative_lost = lost;
    block->extended_highest_sequence_number = extended_max;
    block->jitter = (source->jitter_q4 + 8u) >> 4;
    block->last_sender_report = last_sender_report;
    block->delay_since_last_sender_report = delay_since_last_sender_report;

    source->expected_prior = expected;
    source->received_prior = source->received;
    return 0;
}

int turbo_rtsp_rtp_source_write_receiver_report(
    turbo_rtsp_rtp_source_t *source,
    uint8_t *packet,
    size_t packet_size,
    uint32_t reporter_ssrc,
    uint32_t last_sender_report,
    uint32_t delay_since_last_sender_report) {
    turbo_rtsp_rtcp_report_block_t block;

    if (!source || !packet) {
        return -1;
    }
    if (turbo_rtsp_rtp_source_make_report(
            source,
            last_sender_report,
            delay_since_last_sender_report,
            &block) != 0) {
        return -1;
    }
    return turbo_rtsp_rtcp_write_receiver_report(
        packet,
        packet_size,
        reporter_ssrc,
        &block,
        1);
}

int turbo_rtsp_rtp_source_write_rtcp_compound(
    turbo_rtsp_rtp_source_t *source,
    uint8_t *packet,
    size_t packet_size,
    uint32_t reporter_ssrc,
    uint32_t last_sender_report,
    uint32_t delay_since_last_sender_report,
    const char *cname,
    size_t cname_len) {
    const size_t rr_len = TURBO_RTSP_RTCP_HEADER_SIZE + 4u +
                          TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE;
    size_t sdes_len = 0;
    int written = 0;

    if (!source || !packet ||
        turbo_rtsp_rtcp_sdes_cname_packet_len(cname, cname_len, &sdes_len) != 0 ||
        sdes_len > SIZE_MAX - rr_len ||
        packet_size < rr_len + sdes_len) {
        return -1;
    }

    written = turbo_rtsp_rtp_source_write_receiver_report(
        source,
        packet,
        packet_size,
        reporter_ssrc,
        last_sender_report,
        delay_since_last_sender_report);
    if (written != (int)rr_len) {
        return -1;
    }

    if (turbo_rtsp_rtcp_write_sdes_cname(
            packet + rr_len,
            packet_size - rr_len,
            reporter_ssrc,
            cname,
            cname_len) != (int)sdes_len) {
        return -1;
    }

    return (int)(rr_len + sdes_len);
}

void turbo_rtsp_rtp_sender_init(
    turbo_rtsp_rtp_sender_t *sender,
    uint32_t ssrc) {
    if (!sender) {
        return;
    }
    memset(sender, 0, sizeof(*sender));
    sender->ssrc = ssrc;
    sender->has_ssrc = ssrc != 0 ? 1u : 0u;
}

int turbo_rtsp_rtp_sender_update(
    turbo_rtsp_rtp_sender_t *sender,
    const turbo_rtsp_rtp_header_t *header) {
    if (!sender || !header || header->version != 2) {
        return -1;
    }
    if (sender->has_ssrc && sender->ssrc != header->ssrc) {
        return -1;
    }
    if (!sender->has_ssrc) {
        sender->ssrc = header->ssrc;
        sender->has_ssrc = 1;
    }
    ++sender->packet_count;
    sender->octet_count += (uint32_t)header->payload_len;
    return 0;
}

int turbo_rtsp_rtp_sender_write_sender_report(
    const turbo_rtsp_rtp_sender_t *sender,
    uint8_t *packet,
    size_t packet_size,
    uint64_t ntp_timestamp,
    uint32_t rtp_timestamp,
    const turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_count) {
    turbo_rtsp_rtcp_sender_info_t info;

    if (!sender || !sender->has_ssrc || !packet) {
        return -1;
    }

    memset(&info, 0, sizeof(info));
    info.ssrc = sender->ssrc;
    info.ntp_timestamp = ntp_timestamp;
    info.rtp_timestamp = rtp_timestamp;
    info.packet_count = sender->packet_count;
    info.octet_count = sender->octet_count;

    return turbo_rtsp_rtcp_write_sender_report(
        packet,
        packet_size,
        &info,
        blocks,
        block_count);
}

int turbo_rtsp_rtp_sender_write_rtcp_compound(
    const turbo_rtsp_rtp_sender_t *sender,
    uint8_t *packet,
    size_t packet_size,
    uint64_t ntp_timestamp,
    uint32_t rtp_timestamp,
    const turbo_rtsp_rtcp_report_block_t *blocks,
    size_t block_count,
    const char *cname,
    size_t cname_len) {
    size_t sr_len = 0;
    size_t sdes_len = 0;
    int written = 0;

    if (!sender || !sender->has_ssrc || !packet || block_count > 31 ||
        (block_count > 0 && !blocks) ||
        turbo_rtsp_rtcp_sdes_cname_packet_len(cname, cname_len, &sdes_len) != 0) {
        return -1;
    }

    sr_len = TURBO_RTSP_RTCP_HEADER_SIZE + TURBO_RTSP_RTCP_SENDER_INFO_SIZE +
             (block_count * TURBO_RTSP_RTCP_REPORT_BLOCK_SIZE);
    if (sdes_len > SIZE_MAX - sr_len || packet_size < sr_len + sdes_len) {
        return -1;
    }

    written = turbo_rtsp_rtp_sender_write_sender_report(
        sender,
        packet,
        packet_size,
        ntp_timestamp,
        rtp_timestamp,
        blocks,
        block_count);
    if (written != (int)sr_len) {
        return -1;
    }

    if (turbo_rtsp_rtcp_write_sdes_cname(
            packet + sr_len,
            packet_size - sr_len,
            sender->ssrc,
            cname,
            cname_len) != (int)sdes_len) {
        return -1;
    }

    return (int)(sr_len + sdes_len);
}

void turbo_rtsp_rtp_stream_init(
    turbo_rtsp_rtp_stream_t *stream,
    uint8_t payload_type,
    uint32_t ssrc,
    uint16_t sequence_number,
    uint32_t timestamp,
    uint32_t clock_rate) {
    if (!stream) {
        return;
    }

    memset(stream, 0, sizeof(*stream));
    stream->payload_type = payload_type;
    stream->ssrc = ssrc;
    stream->sequence_number = sequence_number;
    stream->timestamp = timestamp;
    stream->clock_rate = clock_rate;
    turbo_rtsp_rtp_sender_init(&stream->sender, ssrc);
}

int turbo_rtsp_rtp_stream_write_payload(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *payload,
    size_t payload_len,
    int marker,
    uint32_t timestamp_increment,
    uint8_t *packet,
    size_t packet_size,
    size_t *packet_len) {
    turbo_rtsp_rtp_stream_t next_stream;
    turbo_rtsp_rtp_header_t header;
    int written = 0;

    if (packet_len) {
        *packet_len = 0;
    }
    if (!stream || !payload || payload_len == 0 || stream->payload_type > 127 ||
        !packet || !packet_len || payload_len > SIZE_MAX - TURBO_RTSP_RTP_HEADER_SIZE ||
        packet_size < TURBO_RTSP_RTP_HEADER_SIZE + payload_len) {
        return -1;
    }

    next_stream = *stream;
    memset(&header, 0, sizeof(header));
    header.version = 2;
    header.marker = marker ? 1 : 0;
    header.payload_type = next_stream.payload_type;
    header.sequence_number = next_stream.sequence_number;
    header.timestamp = next_stream.timestamp;
    header.ssrc = next_stream.ssrc;
    header.payload_len = payload_len;

    written = turbo_rtsp_rtp_write_packet(
        packet,
        packet_size,
        &header,
        payload,
        payload_len);
    if (written < 0 || turbo_rtsp_rtp_sender_update(&next_stream.sender, &header) != 0) {
        return -1;
    }

    ++next_stream.sequence_number;
    next_stream.timestamp += timestamp_increment;
    *stream = next_stream;
    *packet_len = (size_t)written;
    return 0;
}

static void turbo_rtsp_mpeg4_generic_write_aac_hbr_au_header(
    uint8_t *payload,
    size_t au_len) {
    payload[0] = 0;
    payload[1] = TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_AU_HEADER_BITS;
    payload[2] = (uint8_t)(au_len >> TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_INDEX_LENGTH);
    payload[3] = (uint8_t)((au_len & 0x1fu) << TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_INDEX_LENGTH);
}

int turbo_rtsp_rtp_stream_write_mpeg4_generic_au(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *au,
    size_t au_len,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count) {
    turbo_rtsp_rtp_stream_t next_stream;
    turbo_rtsp_rtp_header_t header;
    size_t fragment_capacity = 0;
    size_t needed_packets = 0;
    size_t max_written_payload = 0;
    size_t offset = 0;
    size_t i = 0;

    if (packet_count) {
        *packet_count = 0;
    }
    if (!stream || !au || au_len == 0 ||
        au_len > TURBO_RTSP_MPEG4_GENERIC_AAC_HBR_MAX_AU_SIZE ||
        stream->payload_type > 127 ||
        max_payload <= TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE ||
        max_payload > SIZE_MAX - TURBO_RTSP_RTP_HEADER_SIZE ||
        !packet || packet_size < TURBO_RTSP_RTP_HEADER_SIZE ||
        !packets || !packet_count || packet_count_capacity == 0) {
        return -1;
    }

    fragment_capacity = max_payload - TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE;
    needed_packets = (au_len + fragment_capacity - 1u) / fragment_capacity;
    max_written_payload = au_len <= fragment_capacity
                              ? TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE + au_len
                              : max_payload;
    if (needed_packets > packet_count_capacity ||
        packet_size < TURBO_RTSP_RTP_HEADER_SIZE + max_written_payload) {
        return -1;
    }

    next_stream = *stream;
    for (i = 0; i < needed_packets; ++i) {
        uint8_t *packet_slot = packet + (i * packet_size);
        uint8_t *rtp_payload = NULL;
        size_t remaining = au_len - offset;
        size_t fragment_len = remaining < fragment_capacity ? remaining : fragment_capacity;
        int header_len = 0;

        memset(&header, 0, sizeof(header));
        header.version = 2;
        header.marker = remaining == fragment_len ? 1 : 0;
        header.payload_type = next_stream.payload_type;
        header.sequence_number = next_stream.sequence_number;
        header.timestamp = next_stream.timestamp;
        header.ssrc = next_stream.ssrc;
        header.payload_len = TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE + fragment_len;

        header_len = turbo_rtsp_rtp_write_header(packet_slot, packet_size, &header);
        if (header_len < 0 ||
            header.payload_len > packet_size - (size_t)header_len) {
            return -1;
        }

        rtp_payload = packet_slot + header_len;
        turbo_rtsp_mpeg4_generic_write_aac_hbr_au_header(rtp_payload, au_len);
        memcpy(
            rtp_payload + TURBO_RTSP_MPEG4_GENERIC_AU_HEADER_SIZE,
            au + offset,
            fragment_len);
        if (turbo_rtsp_rtp_sender_update(&next_stream.sender, &header) != 0) {
            return -1;
        }

        packets[i].packet = packet_slot;
        packets[i].packet_len = (size_t)header_len + header.payload_len;
        ++next_stream.sequence_number;
        offset += fragment_len;
    }

    next_stream.timestamp += timestamp_increment;
    *stream = next_stream;
    *packet_count = needed_packets;
    return 0;
}

int turbo_rtsp_rtp_stream_write_mp4a_latm(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *latm,
    size_t latm_len,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count) {
    turbo_rtsp_rtp_stream_t next_stream;
    turbo_rtsp_rtp_header_t header;
    uint8_t length_info[TURBO_RTSP_MP4A_LATM_MAX_LENGTH_INFO_SIZE];
    size_t length_info_len = 0;
    size_t first_fragment_capacity = 0;
    size_t remaining_after_first = 0;
    size_t needed_packets = 0;
    size_t max_written_payload = 0;
    size_t offset = 0;
    size_t i = 0;

    if (packet_count) {
        *packet_count = 0;
    }
    if (!stream || !latm || latm_len == 0 ||
        stream->payload_type > 127 ||
        max_payload == 0 ||
        max_payload > SIZE_MAX - TURBO_RTSP_RTP_HEADER_SIZE ||
        !packet || packet_size < TURBO_RTSP_RTP_HEADER_SIZE ||
        !packets || !packet_count || packet_count_capacity == 0 ||
        turbo_rtsp_mp4a_latm_length_info_size(latm_len, &length_info_len) != 0 ||
        max_payload <= length_info_len) {
        return -1;
    }

    first_fragment_capacity = max_payload - length_info_len;
    remaining_after_first = latm_len > first_fragment_capacity
                                ? latm_len - first_fragment_capacity
                                : 0;
    needed_packets = 1;
    if (remaining_after_first > 0) {
        needed_packets += (remaining_after_first + max_payload - 1u) / max_payload;
    }
    max_written_payload = latm_len <= first_fragment_capacity
                              ? length_info_len + latm_len
                              : max_payload;
    if (needed_packets > packet_count_capacity ||
        packet_size < TURBO_RTSP_RTP_HEADER_SIZE + max_written_payload) {
        return -1;
    }

    turbo_rtsp_mp4a_latm_write_length_info(length_info, latm_len, length_info_len);
    next_stream = *stream;
    for (i = 0; i < needed_packets; ++i) {
        uint8_t *packet_slot = packet + (i * packet_size);
        uint8_t *rtp_payload = NULL;
        size_t remaining = latm_len - offset;
        size_t prefix_len = i == 0 ? length_info_len : 0;
        size_t fragment_capacity = max_payload - prefix_len;
        size_t fragment_len = remaining < fragment_capacity ? remaining : fragment_capacity;
        int header_len = 0;

        memset(&header, 0, sizeof(header));
        header.version = 2;
        header.marker = remaining == fragment_len ? 1 : 0;
        header.payload_type = next_stream.payload_type;
        header.sequence_number = next_stream.sequence_number;
        header.timestamp = next_stream.timestamp;
        header.ssrc = next_stream.ssrc;
        header.payload_len = prefix_len + fragment_len;

        header_len = turbo_rtsp_rtp_write_header(packet_slot, packet_size, &header);
        if (header_len < 0 ||
            header.payload_len > packet_size - (size_t)header_len) {
            return -1;
        }

        rtp_payload = packet_slot + header_len;
        if (prefix_len > 0) {
            memcpy(rtp_payload, length_info, prefix_len);
        }
        memcpy(rtp_payload + prefix_len, latm + offset, fragment_len);
        if (turbo_rtsp_rtp_sender_update(&next_stream.sender, &header) != 0) {
            return -1;
        }

        packets[i].packet = packet_slot;
        packets[i].packet_len = (size_t)header_len + header.payload_len;
        ++next_stream.sequence_number;
        offset += fragment_len;
    }

    next_stream.timestamp += timestamp_increment;
    *stream = next_stream;
    *packet_count = needed_packets;
    return 0;
}

int turbo_rtsp_rtp_stream_write_mpeg2_ts(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *ts,
    size_t ts_len,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count) {
    turbo_rtsp_rtp_stream_t next_stream;
    turbo_rtsp_rtp_header_t header;
    turbo_rtsp_mpeg2_ts_payload_t parsed_ts;
    size_t payload_capacity = 0;
    size_t needed_packets = 0;
    size_t max_written_payload = 0;
    size_t offset = 0;
    size_t i = 0;

    if (packet_count) {
        *packet_count = 0;
    }
    if (!stream || !ts || ts_len == 0 ||
        ts_len % TURBO_RTSP_MPEG2_TS_PACKET_SIZE != 0 ||
        stream->payload_type > 127 ||
        max_payload < TURBO_RTSP_MPEG2_TS_PACKET_SIZE ||
        max_payload > SIZE_MAX - TURBO_RTSP_RTP_HEADER_SIZE ||
        !packet || packet_size < TURBO_RTSP_RTP_HEADER_SIZE ||
        !packets || !packet_count || packet_count_capacity == 0) {
        return -1;
    }
    memset(&parsed_ts, 0, sizeof(parsed_ts));
    if (turbo_rtsp_mpeg2_ts_payload_parse(ts, ts_len, &parsed_ts) != 0) {
        return -1;
    }

    payload_capacity = (max_payload / TURBO_RTSP_MPEG2_TS_PACKET_SIZE) *
                       TURBO_RTSP_MPEG2_TS_PACKET_SIZE;
    if (payload_capacity < TURBO_RTSP_MPEG2_TS_PACKET_SIZE) {
        return -1;
    }
    needed_packets = (ts_len + payload_capacity - 1u) / payload_capacity;
    max_written_payload = ts_len < payload_capacity ? ts_len : payload_capacity;
    if (needed_packets > packet_count_capacity ||
        packet_size < TURBO_RTSP_RTP_HEADER_SIZE + max_written_payload) {
        return -1;
    }

    next_stream = *stream;
    for (i = 0; i < needed_packets; ++i) {
        uint8_t *packet_slot = packet + (i * packet_size);
        size_t remaining = ts_len - offset;
        size_t chunk_len = remaining < payload_capacity ? remaining : payload_capacity;
        int written = 0;

        memset(&header, 0, sizeof(header));
        header.version = 2;
        header.marker = 0;
        header.payload_type = next_stream.payload_type;
        header.sequence_number = next_stream.sequence_number;
        header.timestamp = next_stream.timestamp;
        header.ssrc = next_stream.ssrc;
        header.payload_len = chunk_len;

        written = turbo_rtsp_rtp_write_packet(
            packet_slot,
            packet_size,
            &header,
            ts + offset,
            chunk_len);
        if (written < 0 || turbo_rtsp_rtp_sender_update(&next_stream.sender, &header) != 0) {
            return -1;
        }

        packets[i].packet = packet_slot;
        packets[i].packet_len = (size_t)written;
        ++next_stream.sequence_number;
        offset += chunk_len;
    }

    next_stream.timestamp += timestamp_increment;
    *stream = next_stream;
    *packet_count = needed_packets;
    return 0;
}

static int turbo_rtsp_rtp_stream_h264_packet_count(
    const uint8_t *nal,
    size_t nal_len,
    size_t max_payload,
    size_t *packet_count) {
    turbo_rtsp_h264_packetizer_t packetizer;
    size_t fragment_data = 0;
    size_t payload_data = 0;

    if (!packet_count) {
        return -1;
    }
    memset(&packetizer, 0, sizeof(packetizer));
    if (turbo_rtsp_h264_packetizer_init(&packetizer, nal, nal_len, max_payload) != 0) {
        return -1;
    }

    if (nal_len <= max_payload) {
        *packet_count = 1;
        return 0;
    }

    fragment_data = max_payload - 2u;
    payload_data = nal_len - 1u;
    *packet_count = (payload_data + fragment_data - 1u) / fragment_data;
    return 0;
}

static int turbo_rtsp_rtp_stream_h265_packet_count(
    const uint8_t *nal,
    size_t nal_len,
    size_t max_payload,
    size_t *packet_count) {
    turbo_rtsp_h265_packetizer_t packetizer;
    size_t fragment_data = 0;
    size_t payload_data = 0;

    if (!packet_count) {
        return -1;
    }
    memset(&packetizer, 0, sizeof(packetizer));
    if (turbo_rtsp_h265_packetizer_init(&packetizer, nal, nal_len, max_payload) != 0) {
        return -1;
    }

    if (nal_len <= max_payload) {
        *packet_count = 1;
        return 0;
    }

    fragment_data = max_payload - 3u;
    payload_data = nal_len - TURBO_RTSP_H265_NAL_HEADER_SIZE;
    *packet_count = (payload_data + fragment_data - 1u) / fragment_data;
    return 0;
}

int turbo_rtsp_rtp_stream_write_h264_nal(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *nal,
    size_t nal_len,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    uint8_t *payload_scratch,
    size_t payload_scratch_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count) {
    return turbo_rtsp_rtp_stream_write_h264_nal_ex(
        stream,
        nal,
        nal_len,
        1,
        timestamp_increment,
        max_payload,
        packet,
        packet_size,
        payload_scratch,
        payload_scratch_size,
        packets,
        packet_count_capacity,
        packet_count);
}

int turbo_rtsp_rtp_stream_write_h264_nal_ex(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *nal,
    size_t nal_len,
    int marker,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    uint8_t *payload_scratch,
    size_t payload_scratch_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count) {
    turbo_rtsp_h264_packetizer_t packetizer;
    turbo_rtsp_rtp_stream_t next_stream;
    size_t needed_packets = 0;
    size_t max_written_payload = 0;
    size_t i = 0;

    if (packet_count) {
        *packet_count = 0;
    }
    if (!stream || !nal || nal_len == 0 || stream->payload_type > 127 ||
        max_payload == 0 || !packet || packet_size < TURBO_RTSP_RTP_HEADER_SIZE ||
        !packets || !packet_count || packet_count_capacity == 0) {
        return -1;
    }
    if (turbo_rtsp_rtp_stream_h264_packet_count(
            nal,
            nal_len,
            max_payload,
            &needed_packets) != 0) {
        return -1;
    }
    if (needed_packets > packet_count_capacity ||
        needed_packets > SIZE_MAX / packet_size) {
        return -1;
    }
    max_written_payload = nal_len <= max_payload ? nal_len : max_payload;
    if (max_written_payload > SIZE_MAX - TURBO_RTSP_RTP_HEADER_SIZE ||
        packet_size < TURBO_RTSP_RTP_HEADER_SIZE + max_written_payload) {
        return -1;
    }
    if (nal_len > max_payload &&
        (!payload_scratch || payload_scratch_size < max_payload)) {
        return -1;
    }

    if (turbo_rtsp_h264_packetizer_init(
            &packetizer,
            nal,
            nal_len,
            max_payload) != 0) {
        return -1;
    }

    next_stream = *stream;
    for (i = 0; i < needed_packets; ++i) {
        turbo_rtsp_h264_packetized_payload_t payload;
        turbo_rtsp_rtp_header_t header;
        uint8_t *packet_slot = packet + (i * packet_size);
        int written = 0;

        memset(&payload, 0, sizeof(payload));
        if (turbo_rtsp_h264_packetizer_next(
                &packetizer,
                payload_scratch,
                payload_scratch_size,
                &payload) != 0) {
            return -1;
        }

        memset(&header, 0, sizeof(header));
        header.version = 2;
        header.marker = payload.marker ? (marker != 0) : 0;
        header.payload_type = next_stream.payload_type;
        header.sequence_number = next_stream.sequence_number;
        header.timestamp = next_stream.timestamp;
        header.ssrc = next_stream.ssrc;
        header.payload_len = payload.payload_len;

        written = turbo_rtsp_rtp_write_packet(
            packet_slot,
            packet_size,
            &header,
            payload.payload,
            payload.payload_len);
        if (written < 0 || turbo_rtsp_rtp_sender_update(&next_stream.sender, &header) != 0) {
            return -1;
        }

        packets[i].packet = packet_slot;
        packets[i].packet_len = (size_t)written;
        ++next_stream.sequence_number;
    }

    next_stream.timestamp += timestamp_increment;
    *stream = next_stream;
    *packet_count = needed_packets;
    return 0;
}

int turbo_rtsp_rtp_stream_write_h265_nal(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *nal,
    size_t nal_len,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    uint8_t *payload_scratch,
    size_t payload_scratch_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count) {
    return turbo_rtsp_rtp_stream_write_h265_nal_ex(
        stream,
        nal,
        nal_len,
        1,
        timestamp_increment,
        max_payload,
        packet,
        packet_size,
        payload_scratch,
        payload_scratch_size,
        packets,
        packet_count_capacity,
        packet_count);
}

int turbo_rtsp_rtp_stream_write_h265_nal_ex(
    turbo_rtsp_rtp_stream_t *stream,
    const uint8_t *nal,
    size_t nal_len,
    int marker,
    uint32_t timestamp_increment,
    size_t max_payload,
    uint8_t *packet,
    size_t packet_size,
    uint8_t *payload_scratch,
    size_t payload_scratch_size,
    turbo_rtsp_rtp_stream_packet_t *packets,
    size_t packet_count_capacity,
    size_t *packet_count) {
    turbo_rtsp_h265_packetizer_t packetizer;
    turbo_rtsp_rtp_stream_t next_stream;
    size_t needed_packets = 0;
    size_t max_written_payload = 0;
    size_t i = 0;

    if (packet_count) {
        *packet_count = 0;
    }
    if (!stream || !nal || nal_len == 0 || stream->payload_type > 127 ||
        max_payload == 0 || !packet || packet_size < TURBO_RTSP_RTP_HEADER_SIZE ||
        !packets || !packet_count || packet_count_capacity == 0) {
        return -1;
    }
    if (turbo_rtsp_rtp_stream_h265_packet_count(
            nal,
            nal_len,
            max_payload,
            &needed_packets) != 0) {
        return -1;
    }
    if (needed_packets > packet_count_capacity ||
        needed_packets > SIZE_MAX / packet_size) {
        return -1;
    }
    max_written_payload = nal_len <= max_payload ? nal_len : max_payload;
    if (max_written_payload > SIZE_MAX - TURBO_RTSP_RTP_HEADER_SIZE ||
        packet_size < TURBO_RTSP_RTP_HEADER_SIZE + max_written_payload) {
        return -1;
    }
    if (nal_len > max_payload &&
        (!payload_scratch || payload_scratch_size < max_payload)) {
        return -1;
    }

    if (turbo_rtsp_h265_packetizer_init(
            &packetizer,
            nal,
            nal_len,
            max_payload) != 0) {
        return -1;
    }

    next_stream = *stream;
    for (i = 0; i < needed_packets; ++i) {
        turbo_rtsp_h265_packetized_payload_t payload;
        turbo_rtsp_rtp_header_t header;
        uint8_t *packet_slot = packet + (i * packet_size);
        int written = 0;

        memset(&payload, 0, sizeof(payload));
        if (turbo_rtsp_h265_packetizer_next(
                &packetizer,
                payload_scratch,
                payload_scratch_size,
                &payload) != 0) {
            return -1;
        }

        memset(&header, 0, sizeof(header));
        header.version = 2;
        header.marker = payload.marker ? (marker != 0) : 0;
        header.payload_type = next_stream.payload_type;
        header.sequence_number = next_stream.sequence_number;
        header.timestamp = next_stream.timestamp;
        header.ssrc = next_stream.ssrc;
        header.payload_len = payload.payload_len;

        written = turbo_rtsp_rtp_write_packet(
            packet_slot,
            packet_size,
            &header,
            payload.payload,
            payload.payload_len);
        if (written < 0 || turbo_rtsp_rtp_sender_update(&next_stream.sender, &header) != 0) {
            return -1;
        }

        packets[i].packet = packet_slot;
        packets[i].packet_len = (size_t)written;
        ++next_stream.sequence_number;
    }

    next_stream.timestamp += timestamp_increment;
    *stream = next_stream;
    *packet_count = needed_packets;
    return 0;
}

void turbo_rtsp_rtp_h264_receive_stream_init(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    uint32_t ssrc) {
    if (!stream) {
        return;
    }

    memset(stream, 0, sizeof(*stream));
    turbo_rtsp_rtp_source_init(&stream->source, ssrc);
    turbo_rtsp_h264_reassembler_init(&stream->reassembler);
}

void turbo_rtsp_rtp_h264_receive_stream_reset(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    uint32_t ssrc) {
    turbo_rtsp_rtp_h264_receive_stream_init(stream, ssrc);
}

static uint16_t turbo_rtsp_rtp_seq_forward_delta(
    uint16_t from,
    uint16_t to) {
    return (uint16_t)(to - from);
}

static void turbo_rtsp_rtp_h264_receive_stream_reorder_reset(
    turbo_rtsp_rtp_h264_receive_stream_t *stream) {
    if (!stream) {
        return;
    }
    memset(stream->reorder, 0, sizeof(stream->reorder));
    stream->reorder_count = 0;
    stream->has_next_sequence_number = 0;
    stream->next_sequence_number = 0;
}

static int turbo_rtsp_rtp_h264_receive_stream_reorder_find(
    const turbo_rtsp_rtp_h264_receive_stream_t *stream,
    uint16_t sequence_number) {
    size_t i = 0;

    for (i = 0; i < TURBO_RTSP_RTP_H264_RECEIVE_REORDER_CAPACITY; ++i) {
        if (stream->reorder[i].occupied &&
            stream->reorder[i].header.sequence_number == sequence_number) {
            return (int)i;
        }
    }
    return -1;
}

static int turbo_rtsp_rtp_h264_receive_stream_reorder_store(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    const turbo_rtsp_rtp_header_t *header) {
    size_t i = 0;

    if (!stream || !header ||
        header->payload_len > TURBO_RTSP_RTP_H264_RECEIVE_REORDER_PAYLOAD_SIZE ||
        stream->reorder_count >= TURBO_RTSP_RTP_H264_RECEIVE_REORDER_CAPACITY) {
        return -1;
    }

    for (i = 0; i < TURBO_RTSP_RTP_H264_RECEIVE_REORDER_CAPACITY; ++i) {
        if (!stream->reorder[i].occupied) {
            stream->reorder[i].header = *header;
            stream->reorder[i].payload_len = header->payload_len;
            if (header->payload_len > 0) {
                memcpy(stream->reorder[i].payload, header->payload, header->payload_len);
            }
            stream->reorder[i].header.payload = stream->reorder[i].payload;
            stream->reorder[i].occupied = 1;
            ++stream->reorder_count;
            return 0;
        }
    }
    return -1;
}

static int turbo_rtsp_rtp_h264_receive_stream_deliver(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    const turbo_rtsp_rtp_header_t *header,
    int from_reorder,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len) {
    int result = turbo_rtsp_h264_reassembler_push(
        &stream->reassembler,
        header,
        nal_buffer,
        nal_buffer_size,
        nal,
        nal_len);

    stream->has_next_sequence_number = 1;
    stream->next_sequence_number = (uint16_t)(header->sequence_number + 1u);

    if (result == TURBO_RTSP_FRAME_OK && from_reorder &&
        *nal == header->payload && *nal_len <= sizeof(stream->reorder_output_payload)) {
        memcpy(stream->reorder_output_payload, *nal, *nal_len);
        *nal = stream->reorder_output_payload;
    }
    return result;
}

static int turbo_rtsp_rtp_h264_receive_stream_remove_reorder(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    size_t index) {
    if (!stream || index >= TURBO_RTSP_RTP_H264_RECEIVE_REORDER_CAPACITY ||
        !stream->reorder[index].occupied) {
        return -1;
    }
    memset(&stream->reorder[index], 0, sizeof(stream->reorder[index]));
    --stream->reorder_count;
    return 0;
}

static int turbo_rtsp_rtp_h264_receive_stream_drain(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    const turbo_rtsp_rtp_header_t *header,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len) {
    int result = turbo_rtsp_rtp_h264_receive_stream_deliver(
        stream,
        header,
        0,
        nal_buffer,
        nal_buffer_size,
        nal,
        nal_len);

    while (result == TURBO_RTSP_FRAME_PARTIAL) {
        int index = turbo_rtsp_rtp_h264_receive_stream_reorder_find(
            stream,
            stream->next_sequence_number);
        turbo_rtsp_rtp_header_t queued_header;

        if (index < 0) {
            break;
        }

        queued_header = stream->reorder[index].header;
        queued_header.payload = stream->reorder[index].payload;
        result = turbo_rtsp_rtp_h264_receive_stream_deliver(
            stream,
            &queued_header,
            1,
            nal_buffer,
            nal_buffer_size,
            nal,
            nal_len);
        if (turbo_rtsp_rtp_h264_receive_stream_remove_reorder(
                stream,
                (size_t)index) != 0) {
            turbo_rtsp_h264_reassembler_reset(&stream->reassembler);
            turbo_rtsp_rtp_h264_receive_stream_reorder_reset(stream);
            return TURBO_RTSP_FRAME_ERROR;
        }
    }

    if (result == TURBO_RTSP_FRAME_ERROR) {
        turbo_rtsp_rtp_h264_receive_stream_reorder_reset(stream);
    }
    return result;
}

int turbo_rtsp_rtp_h264_receive_stream_drain_queued(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len) {
    int index = -1;
    turbo_rtsp_rtp_header_t queued_header;
    int result = TURBO_RTSP_FRAME_PARTIAL;

    if (nal) {
        *nal = NULL;
    }
    if (nal_len) {
        *nal_len = 0;
    }
    if (!stream || !nal || !nal_len || !stream->has_next_sequence_number) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    index = turbo_rtsp_rtp_h264_receive_stream_reorder_find(
        stream,
        stream->next_sequence_number);
    if (index < 0) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    queued_header = stream->reorder[index].header;
    queued_header.payload = stream->reorder[index].payload;
    result = turbo_rtsp_rtp_h264_receive_stream_deliver(
        stream,
        &queued_header,
        1,
        nal_buffer,
        nal_buffer_size,
        nal,
        nal_len);
    if (turbo_rtsp_rtp_h264_receive_stream_remove_reorder(
            stream,
            (size_t)index) != 0) {
        turbo_rtsp_h264_reassembler_reset(&stream->reassembler);
        turbo_rtsp_rtp_h264_receive_stream_reorder_reset(stream);
        return TURBO_RTSP_FRAME_ERROR;
    }
    if (result == TURBO_RTSP_FRAME_ERROR) {
        turbo_rtsp_rtp_h264_receive_stream_reorder_reset(stream);
    }
    return result;
}

static int turbo_rtsp_rtp_h264_receive_stream_accept(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    const turbo_rtsp_rtp_header_t *header,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len) {
    uint16_t sequence_delta = 0;

    if (!stream->has_next_sequence_number) {
        stream->has_next_sequence_number = 1;
        stream->next_sequence_number = header->sequence_number;
    }

    sequence_delta = turbo_rtsp_rtp_seq_forward_delta(
        stream->next_sequence_number,
        header->sequence_number);
    if (sequence_delta == 0) {
        return turbo_rtsp_rtp_h264_receive_stream_drain(
            stream,
            header,
            nal_buffer,
            nal_buffer_size,
            nal,
            nal_len);
    }

    if (sequence_delta <= TURBO_RTSP_RTP_H264_RECEIVE_REORDER_WINDOW &&
        turbo_rtsp_rtp_h264_receive_stream_reorder_find(
            stream,
            header->sequence_number) < 0 &&
        turbo_rtsp_rtp_h264_receive_stream_reorder_store(stream, header) == 0) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    turbo_rtsp_h264_reassembler_reset(&stream->reassembler);
    turbo_rtsp_rtp_h264_receive_stream_reorder_reset(stream);
    return TURBO_RTSP_FRAME_ERROR;
}

int turbo_rtsp_rtp_h264_receive_stream_push(
    turbo_rtsp_rtp_h264_receive_stream_t *stream,
    const uint8_t *packet,
    size_t packet_len,
    uint32_t arrival_rtp_timestamp,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len,
    turbo_rtsp_rtp_source_update_result_t *source_result) {
    turbo_rtsp_rtp_header_t header;
    size_t header_len = 0;
    turbo_rtsp_rtp_source_update_result_t update_result =
        TURBO_RTSP_RTP_SOURCE_UPDATE_ERROR;

    if (nal) {
        *nal = NULL;
    }
    if (nal_len) {
        *nal_len = 0;
    }
    if (source_result) {
        *source_result = update_result;
    }

    if (!stream || !packet || !nal || !nal_len) {
        return TURBO_RTSP_FRAME_ERROR;
    }

    if (turbo_rtsp_rtp_parse_header(packet, packet_len, &header, &header_len) != 0) {
        turbo_rtsp_h264_reassembler_reset(&stream->reassembler);
        turbo_rtsp_rtp_h264_receive_stream_reorder_reset(stream);
        return TURBO_RTSP_FRAME_ERROR;
    }

    update_result = turbo_rtsp_rtp_source_update_ex(
        &stream->source,
        &header,
        arrival_rtp_timestamp);
    if (source_result) {
        *source_result = update_result;
    }

    if (update_result == TURBO_RTSP_RTP_SOURCE_UPDATE_DUPLICATE) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    if (update_result == TURBO_RTSP_RTP_SOURCE_UPDATE_LATE_OR_OUT_OF_ORDER) {
        if (stream->has_next_sequence_number &&
            header.sequence_number == stream->next_sequence_number) {
            return turbo_rtsp_rtp_h264_receive_stream_accept(
                stream,
                &header,
                nal_buffer,
                nal_buffer_size,
                nal,
                nal_len);
        }
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    if (update_result != TURBO_RTSP_RTP_SOURCE_UPDATE_OK) {
        turbo_rtsp_h264_reassembler_reset(&stream->reassembler);
        turbo_rtsp_rtp_h264_receive_stream_reorder_reset(stream);
        return TURBO_RTSP_FRAME_ERROR;
    }

    return turbo_rtsp_rtp_h264_receive_stream_accept(
        stream,
        &header,
        nal_buffer,
        nal_buffer_size,
        nal,
        nal_len);
}

void turbo_rtsp_rtp_h265_receive_stream_init(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    uint32_t ssrc) {
    if (!stream) {
        return;
    }

    memset(stream, 0, sizeof(*stream));
    turbo_rtsp_rtp_source_init(&stream->source, ssrc);
    turbo_rtsp_h265_reassembler_init(&stream->reassembler);
}

void turbo_rtsp_rtp_h265_receive_stream_reset(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    uint32_t ssrc) {
    turbo_rtsp_rtp_h265_receive_stream_init(stream, ssrc);
}

static void turbo_rtsp_rtp_h265_receive_stream_reorder_reset(
    turbo_rtsp_rtp_h265_receive_stream_t *stream) {
    if (!stream) {
        return;
    }
    memset(stream->reorder, 0, sizeof(stream->reorder));
    stream->reorder_count = 0;
    stream->has_next_sequence_number = 0;
    stream->next_sequence_number = 0;
}

static int turbo_rtsp_rtp_h265_receive_stream_reorder_find(
    const turbo_rtsp_rtp_h265_receive_stream_t *stream,
    uint16_t sequence_number) {
    size_t i = 0;

    for (i = 0; i < TURBO_RTSP_RTP_H265_RECEIVE_REORDER_CAPACITY; ++i) {
        if (stream->reorder[i].occupied &&
            stream->reorder[i].header.sequence_number == sequence_number) {
            return (int)i;
        }
    }
    return -1;
}

static int turbo_rtsp_rtp_h265_receive_stream_reorder_store(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    const turbo_rtsp_rtp_header_t *header) {
    size_t i = 0;

    if (!stream || !header ||
        header->payload_len > TURBO_RTSP_RTP_H265_RECEIVE_REORDER_PAYLOAD_SIZE ||
        stream->reorder_count >= TURBO_RTSP_RTP_H265_RECEIVE_REORDER_CAPACITY) {
        return -1;
    }

    for (i = 0; i < TURBO_RTSP_RTP_H265_RECEIVE_REORDER_CAPACITY; ++i) {
        if (!stream->reorder[i].occupied) {
            stream->reorder[i].header = *header;
            stream->reorder[i].payload_len = header->payload_len;
            if (header->payload_len > 0) {
                memcpy(stream->reorder[i].payload, header->payload, header->payload_len);
            }
            stream->reorder[i].header.payload = stream->reorder[i].payload;
            stream->reorder[i].occupied = 1;
            ++stream->reorder_count;
            return 0;
        }
    }
    return -1;
}

static int turbo_rtsp_rtp_h265_receive_stream_deliver(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    const turbo_rtsp_rtp_header_t *header,
    int from_reorder,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len) {
    int result = turbo_rtsp_h265_reassembler_push(
        &stream->reassembler,
        header,
        nal_buffer,
        nal_buffer_size,
        nal,
        nal_len);

    stream->has_next_sequence_number = 1;
    stream->next_sequence_number = (uint16_t)(header->sequence_number + 1u);

    if (result == TURBO_RTSP_FRAME_OK && from_reorder &&
        *nal == header->payload && *nal_len <= sizeof(stream->reorder_output_payload)) {
        memcpy(stream->reorder_output_payload, *nal, *nal_len);
        *nal = stream->reorder_output_payload;
    }
    return result;
}

static int turbo_rtsp_rtp_h265_receive_stream_remove_reorder(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    size_t index) {
    if (!stream || index >= TURBO_RTSP_RTP_H265_RECEIVE_REORDER_CAPACITY ||
        !stream->reorder[index].occupied) {
        return -1;
    }
    memset(&stream->reorder[index], 0, sizeof(stream->reorder[index]));
    --stream->reorder_count;
    return 0;
}

static int turbo_rtsp_rtp_h265_receive_stream_drain(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    const turbo_rtsp_rtp_header_t *header,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len) {
    int result = turbo_rtsp_rtp_h265_receive_stream_deliver(
        stream,
        header,
        0,
        nal_buffer,
        nal_buffer_size,
        nal,
        nal_len);

    while (result == TURBO_RTSP_FRAME_PARTIAL) {
        int index = turbo_rtsp_rtp_h265_receive_stream_reorder_find(
            stream,
            stream->next_sequence_number);
        turbo_rtsp_rtp_header_t queued_header;

        if (index < 0) {
            break;
        }

        queued_header = stream->reorder[index].header;
        queued_header.payload = stream->reorder[index].payload;
        result = turbo_rtsp_rtp_h265_receive_stream_deliver(
            stream,
            &queued_header,
            1,
            nal_buffer,
            nal_buffer_size,
            nal,
            nal_len);
        if (turbo_rtsp_rtp_h265_receive_stream_remove_reorder(
                stream,
                (size_t)index) != 0) {
            turbo_rtsp_h265_reassembler_reset(&stream->reassembler);
            turbo_rtsp_rtp_h265_receive_stream_reorder_reset(stream);
            return TURBO_RTSP_FRAME_ERROR;
        }
    }

    if (result == TURBO_RTSP_FRAME_ERROR) {
        turbo_rtsp_rtp_h265_receive_stream_reorder_reset(stream);
    }
    return result;
}

int turbo_rtsp_rtp_h265_receive_stream_drain_queued(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len) {
    int index = -1;
    turbo_rtsp_rtp_header_t queued_header;
    int result = TURBO_RTSP_FRAME_PARTIAL;

    if (nal) {
        *nal = NULL;
    }
    if (nal_len) {
        *nal_len = 0;
    }
    if (!stream || !nal || !nal_len || !stream->has_next_sequence_number) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    index = turbo_rtsp_rtp_h265_receive_stream_reorder_find(
        stream,
        stream->next_sequence_number);
    if (index < 0) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    queued_header = stream->reorder[index].header;
    queued_header.payload = stream->reorder[index].payload;
    result = turbo_rtsp_rtp_h265_receive_stream_deliver(
        stream,
        &queued_header,
        1,
        nal_buffer,
        nal_buffer_size,
        nal,
        nal_len);
    if (turbo_rtsp_rtp_h265_receive_stream_remove_reorder(
            stream,
            (size_t)index) != 0) {
        turbo_rtsp_h265_reassembler_reset(&stream->reassembler);
        turbo_rtsp_rtp_h265_receive_stream_reorder_reset(stream);
        return TURBO_RTSP_FRAME_ERROR;
    }
    if (result == TURBO_RTSP_FRAME_ERROR) {
        turbo_rtsp_rtp_h265_receive_stream_reorder_reset(stream);
    }
    return result;
}

static int turbo_rtsp_rtp_h265_receive_stream_accept(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    const turbo_rtsp_rtp_header_t *header,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len) {
    uint16_t sequence_delta = 0;

    if (!stream->has_next_sequence_number) {
        stream->has_next_sequence_number = 1;
        stream->next_sequence_number = header->sequence_number;
    }

    sequence_delta = turbo_rtsp_rtp_seq_forward_delta(
        stream->next_sequence_number,
        header->sequence_number);
    if (sequence_delta == 0) {
        return turbo_rtsp_rtp_h265_receive_stream_drain(
            stream,
            header,
            nal_buffer,
            nal_buffer_size,
            nal,
            nal_len);
    }

    if (sequence_delta <= TURBO_RTSP_RTP_H265_RECEIVE_REORDER_WINDOW &&
        turbo_rtsp_rtp_h265_receive_stream_reorder_find(
            stream,
            header->sequence_number) < 0 &&
        turbo_rtsp_rtp_h265_receive_stream_reorder_store(stream, header) == 0) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    turbo_rtsp_h265_reassembler_reset(&stream->reassembler);
    turbo_rtsp_rtp_h265_receive_stream_reorder_reset(stream);
    return TURBO_RTSP_FRAME_ERROR;
}

int turbo_rtsp_rtp_h265_receive_stream_push(
    turbo_rtsp_rtp_h265_receive_stream_t *stream,
    const uint8_t *packet,
    size_t packet_len,
    uint32_t arrival_rtp_timestamp,
    uint8_t *nal_buffer,
    size_t nal_buffer_size,
    const uint8_t **nal,
    size_t *nal_len,
    turbo_rtsp_rtp_source_update_result_t *source_result) {
    turbo_rtsp_rtp_header_t header;
    size_t header_len = 0;
    turbo_rtsp_rtp_source_update_result_t update_result =
        TURBO_RTSP_RTP_SOURCE_UPDATE_ERROR;

    if (nal) {
        *nal = NULL;
    }
    if (nal_len) {
        *nal_len = 0;
    }
    if (source_result) {
        *source_result = update_result;
    }

    if (!stream || !packet || !nal || !nal_len) {
        return TURBO_RTSP_FRAME_ERROR;
    }

    if (turbo_rtsp_rtp_parse_header(packet, packet_len, &header, &header_len) != 0) {
        turbo_rtsp_h265_reassembler_reset(&stream->reassembler);
        turbo_rtsp_rtp_h265_receive_stream_reorder_reset(stream);
        return TURBO_RTSP_FRAME_ERROR;
    }

    update_result = turbo_rtsp_rtp_source_update_ex(
        &stream->source,
        &header,
        arrival_rtp_timestamp);
    if (source_result) {
        *source_result = update_result;
    }

    if (update_result == TURBO_RTSP_RTP_SOURCE_UPDATE_DUPLICATE) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    if (update_result == TURBO_RTSP_RTP_SOURCE_UPDATE_LATE_OR_OUT_OF_ORDER) {
        if (stream->has_next_sequence_number &&
            header.sequence_number == stream->next_sequence_number) {
            return turbo_rtsp_rtp_h265_receive_stream_accept(
                stream,
                &header,
                nal_buffer,
                nal_buffer_size,
                nal,
                nal_len);
        }
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    if (update_result != TURBO_RTSP_RTP_SOURCE_UPDATE_OK) {
        turbo_rtsp_h265_reassembler_reset(&stream->reassembler);
        turbo_rtsp_rtp_h265_receive_stream_reorder_reset(stream);
        return TURBO_RTSP_FRAME_ERROR;
    }

    return turbo_rtsp_rtp_h265_receive_stream_accept(
        stream,
        &header,
        nal_buffer,
        nal_buffer_size,
        nal,
        nal_len);
}

#define TURBO_RTSP_RTP_UDP_DEFAULT_TIMEOUT_MS 30000u
#define TURBO_RTSP_RTP_UDP_DEFAULT_SEND_CAPACITY 16u

typedef struct {
    cnet_datagram socket;
    cnet_datagram_peer peer;
    uint8_t *receive_buffer;
    size_t receive_capacity;
    size_t *receive_size;
    int receive_done;
    int receive_status;
    uint64_t next_send_tag;
    uint64_t waiting_send_tag;
    int send_done;
    int send_status;
    int initialized;
} turbo_rtsp_rtp_udp_channel_t;

struct turbo_rtsp_rtp_udp_pair_s {
    turbo_rtsp_rtp_udp_channel_t rtp;
    turbo_rtsp_rtp_udp_channel_t rtcp;
    uint64_t timeout_ms;
    int family;
    int has_peer;
};

static int turbo_rtsp_resolve_udp_addr(
    const char *host,
    int port,
    int family,
    struct sockaddr_storage *addr) {
    char port_buf[16];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    int rc = 0;

    if (!host || port < 0 || port > 65535 || !addr) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family ? family : AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    rc = getaddrinfo(host, port_buf, &hints, &result);
    if (rc != 0 || !result || !result->ai_addr ||
        result->ai_addrlen > sizeof(*addr)) {
        if (result) {
            freeaddrinfo(result);
        }
        return -1;
    }

    memset(addr, 0, sizeof(*addr));
    memcpy(addr, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    return 0;
}

static native_io_backend_kind turbo_rtsp_native_backend(void) {
#ifdef _WIN32
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_IO_URING;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static int turbo_rtsp_sockaddr_numeric_host(
    const struct sockaddr_storage *addr,
    char *host,
    size_t host_size) {
    const void *source = NULL;

    if (!addr || !host || host_size == 0) {
        return -1;
    }
    if (addr->ss_family == AF_INET) {
        source = &((const struct sockaddr_in *)addr)->sin_addr;
    } else if (addr->ss_family == AF_INET6) {
        source = &((const struct sockaddr_in6 *)addr)->sin6_addr;
    } else {
        return -1;
    }
    return inet_ntop(addr->ss_family, source, host, (socklen_t)host_size) ? 0 : -1;
}

static int turbo_rtsp_sockaddr_to_peer(
    const struct sockaddr_storage *addr,
    cnet_datagram_peer *peer) {
    if (!addr || !peer) {
        return -1;
    }

    memset(peer, 0, sizeof(*peer));
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)addr;
        peer->family = CNET_DATAGRAM_ADDRESS_IPV4;
        peer->port = ntohs(in->sin_port);
        memcpy(peer->address, &in->sin_addr, sizeof(in->sin_addr));
        return 0;
    }
    if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)addr;
        peer->family = CNET_DATAGRAM_ADDRESS_IPV6;
        peer->port = ntohs(in6->sin6_port);
        peer->scope_id = in6->sin6_scope_id;
        memcpy(peer->address, &in6->sin6_addr, sizeof(in6->sin6_addr));
        return 0;
    }
    return -1;
}

static void turbo_rtsp_rtp_udp_receive(
    void *user,
    cnet_datagram *datagram,
    const cnet_datagram_peer *peer,
    const cnet_receive_view *view) {
    turbo_rtsp_rtp_udp_channel_t *channel =
        (turbo_rtsp_rtp_udp_channel_t *)user;
    (void)datagram;
    (void)peer;

    if (!channel || channel->receive_done || !channel->receive_size || !view) {
        return;
    }
    *channel->receive_size = view->size;
    if (view->size > channel->receive_capacity) {
        channel->receive_status = SALTS_EMSGSIZE;
    } else {
        if (view->size > 0) {
            memcpy(channel->receive_buffer, view->data, view->size);
        }
        channel->receive_status = SALTS_OK;
    }
    channel->receive_done = 1;
}

static void turbo_rtsp_rtp_udp_send_complete(
    void *user,
    cnet_datagram *datagram,
    const cnet_datagram_peer *peer,
    size_t size,
    int status,
    uint64_t tag) {
    turbo_rtsp_rtp_udp_channel_t *channel =
        (turbo_rtsp_rtp_udp_channel_t *)user;
    (void)datagram;
    (void)peer;
    (void)size;

    if (channel && tag == channel->waiting_send_tag) {
        channel->send_status = status;
        channel->send_done = 1;
    }
}

static int turbo_rtsp_rtp_udp_channel_init(
    turbo_rtsp_rtp_udp_channel_t *channel,
    const char *host,
    uint16_t port,
    size_t send_capacity,
    size_t max_datagram_bytes) {
    cnet_datagram_config config = CNET_DATAGRAM_CONFIG_INIT;

    if (!channel || !host || send_capacity == 0 ||
        send_capacity == SIZE_MAX || max_datagram_bytes == 0 ||
        max_datagram_bytes > CNET_DATAGRAM_MAX_PAYLOAD_BYTES) {
        return -1;
    }
    config.backend = turbo_rtsp_native_backend();
    config.host = host;
    config.port = port;
    config.send_capacity = send_capacity;
    config.request_capacity = send_capacity + 1u;
    config.completion_batch_capacity = config.request_capacity;
    config.max_datagram_bytes = max_datagram_bytes;
    config.receive_buffer_bytes = max_datagram_bytes;
    config.observer.on_receive = turbo_rtsp_rtp_udp_receive;
    config.observer.on_send = turbo_rtsp_rtp_udp_send_complete;
    config.observer.user = channel;

    if (cnet_datagram_init(&channel->socket, &config) != SALTS_OK) {
        return -1;
    }
    channel->initialized = 1;
    return 0;
}

turbo_rtsp_rtp_udp_pair_t *turbo_rtsp_rtp_udp_pair_create(
    const turbo_rtsp_rtp_udp_pair_config_t *config) {
    turbo_rtsp_rtp_udp_pair_t *pair = NULL;
    struct sockaddr_storage local_rtp;
    const char *local_host = NULL;
    int local_rtp_port = 0;
    int local_rtcp_port = 0;
    uint64_t timeout_ms = 0;
    size_t send_capacity = 0;
    size_t max_datagram_bytes = 0;

    local_host = (config && config->local_host) ? config->local_host : "0.0.0.0";
    local_rtp_port = (config && config->local_rtp_port > 0) ? config->local_rtp_port : 0;
    if (config && config->local_rtcp_port > 0) {
        local_rtcp_port = config->local_rtcp_port;
    } else {
        local_rtcp_port = local_rtp_port > 0 ? local_rtp_port + 1 : 0;
    }
    timeout_ms = (config && config->timeout_ms > 0)
                     ? config->timeout_ms
                     : TURBO_RTSP_RTP_UDP_DEFAULT_TIMEOUT_MS;
    send_capacity = (config && config->send_capacity > 0)
                        ? config->send_capacity
                        : TURBO_RTSP_RTP_UDP_DEFAULT_SEND_CAPACITY;
    max_datagram_bytes = (config && config->max_datagram_bytes > 0)
                             ? config->max_datagram_bytes
                             : CNET_DATAGRAM_MAX_PAYLOAD_BYTES;

    if (local_rtcp_port > 65535 || timeout_ms > UINT32_MAX ||
        send_capacity == SIZE_MAX || max_datagram_bytes == 0 ||
        max_datagram_bytes > CNET_DATAGRAM_MAX_PAYLOAD_BYTES) {
        return NULL;
    }

    pair = (turbo_rtsp_rtp_udp_pair_t *)calloc(1, sizeof(*pair));
    if (!pair) {
        return NULL;
    }

    pair->timeout_ms = timeout_ms;
    if (turbo_rtsp_rtp_udp_channel_init(
            &pair->rtp,
            local_host,
            (uint16_t)local_rtp_port,
            send_capacity,
            max_datagram_bytes) != 0 ||
        turbo_rtsp_resolve_udp_addr(
            local_host, local_rtp_port, 0, &local_rtp) != 0) {
        turbo_rtsp_rtp_udp_pair_destroy(pair);
        return NULL;
    }
    pair->family = local_rtp.ss_family;
    if (turbo_rtsp_rtp_udp_channel_init(
            &pair->rtcp,
            local_host,
            (uint16_t)local_rtcp_port,
            send_capacity,
            max_datagram_bytes) != 0) {
        turbo_rtsp_rtp_udp_pair_destroy(pair);
        return NULL;
    }

    return pair;
}

void turbo_rtsp_rtp_udp_pair_destroy(turbo_rtsp_rtp_udp_pair_t *pair) {
    if (!pair) {
        return;
    }

    if (pair->rtp.initialized) {
        (void)cnet_datagram_stop(&pair->rtp.socket, (uint32_t)pair->timeout_ms);
        (void)cnet_datagram_destroy(&pair->rtp.socket);
    }
    if (pair->rtcp.initialized) {
        (void)cnet_datagram_stop(&pair->rtcp.socket, (uint32_t)pair->timeout_ms);
        (void)cnet_datagram_destroy(&pair->rtcp.socket);
    }
    free(pair);
}

int turbo_rtsp_rtp_udp_pair_get_local_ports(
    turbo_rtsp_rtp_udp_pair_t *pair,
    int *rtp_port,
    int *rtcp_port) {
    uint16_t local_rtp_port = 0;
    uint16_t local_rtcp_port = 0;

    if (!pair || !rtp_port || !rtcp_port ||
        !pair->rtp.initialized || !pair->rtcp.initialized) {
        return -1;
    }

    if (cnet_datagram_port(&pair->rtp.socket, &local_rtp_port) != SALTS_OK ||
        cnet_datagram_port(&pair->rtcp.socket, &local_rtcp_port) != SALTS_OK) {
        return -1;
    }
    *rtp_port = (int)local_rtp_port;
    *rtcp_port = (int)local_rtcp_port;

    return (*rtp_port > 0 && *rtcp_port > 0) ? 0 : -1;
}

int turbo_rtsp_rtp_udp_pair_set_peer(
    turbo_rtsp_rtp_udp_pair_t *pair,
    const char *host,
    int rtp_port,
    int rtcp_port) {
    int effective_rtcp_port = 0;

    if (!pair || !host || rtp_port <= 0 || rtp_port > 65535) {
        return -1;
    }

    effective_rtcp_port = rtcp_port > 0 ? rtcp_port : rtp_port + 1;
    if (effective_rtcp_port <= 0 || effective_rtcp_port > 65535) {
        return -1;
    }

    {
        struct sockaddr_storage peer_rtp;
        struct sockaddr_storage peer_rtcp;
        if (turbo_rtsp_resolve_udp_addr(host, rtp_port, pair->family, &peer_rtp) != 0 ||
        turbo_rtsp_resolve_udp_addr(
            host,
            effective_rtcp_port,
            pair->family,
            &peer_rtcp) != 0 ||
            turbo_rtsp_sockaddr_to_peer(&peer_rtp, &pair->rtp.peer) != 0 ||
            turbo_rtsp_sockaddr_to_peer(&peer_rtcp, &pair->rtcp.peer) != 0) {
            return -1;
        }
    }

    pair->has_peer = 1;
    return 0;
}

static int turbo_rtsp_rtp_udp_pair_send(
    turbo_rtsp_rtp_udp_channel_t *channel,
    uint64_t timeout_ms,
    const uint8_t *packet,
    size_t packet_len) {
    const uint64_t started_ms = salts_monotonic_ms();
    uint64_t tag;
    int status;

    if (!channel || !channel->initialized || !packet || packet_len == 0) {
        return -1;
    }

    tag = ++channel->next_send_tag;
    if (tag == 0) {
        tag = ++channel->next_send_tag;
    }
    channel->waiting_send_tag = tag;
    channel->send_done = 0;
    channel->send_status = SALTS_EIO;
    status = cnet_datagram_send(
        &channel->socket, &channel->peer, packet, packet_len, tag);
    while (status == SALTS_OK && !channel->send_done) {
        const uint64_t elapsed_ms = salts_monotonic_ms() - started_ms;
        size_t events = 0;
        uint32_t wait_ms;
        if (elapsed_ms >= timeout_ms) {
            return -1;
        }
        wait_ms = (uint32_t)((timeout_ms - elapsed_ms) > UINT32_MAX
                                 ? UINT32_MAX
                                 : (timeout_ms - elapsed_ms));
        status = cnet_datagram_poll(&channel->socket, wait_ms, &events);
    }
    return status == SALTS_OK && channel->send_status == SALTS_OK ? 0 : -1;
}

int turbo_rtsp_rtp_udp_pair_send_rtp(
    turbo_rtsp_rtp_udp_pair_t *pair,
    const uint8_t *packet,
    size_t packet_len) {
    if (!pair || !pair->has_peer) {
        return -1;
    }
    return turbo_rtsp_rtp_udp_pair_send(
        &pair->rtp,
        pair->timeout_ms,
        packet,
        packet_len);
}

int turbo_rtsp_rtp_udp_pair_send_rtcp(
    turbo_rtsp_rtp_udp_pair_t *pair,
    const uint8_t *packet,
    size_t packet_len) {
    if (!pair || !pair->has_peer) {
        return -1;
    }
    return turbo_rtsp_rtp_udp_pair_send(
        &pair->rtcp,
        pair->timeout_ms,
        packet,
        packet_len);
}

static int turbo_rtsp_rtp_udp_pair_recv(
    turbo_rtsp_rtp_udp_channel_t *channel,
    uint64_t timeout_ms,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *packet_len) {
    const uint64_t started_ms = salts_monotonic_ms();
    int status;

    if (!channel || !channel->initialized || !buffer || buffer_size == 0 || !packet_len) {
        return -1;
    }

    channel->receive_buffer = buffer;
    channel->receive_capacity = buffer_size;
    channel->receive_size = packet_len;
    channel->receive_done = 0;
    channel->receive_status = SALTS_EIO;
    status = cnet_datagram_receive(&channel->socket, 1u);
    while (status == SALTS_OK && !channel->receive_done) {
        const uint64_t elapsed_ms = salts_monotonic_ms() - started_ms;
        size_t events = 0;
        uint32_t wait_ms;
        if (elapsed_ms >= timeout_ms) {
            status = SALTS_ETIMEDOUT;
            break;
        }
        wait_ms = (uint32_t)((timeout_ms - elapsed_ms) > UINT32_MAX
                                 ? UINT32_MAX
                                 : (timeout_ms - elapsed_ms));
        status = cnet_datagram_poll(&channel->socket, wait_ms, &events);
    }
    channel->receive_buffer = NULL;
    channel->receive_capacity = 0;
    channel->receive_size = NULL;
    return status == SALTS_OK && channel->receive_status == SALTS_OK ? 0 : -1;
}

int turbo_rtsp_rtp_udp_pair_recv_rtp(
    turbo_rtsp_rtp_udp_pair_t *pair,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *packet_len) {
    return pair
               ? turbo_rtsp_rtp_udp_pair_recv(
                     &pair->rtp,
                     pair->timeout_ms,
                     buffer,
                     buffer_size,
                     packet_len)
               : -1;
}

int turbo_rtsp_rtp_udp_pair_recv_rtcp(
    turbo_rtsp_rtp_udp_pair_t *pair,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *packet_len) {
    return pair
               ? turbo_rtsp_rtp_udp_pair_recv(
                     &pair->rtcp,
                     pair->timeout_ms,
                     buffer,
                     buffer_size,
                     packet_len)
               : -1;
}

int turbo_rtsp_interleaved_parse(
    const uint8_t *buffer,
    size_t buffer_len,
    turbo_rtsp_interleaved_frame_t *frame,
    size_t *consumed) {
    uint16_t payload_len = 0;

    if (!buffer || !frame || !consumed) {
        return TURBO_RTSP_FRAME_ERROR;
    }

    *consumed = 0;
    memset(frame, 0, sizeof(*frame));

    if (buffer_len < TURBO_RTSP_INTERLEAVED_HEADER_SIZE) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }
    if (buffer[0] != '$') {
        return TURBO_RTSP_FRAME_ERROR;
    }

    payload_len = turbo_rtsp_read_u16be(buffer + 2);
    if (buffer_len < TURBO_RTSP_INTERLEAVED_HEADER_SIZE + (size_t)payload_len) {
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    frame->channel = buffer[1];
    frame->payload_len = payload_len;
    frame->payload = buffer + TURBO_RTSP_INTERLEAVED_HEADER_SIZE;
    *consumed = TURBO_RTSP_INTERLEAVED_HEADER_SIZE + (size_t)payload_len;
    return TURBO_RTSP_FRAME_OK;
}

void turbo_rtsp_interleaved_parser_reset(
    turbo_rtsp_interleaved_parser_t *parser) {
    uint8_t *payload_buffer = NULL;
    size_t payload_buffer_size = 0;

    if (!parser) {
        return;
    }

    payload_buffer = parser->payload_buffer;
    payload_buffer_size = parser->payload_buffer_size;
    memset(parser, 0, sizeof(*parser));
    parser->payload_buffer = payload_buffer;
    parser->payload_buffer_size = payload_buffer_size;
}

void turbo_rtsp_interleaved_parser_init(
    turbo_rtsp_interleaved_parser_t *parser) {
    if (!parser) {
        return;
    }
    memset(parser, 0, sizeof(*parser));
}

void turbo_rtsp_interleaved_parser_destroy(
    turbo_rtsp_interleaved_parser_t *parser) {
    if (!parser) {
        return;
    }
    free(parser->payload_buffer);
    memset(parser, 0, sizeof(*parser));
}

static size_t turbo_rtsp_interleaved_take(size_t available, size_t needed) {
    return available < needed ? available : needed;
}

static void turbo_rtsp_interleaved_set_header(
    turbo_rtsp_interleaved_parser_t *parser) {
    parser->channel = parser->header[1];
    parser->payload_len = turbo_rtsp_read_u16be(parser->header + 2);
    parser->payload_remaining = parser->payload_len;
    parser->needed = parser->payload_remaining;
}

int turbo_rtsp_interleaved_parser_parse(
    turbo_rtsp_interleaved_parser_t *parser,
    const uint8_t *buffer,
    size_t buffer_len,
    turbo_rtsp_interleaved_frame_t *frame,
    size_t *consumed) {
    size_t offset = 0;

    if (!parser || !frame || !consumed || (!buffer && buffer_len > 0)) {
        return TURBO_RTSP_FRAME_ERROR;
    }

    *consumed = 0;
    memset(frame, 0, sizeof(*frame));

    if (parser->discarding_payload) {
        size_t skipped = turbo_rtsp_interleaved_take(buffer_len, parser->payload_remaining);

        parser->payload_remaining -= skipped;
        parser->needed = parser->payload_remaining;
        *consumed = skipped;
        if (parser->payload_remaining == 0) {
            turbo_rtsp_interleaved_parser_reset(parser);
        }
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    if (parser->header_len == 0) {
        while (offset < buffer_len && buffer[offset] != '$') {
            ++offset;
        }
        if (offset == buffer_len) {
            *consumed = offset;
            parser->needed = 1;
            return TURBO_RTSP_FRAME_PARTIAL;
        }
    }

    while (parser->header_len < TURBO_RTSP_INTERLEAVED_HEADER_SIZE &&
           offset < buffer_len) {
        parser->header[parser->header_len++] = buffer[offset++];
    }
    if (parser->header_len < TURBO_RTSP_INTERLEAVED_HEADER_SIZE) {
        *consumed = offset;
        parser->needed = TURBO_RTSP_INTERLEAVED_HEADER_SIZE - parser->header_len;
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    if (parser->payload_remaining == 0 && parser->payload_len == 0) {
        turbo_rtsp_interleaved_set_header(parser);
    }

    if (parser->payload_remaining == 0) {
        frame->channel = parser->channel;
        frame->payload_len = parser->payload_len;
        frame->payload = buffer + offset;
        *consumed = offset;
        turbo_rtsp_interleaved_parser_reset(parser);
        return TURBO_RTSP_FRAME_OK;
    }

    if (buffer_len - offset >= parser->payload_remaining) {
        frame->channel = parser->channel;
        frame->payload_len = parser->payload_len;
        frame->payload = buffer + offset;
        *consumed = offset + parser->payload_remaining;
        turbo_rtsp_interleaved_parser_reset(parser);
        return TURBO_RTSP_FRAME_OK;
    }

    if (buffer_len > offset) {
        parser->discarding_payload = 1;
        parser->payload_remaining -= buffer_len - offset;
    }
    parser->needed = parser->payload_remaining;
    *consumed = buffer_len;
    return TURBO_RTSP_FRAME_PARTIAL;
}

static int turbo_rtsp_interleaved_parser_ensure_payload(
    turbo_rtsp_interleaved_parser_t *parser,
    size_t payload_len) {
    uint8_t *payload_buffer = NULL;

    if (payload_len == 0 || parser->payload_buffer_size >= payload_len) {
        return 0;
    }

    payload_buffer = (uint8_t *)realloc(parser->payload_buffer, payload_len);
    if (!payload_buffer) {
        return -1;
    }
    parser->payload_buffer = payload_buffer;
    parser->payload_buffer_size = payload_len;
    return 0;
}

static int turbo_rtsp_interleaved_parser_deliver_copy(
    turbo_rtsp_interleaved_parser_t *parser,
    turbo_rtsp_interleaved_frame_t *frame,
    uint8_t *payload,
    size_t payload_capacity) {
    frame->channel = parser->channel;
    frame->payload_len = parser->payload_len;
    frame->payload = NULL;

    if ((size_t)parser->payload_len > payload_capacity) {
        parser->needed = parser->payload_len;
        return TURBO_RTSP_FRAME_ERROR;
    }

    if (parser->payload_len > 0) {
        memcpy(payload, parser->payload_buffer, parser->payload_len);
        frame->payload = payload;
    }
    turbo_rtsp_interleaved_parser_reset(parser);
    return TURBO_RTSP_FRAME_OK;
}

int turbo_rtsp_interleaved_parser_parse_copy(
    turbo_rtsp_interleaved_parser_t *parser,
    const uint8_t *buffer,
    size_t buffer_len,
    turbo_rtsp_interleaved_frame_t *frame,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *consumed) {
    size_t offset = 0;

    if (!parser || !frame || !consumed || (!buffer && buffer_len > 0) ||
        (!payload && payload_capacity > 0)) {
        return TURBO_RTSP_FRAME_ERROR;
    }

    *consumed = 0;
    memset(frame, 0, sizeof(*frame));

    if (parser->frame_pending) {
        return turbo_rtsp_interleaved_parser_deliver_copy(
            parser,
            frame,
            payload,
            payload_capacity);
    }

    if (parser->discarding_payload) {
        size_t skipped = turbo_rtsp_interleaved_take(buffer_len, parser->payload_remaining);

        parser->payload_remaining -= skipped;
        parser->needed = parser->payload_remaining;
        *consumed = skipped;
        if (parser->payload_remaining == 0) {
            turbo_rtsp_interleaved_parser_reset(parser);
        }
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    if (parser->header_len == 0) {
        while (offset < buffer_len && buffer[offset] != '$') {
            ++offset;
        }
        if (offset == buffer_len) {
            *consumed = offset;
            parser->needed = 1;
            return TURBO_RTSP_FRAME_PARTIAL;
        }
    }

    while (parser->header_len < TURBO_RTSP_INTERLEAVED_HEADER_SIZE &&
           offset < buffer_len) {
        parser->header[parser->header_len++] = buffer[offset++];
    }
    if (parser->header_len < TURBO_RTSP_INTERLEAVED_HEADER_SIZE) {
        *consumed = offset;
        parser->needed = TURBO_RTSP_INTERLEAVED_HEADER_SIZE - parser->header_len;
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    if (parser->payload_remaining == 0 && parser->payload_len == 0 &&
        parser->payload_copied == 0) {
        turbo_rtsp_interleaved_set_header(parser);
        if (turbo_rtsp_interleaved_parser_ensure_payload(
                parser,
                parser->payload_len) != 0) {
            turbo_rtsp_interleaved_parser_reset(parser);
            return TURBO_RTSP_FRAME_ERROR;
        }
    }

    if (parser->payload_remaining > 0) {
        size_t copied = turbo_rtsp_interleaved_take(
            buffer_len - offset,
            parser->payload_remaining);

        if (copied > 0) {
            memcpy(parser->payload_buffer + parser->payload_copied, buffer + offset, copied);
            parser->payload_copied += copied;
            parser->payload_remaining -= copied;
            offset += copied;
        }
    }

    *consumed = offset;
    if (parser->payload_remaining > 0) {
        parser->needed = parser->payload_remaining;
        return TURBO_RTSP_FRAME_PARTIAL;
    }

    parser->frame_pending = 1;
    parser->needed = 0;
    return turbo_rtsp_interleaved_parser_deliver_copy(
        parser,
        frame,
        payload,
        payload_capacity);
}

int turbo_rtsp_interleaved_write_header(
    uint8_t *buffer,
    size_t buffer_size,
    uint8_t channel,
    uint16_t payload_len) {
    if (!buffer || buffer_size < TURBO_RTSP_INTERLEAVED_HEADER_SIZE) {
        return -1;
    }

    buffer[0] = '$';
    buffer[1] = channel;
    turbo_rtsp_write_u16be(buffer + 2, payload_len);
    return TURBO_RTSP_INTERLEAVED_HEADER_SIZE;
}

#endif /* TURBO_MEDIA_HAS_RTSP */
