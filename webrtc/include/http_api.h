/**
 * @file http_api.h
 * @brief HTTP Management API for Signaling Server
 */

#ifndef WEBRTC_HTTP_API_H
#define WEBRTC_HTTP_API_H

#include "webrtc_signaling.h"
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTP API Server Handle
 */
typedef struct http_api_server_s http_api_server_t;

/**
 * @brief HTTP API Configuration
 */
typedef struct {
    const char *host; /* Required and copied local address */
    int port;
    int auth_enabled; /* Protect management routes with HTTP Bearer authentication */
    const char *admin_token; /* Optional copied static compatibility token */
    const char *auth_issuer;
    const char *auth_active_key_id;
    const char *auth_active_secret;
    const char *auth_previous_key_id;
    const char *auth_previous_secret;
    const char *auth_revoked_token_sha256;
    int auth_clock_skew_seconds;
    int auth_max_ttl_seconds;
    int use_tls; /* 1 for HTTPS, 0 for HTTP */
    const char *cert_file; /* Required and copied while use_tls is non-zero */
    const char *key_file; /* Required and copied while use_tls is non-zero */
} http_api_config_t;

/**
 * @brief Create HTTP API server
 * 
 * @param loop Reserved for a future caller-owned event loop; currently ignored
 * @param config Configuration
 * @param signaling Borrowed signaling server instance. It must outlive the
 *                  returned HTTP API server.
 * @return Handle or NULL on failure
 */
CXX_C_API http_api_server_t *http_api_create(
    void *loop,
    const http_api_config_t *config,
    webrtc_signaling_server_t *signaling);

/**
 * @brief Start HTTP API server
 */
CXX_C_API int http_api_start(http_api_server_t *server);

/**
 * @brief Stop HTTP API server
 */
CXX_C_API void http_api_stop(http_api_server_t *server);

/**
 * @brief Destroy HTTP API server
 */
CXX_C_API void http_api_destroy(http_api_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* WEBRTC_HTTP_API_H */
