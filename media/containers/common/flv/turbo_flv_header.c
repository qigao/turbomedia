// SPDX-License-Identifier: MIT
// FLV 头文件/标签头解析与序列化实现
// 源自 refer/libflv/source/flv-header.c，已重构为 turbo_flv_ 命名空间

#include "turbo_flv_header.h"
#include "turbo_flv_proto.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

// ========== 常量定义 ==========
#define FLV_HEADER_SIZE     9  // FLV 文件头长度（含 DataOffset）
#define FLV_TAG_HEADER_SIZE 11 // FLV Tag 头长度（含 StreamID）

// ========== 大端序读写辅助函数 ==========
static inline uint32_t be_read_uint32(const uint8_t* ptr) {
    return ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16) | 
           ((uint32_t)ptr[2] << 8)  | ptr[3];
}

static inline void be_write_uint32(uint8_t* ptr, uint32_t val) {
    ptr[0] = (uint8_t)((val >> 24) & 0xFF);
    ptr[1] = (uint8_t)((val >> 16) & 0xFF);
    ptr[2] = (uint8_t)((val >> 8) & 0xFF);
    ptr[3] = (uint8_t)(val & 0xFF);
}

// ========== FLV 文件头读写 ==========

int turbo_flv_header_read(turbo_flv_header_t* header, const uint8_t* buf, size_t len) {
    if (len < FLV_HEADER_SIZE || buf[0] != 'F' || buf[1] != 'L' || buf[2] != 'V') {
        return -1;
    }

    header->signature[0] = buf[0];
    header->signature[1] = buf[1];
    header->signature[2] = buf[2];
    header->version = buf[3];

    // TypeFlags: bit 2 = audio, bit 0 = video
    header->has_audio = (buf[4] >> 2) & 0x01;
    header->has_video = buf[4] & 0x01;
    header->data_offset = be_read_uint32(buf + 5);

    return FLV_HEADER_SIZE;
}

int turbo_flv_header_write(int has_audio, int has_video, uint8_t* buf, size_t len) {
    if (len < FLV_HEADER_SIZE) {
        return -1;
    }

    buf[0] = 'F';
    buf[1] = 'L';
    buf[2] = 'V';
    buf[3] = 0x01; // version
    buf[4] = ((has_audio ? 1 : 0) << 2) | (has_video ? 1 : 0);
    be_write_uint32(buf + 5, FLV_HEADER_SIZE);

    return FLV_HEADER_SIZE;
}

// ========== FLV Tag 头读写 ==========

