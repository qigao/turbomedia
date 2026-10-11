# WebRTC DataChannel Architecture

## Overview

TurboMedia's WebRTC DataChannel implementation provides peer-to-peer data communication over Salts CNet transports. The architecture follows a layered design with clear separation of concerns.

## Layer Stack

```
┌─────────────────────────────────────┐
│      Application Layer              │
│  (User Code using DC API)           │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│      DataChannel API Layer          │
│  - Context/Peer/Channel Management  │
│  - Event/Callback Dispatch          │
│  - DCEP Protocol Handling           │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│         SCTP Layer                  │
│  - Reliable/Unreliable Messaging    │
│  - Stream Multiplexing              │
│  - Flow Control                     │
│  (usrsctp library)                  │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│         DTLS Layer                  │
│  - Encryption/Decryption            │
│  - Certificate Validation           │
│  - Handshake                        │
│  (BoringSSL)                        │
└──────────────┬──────────────────────┘
               │
       ┌───────┴────────┬─────────┬──────────┐
       │                │         │          │
┌──────▼─────┐  ┌──────▼─────┐  ┌▼─────┐  ┌─▼──────┐
│ UDP        │  │ TCP        │  │ KCP  │  │  ICE   │
│ Transport  │  │ Transport  │  │      │  │ (STUN/ │
│            │  │            │  │      │  │  TURN) │
└────────────┘  └────────────┘  └──────┘  └────────┘
```

## Core Components

### 1. Context (`turbo_dc_context_t`)

**Responsibility:** Global configuration and resource management.

**Key Data:**
- DTLS certificate and private key
- Transport mode (UDP/TCP/KCP/ICE)
- SCTP/DTLS MTU settings
- List of all peers

**Lifecycle:**
```c
create → configure → manage_peers → destroy
```

**Design Decisions:**
- One context per process (singleton pattern for SCTP initialization)
- Owns global SCTP resources
- Auto-generates self-signed certificates if not provided

---

### 2. Peer (`turbo_dc_peer_t`)

**Responsibility:** Connection to a remote peer.

**Key Data:**
```c
struct turbo_dc_peer_s {
    turbo_dc_context_t *ctx;           // Parent context
    struct dtls_session_s *dtls;       // DTLS session
    struct socket *sctp_socket;        // SCTP socket (usrsctp)
    void *transport;                    // CNet stream/datagram or external transport
    const dc_transport_ops_t *transport_ops;

    turbo_dc_state_t state;            // Connection state

    // Callbacks
    turbo_dc_state_cb on_state;
    turbo_dc_channel_cb on_channel;
    turbo_dc_error_cb on_error;

    // Channel management
    turbo_dc_channel_t *channels[MAX_CHANNELS];
    uint16_t next_stream_id;
};
```

**State Machine:**
```
NEW → CONNECTING → CONNECTED → DISCONNECTING → CLOSED
                 ↘            ↗
                   FAILED
```

**Transport Integration:**
- UDP/TCP: CNet `cnet_datagram` / `cnet_client`
- ICE: Externally owned datagram transport attached with `turbo_dc_peer_set_external_transport()`
- KCP: uses a CNet packet session and is rejected when none is configured

---

### 3. Channel (`turbo_dc_channel_t`)

**Responsibility:** Application-level data stream.

**Key Data:**
```c
struct turbo_dc_channel_s {
    turbo_dc_peer_t *peer;             // Parent peer
    uint16_t stream_id;                // SCTP stream ID
    char *label;                       // Channel label/name

    turbo_dc_channel_config_t config;  // Reliability config
    turbo_dc_channel_state_t state;    // Channel state

    // Callbacks
    turbo_dc_open_cb on_open;
    turbo_dc_message_cb on_message;
    turbo_dc_close_cb on_close;
};
```

**DCEP (Data Channel Establishment Protocol):**
- Client sends DCEP_OPEN on even stream IDs
- Server responds on odd stream IDs
- Negotiates reliability parameters

**Stream ID Allocation:**
- Even IDs: Client-initiated channels
- Odd IDs: Server-initiated channels
- Prevents conflicts

---

## Data Flow

### Outbound (Send)

