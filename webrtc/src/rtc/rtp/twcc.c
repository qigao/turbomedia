/**
 * Transport-Wide Congestion Control (TWCC) Implementation
 * RFC 8888 - RTP Control Protocol (RTCP) Feedback for Congestion Control
 */
#include "turbo_rtp.h"
#include <platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RTCP packet type for Transport Feedback */
#define RTCP_RTPFB_TWCC 15 /* Transport-wide CC feedback */

/* TWCC constants */
#define TWCC_DELTA_SCALE_US 250       /* Delta scale: 250 microseconds */
#define TWCC_REF_TIME_SCALE_US 64000  /* Reference time: 64ms */
#define TWCC_FEEDBACK_INTERVAL_MS 100 /* Send feedback every 100ms */
#define TWCC_MAX_HISTORY 1000         /* Track last 1000 packets */

/* Bandwidth estimation constants (Google Congestion Control) */
#define BWE_INITIAL_BITRATE 300000 /* 300 kbps initial */
#define BWE_MIN_BITRATE 30000      /* 30 kbps minimum */
#define BWE_MAX_BITRATE 10000000   /* 10 Mbps maximum */
#define BWE_INCREASE_FACTOR 1.08   /* 8% increase when stable */
#define BWE_DECREASE_FACTOR 0.85   /* 15% decrease on congestion */

/**
 * Packet send record (sender side)
 */
typedef struct {
  uint16_t twcc_seq;
  uint64_t send_time_us;
  size_t size;
  int acked;
} twcc_send_record_t;

/**
 * TWCC Tracker (sender side)
 */
struct twcc_tracker_s {
  uint16_t next_seq; /* Next transport-wide seq */
  twcc_send_record_t history[TWCC_MAX_HISTORY];
  int history_count;

  /* Bandwidth estimation state */
  uint32_t current_bitrate_bps;
  uint64_t last_feedback_time_us;
  uint64_t last_increase_time_us;
  int consecutive_stable_rounds;
};

/**
 * Packet receive record (receiver side)
 */
typedef struct {
  uint16_t twcc_seq;
  uint64_t arrival_time_us;
} twcc_recv_record_t;

/**
 * TWCC Receiver (receiver side)
 */
struct twcc_receiver_s {
  uint32_t ssrc;
  uint16_t expected_seq;
  twcc_recv_record_t packets[RTCP_TWCC_MAX_PACKETS];
  int packet_count;
  uint64_t last_feedback_time_us;
  uint8_t fb_pkt_count;
};

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

static uint64_t get_time_us(void) { return turbo_monotonic_ms() * 1000ULL; }

static int seq_newer(uint16_t s1, uint16_t s2) { return ((int16_t)(s1 - s2)) > 0; }

static uint16_t read_u16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

static void write_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFF);
}

static void write_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)(v & 0xFF);
}

static int64_t div_round_nearest_i64(int64_t value, int64_t divisor) {
  if (value >= 0) {
    return (value + divisor / 2) / divisor;
  }
  return -((-value + divisor / 2) / divisor);
}

static int twcc_can_use_one_bit_vector(const uint8_t *symbols, int count) {
  for (int i = 0; i < count; ++i) {
    if (symbols[i] != TWCC_SYMBOL_NOT_RECEIVED && symbols[i] != TWCC_SYMBOL_SMALL_DELTA) {
      return 0;
    }
  }
  return 1;
}

