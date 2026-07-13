/**
 * HLS Streamer Implementation
 *
 * 基于 refer/libhls 实现 HLS 流媒体打包和传输
 * 集成 TurboHTTP::HttpClient 用于分片上传
 */
#include "turbo_streamer.h"
#include "turbo_transport.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef TURBO_MEDIA_HAS_HLS

#include "hls-m3u8.h"
#include "hls-media.h"
#include "http_client.h"

/* =============================================================================
 * HLS Streamer 上下文
 * ============================================================================= */

#define MAX_SEGMENTS 10

typedef struct {
    char *filename;
    size_t size;
    uint64_t duration_ms;
    int sequence;
} hls_segment_t;

typedef struct {
    /* HLS 配置 */
    char *output_dir;
    char *base_url;
    int segment_duration_ms;
    int playlist_size;
    turbo_hls_playlist_type_t playlist_type;
    
    /* HLS 状态 */
    hls_m3u8_t *m3u8;
    hls_media_t *media;
    
    /* 分片管理 */
    hls_segment_t segments[MAX_SEGMENTS];
    int segment_count;
    int current_sequence;
    
    /* 流信息 */
    turbo_stream_info_t video_info;
    turbo_stream_info_t audio_info;
    int has_video;
    int has_audio;
    
    /* 当前分片 */
    FILE *current_segment_fp;
    char current_segment_path[512];
    uint64_t current_segment_start_time;
    uint64_t current_segment_duration;
    
    /* 网络传输（用于上传分片）*/
    turbo_transport_t *transport;
    http_client_t *http_client;
    
    /* 事件回调 */
    turbo_streamer_event_cb event_callback;
    void *event_user_data;
    
    /* 统计 */
    turbo_streamer_stats_t stats;
    
} hls_streamer_ctx_t;

/* =============================================================================
 * HLS 分片管理
 * ============================================================================= */

static int hls_start_new_segment(hls_streamer_ctx_t *ctx) {
    char filename[256];
    
    /* 关闭上一个分片 */
    if (ctx->current_segment_fp) {
        fclose(ctx->current_segment_fp);
        ctx->current_segment_fp = NULL;
        
        /* 触发分片就绪事件 */
        if (ctx->event_callback) {
            ctx->event_callback(NULL, TURBO_STREAMER_EVENT_SEGMENT_READY,
                              ctx->current_segment_path, ctx->event_user_data);
        }
        
        /* 如果配置了 HTTP 上传，上传分片到服务器 */
        if (ctx->http_client && ctx->base_url) {
            /* TODO: 使用 http_upload_file_stream 上传分片 */
        }
    }
    
    /* 生成新分片文件名 */
    snprintf(filename, sizeof(filename), "segment_%d.ts", ctx->current_sequence);
    snprintf(ctx->current_segment_path, sizeof(ctx->current_segment_path),
             "%s/%s", ctx->output_dir, filename);
    
    /* 打开新分片文件 */
    ctx->current_segment_fp = fopen(ctx->current_segment_path, "wb");
    if (!ctx->current_segment_fp) {
        return -1;
    }
    
    /* 创建 HLS media muxer */
    if (ctx->media) {
        hls_media_destroy(ctx->media);
    }
    
    /* TODO: 初始化 hls_media_t */
    
    ctx->current_segment_start_time = 0; /* 应该从数据包获取 */
    ctx->current_segment_duration = 0;
    ctx->current_sequence++;
    
    return 0;
}

static int hls_update_playlist(hls_streamer_ctx_t *ctx) {
    char playlist_path[512];
    FILE *fp;
    
    if (!ctx->m3u8) {
        ctx->m3u8 = hls_m3u8_create(0, ctx->playlist_size);
        if (!ctx->m3u8) {
            return -1;
        }
    }
    
    /* 设置播放列表类型 */
    switch (ctx->playlist_type) {
        case TURBO_HLS_VOD:
            /* VOD 模式 */
            break;
        case TURBO_HLS_LIVE:
            /* 直播模式 */
            break;
        case TURBO_HLS_EVENT:
            /* 事件模式 */
            break;
    }
    
    /* TODO: 添加分片到播放列表 */
    /* hls_m3u8_add(ctx->m3u8, segment_path, duration); */
    
    /* 写入播放列表文件 */
    snprintf(playlist_path, sizeof(playlist_path), "%s/playlist.m3u8", ctx->output_dir);
    fp = fopen(playlist_path, "w");
    if (!fp) {
        return -1;
    }
    
    /* TODO: 生成 M3U8 内容并写入 */
    
    fclose(fp);
    
    return 0;
}

/* =============================================================================
 * Streamer 操作实现
 * ============================================================================= */

