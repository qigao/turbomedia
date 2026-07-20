#include "turbo_muxer.h"

#ifdef TURBO_MEDIA_HAS_FLV

#include "container_io.h"
#include "flv_muxer_internal.h"
#include "flv_writer.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <turbo_error.h>
#include <turbo_str_view.h>

typedef enum {
    FLV_CODEC_NONE = 0,
    FLV_CODEC_H264,
    FLV_CODEC_H265,
    FLV_CODEC_H266,
    FLV_CODEC_AV1,
    FLV_CODEC_AVS3,
    FLV_CODEC_AAC,
    FLV_CODEC_MP3,
    FLV_CODEC_G711A,
    FLV_CODEC_G711U,
    FLV_CODEC_OPUS,
    FLV_CODEC_AC3,
    FLV_CODEC_EAC3
} flv_codec_t;

typedef struct {
    turbo_container_io_t io;
    flv_muxer_t *flv;
    void *writer;
    int video_stream_id;
    int audio_stream_id;
    flv_codec_t video_codec;
    flv_codec_t audio_codec;
    int has_video;
    int has_audio;
    int header_written;
    int trailer_written;
    int error;
} flv_muxer_ctx_t;

static int flv_name_equals(const char *actual, const char *expected) {
    return actual && expected &&
           tstr_v_ieq(tstr_v_from_cstr(actual), tstr_v_from_cstr(expected));
}

static flv_codec_t flv_video_codec(const char *name) {
    if (flv_name_equals(name, "h264") || flv_name_equals(name, "avc"))
        return FLV_CODEC_H264;
    if (flv_name_equals(name, "h265") || flv_name_equals(name, "hevc"))
        return FLV_CODEC_H265;
    if (flv_name_equals(name, "h266") || flv_name_equals(name, "vvc"))
        return FLV_CODEC_H266;
    if (flv_name_equals(name, "av1")) return FLV_CODEC_AV1;
    if (flv_name_equals(name, "avs3")) return FLV_CODEC_AVS3;
    return FLV_CODEC_NONE;
}

static flv_codec_t flv_audio_codec(const char *name) {
    if (flv_name_equals(name, "aac")) return FLV_CODEC_AAC;
    if (flv_name_equals(name, "mp3")) return FLV_CODEC_MP3;
    if (flv_name_equals(name, "pcma") || flv_name_equals(name, "g711a"))
        return FLV_CODEC_G711A;
    if (flv_name_equals(name, "pcmu") || flv_name_equals(name, "g711u"))
        return FLV_CODEC_G711U;
    if (flv_name_equals(name, "opus")) return FLV_CODEC_OPUS;
    if (flv_name_equals(name, "ac3")) return FLV_CODEC_AC3;
    if (flv_name_equals(name, "eac3")) return FLV_CODEC_EAC3;
    return FLV_CODEC_NONE;
}

static int flv_container_write(void *param, const struct flv_vec_t *vectors,
                               int count) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)param;
    int i;

    if (!ctx || !vectors || count <= 0 || ctx->error != TURBO_OK) return -1;
    for (i = 0; i < count; ++i) {
        if (vectors[i].len < 0 ||
            turbo_container_io_write(&ctx->io, vectors[i].ptr,
                                     (uint64_t)vectors[i].len) != TURBO_OK) {
            ctx->error = ctx->io.error != TURBO_OK ? ctx->io.error : TURBO_EIO;
            return -1;
        }
    }
    return 0;
}

static int flv_tag_write(void *param, int type, const void *data, size_t bytes,
                         uint32_t timestamp) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)param;
    if (!ctx || !ctx->writer || !data || bytes == 0) return -1;
    return flv_writer_input(ctx->writer, type, data, bytes, timestamp);
}

static void flv_muxer_destroy_impl(void *ctx_ptr) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    if (ctx->flv) flv_muxer_destroy(ctx->flv);
    if (ctx->writer) flv_writer_destroy(ctx->writer);
    turbo_container_io_close(&ctx->io);
    free(ctx);
}

static void *flv_muxer_create_impl(const turbo_muxer_config_t *config) {
    flv_muxer_ctx_t *ctx;
    if (!config || config->format != TURBO_MUXER_FLV) return NULL;
    ctx = (flv_muxer_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->io.file = TURBO_INVALID_FILE;
    ctx->video_stream_id = -1;
    ctx->audio_stream_id = -1;
    ctx->error = TURBO_OK;
    if (turbo_container_io_open_writer(&ctx->io, config->output_path) != TURBO_OK) {
        flv_muxer_destroy_impl(ctx);
        return NULL;
    }
    return ctx;
}

static int flv_muxer_add_stream_impl(void *ctx_ptr,
                                     const turbo_stream_info_t *stream_info,
                                     int *stream_id) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    int next_id;

    if (!ctx || !stream_info || !stream_id || ctx->header_written)
        return TURBO_EINVAL;
    next_id = ctx->has_audio + ctx->has_video;
    if (stream_info->type == TURBO_CODEC_TYPE_VIDEO) {
        flv_codec_t codec = flv_video_codec(stream_info->codec_name);
        if (ctx->has_video) return TURBO_EALREADY;
        if (codec == FLV_CODEC_NONE) return TURBO_ENOTSUP;
        if (stream_info->width <= 0 || stream_info->height <= 0)
            return TURBO_EINVAL;
        ctx->has_video = 1;
        ctx->video_stream_id = next_id;
        ctx->video_codec = codec;
    } else if (stream_info->type == TURBO_CODEC_TYPE_AUDIO) {
        flv_codec_t codec = flv_audio_codec(stream_info->codec_name);
        if (ctx->has_audio) return TURBO_EALREADY;
        if (codec == FLV_CODEC_NONE) return TURBO_ENOTSUP;
        if (stream_info->sample_rate <= 0 || stream_info->channels <= 0)
            return TURBO_EINVAL;
        ctx->has_audio = 1;
        ctx->audio_stream_id = next_id;
        ctx->audio_codec = codec;
    } else {
        return TURBO_ENOTSUP;
    }
    *stream_id = next_id;
    return TURBO_OK;
}

