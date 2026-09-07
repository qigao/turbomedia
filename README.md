# TurboMedia

TurboMedia 是一个 C11 多媒体框架，覆盖编解码、封装/解封装、流媒体协议、
FFmpeg 图式流水线以及 WebRTC/RTC 媒体处理。项目默认 fail fast：必需依赖、
非法配置和不受支持的执行路径都会明确失败，不做静默降级。

## 当前能力

| 模块 | 已实现能力 | 主要目标 |
| --- | --- | --- |
| Codec | H.264、H.265、VP8、VP9、Opus、G.711 μ-law/A-law | `TurboMedia::Codec` |
| Muxer/Demuxer | FLV、MP4/fMP4、MKV/WebM、MPEG-TS/PS | `TurboMedia::Muxer`、`TurboMedia::Demuxer` |
| Streamer | HLS、DASH、RTMP、HTTP-FLV | `TurboMedia::Streamer` |
| Pipeline | YAML 图校验、FFmpeg File/HLS/RTSP 输入、转封装/转码/filter、Runtime/RTP relay 与转码 | `TurboMedia::Pipeline` |
| Playback/Capture | FFmpeg 解码与 SaltsUtils 设备适配 | `TurboMedia::Player`、`Salts::Playback`、`Salts::Capture` |
| WebRTC/RTC | SDP、DataChannel、ICE、信令、RTP/RTCP、SRTP、jitter、NACK、TWCC、simulcast | `TurboMedia::WebRTC`、`TurboMedia::RTC` |

`common/codec_helpers` 继续服务原有 muxer、demuxer 和 streamer 的码流转换；
一般文件或网络源之间的格式、编解码与传输转换由 `TurboMedia::Pipeline`
通过 FFmpeg 图执行，两套状态不会混在同一处理分支。

## 构建

构建必须通过 `TURBO_MEDIA_PRODUCT` 显式选择产品，不存在自动探测或完整包
fallback。现有 Windows/Linux user preset 选择 `SERVER`；Android preset 固定选择
`CLIENT`。Windows Server 开发构建：

```powershell
cmake --preset win-dev-user
cmake --build --preset win-dev-user
ctest --preset win-dev-user --output-on-failure
```

发布构建使用对应的 `win-release-user` preset。Linux 可使用
`linux-dev-user` 或 `linux-release-user`。

Windows Client 可复用同一环境并使用独立 build/install prefix：

```powershell
cmake --preset win-dev-user -DTURBO_MEDIA_PRODUCT=CLIENT -B build/Msvc-client `
  -DCMAKE_INSTALL_PREFIX=build/install-client
