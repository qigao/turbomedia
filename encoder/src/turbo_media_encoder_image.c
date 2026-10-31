#include "turbo_media_encoder.h"
#include "turbo_media_webp_xmp.h"

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

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

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

static tm_encoder_status tm_encoder_emit(tm_encoder_sink_state *sink,
                                         const uint8_t *bytes, size_t length) {
    if (length == 0)
        return TM_ENCODER_OK;
    if (!bytes || length > sink->max_bytes - sink->written)
        return TM_ENCODER_ECAPACITY;
    if (sink->write(sink->user, bytes, length) != 0)
        return TM_ENCODER_ESINK;
    sink->written += length;
    return TM_ENCODER_OK;
}

static uint32_t tm_encoder_crc32(const uint8_t *bytes, size_t count,
                                 uint32_t running) {
    size_t i;
    for (i = 0; i < count; ++i) {
        unsigned j;
        running ^= bytes[i];
        for (j = 0; j < 8; ++j)
            running = (running >> 1) ^ ((running & 1u) ? 0xedb88320u : 0u);
    }
    return running;
}

static tm_encoder_status tm_encoder_emit_metadata(
    tm_encoder_sink_state *sink, const uint8_t *payload,
    size_t payload_size, const tm_encoder_image_v1 *image) {
    static const uint8_t png_header[] = {
        0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n'
    };
    static const uint8_t png_type[] = {'t', 'E', 'X', 't'};
    static const uint8_t label[] = "parameters"; /* includes the NUL byte */
    const uint8_t *metadata = (const uint8_t *)image->metadata_utf8;
    tm_encoder_status result;
    size_t extension_size;
    uint8_t marker[4], trailer[4];

    if (image->metadata_bytes == 0)
        return tm_encoder_emit(sink, payload, payload_size);
    if (image->output_format == TM_ENCODER_IMAGE_WEBP) {
        size_t wrapped = 0;
        /* The caller supplies ready-to-store XMP XML. TurboMedia validates
         * and constructs the RIFF/VP8X/XMP container itself, with no second
         * encoder or unbounded metadata buffer. */
        result = tm_encoder_webp_wrap_xmp(
            payload, payload_size, image->width, image->height,
            metadata, image->metadata_bytes,
            sink->max_bytes - sink->written,
            sink->write, sink->user, &wrapped);
        if (result == TM_ENCODER_OK)
            sink->written += wrapped;
        return result;
    }
    if (image->output_format == TM_ENCODER_IMAGE_JPEG) {
        size_t jpeg_length;
        if (payload_size < 4 || payload[0] != 0xff || payload[1] != 0xd8 ||
            image->metadata_bytes > 65535u - (sizeof(label) + 1u + 2u))
            return TM_ENCODER_EINVAL;
        jpeg_length = sizeof(label) + image->metadata_bytes + 1u + 2u;
        extension_size = jpeg_length + 2u; /* marker bytes */
        if (extension_size > sink->max_bytes ||
            payload_size > sink->max_bytes - extension_size)
            return TM_ENCODER_ECAPACITY;
        marker[0] = 0xff;
        marker[1] = 0xfe; /* JPEG COM, matching native sd parameters */
        marker[2] = (uint8_t)(jpeg_length >> 8);
        marker[3] = (uint8_t)jpeg_length;
        if ((result = tm_encoder_emit(sink, payload, 2u)) != TM_ENCODER_OK ||
            (result = tm_encoder_emit(sink, marker, sizeof(marker))) != TM_ENCODER_OK ||
            (result = tm_encoder_emit(sink, label, sizeof(label))) != TM_ENCODER_OK ||
            (result = tm_encoder_emit(sink, metadata, image->metadata_bytes)) != TM_ENCODER_OK)
            return result;
        trailer[0] = 0u;
        if ((result = tm_encoder_emit(sink, trailer, 1u)) != TM_ENCODER_OK)
            return result;
        return tm_encoder_emit(sink, payload + 2u, payload_size - 2u);
    }
    if (image->output_format == TM_ENCODER_IMAGE_PNG) {
        const size_t prefix = 8u + 12u + 13u;
        size_t text_length;
        uint32_t crc = 0xffffffffu;
        if (image->metadata_bytes > UINT32_MAX - sizeof(label))
            return TM_ENCODER_EINVAL;
        text_length = sizeof(label) + image->metadata_bytes;
        if (payload_size < prefix + 12u ||
            memcmp(payload, png_header, sizeof(png_header)) != 0 ||
            payload[8] != 0 || payload[9] != 0 ||
            payload[10] != 0 || payload[11] != 13u ||
            memcmp(payload + 12, "IHDR", 4) != 0 ||
            text_length > UINT32_MAX)
            return TM_ENCODER_EINVAL;
        extension_size = 12u + text_length;
        if (extension_size > sink->max_bytes ||
            payload_size > sink->max_bytes - extension_size)
            return TM_ENCODER_ECAPACITY;
        marker[0] = (uint8_t)(text_length >> 24);
        marker[1] = (uint8_t)(text_length >> 16);
        marker[2] = (uint8_t)(text_length >> 8);
        marker[3] = (uint8_t)text_length;
        crc = tm_encoder_crc32(png_type, sizeof(png_type), crc);
        crc = tm_encoder_crc32(label, sizeof(label), crc);
        crc = tm_encoder_crc32(metadata, image->metadata_bytes, crc) ^ 0xffffffffu;
        trailer[0] = (uint8_t)(crc >> 24);
        trailer[1] = (uint8_t)(crc >> 16);
        trailer[2] = (uint8_t)(crc >> 8);
        trailer[3] = (uint8_t)crc;
        if ((result = tm_encoder_emit(sink, payload, prefix)) != TM_ENCODER_OK ||
            (result = tm_encoder_emit(sink, marker, sizeof(marker))) != TM_ENCODER_OK ||
            (result = tm_encoder_emit(sink, png_type, sizeof(png_type))) != TM_ENCODER_OK ||
            (result = tm_encoder_emit(sink, label, sizeof(label))) != TM_ENCODER_OK ||
            (result = tm_encoder_emit(sink, metadata, image->metadata_bytes)) != TM_ENCODER_OK ||
            (result = tm_encoder_emit(sink, trailer, sizeof(trailer))) != TM_ENCODER_OK)
            return result;
        return tm_encoder_emit(sink, payload + prefix, payload_size - prefix);
    }
    return TM_ENCODER_EUNSUPPORTED;
}

