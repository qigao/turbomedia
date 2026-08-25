# Feature Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在仓库中提供可复验、默认 fail-fast 的公网 TURN-only 浏览器验收工具，覆盖 Browser Publisher → WHIP → SFU → WHEP → Browser Viewer、凭据过期、ICE restart、网络切换与完整清理，并生成 canonical JSON、Markdown、JUnit 三种一致结果。

**Architecture:** 新增独立的 Node.js 验收包。Node controller 是每个 case 状态机的唯一 owner；Selenium、TURN/SFU 凭据 provider、拓扑 hook、SFU HTTP API 都是 adapter。浏览器页面只实现受限自动化 API，领域判定、阈值、结果分类和清理均由 controller 完成。现有 SFU 仍是媒体事实源，不增加或改变公开运行时 API。

**Tech Stack:** Node.js 22+ CommonJS、`node:test`、`selenium-webdriver@4.47.0`、`ajv@8.20.0`、WebRTC/WHIP/WHEP、Selenium Remote WebDriver、JSON Schema draft 2020-12。

**Spec:** `docs/superpowers/specs/2026-08-25-webrtc-public-browser-turn-acceptance-design.md`

## Global Constraints

- 仅新增显式运行的 `webrtc/acceptance` 测试包；不得把 npm install 或公网验收加入默认 CMake/CTest。
- 不改变 SFU、WHIP、WHEP、控制 API、配置格式或部署协议。若现有 API 无法提供某项必需事实，该 case 必须为 `INCOMPLETE`，另开 issue，不得在本计划中暗改公开接口。
- release run 必须使用 manifest 中的精确 browser/platform/version；缺浏览器、缺 case、capability 不匹配或 relay 证据不足均返回非零，不得 fallback 到本地浏览器、STUN 或 host candidate。
- 退出码固定为 `PASS=0`、`FAIL=2`、`INCOMPLETE=3`、`ERROR=4`；多 case 汇总按 `ERROR > INCOMPLETE > FAIL > PASS` 取最高严重度。
- 凭据仅通过 provider 的 stdin/stdout 传递；不得进入 argv、环境变量、URL、DOM、localStorage、日志或 artifact。所有错误文本写盘前必须经过同一 redactor。
- provider/hook stdout、stderr、case samples、日志和 artifact 均有 byte/item/time 上限；达到上限立即终止当前操作并保留首个失败分类。
- case 状态只能由 Node event loop owner 推进；所有异步事件必须携带 `run_id`、`case_id`、`generation`、`sequence`。旧 generation/sequence 只计数，不推进状态；未知或非法事件返回 `ERROR`。
- 所有非终态都必须经过 `DRAINING`；清理顺序固定为停止采样、DELETE WHIP/WHEP、关闭 peer connections、退出 drivers、检查 case SFU 零残留、检查全局基线、检查 TURN 基线、执行 topology teardown、落盘报告。
- SFU 自动发布轨道的事实契约来自 `webrtc/apps/sfu_node/src/server.c`：首轨为 `<publisher>-audio` 或 `<publisher>-video`，后续轨为 `<publisher>-<kind>-<index>`。controller 必须通过 SFU 查询确认注册完成后才设置 viewer subscription，不能从浏览器 SDP track id 推断。
- 资源默认值集中在 `src/constants.js`：`MAX_CASES=256`、`MAX_PROVIDER_OUTPUT_BYTES=65536`、`MAX_HOOK_OUTPUT_BYTES=65536`、`MAX_CASE_LOG_BYTES=4194304`、`MAX_SAMPLES_PER_CASE=36000`、`MAX_ARTIFACT_BYTES_PER_CASE=16777216`、`MIN_SAMPLE_INTERVAL_MS=250`、`MAX_CASE_DURATION_MS=3600000`；manifest 只能下调硬上限。
- 默认 deadline 集中命名：provider/hook 30 秒、WebDriver command 45 秒、connect 120 秒、stable window 60 秒、transition 60 秒、recovery 120 秒、drain 120 秒。
- canonical JSON 采用递归 key 排序和 UTF-8/LF；三个 report writer 只消费同一个已完成、已脱敏的 report model。
- #17 只有在外部实验室提供完整 release manifest 的 `PASS` 报告后才能关闭。本计划实现完成只能把仓库能力标记为“harness ready”，不能宣称生产就绪。

