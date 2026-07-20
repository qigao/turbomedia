# Mobile Platform Media Layer

This directory stages the mobile media platform layer migrated from
`turbo-webrtc/media`.

Current scope:

- `ios/`: iOS camera, microphone, screen capture, VideoToolbox hardware codec,
  mobile optimization helpers, and Swift wrapper.
- `android/`: Android camera, microphone, screen capture, MediaCodec hardware
  codec, mobile optimization helpers, JNI bridge, and Java wrapper.

These sources are not wired into the default `turbo_media` target yet. The
capture, mobile optimizer, hardware codec, JNI, Java, and Swift staging code has
been narrowed away from the old TurboWebRTC media-engine APIs.

Current adaptation status:

- iOS and Android capture entry points have been mapped to
  `turbo_audio_capture_create`, `turbo_video_capture_create`,
  `turbo_screen_capture_create`, callback setters, device enumeration, and
  common start/stop/destroy dispatch from `include/turbo_capture.h`.
- iOS and Android mobile optimizer/monitor declarations have been moved behind
  the staged `media/include/turbo_mobile.h` header, removing their dependency on
  the old `turbo_media_engine.h` header.
- iOS and Android hardware codec files have been moved behind the staged
  `media/include/turbo_mobile_codec.h` interface, removing their dependency on
  the old WebRTC codec registry and `turbo_frame_t` type.
- Android JNI is limited to ScreenCapture and MobileOptimizer staging bindings.
  Older MediaEngine, Simulcast, and Recorder Java classes are compatibility
  stubs and no longer declare native methods.
- iOS Swift staging code now wraps the mobile optimizer only.
- The mobile CMake files are platform-aware. On desktop toolchains they expose
  source-only staging targets. On iOS/Android toolchains they configure native
  `turbo_media_ios` / `turbo_media_android` targets.
- The capture sources are still staged and are not enabled in the default
  desktop target.

Build entry points:

- Default desktop builds do not configure mobile targets.
- Mobile targets are part of the default build; platform-specific SDK libraries are required.
- On non-mobile toolchains this creates `turbo_media_ios_staging` and
  `turbo_media_android_staging` source-only targets.
- On iOS and Android toolchains the same CMake files configure native
  `turbo_media_ios` and `turbo_media_android` targets.

Known remaining work:

- Android screen capture still needs a Java/MediaProjection handoff. The
  current public `turbo_screen_capture_config_t` does not carry that platform
  permission/surface state.
- Validate the platform targets with real iOS and Android toolchains.
