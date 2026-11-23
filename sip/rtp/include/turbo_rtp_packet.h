#ifndef TURBO_RTP_PACKET_H
#define TURBO_RTP_PACKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTP_VERSION 2
#define RTP_HEADER_SIZE 12
#define RTP_MAX_CSRC 15
#define RTP_MAX_EXTENSIONS 8

/**
 * RTP Fixed Header (RFC 3550 Section 5.1)
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |V=2|P|X|  CC   |M|     PT      |       sequence number         |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                           timestamp                           |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |           synchronization source (SSRC) identifier            |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
typedef struct {
  uint8_t version : 2;      /* Protocol version (2) */
  uint8_t padding : 1;      /* Padding flag */
  uint8_t extension : 1;    /* Extension header present */
  uint8_t csrc_count : 4;   /* CSRC count */
  uint8_t marker : 1;       /* Marker bit (frame boundary) */
  uint8_t payload_type : 7; /* Payload type */
  uint16_t sequence;        /* Sequence number */
  uint32_t timestamp;       /* RTP timestamp */
  uint32_t ssrc;            /* Synchronization source */
} rtp_header_t;

/**
 * RTP Header Extension (RFC 8285 - One-Byte Header)
 */
typedef struct {
  uint8_t id;       /* Extension ID (1-14) */
  uint8_t length;   /* Length in bytes (1-16) */
  uint8_t data[16]; /* Extension data */
} rtp_extension_t;

/**
 * RTP Packet (header + payload)
 */
typedef struct {
  rtp_header_t header;
  uint32_t csrc[RTP_MAX_CSRC]; /* Contributing sources */

  /* Header extensions */
  rtp_extension_t extensions[RTP_MAX_EXTENSIONS]; /* Up to 8 extensions */
  int extension_count;

  const uint8_t *payload;   /* Payload data (points into buffer) */
  size_t payload_len;       /* Payload length excluding RTP padding */

  /* Buffer management */
  const uint8_t *buffer;   /* Raw packet buffer */
  size_t buffer_len;       /* Total buffer length */
  int owns_buffer;         /* Set only when the packet owns a malloc-allocated buffer. */

  /* Borrowed extension body following the four-byte profile/length prefix.
   * When present it takes precedence over the decoded extension array. */
  const uint8_t *raw_extension;
  size_t raw_extension_len;
  uint16_t extension_profile;
  uint8_t padding_len; /* RTP padding octets, including the final count octet */
} rtp_packet_t;

/**
 * Initialize an RTP packet structure
 */
void rtp_packet_init(rtp_packet_t *pkt);

/**
 * Build an RTP packet with a fixed header and no extensions or padding.
 * On success the packet borrows the output buffer until it is reused or freed.
 *
 * @param pkt       Packet structure to fill
 * @param pt        Payload type
 * @param seq       Sequence number
 * @param ts        RTP timestamp
 * @param ssrc      Synchronization source
 * @param marker    Marker bit (frame boundary)
 * @param payload   Payload data, or NULL when the payload length is zero
 * @param payload_len Payload length
 * @param buffer    Buffer to write packet into
 * @param buffer_len Buffer size
 * @return          Bytes written, or -1 on error
 */
int rtp_packet_build(rtp_packet_t *pkt, uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc,
                     int marker, const uint8_t *payload, size_t payload_len, uint8_t *buffer,
                     size_t buffer_len);

/**
 * Parse an RTP packet. Payload and raw extensions borrow the input buffer.
 * The result is valid on success and while the buffer is live and unchanged.
 *
 * @param pkt       Packet structure to fill
 * @param buffer    Buffer containing RTP packet
 * @param buffer_len Buffer length
 * @return          0 on success, -1 on error
 */
int rtp_packet_parse(rtp_packet_t *pkt, const uint8_t *buffer, size_t buffer_len);

/**
 * Serialize an RTP packet. X and P are derived from the extension and padding data.
 * Raw extensions are preserved until edited. New padding uses zero filler bytes.
 * The destination may be the original buffer of a parsed or built packet. Parse
 * the new bytes again before reusing the packet's borrowed views after such a write.
 *
 * @param pkt       Packet to serialize
 * @param buffer    Output buffer
 * @param buffer_len Buffer size
 * @return          Bytes written, or -1 on error with the output unchanged
 */
int rtp_packet_serialize(const rtp_packet_t *pkt, uint8_t *buffer, size_t buffer_len);

/**
 * Free RTP packet resources (if owns_buffer is set)
 */
void rtp_packet_free(rtp_packet_t *pkt);

/**
 * Add an RFC 8285 one-byte extension. Editing a parsed one-byte extension block
 * switches to serializing the decoded list. Other profiles stay opaque.
 *
 * @param pkt       RTP packet
 * @param id        Extension ID (1-14)
 * @param data      Extension data
 * @param len       Data length (1-16 bytes)
 * @return          0 on success, -1 on error
 */
int rtp_packet_add_extension(rtp_packet_t *pkt, uint8_t id, const uint8_t *data, uint8_t len);

/**
 * Get extension from RTP packet
 *
 * @param pkt       RTP packet
 * @param id        Extension ID to find
 * @param data      Output buffer with capacity for 16 bytes
 * @param len       Output: data length
 * @return          0 on success, -1 if not found
 */
int rtp_packet_get_extension(const rtp_packet_t *pkt, uint8_t id, uint8_t *data, uint8_t *len);

/**
 * Check if sequence number is newer (handles wraparound)
 */
int rtp_seq_newer(uint16_t s1, uint16_t s2);

/**
 * Calculate sequence number difference (handles wraparound)
 */
int rtp_seq_diff(uint16_t s1, uint16_t s2);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_RTP_PACKET_H */
