# TurboMedia 消费 TurboUtils Capture 迁移设计

## 背景与目标

TurboUtils 已将音频、摄像头和屏幕采集发布为可选组件
`TurboUtils::Capture`。TurboMedia 当前仍编译并安装同名 `turbo_capture_*`
实现，导致两个仓库可以同时提供相同 C 符号、公开头文件和平台状态。

本改动让 TurboUtils 成为 Capture 的唯一事实源，同时保留
`TurboMedia::Device` 作为现有播放组件和兼容链接入口。现有源码继续使用
`#include <turbo_capture.h>`，不改函数名、结构布局、回调签名或返回码。

## 现状证据与影响面

- `media/CMakeLists.txt` 同时构建播放和全部桌面/Android Capture 源码；
  `media/include/turbo_capture.h` 随 TurboMedia 安装。
- `tests/test_capture.c`、`tests/test_integration.c`、示例和 WebRTC 媒体引擎均通过
  `TurboMedia::Device` 获得 Capture 头文件与符号。
- `player`、Android 和 iOS 组件公开链接 `TurboMedia::Device`，因此不能删除该目标。
- Android Java `ScreenCapture` JNI 直接使用 `android_screen_*` 完成
  MediaProjection 授权后的 Surface 交接；TurboUtils 当前公开 C API 没有暴露该
  平台桥接。
- 未修改树的 Windows Release `turbo_media_test_capture` 已构建并通过，作为迁移
  前行为基线。

## 目标结构

`turbo_media_device` 只编译 miniaudio 播放实现，并公开链接
`TurboUtils::Capture`。这样所有现有 `TurboMedia::Device` 消费者自动继承 Capture
头文件目录和库依赖，源码不需要逐个改链接目标。

TurboMedia 删除自己的公开 Capture 头文件、桌面实现、Android 音频/摄像头/公共
Capture 调度实现和 iOS Capture 实现。根配置在发现 TurboUtils 后立即检查
`TurboUtils::Capture`；安装包配置也给出明确缺失消息，禁止在旧 SDK 上静默退回本地
实现。

Android `turbo_media_android` 暂时保留并直接编译
`src/capture/capture_screen_android.c`。该文件只实现 Java MediaProjection 的私有
Surface 适配符号，不定义任何 `turbo_capture_*` 公共入口；它由
`turbo_media_android` 独占。等 TurboUtils 提供稳定的 Android Surface 交接 API 后，
可在单独的公开接口迁移中删除此适配器。

## 接口、状态与所有权

- 公开 C Capture ABI 不变；提供者从 `turbo_media_device` 变为
  `TurboUtils::Capture`。
- `TurboMedia::Device` 目标名和播放 API 不变，只新增公开目标依赖。
- Capture 对象、平台句柄和控制面状态由 TurboUtils 独占。调用方保持串行
  `create -> start -> stop -> destroy`；启动失败继续显式向上传播。
- 帧回调中的字节是同步 borrowed view，仅在回调返回前有效。跨队列、线程或协程的
  调用方必须在回调内复制；本迁移不新增缓冲、队列、容量或背压路径。
- Android Java Surface 适配器只拥有自己的 `AImageReader`/`ANativeWindow` 上下文，
  不拥有或镜像 `turbo_capture_t` 状态。

## 构建与包契约

TurboMedia 配置要求其 `TURBOUTILS_ROOT` 指向启用了
`TURBO_ENABLE_CAPTURE=ON` 的安装树。`TurboMediaConfig.cmake` 在加载导出目标前验证
`TurboUtils::Capture`，因此旧 TurboUtils 包会以可操作错误终止。

包消费测试请求 `Device` 组件、链接 `TurboMedia::Device`、包含
`<turbo_capture.h>` 并引用一个无硬件依赖的 FPS 辅助函数。测试同时断言
`find_package(TurboMedia)` 后存在 `TurboUtils::Capture`，覆盖依赖发现、头文件传播和
链接闭包。

## 风险、兼容性与验证

- **HIGH — 重复符号（事实）**：若任一 TurboMedia 公共 Capture 实现残留，进程可
  同时加载两个同名 ABI 提供者。通过源码检索、链接映射和包消费测试验证只剩
  TurboUtils 提供者。
- **MED — SDK 前置条件（事实）**：旧 TurboUtils 安装树没有 Capture 目标，新版
  TurboMedia 将在配置时明确失败。验证错误消息和 Capture-enabled SDK 的成功路径。
- **MED — 平台验证范围（事实）**：Windows 可在当前主机执行；Android 可交叉编译，
  Linux/macOS/iOS 仍需原生 runner 验证链接和设备生命周期，不从源码检查推断成功。
- **LOW — 导出宏变化（事实）**：头文件导出宏从 `TURBO_MEDIA_API` 变为
  `TURBO_CAPTURE_API`，C 名称与数据布局保持不变。
- **MED — Android 私有桥接（推论）**：继续保留私有 Surface 适配器避免 Java API
  回归，但形成一个待消除的平台适配边界；删除它必须先在 TurboUtils 增加经用户确认
  的公开交接 API。

验证顺序为：依赖契约 RED、Windows focused build/test、相邻集成测试、安装树消费、
全量 Windows Release 测试，以及可用的 Android arm64 交叉构建。

## 回滚

单次提交回滚可恢复 TurboMedia 的本地 Capture 文件和原目标源列表。由于迁移不改变
调用方 API 或状态数据，不需要数据迁移；回滚后应同时恢复旧 TurboUtils SDK 前置条件。
