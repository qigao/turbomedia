#include "turbo_media_source.h"
#include <cstl/hash_map.h>

#include <stdlib.h>
#include <string.h>

#define TURBO_MEDIA_REGISTRY_DEFAULT_MAX_SOURCES 1024
#define TURBO_MEDIA_SUBSCRIPTION_NAMESPACE_SHIFT 32u
#define TURBO_MEDIA_SUBSCRIPTION_LOCAL_ID_MAX UINT32_MAX

typedef struct {
    turbo_media_track_info_t info;
    uint8_t *extradata;
} turbo_media_track_state_t;

typedef struct {
    turbo_media_frame_t frame;
    uint8_t *data;
} turbo_media_cached_frame_t;

typedef struct {
    uint64_t id;
    turbo_media_frame_cb callback;
    void *user_data;
} turbo_media_subscription_t;

struct turbo_media_source_s {
    turbo_media_source_key_t key;
    turbo_media_source_config_t config;
    turbo_media_source_state_t state;
    turbo_media_track_state_t *tracks;
    size_t track_count;
    int next_track_id;
    turbo_media_subscription_t *subscribers;
    size_t subscriber_count;
    uint32_t subscription_namespace;
    uint64_t next_subscription_id;
    turbo_media_cached_frame_t *gop_frames;
    size_t gop_count;
    size_t gop_start;
    int has_video_track;
    int has_video_keyframe;
    turbo_media_source_stats_t stats;
};

struct turbo_media_registry_s {
    hash_map_t sources;
    size_t max_sources;
    uint32_t next_subscription_namespace;
};

static size_t turbo_media_default_size(size_t value, size_t default_value) {
    return value == 0 ? default_value : value;
}

static int turbo_media_copy_text(char *dst, size_t dst_size, const char *src) {
    size_t len;

    if (!dst || dst_size == 0 || !src) return TURBO_MEDIA_ERR_INVALID;

    len = strlen(src);
    if (len == 0 || len >= dst_size) return TURBO_MEDIA_ERR_INVALID;

    memcpy(dst, src, len + 1);
    return TURBO_MEDIA_OK;
}

static int turbo_media_key_valid(const turbo_media_source_key_t *key) {
    return key && key->vhost[0] != '\0' && key->app[0] != '\0' && key->stream[0] != '\0' &&
           memchr(key->vhost, '\0', sizeof(key->vhost)) != NULL &&
           memchr(key->app, '\0', sizeof(key->app)) != NULL &&
           memchr(key->stream, '\0', sizeof(key->stream)) != NULL;
}

static int turbo_media_normalize_key(turbo_media_source_key_t *normalized,
                                     const turbo_media_source_key_t *key) {
    if (!normalized || !turbo_media_key_valid(key)) return TURBO_MEDIA_ERR_INVALID;

    return turbo_media_source_key_init(normalized, key->vhost, key->app, key->stream);
}

static void turbo_media_free_track(turbo_media_track_state_t *track) {
    if (!track) return;

    free(track->extradata);
    memset(track, 0, sizeof(*track));
}

static void turbo_media_free_cached_frame(turbo_media_cached_frame_t *cached) {
    if (!cached) return;

    free(cached->data);
    memset(cached, 0, sizeof(*cached));
}

static void turbo_media_clear_gop(turbo_media_source_t *source) {
    size_t i;

    if (!source || !source->gop_frames) return;

    for (i = 0; i < source->config.gop_capacity; ++i) {
        turbo_media_free_cached_frame(&source->gop_frames[i]);
    }

    source->gop_count = 0;
    source->gop_start = 0;
    source->stats.gop_cached_frames = 0;
}

static int turbo_media_track_index(const turbo_media_source_t *source, int track_id) {
    size_t i;

    if (!source) return -1;

    for (i = 0; i < source->track_count; ++i) {
        if (source->tracks[i].info.track_id == track_id) return (int)i;
    }

    return -1;
}

