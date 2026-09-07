/**
 * MPEG-DASH streamer backed by the internal fMP4 MPD writer.
 */
#include "turbo_streamer.h"

#ifdef TURBO_MEDIA_HAS_DASH

#include "dash-mpd.h"
#include "dash-proto.h"
#include "chttp_upload.h"
#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
#include "mpeg4-vvc.h"
#include "mov-format.h"
#include "salts_fs.h"
#include "salts_str.h"
#include "salts_vstr.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    DASH_VIDEO_CONFIG_CAPACITY = 8 * 1024,
    DASH_MANIFEST_BASE_CAPACITY = 4 * 1024,
    DASH_MANIFEST_BYTES_PER_SEGMENT = 128,
    DASH_MAX_ADAPTATION_SETS = 2,
    DASH_DEFAULT_AUDIO_BITS_PER_SAMPLE = 16,
    DASH_MAX_PLAYLIST_SIZE = 10000
};

static const char DASH_MANIFEST_NAME[] = "manifest.mpd";
static const char DASH_VIDEO_PREFIX[] = "video";
static const char DASH_AUDIO_PREFIX[] = "audio";

typedef struct {
    tstr output_dir;
    tstr base_url;
    dash_mpd_t *mpd;
    int playlist_size;
    int video_track;
    int audio_track;
    uint8_t video_object;
    int connected;
    int manifest_dirty;
    int64_t last_pts_ms;
    int64_t last_dts_ms;
    int64_t last_duration_ms;
    char init_names[DASH_MAX_ADAPTATION_SETS][SALTS_FS_MAX_PATH];
    int init_count;

    union {
        struct mpeg4_avc_t avc;
        struct mpeg4_hevc_t hevc;
        struct mpeg4_vvc_t vvc;
    } video_config;
    uint8_t *video_sample;
    size_t video_sample_capacity;

    chttp_client *http_client;
    const chttp_tls_profile *http_tls_profile;
    uint32_t http_timeout_ms;
    turbo_streamer_event_cb event_callback;
    void *event_user_data;
    turbo_streamer_stats_t stats;
} dash_streamer_ctx_t;

static int dash_make_path(const dash_streamer_ctx_t *ctx, const char *name,
                          char path[SALTS_FS_MAX_PATH]) {
    return salts_fs_path_join(path, SALTS_FS_MAX_PATH, ctx->output_dir, name);
}

static tstr dash_make_url(const dash_streamer_ctx_t *ctx, const char *name) {
    tstr url;

    if (!ctx->base_url) return tstr_dup(name);
    url = tstr_clone(ctx->base_url);
    if (!url) return NULL;
    return tstr_cat(url, name);
}

static int dash_write_file(const dash_streamer_ctx_t *ctx, const char *name,
                           const void *data, size_t bytes,
                           char path[SALTS_FS_MAX_PATH]) {
    salts_fs_buf_t buffer;

    if (dash_make_path(ctx, name, path) != 0) return -ENAMETOOLONG;
    buffer = salts_fs_buf_init((char *)data, bytes);
    return salts_fs_write_file(path, &buffer);
}

static int dash_upload_file(const dash_streamer_ctx_t *ctx, const char *name,
                            const char *path) {
    tstr url;
    int result;

    if (!ctx->http_client) return 0;
    if (!ctx->base_url) return -EINVAL;

    url = dash_make_url(ctx, name);
    if (!url) return -ENOMEM;
    result = turbo_streamer_chttp_post_file(
        ctx->http_client, ctx->http_tls_profile, url, path,
        ctx->http_timeout_ms);
    tstr_free(url);
    return result == 0 ? 0 : -EIO;
}

static int dash_render_manifest(dash_streamer_ctx_t *ctx, char **manifest,
                                size_t *size) {
    size_t segment_capacity;
    size_t capacity;
    size_t rendered;
    char *buffer;

    if (!ctx || !ctx->mpd || !manifest || !size) return -EINVAL;
    if ((size_t)ctx->playlist_size >
        (SIZE_MAX / DASH_MAX_ADAPTATION_SETS) / DASH_MANIFEST_BYTES_PER_SEGMENT)
        return -EOVERFLOW;

    segment_capacity = (size_t)ctx->playlist_size * DASH_MAX_ADAPTATION_SETS *
                       DASH_MANIFEST_BYTES_PER_SEGMENT;
    if (segment_capacity > SIZE_MAX - DASH_MANIFEST_BASE_CAPACITY)
        return -EOVERFLOW;
    capacity = DASH_MANIFEST_BASE_CAPACITY + segment_capacity;
    buffer = (char *)malloc(capacity);
    if (!buffer) return -ENOMEM;

    rendered = dash_mpd_playlist(ctx->mpd, buffer, capacity);
    if (rendered == 0) {
        free(buffer);
        return -ENOSPC;
    }
    *manifest = buffer;
    *size = rendered;
    return 0;
}

