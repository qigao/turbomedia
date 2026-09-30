# TurboMedia Android mobile layer

This directory contains the current Android-specific adapters that are actually
owned by TurboMedia.

## Current build boundary

The parent TurboMedia CMake graph adds `media/mobile/android` only for Android
CLIENT builds. The native target is:

```
TurboMedia::Android
```

It currently contains:

- `ScreenCapture` JNI backed by `Salts::Capture` / MediaProjection;
- Android hardware codec helpers;
- battery/network mobile optimization helpers.

`Salts::Capture` owns native capture devices and callback payload lifetime.
TurboMedia does not duplicate microphone/camera/screen device implementations.

The Gradle files in this directory are staging metadata only. They are not a
standalone published Android library.

## Java surface that exists today

- `ScreenCapture`
- `MobileOptimizer`

There is **no** supported Java `MediaEngine`, RTC session, simulcast, or
recorder facade in this directory. Older examples/docs that described those
removed APIs were deleted rather than kept as compatibility stubs.

## Screen capture ownership

Android owns the permission result, `MediaProjection`, and `VirtualDisplay`.
Salts owns the native capture handle and Surface bridge.

Start order:

```
permission -> native capture create -> Surface -> VirtualDisplay -> native start
```

Stop order:

```
release VirtualDisplay/MediaProjection -> native stop -> native destroy
```

`stop()` quiesces native callbacks before destroy. Borrowed frame payloads must
not escape their callback.

## RTCClient lifecycle work

WebRTC publish/subscribe uses the common C `TurboMedia::RTCClient` core. The
Android platform layer must not introduce a second RTC state machine.

Tracked by **#128**:

```
Activity / permission / network event
            |
            v
dedicated Android owner thread
            |
            +--> RTCClient create/prepare/start/poll/restart/stop/destroy
            |
Salts Capture callback -> bounded CaptureSource queue -> owner poll
```

The owner thread is the only thread allowed to mutate the RTC session.
Permission revocation/pause first stops and quiesces CaptureSource. Resume or
network migration uses in-place ICE restart when the existing WHIP/WHEP
resource is still live.

## Fail-fast rules

- unsupported/missing Android SDK, NDK, Salts packages, permissions, or devices
  fail in the normal build/runtime path;
- no fallback to a removed Java media engine;
- no fallback to another capture device or transport;
- credentials, complete SDP, and media payloads are not logged.
