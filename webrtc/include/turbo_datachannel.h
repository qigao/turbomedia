/**
 * turbo_datachannel.h - WebRTC Data Channel over CNet
 *
 * Architecture:
 *   Application
 *       ↓
 *   DataChannel (this API)
 *       ↓
 *   SCTP (usrsctp) - reliable/unreliable messaging
 *       ↓
 *   DTLS (BoringSSL) - encryption
 *       ↓
 *   Transport: CNet (UDP/TCP/KCP) or SaltsNet ICE (NAT traversal)
 *
 * Transport modes:
 * - UDP (default): Direct UDP via CNet
 * - TCP: Direct TCP via CNet
 * - KCP: Reliable UDP via CNet KCP
 * - ICE: ICE agent for NAT traversal (requires signaling)
 */

#ifndef TURBO_DATACHANNEL_H
#define TURBO_DATACHANNEL_H

#include <turbo_export.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration for ICE agent */
struct salts_ice_agent_s;

/* ============================================================================
 * Type Definitions
 * ============================================================================ */

typedef struct turbo_dc_context_s turbo_dc_context_t;
typedef struct turbo_dc_peer_s turbo_dc_peer_t;
typedef struct turbo_dc_channel_s turbo_dc_channel_t;

typedef void (*turbo_dc_transport_send_cb)(void *transport,
                                           const void *data,
                                           size_t len);
typedef void (*turbo_dc_transport_data_cb)(void *user_data,
                                           const uint8_t *data,
                                           size_t len);

/* Transport type */
typedef enum {
    TURBO_DC_TRANSPORT_UDP = 0,   /* Direct UDP via CNet (default) */
    TURBO_DC_TRANSPORT_TCP,       /* Direct TCP via CNet */
    TURBO_DC_TRANSPORT_KCP,       /* Reliable UDP via CNet KCP */
    TURBO_DC_TRANSPORT_ICE        /* ICE for NAT traversal */
} turbo_dc_transport_t;

/* Connection state */
typedef enum {
    TURBO_DC_STATE_NEW = 0,
    TURBO_DC_STATE_CONNECTING,
    TURBO_DC_STATE_CONNECTED,
    TURBO_DC_STATE_DISCONNECTING,
    TURBO_DC_STATE_CLOSED,
    TURBO_DC_STATE_FAILED
} turbo_dc_state_t;

/* Channel configuration */
typedef struct {
    int ordered;              /* TRUE for ordered delivery */
    int max_retransmits;      /* Max retransmits (0 = unlimited) */
    int max_lifetime_ms;      /* Max packet lifetime in ms (0 = unlimited) */
    const char *protocol;     /* Optional sub-protocol */
} turbo_dc_channel_config_t;

/* Context configuration */
typedef struct {
    const char *cert_pem;     /* Optional: PEM certificate for DTLS */
    const char *key_pem;      /* Optional: PEM private key for DTLS */
    int is_server;            /* TRUE if this is the server/answerer side */
    turbo_dc_transport_t transport;  /* Transport type (default = UDP) */
    uint16_t sctp_mtu;        /* SCTP path MTU (0 = default 1188) */
    uint16_t dtls_mtu;        /* DTLS MTU (0 = default 1280) */
    int disable_sctp;         /* TRUE for DTLS-only transport without SCTP/DataChannel */
} turbo_dc_config_t;

/* ============================================================================
 * Callbacks
 * ============================================================================ */

/* Called when connection state changes */
typedef void (*turbo_dc_state_cb)(
    turbo_dc_peer_t *peer,
    turbo_dc_state_t old_state,
    turbo_dc_state_t new_state,
    void *user_data
);

/* Called when a new data channel is created by remote peer */
typedef void (*turbo_dc_channel_cb)(
    turbo_dc_peer_t *peer,
    turbo_dc_channel_t *channel,
    void *user_data
);

/* Called when data channel opens */
typedef void (*turbo_dc_open_cb)(
    turbo_dc_channel_t *channel,
    void *user_data
);

/* Called when message received on data channel */
typedef void (*turbo_dc_message_cb)(
    turbo_dc_channel_t *channel,
    const void *data,
    size_t len,
    int is_binary,
    void *user_data
);

/* Called when data channel closes */
typedef void (*turbo_dc_close_cb)(
    turbo_dc_channel_t *channel,
    void *user_data
);

/* Called on error */
typedef void (*turbo_dc_error_cb)(
    turbo_dc_peer_t *peer,
    int error_code,
    const char *error_msg,
    void *user_data
);

/* ============================================================================
 * Context API - Global initialization
 * ============================================================================ */

/**
 * Create a data channel context
 * Must be called before creating any peers
 */
TURBO_MEDIA_API turbo_dc_context_t *turbo_dc_context_create(const turbo_dc_config_t *config);

/**
 * Destroy context and all associated peers
 */
TURBO_MEDIA_API void turbo_dc_context_destroy(turbo_dc_context_t *ctx);

/**
 * Force global cleanup (optional)
 * Normally cleanup happens automatically when the last context is destroyed.
 * Call this only if you need to forcefully cleanup SCTP resources.
 */
