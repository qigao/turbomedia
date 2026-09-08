#include "turbo_media_crypto.h"

#include "monocypher.h"
#include "platform.h"

#include <string.h>

static int turbo_media_crypto_buffer_valid(const void *buffer, size_t size) {
  return buffer != NULL || size == 0U;
}

int turbo_media_x25519_keypair_generate(
    uint8_t private_key[TURBO_MEDIA_X25519_KEY_SIZE],
    uint8_t public_key[TURBO_MEDIA_X25519_KEY_SIZE]) {
  uint8_t generated_private[TURBO_MEDIA_X25519_KEY_SIZE];
  uint8_t generated_public[TURBO_MEDIA_X25519_KEY_SIZE];
  int rc;

  if (!private_key || !public_key || private_key == public_key) return SALTS_EINVAL;

  rc = salts_secure_random(generated_private, sizeof(generated_private));
  if (rc != SALTS_OK) return rc;

  crypto_x25519_public_key(generated_public, generated_private);
  memcpy(private_key, generated_private, sizeof(generated_private));
  memcpy(public_key, generated_public, sizeof(generated_public));
  crypto_wipe(generated_private, sizeof(generated_private));
  crypto_wipe(generated_public, sizeof(generated_public));
  return SALTS_OK;
}

int turbo_media_x25519_public_key(
    const uint8_t private_key[TURBO_MEDIA_X25519_KEY_SIZE],
    uint8_t public_key[TURBO_MEDIA_X25519_KEY_SIZE]) {
  uint8_t generated[TURBO_MEDIA_X25519_KEY_SIZE];

  if (!private_key || !public_key) return SALTS_EINVAL;

  crypto_x25519_public_key(generated, private_key);
  memcpy(public_key, generated, sizeof(generated));
  crypto_wipe(generated, sizeof(generated));
  return SALTS_OK;
}

int turbo_media_x25519_shared_secret(
    const uint8_t private_key[TURBO_MEDIA_X25519_KEY_SIZE],
    const uint8_t peer_public_key[TURBO_MEDIA_X25519_KEY_SIZE],
    uint8_t shared_secret[TURBO_MEDIA_X25519_KEY_SIZE]) {
  uint8_t generated[TURBO_MEDIA_X25519_KEY_SIZE];
  uint8_t aggregate = 0U;
  size_t i;

  if (!private_key || !peer_public_key || !shared_secret) return SALTS_EINVAL;

  crypto_x25519(generated, private_key, peer_public_key);
  for (i = 0U; i < sizeof(generated); ++i) aggregate |= generated[i];
  if (aggregate == 0U) {
    crypto_wipe(generated, sizeof(generated));
    return SALTS_EPROTO;
  }

  memcpy(shared_secret, generated, sizeof(generated));
  crypto_wipe(generated, sizeof(generated));
  return SALTS_OK;
}

int turbo_media_xchacha20_key_generate(uint8_t key[TURBO_MEDIA_XCHACHA20_KEY_SIZE]) {
  if (!key) return SALTS_EINVAL;
  return salts_secure_random(key, TURBO_MEDIA_XCHACHA20_KEY_SIZE);
}

int turbo_media_xchacha20_nonce_generate(
    uint8_t nonce[TURBO_MEDIA_XCHACHA20_NONCE_SIZE]) {
  if (!nonce) return SALTS_EINVAL;
  return salts_secure_random(nonce, TURBO_MEDIA_XCHACHA20_NONCE_SIZE);
}

int turbo_media_xchacha20poly1305_encrypt(
    const uint8_t key[TURBO_MEDIA_XCHACHA20_KEY_SIZE],
    const uint8_t nonce[TURBO_MEDIA_XCHACHA20_NONCE_SIZE],
    const uint8_t *associated_data,
    size_t associated_data_size,
    const uint8_t *plain_text,
    size_t text_size,
    uint8_t *cipher_text,
    uint8_t tag[TURBO_MEDIA_POLY1305_TAG_SIZE]) {
  if (!key || !nonce || !tag || !turbo_media_crypto_buffer_valid(associated_data,
                                                                  associated_data_size) ||
      !turbo_media_crypto_buffer_valid(plain_text, text_size) ||
      !turbo_media_crypto_buffer_valid(cipher_text, text_size)) {
    return SALTS_EINVAL;
  }

  crypto_aead_lock(cipher_text, tag, key, nonce, associated_data, associated_data_size,
                   plain_text, text_size);
  return SALTS_OK;
}

int turbo_media_xchacha20poly1305_decrypt(
    const uint8_t key[TURBO_MEDIA_XCHACHA20_KEY_SIZE],
    const uint8_t nonce[TURBO_MEDIA_XCHACHA20_NONCE_SIZE],
    const uint8_t *associated_data,
    size_t associated_data_size,
    const uint8_t *cipher_text,
    size_t text_size,
    const uint8_t tag[TURBO_MEDIA_POLY1305_TAG_SIZE],
    uint8_t *plain_text) {
  int mismatch;

  if (!key || !nonce || !tag || !turbo_media_crypto_buffer_valid(associated_data,
                                                                  associated_data_size) ||
      !turbo_media_crypto_buffer_valid(cipher_text, text_size) ||
      !turbo_media_crypto_buffer_valid(plain_text, text_size)) {
    return SALTS_EINVAL;
  }

  mismatch = crypto_aead_unlock(plain_text, tag, key, nonce, associated_data,
                                associated_data_size, cipher_text, text_size);
  return mismatch == 0 ? SALTS_OK : SALTS_EPROTO;
}

void turbo_media_crypto_wipe(void *secret, size_t size) {
  if (secret && size > 0U) crypto_wipe(secret, size);
}
