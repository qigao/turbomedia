# Salts Capture Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `Salts::Capture` the only capture provider used by TurboMedia, migrate all consumers and public callback types to the Salts API, and remove TurboMedia's duplicate capture implementation.

**Architecture:** `TurboMedia::Device` links publicly to `Salts::Capture` and retains only playback plus narrowly required mobile integration. Speech, Recognition, examples, tests, and WebRTC consume `<salts_capture.h>` directly. The Salts capture handle is the unique lifecycle owner; callback frames remain borrowed and must be copied before asynchronous retention.

**Tech Stack:** C11/Objective-C, CMake, SaltsUtils Capture, TinyTest, Windows Media Foundation, PipeWire/X11, Android NDK, Apple AVFoundation

**Spec:** `docs/superpowers/specs/2026-09-05-salts-dependency-refactor-design.md`

## Global Constraints

- This plan starts after the Parser/SaltsUtils plan, so exact `SALTS_ROOT` and `SALTS_UTILS_ROOT` package discovery already works.
- `Salts::Capture` and `salts_capture_t` are the single provider and state owner; do not retain a Turbo compatibility provider or alias.
- Preserve callback borrowed-lifetime rules, start/stop/destroy ordering, error visibility, device bounds, and TurboMedia media-track behavior.
- The change from `struct turbo_capture_s *` to `salts_capture_t *` in installed headers is an explicit source-compatibility break requiring consumer recompilation.
- Delete local capture files only after all consumers compile and behavior tests pass against Salts Capture.
- Windows SaltsUtils must export `Salts::Capture`; non-Windows configurations must enable `SALTS_UTILS_ENABLE_CAPTURE` in the installed profile.

---

### Task 1: Lock Capture behavior and record the legacy inventory

**Files:**
- Modify: `tests/test_capture.c`
- Modify: `speech/tests/test_speech.c`
- Modify: `recognition/tests/test_recognition.c`
- Modify: `webrtc/tests/test_media_engine.c`

**Interfaces:**
- Consumes: current device enumeration, create/start/stop/destroy, callback, Speech/Recognition adapter, and WebRTC media-track behavior
- Produces: behavior tests plus an exact legacy Capture identifier inventory

- [ ] **Step 1: Record the legacy Capture inventory**

```powershell
rg.exe -n "turbo_capture\.h|turbo_(audio|video|screen|capture)_[a-z0-9_]+|TURBO_CAPTURE_[A-Z0-9_]+|struct turbo_capture_s" `
  CMakeLists.txt CMakeUserPresets.json cmake presets common core crypto demuxer examples media muxer network pipeline recognition server speech streamer tests webrtc
```

Expected: matches cover the local provider, public callback types, consumers, tests, and examples. This is a review/CI policy check, not a CTest behavior test.

- [ ] **Step 2: Add lifecycle and callback ownership assertions**

Extend tests to cover:

```text
enumeration respects caller capacity and returns a count/error
invalid configuration fails without exposing a handle
create → start → stop → destroy succeeds
duplicate/invalid lifecycle calls return documented result/state
audio/video callback receives the originating handle and exact timestamp
callback frame is consumed synchronously; tests never retain its pointer
Speech/Recognition adapters accept a Capture callback and preserve frame metadata
WebRTC media engine attaches/detaches without replacing unrelated callbacks
```

Use fake or no-device paths already supported by the tests; hardware-dependent success is not a default CTest requirement.

- [ ] **Step 3: Run the old behavior baseline**

```powershell
cmake --build --preset win-dev-user
ctest --preset win-release-user -R "(test_capture|test_speech$|test_recognition$|test_media_engine)" --output-on-failure
```

Expected: existing behavior assertions pass or skip only on their documented no-device condition when an old SDK baseline is available. If the old SDK is intentionally absent, execute the same tests immediately after switching to `Salts::Capture`.

- [ ] **Step 4: Commit the capture gates**

```powershell
git add tests speech/tests recognition/tests webrtc/tests/test_media_engine.c
git commit -m "test: lock Capture migration behavior"
```

### Task 2: Make `Salts::Capture` a required exported dependency

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `media/CMakeLists.txt`
- Modify: `cmake/TurboMediaConfig.cmake.in`
- Modify: `tests/package_consumer/CMakeLists.txt`
- Modify: `tests/package_consumer/main.c`

**Interfaces:**
- Consumes: imported `Salts::Capture`
- Produces: `TurboMedia::Device` whose installed interface transitively resolves Capture

- [ ] **Step 1: Add a configure-time target check**

After exact SaltsUtils discovery, require the target:

```cmake
if(NOT TARGET Salts::Capture)
  message(FATAL_ERROR
    "SaltsUtils profile does not export Salts::Capture; enable SALTS_UTILS_ENABLE_CAPTURE")
