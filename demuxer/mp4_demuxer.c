#include "turbo_demuxer.h"

#ifdef TURBO_MEDIA_HAS_MP4

#include "container_io.h"
#include "stl_status.h"
#include "mov-buffer.h"
#include "mov-format.h"
#include "mov-reader.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <salts_error.h>
#include <cstl/vec.h>

typedef struct {
    uint32_t track;
    turbo_stream_info_t info;
    uint8_t *extra_data;
} mp4_demuxer_stream_t;

typedef struct {
    turbo_container_io_t io;
    mov_reader_t *reader;
    vec_t streams;
    turbo_container_metadata_t metadata;
    int opened;
    int error;
} mp4_demuxer_ctx_t;

typedef struct {
    mp4_demuxer_ctx_t *ctx;
    turbo_demuxer_packet_t *packet;
} mp4_packet_read_t;

static const struct mov_buffer_t s_mp4_io = {
    turbo_container_io_read,
    turbo_container_io_write,
    turbo_container_io_seek,
    turbo_container_io_tell,
};

static const char *mp4_video_codec_name(uint8_t object) {
    switch (object) {
        case MOV_OBJECT_H264: return "h264";
        case MOV_OBJECT_H265: return "h265";
        case MOV_OBJECT_H266: return "h266";
        case MOV_OBJECT_VP8: return "vp8";
        case MOV_OBJECT_VP9: return "vp9";
        case MOV_OBJECT_AV1: return "av1";
        case MOV_OBJECT_MP4V: return "mpeg4";
        default: return "unknown";
    }
}

static const char *mp4_audio_codec_name(uint8_t object) {
    switch (object) {
        case MOV_OBJECT_AAC:
        case MOV_OBJECT_AAC_MAIN:
        case MOV_OBJECT_AAC_LC:
        case MOV_OBJECT_AAC_SSR: return "aac";
        case MOV_OBJECT_OPUS: return "opus";
        case MOV_OBJECT_MP3: return "mp3";
        case MOV_OBJECT_G711a: return "pcma";
        case MOV_OBJECT_G711u: return "pcmu";
        case MOV_OBJECT_FLAC: return "flac";
        case MOV_OBJECT_AC3: return "ac3";
        case MOV_OBJECT_EAC3: return "eac3";
        default: return "unknown";
    }
}

static int mp4_add_stream(mp4_demuxer_ctx_t *ctx, uint32_t track,
                          turbo_codec_class_t type, const char *codec_name,
                          int width, int height, int channels,
                          int bits_per_sample, int sample_rate,
                          const void *extra, size_t bytes) {
    mp4_demuxer_stream_t stream;
    int result;

    if (!ctx || ctx->error != SALTS_OK) return ctx ? ctx->error : SALTS_EINVAL;
    memset(&stream, 0, sizeof(stream));
    stream.track = track;
    stream.info.stream_id = (int)track;
    stream.info.type = type;
    stream.info.codec_name = codec_name;
    stream.info.width = width;
    stream.info.height = height;
    stream.info.channels = channels;
    stream.info.sample_rate = sample_rate;
    (void)bits_per_sample;

    if (bytes > 0) {
        if (!extra) return ctx->error = SALTS_EPROTO;
        stream.extra_data = (uint8_t *)malloc(bytes);
        if (!stream.extra_data) return ctx->error = SALTS_ENOMEM;
        memcpy(stream.extra_data, extra, bytes);
        stream.info.extradata = stream.extra_data;
        stream.info.extradata_size = bytes;
    }
    result = turbo_media_stl_status_to_error(vec_push(&ctx->streams, &stream));
    if (result != SALTS_OK) {
        free(stream.extra_data);
        ctx->error = result;
    }
    return result;
}

static void mp4_on_video(void *param, uint32_t track, uint8_t object, int width,
                         int height, const void *extra, size_t bytes) {
    mp4_demuxer_ctx_t *ctx = (mp4_demuxer_ctx_t *)param;
    (void)mp4_add_stream(ctx, track, TURBO_CODEC_TYPE_VIDEO,
                         mp4_video_codec_name(object), width, height, 0, 0, 0,
                         extra, bytes);
}

