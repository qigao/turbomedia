/*
 * Real DTLS application-record integration for the private C11 SCTP core.
 *
 * Unlike test_salts_c11_sctp_dtls_handoff.c, this test does not use an
 * "authenticated" boolean to stand in for DTLS. It performs a real in-memory
 * DTLS handshake with ephemeral P-256 identities, then carries the complete
 * C11 SCTP INIT/INIT-ACK/COOKIE-ECHO/COOKIE-ACK exchange and a DATA message
 * inside encrypted DTLS application records.
 *
 * This remains test-only. It does not replace TurboMedia's production
 * usrsctp backend and does not qualify independent browser/libwebrtc interop.
 */
#include "sctp_assoc_internal.h"
#include <crypto/dtls_stream.h>
#include <crypto/symmetric.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  TEST_PACKET_CAPACITY = CNET_SCTP_ASSOC_MAX_PACKET,
  TEST_PLAINTEXT_CAPACITY = CNET_SCTP_ASSOC_MAX_PACKET
};

typedef struct test_mac_provider {
  uint8_t key[32];
} test_mac_provider;

static int test_is_owner(void *opaque) {
  (void)opaque;
  /* All Association calls in this test are made synchronously by one thread. */
  return 1;
}

static int test_entropy(void *opaque, uint8_t *destination, size_t size) {
  (void)opaque;
  if (!destination || size > INT_MAX) return -1;
  return salts_crypto_random(destination, size);
}

static int test_mac(void *opaque, const uint8_t *message, size_t message_size,
                    uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  test_mac_provider *provider = (test_mac_provider *)opaque;
  size_t written = 0;
  if (!provider || (!message && message_size) || !out ||
      message_size > (size_t)INT_MAX) return -1;
  if (salts_crypto_hmac(SALTS_CRYPTO_SHA256, provider->key, sizeof(provider->key), message, message_size, out, CNET_SCTP_ASSOC_MAC_BYTES, &written) != 0) return -1;
  return written == CNET_SCTP_ASSOC_MAC_BYTES ? 0 : -1;
}

static int test_verify_mac(void *opaque, const uint8_t *message,
                           size_t message_size,
                           const uint8_t claimed_mac[CNET_SCTP_ASSOC_MAC_BYTES]) {
  uint8_t expected[CNET_SCTP_ASSOC_MAC_BYTES];
  if (!claimed_mac || test_mac(opaque, message, message_size, expected) != 0)
    return -1;
  uint8_t diff = 0;
  for (size_t i = 0; i < sizeof(expected); ++i) diff |= expected[i] ^ claimed_mac[i];
  return diff ? -1 : 0;
}

static cnet_sctp_assoc_config test_assoc_config(
    uint16_t local_port, uint16_t remote_port, test_mac_provider *provider) {
  cnet_sctp_assoc_config config = {0};
  config.local_port = local_port;
  config.remote_port = remote_port;
  config.receive_window = 8192u;
  config.outbound_streams = 4u;
  config.inbound_streams = 4u;
  config.t1_ms = 100u;
  config.t1_max_ms = 400u;
  config.cookie_lifetime_ms = 1000u;
  config.peer_binding = UINT64_C(0x44544c5353435450);
  config.max_retransmits = 3u;
  config.is_owner = test_is_owner;
  config.entropy = test_entropy;
  config.mac = test_mac;
  config.verify_mac = test_verify_mac;
  config.provider_context = provider;
  return config;
}

typedef struct {
  crypto_dtls_stream *stream;
  uint8_t payload[TEST_PLAINTEXT_CAPACITY];
  size_t size;
} endpoint;

static int application(void *user, crypto_dtls_stream *stream,
                       const uint8_t *bytes, size_t size) {
  endpoint *ep = user;
  (void)stream;
  assert(size <= sizeof(ep->payload));
  memcpy(ep->payload, bytes, size);
  ep->size = size;
  return 0;
}

static int ready(const endpoint *ep) {
  if (!ep->stream) return 0;
  crypto_dtls_stream_info info;
  assert(crypto_dtls_stream_get_info(ep->stream, &info) == 0);
  assert(info.state != CRYPTO_DTLS_FAILED);
  return info.state == CRYPTO_DTLS_READY;
}

