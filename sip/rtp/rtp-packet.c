#include "rtp-packet-internal.h"
#include "rtp-ext.h"
#include "rtp-util.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t size;
  size_t extension_len;
  int has_extension;
} rtp_wire_header_t;

static int rtp_wire_header(const rtp_packet_t *pkt, rtp_wire_header_t *wire) {
  if (pkt->header.version != RTP_VERSION || pkt->extension_count < 0 ||
      pkt->extension_count > RTP_MAX_EXTENSIONS) {
    return -1;
  }

  wire->extension_len = 0;
  wire->has_extension = pkt->raw_extension != NULL || pkt->extension_count > 0;
  if (pkt->raw_extension) {
    if (pkt->raw_extension_len % 4 != 0 || pkt->raw_extension_len / 4 > UINT16_MAX) {
      return -1;
    }
    wire->extension_len = pkt->raw_extension_len;
  } else {
    for (int i = 0; i < pkt->extension_count; ++i) {
      const rtp_extension_t *ext = &pkt->extensions[i];
      if (ext->id == 0 || ext->id > 14 || ext->length == 0 || ext->length > sizeof(ext->data)) {
        return -1;
      }
      wire->extension_len += 1 + ext->length;
    }
    wire->extension_len = (wire->extension_len + 3) & ~(size_t)3;
  }

  wire->size = RTP_HEADER_SIZE + (size_t)pkt->header.csrc_count * 4;
  if (wire->has_extension) {
    wire->size += 4 + wire->extension_len;
  }
  return 0;
}

static void rtp_write_header(const rtp_packet_t *pkt, const rtp_wire_header_t *wire,
                             uint8_t *buffer) {
  uint8_t *ext_header = buffer + RTP_HEADER_SIZE + (size_t)pkt->header.csrc_count * 4;
  buffer[0] = (uint8_t)((pkt->header.version << 6) | ((pkt->padding_len != 0) << 5) |
                        (wire->has_extension << 4) | pkt->header.csrc_count);
  buffer[1] = (uint8_t)((pkt->header.marker << 7) | pkt->header.payload_type);
  nbo_w16(buffer + 2, pkt->header.sequence);
  nbo_w32(buffer + 4, pkt->header.timestamp);
  nbo_w32(buffer + 8, pkt->header.ssrc);
  for (size_t i = 0; i < pkt->header.csrc_count; ++i) {
    nbo_w32(buffer + RTP_HEADER_SIZE + i * 4, pkt->csrc[i]);
  }

  if (!wire->has_extension) {
    return;
  }
  nbo_w16(ext_header, pkt->raw_extension ? pkt->extension_profile : RTP_HDREXT_PROFILE_ONE_BYTE);
  nbo_w16(ext_header + 2, (uint16_t)(wire->extension_len / 4));
  if (pkt->raw_extension) {
    return;
  }

  uint8_t *out = ext_header + 4;
  for (int i = 0; i < pkt->extension_count; ++i) {
    const rtp_extension_t *ext = &pkt->extensions[i];
    *out++ = (uint8_t)((ext->id << 4) | (ext->length - 1));
    memcpy(out, ext->data, ext->length);
    out += ext->length;
  }
  memset(out, 0, (size_t)(ext_header + 4 + wire->extension_len - out));
}

void rtp_packet_init(rtp_packet_t *pkt) {
  if (pkt) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->header.version = RTP_VERSION;
  }
}

int rtp_packet_build(rtp_packet_t *pkt, uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc,
                     int marker, const uint8_t *payload, size_t payload_len, uint8_t *buffer,
                     size_t buffer_len) {
  if (!pkt) {
    return -1;
  }

  rtp_packet_t packet;
  rtp_packet_init(&packet);
  packet.header.payload_type = pt & 0x7F;
  packet.header.sequence = seq;
  packet.header.timestamp = ts;
  packet.header.ssrc = ssrc;
  packet.header.marker = marker != 0;
  packet.payload = payload;
  packet.payload_len = payload_len;

  int size = rtp_packet_serialize(&packet, buffer, buffer_len);
  if (size > 0) {
    packet.buffer = buffer;
    packet.buffer_len = (size_t)size;
    packet.payload = buffer + RTP_HEADER_SIZE;
    *pkt = packet;
  }
  return size;
}

