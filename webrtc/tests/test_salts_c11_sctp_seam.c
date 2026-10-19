/*
 * Test-only bridge between the private Salts C11 Association/DATA/DCEP layers.
 * This is not a DTLS adapter or a production transport implementation.
 */
#include "sctp_assoc_internal.h"
#include "sctp_dcep_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct seam_owner {
  uint32_t tag;
  uint32_t tsn;
  uint32_t key;
  int is_owner;
} seam_owner;

static int owner_ok(void *context) {
  return ((seam_owner *)context)->is_owner;
}

static int test_entropy(void *context, uint8_t *out, size_t size) {
  seam_owner *owner = (seam_owner *)context;
  if (!out || size != CNET_SCTP_ASSOC_ENTROPY_BYTES) return -1;
  out[0] = (uint8_t)(owner->tag >> 24);
  out[1] = (uint8_t)(owner->tag >> 16);
  out[2] = (uint8_t)(owner->tag >> 8);
  out[3] = (uint8_t)owner->tag;
  out[4] = (uint8_t)(owner->tsn >> 24);
  out[5] = (uint8_t)(owner->tsn >> 16);
  out[6] = (uint8_t)(owner->tsn >> 8);
  out[7] = (uint8_t)owner->tsn;
  return 0;
}

/* Deliberately non-cryptographic test fixture. Production keying and CSPRNG
 * qualification remain in Salts' separately pinned crypto test. */
static int test_mac(void *context, const uint8_t *message, size_t size,
                    uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  seam_owner *owner = (seam_owner *)context;
  uint32_t state = owner->key;
  size_t i;
  for (i = 0u; i < size; ++i)
    state = state * UINT32_C(65599) + message[i];
  for (i = 0u; i < CNET_SCTP_ASSOC_MAC_BYTES; ++i) {
    state = state * UINT32_C(1664525) + UINT32_C(1013904223);
    out[i] = (uint8_t)(state >> 24);
  }
  return 0;
}

static int test_verify(void *context, const uint8_t *message, size_t size,
                       const uint8_t mac[CNET_SCTP_ASSOC_MAC_BYTES]) {
  uint8_t expected[CNET_SCTP_ASSOC_MAC_BYTES];
  uint8_t diff = 0u;
  size_t i;
  if (test_mac(context, message, size, expected) != 0) return -1;
  for (i = 0u; i < sizeof(expected); ++i)
    diff |= (uint8_t)(expected[i] ^ mac[i]);
  return diff == 0u ? 0 : -1;
}

static cnet_sctp_assoc_config make_config(uint16_t port, seam_owner *owner) {
  cnet_sctp_assoc_config config = {0};
  config.local_port = port;
  config.remote_port = port == 5000u ? 5001u : 5000u;
  config.receive_window = 8192u;
  config.outbound_streams = 4u;
  config.inbound_streams = 4u;
  config.t1_ms = 100u;
  config.t1_max_ms = 400u;
  config.cookie_lifetime_ms = 1000u;
  config.peer_binding = UINT64_C(0x5345414d00000001);
  config.max_retransmits = 3u;
  config.is_owner = owner_ok;
  config.entropy = test_entropy;
  config.mac = test_mac;
  config.verify_mac = test_verify;
  config.provider_context = owner;
  return config;
}

