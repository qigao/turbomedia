# WebRTC DataChannel API 参考

## 目录

- [上下文 API](#上下文-api)
- [对端 API](#对端-api)
- [通道 API](#通道-api)
- [类型和枚举](#类型和枚举)
- [回调函数](#回调函数)

---

## 上下文 API

### turbo_dc_context_create

创建 DataChannel 上下文。必须在创建对端之前调用。

```c
turbo_dc_context_t *turbo_dc_context_create(const turbo_dc_config_t *config);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `config` | `turbo_dc_config_t *` | 配置选项 |

**配置结构体：**
```c
typedef struct {
    const char *cert_pem;         // PEM 证书（NULL 自动生成）
    const char *key_pem;          // PEM 私钥（NULL 自动生成）
    int is_server;                // 1 = 服务端/应答方，0 = 客户端/发起方
    turbo_dc_transport_t transport; // UDP、TCP、KCP 或 ICE
    uint16_t sctp_mtu;            // SCTP MTU（0 = 默认 1188）
    uint16_t dtls_mtu;            // DTLS MTU（0 = 默认 1280）
} turbo_dc_config_t;
```

**返回值：**
- `turbo_dc_context_t *` - 上下文句柄，失败返回 `NULL`

**示例：**
```c
turbo_dc_config_t config = {
    .is_server = 0,
    .transport = TURBO_DC_TRANSPORT_UDP,
    .cert_pem = NULL,  // 自动生成自签名证书
    .key_pem = NULL
};

turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
if (!ctx) {
    fprintf(stderr, "创建上下文失败\n");
    return 1;
}
```

---

### turbo_dc_context_destroy

销毁上下文及所有关联的对端。

```c
void turbo_dc_context_destroy(turbo_dc_context_t *ctx);
```

---

### turbo_dc_global_cleanup

强制全局 SCTP 清理。通常清理是自动的。

```c
void turbo_dc_global_cleanup(void);
```

---

## 对端 API

### turbo_dc_peer_create

创建对端连接。

```c
turbo_dc_peer_t *turbo_dc_peer_create(
    turbo_dc_context_t *ctx,
    const char *remote_host,
    uint16_t remote_port,
    void *user_data
);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `ctx` | `turbo_dc_context_t *` | 上下文句柄 |
| `remote_host` | `const char *` | 远程主机（服务端为 NULL） |
| `remote_port` | `uint16_t` | 远程端口（服务端监听端口为 0） |
| `user_data` | `void *` | 回调的用户数据 |

**返回值：**
- `turbo_dc_peer_t *` - 对端句柄，失败返回 `NULL`

---

### turbo_dc_peer_on_state

设置状态变更回调。

```c
void turbo_dc_peer_on_state(turbo_dc_peer_t *peer, turbo_dc_state_cb cb);
```

---

### turbo_dc_peer_on_channel

设置收到新通道回调。

```c
void turbo_dc_peer_on_channel(turbo_dc_peer_t *peer, turbo_dc_channel_cb cb);
```

---

### turbo_dc_peer_on_error

设置错误回调。

```c
void turbo_dc_peer_on_error(turbo_dc_peer_t *peer, turbo_dc_error_cb cb);
```

---

### turbo_dc_peer_set_ice_agent

设置 ICE 代理用于 NAT 穿透（仅 ICE 传输模式）。

```c
int turbo_dc_peer_set_ice_agent(
    turbo_dc_peer_t *peer,
    struct turbo_ice_agent_s *ice_agent
);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `peer` | `turbo_dc_peer_t *` | 对端句柄 |
| `ice_agent` | `turbo_ice_agent_s *` | 已连接的 ICE 代理 |

**返回值：**
- `0` - 成功
- `-1` - 参数无效
- `-2` - 传输已设置

---

### turbo_dc_peer_feed_ice_data

将 ICE 代理收到的数据传递给 DataChannel。

```c
void turbo_dc_peer_feed_ice_data(
    turbo_dc_peer_t *peer,
    const void *data,
    size_t len
);
```

在 ICE 代理的 `on_data` 回调中调用此函数。

---

### turbo_dc_peer_connect

开始连接（客户端）或监听（服务端）。

```c
int turbo_dc_peer_connect(turbo_dc_peer_t *peer);
```

**返回值：**
- `0` - 成功
- `-1` - 错误

---

### turbo_dc_peer_get_state

获取当前连接状态。

```c
turbo_dc_state_t turbo_dc_peer_get_state(turbo_dc_peer_t *peer);
```

---

### turbo_dc_peer_close

关闭对端连接。

```c
void turbo_dc_peer_close(turbo_dc_peer_t *peer);
```

---

### turbo_dc_peer_destroy

销毁对端并释放资源。

```c
void turbo_dc_peer_destroy(turbo_dc_peer_t *peer);
```

---

## 通道 API

### turbo_dc_channel_create

创建数据通道。

```c
turbo_dc_channel_t *turbo_dc_channel_create(
    turbo_dc_peer_t *peer,
    const char *label,
    const turbo_dc_channel_config_t *config
);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `peer` | `turbo_dc_peer_t *` | 对端句柄 |
| `label` | `const char *` | 通道标签/名称 |
| `config` | `turbo_dc_channel_config_t *` | 配置（NULL 使用默认） |

**通道配置：**
```c
typedef struct {
    int ordered;          // 1 = 有序传递（默认）
    int max_retransmits;  // 最大重传次数（0 = 无限）
    int max_lifetime_ms;  // 最大数据包生存时间 ms（0 = 无限）
    const char *protocol; // 可选子协议
} turbo_dc_channel_config_t;
```

**返回值：**
- `turbo_dc_channel_t *` - 通道句柄，失败返回 `NULL`

---

### turbo_dc_channel_open

打开通道（发送 DCEP OPEN 消息）。

```c
int turbo_dc_channel_open(turbo_dc_channel_t *channel);
```

**返回值：**
- `0` - 成功
- `-1` - 错误（对端未连接）

---

### turbo_dc_channel_on_open

设置通道打开回调。

```c
void turbo_dc_channel_on_open(turbo_dc_channel_t *channel, turbo_dc_open_cb cb);
```

---

### turbo_dc_channel_on_message

设置收到消息回调。

```c
void turbo_dc_channel_on_message(turbo_dc_channel_t *channel, turbo_dc_message_cb cb);
```

---

### turbo_dc_channel_on_close

设置通道关闭回调。

```c
void turbo_dc_channel_on_close(turbo_dc_channel_t *channel, turbo_dc_close_cb cb);
```

---

### turbo_dc_channel_send

在通道上发送数据。

```c
int turbo_dc_channel_send(
    turbo_dc_channel_t *channel,
    const void *data,
    size_t len,
    int is_binary
);
```

**参数：**

| 名称 | 类型 | 说明 |
|------|------|------|
| `channel` | `turbo_dc_channel_t *` | 通道句柄 |
| `data` | `const void *` | 要发送的数据 |
| `len` | `size_t` | 数据长度 |
| `is_binary` | `int` | 1 = 二进制，0 = 文本 |

**返回值：**
- `0` - 成功
- `-1` - 错误

---

### turbo_dc_channel_get_label

获取通道标签。

```c
const char *turbo_dc_channel_get_label(turbo_dc_channel_t *channel);
```

---

### turbo_dc_channel_get_id

获取通道 ID。

```c
uint16_t turbo_dc_channel_get_id(turbo_dc_channel_t *channel);
```

---

### turbo_dc_channel_is_open

检查通道是否已打开。

```c
int turbo_dc_channel_is_open(turbo_dc_channel_t *channel);
```

---

### turbo_dc_channel_buffered_amount

获取缓冲区大小（等待发送的字节数）。

```c
size_t turbo_dc_channel_buffered_amount(turbo_dc_channel_t *channel);
```

---

### turbo_dc_channel_close

关闭通道。

```c
void turbo_dc_channel_close(turbo_dc_channel_t *channel);
```

---

## 工具函数

### turbo_dc_default_channel_config

获取默认通道配置。

```c
turbo_dc_channel_config_t turbo_dc_default_channel_config(void);
```

**返回值：**
```c
{
    .ordered = 1,
    .max_retransmits = 0,
    .max_lifetime_ms = 0,
    .protocol = NULL
}
```

---

### turbo_dc_handle_timers

处理 SCTP 内部定时器。**必须周期性调用（约 10ms）。**

```c
void turbo_dc_handle_timers(void);
```

---

## 类型和枚举

### turbo_dc_transport_t

```c
typedef enum {
    TURBO_DC_TRANSPORT_UDP = 0,   // 直连 UDP（默认）
    TURBO_DC_TRANSPORT_TCP,       // 直连 TCP
    TURBO_DC_TRANSPORT_KCP,       // 可靠 UDP
    TURBO_DC_TRANSPORT_ICE        // NAT 穿透
} turbo_dc_transport_t;
```

### turbo_dc_state_t

```c
typedef enum {
    TURBO_DC_STATE_NEW = 0,       // 新建
    TURBO_DC_STATE_CONNECTING,    // 连接中
    TURBO_DC_STATE_CONNECTED,     // 已连接
    TURBO_DC_STATE_DISCONNECTING, // 断开中
    TURBO_DC_STATE_CLOSED,        // 已关闭
    TURBO_DC_STATE_FAILED         // 失败
} turbo_dc_state_t;
```

---

## 回调函数

### turbo_dc_state_cb

```c
typedef void (*turbo_dc_state_cb)(
    turbo_dc_peer_t *peer,
    turbo_dc_state_t old_state,
    turbo_dc_state_t new_state,
    void *user_data
);
```

### turbo_dc_channel_cb

```c
typedef void (*turbo_dc_channel_cb)(
    turbo_dc_peer_t *peer,
    turbo_dc_channel_t *channel,
    void *user_data
);
```

### turbo_dc_open_cb

```c
typedef void (*turbo_dc_open_cb)(
    turbo_dc_channel_t *channel,
    void *user_data
);
```

### turbo_dc_message_cb

```c
typedef void (*turbo_dc_message_cb)(
    turbo_dc_channel_t *channel,
    const void *data,
    size_t len,
    int is_binary,
    void *user_data
);
```

### turbo_dc_close_cb

```c
typedef void (*turbo_dc_close_cb)(
    turbo_dc_channel_t *channel,
    void *user_data
);
```

### turbo_dc_error_cb

```c
typedef void (*turbo_dc_error_cb)(
    turbo_dc_peer_t *peer,
    int error_code,
    const char *error_msg,
    void *user_data
);
```
