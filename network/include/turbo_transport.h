/**
 * TurboMedia Network Transport Layer
 *
 * 统一的网络传输抽象，封装 CoroNet 和 TurboHTTP
 */
#ifndef TURBO_TRANSPORT_H
#define TURBO_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 前向声明（避免直接依赖）
 * ============================================================================= */

struct coro_context_s;
struct coro_socket_s;
struct http_client_s;
struct http_response_s;

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
    int verify_peer;            /* 是否验证对端证书 */
    
    /* HTTP 特定 */
    const char *user_agent;
    const char *auth_token;     /* Bearer token */
    
    /* CoroNet 上下文（如果已有）*/
    struct coro_context_s *coro_ctx;
    
    /* HttpClient 实例（如果已有）*/
    struct http_client_s *http_client;
    
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
CXX_C_API turbo_transport_t *turbo_transport_create(const turbo_transport_config_t *config);

/**
 * 销毁传输实例
 */
CXX_C_API void turbo_transport_destroy(turbo_transport_t *transport);

/**
 * 连接到服务器（协程内调用）
 */
CXX_C_API int turbo_transport_connect(turbo_transport_t *transport);

/**
 * 断开连接
 */
CXX_C_API int turbo_transport_disconnect(turbo_transport_t *transport);

/**
 * 发送数据（协程内调用）
 */
CXX_C_API int turbo_transport_send(turbo_transport_t *transport,
                                   const uint8_t *data,
                                   size_t size);

/**
 * 接收数据（协程内调用，会挂起等待）
 */
CXX_C_API int turbo_transport_recv(turbo_transport_t *transport,
                                   uint8_t **data,
                                   size_t *size);

/**
 * 释放接收到的数据
 */
CXX_C_API void turbo_transport_free_recv(turbo_transport_t *transport, uint8_t *data);

/**
 * 检查连接状态
 */
CXX_C_API int turbo_transport_is_connected(turbo_transport_t *transport);

/**
 * 设置事件回调
 */
CXX_C_API void turbo_transport_set_event_callback(turbo_transport_t *transport,
                                                  turbo_transport_event_cb callback,
                                                  void *user_data);

/**
 * 获取底层 CoroNet socket（如果有）
 */
CXX_C_API struct coro_socket_s *turbo_transport_get_socket(turbo_transport_t *transport);

/**
 * 获取底层 HttpClient（如果有）
 */
CXX_C_API struct http_client_s *turbo_transport_get_http_client(turbo_transport_t *transport);

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
CXX_C_API struct http_response_s *turbo_transport_http_request(
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

CXX_C_API struct http_response_s *turbo_transport_http_upload_stream(
    turbo_transport_t *transport,
    const char *path,
    turbo_transport_read_cb read_cb,
    size_t content_length,
    void *user_data);

/**
 * 流式下载（用于拉流）
 */
typedef void (*turbo_transport_data_cb)(const uint8_t *data, size_t size, void *user_data);

CXX_C_API int turbo_transport_http_download_stream(
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
CXX_C_API int turbo_transport_ws_send_text(turbo_transport_t *transport,
                                           const char *text);

/**
 * 发送 WebSocket 二进制消息
 */
CXX_C_API int turbo_transport_ws_send_binary(turbo_transport_t *transport,
                                             const uint8_t *data,
                                             size_t size);

/**
 * 接收 WebSocket 消息
 */
CXX_C_API int turbo_transport_ws_recv(turbo_transport_t *transport,
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
CXX_C_API int turbo_transport_parse_url(const char *url,
                                        turbo_transport_config_t *config);

/**
 * 获取错误描述
 */
CXX_C_API const char *turbo_transport_get_error(turbo_transport_t *transport);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_TRANSPORT_H */
