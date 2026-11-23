#ifndef TURBO_MEDIA_SERVER_H
#define TURBO_MEDIA_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>
#include <turbo_media_source.h>

#ifdef TURBO_MEDIA_HAS_RTSP
#include <turbo_rtsp.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_media_server_s turbo_media_server_t;
typedef struct turbo_media_server_runtime_s turbo_media_server_runtime_t;
typedef struct turbo_media_protocol_session_s turbo_media_protocol_session_t;

#ifdef TURBO_MEDIA_HAS_RTSP
typedef struct turbo_media_rtsp_server_adapter_s turbo_media_rtsp_server_adapter_t;
#endif

typedef enum {
    TURBO_MEDIA_PROTOCOL_UNKNOWN = 0,
    TURBO_MEDIA_PROTOCOL_RTMP = 1,
    TURBO_MEDIA_PROTOCOL_RTSP = 2,
    TURBO_MEDIA_PROTOCOL_HLS = 3,
    TURBO_MEDIA_PROTOCOL_HTTP_FLV = 4,
    TURBO_MEDIA_PROTOCOL_WEBRTC = 5
} turbo_media_protocol_t;

typedef enum {
    TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER = 1,
    TURBO_MEDIA_PROTOCOL_ROLE_PLAYER = 2
} turbo_media_protocol_role_t;

typedef struct {
    size_t max_sources;
    turbo_media_source_config_t source_config;
    void *user_data;
} turbo_media_server_config_t;

typedef struct {
    uint64_t source_count;
    uint64_t tracks_registered;
    uint64_t frames_published;
    uint64_t subscriptions_created;
    uint64_t subscriptions_removed;
    uint64_t sources_removed;
} turbo_media_server_stats_t;

typedef struct {
    turbo_media_protocol_t protocol;
    turbo_media_protocol_role_t role;
    turbo_media_source_key_t key;
    const turbo_media_track_info_t *tracks;
    size_t track_count;
    turbo_media_frame_cb callback;
    void *callback_user_data;
    int replay_cached;
    int remove_source_on_close;
} turbo_media_protocol_session_config_t;

TURBO_MEDIA_API turbo_media_server_t *turbo_media_server_create(
    const turbo_media_server_config_t *config);

TURBO_MEDIA_API void turbo_media_server_destroy(
    turbo_media_server_t *server);

TURBO_MEDIA_API int turbo_media_server_start(
    turbo_media_server_t *server);

TURBO_MEDIA_API int turbo_media_server_stop(
    turbo_media_server_t *server);

TURBO_MEDIA_API turbo_media_server_runtime_t *turbo_media_server_get_runtime(
    turbo_media_server_t *server);

TURBO_MEDIA_API turbo_media_server_runtime_t *turbo_media_server_runtime_create(
    const turbo_media_server_config_t *config);

TURBO_MEDIA_API void turbo_media_server_runtime_destroy(
    turbo_media_server_runtime_t *runtime);

TURBO_MEDIA_API void *turbo_media_server_runtime_user_data(
    const turbo_media_server_runtime_t *runtime);

TURBO_MEDIA_API int turbo_media_server_runtime_get_or_create_source(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_source_key_t *key,
    turbo_media_source_t **source);

TURBO_MEDIA_API int turbo_media_server_runtime_find_source(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_source_key_t *key,
    turbo_media_source_t **source);

TURBO_MEDIA_API int turbo_media_server_runtime_remove_source(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_source_key_t *key);

TURBO_MEDIA_API int turbo_media_server_runtime_add_track(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_source_key_t *key,
    const turbo_media_track_info_t *track,
    int *track_id);

TURBO_MEDIA_API int turbo_media_server_runtime_publish(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_source_key_t *key,
    const turbo_media_frame_t *frame);

TURBO_MEDIA_API int turbo_media_server_runtime_subscribe(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_source_key_t *key,
    turbo_media_frame_cb callback,
    void *user_data,
    int replay_cached,
    uint64_t *subscription_id);

TURBO_MEDIA_API int turbo_media_server_runtime_unsubscribe(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_source_key_t *key,
    uint64_t subscription_id);

TURBO_MEDIA_API int turbo_media_server_runtime_get_stats(
    const turbo_media_server_runtime_t *runtime,
    turbo_media_server_stats_t *stats);

TURBO_MEDIA_API int turbo_media_server_protocol_session_open(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_protocol_session_config_t *config,
    turbo_media_protocol_session_t **session);

