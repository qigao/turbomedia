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
- OpenSSL >= 1.1.1
- usrsctp >= 0.9.5
- libSRTP >= 2.3 (for media)

## STUN/TURN Server Setup

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

### Automatic Reconnection

The ICE integration handles reconnection automatically:

```c
// Reconnection is triggered on:
// 1. ICE_STATE_FAILED
// 2. ICE_STATE_DISCONNECTED
// 3. Connection timeout

// Monitor reconnection via callback
void on_ice_state(ice_state_t state, void *user_data) {
    switch (state) {
        case ICE_STATE_DISCONNECTED:
            printf("Connection lost, reconnecting...\n");
            break;
        case ICE_STATE_FAILED:
            printf("Connection failed, attempting reconnection...\n");
            break;
        case ICE_STATE_CONNECTED:
            printf("Reconnected successfully\n");
            break;
    }
}

ice_integration_on_state_change(ice, on_ice_state, NULL);
```

### Manual Reconnection

Trigger reconnection manually (e.g., on network change):

```c
// Detect network change (platform-specific)
void on_network_change(void) {
    ice_integration_reconnect(ice);
}
```

### Exponential Backoff

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
    turbo_timer_start(timer, on_reconnect, delay, 0);
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

### Browser Compatibility

| Browser | Version | Status |
|---------|---------|--------|
| Chrome | 90+ | ✅ Tested |
| Firefox | 88+ | ✅ Tested |
| Safari | 14+ | ⚠️ Partial |
| Edge | 90+ | ✅ Tested |

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

```c
webrtc_signaling_config_t config = {
    .host = "0.0.0.0",
    .port = 8443,
    .use_tls = 1,
    .cert_file = "/path/to/cert.pem",
    .key_file = "/path/to/key.pem"
};
```

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

### 5. Implement Rate Limiting

```c
// Limit connection attempts per IP
typedef struct {
    char ip[64];
    int attempts;
    time_t last_attempt;
} rate_limit_entry_t;

int check_rate_limit(const char *ip) {
    // Allow max 10 attempts per minute
    return attempts < 10;
}
```

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
- [ ] TLS enabled for signaling
- [ ] Connection timeouts configured
- [ ] Reconnection strategy implemented
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
