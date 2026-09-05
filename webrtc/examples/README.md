# WebRTC Examples

This directory contains examples for WebRTC DataChannel, signaling, and browser interop flows.

## Examples

### 1. Simple Echo (`simple_echo`)

A basic P2P echo server/client demonstrating:
- Creating a peer connection
- Opening a data channel
- Sending and receiving text messages

**Usage:**
```bash
# Terminal 1 (server)
./dc_simple_echo server 127.0.0.1 5000

# Terminal 2 (client)
./dc_simple_echo client 127.0.0.1 5000
```

**Expected output:**
```
[CLIENT] Sending: Hello from client!
[SERVER] Received: Hello from client!
[SERVER] Echoing back...
[CLIENT] Received: Hello from client!
```

---

### 2. File Transfer (`file_transfer`)

P2P file transfer with progress tracking, demonstrating:
- Binary data transmission
- Chunked transfer
- Progress reporting
- Ordered delivery

**Usage:**
```bash
# Terminal 1 (receiver)
./dc_file_transfer server 127.0.0.1 5000 output.bin

# Terminal 2 (sender)
./dc_file_transfer client 127.0.0.1 5000 input.bin
```

**Expected output:**
```
[SENDER] Transferring input.bin (1048576 bytes)
[SENDER] Progress: 45.2% (473088/1048576 bytes)

[RECEIVER] Ready to receive...
[RECEIVER] Progress: 45.2% (473088/1048576 bytes)
```

**Performance:**
- Chunk size: 16KB
- Uses ordered delivery for reliability
- Progress updates in real-time

---

### 3. Benchmark (`benchmark`)

Performance testing tool, measuring:
- **Latency:** Round-trip time for small messages (1000 pings)
- **Throughput:** Maximum data rate (10 second test)
- Uses unordered delivery for maximum throughput

**Usage:**
```bash
# Terminal 1 (server)
./dc_benchmark server 127.0.0.1 5000

# Terminal 2 (client)
./dc_benchmark client 127.0.0.1 5000
```

**Expected output:**
```
=== Latency Test Results ===
Pings sent: 1000
Pongs received: 1000
Average RTT: 0.523 ms

=== Throughput Test (10 seconds) ===
[CLIENT] Sent: 1250.00 MB, Rate: 1000.00 Mbps
[SERVER] Received: 1250.00 MB, Rate: 1000.00 Mbps
```

---

### 4. Signaled Peer (`rtc_signaled_peer`)

Room-based WebRTC example demonstrating:
- ICE candidate gathering and trickle exchange
- SDP offer/answer over WebSocket signaling
- DataChannel establishment over ICE
- Current CoroNet WebSocket client APIs instead of the removed legacy client

**Usage:**
```bash
# Terminal 1
./rtc_signaled_peer --offer --room demo --server 127.0.0.1 --port 8080

# Terminal 2
./rtc_signaled_peer --answer --room demo --server 127.0.0.1 --port 8080
```

**Notes:**
- Requires an external signaling server that speaks this repo's JSON room protocol.
- Works with `ws://` by default; add `--secure` for `wss://`.
- When signaling peer admission is enabled, set
  `TURBO_SIGNALING_PEER_TOKEN` and pass `--peer-id alice` (or set
  `TURBO_SIGNALING_PEER_ID`). The token is sent only in the first WebSocket
  `join` message and is never written to the example log.

---

### 5. Browser DataChannel Interop (`browser_interop`)

Browser-facing answerer example demonstrating:
- Browser SDP offer -> native SDP answer flow
- ICE + DTLS + SCTP/DataChannel with Chrome/Edge
- Local smoke automation via `browser_interop_smoke.js`

**Usage:**
```bash
./browser_interop --no-stun
```

Then open `webrtc/examples/browser_interop.html` and paste the SDP offer/answer, or run:

```bash
node webrtc/examples/browser_interop_smoke.js --native build/Msvc-Release/bin/browser_interop.exe
```

---

### 6. Browser Audio/Video Interop (`browser_media_interop`)

Browser-facing native audio/video sender and receiver demonstrating:
- Browser audio+video offer -> native SDP answer flow
- Browser-side Opus audio plus VP8, VP9, or H.264 video smoke coverage
- Browser send mode can use real microphone/camera or synthetic A/V fallback
- DTLS-SRTP media transport over ICE
- Remote browser audio/video track delivery from native RTP
- Local smoke automation via `browser_media_interop_smoke.js` in both native-send and browser-send directions

