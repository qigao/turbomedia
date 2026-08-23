/**
 * HTTP-FLV push streamer using one chunked TurboHTTP request.
 */
#include "turbo_streamer.h"

#ifdef TURBO_MEDIA_HAS_HTTP_FLV

#include "CoroNet/turbo_coro_context.h"
#include "flv_muxer_internal.h"
#include "flv_writer.h"
#include "http_client.h"
#include "turbo_coro.h"
#include <turbostl/deque.h>
#include "turbo_str.h"
#include "turbo_vstr.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum {
    HTTP_FLV_CODEC_NONE,
    HTTP_FLV_CODEC_H264,
    HTTP_FLV_CODEC_H265,
    HTTP_FLV_CODEC_H266,
    HTTP_FLV_CODEC_AAC,
    HTTP_FLV_CODEC_MP3,
    HTTP_FLV_CODEC_G711A,
    HTTP_FLV_CODEC_G711U
} http_flv_codec_t;

typedef struct {
    uint8_t *data;
    size_t size;
    size_t offset;
} http_flv_chunk_t;

typedef struct {
    tstr url;
    http_client_t *http_client;
    coro_context_t *coro_context;
    coro_task_t *upload_task;
    flv_muxer_t *muxer;
    void *writer;
    deque_t queue;
    size_t queue_bytes;
    size_t queue_capacity;
    http_flv_codec_t video_codec;
    http_flv_codec_t audio_codec;
    int video_stream_id;
    int audio_stream_id;
    int connected;
    int upload_closed;
    int upload_done;
    int upload_result;
    turbo_streamer_event_cb event_callback;
    void *event_user_data;
    turbo_streamer_stats_t stats;
} http_flv_streamer_ctx_t;

static void http_flv_clear_queue(http_flv_streamer_ctx_t *ctx) {
    http_flv_chunk_t chunk;
    while (deque_pop_front(&ctx->queue, &chunk) == STL_OK) free(chunk.data);
    ctx->queue_bytes = 0;
}

static int http_flv_enqueue_vectors(void *param, const struct flv_vec_t *vectors,
                                    int count) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)param;
    http_flv_chunk_t chunk = {0};
    size_t total = 0;
    size_t offset = 0;
    int index;

    if (!ctx || !vectors || count <= 0 || !ctx->connected) return -EINVAL;
    for (index = 0; index < count; ++index) {
        if (vectors[index].len < 0 ||
            (size_t)vectors[index].len > SIZE_MAX - total)
            return -EOVERFLOW;
        total += (size_t)vectors[index].len;
    }
    if (total == 0 || total > ctx->queue_capacity) return -EOVERFLOW;

    while (ctx->queue_bytes > ctx->queue_capacity - total) {
        if (ctx->upload_done || ctx->upload_result != 0) return -EPIPE;
        if (coro_yield() != 0) return -EIO;
    }

    chunk.data = (uint8_t *)malloc(total);
    if (!chunk.data) return -ENOMEM;
    chunk.size = total;
    for (index = 0; index < count; ++index) {
        memcpy(chunk.data + offset, vectors[index].ptr, (size_t)vectors[index].len);
        offset += (size_t)vectors[index].len;
    }
    if (deque_push_back(&ctx->queue, &chunk) != STL_OK) {
        free(chunk.data);
        return -ENOMEM;
    }
    ctx->queue_bytes += total;
    return 0;
}

static int http_flv_on_tag(void *param, int type, const void *data, size_t bytes,
                           uint32_t timestamp) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)param;
    if (!ctx || !ctx->writer) return -EINVAL;
    return flv_writer_input(ctx->writer, type, data, bytes, timestamp);
}

