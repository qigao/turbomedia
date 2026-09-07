/** HTTP-FLV push streamer using one CNet-owned HTTP/1.1 chunked request. */
#include "turbo_streamer.h"

#ifdef TURBO_MEDIA_HAS_HTTP_FLV

#include "flv_muxer_internal.h"
#include "flv_writer.h"
#include "salts_str.h"
#include "salts_vstr.h"
#include "turbo_transport.h"

#include <cstl/deque.h>
#include <salts/thread.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    HTTP_FLV_HOST_CAPACITY = 256,
    HTTP_FLV_AUTHORITY_CAPACITY = 320,
    HTTP_FLV_TARGET_CAPACITY = 1024,
    HTTP_FLV_RESPONSE_CAPACITY = 32 * 1024,
    HTTP_FLV_SEND_SLICE_BYTES = 64 * 1024,
    HTTP_FLV_DEFAULT_TIMEOUT_MS = 30000
};

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

typedef struct { uint8_t *data; size_t size; } http_flv_chunk_t;

typedef struct {
    char host[HTTP_FLV_HOST_CAPACITY];
    char authority[HTTP_FLV_AUTHORITY_CAPACITY];
    char target[HTTP_FLV_TARGET_CAPACITY];
    int port;
    int use_tls;
} http_flv_url_t;

typedef struct {
    tstr url;
    cnet_client *network_client;
    const cnet_tls_client_config *network_tls;
    turbo_transport_t *transport;
    salts_thread_t upload_thread;
    salts_mutex_t mutex;
    salts_cond_t cond;
    int sync_initialized;
    int upload_thread_started;
    int upload_ready;
    flv_muxer_t *muxer;
    void *writer;
    deque_t queue;
    int queue_initialized;
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

static int http_flv_parse_url(const char *url, http_flv_url_t *parsed) {
    const char *cursor, *authority_end, *host_start, *host_end;
    const char *port_start = NULL;
    size_t authority_size, host_size;
    int port;
    if (!url || !parsed || strchr(url, '\r') || strchr(url, '\n')) return -EINVAL;
    memset(parsed, 0, sizeof(*parsed));
    if (strncmp(url, "http://", 7) == 0) { cursor = url + 7; port = 80; }
    else if (strncmp(url, "https://", 8) == 0) {
        cursor = url + 8; parsed->use_tls = 1; port = 443;
    } else return -EINVAL;
    authority_end = strchr(cursor, '/');
    if (!authority_end) authority_end = cursor + strlen(cursor);
    authority_size = (size_t)(authority_end - cursor);
    if (authority_size == 0 || authority_size >= sizeof(parsed->authority) ||
        memchr(cursor, '@', authority_size)) return -EINVAL;
    memcpy(parsed->authority, cursor, authority_size);
    parsed->authority[authority_size] = '\0';
    host_start = cursor;
    if (*host_start == '[') {
        host_start++;
        host_end = memchr(host_start, ']', (size_t)(authority_end - host_start));
        if (!host_end || (host_end + 1 < authority_end && host_end[1] != ':')) return -EINVAL;
        if (host_end + 1 < authority_end) port_start = host_end + 2;
    } else {
        const char *colon = memchr(host_start, ':', authority_size);
        host_end = colon ? colon : authority_end;
        if (colon) port_start = colon + 1;
    }
    host_size = (size_t)(host_end - host_start);
    if (host_size == 0 || host_size >= sizeof(parsed->host)) return -EINVAL;
    memcpy(parsed->host, host_start, host_size);
    parsed->host[host_size] = '\0';
    if (port_start) {
        char *end = NULL;
        long value = strtol(port_start, &end, 10);
        if (end != authority_end || value <= 0 || value > 65535) return -EINVAL;
        port = (int)value;
    }
    if (*authority_end) {
        size_t target_size = strlen(authority_end);
        if (target_size >= sizeof(parsed->target)) return -ENAMETOOLONG;
        memcpy(parsed->target, authority_end, target_size + 1u);
    } else memcpy(parsed->target, "/", 2u);
    parsed->port = port;
    return 0;
}

static void http_flv_clear_queue(http_flv_streamer_ctx_t *ctx) {
    http_flv_chunk_t chunk;
    if (!ctx || !ctx->queue_initialized) return;
    while (deque_pop_front(&ctx->queue, &chunk) == STL_OK) free(chunk.data);
    ctx->queue_bytes = 0;
}

static int http_flv_enqueue_vectors(void *param, const struct flv_vec_t *vectors,
                                    int count) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)param;
    http_flv_chunk_t chunk = {0};
    size_t total = 0, offset = 0;
    if (!ctx || !vectors || count <= 0) return -EINVAL;
    for (int i = 0; i < count; ++i) {
        if (vectors[i].len < 0 || (size_t)vectors[i].len > SIZE_MAX - total)
            return -EOVERFLOW;
        total += (size_t)vectors[i].len;
    }
    if (total == 0 || total > ctx->queue_capacity) return -EOVERFLOW;
    chunk.data = (uint8_t *)malloc(total);
    if (!chunk.data) return -ENOMEM;
    chunk.size = total;
    for (int i = 0; i < count; ++i) {
        memcpy(chunk.data + offset, vectors[i].ptr, (size_t)vectors[i].len);
        offset += (size_t)vectors[i].len;
    }
    salts_mutex_lock(&ctx->mutex);
    while (ctx->connected && !ctx->upload_done &&
           ctx->queue_bytes > ctx->queue_capacity - total)
        salts_cond_wait(&ctx->cond, &ctx->mutex);
    if (!ctx->connected || ctx->upload_done || ctx->upload_result != 0 ||
        deque_push_back(&ctx->queue, &chunk) != STL_OK) {
        salts_mutex_unlock(&ctx->mutex);
        free(chunk.data);
        return -EPIPE;
    }
    ctx->queue_bytes += total;
    salts_cond_signal(&ctx->cond);
    salts_mutex_unlock(&ctx->mutex);
    return 0;
}

