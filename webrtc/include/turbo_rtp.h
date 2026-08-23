/**
 * TurboNet RTP/RTCP Protocol Implementation
 *
 * Implements RFC 3550 (RTP) and RFC 3611 (RTCP XR)
 * Supports audio/video media transport with feedback mechanisms
 */
#ifndef TURBO_RTP_H
#define TURBO_RTP_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define RTP_VERSION 2
#define RTP_HEADER_SIZE 12 /* Fixed header without CSRC */
#define RTP_MAX_CSRC 15
#define RTP_MAX_PACKET 1500 /* MTU-safe */
#define RTP_SEQ_MOD (1 << 16)

/* RTCP Packet Types (RFC 3550) */
#define RTCP_SR 200   /* Sender Report */
#define RTCP_RR 201   /* Receiver Report */
#define RTCP_SDES 202 /* Source Description */
#define RTCP_BYE 203  /* Goodbye */
#define RTCP_APP 204  /* Application-Defined */

/* RTCP Feedback Types (RFC 4585) */
#define RTCP_RTPFB 205 /* Transport Layer Feedback */
#define RTCP_PSFB 206  /* Payload-Specific Feedback */

/* RTCP Feedback Message Types */
#define RTCP_NACK 1  /* Generic NACK (RTPFB) */
#define RTCP_PLI 1   /* Picture Loss Indication (PSFB) */
#define RTCP_FIR 4   /* Full Intra Request (PSFB) */
#define RTCP_REMB 15 /* Receiver Estimated Max Bitrate (PSFB) */

/* RTP Header Extension IDs (RFC 8285) */
#define RTP_EXT_TRANSPORT_CC 1   /* Transport-wide CC sequence number */
#define RTP_EXT_ABS_SEND_TIME 2  /* Absolute send time */
#define RTP_EXT_VIDEO_ROTATION 3 /* Video rotation */
#define RTP_EXT_AUDIO_LEVEL 4    /* Audio level */

/* Payload Types (RFC 3551 + dynamic) */
#define RTP_PT_PCMU 0   /* G.711 u-law */
#define RTP_PT_PCMA 8   /* G.711 A-law */
#define RTP_PT_OPUS 111 /* Opus audio (dynamic) */
#define RTP_PT_VP8 96   /* VP8 video (dynamic) */
#define RTP_PT_VP9 98   /* VP9 video (dynamic) */
#define RTP_PT_H264 102 /* H.264 video (dynamic) */

/* Clock rates */
#define RTP_CLOCK_AUDIO 48000 /* Opus default */
#define RTP_CLOCK_VIDEO 90000 /* Standard video clock */

/* =============================================================================
 * RTP Packet Structures
 * ============================================================================= */

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
  uint8_t length;   /* Length in bytes (0-16) */
  uint8_t data[16]; /* Extension data */
} rtp_extension_t;

/**
 * RTP Packet (header + payload)
 */
typedef struct {
  rtp_header_t header;
  uint32_t csrc[RTP_MAX_CSRC]; /* Contributing sources */

  /* Header extensions */
  rtp_extension_t extensions[8]; /* Up to 8 extensions */
  int extension_count;

  uint8_t *payload;   /* Payload data (points into buffer) */
  size_t payload_len; /* Payload length */

  /* Buffer management */
  uint8_t *buffer;   /* Raw packet buffer */
  size_t buffer_len; /* Total buffer length */
  int owns_buffer;   /* 1 if we should free buffer */
} rtp_packet_t;

/* =============================================================================
 * RTCP Packet Structures
 * ============================================================================= */

/**
 * RTCP Common Header
 */
typedef struct {
  uint8_t version : 2;
  uint8_t padding : 1;
  uint8_t count : 5;   /* Report count or format */
  uint8_t packet_type; /* RTCP packet type */
  uint16_t length;     /* Length in 32-bit words - 1 */
} rtcp_header_t;

/**
 * RTCP Sender Report (SR)
 */
