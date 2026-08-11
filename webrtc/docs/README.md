# WebRTC DataChannel Module

WebRTC DataChannel implementation for peer-to-peer communication.

WebRTC DataChannel 实现，用于点对点通信。

## Status

**Status**: Development preview; not production-ready.

The repository contains the in-tree PeerConnection, TurboNet::Ice, DTLS-SRTP,
DataChannel, RTP/RTCP, SFU and room-service building blocks. Local unit and
loopback end-to-end tests exercise those paths, but they do not establish
production readiness.

The current tree includes authenticated WHIP/WHEP resource endpoints, real ICE
restart and consent freshness, externally configured SFU STUN/TURN servers,
and in-process WSS/HTTPS using CoroNet and Iris. The SFU accepts short-lived
HS256 tokens with audience, scope, room/participant binding and one-key
rotation overlap; static bearer tokens remain an explicit migration mode.
Room Service validates the same contract on its control/facade routes and
mints independent, short-lived tokens for each Room-to-SFU command. The
signaling management plane validates the same contract with route-specific
read/write/dangerous scopes. WebSocket peer admission validates a dedicated
join scope and exact room/peer application identity before admitting a
connection. Signed-token trust domains also support bounded exact-token
SHA-256 revocation lists loaded at startup. Signaling also bounds the
first-join deadline, complete WebSocket
message size, per-connection message rate, copied outbox memory, and
post-upgrade connections per direct source address. There is still no public
browser/TURN acceptance matrix, pre-handshake/trusted-proxy admission policy,
or tenant/distributed quota and dynamic revocation enforcement.
Weak-network, capacity, soak and multi-node failure acceptance remain. See
[Production Readiness](./PRODUCTION_READY_SUMMARY.md) for the evidence and
release gates.

## Documentation

| English | 中文 |
|---------|------|
| [Quick Start](./QUICK_START.md) | [快速开始](./guide-zh.md) |
| [Production Deployment](./PRODUCTION_DEPLOYMENT.md) | [生产部署](./guide-zh.md) |
| [Production Readiness](./PRODUCTION_READY_SUMMARY.md) | - |
| [API Reference](./api-en.md) | [API 参考](./api-zh.md) |
| [Guide](./guide-en.md) | [使用指南](./guide-zh.md) |
| [Architecture](./arch-en.md) | [架构设计](./arch-zh.md) |
| - | [独立 IVR Worker 架构](./ivr-worker-architecture-zh.md) |
| - | [IVR 生产化缺口解决方案](./ivr-production-readiness-design-zh.md) |
| - | [IVR 生产化 TODO Checklist](./ivr-production-readiness-checklist-zh.md) |
| - | [IVR 容量报告与告警 Runbook](./ivr-capacity-and-operations-zh.md) |

## Features

### ✅ Core Features
- **DataChannel**: P2P data transfer with SCTP
- **ICE Integration**: Complete NAT traversal with STUN/TURN
- **DTLS Encryption**: Secure end-to-end encryption
- **Media Support**: Audio/video with Opus/VP8/VP9
- **SDP Parser**: High-performance re2c-based parser
- **Signaling Server**: WebSocket-based signaling

### Implemented but not production-qualified

- **Connection Timeout**: Configurable initial connection timeout
- **ICE Candidate Trickle**: Candidate callbacks and remote candidate import
- **STUN/TURN Configuration**: Multiple server configuration
- **State Monitoring**: Peer and transport callbacks
- **Browser Examples**: Manual interoperability examples, not an acceptance matrix

The legacy `ice_integration_reconnect()` helper remains a retry scheduler.
Applications needing real restart should use
`turbo_peer_connection_restart_ice()` or the conditional WHIP/WHEP trickle-ICE
resource flow; both rotate credentials through the versioned TurboNet::Ice
restart contract.

## Quick Start

```c
#include "turbo_datachannel.h"

// Create context (client mode)
turbo_dc_config_t config = {
    .is_server = 0,
    .transport = TURBO_DC_TRANSPORT_UDP
};
turbo_dc_context_t *ctx = turbo_dc_context_create(&config);

// Create peer
turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, "127.0.0.1", 5000, NULL);

// Set callbacks
turbo_dc_peer_on_state(peer, on_state_change);
turbo_dc_peer_on_channel(peer, on_incoming_channel);

// Connect
turbo_dc_peer_connect(peer);

// Create channel after connected
turbo_dc_channel_t *ch = turbo_dc_channel_create(peer, "chat", NULL);
turbo_dc_channel_on_open(ch, on_open);
turbo_dc_channel_on_message(ch, on_message);
turbo_dc_channel_open(ch);

// Send data
turbo_dc_channel_send(ch, "Hello!", 6, 0);

// Cleanup
turbo_dc_channel_close(ch);
turbo_dc_peer_destroy(peer);
turbo_dc_context_destroy(ctx);
```

## Protocol Stack

```
Application
    ↓
DataChannel API
    ↓
SCTP (usrsctp) - Reliable/unreliable messaging
    ↓
DTLS (BoringSSL) - Encryption
    ↓
Transport: UDP / TCP / KCP / ICE
```

## Transport Modes

| Transport | Use Case |
|-----------|----------|
| UDP (default) | Direct connection, same network |
| TCP | Firewall-friendly |
| KCP | Lossy networks, reliability needed |
| ICE | NAT traversal, different networks |
