# TurboMedia IVR Worker（第一阶段）

本文档说明 `webrtc/ivr/` 模块的范围、布局与已知限制。相关文档：

- [独立 IVR Worker 架构](../docs/ivr-worker-architecture-zh.md)
- [IVR 生产化缺口解决方案](../docs/ivr-production-readiness-design-zh.md)
- [IVR 生产化 TODO Checklist](../docs/ivr-production-readiness-checklist-zh.md)

## 范围（第一阶段：conference 基线）

- [x] **公开 ABI**：`include/ivr/ivr_worker.h`（opaque struct + borrowed view + 错误码 +
  命令网关/媒体端口接口），与架构文档的 C 接口草案一致，并补上 `play_pcm`、
  事件 `input_id/input_value` 与 worker 配置项。
- [x] **TIVR frame 协议**：`src/ivr_frame.c`，12 字节自描述 header + 版本化
  `schema_type_id` 常量表（与 `schema/turbomedia_ivr_v1.schema` 的 message id 一致）。
- [x] **canonical protocol**：`schema/turbomedia_ivr_v1.schema` + `src/ivr_protocol.c`；
  TIVR 只发布 BIN/TEXT 两种 wire 表示，TEXT 是 compact UTF-8 JSON，两者共用同一
  DataBind object；当前生产帧使用 BIN，测试覆盖 semantic round trip、短 buffer、错误
  type/kind 和 BIN golden vector。
- [x] **per-call session**：`src/ivr_session.c`，有界 FIFO inbox、单调 sequence 与 gap 检测、
  snapshot 恢复（gap 时发 `get_snapshot` 并停止推进）、input window（同一
  `input_id` 首 final 获胜、迟到/重复丢弃计数）、terminal latch、每 call 独占控制线程。
- [x] **worker 生命周期**：`src/ivr_worker.c`，create/start/assign/begin_drain/destroy，
  `max_sessions_per_worker` 准入（超限返回 `IVR_ENOSPC`），内容包缓存。
- [x] **内容包**：`content/conference-greeting/`（manifest + rtc_session.scxml +
  call_control.ccxml + conference_menu.vxml + traces），manifest 校验 allowlist 与
  `command_map`（识别输入 → 业务命令）。
- [x] **TurboXML 适配器**：`src/ivr_turboxml_adapter.c`，CCXML + 内建 CCXML↔VXML dialog
  bridge 驱动会议菜单；`<submit next="ivr://command/...">` 按 manifest `command_map`
  在 adapter 层转换（当前 TurboXML 不暴露该 submit 语义）；**`rtc_session` 已迁移为
  由 SCXML 引擎直接驱动**：`rtc_session.scxml` 的 `<send target="ivr.command">` 通过
  plugin `on_execution_point` 捕获（exec point 现暴露 event/target/type/param.* 完整内容），
  命令名取 send event、`<param>` 转成命令 args JSON（如 `{"role":"ivr-bot"}`）。
- [x] FlowMQ 命令网关、RoomService bridge 和测试 mock 已实现并在当前 dev preset
  验证。
- [ ] 咨询转接（场景四）仍属第二阶段，不在本次范围。

## FlowMQ 命令网关（P0）

- [x] `CMakeUserPresets.json` 新增 `FLOWMQ_ROOT`（`$env{PKG_ROOT}/turboflow`）；ivr CMake 把
  `FLOWMQ_ROOT` 加入 `CMAKE_PREFIX_PATH` 后 `find_package(FlowMQ)` /
  `find_package(TurboFlow)`。
- [x] 安装的 TurboFlow 包在 `TurboFlow::Flow` 的 link interface 引用了
  `RulesForge::rules_forge` 但未随包安装 config——CMake 中以一个空
  `INTERFACE IMPORTED` shim 兼容（gateway 从不调用 RulesForge）。
