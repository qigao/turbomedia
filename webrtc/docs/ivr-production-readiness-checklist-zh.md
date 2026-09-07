# IVR 生产化 TODO Checklist

## 使用规则

- 最后更新：2026-08-11
- 设计契约：[IVR 生产化缺口解决方案](./ivr-production-readiness-design-zh.md)
- 基线架构：[独立 IVR Worker 架构](./ivr-worker-architecture-zh.md)
- `[x]` 只表示代码、自动化测试和要求的验收证据均已完成。
- `[ ]` 表示未实现、仅 mock、仅单机验证或缺少发布证据。
- 每项完成时必须在“证据”栏加入测试、报告或配置链接；不能只写“已验证”。
- P0 全部完成前不能标记 active IVR product ready；P1 全部完成前不能扩大生产流量。

## 状态总览

| Workstream | 等级 | Owner 模块 | 状态 | 发布门槛 |
|---|---|---|---|---|
| IVR-P0-01 per-call speech | P0 | `ivr_worker` / `ivr_openai_provider` | [x] | 两个以上 call 无 BUSY/cancel 串扰 |
| IVR-P0-02 lease 与 dispatch ACK | P0 | RoomService / `ivr_room_bridge` | [x] | 无 silent dispatch success，worker-loss 行为确定 |
| IVR-P0-03 readiness/admission | P0 | `ivr_worker` app | [x] | 缺依赖的 worker 不进入 READY |
| IVR-P0-04 mTLS/WSS 与授权 | P0 | CHTTP H1 WebSocket adapters / RoomService | [x] | 证书身份、scope、replay negative tests 通过 |
| IVR-P1-01 媒体失败与重连 | P1 | WHIP/WHEP / session | [x] | disconnect/reconnect/exhausted 全路径通过 |
| IVR-P1-02 真实 DTMF | P1 | WebRTC media / IVR input adapter | [x] | RTP telephone-event E2E 通过 |
| IVR-P1-03 可观测性与容量 | P1 | worker / RoomService operations | [ ] | 指标、SLO、60 分钟 soak、120% burst 通过 |
| IVR-P1-04 发布矩阵 | P1 | CI / deployment | [ ] | Release、ASan、真实 provider 完整 E2E 通过 |

## 架构收敛

- [x] `ARCH-01` 定义 participant 正交状态归属：membership 由 Room 原生转换表拥有，
  RTC session 由 per-session TurboXML SCXML 拥有，二者不复制事实。
  证据：生产化设计 2.3 节。
- [x] `ARCH-02` 定义 CHTTP H1 WebSocket 边界：participant/worker 与 RoomService 之间的
  command/result/event 不因同机或同进程部署而旁路；broker callback 后复制到 owner queue；
  Room aggregate 内部 timer/control 不绕 broker，RTP/PCM 不进入 CHTTP H1 WebSocket。
  证据：生产化设计 2.2、2.3 节；`ivr_control_gateway.c`、
  `ivr_room_bridge.c`、`ivr_control_adapter.c`；`test_ivr_control_gateway`、
  `test_ivr_room_bridge`、`test_ivr_room_bridge_dedup`、`test_ivr_dispatch_processes`。
- [x] `ARCH-03` TIVR wire 收敛为 BIN/TEXT；TEXT 是 compact UTF-8 JSON，两者共享
  canonical DataBind schema，不提供格式猜测或 fallback。
  证据：`ivr_protocol.c`、`test_ivr_protocol`、生产化设计 2.4 节。
- [ ] `ARCH-04` `RtcSessionWorkflow` 删除 `.hpp/.cpp`，纯 C 实现使用 TurboXML C API 和
  Salts Parser，并通过全部 C API 状态/command/非法输入/cleanup 测试。
  当前证据（2026-08-10）：外部 `turbo-webrtc` 已完成 `win-release-user`、BoringSSL
  probe、`turbo_rtc_session_workflow` 和 `test_rtc_session_workflow` 编译；workflow
  target 已补 `SHARED_CXX` 导出，C wrapper 已有界驱动 TurboXML 初始化和事件宏步。
  但安装的 `uscxml-static` DLL 在 `<send>` execution point 仅返回内部 `id`，不返回
  `target/event/param.*`，导致两条 command 行为测试失败。必须重建/发布与当前 C API
  头和实现一致的 TurboXML package 后重新运行完整 CTest，之前不得勾选完成。
- [x] `ARCH-05` re2c/Lemon 不用于 TIVR JSON/BIN；仅在未来有独立自定义 DSL grammar 和
  AST 需求时重新评估。
  证据：生产化设计 2.4 节。

## 已完成基线

- [x] `BASE-01` per-call TurboXML session，call generation 和 input window 隔离。
  证据：`test_ivr_session`、`test_ivr_turboxml`。
- [x] `BASE-02` per-call media bot、WHIP publisher 和 WHEP receiver 生命周期。
  证据：`test_ivr_media_bot`、`test_ivr_whip_transport`。
- [x] `BASE-03` RoomService 精确选择唯一 caller audio track，不猜 track ID。
  证据：`test_ivr_control_adapter`、`test_ivr_whip_transport`。
- [x] `BASE-04` command result 回灌 session，且不推进 domain sequence。
  证据：`test_ivr_control_gateway`、`test_ivr_session`。
