/**
 * @file webrtc_signaling.h
 * @brief WebRTC Signaling Server
 *
 * Simple WebSocket-based signaling server for WebRTC connections.
 * Handles SDP offer/answer exchange and ICE candidate trickle.
 *
 * Message Protocol (JSON):
 * {
 *   "type": "offer|answer|candidate|join|leave",
 *   "from": "peer_id",
 *   "to": "peer_id",
 *   "data": { ... }
 * }
 */

#ifndef WEBRTC_SIGNALING_H
#define WEBRTC_SIGNALING_H

#include <stdint.h>
#include <CoroNet/turbo_coro_context.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Types
 * ============================================================================= */

typedef struct webrtc_signaling_server_s webrtc_signaling_server_t;
typedef struct webrtc_peer_s webrtc_peer_t;

/**
 * @brief Signaling server configuration
 */
typedef struct {
    const char *host;               /**< Bind host (e.g., "0.0.0.0") */
    uint16_t port;                  /**< Bind port (e.g., 8080) */
    int use_tls;                    /**< 1 for WSS, 0 for WS */
    const char *cert_file;          /**< Path to SSL certificate */
    const char *key_file;           /**< Path to SSL private key */
    int max_peers;                  /**< Max peers per room (0 = unlimited) */
    int max_rooms;                  /**< Max active rooms (0 = unlimited) */
    int peer_timeout_ms;            /**< Peer idle timeout */
    
    /* Authentication */
    int jwt_enabled;                /**< Enable JWT authentication */
    const char *jwt_secret;         /**< JWT secret key */
    const char *jwt_algo;           /**< JWT algorithm (default: HS256) */
} webrtc_signaling_config_t;

/**
 * @brief Message type
 */
typedef enum {
    SIGNAL_MSG_JOIN,                /**< Peer joins room */
    SIGNAL_MSG_LEAVE,               /**< Peer leaves room */
    SIGNAL_MSG_OFFER,               /**< SDP offer */
    SIGNAL_MSG_ANSWER,              /**< SDP answer */
    SIGNAL_MSG_CANDIDATE,           /**< ICE candidate */
    SIGNAL_MSG_ERROR                /**< Error message */
} webrtc_signal_msg_type_t;

/**
 * @brief Signaling message
 */
typedef struct {
    webrtc_signal_msg_type_t type;
    char *from;                     /**< Sender peer ID */
    char *to;                       /**< Recipient peer ID (NULL = broadcast) */
    char *room;                     /**< Room ID */
    char *data;                     /**< JSON data payload */
} webrtc_signal_msg_t;

/* =============================================================================
 * Server API
 * ============================================================================= */

/**
 * @brief Create WebRTC signaling server
 *
 * @param loop Opaque backend loop pointer. Prefer passing a CoroNet
 *             `turbo_loop_t *`; pass NULL to let the implementation allocate
 *             or bind lazily as supported by the active backend.
 */
CXX_C_API webrtc_signaling_server_t *webrtc_signaling_create(
    void *loop,
    const webrtc_signaling_config_t *config);

/**
 * @brief Start signaling server
 */
CXX_C_API int webrtc_signaling_start(webrtc_signaling_server_t *server);

/**
 * @brief Stop signaling server
 */
CXX_C_API void webrtc_signaling_stop(webrtc_signaling_server_t *server);

/**
 * @brief Drive the server's coroutine context for one iteration.
 *
 * Higher-level wrappers that embed the signaling server in their own main loop
 * should use this instead of polling the raw backend loop directly.
 */
CXX_C_API int webrtc_signaling_run(webrtc_signaling_server_t *server, turbo_run_mode_t mode);

/**
 * @brief Destroy signaling server
 */
CXX_C_API void webrtc_signaling_destroy(webrtc_signaling_server_t *server);

/**
 * @brief Get active peer count
 */
CXX_C_API int webrtc_signaling_get_peer_count(webrtc_signaling_server_t *server);

/**
 * @brief Broadcast message to all peers in room
 */
CXX_C_API int webrtc_signaling_broadcast(
    webrtc_signaling_server_t *server,
    const char *room,
    const char *from,
    const char *message);

/**
 * @brief Get active room count
 */
CXX_C_API int webrtc_signaling_get_room_count(webrtc_signaling_server_t *server);

/**
 * @brief Get JSON representation of all rooms
 * Caller must free the returned string
 */
CXX_C_API char *webrtc_signaling_get_rooms_json(webrtc_signaling_server_t *server);

/**
 * @brief Get JSON representation of peers in a room
 * Caller must free the returned string
 */
CXX_C_API char *webrtc_signaling_get_room_peers_json(webrtc_signaling_server_t *server, const char *room_id);

/**
 * @brief Kick a peer from a room
 */
CXX_C_API int webrtc_signaling_kick_peer(
    webrtc_signaling_server_t *server, 
    const char *room_id, 
    const char *peer_id, 
    const char *reason);

/**
 * @brief Get server status/metrics as JSON
 * Caller must free the returned string
 */
CXX_C_API char *webrtc_signaling_get_status_json(webrtc_signaling_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* WEBRTC_SIGNALING_H */
