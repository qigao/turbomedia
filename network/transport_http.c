/**
 * TurboMedia Network Transport - HTTP Implementation
 *
 * 基于 TurboHTTP::HttpClient 实现的 HTTP/HTTPS 传输层
 */
#include "turbo_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TurboHTTP */
#include "http_client.h"

/* =============================================================================
 * HTTP 传输上下文
 * ============================================================================= */

typedef struct {
    turbo_transport_config_t config;
    
    /* HttpClient */
    http_client_t *http_client;
    int owns_client;
    
    /* 状态 */
    int connected;
    char error_msg[256];
    char *base_url;
    
    /* 流式接收 */
    uint8_t *stream_buffer;
    size_t stream_buffer_size;
    size_t stream_buffer_capacity;
    
} http_transport_impl_t;

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void http_transport_set_error(http_transport_impl_t *transport, const char *error) {
    if (error) {
        snprintf(transport->error_msg, sizeof(transport->error_msg), "%s", error);
    } else {
        transport->error_msg[0] = '\0';
    }
}

static char *build_base_url(const char *host, int port, int use_tls) {
    char *base_url = (char *)malloc(256);
    if (!base_url) return NULL;
    
    snprintf(base_url, 256, "%s://%s:%d",
             use_tls ? "https" : "http",
             host, port);
    
    return base_url;
}

/* =============================================================================
 * HTTP 特定 API 实现
 * ============================================================================= */

http_response_t *turbo_transport_http_request(turbo_transport_t *transport_ptr,
                                              turbo_http_method_t method,
                                              const char *path,
                                              const uint8_t *body,
                                              size_t body_size,
                                              const char **headers,
                                              int header_count) {
    http_transport_impl_t *transport = (http_transport_impl_t *)transport_ptr;
    
    if (!transport->http_client) {
        http_transport_set_error(transport, "HTTP client not initialized");
        return NULL;
    }
    
    /* 构建完整 URL */
    char full_url[1024];
    snprintf(full_url, sizeof(full_url), "%s%s", transport->base_url, path ? path : "/");
    
    /* 调用 HttpClient */
    http_response_t *response = NULL;
    
    switch (method) {
        case TURBO_HTTP_GET:
            response = http_get(transport->http_client, full_url);
            break;
            
        case TURBO_HTTP_POST:
            response = http_post(transport->http_client, full_url, 
                               (const char *)body, body_size);
            break;
            
        case TURBO_HTTP_PUT:
            response = http_put(transport->http_client, full_url,
                              (const char *)body, body_size);
            break;
            
        case TURBO_HTTP_DELETE:
            response = http_del(transport->http_client, full_url);
            break;
            
        case TURBO_HTTP_HEAD:
            response = http_head(transport->http_client, full_url);
            break;
            
        default:
            http_transport_set_error(transport, "Unsupported HTTP method");
            return NULL;
    }
    
    if (!response) {
        http_transport_set_error(transport, "HTTP request failed");
    }
    
    return response;
}

http_response_t *turbo_transport_http_upload_stream(turbo_transport_t *transport_ptr,
                                                    const char *path,
                                                    turbo_transport_read_cb read_cb,
                                                    size_t content_length,
                                                    void *user_data) {
    http_transport_impl_t *transport = (http_transport_impl_t *)transport_ptr;
    
    if (!transport->http_client || !read_cb) {
        return NULL;
    }
    
    char full_url[1024];
    snprintf(full_url, sizeof(full_url), "%s%s", transport->base_url, path ? path : "/");
    
    /* 使用 TurboHTTP 的流式上传 */
    return http_post_stream(transport->http_client, full_url,
                           (http_data_read_cb)read_cb,
                           content_length, user_data);
}

