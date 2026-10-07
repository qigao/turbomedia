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

This transport authenticates the control-plane publisher at the application layer. Deployment network
ACLs remain required, and mutual TLS may be added as an additional edge identity layer. Signaling and SFU
nodes consume the same thread-safe revocation projection abstraction and the same v1 snapshot/revoke wire
parser. Multi-node fan-out/retry is not implemented by this slice.

### Edge

The edge terminates Internet-facing transport and performs handshake admission before expensive application
work. Trusted proxy/client identity is **not implemented by this slice**.

Future proxy-derived client identity may be accepted only when all of the following are true:

1. the direct peer is authenticated as an approved proxy;
2. network ACLs restrict the proxy path;
3. the proxy is present in an explicit allowlist;
4. the forwarded identity/header format is versioned and bounded.

Until then, forwarded identity headers are not a trusted fact source.

### Tenant identity fact source

Tenant-wide quota must not infer tenant identity independently in signaling, SFU, and Room Service. The
shared signed-token contract therefore defines one optional `tenant_id` resource claim.

Tenant binding follows the same exact-match rule as room/participant binding:

- policy and token both omit `tenant_id`, or both contain the exact same identifier;
- a tenant-bound token is never accepted by an unbound policy;
- `tenant_id` uses the bounded identifier alphabet but cannot contain `/`;
- when both tenant and room are present, `room_id` must be in the existing
  `<tenant_id>/...` namespace and must contain a non-empty suffix.

This keeps the existing unbound token contract valid while preventing a future quota layer from treating
`tenant-a` and `tenant-b/room` as one identity. The quota layer is not implemented by this slice; it
must consume this signed tenant fact instead of reconstructing a second tenant namespace locally.

### Tenant quota lease projection

The shared security control plane is the only source of tenant-wide quota budget. Application nodes do not
independently compute a tenant-wide count and do not infer spare global capacity from local usage.

The quota projection is a bounded, caller-serialized local view of leases assigned to one node. A lease
binds:

- the signed `tenant_id` fact;
- the exact target `node_id`;
- control-plane `epoch/sequence` ordering;
- an absolute `expires_at_unix_ms`;
- bounded limits for signaling connections, rooms, participants, media sessions, and published tracks.

The control plane must maintain the invariant that the sum of simultaneously valid per-node leases for a
tenant/resource does not exceed that tenant's global policy. Nodes consume only their own lease; they do
not reconstruct or reconcile the global sum themselves.

A newly created projection is UNSYNCHRONIZED and rejects new reservations until a complete covering
snapshot is applied. Lease updates are exact-next in the current epoch. Sequence gaps, epoch jumps,
capacity exhaustion, and unusable admissible updates make the projection UNSYNCHRONIZED and therefore
fail new reservations closed until a covering snapshot arrives.

Lease expiry uses Unix epoch milliseconds and rejects new reservations at or after expiry. Tightening a
lease below current live usage never destroys or silently forgets owned resources: new reservations are
denied until usage drains below the new limit. Removing a tenant from a covering snapshot removes its
lease immediately, but a bounded no-lease tombstone retains only the live local usage counts needed to
permit deterministic release. Release is allowed while UNSYNCHRONIZED, expired, or lease-less so cleanup
can always drain ownership.

This projection is not the distributed quota issuer and is not yet wired into signaling/SFU/Room Service
resource admission by this slice. Those runtime integrations must consume the same signed tenant identity
and the same control-plane lease stream; they must not introduce a second tenant namespace, a local
"global" counter, or a permissive fallback when lease state is UNKNOWN.

### Tenant-wide quota allocator

The shared control plane now has a bounded allocator contract that owns tenant global caps and the
currently issued per-node lease limits. It is the only component allowed to decide whether another node
lease may be issued.

The allocator starts UNSYNCHRONIZED and is restored from one complete snapshot containing both tenant
policies and all currently live issued leases. A snapshot is accepted only when every lease references a
known tenant policy, targets a valid node identity, is unexpired at the supplied Unix-ms time, is unique by
tenant/node, and the aggregate issued limits for every tenant/resource are less than or equal to the
tenant-wide policy.

Canonical grant, replacement, revoke, and policy-change proposals are exact-next in the allocator's
epoch/sequence. A sequence gap or epoch jump means the control-plane mutation stream is incomplete and
moves the allocator to UNKNOWN until a covering policy+lease snapshot arrives. A newer covering snapshot
that cannot be validated or installed also moves the allocator to UNKNOWN; continuing to issue from an
older global budget would be unsafe. In contrast, an overcommitted grant or policy-tightening proposal is
rejected before becoming a canonical mutation, does not consume the next sequence, and does not poison a
healthy allocator.

