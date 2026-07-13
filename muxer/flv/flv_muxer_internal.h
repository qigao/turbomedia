// SPDX-License-Identifier: MIT
// FLV Muxer 内部接口
// 迁移自 refer/libflv/include/flv-muxer.h

#ifndef FLV_MUXER_INTERNAL_H_
#define FLV_MUXER_INTERNAL_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flv_muxer_t flv_muxer_t;

/// FLV 输出回调函数
/// @param[in] param 用户参数
/// @param[in] type 8=audio, 9=video, 18=script
/// @param[in] data FLV Audio/Video Data（不含 FLV Tag Header）
/// @param[in] bytes 数据长度
/// @param[in] timestamp 时间戳（毫秒）
/// @return 0-ok, other-error
typedef int (*flv_muxer_handler)(void* param, int type, const void* data, size_t bytes, uint32_t timestamp);

/// 创建 FLV muxer
flv_muxer_t* flv_muxer_create(flv_muxer_handler handler, void* param);

/// 销毁 FLV muxer
void flv_muxer_destroy(flv_muxer_t* muxer);

/// 重置 muxer 状态（重新创建 AAC/AVC sequence header）
int flv_muxer_reset(flv_muxer_t* muxer);

/// 启用/禁用 Enhanced RTMP
/// @param[in] enable 1=启用, 0=禁用
void flv_muxer_set_enhanced_rtmp(flv_muxer_t* muxer, int enable);

/// 封装 AAC 音频帧
/// @param[in] data AAC ADTS stream
int flv_muxer_aac(flv_muxer_t* muxer, const void* data, size_t bytes, uint32_t pts, uint32_t dts);

/// 封装 MP3 音频帧
int flv_muxer_mp3(flv_muxer_t* muxer, const void* data, size_t bytes, uint32_t pts, uint32_t dts);

/// 封装 G.711 A-law 音频帧
int flv_muxer_g711a(flv_muxer_t* muxer, const void* data, size_t bytes, uint32_t pts, uint32_t dts);

/// 封装 G.711 μ-law 音频帧
int flv_muxer_g711u(flv_muxer_t* muxer, const void* data, size_t bytes, uint32_t pts, uint32_t dts);

/// 封装 Opus 音频帧
/// @param[in] data 第一帧为 Opus Head，后续为 Opus samples
int flv_muxer_opus(flv_muxer_t* muxer, const void* data, size_t bytes, uint32_t pts, uint32_t dts);

/// 封装 AC-3 音频帧
int flv_muxer_ac3(flv_muxer_t* muxer, const void* data, size_t bytes, uint32_t pts, uint32_t dts);

/// 封装 E-AC-3 音频帧
int flv_muxer_eac3(flv_muxer_t* muxer, const void* data, size_t bytes, uint32_t pts, uint32_t dts);

/// 封装 H.264 视频帧
/// @param[in] data H.264 Annexb bitstream (start code + NALU)
int flv_muxer_avc(flv_muxer_t* muxer, const void* data, size_t bytes, uint32_t pts, uint32_t dts);

/// 封装 H.265 视频帧
/// @param[in] data H.265 Annexb bitstream (start code + NALU)
int flv_muxer_hevc(flv_muxer_t* muxer, const void* data, size_t bytes, uint32_t pts, uint32_t dts);

/// 封装 AV1 视频帧
/// @param[in] data AV1 low overhead bitstream format
int flv_muxer_av1(flv_muxer_t* muxer, const void* data, size_t bytes, uint32_t pts, uint32_t dts);

/// 封装 AVS3 视频帧
/// @param[in] data AVS3 bitstream (00 00 01 B0 ...)
int flv_muxer_avs3(flv_muxer_t* muxer, const void* data, size_t bytes, uint32_t pts, uint32_t dts);

/// FLV 元数据
struct flv_metadata_t {
    int audiocodecid;
    double audiodatarate;  // kbps
    int audiosamplerate;
    int audiosamplesize;
    int stereo;

    int videocodecid;
    double videodatarate;  // kbps
    double framerate;      // fps
    double duration;
    int interval;          // frame interval
    int width;
    int height;
};

/// 写入 FLV 元数据（onMetaData）
int flv_muxer_metadata(flv_muxer_t* muxer, const struct flv_metadata_t* metadata);

#ifdef __cplusplus
}
#endif

#endif // FLV_MUXER_INTERNAL_H_
