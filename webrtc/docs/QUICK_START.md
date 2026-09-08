# WebRTC Quick Start Guide

Get started with TurboMedia WebRTC in 5 minutes.

## Installation

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install cmake ninja-build

# macOS
brew install cmake ninja

# Windows (vcpkg)
vcpkg install boringssl usrsctp
```

The project requires BoringSSL on every platform. Do not install or substitute
OpenSSL; the CMake presets resolve BoringSSL through the vcpkg manifest.

### Build

```bash
git clone https://github.com/qigao/turbomedia.git
cd turbomedia
cmake -B build -G Ninja
cmake --build build
```

## Basic P2P Connection

### Step 1: Create Peers

```c
#include "turbo_datachannel.h"
#include "ice_integration.h"
#include <salts/time.h>

// Create event loop
turbo_loop_t *loop = turbo_loop_create();

// Create peer A (offerer)
turbo_dc_config_t config_a = {
    .is_server = 0,
    .transport = TURBO_DC_TRANSPORT_ICE
};
turbo_dc_context_t *ctx_a = turbo_dc_context_create(&config_a);
turbo_dc_peer_t *peer_a = turbo_dc_peer_create(ctx_a, NULL, 0, NULL);

// Create peer B (answerer)
turbo_dc_config_t config_b = {
    .is_server = 1,
    .transport = TURBO_DC_TRANSPORT_ICE
};
turbo_dc_context_t *ctx_b = turbo_dc_context_create(&config_b);
turbo_dc_peer_t *peer_b = turbo_dc_peer_create(ctx_b, NULL, 0, NULL);
```

### Step 2: Setup ICE

```c
// STUN servers (use Google's public STUN)
const char *stun_servers[] = {
    "stun:stun.l.google.com:19302"
};

// Create ICE integration for peer A
ice_integration_ctx_t *ice_a = ice_integration_create(
    peer_a, loop,
    stun_servers, 1,
    NULL, NULL, NULL, 0
);

// Create ICE integration for peer B
ice_integration_ctx_t *ice_b = ice_integration_create(
    peer_b, loop,
    stun_servers, 1,
    NULL, NULL, NULL, 0
);
```

### Step 3: Exchange Credentials

```c
// Get credentials from peer A
char ufrag_a[32], pwd_a[64];
ice_integration_get_local_credentials(ice_a, ufrag_a, sizeof(ufrag_a), 
                                      pwd_a, sizeof(pwd_a));

// Get credentials from peer B
char ufrag_b[32], pwd_b[64];
ice_integration_get_local_credentials(ice_b, ufrag_b, sizeof(ufrag_b),
                                      pwd_b, sizeof(pwd_b));

// Set remote credentials
ice_integration_set_remote_credentials(ice_a, ufrag_b, pwd_b);
ice_integration_set_remote_credentials(ice_b, ufrag_a, pwd_a);

// Exchange DTLS fingerprints
char fp_hash_a[32], fp_a[128];
char fp_hash_b[32], fp_b[128];
turbo_dc_context_get_local_fingerprint(ctx_a, fp_hash_a, sizeof(fp_hash_a),
                                       fp_a, sizeof(fp_a));
turbo_dc_context_get_local_fingerprint(ctx_b, fp_hash_b, sizeof(fp_hash_b),
                                       fp_b, sizeof(fp_b));

turbo_dc_peer_set_remote_fingerprint(peer_a, fp_hash_b, fp_b);
turbo_dc_peer_set_remote_fingerprint(peer_b, fp_hash_a, fp_a);
```

### Step 4: Setup Callbacks

```c
// ICE candidate trickle
void on_candidate_a(const char *candidate_sdp, void *user_data) {
    // Send to peer B via signaling
    ice_integration_add_remote_candidate(ice_b, candidate_sdp);
}

void on_candidate_b(const char *candidate_sdp, void *user_data) {
    // Send to peer A via signaling
    ice_integration_add_remote_candidate(ice_a, candidate_sdp);
}

ice_integration_on_candidate(ice_a, on_candidate_a, NULL);
ice_integration_on_candidate(ice_b, on_candidate_b, NULL);

// Connection state
void on_peer_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
                   turbo_dc_state_t new_state, void *user_data) {
    if (new_state == TURBO_DC_STATE_CONNECTED) {
        printf("Connected!\n");
    }
}

turbo_dc_peer_on_state(peer_a, on_peer_state);
turbo_dc_peer_on_state(peer_b, on_peer_state);
```

### Step 5: Create DataChannel

```c
// Peer A creates channel
void on_channel_open(turbo_dc_channel_t *channel, void *user_data) {
    printf("Channel opened: %s\n", turbo_dc_channel_get_label(channel));
    
    // Send message
    const char *msg = "Hello!";
    turbo_dc_channel_send(channel, msg, strlen(msg), 0);
}

