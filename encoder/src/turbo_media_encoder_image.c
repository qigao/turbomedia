#include "turbo_media_encoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static enum AVCodecID tm_encoder_codec_id(uint32_t format) {
    switch (format) {
    case TM_ENCODER_IMAGE_PNG: return AV_CODEC_ID_PNG;
    case TM_ENCODER_IMAGE_JPEG: return AV_CODEC_ID_MJPEG;
    case TM_ENCODER_IMAGE_WEBP: return AV_CODEC_ID_WEBP;
    default: return AV_CODEC_ID_NONE;
    }
}

static enum AVPixelFormat tm_encoder_source_format(uint32_t format,
                                                    unsigned *channels) {
    switch (format) {
    case TM_ENCODER_PIXEL_GRAY8:
        *channels = 1u; return AV_PIX_FMT_GRAY8;
    case TM_ENCODER_PIXEL_GRAY_ALPHA8:
        *channels = 2u; return AV_PIX_FMT_YA8;
    case TM_ENCODER_PIXEL_RGB24:
        *channels = 3u; return AV_PIX_FMT_RGB24;
    case TM_ENCODER_PIXEL_RGBA32:
        *channels = 4u; return AV_PIX_FMT_RGBA;
    default: return AV_PIX_FMT_NONE;
    }
}

static int tm_encoder_format_is_supported(const AVCodec *codec,
                                          enum AVPixelFormat format) {
    const enum AVPixelFormat *candidate;
    if (!codec || !codec->pix_fmts)
        return 0;
    for (candidate = codec->pix_fmts; *candidate != AV_PIX_FMT_NONE; ++candidate) {
        if (*candidate == format)
            return 1;
    }
    return 0;
}