static int turbo_media_copy_track(turbo_media_track_state_t *dst,
                                  const turbo_media_track_info_t *src,
                                  int track_id) {
    if (!dst || !src || src->type < TURBO_MEDIA_TRACK_AUDIO ||
        src->type > TURBO_MEDIA_TRACK_DATA) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    memset(dst, 0, sizeof(*dst));
    dst->info = *src;
    dst->info.track_id = track_id;
    dst->info.extradata = NULL;

    if (src->codec_name[0] == '\0') {
        return TURBO_MEDIA_ERR_INVALID;
    }
    dst->info.codec_name[TURBO_MEDIA_MAX_CODEC_NAME_LEN - 1] = '\0';

    if (src->extradata_size > 0) {
        if (!src->extradata) return TURBO_MEDIA_ERR_INVALID;

        dst->extradata = (uint8_t *)malloc(src->extradata_size);
        if (!dst->extradata) return TURBO_MEDIA_ERR_NOMEM;

        memcpy(dst->extradata, src->extradata, src->extradata_size);
        dst->info.extradata = dst->extradata;
        dst->info.extradata_size = src->extradata_size;
    }

    return TURBO_MEDIA_OK;
}

static int turbo_media_cache_frame(turbo_media_source_t *source,
                                   const turbo_media_frame_t *frame,
                                   const turbo_media_track_info_t *track) {
    turbo_media_cached_frame_t *slot;
    uint8_t *data_copy;
    size_t index;
    int should_cache;
    int starts_video_gop;

    if (!source || !frame || !track || source->config.gop_capacity == 0) {
        return TURBO_MEDIA_OK;
    }

    should_cache = 0;
    starts_video_gop = 0;
    if (!source->has_video_track) {
        should_cache = 1;
    } else if (track->type == TURBO_MEDIA_TRACK_VIDEO && frame->is_keyframe) {
        starts_video_gop = 1;
        should_cache = 1;
    } else if (source->has_video_keyframe) {
        should_cache = 1;
    }

    if (!should_cache) return TURBO_MEDIA_OK;

    data_copy = NULL;
    if (frame->size > 0) {
        data_copy = (uint8_t *)malloc(frame->size);
        if (!data_copy) return TURBO_MEDIA_ERR_NOMEM;
        memcpy(data_copy, frame->data, frame->size);
    }

    if (starts_video_gop) {
        turbo_media_clear_gop(source);
        source->has_video_keyframe = 1;
    }

    if (source->gop_count < source->config.gop_capacity) {
        index = (source->gop_start + source->gop_count) % source->config.gop_capacity;
        source->gop_count++;
    } else {
        index = source->gop_start;
        source->gop_start = (source->gop_start + 1) % source->config.gop_capacity;
        turbo_media_free_cached_frame(&source->gop_frames[index]);
    }

    slot = &source->gop_frames[index];
    slot->frame = *frame;
    slot->data = data_copy;
    slot->frame.data = data_copy;

    source->stats.gop_cached_frames = source->gop_count;
    return TURBO_MEDIA_OK;
}

static int turbo_media_deliver_frame(turbo_media_source_t *source,
                                     const turbo_media_frame_t *frame,
                                     turbo_media_subscription_t *subscriber) {
    int rc;

    if (!subscriber->callback) return TURBO_MEDIA_ERR_INVALID;

    rc = subscriber->callback(source, frame, subscriber->user_data);
    if (rc == TURBO_MEDIA_OK) {
        source->stats.frames_delivered++;
    }

    return rc;
}

int turbo_media_source_key_init(turbo_media_source_key_t *key,
                                const char *vhost,
                                const char *app,
                                const char *stream) {
    int rc;

    if (!key) return TURBO_MEDIA_ERR_INVALID;

    memset(key, 0, sizeof(*key));
    rc = turbo_media_copy_text(key->vhost, sizeof(key->vhost), vhost);
    if (rc != TURBO_MEDIA_OK) return rc;

    rc = turbo_media_copy_text(key->app, sizeof(key->app), app);
    if (rc != TURBO_MEDIA_OK) return rc;

    return turbo_media_copy_text(key->stream, sizeof(key->stream), stream);
}

int turbo_media_source_key_equal(const turbo_media_source_key_t *lhs,
                                 const turbo_media_source_key_t *rhs) {
    if (!turbo_media_key_valid(lhs) || !turbo_media_key_valid(rhs)) return 0;

    return strcmp(lhs->vhost, rhs->vhost) == 0 &&
           strcmp(lhs->app, rhs->app) == 0 &&
           strcmp(lhs->stream, rhs->stream) == 0;
}

