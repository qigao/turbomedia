# FFmpeg Pipeline Multi-Sink Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在保持 `turbo.media.pipeline/v1` 单输出配置和 C API 兼容的情况下，让 FFmpeg 流水线把共享的复制或编码媒体分支同步写入多个 mux/sink 输出。

**Architecture:** `turbo_pipeline_t` 继续单线程拥有输入、编解码器与输出；把单一 output context 改为最多 8 个有界 output slots。媒体分支只准备一次，随后为每个 output 建立独立 `AVStream`；包经一个预分配 scratch packet 顺序引用、重标时间戳并写入各输出。所有输出共享背压和失败域，Runtime/RTP 维持单输出。

**Tech Stack:** C11、FFmpeg libavformat/libavcodec/libavfilter、TurboParser DataBind YAML、TinyTest、CMake Presets。

**Spec:** `pipeline/docs/multi-sink-design-zh.md`

## Global Constraints

- 不新增或改变导出的 C API；YAML `api_version` 保持 `turbo.media.pipeline/v1`。
- 已配置的音频/视频分支必须扇出到所有 mux；首版不支持 per-output 轨道或编码参数。
- 每个 mux 只绑定一个 sink；输出数固定上限为 8。
- 同步写入、无隐藏队列、无静默丢包；慢 sink 受 `io_timeout_ms` 约束并对全局施加背压。
- 任一输出失败使整条流水线失败；错误关联实际 mux/sink 节点。
- Runtime/RTP 的现有队列、owner 和单输出行为不改变。
- 所有代码修改先由失败测试驱动，完成声明前执行 Release 构建、定向测试和差异检查。

### Task 1: 固化多输出图验证契约

**Files:**
- Modify: `tests/test_pipeline.c`
- Modify: `pipeline/src/turbo_pipeline.c`

**Interfaces:**
- Consumes: 现有 Node/Edge YAML schema。
- Produces: 1..8 个 FFmpeg mux/sink 对；Runtime/RTP 仍为 1 个输出。

- [x] **Step 1: 新增失败测试**

在 `test_pipeline.c` 增加：两个完整 FFmpeg 输出图创建成功；mux/sink 数量不匹配失败；一个媒体分支未连接全部 mux 失败；Runtime/RTP 多输出失败。

- [x] **Step 2: 运行定向测试并确认 RED**

Run: `cmake --build build/Msvc-Release --target turbo_media_test_pipeline && ctest --test-dir build/Msvc-Release -R '^turbo_media_test_pipeline$' --output-on-failure`

Expected: 合法双输出图仍因“v1 requires exactly one ... mux and sink”创建失败。

- [x] **Step 3: 引入有界 output 拓扑元数据**

增加 `PIPELINE_MAX_OUTPUTS = 8` 和固定数组 output slots。收集 mux 节点、逐一验证唯一 `mux.out -> sink.in` 配对，并标记使用的 backbone 节点/边。

- [x] **Step 4: 改造 branch trace 的终点 fan-out**

保留 source 到 encoder 前的唯一链约束；终点必须以同名媒体 pad 连接全部 mux，且无重复/额外边。Runtime/RTP 在识别执行模式时明确拒绝多输出。

- [x] **Step 5: 重跑定向测试和 `git diff --check`**

### Task 2: 拆分共享媒体准备与逐输出 stream 准备

**Files:**
- Modify: `pipeline/src/turbo_pipeline.c`
- Modify: `tests/test_pipeline.c`

**Interfaces:**
- Consumes: 单一输入 stream 和可选 decoder/filter/encoder。
- Produces: 每个 output 独立的 `AVFormatContext` 及 audio/video `AVStream`。

- [x] **Step 1: 新增双文件 stream-copy 集成测试**

从临时 WAV 输入复制到两个 Matroska 文件；prepare/run 均成功，两个文件存在且非空，stats 的成功写入数按两个 sink 累加。

- [x] **Step 2: 运行测试并确认 RED**

- [x] **Step 3: 拆分 branch 初始化**