static enum AVPixelFormat tm_encoder_output_pixel_format(
    const AVCodec *codec, uint32_t output_format, enum AVPixelFormat input) {
    static const enum AVPixelFormat lossless[] = {
        AV_PIX_FMT_RGB24, AV_PIX_FMT_RGBA, AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YA8, AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat lossy[] = {
        AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_RGB24, AV_PIX_FMT_NONE
    };
    const enum AVPixelFormat *preferred =
        output_format == TM_ENCODER_IMAGE_PNG ? lossless : lossy;
    const enum AVPixelFormat *candidate;

    if (output_format == TM_ENCODER_IMAGE_PNG &&
        tm_encoder_format_is_supported(codec, input))
        return input;
    for (candidate = preferred; *candidate != AV_PIX_FMT_NONE; ++candidate) {
        if (tm_encoder_format_is_supported(codec, *candidate))
            return *candidate;
    }
    if (tm_encoder_format_is_supported(codec, input))
        return input;
    return AV_PIX_FMT_NONE;
}

typedef struct tm_encoder_sink_state {
    tm_encoder_write_fn write;
    void *user;
    size_t max_bytes;
    size_t written;
    int packets;
} tm_encoder_sink_state;

static tm_encoder_status tm_encoder_drain(AVCodecContext *context,
                                          AVPacket *packet,
                                          tm_encoder_sink_state *sink) {
    int result;
    for (;;) {
        result = avcodec_receive_packet(context, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return TM_ENCODER_OK;
        if (result < 0)
            return TM_ENCODER_ECODEC;
        if (!packet->data || packet->size <= 0) {
            av_packet_unref(packet);
            return TM_ENCODER_ECODEC;
        }
        if ((size_t)packet->size > sink->max_bytes - sink->written) {
            av_packet_unref(packet);
            return TM_ENCODER_ECAPACITY;
        }
        if (sink->write(sink->user, packet->data, (size_t)packet->size) != 0) {
            av_packet_unref(packet);
            return TM_ENCODER_ESINK;
        }
        sink->written += (size_t)packet->size;
        sink->packets++;
        av_packet_unref(packet);
    }
}

int tm_encoder_image_available(uint32_t output_format) {
    enum AVCodecID codec_id = tm_encoder_codec_id(output_format);
    return codec_id != AV_CODEC_ID_NONE &&
           avcodec_find_encoder(codec_id) != NULL;
}

tm_encoder_status tm_encoder_image_encode(
    const tm_encoder_image_v1 *options,
    tm_encoder_write_fn write,
    void *user,
    size_t *written) {
    const AVCodec *codec;
    AVCodecContext *context = NULL;
    AVFrame *frame = NULL;
    AVPacket *packet = NULL;
    struct SwsContext *scale = NULL;
    enum AVCodecID codec_id;
    enum AVPixelFormat input_format, output_format;
    tm_encoder_sink_state sink;
    tm_encoder_status status = TM_ENCODER_EINVAL;
    unsigned channels = 0;
    size_t min_stride, last_row;
    int codec_result;
    unsigned y;

    if (written)
        *written = 0;
    if (!options || !write || !written)
        return TM_ENCODER_EINVAL;
    if (options->size != sizeof(*options) ||
        options->version != TM_ENCODER_IMAGE_V1)
        return TM_ENCODER_EVERSION;
    codec_id = tm_encoder_codec_id(options->output_format);
    input_format = tm_encoder_source_format(options->pixel_format, &channels);
    if (codec_id == AV_CODEC_ID_NONE || input_format == AV_PIX_FMT_NONE ||
        !options->pixels || !options->max_output_bytes || options->reserved ||
        !options->width || !options->height ||
        options->width > TM_ENCODER_MAX_IMAGE_SIDE ||
        options->height > TM_ENCODER_MAX_IMAGE_SIDE ||
        options->quality < 1u || options->quality > 100u)
        return TM_ENCODER_EINVAL;
    /* Never imply metadata preservation if the selected encoder cannot
     * reproduce the exact requested container metadata contract. */
    if (options->metadata_bytes)
        return TM_ENCODER_EUNSUPPORTED;

    min_stride = (size_t)options->width * channels;
    if (min_stride > UINT32_MAX ||
        options->stride_bytes < min_stride ||
        options->stride_bytes > INT_MAX ||
        (size_t)(options->height - 1u) >
            (SIZE_MAX - min_stride) / (size_t)options->stride_bytes)
        return TM_ENCODER_EINVAL;
    last_row = (size_t)(options->height - 1u) *
               (size_t)options->stride_bytes + min_stride;
    if (options->pixels_capacity < last_row)
        return TM_ENCODER_EINVAL;

    codec = avcodec_find_encoder(codec_id);
    if (!codec)
        return TM_ENCODER_EUNSUPPORTED;
    output_format = tm_encoder_output_pixel_format(
        codec, options->output_format, input_format);
    if (output_format == AV_PIX_FMT_NONE)
        return TM_ENCODER_EUNSUPPORTED;

    context = avcodec_alloc_context3(codec);
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!context || !frame || !packet) {
        status = TM_ENCODER_ENOMEM;
        goto cleanup;
    }
    context->width = (int)options->width;
    context->height = (int)options->height;
    context->pix_fmt = output_format;
    context->time_base = (AVRational){1, 1};
    context->thread_count = 1;
    if (options->output_format != TM_ENCODER_IMAGE_PNG) {
        /* A controlled quality parameter for lossy still-frame codecs. */
        int q = 2 + (int)((100u - options->quality) * 29u / 99u);
        context->flags |= AV_CODEC_FLAG_QSCALE;
        context->global_quality = q * FF_QP2LAMBDA;
    }
    if (avcodec_open2(context, codec, NULL) < 0) {
        status = TM_ENCODER_ECODEC;
        goto cleanup;
    }

    frame->format = (int)output_format;
    frame->width = (int)options->width;
    frame->height = (int)options->height;
    frame->pts = 0;
    if (av_frame_get_buffer(frame, 32) < 0 ||
        av_frame_make_writable(frame) < 0) {
        status = TM_ENCODER_ENOMEM;
        goto cleanup;
    }
    if (input_format == output_format) {
        for (y = 0; y < options->height; ++y) {
            memcpy(frame->data[0] + (size_t)y * frame->linesize[0],
                   options->pixels + (size_t)y * options->stride_bytes,
                   min_stride);
        }
    } else {
        const uint8_t *planes[4] = {options->pixels, NULL, NULL, NULL};
        const int stride[4] = {(int)options->stride_bytes, 0, 0, 0};
        scale = sws_getContext((int)options->width, (int)options->height,
                               input_format, (int)options->width,
                               (int)options->height, output_format,
                               SWS_BILINEAR, NULL, NULL, NULL);
        if (!scale || sws_scale(scale, planes, stride, 0,
                                (int)options->height, frame->data,
                                frame->linesize) != (int)options->height) {
            status = TM_ENCODER_ECODEC;
            goto cleanup;
        }
    }
    sink = (tm_encoder_sink_state){
        write, user, options->max_output_bytes, 0u, 0
    };
    codec_result = avcodec_send_frame(context, frame);
    if (codec_result < 0) {
        status = TM_ENCODER_ECODEC;
        goto cleanup;
    }
    status = tm_encoder_drain(context, packet, &sink);
    if (status != TM_ENCODER_OK)
        goto cleanup;
    codec_result = avcodec_send_frame(context, NULL);
    if (codec_result < 0 && codec_result != AVERROR_EOF) {
        status = TM_ENCODER_ECODEC;
        goto cleanup;
    }
    status = tm_encoder_drain(context, packet, &sink);
    if (status != TM_ENCODER_OK)
        goto cleanup;
    if (sink.packets < 1) {
        status = TM_ENCODER_ECODEC;
        goto cleanup;
    }
    *written = sink.written;
    status = TM_ENCODER_OK;

cleanup:
    sws_freeContext(scale);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
    return status;
}
