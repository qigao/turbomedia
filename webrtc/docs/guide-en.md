# WebRTC DataChannel Usage Guide

## Simple Echo Example

### Server

```c
#include "turbo_datachannel.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    turbo_dc_context_t *ctx;
    turbo_dc_peer_t *peer;
    turbo_dc_channel_t *channel;
    int running;
} app_t;

void on_message(turbo_dc_channel_t *channel, const void *data,
                size_t len, int is_binary, void *user_data) {
    printf("Received: %.*s\n", (int)len, (const char *)data);

    // Echo back
    turbo_dc_channel_send(channel, data, len, is_binary);
}

void on_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel,
                void *user_data) {
    app_t *app = (app_t *)user_data;
    printf("New channel: %s\n", turbo_dc_channel_get_label(channel));

    app->channel = channel;
    turbo_dc_channel_on_message(channel, on_message);
}

void on_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
              turbo_dc_state_t new_state, void *user_data) {
    app_t *app = (app_t *)user_data;

    if (new_state == TURBO_DC_STATE_CONNECTED) {
        printf("Client connected!\n");
    } else if (new_state == TURBO_DC_STATE_CLOSED) {
        printf("Client disconnected\n");
        app->running = 0;
    }
}

int main(void) {
    app_t app = {0};
    app.running = 1;

    // Create context as server
    turbo_dc_config_t config = {
        .is_server = 1,
        .transport = TURBO_DC_TRANSPORT_UDP
    };
    app.ctx = turbo_dc_context_create(&config);

    // Create peer listening on port 5000
    app.peer = turbo_dc_peer_create(app.ctx, "0.0.0.0", 5000, &app);
    turbo_dc_peer_on_state(app.peer, on_state);
    turbo_dc_peer_on_channel(app.peer, on_channel);

    // Start listening
    turbo_dc_peer_connect(app.peer);
    printf("Server listening on port 5000...\n");

    // Event loop
    while (app.running) {
        turbo_dc_handle_timers();
        usleep(10000);  // 10ms
    }

    turbo_dc_peer_destroy(app.peer);
    turbo_dc_context_destroy(app.ctx);
    return 0;
}
```

### Client

```c
#include "turbo_datachannel.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    turbo_dc_context_t *ctx;
    turbo_dc_peer_t *peer;
    turbo_dc_channel_t *channel;
    int running;
} app_t;

void on_message(turbo_dc_channel_t *channel, const void *data,
                size_t len, int is_binary, void *user_data) {
    printf("Echo: %.*s\n", (int)len, (const char *)data);
}

void on_open(turbo_dc_channel_t *channel, void *user_data) {
    printf("Channel opened! Sending message...\n");
    turbo_dc_channel_send(channel, "Hello Server!", 13, 0);
}

void on_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
              turbo_dc_state_t new_state, void *user_data) {
    app_t *app = (app_t *)user_data;

    if (new_state == TURBO_DC_STATE_CONNECTED) {
        printf("Connected! Creating channel...\n");

        // Create and open channel
        app->channel = turbo_dc_channel_create(app->peer, "echo", NULL);
        turbo_dc_channel_on_open(app->channel, on_open);
        turbo_dc_channel_on_message(app->channel, on_message);
        turbo_dc_channel_open(app->channel);
    } else if (new_state == TURBO_DC_STATE_FAILED) {
        printf("Connection failed\n");
        app->running = 0;
    }
}

int main(void) {
    app_t app = {0};
    app.running = 1;

    // Create context as client
    turbo_dc_config_t config = {
        .is_server = 0,
        .transport = TURBO_DC_TRANSPORT_UDP
    };
    app.ctx = turbo_dc_context_create(&config);

    // Create peer connecting to server
    app.peer = turbo_dc_peer_create(app.ctx, "127.0.0.1", 5000, &app);
    turbo_dc_peer_on_state(app.peer, on_state);

    // Connect
    turbo_dc_peer_connect(app.peer);
    printf("Connecting to server...\n");

    // Event loop
    while (app.running) {
        turbo_dc_handle_timers();
        usleep(10000);
    }

    turbo_dc_peer_destroy(app.peer);
    turbo_dc_context_destroy(app.ctx);
    return 0;
}
```

---

## Transport Modes

### Direct UDP (Default)

Best for: LAN, same network, low latency

```c
turbo_dc_config_t config = {
    .is_server = 0,
    .transport = TURBO_DC_TRANSPORT_UDP
};
```

### Direct TCP

Best for: Firewall-friendly, guaranteed delivery

```c
turbo_dc_config_t config = {
    .is_server = 0,
    .transport = TURBO_DC_TRANSPORT_TCP
};
```

### KCP (Reliable UDP)

Best for: Lossy networks, mobile, gaming

```c
turbo_dc_config_t config = {
    .is_server = 0,
    .transport = TURBO_DC_TRANSPORT_KCP
};
```

### ICE (NAT Traversal)

Best for: Different networks, behind NAT

```c
// 1. Create ICE agent and gather candidates
turbo_ice_agent_t *ice = ice_agent_create(&ice_config);
ice_agent_gather_candidates(ice);

// 2. Exchange credentials via signaling server
// ... (out of band)

// 3. Wait for ICE connection
ice_agent_start_checks(ice);

// 4. Create DataChannel with ICE
turbo_dc_config_t config = {
    .is_server = !is_offerer,  // Answerer is DTLS server
    .transport = TURBO_DC_TRANSPORT_ICE
};
turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, NULL, 0, &app);

// 5. Connect ICE agent to DataChannel
turbo_dc_peer_set_ice_agent(peer, ice);

// 6. Feed ICE data to DataChannel
void on_ice_data(turbo_ice_agent_t *agent, const void *data,
                 size_t len, void *user_data) {
    turbo_dc_peer_feed_ice_data(peer, data, len);
}
```

