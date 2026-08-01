/**
 * ice_integration.h - Complete ICE integration for WebRTC
 *
 * Implemented features:
 * - ICE candidate trickle
 * - STUN/TURN server support
 * - Connection timeout handling
 * - Retry scheduling (not a complete ICE restart)
 */

#ifndef ICE_INTEGRATION_H
#define ICE_INTEGRATION_H

#include "turbo_datachannel.h"
#include "ice/turbo_ice.h"
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ice_integration_ctx_s ice_integration_ctx_t;

/**
 * Create ICE integration context with STUN/TURN support
 *
 * @param peer DataChannel peer
 * @param loop Opaque backend loop pointer. Prefer passing a CoroNet
 *             `turbo_loop_t *`; pass NULL to let timer primitives bind lazily.
 * @param stun_servers Array of STUN server URLs (e.g., "stun:stun.l.google.com:19302")
 * @param stun_count Number of STUN servers
 * @param turn_servers Array of TURN server URLs (e.g., "turn:turn.example.com:3478")
 * @param turn_usernames Array of TURN usernames (can be NULL if no auth)
 * @param turn_credentials Array of TURN credentials (can be NULL if no auth)
 * @param turn_count Number of TURN servers
 * @return ICE integration context or NULL on error
 */
CXX_C_API ice_integration_ctx_t *ice_integration_create(
    turbo_dc_peer_t *peer,
    void *loop,
    const char **stun_servers,
    int stun_count,
    const char **turn_servers,
    const char **turn_usernames,
    const char **turn_credentials,
    int turn_count
);

/**
 * Destroy ICE integration context
 */
CXX_C_API void ice_integration_destroy(ice_integration_ctx_t *ctx);

/**
 * Set callback for ICE candidate trickle
 * Called when a new local candidate is discovered
 *
 * @param ctx ICE integration context
 * @param callback Callback function receiving SDP-format candidate string
 * @param user_data User data passed to callback
 */
CXX_C_API void ice_integration_on_candidate(
    ice_integration_ctx_t *ctx,
    void (*callback)(const char *candidate_sdp, void *user_data),
    void *user_data
);

/**
 * Set callback for ICE state changes
 *
 * @param ctx ICE integration context
 * @param callback Callback function receiving ICE state
 * @param user_data User data passed to callback
 */
CXX_C_API void ice_integration_on_state_change(
    ice_integration_ctx_t *ctx,
    void (*callback)(ice_state_t state, void *user_data),
    void *user_data
);

/**
 * Get local ICE credentials for SDP
 *
 * @param ctx ICE integration context
 * @param ufrag Buffer for username fragment (min 16 bytes)
 * @param ufrag_len Size of ufrag buffer
 * @param pwd Buffer for password (min 32 bytes)
 * @param pwd_len Size of pwd buffer
 * @return 0 on success, negative on error
 */
CXX_C_API int ice_integration_get_local_credentials(
    ice_integration_ctx_t *ctx,
    char *ufrag,
    size_t ufrag_len,
    char *pwd,
    size_t pwd_len
);

/**
 * Set remote ICE credentials from SDP
 *
 * @param ctx ICE integration context
 * @param ufrag Remote username fragment
 * @param pwd Remote password
 * @return 0 on success, negative on error
 */
CXX_C_API int ice_integration_set_remote_credentials(
    ice_integration_ctx_t *ctx,
    const char *ufrag,
    const char *pwd
);

/**
 * Start ICE gathering
 * Begins discovering local candidates
 *
 * @param ctx ICE integration context
 * @return 0 on success, negative on error
 */
CXX_C_API int ice_integration_start_gathering(ice_integration_ctx_t *ctx);

/**
 * Add remote ICE candidate (trickle ICE)
 * Called when receiving a candidate from remote peer via signaling
 *
 * @param ctx ICE integration context
 * @param candidate_sdp SDP-format candidate string
 * @return 0 on success, negative on error
 */
CXX_C_API int ice_integration_add_remote_candidate(
    ice_integration_ctx_t *ctx,
    const char *candidate_sdp
);

/**
 * Signal end of remote candidates
 * Called when remote peer has finished sending candidates
 *
 * @param ctx ICE integration context
 */
CXX_C_API void ice_integration_end_of_candidates(ice_integration_ctx_t *ctx);

/**
 * Drive the internal ICE coroutine context once without blocking.
 *
 * Call this regularly when the integration wraps an external `turbo_loop_t *`
 * that you are polling manually, such as from tests or simple examples.
 *
 * @param ctx ICE integration context
 */
CXX_C_API void ice_integration_poll(ice_integration_ctx_t *ctx);

/**
 * Query whether local ICE gathering has completed.
 *
 * @param ctx ICE integration context
 * @return 1 if gathering is complete, 0 otherwise
 */
CXX_C_API int ice_integration_is_gathering_complete(ice_integration_ctx_t *ctx);

/**
 * Set connection timeout (milliseconds)
 * Default: 30000 ms (30 seconds)
 *
 * @param ctx ICE integration context
 * @param timeout_ms Timeout in milliseconds
 */
CXX_C_API void ice_integration_set_connection_timeout(ice_integration_ctx_t *ctx, int timeout_ms);

/**
 * Set max reconnection attempts
 * Default: 5 attempts
 *
 * @param ctx ICE integration context
 * @param max_attempts Maximum number of reconnection attempts
 */
CXX_C_API void ice_integration_set_max_reconnect_attempts(ice_integration_ctx_t *ctx, int max_attempts);

/**
 * Schedule another retry attempt.
 *
 * This function does not create a new ICE generation, rotate credentials,
 * gather new candidates, or exchange them through signaling. It is not a
 * complete ICE restart and must not be used as proof of connection recovery.
 *
 * @param ctx ICE integration context
 * @return 0 on success, negative on error
 */
CXX_C_API int ice_integration_reconnect(ice_integration_ctx_t *ctx);

/**
 * Enable/disable loopback candidate gathering
 *
 * @param ctx ICE integration context
 * @param allow 1 to allow, 0 to disable
 */
CXX_C_API void ice_integration_set_allow_loopback(ice_integration_ctx_t *ctx, int allow);

#ifdef __cplusplus
}
#endif

#endif /* ICE_INTEGRATION_H */