- [x] `src/ivr_flowmq_gateway.c`：`ivr_command_gateway_ops_t` 的 FlowMQ DEALER 实现
  （CONNECT 到 RoomService ROUTER）。命令编码为 TIVR frame（12 字节头 + canonical
  schema 的动态 DataBind BIN payload）；`conference.join/leave`、
  `get_snapshot` 映射到对应 message，`rtc.*/accept/disconnect` 属 worker 本地意图，
  在 gateway 被拒（`IVR_ESTATE`），不上 DEALER 通道。
- [x] `FlowMQ + TurboFlow + tbe_compiler` 是 IVR 构建必需依赖；任一缺失时 CMake
  fail fast，不生成 interface-only 或生产 fallback。mock gateway 只用于单元测试注入。
- [x] `src/ivr_room_bridge.c`：RoomService 侧 `room_fmq_bridge_t`（ROUTER BIND）。
  在独立 worker 线程处理（FlowMQ 回调只 detach+clone 入队，不 re-enter send）：
  解码 TIVR 命令帧 → `message_id` 幂等缓存（有界）→ `expected_room_version`
  校验（stale → `IVR_EVERSION`，不 apply）→ host handler 应用 → 回
  `IvrCommandResultV1` 帧给 DEALER。host（RoomService）通过
  `get_room_version` / `on_command` 钩子持有权威状态。
- [x] `test_ivr_room_bridge`：真实 FlowMQ DEALER→ROUTER 端到端（round-trip +
  `IvrCommandResultV1` 回执、同 `message_id` 重传只 apply 一次、stale 版本拒绝）。
- [x] **domain event PUB/SUB**：`src/ivr_room_bridge.c` 提供 PUB endpoint
  （live IVR 要求 `pub_port>0`，默认 topic `room.events`），命令 apply 成功后发布
  `ConferenceParticipantJoinedEventV1` 帧（计数 `events_published`）；
  `src/ivr_flowmq_subscriber.c` 是 worker 侧 SUB 订阅器（SUB CONNECT 到
  RoomService PUB），把 TIVR event 帧解码为 `ivr_event_view_t`
  （`schema_type_id` → 事件名，如 `room.participant.joined`；session 约定
  `call.expected_room_version` 即事件 `room_version`），`on_event` 回调内借用、
  回调外自持。纯解码 `ivr_flowmq_subscriber_decode` 可单测，非 event 帧
  （result/command/短包）fail fast 拒绝。
- [x] `test_ivr_flowmq_subscriber`：纯解码单元（participant_joined 字段断言 +
  result/command/短包拒绝）+ 真实 PUB/SUB 端到端（DEALER join → bridge apply →
  PUB → SUB on_event 收到 `room.participant.joined`，校验
  room_version/sequence/participant_role）。
- [x] **RoomService 真实 aggregate 接入**（`webrtc/apps/room_service/src/ivr_fmq_adapter.c`）：
  bridge 的 host handler 现由真实 `turbo_room_service_t` 支撑——`get_room_version`
  取 room summary 的权威 `version`；`conference.join` → `add_participant`
  （`participant_role` 映射到聚合角色枚举，`caller/customer→CUSTOMER`、
  `agent→AGENT`、`bot→BOT`、`supervisor→SUPERVISOR`、`guest→GUEST`，未知/空角色
  fail fast）；`conference.leave` → `remove_participant`；`get_snapshot` 为只读
  version/sequence 报告。聚合无 per-call 计数器，adapter 内维护有界
  （默认 256，满则 `IVR_ENOSPC`）per-call 派生 sequence 表（join 幂等重放不推进）。
  配置：`room_service` 新增 `[fmq]` 表（`bind_host`/`bind_port`/`pub_port`/
  `pub_topic`；启用 IVR 时 ROUTER/PUB 均为必需，关闭端口只表示整个 IVR bus 未启用；
  env `TURBO_ROOM_SERVICE_FMQ_*`）。`webrtc/ivr/CMakeLists` 要求 FlowMQ 并置
  `TURBO_MEDIA_HAS_IVR_FMQ`，`apps` 在其后链接 `turbo_media_ivr`。
