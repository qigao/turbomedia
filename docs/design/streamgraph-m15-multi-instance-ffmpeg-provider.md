# M15 — Multi-instance FFmpeg CMeta Provider contract (design and implementation gates)

Parent: [#166](https://github.com/qigao/turbomedia/issues/166), [#165](https://github.com/qigao/turbomedia/issues/165), [#180](https://github.com/qigao/turbomedia/issues/180). Stack on Draft [#186](https://github.com/qigao/turbomedia/pull/186); **DO NOT MERGE**.

## Why M14 is insufficient

M14 `real_component_provider.c` uses DSO-static `fixture_decoder`, `fixture_call_active`, `in_get`, `permit_return`, and `fixture_last_wake`. It deliberately admits one request per Provider DSO, while the Service object represents the generation's fixed integer. Neither the singleton nor the test-only `run_decode/take_luma/finish_decode` methods constitute a user-facing instance factory. They cannot demonstrate two independent active Codec sessions in the same loaded DSO. Do not treat the deterministic pre-Codec cancellation gate as in-Codec cancellation coverage.

## Canonical ownership

- **Generation**: the one canonical Salts ComponentPlugin loader, published Generation, address-stable Scope, DSO lifetime. Scope remains alive through all instance calls, Portal terminal reconciliation, CNet wake callback completion, worker join, and decoder destruction. No supplementary registry or `dlopen`.
- **Provider factory**: stateless CMeta service resolved through `salts_component_plugin_scope_find_service`. A factory operation constructs a *distinct address-stable instance* with a bounded immutable configuration and the calling Generation identity. Never publish raw `AVCodecContext`, `AVFrame`, or `AVPacket` in the public ABI.
- **Instance**: private `tm_ffa_decoder*`, immutable instance ID and generation, explicit states `CREATED -> ACTIVE -> CLOSING -> QUIESCENT -> DESTROYED`. No DSO-static active decoder or mutable per-request state. Each Instance admits only one in-flight Codec call until the existing async decoder's actual concurrency contract is extended.
- **Media Owner**: owns NativeIO session, bounded Portal tickets, single authoritative terminal receipt, CNet wake Client, and the sole frame publication to StreamGraph. Other threads deliver bounded requests; no hidden scheduler or retry.
- **Borrowed data**: packet and wake pointers are borrowed only until the synchronous DSO entry/worker callback has returned. Completion never guarantees that borrowed buffers or Provider code are quiescent. Disallow moving/copying live Scope or Instance storage.

## Proposed additive C11 CMeta shape (NOT implemented yet)

```c
/* Illustrative ABI sketch; names and ABI version require actual SDK review. */
typedef struct tm_ff_provider_instance tm_ff_provider_instance;
typedef struct tm_ff_instance_config_v1 {
    size_t size;
    uint64_t provider_generation;
    uint64_t instance_id;
    uint32_t max_packet_bytes;
    uint32_t max_frame_bytes;
} tm_ff_instance_config_v1;

/* Factory via CMeta typed service; each successful create returns a unique
   address-stable instance, not a DSO global singleton. */
int create_instance(void *provider, const tm_ff_instance_config_v1 *,
                    tm_ff_provider_instance **out);
int run_instance(void *provider, tm_ff_provider_instance *,
                 const tm_test_decode_request * /* migrate to public ABI */);
int take_instance(void *provider, tm_ff_provider_instance *,
                  const tm_test_take_request * /* migrate to public ABI */);
int request_close(void *provider, tm_ff_provider_instance *);
int destroy_instance(void *provider, tm_ff_provider_instance *);
```

The final interface must use public non-test request types and explicit ABI size/version checks, with owner ticket identity validated against the instance's generation. The Provider instance is not itself a Salts Scope; only the host can release that Scope after instance quiescence.

## Safety and concurrency gates

1. **Two live instances, same DSO**: independent decoder pointers, packet buffers, result slots, wake receipts and correlation IDs; interleave their calls; no cross-instance overwrite or contamination.
2. **N/N+1 hot replacement**: two instances in N with one active cancellation, publish N+1 with its own instances; old Scope drain/unload remains BUSY until *every* N instance is quiescent and destroyed.
3. **Cancellation at the Codec boundary**: add an instrumentation hook in the first-party FFmpeg adapter immediately surrounding `avcodec_send_packet` (enter/leave). Race owner cancellation with a worker that has entered the real call. Never assume cancellation can safely interrupt FFmpeg while the function is executing: ownership stays retained, worker returns, then terminal delivery/reconciliation, flush and decoder teardown occur in order.
4. **Flush semantics**: implement/test explicit EOF/drain flush through the existing Codec API, observe all delayed frames and one terminal; cancel must discard non-accepted output and prevent an additional externally visible completion. Never conflate `EAGAIN`, EOF, cancellation and fatal Codec errors.
5. **NativeIO**: real RTSP input session first; RTC separately because signaling/transport and teardown semantics differ. Close underlying NativeIO admission, request cancellation, observe the async callback, and prove no stale generation frame reaches FrameStore.
6. **Negative cases**: invalid ABI/oversized config, capacity exhaustion, duplicate instance ID, wrong generation, stale ticket, concurrent destroy/run, repeated close, failed partial create, cancelled-but-never-quiescent and Plugin unload BUSY.
7. **Build/test**: exact installed Salts and installed optional desktop StreamGraphFFmpeg; dedicated Linux FFmpeg+ASan/UBSan/TSan CI and Windows/macOS FFmpeg-free public SDK contract. FFmpeg and released Salts system binaries may not be TSan-instrumented; report that limit.

## Acceptance order

- M15a: settle public instance/factory CMeta interface; implement two independent instances in one real DSO and negative ownership tests.
- M15b: introduce an optional adapter-local enter/leave Codec hook strictly for deterministic conformance; demonstrate cancellation while *inside* the Codec call without freeing active storage.
- M15c: delayed packet/frame flush, EOF and bounded receipt tests.
- M15d: real RTSP NativeIO cancellation and generation handoff; RTC only with its own observable conformance fixture.

**This file is a design specification, not evidence that M15a–d have been implemented or tested.** Keep #165/#166/#180 Open and all integration PRs Draft until evidence is available.