TURBO_MEDIA_API void turbo_dc_global_cleanup(void);
 
/**
 * Get the local DTLS certificate fingerprint
 * This must be sent to the remote peer via SDP (a=fingerprint)
 *
 * @param ctx Context handle
 * @param hash Buffer for hash name (e.g., "sha-256"), min 16 bytes
 * @param hash_len Size of hash buffer
 * @param fingerprint Buffer for hex fingerprint, min 128 bytes
 * @param fp_len Size of fingerprint buffer
 * @return 0 on success, negative on error
 */
TURBO_MEDIA_API int turbo_dc_context_get_local_fingerprint(
    turbo_dc_context_t *ctx,
    char *hash,
    size_t hash_len,
    char *fingerprint,
    size_t fp_len
);

/* ============================================================================
 * Peer Connection API
 * ============================================================================ */

/**
 * Create a peer connection
 *
 * @param ctx Context from turbo_dc_context_create
 * @param remote_host Remote host to connect to (NULL for server mode)
 * @param remote_port Remote port (0 for server mode - will listen)
 * @param user_data User data passed to callbacks
 * @return Peer handle or NULL on error
 */
TURBO_MEDIA_API turbo_dc_peer_t *turbo_dc_peer_create(
    turbo_dc_context_t *ctx,
    const char *remote_host,
    uint16_t remote_port,
    void *user_data
);

/**
 * Set callbacks
 */
TURBO_MEDIA_API void turbo_dc_peer_on_state(turbo_dc_peer_t *peer, turbo_dc_state_cb cb);
TURBO_MEDIA_API void turbo_dc_peer_on_channel(turbo_dc_peer_t *peer, turbo_dc_channel_cb cb);
TURBO_MEDIA_API void turbo_dc_peer_on_error(turbo_dc_peer_t *peer, turbo_dc_error_cb cb);
 
/**
 * Set the expected remote certificate fingerprint
 * Received via SDP (a=fingerprint) from the remote peer.
 *
 * @param peer Peer handle
 * @param hash Hash name (e.g., "sha-256")
 * @param fingerprint Hex fingerprint string
 *
 * Only SHA-256 fingerprints in colon-separated hexadecimal form are accepted.
 *
 * @return 0 on success, -1 for invalid arguments, -2 for an unsupported hash,
 *         -3 for a malformed fingerprint, or -4 on allocation failure
 */
TURBO_MEDIA_API int turbo_dc_peer_set_remote_fingerprint(
    turbo_dc_peer_t *peer,
    const char *hash,
    const char *fingerprint
);

/**
 * Override the DTLS role before the handshake starts.
 *
 * @param peer      Peer handle
 * @param is_server 1 for DTLS server/passive, 0 for DTLS client/active
 * @return 0 on success, negative on error
 */
TURBO_MEDIA_API int turbo_dc_peer_set_dtls_role(
    turbo_dc_peer_t *peer,
    int is_server
);

/** Return 1 when the peer uses the DTLS server role, otherwise 0. */
TURBO_MEDIA_API int turbo_dc_peer_is_dtls_server(const turbo_dc_peer_t *peer);

/**
 * Attach an externally owned datagram transport to the peer.
 *
 * The caller retains ownership and must detach the transport before destroying
 * it. Incoming DTLS packets are supplied through
 * turbo_dc_peer_feed_transport_data().
 *
 * @param peer Peer receiving the external transport.
 * @param transport Borrowed transport context, or NULL to detach.
 * @param send_cb Datagram sender required when transport is non-NULL.
 * @return 0 on success, -1 for invalid arguments, or -2 when another transport
 *         is already attached.
 */
TURBO_MEDIA_API int turbo_dc_peer_set_external_transport(
    turbo_dc_peer_t *peer,
    void *transport,
    turbo_dc_transport_send_cb send_cb);

TURBO_MEDIA_API void turbo_dc_peer_feed_transport_data(
    turbo_dc_peer_t *peer,
    const void *data,
    size_t len);

TURBO_MEDIA_API void turbo_dc_peer_set_transport_data_handler(
    turbo_dc_peer_t *peer,
    turbo_dc_transport_data_cb callback,
    void *user_data);

/**
 * Set ICE agent for this peer (ICE transport mode only)
 *
 * The agent may be attached before candidate gathering and connectivity checks,
 * but turbo_dc_peer_connect() must not be called until ICE reaches CONNECTED or
 * COMPLETED. The caller retains ownership and must keep the agent alive until
 * the peer is destroyed or its transport is detached.
 *
 * @param peer Peer connection.
 * @param ice_agent Borrowed SaltsNet ICE agent that is not closed.
 * @return 0 on success, -1 for invalid/closed input, or -2 when a different
 *         transport is already attached.
 *
 */
TURBO_MEDIA_API int turbo_dc_peer_set_ice_agent(turbo_dc_peer_t *peer, struct salts_ice_agent_s *ice_agent);

/**
 * Feed data from ICE agent to DataChannel
 *
 * Call this from ICE agent's on_data callback to process incoming data.
 * This handles DTLS decryption and SCTP demuxing internally.
 *
 * @param peer Peer connection
 * @param data Data received from ICE agent
 * @param len Data length
 */
