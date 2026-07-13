#include "turbo_media_server.h"

#include "CoroNet/turbo_coro_context.h"

#include <stdlib.h>
#include <string.h>

#define TURBO_MEDIA_SERVER_DEFAULT_MAX_SOURCES 1024
#define TURBO_MEDIA_SERVER_DEFAULT_VHOST "default"
#define TURBO_MEDIA_SERVER_DEFAULT_APP "live"
#define TURBO_MEDIA_RTMP_VIDEO_TRACK_ID 0
#define TURBO_MEDIA_RTMP_AUDIO_TRACK_ID 1
#define TURBO_MEDIA_RTSP_MAX_RUNTIME_CHANNELS 8

struct turbo_media_server_s {
    turbo_media_server_runtime_t *runtime;
    struct coro_context_s *coro_context;
    int owns_coro_context;
    int started;
};

struct turbo_media_server_runtime_s {
    turbo_media_server_config_t config;
    turbo_media_registry_t *registry;
    turbo_media_server_stats_t stats;
};

struct turbo_media_protocol_session_s {
    turbo_media_server_runtime_t *runtime;
    turbo_media_protocol_t protocol;
    turbo_media_protocol_role_t role;
    turbo_media_source_key_t key;
    uint64_t subscription_id;
    int remove_source_on_close;
    int closed;
};

static size_t turbo_media_server_default_size(size_t value, size_t default_value) {
    return value == 0 ? default_value : value;
}

static int turbo_media_server_find_existing_source(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_source_key_t *key,
    turbo_media_source_t **source) {
    turbo_media_source_t *found;

    if (!runtime || !key || !source) return TURBO_MEDIA_ERR_INVALID;

    found = turbo_media_registry_find(runtime->registry, key);
    if (!found) {
        *source = NULL;
        return TURBO_MEDIA_ERR_NOT_FOUND;
    }

    *source = found;
    return TURBO_MEDIA_OK;
}

turbo_media_server_t *turbo_media_server_create(
    const turbo_media_server_config_t *config) {
    turbo_media_server_t *server;
    turbo_media_server_config_t runtime_config;

    server = (turbo_media_server_t *)calloc(1, sizeof(*server));
    if (!server) return NULL;

    if (config) {
        runtime_config = *config;
    } else {
        memset(&runtime_config, 0, sizeof(runtime_config));
    }

    if (runtime_config.coro_context) {
        server->coro_context = runtime_config.coro_context;
    } else {
        server->coro_context = (struct coro_context_s *)coro_context_create(NULL);
        if (!server->coro_context) {
            free(server);
            return NULL;
        }
        server->owns_coro_context = 1;
        runtime_config.coro_context = server->coro_context;
    }

    server->runtime = turbo_media_server_runtime_create(&runtime_config);
    if (!server->runtime) {
        if (server->owns_coro_context) {
            coro_context_destroy((coro_context_t *)server->coro_context);
        }
        free(server);
        return NULL;
    }

    return server;
}

void turbo_media_server_destroy(turbo_media_server_t *server) {
    if (!server) return;

    (void)turbo_media_server_stop(server);

    turbo_media_server_runtime_destroy(server->runtime);
    if (server->owns_coro_context && server->coro_context) {
        coro_context_destroy((coro_context_t *)server->coro_context);
    }
    free(server);
}

int turbo_media_server_start(turbo_media_server_t *server) {
    if (!server || !server->runtime || !server->coro_context) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    server->started = 1;
    return TURBO_MEDIA_OK;
}

int turbo_media_server_stop(turbo_media_server_t *server) {
    if (!server) return TURBO_MEDIA_ERR_INVALID;
    if (!server->started) return TURBO_MEDIA_OK;

    if (server->owns_coro_context && server->coro_context) {
        coro_context_stop((coro_context_t *)server->coro_context);
    }
    server->started = 0;
    return TURBO_MEDIA_OK;
}

turbo_media_server_runtime_t *turbo_media_server_get_runtime(
    turbo_media_server_t *server) {
    return server ? server->runtime : NULL;
}

