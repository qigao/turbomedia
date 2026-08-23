#include "turbo_demuxer.h"

#ifdef TURBO_MEDIA_HAS_MKV

#include "container_io.h"
#include "stl_status.h"
#include "mkv-buffer.h"
#include "mkv-format.h"
#include "mkv-reader.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <turbo_error.h>
#include <turbostl/vec.h>

enum { MKV_PROBE_HEADER_BYTES = 4096 };

typedef struct {
    uint32_t track;
    turbo_stream_info_t info;
    uint8_t *extra_data;
} mkv_demuxer_stream_t;

typedef struct {
    turbo_container_io_t io;
    mkv_reader_t *reader;
    vec_t streams;
    turbo_container_metadata_t metadata;
    int opened;
    int error;
} mkv_demuxer_ctx_t;

typedef struct {
    mkv_demuxer_ctx_t *ctx;
    turbo_demuxer_packet_t *packet;
} mkv_packet_read_t;

static const struct mkv_buffer_t s_mkv_io = {
    turbo_container_io_read,
    turbo_container_io_write,
    turbo_container_io_seek,
    turbo_container_io_tell,
};

static const char *mkv_video_codec_name(enum mkv_codec_t codec) {
    switch (codec) {
        case MKV_CODEC_VIDEO_H264: return "h264";
        case MKV_CODEC_VIDEO_H265: return "h265";
        case MKV_CODEC_VIDEO_H266: return "h266";
        case MKV_CODEC_VIDEO_VP8: return "vp8";
        case MKV_CODEC_VIDEO_VP9: return "vp9";
        case MKV_CODEC_VIDEO_AV1: return "av1";
        case MKV_CODEC_VIDEO_MPEG4: return "mpeg4";
        case MKV_CODEC_VIDEO_MPEG1: return "mpeg1video";
        case MKV_CODEC_VIDEO_MPEG2: return "mpeg2video";
        default: return "unknown";
    }
}

static const char *mkv_audio_codec_name(enum mkv_codec_t codec) {
    switch (codec) {
        case MKV_CODEC_AUDIO_AAC: return "aac";
        case MKV_CODEC_AUDIO_OPUS: return "opus";
        case MKV_CODEC_AUDIO_MP3: return "mp3";
        case MKV_CODEC_AUDIO_FLAC: return "flac";
        case MKV_CODEC_AUDIO_AC3: return "ac3";
        case MKV_CODEC_AUDIO_EAC3: return "eac3";
        case MKV_CODEC_AUDIO_VORBIS: return "vorbis";
        default: return "unknown";
    }
}

static int mkv_add_stream(mkv_demuxer_ctx_t *ctx, uint32_t track,
                          turbo_codec_class_t type, const char *codec_name,
                          int width, int height, int channels, int sample_rate,
                          const void *extra, size_t bytes) {
    mkv_demuxer_stream_t stream;
    int result;
    if (!ctx || ctx->error != TURBO_OK) return ctx ? ctx->error : TURBO_EINVAL;
    memset(&stream, 0, sizeof(stream));
    stream.track = track;
    stream.info.stream_id = (int)track;
    stream.info.type = type;
    stream.info.codec_name = codec_name;
    stream.info.width = width;
    stream.info.height = height;
    stream.info.channels = channels;
    stream.info.sample_rate = sample_rate;
    if (bytes > 0) {
        if (!extra) return ctx->error = TURBO_EPROTO;
        stream.extra_data = (uint8_t *)malloc(bytes);
        if (!stream.extra_data) return ctx->error = TURBO_ENOMEM;
        memcpy(stream.extra_data, extra, bytes);
        stream.info.extradata = stream.extra_data;
        stream.info.extradata_size = bytes;
    }
    result = turbo_media_stl_status_to_error(vec_push(&ctx->streams, &stream));
    if (result != TURBO_OK) {
        free(stream.extra_data);
        ctx->error = result;
    }
    return result;
}

static void mkv_on_video(void *param, uint32_t track, enum mkv_codec_t codec,
                         int width, int height, const void *extra, size_t bytes) {
    mkv_demuxer_ctx_t *ctx = (mkv_demuxer_ctx_t *)param;
    (void)mkv_add_stream(ctx, track, TURBO_CODEC_TYPE_VIDEO,
                         mkv_video_codec_name(codec), width, height, 0, 0, extra,
                         bytes);
}

static void mkv_on_audio(void *param, uint32_t track, enum mkv_codec_t codec,
                         int channels, int bits_per_sample, int sample_rate,
                         const void *extra, size_t bytes) {
    mkv_demuxer_ctx_t *ctx = (mkv_demuxer_ctx_t *)param;
    (void)bits_per_sample;
    (void)mkv_add_stream(ctx, track, TURBO_CODEC_TYPE_AUDIO,
                         mkv_audio_codec_name(codec), 0, 0, channels, sample_rate,
                         extra, bytes);
}

