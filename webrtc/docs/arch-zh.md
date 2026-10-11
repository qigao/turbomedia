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
│  (GmSSL)                            │
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

**DTLS 生产接入与归属：**
DataChannel 已接入基于缓存 GmSSL primitives 的私有 DTLS 1.2 引擎。
context 独占不可变身份；peer 在信令提供角色和 SHA-256 指纹后创建会话。
操作租约保活，现有 mutex/condition 准入串行化引擎调用。握手后更换指纹
返回 -5，重复设置同一指纹仍成功；更换身份须新建关联。

context worker 观察重传、60 秒握手有效期及服务端完成后 120 秒最终 flight
退役期限。有期限时按 10ms 检查，直接 CNet 保留 1ms poll；否则等待条件变量。
ICE 收包保持原线程。worker 持有 peer 租约，但不在 peer-list mutex 或 DTLS
准入内调用 transport、SCTP、用户回调。输入在调用内借用；引擎有界保存
重组、暂存密文、flight、transcript。输出和明文以拥有型 tstr 转移，回调
仅借用，返回后释放并擦除明文。输出/明文队列各最多 256 报文、256 KiB。

只选举一次握手完成者，SCTP 启动后排空应用数据。显式本地关闭沿用不发送
网络回调的行为，注销期限并销毁引擎，立即清除未完成握手秘密。已认证远端
关闭撤销导出、排空已认证尾包并回复后通知 CLOSED。销毁先拒绝新操作并排空
租约，context 最后 join worker、释放身份。回调内重入销毁仍不支持；channel
修改、transport 替换及上层状态访问仍须遵守原 owner 约束。

生产不再使用 BoringSSL BIO；独立互通测试对端保留 packet BIO。生产回归保留
小 MTU、SRTP 一致、最终 flight 丢包恢复、一次 CONNECTED、事务串行化、销毁
等待和定时器回调重入。ready 服务端收到已认证重复 Finished 后重发保留 flight，
不依赖应用写入。第一次已认证重试立即回复，后续每秒最多一次，避免客户端更早
启动的 RTO 被服务端发送时刻的限流压制；可控传输延迟用例覆盖此边界和重放限流。
私有并发测试直接编译生产源码清单，保持 Windows DLL ABI 私有边界；公开集成
测试仍链接正式共享库。#160 缺少历史报文追踪，回归通过不能单独证明旧失败原因。
候选方案和取舍见 [DTLS operation ownership](arch-en.md#dtls-operation-ownership)。

**GmSSL 迁移与兼容边界：**
认证使用 Salts Core 的 SHA-256、HMAC-SHA256、常量时间比较，Base64 复用
libbase64。Salts 保持 2.3.0-rc.10。临时身份以 GmSSL 生成并直接导入 DER/PKCS#8，
保留 v3、CN、365 天有效期和 SHA-256 指纹，私钥临时存储在导入后或失败时擦除。

支持 DTLS 1.2、P-256 ECDHE、P-256 ECDSA/RSA SHA-256 签名、AES-128/256-GCM、
EMS 和四种 SRTP profile。RSA 身份至少 2048 位。不支持恢复、重协商、DTLS
1.0/1.3、其他 EC 身份曲线、有限域 DHE、ChaCha20。仅支持旧算法组合的对端会
明确失败，不自动降级。Finished、CertificateVerify 和指纹通过后才发布 ready/export。

仍保留两个 BoringSSL 兼容入口：`dc_identity.cpp` 用临时 SSL context 将
PKCS#8、SEC1 EC、PKCS#1 RSA PEM 文件规范化；`dc_srtp_legacy.c` 保持已安装
`srtp_derive_keys_from_dtls(void *ssl, ...)` 接收 BoringSSL SSL*。生产 peer
通过 `turbo_dc_peer_get_srtp_keys` 使用 GmSSL 导出，不得将新指针传给旧接口。
证书/密钥路径必须同时提供，不可读、不匹配或不支持的身份直接创建失败，
不回退生成身份。保持原单叶证书语义，未增加链文件或加密密钥密码配置。
配置身份现在也发布指纹。PEM 私钥编码使用固定 16 KiB 拥有型存储，失败时
擦除部分结果。此阶段不能宣称已完全移除 BoringSSL 包。

引擎保持私有且不安装。GmSSL 3.2.0#9 不提供现成 DTLS-SRTP owner；状态机
复用成熟密码算法且无新依赖，代价是维护分片/重传并验证收窄的算法范围。
独立双向互通、四种 profile、混合身份、损坏标签、重放、容量、关闭、超时
测试不能替代本次生产 SCTP、WHIP 和平台回归，不代表浏览器、移动端运行或
TSAN 已验证。回滚须成套恢复旧 owner、证书导入和测试，并保留此前准入、
数据报边界和最终 flight 修复。SRTP/auth 可独立回滚，缓存只读恢复不变。
容量、一手资料及迁移边界见 [GmSSL migration boundary](arch-en.md#gmssl-migration-boundary)。

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
