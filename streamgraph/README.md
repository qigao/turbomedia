# TurboMedia::StreamGraph — typed graph compilation M0

**Development slice** [#163](https://github.com/qigao/turbomedia/issues/163), under [StreamGraph v2 #162](https://github.com/qigao/turbomedia/issues/162). This is **compile-only immutable graph preflight**. It is **not** an executable media graph.

The platform-neutral C11 header `turbo_streamgraph.h` models Source/Processor/Sink, six typed Port kinds and explicit edges. `tm_sg_compile()` deep-copies all IDs, Port descriptors and Edges, checks capacities, port direction, DAG cycle, schema, timebase, memory domain, media format, connectedness and fanout, and produces a read-only topology plan. Views remain borrowed until `tm_sg_program_destroy()`.

**Not implemented:** provider selection and exact capabilities, codec/model loading, FrameStore/FrameLease (#164), GPU synchronization, stream execution and QoS (#165), Observation serialization (#167), FFmpeg adapters (#166) or Android Capture. `format_id == 0` and memory `ANY` are unconstrained *proposals* requiring future explicit provider negotiation, not implicit conversion or a production capability claim.

`TurboMedia::StreamGraph` is a shared platform component, unlike desktop-only Pipeline. The small standalone CMake build supports fast conformance independent of a complete Salts/FFmpeg toolchain; its **test-only** installed `TurboMediaStreamGraph` CMake package does not prove the full TurboMedia package is usable.

```bash
cd streamgraph
cmake --preset native-ci
cmake --build --preset native-ci
ctest --preset native-ci
```

The existing Pipeline v1 is unchanged; v2 execution migration will be tracked separately and cannot silently fall back to the legacy scheduler.

## M1: CPU FrameStore (new constrained slice)

The companion `turbo_frame_store.h` declares an explicitly **owner-serialized,
same-process CPU-only** frame store with upfront bounded allocation and
generation-fenced `FrameRef` lookup. The initial `tm_fs_publish_cpu()` takes
a **copy** of the bytes into owned storage; do **not** advertise end-to-end
zero-copy or GPU surfaces. A `FrameRef` is a value identity, not a retention
claim. The Store issues one independently charged `tm_fs_pin_v1` per admitted
consumer; acquiring a `tm_fs_lease_v1` temporarily retains the slot while
`tm_fs_map_cpu()` exposes a borrowed read-only view.

A slot cannot be reused until it is explicitly retired and all pins/leases
settle. Duplicate pin/lease release is stale. The process-local Store
generation changes upon recreation; stale FrameRefs are rejected after Store
and slot reuse. `tm_fs_seal()` stops new publication/pin admission but
allows previously pinned work to settle; `destroy` returns BUSY for
outstanding credits. All operations must be serialized by the embedding
Media Owner; this does not create a second executor.

**Not provided:** GPU FrameLease, external zero-copy buffers, asynchronous
cross-thread/MPSC credit transfer, IPC broker, GPU fences, model inference,
cross-Graph scheduler or source adapters. A C11 local atomic is used only to
generate process-local Store incarnations. Frame identities are never persisted.
[FrameStore issue #164](https://github.com/qigao/turbomedia/issues/164)
remains open until full domain qualification.
