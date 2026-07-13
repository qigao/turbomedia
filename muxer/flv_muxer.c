/**
 * FLV Muxer Implementation
 *
 * 基于 refer/libflv 的 FLV 容器封装器
 */
#include "turbo_muxer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef TURBO_MEDIA_HAS_FLV

#include "flv_muxer_internal.h"
#include "../common/flv/turbo_flv_proto.h"

/* =============================================================================
 * FLV Muxer 上下文
 * ============================================================================= */

typedef struct {
    flv_muxer_t *flv;
    FILE *fp;                   /* 文件句柄 */
    uint8_t *buffer;            /* 内存模式缓冲区 */
    size_t buffer_size;
    size_t buffer_capacity;
    
    int video_stream_id;
    int audio_stream_id;
    
    int has_video;
    int has_audio;
    
    uint32_t start_time;
} flv_muxer_ctx_t;

/* =============================================================================
 * FLV 输出回调
 * ============================================================================= */

static int flv_write_callback(void *param, int type, const void *data, size_t bytes, uint32_t timestamp) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)param;
    
    if (ctx->fp) {
        /* 文件模式 */
        return fwrite(data, 1, bytes, ctx->fp) == bytes ? 0 : -1;
    } else {
        /* 内存模式 */
        size_t needed = ctx->buffer_size + bytes;
        if (needed > ctx->buffer_capacity) {
            size_t new_capacity = ctx->buffer_capacity * 2;
            if (new_capacity < needed) {
                new_capacity = needed;
            }
            
            uint8_t *new_buffer = (uint8_t *)realloc(ctx->buffer, new_capacity);
            if (!new_buffer) {
                return -1;
            }
            
            ctx->buffer = new_buffer;
            ctx->buffer_capacity = new_capacity;
        }
        
        memcpy(ctx->buffer + ctx->buffer_size, data, bytes);
        ctx->buffer_size += bytes;
        return 0;
    }
}

/* =============================================================================
 * Muxer 操作实现
 * ============================================================================= */

static void *flv_muxer_create_impl(const turbo_muxer_config_t *config) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)calloc(1, sizeof(flv_muxer_ctx_t));
    if (!ctx) return NULL;
    
    /* 创建 FLV muxer */
    ctx->flv = flv_muxer_create(flv_write_callback, ctx);
    if (!ctx->flv) {
        free(ctx);
        return NULL;
    }
    
    /* 打开输出 */
    if (config->output_path) {
        ctx->fp = fopen(config->output_path, "wb");
        if (!ctx->fp) {
            flv_muxer_destroy(ctx->flv);
            free(ctx);
            return NULL;
        }
    } else {
        /* 内存模式 */
        ctx->buffer_capacity = 1024 * 1024; /* 初始 1MB */
        ctx->buffer = (uint8_t *)malloc(ctx->buffer_capacity);
        if (!ctx->buffer) {
            flv_muxer_destroy(ctx->flv);
            free(ctx);
            return NULL;
        }
    }
    
    ctx->video_stream_id = -1;
    ctx->audio_stream_id = -1;
    
    return ctx;
}

static void flv_muxer_destroy_impl(void *ctx_ptr) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    
    if (ctx->flv) {
        flv_muxer_destroy(ctx->flv);
    }
    
    if (ctx->fp) {
        fclose(ctx->fp);
    }
    
    if (ctx->buffer) {
        free(ctx->buffer);
    }
    
    free(ctx);
}

