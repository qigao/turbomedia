# TurboMedia Server/Client Todo

本文档跟踪 TurboMedia 服务端与客户端实现计划。当前架构约束是：
`Protocol Adapter -> ServerRuntime -> MediaRegistry -> MediaSource` 是唯一媒体状态路径，协议层不得各自维护独立流事实源。

## 当前基线

- `TurboMedia::Core` 已提供 `MediaSource`、`MediaRegistry`、track、frame、GOP cache 与订阅分发。
- `TurboMedia::Server` 已提供 `ServerRuntime`、`ProtocolSession`、RTMP/RTSP adapter helper。
- `TurboMedia::Server` 已提供基础 `Server facade` 生命周期 API：create/start/stop/destroy/get_runtime，内部拥有或复用 `coro_context_t` 并持有一个 `ServerRuntime`。
- `TurboMedia::Media` 与 `turbo_media.dll` 已移除。
- 默认构建已验证多 DLL：`core`、`server`、`codec`、`muxer`、`demuxer`、`transport`、`streamer`、`device`。
- `turbo-webrtc/webrtc` 已迁入本仓库 `webrtc/`；TurboMedia 负责 SDP、DTLS/SCTP DataChannel 与统一 RTC adapter，TurboRTC 暂时保留 ICE、RTP/RTCP/SRTP、PeerConnection、SFU 和 conference transport/session 实现。
- 本地 `C:\projects\cpp\turbonet\turbo-webrtc` 仍作为 `TurboMedia::RTC` 的可选 PeerConnection backend；其内部 `turbo_media_context` 或 SFU participant state 不得升级为 TurboMedia 跨协议流事实源。
- TurboRTC 已导出不含旧 `TurboRTC::Media` 的组件化 package；`TurboMedia::RTC -> TurboRTC::MediaEngine -> TurboMedia::Device/DataChannel` 安装包消费链已验证。

## HIGH

### 1. Server facade

目标：提供统一服务端生命周期 API，屏蔽 CoroNet、RTSP、RTMP listener 的组合细节。

状态：基础生命周期已实现；RTSP/RTMP listener 组合将在对应 adapter 接入时挂到该 facade。

任务：
- 新增 `turbo_media_server_create`、`turbo_media_server_start`、`turbo_media_server_stop`、`turbo_media_server_destroy`。
- 新增 `turbo_media_server_get_runtime`，允许 HTTP API 或测试访问只读/命令入口。
- Server facade 内部拥有 `turbo_media_server_runtime_t`。
- Server facade 内部拥有或复用 `coro_context_t`。
- Server facade 不直接持有媒体帧、track cache 或 subscriber 列表。

完成条件：
- 创建/销毁 server 不泄漏 runtime。
- start/stop 可重复调用并返回稳定错误码。
- 现有 `ctest` 全部通过。

验证：
```powershell
cmake --build build/codex-mediasource-check --config Debug --parallel
ctest --test-dir build/codex-mediasource-check --output-on-failure
```

### 2. RTSP server adapter

目标：RTSP 服务端请求统一转换为 `ProtocolSession` 操作。

状态：基础 adapter 已实现，使用 `turbo_rtsp_server_t` 回调接入 `ServerRuntime`；当前覆盖 TCP interleaved publish/play 路径、连接关闭清理和最小 E2E 测试。ANNOUNCE SDP 仍未驱动 track metadata，当前使用默认 RTP track。

任务：
- `[done]` `ANNOUNCE`：解析 URI，准备 publish source key。
- `[partial]` `SETUP`：记录 TCP RTP/RTCP interleaved channel；UDP transport 未接入。
- `[done]` `RECORD`：调用 `turbo_media_server_rtsp_open_record` 创建 publisher session。
- `[done]` `on_interleaved_frame`：调用 `turbo_media_server_rtsp_publish_interleaved`。
- `[done]` `PLAY`：调用 `turbo_media_server_rtsp_open_play` 创建 player session。
- `[done]` player callback：把 `MediaSource` frame 发送回 RTSP interleaved channel。
- `[done]` `TEARDOWN` 或连接关闭：close 对应 `turbo_media_protocol_session_t`。
- `[pending]` ANNOUNCE SDP track metadata：解析 H264/AAC 等 rtpmap/fmtp 后注册到 `MediaSource`。

完成条件：
- 一个 RTSP publisher 写入 RTP frame。
- 一个 RTSP player 从同一 source 收到相同 frame。
- publisher 关闭后按配置删除 source。

当前验证：
- 默认构建 `ctest` 通过。
- `TURBO_MEDIA_ENABLE_RTSP=ON` 时 adapter 可编译，`turbo_media_test_server_rtsp_adapter` 通过。
- `TURBO_MEDIA_ENABLE_RTSP=ON` 全量 `ctest` 通过。

