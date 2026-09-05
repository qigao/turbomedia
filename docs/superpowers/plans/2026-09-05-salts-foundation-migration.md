# Salts Foundation Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace TurboMedia's TurboUtils Core/STL/TinyTest package, headers, symbols, presets, and exported dependency with the Salts foundation package while preserving observable behavior.

**Architecture:** `SALTS_ROOT` becomes the exact and only source of foundation targets. This plan migrates target links and source APIs together, then proves that both the source-tree build and an installed consumer resolve `Salts` without compatibility aliases or default-path fallback. Parser/DataBind/Mustache and Capture remain assigned to their later plans; if the installed TurboParser still pulls TurboUtils into a final target, this plan is kept as a stacked PR until the parser plan is complete.

**Tech Stack:** C11/C++, CMake presets, Salts Core/CSTL/TinyTest, TinyTest, PowerShell, CTest

**Spec:** `docs/superpowers/specs/2026-09-05-salts-dependency-refactor-design.md`

## Global Constraints

- Use `SALTS_ROOT` with `find_package(Salts CONFIG REQUIRED PATHS "$ENV{SALTS_ROOT}" NO_DEFAULT_PATH)`; do not search system paths.
- Map `TurboUtils::Core` → `Salts::Core`, `TurboUtils::STL` → `Salts::CSTL`, and `TurboUtils::TinyTest` → `Salts::TinyTest`.
- Do not define Turbo target aliases or preprocessor compatibility aliases.
- Preserve TurboMedia function names, error behavior, ownership, wire formats, and default features.
- A build target must not link both TurboUtils Core and Salts Core; keep this as a stacked PR if the remaining parser package violates that rule.
- Run all verification from a freshly configured build directory and report unexecuted platforms explicitly.

---

### Task 1: Capture the legacy-foundation baseline

**Files:**
- No repository file changes

**Interfaces:**
- Consumes: current source tree and `win-release-user` configure preset
- Produces: reproducible issue evidence for the old package failure and exact legacy identifier inventory

- [ ] **Step 1: Record the static baseline**

```powershell
rg.exe -n "TurboUtils::|TURBOUTILS_ROOT|<turbostl/|<turbo_(error|fs|uuid|str)\.h>|TURBO_E[A-Z0-9_]+|turbo_(fs|uuid)_[a-z0-9_]+" `
  CMakeLists.txt CMakeUserPresets.json cmake presets common core crypto demuxer examples media muxer network pipeline recognition server speech streamer tests webrtc
```

Expected: matches include root `CMakeLists.txt`, `core/CMakeLists.txt`, and foundation headers/symbols. This is a review/CI policy check, not a CTest behavior test.

- [ ] **Step 2: Prove the old package boundary fails**

```powershell
cmake --fresh --preset win-release-user
```

Expected in the migration environment: configure fails and names the missing legacy `TURBOUTILS_ROOT`; compiler and vcpkg setup complete before that error.

### Task 2: Make `SALTS_ROOT` the exact foundation package root

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `CMakeUserPresets.json`
- Modify: `cmake/TurboMediaConfig.cmake.in`

**Interfaces:**
- Consumes: environment variable `SALTS_ROOT`
- Produces: imported `Salts::*` targets for the source build and installed consumers

- [ ] **Step 1: Add a negative configure check for a missing root**

Run in a shell with `SALTS_ROOT` removed and other preset variables unchanged:

```powershell
Remove-Item Env:SALTS_ROOT -ErrorAction SilentlyContinue
cmake -S . -B build/salts-root-negative -G Ninja
```

Expected: configure fails with a message that names `SALTS_ROOT`; a successful configure is a test failure.

- [ ] **Step 2: Replace root discovery with exact Salts discovery**

Implement the root boundary in `CMakeLists.txt` and mirror it in `TurboMediaConfig.cmake.in`:

```cmake
if(NOT DEFINED ENV{SALTS_ROOT} OR "$ENV{SALTS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_ROOT must name the installed Salts profile")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" SALTS_ROOT_PATH)
if(NOT IS_DIRECTORY "${SALTS_ROOT_PATH}")
  message(FATAL_ERROR "SALTS_ROOT is not a directory: $ENV{SALTS_ROOT}")