- [x] `test_ivr_fmq_adapter`：纯 apply（真实聚合 join/leave/snapshot、幂等重放、
  角色冲突/未知角色/房间缺失 fail fast）+ 真实 DEALER→ROUTER 端到端（join 落到
  aggregate、`message_id` 重传不重复 apply、stale expected_room_version 拒绝）。
- [x] **快照恢复闭环（get_snapshot → `room.snapshot.loaded` → per-call session）**：
  bridge 对 `get_snapshot` 成功时在 PUB 发布 `RoomSnapshotV1`（kind=snapshot，
  `ivr_room_bridge_encode_snapshot`）；subscriber 接受 snapshot 帧并映射
  `RoomSnapshotV1 → room.snapshot.loaded`（snapshot 的连续性标记字段是
  `last_sequence`）；worker 新增公开 `ivr_worker_submit_event_copy()`，按
  room_id+call_id+call_generation 路由到 per-call session inbox（SUB/ASR/DTMF
  回调线程可直接调用，未分配调用丢弃计数）。gateway 编码修正：只有 schema 消息
  带 `expected_room_version` 时才输出该字段（`GetSnapshotCommandV1` 等不带）。
- [x] 快照恢复测试：`test_ivr_flowmq`（get_snapshot 编码无多余字段）、
  `test_ivr_flowmq_subscriber`（snapshot 纯解码 + 真实 get_snapshot→PUB→SUB 事件）、
  `test_ivr_worker`（worker 路由命中/未命中、gap→get_snapshot→snapshot 恢复会话）。
- [x] **worker.sync 注册握手（readiness 前置）**：gateway 新增
  `ivr_flowmq_gateway_send_worker_sync()` / 纯编码
  `ivr_flowmq_gateway_encode_worker_sync()`（`WorkerSyncCommandV1`，仅
  message_id+worker_id，无 per-call 字段）；bridge 识别 `worker.sync` 命令并回
  `WorkerSyncResultV1`（`ivr_room_encode_worker_sync_result`，回复编码按命令分支）；
  RoomService 侧 adapter 维护有界（默认 64）worker 注册表（幂等、满则
  `IVR_ENOSPC`、空 worker_id 拒绝），`ivr_fmq_adapter_worker_registered()` 可查询。
  测试：`test_ivr_flowmq`（worker.sync 编码无 per-call 字段）、
  `test_ivr_room_bridge`（DEALER worker.sync → `WorkerSyncResultV1` 回执）、
  `test_ivr_fmq_adapter`（纯注册/幂等/空拒绝 + 真实 DEALER 注册到聚合）。
- [x] **RoomService→worker call 分派（dispatch）**：schema 新增
  `CallDispatchCommandV1`（id 1006，含 worker_id/room/call/generation/
  expected_room_version/content_package）；bridge 在 `worker.sync` 成功时保存该
  DEALER 的 ROUTER detached route（有界 route 表，延迟回复模式，重连后由
  re-registration 刷新），`conference.join` 提交后 adapter 轮询选一个已注册
  worker 并经该 route 推送 `CallDispatchCommandV1`（`ivr_room_bridge_dispatch_call`）；
  worker 侧 gateway 新增 `ivr_flowmq_gateway_decode_dispatch` 解码，`ivr_worker`
  可执行文件 on_reply 收到 dispatch 后 `assign_session`（`ivr_worker_has_session`
  幂等守卫，重复 dispatch 不建重复 session）。探针验证：ROUTER 不支持按任意
  identity 定向推送（facade `send_message` 仅保留 detached route），故采用
  worker.sync 捕获 route 的延迟推送，符合 FlowMQ 文档化模式。
- [x] **dispatch ACK 与事件顺序**：worker owner loop 在 session/media 创建后发送
  `CallDispatchResultV1` accepted/rejected；Room bridge 校验当前 route 后通知 RoomService
  adapter 的有界 assignment 表。`participant.joined` PUB event 仅在 accepted ACK 后发布，
  避免 worker mailbox 时序导致首个事件丢失。当前 join response 仍 immediate-success；lease、
  deadline、ACTIVE gating 和自动重派尚未完成。