- [x] `BASE-05` domain sequence gap 停止 mutation 并通过权威 snapshot 恢复。
  证据：`test_ivr_session`、`test_ivr_worker_e2e`。
- [x] `BASE-06` session inbox item/byte 上限、drain deadline 和 callback quiescence。
  证据：`test_ivr_session`、`test_ivr_worker`、`test_ivr_media_bot`。
- [x] `BASE-07` live worker 默认通过 CHTTP H1 WebSocket 发 mutation command；client callback 仅复制
  TIVR frame 到固定容量 reply mailbox，由 worker main loop 解码并推进 session；关闭时
  先停止 producer，再销毁 mailbox。
  证据：`webrtc/apps/ivr_worker/main.c`、`test_ivr_dispatch_processes`。
- [x] `BASE-08` CHTTP H1 WebSocket peer disconnect 会失效 server route；worker selection 只接受
  live route。无可用 worker 的 `conference.join` 返回 `IVR_ESTATE + retryable`，不提交
  Room mutation；dispatch/media 失败的新增 participant 会走有界补偿，sequence reservation
  可回滚，失效 registry/route slot 可被 replacement worker 复用。
  证据：`ivr_room_bridge.c`、`ivr_control_adapter.c`、`test_ivr_control_adapter`（无 worker、
  media failure retry、disconnect/replacement）。
- [x] `BASE-09` worker 在 owner loop 完成 session/media 创建后发送
  `CallDispatchResultV2` accepted/rejected ACK；bridge 校验当前 live route 后再通知
  RoomService adapter。join 的 `room.participant.joined` event 以 accepted ACK 为门槛，
  避免 event 先于 worker session 到达而丢失；adapter 维护有界
  `PENDING/ACCEPTED/REJECTED` assignment 观测表，容量满时只复用 terminal slot，不覆盖
  pending。当前 join response 仍 immediate-success，rejected ACK 只观测，不回滚已提交
  participant。
  证据：`ivr_control_gateway.c`、`ivr_room_bridge.c`、`ivr_control_adapter.c`、
  `test_ivr_control_gateway`、`test_ivr_worker_e2e`。
- [x] `BASE-10` RoomService 以有界 worker record 维护 instance/generation、
  `READY/DRAINING/EXPIRED`、active/reserved/max 和 lease；selection 与 reservation 在同一
  锁域提交。worker 使用 `WorkerSyncCommandV2` 与 heartbeat 续租。`conference.leave`
  通过独立 `CallReleaseCommandV1/CallReleaseResultV1` 闭环推进
  `ACCEPTED -> RELEASING -> removed`，只有 accepted release result 才释放 active slot；
  send failure 回滚到 `ACCEPTED`，worker release 幂等销毁真实 session。bridge owner tick
  按配置 deadline 将无 ACK 的 PENDING 标记为 `dispatch_timeout` 并释放 reservation；
  timeout/cancel 后迟到的 accepted ACK 会恢复有界 active 所有权并立即走 fail-closed
  release，不遗留无 assignment 的 worker session。release result 超时后使用同一稳定
  message ID 重发 release command，依赖 worker 幂等销毁完成闭环。
  证据：`ivr_control_gateway.c`、`ivr_room_bridge.c`、`ivr_control_adapter.c`、`ivr_worker.c`、
  `test_ivr_control_gateway`、`test_ivr_control_adapter`、`test_ivr_worker`、`test_ivr_worker_e2e`。

## P0

### IVR-P0-01：per-call speech runtime

Owner：`webrtc/apps/ivr_worker`、`webrtc/ivr/src/ivr_openai_provider.*`

- [x] `P0-01.1` 新增 app-internal `ivr_speech_session_factory_ops_t`，接口仅包含
  `create/destroy/probe`，记录 borrowed/owned 和线程契约。
- [x] `P0-01.2` 将 OpenAI config 移入不可变 factory context；删除进程级
  `g_remote_tts/g_remote_asr` 可取消状态。
- [x] `P0-01.3` 在 `media_factory_create(call)` 内创建独立 TTS/ASR provider，并将实例
  ownership 存入 `ivr_worker_media_instance_t`。
- [x] `P0-01.4` cleanup 顺序固定为 terminal -> bot cancel/quiesce -> transport stop ->
  bot destroy -> provider destroy -> instance free。
- [x] `P0-01.5` provider 创建一半失败时释放已创建部分并返回明确错误，不忽略失败。
- [x] `P0-01.6` 为每 call provider thread、HTTP client、TTS input、ASR buffer 和 response bytes 设置
  配置上限，并纳入 `max_sessions` 容量计算。
- [x] `P0-01.7` 增加双 call 并发 TTS 测试：交错 synthesize，两个 call 均完成且无
  `TURBO_SPEECH_ERR_BUSY`。
- [x] `P0-01.8` 增加双 call ASR 测试：交错 write/finish，transcript 和 callback_user_data
  不串 call。
- [x] `P0-01.9` 增加 cancel 隔离测试：cancel call A 不停止 call B，也不丢失 B 的 final。
- [x] `P0-01.10` 增加 create/destroy/并发 stop 的 ASan 测试，结束后 provider thread 和
  retained bytes 为零。

完成证据：

