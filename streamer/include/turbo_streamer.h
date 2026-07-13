/**
 * TurboMedia Streamer Abstraction
 *
 * 统一的流媒体协议接口，支持 HLS、DASH、RTMP 等
 * 集成 TurboNet::CoroNet 和 TurboHTTP::HttpClient
 */
#ifndef TURBO_STREAMER_H
#define TURBO_STREAMER_H

#include <stdint.h>
#include <stddef.h>
#include <turbo_export.h>
#include <turbo_codec.h>
#include <turbo_demuxer.h>
#include <turbo_muxer.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * 流媒体协议类型
 */
typedef enum {
    TURBO_STREAMER_HLS,         /* HTTP Live Streaming */
    TURBO_STREAMER_DASH,        /* MPEG-DASH */
    TURBO_STREAMER_RTMP,        /* Real-Time Messaging Protocol */
    TURBO_STREAMER_HTTP_FLV,    /* HTTP-FLV 直播流 */
    TURBO_STREAMER_WEBRTC       /* WebRTC (未来扩展) */
} turbo_streamer_protocol_t;

/**
 * Streamer 配置
 */
typedef struct {
    turbo_streamer_protocol_t protocol;
    
    /* 通用配置 */
    const char *url;            /* 推流/拉流 URL */
    int timeout_ms;             /* 超时时间 */
    int buffer_size;            /* 缓冲区大小 */
    
    /* HLS/DASH 特定 */
    int segment_duration_ms;    /* 分片时长 */
    int playlist_size;          /* 播放列表保留分片数 */
    const char *output_dir;     /* 输出目录 */
    const char *base_url;       /* M3U8/MPD 中的基础 URL */
    
    /* RTMP 特定 */
    const char *app;            /* RTMP 应用名 */
    const char *stream_key;     /* 推流密钥 */
    int chunk_size;             /* RTMP chunk size */
    
    /* 网络配置（使用 CoroNet/HttpClient）*/
    void *coro_context;         /* CoroNet 协程上下文 */
    void *http_client;          /* TurboHTTP 客户端 */
    
} turbo_streamer_config_t;

/**
 * Streamer 统计信息
 */
typedef struct {
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t packets_dropped;
    int current_bitrate_bps;
    int64_t uptime_ms;
} turbo_streamer_stats_t;

/* 前向声明 */
typedef struct turbo_streamer_s turbo_streamer_t;

/**
 * Streamer 事件回调
 */
typedef enum {
    TURBO_STREAMER_EVENT_CONNECTED,
    TURBO_STREAMER_EVENT_DISCONNECTED,
    TURBO_STREAMER_EVENT_ERROR,
    TURBO_STREAMER_EVENT_SEGMENT_READY,  /* HLS/DASH: 新分片就绪 */
    TURBO_STREAMER_EVENT_BITRATE_CHANGE
} turbo_streamer_event_t;

typedef void (*turbo_streamer_event_cb)(turbo_streamer_t *streamer,
                                        turbo_streamer_event_t event,
                                        void *event_data,
                                        void *user_data);

/**
 * Streamer 操作表
 */
typedef struct {
    const char *name;
    turbo_streamer_protocol_t protocol;

    /* 创建/销毁 */
    void *(*create)(const turbo_streamer_config_t *config);
    void (*destroy)(void *ctx);

    /* 连接/断开 */
    int (*connect)(void *ctx);
    int (*disconnect)(void *ctx);

    /* 添加流轨道 */
    int (*add_stream)(void *ctx, const turbo_stream_info_t *stream_info, int *stream_id);

    /* 写入数据包（推流）*/
    int (*write_packet)(void *ctx, const turbo_muxer_packet_t *packet);

    /* 读取数据包（拉流）*/
    int (*read_packet)(void *ctx, turbo_demuxer_packet_t *packet);

    /* 获取统计信息 */
    int (*get_stats)(void *ctx, turbo_streamer_stats_t *stats);

    /* 设置事件回调 */
    void (*set_event_callback)(void *ctx, turbo_streamer_event_cb callback, void *user_data);

} turbo_streamer_ops_t;

/**
 * Streamer 实例
 */
struct turbo_streamer_s {
    const turbo_streamer_ops_t *ops;
    void *ctx;
    turbo_streamer_config_t config;
    int connected;
};

/* =============================================================================
 * Streamer 注册表
 * ============================================================================= */

