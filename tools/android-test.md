# Android 真机测试与 LLDB 调试

`tools/android-test.ps1` 用于在 Windows 主机上构建一个 Android CMake 测试 target，
通过 ADB 将可执行文件及其动态库部署到 Android 设备，然后在设备 CPU 上运行测试。
脚本也可以启动 NDK `lldb-server`，通过 USB 或 WiFi ADB 使用主机 LLDB 调试测试。

当前 runner 一次处理一个可执行 target，不会把 Windows CTest 命令直接放到设备执行。

## 前置条件

- PowerShell 7 或更高版本。
- CMake、Ninja、Android SDK Platform Tools 和 Android NDK 已安装。
- `CMakeUserPresets.json` 中的 Android Windows-host preset 路径与本机环境一致。
- 同 ABI、同构建类型的 TurboUtils Android build tree 已生成；当前 Windows-host presets
  默认从相邻 `../turbo-utils/build/android-<abi>-<config>` 查找它。
- 构建通用 transport 组件时，同 ABI、同构建类型的 TurboNet Android build tree 也应已生成。
- Android preset 与 vcpkg 官方 Android triplet 统一使用 API 28（Android 9）。
- 设备 ABI 与 preset 一致。默认 preset 构建 `arm64-v8a`。
- USB 或 WiFi ADB 设备已经连接并显示为 `device`。

检查设备连接：

```powershell
adb devices -l
```

