// SPDX-License-Identifier: MIT
// FLV Demuxer 内部接口
// 迁移自 refer/libflv/include/flv-demuxer.h

#ifndef FLV_DEMUXER_INTERNAL_H_
#define FLV_DEMUXER_INTERNAL_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flv_demuxer_t flv_demuxer_t;

/// 音频/视频基本流回调
/// @param[in] param 用户参数
/// @param[in] codec 音频/视频格式（见 turbo_flv_proto.h）
/// @param[in] data 音频/视频数据：AAC=ADTS+AAC-Frame, H.264=startcode+NALU, MP3=Raw data
/// @param[in] bytes 数据长度
/// @param[in] pts 显示时间戳
/// @param[in] dts 解码时间戳
/// @param[in] flags 1=视频关键帧, other=undefined
/// @return 0-ok, other-error
typedef int (*flv_demuxer_handler)(void* param, int codec, const void* data, size_t bytes, uint32_t pts, uint32_t dts, int flags);

/// 创建 FLV demuxer
flv_demuxer_t* flv_demuxer_create(flv_demuxer_handler handler, void* param);

/// 销毁 FLV demuxer
void flv_demuxer_destroy(flv_demuxer_t* demuxer);

/// 输入 FLV Audio/Video 流
/// @param[in] type 8=audio, 9=video, 18=script（见 turbo_flv_proto.h）
/// @param[in] data FLV audio/video Stream（AudioTagHeader/VideoTagHeader + A/V Data）
/// @param[in] bytes 数据长度
/// @param[in] timestamp 相对于第一个 Tag 的毫秒时间戳（DTS）
/// @return 0-ok, other-error
int flv_demuxer_input(flv_demuxer_t* demuxer, int type, const void* data, size_t bytes, uint32_t timestamp);

#ifdef __cplusplus
}
#endif

#endif // FLV_DEMUXER_INTERNAL_H_
