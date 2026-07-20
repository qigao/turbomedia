# Codec Sources

`TurboMedia::Codec` 始终构建注册表、G.711、Opus、VP8/VP9、H.264 和 H.265 实现。
对应的 libopus、libvpx、OpenH264、x265 与 libde265 都是配置期必需依赖；缺失时 CMake
直接失败，不会跳过源文件或选择回退实现。

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
