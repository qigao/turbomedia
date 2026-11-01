# M17c — Real TurboMedia H264 decoder into the existing CPU StreamGraph

**Status: implementation and real Codec test committed; product-root CI
qualification still pending due upstream immutable SDK restoration.**
Stacked on M17b [PR #277](https://github.com/qigao/turbomedia/pull/277)
and M17a [PR #276](https://github.com/qigao/turbomedia/pull/276).
Tracks [M17 #275](https://github.com/qigao/turbomedia/issues/275).

## Production runtime ownership

The new opt-in `TurboMedia::StreamGraphH264` component links the SAME
`TurboMedia::Codec` (OpenH264) and `TurboMedia::StreamGraph` product
targets. It exports `turbo_streamgraph_h264.h`. It does not contain
another decoder algorithm, FrameStore, scheduler, event loop or thread.

The host's existing **Media Owner** controls all callbacks and packet
admission. The Graph still executes only **one CPU VideoFrame Source,
1–8 read-only Processors and 1–8 Sinks**. The heterogeneous Android
EncodedPacket→Decode→Vision→Observation graph remains a compile-only
typed plan, not a runtime that has magically acquired new node types.

## Exact first runnable profile

1. Caller explicitly initializes the existing TurboMedia Codec registry.
   `tm_sh_create` validates the M17b immutable device/display/capture/stream
   identity, even dimensions, and the I420 byte budget (at most 8 MiB).
   It owns one existing OpenH264 decoder and one bounded packet ingress.
2. `tm_sh_offer` copies a verified Annex-B SPS/PPS CONFIG packet; the
   owner calls `tm_sh_pump`. A real `TURBO_CODEC_ERR_NEED_MORE` from the
   existing decoder is accepted for CONFIG only.
3. The next IDR packet has the correct sequence/generation and PTS=DTS.
   `tm_sh_pump` invokes `turbo_codec_decode` and requires a full exact-size
   I420 output. It stages **one** decoded frame; it never double-decodes
   on downstream backpressure or accepts unqualified B-frame delay.
4. The owner checks `tm_sh_ready` before asking the existing CPU Executor
   to pump the Source (except when the Executor already retains its own
   pending frame). `tm_sh_next_cpu_i420` copies the staged frame into the
   Executor's existing scratch. The Executor publishes to the existing CPU
   FrameStore and charges independent Fanout pins to ordered lossless,
   sample or latest consumers. No pixels cross Turbo-Flow messages.
5. During the immediate owner-serialized Processor visit,
   `tm_sh_last_stamp` copies the exact packet sequence/PTS/DTS, source,
   display and geometry generations and decoded byte count. The caller
   associates that metadata with the callback's real `tm_fs_frame_ref_v1`;
   the sidecar is not a Pin/Lease or a persistent/wire reference. Async
   Vision/GraphBridge requests still reserve a FramePin before queuing.
6. Explicit EOS yields the Executor's genuine EOS after prior output.
   On cancel the host separately stops and drains Executor, Fanout,
   Store pins, callbacks and any DSO scopes before releasing the Codec.
   No implicit replay, silent fallback or preemptive resource freeing.

## Actual product test (not fake Codec)

Existing `tests/test_codec.c` now generates an I420 test image,
encodes it using the **actual TurboMedia OpenH264 encoder**, separates
the resulting Annex-B SPS/PPS/IDR NAL units, and decodes through the
new production Source Provider. It compiles a real CPU VideoFrame
Source→Processor→record+preview plan, drives `tm_ex_pump_source` and
`tm_ex_pump_sink` and checks exact session/display provenance,
frame length, both actual sink visits and terminal EOS/drain.
The existing H264 decoder now bounds signed dimensions, stride,
plane pointers and even I420 shape before copying decoded pixels.

**Do not claim the new test passed merely because standalone Core/FFmpeg
CI was green.** It links the optional Codec component in the **root**
TurboMedia product graph. The historical M16 locked SDK restore attempts
to verify obsolete `qigao/salts` rc.15 tag provenance and currently fails
before CMake configure on the full jobs. The root test must actually run
after a separately authorized immutable SDK/provenance correction.

## Independent remaining gates

- AVC length-prefixed MediaCodec NAL conversion, dynamic SPS/PPS,
  B frames, reordered DTS, changing geometry/rotation and hardware
  codec capability negotiation are unsupported and must fail closed.
- This path provides CPU I420 with an explicit COPY; not zero-copy or
  general multi-kind Graph execution or GPU FrameLease/fences.
- Remote Android/PC transport is separate and may not send FrameRef
  as a substitute for compressed access units.
- M17d still needs Vision Provider, typed Observation, GraphBridge
  Pin-before-commit and real delayed cancellation. No Graph node can
  execute Android input: TurboInteract C11 ActionOwner stays authoritative.
- No installer/consumer test project, probe CMake/CI, real-device or
  emulator CI, duplicate Codec or media scheduling engine was created.