static size_t twcc_build_status_chunks(const uint8_t *symbols, int status_count, uint16_t *chunks,
                                       size_t max_chunks) {
  size_t chunk_count = 0;

  for (int i = 0; i < status_count;) {
    int run_len = 1;
    while (i + run_len < status_count && run_len < 0x1FFF && symbols[i + run_len] == symbols[i]) {
      ++run_len;
    }

    if (run_len >= 7) {
      if (chunk_count >= max_chunks) return 0;
      chunks[chunk_count++] = (uint16_t)(((uint16_t)symbols[i] << 13) | (uint16_t)run_len);
      i += run_len;
      continue;
    }

    int remaining = status_count - i;
    int vector_len = remaining < 14 ? remaining : 14;
    if (twcc_can_use_one_bit_vector(symbols + i, vector_len)) {
      uint16_t chunk = 0x8000;
      for (int j = 0; j < vector_len; ++j) {
        if (symbols[i + j] == TWCC_SYMBOL_SMALL_DELTA) {
          chunk |= (uint16_t)(1u << (13 - j));
        }
      }
      if (chunk_count >= max_chunks) return 0;
      chunks[chunk_count++] = chunk;
      i += vector_len;
    } else {
      vector_len = remaining < 7 ? remaining : 7;
      uint16_t chunk = 0xC000;
      for (int j = 0; j < vector_len; ++j) {
        chunk |= (uint16_t)((uint16_t)symbols[i + j] << (12 - j * 2));
      }
      if (chunk_count >= max_chunks) return 0;
      chunks[chunk_count++] = chunk;
      i += vector_len;
    }
  }

  return chunk_count;
}

/* =============================================================================
 * TWCC Tracker (Sender Side)
 * ============================================================================= */

twcc_tracker_t *twcc_tracker_create(void) {
  twcc_tracker_t *tracker = calloc(1, sizeof(twcc_tracker_t));
  if (!tracker) return NULL;

  tracker->next_seq = 0;
  tracker->current_bitrate_bps = BWE_INITIAL_BITRATE;
  tracker->last_feedback_time_us = get_time_us();
  tracker->last_increase_time_us = tracker->last_feedback_time_us;

  return tracker;
}

void twcc_tracker_destroy(twcc_tracker_t *tracker) {
  if (tracker) {
    free(tracker);
  }
}

void twcc_tracker_set_bitrate(twcc_tracker_t *tracker, uint32_t bitrate_bps) {
  if (!tracker || bitrate_bps == 0) {
    return;
  }

  if (bitrate_bps < BWE_MIN_BITRATE) {
    bitrate_bps = BWE_MIN_BITRATE;
  }
  if (bitrate_bps > BWE_MAX_BITRATE) {
    bitrate_bps = BWE_MAX_BITRATE;
  }

  tracker->current_bitrate_bps = bitrate_bps;
}

uint16_t twcc_tracker_register_packet(twcc_tracker_t *tracker, size_t size, uint64_t send_time_us) {
  if (!tracker) return 0;

  uint16_t seq = tracker->next_seq++;

  /* Store in circular buffer */
  int idx = seq % TWCC_MAX_HISTORY;
  tracker->history[idx].twcc_seq = seq;
  tracker->history[idx].send_time_us = send_time_us;
  tracker->history[idx].size = size;
  tracker->history[idx].acked = 0;

  if (tracker->history_count < TWCC_MAX_HISTORY) {
    tracker->history_count++;
  }

  return seq;
}