- [x] **全链路集成测试 `test_ivr_worker_e2e`**（room_service 侧）：真实
  `turbo_room_service_t` aggregate + `ivr_fmq_adapter`（ROUTER+PUB）+ 真实
  DEALER gateway + SUB subscriber + 真实 worker/session（TurboXML，shadow
  命令网关捕获）。DEALER `conference.join` → aggregate 落 participant →
  PUB `room.participant.joined` → SUB → `ivr_worker_submit_event_copy` →
  session（last_sequence/room_version 推进、聚合可查 participant）；
  `get_snapshot` → `RoomSnapshotV1` → session 连续性保持；未分配 call 的事件
  被丢弃不影响已分配 session。
- [x] **独立 `ivr_worker` 可执行文件**（`webrtc/apps/ivr_worker/`）：CLI 配置
  （worker-id/content-root/router/pub/max-sessions/assign），接线 DEALER gateway +
  SUB subscriber + worker + 媒体 adapter；live 默认经 FlowMQ 转发，显式 `--shadow`
  只用于 canary 捕获 mutation command；启动即发
  `worker.sync` 注册，未收到 `WorkerSyncResultV1` 回执则每 2s 重试（readiness
  前置）。DEALER callback 只把有界 TIVR frame 复制到固定容量 Disruptor mailbox，
  由 main loop 解码和推进 session，避免在 FlowMQ callback 内重入发送；SIGINT/SIGTERM
  优雅 drain；`--dry-run` 验证配置并实际创建
  worker/session/内容包后退出。双进程冒烟已验证：`room_service`（启用
  `[fmq]`）↔ `ivr_worker` 连接、注册回执到达。
- [x] **媒体端口适配器（P2 核心）`ivr_media_bot`**：`ivr_media_port_ops_t` 的
  TurboMedia speech 实现（`turbo_speech`，provider 可插拔）。`play_pcm` 把文本经
  `turbo_tts` 合成，PCM 帧交给可插拔音频传输（生产为 WebRTC/SFU 发送轨边界）；
  传输侧喂入的 caller PCM 经 `turbo_asr` 识别，final 结果转成 `asr.final` 事件
  （`ivr_media_bot_feed_caller_audio`，回调线程安全）。`cancel_input` 按
  turbo_speech 契约 quiesce（TTS 需 RUNNING 才触发 provider cancel），`stop_bot`
  先 cancel 再 destroy 再释放（回调屏障后才有 UAF 安全）；baseline 单活动 call，
  缺 TTS provider 时 `play_pcm` fail fast 不伪造音频。`test_ivr_media_bot` 用
  mock TTS/ASR provider + loopback 传输验证 TTS→PCM→发送轨、caller PCM→
  `asr.final`、wrong-call 丢弃、cancel/stop 生命周期、缺 provider 拒绝。
  `ivr_worker` 可执行文件已改用 `ivr_media_bot`（logging 传输 + `asr.final`
  等媒体事件经 `ivr_worker_submit_event_copy` 直接路由进 session）；TTS/ASR
  provider 与 WebRTC/SFU 传输按 config 接入（当前 NULL provider → `play_pcm`
  fail fast 不伪造音频）。gateway 新增 `ivr_flowmq_gateway_send_frame`（DEALER
  裸帧发送，供未来命令/测试用）。