int turbo_flv_tag_header_read(turbo_flv_tag_header_t* tag, const uint8_t* buf, size_t len) {
    if (len < FLV_TAG_HEADER_SIZE) {
        return -1;
    }

    tag->type = buf[0] & 0x1F;
    tag->filter = (buf[0] >> 5) & 0x01;
    tag->size = ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
    
    // Timestamp: [buf[4:6]] + ExtendedTimestamp[buf[7]]
    tag->timestamp = ((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 8) | 
                     buf[6] | ((uint32_t)buf[7] << 24);
    
    tag->stream_id = ((uint32_t)buf[8] << 16) | ((uint32_t)buf[9] << 8) | buf[10];

    return FLV_TAG_HEADER_SIZE;
}

int turbo_flv_tag_header_write(const turbo_flv_tag_header_t* tag, uint8_t* buf, size_t len) {
    if (len < FLV_TAG_HEADER_SIZE) {
        return -1;
    }

    buf[0] = (tag->type & 0x1F) | ((tag->filter & 0x01) << 5);
    buf[1] = (tag->size >> 16) & 0xFF;
    buf[2] = (tag->size >> 8) & 0xFF;
    buf[3] = tag->size & 0xFF;
    buf[4] = (tag->timestamp >> 16) & 0xFF;
    buf[5] = (tag->timestamp >> 8) & 0xFF;
    buf[6] = tag->timestamp & 0xFF;
    buf[7] = (tag->timestamp >> 24) & 0xFF; // Extended Timestamp
    buf[8] = (tag->stream_id >> 16) & 0xFF;
    buf[9] = (tag->stream_id >> 8) & 0xFF;
    buf[10] = tag->stream_id & 0xFF;

    return FLV_TAG_HEADER_SIZE;
}

// ========== FLV 音频 Tag 头读写辅助函数 ==========

// 从 FourCC 识别音频编解码器
static int flv_audio_tag_fourcc_to_codec(struct turbo_flv_audio_tag_header* audio, 
                                          const uint8_t* buf, size_t len) {
    if (len < 4) return -1;

    uint32_t fourcc = TURBO_FLV_FOURCC(buf[0], buf[1], buf[2], buf[3]);
    switch (fourcc) {
        case TURBO_FLV_AUDIO_FOURCC_AC3:  audio->codec_id = TURBO_FLV_AUDIO_AC3;  break;
        case TURBO_FLV_AUDIO_FOURCC_EAC3: audio->codec_id = TURBO_FLV_AUDIO_EAC3; break;
        case TURBO_FLV_AUDIO_FOURCC_OPUS: audio->codec_id = TURBO_FLV_AUDIO_OPUS; break;
        case TURBO_FLV_AUDIO_FOURCC_MP3:  audio->codec_id = TURBO_FLV_AUDIO_MP3;  break;
        case TURBO_FLV_AUDIO_FOURCC_FLAC: audio->codec_id = TURBO_FLV_AUDIO_FLAC; break;
        case TURBO_FLV_AUDIO_FOURCC_AAC:  audio->codec_id = TURBO_FLV_AUDIO_AAC;  break;
        default: audio->codec_id = TURBO_FLV_AUDIO_AAC; break; // 未知默认 AAC
    }
    return 0;
}

int turbo_flv_audio_tag_header_read(turbo_flv_audio_tag_header_t* audio, 
                                     const uint8_t* buf, size_t len) {
    if (len < 1) return -1;

    audio->codec_id = buf[0] & 0xF0;
    audio->sample_rate = (buf[0] & 0x0C) >> 2;
    audio->sample_bits = (buf[0] & 0x02) >> 1;
    audio->channels = buf[0] & 0x01;
    audio->av_packet = TURBO_FLV_AVPACKET;
    audio->multitrack = TURBO_FLV_AUDIO_MULTI_TRACK_NONE;

    // AAC/Opus: 需要读取 AACPacketType / OpusPacketType
    if (audio->codec_id == TURBO_FLV_AUDIO_AAC || audio->codec_id == TURBO_FLV_AUDIO_OPUS) {
        if (len < 2) return -1;
        audio->av_packet = buf[1];
        return 2;
    }
    
    // Enhanced RTMP: FourCC 编解码器
    if (audio->codec_id == TURBO_FLV_AUDIO_FOURCC) {
        audio->av_packet = buf[0] & 0x0F;
        size_t off = 1;

        // 多轨音频
        if (audio->av_packet == TURBO_FLV_AUDIO_PACKET_TYPE_MULTITRACK) {
            if (len < 2) return -1;
            audio->multitrack = (buf[1] & 0xF0) >> 4;
            audio->av_packet = buf[1] & 0x0F;
            
            // ManyTracksManyCodecs: 读取第一个编解码器
            if (flv_audio_tag_fourcc_to_codec(audio, buf + 2, len - 2) != 0) return -1;
            off = 6;
        } else {
            if (flv_audio_tag_fourcc_to_codec(audio, buf + 1, len - 1) != 0) return -1;
            off = 5;
        }

        // 多声道配置
        if (audio->av_packet == TURBO_FLV_AUDIO_PACKET_TYPE_MULTICHANNEL_CONFIG) {
            if (off + 2 > len) return -1;
            audio->channel_order = buf[off++];
            audio->channels = buf[off++];

            if (audio->channel_order == 1) {
                if (off + 4 > len) return -1;
                audio->channel_flags = be_read_uint32(buf + off);
                off += 4;
            } else if (audio->channel_order == 2) {
                size_t copy_len = (audio->channels < 8) ? audio->channels : 8;
                if (off + audio->channels > len) return -1;
                memcpy(audio->channel_mapping, buf + off, copy_len);
                off += audio->channels;
            }
        }

        return (int)off;
    }

    return 1; // 其他音频格式（G711, MP3 等）
}

int turbo_flv_audio_tag_header_write(const turbo_flv_audio_tag_header_t* audio, 
                                      uint8_t* buf, size_t len) {
    if (len < 1) return -1;

    // AAC: 2 字节头
    if (audio->codec_id == TURBO_FLV_AUDIO_AAC) {
        if (len < 2) return -1;
        buf[0] = audio->codec_id | (3 << 2) | (1 << 1) | 1; // 44kHz, 16-bit, Stereo
        buf[1] = audio->av_packet;
        return 2;
    }

    // Enhanced RTMP: Opus/FLAC/AC-3/E-AC-3
    if (audio->codec_id == TURBO_FLV_AUDIO_OPUS || audio->codec_id == TURBO_FLV_AUDIO_FLAC ||
        audio->codec_id == TURBO_FLV_AUDIO_AC3  || audio->codec_id == TURBO_FLV_AUDIO_EAC3) {
        if (len < 5) return -1;

        buf[0] = TURBO_FLV_AUDIO_FOURCC | audio->av_packet;
        
        uint32_t fourcc = 0;
        switch (audio->codec_id) {
            case TURBO_FLV_AUDIO_FLAC: fourcc = TURBO_FLV_AUDIO_FOURCC_FLAC; break;
            case TURBO_FLV_AUDIO_AC3:  fourcc = TURBO_FLV_AUDIO_FOURCC_AC3;  break;
            case TURBO_FLV_AUDIO_EAC3: fourcc = TURBO_FLV_AUDIO_FOURCC_EAC3; break;
            case TURBO_FLV_AUDIO_OPUS: fourcc = TURBO_FLV_AUDIO_FOURCC_OPUS; break;
        }
        be_write_uint32(buf + 1, fourcc);
        return 5;
    }

    // 其他格式（G711, MP3 等）
    buf[0] = audio->codec_id | ((audio->sample_rate & 0x03) << 2) | 
             ((audio->sample_bits & 0x01) << 1) | (audio->channels & 0x01);
    return 1;
}

// ========== FLV 视频 Tag 头读写 ==========

int turbo_flv_video_tag_header_read(turbo_flv_video_tag_header_t* video, 
                                     const uint8_t* buf, size_t len) {
    if (len < 1) return -1;

    // Enhanced RTMP 检测（bit 7 置位）
    if (len >= 5 && (buf[0] & 0x80)) {
        video->keyframe = (buf[0] & 0x70) >> 4;
        video->av_packet = buf[0] & 0x0F;
        video->cts = 0;

        uint32_t fourcc = TURBO_FLV_FOURCC(buf[1], buf[2], buf[3], buf[4]);
        
        if (fourcc == TURBO_FLV_VIDEO_FOURCC_AV1) {
            video->codec_id = TURBO_FLV_VIDEO_AV1;
            return 5;
        }

        if (fourcc == TURBO_FLV_VIDEO_FOURCC_HEVC || fourcc == TURBO_FLV_VIDEO_FOURCC_VVC) {
            video->codec_id = (fourcc == TURBO_FLV_VIDEO_FOURCC_HEVC) ? 
                              TURBO_FLV_VIDEO_H265 : TURBO_FLV_VIDEO_H266;
            
            // HEVC/VVC 带 CTS（仅对 NALU 包）
            if (len >= 8 && video->av_packet == TURBO_FLV_AVPACKET) {
                video->cts = ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 8) | buf[7];
                video->cts = (video->cts + 0xFF800000) ^ 0xFF800000; // signed 24-bit
                return 8;
            }

            // CODED_FRAMES_X -> AVPACKET (CTS = 0 优化)
            if (video->av_packet == TURBO_FLV_PACKET_TYPE_CODED_FRAMES_X) {
                video->av_packet = TURBO_FLV_AVPACKET;
            }
            return 5;
        }

        video->codec_id = 0; // 未知编解码器
        return 5;
    }

    // 标准 FLV 格式
    video->keyframe = (buf[0] & 0xF0) >> 4;
    video->codec_id = buf[0] & 0x0F;
    video->av_packet = TURBO_FLV_AVPACKET;

    // H.264/H.265/AV1: 5 字节头（含 CTS）
    if (video->codec_id == TURBO_FLV_VIDEO_H264 || video->codec_id == TURBO_FLV_VIDEO_H265 ||
        video->codec_id == TURBO_FLV_VIDEO_H266 || video->codec_id == TURBO_FLV_VIDEO_AV1 ||
        video->codec_id == TURBO_FLV_VIDEO_AVS3) {
        if (len < 5) return -1;

        video->av_packet = buf[1];
        video->cts = ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 8) | buf[4];
        video->cts = (video->cts + 0xFF800000) ^ 0xFF800000; // signed 24-bit
        return 5;
    }

    return 1; // 其他视频格式（H.263, VP6 等）
}

