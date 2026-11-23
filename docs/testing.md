# TurboMedia 测试套件

全面的 TDD/BDD 测试套件，基于 TinyTest 框架构建。

Windows Release 使用 `msvc-release-user` 的 configure/build/test
preset，须在 VS 开发环境中执行。Salts、SaltsUtils、CHttp 与 SaltsNet 的
运行库应来自匹配的 SDK；旧 `SaltsICE.dll` 对旧 Core/CNet 的依赖不能通过
复制旧 DLL 到新 build 目录解决。

TLS 用例使用 `webrtc/tests/fixtures/tls/localhost-cert.pem` 中的 localhost
叶证书及测试根 CA 链，配套密钥仅用于测试。证书具有显式 BasicConstraints、
KeyUsage、serverAuth 与仅含 localhost 的 SAN，有效期为 2026-03-01 至
2036-03-01。更新 fixture 时需保留未配置可信 CA 时拒绝连接、显式信任与
正确 server name 时连接成功的断言，不关闭生产证书验证。

在 VS x64 环境执行：

```powershell
cmake --preset msvc-release-user
cmake --build --preset msvc-release-user
ctest --preset msvc-release-user --output-on-failure
```

IVR 协议专项可用 `-R "^test_ivr_(frame|protocol|schema|schema_typed)$"`，
覆盖新 envelope/二进制字节、旧主版本拒绝和 generated/dynamic 互通。
Iris/ORM 模块及对应 PostgreSQL live gate 已随主线退役，不属于当前测试图。

## 测试文件概览

测试源码和夹具放在所属模块的 `tests/` 目录，由该模块注册。各模块的测试可从同一个构建目录通过 CTest 运行。

| 路径 | 覆盖范围 |
|------|---------|
| `sip/tests/`、`sip/sdp/tests/` | SIP 客户端、RTP/RTCP 通话链路、SDP |
| `media/tests/`、`media/<模块>/tests/` | 采集、编解码、播放、容器、服务运行时，以及跨媒体模块集成和基准 |
| `media/mobile/android/tests/` | Android 采集、移动优化和屏幕采集夹具 |
| `streaming/{rtsp,streamer,pipeline}/tests/` | RTSP、HLS、DASH、HTTP-FLV、RTMP 和流媒体管线 |
| `webrtc/tests/` | WebRTC、RTC 客户端与音视频适配 |
| `streaming/transport/tests/` | HTTP 传输和 URL |

### 核心功能测试

| 测试文件 | 测试类型 | 测试模块 | 说明 |
|---------|---------|---------|------|
| `test_codec.c` | 单元测试 | 编解码器 | 音频/视频编解码器注册、配置、编解码操作、生命周期管理 |
| `test_capture.c` | BDD测试 | 捕获设备 | 音频/视频/屏幕捕获设备枚举、配置、回调、生命周期 |
| `test_player_audio.c` | 集成测试 | 音频播放 | 播放完成、主动停止和 Salts 播放配置校验 |
| `test_rtsp.c` | 集成测试 | RTSP协议 | RTSP 服务器/客户端功能（需 RTSP 模块） |
| `test_rtsp_lib.c` | 单元测试 | RTSP库 | RTSP 消息解析、SDP 处理（需 RTSP 模块） |

### 高级功能测试

| 测试文件 | 测试类型 | 说明 |
|---------|---------|------|
| `test_mobile_optimizer.c` | 功能测试 | 移动平台优化：电池管理、网络自适应、硬件编码推荐 |
| `test_integration.c` | 集成测试 | 端到端工作流：捕获→编码→解码→播放完整管道测试 |
| `test_benchmark.c` | 性能测试 | 编解码性能基准、设备枚举性能、内存操作性能、端到端延迟 |

### 测试辅助文件

- **`media/tests/helpers.h`** - 测试宏、数据生成器、内联辅助函数
- **`media/tests/helpers.c`** - 音频分析、性能计时、文件 I/O，由 `turbo_media_test_helpers` 提供

## 构建和运行测试

### 构建所有 Windows 测试

