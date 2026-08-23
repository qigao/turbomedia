/**
 * MPEG-TS/PS demuxers using FFmpeg container algorithms and TurboUtils I/O.
 */
#include "turbo_demuxer.h"

#ifdef TURBO_MEDIA_HAS_MPEG

#include "container_io.h"
#include "stl_status.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <turbo_error.h>
#include <turbostl/vec.h>

enum {
    MPEG_AVIO_BUFFER_SIZE = 32 * 1024,
    MPEG_TS_PACKET_SIZE = 188,
    MPEG_M2TS_PACKET_SIZE = 192,
    MPEG_M2TS_SYNC_OFFSET = 4
};

typedef struct {
    int av_stream_index;
    turbo_stream_info_t info;
    uint8_t *extra_data;
} mpeg_demuxer_stream_t;

typedef struct {
    turbo_container_io_t io;
    AVFormatContext *format;
    AVIOContext *avio;
    vec_t streams;
    const char *format_name;
    int probe_size;
    int opened;
} mpeg_demuxer_ctx_t;

static int mpeg_avio_read(void *opaque, uint8_t *buffer, int capacity) {
    mpeg_demuxer_ctx_t *ctx = (mpeg_demuxer_ctx_t *)opaque;
    size_t bytes_read = 0;

    if (!ctx || !buffer || capacity <= 0) return AVERROR(EINVAL);
    if (turbo_container_io_read_some(&ctx->io, buffer, (size_t)capacity,
                                     &bytes_read) != TURBO_OK)
        return AVERROR(EIO);
    if (bytes_read == 0) return AVERROR_EOF;
    return (int)bytes_read;
}

static int64_t mpeg_avio_seek(void *opaque, int64_t offset, int whence) {
    mpeg_demuxer_ctx_t *ctx = (mpeg_demuxer_ctx_t *)opaque;
    int origin = whence & ~AVSEEK_FORCE;
    int64_t base;
    int64_t target;

    if (!ctx) return AVERROR(EINVAL);
    if (origin == AVSEEK_SIZE) return turbo_container_io_size(&ctx->io);
    if (origin == SEEK_SET) {
        base = 0;
    } else if (origin == SEEK_CUR) {
        base = turbo_container_io_tell(&ctx->io);
    } else if (origin == SEEK_END) {
        base = turbo_container_io_size(&ctx->io);
    } else {
        return AVERROR(EINVAL);
    }
    if (base < 0 || offset == INT64_MIN ||
        (offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < -offset))
        return AVERROR(EINVAL);
    target = base + offset;
    if (target < 0 || turbo_container_io_seek(&ctx->io, target) != TURBO_OK)
        return AVERROR(EIO);
    return turbo_container_io_tell(&ctx->io);
}

static const char *mpeg_codec_name(enum AVCodecID codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264: return "h264";
        case AV_CODEC_ID_HEVC: return "h265";
        case AV_CODEC_ID_VVC: return "h266";
        case AV_CODEC_ID_MPEG2VIDEO: return "mpeg2video";
        case AV_CODEC_ID_MPEG4: return "mpeg4";
        case AV_CODEC_ID_AAC: return "aac";
        case AV_CODEC_ID_MP3: return "mp3";
        case AV_CODEC_ID_AC3: return "ac3";
        case AV_CODEC_ID_EAC3: return "eac3";
        case AV_CODEC_ID_PCM_ALAW: return "pcma";
        case AV_CODEC_ID_PCM_MULAW: return "pcmu";
        case AV_CODEC_ID_OPUS: return "opus";
        default: return avcodec_get_name(codec_id);
    }
}

static int mpeg_public_stream_index(const mpeg_demuxer_ctx_t *ctx,
                                    int av_stream_index) {
    size_t count = vec_size(&ctx->streams);
    size_t i;
    for (i = 0; i < count; ++i) {
        const mpeg_demuxer_stream_t *stream =
            (const mpeg_demuxer_stream_t *)vec_at_const(&ctx->streams, i);
        if (stream && stream->av_stream_index == av_stream_index) return (int)i;
    }
    return -1;
}

