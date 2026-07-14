/**
 * Jitter Buffer Implementation
 *
 * Adaptive jitter buffer for smooth RTP packet playout
 */
#include "jitter_buffer.h"
#include <stdlib.h>
#include <string.h>

jitter_buffer_t *jitter_buffer_create(uint32_t clock_rate, uint32_t target_delay_ms,
                                      size_t slot_size) {
  jitter_buffer_t *jb = (jitter_buffer_t *)calloc(1, sizeof(jitter_buffer_t));
  if (!jb) return NULL;

  /* Allocate contiguous data buffer */
  jb->data_buffer = (uint8_t *)calloc(JITTER_BUFFER_SIZE, slot_size);
  if (!jb->data_buffer) {
    free(jb);
    return NULL;
  }

  jb->slot_size = slot_size;
  jb->clock_rate = clock_rate;
  jb->target_delay_ms = target_delay_ms;
  jb->min_delay_ms = 10;
  jb->max_delay_ms = 500;

  /* Initialize slots */
  for (int i = 0; i < JITTER_BUFFER_SIZE; i++) {
    jb->slots[i].data = jb->data_buffer + (i * slot_size);
    jb->slots[i].valid = 0;
  }

  return jb;
}

void jitter_buffer_destroy(jitter_buffer_t *jb) {
  if (!jb) return;
  if (jb->data_buffer) {
    free(jb->data_buffer);
  }
  free(jb);
}

void jitter_buffer_reset(jitter_buffer_t *jb) {
  if (!jb) return;

  for (int i = 0; i < JITTER_BUFFER_SIZE; i++) {
    jb->slots[i].valid = 0;
  }

  jb->initialized = 0;
  jb->next_seq_out = 0;
  jb->current_jitter = 0;
  jb->packets_in = 0;
  jb->packets_out = 0;
  jb->packets_lost = 0;
  jb->packets_late = 0;
  jb->packets_duplicate = 0;
}

static int seq_diff(uint16_t s1, uint16_t s2) { return (int16_t)(s1 - s2); }

int jitter_buffer_put(jitter_buffer_t *jb, const rtp_packet_t *pkt, uint64_t now_ms) {
  if (!jb || !pkt) return -1;

  uint16_t seq = pkt->header.sequence;
  uint32_t ts = pkt->header.timestamp;

  if (!jb->initialized) {
    jb->next_seq_out = seq;
    jb->initialized = 1;
  }

  /* Check if packet is too old */
  int diff = seq_diff(seq, jb->next_seq_out);
  if (diff < -JITTER_BUFFER_SIZE / 2) {
    jb->packets_late++;
    return -1; /* Too late */
  }

  /* Check if packet is too far ahead */
  if (diff >= JITTER_BUFFER_SIZE / 2) {
    jb->packets_late++;
    return -1; /* Too far ahead */
  }

  /* Calculate slot index */
  int slot_idx = seq & JITTER_BUFFER_MASK;
  jitter_slot_t *slot = &jb->slots[slot_idx];

  /* Check for duplicate */
  if (slot->valid && slot->sequence == seq) {
    jb->packets_duplicate++;
    return 1; /* Duplicate */
  }

  /* Store packet */
  if (pkt->payload_len > jb->slot_size) {
    return -1; /* Packet too large */
  }

  memcpy(slot->data, pkt->payload, pkt->payload_len);
  slot->len = pkt->payload_len;
  slot->sequence = seq;
  slot->timestamp = ts;
  slot->arrival_time = now_ms;
  slot->marker = pkt->header.marker ? 1 : 0;
  slot->valid = 1;

  jb->packets_in++;

  /* Update jitter estimate (simplified) */
  /* Full RFC 3550 jitter calculation should consider timestamp vs arrival */

  return 0;
}

