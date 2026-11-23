# TurboMedia

TurboMedia 是一个 C11 多媒体框架，覆盖 SIP 语音、编解码、封装/解封装、流媒体协议、
FFmpeg 图式流水线以及 WebRTC/RTC 媒体处理。项目默认 fail fast：必需依赖、
非法配置和不受支持的执行路径都会明确失败，不做静默降级。

## 当前能力

| 模块 | 已实现能力 | 主要目标 |
| --- | --- | --- |
| Codec | H.264、H.265、VP8、VP9、Opus、G.711 μ-law/A-law | `TurboMedia::Codec` |
| Muxer/Demuxer | FLV、MP4/fMP4、MKV/WebM、MPEG-TS/PS | `TurboMedia::Muxer`、`TurboMedia::Demuxer` |
| SIP | 事务和对话、账户注册、语音通话、MESSAGE | `TurboMedia::SIP`、`TurboMedia::SIPClient` |
| Media Runtime | 媒体源、轨道、订阅和桌面服务器运行时 | `TurboMedia::MediaRuntime` |
| RTSP | 协议解析、客户端和服务端会话 | `TurboMedia::RTSP`、`TurboMedia::ServerRTSP` |
| Streamer | HLS、DASH、RTMP、HTTP-FLV | `TurboMedia::Streamer` |
| Pipeline | YAML 图校验、FFmpeg File/HLS/RTSP 输入、转封装/转码/filter、Runtime/RTP relay 与转码 | `TurboMedia::Pipeline` |
| Playback/Capture | FFmpeg 解码与 SaltsBase 设备适配 | `TurboMedia::Player`、`Salts::Media` |
| WebRTC/RTC | SDP、DataChannel、ICE、信令、RTP/RTCP、SRTP、jitter、NACK、TWCC、simulcast | `TurboMedia::WebRTC`、`TurboMedia::RTC` |

`media/containers/common/codec_helpers` 继续服务原有 muxer、demuxer 和 streamer 的码流转换；
一般文件或网络源之间的格式、编解码与传输转换由 `TurboMedia::Pipeline`
通过 FFmpeg 图执行，两套状态不会混在同一处理分支。

源码按协议和共享能力组织：

| 目录 | 职责 |
| --- | --- |
| [sip/](sip/CMakeLists.txt) | SIP 信令、客户端、SDP、RTP/RTCP、RTP ICE 和 SDK 消费示例 |
| [streaming/](streaming/CMakeLists.txt) | RTSP、HLS、DASH、RTMP、HTTP-FLV 和 FFmpeg 流媒体管道 |
| [webrtc/](webrtc/CMakeLists.txt) | WebRTC/RTC、DataChannel、信令和实时通信应用 |
| [streaming/transport/](streaming/transport/CMakeLists.txt) | 流媒体 CNet/HTTP 传输与 RTSP、DataChannel 共用的 CNet 发送适配 |
| [media/](media/README.md) | 共享编解码、封装、播放、设备适配和媒体服务运行时 |

## 构建

TurboMedia 只有一套构建图，不要求调用方选择产品 profile。平台决定可用能力：

| 平台 | 正常构建能力 |
| --- | --- |
| Linux / Windows | MediaRuntime（含服务器）、设备 I/O、RTC/WebRTC、Player、Streamer、Pipeline、desktop RTC apps |
| Android | MediaRuntime（媒体源）、设备 I/O、RTC/RTCClient、Player、mobile adapter |
| macOS | MediaRuntime（媒体源）、设备 I/O、RTC/RTCClient、Player |
| iOS | MediaRuntime（媒体源）、设备 I/O、RTC/RTCClient、Player、mobile adapter |

Windows 开发构建：

```powershell
cmake --preset win-dev-user
cmake --build --preset win-dev-user
ctest --preset win-dev-user --output-on-failure
```