风险：
- HIGH：RTSP session 生命周期和 socket 生命周期不一致会导致 use-after-free。
- MED：同步 callback 直接发 socket 可能遇到回压，需要先保证错误能向上返回。

### 3. RTSP end-to-end test

目标：覆盖最小可跑链路。

任务：
- `[done]` 新增测试：server 启动后，client ANNOUNCE/SETUP/RECORD 推 RTP。
- `[done]` 新增测试：另一个 client DESCRIBE/SETUP/PLAY 拉取同一 source。
- `[done]` 验证 `MediaRegistry` 只有一个 source。
- `[done]` 验证 close 后 subscriber/source 状态正确清理。

完成条件：
- `[done]` 新测试可独立运行。
- `[done]` 新测试与全量 `ctest` 均通过。

## MED

### 4. RTC/WebRTC adapter

目标：基于本地 `turbo-webrtc` 提供 WebRTC publish/play 接入，仍统一落到 `ProtocolSession -> ServerRuntime -> MediaRegistry -> MediaSource`。

依赖边界：
- `TurboHTTP::Iris` 负责 signaling/API，例如 WHIP ingest，后续扩展 WHEP play。
- `TurboMedia::SDP` / `TurboMedia::DataChannel` 负责 SDP、DTLS、SCTP、DCEP；DataChannel 通过 external transport bridge 借用外部 ICE transport，不拥有 ICE agent。
- `TurboRTC::MediaEngine`/`turbo_peer_connection` 暂时负责 ICE、SRTP、RTP/RTCP、NACK、PLI、TWCC 和 RTC track/session 状态。
- `TurboNet::CoroNet` 继续负责普通 TCP/UDP 协议入口；RTC 媒体面由 `turbo-webrtc` 的 ICE transport 驱动。
- `TurboMedia::RTC` 依赖 `TurboMedia::Server` 和可选 `TurboRTC::MediaEngine` backend，不得让 `TurboMedia::Core` 反向依赖 RTC。

任务：
- `[done]` 新增可选 `turbo_media_rtc` / `TurboMedia::RTC` target，默认可通过构建选项关闭。
- `[done]` 新增 RTC source key 规则：`/{app}/{stream}`、WHIP resource path、HTTP query 中的 vhost/app/stream 归一化为 `turbo_media_source_key_t`。
- `[done]` 迁移 `turbo-webrtc/webrtc` 到 `turbomedia/webrtc`，导出 `TurboMedia::SDP` 与 `TurboMedia::DataChannel`。
- `[done]` WebRTC publish：远端 recv track 的 plaintext RTP packet 回调转换为 `turbo_media_frame_t`，通过 `TURBO_MEDIA_PROTOCOL_WEBRTC` publisher session 写入 `MediaSource`。
- `[done]` WebRTC play：通过 `TURBO_MEDIA_PROTOCOL_WEBRTC` player session 订阅 `MediaSource`，将收到的 RTP frame 送入本地 send track。
- `[done]` `turbo_peer_connection_poll`、`turbo_dc_handle_timers`、`turbo_media_handle_timers` 由显式 RTC pump 驱动，不依赖隐藏线程。
- `[done]` 重建 TurboRTC package 导出并验证启用真实 `TurboRTC::MediaEngine` backend 的 TurboMedia 构建、测试和安装包消费者。
- `[done]` 将 `turbo-webrtc/ice` 迁入 TurboNet，并导出可安装的 `TurboNet::Ice` target。
- `[pending]` 将 `ice_integration.c` 与 ICE E2E 测试接入 `TurboNet::Ice`。
- `[pending]` 将已迁入的 room signaling / Iris HTTP API 注册为独立 `TurboMedia::RTCSignaling` target；不得并入 DataChannel DLL。
- `[pending]` 修复旧 `test_datachannel` / `test_dc_msg` 套件退出阻塞后重新注册；当前只验证 SDP、统一 RTC bridge 和 TurboRTC MediaEngine consumer build。
- RTCP feedback 初期只在 RTC adapter 内处理；跨协议转发 PLI/NACK/TWCC 需要单独设计，不混入 `MediaSource`。

完成条件：
- 一个 WHIP/WebRTC publisher 写入一个 `MediaSource`。
- 一个 WebRTC player 从同一 `{vhost, app, stream}` 订阅并发送 RTP。
- RTMP/RTSP/WebRTC 共用同一 `MediaRegistry`，无第二套 source registry。
- `[done]` RTC target 关闭时，现有构建和 `ctest` 不受影响。