turbo_media_server_runtime_t *turbo_media_server_runtime_create(
    const turbo_media_server_config_t *config) {
    turbo_media_server_runtime_t *runtime;
    size_t max_sources;

    runtime = (turbo_media_server_runtime_t *)calloc(1, sizeof(*runtime));
    if (!runtime) return NULL;

    if (config) runtime->config = *config;

    max_sources = turbo_media_server_default_size(runtime->config.max_sources,
                                                  TURBO_MEDIA_SERVER_DEFAULT_MAX_SOURCES);
    runtime->config.max_sources = max_sources;
    runtime->registry = turbo_media_registry_create(max_sources);
    if (!runtime->registry) {
        free(runtime);
        return NULL;
    }

    return runtime;
}

void turbo_media_server_runtime_destroy(turbo_media_server_runtime_t *runtime) {
    if (!runtime) return;

    turbo_media_registry_destroy(runtime->registry);
    free(runtime);
}

struct coro_context_s *turbo_media_server_runtime_coro_context(
    const turbo_media_server_runtime_t *runtime) {
    return runtime ? runtime->config.coro_context : NULL;
}

void *turbo_media_server_runtime_user_data(const turbo_media_server_runtime_t *runtime) {
    return runtime ? runtime->config.user_data : NULL;
}

int turbo_media_server_runtime_get_or_create_source(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_source_key_t *key,
    turbo_media_source_t **source) {
    int rc;

    if (!runtime || !key || !source) return TURBO_MEDIA_ERR_INVALID;

    rc = turbo_media_registry_get_or_create(runtime->registry,
                                            key,
                                            &runtime->config.source_config,
                                            source);
    if (rc == TURBO_MEDIA_OK) {
        runtime->stats.source_count = turbo_media_registry_count(runtime->registry);
    }

    return rc;
}

int turbo_media_server_runtime_find_source(turbo_media_server_runtime_t *runtime,
                                           const turbo_media_source_key_t *key,
                                           turbo_media_source_t **source) {
    return turbo_media_server_find_existing_source(runtime, key, source);
}

int turbo_media_server_runtime_remove_source(turbo_media_server_runtime_t *runtime,
                                             const turbo_media_source_key_t *key) {
    int rc;

    if (!runtime || !key) return TURBO_MEDIA_ERR_INVALID;

    rc = turbo_media_registry_remove(runtime->registry, key);
    if (rc == TURBO_MEDIA_OK) {
        runtime->stats.sources_removed++;
        runtime->stats.source_count = turbo_media_registry_count(runtime->registry);
    }

    return rc;
}

int turbo_media_server_runtime_add_track(turbo_media_server_runtime_t *runtime,
                                         const turbo_media_source_key_t *key,
                                         const turbo_media_track_info_t *track,
                                         int *track_id) {
    turbo_media_source_t *source;
    int rc;

    if (!runtime || !key || !track) return TURBO_MEDIA_ERR_INVALID;

    rc = turbo_media_server_runtime_get_or_create_source(runtime, key, &source);
    if (rc != TURBO_MEDIA_OK) return rc;

    rc = turbo_media_source_add_track(source, track, track_id);
    if (rc == TURBO_MEDIA_OK) {
        runtime->stats.tracks_registered++;
    }

    return rc;
}

int turbo_media_server_runtime_publish(turbo_media_server_runtime_t *runtime,
                                       const turbo_media_source_key_t *key,
                                       const turbo_media_frame_t *frame) {
    turbo_media_source_t *source;
    int rc;

    rc = turbo_media_server_find_existing_source(runtime, key, &source);
    if (rc != TURBO_MEDIA_OK) return rc;

    rc = turbo_media_source_publish(source, frame);
    if (rc == TURBO_MEDIA_OK) {
        runtime->stats.frames_published++;
    }

    return rc;
}

int turbo_media_server_runtime_subscribe(turbo_media_server_runtime_t *runtime,
                                         const turbo_media_source_key_t *key,
                                         turbo_media_frame_cb callback,
                                         void *user_data,
                                         int replay_cached,
                                         uint64_t *subscription_id) {
    turbo_media_source_t *source;
    int rc;

    rc = turbo_media_server_find_existing_source(runtime, key, &source);
    if (rc != TURBO_MEDIA_OK) return rc;

    rc = turbo_media_source_subscribe(source,
                                      callback,
                                      user_data,
                                      replay_cached,
                                      subscription_id);
    if (rc == TURBO_MEDIA_OK) {
        runtime->stats.subscriptions_created++;
    }

    return rc;
}

