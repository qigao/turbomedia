/**
 * RTCP Packet Implementation
 *
 * Build and parse RTCP packets per RFC 3550 and RFC 4585
 */
#include "turbo_rtp.h"
#include <stdlib.h>
#include <string.h>

/* Network byte order helpers */
static inline uint16_t read_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static inline void write_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFF);
}

static inline void write_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)(v & 0xFF);
}

void rtcp_compound_init(rtcp_compound_t *compound, uint8_t *buffer, size_t buffer_len) {
  if (!compound) return;
  compound->buffer = buffer;
  compound->buffer_len = buffer_len;
  compound->offset = 0;
}

static int rtcp_write_header(uint8_t *p, int version, int padding, int count, int type,
                             int length_words) {
  p[0] = (version << 6) | (padding << 5) | (count & 0x1F);
  p[1] = type;
  write_u16(p + 2, (uint16_t)(length_words - 1));
  return 4;
}

int rtcp_compound_add_sr(rtcp_compound_t *compound, const rtcp_sr_t *sr,
                         const rtcp_rr_block_t *blocks, int block_count) {
  if (!compound || !sr) return -1;

  /* SR packet: header(4) + sender info(24) + report blocks(24 each) */
  size_t block_size = block_count * 24;
  size_t total_size = 4 + 24 + block_size;
  size_t length_words = (24 + block_size) / 4;

  if (compound->offset + total_size > compound->buffer_len) return -1;

  uint8_t *p = compound->buffer + compound->offset;

  /* Header: V=2, P=0, RC=block_count, PT=200 (SR) */
  rtcp_write_header(p, 2, 0, block_count, RTCP_SR, (int)(length_words + 1));
  p += 4;

  /* Sender SSRC */
  write_u32(p, sr->ssrc);
  p += 4;

  /* NTP timestamp */
  write_u32(p, sr->ntp_sec);
  p += 4;
  write_u32(p, sr->ntp_frac);
  p += 4;

  /* RTP timestamp */
  write_u32(p, sr->rtp_ts);
  p += 4;

  /* Packet count */
  write_u32(p, sr->packet_count);
  p += 4;

  /* Octet count */
  write_u32(p, sr->octet_count);
  p += 4;

  /* Report blocks */
  for (int i = 0; i < block_count && blocks; i++) {
    write_u32(p, blocks[i].ssrc);
    p += 4;

    /* Fraction lost (8 bits) + cumulative lost (24 bits) */
    uint32_t loss_word =
        ((uint32_t)blocks[i].fraction_lost << 24) | (blocks[i].cumulative_lost & 0x00FFFFFF);
    write_u32(p, loss_word);
    p += 4;

    write_u32(p, blocks[i].extended_seq);
    p += 4;
    write_u32(p, blocks[i].jitter);
    p += 4;
    write_u32(p, blocks[i].lsr);
    p += 4;
    write_u32(p, blocks[i].dlsr);
    p += 4;
  }

  compound->offset += total_size;
  return 0;
}

int rtcp_compound_add_rr(rtcp_compound_t *compound, uint32_t ssrc, const rtcp_rr_block_t *blocks,
                         int block_count) {
  if (!compound) return -1;

  /* RR packet: header(4) + reporter SSRC(4) + report blocks(24 each) */
  size_t block_size = block_count * 24;
  size_t total_size = 4 + 4 + block_size;
  size_t length_words = (4 + block_size) / 4;

  if (compound->offset + total_size > compound->buffer_len) return -1;

  uint8_t *p = compound->buffer + compound->offset;

  /* Header: V=2, P=0, RC=block_count, PT=201 (RR) */
  rtcp_write_header(p, 2, 0, block_count, RTCP_RR, (int)(length_words + 1));
  p += 4;

  /* Reporter SSRC */
  write_u32(p, ssrc);
  p += 4;

  /* Report blocks */
  for (int i = 0; i < block_count && blocks; i++) {
    write_u32(p, blocks[i].ssrc);
    p += 4;

    uint32_t loss_word =
        ((uint32_t)blocks[i].fraction_lost << 24) | (blocks[i].cumulative_lost & 0x00FFFFFF);
    write_u32(p, loss_word);
    p += 4;

    write_u32(p, blocks[i].extended_seq);
    p += 4;
    write_u32(p, blocks[i].jitter);
    p += 4;
    write_u32(p, blocks[i].lsr);
    p += 4;
    write_u32(p, blocks[i].dlsr);
    p += 4;
  }

  compound->offset += total_size;
  return 0;
}

int rtcp_compound_add_nack(rtcp_compound_t *compound, uint32_t sender_ssrc, uint32_t media_ssrc,
                           uint16_t pid, uint16_t blp) {
  if (!compound) return -1;

  /* NACK: header(4) + sender SSRC(4) + media SSRC(4) + FCI(4) = 16 bytes */
  size_t total_size = 16;
  if (compound->offset + total_size > compound->buffer_len) return -1;

  uint8_t *p = compound->buffer + compound->offset;

  /* Header: V=2, P=0, FMT=1, PT=205 (RTPFB) */
  rtcp_write_header(p, 2, 0, RTCP_NACK, RTCP_RTPFB, 4);
  p += 4;

  write_u32(p, sender_ssrc);
  p += 4;
  write_u32(p, media_ssrc);
  p += 4;

  /* FCI: PID (16 bits) + BLP (16 bits) */
  write_u16(p, pid);
  write_u16(p + 2, blp);

  compound->offset += total_size;
  return 0;
}