Grant replacement subtracts the previous lease for the same tenant/node before checking the new limits.
Expired leases are pruned before commitment checks, so their budget can be safely reissued. A tighter
tenant policy is rejected while currently live issued leases exceed the proposed cap; the control plane
must first shrink/revoke/expire those leases.

The allocator can materialize a complete node-specific lease snapshot at its current canonical version.
Those snapshots are the only lease facts that the multi-node publisher may send to node projections. The
allocator does not store runtime usage and nodes do not report local usage as an alternative global
counter. The tenant-wide invariant is purely that the sum of all simultaneously live issued lease limits
never exceeds the global tenant policy.

The authenticated node ingress below is already implemented. Allocator-to-target publication and runtime
resource reserve/release wiring remain separate slices.

### Authenticated tenant quota lease transport

Signaling and SFU nodes now expose the same bounded HTTPS ingress for their local tenant quota projection:

- `POST /api/v1/security/tenant-quotas/snapshot`
- `POST /api/v1/security/tenant-quotas/update`

Both routes use audience `turbomedia-security-control` and the dedicated scope
`security.tenant_quota.write`. Static admin/control/media compatibility bearers are never accepted on
these routes. Quota bootstrap authorization intentionally does not consult the target quota projection,
so an UNKNOWN node can receive the covering snapshot required to recover.

The v1 wire contract is strict JSON. Snapshot and update messages bind an explicit target `node_id`,
control-plane epoch/sequence, and bounded lease data. Lease limits cover signaling connections, rooms,
participants, media sessions, and published tracks. Epoch, sequence, expiry, and limit values are parsed
from raw decimal JSON number text; fractional, negative, or overflowing values are rejected rather than
rounded through binary floating point.

Each process owns exactly one caller-serialized quota projection protected by a process-local mutex.
The signaling application enables that projection with its canonical signaling node id; the SFU enables
it with its canonical SFU node id. A message for a different node is rejected before projection mutation,
including an empty snapshot where no lease entry exists to reveal the target.

Quota mode also closes legacy identity bypasses before runtime reservation is added. A signaling process
cannot enable quota projection without signed peer JWT admission. An SFU quota process cannot retain
static control or media bearer compatibility tokens; it must use signed control/media identities. This
ensures the next reservation slice can consume the signed `tenant_id` fact on every quota-controlled
resource path instead of inventing an unauthenticated/default tenant.

Transport structure and quota semantics remain deliberately separated. Structurally valid exact-next input
is passed to the quota projection. A semantically invalid exact-next lease therefore makes the projection
UNSYNCHRONIZED exactly like any other unusable control-plane update; a covering snapshot is then required.

This slice is a distribution **sink**, not the global allocator/issuer. The shared control plane still owns
the tenant-wide budget invariant and canonical lease stream. Runtime resource admission/release has not yet
been wired to the projection by this slice.

### Application process

The application process verifies:

- signed-token issuer, key id/signature, audience, scope, expiry, tenant, room and participant binding;
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

### SFU projection integration

When `auth_dynamic_revocation_capacity` is non-zero, the SFU node owns one shared projection used by both
control-plane and WHIP/WHEP media authorization. The legacy static `control_token` and
`media_access_token` cannot bypass that projection because dynamic callback mode disables static bearer
authorization.

SFU dynamic mode requires TLS plus valid signed auth configuration. The node starts UNSYNCHRONIZED, so
signed control/media requests fail closed until a covering snapshot is applied.

SFU exposes the same HTTPS paths, schema version, audience, and scope as signaling:
`/api/v1/security/revocations/snapshot` and `/api/v1/security/revocations/revoke`.
The security-control token is intentionally verified without consulting the target projection; otherwise an
UNSYNCHRONIZED node could never bootstrap or recover. Static control/media compatibility tokens are never
accepted on these security-control routes.

### Multi-node fan-out and recovery

The distributor is a bounded, caller-serialized coordinator over canonical updates produced by the shared
security control plane. It is **not** a second revocation source: it stores only target identifiers, opaque
transport handles, per-target acknowledgement versions, and delivery status. Bearer tokens, credentials,
and revocation fingerprint sets remain caller-owned and are only borrowed for a send operation.

Targets are bounded to 64 and retry attempts to 8. Target identifiers use a restricted non-secret label
alphabet so diagnostics cannot accidentally become credential carriers.

A covering snapshot establishes the distributor's canonical cursor. Revoke events must be exact-next in
the same epoch. Rollback, sequence gaps from the producer, malformed SHA-256 payloads, duplicate snapshot
entries, and over-capacity payloads are rejected before any transport side effect.

For each target:

