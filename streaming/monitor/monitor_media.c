#include "monitor.h"

#include <core/clock.h>
#include <core/fmt.h>
#include <file/fs.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <stdlib.h>

enum {
    OPEN_TIMEOUT_MS = 4000,
    FRAME_TIMEOUT_MS = 3000,
    RECONNECT_MS = 1000,
    PREVIEW_INTERVAL_MS = 400,
    PREVIEW_WIDTH = 480,
    PREVIEW_HEIGHT = 270,
    CLIP_WINDOW_MS = 8000,
    CLIP_BYTES = 8 * 1024 * 1024,
    CLIP_PACKETS = 600
};

struct monitor_clip {
    monitor_source_t *source;
    uint64_t incident;
    uint64_t started_ms;
    size_t bytes, count;
    bool sealed;
    AVCodecParameters *codec;
    AVRational time_base;
    AVPacket *packets[CLIP_PACKETS];
};

typedef struct monitor_input {
    monitor_source_t *source;
    uint64_t deadline;
    uint64_t next_preview;
    uint64_t preview_pts;
    AVFormatContext *format;
    AVCodecContext *decoder, *jpeg;
    AVFrame *frame, *image;
    struct SwsContext *scale;
    monitor_clip_t *clip, *previous_clip;
} monitor_input_t;

tstr monitor_clip_path(const monitor_source_t *source, uint64_t incident) {
    return tstr_format("{}/camera-{}-{}.mp4", source->monitor->archive,
                       source->id, incident);
}

static void clip_destroy(monitor_clip_t *clip) {
    if (!clip) return;
    for (size_t i = 0; i < clip->count; ++i) av_packet_free(&clip->packets[i]);
    avcodec_parameters_free(&clip->codec);
    free(clip);
}

static monitor_clip_t *clip_create(monitor_source_t *source, const AVStream *video) {
    monitor_clip_t *clip = calloc(1, sizeof(*clip));
    if (!clip) return NULL;
    clip->codec = avcodec_parameters_alloc();
    if (!clip->codec || avcodec_parameters_copy(clip->codec, video->codecpar) < 0) {
        clip_destroy(clip);
        return NULL;
    }
    clip->source = source;
    clip->started_ms = monotonic_ms();
    clip->time_base = video->time_base;
    return clip;
}

static void clip_capture(monitor_input_t *input, const AVStream *video,
                         const AVPacket *packet) {
    monitor_clip_t *clip = input->clip;
    bool key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
    int64_t timestamp = packet->dts == AV_NOPTS_VALUE ? packet->pts : packet->dts;
    if (timestamp == AV_NOPTS_VALUE || packet->size <= 0) return;
    if (key && (!clip || clip->sealed || monotonic_ms() - clip->started_ms >= CLIP_WINDOW_MS ||
                clip->count >= CLIP_PACKETS || (size_t)packet->size > CLIP_BYTES - clip->bytes)) {
        monitor_clip_t *replacement = clip_create(input->source, video);
        if (!replacement) return;
        if (clip && clip->count >= 2) {
            clip_destroy(input->previous_clip);
            input->previous_clip = clip;
        } else {
            clip_destroy(clip);
        }
        input->clip = clip = replacement;
    }
    if (!clip || clip->sealed) return;
    if (clip->count == CLIP_PACKETS || (size_t)packet->size > CLIP_BYTES - clip->bytes) {
        clip->sealed = true;
        return;
    }
    AVPacket *copy = av_packet_clone(packet);
    if (!copy) {
        clip->sealed = true;
        return;
    }
    if (copy->dts == AV_NOPTS_VALUE) copy->dts = timestamp;
    if (copy->pts == AV_NOPTS_VALUE) copy->pts = timestamp;
    clip->packets[clip->count++] = copy;
    clip->bytes += (size_t)copy->size;
}