int turbo_transport_http_download_stream(turbo_transport_t *transport_ptr,
                                        const char *path,
                                        turbo_transport_data_cb data_cb,
                                        void *user_data) {
    http_transport_impl_t *transport = (http_transport_impl_t *)transport_ptr;
    
    if (!transport->http_client || !data_cb) {
        return -1;
    }
    
    char full_url[1024];
    snprintf(full_url, sizeof(full_url), "%s%s", transport->base_url, path ? path : "/");
    
    /* 使用 TurboHTTP 的流式下载 */
    http_response_t *response = http_receive_stream_get(transport->http_client, full_url,
                                                       (http_data_cb)data_cb, user_data);
    
    if (response) {
        int status = response->status_code;
        http_response_free(response);
        return (status >= 200 && status < 300) ? 0 : -1;
    }
    
    return -1;
}

/* =============================================================================
 * 通用传输 API 实现
 * ============================================================================= */

turbo_transport_t *turbo_transport_create_http(const turbo_transport_config_t *config) {
    http_transport_impl_t *transport = (http_transport_impl_t *)calloc(1, 
                                                sizeof(http_transport_impl_t));
    if (!transport) return NULL;
    
    transport->config = *config;
    if (config->host) {
        transport->config.host = strdup(config->host);
    }
    if (config->path) {
        transport->config.path = strdup(config->path);
    }
    
    /* 构建 base URL */
    transport->base_url = build_base_url(config->host, 
                                        config->port > 0 ? config->port : 
                                        (config->use_tls ? 443 : 80),
                                        config->use_tls);
    if (!transport->base_url) {
        free((void *)transport->config.host);
        free((void *)transport->config.path);
        free(transport);
        return NULL;
    }
    
    /* 获取或创建 HttpClient */
    if (config->http_client) {
        transport->http_client = (http_client_t *)config->http_client;
        transport->owns_client = 0;
    } else {
        transport->http_client = http_client_create(transport->base_url);
        if (!transport->http_client) {
            free(transport->base_url);
            free((void *)transport->config.host);
            free((void *)transport->config.path);
            free(transport);
            return NULL;
        }
        transport->owns_client = 1;
        
        /* 配置 HttpClient */
        if (config->connect_timeout_ms > 0) {
            http_client_set_connect_timeout(transport->http_client, 
                                           config->connect_timeout_ms);
        }
        if (config->read_timeout_ms > 0) {
            http_client_set_read_timeout(transport->http_client,
                                        config->read_timeout_ms);
        }
        if (config->user_agent) {
            http_client_set_user_agent(transport->http_client, config->user_agent);
        }
        if (config->auth_token) {
            http_client_set_bearer_token(transport->http_client, config->auth_token);
        }
    }
    
    return (turbo_transport_t *)transport;
}

void turbo_transport_destroy_http(turbo_transport_t *transport_ptr) {
    http_transport_impl_t *transport = (http_transport_impl_t *)transport_ptr;
    if (!transport) return;
    
    if (transport->owns_client && transport->http_client) {
        http_client_destroy(transport->http_client);
    }
    
    free(transport->base_url);
    free(transport->stream_buffer);
    free((void *)transport->config.host);
    free((void *)transport->config.path);
    free(transport);
}

int turbo_transport_connect_http(turbo_transport_t *transport_ptr) {
    http_transport_impl_t *transport = (http_transport_impl_t *)transport_ptr;
    
    /* HTTP 是无状态的，不需要持久连接 */
    /* 这里只是标记为"已连接"状态 */
    transport->connected = 1;
    
    return 0;
}

http_client_t *turbo_transport_get_http_client(turbo_transport_t *transport_ptr) {
    http_transport_impl_t *transport = (http_transport_impl_t *)transport_ptr;
    return transport ? transport->http_client : NULL;
}

const char *turbo_transport_get_error_http(turbo_transport_t *transport_ptr) {
    http_transport_impl_t *transport = (http_transport_impl_t *)transport_ptr;
    return transport->error_msg[0] ? transport->error_msg : NULL;
}