static void *hls_streamer_create(const turbo_streamer_config_t *config) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)calloc(1, sizeof(hls_streamer_ctx_t));
    if (!ctx) return NULL;
    
    /* 复制配置 */
    ctx->output_dir = config->output_dir ? strdup(config->output_dir) : strdup(".");
    ctx->base_url = config->base_url ? strdup(config->base_url) : NULL;
    ctx->segment_duration_ms = config->segment_duration_ms > 0 ? 
                               config->segment_duration_ms : 6000; /* 默认 6 秒 */
    ctx->playlist_size = config->playlist_size > 0 ? config->playlist_size : 5;
    ctx->playlist_type = TURBO_HLS_LIVE;
    
    /* 保存 HttpClient 引用 */
    ctx->http_client = (http_client_t *)config->http_client;
    
    /* 初始化统计 */
    ctx->stats.uptime_ms = time(NULL) * 1000;
    
    return ctx;
}

static void hls_streamer_destroy_impl(void *ctx_ptr) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    
    /* 关闭当前分片 */
    if (ctx->current_segment_fp) {
        fclose(ctx->current_segment_fp);
    }
    
    /* 清理 HLS 资源 */
    if (ctx->m3u8) {
        hls_m3u8_destroy(ctx->m3u8);
    }
    
    if (ctx->media) {
        hls_media_destroy(ctx->media);
    }
    
    /* 清理传输层 */
    if (ctx->transport) {
        turbo_transport_destroy(ctx->transport);
    }
    
    free(ctx->output_dir);
    free(ctx->base_url);
    free(ctx);
}

static int hls_streamer_connect_impl(void *ctx_ptr) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    
    /* HLS 不需要持久连接，但如果配置了上传服务器，可以预连接 */
    if (ctx->base_url && ctx->http_client) {
        /* 可选：测试连接到上传服务器 */
    }
    
    /* 触发连接事件 */
    if (ctx->event_callback) {
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_CONNECTED, NULL, ctx->event_user_data);
    }
    
    return 0;
}

static int hls_streamer_disconnect_impl(void *ctx_ptr) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    
    /* 完成最后一个分片 */
    if (ctx->current_segment_fp) {
        fclose(ctx->current_segment_fp);
        ctx->current_segment_fp = NULL;
    }
    
    /* 更新播放列表 */
    hls_update_playlist(ctx);
    
    /* 触发断开事件 */
    if (ctx->event_callback) {
        ctx->event_callback(NULL, TURBO_STREAMER_EVENT_DISCONNECTED, NULL, ctx->event_user_data);
    }
    
    return 0;
}

static int hls_streamer_add_stream_impl(void *ctx_ptr, const turbo_stream_info_t *stream_info, int *stream_id) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    
    if (stream_info->type == TURBO_CODEC_TYPE_VIDEO) {
        ctx->video_info = *stream_info;
        ctx->has_video = 1;
        *stream_id = 0;
    } else if (stream_info->type == TURBO_CODEC_TYPE_AUDIO) {
        ctx->audio_info = *stream_info;
        ctx->has_audio = 1;
        *stream_id = 1;
    } else {
        return -1;
    }
    
    return 0;
}

static int hls_streamer_write_packet_impl(void *ctx_ptr, const turbo_muxer_packet_t *packet) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    
    /* 检查是否需要开始新分片 */
    if (!ctx->current_segment_fp || 
        ctx->current_segment_duration >= ctx->segment_duration_ms) {
        
        /* 只在关键帧时切换分片 */
        if (packet->is_keyframe) {
            if (hls_start_new_segment(ctx) < 0) {
                return -1;
            }
        }
    }
    
    /* 写入数据到当前分片 */
    if (ctx->current_segment_fp) {
        /* TODO: 通过 hls_media_t 写入 TS 格式 */
        /* 这里需要调用 libmpeg 的 TS muxer */
        
        ctx->stats.bytes_sent += packet->size;
        ctx->stats.packets_sent++;
    }
    
    /* 更新分片时长 */
    ctx->current_segment_duration = packet->pts / 1000 - ctx->current_segment_start_time;
    
    return 0;
}

static int hls_streamer_get_stats_impl(void *ctx_ptr, turbo_streamer_stats_t *stats) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    *stats = ctx->stats;
    return 0;
}

static void hls_streamer_set_event_callback_impl(void *ctx_ptr, 
                                                 turbo_streamer_event_cb callback,
                                                 void *user_data) {
    hls_streamer_ctx_t *ctx = (hls_streamer_ctx_t *)ctx_ptr;
    ctx->event_callback = callback;
    ctx->event_user_data = user_data;
}

/* =============================================================================
 * HLS Streamer 操作表
 * ============================================================================= */

const turbo_streamer_ops_t turbo_hls_streamer_ops = {
    .name = "hls",
    .protocol = TURBO_STREAMER_HLS,
    
    .create = hls_streamer_create,
    .destroy = hls_streamer_destroy_impl,
    .connect = hls_streamer_connect_impl,
    .disconnect = hls_streamer_disconnect_impl,
    .add_stream = hls_streamer_add_stream_impl,
    .write_packet = hls_streamer_write_packet_impl,
    .read_packet = NULL, /* HLS 主要用于推流 */
    .get_stats = hls_streamer_get_stats_impl,
    .set_event_callback = hls_streamer_set_event_callback_impl
};

#endif /* TURBO_MEDIA_HAS_HLS */
