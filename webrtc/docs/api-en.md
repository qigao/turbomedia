# WebRTC DataChannel API Reference

## Table of Contents

- [Context API](#context-api)
- [Peer API](#peer-api)
- [Channel API](#channel-api)
- [Types and Enums](#types-and-enums)
- [Callbacks](#callbacks)

---

## Context API

### turbo_dc_context_create

Creates a DataChannel context. Must be called before creating peers.

```c
turbo_dc_context_t *turbo_dc_context_create(const turbo_dc_config_t *config);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `config` | `turbo_dc_config_t *` | Configuration options |

**Configuration:**
```c
typedef struct {
    const char *cert_pem;         // PEM certificate (NULL for auto-generate)
    const char *key_pem;          // PEM private key (NULL for auto-generate)
    int is_server;                // 1 = server/answerer, 0 = client/offerer
    turbo_dc_transport_t transport; // UDP, TCP, KCP, or ICE
    uint16_t sctp_mtu;            // SCTP MTU (0 = default 1188)
    uint16_t dtls_mtu;            // DTLS MTU (0 = default 1280)
} turbo_dc_config_t;
```

**Returns:**
- `turbo_dc_context_t *` - Context handle, or `NULL` on failure

**Example:**
```c
turbo_dc_config_t config = {
    .is_server = 0,
    .transport = TURBO_DC_TRANSPORT_UDP,
    .cert_pem = NULL,  // Auto-generate self-signed cert
    .key_pem = NULL
};

turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
if (!ctx) {
    fprintf(stderr, "Failed to create context\n");
    return 1;
}
```

---

### turbo_dc_context_destroy

Destroys context and all associated peers.

```c
void turbo_dc_context_destroy(turbo_dc_context_t *ctx);
```

---

### turbo_dc_global_cleanup

Forces global SCTP cleanup. Normally cleanup is automatic.

```c
void turbo_dc_global_cleanup(void);
```

---

## Peer API

### turbo_dc_peer_create

Creates a peer connection.

```c
turbo_dc_peer_t *turbo_dc_peer_create(
    turbo_dc_context_t *ctx,
    const char *remote_host,
    uint16_t remote_port,
    void *user_data
);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `ctx` | `turbo_dc_context_t *` | Context handle |
| `remote_host` | `const char *` | Remote host (NULL for server) |
| `remote_port` | `uint16_t` | Remote port (0 for server listen port) |
| `user_data` | `void *` | User data for callbacks |

**Returns:**
- `turbo_dc_peer_t *` - Peer handle, or `NULL` on failure

---

### turbo_dc_peer_on_state

Sets state change callback.

```c
void turbo_dc_peer_on_state(turbo_dc_peer_t *peer, turbo_dc_state_cb cb);
```

---

### turbo_dc_peer_on_channel

Sets incoming channel callback.

```c
void turbo_dc_peer_on_channel(turbo_dc_peer_t *peer, turbo_dc_channel_cb cb);
```

---

### turbo_dc_peer_on_error

Sets error callback.

```c
void turbo_dc_peer_on_error(turbo_dc_peer_t *peer, turbo_dc_error_cb cb);
```

---

### turbo_dc_peer_set_ice_agent

Sets ICE agent for NAT traversal (ICE transport mode only).

```c
int turbo_dc_peer_set_ice_agent(
    turbo_dc_peer_t *peer,
    struct turbo_ice_agent_s *ice_agent
);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `peer` | `turbo_dc_peer_t *` | Peer handle |
| `ice_agent` | `turbo_ice_agent_s *` | Pre-connected ICE agent |

**Returns:**
- `0` - Success
- `-1` - Invalid parameters
- `-2` - Transport already set

---

### turbo_dc_peer_feed_ice_data

Feeds data from ICE agent to DataChannel.

```c
void turbo_dc_peer_feed_ice_data(
    turbo_dc_peer_t *peer,
    const void *data,
    size_t len
);
```

Call this from ICE agent's `on_data` callback.

---

### turbo_dc_peer_connect

Starts connection (client) or listening (server).

```c
int turbo_dc_peer_connect(turbo_dc_peer_t *peer);
```

**Returns:**
- `0` - Success
- `-1` - Error

---

### turbo_dc_peer_get_state

Gets current connection state.

```c
turbo_dc_state_t turbo_dc_peer_get_state(turbo_dc_peer_t *peer);
```

---

### turbo_dc_peer_close

Closes peer connection.

```c
void turbo_dc_peer_close(turbo_dc_peer_t *peer);
```

---

### turbo_dc_peer_destroy

Destroys peer and frees resources.

```c
void turbo_dc_peer_destroy(turbo_dc_peer_t *peer);
```

---

## Channel API

### turbo_dc_channel_create

Creates a data channel.

```c
turbo_dc_channel_t *turbo_dc_channel_create(
    turbo_dc_peer_t *peer,
    const char *label,
    const turbo_dc_channel_config_t *config
);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `peer` | `turbo_dc_peer_t *` | Peer handle |
| `label` | `const char *` | Channel label/name |
| `config` | `turbo_dc_channel_config_t *` | Config (NULL for defaults) |

**Channel Configuration:**
```c
typedef struct {
    int ordered;          // 1 = ordered delivery (default)
    int max_retransmits;  // Max retransmits (0 = unlimited)
    int max_lifetime_ms;  // Max packet lifetime in ms (0 = unlimited)
    const char *protocol; // Optional sub-protocol
} turbo_dc_channel_config_t;
```

**Returns:**
- `turbo_dc_channel_t *` - Channel handle, or `NULL` on failure

---

### turbo_dc_channel_open

Opens the channel (sends DCEP OPEN message).

```c
int turbo_dc_channel_open(turbo_dc_channel_t *channel);
```

**Returns:**
- `0` - Success
- `-1` - Error (peer not connected)

---

### turbo_dc_channel_on_open

Sets channel open callback.

```c
void turbo_dc_channel_on_open(turbo_dc_channel_t *channel, turbo_dc_open_cb cb);
```

---

### turbo_dc_channel_on_message

Sets message received callback.

```c
void turbo_dc_channel_on_message(turbo_dc_channel_t *channel, turbo_dc_message_cb cb);
```

---

### turbo_dc_channel_on_close

Sets channel close callback.

```c
void turbo_dc_channel_on_close(turbo_dc_channel_t *channel, turbo_dc_close_cb cb);
```

---

### turbo_dc_channel_send

Sends data on channel.

```c
int turbo_dc_channel_send(
    turbo_dc_channel_t *channel,
    const void *data,
    size_t len,
    int is_binary
);
```

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `channel` | `turbo_dc_channel_t *` | Channel handle |
| `data` | `const void *` | Data to send |
| `len` | `size_t` | Data length |
| `is_binary` | `int` | 1 = binary, 0 = text |

**Returns:**
- `0` - Success
- `-1` - Error

---

### turbo_dc_channel_get_label

Gets channel label.

```c
const char *turbo_dc_channel_get_label(turbo_dc_channel_t *channel);
```

---

### turbo_dc_channel_get_id

Gets channel ID.

```c
uint16_t turbo_dc_channel_get_id(turbo_dc_channel_t *channel);
```

---

### turbo_dc_channel_is_open

Checks if channel is open.

```c
int turbo_dc_channel_is_open(turbo_dc_channel_t *channel);
```

---

### turbo_dc_channel_buffered_amount

Gets buffered amount (bytes waiting to send).

```c
size_t turbo_dc_channel_buffered_amount(turbo_dc_channel_t *channel);
```

---

### turbo_dc_channel_close

Closes the channel.

```c
void turbo_dc_channel_close(turbo_dc_channel_t *channel);
```

---

## Utility Functions

### turbo_dc_default_channel_config

Gets default channel configuration.

```c
turbo_dc_channel_config_t turbo_dc_default_channel_config(void);
```

**Returns:**
```c
{
    .ordered = 1,
    .max_retransmits = 0,
    .max_lifetime_ms = 0,
    .protocol = NULL
}
```

---

### turbo_dc_handle_timers

Processes SCTP internal timers. **Must be called periodically (~10ms).**

```c
void turbo_dc_handle_timers(void);
```

---

## Types and Enums

### turbo_dc_transport_t

```c
typedef enum {
    TURBO_DC_TRANSPORT_UDP = 0,   // Direct UDP (default)
    TURBO_DC_TRANSPORT_TCP,       // Direct TCP
    TURBO_DC_TRANSPORT_KCP,       // Reliable UDP
    TURBO_DC_TRANSPORT_ICE        // NAT traversal
} turbo_dc_transport_t;
```

### turbo_dc_state_t

```c
typedef enum {
    TURBO_DC_STATE_NEW = 0,
    TURBO_DC_STATE_CONNECTING,
    TURBO_DC_STATE_CONNECTED,
    TURBO_DC_STATE_DISCONNECTING,
    TURBO_DC_STATE_CLOSED,
    TURBO_DC_STATE_FAILED
} turbo_dc_state_t;
```

---

## Callbacks

### turbo_dc_state_cb

```c
typedef void (*turbo_dc_state_cb)(
    turbo_dc_peer_t *peer,
    turbo_dc_state_t old_state,
    turbo_dc_state_t new_state,
    void *user_data
);
```

### turbo_dc_channel_cb

```c
typedef void (*turbo_dc_channel_cb)(
    turbo_dc_peer_t *peer,
    turbo_dc_channel_t *channel,
    void *user_data
);
```

### turbo_dc_open_cb

```c
typedef void (*turbo_dc_open_cb)(
    turbo_dc_channel_t *channel,
    void *user_data
);
```

### turbo_dc_message_cb

```c
typedef void (*turbo_dc_message_cb)(
    turbo_dc_channel_t *channel,
    const void *data,
    size_t len,
    int is_binary,
    void *user_data
);
```

### turbo_dc_close_cb

```c
typedef void (*turbo_dc_close_cb)(
    turbo_dc_channel_t *channel,
    void *user_data
);
```

### turbo_dc_error_cb

```c
typedef void (*turbo_dc_error_cb)(
    turbo_dc_peer_t *peer,
    int error_code,
    const char *error_msg,
    void *user_data
);
```
