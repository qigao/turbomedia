# WebRTC DataChannel Architecture

## Overview

TurboNet's WebRTC DataChannel implementation provides peer-to-peer data communication with multiple transport options. The architecture follows a layered design with clear separation of concerns.

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
│  (OpenSSL)                          │
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
    turbo_netcore_element_t *transport; // Underlying transport (TCP/UDP/KCP)

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
- UDP/TCP/KCP: Direct netcore element
- ICE: Feeds data from ICE agent via `turbo_dc_peer_feed_ice_data()`

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

// 4. Close transport
turbo_netcore_close(peer->transport);

// 5. Free peer
free(peer);
```

---

## Threading Model

**Single-threaded by design:**
- All callbacks run in the same thread as netcore loop
- SCTP timer must be called periodically from main thread
- No locks needed

**Multi-threading support:**
- User must synchronize if calling API from multiple threads
- Recommended: Use message queue to main thread

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
