#include "turbo_media_encoder.h"
#include "turbo_codec.h"
#include "turbo_muxer.h"
#include "opus-head.h"

#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include <limits.h>
#include <math.h>
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

typedef struct tm_audio_owner {
    const tm_encoder_audio_v1 *input;
    turbo_codec_t *codec;
    turbo_muxer_t *muxer;
    int track;
    size_t current;
    size_t packets;
    uint64_t tail_discard_ns;
} tm_audio_owner;

static tm_encoder_status tm_audio_preflight(const tm_encoder_video_v1 *video,
                                           const tm_encoder_audio_v1 *audio) {
    size_t total, samples;
    if (!audio || audio->size != sizeof(*audio) ||
        audio->version != TM_ENCODER_AUDIO_V1)
        return TM_ENCODER_EVERSION;
    if (!audio->samples || !audio->sample_count || audio->reserved ||
        audio->channels < 1u || audio->channels > 2u)
        return TM_ENCODER_EINVAL;
    /* No silent SRC or trim: exact 48kHz / full 20ms frames, with source
     * duration matching the requested video frame timeline. */
    if (audio->sample_rate != 48000u)
        return TM_ENCODER_EUNSUPPORTED;
    if (!video->fps || video->frame_count > SIZE_MAX / 48000u)
        return TM_ENCODER_EINVAL;
    total = video->frame_count * 48000u;
    if (total % video->fps != 0u)
        return TM_ENCODER_EUNSUPPORTED;
    samples = total / video->fps;
    if (samples != audio->sample_count || samples % 960u != 0u)
        return TM_ENCODER_EUNSUPPORTED;
    if (samples > SIZE_MAX / audio->channels ||
        samples * audio->channels > SIZE_MAX / sizeof(float) ||
        audio->sample_capacity_bytes <
            samples * audio->channels * sizeof(float))
        return TM_ENCODER_EINVAL;
    return TM_ENCODER_OK;
}

static tm_encoder_status tm_audio_emit_next(
    tm_audio_owner *owner,
    tm_encoder_should_cancel_fn cancelled, void *cancel_user) {
    int16_t pcm[960u * 2u];
    uint8_t compressed[4096u];
    size_t n = sizeof(compressed);
    size_t count = 960u * owner->input->channels;
    size_t start, i;
    turbo_encoded_frame_t info = {0};
    turbo_muxer_packet_t packet = {0};
    if (owner->current >= owner->packets)
        return TM_ENCODER_EINVAL;
    if (cancelled && cancelled(cancel_user))
        return TM_ENCODER_ECANCELLED;
    start = owner->current * count;
    for (i = 0; i < count; ++i) {
        size_t frame_sample = owner->current * 960u +
                              i / owner->input->channels;
        float v;
        if (frame_sample >= owner->input->sample_count) {
            pcm[i] = 0; /* zero drain: do not read past the borrowed PCM */
            continue;
        }
        v = owner->input->samples[start + i];
        if (!isfinite(v))
            return TM_ENCODER_EINVAL;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[i] = (int16_t)(v * 32767.0f + (v < 0.0f ? -0.5f : 0.5f));
    }
    if (turbo_codec_encode(owner->codec, (const uint8_t *)pcm,
                           count * sizeof(int16_t), compressed,
                           &n, &info) != TURBO_CODEC_OK || n == 0u)
        return TM_ENCODER_ECODEC;
    packet.stream_id = owner->track;
    packet.data = compressed;
    packet.size = n;
    packet.pts = (int64_t)(owner->current * UINT64_C(20000));
    packet.dts = packet.pts;
    packet.duration = 20000;
    packet.is_keyframe = 1;
    if ((owner->current + 1u == owner->packets && owner->tail_discard_ns ?
            turbo_muxer_write_packet_with_discard_padding(
                owner->muxer, &packet, owner->tail_discard_ns) :
            turbo_muxer_write_packet(owner->muxer, &packet)) != 0)
        return TM_ENCODER_ECAPACITY;
    ++owner->current;
    return TM_ENCODER_OK;
}