static int dash_publish_manifest(dash_streamer_ctx_t *ctx) {
    char path[SALTS_FS_MAX_PATH];
    char *manifest;
    size_t size;
    int result;

    result = dash_render_manifest(ctx, &manifest, &size);
    if (result != 0) return result;
    result = dash_write_file(ctx, DASH_MANIFEST_NAME, manifest, size, path);
    free(manifest);
    if (result != 0) return result;
    result = dash_upload_file(ctx, DASH_MANIFEST_NAME, path);
    if (result == 0) ctx->manifest_dirty = 0;
    return result;
}

static int dash_remember_init(dash_streamer_ctx_t *ctx, const char *name) {
    int written;

    if (ctx->init_count >= DASH_MAX_ADAPTATION_SETS) return -EOVERFLOW;
    written = snprintf(ctx->init_names[ctx->init_count], SALTS_FS_MAX_PATH, "%s", name);
    if (written <= 0 || written >= SALTS_FS_MAX_PATH) return -ENAMETOOLONG;
    ++ctx->init_count;
    return 0;
}

static int dash_on_segment(void *param, int adaptation, const void *data, size_t bytes,
                           int64_t pts, int64_t dts, int64_t duration,
                           const char *name) {
    dash_streamer_ctx_t *ctx = (dash_streamer_ctx_t *)param;
    char path[SALTS_FS_MAX_PATH];
    int result;

    (void)adaptation;
    (void)pts;
    (void)dts;
    if (!ctx || !data || bytes == 0 || !name) return -EINVAL;

    result = dash_write_file(ctx, name, data, bytes, path);
    if (result != 0) return result;
    if (duration == 0 && !ctx->connected) return dash_remember_init(ctx, name);

    result = dash_upload_file(ctx, name, path);
    if (result != 0) return result;
    ctx->manifest_dirty = 1;
    if (ctx->event_callback) {
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_SEGMENT_READY, path,
                            ctx->event_user_data);
    }
    return 0;
}

static int dash_codec_object(const char *codec_name, int video, uint8_t *object) {
    vstr name;

    if (!codec_name || !object) return -EINVAL;
    name = vstr_from_cstr(codec_name);
    if (video) {
        if (vstr_ieq(name, vstr_from_cstr("h264")) ||
            vstr_ieq(name, vstr_from_cstr("avc")))
            *object = MOV_OBJECT_H264;
        else if (vstr_ieq(name, vstr_from_cstr("h265")) ||
                 vstr_ieq(name, vstr_from_cstr("hevc")))
            *object = MOV_OBJECT_H265;
        else if (vstr_ieq(name, vstr_from_cstr("h266")) ||
                 vstr_ieq(name, vstr_from_cstr("vvc")))
            *object = MOV_OBJECT_H266;
        else
            return -ENOTSUP;
    } else if (vstr_ieq(name, vstr_from_cstr("aac"))) {
        *object = MOV_OBJECT_AAC;
    } else {
        return -ENOTSUP;
    }
    return 0;
}