---

### Task 1: 建立独立包、固定依赖与 schema 验证边界

**Files:**
- Modify: `.gitignore`
- Create: `webrtc/acceptance/package.json`
- Create: `webrtc/acceptance/package-lock.json`
- Create: `webrtc/acceptance/src/constants.js`
- Create: `webrtc/acceptance/src/contracts.js`
- Create: `webrtc/acceptance/schemas/manifest.schema.json`
- Create: `webrtc/acceptance/schemas/turn-credential.schema.json`
- Create: `webrtc/acceptance/schemas/sfu-token.schema.json`
- Create: `webrtc/acceptance/schemas/hook-receipt.schema.json`
- Create: `webrtc/acceptance/schemas/report.schema.json`
- Create: `webrtc/acceptance/test/contracts.test.js`

**Interfaces:**

```js
const LIMITS = Object.freeze({
  MAX_CASES: 256,
  MAX_PROVIDER_OUTPUT_BYTES: 65_536,
  MAX_HOOK_OUTPUT_BYTES: 65_536,
  MAX_CASE_LOG_BYTES: 4_194_304,
  MAX_SAMPLES_PER_CASE: 36_000,
  MAX_ARTIFACT_BYTES_PER_CASE: 16_777_216,
  MIN_SAMPLE_INTERVAL_MS: 250,
  MAX_CASE_DURATION_MS: 3_600_000
});

function createContractValidator(schemaDirectory) {}
function validateContract(validator, schemaName, value) {}

module.exports = { LIMITS, createContractValidator, validateContract };
```

Manifest schema 必须要求 `schema_version`、run mode、SFU endpoints、provider commands、topology hooks、browser matrix、phase deadlines、threshold profile 和 artifact directory；所有对象默认 `additionalProperties: false`。凭据 schema 只接受短期 TURN `username/credential/urls/expires_at` 与带 audience/scope/subject/expiry 的 SFU token。hook receipt 必须包含 `hook_id/action/generation/sequence/started_at/finished_at/status`，不得包含 secret 字段。

- [ ] 写 `contracts.test.js`：验证合法最小 manifest；拒绝未知字段、超过 256 cases、低于 250ms sampling、超出一小时 duration、缺 expiry 的 credential、带 secret 的 hook receipt、未知 report outcome。
- [ ] 运行 `npm test -- --test-name-pattern=contracts`，确认因模块/schema 不存在而失败。
- [ ] 创建 package，精确 pin `selenium-webdriver: 4.47.0`、`ajv: 8.20.0`；生成并提交 lockfile。实现常量和 Ajv schema registry，禁止 coercion/default/removeAdditional。
- [ ] 在 `.gitignore` 增加 `/webrtc/acceptance/node_modules/` 与 `/artifacts/webrtc-acceptance/`，不忽略 golden fixtures。
- [ ] 重跑 contracts 测试，确认所有正反例通过；运行 `npm audit --omit=dev` 并记录输出（本包依赖全部放 `devDependencies`，审计结果仍须可复验）。
- [ ] 提交：`test(webrtc): scaffold TURN acceptance contracts (#17)`。

### Task 2: 实现确定性 manifest 归一化、矩阵展开与标识

**Files:**
- Create: `webrtc/acceptance/src/canonical_json.js`
- Create: `webrtc/acceptance/src/manifest.js`
- Create: `webrtc/acceptance/src/ids.js`
- Create: `webrtc/acceptance/test/manifest.test.js`
- Create: `webrtc/acceptance/test/ids.test.js`
- Create: `webrtc/acceptance/test/fixtures/minimal-manifest.json`

**Interfaces:**

