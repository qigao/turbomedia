#include "turbo_media_encoder.h"
#include "turbo_codec.h"
#include "turbo_muxer.h"
#include "opus-head.h"

#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

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

enum { TM_AUDIO_RESAMPLE_SOURCE_BATCH = 256,
       TM_AUDIO_RESAMPLE_PENDING_FRAMES = 4096 };

typedef struct tm_audio_owner {
    const tm_encoder_audio_v1 *input;
    turbo_codec_t *codec;
    turbo_muxer_t *muxer;
    int track;
    size_t current;
    size_t packets;
    size_t output_samples;     /* exact target samples per channel at 48 kHz */
    size_t source_cursor;      /* source samples per channel consumed by swr */
    struct SwrContext *resample; /* owned; only for non-48 kHz input */
    int16_t pending_pcm[TM_AUDIO_RESAMPLE_PENDING_FRAMES * 2u];
    size_t pending_count;       /* frames per channel */
    size_t pending_offset;
    uint64_t tail_discard_ns;
} tm_audio_owner;

/* All timing calculations use the exact rational frame_count/fps.
 * Non-integral sample counts are rejected instead of inventing/dropping
 * time. Supported rates are explicitly pinned; no provider auto-detection. */
static tm_encoder_status tm_audio_preflight(const tm_encoder_video_v1 *video,
                                           const tm_encoder_audio_v1 *audio) {
    uint64_t source_total, output_total;
    size_t elements, i;
    if (!video) return TM_ENCODER_EINVAL;
    if (!audio || audio->size != sizeof(*audio) ||
        audio->version != TM_ENCODER_AUDIO_V1)
        return TM_ENCODER_EVERSION;
    if (!audio->samples || !audio->sample_count || audio->reserved ||
        audio->channels < 1u || audio->channels > 2u)
        return TM_ENCODER_EINVAL;
    switch (audio->sample_rate) {
    case 8000u: case 12000u: case 16000u:
    case 24000u: case 44100u: case 48000u:
        break;
    default:
        return TM_ENCODER_EUNSUPPORTED;
    }
    if (!video->fps || !video->frame_count ||
        video->frame_count > UINT64_MAX / 48000u)
        return TM_ENCODER_EINVAL;
    source_total = (uint64_t)video->frame_count * audio->sample_rate;
    output_total = (uint64_t)video->frame_count * UINT64_C(48000);
    if (source_total % video->fps || output_total % video->fps ||
        source_total / video->fps != audio->sample_count)
        return TM_ENCODER_EUNSUPPORTED;
    if (audio->sample_count > SIZE_MAX / audio->channels ||
        audio->sample_count * audio->channels > SIZE_MAX / sizeof(float) ||
        audio->sample_capacity_bytes <
            audio->sample_count * audio->channels * sizeof(float))
        return TM_ENCODER_EINVAL;
    /* libswresample may otherwise convert NaN/Inf to plausible PCM16;
     * reject the full borrowed buffer before creating codecs or muxers. */
    elements = audio->sample_count * audio->channels;
    for (i = 0; i < elements; ++i)
        if (!isfinite(audio->samples[i]))
            return TM_ENCODER_EINVAL;
    return TM_ENCODER_OK;
}

/* Converts exactly the requested output samples into one bounded Opus
 * frame. Feed FFmpeg at most 256 source frames; drain into a bounded
 * 4096-frame staging buffer before taking another chunk. Unlike repeatedly
 * feeding with only 960 output slots, this cannot accumulate an unbounded
 * internal FIFO when upsampling 8/12/16/24 kHz inputs. The output owner
 * consumes staged samples before requesting more. No full-clip buffer.
 * Pad only the unused final Opus frame capacity with zeros. */
