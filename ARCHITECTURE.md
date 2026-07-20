# TurboMedia 架构文档

## 概述

TurboMedia 是一个模块化的多媒体处理框架，提供统一的接口用于音视频编解码、容器封装/解封装和流媒体传输。

## 核心架构

```
┌─────────────────────────────────────────────────────────────┐
│                       Application Layer                      │
└─────────────────────────────────────────────────────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
┌───────▼────────┐  ┌──────▼───────┐  ┌───────▼────────┐
│  Codec Layer   │  │ Muxer/Demuxer│  │ Streamer Layer │
│                │  │    Layer     │  │                │
│ • H.264/H.265  │  │ • FLV        │  │ • HLS          │
│ • VP8/VP9      │  │ • MP4/MOV    │  │ • DASH         │
│ • Opus/G.711   │  │ • MKV/WebM   │  │ • RTMP         │
│                │  │ • MPEG-TS/PS │  │ • HTTP-FLV     │
└────────────────┘  └──────────────┘  └────────┬───────┘
                                               │
                                   ┌───────────▼────────────┐
                                   │   Network Transport    │
                                   │                        │
                                   │ • CoroNet (TCP/UDP/WS) │
                                   │ • TurboHTTP (HTTP/S)   │
                                   └────────────────────────┘
```

## 模块说明

### 1. Codec Layer（编解码层）

**位置**: `codec/`

**职责**: 音视频压缩和解压缩

**特点**:
- 统一的 `turbo_codec_ops_t` 接口
- 插件式注册机制
- 支持内置和外部编解码器
- 独立的编码器/解码器实例

**支持的编解码器**:
- **视频**: H.264, H.265, VP8, VP9, AV1 (未来)
- **音频**: Opus, G.711, AAC (未来), MP3 (未来)

**关键接口**:
```c
turbo_codec_t *turbo_codec_create_encoder(const char *name, const void *config);
turbo_codec_t *turbo_codec_create_decoder(const char *name, const void *config);
int turbo_codec_encode(turbo_codec_t *codec, ...);
int turbo_codec_decode(turbo_codec_t *codec, ...);
```

### 2. Muxer Layer（容器封装层）

**位置**: `muxer/`

**职责**: 将编码后的音视频流封装成容器格式

**特点**:
- 统一的 `turbo_muxer_ops_t` 接口
- 支持文件模式和内存模式
- 多轨道支持
- 分片支持（HLS/DASH）

**支持的容器格式**:
- **FLV**: Flash Video（基于 `refer/libflv`）
- **MP4/MOV**: MPEG-4 Part 14（基于 `refer/libmov`）
- **MKV/WebM**: Matroska（基于 `refer/libmkv`）
- **MPEG-TS/PS**: MPEG Transport/Program Stream（基于 `refer/libmpeg`）

**关键接口**:
```c
turbo_muxer_t *turbo_muxer_create(const turbo_muxer_config_t *config);
int turbo_muxer_add_stream(turbo_muxer_t *muxer, const turbo_stream_info_t *info);
int turbo_muxer_write_packet(turbo_muxer_t *muxer, const turbo_muxer_packet_t *packet);
```

### 3. Demuxer Layer（容器解封装层）

**位置**: `demuxer/`

**职责**: 从容器格式中提取音视频流

**特点**:
- 统一的 `turbo_demuxer_ops_t` 接口
- 自动格式探测
- Seek 支持
- 元数据提取

**关键接口**:
```c
turbo_demuxer_t *turbo_demuxer_create(const turbo_demuxer_config_t *config);
int turbo_demuxer_read_packet(turbo_demuxer_t *demuxer, turbo_demuxer_packet_t *packet);
int turbo_demuxer_seek(turbo_demuxer_t *demuxer, int64_t timestamp_ms, int flags);
```

### 4. Streamer Layer（流媒体协议层）

**位置**: `streamer/`

**职责**: 实现流媒体协议（推流/拉流）

**特点**:
- 统一的 `turbo_streamer_ops_t` 接口
- 集成网络传输层
- 自动分片管理（HLS/DASH）
- 事件回调机制

**支持的协议**:
- **HLS**: HTTP Live Streaming（基于 `refer/libhls`）
- **DASH**: MPEG-DASH（基于 `refer/libdash`）
- **RTMP**: Real-Time Messaging Protocol（基于 `refer/librtmp` + CoroNet）
- **HTTP-FLV**: HTTP 传输 FLV 直播流

**关键接口**:
```c
turbo_streamer_t *turbo_streamer_create(const turbo_streamer_config_t *config);
int turbo_streamer_connect(turbo_streamer_t *streamer);
int turbo_streamer_write_packet(turbo_streamer_t *streamer, const turbo_muxer_packet_t *packet);
```

### 5. Network Transport Layer（网络传输层）

**位置**: `network/`

**职责**: 提供统一的网络传输抽象

**特点**:
- 集成 TurboNet::CoroNet（协程网络库）
- 集成 TurboHTTP::HttpClient（HTTP 客户端）
- 支持多种传输协议
- 协程友好的异步 I/O

**支持的传输类型**:
- **TCP**: 原始 TCP 连接
- **TLS**: TLS 加密连接
- **UDP**: UDP 数据报
- **WebSocket**: WebSocket 连接
- **HTTP/HTTPS**: HTTP 请求/响应

**关键接口**:
```c
turbo_transport_t *turbo_transport_create(const turbo_transport_config_t *config);
int turbo_transport_connect(turbo_transport_t *transport);
int turbo_transport_send(turbo_transport_t *transport, const uint8_t *data, size_t size);
int turbo_transport_recv(turbo_transport_t *transport, uint8_t **data, size_t *size);
```