**Usage:**
```bash
./browser_media_interop --no-stun
```

Then open `webrtc/examples/browser_media_interop.html` and paste the SDP offer/answer, or run:

```bash
node webrtc/examples/browser_media_interop_smoke.js --matrix --native build/Msvc-Release/bin/browser_media_interop.exe
node webrtc/examples/browser_media_interop_smoke.js --native build/Msvc-Release/bin/browser_media_interop.exe
node webrtc/examples/browser_media_interop_smoke.js --codec vp9 --native build/Msvc-Release/bin/browser_media_interop.exe
node webrtc/examples/browser_media_interop_smoke.js --browser-send --codec vp9 --native build/Msvc-Release/bin/browser_media_interop.exe
```

Manual browser-send notes:
- `Send Source = Real Devices if Available` will try microphone/camera first and fall back to synthetic A/V.
- `Send Source = Real Devices Only` is useful when you want to validate real capture permissions and browser device input end-to-end.

---

## Building

Examples are built automatically with the project:

```bash
cmake --build build --target dc_simple_echo
cmake --build build --target dc_file_transfer
cmake --build build --target dc_benchmark
cmake --build build --target rtc_signaled_peer
cmake --build build --target browser_interop
cmake --build build --target browser_media_interop
```

Or build all examples:
```bash
cmake --build build
```

---

## Architecture

All examples use the same pattern:

```c
/* 1. Create context */
turbo_dc_config_t config = {
    .is_server = is_server
};
turbo_dc_context_t *ctx = turbo_dc_context_create(&config);

/* 2. Create peer */
turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, host, port, user_data);

/* 3. Set callbacks */
turbo_dc_peer_on_state(peer, on_peer_state);
turbo_dc_peer_on_channel(peer, on_peer_channel);
turbo_dc_peer_on_error(peer, on_peer_error);

/* 4. Connect */
turbo_dc_peer_connect(peer);

/* 5. Keep the process alive while the internal CoroNet runtime drives I/O */
while (running) {
    salts_sleep_ms(100);
}
```

**Client side:**
- Creates and opens data channel
- Sends data

**Server side:**
- Receives incoming channel
- Handles data

---

## Notes

### Simplified WebRTC

Most examples use a **simplified** WebRTC implementation:
- ✅ DTLS encryption
- ✅ SCTP data channels
- ✅ Ordered/unordered delivery
- ❌ No ICE/STUN/TURN (direct connectivity only)
- ❌ No SDP negotiation
- ❌ No signaling server

**This means:**
- Works great for LAN connections
- Works for known public IPs
- Does NOT work behind NAT without port forwarding

### Future Extensions

To support full WebRTC:
1. Add ICE candidate gathering
2. Add STUN/TURN support
3. Add SDP offer/answer exchange
4. Add signaling server

See `ice/` module for ICE/STUN implementation.

`rtc_signaled_peer` is the exception: it shows the current room-signaling flow with ICE and SDP over WebSocket.

---

## Performance Tips

**For maximum throughput:**
```c
turbo_dc_channel_config_t config = {
    .ordered = 0,           // Unordered delivery
    .max_retransmits = 0    // No retransmissions
};
```

**For reliability:**
```c
turbo_dc_channel_config_t config = {
    .ordered = 1,           // Ordered delivery
    .max_retransmits = 0    // Unlimited retransmissions
};
```

**For real-time (voice/video):**
```c
turbo_dc_channel_config_t config = {
    .ordered = 0,               // Unordered
    .max_lifetime_ms = 100      // Drop old packets
};
```

---

## Troubleshooting

**Connection fails:**
- Check firewall settings
- Verify port is not in use: `netstat -an | grep 5000`
- Try different port number

**安全上下文创建失败：**
- 确认上游 TurboNet、TurboHTTP 与 WebRTC 依赖包来自同一安装前缀
- BoringSSL 由上游包提供；TurboMedia 不执行额外配置或运行时校验

**SCTP errors:**
- Missing usrsctp library
- Check vcpkg: `vcpkg list | grep sctp`

---

## License

MIT License - See project LICENSE file