static int mpeg_add_stream(mpeg_demuxer_ctx_t *ctx, AVStream *av_stream) {
    AVCodecParameters *parameters = av_stream->codecpar;
    mpeg_demuxer_stream_t stream;
    int result;

    if (parameters->codec_type != AVMEDIA_TYPE_VIDEO &&
        parameters->codec_type != AVMEDIA_TYPE_AUDIO)
        return TURBO_OK;
    memset(&stream, 0, sizeof(stream));
    stream.av_stream_index = av_stream->index;
    stream.info.stream_id = av_stream->index;
    stream.info.type = parameters->codec_type == AVMEDIA_TYPE_VIDEO
                           ? TURBO_CODEC_TYPE_VIDEO
                           : TURBO_CODEC_TYPE_AUDIO;
    stream.info.codec_name = mpeg_codec_name(parameters->codec_id);
    stream.info.width = parameters->width;
    stream.info.height = parameters->height;
    stream.info.sample_rate = parameters->sample_rate;
    stream.info.channels = parameters->ch_layout.nb_channels;
    if (av_stream->avg_frame_rate.num > 0 && av_stream->avg_frame_rate.den > 0) {
        stream.info.framerate =
            (int)av_rescale_rnd(av_stream->avg_frame_rate.num, 1,
                                av_stream->avg_frame_rate.den, AV_ROUND_NEAR_INF);
    }
    if (parameters->extradata_size > 0) {
        size_t bytes = (size_t)parameters->extradata_size;
        stream.extra_data = (uint8_t *)malloc(bytes);
        if (!stream.extra_data) return TURBO_ENOMEM;
        memcpy(stream.extra_data, parameters->extradata, bytes);
        stream.info.extradata = stream.extra_data;
        stream.info.extradata_size = bytes;
    }
    result = turbo_media_stl_status_to_error(vec_push(&ctx->streams, &stream));
    if (result != TURBO_OK) free(stream.extra_data);
    return result;
}

static void mpeg_demuxer_destroy_impl(void *ctx_ptr) {
    mpeg_demuxer_ctx_t *ctx = (mpeg_demuxer_ctx_t *)ctx_ptr;
    size_t count;
    size_t i;

    if (!ctx) return;
    if (ctx->format) {
        ctx->format->pb = NULL;
        avformat_close_input(&ctx->format);
    }
    if (ctx->avio) {
        av_freep(&ctx->avio->buffer);
        avio_context_free(&ctx->avio);
    }
    count = vec_size(&ctx->streams);
    for (i = 0; i < count; ++i) {
        mpeg_demuxer_stream_t *stream =
            (mpeg_demuxer_stream_t *)vec_at(&ctx->streams, i);
        if (stream) free(stream->extra_data);
    }
    vec_destroy(&ctx->streams);
    turbo_container_io_close(&ctx->io);
    free(ctx);
}

