# TurboMedia Client Media Processing

## Status

Proposed architecture for issue #50.

This decision keeps the existing `TurboMedia::Pipeline` contract server-only and
introduces a separate client media-processing product boundary. The client
processing core must build on Windows, Linux, macOS, Android, and iOS without
depending on ServerRuntime, Streamer, TurboDB, RulesForge, or server
applications.

## Decision

Add a future installed component named `TurboMedia::ClientProcessing`.

It is not an alias, subset build, or compatibility mode of
`TurboMedia::Pipeline`. The two products may reuse lower-level TurboMedia
Codec/Demuxer/Muxer contracts and third-party codec implementations, but they
do not share runtime ownership, graph configuration, or server-only adapters.

The dependency direction is:

```text
Salts::Capture / Salts::Playback
              |
              v
client adapters
              |
              v
TurboMedia::ClientProcessing
      |        |        |
      v        v        v
   Codec    Demuxer    Muxer
      \        |        /
       format-neutral frame/packet contract

TurboMedia::Pipeline
      |
      +--> ServerRuntime / RTP / Streamer
```

There is no dependency from ClientProcessing to Pipeline and no dependency
from Pipeline to ClientProcessing.

## Why not reuse TurboMedia::Pipeline

The current Pipeline contract is intentionally server-oriented:

- it is built only for the SERVER product;
- its Runtime/RTP path borrows ServerRuntime;
- its YAML graph is designed around long-lived server processing and
  deployment-owned inputs/outputs;
- server shutdown, source publication, and retained Runtime/RTP resources are
  part of its ownership model;
- SERVER is intentionally limited to Windows/Linux.

Making that target available to CLIENT would pull server ownership and
platform assumptions into mobile builds. Hiding those dependencies behind
feature flags would recreate the mixed-product dependency graph that the
CLIENT/SERVER split removed.

ClientProcessing therefore gets its own API and lifecycle even where both
products happen to use FFmpeg internally.

## Public API shape

The public API is a stable C ABI with opaque handles. No FFmpeg, device, Java,
Objective-C, or platform-native type crosses the boundary.

Proposed public values:

```c
typedef struct turbo_client_processing_s turbo_client_processing_t;

typedef enum turbo_client_processing_state_t {
    TURBO_CLIENT_PROCESSING_CREATED = 0,
    TURBO_CLIENT_PROCESSING_PREPARED,
    TURBO_CLIENT_PROCESSING_RUNNING,
    TURBO_CLIENT_PROCESSING_PAUSED,
    TURBO_CLIENT_PROCESSING_DRAINING,
    TURBO_CLIENT_PROCESSING_STOPPED,
    TURBO_CLIENT_PROCESSING_FAILED
} turbo_client_processing_state_t;

typedef struct turbo_client_frame_view_t {
    const void *planes[4];
    size_t strides[4];
    uint32_t width;
    uint32_t height;
    uint32_t sample_rate;
    uint32_t channels;
    uint64_t pts;
    uint32_t time_base_num;
    uint32_t time_base_den;
    uint32_t format;
    uint32_t media_type;
} turbo_client_frame_view_t;
```

Frame views are borrowed. A callback must not retain plane pointers after
return. Any adapter crossing a thread, queue, coroutine, Java/Objective-C
boundary, or asynchronous encoder boundary must copy or move the payload into
bounded owned storage first.

The initial API should expose:

- create from an immutable plan;
- prepare;
- run or step from one owner context;
- request pause/resume;
- request stop;
- drain;
- snapshot state/statistics;
- destroy.

No implicit background worker is created by the core. An adapter may own a
worker only when that ownership is explicit in its API and shutdown contract.

## Plan model

ClientProcessing uses a small typed plan, not the server Pipeline YAML schema.

The v1 plan contains exactly one input, zero or one transform chain, and one
output. It deliberately does not try to become a general nonlinear editor.

Initial endpoint classes:

| Endpoint | v1 | Ownership |
| --- | --- | --- |
| file input | yes | ClientProcessing |
| memory input | after file slice | caller supplies copied/retained bytes |
| Salts Capture input | after core slice | Salts owns device; adapter owns bounded copied frames |
| file output | yes | ClientProcessing |
| preview callback | after core slice | callback receives borrowed frame view |
| Salts Playback output | after core slice | Salts owns device; adapter submits bounded PCM |
| memory output | later | caller-owned bounded sink |

Initial transforms:

- decode one selected audio or video stream;
- one bounded format conversion/filter operation;
- encode one selected output stream;
- mux to one file.

No arbitrary filter graph labels, multi-input composition, dynamic graph
editing, fan-out, or timeline editing are part of v1.

## First vertical slice

The first complete implementation slice is:

```text
file
  -> demux
  -> decode
  -> one bounded video scale/format transform
  -> encode
  -> mux
  -> file
```

The slice is intentionally device-independent so it can be made deterministic
on all five CLIENT platforms before adding camera/surface/audio-session
lifecycle.

Acceptance for this slice:

- fixed local input fixture;
- output stream has the expected codec, dimensions, time base, and duration;
- input timestamps remain monotonic after transformation;
- every queue has explicit item and byte capacity;
- max+1 admission returns a deterministic backpressure error;
- cancel during decode, transform, encode, and mux closes every owned resource;
- unsupported codec/filter fails during prepare, never by silent fallback;
- OOM/fault injection leaves no published half-initialized handle.

## Capture and playback adapters

Capture and Playback remain exclusively owned by Salts.

ClientProcessing never implements platform capture/playback and never exposes
a second device identity or lifecycle model.

### Capture

`Salts::Capture` owns:

- device enumeration and identity;
- permission/platform setup;
- native capture callbacks;
- device start/stop/destroy.

The ClientProcessing capture adapter:

