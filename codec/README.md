# Codec Sources

`codec_registry.c` and `g711_codec.c` are built into `TurboMedia::Codec` by default.
They provide the public `turbo_codec.h` registry and dependency-free G.711
PCMU/PCMA codecs.

The remaining sources are available behind explicit CMake options because they
require external codec SDKs:

- `TURBO_MEDIA_ENABLE_OPUS`: `opus_codec.c` with libopus
- `TURBO_MEDIA_ENABLE_VPX`: `vpx_codec.c` with libvpx
- `TURBO_MEDIA_ENABLE_H264`: `h264_codec.c` with OpenH264
- `TURBO_MEDIA_ENABLE_H265`: `h265_codec.c` with x265 and libde265

Each option is OFF by default. When an option is enabled, CMake requires the
matching headers and libraries and exports the matching `TURBO_MEDIA_HAS_*`
compile definition through `TurboMedia::Codec`.

With vcpkg manifest mode, enable the matching manifest features as well:

```sh
cmake -S . -B build/codec-options \
  -DVCPKG_MANIFEST_MODE=ON \
  "-DVCPKG_MANIFEST_FEATURES=opus;vpx;h264;h265" \
  -DTURBO_MEDIA_ENABLE_OPUS=ON \
  -DTURBO_MEDIA_ENABLE_VPX=ON \
  -DTURBO_MEDIA_ENABLE_H264=ON \
  -DTURBO_MEDIA_ENABLE_H265=ON
```

`VCPKG_MANIFEST_FEATURES` installs the external dependencies. The
`TURBO_MEDIA_ENABLE_*` options decide which codec sources are compiled into
`TurboMedia::Codec`.

## FFmpeg Player

`TURBO_MEDIA_ENABLE_FFMPEG` builds the optional universal player layer in
`turbo_player.h`. This layer uses FFmpeg for container demuxing and codec
decode, writes decoded F32 PCM into `turbo_playback`, and exposes decoded video
frames through a callback.

Use the `ffmpeg` manifest feature to install FFmpeg:

```sh
cmake -S . -B build/ffmpeg-player \
  -DVCPKG_MANIFEST_MODE=ON \
  -DVCPKG_MANIFEST_FEATURES=ffmpeg \
  -DTURBO_MEDIA_ENABLE_FFMPEG=ON
```

The `ffmpeg_probe` example decodes a media file without opening an audio device:

```sh
ffmpeg_probe sample.mp4
```

The player supports both blocking and background-thread playback:

- `turbo_player_play_to_end`: blocking decode/playback.
- `turbo_player_start` / `turbo_player_wait`: asynchronous playback.
- `turbo_player_pause` / `turbo_player_resume`: pause and resume the decode loop
  and audio output.
- `turbo_player_seek_ms`: request a timestamp seek and flush decode state.

`ffmpeg_probe` also accepts `--async` and `--seek-ms N` for smoke testing these
paths.