static size_t http_flv_read_body(char *buffer, size_t size, void *user_data) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)user_data;
    http_flv_chunk_t *front;
    size_t available;
    size_t copied;

    if (!ctx || !buffer || size == 0) return (size_t)-1;
    while (deque_empty(&ctx->queue)) {
        if (ctx->upload_closed) return 0;
        if (coro_yield() != 0) return (size_t)-1;
    }

    front = (http_flv_chunk_t *)deque_front(&ctx->queue);
    if (!front || front->offset > front->size) return (size_t)-1;
    available = front->size - front->offset;
    copied = available < size ? available : size;
    memcpy(buffer, front->data + front->offset, copied);
    front->offset += copied;
    ctx->queue_bytes -= copied;
    if (front->offset == front->size) {
        http_flv_chunk_t completed;
        if (deque_pop_front(&ctx->queue, &completed) != STL_OK)
            return (size_t)-1;
        free(completed.data);
    }
    return copied;
}

static void http_flv_upload_task(coro_t *co, void *arg) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)arg;
    http_response_t *response;

    (void)co;
    response = http_post_stream(ctx->http_client, ctx->url, http_flv_read_body, 0, ctx);
    if (!response) {
        ctx->upload_result = -EIO;
    } else {
        ctx->upload_result =
            response->error_code == HTTP_ERROR_NONE && response->status_code >= 200 &&
                    response->status_code < 300
                ? 0
                : -EIO;
        http_response_free(response);
    }
    ctx->upload_done = 1;
}

static int http_flv_codec(const char *name, int video, http_flv_codec_t *codec) {
    vstr value;

    if (!name || !codec) return -EINVAL;
    value = vstr_from_cstr(name);
    if (video) {
        if (vstr_ieq(value, vstr_from_cstr("h264")) ||
            vstr_ieq(value, vstr_from_cstr("avc")))
            *codec = HTTP_FLV_CODEC_H264;
        else if (vstr_ieq(value, vstr_from_cstr("h265")) ||
                 vstr_ieq(value, vstr_from_cstr("hevc")))
            *codec = HTTP_FLV_CODEC_H265;
        else if (vstr_ieq(value, vstr_from_cstr("h266")) ||
                 vstr_ieq(value, vstr_from_cstr("vvc")))
            *codec = HTTP_FLV_CODEC_H266;
        else
            return -ENOTSUP;
    } else if (vstr_ieq(value, vstr_from_cstr("aac")))
        *codec = HTTP_FLV_CODEC_AAC;
    else if (vstr_ieq(value, vstr_from_cstr("mp3")))
        *codec = HTTP_FLV_CODEC_MP3;
    else if (vstr_ieq(value, vstr_from_cstr("pcma")) ||
             vstr_ieq(value, vstr_from_cstr("g711a")))
        *codec = HTTP_FLV_CODEC_G711A;
    else if (vstr_ieq(value, vstr_from_cstr("pcmu")) ||
             vstr_ieq(value, vstr_from_cstr("g711u")))
        *codec = HTTP_FLV_CODEC_G711U;
    else
        return -ENOTSUP;
    return 0;
}

static void http_flv_streamer_destroy_impl(void *ctx_ptr) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    if (ctx->writer) flv_writer_destroy(ctx->writer);
    if (ctx->muxer) flv_muxer_destroy(ctx->muxer);
    if (ctx->upload_task) coro_task_destroy(ctx->upload_task);
    http_flv_clear_queue(ctx);
    deque_destroy(&ctx->queue);
    tstr_free(ctx->url);
    free(ctx);
}

