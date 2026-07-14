/**
 * RTP Packet Implementation
 *
 * Build and parse RTP packets per RFC 3550
 */
#include "turbo_rtp.h"
#include <stdlib.h>
#include <string.h>

/* Network byte order helpers */
static inline uint16_t read_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static inline uint32_t read_u32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

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

void rtp_packet_init(rtp_packet_t *pkt) {
  if (!pkt) return;
  memset(pkt, 0, sizeof(rtp_packet_t));
  pkt->header.version = RTP_VERSION;
}

int rtp_packet_build(rtp_packet_t *pkt, uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc,
                     int marker, const uint8_t *payload, size_t payload_len, uint8_t *buffer,
                     size_t buffer_len) {
  if (!pkt || !buffer) return -1;

  size_t header_size = RTP_HEADER_SIZE;
  size_t total_size = header_size + payload_len;

  if (buffer_len < total_size) return -1;

  memset(pkt, 0, sizeof(*pkt));

  /* Fill header */
  pkt->header.version = RTP_VERSION;
  pkt->header.padding = 0;
  pkt->header.extension = 0;
  pkt->header.csrc_count = 0;
  pkt->header.marker = marker ? 1 : 0;
  pkt->header.payload_type = pt & 0x7F;
  pkt->header.sequence = seq;
  pkt->header.timestamp = ts;
  pkt->header.ssrc = ssrc;

  /* Write to buffer */
  uint8_t *p = buffer;

  /* Byte 0: V=2, P, X, CC */
  p[0] = (RTP_VERSION << 6) | (pkt->header.padding << 5) | (pkt->header.extension << 4) |
         (pkt->header.csrc_count & 0x0F);

  /* Byte 1: M, PT */
  p[1] = (pkt->header.marker << 7) | (pkt->header.payload_type & 0x7F);

  /* Bytes 2-3: Sequence number */
  write_u16(p + 2, seq);

  /* Bytes 4-7: Timestamp */
  write_u32(p + 4, ts);

  /* Bytes 8-11: SSRC */
  write_u32(p + 8, ssrc);

  p += RTP_HEADER_SIZE;

  /* Copy payload */
  if (payload && payload_len > 0) {
    memcpy(p, payload, payload_len);
  }

  /* Setup packet pointers */
  pkt->buffer = buffer;
  pkt->buffer_len = total_size;
  pkt->payload = buffer + header_size;
  pkt->payload_len = payload_len;
  pkt->owns_buffer = 0;

  return (int)total_size;
}

int rtp_packet_parse(rtp_packet_t *pkt, const uint8_t *buffer, size_t buffer_len) {
  if (!pkt || !buffer) return -1;
  if (buffer_len < RTP_HEADER_SIZE) return -1;

  memset(pkt, 0, sizeof(*pkt));

  const uint8_t *p = buffer;

  /* Byte 0: V, P, X, CC */
  uint8_t byte0 = p[0];
  pkt->header.version = (byte0 >> 6) & 0x03;
  pkt->header.padding = (byte0 >> 5) & 0x01;
  pkt->header.extension = (byte0 >> 4) & 0x01;
  pkt->header.csrc_count = byte0 & 0x0F;

  /* Check version */
  if (pkt->header.version != RTP_VERSION) return -1;

  /* Byte 1: M, PT */
  uint8_t byte1 = p[1];
  pkt->header.marker = (byte1 >> 7) & 0x01;
  pkt->header.payload_type = byte1 & 0x7F;

  /* Bytes 2-3: Sequence */
  pkt->header.sequence = read_u16(p + 2);

  /* Bytes 4-7: Timestamp */
  pkt->header.timestamp = read_u32(p + 4);

  /* Bytes 8-11: SSRC */
  pkt->header.ssrc = read_u32(p + 8);

  size_t header_size = RTP_HEADER_SIZE;

  /* Parse CSRC list */
  if (pkt->header.csrc_count > 0) {
    size_t csrc_size = pkt->header.csrc_count * 4;
    if (buffer_len < header_size + csrc_size) return -1;

    for (int i = 0; i < pkt->header.csrc_count && i < RTP_MAX_CSRC; i++) {
      pkt->csrc[i] = read_u32(p + header_size + i * 4);
    }
    header_size += csrc_size;
  }

  /* Parse RFC 8285 one-byte RTP header extensions when present. */
  if (pkt->header.extension) {
    if (buffer_len < header_size + 4) return -1;

    /* Extension header: 16-bit profile, 16-bit length (in 32-bit words) */
    uint16_t ext_profile = read_u16(p + header_size);
    uint16_t ext_len = read_u16(p + header_size + 2);
    size_t ext_size = 4 + ext_len * 4;

    if (buffer_len < header_size + ext_size) return -1;

    if (ext_profile == 0xBEDE) {
      const uint8_t *ext_p = p + header_size + 4;
      const uint8_t *ext_end = p + header_size + ext_size;

      while (ext_p < ext_end) {
        uint8_t ext_byte = *ext_p++;
        uint8_t id = (uint8_t)(ext_byte >> 4);
        uint8_t len = (uint8_t)((ext_byte & 0x0F) + 1);

        if (id == 0) {
          continue;
        }
        if (id == 15) {
          break;
        }
        if (ext_p + len > ext_end) {
          return -1;
        }

        if (pkt->extension_count < 8) {
          rtp_extension_t *ext = &pkt->extensions[pkt->extension_count++];
          ext->id = id;
          ext->length = len;
          memcpy(ext->data, ext_p, len);
        }
        ext_p += len;
      }
    }
    header_size += ext_size;
  }

  /* Handle padding */
  size_t payload_len = buffer_len - header_size;
  if (pkt->header.padding && payload_len > 0) {
    uint8_t pad_len = buffer[buffer_len - 1];
    if (pad_len > payload_len) return -1;
    payload_len -= pad_len;
  }

  /* Set payload pointer (points into original buffer) */
  pkt->payload = (uint8_t *)(buffer + header_size);
  pkt->payload_len = payload_len;
  pkt->buffer = (uint8_t *)buffer;
  pkt->buffer_len = buffer_len;
  pkt->owns_buffer = 0;

  return 0;
}

