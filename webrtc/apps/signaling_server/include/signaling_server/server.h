/**
 * @file server.h
 * @brief Signaling server wrapper
 */

#ifndef SIGNALING_SERVER_SERVER_H
#define SIGNALING_SERVER_SERVER_H

#include "signaling_server/config.h"
#include "webrtc_signaling.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Signaling server instance
 */
typedef struct signaling_server_s signaling_server_t;

/**
 * Create signaling server instance
 */
signaling_server_t *signaling_server_create(const signaling_server_config_t *config);

/**
 * Start signaling server
 */
int signaling_server_start(signaling_server_t *server);

/**
 * Run signaling server (blocks until stopped)
 */
int signaling_server_run(signaling_server_t *server);

/**
 * Stop signaling server
 */
void signaling_server_stop(signaling_server_t *server);

/**
 * Destroy signaling server instance
 */
void signaling_server_destroy(signaling_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* SIGNALING_SERVER_SERVER_H */