static bool clip_write(monitor_clip_t *clip) {
    tstr path = monitor_clip_path(clip->source, clip->incident);
    tstr temporary = path ? tstr_format("{}.part", path) : NULL;
    AVFormatContext *output = NULL;
    AVStream *video;
    int64_t base = clip->packets[0]->dts;
    bool published = false;
    if (!temporary || avformat_alloc_output_context2(&output, NULL, "mp4", temporary) < 0)
        goto done;
    video = avformat_new_stream(output, NULL);
    if (!video || avcodec_parameters_copy(video->codecpar, clip->codec) < 0) goto done;
    video->codecpar->codec_tag = 0;
    video->time_base = clip->time_base;
    if (avio_open(&output->pb, temporary, AVIO_FLAG_WRITE) < 0 ||
        avformat_write_header(output, NULL) < 0)
        goto done;
    for (size_t i = 0; i < clip->count; ++i) {
        AVPacket *packet = clip->packets[i];
        packet->stream_index = video->index;
        packet->dts = av_sat_sub64(packet->dts, base);
        packet->pts = av_sat_sub64(packet->pts, base);
        av_packet_rescale_ts(packet, clip->time_base, video->time_base);
        if (av_interleaved_write_frame(output, packet) < 0) goto done;
    }
    if (av_write_trailer(output) < 0 || avio_closep(&output->pb) < 0) goto done;
    published = fs_rename(temporary, path) == 0;
done:
    if (output) {
        avio_closep(&output->pb);
        avformat_free_context(output);
    }
    if (temporary && !published) (void)fs_unlink(temporary);
    tstr_free(temporary);
    tstr_free(path);
    return published;
}

static void record_worker(void *context) {
    monitor_t *monitor = context;
    for (;;) {
        monitor_clip_t *clip = NULL;
        sync_mutex_lock(&monitor->archive_mutex);
        for (unsigned i = 0; i < monitor->count; ++i) {
            if (monitor->sources[i].job) {
                clip = monitor->sources[i].job;
                monitor->sources[i].job = NULL;
                break;
            }
        }
        if (!clip) {
            bool closed = monitor->archive_closed;
            if (!closed) sync_cond_wait(&monitor->archive_ready, &monitor->archive_mutex);
            sync_mutex_unlock(&monitor->archive_mutex);
            if (closed) break;
            continue;
        }
        sync_mutex_unlock(&monitor->archive_mutex);
        monitor_clip_status_t status = clip_write(clip) ? MONITOR_CLIP_READY : MONITOR_CLIP_UNAVAILABLE;
        monitor_source_t *source = clip->source;
        sync_mutex_lock(&source->mutex);
        if (source->state.incident == clip->incident) source->state.clip = status;
        sync_mutex_unlock(&source->mutex);
        clip_destroy(clip);
    }
}

static void source_lost(monitor_input_t *input, monitor_signal_t signal) {
    monitor_source_t *source = input->source;
    monitor_t *monitor = source->monitor;
    sync_mutex_lock(&source->mutex);
    source->state.signal = signal;
    if (!source->state.incident || source->state.recovered_ms) {
        ++source->state.incident;
        source->state.opened_ms = realtime_ms();
        source->state.recovered_ms = 0;
        source->state.acknowledged = false;
        source->state.clip = MONITOR_CLIP_UNAVAILABLE;
        sync_mutex_lock(&monitor->archive_mutex);
        monitor_clip_t **candidate = input->clip && input->clip->count >= 2
            ? &input->clip : &input->previous_clip;
        if (*candidate && !source->job &&
            monitor->clips_submitted < MONITOR_MAX_CLIPS) {
            (*candidate)->incident = source->state.incident;
            source->job = *candidate;
            *candidate = NULL;
            ++monitor->clips_submitted;
            source->state.clip = MONITOR_CLIP_PENDING;
            sync_cond_signal(&monitor->archive_ready);
        }
        sync_mutex_unlock(&monitor->archive_mutex);
    }
    sync_mutex_unlock(&source->mutex);
}

static int interrupt_input(void *context) {
    monitor_input_t *input = context;
    return atomic_load(&input->source->monitor->stop) || monotonic_ms() >= input->deadline;
}

static void release_jpeg(void *data, void *context) {
    AVPacket *packet = context;
    (void)data;
    av_packet_free(&packet);
}

