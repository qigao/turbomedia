/**
 * MPEG-TS/PS muxers using FFmpeg container algorithms and Salts I/O.
 */
#include "turbo_muxer.h"

#ifdef TURBO_MEDIA_HAS_MPEG

#include "container_io.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <salts_error.h>
#include <salts_vstr.h>

enum { MPEG_AVIO_BUFFER_SIZE = 32 * 1024 };

typedef struct {
    turbo_container_io_t io;
    AVFormatContext *format;
    AVIOContext *avio;
    int header_written;
    int trailer_written;
} mpeg_muxer_ctx_t;

static int mpeg_avio_write(void *opaque, const uint8_t *buffer, int bytes) {
    mpeg_muxer_ctx_t *ctx = (mpeg_muxer_ctx_t *)opaque;
    if (!ctx || !buffer || bytes <= 0 ||
        turbo_container_io_write(&ctx->io, buffer, (size_t)bytes) != SALTS_OK)
        return AVERROR(EIO);
    return bytes;
}

static int64_t mpeg_avio_seek(void *opaque, int64_t offset, int whence) {
    mpeg_muxer_ctx_t *ctx = (mpeg_muxer_ctx_t *)opaque;
    int64_t size;
    int64_t current;
    int64_t target;
    int origin = whence & ~AVSEEK_FORCE;

    if (!ctx) return AVERROR(EINVAL);
    if (origin == AVSEEK_SIZE) {
        return turbo_container_io_size(&ctx->io);
    }
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END)
        return AVERROR(EINVAL);
    if (offset == INT64_MIN) return AVERROR(EINVAL);
    current = turbo_container_io_tell(&ctx->io);
    size = turbo_container_io_size(&ctx->io);
    if (current < 0 || size < 0) return AVERROR(EIO);
    if ((origin == SEEK_CUR &&
         ((offset > 0 && current > INT64_MAX - offset) ||
          (offset < 0 && current < -offset))) ||
        (origin == SEEK_END &&
         ((offset > 0 && size > INT64_MAX - offset) ||
          (offset < 0 && size < -offset))))
        return AVERROR(EINVAL);
    target = origin == SEEK_SET ? offset
                                : origin == SEEK_CUR ? current + offset : size + offset;
    if (target < 0 || turbo_container_io_seek(&ctx->io, target) != SALTS_OK)
        return AVERROR(EIO);
    return turbo_container_io_tell(&ctx->io);
}

static int mpeg_name_equals(const char *actual, const char *expected) {
    return actual && expected &&
           vstr_ieq(vstr_from_cstr(actual), vstr_from_cstr(expected));
}

static enum AVCodecID mpeg_codec_id(const turbo_stream_info_t *info) {
    if (!info || !info->codec_name) return AV_CODEC_ID_NONE;
    if (info->type == TURBO_CODEC_TYPE_VIDEO) {
        if (mpeg_name_equals(info->codec_name, "h264") ||
            mpeg_name_equals(info->codec_name, "avc"))
            return AV_CODEC_ID_H264;
        if (mpeg_name_equals(info->codec_name, "h265") ||
            mpeg_name_equals(info->codec_name, "hevc"))
            return AV_CODEC_ID_HEVC;
        if (mpeg_name_equals(info->codec_name, "h266") ||
            mpeg_name_equals(info->codec_name, "vvc"))
            return AV_CODEC_ID_VVC;
        if (mpeg_name_equals(info->codec_name, "mpeg2video"))
            return AV_CODEC_ID_MPEG2VIDEO;
        if (mpeg_name_equals(info->codec_name, "mpeg4")) return AV_CODEC_ID_MPEG4;
    } else if (info->type == TURBO_CODEC_TYPE_AUDIO) {
        if (mpeg_name_equals(info->codec_name, "aac")) return AV_CODEC_ID_AAC;
        if (mpeg_name_equals(info->codec_name, "mp3")) return AV_CODEC_ID_MP3;
        if (mpeg_name_equals(info->codec_name, "ac3")) return AV_CODEC_ID_AC3;
        if (mpeg_name_equals(info->codec_name, "eac3")) return AV_CODEC_ID_EAC3;
        if (mpeg_name_equals(info->codec_name, "pcma") ||
            mpeg_name_equals(info->codec_name, "g711a"))
            return AV_CODEC_ID_PCM_ALAW;
        if (mpeg_name_equals(info->codec_name, "pcmu") ||
            mpeg_name_equals(info->codec_name, "g711u"))
            return AV_CODEC_ID_PCM_MULAW;
        if (mpeg_name_equals(info->codec_name, "opus")) return AV_CODEC_ID_OPUS;
    }
    return AV_CODEC_ID_NONE;
}

