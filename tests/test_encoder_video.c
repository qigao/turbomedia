#include <tinytest.h>
#include <turbo_media_encoder.h>
#include <turbo_demuxer.h>

#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>

typedef struct video_result {
    uint8_t *bytes;
    size_t capacity, size;
    unsigned writes;
} video_result;

static int collect_video(void *user, const uint8_t *bytes, size_t size) {
    video_result *sink = (video_result *)user;
    if (!sink || !bytes || size > sink->capacity - sink->size)
        return -1;
    memcpy(sink->bytes + sink->size, bytes, size);
    sink->size += size;
    ++sink->writes;
    return 0;
}

static int cancel_now(void *user) {
    (void)user;
    return 1;
}

typedef struct cancel_after {
    unsigned calls;
    unsigned maximum;
} cancel_after;

static int cancel_after_frames(void *user) {
    cancel_after *state = (cancel_after *)user;
    ++state->calls;
    return state->calls > state->maximum;
}

static tm_encoder_video_v1 init_video(const tm_encoder_video_frame_v1 *frames,
                                      size_t frame_count) {
    tm_encoder_video_v1 options = TM_ENCODER_VIDEO_V1_INIT;
    options.width = 16;
    options.height = 16;
    options.fps = 10;
    options.bitrate = 250000;
    options.frames = frames;
    options.frame_count = frame_count;
    options.max_output_bytes = 1024 * 1024;
    return options;
}

static void prepare_frames(uint8_t pixels[][16 * 16 * 3],
                           tm_encoder_video_frame_v1 *frames,
                           size_t frame_count) {
    size_t frame, byte;
    for (frame = 0; frame < frame_count; ++frame) {
        for (byte = 0; byte < 16 * 16 * 3; ++byte)
            pixels[frame][byte] = (uint8_t)((byte + frame * 37u) & 0xffu);
        frames[frame] = (tm_encoder_video_frame_v1)
            TM_ENCODER_VIDEO_FRAME_V1_INIT;
        frames[frame].pixel_format = TM_ENCODER_PIXEL_RGB24;
        frames[frame].pixels = pixels[frame];
        frames[frame].stride_bytes = 16 * 3;
        frames[frame].pixels_capacity = sizeof(pixels[frame]);
    }
}

/* Test the actual positive signed EBML element payload in the output.
 * All three queried IDs are two bytes and have a one-byte size here. */
static int find_webm_positive_element(const uint8_t *data, size_t size,
                                      uint16_t id, uint64_t *out) {
    size_t i, j;
    if (!data || !out || size < 4) return 0;
    for (i = 0; i + 3 < size; ++i) {
        size_t n;
        uint64_t value = 0;
        if (data[i] != (uint8_t)(id >> 8) ||
            data[i + 1] != (uint8_t)id ||
            (data[i + 2] & 0xf8u) != 0x80u)
            continue;
        n = data[i + 2] & 0x7fu;
        if (!n || n > 8 || n > size - i - 3u ||
            (data[i + 3] & 0x80u) != 0)
            continue;
        for (j = 0; j < n; ++j)
            value = (value << 8) | data[i + 3 + j];
        *out = value;
        return 1;
    }
    return 0;
}

/* Independent FORMAT acceptance, not another production demuxer.
 * The FFmpeg SDK already required by TurboMedia must be able to parse and
 * seek the output of the single TurboMedia-owned Codec/Muxer path. */
typedef struct tm_ffmpeg_webm_memory {
    const uint8_t *bytes;
    size_t length;
    size_t position;
} tm_ffmpeg_webm_memory;

static int tm_ffmpeg_webm_read(void *opaque, uint8_t *dst, int want) {
    tm_ffmpeg_webm_memory *r = (tm_ffmpeg_webm_memory *)opaque;
    size_t available, amount;
    if (!r || !dst || want <= 0) return AVERROR(EINVAL);
    if (r->position >= r->length) return AVERROR_EOF;
    available = r->length - r->position;
    amount = (size_t)want < available ? (size_t)want : available;
    memcpy(dst, r->bytes + r->position, amount);
    r->position += amount;
    return (int)amount;
}

static int64_t tm_ffmpeg_webm_seek(void *opaque, int64_t offset, int whence) {
    tm_ffmpeg_webm_memory *r = (tm_ffmpeg_webm_memory *)opaque;
    int64_t base, next;
    if (!r || (uint64_t)r->length > (uint64_t)INT64_MAX)
        return AVERROR(EINVAL);
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return (int64_t)r->length;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (int64_t)r->position;
    else if (whence == SEEK_END) base = (int64_t)r->length;
    else return AVERROR(EINVAL);
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset))
        return AVERROR(EINVAL);
    next = base + offset;
    if (next < 0 || (uint64_t)next > r->length)
        return AVERROR(EINVAL);
    r->position = (size_t)next;
    return next;
}

