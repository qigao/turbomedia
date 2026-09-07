#include "turbo_player.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE player_thread_t;
static void player_sleep_ms(unsigned int ms) {
    Sleep(ms);
}
#else
#include <pthread.h>
#include <time.h>
typedef pthread_t player_thread_t;
static void player_sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

enum {
    PLAYER_DEFAULT_AUDIO_BUFFER_MS = 500,
    PLAYER_DRAIN_MARGIN_MS = 1000,
    PLAYER_DRAIN_SLICE_MS = 10
};

enum {
    PLAYER_TERMINAL_ACTIVE = 0,
    PLAYER_TERMINAL_CANCELLED,
    PLAYER_TERMINAL_COMPLETED
};

struct turbo_player_s {
    AVFormatContext *format;
    AVCodecContext *audio_codec;
    AVCodecContext *video_codec;
    SwrContext *resampler;
    struct SwsContext *scaler;
    AVPacket *packet;
    AVFrame *frame;

    salts_playback_t *playback;
    turbo_player_config_t config;

    int audio_stream;
    int video_stream;
    int audio_rate;
    int audio_channels;
    _Atomic int stopped;
    _Atomic int paused;
    _Atomic int running;
    _Atomic int thread_started;
    _Atomic int terminal;
    _Atomic uint64_t seek_sequence;
    _Atomic int64_t seek_target_ms;
    uint64_t consumed_seek_sequence;
    int64_t discard_until_ms;
    int last_result;
    player_thread_t thread;

    uint8_t *audio_buf;
    size_t audio_buf_cap;
    uint8_t *video_buf;
    size_t video_buf_cap;

    turbo_player_video_cb video_cb;
    void *video_user_data;
    turbo_player_audio_cb audio_cb;
    void *audio_user_data;
    turbo_player_complete_cb complete_cb;
    void *complete_user_data;
};

static int64_t frame_pts_ms(const AVFrame *frame, const AVStream *stream) {
    int64_t pts = frame->best_effort_timestamp;
    if (!frame || !stream || pts == AV_NOPTS_VALUE) {
        return -1;
    }
    return av_rescale_q(pts, stream->time_base, (AVRational){1, 1000});
}

static int wait_if_paused(turbo_player_t *player) {
    if (!player) {
        return TURBO_PLAYER_ERR_PLAYBACK;
    }
    int playback_paused = 0;
    while (player->paused && !player->stopped) {
        if (!playback_paused && player->playback) {
            if (salts_playback_pause(player->playback) != SALTS_PLAYBACK_OK) {
                return TURBO_PLAYER_ERR_PLAYBACK;
            }
            playback_paused = 1;
        }
        player_sleep_ms(10);
    }
    if (player->stopped) {
        return TURBO_PLAYER_ERR_PLAYBACK;
    }
    if (playback_paused &&
        salts_playback_resume(player->playback) != SALTS_PLAYBACK_OK) {
        return TURBO_PLAYER_ERR_PLAYBACK;
    }
    return TURBO_PLAYER_OK;
}

static int apply_pending_seek(turbo_player_t *player) {
    if (!player) {
        return TURBO_PLAYER_OK;
    }

    uint64_t sequence_before;
    uint64_t sequence_after;
    int64_t target_ms;
    do {
        sequence_before =
            atomic_load_explicit(&player->seek_sequence, memory_order_acquire);
        if ((sequence_before & 1u) != 0u ||
            sequence_before == player->consumed_seek_sequence) {
            return TURBO_PLAYER_OK;
        }
        target_ms =
            atomic_load_explicit(&player->seek_target_ms, memory_order_relaxed);
        sequence_after =
            atomic_load_explicit(&player->seek_sequence, memory_order_acquire);
    } while (sequence_before != sequence_after);

    player->consumed_seek_sequence = sequence_after;
    player->discard_until_ms = target_ms;

    int stream_index = player->video_stream >= 0 ? player->video_stream : player->audio_stream;
    AVStream *stream = player->format->streams[stream_index];
    int64_t target = av_rescale_q(target_ms, (AVRational){1, 1000}, stream->time_base);
    if (av_seek_frame(player->format, stream_index, target, AVSEEK_FLAG_BACKWARD) < 0) {
        return TURBO_PLAYER_ERR_STREAM;
    }

    if (player->audio_codec) {
        avcodec_flush_buffers(player->audio_codec);
    }
    if (player->video_codec) {
        avcodec_flush_buffers(player->video_codec);
    }
    if (player->resampler) {
        swr_close(player->resampler);
        if (swr_init(player->resampler) < 0) {
            return TURBO_PLAYER_ERR_CODEC;
        }
    }
    if (player->playback) {
        if (salts_playback_clear(player->playback) != SALTS_PLAYBACK_OK) {
            return TURBO_PLAYER_ERR_PLAYBACK;
        }
    }

    return TURBO_PLAYER_OK;
}