static void mpeg_muxer_destroy_impl(void *ctx_ptr) {
    mpeg_muxer_ctx_t *ctx = (mpeg_muxer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    if (ctx->format) {
        ctx->format->pb = NULL;
        avformat_free_context(ctx->format);
    }
    if (ctx->avio) {
        av_freep(&ctx->avio->buffer);
        avio_context_free(&ctx->avio);
    }
    turbo_container_io_close(&ctx->io);
    free(ctx);
}

static void *mpeg_muxer_create(const turbo_muxer_config_t *config,
                               const char *format_name) {
    mpeg_muxer_ctx_t *ctx;
    uint8_t *avio_buffer;

    if (!config || !format_name) return NULL;
    ctx = (mpeg_muxer_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    if (turbo_container_io_open_writer(&ctx->io, config->output_path) != SALTS_OK ||
        avformat_alloc_output_context2(&ctx->format, NULL, format_name, NULL) < 0 ||
        !ctx->format) {
        mpeg_muxer_destroy_impl(ctx);
        return NULL;
    }

    avio_buffer = (uint8_t *)av_malloc(MPEG_AVIO_BUFFER_SIZE);
    if (!avio_buffer) {
        mpeg_muxer_destroy_impl(ctx);
        return NULL;
    }
    ctx->avio = avio_alloc_context(avio_buffer, MPEG_AVIO_BUFFER_SIZE, 1, ctx, NULL,
                                   mpeg_avio_write, mpeg_avio_seek);
    if (!ctx->avio) {
        av_free(avio_buffer);
        mpeg_muxer_destroy_impl(ctx);
        return NULL;
    }
    ctx->format->pb = ctx->avio;
    ctx->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    return ctx;
}

static void *mpegts_muxer_create(const turbo_muxer_config_t *config) {
    return config && config->format == TURBO_MUXER_MPEG_TS
               ? mpeg_muxer_create(config, "mpegts")
               : NULL;
}

static void *mpegps_muxer_create(const turbo_muxer_config_t *config) {
    return config && config->format == TURBO_MUXER_MPEG_PS
               ? mpeg_muxer_create(config, "mpeg")
               : NULL;
}

static int mpeg_muxer_add_stream_impl(void *ctx_ptr,
                                      const turbo_stream_info_t *info,
                                      int *stream_id) {
    mpeg_muxer_ctx_t *ctx = (mpeg_muxer_ctx_t *)ctx_ptr;
    enum AVCodecID codec_id;
    AVCodecParameters *parameters;
    AVStream *stream;
    int codec_support;

    if (!ctx || !info || !stream_id || ctx->header_written) return -EINVAL;
    codec_id = mpeg_codec_id(info);
    if (codec_id == AV_CODEC_ID_NONE) return -ENOTSUP;
    codec_support = avformat_query_codec(ctx->format->oformat, codec_id,
                                         FF_COMPLIANCE_NORMAL);
    if (codec_support == 0) return -ENOTSUP;

    stream = avformat_new_stream(ctx->format, NULL);
    if (!stream) return -ENOMEM;
    parameters = stream->codecpar;
    parameters->codec_id = codec_id;
    parameters->codec_type = info->type == TURBO_CODEC_TYPE_VIDEO
                                 ? AVMEDIA_TYPE_VIDEO
                                 : AVMEDIA_TYPE_AUDIO;
    if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (info->width <= 0 || info->height <= 0) return -EINVAL;
        parameters->width = info->width;
        parameters->height = info->height;
    } else {
        if (info->sample_rate <= 0 || info->channels <= 0) return -EINVAL;
        parameters->sample_rate = info->sample_rate;
        av_channel_layout_default(&parameters->ch_layout, info->channels);
    }

    if (info->extradata_size > 0) {
        if (!info->extradata || info->extradata_size > INT_MAX) return -EINVAL;
        parameters->extradata =
            (uint8_t *)av_mallocz(info->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!parameters->extradata) return -ENOMEM;
        memcpy(parameters->extradata, info->extradata, info->extradata_size);
        parameters->extradata_size = (int)info->extradata_size;
    }
    stream->time_base = (AVRational){1, AV_TIME_BASE};
    *stream_id = stream->index;
    return 0;
}

static int mpeg_muxer_write_header_impl(void *ctx_ptr) {
    mpeg_muxer_ctx_t *ctx = (mpeg_muxer_ctx_t *)ctx_ptr;
    if (!ctx || ctx->header_written || ctx->format->nb_streams == 0) return -EINVAL;
    if (avformat_write_header(ctx->format, NULL) < 0) return -EIO;
    ctx->header_written = 1;
    return 0;
}

static int mpeg_muxer_write_packet_impl(void *ctx_ptr,
                                        const turbo_muxer_packet_t *packet) {
    static const AVRational microseconds = {1, AV_TIME_BASE};
    mpeg_muxer_ctx_t *ctx = (mpeg_muxer_ctx_t *)ctx_ptr;
    AVPacket *av_packet;
    AVStream *stream;
    int result;

    if (!ctx || !packet || !ctx->header_written || ctx->trailer_written ||
        !packet->data || packet->size == 0 || packet->size > INT_MAX ||
        packet->stream_id < 0 || packet->stream_id >= (int)ctx->format->nb_streams)
        return -EINVAL;
    stream = ctx->format->streams[packet->stream_id];
    av_packet = av_packet_alloc();
    if (!av_packet) return -ENOMEM;
    result = av_new_packet(av_packet, (int)packet->size);
    if (result < 0) {
        av_packet_free(&av_packet);
        return -ENOMEM;
    }
    memcpy(av_packet->data, packet->data, packet->size);
    av_packet->stream_index = packet->stream_id;
    av_packet->pts = packet->pts == INT64_MIN
                         ? AV_NOPTS_VALUE
                         : av_rescale_q(packet->pts, microseconds, stream->time_base);
    av_packet->dts = packet->dts == INT64_MIN
                         ? AV_NOPTS_VALUE
                         : av_rescale_q(packet->dts, microseconds, stream->time_base);
    av_packet->duration =
        packet->duration > 0
            ? av_rescale_q(packet->duration, microseconds, stream->time_base)
            : 0;
    if (packet->is_keyframe) av_packet->flags |= AV_PKT_FLAG_KEY;
    result = av_interleaved_write_frame(ctx->format, av_packet);
    av_packet_free(&av_packet);
    return result < 0 ? -EIO : 0;
}

static int mpeg_muxer_write_trailer_impl(void *ctx_ptr) {
    mpeg_muxer_ctx_t *ctx = (mpeg_muxer_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->header_written || ctx->trailer_written) return -EINVAL;
    if (av_write_trailer(ctx->format) < 0) return -EIO;
    avio_flush(ctx->avio);
    if (turbo_container_io_flush(&ctx->io) != SALTS_OK) return -EIO;
    ctx->trailer_written = 1;
    return 0;
}

static int mpeg_muxer_get_data_impl(void *ctx_ptr, uint8_t **data, size_t *size) {
    mpeg_muxer_ctx_t *ctx = (mpeg_muxer_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->trailer_written) return -EINVAL;
    return turbo_container_io_get_memory(&ctx->io, data, size);
}

static const char *mpegts_extensions[] = {".ts", ".m2ts", NULL};
static const char *mpegps_extensions[] = {".ps", ".mpg", ".mpeg", ".vob", NULL};

const turbo_muxer_ops_t turbo_mpegts_muxer_ops = {
    .name = "mpegts",
    .format = TURBO_MUXER_MPEG_TS,
    .extensions = mpegts_extensions,
    .create = mpegts_muxer_create,
    .destroy = mpeg_muxer_destroy_impl,
    .add_stream = mpeg_muxer_add_stream_impl,
    .write_header = mpeg_muxer_write_header_impl,
    .write_packet = mpeg_muxer_write_packet_impl,
    .write_trailer = mpeg_muxer_write_trailer_impl,
    .get_data = mpeg_muxer_get_data_impl
};

const turbo_muxer_ops_t turbo_mpegps_muxer_ops = {
    .name = "mpegps",
    .format = TURBO_MUXER_MPEG_PS,
    .extensions = mpegps_extensions,
    .create = mpegps_muxer_create,
    .destroy = mpeg_muxer_destroy_impl,
    .add_stream = mpeg_muxer_add_stream_impl,
    .write_header = mpeg_muxer_write_header_impl,
    .write_packet = mpeg_muxer_write_packet_impl,
    .write_trailer = mpeg_muxer_write_trailer_impl,
    .get_data = mpeg_muxer_get_data_impl
};

#endif
