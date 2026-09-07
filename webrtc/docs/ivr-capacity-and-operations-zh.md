# IVR 容量报告与告警 Runbook

## 状态与适用范围

- 状态：容量输入已固化；`C_target=4` 的准入测试已完成，真实 60 分钟 soak 和部署签署未完成。
- 范围：单个 `ivr_worker` 进程、per-call speech/media/session，以及 RoomService assignment。
- 证据日期：2026-08-10。
- 规则：本文件区分代码事实、可复算容量和待实测假设；CI smoke 不替代生产硬件 soak。

## 有界数据路径

| 路径 | owner / 拓扑 | 容量与所有权 | 满载与关闭语义 |
|---|---|---|---|
| session event inbox | 单 session control thread 消费，多 callback 复制入队 | 8 项；默认 byte budget 为 `8 * 65536 = 524288` bytes；成功后 session 拥有副本 | 满时 `IVR_ENOSPC`，不覆盖旧项；terminal/drain 唤醒 owner，join 后释放 |
| media supervisor | 单 supervisor thread 消费，多 transport callback 复制固定 state | 16 项内联 state | 满时计 overflow、置 stop，返回 `IVR_ENOSPC`；不 `DROP_OLDEST` |
| worker reply mailbox | worker owner loop 消费，CHTTP H1 WebSocket callback 复制 frame | 16 项，每项上限为 frame header + 64 KiB；暴露 item/byte current/high-water | claim 失败计 queue-full；关闭先停止 producer，再 destroy 并将 current gauge 归零 |
| provider | 每 call 一个 TTS thread 和一个 ASR thread | TTS input 64 KiB；ASR PCM 4 MiB；每个 provider response 16 MiB | 超限明确拒绝；cancel/quiesce 后释放，factory snapshot 必须归零 |
| RTP/PCM data plane | WebRTC/media owner；不经 CHTTP H1 WebSocket | 由 peer、jitter/history 和 provider buffer 各自有界 | control/event queue 满载不得使 RTP/PCM 改走 CHTTP H1 WebSocket |

## 默认容量输入

`事实`：示例配置和 app 默认 `C_target = max_sessions = 4`。每个已接纳 call 创建两个
persistent provider thread、一个 WHIP publish direction 和一个 WHEP receive direction。
peer/ICE 的实际 socket 数取决于候选、TURN 和网络路径，当前 API 没有可签署的固定 socket
上限，因此必须在目标网络实测，不能写成常量。

`计算`：默认 16 kHz、mono、16-bit PCM 的速率为：

```text
pcm_bytes_per_second = 16000 * 1 * (16 / 8) = 32000 bytes/s
max_asr_audio_window = 4194304 / 32000 = 131.072 seconds
```

默认 provider HTTP request timeout 为 30 秒；它限制已经开始的 HTTP request，不等同于
131.072 秒的 ASR PCM accumulation window。

`计算`：只计算代码中已有硬上限、并允许 TTS/ASR response 同时滞留时，每 call 的 configured
retained budget 下界为：

```text
session inbox       524288
TTS queued input     65536
ASR PCM            4194304
TTS response      16777216
ASR response      16777216
--------------------------------
known lower bound 38338560 bytes = 36.5625 MiB/call
```

`推论`：`C_target=4` 的上述下界为 146.25 MiB。该数值不含 WAV/multipart 临时副本、HTTP
request body、TurboXML/session 对象、WHIP/WHEP peer、ICE/DTLS/SRTP、线程栈和库 allocator
overhead，因此不能作为进程 RSS 上限。最终容量报告必须同时记录 RSS/commit、thread、handle/
socket、peer、queue high-water 和 provider retained-byte high-water。

峰值 domain event rate 尚无真实 workload 证据。容量签署前必须记录每 call 与进程总
events/s，以及最长 consumer stall；用
`ceil(peak_events_per_second * stall_seconds) + in_flight_batch` 复算 inbox/mailbox 容量。

## 自动化验收

`test_ivr_worker` 的 `120 percent burst rejects only excess call` 固定执行：

1. `C_target=4`，连续发起 `ceil(1.2 * 4)=5` 个不同 call。
2. 前 4 个成功，第 5 个只返回 `IVR_ENOSPC`。
3. 第 1 个已接纳 call 仍可接收事件，active session 保持 4。
4. drain 后 active session 与 mock media instance 均为 0。

该测试证明 admission、隔离和 drain 契约，不证明真实 provider/media 吞吐。

60 分钟 soak 必须在 Release、目标硬件和目标网络执行；固定 `C_target` 活跃 call，并包含
TTS、ASR、DTMF、WHIP/WHEP 与 CHTTP H1 WebSocket heartbeat。报告至少每分钟保存：active/reserved、
RSS/commit、thread/handle/socket、peer、各 queue item/byte high-water、provider retained bytes、
P50/P95/P99 和全部拒绝/错误计数。通过条件为无非预期 admission failure、queue full、
deadlock/crash，drain 后 session/provider thread/peer/lease/retained bytes 全部归零。

## 延迟指标契约

