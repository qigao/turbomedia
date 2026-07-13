# WebRTC DataChannel Module

WebRTC DataChannel implementation for peer-to-peer communication.

WebRTC DataChannel 实现，用于点对点通信。

## 🎉 Production Ready

**Status**: ✅ Production Ready (v1.0.0)

The WebRTC module is now production-ready with complete ICE integration, STUN/TURN support, automatic reconnection, and full browser interoperability.

## Documentation

| English | 中文 |
|---------|------|
| [Quick Start](./QUICK_START.md) | [快速开始](./guide-zh.md) |
| [Production Deployment](./PRODUCTION_DEPLOYMENT.md) | [生产部署](./guide-zh.md) |
| [Production Ready Summary](./PRODUCTION_READY_SUMMARY.md) | - |
| [API Reference](./api-en.md) | [API 参考](./api-zh.md) |
| [Guide](./guide-en.md) | [使用指南](./guide-zh.md) |
| [Architecture](./arch-en.md) | [架构设计](./arch-zh.md) |

## Features

### ✅ Core Features
- **DataChannel**: P2P data transfer with SCTP
- **ICE Integration**: Complete NAT traversal with STUN/TURN
- **DTLS Encryption**: Secure end-to-end encryption
- **Media Support**: Audio/video with Opus/VP8/VP9
- **SDP Parser**: High-performance re2c-based parser
- **Signaling Server**: WebSocket-based signaling

### ✅ Production Features
- **Connection Timeout**: Configurable timeout handling
- **Automatic Reconnection**: Exponential backoff strategy
- **ICE Candidate Trickle**: Real-time candidate exchange
- **Browser Interoperability**: Chrome/Firefox/Edge compatible
- **STUN/TURN Support**: Multiple server configuration
- **State Monitoring**: Comprehensive callbacks

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
DTLS (OpenSSL) - Encryption
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
