# Pipeline 本地 RTSP 输入验收实现计划

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 为 `ffmpeg.input` 增加不依赖公网的 RTSP/TCP 端到端验收，证明本地 RTSP Server Adapter 发布的 RTP access unit 能经过 Pipeline demux/mux/sink，并可协作式停止和完整回收。

**Architecture:** 测试主线程拥有 `coro_context_t`、Server Runtime、媒体 source 与 RTSP adapter，并以 `TURBO_RUN_NOWAIT` 驱动事件循环。独立 TurboUtils 线程执行 Pipeline 的 prepare/run；主线程在 source 出现订阅者后发布有界数量的 RTP packet，观察 Pipeline stats 后调用 `turbo_pipeline_request_stop()`，join 后按 owner 逆序销毁资源。生产接口与部署方式不变。

**Tech Stack:** C11、TinyTest、CoroNet、TurboMedia Server/RTSP/Pipeline、FFmpeg libavformat、CMake Presets、CTest。

---

## 数据路径与生命周期契约

- 数据单元：单个有界 RTP packet，测试栈上 buffer 为调用方权威内容；`turbo_media_source_publish()` 调用期间 borrowed，返回后调用方可失效原 buffer，source 只保留其有界 GOP cache 自有副本。
- 拓扑：一个测试线程 producer/owner，一个 FFmpeg Pipeline consumer 线程；RTSP/TCP interleaved 保持 per-track 顺序。
- 容量：source/runtime 使用既有有界 GOP/subscriber 配置；fixture 最多发布固定数量 packet，单 packet 受栈 buffer 容量限制。
- 背压/失败：任一 create/start/publish/prepare/run/status 失败都由 TinyTest 明确断言；不自动切换 UDP 或公网输入。
- 关闭：停止生产，调用 Pipeline request-stop，持续驱动 CoroNet 直到 Pipeline 线程退出并 join；随后 adapter stop/destroy、runtime destroy、context destroy，不使用 kill。
- 用户可见行为：只新增测试覆盖与测试目标链接依赖，不改变公开 API、配置格式或生产数据路径。

### Task 1: 增加失败的本地 RTSP/TCP Pipeline 验收

**Files:**
- Modify: `tests/test_pipeline.c`
- Modify: `tests/CMakeLists.txt`

**Step 1: 写行为测试**

在 `test_pipeline.c` 增加 fixture helper 与单一 `it(...)`：创建本地 RTSP adapter/source，Pipeline 使用 `rtsp_transport: tcp` 拉取，发布实际 RTP access unit，并断言 `packets_read`、`packets_written` 和协作式停止结果。

状态：完成。

**Step 2: 验证 RED**

通过 `win-release-user` 重新 configure、构建 `turbo_media_test_pipeline` 并过滤运行新增用例。预期测试因尚未满足的 RTSP/FFmpeg 交互或链接边界失败；确认失败来自目标行为，而不是语法或 fixture 泄漏。

状态：完成。有效 RED 为无效 H264 fixture 导致 FFmpeg 直到 RTSP EOF 才结束探测，从而未经过协作式停止；改用真实 OpenH264 access unit 后进入 GREEN。

**Step 3: 最小化修复**

只修复测试揭示的最小生产边界；若现有生产实现已满足契约，则只补齐 `test_pipeline` 对 `TurboMedia::RTSP` 的显式链接，不制造生产改动。

状态：完成。生产实现无需修改；仅补测试链接、fixture 和文档。

**Step 4: 验证 GREEN**

重复运行新增用例至少 3 次，确认每次均有 packet 穿过 Pipeline 且停止/回收不挂起。

状态：完成。`--repeat until-fail:3` 三次通过。

### Task 2: 回归、并行验证与交付

**Files:**
- Modify: `README.md`（仅当现有测试文档需要登记本地 RTSP 覆盖）

**Step 1: 聚焦回归**

运行 Pipeline、RTSP adapter、RTSP 相关 CTest 过滤集，确认现有 HLS/File/Runtime RTP 路径不回归。

状态：完成，4/4 通过。

**Step 2: 并行回归**

运行 `ctest --preset win-release-user -j 4`，确认固定端口不与仓库测试冲突且所有 teardown 完成。

状态：完成。一次完整并行运行 83/83 通过；最终安全审查补丁后的并行运行中，本任务相关测试全部通过，唯一无关 IVR readiness 场景首次失败后在 focused 与 CTest 正式入口重跑通过。

**Step 3: 审查与提交**

检查 `git diff --check`、`git status` 与最终 diff；提交关联 issue #16，push `origin/master`，并用测试证据关闭 issue。

状态：完成。

## 验证记录（2026-08-25）

- `turbo_media_test_pipeline --repeat until-fail:3`：3/3 通过。
- Pipeline/RTSP/RTSP library/Server Adapter 聚焦 CTest：4/4 通过。
- Release 全目标构建：成功。
- `ctest --preset win-release-user -j 4 --output-on-failure`：一次 83/83 通过（164.88 秒）；最终工作树复跑为 82/83，唯一失败是既有 `test_ivr_dispatch_processes` readiness 时序（期望 202、实际 503）。
- 该失败子场景 focused 重跑：1/1 用例、129/129 断言通过（16.64 秒）。
- `ctest --preset win-release-user --rerun-failed --output-on-failure`：1/1 通过（162.58 秒）。