turbo_media_source_t *turbo_media_source_create(const turbo_media_source_key_t *key,
                                                const turbo_media_source_config_t *config) {
    turbo_media_source_key_t normalized_key;
    turbo_media_source_config_t normalized;
    turbo_media_source_t *source;

    if (turbo_media_normalize_key(&normalized_key, key) != TURBO_MEDIA_OK) return NULL;

    memset(&normalized, 0, sizeof(normalized));
    if (config) normalized = *config;

    normalized.max_tracks = turbo_media_default_size(normalized.max_tracks,
                                                     TURBO_MEDIA_DEFAULT_MAX_TRACKS);
    normalized.max_subscribers = turbo_media_default_size(normalized.max_subscribers,
                                                          TURBO_MEDIA_DEFAULT_MAX_SUBSCRIBERS);
    normalized.gop_capacity = turbo_media_default_size(normalized.gop_capacity,
                                                       TURBO_MEDIA_DEFAULT_GOP_CAPACITY);

    source = (turbo_media_source_t *)calloc(1, sizeof(*source));
    if (!source) return NULL;

    source->key = normalized_key;
    source->config = normalized;
    source->state = TURBO_MEDIA_SOURCE_IDLE;
    source->next_subscription_id = 1;

    source->tracks = (turbo_media_track_state_t *)calloc(normalized.max_tracks,
                                                         sizeof(*source->tracks));
    source->subscribers = (turbo_media_subscription_t *)calloc(normalized.max_subscribers,
                                                               sizeof(*source->subscribers));
    source->gop_frames = (turbo_media_cached_frame_t *)calloc(normalized.gop_capacity,
                                                              sizeof(*source->gop_frames));

    if (!source->tracks || !source->subscribers || !source->gop_frames) {
        turbo_media_source_destroy(source);
        return NULL;
    }

    return source;
}

void turbo_media_source_destroy(turbo_media_source_t *source) {
    size_t i;

    if (!source) return;

    if (source->tracks) {
        for (i = 0; i < source->track_count; ++i) {
            turbo_media_free_track(&source->tracks[i]);
        }
    }

    turbo_media_clear_gop(source);
    free(source->tracks);
    free(source->subscribers);
    free(source->gop_frames);
    free(source);
}

const turbo_media_source_key_t *turbo_media_source_key(const turbo_media_source_t *source) {
    return source ? &source->key : NULL;
}

turbo_media_source_state_t turbo_media_source_state(const turbo_media_source_t *source) {
    return source ? source->state : TURBO_MEDIA_SOURCE_CLOSED;
}

int turbo_media_source_add_track(turbo_media_source_t *source,
                                 const turbo_media_track_info_t *track,
                                 int *track_id) {
    turbo_media_track_state_t copied;
    int assigned_track_id;
    int rc;

    if (!source || !track) return TURBO_MEDIA_ERR_INVALID;
    if (source->state == TURBO_MEDIA_SOURCE_CLOSED) return TURBO_MEDIA_ERR_STATE;
    if (source->track_count >= source->config.max_tracks) return TURBO_MEDIA_ERR_FULL;

    assigned_track_id = track->track_id >= 0 ? track->track_id : source->next_track_id;
    if (turbo_media_track_index(source, assigned_track_id) >= 0) {
        return TURBO_MEDIA_ERR_EXISTS;
    }

    rc = turbo_media_copy_track(&copied, track, assigned_track_id);
    if (rc != TURBO_MEDIA_OK) return rc;

    source->tracks[source->track_count++] = copied;
    if (assigned_track_id >= source->next_track_id) {
        source->next_track_id = assigned_track_id + 1;
    }

    if (copied.info.type == TURBO_MEDIA_TRACK_VIDEO) {
        source->has_video_track = 1;
    }

    if (track_id) *track_id = assigned_track_id;
    return TURBO_MEDIA_OK;
}

int turbo_media_source_get_track(const turbo_media_source_t *source,
                                 int track_id,
                                 turbo_media_track_info_t *track) {
    int index;

    if (!source || !track) return TURBO_MEDIA_ERR_INVALID;

    index = turbo_media_track_index(source, track_id);
    if (index < 0) return TURBO_MEDIA_ERR_NOT_FOUND;

    *track = source->tracks[index].info;
    return TURBO_MEDIA_OK;
}

int turbo_media_source_get_track_at(const turbo_media_source_t *source,
                                    size_t index,
                                    turbo_media_track_info_t *track) {
    if (!source || !track) return TURBO_MEDIA_ERR_INVALID;
    if (index >= source->track_count) return TURBO_MEDIA_ERR_NOT_FOUND;

    *track = source->tracks[index].info;
    return TURBO_MEDIA_OK;
}

