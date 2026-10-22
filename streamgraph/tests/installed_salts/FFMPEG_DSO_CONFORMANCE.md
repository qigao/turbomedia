# M13 — Genuine FFmpeg Codec in Salts ComponentPlugin DSO

**Scope:** [TurboMedia #165](https://github.com/qigao/turbomedia/issues/165), [#166](https://github.com/qigao/turbomedia/issues/166), [#180](https://github.com/qigao/turbomedia/issues/180). Stacked development on [PR #184](https://github.com/qigao/turbomedia/pull/184), not merged.

## What this executable gate actually does

The independent **installed** Salts.Native **2.3.0-rc.14** SDK supplies the canonical Plugin Loader, ComponentPlugin Generation/Scope and CNet Owner Wake. StreamGraph's own Core SDK is independently installed. The **Linux desktop-only** variant of `streamgraph/tests/installed_salts` links two genuine native Provider DSOs N=101 and N+1=102 to system `libavcodec/libavutil`. Each DSO exports a typed CMeta Component Provider method:

```c
int decode_mjpeg(void *self,
                 const uint8_t *packet, size_t packet_size,
                 uint8_t *out_luma, size_t luma_capacity);
```

This method decodes a **real JPEG packet using FFmpeg `avcodec_send_packet/avcodec_receive_frame` inside the DSO**, copies exactly 16×16 8-bit luma bytes into the caller-owned buffer, and then waits on a deterministic test barrier **while the DSO method remains on the worker's native call stack**. The caller encodes distinct MJPEG packets using the real FFmpeg MJPEG encoder. The public C11 CMeta interface passes *only* borrowed bounded bytes and does not expose `AVFrame`, `AVPacket` or decoder context.

The rest of the path is the **same, already-qualified Salts+StreamGraph lifecycle**, not a second simulation:

```mermaid
sequenceDiagram
    participant H as Media Owner
    participant S as Salts ComponentPlugin
    participant N as FFmpeg Provider DSO N
    participant N1 as FFmpeg Provider DSO N+1
    participant Q as Completion Portal
    participant C as Salts CNet Wake
    participant F as StreamGraph FrameStore
    H->>S: publish N / acquire stable Scope N
    H->>N: start worker decode_mjpeg real libavcodec
    N->>N: decode and hold on DSO stack
    H->>S: publish N+1, drain N returns BUSY
    H->>N: cancel N Portal ticket, unload N returns BUSY
    H->>N1: acquire Scope N+1 / decode real JPEG
    N1->>Q: complete exact generation ticket
    N1->>C: cnet_client_wake after completion
    C-->>H: poll_strategy resumes Media Owner
    H->>H: join callback and wake tail
    H->>Q: tm_od_step_completion N+1
    H->>F: copy decoded luma via Source then settle Sink
    H->>S: release N+1 Scope / drain and unload N+1
    N->>Q: complete cancelled N terminal
    N->>C: cnet_client_wake
    C-->>H: Owner resumes, joins N callback
    H->>Q: reject one N terminal; retained TAKEN credit
    H->>S: generation_drain N and unload N return BUSY
    H->>Q: explicit Host reconciliation / settle
    H->>S: release stable Scope N, drain and unload N
```

**Test invariants:**

- Native Salts Plugin Registry is the **only module loader**. The old Provider Scope is address-stable and cannot be duplicated by a shallow copy. Both DSOs have distinct immutable Plugin IDs; only N+1 is currently published after hot replacement.
- Calling the old typed interface remains authorized while its worker executes despite new publication. `generation_drain(N)` and `cmeta_plugin_registry_unload(N)` return **BUSY** before old Scope release.
- Both native FFmpeg decoders output genuine, verified pixel values. Only N+1's decoded 16×16 luma reaches the existing CPU FrameStore, read-only Processor, and single Sink through `tm_od_step_completion()`. N's decoded frame is **not** permitted to publish after cancellation and downstream terminal rejection.
- The callback thread calls `tm_pc_complete` before cross-thread `cnet_client_wake`. The owner drives `cnet_client_poll_strategy` and the exact Portal result, then joins the callback and wake tail before releasing its Scope or closing the CNet client. Wake does not mean a terminal result is settled.
- A deliberately refused N terminal result leaves the Portal ticket TAKEN; no automatic re-emission, no frame publishing, no Scope release and no unload. Explicit Host reconciliation precedes final release.
- The test uses the official published Salts SDK with checksum, manifest, exact version and source SHA attestation. No Salts checkout, custom native Plugin Loader or unspecified SDK fallback.
- Windows/MSVC and macOS/ARM64 retain the original **non-FFmpeg** real Salts DSO + CNet Wake gate; the optional system FFmpeg DSO profile is enabled only on Linux x64, where desktop libavcodec development packages are explicitly installed.

## Deliberate limitations

This is a **test-only Codec-in-DSO integration fixture**, not a new production FFmpeg Provider implementation. The production-oriented `tm_ffa_decoder` M12 API and the test's typed ComponentProvider `decode_mjpeg` interface are not the same object. This gate proves cross-component lifetime and actual codec invocation **together**, not yet the final production adapter ABI.

No live RTSP/RTC socket, `avformat_open_input` asynchronous demux cancellation, NativeIO request ownership, delayed/reordered frame flush, video/audio format negotiation, GPU fence, frame zero-copy, writable DAG, network error decomposition, or Pipeline v1 retirement is claimed. The installed released Salts binaries and system FFmpeg shared objects are **not themselves TSan-instrumented**; existing StreamGraph-only TSan is a separate qualification. Keep #165/#166/#180 Open until these independent exit gates pass.
