/**
 * HLS fMP4 streamer backed by Salts filesystem and CHTTP APIs.
 */
#include "turbo_streamer.h"

#ifdef TURBO_MEDIA_HAS_HLS

#include "hls_streamer_internal.h"
#include "hls-fmp4.h"
#include "hls-m3u8.h"
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
    HLS_FMP4_VERSION = 7,
    HLS_MIN_LIVE_SEGMENTS = 3,
    HLS_INIT_SEGMENT_CAPACITY = 1024 * 1024,
    HLS_PLAYLIST_BASE_CAPACITY = 1024,
    HLS_PLAYLIST_BYTES_PER_SEGMENT = 512,
    HLS_VIDEO_CONFIG_CAPACITY = 8 * 1024,
    HLS_DEFAULT_AUDIO_BITS_PER_SAMPLE = 16,
    HLS_PUBLIC_VIDEO_STREAM_ID = 0,
    HLS_PUBLIC_AUDIO_STREAM_ID = 1
};

static const char HLS_INIT_SEGMENT_NAME[] = "init.mp4";
static const char HLS_PLAYLIST_NAME[] = "playlist.m3u8";

typedef struct {
    tstr output_dir;
    tstr base_url;
    int segment_duration_ms;
    int playlist_size;
    turbo_hls_playlist_type_t playlist_type;

    hls_m3u8_t *m3u8;
    hls_fmp4_t *fmp4;
    int video_track;
    uint8_t video_object;
    int audio_track;
    int has_video;
    int has_audio;
    int connected;
    int init_segment_written;
    int sequence;
    int64_t last_pts_ms;
    int64_t last_dts_ms;

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
} hls_streamer_ctx_t;

static int hls_make_path(const hls_streamer_ctx_t *ctx, const char *name,
                         char path[SALTS_FS_MAX_PATH]) {
    return salts_fs_path_join(path, SALTS_FS_MAX_PATH, ctx->output_dir, name);
}

static tstr hls_make_url(const hls_streamer_ctx_t *ctx, const char *name) {
    tstr url;
    size_t length;

    if (!ctx->base_url) return tstr_dup(name);

    url = tstr_clone(ctx->base_url);
    if (!url) return NULL;

    length = tstr_len(url);
    if (length > 0 && url[length - 1] != '/') {
        url = tstr_cat(url, "/");
        if (!url) return NULL;
    }

    return tstr_cat(url, name);
}

static int hls_write_file(const hls_streamer_ctx_t *ctx, const char *name,
                          const void *data, size_t bytes, char path[SALTS_FS_MAX_PATH]) {
    salts_fs_buf_t buffer;

    if (hls_make_path(ctx, name, path) != 0) return -ENAMETOOLONG;
    buffer = salts_fs_buf_init((char *)data, bytes);
    return salts_fs_write_file(path, &buffer);
}

static int hls_upload_file(const hls_streamer_ctx_t *ctx, const char *name,
                           const char *path) {
    tstr url;
    int result;

    if (!ctx->http_client) return 0;
    if (!ctx->base_url) return -EINVAL;

    url = hls_make_url(ctx, name);
    if (!url) return -ENOMEM;

    result = turbo_streamer_chttp_post_file(
        ctx->http_client, ctx->http_tls_profile, url, path,
        ctx->http_timeout_ms);
    tstr_free(url);
    return result == 0 ? 0 : -EIO;
}

static int hls_render_playlist(hls_streamer_ctx_t *ctx, int eof, char **playlist,
                               size_t *size) {
    size_t capacity;
    char *buffer;

    if (!ctx->m3u8 || !playlist || !size) return -EINVAL;
    if ((size_t)ctx->playlist_size >
        (SIZE_MAX - HLS_PLAYLIST_BASE_CAPACITY) / HLS_PLAYLIST_BYTES_PER_SEGMENT) {
        return -EOVERFLOW;
    }

    capacity = HLS_PLAYLIST_BASE_CAPACITY +
               (size_t)ctx->playlist_size * HLS_PLAYLIST_BYTES_PER_SEGMENT;
    buffer = (char *)malloc(capacity);
    if (!buffer) return -ENOMEM;

    if (hls_m3u8_playlist(ctx->m3u8, eof, buffer, capacity) != 0) {
        free(buffer);
        return -ENOSPC;
    }

    *size = strlen(buffer);
    *playlist = buffer;
    return 0;
}