- [x] **远程 TTS/ASR provider（OpenAI-compatible，`src/ivr_openai_provider.c`）**：
  `turbo_speech` 的可插拔远程实现，协议对齐 OpenAI Audio API（端点/路径/模型/密钥均可配置）：
  - **TTS**：`POST {base_url}{tts_path}`（默认 `/v1/audio/speech`），JSON body
    `{model,input,voice,response_format:"pcm",speed}` → 返回 24 kHz 16-bit 单声道 PCM；
    provider 线性插值重采样到 bot 期望的 `sample_rate`（默认 16000），按
    `tts_frame_bytes` 分帧回调 `on_audio`，完毕 `on_complete`。
  - **ASR**：`start()` 记录格式，`write()` 非阻塞累积 PCM（有界
    `max_asr_buffer_bytes`，超限 `TURBO_SPEECH_ERR_BUSY` 拒绝并计数，绝不静默丢弃），
    `finish()` 把 PCM 包成 WAV（RIFF/WAVE，`ivr_openai_build_wav`）→
    手动构造 `multipart/form-data` body（`ivr_openai_build_multipart`，单调边界 +
    文件/模型/语言 parts）经 `http_post` + `Content-Type` 头 POST
    `{base_url}{asr_path}`（默认
    `/v1/audio/transcriptions`，字段 `file`/`model`/可选 `language`）→ 解析
    `{"text":"..."}` 回调 `on_result(is_final=1)` + `on_complete`；空缓冲本地完成，
    不发网络请求。
  - **线程模型**：每个 wrapper 持有一条常驻 worker 线程跑阻塞 http_client
    （`http_client` 是同步 coroutine-backed 客户端，worker 线程内直接调用）；
    provider 回调在 worker 线程串行触发。`cancel()` 置位取消标志并等待 worker
    quiesce（回调屏障，`cancel` 返回后无任何回调在途）；`destroy()` 仅 quiesce，
    保留 wrapper 供 media bot 跨 call 复用（app 持有生命周期，退出时
    `ivr_openai_*_free` 停线程并释放）。错误（HTTP 非 2xx/传输/解析/空音频）
    走 `on_error(TURBO_SPEECH_ERR_PROVIDER/FORMAT, 消息)`，fail fast 不伪造结果。
  - **接入**：`ivr_worker` 可执行文件读环境变量
    `IVR_OPENAI_BASE_URL`（必填才启用）+ `OPENAI_API_KEY` +
    `IVR_OPENAI_TTS_MODEL/TTS_VOICE/ASR_MODEL/ASR_LANGUAGE/SAMPLE_RATE/TIMEOUT_MS`，
    把 provider 注入 `ivr_media_bot_config_t`；未配置时保持 NULL（`play_pcm` fail
    fast，行为与基线一致）。
  - **验证**：`test_ivr_openai_provider`（9 用例）用进程内 mock HTTP 服务器（raw
    socket，支持 Content-Length 与 chunked body）验证真实协议路径：TTS 请求体字段
    + 认证头 + 重采样后帧数/采样率、ASR multipart 字段 + WAV 魔数、非 2xx →
    `on_error`、cancel 回调 quiesce（无 on_complete/on_result）、在途 synthesize
    拒绝、缓冲上限拒绝、空音频本地完成。真实 OpenAI 端点验证需 API key（当前
    环境无 key，协议路径由 mock 覆盖）。
- [x] **三进程分派冒烟自动化（`test_ivr_dispatch_processes`）**：测试进程自己
  spawn 真实 `room_service` + `ivr_worker` 可执行文件，经 HTTP 控制 API（raw
  socket）建立 room、customer participant 和带 SSRC 的 Opus caller audio track，
  再以 DEALER 客户端经 FlowMQ 发 `conference.join`；断言 command result
  `status_code == 0`，且 worker 进程先 `worker.sync acknowledged`、后记录
  `dispatch room-42/call-42 ... assigned`。覆盖真实部署拓扑（独立进程、独立
  FlowMQ 实例、管理 HTTP + FlowMQ command/result/dispatch），替代原「手动验证项」。
- [x] **worker-loss fail-closed**：FlowMQ peer disconnect 会立即失效旧 ROUTER route；
  adapter 不再选择断线 worker。无 live worker 的 join 返回 retryable 错误并保持 Room
  participant/version 不变；新增 participant 在 media/dispatch 失败时执行补偿，固定
  registry slot 可由 replacement worker 重新注册复用。证据：`test_ivr_fmq_adapter`。