```
Application
    ↓ turbo_dc_channel_send(data)
Channel Layer
    ↓ Add PPID (WebRTC DCEP protocol)
SCTP
    ↓ sctp_sendv() - Fragment, sequence, reliability
DTLS
    ↓ dtls_write() - Encrypt
Transport
    ↓ UDP/TCP/KCP/ICE send
Network
```

### Inbound (Receive)

```
Network
    ↓ Socket read
Transport
    ↓ on_data callback
DTLS
    ↓ dtls_read() - Decrypt
SCTP
    ↓ usrsctp_conninput() - Reassemble
SCTP Callback
    ↓ Parse PPID, find channel by stream_id
Channel
    ↓ on_message callback
Application
```

---

## SCTP Integration

### usrsctp Configuration

```c
// Initialize once per process
usrsctp_init(0, send_cb, debug_printf);
usrsctp_sysctl_set_sctp_ecn_enable(0);
usrsctp_sysctl_set_sctp_pr_enable(1);  // Partial reliability
```

### Socket Setup

```c
socket = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, ...);

// Enable SCTP events
usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_EVENT, ...);

// Set NODELAY (disable Nagle)
usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_NODELAY, ...);
```

### Send Callback

```c
int send_cb(void *addr, void *data, size_t len, ...) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)addr;

    // Pass to DTLS for encryption
    dtls_write(peer->dtls, data, len);

    return 0;
}
```

---

## PeerConnection callback lifetime

PeerConnection owns its ICE owner, DC peer and media context. ICE receive,
ICE coordination and the DC deadline worker may execute concurrently with
the application's control thread. Internal callbacks borrow those objects;
they do not transfer ownership or retain packet views after returning.

Callback admission and an in-flight count use the existing ICE mutex/condition.
Destruction closes admission, requests ICE cancellation, joins the coordination
worker and waits for admitted callbacks while all three dependencies remain
alive. It then detaches/drains the media transport callback, destroys media,
drains/destroys DC, and finally joins/destroys the ICE owner. Rejected callbacks
touch only the still-live PeerConnection admission state. No mutex is held
across external callbacks, owner commands or thread joins. This adds no queue,
payload copy or thread; a callback that does not return delays destruction.

Cross-thread status flags use C11 atomics; this publishes individual flags, not
an atomic SDP/track transaction. The ICE-to-DC binding is installed once before
gathering starts and remains immutable through ICE restarts and teardown.
Callers must stop their own poll/control/media operations before destroy and
must not destroy a PeerConnection from its own callback. Track and signaling
mutation still require application coordination. Public signatures and packet
semantics do not change. Merely setting a closing flag or detaching callback
pointers cannot replace drain; reverting this protocol would restore that gap.

Connection readiness has three prerequisites: a usable ICE pair, established
DTLS, and successful SRTP/receive-track initialization. The DTLS flag remains
the transport fact used to protect the negotiated fingerprint; media readiness
is a separate fact, published only after initialization succeeds. ICE restart
retains successful media readiness, whereas DC failure/closure invalidates it.
ICE recovery cannot turn a failed media initialization into CONNECTED or restart
an already-established DTLS session. Flag updates and the CONNECTED predicate
commit under the existing ICE mutex; user callbacks remain outside that lock.
This prevents a delayed CONNECTED request from overriding a committed transport
loss. It does not serialize application callbacks or signaling/track mutation.

## DTLS Integration

### Session Creation

```c
dtls_session_t *dtls = dtls_session_create(
    cert_pem,
    key_pem,
    is_server,  // Client or server role
    on_dtls_data,
    on_dtls_state,
    peer
);
```

### Handshake

1. Client sends DTLS ClientHello
2. Server responds with ServerHello, Certificate
3. Client validates certificate (fingerprint in SDP for ICE mode)
4. Keys established
5. Application data can flow

### Data Callbacks

```c
void on_dtls_data(dtls_session_t *dtls, const void *data, size_t len, void *ud) {
    turbo_dc_peer_t *peer = (turbo_dc_peer_t *)ud;

    // Decrypted data → SCTP
    usrsctp_conninput(peer, data, len, 0);
}
```

---

## Transport Abstraction