static void *mpeg_demuxer_create(const turbo_demuxer_config_t *config,
                                 const char *format_name) {
    mpeg_demuxer_ctx_t *ctx;
    uint8_t *avio_buffer;

    if (!config || !format_name || (!!config->input_path == !!config->data) ||
        (!config->input_path && config->data_size == 0) || config->probe_size < 0)
        return NULL;
    ctx = (mpeg_demuxer_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->io.file = TURBO_INVALID_FILE;
    ctx->format_name = format_name;
    ctx->probe_size = config->probe_size;
    if (vec_init_bytes(&ctx->streams, sizeof(mpeg_demuxer_stream_t),
                       CMETA_ALIGNOF(mpeg_demuxer_stream_t), SIZE_MAX) != STL_OK ||
        turbo_container_io_open_reader(&ctx->io, config->input_path, config->data,
                                       config->data_size) != TURBO_OK) {
        mpeg_demuxer_destroy_impl(ctx);
        return NULL;
    }
    avio_buffer = (uint8_t *)av_malloc(MPEG_AVIO_BUFFER_SIZE);
    if (!avio_buffer) {
        mpeg_demuxer_destroy_impl(ctx);
        return NULL;
    }
    ctx->avio = avio_alloc_context(avio_buffer, MPEG_AVIO_BUFFER_SIZE, 0, ctx,
                                   mpeg_avio_read, NULL, mpeg_avio_seek);
    if (!ctx->avio) {
        av_free(avio_buffer);
        mpeg_demuxer_destroy_impl(ctx);
        return NULL;
    }
    return ctx;
}

static void *mpegts_demuxer_create(const turbo_demuxer_config_t *config) {
    return mpeg_demuxer_create(config, "mpegts");
}

static void *mpegps_demuxer_create(const turbo_demuxer_config_t *config) {
    return mpeg_demuxer_create(config, "mpeg");
}

static int mpeg_demuxer_open_impl(void *ctx_ptr) {
    mpeg_demuxer_ctx_t *ctx = (mpeg_demuxer_ctx_t *)ctx_ptr;
    const AVInputFormat *input_format;
    size_t i;

    if (!ctx || ctx->opened) return TURBO_EINVAL;
    input_format = av_find_input_format(ctx->format_name);
    if (!input_format) return TURBO_ENOTSUP;
    ctx->format = avformat_alloc_context();
    if (!ctx->format) return TURBO_ENOMEM;
    ctx->format->pb = ctx->avio;
    ctx->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (ctx->probe_size > 0) ctx->format->probesize = ctx->probe_size;
    if (avformat_open_input(&ctx->format, NULL, input_format, NULL) < 0)
        return TURBO_EPROTO;
    if (avformat_find_stream_info(ctx->format, NULL) < 0) return TURBO_EPROTO;
    for (i = 0; i < ctx->format->nb_streams; ++i) {
        int result = mpeg_add_stream(ctx, ctx->format->streams[i]);
        if (result != TURBO_OK) return result;
    }
    if (vec_empty(&ctx->streams)) return TURBO_EPROTO;
    ctx->opened = 1;
    return TURBO_OK;
}

static int mpeg_demuxer_read_packet_impl(void *ctx_ptr,
                                         turbo_demuxer_packet_t *packet) {
    static const AVRational microseconds = {1, AV_TIME_BASE};
    mpeg_demuxer_ctx_t *ctx = (mpeg_demuxer_ctx_t *)ctx_ptr;
    AVPacket *av_packet;
    int result;

    if (!ctx || !ctx->opened || !packet) return TURBO_EINVAL;
    memset(packet, 0, sizeof(*packet));
    av_packet = av_packet_alloc();
    if (!av_packet) return TURBO_ENOMEM;
    for (;;) {
        int public_index;
        AVStream *stream;
        result = av_read_frame(ctx->format, av_packet);
        if (result == AVERROR_EOF) {
            av_packet_free(&av_packet);
            return 0;
        }
        if (result < 0) {
            av_packet_free(&av_packet);
            return TURBO_EIO;
        }
        public_index = mpeg_public_stream_index(ctx, av_packet->stream_index);
        if (public_index < 0) {
            av_packet_unref(av_packet);
            continue;
        }
        if (av_packet->size <= 0 || !av_packet->data) {
            av_packet_unref(av_packet);
            continue;
        }
        stream = ctx->format->streams[av_packet->stream_index];
        packet->data = (uint8_t *)malloc((size_t)av_packet->size);
        if (!packet->data) {
            av_packet_free(&av_packet);
            return TURBO_ENOMEM;
        }
        memcpy(packet->data, av_packet->data, (size_t)av_packet->size);
        packet->size = (size_t)av_packet->size;
        packet->stream_index = public_index;
        packet->pts = av_packet->pts == AV_NOPTS_VALUE
                          ? INT64_MIN
                          : av_rescale_q(av_packet->pts, stream->time_base,
                                         microseconds);
        packet->dts = av_packet->dts == AV_NOPTS_VALUE
                          ? INT64_MIN
                          : av_rescale_q(av_packet->dts, stream->time_base,
                                         microseconds);
        packet->duration = av_packet->duration > 0
                               ? av_rescale_q(av_packet->duration, stream->time_base,
                                              microseconds)
                               : 0;
        packet->is_keyframe = (av_packet->flags & AV_PKT_FLAG_KEY) != 0;
        av_packet_free(&av_packet);
        return 1;
    }
}

static int mpeg_demuxer_seek_impl(void *ctx_ptr, int64_t timestamp_ms, int flags) {
    mpeg_demuxer_ctx_t *ctx = (mpeg_demuxer_ctx_t *)ctx_ptr;
    int av_flags = 0;
    int64_t timestamp;

    if (!ctx || !ctx->opened || timestamp_ms < 0 ||
        timestamp_ms > INT64_MAX / 1000)
        return TURBO_EINVAL;
    if (flags & TURBO_DEMUXER_SEEK_BACKWARD) av_flags |= AVSEEK_FLAG_BACKWARD;
    if (flags & TURBO_DEMUXER_SEEK_ANY) av_flags |= AVSEEK_FLAG_ANY;
    timestamp = timestamp_ms * 1000;
    if (av_seek_frame(ctx->format, -1, timestamp, av_flags) < 0) return TURBO_EIO;
    avformat_flush(ctx->format);
    return TURBO_OK;
}

static int mpeg_demuxer_get_stream_count_impl(void *ctx_ptr) {
    mpeg_demuxer_ctx_t *ctx = (mpeg_demuxer_ctx_t *)ctx_ptr;
    size_t count;
    if (!ctx || !ctx->opened) return TURBO_EINVAL;
    count = vec_size(&ctx->streams);
    return count > INT_MAX ? TURBO_EFBIG : (int)count;
}

static int mpeg_demuxer_get_stream_info_impl(void *ctx_ptr, int stream_index,
                                             turbo_stream_info_t *info) {
    mpeg_demuxer_ctx_t *ctx = (mpeg_demuxer_ctx_t *)ctx_ptr;
    const mpeg_demuxer_stream_t *stream;
    if (!ctx || !ctx->opened || !info || stream_index < 0) return TURBO_EINVAL;
    stream = (const mpeg_demuxer_stream_t *)vec_at_const(
        &ctx->streams, (size_t)stream_index);
    if (!stream) return TURBO_EINVAL;
    *info = stream->info;
    return TURBO_OK;
}

static int mpeg_demuxer_get_metadata_impl(void *ctx_ptr,
                                          turbo_container_metadata_t *metadata) {
    mpeg_demuxer_ctx_t *ctx = (mpeg_demuxer_ctx_t *)ctx_ptr;
    AVDictionaryEntry *entry;

    if (!ctx || !ctx->opened || !metadata) return TURBO_EINVAL;
    memset(metadata, 0, sizeof(*metadata));
    if (ctx->format->duration != AV_NOPTS_VALUE)
        metadata->duration_ms = ctx->format->duration / 1000;
    metadata->bitrate = ctx->format->bit_rate;
    entry = av_dict_get(ctx->format->metadata, "title", NULL, 0);
    if (entry) metadata->title = entry->value;
    entry = av_dict_get(ctx->format->metadata, "artist", NULL, 0);
    if (entry) metadata->author = entry->value;
    entry = av_dict_get(ctx->format->metadata, "copyright", NULL, 0);
    if (entry) metadata->copyright = entry->value;
    entry = av_dict_get(ctx->format->metadata, "comment", NULL, 0);
    if (entry) metadata->comment = entry->value;
    return TURBO_OK;
}

static int mpegts_probe_impl(const uint8_t *data, size_t size) {
    size_t offset;
    if (!data || size < (size_t)MPEG_TS_PACKET_SIZE * 3) return 0;
    for (offset = 0; offset < MPEG_TS_PACKET_SIZE && offset < size; ++offset) {
        if (offset + (size_t)MPEG_TS_PACKET_SIZE * 2 < size && data[offset] == 0x47 &&
            data[offset + MPEG_TS_PACKET_SIZE] == 0x47 &&
            data[offset + MPEG_TS_PACKET_SIZE * 2] == 0x47)
            return 100;
    }
    if (size >= (size_t)MPEG_M2TS_SYNC_OFFSET + MPEG_M2TS_PACKET_SIZE * 3 &&
        data[MPEG_M2TS_SYNC_OFFSET] == 0x47 &&
        data[MPEG_M2TS_SYNC_OFFSET + MPEG_M2TS_PACKET_SIZE] == 0x47 &&
        data[MPEG_M2TS_SYNC_OFFSET + MPEG_M2TS_PACKET_SIZE * 2] == 0x47)
        return 100;
    return 0;
}

static int mpegps_probe_impl(const uint8_t *data, size_t size) {
    size_t i;
    if (!data || size < 4) return 0;
    for (i = 0; i + 4 <= size; ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01 &&
            data[i + 3] == 0xBA)
            return 100;
    }
    return 0;
}