## 数据流示例

### 本地录制流程

```
摄像头/麦克风
    │
    ▼
Capture API
    │
    ▼
原始数据 (YUV420/PCM)
    │
    ▼
Codec Encoder (H.264/Opus)
    │
    ▼
编码数据 (NAL/OpusPacket)
    │
    ▼
Muxer (FLV/MP4)
    │
    ▼
容器文件
```

### HLS 直播推流

```
摄像头/麦克风
    │
    ▼
Capture API
    │
    ▼
原始数据 (YUV420/PCM)
    │
    ▼
Codec Encoder (H.264/AAC)
    │
    ▼
编码数据
    │
    ▼
HLS Streamer
    ├── TS Muxer (MPEG-TS 分片)
    ├── M3U8 Generator (播放列表)
    └── HTTP Transport (上传分片)
            │
            ▼
        CDN/服务器
```

### RTMP 推流

```
编码数据
    │
    ▼
RTMP Streamer
    ├── RTMP Protocol Handler
    └── CoroNet Transport (TCP)
            │
            ▼
        RTMP 服务器
```

### 播放流程

```
网络流/文件
    │
    ▼
Demuxer (自动探测格式)
    │
    ▼
编码数据包
    │
    ▼
Codec Decoder
    │
    ▼
原始数据
    │
    ▼
Playback API (音视频渲染)
```

## 集成外部库

### 从 refer/ 目录导入

TurboMedia 已经包含了以下库（位于 `refer/` 目录）:

- **libflv**: FLV muxer/demuxer
- **libhls**: HLS packager
- **libmov**: MP4/MOV muxer/demuxer
- **libmkv**: MKV muxer/demuxer
- **libmpeg**: MPEG-TS/PS muxer/demuxer
- **librtmp**: RTMP client/server
- **librtp**: RTP/RTCP
- **librtsp**: RTSP client/server

这些库通过 CMake 自动集成。

### 添加新的编解码器

1. 在 `codec/` 目录创建实现文件
2. 实现 `turbo_codec_ops_t` 接口
3. 在 `codec_registry.c` 中注册
4. 更新 `codec/CMakeLists.txt`

示例（添加 AV1）:
```c
// av1_codec.c
const turbo_codec_ops_t turbo_av1_codec_ops = {
    .name = "av1",
    .type = TURBO_CODEC_TYPE_VIDEO,
    .create_encoder = av1_create_encoder,
    .create_decoder = av1_create_decoder,
    // ...
};

// codec_registry.c
#ifdef TURBO_MEDIA_HAS_AV1
    turbo_codec_register(&turbo_av1_codec_ops);
#endif
```

### 集成外部网络库

当前支持:
- **TurboNet::CoroNet**: 协程网络库（TCP/UDP/WebSocket/TLS）
- **TurboHTTP::HttpClient**: HTTP 客户端库

新的网络库可以通过实现 `turbo_transport_ops_t` 接口集成。

## 编译配置

所有功能进入默认构建，不存在功能 CMake 选项。依赖和工具位置由本机
`CMakeUserPresets.json` 提供；项目配置只消费标准 CMake package/target，并在缺失时失败。

### 依赖项

**必需**:
- C11 编译器
- CMake >= 3.20

**必需**:
- libx264/OpenH264 (H.264)
- x265 + libde265 (H.265)
- libopus (Opus)
- libvpx (VP8/VP9)
- TurboNet (网络传输)
- TurboHTTP (HTTP 客户端)

## 线程模型

### 协程模型（推荐）

使用 TurboNet::CoroNet 的协程模型:
- **优点**: 高并发、低开销、代码简洁
- **适用**: 流媒体推拉流、网络传输

```c
coro_context_t *ctx = coro_context_create();
coro_create(ctx, streaming_coroutine, user_data);
coro_context_run(ctx);
```

### 传统线程模型

也支持传统的多线程模型:
- **优点**: 灵活性高
- **适用**: 本地文件处理、离线转码

## 性能考虑

1. **零拷贝**: 尽可能使用引用而非拷贝数据
2. **批处理**: 批量处理多个数据包以减少系统调用
3. **内存池**: 复用缓冲区以减少分配开销
4. **硬件加速**: 支持硬件编解码器（iOS VideoToolbox、Android MediaCodec）

## 安全性

1. **输入验证**: 所有公共 API 验证输入参数
2. **缓冲区保护**: 防止缓冲区溢出
3. **资源限制**: 限制内存使用和并发连接数
4. **TLS 支持**: 网络传输支持 TLS 加密

## 扩展性

TurboMedia 设计为高度可扩展:

1. **插件式架构**: 通过注册表添加新的编解码器/容器/协议
2. **统一接口**: 所有模块使用一致的 ops 表模式
3. **模块化构建**: 可选择性编译需要的功能
4. **跨平台**: 支持 Windows、Linux、macOS、iOS、Android

## 示例代码

参见 `examples/` 目录:
- `streaming_pipeline.c`: 完整的流媒体处理管道
- `codec_g711_smoke.c`: G.711 编解码示例
- `capture_save_image.c`: 摄像头采集示例

## 未来规划

- [ ] WebRTC 支持
- [ ] AV1 编解码器
- [ ] SRT (Secure Reliable Transport)
- [ ] NDI 支持
- [ ] GPU 加速编解码
- [ ] 完善的 Python 绑定

## 贡献指南

1. 遵循现有代码风格
2. 所有新功能需提供示例
3. 更新相关文档
4. 确保跨平台兼容性
5. 添加单元测试

## 许可证

参见项目根目录的 LICENSE 文件。