- [x] `test_ivr_openai_provider` 覆盖 per-call factory、双 TTS、双 ASR、cancel 隔离、
  部分创建回滚、配置上限和资源快照；`win-release-user` 通过。
- [x] `win-dev-user` ASan 下连续创建/销毁 100 个双 call batch，无 sanitizer 错误或
  callback after free，结束时 session/provider/thread/retained bytes 全部为零。
- [x] 设计文档中的 ownership/lifecycle、容量公式与头文件注释一致。

验证记录（2026-08-09）：

```text
cmake --build --preset win-release-user --target test_ivr_openai_provider ivr_worker
ctest --preset win-release-user -R test_ivr_openai_provider --output-on-failure
cmake --build --preset win-dev-user --target test_ivr_openai_provider
ctest --preset win-dev-user -R test_ivr_openai_provider --output-on-failure
```

### IVR-P0-02：worker lease、容量与 dispatch ACK

Owner：`webrtc/apps/room_service`、`webrtc/ivr/src/ivr_room_bridge.*`、IVR schema

- [x] `P0-02.1` 在 RoomService owner context 建立唯一 `ivr_worker_registry_t`；移除
  adapter 内永不失效的裸 worker ID 数组作为事实源。
- [x] `P0-02.2` worker record 包含 instance ID、connection generation、state、capacity、
  active/reserved、capabilities 和 lease deadline。
- [x] `P0-02.3` CHTTP H1 WebSocket connect/disconnect callback 只复制事件到 owner queue，由 owner
  推进 worker 状态和失效 route。
- [x] `P0-02.4` 配置并验证 heartbeat `H`、lease `L`、dispatch deadline `D`，满足
  `L >= 3H` 且 `D < L`。
- [x] `P0-02.5` 新增 `WorkerSyncCommandV2` 和 `WorkerHeartbeatV1` type ID；不修改 V1 ID。
- [x] `P0-02.6` 新增 `CallDispatchCommandV2`、`CallDispatchResultV2`、release command/result
  及 BIN golden vectors。
- [x] `P0-02.7` 建立 `PENDING/ACCEPTED/REJECTED/RELEASING/RECOVERING` assignment 状态机和转换表。
- [x] `P0-02.8` selection 只选择 `READY && active + reserved < max_sessions` 的 worker，
  reservation 与选择在同一 owner context 提交。
- [x] `P0-02.9` worker 仅在 content、media instance 和 session slot 创建成功后 ACK
  accepted；ENOSPC、bad generation、bad content 和 draining 返回结构化 reject。
- [x] `P0-02.10` RoomService 只在 accepted result 后提交 `ACTIVE`；send success 不推进
  assignment。
- [x] `P0-02.11` dispatch timeout/reject 释放 reservation，并以相同 assignment ID、
  新 attempt ID 选择下一个 worker；每个 attempt 使用稳定 message ID 幂等重试。
- [x] `P0-02.12` dispatch result queue 满、result 丢失、重复、乱序和 stale generation 均有
  明确错误/恢复语义。
- [x] `P0-02.13` worker lease 失效时停止新 dispatch，失效 route，并按第一版
  fail-closed 策略提交 `ivr.worker_lost` 和幂等 cleanup。
- [x] `P0-02.14` conference/Room mutation、desired media 和 assignment 的提交/补偿顺序写入
  RoomService API 注释及测试。
- [x] `P0-02.15` 暂不实现无 snapshot 的 mid-dialog 自动恢复；若后续实现，必须新增
  workflow definition version、last sequence、input window 和恢复兼容测试。

完成证据：

- [x] `test_ivr_control_adapter` 覆盖 worker lease/capacity、generation、health admission、
  deadline、release 和 stale registration。
- [x] `test_ivr_control_gateway`、`test_ivr_schema` 覆盖 V2 command/result BIN 编解码与 golden vector。
- [x] `test_ivr_dispatch_processes` 使用真实 CHTTP H1 WebSocket V2 worker probe 覆盖 dispatch 前、ACK 前、
  ACK 后 worker 退出，以及 replacement worker 重派和 orphan cleanup；真实 `ivr_worker`
  的 session/media ACK 由 `test_ivr_worker_e2e` 覆盖。
- [x] 故障后 `reserved_sessions == 0`，且不存在成功但无 owner 的 assignment。

实现注记：V2 worker instance/generation fencing、bounded attempt records、stable message ID
与新 attempt ID、timeout/reject reservation release、worker-loss fail-closed cleanup、fresh
registry restart rejection 和 control queue overflow fail-closed 均已落地。无 assignment
snapshot 的 mid-dialog 自动恢复仍不提供，必须通过显式 terminal/fail-closed 收敛。

验证记录（2026-08-09，均为独立 CTest invocation）：

```text
cmake --build --preset win-release-user --target room_service ivr_worker \
  test_ivr_protocol test_ivr_schema test_ivr_control_gateway test_ivr_room_bridge \
  test_ivr_control_adapter test_ivr_worker_e2e test_ivr_dispatch_processes
ctest --preset win-release-user -R '^test_ivr_control_adapter$' --output-on-failure
ctest --preset win-release-user -R '^test_ivr_dispatch_processes$' --output-on-failure
```

上述两项及协议/schema/CHTTP H1 WebSocket/E2E 独立测试通过。CTest preset 已显式注入 release runtime
PATH；`test_ivr_room_bridge`、`test_ivr_control_adapter`、`test_ivr_worker_e2e`、
`test_ivr_dispatch_processes` 组合运行 4/4 通过。