typedef struct {
  uint32_t ssrc;         /* Sender SSRC */
  uint32_t ntp_sec;      /* NTP timestamp (seconds) */
  uint32_t ntp_frac;     /* NTP timestamp (fraction) */
  uint32_t rtp_ts;       /* RTP timestamp */
  uint32_t packet_count; /* Sender's packet count */
  uint32_t octet_count;  /* Sender's octet count */
} rtcp_sr_t;

/**
 * RTCP Reception Report Block
 */
typedef struct {
  uint32_t ssrc;                 /* Source being reported */
  uint32_t fraction_lost : 8;    /* Fraction lost since last SR/RR */
  uint32_t cumulative_lost : 24; /* Cumulative packets lost */
  uint32_t extended_seq;         /* Extended highest sequence received */
  uint32_t jitter;               /* Interarrival jitter */
  uint32_t lsr;                  /* Last SR timestamp (middle 32 bits) */
  uint32_t dlsr;                 /* Delay since last SR (1/65536 sec units) */
} rtcp_rr_block_t;

/**
 * RTCP Receiver Report (RR)
 */
typedef struct {
  uint32_t ssrc;              /* Reporter SSRC */
  rtcp_rr_block_t blocks[31]; /* Up to 31 report blocks */
  int block_count;
} rtcp_rr_t;

/**
 * RTCP NACK (Generic Negative Acknowledgement)
 */
typedef struct {
  uint32_t sender_ssrc; /* SSRC of sender */
  uint32_t media_ssrc;  /* SSRC of media source */
  uint16_t pid;         /* Packet ID (sequence number) */
  uint16_t blp;         /* Bitmask of following lost packets */
} rtcp_nack_t;

/**
 * RTCP PLI (Picture Loss Indication)
 */
typedef struct {
  uint32_t sender_ssrc; /* SSRC of sender */
  uint32_t media_ssrc;  /* SSRC of media source */
} rtcp_pli_t;

/**
 * RTCP FIR (Full Intra Request)
 */
typedef struct {
  uint32_t sender_ssrc; /* SSRC of sender */
  uint32_t media_ssrc;  /* SSRC of media source */
  uint8_t seq_nr;       /* Sequence number */
} rtcp_fir_t;

/**
 * RTCP REMB (Receiver Estimated Max Bitrate)
 */
typedef struct {
  uint32_t sender_ssrc; /* SSRC of sender */
  uint32_t media_ssrc;  /* SSRC of media source */
  uint32_t bitrate;     /* Estimated bitrate in bps */
} rtcp_remb_t;

/**
 * RTCP Transport-Wide Congestion Control (TWCC) - RFC 8888
 *
 * Provides detailed feedback on packet arrival times for bandwidth estimation
 */
#define RTCP_TWCC_MAX_PACKETS 8192 /* Maximum packets to track */

typedef enum {
  TWCC_SYMBOL_NOT_RECEIVED = 0, /* Packet not received */
  TWCC_SYMBOL_SMALL_DELTA = 1,  /* Small delta (1 byte) */
  TWCC_SYMBOL_LARGE_DELTA = 2,  /* Large delta (2 bytes) */
  TWCC_SYMBOL_RESERVED = 3      /* Reserved */
} twcc_symbol_t;

/**
 * TWCC Packet Status
 */
typedef struct {
  uint16_t seq;             /* Transport-wide sequence number */
  uint64_t arrival_time_us; /* Arrival timestamp (microseconds) */
  int received;             /* 1 if received, 0 if lost */
} twcc_packet_status_t;

/**
 * TWCC Feedback Message
 */
typedef struct {
  uint32_t sender_ssrc;    /* SSRC of feedback sender */
  uint32_t media_ssrc;     /* SSRC of media source */
  uint16_t base_seq;       /* Base transport sequence number */
  uint16_t packet_count;   /* Number of packets in this feedback */
  uint32_t reference_time; /* Reference time (24-bit, 64ms units) */
  uint8_t fb_pkt_count;    /* Feedback packet count */

  /* Packet status chunks */
  twcc_packet_status_t packets[RTCP_TWCC_MAX_PACKETS];
  int num_packets;
} rtcp_twcc_t;

