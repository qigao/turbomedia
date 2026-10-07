#ifndef TURBO_MEDIA_REVOCATION_WIRE_H
#define TURBO_MEDIA_REVOCATION_WIRE_H

#include "turbo_media_auth.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MEDIA_REVOCATION_WIRE_SCHEMA_VERSION 1U
#define TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES (32U * 1024U)
#define TURBO_MEDIA_REVOCATION_WIRE_SHA256_HEX_BYTES 64U

typedef struct turbo_media_revocation_wire_snapshot_s {
    uint32_t epoch;
    uint32_t sequence;
    size_t count;
    char storage[
        TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS *
            (TURBO_MEDIA_REVOCATION_WIRE_SHA256_HEX_BYTES + 1U)];
    const char *digests[TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS];
} turbo_media_revocation_wire_snapshot_t;

typedef struct turbo_media_revocation_wire_revoke_s {
    uint32_t epoch;
    uint32_t sequence;
    char sha256[TURBO_MEDIA_REVOCATION_WIRE_SHA256_HEX_BYTES + 1U];
} turbo_media_revocation_wire_revoke_t;

int turbo_media_revocation_wire_parse_snapshot(
    const char *body, size_t body_size,
    turbo_media_revocation_wire_snapshot_t *out_snapshot);

int turbo_media_revocation_wire_parse_revoke(
    const char *body, size_t body_size,
    turbo_media_revocation_wire_revoke_t *out_revoke);

#ifdef __cplusplus
}
#endif

#endif