/* PNG/JPEG are owned by one explicitly selected stb encoder inside the
 * TurboMedia::Encoder component. The locked FFmpeg SDK does NOT provide a
 * PNG encoder, so no runtime "try FFmpeg then fallback" behavior is allowed.
 * The C callback cannot report failures, thus collect into a quota-bounded
 * buffer and discard it on error before calling the public sink.
 */
typedef struct tm_stb_buffer {
    uint8_t *bytes;
    size_t length, capacity, limit;
    tm_encoder_status status;
} tm_stb_buffer;

static void tm_stb_write(void *user, void *data, int length) {
    tm_stb_buffer *buffer = (tm_stb_buffer *)user;
    size_t wanted, next_capacity;
    uint8_t *next;
    if (buffer->status != TM_ENCODER_OK)
        return;
    if (length < 0 || (length > 0 && !data)) {
        buffer->status = TM_ENCODER_ECODEC;
        return;
    }
    if (length == 0)
        return;
    if ((size_t)length > buffer->limit - buffer->length) {
        buffer->status = TM_ENCODER_ECAPACITY;
        return;
    }
    wanted = buffer->length + (size_t)length;
    if (wanted > buffer->capacity) {
        next_capacity = buffer->capacity ? buffer->capacity : 4096u;
        if (next_capacity > buffer->limit)
            next_capacity = buffer->limit;
        while (next_capacity < wanted) {
            if (next_capacity > buffer->limit / 2u) {
                next_capacity = buffer->limit;
                break;
            }
            next_capacity *= 2u;
        }
        next = (uint8_t *)realloc(buffer->bytes, next_capacity);
        if (!next) {
            buffer->status = TM_ENCODER_ENOMEM;
            return;
        }
        buffer->bytes = next;
        buffer->capacity = next_capacity;
    }
    memcpy(buffer->bytes + buffer->length, data, (size_t)length);
    buffer->length = wanted;
}