static void handshake(endpoint *client, endpoint *server, crypto_dtls_listener *listener,
                      const uint8_t fingerprint[32]) {
  static const uint8_t address[] = {4, 127, 0, 0, 1, 0x13, 0x88};
  for (unsigned round = 0; round < 32 && (!ready(client) || !ready(server)); ++round) {
    const uint8_t *wire;
    size_t size;
    while (crypto_dtls_stream_next(client->stream, 0, &wire, &size) == 0) {
      uint8_t packet[CRYPTO_DTLS_MTU];
      assert(size <= sizeof(packet));
      memcpy(packet, wire, size);
      assert(crypto_dtls_stream_sent(client->stream, 0, true) == 0);
      if (server->stream) {
        assert(crypto_dtls_stream_receive(server->stream, 0, packet, size) == 0);
      } else {
        uint8_t reply[CRYPTO_DTLS_COOKIE_REPLY_BYTES];
        size_t reply_size;
        bool verified;
        assert(crypto_dtls_listener_gate(listener, address, sizeof(address), packet, size,
                   0, reply, sizeof(reply), &reply_size, &verified) == 0);
        if (verified) {
          assert(crypto_dtls_stream_accept_srtp(listener, address, sizeof(address), packet,
                     size, 0, fingerprint, application, server, &server->stream) == 0);
        } else {
          assert(reply_size > 0);
          assert(crypto_dtls_stream_receive(client->stream, 0, reply, reply_size) == 0);
        }
      }
    }
    while (server->stream && crypto_dtls_stream_next(server->stream, 0, &wire, &size) == 0) {
      assert(crypto_dtls_stream_receive(client->stream, 0, wire, size) == 0);
      assert(crypto_dtls_stream_sent(server->stream, 0, true) == 0);
    }
  }
  assert(ready(client) && ready(server));
}

static size_t dtls_roundtrip(endpoint *sender, endpoint *receiver,
                             const uint8_t *plaintext, size_t plaintext_size,
                             uint8_t *received, size_t received_capacity) {
  assert(crypto_dtls_stream_write(sender->stream, plaintext, plaintext_size) == 0);
  const uint8_t *wire;
  size_t size;
  assert(crypto_dtls_stream_next(sender->stream, 0, &wire, &size) == 0);
  assert(crypto_dtls_stream_receive(receiver->stream, 0, wire, size) == 0);
  assert(crypto_dtls_stream_sent(sender->stream, 0, true) == 0);
  assert(receiver->size == plaintext_size && receiver->size <= received_capacity);
  memcpy(received, receiver->payload, receiver->size);
  return receiver->size;
}