endif()
find_package(Salts CONFIG REQUIRED PATHS "${SALTS_ROOT_PATH}" NO_DEFAULT_PATH)
```

Remove `TURBOUTILS_ROOT` from the dependency-root validation loop. Do not add `CMAKE_PREFIX_PATH` fallback and do not create `TurboUtils::*` aliases.

- [ ] **Step 3: Replace preset roots and runtime directories**

For Windows/Linux debug/release and every Android ABI preset, set:

```json
"SALTS_ROOT": "$env{PKG_ROOT}/salts/debug"
```

Use `/salts/release`, `/salts-android/debug`, or `/salts-android/release` for the matching profile. Replace each `$env{TURBOUTILS_ROOT}/bin` or `/lib` runtime entry with `$env{SALTS_ROOT}/bin` or `/lib`.

- [ ] **Step 4: Reconfigure with the installed Salts profile**

```powershell
cmake --preset win-dev-user --fresh
```

Expected: `Salts_FOUND` is true and configure progresses until the first still-unmigrated target/source error; it must not resolve a TurboUtils package through a default path.

- [ ] **Step 5: Commit the package-root boundary**

```powershell
git add CMakeLists.txt CMakeUserPresets.json cmake/TurboMediaConfig.cmake.in
git commit -m "build: resolve Salts foundation from exact root"
```

### Task 3: Migrate all foundation target links

**Files:**
- Modify: `common/CMakeLists.txt`
- Modify: `core/CMakeLists.txt`
- Modify: `crypto/CMakeLists.txt`
- Modify: `demuxer/CMakeLists.txt`
- Modify: `examples/CMakeLists.txt`
- Modify: `media/rtsp/CMakeLists.txt`
- Modify: `muxer/CMakeLists.txt`
- Modify: `network/CMakeLists.txt`
- Modify: `pipeline/CMakeLists.txt`
- Modify: `recognition/CMakeLists.txt`
- Modify: `server/CMakeLists.txt`
- Modify: `speech/CMakeLists.txt`
- Modify: `streamer/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `webrtc/CMakeLists.txt`
- Modify: `webrtc/apps/CMakeLists.txt`
- Modify: `webrtc/apps/ivr_worker/CMakeLists.txt`
- Modify: `webrtc/apps/room_service/CMakeLists.txt`
- Modify: `webrtc/apps/sfu_node/CMakeLists.txt`
- Modify: `webrtc/apps/signaling_server/CMakeLists.txt`
- Modify: `webrtc/examples/CMakeLists.txt`
- Modify: `webrtc/ivr/CMakeLists.txt`
- Modify: `webrtc/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Salts::Core`, `Salts::CSTL`, `Salts::TinyTest`
- Produces: the same TurboMedia targets with no `TurboUtils::*` link dependency

- [ ] **Step 1: Capture the exact old-target baseline**

```powershell
rg.exe -n "TurboUtils::(Core|STL|TinyTest)" --glob "CMakeLists.txt"
```

Expected before migration: matches in all applicable files listed above.

- [ ] **Step 2: Apply the target mapping without widening visibility**

Perform only these replacements and retain each existing `PUBLIC`/`PRIVATE` keyword:

```cmake
TurboUtils::Core     -> Salts::Core
TurboUtils::STL      -> Salts::CSTL
TurboUtils::TinyTest -> Salts::TinyTest
```

Update generator expressions such as `$<TARGET_FILE:TurboUtils::Core>` to `$<TARGET_FILE:Salts::Core>`.

- [ ] **Step 3: Prove target names are gone and configure is structurally valid**

