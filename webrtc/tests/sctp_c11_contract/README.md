# #227: isolated C11 SCTP Adapter contract (test-only)

This is a **compile/test contract**, not a second DataChannel backend, an
implementation of Salts #1181, or a released API. `sctp_c11_contract.h` is
included **only** by this test target, is not installed/exported, and
does not include `usrsctp.h`, OpenSSL, libwebrtc or a C++/Rust protocol engine.
The normal `TurboMedia::DataChannel` continues to use exactly its previous
usrsctp/DTLS source and installed target.

## Boundary

- The proposed `tm_sc_owner_ops_v1` deliberately has **no implementation**.
  Its C11 typedefs record the minimum future Salts CNetSctp shape: one Owner,
  explicit monotonic clock and deadline, borrowed authenticated SCTP plaintext
  input, bounded owned admission of outbound user bytes, stream reset,
  per-SID pending bytes, and terminal stop/drained/destroy.
- Actual pure-C11 helpers build a **borrowed request envelope only**, mapping
  existing `turbo_dc_channel_config_t` reliable/unordered/RTX/TTL semantics
  and PPIDs 50/51/53/56/57; there is no SCTP packet generation or I/O.
- For empty user messages, the proposed SCTP wire request passes one static
  zero octet with PPID 56/57 while `app_bytes == 0`. The existing public
  `turbo_dc_channel_send` still rejects a NULL data pointer for zero length.
  Actual production empty-message behavior remains unchanged until cutover.
- An incoming event view is borrowed and must not survive Owner callback
  return without a charged copy/lease. Empty PPID 56/57 accepts exactly the
  zero octet and publishes a zero-length application view.
- Capacity is caller-supplied and checked **before** request publication;
  these pure helpers allocate no buffers and maintain no second queue.
- DCEP frame parsing and label/protocol validation stay in the existing
  TurboMedia DCEP module. The private helper checks only OPEN/ACK framing
  minimum and guarantees reliable ordered PPID 50 for the future engine.

## Tests and admission

`turbo_media_test_sctp_c11_contract` is registered through the existing
`webrtc/tests/CMakeLists.txt` TinyTest helper and exercised by the normal
four-platform `Platform Build` matrix. It does **not** link
`TurboMedia::DataChannel`, `unofficial::usrsctp::usrsctp`, a new SCTP
engine, or a test/mock engine implementation. Android compiles this test but
does not execute CTest on the cross-compiled target.

Required follow-up for #227 Task 1: reconcile this **proposal** with the
upstream published C11 session API once it exists, add a compile-only
installed-SDK consumer and negative ABI checks. Salts Draft #1182 currently
exports only `CNetSctpPrototype::Wire`; no Association Session API is
installed. Do not claim independent SCTP wire or browser interoperability.

Additional, separate gates before a cutover:

- [#230](https://github.com/qigao/turbomedia/issues/230) — real per-channel
  `buffered_amount` queue accounting is currently absent in production.
  This contract proposes the pending-bytes query but does not fake it.
- [#195](https://github.com/qigao/turbomedia/issues/195) — independently
  admitted channel callback lease versus reentrant channel close.
- DCEP receive OPEN currently does not inspect send-ACK failure before
  notifying the application; a separate deterministic failure gate is needed.
- [#201](https://github.com/qigao/turbomedia/issues/201),
  [#208](https://github.com/qigao/turbomedia/issues/208) and
  [#222](https://github.com/qigao/turbomedia/issues/222) — usrsctp-specific
  Timer/Socket lifecycle and native lock-order diagnostic.
- Salts #1181 M2–M6, independent real DTLS/browser peer, native Owner
  Generation/Drain, sanitizer and platform installed-SDK gates remain open.

**Draft / DO NOT MERGE.** No production behavior change or release authority.
