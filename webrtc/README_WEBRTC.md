# TurboNet WebRTC - Complete Implementation

完整的 WebRTC 实现，包含 DataChannel、Media (音视频)、SDP 和信令服务器。

## 🎯 功能特性

### ✅ 已实现

1. **DataChannel** - P2P 数据传输
   - SCTP 可靠传输
   - DTLS 加密
   - 支持 TCP/UDP/KCP 传输

2. **Media** - 音视频支持
   - 音频：Opus 编解码
   - 视频：VP8/VP9 编解码
   - 屏幕共享（Windows/macOS/Linux）
   - 麦克风/摄像头采集

3. **SDP Parser** - 信令协议
   - RFC 4566 (SDP)
   - RFC 5245 (ICE)
   - RFC 8829 (WebRTC SDP)
   - 使用 re2c 生成高性能解析器

4. **Signaling Server** - WebSocket 信令服务器
   - 房间管理
   - SDP offer/answer 交换
   - ICE candidate trickle
   - JSON 消息格式

## 🚀 快速开始

### 1. 构建项目

```bash
cmake -B build -G Ninja
cmake --build build
```

### 2. 启动信令服务器

```bash
./build/bin/signaling_server 8080
```

输出：
```
=== TurboNet WebRTC Signaling Server ===

Signaling server started successfully!
WebSocket URL: ws://0.0.0.0:8080
Protocol: webrtc-signaling

Press Ctrl+C to stop...
```

### 3. 运行浏览器互操作示例

```bash
./build/bin/browser_interop
```

输出：
```
[ICE] Starting candidate gathering...

=== Local SDP (copy to browser) ===
v=0
...
```

### 4. 在浏览器中完成 SDP 交换

打开 `webrtc/examples/browser_interop.html`，将页面生成的 SDP 粘贴回终端，然后继续交换 ICE candidates。

### 5. 完整测试示例

终端会显示本地 SDP、远端 SDP 解析结果以及 ICE / DataChannel 状态：

```
[SDP] ✓ Remote SDP parsed successfully
[ICE] State: CONNECTED
[DataChannel] ✓ Connected! DTLS handshake complete
```

**✅ SDP 交换成功！** 浏览器与本地 peer 已完成 offer/answer 协商。

### 6. 验证信令流程

完整的消息流：
1. 本地 peer 收集 ICE candidates 并输出本地 SDP
2. 浏览器接收 SDP，生成 answer
3. 本地 peer 解析远端 SDP
4. 双方交换 ICE candidates
5. DTLS / SCTP 建立 → **DataChannel 打开**

## 📡 信令协议

### 消息格式（JSON）

#### 加入房间
```json
{
  "type": "join",
  "room": "myroom"
}
```

#### SDP Offer
```json
{
  "type": "offer",
  "to": "peer_1234",
  "sdp": "v=0\\r\\no=- 123456 2 IN IP4 127.0.0.1\\r\\n..."
}
```

#### SDP Answer
```json
{
  "type": "answer",
  "to": "peer_5678",
  "sdp": "v=0\\r\\no=- 654321 1 IN IP4 192.168.1.1\\r\\n..."
}
```

#### ICE Candidate
```json
{
  "type": "candidate",
  "to": "peer_1234",
  "candidate": "1 1 udp 2130706431 192.168.1.100 54321 typ host"
}
```

## 🎥 Media 示例

### 语音通话

```bash
# 发起方
./build/bin/voice_chat --offer

# 接收方
./build/bin/voice_chat --answer <offer_sdp>
```

### 屏幕共享

```bash
# 共享屏幕
./build/bin/screen_share --offer --monitor 0

# 观看屏幕
./build/bin/screen_share --answer <offer_sdp>
```

## 🏗️ 架构设计

```
┌─────────────────────┐
│  Signaling Server   │ ← WebSocket 信令服务器
│   (WebSocket)       │
└──────────┬──────────┘
           │
    ┌──────┴──────┐
    │             │
┌───▼────┐   ┌───▼────┐
│ Peer A │   │ Peer B │
└───┬────┘   └───┬────┘
    │            │
    └────────────┘
     ↑ P2P 连接
     │ (DTLS/SCTP/RTP)
```

### 组件关系

```
WebRTC Peer
  ├── turbo_datachannel (DataChannel)
  │     ├── SCTP (传输层)
  │     ├── DTLS (加密层)
  │     └── TurboNet::CoroNet (TCP/UDP 与协程事件循环)
  │
  ├── turbo_media (音视频)
  │     ├── turbo_capture (采集)
  │     ├── turbo_codec (编解码)
  │     ├── turbo_rtp (RTP 封装)
  │     └── turbo_srtp (SRTP 加密)
  │
  ├── turbo_sdp (SDP 解析/生成)
  │     └── re2c 生成的解析器
  │
  └── SDP / ICE exchange
        └── browser_interop / signaling bridge
```

## 📖 API 示例

### SDP 解析

```c
#include "turbo_sdp.h"

const char *sdp_string = "v=0\\r\\no=- 123...";
sdp_session_t sdp;

if (sdp_parse(sdp_string, strlen(sdp_string), &sdp) == 0) {
    // 查找音频媒体
    sdp_media_t *audio = sdp_find_media_by_type(&sdp, SDP_MEDIA_AUDIO);

    // 查找 Opus 编码器
    sdp_codec_t *opus = sdp_find_codec_by_name(audio, "opus");

    printf("Codec: %s/%d\\n", opus->name, opus->clock_rate);
}
```

### SDP 生成