- [x] **worker→session→media_bot 全环 e2e（`test_ivr_worker_media_loop`）**：真实
  `ivr_worker` + per-call TurboXML session + 真实 `ivr_media_bot`（mock TTS/ASR +
  loopback 传输）。按 happy path 提交 `room.assigned`→`rtc.connected`→
  `connection.alerting`，CCXML/VXML 菜单提示音经 `play_pcm`→TTS 合成→PCM 到传输；
  喂入 caller PCM→ASR→`asr.final`("1")→路由回 session 的 collect_input→
  command_map→shadow gateway 捕获 `conference.join`。证明媒体端口与 session
  输入/命令事件闭环真实打通（19 断言）。
- [x] **真实 WHIP/WHEP 双向音频传输**：WHIP 发布侧实现
  `ivr_media_transport_t` 的 WebRTC 发布传输——`turbo_peer_connection`（ICE/DTLS/
  SRTP）+ 音频发送轨；WHIP POST 最小 offer（从 create_offer 提取 ufrag/pwd/
  fingerprint，候选走 trickle）→ 201 + Location/ETag → set_remote_description →
  PATCH（If-Match）trickle 本地候选 → 连接后 `turbo_media_track_start` +
  `turbo_media_track_send_speech_frame` 推送 TTS PCM。WHEP 接收侧为每个 call 建立
  独立 recvonly 会话，将解码后的 caller PCM 送入该 call 的 ASR。RoomService 从
  权威 room aggregate 精确选择 `owner_participant_id == call_id` 的唯一未静音音轨，
  先提交 desired subscription，再创建 `<call_id>-rx` receiver；SFU 在 receiver
  session 就绪时原子应用订阅。`test_ivr_whip_transport` spawn 真实 sfu_node，使用
  WHIP 的真实 publish SSRC 注册 caller track，验证 ICE/DTLS/SRTP 建连、PCM 经 SFU
  转发到 WHEP、接收端成功解码并触发回调，且 rejected frame 为 0。
- [x] **SFU + 信令最小环境（`test_sfu_minimal_environment`）**：spawn 真实
  `sfu_node` 可执行文件（`[ice] allow_loopback`），经控制 API `attach_room`
  预置房间，再对标准 **WHIP**（发布，sendonly 音频 offer）与 **WHEP**（订阅，
  recvonly）端点 POST SDP offer，断言 `201` + 有效 answer（`m=audio`、
  SFU 自己的 `a=ice-ufrag`、`a=setup:`）。验证了 bot 媒体传输要接入的信令路径
  （SDP offer/answer + 媒体会话创建）在真实 SFU 上可用。该用例只覆盖信令最小
  环境；实际 ICE/DTLS/SRTP 双向媒体字节由 `test_ivr_whip_transport` 覆盖。
- [x] **worker.sync 身份绑定（连接事件级）**：探针验证 ROUTER 的
  `PEER_CONNECTED` 事件可拿到 DEALER 配置 identity（`message_identity` 在 ROUTER
  接收侧不可用 -4029，改用 `turbo_flow_fmq_event_fn` 连接事件）。bridge 维护有界
  「已连接 peer 身份」集合（PEER_CONNECTED 增、DISCONNECTED 删，独立锁），
  `worker.sync` 注册要求声称的 `worker_id` 有活跃连接，否则 `IVR_EAUTH` 拒绝
  （不注册、不存 dispatch route）。这是防「未连接身份冒用」的纵深防御；完整防伪
  需传输层 mTLS/WSS 身份（facade 不暴露 ROUTER 侧 per-message peer identity，
  同 identity 多连接无法区分），属后续项。`ivr_worker` readiness 只认
  `WorkerSyncResultV1` status 0（被拒重试不误报 ready）。
  测试：`test_worker_sync_unconnected_identity_rejected`（bridge 层 `IVR_EAUTH`
  + adapter 层聚合注册表不含该身份；合法连接身份注册正常），e2e/两进程冒烟
  worker.sync 回执正常。
- [x] 运行时需 `turboflow/bin`、`rules_forge/bin`、`turbohttp/bin`（room_service 的
  iris/http_client）、`turbonet/bin`、`turboutils/bin` 与 vcpkg `ssl/crypto`；
  已加入 win-dev-user 预设 PATH。

