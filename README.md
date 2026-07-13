# TurboMedia

**现代化、模块化的多媒体处理框架**

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![C Standard](https://img.shields.io/badge/C-11-blue.svg)]()

TurboMedia 是一个高性能、模块化的多媒体处理框架，提供音视频编解码、容器封装/解封装、流媒体传输等完整解决方案。

## ✨ 核心特性

### 🎬 编解码器（Codec）
- **视频**: H.264/AVC, H.265/HEVC, VP8, VP9
- **音频**: Opus, G.711 (μ-law/A-law)
- **插件式架构**: 轻松集成自定义编解码器
- **硬件加速**: 支持 iOS VideoToolbox、Android MediaCodec

### 📦 容器格式（Muxer/Demuxer）
- **FLV**: Flash Video 封装/解封装
- **MP4/MOV**: MPEG-4 Part 14 容器
- **MKV/WebM**: Matroska 容器
- **MPEG-TS/PS**: MPEG 传输流/程序流
- **自动格式探测**: 智能识别文件格式

### 🌐 流媒体协议（Streamer）
- **HLS**: HTTP Live Streaming（m3u8 + TS/fMP4）
- **DASH**: MPEG-DASH 自适应流
- **RTMP**: Real-Time Messaging Protocol
- **HTTP-FLV**: HTTP 传输 FLV 直播流

### 🔌 网络传输（Transport）
- **CoroNet**: 协程网络库（TCP/UDP/WebSocket/TLS）
- **TurboHTTP**: HTTP/HTTPS 客户端
- **统一接口**: 透明切换不同传输协议

## 🚀 快速开始

### 安装

```bash
git clone https://github.com/turbomedia/turbomedia.git
cd turbomedia
mkdir build && cd build

cmake .. \
  -DTURBO_MEDIA_ENABLE_H264=ON \
  -DTURBO_MEDIA_ENABLE_H265=ON \
  -DTURBO_MEDIA_ENABLE_OPUS=ON \
  -DTURBO_MEDIA_ENABLE_FLV=ON \
  -DTURBO_MEDIA_ENABLE_HLS=ON

cmake --build . --config Release
```

### Hello World 示例

```c
#include <turbo_codec.h>
#include <turbo_muxer.h>

int main() {
    /* 初始化 */
    turbo_codec_registry_init();
    turbo_muxer_registry_init();
    
    /* 创建 H.264 编码器 */
    turbo_video_codec_config_t config = {
        .width = 1920,
        .height = 1080,
        .framerate = 30,
        .bitrate = 5000000
    };
    
    turbo_codec_t *encoder = turbo_codec_create_encoder("h264", &config);
    
    /* 创建 FLV 封装器 */
    turbo_muxer_config_t muxer_config = {
        .format = TURBO_MUXER_FLV,
        .output_path = "output.flv"
    };
    
    turbo_muxer_t *muxer = turbo_muxer_create(&muxer_config);
    
    /* 添加视频流 */
    turbo_stream_info_t stream_info = {
        .type = TURBO_CODEC_TYPE_VIDEO,
        .codec_name = "h264",
        .width = 1920,
        .height = 1080,
        .framerate = 30
    };
    
    int stream_id;
    turbo_muxer_add_stream(muxer, &stream_info, &stream_id);
    turbo_muxer_write_header(muxer);
    
    /* 编码并封装帧 */
    for (int i = 0; i < 300; i++) {
        uint8_t *raw_frame = get_yuv_frame();  /* 获取 YUV 数据 */
        
        uint8_t encoded[100000];
        size_t encoded_size = sizeof(encoded);
        turbo_encoded_frame_t info;
        
        turbo_codec_encode(encoder, raw_frame, yuv_size,
                          encoded, &encoded_size, &info);
        
        turbo_muxer_packet_t packet = {
            .stream_id = stream_id,
            .data = encoded,
            .size = encoded_size,
            .pts = i * 1000000 / 30,
            .is_keyframe = info.is_keyframe
        };
        
        turbo_muxer_write_packet(muxer, &packet);
    }
    
    turbo_muxer_write_trailer(muxer);
    
    /* 清理 */
    turbo_muxer_destroy(muxer);
    turbo_codec_destroy(encoder);
    
    return 0;
}
```

## 📚 文档

- [📖 快速入门指南](QUICKSTART.md)
- [🏗️ 架构设计文档](ARCHITECTURE.md)
- [💡 示例代码](examples/)
- [🔧 API 参考](docs/API.md)

## 🏛️ 架构设计

```
┌─────────────────────────────────────────────────────┐
│                  Application Layer                   │
└─────────────────────────────────────────────────────┘
         │                  │                  │
    ┌────▼────┐      ┌─────▼──────┐     ┌────▼─────┐
    │ Codec   │      │   Muxer/   │     │ Streamer │
    │         │      │  Demuxer   │     │          │
    │ H.264   │      │   FLV      │     │   HLS    │
    │ H.265   │      │   MP4      │     │  DASH    │
    │ Opus    │      │   MKV      │     │  RTMP    │
    │ VP8/9   │      │ MPEG-TS    │     │          │
    └─────────┘      └────────────┘     └────┬─────┘
                                              │
                                    ┌─────────▼──────────┐
                                    │  Network Transport │
                                    │                    │
                                    │  CoroNet (协程)     │
                                    │  TurboHTTP (HTTP)  │
                                    └────────────────────┘
```

## 🎯 使用场景

### 🎥 视频录制
```c
摄像头 → Codec Encoder → Muxer → 文件
```

### 📡 直播推流
```c
摄像头 → Codec Encoder → HLS Streamer → CDN
摄像头 → Codec Encoder → RTMP Streamer → 直播服务器
```

### 📺 视频播放
```c
文件 → Demuxer → Codec Decoder → 渲染
网络流 → Streamer → Demuxer → Decoder → 渲染
```

### 🔄 视频转码
```c
输入文件 → Demuxer → Decoder → Encoder → Muxer → 输出文件
```

## 🛠️ CMake 配置选项

### 编解码器
```cmake
-DTURBO_MEDIA_ENABLE_H264=ON      # H.264 编解码
-DTURBO_MEDIA_ENABLE_H265=ON      # H.265 编解码
-DTURBO_MEDIA_ENABLE_OPUS=ON      # Opus 音频编解码
-DTURBO_MEDIA_ENABLE_VPX=ON       # VP8/VP9 编解码
```

### 容器格式
```cmake
-DTURBO_MEDIA_ENABLE_FLV=ON       # FLV 容器
-DTURBO_MEDIA_ENABLE_MP4=ON       # MP4 容器
-DTURBO_MEDIA_ENABLE_MKV=ON       # MKV/WebM 容器
-DTURBO_MEDIA_ENABLE_MPEG=ON      # MPEG-TS/PS 容器
```

### 流媒体协议
```cmake
-DTURBO_MEDIA_ENABLE_HLS=ON       # HLS 协议
-DTURBO_MEDIA_ENABLE_DASH=ON      # DASH 协议
-DTURBO_MEDIA_ENABLE_RTMP=ON      # RTMP 协议
-DTURBO_MEDIA_ENABLE_HTTP_FLV=ON  # HTTP-FLV 协议
```

### 其他选项
```cmake
-DBUILD_EXAMPLES=ON               # 编译示例程序
-DBUILD_TESTS=ON                  # 编译测试
-DBUILD_MOBILE=ON                 # 移动平台支持
```

## 🔗 依赖项

### 必需
- CMake >= 3.20
- C11 编译器（GCC/Clang/MSVC）

### 可选（根据启用的功能）

#### 编解码器
- **H.264**: libx264 或 OpenH264
- **H.265**: x265 + libde265
- **Opus**: libopus
- **VP8/VP9**: libvpx

#### 网络传输
- **TurboNet**: 协程网络库（用于 RTMP、WebSocket）
- **TurboHTTP**: HTTP 客户端（用于 HLS、DASH）

#### 其他
- **FFmpeg**: 可选的通用播放器支持

## 🌍 平台支持

| 平台 | 状态 | 备注 |
|------|------|------|
| Linux | ✅ | 完全支持 |
| Windows | ✅ | 完全支持 |
| macOS | ✅ | 完全支持 |
| iOS | ✅ | 支持硬件编解码 |
| Android | ✅ | 支持硬件编解码 |
| WebAssembly | 🚧 | 计划中 |

## 📊 性能指标

| 场景 | 性能 | 配置 |
|------|------|------|
| H.264 编码 (1080p@30fps) | ~200 fps | Intel i7-10700K |
| H.265 编码 (1080p@30fps) | ~120 fps | Intel i7-10700K |
| FLV 封装 | ~5000 fps | SSD 存储 |
| HLS 分片 | ~2000 fps | 6秒分片 |
| RTMP 推流 | ~1000 fps | 5Mbps 比特率 |

*性能数据仅供参考，实际性能取决于硬件配置和编码参数*

## 🤝 贡献指南

我们欢迎所有形式的贡献！

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 开启 Pull Request

### 贡献规范
- 遵循现有代码风格
- 添加适当的注释和文档
- 确保跨平台兼容性
- 编写单元测试
- 更新相关文档

## 📋 路线图

### v1.0 (当前)
- [x] 基础编解码器（H.264/Opus/G.711）
- [x] FLV 容器支持
- [x] HLS 流媒体协议
- [x] CoroNet 网络集成

### v1.1 (计划中)
- [ ] MP4 容器完整实现
- [ ] DASH 协议支持
- [ ] H.265 编解码优化
- [ ] WebRTC 支持

### v2.0 (未来)
- [ ] AV1 编解码器
- [ ] GPU 加速编解码
- [ ] 完善的 Python 绑定
- [ ] SRT (Secure Reliable Transport)

## 🐛 问题反馈

遇到问题？请通过以下方式反馈：
- [GitHub Issues](https://github.com/turbomedia/turbomedia/issues)
- [讨论区](https://github.com/turbomedia/turbomedia/discussions)

## 📄 许可证

本项目采用 MIT 许可证 - 详见 [LICENSE](LICENSE) 文件

## 🙏 致谢

TurboMedia 使用或参考了以下优秀的开源项目：

- [media-server](https://github.com/ireader/media-server) - FLV/HLS/RTMP/MPEG 实现参考
- [x264](https://www.videolan.org/developers/x264.html) - H.264 编码器
- [x265](https://www.videolan.org/developers/x265.html) - H.265 编码器
- [libopus](https://opus-codec.org/) - Opus 音频编解码器
- [libvpx](https://www.webmproject.org/) - VP8/VP9 视频编解码器
- [TurboNet](https://github.com/your/turbonet) - 协程网络库
- [TurboHTTP](https://github.com/your/turbohttp) - HTTP 客户端库

## 📞 联系方式

- 邮箱: support@turbomedia.io
- 网站: https://turbomedia.io
- Twitter: [@TurboMedia](https://twitter.com/turbomedia)

---

<p align="center">
  使用 ❤️ 和 ☕ 开发 by TurboMedia Team
</p>
