#ifndef TURBO_MEDIA_REVOCATION_HTTPS_H
#define TURBO_MEDIA_REVOCATION_HTTPS_H

#include "turbo_media_revocation_fanout.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MEDIA_REVOCATION_HTTPS_MAX_TARGETS     TURBO_MEDIA_REVOCATION_FANOUT_MAX_TARGETS
#define TURBO_MEDIA_REVOCATION_HTTPS_ENDPOINT_BYTES 512U
#define TURBO_MEDIA_REVOCATION_HTTPS_PATH_BYTES 128U
#define TURBO_MEDIA_REVOCATION_HTTPS_TLS_NAME_BYTES 256U
#define TURBO_MEDIA_REVOCATION_HTTPS_CA_PATH_BYTES 512U
#define TURBO_MEDIA_REVOCATION_HTTPS_TOKEN_BYTES 4096U

/**
 * Fill one short-lived bearer for exactly one delivery attempt.
 *
 * The adapter owns the output buffer and clears it after the request. The
 * callback must not retain the buffer pointer. Return 0 on success.
 */
typedef int (*turbo_media_revocation_https_token_fn)(
    void *token_context,
    const char *target_id,
    unsigned int attempt,
    char *out_token,
    size_t out_token_capacity);

typedef struct turbo_media_revocation_https_target_config_s {
    const char *target_id;
    const char *base_url;
    const char *ca_file;
    const char *server_name;
} turbo_media_revocation_https_target_config_t;

typedef struct turbo_media_revocation_https_config_s {
    unsigned int timeout_ms;
    turbo_media_revocation_https_token_fn acquire_token;
    void *token_context;
} turbo_media_revocation_https_config_t;

typedef struct turbo_media_revocation_https_s
    turbo_media_revocation_https_t;

turbo_media_revocation_https_t *turbo_media_revocation_https_create(
    const turbo_media_revocation_https_config_t *config,
    const turbo_media_revocation_https_target_config_t *targets,
    size_t target_count);

void turbo_media_revocation_https_destroy(
    turbo_media_revocation_https_t *adapter);

size_t turbo_media_revocation_https_target_count(
    const turbo_media_revocation_https_t *adapter);

int turbo_media_revocation_https_get_fanout_target(
    turbo_media_revocation_https_t *adapter,
    size_t index,
    turbo_media_revocation_fanout_target_t *out_target);

turbo_media_revocation_fanout_transport_result_t
turbo_media_revocation_https_send_snapshot(
    void *transport_context,
    void *target_context,
    const char *target_id,
    uint64_t epoch,
    uint64_t sequence,
    const char *const *sha256_hex,
    size_t count,
    unsigned int attempt,
    turbo_media_revocation_fanout_response_t *response);

turbo_media_revocation_fanout_transport_result_t
turbo_media_revocation_https_send_revoke(
    void *transport_context,
    void *target_context,
    const char *target_id,
    uint64_t epoch,
    uint64_t sequence,
    const char *sha256_hex,
    unsigned int attempt,
    turbo_media_revocation_fanout_response_t *response);

#ifdef __cplusplus
}
#endif

#endif
