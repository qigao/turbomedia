/**
 * TurboNet SRTP Definitions
 *
 * Constants and types for DTLS-SRTP negotiation and key derivation.
 * Shared between DataChannel (key negotiation) and Media (packet protection).
 */
#ifndef TURBO_SRTP_DEFS_H
#define TURBO_SRTP_DEFS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define SRTP_MAX_KEY_LEN        32
#define SRTP_MAX_SALT_LEN       14
#define SRTP_AUTH_TAG_LEN       10      /* HMAC-SHA1-80 */
#define SRTP_MAX_TRAILER_LEN    (SRTP_AUTH_TAG_LEN + 4)  /* Tag + potential MKI */

/* SRTP Profiles (RFC 5764) */
#define SRTP_PROFILE_AES128_CM_SHA1_80      0x0001
#define SRTP_PROFILE_AES128_CM_SHA1_32      0x0002
#define SRTP_PROFILE_AEAD_AES_128_GCM       0x0007
#define SRTP_PROFILE_AEAD_AES_256_GCM       0x0008

/* =============================================================================
 * Keys
 * ============================================================================= */

/**
 * DTLS-SRTP keying material
 * Derived from DTLS master secret using PRF
 */
typedef struct {
    uint8_t client_key[SRTP_MAX_KEY_LEN];
    uint8_t client_salt[SRTP_MAX_SALT_LEN];
    uint8_t server_key[SRTP_MAX_KEY_LEN];
    uint8_t server_salt[SRTP_MAX_SALT_LEN];
    size_t key_len;
    size_t salt_len;
} srtp_keying_material_t;

/* =============================================================================
 * DTLS-SRTP Key Derivation Functions
 * ============================================================================= */

/**
 * SRTP profile negotiation label for DTLS extension
 */
#define DTLS_SRTP_PROFILE_LABEL "EXTRACTOR-dtls_srtp"

/**
 * Derive SRTP keys from DTLS session
 * Called after DTLS handshake completes
 *
 * @param ssl           OpenSSL SSL pointer
 * @param keys          Output keying material
 * @param profile       Negotiated SRTP profile
 * @return              0 on success, -1 on error
 */
int srtp_derive_keys_from_dtls(void *ssl,
                               srtp_keying_material_t *keys,
                               uint16_t profile);

/**
 * Get SRTP key length for profile
 */
size_t srtp_profile_key_len(uint16_t profile);

/**
 * Get SRTP salt length for profile
 */
size_t srtp_profile_salt_len(uint16_t profile);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SRTP_DEFS_H */