int turbo_media_server_runtime_unsubscribe(turbo_media_server_runtime_t *runtime,
                                           const turbo_media_source_key_t *key,
                                           uint64_t subscription_id) {
    turbo_media_source_t *source;
    int rc;

    rc = turbo_media_server_find_existing_source(runtime, key, &source);
    if (rc != TURBO_MEDIA_OK) return rc;

    rc = turbo_media_source_unsubscribe(source, subscription_id);
    if (rc == TURBO_MEDIA_OK) {
        runtime->stats.subscriptions_removed++;
    }

    return rc;
}

int turbo_media_server_runtime_get_stats(const turbo_media_server_runtime_t *runtime,
                                         turbo_media_server_stats_t *stats) {
    if (!runtime || !stats) return TURBO_MEDIA_ERR_INVALID;

    *stats = runtime->stats;
    stats->source_count = turbo_media_registry_count(runtime->registry);
    return TURBO_MEDIA_OK;
}

static int turbo_media_server_protocol_valid(turbo_media_protocol_t protocol) {
    return protocol >= TURBO_MEDIA_PROTOCOL_RTMP &&
           protocol <= TURBO_MEDIA_PROTOCOL_WEBRTC;
}

static int turbo_media_server_role_valid(turbo_media_protocol_role_t role) {
    return role == TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER ||
           role == TURBO_MEDIA_PROTOCOL_ROLE_PLAYER;
}

static int turbo_media_server_session_add_tracks(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_source_key_t *key,
    const turbo_media_track_info_t *tracks,
    size_t track_count) {
    size_t i;
    int rc;

    if (track_count > 0 && !tracks) return TURBO_MEDIA_ERR_INVALID;

    for (i = 0; i < track_count; ++i) {
        rc = turbo_media_server_runtime_add_track(runtime, key, &tracks[i], NULL);
        if (rc != TURBO_MEDIA_OK) return rc;
    }

    return TURBO_MEDIA_OK;
}

int turbo_media_server_protocol_session_open(
    turbo_media_server_runtime_t *runtime,
    const turbo_media_protocol_session_config_t *config,
    turbo_media_protocol_session_t **session) {
    turbo_media_protocol_session_t *opened;
    turbo_media_source_t *source;
    int rc;

    if (!runtime || !config || !session ||
        !turbo_media_server_protocol_valid(config->protocol) ||
        !turbo_media_server_role_valid(config->role)) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    *session = NULL;
    opened = (turbo_media_protocol_session_t *)calloc(1, sizeof(*opened));
    if (!opened) return TURBO_MEDIA_ERR_NOMEM;

    opened->runtime = runtime;
    opened->protocol = config->protocol;
    opened->role = config->role;
    opened->key = config->key;
    opened->remove_source_on_close = config->remove_source_on_close;

    if (config->role == TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER) {
        rc = turbo_media_server_runtime_get_or_create_source(runtime, &config->key, &source);
        if (rc == TURBO_MEDIA_OK) {
            rc = turbo_media_server_session_add_tracks(
                runtime,
                &config->key,
                config->tracks,
                config->track_count);
        }
    } else {
        if (!config->callback) {
            free(opened);
            return TURBO_MEDIA_ERR_INVALID;
        }

        rc = turbo_media_server_runtime_subscribe(runtime,
                                                  &config->key,
                                                  config->callback,
                                                  config->callback_user_data,
                                                  config->replay_cached,
                                                  &opened->subscription_id);
    }

    if (rc != TURBO_MEDIA_OK) {
        free(opened);
        return rc;
    }

    *session = opened;
    return TURBO_MEDIA_OK;
}

