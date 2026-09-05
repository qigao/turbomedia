# Salts Parser and SaltsUtils Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the TurboParser package and aggregate facade by migrating every TurboMedia parser, DataBind, Mustache, and TBE compiler dependency to concrete Salts and SaltsUtils capabilities without changing schemas or wire semantics.

**Architecture:** TurboMedia imports `Salts` and `SaltsUtils` from exact roots, and each target links only the parser formats its sources use. High-level DataBind and Mustache stay owned by SaltsUtils; the TBE compiler is a build-time host tool with a separate root for cross compilation. Migration is organized by behavior clusters so focused tests guard JSON/config/URI/LTV/DataBind semantics before the aggregate parser is removed.

**Tech Stack:** C11, CMake presets, Salts concrete parsers, SaltsUtils DataBind/Mustache, TBE compiler, TinyTest, CTest

**Spec:** `docs/superpowers/specs/2026-09-05-salts-dependency-refactor-design.md`

## Global Constraints

- This plan starts after the Salts Foundation plan; `SALTS_ROOT` and foundation target names already exist.
- Use `SALTS_UTILS_ROOT` with exact `NO_DEFAULT_PATH` discovery; cross builds require `SALTS_UTILS_HOST_ROOT` for `tbe_compiler`.
- Never create `Salts::Parser`, `TurboParser::*`, or `TurboUtils::DataBind` aliases.
- Link parser targets by actual capability and keep them `PRIVATE` unless a public installed header exposes their types.
- Preserve JSON/YAML/TOML/XML/LTV semantics, DataBind schema identity, JSON/XML/BIN round trips, and BIN golden vectors.
- Parsing failures remain explicit; do not guess formats or fall back to TurboParser.

---

### Task 1: Capture parser behavior and static baselines

**Files:**
- Modify: `webrtc/tests/test_signaling_json.c`
- Modify: `webrtc/tests/test_dc_msg.c`
- Modify: `webrtc/apps/sfu_node/tests/test_sfu_node_config.c`

**Interfaces:**
- Consumes: current parser-facing public behavior
- Produces: focused behavior assertions for malformed JSON/config and canonical LTV plus an exact legacy identifier inventory

- [ ] **Step 1: Record the legacy parser inventory**

```powershell
rg.exe -n "TurboParser::|TurboUtils::DataBind|TURBOPARSER_ROOT|TURBOPARSER_HOST_ROOT|turbo_parser\.h|turbo_(json|yaml|toml|xml|uri|ltv|datetime)_[a-z0-9_]+" `
  CMakeLists.txt CMakeUserPresets.json cmake presets common core crypto demuxer examples media muxer network pipeline recognition server speech streamer tests webrtc
```

Expected: matches identify every source/header and compiling target that must move. This is a review/CI policy check, not a CTest behavior test.

- [ ] **Step 2: Strengthen format behavior tests before implementation**

Add assertions that prove:

```text
signaling JSON: valid offer/answer/candidate round-trip; malformed/truncated JSON rejected
SFU/config: required TOML fields read; duplicate/invalid values rejected with the existing error class
LTV: canonical varint accepted; overlong varint, short frame, and payload > LTV_MAX_PAYLOAD_SIZE rejected
```

Reuse existing TinyTest helpers and existing fixtures; do not add a second parser test harness.

- [ ] **Step 3: Run focused behavior tests before implementation**

```powershell
cmake --preset win-dev-user
cmake --build --preset win-dev-user
ctest --preset win-release-user -R "(signaling_json|sfu_node_config|dc_msg)" --output-on-failure
```

Expected: behavior tests pass on the old implementation when an old SDK baseline is available. If the old SDK is intentionally absent, record the configure failure and execute these tests immediately after the first buildable Salts migration.

- [ ] **Step 4: Commit the behavior gates**

```powershell
git add webrtc/tests/test_signaling_json.c webrtc/tests/test_dc_msg.c webrtc/apps/sfu_node/tests/test_sfu_node_config.c
git commit -m "test: lock parser migration semantics"
```

### Task 2: Import SaltsUtils and resolve the host TBE compiler

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `CMakeUserPresets.json`
- Modify: `cmake/TurboMediaConfig.cmake.in`

**Interfaces:**
- Consumes: `SALTS_UTILS_ROOT`; `SALTS_UTILS_HOST_ROOT` when `CMAKE_CROSSCOMPILING`
- Produces: imported `Salts::DataBind`, `Salts::Mustache`, optional `Salts::Capture`, and absolute host `tbe_compiler` path

- [ ] **Step 1: Write the expected negative configure cases**

Run and record failure for both missing roots:

```powershell
$savedSaltsUtilsRoot = $env:SALTS_UTILS_ROOT
Remove-Item Env:SALTS_UTILS_ROOT -ErrorAction SilentlyContinue
cmake -S . -B build/salts-utils-root-negative -G Ninja
$env:SALTS_UTILS_ROOT = $savedSaltsUtilsRoot
```

For an Android configure, remove `SALTS_UTILS_HOST_ROOT`; expected failure must identify the missing host compiler root rather than attempting to execute an Android binary.

- [ ] **Step 2: Add exact SaltsUtils discovery**

Implement:

```cmake
if(NOT DEFINED ENV{SALTS_UTILS_ROOT} OR "$ENV{SALTS_UTILS_ROOT}" STREQUAL "")
  message(FATAL_ERROR "SALTS_UTILS_ROOT must name the installed SaltsUtils profile")