static int rtp_read_one_byte_extensions(rtp_packet_t *pkt) {
  const uint8_t *p = pkt->raw_extension;
  const uint8_t *end = p + pkt->raw_extension_len;
  while (p < end) {
    uint8_t element = *p++;
    uint8_t id = element >> 4;
    uint8_t len = (element & 0x0F) + 1;
    if (id == 0) {
      continue;
    }
    if (id == 15) {
      break;
    }
    if ((size_t)(end - p) < len) {
      return -1;
    }
    if (pkt->extension_count < RTP_MAX_EXTENSIONS) {
      rtp_extension_t *ext = &pkt->extensions[pkt->extension_count++];
      ext->id = id;
      ext->length = len;
      memcpy(ext->data, p, len);
    }
    p += len;
  }
  return 0;
}

int rtp_packet_parse(rtp_packet_t *pkt, const uint8_t *buffer, size_t buffer_len) {
  if (!pkt || !buffer || buffer_len < RTP_HEADER_SIZE || buffer_len > INT_MAX ||
      (buffer[0] >> 6) != RTP_VERSION) {
    return -1;
  }

  rtp_packet_init(pkt);
  pkt->header.padding = (buffer[0] >> 5) & 1;
  pkt->header.extension = (buffer[0] >> 4) & 1;
  pkt->header.csrc_count = buffer[0] & 0x0F;
  pkt->header.marker = buffer[1] >> 7;
  pkt->header.payload_type = buffer[1] & 0x7F;
  pkt->header.sequence = nbo_r16(buffer + 2);
  pkt->header.timestamp = nbo_r32(buffer + 4);
  pkt->header.ssrc = nbo_r32(buffer + 8);

  size_t header_size = RTP_HEADER_SIZE + (size_t)pkt->header.csrc_count * 4;
  if (header_size > buffer_len) {
    return -1;
  }
  for (size_t i = 0; i < pkt->header.csrc_count; ++i) {
    pkt->csrc[i] = nbo_r32(buffer + RTP_HEADER_SIZE + i * 4);
  }

  if (pkt->header.extension) {
    if (buffer_len - header_size < 4) {
      return -1;
    }
    pkt->extension_profile = nbo_r16(buffer + header_size);
    pkt->raw_extension_len = (size_t)nbo_r16(buffer + header_size + 2) * 4;
    header_size += 4;
    if (pkt->raw_extension_len > buffer_len - header_size) {
      return -1;
    }
    pkt->raw_extension = buffer + header_size;
    if (pkt->extension_profile == RTP_HDREXT_PROFILE_ONE_BYTE &&
        rtp_read_one_byte_extensions(pkt) != 0) {
      return -1;
    }
    header_size += pkt->raw_extension_len;
  }

  size_t body_size = buffer_len - header_size;
  if (pkt->header.padding) {
    pkt->padding_len = buffer[buffer_len - 1];
    if (pkt->padding_len == 0 || pkt->padding_len > body_size) {
      return -1;
    }
  }
  pkt->payload = buffer + header_size;
  pkt->payload_len = body_size - pkt->padding_len;
  pkt->buffer = buffer;
  pkt->buffer_len = buffer_len;
  return 0;
}

int rtp_packet_serialize_header(const rtp_packet_t *pkt, uint8_t *buffer, size_t buffer_len) {
  rtp_wire_header_t wire;
  if (!pkt || !buffer || rtp_wire_header(pkt, &wire) != 0 || wire.size > buffer_len) {
    return -1;
  }
  if (pkt->raw_extension && wire.extension_len != 0) {
    memmove(buffer + wire.size - wire.extension_len, pkt->raw_extension, wire.extension_len);
  }
  rtp_write_header(pkt, &wire, buffer);
  return (int)wire.size;
}