static tm_encoder_status tm_audio_prepare_pcm(tm_audio_owner *owner,
                                               int16_t pcm[960u * 2u]) {
    const tm_encoder_audio_v1 *audio = owner->input;
    size_t output_index = owner->current * 960u;
    size_t valid = output_index < owner->output_samples ?
                   owner->output_samples - output_index : 0u;
    size_t channels = audio->channels, i;
    if (valid > 960u) valid = 960u;
    if (owner->resample) {
        size_t completed = 0;
        while (completed < valid) {
            if (owner->pending_offset < owner->pending_count) {
                size_t available = owner->pending_count - owner->pending_offset;
                size_t take = valid - completed;
                if (take > available) take = available;
                memcpy(pcm + completed * channels,
                       owner->pending_pcm + owner->pending_offset * channels,
                       take * channels * sizeof(int16_t));
                owner->pending_offset += take;
                completed += take;
                continue;
            }
            {
                int incoming = 0, required, produced;
                const uint8_t *source[1] = {NULL};
                uint8_t *dest[1] = {(uint8_t *)owner->pending_pcm};
                owner->pending_offset = owner->pending_count = 0u;
                if (owner->source_cursor < audio->sample_count) {
                    size_t n = audio->sample_count - owner->source_cursor;
                    if (n > TM_AUDIO_RESAMPLE_SOURCE_BATCH)
                        n = TM_AUDIO_RESAMPLE_SOURCE_BATCH;
                    incoming = (int)n;
                    source[0] = (const uint8_t *)(audio->samples +
                        owner->source_cursor * channels);
                }
                required = swr_get_out_samples(owner->resample, incoming);
                if (required < 0 || required > TM_AUDIO_RESAMPLE_PENDING_FRAMES)
                    return TM_ENCODER_ECAPACITY;
                produced = swr_convert(owner->resample, dest,
                                       TM_AUDIO_RESAMPLE_PENDING_FRAMES,
                                       incoming ? source : NULL, incoming);
                if (produced < 0 ||
                    produced > TM_AUDIO_RESAMPLE_PENDING_FRAMES)
                    return TM_ENCODER_ECODEC;
                owner->source_cursor += (size_t)incoming;
                owner->pending_count = (size_t)produced;
                if (!produced && !incoming)
                    return TM_ENCODER_ECODEC;
            }
        }
        /* A/V success must never hide unconsumed original source samples.
         * The final Opus frame may also contain padded zeros past valid. */
        if (owner->current + 1u == owner->packets &&
            owner->source_cursor != audio->sample_count)
            return TM_ENCODER_ECODEC;
    } else {
        /* Exact 48kHz path remains a direct bounded float->S16 conversion. */
        for (i = 0; i < valid * channels; ++i) {
            float v = audio->samples[output_index * channels + i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            pcm[i] = (int16_t)(v * 32767.0f +
                               (v < 0.0f ? -0.5f : 0.5f));
        }
    }
    if (valid < 960u)
        memset(pcm + valid * channels, 0,
               (960u - valid) * channels * sizeof(int16_t));
    return TM_ENCODER_OK;
}

static tm_encoder_status tm_audio_emit_next(
    tm_audio_owner *owner,
    tm_encoder_should_cancel_fn cancelled, void *cancel_user) {
    int16_t pcm[960u * 2u];
    uint8_t compressed[4096u];
    size_t n = sizeof(compressed);
    size_t count = 960u * owner->input->channels;
    turbo_encoded_frame_t info = {0};
    turbo_muxer_packet_t packet = {0};
    tm_encoder_status pcm_status;
    if (owner->current >= owner->packets)
        return TM_ENCODER_EINVAL;
    if (cancelled && cancelled(cancel_user))
        return TM_ENCODER_ECANCELLED;
    pcm_status = tm_audio_prepare_pcm(owner, pcm);
    if (pcm_status != TM_ENCODER_OK) return pcm_status;
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
    if (audio) {
        status = tm_audio_preflight(options, audio);
        if (status != TM_ENCODER_OK)
            return status;
    }

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
        head.input_sample_rate = audio->sample_rate; /* informational source rate */
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
        audio_owner.output_samples =
            (size_t)((uint64_t)options->frame_count * UINT64_C(48000) /
                     options->fps);
        /* At non-48kHz input rates, use the one installed FFmpeg
         * swresample provider with bounded streaming input. */
        if (audio->sample_rate != 48000u) {
            AVChannelLayout layout = {0};
            int setup_result;
            av_channel_layout_default(&layout, (int)audio->channels);
            setup_result = swr_alloc_set_opts2(
                &audio_owner.resample, &layout, AV_SAMPLE_FMT_S16, 48000,
                &layout, AV_SAMPLE_FMT_FLT, (int)audio->sample_rate, 0, NULL);
            av_channel_layout_uninit(&layout);
            if (setup_result < 0 || !audio_owner.resample ||
                swr_init(audio_owner.resample) < 0) {
                status = TM_ENCODER_ECODEC;
                goto cleanup;
            }
        }
        /* Encoded 48k samples - actual pre-skip - terminal padding
         * = exact resampled output duration. */
        {
            uint64_t needed = (uint64_t)audio_owner.output_samples + pre_skip;
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
    swr_free(&audio_owner.resample);
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
