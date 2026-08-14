#ifndef TURBO_ROOM_SERVICE_IRIS_COMMAND_FINGERPRINT_H
#define TURBO_ROOM_SERVICE_IRIS_COMMAND_FINGERPRINT_H

#include <turbo_crypto.h>

#include <stddef.h>
#include <stdint.h>

typedef struct iris_command_fingerprint_s {
    turbo_crypto_sha256_ctx_t hash;
    int valid;
} iris_command_fingerprint_t;

int iris_command_fingerprint_init(iris_command_fingerprint_t *fingerprint);
int iris_command_fingerprint_add_bytes(
    iris_command_fingerprint_t *fingerprint, const void *data, size_t size);
int iris_command_fingerprint_add_text(
    iris_command_fingerprint_t *fingerprint, const char *text);
int iris_command_fingerprint_add_u64(
    iris_command_fingerprint_t *fingerprint, uint64_t value);
int iris_command_fingerprint_final(
    iris_command_fingerprint_t *fingerprint, char output[65]);

#endif