---

## Channel Reliability

### Reliable Ordered (Default)

Messages delivered in order, with retransmission.

```c
turbo_dc_channel_config_t config = turbo_dc_default_channel_config();
// config.ordered = 1 (default)
// config.max_retransmits = 0 (unlimited)

turbo_dc_channel_t *ch = turbo_dc_channel_create(peer, "reliable", &config);
```

### Reliable Unordered

Messages may arrive out of order, but all delivered.

```c
turbo_dc_channel_config_t config = {
    .ordered = 0,
    .max_retransmits = 0,  // Unlimited retries
    .max_lifetime_ms = 0
};

turbo_dc_channel_t *ch = turbo_dc_channel_create(peer, "reliable-unordered", &config);
```

### Unreliable (Max Retransmits)

Limited retransmits, then drop.

```c
turbo_dc_channel_config_t config = {
    .ordered = 0,
    .max_retransmits = 3,  // Try 3 times then drop
    .max_lifetime_ms = 0
};

turbo_dc_channel_t *ch = turbo_dc_channel_create(peer, "semi-reliable", &config);
```

### Unreliable (Max Lifetime)

Drop if not sent within time limit.

```c
turbo_dc_channel_config_t config = {
    .ordered = 0,
    .max_retransmits = 0,
    .max_lifetime_ms = 500  // Drop if not sent in 500ms
};

turbo_dc_channel_t *ch = turbo_dc_channel_create(peer, "realtime", &config);
```

---

## Binary vs Text

### Text Messages

```c
const char *msg = "Hello World!";
turbo_dc_channel_send(channel, msg, strlen(msg), 0);  // is_binary = 0
```

### Binary Messages

```c
uint8_t packet[256];
// ... fill packet
turbo_dc_channel_send(channel, packet, sizeof(packet), 1);  // is_binary = 1
```

### Handling Messages

```c
void on_message(turbo_dc_channel_t *channel, const void *data,
                size_t len, int is_binary, void *user_data) {
    if (is_binary) {
        // Handle binary data
        process_binary((const uint8_t *)data, len);
    } else {
        // Handle text data
        printf("Text: %.*s\n", (int)len, (const char *)data);
    }
}
```

---

## Multiple Channels

```c
void on_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
              turbo_dc_state_t new_state, void *user_data) {
    if (new_state == TURBO_DC_STATE_CONNECTED) {
        // Create multiple channels
        turbo_dc_channel_t *chat = turbo_dc_channel_create(peer, "chat", NULL);
        turbo_dc_channel_t *game = turbo_dc_channel_create(peer, "game", NULL);
        turbo_dc_channel_t *voice = turbo_dc_channel_create(peer, "voice", NULL);

        // Set different callbacks
        turbo_dc_channel_on_message(chat, on_chat_message);
        turbo_dc_channel_on_message(game, on_game_update);
        turbo_dc_channel_on_message(voice, on_voice_data);

        // Open all
        turbo_dc_channel_open(chat);
        turbo_dc_channel_open(game);
        turbo_dc_channel_open(voice);
    }
}
```

---

## Event Loop Integration

### With TurboNet Native Loop

```c
int main(void) {
    turbo_loop_t *loop = turbo_loop_create();

    // ... create context, peer, etc.

    while (turbo_loop_alive(loop)) {
        turbo_dc_handle_timers();
        turbo_loop_poll(loop, 10, 1);
    }

    turbo_loop_destroy(loop);
    return 0;
}
```

### Standalone

```c
while (app.running) {
    // Required: process SCTP timers
    turbo_dc_handle_timers();

    // Your application logic here
    // ...

    // Sleep to avoid busy loop
    usleep(10000);  // 10ms
}
```

---

## Error Handling

```c
void on_error(turbo_dc_peer_t *peer, int error_code,
              const char *error_msg, void *user_data) {
    fprintf(stderr, "Error %d: %s\n", error_code, error_msg);

    // Check error type
    turbo_dc_error_t err = turbo_dc_peer_get_error(peer);
    fprintf(stderr, "Detailed: %s\n", turbo_dc_error_string(err.code));

    // Cleanup
    turbo_dc_peer_close(peer);
}

// Set error callback
turbo_dc_peer_on_error(peer, on_error);
```

---

## Best Practices

### 1. Call handle_timers() Regularly

```c
// Every 10ms is recommended
while (running) {
    turbo_dc_handle_timers();
    // ... other logic
    usleep(10000);
}
```

### 2. Set All Callbacks Before Connect

```c
turbo_dc_peer_on_state(peer, on_state);
turbo_dc_peer_on_channel(peer, on_channel);
turbo_dc_peer_on_error(peer, on_error);
turbo_dc_peer_connect(peer);  // Set callbacks first!
```

### 3. Check Channel State Before Send

```c
if (turbo_dc_channel_is_open(channel)) {
    turbo_dc_channel_send(channel, data, len, 1);
}
```

### 4. Proper Cleanup Order

```c
// 1. Close channels first
turbo_dc_channel_close(channel);

// 2. Destroy peer
turbo_dc_peer_destroy(peer);

// 3. Destroy context last
turbo_dc_context_destroy(ctx);
```
