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

## Bounded raw-image Encoder — Draft API

`TurboMedia::Encoder` has a versioned C11 one-shot `tm_encoder_image_encode()`
for borrowed GRAY8/GA8/RGB24/RGBA32 frames. PNG/JPEG are implemented
by the single required stb provider inside `TurboMedia::Encoder`, with a
quota-bounded memory writer. WebP calls the exact selected FFmpeg backend
and reports `TM_ENCODER_EUNSUPPORTED` if it is not available. No format
has an alternate runtime fallback provider.

PNG preserves native `parameters` tEXt and JPEG the sd-style `parameters`
COM marker. These additions are bounded and tested by decoding the resulting
image. **Not migration-ready for sd yet:** WebP XMP metadata embedding
is not implemented; nonempty WebP metadata explicitly fails
`TM_ENCODER_EUNSUPPORTED` before writing bytes. AVI, animated WebP, and
end-to-end audio+video frame-to-WebM encode also remain open. The existing
sd local output must not be misidentified as TurboMedia-backed until these
contracts, exact package identity and platform-native CI pass.
See [Issue #272](https://github.com/qigao/turbomedia/issues/272).