## 构建与测试（Gate A codegen）

构建要求 `tbe_compiler` 可用（`find_program`，如 turbo-utils 的
`build/Msvc-Release/bin`），CMake 会在构建期用 `tbe_compiler --source-output`
从 `schema/turbomedia_ivr_v1.schema` 生成 owning typed `.h/.c`，编译为
`turbo_media_ivr_schema` 并链接进 `turbo_media_ivr`；`test_ivr_schema_typed`
用生成的 typed 结构体（`ConferenceJoinCommandV1_t` 等）做 JSON 往返与字段断言。
工具缺失时 CMake fail fast，因为 FlowMQ wire 不允许退回另一套 schema 路径。

> 已知边界：typed `to_bin/from_bin` 需要 schema 声明 fixed-first 的显式 wire
> layout（`wire_offset`）；当前 canonical schema 保持架构文档草案的字段顺序
> （string 在前），故 typed BIN 不可用，BIN 稳定性由动态 API 的 golden vector
> 测试覆盖（`test_ivr_schema.c`）。若后续需要 typed BIN，须重排字段并重生成
> golden vector（属 schema 变更，需评审兼容性）。

## 构建与测试

依赖（已安装于 `%PKG_ROOT%`）：`TurboXML`、`TurboUtils`、`FlowMQ` 和 `TurboFlow`。
`CMakeUserPresets.json` 统一提供 `TURBOXML_ROOT` 与 `FLOWMQ_ROOT`；构建命令不再
传入模块专用的 package directory 或 root override。

```powershell
cmake --preset win-dev-user
cmake --build --preset win-dev-user --target test_ivr_frame test_ivr_schema test_ivr_content `
      test_ivr_session test_ivr_worker test_ivr_turboxml