```powershell
rg.exe -n "TurboUtils::(Core|STL|TinyTest)" --glob "CMakeLists.txt"
cmake --preset win-dev-user --fresh
```

Expected: the scan has no output; configure must not report an unknown `Salts::Core`, `Salts::CSTL`, or `Salts::TinyTest` target.

- [ ] **Step 4: Commit target migration**

```powershell
git add common core crypto demuxer examples media muxer network pipeline recognition server speech streamer tests webrtc
git commit -m "build: link TurboMedia foundation to Salts"
```

### Task 4: Migrate foundation headers and symbols

**Files:**
- Modify: every production/public/test/example file returned by the scanner from Task 1
- Modify: `common/stl_status.h`
- Modify: `common/container_io.h`
- Modify: `crypto/include/turbo_media_crypto.h`

**Interfaces:**
- Consumes: Salts error/fs/uuid/string headers and CSTL headers
- Produces: existing TurboMedia APIs implemented with `SALTS_E*`, `salts_fs_*`, `salts_uuid_*`, `tstr_*`, and CSTL container APIs

- [ ] **Step 1: Run focused behavior tests before source edits**

```powershell
cmake --build --preset win-dev-user
ctest --preset win-dev-user -R "(containers|crypto|pipeline)" --output-on-failure
```

Expected: record the baseline result and exact test names. If the current mixed dependency state cannot link, record that as the expected red condition and continue with the minimal source migration.

- [ ] **Step 2: Replace headers and prefixed APIs**

Apply these exact transformations, reviewing each call for ownership and return-code assumptions:

```c
#include <turbo_error.h>  /* becomes */ #include <salts_error.h>
#include <turbo_fs.h>     /* becomes */ #include <salts_fs.h>
#include <turbo_uuid.h>   /* becomes */ #include <salts_uuid.h>
#include <turbo_str.h>    /* becomes */ #include <salts_str.h>
#include <turbostl/x.h>   /* becomes */ #include <cstl/x.h>
```

Replace `TURBO_E*` with the same-suffix `SALTS_E*`, `turbo_fs_*`/`TURBO_FS_*` with `salts_fs_*`/`SALTS_FS_*`, and `turbo_uuid_*`/`TURBO_UUID_*` with `salts_uuid_*`/`SALTS_UUID_*`. Keep `tstr_*`, `stl_status`, `vec_*`, and other unchanged CSTL API names.

- [ ] **Step 3: Update the shared STL-status mapping**

In `common/stl_status.h`, map each `stl_status` value to its same-semantics `SALTS_E*` value. Add or retain assertions in `test_containers` for success, allocation/error, invalid argument, and capacity cases so numeric translation is observable at the TurboMedia boundary.

- [ ] **Step 4: Build and run foundation-focused tests**

```powershell
cmake --build --preset win-dev-user
ctest --preset win-dev-user -R "(containers|crypto|pipeline|server_runtime|rtsp)" --output-on-failure
```

Expected: build succeeds and all selected tests pass with no ignored return-code warnings introduced by the migration.

- [ ] **Step 5: Make the legacy static gate pass**

```powershell
rg.exe -n "TurboUtils::|TURBOUTILS_ROOT|<turbostl/|<turbo_(error|fs|uuid|str)\.h>|TURBO_E[A-Z0-9_]+|turbo_(fs|uuid)_[a-z0-9_]+" `
  CMakeLists.txt CMakeUserPresets.json cmake presets common core crypto demuxer examples media muxer network pipeline recognition server speech streamer tests webrtc
```

Expected: no output.

- [ ] **Step 6: Commit source migration**

```powershell
git add common core crypto demuxer examples media muxer network pipeline recognition server speech streamer tests webrtc
git commit -m "refactor: migrate foundation APIs to Salts"
```

### Task 5: Verify installed consumption and dependency purity

**Files:**
- Modify: `tests/package_consumer/CMakeLists.txt`
- Modify: `tests/package_consumer/main.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: installed `TurboMediaConfig.cmake` and `SALTS_ROOT`
- Produces: a clean external consumer build that imports `TurboMedia::Pipeline` without TurboUtils