static tm_encoder_status tm_encoder_webm_owner(
    const tm_encoder_video_v1 *options,
    const tm_encoder_audio_v1 *audio,
    tm_encoder_write_fn write, void *write_user,
    tm_encoder_should_cancel_fn cancelled, void *cancel_user,
    size_t *written) {
    tm_encoder_status status = TM_ENCODER_EINVAL;
    turbo_video_codec_config_t codec_settings = {0};
    turbo_codec_t *codec = NULL;
    turbo_codec_t *opus = NULL;
    tm_audio_owner audio_owner = {0};
    turbo_audio_codec_config_t opus_config = {0};
    uint8_t opus_head_bytes[19];
    turbo_muxer_t *muxer = NULL;
    turbo_muxer_config_t muxer_settings = {0};
    turbo_stream_info_t stream = {0};
    turbo_muxer_packet_t packet = {0};
    turbo_encoded_frame_t frame_info = {0};
    struct SwsContext *scale = NULL;
    uint8_t *yuv = NULL, *compressed = NULL, *container = NULL;
    size_t pixel_count, yuv_size, packet_capacity, frame_index, container_size = 0;
    int video_track = -1;
    int audio_track = -1;
    uint32_t pre_skip = 0;
    int width, height;

    if (written)
        *written = 0;
    if (!options || !write || !written)
        return TM_ENCODER_EINVAL;
    if (options->size != sizeof(*options) ||
        options->version != TM_ENCODER_VIDEO_V1)
        return TM_ENCODER_EVERSION;
    if (options->audio_bytes || options->audio_pcm)
        return TM_ENCODER_EUNSUPPORTED; /* V1 flags must never be ignored */
    if (audio) {
        status = tm_audio_preflight(options, audio);
        if (status != TM_ENCODER_OK)
            return status;
    }
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
    if (turbo_muxer_add_stream(muxer, &stream, &video_track) != 0) {
        status = TM_ENCODER_ECODEC;
        goto cleanup;
    }
    if (audio) {
        struct opus_head_t head = {0};
        opus_config.sample_rate = 48000;
        opus_config.channels = (int)audio->channels;
        opus_config.bitrate = 64000;
        opus_config.frame_size_ms = 20;
        opus_config.complexity = 5;
        opus = turbo_codec_create_encoder("opus", &opus_config);
        if (!opus) {
            status = TM_ENCODER_EUNSUPPORTED;
            goto cleanup;
        }
        if (turbo_codec_opus_lookahead_samples(opus, &pre_skip) !=
                TURBO_CODEC_OK || !pre_skip || pre_skip > UINT16_MAX) {
            status = TM_ENCODER_ECODEC;
            goto cleanup;
        }
        head.version = 1;
        head.channels = (uint8_t)audio->channels;
        head.pre_skip = (uint16_t)pre_skip;
        head.input_sample_rate = 48000;
        if (opus_head_save(&head, opus_head_bytes, sizeof(opus_head_bytes)) != 19) {
            status = TM_ENCODER_ECODEC;
            goto cleanup;
        }
        stream = (turbo_stream_info_t){0};
        stream.type = TURBO_CODEC_TYPE_AUDIO;
        stream.codec_name = "opus";
        stream.sample_rate = 48000;
        stream.channels = (int)audio->channels;
        stream.extradata = opus_head_bytes;
        stream.extradata_size = sizeof(opus_head_bytes);
        if (turbo_muxer_add_stream(muxer, &stream, &audio_track) != 0) {
            status = TM_ENCODER_ECODEC;
            goto cleanup;
        }
        audio_owner.input = audio;
        audio_owner.codec = opus;
        audio_owner.muxer = muxer;
        audio_owner.track = audio_track;
        /* Encoded samples - actual Opus pre-skip - tail trim = source.
         * Pad with bounded zeros only after the last borrowed sample. */
        {
            uint64_t needed = (uint64_t)audio->sample_count + pre_skip;
            uint64_t count = (needed + 959u) / 960u;
            uint64_t excess = count * 960u - needed;
            if (!count || count > SIZE_MAX || excess >= 960u) {
                status = TM_ENCODER_ECAPACITY;
                goto cleanup;
            }
            audio_owner.packets = (size_t)count;
            audio_owner.tail_discard_ns =
                excess * UINT64_C(1000000000) / UINT64_C(48000);
        }
    }
    if (turbo_muxer_write_header(muxer) != 0) {
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
        if (audio) {
            uint64_t timestamp = (uint64_t)frame_index *
                                 UINT64_C(1000000) / options->fps;
            while (audio_owner.current < audio_owner.packets &&
                   (uint64_t)audio_owner.current * UINT64_C(20000) <= timestamp) {
                status = tm_audio_emit_next(&audio_owner, cancelled, cancel_user);
                if (status != TM_ENCODER_OK)
                    goto cleanup;
            }
        }
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

    while (audio && audio_owner.current < audio_owner.packets) {
        status = tm_audio_emit_next(&audio_owner, cancelled, cancel_user);
        if (status != TM_ENCODER_OK)
            goto cleanup;
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
    turbo_codec_destroy(opus);
    turbo_codec_destroy(codec);
    return status;
}

tm_encoder_status tm_encoder_webm_encode(
    const tm_encoder_video_v1 *options,
    tm_encoder_write_fn write, void *write_user,
    tm_encoder_should_cancel_fn cancelled, void *cancel_user,
    size_t *written) {
    return tm_encoder_webm_owner(
        options, NULL, write, write_user, cancelled, cancel_user, written);
}

tm_encoder_status tm_encoder_webm_encode_av_v2(
    const tm_encoder_video_v1 *video,
    const tm_encoder_audio_v1 *audio,
    tm_encoder_write_fn write, void *write_user,
    tm_encoder_should_cancel_fn cancelled, void *cancel_user,
    size_t *written) {
    if (!audio) {
        if (written) *written = 0;
        return TM_ENCODER_EINVAL;
    }
    return tm_encoder_webm_owner(
        video, audio, write, write_user, cancelled, cancel_user, written);
}
