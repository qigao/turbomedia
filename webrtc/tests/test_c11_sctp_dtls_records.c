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
#include "dc_certificate.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <tstr.h>

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
  return RAND_bytes(destination, (int)size) == 1 ? 0 : -1;
}

static int test_mac(void *opaque, const uint8_t *message, size_t message_size,
                    uint8_t out[CNET_SCTP_ASSOC_MAC_BYTES]) {
  test_mac_provider *provider = (test_mac_provider *)opaque;
  unsigned int written = 0;
  if (!provider || (!message && message_size) || !out ||
      message_size > (size_t)INT_MAX) return -1;
  if (!HMAC(EVP_sha256(), provider->key, (int)sizeof(provider->key),
            message, message_size, out, &written)) return -1;
  return written == CNET_SCTP_ASSOC_MAC_BYTES ? 0 : -1;
}

static int test_verify_mac(void *opaque, const uint8_t *message,
                           size_t message_size,
                           const uint8_t claimed_mac[CNET_SCTP_ASSOC_MAC_BYTES]) {
  uint8_t expected[CNET_SCTP_ASSOC_MAC_BYTES];
  if (!claimed_mac || test_mac(opaque, message, message_size, expected) != 0)
    return -1;
  return CRYPTO_memcmp(expected, claimed_mac, sizeof(expected)) == 0 ? 0 : -1;
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

static SSL_CTX *create_dtls_context(tstr *fingerprint) {
  SSL_CTX *context = SSL_CTX_new(DTLS_method());
  assert(context != NULL);
  SSL_CTX_set_verify(context, SSL_VERIFY_NONE, NULL);
  assert(turbo_dc_generate_identity(context, fingerprint) == 0);
  assert(*fingerprint != NULL);
  return context;
}

static SSL *create_dtls_session(SSL_CTX *context, int server) {
  SSL *ssl = SSL_new(context);
  BIO *read_bio;
  BIO *write_bio;
  assert(ssl != NULL);
  read_bio = BIO_new(BIO_s_mem());
  write_bio = BIO_new(BIO_s_mem());
  assert(read_bio != NULL && write_bio != NULL);
  BIO_set_mem_eof_return(read_bio, -1);
  BIO_set_mem_eof_return(write_bio, -1);
  SSL_set_bio(ssl, read_bio, write_bio);
  assert(SSL_set_mtu(ssl, 1200u) == 1);
  if (server) SSL_set_accept_state(ssl);
  else SSL_set_connect_state(ssl);
  return ssl;
}

/* Move all complete pending output bytes from one DTLS write BIO into the
 * other session's read BIO. The BIOs are test transport queues, not DTLS
 * plaintext shortcuts: SSL still encrypts and authenticates every record. */
static size_t transfer_dtls_records(SSL *from, SSL *to) {
  uint8_t buffer[4096];
  size_t total = 0;
  BIO *output = SSL_get_wbio(from);
  BIO *input = SSL_get_rbio(to);
  int read_count;
  while ((read_count = BIO_read(output, buffer, sizeof(buffer))) > 0) {
    int written = BIO_write(input, buffer, read_count);
    assert(written == read_count);
    total += (size_t)written;
  }
  assert(read_count <= 0);
  return total;
}

static void verify_peer_fingerprint(SSL *session, const char *expected) {
  X509 *certificate;
  uint8_t digest[EVP_MAX_MD_SIZE];
  unsigned int digest_size = 0;
  char actual[EVP_MAX_MD_SIZE * 3];
  static const char hex[] = "0123456789ABCDEF";
  size_t position = 0;
  unsigned int i;

  assert(session != NULL && expected != NULL);
  certificate = SSL_get_peer_certificate(session);
  assert(certificate != NULL);
  assert(X509_digest(certificate, EVP_sha256(), digest, &digest_size) == 1);
  X509_free(certificate);
  assert(digest_size == 32u);
  for (i = 0; i < digest_size; ++i) {
    actual[position++] = hex[(digest[i] >> 4) & 0x0fu];
    actual[position++] = hex[digest[i] & 0x0fu];
    if (i + 1u != digest_size) actual[position++] = ':';
  }
  actual[position] = '\0';
  assert(strcmp(actual, expected) == 0);
}

static void drive_dtls_handshake(SSL *client, SSL *server) {
  unsigned attempt;
  for (attempt = 0; attempt < 100u; ++attempt) {
    int result;
    int error;
    if (!SSL_is_init_finished(client)) {
      result = SSL_do_handshake(client);
      if (result != 1) {
        error = SSL_get_error(client, result);
        assert(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
      }
    }
    (void)transfer_dtls_records(client, server);

    if (!SSL_is_init_finished(server)) {
      result = SSL_do_handshake(server);
      if (result != 1) {
        error = SSL_get_error(server, result);
        assert(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE);
      }
    }
    (void)transfer_dtls_records(server, client);
    if (SSL_is_init_finished(client) && SSL_is_init_finished(server)) return;
  }
  assert(!"DTLS handshake did not complete within bounded pump iterations");
}

/* Encrypt one complete SCTP packet as a DTLS application record and read the
 * authenticated plaintext at the opposite endpoint. */
static size_t dtls_roundtrip(SSL *sender, SSL *receiver,
                             const uint8_t *plaintext, size_t plaintext_size,
                             uint8_t *received, size_t received_capacity) {
  int written;
  int read_count;
  assert(plaintext != NULL && plaintext_size > 0u);
  assert(plaintext_size <= INT_MAX && received != NULL);
  assert(received_capacity <= INT_MAX);
  written = SSL_write(sender, plaintext, (int)plaintext_size);
  assert(written == (int)plaintext_size);
  assert(transfer_dtls_records(sender, receiver) > 0u);
  read_count = SSL_read(receiver, received, (int)received_capacity);
  assert(read_count > 0);
  return (size_t)read_count;
}

static void test_real_dtls_records_carry_c11_sctp(void) {
  SSL_CTX *client_context = NULL;
  SSL_CTX *server_context = NULL;
  SSL *client = NULL;
  SSL *server = NULL;
  tstr client_fingerprint = NULL;
  tstr server_fingerprint = NULL;
  test_mac_provider client_provider = {{0}};
  test_mac_provider server_provider = {{0}};
  cnet_sctp_assoc client_assoc = {0};
  cnet_sctp_assoc server_assoc = {0};
  cnet_sctp_assoc_config client_config;
  cnet_sctp_assoc_config server_config;
  uint8_t init[TEST_PACKET_CAPACITY], init_ack[TEST_PACKET_CAPACITY];
  uint8_t cookie_echo[TEST_PACKET_CAPACITY], cookie_ack[TEST_PACKET_CAPACITY];
  uint8_t data_packet[TEST_PACKET_CAPACITY], sack[TEST_PACKET_CAPACITY];
  uint8_t plaintext[TEST_PLAINTEXT_CAPACITY], received[64];
  const uint8_t message[] = {'r','e','a','l','-','d','t','l','s','-','s','c','t','p'};
  cnet_sctp_data_message metadata = {0};
  size_t init_size = 0, init_ack_size = 0, cookie_echo_size = 0;
  size_t cookie_ack_size = 0, data_packet_size = 0, sack_size = 0;
  size_t plaintext_size = 0, received_size = 0;

  assert(RAND_bytes(client_provider.key, sizeof(client_provider.key)) == 1);
  assert(RAND_bytes(server_provider.key, sizeof(server_provider.key)) == 1);
  client_context = create_dtls_context(&client_fingerprint);
  server_context = create_dtls_context(&server_fingerprint);
  client = create_dtls_session(client_context, 0);
  server = create_dtls_session(server_context, 1);
  drive_dtls_handshake(client, server);
  assert(SSL_is_init_finished(client));
  assert(SSL_is_init_finished(server));
  assert(client_fingerprint != NULL && server_fingerprint != NULL);
  assert(strcmp(client_fingerprint, server_fingerprint) != 0);
  verify_peer_fingerprint(client, server_fingerprint);
  verify_peer_fingerprint(server, client_fingerprint);

  /* Cookie MAC providers are endpoint-local, like production key ownership. */
  client_config = test_assoc_config(5000u, 5001u, &client_provider);
  server_config = test_assoc_config(5001u, 5000u, &server_provider);
  assert(cnet_sctp_assoc_init(&client_assoc, &client_config, 0u) ==
         CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_init(&server_assoc, &server_config, 0u) ==
         CNET_SCTP_ASSOC_OK);

  assert(cnet_sctp_assoc_start(&client_assoc, 1u, init, sizeof(init),
                               &init_size) == CNET_SCTP_ASSOC_OK);
  plaintext_size = dtls_roundtrip(client, server, init, init_size,
                                  plaintext, sizeof(plaintext));
  assert(plaintext_size == init_size &&
         memcmp(plaintext, init, init_size) == 0);
  assert(cnet_sctp_assoc_input(&server_assoc, 2u, plaintext, plaintext_size,
                               init_ack, sizeof(init_ack), &init_ack_size) ==
         CNET_SCTP_ASSOC_OK);

  received_size = dtls_roundtrip(server, client, init_ack, init_ack_size,
                                 plaintext, sizeof(plaintext));
  assert(received_size == init_ack_size &&
         memcmp(plaintext, init_ack, init_ack_size) == 0);
  assert(cnet_sctp_assoc_input(&client_assoc, 3u, plaintext, received_size,
                               cookie_echo, sizeof(cookie_echo),
                               &cookie_echo_size) == CNET_SCTP_ASSOC_OK);

  received_size = dtls_roundtrip(client, server, cookie_echo, cookie_echo_size,
                                 plaintext, sizeof(plaintext));
  assert(received_size == cookie_echo_size &&
         memcmp(plaintext, cookie_echo, cookie_echo_size) == 0);
  assert(cnet_sctp_assoc_input(&server_assoc, 4u, plaintext, received_size,
                               cookie_ack, sizeof(cookie_ack),
                               &cookie_ack_size) == CNET_SCTP_ASSOC_OK);

  received_size = dtls_roundtrip(server, client, cookie_ack, cookie_ack_size,
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
  received_size = dtls_roundtrip(client, server, data_packet, data_packet_size,
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

  assert(cnet_sctp_assoc_stop(&client_assoc) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_stop(&server_assoc) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&client_assoc) == CNET_SCTP_ASSOC_OK);
  assert(cnet_sctp_assoc_destroy(&server_assoc) == CNET_SCTP_ASSOC_OK);

  SSL_free(client);
  SSL_free(server);
  SSL_CTX_free(client_context);
  SSL_CTX_free(server_context);
  tstr_free(client_fingerprint);
  tstr_free(server_fingerprint);
}

int main(void) {
  test_real_dtls_records_carry_c11_sctp();
  puts("Real DTLS application records carry private C11 SCTP PASS");
  return 0;
}
