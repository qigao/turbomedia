# Media Device and Mobile Platform Layer

`TurboMedia::Device` owns audio playback and publicly links
`TurboUtils::Capture`. Applications may keep including `<turbo_capture.h>` and
linking `TurboMedia::Device`; the header, Capture ABI, native state, and backend
implementation are supplied only by the TurboUtils package.

Configure TurboUtils with `TURBO_ENABLE_CAPTURE=ON` and install that build before
configuring TurboMedia. TurboMedia fails at configure time when the installed
package does not export `TurboUtils::Capture`; there is no local fallback.
When upgrading an existing TurboMedia install prefix in place, remove the old
`include/turbo_capture.h` once or install into a clean prefix; CMake install does
not prune files owned by an older version.

Capture callbacks borrow their frame bytes synchronously. A consumer that keeps
data after the callback returns, or sends it to another thread/queue/coroutine,
must copy it during the callback. The caller serializes start/stop/destroy and
stops capture before destroying it.

## Mobile targets

- `turbo_media_ios` contains VideoToolbox hardware codec and mobile optimizer /
  monitor adapters. iOS Capture is supplied by `TurboUtils::Capture`.
- `turbo_media_android` contains MediaCodec, mobile optimizer/monitor and JNI
  adapters. Android audio and camera Capture are supplied by
  `TurboUtils::Capture`.
- `media/mobile/android/src/capture/capture_screen_android.c` remains a private
  Java MediaProjection Surface adapter. It exports no `turbo_capture_*` public
  API and does not own Capture state. Removing it requires a stable TurboUtils
  Android Surface handoff API first.

Desktop builds do not configure mobile targets. Native iOS and Android builds
require their platform SDKs; runtime capture tests additionally require hardware
and user permission.
