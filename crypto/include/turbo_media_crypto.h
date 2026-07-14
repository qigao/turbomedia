#ifndef TURBO_MEDIA_CRYPTO_H
#define TURBO_MEDIA_CRYPTO_H

/**
 * @file turbo_media_crypto.h
 * @brief X25519 key agreement and XChaCha20-Poly1305 authenticated encryption.
 *
 * This API provides cryptographic building blocks, not a complete key exchange
 * protocol. Callers must authenticate peer public keys, derive purpose-specific
 * session keys, and never reuse a nonce with the same XChaCha20 key.
 *
 * @code
 * uint8_t private_key[TURBO_MEDIA_X25519_KEY_SIZE];
 * uint8_t public_key[TURBO_MEDIA_X25519_KEY_SIZE];
 * if (turbo_media_x25519_keypair_generate(private_key, public_key) == TURBO_OK) {
 *   publish_authenticated_public_key(public_key);
 * }
 * turbo_media_crypto_wipe(private_key, sizeof(private_key));
 * @endcode
 */

#include "turbo_error.h"
#include "turbo_export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MEDIA_X25519_KEY_SIZE 32U
#define TURBO_MEDIA_XCHACHA20_KEY_SIZE 32U
#define TURBO_MEDIA_XCHACHA20_NONCE_SIZE 24U
#define TURBO_MEDIA_POLY1305_TAG_SIZE 16U

/**
 * Generate an X25519 private/public key pair using the system CSPRNG.
 * Outputs must not alias and remain unchanged if entropy acquisition fails.
 *
 * @param private_key Destination for the 32-byte private key
 * @param public_key Destination for the 32-byte public key
 * @return TURBO_OK, TURBO_EINVAL, or an entropy acquisition error
 */
CXX_C_API int turbo_media_x25519_keypair_generate(
    uint8_t private_key[TURBO_MEDIA_X25519_KEY_SIZE],
    uint8_t public_key[TURBO_MEDIA_X25519_KEY_SIZE]);

/**
 * Derive an X25519 public key from a 32-byte private key.
 *
 * @param private_key Source private key
 * @param public_key Destination public key; exact input/output aliasing is supported
 * @return TURBO_OK or TURBO_EINVAL
 */
CXX_C_API int turbo_media_x25519_public_key(
    const uint8_t private_key[TURBO_MEDIA_X25519_KEY_SIZE],
    uint8_t public_key[TURBO_MEDIA_X25519_KEY_SIZE]);

/**
 * Compute an X25519 raw shared secret.
 *
 * The function returns TURBO_EPROTO and leaves shared_secret unchanged when
 * the peer key produces the all-zero result. A protocol-specific KDF must be
 * applied before using the result as an encryption key.
 *
 * @param private_key Local private key
 * @param peer_public_key Authenticated peer public key
 * @param shared_secret Destination raw shared secret
 * @return TURBO_OK, TURBO_EINVAL, or TURBO_EPROTO for a low-order peer key
 */
CXX_C_API int turbo_media_x25519_shared_secret(
    const uint8_t private_key[TURBO_MEDIA_X25519_KEY_SIZE],
    const uint8_t peer_public_key[TURBO_MEDIA_X25519_KEY_SIZE],
    uint8_t shared_secret[TURBO_MEDIA_X25519_KEY_SIZE]);

/**
 * Generate a random 32-byte XChaCha20 key using the system CSPRNG.
 *
 * @param key Destination key
 * @return TURBO_OK, TURBO_EINVAL, or an entropy acquisition error
 */
CXX_C_API int turbo_media_xchacha20_key_generate(
    uint8_t key[TURBO_MEDIA_XCHACHA20_KEY_SIZE]);

/**
 * Generate a random 24-byte XChaCha20 nonce.
 * The caller remains responsible for guaranteeing uniqueness per key.
 *
 * @param nonce Destination nonce
 * @return TURBO_OK, TURBO_EINVAL, or an entropy acquisition error
 */
CXX_C_API int turbo_media_xchacha20_nonce_generate(
    uint8_t nonce[TURBO_MEDIA_XCHACHA20_NONCE_SIZE]);

/**
 * Encrypt and authenticate a message with XChaCha20-Poly1305.
 *
 * cipher_text must provide text_size bytes. associated_data and plain_text may
 * be NULL only when their corresponding size is zero. Exact in-place operation
 * (cipher_text == plain_text) is supported; partial overlap is not supported.
 *
 * @param key Secret encryption key
 * @param nonce Nonce that must be unique for this key
 * @param associated_data Authenticated but unencrypted data
 * @param associated_data_size Associated data length
 * @param plain_text Message to encrypt
 * @param text_size Message length and cipher_text capacity
 * @param cipher_text Destination ciphertext
 * @param tag Destination authentication tag
 * @return TURBO_OK or TURBO_EINVAL
 */
CXX_C_API int turbo_media_xchacha20poly1305_encrypt(
    const uint8_t key[TURBO_MEDIA_XCHACHA20_KEY_SIZE],
    const uint8_t nonce[TURBO_MEDIA_XCHACHA20_NONCE_SIZE],
    const uint8_t *associated_data,
    size_t associated_data_size,
    const uint8_t *plain_text,
    size_t text_size,
    uint8_t *cipher_text,
    uint8_t tag[TURBO_MEDIA_POLY1305_TAG_SIZE]);

/**
 * Authenticate and decrypt a message with XChaCha20-Poly1305.
 *
 * Returns TURBO_EPROTO on authentication failure. plain_text remains unchanged
 * on authentication failure. Exact in-place operation is supported.
 *
 * @param key Secret decryption key
 * @param nonce Nonce supplied with the ciphertext
 * @param associated_data Authenticated but unencrypted data
 * @param associated_data_size Associated data length
 * @param cipher_text Ciphertext to authenticate and decrypt
 * @param text_size Ciphertext length and plain_text capacity
 * @param tag Authentication tag supplied with the ciphertext
 * @param plain_text Destination plaintext
 * @return TURBO_OK, TURBO_EINVAL, or TURBO_EPROTO on authentication failure
 */
CXX_C_API int turbo_media_xchacha20poly1305_decrypt(
    const uint8_t key[TURBO_MEDIA_XCHACHA20_KEY_SIZE],
    const uint8_t nonce[TURBO_MEDIA_XCHACHA20_NONCE_SIZE],
    const uint8_t *associated_data,
    size_t associated_data_size,
    const uint8_t *cipher_text,
    size_t text_size,
    const uint8_t tag[TURBO_MEDIA_POLY1305_TAG_SIZE],
    uint8_t *plain_text);

/**
 * Erase secret material through the crypto backend's non-elidable wipe.
 *
 * @param secret Mutable secret buffer; NULL is accepted as a no-op
 * @param size Number of bytes to erase
 */
CXX_C_API void turbo_media_crypto_wipe(void *secret, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_CRYPTO_H */
