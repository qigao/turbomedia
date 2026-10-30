#include "turbo_media_encoder.h"
#include "turbo_codec.h"
#include "turbo_muxer.h"

#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* This is a single native media owner on the calling thread:
 * sd raw frame -> FFmpeg swscale I420 -> TurboMedia::Codec VP8 ->
 * TurboMedia::Muxer WebM -> bounded publication callback.
 *
 * No media encoding fallback and no independent libvpx or WebM implementation
 * in sd. TurboMedia's memory-backed WebM writer is byte-bounded; native
 * libvpx/swscale working sets are not a full process-memory quota.
 */

static enum AVPixelFormat tm_video_input_format(uint32_t kind, unsigned *channels) {
    switch (kind) {
    case TM_ENCODER_PIXEL_GRAY8:
        *channels = 1u;
        return AV_PIX_FMT_GRAY8;
    case TM_ENCODER_PIXEL_RGB24:
        *channels = 3u;
        return AV_PIX_FMT_RGB24;
    case TM_ENCODER_PIXEL_RGBA32:
        *channels = 4u;
        return AV_PIX_FMT_RGBA;
    default:
        return AV_PIX_FMT_NONE;
    }
}

tm_encoder_status tm_encoder_webm_encode(
    const tm_encoder_video_v1 *options,
    tm_encoder_write_fn write, void *write_user,
    tm_encoder_should_cancel_fn cancelled, void *cancel_user,
    size_t *written) {
    tm_encoder_status status = TM_ENCODER_EINVAL;
    turbo_video_codec_config_t codec_settings = {0};
    turbo_codec_t *codec = NULL;
    turbo_muxer_t *muxer = NULL;
    turbo_muxer_config_t muxer_settings = {0};
    turbo_stream_info_t stream = {0};
    turbo_muxer_packet_t packet = {0};
    turbo_encoded_frame_t frame_info = {0};
    struct SwsContext *scale = NULL;
    uint8_t *yuv = NULL, *compressed = NULL, *container = NULL;
    size_t pixel_count, yuv_size, packet_capacity, frame_index, container_size = 0;
    int video_track = -1;
    int width, height;

    if (written)
        *written = 0;
    if (!options || !write || !written)
        return TM_ENCODER_EINVAL;
    if (options->size != sizeof(*options) ||
        options->version != TM_ENCODER_VIDEO_V1)
        return TM_ENCODER_EVERSION;
    if (options->audio_bytes || options->audio_pcm)
        return TM_ENCODER_EUNSUPPORTED;  /* no silent soundtrack loss */
    if (!options->frames || options->reserved ||
        options->width < 2u || options->height < 2u ||
        options->width > TURBO_VIDEO_MAX_WIDTH ||
        options->height > TURBO_VIDEO_MAX_HEIGHT ||
        (options->width & 1u) || (options->height & 1u) ||
        options->fps == 0 || options->fps > 240u ||
        options->bitrate > 50000000u ||
        options->frame_count == 0 || options->frame_count > 600u ||
        options->max_output_bytes == 0u)
        return TM_ENCODER_EINVAL;

    width = (int)options->width;
    height = (int)options->height;
    pixel_count = (size_t)width * (size_t)height;
    yuv_size = pixel_count + pixel_count / 2u;
    if (pixel_count > (SIZE_MAX - 65536u) / 3u)
        return TM_ENCODER_ECAPACITY;
    packet_capacity = pixel_count * 3u + 65536u;
    if (packet_capacity > options->max_output_bytes)
        packet_capacity = options->max_output_bytes;
    if (packet_capacity < 4)
        return TM_ENCODER_ECAPACITY;

    /* The built-in Registry is process-global. Do not shut it down here:
     * other TurboMedia Codec/Muxer users might still have active contexts.
     */
    turbo_codec_registry_init();
    turbo_muxer_registry_init();

    codec_settings.width = width;
    codec_settings.height = height;
    codec_settings.framerate = (int)options->fps;
    codec_settings.bitrate = options->bitrate ? (int)options->bitrate : 1000000;
    codec_settings.keyframe_interval = (int)options->fps;
    codec_settings.threads = 1;
    codec_settings.quality = 20;
    codec = turbo_codec_create_encoder("vp8", &codec_settings);
    if (!codec) {
        status = TM_ENCODER_EUNSUPPORTED;
        goto cleanup;
    }

    muxer_settings.format = TURBO_MUXER_WEBM;
    muxer_settings.output_path = NULL;
    muxer_settings.write_duration = 1;
    muxer = turbo_muxer_create(&muxer_settings);
    if (!muxer) {
        status = TM_ENCODER_ECODEC;
        goto cleanup;
    }
    // Enforce the cap inside the existing TurboMedia WebM memory writer.
    if (turbo_muxer_set_memory_limit(muxer, options->max_output_bytes) != 0) {
        status = TM_ENCODER_ECAPACITY;
        goto cleanup;
    }
    stream.type = TURBO_CODEC_TYPE_VIDEO;
    stream.codec_name = "vp8";
    stream.width = width;
    stream.height = height;
    stream.framerate = (int)options->fps;
    if (turbo_muxer_add_stream(muxer, &stream, &video_track) != 0 ||
        turbo_muxer_write_header(muxer) != 0) {
        status = TM_ENCODER_ECODEC;
        goto cleanup;
    }

    yuv = (uint8_t *)malloc(yuv_size);
    compressed = (uint8_t *)malloc(packet_capacity);
    if (!yuv || !compressed) {
        status = TM_ENCODER_ENOMEM;
        goto cleanup;
    }

    for (frame_index = 0; frame_index < options->frame_count; ++frame_index) {
        const tm_encoder_video_frame_v1 *frame = &options->frames[frame_index];
        enum AVPixelFormat input_format;
        unsigned channels = 0;
        size_t minimum_stride, last_byte, encoded_len = packet_capacity;
        int src_stride[4] = {0, 0, 0, 0};
        int dst_stride[4] = {width, width / 2, width / 2, 0};
        const uint8_t *src_planes[4] = {NULL, NULL, NULL, NULL};
        uint8_t *dst_planes[4] = {yuv, yuv + pixel_count,
                                  yuv + pixel_count + pixel_count / 4u, NULL};
        int encoded_status;

        if (cancelled && cancelled(cancel_user)) {
            status = TM_ENCODER_ECANCELLED;
            goto cleanup;
        }
        if (frame->size != sizeof(*frame) ||
            frame->version != TM_ENCODER_VIDEO_V1 ||
            !frame->pixels) {
            status = TM_ENCODER_EVERSION;
            goto cleanup;
        }
        input_format = tm_video_input_format(frame->pixel_format, &channels);
        if (input_format == AV_PIX_FMT_NONE) {
            status = TM_ENCODER_EUNSUPPORTED;
            goto cleanup;
        }
        minimum_stride = pixel_count / (size_t)height * (size_t)channels;
        if (frame->stride_bytes < minimum_stride ||
            frame->stride_bytes > INT_MAX ||
            (size_t)(height - 1) > (SIZE_MAX - minimum_stride) /
                                       (size_t)frame->stride_bytes) {
            status = TM_ENCODER_EINVAL;
            goto cleanup;
        }
        last_byte = (size_t)(height - 1) * (size_t)frame->stride_bytes +
                    minimum_stride;
        if (last_byte > frame->pixels_capacity) {
            status = TM_ENCODER_EINVAL;
            goto cleanup;
        }

        scale = sws_getCachedContext(scale, width, height, input_format,
                                     width, height, AV_PIX_FMT_YUV420P,
                                     SWS_BILINEAR, NULL, NULL, NULL);
        if (!scale) {
            status = TM_ENCODER_ECODEC;
            goto cleanup;
        }
        src_planes[0] = frame->pixels;
        src_stride[0] = (int)frame->stride_bytes;
        if (sws_scale(scale, src_planes, src_stride, 0, height,
                      dst_planes, dst_stride) != height) {
            status = TM_ENCODER_ECODEC;
            goto cleanup;
        }
        encoded_status = turbo_codec_encode(codec, yuv, yuv_size, compressed,
                                             &encoded_len, &frame_info);
        if (encoded_status != TURBO_CODEC_OK || encoded_len == 0u) {
            /* Delayed codec output is not silently dropped/retried in V1. */
            status = encoded_status == TURBO_CODEC_ERR_BUFFER
                         ? TM_ENCODER_ECAPACITY : TM_ENCODER_ECODEC;
            goto cleanup;
        }
        packet.stream_id = video_track;
        packet.data = compressed;
        packet.size = encoded_len;
        packet.pts = (int64_t)(frame_index * 1000000ull / options->fps);
        packet.dts = packet.pts;
        packet.duration = (int64_t)(1000000ull / options->fps);
        packet.is_keyframe = frame_info.is_keyframe;
        if (turbo_muxer_write_packet(muxer, &packet) != 0) {
            status = TM_ENCODER_ECODEC;
            goto cleanup;
        }
    }

    if (cancelled && cancelled(cancel_user)) {
        status = TM_ENCODER_ECANCELLED;
        goto cleanup;
    }
    if (turbo_muxer_write_trailer(muxer) != 0 ||
        turbo_muxer_get_data(muxer, &container, &container_size) != 0 ||
        !container || !container_size) {
        status = TM_ENCODER_ECODEC;
        goto cleanup;
    }
    if (container_size > options->max_output_bytes) {
        status = TM_ENCODER_ECAPACITY;
        goto cleanup;
    }
    if (write(write_user, container, container_size) != 0) {
        status = TM_ENCODER_ESINK;
        goto cleanup;
    }
    *written = container_size;
    status = TM_ENCODER_OK;

cleanup:
    /* Finalize before destroy only on the successful path; the muxer's
     * destroy function retires its own intermediate output state. */
    sws_freeContext(scale);
    free(compressed);
    free(yuv);
    turbo_muxer_destroy(muxer);
    turbo_codec_destroy(codec);
    return status;
}
