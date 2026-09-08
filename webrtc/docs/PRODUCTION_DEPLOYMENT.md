## WebRTC Production Deployment Guide

This guide covers deploying TurboNet WebRTC in production environments.

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [STUN/TURN Server Setup](#stunturn-server-setup)
3. [Connection Timeout Configuration](#connection-timeout-configuration)
4. [Reconnection Strategy](#reconnection-strategy)
5. [Browser Interoperability](#browser-interoperability)
6. [Security Best Practices](#security-best-practices)
7. [Monitoring and Debugging](#monitoring-and-debugging)
8. [Performance Tuning](#performance-tuning)

## Prerequisites

### Required Components

- **STUN Server**: For NAT traversal (discovering public IP)
- **TURN Server**: For relay when direct P2P fails
- **Signaling Server**: For SDP/candidate exchange
- **TLS Certificates**: For secure signaling (WSS)

### System Requirements

- Linux/Windows/macOS
- libuv >= 1.40
- BoringSSL (resolved by the project vcpkg manifest)
- usrsctp >= 0.9.5
- libSRTP >= 2.3 (for media)

## STUN/TURN Server Setup

### SFU node configuration

The SFU passes the following bounded list directly into each PeerConnection:

```toml
[ice]
stun_servers = ["stun:stun.example.com:3478"]
turn_servers = ["turn:username:password@turn.example.com:3478"]
allow_loopback = false
```

Keep TURN credentials out of committed files by setting
`TURBO_SFU_TURN_SERVER`. `TURBO_SFU_STUN_SERVER` provides the equivalent
single-server environment override. Loopback candidates are disabled by
default; `allow_loopback` and `--allow-loopback` are intended for local tests.

The WHIP/WHEP endpoints are unavailable unless either the legacy
`TURBO_SFU_MEDIA_ACCESS_TOKEN` or signed-token verification is configured.
The signaling management API is disabled by default and, when enabled,
requires scoped signed-token verification or the explicit
`TURBO_SIGNALING_ADMIN_TOKEN` compatibility path.

For multi-tenant SFU access, configure:

```text
TURBO_SFU_AUTH_ACTIVE_KEY_ID=sfu-2026-07
TURBO_SFU_AUTH_ACTIVE_SECRET=<at-least-32-bytes-of-CSPRNG-output>
TURBO_SFU_AUTH_MAX_TTL_SECONDS=3600
TURBO_SFU_AUTH_CLOCK_SKEW_SECONDS=30
```

Tokens use the compact JWS form with `alg=HS256`,
`typ=turbomedia-auth+jwt`, and the configured `kid`. Required claims are
`iss`, `sub`, `aud`, `scope`, `iat`, and `exp`. SFU control tokens use audience
`turbomedia-sfu-control` and scope `sfu.control.write` or
`sfu.control.dangerous`; media tokens use audience `turbomedia-sfu-media` and
one or more of `sfu.media.publish`, `sfu.media.subscribe`,
`sfu.media.trickle`, and `sfu.media.delete`. `room_id` and `participant_id`
must exactly match the routed resource; unbound tokens cannot authorize bound
routes. The deliberately narrow profile follows the algorithm, audience,
explicit-type and mutually exclusive validation guidance in
[RFC 8725](https://www.rfc-editor.org/rfc/rfc8725.html).

Rotate by moving the old pair to
`TURBO_SFU_AUTH_PREVIOUS_KEY_ID`/`TURBO_SFU_AUTH_PREVIOUS_SECRET` and installing
the new active pair atomically. Remove the previous pair after the configured
maximum TTL plus clock skew. The `kid` is matched only against these two local
slots; it never triggers a file, network, or database lookup. Static
`TURBO_SFU_NODE_CONTROL_TOKEN` and `TURBO_SFU_MEDIA_ACCESS_TOKEN` values are
accepted only when explicitly configured and should be removed after client
migration.

To revoke an individual signed token before its expiry, compute SHA-256 over
the exact compact token bytes and configure its 64-character lowercase digest
in the verifier's `revoked_token_sha256` list. Lists contain at most 256
comma-separated digests, with no whitespace or wildcard entries. Use the
matching trust-domain variable:

```text
TURBO_SFU_AUTH_REVOKED_TOKEN_SHA256=<digest>[,<digest>...]
TURBO_SIGNALING_HTTP_AUTH_REVOKED_TOKEN_SHA256=<digest>[,<digest>...]
TURBO_SIGNALING_AUTH_REVOKED_TOKEN_SHA256=<digest>[,<digest>...]
TURBO_ROOM_SERVICE_AUTH_REVOKED_TOKEN_SHA256=<digest>[,<digest>...]
```

The same value is available as `revoked_token_sha256` in each corresponding
TOML auth table. Configuration is validated at startup; malformed or oversized
lists fail fast. The list contains no plaintext token and applies only to
signed tokens. Revoke a static compatibility bearer by removing or rotating
that static value instead. Revocation state is immutable and process-local:
update every verifier instance and restart it for a change to take effect.
Remove entries after the token expiry plus clock skew. This mechanism provides
exact emergency revocation, not distributed or real-time revocation.

For signaling management, configure:

```text
TURBO_SIGNALING_HTTP_AUTH_ISSUER=turbomedia
TURBO_SIGNALING_HTTP_AUTH_ACTIVE_KEY_ID=signaling-management-2026-07
TURBO_SIGNALING_HTTP_AUTH_ACTIVE_SECRET=<at-least-32-bytes-of-CSPRNG-output>
TURBO_SIGNALING_HTTP_AUTH_PREVIOUS_KEY_ID=signaling-management-2026-06
TURBO_SIGNALING_HTTP_AUTH_PREVIOUS_SECRET=<old-secret-during-overlap>
TURBO_SIGNALING_HTTP_AUTH_MAX_TTL_SECONDS=3600
TURBO_SIGNALING_HTTP_AUTH_CLOCK_SKEW_SECONDS=30
```

The management audience is `turbomedia-signaling-management`. Status and list
routes require `signaling.management.read`; broadcast requires
`signaling.management.write`; peer removal requires
`signaling.management.dangerous`. Room and peer path parameters must exactly
match the token claims. Static `TURBO_SIGNALING_ADMIN_TOKEN` remains an
explicit migration mode.

For WebSocket peer admission, configure the separate trust domain:

```text
TURBO_SIGNALING_AUTH_ISSUER=turbomedia
TURBO_SIGNALING_AUTH_ACTIVE_KEY_ID=signaling-peer-2026-07
TURBO_SIGNALING_AUTH_ACTIVE_SECRET=<at-least-32-bytes-of-CSPRNG-output>
TURBO_SIGNALING_AUTH_PREVIOUS_KEY_ID=signaling-peer-2026-06
TURBO_SIGNALING_AUTH_PREVIOUS_SECRET=<old-secret-during-overlap>
TURBO_SIGNALING_AUTH_MAX_TTL_SECONDS=3600
TURBO_SIGNALING_AUTH_CLOCK_SKEW_SECONDS=30
```

Set `[auth].enabled = true` in the signaling TOML. The application identity
provider must issue a token with audience `turbomedia-signaling-peer`, scope
`signaling.peer.join`, and exact `room_id` and `participant_id` claims. A
browser sends it in the first application message:

```javascript
const socket = new WebSocket("wss://signal.example.com/");
socket.addEventListener("open", () => {
  socket.send(JSON.stringify({
    type: "join",
    room: "room-a",
    peer_id: "alice",
    token: accessToken
  }));
});
```

The server verifies the token before replacing its temporary connection ID.
It rejects missing, expired, wrong-scope, cross-room, cross-peer and duplicate
identity attempts and does not accept other signaling messages before a
successful join. Keep the token in the message body rather than the WebSocket
URL so reverse-proxy access logs do not capture it. Use WSS in production.
Rotate the peer-admission pair with the same active/previous overlap rule, but
do not reuse the management or SFU secrets.

### Room Service control and SFU command identity

Room Service mutating commands and the `/api/v1/join`, `/api/v1/publish`, and
`/api/v1/subscribe` facade routes accept audience
`turbomedia-room-control`. Use `room.control.write` for ordinary mutations and
`room.control.dangerous` for destructive or externally consequential
commands. A token carrying `room_id` or `participant_id` authorizes only that
exact resource.

Configure the Room verifier independently from the SFU verifier:

```text
TURBO_ROOM_SERVICE_AUTH_ISSUER=turbomedia
TURBO_ROOM_SERVICE_AUTH_ACTIVE_KEY_ID=room-control-2026-07
TURBO_ROOM_SERVICE_AUTH_ACTIVE_SECRET=<at-least-32-bytes-of-CSPRNG-output>
TURBO_ROOM_SERVICE_AUTH_PREVIOUS_KEY_ID=room-control-2026-06
TURBO_ROOM_SERVICE_AUTH_PREVIOUS_SECRET=<old-secret-during-overlap>
TURBO_ROOM_SERVICE_AUTH_MAX_TTL_SECONDS=3600
TURBO_ROOM_SERVICE_AUTH_CLOCK_SKEW_SECONDS=30
```

Room-to-SFU authentication uses a separate signer, so a Room API verifier key
does not automatically grant SFU control authority:

```text
TURBO_ROOM_SERVICE_SFU_AUTH_ISSUER=turbomedia
TURBO_ROOM_SERVICE_SFU_AUTH_KEY_ID=sfu-control-2026-07
TURBO_ROOM_SERVICE_SFU_AUTH_SECRET=<at-least-32-bytes-of-CSPRNG-output>
TURBO_ROOM_SERVICE_SFU_AUTH_TTL_SECONDS=60
```

The Room signer key must match the SFU active or previous verifier slot.
Rotate without an authorization outage by first deploying the new SFU active
key while retaining the old key as previous, then switching every Room signer,
then removing the SFU previous key after the maximum token TTL plus clock
skew. Room Service signs each internal command immediately before its
CHTTP `chttp_client` request, binding the token to the command's
`room_id`, optional `participant_id`, and required SFU scope. When the signed
Room-to-SFU mode is configured it takes precedence over
`TURBO_ROOM_SERVICE_SFU_CONTROL_TOKEN`; the static value remains only as an
explicit rollback/migration path.

### SFU media resource API

Provision the room through the control plane, then create a media resource:

```bash
curl -i \
  -H "Authorization: Bearer ${SFU_MEDIA_JWT}" \
  -H "Content-Type: application/sdp" \
  --data-binary @offer.sdp \
  https://media.example.com/whip/room-1/publisher-1
```

Use `/whep/room-1/viewer-1` for playback. WHIP accepts active `sendonly` or
`sendrecv` offers; WHEP accepts active `recvonly` or `sendrecv` offers. The
response is `201 Created` with an SDP answer, a strong `ETag`, and an opaque
resource URL in `Location`.

Trickle candidates or restart ICE at that exact resource URL:

```bash
curl -i -X PATCH \
  -H "Authorization: Bearer ${SFU_MEDIA_JWT}" \
  -H 'Content-Type: application/trickle-ice-sdpfrag' \
  -H 'If-Match: "1"' \
  --data-binary @restart.sdpfrag \
  https://media.example.com/whip/room-1/publisher-1/sessions/RESOURCE_ID
```

Terminate the resource with authenticated `DELETE`. The WHIP contract follows
[RFC 9725](https://www.rfc-editor.org/rfc/rfc9725.html); WHEP is pinned to
[draft-04](https://datatracker.ietf.org/doc/html/draft-ietf-wish-whep-04).

### Option 1: Use Public STUN Servers

```c
const char *stun_servers[] = {
    "stun:stun.l.google.com:19302",
    "stun:stun1.l.google.com:19302",
    "stun:stun2.l.google.com:19302"
};

ice_integration_ctx_t *ice = ice_integration_create(
    peer, loop,
    stun_servers, 3,
    NULL, NULL, NULL, 0
);
```

**Pros**: Free, no setup required
**Cons**: No reliability guarantee, no TURN support

### Option 2: Deploy coturn Server

Install coturn on your server:

```bash
# Ubuntu/Debian
sudo apt-get install coturn

# Enable coturn
sudo systemctl enable coturn
sudo systemctl start coturn
```

Configure `/etc/turnserver.conf`:

```conf
# Listening port
listening-port=3478
tls-listening-port=5349

# External IP (your server's public IP)
external-ip=YOUR_PUBLIC_IP

# Realm
realm=example.com

# Authentication
lt-cred-mech
user=username:password

# Logging
log-file=/var/log/turnserver.log
verbose

# Security
fingerprint
no-multicast-peers
no-cli
```

Restart coturn:

```bash
sudo systemctl restart coturn
```

Use in your application:

```c
const char *stun_servers[] = {
    "stun:your-server.com:3478"
};

const char *turn_servers[] = {
    "turn:your-server.com:3478"
};

const char *turn_usernames[] = {
    "username"
};

const char *turn_credentials[] = {
    "password"
};

ice_integration_ctx_t *ice = ice_integration_create(
    peer, loop,
    stun_servers, 1,
    turn_servers, turn_usernames, turn_credentials, 1
);
```

### Option 3: Use Managed TURN Service

Services like Twilio, Xirsys, or Metered provide managed TURN:

```c
const char *turn_servers[] = {
    "turn:global.turn.twilio.com:3478?transport=udp"
};

const char *turn_usernames[] = {
    "your-twilio-username"
};

const char *turn_credentials[] = {
    "your-twilio-credential"
};
```

## Connection Timeout Configuration

### Default Timeouts

```c
ice_integration_ctx_t *ice = ice_integration_create(...);

// Set connection timeout (default: 30 seconds)
ice_integration_set_connection_timeout(ice, 30000);

// Set max reconnection attempts (default: 5)
ice_integration_set_max_reconnect_attempts(ice, 5);
```

### Recommended Settings by Use Case

#### Real-time Communication (Voice/Video)
```c
ice_integration_set_connection_timeout(ice, 10000);  // 10 seconds
ice_integration_set_max_reconnect_attempts(ice, 3);
```

#### File Transfer
```c
ice_integration_set_connection_timeout(ice, 60000);  // 60 seconds
ice_integration_set_max_reconnect_attempts(ice, 10);
```

#### Gaming
```c
ice_integration_set_connection_timeout(ice, 5000);   // 5 seconds
ice_integration_set_max_reconnect_attempts(ice, 2);
```

## Reconnection Strategy

SaltsNet::ICE exposes a versioned restart operation, and PeerConnection exposes
`turbo_peer_connection_restart_ice()`. The SFU resource API carries restart
credentials and candidates in `application/trickle-ice-sdpfrag` under a strong
ETag. The legacy `ice_integration_reconnect()` helper below remains retry
accounting and scheduling only.

### Current retry scheduling

The ICE integration reports failures and schedules retry attempts:

```c
// Reconnection is triggered on:
// 1. ICE_STATE_FAILED
// 2. ICE_STATE_DISCONNECTED
// 3. Connection timeout

// Monitor reconnection via callback
void on_ice_state(ice_state_t state, void *user_data) {
    switch (state) {
        case ICE_STATE_DISCONNECTED:
            printf("Connection lost; application restart flow required\n");
            break;
        case ICE_STATE_FAILED:
            printf("Connection failed; retry scheduled\n");
            break;
        case ICE_STATE_CONNECTED:
            printf("Transport connected\n");
            break;
    }
}

ice_integration_on_state_change(ice, on_ice_state, NULL);
```

### Conditional WHIP/WHEP restart

Send a `PATCH` to the resource `Location` returned by `POST`, with its current
`If-Match` ETag and an SDP fragment containing a new `ice-ufrag`/`ice-pwd`
generation. A candidate-only fragment returns `204`; a restart returns `200`
with the server's new fragment and ETag. A stale ETag returns `412`.

### Application backoff

Implement custom backoff for reconnection:

```c
typedef struct {
    int attempt;
    int base_delay_ms;
    int max_delay_ms;
} reconnect_state_t;

void schedule_reconnect(reconnect_state_t *state) {
    int delay = state->base_delay_ms * (1 << state->attempt);
    if (delay > state->max_delay_ms) {
        delay = state->max_delay_ms;
    }
    
    // Schedule reconnection after delay
    salts_timer_start(timer, on_reconnect, delay, 0);
    state->attempt++;
}
```

## Browser Interoperability

### Testing with Browsers

1. Build the browser interop example:

```bash
cmake --build build --target browser_interop
```

2. Run the C application:

```bash
./build/bin/browser_interop
```

3. Open `browser_interop.html` in Chrome/Firefox

4. Follow the on-screen instructions to exchange SDP

### Browser compatibility status

| Browser | Version | Status |
|---------|---------|--------|
| Chrome | Current supported releases | Manual example only |
| Firefox | Current supported releases | Not acceptance-tested |
| Safari | Current supported releases | Not acceptance-tested |
| Edge | Current supported releases | Manual example only |

### Common Browser Issues

#### Issue: ICE candidates not exchanged

**Solution**: Ensure trickle ICE is implemented:

```javascript
// Browser side
pc.onicecandidate = (event) => {
    if (event.candidate) {
        // Send to C application via signaling
        sendToSignaling({
            type: 'candidate',
            candidate: event.candidate.candidate
        });
    }
};
```

```c
// C application side
void on_candidate(const char *candidate_sdp, void *user_data) {
    // Send to browser via signaling
    send_to_browser(candidate_sdp);
}
```

#### Issue: DTLS handshake fails

**Solution**: Verify fingerprint exchange:

```c
// Get local fingerprint
char fp_hash[32], fp[128];
turbo_dc_context_get_local_fingerprint(ctx, fp_hash, sizeof(fp_hash), 
                                       fp, sizeof(fp));

// Send to browser in SDP
// Browser will verify against its certificate
```

## Security Best Practices

### 1. Use TLS for Signaling

The signaling server loads an explicit certificate chain and private key for
WSS through CHTTP/CNet. Its management API, the SFU WHIP/WHEP/control listener,
and the room-service listener use Iris HTTPS with the same explicit identity
contract. Set `use_tls = true` and both `cert_file` and `key_file` in the
corresponding `[server]` or `[http_api]` section. Startup fails before serving
traffic if either identity file is missing or cannot be loaded.

Room Service uses CHTTP `chttp_client` for outbound SFU control requests.
Public certificates use the system trust store. For a private PKI, set
`[sfu].ca_file` (or `TURBO_ROOM_SERVICE_SFU_CA_FILE`); peer and hostname
verification remain enabled. There is no insecure skip-verification option.
A reverse proxy remains optional for edge policy, certificate automation, and
rate limiting, rather than being required to provide transport encryption.

### 2. Validate Remote Fingerprints

```c
// Always verify remote DTLS fingerprint
turbo_dc_peer_set_remote_fingerprint(peer, fp_hash, fp);

// Fingerprint mismatch will cause connection to fail
```

### 3. Use Strong ICE Credentials

```c
// ICE credentials are auto-generated with sufficient entropy
// Minimum 4 chars for ufrag, 22 chars for password (RFC 8445)
```

### 4. Enable SRTP for Media

```c
// SRTP is automatically enabled for media tracks
turbo_media_setup_srtp(media_ctx);
```

### 5. Configure Signaling Resource Bounds

The signaling application enables bounded defaults. Tune them from measured
SDP sizes, fan-out, and expected client behavior:

```toml
[limits]
max_peers = 1000
max_rooms = 100
peer_timeout_ms = 60000
join_timeout_ms = 10000
max_message_size = 65536
messages_per_second = 100
message_burst = 200
max_outbox_messages = 256
max_outbox_bytes = 1048576
max_connections_per_source = 100
source_admissions_per_second = 20
source_admission_burst = 50
max_source_states = 4096
source_state_ttl_ms = 300000
```

`join_timeout_ms` is a fixed deadline from WebSocket admission to the first
successful `join`; later traffic does not extend it. CHTTP rejects a complete
text or fragmented WebSocket message above `max_message_size` before handing
it to signaling. Each connection then has a token bucket and a bounded copied
outbox. A rate or outbox violation closes that connection.

After CHTTP/CNet completes TLS and the HTTP WebSocket upgrade, signaling groups
connections by the socket peer's binary IPv4/IPv6 address. The ephemeral port
is ignored and IPv4-mapped IPv6 is normalized to IPv4.
`max_connections_per_source` bounds concurrent upgraded connections, while
`source_admissions_per_second` and `source_admission_burst` bound upgraded
connection admission attempts. Inactive token state is retained for
`source_state_ttl_ms`; `max_source_states` is a hard process-local cardinality
limit. If the state table is full, a previously unseen source is rejected.

The authenticated `/api/v1/status` response reports cumulative
`authentication`, `join_timeout`, `message_rate`, `outbox_overflow`,
`source_address`, `source_capacity`, `source_rate`, and `source_concurrency`
rejection counters, plus the current `source_state_count`. Alert on rejection
rates, not only their absolute values.

The source identity is the direct socket peer. TurboMedia does not trust
`X-Forwarded-For` or similar headers. Behind a reverse proxy, configure the
corresponding policy at that trusted proxy because signaling sees the proxy IP.
These controls run after TLS and WebSocket upgrade and are process-local.
Internet deployments still need pre-handshake edge limits, tenant-wide quotas,
and distributed enforcement where multiple signaling nodes share traffic.

#### Policy boundary and rollback

The selected design uses the direct socket peer after upgrade because that
identity is available from CHTTP/CNet on every supported WS/WSS backend and does
not require trusting attacker-controlled HTTP forwarding headers. Enforcing
only at an external proxy would leave direct deployments unbounded; trusting
`X-Forwarded-For` inside signaling would require an explicit trusted-proxy CIDR
contract that the current configuration does not have.

The trade-off is that proxy deployments group traffic under the proxy address,
and handshake work occurs before this policy. Deploy the same limits at the
trusted edge using its verified client identity. Migration requires a process
restart because the limits are startup configuration. To roll back only this
policy while preserving all other signaling protections, set
`max_connections_per_source`, `source_admissions_per_second`,
`source_admission_burst`, `max_source_states`, and `source_state_ttl_ms` to
zero and restart. The production defaults remain enabled.

## Monitoring and Debugging

### Enable Logging

```c
#include "tlog.h"

// Set log level
tlog_set_level(TURBO_LOG_DEBUG);

// Custom log handler
void my_log_handler(turbo_log_level_t level, const char *module,
                    const char *message, void *user_data) {
    // Send to monitoring system
    send_to_monitoring(level, module, message);
}

tlog_set_handler(my_log_handler, NULL);
```

### Monitor ICE Statistics

```c
void on_ice_state(ice_state_t state, void *user_data) {
    // Log state transitions
    log_metric("ice.state", state);
    
    if (state == ICE_STATE_CONNECTED) {
        // Get selected candidate pair
        ice_candidate_t local, remote;
        ice_agent_get_selected_pair(agent, &local, &remote);
        
        log_info("ICE", "Selected pair: %s:%d -> %s:%d",
                 local.ip, local.port, remote.ip, remote.port);
    }
}
```

### Monitor DataChannel Statistics

```c
turbo_media_stats_t stats;
turbo_media_track_get_stats(track, &stats);

printf("Packets sent: %lu\n", stats.packets_sent);
printf("Packets received: %lu\n", stats.packets_received);
printf("Bytes sent: %lu\n", stats.bytes_sent);
printf("Bytes received: %lu\n", stats.bytes_received);
printf("Packet loss: %.2f%%\n", stats.packet_loss_rate * 100);
```

### Health Checks

```c
typedef struct {
    int ice_connected;
    int dtls_connected;
    int channel_open;
    time_t last_activity;
} health_status_t;

int check_health(health_status_t *status) {
    time_t now = time(NULL);
    
    // Check for stale connection
    if (now - status->last_activity > 60) {
        return 0;  // Unhealthy
    }
    
    // Check all components
    return status->ice_connected && 
           status->dtls_connected && 
           status->channel_open;
}
```

## Performance Tuning

### 1. Optimize ICE Gathering

```c
ice_config_t config = ice_default_config();

// Reduce gathering timeout for faster connection
config.gathering_timeout_ms = 5000;  // 5 seconds

// Use aggressive nomination for faster checks
config.aggressive_nomination = 1;

// Disable mDNS if not needed (faster gathering)
config.use_mdns_candidates = 0;
```

### 2. Tune SCTP Parameters

```c
turbo_dc_config_t dc_config = {
    .sctp_mtu = 1200,  // Adjust for network
    .dtls_mtu = 1280
};
```

### 3. Optimize Media Encoding

```c
turbo_video_codec_config_t video_cfg = {
    .width = 1280,
    .height = 720,
    .framerate = 30,
    .bitrate = 2000000,  // 2 Mbps
    .keyframe_interval = 60,
    .threads = 4,  // Use multiple threads
    .quality = 30  // Balance quality/speed
};
```

### 4. Use Connection Pooling

```c
// Reuse ICE agents for multiple connections
typedef struct {
    turbo_ice_agent_t *agents[MAX_CONNECTIONS];
    int count;
} ice_pool_t;

turbo_ice_agent_t *get_ice_agent(ice_pool_t *pool) {
    if (pool->count > 0) {
        return pool->agents[--pool->count];
    }
    return ice_agent_create(&config);
}
```

### 5. Monitor Memory Usage

```c
// Track allocations
size_t total_allocated = 0;

void *tracked_malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr) {
        total_allocated += size;
    }
    return ptr;
}

// Periodic memory check
void check_memory(void) {
    if (total_allocated > MAX_MEMORY) {
        log_warning("Memory", "High memory usage: %zu bytes", total_allocated);
    }
}
```

## Load Testing

### Simulate Multiple Connections

```c
#define MAX_PEERS 100

typedef struct {
    turbo_dc_peer_t *peers[MAX_PEERS];
    ice_integration_ctx_t *ice_contexts[MAX_PEERS];
    int count;
} load_test_t;

void run_load_test(load_test_t *test) {
    for (int i = 0; i < MAX_PEERS; i++) {
        // Create peer
        test->peers[i] = turbo_dc_peer_create(ctx, NULL, 0, NULL);
        
        // Create ICE integration
        test->ice_contexts[i] = ice_integration_create(
            test->peers[i], loop,
            stun_servers, 1,
            NULL, NULL, NULL, 0
        );
        
        // Start gathering
        ice_integration_start_gathering(test->ice_contexts[i]);
    }
    
    // Monitor connection success rate
    int connected = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (turbo_dc_peer_get_state(test->peers[i]) == TURBO_DC_STATE_CONNECTED) {
            connected++;
        }
    }
    
    printf("Connection success rate: %.2f%%\n", 
           (connected * 100.0) / MAX_PEERS);
}
```

## Troubleshooting

### Connection Fails

1. Check STUN/TURN server accessibility
2. Verify firewall rules
3. Enable debug logging
4. Test with public STUN servers first

### High Latency

1. Check network conditions
2. Use closer TURN servers
3. Optimize codec settings
4. Enable jitter buffer tuning

### Memory Leaks

1. Use valgrind: `valgrind --leak-check=full ./your_app`
2. Ensure proper cleanup of all resources
3. Check for circular references

### Browser Compatibility Issues

1. Test with latest browser versions
2. Check browser console for errors
3. Verify SDP format compatibility
4. Test ICE candidate exchange

## Production Checklist

- [ ] STUN/TURN servers configured
- [x] In-process WSS/HTTPS implemented and loopback certificate verification tested
- [ ] Connection timeouts configured
- [x] PeerConnection and WHIP/WHEP ICE restart implemented
- [ ] Logging and monitoring enabled
- [ ] Load testing completed
- [ ] Browser interop tested
- [ ] Security audit performed
- [ ] Documentation updated
- [ ] Deployment scripts ready

## Support

For production support:
- GitHub Issues: https://github.com/your-repo/turbonet/issues
- Documentation: https://docs.turbonet.io
- Email: support@turbonet.io
