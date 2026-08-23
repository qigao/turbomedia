#ifndef TURBO_MEDIA_SOURCE_H
#define TURBO_MEDIA_SOURCE_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MEDIA_MAX_VHOST_LEN 64
#define TURBO_MEDIA_MAX_APP_LEN 64
#define TURBO_MEDIA_MAX_STREAM_LEN 128
#define TURBO_MEDIA_MAX_CODEC_NAME_LEN 32
#define TURBO_MEDIA_DEFAULT_MAX_TRACKS 8
#define TURBO_MEDIA_DEFAULT_MAX_SUBSCRIBERS 64
#define TURBO_MEDIA_DEFAULT_GOP_CAPACITY 128

typedef enum {
    TURBO_MEDIA_OK = 0,
    TURBO_MEDIA_ERR_INVALID = -1,
    TURBO_MEDIA_ERR_NOMEM = -2,
    TURBO_MEDIA_ERR_EXISTS = -3,
    TURBO_MEDIA_ERR_NOT_FOUND = -4,
    TURBO_MEDIA_ERR_FULL = -5,
    TURBO_MEDIA_ERR_STATE = -6
} turbo_media_result_t;

typedef enum {
    TURBO_MEDIA_TRACK_AUDIO = 1,
    TURBO_MEDIA_TRACK_VIDEO = 2,
    TURBO_MEDIA_TRACK_DATA = 3
} turbo_media_track_type_t;

typedef enum {
    TURBO_MEDIA_SOURCE_IDLE = 0,
    TURBO_MEDIA_SOURCE_PUBLISHING = 1,
    TURBO_MEDIA_SOURCE_CLOSED = 2
} turbo_media_source_state_t;

typedef struct {
    char vhost[TURBO_MEDIA_MAX_VHOST_LEN];
    char app[TURBO_MEDIA_MAX_APP_LEN];
    char stream[TURBO_MEDIA_MAX_STREAM_LEN];
} turbo_media_source_key_t;

typedef struct {
    size_t max_tracks;
    size_t max_subscribers;
    size_t gop_capacity;
} turbo_media_source_config_t;

typedef struct {
    int track_id;
    turbo_media_track_type_t type;
    char codec_name[TURBO_MEDIA_MAX_CODEC_NAME_LEN];
    int payload_type;
    int clock_rate;
    int width;
    int height;
    int framerate;
    int sample_rate;
    int channels;
    const uint8_t *extradata;
    size_t extradata_size;
} turbo_media_track_info_t;

typedef struct {
    int track_id;
    const uint8_t *data;
    size_t size;
    int64_t pts;
    int64_t dts;
    int64_t duration;
    int is_keyframe;
    uint32_t flags;
} turbo_media_frame_t;

typedef struct {
    uint64_t frames_published;
    uint64_t bytes_published;
    uint64_t frames_delivered;
    uint64_t subscriber_count;
    uint64_t gop_cached_frames;
} turbo_media_source_stats_t;

typedef struct turbo_media_source_s turbo_media_source_t;
typedef struct turbo_media_registry_s turbo_media_registry_t;

typedef int (*turbo_media_frame_cb)(turbo_media_source_t *source,
                                    const turbo_media_frame_t *frame,
                                    void *user_data);

TURBO_MEDIA_API int turbo_media_source_key_init(turbo_media_source_key_t *key,
                                          const char *vhost,
                                          const char *app,
                                          const char *stream);

TURBO_MEDIA_API int turbo_media_source_key_equal(const turbo_media_source_key_t *lhs,
                                           const turbo_media_source_key_t *rhs);

TURBO_MEDIA_API turbo_media_source_t *turbo_media_source_create(
    const turbo_media_source_key_t *key,
    const turbo_media_source_config_t *config);

TURBO_MEDIA_API void turbo_media_source_destroy(turbo_media_source_t *source);

TURBO_MEDIA_API const turbo_media_source_key_t *turbo_media_source_key(
    const turbo_media_source_t *source);

TURBO_MEDIA_API turbo_media_source_state_t turbo_media_source_state(
    const turbo_media_source_t *source);

TURBO_MEDIA_API int turbo_media_source_add_track(turbo_media_source_t *source,
                                           const turbo_media_track_info_t *track,
                                           int *track_id);

TURBO_MEDIA_API int turbo_media_source_get_track(const turbo_media_source_t *source,
                                           int track_id,
                                           turbo_media_track_info_t *track);

TURBO_MEDIA_API int turbo_media_source_get_track_at(const turbo_media_source_t *source,
                                              size_t index,
                                              turbo_media_track_info_t *track);

TURBO_MEDIA_API size_t turbo_media_source_track_count(const turbo_media_source_t *source);

TURBO_MEDIA_API int turbo_media_source_publish(turbo_media_source_t *source,
                                         const turbo_media_frame_t *frame);

TURBO_MEDIA_API int turbo_media_source_subscribe(turbo_media_source_t *source,
                                           turbo_media_frame_cb callback,
                                           void *user_data,
                                           int replay_cached,
                                           uint64_t *subscription_id);

TURBO_MEDIA_API int turbo_media_source_unsubscribe(turbo_media_source_t *source,
                                             uint64_t subscription_id);

TURBO_MEDIA_API int turbo_media_source_get_stats(const turbo_media_source_t *source,
                                           turbo_media_source_stats_t *stats);

TURBO_MEDIA_API turbo_media_registry_t *turbo_media_registry_create(size_t max_sources);

TURBO_MEDIA_API void turbo_media_registry_destroy(turbo_media_registry_t *registry);

TURBO_MEDIA_API turbo_media_source_t *turbo_media_registry_find(
    const turbo_media_registry_t *registry,
    const turbo_media_source_key_t *key);

TURBO_MEDIA_API int turbo_media_registry_get_or_create(
    turbo_media_registry_t *registry,
    const turbo_media_source_key_t *key,
    const turbo_media_source_config_t *config,
    turbo_media_source_t **source);

TURBO_MEDIA_API int turbo_media_registry_remove(turbo_media_registry_t *registry,
                                          const turbo_media_source_key_t *key);

TURBO_MEDIA_API size_t turbo_media_registry_count(const turbo_media_registry_t *registry);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_SOURCE_H */