在 VS x64 开发环境执行：

```powershell
cmake --preset msvc-release-user
cmake --build --preset msvc-release-user
```

### 运行测试

```powershell
ctest --preset msvc-release-user --output-on-failure
ctest --preset msvc-release-user -R "^turbo_media_test_(transport_http|auth_token|signaling_lifecycle)$" --output-on-failure
```

平台构建和测试矩阵见 [Platform Build](../.github/workflows/platform-build.yml)。

### 添加新测试

1. 在所属模块的 `tests/` 创建 `test_<name>.c`。
2. 在模块的 CMake 中沿用现有注册方式，列出源码和所需的库。
3. 若需要共用的媒体测试辅助函数，链接 `turbo_media_test_helpers` 并包含 `helpers.h`。
4. 重新配置和构建后，通过 `ctest -R <测试名> --output-on-failure` 运行。

```c
#include "helpers.h"
#include <turbo_xxx.h>
#include <tinytest.h>

suite("我的测试套件") {
    it("应该通过基本测试") {
        int result = my_function();
        check_equal(result, 0);
    }
}

int main(int argc, char *argv[]) {
    return test_run_all();
}
```

## 测试覆盖范围

### test_codec.c - 编解码器测试

**测试段**：
1. 注册管理 - 编解码器注册、查找、能力查询
2. 配置验证 - 参数有效性、边界条件
3. 基本操作 - 编码、解码、重置
4. 生命周期 - 创建、销毁、资源管理
5. 错误处理 - 无效参数、空指针、错误状态
6. 高级功能 - 比特率控制、关键帧、丢包隐藏
7. 设备集成 - 硬件编解码器支持

**支持的编解码器**：
- 音频：G.711 (A-law/μ-law), Opus
- 视频：H.264, H.265, VP8, VP9

### test_capture.c - 捕获设备测试（BDD 风格）

**行为场景**：
- 设备枚举应返回可用设备列表
- 配置验证应拒绝无效参数
- 捕获生命周期应正确管理资源
- 回调机制应在数据到达时触发
- 错误处理应返回正确错误码

**测试类型**：
- 音频捕获（麦克风）
- 视频捕获（摄像头）
- 屏幕捕获（桌面录制）

### test_player_audio.c - 音频播放测试

- 关闭设备播放后打开音频夹具。
- 自然结束只通知一次，主动停止时不发出完成通知。
- 拒绝无效的播放设备标识、采样率和缓冲区配置。

### test_mobile_optimizer.c - 移动平台优化测试

**优化策略测试**：
- 电池级别自适应（充电/高电量/中电量/低电量/极低电量）
- 网络类型自适应（WiFi/4G/3G/2G）
- 硬件编解码器推荐（平台特定）
- Simulcast 推荐（多码率自适应）
- 信号强度处理（强/中/弱）

**测试维度**：
- 单因素优化（只考虑电池或网络）
- 多因素联合优化（电池+网络+信号）
- 参数验证和边界情况
- 真实使用场景模拟

### test_integration.c - 集成测试

**端到端管道**：
1. Capture → Encode - 捕获数据编码管道
2. Encode → Decode - 编解码往返测试
3. Decode → Playback - 解码播放管道
4. 完整音频流程 - 捕获→编码→解码→播放
5. 错误传播 - 跨组件错误处理
6. 资源管理 - 跨组件资源清理
7. 性能测试 - 吞吐量和延迟
8. 状态一致性 - 跨组件状态同步

### test_benchmark.c - 性能基准测试

**性能指标**：
- 延迟（微秒）：最小值、最大值、平均值、中位数、P95、P99
- 吞吐量（操作/秒）
- 实时因子（视频编码）
- 压缩率
- 内存带宽（MB/s）

**基准测试项目**：
- 音频编解码（G.711, Opus）
- 视频编解码（H.264, VP8, 720p30）
- 设备枚举性能
- 播放队列操作
- 内存分配/拷贝
- 端到端管道延迟