`pipeline_prepare_branch()` 只查找输入、打开 decoder/encoder 并分配长期 frame/packet 一次；新增逐 output stream 创建函数，从输入 codec parameters 或共享 encoder context 导出参数。

- [x] **Step 4: 循环准备 output contexts**

按 YAML 节点顺序为每个 mux/sink 创建 context、options、streams 和 IO。任一步失败保留明确 node_id，统一释放此前已创建输出。

- [x] **Step 5: 重跑双输出、既有单输出 prepare 测试**

### Task 3: 实现同步包扇出、收尾与资源释放

**Files:**
- Modify: `pipeline/src/turbo_pipeline.c`
- Modify: `tests/test_pipeline.c`

**Interfaces:**
- Consumes: copy packet 或 encoder packet，以及源 time base。
- Produces: 对所有 outputs 的有序成功 delivery，或首个明确失败。

- [x] **Step 1: 新增转码双输出和失败域测试**

双输出转码应成功；不可打开的第二 sink 应在 prepare 时返回该 sink node_id，且不会报告为第一个输出失败。

- [x] **Step 2: 运行测试并确认 RED**

- [x] **Step 3: 增加预分配 scratch packet 与 fan-out helper**

每次输出先 `av_packet_ref()`，再按对应 `AVStream::time_base` 重标时间戳并写入。每次成功写入累加 `packets_written/bytes_written`；失败立即返回，绝不继续或丢弃错误。

- [x] **Step 4: 循环写 header/trailer**

header 顺序 fail fast；正常 trailer 对所有已写 header 的输出执行收尾并返回首个错误。所有 deadline 沿用现有 open/io timeout。

- [x] **Step 5: 循环释放 output 与 scratch packet**

确保 prepare 失败、run 失败、stop、正常 EOF 和 destroy 均只释放一次；Runtime/RTP 资源释放路径保持独立。

- [x] **Step 6: 运行全部 pipeline 测试及 `git diff --check`**

### Task 4: 文档、示例与最终验证

**Files:**
- Modify: `pipeline/README.md`
- Add: `examples/pipeline/ffmpeg_multi_sink.yml`
- Modify: `docs/superpowers/plans/2026-08-25-pipeline-multi-sink.md`

**Interfaces:**
- Consumes: 已验证的多输出实现。
- Produces: 可复制的直播+录制拓扑说明、统计/失败/背压契约和验证记录。

- [x] **Step 1: 文档化受限 fan-out**

说明所有 mux 必须接收全部已配置媒体分支、同步共同失败域、`io_timeout_ms` 背压，以及 written stats 按 sink delivery 计数。

- [x] **Step 2: 添加双输出示例**

示例使用环境可替换的输入/输出 URL，并明确一个输出可对应直播协议、另一个可对应文件归档；不承诺自动重连或 per-output 队列。

- [x] **Step 3: 执行最终验证**

Run:

```powershell
cmake --build build/Msvc-Release --target turbo_media_test_pipeline turbo_pipeline_run
ctest --test-dir build/Msvc-Release -R '^turbo_media_test_pipeline$' --output-on-failure
git diff --check
git status --short
```

- [x] **Step 4: 审查残留单输出字段与拓扑假设**

Run: `rg.exe -n "output_stream|output_options|header_written|pipeline->output|mux_node|sink_node" pipeline/src/turbo_pipeline.c`

只允许 Runtime/RTP 的明确 primary-output 引用或新 output slot 内字段；不得遗留 FFmpeg 单输出写入路径。

**Verification record (2026-08-25):**

- Release 全目标构建成功。
- `turbo_media_test_pipeline` 定向测试通过。
- 全量 CTest 首轮 82/83 通过；唯一失败为无关的
  `test_ivr_dispatch_processes` readiness 时序场景（期望 202，实际 503）。
- 该场景 focused rerun 28/28 断言通过；CTest `--rerun-failed` 正式入口随后
  1/1 通过（177.97 秒）。
- `git diff --check` 通过，旧 FFmpeg 单 output context/stream 字段检索无残留。

- [ ] **Step 5: 提交并推送**

仅提交本计划涉及的源代码、测试、示例和文档；推送前再次确认工作树无未知改动。