static void establish_pair(cnet_sctp_assoc *client, cnet_sctp_assoc *server,
                           seam_owner *client_owner,
                           seam_owner *server_owner) {
  cnet_sctp_assoc_config client_config, server_config;
  uint8_t init[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t cookie_ack[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t cookie_echo[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t final_ack[CNET_SCTP_ASSOC_MAX_PACKET];
  size_t init_size = 0u, cookie_ack_size = 0u;
  size_t cookie_echo_size = 0u, final_ack_size = 0u;

  memset(client, 0, sizeof(*client));
  memset(server, 0, sizeof(*server));
  *client_owner = (seam_owner){UINT32_C(0x11223344),
      UINT32_C(0xfffffffe), UINT32_C(0x13579bdf), 1};
  *server_owner = (seam_owner){UINT32_C(0x55667788),
      UINT32_C(1000), UINT32_C(0x13579bdf), 1};
  client_config = make_config(5000u, &client_owner);
  server_config = make_config(5001u, &server_owner);

  assert(cnet_sctp_assoc_init(client, &client_config, 0u) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(server, &server_config, 0u) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_start(client, 1u, init, sizeof(init), &init_size) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(server, 2u, init, init_size, cookie_ack,
      sizeof(cookie_ack), &cookie_ack_size) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(client, 3u, cookie_ack, cookie_ack_size,
      cookie_echo, sizeof(cookie_echo), &cookie_echo_size) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(server, 4u, cookie_echo, cookie_echo_size,
      final_ack, sizeof(final_ack), &final_ack_size) == CNET_SCTP_ASSOC_OK);
  assert(server->state == CNET_SCTP_ASSOC_ESTABLISHED && server->data_ready);
  assert(cnet_sctp_assoc_input(client, 5u, final_ack, final_ack_size,
      init, sizeof(init), &init_size) == CNET_SCTP_ASSOC_OK);
  assert(client->state == CNET_SCTP_ASSOC_ESTABLISHED && client->data_ready);
}

static void dcep_open_ack_over_sctp(void) {
  cnet_sctp_assoc client_assoc, server_assoc;
  cnet_sctp_dcep client_dcep = {0}, server_dcep = {0};
  seam_owner client_owner = {0}, server_owner = {0};
  uint8_t open_payload[CNET_SCTP_DCEP_OPEN_MAX];
  uint8_t ack_payload[1];
  uint8_t packet[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t response[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t sack[CNET_SCTP_ASSOC_MAX_PACKET];
  uint8_t received[512];
  cnet_sctp_data_message message = {0};
  size_t open_size = 0u, packet_size = 0u, response_size = 0u;
  size_t sack_size = 0u, received_size = 0u, ack_size = 0u;

  establish_pair(&client_assoc, &server_assoc, &client_owner, &server_owner);
  assert(cnet_sctp_dcep_init(&client_dcep, 4u, 1, owner_ok, &client_owner) ==
         CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_init(&server_dcep, 4u, 0, owner_ok, &server_owner) ==
         CNET_SCTP_DCEP_OK);

  assert(cnet_sctp_dcep_open(&client_dcep, 0u, 0x00u, 128u, 0u,
      "seam", 4u, "test", 4u, open_payload, sizeof(open_payload),
      &open_size) == CNET_SCTP_DCEP_OK);
  assert(!cnet_sctp_dcep_ready(&client_dcep, 0u));

  /* DCEP OPEN traverses a real SCTP DATA packet, not a direct codec call. */
  assert(cnet_sctp_assoc_send_message(&client_assoc, 6u, 0u,
      CNET_SCTP_DCEP_PPID, 0, open_payload, open_size, packet,
      sizeof(packet), &packet_size) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&server_assoc, 7u, packet, packet_size, sack,
      sizeof(sack), &sack_size) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&server_assoc, &message, received,
      sizeof(received), &received_size) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_id == 0u && !message.unordered &&
         message.ppid == CNET_SCTP_DCEP_PPID &&
         received_size == open_size);
  assert(memcmp(received, open_payload, open_size) == 0);

  assert(cnet_sctp_dcep_input(&server_dcep, message.stream_id, received,
      received_size, ack_payload, sizeof(ack_payload), &ack_size) ==
      CNET_SCTP_DCEP_OK);
  assert(ack_size == 1u && ack_payload[0] == 2u);
  assert(!cnet_sctp_dcep_ready(&server_dcep, 0u));

  /* A full SCTP packet cannot fit in a one-byte output buffer. The failed
   * admission must not mark the DCEP channel ready or consume its ACK state. */
  assert(cnet_sctp_assoc_send_message(&server_assoc, 8u, 0u,
      CNET_SCTP_DCEP_PPID, 0, ack_payload, ack_size, response,
      1u, &response_size) == CNET_SCTP_ASSOC_CAPACITY);
  assert(response_size == 0u);
  assert(server_dcep.channels[0].state == 3u);
  assert(!cnet_sctp_dcep_ready(&server_dcep, 0u));

  /* Retry with capacity, then commit DCEP readiness only after SCTP accepts
   * the ACK for transmission. */
  assert(cnet_sctp_assoc_send_message(&server_assoc, 8u, 0u,
      CNET_SCTP_DCEP_PPID, 0, ack_payload, ack_size, response,
      sizeof(response), &response_size) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_dcep_ack_admitted(&server_dcep, 0u) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_ready(&server_dcep, 0u));

  assert(cnet_sctp_assoc_input(&client_assoc, 9u, response, response_size,
      sack, sizeof(sack), &sack_size) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_take_message(&client_assoc, &message, received,
      sizeof(received), &received_size) == CNET_SCTP_ASSOC_OK);
  assert(message.stream_id == 0u && message.ppid == CNET_SCTP_DCEP_PPID &&
         received_size == 1u && received[0] == 2u);
  assert(cnet_sctp_dcep_input(&client_dcep, message.stream_id, received,
      received_size, ack_payload, sizeof(ack_payload), &ack_size) ==
      CNET_SCTP_DCEP_OK);
  assert(ack_size == 0u && cnet_sctp_dcep_ready(&client_dcep, 0u));

  assert(cnet_sctp_dcep_stop(&client_dcep) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_dcep_stop(&server_dcep) == CNET_SCTP_DCEP_OK);
  assert(cnet_sctp_assoc_stop(&client_assoc) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&server_assoc) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&client_assoc) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&server_assoc) == CNET_SCTP_ASSOC_OK);
}

int main(void) {
  dcep_open_ack_over_sctp();
  puts("Private Salts C11 SCTP/DCEP integration seam PASS");
  return 0;
}
