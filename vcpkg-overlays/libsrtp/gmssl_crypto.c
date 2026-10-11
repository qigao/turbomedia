/* libSRTP provider adapter. Packet policy and replay handling stay in libSRTP;
 * cipher primitives and authentication are supplied by the cached GmSSL. */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include "alloc.h"
#include "auth.h"
#include "err.h"
#include "cipher.h"
#include "cipher_types.h"
#include "crypto_types.h"
#include "cipher_test_cases.h"
#include "../hash/auth_test_cases.h"
#include <gmssl/aes.h>
#include <gmssl/hmac.h>
#include <gmssl/mem.h>
#include <limits.h>
#include <string.h>

srtp_debug_module_t srtp_mod_aes_icm = {0, "aes icm gmssl"};
srtp_debug_module_t srtp_mod_aes_gcm = {0, "aes gcm gmssl"};
srtp_debug_module_t srtp_mod_hmac = {0, "hmac sha-1 gmssl"};

typedef struct {
    AES_KEY key;
    uint8_t offset[16], counter[16], stream[16];
    size_t used;
    int key_size;
} gmssl_icm_t;

static srtp_err_status_t icm_alloc(srtp_cipher_t **out, int key_len, int tag_len)
{
    srtp_cipher_t *cipher;
    gmssl_icm_t *state;
    const srtp_cipher_type_t *type;
    (void)tag_len;
    *out = NULL;
    switch (key_len) {
    case SRTP_AES_ICM_128_KEY_LEN_WSALT: type = &srtp_aes_icm_128; break;
    case SRTP_AES_ICM_192_KEY_LEN_WSALT: type = &srtp_aes_icm_192; break;
    case SRTP_AES_ICM_256_KEY_LEN_WSALT: type = &srtp_aes_icm_256; break;
    default: return srtp_err_status_bad_param;
    }
    cipher = srtp_crypto_alloc(sizeof(*cipher));
    state = srtp_crypto_alloc(sizeof(*state));
    if (!cipher || !state) {
        srtp_crypto_free(state);
        srtp_crypto_free(cipher);
        return srtp_err_status_alloc_fail;
    }
    memset(state, 0, sizeof(*state));
    state->key_size = key_len - SRTP_SALT_LEN;
    cipher->type = type;
    cipher->state = state;
    cipher->key_len = key_len;
    cipher->algorithm = type->id;
    *out = cipher;
    return srtp_err_status_ok;
}

static srtp_err_status_t icm_dealloc(srtp_cipher_t *cipher)
{
    gmssl_secure_clear(cipher->state, sizeof(gmssl_icm_t));
    srtp_crypto_free(cipher->state);
    srtp_crypto_free(cipher);
    return srtp_err_status_ok;
}

static srtp_err_status_t icm_init(void *opaque, const uint8_t *key)
{
    gmssl_icm_t *state = opaque;
    if (aes_set_encrypt_key(&state->key, key, state->key_size) != 1)
        return srtp_err_status_init_fail;
    memset(state->offset, 0, sizeof(state->offset));
    memcpy(state->offset, key + state->key_size, SRTP_SALT_LEN);
    memcpy(state->counter, state->offset, sizeof(state->counter));
    state->used = sizeof(state->stream);
    return srtp_err_status_ok;
}

static srtp_err_status_t icm_set_iv(void *opaque, uint8_t *iv,
                                 srtp_cipher_direction_t direction)
{
    gmssl_icm_t *state = opaque;
    size_t i;
    (void)direction;
    for (i = 0; i < sizeof(state->counter); ++i)
        state->counter[i] = state->offset[i] ^ iv[i];
    state->used = sizeof(state->stream);
    return srtp_err_status_ok;
}

static srtp_err_status_t icm_crypt(void *opaque, uint8_t *buffer,
                                unsigned int *length)
{
    gmssl_icm_t *state = opaque;
    size_t remaining = *length;
    /* GmSSL's one-shot CTR consumes a full counter block even for a short
     * input. Preserve its unused keystream across libSRTP's split calls. */
    while (remaining && state->used < sizeof(state->stream)) {
        *buffer++ ^= state->stream[state->used++];
        --remaining;
    }
    if (remaining >= sizeof(state->stream)) {
        size_t whole = remaining - remaining % sizeof(state->stream);
        aes_ctr_encrypt(&state->key, state->counter, buffer, whole, buffer);
        buffer += whole;
        remaining -= whole;
    }
    if (remaining) {
        memset(state->stream, 0, sizeof(state->stream));
        aes_ctr_encrypt(&state->key, state->counter, state->stream,
                        sizeof(state->stream), state->stream);
        state->used = 0;
        while (remaining--) *buffer++ ^= state->stream[state->used++];
    }
    return srtp_err_status_ok;
}

#define ICM_TYPE(bits) \
    const srtp_cipher_type_t srtp_aes_icm_##bits = { \
        icm_alloc, icm_dealloc, icm_init, NULL, icm_crypt, icm_crypt, \
        icm_set_iv, NULL, "AES-" #bits " counter mode using GmSSL", \
        &srtp_aes_icm_##bits##_test_case_0, SRTP_AES_ICM_##bits }
