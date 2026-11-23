#ifndef TURBO_MEDIA_TEST_SCTP_COOKIE_MAC_H
#define TURBO_MEDIA_TEST_SCTP_COOKIE_MAC_H

#include <crypto/sm3.h>
#include <crypto/symmetric.h>
#include <string.h>

enum { SCTP_COOKIE_KEY_BYTES = 32 };

typedef struct {
    uint8_t key[SCTP_COOKIE_KEY_BYTES];
    uint8_t previous_key[SCTP_COOKIE_KEY_BYTES];
    unsigned active;
    unsigned previous_active;
} sctp_cookie_mac;

static inline void sctp_cookie_mac_retire(sctp_cookie_mac *mac) {
    volatile uint8_t *bytes = (volatile uint8_t *)mac;
    for (size_t i = 0; i < sizeof(*mac); ++i) bytes[i] = 0;
}

static inline int sctp_cookie_mac_init(sctp_cookie_mac *mac, const uint8_t *key, size_t n) {
    if (!mac || !key || n != sizeof(mac->key)) return -1;
    sctp_cookie_mac_retire(mac);
    memcpy(mac->key, key, n);
    mac->active = 1;
    return 0;
}

static inline int sctp_cookie_mac_rotate(sctp_cookie_mac *mac, const uint8_t *key, size_t n) {
    if (!mac || !mac->active || !key || n != sizeof(mac->key) || mac->previous_active) return -1;
    memcpy(mac->previous_key, mac->key, n);
    memcpy(mac->key, key, n);
    mac->previous_active = 1;
    return 0;
}

static inline int sctp_cookie_mac_sign(const sctp_cookie_mac *mac, const void *data,
                                      size_t n, uint8_t out[32]) {
    if (!mac || !mac->active) return -1;
    return salts_crypto_hmac_sm3(mac->key, sizeof(mac->key), data, n, out);
}

static inline int sctp_cookie_mac_matches(const uint8_t *key, const void *data,
                                         size_t n, const uint8_t claimed[32]) {
    uint8_t expected[32], difference = 0;
    if (salts_crypto_hmac_sm3(key, SCTP_COOKIE_KEY_BYTES, data, n, expected) != 0) return 0;
    for (size_t i = 0; i < sizeof(expected); ++i) difference |= expected[i] ^ claimed[i];
    return difference == 0;
}

static inline int sctp_cookie_mac_verify(const sctp_cookie_mac *mac, const void *data,
                                        size_t n, const uint8_t claimed[32]) {
    if (!mac || !mac->active || !claimed) return -1;
    int valid = sctp_cookie_mac_matches(mac->key, data, n, claimed);
    if (mac->previous_active) valid |= sctp_cookie_mac_matches(mac->previous_key, data, n, claimed);
    return valid ? 0 : -1;
}

#endif
