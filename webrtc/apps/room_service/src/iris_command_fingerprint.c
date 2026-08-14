#include "iris_command_fingerprint.h"

#include <string.h>

int iris_command_fingerprint_init(iris_command_fingerprint_t *fingerprint) {
    if (!fingerprint) return 0;
    memset(fingerprint, 0, sizeof(*fingerprint));
    fingerprint->valid =
        turbo_crypto_sha256_init(&fingerprint->hash) == 0;
    return fingerprint->valid;
}

int iris_command_fingerprint_add_bytes(
    iris_command_fingerprint_t *fingerprint, const void *data, size_t size) {
    uint8_t length[8];
    uint64_t value = (uint64_t)size;
    size_t i;
    if (!fingerprint || !fingerprint->valid || (!data && size != 0u)) {
        return 0;
    }
    for (i = 0u; i < sizeof(length); ++i) {
        length[i] = (uint8_t)(value & UINT64_C(0xff));
        value >>= 8u;
    }
    fingerprint->valid =
        turbo_crypto_sha256_update(&fingerprint->hash, length,
                                   sizeof(length)) == 0 &&
        turbo_crypto_sha256_update(&fingerprint->hash, data, size) == 0;
    return fingerprint->valid;
}

int iris_command_fingerprint_add_text(
    iris_command_fingerprint_t *fingerprint, const char *text) {
    return text && iris_command_fingerprint_add_bytes(
                       fingerprint, text, strlen(text));
}

int iris_command_fingerprint_add_u64(
    iris_command_fingerprint_t *fingerprint, uint64_t value) {
    uint8_t bytes[8];
    size_t i;
    for (i = 0u; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)(value & UINT64_C(0xff));
        value >>= 8u;
    }
    return iris_command_fingerprint_add_bytes(fingerprint, bytes,
                                              sizeof(bytes));
}

int iris_command_fingerprint_final(
    iris_command_fingerprint_t *fingerprint, char output[65]) {
    uint8_t digest[TURBO_CRYPTO_SHA256_SIZE];
    static const char hex[] = "0123456789abcdef";
    size_t i;
    if (!fingerprint || !fingerprint->valid || !output ||
        turbo_crypto_sha256_final(&fingerprint->hash, digest) != 0) {
        return 0;
    }
    for (i = 0u; i < sizeof(digest); ++i) {
        output[i * 2u] = hex[digest[i] >> 4u];
        output[i * 2u + 1u] = hex[digest[i] & 0x0fu];
    }
    output[64] = '\0';
    fingerprint->valid = 0;
    return 1;
}