static int tm_ffmpeg_webm_read_seek_verify(
    const uint8_t *bytes, size_t size, int64_t target_ms) {
    tm_ffmpeg_webm_memory memory = {0};
    AVIOContext *io = NULL;
    AVFormatContext *format = NULL;
    AVPacket *packet = NULL;
    uint8_t *io_buffer = NULL;
    int video = -1, audio = -1, seen_video = 0, seen_audio = 0;
    int status = -1, i;
    if (!bytes || !size || (uint64_t)size > (uint64_t)INT64_MAX ||
        target_ms < 0)
        return -1;
    memory.bytes = bytes;
    memory.length = size;
    io_buffer = (uint8_t *)av_malloc(4096);
    if (!io_buffer) goto done;
    io = avio_alloc_context(io_buffer, 4096, 0, &memory,
                            tm_ffmpeg_webm_read, NULL, tm_ffmpeg_webm_seek);
    if (!io) goto done;
    io_buffer = NULL; /* AVIOContext owns the buffer */
    format = avformat_alloc_context();
    if (!format) goto done;
    format->pb = io;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&format, NULL, NULL, NULL) < 0 ||
        avformat_find_stream_info(format, NULL) < 0)
        goto done;
    for (i = 0; i < (int)format->nb_streams; ++i) {
        const AVCodecParameters *c = format->streams[i]->codecpar;
        if (c->codec_type == AVMEDIA_TYPE_VIDEO &&
            c->codec_id == AV_CODEC_ID_VP8) video = i;
        if (c->codec_type == AVMEDIA_TYPE_AUDIO &&
            c->codec_id == AV_CODEC_ID_OPUS &&
            c->sample_rate == 48000 &&
            c->ch_layout.nb_channels == 1) audio = i;
    }
    if (format->nb_streams != 2u || video < 0 || audio < 0)
        goto done;
    packet = av_packet_alloc();
    if (!packet) goto done;
    /* An independent Matroska reader must resolve the Cues itself.
     * An indexed backward seek exercises the segment-relative offsets. */
    if (av_seek_frame(format, video,
        av_rescale_q(target_ms, (AVRational){1, 1000},
                     format->streams[video]->time_base),
        AVSEEK_FLAG_BACKWARD) < 0)
        goto done;
    for (i = 0; i < 80 && av_read_frame(format, packet) >= 0; ++i) {
        if (packet->stream_index == video) ++seen_video;
        if (packet->stream_index == audio) ++seen_audio;
        av_packet_unref(packet);
        if (seen_video && seen_audio) break;
    }
    if (seen_video && seen_audio) status = 0;
done:
    av_packet_free(&packet);
    avformat_close_input(&format);
    /* Per FFmpeg's avio_read_callback example, AVIOContext owns the
     * current, possibly reallocated buffer, but avio_context_free does
     * not release it. Free the current buffer before the context. */
    if (io) av_freep(&io->buffer);
    avio_context_free(&io);
    av_free(io_buffer);
    return status;
}

/* Real in-repo A/V profile probe. Not a new codec or consumer:
 * all packets are written by TurboMedia::Encoder, demuxed by
 * TurboMedia::Demuxer, and decoded by TurboMedia::Codec. */
typedef struct av_profile_probe {
    tm_encoder_status status;
    int tracks, audio_channels, audio_rate, video_packets, audio_packets;
    int nonzero_left, nonzero_right, pts_monotonic;
    int has_codec_delay, has_preroll, has_padding;
    int trimmed_audio_packets;
    int64_t padding_from_demuxer_ns;
    uint32_t opus_input_rate;
    uint16_t pre_skip;
    uint64_t codec_delay_ns, seek_preroll_ns, discard_padding_ns;
    uint64_t parsed_codec_delay_ns, parsed_seek_preroll_ns;
    size_t decoded_samples_per_channel;
    /* Correlation of the final 20 ms of audible, pre-skip-adjusted PCM
     * against the known 100 Hz test waveform; never count padded samples. */
    size_t tail_samples_checked;
    int64_t tail_correlation_left, tail_correlation_right;
    int64_t duration_ms;
    unsigned writes;
} av_profile_probe;