ASan 回归（2026-08-10）：

```text
cmake --build --preset win-dev-user --target \
  test_ivr_content test_ivr_control_adapter test_ivr_dispatch_processes
ctest --preset win-dev-user -R \
  "test_ivr_content|test_ivr_control_adapter|test_ivr_dispatch_processes" \
  --output-on-failure
```

3/3 通过，无 sanitizer 报告。

### IVR-P0-03：readiness、配置与 admission

Owner：`webrtc/apps/ivr_worker`

- [x] `P0-03.1` 定义线程安全 health snapshot：config/content/schema、authenticated
  command/event channel、sync、speech、SFU、draining、capacity。
- [x] `P0-03.2` content manifest 声明 required capabilities；active mode 在依赖缺失时
  启动失败或保持 not-ready。
- [x] `P0-03.3` 删除 active mode 中的隐式 NULL provider/logging transport 路径；只允许
  dry-run/shadow 显式使用测试 transport。
- [x] `P0-03.4` `remote_speech_init` 返回错误并由 main 消费；不得记录“ignored”后继续
  注册 ready。
- [x] `P0-03.5` 将 CLI/env 解析归一化为 validated config，替换无范围检查的 `atoi`。
- [x] `P0-03.6` 配置优先级为 CLI > env > TOML > default；secret 不写入示例明文配置。
- [x] `P0-03.7` 增加 `/live`、`/ready` 和只读 health JSON；默认管理端口只绑定 loopback。
- [x] `P0-03.8` readiness false 立即停止新 reservation；已有 call 由显式 drain/failure
  policy 决定，不在 health callback 中销毁。
- [x] `P0-03.9` status/heartbeat 将 health generation 和 capacity 发送到 RoomService。

完成证据：

- [x] 缺 content、schema、speech、SFU、command channel、event channel 的 negative tests
  均返回 not-ready。
- [x] shadow worker 从不进入 active selection pool。
- [x] readiness 恢复和失效不会产生重复 registration 或旧 generation 状态覆盖。

当前实现注记：management listener 使用独立 CHTTP/CNet owner thread，默认仅绑定
`127.0.0.1:18081`，dry-run 不监听端口。CHTTP H1 WebSocket callback 只复制 peer connection 状态到
原子标志，worker owner loop 才推进 health；command/event 任一断开会撤销 readiness，
command 仍连通时立即发送一次 not-ready heartbeat。health update 以 generation 作为
optimistic token，统一派生 dependency、draining 和 capacity readiness，拒绝旧快照覆盖。
TLS transport 的认证身份由 P0-04 提供；P0-03 只消费 CHTTP H1 WebSocket 已验证的连接结果。

已验证：`test_ivr_worker_health` 覆盖 dependency negative matrix、容量、generation
翻转和 stale update；`test_ivr_worker_http` 使用真实 listener 覆盖 `/live` 200、
`/ready` 200/503、draining、只读 JSON、loopback 拒绝和 stop/join；
`test_ivr_room_bridge` 使用真实 CHTTP H1 WebSocket server/client 覆盖单一 typed channel 的
connect/disconnect、双向 route 和身份冒用拒绝；
`test_ivr_content`、`test_ivr_control_adapter` 的 `health.shadow`/无 `health.ready`
worker negative case、`turbo_media_test_ivr_worker_dry_run`（Release）。
`ivr_worker` 的 `--config`、`IVR_*` 环境变量、CLI 解析顺序实现为
`CLI > env > TOML > default`；`WorkerSyncCommandV2`/`WorkerHeartbeatV1` 携带
`health_generation`、`health_ready`、capacity 字段，旧 V1 type ID 未改变。
非敏感 TOML 示例位于 `webrtc/apps/ivr_worker/config/ivr_worker.toml.example`；
speech/SFU secret 仍只从环境变量读取。

2026-08-24 Release 验证：`test_ivr_control_gateway`、`test_ivr_room_bridge`、
`test_ivr_worker_e2e`、`turbo_media_test_ivr_worker_dry_run`、
`test_ivr_worker_health`、`test_ivr_worker_http` 共 6/6 通过。

### IVR-P0-04：mTLS/WSS identity 与授权

Owner：CHTTP H1 WebSocket gateway/bridge、RoomService certificate identity adapter

- [x] `P0-04.1` gateway 和 bridge config 使用独立 CHTTP H1 WebSocket 的 client/server TLS object，
  增加 CA bundle、cert、key、server name 与 peer verification；缺少对象配置时 fail closed。
- [x] `P0-04.2` active mode 仅允许 TLS/WSS；TCP/WS 只允许显式 test/loopback/trusted
  boundary 配置，禁止自动 fallback。
- [x] `P0-04.3` RoomService 从验证后的 canonical certificate fingerprint 映射 worker ID；payload
  worker ID 必须精确匹配。
- [x] `P0-04.4` 为 worker 配置 tenant/room/call scope 与 content capability ACL；
  连接成功不绕过 command authorization。
- [x] `P0-04.5` command/result/status 校验 deadline、instance ID、connection generation 和
  stable message ID。
- [x] `P0-04.6` replay/dedup cache 设置 item/time 上限，过 retention window 的 mutation
  明确拒绝。