**性能阈值**：
- 音频编码：< 500 µs
- 音频解码：< 300 µs
- 视频编码：< 5000 µs (720p)
- 视频解码：< 3000 µs (720p)
- 端到端延迟：< 5 ms

## 测试工具和辅助函数

### TinyTest 断言

```c
check(condition)                       // 布尔条件
check_equal(actual, expected)          // 标量或字符串相等
check_equal(actual, expected, size)    // 内存区域相等
check_null(ptr)                        // 指针为空
check_not_null(ptr)                    // 指针非空
```

### 音频测试工具

**数据生成**：
```c
test_generate_sine_wave()        // 正弦波（float）
test_generate_sine_wave_i16()    // 正弦波（int16）
test_generate_silence()          // 静音
test_generate_white_noise()      // 白噪声
test_generate_square_wave()      // 方波
```

**音频分析**：
```c
test_calculate_rms()             // RMS 能量
test_calculate_rms_i16()         // RMS (int16)
test_is_silence()                // 静音检测
test_find_peak_amplitude()       // 峰值检测
test_has_clipping()              // 削波检测
test_compare_audio_buffers()     // 缓冲区比较
test_calculate_mse()             // 均方误差
test_calculate_snr()             // 信噪比
```

### 视频测试工具

**测试图案生成**：
```c
test_generate_i420_gradient()     // 渐变图案
test_generate_i420_solid()        // 纯色图案
test_generate_i420_checkerboard() // 棋盘格图案
```

**尺寸计算**：
```c
test_calculate_i420_frame_size()  // I420 帧大小
test_validate_video_config()      // 配置验证
```

### 性能测试工具

```c
test_timer_t timer;
test_timer_start(&timer);
// ... 执行操作 ...
double elapsed_ms = test_timer_stop(&timer);
double current_ms = test_timer_elapsed(&timer);  // 不停止计时器
```

### 文件I/O工具

```c
test_create_temp_file(filename, data, size);
test_read_file(filename, buffer, max_size, &actual_size);
test_delete_temp_file(filename);
```

### 内存测试工具

```c
test_fill_pattern(buffer, size, 0xAA);         // 填充模式
test_verify_pattern(buffer, size, 0xAA);       // 验证模式
test_reset_allocation_counter();               // 重置计数
test_increment_allocation_counter();           // 增加计数
test_decrement_allocation_counter();           // 减少计数
int leaks = test_get_allocation_count();       // 检查泄漏
```

## TinyTest 框架使用

### 基本结构

```c
suite("测试套件名称") {
    it("应该满足某个行为或条件") {
        // 准备
        int value = 42;
        
        // 执行
        int result = my_function(value);
        
        // 验证
        check_equal(result, 84);
    }
    
    it("应该处理错误情况") {
        int result = my_function(-1);
        check_equal(result, -1);
    }
}

int main(int argc, char *argv[]) {
    return test_run_all();
}
```

### TinyTest 断言

```c
check(condition)                       // 布尔条件
check_equal(actual, expected)          // 标量或字符串相等
check_equal(actual, expected, size)    // 内存区域相等
check_null(ptr)                        // 指针为空
check_not_null(ptr)                    // 指针非空
```

### BDD 风格命名

使用描述性语言表达行为：

```c
suite("音频捕获设备") {
    it("应该枚举所有可用的音频设备") { ... }
    it("应该在配置无效时返回错误") { ... }
    it("应该在捕获停止时释放所有资源") { ... }
}
```

### TDD 风格命名

使用功能性语言描述测试：

```c
suite("播放器测试") {
    it("测试设备枚举") { ... }
    it("测试文件播放") { ... }
    it("测试音量控制") { ... }
}
```

## 最佳实践

### 测试编写原则

1. **单一职责** - 每个测试只验证一个行为
2. **独立性** - 测试之间不应相互依赖
3. **可重复** - 多次运行结果一致
4. **快速执行** - 单元测试应在毫秒级完成
5. **清晰命名** - 测试名称应描述预期行为

### 测试组织