static int flv_muxer_add_stream_impl(void *ctx_ptr, const turbo_stream_info_t *stream_info, int *stream_id) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    
    if (stream_info->type == TURBO_CODEC_TYPE_VIDEO) {
        if (ctx->has_video) {
            return -1; /* FLV 只支持一个视频轨 */
        }
        ctx->has_video = 1;
        ctx->video_stream_id = 0;
        *stream_id = 0;
        
        /* 写入视频配置（AVC Decoder Configuration Record）*/
        if (stream_info->extradata && stream_info->extradata_size > 0) {
            /* H.264: extradata 是 AVCDecoderConfigurationRecord */
            /* H.265: extradata 是 HEVCDecoderConfigurationRecord */
            int codec_id = TURBO_FLV_VIDEO_H264; /* 默认 H.264 */
            
            if (strcmp(stream_info->codec_name, "h265") == 0 || 
                strcmp(stream_info->codec_name, "hevc") == 0) {
                codec_id = TURBO_FLV_VIDEO_H265;
            }
            
            flv_muxer_avc(ctx->flv, stream_info->extradata,
                         stream_info->extradata_size, 0, 0);
        }
        
    } else if (stream_info->type == TURBO_CODEC_TYPE_AUDIO) {
        if (ctx->has_audio) {
            return -1; /* FLV 只支持一个音频轨 */
        }
        ctx->has_audio = 1;
        ctx->audio_stream_id = 1;
        *stream_id = 1;
        
        /* 写入音频配置 */
        if (stream_info->extradata && stream_info->extradata_size > 0) {
            int codec_id = TURBO_FLV_AUDIO_AAC; /* 默认 AAC */
            
            if (strcmp(stream_info->codec_name, "opus") == 0) {
                codec_id = TURBO_FLV_AUDIO_OPUS;
            } else if (strcmp(stream_info->codec_name, "mp3") == 0) {
                codec_id = TURBO_FLV_AUDIO_MP3;
            }
            
            flv_muxer_aac(ctx->flv, stream_info->extradata,
                         stream_info->extradata_size, 0, 0);
        }
    } else {
        return -1;
    }
    
    return 0;
}

static int flv_muxer_write_header_impl(void *ctx_ptr) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    
    /* FLV header 会在第一个数据包写入时自动生成 */
    ctx->start_time = 0;
    
    return 0;
}

static int flv_muxer_write_packet_impl(void *ctx_ptr, const turbo_muxer_packet_t *packet) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    
    /* 转换时间戳：假设输入是微秒，FLV 需要毫秒 */
    uint32_t timestamp = (uint32_t)(packet->pts / 1000);
    
    if (packet->stream_id == ctx->video_stream_id) {
        /* 写入视频帧 */
        return flv_muxer_avc(ctx->flv, packet->data, packet->size, timestamp, timestamp);
        
    } else if (packet->stream_id == ctx->audio_stream_id) {
        /* 写入音频帧 */
        return flv_muxer_aac(ctx->flv, packet->data, packet->size, timestamp, timestamp);
    }
    
    return -1;
}

static int flv_muxer_write_trailer_impl(void *ctx_ptr) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    
    /* FLV 不需要特殊的 trailer */
    if (ctx->fp) {
        fflush(ctx->fp);
    }
    
    return 0;
}

static int flv_muxer_get_data_impl(void *ctx_ptr, uint8_t **data, size_t *size) {
    flv_muxer_ctx_t *ctx = (flv_muxer_ctx_t *)ctx_ptr;
    
    if (ctx->fp) {
        return -1; /* 文件模式不支持 */
    }
    
    *data = ctx->buffer;
    *size = ctx->buffer_size;
    
    return 0;
}

/* =============================================================================
 * FLV Muxer 操作表
 * ============================================================================= */

static const char *flv_extensions[] = {".flv", NULL};

const turbo_muxer_ops_t turbo_flv_muxer_ops = {
    .name = "flv",
    .format = TURBO_MUXER_FLV,
    .extensions = flv_extensions,
    
    .create = flv_muxer_create_impl,
    .destroy = flv_muxer_destroy_impl,
    .add_stream = flv_muxer_add_stream_impl,
    .write_header = flv_muxer_write_header_impl,
    .write_packet = flv_muxer_write_packet_impl,
    .write_trailer = flv_muxer_write_trailer_impl,
    .get_data = flv_muxer_get_data_impl
};

#endif /* TURBO_MEDIA_HAS_FLV */