int twcc_tracker_process_feedback(twcc_tracker_t *tracker, const rtcp_twcc_t *twcc,
                                  twcc_bwe_result_t *result) {
  if (!tracker || !twcc || !result) return -1;

  uint64_t now_us = get_time_us();
  tracker->last_feedback_time_us = now_us;

  /* Calculate packet loss and RTT */
  int received_count = 0;
  int lost_count = 0;
  uint64_t total_rtt_us = 0;
  int rtt_samples = 0;

  for (int i = 0; i < twcc->num_packets; i++) {
    const twcc_packet_status_t *pkt = &twcc->packets[i];
    int idx = pkt->seq % TWCC_MAX_HISTORY;

    if (tracker->history[idx].twcc_seq == pkt->seq) {
      if (pkt->received) {
        tracker->history[idx].acked = 1;
        received_count++;

        /* Calculate RTT */
        uint64_t rtt_us = pkt->arrival_time_us - tracker->history[idx].send_time_us;
        if (rtt_us > 0 && rtt_us < 10000000) { /* Sanity check: < 10 seconds */
          total_rtt_us += rtt_us;
          rtt_samples++;
        }
      } else {
        lost_count++;
      }
    }
  }

  int total_packets = received_count + lost_count;
  float loss_ratio = total_packets > 0 ? (float)lost_count / total_packets : 0.0f;
  uint32_t rtt_ms = rtt_samples > 0 ? (uint32_t)(total_rtt_us / rtt_samples / 1000) : 0;

  /* Simple bandwidth estimation (Google Congestion Control inspired) */
  int congestion_detected = 0;

  if (loss_ratio > 0.1f) {
    /* High loss: decrease bitrate aggressively */
    tracker->current_bitrate_bps = (uint32_t)(tracker->current_bitrate_bps * BWE_DECREASE_FACTOR);
    congestion_detected = 1;
    tracker->consecutive_stable_rounds = 0;
  } else if (loss_ratio > 0.02f) {
    /* Moderate loss: slight decrease */
    tracker->current_bitrate_bps = (uint32_t)(tracker->current_bitrate_bps * 0.95f);
    congestion_detected = 1;
    tracker->consecutive_stable_rounds = 0;
  } else {
    /* Low/no loss: increase bitrate gradually */
    tracker->consecutive_stable_rounds++;

    /* Only increase every 1 second to avoid oscillation */
    if (now_us - tracker->last_increase_time_us > 1000000) {
      tracker->current_bitrate_bps = (uint32_t)(tracker->current_bitrate_bps * BWE_INCREASE_FACTOR);
      tracker->last_increase_time_us = now_us;
    }
  }

  /* Clamp bitrate */
  if (tracker->current_bitrate_bps < BWE_MIN_BITRATE) {
    tracker->current_bitrate_bps = BWE_MIN_BITRATE;
  }
  if (tracker->current_bitrate_bps > BWE_MAX_BITRATE) {
    tracker->current_bitrate_bps = BWE_MAX_BITRATE;
  }

  /* Fill result */
  result->target_bitrate_bps = tracker->current_bitrate_bps;
  result->estimated_bw_bps = tracker->current_bitrate_bps;
  result->packet_loss_ratio = loss_ratio;
  result->rtt_ms = rtt_ms;
  result->congestion_detected = congestion_detected;

  return 0;
}

/* =============================================================================
 * TWCC Receiver (Receiver Side)
 * ============================================================================= */

twcc_receiver_t *twcc_receiver_create(uint32_t ssrc) {
  twcc_receiver_t *receiver = calloc(1, sizeof(twcc_receiver_t));
  if (!receiver) return NULL;

  receiver->ssrc = ssrc;
  receiver->expected_seq = 0;
  receiver->last_feedback_time_us = get_time_us();
  receiver->fb_pkt_count = 0;

  return receiver;
}

void twcc_receiver_destroy(twcc_receiver_t *receiver) {
  if (receiver) {
    free(receiver);
  }
}

int twcc_receiver_register_packet(twcc_receiver_t *receiver, uint16_t twcc_seq,
                                  uint64_t arrival_time_us) {
  if (!receiver) return -1;

  /* Handle out-of-order packets */
  if (receiver->packet_count > 0) {
    uint16_t last_seq = receiver->packets[receiver->packet_count - 1].twcc_seq;
    if (!seq_newer(twcc_seq, last_seq)) {
      /* Out of order - insert in correct position */
      int insert_pos = receiver->packet_count;
      for (int i = receiver->packet_count - 1; i >= 0; i--) {
        if (seq_newer(twcc_seq, receiver->packets[i].twcc_seq)) {
          insert_pos = i + 1;
          break;
        }
        insert_pos = i;
      }

      /* Shift packets */
      if (receiver->packet_count < RTCP_TWCC_MAX_PACKETS) {
        memmove(&receiver->packets[insert_pos + 1], &receiver->packets[insert_pos],
                (receiver->packet_count - insert_pos) * sizeof(twcc_recv_record_t));
        receiver->packets[insert_pos].twcc_seq = twcc_seq;
        receiver->packets[insert_pos].arrival_time_us = arrival_time_us;
        receiver->packet_count++;
      }
      return 0;
    }
  }

  /* Append packet */
  if (receiver->packet_count < RTCP_TWCC_MAX_PACKETS) {
    receiver->packets[receiver->packet_count].twcc_seq = twcc_seq;
    receiver->packets[receiver->packet_count].arrival_time_us = arrival_time_us;
    receiver->packet_count++;
  }

  return 0;
}