/**
 * TWCC Tracker (for sender side)
 *
 * Tracks outgoing packets with transport-wide sequence numbers
 */
typedef struct twcc_tracker_s twcc_tracker_t;

/**
 * TWCC Receiver (for receiver side)
 *
 * Tracks incoming packets and generates feedback
 */
typedef struct twcc_receiver_s twcc_receiver_t;

/**
 * Bandwidth Estimation Result
 */
typedef struct {
  uint32_t target_bitrate_bps; /* Recommended target bitrate */
  uint32_t estimated_bw_bps;   /* Estimated available bandwidth */
  float packet_loss_ratio;     /* 0.0 - 1.0 */
  uint32_t rtt_ms;             /* Round-trip time */
  int congestion_detected;     /* 1 if congestion detected */
} twcc_bwe_result_t;

/**
 * Generic RTCP Compound Packet
 */
typedef struct {
  uint8_t *buffer;
  size_t buffer_len;
  size_t offset; /* Current write position */
} rtcp_compound_t;

/**
 * RTP Packet History for retransmission (NACK)
 */
typedef struct rtp_history_s rtp_history_t;

TURBO_MEDIA_API rtp_history_t *rtp_history_create(size_t max_packets, size_t max_packet_size);
TURBO_MEDIA_API void rtp_history_destroy(rtp_history_t *history);
TURBO_MEDIA_API void rtp_history_put(rtp_history_t *history, uint16_t seq, const uint8_t *packet,
                               size_t len);
TURBO_MEDIA_API int rtp_history_get(rtp_history_t *history, uint16_t seq, uint8_t *packet, size_t *len,
                              size_t max_len);

/* =============================================================================
 * RTP Session
 * ============================================================================= */

typedef struct rtp_session_s rtp_session_t;

typedef struct {
  uint32_t ssrc;        /* Local SSRC */
  uint8_t payload_type; /* Payload type for outgoing */
  uint32_t clock_rate;  /* Clock rate (e.g., 48000, 90000) */
  int is_audio;         /* 1 for audio, 0 for video */
} rtp_session_config_t;

/**
 * RTP Session Statistics
 */
typedef struct {
  /* Sender stats */
  uint64_t packets_sent;
  uint64_t octets_sent;

  /* Receiver stats */
  uint64_t packets_recv;
  uint64_t packets_lost;
  uint32_t jitter; /* Interarrival jitter (timestamp units) */

  /* Sequence tracking */
  uint16_t max_seq;  /* Highest sequence seen */
  uint32_t cycles;   /* Sequence number wraparound count */
  uint32_t base_seq; /* Base sequence number */

  /* Timing */
  uint32_t last_sr_ntp;  /* Last SR NTP timestamp (middle 32 bits) */
  uint64_t last_sr_time; /* Local time when last SR received */
  uint32_t rtt_ms;       /* Round-trip time estimate */
} rtp_session_stats_t;

/* =============================================================================
 * RTP Packet Functions
 * ============================================================================= */

/**
 * Initialize an RTP packet structure
 */
TURBO_MEDIA_API void rtp_packet_init(rtp_packet_t *pkt);

/**
 * Build RTP packet from header and payload
 *
 * @param pkt       Packet structure to fill
 * @param pt        Payload type
 * @param seq       Sequence number
 * @param ts        RTP timestamp
 * @param ssrc      Synchronization source
 * @param marker    Marker bit (frame boundary)
 * @param payload   Payload data
 * @param payload_len Payload length
 * @param buffer    Buffer to write packet into
 * @param buffer_len Buffer size
 * @return          Bytes written, or -1 on error
 */
TURBO_MEDIA_API int rtp_packet_build(rtp_packet_t *pkt, uint8_t pt, uint16_t seq, uint32_t ts,
                               uint32_t ssrc, int marker, const uint8_t *payload,
                               size_t payload_len, uint8_t *buffer, size_t buffer_len);