- [x] `P0-04.7` 支持 active/previous CA 或身份映射的有界证书轮换，不接受无限旧证书。
- [x] `P0-04.8` 日志脱敏：token、key、证书正文、完整 ASR 文本和音频不得输出。

完成证据：

- [x] 无证书、错误 CA、过期证书、hostname mismatch、worker ID mismatch 全部连接失败。
- [x] 跨 tenant/room/call、错误 topic、错误 content capability 的命令不改变 Room version。
- [x] replay、旧 generation 和轮换窗口前后测试通过。

P0-04.1 完成证据（事实，2026-08-24）：TurboMedia 直接链接独立
`Salts::CHTTP`，gateway 使用 typed client endpoint，Room bridge 使用 typed server
endpoint；两者在 create 时复制证书/密钥配置，并在 TLS/WSS CONNECT/BIND 前配置 CNet。
内部 command/result/event/query 共用一个身份约束的 server/client channel，不再创建
独立 PUB/SUB endpoint。`test_ivr_control_gateway`、`test_ivr_room_bridge`、
`test_ivr_room_bridge_dedup` 与 `test_ivr_control_adapter` 覆盖 codec、route、身份绑定与去重；
不得使用进程级 `TURBONET_TLS_*` 环境变量冒充多 endpoint 配置，也不得在 TLS 失败时
自动回退 TCP/WS。

P0-04.2/04.3/04.7 完成证据（事实，2026-08-10）：`webrtc/ivr/src/ivr_certificate_identity.*`
提供有界的 `fingerprint -> worker_id` owner。输入要求 `sha256:<64 lowercase hex>`、非空
worker ID、正 generation；同一 worker/fingerprint 的重复映射 fail fast。BIND callback
只比较已由 CNet 验证的 fingerprint 与 claimed identity，不执行网络或 Room mutation。
每项可配置 active 与 previous fingerprint，previous 在注入时钟达到 expiry 时立即失效；
`test_ivr_certificate_identity` 覆盖 active、previous expiry、错误 identity、格式错误和
重复映射。RoomService 配置解析并验证 TLS material 和 bounded worker identity 数组，
server owner 将 certificate verifier 注入 server；worker client 使用显式 CONNECT identity。
每条 server 消息携带 peer identity，bridge 要求其与 claimed worker ID 完全相同。
`test_ivr_certificate_identity` 覆盖轮换窗口，Room bridge/adapter loopback 测试覆盖冒用拒绝；
`turbo_media_test_ivr_worker_rejects_active_plaintext` 验证 active worker
不会回退 plaintext；`turbo_media_test_room_service_config` 验证 plaintext 仅限显式 loopback。

P0-04.8 完成证据（事实，2026-08-10）：IVR/RoomService 日志审查未发现 token、key、证书
正文、ASR transcript 或音频 payload 输出；worker 不再输出完整 speech provider URL 或外部
release reason，RoomService config summary 不再输出完整 SFU control URL/registry。配置只报告
`configured/disabled`，认证 secret 仍只报告启用状态。`rg.exe` 敏感字段与所有
`printf/fprintf/TLOG_*` 调用点已人工交叉检查。进程级 CTest
`turbo_media_test_ivr_worker_log_redaction` 和
`turbo_media_test_room_service_log_redaction` 分别向 URL、API key、node registry 与 auth
secret 注入唯一标记，并通过 `FAIL_REGULAR_EXPRESSION` 验证 stdout/stderr 不含标记；Release
2/2 通过。独立 CHTTP H1 WebSocket 迁移后的 focused codec、server/client、dedup、adapter 与
SQLite ORM 测试另见 2026-08-24 验证记录。

P0-04.4/04.5/04.6 完成证据（事实，2026-08-11）：

- `P0-04.4`：新增 `webrtc/ivr/include/ivr/ivr_acl.h`（`ivr_acl_scope_allows` /
  `ivr_acl_tenant_allows`，tenant 采用 `<tenant>/` room_id 前缀约定）。RoomService
  `[fmq.workers]` 每项可配置 `tenant_id` / `room_scope` / `call_scope` /
  `content_capabilities`（`config.c`、`room_service.toml.example`），
  server 注入 adapter 的 worker ACL；adapter 在 `ivr_control_adapter_apply` 与
  `ivr_control_adapter_on_command` 入口做 command authorization（未配置 ACL 时保持
  legacy allow-all，配置后未列名 worker fail closed），dispatch selection 与 retry
  只在 ACL 覆盖 room/call 与 content package 的 worker 间进行，且命令被拒绝不改变
  Room version（新增 `acl_rejects` 计数）。CHTTP H1 WebSocket transport identity 只负责连接身份，
  tenant/room/call/content authorization 仍由 adapter 的 worker ACL 负责。
  worker 侧 `ivr_worker.toml.example` 与 `main.c` 增加同名字段，dispatch 收包时校验
  scope（`worker_dispatch_in_scope`）并拒绝 `IVR_EAUTH`。
- `P0-04.5`：worker 在 dispatch 收包时按 `deadline_timeout_ms` 相对 TTL 求值
  （`ivr_control_gateway_dispatch_deadline_ok`），过期以 `IVR_ESTALE` +
  `dispatch.deadline` 结构化拒绝；V2 worker instance/connection generation fence、
  stable message_id 幂等与 RoomService 侧 dispatch result 的 assignment/attempt/
  instance/generation/max_sessions 校验保持既有实现并纳入回归。
