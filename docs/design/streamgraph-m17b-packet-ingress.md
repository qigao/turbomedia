# M17b: Source packet lineage and bounded ingress

This is the first real TurboMedia StreamGraph H264 Source admission component,
tracked in M17 issue 275, stacked after M17a PR 276. It does not decode
video, run a heterogeneous Graph or implement an Android transport.

The new public turbo_streamgraph_screen_packet.h belongs to the existing
TurboMedia::StreamGraph product target. It requires all packet metadata to
match one immutable device session, display ID/incarnation/geometry generation,
128-bit capture ID, stream epoch and even source dimensions.

One caller-owned Media Owner preallocates a bounded H264 Annex-B packet slot,
then uses offer -> take -> settle. The owner copies bytes on offer and never
borrows an Android/transport buffer after returning. Full/STALE/error does
not consume the input. An already TAKEN packet cannot be overwritten or
destroyed. Stop/seal denies ingress, while cancellation preserves TAKEN
buffer lifetime until explicit settlement. Sequence must increase exactly
by one. PTS and DTS are monotonic microseconds with PTS == DTS: frames
requiring decode/presentation reordering are rejected in this qualified
initial profile instead of silently re-timed.

CONFIG may contain SPS/PPS NALs, followed by an IDR with prior completed
SPS and PPS, then non-IDR access units and EOS. This verifies Annex-B
framing and NAL types, not full SPS/PPS/slice syntax. An AVC length-prefixed
packet requires explicit conversion and a full Codec semantic check before
runtime decode. Midstream codec reconfiguration requires a new stream epoch
and owner; no implicit fallback or hidden reset.

The same existing streamgraph_contract native test now exercises the actual
production code's copy/borrow/settle, stale geometry, packet gap, out-of-order
PTS/DTS, missing SPS/PPS, malformed NAL, cancellation and BUSY destroy.
No second scheduler, CMake target, new workflow, installer/consumer project
or device/emulator CI was added.

Next M17c: route a qualified copied packet to the existing TurboMedia::Codec
H264/OpenH264 decoder under one Media Owner, then publish a CPU frame into
the existing FrameStore using exact Pin/Lease and backpressure. The decoded
frame still needs an additive sidecar holding the source packet provenance,
pixel format/stride/timebase and a matching process-local FrameRef.

The ingress header is an IN-PROCESS ABI, not a wire protocol; neither packet
sequence nor capture UUID establishes an authenticated remote source.