ctest --preset win-dev-user -R test_ivr --output-on-failure
```

## 任务清单

状态约定：`[x]` 表示代码与当前验证环境均已完成；`[ ]` 表示尚未完成，或只完成了
mock/适配层而未达到生产验收。

本节只保留 conference 基线摘要。P0/P1/P2 的唯一进度事实源是
[IVR 生产化 TODO Checklist](../docs/ivr-production-readiness-checklist-zh.md)，设计与错误
语义见 [IVR 生产化缺口解决方案](../docs/ivr-production-readiness-design-zh.md)。

- [x] Gate A schema codegen：`tbe_compiler` 生成 typed `.h/.c` 并通过
  `test_ivr_schema_typed`；TIVR BIN/TEXT protocol round trip 及 BIN golden vector 通过。
- [x] FlowMQ DEALER→ROUTER、PUB/SUB、RoomService aggregate、snapshot recovery、
  worker.sync 和 call dispatch 通过当前 `win-dev-user` 的 18 个相关 CTest。
- [x] 独立 `ivr_worker` 的 dry-run、双进程注册和三进程 dispatch 冒烟通过。
- [x] TurboXML CCXML/VXML/SCXML session、媒体 bot 生命周期和 mock TTS/ASR loopback
  闭环通过。
- [x] WHIP 发布侧与 WHEP 订阅侧 ICE/DTLS/SRTP、SFU 精确 track 订阅和双向 PCM
  媒体测试通过；caller 音频可送入对应 call 的 ASR。
- [x] 远程 TTS/ASR provider（OpenAI-compatible）实现并通过 mock HTTP 协议路径/取消/错误/资源上限测试；真实端点验收需 API key（`IVR_OPENAI_BASE_URL` + `OPENAI_API_KEY`）。
- [ ] P0：per-call speech、可靠 dispatch/lease/ACK、readiness admission、mTLS/WSS。
- [ ] P1：媒体失败/重连、真实 RTP DTMF、可观测性/容量和发布矩阵。
- [ ] P2：咨询转接、录音/CDR、transcript/content rollout、多语言和 typed BIN 迁移。

## 已知限制（已用探针复验）

| 等级 | 证据类型 | 说明 |
|---|---|---|
| LOW | 事实 | `turboxml.dll` 的 SCXML 引擎有两个**确定性**约束，均已由 adapter 处理：① `receive()` 必须在首次 `step()`（触发 `InterpreterImpl::init()`）之后调用，否则 `_externalQueue._impl==NULL` 崩溃（NULL 读）；② `Interpreter::step()` 默认 `blockMs=max`，外部事件队列为空时 `BasicEventQueue::dequeue` 在 condvar 上**永久阻塞**——因此不能对空闲引擎反复 step（此前记录的“plugin+reconnecting 挂起”实为多步 drain 在事件消费完后的空闲 step 阻塞，与插件无关，8 步/事件 10/10 复现、4 步 0/10）。adapter 以 `event_seen + stable` 判定事件处理完成即停止 drain。`<send target="ivr.command">` 通过 plugin `on_execution_point`（`"name":"send"`）捕获，exec point 暴露完整 send 内容（event/target/type/delay/`param.*`），命令名 = send event、`<param>` 转 args JSON（简单引号字面量去引号）。`rtc_session` 已迁移为 SCXML 驱动，测试稳定无挂起。 |
| HIGH→已修复 | 事实 | 安装的 `turbo_parser.dll`（vendored cxml）对空字符串调用 `turbo_xml_set_text(node, "")` 崩溃：`cxml_set_text_value` → `cxml_string_append(...,"",0)` 因 len==0 直接返回、`_raw_chars` 保持 NULL → `_get_literal_type` → `_cxml_is_integer(NULL,0)` 解引用 NULL。已在 turbo-utils 修复（`vendor/cxml/src/core/cxstr.c` 空串也分配缓冲、`cxliteral.c` 判空、`query/cxqapi.c` `_get_literal_type` 判空；另修复 `turbo_parser.c` `turbo_xml_get_text` 对无文本元素返回空串），重建 `turbo_parser.dll` 并替换本机安装。cxml-only 探针与 DataBind round-trip 复测通过，`IvrCommandResultV1` 的 XML round trip 已重新启用。注意：修复在 turbo-utils 源码侧，其他机器需重建/重装该 DLL。 |
| LOW | 事实 | 当前 `win-dev-user` 环境已启用 FlowMQ，相关 18 个 IVR/SFU CTest 全部通过；`win-release-user` 旧 build tree 未注册这些测试，不能作为验收证据。 |
| LOW | 推论 | drain deadline 已强制（`begin_drain` 超时计数 `ivr_worker_drain_timed_out`，超时 session 由 `destroy` 再 join 保内存安全）；inbox 已按 per-event 上限 + 总字节预算拒绝（ENOSPC）。 |
| MED | 事实 | FlowMQ DEALER 网关、ROUTER bridge、PUB/SUB、aggregate、快照恢复、worker.sync、RoomService→worker dispatch、三进程冒烟及 WHIP/WHEP 双向真实媒体均已接入并由 CTest 覆盖。OpenAI-compatible TTS/ASR provider 已通过进程内 mock HTTP 的协议、取消、错误和资源上限测试；尚缺真实外部 provider 凭据验收，以及 worker.sync 完整 mTLS/per-message 身份绑定。 |

## 目录

```
webrtc/ivr/
  include/ivr/ivr_worker.h     公开 C ABI
  src/ivr_frame.c/.h           TIVR frame + 类型表
  src/ivr_content.c/.h         内容包加载/校验
  src/ivr_session.c/.h         per-call session 核心
  src/ivr_worker.c             worker 生命周期
  src/ivr_turboxml_adapter.c/.h TurboXML 适配器
  src/ivr_internal.h           内部共享类型
  src/ivr_thread.c/.h          平台线程/同步封装
  src/ivr_util.c               owned string/command/event
  schema/turbomedia_ivr_v1.schema
  content/conference-greeting/ 内容包
  tests/                       IVR/SFU TinyTest 与集成测试
```
