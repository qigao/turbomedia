# TurboMedia 快速入门指南

## 概述

TurboMedia 是一个模块化的多媒体处理框架，提供：
- **Codec**: 音视频编解码（H.264/H.265/Opus/VP8/VP9）
- **Muxer/Demuxer**: 容器封装/解封装（FLV/MP4/MKV/MPEG-TS）
- **Streamer**: 流媒体协议（HLS/DASH/RTMP/HTTP-FLV）
- **Transport**: 网络传输（CoroNet/TurboHTTP）

## 安装和编译

### 依赖项

```bash
# 必需
- CMake >= 3.20
- C11 编译器

# 可选（根据启用的功能）
- libx264/OpenH264 (H.264)
- x265 + libde265 (H.265)
- libopus (Opus)
- libvpx (VP8/VP9)
- TurboNet (网络传输)
- TurboHTTP (HTTP 客户端)
```

### 编译

```bash
cd turbomedia
cmake --preset win-dev-user
cmake --build --preset win-dev-user
```

## 基础用法

### 1. 视频编码

```c
#include <turbo_codec.h>

/* 初始化 */
turbo_codec_registry_init();

/* 创建 H.264 编码器 */
turbo_video_codec_config_t config = {
    .width = 1920,
    .height = 1080,
    .framerate = 30,
    .bitrate = 5000000,  /* 5 Mbps */
    .keyframe_interval = 60,
    .threads = 4
};

turbo_codec_t *encoder = turbo_codec_create_encoder("h264", &config);

/* 编码帧 */
uint8_t *raw_frame = ...; /* YUV420 数据 */
uint8_t encoded_buffer[100000];
size_t encoded_size = sizeof(encoded_buffer);
turbo_encoded_frame_t frame_info;

int ret = turbo_codec_encode(encoder, 
                             raw_frame, yuv_size,
                             encoded_buffer, &encoded_size,
                             &frame_info);

if (ret == TURBO_CODEC_OK) {
    printf("Encoded %zu bytes, keyframe=%d\n", 
           encoded_size, frame_info.is_keyframe);
}

/* 清理 */
turbo_codec_destroy(encoder);
turbo_codec_registry_shutdown();
```

### 2. FLV 文件封装

```c
#include <turbo_codec.h>
#include <turbo_muxer.h>

/* 初始化 */
turbo_codec_registry_init();
turbo_muxer_registry_init();

/* 创建编码器 */
turbo_codec_t *h264 = turbo_codec_create_encoder("h264", &video_config);
turbo_codec_t *opus = turbo_codec_create_encoder("opus", &audio_config);

/* 创建 FLV muxer */
turbo_muxer_config_t muxer_config = {
    .format = TURBO_MUXER_FLV,
    .output_path = "output.flv"
};

turbo_muxer_t *flv = turbo_muxer_create(&muxer_config);

/* 添加流 */
turbo_stream_info_t video_info = {
    .type = TURBO_CODEC_TYPE_VIDEO,
    .codec_name = "h264",
    .width = 1920,
    .height = 1080,
    .framerate = 30
};

turbo_stream_info_t audio_info = {
    .type = TURBO_CODEC_TYPE_AUDIO,
    .codec_name = "opus",
    .sample_rate = 48000,
    .channels = 2
};

int video_id, audio_id;
turbo_muxer_add_stream(flv, &video_info, &video_id);
turbo_muxer_add_stream(flv, &audio_info, &audio_id);

/* 写入头部 */
turbo_muxer_write_header(flv);

/* 编码并封装 */
for (int i = 0; i < frame_count; i++) {
    /* 编码视频 */
    turbo_codec_encode(h264, ...);
    
    /* 封装 */
    turbo_muxer_packet_t packet = {
        .stream_id = video_id,
        .data = encoded_buffer,
        .size = encoded_size,
        .pts = i * 1000000 / 30,
        .dts = i * 1000000 / 30,
        .is_keyframe = frame_info.is_keyframe
    };
    
    turbo_muxer_write_packet(flv, &packet);
}

/* 写入尾部 */
turbo_muxer_write_trailer(flv);

/* 清理 */
turbo_muxer_destroy(flv);
turbo_codec_destroy(h264);
turbo_codec_destroy(opus);
```

