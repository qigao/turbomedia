#ifndef TURBO_RTP_PACKET_INTERNAL_H
#define TURBO_RTP_PACKET_INTERNAL_H

#include "turbo_rtp_packet.h"

int rtp_packet_serialize_header(const rtp_packet_t *pkt, uint8_t *buffer, size_t buffer_len);

#endif