static int http_flv_on_tag(void *param, int type, const void *data, size_t bytes,
                           uint32_t timestamp) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)param;
    return ctx && ctx->writer
               ? flv_writer_input(ctx->writer, type, data, bytes, timestamp)
               : -EINVAL;
}

static int http_flv_send_all(http_flv_streamer_ctx_t *ctx,
                             const void *data, size_t size) {
    const uint8_t *cursor = (const uint8_t *)data;
    while (size > 0) {
        size_t part = size < HTTP_FLV_SEND_SLICE_BYTES ? size : HTTP_FLV_SEND_SLICE_BYTES;
        if (turbo_transport_send(ctx->transport, cursor, part) != (int)part) return -EIO;
        cursor += part;
        size -= part;
    }
    return 0;
}

static int http_flv_send_chunk(http_flv_streamer_ctx_t *ctx,
                               const http_flv_chunk_t *chunk) {
    char prefix[32];
    int prefix_size = snprintf(prefix, sizeof(prefix), "%zx\r\n", chunk->size);
    return prefix_size > 0 && (size_t)prefix_size < sizeof(prefix) &&
                   http_flv_send_all(ctx, prefix, (size_t)prefix_size) == 0 &&
                   http_flv_send_all(ctx, chunk->data, chunk->size) == 0 &&
                   http_flv_send_all(ctx, "\r\n", 2u) == 0
               ? 0 : -EIO;
}

static int http_flv_read_response(http_flv_streamer_ctx_t *ctx) {
    char response[HTTP_FLV_RESPONSE_CAPACITY + 1u];
    size_t used = 0;
    while (used < HTTP_FLV_RESPONSE_CAPACITY) {
        uint8_t *data = NULL;
        size_t size = 0;
        int received = turbo_transport_recv(ctx->transport, &data, &size);
        if (received <= 0 || !data || size == 0 || size > HTTP_FLV_RESPONSE_CAPACITY - used) {
            turbo_transport_free_recv(ctx->transport, data);
            return -EIO;
        }
        memcpy(response + used, data, size);
        used += size;
        response[used] = '\0';
        turbo_transport_free_recv(ctx->transport, data);
        if (strstr(response, "\r\n\r\n")) {
            unsigned int status = 0;
            return sscanf(response, "HTTP/%*u.%*u %u", &status) == 1 &&
                           status >= 200u && status < 300u ? 0 : -EIO;
        }
    }
    return -EOVERFLOW;
}