```js
function canonicalStringify(value) {}
function loadManifest(filePath, runtimeOverrides) {}
function expandCases(manifest) {}
function hashCanonical(value) {}
function createRunIdentity(clock, randomUUID, manifestHash) {}
function createCaseIdentity(runIdentity, caseDefinition, ordinal) {}

module.exports = {
  canonicalStringify, loadManifest, expandCases, hashCanonical,
  createRunIdentity, createCaseIdentity
};
```

Case key 固定由 browser name/version/platform、publisher/viewer role、network profile、credential profile 与 scenario 组成。输入数组顺序保持；对象 key 排序；重复 case key、空矩阵或展开超过上限立即报错。允许的 runtime override 仅限 output directory 与 run label，不能覆盖 browser version、relay policy、threshold 或 security setting。

- [ ] 写测试：同一语义不同 key 顺序得到同 hash；矩阵展开顺序稳定；重复 case 拒绝；override 白名单生效；越权 override 拒绝；固定 clock/UUID 产生稳定 run/case id。
- [ ] 运行两个测试文件，确认缺实现失败。
- [ ] 实现 canonical serializer、SHA-256 hash、schema 验证后的深冻结 manifest、稳定矩阵展开与 ID 生成。
- [ ] 重跑测试两次并比较输出，确认没有时间、locale 或对象插入顺序漂移。
- [ ] 提交：`test(webrtc): add deterministic acceptance manifest model (#17)`。

### Task 3: 实现显式 case 状态机与结果分类

**Files:**
- Create: `webrtc/acceptance/src/state_machine.js`
- Create: `webrtc/acceptance/test/state_machine.test.js`

**Interfaces:**

```js
const CaseState = Object.freeze({
  CREATED: 'CREATED', PREPARING: 'PREPARING', CONNECTING: 'CONNECTING',
  STABLE: 'STABLE', TRANSITIONING: 'TRANSITIONING', RECOVERING: 'RECOVERING',
  DRAINING: 'DRAINING', PASSED: 'PASSED', FAILED: 'FAILED',
  INCOMPLETE: 'INCOMPLETE', ERROR: 'ERROR'
});

const Outcome = Object.freeze({
  PASS: 'PASS', FAIL: 'FAIL', INCOMPLETE: 'INCOMPLETE', ERROR: 'ERROR'
});

function createCaseMachine(identity) {}
function stepCase(machine, event) {}
function beginDrain(machine, primaryOutcome, reason) {}
function finishDrain(machine, cleanupFailures) {}

module.exports = { CaseState, Outcome, createCaseMachine, stepCase, beginDrain, finishDrain };
```

转换表必须逐条对应 spec。`stepCase` 是纯函数并返回新对象；event 必须匹配 run/case、当前 generation 且 sequence 严格递增。旧事件增加 `stale_event_count` 后返回，不改变 phase。cleanup 失败不能覆盖已有 `FAIL`/`INCOMPLETE` 的 primary evidence，但没有 primary failure 时 cleanup 失败归类 `ERROR`。

- [ ] 写全状态/转换表测试，覆盖正常通过、阈值失败、实验室缺能力、provider 异常、timeout、cancel、重复/乱序/旧 generation、非法事件、终止抢占、每个非终态到 drain、cleanup-only error。
- [ ] 运行测试，确认缺实现失败。
- [ ] 用不可变状态对象和静态 transition map 实现；不得在 guard/action 中执行 I/O。
- [ ] 运行状态机测试并用表驱动断言确认所有公开 state/event 至少覆盖一次。
- [ ] 提交：`test(webrtc): add acceptance case state machine (#17)`。

### Task 4: 实现有界子进程、凭据 provider、拓扑 hook 与统一脱敏

**Files:**
- Create: `webrtc/acceptance/src/redaction.js`
- Create: `webrtc/acceptance/src/process_adapter.js`
- Create: `webrtc/acceptance/src/providers.js`
- Create: `webrtc/acceptance/test/redaction.test.js`
- Create: `webrtc/acceptance/test/providers.test.js`
- Create: `webrtc/acceptance/test/fixtures/provider_process.js`
- Create: `webrtc/acceptance/test/fixtures/hook_process.js`