### Direct Transports (UDP/TCP/KCP)

```c
// Create async client - transport determined by URL prefix
async_client_t *client = async_client_create(on_event, NULL);

// TCP connection
async_client_connect(client, "tcp://remote_host:port");

// UDP connection
async_client_connect(client, "udp://remote_host:port");

// KCP connection (reliable UDP)
async_client_connect(client, "kcp://remote_host:port");

void on_event(async_client_t *client, const async_client_event_t *event, void *ud) {
    if (event->type == ASYNC_CLIENT_EVENT_DATA) {
        // Network data → DTLS
        dtls_feed_data(peer->dtls, event->data, event->length);
    }
}
```

### ICE Transport

```c
// Set ICE agent
turbo_dc_peer_set_ice_agent(peer, ice_agent);

// ICE agent calls this when data arrives:
void on_ice_data(ice_agent_t *agent, const void *data, size_t len, void *ud) {
    turbo_dc_peer_feed_ice_data(peer, data, len);
}

// DataChannel writes back via:
void ice_send_cb(peer, data, len) {
    ice_agent_send(peer->ice_agent, data, len);
}
```

---

## Memory Management

### Allocation Strategy

1. **Context**: malloc() once, destroyed manually
2. **Peer**: malloc() per connection, ref-counted
3. **Channel**: malloc() per channel, owned by peer
4. **Buffers**: Zero-copy where possible, arena_buffer for SCTP

### Cleanup Order

```c
// 1. Close all channels
for (each channel) {
    turbo_dc_channel_close(channel);
    free(channel);
}

// 2. Close SCTP socket
usrsctp_close(peer->sctp_socket);

// 3. Destroy DTLS session
dtls_session_destroy(peer->dtls);

// 4. Close and destroy the selected transport strategy
peer->transport_ops->close(peer);
peer->transport_ops->destroy(peer);

// 5. Free peer
free(peer);
```

---

## Threading Model

**Transport ownership:**
- Direct TCP/UDP operations are submitted to the context's dedicated CNet owner thread
- CNet drives transport callbacks on that owner thread while the context coordinates DTLS timers
- ICE remains externally owned and feeds datagrams through the transport adapter

**Multi-threading support:**
- Public lifetime calls synchronize transport work through the internal command protocol
- Callbacks must not destroy their owning peer reentrantly

### DTLS operation ownership

ICE receive, connection startup, SCTP output and retransmission processing
can enter from different threads. The peer's operation count keeps storage alive;
it does not serialize `SSL` or its BIOs. Each SSL transaction therefore acquires
exclusive DTLS admission through the existing operation mutex/condition. The
mutex protects admission only; the admitted thread owns SSL, BIOs, handshake
readiness and shutdown state until it releases admission. Receive data remains
borrowed for the synchronous call and is copied into the input BIO before return.

The context transport worker also observes DTLS deadlines and is now created
for ICE contexts. ICE receive and ordinary calls remain on their existing
threads; only retransmission processing uses the worker. BoringSSL remains the
deadline authority. Active handshakes are counted under the context command
mutex; startup wakes the worker and completion/failure/close removes the peer
from that count. With no active handshakes an ICE worker sleeps on its condition
variable. During handshakes it checks every 10ms (direct CNet retains its existing
1ms poll). These are observation intervals, not changed protocol timeouts; OS
scheduling or a blocked callback can add delay.

The worker holds a peer operation lease while processing it, and releases the
context peer-list mutex before polling or invoking callbacks. The lease keeps
the current list node alive until the worker reacquires the list mutex and reads
its successor. Timer processing needs no native timer handle, retained payload,
or synchronous cross-owner command. The resource cost is one worker per context,
including ICE contexts, and wakeups during active handshakes; no throughput or
memory improvement is claimed.

SSL error classification and fingerprint verification occur in the transaction
that produced them. SCTP entry, state/error callbacks and transport sends occur
after admission is released, avoiding lock cycles with those external owners.
Handshake completion is claimed once; application reads wait until the elected
thread finishes SCTP startup. Output is copied from the BIO into callback-scoped
storage before sending; concurrent datagram sends may reorder records, which
DTLS already tolerates. No TLS identity or fingerprint check is relaxed.