static bool prepare_preview(monitor_input_t *input, const AVFrame *frame) {
    int width = PREVIEW_WIDTH, height = PREVIEW_HEIGHT;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec || frame->width <= 0 || frame->height <= 0) return false;
    if ((int64_t)frame->width * height > (int64_t)frame->height * width)
        height = (int)((int64_t)width * frame->height / frame->width) & ~1;
    else
        width = (int)((int64_t)height * frame->width / frame->height) & ~1;
    if (width < 2 || height < 2) return false;
    input->jpeg = avcodec_alloc_context3(codec);
    input->image = av_frame_alloc();
    if (!input->jpeg || !input->image) return false;
    input->jpeg->width = input->image->width = width;
    input->jpeg->height = input->image->height = height;
    input->jpeg->time_base = (AVRational){1, 1000};
    input->jpeg->pix_fmt = input->image->format = AV_PIX_FMT_YUVJ420P;
    input->jpeg->color_range = input->image->color_range = AVCOL_RANGE_JPEG;
    input->jpeg->thread_count = 1;
    return avcodec_open2(input->jpeg, codec, NULL) >= 0 &&
           av_frame_get_buffer(input->image, 32) >= 0;
}

static void render_preview(monitor_input_t *input) {
    AVFrame *frame = input->frame;
    AVPacket *packet = NULL;
    mem_buffer_t *next = NULL;
    if (!input->jpeg && !prepare_preview(input, frame)) {
        avcodec_free_context(&input->jpeg);
        av_frame_free(&input->image);
        return;
    }
    input->scale = sws_getCachedContext(input->scale, frame->width, frame->height, frame->format,
        input->image->width, input->image->height, input->image->format,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!input->scale || av_frame_make_writable(input->image) < 0 ||
        sws_scale(input->scale, (const uint8_t *const *)frame->data, frame->linesize,
                  0, frame->height, input->image->data, input->image->linesize) != input->image->height)
        return;
    input->image->pts = (int64_t)input->preview_pts++;
    packet = av_packet_alloc();
    if (!packet) return;
    if (avcodec_send_frame(input->jpeg, input->image) >= 0 &&
        avcodec_receive_packet(input->jpeg, packet) >= 0 &&
        packet->size > 0 && packet->size <= MONITOR_PREVIEW_BYTES)
        next = mem_wrap_external(packet->data, (size_t)packet->size, release_jpeg, packet);
    if (!next) {
        av_packet_free(&packet);
        return;
    }
    monitor_source_t *source = input->source;
    sync_mutex_lock(&source->mutex);
    mem_buffer_t *previous = source->preview;
    source->preview = next;
    ++source->state.preview_version;
    sync_mutex_unlock(&source->mutex);
    if (previous) mem_buffer_release(previous);
}

static int open_input(monitor_input_t *input) {
    AVDictionary *options = NULL;
    const AVCodec *codec = NULL;
    int stream = AVERROR(ENOMEM);
    input->format = avformat_alloc_context();
    if (!input->format) return stream;
    input->format->interrupt_callback = (AVIOInterruptCB){interrupt_input, input};
    input->deadline = monotonic_ms() + OPEN_TIMEOUT_MS;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "allowed_media_types", "video", 0);
    stream = avformat_open_input(&input->format, input->source->url, NULL, &options);
    av_dict_free(&options);
    if (stream < 0) return stream;
    int result = avformat_find_stream_info(input->format, NULL);
    if (result < 0) return result;
    stream = av_find_best_stream(input->format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0) return stream;
    AVCodecParameters *parameters = input->format->streams[stream]->codecpar;
    if (parameters->codec_id != AV_CODEC_ID_H264) return AVERROR(ENOSYS);
    input->decoder = avcodec_alloc_context3(codec);
    input->frame = av_frame_alloc();
    if (!input->decoder || !input->frame) return AVERROR(ENOMEM);
    input->decoder->thread_count = 1;
    input->decoder->max_pixels = 3840 * 2160;
    result = avcodec_parameters_to_context(input->decoder, parameters);
    if (result < 0) return result;
    result = avcodec_open2(input->decoder, codec, NULL);
    return result < 0 ? result : stream;
}

