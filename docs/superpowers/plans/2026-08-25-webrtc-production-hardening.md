# WebRTC Production Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 GitHub #2–#12 中影响 WebRTC 协议、安全、线程所有权、关闭恢复、ORM 部署与运行时验证的问题。

**Architecture:** WHIP/WHEP 信令统一经 `TurboHttp::TurboHttp` 同步 facade，并将 TLS、URL 和响应上限封装在 IVR HTTP adapter；FlowMQ callback 只复制不可变事件到有界 owner queue，worker/bridge owner 独占状态迁移。持久化继续通过 `iris_record_store_t` 隔离 TurboDB ORM，TurboMedia 只发布 SQLite runtime，但允许部署方替换为 PG-enabled TurboDB ORM 构建结果。

**Tech Stack:** C11、TinyTest、FlowMQ::FlowMQ、TurboHttp::TurboHttp、TurboDB::ORM、TurboUtils 并发原语、CMake Presets。

**Spec:** `webrtc/docs/ivr-production-readiness-design-zh.md`，GitHub issues #2–#12。

## Global Constraints

- 保留 Iris、RoomService、worker 三层状态所有权，不把业务 workflow 下沉到媒体 worker。
- callback payload 均为 borrowed；跨 callback 或线程前必须复制。
- 所有 control queue 必须有 item/byte 上限；满时返回明确错误或进入 fail-closed shutdown。
- TurboMedia 不直接链接、复制或发布 libpq；默认只发布 SQLite-enabled `TurboDB::ORM`。
- 未显式允许时不从 HTTPS 降级到 HTTP；明文 WHIP/WHEP 仅限显式 loopback 测试模式。
- 当前工作区位于 dirty `master`，保留用户迁移改动，不自动提交或重写历史；每个任务以 focused test 和 `git diff --check` 作为检查点。

---

### Task 1: RFC 7587 Opus SDP 与 URL 输入边界

**Files:**
- Modify: `webrtc/ivr/src/ivr_http_media_client.h`
- Modify: `webrtc/ivr/src/ivr_http_media_client.c`
- Test: `webrtc/apps/ivr_worker/test_ivr_worker_http.c`

**Interfaces:**
- Consumes: ICE ufrag/pwd/fingerprint 与内部 PCM sample rate。
- Produces: `ivr_sdp_build_minimal_audio_offer()` 固定输出 `opus/48000/2`；`ivr_http_media_encode_path_segment()` 返回 caller-owned percent-encoded string。

- [ ] **Step 1: Write failing SDP and path tests**

```c
ivr_sdp_build_minimal_audio_offer(source, offer, sizeof(offer), 16000,
                                  "sendonly");
check_not_null(strstr(offer, "a=rtpmap:111 opus/48000/2\r\n"));
check_null(strstr(offer, "opus/16000/2"));

char *encoded = ivr_http_media_encode_path_segment("room/a?b");
check_equal(encoded, "room%2Fa%3Fb");
free(encoded);
check_null(ivr_http_media_encode_path_segment("bad\r\nid"));
```

- [ ] **Step 2: Run the focused test and verify RED**

Run: `cmake --build --preset win-release-user --target test_ivr_worker_http && ctest --preset win-release-user -R test_ivr_worker_http --output-on-failure`

Expected: SDP assertion fails with `opus/16000/2`, and path helper is missing.

- [ ] **Step 3: Implement the minimal protocol fix**

```c
enum { IVR_OPUS_RTP_CLOCK_RATE = 48000 };
/* The sample_rate remains an internal PCM preference and never changes the
   Opus RTP timestamp clock. */
"a=rtpmap:111 opus/48000/2\r\n"
```

The encoder accepts RFC 3986 unreserved bytes only, percent-encodes other
visible bytes, rejects control bytes and checked-arithmetic overflow.

- [ ] **Step 4: Re-run focused tests and `git diff --check`**

---