/**
 * Parse RTP packet from buffer
 *
 * @param pkt       Packet structure to fill
 * @param buffer    Buffer containing RTP packet
 * @param buffer_len Buffer length
 * @return          0 on success, -1 on error
 */
TURBO_MEDIA_API int rtp_packet_parse(rtp_packet_t *pkt, const uint8_t *buffer, size_t buffer_len);

/**
 * Serialize RTP packet to buffer
 *
 * @param pkt       Packet to serialize
 * @param buffer    Output buffer
 * @param buffer_len Buffer size
 * @return          Bytes written, or -1 on error
 */
TURBO_MEDIA_API int rtp_packet_serialize(const rtp_packet_t *pkt, uint8_t *buffer, size_t buffer_len);

/**
 * Free RTP packet resources (if owns_buffer is set)
 */
TURBO_MEDIA_API void rtp_packet_free(rtp_packet_t *pkt);

/**
 * Add extension to RTP packet
 *
 * @param pkt       RTP packet
 * @param id        Extension ID (1-14)
 * @param data      Extension data
 * @param len       Data length (0-16 bytes)
 * @return          0 on success, -1 on error
 */
TURBO_MEDIA_API int rtp_packet_add_extension(rtp_packet_t *pkt, uint8_t id, const uint8_t *data,
                                       uint8_t len);

/**
 * Get extension from RTP packet
 *
 * @param pkt       RTP packet
 * @param id        Extension ID to find
 * @param data      Output: extension data
 * @param len       Output: data length
 * @return          0 on success, -1 if not found
 */
TURBO_MEDIA_API int rtp_packet_get_extension(const rtp_packet_t *pkt, uint8_t id, uint8_t *data,
                                       uint8_t *len);

/* =============================================================================
 * RTCP Packet Functions
 * ============================================================================= */

/**
 * Initialize RTCP compound packet builder
 */
TURBO_MEDIA_API void rtcp_compound_init(rtcp_compound_t *compound, uint8_t *buffer, size_t buffer_len);

/**
 * Add Sender Report to compound packet
 */
TURBO_MEDIA_API int rtcp_compound_add_sr(rtcp_compound_t *compound, const rtcp_sr_t *sr,
                                   const rtcp_rr_block_t *blocks, int block_count);

/**
 * Add Receiver Report to compound packet
 */
TURBO_MEDIA_API int rtcp_compound_add_rr(rtcp_compound_t *compound, uint32_t ssrc,
                                   const rtcp_rr_block_t *blocks, int block_count);

/**
 * Add NACK to compound packet
 */
TURBO_MEDIA_API int rtcp_compound_add_nack(rtcp_compound_t *compound, uint32_t sender_ssrc,
                                     uint32_t media_ssrc, uint16_t pid, uint16_t blp);

/**
 * Add PLI to compound packet
 */
TURBO_MEDIA_API int rtcp_compound_add_pli(rtcp_compound_t *compound, uint32_t sender_ssrc,
                                    uint32_t media_ssrc);

/**
 * Add FIR to compound packet
 */
TURBO_MEDIA_API int rtcp_compound_add_fir(rtcp_compound_t *compound, uint32_t sender_ssrc,
                                    uint32_t media_ssrc, uint8_t seq_nr);

/**
 * Add REMB to compound packet
 */
TURBO_MEDIA_API int rtcp_compound_add_remb(rtcp_compound_t *compound, uint32_t sender_ssrc,
                                     uint32_t media_ssrc, uint32_t bitrate);

/**
 * Add TWCC feedback to compound packet
 */
TURBO_MEDIA_API int rtcp_compound_add_twcc(rtcp_compound_t *compound, const rtcp_twcc_t *twcc);

/**
 * Finalize compound packet and get total length
 */
TURBO_MEDIA_API size_t rtcp_compound_finish(rtcp_compound_t *compound);

