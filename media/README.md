# Media components

- [`runtime/`](runtime/CMakeLists.txt) provides `TurboMedia::MediaRuntime`, the
  shared library for media sources, tracks, and subscriptions. Windows and Linux
  also build the media server runtime into this library.
- [`codec/`](codec/README.md) handles encoded and raw media conversion.
- `containers/` contains the muxer, demuxer, and shared format code.
- `client_processing/` and `player/` implement local processing and playback.
- [`speech/`](speech/README.md) and [`recognition/`](recognition/README.md) provide speech APIs.
- [`mobile/`](mobile/README.md) provides the Android and iOS adapters.

RTSP sessions and parsing live in [streaming/rtsp/](../streaming/rtsp/CMakeLists.txt).
See the [architecture](../ARCHITECTURE.md) for component and platform boundaries.
