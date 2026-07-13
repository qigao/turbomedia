/**
 * RTMP Streamer Implementation
 *
 * 基于 refer/librtmp + CoroNet 实现 RTMP 推拉流
 */
#include "turbo_streamer.h"
#include "turbo_transport.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef TURBO_MEDIA_HAS_RTMP

#include "rtmp-client.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_socket.h"

/* =============================================================================
 * RTMP Streamer 上下文
 * ============================================================================= */

typedef struct {
    /* RTMP 配置 */
    char *url;
    char *app;
    char *stream_key;
    int chunk_size;
    
    /* RTMP 状态 */
    rtmp_client_t *rtmp;
    
    /* 网络传输 */
    turbo_transport_t *transport;
    coro_context_t *coro_ctx;
    int owns_context;
    
    /* 流信息 */
    turbo_stream_info_t video_info;
    turbo_stream_info_t audio_info;
    int has_video;
    int has_audio;
    
    /* 元数据 */
    char *metadata_title;
    char *metadata_author;
    
    /* 事件回调 */
    turbo_streamer_event_cb event_callback;
    void *event_user_data;
    
    /* 统计 */
    turbo_streamer_stats_t stats;
    
    /* 状态 */
    int connected;
    int published;
    
} rtmp_streamer_ctx_t;

/* =============================================================================
 * RTMP URL 解析
 * ============================================================================= */

