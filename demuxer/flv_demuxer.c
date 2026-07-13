/**
 * FLV Demuxer Implementation
 *
 * 基于 refer/libflv 的 FLV 容器解封装器
 */
#include "turbo_demuxer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef TURBO_MEDIA_HAS_FLV

#include "flv_demuxer_internal.h"
#include "flv_reader.h"
#include "amf0.h"
#include "../common/flv/turbo_flv_header.h"
#include "../common/flv/turbo_flv_proto.h"

/* =============================================================================
 * FLV Demuxer 上下文
 * ============================================================================= */

typedef struct {
    flv_demuxer_t *flv;
    FILE *fp;
    
    /* 内存模式 */
    const uint8_t *data;
    size_t data_size;
    size_t data_pos;
    
    /* 流信息 */
    turbo_stream_info_t streams[2];  /* 最多 1 个视频 + 1 个音频 */
    int stream_count;
    int video_stream_index;
    int audio_stream_index;
    
    /* 当前数据包 */
    uint8_t *packet_buffer;
    size_t packet_size;
    size_t packet_capacity;
    int packet_stream_index;
    int64_t packet_pts;
    int64_t packet_dts;
    int packet_flags;
    int current_tag_type;
    
    /* 元数据 */
    turbo_container_metadata_t metadata;
    
} flv_demuxer_ctx_t;

static const char *flv_codec_name(int codec) {
    switch (codec) {
        case TURBO_FLV_AUDIO_AAC:
        case TURBO_FLV_AUDIO_ASC:
            return "aac";
        case TURBO_FLV_AUDIO_MP3:
        case TURBO_FLV_AUDIO_MP3_8K:
            return "mp3";
        case TURBO_FLV_AUDIO_OPUS:
        case TURBO_FLV_AUDIO_OPUS_HEAD:
            return "opus";
        case TURBO_FLV_AUDIO_FLAC:
        case TURBO_FLV_AUDIO_FLAC_HEAD:
            return "flac";
        case TURBO_FLV_VIDEO_H264:
        case TURBO_FLV_VIDEO_AVCC:
            return "h264";
        case TURBO_FLV_VIDEO_H265:
        case TURBO_FLV_VIDEO_HVCC:
            return "h265";
        case TURBO_FLV_VIDEO_H266:
        case TURBO_FLV_VIDEO_VVCC:
            return "h266";
        case TURBO_FLV_VIDEO_AV1:
        case TURBO_FLV_VIDEO_AV1C:
            return "av1";
        case TURBO_FLV_VIDEO_AVS3:
        case TURBO_FLV_VIDEO_AVSC:
            return "avs3";
        default:
            return "unknown";
    }
}

static void flv_metadata_item(struct amf_object_item_t *item, enum AMFDataType type,
                              const char *name, void *value, size_t size) {
    item->type = type;
    item->name = name;
    item->value = value;
    item->size = size;
}

static int flv_parse_metadata(flv_demuxer_ctx_t *ctx, const uint8_t *data, size_t bytes) {
    const uint8_t *end = data + bytes;
    char name[64] = {0};
    double duration = 0.0;
    double audiodatarate = 0.0;
    double videodatarate = 0.0;
    struct amf_object_item_t props[3];
    struct amf_object_item_t item;

    if (bytes < 1 || data[0] != AMF_STRING) {
        return 0;
    }

    data = AMFReadString(data + 1, end, 0, name, sizeof(name) - 1);
    if (!data) {
        return -1;
    }
    if (strcmp(name, "@setDataFrame") == 0) {
        if (data >= end || data[0] != AMF_STRING) {
            return -1;
        }
        data = AMFReadString(data + 1, end, 0, name, sizeof(name) - 1);
        if (!data) {
            return -1;
        }
    }
    if (strcmp(name, "onMetaData") != 0) {
        return 0;
    }

    flv_metadata_item(&props[0], AMF_NUMBER, "duration", &duration, sizeof(duration));
    flv_metadata_item(&props[1], AMF_NUMBER, "audiodatarate", &audiodatarate, sizeof(audiodatarate));
    flv_metadata_item(&props[2], AMF_NUMBER, "videodatarate", &videodatarate, sizeof(videodatarate));
    flv_metadata_item(&item, AMF_OBJECT, "onMetaData", props, sizeof(props) / sizeof(props[0]));

    if (!amf_read_items(data, end, &item, 1)) {
        item.type = AMF_ECMA_ARRAY;
        if (!amf_read_items(data, end, &item, 1)) {
            return -1;
        }
    }

    if (duration > 0.0) {
        ctx->metadata.duration_ms = (int64_t)(duration * 1000.0);
    }
    if (audiodatarate > 0.0 || videodatarate > 0.0) {
        ctx->metadata.bitrate = (int64_t)((audiodatarate + videodatarate) * 1024.0);
    }
    return 0;
}