**Interfaces:**

```js
async function runBoundedCommand(options) {}
function createRedactor(secretValues) {}
async function issueTurnCredential(adapter, request) {}
async function issueSfuToken(adapter, request) {}
async function invokeTopologyHook(adapter, request) {}

module.exports = {
  runBoundedCommand, createRedactor,
  issueTurnCredential, issueSfuToken, invokeTopologyHook
};
```

`runBoundedCommand` 只接受 manifest 中拆分后的 executable/args，不经 shell；JSON request 从 stdin 写入，单个 JSON response 从 stdout 读取。timeout、非零退出、额外 stdout、超限 stdout/stderr、invalid UTF-8、schema error 均 fail fast。进程终止先发正常终止，短 grace 后强制 kill。redactor 同时匹配原 secret、JSON escaped 与 URL encoded 形式；空串不得注册为 secret。

- [ ] 写 fixture 模式覆盖 success、timeout、nonzero、malformed、multiple JSON、stdout/stderr overflow、expired credential、receipt sequence mismatch；写 redaction 测试确保三种编码都不会泄漏。
- [ ] 运行 tests，确认缺实现失败。
- [ ] 实现无 shell 的 bounded process adapter、schema validation、expiry/deadline 检查、receipt correlation 检查和 redactor。
- [ ] 重跑测试并检查失败消息只含 operation、case id、stage、exit/signal/schema path，不含完整 payload。
- [ ] 提交：`test(webrtc): add bounded lab provider adapters (#17)`。

### Task 5: 实现统一采样、nearest-rank 百分位与阈值判定

**Files:**
- Create: `webrtc/acceptance/src/metrics.js`
- Create: `webrtc/acceptance/test/metrics.test.js`

**Interfaces:**

```js
function nearestRank(samples, percentile) {}
function normalizeBrowserSnapshot(raw, previous, timestampMs) {}
function normalizeSfuSnapshot(raw, previous, timestampMs) {}
function appendBoundedSample(series, sample, maxSamples) {}
function evaluatePhaseMetrics(series, thresholdProfile) {}

module.exports = {
  nearestRank, normalizeBrowserSnapshot, normalizeSfuSnapshot,
  appendBoundedSample, evaluatePhaseMetrics
};
```

只把单调 counter 转成 delta/rate；counter reset 或时间倒退是明确采样错误。百分位采用排序后 `x[ceil(p*n)-1]`，report 必须同时记录 sample count、min/max、p50/p95/p99。必需字段缺失、样本不足或采样 gap 超限为 `INCOMPLETE`，值超过阈值为 `FAIL`，parser/非有限数/负 delta 为 `ERROR`。

- [ ] 写测试覆盖 n=1、奇偶样本、重复值、p50/p95/p99、counter reset、NaN/Infinity、上限、gap、缺字段、阈值边界等号和超出。
- [ ] 运行测试，确认失败。
- [ ] 实现纯函数指标层；禁止对缺数据填 0，禁止平均值替代 percentile。
- [ ] 重跑测试并使用手算向量 `[1, 2, 3, 4, 100]` 验证 nearest-rank 结果。
- [ ] 提交：`test(webrtc): add deterministic acceptance metrics (#17)`。

### Task 6: 实现单一 report model 与三种原子 writer

**Files:**
- Create: `webrtc/acceptance/src/report.js`
- Create: `webrtc/acceptance/test/report.test.js`
- Create: `webrtc/acceptance/test/golden/report-v1.json`
- Create: `webrtc/acceptance/test/golden/report-v1.md`
- Create: `webrtc/acceptance/test/golden/report-v1.xml`

**Interfaces:**

```js
function createRunReport(runIdentity, manifestSummary, environment) {}
function addCaseResult(report, caseResult) {}
function finalizeRunReport(report) {}
async function writeRunArtifacts(report, outputDirectory, redactor) {}

module.exports = {
  createRunReport, addCaseResult, finalizeRunReport, writeRunArtifacts
};
```