worker metrics 使用固定桶 `1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000,
10000, 30000 ms, +Inf`。采集只做 relaxed atomic add；scrape 时累积为 Prometheus histogram。
唯一 label 是 Prometheus 固定 `le`，不接受 room/call/message/provider text。

已接真实因果链：

- `dispatch_duration_seconds`：已解码且 target worker 匹配，到 dispatch result 交给 gateway。
- `event_to_inbox_duration_seconds`：domain/command-result callback 收到 event，到 session inbox 接纳；失败入队不观察 latency，由错误 counter 表达。
- `tts_provider_duration_seconds`、`asr_provider_duration_seconds`：provider worker 取出 job，
  到 HTTP、解析/重采样和 provider callbacks 全部返回；可选 Observer 在无 provider lock
  状态通知，生产 observer 只做 relaxed atomic observe。

`dtmf_to_cancel_duration_seconds` 与 `asr_to_command_duration_seconds` 也已接真实因果链：
callback first-final-wins 接纳点保存 monotonic timestamp、source 和 input generation；
TurboXML adapter 通过 session-owner 完成 hook，仅在媒体 cancel 或 gateway command submit
成功后观察。duplicate、stale、loser、timeout 和失败副作用不产生样本。

峰值事件率从已提交 event 的 histogram `_count` 计算，不增加动态 label。例如：

```promql
rate(turbo_ivr_worker_event_to_inbox_duration_seconds_count[1m])
max_over_time(rate(turbo_ivr_worker_event_to_inbox_duration_seconds_count[1m])[60m:1m])
```

第一行是每个 scrape target 的 1 分钟 events/s，第二行是最近 60 分钟内该 1 分钟速率的
峰值。容量签署必须保存查询窗口、scrape interval 和 target 数；多个 worker 的进程总量在
Prometheus target label 层聚合，业务代码不添加 worker/room/call label。

## 运行时资源指标口径

- worker：reply queue item/byte current/high-water、media peer current/high-water、drain timeout。
- RoomService：worker/assignment current/capacity/high-water，lease expired、dispatch/release
  timeout，request/peer-event queue current/capacity/high-water/drop/overflow。
- `media_peers` 每个 RTC media instance 计两个 transport peer（WHIP + WHEP），不等于
  ICE candidate pair、TURN allocation 或 OS socket。socket/handle high-water 仍须由底层
  transport 或进程级目标硬件采集提供，当前条目保持未签署。

## 初始告警与处置

以下为 canary 初始阈值，部署方应根据签署后的 SLO 调整。运行时 counter 已具备；正式告警
规则仍须在部署仓库按 scrape target 和错误预算配置并演练。

| 信号 | 初始阈值 | 级别 | 处置 |
|---|---|---|---|
| worker lease expired | active/reserved call 存在时任意 1 次；空闲 worker 5 分钟内 >= 3 次 | HIGH / MED | 冻结新 assignment；核对 CHTTP H1 WebSocket connection generation、heartbeat latency、broker ACL/TLS；不得手工把 expired 改回 ready |
| dispatch timeout | 5 分钟比例 >1% 或连续 3 次 | HIGH | 停止扩大 canary；按 correlation ID 检查 reservation、route、reply queue、worker generation；只由幂等 retry/补偿推进 |
| provider error | 10 分钟比例 >1% 或同 worker 连续 3 次 | MED | 检查 endpoint status、timeout、buffer reject、credential；保持 per-call 隔离，不切 logging provider fallback |
| media retry exhausted | 任意 active call 1 次 | HIGH | 检查 WHIP/WHEP attempt generation、ICE consent/TURN 和 SFU participant；终止仅限该 call，不重建旧 generation |
| drain timeout | 任意 1 次 | HIGH | 停止发布；保存 thread/peer/provider snapshot，等待 quiescence，禁止为赶 deadline 提前 free |
| release timeout | 任意 active call 连续 2 次 | HIGH | 停止新 assignment；检查 release result route、generation 和幂等 message ID，保留重试事实，不手工删 assignment |
| queue drop/overflow | 任一 request drop 或 peer-event overflow | HIGH | 冻结新 assignment；保存 current/capacity/high-water，定位慢 consumer，禁止扩大无界队列或默认丢旧事件 |

告警日志只记录 worker/correlation/call 的受控标识摘要和错误阶段，不记录 token、完整 payload、
transcript 或 provider response。恢复条件必须来自权威 snapshot/ACK，不以日志或 publish success
替代状态提交。

## 容量签署记录

以下字段为空表示尚未签署，不得把本文件状态改为完成：

| 字段 | 结果 |
|---|---|
| CPU / RAM / OS / build commit | 未执行 |
| Release preset / compiler | 未执行 |
| provider / SFU / TURN / CHTTP H1 WebSocket topology | 未执行 |
| peak events/s / max measured stall | 未执行 |
| 60 分钟 P50/P95/P99 | 未执行 |
| peak RSS/thread/handle/socket/peer | 未执行 |
| queue/provider retained high-water | 未执行 |
| failure counters / drain-to-zero | 未执行 |