static int dash_normalize_video_config(dash_streamer_ctx_t *ctx, uint8_t object,
                                       const uint8_t *extra_data,
                                       size_t extra_data_size, uint8_t *output,
                                       size_t output_capacity) {
    int bytes;

    if (!ctx || !extra_data || extra_data_size == 0 || !output) return -EINVAL;
    switch (object) {
        case MOV_OBJECT_H264:
            memset(&ctx->video_config.avc, 0, sizeof(ctx->video_config.avc));
            if (mpeg4_avc_decoder_configuration_record_load(
                    extra_data, extra_data_size, &ctx->video_config.avc) <= 0)
                return -EINVAL;
            bytes = mpeg4_avc_decoder_configuration_record_save(
                &ctx->video_config.avc, output, output_capacity);
            break;
        case MOV_OBJECT_H265:
            memset(&ctx->video_config.hevc, 0, sizeof(ctx->video_config.hevc));
            if (mpeg4_hevc_decoder_configuration_record_load(
                    extra_data, extra_data_size, &ctx->video_config.hevc) <= 0)
                return -EINVAL;
            bytes = mpeg4_hevc_decoder_configuration_record_save(
                &ctx->video_config.hevc, output, output_capacity);
            break;
        case MOV_OBJECT_H266:
            memset(&ctx->video_config.vvc, 0, sizeof(ctx->video_config.vvc));
            if (mpeg4_vvc_decoder_configuration_record_load(
                    extra_data, extra_data_size, &ctx->video_config.vvc) <= 0) {
                int update = 0;
                memset(&ctx->video_config.vvc, 0, sizeof(ctx->video_config.vvc));
                (void)h266_annexbtomp4(&ctx->video_config.vvc, extra_data,
                                       extra_data_size, output, output_capacity, NULL,
                                       &update);
                if (!update) return -EINVAL;
            }
            bytes = mpeg4_vvc_decoder_configuration_record_save(
                &ctx->video_config.vvc, output, output_capacity);
            break;
        default:
            return -ENOTSUP;
    }
    return bytes > 0 ? bytes : -EINVAL;
}

static int dash_reserve_video_sample(dash_streamer_ctx_t *ctx, size_t packet_size) {
    uint8_t *replacement;
    size_t capacity;

    if (packet_size > SIZE_MAX / 2) return -EOVERFLOW;
    capacity = packet_size * 2;
    if (capacity <= ctx->video_sample_capacity) return 0;
    replacement = (uint8_t *)realloc(ctx->video_sample, capacity);
    if (!replacement) return -ENOMEM;
    ctx->video_sample = replacement;
    ctx->video_sample_capacity = capacity;
    return 0;
}

static int dash_convert_video_sample(dash_streamer_ctx_t *ctx, const uint8_t *input,
                                     size_t input_size, const uint8_t **output,
                                     size_t *output_size) {
    int bytes;
    int update = 0;
    int vcl = 0;
    int result;

    if (!ctx || !input || !output || !output_size) return -EINVAL;
    result = dash_reserve_video_sample(ctx, input_size);
    if (result != 0) return result;

    if (ctx->video_object == MOV_OBJECT_H264)
        bytes = h264_annexbtomp4(&ctx->video_config.avc, input, input_size,
                                 ctx->video_sample, ctx->video_sample_capacity, &vcl,
                                 &update);
    else if (ctx->video_object == MOV_OBJECT_H265)
        bytes = h265_annexbtomp4(&ctx->video_config.hevc, input, input_size,
                                 ctx->video_sample, ctx->video_sample_capacity, &vcl,
                                 &update);
    else if (ctx->video_object == MOV_OBJECT_H266)
        bytes = h266_annexbtomp4(&ctx->video_config.vvc, input, input_size,
                                 ctx->video_sample, ctx->video_sample_capacity, &vcl,
                                 &update);
    else
        return -ENOTSUP;

    if (bytes <= 0 || update) return -EINVAL;
    *output = ctx->video_sample;
    *output_size = (size_t)bytes;
    return 0;
}

static int dash_streamer_destroy_impl(void *ctx_ptr) {
    dash_streamer_ctx_t *ctx = (dash_streamer_ctx_t *)ctx_ptr;
    if (!ctx) return 0;
    if (ctx->mpd) dash_mpd_destroy(ctx->mpd);
    free(ctx->video_sample);
    tstr_free(ctx->output_dir);
    tstr_free(ctx->base_url);
    free(ctx);
    return 0;
}

