# TurboMedia 消费 TurboUtils Capture 执行计划

> **执行要求：** 按 `superpowers:executing-plans` 分批执行，并在每批后核对构建与测试证据。

**目标：** 让 `TurboUtils::Capture` 成为 TurboMedia Capture 的唯一公共实现，同时保持
`TurboMedia::Device`、现有 C 调用点和 Android Java ScreenCapture 行为兼容。

**架构：** Device 继续拥有播放并公开转发 Capture 目标；删除重复公共头文件和原生
后端。Android 仅保留 Java MediaProjection 所需的私有 Surface 适配器。

**技术栈：** C11、Objective-C、CMake Presets、vcpkg、TinyTest、TurboUtils 安装包。

**设计：** `docs/superpowers/specs/2026-08-26-turboutils-capture-consumer-migration-design.md`

## 执行状态（2026-08-26）

迁移、重复源码删除、包依赖契约和文档已实现。Windows Release fresh configure、
完整构建与安装成功，83/83 CTest 通过；安装树 package consumer 成功配置、编译、链接
并运行。TurboUtils SDK 来源是已合并提交 `a236ced`，且安装树已验证包含
`TurboUtils::Capture`、头文件和 DLL/import library。

当前主机有 Android NDK 28.2，但 TurboMedia 所需的七个 Android first-party 安装树
（TurboUtils、TurboParser、TurboNet、TurboHTTP、FlowMQ、TurboDB、RulesForge）均不存在，
因此没有把依赖缺失误报为 Android 源码验证成功。Linux、macOS、iOS 和 Android 原生/
交叉构建仍由对应具备完整依赖的 runner 验证。

## Task 1：建立包依赖契约 RED

**文件：**

- 修改 `tests/package_consumer/CMakeLists.txt`
- 修改 `tests/package_consumer/main.c`

- [ ] 请求 `Device` 组件并断言 `TurboUtils::Capture` 目标存在。
- [ ] 链接 `TurboMedia::Device`，包含 `<turbo_capture.h>`，调用
      `turbo_video_mode_fps` 验证编译和链接传播。
- [ ] 对旧 TurboUtils 安装树运行包消费配置，确认因缺失 Capture 目标按预期失败。

## Task 2：切换构建与安装契约

**文件：**

- 修改 `CMakeLists.txt`
- 修改 `media/CMakeLists.txt`
- 修改 `cmake/TurboMediaConfig.cmake.in`
- 修改 `media/mobile/android/CMakeLists.txt`
- 修改 `media/mobile/ios/CMakeLists.txt`

- [ ] `find_package(TurboUtils)` 后 fail fast 检查 `TurboUtils::Capture`。
- [ ] Device 移除全部公共 Capture 源和平台库，公开链接
      `TurboUtils::Capture`，保留 miniaudio 播放实现。
- [ ] 删除根级 Linux PipeWire/X11 查找；这些依赖改由 TurboUtils Capture 私有拥有。
- [ ] Android 组件编译唯一的 Java Surface 适配源并保留其 NDK/libyuv 私有依赖。
- [ ] iOS 组件删除 Capture 源，只保留硬件编解码与移动优化源。
- [ ] 安装包配置对缺少 Capture 的 TurboUtils 给出明确错误。

## Task 3：删除重复实现并更新文档

**文件：**

- 删除 `media/include/turbo_capture.h`
- 删除 `media/capture/` 下全部 Capture 源与私有头
- 删除 Android `capture_android.c`、`capture_audio_android.c`、
  `capture_video_android.c`
- 保留 Android `capture_screen_android.c` 私有 Surface 适配器
- 删除 `media/mobile/ios/src/capture/` 下 Capture 源与私有头
- 修改 `media/README.md`

- [ ] 用精确文件清单删除，不递归删除未核实目录。
- [ ] 搜索确认 TurboMedia 不再定义任何 `turbo_capture_*` 公共符号。
- [ ] 文档说明依赖目标、SDK 启用方式、所有权和 Android 例外边界。

## Task 4：安装合并后的 TurboUtils Capture SDK

- [ ] 从 TurboUtils `origin/master` 的干净源码配置
      `win-capture-release-user`。
- [ ] 构建并安装到 TurboMedia Release preset 使用的 `TURBOUTILS_ROOT`。
- [ ] 验证安装树含 `turbo_capture.h`、Capture 库和导出目标。
- [ ] 若 Android 依赖树可用，安装 arm64 Capture-enabled SDK；否则记录准确缺口。

## Task 5：GREEN 与回归验证

- [ ] fresh configure TurboMedia `win-release-user`。
- [ ] 构建并运行 `turbo_media_test_capture`。
- [ ] 构建并运行 `turbo_media_test_integration` 与相关 WebRTC 测试。
- [ ] 安装 TurboMedia 后配置、构建并运行 package consumer。
- [ ] 运行 Windows Release 全量 CTest。
- [ ] 尝试 Android arm64 交叉构建，区分环境缺失与代码失败。

## Task 6：差异审查与交付

- [ ] `codegraph affected` 和 `rg.exe` 复核影响面及重复符号。
- [ ] 检查 `git diff --check`、工作树状态和未跟踪产物。
- [ ] 按重要性记录残余平台验证风险。
- [ ] 提交分支；只有用户再次明确要求时才推送、开 PR 或合并。
