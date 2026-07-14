# WebRTC DataChannel 架构设计

## 概述

TurboNet 的 WebRTC DataChannel 实现提供了点对点数据通信，支持多种传输选项。架构采用分层设计，职责清晰分离。

## 层次结构

```
┌─────────────────────────────────────┐
│         应用层                       │
│   (使用 DC API 的用户代码)           │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│      DataChannel API 层             │
│  - 上下文/对端/通道管理              │
│  - 事件/回调分发                     │
│  - DCEP 协议处理                     │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│         SCTP 层                     │
│  - 可靠/不可靠消息传递               │
│  - 流复用                            │
│  - 流量控制                          │
│  (usrsctp 库)                       │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│         DTLS 层                     │
│  - 加密/解密                         │
│  - 证书验证                          │
│  - 握手                              │
│  (OpenSSL)                          │
└──────────────┬──────────────────────┘
               │
       ┌───────┴────────┬─────────┬──────────┐
       │                │         │          │
┌──────▼─────┐  ┌──────▼─────┐  ┌▼─────┐  ┌─▼──────┐
│ UDP        │  │ TCP        │  │ KCP  │  │  ICE   │
│ 传输       │  │ 传输       │  │      │  │ (STUN/ │
│            │  │            │  │      │  │  TURN) │
└────────────┘  └────────────┘  └──────┘  └────────┘
```

## 核心组件

### 1. 上下文 (`turbo_dc_context_t`)

**职责：** 全局配置和资源管理。

**关键数据：**
- DTLS 证书和私钥
- 传输模式 (UDP/TCP/KCP/ICE)
- SCTP/DTLS MTU 设置
- 所有对端列表

**生命周期：**
```c
创建 → 配置 → 管理对端 → 销毁
```

**设计决策：**
- 每个进程一个上下文（SCTP 初始化单例模式）
- 拥有全局 SCTP 资源
- 如未提供，自动生成自签名证书

---

### 2. 对端 (`turbo_dc_peer_t`)

**职责：** 连接到远程对端。

**关键数据：**
```c
struct turbo_dc_peer_s {
    turbo_dc_context_t *ctx;           // 父上下文
    struct dtls_session_s *dtls;       // DTLS 会话
    struct socket *sctp_socket;        // SCTP 套接字 (usrsctp)
    void *transport;                    // CoroNet stream/datagram 或外部传输
    const dc_transport_ops_t *transport_ops;

    turbo_dc_state_t state;            // 连接状态

    // 回调
    turbo_dc_state_cb on_state;
    turbo_dc_channel_cb on_channel;
    turbo_dc_error_cb on_error;

    // 通道管理
    turbo_dc_channel_t *channels[MAX_CHANNELS];
    uint16_t next_stream_id;
};
```

**状态机：**
```
NEW → CONNECTING → CONNECTED → DISCONNECTING → CLOSED
                 ↘            ↗
                   FAILED
```

**传输集成：**
- UDP/TCP：使用 CoroNet `turbo_datagram_t` / `turbo_stream_t`
- ICE：通过 `turbo_dc_peer_set_external_transport()` 挂接外部所有的 datagram 传输
- KCP：当前安装的 CoroNet 包未公开所需传输 API，因此明确拒绝

---

### 3. 通道 (`turbo_dc_channel_t`)

**职责：** 应用层数据流。

**关键数据：**
```c
struct turbo_dc_channel_s {
    turbo_dc_peer_t *peer;             // 父对端
    uint16_t stream_id;                // SCTP 流 ID
    char *label;                       // 通道标签/名称

    turbo_dc_channel_config_t config;  // 可靠性配置
    turbo_dc_channel_state_t state;    // 通道状态

    // 回调
    turbo_dc_open_cb on_open;
    turbo_dc_message_cb on_message;
    turbo_dc_close_cb on_close;
};
```

**DCEP (Data Channel Establishment Protocol):**
- 客户端在偶数流 ID 上发送 DCEP_OPEN
- 服务端在奇数流 ID 上响应
- 协商可靠性参数

**流 ID 分配：**
- 偶数 ID: 客户端发起的通道
- 奇数 ID: 服务端发起的通道
- 避免冲突

---

## 数据流

### 出站（发送）

```
应用
    ↓ turbo_dc_channel_send(data)
通道层
    ↓ 添加 PPID (WebRTC DCEP 协议)
SCTP
    ↓ sctp_sendv() - 分片、排序、可靠性
DTLS
    ↓ dtls_write() - 加密
传输
    ↓ UDP/TCP/KCP/ICE 发送
网络
```

### 入站（接收）

```
网络
    ↓ 套接字读取
传输
    ↓ on_data 回调
DTLS
    ↓ dtls_read() - 解密
SCTP
    ↓ usrsctp_conninput() - 重组
SCTP 回调
    ↓ 解析 PPID，通过 stream_id 查找通道
通道
    ↓ on_message 回调
应用
```

---

## SCTP 集成

### usrsctp 配置

