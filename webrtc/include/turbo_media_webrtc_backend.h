#ifndef TURBO_MEDIA_WEBRTC_PEER_BACKEND_H
#define TURBO_MEDIA_WEBRTC_PEER_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "turbo_export.h"

typedef struct turbo_media_webrtc_session_s turbo_media_webrtc_session_t;
typedef struct turbo_media_webrtc_session_config_s turbo_media_webrtc_session_config_t;
typedef struct turbo_media_webrtc_backend_peer_s turbo_media_webrtc_backend_peer_t;
typedef struct turbo_media_webrtc_backend_track_s turbo_media_webrtc_backend_track_t;

typedef enum {
    TURBO_MEDIA_WEBRTC_BACKEND_TRACK_AUDIO = 1,
    TURBO_MEDIA_WEBRTC_BACKEND_TRACK_VIDEO = 2
} turbo_media_webrtc_backend_track_type_t;

typedef struct {
    int track_id;
    turbo_media_webrtc_backend_track_type_t type;
    char codec_name[32];
    int payload_type;
    int clock_rate;
    int width;
    int height;
    int framerate;
    int sample_rate;
    int channels;
} turbo_media_webrtc_backend_track_info_t;

typedef void (*turbo_media_webrtc_backend_state_cb)(void *user_data, int state);
typedef void (*turbo_media_webrtc_backend_ice_cb)(void *user_data, const char *candidate);
typedef int (*turbo_media_webrtc_backend_remote_track_cb)(
    void *user_data,
    turbo_media_webrtc_backend_track_t *track,
    const turbo_media_webrtc_backend_track_info_t *info);
typedef int (*turbo_media_webrtc_backend_rtp_cb)(
    void *user_data,
    turbo_media_webrtc_backend_track_t *track,
    const uint8_t *packet,
    size_t packet_len);

typedef struct {
    const char *const *stun_servers;
    size_t stun_server_count;
    const char *const *turn_servers;
    size_t turn_server_count;
    int allow_loopback;
    int accept_remote_tracks;
    size_t event_queue_capacity;
    size_t event_queue_max_bytes;
    size_t max_rtp_packet_bytes;
    turbo_media_webrtc_backend_state_cb on_state;
    turbo_media_webrtc_backend_ice_cb on_ice_candidate;
    turbo_media_webrtc_backend_remote_track_cb on_remote_track;
    turbo_media_webrtc_backend_rtp_cb on_rtp;
    void *user_data;
} turbo_media_webrtc_backend_config_t;

typedef struct {
    turbo_media_webrtc_backend_peer_t *(*create)(
        const turbo_media_webrtc_backend_config_t *config);
    void (*destroy)(turbo_media_webrtc_backend_peer_t *peer);
    int (*add_send_track)(turbo_media_webrtc_backend_peer_t *peer,
                          const turbo_media_webrtc_backend_track_info_t *info,
                          turbo_media_webrtc_backend_track_t **track);
    int (*set_remote_offer)(turbo_media_webrtc_backend_peer_t *peer,
                            const char *offer_sdp);
    int (*create_answer)(turbo_media_webrtc_backend_peer_t *peer,
                         char *answer_sdp,
                         size_t answer_sdp_capacity,
                         size_t *answer_sdp_length);
    int (*add_ice_candidate)(turbo_media_webrtc_backend_peer_t *peer,
                             const char *candidate);
    int (*start_track)(turbo_media_webrtc_backend_track_t *track);
    int (*send_rtp)(turbo_media_webrtc_backend_track_t *track,
                    const uint8_t *packet,
                    size_t packet_len);
    int (*pump)(turbo_media_webrtc_backend_peer_t *peer);
} turbo_media_webrtc_backend_ops_t;

/** Return the process-lifetime TurboMedia PeerConnection backend operations table. */
TURBO_MEDIA_C_API const turbo_media_webrtc_backend_ops_t *
turbo_media_webrtc_internal_backend(void);

TURBO_MEDIA_C_API int turbo_media_webrtc_session_create_with_backend(
    const turbo_media_webrtc_session_config_t *config,
    const turbo_media_webrtc_backend_ops_t *backend,
    char *answer_sdp,
    size_t answer_sdp_capacity,
    size_t *answer_sdp_length,
    turbo_media_webrtc_session_t **session);

#endif /* TURBO_MEDIA_WEBRTC_PEER_BACKEND_H */