static void mp4_on_audio(void *param, uint32_t track, uint8_t object,
                         int channels, int bits_per_sample, int sample_rate,
                         const void *extra, size_t bytes) {
    mp4_demuxer_ctx_t *ctx = (mp4_demuxer_ctx_t *)param;
    (void)mp4_add_stream(ctx, track, TURBO_CODEC_TYPE_AUDIO,
                         mp4_audio_codec_name(object), 0, 0, channels,
                         bits_per_sample, sample_rate, extra, bytes);
}

static int mp4_stream_index(const mp4_demuxer_ctx_t *ctx, uint32_t track) {
    size_t count = vec_size(&ctx->streams);
    for (size_t i = 0; i < count; ++i) {
        const mp4_demuxer_stream_t *stream =
            (const mp4_demuxer_stream_t *)vec_at_const(&ctx->streams, i);
        if (stream && stream->track == track) return (int)i;
    }
    return -1;
}

static void *mp4_on_packet(void *param, uint32_t track, size_t bytes,
                           int64_t pts, int64_t dts, int flags) {
    mp4_packet_read_t *read = (mp4_packet_read_t *)param;
    turbo_demuxer_packet_t *packet;
    int stream_index;

    if (!read || !read->ctx || !read->packet || bytes == 0) return NULL;
    stream_index = mp4_stream_index(read->ctx, track);
    if (stream_index < 0) return NULL;
    packet = read->packet;
    packet->data = (uint8_t *)malloc(bytes);
    if (!packet->data) return NULL;
    packet->size = bytes;
    packet->stream_index = stream_index;
    packet->pts = pts;
    packet->dts = dts;
    packet->duration = 0;
    packet->is_keyframe = (flags & MOV_AV_FLAG_KEYFREAME) != 0;
    return packet->data;
}

static int mp4_probe_impl(const uint8_t *data, size_t size) {
    if (!data || size < 8) return 0;
    if ((memcmp(data + 4, "ftyp", 4) == 0) ||
        (memcmp(data + 4, "moov", 4) == 0) ||
        (memcmp(data + 4, "moof", 4) == 0) ||
        (memcmp(data + 4, "mdat", 4) == 0) ||
        (memcmp(data + 4, "free", 4) == 0))
        return 100;
    return 0;
}