static void test_real_dtls_records_carry_c11_sctp(void) {
  endpoint client = {0}, server = {0};
  crypto_tls_credential_object *client_key = NULL, *server_key = NULL;
  crypto_dtls_listener *listener = NULL;
  uint8_t client_fingerprint[32], server_fingerprint[32];
  test_mac_provider client_provider = {{0}};
  test_mac_provider server_provider = {{0}};
  cnet_sctp_assoc client_assoc = {0};
  cnet_sctp_assoc server_assoc = {0};
  cnet_sctp_assoc_config client_config;
  cnet_sctp_assoc_config server_config;
  uint8_t init[TEST_PACKET_CAPACITY], init_ack[TEST_PACKET_CAPACITY];
  uint8_t cookie_echo[TEST_PACKET_CAPACITY], cookie_ack[TEST_PACKET_CAPACITY];
  uint8_t data_packet[TEST_PACKET_CAPACITY], sack[TEST_PACKET_CAPACITY];
  uint8_t ack_reply[TEST_PACKET_CAPACITY];
  uint8_t plaintext[TEST_PLAINTEXT_CAPACITY], received[64];
  const uint8_t message[] = {'r','e','a','l','-','d','t','l','s','-','s','c','t','p'};
  cnet_sctp_data_message metadata = {0};
  size_t init_size = 0, init_ack_size = 0, cookie_echo_size = 0;
  size_t cookie_ack_size = 0, data_packet_size = 0, sack_size = 0;
  size_t ack_reply_size = 0;
  size_t plaintext_size = 0, received_size = 0;

  assert(salts_crypto_random(client_provider.key, sizeof(client_provider.key)) == 0);
  assert(salts_crypto_random(server_provider.key, sizeof(server_provider.key)) == 0);
  assert(crypto_dtls_srtp_identity_create(&client_key) == 0);
  assert(crypto_dtls_srtp_identity_create(&server_key) == 0);
  assert(crypto_dtls_srtp_identity_fingerprint(client_key, client_fingerprint) == 0);
  assert(crypto_dtls_srtp_identity_fingerprint(server_key, server_fingerprint) == 0);
  const crypto_tls_credentials client_identity = {.size = sizeof(client_identity), .object = client_key};
  const crypto_tls_credentials server_identity = {.size = sizeof(server_identity), .object = server_key};
  assert(crypto_dtls_stream_create_srtp(&client_identity, server_fingerprint,
             CRYPTO_DTLS_SRTP_PROFILES, application, &client, &client.stream) == 0);
  assert(crypto_dtls_listener_create_srtp(&server_identity, CRYPTO_DTLS_SRTP_PROFILES,
             1000, &listener) == 0);
  handshake(&client, &server, listener, client_fingerprint);

  /* Cookie MAC providers are endpoint-local, like production key ownership. */
  client_config = test_assoc_config(5000u, 5001u, &client_provider);
  server_config = test_assoc_config(5001u, 5000u, &server_provider);
  assert(cnet_sctp_assoc_init(&client_assoc, &client_config, 0u) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(&server_assoc, &server_config, 0u) ==
         CNET_SCTP_ASSOC_OK);

  assert(cnet_sctp_assoc_start(&client_assoc, 1u, init, sizeof(init),
                               &init_size) == CNET_SCTP_ASSOC_OK);
  plaintext_size = dtls_roundtrip(&client, &server, init, init_size,
                                  plaintext, sizeof(plaintext));
  assert(plaintext_size == init_size &&
         memcmp(plaintext, init, init_size) == 0);
  assert(cnet_sctp_assoc_input(&server_assoc, 2u, plaintext, plaintext_size,
                               init_ack, sizeof(init_ack), &init_ack_size) ==
         CNET_SCTP_ASSOC_OK);

  received_size = dtls_roundtrip(&server, &client, init_ack, init_ack_size,
                                 plaintext, sizeof(plaintext));
  assert(received_size == init_ack_size &&
         memcmp(plaintext, init_ack, init_ack_size) == 0);
  assert(cnet_sctp_assoc_input(&client_assoc, 3u, plaintext, received_size,
                               cookie_echo, sizeof(cookie_echo),
                               &cookie_echo_size) == CNET_SCTP_ASSOC_OK);

  received_size = dtls_roundtrip(&client, &server, cookie_echo, cookie_echo_size,
                                 plaintext, sizeof(plaintext));
  assert(received_size == cookie_echo_size &&
         memcmp(plaintext, cookie_echo, cookie_echo_size) == 0);
  assert(cnet_sctp_assoc_input(&server_assoc, 4u, plaintext, received_size,
                               cookie_ack, sizeof(cookie_ack),
                               &cookie_ack_size) == CNET_SCTP_ASSOC_OK);

  received_size = dtls_roundtrip(&server, &client, cookie_ack, cookie_ack_size,
                                 plaintext, sizeof(plaintext));
  assert(received_size == cookie_ack_size &&
         memcmp(plaintext, cookie_ack, cookie_ack_size) == 0);
  assert(cnet_sctp_assoc_input(&client_assoc, 5u, plaintext, received_size,
                               sack, sizeof(sack), &sack_size) ==
         CNET_SCTP_ASSOC_OK);
  assert(sack_size == 0u);
  assert(client_assoc.state == CNET_SCTP_ASSOC_ESTABLISHED);
  assert(server_assoc.state == CNET_SCTP_ASSOC_ESTABLISHED);

  assert(cnet_sctp_assoc_send_message(
      &client_assoc, 6u, 0u, 51u, 0, message, sizeof(message),
      data_packet, sizeof(data_packet), &data_packet_size) == CNET_SCTP_ASSOC_OK);
  received_size = dtls_roundtrip(&client, &server, data_packet, data_packet_size,
                                 plaintext, sizeof(plaintext));
  assert(received_size == data_packet_size &&
         memcmp(plaintext, data_packet, data_packet_size) == 0);
  assert(cnet_sctp_assoc_input(&server_assoc, 7u, plaintext, received_size,
                               sack, sizeof(sack), &sack_size) ==
         CNET_SCTP_ASSOC_OK);
  assert(sack_size > 0u);
  assert(cnet_sctp_assoc_take_message(&server_assoc, &metadata, received,
                                      sizeof(received), &received_size) ==
         CNET_SCTP_ASSOC_OK);
  assert(metadata.stream_id == 0u && metadata.ppid == 51u);
  assert(received_size == sizeof(message));
  assert(memcmp(received, message, sizeof(message)) == 0);

  /* Carry the generated SACK back through a real encrypted DTLS record and
   * prove the sender releases the DATA bytes retained for retransmission. */
  received_size = dtls_roundtrip(&server, &client, sack, sack_size,
                                 plaintext, sizeof(plaintext));
  assert(received_size == sack_size &&
         memcmp(plaintext, sack, sack_size) == 0);
  assert(cnet_sctp_assoc_input(&client_assoc, 8u, plaintext, received_size,
                               ack_reply, sizeof(ack_reply),
                               &ack_reply_size) == CNET_SCTP_ASSOC_OK);
  assert(ack_reply_size == 0u);
  assert(client_assoc.data.in_flight == 0u);

  assert(cnet_sctp_assoc_stop(&client_assoc) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&server_assoc) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&client_assoc) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&server_assoc) == CNET_SCTP_ASSOC_OK);

  assert(crypto_dtls_stream_destroy(client.stream) == 0);
  assert(crypto_dtls_stream_destroy(server.stream) == 0);
  assert(crypto_dtls_listener_destroy(listener) == 0);
  crypto_tls_credential_object_release(client_key);
  crypto_tls_credential_object_release(server_key);
}

int main(void) {
  test_real_dtls_records_carry_c11_sctp();
  puts("Real DTLS application records carry private C11 SCTP PASS");
  return 0;
}