static int hls_publish_playlist(hls_streamer_ctx_t *ctx, int eof) {
    char path[SALTS_FS_MAX_PATH];
    char *playlist;
    size_t size;
    int result;

    result = hls_render_playlist(ctx, eof, &playlist, &size);
    if (result != 0) return result;

    result = hls_write_file(ctx, HLS_PLAYLIST_NAME, playlist, size, path);
    free(playlist);
    if (result != 0) return result;
    return hls_upload_file(ctx, HLS_PLAYLIST_NAME, path);
}

static int hls_on_segment(void *param, const void *data, size_t bytes, int64_t pts,
                          int64_t dts, int64_t duration) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)param;
    char name[64];
    char path[SALTS_FS_MAX_PATH];
    tstr uri;
    int written;
    int result;

    (void)dts;
    written = snprintf(name, sizeof(name), "segment_%d.m4s", ctx->sequence);
    if (written <= 0 || (size_t)written >= sizeof(name)) return -EOVERFLOW;

    result = hls_write_file(ctx, name, data, bytes, path);
    if (result != 0) return result;

    uri = hls_make_url(ctx, name);
    if (!uri) return -ENOMEM;
    result = hls_m3u8_add(ctx->m3u8, uri, pts, duration, 0);
    tstr_free(uri);
    if (result != 0) return result;

    result = hls_upload_file(ctx, name, path);
    if (result != 0) return result;
    result = hls_publish_playlist(ctx, 0);
    if (result != 0) return result;

    ++ctx->sequence;
    if (ctx->event_callback) {
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_SEGMENT_READY, path,
                            ctx->event_user_data);
    }
    return 0;
}

static int hls_codec_object(const char *codec_name, int video, uint8_t *object) {
    vstr name;

    if (!codec_name || !object) return -EINVAL;
    name = vstr_from_cstr(codec_name);

    if (video) {
        if (vstr_ieq(name, vstr_from_cstr("h264")) || vstr_ieq(name, vstr_from_cstr("avc")))
            *object = MOV_OBJECT_H264;
        else if (vstr_ieq(name, vstr_from_cstr("h265")) ||
                 vstr_ieq(name, vstr_from_cstr("hevc")))
            *object = MOV_OBJECT_H265;
        else if (vstr_ieq(name, vstr_from_cstr("h266")) ||
                 vstr_ieq(name, vstr_from_cstr("vvc")))
            *object = MOV_OBJECT_H266;
        else if (vstr_ieq(name, vstr_from_cstr("vp8")))
            *object = MOV_OBJECT_VP8;
        else if (vstr_ieq(name, vstr_from_cstr("vp9")))
            *object = MOV_OBJECT_VP9;
        else if (vstr_ieq(name, vstr_from_cstr("av1")))
            *object = MOV_OBJECT_AV1;
        else
            return -ENOTSUP;
    } else {
        if (vstr_ieq(name, vstr_from_cstr("aac")))
            *object = MOV_OBJECT_AAC;
        else if (vstr_ieq(name, vstr_from_cstr("opus")))
            *object = MOV_OBJECT_OPUS;
        else if (vstr_ieq(name, vstr_from_cstr("mp3")))
            *object = MOV_OBJECT_MP3;
        else if (vstr_ieq(name, vstr_from_cstr("pcma")) ||
                 vstr_ieq(name, vstr_from_cstr("g711a")))
            *object = MOV_OBJECT_G711a;
        else if (vstr_ieq(name, vstr_from_cstr("pcmu")) ||
                 vstr_ieq(name, vstr_from_cstr("g711u")))
            *object = MOV_OBJECT_G711u;
        else if (vstr_ieq(name, vstr_from_cstr("flac")))
            *object = MOV_OBJECT_FLAC;
        else
            return -ENOTSUP;
    }
    return 0;
}

static int hls_normalize_video_config(hls_streamer_ctx_t *ctx, uint8_t object,
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
            if (extra_data_size > output_capacity) return -EOVERFLOW;
            memcpy(output, extra_data, extra_data_size);
            return (int)extra_data_size;
    }
    return bytes > 0 ? bytes : -EINVAL;
}