static void *mp4_demuxer_create_impl(const turbo_demuxer_config_t *config) {
    mp4_demuxer_ctx_t *ctx;
    int result;

    if (!config || (!!config->input_path == !!config->data) ||
        (!config->input_path && config->data_size == 0))
        return NULL;
    ctx = (mp4_demuxer_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->io.file = SALTS_INVALID_FILE;
    result = turbo_media_stl_status_to_error(vec_init_bytes(
        &ctx->streams, sizeof(mp4_demuxer_stream_t),
        CMETA_ALIGNOF(mp4_demuxer_stream_t), SIZE_MAX));
    if (result != SALTS_OK) goto fail;
    result = turbo_container_io_open_reader(&ctx->io, config->input_path,
                                            config->data, config->data_size);
    if (result != SALTS_OK) goto fail;
    return ctx;

fail:
    turbo_container_io_close(&ctx->io);
    vec_destroy(&ctx->streams);
    free(ctx);
    return NULL;
}

static void mp4_demuxer_destroy_impl(void *ctx_ptr) {
    mp4_demuxer_ctx_t *ctx = (mp4_demuxer_ctx_t *)ctx_ptr;
    size_t count;

    if (!ctx) return;
    if (ctx->reader) mov_reader_destroy(ctx->reader);
    count = vec_size(&ctx->streams);
    for (size_t i = 0; i < count; ++i) {
        mp4_demuxer_stream_t *stream =
            (mp4_demuxer_stream_t *)vec_at(&ctx->streams, i);
        if (stream) free(stream->extra_data);
    }
    vec_destroy(&ctx->streams);
    turbo_container_io_close(&ctx->io);
    free(ctx);
}

static int mp4_demuxer_open_impl(void *ctx_ptr) {
    mp4_demuxer_ctx_t *ctx = (mp4_demuxer_ctx_t *)ctx_ptr;
    struct mov_reader_trackinfo_t callbacks = {
        mp4_on_video,
        mp4_on_audio,
        NULL,
    };
    int result;

    if (!ctx || ctx->opened) return SALTS_EINVAL;
    ctx->reader = mov_reader_create(&s_mp4_io, &ctx->io);
    if (!ctx->reader) return ctx->io.error != SALTS_OK ? ctx->io.error : SALTS_EPROTO;
    result = mov_reader_getinfo(ctx->reader, &callbacks, ctx);
    if (result != SALTS_OK) return result;
    if (ctx->error != SALTS_OK) return ctx->error;
    if (vec_empty(&ctx->streams)) return SALTS_EPROTO;
    ctx->metadata.duration_ms = (int64_t)mov_reader_getduration(ctx->reader);
    ctx->opened = 1;
    return SALTS_OK;
}

static int mp4_demuxer_read_packet_impl(void *ctx_ptr,
                                        turbo_demuxer_packet_t *packet) {
    mp4_demuxer_ctx_t *ctx = (mp4_demuxer_ctx_t *)ctx_ptr;
    mp4_packet_read_t read;
    int result;

    if (!ctx || !ctx->opened || !ctx->reader || !packet) return SALTS_EINVAL;
    memset(packet, 0, sizeof(*packet));
    read.ctx = ctx;
    read.packet = packet;
    result = mov_reader_read2(ctx->reader, mp4_on_packet, &read);
    if (result <= 0 && packet->data) {
        free(packet->data);
        memset(packet, 0, sizeof(*packet));
    }
    return result;
}

static int mp4_demuxer_seek_impl(void *ctx_ptr, int64_t timestamp_ms, int flags) {
    mp4_demuxer_ctx_t *ctx = (mp4_demuxer_ctx_t *)ctx_ptr;
    int64_t timestamp = timestamp_ms;
    (void)flags;
    if (!ctx || !ctx->opened || timestamp_ms < 0) return SALTS_EINVAL;
    return mov_reader_seek(ctx->reader, &timestamp);
}

static int mp4_demuxer_get_stream_count_impl(void *ctx_ptr) {
    mp4_demuxer_ctx_t *ctx = (mp4_demuxer_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->opened || vec_size(&ctx->streams) > INT_MAX)
        return SALTS_EINVAL;
    return (int)vec_size(&ctx->streams);
}

static int mp4_demuxer_get_stream_info_impl(void *ctx_ptr, int stream_index,
                                            turbo_stream_info_t *info) {
    mp4_demuxer_ctx_t *ctx = (mp4_demuxer_ctx_t *)ctx_ptr;
    const mp4_demuxer_stream_t *stream;
    if (!ctx || !ctx->opened || !info || stream_index < 0) return SALTS_EINVAL;
    stream = (const mp4_demuxer_stream_t *)vec_at_const(
        &ctx->streams, (size_t)stream_index);
    if (!stream) return SALTS_EINVAL;
    *info = stream->info;
    return SALTS_OK;
}

static int mp4_demuxer_get_metadata_impl(void *ctx_ptr,
                                         turbo_container_metadata_t *metadata) {
    mp4_demuxer_ctx_t *ctx = (mp4_demuxer_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->opened || !metadata) return SALTS_EINVAL;
    *metadata = ctx->metadata;
    return SALTS_OK;
}

static const char *s_mp4_extensions[] = {".mp4", ".m4v", ".m4a", ".mov", NULL};

const turbo_demuxer_ops_t turbo_mp4_demuxer_ops = {
    .name = "mp4",
    .extensions = s_mp4_extensions,
    .create = mp4_demuxer_create_impl,
    .destroy = mp4_demuxer_destroy_impl,
    .probe = mp4_probe_impl,
    .open = mp4_demuxer_open_impl,
    .read_packet = mp4_demuxer_read_packet_impl,
    .seek = mp4_demuxer_seek_impl,
    .get_stream_count = mp4_demuxer_get_stream_count_impl,
    .get_stream_info = mp4_demuxer_get_stream_info_impl,
    .get_metadata = mp4_demuxer_get_metadata_impl,
};

#endif