TURBO_MEDIA_API void turbo_dc_peer_feed_ice_data(turbo_dc_peer_t *peer, const void *data, size_t len);

/**
 * Start connection (client) or listening (server)
 */
TURBO_MEDIA_API int turbo_dc_peer_connect(turbo_dc_peer_t *peer);

/**
 * Poll peer-internal state after transport/timer progress.
 */
TURBO_MEDIA_API void turbo_dc_peer_poll(turbo_dc_peer_t *peer);

/**
 * Get current state
 */
TURBO_MEDIA_API turbo_dc_state_t turbo_dc_peer_get_state(turbo_dc_peer_t *peer);

/**
 * Close peer connection
 */
TURBO_MEDIA_API void turbo_dc_peer_close(turbo_dc_peer_t *peer);

/**
 * Destroy peer (must be closed first or will force close)
 */
TURBO_MEDIA_API void turbo_dc_peer_destroy(turbo_dc_peer_t *peer);

/**
 * Get negotiated SRTP keys from DTLS session
 *
 * @param peer      Peer handle
 * @param material  Output buffer for keys
 * @return          Negotated SRTP profile ID, or 0 if not ready/error
 */
TURBO_MEDIA_API uint16_t turbo_dc_peer_get_srtp_keys(turbo_dc_peer_t *peer, void *material);

/**
 * Send raw transport data (for RTP/RTCP media packets)
 *
 * Sends data directly through the transport layer without SCTP encapsulation.
 * Used by turbo_media for sending encrypted RTP/RTCP packets.
 *
 * @param peer Peer handle
 * @param data Data to send
 * @param len  Data length
 */
TURBO_MEDIA_API void turbo_dc_peer_send_transport_data(turbo_dc_peer_t *peer, const void *data, size_t len);

/* ============================================================================
 * Data Channel API
 * ============================================================================ */

/**
 * Create a data channel
 *
 * @param peer Peer connection
 * @param label Channel label/name
 * @param config Channel configuration (NULL for defaults)
 * @return Channel handle or NULL on error
 */
TURBO_MEDIA_API turbo_dc_channel_t *turbo_dc_channel_create(
    turbo_dc_peer_t *peer,
    const char *label,
    const turbo_dc_channel_config_t *config
);

/**
 * Open the data channel (send DCEP OPEN message)
 * Call this after peer is connected to negotiate the channel with remote peer.
 *
 * @param channel Channel handle
 * @return 0 on success, negative on error
 */
TURBO_MEDIA_API int turbo_dc_channel_open(turbo_dc_channel_t *channel);

/**
 * Set channel callbacks
 */
TURBO_MEDIA_API void turbo_dc_channel_on_open(turbo_dc_channel_t *channel, turbo_dc_open_cb cb);
TURBO_MEDIA_API void turbo_dc_channel_on_message(turbo_dc_channel_t *channel, turbo_dc_message_cb cb);
TURBO_MEDIA_API void turbo_dc_channel_on_close(turbo_dc_channel_t *channel, turbo_dc_close_cb cb);

/**
 * Set/get user data for channel callbacks
 */
TURBO_MEDIA_API void turbo_dc_channel_set_user_data(turbo_dc_channel_t *channel, void *user_data);
TURBO_MEDIA_API void *turbo_dc_channel_get_user_data(turbo_dc_channel_t *channel);

/**
 * Send data on channel
 *
 * @param channel Channel handle
 * @param data Data to send
 * @param len Data length
 * @param is_binary TRUE for binary, FALSE for text
 * @return 0 on success, negative on error
 */
TURBO_MEDIA_API int turbo_dc_channel_send(
    turbo_dc_channel_t *channel,
    const void *data,
    size_t len,
    int is_binary
);

/**
 * Get channel label
 */
TURBO_MEDIA_API const char *turbo_dc_channel_get_label(turbo_dc_channel_t *channel);

/**
 * Get channel ID
 */
TURBO_MEDIA_API uint16_t turbo_dc_channel_get_id(turbo_dc_channel_t *channel);

/**
 * Check if channel is open
 */
TURBO_MEDIA_API int turbo_dc_channel_is_open(turbo_dc_channel_t *channel);

/**
 * Get buffered amount (bytes waiting to send)
 */
TURBO_MEDIA_API size_t turbo_dc_channel_buffered_amount(turbo_dc_channel_t *channel);

/**
 * Close data channel
 */
TURBO_MEDIA_API void turbo_dc_channel_close(turbo_dc_channel_t *channel);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/**
 * Get default channel config
 */
TURBO_MEDIA_API turbo_dc_channel_config_t turbo_dc_default_channel_config(void);

/**
 * Handle SCTP timers
 *
 * IMPORTANT: When using usrsctp in "nothreads" mode, you must call this
 * function periodically (e.g., every 10ms) in your event loop to allow
 * SCTP to process internal timers and pending events.
 *
 * This is required for proper message delivery and retransmissions.
 */
TURBO_MEDIA_API void turbo_dc_handle_timers(void);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_DATACHANNEL_H */
