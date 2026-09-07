# IVR CHTTP H1 WebSocket Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove FlowMQ from TurboMedia control paths and preserve the existing IVR/Iris domain contract over bounded, authenticated CHTTP HTTP/1.1 WebSocket channels.

**Architecture:** TIVR/DataBind remains the canonical application protocol. A Worker-owned single-owner WebSocket client uses a bounded copied MPSC mailbox; RoomService uses a CHTTP WebSocket route and captured generation-checked sessions while retaining the existing bridge owner queue and domain handlers.

**Tech Stack:** C11, Salts::CHTTP, Salts::CNet, TurboParser::DataBind, TinyTest, CMake Presets.

**Spec:** `docs/design/ivr-chttp-h1-websocket-control-channel.md`

## Global Constraints

- Use HTTP/1.1 WebSocket only; set `CHTTP_PROTOCOL_HTTP_1_1` explicitly and never fallback.
- Preserve published TIVR V1 schema IDs and payload semantics.
- Use `turbomedia.control.v1.bin` as the exact WebSocket subprotocol.
- Every retained queue is bounded by item and byte capacity and rejects overflow explicitly.
- TLS identity and claimed worker/provider identity remain bound at Upgrade admission.
- Production source, active configuration and link targets contain no FlowMQ, TurboNet or TurboHTTP dependency.

---

### Task 1: Freeze the transport-neutral V1 protocol

**Files:**
- Modify: `webrtc/ivr/tests/test_ivr_frame.c`
- Modify: `webrtc/ivr/tests/test_ivr_protocol.c`
- Modify: `webrtc/ivr/src/ivr_frame.h`
- Modify: `webrtc/ivr/src/ivr_protocol.h`

**Interfaces:**
- Consumes: existing `ivr_frame_encode`, `ivr_frame_decode`, `ivr_protocol_encode`, `ivr_protocol_decode`.
- Produces: unchanged V1 golden bytes and documented one-message/one-frame constraints for both WebSocket adapters.

- [ ] Add failing tests rejecting trailing bytes, invalid kind/type pairs, TEXT on the BIN subprotocol boundary, and payloads above the configured message maximum.
- [ ] Run `test_ivr_frame` and `test_ivr_protocol`; confirm each new assertion fails for the missing boundary validation.
- [ ] Add the minimum transport-neutral validation functions without changing published type IDs or schema payload bytes.
- [ ] Re-run the two tests and existing schema semantic/golden tests.

### Task 2: Add the bounded H1 WebSocket transport

**Files:**
- Create: `webrtc/ivr/src/ivr_control_ws.h`
- Create: `webrtc/ivr/src/ivr_control_ws.c`
- Create: `webrtc/ivr/tests/test_ivr_control_ws.c`
- Modify: `webrtc/ivr/CMakeLists.txt`

**Interfaces:**
- Consumes: `chttp_websocket_client_*`, `chttp_server_websocket_*`, `ivr_thread_*`.
- Produces: `ivr_control_ws_client_*` for single-owner client lifecycle and copied send admission; `ivr_control_ws_server_*` for Upgrade admission, copied ingress, captured-session sends and disconnect events.

- [ ] Add a failing loopback test for explicit H1 Upgrade, exact subprotocol selection, binary round trip and application callback delivery.
- [ ] Add failing tests for wrong subprotocol, claimed identity rejection, queue item/byte exhaustion and stale captured session.
- [ ] Run `test_ivr_control_ws`; confirm failures identify the missing transport implementation.
- [ ] Implement the client owner loop, bounded copied mailbox, server route, TLS/identity callback, captured session registry and shutdown order.
- [ ] Re-run `test_ivr_control_ws` and the Salts-adjacent WebSocket integration cases.

### Task 3: Migrate IVR Worker and RoomService bridge

**Files:**
- Rename: `webrtc/ivr/src/ivr_flowmq_gateway.[ch]` to `webrtc/ivr/src/ivr_control_gateway.[ch]`
- Modify: `webrtc/ivr/src/ivr_room_bridge.[ch]`
- Rename: `webrtc/apps/room_service/src/ivr_fmq_adapter.[ch]` to `webrtc/apps/room_service/src/ivr_control_adapter.[ch]`
- Rename tests containing `flowmq` or `fmq` to their `control_ws` equivalents.
- Modify: `webrtc/apps/ivr_worker/main.c`
- Modify: `webrtc/apps/room_service/src/server.c`
- Modify: adjacent CMake files and callers.