static void *http_flv_streamer_create(const turbo_streamer_config_t *config) {
    http_flv_streamer_ctx_t *ctx;

    if (!config || config->protocol != TURBO_STREAMER_HTTP_FLV || !config->url ||
        !config->http_client || !config->coro_context || config->buffer_size <= 0)
        return NULL;
    ctx = (http_flv_streamer_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->url = tstr_dup(config->url);
    ctx->http_client = (http_client_t *)config->http_client;
    ctx->coro_context = (coro_context_t *)config->coro_context;
    ctx->queue_capacity = (size_t)config->buffer_size;
    ctx->video_stream_id = -1;
    ctx->audio_stream_id = -1;
    ctx->stats.uptime_ms = (int64_t)time(NULL) * 1000;
    if (!ctx->url || deque_init_bytes(
                         &ctx->queue, sizeof(http_flv_chunk_t),
                         CMETA_ALIGNOF(http_flv_chunk_t), ctx->queue_capacity) != STL_OK) {
        http_flv_streamer_destroy_impl(ctx);
        return NULL;
    }
    ctx->muxer = flv_muxer_create(http_flv_on_tag, ctx);
    if (!ctx->muxer) {
        http_flv_streamer_destroy_impl(ctx);
        return NULL;
    }
    return ctx;
}

static int http_flv_streamer_add_stream_impl(
    void *ctx_ptr, const turbo_stream_info_t *stream_info, int *stream_id) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    http_flv_codec_t codec;
    int result;

    if (!ctx || !stream_info || !stream_id || ctx->connected) return -EINVAL;
    if (stream_info->type == TURBO_CODEC_TYPE_VIDEO) {
        if (ctx->video_stream_id >= 0) return -EINVAL;
        result = http_flv_codec(stream_info->codec_name, 1, &codec);
        if (result != 0) return result;
        ctx->video_codec = codec;
        ctx->video_stream_id = 0;
        *stream_id = 0;
        return 0;
    }
    if (stream_info->type == TURBO_CODEC_TYPE_AUDIO) {
        if (ctx->audio_stream_id >= 0) return -EINVAL;
        result = http_flv_codec(stream_info->codec_name, 0, &codec);
        if (result != 0) return result;
        ctx->audio_codec = codec;
        ctx->audio_stream_id = 1;
        *stream_id = 1;
        return 0;
    }
    return -ENOTSUP;
}

static int http_flv_streamer_connect_impl(void *ctx_ptr) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    int result;

    if (!ctx || (ctx->video_stream_id < 0 && ctx->audio_stream_id < 0) ||
        coro_context_current() != ctx->coro_context)
        return -EINVAL;
    if (ctx->connected) return 0;

    ctx->upload_closed = 0;
    ctx->upload_done = 0;
    ctx->upload_result = 0;
    http_flv_clear_queue(ctx);
    if (flv_muxer_reset(ctx->muxer) != 0) return -EIO;
    ctx->connected = 1;
    ctx->writer = flv_writer_create2(ctx->audio_stream_id >= 0,
                                     ctx->video_stream_id >= 0,
                                     http_flv_enqueue_vectors, ctx);
    if (!ctx->writer) {
        ctx->connected = 0;
        return -EIO;
    }
    ctx->upload_task =
        coro_task_create(ctx->coro_context, http_flv_upload_task, ctx);
    if (!ctx->upload_task) {
        flv_writer_destroy(ctx->writer);
        ctx->writer = NULL;
        ctx->connected = 0;
        http_flv_clear_queue(ctx);
        return -ENOMEM;
    }
    result = coro_task_start(ctx->upload_task);
    if (result != 0) {
        coro_task_destroy(ctx->upload_task);
        ctx->upload_task = NULL;
        flv_writer_destroy(ctx->writer);
        ctx->writer = NULL;
        ctx->connected = 0;
        http_flv_clear_queue(ctx);
        return result;
    }
    if (ctx->event_callback)
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_CONNECTED, NULL,
                            ctx->event_user_data);
    return 0;
}

static int http_flv_streamer_disconnect_impl(void *ctx_ptr) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    int result;

    if (!ctx || coro_context_current() != ctx->coro_context) return -EINVAL;
    if (!ctx->connected) return 0;
    flv_writer_destroy(ctx->writer);
    ctx->writer = NULL;
    ctx->upload_closed = 1;
    result = coro_when_all(ctx->coro_context, &ctx->upload_task, 1);
    if (result == 0) result = ctx->upload_result;
    coro_task_destroy(ctx->upload_task);
    ctx->upload_task = NULL;
    ctx->connected = 0;
    if (result != 0) return result;
    if (ctx->event_callback)
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_DISCONNECTED, NULL,
                            ctx->event_user_data);
    return 0;
}