### Task 2: TurboHTTP HTTPS WHIP/WHEP adapter

**Files:**
- Modify: `webrtc/ivr/src/ivr_http_media_client.h`
- Modify: `webrtc/ivr/src/ivr_http_media_client.c`
- Modify: `webrtc/ivr/src/ivr_whip_transport.h`
- Modify: `webrtc/ivr/src/ivr_whip_transport.c`
- Modify: `webrtc/ivr/src/ivr_whep_transport.h`
- Modify: `webrtc/ivr/src/ivr_whep_transport.c`
- Modify: `webrtc/apps/ivr_worker/main.c`
- Modify: `webrtc/apps/ivr_worker/CMakeLists.txt`
- Modify: `webrtc/ivr/CMakeLists.txt`
- Test: `webrtc/apps/ivr_worker/test_ivr_worker_http.c`
- Test: `webrtc/apps/room_service/tests/test_ivr_whip_transport.c`

**Interfaces:**
- Consumes: absolute endpoint URL, bearer token, optional CA/client identity, explicit loopback-HTTP permission.
- Produces: versioned `ivr_http_media_client_config_t`, owned `ivr_http_media_client_t`, bounded `ivr_http_media_response_t`.

- [ ] **Step 1: Write failing creation and request tests**

```c
ivr_http_media_client_config_t config = IVR_HTTP_MEDIA_CLIENT_CONFIG_INIT;
config.base_url = "http://192.0.2.1:8080";
check_equal(ivr_http_media_client_create(&config, &client), IVR_EAUTH);

config.base_url = loopback_url;
config.allow_plaintext_loopback = 1;
check_equal(ivr_http_media_client_create(&config, &client), IVR_OK);
```

Add a real loopback Iris request asserting status/body/header mapping and a TLS
fixture asserting CA/hostname failure is returned rather than downgraded.

- [ ] **Step 2: Verify RED against the socket adapter**

- [ ] **Step 3: Replace raw sockets with the sync facade**

```c
turbo_http_options_t options;
turbo_http_options_init(&options, sizeof(options));
options.transport = TURBO_HTTP_TRANSPORT_H1;
options.follow_redirects = 0;
options.timeout_ms = config->timeout_ms;
turbo_http_create_sync(&options, &client->http);
turbo_http_set_max_response_size(client->http,
                                 IVR_HTTP_MEDIA_MAX_RESPONSE - 1u);
```

Set TLS with `verify_peer = 1`, set bearer through the facade, map Location and
ETag using `http_response_get_header()`, and free every response/header exactly
once. DELETE/PATCH/POST keep existing WHIP/WHEP status semantics.

- [ ] **Step 4: Encode all room/call/participant path segments before request**

- [ ] **Step 5: Link only `TurboHttp::TurboHttp` for the IVR adapter**

- [ ] **Step 6: Run focused HTTP and live loopback WHIP/WHEP tests**

---

### Task 3: Worker connection control queue and shutdown ownership

**Files:**
- Modify: `webrtc/apps/ivr_worker/main.c`
- Test: add or extend the nearest worker gateway/process test under `webrtc/apps/ivr_worker/`.

**Interfaces:**
- Consumes: FlowMQ connection callback `(connected, generation)` and management/signal stop requests.
- Produces: bounded MPSC-to-owner control events; only the main worker owner calls `ivr_worker_advance_epoch()` and starts drain.

- [ ] **Step 1: Write a failing rapid reconnect test**

The test sends connected, disconnected, connected events while an active worker
session exists, then asserts epochs advance in owner-loop order and no callback
thread mutates worker state.

- [ ] **Step 2: Verify RED**

- [ ] **Step 3: Implement fixed-size control events and explicit full behavior**

```c
typedef enum {
    IVR_WORKER_CONTROL_CONNECTION,
    IVR_WORKER_CONTROL_DRAIN
} ivr_worker_control_kind_t;

typedef struct {
    ivr_worker_control_kind_t kind;
    int connected;
} ivr_worker_control_event_t;
```