void on_message(turbo_dc_channel_t *channel, const void *data,
                size_t len, int is_binary, void *user_data) {
    printf("Received: %.*s\n", (int)len, (const char *)data);
}

// Peer A creates channel after connection
turbo_dc_channel_t *channel_a = turbo_dc_channel_create(peer_a, "chat", NULL);
turbo_dc_channel_on_open(channel_a, on_channel_open);
turbo_dc_channel_on_message(channel_a, on_message);

// Peer B receives channel
void on_incoming_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel,
                         void *user_data) {
    printf("Incoming channel: %s\n", turbo_dc_channel_get_label(channel));
    turbo_dc_channel_on_open(channel, on_channel_open);
    turbo_dc_channel_on_message(channel, on_message);
}

turbo_dc_peer_on_channel(peer_b, on_incoming_channel);
```

### Step 6: Start Connection

```c
// Start ICE gathering
ice_integration_start_gathering(ice_a);
ice_integration_start_gathering(ice_b);

// Open channel (after peer A connects)
turbo_dc_channel_open(channel_a);

// Run event loop
while (turbo_loop_alive(loop)) {
    turbo_loop_poll(loop, 50, 1);
}
```

## Complete Example

See `webrtc/examples/ice_datachannel.c` for a complete working example.

## Browser Connection

### C Application

```c
// See webrtc/examples/browser_interop.c
./build/bin/browser_interop
```

### Browser

Open `webrtc/examples/browser_interop.html` and follow instructions.

For a repeatable local Chrome smoke test, build `browser_interop` and run:

```bash
node webrtc/examples/browser_interop_smoke.js --native build/Msvc-Release/bin/browser_interop.exe
```

Pass `--chrome <path>` if Chrome/Edge is not in the default location.

## Browser Video Connection

### Native Sender

```bash
./build/bin/browser_media_interop --no-stun
```

### Browser

Open `webrtc/examples/browser_media_interop.html` and follow the SDP copy/paste flow to receive a native browser-compatible video stream in Chrome/Edge. The example prefers VP8 for smoke testing and falls back to H.264 if needed.

For a repeatable local Chrome smoke test, build `browser_media_interop` and run:

```bash
node webrtc/examples/browser_media_interop_smoke.js --matrix --native build/Msvc-Release/bin/browser_media_interop.exe
node webrtc/examples/browser_media_interop_smoke.js --native build/Msvc-Release/bin/browser_media_interop.exe
node webrtc/examples/browser_media_interop_smoke.js --codec vp9 --native build/Msvc-Release/bin/browser_media_interop.exe
node webrtc/examples/browser_media_interop_smoke.js --browser-send --codec vp9 --native build/Msvc-Release/bin/browser_media_interop.exe
```

Pass `--chrome <path>` if Chrome/Edge is not in the default location.

## Common Patterns

### With Signaling Server

```c
// Connect to signaling server
webrtc_signaling_client_t *client = signaling_client_create(loop);
signaling_client_connect(client, "ws://localhost:8080");

// Join room
signaling_client_join_room(client, "my-room");

// Handle signaling messages
void on_signaling_message(const char *type, const char *data, void *user_data) {
    if (strcmp(type, "candidate") == 0) {
        ice_integration_add_remote_candidate(ice, data);
    }
}

signaling_client_on_message(client, on_signaling_message, NULL);
```

### With TURN Server

```c
const char *turn_servers[] = {
    "turn:turn.example.com:3478"
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

### With Reconnection

```c
// Configure reconnection
ice_integration_set_connection_timeout(ice, 30000);  // 30 seconds
ice_integration_set_max_reconnect_attempts(ice, 5);

// Monitor state
void on_ice_state(ice_state_t state, void *user_data) {
    switch (state) {
        case ICE_STATE_DISCONNECTED:
            printf("Connection lost, reconnecting...\n");
            break;
        case ICE_STATE_CONNECTED:
            printf("Reconnected!\n");
            break;
    }
}

ice_integration_on_state_change(ice, on_ice_state, NULL);
```

## Next Steps

- Read [Production Deployment Guide](PRODUCTION_DEPLOYMENT.md)
- Check [API Reference](api-en.md)
- Explore [Examples](../examples/)
- Run [Tests](../tests/)

## Troubleshooting

### Connection Fails

```bash
# Enable debug logging
export TURBO_LOG_LEVEL=DEBUG
./your_app
```

### Check STUN Server

```bash
# Test STUN server connectivity
nc -u stun.l.google.com 19302
```

### Verify Firewall

```bash
# Allow UDP traffic
sudo ufw allow 3478/udp  # STUN/TURN
sudo ufw allow 49152:65535/udp  # ICE candidates
```

## Support

- Documentation: [docs/](.)
- Examples: [examples/](../examples/)
- Tests: [tests/](../tests/)
- Issues: GitHub Issues