**Interfaces:**
- Consumes: Task 2 transport API and existing TIVR codec helpers.
- Produces: `ivr_control_gateway_*`, `ivr_room_bridge_*`, and `ivr_control_adapter_*` with no FlowMQ type in public or private signatures.

- [ ] Rename test-facing APIs first and run their targets to confirm compile failures against the old production symbols.
- [ ] Replace DEALER frame wrapping with one binary WebSocket message containing one TIVR frame.
- [ ] Replace ROUTER route tokens with captured CHTTP WebSocket session handles and authenticated identity records.
- [ ] Preserve peer connect/disconnect events, worker sync route refresh, generation fencing, ACK, dedup and bounded request queue behavior.
- [ ] Run bridge, dedup, adapter, process and shutdown tests until green.

### Task 4: Migrate the Iris provider channel and protocol ownership

**Files:**
- Rename: `webrtc/apps/room_service/src/iris_flowmq_provider.[ch]` to `iris_control_provider.[ch]`
- Rename: `webrtc/apps/room_service/src/iris_flowmq_provider_codec.[ch]` to `iris_provider_protocol.[ch]`
- Rename: adjacent provider tests and process peer helpers.
- Modify: completion dispatcher, outbox, media bridge, reconciler and server callers.

**Interfaces:**
- Consumes: Task 2 H1 client and existing Provider V1 generated codec definitions.
- Produces: transport-neutral `iris_provider_protocol_*` codecs and `iris_control_provider_*` lifecycle/send APIs.

- [ ] Rename codec tests first; confirm compile failure while they still reference FlowMQ-owned headers.
- [ ] Move Provider V1 encode/decode validation behind TurboMedia-owned names without changing encoded bytes.
- [ ] Replace the provider DEALER lifecycle with an H1 WebSocket client owner and exact Iris identity verification.
- [ ] Preserve completion/event durable ACK, query observation and call-offer binding semantics.
- [ ] Run provider codec, provider, completion dispatcher, outbox and reconciler tests.

### Task 5: Replace configuration and dependency ownership

**Files:**
- Modify: root and WebRTC CMake files.
- Modify: `CMakeUserPresets.json`.
- Modify: Worker and RoomService config headers/parsers/examples/tests.
- Modify: `cmake/VerifyNoLegacyNetworkDependencies.cmake`.
- Modify: IVR architecture and operations documentation.

**Interfaces:**
- Consumes: `Salts::CHTTP`, `Salts::CNet`, `TurboParser::DataBind` and the Task 3/4 APIs.
- Produces: `control_ws` and `iris_control_ws` configuration with explicit WS/WSS URI, subprotocol, limits, TLS and reconnect values.

- [ ] Change configuration tests to accept new keys and reject removed FlowMQ keys; run them to verify RED.
- [ ] Update parser, environment variables and examples without compatibility fallback.
- [ ] Remove `FLOWMQ_ROOT`, `FlowMQ::FlowMQ` and FlowMQ include discovery; use exact first-party package roots.
- [ ] Update dependency audit patterns and documentation.
- [ ] Run config tests and the dependency audit.

### Task 6: Verify the migration

**Files:**
- Verify only; fix the owning task when a failure is found.

**Interfaces:**
- Consumes: all prior tasks.
- Produces: reproducible build/test/dependency evidence.

- [ ] Run `git diff --check`.
- [ ] Configure with `cmake --fresh --preset win-release-user` from `VsDevCmd.bat`.
- [ ] Build focused IVR and RoomService test targets, then the complete `win-release-user` build.
- [ ] Run focused CTest filters, then `ctest --preset win-release-user --output-on-failure`.
- [ ] Run `rg.exe` dependency audits for FlowMQ, TurboNet and TurboHTTP and inspect every allowed historical/documentation match.
- [ ] Review ownership, bounds, shutdown and TLS identity requirements against the design specification.

