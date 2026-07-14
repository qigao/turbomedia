/**
 * RTP Packet History Implementation
 *
 * Stores recently sent RTP packets for NACK retransmission
 */
#include "turbo_rtp.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint8_t *data;
  size_t len;
  uint16_t seq;
  int valid;
} history_slot_t;

struct rtp_history_s {
  history_slot_t *slots;
  uint8_t *data_buffer;
  size_t max_packets;
  size_t max_packet_size;
};

rtp_history_t *rtp_history_create(size_t max_packets, size_t max_packet_size) {
  /* max_packets should be a power of 2 for efficiency, but we'll use modulo for now */
  rtp_history_t *history = (rtp_history_t *)calloc(1, sizeof(rtp_history_t));
  if (!history) return NULL;

  history->max_packets = max_packets;
  history->max_packet_size = max_packet_size;

  history->slots = (history_slot_t *)calloc(max_packets, sizeof(history_slot_t));
  history->data_buffer = (uint8_t *)malloc(max_packets * max_packet_size);

  if (!history->slots || !history->data_buffer) {
    free(history->slots);
    free(history->data_buffer);
    free(history);
    return NULL;
  }

  for (size_t i = 0; i < max_packets; i++) {
    history->slots[i].data = history->data_buffer + (i * max_packet_size);
    history->slots[i].valid = 0;
  }

  return history;
}

void rtp_history_destroy(rtp_history_t *history) {
  if (!history) return;
  free(history->data_buffer);
  free(history->slots);
  free(history);
}

void rtp_history_put(rtp_history_t *history, uint16_t seq, const uint8_t *packet, size_t len) {
  if (!history || !packet || len > history->max_packet_size) return;

  size_t idx = seq % history->max_packets;
  history_slot_t *slot = &history->slots[idx];

  memcpy(slot->data, packet, len);
  slot->len = len;
  slot->seq = seq;
  slot->valid = 1;
}

int rtp_history_get(rtp_history_t *history, uint16_t seq, uint8_t *packet, size_t *len,
                    size_t max_len) {
  if (!history || !packet || !len) return -1;

  size_t idx = seq % history->max_packets;
  history_slot_t *slot = &history->slots[idx];

  if (!slot->valid || slot->seq != seq) {
    return -1; /* Not found or already overwritten */
  }

  if (slot->len > max_len) {
    return -1; /* Buffer too small */
  }

  memcpy(packet, slot->data, slot->len);
  *len = slot->len;
  return 0;
}