int rtp_packet_serialize(const rtp_packet_t *pkt, uint8_t *buffer, size_t buffer_len) {
  if (!pkt || !buffer) return -1;

  size_t csrc_size = pkt->header.csrc_count * 4;
  size_t header_size = RTP_HEADER_SIZE + csrc_size;
  size_t ext_payload_size = 0;
  size_t ext_padding = 0;
  size_t ext_size = 0;
  int write_extensions = pkt->extension_count > 0;

  if (write_extensions) {
    for (int i = 0; i < pkt->extension_count; i++) {
      if (pkt->extensions[i].id == 0 || pkt->extensions[i].id > 14 ||
          pkt->extensions[i].length == 0 || pkt->extensions[i].length > 16) {
        return -1;
      }
      ext_payload_size += 1 + pkt->extensions[i].length;
    }
    ext_padding = (4 - (ext_payload_size % 4)) % 4;
    ext_size = 4 + ext_payload_size + ext_padding;
  }

  size_t total_size = header_size + ext_size + pkt->payload_len;

  if (buffer_len < total_size) return -1;

  uint8_t *p = buffer;

  /* Byte 0: V, P, X, CC */
  p[0] = (pkt->header.version << 6) | (pkt->header.padding << 5) | (write_extensions << 4) |
         (pkt->header.csrc_count & 0x0F);

  /* Byte 1: M, PT */
  p[1] = (pkt->header.marker << 7) | (pkt->header.payload_type & 0x7F);

  /* Bytes 2-3: Sequence */
  write_u16(p + 2, pkt->header.sequence);

  /* Bytes 4-7: Timestamp */
  write_u32(p + 4, pkt->header.timestamp);

  /* Bytes 8-11: SSRC */
  write_u32(p + 8, pkt->header.ssrc);

  p += RTP_HEADER_SIZE;

  /* Write CSRC list */
  for (int i = 0; i < pkt->header.csrc_count && i < RTP_MAX_CSRC; i++) {
    write_u32(p, pkt->csrc[i]);
    p += 4;
  }

  if (write_extensions) {
    uint8_t *ext_start = p;
    uint8_t *payload_dst = ext_start + ext_size;

    if (pkt->payload && pkt->payload_len > 0) {
      memmove(payload_dst, pkt->payload, pkt->payload_len);
    }

    write_u16(ext_start, 0xBEDE);
    write_u16(ext_start + 2, (uint16_t)((ext_payload_size + ext_padding) / 4));
    p = ext_start + 4;

    for (int i = 0; i < pkt->extension_count; i++) {
      const rtp_extension_t *ext = &pkt->extensions[i];
      *p++ = (uint8_t)((ext->id << 4) | ((ext->length - 1) & 0x0F));
      memcpy(p, ext->data, ext->length);
      p += ext->length;
    }
    if (ext_padding > 0) {
      memset(p, 0, ext_padding);
    }
  } else if (pkt->payload && pkt->payload_len > 0) {
    memmove(p, pkt->payload, pkt->payload_len);
  }

  /* Copy payload */
  return (int)total_size;
}

void rtp_packet_free(rtp_packet_t *pkt) {
  if (!pkt) return;
  if (pkt->owns_buffer && pkt->buffer) {
    free(pkt->buffer);
  }
  memset(pkt, 0, sizeof(rtp_packet_t));
}

int rtp_seq_newer(uint16_t s1, uint16_t s2) {
  /* s1 is newer if it's within half the sequence space ahead of s2 */
  return ((int16_t)(s1 - s2)) > 0;
}

int rtp_seq_diff(uint16_t s1, uint16_t s2) {
  /* Returns s1 - s2 handling wraparound */
  return (int16_t)(s1 - s2);
}

/* =============================================================================
 * RTP Header Extension Functions (RFC 8285)
 * ============================================================================= */

int rtp_packet_add_extension(rtp_packet_t *pkt, uint8_t id, const uint8_t *data, uint8_t len) {
  if (!pkt || id == 0 || id > 14 || len == 0 || len > 16) return -1;
  if (pkt->extension_count >= 8) return -1; /* Max 8 extensions */

  /* Add extension */
  rtp_extension_t *ext = &pkt->extensions[pkt->extension_count];
  ext->id = id;
  ext->length = len;
  if (data && len > 0) {
    memcpy(ext->data, data, len);
  }

  pkt->extension_count++;
  pkt->header.extension = 1; /* Mark extension present */

  return 0;
}

int rtp_packet_get_extension(const rtp_packet_t *pkt, uint8_t id, uint8_t *data, uint8_t *len) {
  if (!pkt || id == 0 || !data || !len) return -1;

  /* Search for extension */
  for (int i = 0; i < pkt->extension_count; i++) {
    if (pkt->extensions[i].id == id) {
      *len = pkt->extensions[i].length;
      memcpy(data, pkt->extensions[i].data, *len);
      return 0;
    }
  }

  return -1; /* Not found */
}
