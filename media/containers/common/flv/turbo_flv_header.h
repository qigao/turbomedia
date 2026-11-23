// SPDX-License-Identifier: MIT
// FLV 头文件/标签头解析与序列化
// 源自 refer/libflv，已重构为 turbo_flv_ 命名空间

#ifndef TURBO_FLV_HEADER_H_
#define TURBO_FLV_HEADER_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ========== FLV 文件头 ==========
typedef struct turbo_flv_header {
    uint8_t  signature[3]; // "FLV"
    uint8_t  version;      // 版本号，通常为 1
    uint8_t  has_audio;    // 是否包含音频流
    uint8_t  has_video;    // 是否包含视频流
    uint32_t data_offset;  // 数据偏移（通常为 9）
} turbo_flv_header_t;
typedef turbo_flv_header_t flv_header_t;

// ========== FLV Tag 头 ==========
typedef struct turbo_flv_tag_header {
    uint8_t  filter;     // 0 = 不需要预处理
    uint8_t  type;       // 8=音频, 9=视频, 18=脚本数据
    uint32_t size;       // 数据大小（不含 Tag Header）
    uint32_t timestamp;  // 时间戳（毫秒）
    uint32_t stream_id;  // 流 ID（始终为 0）
} turbo_flv_tag_header_t;
typedef turbo_flv_tag_header_t flv_tag_header_t;

// ========== FLV 音频 Tag 头 ==========
typedef struct turbo_flv_audio_tag_header {
    uint8_t  codec_id;    // 音频编解码器 ID（见 TURBO_FLV_AUDIO_*）
    uint8_t  sample_rate; // 采样率：0=5.5kHz, 1=11kHz, 2=22kHz, 3=44kHz
    uint8_t  sample_bits; // 位深：0=8-bit, 1=16-bit
    uint8_t  channels;    // 声道：0=单声道, 1=立体声
    uint8_t  av_packet;   // AAC/Opus: 0=SequenceHeader, 1=RawData

    // Enhanced RTMP v2 多轨音频
    uint8_t  multitrack;       // 多轨类型（见 turbo_flv_audio_multi_track_e）
    uint8_t  channel_order;    // 声道顺序
    uint32_t channel_flags;    // 声道标志（channel_order == 1 时有效）
    uint8_t  channel_mapping[8]; // 声道映射（channel_order == 2 时有效）
} turbo_flv_audio_tag_header_t;
typedef turbo_flv_audio_tag_header_t flv_audio_tag_header_t;

// ========== FLV 视频 Tag 头 ==========
typedef struct turbo_flv_video_tag_header {
    uint8_t codec_id;     // 视频编解码器 ID（见 TURBO_FLV_VIDEO_*）
    uint8_t keyframe;     // 帧类型：1=关键帧, 2=非关键帧
    uint8_t av_packet;    // H.264/H.265/AV1: 0=SequenceHeader, 1=NALU, 2=EndOfSequence
    int32_t cts;          // Composition Time Offset (PTS - DTS)

    int enhanced_rtmp;    // 是否启用 Enhanced RTMP 扩展
} turbo_flv_video_tag_header_t;
typedef turbo_flv_video_tag_header_t flv_video_tag_header_t;

// ========== FLV 文件头读写 ==========

/// 读取 FLV 文件头
/// @return >=0 成功（返回头长度），<0 失败
int turbo_flv_header_read(turbo_flv_header_t* header, const uint8_t* buf, size_t len);

/// 写入 FLV 文件头
/// @param has_audio 1=有音频流，0=无
/// @param has_video 1=有视频流，0=无
/// @return >=0 成功（返回头长度），<0 失败
int turbo_flv_header_write(int has_audio, int has_video, uint8_t* buf, size_t len);

// ========== FLV Tag 头读写 ==========

/// 读取 FLV Tag 头
/// @return >=0 成功（返回头长度），<0 失败
int turbo_flv_tag_header_read(turbo_flv_tag_header_t* tag, const uint8_t* buf, size_t len);

/// 写入 FLV Tag 头
/// @return >=0 成功（返回头长度），<0 失败
int turbo_flv_tag_header_write(const turbo_flv_tag_header_t* tag, uint8_t* buf, size_t len);

// ========== FLV 音频 Tag 头读写 ==========

/// 读取 FLV 音频 Tag 头
/// @return >=0 成功（返回头长度），<0 失败
int turbo_flv_audio_tag_header_read(turbo_flv_audio_tag_header_t* audio, const uint8_t* buf, size_t len);

/// 写入 FLV 音频 Tag 头
/// @return >=0 成功（返回头长度），<0 失败
int turbo_flv_audio_tag_header_write(const turbo_flv_audio_tag_header_t* audio, uint8_t* buf, size_t len);

// ========== FLV 视频 Tag 头读写 ==========

/// 读取 FLV 视频 Tag 头
/// @return >=0 成功（返回头长度），<0 失败
int turbo_flv_video_tag_header_read(turbo_flv_video_tag_header_t* video, const uint8_t* buf, size_t len);

/// 写入 FLV 视频 Tag 头
/// @return >=0 成功（返回头长度），<0 失败
int turbo_flv_video_tag_header_write(const turbo_flv_video_tag_header_t* video, uint8_t* buf, size_t len);

// ========== FLV 脚本 Tag 读写 ==========

/// 读取 FLV 脚本数据 Tag 头（占位函数）
/// @return >=0 成功（返回头长度），<0 失败
int turbo_flv_data_tag_header_read(const uint8_t* buf, size_t len);

/// 写入 FLV 脚本数据 Tag 头（占位函数）
/// @return >=0 成功（返回头长度），<0 失败
int turbo_flv_data_tag_header_write(uint8_t* buf, size_t len);

// ========== FLV Tag Size 字段读写 ==========

/// 读取 PreviousTagSize 字段（4字节）
/// @return >=0 成功，<0 失败
int turbo_flv_tag_size_read(const uint8_t* buf, size_t len, uint32_t* size);

/// 写入 PreviousTagSize 字段（4字节）
/// @return >=0 成功，<0 失败
int turbo_flv_tag_size_write(uint8_t* buf, size_t len, uint32_t size);

#define flv_header_read turbo_flv_header_read
#define flv_header_write turbo_flv_header_write
#define flv_tag_header_read turbo_flv_tag_header_read
#define flv_tag_header_write turbo_flv_tag_header_write
#define flv_audio_tag_header_read turbo_flv_audio_tag_header_read
#define flv_audio_tag_header_write turbo_flv_audio_tag_header_write
#define flv_video_tag_header_read turbo_flv_video_tag_header_read
#define flv_video_tag_header_write turbo_flv_video_tag_header_write
#define flv_data_tag_header_read turbo_flv_data_tag_header_read
#define flv_data_tag_header_write turbo_flv_data_tag_header_write
#define flv_tag_size_read turbo_flv_tag_size_read
#define flv_tag_size_write turbo_flv_tag_size_write

#ifdef __cplusplus
}
#endif

#endif // TURBO_FLV_HEADER_H_