static int hls_reserve_video_sample(hls_streamer_ctx_t *ctx, size_t packet_size) {
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

static int hls_convert_video_sample(hls_streamer_ctx_t *ctx, const uint8_t *input,
                                    size_t input_size, const uint8_t **output,
                                    size_t *output_size) {
    int bytes;
    int update = 0;
    int vcl = 0;
    int result;

    if (!ctx || !input || !output || !output_size) return -EINVAL;
    if (ctx->video_object != MOV_OBJECT_H264 && ctx->video_object != MOV_OBJECT_H265 &&
        ctx->video_object != MOV_OBJECT_H266) {
        *output = input;
        *output_size = input_size;
        return 0;
    }

    result = hls_reserve_video_sample(ctx, input_size);
    if (result != 0) return result;

    if (ctx->video_object == MOV_OBJECT_H264)
        bytes = h264_annexbtomp4(&ctx->video_config.avc, input, input_size,
                                 ctx->video_sample, ctx->video_sample_capacity, &vcl,
                                 &update);
    else if (ctx->video_object == MOV_OBJECT_H265)
        bytes = h265_annexbtomp4(&ctx->video_config.hevc, input, input_size,
                                 ctx->video_sample, ctx->video_sample_capacity, &vcl,
                                 &update);
    else
        bytes = h266_annexbtomp4(&ctx->video_config.vvc, input, input_size,
                                 ctx->video_sample, ctx->video_sample_capacity, &vcl,
                                 &update);

    if (bytes <= 0) return -EINVAL;
    if (update) return -EINVAL;
    *output = ctx->video_sample;
    *output_size = (size_t)bytes;
    return 0;
}

static void hls_streamer_destroy_impl(void *ctx_ptr) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    if (ctx->fmp4) hls_fmp4_destroy(ctx->fmp4);
    if (ctx->m3u8) hls_m3u8_destroy(ctx->m3u8);
    free(ctx->video_sample);
    tstr_free(ctx->output_dir);
    tstr_free(ctx->base_url);
    free(ctx);
}

static void *hls_streamer_create(const turbo_streamer_config_t *config) {
    hls_streamer_ctx_t *ctx;
    int live_segments;

    if (!config || !config->output_dir || config->segment_duration_ms <= 0 ||
        config->playlist_size < HLS_MIN_LIVE_SEGMENTS ||
        (config->http_client && !config->base_url)) {
        return NULL;
    }

    if (salts_fs_access(config->output_dir, SALTS_FS_ACCESS_EXISTS) != 0 &&
        salts_fs_mkdir(config->output_dir, 0755) != 0) {
        return NULL;
    }
    if (salts_fs_access(config->output_dir, SALTS_FS_ACCESS_WRITE) != 0) return NULL;

    ctx = (hls_streamer_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->output_dir = tstr_dup(config->output_dir);
    ctx->base_url = config->base_url ? tstr_dup(config->base_url) : NULL;
    ctx->segment_duration_ms = config->segment_duration_ms;
    ctx->playlist_size = config->playlist_size;
    ctx->playlist_type = TURBO_HLS_LIVE;
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
        hls_streamer_destroy_impl(ctx);
        return NULL;
    }

    live_segments = ctx->playlist_size;
    ctx->m3u8 = hls_m3u8_create(live_segments, HLS_FMP4_VERSION);
    ctx->fmp4 = hls_fmp4_create(ctx->segment_duration_ms, hls_on_segment, ctx);
    if (!ctx->m3u8 || !ctx->fmp4) {
        hls_streamer_destroy_impl(ctx);
        return NULL;
    }
    return ctx;
}