int twcc_receiver_generate_feedback(twcc_receiver_t *receiver, rtcp_twcc_t *twcc) {
  if (!receiver || !twcc) return -1;

  uint64_t now_us = get_time_us();

  /* Check if it's time to send feedback */
  if (receiver->packet_count < 10 &&
      (now_us - receiver->last_feedback_time_us) < (TWCC_FEEDBACK_INTERVAL_MS * 1000)) {
    return 0; /* Not ready */
  }

  if (receiver->packet_count == 0) {
    return 0; /* No packets to report */
  }

  /* Build feedback */
  memset(twcc, 0, sizeof(rtcp_twcc_t));
  twcc->sender_ssrc = receiver->ssrc;
  twcc->media_ssrc = 0; /* Will be filled by caller */
  twcc->base_seq = receiver->packets[0].twcc_seq;
  twcc->packet_count = receiver->packet_count;
  twcc->reference_time = (uint32_t)(receiver->packets[0].arrival_time_us / TWCC_REF_TIME_SCALE_US);
  twcc->fb_pkt_count = receiver->fb_pkt_count++;

  /* Copy packet statuses */
  uint16_t expected_seq = twcc->base_seq;
  int out_idx = 0;

  for (int i = 0; i < receiver->packet_count && out_idx < RTCP_TWCC_MAX_PACKETS; i++) {
    /* Fill gaps with "not received" */
    while (expected_seq != receiver->packets[i].twcc_seq && out_idx < RTCP_TWCC_MAX_PACKETS) {
      twcc->packets[out_idx].seq = expected_seq;
      twcc->packets[out_idx].received = 0;
      twcc->packets[out_idx].arrival_time_us = 0;
      out_idx++;
      expected_seq++;
    }

    /* Add received packet */
    if (out_idx < RTCP_TWCC_MAX_PACKETS) {
      twcc->packets[out_idx].seq = receiver->packets[i].twcc_seq;
      twcc->packets[out_idx].received = 1;
      twcc->packets[out_idx].arrival_time_us = receiver->packets[i].arrival_time_us;
      out_idx++;
      expected_seq++;
    }
  }

  twcc->num_packets = out_idx;
  twcc->packet_count = (uint16_t)out_idx;

  /* Reset receiver state */
  receiver->packet_count = 0;
  receiver->last_feedback_time_us = now_us;

  return 1; /* Feedback generated */
}

/* =============================================================================
 * RTCP TWCC Serialization
 * ============================================================================= */

