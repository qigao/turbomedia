# WebRTC Production Readiness

## Status

**Development preview — not production-ready.**

This document records verified repository capabilities and remaining release
gates. A local test pass proves only the paths exercised by that test; it does
not prove public-network, browser, capacity, security or recovery readiness.

## Verified implementation

- PeerConnection, SDP, RTP/RTCP, DTLS-SRTP and DataChannel are implemented in
  the TurboMedia source tree.
- Candidate gathering, connectivity checks and selected-pair I/O use
  `TurboNet::ICE`.
- TurboNet::ICE performs RFC 7675 consent freshness checks, rejects responses
  from the wrong transport tuple or transaction generation, expires consent
  after 30 seconds, and exposes a versioned ICE restart operation.
- PeerConnection and the SFU WHIP/WHEP resource path rotate ICE credentials,
  exchange trickle-ICE SDP fragments under a strong ETag, and keep DTLS/SRTP
  state owned by the PeerConnection during restart.
- The SFU exposes the client-offer resource lifecycle for WHIP (RFC 9725) and
  WHEP draft-04: authenticated SDP `POST`, `201 Created`, `Location`,
  conditional `PATCH`, and `DELETE`.
- SFU STUN/TURN servers are externally configurable. Loopback candidates are
  disabled by default and can only be enabled explicitly.
- The signaling management HTTP API is disabled by default. When enabled it
  requires either scoped signed-token verification or the explicit
  `TURBO_SIGNALING_ADMIN_TOKEN` compatibility path.
- The SFU control and media routes can instead validate strict HS256 access
  tokens. The profile fixes `typ=turbomedia-auth+jwt` and `alg=HS256`, requires
  issuer, subject, audience, scope, issue and expiry claims, enforces exact
  room/participant bindings, limits token lifetime, and accepts only the
  configured active or previous `kid`. Negative tests cover cross-room,
  cross-participant, wrong-scope, expired, tampered and rotation-overlap cases.
- Every signed-token verifier can additionally reject an exact compact-token
  SHA-256 fingerprint from a startup-validated, process-local list of at most
  256 entries. Tests cover exact rejection, later-list matches, malformed
  syntax and the cardinality boundary.
- Room Service validates the same strict profile for mutating command and
  facade routes with `room.control.write` or `room.control.dangerous` and exact
  room/participant binding. It accepts a bounded active/previous verification
  pair during rotation.
- Room Service uses a separate signing key to mint a fresh, short-lived,
  resource-bound `turbomedia-sfu-control` token for each outbound SFU command.
  Signed mode deterministically takes precedence over the explicitly
  configured static compatibility token.
- Signaling management routes validate audience
  `turbomedia-signaling-management`, route-specific
  `signaling.management.read`, `.write`, or `.dangerous` scopes, exact
  room/peer bindings, expiry, and active/previous-key rotation.
- Signaling WebSocket peer admission can require a browser-compatible token in
  the first `join` message. It validates audience
  `turbomedia-signaling-peer`, scope `signaling.peer.join`, exact room/peer
  bindings, expiry, and the configured active/previous key pair before binding
  the application peer ID.
- Signaling applies a fixed first-join deadline, CoroNet complete-message size
  enforcement, a per-connection token bucket, and message/byte bounds on each
  copied peer outbox. The status API exposes cumulative rejection counters for
  authentication, join timeout, message rate, and outbox overflow.
- After WebSocket upgrade, signaling normalizes the direct socket peer's
  IPv4/IPv6 address and applies process-local concurrent-connection and token
  bucket admission bounds. Inactive rate state has a TTL and the source-state
  table has a hard cardinality limit.
- CoroNet terminates signaling WSS with an explicit PEM certificate/key.
  Iris terminates HTTPS for the signaling management API, SFU, and Room
  Service. TLS startup is fail-fast when the configured server identity is
  absent or invalid.
- Room Service configures TurboHTTP `http_client` with mandatory peer and
  hostname verification. It uses the system trust store by default or an
  explicitly configured private CA bundle for internal SFU HTTPS.
- The dependency manifest and overlay select BoringSSL; the
  `OpenSSL::SSL` and `OpenSSL::Crypto` names in CMake are compatibility target
  names exported by the package.
- Remote SDP must provide one valid SHA-256 DTLS fingerprint for every active
  media section. A bundled transport must use consistent ICE credentials and
  fingerprint values across those sections.
- Local loopback tests cover ICE, DTLS, SCTP/DataChannel, SRTP, SDP and the
  higher-level WebRTC session workflow, including a successful conditional ICE
  restart through the WHIP HTTP resource.

## Release blockers

### HIGH — Internet-facing identity and abuse controls

The SFU, Room Service, signaling management API, and WebSocket peer admission
now share a scoped, time-bounded token contract with room/participant negative
tests and bounded active/previous-key rotation. Room Service also mints
per-command SFU tokens using a separate signing trust domain. Signaling has
local per-connection message/memory bounds and post-upgrade direct-source
admission limits. Exact token fingerprints can now be revoked through bounded,
startup-validated process-local lists. There is still no dynamic/distributed
revocation propagation, pre-TLS/WebSocket edge admission policy, trusted-proxy
identity integration, tenant-wide quota, distributed enforcement, or
Internet-facing abuse-control evidence.
Application TLS protects credentials in transit but does not close those
remaining abuse-control gaps.

### HIGH — public-network acceptance evidence

STUN/TURN configuration is now wired into SFU PeerConnections, but the
repository still has no reproducible acceptance result for TURN-only,
restrictive NAT, IPv6, browser network migration, or relay credential expiry.
Configuration is not evidence that these paths interoperate.

### MED — WHEP protocol breadth

The server implements the WHEP draft-04 client-offer flow. It does not implement
the optional server-offer/counter-offer mechanism or extension negotiation.
Clients requiring those paths are not yet supported. The draft revision must
remain pinned in release notes and interoperability tests.

### MED — interoperability and operations evidence

The repository does not contain a current, reproducible acceptance report for:

- Chrome, Firefox, Edge and Safari release matrices;
- TURN-only IPv4/IPv6 and restrictive NAT paths;
- loss, delay, jitter, reordering, MTU and network-transition scenarios;
- connection capacity, sustained throughput and backpressure;
- multi-hour soak, leak and shutdown testing;
- multi-node failover, rolling deployment and state reconciliation;
- production metrics, alert thresholds and incident runbooks.

## Release decision

The module is suitable for continued development and controlled TLS-protected
integration. It is not yet suitable as a directly Internet-facing,
multi-tenant WebRTC product until the HIGH identity/acceptance blockers are
closed and the MED acceptance matrix has recorded, repeatable results.
