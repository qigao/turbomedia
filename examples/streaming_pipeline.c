/**
 * TurboMedia Streaming Pipeline 示例
 *
 * 演示完整的流媒体处理管道：
 * 摄像头采集 → H.264编码 → FLV封装 → HLS分片 → HTTP上传
 */
#include <stdio.h>
#include <stdlib.h>

/* TurboMedia 模块 */
#include <salts_capture.h>
#include <turbo_codec.h>
#include <turbo_muxer.h>
#include <turbo_streamer.h>
#include <turbo_transport.h>

/* CoroNet */
#include <CoroNet/turbo_coro_context.h>

/* TurboHTTP */
#include <http_client.h>

/* =============================================================================
 * 示例 1: 本地视频录制为 FLV
 * ============================================================================= */

int example_record_to_flv(void) {
    int ret = 0;
    
    /* 初始化 Codec 和 Muxer 注册表 */
    turbo_codec_registry_init();
    turbo_muxer_registry_init();
    
    /* 1. 创建视频编码器 */
    turbo_video_codec_config_t video_config = {
        .width = 1280,
        .height = 720,
        .framerate = 30,
        .bitrate = 2000000,  /* 2 Mbps */
        .keyframe_interval = 60,
        .threads = 4
    };
    
    turbo_codec_t *h264_encoder = turbo_codec_create_encoder("h264", &video_config);
    if (!h264_encoder) {
        fprintf(stderr, "Failed to create H.264 encoder\n");
        return -1;
    }
    
    /* 2. 创建 FLV muxer */
    turbo_muxer_config_t muxer_config = {
        .format = TURBO_MUXER_FLV,
        .output_path = "output.flv",
        .write_duration = 1,
        .faststart = 0
    };
    
    turbo_muxer_t *flv_muxer = turbo_muxer_create(&muxer_config);
    if (!flv_muxer) {
        fprintf(stderr, "Failed to create FLV muxer\n");
        turbo_codec_destroy(h264_encoder);
        return -1;
    }
    
    /* 3. 添加视频流 */
    turbo_stream_info_t stream_info = {
        .type = TURBO_CODEC_TYPE_VIDEO,
        .codec_name = "h264",
        .width = 1280,
        .height = 720,
        .framerate = 30,
        .extradata = NULL,  /* SPS/PPS 会在编码后获取 */
        .extradata_size = 0
    };
    
    int stream_id;
    ret = turbo_muxer_add_stream(flv_muxer, &stream_info, &stream_id);
    if (ret < 0) {
        fprintf(stderr, "Failed to add video stream\n");
        goto cleanup;
    }
    
    /* 4. 写入头部 */
    ret = turbo_muxer_write_header(flv_muxer);
    if (ret < 0) {
        fprintf(stderr, "Failed to write FLV header\n");
        goto cleanup;
    }
    
    /* 5. 编码和封装循环 */
    printf("Recording to output.flv...\n");
    
    for (int frame = 0; frame < 300; frame++) {  /* 录制 10 秒 @ 30fps */
        /* 这里应该从摄像头获取帧 */
        /* uint8_t *raw_frame = capture_frame(); */
        
        /* 模拟：创建一个空帧 */
        size_t yuv_size = 1280 * 720 * 3 / 2;
        uint8_t *raw_frame = (uint8_t *)calloc(1, yuv_size);
        
        /* 编码 */
        uint8_t encoded_buffer[100000];
        size_t encoded_size = sizeof(encoded_buffer);
        turbo_encoded_frame_t frame_info;
        
        ret = turbo_codec_encode(h264_encoder,
                                raw_frame, yuv_size,
                                encoded_buffer, &encoded_size,
                                &frame_info);
        
        free(raw_frame);
        
        if (ret == TURBO_CODEC_OK && encoded_size > 0) {
            /* 封装到 FLV */
            turbo_muxer_packet_t packet = {
                .stream_id = stream_id,
                .data = encoded_buffer,
                .size = encoded_size,
                .pts = frame * 1000000 / 30,  /* 微秒 */
                .dts = frame * 1000000 / 30,
                .is_keyframe = frame_info.is_keyframe
            };
            
            ret = turbo_muxer_write_packet(flv_muxer, &packet);
            if (ret < 0) {
                fprintf(stderr, "Failed to write packet\n");
                break;
            }
            
            if (frame % 30 == 0) {
                printf("Encoded frame %d\n", frame);
            }
        }
    }
    
    /* 6. 写入尾部 */
    turbo_muxer_write_trailer(flv_muxer);
    
    printf("Recording completed!\n");

cleanup:
    turbo_muxer_destroy(flv_muxer);
    turbo_codec_destroy(h264_encoder);
    turbo_codec_registry_shutdown();
    turbo_muxer_registry_shutdown();
    
    return ret;
}

/* =============================================================================
 * 示例 2: HLS 直播推流（协程版本）
 * ============================================================================= */

typedef struct {
    turbo_codec_t *encoder;
    turbo_streamer_t *streamer;
    int frame_count;
} streaming_context_t;