void turbo_media_server_protocol_session_close(turbo_media_protocol_session_t *session) {
    if (!session || session->closed) return;

    if (session->role == TURBO_MEDIA_PROTOCOL_ROLE_PLAYER &&
        session->subscription_id != 0) {
        (void)turbo_media_server_runtime_unsubscribe(
            session->runtime,
            &session->key,
            session->subscription_id);
        session->subscription_id = 0;
    }

    if (session->role == TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER &&
        session->remove_source_on_close) {
        (void)turbo_media_server_runtime_remove_source(session->runtime, &session->key);
    }

    session->closed = 1;
    free(session);
}

int turbo_media_server_protocol_session_publish(
    turbo_media_protocol_session_t *session,
    const turbo_media_frame_t *frame) {
    if (!session || session->closed || !frame) return TURBO_MEDIA_ERR_INVALID;
    if (session->role != TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER) {
        return TURBO_MEDIA_ERR_STATE;
    }

    return turbo_media_server_runtime_publish(session->runtime, &session->key, frame);
}

const turbo_media_source_key_t *turbo_media_server_protocol_session_key(
    const turbo_media_protocol_session_t *session) {
    return session && !session->closed ? &session->key : NULL;
}

turbo_media_protocol_t turbo_media_server_protocol_session_protocol(
    const turbo_media_protocol_session_t *session) {
    return session ? session->protocol : TURBO_MEDIA_PROTOCOL_UNKNOWN;
}

turbo_media_protocol_role_t turbo_media_server_protocol_session_role(
    const turbo_media_protocol_session_t *session) {
    return session ? session->role : 0;
}

static const char *turbo_media_nonempty_or_default(const char *value,
                                                   const char *default_value) {
    return value && value[0] != '\0' ? value : default_value;
}

static void turbo_media_server_default_rtmp_tracks(turbo_media_track_info_t tracks[2]) {
    memset(tracks, 0, sizeof(turbo_media_track_info_t) * 2);

    tracks[0].track_id = TURBO_MEDIA_RTMP_VIDEO_TRACK_ID;
    tracks[0].type = TURBO_MEDIA_TRACK_VIDEO;
    tracks[0].payload_type = 0;
    tracks[0].clock_rate = 1000;
    strcpy(tracks[0].codec_name, "flv-video");

    tracks[1].track_id = TURBO_MEDIA_RTMP_AUDIO_TRACK_ID;
    tracks[1].type = TURBO_MEDIA_TRACK_AUDIO;
    tracks[1].payload_type = 0;
    tracks[1].clock_rate = 1000;
    strcpy(tracks[1].codec_name, "flv-audio");
}

static void turbo_media_server_default_rtsp_track(turbo_media_track_info_t *track,
                                                  int track_id) {
    memset(track, 0, sizeof(*track));
    track->track_id = track_id;
    track->type = TURBO_MEDIA_TRACK_DATA;
    track->payload_type = track_id;
    track->clock_rate = 90000;
    strcpy(track->codec_name, "rtp");
}

static int turbo_media_server_open_session_with_key(
    turbo_media_server_runtime_t *runtime,
    turbo_media_protocol_t protocol,
    turbo_media_protocol_role_t role,
    const turbo_media_source_key_t *key,
    const turbo_media_track_info_t *tracks,
    size_t track_count,
    turbo_media_frame_cb callback,
    void *callback_user_data,
    int replay_cached,
    int remove_source_on_close,
    turbo_media_protocol_session_t **session) {
    turbo_media_protocol_session_config_t config;

    if (!key) return TURBO_MEDIA_ERR_INVALID;

    memset(&config, 0, sizeof(config));
    config.protocol = protocol;
    config.role = role;
    config.key = *key;
    config.tracks = tracks;
    config.track_count = track_count;
    config.callback = callback;
    config.callback_user_data = callback_user_data;
    config.replay_cached = replay_cached;
    config.remove_source_on_close = remove_source_on_close;
    return turbo_media_server_protocol_session_open(runtime, &config, session);
}

int turbo_media_server_rtmp_source_key(const char *vhost,
                                       const char *app,
                                       const char *stream,
                                       turbo_media_source_key_t *key) {
    return turbo_media_source_key_init(
        key,
        turbo_media_nonempty_or_default(vhost, TURBO_MEDIA_SERVER_DEFAULT_VHOST),
        turbo_media_nonempty_or_default(app, TURBO_MEDIA_SERVER_DEFAULT_APP),
        stream);
}

