// RFC6184 RTP Payload Format for H.264 Video
//
// 6.2. Single NAL Unit Mode (All receivers MUST support this mode)
//   packetization-mode media type parameter is equal to 0 or the packetization - mode is not present.
//   Only single NAL unit packets MAY be used in this mode.
//   STAPs, MTAPs, and FUs MUST NOT be used.
//   The transmission order of single NAL unit packets MUST comply with the NAL unit decoding order.
// 6.3. Non-Interleaved Mode (This mode SHOULD be supported)
//   packetization-mode media type parameter is equal to 1.
//   Only single NAL unit packets, STAP - As, and FU - As MAY be used in this mode.
//   STAP-Bs, MTAPs, and FU-Bs MUST NOT be used.
//   The transmission order of NAL units MUST comply with the NAL unit decoding order
// 6.4. Interleaved Mode
//   packetization-mode media type parameter is equal to 2.
//   STAP-Bs, MTAPs, FU-As, and FU-Bs MAY be used. 
//   STAP-As and single NAL unit packets MUST NOT be used.
//   The transmission order of packets and NAL units is constrained as specified in Section 5.5.
//
// 5.1. RTP Header Usage (p10)
// The RTP timestamp is set to the sampling timestamp of the content. A 90 kHz clock rate MUST be used.

#include "turbo_rtp_packet.h"
#include "rtp-payload-internal.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#define KHz         90 // 90000Hz
#define FU_START    0x80
#define FU_END      0x40

#define N_FU_HEADER	2

int rtp_h264_annexb_nalu(const void* h264, int bytes, int (*handler)(void* param, const uint8_t* nalu, int bytes, int last), void* param);

struct rtp_encode_h264_t
{
	rtp_packet_t pkt;
	struct rtp_payload_t handler;
	void* cbparam;
	int size;
};

static void* rtp_h264_pack_create(int size, uint8_t pt, uint16_t seq, uint32_t ssrc, struct rtp_payload_t *handler, void* cbparam)
{
	struct rtp_encode_h264_t *packer;
	packer = (struct rtp_encode_h264_t *)calloc(1, sizeof(*packer));
	if(!packer) return NULL;

	memcpy(&packer->handler, handler, sizeof(packer->handler));
	packer->cbparam = cbparam;
	packer->size = size;

	packer->pkt.header.version = RTP_VERSION;
	packer->pkt.header.payload_type = pt;
	packer->pkt.header.sequence = seq;
	packer->pkt.header.ssrc = ssrc;
	return packer;
}

static void rtp_h264_pack_destroy(void* pack)
{
	struct rtp_encode_h264_t *packer;
	packer = (struct rtp_encode_h264_t *)pack;
#if defined(_DEBUG) || defined(DEBUG)
	memset(packer, 0xCC, sizeof(*packer));
#endif
	free(packer);
}

static void rtp_h264_pack_get_info(void* pack, uint16_t* seq, uint32_t* timestamp)
{
	struct rtp_encode_h264_t *packer;
	packer = (struct rtp_encode_h264_t *)pack;
	*seq = (uint16_t)packer->pkt.header.sequence;
	*timestamp = packer->pkt.header.timestamp;
}

static int rtp_h264_pack_nalu(struct rtp_encode_h264_t *packer, const uint8_t* nalu, int bytes, int mark)
{
	int r, n;
	uint8_t *rtp;

	packer->pkt.payload = nalu;
	packer->pkt.payload_len = bytes;
	n = RTP_HEADER_SIZE + packer->pkt.payload_len;
	rtp = (uint8_t*)packer->handler.alloc(packer->cbparam, n);
	if (!rtp) return -ENOMEM;

	//packer->pkt.header.marker = 1; // set marker flag
	packer->pkt.header.marker = (*nalu & 0x1f) <= 5 ? mark : 0; // VCL only
	n = rtp_packet_serialize(&packer->pkt, rtp, n);
	if (n < 0 || (size_t)n != RTP_HEADER_SIZE + packer->pkt.payload_len)
	{
		assert(0);
		return -1;
	}

	++packer->pkt.header.sequence;
	r = packer->handler.packet(packer->cbparam, rtp, n, packer->pkt.header.timestamp, 0);
	packer->handler.free(packer->cbparam, rtp);
	return r;
}

