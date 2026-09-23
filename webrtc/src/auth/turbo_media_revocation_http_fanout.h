#ifndef TURBO_MEDIA_REVOCATION_HTTP_FANOUT_H
#define TURBO_MEDIA_REVOCATION_HTTP_FANOUT_H

#include "turbo_media_revocation_fanout.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MEDIA_REVOCATION_HTTP_BASE_URL_BYTES 512U
#define TURBO_MEDIA_REVOCATION_HTTP_CA_PATH_BYTES 1024U
#define TURBO_MEDIA_REVOCATION_HTTP_SERVER_NAME_BYTES 256U
#define TURBO_MEDIA_REVOCATION_HTTP_BEARER_BYTES 4096U
#define TURBO_MEDIA_REVOCATION_HTTP_RESPONSE_BYTES 1024U

typedef struct turbo_media_revocation_http_fanout_s
    turbo_media_revocation_http_fanout_t;

typedef int (*turbo_media_revocation_http_token_provider_fn)(
    void *context, const char *target_id, char *token, size_t token_capacity);

typedef struct turbo_media_revocation_http_target_s {
    const char *target_id;
    const char *base_url;
    const char *ca_file;
    const char *server_name;
} turbo_media_revocation_http_target_t;

typedef struct turbo_media_revocation_http_fanout_config_s {
    unsigned int max_attempts;
    uint32_t connect_timeout_ms;
    uint32_t read_timeout_ms;
    uint32_t write_timeout_ms;
    turbo_media_revocation_http_token_provider_fn token_provider;
    void *token_context;
} turbo_media_revocation_http_fanout_config_t;

typedef struct turbo_media_revocation_http_response_s {
    unsigned int status_code;
    size_t body_size;
    char body[TURBO_MEDIA_REVOCATION_HTTP_RESPONSE_BYTES];
} turbo_media_revocation_http_response_t;

typedef int (*turbo_media_revocation_http_request_fn)(
    void *context,
    const turbo_media_revocation_http_target_t *target,
    const char *path,
    const char *bearer_token,
    const char *body,
    size_t body_size,
    uint32_t connect_timeout_ms,
    uint32_t read_timeout_ms,
    uint32_t write_timeout_ms,
    turbo_media_revocation_http_response_t *response);

typedef struct turbo_media_revocation_http_dependencies_s {
    turbo_media_revocation_http_request_fn request;
    void *context;
} turbo_media_revocation_http_dependencies_t;

/**
 * Create a production HTTPS fan-out adapter.
 *
 * Targets are copied, bounded, and must be HTTPS origins with no base path or
 * query. CA file and server name are mandatory. The bearer provider is invoked
 * for every transport attempt; returned bearer text is borrowed only by that
 * attempt and is wiped immediately afterwards.
 */
turbo_media_revocation_http_fanout_t *
turbo_media_revocation_http_fanout_create(
    const turbo_media_revocation_http_fanout_config_t *config,
    const turbo_media_revocation_http_target_t *targets,
    size_t target_count);

/**
 * Internal dependency-injected constructor used by deterministic contract
 * tests/embedders. Target validation and bearer lifetime rules are identical
 * to the production constructor.
 */
turbo_media_revocation_http_fanout_t *
turbo_media_revocation_http_fanout_create_with_dependencies(
    const turbo_media_revocation_http_fanout_config_t *config,
    const turbo_media_revocation_http_target_t *targets,
    size_t target_count,
    const turbo_media_revocation_http_dependencies_t *dependencies);

void turbo_media_revocation_http_fanout_destroy(
    turbo_media_revocation_http_fanout_t *fanout);

int turbo_media_revocation_http_fanout_publish_snapshot(
    turbo_media_revocation_http_fanout_t *fanout,
    uint64_t epoch,
    uint64_t sequence,
    const char *const *sha256_hex,
    size_t count,
    turbo_media_revocation_fanout_report_t *report);

int turbo_media_revocation_http_fanout_publish_revoke(
    turbo_media_revocation_http_fanout_t *fanout,
    uint64_t epoch,
    uint64_t sequence,
    const char *sha256_hex,
    const char *const *covering_sha256_hex,
    size_t covering_count,
    turbo_media_revocation_fanout_report_t *report);

size_t turbo_media_revocation_http_fanout_target_count(
    const turbo_media_revocation_http_fanout_t *fanout);

int turbo_media_revocation_http_fanout_get_target_status(
    const turbo_media_revocation_http_fanout_t *fanout,
    size_t index,
    turbo_media_revocation_fanout_target_status_t *status);

#ifdef __cplusplus
}
#endif

#endif
