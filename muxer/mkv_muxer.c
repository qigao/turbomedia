#include "turbo_muxer.h"

#ifdef TURBO_MEDIA_HAS_MKV

#include "container_io.h"
#include "mkv-buffer.h"
#include "mkv-format.h"
#include "mkv-writer.h"
#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
#include "mpeg4-vvc.h"

#include <stdlib.h>
#include <string.h>

#include <turbo_error.h>
#include <turbo_str_view.h>
#include <turbo_vec.h>

typedef union {
    struct mpeg4_avc_t avc;
    struct mpeg4_hevc_t hevc;
    struct mpeg4_vvc_t vvc;
} mkv_video_config_t;

typedef struct {
    int track;
    enum mkv_codec_t codec;
    mkv_video_config_t config;
    turbo_vec_t sample;
} mkv_muxer_stream_t;

typedef struct {
    turbo_container_io_t io;
    mkv_writer_t *writer;
    turbo_vec_t streams;
    turbo_muxer_format_t format;
    int finalized;
} mkv_muxer_ctx_t;

static const struct mkv_buffer_t s_mkv_io = {
    turbo_container_io_read,
    turbo_container_io_write,
    turbo_container_io_seek,
    turbo_container_io_tell,
};

static int mkv_name_is(const char *name, const char *expected) {
    return tstr_v_ieq(tstr_v_from_cstr(name), tstr_v_from_cstr(expected));
}

static int mkv_codec(const turbo_stream_info_t *info, enum mkv_codec_t *codec) {
    const char *name;
    if (!info || !codec || !info->codec_name) return TURBO_EINVAL;
    name = info->codec_name;
    if (info->type == TURBO_CODEC_TYPE_VIDEO) {
        if (mkv_name_is(name, "h264") || mkv_name_is(name, "avc"))
            *codec = MKV_CODEC_VIDEO_H264;
        else if (mkv_name_is(name, "h265") || mkv_name_is(name, "hevc"))
            *codec = MKV_CODEC_VIDEO_H265;
        else if (mkv_name_is(name, "h266") || mkv_name_is(name, "vvc"))
            *codec = MKV_CODEC_VIDEO_H266;
        else if (mkv_name_is(name, "vp8"))
            *codec = MKV_CODEC_VIDEO_VP8;
        else if (mkv_name_is(name, "vp9"))
            *codec = MKV_CODEC_VIDEO_VP9;
        else if (mkv_name_is(name, "av1"))
            *codec = MKV_CODEC_VIDEO_AV1;
        else
            return TURBO_ENOTSUP;
        return TURBO_OK;
    }
    if (info->type != TURBO_CODEC_TYPE_AUDIO) return TURBO_ENOTSUP;
    if (mkv_name_is(name, "aac"))
        *codec = MKV_CODEC_AUDIO_AAC;
    else if (mkv_name_is(name, "opus"))
        *codec = MKV_CODEC_AUDIO_OPUS;
    else if (mkv_name_is(name, "mp3"))
        *codec = MKV_CODEC_AUDIO_MP3;
    else if (mkv_name_is(name, "flac"))
        *codec = MKV_CODEC_AUDIO_FLAC;
    else if (mkv_name_is(name, "ac3"))
        *codec = MKV_CODEC_AUDIO_AC3;
    else if (mkv_name_is(name, "eac3"))
        *codec = MKV_CODEC_AUDIO_EAC3;
    else
        return TURBO_ENOTSUP;
    return TURBO_OK;
}

static int mkv_codec_is_h26x(enum mkv_codec_t codec) {
    return codec == MKV_CODEC_VIDEO_H264 || codec == MKV_CODEC_VIDEO_H265 ||
           codec == MKV_CODEC_VIDEO_H266;
}

static int mkv_resize_bytes(turbo_vec_t *buffer, size_t input_size) {
    if (input_size > (SIZE_MAX - 64U) / 2U) return TURBO_EFBIG;
    return turbo_vec_resize(buffer, input_size * 2U + 64U);
}