static AVCodecContext *open_decoder(AVFormatContext *format, int stream_index) {
    if (!format || stream_index < 0) {
        return NULL;
    }

    AVStream *stream = format->streams[stream_index];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        return NULL;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        return NULL;
    }

    if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0 ||
        avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }

    return ctx;
}

static int init_audio(turbo_player_t *player) {
    if (!player || !player->audio_codec) {
        return TURBO_PLAYER_OK;
    }

    player->audio_rate = player->config.audio_sample_rate > 0
                             ? player->config.audio_sample_rate
                             : player->audio_codec->sample_rate;
    if (player->audio_rate != 8000 && player->audio_rate != 16000 &&
        player->audio_rate != 24000 && player->audio_rate != 48000) {
        if (player->config.audio_sample_rate > 0) {
            return TURBO_PLAYER_ERR_PLAYBACK;
        }
        player->audio_rate = 48000;
    }

    player->audio_channels = player->config.audio_channels > 0
                                 ? player->config.audio_channels
                                 : player->audio_codec->ch_layout.nb_channels;
    if (player->audio_channels != 1 && player->audio_channels != 2) {
        if (player->config.audio_channels > 0) {
            return TURBO_PLAYER_ERR_PLAYBACK;
        }
        player->audio_channels = 2;
    }

    AVChannelLayout in_layout;
    AVChannelLayout out_layout;
    memset(&in_layout, 0, sizeof(in_layout));
    memset(&out_layout, 0, sizeof(out_layout));

    if (player->audio_codec->ch_layout.nb_channels > 0) {
        if (av_channel_layout_copy(&in_layout, &player->audio_codec->ch_layout) < 0) {
            return TURBO_PLAYER_ERR_CODEC;
        }
    } else {
        av_channel_layout_default(&in_layout, player->audio_channels);
    }
    av_channel_layout_default(&out_layout, player->audio_channels);

    int rc = swr_alloc_set_opts2(&player->resampler,
                                 &out_layout,
                                 AV_SAMPLE_FMT_FLT,
                                 player->audio_rate,
                                 &in_layout,
                                 player->audio_codec->sample_fmt,
                                 player->audio_codec->sample_rate,
                                 0,
                                 NULL);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    if (rc < 0 || swr_init(player->resampler) < 0) {
        return TURBO_PLAYER_ERR_CODEC;
    }

    if (player->config.play_audio) {
        salts_playback_config_t playback_config;
        memset(&playback_config, 0, sizeof(playback_config));
        playback_config.sample_rate = (uint32_t)player->audio_rate;
        playback_config.channels = (uint32_t)player->audio_channels;
        playback_config.format = SALTS_PLAYBACK_FORMAT_F32;
        playback_config.buffer_duration_ms = (uint32_t)(
            player->config.audio_buffer_ms > 0
                ? player->config.audio_buffer_ms
                : PLAYER_DEFAULT_AUDIO_BUFFER_MS);

        if (salts_playback_create(player->config.audio_device,
                                  &playback_config,
                                  &player->playback) != SALTS_PLAYBACK_OK) {
            return TURBO_PLAYER_ERR_PLAYBACK;
        }
    }

    return TURBO_PLAYER_OK;
}

