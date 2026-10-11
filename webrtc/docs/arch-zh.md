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
│  (BoringSSL)                        │
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
    void *transport;                    // CNet stream/datagram 或外部传输
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
- UDP/TCP：使用 CNet `cnet_datagram` / `cnet_client`
- ICE：通过 `turbo_dc_peer_set_external_transport()` 挂接外部所有的 datagram 传输
- KCP：使用 CNet packet session；未配置 packet session 时明确拒绝

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

## PeerConnection 回调生命周期

PeerConnection 拥有 ICE owner、DC peer 和媒体 context。内部 ICE/DC 回调通过
现有 ICE mutex/condition 登记进入和退出；回调借用这些资源，不转移所有权。
销毁先关闭回调入口、请求取消 ICE 操作并 join 协调 worker，再等待已进入的回调
返回。此时三个依赖均保持存活。随后排空媒体传输回调，释放媒体，排空并销毁 DC，
最后 join 并销毁 ICE owner。锁不跨越外部回调、owner 命令或线程 join；没有新增
队列、复制或线程。不返回的回调会阻止销毁完成。

跨线程状态标志使用 C11 原子操作；ICE→DC 绑定在开始 gathering 前安装一次，
重启与销毁时不再改写。原子标志不等于 SDP/track 多字段事务：应用仍须协调信令
和 track 修改，在销毁前停止自身 poll/control/media 操作，且不能从本对象的回调
中销毁它。公开签名与报文语义不变；仅设置 closing 或清空回调指针不能代替排空。
设计与回滚约束见 [PeerConnection callback lifetime](arch-en.md#peerconnection-callback-lifetime)。

CONNECTED 要求 ICE pair 可用、DTLS 已建立、SRTP 与接收 track 初始化成功。
DTLS 标志继续保护已协商的远端指纹；媒体就绪单独记录，仅在初始化成功后发布。
ICE restart 保留成功的媒体就绪状态，DC 失败或关闭则使其失效。ICE 恢复不能把
媒体初始化失败变成 CONNECTED，也不能重启已经建立的 DTLS。相关标志更新与
CONNECTED 条件的状态提交共用 ICE mutex，用户回调仍在锁外执行；不承诺回调
全局排序或任意并发信令/track 修改。

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
- TCP/UDP 操作通过内部有界命令槽投递到 context 专属的 CNet owner 线程
- CNet 在该 owner 线程驱动传输回调，DTLS 定时器由同一 context 协调
- ICE 由外部持有，并通过传输适配器送入 datagram

**多线程支持：**
- 公开生命周期 API 通过内部同步投递协议串行化传输操作
- 回调中不得重入销毁其所属 peer

**DTLS 并发与重传：**
- peer 的操作计数只保护存活期。SSL/BIO、握手完成与停止标志通过独立的
  串行准入保护；SCTP、状态回调和 transport 发送在准入释放后执行，避免重入锁环。
- context worker 观察 BoringSSL 的重传截止时间，因此 ICE context 也创建一个
  worker。ICE 收包和普通调用保持原线程；没有在途握手时 worker 等待条件变量，
  握手期间按 10ms 间隔检查，直接 CNet 仍沿用 1ms poll。协议超时值不变。
- worker 持有 peer 操作租约，但在执行传输或应用回调前释放 peer-list mutex。
  销毁先拒绝新操作并排空租约，再释放 SSL；context 最后停止并 join worker。
  不再依赖原生 timer 自重排与销毁之间的隐含同步。
- 取舍是每个 ICE context 增加一个 worker；没有宣称性能收益。此协议不保证
  任意并发 channel 修改、transport 替换或上层 peer-connection 字段访问安全。
  背景、候选方案与回滚边界见 [DTLS operation ownership](arch-en.md#dtls-operation-ownership)。

**DTLS 数据报边界：**
- BoringSSL 的每次 BIO 写入/读取对应一个数据报。普通 memory BIO 会合并报文，
  固定 2048 字节的输出读取还可能截断 record。输入、输出现均由私有 BIO 适配器
  使用 CSTL deque 保存拥有型 `tstr` 字节报文，不解析或重建 DTLS record。
- 每个 BIO 限制为 256 个报文、256 KiB payload，单报文最多 65535 字节。
  容量或分配失败时不接纳部分报文，明确返回错误；不覆盖、不阻塞、不无限扩容。
  读取消费一个报文，短读取丢弃该报文余下部分，保持数据报语义。
- 所有队列操作受现有 SSL 准入保护。输入复制调用方数据；输出出队转移所有权，
  释放准入后调用 transport，回调返回后释放报文。并发发送可以使完整数据报乱序。
  销毁排空操作租约后，由 SSL 释放 BIO 和残留报文。
- 正式测试覆盖真实证书、小 MTU 双端握手、SRTP 密钥一致性、报文隔离、短读取、
  容量恢复及回调重入。原 #160 日志没有数据报追踪，因此此源码缺陷的修复不能单独
  证明历史失败的原因。取舍与回滚见 [DTLS datagram BIO boundary](arch-en.md#dtls-datagram-bio-boundary)。
- 握手完成后的 `SSL_read` 即使返回 WANT_READ，也可能因重复 Finished 生成末次
  握手报文。因此每次读取都在错误处理后、SSL 准入外发送输出；停止状态不再发送。
  正式回归丢弃服务端第一个末次握手数据报，验证原有重传计时器能够恢复握手、
  双方 SRTP 密钥一致且各只通知一次 CONNECTED，不依赖 SCTP 写入推动进度。

**GmSSL 迁移边界：**认证模块的 SHA-256、HMAC-SHA256、常量时间比较已改用
Salts Core 的 GmSSL 后端，Base64 复用 Core 的 libbase64；认证 target 不再直接链接
OpenSSL::Crypto。Salts 保持 2.3.0-rc.10，token 格式、校验规则与公开接口不变。
临时身份的 P-256 密钥、随机正序列号和 ECDSA/SHA-256 自签名也由已有 GmSSL 包
生成，保留 v3、CN、365 天有效期与大写 SHA-256 指纹格式。Salts Core 计算 DER
摘要；私有 DER 边界隔离两套库的头文件，BoringSSL 仅导入证书/PKCS#8 私钥并
检查匹配。所有返回路径清理生成端密钥，私钥 DER 在导入后或失败时擦除；成功安装
后才发布指纹。配置的 PEM 身份加载行为不变。正式证书测试通过导入端独立验证
自签名、曲线、算法、名称、序列号、有效期与指纹，实际握手由已有 DTLS/SRTP 测试
覆盖。迁移可在私有证书 helper 内回滚，不影响公开接口或信令格式。
中央缓存 GmSSL 3.2.0#9 的 `tls_ctx_init` 只接受 TLS 1.2、TLS 1.3 和 TLCP，
DTLS 常量并不代表已实现 DTLS-SRTP。DataChannel/RTC 的 DTLS 仍需 BoringSSL；
完整替换须先提供经过互通验证的 DTLS-SRTP 后端，不能靠替换库名或禁用 AEAD
完成。证据、取舍、验证与回滚见 [GmSSL migration boundary](arch-en.md#gmssl-migration-boundary)。

正在实现的 `gmssl_dtls.h` 为私有、未安装的 DTLS 1.2 引擎，当前只进入正式互通
测试，不替换生产后端。它复用缓存 GmSSL 的 X.509、ECDH/签名、TLS PRF 和
AES-GCM，支持 EMS 和现有四种 SRTP profile，强制校验对端 SHA-256 指纹。
状态由现有 transport owner 串行推进，输入仅在调用内借用，输出以拥有型 tstr
转移；context 必须晚于所有 session 销毁。重组、flight、transcript 和报文队列均
设硬上限，完成/失败路径清理密钥。具体范围、容量、迁移门槛与未支持算法见英文
文档的私有 DTLS 实现章节；通过独立双向互通及生产回归后再切换，不设自动降级。

libsrtp 的现有 overlay 独立改用 GmSSL AES-CTR、AES-GCM 和 HMAC-SHA1，保留
四种公开 profile、密钥派生、重放保护与 RTP/SRTCP 行为。ICM 保留跨调用的剩余
密钥流；GCM 复制并复用 AAD 存储以覆盖 SRTCP 的临时 trailer，容量受 libsrtp 的
INT_MAX 报文长度域限制，分配失败不接纳部分输入。每个上下文独占其状态，沿用
原有 session 串行调用约束；销毁时擦除密钥和保留存储。正式验证包含库初始化时
的已知向量、双向 RTP/SRTCP、错误密钥和损坏标签拒绝，以及后续合法报文恢复。

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