风险：
- HIGH：TurboRTC 目前仍有自己的 `turbo_media_context` 与 SFU 状态，如果直接作为业务流目录，会破坏 TurboMedia 单一事实源。
- HIGH：`turbo_peer_connection` 当前需要显式 `poll()`/timer 驱动，若生命周期绑定不清会导致连接无法推进或销毁期间回调访问已释放对象。
- MED：RTC packet 是 RTP/RTCP 级别，RTMP/HTTP-FLV 是封装帧级别，跨协议互通需要后续补 depacketize/packetize 边界。
- MED：ICE/STUN/TURN、DTLS、SRTP 引入 OpenSSL、libsrtp、usrsctp 等部署依赖，需要保持 RTC 为可选组件。

### 5. RTMP server adapter

目标：RTMP AIO server 回调统一转换为 `ProtocolSession` 操作。

任务：
- `onpublish`：调用 `turbo_media_server_rtmp_open_publish`。
- `onplay`：调用 `turbo_media_server_rtmp_open_play`。
- `onvideo`：调用 `turbo_media_server_rtmp_publish_video`。
- `onaudio`：调用 `turbo_media_server_rtmp_publish_audio`。
- `onclose`：close `turbo_media_protocol_session_t`。
- player callback：根据 `track_id` 调 `aio_rtmp_server_send_video` 或 `aio_rtmp_server_send_audio`。

完成条件：
- RTMP publish/play 使用同一 `{vhost, app, stream}` key。
- 不引入 RTMP 层独立 source registry。

风险：
- MED：当前 RTMP client/server 仍有握手和发送路径未完成区域，需要先限制测试范围。

### 6. Client facade

目标：提供统一客户端 API，客户端拉流写入本地 `MediaSource`，客户端推流从本地 `MediaSource` 订阅。

任务：
- 新增 `turbo_media_client_create/destroy`。
- 新增 `turbo_media_client_pull_start/stop`。
- 新增 `turbo_media_client_push_start/stop`。
- RTSP pull：收到 RTP/interleaved frame 后 publish 到本地 `ServerRuntime`。
- RTSP push：从本地 `MediaSource` subscribe 后发 RTP/interleaved frame。

完成条件：
- 本地 client pull 产生一个可查询 source。
- 本地 client push 能从已有 source 读取 frame。

### 7. Key mapping contract

目标：明确跨协议统一流标识。

规则：
- RTMP `app=live, stream=cam` -> `{vhost=default, app=live, stream=cam}`。
- RTSP `/live/cam` -> `{vhost=default, app=live, stream=cam}`。
- RTSP `rtsp://host/live/cam` -> `{vhost=default, app=live, stream=cam}`。
- WHIP `/live/cam` 或 HTTP 参数 `app=live&stream=cam` -> `{vhost=default, app=live, stream=cam}`。
- HTTP API 使用同一三元组。

完成条件：
- 文档和测试覆盖 RTMP/RTSP 同 key 互通。

### 8. Threading and backpressure

目标：明确初版线程模型和发送失败语义。

任务：
- 文档化：初版假设同一 CoroNet loop 串行访问 `ServerRuntime`。
- 对 player callback 发送失败定义清理策略。
- 记录 socket 发送失败是否取消订阅。

完成条件：
- 所有跨层错误返回可复验，不只记录日志后返回成功。

## LOW

### 9. Error strings

目标：提升 HTTP API、日志和测试输出可读性。

任务：
- 新增 `turbo_media_error_to_string(turbo_media_result_t code)`。
- 覆盖所有 `TURBO_MEDIA_ERR_*`。
- 增加测试。

### 10. Observability

目标：提供最小运行时诊断。

任务：
- 增加 server stats 查询接口。
- 区分 active sources、publisher sessions、player sessions。
- 记录 frames published/delivered。

### 11. Docs

目标：同步架构文档。

任务：
- 更新 `ARCHITECTURE.md`：加入 `Core`、`ServerRuntime`、`ProtocolSession`。
- 更新 `README.md`：移除旧单 DLL 表述。
- 增加 RTSP/RTMP flow 图。

## 不做事项

- 不恢复 `TurboMedia::Media`。
- 不恢复 `turbo_media.dll` 聚合库。
- 不在 RTMP/RTSP 协议层新增独立媒体 registry。
- 不在 RTC/WebRTC 层新增跨协议媒体 registry；`turbo-webrtc` 内部连接/track/SFU 状态只能作为 RTC transport/session 状态。
- 不把 payload 解析、RTP depacketize、codec-aware packetize 混入 `MediaSource`。
- 不在未定义线程模型前支持跨线程直接 publish/subscribe。
