#ifndef TURBO_MEDIA_WEBRTC_H
#define TURBO_MEDIA_WEBRTC_H

#include <turbo_export.h>
#include <turbo_media_server.h>
#include <turbo_media_source.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Normalize RTC/WHIP/WHEP resource identity into the shared TurboMedia source
 * key. Query parameters app= and stream= take precedence over path segments.
 */
CXX_C_API int turbo_media_webrtc_source_key(
    const char *default_vhost,
    const char *resource_path,
    const char *query,
    turbo_media_source_key_t *key);

typedef struct turbo_media_webrtc_session_s turbo_media_webrtc_session_t;

typedef enum {
    TURBO_MEDIA_WEBRTC_ROLE_PUBLISHER = 1,
    TURBO_MEDIA_WEBRTC_ROLE_PLAYER = 2
} turbo_media_webrtc_role_t;

typedef enum {
    TURBO_MEDIA_WEBRTC_PEER_NEW = 0,
    TURBO_MEDIA_WEBRTC_PEER_CONNECTING = 1,
    TURBO_MEDIA_WEBRTC_PEER_CONNECTED = 2,
    TURBO_MEDIA_WEBRTC_PEER_DISCONNECTED = 3,
    TURBO_MEDIA_WEBRTC_PEER_FAILED = 4,
    TURBO_MEDIA_WEBRTC_PEER_CLOSED = 5
} turbo_media_webrtc_peer_state_t;

typedef void (*turbo_media_webrtc_ice_candidate_cb)(
    turbo_media_webrtc_session_t *session,
    const char *candidate,
    void *user_data);

typedef void (*turbo_media_webrtc_state_cb)(
    turbo_media_webrtc_session_t *session,
    turbo_media_webrtc_peer_state_t state,
    void *user_data);

typedef struct turbo_media_webrtc_session_config_s {
    turbo_media_server_runtime_t *runtime;
    turbo_media_webrtc_role_t role;
    const char *default_vhost;
    const char *resource_path;
    const char *query;
    const char *remote_offer_sdp;
    const char *const *stun_servers;
    size_t stun_server_count;
    const char *const *turn_servers;
    size_t turn_server_count;
    int allow_loopback;
    size_t event_queue_capacity;
    size_t event_queue_max_bytes;
    size_t max_rtp_packet_bytes;
    int replay_cached;
    int remove_source_on_close;
    turbo_media_webrtc_ice_candidate_cb on_ice_candidate;
    turbo_media_webrtc_state_cb on_state;
    void *user_data;
} turbo_media_webrtc_session_config_t;

/**
 * Create a WHIP/WHEP session with the built-in TurboMedia PeerConnection backend.
 *
 * @param config Session identity, role, ICE servers, callbacks, and queue limits.
 *        Queue limit fields use production defaults when zero.
 * @param answer_sdp Receives the NUL-terminated SDP answer.
 * @param answer_sdp_capacity Capacity of answer_sdp, including the NUL byte.
 * @param answer_sdp_length Optional answer length excluding the NUL byte.
 * @param session Receives the owned session on success.
 * @return TURBO_MEDIA_OK on success; TURBO_MEDIA_ERR_INVALID for invalid SDP,
 *         configuration, or codec negotiation; TURBO_MEDIA_ERR_NOMEM on
 *         allocation failure; otherwise a TurboMedia state/resource error.
 *
 * The creating thread owns the session and must call
 * turbo_media_webrtc_session_pump() regularly. Destroy the result with
 * turbo_media_webrtc_session_destroy().
 */
CXX_C_API int turbo_media_webrtc_session_create(
    const turbo_media_webrtc_session_config_t *config,
    char *answer_sdp,
    size_t answer_sdp_capacity,
    size_t *answer_sdp_length,
    turbo_media_webrtc_session_t **session);

CXX_C_API void turbo_media_webrtc_session_destroy(
    turbo_media_webrtc_session_t *session);

CXX_C_API int turbo_media_webrtc_session_add_ice_candidate(
    turbo_media_webrtc_session_t *session,
    const char *candidate);

CXX_C_API int turbo_media_webrtc_session_pump(
    turbo_media_webrtc_session_t *session);

CXX_C_API turbo_media_webrtc_peer_state_t turbo_media_webrtc_session_state(
    const turbo_media_webrtc_session_t *session);

CXX_C_API int turbo_media_webrtc_session_last_error(
    const turbo_media_webrtc_session_t *session);

CXX_C_API const turbo_media_source_key_t *turbo_media_webrtc_session_key(
    const turbo_media_webrtc_session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_WEBRTC_H */