static void *dash_streamer_create(const turbo_streamer_config_t *config) {
    dash_streamer_ctx_t *ctx;
    size_t base_url_length;

    if (!config || config->protocol != TURBO_STREAMER_DASH || !config->output_dir ||
        config->segment_duration_ms <= 0 || config->playlist_size <= 0 ||
        config->playlist_size > DASH_MAX_PLAYLIST_SIZE ||
        (config->http_client && !config->base_url) ||
        (config->base_url && strpbrk(config->base_url, "&<>\"'")))
        return NULL;

    if (salts_fs_access(config->output_dir, SALTS_FS_ACCESS_EXISTS) != 0 &&
        salts_fs_mkdir(config->output_dir, 0755) != 0)
        return NULL;
    if (salts_fs_access(config->output_dir, SALTS_FS_ACCESS_WRITE) != 0) return NULL;

    ctx = (dash_streamer_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->output_dir = tstr_dup(config->output_dir);
    ctx->base_url = config->base_url ? tstr_dup(config->base_url) : NULL;
    ctx->playlist_size = config->playlist_size;
    ctx->video_track = -1;
    ctx->audio_track = -1;
    ctx->last_pts_ms = -1;
    ctx->last_dts_ms = -1;
    ctx->http_client = config->http_client;
    ctx->http_tls_profile = config->http_tls_profile;
    ctx->http_timeout_ms = config->timeout_ms > 0
                               ? (uint32_t)config->timeout_ms
                               : 0u;
    ctx->stats.uptime_ms = (int64_t)time(NULL) * 1000;
    if (!ctx->output_dir || (config->base_url && !ctx->base_url)) {
        dash_streamer_destroy_impl(ctx);
        return NULL;
    }

    if (ctx->base_url) {
        base_url_length = tstr_len(ctx->base_url);
        if (base_url_length == 0 || ctx->base_url[base_url_length - 1] != '/') {
            ctx->base_url = tstr_cat(ctx->base_url, "/");
            if (!ctx->base_url) {
                dash_streamer_destroy_impl(ctx);
                return NULL;
            }
        }
    }

    ctx->mpd = dash_mpd_create(DASH_DYNAMIC, config->segment_duration_ms,
                               config->playlist_size, ctx->base_url, dash_on_segment,
                               ctx);
    if (!ctx->mpd) {
        dash_streamer_destroy_impl(ctx);
        return NULL;
    }
    return ctx;
}

static int dash_streamer_connect_impl(void *ctx_ptr) {
    dash_streamer_ctx_t *ctx = (dash_streamer_ctx_t *)ctx_ptr;
    char path[SALTS_FS_MAX_PATH];
    int index;
    int result;

    if (!ctx || (ctx->video_track < 0 && ctx->audio_track < 0)) return -EINVAL;
    if (ctx->connected) return 0;
    for (index = 0; index < ctx->init_count; ++index) {
        result = dash_make_path(ctx, ctx->init_names[index], path);
        if (result != 0) return -ENAMETOOLONG;
        result = dash_upload_file(ctx, ctx->init_names[index], path);
        if (result != 0) return result;
    }
    result = dash_publish_manifest(ctx);
    if (result != 0) return result;
    ctx->connected = 1;
    if (ctx->event_callback)
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_CONNECTED, NULL,
                            ctx->event_user_data);
    return 0;
}

static int dash_streamer_disconnect_impl(void *ctx_ptr) {
    dash_streamer_ctx_t *ctx = (dash_streamer_ctx_t *)ctx_ptr;
    int track;
    int result = 0;

    if (!ctx) return -EINVAL;
    if (!ctx->connected) return 0;
    track = ctx->video_track >= 0 ? ctx->video_track : ctx->audio_track;
    if (ctx->last_dts_ms >= 0)
        result = dash_mpd_input(ctx->mpd, track, NULL, 0,
                                ctx->last_pts_ms + ctx->last_duration_ms,
                                ctx->last_dts_ms + ctx->last_duration_ms, 0);
    if (result == 0 && ctx->manifest_dirty) result = dash_publish_manifest(ctx);
    if (result != 0) return result;

    ctx->connected = 0;
    if (ctx->event_callback)
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_DISCONNECTED, NULL,
                            ctx->event_user_data);
    return 0;
}