int jitter_buffer_get_ex(jitter_buffer_t *jb, uint8_t *data, size_t max_len, size_t *len,
                         uint32_t *timestamp, int *marker, uint64_t now_ms) {
  if (!jb || !data || !len) return -1;
  if (!jb->initialized) return 0;

  int slot_idx = jb->next_seq_out & JITTER_BUFFER_MASK;
  jitter_slot_t *slot = &jb->slots[slot_idx];

  if (!slot->valid) {
    /* Check if we should skip (assume lost) */
    /* Look ahead to see if we have future packets */
    int have_future = 0;
    for (int i = 1; i < JITTER_BUFFER_SIZE / 4; i++) {
      int future_idx = (jb->next_seq_out + i) & JITTER_BUFFER_MASK;
      if (jb->slots[future_idx].valid) {
        have_future = 1;
        break;
      }
    }

    if (have_future) {
      /* Mark as lost and advance */
      jb->packets_lost++;
      jb->next_seq_out++;
      return -1; /* Packet lost */
    }

    return 0; /* Not ready yet */
  }

  /* Check if enough delay has accumulated */
  uint64_t delay = now_ms - slot->arrival_time;
  if (delay < jb->target_delay_ms) {
    return 0; /* Not ready yet */
  }

  /* Copy data out */
  if (slot->len > max_len) {
    return -1; /* Buffer too small */
  }

  memcpy(data, slot->data, slot->len);
  *len = slot->len;
  if (timestamp) {
    *timestamp = slot->timestamp;
  }
  if (marker) {
    *marker = slot->marker;
  }

  /* Mark slot as empty */
  slot->valid = 0;
  jb->next_seq_out++;
  jb->packets_out++;

  return 1; /* Packet available */
}

int jitter_buffer_get(jitter_buffer_t *jb, uint8_t *data, size_t max_len, size_t *len,
                      uint32_t *timestamp, uint64_t now_ms) {
  return jitter_buffer_get_ex(jb, data, max_len, len, timestamp, NULL, now_ms);
}

int jitter_buffer_peek(jitter_buffer_t *jb, uint32_t *timestamp) {
  if (!jb || !jb->initialized) return 0;

  int slot_idx = jb->next_seq_out & JITTER_BUFFER_MASK;
  jitter_slot_t *slot = &jb->slots[slot_idx];

  if (!slot->valid) return 0;

  if (timestamp) {
    *timestamp = slot->timestamp;
  }
  return 1;
}

uint32_t jitter_buffer_get_delay(const jitter_buffer_t *jb) { return jb ? jb->target_delay_ms : 0; }

uint32_t jitter_buffer_get_jitter(const jitter_buffer_t *jb) {
  if (!jb || jb->clock_rate == 0) return 0;
  return (jb->current_jitter * 1000) / jb->clock_rate;
}

int jitter_buffer_get_count(const jitter_buffer_t *jb) {
  if (!jb) return 0;

  int count = 0;
  for (int i = 0; i < JITTER_BUFFER_SIZE; i++) {
    if (jb->slots[i].valid) count++;
  }
  return count;
}

int jitter_buffer_is_empty(const jitter_buffer_t *jb) { return jitter_buffer_get_count(jb) == 0; }

int jitter_buffer_get_missing(jitter_buffer_t *jb, uint16_t *missing, int max_count) {
  if (!jb || !missing || !jb->initialized) return 0;

  int count = 0;

  /* Scan from next_seq_out looking for gaps */
  for (int i = 0; i < JITTER_BUFFER_SIZE / 4 && count < max_count; i++) {
    uint16_t seq = jb->next_seq_out + i;
    int slot_idx = seq & JITTER_BUFFER_MASK;

    if (!jb->slots[slot_idx].valid) {
      /* Check if there's a valid packet after this one */
      int has_later = 0;
      for (int j = i + 1; j < JITTER_BUFFER_SIZE / 4; j++) {
        int later_idx = (jb->next_seq_out + j) & JITTER_BUFFER_MASK;
        if (jb->slots[later_idx].valid) {
          has_later = 1;
          break;
        }
      }

      if (has_later) {
        missing[count++] = seq;
      }
    }
  }

  return count;
}

void jitter_buffer_set_delay_bounds(jitter_buffer_t *jb, uint32_t min_ms, uint32_t max_ms) {
  if (!jb) return;
  jb->min_delay_ms = min_ms;
  jb->max_delay_ms = max_ms;

  if (jb->target_delay_ms < min_ms) jb->target_delay_ms = min_ms;
  if (jb->target_delay_ms > max_ms) jb->target_delay_ms = max_ms;
}