static void http_flv_upload_thread(void *arg) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)arg;
    http_flv_url_t url;
    turbo_transport_config_t config = {0};
    char request[HTTP_FLV_TARGET_CAPACITY + HTTP_FLV_AUTHORITY_CAPACITY + 256u];
    int result = http_flv_parse_url(ctx->url, &url);
    if (result == 0) {
        config.type = url.use_tls ? TURBO_TRANSPORT_TLS : TURBO_TRANSPORT_TCP;
        config.host = url.host;
        config.port = url.port;
        config.connect_timeout_ms = HTTP_FLV_DEFAULT_TIMEOUT_MS;
        config.read_timeout_ms = HTTP_FLV_DEFAULT_TIMEOUT_MS;
        config.write_timeout_ms = HTTP_FLV_DEFAULT_TIMEOUT_MS;
        config.cnet_client = ctx->network_client;
        config.tls = ctx->network_tls;
        ctx->transport = turbo_transport_create(&config);
        if (!ctx->transport || turbo_transport_connect(ctx->transport) != 0) result = -EIO;
    }
    if (result == 0) {
        int size = snprintf(request, sizeof(request),
            "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: video/x-flv\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
            url.target, url.authority);
        if (size <= 0 || (size_t)size >= sizeof(request) ||
            http_flv_send_all(ctx, request, (size_t)size) != 0) result = -EIO;
    }
    salts_mutex_lock(&ctx->mutex);
    ctx->upload_ready = result == 0;
    if (result != 0) { ctx->upload_result = result; ctx->upload_done = 1; }
    salts_cond_broadcast(&ctx->cond);
    salts_mutex_unlock(&ctx->mutex);
    while (result == 0) {
        http_flv_chunk_t chunk = {0};
        salts_mutex_lock(&ctx->mutex);
        while (deque_empty(&ctx->queue) && !ctx->upload_closed)
            salts_cond_wait(&ctx->cond, &ctx->mutex);
        if (!deque_empty(&ctx->queue)) {
            if (deque_pop_front(&ctx->queue, &chunk) != STL_OK) result = -EIO;
            else ctx->queue_bytes -= chunk.size;
            salts_cond_broadcast(&ctx->cond);
        } else {
            salts_mutex_unlock(&ctx->mutex);
            break;
        }
        salts_mutex_unlock(&ctx->mutex);
        if (result == 0) result = http_flv_send_chunk(ctx, &chunk);
        free(chunk.data);
    }
    if (result == 0) {
        result = http_flv_send_all(ctx, "0\r\n\r\n", 5u);
        if (result == 0) result = http_flv_read_response(ctx);
    }
    salts_mutex_lock(&ctx->mutex);
    ctx->upload_result = result;
    ctx->upload_done = 1;
    salts_cond_broadcast(&ctx->cond);
    salts_mutex_unlock(&ctx->mutex);
}

static int http_flv_codec(const char *name, int video, http_flv_codec_t *codec) {
    vstr value;
    if (!name || !codec) return -EINVAL;
    value = vstr_from_cstr(name);
    if (video) {
        if (vstr_ieq(value, vstr_from_cstr("h264")) || vstr_ieq(value, vstr_from_cstr("avc")))
            *codec = HTTP_FLV_CODEC_H264;
        else if (vstr_ieq(value, vstr_from_cstr("h265")) || vstr_ieq(value, vstr_from_cstr("hevc")))
            *codec = HTTP_FLV_CODEC_H265;
        else if (vstr_ieq(value, vstr_from_cstr("h266")) || vstr_ieq(value, vstr_from_cstr("vvc")))
            *codec = HTTP_FLV_CODEC_H266;
        else return -ENOTSUP;
    } else if (vstr_ieq(value, vstr_from_cstr("aac"))) *codec = HTTP_FLV_CODEC_AAC;
    else if (vstr_ieq(value, vstr_from_cstr("mp3"))) *codec = HTTP_FLV_CODEC_MP3;
    else if (vstr_ieq(value, vstr_from_cstr("pcma")) || vstr_ieq(value, vstr_from_cstr("g711a")))
        *codec = HTTP_FLV_CODEC_G711A;
    else if (vstr_ieq(value, vstr_from_cstr("pcmu")) || vstr_ieq(value, vstr_from_cstr("g711u")))
        *codec = HTTP_FLV_CODEC_G711U;
    else return -ENOTSUP;
    return 0;
}

static int http_flv_streamer_disconnect_impl(void *ctx_ptr);

static void http_flv_streamer_destroy_impl(void *ctx_ptr) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    if (ctx->connected) (void)http_flv_streamer_disconnect_impl(ctx);
    if (ctx->writer) flv_writer_destroy(ctx->writer);
    if (ctx->muxer) flv_muxer_destroy(ctx->muxer);
    if (ctx->upload_thread_started) {
        salts_mutex_lock(&ctx->mutex);
        ctx->upload_closed = 1;
        salts_cond_broadcast(&ctx->cond);
        salts_mutex_unlock(&ctx->mutex);
        (void)salts_thread_join(&ctx->upload_thread);
        salts_thread_destroy(&ctx->upload_thread);
    }
    if (ctx->transport) turbo_transport_destroy(ctx->transport);
    http_flv_clear_queue(ctx);
    if (ctx->queue_initialized) deque_destroy(&ctx->queue);
    if (ctx->sync_initialized) { salts_cond_destroy(&ctx->cond); salts_mutex_destroy(&ctx->mutex); }
    tstr_free(ctx->url);
    free(ctx);
}