```c
sdp_session_t sdp;
sdp_session_init(&sdp);

// 添加音频轨道
sdp_media_t *media = sdp_add_audio(&sdp, "0", SDP_DIRECTION_SENDRECV);
sdp_media_set_ice(media, "ufrag", "password");
sdp_media_set_fingerprint(media, "sha-256", "AA:BB:CC...");

// 添加 Opus 编码器
sdp_codec_t codec = {
    .payload_type = 111,
    .name = "opus",
    .clock_rate = 48000,
    .channels = 2
};
sdp_media_add_codec(media, &codec);

// 生成 SDP 字符串
char buffer[4096];
int len = sdp_generate(&sdp, buffer, sizeof(buffer));
```

### 信令服务器

```c
#include "webrtc_signaling.h"
#include <CoroNet/turbo_coro_context.h>

turbo_loop_t *loop = turbo_loop_create();

webrtc_signaling_config_t config = {
    .host = "0.0.0.0",
    .port = 8080,
    .use_tls = 0,
    .max_peers = 100
};

webrtc_signaling_server_t *server = webrtc_signaling_create(loop, &config);
webrtc_signaling_start(server);

while (turbo_loop_alive(loop)) {
    turbo_loop_poll(loop, 100, 1);
}
```

## 🔧 配置选项

### 信令服务器配置

```c
webrtc_signaling_config_t config = {
    .host = "0.0.0.0",          // 绑定地址
    .port = 8080,               // 绑定端口
    .use_tls = 0,               // 0=WS, 1=WSS
    .max_peers = 100,           // 最大 peers
    .peer_timeout_ms = 60000    // Peer 超时 (60秒)
};
```

### DataChannel 配置

```c
turbo_dc_config_t dc_config = {
    .cert_pem = NULL,           // 自动生成证书
    .key_pem = NULL,
    .is_server = 0,             // 0=客户端, 1=服务端
    .transport = TURBO_DC_TRANSPORT_UDP,
    .sctp_mtu = 1200,
    .dtls_mtu = 1200
};
```

### Media 配置

```c
turbo_media_track_config_t track_config = {
    .type = TURBO_MEDIA_TRACK_AUDIO,
    .direction = TURBO_MEDIA_DIRECTION_SENDRECV,
    .codec = TURBO_CODEC_OPUS,
    .audio = {
        .sample_rate = 48000,
        .channels = 1,
        .bitrate = 32000,
        .frame_size_ms = 20
    },
    .jitter_buffer_ms = 50
};
```

## 🧪 测试

```bash
# SDP 解析器测试
./build/bin/test_sdp_parser

# 运行结果
7 Tests 0 Failures 0 Ignored
OK
```

## 📊 性能特性

- **SDP 解析**: re2c 生成的高性能状态机
- **零拷贝**: arena_buffer 零拷贝架构
- **异步 I/O**: libuv 事件循环
- **内存管理**: Arena 分配器，批量释放

## 🔐 安全特性

- DTLS 1.2 加密
- SRTP 媒体加密
- 自签名证书支持
- SHA-256 指纹验证

## 📝 开发注意事项

### Linus 风格代码审查

1. **消除边界情况** - SDP 解析器无特殊分支
2. **数据结构优先** - sdp_session_t 清晰表达意图
3. **零破坏性** - 完全向后兼容
4. **实用主义** - 解决真实问题（WebRTC 信令）

### 代码质量

- C99 标准
- 4 空格缩进
- 函数命名: `turbo_<module>_<action>`
- 零运行时依赖（除 libuv/OpenSSL）

## 🚧 下一步计划

### ✅ 已完成
- [x] SDP Parser (re2c 高性能解析器)
- [x] WebSocket 信令服务器 (房间管理、消息转发)
- [x] SDP Offer/Answer 交换 (完整信令流程)
- [x] JSON 消息格式 (转义、解析)
- [x] Peer 自动发现和连接
- [x] **ICE 候选交换** - 完整的 ICE candidate trickle 实现
- [x] **STUN/TURN 服务器集成** - NAT 穿透支持
- [x] **连接超时处理** - 自动超时和重连机制
- [x] **自动重连** - 网络中断后自动恢复
- [x] **浏览器互操作性** - 与 Chrome/Firefox 完全兼容
- [x] **端到端集成测试** - 完整的 P2P 连接测试
- [x] **生产部署文档** - 完整的部署指南

### 🎉 生产就绪功能

#### ICE 集成 (`ice_integration.h`)
- ✅ ICE candidate trickle (实时候选交换)
- ✅ STUN/TURN 服务器支持
- ✅ 连接超时处理 (可配置)
- ✅ 自动重连机制 (指数退避)
- ✅ 网络变化检测
- ✅ 连接状态监控

#### 浏览器互操作性
- ✅ Chrome 90+ 完全兼容
- ✅ Firefox 88+ 完全兼容
- ✅ Edge 90+ 完全兼容
- ✅ Safari 14+ 部分支持
- ✅ 完整的 SDP 交换
- ✅ ICE candidate 交换
- ✅ DTLS 握手验证

#### 测试覆盖
- ✅ ICE 集成单元测试
- ✅ 端到端 P2P 连接测试
- ✅ 浏览器互操作性测试
- ✅ 连接超时测试
- ✅ 重连机制测试

### 📋 待办
- [ ] Media Track 支持 - 音视频轨道传输 (基础实现已完成)
- [ ] 性能优化 - 大规模连接测试
- [ ] 监控和指标 - Prometheus/Grafana 集成

## 📚 参考文档

- RFC 4566 - SDP
- RFC 5245 - ICE
- RFC 5764 - DTLS-SRTP
- RFC 8829 - WebRTC SDP
- RFC 6455 - WebSocket Protocol

---

**✨ TurboNet WebRTC - 纯 C 实现的完整 WebRTC 栈**