int turbo_media_server_rtmp_open_publish(turbo_media_server_runtime_t *runtime,
                                         const char *vhost,
                                         const char *app,
                                         const char *stream,
                                         int remove_source_on_close,
                                         turbo_media_protocol_session_t **session) {
    turbo_media_source_key_t key;
    turbo_media_track_info_t tracks[2];
    int rc;

    rc = turbo_media_server_rtmp_source_key(vhost, app, stream, &key);
    if (rc != TURBO_MEDIA_OK) return rc;

    turbo_media_server_default_rtmp_tracks(tracks);
    return turbo_media_server_open_session_with_key(runtime,
                                                    TURBO_MEDIA_PROTOCOL_RTMP,
                                                    TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER,
                                                    &key,
                                                    tracks,
                                                    2,
                                                    NULL,
                                                    NULL,
                                                    0,
                                                    remove_source_on_close,
                                                    session);
}

int turbo_media_server_rtmp_open_play(turbo_media_server_runtime_t *runtime,
                                      const char *vhost,
                                      const char *app,
                                      const char *stream,
                                      turbo_media_frame_cb callback,
                                      void *callback_user_data,
                                      int replay_cached,
                                      turbo_media_protocol_session_t **session) {
    turbo_media_source_key_t key;
    int rc;

    rc = turbo_media_server_rtmp_source_key(vhost, app, stream, &key);
    if (rc != TURBO_MEDIA_OK) return rc;

    return turbo_media_server_open_session_with_key(runtime,
                                                    TURBO_MEDIA_PROTOCOL_RTMP,
                                                    TURBO_MEDIA_PROTOCOL_ROLE_PLAYER,
                                                    &key,
                                                    NULL,
                                                    0,
                                                    callback,
                                                    callback_user_data,
                                                    replay_cached,
                                                    0,
                                                    session);
}

int turbo_media_server_rtmp_publish_video(turbo_media_protocol_session_t *session,
                                          const void *flv_video_tag,
                                          size_t bytes,
                                          uint32_t timestamp_ms) {
    turbo_media_frame_t frame;
    const uint8_t *tag = (const uint8_t *)flv_video_tag;

    if (!tag && bytes > 0) return TURBO_MEDIA_ERR_INVALID;

    memset(&frame, 0, sizeof(frame));
    frame.track_id = TURBO_MEDIA_RTMP_VIDEO_TRACK_ID;
    frame.data = tag;
    frame.size = bytes;
    frame.pts = timestamp_ms;
    frame.dts = timestamp_ms;
    frame.is_keyframe = bytes > 0 && ((tag[0] >> 4) == 1);
    return turbo_media_server_protocol_session_publish(session, &frame);
}

int turbo_media_server_rtmp_publish_audio(turbo_media_protocol_session_t *session,
                                          const void *flv_audio_tag,
                                          size_t bytes,
                                          uint32_t timestamp_ms) {
    turbo_media_frame_t frame;

    if (!flv_audio_tag && bytes > 0) return TURBO_MEDIA_ERR_INVALID;

    memset(&frame, 0, sizeof(frame));
    frame.track_id = TURBO_MEDIA_RTMP_AUDIO_TRACK_ID;
    frame.data = (const uint8_t *)flv_audio_tag;
    frame.size = bytes;
    frame.pts = timestamp_ms;
    frame.dts = timestamp_ms;
    return turbo_media_server_protocol_session_publish(session, &frame);
}

