/**
 * NACK (Negative Acknowledgement) Implementation
 *
 * Handles packet loss detection and retransmission requests
 * Essential for reliable video streaming over lossy networks
 *
 * Features:
 * - Sequence number gap detection
 * - NACK generation (RFC 4585)
 * - Retransmission handling
 * - Duplicate detection
 * - Timeout management
 */
#include "turbo_nack.h"
#include "turbo_rtp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * NACK Configuration
 * ============================================================================= */

#define NACK_MAX_PACKET_AGE_MS 1000  /* Don't NACK packets older than 1s */
#define NACK_MAX_RETRIES 3           /* Max retransmission attempts */
#define NACK_RETRY_INTERVAL_MS 100   /* Wait 100ms before retry */
#define NACK_SEND_INTERVAL_MS 50     /* Send NACK every 50ms */
#define NACK_MAX_MISSING_PACKETS 100 /* Track up to 100 missing packets */

/* =============================================================================
 * NACK Receiver (Detects Loss and Requests Retransmission)
 * ============================================================================= */

typedef struct {
  uint16_t seq_num;
  int64_t first_nack_time;
  int64_t last_nack_time;
  int retry_count;
} nack_entry_t;

struct nack_receiver_t {
  uint32_t ssrc;

  /* Sequence tracking */
  uint16_t highest_seq_received;
  int initialized;

  /* Missing packets */
  nack_entry_t missing[NACK_MAX_MISSING_PACKETS];
  int missing_count;

  /* Statistics */
  int64_t packets_received;
  int64_t packets_lost;
  int64_t nacks_sent;
  int64_t retransmissions_received;

  /* Callbacks */
  void (*on_nack)(void *user_data, uint32_t ssrc, const uint16_t *seq_nums, int count);
  void *user_data;
};

nack_receiver_t *nack_receiver_create(uint32_t ssrc) {
  nack_receiver_t *nack = (nack_receiver_t *)calloc(1, sizeof(nack_receiver_t));
  if (!nack) return NULL;

  nack->ssrc = ssrc;
  nack->initialized = 0;
  nack->missing_count = 0;

  return nack;
}

void nack_receiver_destroy(nack_receiver_t *nack) { free(nack); }

/* Calculate sequence number difference considering wraparound */
static int seq_diff(uint16_t a, uint16_t b) {
  int diff = (int)a - (int)b;
  if (diff > 32768) {
    diff -= 65536;
  } else if (diff < -32768) {
    diff += 65536;
  }
  return diff;
}

/* Add missing packet to tracking list */
static void add_missing_packet(nack_receiver_t *nack, uint16_t seq_num, int64_t now) {
  /* Check if already tracking */
  for (int i = 0; i < nack->missing_count; i++) {
    if (nack->missing[i].seq_num == seq_num) {
      return; /* Already tracking */
    }
  }

  /* Add new entry */
  if (nack->missing_count < NACK_MAX_MISSING_PACKETS) {
    nack->missing[nack->missing_count].seq_num = seq_num;
    nack->missing[nack->missing_count].first_nack_time = now;
    nack->missing[nack->missing_count].last_nack_time = 0;
    nack->missing[nack->missing_count].retry_count = 0;
    nack->missing_count++;
  }
}

/* Remove packet from missing list */
static void remove_missing_packet(nack_receiver_t *nack, uint16_t seq_num) {
  for (int i = 0; i < nack->missing_count; i++) {
    if (nack->missing[i].seq_num == seq_num) {
      /* Shift remaining entries */
      memmove(&nack->missing[i], &nack->missing[i + 1],
              (nack->missing_count - i - 1) * sizeof(nack_entry_t));
      nack->missing_count--;
      nack->retransmissions_received++;
      return;
    }
  }
}

