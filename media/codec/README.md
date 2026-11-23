# Codec Sources

`TurboMedia::Codec` 始终构建注册表、G.711、Opus、VP8/VP9、H.264 和 H.265 实现。
对应的 libopus、libvpx、OpenH264、x265 与 libde265 都是配置期必需依赖；缺失时 CMake
直接失败，不会跳过源文件或选择回退实现。

## 嵌入式 SDK

音频和视频 SDK 各自安装一个静态库。音频包含 PCMU、PCMA 和 Opus，第三方编解码依赖只有 Opus。
视频包含 VP8、VP9、H.264 和 H.265，依赖 libvpx、OpenH264、x265 和 libde265。

```powershell
cmake --install <build-dir> --config Release --prefix <audio-sdk> --component AudioCodec
cmake --install <build-dir> --config Release --prefix <video-sdk> --component VideoCodec
```

消费方的 `CMAKE_PREFIX_PATH` 包含 SDK 和同一工具链的依赖前缀。音频应用包含
`turbo_audio_codec.h`。视频应用使用 `VideoCodec` 和 `turbo_video_codec.h`，并启用 C++
工具链以便链接 x265 等 C++ 实现。

```cmake
find_package(TurboMedia CONFIG REQUIRED COMPONENTS AudioCodec)
target_link_libraries(my_app PRIVATE TurboMedia::AudioCodec)
```

同时使用音视频时，将两个组件安装到同一 SDK 前缀，在 `find_package` 中列出两个组件，
并链接两个 target。各自的 `create_encoder` / `create_decoder` 返回由调用方拥有的句柄。
调用 `encode` / `decode` 时，`output_len` 输入缓冲区容量，成功后返回写入的字节数。
最终通过各自的 `destroy` 释放句柄。两个组件根据名字选择各自的内置 codec。

第三方依赖使用消费方前缀中的库，动态版本随应用部署。原有的共享
`TurboMedia::Codec` 和 `turbo_codec.h` 继续提供既有注册表接口。
原先链接 `CodecStatic` 的应用按所需的媒体类型改用新的 target 和对应的 C 接口。

FFmpeg player 同样进入默认构建，并要求 FFmpeg 的 avformat、avcodec、avutil、
swresample 和 swscale。依赖安装前缀只通过 `CMakeUserPresets.json` 的
`CMAKE_PREFIX_PATH`/package root 提供。

`ffmpeg_probe` 可在不打开音频设备的情况下解码媒体文件：

```sh
ffmpeg_probe sample.mp4
```

Player 提供阻塞和后台线程两种播放流程：

- `turbo_player_play_to_end`：阻塞解码和播放。
- `turbo_player_start` / `turbo_player_wait`：异步播放。
- `turbo_player_pause` / `turbo_player_resume`：暂停或恢复解码循环和音频输出。
- `turbo_player_seek_ms`：请求时间戳跳转并刷新解码状态。

`ffmpeg_probe` 也接受 `--async` 和 `--seek-ms N`，用于冒烟验证这些路径。