static void mkv_on_subtitle(void *param, uint32_t track, enum mkv_codec_t codec,
                            const void *extra, size_t bytes) {
    mkv_demuxer_ctx_t *ctx = (mkv_demuxer_ctx_t *)param;
    (void)track;
    (void)codec;
    (void)extra;
    (void)bytes;
    if (ctx && ctx->error == TURBO_OK) ctx->error = TURBO_ENOTSUP;
}

static int mkv_stream_index(const mkv_demuxer_ctx_t *ctx, uint32_t track) {
    for (size_t i = 0; i < vec_size(&ctx->streams); ++i) {
        const mkv_demuxer_stream_t *stream =
            (const mkv_demuxer_stream_t *)vec_at_const(&ctx->streams, i);
        if (stream && stream->track == track) return (int)i;
    }
    return -1;
}

static void *mkv_on_packet(void *param, uint32_t track, size_t bytes,
                           int64_t pts, int64_t dts, int flags) {
    mkv_packet_read_t *read = (mkv_packet_read_t *)param;
    turbo_demuxer_packet_t *packet;
    int stream_index;
    if (!read || !read->ctx || !read->packet || bytes == 0) return NULL;
    stream_index = mkv_stream_index(read->ctx, track);
    if (stream_index < 0) return NULL;
    packet = read->packet;
    packet->data = (uint8_t *)malloc(bytes);
    if (!packet->data) return NULL;
    packet->size = bytes;
    packet->stream_index = stream_index;
    packet->pts = pts;
    packet->dts = dts;
    packet->duration = 0;
    packet->is_keyframe = (flags & MKV_FLAGS_KEYFRAME) != 0;
    return packet->data;
}

static int mkv_is_ebml(const uint8_t *data, size_t size) {
    static const uint8_t signature[] = {0x1a, 0x45, 0xdf, 0xa3};
    return data && size >= sizeof(signature) &&
           memcmp(data, signature, sizeof(signature)) == 0;
}

static int mkv_header_contains(const uint8_t *data, size_t size,
                               const char *text) {
    size_t text_size = strlen(text);
    size_t limit = size < MKV_PROBE_HEADER_BYTES ? size : MKV_PROBE_HEADER_BYTES;
    if (text_size > limit) return 0;
    for (size_t i = 0; i <= limit - text_size; ++i) {
        if (memcmp(data + i, text, text_size) == 0) return 1;
    }
    return 0;
}

static int mkv_probe_impl(const uint8_t *data, size_t size) {
    if (!mkv_is_ebml(data, size)) return 0;
    return mkv_header_contains(data, size, "webm") ? 0 : 100;
}

static int webm_probe_impl(const uint8_t *data, size_t size) {
    if (!mkv_is_ebml(data, size)) return 0;
    return mkv_header_contains(data, size, "webm") ? 100 : 0;
}

