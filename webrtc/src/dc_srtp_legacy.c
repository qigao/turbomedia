#include "turbo_srtp_defs.h"
#include <openssl/ssl.h>
#include <cmeta_crypto.h>
#include <string.h>

/* Installed compatibility API: the opaque argument remains a BoringSSL SSL*.
 * Production DataChannel sessions use turbo_dc_peer_get_srtp_keys instead. */
/* Derive SRTP keys from DTLS handshake */
int srtp_derive_keys_from_dtls(void *ssl_ptr,
                               srtp_keying_material_t *keys,
                               uint16_t profile) {
    if (!ssl_ptr || !keys) return -1;

    SSL *ssl = (SSL *)ssl_ptr;

    size_t key_len = srtp_profile_key_len(profile);
    size_t salt_len = srtp_profile_salt_len(profile);
    size_t total_len = 2 * (key_len + salt_len);

    /* Buffer for keying material:
     * client_key || server_key || client_salt || server_salt
     */
    uint8_t keying_material[128];
    if (total_len > sizeof(keying_material)) return -1;

    /* Export keying material using RFC 5705 */
    int result = SSL_export_keying_material(
        ssl,
        keying_material, total_len,
        DTLS_SRTP_PROFILE_LABEL, strlen(DTLS_SRTP_PROFILE_LABEL),
        NULL, 0,
        0  /* No context */
    );

    if (result != 1) {
        cmeta_crypto_clear(keying_material, sizeof(keying_material));
        return -1;
    }

    /* Parse keying material */
    uint8_t *p = keying_material;
    memcpy(keys->client_key, p, key_len);
    p += key_len;
    memcpy(keys->server_key, p, key_len);
    p += key_len;
    memcpy(keys->client_salt, p, salt_len);
    p += salt_len;
    memcpy(keys->server_salt, p, salt_len);

    keys->key_len = key_len;
    keys->salt_len = salt_len;

    /* Clear sensitive data from stack */
    cmeta_crypto_clear(keying_material, sizeof(keying_material));

    return 0;
}