int turbo_media_server_rtsp_source_key(const char *vhost,
                                       const char *uri,
                                       turbo_media_source_key_t *key) {
    const char *path;
    const char *app_start;
    const char *app_end;
    const char *stream_start;
    char app[TURBO_MEDIA_MAX_APP_LEN];
    char stream[TURBO_MEDIA_MAX_STREAM_LEN];
    size_t app_len;
    size_t stream_len;

    if (!uri || !key) return TURBO_MEDIA_ERR_INVALID;

    path = strstr(uri, "://");
    if (path) {
        path = strchr(path + 3, '/');
    } else {
        path = uri;
    }
    if (!path) return TURBO_MEDIA_ERR_INVALID;

    while (*path == '/') path++;
    if (*path == '\0') return TURBO_MEDIA_ERR_INVALID;

    app_start = path;
    app_end = strchr(app_start, '/');
    if (!app_end) {
        return turbo_media_source_key_init(
            key,
            turbo_media_nonempty_or_default(vhost, TURBO_MEDIA_SERVER_DEFAULT_VHOST),
            TURBO_MEDIA_SERVER_DEFAULT_APP,
            app_start);
    }

    stream_start = app_end + 1;
    if (*stream_start == '\0') return TURBO_MEDIA_ERR_INVALID;

    app_len = (size_t)(app_end - app_start);
    stream_len = strlen(stream_start);
    if (app_len == 0 || app_len >= sizeof(app) ||
        stream_len == 0 || stream_len >= sizeof(stream)) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    memcpy(app, app_start, app_len);
    app[app_len] = '\0';
    memcpy(stream, stream_start, stream_len + 1);
    return turbo_media_source_key_init(
        key,
        turbo_media_nonempty_or_default(vhost, TURBO_MEDIA_SERVER_DEFAULT_VHOST),
        app,
        stream);
}

int turbo_media_server_rtsp_open_record(turbo_media_server_runtime_t *runtime,
                                        const char *vhost,
                                        const char *uri,
                                        size_t rtp_channel_count,
                                        int remove_source_on_close,
                                        turbo_media_protocol_session_t **session) {
    turbo_media_source_key_t key;
    turbo_media_track_info_t tracks[TURBO_MEDIA_RTSP_MAX_RUNTIME_CHANNELS];
    size_t i;
    int rc;

    if (rtp_channel_count == 0 ||
        rtp_channel_count > TURBO_MEDIA_RTSP_MAX_RUNTIME_CHANNELS) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    rc = turbo_media_server_rtsp_source_key(vhost, uri, &key);
    if (rc != TURBO_MEDIA_OK) return rc;

    for (i = 0; i < rtp_channel_count; ++i) {
        turbo_media_server_default_rtsp_track(&tracks[i], (int)i);
    }

    return turbo_media_server_open_session_with_key(runtime,
                                                    TURBO_MEDIA_PROTOCOL_RTSP,
                                                    TURBO_MEDIA_PROTOCOL_ROLE_PUBLISHER,
                                                    &key,
                                                    tracks,
                                                    rtp_channel_count,
                                                    NULL,
                                                    NULL,
                                                    0,
                                                    remove_source_on_close,
                                                    session);
}

int turbo_media_server_rtsp_open_play(turbo_media_server_runtime_t *runtime,
                                      const char *vhost,
                                      const char *uri,
                                      turbo_media_frame_cb callback,
                                      void *callback_user_data,
                                      int replay_cached,
                                      turbo_media_protocol_session_t **session) {
    turbo_media_source_key_t key;
    int rc;

    rc = turbo_media_server_rtsp_source_key(vhost, uri, &key);
    if (rc != TURBO_MEDIA_OK) return rc;

    return turbo_media_server_open_session_with_key(runtime,
                                                    TURBO_MEDIA_PROTOCOL_RTSP,
                                                    TURBO_MEDIA_PROTOCOL_ROLE_PLAYER,
                                                    &key,
                                                    NULL,
                                                    0,
                                                    callback,
                                                    callback_user_data,
                                                    replay_cached,
                                                    0,
                                                    session);
}

int turbo_media_server_rtsp_publish_interleaved(turbo_media_protocol_session_t *session,
                                                uint8_t channel,
                                                const uint8_t *payload,
                                                size_t payload_len,
                                                uint32_t timestamp_ms) {
    turbo_media_frame_t frame;

    if (!payload && payload_len > 0) return TURBO_MEDIA_ERR_INVALID;

    memset(&frame, 0, sizeof(frame));
    frame.track_id = (int)channel;
    frame.data = payload;
    frame.size = payload_len;
    frame.pts = timestamp_ms;
    frame.dts = timestamp_ms;
    return turbo_media_server_protocol_session_publish(session, &frame);
}