endif()
file(TO_CMAKE_PATH "$ENV{SALTS_UTILS_ROOT}" SALTS_UTILS_ROOT_PATH)
find_package(SaltsUtils CONFIG REQUIRED
  PATHS "${SALTS_UTILS_ROOT_PATH}" NO_DEFAULT_PATH)
```

The package config must import `Salts` from `SALTS_ROOT` before importing exported TurboMedia targets.

- [ ] **Step 3: Replace TBE compiler root selection**

Use this root decision in `turbo_media_find_tbe_compiler`:

```cmake
if(CMAKE_CROSSCOMPILING)
  set(_turbomedia_tbe_root_name SALTS_UTILS_HOST_ROOT)
else()
  set(_turbomedia_tbe_root_name SALTS_UTILS_ROOT)
endif()
```

Search only `${root}/bin` with `NO_DEFAULT_PATH`, validate that the resolved file exists, and keep the absolute path in the existing imported executable/command boundary.

- [ ] **Step 4: Update every native and Android preset**

Set `SALTS_UTILS_ROOT` to the matching `salts-utils/{debug,release}` or `salts-utils-android/{debug,release}` profile. For Android set:

```json
"SALTS_UTILS_HOST_ROOT": "$env{PKG_ROOT}/salts-utils/release"
```

Replace runtime `TURBOPARSER_ROOT/bin|lib` entries with `SALTS_UTILS_ROOT/bin|lib` and remove the old root variables.

- [ ] **Step 5: Commit package and compiler discovery**

```powershell
git add CMakeLists.txt CMakeUserPresets.json cmake/TurboMediaConfig.cmake.in
git commit -m "build: import SaltsUtils and host TBE compiler"
```

### Task 3: Migrate JSON consumers to `Salts::JsonParser`

**Files:**
- Modify: `pipeline/src/turbo_pipeline.c`
- Modify: `webrtc/examples/signaled_peer.c`
- Modify: `webrtc/src/auth/turbo_media_auth.c`
- Modify: `webrtc/src/signaling/webrtc_signaling.c`
- Modify: `webrtc/apps/sfu_node/src/http_api.c`
- Modify: `webrtc/apps/room_service/src/http_api.c`
- Modify: `webrtc/apps/room_service/src/iris_completion_dispatcher.c`
- Modify: `webrtc/apps/room_service/src/iris_flowmq_provider_codec.c`
- Modify: `webrtc/apps/room_service/src/iris_media_bridge.c`
- Modify: `webrtc/apps/room_service/src/iris_media_reconciler.c`
- Modify: `webrtc/apps/room_service/src/iris_orm_store.c`
- Modify: `webrtc/apps/room_service/src/iris_room_bridge.c`
- Modify: `webrtc/apps/room_service/src/server.c`
- Modify: JSON-using tests under `webrtc/tests/` and `webrtc/apps/room_service/tests/`
- Modify: the matching target definitions in `pipeline/CMakeLists.txt`, `webrtc/CMakeLists.txt`, `webrtc/examples/CMakeLists.txt`, `webrtc/apps/sfu_node/CMakeLists.txt`, and `webrtc/apps/room_service/CMakeLists.txt`

**Interfaces:**
- Consumes: `<json_parser.h>` and installed `Salts::JsonParser`
- Produces: unchanged TurboMedia JSON messages/config/results with no `turbo_json_*` use

- [ ] **Step 1: Partition the exact JSON call sites**

```powershell
rg.exe -l "\bturbo_json_[a-z0-9_]+\b|turbo_parser\.h" pipeline webrtc
```

For each returned source, record which target compiles it before editing CMake. A target receives `Salts::JsonParser` only when at least one of its own sources contains the JSON API.

- [ ] **Step 2: Convert includes, types, calls, and cleanup paths**

Use `<json_parser.h>` and the corresponding `json_*` API declared by the installed Salts header. Preserve the existing ownership pattern: parse result/tree is destroyed on the single cleanup path, borrowed string/value views are not retained past the tree lifetime, and allocation/parse errors map to the same TurboMedia error class.

Representative conversion shape:

```c
#include <json_parser.h>