The callback only publishes the value. Queue full latches shutdown and records
a metric; it never calls `ivr_worker_advance_epoch()`. Keep one signal-only
`volatile sig_atomic_t`; normal thread stop state is atomic.

- [ ] **Step 4: Run worker connection, drain and process tests**

---

### Task 4: Room bridge quiescence, route generation and atomic stats

**Files:**
- Modify: `webrtc/ivr/src/ivr_room_bridge.c`
- Modify: `webrtc/ivr/src/ivr_room_bridge.h`
- Test: `webrtc/ivr/tests/test_ivr_room_bridge.c`
- Test: `webrtc/ivr/tests/test_ivr_room_bridge_dedup.c`

**Interfaces:**
- Consumes: FlowMQ request and peer callbacks, each with copied route token.
- Produces: one ordered owner-event stream or equivalent generation fence; stop establishes acceptance and callback quiescence before owner exit.

- [ ] **Step 1: Write failing stop/restart stale-command test**

Inject a request while stop is quiescing, restart, and assert the old request
never changes room state or emits a successful reply.

- [ ] **Step 2: Write failing old-sync-after-reconnect test**

Connect route A, queue sync A, disconnect A, connect route B, then deliver the
delayed sync A. Assert route A is rejected and only sync B registers.

- [ ] **Step 3: Verify both tests RED**

- [ ] **Step 4: Add acceptance gate and callback barrier**

Stop order is: close acceptance, stop endpoint, wake queue, join owner, clear
or drain according to the documented policy. Restart begins with empty request,
peer and route state.

- [ ] **Step 5: Carry route generation through peer events**

Store the complete copied `flowmq_router_route_t` for connected peers and
require worker.sync route equality with the current connected generation.

- [ ] **Step 6: Make bridge stats atomic or owner-snapshotted**

All cross-thread counters use one synchronization scheme; queue locks protect
only their own queue fields.

- [ ] **Step 7: Run bridge, dedup, FlowMQ and concurrent stats tests**

---

### Task 5: RoomService shutdown dependency order

**Files:**
- Modify: `webrtc/apps/room_service/src/server.c`
- Test: `webrtc/apps/room_service/tests/test_room_service_app.c`
- Test: `webrtc/apps/room_service/tests/test_iris_flowmq_provider.c`

**Interfaces:**
- Consumes: provider ingress, durable ledger/outbox, completion dispatcher and media adapter.
- Produces: quiesce-first shutdown in which no new command reaches a stopped media adapter.

- [ ] **Step 1: Write a failing blocked-command shutdown test**

Hold one provider command before dispatch, start server stop, release it, and
assert the command is either drained before adapter stop or rejected with the
documented closed result.

- [ ] **Step 2: Verify RED**

- [ ] **Step 3: Reorder shutdown**

```c
iris_flowmq_provider_stop(server->iris_flowmq_provider);
iris_media_reconciler_stop(server->iris_media_reconciler);
iris_completion_dispatcher_stop(server->iris_completion_dispatcher);
iris_event_outbox_stop(server->iris_event_outbox);
ivr_fmq_adapter_stop(server->ivr_fmq);
iris_command_ledger_stop(server->iris_command_ledger);
```

Adjust dispatcher/outbox order if their actual drain contract requires the
outbox to remain available while accepted completions settle; encode that order
in the test rather than relying on comments.

- [ ] **Step 4: Run RoomService provider/outbox/dispatcher/app tests**

---

### Task 6: TurboDB ORM driver deployment and byte limits

**Files:**
- Modify: `webrtc/apps/room_service/src/iris_orm_store.c`
- Modify: `webrtc/apps/room_service/src/iris_orm_store.h`
- Modify: `webrtc/apps/room_service/include/room_service/config.h`
- Modify: `webrtc/apps/room_service/src/config.c`
- Modify: `webrtc/apps/room_service/config/room_service.toml.example`
- Modify: `webrtc/apps/room_service/config/room_service.orm.sqlite.yaml.example`
- Test: `webrtc/apps/room_service/tests/test_iris_orm_store.c`
- Test: `webrtc/apps/room_service/tests/test_room_service_config.c`