/**
 * Parse RTCP compound packet
 *
 * @param buffer    Buffer containing RTCP packet(s)
 * @param buffer_len Buffer length
 * @param callback  Called for each parsed packet
 * @param user_data User data for callback
 * @return          Number of packets parsed, or -1 on error
 */
typedef void (*rtcp_parse_cb)(int type, const void *data, size_t len, void *user_data);
TURBO_MEDIA_API int rtcp_compound_parse(const uint8_t *buffer, size_t buffer_len, rtcp_parse_cb callback,
                                  void *user_data);

/* =============================================================================
 * RTP Session Functions
 * ============================================================================= */

/**
 * Create RTP session
 */
TURBO_MEDIA_API rtp_session_t *rtp_session_create(const rtp_session_config_t *config);

/**
 * Destroy RTP session
 */
TURBO_MEDIA_API void rtp_session_destroy(rtp_session_t *session);

/**
 * Generate next RTP packet for sending
 *
 * @param session   RTP session
 * @param payload   Payload data
 * @param payload_len Payload length
 * @param marker    Marker bit
 * @param pkt       Output packet
 * @param buffer    Buffer for packet data
 * @param buffer_len Buffer size
 * @return          Bytes written, or -1 on error
 */
TURBO_MEDIA_API int rtp_session_send(rtp_session_t *session, const uint8_t *payload, size_t payload_len,
                               int marker, rtp_packet_t *pkt, uint8_t *buffer, size_t buffer_len);

/**
 * Send payload using an explicit RTP timestamp while still allocating local
 * sequence numbers
 * and SSRC from the session.
 */
TURBO_MEDIA_API int rtp_session_send_with_timestamp(rtp_session_t *session, const uint8_t *payload,
                                              size_t payload_len, int marker, uint32_t timestamp,
                                              rtp_packet_t *pkt, uint8_t *buffer,
                                              size_t buffer_len);

/**
 * Process received RTP packet
 *
 * @param session   RTP session
 * @param pkt       Received packet
 * @return          0 on success, -1 on error (e.g., duplicate, too old)
 */
TURBO_MEDIA_API int rtp_session_recv(rtp_session_t *session, const rtp_packet_t *pkt);

/**
 * Advance timestamp for next packet
 *
 * @param session   RTP session
 * @param samples   Number of samples (audio) or ticks (video)
 */
TURBO_MEDIA_API void rtp_session_advance_timestamp(rtp_session_t *session, uint32_t samples);

/**
 * Build Sender Report
 */
TURBO_MEDIA_API int rtp_session_build_sr(rtp_session_t *session, rtcp_sr_t *sr);

/**
 * Build Receiver Report block
 */
TURBO_MEDIA_API int rtp_session_build_rr_block(rtp_session_t *session, uint32_t ssrc,
                                         rtcp_rr_block_t *block);

/**
 * Process received RTCP Sender Report
 */
TURBO_MEDIA_API void rtp_session_process_sr(rtp_session_t *session, const rtcp_sr_t *sr);

/**
 * Process received RTCP Receiver Report
 */
TURBO_MEDIA_API void rtp_session_process_rr(rtp_session_t *session, const rtcp_rr_block_t *block);

/**
 * Get session statistics
 */
TURBO_MEDIA_API void rtp_session_get_stats(const rtp_session_t *session, rtp_session_stats_t *stats);

/**
 * Get remote SSRC
 */
TURBO_MEDIA_API uint32_t rtp_session_get_remote_ssrc(const rtp_session_t *session);

/**
  * Set remote SSRC
  */
TURBO_MEDIA_API void rtp_session_set_remote_ssrc(rtp_session_t *session, uint32_t ssrc);

/**
  * Get RTP payload type used for outgoing packets.
  */
TURBO_MEDIA_API uint8_t rtp_session_get_payload_type(const rtp_session_t *session);

/**
  * Set RTP payload type for outgoing packets.
  */
TURBO_MEDIA_API void rtp_session_set_payload_type(rtp_session_t *session, uint8_t payload_type);

