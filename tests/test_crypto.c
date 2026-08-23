#include "tinytest.h"
#include "turbo_media_crypto.h"

#include <stdint.h>
#include <string.h>

static int bytes_are_zero(const uint8_t *bytes, size_t size) {
  uint8_t aggregate = 0U;
  size_t i;
  for (i = 0U; i < size; ++i) aggregate |= bytes[i];
  return aggregate == 0U;
}

spec("TurboMedia crypto") {
  describe("X25519") {
    it("matches the RFC 7748 Alice and Bob vector") {
      static const uint8_t alice_private[32] = {
          0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
          0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
          0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
          0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a};
      static const uint8_t alice_public_expected[32] = {
          0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54,
          0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
          0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
          0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a};
      static const uint8_t bob_private[32] = {
          0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
          0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
          0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
          0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb};
      static const uint8_t bob_public[32] = {
          0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4,
          0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
          0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d,
          0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f};
      static const uint8_t shared_expected[32] = {
          0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1,
          0x72, 0x8e, 0x3b, 0xf4, 0x80, 0x35, 0x0f, 0x25,
          0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33,
          0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42};
      uint8_t alice_public[32];
      uint8_t shared[32];

      check_equal(turbo_media_x25519_public_key(alice_private, alice_public), TURBO_OK);
      check_equal(alice_public, alice_public_expected, sizeof(alice_public));
      check_equal(turbo_media_x25519_shared_secret(alice_private, bob_public, shared), TURBO_OK);
      check_equal(shared, shared_expected, sizeof(shared));

      check_equal(turbo_media_x25519_shared_secret(bob_private, alice_public, shared), TURBO_OK);
      check_equal(shared, shared_expected, sizeof(shared));
    }

    it("generates distinct key pairs and rejects an all-zero peer key") {
      uint8_t private_a[32];
      uint8_t public_a[32];
      uint8_t private_b[32];
      uint8_t public_b[32];
      uint8_t zero_public[32] = {0};
      uint8_t unchanged[32];

      memset(unchanged, 0xa5, sizeof(unchanged));
      check_equal(turbo_media_x25519_keypair_generate(private_a, private_a), TURBO_EINVAL);
      check_equal(turbo_media_x25519_keypair_generate(private_a, public_a), TURBO_OK);
      check_equal(turbo_media_x25519_keypair_generate(private_b, public_b), TURBO_OK);
      check_false(bytes_are_zero(private_a, sizeof(private_a)));
      check_false(bytes_are_zero(public_a, sizeof(public_a)));
      check_not_equal(private_a, private_b, sizeof(private_a));
      check_not_equal(public_a, public_b, sizeof(public_a));
      check_equal(turbo_media_x25519_shared_secret(private_a, zero_public, unchanged),
                   TURBO_EPROTO);
      {
        uint8_t expected[32];
        memset(expected, 0xa5, sizeof(expected));
        check_equal(unchanged, expected, sizeof(unchanged));
      }
    }
  }

  describe("XChaCha20-Poly1305") {
    it("matches the Monocypher 4.0.3 empty-message vector") {
      static const uint8_t key[32] = {
          0xe4, 0xe4, 0xc4, 0x05, 0x4f, 0xe3, 0x5a, 0x75,
          0xd9, 0xc0, 0xf6, 0x79, 0xad, 0x87, 0x70, 0xd8,
          0x22, 0x7e, 0x68, 0xe4, 0xc1, 0xe6, 0x8c, 0xe6,
          0x7e, 0xe8, 0x8e, 0x6b, 0xe2, 0x51, 0xa2, 0x07};
      static const uint8_t nonce[24] = {
          0x48, 0xb3, 0x75, 0x3c, 0xff, 0x3a, 0x6d, 0x99,
          0x01, 0x63, 0xe6, 0xb6, 0x0d, 0xa1, 0xe4, 0xe5,
          0xd6, 0xa2, 0xdf, 0x78, 0xc1, 0x6c, 0x96, 0xa5};
      static const uint8_t expected_tag[16] = {
          0xb5, 0xed, 0x4c, 0x7e, 0x63, 0xa1, 0x44, 0xf1,
          0x05, 0xdb, 0xe2, 0xb0, 0x39, 0xc7, 0xe8, 0x05};
      uint8_t tag[16];

      check_equal(turbo_media_xchacha20poly1305_encrypt(
                       key, nonce, NULL, 0U, NULL, 0U, NULL, tag),
                   TURBO_OK);
      check_equal(tag, expected_tag, sizeof(tag));
    }

    it("round trips associated data and rejects a modified tag") {
      static const uint8_t message[] = "authenticated media payload";
      static const uint8_t associated_data[] = "track=video;seq=42";
      uint8_t key[32];
      uint8_t nonce[24];
      uint8_t cipher[sizeof(message)];
      uint8_t plain[sizeof(message)];
      uint8_t tag[16];

      check_equal(turbo_media_xchacha20_key_generate(key), TURBO_OK);
      check_equal(turbo_media_xchacha20_nonce_generate(nonce), TURBO_OK);
      check_equal(turbo_media_xchacha20poly1305_encrypt(
                       key, nonce, associated_data, sizeof(associated_data) - 1U,
                       message, sizeof(message), cipher, tag),
                   TURBO_OK);
      check_not_equal(cipher, message, sizeof(message));
      check_equal(turbo_media_xchacha20poly1305_decrypt(
                       key, nonce, associated_data, sizeof(associated_data) - 1U,
                       cipher, sizeof(cipher), tag, plain),
                   TURBO_OK);
      check_equal(plain, message, sizeof(message));

      tag[0] ^= 0x01U;
      memset(plain, 0xa5, sizeof(plain));
      check_equal(turbo_media_xchacha20poly1305_decrypt(
                       key, nonce, associated_data, sizeof(associated_data) - 1U,
                       cipher, sizeof(cipher), tag, plain),
                   TURBO_EPROTO);
      {
        uint8_t expected[sizeof(plain)];
        memset(expected, 0xa5, sizeof(expected));
        check_equal(plain, expected, sizeof(plain));
      }

      memcpy(plain, message, sizeof(message));
      check_equal(turbo_media_xchacha20_nonce_generate(nonce), TURBO_OK);
      check_equal(turbo_media_xchacha20poly1305_encrypt(
                       key, nonce, associated_data, sizeof(associated_data) - 1U,
                       plain, sizeof(plain), plain, tag),
                   TURBO_OK);
      check_equal(turbo_media_xchacha20poly1305_decrypt(
                       key, nonce, associated_data, sizeof(associated_data) - 1U,
                       plain, sizeof(plain), tag, plain),
                   TURBO_OK);
      check_equal(plain, message, sizeof(message));

      turbo_media_crypto_wipe(key, sizeof(key));
      check_true(bytes_are_zero(key, sizeof(key)));
    }

    it("validates required buffers") {
      uint8_t key[32] = {0};
      uint8_t nonce[24] = {0};
      uint8_t tag[16] = {0};

      check_equal(turbo_media_xchacha20_key_generate(NULL), TURBO_EINVAL);
      check_equal(turbo_media_xchacha20_nonce_generate(NULL), TURBO_EINVAL);
      check_equal(turbo_media_xchacha20poly1305_encrypt(
                       key, nonce, NULL, 1U, NULL, 0U, NULL, tag),
                   TURBO_EINVAL);
      check_equal(turbo_media_xchacha20poly1305_encrypt(
                       key, nonce, NULL, 0U, NULL, 0U, NULL, tag),
                   TURBO_OK);
    }
  }
}