turbo_player_t *turbo_player_open(const char *url, const turbo_player_config_t *config) {
    if (!url) {
        return NULL;
    }

    turbo_player_t *player = (turbo_player_t *)calloc(1, sizeof(turbo_player_t));
    if (!player) {
        return NULL;
    }

    player->audio_stream = -1;
    player->video_stream = -1;
    player->discard_until_ms = -1;
    player->config.play_audio = 1;
    player->config.video_format = TURBO_PLAYER_VIDEO_RGBA;
    if (config) {
        player->config = *config;
    }

    player->packet = av_packet_alloc();
    player->frame = av_frame_alloc();
    if (!player->packet || !player->frame) {
        turbo_player_close(player);
        return NULL;
    }

    if (avformat_open_input(&player->format, url, NULL, NULL) < 0 ||
        avformat_find_stream_info(player->format, NULL) < 0) {
        turbo_player_close(player);
        return NULL;
    }

    player->audio_stream = av_find_best_stream(player->format, AVMEDIA_TYPE_AUDIO,
                                               -1, -1, NULL, 0);
    player->video_stream = av_find_best_stream(player->format, AVMEDIA_TYPE_VIDEO,
                                               -1, -1, NULL, 0);

    if (player->audio_stream < 0 && player->video_stream < 0) {
        turbo_player_close(player);
        return NULL;
    }

    if (player->audio_stream >= 0) {
        player->audio_codec = open_decoder(player->format, player->audio_stream);
        if (!player->audio_codec || init_audio(player) != TURBO_PLAYER_OK) {
            turbo_player_close(player);
            return NULL;
        }
    }

    if (player->video_stream >= 0) {
        player->video_codec = open_decoder(player->format, player->video_stream);
        if (!player->video_codec) {
            turbo_player_close(player);
            return NULL;
        }
    }

    return player;
}

void turbo_player_close(turbo_player_t *player) {
    if (!player) {
        return;
    }

    turbo_player_stop(player);
    turbo_player_wait(player);

    if (player->playback) {
        salts_playback_destroy(player->playback);
    }
    swr_free(&player->resampler);
    sws_freeContext(player->scaler);
    avcodec_free_context(&player->audio_codec);
    avcodec_free_context(&player->video_codec);
    avformat_close_input(&player->format);
    av_packet_free(&player->packet);
    av_frame_free(&player->frame);
    av_free(player->audio_buf);
    av_free(player->video_buf);
    free(player);
}

void turbo_player_set_video_callback(turbo_player_t *player,
                                     turbo_player_video_cb cb,
                                     void *user_data) {
    if (!player) {
        return;
    }
    player->video_cb = cb;
    player->video_user_data = user_data;
}

void turbo_player_set_audio_callback(turbo_player_t *player,
                                     turbo_player_audio_cb cb,
                                     void *user_data) {
    if (!player) {
        return;
    }
    player->audio_cb = cb;
    player->audio_user_data = user_data;
}

void turbo_player_on_complete(turbo_player_t *player,
                              turbo_player_complete_cb cb,
                              void *user_data) {
    if (!player) {
        return;
    }
    player->complete_cb = cb;
    player->complete_user_data = user_data;
}

static int write_audio_to_playback(turbo_player_t *player,
                                   const uint8_t *data,
                                   size_t len) {
    size_t written = 0;
    while (!player->stopped && written < len) {
        if (wait_if_paused(player) != TURBO_PLAYER_OK) {
            break;
        }
        size_t available = salts_playback_get_available(player->playback);
        if (available == 0) {
            if (salts_playback_get_state(player->playback) ==
                SALTS_PLAYBACK_STATE_ERROR) {
                return TURBO_PLAYER_ERR_PLAYBACK;
            }
            player_sleep_ms(5);
            continue;
        }

        size_t chunk = len - written;
        if (chunk > available) {
            chunk = available;
        }
        size_t chunk_written = 0;
        if (salts_playback_write(player->playback,
                                 data + written,
                                 chunk,
                                 &chunk_written) != SALTS_PLAYBACK_OK ||
            chunk_written == 0) {
            return TURBO_PLAYER_ERR_PLAYBACK;
        }
        written += chunk_written;
    }
    return written == len ? TURBO_PLAYER_OK : TURBO_PLAYER_ERR_PLAYBACK;
}

static int drain_playback(turbo_player_t *player, uint32_t timeout_ms) {
    uint32_t elapsed_ms = 0;
    while (salts_playback_get_buffered(player->playback) > 0) {
        if (player->stopped) {
            return TURBO_PLAYER_OK;
        }
        uint32_t remaining_ms = timeout_ms - elapsed_ms;
        uint32_t slice_ms = remaining_ms < PLAYER_DRAIN_SLICE_MS
                                ? remaining_ms
                                : PLAYER_DRAIN_SLICE_MS;
        if (slice_ms == 0) {
            return TURBO_PLAYER_ERR_PLAYBACK;
        }
        int result = salts_playback_drain(player->playback, slice_ms);
        if (result == SALTS_PLAYBACK_OK) {
            return TURBO_PLAYER_OK;
        }
        if (result != SALTS_PLAYBACK_ERR_TIMEOUT) {
            return TURBO_PLAYER_ERR_PLAYBACK;
        }
        elapsed_ms += slice_ms;
    }
    return TURBO_PLAYER_OK;
}