static void *http_flv_streamer_create(const turbo_streamer_config_t *config) {
    http_flv_streamer_ctx_t *ctx;
    http_flv_url_t parsed;
    if (!config || config->protocol != TURBO_STREAMER_HTTP_FLV || !config->url ||
        config->buffer_size <= 0 || http_flv_parse_url(config->url, &parsed) != 0 ||
        (parsed.use_tls && !config->network_tls)) return NULL;
    ctx = (http_flv_streamer_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->url = tstr_dup(config->url);
    ctx->network_client = config->network_client;
    ctx->network_tls = config->network_tls;
    ctx->queue_capacity = (size_t)config->buffer_size;
    ctx->video_stream_id = ctx->audio_stream_id = -1;
    ctx->stats.uptime_ms = (int64_t)time(NULL) * 1000;
    salts_mutex_init(&ctx->mutex);
    salts_cond_init(&ctx->cond);
    ctx->sync_initialized = 1;
    if (!ctx->url || deque_init_bytes(&ctx->queue, sizeof(http_flv_chunk_t),
                                      CMETA_ALIGNOF(http_flv_chunk_t),
                                      ctx->queue_capacity) != STL_OK) {
        http_flv_streamer_destroy_impl(ctx);
        return NULL;
    }
    ctx->queue_initialized = 1;
    ctx->muxer = flv_muxer_create(http_flv_on_tag, ctx);
    if (!ctx->muxer) { http_flv_streamer_destroy_impl(ctx); return NULL; }
    return ctx;
}

static int http_flv_streamer_add_stream_impl(
    void *ctx_ptr, const turbo_stream_info_t *info, int *stream_id) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    http_flv_codec_t codec;
    int result;
    if (!ctx || !info || !stream_id || ctx->connected) return -EINVAL;
    if (info->type == TURBO_CODEC_TYPE_VIDEO) {
        if (ctx->video_stream_id >= 0) return -EINVAL;
        result = http_flv_codec(info->codec_name, 1, &codec);
        if (result != 0) return result;
        ctx->video_codec = codec;
        ctx->video_stream_id = *stream_id = 0;
        return 0;
    }
    if (info->type == TURBO_CODEC_TYPE_AUDIO) {
        if (ctx->audio_stream_id >= 0) return -EINVAL;
        result = http_flv_codec(info->codec_name, 0, &codec);
        if (result != 0) return result;
        ctx->audio_codec = codec;
        ctx->audio_stream_id = *stream_id = 1;
        return 0;
    }
    return -ENOTSUP;
}

static int http_flv_streamer_connect_impl(void *ctx_ptr) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    int result;
    if (!ctx || (ctx->video_stream_id < 0 && ctx->audio_stream_id < 0)) return -EINVAL;
    if (ctx->connected) return 0;
    ctx->upload_closed = ctx->upload_done = ctx->upload_ready = 0;
    ctx->upload_result = 0;
    http_flv_clear_queue(ctx);
    if (flv_muxer_reset(ctx->muxer) != 0) return -EIO;
    ctx->connected = 1;
    ctx->writer = flv_writer_create2(ctx->audio_stream_id >= 0,
                                     ctx->video_stream_id >= 0,
                                     http_flv_enqueue_vectors, ctx);
    if (!ctx->writer) { ctx->connected = 0; return -EIO; }
    if (salts_thread_create(&ctx->upload_thread, http_flv_upload_thread, ctx) != 0) {
        flv_writer_destroy(ctx->writer); ctx->writer = NULL; ctx->connected = 0;
        return -ENOMEM;
    }
    ctx->upload_thread_started = 1;
    salts_mutex_lock(&ctx->mutex);
    while (!ctx->upload_ready && !ctx->upload_done) salts_cond_wait(&ctx->cond, &ctx->mutex);
    result = ctx->upload_ready ? 0 : ctx->upload_result;
    salts_mutex_unlock(&ctx->mutex);
    if (result != 0) {
        (void)salts_thread_join(&ctx->upload_thread);
        salts_thread_destroy(&ctx->upload_thread);
        ctx->upload_thread_started = 0;
        flv_writer_destroy(ctx->writer); ctx->writer = NULL; ctx->connected = 0;
        return result;
    }
    if (ctx->event_callback)
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_CONNECTED, NULL, ctx->event_user_data);
    return 0;
}