JSON 是 canonical source；Markdown/JUnit 从同一冻结 model 生成。每个 case 写 browser requested/observed capabilities、candidate types、selected pair、phase timestamps、threshold inputs/results、stale count、primary evidence、cleanup evidence 与 artifact hashes。writer 先在目标目录写唯一临时文件，fsync/close 后 rename；任何 writer 失败返回 `ERROR`，不得留下看似完整的正式文件。

- [ ] 写 golden tests：三格式稳定、schema 合法、case/outcome/count 一致、secret 变体全被替换、LF 稳定、artifact byte cap 生效、部分写失败不产生正式文件。
- [ ] 运行 report tests，确认失败。
- [ ] 实现 report model、canonical writer、Markdown table、JUnit testsuite/testcase mapping 和 SHA-256 artifact inventory。
- [ ] 重跑测试；连续生成两次并逐字节比较三种输出。
- [ ] 提交：`test(webrtc): add canonical acceptance reports (#17)`。

### Task 7: 实现现有 SFU API 的验收 adapter 与 fixture 生命周期

**Files:**
- Create: `webrtc/acceptance/src/http_client.js`
- Create: `webrtc/acceptance/src/sfu_adapter.js`
- Create: `webrtc/acceptance/test/sfu_adapter.test.js`
- Create: `webrtc/acceptance/test/fixtures/fake_sfu_server.js`

**Interfaces:**

```js
function createSfuAdapter(options) {}

// returned adapter
// preflight(signal)
// captureBaseline(signal)
// attachRoom(caseContext, token, signal)
// waitForPublishedTracks(caseContext, token, expectedTracks, signal)
// setViewerSubscriptions(caseContext, token, trackIds, signal)
// getWebRtcSession(caseContext, token, sessionId, signal)
// getNodeStats(token, signal)
// deleteMediaResource(resource, token, signal)
// detachRoom(caseContext, token, signal)
// waitForBaseline(baseline, token, signal)
```

HTTP client 必须设置 per-request deadline、body byte cap、严格 content type 和 JSON parse。setup 顺序固定：`/health`、`/ready`、capture node baseline、`attach_room(max_participants=2)`；WHIP publisher 建立后等待 `<publisher>-audio`/`<publisher>-video-N` 自动轨道事实，再提交 `set_track_subscription`，之后才能 WHEP。所有 mutation command 使用稳定 message id/correlation id。DELETE 接受首次 204；清理重试仅接受已经确认同一 resource 的 404，其他状态保留为 cleanup failure。

- [ ] fake SFU 覆盖 health/ready、auth scope、attach、延迟轨道注册、subscription、session/node stats、DELETE 204→404、detach、baseline 恢复；测试错误 content type、超限 body、5xx、timeout、轨道缺失、残留不归零。
- [ ] 运行 adapter test，确认失败。
- [ ] 实现 fetch-based bounded client 和 adapter；URL path segment 必须逐段 encode，token 只放 Authorization header。
- [ ] 重跑测试，断言调用顺序和 message/correlation id 稳定；确认无 token 出现在 fake server request log snapshot。
- [ ] 提交：`test(webrtc): add SFU acceptance lifecycle adapter (#17)`。

### Task 8: 实现浏览器 WHIP/WHEP 页面及其窄自动化 API

**Files:**
- Create: `webrtc/acceptance/web/client.html`
- Create: `webrtc/acceptance/web/client.js`
- Create: `webrtc/acceptance/src/sdpfrag.js`
- Create: `webrtc/acceptance/test/sdpfrag.test.js`
- Create: `webrtc/acceptance/test/browser_api_contract.test.js`

**Interfaces exposed only in the page runtime:**

```js
window.turboAcceptance = Object.freeze({
  configure,
  startPublisher,
  startViewer,
  restartIce,
  snapshot,
  close
});
```