static const char *s_mpegts_extensions[] = {".ts", ".m2ts", NULL};
static const char *s_mpegps_extensions[] = {".ps", ".mpg", ".mpeg", ".vob", NULL};

const turbo_demuxer_ops_t turbo_mpegts_demuxer_ops = {
    .name = "mpegts",
    .extensions = s_mpegts_extensions,
    .create = mpegts_demuxer_create,
    .destroy = mpeg_demuxer_destroy_impl,
    .probe = mpegts_probe_impl,
    .open = mpeg_demuxer_open_impl,
    .read_packet = mpeg_demuxer_read_packet_impl,
    .seek = mpeg_demuxer_seek_impl,
    .get_stream_count = mpeg_demuxer_get_stream_count_impl,
    .get_stream_info = mpeg_demuxer_get_stream_info_impl,
    .get_metadata = mpeg_demuxer_get_metadata_impl,
};

const turbo_demuxer_ops_t turbo_mpegps_demuxer_ops = {
    .name = "mpegps",
    .extensions = s_mpegps_extensions,
    .create = mpegps_demuxer_create,
    .destroy = mpeg_demuxer_destroy_impl,
    .probe = mpegps_probe_impl,
    .open = mpeg_demuxer_open_impl,
    .read_packet = mpeg_demuxer_read_packet_impl,
    .seek = mpeg_demuxer_seek_impl,
    .get_stream_count = mpeg_demuxer_get_stream_count_impl,
    .get_stream_info = mpeg_demuxer_get_stream_info_impl,
    .get_metadata = mpeg_demuxer_get_metadata_impl,
};

#endif
