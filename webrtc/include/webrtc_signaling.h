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

#include <stddef.h>
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
    int join_timeout_ms;            /**< Fixed deadline for the first successful join */
    size_t max_message_size;        /**< Complete WebSocket message limit (0 = unlimited) */
    int messages_per_second;        /**< Per-peer token refill rate (0 = disabled) */
    int message_burst;              /**< Per-peer token bucket capacity */
    size_t max_outbox_messages;     /**< Per-peer queued message limit (0 = unlimited) */
    size_t max_outbox_bytes;        /**< Per-peer queued byte limit (0 = unlimited) */
    int max_connections_per_source; /**< Concurrent upgraded connections per IP (0 = unlimited) */
    int source_admissions_per_second; /**< Upgraded connection admission refill rate */
    int source_admission_burst;     /**< Upgraded connection admission burst capacity */
    size_t max_source_states;       /**< Tracked source IP limit (0 only when policy is disabled) */
    int source_state_ttl_ms;        /**< Retain inactive source rate state for this duration */
    
    /* Authentication */
    int jwt_enabled;                /**< Require a signed token in the join message */
    const char *jwt_issuer;         /**< Exact token issuer */
    const char *jwt_active_key_id;  /**< Active HS256 key identifier */
    const char *jwt_secret;         /**< Active HS256 secret (at least 32 bytes) */
    const char *jwt_previous_key_id; /**< Previous key identifier during rotation */
    const char *jwt_previous_secret; /**< Previous secret during rotation */
    const char *jwt_revoked_token_sha256; /**< Comma-separated revoked token digests */
    int jwt_clock_skew_seconds;     /**< Accepted clock skew (0..300) */
    int jwt_max_ttl_seconds;        /**< Maximum token lifetime (1..86400) */
    const char *jwt_algo;           /**< Must be HS256 when specified */
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
 * @brief Start asynchronous signaling server shutdown
 *
 * Stops accepting new connections and wakes active peers. The listener and
 * accepted connection tasks remain owned by CoroNet until destroy drains them.
 * This function is thread-safe; listener shutdown is posted to the owning
 * coroutine context.
 */
CXX_C_API void webrtc_signaling_stop(webrtc_signaling_server_t *server);

/**
 * @brief Drive the server's coroutine context for one iteration.
 *
 * Higher-level wrappers that embed the signaling server in their own main loop
 * should use this instead of polling the raw backend loop directly. Call from
 * the coroutine context owner thread only.
 */
CXX_C_API int webrtc_signaling_run(webrtc_signaling_server_t *server, turbo_run_mode_t mode);

/**
 * @brief Destroy signaling server after draining all CoroNet connection tasks
 *
 * Call from the coroutine context owner thread after concurrent run calls have
 * returned.
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