endif()
```

Configure once against a SaltsUtils profile without Capture when available. Expected: explicit failure naming the target and option, not a later linker error.

- [ ] **Step 2: Link the Device component publicly**

```cmake
target_link_libraries(turbo_media_device
  PUBLIC Salts::Capture
  PRIVATE yuv)
```

The edge is public because installed Speech/Recognition or Device-facing headers use `salts_capture_t`.

- [ ] **Step 3: Stop compiling desktop provider sources from TurboMedia**

Remove `media/capture/*.c` and `*.m` plus `media/mobile/android/src/capture/*` from `turbo_media_device` source lists, but do not delete the files yet. Retain playback and non-Capture mobile sources. SaltsUtils already owns the Windows, Linux, macOS, Android, and iOS providers in the selected package revision.

- [ ] **Step 4: Strengthen installed consumer coverage**

In `tests/package_consumer/main.c`, include `<salts_capture.h>` through the installed dependency and type-check a `salts_capture_t *` used with a TurboMedia public callback. The consumer still links only a TurboMedia target; do not add direct `Salts::Capture` linkage.

- [ ] **Step 5: Configure and build before source renames**

```powershell
cmake --preset win-dev-user --fresh
cmake --build --preset win-dev-user
```

Expected at this point: compile failures identify remaining `turbo_capture_*` consumers; there must be no duplicate-symbol link from local and Salts providers.

- [ ] **Step 6: Commit the provider boundary**

```powershell
git add CMakeLists.txt media/CMakeLists.txt cmake/TurboMediaConfig.cmake.in tests/package_consumer
git commit -m "build: use Salts Capture as the device provider"
```

### Task 3: Migrate installed Speech and Recognition callback types

**Files:**
- Modify: `speech/include/turbo_speech.h`
- Modify: `speech/turbo_speech.c`
- Modify: `speech/tests/test_speech.c`
- Modify: `speech/CMakeLists.txt`
- Modify: `recognition/include/turbo_recognition.h`
- Modify: `recognition/turbo_recognition.c`
- Modify: `recognition/tests/test_recognition.c`
- Modify: `recognition/CMakeLists.txt`

**Interfaces:**
- Consumes: `salts_capture_t`, `salts_audio_capture_cb`, borrowed audio buffers
- Produces: `turbo_asr_capture_callback`, `turbo_voice_detector_capture_callback`, and `turbo_fingerprint_capture_callback` with Salts-compatible signatures

- [ ] **Step 1: Replace public forward declarations with the public dependency header**

```c
#include <salts_capture.h>
```

Remove `struct turbo_capture_s;`. Change all three adapter signatures from `struct turbo_capture_s *capture` to `salts_capture_t *capture`.

- [ ] **Step 2: Update implementations without changing frame semantics**

Use the exact callback signature:

```c
void turbo_asr_capture_callback(salts_capture_t *capture,
                                const uint8_t *samples,
                                size_t len,
                                uint64_t timestamp_us,
                                void *user_data);
```

Recognition adapters use the same first-parameter type. Preserve borrowed data, timestamp, format validation, busy handling, and rejected-frame counters. Mark `capture` unused with the repository's existing convention if the adapter does not inspect it.

- [ ] **Step 3: Propagate link visibility**

Ensure Speech and Recognition targets receive `Salts::Capture` through `TurboMedia::Device` or an explicit `PUBLIC` edge required by their installed headers. Verify generated package targets do not leave an unresolved include directory.

- [ ] **Step 4: Build and run adapter tests**

```powershell
cmake --build --preset win-dev-user
ctest --preset win-dev-user -R "(test_speech$|test_recognition$|speech_integration|recognition_integration)" --output-on-failure
```

Expected: callbacks compile as `salts_audio_capture_cb`-compatible functions and all selected tests pass.

- [ ] **Step 5: Commit public callback migration**

```powershell
git add speech recognition
git commit -m "refactor: expose Salts Capture in speech adapters"
```

### Task 4: Migrate WebRTC, tests, and examples to the Salts API

**Files:**
- Modify: `webrtc/src/rtc/media/turbo_media_engine.c`
- Modify: `examples/capture_save_image.c`
- Modify: `examples/streaming_pipeline.c`
- Modify: `tests/test_capture.c`
- Modify: `tests/test_integration.c`
- Modify: `tests/test_codec.c`
- Modify: `tests/test_benchmark.c`
- Modify: `tests/test_android_capture.c`
- Modify: all additional consumers returned by the scan below
- Modify: matching CMake target definitions under `examples/`, `tests/`, and `webrtc/`

**Interfaces:**
- Consumes: `<salts_capture.h>` and `salts_*` capture functions/types/constants
- Produces: unchanged TurboMedia media-engine and example behavior using the Salts provider

- [ ] **Step 1: Capture the exact consumer inventory**

```powershell
rg.exe -n "turbo_capture\.h|\bturbo_(audio|video|screen|capture)_[a-z0-9_]+\b|\bTURBO_CAPTURE_[A-Z0-9_]+\b|struct turbo_capture_s" `
  examples media recognition speech tests webrtc
```

Classify every match as include, type, function, constant, callback signature, or documentation before changing it.

- [ ] **Step 2: Apply the public one-to-one API mapping**

```c
turbo_capture_t                     -> salts_capture_t
turbo_capture_device_t              -> salts_capture_device_t
turbo_audio_capture_config_t        -> salts_audio_capture_config_t
turbo_video_capture_format_t        -> salts_video_capture_format_t
turbo_screen_capture_config_t       -> salts_screen_capture_config_t
TURBO_CAPTURE_*                     -> SALTS_CAPTURE_*
turbo_capture_list_*                -> salts_capture_list_*
turbo_audio_capture_create          -> salts_audio_capture_create
turbo_audio_capture_set_callback    -> salts_audio_capture_set_callback
turbo_video_capture_set_callback    -> salts_video_capture_set_callback
turbo_capture_start/stop/destroy    -> salts_capture_start/stop/destroy
```

For video modes, device handles, screen/GPU capture, camera controls, and stats, map to the exact same-suffix declarations in `<salts_capture.h>` and preserve capacity/out-count checks.

- [ ] **Step 3: Review every callback crossing a thread boundary**

Any callback that queues work, schedules a coroutine, or returns before consumption must copy the frame into an existing bounded buffer before returning. Synchronous Speech/Recognition adapters may keep their current borrowed path. Do not add an unbounded queue.

- [ ] **Step 4: Build and run consumer-focused tests**

```powershell
cmake --build --preset win-dev-user
ctest --preset win-dev-user -R "(test_capture|test_integration|test_codec|test_media_engine|speech|recognition)" --output-on-failure
```

Expected: selected tests execute and pass; hardware-absent results use the documented unsupported/no-device path rather than fallback capture.

- [ ] **Step 5: Commit consumer migration**

```powershell
git add examples tests webrtc media recognition speech
git commit -m "refactor: migrate capture consumers to Salts API"
```

### Task 5: Remove duplicate Android and Apple providers

**Files:**
- Delete: `media/mobile/android/src/capture/capture_android.c`
- Delete: `media/mobile/android/src/capture/capture_audio_android.c`
- Delete: `media/mobile/android/src/capture/capture_video_android.c`
- Delete: `media/mobile/android/src/capture/capture_screen_android.c`
- Delete: `media/mobile/ios/src/capture/capture_ios.m`
- Delete: `media/mobile/ios/src/capture/capture_audio_ios.m`
- Delete: `media/mobile/ios/src/capture/capture_video_ios.m`
- Delete: `media/mobile/ios/src/capture/capture_screen_ios.m`
- Delete: `media/mobile/ios/src/capture/capture_video_ios_backend.h`
- Modify: `media/CMakeLists.txt`
- Modify: `media/mobile/ios/CMakeLists.txt`

**Interfaces:**
- Consumes: Android and iOS provider coverage exported by the installed SaltsUtils profile
- Produces: exactly one platform capture backend per target platform

- [ ] **Step 1: Record the upstream platform ownership evidence**

Use `rg.exe` in the exact SaltsUtils revision being consumed and record that `capture/CMakeLists.txt` compiles `src/android/capture_*.c` for Android and `src/ios/capture_*.m` for iOS. Also record the installed SaltsUtils version/commit and `SALTS_UTILS_ROOT`; this prevents a future profile lacking those providers from silently passing review.

- [ ] **Step 2: Remove TurboMedia's duplicate platform sources**

Delete the Android/iOS files listed above. Remove their source-list entries and Capture-only platform libraries/definitions from `media/CMakeLists.txt` and `media/mobile/ios/CMakeLists.txt`. Keep non-Capture mobile codec, JNI, battery, network-monitor, and optimizer sources unchanged.

- [ ] **Step 3: Configure/build Android**

```powershell
cmake --preset android-arm64-v8a-debug-win --fresh
cmake --build --preset android-arm64-v8a-debug-win
cmake --build --preset install-android-arm64-v8a-debug-win
```

Expected: exactly one definition of each Salts capture symbol and no `turbo_capture_*` symbol.

- [ ] **Step 4: Verify Apple source paths**

On a macOS/iOS runner, configure and compile the existing presets/toolchain. If no runner is available, run the scanner and CMake source-list review, mark Apple compilation unverified in the issue, and do not claim cross-platform completion.

- [ ] **Step 5: Commit platform ownership resolution**

```powershell
git add media/mobile media/CMakeLists.txt
git commit -m "refactor: unify platform capture ownership"
```

### Task 6: Delete the duplicate provider and complete the migration gate

**Files:**
- Delete: `media/include/turbo_capture.h`
- Delete: `media/capture/capture_audio_miniaudio.c`
- Delete: `media/capture/capture_audio_win32.c`
- Delete: `media/capture/capture_linux.c`
- Delete: `media/capture/capture_macos.c`
- Delete: `media/capture/capture_screen_linux.c`
- Delete: `media/capture/capture_screen_macos.m`
- Delete: `media/capture/capture_screen_win32.c`
- Delete: `media/capture/capture_video.c`
- Delete: `media/capture/capture_video_backend.h`
- Delete: `media/capture/capture_video_linux.c`
- Delete: `media/capture/capture_video_macos.m`
- Delete: `media/capture/capture_video_win32.c`
- Delete: `media/capture/capture_win32.c`
- Modify: `media/README.md`
- Modify: `README.md` and active capture documentation returned by the legacy scan

**Interfaces:**
- Consumes: fully migrated consumers and single provider boundary
- Produces: no legacy Capture header/source/symbol and reproducible Capture issue acceptance evidence

- [ ] **Step 1: Prove no compiled consumer needs the local files**

```powershell
rg.exe -n "turbo_capture\.h|\bturbo_(audio|video|screen|capture)_[a-z0-9_]+\b|\bTURBO_CAPTURE_[A-Z0-9_]+\b|struct turbo_capture_s" `
  CMakeLists.txt cmake presets common core crypto demuxer examples media muxer network pipeline recognition server speech streamer tests webrtc
```

Expected: only the local files scheduled for deletion and active documentation remain. Any code match must be migrated before deletion.

- [ ] **Step 2: Delete the local provider and update docs**

Remove the files listed above with `apply_patch` or an equivalent reviewed patch. Update active examples to include `<salts_capture.h>`, use `salts_*`, and state that capture comes from SaltsUtils.

- [ ] **Step 3: Make the legacy static gate pass**

```powershell
rg.exe -n "turbo_capture\.h|turbo_(audio|video|screen|capture)_[a-z0-9_]+|TURBO_CAPTURE_[A-Z0-9_]+|struct turbo_capture_s" `
  CMakeLists.txt CMakeUserPresets.json cmake presets common core crypto demuxer examples media muxer network pipeline recognition server speech streamer tests webrtc
```

Expected: no output.

- [ ] **Step 4: Run Windows full/install-consumer verification**

```powershell
cmake --preset win-dev-user --fresh
cmake --build --preset win-dev-user
ctest --preset win-dev-user --output-on-failure
cmake --build --preset install-win-dev-user
cmake -S tests/package_consumer -B build/package-consumer-capture -G Ninja `
  -DCMAKE_PREFIX_PATH="$env:PKG_ROOT/turbomedia/debug"
cmake --build build/package-consumer-capture
```

Expected: all commands exit 0 and installed public headers resolve `<salts_capture.h>` through exported dependencies.

- [ ] **Step 5: Inspect binaries and source for duplicate providers**

```powershell
rg.exe -n "turbo_capture\.h|\bturbo_(audio|video|screen|capture)_[a-z0-9_]+\b|\bTURBO_CAPTURE_[A-Z0-9_]+\b|struct turbo_capture_s" `
  CMakeLists.txt CMakeUserPresets.json cmake presets common core crypto demuxer examples media muxer network pipeline recognition server speech streamer tests webrtc
```

Expected: no output. Inspect the platform library with the toolchain symbol dumper and confirm only `salts_capture_*` is provided by SaltsUtils, not by TurboMedia.

- [ ] **Step 6: Commit deletion and documentation**

```powershell
git add -A media examples tests speech recognition webrtc README.md docs
git commit -m "refactor: remove duplicate TurboMedia capture provider"
```