static void run_input(monitor_input_t *input, int index) {
    AVPacket *packet = av_packet_alloc();
    if (!packet) return;
    monitor_source_t *source = input->source;
    input->deadline = monotonic_ms() + FRAME_TIMEOUT_MS;
    while (!interrupt_input(input) && av_read_frame(input->format, packet) >= 0) {
        if (packet->stream_index == index && avcodec_send_packet(input->decoder, packet) >= 0) {
            clip_capture(input, input->format->streams[index], packet);
            while (avcodec_receive_frame(input->decoder, input->frame) >= 0) {
                uint64_t now = monotonic_ms();
                input->deadline = now + FRAME_TIMEOUT_MS;
                sync_mutex_lock(&source->mutex);
                source->state.signal = MONITOR_LIVE;
                source->state.last_frame_ms = realtime_ms();
                if (source->state.incident && !source->state.recovered_ms)
                    source->state.recovered_ms = source->state.last_frame_ms;
                sync_mutex_unlock(&source->mutex);
                if (now >= input->next_preview) {
                    input->next_preview = now + PREVIEW_INTERVAL_MS;
                    render_preview(input);
                }
                av_frame_unref(input->frame);
            }
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
}

static void source_worker(void *context) {
    monitor_source_t *source = context;
    while (!atomic_load(&source->monitor->stop)) {
        monitor_input_t input = {.source = source};
        int stream = open_input(&input);
        if (stream >= 0) run_input(&input, stream);
        bool unsupported = stream == AVERROR(ENOSYS) || stream == AVERROR_DECODER_NOT_FOUND;
        if (!atomic_load(&source->monitor->stop))
            source_lost(&input, unsupported ? MONITOR_UNSUPPORTED : MONITOR_OFFLINE);
        clip_destroy(input.clip);
        clip_destroy(input.previous_clip);
        sws_freeContext(input.scale);
        av_frame_free(&input.image);
        av_frame_free(&input.frame);
        avcodec_free_context(&input.jpeg);
        avcodec_free_context(&input.decoder);
        avformat_close_input(&input.format);
        if (unsupported) break;
        for (unsigned wait = 0; wait < RECONNECT_MS && !atomic_load(&source->monitor->stop); wait += 50)
            sleep_ms(50);
    }
}

bool monitor_start(monitor_t *monitor) {
    atomic_init(&monitor->stop, false);
    sync_mutex_init(&monitor->archive_mutex);
    sync_cond_init(&monitor->archive_ready);
    if (!monitor->archive_mutex || !monitor->archive_ready) return false;
    for (unsigned i = 0; i < monitor->count; ++i) {
        monitor_source_t *source = &monitor->sources[i];
        source->monitor = monitor;
        source->id = i + 1;
        sync_mutex_init(&source->mutex);
        if (!source->mutex) return false;
    }
    if (sync_thread_create(&monitor->archive_thread, record_worker, monitor) != 0) return false;
    for (unsigned i = 0; i < monitor->count; ++i) {
        monitor_source_t *source = &monitor->sources[i];
        if (sync_thread_create(&source->thread, source_worker, source) != 0) return false;
    }
    return true;
}

void monitor_stop(monitor_t *monitor) {
    atomic_store(&monitor->stop, true);
    for (unsigned i = 0; i < monitor->count; ++i) {
        monitor_source_t *source = &monitor->sources[i];
        if (source->thread) {
            (void)sync_thread_join(&source->thread);
            sync_thread_destroy(&source->thread);
        }
    }
    if (monitor->archive_thread) {
        sync_mutex_lock(&monitor->archive_mutex);
        monitor->archive_closed = true;
        sync_cond_signal(&monitor->archive_ready);
        sync_mutex_unlock(&monitor->archive_mutex);
        (void)sync_thread_join(&monitor->archive_thread);
        sync_thread_destroy(&monitor->archive_thread);
    }
    for (unsigned i = 0; i < monitor->count; ++i) {
        monitor_source_t *source = &monitor->sources[i];
        if (source->preview) mem_buffer_release(source->preview);
        sync_mutex_destroy(&source->mutex);
    }
    sync_cond_destroy(&monitor->archive_ready);
    sync_mutex_destroy(&monitor->archive_mutex);
}
