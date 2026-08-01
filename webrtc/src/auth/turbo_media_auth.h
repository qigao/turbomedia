#ifndef TURBO_MEDIA_AUTH_H
#define TURBO_MEDIA_AUTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MEDIA_AUTH_MIN_SECRET_BYTES 32
#define TURBO_MEDIA_AUTH_DEFAULT_CLOCK_SKEW_SECONDS 30
#define TURBO_MEDIA_AUTH_DEFAULT_MAX_TTL_SECONDS 3600
#define TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS 256

typedef struct turbo_media_auth_config_s {
    const char *issuer;
    const char *active_key_id;
    const char *active_secret;
    const char *previous_key_id;
    const char *previous_secret;
    /** Comma-separated lowercase SHA-256 digests of revoked compact tokens. */
    const char *revoked_token_sha256;
    int clock_skew_seconds;
    int max_ttl_seconds;
} turbo_media_auth_config_t;

typedef struct turbo_media_auth_policy_s {
    const char *audience;
    const char *required_scope;
    const char *room_id;
    const char *participant_id;
    int64_t now;
} turbo_media_auth_policy_t;

typedef struct turbo_media_auth_claims_s {
    const char *subject;
    const char *audience;
    const char *scope;
    const char *room_id;
    const char *participant_id;
    int64_t issued_at;
    int64_t expires_at;
} turbo_media_auth_claims_t;

typedef enum turbo_media_auth_result_e {
    TURBO_MEDIA_AUTH_DENIED = 0,
    TURBO_MEDIA_AUTH_STATIC_TOKEN = 1,
    TURBO_MEDIA_AUTH_SIGNED_TOKEN = 2
} turbo_media_auth_result_t;

int turbo_media_auth_config_enabled(const turbo_media_auth_config_t *config);
int turbo_media_auth_config_validate(const turbo_media_auth_config_t *config);

/**
 * Authorize one HTTP Authorization header.
 *
 * The static token and signed-token verifier are independent, explicit
 * compatibility modes. If both are configured, either may authorize the
 * request. Resource claims are exact: a policy without room_id or
 * participant_id only accepts an equally unbound token.
 */
turbo_media_auth_result_t turbo_media_auth_authorize(
    const char *authorization,
    const char *static_token,
    const turbo_media_auth_config_t *config,
    const turbo_media_auth_policy_t *policy);

/**
 * Authorize a raw signed access token.
 *
 * This entry point is intended for protocols such as browser WebSocket
 * messages that cannot attach an HTTP Authorization header.
 */
turbo_media_auth_result_t turbo_media_auth_authorize_token(
    const char *token,
    const turbo_media_auth_config_t *config,
    const turbo_media_auth_policy_t *policy);

/**
 * Issue a strict HS256 TurboMedia access token with the active key.
 *
 * Returned memory is owned by the caller and must be released with free().
 */
char *turbo_media_auth_issue(const turbo_media_auth_config_t *config,
                             const turbo_media_auth_claims_t *claims);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_AUTH_H */