ICM_TYPE(128);
ICM_TYPE(192);
ICM_TYPE(256);

typedef struct {
    AES_KEY key;
    uint8_t iv[12], tag[16];
    uint8_t *aad;
    size_t aad_length, aad_capacity;
    int key_size, tag_length, iv_ready, tag_ready;
    srtp_cipher_direction_t direction;
} gmssl_gcm_t;

static srtp_err_status_t gcm_alloc(srtp_cipher_t **out, int key_len, int tag_len)
{
    srtp_cipher_t *cipher;
    gmssl_gcm_t *state;
    const srtp_cipher_type_t *type;
    *out = NULL;
    if (tag_len != 8 && tag_len != 16) return srtp_err_status_bad_param;
    switch (key_len) {
    case SRTP_AES_GCM_128_KEY_LEN_WSALT: type = &srtp_aes_gcm_128; break;
    case SRTP_AES_GCM_256_KEY_LEN_WSALT: type = &srtp_aes_gcm_256; break;
    default: return srtp_err_status_bad_param;
    }
    cipher = srtp_crypto_alloc(sizeof(*cipher));
    state = srtp_crypto_alloc(sizeof(*state));
    if (!cipher || !state) {
        srtp_crypto_free(state);
        srtp_crypto_free(cipher);
        return srtp_err_status_alloc_fail;
    }
    memset(state, 0, sizeof(*state));
    state->key_size = key_len == SRTP_AES_GCM_128_KEY_LEN_WSALT ? 16 : 32;
    state->tag_length = tag_len;
    cipher->type = type;
    cipher->state = state;
    cipher->key_len = key_len;
    cipher->algorithm = type->id;
    *out = cipher;
    return srtp_err_status_ok;
}

static srtp_err_status_t gcm_dealloc(srtp_cipher_t *cipher)
{
    gmssl_gcm_t *state = cipher->state;
    if (state->aad) {
        gmssl_secure_clear(state->aad, state->aad_capacity);
        srtp_crypto_free(state->aad);
    }
    gmssl_secure_clear(state, sizeof(*state));
    srtp_crypto_free(state);
    srtp_crypto_free(cipher);
    return srtp_err_status_ok;
}

static srtp_err_status_t gcm_init(void *opaque, const uint8_t *key)
{
    gmssl_gcm_t *state = opaque;
    state->iv_ready = state->tag_ready = 0;
    state->aad_length = 0;
    return aes_set_encrypt_key(&state->key, key, state->key_size) == 1
        ? srtp_err_status_ok : srtp_err_status_init_fail;
}

static srtp_err_status_t gcm_set_iv(void *opaque, uint8_t *iv,
                                 srtp_cipher_direction_t direction)
{
    gmssl_gcm_t *state = opaque;
    if (direction != srtp_direction_encrypt && direction != srtp_direction_decrypt)
        return srtp_err_status_bad_param;
    memcpy(state->iv, iv, sizeof(state->iv));
    state->direction = direction;
    state->aad_length = 0;
    state->tag_ready = 0;
    state->iv_ready = 1;
    return srtp_err_status_ok;
}

static srtp_err_status_t gcm_set_aad(void *opaque, const uint8_t *aad,
                                  uint32_t length)
{
    gmssl_gcm_t *state = opaque;
    size_t needed;
    if (!state->iv_ready || state->tag_ready || (!aad && length) ||
        length > (size_t)INT_MAX - state->aad_length)
        return srtp_err_status_bad_param;
    needed = state->aad_length + length;
    if (needed > state->aad_capacity) {
        uint8_t *storage = srtp_crypto_alloc(needed);
        if (!storage) return srtp_err_status_alloc_fail;
        if (state->aad_length) memcpy(storage, state->aad, state->aad_length);
        if (state->aad) {
            gmssl_secure_clear(state->aad, state->aad_capacity);
            srtp_crypto_free(state->aad);
        }
        state->aad = storage;
        state->aad_capacity = needed;
    }
    if (length) memcpy(state->aad + state->aad_length, aad, length);
    state->aad_length = needed;
    return srtp_err_status_ok;
}

static srtp_err_status_t gcm_encrypt(void *opaque, uint8_t *buffer,
                                   unsigned int *length)
{
    gmssl_gcm_t *state = opaque;
    if (!state->iv_ready || state->tag_ready ||
        state->direction != srtp_direction_encrypt)
        return srtp_err_status_bad_param;
    if (aes_gcm_encrypt(&state->key, state->iv, sizeof(state->iv),
                        state->aad, state->aad_length, buffer, *length,
                        buffer, state->tag_length, state->tag) != 1) {
        state->iv_ready = 0;
        return srtp_err_status_cipher_fail;
    }
    state->tag_ready = 1;
    return srtp_err_status_ok;
}