Shutdown marks DTLS stopped under admission. Destruction
first rejects new peer operations and drains admitted callers, including the
timer worker, before freeing SSL and peer storage. Context destruction drains
its peers before stopping and joining the worker. Callback reentrant destruction
remains unsupported. This does not make arbitrary concurrent channel mutation,
transport replacement or higher-level peer-connection state access safe; those
retain their existing owner obligations.

A single coarse lock around handshake/receive was rejected because of timer
stop/wait deadlock and SCTP callback lock inversion. Moving ICE processing onto
the direct-CNet worker would change callback affinity and introduce synchronous
cross-owner waits. Native timer self-rearm also complicates proving callback
quiescence on the rc.10 Windows timer implementation. Reusing the context worker
for deadlines trades an ICE-context thread for explicit join/drain semantics,
while preserving receive/caller affinity. This local
admission protocol preserves those boundaries and the public ABI. Reverting it
restores the prior concurrency risk; a rollback
must instead keep all SSL entries on one caller-owned thread.

### DTLS datagram BIO boundary

BoringSSL writes one datagram per BIO write and reads one datagram per BIO read
(`ssl/d1_both.cc` and `ssl/ssl_buffer.cc` in the pinned 0.20240913.0 dependency).
A memory BIO loses that boundary: a fixed-size drain may merge packets, exceed
the configured MTU, or split a record. Both BIOs therefore use the existing CSTL
deque to hold owned `tstr` byte packets. This is a private adapter, not a DTLS
record parser; certificate verification and handshake/retransmission policy stay
with BoringSSL. Each BIO is bounded to 256 packets and 256 KiB of payload, with
at most 65535 bytes per packet. Capacity/allocation failure accepts no partial
packet and reports an error; there is no overwrite, wait, or unbounded growth.