`configure` 只在内存保存 endpoints、ephemeral tokens、TURN config 与 case identity；页面不写 storage、不把 secret 放 DOM。publisher 使用可预测的 synthetic audio/video（Web Audio oscillator + canvas capture），POST WHIP、保存 Location/ETag、trickle PATCH；viewer 建 recvonly transceivers、POST WHEP 并确认远端 track/frames。`restartIce` 用新的 generation 创建 SDP fragment，以当前 ETag PATCH，应用 response fragment/new ETag，并显式验证旧 ETag 得到 412。`snapshot` 返回规范化前原始 getStats 和 API phase evidence，不返回 credentials。

- [ ] 先写 `sdpfrag.test.js` 覆盖 ufrag/pwd/mid/candidate/end-of-candidates、CRLF、未知/重复字段、最大长度；写静态 API contract test 检查方法集合、禁止 local/sessionStorage、禁止 query token。
- [ ] 运行 tests，确认失败。
- [ ] 实现严格 SDP fragment builder/parser 与浏览器页面；所有 fetch 检查 status/content-type/Location/ETag，unexpected response 立即抛结构化错误。
- [ ] 重跑 Node tests；用现有本地 Chrome smoke 的静态服务器方式加载页面，确认 `Object.keys(window.turboAcceptance)` 精确匹配六个方法且 `snapshot()` 不含 secret key。
- [ ] 提交：`test(webrtc): add WHIP WHEP browser acceptance client (#17)`。

### Task 9: 实现 Selenium Remote WebDriver adapter 与 relay 证据校验

**Files:**
- Create: `webrtc/acceptance/src/selenium_adapter.js`
- Create: `webrtc/acceptance/test/selenium_adapter.test.js`
- Create: `webrtc/acceptance/test/fixtures/fake_webdriver.js`

**Interfaces:**

```js
function createSeleniumAdapter(options) {}

// returned adapter
// preflightBrowser(caseDefinition, signal)
// openPublisher(caseContext, signal)
// openViewer(caseContext, signal)
// execute(role, operation, argument, signal)
// collectCapabilities(role, signal)
// closeRole(role, signal)
// closeAll(signal)
```

Remote session capability 必须与 manifest 的 browser name、exact version、platform 匹配；release mode 不允许 version range。页面配置强制 `iceTransportPolicy: 'relay'`，启动后从 getStats 验证 selected candidate pair 两端 candidate type 符合 case contract，且本地候选为 relay；只有配置声明不足不能判 PASS。所有 WebDriver call 受 45 秒 deadline 包裹，publisher/viewer 各自最多一个 session。

- [ ] fake WebDriver 测试 exact match、version/platform mismatch、missing capability、command timeout、relay pair pass、host/srflx fail、missing candidate evidence incomplete、部分创建后的逆序 cleanup。
- [ ] 运行 test，确认失败。
- [ ] 实现 Selenium builder、capability normalization、页面加载/async script invocation 与有界关闭；adapter 不做 outcome 决策，只返回结构化 evidence/error category。
- [ ] 重跑测试，并在本机已安装 Chrome 上仅运行 diagnostic preflight；本机没有 Grid 时预期返回清晰 `INCOMPLETE`，不得自动切 local driver。
- [ ] 提交：`test(webrtc): add remote browser acceptance adapter (#17)`。

### Task 10: 实现 controller 的 preflight、case orchestration 与 drain

**Files:**
- Create: `webrtc/acceptance/src/controller.js`
- Create: `webrtc/acceptance/test/controller.test.js`
- Create: `webrtc/acceptance/test/fixtures/fake_lab.js`

**Interfaces:**

```js
async function preflightRun(context, signal) {}
async function runCase(context, caseDefinition, signal) {}
async function drainCase(context, caseRuntime, primaryResult, signal) {}
async function runManifest(context, manifest, signal) {}

module.exports = { preflightRun, runCase, drainCase, runManifest };
```