### 3. HLS 直播推流（协程版本）

```c
#include <turbo_codec.h>
#include <turbo_streamer.h>
#include <CoroNet/turbo_coro_context.h>
#include <http_client.h>

/* 流式传输协程 */
static void streaming_coroutine(void *arg) {
    turbo_streamer_t *streamer = (turbo_streamer_t *)arg;
    
    /* 连接 */
    turbo_streamer_connect(streamer);
    
    /* 推流循环 */
    for (int i = 0; i < frame_count; i++) {
        /* 编码 */
        turbo_codec_encode(...);
        
        /* 推送到 HLS */
        turbo_muxer_packet_t packet = {...};
        turbo_streamer_write_packet(streamer, &packet);
    }
    
    /* 断开 */
    turbo_streamer_disconnect(streamer);
}

int main() {
    /* 初始化 */
    turbo_codec_registry_init();
    turbo_streamer_registry_init();
    
    /* 创建协程上下文 */
    coro_context_t *ctx = coro_context_create();
    
    /* 创建 HTTP 客户端（用于上传分片）*/
    http_client_t *http = http_client_create("https://cdn.example.com");
    http_client_set_bearer_token(http, "your_token");
    
    /* 创建 HLS streamer */
    turbo_streamer_config_t config = {
        .protocol = TURBO_STREAMER_HLS,
        .segment_duration_ms = 6000,  /* 6 秒分片 */
        .playlist_size = 5,
        .output_dir = "./hls_output",
        .base_url = "https://cdn.example.com/live",
        .coro_context = ctx,
        .http_client = http
    };
    
    turbo_streamer_t *streamer = turbo_streamer_create(&config);
    
    /* 添加流 */
    turbo_stream_info_t info = {...};
    int stream_id;
    turbo_streamer_add_stream(streamer, &info, &stream_id);
    
    /* 启动协程 */
    coro_create(ctx, streaming_coroutine, streamer);
    
    /* 运行事件循环 */
    coro_context_run(ctx);
    
    /* 清理 */
    turbo_streamer_destroy(streamer);
    http_client_destroy(http);
    coro_context_destroy(ctx);
    
    return 0;
}
```

### 4. RTMP 推流

```c
#include <turbo_codec.h>
#include <turbo_streamer.h>

/* RTMP 推流 */
turbo_streamer_config_t config = {
    .protocol = TURBO_STREAMER_RTMP,
    .url = "rtmp://live.example.com:1935/live/stream_key",
    .chunk_size = 4096
};

turbo_streamer_t *rtmp = turbo_streamer_create(&config);

/* 连接 */
turbo_streamer_connect(rtmp);

/* 添加流 */
turbo_stream_info_t video_info = {...};
turbo_streamer_add_stream(rtmp, &video_info, &stream_id);

/* 推流 */
for (int i = 0; i < frame_count; i++) {
    turbo_muxer_packet_t packet = {...};
    turbo_streamer_write_packet(rtmp, &packet);
}

/* 清理 */
turbo_streamer_disconnect(rtmp);
turbo_streamer_destroy(rtmp);
```

### 5. 文件解析（Demuxer）