如果使用 Android 11 及以上版本的无线调试，应先在开发者选项中完成配对。具体流程参见
[Android 无线调试官方文档](https://developer.android.com/studio/run/device.html#wireless)。

## 快速开始

在仓库根目录运行：

```powershell
cmake --preset android-arm64-v8a-release-win
cmake --build --preset android-arm64-v8a-release-win --parallel
./tools/android-test.ps1 turbo_media_test_crypto -Tap
```

需要供其他项目通过 CMake package 使用时：

```powershell
cmake --build --preset install-android-arm64-v8a-release-win --parallel
```

默认安装到 `C:/projects/cpp/external/pkgs/turbomedia-android`。基础组件按既有
`TurboMedia::*` target 导出，Android mobile target 为 `TurboMedia::Android`；WebRTC
还会导出 `TurboMedia::WebRTC`、`TurboMedia::WebRTCSignaling`、
`TurboMedia::DataChannel` 与 `TurboMedia::RTC`。

默认使用：

```text
Preset:           android-arm64-v8a-release-win
Build directory: build/android-arm64-v8a-release
Remote directory:/data/local/tmp/turbomedia-tests
LLDB port:        5039
```

脚本会依次执行：

1. 解析 CMake Presets JSON 的 include 和 inherits，找到 build preset 关联的
   configure preset 及 `binaryDir`。
2. 写入 CMake File API codemodel query，并使用 configure preset 重新 configure。
3. 使用 build preset 只构建指定 target。
4. 从 File API reply index 读取 target 类型、实际 executable artifact 和项目库 artifacts。
5. 检查目标 ELF 与设备 ABI 是否匹配。
6. 读取 ELF `DT_NEEDED`，递归定位非系统动态库。
7. 部署测试、项目 `.so`、NDK `libc++_shared.so` 等必要文件。
8. 在设备的 `/data/local/tmp` 目录运行测试。
9. 返回设备测试进程的退出状态。

测试退出状态非零时，脚本以失败结束，不会把失败报告成成功。

### CMake 元数据来源

runner 不解析 CMake 或 CTest XML，也不从 configure 输出文本猜测路径：

| 信息 | 事实源 |
|---|---|
| build preset 对应哪个 configure preset | `CMakePresets.json` / `CMakeUserPresets.json` |
| build tree | configure preset 继承后的 `binaryDir` |
| target 类型和实际 artifact | CMake File API codemodel JSON |
| NDK 路径 | configure preset 继承后的 `environment` / `cacheVariables` |
| triplet 等已配置值 | `CMakeCache.txt` |
| ABI 和运行时共享库 | ELF header 与 `DT_NEEDED` |
| 测试结果 | 设备进程退出状态及可选 TinyTest JUnit XML |

CTest `Testing/*/Test.xml` 是测试结果，不包含可靠的 CMake target artifact 和工具链信息，
因此不用于部署发现。

脚本只读取 preset JSON，不会更新或回写 `CMakePresets.json` / `CMakeUserPresets.json`。
NDK 优先取合并后的 `ANDROID_NDK_HOME`，否则从
`VCPKG_CHAINLOAD_TOOLCHAIN_FILE` 的 `android.toolchain.cmake` 路径确定，并验证该目录。

格式与字段语义以 [CMake Presets 官方手册](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)
和 [CMake File API 官方手册](https://cmake.org/cmake/help/latest/manual/cmake-file-api.7.html)
为准。File API reply 文件名是不透明的，必须从 index 跟随 `jsonFile` 引用。

### Android API 的单一配置源

项目的最低 Android API 只在 `presets/AndroidPresets.json` 的 `android-base` 中设置：

```json
"ANDROID_PLATFORM": "android-28"
```

当前 vcpkg 官方 `arm64-android`、`x64-android` 与 `x86-android` triplet
也使用 API 28，因此项目及静态依赖具有相同的 libc 符号边界。不要让 PowerShell runner
根据设备版本回写 preset；设备 API 不是构建契约。runner 只读取 configure 后的
`CMakeCache.txt` 并验证产物 ABI。若以后改变最低 API，应同时确认 vcpkg triplet 的
`VCPKG_CMAKE_SYSTEM_VERSION`，否则静态库可能引用主工程最低 API 不提供的 libc 符号。

## 选择 WiFi 设备

只有一个在线设备时不需要指定 serial。存在多个设备时，使用 `-Serial`：

```powershell
./tools/android-test.ps1 turbo_media_test_crypto `
  -Serial "adb-38101FDJG00AVU-Rx6MV9._adb-tls-connect._tcp"
```

`-Serial` 接受 `adb devices` 输出的完整第一列。对 runner 而言，USB 和 WiFi ADB
使用相同的部署、执行和调试流程。

## TinyTest 选项

### 过滤测试

```powershell
./tools/android-test.ps1 turbo_media_test_crypto -Filter "X25519"
```

这会在设备端传入：

```text
--filter "Basic Types"
```

### TAP 输出

```powershell
./tools/android-test.ps1 turbo_media_test_crypto -Tap
```

### JUnit 输出

`-JUnit` 接受主机输出路径。XML 先在设备生成，测试结束后再拉取到主机：

```powershell
./tools/android-test.ps1 turbo_media_test_crypto `
  -Filter "X25519" `
  -JUnit artifacts/turbo_media_test_crypto.android.xml
```

如果父目录不存在，脚本会创建它。测试失败时仍会尝试拉取已经生成的 JUnit 文件。

### 其他 TinyTest 参数

使用 `-TestArgument` 传递 runner 没有单独封装的参数：

```powershell
./tools/android-test.ps1 turbo_media_test_crypto `
  -TestArgument @('--list', '--no-color')
```

TinyTest 常用参数包括：

```text
--list, -l
--filter <pattern>, -f <pattern>
--tap
--junit <file>
--color
--no-color
--help, -h
```

## 使用已有构建产物

跳过 configure 和 build：

```powershell
./tools/android-test.ps1 turbo_media_test_crypto -NoBuild
```

`-NoBuild` 仍会检查 CMake cache、ELF、动态库和设备 ABI。如果指定 target 尚未生成，
脚本会立即报错。

## 测试数据和额外动态库

### 部署测试数据

相对路径测试依赖 fixture、配置或样例文件时，通过 `-Data` 部署：

```powershell
./tools/android-test.ps1 turbo_media_test_crypto -Tap -Data tests
```

文件和目录会被推送到远程工作目录。测试默认以该目录为当前目录运行。

### 补充动态库

脚本会自动解析可执行文件和已发现 `.so` 的 `DT_NEEDED`。TurboMedia 依赖外部
TurboUtils build tree；链接 transport 的目标还会依赖 TurboNet。最小 crypto 测试只需：

```powershell
./tools/android-test.ps1 turbo_media_test_crypto `
  -LibraryDirectory '../turbo-utils/build/android-arm64-v8a-release/bin'
```

若只有单个库位于非标准位置，也可以用 `-Library <file>` 显式部署。无法解析非系统
依赖时，脚本 fail fast，并在错误信息中给出缺失的库名。

Android mobile 测试示例：

```powershell
./tools/android-test.ps1 turbo_media_test_mobile_optimizer -Tap `
  -LibraryDirectory @(
    '../turbo-utils/build/android-arm64-v8a-release/bin',
    '../turbonet/build/android-arm64-v8a-release/bin'
  )
```

## WebRTC 与 TurboHTTP 链路

Android presets 构建全部功能。ARM64 Release 使用
已安装的 `C:/projects/cpp/external/pkgs/turbohttp-android`；其他 ABI/配置从
`C:/projects/cpp/TurboHTTP/build/android-<abi>-<config>` 查找。对应 TurboHTTP package
必须先构建，且 TurboHTTP、TurboNet、TurboUtils、TurboMedia 必须使用相同 ABI、构建类型、
NDK 与 Android API。

WiFi 真机运行 WebRTC signaling 测试：

```powershell
./tools/android-test.ps1 turbo_media_test_signaling_lifecycle -Tap `
  -Serial "adb-38101FDJG00AVU-Rx6MV9._adb-tls-connect._tcp" `
  -LibraryDirectory @(
    'C:/projects/cpp/external/pkgs/turbohttp-android/lib',
    '../turbonet/build/android-arm64-v8a-release/bin',
    '../turbo-utils/build/android-arm64-v8a-release/bin'
  )
```

RTC 与 DataChannel 可分别使用 `turbo_media_test_media_engine` 和
`turbo_media_test_datachannel`。runner 会从 ELF `DT_NEEDED` 递归部署实际依赖，不需要手写
每个 `.so` 文件名。

## 音频、视频与屏幕共享测试

捕获后端的 native 真机测试使用 `turbo_media_test_android_capture`。它不是只检查对象能否
创建，而是等待设备产生真实数据：

- OpenSL ES 麦克风至少回调一批 PCM 数据；
- Camera2 后置摄像头至少回调一帧 `640x480` I420 数据；
- 音频、视频和屏幕对象的启动、停止及状态回调保持一致；
- Android 尚未实现的 Camera2 控制和裁剪接口必须明确返回
  `TURBO_CAPTURE_ERR_UNSUPPORTED`，并清空输出参数；
- 设备枚举同时覆盖音频、前后摄像头、屏幕和当前为空的 GPU 捕获列表。

WiFi 设备示例：

```powershell
./tools/android-test.ps1 turbo_media_test_android_capture `
  -Serial "adb-38101FDJG00AVU-Rx6MV9._adb-tls-connect._tcp" `
  -JUnit artifacts/turbo_media_test_android_capture.xml `
  -LibraryDirectory '../turbo-utils/build/android-arm64-v8a-release/bin'
```

`turbo_media_test_android_capture` 中的屏幕用例只能验证 native ImageReader surface 和生命周期。
MediaProjection 必须由 Activity 发起系统授权，命令行 ELF 无法独立取得投屏 token。真实屏幕帧
使用单独的交互式 APK 测试：

```powershell
./tools/android-screen-test.ps1 `
  -Serial "adb-38101FDJG00AVU-Rx6MV9._adb-tls-connect._tcp"
```

脚本按以下顺序 fail fast：构建 `turbo_media_android`、用已安装的 Android SDK/JDK 生成并签名
测试 APK、安装和启动 Activity、等待用户在系统对话框中允许录屏，再等待 native ImageReader
取得 RGBA 帧并通过 libyuv 转换成 I420。只有 APK 内部的 native 帧计数大于零才输出 PASS；默认
超时 90 秒，并在结束后卸载测试 APK。此 APK 的 `minSdkVersion` 和 `targetSdkVersion` 都是 28，
与当前 Android preset 的 API 基线一致。

可用参数：

- `-NoBuild`：复用已有 native 库；
- `-BuildDirectory`：覆盖默认的 `build/android-arm64-v8a-release`；
- `-TimeoutSeconds`：调整等待授权和首帧的时间；
- `-KeepInstalled`：调试 Activity 时保留测试 APK。

系统授权属于真实屏幕共享的安全边界，脚本不会通过 shell 权限绕过它。CI 若无人操作，只运行
native 捕获测试；MediaProjection APK 测试应放到有人确认授权的设备任务中。

### 从其他 Android 项目消费安装包

consumer 的 Android user preset 应提供这些精确 package 目录：

```json
{
  "TurboMedia_DIR": "C:/projects/cpp/external/pkgs/turbomedia-android/lib/cmake/TurboMedia",
  "TurboHttp_DIR": "C:/projects/cpp/external/pkgs/turbohttp-android/lib/cmake/TurboHttp",
  "TurboNet_DIR": "C:/projects/cpp/turbonet/turbonet/build/android-arm64-v8a-release",
  "TurboUtils_DIR": "C:/projects/cpp/turbonet/turbo-utils/build/android-arm64-v8a-release"
}
```

随后按组件加载并链接：

```cmake
find_package(TurboMedia REQUIRED COMPONENTS RTC WebRTCSignaling)

target_link_libraries(
  my_android_target
  PRIVATE TurboMedia::RTC
          TurboMedia::WebRTCSignaling
          TurboMedia::Android)
```

consumer 还必须通过相同 `arm64-android` vcpkg installed tree 提供 OpenSSL、libSRTP 等
静态依赖。不能把 Windows package 或另一 ABI 的 package 放进 Android 查找路径。

## 选择其他 Android preset

ARM64 Debug 示例：

```powershell
./tools/android-test.ps1 turbo_media_test_crypto `
  -Preset android-arm64-v8a-debug-win
```

脚本会解析 preset include、inherits 和 `binaryDir`，所以 preset 名和 build tree 不需要遵循
固定映射。只有 preset 没有可解析的 `binaryDir`，或者需要显式覆盖时，才使用
`-BuildDirectory`：

```powershell
./tools/android-test.ps1 turbo_media_test_crypto `
  -Preset my-android-preset `
  -BuildDirectory build/my-android-tree
```

Release 构建可能包含调试信息但仍受优化影响。需要逐语句调试或稳定查看局部变量时，
优先使用不启用不兼容 sanitizer 的 Debug 配置。

## 在其他项目中使用

可以在多个项目中统一 preset 名称和相对输出路径。统一约定便于人工操作，但 runner
已经从 Presets JSON 和 File API 读取实际路径，因此这些名称与路径是推荐值，不是硬编码
要求：

| 用途 | 统一值 |
|---|---|
| ARM64 Release configure/build preset | `android-arm64-v8a-release-win` |
| ARM64 Debug configure/build preset | `android-arm64-v8a-debug-win` |
| Release build tree | `build/android-arm64-v8a-release` |
| Debug build tree | `build/android-arm64-v8a-debug` |
| executable/shared library 输出 | `<build tree>/bin` |
| static library 输出 | `<build tree>/lib` |

configure preset 和 build preset 属于不同 preset 类型，可以使用相同名称；同一类型中的
每个名称仍必须唯一。不同项目则可以使用完全相同的名称。

直接复用还需要满足下列约定：

- 脚本位于项目根目录的 `tools/` 下。
- `-Preset` 指向一个 build preset，且它能解析出关联 configure preset。
- configure preset 能通过自身或 inherits 解析出 `binaryDir`；否则显式传入
  `-BuildDirectory`。
- configure 后能够生成 CMake File API codemodel，目标类型为 `EXECUTABLE`。
- configure preset 通过继承后的 `ANDROID_NDK_HOME` 或
  `VCPKG_CHAINLOAD_TOOLCHAIN_FILE` 指定 NDK；vcpkg 是可选的。
- 测试命令行与 TinyTest 选项兼容，或者只使用 `-TestArgument`。

项目库可以输出到任意目录；runner 从 codemodel artifacts 查找，不要求统一放在 `bin/`。
TurboMedia 的 `turbo_media_android` 目前输出到
`media/mobile/android/libs/<abi>`，正是这一规则的实际用例。
满足这些条件的项目可以直接复制或共享 `tools/android-test.ps1`。不符合条件时，不应通过
硬编码更多项目路径继续扩展本脚本。更合适的结构是：

1. 通用 Android ELF runner 只负责 ABI 检查、依赖解析、ADB 部署和 LLDB。
2. 每个项目的 wrapper 负责 configure、build、target 路径和测试框架参数。

如果后续项目不能统一这些约定，再把构建 wrapper 与 Android ELF runner 分离；在此之前，
统一 preset 契约仍比增加更多脚本参数更清晰。

## 交互式 LLDB 调试

```powershell
./tools/android-test.ps1 turbo_media_test_crypto -Lldb
```

脚本会：

1. 从当前 NDK 选择与 ELF 架构匹配的 `lldb-server`。
2. 将 `lldb-server` 部署到设备。
3. 在设备 localhost 上启动 GDB remote protocol listener。
4. 使用 `adb forward` 映射到主机端口。
5. 使用 NDK `lldb.cmd` 启动主机 LLDB，并加载本地未剥离 ELF 和 `.so` 符号。
6. 调试结束后清理 ADB forward 和脚本启动的远程 server。

连接后测试会先停在初始 `SIGSTOP`。例如：

```text
(lldb) breakpoint set --name main
(lldb) process continue
(lldb) thread backtrace
(lldb) process continue
```

脚本使用设备 localhost TCP 转发，不依赖 `adb root`。这是为了兼容不能使用 AOSP
`gdbclient.py` filesystem socket 流程的普通 Pixel user build。

AOSP 当前的 `lldbclient.py` 是指向历史名称 `gdbclient.py` 的符号链接，其自动化思路包括
部署 `lldb-server`、ADB forwarding 和符号路径配置。源码参见
[AOSP gdbclient.py](https://android.googlesource.com/platform/development/+/refs/heads/main/scripts/gdbclient.py)。

## 脚本化 LLDB 会话

使用 `-LldbCommand` 在连接后执行额外命令：

```powershell
./tools/android-test.ps1 turbo_media_test_crypto `
  -NoBuild `
  -Lldb `
  -LldbCommand @(
    'breakpoint set --name main',
    'process continue',
    'thread backtrace',
    'process continue',
    'quit'
  )
```

这适合复验断点解析和调用栈，但不能替代人工调试复杂状态。

更换端口：

```powershell
./tools/android-test.ps1 turbo_media_test_crypto -Lldb -Port 5040
```

端口必须在主机与设备上均未被其他进程占用。

## 完整参数

```text
Target            必填，CMake 可执行 target 名
Preset            Android build preset
Serial            adb devices 输出的设备 serial
BuildDirectory    可选的 CMake build tree 覆盖值
RemoteDirectory   /data/local/tmp 下的设备工作目录
Filter            TinyTest filter
Tap               启用 TAP 输出
JUnit             主机 JUnit XML 输出路径
Data              额外部署的测试文件或目录
Library           额外部署的共享库
LibraryDirectory  额外的共享库依赖搜索目录
TestArgument      传给设备程序的额外参数
Lldb              使用 LLDB 调试
LldbCommand       连接后执行的 LLDB 命令
Port              LLDB 主机和设备 localhost 端口
NoBuild           跳过 configure 和 build
```

PowerShell 内置帮助也包含参数和示例：

```powershell
Get-Help ./tools/android-test.ps1 -Detailed
```

## 常见错误

### 没有在线设备

```text
No online ADB device was found
```

重新启用设备无线调试，确认主机和设备处于可通信的网络，然后检查：

```powershell
adb devices -l
```

### 存在多个设备

使用 `-Serial` 明确选择目标设备。脚本不会猜测应该在哪一台设备运行。

### ABI 不匹配

例如 ARM64 test 不能部署到 x86_64 emulator。改用匹配设备 ABI 的 preset，或连接正确设备。

### 找不到共享库

先确认对应 CMake target 已链接并生成必要 `.so`。第三方库集中在一个非标准输出目录
时使用 `-LibraryDirectory`；只有单个库时使用 `-Library`。不要通过忽略缺失库继续执行。

### 找不到 CMake File API reply

首次使用时不要传 `-NoBuild`。runner 需要先写入 codemodel query 并执行一次 configure，
之后 `-NoBuild` 才能复用现有 reply。reply 文件名由 CMake 生成，脚本始终通过最新
`index-*.json` 跟随引用，不按文件名模式猜测 target JSON。

### LLDB 无法连接

- 确认 `-Port` 没有被主机或设备占用。
- 确认设备仍显示为 `device`。
- 确认 NDK 同时包含主机 `lldb.cmd` 和目标架构 `lldb-server`。
- 先运行不带 `-Lldb` 的测试，区分部署/运行错误与调试连接错误。

### vcpkg triplet 切换

Android 与 Windows presets 当前共用仓库的 `vcpkg_installed`。Android configure 可能把
其中的目标包切换到 Android triplet；之后运行 Windows preset configure 会恢复 Windows
目标包。这是当前 preset 布局的行为，不应通过复制头文件或静默 fallback 绕过。

## 最小复验命令

```powershell
./tools/android-test.ps1 turbo_media_test_crypto `
  -Tap `
  -LibraryDirectory '../turbo-utils/build/android-arm64-v8a-release/bin'
```

需要验证调试链路时：

```powershell
./tools/android-test.ps1 turbo_media_test_crypto `
  -Lldb `
  -LibraryDirectory '../turbo-utils/build/android-arm64-v8a-release/bin' `
  -LldbCommand 'breakpoint set --name main','process continue','thread backtrace','process continue','quit'
```

本次迁移已在 ARM64 Pixel WiFi ADB 设备复验：

- `turbo_media_test_crypto`：5 个测试、34 个断言通过。
- `turbo_media_test_mobile_optimizer`：25 个测试通过。
- `turbo_media_test_signaling_lifecycle`：3 个测试通过，覆盖 Iris HTTP API 与原生 WebSocket 生命周期。
- `turbo_media_test_webrtc`：7 个测试通过。
- `turbo_media_test_datachannel`：28 个测试通过。
- `turbo_media_test_media_engine`：2 个测试通过。
- `turbo_media_test_android_capture`：5 个测试、45 个断言通过，覆盖真实 PCM 与 Camera2 I420 帧。
- `turbo_media_test_capture`：21 个捕获回归测试通过。
- `android-screen-test.ps1`：MediaProjection 测试 APK 收到 5 个 native I420 屏幕帧。
- LLDB 命中 `turbo_media_test_crypto` 的 `main`、输出源码 backtrace，并继续运行至退出状态 0。
