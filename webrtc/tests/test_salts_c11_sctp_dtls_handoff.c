/*
 * Test-only DTLS/SCTP handoff contract for the private C11 association.
 *
 * This is NOT a DTLS implementation: authenticated is a fixture assertion
 * that the caller has already accepted a record through DTLS. The test makes
 * the plaintext boundary explicit and verifies generation, Owner, capacity,
 * and close behavior without touching the production usrsctp path.
 */
#include "sctp_assoc_internal.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { HANDOFF_OK = 0, HANDOFF_REJECTED = -100 };

typedef struct {
  uint32_t tag;
  uint32_t tsn;
  uint32_t key;
  int owner;
} fixture_provider;

typedef struct {
  cnet_sctp_assoc *association;
  uint64_t generation;
  int active;
  fixture_provider *provider;
} dtls_sctp_handoff;

static int fixture_is_owner(void *context) {
  return ((fixture_provider *)context)->owner;
}
static int fixture_entropy(void *context, uint8_t *out, size_t size) {
  fixture_provider *p = (fixture_provider *)context;
  if (!out || size != CNET_SCTP_ASSOC_ENTROPY_BYTES) return -1;
  out[0]=(uint8_t)(p->tag>>24); out[1]=(uint8_t)(p->tag>>16);
  out[2]=(uint8_t)(p->tag>>8);  out[3]=(uint8_t)p->tag;
  out[4]=(uint8_t)(p->tsn>>24); out[5]=(uint8_t)(p->tsn>>16);
  out[6]=(uint8_t)(p->tsn>>8);  out[7]=(uint8_t)p->tsn;
  return 0;
}
static int fixture_mac(void *context, const uint8_t *data, size_t size,
                       uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  fixture_provider *p = (fixture_provider *)context;
  uint32_t s = p->key;
  size_t i;
  for (i=0; i<size; ++i) s = s * UINT32_C(65599) + data[i];
  for (i=0; i<CNET_SCTP_ASSOC_MAC_BYTES; ++i) {
    s = s * UINT32_C(1664525) + UINT32_C(1013904223);
    out[i] = (uint8_t)(s >> 24);
  }
  return 0;
}
static int fixture_verify(void *context, const uint8_t *data, size_t size,
                          const uint8_t mac[CNET_SCTP_ASSOC_MAC_BYTES]) {
  uint8_t expected[CNET_SCTP_ASSOC_MAC_BYTES];
  uint8_t diff = 0;
  size_t i;
  if (fixture_mac(context, data, size, expected) != 0) return -1;
  for (i=0; i<sizeof(expected); ++i) diff |= (uint8_t)(expected[i] ^ mac[i]);
  return diff == 0 ? 0 : -1;
}
static cnet_sctp_assoc_config fixture_config(uint16_t port,
                                               fixture_provider *provider,
                                               uint64_t generation) {
  cnet_sctp_assoc_config c = {0};
  c.local_port = port;
  c.remote_port = port == 5000u ? 5001u : 5000u;
  c.receive_window = 8192u;
  c.outbound_streams = 4u;
  c.inbound_streams = 4u;
  c.t1_ms = 100u;
  c.t1_max_ms = 400u;
  c.cookie_lifetime_ms = 1000u;
  c.peer_binding = generation;
  c.max_retransmits = 3u;
  c.is_owner = fixture_is_owner;
  c.entropy = fixture_entropy;
  c.mac = fixture_mac;
  c.verify_mac = fixture_verify;
  c.provider_context = provider;
  return c;
}

/* Entry point models a post-DTLS authenticated application record. A real
 * caller must only pass plaintext produced by its verified DTLS session. */
static int handoff_inject(dtls_sctp_handoff *h, uint64_t generation,
                          int authenticated, uint64_t now,
                          const void *plaintext, size_t plaintext_size,
                          void *reply, size_t reply_capacity,
                          size_t *reply_size) {
  if (reply_size) *reply_size = 0u;
  if (!h || !h->association || !reply_size || (!plaintext && plaintext_size) ||
      (!reply && reply_capacity)) return CNET_SCTP_ASSOC_INVALID;
  if (!h->active) return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if (generation != h->generation || !authenticated)
    return HANDOFF_REJECTED;
  if (!fixture_is_owner(h->provider)) return CNET_SCTP_ASSOC_OWNER;
  return (int)cnet_sctp_assoc_input(h->association, now,
      (const uint8_t *)plaintext, plaintext_size,
      (uint8_t *)reply, reply_capacity, reply_size);
}

/* Owner-pulled bounded outbound SCTP packet; no internal queue or borrowed
 * output pointer is exposed by this adapter. */