void nack_receiver_process_packet(nack_receiver_t *nack, uint16_t seq_num, int64_t now) {
  if (!nack) return;

  /* Initialize on first packet */
  if (!nack->initialized) {
    nack->highest_seq_received = seq_num;
    nack->initialized = 1;
    nack->packets_received = 1;
    return;
  }

  /* Check if this is a retransmission */
  if (seq_diff(seq_num, nack->highest_seq_received) <= 0) {
    /* Old packet - might be retransmission */
    remove_missing_packet(nack, seq_num);
    nack->packets_received++;
    return;
  }

  /* New packet - check for gaps */
  uint16_t expected = nack->highest_seq_received + 1;
  int gap = seq_diff(seq_num, expected);

  if (gap > 0) {
    /* Gap detected - add missing packets */
    nack->packets_lost += gap;

    for (int i = 0; i < gap && i < NACK_MAX_MISSING_PACKETS; i++) {
      uint16_t missing_seq = (uint16_t)(expected + i);
      add_missing_packet(nack, missing_seq, now);
    }
  }

  /* Update highest sequence */
  nack->highest_seq_received = seq_num;
  nack->packets_received++;
}

int nack_receiver_get_nacks(nack_receiver_t *nack, uint16_t *seq_nums, int max_count, int64_t now) {
  if (!nack || !seq_nums || max_count == 0) return 0;

  int count = 0;

  for (int i = 0; i < nack->missing_count && count < max_count; i++) {
    nack_entry_t *entry = &nack->missing[i];

    /* Check if packet is too old */
    if (now - entry->first_nack_time > NACK_MAX_PACKET_AGE_MS * 1000) {
      /* Too old - give up */
      continue;
    }

    /* Check if we've retried too many times */
    if (entry->retry_count >= NACK_MAX_RETRIES) {
      continue;
    }

    /* Check if enough time has passed since last NACK */
    if (entry->last_nack_time > 0 && now - entry->last_nack_time < NACK_RETRY_INTERVAL_MS * 1000) {
      continue;
    }

    /* Add to NACK list */
    seq_nums[count++] = entry->seq_num;
    entry->last_nack_time = now;
    entry->retry_count++;
  }

  if (count > 0) {
    nack->nacks_sent++;

    /* Call callback if set */
    if (nack->on_nack) {
      nack->on_nack(nack->user_data, nack->ssrc, seq_nums, count);
    }
  }

  return count;
}

void nack_receiver_cleanup_old(nack_receiver_t *nack, int64_t now) {
  if (!nack) return;

  /* Remove packets that are too old or retried too many times */
  int i = 0;
  while (i < nack->missing_count) {
    nack_entry_t *entry = &nack->missing[i];

    if (now - entry->first_nack_time > NACK_MAX_PACKET_AGE_MS * 1000 ||
        entry->retry_count >= NACK_MAX_RETRIES) {
      /* Remove this entry */
      memmove(&nack->missing[i], &nack->missing[i + 1],
              (nack->missing_count - i - 1) * sizeof(nack_entry_t));
      nack->missing_count--;
    } else {
      i++;
    }
  }
}

void nack_receiver_set_callback(nack_receiver_t *nack,
                                void (*callback)(void *user_data, uint32_t ssrc,
                                                 const uint16_t *seq_nums, int count),
                                void *user_data) {
  if (!nack) return;
  nack->on_nack = callback;
  nack->user_data = user_data;
}

void nack_receiver_get_stats(nack_receiver_t *nack, int64_t *packets_received,
                             int64_t *packets_lost, int64_t *nacks_sent,
                             int64_t *retransmissions_received, int *missing_count) {
  if (!nack) return;

  if (packets_received) *packets_received = nack->packets_received;
  if (packets_lost) *packets_lost = nack->packets_lost;
  if (nacks_sent) *nacks_sent = nack->nacks_sent;
  if (retransmissions_received) *retransmissions_received = nack->retransmissions_received;
  if (missing_count) *missing_count = nack->missing_count;
}

/* =============================================================================
 * NACK Sender (Handles Retransmission Requests)
 * ============================================================================= */