- `P0-04.6`：bridge dedup cache 增加 `stored_at_ms`、可注入 clock 与
  `dedup_retention_ms`（默认 60000ms），store 时先驱逐过期项，lookup 在窗口内返回缓存
  结果、窗口外以 `IVR_ESTALE` 显式拒绝（新增 `dedup_expired_rejects` 计数），容量仍受
  `dedup_capacity` 环缓冲约束。

验收证据：`test_ivr_acl`（scope/tenant 规则）、`test_ivr_control_adapter`
（`test_apply_acl_scope_denies_cross_tenant_room_call` 跨 tenant/room/call 命令不改
Room version、`test_live_acl_blocks_out_of_scope_join` 连接成功不绕过授权）、
`test_ivr_room_bridge_dedup`（窗口内幂等、窗口外 `IVR_ESTALE`）、`test_ivr_control_gateway`
（dispatch deadline TTL）、`turbo_media_test_room_service_config`（新 worker ACL 字段
解析）。Release 与 ASan（win-dev-user）下 `test_ivr*` 与 RoomService 进程级测试全部通过。

## P1

### IVR-P1-01：媒体失败事件与重连

Owner：`ivr_whip_transport`、`ivr_whep_transport`、worker media adapter、session

- [x] `P1-01.1` WHIP/WHEP 增加版本化 `ivr_media_state_fn` callback，携带 call ref、
  attempt generation、state 和 error code。
- [x] `P1-01.2` callback 只复制到 per-call inbox；不在 callback 中执行 HTTP、TurboXML、
  destroy 或外部 callback chain。
- [x] `P1-01.3` WHIP 和 WHEP 都处理 CONNECTED、DISCONNECTED、FAILED、CLOSED；修复 WHIP
  只设置 connected、不清理失败状态的问题。
- [x] `P1-01.4` control loop 实现有界 reconnect attempts、backoff 和总 deadline。
- [x] `P1-01.5` stale attempt callback 和旧 audio frame 按 generation 拒绝并计数。
- [x] `P1-01.6` 两条必需 link 恢复后才发 `rtc.reconnected`；耗尽后发
  `rtc.retry_exhausted` 并触发 terminal latch。
- [x] `P1-01.7` 增加 WHEP RTP inactivity deadline 和 `media.input_stalled`，与网络断线
  分开统计。
- [x] `P1-01.8` reconnect/stop/destroy 的 callback barrier 和所有失败 cleanup 路径通过
  并发测试。

完成证据：

- [x] 新测试 `test_ivr_media_reconnect` 覆盖成功、耗尽、stale generation、input stall。
- [x] `test_ivr_turboxml` 验证 `media.input_stalled` 进入 reconnecting，`provider.error`
  只驱动当前 call 进入 closing；两者均为 sequence=0 derived media fact。
- [x] `test_ivr_whip_transport` 通过 dangerous control command 按 participant 精确删除
  owned media session；WHEP 单侧断开时 WHIP 保持连接，WHEP 以新 attempt generation
  恢复；随后 WHIP 单侧断开时 WHEP 保持连接。Release 实测总耗时约 80 秒，包含两次
  SaltsNet::ICE consent expiry（单侧上限 40 秒）与一次 WHEP 重建。

### IVR-P1-02：真实 RTP DTMF

Owner：WebRTC media engine、WHEP/input adapter、IVR session

- [x] `P1-02.1` 复用或扩展媒体引擎解析 RTP `telephone-event`，不在 XML 层解析 RTP。
- [x] `P1-02.2` 定义 canonical DTMF input：call ref、input ID、digit、duration、source
  generation 和 event end。
- [x] `P1-02.3` 允许字符仅为 `0-9*#A-D`；非法 payload、duration 和未结束 event 拒绝。
- [x] `P1-02.4` 重复 RTP end packet按 call + event generation 去重。
- [x] `P1-02.5` 新增版本化 input-window hook；session begin/end input 通知 per-call media，
  不无版本扩展现有公开 struct。
- [x] `P1-02.6` 没有活动 window、迟到 window/generation 的 DTMF 拒绝并计数。
- [x] `P1-02.7` DTMF 与 ASR 复用 first-final-wins 规则，不推进 domain sequence。
- [x] `P1-02.8` SIP/外部 telephony 来源如后续接入，必须转成同一 canonical input，不增加
  第二套状态机。

完成证据：

- [x] 新测试 `test_ivr_dtmf_rtp` 用真实 RTP packet 验证 digit/end/dedup/invalid；
  `test_ivr_whip_transport` 用真实 WHIP -> SFU -> WHEP telephone-event 验证 end dedup。
- [x] `test_ivr_worker_media_loop` 在同一活动 input window 并发提交真实 RTP DTMF 和
  media-bot ASR final，验证 loser 返回/触发 `IVR_ESTATE` 且只产生一个 conference command；
  Release 连续 5 次及 ASan 通过。

### IVR-P1-03：metrics、SLO、容量与 soak

Owner：worker diagnostics、RoomService operations、CI benchmark

- [x] `P1-03.1` 暴露 worker/assignment/queue/media/provider/recovery/drain 指标，名称和
  label cardinality 文档化。