static int flv_muxer_write_header_impl(void *ctx_ptr) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    if (!ctx || ctx->header_written || (!ctx->has_audio && !ctx->has_video))
        return TURBO_EINVAL;
    ctx->writer = flv_writer_create2(ctx->has_audio, ctx->has_video,
                                     flv_container_write, ctx);
    if (!ctx->writer) return ctx->error != TURBO_OK ? ctx->error : TURBO_EIO;
    ctx->flv = flv_muxer_create(flv_tag_write, ctx);
    if (!ctx->flv) {
        flv_writer_destroy(ctx->writer);
        ctx->writer = NULL;
        return TURBO_ENOMEM;
    }
    ctx->header_written = 1;
    return TURBO_OK;
}

static int flv_write_video(flv_muxer_ctx_t *ctx, const void *data, size_t bytes,
                           uint32_t pts, uint32_t dts) {
    switch (ctx->video_codec) {
        case FLV_CODEC_H264: return flv_muxer_avc(ctx->flv, data, bytes, pts, dts);
        case FLV_CODEC_H265: return flv_muxer_hevc(ctx->flv, data, bytes, pts, dts);
        case FLV_CODEC_H266: return flv_muxer_vvc(ctx->flv, data, bytes, pts, dts);
        case FLV_CODEC_AV1: return flv_muxer_av1(ctx->flv, data, bytes, pts, dts);
        case FLV_CODEC_AVS3: return flv_muxer_avs3(ctx->flv, data, bytes, pts, dts);
        default: return TURBO_ENOTSUP;
    }
}

static int flv_write_audio(flv_muxer_ctx_t *ctx, const void *data, size_t bytes,
                           uint32_t pts, uint32_t dts) {
    switch (ctx->audio_codec) {
        case FLV_CODEC_AAC: return flv_muxer_aac(ctx->flv, data, bytes, pts, dts);
        case FLV_CODEC_MP3: return flv_muxer_mp3(ctx->flv, data, bytes, pts, dts);
        case FLV_CODEC_G711A: return flv_muxer_g711a(ctx->flv, data, bytes, pts, dts);
        case FLV_CODEC_G711U: return flv_muxer_g711u(ctx->flv, data, bytes, pts, dts);
        case FLV_CODEC_OPUS: return flv_muxer_opus(ctx->flv, data, bytes, pts, dts);
        case FLV_CODEC_AC3: return flv_muxer_ac3(ctx->flv, data, bytes, pts, dts);
        case FLV_CODEC_EAC3: return flv_muxer_eac3(ctx->flv, data, bytes, pts, dts);
        default: return TURBO_ENOTSUP;
    }
}

static int flv_muxer_write_packet_impl(void *ctx_ptr,
                                       const turbo_muxer_packet_t *packet) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    uint64_t pts_ms;
    uint64_t dts_ms;
    int result;

    if (!ctx || !packet || !ctx->header_written || ctx->trailer_written ||
        !packet->data || packet->size == 0 || packet->pts < 0 || packet->dts < 0)
        return TURBO_EINVAL;
    pts_ms = (uint64_t)packet->pts / 1000;
    dts_ms = (uint64_t)packet->dts / 1000;
    if (pts_ms > UINT32_MAX || dts_ms > UINT32_MAX) return TURBO_ERANGE;
    if (packet->stream_id == ctx->video_stream_id) {
        result = flv_write_video(ctx, packet->data, packet->size, (uint32_t)pts_ms,
                                 (uint32_t)dts_ms);
    } else if (packet->stream_id == ctx->audio_stream_id) {
        result = flv_write_audio(ctx, packet->data, packet->size, (uint32_t)pts_ms,
                                 (uint32_t)dts_ms);
    } else {
        return TURBO_EINVAL;
    }
    return result == 0 ? TURBO_OK
                       : ctx->error != TURBO_OK ? ctx->error : TURBO_EPROTO;
}

static int flv_muxer_write_trailer_impl(void *ctx_ptr) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->header_written || ctx->trailer_written) return TURBO_EINVAL;
    flv_muxer_destroy(ctx->flv);
    ctx->flv = NULL;
    flv_writer_destroy(ctx->writer);
    ctx->writer = NULL;
    if (ctx->error != TURBO_OK) return ctx->error;
    if (turbo_container_io_flush(&ctx->io) != TURBO_OK) return ctx->io.error;
    ctx->trailer_written = 1;
    return TURBO_OK;
}

static int flv_muxer_get_data_impl(void *ctx_ptr, uint8_t **data, size_t *size) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->trailer_written) return TURBO_EINVAL;
    return turbo_container_io_get_memory(&ctx->io, data, size);
}

static const char *flv_extensions[] = {".flv", NULL};

const turbo_muxer_ops_t turbo_flv_muxer_ops = {
    .name = "flv",
    .format = TURBO_MUXER_FLV,
    .extensions = flv_extensions,
    .create = flv_muxer_create_impl,
    .destroy = flv_muxer_destroy_impl,
    .add_stream = flv_muxer_add_stream_impl,
    .write_header = flv_muxer_write_header_impl,
    .write_packet = flv_muxer_write_packet_impl,
    .write_trailer = flv_muxer_write_trailer_impl,
    .get_data = flv_muxer_get_data_impl,
};

#endif
