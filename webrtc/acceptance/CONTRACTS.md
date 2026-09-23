# WebRTC Acceptance Contract Decisions

## Manifest v2

The acceptance manifest uses `schema_version: 2` for the controller contract introduced by #39.
This is a breaking contract revision: v1 is rejected and there is no compatibility or inference fallback.

Each topology owns one explicit, versioned `relay_contract`. The controller and browser adapter consume
that declared contract directly; topology names are labels only and must never imply IP family, candidate
protocol, relay protocol, or allowed remote candidate types.

Each scenario declares its workflow explicitly. Credential-expiry scenarios must also perform topology
transition and ICE recovery; an expiry-only PASS is not valid.

## Hook receipt v2

Topology hook receipts use `schema_version: 2`. A receipt carries the canonical SHA-256 of the manifest
topology's `relay_contract`; it does not carry an independently editable copy of that contract.

The provider boundary and controller both correlate topology id, action, generation, sequence, effective
status, and relay-contract hash. A mismatch is a harness error. There is no topology-name inference and
no fallback to an older receipt shape.

## Generation domains

Topology generation and browser ICE generation are separate domains. A case starts at topology generation
0 while the browser page's initial PeerConnection evidence is ICE generation 1. The first topology
transition advances topology generation to 1; the corresponding browser ICE restart must produce ICE
generation 2, new local and remote ICE hashes, a new selected relay pair, and stale-ETag rejection.

## Evidence ownership

The controller is the only case-state owner. Adapters return bounded observations and receipts; they do
not decide PASS, FAIL, INCOMPLETE, or ERROR. Callback payloads and external output are evidence only.

Track confirmation must precede viewer subscription. The confirmed track IDs are passed unchanged to the
subscription adapter, and the SFU adapter requires exactly one participant while confirming the initial
publisher tracks.

Credential-expiry recovery requires evidence that the old credential was rejected plus a non-secret fresh
credential id. The same id must be bound to the following ICE restart evidence.

All sampling is bounded by the manifest limit, itself capped by the package hard limit. Cleanup always
runs in the fixed reverse-resource order and never replaces the first primary failure.
