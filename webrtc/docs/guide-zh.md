# WebRTC DataChannel 使用指南

## 简单回显示例

### 服务端

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
    printf("收到消息: %.*s\n", (int)len, (const char *)data);

    // 回显
    turbo_dc_channel_send(channel, data, len, is_binary);
}

void on_channel(turbo_dc_peer_t *peer, turbo_dc_channel_t *channel,
                void *user_data) {
    app_t *app = (app_t *)user_data;
    printf("新通道: %s\n", turbo_dc_channel_get_label(channel));

    app->channel = channel;
    turbo_dc_channel_on_message(channel, on_message);
}

void on_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
              turbo_dc_state_t new_state, void *user_data) {
    app_t *app = (app_t *)user_data;

    if (new_state == TURBO_DC_STATE_CONNECTED) {
        printf("客户端已连接！\n");
    } else if (new_state == TURBO_DC_STATE_CLOSED) {
        printf("客户端已断开\n");
        app->running = 0;
    }
}

int main(void) {
    app_t app = {0};
    app.running = 1;

    // 创建服务端上下文
    turbo_dc_config_t config = {
        .is_server = 1,
        .transport = TURBO_DC_TRANSPORT_UDP
    };
    app.ctx = turbo_dc_context_create(&config);

    // 创建监听 5000 端口的对端
    app.peer = turbo_dc_peer_create(app.ctx, "0.0.0.0", 5000, &app);
    turbo_dc_peer_on_state(app.peer, on_state);
    turbo_dc_peer_on_channel(app.peer, on_channel);

    // 开始监听
    turbo_dc_peer_connect(app.peer);
    printf("服务器监听端口 5000...\n");

    // 事件循环
    while (app.running) {
        turbo_dc_handle_timers();
        usleep(10000);  // 10ms
    }

    turbo_dc_peer_destroy(app.peer);
    turbo_dc_context_destroy(app.ctx);
    return 0;
}
```

### 客户端

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
    printf("回显: %.*s\n", (int)len, (const char *)data);
}

void on_open(turbo_dc_channel_t *channel, void *user_data) {
    printf("通道已打开！发送消息...\n");
    turbo_dc_channel_send(channel, "你好，服务器！", 21, 0);
}

void on_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
              turbo_dc_state_t new_state, void *user_data) {
    app_t *app = (app_t *)user_data;

    if (new_state == TURBO_DC_STATE_CONNECTED) {
        printf("已连接！创建通道...\n");

        // 创建并打开通道
        app->channel = turbo_dc_channel_create(app->peer, "echo", NULL);
        turbo_dc_channel_on_open(app->channel, on_open);
        turbo_dc_channel_on_message(app->channel, on_message);
        turbo_dc_channel_open(app->channel);
    } else if (new_state == TURBO_DC_STATE_FAILED) {
        printf("连接失败\n");
        app->running = 0;
    }
}

int main(void) {
    app_t app = {0};
    app.running = 1;

    // 创建客户端上下文
    turbo_dc_config_t config = {
        .is_server = 0,
        .transport = TURBO_DC_TRANSPORT_UDP
    };
    app.ctx = turbo_dc_context_create(&config);

    // 创建连接到服务器的对端
    app.peer = turbo_dc_peer_create(app.ctx, "127.0.0.1", 5000, &app);
    turbo_dc_peer_on_state(app.peer, on_state);

    // 连接
    turbo_dc_peer_connect(app.peer);
    printf("正在连接服务器...\n");

    // 事件循环
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

## 传输模式

### 直连 UDP（默认）

适用于：局域网、同网络、低延迟

```c
turbo_dc_config_t config = {
    .is_server = 0,
    .transport = TURBO_DC_TRANSPORT_UDP
};
```

### 直连 TCP

适用于：防火墙友好、保证送达

```c
turbo_dc_config_t config = {
    .is_server = 0,
    .transport = TURBO_DC_TRANSPORT_TCP
};
```

### KCP（可靠 UDP）

适用于：弱网环境、移动网络、游戏

```c
turbo_dc_config_t config = {
    .is_server = 0,
    .transport = TURBO_DC_TRANSPORT_KCP
};
```

### ICE（NAT 穿透）

适用于：不同网络、NAT 后

```c
// 1. 创建 ICE 代理并收集候选地址
turbo_ice_agent_t *ice = ice_agent_create(&ice_config);
ice_agent_gather_candidates(ice);

// 2. 通过信令服务器交换凭据
// ... (带外)

// 3. 等待 ICE 连接
ice_agent_start_checks(ice);

// 4. 创建带 ICE 的 DataChannel
turbo_dc_config_t config = {
    .is_server = !is_offerer,  // 应答方是 DTLS 服务端
    .transport = TURBO_DC_TRANSPORT_ICE
};
turbo_dc_context_t *ctx = turbo_dc_context_create(&config);
turbo_dc_peer_t *peer = turbo_dc_peer_create(ctx, NULL, 0, &app);

// 5. 连接 ICE 代理到 DataChannel
turbo_dc_peer_set_ice_agent(peer, ice);