static av_profile_probe run_av_profile(uint32_t sample_rate,
                                       uint32_t channels,
                                       uint32_t fps, size_t source_samples) {
    av_profile_probe result = {0};
    uint8_t pixels[3][16 * 16 * 3];
    tm_encoder_video_frame_v1 frames[3];
    tm_encoder_video_v1 video;
    tm_encoder_audio_v1 audio = TM_ENCODER_AUDIO_V1_INIT;
    float *pcm = NULL;
    video_result sink = {0};
    turbo_demuxer_config_t config = {0};
    turbo_stream_info_t info = {0};
    turbo_demuxer_packet_t packet = {0};
    turbo_demuxer_packet_timing_v1 timing = TURBO_DEMUXER_PACKET_TIMING_V1_INIT;
    turbo_container_metadata_t metadata = {0};
    turbo_demuxer_track_timing_v1 track_timing =
        TURBO_DEMUXER_TRACK_TIMING_V1_INIT;
    turbo_audio_codec_config_t decoder_config = {0};
    turbo_demuxer_t *demuxer = NULL;
    turbo_codec_t *decoder = NULL;
    size_t i, written = 0, target_samples;
    int64_t previous_pts = -1;
    int read_status = 0;
    result.status = TM_ENCODER_ENOMEM;
    if (!sample_rate || !channels || channels > 2 ||
        source_samples > SIZE_MAX / channels ||
        source_samples * channels > SIZE_MAX / sizeof(float) ||
        source_samples > SIZE_MAX / 48000u)
        return result;
    target_samples = source_samples * 48000u / sample_rate;
    pcm = (float *)malloc(source_samples * channels * sizeof(float));
    if (!pcm) return result;
    prepare_frames(pixels, frames, 3);
    video = init_video(frames, 3);
    video.fps = fps;
    /* Time-based square wave has the same 100 Hz phase at every source
     * rate, so post-resample 48k samples can be checked independently of
     * Opus's nonzero pre-skip and terminal DiscardPadding. Opposite stereo
     * polarity also detects accidental channel swaps/interleaving. */
    for (i = 0; i < source_samples; ++i) {
        float left = (((uint64_t)i * 200u / sample_rate) & 1u) ?
                     0.70f : -0.70f;
        pcm[i * channels] = left;
        if (channels == 2u)
            pcm[i * channels + 1u] = -left * 0.80f;
    }
    audio.samples = pcm;
    audio.sample_count = source_samples;
    audio.sample_rate = sample_rate;
    audio.channels = channels;
    audio.sample_capacity_bytes = source_samples * channels * sizeof(float);
    sink.capacity = video.max_output_bytes;
    sink.bytes = (uint8_t *)malloc(sink.capacity);
    if (!sink.bytes) goto finished;

    result.status = tm_encoder_webm_encode_av_v2(
        &video, &audio, collect_video, &sink, NULL, NULL, &written);
    result.writes = sink.writes;
    if (result.status != TM_ENCODER_OK) goto finished;
    if (sink.writes != 1u || written != sink.size || !written) {
        result.status = TM_ENCODER_ECODEC;
        goto finished;
    }
    turbo_demuxer_registry_init();
    config.data = sink.bytes;
    config.data_size = sink.size;
    result.status = TM_ENCODER_ECODEC;
    demuxer = turbo_demuxer_create_by_name("webm", &config);
    if (!demuxer || turbo_demuxer_open(demuxer) != 0) goto finished;
    result.tracks = turbo_demuxer_get_stream_count(demuxer);
    if (result.tracks != 2 ||
        turbo_demuxer_get_stream_info(demuxer, 1, &info) != 0 ||
        !info.codec_name || strcmp(info.codec_name, "opus") != 0 ||
        !info.extradata || info.extradata_size < 19u)
        goto finished;
    result.audio_rate = info.sample_rate;
    result.audio_channels = info.channels;
    result.pre_skip = (uint16_t)((unsigned)info.extradata[10] |
                                 (unsigned)info.extradata[11] << 8);
    result.opus_input_rate =
        (uint32_t)info.extradata[12] |
        (uint32_t)info.extradata[13] << 8 |
        (uint32_t)info.extradata[14] << 16 |
        (uint32_t)info.extradata[15] << 24;
    result.has_codec_delay = find_webm_positive_element(
        sink.bytes, sink.size, 0x56AA, &result.codec_delay_ns);
    result.has_preroll = find_webm_positive_element(
        sink.bytes, sink.size, 0x56BB, &result.seek_preroll_ns);
    result.has_padding = find_webm_positive_element(
        sink.bytes, sink.size, 0x75A2, &result.discard_padding_ns);
    if (turbo_demuxer_get_track_timing(demuxer, 1, &track_timing) != 0)
        goto finished;
    result.parsed_codec_delay_ns = track_timing.codec_delay_ns;
    result.parsed_seek_preroll_ns = track_timing.seek_preroll_ns;
    /* These are the real TrackEntry values, not inferred by consumers
     * from OpusHead; a video track has no Opus pre-skip/pre-roll. */
    if (result.parsed_codec_delay_ns != result.codec_delay_ns ||
        result.parsed_seek_preroll_ns != result.seek_preroll_ns ||
        turbo_demuxer_get_track_timing(demuxer, 0, &track_timing) != 0 ||
        track_timing.codec_delay_ns != 0 ||
        track_timing.seek_preroll_ns != 0)
        goto finished;
    decoder_config.sample_rate = 48000;
    decoder_config.channels = (int)channels;
    decoder_config.bitrate = 64000;
    decoder_config.frame_size_ms = 20;
    decoder_config.complexity = 5;
    decoder = turbo_codec_create_decoder("opus", &decoder_config);
    if (!decoder) goto finished;
    result.pts_monotonic = 1;
    while ((read_status = turbo_demuxer_read_packet_with_timing(
                 demuxer, &packet, &timing)) == 1) {
        if (packet.pts < previous_pts)
            result.pts_monotonic = 0;
        previous_pts = packet.pts;
        if (packet.stream_index == 0) {
            if (timing.discard_padding_ns != 0) {
                turbo_demuxer_free_packet(&packet);
                goto finished; /* never attach Opus padding to video */
            }
            ++result.video_packets;
        } else if (packet.stream_index == 1) {
            int16_t decoded[960u * 2u];
            size_t decoded_bytes = sizeof(decoded);
            ++result.audio_packets;
            if (timing.discard_padding_ns != 0) {
                ++result.trimmed_audio_packets;
                result.padding_from_demuxer_ns = timing.discard_padding_ns;
            }
            if (turbo_codec_decode(decoder, packet.data, packet.size,
                (uint8_t *)decoded, &decoded_bytes) != TURBO_CODEC_OK ||
                decoded_bytes % (sizeof(int16_t) * channels) != 0) {
                turbo_demuxer_free_packet(&packet);
                goto finished;
            }
            {
                size_t before = result.decoded_samples_per_channel;
                size_t count = decoded_bytes / (sizeof(int16_t) * channels);
                result.decoded_samples_per_channel += count;
                for (i = 0; i < count; ++i) {
                    size_t absolute_index = before + i;
                    if (decoded[i * channels] != 0) ++result.nonzero_left;
                    if (channels == 2u && decoded[i * channels + 1u] != 0)
                        ++result.nonzero_right;
                    if (absolute_index >= result.pre_skip &&
                        target_samples >= 960u) {
                        size_t presented = absolute_index - result.pre_skip;
                        if (presented >= target_samples - 960u &&
                            presented < target_samples) {
                            int expected_sign =
                                (((uint64_t)presented * 200u / 48000u) & 1u) ?
                                1 : -1;
                            result.tail_correlation_left +=
                                (int64_t)decoded[i * channels] * expected_sign;
                            if (channels == 2u)
                                result.tail_correlation_right -=
                                    (int64_t)decoded[i * channels + 1u] *
                                    expected_sign;
                            ++result.tail_samples_checked;
                        }
                    }
                }
            }
        } else {
            turbo_demuxer_free_packet(&packet);
            goto finished;
        }
        turbo_demuxer_free_packet(&packet);
        if (result.audio_packets + result.video_packets > 64)
            goto finished;
    }
    if (read_status != 0 || turbo_demuxer_get_metadata(
            demuxer, &metadata) != 0)
        goto finished;
    if (result.trimmed_audio_packets != 1 ||
        result.padding_from_demuxer_ns <= 0 ||
        (uint64_t)result.padding_from_demuxer_ns != result.discard_padding_ns)
        goto finished;
    result.duration_ms = metadata.duration_ms;
    result.status = TM_ENCODER_OK;
finished:
    turbo_demuxer_free_packet(&packet);
    turbo_codec_destroy(decoder);
    turbo_demuxer_destroy(demuxer);
    free(sink.bytes);
    free(pcm);
    return result;
}