static int rtmp_parse_url(const char *url, char **host, int *port, 
                          char **app, char **stream) {
    if (!url) return -1;
    
    /* 格式: rtmp://host:port/app/stream */
    const char *p = url;
    
    /* 跳过 scheme */
    if (strncmp(p, "rtmp://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "rtmps://", 8) == 0) {
        p += 8;
    } else {
        return -1;
    }
    
    /* 解析 host */
    const char *host_start = p;
    const char *host_end = strchr(p, ':');
    if (!host_end) host_end = strchr(p, '/');
    if (!host_end) return -1;
    
    size_t host_len = host_end - host_start;
    *host = (char *)malloc(host_len + 1);
    memcpy(*host, host_start, host_len);
    (*host)[host_len] = '\0';
    
    p = host_end;
    
    /* 解析 port */
    if (*p == ':') {
        p++;
        *port = atoi(p);
        p = strchr(p, '/');
        if (!p) {
            free(*host);
            return -1;
        }
    } else {
        *port = 1935;  /* 默认 RTMP 端口 */
    }
    
    /* 解析 app 和 stream */
    p++;  /* 跳过 '/' */
    const char *app_start = p;
    const char *app_end = strchr(p, '/');
    if (!app_end) {
        free(*host);
        return -1;
    }
    
    size_t app_len = app_end - app_start;
    *app = (char *)malloc(app_len + 1);
    memcpy(*app, app_start, app_len);
    (*app)[app_len] = '\0';
    
    /* Stream key */
    p = app_end + 1;
    *stream = strdup(p);
    
    return 0;
}

/* =============================================================================
 * 协程网络回调
 * ============================================================================= */

static void rtmp_transport_event(turbo_transport_t *transport,
                                 turbo_transport_event_t event,
                                 void *event_data,
                                 void *user_data) {
    rtmp_streamer_ctx_t *ctx = (rtmp_streamer_ctx_t *)user_data;
    
    switch (event) {
        case TURBO_TRANSPORT_EVENT_CONNECTED:
            if (ctx->event_callback) {
                ctx->event_callback(NULL, TURBO_STREAMER_EVENT_CONNECTED, 
                                  NULL, ctx->event_user_data);
            }
            break;
            
        case TURBO_TRANSPORT_EVENT_DISCONNECTED:
            ctx->connected = 0;
            ctx->published = 0;
            if (ctx->event_callback) {
                ctx->event_callback(NULL, TURBO_STREAMER_EVENT_DISCONNECTED,
                                  NULL, ctx->event_user_data);
            }
            break;
            
        case TURBO_TRANSPORT_EVENT_ERROR:
            if (ctx->event_callback) {
                ctx->event_callback(NULL, TURBO_STREAMER_EVENT_ERROR,
                                  event_data, ctx->event_user_data);
            }
            break;
            
        default:
            break;
    }
}

/* =============================================================================
 * Streamer 操作实现
 * ============================================================================= */

static void *rtmp_streamer_create(const turbo_streamer_config_t *config) {
    rtmp_streamer_ctx_t *ctx = (rtmp_streamer_ctx_t *)calloc(1, sizeof(rtmp_streamer_ctx_t));
    if (!ctx) return NULL;
    
    /* 保存配置 */
    ctx->url = config->url ? strdup(config->url) : NULL;
    ctx->app = config->app ? strdup(config->app) : NULL;
    ctx->stream_key = config->stream_key ? strdup(config->stream_key) : NULL;
    ctx->chunk_size = config->chunk_size > 0 ? config->chunk_size : 4096;
    
    /* 获取或创建 CoroNet 上下文 */
    if (config->coro_context) {
        ctx->coro_ctx = (coro_context_t *)config->coro_context;
        ctx->owns_context = 0;
    } else {
        ctx->coro_ctx = coro_context_create(NULL);
        if (!ctx->coro_ctx) {
            free(ctx->url);
            free(ctx->app);
            free(ctx->stream_key);
            free(ctx);
            return NULL;
        }
        ctx->owns_context = 1;
    }
    
    /* 初始化统计 */
    ctx->stats.uptime_ms = 0;
    
    return ctx;
}

static void rtmp_streamer_destroy_impl(void *ctx_ptr) {
    rtmp_streamer_ctx_t *ctx = (rtmp_streamer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    
    /* 清理 RTMP */
    if (ctx->rtmp) {
        rtmp_client_destroy(ctx->rtmp);
    }
    
    /* 清理传输层 */
    if (ctx->transport) {
        turbo_transport_destroy(ctx->transport);
    }
    
    /* 清理上下文 */
    if (ctx->owns_context && ctx->coro_ctx) {
        coro_context_destroy(ctx->coro_ctx);
    }
    
    free(ctx->url);
    free(ctx->app);
    free(ctx->stream_key);
    free(ctx->metadata_title);
    free(ctx->metadata_author);
    free(ctx);
}

static int rtmp_streamer_connect_impl(void *ctx_ptr) {
    rtmp_streamer_ctx_t *ctx = (rtmp_streamer_ctx_t *)ctx_ptr;
    
    if (ctx->connected) {
        return 0;  /* 已连接 */
    }
    
    /* 解析 URL */
    char *host = NULL;
    int port = 0;
    char *app = NULL;
    char *stream = NULL;
    
    int ret = rtmp_parse_url(ctx->url, &host, &port, &app, &stream);
    if (ret < 0) {
        return -1;
    }
    
    /* 创建传输层 */
    turbo_transport_config_t transport_config = {
        .type = TURBO_TRANSPORT_TCP,
        .host = host,
        .port = port,
        .connect_timeout_ms = 5000,
        .coro_ctx = ctx->coro_ctx
    };
    
    ctx->transport = turbo_transport_create(&transport_config);
    free(host);
    
    if (!ctx->transport) {
        free(app);
        free(stream);
        return -1;
    }
    
    /* 设置事件回调 */
    turbo_transport_set_event_callback(ctx->transport, rtmp_transport_event, ctx);
    
    /* 连接到服务器 */
    ret = turbo_transport_connect(ctx->transport);
    if (ret < 0) {
        free(app);
        free(stream);
        return -1;
    }
    
    /* 创建 RTMP 客户端 */
    ctx->rtmp = rtmp_client_create(app, stream, ctx->url, RTMP_CLIENT_PUBLISH);
    free(app);
    free(stream);
    
    if (!ctx->rtmp) {
        return -1;
    }
    
    /* TODO: RTMP 握手 */
    /* rtmp_client_start(ctx->rtmp) */
    
    ctx->connected = 1;
    
    return 0;
}

static int rtmp_streamer_disconnect_impl(void *ctx_ptr) {
    rtmp_streamer_ctx_t *ctx = (rtmp_streamer_ctx_t *)ctx_ptr;
    
    if (!ctx->connected) {
        return 0;
    }
    
    /* 停止推流 */
    if (ctx->published && ctx->rtmp) {
        /* rtmp_client_stop(ctx->rtmp) */
    }
    
    /* 断开连接 */
    if (ctx->transport) {
        turbo_transport_disconnect(ctx->transport);
    }
    
    ctx->connected = 0;
    ctx->published = 0;
    
    return 0;
}

static int rtmp_streamer_add_stream_impl(void *ctx_ptr, 
                                        const turbo_stream_info_t *stream_info,
                                        int *stream_id) {
    rtmp_streamer_ctx_t *ctx = (rtmp_streamer_ctx_t *)ctx_ptr;
    
    if (stream_info->type == TURBO_CODEC_TYPE_VIDEO) {
        if (ctx->has_video) {
            return -1;  /* RTMP 只支持一个视频轨 */
        }
        ctx->video_info = *stream_info;
        ctx->has_video = 1;
        *stream_id = 0;
    } else if (stream_info->type == TURBO_CODEC_TYPE_AUDIO) {
        if (ctx->has_audio) {
            return -1;  /* RTMP 只支持一个音频轨 */
        }
        ctx->audio_info = *stream_info;
        ctx->has_audio = 1;
        *stream_id = 1;
    } else {
        return -1;
    }
    
    return 0;
}

static int rtmp_streamer_write_packet_impl(void *ctx_ptr, 
                                           const turbo_muxer_packet_t *packet) {
    rtmp_streamer_ctx_t *ctx = (rtmp_streamer_ctx_t *)ctx_ptr;
    
    if (!ctx->connected || !ctx->rtmp) {
        return -1;
    }
    
    /* 开始推流（首次）*/
    if (!ctx->published) {
        /* rtmp_client_publish(ctx->rtmp) */
        ctx->published = 1;
    }
    
    /* 发送数据包 */
    /* 这里需要调用 librtmp 的发送函数 */
    /* rtmp_client_send_video/audio(ctx->rtmp, packet->data, packet->size, packet->pts) */
    
    ctx->stats.bytes_sent += packet->size;
    ctx->stats.packets_sent++;
    
    return 0;
}

static int rtmp_streamer_get_stats_impl(void *ctx_ptr, turbo_streamer_stats_t *stats) {
    rtmp_streamer_ctx_t *ctx = (rtmp_streamer_ctx_t *)ctx_ptr;
    *stats = ctx->stats;
    return 0;
}

static void rtmp_streamer_set_event_callback_impl(void *ctx_ptr,
                                                  turbo_streamer_event_cb callback,
                                                  void *user_data) {
    rtmp_streamer_ctx_t *ctx = (rtmp_streamer_ctx_t *)ctx_ptr;
    ctx->event_callback = callback;
    ctx->event_user_data = user_data;
}

/* =============================================================================
 * RTMP Streamer 操作表
 * ============================================================================= */

const turbo_streamer_ops_t turbo_rtmp_streamer_ops = {
    .name = "rtmp",
    .protocol = TURBO_STREAMER_RTMP,
    
    .create = rtmp_streamer_create,
    .destroy = rtmp_streamer_destroy_impl,
    .connect = rtmp_streamer_connect_impl,
    .disconnect = rtmp_streamer_disconnect_impl,
    .add_stream = rtmp_streamer_add_stream_impl,
    .write_packet = rtmp_streamer_write_packet_impl,
    .read_packet = NULL,  /* TODO: 实现拉流 */
    .get_stats = rtmp_streamer_get_stats_impl,
    .set_event_callback = rtmp_streamer_set_event_callback_impl
};

#endif /* TURBO_MEDIA_HAS_RTMP */