static int mkv_normalize_config(mkv_muxer_stream_t *stream,
                                const uint8_t *input, size_t input_size,
                                turbo_vec_t *output) {
    int bytes;
    int result;
    if (!stream || !input || input_size == 0 || !output) return TURBO_EINVAL;
    result = mkv_resize_bytes(output, input_size);
    if (result != TURBO_OK) return result;

    if (stream->codec == MKV_CODEC_VIDEO_H264) {
        memset(&stream->config.avc, 0, sizeof(stream->config.avc));
        if (mpeg4_avc_decoder_configuration_record_load(
                input, input_size, &stream->config.avc) <= 0)
            return TURBO_EINVAL;
        bytes = mpeg4_avc_decoder_configuration_record_save(
            &stream->config.avc, turbo_vec_data(output), turbo_vec_size(output));
    } else if (stream->codec == MKV_CODEC_VIDEO_H265) {
        memset(&stream->config.hevc, 0, sizeof(stream->config.hevc));
        if (mpeg4_hevc_decoder_configuration_record_load(
                input, input_size, &stream->config.hevc) <= 0)
            return TURBO_EINVAL;
        bytes = mpeg4_hevc_decoder_configuration_record_save(
            &stream->config.hevc, turbo_vec_data(output), turbo_vec_size(output));
    } else {
        int update = 0;
        memset(&stream->config.vvc, 0, sizeof(stream->config.vvc));
        if (mpeg4_vvc_decoder_configuration_record_load(
                input, input_size, &stream->config.vvc) <= 0) {
            (void)h266_annexbtomp4(&stream->config.vvc, input, input_size,
                                   turbo_vec_data(output), turbo_vec_size(output),
                                   NULL, &update);
            if (!update) return TURBO_EINVAL;
        }
        bytes = mpeg4_vvc_decoder_configuration_record_save(
            &stream->config.vvc, turbo_vec_data(output), turbo_vec_size(output));
    }
    if (bytes <= 0) return TURBO_EINVAL;
    return turbo_vec_resize(output, (size_t)bytes);
}

static int mkv_convert_sample(mkv_muxer_stream_t *stream, const uint8_t *input,
                              size_t input_size, const uint8_t **output,
                              size_t *output_size) {
    int bytes;
    int update = 0;
    int result;
    if (!stream || !input || input_size == 0 || !output || !output_size)
        return TURBO_EINVAL;
    if (!mkv_codec_is_h26x(stream->codec)) {
        *output = input;
        *output_size = input_size;
        return TURBO_OK;
    }
    result = mkv_resize_bytes(&stream->sample, input_size);
    if (result != TURBO_OK) return result;
    if (stream->codec == MKV_CODEC_VIDEO_H264)
        bytes = h264_annexbtomp4(&stream->config.avc, input, input_size,
                                 turbo_vec_data(&stream->sample),
                                 turbo_vec_size(&stream->sample), NULL, &update);
    else if (stream->codec == MKV_CODEC_VIDEO_H265)
        bytes = h265_annexbtomp4(&stream->config.hevc, input, input_size,
                                 turbo_vec_data(&stream->sample),
                                 turbo_vec_size(&stream->sample), NULL, &update);
    else
        bytes = h266_annexbtomp4(&stream->config.vvc, input, input_size,
                                 turbo_vec_data(&stream->sample),
                                 turbo_vec_size(&stream->sample), NULL, &update);
    if (bytes <= 0 || update) return TURBO_EPROTO;
    *output = (const uint8_t *)turbo_vec_data_const(&stream->sample);
    *output_size = (size_t)bytes;
    return TURBO_OK;
}

static void mkv_stream_destroy(mkv_muxer_stream_t *stream) {
    if (!stream) return;
    turbo_vec_destroy(&stream->sample);
    free(stream);
}