```c
#include <turbo_demuxer.h>
#include <turbo_codec.h>

/* 初始化 */
turbo_demuxer_registry_init();
turbo_codec_registry_init();

/* 打开文件（自动探测格式）*/
turbo_demuxer_config_t config = {
    .input_path = "video.mp4"
};

turbo_demuxer_t *demuxer = turbo_demuxer_create(&config);
turbo_demuxer_open(demuxer);

/* 获取流信息 */
int stream_count = turbo_demuxer_get_stream_count(demuxer);
printf("Found %d streams\n", stream_count);

for (int i = 0; i < stream_count; i++) {
    turbo_stream_info_t info;
    turbo_demuxer_get_stream_info(demuxer, i, &info);
    printf("Stream %d: %s, codec=%s\n", 
           i, 
           info.type == TURBO_CODEC_TYPE_VIDEO ? "video" : "audio",
           info.codec_name);
}

/* 创建解码器 */
turbo_stream_info_t video_info;
turbo_demuxer_get_stream_info(demuxer, 0, &video_info);
turbo_codec_t *decoder = turbo_codec_create_decoder(video_info.codec_name, NULL);

/* 读取并解码 */
turbo_demuxer_packet_t packet;
while (turbo_demuxer_read_packet(demuxer, &packet) > 0) {
    if (packet.stream_index == 0) {  /* 视频流 */
        uint8_t decoded_buffer[4000000];
        size_t decoded_size = sizeof(decoded_buffer);
        
        int ret = turbo_codec_decode(decoder,
                                     packet.data, packet.size,
                                     decoded_buffer, &decoded_size);
        
        if (ret == TURBO_CODEC_OK) {
            printf("Decoded frame: %zu bytes\n", decoded_size);
        }
    }
    
    turbo_demuxer_free_packet(&packet);
}

/* 清理 */
turbo_codec_destroy(decoder);
turbo_demuxer_destroy(demuxer);
```

## 高级用法

### 自定义编解码器

```c
/* 实现 turbo_codec_ops_t 接口 */
static void *my_encoder_create(const void *config) {
    /* 创建编码器上下文 */
}

static int my_encode(void *ctx, ...) {
    /* 编码实现 */
}

const turbo_codec_ops_t my_codec_ops = {
    .name = "my_codec",
    .type = TURBO_CODEC_TYPE_VIDEO,
    .create_encoder = my_encoder_create,
    .encode = my_encode,
    /* ... */
};

/* 注册 */
turbo_codec_register(&my_codec_ops);
```

### 事件回调

```c
void streamer_event_handler(turbo_streamer_t *streamer,
                           turbo_streamer_event_t event,
                           void *event_data,
                           void *user_data) {
    switch (event) {
        case TURBO_STREAMER_EVENT_CONNECTED:
            printf("Connected to server\n");
            break;
        case TURBO_STREAMER_EVENT_DISCONNECTED:
            printf("Disconnected\n");
            break;
        case TURBO_STREAMER_EVENT_SEGMENT_READY:
            printf("HLS segment ready: %s\n", (char *)event_data);
            break;
        case TURBO_STREAMER_EVENT_ERROR:
            printf("Error: %s\n", (char *)event_data);
            break;
    }
}

turbo_streamer_set_event_callback(streamer, streamer_event_handler, NULL);
```

## 常见问题

### Q: 如何选择合适的编码器？
A: 
- **H.264**: 兼容性好，广泛支持
- **H.265**: 压缩率高，适合高分辨率
- **VP8/VP9**: 开源免费，WebRTC 友好

### Q: HLS 和 RTMP 有什么区别？
A:
- **HLS**: 基于 HTTP，CDN 友好，延迟较高（6-30秒）
- **RTMP**: 专用协议，延迟低（2-5秒），需要 RTMP 服务器

### Q: 如何减少延迟？
A:
- 使用低延迟编码设置（减少 B 帧）
- 减小 HLS 分片时长（2-4秒）
- 考虑使用 RTMP 或 WebRTC

### Q: 是否支持硬件编码？
A: 是的，移动平台支持：
- **iOS**: VideoToolbox
- **Android**: MediaCodec

## 示例项目

完整示例请参见 `examples/` 目录：
- `streaming_pipeline.c`: 完整的流媒体处理管道
- `codec_g711_smoke.c`: G.711 编解码示例
- `capture_save_image.c`: 摄像头采集示例

## 资源链接

- [架构文档](ARCHITECTURE.md)
- [API 参考](https://docs.turbomedia.io/api)
- [GitHub](https://github.com/turbomedia/turbomedia)

## 许可证

参见项目根目录的 LICENSE 文件。
