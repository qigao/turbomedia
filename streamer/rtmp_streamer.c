/**
 * RTMP Streamer Implementation
 *
 * 基于 refer/librtmp + CoroNet 实现 RTMP 推拉流
 */
#include "turbo_streamer.h"
#include "turbo_transport.h"
#include "rtmp_streamer_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef TURBO_MEDIA_HAS_RTMP

#include "rtmp-client.h"
#include "amf0.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_socket.h"

static const char RTMP_METADATA_TITLE[] = "title";
static const char RTMP_METADATA_AUTHOR[] = "author";
static const char RTMP_METADATA_EVENT[] = "onMetaData";

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

static int rtmp_metadata_field_size(size_t name_size, size_t value_size, size_t *field_size) {
    size_t length_size;

    if (!field_size || name_size > UINT16_MAX || value_size > UINT32_MAX) return -1;
    length_size = value_size < ((size_t)UINT16_MAX + 1U) ? 2U : 4U;
    if (value_size > SIZE_MAX - name_size - length_size - 3U) return -1;
    *field_size = name_size + value_size + length_size + 3U;
    return 0;
}

static int rtmp_streamer_send_metadata(rtmp_streamer_ctx_t *ctx) {
    const size_t event_size = sizeof(RTMP_METADATA_EVENT) - 1U;
    size_t payload_size = 1U + 2U + event_size + 1U + 3U;
    size_t field_size;
    uint8_t *payload;
    uint8_t *ptr;
    const uint8_t *end;
    int result;

    if (!ctx || !ctx->rtmp) return -1;
    if (!ctx->metadata_title && !ctx->metadata_author) return 0;

    if (ctx->metadata_title) {
        if (rtmp_metadata_field_size(sizeof(RTMP_METADATA_TITLE) - 1U,
                                     strlen(ctx->metadata_title), &field_size) != 0 ||
            field_size > SIZE_MAX - payload_size) {
            return -1;
        }
        payload_size += field_size;
    }
    if (ctx->metadata_author) {
        if (rtmp_metadata_field_size(sizeof(RTMP_METADATA_AUTHOR) - 1U,
                                     strlen(ctx->metadata_author), &field_size) != 0 ||
            field_size > SIZE_MAX - payload_size) {
            return -1;
        }
        payload_size += field_size;
    }

    payload = (uint8_t *)malloc(payload_size);
    if (!payload) return -1;
    ptr = payload;
    end = payload + payload_size;

    ptr = AMFWriteString(ptr, end, RTMP_METADATA_EVENT, event_size);
    ptr = ptr ? AMFWriteObject(ptr, end) : NULL;
    if (ptr && ctx->metadata_title) {
        ptr = AMFWriteNamedString(ptr, end, RTMP_METADATA_TITLE,
                                  sizeof(RTMP_METADATA_TITLE) - 1U,
                                  ctx->metadata_title, strlen(ctx->metadata_title));
    }
    if (ptr && ctx->metadata_author) {
        ptr = AMFWriteNamedString(ptr, end, RTMP_METADATA_AUTHOR,
                                  sizeof(RTMP_METADATA_AUTHOR) - 1U,
                                  ctx->metadata_author, strlen(ctx->metadata_author));
    }
    ptr = ptr ? AMFWriteObjectEnd(ptr, end) : NULL;
    if (!ptr || ptr != end) {
        free(payload);
        return -1;
    }

    result = rtmp_client_push_script(ctx->rtmp, payload, payload_size, 0);
    free(payload);
    return result == 0 ? 0 : -1;
}

static char *rtmp_metadata_copy(const char *value) {
    size_t size;
    char *copy;

    if (!value) return NULL;
    size = strlen(value);
    if (size == SIZE_MAX) return NULL;
    copy = (char *)malloc(size + 1U);
    if (!copy) return NULL;
    memcpy(copy, value, size + 1U);
    return copy;
}

int turbo_rtmp_streamer_set_metadata(void *ctx_ptr, const char *key, const char *value) {
    rtmp_streamer_ctx_t *ctx = (rtmp_streamer_ctx_t *)ctx_ptr;
    char **slot;
    char *old_value;
    char *new_value;

    if (!ctx || !key || !value) return -1;
    if (strcmp(key, RTMP_METADATA_TITLE) == 0) {
        slot = &ctx->metadata_title;
    } else if (strcmp(key, RTMP_METADATA_AUTHOR) == 0) {
        slot = &ctx->metadata_author;
    } else {
        return -1;
    }

    new_value = rtmp_metadata_copy(value);
    if (!new_value) return -1;
    old_value = *slot;
    *slot = new_value;

    if (ctx->connected && rtmp_streamer_send_metadata(ctx) != 0) {
        *slot = old_value;
        free(new_value);
        return -1;
    }

    free(old_value);
    return 0;
}

static int rtmp_client_send_data(void *param, const void *header, size_t header_size,
                                 const void *payload, size_t payload_size) {
    rtmp_streamer_ctx_t *ctx = (rtmp_streamer_ctx_t *)param;
    int sent;

    if (header_size > 0) {
        sent = turbo_transport_send(ctx->transport, (const uint8_t *)header, header_size);
        if (sent != (int)header_size) return -1;
    }
    if (payload_size > 0) {
        sent = turbo_transport_send(ctx->transport, (const uint8_t *)payload, payload_size);
        if (sent != (int)payload_size) return -1;
    }
    return (int)(header_size + payload_size);
}

static int rtmp_client_ignore_media(void *param, const void *data, size_t size,
                                    uint32_t timestamp) {
    (void)param;
    (void)data;
    (void)size;
    (void)timestamp;
    return 0;
}

static const struct rtmp_client_handler_t s_rtmp_client_handler = {
    rtmp_client_send_data,
    rtmp_client_ignore_media,
    rtmp_client_ignore_media,
    rtmp_client_ignore_media
};

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
    ctx->rtmp = rtmp_client_create(app, stream, ctx->url, ctx, &s_rtmp_client_handler);
    free(app);
    free(stream);
    
    if (!ctx->rtmp) {
        return -1;
    }
    
    ret = rtmp_client_start(ctx->rtmp, 0);
    while (0 == ret && rtmp_client_getstate(ctx->rtmp) < 4) {
        uint8_t *data = NULL;
        size_t size = 0;
        ret = turbo_transport_recv(ctx->transport, &data, &size);
        if (ret > 0) ret = rtmp_client_input(ctx->rtmp, data, size);
        turbo_transport_free_recv(ctx->transport, data);
    }
    if (ret != 0) return -1;

    if (rtmp_streamer_send_metadata(ctx) != 0) return -1;
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
        if (0 != rtmp_client_stop(ctx->rtmp)) return -1;
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
        ctx->published = 1;
    }
    
    if (packet->stream_id == 0) {
        if (0 != rtmp_client_push_video(ctx->rtmp, packet->data, packet->size,
                                        (uint32_t)packet->pts)) return -1;
    } else if (packet->stream_id == 1) {
        if (0 != rtmp_client_push_audio(ctx->rtmp, packet->data, packet->size,
                                        (uint32_t)packet->pts)) return -1;
    } else {
        return -1;
    }
    
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
    .read_packet = NULL,
    .get_stats = rtmp_streamer_get_stats_impl,
    .set_event_callback = rtmp_streamer_set_event_callback_impl
};

#endif /* TURBO_MEDIA_HAS_RTMP */
