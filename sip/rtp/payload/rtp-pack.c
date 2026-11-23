// RFC3551 RTP Profile for Audio and Video Conferences with Minimal Control

#include "turbo_rtp_packet.h"
#include "rtp-payload-internal.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

struct rtp_packer_t
{
	struct rtp_payload_t handler;
	void* cbparam;

	rtp_packet_t pkt;
	int size;
};

static void* rtp_pack_create(int size, uint8_t pt, uint16_t seq, uint32_t ssrc, struct rtp_payload_t *handler, void* param)
{
	struct rtp_packer_t *packer;
	packer = (struct rtp_packer_t *)calloc(1, sizeof(*packer));
	if (!packer) return NULL;

	memcpy(&packer->handler, handler, sizeof(packer->handler));
	packer->cbparam = param;
	packer->size = size;

	packer->pkt.header.version = RTP_VERSION;
	packer->pkt.header.payload_type = pt;
	packer->pkt.header.sequence = seq;
	packer->pkt.header.ssrc = ssrc;
	return packer;
}

void rtp_pack_destroy(void* p)
{
	struct rtp_packer_t *packer;
	packer = (struct rtp_packer_t *)p;
#if defined(_DEBUG) || defined(DEBUG)
	memset(packer, 0xCC, sizeof(*packer));
#endif
	free(packer);
}

void rtp_pack_get_info(void* p, uint16_t* seq, uint32_t* timestamp)
{
	struct rtp_packer_t *packer;
	packer = (struct rtp_packer_t *)p;
	*seq = (uint16_t)packer->pkt.header.sequence;
	*timestamp = packer->pkt.header.timestamp;
}

int rtp_pack_input(void* p, const void* data, int bytes, uint32_t timestamp)
{
	int r, n;
	uint8_t *rtp;
	const uint8_t *ptr;
	struct rtp_packer_t *packer;

	r = 0;
	packer = (struct rtp_packer_t *)p;
	assert(packer->pkt.header.timestamp != timestamp || !packer->pkt.payload /*first packet*/);
	packer->pkt.header.timestamp = timestamp; // (uint32_t)time * packer->frequency / 1000; // ms -> 8KHZ
	packer->pkt.header.marker = 0; // marker bit alway 0

	for(ptr = (const uint8_t *)data; 0 == r && bytes > 0; ++packer->pkt.header.sequence)
	{
		packer->pkt.payload = ptr;
		packer->pkt.payload_len = (bytes + RTP_HEADER_SIZE) <= packer->size ? bytes : (packer->size - RTP_HEADER_SIZE);
		ptr += packer->pkt.payload_len;
		bytes -= packer->pkt.payload_len;

		n = RTP_HEADER_SIZE + packer->pkt.payload_len;
		rtp = (uint8_t*)packer->handler.alloc(packer->cbparam, n);
		if (!rtp) return -ENOMEM;

		n = rtp_packet_serialize(&packer->pkt, rtp, n);
		if (n < 0 || (size_t)n != RTP_HEADER_SIZE + packer->pkt.payload_len)
		{
			assert(0);
			return -1;
		}

		r = packer->handler.packet(packer->cbparam, rtp, n, packer->pkt.header.timestamp, 0);
		packer->handler.free(packer->cbparam, rtp);

		//packer->pkt.header.timestamp += packer->pkt.payload_len * packer->frequency / 1000;
	}

	return r;
}

struct rtp_payload_encode_t *rtp_common_encode()
{
	static struct rtp_payload_encode_t packer = {
		rtp_pack_create,
		rtp_pack_destroy,
		rtp_pack_get_info,
		rtp_pack_input,
	};

	return &packer;
}
