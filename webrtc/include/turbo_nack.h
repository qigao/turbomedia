/**
 * NACK (Negative Acknowledgement) API
 *
 * Packet loss detection and retransmission for reliable video streaming
 */
#ifndef TURBO_NACK_H
#define TURBO_NACK_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct nack_receiver_t nack_receiver_t;
typedef struct nack_sender_t nack_sender_t;

/* =============================================================================
 * NACK Receiver (Detects Loss and Requests Retransmission)
 * ============================================================================= */

/**
 * Create NACK receiver
 *
 * @param ssrc SSRC of the stream
 * @return NACK receiver context or NULL on error
 */
CXX_C_API nack_receiver_t *nack_receiver_create(uint32_t ssrc);

/**
 * Destroy NACK receiver
 */
CXX_C_API void nack_receiver_destroy(nack_receiver_t *nack);

/**
 * Process received packet
 *
 * Detects sequence number gaps and tracks missing packets
 *
 * @param nack NACK receiver context
 * @param seq_num RTP sequence number
 * @param now Current timestamp in microseconds
 */
CXX_C_API void nack_receiver_process_packet(nack_receiver_t *nack, uint16_t seq_num, int64_t now);

/**
 * Get list of packets to NACK
 *
 * Returns sequence numbers that need retransmission
 *
 * @param nack NACK receiver context
 * @param seq_nums Output buffer for sequence numbers
 * @param max_count Maximum number of sequence numbers to return
 * @param now Current timestamp in microseconds
 * @return Number of sequence numbers returned
 */
CXX_C_API int nack_receiver_get_nacks(nack_receiver_t *nack, uint16_t *seq_nums, int max_count,
                                      int64_t now);

/**
 * Cleanup old missing packets
 *
 * Removes packets that are too old or retried too many times
 *
 * @param nack NACK receiver context
 * @param now Current timestamp in microseconds
 */
CXX_C_API void nack_receiver_cleanup_old(nack_receiver_t *nack, int64_t now);

/**
 * Set NACK callback
 *
 * Called when NACKs need to be sent
 *
 * @param nack NACK receiver context
 * @param callback Callback function
 * @param user_data User data passed to callback
 */
CXX_C_API void nack_receiver_set_callback(nack_receiver_t *nack,
                                          void (*callback)(void *user_data, uint32_t ssrc,
                                                           const uint16_t *seq_nums, int count),
                                          void *user_data);

/**
 * Get receiver statistics
 *
 * @param nack NACK receiver context
 * @param packets_received Output: total packets received (can be NULL)
 * @param packets_lost Output: total packets lost (can be NULL)
 * @param nacks_sent Output: total NACKs sent (can be NULL)
 * @param retransmissions_received Output: retransmissions received (can be NULL)
 * @param missing_count Output: currently missing packets (can be NULL)
 */
CXX_C_API void nack_receiver_get_stats(nack_receiver_t *nack, int64_t *packets_received,
                                       int64_t *packets_lost, int64_t *nacks_sent,
                                       int64_t *retransmissions_received, int *missing_count);

/* =============================================================================
 * NACK Sender (Handles Retransmission Requests)
 * ============================================================================= */

/**
 * Create NACK sender
 *
 * @param ssrc SSRC of the stream
 * @param history_size Number of packets to keep in history
 * @return NACK sender context or NULL on error
 */
CXX_C_API nack_sender_t *nack_sender_create(uint32_t ssrc, size_t history_size);

/**
 * Destroy NACK sender
 */
CXX_C_API void nack_sender_destroy(nack_sender_t *nack);

/**
 * Add packet to history
 *
 * Store packet for potential retransmission
 *
 * @param nack NACK sender context
 * @param seq_num RTP sequence number
 * @param packet Packet data
 * @param len Packet length
 */
CXX_C_API void nack_sender_add_packet(nack_sender_t *nack, uint16_t seq_num, const uint8_t *packet,
                                      size_t len);

/**
 * Process NACK request
 *
 * Retransmits requested packets
 *
 * @param nack NACK sender context
 * @param seq_nums Sequence numbers to retransmit
 * @param count Number of sequence numbers
 * @return Number of packets retransmitted
 */
CXX_C_API int nack_sender_process_nack(nack_sender_t *nack, const uint16_t *seq_nums, int count);

/**
 * Set retransmission callback
 *
 * Called when packets need to be retransmitted
 *
 * @param nack NACK sender context
 * @param callback Callback function
 * @param user_data User data passed to callback
 */
CXX_C_API void nack_sender_set_callback(nack_sender_t *nack,
                                        void (*callback)(void *user_data, const uint8_t *packet,
                                                         size_t len),
                                        void *user_data);

/**
 * Get sender statistics
 *
 * @param nack NACK sender context
 * @param nacks_received Output: total NACKs received (can be NULL)
 * @param packets_retransmitted Output: packets retransmitted (can be NULL)
 * @param retransmit_failures Output: retransmit failures (can be NULL)
 */
CXX_C_API void nack_sender_get_stats(nack_sender_t *nack, int64_t *nacks_received,
                                     int64_t *packets_retransmitted, int64_t *retransmit_failures);

/* =============================================================================
 * RTCP NACK Packet Building/Parsing (RFC 4585)
 * ============================================================================= */

/**
 * Build RTCP NACK packet
 *
 * @param sender_ssrc SSRC of packet sender
 * @param media_ssrc SSRC of media source
 * @param seq_nums Sequence numbers to NACK
 * @param count Number of sequence numbers
 * @param buffer Output buffer
 * @param len Input: buffer size, Output: packet size
 * @return 0 on success, -1 on error
 */
CXX_C_API int nack_build_rtcp(uint32_t sender_ssrc, uint32_t media_ssrc, const uint16_t *seq_nums,
                              int count, uint8_t *buffer, size_t *len);

/**
 * Parse RTCP NACK packet
 *
 * @param buffer RTCP packet data
 * @param len Packet length
 * @param sender_ssrc Output: SSRC of packet sender
 * @param media_ssrc Output: SSRC of media source
 * @param seq_nums Output: sequence numbers
 * @param count Output: number of sequence numbers
 * @param max_count Maximum sequence numbers to parse
 * @return 0 on success, -1 on error
 */
CXX_C_API int nack_parse_rtcp(const uint8_t *buffer, size_t len, uint32_t *sender_ssrc,
                              uint32_t *media_ssrc, uint16_t *seq_nums, int *count, int max_count);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_NACK_H */
