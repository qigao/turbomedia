# IVR CHTTP H1 WebSocket 控制通道设计

## 决策

TurboMedia 的内部控制面使用 CHTTP HTTP/1.1 WebSocket：IVR Worker 主动连接
RoomService，RoomService 以捕获的 generation-checked WebSocket session 定向回复和推送。
不再使用已退役的消息总线传输，不启用 HTTP/2 WebSocket，也不做协议或传输自动降级。

## 边界

```text
IVR / Iris domain command, result, event
                |
      TurboMedia control protocol
      (TIVR/DataBind payload contract)
                |
      CHTTP H1 WebSocket adapter
                |
             CNet/TLS
```

- TIVR V1 schema、published type id、BIN/TEXT 语义、message id、generation、ACK、
  sequence 和 dedup 行为保持不变。
- 每个 WebSocket binary message 恰好承载一个完整 TIVR frame；不探测格式，不允许
  一个 WebSocket message 拼接多个 frame。
- WebSocket subprotocol 固定为 `turbomedia.control.v1.bin`。
- Worker 身份在 Upgrade 阶段由 `X-TurboMedia-Identity` 提交；RoomService 将其与
  `peer_certificate_sha256` 一起交给配置的 verifier。已认证 session 是 source identity
  的唯一事实源，payload 中的 worker id 必须与 session identity 相等。
- 开发态明文仅允许显式配置的 loopback；生产配置要求 WSS、服务端证书和 mTLS。

## 所有权、线程与背压

### Worker client

- 一个 owner thread 独占 `chttp_websocket_client` 的 connect/send/receive/close/destroy。
- 外部 MPSC producer 调用只把 payload copy 到有界 FIFO；成功入队后队列拥有字节，
  失败时调用方仍拥有输入。
- queue 同时限制 item 和 byte；满时返回 `IVR_ENOSPC`，不阻塞、不丢旧消息。
- CHTTP receive event 是 borrowed view；回调若跨线程保留必须自行复制。

### RoomService server

- CHTTP owner thread 的 Upgrade/message/close callback 只校验并复制输入，然后投递给
  现有 bridge owner queue。
- 捕获的 `chttp_server_websocket_session` 不延长连接生命；route table 保存 worker id、
  session handle 和 connection generation。stale handle 的发送失败会使 route 失效。
- 服务端跨 callback 发送使用 CHTTP copied admission；业务 ACK 不能由 transport send
  completion 替代。
- bridge owner 每 100 ms 检查 CHTTP server terminal 状态。listener 异常终止时先让旧
  peer/route generation 失效，再在固定 bind port 上重建；失败采用 1–30 秒指数退避。
  新 listener 就绪不等于 worker 可调度，必须等待 worker reconnect、推进 generation 并
  重新提交 `worker.sync`/inventory。

### Shutdown

1. 停止新 admission。
2. 关闭 outbound/request queues 并唤醒 owner。
3. 请求 WebSocket close，停止 CHTTP client/server。
4. join owner/bridge threads。
5. 清空 retained payload、session route、codec 和 TLS context。

## 错误语义

- Upgrade 身份、subprotocol、frame type、schema、长度或容量不合法时 fail fast。
- reconnect 后旧 route/session generation 立即失效，必须由新连接的 worker sync 刷新。
- send admission 成功只说明 CHTTP 已复制消息，不说明对端执行成功；领域结果继续使用
  既有 ACK/result 契约。
- 不自动改用 HTTP/2、REST、明文或已退役的消息总线传输。

## 兼容与迁移

- 传输配置统一为 `control_ws` / `control_ws_*`；退役字段被明确拒绝，
  避免看似成功却运行在不同安全语义下。
- 先保持 V1 payload bytes，待 H1 WebSocket 迁移稳定后再单独引入带 payload length 和
  公共 correlation metadata 的 envelope V2。
- 回滚通过切回迁移前构建完成，不在同一进程中保留双 transport fallback。

## 验证契约

- V1 BIN golden vector 与 JSON/BIN semantic round trip 不变。
- 覆盖正确 subprotocol、错误 subprotocol、身份不匹配、binary-only、短帧、超限、
  queue item/byte 满、stale route、重连 generation、listener terminal recovery、
  ACK/dedup、stop/drain。
- 通过依赖门禁确认生产源码、构建与活动配置不再引用已退役的网络栈或消息总线。
