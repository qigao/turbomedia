# TurboMedia IVR media service

本目录实现 Iris/TurboXML 的媒体执行端，不实现 IVR 业务流程。

## 边界

- Iris 唯一拥有 XML/JavaScript、session、IVR/conference/transfer/call-center
  业务状态和下一步动作选择。
- RoomService 只负责媒体资源寻址、worker route/lease 和 typed command 转发。
- IVR worker 只负责 WebRTC/RTP/PCM、TTS/ASR、DTMF、播放和 input window。
- FlowMQ 只传有界的 typed command/result/event；RTP、PCM 和脚本不经过 FlowMQ。
- 本模块不加载 XML/JS/content archive，不创建 TurboXML interpreter，也不消费业务事件。

```mermaid
flowchart LR
  Iris[Iris: XML + JS + session owner]
  IrisMQ[Iris FlowMQ adapter + durable outbox]
  Room[RoomService: media adapter]
  MQ[FlowMQ typed command/result/event]
  Worker[IVR media worker]
  Media[WebRTC/RTP/ASR/TTS/DTMF]

  Iris --> IrisMQ --> MQ --> Room --> MQ --> Worker --> Media
  Media --> Worker --> MQ --> Room --> MQ --> IrisMQ --> Iris
```

## Wire contract

Canonical schema: `schema/turbomedia_ivr_v1.schema`.

- media commands: type IDs `1011..1016`
- media result: `MediaCommandResultV1`, type ID `2007`
- media event: `MediaEventV1`, type ID `3007`
- `provider_session_id` correlates every media fact with an Iris session.
- `call_generation` fences call reuse; `operation_generation` makes media operations
  idempotent; input commands additionally use `input_generation`.

Legacy dispatch/content workflow messages are not the IVR execution contract. The worker
rejects them and requires the typed media protocol.

## Ownership and concurrency

`ivr_worker_t` owns a bounded array of media-call slots. Its explicit operations are
open, play, begin/end/cancel input and close. The worker owns no business state.

Media callbacks copy events into a bounded MPSC-to-single-consumer queue. The application
owner loop serializes those events to FlowMQ. Queue full returns `IVR_ENOSPC`; shutdown
stops producers, drains owned entries, closes all media calls, then destroys transports.

FlowMQ connection and management callbacks also only publish immutable events into a
bounded control queue. Only the worker owner loop changes connection generation, health
or drain state. Room bridge callbacks are fenced by the full route token; stop closes
acceptance and discards queued commands before joining the owner so restart begins empty.

Room bridge callbacks receive owning `ivr_media_command_result_t` and
`ivr_media_event_t` values on the bridge owner thread after the authenticated route is
matched. These callbacks may forward facts upstream, but must not decide a workflow action.

## Test doubles

- `tests/test_ivr_worker.c` uses a mock media factory/port/event sink to test lifecycle,
  idempotency, deadlines, input windows, capacity and drain.
- `tests/test_ivr_room_bridge.c` uses real loopback FlowMQ and DataBind wire frames, while
  mocking only the upstream observer. It verifies route fencing and that media facts do not
  call the Room business handler.
- `ivr_worker --dry-run` uses a logging media transport for application smoke tests.

These substitutes are test-only. Active non-shadow mode requires configured speech and
SFU dependencies and does not silently fall back to logging media.

## Build and focused verification

```powershell
cmake --preset win-dev-user
cmake --build --preset win-dev-user --target test_ivr_worker
cmake --build --preset win-dev-user --target test_ivr_room_bridge
cmake --build --preset win-dev-user --target ivr_worker room_service
ctest --preset win-dev-user -R "test_ivr_worker|test_ivr_room_bridge|test_ivr_flowmq" --output-on-failure
```

## Production provider boundary

Iris 与 RoomService 之间只使用 typed FlowMQ provider lane：command 必须收到 durable
receipt，completion/event 必须收到 application ACK；transport send 成功不等于 Iris 已提交。
断线期间 event 先进入 TurboDB ORM durable outbox，发现 sequence gap 时通过 query/observation
恢复。不存在 HTTP provider fallback，也不得从 broker callback 直接推进 workflow。

WHIP/WHEP 是独立 media-edge 协议：信令经 `TurboHttp::TurboHttp`，生产仅允许验证过的 HTTPS
（可选 mTLS），明文只允许显式 loopback 测试。动态 room/call/participant ID 必须按单个 URL
segment 编码；Opus SDP 的 RTP clock 固定为 48000 Hz，与 PCM 处理采样率解耦。

完整状态归属、关闭顺序、容量与恢复契约见
[`../docs/ivr-production-readiness-design-zh.md`](../docs/ivr-production-readiness-design-zh.md)，
部署容量见 [`../docs/ivr-capacity-and-operations-zh.md`](../docs/ivr-capacity-and-operations-zh.md)。