static srtp_err_status_t gcm_get_tag(void *opaque, uint8_t *tag, uint32_t *length)
{
    gmssl_gcm_t *state = opaque;
    /* Authentication-only SRTCP calls get_tag without encrypting a payload. */
    if (!state->tag_ready) {
        unsigned int empty = 0;
        srtp_err_status_t status = gcm_encrypt(state, NULL, &empty);
        if (status != srtp_err_status_ok) return status;
    }
    memcpy(tag, state->tag, state->tag_length);
    *length = state->tag_length;
    return srtp_err_status_ok;
}

static srtp_err_status_t gcm_decrypt(void *opaque, uint8_t *buffer,
                                   unsigned int *length)
{
    gmssl_gcm_t *state = opaque;
    size_t payload;
    int result;
    if (!state->iv_ready || state->direction != srtp_direction_decrypt ||
        *length < (unsigned int)state->tag_length)
        return srtp_err_status_bad_param;
    payload = *length - state->tag_length;
    /* The cached provider verifies the tag with gmssl_secure_memcmp before
     * decrypting. Failed authentication leaves ciphertext and length intact. */
    result = aes_gcm_decrypt(&state->key, state->iv, sizeof(state->iv),
                            state->aad, state->aad_length, buffer, payload,
                            buffer + payload, state->tag_length, buffer);
    state->iv_ready = 0;
    if (result != 1) return srtp_err_status_auth_fail;
    *length = (unsigned int)payload;
    return srtp_err_status_ok;
}

#define GCM_TYPE(bits) \
    const srtp_cipher_type_t srtp_aes_gcm_##bits = { \
        gcm_alloc, gcm_dealloc, gcm_init, gcm_set_aad, gcm_encrypt, gcm_decrypt, \
        gcm_set_iv, gcm_get_tag, "AES-" #bits " GCM using GmSSL", \
        &srtp_aes_gcm_##bits##_test_case_0, SRTP_AES_GCM_##bits }
GCM_TYPE(128);
GCM_TYPE(256);

typedef struct {
    HMAC_CTX initial, active;
} gmssl_hmac_t;

static srtp_err_status_t mac_alloc(srtp_auth_t **out, int key_len, int tag_len)
{
    srtp_auth_t *auth;
    *out = NULL;
    if (key_len < 0 || tag_len < 0 || tag_len > 20)
        return srtp_err_status_bad_param;
    auth = srtp_crypto_alloc(sizeof(*auth));
    if (!auth) return srtp_err_status_alloc_fail;
    auth->state = srtp_crypto_alloc(sizeof(gmssl_hmac_t));
    if (!auth->state) {
        srtp_crypto_free(auth);
        return srtp_err_status_alloc_fail;
    }
    memset(auth->state, 0, sizeof(gmssl_hmac_t));
    auth->type = &srtp_hmac;
    auth->key_len = key_len;
    auth->out_len = tag_len;
    auth->prefix_len = 0;
    *out = auth;
    return srtp_err_status_ok;
}

static srtp_err_status_t mac_dealloc(srtp_auth_t *auth)
{
    gmssl_secure_clear(auth->state, sizeof(gmssl_hmac_t));
    srtp_crypto_free(auth->state);
    gmssl_secure_clear(auth, sizeof(*auth));
    srtp_crypto_free(auth);
    return srtp_err_status_ok;
}

static srtp_err_status_t mac_start(void *opaque)
{
    gmssl_hmac_t *state = opaque;
    state->active = state->initial;
    return srtp_err_status_ok;
}

static srtp_err_status_t mac_init(void *opaque, const uint8_t *key, int length)
{
    gmssl_hmac_t *state = opaque;
    if (length < 0 || hmac_init(&state->initial, DIGEST_sha1(), key, length) != 1)
        return srtp_err_status_init_fail;
    return mac_start(state);
}

static srtp_err_status_t mac_update(void *opaque, const uint8_t *message, int length)
{
    gmssl_hmac_t *state = opaque;
    if (length < 0 || hmac_update(&state->active, message, length) != 1)
        return srtp_err_status_auth_fail;
    return srtp_err_status_ok;
}

static srtp_err_status_t mac_compute(void *opaque, const uint8_t *message,
                                   int length, int tag_len, uint8_t *tag)
{
    gmssl_hmac_t *state = opaque;
    uint8_t digest[HMAC_MAX_SIZE];
    size_t digest_len = 0;
    int result;
    if (tag_len < 0 || tag_len > 20 || (!tag && tag_len))
        return srtp_err_status_bad_param;
    if (mac_update(state, message, length) != srtp_err_status_ok)
        return srtp_err_status_auth_fail;
    result = hmac_finish(&state->active, digest, &digest_len);
    if (result == 1 && digest_len == 20 && tag_len) memcpy(tag, digest, tag_len);
    gmssl_secure_clear(digest, sizeof(digest));
    return result == 1 && digest_len == 20
        ? srtp_err_status_ok : srtp_err_status_auth_fail;
}

const srtp_auth_type_t srtp_hmac = {
    mac_alloc, mac_dealloc, mac_init, mac_compute, mac_update, mac_start,
    "HMAC-SHA1 using GmSSL", &srtp_hmac_test_case_0, SRTP_HMAC_SHA1
};