static int handle_audio_frame(turbo_player_t *player, const AVFrame *frame) {
    int dst_samples = (int)av_rescale_rnd(
        swr_get_delay(player->resampler, player->audio_codec->sample_rate) +
            frame->nb_samples,
        player->audio_rate,
        player->audio_codec->sample_rate,
        AV_ROUND_UP);
    if (dst_samples <= 0) {
        return TURBO_PLAYER_OK;
    }

    size_t needed = (size_t)dst_samples * (size_t)player->audio_channels * sizeof(float);
    if (player->audio_buf_cap < needed) {
        uint8_t *buf = (uint8_t *)av_realloc(player->audio_buf, needed);
        if (!buf) {
            return TURBO_PLAYER_ERR_NOMEM;
        }
        player->audio_buf = buf;
        player->audio_buf_cap = needed;
    }

    uint8_t *out_planes[1] = {player->audio_buf};
    int converted = swr_convert(player->resampler,
                                out_planes,
                                dst_samples,
                                (const uint8_t **)frame->extended_data,
                                frame->nb_samples);
    if (converted < 0) {
        return TURBO_PLAYER_ERR_CODEC;
    }

    size_t bytes = (size_t)converted * (size_t)player->audio_channels * sizeof(float);
    if (bytes == 0) {
        return TURBO_PLAYER_OK;
    }

    int64_t pts_ms = frame_pts_ms(frame, player->format->streams[player->audio_stream]);
    if (player->discard_until_ms >= 0 && pts_ms >= 0 && pts_ms < player->discard_until_ms) {
        return TURBO_PLAYER_OK;
    }
    if (player->audio_cb) {
        player->audio_cb(player,
                         (const float *)player->audio_buf,
                         (size_t)converted,
                         player->audio_rate,
                         player->audio_channels,
                         pts_ms,
                         player->audio_user_data);
    }
    if (player->playback) {
        return write_audio_to_playback(player, player->audio_buf, bytes);
    }
    return TURBO_PLAYER_OK;
}

static int ensure_video_buffer(turbo_player_t *player, int width, int height,
                               int *stride, size_t *len) {
    if (player->config.video_format == TURBO_PLAYER_VIDEO_I420) {
        int chroma_width = (width + 1) / 2;
        int chroma_height = (height + 1) / 2;
        stride[0] = width;
        stride[1] = chroma_width;
        stride[2] = chroma_width;
        stride[3] = 0;
        *len = (size_t)width * (size_t)height +
               2u * (size_t)chroma_width * (size_t)chroma_height;
    } else {
        stride[0] = width * 4;
        stride[1] = 0;
        stride[2] = 0;
        stride[3] = 0;
        *len = (size_t)stride[0] * (size_t)height;
    }

    if (player->video_buf_cap < *len) {
        uint8_t *buf = (uint8_t *)av_realloc(player->video_buf, *len);
        if (!buf) {
            return TURBO_PLAYER_ERR_NOMEM;
        }
        player->video_buf = buf;
        player->video_buf_cap = *len;
    }
    return TURBO_PLAYER_OK;
}

static int handle_video_frame(turbo_player_t *player, const AVFrame *frame) {
    if (!player->video_cb) {
        return TURBO_PLAYER_OK;
    }

    int width = frame->width;
    int height = frame->height;
    enum AVPixelFormat dst_format = player->config.video_format == TURBO_PLAYER_VIDEO_I420
                                        ? AV_PIX_FMT_YUV420P
                                        : AV_PIX_FMT_RGBA;
    int stride[4];
    size_t len;
    int rc = ensure_video_buffer(player, width, height, stride, &len);
    if (rc != TURBO_PLAYER_OK) {
        return rc;
    }

    uint8_t *dst_data[4] = {0};
    dst_data[0] = player->video_buf;
    if (player->config.video_format == TURBO_PLAYER_VIDEO_I420) {
        dst_data[1] = dst_data[0] + (size_t)stride[0] * (size_t)height;
        dst_data[2] = dst_data[1] + (size_t)stride[1] * (size_t)((height + 1) / 2);
    }

    player->scaler = sws_getCachedContext(player->scaler,
                                          width,
                                          height,
                                          (enum AVPixelFormat)frame->format,
                                          width,
                                          height,
                                          dst_format,
                                          SWS_BILINEAR,
                                          NULL,
                                          NULL,
                                          NULL);
    if (!player->scaler) {
        return TURBO_PLAYER_ERR_CODEC;
    }

    sws_scale(player->scaler,
              (const uint8_t * const *)frame->data,
              frame->linesize,
              0,
              height,
              dst_data,
              stride);

    turbo_player_video_frame_t out;
    memset(&out, 0, sizeof(out));
    out.data = player->video_buf;
    out.len = len;
    out.width = width;
    out.height = height;
    out.stride[0] = stride[0];
    out.stride[1] = stride[1];
    out.stride[2] = stride[2];
    out.stride[3] = stride[3];
    out.pts_ms = frame_pts_ms(frame, player->format->streams[player->video_stream]);
    if (player->discard_until_ms >= 0 && out.pts_ms >= 0 &&
        out.pts_ms < player->discard_until_ms) {
        return TURBO_PLAYER_OK;
    }
    out.format = player->config.video_format;
    player->video_cb(player, &out, player->video_user_data);
    return TURBO_PLAYER_OK;
}

