#ifndef _rtp_util_h_
#define _rtp_util_h_

#include <stdint.h>
#include "rtcp-header.h"

// The Internet Protocol defines big-endian as the standard network byte order
#define nbo_r16 rtp_read_uint16
#define nbo_r32 rtp_read_uint32
#define nbo_w16 rtp_write_uint16
#define nbo_w32 rtp_write_uint32

static inline uint16_t rtp_read_uint16(const uint8_t* ptr)
{
	return (((uint16_t)ptr[0]) << 8) | ptr[1];
}

static inline uint32_t rtp_read_uint32(const uint8_t* ptr)
{
	return (((uint32_t)ptr[0]) << 24) | (((uint32_t)ptr[1]) << 16) | (((uint32_t)ptr[2]) << 8) | ptr[3];
}

static inline uint64_t rtp_read_uint64(const uint8_t* ptr)
{
	return (((uint64_t)rtp_read_uint32(ptr)) << 32) | rtp_read_uint32(ptr + 4);
}

static inline void rtp_write_uint16(uint8_t* ptr, uint16_t val)
{
	ptr[0] = (uint8_t)(val >> 8);
	ptr[1] = (uint8_t)val;
}

static inline void rtp_write_uint32(uint8_t* ptr, uint32_t val)
{
	ptr[0] = (uint8_t)(val >> 24);
	ptr[1] = (uint8_t)(val >> 16);
	ptr[2] = (uint8_t)(val >> 8);
	ptr[3] = (uint8_t)val;
}

static inline void rtp_write_uint64(uint8_t* ptr, uint64_t val)
{
	rtp_write_uint32(ptr, (uint32_t)(val >> 32));
	rtp_write_uint32(ptr + 4, (uint32_t)val);
}

static inline void nbo_write_rtcp_header(uint8_t *ptr, const rtcp_header_t *header)
{
	ptr[0] = (uint8_t)((header->v << 6) | (header->p << 5) | header->rc);
	ptr[1] = (uint8_t)(header->pt);
	ptr[2] = (uint8_t)(header->length >> 8);
	ptr[3] = (uint8_t)(header->length & 0xFF);
}

#endif /* !_rtp_util_h_ */