static int handoff_pull(dtls_sctp_handoff *h, uint64_t generation,
                        uint64_t now, uint16_t sid, uint32_t ppid,
                        const void *payload, size_t payload_size,
                        void *packet, size_t packet_capacity,
                        size_t *packet_size) {
  if (packet_size) *packet_size = 0u;
  if (!h || !h->association || !packet_size || (!payload && payload_size) ||
      (!packet && packet_capacity)) return CNET_SCTP_ASSOC_INVALID;
  if (!h->active) return CNET_SCTP_ASSOC_CLOSED_ERROR;
  if (generation != h->generation) return HANDOFF_REJECTED;
  if (!fixture_is_owner(h->provider)) return CNET_SCTP_ASSOC_OWNER;
  return (int)cnet_sctp_assoc_send_message(h->association, now, sid, ppid, 0,
      payload, payload_size, packet, packet_capacity, packet_size);
}
static void handoff_close(dtls_sctp_handoff *h) {
  if (!h || !h->association || !h->active) return;
  h->active = 0;
  assert(cnet_sctp_assoc_stop(h->association) == CNET_SCTP_ASSOC_OK);
}

static void test_handoff_generation_capacity_close(void) {
  cnet_sctp_assoc a = {0}, b = {0};
  fixture_provider pa = {UINT32_C(0x11223344), UINT32_C(0xfffffffe),
                         UINT32_C(0x13579bdf), 1};
  fixture_provider pb = {UINT32_C(0x55667788), 1000u,
                         UINT32_C(0x13579bdf), 1};
  const uint64_t ga = UINT64_C(0x100000001);
  const uint64_t gb = UINT64_C(0x100000002);
  cnet_sctp_assoc_config ac = fixture_config(5000u, &pa, UINT64_C(0x5a5a5a5a));
  cnet_sctp_assoc_config bc = fixture_config(5001u, &pb, UINT64_C(0x5a5a5a5a));
  dtls_sctp_handoff ha = {&a, ga, 1, &pa};
  dtls_sctp_handoff hb = {&b, gb, 1, &pb};
  uint8_t init[1200], ack[1200], echo[1200], final_ack[1200];
  uint8_t wire[1200], reply[1200], received[32], deferred_packet[1200];
  const uint8_t message[] = {'d','t','l','s','-','s','c','t','p'};
  cnet_sctp_data_message meta = {0};
  size_t ni=0, na=0, ne=0, nf=0, nw=0, nr=0, nmsg=0, deferred_size=0;
  uint32_t rx_before=0;
  size_t buffered_before=0;
  cnet_sctp_assoc_state state_before_close;
  uint32_t cumulative_before_close;
  size_t buffered_before_close;

  assert(cnet_sctp_assoc_init(&a, &ac, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(&b, &bc, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &ni) ==
         CNET_SCTP_ASSOC_OK);

  /* An INIT ACK that cannot fit must not commit responder state or sign a
   * cookie. Retrying the same packet at the same logical time must work. */
  na = 77u;
  assert(handoff_inject(&hb, gb, 1, 2u, init, ni,
      ack, 8u, &na) == CNET_SCTP_ASSOC_CAPACITY && na == 0u);
  assert(b.state == CNET_SCTP_ASSOC_CLOSED && !b.data_ready);
  assert(handoff_inject(&hb, gb, 1, 2u, init, ni,
      ack, sizeof(ack), &na) == CNET_SCTP_ASSOC_OK);
  /* A too-small COOKIE ECHO response buffer must not publish ESTABLISHED
   * or retain a partially initialized DATA engine; same-time retry succeeds. */
  ne = 77u;
  assert(handoff_inject(&ha, ga, 1, 3u, ack, na,
      echo, 8u, &ne) == CNET_SCTP_ASSOC_CAPACITY && ne == 0u);
  assert(a.state == CNET_SCTP_ASSOC_COOKIE_WAIT && !a.data_ready);
  assert(handoff_inject(&ha, ga, 1, 3u, ack, na,
      echo, sizeof(echo), &ne) == CNET_SCTP_ASSOC_OK);
  nf = 77u;
  assert(handoff_inject(&hb, gb, 1, 4u, echo, ne,
      final_ack, 8u, &nf) == CNET_SCTP_ASSOC_CAPACITY && nf == 0u);
  assert(b.state == CNET_SCTP_ASSOC_CLOSED && !b.data_ready);
  assert(handoff_inject(&hb, gb, 1, 4u, echo, ne,
      final_ack, sizeof(final_ack), &nf) == CNET_SCTP_ASSOC_OK);
  assert(handoff_inject(&ha, ga, 1, 5u, final_ack, nf,
      reply, sizeof(reply), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 0u && a.state == CNET_SCTP_ASSOC_ESTABLISHED &&
         b.state == CNET_SCTP_ASSOC_ESTABLISHED);

  /* Reject stale-generation and non-authenticated inputs before SCTP state
   * changes; output size is always reset on every rejection path. */
  nr = 77u;
  assert(handoff_inject(&hb, gb + 1u, 1, 6u, init, ni,
      reply, sizeof(reply), &nr) == HANDOFF_REJECTED && nr == 0u);
  assert(handoff_inject(&hb, gb, 0, 6u, init, ni,
      reply, sizeof(reply), &nr) == HANDOFF_REJECTED && nr == 0u);
  assert(handoff_inject(&hb, gb, 1, 6u, NULL, 1u,
      reply, sizeof(reply), &nr) == CNET_SCTP_ASSOC_INVALID && nr == 0u);
  pb.owner = 0;
  assert(handoff_inject(&hb, gb, 1, 6u, init, ni,
      reply, sizeof(reply), &nr) == CNET_SCTP_ASSOC_OWNER && nr == 0u);
  nw = 77u;
  assert(handoff_pull(&hb, gb, 6u, 0u, 51u, message, sizeof(message),
      wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_OWNER && nw == 0u);
  pb.owner = 1;

  /* Outbound pointer/length pairs are validated before touching SCTP state. */
  nw = 77u;
  assert(handoff_pull(&ha, ga, 6u, 0u, 51u, NULL, sizeof(message),
      wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_INVALID && nw == 0u);
  nw = 77u;
  assert(handoff_pull(&ha, ga, 6u, 0u, 51u, message, sizeof(message),
      NULL, sizeof(wire), &nw) == CNET_SCTP_ASSOC_INVALID && nw == 0u);

  /* Capacity refusal is atomic; the same payload remains sendable. */
  assert(handoff_pull(&ha, ga, 6u, 0u, 51u, message, sizeof(message),
      wire, 8u, &nw) == CNET_SCTP_ASSOC_CAPACITY && nw == 0u);
  assert(handoff_pull(&ha, ga + 1u, 6u, 0u, 51u, message, sizeof(message),
      wire, sizeof(wire), &nw) == HANDOFF_REJECTED && nw == 0u);
  assert(handoff_pull(&ha, ga, 6u, 0u, 51u, message, sizeof(message),
      wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_OK && nw != 0u);
  /* A DATA packet whose SACK cannot fit must not consume receive TSN
   * state or enqueue payload; retrying the same packet must deliver once. */
  rx_before = b.data.cumulative_rx;
  buffered_before = b.data.buffered_rx;
  {
    static const uint8_t sentinel[8] =
        {0xa5u, 0x5au, 0xc3u, 0x3cu, 0x96u, 0x69u, 0xf0u, 0x0fu};
    memcpy(reply, sentinel, sizeof(sentinel));
    nr = 77u;
    assert(handoff_inject(&hb, gb, 1, 7u, wire, nw,
        reply, sizeof(sentinel), &nr) == CNET_SCTP_ASSOC_CAPACITY &&
        nr == 0u);
    /* Capacity preflight must not partially publish a SACK either. */
    assert(memcmp(reply, sentinel, sizeof(sentinel)) == 0);
  }
  assert(b.data.cumulative_rx == rx_before &&
         b.data.buffered_rx == buffered_before);
  assert(handoff_inject(&hb, gb, 1, 7u, wire, nw,
      reply, sizeof(reply), &nr) == CNET_SCTP_ASSOC_OK && nr != 0u);
  assert(cnet_sctp_assoc_take_message(&b, &meta, received,
      sizeof(received), &nmsg) == CNET_SCTP_ASSOC_OK);
  assert(nmsg == sizeof(message) && meta.ppid == 51u &&
         memcmp(received, message, sizeof(message)) == 0);
  assert(handoff_inject(&ha, ga, 1, 8u, reply, nr,
      wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_OK);

  /* Model a packet callback captured before close but dispatched only
   * after the Owner's close/drain barrier. The copied packet is deliberately
   * held by the test; no concurrent association call is made. */
  assert(nr <= sizeof(deferred_packet));
  memcpy(deferred_packet, reply, nr);
  deferred_size = nr;
  handoff_close(&ha);
  /* Closing legitimately changes association state; snapshot only after the
   * close barrier so this assertion isolates the delayed callback. */
  state_before_close = a.state;
  cumulative_before_close = a.data.cumulative_rx;
  buffered_before_close = a.data.buffered_rx;
  assert(handoff_inject(&ha, ga, 1, 9u, deferred_packet, deferred_size,
      wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_CLOSED_ERROR && nw == 0u);
  assert(a.state == state_before_close &&
      a.data.cumulative_rx == cumulative_before_close &&
      a.data.buffered_rx == buffered_before_close);
  assert(handoff_inject(&ha, ga, 1, 9u, reply, nr,
      wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_CLOSED_ERROR && nw == 0u);
  assert(handoff_pull(&ha, ga, 9u, 0u, 51u, message, sizeof(message),
      wire, sizeof(wire), &nw) == CNET_SCTP_ASSOC_CLOSED_ERROR && nw == 0u);
  handoff_close(&hb);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void test_bundled_data_atomic_retry(void) {
  cnet_sctp_assoc a = {0}, b = {0};
  fixture_provider pa = {UINT32_C(0x11223344), UINT32_C(0xfffffffe),
                         UINT32_C(0x13579bdf), 1};
  fixture_provider pb = {UINT32_C(0x55667788), 1000u,
                         UINT32_C(0x13579bdf), 1};
  cnet_sctp_assoc_config ac = fixture_config(5000u, &pa, 7u);
  cnet_sctp_assoc_config bc = fixture_config(5001u, &pb, 7u);
  uint8_t init[1200], ack[1200], echo[1200], final_ack[1200];
  uint8_t frame[2][1200], bundle[1200], sack[1200], reply[1200], out[8];
  const uint8_t bytes[2] = {'a', 'b'};
  cnet_sctp_wire_header header = {0};
  cnet_sctp_wire_decoder decoder = {0};
  cnet_sctp_wire_chunk chunk[2], malformed[2];
  cnet_sctp_data_message meta = {0};
  cnet_sctp_data_rx rx_before[CNET_SCTP_DATA_RX_SLOTS];
  uint16_t rx_ssn_before[CNET_SCTP_DATA_STREAMS];
  size_t ni=0u, na=0u, ne=0u, nf=0u, sizes[2], n=0u, ns=0u, nr=0u, i;
  uint32_t cumulative, rx_gaps;
  size_t buffered;

  assert(cnet_sctp_assoc_init(&a, &ac, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(&b, &bc, 0u) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_start(&a, 1u, init, sizeof(init), &ni) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 2u, init, ni, ack, sizeof(ack), &na) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 3u, ack, na, echo, sizeof(echo), &ne) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&b, 4u, echo, ne, final_ack, sizeof(final_ack), &nf) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_input(&a, 5u, final_ack, nf, reply, sizeof(reply), &nr) == CNET_SCTP_ASSOC_OK);
  assert(nr == 0u && a.data_ready && b.data_ready);

  for (i=0u; i<2u; ++i) {
    assert(cnet_sctp_assoc_send_message(&a, 6u+i, 0u, 53u, 0,
        &bytes[i], 1u, frame[i], sizeof(frame[i]), &sizes[i]) == CNET_SCTP_ASSOC_OK);
    assert(cnet_sctp_wire_decode(frame[i], sizes[i], &header, &decoder) == CNET_SCTP_WIRE_OK);
    assert(header.chunk_count == 1u);
    assert(cnet_sctp_wire_next_chunk(&decoder, &chunk[i]) == CNET_SCTP_WIRE_OK);
    assert(chunk[i].type == 0u);
  }
  cumulative = b.data.cumulative_rx;
  buffered = b.data.buffered_rx;
  rx_gaps = b.data.rx_gaps;
  memcpy(rx_before, b.data.rx, sizeof(rx_before));
  memcpy(rx_ssn_before, b.data.rx_ssn, sizeof(rx_ssn_before));
  memcpy(malformed, chunk, sizeof(malformed));
  malformed[1].flags = 0x80u;
  assert(cnet_sctp_wire_encode(a.config.local_port, a.config.remote_port,
      a.peer_tag, malformed, 2u, bundle, sizeof(bundle), &n) == CNET_SCTP_WIRE_OK);
  memset(sack, 0xa5, sizeof(sack));
  assert(cnet_sctp_assoc_input(&b, 10u, bundle, n,
      sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_PROTOCOL);
  assert(ns == 0u && b.data.cumulative_rx == cumulative &&
      b.data.buffered_rx == buffered && b.data.rx_gaps == rx_gaps);
  assert(memcmp(b.data.rx, rx_before, sizeof(rx_before)) == 0);
  assert(memcmp(b.data.rx_ssn, rx_ssn_before, sizeof(rx_ssn_before)) == 0);
  for (i=0u; i<sizeof(sack); ++i) assert(sack[i] == 0xa5u);
  assert(cnet_sctp_assoc_take_message(&b, &meta, out, sizeof(out), &nr) == CNET_SCTP_ASSOC_EMPTY);

  assert(cnet_sctp_wire_encode(a.config.local_port, a.config.remote_port,
      a.peer_tag, chunk, 2u, bundle, sizeof(bundle), &n) == CNET_SCTP_WIRE_OK);
  memset(sack, 0xa5, sizeof(sack));
  assert(cnet_sctp_assoc_input(&b, 10u, bundle, n,
      sack, 8u, &ns) == CNET_SCTP_ASSOC_CAPACITY);
  assert(ns == 0u && b.data.cumulative_rx == cumulative &&
      b.data.buffered_rx == buffered && b.data.rx_gaps == rx_gaps);
  assert(memcmp(b.data.rx, rx_before, sizeof(rx_before)) == 0);
  assert(memcmp(b.data.rx_ssn, rx_ssn_before, sizeof(rx_ssn_before)) == 0);
  for (i=0u; i<sizeof(sack); ++i) assert(sack[i] == 0xa5u);
  assert(cnet_sctp_assoc_take_message(&b, &meta, out, sizeof(out), &nr) == CNET_SCTP_ASSOC_EMPTY);

  assert(cnet_sctp_assoc_input(&b, 10u, bundle, n,
      sack, sizeof(sack), &ns) == CNET_SCTP_ASSOC_OK);
  for (i=0u; i<2u; ++i) {
    assert(cnet_sctp_assoc_take_message(&b, &meta, out, sizeof(out), &nr) == CNET_SCTP_ASSOC_OK);
    assert(nr == 1u && out[0] == bytes[i] && meta.stream_sequence == (uint16_t)i);
  }
  assert(cnet_sctp_assoc_take_message(&b, &meta, out, sizeof(out), &nr) == CNET_SCTP_ASSOC_EMPTY);
  assert(cnet_sctp_assoc_stop(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&b) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&a) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&b) == CNET_SCTP_ASSOC_OK);
}

static void test_stale_callback_after_replacement(void) {
  cnet_sctp_assoc replacement = {0};
  fixture_provider provider = {UINT32_C(0x778899aa), 1200u,
                               UINT32_C(0x2468ace0), 1};
  const uint64_t old_generation = UINT64_C(0x200000001);
  const uint64_t new_generation = UINT64_C(0x200000002);
  cnet_sctp_assoc_config config = fixture_config(5000u, &provider, 9u);
  dtls_sctp_handoff current = {&replacement, new_generation, 1, &provider};
  uint8_t stale_packet[16] = {0x13u, 0x37u};
  uint8_t output[32];
  size_t output_size = 77u;
  cnet_sctp_assoc_state state_before;
  uint32_t cumulative_before;
  size_t buffered_before;

  assert(cnet_sctp_assoc_init(&replacement, &config, 0u) ==
         CNET_SCTP_ASSOC_OK);
  state_before = replacement.state;
  cumulative_before = replacement.data.cumulative_rx;
  buffered_before = replacement.data.buffered_rx;
  memset(output, 0xa5, sizeof(output));

  /* The handoff now represents a replacement association/generation.
   * A callback captured by the retired generation must not reach the new
   * association, even if its payload is otherwise untrusted or malformed. */
  assert(handoff_inject(&current, old_generation, 1, 1u,
      stale_packet, sizeof(stale_packet), output, sizeof(output),
      &output_size) == HANDOFF_REJECTED && output_size == 0u);
  assert(replacement.state == state_before &&
      replacement.data.cumulative_rx == cumulative_before &&
      replacement.data.buffered_rx == buffered_before);
  for (size_t i = 0u; i < sizeof(output); ++i) assert(output[i] == 0xa5u);

  output_size = 77u;
  assert(handoff_pull(&current, old_generation, 1u, 0u, 51u,
      stale_packet, sizeof(stale_packet), output, sizeof(output),
      &output_size) == HANDOFF_REJECTED && output_size == 0u);
  assert(replacement.state == state_before &&
      replacement.data.cumulative_rx == cumulative_before &&
      replacement.data.buffered_rx == buffered_before);
  assert(cnet_sctp_assoc_destroy(&replacement) == CNET_SCTP_ASSOC_OK);
}

int main(void) {
  test_handoff_generation_capacity_close();
  test_stale_callback_after_replacement();
  test_bundled_data_atomic_retry();
  puts("C11 SCTP test-only DTLS handoff contract PASS");
  return 0;
}