static int hls_write_init_segment(hls_streamer_ctx_t *ctx) {
    uint8_t *data;
    char path[SALTS_FS_MAX_PATH];
    tstr uri;
    int bytes;
    int result;

    data = (uint8_t *)malloc(HLS_INIT_SEGMENT_CAPACITY);
    if (!data) return -ENOMEM;
    bytes = hls_fmp4_init_segment(ctx->fmp4, data, HLS_INIT_SEGMENT_CAPACITY);
    if (bytes <= 0) {
        free(data);
        return -EIO;
    }

    result = hls_write_file(ctx, HLS_INIT_SEGMENT_NAME, data, (size_t)bytes, path);
    free(data);
    if (result != 0) return result;

    uri = hls_make_url(ctx, HLS_INIT_SEGMENT_NAME);
    if (!uri) return -ENOMEM;
    result = hls_m3u8_set_x_map(ctx->m3u8, uri);
    tstr_free(uri);
    if (result != 0) return result;

    result = hls_upload_file(ctx, HLS_INIT_SEGMENT_NAME, path);
    if (result != 0) return result;
    result = hls_publish_playlist(ctx, 0);
    if (result == 0) ctx->init_segment_written = 1;
    return result;
}

static int hls_streamer_connect_impl(void *ctx_ptr) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    int result;

    if (!ctx || (!ctx->has_video && !ctx->has_audio)) return -EINVAL;
    if (ctx->connected) return 0;

    result = hls_write_init_segment(ctx);
    if (result != 0) return result;
    ctx->connected = 1;
    if (ctx->event_callback) {
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_CONNECTED, NULL,
                            ctx->event_user_data);
    }
    return 0;
}

static int hls_streamer_disconnect_impl(void *ctx_ptr) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    int result = 0;
    int eof;

    if (!ctx) return -EINVAL;
    if (ctx->last_dts_ms >= 0) {
        result = hls_fmp4_input(ctx->fmp4, 0, NULL, 0, ctx->last_pts_ms,
                                ctx->last_dts_ms, 0);
    }
    eof = ctx->playlist_type != TURBO_HLS_LIVE;
    if (result == 0 && ctx->init_segment_written) result = hls_publish_playlist(ctx, eof);
    if (result != 0) return result;

    ctx->connected = 0;
    if (ctx->event_callback) {
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_DISCONNECTED, NULL,
                            ctx->event_user_data);
    }
    return 0;
}

static int hls_streamer_add_stream_impl(void *ctx_ptr,
                                        const turbo_stream_info_t *stream_info,
                                        int *stream_id) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    uint8_t object;
    uint8_t normalized_config[HLS_VIDEO_CONFIG_CAPACITY];
    const uint8_t *extra_data;
    size_t extra_data_size;
    int track;

    if (!ctx || !stream_info || !stream_id || ctx->connected || !stream_info->codec_name)
        return -EINVAL;

    if (stream_info->type == TURBO_CODEC_TYPE_VIDEO) {
        if (ctx->has_video || stream_info->width <= 0 || stream_info->height <= 0 ||
            hls_codec_object(stream_info->codec_name, 1, &object) != 0)
            return -EINVAL;
        extra_data = stream_info->extradata;
        extra_data_size = stream_info->extradata_size;
        if (object == MOV_OBJECT_H264 || object == MOV_OBJECT_H265 ||
            object == MOV_OBJECT_H266) {
            int normalized_size = hls_normalize_video_config(
                ctx, object, stream_info->extradata, stream_info->extradata_size,
                normalized_config, sizeof(normalized_config));
            if (normalized_size < 0) return normalized_size;
            extra_data = normalized_config;
            extra_data_size = (size_t)normalized_size;
        }
        track = hls_fmp4_add_video(ctx->fmp4, object, stream_info->width,
                                   stream_info->height, extra_data, extra_data_size);
        if (track < 0) return track;
        ctx->video_track = track;
        ctx->video_object = object;
        ctx->has_video = 1;
        *stream_id = HLS_PUBLIC_VIDEO_STREAM_ID;
        return 0;
    }

    if (stream_info->type == TURBO_CODEC_TYPE_AUDIO) {
        if (ctx->has_audio || stream_info->sample_rate <= 0 || stream_info->channels <= 0 ||
            hls_codec_object(stream_info->codec_name, 0, &object) != 0)
            return -EINVAL;
        track = hls_fmp4_add_audio(ctx->fmp4, object, stream_info->channels,
                                   HLS_DEFAULT_AUDIO_BITS_PER_SAMPLE,
                                   stream_info->sample_rate, stream_info->extradata,
                                   stream_info->extradata_size);
        if (track < 0) return track;
        ctx->audio_track = track;
        ctx->has_audio = 1;
        *stream_id = HLS_PUBLIC_AUDIO_STREAM_ID;
        return 0;
    }
    return -ENOTSUP;
}