size_t turbo_media_source_track_count(const turbo_media_source_t *source) {
    return source ? source->track_count : 0;
}

int turbo_media_source_publish(turbo_media_source_t *source,
                               const turbo_media_frame_t *frame) {
    int track_index;
    int rc;
    size_t i;

    if (!source || !frame || (frame->size > 0 && !frame->data)) {
        return TURBO_MEDIA_ERR_INVALID;
    }
    if (source->state == TURBO_MEDIA_SOURCE_CLOSED) return TURBO_MEDIA_ERR_STATE;

    track_index = turbo_media_track_index(source, frame->track_id);
    if (track_index < 0) return TURBO_MEDIA_ERR_NOT_FOUND;

    rc = turbo_media_cache_frame(source, frame, &source->tracks[track_index].info);
    if (rc != TURBO_MEDIA_OK) return rc;

    source->state = TURBO_MEDIA_SOURCE_PUBLISHING;
    source->stats.frames_published++;
    source->stats.bytes_published += frame->size;

    for (i = 0; i < source->subscriber_count; ++i) {
        rc = turbo_media_deliver_frame(source, frame, &source->subscribers[i]);
        if (rc != TURBO_MEDIA_OK) return rc;
    }

    return TURBO_MEDIA_OK;
}

int turbo_media_source_subscribe(turbo_media_source_t *source,
                                 turbo_media_frame_cb callback,
                                 void *user_data,
                                 int replay_cached,
                                 uint64_t *subscription_id) {
    turbo_media_subscription_t subscriber;
    size_t i;
    int rc;

    if (!source || !callback) return TURBO_MEDIA_ERR_INVALID;
    if (source->state == TURBO_MEDIA_SOURCE_CLOSED) return TURBO_MEDIA_ERR_STATE;
    if (source->subscriber_count >= source->config.max_subscribers) {
        return TURBO_MEDIA_ERR_FULL;
    }
    if (source->next_subscription_id > TURBO_MEDIA_SUBSCRIPTION_LOCAL_ID_MAX) {
        return TURBO_MEDIA_ERR_FULL;
    }

    subscriber.id =
        ((uint64_t)source->subscription_namespace <<
         TURBO_MEDIA_SUBSCRIPTION_NAMESPACE_SHIFT) |
        source->next_subscription_id++;
    subscriber.callback = callback;
    subscriber.user_data = user_data;
    source->subscribers[source->subscriber_count++] = subscriber;
    source->stats.subscriber_count = source->subscriber_count;

    if (subscription_id) *subscription_id = subscriber.id;

    if (replay_cached) {
        for (i = 0; i < source->gop_count; ++i) {
            size_t index = (source->gop_start + i) % source->config.gop_capacity;
            rc = turbo_media_deliver_frame(source, &source->gop_frames[index].frame,
                                           &source->subscribers[source->subscriber_count - 1]);
            if (rc != TURBO_MEDIA_OK) {
                source->subscriber_count--;
                source->stats.subscriber_count = source->subscriber_count;
                return rc;
            }
        }
    }

    return TURBO_MEDIA_OK;
}

int turbo_media_source_unsubscribe(turbo_media_source_t *source,
                                   uint64_t subscription_id) {
    size_t i;

    if (!source || subscription_id == 0) return TURBO_MEDIA_ERR_INVALID;

    for (i = 0; i < source->subscriber_count; ++i) {
        if (source->subscribers[i].id == subscription_id) {
            if (i + 1 < source->subscriber_count) {
                memmove(&source->subscribers[i],
                        &source->subscribers[i + 1],
                        (source->subscriber_count - i - 1) * sizeof(source->subscribers[0]));
            }
            source->subscriber_count--;
            source->stats.subscriber_count = source->subscriber_count;
            return TURBO_MEDIA_OK;
        }
    }

    return TURBO_MEDIA_ERR_NOT_FOUND;
}

int turbo_media_source_get_stats(const turbo_media_source_t *source,
                                 turbo_media_source_stats_t *stats) {
    if (!source || !stats) return TURBO_MEDIA_ERR_INVALID;

    *stats = source->stats;
    stats->subscriber_count = source->subscriber_count;
    stats->gop_cached_frames = source->gop_count;
    return TURBO_MEDIA_OK;
}