static int receive_frames(turbo_player_t *player, AVCodecContext *codec, int stream_index) {
    for (;;) {
        int ret = avcodec_receive_frame(codec, player->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return TURBO_PLAYER_OK;
        }
        if (ret < 0) {
            return TURBO_PLAYER_ERR_CODEC;
        }

        int rc = stream_index == player->audio_stream
                     ? handle_audio_frame(player, player->frame)
                     : handle_video_frame(player, player->frame);
        av_frame_unref(player->frame);
        if (rc != TURBO_PLAYER_OK || player->stopped) {
            return rc;
        }
    }
}

static int send_packet(turbo_player_t *player, AVCodecContext *codec,
                       int stream_index, const AVPacket *packet) {
    int ret = avcodec_send_packet(codec, packet);
    if (ret < 0 && ret != AVERROR_EOF) {
        return TURBO_PLAYER_ERR_CODEC;
    }
    return receive_frames(player, codec, stream_index);
}

static int player_decode_to_end(turbo_player_t *player) {
    if (!player) {
        return TURBO_PLAYER_ERR_OPEN;
    }

    if (player->playback &&
        salts_playback_start(player->playback) != SALTS_PLAYBACK_OK) {
        return TURBO_PLAYER_ERR_PLAYBACK;
    }

    int rc = TURBO_PLAYER_OK;
    while (!player->stopped && av_read_frame(player->format, player->packet) >= 0) {
        rc = wait_if_paused(player);
        if (rc != TURBO_PLAYER_OK) {
            break;
        }

        rc = apply_pending_seek(player);
        if (rc != TURBO_PLAYER_OK) {
            av_packet_unref(player->packet);
            break;
        }

        if (player->packet->stream_index == player->audio_stream) {
            rc = send_packet(player, player->audio_codec, player->audio_stream, player->packet);
        } else if (player->packet->stream_index == player->video_stream) {
            rc = send_packet(player, player->video_codec, player->video_stream, player->packet);
        }
        av_packet_unref(player->packet);
        if (rc != TURBO_PLAYER_OK) {
            break;
        }
    }

    if (rc == TURBO_PLAYER_OK && !player->stopped) {
        if (player->audio_codec) {
            rc = send_packet(player, player->audio_codec, player->audio_stream, NULL);
        }
        if (rc == TURBO_PLAYER_OK && player->video_codec) {
            rc = send_packet(player, player->video_codec, player->video_stream, NULL);
        }
    }

    if (atomic_load_explicit(&player->terminal, memory_order_acquire) ==
        PLAYER_TERMINAL_CANCELLED) {
        rc = TURBO_PLAYER_OK;
    }

    if (player->playback) {
        if (rc == TURBO_PLAYER_OK &&
            atomic_load_explicit(&player->terminal, memory_order_acquire) ==
                PLAYER_TERMINAL_ACTIVE) {
            uint32_t drain_timeout_ms = (uint32_t)(
                (player->config.audio_buffer_ms > 0
                     ? player->config.audio_buffer_ms
                     : PLAYER_DEFAULT_AUDIO_BUFFER_MS) +
                PLAYER_DRAIN_MARGIN_MS);
            if (drain_playback(player, drain_timeout_ms) != TURBO_PLAYER_OK) {
                rc = TURBO_PLAYER_ERR_PLAYBACK;
            }
        }
        if (atomic_load_explicit(&player->terminal, memory_order_acquire) ==
            PLAYER_TERMINAL_CANCELLED) {
            rc = TURBO_PLAYER_OK;
        }
        if (salts_playback_stop(player->playback) != SALTS_PLAYBACK_OK &&
            rc == TURBO_PLAYER_OK) {
            rc = TURBO_PLAYER_ERR_PLAYBACK;
        }
    }

    if (rc == TURBO_PLAYER_OK) {
        int expected = PLAYER_TERMINAL_ACTIVE;
        if (atomic_compare_exchange_strong_explicit(&player->terminal,
                                                    &expected,
                                                    PLAYER_TERMINAL_COMPLETED,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire) &&
            player->complete_cb) {
            player->complete_cb(player, player->complete_user_data);
        }
    }
    return rc;
}