static int hls_streamer_write_packet_impl(void *ctx_ptr,
                                          const turbo_muxer_packet_t *packet) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    int track;
    int flags;
    int64_t pts_ms;
    int64_t dts_ms;
    const uint8_t *sample_data;
    size_t sample_size;
    int result;

    if (!ctx || !packet || !ctx->connected || !packet->data || packet->size == 0)
        return -EINVAL;

    sample_data = packet->data;
    sample_size = packet->size;
    if (packet->stream_id == HLS_PUBLIC_VIDEO_STREAM_ID && ctx->has_video) {
        track = ctx->video_track;
        result = hls_convert_video_sample(ctx, packet->data, packet->size,
                                          &sample_data, &sample_size);
        if (result != 0) return result;
    } else if (packet->stream_id == HLS_PUBLIC_AUDIO_STREAM_ID && ctx->has_audio)
        track = ctx->audio_track;
    else
        return -EINVAL;

    pts_ms = packet->pts / 1000;
    dts_ms = packet->dts / 1000;
    flags = packet->is_keyframe ? MOV_AV_FLAG_KEYFREAME : 0;
    result = hls_fmp4_input(ctx->fmp4, track, sample_data, sample_size, pts_ms,
                            dts_ms, flags);
    if (result != 0) return result;

    ctx->last_pts_ms = pts_ms;
    ctx->last_dts_ms = dts_ms;
    ctx->stats.bytes_sent += packet->size;
    ++ctx->stats.packets_sent;
    return 0;
}

static int hls_streamer_get_stats_impl(void *ctx_ptr, turbo_streamer_stats_t *stats) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    if (!ctx || !stats) return -EINVAL;
    *stats = ctx->stats;
    return 0;
}

static void hls_streamer_set_event_callback_impl(void *ctx_ptr,
                                                 turbo_streamer_event_cb callback,
                                                 void *user_data) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    ctx->event_callback = callback;
    ctx->event_user_data = user_data;
}

int turbo_hls_streamer_get_playlist(void *ctx_ptr, char **playlist, size_t *size) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    int eof;
    if (!ctx) return -EINVAL;
    eof = !ctx->connected && ctx->playlist_type != TURBO_HLS_LIVE;
    return hls_render_playlist(ctx, eof, playlist, size);
}

void turbo_hls_streamer_set_playlist_type(void *ctx_ptr,
                                          turbo_hls_playlist_type_t type) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    hls_m3u8_t *replacement;
    int live_segments;

    if (!ctx || ctx->connected || ctx->init_segment_written || type < TURBO_HLS_VOD ||
        type > TURBO_HLS_EVENT)
        return;

    live_segments = type == TURBO_HLS_LIVE ? ctx->playlist_size : 0;
    replacement = hls_m3u8_create(live_segments, HLS_FMP4_VERSION);
    if (!replacement) return;
    if (hls_m3u8_set_playlist_type(replacement, type == TURBO_HLS_EVENT ? "EVENT" :
                                                    type == TURBO_HLS_VOD ? "VOD" : NULL) != 0) {
        hls_m3u8_destroy(replacement);
        return;
    }

    hls_m3u8_destroy(ctx->m3u8);
    ctx->m3u8 = replacement;
    ctx->playlist_type = type;
}

const turbo_streamer_ops_t turbo_hls_streamer_ops = {
    .name = "hls",
    .protocol = TURBO_STREAMER_HLS,
    .create = hls_streamer_create,
    .destroy = hls_streamer_destroy_impl,
    .connect = hls_streamer_connect_impl,
    .disconnect = hls_streamer_disconnect_impl,
    .add_stream = hls_streamer_add_stream_impl,
    .write_packet = hls_streamer_write_packet_impl,
    .read_packet = NULL,
    .get_stats = hls_streamer_get_stats_impl,
    .set_event_callback = hls_streamer_set_event_callback_impl
};

#endif /* TURBO_MEDIA_HAS_HLS */