- [x] `P1-03.2` metrics label 禁止 room/call/message ID；这些 ID 只用于采样日志和 trace。
- [x] `P1-03.3` media hot path 使用 atomic/batch，不在 frame callback 格式化 INFO 日志。
- [ ] `P1-03.4` 定义并记录 `C_target`、峰值事件率、最大 ASR stall、每 call 内存/线程/
  socket 预算。
- [ ] `P1-03.5` 建立 P50/P95/P99：dispatch、event-to-inbox、DTMF-to-cancel、ASR-to-command、
  TTS/ASR provider latency。
- [ ] `P1-03.6` `C_target` 持续 60 分钟，无 admission failure、queue full、deadlock、crash、
  session/provider/peer 泄漏。
- [x] `P1-03.7` `120% * C_target` burst 明确拒绝新 call，不破坏已接纳 call。
- [x] `P1-03.8` 建立 worker lease expired、dispatch timeout、provider error、media retry
  exhausted 和 drain timeout 告警阈值及 runbook。

完成证据：

- [x] `test_ivr_worker_http` 验证 `/metrics` gauge/counter/histogram、除固定 `le` 外无动态
  label，且 ID 不泄漏；
  `test_ivr_media_bot` 验证 TTS provider failure 归一化为 `provider.error`，Release 通过。
- [x] `test_ivr_worker` 以 `C_target=4`、5 次 burst 验证前 4 个接纳、第 5 个
  `IVR_ENOSPC`、既有 call 仍可接收 event，drain 后 session/media instance 为零。
- [x] 六组固定 histogram 均已接真实 monotonic clock 因果链。DTMF/ASR final 在 callback
  首次接纳时记录 source/generation/timestamp，由 session owner 在成功 cancel/command 完成
  hook 观察；duplicate、stale、loser、timeout 和失败副作用不产生样本。
- [x] worker 暴露 reply queue item/byte current/high-water、media peer current/high-water 和
  drain timeout；RoomService 暴露 worker/assignment current/capacity/high-water、lease expired、
  dispatch/release timeout 及 request/peer-event queue current/capacity/high-water/drop/overflow。
- [ ] 容量输入、可复算下界、60 分钟采集字段和初始告警处置已写入
  [容量报告与告警 Runbook](./ivr-capacity-and-operations-zh.md)；事件率可从 histogram
  `_count` 计算，峰值 workload、OS socket 上限和真实硬件 high-water 尚未签署。
- [ ] 保存容量输入、测试硬件、配置、P50/P95/P99、资源 high-water 和失败计数报告。
- [ ] soak 结束后 active/reserved session、provider thread、lease、peer 和 retained bytes
  全部归零。

本轮验证记录（2026-08-10）：

```text
win-release-user:
  test_ivr_openai_provider  18/18, 460 assertions
  test_ivr_worker          14/14, 88 assertions
  test_ivr_worker_http      3/3, 49 assertions
win-dev-user + ASan:
  test_ivr_openai_provider  18/18, 460 assertions
  test_ivr_worker          14/14, 88 assertions
  test_ivr_worker_http      3/3, 49 assertions
```

ASan executable 直接启动最初返回 `0xc0000135`；`dumpbin /dependents` 证明缺少测试进程
`PATH` 中的 `turbo_media_speech.dll` 与 `clang_rt.asan_dynamic-x86_64.dll`。按 preset 规范只在
测试进程补充 `turbomedia/bin`、依赖 bin 和 MSVC ASan runtime 后通过，未向 CMake 增加 DLL
复制规则。以上仍是功能/内存安全 smoke，不是 60 分钟 soak。

本轮指标补充验证（2026-08-10，`win-release-user`）：

```text
test_room_service_app   Passed 67.79 sec
test_ivr_control_adapter    Passed 15.49 sec
test_ivr_worker_http    Passed  2.05 sec
3/3 passed
```

`media_peers` 的口径是每个 RTC media instance 的 WHIP + WHEP transport peer 数，不是
ICE candidate、TURN allocation 或进程 OS socket 数；不得用该 gauge 关闭 socket 预算条目。

### IVR-P1-04：发布配置和端到端矩阵

Owner：CI、deployment、IVR integration tests

- [x] `P1-04.1` 用 `win-release-user` fresh configure/build/test 当前 IVR/Room/SFU 组。
- [x] `P1-04.2` 用 `win-dev-user` ASan 执行并发 speech、dispatch failure、media reconnect
  和 shutdown tests。
- [ ] `P1-04.3` 建立 RoomService + SFU + ivr_worker + 真实 OpenAI-compatible endpoint 的
  完整 call E2E，不把 mock HTTP 当作外部 provider 验收。
- [ ] `P1-04.4` 验证 provider 401/429/5xx、timeout、malformed audio/transcript、证书错误和
  cancel；错误只影响对应 call。
- [ ] `P1-04.5` 验证 TURN-only、IPv6、丢包/抖动/重排、ICE restart 和 SFU rolling restart
  下的 IVR 行为。
- [ ] `P1-04.6` 验证 graceful drain、hard kill、RoomService restart 和 worker restart。
- [ ] `P1-04.7` 保存部署示例：active/shadow、TLS、secret、capacity、health、metrics 和
  rollback 配置。