static tm_encoder_status tm_encoder_stb_image(
    const tm_encoder_image_v1 *options, tm_encoder_sink_state *sink,
    size_t *written) {
    tm_stb_buffer buffer = {NULL, 0u, 0u, options->max_output_bytes, TM_ENCODER_OK};
    tm_encoder_status status;
    int ok;
    const int channels =
        options->pixel_format == TM_ENCODER_PIXEL_GRAY8 ? 1 :
        options->pixel_format == TM_ENCODER_PIXEL_GRAY_ALPHA8 ? 2 :
        options->pixel_format == TM_ENCODER_PIXEL_RGB24 ? 3 : 4;
    if (options->output_format == TM_ENCODER_IMAGE_PNG) {
        ok = stbi_write_png_to_func(tm_stb_write, &buffer,
                                    (int)options->width, (int)options->height,
                                    channels, options->pixels,
                                    (int)options->stride_bytes);
    } else {
        /* JPEG API has no stride parameter. Never read past a padded row:
         * reject non-contiguous input rather than copy into an unbounded
         * hidden temporary buffer or silently change pixel ownership.
         */
        if (options->stride_bytes != options->width * (uint32_t)channels)
            return TM_ENCODER_EUNSUPPORTED;
        ok = stbi_write_jpg_to_func(tm_stb_write, &buffer,
                                    (int)options->width, (int)options->height,
                                    channels, options->pixels,
                                    (int)options->quality);
    }
    status = buffer.status != TM_ENCODER_OK ? buffer.status :
             !ok || !buffer.length ? TM_ENCODER_ECODEC :
             tm_encoder_emit_metadata(sink, buffer.bytes, buffer.length,
                                       options);
    if (status == TM_ENCODER_OK)
        *written = sink->written;
    free(buffer.bytes);
    return status;
}

static tm_encoder_status tm_encoder_drain(AVCodecContext *context,
                                          AVPacket *packet,
                                          tm_encoder_sink_state *sink,
                                          const tm_encoder_image_v1 *options) {
    int result;
    for (;;) {
        tm_encoder_status status;
        result = avcodec_receive_packet(context, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return TM_ENCODER_OK;
        if (result < 0)
            return TM_ENCODER_ECODEC;
        if (!packet->data || packet->size <= 0) {
            av_packet_unref(packet);
            return TM_ENCODER_ECODEC;
        }
        if (options->metadata_bytes > 0 && sink->packets != 0) {
            av_packet_unref(packet);
            return TM_ENCODER_ECODEC;
        }
        status = tm_encoder_emit_metadata(sink, packet->data,
                                          (size_t)packet->size, options);
        av_packet_unref(packet);
        if (status != TM_ENCODER_OK)
            return status;
        sink->packets++;
    }
}

int tm_encoder_image_available(uint32_t output_format) {
    if (output_format == TM_ENCODER_IMAGE_PNG ||
        output_format == TM_ENCODER_IMAGE_JPEG)
        return 1; /* one built-in TurboMedia stb provider per format */
    if (output_format == TM_ENCODER_IMAGE_WEBP)
        return avcodec_find_encoder(AV_CODEC_ID_WEBP) != NULL;
    return 0;
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
    /* PNG/JPEG receive the caller-prepared parameters text; WebP receives
     * caller-prepared XMP XML. Reject invalid embedded NUL and absent bytes,
     * but never silently omit metadata or swap to another codec provider. */
    if (options->metadata_bytes &&
        (!options->metadata_utf8 ||
         memchr(options->metadata_utf8, '\0', options->metadata_bytes)))
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

    if (options->output_format == TM_ENCODER_IMAGE_PNG ||
        options->output_format == TM_ENCODER_IMAGE_JPEG) {
        tm_encoder_sink_state output = {
            write, user, options->max_output_bytes, 0u, 0
        };
        return tm_encoder_stb_image(options, &output, written);
    }

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
    status = tm_encoder_drain(context, packet, &sink, options);
    if (status != TM_ENCODER_OK)
        goto cleanup;
    codec_result = avcodec_send_frame(context, NULL);
    if (codec_result < 0 && codec_result != AVERROR_EOF) {
        status = TM_ENCODER_ECODEC;
        goto cleanup;
    }
    status = tm_encoder_drain(context, packet, &sink, options);
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
