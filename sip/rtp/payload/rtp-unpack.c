// RFC3551 RTP Profile for Audio and Video Conferences with Minimal Control

#include "turbo_rtp_packet.h"
#include "rtp-profile.h"
#include "rtp-payload-helper.h"
#include "rtp-payload-internal.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

static int rtp_decode_rfc2250(void* p, const void* packet, int bytes)
{
	int r;
	rtp_packet_t pkt;
	struct rtp_payload_helper_t *helper;

	r = 0;
	helper = (struct rtp_payload_helper_t *)p;
	if (!helper || 0 != rtp_packet_parse(&pkt, packet, bytes))
		return -EINVAL;

	assert(pkt.payload_len >= 0);
	if(pkt.payload_len > 0)
		r = helper->handler.packet(helper->cbparam, pkt.payload, pkt.payload_len, pkt.header.timestamp, 0);
	return 0 == r ? 1 : r; // packet handled
}

struct rtp_payload_decode_t *rtp_common_decode()
{
	static struct rtp_payload_decode_t unpacker = {
		rtp_payload_helper_create,
		rtp_payload_helper_destroy,
		rtp_decode_rfc2250,
	};

	return &unpacker;
}