All queue access uses the existing exclusive SSL admission, even when callers
come from different threads. Writes copy the caller's bytes; reads consume one
packet (a short read discards that packet's remainder, as a datagram read does).
Output dequeue transfers the owned packet to the sender, which releases SSL
admission before invoking transport code and frees the packet after the callback.
Concurrent send callbacks may reorder whole datagrams, as UDP can. Close stops
new SSL work; destruction drains operation leases before SSL frees both BIOs and
their queued packets. Queue storage is allocated on demand under a hard limit;
no allocation or throughput improvement is claimed.

Keeping the memory BIO and parsing DTLS record headers was rejected because it
would duplicate protocol framing and fail to preserve the library's packetization.
The adapter preserves the public ABI and existing transport callback lifetime.
Rolling back requires another boundary-preserving BIO, not a larger byte buffer.
The formal DTLS test uses real small-MTU handshakes and SRTP key export, alongside
packet isolation, short-read, capacity, and callback reentry checks. This fixes a
source-confirmed transport defect; the original #160 log contains no packet trace
and does not by itself establish this defect as that run's cause.

After handshake completion, `SSL_read` can regenerate the final handshake flight
in response to a repeated Finished without returning application bytes. The read
drain therefore flushes output after every read, including WANT_READ, outside
SSL admission and after terminal-error handling. A stopped session sends nothing.
The formal regression drops the server's first final-flight datagram and checks
recovery using the client's existing retransmission deadline, matching SRTP keys
and exactly one CONNECTED notification per peer. No SCTP write is needed to make
progress. The test failed before this fix on macOS in run 38105245427; this is
evidence for the regression scenario, not a packet trace of historical #160.

### GmSSL migration boundary

#### Private DTLS implementation work

The next migration implements a datagram state machine over GmSSL's existing
X.509, ECDH/signature, TLS PRF and AES-GCM APIs. `gmssl_dtls.h` is private and is
not installed. Production continues to use the qualified provider until the new
engine passes independent client/server interoperability and existing ownership
tests. It is not a stream-TLS-over-UDP adapter or an automatic fallback.

The transport owner serializes each session, supplies monotonic time and drains
owned output datagrams outside admission. A session borrows its immutable
identity context; context destruction requires all sessions to be gone. Input
is callback-scoped borrowing. Reassembly, retained flights and output queues own
their bytes. Destruction drains these objects and wipes session secrets.

Initial scope is DTLS 1.2, ECDHE with P-256, ECDSA/RSA SHA-256 signatures,
AES-128/256-GCM record protection, Extended Master Secret and all four existing
SRTP profiles. No resumption, renegotiation, DTLS 1.0/1.3, finite-field DHE or
ChaCha20 is claimed. Unsupported negotiation fails; it never downgrades to an
unauthenticated connection. Peer SHA-256 fingerprints are mandatory. Both
Finished and CertificateVerify are verified before readiness or key export.

Resource limits are per session: 64 KiB per handshake message, eight reassembly
slots with a combined 256 KiB body/coverage budget, sixteen retained flight
records/messages with 256 KiB payload, 256 KiB transcript, and 256 packets /
256 KiB for each output/plaintext queue. Datagram size is at most 65535 bytes;
encrypted plaintext records are at most 16384 bytes and outgoing records fit
the configured MTU. Arithmetic and queue admission are checked before mutation.
Unauthenticated unusable datagrams may be discarded as DTLS requires; accepted
application/output data is never silently dropped. Authenticated protocol or
capacity failures stop the session and discard its queued data explicitly.
Authenticated application records that arrive before Finished are discarded
without advancing handshake state; UDP/SCTP supplies any required retransmission.
The engine reports the earlier of its next retransmission and its 60-second
handshake lifetime, including a server waiting for ClientHello. Both receive
and poll enforce that lifetime. The outer connection deadline is unchanged.
Local and authenticated remote close revoke readiness/export access, clear
secrets and retained handshake input, and leave a closing alert for the caller
to drain. Already authenticated application data remains drainable after clean
close, including records coalesced ahead of close_notify; destruction clears any
undrained data. Terminal failures discard all queued data.

Qualification includes an independent BoringSSL peer in both roles, each SRTP
profile and matching exporter bytes, identity rejection, application data,
fragmentation, ordinary datagram loss/reordering and final-flight recovery.
The same formal suite covers mixed RSA/P-256 identities, damaged-tag discard,
replay suppression, export capacity, bounded output failure, deadlines, closing
alerts, early application records and paired GmSSL profile preference/final-flight
retirement. These tests use runtime-generated identities, not reusable keys.
Production cutover also requires the existing SCTP, lifetime, WHIP and platform
suites. The reference contracts are [DTLS 1.2](https://www.rfc-editor.org/rfc/rfc6347),
[DTLS-SRTP](https://www.rfc-editor.org/rfc/rfc5764),
[AEAD SRTP](https://www.rfc-editor.org/rfc/rfc7714) and
[Extended Master Secret](https://www.rfc-editor.org/rfc/rfc7627).

Authentication uses Salts Core's provider-neutral `cmeta_sha256`,
`cmeta_hmac_sha256`, `cmeta_crypto_equal` and Base64 helpers. Salts 2.3.0-rc.10
owns the GmSSL crypto backend; the auth target no longer links OpenSSL::Crypto.
HS256 token encoding, claim validation, limits and constant-time comparison are
preserved, with an independent known token vector in the formal auth suite.
Base64 is supplied by Core's existing libbase64 dependency, not by GmSSL.

Ephemeral identity generation also uses that existing GmSSL package: P-256 key
generation, the random positive serial and ECDSA/SHA-256 self-signing. The existing
v3 certificate, CN, 365-day validity and uppercase SHA-256 fingerprint contract
remain unchanged. Salts Core hashes the certificate DER. A private DER boundary
separates provider headers; BoringSSL imports the certificate/PKCS#8 key into its
DTLS context and checks the pair. No GmSSL type enters a public API. Provider key
state is cleared on every return; owned PKCS#8 bytes are cleared after import or
on failure. Fingerprints are published only after successful installation.
Configured PEM identities retain their existing loading behavior.

The formal certificate test verifies the imported self-signature, P-256 curve,
SHA-256 algorithm, subject/issuer, serial, validity and fingerprint using the
importing provider. Existing real DTLS/SRTP tests cover identity use in handshakes.
This isolates a supported migration from the DTLS blocker without adding another
dependency or a crypto abstraction framework. It can be rolled back within the
private certificate helper without changing signaling or configured identities.

Full WebRTC provider replacement is blocked by the current central
[GmSSL port](https://github.com/qigao/vcpkg-cache/tree/master/ports/gmssl)
(3.2.0#9, upstream commit `7c9f02904ef33e59c87b4f16621cc8fd434e7579`).
The patched `tls_ctx_init` accepts TLS 1.2, TLS 1.3 and TLCP only. DTLS constants
and trace names do not implement datagram records, handshake fragmentation,
retransmission or DTLS-SRTP negotiation. Stream TLS/exporter support cannot
replace that contract. DataChannel/RTC still use BoringSSL for DTLS. The libsrtp
overlay is independently migrated to GmSSL while retaining existing AEAD profiles.
RTC no longer directly links OpenSSL::SSL or OpenSSL::Crypto; its DTLS dependency
is the existing public DataChannel component.

Replacing library names or disabling profiles would break browser interop.
The next prerequisite is a qualified DTLS-SRTP-capable provider in the central
cache, including certificate fingerprints, SRTP exporter/profile negotiation,
loss recovery and browser interop. Until then, keep this dependency explicit;
do not present the auth migration as complete BoringSSL removal. Existing
cache-only restoration remains in use. The auth change can be rolled back
independently without changing tokens or the public API.

The libSRTP provider lives in the existing product overlay. It keeps libSRTP's
packet policy, key derivation and replay state, and adapts its cipher/auth tables
to GmSSL AES-CTR, AES-GCM and HMAC-SHA1. All existing
AES-CM and GCM profiles remain available, including AES-256-GCM. Replacing the
whole SRTP stack or disabling AEAD is rejected. DTLS remains a separate blocker.

Each libSRTP cipher/auth context exclusively owns its GmSSL state under the
existing session's serialization contract. ICM retains the unused bytes of the
last CTR block between calls. GCM copies successive AAD inputs because SRTCP
passes a temporary trailer separately; storage is owned until context teardown,
reused between packets, and bounded by INT_MAX (libSRTP's packet-length domain).
Growth checks the sum before allocation and preserves old storage on failure.
No partial input is accepted on allocation failure. IV reset clears per-packet
AAD/tag state; authentication must complete before plaintext becomes visible.
Context destruction wipes key schedules, HMAC state and retained AAD storage.
This requires no new dependency or public API. It may add an allocation when a
session first sees a larger authenticated header; no speedup is claimed.

Qualification must include libSRTP's built-in cipher/auth known-answer tests
(run by initialization), all four public profiles for RTP/SRTCP, wrong-key and
damaged-tag rejection, and the existing DTLS/WebRTC suites through CTest. Cache
publication precedes consumer CI; do not add a consumer source-build fallback.
Rollback restores the prior overlay and its cache recipe without changing keys,
SDP, tokens or packet formats.

---

## Error Handling

### Error Propagation

```
Transport error
    ↓
DTLS error (connection lost)
    ↓
Peer state → FAILED
    ↓
on_state(FAILED) callback
    ↓
All channels closed
```

### Error Codes

- DTLS errors: Certificate validation, handshake timeout
- SCTP errors: Association failed, stream reset
- Transport errors: Connection refused, timeout

---

## Performance Optimizations

### 1. Zero-Copy Paths

- SCTP uses `sctp_sendv()` with iovec (no buffer copy)
- arena_buffer wraps external data without copying

### 2. Batching

- SCTP coalesces small messages into single DTLS packet
- DTLS writes MTU-sized packets when possible

### 3. Timer Efficiency

- Single global SCTP timer for all peers
- 10ms interval sufficient for retransmission timing

---

## Limitations

1. **Max Channels per Peer**: 65535 (SCTP stream limit)
2. **Max Message Size**: 256KB (configurable via SCTP)
3. **MTU**: 1188 bytes (SCTP) / 1280 bytes (DTLS) default

---

## Future Improvements

- [ ] SCTP PMTUD (Path MTU Discovery)
- [ ] Bandwidth estimation
- [ ] Congestion control tuning
- [ ] Multi-homed SCTP support