**Interfaces:**
- Consumes: YAML backend `sqlite` or `postgresql` and driver-specific ORM options.
- Produces: `orm_config_t.driver` equals configured backend; default package succeeds only for SQLite, while a PG-enabled replacement ORM runtime may satisfy PostgreSQL without TurboMedia linking libpq.

- [ ] **Step 1: Write failing config contract tests**

```c
check_not_null(iris_orm_store_owner_create(sqlite_yaml, "iris.test", 1,
                                           error, sizeof(error)));
/* A PostgreSQL config must reach orm_connect; the installed SQLite-only ORM
   returns its own unsupported-driver error, not a TurboMedia hard rejection. */
```

Add tests proving `max_bytes` and `max_item_bytes` cannot be accepted as no-op
fields: either they enforce byte budgets or configuration fails.

- [ ] **Step 2: Verify RED**

- [ ] **Step 3: Remove the PostgreSQL pre-rejection and hard-coded driver**

Pass `backend` to `orm_config_t.driver`. Map SQLite filename/options and
PostgreSQL connection options separately. Preserve the SQLite-only packaging
and no-libpq CMake contract.

- [ ] **Step 4: Implement checked byte-budget arithmetic**

Use `max_item_bytes` as the canonical per-record key+value limit and
`max_bytes` as the namespace retained-payload limit, with overflow-safe batch
delta computation inside the same transaction as revision CAS. If the current
record-store API cannot expose those semantics without a second scan, reject
the two legacy fields and update examples instead of silently accepting them.

- [ ] **Step 5: Run ORM, ledger, outbox and config tests**

---

### Task 7: Runtime dependency closure

**Files:**
- Modify only if evidence identifies a preset/deployment defect: `CMakeUserPresets.json` or target runtime installation rules.
- Test: existing executable smoke tests through CTest presets.

**Interfaces:**
- Consumes: one profile-consistent TurboUtils, TurboNet, TurboHTTP, FlowMQ and TurboDB installation set.
- Produces: packaged executables whose DLL imports resolve without copied mixed-version binaries.

- [ ] **Step 1: Reproduce the entrypoint failure and record exact import/export evidence**

- [ ] **Step 2: Rebuild/install TurboUtils then TurboNet from matching Release presets**

- [ ] **Step 3: Fresh-configure and rebuild TurboMedia with the same profile roots**

- [ ] **Step 4: Run ivr_worker dry-run, RoomService startup and live WebRTC smoke tests**

Do not add post-build DLL-copy fallback. If an external dependency rebuild is
still required, report the exact root/version mismatch and leave repository
code unchanged.

---

### Task 8: Documentation and final verification

**Files:**
- Modify: `webrtc/ivr/README.md`
- Modify: `webrtc/docs/ivr-production-readiness-design-zh.md`
- Modify: `webrtc/docs/ivr-production-readiness-checklist-zh.md`

**Interfaces:**
- Consumes: verified implementation and test output from Tasks 1–7.
- Produces: FlowMQ-only data-flow documentation and reproducible release gates.

- [ ] **Step 1: Replace obsolete HTTP-provider diagrams and POST-to-Iris text**

- [ ] **Step 2: Document HTTPS, route-generation, shutdown and ORM runtime contracts**

- [ ] **Step 3: Run focused tests, then adjacent WebRTC/RoomService CTest filters**

- [ ] **Step 4: Run Release build and executable smoke tests**

- [ ] **Step 5: Run `git diff --check`, residual dependency searches and inspect final status**

- [ ] **Step 6: Update GitHub issues with verification evidence; close only fully satisfied issues**
