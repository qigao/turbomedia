#include "turbo_muxer.h"

#ifdef TURBO_MEDIA_HAS_MP4

#include "container_io.h"
#include "stl_status.h"
#include "mov-buffer.h"
#include "mov-format.h"
#include "mov-writer.h"
#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
#include "mpeg4-vvc.h"

#include <stdlib.h>
#include <string.h>

#include <turbo_error.h>
#include <turbo_vstr.h>
#include <turbostl/vec.h>

typedef union {
    struct mpeg4_avc_t avc;
    struct mpeg4_hevc_t hevc;
    struct mpeg4_vvc_t vvc;
} mp4_video_config_t;

typedef struct {
    int track;
    uint8_t object;
    mp4_video_config_t config;
    vec_t sample;
} mp4_muxer_stream_t;

typedef struct {
    turbo_container_io_t io;
    mov_writer_t *writer;
    vec_t streams;
    int finalized;
} mp4_muxer_ctx_t;

static const struct mov_buffer_t s_mp4_io = {
    turbo_container_io_read,
    turbo_container_io_write,
    turbo_container_io_seek,
    turbo_container_io_tell,
};

static int mp4_name_is(const char *name, const char *expected) {
    return vstr_ieq(vstr_from_cstr(name), vstr_from_cstr(expected));
}

static int mp4_codec_object(const turbo_stream_info_t *info, uint8_t *object) {
    const char *name;

    if (!info || !object || !info->codec_name) return TURBO_EINVAL;
    name = info->codec_name;
    if (info->type == TURBO_CODEC_TYPE_VIDEO) {
        if (mp4_name_is(name, "h264") || mp4_name_is(name, "avc"))
            *object = MOV_OBJECT_H264;
        else if (mp4_name_is(name, "h265") || mp4_name_is(name, "hevc"))
            *object = MOV_OBJECT_H265;
        else if (mp4_name_is(name, "h266") || mp4_name_is(name, "vvc"))
            *object = MOV_OBJECT_H266;
        else if (mp4_name_is(name, "vp8"))
            *object = MOV_OBJECT_VP8;
        else if (mp4_name_is(name, "vp9"))
            *object = MOV_OBJECT_VP9;
        else if (mp4_name_is(name, "av1"))
            *object = MOV_OBJECT_AV1;
        else
            return TURBO_ENOTSUP;
        return TURBO_OK;
    }

    if (info->type != TURBO_CODEC_TYPE_AUDIO) return TURBO_ENOTSUP;
    if (mp4_name_is(name, "aac"))
        *object = MOV_OBJECT_AAC;
    else if (mp4_name_is(name, "opus"))
        *object = MOV_OBJECT_OPUS;
    else if (mp4_name_is(name, "mp3"))
        *object = MOV_OBJECT_MP3;
    else if (mp4_name_is(name, "pcma") || mp4_name_is(name, "g711a"))
        *object = MOV_OBJECT_G711a;
    else if (mp4_name_is(name, "pcmu") || mp4_name_is(name, "g711u"))
        *object = MOV_OBJECT_G711u;
    else if (mp4_name_is(name, "flac"))
        *object = MOV_OBJECT_FLAC;
    else
        return TURBO_ENOTSUP;
    return TURBO_OK;
}

static int mp4_resize_bytes(vec_t *buffer, size_t input_size) {
    size_t capacity;

    if (input_size > (SIZE_MAX - 64U) / 2U) return TURBO_EFBIG;
    capacity = input_size * 2U + 64U;
    return turbo_media_stl_status_to_error(vec_resize(buffer, capacity));
}