// 6. 将 ICE 数据传递给 DataChannel
void on_ice_data(turbo_ice_agent_t *agent, const void *data,
                 size_t len, void *user_data) {
    turbo_dc_peer_feed_ice_data(peer, data, len);
}
```

---

## 通道可靠性

### 可靠有序（默认）

消息按顺序送达，带重传。

```c
turbo_dc_channel_config_t config = turbo_dc_default_channel_config();
// config.ordered = 1 (默认)
// config.max_retransmits = 0 (无限)

turbo_dc_channel_t *ch = turbo_dc_channel_create(peer, "reliable", &config);
```

### 可靠无序

消息可能乱序到达，但全部送达。

```c
turbo_dc_channel_config_t config = {
    .ordered = 0,
    .max_retransmits = 0,  // 无限重试
    .max_lifetime_ms = 0
};

turbo_dc_channel_t *ch = turbo_dc_channel_create(peer, "reliable-unordered", &config);
```

### 不可靠（最大重传次数）

有限重传，超过后丢弃。

```c
turbo_dc_channel_config_t config = {
    .ordered = 0,
    .max_retransmits = 3,  // 重试 3 次后丢弃
    .max_lifetime_ms = 0
};

turbo_dc_channel_t *ch = turbo_dc_channel_create(peer, "semi-reliable", &config);
```

### 不可靠（最大生存时间）

超时未送达则丢弃。

```c
turbo_dc_channel_config_t config = {
    .ordered = 0,
    .max_retransmits = 0,
    .max_lifetime_ms = 500  // 500ms 内未送达则丢弃
};

turbo_dc_channel_t *ch = turbo_dc_channel_create(peer, "realtime", &config);
```

---

## 二进制 vs 文本

### 文本消息

```c
const char *msg = "你好世界！";
turbo_dc_channel_send(channel, msg, strlen(msg), 0);  // is_binary = 0
```

### 二进制消息

```c
uint8_t packet[256];
// ... 填充数据包
turbo_dc_channel_send(channel, packet, sizeof(packet), 1);  // is_binary = 1
```

### 处理消息

```c
void on_message(turbo_dc_channel_t *channel, const void *data,
                size_t len, int is_binary, void *user_data) {
    if (is_binary) {
        // 处理二进制数据
        process_binary((const uint8_t *)data, len);
    } else {
        // 处理文本数据
        printf("文本: %.*s\n", (int)len, (const char *)data);
    }
}
```

---

## 多通道

```c
void on_state(turbo_dc_peer_t *peer, turbo_dc_state_t old_state,
              turbo_dc_state_t new_state, void *user_data) {
    if (new_state == TURBO_DC_STATE_CONNECTED) {
        // 创建多个通道
        turbo_dc_channel_t *chat = turbo_dc_channel_create(peer, "chat", NULL);
        turbo_dc_channel_t *game = turbo_dc_channel_create(peer, "game", NULL);
        turbo_dc_channel_t *voice = turbo_dc_channel_create(peer, "voice", NULL);

        // 设置不同回调
        turbo_dc_channel_on_message(chat, on_chat_message);
        turbo_dc_channel_on_message(game, on_game_update);
        turbo_dc_channel_on_message(voice, on_voice_data);

        // 全部打开
        turbo_dc_channel_open(chat);
        turbo_dc_channel_open(game);
        turbo_dc_channel_open(voice);
    }
}
```

---

## 事件循环集成

### 使用 TurboNet 原生循环

```c
int main(void) {
    turbo_loop_t *loop = turbo_loop_create();

    // ... 创建上下文、对端等

    while (turbo_loop_alive(loop)) {
        turbo_dc_handle_timers();
        turbo_loop_poll(loop, 10, 1);
    }

    turbo_loop_destroy(loop);
    return 0;
}
```

### 独立事件循环

```c
while (app.running) {
    // 必需：处理 SCTP 定时器
    turbo_dc_handle_timers();

    // 你的应用逻辑
    // ...

    // 休眠避免忙等待
    usleep(10000);  // 10ms
}
```

---

## 错误处理

```c
void on_error(turbo_dc_peer_t *peer, int error_code,
              const char *error_msg, void *user_data) {
    fprintf(stderr, "错误 %d: %s\n", error_code, error_msg);

    // 检查错误类型
    turbo_dc_error_t err = turbo_dc_peer_get_error(peer);
    fprintf(stderr, "详细: %s\n", turbo_dc_error_string(err.code));

    // 清理
    turbo_dc_peer_close(peer);
}

// 设置错误回调
turbo_dc_peer_on_error(peer, on_error);
```

---

## 最佳实践

### 1. 定期调用 handle_timers()

```c
// 建议每 10ms 调用一次
while (running) {
    turbo_dc_handle_timers();
    // ... 其他逻辑
    usleep(10000);
}
```

### 2. 连接前设置所有回调

```c
turbo_dc_peer_on_state(peer, on_state);
turbo_dc_peer_on_channel(peer, on_channel);
turbo_dc_peer_on_error(peer, on_error);
turbo_dc_peer_connect(peer);  // 先设置回调！
```

### 3. 发送前检查通道状态

```c
if (turbo_dc_channel_is_open(channel)) {
    turbo_dc_channel_send(channel, data, len, 1);
}
```

### 4. 正确的清理顺序

```c
// 1. 先关闭通道
turbo_dc_channel_close(channel);

// 2. 销毁对端
turbo_dc_peer_destroy(peer);

// 3. 最后销毁上下文
turbo_dc_context_destroy(ctx);
```
