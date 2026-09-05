# Mobile Platform Media Layer

This directory stages the mobile media platform layer migrated from
`turbo-webrtc/media`.

Current scope:

- `ios/`: iOS VideoToolbox hardware codec, mobile optimization helpers, and
  Swift wrapper.
- `android/`: Android MediaCodec hardware codec, mobile optimization helpers,
  JNI bridge, and Java wrapper.

These sources are not wired into the default `turbo_media` target yet. The
capture, mobile optimizer, hardware codec, JNI, Java, and Swift staging code has
been narrowed away from the old TurboWebRTC media-engine APIs.

Current adaptation status:

- Camera, microphone, and C-level screen capture are provided by
  `Salts::Capture` via `<salts_capture.h>`. Android temporarily retains only
  the Java MediaProjection surface bridge tracked by issue #25; it does not
  define a second `salts_capture_*` provider.
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

Build entry points:

- Default desktop builds do not configure mobile targets.
- Mobile targets are part of the default build; platform-specific SDK libraries are required.
- On non-mobile toolchains this creates `turbo_media_ios_staging` and
  `turbo_media_android_staging` source-only targets.
- On iOS and Android toolchains the same CMake files configure native
  `turbo_media_ios` and `turbo_media_android` targets.

Known remaining work:

- Android screen capture still needs a public Salts MediaProjection/surface
  handoff before TurboMedia's legacy `ScreenCapture.java` JNI bridge can be
  removed without losing that Java-facing behavior.
- Validate the platform targets with real iOS and Android toolchains.