- receives borrowed Salts frames;
- copies each admitted frame into a preallocated/bounded queue;
- rejects on item/byte/time budget exhaustion;
- preserves source timestamp and format metadata;
- never blocks the Salts device callback waiting for the processing graph.

### Playback / preview

`Salts::Playback` owns audio output. ClientProcessing submits decoded PCM and
treats short writes/backpressure as an explicit processing event.

Video preview is a separate adapter. The core publishes a borrowed frame view
to a platform binding that must copy/retain before asynchronous UI/surface
use.

## Mobile lifecycle

The core models platform events explicitly:

- app pause;
- app resume;
- permission revoked;
- capture device lost;
- playback device lost;
- video surface lost;
- video surface replaced;
- background execution denied.

These events are not translated into automatic fallback.

Expected behavior:

| Event | Required behavior |
| --- | --- |
| pause | stop accepting new device frames, quiesce owned processing work |
| resume | explicit adapter revalidation before new admission |
| permission revoked | fail current device path with a stable error |
| device lost | emit device-lost state; no automatic alternate-device selection |
| surface lost | stop preview delivery while core processing policy remains explicit |
| cancel | reject new work, drain/cancel admitted work, then stop |
| destroy | valid only after owned callbacks/workers are quiescent |

## Queue and memory budgets

Every asynchronous boundary has three independent bounds:

- item count;
- retained bytes;
- retained media time.

The v1 core does not use `DROP_OLDEST` as a default policy. When a bound is
reached, the producer receives an explicit full/backpressure result.

Statistics expose at least:

- admitted frames/packets;
- rejected frames/packets;
- queue high-water items;
- queue high-water bytes;
- processing latency;
- drain duration;
- codec/filter errors.

Statistics are snapshots only and do not become a second state owner.

## Codec and filter capability

Capability is explicit and queryable before prepare.

A plan names:

- media type;
- input codec when applicable;
- output codec;
- pixel/sample format;
- transform operation.

Unsupported combinations fail during prepare.

No behavior is allowed to silently switch:

- hardware to software codec;
- requested codec to another codec;
- requested filter to pass-through;
- capture device to a different device.

Hardware acceleration may be introduced only behind an explicit capability
record that names the backend and supported formats.

## CMake / installed target boundary

Proposed target:

```text
TurboMedia::ClientProcessing
```

Rules:

- built only for `TURBO_MEDIA_PRODUCT=CLIENT`;
- available on Windows, Linux, macOS, Android, and iOS;
- must not link `TurboMedia::Server`, `TurboMedia::Pipeline`,
  `TurboMedia::Streamer`, TurboDB, RulesForge, or server applications;
- may link shared TurboMedia Codec/Demuxer/Muxer targets;
- Capture/Playback adapter targets may link `Salts::Capture` and
  `Salts::Playback`;
- installed package consumers must be able to request ClientProcessing without
  resolving any SERVER target.

The package contract test must fail if a future change introduces a
Server/Pipeline dependency edge.

## Platform capability matrix

The initial target matrix is conservative:

| Capability | Win | Linux | macOS | Android | iOS |
| --- | --- | --- | --- | --- | --- |
| core create/prepare/run/drain | yes | yes | yes | yes | yes |
| local file input/output | yes | yes | yes | yes | yes |
| software decode/encode | explicit codec matrix | explicit codec matrix | explicit codec matrix | explicit codec matrix | explicit codec matrix |
| Salts Capture adapter | yes | yes | yes | yes | yes |
| Salts Playback adapter | yes | yes | yes | yes | yes |
| preview adapter | desktop callback | desktop callback | native surface adapter | native surface adapter | native surface adapter |
| hardware codec | not v1 default | not v1 default | not v1 default | not v1 default | not v1 default |

A platform being listed does not imply every codec is available. The package
must expose the actual runtime/build capability set.

## Licensing and distribution

FFmpeg/codec dependencies are implementation choices, not public API.

Before enabling an encoder/filter in a shipping profile, the product build
must record:

- enabled FFmpeg features;
- codec implementation;
- static/dynamic linkage;
- relevant license obligations;
- mobile package-size delta.

The core must not auto-discover an ambient system FFmpeg installation in a
way that changes the shipping capability contract.

## Verification

Phase 1 design/package gate:

- CLIENT configure exports no SERVER targets;
- ClientProcessing target dependency graph contains no server-only edge;
- installed-package consumer includes only public ClientProcessing headers;
- unsupported platform/product combinations fail at configure time.

Phase 2 deterministic core gate:

- local fixture vertical slice on desktop;
- exact timestamp/format/output assertions;
- queue max and max+1 tests;
- cancel/drain/destroy tests;
- injected allocation/codec failure tests.

Phase 3 platform adapter gate:

- Capture callback ownership tests;
- Playback backpressure tests;
- pause/resume/device-loss/surface-loss tests;
- Android/iOS lifecycle smoke tests;
- desktop ASan qualification where available.

## Relationship to other issues

- #50 owns this ClientProcessing core and adapters.
- #51 may consume the format-neutral frame/codec contract but owns WebRTC/SIP
  session state, signaling, RTP, NAT traversal, and live-client policy.
- #46 owns SERVER Runtime/RTP multi-track behavior.
- #49 owns SERVER Pipeline runtime graph reconfiguration.
- #47/#48 own SERVER Pipeline graph/fan-out expansion.
- #17 owns real-browser/TURN production evidence.

These issues must not be merged into one universal media runtime.

## Migration and rollback

ClientProcessing is additive.

Existing CLIENT Player/Mobile and SERVER Pipeline behavior stays unchanged
until a caller explicitly links the new component.

Rollback is therefore removal of the new target and its adapters; it requires
no stored-data migration and does not require a compatibility alias to
`TurboMedia::Pipeline`.