json_value_t *root = json_parse(input, input_len);
if (root == NULL) {
    return SALTS_EPROTO;
}
/* Existing field validation and result construction stay in the same order. */
json_free(root);
```

Use the actual names/signatures from `json_parser.h`; do not invent a wrapper solely to preserve `turbo_json_*` spelling.

- [ ] **Step 3: Link only JSON-consuming targets**

```cmake
target_link_libraries(the_existing_target PRIVATE Salts::JsonParser)
```

If an installed public header exposes a Salts JSON type, make that one edge `PUBLIC`; otherwise keep it `PRIVATE`.

- [ ] **Step 4: Build and run the JSON cluster**

```powershell
cmake --build --preset win-dev-user
ctest --preset win-dev-user -R "(pipeline|signaling_json|auth_token|sfu_node|room_service|iris_)" --output-on-failure
```

Expected: all discovered matching tests execute and pass; verify the regex selected at least one test with `ctest --test-dir build/Msvc -N -R` first.

- [ ] **Step 5: Commit JSON migration**

```powershell
git add pipeline webrtc
git commit -m "refactor: migrate JSON consumers to Salts parser"
```

### Task 4: Migrate TOML, YAML, XML, and DateTime consumers

**Files:**
- Modify: `webrtc/apps/config_toml.h`
- Modify: `webrtc/apps/signaling_server/src/config.c`
- Modify: `webrtc/apps/sfu_node/src/config.c`
- Modify: every additional file returned by the format scan below
- Modify: matching CMake target files under `webrtc/apps/`, `webrtc/ivr/`, `pipeline/`, and `tests/`

**Interfaces:**
- Consumes: `<toml.h>`/`Salts::TomlParser`, `<cyaml.h>`/`Salts::CYaml`, `<xml_parser/xml_parser.h>`/`Salts::XmlParser`, `<datetime_parser.h>`/`Salts::DateTimeParser`
- Produces: unchanged configuration and serialization semantics without facade API use

- [ ] **Step 1: Identify each capability and owning target**

```powershell
rg.exe -n "\bturbo_(toml|yaml|xml|datetime)_[a-z0-9_]+\b" pipeline tests webrtc
```

Do not assign all four targets to every consumer. Map source → compiling target → exact parser target.

- [ ] **Step 2: Convert TOML configuration paths**

Use `<toml.h>` and `toml_parse`/typed accessors/`toml_free` from Salts. Preserve required-field checks, duplicate-key rejection, environment override order, string bounds, and cleanup on every failure path.

- [ ] **Step 3: Convert YAML, XML, and DateTime paths**

Use each installed public header directly. Preserve borrowed/owned rules documented by that header, reject malformed/unknown values at the same boundary, and return existing TurboMedia/Salts error codes rather than logging and succeeding.

- [ ] **Step 4: Link concrete capabilities and test configuration behavior**

```powershell
cmake --build --preset win-dev-user
ctest --preset win-dev-user -R "(signaling_config|sfu_node_config|ivr_schema|pipeline)" --output-on-failure
```

Expected: valid fixtures produce the same values; malformed and duplicate fixtures fail with the expected class.

- [ ] **Step 5: Commit multi-format configuration migration**

```powershell
git add pipeline tests webrtc
git commit -m "refactor: migrate config parsers to concrete Salts targets"
```

### Task 5: Migrate URI parsing without weakening validation

**Files:**
- Add: `network/transport_url.c`
- Add: `tests/test_transport_url.c`
- Modify: `network/transport_coronet.c`
- Modify: `network/CMakeLists.txt`
- Modify: `tests/package_consumer/transport_consumer.c`

**Interfaces:**
- Consumes: `<uri_parser.h>`, `int uri_parse(const char *, uri_t *)`, `Salts::UriParser`
- Produces: the existing transport endpoint configuration and error behavior

- [ ] **Step 1: Add URI boundary cases to the owning transport test**

Cover:

```text
valid scheme/host/port/path/query
missing or unsupported scheme
empty host where transport requires one
port overflow (`URI_OVERFLOW_PORT`)
component overflow (`URI_OVERFLOW_COMPONENT`)
malformed input (`valid == 0`)
```

Assertions must check the TurboMedia return code and that no partially initialized transport is exposed.

- [ ] **Step 2: Prove the new overflow tests fail against facade-only behavior when applicable**

```powershell
cmake --build --preset win-dev-user
ctest --preset win-dev-user -R "(transport|network)" --output-on-failure
```

Expected: newly exposed overflow cases fail before the direct `uri_t` implementation or existing behavior is recorded if already correct.

- [ ] **Step 3: Replace convenience getters with explicit `uri_t` checks**

```c
uri_t parsed = {0};
if (!uri_parse(endpoint, &parsed) || !parsed.valid || parsed.overflow_flags != 0) {
    return SALTS_EINVAL;
}
```

Then copy `scheme`, `host`, `port`, `path`, and optional `query` into the existing transport configuration using existing bounded-copy utilities. Use `component_flags` to distinguish absent from explicitly empty query/port components.

- [ ] **Step 4: Link and verify**

Add `Salts::UriParser` to the compiling target, keep it `PRIVATE` unless its type appears in an installed header, then run the transport tests. Expected: all boundary cases pass and sanitizer/debug builds report no lifetime issue.

- [ ] **Step 5: Commit URI migration**

```powershell
git add network tests
git commit -m "refactor: migrate transport URI parsing to Salts"
```

### Task 6: Migrate LTV while protecting the installed DataChannel API

**Files:**
- Modify: `webrtc/include/turbo_dc_msg.h`
- Modify: `webrtc/src/turbo_dc_msg.c`
- Modify: `webrtc/tests/test_dc_msg.c`
- Modify: `webrtc/CMakeLists.txt`
- Add: `tests/package_consumer/datachannel_consumer.c`

**Interfaces:**
- Consumes: `<ltv_parser.h>`, `ltv_message_t`, `ltv_build`, `ltv_parse`, `Salts::LtvParser`
- Produces: TurboMedia DataChannel message build/parse APIs with canonical LTV wire bytes

- [ ] **Step 1: Decide the public boundary from actual callers**

```powershell
rg.exe -n "turbo_ltv_|ltv_message_t|turbo_dc_msg" --glob "*.c" --glob "*.h"
```

If external-facing TurboMedia APIs can return TurboMedia-owned scalar/enum values, remove parser types from `turbo_dc_msg.h` and include `<ltv_parser.h>` only in `.c`. If existing public signatures necessarily expose `ltv_message_t`, include `<ltv_parser.h>` publicly and make `Salts::LtvParser` a `PUBLIC` link edge. Do not add typedef aliases for old TurboParser types.

- [ ] **Step 2: Convert build and streaming parse calls**

Use `ltv_wire_size` before allocation, `ltv_build` for all-or-nothing encoding, and `ltv_parse`/stream API for decoding. Preserve `LTV_MAX_PAYLOAD_SIZE`, canonical unsigned LEB128, zero-copy `value` lifetime, and buffer ownership.

- [ ] **Step 3: Run golden and malformed-frame tests**

```powershell
cmake --build --preset win-dev-user
ctest --preset win-dev-user -R "^turbo_media_test_dc_msg$" --output-on-failure
```

Expected: canonical bytes match the pre-migration fixture; short, overlong, oversized, and buffer-full cases return their expected errors.

- [ ] **Step 4: Build the installed package consumer**

Run the install-consumer flow from the foundation plan. Expected: `turbo_dc_msg.h` compiles from the install tree and its transitive `Salts::LtvParser` dependency is correct for the selected public boundary.

- [ ] **Step 5: Commit LTV migration**

```powershell
git add webrtc tests/package_consumer
git commit -m "refactor: migrate DataChannel LTV parsing to Salts"
```

### Task 7: Migrate DataBind and Mustache to SaltsUtils

**Files:**
- Modify: `pipeline/CMakeLists.txt`
- Modify: `webrtc/ivr/CMakeLists.txt`
- Modify: `webrtc/apps/room_service/CMakeLists.txt`
- Modify: every CMake file returned by `rg.exe -l "TurboParser::(DataBind|Mustache)|TurboUtils::DataBind" --glob "CMakeLists.txt"`
- Modify: generated-binding build rules that invoke `turbo_media_find_tbe_compiler`

**Interfaces:**
- Consumes: `Salts::DataBind`, `Salts::Mustache`, resolved host `tbe_compiler`
- Produces: the same IVR schemas/generated types/templates and semantic wire results

- [ ] **Step 1: Capture existing schema and BIN golden behavior**

```powershell
cmake --build --preset win-dev-user
ctest --preset win-dev-user -R "(test_ivr_schema$|test_ivr_schema_typed|test_ivr_protocol|test_ivr_flowmq|iris_flowmq_provider_codec)" --output-on-failure
```

Expected: record current semantic round-trip and golden-vector results before target migration.

- [ ] **Step 2: Replace high-level targets and runtime copy expressions**

```cmake
TurboParser::DataBind -> Salts::DataBind
TurboUtils::DataBind  -> Salts::DataBind
TurboParser::Mustache -> Salts::Mustache
```

Update `$<TARGET_FILE:...>` expressions to the Salts target. Preserve existing `PUBLIC`/`PRIVATE` visibility based on installed generated headers.

- [ ] **Step 3: Regenerate typed bindings with the SaltsUtils compiler**

Run a clean build so no old generated file masks compiler incompatibility:

```powershell
cmake --preset win-dev-user --fresh
cmake --build --preset win-dev-user
```

Expected: build output invokes the absolute compiler under `SALTS_UTILS_ROOT/bin`; generated headers/sources compile against installed `data_bind.h` and `tbe_typed.h`.

- [ ] **Step 4: Re-run semantic and golden tests**

```powershell
ctest --preset win-dev-user -R "(test_ivr_schema$|test_ivr_schema_typed|test_ivr_protocol|test_ivr_flowmq|iris_flowmq_provider_codec)" --output-on-failure
```

Expected: JSON/XML/BIN semantic values and BIN golden bytes are unchanged.

- [ ] **Step 5: Commit high-level utility migration**

```powershell
git add CMakeLists.txt CMakeUserPresets.json cmake pipeline webrtc
git commit -m "refactor: migrate DataBind and Mustache to SaltsUtils"
```

### Task 8: Remove the aggregate parser and complete verification

**Files:**
- Modify: every residual file returned by the scanner from Task 1
- Modify: `cmake/TurboMediaConfig.cmake.in`
- Modify: active dependency documentation under `README.md`, `docs/`, and `webrtc/docs/`

**Interfaces:**
- Consumes: completed concrete-parser and SaltsUtils migrations
- Produces: no TurboParser package/root/header/symbol reference and a reproducible Parser issue acceptance record

- [ ] **Step 1: Remove package discovery, roots, aliases, and residual facade links**

Delete `find_package(TurboParser ...)`, `TURBOPARSER_ROOT`, `TURBOPARSER_HOST_ROOT`, the `TurboUtils::DataBind` alias block, and every `TurboParser::Parser` link item. For each removed aggregate edge, add only the concrete targets proven by that target's source scan.

- [ ] **Step 2: Make the legacy parser static gate pass**

```powershell
rg.exe -n "TurboParser::|TurboUtils::DataBind|TURBOPARSER_(ROOT|HOST_ROOT)|turbo_parser\.h|\bturbo_(json|yaml|toml|xml|uri|ltv|datetime)_[a-z0-9_]+\b" `
  CMakeLists.txt CMakeUserPresets.json cmake presets common core crypto demuxer examples media muxer network pipeline recognition server speech streamer tests webrtc
```

Expected: no output.

- [ ] **Step 3: Run Windows full and install-consumer gates**

```powershell
cmake --preset win-dev-user --fresh
cmake --build --preset win-dev-user
ctest --preset win-dev-user --output-on-failure
cmake --build --preset install-win-dev-user
cmake -S tests/package_consumer -B build/package-consumer-parser -G Ninja `
  -DCMAKE_PREFIX_PATH="$env:PKG_ROOT/turbomedia/debug"
cmake --build build/package-consumer-parser
```

Expected: all commands exit 0; package config resolves only `Salts` and `SaltsUtils` from exact roots.

- [ ] **Step 4: Run Android host/target separation gate**

```powershell
cmake --preset android-arm64-v8a-debug-win --fresh
cmake --build --preset android-arm64-v8a-debug-win
cmake --build --preset install-android-arm64-v8a-debug-win
```

Expected: code generation uses host `SALTS_UTILS_HOST_ROOT/bin/tbe_compiler`; target links use Android Salts/SaltsUtils roots.

- [ ] **Step 5: Update active documentation and commit the final removal**

```powershell
git add CMakeLists.txt CMakeUserPresets.json cmake presets README.md docs pipeline tests webrtc network
git commit -m "refactor: remove TurboParser aggregate dependency"
```
