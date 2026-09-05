/**
 * TurboMedia Network Transport - CoroNet Implementation
 *
 * 基于 TurboNet::CoroNet 实现的协程网络传输层
 */
#include "turbo_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CoroNet 头文件 */
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_socket.h"

/* =============================================================================
 * 传输上下文
 * ============================================================================= */

typedef struct turbo_transport_s {
    turbo_transport_config_t config;
    
    /* CoroNet */
    coro_context_t *coro_ctx;
    coro_socket_t *socket;
    int owns_context;           /* 是否拥有 coro_context 的所有权 */
    
    /* 状态 */
    int connected;
    char error_msg[256];
    
    /* 事件回调 */
    turbo_transport_event_cb event_callback;
    void *event_user_data;
    
} turbo_transport_impl_t;

/* =============================================================================
 * 内部函数
 * ============================================================================= */

static void transport_set_error(turbo_transport_impl_t *transport, const char *error) {
    if (error) {
        snprintf(transport->error_msg, sizeof(transport->error_msg), "%s", error);
    } else {
        transport->error_msg[0] = '\0';
    }
}

static void transport_fire_event(turbo_transport_impl_t *transport,
                                 turbo_transport_event_t event,
                                 void *event_data) {
    if (transport->event_callback) {
        transport->event_callback((turbo_transport_t *)transport, 
                                event, event_data, transport->event_user_data);
    }
}

/* =============================================================================
 * 公共 API 实现
 * ============================================================================= */

turbo_transport_t *turbo_transport_create(const turbo_transport_config_t *config) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)calloc(1, sizeof(turbo_transport_impl_t));
    if (!transport) return NULL;
    
    /* 复制配置 */
    transport->config = *config;
    if (config->host) {
        transport->config.host = strdup(config->host);
    }
    if (config->path) {
        transport->config.path = strdup(config->path);
    }
    
    /* 获取或创建 CoroNet 上下文 */
    if (config->coro_ctx) {
        transport->coro_ctx = (coro_context_t *)config->coro_ctx;
        transport->owns_context = 0;
    } else {
        /* 创建新的协程上下文 */
        transport->coro_ctx = coro_context_create(NULL);
        if (!transport->coro_ctx) {
            free((void *)transport->config.host);
            free((void *)transport->config.path);
            free(transport);
            return NULL;
        }
        transport->owns_context = 1;
    }
    
    /* 创建 socket */
    coro_socket_type_t socket_type;
    switch (config->type) {
        case TURBO_TRANSPORT_TCP:
            socket_type = CORO_SOCKET_TCP_V4;
            break;
        case TURBO_TRANSPORT_TLS:
            socket_type = CORO_SOCKET_TLS;
            break;
        case TURBO_TRANSPORT_UDP:
            socket_type = CORO_SOCKET_UDP_V4;
            break;
        case TURBO_TRANSPORT_WEBSOCKET:
            socket_type = CORO_SOCKET_TCP_V4; /* WebSocket 先建立 TCP */
            break;
        default:
            socket_type = CORO_SOCKET_TCP_V4;
    }
    
    transport->socket = coro_socket_create(transport->coro_ctx, socket_type);
    if (!transport->socket) {
        if (transport->owns_context) {
            coro_context_destroy(transport->coro_ctx);
        }
        free((void *)transport->config.host);
        free((void *)transport->config.path);
        free(transport);
        return NULL;
    }
    
    /* 设置超时 */
    if (config->connect_timeout_ms > 0) {
        coro_socket_set_timeout(transport->socket, config->connect_timeout_ms);
    }
    
    return (turbo_transport_t *)transport;
}

void turbo_transport_destroy(turbo_transport_t *transport_ptr) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)transport_ptr;
    if (!transport) return;
    
    /* 断开连接 */
    if (transport->connected) {
        turbo_transport_disconnect(transport_ptr);
    }
    
    /* 清理 socket */
    if (transport->socket) {
        coro_socket_destroy(transport->socket);
    }
    
    /* 清理上下文（如果拥有所有权）*/
    if (transport->owns_context && transport->coro_ctx) {
        coro_context_destroy(transport->coro_ctx);
    }
    
    free((void *)transport->config.host);
    free((void *)transport->config.path);
    free(transport);
}

int turbo_transport_connect(turbo_transport_t *transport_ptr) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)transport_ptr;
    int ret;
    
    if (transport->connected) {
        return 0; /* 已连接 */
    }
    
    /* 根据传输类型连接 */
    switch (transport->config.type) {
        case TURBO_TRANSPORT_TCP:
        case TURBO_TRANSPORT_TLS:
            ret = coro_socket_connect(transport->socket, 
                                     transport->config.host,
                                     transport->config.port);
            break;
            
        case TURBO_TRANSPORT_WEBSOCKET:
            ret = coro_socket_connect_ws(transport->socket,
                                        transport->config.host,
                                        transport->config.port,
                                        transport->config.path ? transport->config.path : "/",
                                        transport->config.use_tls);
            break;
            
        case TURBO_TRANSPORT_UDP:
            /* UDP 是无连接的，直接标记为已连接 */
            ret = 0;
            break;
            
        default:
            transport_set_error(transport, "Unsupported transport type");
            return -1;
    }
    
    if (ret < 0) {
        transport_set_error(transport, "Connection failed");
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_ERROR, transport->error_msg);
        return -1;
    }
    
    transport->connected = 1;
    transport_fire_event(transport, TURBO_TRANSPORT_EVENT_CONNECTED, NULL);
    
    return 0;
}