turbo_media_registry_t *turbo_media_registry_create(size_t max_sources) {
    turbo_media_registry_t *registry;
    int rc;

    max_sources = turbo_media_default_size(max_sources,
                                           TURBO_MEDIA_REGISTRY_DEFAULT_MAX_SOURCES);

    registry = (turbo_media_registry_t *)calloc(1, sizeof(*registry));
    if (!registry) return NULL;
    registry->next_subscription_namespace = 1;

    rc = hash_map_init_bytes(&registry->sources,
                             sizeof(turbo_media_source_key_t),
                             CMETA_ALIGNOF(turbo_media_source_key_t),
                             sizeof(turbo_media_source_t *),
                             CMETA_ALIGNOF(turbo_media_source_t *), max_sources,
                             hash_bytes, hash_key_equal, NULL);
    if (rc != STL_OK) {
        free(registry);
        return NULL;
    }

    rc = hash_map_reserve(&registry->sources, max_sources);
    if (rc != STL_OK) {
        hash_map_destroy(&registry->sources);
        free(registry);
        return NULL;
    }

    registry->max_sources = max_sources;
    return registry;
}

void turbo_media_registry_destroy(turbo_media_registry_t *registry) {
    size_t i;

    if (!registry) return;

    for (i = 0; i < hash_map_capacity(&registry->sources); ++i) {
        turbo_media_source_t *const *source =
            (turbo_media_source_t *const *)hash_map_value_at_const(
                &registry->sources, i);
        if (source) turbo_media_source_destroy(*source);
    }

    hash_map_destroy(&registry->sources);
    free(registry);
}

turbo_media_source_t *turbo_media_registry_find(const turbo_media_registry_t *registry,
                                                const turbo_media_source_key_t *key) {
    turbo_media_source_key_t normalized_key;
    turbo_media_source_t *const *source;

    if (!registry || turbo_media_normalize_key(&normalized_key, key) != TURBO_MEDIA_OK) {
        return NULL;
    }

    source = (turbo_media_source_t *const *)hash_map_get_const(&registry->sources,
                                                                     &normalized_key);
    return source ? *source : NULL;
}

int turbo_media_registry_get_or_create(turbo_media_registry_t *registry,
                                       const turbo_media_source_key_t *key,
                                       const turbo_media_source_config_t *config,
                                       turbo_media_source_t **source) {
    turbo_media_source_key_t normalized_key;
    turbo_media_source_t *found;
    turbo_media_source_t *const *existing;
    int rc;

    if (!registry || !source ||
        turbo_media_normalize_key(&normalized_key, key) != TURBO_MEDIA_OK) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    existing = (turbo_media_source_t *const *)hash_map_get_const(&registry->sources,
                                                                       &normalized_key);
    if (existing) {
        *source = *existing;
        return TURBO_MEDIA_OK;
    }

    if (hash_map_size(&registry->sources) >= registry->max_sources) {
        return TURBO_MEDIA_ERR_FULL;
    }

    found = turbo_media_source_create(&normalized_key, config);
    if (!found) return TURBO_MEDIA_ERR_NOMEM;
    if (registry->next_subscription_namespace == 0) {
        turbo_media_source_destroy(found);
        return TURBO_MEDIA_ERR_FULL;
    }
    /* A removed source must not leave a token that can match its replacement. */
    found->subscription_namespace = registry->next_subscription_namespace++;

    rc = hash_map_put(&registry->sources, &normalized_key, &found);
    if (rc != STL_OK) {
        turbo_media_source_destroy(found);
        return rc == STL_OUT_OF_MEMORY ? TURBO_MEDIA_ERR_NOMEM : TURBO_MEDIA_ERR_INVALID;
    }

    *source = found;
    return TURBO_MEDIA_OK;
}

int turbo_media_registry_remove(turbo_media_registry_t *registry,
                                const turbo_media_source_key_t *key) {
    turbo_media_source_key_t normalized_key;
    turbo_media_source_t *source = NULL;
    int rc;

    if (!registry || turbo_media_normalize_key(&normalized_key, key) != TURBO_MEDIA_OK) {
        return TURBO_MEDIA_ERR_INVALID;
    }

    rc = hash_map_remove(&registry->sources, &normalized_key, &source);
    if (rc == STL_NOT_FOUND) return TURBO_MEDIA_ERR_NOT_FOUND;
    if (rc != STL_OK) return TURBO_MEDIA_ERR_INVALID;

    turbo_media_source_destroy(source);
    return TURBO_MEDIA_OK;
}

size_t turbo_media_registry_count(const turbo_media_registry_t *registry) {
    return registry ? hash_map_size(&registry->sources) : 0;
}
