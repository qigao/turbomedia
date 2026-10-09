# TurboMedia iOS mobile layer

This directory contains the iOS-specific native/mobile adapters currently owned
by TurboMedia.

## Current build boundary

The parent TurboMedia CMake graph adds `media/mobile/ios` only when
`CMAKE_SYSTEM_NAME=iOS`.

Native qualification is platform-matrix driven. The canonical rows are:

- device: `ios-arm64` / vcpkg `arm64-ios`;
- Apple Silicon simulator: `ios-simulator-arm64` / vcpkg `arm64-ios-simulator`.

Both consume released SDKs and the central `vcpkg-cache_*` namespace read-only,
then configure/build/install the normal TurboMedia graph. Cross-built target
tests are compiled but are not executed on the macOS host. There is no separate
iOS build script or universal x86_64 compatibility path.

The native target is:

```
TurboMedia::IOS
```

It currently contains:

- VideoToolbox hardware codec helpers;
- battery/network mobile optimization helpers;
- a dependency on `Salts::Capture` for native capture ownership.

The Swift wrapper currently exposes only `TurboMobileOptimizer`. There is no
supported Swift `MediaEngine`, RTC session, simulcast, or recorder facade.

`Package.swift` is staging metadata only and intentionally exports no product
or target yet.

## RTCClient lifecycle work

WebRTC publish/subscribe uses the same C `TurboMedia::RTCClient` core as
desktop. Swift owns UI, permission, `AVAudioSession`, foreground/background,
and network-path policy only.

Tracked by **#129**:

```
SwiftUI/UIKit / permission / AVAudioSession / NWPath
                      |
                      v
             serial RTC owner queue
                      |
                      +--> RTCClient C core
                      |
Salts Capture callback -> bounded CaptureSource queue -> owner poll
```

All mutating RTCClient calls execute on one serial owner queue. The platform
layer must not introduce a second session state machine.

## Lifecycle rules

- microphone permission denial/revocation stops and quiesces CaptureSource;
- AVAudioSession interruption/background policy is resolved before restarting
  the capture/playback device;
- foreground/network migration uses in-place ICE restart while the existing
  WHIP/WHEP resource is still live;
- if the OS/process destroys the session, recreation is explicit and is not
  reported as reconnect;
- subscriber shutdown quiesces WHEP production before bounded Playback
  drain/stop/clear and destroy.

## Fail-fast rules

- unsupported/missing iOS SDK, toolchain, Salts packages, entitlements,
  permissions, devices, or formats fail in the normal build/runtime path;
- no fallback to a removed Swift media engine;
- no fallback to another device, transport, or hidden session recreation;
- credentials, complete SDP, and media payloads are not logged.