static int http_flv_streamer_disconnect_impl(void *ctx_ptr) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    int result;
    if (!ctx) return -EINVAL;
    if (!ctx->connected) return 0;
    flv_writer_destroy(ctx->writer);
    ctx->writer = NULL;
    salts_mutex_lock(&ctx->mutex);
    ctx->upload_closed = 1;
    salts_cond_broadcast(&ctx->cond);
    salts_mutex_unlock(&ctx->mutex);
    result = salts_thread_join(&ctx->upload_thread);
    if (result == 0) result = ctx->upload_result;
    salts_thread_destroy(&ctx->upload_thread);
    ctx->upload_thread_started = 0;
    if (ctx->transport) { turbo_transport_destroy(ctx->transport); ctx->transport = NULL; }
    ctx->connected = 0;
    if (result != 0) return result;
    if (ctx->event_callback)
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_DISCONNECTED, NULL, ctx->event_user_data);
    return 0;
}

static int http_flv_streamer_write_packet_impl(
    void *ctx_ptr, const turbo_muxer_packet_t *packet) {
    http_flv_streamer_ctx_t *ctx = (http_flv_streamer_ctx_t *)ctx_ptr;
    uint64_t pts_ms, dts_ms;
    int result;
    if (!ctx || !packet || !ctx->connected || !packet->data || packet->size == 0 ||
        packet->pts < 0 || packet->dts < 0) return -EINVAL;
    salts_mutex_lock(&ctx->mutex);
    result = ctx->upload_done || ctx->upload_result != 0 ? -EPIPE : 0;
    salts_mutex_unlock(&ctx->mutex);
    if (result != 0) return result;
    pts_ms = (uint64_t)packet->pts / 1000;
    dts_ms = (uint64_t)packet->dts / 1000;
    if (pts_ms > UINT32_MAX || dts_ms > UINT32_MAX) return -EOVERFLOW;
    if (packet->stream_id == ctx->video_stream_id) {
        if (ctx->video_codec == HTTP_FLV_CODEC_H264)
            result = flv_muxer_avc(ctx->muxer, packet->data, packet->size, (uint32_t)pts_ms, (uint32_t)dts_ms);
        else if (ctx->video_codec == HTTP_FLV_CODEC_H265)
            result = flv_muxer_hevc(ctx->muxer, packet->data, packet->size, (uint32_t)pts_ms, (uint32_t)dts_ms);
        else if (ctx->video_codec == HTTP_FLV_CODEC_H266)
            result = flv_muxer_vvc(ctx->muxer, packet->data, packet->size, (uint32_t)pts_ms, (uint32_t)dts_ms);
        else return -ENOTSUP;
    } else if (packet->stream_id == ctx->audio_stream_id) {
        if (ctx->audio_codec == HTTP_FLV_CODEC_AAC)
            result = flv_muxer_aac(ctx->muxer, packet->data, packet->size, (uint32_t)pts_ms, (uint32_t)dts_ms);
        else if (ctx->audio_codec == HTTP_FLV_CODEC_MP3)
            result = flv_muxer_mp3(ctx->muxer, packet->data, packet->size, (uint32_t)pts_ms, (uint32_t)dts_ms);
        else if (ctx->audio_codec == HTTP_FLV_CODEC_G711A)
            result = flv_muxer_g711a(ctx->muxer, packet->data, packet->size, (uint32_t)pts_ms, (uint32_t)dts_ms);
        else if (ctx->audio_codec == HTTP_FLV_CODEC_G711U)
            result = flv_muxer_g711u(ctx->muxer, packet->data, packet->size, (uint32_t)pts_ms, (uint32_t)dts_ms);
        else return -ENOTSUP;
    } else return -EINVAL;
    if (result != 0) return result;
    ctx->stats.bytes_sent += packet->size;
    ++ctx->stats.packets_sent;
    return 0;
}

static int http_flv_streamer_get_stats_impl(void *ctx_ptr, turbo_streamer_stats_t *stats) {
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
    .name = "http-flv", .protocol = TURBO_STREAMER_HTTP_FLV,
    .create = http_flv_streamer_create, .destroy = http_flv_streamer_destroy_impl,
    .connect = http_flv_streamer_connect_impl, .disconnect = http_flv_streamer_disconnect_impl,
    .add_stream = http_flv_streamer_add_stream_impl,
    .write_packet = http_flv_streamer_write_packet_impl, .read_packet = NULL,
    .get_stats = http_flv_streamer_get_stats_impl,
    .set_event_callback = http_flv_streamer_set_event_callback_impl
};

#endif