```c
// 每个进程初始化一次
usrsctp_init(0, send_cb, debug_printf);
usrsctp_sysctl_set_sctp_ecn_enable(0);
usrsctp_sysctl_set_sctp_pr_enable(1);  // 部分可靠性
```

### 套接字设置

```c
socket = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, ...);

// 启用 SCTP 事件
usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_EVENT, ...);

// 设置 NODELAY (禁用 Nagle)
usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_NODELAY, ...);
```

### 发送回调

```c
int send_cb(void *addr, void *data, size_t len, ...) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)addr;

    // 传递给 DTLS 加密
    dtls_write(peer->dtls, data, len);

    return 0;
}
```

---

## DTLS 集成

### 会话创建

```c
dtls_session_t *dtls = dtls_session_create(
    cert_pem,
    key_pem,
    is_server,  // 客户端或服务端角色
    on_dtls_data,
    on_dtls_state,
    peer
);
```

### 握手

1. 客户端发送 DTLS ClientHello
2. 服务端响应 ServerHello, Certificate
3. 客户端验证证书（ICE 模式下 SDP 中的指纹）
4. 密钥建立
5. 应用数据可以流动

### 数据回调

```c
void on_dtls_data(dtls_session_t *dtls, const void *data, size_t len, void *ud) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)ud;

    // 解密数据 → SCTP
    usrsctp_conninput(peer, data, len, 0);
}
```

---

## 传输抽象

### 直连传输 (UDP/TCP/KCP)

```c
// 创建异步客户端 - 传输类型由 URL 前缀决定
async_client_t *client = async_client_create(on_event, NULL);

// TCP 连接
async_client_connect(client, "tcp://remote_host:port");

// UDP 连接
async_client_connect(client, "udp://remote_host:port");

// KCP 连接 (可靠 UDP)
async_client_connect(client, "kcp://remote_host:port");

void on_event(async_client_t *client, const async_client_event_t *event, void *ud) {
    if (event->type == ASYNC_CLIENT_EVENT_DATA) {
        // 网络数据 → DTLS
        dtls_feed_data(peer->dtls, event->data, event->length);
    }
}
```

### ICE 传输

```c
// 设置 ICE 代理
turbo_dc_peer_set_ice_agent(peer, ice_agent);

// ICE 代理在数据到达时调用：
void on_ice_data(ice_agent_t *agent, const void *data, size_t len, void *ud) {
    turbo_dc_peer_feed_ice_data(peer, data, len);
}

// DataChannel 通过以下方式写回：
void ice_send_cb(peer, data, len) {
    ice_agent_send(peer->ice_agent, data, len);
}
```

---

## 内存管理

### 分配策略

1. **上下文**: malloc() 一次，手动销毁
2. **对端**: 每个连接 malloc()，引用计数
3. **通道**: 每个通道 malloc()，由对端拥有
4. **缓冲区**: 尽可能零拷贝，SCTP 使用 arena_buffer

### 清理顺序

```c
// 1. 关闭所有通道
for (each channel) {
    turbo_dc_channel_close(channel);
    free(channel);
}

// 2. 关闭 SCTP 套接字
usrsctp_close(peer->sctp_socket);

// 3. 销毁 DTLS 会话
dtls_session_destroy(peer->dtls);

// 4. 关闭并销毁所选传输策略
peer->transport_ops->close(peer);
peer->transport_ops->destroy(peer);

// 5. 释放对端
free(peer);
```

---

## 线程模型

**传输所有权：**
- TCP/UDP 操作通过 `coro_post()` 投递到 context 专属的 CoroNet 线程
- CoroNet 在该 owner 线程驱动 DTLS 定时器与传输回调
- ICE 由外部持有，并通过传输适配器送入 datagram

**多线程支持：**
- 公开生命周期 API 通过 `coro_post()` 同步传输操作
- 回调中不得重入销毁其所属 peer

---

## 错误处理

### 错误传播

```
传输错误
    ↓
DTLS 错误 (连接丢失)
    ↓
对端状态 → FAILED
    ↓
on_state(FAILED) 回调
    ↓
所有通道关闭
```

### 错误码

- DTLS 错误: 证书验证、握手超时
- SCTP 错误: 关联失败、流重置
- 传输错误: 连接被拒绝、超时

---

## 性能优化

### 1. 零拷贝路径

- SCTP 使用 `sctp_sendv()` 与 iovec（无缓冲区拷贝）
- arena_buffer 包装外部数据无需拷贝

### 2. 批处理

- SCTP 将小消息合并到单个 DTLS 数据包
- DTLS 尽可能写入 MTU 大小的数据包

### 3. 定时器效率

- 所有对端共享单个全局 SCTP 定时器
- 10ms 间隔足够重传定时

---

## 限制

1. **每对端最大通道数**: 65535 (SCTP 流限制)
2. **最大消息大小**: 256KB (通过 SCTP 可配置)
3. **MTU**: 1188 字节 (SCTP) / 1280 字节 (DTLS) 默认

---

## 未来改进

- [ ] SCTP PMTUD (路径 MTU 发现)
- [ ] 带宽估计
- [ ] 拥塞控制调优
- [ ] 多宿主 SCTP 支持