static int mp4_normalize_video_config(mp4_muxer_stream_t *stream,
                                      const uint8_t *extra_data,
                                      size_t extra_data_size,
                                      vec_t *normalized) {
    int bytes;
    int result;

    if (!stream || !extra_data || extra_data_size == 0 || !normalized)
        return TURBO_EINVAL;
    result = mp4_resize_bytes(normalized, extra_data_size);
    if (result != TURBO_OK) return result;

    switch (stream->object) {
        case MOV_OBJECT_H264:
            memset(&stream->config.avc, 0, sizeof(stream->config.avc));
            if (mpeg4_avc_decoder_configuration_record_load(
                    extra_data, extra_data_size, &stream->config.avc) <= 0)
                return TURBO_EINVAL;
            bytes = mpeg4_avc_decoder_configuration_record_save(
                &stream->config.avc, vec_data(normalized),
                vec_size(normalized));
            break;
        case MOV_OBJECT_H265:
            memset(&stream->config.hevc, 0, sizeof(stream->config.hevc));
            if (mpeg4_hevc_decoder_configuration_record_load(
                    extra_data, extra_data_size, &stream->config.hevc) <= 0)
                return TURBO_EINVAL;
            bytes = mpeg4_hevc_decoder_configuration_record_save(
                &stream->config.hevc, vec_data(normalized),
                vec_size(normalized));
            break;
        case MOV_OBJECT_H266: {
            int update = 0;
            memset(&stream->config.vvc, 0, sizeof(stream->config.vvc));
            if (mpeg4_vvc_decoder_configuration_record_load(
                    extra_data, extra_data_size, &stream->config.vvc) <= 0) {
                (void)h266_annexbtomp4(
                    &stream->config.vvc, extra_data, extra_data_size,
                    vec_data(normalized), vec_size(normalized), NULL,
                    &update);
                if (!update) return TURBO_EINVAL;
            }
            bytes = mpeg4_vvc_decoder_configuration_record_save(
                &stream->config.vvc, vec_data(normalized),
                vec_size(normalized));
            break;
        }
        default:
            return TURBO_ENOTSUP;
    }
    if (bytes <= 0) return TURBO_EINVAL;
    return turbo_media_stl_status_to_error(
        vec_resize(normalized, (size_t)bytes));
}

static int mp4_convert_video_sample(mp4_muxer_stream_t *stream,
                                    const uint8_t *input, size_t input_size,
                                    const uint8_t **output, size_t *output_size) {
    int bytes;
    int update = 0;
    int result;

    if (!stream || !input || input_size == 0 || !output || !output_size)
        return TURBO_EINVAL;
    if (stream->object != MOV_OBJECT_H264 && stream->object != MOV_OBJECT_H265 &&
        stream->object != MOV_OBJECT_H266) {
        *output = input;
        *output_size = input_size;
        return TURBO_OK;
    }

    result = mp4_resize_bytes(&stream->sample, input_size);
    if (result != TURBO_OK) return result;
    if (stream->object == MOV_OBJECT_H264)
        bytes = h264_annexbtomp4(&stream->config.avc, input, input_size,
                                 vec_data(&stream->sample),
                                 vec_size(&stream->sample), NULL, &update);
    else if (stream->object == MOV_OBJECT_H265)
        bytes = h265_annexbtomp4(&stream->config.hevc, input, input_size,
                                 vec_data(&stream->sample),
                                 vec_size(&stream->sample), NULL, &update);
    else
        bytes = h266_annexbtomp4(&stream->config.vvc, input, input_size,
                                 vec_data(&stream->sample),
                                 vec_size(&stream->sample), NULL, &update);

    if (bytes <= 0 || update) return TURBO_EPROTO;
    *output = (const uint8_t *)vec_data_const(&stream->sample);
    *output_size = (size_t)bytes;
    return TURBO_OK;
}

static void mp4_stream_destroy(mp4_muxer_stream_t *stream) {
    if (!stream) return;
    vec_destroy(&stream->sample);
    free(stream);
}