/**
 * Get current SSRC
 */
TURBO_MEDIA_API uint32_t rtp_session_get_ssrc(const rtp_session_t *session);

/**
 * Check if sequence number is newer (handles wraparound)
 */
TURBO_MEDIA_API int rtp_seq_newer(uint16_t s1, uint16_t s2);

/**
 * Calculate sequence number difference (handles wraparound)
 */
TURBO_MEDIA_API int rtp_seq_diff(uint16_t s1, uint16_t s2);

/* =============================================================================
 * TWCC Functions
 * ============================================================================= */

/**
 * Create TWCC tracker (sender side)
 *
 * Tracks outgoing packets with transport-wide sequence numbers
 *
 * @return  TWCC tracker instance, or NULL on error
 */
TURBO_MEDIA_API twcc_tracker_t *twcc_tracker_create(void);

/**
 * Destroy TWCC tracker
 */
TURBO_MEDIA_API void twcc_tracker_destroy(twcc_tracker_t *tracker);

/**
 * Override the sender-side TWCC bitrate baseline.
 *
 * Keeps TWCC bandwidth estimation aligned
 * with the encoder's current target.
 */
TURBO_MEDIA_API void twcc_tracker_set_bitrate(twcc_tracker_t *tracker, uint32_t bitrate_bps);

/**
 * Register outgoing packet with TWCC tracker
 *
 * @param tracker   TWCC tracker
 * @param seq       Transport-wide sequence number (assigned by tracker)
 * @param size      Packet size in bytes
 * @param send_time_us Send timestamp in microseconds
 * @return          Assigned transport-wide sequence number
 */
TURBO_MEDIA_API uint16_t twcc_tracker_register_packet(twcc_tracker_t *tracker, size_t size,
                                                uint64_t send_time_us);

/**
 * Process received TWCC feedback
 *
 * @param tracker   TWCC tracker
 * @param twcc      TWCC feedback message
 * @param result    Output: bandwidth estimation result
 * @return          0 on success, -1 on error
 */
TURBO_MEDIA_API int twcc_tracker_process_feedback(twcc_tracker_t *tracker, const rtcp_twcc_t *twcc,
                                            twcc_bwe_result_t *result);

/**
 * Create TWCC receiver (receiver side)
 *
 * Tracks incoming packets and generates feedback
 *
 * @param ssrc      Local SSRC for feedback
 * @return          TWCC receiver instance, or NULL on error
 */
TURBO_MEDIA_API twcc_receiver_t *twcc_receiver_create(uint32_t ssrc);

/**
 * Destroy TWCC receiver
 */
TURBO_MEDIA_API void twcc_receiver_destroy(twcc_receiver_t *receiver);

/**
 * Register received packet
 *
 * @param receiver      TWCC receiver
 * @param twcc_seq      Transport-wide sequence number from RTP extension
 * @param arrival_time_us Arrival timestamp in microseconds
 * @return              0 on success, -1 on error
 */
TURBO_MEDIA_API int twcc_receiver_register_packet(twcc_receiver_t *receiver, uint16_t twcc_seq,
                                            uint64_t arrival_time_us);

/**
 * Generate TWCC feedback message
 *
 * Should be called periodically (e.g., every 100ms) or when enough packets received
 *
 * @param receiver  TWCC receiver
 * @param twcc      Output: TWCC feedback message
 * @return          1 if feedback generated, 0 if not ready, -1 on error
 */
TURBO_MEDIA_API int twcc_receiver_generate_feedback(twcc_receiver_t *receiver, rtcp_twcc_t *twcc);

/**
 * Parse TWCC feedback from RTCP packet
 *
 * @param buffer    RTCP packet buffer
 * @param len       Buffer length
 * @param twcc      Output: parsed TWCC feedback
 * @return          0 on success, -1 on error
 */
TURBO_MEDIA_API int rtcp_parse_twcc(const uint8_t *buffer, size_t len, rtcp_twcc_t *twcc);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_RTP_H */