static int http_flv_streamer_write_packet_impl(
    void *ctx_ptr, const turbo_muxer_packet_t *packet) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    uint64_t pts_ms;
    uint64_t dts_ms;
    int result;

    if (!ctx || !packet || !ctx->connected || !packet->data || packet->size == 0 ||
        packet->pts < 0 || packet->dts < 0 ||
        coro_context_current() != ctx->coro_context)
        return -EINVAL;
    if (ctx->upload_done || ctx->upload_result != 0) return -EPIPE;
    pts_ms = (uint64_t)packet->pts / 1000;
    dts_ms = (uint64_t)packet->dts / 1000;
    if (pts_ms > UINT32_MAX || dts_ms > UINT32_MAX) return -EOVERFLOW;

    if (packet->stream_id == ctx->video_stream_id) {
        if (ctx->video_codec == HTTP_FLV_CODEC_H264)
            result = flv_muxer_avc(ctx->muxer, packet->data, packet->size,
                                   (uint32_t)pts_ms, (uint32_t)dts_ms);
        else if (ctx->video_codec == HTTP_FLV_CODEC_H265)
            result = flv_muxer_hevc(ctx->muxer, packet->data, packet->size,
                                    (uint32_t)pts_ms, (uint32_t)dts_ms);
        else if (ctx->video_codec == HTTP_FLV_CODEC_H266)
            result = flv_muxer_vvc(ctx->muxer, packet->data, packet->size,
                                   (uint32_t)pts_ms, (uint32_t)dts_ms);
        else
            return -ENOTSUP;
    } else if (packet->stream_id == ctx->audio_stream_id) {
        if (ctx->audio_codec == HTTP_FLV_CODEC_AAC)
            result = flv_muxer_aac(ctx->muxer, packet->data, packet->size,
                                   (uint32_t)pts_ms, (uint32_t)dts_ms);
        else if (ctx->audio_codec == HTTP_FLV_CODEC_MP3)
            result = flv_muxer_mp3(ctx->muxer, packet->data, packet->size,
                                   (uint32_t)pts_ms, (uint32_t)dts_ms);
        else if (ctx->audio_codec == HTTP_FLV_CODEC_G711A)
            result = flv_muxer_g711a(ctx->muxer, packet->data, packet->size,
                                     (uint32_t)pts_ms, (uint32_t)dts_ms);
        else if (ctx->audio_codec == HTTP_FLV_CODEC_G711U)
            result = flv_muxer_g711u(ctx->muxer, packet->data, packet->size,
                                     (uint32_t)pts_ms, (uint32_t)dts_ms);
        else
            return -ENOTSUP;
    } else {
        return -EINVAL;
    }
    if (result != 0) return result;
    ctx->stats.bytes_sent += packet->size;
    ++ctx->stats.packets_sent;
    return 0;
}

static int http_flv_streamer_get_stats_impl(void *ctx_ptr,
                                            turbo_streamer_stats_t *stats) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    if (!ctx || !stats) return -EINVAL;
    *stats = ctx->stats;
    return 0;
}

static void http_flv_streamer_set_event_callback_impl(
    void *ctx_ptr, turbo_streamer_event_cb callback, void *user_data) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    ctx->event_callback = callback;
    ctx->event_user_data = user_data;
}

const turbo_streamer_ops_t turbo_http_flv_streamer_ops = {
    .name = "http-flv",
    .protocol = TURBO_STREAMER_HTTP_FLV,
    .create = http_flv_streamer_create,
    .destroy = http_flv_streamer_destroy_impl,
    .connect = http_flv_streamer_connect_impl,
    .disconnect = http_flv_streamer_disconnect_impl,
    .add_stream = http_flv_streamer_add_stream_impl,
    .write_packet = http_flv_streamer_write_packet_impl,
    .read_packet = NULL,
    .get_stats = http_flv_streamer_get_stats_impl,
    .set_event_callback = http_flv_streamer_set_event_callback_impl
};

#endif