/* =============================================================================
 * FLV 数据包回调
 * ============================================================================= */

static int flv_packet_handler(void *param, int codec, const void *data, size_t bytes, 
                              uint32_t pts, uint32_t dts, int flags) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)param;
    int stream_index;

    if (codec == TURBO_FLV_SCRIPT_METADATA) {
        return flv_parse_metadata(ctx, (const uint8_t *)data, bytes);
    }

    stream_index = ctx->current_tag_type == TURBO_FLV_TYPE_AUDIO
        ? ctx->audio_stream_index
        : ctx->video_stream_index;
    if (stream_index < 0) {
        return 0;
    }
    
    /* 确保缓冲区足够大 */
    if (bytes > ctx->packet_capacity) {
        size_t new_capacity = bytes * 2;
        uint8_t *new_buffer = (uint8_t *)realloc(ctx->packet_buffer, new_capacity);
        if (!new_buffer) {
            return -1;
        }
        ctx->packet_buffer = new_buffer;
        ctx->packet_capacity = new_capacity;
    }
    
    /* 复制数据 */
    memcpy(ctx->packet_buffer, data, bytes);
    ctx->packet_size = bytes;
    ctx->packet_stream_index = stream_index;
    ctx->packet_pts = pts;
    ctx->packet_dts = dts;
    ctx->packet_flags = flags;
    ctx->streams[stream_index].codec_name = flv_codec_name(codec);
    
    return 0;
}

/* =============================================================================
 * 文件读取回调
 * ============================================================================= */

static int flv_file_read(void *param, void *buf, int len) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)param;
    
    if (ctx->fp) {
        /* 文件模式 */
        return (int)fread(buf, 1, len, ctx->fp);
    } else {
        /* 内存模式 */
        if (ctx->data_pos >= ctx->data_size) {
            return 0;  /* EOF */
        }
        
        size_t available = ctx->data_size - ctx->data_pos;
        size_t to_read = (size_t)len < available ? (size_t)len : available;
        
        memcpy(buf, ctx->data + ctx->data_pos, to_read);
        ctx->data_pos += to_read;
        
        return (int)to_read;
    }
}

static int flv_file_seek(void *param, int64_t offset) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)param;
    
    if (ctx->fp) {
        return fseek(ctx->fp, (long)offset, SEEK_SET);
    } else {
        if (offset < 0 || (size_t)offset > ctx->data_size) {
            return -1;
        }
        ctx->data_pos = (size_t)offset;
        return 0;
    }
}

static int64_t flv_file_tell(void *param) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)param;
    
    if (ctx->fp) {
        return ftell(ctx->fp);
    } else {
        return (int64_t)ctx->data_pos;
    }
}

/* =============================================================================
 * Demuxer 操作实现
 * ============================================================================= */

static int flv_probe_impl(const uint8_t *data, size_t size) {
    if (size < 9) return 0;
    
    /* 检查 FLV 文件头签名 */
    if (data[0] == 'F' && data[1] == 'L' && data[2] == 'V') {
        return 100;  /* 100% 置信度 */
    }
    
    return 0;
}