```c
suite("模块名称 - 功能分类") {
    // 正常路径测试
    it("应该处理有效输入") { ... }
    
    // 边界条件测试
    it("应该处理边界值") { ... }
    
    // 错误路径测试
    it("应该拒绝无效输入") { ... }
    
    // 资源管理测试
    it("应该正确清理资源") { ... }
}
```

### 资源管理

```c
it("应该管理资源") {
    // 分配资源
    void *resource = allocate_resource();
    check_not_null(resource);
    
    // 使用资源
    int result = use_resource(resource);
    check_equal(result, 0);
    
    // 清理资源（即使断言失败也应执行）
    cleanup_resource(resource);
}
```

### 错误测试

```c
it("应该处理空指针") {
    int result = my_function(NULL);
    check_equal(result, -1);  // 或其他错误码
}

it("应该处理无效参数") {
    int result = my_function_with_bounds(999999);
    check(result < 0);
}
```

### 性能测试

```c
it("操作应在合理时间内完成") {
    test_timer_t timer;
    test_timer_start(&timer);
    
    perform_operation();
    
    double elapsed_ms = test_timer_stop(&timer);
    check(elapsed_ms < 100.0);  // 应在 100ms 内完成
}
```

## 持续集成

### 自动化测试脚本

```bash
#!/bin/bash
# run_tests.sh

set -e

echo "构建测试..."
cmake --build build/release

echo "运行测试..."
ctest --test-dir build/release --output-on-failure

echo "所有测试通过！"
```

### CI 配置示例（GitHub Actions）

```yaml
name: Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Install dependencies
        run: sudo apt-get install -y cmake ninja-build
      - name: Configure
        run: cmake --preset release
      - name: Build
        run: cmake --build build/release
      - name: Test
        run: ctest --test-dir build/release --output-on-failure
```

## 故障排查

### 测试失败调试

1. **查看详细输出**：
   ```bash
   ctest --test-dir build/release --verbose --rerun-failed
   ```

2. **直接运行测试可执行文件**：
   ```bash
   ./build/release/bin/turbo_media_test_codec
   ```

3. **使用调试器**：
   ```bash
   gdb ./build/release/bin/turbo_media_test_codec
   ```

### 常见问题

**问题**: 设备枚举测试失败
- **原因**: 没有可用的音频/视频设备
- **解决**: 在有设备的环境运行，或跳过设备依赖测试

**问题**: 性能测试不稳定
- **原因**: 系统负载影响计时
- **解决**: 在空闲系统运行，或放宽性能阈值

**问题**: 编解码器测试失败
- **原因**: 缺少编解码器库（如 Opus, libvpx）
- **解决**: 安装依赖库或禁用相关测试

## 测试覆盖率

### 生成覆盖率报告

```bash
# 使用 Coverage preset 构建
cmake --preset coverage
cmake --build build/coverage

# 运行测试
ctest --test-dir build/coverage

# 生成覆盖率报告（需要 gcovr 或 lcov）
gcovr --root . --html --html-details -o coverage.html
```

### 覆盖目标

- **行覆盖率**: > 80%
- **函数覆盖率**: > 90%
- **分支覆盖率**: > 70%

## 贡献指南

### 添加新测试

1. 在所属模块创建并注册 `tests/test_<name>.c`
2. 包含必要头文件，按需使用共享辅助函数
3. 使用 `suite()` 和 `it()` 组织测试
4. 遵循现有命名和风格约定
5. 添加必要的文档注释
6. 确保测试通过后提交

### 代码审查检查点

- [ ] 测试名称清晰描述行为
- [ ] 资源正确分配和释放
- [ ] 边界条件和错误路径覆盖
- [ ] 性能测试有合理阈值
- [ ] 文档注释完整
- [ ] 所有测试通过

## 参考资料

- [TinyTest 框架文档](../../turbonet/tinytest/README.md)
- [TurboMedia 项目说明](../README.md)
- [CMake 测试文档](https://cmake.org/cmake/help/latest/manual/ctest.1.html)

---

**维护者**: TurboMedia 团队  
**最后更新**: 2026-07-03
