# WebRTC Security Control Plane

## Status

This document defines the public multi-tenant security boundary tracked by #42.

The first implemented slice is the **dynamic signed-token revocation view**. It provides a bounded,
versioned in-process state machine and integrates it with the existing signed-token verifier. The remaining
edge identity, trusted-proxy, tenant-wide quota, and Internet abuse-capacity work is still open under #42.
This slice alone does not make TurboMedia public multi-tenant ready.

## Trust boundaries

### Shared security control plane

The shared control plane is the source of truth for:

- revocation epoch and monotonically ordered sequence;
- complete revocation snapshots;
- future tenant-wide quota leases / reservations;
- future trusted edge identity policy versions.

Application nodes must not independently invent a newer control-plane version or silently continue from a
known sequence gap.

The signaling node now exposes one authenticated propagation transport for this projection over its
existing HTTPS management listener. Revocation writes require a short-lived signed bearer with audience
`turbomedia-security-control` and scope `security.revocation.write`. The static management bearer
compatibility token is never accepted on these routes. The HTTPS listener certificate is still required,
so control tokens are not sent over plaintext HTTP.

This slice authenticates the control-plane publisher at the application layer. Deployment network ACLs
remain required, and mutual TLS may be added as an additional edge identity layer. SFU-node consumption
and multi-node fan-out/retry are not implemented by this slice.

### Edge

The edge terminates Internet-facing transport and performs handshake admission before expensive application
work. Trusted proxy/client identity is **not implemented by this slice**.

Future proxy-derived client identity may be accepted only when all of the following are true:

1. the direct peer is authenticated as an approved proxy;
2. network ACLs restrict the proxy path;
3. the proxy is present in an explicit allowlist;
4. the forwarded identity/header format is versioned and bounded.

Until then, forwarded identity headers are not a trusted fact source.

### Application process

The application process verifies:

- signed-token issuer, key id/signature, audience, scope, expiry, room and participant binding;
- static fingerprint revocation where legacy configuration still uses it;
- the dynamic revocation view when configured.

When dynamic revocation is configured, the legacy static bearer-token compatibility path is disabled so it
cannot bypass the shared revocation control plane.

## Dynamic revocation contract

### State

Each node owns one bounded local projection of the shared revocation state:

- `epoch`: control-plane generation;
- `sequence`: last complete event sequence in that epoch;
- bounded SHA-256 token fingerprint set;
- synchronization state.

A newly created process starts **UNSYNCHRONIZED**. Restart does not restore trust from stale local memory.
A complete snapshot is required before dynamic signed-token authorization can succeed.

### Snapshots

A snapshot is a complete replacement of the revocation set and contains:

- non-zero epoch;
- sequence;
- bounded list of unique lowercase SHA-256 token fingerprints.

A snapshot may restore an UNSYNCHRONIZED node only when its version covers the highest missing version
observed by that node. Older snapshots are stale no-ops.

### Revoke events

A revoke event contains exactly one token fingerprint plus epoch/sequence.

An event is applied only when:

- epoch exactly matches the synchronized epoch; and
- sequence is exactly current sequence + 1.

Older/duplicate events are stale no-ops.

A sequence gap or epoch jump immediately marks the node UNSYNCHRONIZED. The triggering event is not partially
applied. A complete covering snapshot is required for recovery.

If the bounded local set cannot admit an event, the node also becomes UNSYNCHRONIZED instead of dropping an
old revocation or silently continuing with incomplete state.

### Signaling propagation transport

Dynamic peer revocation is enabled with a bounded `dynamic_revocation_capacity`. Enabling it requires
peer JWT admission plus an HTTPS management listener with valid signed management-token configuration.
Startup fails when that recovery channel is absent.

A newly started signaling process creates an empty UNSYNCHRONIZED projection. Peer JWT authorization is
therefore denied until a covering snapshot arrives.

The signaling management listener accepts two bounded v1 JSON messages:

- `POST /api/v1/security/revocations/snapshot` with `schema_version`, non-zero `epoch`,
  `sequence`, and `revoked_sha256` as a comma-separated set of lowercase SHA-256 digests;
- `POST /api/v1/security/revocations/revoke` with `schema_version`, non-zero `epoch`,
  non-zero `sequence`, and one lowercase `sha256` digest.

Epoch and sequence are limited to exact JSON uint32 values. Snapshots are capped by
`TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS`; request bodies are independently bounded. Responses expose only
result, synchronization state, version, and count. They never echo token fingerprints or bearer material.

Stale writes are idempotent. Sequence gaps, capacity failures, and admissible newer malformed updates
leave the projection UNSYNCHRONIZED and therefore fail peer admission closed until a covering snapshot.

### Authorization

The signed-token verifier computes the compact-token SHA-256 and invokes the configured dynamic revocation
check after signature, claim, resource, and expiry validation.

| Revocation state | Authorization |
| --- | --- |
| CLEAR | continue |
| REVOKED | deny |
| UNKNOWN / UNSYNCHRONIZED | deny |
| malformed callback result | deny |

Dynamic revocation can only make authorization stricter. A static revocation-list match still denies even
when the dynamic view returns CLEAR.

## Failure and recovery rules

- node restart -> UNKNOWN -> deny signed tokens until snapshot;
- missing event -> UNKNOWN -> deny until covering snapshot;
- epoch jump event -> UNKNOWN -> deny until new-epoch snapshot;
- local capacity exhaustion -> UNKNOWN -> deny until bounded covering snapshot;
- admissible newer snapshot/event cannot be parsed or installed -> UNKNOWN -> deny until covering snapshot;
- stale event/snapshot, including malformed stale payloads -> no state regression;
- active/previous signing-key rotation remains independent from revocation ordering;
- token expiry is still enforced even when the revocation view is CLEAR.

No local permissive fallback is allowed from UNKNOWN.

## Bounds and data handling

The local dynamic revocation set is explicitly bounded by
`TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS`; callers may choose a smaller cap.

Only SHA-256 token fingerprints are stored. Compact bearer tokens, signing secrets, TURN credentials, and
other credential material are not stored in the revocation state and must not appear in metrics/audit logs.

## Remaining #42 work

The following are intentionally not claimed complete by the dynamic-revocation slice:

- authenticated fan-out/retry of revocation updates across all signaling and SFU nodes;
- SFU-node integration with the shared revocation projection;
- edge admission before TLS/WebSocket application allocation;
- trusted-proxy allowlist and spoofed-header negative tests;
- tenant/subject/IP/connection/request/media-resource quota model;
- deterministic multi-node tenant-wide quota enforcement;
- security metrics/alerts/audit integration;
- public handshake-flood / abuse / attack-capacity evidence.

Those capabilities must share the same explicit identity/control-plane fact sources. They must not introduce
independent local counters or a static-token/local fallback that can bypass distributed policy.
