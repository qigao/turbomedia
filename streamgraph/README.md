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