int rtcp_compound_add_pli(rtcp_compound_t *compound, uint32_t sender_ssrc, uint32_t media_ssrc) {
  if (!compound) return -1;

  /* PLI: header(4) + sender SSRC(4) + media SSRC(4) = 12 bytes */
  size_t total_size = 12;
  if (compound->offset + total_size > compound->buffer_len) return -1;

  uint8_t *p = compound->buffer + compound->offset;

  /* Header: V=2, P=0, FMT=1 (PLI), PT=206 (PSFB) */
  rtcp_write_header(p, 2, 0, RTCP_PLI, RTCP_PSFB, 3);
  p += 4;

  write_u32(p, sender_ssrc);
  p += 4;
  write_u32(p, media_ssrc);

  compound->offset += total_size;
  return 0;
}

int rtcp_compound_add_fir(rtcp_compound_t *compound, uint32_t sender_ssrc, uint32_t media_ssrc,
                          uint8_t seq_nr) {
  if (!compound) return -1;

  /* FIR: header(4) + sender SSRC(4) + media SSRC(4) + FCI(8) = 20 bytes */
  size_t total_size = 20;
  if (compound->offset + total_size > compound->buffer_len) return -1;

  uint8_t *p = compound->buffer + compound->offset;

  /* Header: V=2, P=0, FMT=4 (FIR), PT=206 (PSFB) */
  rtcp_write_header(p, 2, 0, RTCP_FIR, RTCP_PSFB, 5);
  p += 4;

  write_u32(p, sender_ssrc);
  p += 4;
  write_u32(p, media_ssrc);
  p += 4;

  /* FCI: SSRC (4) + Seq Nr (1) + Reserved (3) */
  write_u32(p, media_ssrc);
  p += 4;
  p[0] = seq_nr;
  p[1] = p[2] = p[3] = 0;

  compound->offset += total_size;
  return 0;
}

int rtcp_compound_add_remb(rtcp_compound_t *compound, uint32_t sender_ssrc, uint32_t media_ssrc,
                           uint32_t bitrate) {
  if (!compound) return -1;

  /* REMB: header(4) + sender SSRC(4) + media SSRC(4) + FCI(8+) */
  /* FCI: "REMB" (4) + num SSRCs (1) + BR exp (6 bits) + BR mantissa (18 bits) + SSRCs */
  size_t total_size = 24; /* With 1 SSRC */
  if (compound->offset + total_size > compound->buffer_len) return -1;

  uint8_t *p = compound->buffer + compound->offset;

  /* Header: V=2, P=0, FMT=15 (REMB), PT=206 (PSFB), total len = 24 bytes = 6 words */
  rtcp_write_header(p, 2, 0, RTCP_REMB, RTCP_PSFB, 6);
  p += 4;

  write_u32(p, sender_ssrc);
  p += 4;
  write_u32(p, 0); /* Media SSRC = 0 for REMB */
  p += 4;

  /* FCI: "REMB" identifier */
  p[0] = 'R';
  p[1] = 'E';
  p[2] = 'M';
  p[3] = 'B';
  p += 4;

  /* Num SSRCs (1) + BR (exponent 6 bits, mantissa 18 bits) */
  uint8_t exp = 0;
  uint32_t mantissa = bitrate;
  while (mantissa > 0x3FFFF && exp < 63) {
    mantissa >>= 1;
    exp++;
  }

  p[0] = 1; /* Num SSRCs */
  p[1] = (exp << 2) | ((mantissa >> 16) & 0x03);
  p[2] = (mantissa >> 8) & 0xFF;
  p[3] = mantissa & 0xFF;
  p += 4;

  /* SSRC being rate-limited */
  write_u32(p, media_ssrc);

  compound->offset += total_size;
  return 0;
}

size_t rtcp_compound_finish(rtcp_compound_t *compound) {
  if (!compound) return 0;
  return compound->offset;
}

int rtcp_compound_parse(const uint8_t *buffer, size_t buffer_len, rtcp_parse_cb callback,
                        void *user_data) {
  if (!buffer || buffer_len < 4) return -1;

  int packet_count = 0;
  size_t offset = 0;

  while (offset + 4 <= buffer_len) {
    const uint8_t *p = buffer + offset;

    /* Parse common header */
    uint8_t byte0 = p[0];
    int version = (byte0 >> 6) & 0x03;
    int type = p[1];
    uint16_t length_words = read_u16(p + 2);
    size_t length_bytes = (length_words + 1) * 4;

    if (version != 2) break;
    if (offset + length_bytes > buffer_len) break;

    if (callback) {
      callback(type, p, length_bytes, user_data);
    }

    offset += length_bytes;
    packet_count++;
  }

  return packet_count;
}