struct nack_sender_t {
  uint32_t ssrc;

  /* Packet history (uses existing rtp_history) */
  rtp_history_t *history;

  /* Statistics */
  int64_t nacks_received;
  int64_t packets_retransmitted;
  int64_t retransmit_failures;

  /* Callbacks */
  void (*on_retransmit)(void *user_data, const uint8_t *packet, size_t len);
  void *user_data;
};

nack_sender_t *nack_sender_create(uint32_t ssrc, size_t history_size) {
  nack_sender_t *nack = (nack_sender_t *)calloc(1, sizeof(nack_sender_t));
  if (!nack) return NULL;

  nack->ssrc = ssrc;

  /* Create packet history */
  nack->history = rtp_history_create(history_size, RTP_MAX_PACKET);
  if (!nack->history) {
    free(nack);
    return NULL;
  }

  return nack;
}

void nack_sender_destroy(nack_sender_t *nack) {
  if (!nack) return;

  if (nack->history) {
    rtp_history_destroy(nack->history);
  }

  free(nack);
}

void nack_sender_add_packet(nack_sender_t *nack, uint16_t seq_num, const uint8_t *packet,
                            size_t len) {
  if (!nack || !packet) return;

  rtp_history_put(nack->history, seq_num, packet, len);
}

int nack_sender_process_nack(nack_sender_t *nack, const uint16_t *seq_nums, int count) {
  if (!nack || !seq_nums || count == 0) return 0;

  int retransmitted = 0;

  nack->nacks_received++;

  for (int i = 0; i < count; i++) {
    uint16_t seq_num = seq_nums[i];

    /* Look up packet in history */
    uint8_t packet[2048];
    size_t len = sizeof(packet);

    if (rtp_history_get(nack->history, seq_num, packet, &len, sizeof(packet)) == 0) {
      /* Found packet - retransmit */
      if (nack->on_retransmit) {
        nack->on_retransmit(nack->user_data, packet, len);
      }

      nack->packets_retransmitted++;
      retransmitted++;
    } else {
      /* Packet not found in history */
      nack->retransmit_failures++;
    }
  }

  return retransmitted;
}

void nack_sender_set_callback(nack_sender_t *nack,
                              void (*callback)(void *user_data, const uint8_t *packet, size_t len),
                              void *user_data) {
  if (!nack) return;
  nack->on_retransmit = callback;
  nack->user_data = user_data;
}

void nack_sender_get_stats(nack_sender_t *nack, int64_t *nacks_received,
                           int64_t *packets_retransmitted, int64_t *retransmit_failures) {
  if (!nack) return;

  if (nacks_received) *nacks_received = nack->nacks_received;
  if (packets_retransmitted) *packets_retransmitted = nack->packets_retransmitted;
  if (retransmit_failures) *retransmit_failures = nack->retransmit_failures;
}

/* =============================================================================
 * RTCP NACK Packet Building/Parsing (RFC 4585)
 * ============================================================================= */

/* NACK packet format:
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |V=2|P|  FMT=1  |   PT=205      |          length               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                  SSRC of packet sender                        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                  SSRC of media source                         |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |            PID                |             BLP               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */

int nack_build_rtcp(uint32_t sender_ssrc, uint32_t media_ssrc, const uint16_t *seq_nums, int count,
                    uint8_t *buffer, size_t *len) {
  if (!seq_nums || count == 0 || !buffer || !len) return -1;

  /* Calculate how many NACK blocks we need */
  /* Each block can represent up to 17 packets (PID + 16 BLP bits) */
  int blocks_needed = 0;
  int i = 0;

  while (i < count) {
    uint16_t pid = seq_nums[i];
    int j = i + 1;

    /* Check next 16 packets for consecutive losses */
    while (j < count && j < i + 17) {
      int diff = seq_diff(seq_nums[j], pid);
      if (diff > 0 && diff <= 16) {
        j++;
      } else {
        break;
      }
    }

    blocks_needed++;
    i = j;
  }

  size_t packet_len = 12 + (blocks_needed * 4); /* Header + FCI blocks */

  if (*len < packet_len) {
    return -1; /* Buffer too small */
  }

  /* Build RTCP header */
  buffer[0] = 0x80 | 1; /* V=2, P=0, FMT=1 (Generic NACK) */
  buffer[1] = 205;      /* PT=205 (RTPFB) */
  buffer[2] = (uint8_t)((packet_len / 4 - 1) >> 8);
  buffer[3] = (packet_len / 4 - 1) & 0xFF;

  /* Sender SSRC */
  buffer[4] = (sender_ssrc >> 24) & 0xFF;
  buffer[5] = (sender_ssrc >> 16) & 0xFF;
  buffer[6] = (sender_ssrc >> 8) & 0xFF;
  buffer[7] = sender_ssrc & 0xFF;

  /* Media source SSRC */
  buffer[8] = (media_ssrc >> 24) & 0xFF;
  buffer[9] = (media_ssrc >> 16) & 0xFF;
  buffer[10] = (media_ssrc >> 8) & 0xFF;
  buffer[11] = media_ssrc & 0xFF;

  /* Build FCI blocks */
  size_t offset = 12;
  i = 0;

  while (i < count) {
    uint16_t pid = seq_nums[i];
    uint16_t blp = 0;
    int j = i + 1;

    /* Build BLP for this block */
    while (j < count && j < i + 17) {
      int diff = seq_diff(seq_nums[j], pid);
      if (diff > 0 && diff <= 16) {
        blp |= (1 << (diff - 1));
        j++;
      } else {
        break;
      }
    }

    /* Write PID and BLP */
    buffer[offset++] = (pid >> 8) & 0xFF;
    buffer[offset++] = pid & 0xFF;
    buffer[offset++] = (blp >> 8) & 0xFF;
    buffer[offset++] = blp & 0xFF;

    i = j;
  }

  *len = packet_len;
  return 0;
}

int nack_parse_rtcp(const uint8_t *buffer, size_t len, uint32_t *sender_ssrc, uint32_t *media_ssrc,
                    uint16_t *seq_nums, int *count, int max_count) {
  if (!buffer || len < 12 || !sender_ssrc || !media_ssrc || !seq_nums || !count) {
    return -1;
  }

  /* Verify RTCP header */
  if ((buffer[0] & 0xC0) != 0x80) return -1; /* Version must be 2 */
  if (buffer[1] != 205) return -1;           /* PT must be 205 (RTPFB) */
  if ((buffer[0] & 0x1F) != 1) return -1;    /* FMT must be 1 (Generic NACK) */

  /* Parse SSRCs */
  *sender_ssrc = ((uint32_t)buffer[4] << 24) | ((uint32_t)buffer[5] << 16) |
                 ((uint32_t)buffer[6] << 8) | buffer[7];
  *media_ssrc = ((uint32_t)buffer[8] << 24) | ((uint32_t)buffer[9] << 16) |
                ((uint32_t)buffer[10] << 8) | buffer[11];

  /* Parse FCI blocks */
  size_t offset = 12;
  int seq_count = 0;

  while (offset + 4 <= len && seq_count < max_count) {
    uint16_t pid = ((uint16_t)buffer[offset] << 8) | buffer[offset + 1];
    uint16_t blp = ((uint16_t)buffer[offset + 2] << 8) | buffer[offset + 3];

    /* Add PID */
    seq_nums[seq_count++] = pid;

    /* Add packets indicated by BLP */
    for (int i = 0; i < 16 && seq_count < max_count; i++) {
      if (blp & (1 << i)) {
        seq_nums[seq_count++] = (uint16_t)(pid + i + 1);
      }
    }

    offset += 4;
  }

  *count = seq_count;
  return 0;
}