/**
 * 初始化 Streamer 注册表
 */
CXX_C_API void turbo_streamer_registry_init(void);

/**
 * 关闭 Streamer 注册表
 */
CXX_C_API void turbo_streamer_registry_shutdown(void);

/**
 * 注册 Streamer
 */
CXX_C_API int turbo_streamer_register(const turbo_streamer_ops_t *ops);

/**
 * 根据协议查找 Streamer
 */
CXX_C_API const turbo_streamer_ops_t *turbo_streamer_find_by_protocol(
    turbo_streamer_protocol_t protocol);

/**
 * 根据名称查找 Streamer
 */
CXX_C_API const turbo_streamer_ops_t *turbo_streamer_find_by_name(const char *name);

/**
 * 根据 URL 自动检测协议
 */
CXX_C_API turbo_streamer_protocol_t turbo_streamer_detect_protocol(const char *url);

/* =============================================================================
 * Streamer 实例操作
 * ============================================================================= */

/**
 * 创建 Streamer 实例
 */
CXX_C_API turbo_streamer_t *turbo_streamer_create(const turbo_streamer_config_t *config);

/**
 * 销毁 Streamer 实例
 */
CXX_C_API void turbo_streamer_destroy(turbo_streamer_t *streamer);

/**
 * 连接到流媒体服务器
 */
CXX_C_API int turbo_streamer_connect(turbo_streamer_t *streamer);

/**
 * 断开连接
 */
CXX_C_API int turbo_streamer_disconnect(turbo_streamer_t *streamer);

/**
 * 添加流轨道
 */
CXX_C_API int turbo_streamer_add_stream(turbo_streamer_t *streamer,
                                        const turbo_stream_info_t *stream_info,
                                        int *stream_id);

/**
 * 写入数据包（推流）
 */
CXX_C_API int turbo_streamer_write_packet(turbo_streamer_t *streamer,
                                          const turbo_muxer_packet_t *packet);

/**
 * 读取数据包（拉流）
 */
CXX_C_API int turbo_streamer_read_packet(turbo_streamer_t *streamer,
                                         turbo_demuxer_packet_t *packet);

/**
 * 获取统计信息
 */
CXX_C_API int turbo_streamer_get_stats(turbo_streamer_t *streamer,
                                       turbo_streamer_stats_t *stats);

/**
 * 设置事件回调
 */
CXX_C_API void turbo_streamer_set_event_callback(turbo_streamer_t *streamer,
                                                 turbo_streamer_event_cb callback,
                                                 void *user_data);

/* =============================================================================
 * HLS 特定接口
 * ============================================================================= */

/**
 * HLS 播放列表类型
 */
typedef enum {
    TURBO_HLS_VOD,              /* 点播 */
    TURBO_HLS_LIVE,             /* 直播 */
    TURBO_HLS_EVENT             /* 事件（类似直播但有固定终点）*/
} turbo_hls_playlist_type_t;

/**
 * 获取 M3U8 播放列表内容
 */
CXX_C_API int turbo_streamer_hls_get_playlist(turbo_streamer_t *streamer,
                                              char **playlist,
                                              size_t *size);

/**
 * 设置 HLS 播放列表类型
 */
CXX_C_API void turbo_streamer_hls_set_playlist_type(turbo_streamer_t *streamer,
                                                    turbo_hls_playlist_type_t type);

/* =============================================================================
 * RTMP 特定接口
 * ============================================================================= */

/**
 * 设置 RTMP 元数据
 */
CXX_C_API int turbo_streamer_rtmp_set_metadata(turbo_streamer_t *streamer,
                                               const char *key,
                                               const char *value);

/* =============================================================================
 * 内置 Streamer 声明
 * ============================================================================= */

#ifdef TURBO_MEDIA_HAS_HLS
extern const turbo_streamer_ops_t turbo_hls_streamer_ops;
#endif

#ifdef TURBO_MEDIA_HAS_DASH
extern const turbo_streamer_ops_t turbo_dash_streamer_ops;
#endif

#ifdef TURBO_MEDIA_HAS_RTMP
extern const turbo_streamer_ops_t turbo_rtmp_streamer_ops;
#endif

#ifdef TURBO_MEDIA_HAS_HTTP_FLV
extern const turbo_streamer_ops_t turbo_http_flv_streamer_ops;
#endif

#ifdef __cplusplus
}
#endif

#endif /* TURBO_STREAMER_H */
