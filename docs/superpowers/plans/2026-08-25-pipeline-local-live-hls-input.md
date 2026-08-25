# Pipeline 本地 Live HLS 输入验收实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 `ffmpeg.input` 增加不依赖公网的增量 Live HLS 验收，证明 Pipeline 会等待 playlist revision、读取新 segment，并能协作式停止和完整回收。

**Architecture:** 先用现有 Pipeline/FFmpeg 路径在临时目录生成确定性的 H.264/MPEG-TS segments，再由动态端口 CoroNet loopback HTTP server 提供没有 `#EXT-X-ENDLIST` 的不可变 playlist response 和 segment response。独立 TurboUtils 线程运行 reader；测试 owner 驱动 HTTP context，观察 6 秒初始窗口读取达到稳定水位后，原子切换 server 的 playlist revision，确认第三个 segment 被请求且 packet 计数继续推进，再请求停止并 join。

**Tech Stack:** C11、TinyTest、TurboUtils filesystem/thread、TurboMedia Pipeline、FFmpeg HLS demuxer、CMake Presets、CTest。

**Spec:** GitHub issue #19；`pipeline/README.md` 的状态、所有权、协作停止与验证范围契约。

## Global Constraints

- 不使用公网、固定端口或 sleep-only 成功判定；所有等待都有状态条件和硬上限。
- 每次 HTTP playlist response 对应一个完整、不可变 revision；Windows 上 FFmpeg 持有 file URL 时不支持 `MoveFileEx(...REPLACE_EXISTING)`，因此不使用原地文件覆盖模拟更新。
- reader 线程是 FFmpeg context、packet/frame 和输出文件的唯一 owner；测试线程只调用并发安全的 stats snapshot 与 request-stop。
- 先停止 reader、join，再销毁 Pipeline 和临时目录；失败路径执行同一清理顺序。
- 只新增测试/文档证据，不改变公开 API、配置格式、协议语义或生产实现。

---

### Task 1: 抽取确定性 HLS fixture 边界

**Files:**
- Modify: `tests/test_pipeline.c`

**Interfaces:**
- Consumes: `write_test_yuv420p_frames()`、现有 YAML Pipeline create/prepare/run API。
- Produces: 测试内 helper，生成完整 VOD playlist/segments，并从 playlist 提取前两个非注释 segment URI。

- [x] **Step 1: 抽取现有 VOD writer setup**

把当前 local HLS 测试的 rawvideo → H.264 → HLS writer 收敛为测试内 helper；现有 VOD reader 仍断言正常 EOF、Matroska header 和非零 packet stats。

- [x] **Step 2: 构建并运行现有 Pipeline 测试**

Run: `cmake --build --preset win-release-user --target turbo_media_test_pipeline && ctest --preset win-release-user -R turbo_media_test_pipeline --output-on-failure`

Expected: 现有测试保持通过，证明纯重构没有改变行为。

### Task 2: 增加 Live HLS playlist revision 行为测试

**Files:**
- Modify: `tests/test_pipeline.c`
- Modify: `pipeline/README.md`

**Interfaces:**
- Consumes: Task 1 fixture segments、`pipeline_execute_result_t`、`execute_pipeline_thread()`、CoroNet loopback TCP handler。
- Produces: 一个 TinyTest `it(...)`，覆盖无 ENDLIST、首段稳定、第二 revision 增量读取、request-stop 和 join。

- [x] **Step 1: 写 Live HLS 行为测试**

初始 playlist 引用两个 3 秒 segment，给 `avformat_find_stream_info()` 足够的真实 live window；reader 进入 RUNNING 且 packet stats 在非零值上稳定后，原子切换为包含第三个 segment 的 revision。测试要求第三个 segment 收到 HTTP 请求、`packets_read` 超过初始稳定水位，且在更新前 Pipeline 没有错误退出。

- [x] **Step 2: 验证新增测试实际走增量路径**

Run: `build\\Msvc-Release\\bin\\turbo_media_test_pipeline.exe --filter "waits for a local live HLS playlist revision"`

Expected: 测试只有在第二 revision 被读取后才能通过；删除 revision replace 或重复旧 playlist 会导致 packet 水位断言失败。

- [x] **Step 3: 验证协作停止与资源清理**

断言 `turbo_pipeline_request_stop()` 返回 `TURBO_PIPELINE_OK`，线程在 deadline 内完成，run status 为 `TURBO_PIPELINE_ESTOPPED`；随后读取输出 EBML header 并删除完整临时目录。

- [x] **Step 4: 更新验证范围文档**

把 `pipeline/README.md` 的本地 HLS 覆盖从仅 VOD 扩展为 VOD + 增量 live playlist + 协作停止，明确这是本地确定性验收而非公网兼容性证明。

### Task 3: 回归、审查与交付

**Files:**
- Modify: `docs/superpowers/plans/2026-08-25-pipeline-local-live-hls-input.md`

**Interfaces:**
- Consumes: Tasks 1–2 的实现和 focused test。
- Produces: issue #19 的可复验证据、提交和远端分支更新。

- [x] **Step 1: 重复 focused test**

Run: PowerShell 循环 3 次执行 `build\\Msvc-Release\\bin\\turbo_media_test_pipeline.exe --filter "waits for a local live HLS playlist revision"`，逐次检查退出码。

Expected: 3/3 通过，无 hang、timeout 或残留临时目录。

- [x] **Step 2: 运行邻接与全量 Release 验证**

Run Pipeline/HLS focused CTest，随后 `cmake --build --preset win-release-user` 和 `ctest --preset win-release-user -j 4 --output-on-failure`。

Result: focused test 3/3 通过；Release build 无待构建项；CTest 83/83 通过，总耗时 185.05 秒。

- [x] **Step 3: 自审、提交与关闭 issue**

运行 `git diff --check`，检查 diff、`.codegraph/` 排除和工作树状态；提交关联 #19，push `origin/master`，在 issue 留下准确测试证据后关闭。
