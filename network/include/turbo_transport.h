/**
 * TurboMedia Network Transport Layer
 *
 * 统一的网络传输抽象，封装 Salts CNet 和 CHTTP
 */
#ifndef TURBO_TRANSPORT_H
#define TURBO_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <cnet/cnet.h>
#include <chttp/chttp.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 前向声明（避免直接依赖）
 * ============================================================================= */

/* =============================================================================
 * 传输协议类型
 * ============================================================================= */

typedef enum {
    TURBO_TRANSPORT_TCP,         /* 原始 TCP */
    TURBO_TRANSPORT_TLS,         /* TLS over TCP */
    TURBO_TRANSPORT_UDP,         /* UDP */
    TURBO_TRANSPORT_WEBSOCKET,   /* WebSocket */
    TURBO_TRANSPORT_HTTP,        /* HTTP/HTTPS */
    TURBO_TRANSPORT_RTMP         /* RTMP (基于 TCP) */
} turbo_transport_type_t;

/* =============================================================================
 * 传输配置
 * ============================================================================= */

typedef struct {
    turbo_transport_type_t type;
    
    /* 目标地址 */
    const char *host;
    int port;
    const char *path;           /* HTTP/WebSocket path */
    
    /* 超时配置 */
    int connect_timeout_ms;
    int read_timeout_ms;
    int write_timeout_ms;
    
    /* TLS 配置 */
    int use_tls;
    const char *ca_cert_path;   /* CA 证书路径 */
    int verify_peer;            /* 保留字段；Salts TLS 始终验证证书与主机名 */
    const cnet_tls_client_config *tls;
    
    /* HTTP 特定 */
    const char *user_agent;
    const char *auth_token;     /* Bearer token */
    
    /* 可选的外部 CNet owner；传入后调用方负责其生命周期与串行化。 */
    cnet_client *cnet_client;
    
    /* 可选的外部 CHTTP client。 */
    chttp_client *http_client;
    
} turbo_transport_config_t;

/* =============================================================================
 * 传输句柄
 * ============================================================================= */

typedef struct turbo_transport_s turbo_transport_t;

/**
 * 传输事件回调
 */
typedef enum {
    TURBO_TRANSPORT_EVENT_CONNECTED,
    TURBO_TRANSPORT_EVENT_DISCONNECTED,
    TURBO_TRANSPORT_EVENT_ERROR,
    TURBO_TRANSPORT_EVENT_DATA_RECEIVED
} turbo_transport_event_t;

typedef void (*turbo_transport_event_cb)(turbo_transport_t *transport,
                                         turbo_transport_event_t event,
                                         void *event_data,
                                         void *user_data);

/* =============================================================================
 * 传输 API
 * ============================================================================= */

/**
 * 创建传输实例
 */
TURBO_MEDIA_API turbo_transport_t *turbo_transport_create(const turbo_transport_config_t *config);

/**
 * 销毁传输实例
 */
TURBO_MEDIA_API void turbo_transport_destroy(turbo_transport_t *transport);

/**
 * 连接到服务器（协程内调用）
 */
TURBO_MEDIA_API int turbo_transport_connect(turbo_transport_t *transport);

/**
 * 断开连接
 */
TURBO_MEDIA_API int turbo_transport_disconnect(turbo_transport_t *transport);

/**
 * 发送数据（协程内调用）
 */
TURBO_MEDIA_API int turbo_transport_send(turbo_transport_t *transport,
                                   const uint8_t *data,
                                   size_t size);

/**
 * 接收数据（协程内调用，会挂起等待）
 */
TURBO_MEDIA_API int turbo_transport_recv(turbo_transport_t *transport,
                                   uint8_t **data,
                                   size_t *size);

/**
 * 释放接收到的数据
 */
TURBO_MEDIA_API void turbo_transport_free_recv(turbo_transport_t *transport, uint8_t *data);

/**
 * 检查连接状态
 */
TURBO_MEDIA_API int turbo_transport_is_connected(turbo_transport_t *transport);

/**
 * 设置事件回调
 */
TURBO_MEDIA_API void turbo_transport_set_event_callback(turbo_transport_t *transport,
                                                  turbo_transport_event_cb callback,
                                                  void *user_data);

/**
 * 获取底层 CNet connection handle（如果有）
 */
TURBO_MEDIA_API int turbo_transport_get_connection(
    turbo_transport_t *transport, cnet_connection *connection);

/**
 * 获取底层 CHTTP client（如果有）
 */
TURBO_MEDIA_API chttp_client *turbo_transport_get_http_client(turbo_transport_t *transport);

/* =============================================================================
 * HTTP 特定 API
 * ============================================================================= */

/**
 * HTTP 请求方法
 */
typedef enum {
    TURBO_HTTP_GET,
    TURBO_HTTP_POST,
    TURBO_HTTP_PUT,
    TURBO_HTTP_DELETE,
    TURBO_HTTP_HEAD
} turbo_http_method_t;

/**
 * 发送 HTTP 请求
 */
TURBO_MEDIA_API chttp_response *turbo_transport_http_request(
    turbo_transport_t *transport,
    turbo_http_method_t method,
    const char *path,
    const uint8_t *body,
    size_t body_size,
    const char **headers,
    int header_count);

/**
 * 流式上传（用于 HLS/DASH 分片上传）
 */
typedef size_t (*turbo_transport_read_cb)(uint8_t *buffer, size_t size, void *user_data);

TURBO_MEDIA_API chttp_response *turbo_transport_http_upload_stream(
    turbo_transport_t *transport,
    const char *path,
    turbo_transport_read_cb read_cb,
    size_t content_length,
    void *user_data);

/**
 * 流式下载（用于拉流）
 */
typedef void (*turbo_transport_data_cb)(const uint8_t *data, size_t size, void *user_data);

TURBO_MEDIA_API int turbo_transport_http_download_stream(
    turbo_transport_t *transport,
    const char *path,
    turbo_transport_data_cb data_cb,
    void *user_data);

/* =============================================================================
 * WebSocket 特定 API
 * ============================================================================= */

/**
 * 发送 WebSocket 文本消息
 */
TURBO_MEDIA_API int turbo_transport_ws_send_text(turbo_transport_t *transport,
                                           const char *text);

/**
 * 发送 WebSocket 二进制消息
 */
TURBO_MEDIA_API int turbo_transport_ws_send_binary(turbo_transport_t *transport,
                                             const uint8_t *data,
                                             size_t size);

/**
 * 接收 WebSocket 消息
 */
TURBO_MEDIA_API int turbo_transport_ws_recv(turbo_transport_t *transport,
                                      uint8_t **data,
                                      size_t *size,
                                      int *is_text);

/* =============================================================================
 * 工具函数
 * ============================================================================= */

/**
 * 从 URL 解析传输配置
 * 
 * 支持格式：
 * - tcp://host:port
 * - tls://host:port
 * - ws://host:port/path
 * - wss://host:port/path
 * - http://host:port/path
 * - https://host:port/path
 * - rtmp://host:port/app/stream
 */
TURBO_MEDIA_API int turbo_transport_parse_url(const char *url,
                                        turbo_transport_config_t *config);

/**
 * 获取错误描述
 */
TURBO_MEDIA_API const char *turbo_transport_get_error(turbo_transport_t *transport);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_TRANSPORT_H */