int rtp_packet_serialize(const rtp_packet_t *pkt, uint8_t *buffer, size_t buffer_len) {
  rtp_wire_header_t wire;
  if (!pkt || !buffer || (!pkt->payload && pkt->payload_len != 0) ||
      rtp_wire_header(pkt, &wire) != 0) {
    return -1;
  }
  size_t overhead = wire.size + pkt->padding_len;
  if (overhead > buffer_len || pkt->payload_len > buffer_len - overhead ||
      pkt->payload_len > INT_MAX - overhead) {
    return -1;
  }

  size_t copied = pkt->payload_len;
  size_t body_size = pkt->payload_len + pkt->padding_len;
  // Reuse the original padding when the payload and padding still form the source packet tail.
  if (pkt->padding_len && pkt->buffer && pkt->buffer_len >= RTP_HEADER_SIZE + body_size &&
      pkt->payload == pkt->buffer + pkt->buffer_len - body_size &&
      (pkt->buffer[0] & 0x20) && pkt->buffer[pkt->buffer_len - 1] == pkt->padding_len) {
    copied = body_size;
  }
  if (pkt->raw_extension && pkt->payload == pkt->raw_extension + wire.extension_len) {
    // Move adjacent views together when the CSRC list changes during an in-place write.
    memmove(buffer + wire.size - wire.extension_len, pkt->raw_extension,
            wire.extension_len + copied);
  } else {
    if (copied != 0) {
      memmove(buffer + wire.size, pkt->payload, copied);
    }
    if (pkt->raw_extension && wire.extension_len != 0) {
      memmove(buffer + wire.size - wire.extension_len, pkt->raw_extension, wire.extension_len);
    }
  }
  rtp_write_header(pkt, &wire, buffer);
  if (copied != body_size) {
    uint8_t *padding = buffer + wire.size + pkt->payload_len;
    memset(padding, 0, pkt->padding_len - 1);
    padding[pkt->padding_len - 1] = pkt->padding_len;
  }
  return (int)(wire.size + body_size);
}

void rtp_packet_free(rtp_packet_t *pkt) {
  if (pkt) {
    if (pkt->owns_buffer) {
      free((void *)pkt->buffer);
    }
    memset(pkt, 0, sizeof(*pkt));
  }
}

int rtp_seq_newer(uint16_t s1, uint16_t s2) {
  return rtp_seq_diff(s1, s2) > 0;
}

int rtp_seq_diff(uint16_t s1, uint16_t s2) {
  return (int16_t)(s1 - s2);
}

int rtp_packet_add_extension(rtp_packet_t *pkt, uint8_t id, const uint8_t *data, uint8_t len) {
  if (!pkt || !data || id == 0 || id > 14 || len == 0 || len > sizeof(pkt->extensions[0].data) ||
      pkt->extension_count < 0 || pkt->extension_count >= RTP_MAX_EXTENSIONS ||
      (pkt->raw_extension && pkt->extension_profile != RTP_HDREXT_PROFILE_ONE_BYTE)) {
    return -1;
  }

  rtp_extension_t *ext = &pkt->extensions[pkt->extension_count++];
  ext->id = id;
  ext->length = len;
  memcpy(ext->data, data, len);
  pkt->raw_extension = NULL;
  pkt->raw_extension_len = 0;
  pkt->extension_profile = RTP_HDREXT_PROFILE_ONE_BYTE;
  pkt->header.extension = 1;
  return 0;
}

int rtp_packet_get_extension(const rtp_packet_t *pkt, uint8_t id, uint8_t *data, uint8_t *len) {
  if (!pkt || !data || !len || id == 0 || id > 14 ||
      pkt->extension_count < 0 || pkt->extension_count > RTP_MAX_EXTENSIONS) {
    return -1;
  }
  for (int i = 0; i < pkt->extension_count; ++i) {
    const rtp_extension_t *ext = &pkt->extensions[i];
    if (ext->id == id && ext->length != 0 && ext->length <= sizeof(ext->data)) {
      *len = ext->length;
      memcpy(data, ext->data, *len);
      return 0;
    }
  }
  return -1;
}