static int rtp_h264_pack_fu_a(struct rtp_encode_h264_t *packer, const uint8_t* nalu, int bytes, int mark)
{
	int r, n;
	unsigned char *rtp;

	// RFC6184 5.3. NAL Unit Header Usage: Table 2 (p15)
	// RFC6184 5.8. Fragmentation Units (FUs) (p29)
	uint8_t fu_indicator = (*nalu & 0xE0) | 28; // FU-A
	uint8_t fu_header = *nalu & 0x1F;

	r = 0;
	nalu += 1; // skip NAL Unit Type byte
	bytes -= 1;
	assert(bytes > 0);

	// FU-A start
	for (fu_header |= FU_START; 0 == r && bytes > 0; ++packer->pkt.header.sequence)
	{
		if (bytes + RTP_HEADER_SIZE <= packer->size - N_FU_HEADER)
		{
			assert(0 == (fu_header & FU_START));
			fu_header = FU_END | (fu_header & 0x1F); // FU-A end
			packer->pkt.payload_len = bytes;
		}
		else
		{
			packer->pkt.payload_len = packer->size - RTP_HEADER_SIZE - N_FU_HEADER;
		}

		packer->pkt.payload = nalu;
		n = RTP_HEADER_SIZE + N_FU_HEADER + packer->pkt.payload_len;
		rtp = (uint8_t*)packer->handler.alloc(packer->cbparam, n);
		if (!rtp) return -ENOMEM;

		packer->pkt.header.marker = (FU_END & fu_header) ? mark : 0; // set marker flag
		n = rtp_packet_serialize_header(&packer->pkt, rtp, n);
		if (n != RTP_HEADER_SIZE)
		{
			assert(0);
			return -1;
		}

		/*fu_indicator + fu_header*/
		rtp[n + 0] = fu_indicator;
		rtp[n + 1] = fu_header;
		memcpy(rtp + n + N_FU_HEADER, packer->pkt.payload, packer->pkt.payload_len);

		r = packer->handler.packet(packer->cbparam, rtp, n + N_FU_HEADER + packer->pkt.payload_len, packer->pkt.header.timestamp, 0);
		packer->handler.free(packer->cbparam, rtp);

		bytes -= packer->pkt.payload_len;
		nalu += packer->pkt.payload_len;
		fu_header &= 0x1F; // clear flags
	}

	return r;
}

static int rtp_h264_pack_handler(void* pack, const uint8_t* nalu, int bytes, int last)
{
	struct rtp_encode_h264_t* packer;
	packer = (struct rtp_encode_h264_t*)pack;
	if (bytes + RTP_HEADER_SIZE <= packer->size)
	{
		// single NAl unit packet 
		return rtp_h264_pack_nalu(packer, nalu, bytes, last ? 1 : 0);
	}
	else
	{
		return rtp_h264_pack_fu_a(packer, nalu, bytes, last ? 1 : 0);
	}
}

static int rtp_h264_pack_input(void* pack, const void* h264, int bytes, uint32_t timestamp)
{
	struct rtp_encode_h264_t *packer;
	packer = (struct rtp_encode_h264_t *)pack;
//	assert(packer->pkt.header.timestamp != timestamp || !packer->pkt.payload /*first packet*/);
	packer->pkt.header.timestamp = timestamp; //(uint32_t)time * KHz; // ms -> 90KHZ
	return rtp_h264_annexb_nalu(h264, bytes, rtp_h264_pack_handler, packer);
}

struct rtp_payload_encode_t *rtp_h264_encode()
{
	static struct rtp_payload_encode_t packer = {
		rtp_h264_pack_create,
		rtp_h264_pack_destroy,
		rtp_h264_pack_get_info,
		rtp_h264_pack_input,
	};

	return &packer;
}