int rtcp_compound_add_twcc(rtcp_compound_t *compound, const rtcp_twcc_t *twcc) {
  if (!compound || !twcc || twcc->num_packets == 0) return -1;

  uint8_t *buf = compound->buffer + compound->offset;
  size_t remaining = compound->buffer_len - compound->offset;

  if (remaining < 20) return -1; /* Minimum TWCC size */

  int status_count = twcc->num_packets;
  if (status_count <= 0 || status_count > RTCP_TWCC_MAX_PACKETS) return -1;

  uint8_t symbols[RTCP_TWCC_MAX_PACKETS];
  int16_t deltas[RTCP_TWCC_MAX_PACKETS];
  uint16_t chunks[RTCP_TWCC_MAX_PACKETS];
  memset(symbols, 0, (size_t)status_count);
  memset(deltas, 0, (size_t)status_count * sizeof(deltas[0]));
  memset(chunks, 0, (size_t)status_count * sizeof(chunks[0]));

  size_t delta_size = 0;
  /* Keep the full local reference-time epoch while deriving deltas. Only the
   * wire field wraps to 24 bits; masking here breaks serialization after the
   * monotonic clock crosses the approximately 12.4-day TWCC wrap interval. */
  uint64_t prev_recv_time_us =
      (uint64_t)twcc->reference_time * TWCC_REF_TIME_SCALE_US;

  for (int i = 0; i < status_count; ++i) {
    if (!twcc->packets[i].received) {
      symbols[i] = TWCC_SYMBOL_NOT_RECEIVED;
      continue;
    }

    int64_t delta_us = (int64_t)twcc->packets[i].arrival_time_us - (int64_t)prev_recv_time_us;
    int64_t scaled_delta = div_round_nearest_i64(delta_us, TWCC_DELTA_SCALE_US);
    if (scaled_delta >= 0 && scaled_delta <= 255) {
      symbols[i] = TWCC_SYMBOL_SMALL_DELTA;
      delta_size += 1;
    } else if (scaled_delta >= -32768 && scaled_delta <= 32767) {
      symbols[i] = TWCC_SYMBOL_LARGE_DELTA;
      delta_size += 2;
    } else {
      return -1;
    }

    deltas[i] = (int16_t)scaled_delta;
    prev_recv_time_us = twcc->packets[i].arrival_time_us;
  }

  size_t chunk_count =
      twcc_build_status_chunks(symbols, status_count, chunks, (size_t)status_count);
  if (chunk_count == 0) return -1;

  size_t total_size = 20 + chunk_count * 2 + delta_size;
  size_t padded_size = (total_size + 3) & ~(size_t)3;
  if (remaining < padded_size) return -1;

  memset(buf, 0, padded_size);

  /* RTCP header */
  buf[0] = 0x80 | RTCP_RTPFB_TWCC; /* V=2, P=0, FMT=15 */
  buf[1] = RTCP_RTPFB;
  write_u16(buf + 2, (uint16_t)(padded_size / 4 - 1));

  write_u32(buf + 4, twcc->sender_ssrc);
  write_u32(buf + 8, twcc->media_ssrc);
  write_u16(buf + 12, twcc->base_seq);
  write_u16(buf + 14, (uint16_t)status_count);

  /* Reference time (24-bit) */
  buf[16] = (uint8_t)((twcc->reference_time >> 16) & 0xFF);
  buf[17] = (uint8_t)((twcc->reference_time >> 8) & 0xFF);
  buf[18] = (uint8_t)(twcc->reference_time & 0xFF);
  buf[19] = twcc->fb_pkt_count;

  uint8_t *p = buf + 20;
  for (size_t i = 0; i < chunk_count; ++i) {
    write_u16(p, chunks[i]);
    p += 2;
  }

  for (int i = 0; i < status_count; ++i) {
    if (symbols[i] == TWCC_SYMBOL_SMALL_DELTA) {
      *p++ = (uint8_t)deltas[i];
    } else if (symbols[i] == TWCC_SYMBOL_LARGE_DELTA) {
      write_u16(p, (uint16_t)deltas[i]);
      p += 2;
    }
  }

  compound->offset += padded_size;
  return 0;
}