static int dash_streamer_add_stream_impl(void *ctx_ptr,
                                         const turbo_stream_info_t *stream_info,
                                         int *stream_id) {
    dash_streamer_ctx_t *ctx = (dash_streamer_ctx_t *)ctx_ptr;
    uint8_t normalized_config[DASH_VIDEO_CONFIG_CAPACITY];
    uint8_t object;
    int normalized_size;
    int track;

    if (!ctx || !stream_info || !stream_id || ctx->connected ||
        !stream_info->codec_name || !stream_info->extradata ||
        stream_info->extradata_size == 0)
        return -EINVAL;

    if (stream_info->type == TURBO_CODEC_TYPE_VIDEO) {
        if (ctx->video_track >= 0 || stream_info->width <= 0 ||
            stream_info->height <= 0 || stream_info->framerate <= 0 ||
            dash_codec_object(stream_info->codec_name, 1, &object) != 0)
            return -EINVAL;
        normalized_size = dash_normalize_video_config(
            ctx, object, stream_info->extradata, stream_info->extradata_size,
            normalized_config, sizeof(normalized_config));
        if (normalized_size < 0) return normalized_size;
        track = dash_mpd_add_video_adaptation_set(
            ctx->mpd, DASH_VIDEO_PREFIX, object, stream_info->width,
            stream_info->height, stream_info->framerate, normalized_config,
            (size_t)normalized_size);
        if (track < 0) return track;
        ctx->video_track = track;
        ctx->video_object = object;
        *stream_id = track;
        return 0;
    }

    if (stream_info->type == TURBO_CODEC_TYPE_AUDIO) {
        if (ctx->audio_track >= 0 || stream_info->sample_rate <= 0 ||
            stream_info->channels <= 0 ||
            dash_codec_object(stream_info->codec_name, 0, &object) != 0)
            return -EINVAL;
        track = dash_mpd_add_audio_adaptation_set(
            ctx->mpd, DASH_AUDIO_PREFIX, object, stream_info->channels,
            DASH_DEFAULT_AUDIO_BITS_PER_SAMPLE, stream_info->sample_rate,
            stream_info->extradata, stream_info->extradata_size);
        if (track < 0) return track;
        ctx->audio_track = track;
        *stream_id = track;
        return 0;
    }
    return -ENOTSUP;
}

static int dash_streamer_write_packet_impl(void *ctx_ptr,
                                           const turbo_muxer_packet_t *packet) {
    dash_streamer_ctx_t *ctx = (dash_streamer_ctx_t *)ctx_ptr;
    const uint8_t *sample_data;
    size_t sample_size;
    int flags;
    int result;

    if (!ctx || !packet || !ctx->connected || !packet->data || packet->size == 0)
        return -EINVAL;
    sample_data = packet->data;
    sample_size = packet->size;
    if (packet->stream_id == ctx->video_track) {
        result = dash_convert_video_sample(ctx, packet->data, packet->size,
                                           &sample_data, &sample_size);
        if (result != 0) return result;
    } else if (packet->stream_id != ctx->audio_track) {
        return -EINVAL;
    }

    flags = packet->is_keyframe ? MOV_AV_FLAG_KEYFREAME : 0;
    result = dash_mpd_input(ctx->mpd, packet->stream_id, sample_data, sample_size,
                            packet->pts / 1000, packet->dts / 1000, flags);
    if (result != 0) return result;
    if (ctx->manifest_dirty) {
        result = dash_publish_manifest(ctx);
        if (result != 0) return result;
    }

    ctx->last_pts_ms = packet->pts / 1000;
    ctx->last_dts_ms = packet->dts / 1000;
    ctx->last_duration_ms = packet->duration / 1000;
    ctx->stats.bytes_sent += packet->size;
    ++ctx->stats.packets_sent;
    return 0;
}

static int dash_streamer_get_stats_impl(void *ctx_ptr,
                                        turbo_streamer_stats_t *stats) {
    dash_streamer_ctx_t *ctx = (dash_streamer_ctx_t *)ctx_ptr;
    if (!ctx || !stats) return -EINVAL;
    *stats = ctx->stats;
    return 0;
}

static void dash_streamer_set_event_callback_impl(
    void *ctx_ptr, turbo_streamer_event_cb callback, void *user_data) {
    dash_streamer_ctx_t *ctx = (dash_streamer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    ctx->event_callback = callback;
    ctx->event_user_data = user_data;
}

const turbo_streamer_ops_t turbo_dash_streamer_ops = {
    .name = "dash",
    .protocol = TURBO_STREAMER_DASH,
    .create = dash_streamer_create,
    .destroy = dash_streamer_destroy_impl,
    .connect = dash_streamer_connect_impl,
    .disconnect = dash_streamer_disconnect_impl,
    .add_stream = dash_streamer_add_stream_impl,
    .write_packet = dash_streamer_write_packet_impl,
    .read_packet = NULL,
    .get_stats = dash_streamer_get_stats_impl,
    .set_event_callback = dash_streamer_set_event_callback_impl
};

#endif