static void *mkv_demuxer_create_impl(const turbo_demuxer_config_t *config) {
    mkv_demuxer_ctx_t *ctx;
    int result;
    if (!config || (!!config->input_path == !!config->data) ||
        (!config->input_path && config->data_size == 0))
        return NULL;
    ctx = (mkv_demuxer_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->io.file = TURBO_INVALID_FILE;
    result = turbo_media_stl_status_to_error(vec_init_bytes(
        &ctx->streams, sizeof(mkv_demuxer_stream_t),
        CMETA_ALIGNOF(mkv_demuxer_stream_t), SIZE_MAX));
    if (result != TURBO_OK) goto fail;
    result = turbo_container_io_open_reader(&ctx->io, config->input_path,
                                            config->data, config->data_size);
    if (result != TURBO_OK) goto fail;
    return ctx;
fail:
    turbo_container_io_close(&ctx->io);
    vec_destroy(&ctx->streams);
    free(ctx);
    return NULL;
}

static void mkv_demuxer_destroy_impl(void *ctx_ptr) {
    mkv_demuxer_ctx_t *ctx = (mkv_demuxer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    if (ctx->reader) mkv_reader_destroy(ctx->reader);
    for (size_t i = 0; i < vec_size(&ctx->streams); ++i) {
        mkv_demuxer_stream_t *stream =
            (mkv_demuxer_stream_t *)vec_at(&ctx->streams, i);
        if (stream) free(stream->extra_data);
    }
    vec_destroy(&ctx->streams);
    turbo_container_io_close(&ctx->io);
    free(ctx);
}

static int mkv_demuxer_open_impl(void *ctx_ptr) {
    mkv_demuxer_ctx_t *ctx = (mkv_demuxer_ctx_t *)ctx_ptr;
    struct mkv_reader_trackinfo_t callbacks = {
        mkv_on_video,
        mkv_on_audio,
        mkv_on_subtitle,
    };
    int result;
    if (!ctx || ctx->opened) return TURBO_EINVAL;
    ctx->reader = mkv_reader_create(&s_mkv_io, &ctx->io);
    if (!ctx->reader) return ctx->io.error != TURBO_OK ? ctx->io.error : TURBO_EPROTO;
    result = mkv_reader_getinfo(ctx->reader, &callbacks, ctx);
    if (result != TURBO_OK) return result;
    if (ctx->error != TURBO_OK) return ctx->error;
    if (vec_empty(&ctx->streams)) return TURBO_EPROTO;
    ctx->metadata.duration_ms = (int64_t)mkv_reader_getduration(ctx->reader);
    ctx->opened = 1;
    return TURBO_OK;
}

static int mkv_demuxer_read_packet_impl(void *ctx_ptr,
                                        turbo_demuxer_packet_t *packet) {
    mkv_demuxer_ctx_t *ctx = (mkv_demuxer_ctx_t *)ctx_ptr;
    mkv_packet_read_t read;
    int result;
    if (!ctx || !ctx->opened || !ctx->reader || !packet) return TURBO_EINVAL;
    memset(packet, 0, sizeof(*packet));
    read.ctx = ctx;
    read.packet = packet;
    result = mkv_reader_read2(ctx->reader, mkv_on_packet, &read);
    if (result <= 0 && packet->data) {
        free(packet->data);
        memset(packet, 0, sizeof(*packet));
    }
    return result;
}

static int mkv_demuxer_seek_impl(void *ctx_ptr, int64_t timestamp_ms, int flags) {
    mkv_demuxer_ctx_t *ctx = (mkv_demuxer_ctx_t *)ctx_ptr;
    int64_t timestamp = timestamp_ms;
    (void)flags;
    if (!ctx || !ctx->opened || timestamp_ms < 0) return TURBO_EINVAL;
    return mkv_reader_seek(ctx->reader, &timestamp);
}

static int mkv_demuxer_get_stream_count_impl(void *ctx_ptr) {
    mkv_demuxer_ctx_t *ctx = (mkv_demuxer_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->opened || vec_size(&ctx->streams) > INT_MAX)
        return TURBO_EINVAL;
    return (int)vec_size(&ctx->streams);
}

static int mkv_demuxer_get_stream_info_impl(void *ctx_ptr, int stream_index,
                                            turbo_stream_info_t *info) {
    mkv_demuxer_ctx_t *ctx = (mkv_demuxer_ctx_t *)ctx_ptr;
    const mkv_demuxer_stream_t *stream;
    if (!ctx || !ctx->opened || !info || stream_index < 0) return TURBO_EINVAL;
    stream = (const mkv_demuxer_stream_t *)vec_at_const(
        &ctx->streams, (size_t)stream_index);
    if (!stream) return TURBO_EINVAL;
    *info = stream->info;
    return TURBO_OK;
}

static int mkv_demuxer_get_metadata_impl(void *ctx_ptr,
                                         turbo_container_metadata_t *metadata) {
    mkv_demuxer_ctx_t *ctx = (mkv_demuxer_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->opened || !metadata) return TURBO_EINVAL;
    *metadata = ctx->metadata;
    return TURBO_OK;
}

static const char *s_mkv_extensions[] = {".mkv", ".mka", NULL};
static const char *s_webm_extensions[] = {".webm", ".weba", NULL};

const turbo_demuxer_ops_t turbo_mkv_demuxer_ops = {
    .name = "mkv", .extensions = s_mkv_extensions,
    .create = mkv_demuxer_create_impl, .destroy = mkv_demuxer_destroy_impl,
    .probe = mkv_probe_impl, .open = mkv_demuxer_open_impl,
    .read_packet = mkv_demuxer_read_packet_impl,
    .seek = mkv_demuxer_seek_impl,
    .get_stream_count = mkv_demuxer_get_stream_count_impl,
    .get_stream_info = mkv_demuxer_get_stream_info_impl,
    .get_metadata = mkv_demuxer_get_metadata_impl,
};

const turbo_demuxer_ops_t turbo_webm_demuxer_ops = {
    .name = "webm", .extensions = s_webm_extensions,
    .create = mkv_demuxer_create_impl, .destroy = mkv_demuxer_destroy_impl,
    .probe = webm_probe_impl, .open = mkv_demuxer_open_impl,
    .read_packet = mkv_demuxer_read_packet_impl,
    .seek = mkv_demuxer_seek_impl,
    .get_stream_count = mkv_demuxer_get_stream_count_impl,
    .get_stream_info = mkv_demuxer_get_stream_info_impl,
    .get_metadata = mkv_demuxer_get_metadata_impl,
};

#endif