#ifdef _WIN32
static DWORD WINAPI player_thread_proc(LPVOID arg) {
    turbo_player_t *player = (turbo_player_t *)arg;
    player->last_result = player_decode_to_end(player);
    player->running = 0;
    return 0;
}
#else
static void *player_thread_proc(void *arg) {
    turbo_player_t *player = (turbo_player_t *)arg;
    player->last_result = player_decode_to_end(player);
    player->running = 0;
    return NULL;
}
#endif

int turbo_player_play_to_end(turbo_player_t *player) {
    if (!player || player->running) {
        return TURBO_PLAYER_ERR_OPEN;
    }
    player->stopped = 0;
    player->paused = 0;
    atomic_store_explicit(&player->terminal,
                          PLAYER_TERMINAL_ACTIVE,
                          memory_order_release);
    return player_decode_to_end(player);
}

int turbo_player_start(turbo_player_t *player) {
    if (!player || player->running) {
        return TURBO_PLAYER_ERR_OPEN;
    }

    player->stopped = 0;
    player->paused = 0;
    atomic_store_explicit(&player->terminal,
                          PLAYER_TERMINAL_ACTIVE,
                          memory_order_release);
    player->last_result = TURBO_PLAYER_OK;
    player->running = 1;

#ifdef _WIN32
    player->thread = CreateThread(NULL, 0, player_thread_proc, player, 0, NULL);
    if (!player->thread) {
        player->running = 0;
        return TURBO_PLAYER_ERR_PLAYBACK;
    }
#else
    if (pthread_create(&player->thread, NULL, player_thread_proc, player) != 0) {
        player->running = 0;
        return TURBO_PLAYER_ERR_PLAYBACK;
    }
#endif

    player->thread_started = 1;
    return TURBO_PLAYER_OK;
}

int turbo_player_wait(turbo_player_t *player) {
    if (!player || !player->thread_started) {
        return player ? player->last_result : TURBO_PLAYER_ERR_OPEN;
    }

#ifdef _WIN32
    WaitForSingleObject(player->thread, INFINITE);
    CloseHandle(player->thread);
    player->thread = NULL;
#else
    pthread_join(player->thread, NULL);
#endif
    player->thread_started = 0;
    player->running = 0;
    return player->last_result;
}

int turbo_player_is_running(turbo_player_t *player) {
    return (player && player->running) ? 1 : 0;
}

void turbo_player_stop(turbo_player_t *player) {
    if (!player) {
        return;
    }
    int expected = PLAYER_TERMINAL_ACTIVE;
    atomic_compare_exchange_strong_explicit(&player->terminal,
                                            &expected,
                                            PLAYER_TERMINAL_CANCELLED,
                                            memory_order_acq_rel,
                                            memory_order_acquire);
    player->stopped = 1;
    player->paused = 0;
}

void turbo_player_pause(turbo_player_t *player) {
    if (!player) {
        return;
    }
    player->paused = 1;
}

void turbo_player_resume(turbo_player_t *player) {
    if (!player) {
        return;
    }
    player->paused = 0;
}

int turbo_player_seek_ms(turbo_player_t *player, int64_t position_ms) {
    if (!player || position_ms < 0) {
        return TURBO_PLAYER_ERR_OPEN;
    }

    atomic_fetch_add_explicit(&player->seek_sequence, 1, memory_order_acq_rel);
    atomic_store_explicit(&player->seek_target_ms, position_ms, memory_order_relaxed);
    atomic_fetch_add_explicit(&player->seek_sequence, 1, memory_order_release);
    if (!player->running) {
        return apply_pending_seek(player);
    }
    return TURBO_PLAYER_OK;
}

int64_t turbo_player_get_duration_ms(turbo_player_t *player) {
    if (!player || !player->format || player->format->duration == AV_NOPTS_VALUE) {
        return 0;
    }
    return av_rescale_q(player->format->duration, AV_TIME_BASE_Q, (AVRational){1, 1000});
}