Preflight 必须在创建任何媒体资源前验证全部 exact browser availability、provider/hook contract、SFU health/ready、TURN reachability receipt、artifact writable/capacity 与 manifest 完整性。单 case 顺序：prepare credentials/tokens/hooks → attach room → open publisher and WHIP → confirm tracks → set subscriptions → open viewer and WHEP → stable sample → expiry boundary assertion → topology transition → ICE restart and stale ETag assertion → recovery sample → drain。credential expiry 前后均需由 monotonic clock 判定；旧凭据继续成功是 `FAIL`，实验室无法制造过期窗口是 `INCOMPLETE`。

- [ ] 用 fake lab 写表驱动测试覆盖 PASS、threshold FAIL、missing browser INCOMPLETE、provider ERROR、credential unexpectedly accepted、transition timeout、recovery timeout、SIGINT cancel、case 1 失败后 case 2 仍按 manifest policy 执行、所有路径 drain。
- [ ] 运行 controller tests，确认失败。
- [ ] 实现 AbortController 层级、状态机事件投递、phase deadlines、bounded sampling、primary/cleanup evidence 合并和 run aggregate。
- [ ] 重跑测试，断言每种注入失败都执行完整清理顺序且不会生成第二个 primary failure。
- [ ] 提交：`test(webrtc): orchestrate TURN acceptance cases (#17)`。

### Task 11: 实现 CLI、信号处理和稳定退出语义

**Files:**
- Create: `webrtc/acceptance/src/cli.js`
- Create: `webrtc/acceptance/test/cli.test.js`
- Modify: `webrtc/acceptance/package.json`

**CLI contract:**

```text
node src/cli.js run --manifest <absolute-or-relative-json> [--output <directory>] [--label <text>]
node src/cli.js validate --manifest <absolute-or-relative-json>
```

未知 flag、重复 flag、缺值、非文件 manifest 立即返回 `ERROR=4`。SIGINT/SIGTERM 只设置 terminal latch 并触发一次 drain；第二次信号允许进程退出但必须在 stderr 明确报告 artifact 可能不完整。stdout 只输出一行脱敏 summary JSON；诊断写 stderr；不打印 provider/hook 原始输出。

- [ ] 写 child-process CLI tests 覆盖 validate/pass/fail/incomplete/error 退出码、未知参数、信号一次/两次、output override 白名单、stdout 单 JSON 行、secret absence。
- [ ] 运行 CLI tests，确认失败。
- [ ] 实现 parser、dependency wiring、signal latch、report writer 调用和 `bin`/npm scripts：`test`、`validate`、`acceptance`。
- [ ] 重跑 CLI tests，并在 Windows PowerShell 路径含空格 fixture 上验证不经 shell 仍可执行。
- [ ] 提交：`test(webrtc): add acceptance runner CLI (#17)`。

### Task 12: 建立完全本地的 contract lab E2E，不伪装公网证明

**Files:**
- Create: `webrtc/acceptance/test/contract_lab.test.js`
- Create: `webrtc/acceptance/test/fixtures/contract-manifest.json`
- Create: `webrtc/acceptance/test/fixtures/fake_turn_provider.js`
- Create: `webrtc/acceptance/test/fixtures/fake_sfu_token_provider.js`
- Create: `webrtc/acceptance/test/fixtures/fake_topology_hook.js`

Contract lab 通过 fake provider/hook/SFU/WebDriver 验证 controller 与外部系统之间的完整协议，但 report 必须标记 `environment.kind=contract_lab`、`release_eligible=false`。它必须模拟短期凭据先有效后过期、relay-only pair、WHIP/WHEP resource、旧 ETag 412、网络切换 receipt、ICE generation 增加、恢复窗口、资源归零。

- [ ] 写 E2E 期望 report/golden assertions，先确认缺 fixture 失败。
- [ ] 实现各 fake 为独立进程或 HTTP server，使用真实 stdin/stdout JSON、HTTP status/header 与 async timing，不直接调用 controller 内部函数伪造成功。
- [ ] 运行 `npm test -- --test-name-pattern="contract lab"`，确认 PASS report；分别注入 host candidate、过期凭据仍接受、stale ETag 204、cleanup residue，确认对应 FAIL/ERROR。
- [ ] 检查 contract lab report 永远不能满足 release eligibility。
- [ ] 提交：`test(webrtc): add acceptance contract lab (#17)`。