TURBO_MEDIA_API void turbo_media_server_protocol_session_close(
    turbo_media_protocol_session_t *session);

TURBO_MEDIA_API int turbo_media_server_protocol_session_publish(
    turbo_media_protocol_session_t *session,
    const turbo_media_frame_t *frame);

TURBO_MEDIA_API const turbo_media_source_key_t *turbo_media_server_protocol_session_key(
    const turbo_media_protocol_session_t *session);

TURBO_MEDIA_API turbo_media_protocol_t turbo_media_server_protocol_session_protocol(
    const turbo_media_protocol_session_t *session);

TURBO_MEDIA_API turbo_media_protocol_role_t turbo_media_server_protocol_session_role(
    const turbo_media_protocol_session_t *session);

TURBO_MEDIA_API int turbo_media_server_rtmp_source_key(
    const char *vhost,
    const char *app,
    const char *stream,
    turbo_media_source_key_t *key);

TURBO_MEDIA_API int turbo_media_server_rtmp_open_publish(
    turbo_media_server_runtime_t *runtime,
    const char *vhost,
    const char *app,
    const char *stream,
    int remove_source_on_close,
    turbo_media_protocol_session_t **session);

TURBO_MEDIA_API int turbo_media_server_rtmp_open_play(
    turbo_media_server_runtime_t *runtime,
    const char *vhost,
    const char *app,
    const char *stream,
    turbo_media_frame_cb callback,
    void *callback_user_data,
    int replay_cached,
    turbo_media_protocol_session_t **session);

TURBO_MEDIA_API int turbo_media_server_rtmp_publish_video(
    turbo_media_protocol_session_t *session,
    const void *flv_video_tag,
    size_t bytes,
    uint32_t timestamp_ms);

TURBO_MEDIA_API int turbo_media_server_rtmp_publish_audio(
    turbo_media_protocol_session_t *session,
    const void *flv_audio_tag,
    size_t bytes,
    uint32_t timestamp_ms);

TURBO_MEDIA_API int turbo_media_server_rtsp_source_key(
    const char *vhost,
    const char *uri,
    turbo_media_source_key_t *key);

TURBO_MEDIA_API int turbo_media_server_rtsp_open_record(
    turbo_media_server_runtime_t *runtime,
    const char *vhost,
    const char *uri,
    size_t rtp_channel_count,
    int remove_source_on_close,
    turbo_media_protocol_session_t **session);

TURBO_MEDIA_API int turbo_media_server_rtsp_open_record_tracks(
    turbo_media_server_runtime_t *runtime,
    const char *vhost,
    const char *uri,
    const turbo_media_track_info_t *tracks,
    size_t track_count,
    int remove_source_on_close,
    turbo_media_protocol_session_t **session);

TURBO_MEDIA_API int turbo_media_server_rtsp_open_play(
    turbo_media_server_runtime_t *runtime,
    const char *vhost,
    const char *uri,
    turbo_media_frame_cb callback,
    void *callback_user_data,
    int replay_cached,
    turbo_media_protocol_session_t **session);

TURBO_MEDIA_API int turbo_media_server_rtsp_publish_interleaved(
    turbo_media_protocol_session_t *session,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_len,
    uint32_t timestamp_ms);

#ifdef TURBO_MEDIA_HAS_RTSP
typedef struct {
    const char *vhost;
    const char *session_id; /* Optional prefix; each RTSP connection gets a unique suffix. */
    size_t default_rtp_channel_count;
    int remove_source_on_close;
    int replay_cached;
} turbo_media_rtsp_server_adapter_config_t;

TURBO_MEDIA_API turbo_media_rtsp_server_adapter_t *turbo_media_server_rtsp_adapter_create(
    turbo_media_server_runtime_t *runtime,
    const turbo_rtsp_server_config_t *rtsp_config,
    const turbo_media_rtsp_server_adapter_config_t *adapter_config);

TURBO_MEDIA_API void turbo_media_server_rtsp_adapter_destroy(
    turbo_media_rtsp_server_adapter_t *adapter);

TURBO_MEDIA_API int turbo_media_server_rtsp_adapter_start(
    turbo_media_rtsp_server_adapter_t *adapter);

TURBO_MEDIA_API int turbo_media_server_rtsp_adapter_stop(
    turbo_media_rtsp_server_adapter_t *adapter);

TURBO_MEDIA_API turbo_rtsp_server_t *turbo_media_server_rtsp_adapter_rtsp_server(
    turbo_media_rtsp_server_adapter_t *adapter);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_SERVER_H */
