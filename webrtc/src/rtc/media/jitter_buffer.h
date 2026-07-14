/**
 * Jitter Buffer Implementation
 *
 * Adaptive jitter buffer for RTP packet reordering and playout timing
 */
#ifndef JITTER_BUFFER_H
#define JITTER_BUFFER_H

#include "turbo_rtp.h"
#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define JITTER_BUFFER_SIZE 256 /* Must be power of 2 */
#define JITTER_BUFFER_MASK (JITTER_BUFFER_SIZE - 1)

/* =============================================================================
 * Types
 * ============================================================================= */

typedef struct {
  uint8_t *data;
  size_t len;
  uint16_t sequence;
  uint32_t timestamp;
  uint64_t arrival_time;
  int marker;
  int valid;
} jitter_slot_t;

typedef struct jitter_buffer_s {
  jitter_slot_t slots[JITTER_BUFFER_SIZE];
  uint8_t *data_buffer; /* Pre-allocated data storage */
  size_t slot_size;     /* Max size per slot */

  /* State */
  uint16_t next_seq_out; /* Next sequence to output */
  int initialized;
  uint32_t clock_rate;

  /* Adaptive jitter estimation */
  uint32_t target_delay_ms; /* Target playout delay */
  uint32_t min_delay_ms;
  uint32_t max_delay_ms;
  uint32_t current_jitter; /* Estimated jitter in timestamp units */

  /* Statistics */
  uint64_t packets_in;
  uint64_t packets_out;
  uint64_t packets_lost;
  uint64_t packets_late;
  uint64_t packets_duplicate;
} jitter_buffer_t;

/* =============================================================================
 * Functions
 * ============================================================================= */

/**
 * Create jitter buffer
 *
 * @param clock_rate    RTP clock rate (e.g., 48000 for audio, 90000 for video)
 * @param target_delay  Target delay in milliseconds
 * @param slot_size     Maximum size of each packet
 * @return              Jitter buffer, or NULL on error
 */
CXX_C_API jitter_buffer_t *jitter_buffer_create(uint32_t clock_rate, uint32_t target_delay_ms,
                                                size_t slot_size);

/**
 * Destroy jitter buffer
 */
CXX_C_API void jitter_buffer_destroy(jitter_buffer_t *jb);

/**
 * Reset jitter buffer state
 */
CXX_C_API void jitter_buffer_reset(jitter_buffer_t *jb);

/**
 * Insert packet into jitter buffer
 *
 * @param jb        Jitter buffer
 * @param pkt       RTP packet
 * @param now_ms    Current time in milliseconds
 * @return          0 on success, -1 on error, 1 if duplicate
 */
CXX_C_API int jitter_buffer_put(jitter_buffer_t *jb, const rtp_packet_t *pkt, uint64_t now_ms);

/**
 * Get next packet from jitter buffer
 *
 * @param jb        Jitter buffer
 * @param data      Output buffer for packet data
 * @param max_len   Output buffer size
 * @param len       Output: actual data length
 * @param timestamp Output: RTP timestamp
 * @param now_ms    Current time in milliseconds
 * @return          1 if packet available, 0 if not ready, -1 if lost
 */
CXX_C_API int jitter_buffer_get(jitter_buffer_t *jb, uint8_t *data, size_t max_len, size_t *len,
                                uint32_t *timestamp, uint64_t now_ms);

/**
 * Get next packet from jitter buffer and also return the RTP marker bit.
 *
 * Same semantics
 * as jitter_buffer_get().
 */
CXX_C_API int jitter_buffer_get_ex(jitter_buffer_t *jb, uint8_t *data, size_t max_len, size_t *len,
                                   uint32_t *timestamp, int *marker, uint64_t now_ms);

/**
 * Peek at next packet timestamp without removing
 *
 * @param jb        Jitter buffer
 * @param timestamp Output: next packet timestamp
 * @return          1 if packet available, 0 if empty
 */
CXX_C_API int jitter_buffer_peek(jitter_buffer_t *jb, uint32_t *timestamp);

/**
 * Get current delay estimate in milliseconds
 */
CXX_C_API uint32_t jitter_buffer_get_delay(const jitter_buffer_t *jb);

/**
 * Get estimated jitter in milliseconds
 */
CXX_C_API uint32_t jitter_buffer_get_jitter(const jitter_buffer_t *jb);

/**
 * Get number of packets currently buffered
 */
CXX_C_API int jitter_buffer_get_count(const jitter_buffer_t *jb);

/**
 * Check if jitter buffer is empty
 */
CXX_C_API int jitter_buffer_is_empty(const jitter_buffer_t *jb);

/**
 * Get missing sequence numbers for NACK
 *
 * @param jb        Jitter buffer
 * @param missing   Output array of missing sequence numbers
 * @param max_count Maximum sequences to return
 * @return          Number of missing sequences
 */
CXX_C_API int jitter_buffer_get_missing(jitter_buffer_t *jb, uint16_t *missing, int max_count);

/**
 * Set adaptive delay bounds
 */
CXX_C_API void jitter_buffer_set_delay_bounds(jitter_buffer_t *jb, uint32_t min_ms,
                                              uint32_t max_ms);

#ifdef __cplusplus
}
#endif

#endif /* JITTER_BUFFER_H */