static void streaming_coroutine(void *arg) {
    streaming_context_t *ctx = (streaming_context_t *)arg;
    
    printf("Streaming coroutine started\n");
    
    /* 连接到 HLS streamer */
    int ret = turbo_streamer_connect(ctx->streamer);
    if (ret < 0) {
        fprintf(stderr, "Failed to connect streamer\n");
        return;
    }
    
    /* 流式传输循环 */
    for (int frame = 0; frame < 1800; frame++) {  /* 1 分钟 @ 30fps */
        /* 模拟获取和编码帧 */
        size_t yuv_size = 1920 * 1080 * 3 / 2;
        uint8_t *raw_frame = (uint8_t *)calloc(1, yuv_size);
        
        uint8_t encoded_buffer[200000];
        size_t encoded_size = sizeof(encoded_buffer);
        turbo_encoded_frame_t frame_info;
        
        ret = turbo_codec_encode(ctx->encoder,
                                raw_frame, yuv_size,
                                encoded_buffer, &encoded_size,
                                &frame_info);
        
        free(raw_frame);
        
        if (ret == TURBO_CODEC_OK && encoded_size > 0) {
            /* 推送到 HLS streamer */
            turbo_muxer_packet_t packet = {
                .stream_id = 0,
                .data = encoded_buffer,
                .size = encoded_size,
                .pts = frame * 1000000 / 30,
                .dts = frame * 1000000 / 30,
                .is_keyframe = frame_info.is_keyframe
            };
            
            ret = turbo_streamer_write_packet(ctx->streamer, &packet);
            if (ret < 0) {
                fprintf(stderr, "Failed to write packet to streamer\n");
                break;
            }
        }
        
        ctx->frame_count = frame;
        
        /* 协程让出控制权（模拟帧率控制）*/
        /* coro_sleep(33); // 30fps = ~33ms per frame */
    }
    
    /* 断开连接 */
    turbo_streamer_disconnect(ctx->streamer);
    
    printf("Streaming completed, %d frames sent\n", ctx->frame_count);
}

int example_hls_streaming(void) {
    /* 初始化注册表 */
    turbo_codec_registry_init();
    turbo_streamer_registry_init();
    
    /* 创建 CoroNet 上下文 */
    coro_context_t *coro_ctx = coro_context_create(NULL);
    if (!coro_ctx) {
        fprintf(stderr, "Failed to create coroutine context\n");
        return -1;
    }
    
    /* 创建 HttpClient（用于上传分片）*/
    http_client_t *http_client = http_client_create("https://cdn.example.com");
    if (!http_client) {
        fprintf(stderr, "Failed to create HTTP client\n");
        coro_context_destroy(coro_ctx);
        return -1;
    }
    
    /* 配置认证 */
    http_client_set_bearer_token(http_client, "your_api_token_here");
    
    /* 创建 H.264 编码器 */
    turbo_video_codec_config_t video_config = {
        .width = 1920,
        .height = 1080,
        .framerate = 30,
        .bitrate = 5000000,  /* 5 Mbps */
        .keyframe_interval = 60,
        .threads = 4
    };
    
    turbo_codec_t *encoder = turbo_codec_create_encoder("h264", &video_config);
    if (!encoder) {
        fprintf(stderr, "Failed to create encoder\n");
        http_client_destroy(http_client);
        coro_context_destroy(coro_ctx);
        return -1;
    }
    
    /* 创建 HLS streamer */
    turbo_streamer_config_t streamer_config = {
        .protocol = TURBO_STREAMER_HLS,
        .segment_duration_ms = 6000,  /* 6 秒分片 */
        .playlist_size = 5,
        .output_dir = "./hls_output",
        .base_url = "https://cdn.example.com/live/stream",
        .coro_context = coro_ctx,
        .http_client = http_client
    };
    
    turbo_streamer_t *streamer = turbo_streamer_create(&streamer_config);
    if (!streamer) {
        fprintf(stderr, "Failed to create HLS streamer\n");
        turbo_codec_destroy(encoder);
        http_client_destroy(http_client);
        coro_context_destroy(coro_ctx);
        return -1;
    }
    
    /* 添加视频流 */
    turbo_stream_info_t stream_info = {
        .type = TURBO_CODEC_TYPE_VIDEO,
        .codec_name = "h264",
        .width = 1920,
        .height = 1080,
        .framerate = 30
    };
    
    int stream_id;
    turbo_streamer_add_stream(streamer, &stream_info, &stream_id);
    
    /* 创建流式传输上下文 */
    streaming_context_t stream_ctx = {
        .encoder = encoder,
        .streamer = streamer,
        .frame_count = 0
    };
    
    /* 启动协程 */
    printf("Starting HLS streaming...\n");
    coro_create(coro_ctx, streaming_coroutine, &stream_ctx);
    
    /* 运行事件循环 */
    coro_context_run(coro_ctx);
    
    /* 清理 */
    turbo_streamer_destroy(streamer);
    turbo_codec_destroy(encoder);
    http_client_destroy(http_client);
    coro_context_destroy(coro_ctx);
    
    turbo_codec_registry_shutdown();
    turbo_streamer_registry_shutdown();
    
    return 0;
}

/* =============================================================================
 * 主函数
 * ============================================================================= */

int main(int argc, char *argv[]) {
    printf("TurboMedia Streaming Pipeline Examples\n");
    printf("========================================\n\n");
    
    if (argc < 2) {
        printf("Usage: %s <example_number>\n", argv[0]);
        printf("  1: Record to FLV file\n");
        printf("  2: HLS live streaming\n");
        return 1;
    }
    
    int example = atoi(argv[1]);
    
    switch (example) {
        case 1:
            return example_record_to_flv();
        case 2:
            return example_hls_streaming();
        default:
            printf("Invalid example number\n");
            return 1;
    }
}
