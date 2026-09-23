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
    size_t connection_capacity;     /**< Required hard limit for live WebSocket connections */
    int use_tls;                    /**< 1 for WSS, 0 for WS */
    const char *cert_file;          /**< Path to SSL certificate */
    const char *key_file;           /**< Path to SSL private key */
    int max_peers;                  /**< Max peers per room (0 = unlimited) */
    int max_rooms;                  /**< Max active rooms (0 = unlimited) */
    int peer_timeout_ms;            /**< Peer idle timeout */
    int join_timeout_ms;            /**< Fixed deadline for the first successful join */
    size_t max_message_size;        /**< Complete WebSocket message limit (0 = bounded 64 KiB default) */
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
    size_t jwt_dynamic_revocation_capacity; /**< 0 disables dynamic revocation; otherwise bounded entries */
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

typedef enum {
    WEBRTC_SIGNALING_REVOCATION_APPLY_ERROR = -1,
    WEBRTC_SIGNALING_REVOCATION_APPLY_APPLIED = 0,
    WEBRTC_SIGNALING_REVOCATION_APPLY_STALE = 1,
    WEBRTC_SIGNALING_REVOCATION_APPLY_GAP = 2,
    WEBRTC_SIGNALING_REVOCATION_APPLY_LIMIT = 3
} webrtc_signaling_revocation_apply_result_t;

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
 * @param reserved Reserved for ABI growth; must be NULL.
 */
TURBO_MEDIA_API webrtc_signaling_server_t *webrtc_signaling_create(
    void *reserved,
    const webrtc_signaling_config_t *config);

/**
 * @brief Start signaling server
 */
TURBO_MEDIA_API int webrtc_signaling_start(webrtc_signaling_server_t *server);

/**
 * @brief Stop signaling server and drain its CHTTP/CNet owner thread
 *
 * Stops accepting new connections, closes active peers, joins the owner and
 * cleanup threads, and releases the stopped CHTTP instance. This function is
 * thread-safe and permits a later restart.
 */
TURBO_MEDIA_API void webrtc_signaling_stop(webrtc_signaling_server_t *server);

/**
 * @brief Destroy a stopped signaling server and all owned state
 */
TURBO_MEDIA_API void webrtc_signaling_destroy(webrtc_signaling_server_t *server);

/**
 * @brief Get active peer count
 */
TURBO_MEDIA_API int webrtc_signaling_get_peer_count(webrtc_signaling_server_t *server);

/**
 * @brief Get the bound listener port after start
 * @return 0 on success, -1 when stopped or invalid
 */
TURBO_MEDIA_API int webrtc_signaling_get_port(
    webrtc_signaling_server_t *server, uint16_t *out_port);

/**
 * Apply one complete dynamic revocation snapshot.
 *
 * Dynamic revocation must be enabled by jwt_dynamic_revocation_capacity.
 * A newly created server starts unsynchronized and signed peer admission
 * fails closed until a covering snapshot is applied.
 */
TURBO_MEDIA_API webrtc_signaling_revocation_apply_result_t
webrtc_signaling_apply_revocation_snapshot(
    webrtc_signaling_server_t *server,
    uint64_t epoch,
    uint64_t sequence,
    const char *const *sha256_hex,
    size_t count);

/**
 * Apply one exact-next dynamic revoke event.
 */
TURBO_MEDIA_API webrtc_signaling_revocation_apply_result_t
webrtc_signaling_apply_revocation(
    webrtc_signaling_server_t *server,
    uint64_t epoch,
    uint64_t sequence,
    const char *sha256_hex);

/**
 * Read the bounded dynamic revocation projection status.
 */
TURBO_MEDIA_API int webrtc_signaling_get_revocation_status(
    webrtc_signaling_server_t *server,
    int *out_synchronized,
    uint64_t *out_epoch,
    uint64_t *out_sequence,
    size_t *out_count);

/**
 * @brief Broadcast message to all peers in room
 */
TURBO_MEDIA_API int webrtc_signaling_broadcast(
    webrtc_signaling_server_t *server,
    const char *room,
    const char *from,
    const char *message);

/**
 * @brief Get active room count
 */
TURBO_MEDIA_API int webrtc_signaling_get_room_count(webrtc_signaling_server_t *server);

/**
 * @brief Get JSON representation of all rooms
 * Caller must free the returned string
 */
TURBO_MEDIA_API char *webrtc_signaling_get_rooms_json(webrtc_signaling_server_t *server);

/**
 * @brief Get JSON representation of peers in a room
 * Caller must free the returned string
 */
TURBO_MEDIA_API char *webrtc_signaling_get_room_peers_json(webrtc_signaling_server_t *server, const char *room_id);

/**
 * @brief Kick a peer from a room
 */
TURBO_MEDIA_API int webrtc_signaling_kick_peer(
    webrtc_signaling_server_t *server, 
    const char *room_id, 
    const char *peer_id, 
    const char *reason);

/**
 * @brief Get server status/metrics as JSON
 * Caller must free the returned string
 */
TURBO_MEDIA_API char *webrtc_signaling_get_status_json(webrtc_signaling_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* WEBRTC_SIGNALING_H */
