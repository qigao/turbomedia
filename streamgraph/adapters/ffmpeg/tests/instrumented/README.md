# M15b — deterministic cancellation from inside real libavcodec

Tracking: [#188](https://github.com/qigao/turbomedia/issues/188), [#166](https://github.com/qigao/turbomedia/issues/166), Draft [#187](https://github.com/qigao/turbomedia/pull/187) (**DO NOT MERGE**).

## What is actually proven

The dedicated Linux workflow builds *test-only* FFmpeg **6.1.1** from immutable release commit `e38092ef9395d7049f871ef4d5411eb410e283e0`. The Python patch checks the exact FFmpeg function shape and inserts a one-shot atomic gate **inside** `avcodec_send_packet()`, **after successful packet referencing** and **before the internal decode attempt**. It is not a wrapper, a pre-call TurboMedia gate or a claim about unmodified FFmpeg.

The test configures two genuine `tm_ffa_decoder` instances. A enters the patched libavcodec call and stops at the gate while the original Codec, borrowed input and worker-owned DSO-independent state remain alive. B, using its *own* Codec and Portal, must decode its packet and publish its luma without waiting for A. The owner then requests Portal cancellation and `tm_ffa_owner_cancel(A)` while A is still suspended inside `avcodec_send_packet`. Destroy(A) rejects while the worker is active. Only after the gate is released may the worker finish, publish its single `CANCELLED` terminal (with completion observed) and wake once; A publishes no luma. B's decoded luma transfers once with no cross-ticket leak. Both Portals are settled and destroyed before both decoders are destroyed.

The new `tm_ff_codec_activity_v1` first-party atomic snapshot independently reports `in_call=1` while A is stopped and `packet_calls=1, in_call=0` after return. The patched libavcodec gate state, not this snapshot by itself, establishes the in-function location.

```mermaid
sequenceDiagram
    participant Owner as Media Owner
    participant A as Worker A
    participant FF as Instrumented libavcodec
    participant B as Worker B
    participant P as Original Portals
    Owner->>A: start MJPEG packet
    A->>FF: enter avcodec_send_packet
    FF->>FF: av_packet_ref; hold in function body
    FF-->>Owner: instrumented gate state 2
    Owner->>B: start independent Decoder B
    B->>P: complete B exactly once
    Owner->>P: request_cancel A ticket
    Owner->>A: tm_ffa_owner_cancel A metadata
    Owner->>A: attempt destroy A returns ESTATE
    Owner->>FF: release in-function gate
    FF-->>A: return from actual avcodec_send_packet
    A->>P: complete A ticket as CANCELLED
    A-->>Owner: invoke wake exactly once
    Owner->>P: settle both terminals
    Owner->>A: join before destroy
```

## Reproducibility and boundaries

- CI: [Instrumented FFmpeg cancellation workflow](https://github.com/qigao/turbomedia/actions/workflows/streamgraph-ffmpeg-internal-cancel.yml). The job checks the immutable FFmpeg commit, patched symbol export, selected pkg-config prefix and `ldd` resolution into the test-only libavcodec. CTest repeats the process ten times, so the one-shot gate is freshly armed each run.
- The existing normal FFmpeg workflow still runs native, ASan/UBSan and TSan tests against system FFmpeg. This extra Linux gate uses a purpose-built, modified FFmpeg and is **not** a release dependency.
- This proves A's cancellation with a packet **already referenced inside libavcodec**, not cancellation at an arbitrary CPU instruction throughout every FFmpeg decoder, codec, format or flush path. It is not proof of a real RTSP/RTC NativeIO cancellation, of a full ComponentPlugin Generation Scope unloading while the patched function is suspended, or of GPU fences.
- Separate acceptance gates remain for NULL-packet flush and delayed frames (M15c), and for exact installed Salts ComponentPlugin DSO + Generation Scope in the same instrumented interleaving (#180). Do not close those tasks or merge #187 solely on this evidence.
