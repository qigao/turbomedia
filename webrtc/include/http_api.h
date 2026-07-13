/**
 * @file http_api.h
 * @brief HTTP Management API for Signaling Server
 */

#ifndef WEBRTC_HTTP_API_H
#define WEBRTC_HTTP_API_H

#include "webrtc_signaling.h"

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
    const char *host;
    int port;
    int auth_enabled;
    const char *admin_token; /* If simple token auth */
} http_api_config_t;

/**
 * @brief Create HTTP API server
 * 
 * @param loop Event loop
 * @param config Configuration
 * @param signaling Signaling server instance (for data access)
 * @return Handle or NULL on failure
 */
http_api_server_t *http_api_create(void *loop, const http_api_config_t *config, webrtc_signaling_server_t *signaling);

/**
 * @brief Start HTTP API server
 */
int http_api_start(http_api_server_t *server);

/**
 * @brief Stop HTTP API server
 */
void http_api_stop(http_api_server_t *server);

/**
 * @brief Destroy HTTP API server
 */
void http_api_destroy(http_api_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* WEBRTC_HTTP_API_H */