1. if the target is synchronized at the prior canonical version, send the exact-next revoke event;
2. retry only transport-level RETRYABLE failures, up to the configured bound;
3. if the node reports GAP/LIMIT/ERROR/FATAL, returns an invalid stale acknowledgement, or exhausts event
   retries, send the caller-supplied covering snapshot for the same canonical version;
4. a target is synchronized only when an APPLIED/STALE response explicitly reports a synchronized version
   that covers the requested version;
5. one failed target does not stop attempts to the remaining targets.

A target that remains failed is skipped for the next incremental event and is reconciled directly with that
event's covering snapshot. This avoids repeatedly sending events onto a known-incomplete projection.

The transport callback is intentionally injected. Production adapters must use the existing HTTPS
security-control endpoints with short-lived `turbomedia-security-control` /
`security.revocation.write` credentials. The fan-out core never stores those credentials.

### Production SFU target and token wiring

Room Service is the source of truth for the SFU membership it already uses for room placement and
control routing. Revocation fan-out does not maintain a second SFU URL registry.

When `sfu.revocation_server_names` is configured, each existing `sfu.nodes` member must have exactly
one explicit TLS server-name mapping. The target URL remains the existing membership URL, the CA remains
the existing `sfu.ca_file`, and the target set is bounded by the 64-node fan-out limit. Plain HTTP,
non-root target URLs, missing CA material, missing mappings, duplicate mappings, or membership that
exceeds the bound fail closed.

Room Service uses the existing `[sfu_auth]` issuer/key/secret as the security-control signer. For every
delivery attempt it creates a short-lived bearer with audience `turbomedia-security-control` and scope
`security.revocation.write`; the HTTPS adapter borrows that bearer for one attempt and never retains it.
Room Service zeroes and frees the transient bearer after the publish operation. The configured token TTL
must exceed one fan-out request deadline.

SFU membership has a local monotonic version. A node add or target URL/TLS-name change invalidates the
current adapter/coordinator. The next publish rebuilds from the same Room Service membership. If the next
canonical update is a revoke event, Room Service sends the caller-provided covering snapshot at that
version to the rebuilt target set instead of sending an incremental event to a newly UNKNOWN node.

This wiring does not make Room Service a second revocation source: canonical epoch/sequence and covering
snapshot payloads remain caller-supplied facts from the shared security control plane.

### Signaling source identity and trusted proxies

The default signaling source identity is the TCP socket peer IP. Forwarded headers are ignored in this
mode, so an Internet client cannot change the source key used by connection/rate admission.

Trusted-proxy mode is explicit and all-or-nothing. It requires WSS, an mTLS client CA, enabled source
admission limits, and a bounded map of at most 32 `numeric-ip=leaf-sha256` proxy identities. The WSS
listener then requires client certificates for every connection. A request is considered proxied only when
both the actual socket peer IP and the verified TLS client leaf SHA-256 match the same configured entry.

Only one `X-Forwarded-For` header is accepted. Its value must be one bare numeric IPv4 or IPv6 literal.
Repeated headers, comma-separated chains, whitespace, hostnames, ports, bracketed IPv6, and zone identifiers
are rejected. Other forwarding headers are not identity inputs. IPv4-mapped IPv6 is normalized to IPv4.

The CHTTP admission hook resolves the source identity and consumes the bounded source admission/rate budget
before WebSocket application allocation. Active-connection accounting is bound during WebSocket open and
released on peer teardown. An untrusted proxy is rejected with 403; malformed source/forwarded identity is
rejected with 400; rate/concurrency rejection is 429 with a bounded Retry-After.

This application-layer proxy allowlist complements deployment network ACLs. The certificate pin is derived
from CHTTP's already verified TLS client certificate; no unverified certificate/header value is trusted.

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

- signaling-node production membership/source wiring into the shared fan-out path, if required by the final control-plane owner;
- shared-control-plane producer integration for canonical revocation publication;
- TLS-connection-level admission before handshake CPU allocation (source HTTP/WebSocket admission is now pre-application);
- trusted-proxy allowlist and spoofed-header negative tests;
- authenticated multi-node lease publication from the shared allocator using the node ingress above;
- runtime reservation/release wiring for signaling connections, rooms, participants, media sessions, and published tracks;
- subject/IP/request/byte/time quota dimensions beyond the initial tenant resource lease model;
- deterministic allocator-to-node multi-node quota evidence beyond the exact global over-issue contract test;
- security metrics/alerts/audit integration;
- public handshake-flood / abuse / attack-capacity evidence.

Those capabilities must share the same explicit identity/control-plane fact sources. They must not introduce
independent local counters or a static-token/local fallback that can bypass distributed policy.