int rtcp_parse_twcc(const uint8_t *buffer, size_t len, rtcp_twcc_t *twcc) {
  if (!buffer || !twcc || len < 20) return -1;

  /* Parse header */
  uint8_t version = (buffer[0] >> 6) & 0x03;
  uint8_t fmt = buffer[0] & 0x1F;
  if (version != 2 || fmt != RTCP_RTPFB_TWCC || buffer[1] != RTCP_RTPFB) return -1;

  uint16_t rtcp_length = read_u16(buffer + 2);
  size_t packet_len = ((size_t)rtcp_length + 1) * 4;
  if (packet_len < 20 || packet_len > len) return -1;

  memset(twcc, 0, sizeof(rtcp_twcc_t));

  twcc->sender_ssrc = ((uint32_t)buffer[4] << 24) | ((uint32_t)buffer[5] << 16) |
                      ((uint32_t)buffer[6] << 8) | buffer[7];
  twcc->media_ssrc = ((uint32_t)buffer[8] << 24) | ((uint32_t)buffer[9] << 16) |
                     ((uint32_t)buffer[10] << 8) | buffer[11];

  /* Base sequence */
  twcc->base_seq = read_u16(buffer + 12);

  /* Packet status count */
  twcc->packet_count = read_u16(buffer + 14);
  if (twcc->packet_count > RTCP_TWCC_MAX_PACKETS) return -1;

  /* Reference time */
  twcc->reference_time = ((uint32_t)buffer[16] << 16) | ((uint32_t)buffer[17] << 8) | buffer[18];

  /* Feedback packet count */
  twcc->fb_pkt_count = buffer[19];

  uint8_t symbols[RTCP_TWCC_MAX_PACKETS];
  memset(symbols, 0, twcc->packet_count);

  size_t offset = 20;
  uint16_t status_idx = 0;

  while (status_idx < twcc->packet_count) {
    if (offset + 2 > packet_len) return -1;

    uint16_t chunk = read_u16(buffer + offset);
    offset += 2;

    if ((chunk & 0x8000) == 0) {
      uint8_t symbol = (uint8_t)((chunk >> 13) & 0x03);
      uint16_t run_len = (uint16_t)(chunk & 0x1FFF);
      if (symbol == TWCC_SYMBOL_RESERVED || run_len == 0) return -1;
      if ((uint32_t)status_idx + run_len > twcc->packet_count) return -1;

      for (uint16_t i = 0; i < run_len; ++i) {
        symbols[status_idx] = symbol;
        twcc->packets[status_idx].seq = (uint16_t)(twcc->base_seq + status_idx);
        twcc->packets[status_idx].received = (symbol != TWCC_SYMBOL_NOT_RECEIVED);
        ++status_idx;
      }
    } else {
      int two_bit_symbols = (chunk & 0x4000) != 0;
      int symbols_in_chunk = two_bit_symbols ? 7 : 14;

      for (int i = 0; i < symbols_in_chunk && status_idx < twcc->packet_count; ++i) {
        uint8_t symbol;
        if (two_bit_symbols) {
          int shift = 12 - i * 2;
          symbol = (uint8_t)((chunk >> shift) & 0x03);
        } else {
          int shift = 13 - i;
          symbol = (uint8_t)(((chunk >> shift) & 0x01) ? TWCC_SYMBOL_SMALL_DELTA
                                                       : TWCC_SYMBOL_NOT_RECEIVED);
        }

        if (symbol == TWCC_SYMBOL_RESERVED) return -1;
        symbols[status_idx] = symbol;
        twcc->packets[status_idx].seq = (uint16_t)(twcc->base_seq + status_idx);
        twcc->packets[status_idx].received = (symbol != TWCC_SYMBOL_NOT_RECEIVED);
        ++status_idx;
      }
    }
  }

  uint64_t prev_recv_time_us =
      (uint64_t)(twcc->reference_time & 0x00FFFFFF) * TWCC_REF_TIME_SCALE_US;
  for (uint16_t i = 0; i < twcc->packet_count; ++i) {
    if (symbols[i] == TWCC_SYMBOL_NOT_RECEIVED) {
      twcc->packets[i].arrival_time_us = 0;
      continue;
    }

    int16_t delta = 0;
    if (symbols[i] == TWCC_SYMBOL_SMALL_DELTA) {
      if (offset + 1 > packet_len) return -1;
      delta = buffer[offset++];
    } else {
      if (offset + 2 > packet_len) return -1;
      delta = (int16_t)read_u16(buffer + offset);
      offset += 2;
    }

    int64_t arrival_time_us = (int64_t)prev_recv_time_us + (int64_t)delta * TWCC_DELTA_SCALE_US;
    if (arrival_time_us < 0) return -1;

    twcc->packets[i].arrival_time_us = (uint64_t)arrival_time_us;
    prev_recv_time_us = twcc->packets[i].arrival_time_us;
  }

  twcc->num_packets = twcc->packet_count;

  return 0;
}