### Task 13: 提供 diagnostic/release manifests、运维文档与诚实 readiness 状态

**Files:**
- Create: `webrtc/acceptance/manifests/diagnostic.chrome.example.json`
- Create: `webrtc/acceptance/manifests/release.example.json`
- Create: `webrtc/acceptance/README.md`
- Modify: `webrtc/docs/PRODUCTION_READY_SUMMARY.md`

Release example 必须列出项目批准的全部 browser/platform/exact-version case、TURN credential TTL、stable/transition/recovery threshold profiles、provider/hook executable contracts、Grid URL 和 HTTPS test page URL，但只能使用明显的非 secret 示例值。README 必须说明实验室前置条件、provider request/response schema、hook receipt、运行命令、exit code、artifact、故障分类、凭据轮换、清理审计和 #17 closure gate。

- [ ] 写 doc verification test 或现有测试中的静态检查：两个 example manifest 均通过 schema；release manifest 展开 case 数与文档表格一致；无 `password`/真实 token/private host；summary 不含“production ready”误报。
- [ ] 运行验证，确认文档/manifest 尚不存在而失败。
- [ ] 创建 examples 和 README；在 production summary 中只新增“公网 TURN 验收 harness 已具备，尚待外部 release report”的事实及命令，不关闭 #17。
- [ ] 运行两个 manifest 的 `validate`；对 release example 执行 preflight，在无外部 Grid/provider 的本机必须稳定返回 `INCOMPLETE=3` 且不创建媒体资源。
- [ ] 提交：`docs(webrtc): document public TURN acceptance runner (#17)`。

### Task 14: 全量验证、安全审计与交付检查点

**Files:**
- Modify only if verification exposes a defect in files introduced by Tasks 1–13.

- [ ] 在 `webrtc/acceptance` 运行 `npm ci`、`npm test`、`npm audit`，保存命令、版本与最终计数。
- [ ] 运行 contract lab PASS 和四个注入失败场景，逐个核对 exit code、primary evidence、cleanup evidence、三格式一致性与 secret absence。
- [ ] 运行 repository Release CTest preset，确认既有 C/C++ 测试不回归；若 preset 名称发生变化，按 `cmake-presets` skill 先检查实际 preset，不猜测命令。
- [ ] 用 `rg.exe` 扫描新增文件中的 `TODO|FIXME|HACK|Not implemented|password|credential`，人工区分 schema 字段名与泄漏；扫描 `.codegraph/`、`node_modules/`、runtime artifacts 均未进入 git index。
- [ ] 运行 `git diff --check`、`git status --short`、`git diff --stat`，审查所有改动仅在批准范围。
- [ ] 使用 `superpowers:requesting-code-review` 审查状态机、secret handling、relay proof、cleanup 与 report eligibility；修正 HIGH/MED 后重跑相关测试。
- [ ] 最终提交：`test(webrtc): deliver public TURN browser acceptance harness (#17)`；推送前再次运行 `superpowers:verification-before-completion`。没有真实外部 release PASS artifact 时，不关闭 #17。

## Implementation Completion Evidence

实现完成时交付说明必须区分：

- `事实`：列出 Node 测试、contract lab、Release CTest、manifest validation、diff/security scan 的具体命令与结果。
- `事实`：列出提交 hash、推送分支、生成但未提交的 runtime artifact 路径。
- `推论`：harness 能按 contract 驱动外部实验室；在未取得真实 Chrome/Edge/Firefox/Safari + TURN-only 公网报告前，此推论不能升级为生产就绪事实。
- `风险`：外部 Grid、TURN、NAT/网络切换 hook 与精确浏览器镜像仍由部署方提供；任何缺项都必须表现为 `INCOMPLETE`。