- [ ] **Step 1: Strengthen the package consumer compile contract**

Ensure `main.c` includes a TurboMedia installed public header whose transitive foundation dependency is representative and invokes a real, non-I/O API. Keep the target link restricted to:

```cmake
target_link_libraries(turbo_media_pipeline_package_consumer
  PRIVATE TurboMedia::Pipeline)
```

Do not link `Salts::*` directly in the consumer; the test must detect broken export propagation.

- [ ] **Step 2: Install and build from a fresh consumer directory**

```powershell
cmake --build --preset install-win-dev-user
cmake -S tests/package_consumer -B build/package-consumer-salts -G Ninja `
  -DCMAKE_PREFIX_PATH="$env:PKG_ROOT/turbomedia/debug"
cmake --build build/package-consumer-salts
```

Expected: configure imports Salts only from `SALTS_ROOT`, and the executable links.

- [ ] **Step 3: Prove missing-root failure is explicit**

```powershell
$savedSaltsRoot = $env:SALTS_ROOT
Remove-Item Env:SALTS_ROOT
cmake -S tests/package_consumer -B build/package-consumer-no-salts -G Ninja `
  -DCMAKE_PREFIX_PATH="$env:PKG_ROOT/turbomedia/debug"
$env:SALTS_ROOT = $savedSaltsRoot
```

Expected: configure fails and names `SALTS_ROOT`. Delete neither build tree in this step; their logs are review evidence.

- [ ] **Step 4: Inspect imported link interfaces for mixed runtime dependencies**

```powershell
rg.exe -n "TurboUtils|turboutils" build/package-consumer-salts build/Msvc
```

Expected: no TurboUtils import or link item. If remaining TurboParser imports TurboUtils transitively, stop merging this plan and execute the parser plan as the next stacked PR.

- [ ] **Step 5: Commit package-consumer coverage**

```powershell
git add tests/package_consumer tests/CMakeLists.txt
git commit -m "test: verify installed Salts foundation dependency"
```

### Task 6: Run the foundation completion gate

**Files:**
- Modify: `README.md` only if it documents dependency roots
- Modify: build documentation returned by `rg.exe -l "TURBOUTILS_ROOT|TurboUtils::" README.md docs --glob "*.md"`

**Interfaces:**
- Consumes: completed Tasks 1–5
- Produces: reviewable verification record for the Foundation GitHub issue

- [ ] **Step 1: Update active build documentation**

Replace active setup instructions with `SALTS_ROOT` and the mapped targets. Historical design records may retain old names only when clearly labeled historical and excluded from the production scanner.

- [ ] **Step 2: Run the complete Windows gate**

```powershell
cmake --preset win-dev-user --fresh
cmake --build --preset win-dev-user
ctest --preset win-dev-user --output-on-failure
cmake --build --preset install-win-dev-user
```

Expected: all commands exit 0.

- [ ] **Step 3: Run final negative scans**

```powershell
rg.exe -n "TurboUtils::|TURBOUTILS_ROOT|<turbostl/|<turbo_(error|fs|uuid|str)\.h>|\bTURBO_E[A-Z0-9_]+\b|\bturbo_(fs|uuid)_[a-z0-9_]+\b" `
  CMakeLists.txt CMakeUserPresets.json cmake presets common core crypto demuxer examples media muxer network pipeline recognition server speech streamer tests webrtc
```

Expected: no output.

- [ ] **Step 4: Commit documentation and record the gate output in the PR/issue**

```powershell
git add README.md docs
git commit -m "docs: document Salts foundation dependency"
```

If no active documentation required an edit, omit this commit and attach the verification output to the Foundation issue instead.