static void *mkv_muxer_create_impl(const turbo_muxer_config_t *config) {
    mkv_muxer_ctx_t *ctx;
    int options;
    int result;
    if (!config || (config->format != TURBO_MUXER_MKV &&
                    config->format != TURBO_MUXER_WEBM))
        return NULL;
    ctx = (mkv_muxer_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->io.file = TURBO_INVALID_FILE;
    ctx->format = config->format;
    result = turbo_vec_init(&ctx->streams, sizeof(mkv_muxer_stream_t *));
    if (result != TURBO_OK) goto fail;
    result = turbo_container_io_open_writer(&ctx->io, config->output_path);
    if (result != TURBO_OK) goto fail;
    options = config->format == TURBO_MUXER_WEBM ? MKV_OPTION_WEBM : 0;
    ctx->writer = mkv_writer_create(&s_mkv_io, &ctx->io, options);
    if (!ctx->writer || ctx->io.error != TURBO_OK) goto fail;
    return ctx;
fail:
    if (ctx->writer) mkv_writer_destroy(ctx->writer);
    turbo_container_io_close(&ctx->io);
    turbo_vec_destroy(&ctx->streams);
    free(ctx);
    return NULL;
}

static int mkv_muxer_finalize(mkv_muxer_ctx_t *ctx) {
    if (!ctx) return TURBO_EINVAL;
    if (ctx->finalized) return ctx->io.error;
    if (!ctx->writer) return TURBO_EINVAL;
    mkv_writer_destroy(ctx->writer);
    ctx->writer = NULL;
    ctx->finalized = 1;
    if (ctx->io.error != TURBO_OK) return ctx->io.error;
    return turbo_container_io_flush(&ctx->io);
}

static void mkv_muxer_destroy_impl(void *ctx_ptr) {
    mkv_muxer_ctx_t *ctx = (mkv_muxer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    if (!ctx->finalized && ctx->writer) (void)mkv_muxer_finalize(ctx);
    for (size_t i = 0; i < turbo_vec_size(&ctx->streams); ++i) {
        mkv_muxer_stream_t **stream =
            (mkv_muxer_stream_t **)turbo_vec_at(&ctx->streams, i);
        if (stream) mkv_stream_destroy(*stream);
    }
    turbo_vec_destroy(&ctx->streams);
    turbo_container_io_close(&ctx->io);
    free(ctx);
}

static int mkv_muxer_add_stream_impl(void *ctx_ptr,
                                     const turbo_stream_info_t *info,
                                     int *stream_id) {
    mkv_muxer_ctx_t *ctx = (mkv_muxer_ctx_t *)ctx_ptr;
    mkv_muxer_stream_t *stream = NULL;
    turbo_vec_t normalized = {0};
    const void *extra;
    size_t extra_size;
    size_t next_id;
    int result;
    if (!ctx || !ctx->writer || !info || !stream_id || ctx->finalized)
        return TURBO_EINVAL;
    if ((info->type == TURBO_CODEC_TYPE_VIDEO &&
         (info->width <= 0 || info->height <= 0)) ||
        (info->type == TURBO_CODEC_TYPE_AUDIO &&
         (info->sample_rate <= 0 || info->channels <= 0)))
        return TURBO_EINVAL;
    next_id = turbo_vec_size(&ctx->streams);
    result = turbo_vec_reserve(&ctx->streams, next_id + 1);
    if (result != TURBO_OK) return result;
    stream = (mkv_muxer_stream_t *)calloc(1, sizeof(*stream));
    if (!stream) return TURBO_ENOMEM;
    result = turbo_vec_init(&stream->sample, sizeof(uint8_t));
    if (result != TURBO_OK) goto fail;
    result = mkv_codec(info, &stream->codec);
    if (result != TURBO_OK) goto fail;
    if (ctx->format == TURBO_MUXER_WEBM &&
        stream->codec != MKV_CODEC_VIDEO_VP8 &&
        stream->codec != MKV_CODEC_VIDEO_VP9 &&
        stream->codec != MKV_CODEC_VIDEO_AV1 &&
        stream->codec != MKV_CODEC_AUDIO_OPUS) {
        result = TURBO_ENOTSUP;
        goto fail;
    }

    extra = info->extradata;
    extra_size = info->extradata_size;
    if (mkv_codec_is_h26x(stream->codec)) {
        result = turbo_vec_init(&normalized, sizeof(uint8_t));
        if (result != TURBO_OK) goto fail;
        result = mkv_normalize_config(stream, info->extradata,
                                      info->extradata_size, &normalized);
        if (result != TURBO_OK) goto fail;
        extra = turbo_vec_data_const(&normalized);
        extra_size = turbo_vec_size(&normalized);
    }

    if (info->type == TURBO_CODEC_TYPE_VIDEO)
        stream->track = mkv_writer_add_video(ctx->writer, stream->codec,
                                             info->width, info->height, extra,
                                             extra_size);
    else
        stream->track = mkv_writer_add_audio(ctx->writer, stream->codec,
                                             info->channels, 16,
                                             info->sample_rate, extra, extra_size);
    if (stream->track < 0) {
        result = stream->track;
        goto fail;
    }
    result = turbo_vec_push(&ctx->streams, &stream);
    if (result != TURBO_OK) goto fail;
    *stream_id = (int)next_id;
    turbo_vec_destroy(&normalized);
    return TURBO_OK;
fail:
    turbo_vec_destroy(&normalized);
    mkv_stream_destroy(stream);
    return result;
}

static int mkv_muxer_write_header_impl(void *ctx_ptr) {
    mkv_muxer_ctx_t *ctx = (mkv_muxer_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->writer || ctx->finalized) return TURBO_EINVAL;
    return turbo_vec_empty(&ctx->streams) ? TURBO_EINVAL : ctx->io.error;
}

static int mkv_muxer_write_packet_impl(void *ctx_ptr,
                                       const turbo_muxer_packet_t *packet) {
    mkv_muxer_ctx_t *ctx = (mkv_muxer_ctx_t *)ctx_ptr;
    mkv_muxer_stream_t **entry;
    const uint8_t *data;
    size_t size;
    int flags;
    int result;
    if (!ctx || !ctx->writer || !packet || !packet->data || packet->size == 0 ||
        packet->stream_id < 0 || ctx->finalized)
        return TURBO_EINVAL;
    entry = (mkv_muxer_stream_t **)turbo_vec_at(
        &ctx->streams, (size_t)packet->stream_id);
    if (!entry || !*entry) return TURBO_EINVAL;
    result = mkv_convert_sample(*entry, packet->data, packet->size, &data, &size);
    if (result != TURBO_OK) return result;
    flags = packet->is_keyframe ? MKV_FLAGS_KEYFRAME : 0;
    result = mkv_writer_write(ctx->writer, (*entry)->track, data, size,
                              packet->pts / 1000, packet->dts / 1000, flags);
    return result == TURBO_OK ? ctx->io.error : result;
}

static int mkv_muxer_write_trailer_impl(void *ctx_ptr) {
    return mkv_muxer_finalize((mkv_muxer_ctx_t *)ctx_ptr);
}

static int mkv_muxer_get_data_impl(void *ctx_ptr, uint8_t **data, size_t *size) {
    mkv_muxer_ctx_t *ctx = (mkv_muxer_ctx_t *)ctx_ptr;
    return ctx ? turbo_container_io_get_memory(&ctx->io, data, size)
               : TURBO_EINVAL;
}

static const char *s_mkv_extensions[] = {".mkv", ".mka", NULL};
static const char *s_webm_extensions[] = {".webm", ".weba", NULL};

const turbo_muxer_ops_t turbo_mkv_muxer_ops = {
    .name = "mkv", .format = TURBO_MUXER_MKV, .extensions = s_mkv_extensions,
    .create = mkv_muxer_create_impl, .destroy = mkv_muxer_destroy_impl,
    .add_stream = mkv_muxer_add_stream_impl,
    .write_header = mkv_muxer_write_header_impl,
    .write_packet = mkv_muxer_write_packet_impl,
    .write_trailer = mkv_muxer_write_trailer_impl,
    .get_data = mkv_muxer_get_data_impl,
};

const turbo_muxer_ops_t turbo_webm_muxer_ops = {
    .name = "webm", .format = TURBO_MUXER_WEBM, .extensions = s_webm_extensions,
    .create = mkv_muxer_create_impl, .destroy = mkv_muxer_destroy_impl,
    .add_stream = mkv_muxer_add_stream_impl,
    .write_header = mkv_muxer_write_header_impl,
    .write_packet = mkv_muxer_write_packet_impl,
    .write_trailer = mkv_muxer_write_trailer_impl,
    .get_data = mkv_muxer_get_data_impl,
};

#endif