static void *flv_demuxer_create_impl(const turbo_demuxer_config_t *config) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)calloc(1, sizeof(flv_demuxer_ctx_t));
    if (!ctx) return NULL;
    
    /* 打开输入 */
    if (config->input_path) {
        ctx->fp = fopen(config->input_path, "rb");
        if (!ctx->fp) {
            free(ctx);
            return NULL;
        }
    } else if (config->data && config->data_size > 0) {
        ctx->data = config->data;
        ctx->data_size = config->data_size;
        ctx->data_pos = 0;
    } else {
        free(ctx);
        return NULL;
    }
    
    /* 创建 FLV demuxer */
    ctx->flv = flv_demuxer_create(flv_packet_handler, ctx);
    if (!ctx->flv) {
        if (ctx->fp) fclose(ctx->fp);
        free(ctx);
        return NULL;
    }
    
    /* 初始化缓冲区 */
    ctx->packet_capacity = 65536;  /* 64KB */
    ctx->packet_buffer = (uint8_t *)malloc(ctx->packet_capacity);
    if (!ctx->packet_buffer) {
        flv_demuxer_destroy(ctx->flv);
        if (ctx->fp) fclose(ctx->fp);
        free(ctx);
        return NULL;
    }
    
    ctx->video_stream_index = -1;
    ctx->audio_stream_index = -1;
    ctx->packet_stream_index = -1;
    ctx->stream_count = 0;
    
    return ctx;
}

static void flv_demuxer_destroy_impl(void *ctx_ptr) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)ctx_ptr;
    if (!ctx) return;
    
    if (ctx->flv) {
        flv_demuxer_destroy(ctx->flv);
    }
    
    if (ctx->fp) {
        fclose(ctx->fp);
    }
    
    if (ctx->packet_buffer) {
        free(ctx->packet_buffer);
    }
    
    free(ctx);
}

static int flv_demuxer_open_impl(void *ctx_ptr) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)ctx_ptr;
    turbo_flv_header_t flv_header;
    
    /* 读取 FLV 头部 */
    uint8_t header[13];
    int ret = flv_file_read(ctx, header, sizeof(header));
    if (ret < (int)sizeof(header)) {
        return -1;
    }
    
    /* 验证签名 */
    if (header[0] != 'F' || header[1] != 'L' || header[2] != 'V') {
        return -1;
    }
    if (turbo_flv_header_read(&flv_header, header, 9) < 0) {
        return -1;
    }
    
    /* 检查流类型 */
    int has_video = (header[4] & 0x01) != 0;
    int has_audio = (header[4] & 0x04) != 0;
    
    /* 初始化流信息（实际编解码器信息需要从第一个数据包解析）*/
    if (has_video) {
        ctx->video_stream_index = ctx->stream_count;
        ctx->streams[ctx->stream_count].type = TURBO_CODEC_TYPE_VIDEO;
        ctx->streams[ctx->stream_count].codec_name = "unknown";
        ctx->stream_count++;
    }
    
    if (has_audio) {
        ctx->audio_stream_index = ctx->stream_count;
        ctx->streams[ctx->stream_count].type = TURBO_CODEC_TYPE_AUDIO;
        ctx->streams[ctx->stream_count].codec_name = "unknown";
        ctx->stream_count++;
    }
    
    if (flv_file_seek(ctx, flv_header.data_offset + 4) != 0) {
        return -1;
    }
    
    return 0;
}