int turbo_flv_video_tag_header_write(const turbo_flv_video_tag_header_t* video, 
                                      uint8_t* buf, size_t len) {
    // Enhanced RTMP 模式
    if (video->enhanced_rtmp) {
        if (len < 5) return -1;

        uint8_t packet_type = (video->cts == 0 && video->av_packet == TURBO_FLV_AVPACKET) ?
                              TURBO_FLV_PACKET_TYPE_CODED_FRAMES_X : video->av_packet;
        buf[0] = 0x80 | (video->keyframe << 4) | packet_type;

        uint32_t fourcc = 0;
        switch (video->codec_id) {
            case TURBO_FLV_VIDEO_AV1:  fourcc = TURBO_FLV_VIDEO_FOURCC_AV1;  break;
            case TURBO_FLV_VIDEO_H265: fourcc = TURBO_FLV_VIDEO_FOURCC_HEVC; break;
            case TURBO_FLV_VIDEO_H266: fourcc = TURBO_FLV_VIDEO_FOURCC_VVC;  break;
            default: return -1; // 不支持的编解码器
        }

        be_write_uint32(buf + 1, fourcc);

        // HEVC/VVC: 如果 CTS != 0 且为 NALU 包，写入 CTS
        if ((video->codec_id == TURBO_FLV_VIDEO_H265 || video->codec_id == TURBO_FLV_VIDEO_H266) &&
            video->av_packet == TURBO_FLV_AVPACKET && video->cts != 0 && len >= 8) {
            buf[5] = (video->cts >> 16) & 0xFF;
            buf[6] = (video->cts >> 8) & 0xFF;
            buf[7] = video->cts & 0xFF;
            return 8;
        }

        return 5;
    }

    // 标准 FLV 格式
    if (len < 1) return -1;
    buf[0] = (video->keyframe << 4) | (video->codec_id & 0x0F);

    // H.264/H.265/AV1: 5 字节头
    if (video->codec_id == TURBO_FLV_VIDEO_H264 || video->codec_id == TURBO_FLV_VIDEO_H265 ||
        video->codec_id == TURBO_FLV_VIDEO_H266 || video->codec_id == TURBO_FLV_VIDEO_AV1) {
        if (len < 5) return -1;
        buf[1] = video->av_packet;
        buf[2] = (video->cts >> 16) & 0xFF;
        buf[3] = (video->cts >> 8) & 0xFF;
        buf[4] = video->cts & 0xFF;
        return 5;
    }

    return 1;
}

// ========== FLV 脚本 Tag ==========

int turbo_flv_data_tag_header_read(const uint8_t* buf, size_t len) {
    (void)buf;
    return (int)len;
}

int turbo_flv_data_tag_header_write(uint8_t* buf, size_t len) {
    (void)buf;
    (void)len;
    return 0;
}

// ========== PreviousTagSize 读写 ==========

int turbo_flv_tag_size_read(const uint8_t* buf, size_t len, uint32_t* size) {
    if (len < 4) return -1;
    *size = be_read_uint32(buf);
    return 4;
}

int turbo_flv_tag_size_write(uint8_t* buf, size_t len, uint32_t size) {
    if (len < 4) return -1;
    be_write_uint32(buf, size);
    return 4;
}