- [ ] `P1-04.8` 完成 canary：只接收 allowlist tenant/queue，新旧 workflow 不共同拥有
  同一个 call 的 mutation 权限。

完成证据：

- [ ] Release、ASan、真实 provider、网络矩阵和故障演练报告有日期、commit、preset、
  配置摘要和结果。
- [ ] 回滚演练不删除 Room/participant/subscription/event 事实，并在 drain deadline 内完成。

2026-08-10 ASan 验证：`win-dev-user` 明确输出 `AddressSanitizer: ON`；
`test_ivr_dtmf_rtp`、`test_ivr_session`、`test_ivr_worker`、
`test_ivr_media_reconnect`、`test_ivr_media_supervisor`、
`test_ivr_worker_media_loop`、`test_ivr_room_bridge`、
`test_ivr_worker_e2e`、`turbo_media_test_ivr_worker_dry_run`、
`test_ivr_worker_health`、`test_ivr_worker_http` 共 11/11 通过；随后
`test_ivr_openai_provider`（并发 speech/cancel/resource cleanup）与
`test_ivr_dispatch_processes`（跨进程 dispatch failure/replacement）2/2 通过。

`P1-04.4` 当前子范围：真实 HTTP mock 已覆盖 401、429、5xx、response byte cap、自然
request timeout、奇数字节 16-bit PCM、缺少 `text` 的 transcript、输入 cap 和 cancel
quiescence；Release/ASan 均为 18/18、460 assertions。奇数字节 PCM 明确归类
`TURBO_SPEECH_ERR_FORMAT`，不再误报 OOM。尚缺真实 TLS 证书错误，因此该项保持未完成。

2026-08-10 fresh Release 验证：`cmake --fresh --preset win-release-user` 成功；构建
`room_service`、`sfu_node`、`ivr_worker` 和 IVR/Room/SFU 关键测试 target 成功。以下 12 项
CTest 100% 通过，总计 89.62 秒：`test_ivr_dtmf_rtp`、`test_ivr_session`、
`test_ivr_worker`、`test_ivr_media_supervisor`、`test_ivr_worker_media_loop`、
`test_ivr_openai_provider`、`test_ivr_control_gateway`、`test_ivr_control_adapter`、
`test_ivr_worker_e2e`、`test_ivr_dispatch_processes`、`test_sfu_node_app`、
`test_ivr_worker_http`。真实 SFU `test_ivr_whip_transport` 另行通过，78.65 秒。configure 的
FFmpeg dev warning 与 build 的 `flv_demuxer_impl.c` C4702 为既有非 IVR warning；本组目标
无新增 warning。

## P2 产品功能

这些项目不阻塞 conference 基线的 P0/P1 修复，但不能宣传为已支持：

- [ ] `P2-01` 咨询转接：claim/prepare/complete/rollback、父子状态机、幂等和恢复。
- [ ] `P2-02` 录音/CDR：授权、双向媒体归档、retention、加密、删除和审计。
- [ ] `P2-03` transcript：敏感信息脱敏、访问控制、retention 和导出。
- [ ] `P2-04` 多租户 content：签名/version、tenant allowlist、canary、原子切换和回滚。
- [ ] `P2-05` 多语言 voice/grammar、locale negotiation 和 package validation。
- [ ] `P2-06` typed BIN fixed-first layout、golden vector、schema migration 和兼容评审。

## 推荐实施顺序

```text
P0-01 per-call speech
  -> P0-03 readiness/config
  -> P0-02 registry/dispatch ACK
  -> P0-04 mTLS/authorization
  -> P1-01 media failure/reconnect
  -> P1-02 DTMF
  -> P1-03 capacity/observability
  -> P1-04 release/canary
```

P0-02 schema 设计可与 P0-01 并行，但 active dispatch 切换必须等待 P0-03 readiness。
P1-03 的基础 counter 可提前实现；最终容量签署必须等待 per-call speech 和 reconnect 路径
稳定，否则测到的是旧资源模型。

## 当前回归命令

实现过程中先运行最小相关 target，再扩展：

```powershell
cmake --build --preset win-dev-user --target room_service sfu_node ivr_worker `
  test_ivr_control_gateway test_ivr_session test_ivr_control_adapter `
  test_ivr_worker_e2e test_ivr_whip_transport test_ivr_dispatch_processes `
  test_sfu_node_app

ctest --preset win-dev-user `
  -R "test_room_service_app$|test_sfu_node_app$|test_ivr_whip_transport$|test_ivr_dispatch_processes$|test_ivr_(room_bridge|control_gateway$|session$|content$|control_adapter$|worker_e2e$)|turbo_media_test_ivr_worker_dry_run" `
  --output-on-failure
```

新增测试 target 未创建前不得把清单中的建议名称放入 CTest filter 并声称通过。

## Product-ready 签署

- [ ] 所有 P0/P1 checkbox 完成并有证据。
- [ ] 没有未关闭 HIGH 问题。
- [ ] 所有 MED 问题有模块 owner 和接受/修复结论。
- [ ] `C_target`、延迟 SLO、错误预算和资源预算已由部署方确认。
- [ ] Release、ASan、真实 provider、mTLS、故障、网络和 soak 报告已归档。
- [ ] canary 与 rollback 演练完成，active 流量扩大获得明确批准。