int turbo_transport_disconnect(turbo_transport_t *transport_ptr) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)transport_ptr;
    
    if (!transport->connected) {
        return 0;
    }
    
    /* CoroNet 的 socket destroy 会自动关闭连接 */
    /* 这里只需要标记状态 */
    transport->connected = 0;
    transport_fire_event(transport, TURBO_TRANSPORT_EVENT_DISCONNECTED, NULL);
    
    return 0;
}

int turbo_transport_send(turbo_transport_t *transport_ptr,
                        const uint8_t *data,
                        size_t size) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)transport_ptr;
    
    if (!transport->connected) {
        transport_set_error(transport, "Not connected");
        return -1;
    }
    
    int ret = coro_socket_send(transport->socket, (const char *)data, size);
    if (ret < 0) {
        transport_set_error(transport, "Send failed");
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_ERROR, transport->error_msg);
        return -1;
    }
    
    return ret;
}

int turbo_transport_recv(turbo_transport_t *transport_ptr,
                        uint8_t **data,
                        size_t *size) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)transport_ptr;
    char *recv_data = NULL;
    size_t recv_size = 0;
    
    if (!transport->connected) {
        transport_set_error(transport, "Not connected");
        return -1;
    }
    
    /* CoroNet recv 会挂起协程直到数据到达 */
    int ret = coro_socket_recv(transport->socket, &recv_data, &recv_size);
    if (ret < 0) {
        transport_set_error(transport, "Receive failed");
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_ERROR, transport->error_msg);
        return -1;
    }
    
    if (ret == 0 && recv_size == 0) {
        /* 连接关闭 */
        transport->connected = 0;
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_DISCONNECTED, NULL);
        return 0;
    }
    
    *data = (uint8_t *)recv_data;
    *size = recv_size;
    
    transport_fire_event(transport, TURBO_TRANSPORT_EVENT_DATA_RECEIVED, recv_data);
    
    return (int)recv_size;
}

void turbo_transport_free_recv(turbo_transport_t *transport_ptr, uint8_t *data) {
    /* CoroNet 的接收缓冲区需要手动释放 */
    coro_socket_free_recv(data);
}

int turbo_transport_is_connected(turbo_transport_t *transport_ptr) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)transport_ptr;
    return transport->connected;
}

void turbo_transport_set_event_callback(turbo_transport_t *transport_ptr,
                                       turbo_transport_event_cb callback,
                                       void *user_data) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)transport_ptr;
    transport->event_callback = callback;
    transport->event_user_data = user_data;
}

coro_socket_t *turbo_transport_get_socket(turbo_transport_t *transport_ptr) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)transport_ptr;
    return transport->socket;
}

const char *turbo_transport_get_error(turbo_transport_t *transport_ptr) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)transport_ptr;
    return transport->error_msg[0] ? transport->error_msg : NULL;
}

/* =============================================================================
 * WebSocket 特定实现
 * ============================================================================= */

int turbo_transport_ws_send_text(turbo_transport_t *transport_ptr, const char *text) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)transport_ptr;

    if (!transport->connected) {
        transport_set_error(transport, "Not connected");
        return -1;
    }

    int ret = coro_socket_send_ws_text(transport->socket, text, strlen(text));
    if (ret < 0) {
        transport_set_error(transport, "WebSocket text send failed");
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_ERROR, transport->error_msg);
        return -1;
    }

    return ret;
}

int turbo_transport_ws_send_binary(turbo_transport_t *transport_ptr,
                                  const uint8_t *data,
                                  size_t size) {
    /* CoroNet 会根据 socket 类型自动使用正确的帧格式 */
    return turbo_transport_send(transport_ptr, data, size);
}

int turbo_transport_ws_recv(turbo_transport_t *transport_ptr,
                           uint8_t **data,
                           size_t *size,
                           int *is_text) {
    turbo_transport_impl_t *transport = (turbo_transport_impl_t *)transport_ptr;
    char *recv_data = NULL;
    size_t recv_size = 0;

    if (!transport->connected) {
        transport_set_error(transport, "Not connected");
        return -1;
    }

    int ret = coro_socket_recv_ws(transport->socket, &recv_data, &recv_size, is_text);
    if (ret < 0) {
        transport_set_error(transport, "WebSocket receive failed");
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_ERROR, transport->error_msg);
        return -1;
    }

    if (ret == 0 && recv_size == 0) {
        transport->connected = 0;
        transport_fire_event(transport, TURBO_TRANSPORT_EVENT_DISCONNECTED, NULL);
        return 0;
    }

    *data = (uint8_t *)recv_data;
    *size = recv_size;

    transport_fire_event(transport, TURBO_TRANSPORT_EVENT_DATA_RECEIVED, recv_data);
    
    return (int)recv_size;
}