suite("TurboMedia Encoder VP8 WebM raw-frames contract") {
    group("native Opus seek and decoder discontinuity") {
        it("resets the codec and delivers exact PCM after cue-based preroll") {
            enum { FRAME_COUNT = 70, SAMPLE_COUNT = 336000 };
            uint8_t (*pixels)[16 * 16 * 3] = NULL;
            tm_encoder_video_frame_v1 *frames = NULL;
            float *pcm = NULL;
            tm_encoder_video_v1 video;
            tm_encoder_audio_v1 audio = TM_ENCODER_AUDIO_V1_INIT;
            video_result sink = {0};
            turbo_demuxer_config_t demux_config = {0};
            turbo_demuxer_t *demuxer = NULL;
            turbo_codec_t *decoder = NULL;
            turbo_audio_codec_config_t codec_config = {0};
            turbo_demuxer_track_timing_v1 track =
                TURBO_DEMUXER_TRACK_TIMING_V1_INIT;
            turbo_demuxer_packet_timing_v1 timing =
                TURBO_DEMUXER_PACKET_TIMING_V1_INIT;
            turbo_demuxer_packet_t packet = {0};
            size_t written = 0, kept_samples = 0;
            size_t before_target = 0, nonzero = 0, n, i;
            int first_decoded = 0, audio_packets = 0, trimmed = 0;
            int read_status = -1;
            int64_t anchor_ms = -1, prior_pts = -1;
            const int64_t target_ms = INT64_C(5940);
            const int64_t duration_ms = INT64_C(7000);
            tm_encoder_status result;

            pixels = (uint8_t (*)[16 * 16 * 3])
                malloc(FRAME_COUNT * sizeof(*pixels));
            frames = (tm_encoder_video_frame_v1 *)
                calloc(FRAME_COUNT, sizeof(*frames));
            pcm = (float *)malloc(SAMPLE_COUNT * sizeof(*pcm));
            sink.capacity = 4u * 1024u * 1024u;
            sink.bytes = (uint8_t *)malloc(sink.capacity);
            check_not_null(pixels);
            check_not_null(frames);
            check_not_null(pcm);
            check_not_null(sink.bytes);
            if (!pixels || !frames || !pcm || !sink.bytes) goto cleanup;
            prepare_frames(pixels, frames, FRAME_COUNT);
            video = init_video(frames, FRAME_COUNT);
            video.max_output_bytes = sink.capacity;
            for (i = 0; i < SAMPLE_COUNT; ++i)
                pcm[i] = i % 100u < 50u ? 0.65f : -0.65f;
            audio.samples = pcm;
            audio.sample_count = SAMPLE_COUNT;
            audio.sample_capacity_bytes = SAMPLE_COUNT * sizeof(*pcm);
            result = tm_encoder_webm_encode_av_v2(
                &video, &audio, collect_video, &sink, NULL, NULL, &written);
            check_equal(result, TM_ENCODER_OK);
            check_equal(written, sink.size);
            check_equal(sink.writes, 1u);
            if (result != TM_ENCODER_OK || !written) goto cleanup;
            /* External FFmpeg Matroska parser / native seek oracle:
             * it is NOT part of TurboMedia's production media runtime. */
            check_equal(tm_ffmpeg_webm_read_seek_verify(
                sink.bytes, sink.size, INT64_C(5940)), 0);

            turbo_demuxer_registry_init();
            demux_config.data = sink.bytes;
            demux_config.data_size = written;
            demuxer = turbo_demuxer_create_by_name("webm", &demux_config);
            check_not_null(demuxer);
            if (!demuxer) goto cleanup;
            check_equal(turbo_demuxer_open(demuxer), 0);
            if (turbo_demuxer_get_stream_count(demuxer) != 2 ||
                turbo_demuxer_get_track_timing(demuxer, 1, &track) != 0)
                goto cleanup;
            check_true(track.codec_delay_ns > 0);
            check_true(track.seek_preroll_ns >= UINT64_C(80000000));

            codec_config.sample_rate = 48000;
            codec_config.channels = 1;
            codec_config.bitrate = 64000;
            codec_config.frame_size_ms = 20;
            codec_config.complexity = 5;
            decoder = turbo_codec_create_decoder("opus", &codec_config);
            check_not_null(decoder);
            if (!decoder) goto cleanup;
            /* Decode initial packets first to establish a nonempty decoder
             * state, then retire this owner across the discontinuity. */
            while (first_decoded < 3 &&
                   (read_status = turbo_demuxer_read_packet(
                        demuxer, &packet)) == 1) {
                if (packet.stream_index == 1) {
                    int16_t samples[960];
                    size_t sample_bytes = sizeof(samples);
                    check_equal(turbo_codec_decode(
                        decoder, packet.data, packet.size,
                        (uint8_t *)samples, &sample_bytes), TURBO_CODEC_OK);
                    ++first_decoded;
                }
                turbo_demuxer_free_packet(&packet);
            }
            turbo_demuxer_free_packet(&packet);
            check_equal(first_decoded, 3);
            if (first_decoded != 3) goto cleanup;
            turbo_codec_destroy(decoder);
            decoder = NULL;

            check_equal(turbo_demuxer_seek_opus_preroll(
                demuxer, 1, target_ms, &anchor_ms), 0);
            /* The conservative threshold is 5940ms-86.5ms=5853.5ms.
             * A future keyframe at 6000ms must never be selected. */
            check_true(anchor_ms >= 0 && anchor_ms <= INT64_C(5853));
            if (anchor_ms < 0 || anchor_ms > INT64_C(5853))
                goto cleanup;
            decoder = turbo_codec_create_decoder("opus", &codec_config);
            check_not_null(decoder);
            if (!decoder) goto cleanup;
            while ((read_status = turbo_demuxer_read_packet_with_timing(
                        demuxer, &packet, &timing)) == 1) {
                check_true(packet.pts >= prior_pts);
                prior_pts = packet.pts;
                if (packet.stream_index == 1) {
                    int16_t samples[960];
                    size_t decoded_bytes = sizeof(samples);
                    size_t valid;
                    int64_t first_sample_ns;
                    check_equal(turbo_codec_decode(
                        decoder, packet.data, packet.size,
                        (uint8_t *)samples, &decoded_bytes), TURBO_CODEC_OK);
                    if (decoded_bytes > sizeof(samples) ||
                        decoded_bytes % sizeof(int16_t)) {
                        turbo_demuxer_free_packet(&packet);
                        goto cleanup;
                    }
                    ++audio_packets;
                    valid = decoded_bytes / sizeof(int16_t);
                    if (timing.discard_padding_ns > 0) {
                        uint64_t trim = (
                            (uint64_t)timing.discard_padding_ns *
                            UINT64_C(48000) + UINT64_C(500000000)) /
                            UINT64_C(1000000000);
                        check_true(trim <= valid);
                        if (trim > valid) {
                            turbo_demuxer_free_packet(&packet);
                            goto cleanup;
                        }
                        valid -= (size_t)trim;
                        ++trimmed;
                    }
                    first_sample_ns =
                        packet.pts * INT64_C(1000000) -
                        (int64_t)track.codec_delay_ns;
                    for (n = 0; n < valid; ++n) {
                        int64_t position_ns = first_sample_ns +
                            (int64_t)((n * UINT64_C(1000000000)) /
                                      UINT64_C(48000));
                        if (position_ns < target_ms * INT64_C(1000000))
                            ++before_target; /* decoded, never presented */
                        else if (position_ns <
                                 duration_ms * INT64_C(1000000)) {
                            ++kept_samples;
                            if (samples[n] != 0) ++nonzero;
                        }
                    }
                } else {
                    check_equal(timing.discard_padding_ns, INT64_C(0));
                }
                turbo_demuxer_free_packet(&packet);
                if (audio_packets > 400) goto cleanup;
            }
            check_equal(read_status, 0);
            check_true(audio_packets > 8);
            check_true(before_target >= 4u * 960u); /* >=80ms decoded */
            check_equal(trimmed, 1);
            check_true(nonzero > 0);
            check_equal(kept_samples, (size_t)(duration_ms - target_ms) * 48u);
        cleanup:
            turbo_demuxer_free_packet(&packet);
            turbo_codec_destroy(decoder);
            turbo_demuxer_destroy(demuxer);
            free(sink.bytes);
            free(pcm);
            free(frames);
            free(pixels);
        }
    }

    group("C11 float PCM / exact-duration Opus WebM v2") {
        it("preserves final audible waveform phase at every supported source rate and channel count") {
            static const uint32_t source_rates[] = {
                8000u, 12000u, 16000u, 24000u, 44100u, 48000u
            };
            size_t r;
            uint32_t channels;
            for (r = 0u; r < sizeof(source_rates) / sizeof(source_rates[0]); ++r) {
                for (channels = 1u; channels <= 2u; ++channels) {
                    uint32_t rate = source_rates[r];
                    av_profile_probe got = run_av_profile(
                        rate, channels, 10u, (size_t)rate * 3u / 10u);
                    uint64_t tail_samples =
                        (got.discard_padding_ns * UINT64_C(48000) +
                         UINT64_C(500000000)) / UINT64_C(1000000000);
                    check_equal(got.status, TM_ENCODER_OK);
                    check_equal(got.tracks, 2);
                    check_equal(got.audio_rate, 48000);
                    check_equal(got.audio_channels, (int)channels);
                    check_equal(got.video_packets, 3);
                    check_true(got.audio_packets > 0);
                    check_equal(got.trimmed_audio_packets, 1);
                    check_true(got.pts_monotonic);
                    check_equal(got.tail_samples_checked, 960u);
                    /* Exact count alone can pass while the last encoded
                     * Opus samples are silence or shifted before trimming.
                     * A positive signed waveform correlation requires
                     * real content, correct channel order, and correct
                     * pre-skip adjustment in the last 20ms. The threshold
                     * tolerates normal lossy Opus and resampler distortion. */
                    if (got.tail_samples_checked == 960u) {
                        check_true(got.tail_correlation_left >
                            (int64_t)got.tail_samples_checked * 32767 / 5);
                        if (channels == 2u)
                            check_true(got.tail_correlation_right >
                                (int64_t)got.tail_samples_checked * 32767 / 8);
                    }
                    check_true(got.decoded_samples_per_channel >=
                               (size_t)got.pre_skip + (size_t)tail_samples);
                    if (got.decoded_samples_per_channel >=
                            (size_t)got.pre_skip + (size_t)tail_samples)
                        check_equal(got.decoded_samples_per_channel -
                            (size_t)got.pre_skip -
                            (size_t)tail_samples, 14400u);
                    check_equal(got.duration_ms, 300);
                    check_equal(got.writes, 1u);
                }
            }
        }
        it("resamples 44.1kHz stereo with original rate in OpusHead") {
            av_profile_probe got = run_av_profile(44100u, 2u, 10u, 13230u);
            uint64_t tail = (got.discard_padding_ns * 48000u +
                             UINT64_C(500000000)) / UINT64_C(1000000000);
            check_equal(got.status, TM_ENCODER_OK);
            check_equal(got.tracks, 2);
            check_equal(got.audio_channels, 2);
            check_equal(got.audio_rate, 48000);
            /* WebM OpusHead and Track SamplingFrequency must match
             * the actual 48kHz post-resample codec input. */
            check_equal(got.opus_input_rate, (uint32_t)got.audio_rate);
            check_equal(got.opus_input_rate, 48000u);
            check_true(got.pre_skip > 0u);
            check_true(got.has_codec_delay);
            check_true(got.has_preroll);
            check_true(got.has_padding);
            check_equal(got.trimmed_audio_packets, 1);
            check_equal(got.padding_from_demuxer_ns,
                        (int64_t)got.discard_padding_ns);
            check_equal(got.codec_delay_ns,
                        (uint64_t)got.pre_skip * UINT64_C(1000000000) /
                        UINT64_C(48000));
            check_equal(got.seek_preroll_ns, UINT64_C(80000000));
            check_equal(got.parsed_codec_delay_ns, got.codec_delay_ns);
            check_equal(got.parsed_seek_preroll_ns, got.seek_preroll_ns);
            check_equal(got.video_packets, 3);
            check_equal(got.audio_packets, (int)((14400u + got.pre_skip + 959u) / 960u));
            check_true(got.pts_monotonic);
            check_true(got.nonzero_left > 0);
            check_true(got.nonzero_right > 0);
            check_equal(got.decoded_samples_per_channel -
                        (size_t)got.pre_skip - (size_t)tail, 14400u);
            check_equal(got.duration_ms, 300);
            check_equal(got.writes, 1u);
        }
        it("supports partial final Opus frames at 48kHz stereo and 24kHz mono") {
            av_profile_probe stereo = run_av_profile(48000u, 2u, 24u, 6000u);
            av_profile_probe mono = run_av_profile(24000u, 1u, 24u, 3000u);
            av_profile_probe low_rate = run_av_profile(8000u, 1u, 10u, 2400u);
            uint64_t stereo_tail =
                (stereo.discard_padding_ns * 48000u +
                 UINT64_C(500000000)) / UINT64_C(1000000000);
            uint64_t mono_tail =
                (mono.discard_padding_ns * 48000u +
                 UINT64_C(500000000)) / UINT64_C(1000000000);
            check_equal(stereo.status, TM_ENCODER_OK);
            check_equal(stereo.tracks, 2);
            check_equal(stereo.audio_channels, 2);
            check_equal(stereo.opus_input_rate, 48000u);
            check_equal(stereo.video_packets, 3);
            check_true(stereo.audio_packets > 0);
            check_true(stereo.nonzero_left > 0 && stereo.nonzero_right > 0);
            check_true(stereo.pts_monotonic);
            check_equal(stereo.parsed_codec_delay_ns,
                        stereo.codec_delay_ns);
            check_equal(stereo.parsed_seek_preroll_ns,
                        stereo.seek_preroll_ns);
            check_equal(stereo.trimmed_audio_packets, 1);
            check_equal(stereo.padding_from_demuxer_ns,
                        (int64_t)stereo.discard_padding_ns);
            check_equal(stereo.decoded_samples_per_channel -
                        stereo.pre_skip - (size_t)stereo_tail, 6000u);
            check_equal(stereo.duration_ms, 125);
            check_equal(mono.status, TM_ENCODER_OK);
            check_equal(mono.tracks, 2);
            check_equal(mono.audio_channels, 1);
            check_equal(mono.opus_input_rate, 48000u);
            check_equal(mono.video_packets, 3);
            check_true(mono.nonzero_left > 0);
            check_true(mono.pts_monotonic);
            check_equal(mono.parsed_codec_delay_ns, mono.codec_delay_ns);
            check_equal(mono.parsed_seek_preroll_ns, mono.seek_preroll_ns);
            check_equal(mono.trimmed_audio_packets, 1);
            check_equal(mono.padding_from_demuxer_ns,
                        (int64_t)mono.discard_padding_ns);
            check_equal(mono.decoded_samples_per_channel -
                        mono.pre_skip - (size_t)mono_tail, 6000u);
            check_equal(mono.duration_ms, 125);
            check_equal(low_rate.status, TM_ENCODER_OK);
            check_equal(low_rate.opus_input_rate, 48000u);
            check_equal(low_rate.video_packets, 3);
            check_true(low_rate.audio_packets > 0);
            check_true(low_rate.nonzero_left > 0);
            check_true(low_rate.pts_monotonic);
            check_equal(low_rate.parsed_codec_delay_ns,
                        low_rate.codec_delay_ns);
            check_equal(low_rate.parsed_seek_preroll_ns,
                        low_rate.seek_preroll_ns);
            check_equal(low_rate.trimmed_audio_packets, 1);
            check_equal(low_rate.padding_from_demuxer_ns,
                        (int64_t)low_rate.discard_padding_ns);
            check_equal(low_rate.decoded_samples_per_channel -
                        low_rate.pre_skip -
                        (size_t)((low_rate.discard_padding_ns * UINT64_C(48000) +
                          UINT64_C(500000000)) / UINT64_C(1000000000)), 14400u);
            check_equal(low_rate.duration_ms, 300);
        }
        it("encodes real non-silent Opus and VP8 packets in two WebM tracks") {
            uint8_t pixels[3][16 * 16 * 3];
            tm_encoder_video_frame_v1 frames[3];
            tm_encoder_video_v1 video;
            float pcm[14400];
            tm_encoder_audio_v1 audio = TM_ENCODER_AUDIO_V1_INIT;
            video_result sink = {0};
            turbo_demuxer_config_t demux_cfg = {0};
            turbo_stream_info_t info = {0};
            turbo_demuxer_packet_t packet = {0};
            turbo_demuxer_packet_timing_v1 timing =
                TURBO_DEMUXER_PACKET_TIMING_V1_INIT;
            turbo_demuxer_t *demuxer = NULL;
            turbo_audio_codec_config_t decoder_config = {0};
            turbo_codec_t *decoder = NULL;
            size_t written = 0, i, decoded_samples = 0;
            uint64_t padding_ns = 0, delay_ns = 0, preroll_ns = 0;
            uint16_t pre_skip = 0;
            int videos = 0, audios = 0, nonzero = 0;
            int64_t previous_pts = -1;
            prepare_frames(pixels, frames, 3);
            video = init_video(frames, 3);
            for (i = 0; i < 14400u; ++i)
                pcm[i] = i % 100u < 50u ? 0.65f : -0.65f;
            audio.samples = pcm;
            audio.sample_count = 14400u;
            audio.sample_capacity_bytes = sizeof(pcm);
            sink.capacity = video.max_output_bytes;
            sink.bytes = (uint8_t *)malloc(sink.capacity);
            check_not_null(sink.bytes);
            if (!sink.bytes) return;
            check_equal(tm_encoder_webm_encode_av_v2(
                &video, &audio, collect_video, &sink,
                NULL, NULL, &written), TM_ENCODER_OK);
            check_equal(written, sink.size);
            check_equal(sink.writes, 1u);
            if (written == 0) { free(sink.bytes); return; }
            turbo_demuxer_registry_init();
            demux_cfg.data = sink.bytes;
            demux_cfg.data_size = sink.size;
            demuxer = turbo_demuxer_create_by_name("webm", &demux_cfg);
            check_not_null(demuxer);
            if (demuxer) {
                check_equal(turbo_demuxer_open(demuxer), 0);
                check_equal(turbo_demuxer_get_stream_count(demuxer), 2);
                check_equal(turbo_demuxer_get_stream_info(demuxer, 1, &info), 0);
                check_equal(info.codec_name, "opus");
                check_equal(info.channels, 1);
                check_true(info.extradata != NULL);
                check_true(info.extradata_size >= 19u);
                if (info.extradata && info.extradata_size >= 19u)
                    pre_skip = (uint16_t)((unsigned)info.extradata[10] |
                                          ((unsigned)info.extradata[11] << 8));
                check_true(pre_skip > 0u);
                check_true(find_webm_positive_element(
                    sink.bytes, sink.size, 0x56AA, &delay_ns));
                check_true(find_webm_positive_element(
                    sink.bytes, sink.size, 0x56BB, &preroll_ns));
                check_true(find_webm_positive_element(
                    sink.bytes, sink.size, 0x75A2, &padding_ns));
                check_equal(delay_ns,
                    (uint64_t)pre_skip * UINT64_C(1000000000) / UINT64_C(48000));
                check_equal(preroll_ns, UINT64_C(80000000));
                decoder_config.sample_rate = 48000;
                decoder_config.channels = 1;
                decoder_config.bitrate = 64000;
                decoder_config.frame_size_ms = 20;
                decoder_config.complexity = 5;
                decoder = turbo_codec_create_decoder("opus", &decoder_config);
                check_not_null(decoder);
                while (turbo_demuxer_read_packet_with_timing(
                           demuxer, &packet, &timing) == 1) {
                    if (timing.discard_padding_ns) {
                        check_equal(packet.stream_index, 1);
                        check_equal(timing.discard_padding_ns,
                                    (int64_t)padding_ns);
                    }
                    check_true(packet.pts >= previous_pts);
                    previous_pts = packet.pts;
                    if (packet.stream_index == 0) {
                        ++videos;
                    } else if (packet.stream_index == 1) {
                        int16_t decoded[960];
                        size_t decoded_size = sizeof(decoded);
                        ++audios;
                        if (decoder && turbo_codec_decode(
                                decoder, packet.data, packet.size,
                                (uint8_t *)decoded, &decoded_size) == TURBO_CODEC_OK) {
                            size_t n;
                            decoded_samples += decoded_size / sizeof(decoded[0]);
                            for (n = 0; n < decoded_size / sizeof(decoded[0]); ++n)
                                if (decoded[n] != 0) ++nonzero;
                        }
                    }
                    turbo_demuxer_free_packet(&packet);
                    if (audios + videos > 18) break;
                }
                turbo_demuxer_free_packet(&packet);
                check_equal(videos, 3);
                check_equal(audios,
                    (int)((audio.sample_count + pre_skip + 959u) / 960u));
                check_true(nonzero > 0);
                {
                    uint64_t tail_samples =
                        (padding_ns * UINT64_C(48000) +
                         UINT64_C(500000000)) / UINT64_C(1000000000);
                    check_true(decoded_samples >= (size_t)pre_skip + tail_samples);
                    if (decoded_samples >= (size_t)pre_skip + tail_samples)
                        check_equal(decoded_samples - (size_t)pre_skip -
                                    (size_t)tail_samples, audio.sample_count);
                }
                {
                    turbo_container_metadata_t metadata = {0};
                    check_equal(turbo_demuxer_get_metadata(
                        demuxer, &metadata), 0);
                    check_equal(metadata.duration_ms, 300);
                }
                turbo_codec_destroy(decoder);
                turbo_demuxer_destroy(demuxer);
            }
            free(sink.bytes);
        }
        it("rejects wrong sample rate, incomplete Opus frame, invalid PCM and cancel") {
            uint8_t pixels[3][16 * 16 * 3];
            tm_encoder_video_frame_v1 frames[3];
            tm_encoder_video_v1 video;
            float pcm[14400] = {0};
            tm_encoder_audio_v1 audio = TM_ENCODER_AUDIO_V1_INIT;
            video_result sink = {0};
            size_t written = 111;
            prepare_frames(pixels, frames, 3);
            video = init_video(frames, 3);
            audio.samples = pcm;
            audio.sample_count = 14400u;
            audio.sample_capacity_bytes = sizeof(pcm);
            audio.sample_rate = 96000; /* no unknown-rate fallback */
            check_equal(tm_encoder_webm_encode_av_v2(
                &video, &audio, collect_video, &sink, NULL, NULL,
                &written), TM_ENCODER_EUNSUPPORTED);
            check_equal(written, 0u);
            audio.sample_rate = 44100; /* supported rate, wrong duration */
            check_equal(tm_encoder_webm_encode_av_v2(
                &video, &audio, collect_video, &sink, NULL, NULL,
                &written), TM_ENCODER_EUNSUPPORTED);
            audio.sample_rate = 48000;
            audio.sample_count = 14401u;
            check_equal(tm_encoder_webm_encode_av_v2(
                &video, &audio, collect_video, &sink, NULL, NULL,
                &written), TM_ENCODER_EUNSUPPORTED);
            audio.sample_count = 14400u;
            audio.sample_capacity_bytes = 1;
            check_equal(tm_encoder_webm_encode_av_v2(
                &video, &audio, collect_video, &sink, NULL, NULL,
                &written), TM_ENCODER_EINVAL);
            audio.sample_capacity_bytes = sizeof(pcm);
            pcm[0] = NAN; /* NaN: cannot become valid PCM16 */
            check_equal(tm_encoder_webm_encode_av_v2(
                &video, &audio, collect_video, &sink, NULL, NULL,
                &written), TM_ENCODER_EINVAL);
            pcm[0] = 0;
            pcm[14399] = INFINITY;
            check_equal(tm_encoder_webm_encode_av_v2(
                &video, &audio, collect_video, &sink, NULL, NULL,
                &written), TM_ENCODER_EINVAL);
            pcm[14399] = 0;
            check_equal(tm_encoder_webm_encode_av_v2(
                NULL, &audio, collect_video, &sink, NULL, NULL,
                &written), TM_ENCODER_EINVAL);
            check_equal(tm_encoder_webm_encode_av_v2(
                &video, &audio, collect_video, &sink, cancel_now,
                NULL, &written), TM_ENCODER_ECANCELLED);
            check_equal(sink.writes, 0u);
            check_equal(written, 0u);
        }
    }

    group("exact TurboMedia codec and muxer") {
        it("encodes RGB input into native VP8 WebM with real demuxed frames") {
            uint8_t pixels[3][16 * 16 * 3];
            tm_encoder_video_frame_v1 frames[3];
            tm_encoder_video_v1 options;
            video_result sink = {0};
            turbo_demuxer_config_t demux_config = {0};
            turbo_demuxer_t *demuxer = NULL;
            turbo_stream_info_t stream = {0};
            turbo_demuxer_packet_t packet = {0};
            size_t written = 0;
            int packet_count = 0;
            int64_t last_pts = -1;
            prepare_frames(pixels, frames, 3);
            options = init_video(frames, 3);
            sink.capacity = options.max_output_bytes;
            sink.bytes = (uint8_t *)malloc(sink.capacity);
            check_not_null(sink.bytes);
            if (!sink.bytes) return;
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, NULL, NULL, &written),
                        TM_ENCODER_OK);
            check_equal(sink.size, written);
            check_equal(sink.writes, 1u);
            check_true(written > 32);
            if (written > 32) {
                static const uint8_t ebml[] = {0x1a, 0x45, 0xdf, 0xa3};
                check_true(memcmp(sink.bytes, ebml, sizeof(ebml)) == 0);
                turbo_demuxer_registry_init();
                demux_config.data = sink.bytes;
                demux_config.data_size = written;
                demuxer = turbo_demuxer_create_by_name("webm", &demux_config);
                check_not_null(demuxer);
                if (demuxer) {
                    check_equal(turbo_demuxer_open(demuxer), 0);
                    check_equal(turbo_demuxer_get_stream_count(demuxer), 1);
                    check_equal(turbo_demuxer_get_stream_info(demuxer, 0, &stream), 0);
                    check_equal(stream.width, 16);
                    check_equal(stream.height, 16);
                    check_equal(stream.codec_name, "vp8");
                    while (turbo_demuxer_read_packet(demuxer, &packet) == 1) {
                        check_true(packet.size > 0);
                        check_true(packet.pts > last_pts);
                        last_pts = packet.pts;
                        ++packet_count;
                        turbo_demuxer_free_packet(&packet);
                        if (packet_count > 3)
                            break;
                    }
                    turbo_demuxer_free_packet(&packet);
                    check_equal(packet_count, 3);
                    turbo_demuxer_destroy(demuxer);
                }
            }
            free(sink.bytes);
        }

        it("rejects audio silently dropping, bad stride, and invalid ABI") {
            uint8_t pixels[1][16 * 16 * 3];
            tm_encoder_video_frame_v1 frames[1];
            tm_encoder_video_v1 options;
            video_result sink = {0};
            size_t written = 123;
            prepare_frames(pixels, frames, 1);
            options = init_video(frames, 1);
            options.audio_bytes = 4;
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, NULL, NULL, &written),
                        TM_ENCODER_EUNSUPPORTED);
            check_equal(written, 0u);
            options.audio_bytes = 0;
            options.version = 2;
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, NULL, NULL, &written),
                        TM_ENCODER_EVERSION);
            options.version = TM_ENCODER_VIDEO_V1;
            options.width = 15; /* VP8 I420 requires even dimensions. */
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, NULL, NULL, &written),
                        TM_ENCODER_EINVAL);
            options.width = 16;
            frames[0].stride_bytes = 1;
            sink.bytes = (uint8_t *)malloc(1024 * 1024);
            sink.capacity = 1024 * 1024;
            check_not_null(sink.bytes);
            if (sink.bytes) {
                check_equal(tm_encoder_webm_encode(&options, collect_video,
                                                   &sink, NULL, NULL, &written),
                            TM_ENCODER_EINVAL);
                check_equal(sink.writes, 0u);
                free(sink.bytes);
            }
        }

        it("refuses undersized output and cancellation without emitting a container") {
            uint8_t pixels[3][16 * 16 * 3];
            tm_encoder_video_frame_v1 frames[3];
            tm_encoder_video_v1 options;
            video_result sink = {0};
            size_t written = 3u;
            cancel_after middle = {0u, 1u};
            prepare_frames(pixels, frames, 3);
            options = init_video(frames, 3);
            options.max_output_bytes = 1;
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, NULL, NULL, &written),
                        TM_ENCODER_ECAPACITY);
            check_equal(written, 0u);
            options.max_output_bytes = 1024 * 1024;
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, cancel_now, NULL, &written),
                        TM_ENCODER_ECANCELLED);
            check_equal(sink.writes, 0u);
            check_equal(tm_encoder_webm_encode(&options, collect_video,
                                               &sink, cancel_after_frames,
                                               &middle, &written),
                        TM_ENCODER_ECANCELLED);
            check_equal(sink.writes, 0u);
        }
    }
}