Linux 使用 `linux-dev-user` / `linux-release-user`。Android 使用对应
`android-*-win` preset。平台依赖缺失直接 configure 失败，不回退到其他平台、
后端或源码依赖。

TurboMedia 通过 SaltsBase 的 `Salts::Media` 接入采集和播放。Linux/Windows 额外启用 desktop
service capability，因此还要求 RulesForge。TurboDB/Orm 不属于当前 TurboMedia
依赖图；数据库 Driver 不由 RoomService 隐式选择或加载。

主要共享依赖包括 FFmpeg（含 `openh264`、`opus`、`xml2` feature）、
OpenH264、x265、libde265、libvpx、Opus、SaltsBase、SaltsUtils、SaltsCNet、
CMeta、SaltsNet ICE、Chttp、libSRTP 和 usrsctp。
JWT 和 RTSP Digest 使用 `Salts::Crypto`。WebRTC 的 DTLS-SRTP 仍使用
WebRTC 使用 `Salts::Crypto` 协商 RFC 5764 `use_srtp`、验证 SDP 指纹和导出 SRTP 密钥。
`SALTS_CNET_ROOT` 必须指向公开 DTLS-SRTP API 的 SaltsCNet SDK。

拆分 SDK 从 `SALTS_BASE_ROOT`、`CMETA_ROOT`、`SALTS_CNET_ROOT`、
`SALTS_UTILS_ROOT` 和 `CHTTP_ROOT` 加载，版本由 `.env` 选择。
完整构建还需要 `SALTSNET_ROOT` 提供以同一拆分 SDK 构建的 ICE 目标。
`CHTTP_ROOT` 指向带 `WebSocketPeerLease` 组件的 Chttp 0.1.4 SDK。补丁、构建步骤与
信令配额生命周期见 [SDK 迁移设计](docs/superpowers/specs/2026-09-05-salts-dependency-refactor-design.md#2026-11-21-拆分-sdk-与共享缓存)。
IVR schema 由 `salts-idlc` 重新生成，当前协议要求 RoomService 与 worker 同步升级。
Windows 构建应在 `VsDevCmd.bat -arch=x64 -host_arch=x64` 环境执行。

### Windows LLVM-MinGW 共享依赖

将 [`.env.example`](.env.example) 中的设置及本机安装路径载入构建环境。
`VCPKG_CACHE_REPOSITORY_ROOT` 指向所选发布版本的 checkout，
`VCPKG_INSTALLED_DIR` 指向已恢复的 `vcpkg_installed` 目录。
`RE2C_ROOT` 和 `LEMON_ROOT` 指向共享的宿主工具包，Lemon 使用同一包中的程序和模板。
`VCPKG_BINARY_SOURCES` 可供多个项目使用同一二进制缓存。

共享发布归档提供通用依赖。完整构建还需要按本仓库的 `vcpkg.json` 安装媒体端口。
将 `VCPKG_INSTALLED_DIR` 指向 TurboMedia manifest 的安装根，根下应包含目标 triplet。
vcpkg 通过共享的 `VCPKG_BINARY_SOURCES` 恢复这一安装根所需的包。
拆分迁移的验证入口为 `cmake --preset llvm-mingw-x64-release-user`。
旧版 SDK 的回归需要使用相应的旧版 TurboMedia checkout 及其 SDK 配置。

Linux 桌面采集由 SaltsBase 的 `Salts::Media` 提供。从源码构建对应 SDK 时需要 `pkg-config`、
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
[Pipeline 设计与使用指南](streaming/pipeline/README.md)。仓库还提供：

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

`webrtc/apps` 属于 Linux/Windows desktop-service capability：

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

## 按应用安装

Windows 构建可将 SFU 媒体节点和转码工具分别安装到独立前缀。安装时从本次构建解析
各自实际使用的 DLL，并将它们与可执行文件放在一起。

```powershell
cmake --install <build-dir> --config Release --prefix <sfu-dir> --component SfuNode
cmake --install <build-dir> --config Release --prefix <transcode-dir> --component MediaTranscode
```

`SfuNode` 包含 `sfu_node` 和 TOML 配置示例。`MediaTranscode` 包含 `turbo_pipeline_run`，
通过 [Pipeline YAML](streaming/pipeline/README.md) 配置音视频处理。需要嵌入音视频编解码的
应用可分别安装 [`AudioCodec` 和 `VideoCodec`](media/codec/README.md#嵌入式-sdk)。

### SIP 语音组件

`TurboMedia::SIP` 处理 SIP 消息、事务和对话。应用通过 `sip_transport_t`
接入信令传输。`TurboMedia::RTP` 处理 RTP/RTCP，媒体端点、端口和收发队列由应用自行管理。
`TurboMedia::SDP` 协商媒体地址和格式，`TurboMedia::AudioCodec` 编解码 PCM。
设备采集和播放可通过 `Salts::Media` 接入。

应用使用 `<sip-registration.h>` 创建注册对象，并在信令线程中用单调时钟调用
`sip_registration_poll`。将来自选定 registrar 或代理的响应交给 SIP agent。
注册支持 `401/407` Digest、`423 Min-Expires` 及按服务器确认的 Contact 到期时间续租。
UDP registrar 改写本地 Contact 时，SDK 用该事务的 `Via received/rport` 核对公网绑定。
检查状态中的 SIP 响应码和本地错误。需要主动撤销绑定时调用
`sip_registration_unregister`，继续驱动 poll，直到收到 `SIP_REGISTRATION_INACTIVE`。
销毁注册对象会停止本地续租，服务端绑定按原期限到期。

将四个组件安装到同一 SDK 前缀。

```powershell
foreach ($component in 'SIP', 'RTP', 'SDP', 'AudioCodec') {
  cmake --install <build-dir> --config Release --prefix <sip-sdk> --component $component
}
```

```cmake
find_package(TurboMedia CONFIG REQUIRED COMPONENTS SIP RTP SDP AudioCodec)
target_link_libraries(my_app PRIVATE
  TurboMedia::SIP TurboMedia::RTP TurboMedia::SDP TurboMedia::AudioCodec)
```

组件消费需要匹配的 SaltsBase、CMeta、Opus 和 SaltsCNet SDK 中的 `Salts::Crypto`。
应用选用 CNet 作为网络后端时，单独链接 `Salts::CNet`。许可和第三方声明随组件安装至
`share/TurboMedia`。从旧版升级时，SIP 调用方链接 `TurboMedia::SIP`。RTP 调用方链接
`TurboMedia::RTP`，包含 `<turbo_rtp_packet.h>` 并使用 `rtp_packet_parse` 和
`rtp_packet_serialize`。RTP 包结构的 ABI 已变更，升级时需重新编译和链接调用方。
安装后验证入口见 [`installed_voice_consumer`](sip/tests/installed_voice_consumer)。

#### ICE/STUN/TURN 媒体传输

需要媒体 NAT 穿透时，额外安装并链接 `TurboMedia::RTPICE`。它依赖 `RTP`、`SDP`
和 `salts-netutils` 提供的 `SaltsNet::ICE`。使用 `SALTSNET_ROOT` 选择 SDK，并按该版本的
发布说明配置配套依赖。SIP 信令仍通过 `sip_transport_t` 单独收发。

```cmake
find_package(TurboMedia CONFIG REQUIRED COMPONENTS SIP RTP SDP AudioCodec RTPICE)
target_link_libraries(my_app PRIVATE TurboMedia::SIP TurboMedia::RTP TurboMedia::SDP
  TurboMedia::AudioCodec TurboMedia::RTPICE)
```

在应用的媒体线程创建 `<turbo_rtp_ice.h>` 传输，使用 `ice_config_t` 配置 STUN/TURN
和 ICE 角色。先收集候选并写入本地 SDP，通过 SIP 交换 offer/answer，再导入对端媒体段
并执行连通性检查。连接后，用 `turbo_rtp_ice_send` 发送 RTP/RTCP，用
`turbo_rtp_ice_poll` 驱动接收和 consent。当前支持 UDP、完整候选交换和协商后的
`rtcp-mux`。用 `ctest -R '^turbo_media_test_sip_uac_e2e$'` 运行回环测试。
为回环 TURN 服务设置 `TURBO_MEDIA_TEST_TURN_PORT` 后，测试仅通过 SDP 交换中继候选，
并检查选中路径与双向 RTP/RTCP。 [回环测试](sip/tests/test_sip_uac_e2e.c)

#### SIP 客户端 API

`TurboMedia::SIPClient` 是可选的 SIP 语音客户端，入口为 `<turbo_sip_client.h>`。
Windows 提供 DLL 和匹配工具链的导入库，其他平台提供静态库。
它将账户、呼入、拨号、接听、取消、挂断和 MESSAGE 组织成一个客户端实例。
每个实例持有一个账户和一个当前通话。通话结束后释放句柄即可再次拨号。
INVITE、BYE 和 MESSAGE 遇到 `401/407` 可用账户凭据自动重试。
客户端为每个已接纳的分叉应答发送 ACK，保留第一个应答的对话，并对其他对话发送 BYE。

Windows 语音 SDK 在同一个前缀安装以下组件：

```powershell
foreach ($component in 'SIP', 'SDP', 'SIPClient') {
  cmake --install <build-dir> --config Release --prefix <sip-sdk> --component $component
}
```

`SIPClient` 将 DLL 及运行时依赖装入 `bin/`，导入库装入 `lib/`。应用部署时将
`bin/` 中的 DLL 放到可执行文件旁。开发时使用匹配的 Salts SDK 和 CMake 导出。
静态平台另需安装 `RTP` 和 `AudioCodec`。应用直接使用低层 RTP 或 ICE API 时，
按需安装 `RTP` 或 `RTP` 与 `RTPICE`。

```cmake
find_package(TurboMedia CONFIG REQUIRED COMPONENTS SIPClient)
target_link_libraries(my_app PRIVATE TurboMedia::SIPClient)
```

配置中的 `signaling` 管理 SIP 路由和收发，`media` 独立管理 RTP/RTCP 端点和传输。
应用从同一个 owner 调用 `turbo_sip_client_poll` 和输入接口，传入单调毫秒时钟。
回调的报文和 PCM 只在当前调用期间有效，需要异步使用时由接入层复制。
应用在事件回调返回后执行接听或挂断。PCM 每帧为 20 ms，G.711 为 8 kHz 单声道，
Opus 为 48 kHz 双声道。`media.prepare` 配置本地 SDP，`media.start` 在通话建立后
应用双方的媒体参数；接入 ICE 时，媒体 owner 可以通过 `RTPICE` 驱动检查和收发。
主动退出时先挂断并注销，继续 poll 直到收到终态，再销毁客户端。

当前客户端工作于完整 UDP SIP 报文、预先提供 SDP 的 INVITE 和单路 RTP/AVP 语音，
支持 PCMU、PCMA 和 Opus。
TCP/TLS 信令、SRTP、早期媒体和 DTMF 仍需相应的部署支持及互通验证。
调用方继续使用原有协议组件时可独立管理事务、媒体和设备。
[公开 API](sip/client/include/turbo_sip_client.h) ·
[接入与回归示例](sip/tests/test_sip_client.c)

## 文档与示例

- [快速入门](QUICKSTART.md)
- [架构设计](ARCHITECTURE.md)
- [媒体组件源码](media/README.md)
- [Pipeline](streaming/pipeline/README.md)
- [Codec](media/codec/README.md)
- [WebRTC 文档索引](webrtc/docs/README.md)
- [可编译示例](examples/)
- [测试说明](docs/testing.md)

文档若与公开头文件、测试或当前构建结果冲突，以代码和可复验测试为准。