cmake --build build/Msvc-client
ctest --test-dir build/Msvc-client --output-on-failure
```

Client 只要求 shared media/RTC 依赖以及 SaltsUtils 的 `Capture`、`Playback`；
Server 才查找 RulesForge、TurboDB 和 PostgreSQL-only Orm，并且不查找设备
Capture/Playback。完整 target/platform 矩阵见
[Client/Server 产品拆分](docs/design/client-server-product-profiles.md)。主要共享依赖包括 FFmpeg
（含 `openh264`、`opus`、`xml2` feature）、OpenH264、x265、libde265、
libvpx、Opus、Salts、SaltsUtils、SaltsNet、CHTTP、CNet、libSRTP 和 usrsctp。
WebRTC PeerConnection、ICE、DTLS-SRTP 与 DataChannel 由仓库内 TurboMedia 与
SaltsNet 模块实现；安全传输强制使用 BoringSSL。

Linux Client 桌面 Capture 由 `Salts::Capture` 提供；所选 SaltsUtils 安装 profile
必须已启用 Capture。从源码构建该 profile 时需要 `pkg-config`、
`libpipewire-0.3-dev`、`libx11-dev` 和 `libxext-dev`。

## FFmpeg 图式流水线

流水线使用严格 YAML 配置，必须声明：

```yaml
api_version: turbo.media.pipeline/v1
```

构建后可直接运行：

```powershell
.\build\Msvc\bin\turbo_pipeline_run.exe .\examples\ffmpeg_pipeline.yml
.\build\Msvc\bin\turbo_pipeline_run.exe --stop-after 10s .\examples\public_hls_to_matroska.yml
```

可用节点、拓扑约束、状态所有权、背压和 RTC/RTP 图说明见
[Pipeline 设计与使用指南](pipeline/README.md)。仓库还提供：

- [HTTPS MP4 → MPEG-TS](examples/public_https_to_mpegts.yml)
- [HLS → Matroska](examples/public_hls_to_matroska.yml)
- [DASH → Matroska](examples/public_dash_to_matroska.yml)
- [Runtime RTP relay](examples/runtime_rtp_relay.yml)
- [Runtime RTP transcode](examples/runtime_rtp_transcode.yml)

公开网络示例只用于显式 smoke 测试，不进入默认 CTest，也没有备用 URL。

## WebRTC 信令 TOML

信令服务示例配置位于
[signaling.toml.example](webrtc/apps/signaling_server/config/signaling.toml.example)。
配置优先级为：

```text
内置默认值 < TOML 文件 < 命令行参数
```

加载器执行严格 schema 和类型校验：

- 未知 section 或 key 立即失败；
- 类型、整数范围或语义不合法立即失败；
- 加载是事务式的，失败不会部分修改现有配置；
- TOML 字符串由配置对象持有，并由
  `signaling_server_config_cleanup()` 统一释放；
- WebSocket 与管理 API 均可加载 PEM 证书/私钥并在进程内提供 WSS/HTTPS；
- JWT 和 Redis 仍是保留配置，启用这些路径会明确失败。

信令服务应用是根工程 `webrtc/apps` 下的构建目标；配置与 WSS/HTTPS
生命周期分别由 `turbo_media_test_signaling_config` 和
`turbo_media_test_signaling_lifecycle` 验证。

## RTC 服务进程

`webrtc/apps` 只随 `SERVER` product 构建并安装：

- `sfu_node`：WebRTC 会话、媒体发布/订阅、分层转发、录制和节点 drain；
- `room_service`：房间事实源、SFU 节点路由、状态重放和会议策略。

`sfu_node` 的 PeerConnection 路径使用 SaltsNet ICE agent，并由
`SaltsNet::ICE` 和 `Salts::CNet` 作为 RTC 目标的显式私有依赖提供
candidate gathering、connectivity checks、selected-pair I/O 与关闭排空。

两个进程依赖仓库内 `TurboMedia::RtcApps` 提供的房间/SFU/录制模型层
（`turbo_room_service`、`turbo_sfu_node`、`turbo_recorder` 与底层
`turbo_sfu`，源码位于 `webrtc/src/{conference,sfu,recording}`），
不再需要外部 TurboRTCApps SDK 或 `TURBORTC_ROOT`：

```powershell
cmake --preset win-dev-user
cmake --build --preset win-dev-user --target sfu_node room_service
```

示例配置：

- [sfu_node.toml.example](webrtc/apps/sfu_node/config/sfu_node.toml.example)
- [room_service.toml.example](webrtc/apps/room_service/config/room_service.toml.example)

两个加载器均执行严格 section/key/type/语义校验，并以事务方式提交配置。
运行时优先级为：

```text
内置默认值 < TOML 文件 < 环境变量 < 命令行参数
```

配置字符串归配置对象所有，进程退出或测试结束时分别调用
`sfu_node_app_config_cleanup()` 和 `room_service_app_config_cleanup()`。控制令牌
可以写入 TOML，但生产部署应优先通过帮助信息中列出的环境变量或命令行注入，
避免把真实凭据提交到仓库。

两个进程都支持 `--dry-run`；该模式只完成优先级合并、严格校验和最终配置打印，
不会创建监听器或后台线程。

应用进入根 CTest 后，配置测试和进程级测试可单独运行：

```powershell
ctest --preset win-dev-user -R "sfu_node|room_service" --output-on-failure
```

这些本地测试验证配置、控制 API、房间/SFU 同步、媒体转发与录制，不代表已经
完成公网浏览器矩阵、TURN、弱网、容量、长稳或多节点故障验收。

## 测试

最小相关测试可单独运行：

```powershell
cmake --build --preset win-dev-user --target turbo_media_test_signaling_config
ctest --preset win-dev-user -R turbo_media_test_signaling_config --output-on-failure
```

公网 HLS 兼容性测试默认关闭。显式运行时必须同时提供固定测试根地址：

```powershell
$env:TURBO_MEDIA_RUN_PUBLIC_HLS_TESTS = '1'
$env:TURBO_MEDIA_PUBLIC_HLS_ROOT = 'https://mtoczko.github.io/hls-test-streams'
ctest --preset win-dev-user -R turbo_media_test_hls_public --output-on-failure
```

启用公网测试但未提供根地址时会立即失败。

## 文档与示例

- [快速入门](QUICKSTART.md)
- [架构设计](ARCHITECTURE.md)
- [Pipeline](pipeline/README.md)
- [Codec](codec/README.md)
- [WebRTC 文档索引](webrtc/docs/README.md)
- [可编译示例](examples/)
- [测试说明](tests/README.md)

文档若与公开头文件、测试或当前构建结果冲突，以代码和可复验测试为准。