static int flv_demuxer_read_packet_impl(void *ctx_ptr, turbo_demuxer_packet_t *packet) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)ctx_ptr;
    uint8_t tag_header[11];
    uint8_t prev_size[4];
    turbo_flv_tag_header_t tag;

    while (ctx->packet_size == 0) {
        int ret = flv_file_read(ctx, tag_header, sizeof(tag_header));
        if (ret == 0) {
            return 0;
        }
        if (ret != (int)sizeof(tag_header) ||
            turbo_flv_tag_header_read(&tag, tag_header, sizeof(tag_header)) < 0) {
            return -1;
        }

        if (tag.size > ctx->packet_capacity) {
            uint8_t *new_buffer = (uint8_t *)realloc(ctx->packet_buffer, tag.size);
            if (!new_buffer) {
                return -1;
            }
            ctx->packet_buffer = new_buffer;
            ctx->packet_capacity = tag.size;
        }

        ret = flv_file_read(ctx, ctx->packet_buffer, (int)tag.size);
        if (ret != (int)tag.size) {
            return ret < 0 ? ret : -1;
        }
        ret = flv_file_read(ctx, prev_size, sizeof(prev_size));
        if (ret != (int)sizeof(prev_size)) {
            return ret < 0 ? ret : -1;
        }

        ctx->current_tag_type = tag.type;
        ret = flv_demuxer_input(ctx->flv, tag.type, ctx->packet_buffer, tag.size, tag.timestamp);
        ctx->current_tag_type = 0;
        if (ret < 0) {
            return -1;
        }
    }

    if (ctx->packet_size > 0) {
        packet->data = (uint8_t *)malloc(ctx->packet_size);
        if (!packet->data) {
            return -1;
        }
        
        memcpy(packet->data, ctx->packet_buffer, ctx->packet_size);
        packet->size = ctx->packet_size;
        packet->stream_index = ctx->packet_stream_index;
        packet->pts = ctx->packet_pts;
        packet->dts = ctx->packet_dts;
        packet->duration = 0;
        packet->is_keyframe = ctx->packet_flags != 0;
        
        ctx->packet_size = 0;  /* 重置 */
        ctx->packet_stream_index = -1;
        return 1;  /* 成功读取一个包 */
    }
    
    return 0;  /* 需要更多数据 */
}

static int flv_demuxer_get_stream_count_impl(void *ctx_ptr) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)ctx_ptr;
    return ctx->stream_count;
}

static int flv_demuxer_get_stream_info_impl(void *ctx_ptr, int stream_index, 
                                            turbo_stream_info_t *info) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)ctx_ptr;
    
    if (stream_index < 0 || stream_index >= ctx->stream_count) {
        return -1;
    }
    
    *info = ctx->streams[stream_index];
    return 0;
}

static int flv_demuxer_seek_impl(void *ctx_ptr, int64_t timestamp_ms, int flags) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)ctx_ptr;
    
    /* FLV 的 seek 实现需要解析关键帧索引 */
    /* 这里是简化版本，只支持回到开头 */
    if (timestamp_ms == 0) {
        return flv_file_seek(ctx, 0);
    }
    
    return -1;  /* 不支持任意位置 seek */
}

static int flv_demuxer_get_metadata_impl(void *ctx_ptr,
                                         turbo_container_metadata_t *metadata) {
    flv_demuxer_ctx_t *ctx = (flv_demuxer_ctx_t *)ctx_ptr;
    *metadata = ctx->metadata;
    return 0;
}

/* =============================================================================
 * FLV Demuxer 操作表
 * ============================================================================= */

static const char *flv_extensions[] = {".flv", NULL};

const turbo_demuxer_ops_t turbo_flv_demuxer_ops = {
    .name = "flv",
    .extensions = flv_extensions,
    
    .create = flv_demuxer_create_impl,
    .destroy = flv_demuxer_destroy_impl,
    .probe = flv_probe_impl,
    .open = flv_demuxer_open_impl,
    .read_packet = flv_demuxer_read_packet_impl,
    .seek = flv_demuxer_seek_impl,
    .get_stream_count = flv_demuxer_get_stream_count_impl,
    .get_stream_info = flv_demuxer_get_stream_info_impl,
    .get_metadata = flv_demuxer_get_metadata_impl
};

#endif /* TURBO_MEDIA_HAS_FLV */