static void *mp4_muxer_create_impl(const turbo_muxer_config_t *config) {
    mp4_muxer_ctx_t *ctx;
    int result;
    int flags;

    if (!config || config->format != TURBO_MUXER_MP4) return NULL;
    ctx = (mp4_muxer_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->io.file = TURBO_INVALID_FILE;

    result = turbo_media_stl_status_to_error(vec_init_bytes(
        &ctx->streams, sizeof(mp4_muxer_stream_t *),
        CMETA_ALIGNOF(mp4_muxer_stream_t *), SIZE_MAX));
    if (result != TURBO_OK) goto fail;
    result = turbo_container_io_open_writer(&ctx->io, config->output_path);
    if (result != TURBO_OK) goto fail;

    flags = config->faststart ? MOV_FLAG_FASTSTART : 0;
    ctx->writer = mov_writer_create(&s_mp4_io, &ctx->io, flags);
    if (!ctx->writer || ctx->io.error != TURBO_OK) goto fail;
    return ctx;

fail:
    if (ctx->writer) mov_writer_destroy(ctx->writer);
    turbo_container_io_close(&ctx->io);
    vec_destroy(&ctx->streams);
    free(ctx);
    return NULL;
}

static int mp4_muxer_finalize(mp4_muxer_ctx_t *ctx) {
    if (!ctx) return TURBO_EINVAL;
    if (ctx->finalized) return ctx->io.error;
    if (!ctx->writer) return TURBO_EINVAL;
    mov_writer_destroy(ctx->writer);
    ctx->writer = NULL;
    ctx->finalized = 1;
    if (ctx->io.error != TURBO_OK) return ctx->io.error;
    return turbo_container_io_flush(&ctx->io);
}

static void mp4_muxer_destroy_impl(void *ctx_ptr) {
    mp4_muxer_ctx_t *ctx = (mp4_muxer_ctx_t *)ctx_ptr;
    size_t count;

    if (!ctx) return;
    if (!ctx->finalized && ctx->writer) (void)mp4_muxer_finalize(ctx);
    count = vec_size(&ctx->streams);
    for (size_t i = 0; i < count; ++i) {
        mp4_muxer_stream_t **entry =
            (mp4_muxer_stream_t **)vec_at(&ctx->streams, i);
        if (entry) mp4_stream_destroy(*entry);
    }
    vec_destroy(&ctx->streams);
    turbo_container_io_close(&ctx->io);
    free(ctx);
}

static int mp4_muxer_add_stream_impl(void *ctx_ptr,
                                     const turbo_stream_info_t *info,
                                     int *stream_id) {
    mp4_muxer_ctx_t *ctx = (mp4_muxer_ctx_t *)ctx_ptr;
    mp4_muxer_stream_t *stream = NULL;
    vec_t normalized = {0};
    const void *extra_data;
    size_t extra_data_size;
    size_t next_id;
    int result;

    if (!ctx || !ctx->writer || !info || !stream_id || ctx->finalized)
        return TURBO_EINVAL;
    if ((info->type == TURBO_CODEC_TYPE_VIDEO &&
         (info->width <= 0 || info->height <= 0)) ||
        (info->type == TURBO_CODEC_TYPE_AUDIO &&
         (info->sample_rate <= 0 || info->channels <= 0)))
        return TURBO_EINVAL;

    next_id = vec_size(&ctx->streams);
    result = turbo_media_stl_status_to_error(
        vec_reserve(&ctx->streams, next_id + 1));
    if (result != TURBO_OK) return result;
    stream = (mp4_muxer_stream_t *)calloc(1, sizeof(*stream));
    if (!stream) return TURBO_ENOMEM;
    result = turbo_media_stl_status_to_error(vec_init_bytes(
        &stream->sample, sizeof(uint8_t), CMETA_ALIGNOF(uint8_t), SIZE_MAX));
    if (result != TURBO_OK) goto fail;
    result = mp4_codec_object(info, &stream->object);
    if (result != TURBO_OK) goto fail;

    extra_data = info->extradata;
    extra_data_size = info->extradata_size;
    if (info->type == TURBO_CODEC_TYPE_VIDEO &&
        (stream->object == MOV_OBJECT_H264 || stream->object == MOV_OBJECT_H265 ||
         stream->object == MOV_OBJECT_H266)) {
        result = turbo_media_stl_status_to_error(vec_init_bytes(
            &normalized, sizeof(uint8_t), CMETA_ALIGNOF(uint8_t), SIZE_MAX));
        if (result != TURBO_OK) goto fail;
        result = mp4_normalize_video_config(stream, info->extradata,
                                            info->extradata_size, &normalized);
        if (result != TURBO_OK) goto fail;
        extra_data = vec_data_const(&normalized);
        extra_data_size = vec_size(&normalized);
    }

    if (info->type == TURBO_CODEC_TYPE_VIDEO)
        stream->track = mov_writer_add_video(
            ctx->writer, stream->object, info->width, info->height, extra_data,
            extra_data_size);
    else
        stream->track = mov_writer_add_audio(
            ctx->writer, stream->object, info->channels, 16, info->sample_rate,
            extra_data, extra_data_size);
    if (stream->track < 0) {
        result = stream->track;
        goto fail;
    }

    result = turbo_media_stl_status_to_error(vec_push(&ctx->streams, &stream));
    if (result != TURBO_OK) goto fail;
    *stream_id = (int)next_id;
    vec_destroy(&normalized);
    return TURBO_OK;

fail:
    vec_destroy(&normalized);
    mp4_stream_destroy(stream);
    return result;
}

static int mp4_muxer_write_header_impl(void *ctx_ptr) {
    mp4_muxer_ctx_t *ctx = (mp4_muxer_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->writer || ctx->finalized) return TURBO_EINVAL;
    return vec_empty(&ctx->streams) ? TURBO_EINVAL : ctx->io.error;
}

static int mp4_muxer_write_packet_impl(void *ctx_ptr,
                                       const turbo_muxer_packet_t *packet) {
    mp4_muxer_ctx_t *ctx = (mp4_muxer_ctx_t *)ctx_ptr;
    mp4_muxer_stream_t **entry;
    mp4_muxer_stream_t *stream;
    const uint8_t *data;
    size_t size;
    int flags;
    int result;

    if (!ctx || !ctx->writer || !packet || !packet->data || packet->size == 0 ||
        packet->stream_id < 0 || ctx->finalized)
        return TURBO_EINVAL;
    entry = (mp4_muxer_stream_t **)vec_at(
        &ctx->streams, (size_t)packet->stream_id);
    if (!entry || !*entry) return TURBO_EINVAL;
    stream = *entry;
    result = mp4_convert_video_sample(stream, packet->data, packet->size, &data,
                                      &size);
    if (result != TURBO_OK) return result;

    flags = packet->is_keyframe ? MOV_AV_FLAG_KEYFREAME : 0;
    result = mov_writer_write(ctx->writer, stream->track, data, size,
                              packet->pts / 1000, packet->dts / 1000, flags);
    if (result != TURBO_OK) return result;
    return ctx->io.error;
}

static int mp4_muxer_write_trailer_impl(void *ctx_ptr) {
    return mp4_muxer_finalize((mp4_muxer_ctx_t *)ctx_ptr);
}

static int mp4_muxer_get_data_impl(void *ctx_ptr, uint8_t **data, size_t *size) {
    mp4_muxer_ctx_t *ctx = (mp4_muxer_ctx_t *)ctx_ptr;
    if (!ctx) return TURBO_EINVAL;
    return turbo_container_io_get_memory(&ctx->io, data, size);
}

static const char *s_mp4_extensions[] = {".mp4", ".m4v", ".m4a", ".mov", NULL};

const turbo_muxer_ops_t turbo_mp4_muxer_ops = {
    .name = "mp4",
    .format = TURBO_MUXER_MP4,
    .extensions = s_mp4_extensions,
    .create = mp4_muxer_create_impl,
    .destroy = mp4_muxer_destroy_impl,
    .add_stream = mp4_muxer_add_stream_impl,
    .write_header = mp4_muxer_write_header_impl,
    .write_packet = mp4_muxer_write_packet_impl,
    .write_trailer = mp4_muxer_write_trailer_impl,
    .get_data = mp4_muxer_get_data_impl,
};

#endif
