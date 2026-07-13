// SPDX-License-Identifier: MIT
// FLV 协议定义：Tag 类型、编解码器 ID、常量定义
// 源自 refer/libflv，已重构为 turbo_flv_ 命名空间

#ifndef TURBO_FLV_PROTO_H_
#define TURBO_FLV_PROTO_H_

#ifdef __cplusplus
extern "C" {
#endif

// ========== FLV Tag 类型 ==========
#define TURBO_FLV_TYPE_AUDIO  8
#define TURBO_FLV_TYPE_VIDEO  9
#define TURBO_FLV_TYPE_SCRIPT 18

// ========== FLV 音频编解码器 ID ==========
#define TURBO_FLV_AUDIO_LPCM    (0 << 4)  // Linear PCM, platform endian
#define TURBO_FLV_AUDIO_ADPCM   (1 << 4)
#define TURBO_FLV_AUDIO_MP3     (2 << 4)
#define TURBO_FLV_AUDIO_LLPCM   (3 << 4)  // Linear PCM, little endian
#define TURBO_FLV_AUDIO_FLAC    (4 << 4)  // Nellymoser16KMono -> FLAC(enhanced rtmp v2)
#define TURBO_FLV_AUDIO_EAC3    (5 << 4)  // Nellymoser8KMono -> EAC3(enhanced rtmp v2)
#define TURBO_FLV_AUDIO_NELLY   (6 << 4)  // Nellymoser
#define TURBO_FLV_AUDIO_G711A   (7 << 4)  // G711 A-law
#define TURBO_FLV_AUDIO_G711U   (8 << 4)  // G711 mu-law
#define TURBO_FLV_AUDIO_FOURCC  (9 << 4)  // enhanced rtmp v2
#define TURBO_FLV_AUDIO_AAC     (10 << 4)
#define TURBO_FLV_AUDIO_SPEEX   (11 << 4)
#define TURBO_FLV_AUDIO_AC3     (12 << 4) // enhanced rtmp v2
#define TURBO_FLV_AUDIO_OPUS    (13 << 4) // opus-codec.org
#define TURBO_FLV_AUDIO_MP3_8K  (14 << 4) // MP3 8 kHz
#define TURBO_FLV_AUDIO_DEVICE  (15 << 4) // Device-specific sound
#define TURBO_FLV_AUDIO_ASC     (0x1000 | TURBO_FLV_AUDIO_AAC)  // AudioSpecificConfig(ISO-14496-3)
#define TURBO_FLV_AUDIO_OPUS_HEAD (0x1100 | TURBO_FLV_AUDIO_OPUS) // https://datatracker.ietf.org/doc/html/rfc7845#section-5.1
#define TURBO_FLV_AUDIO_FLAC_HEAD (0x1200 | TURBO_FLV_AUDIO_FLAC) // xiph.org/flac

// ========== FLV 视频编解码器 ID ==========
#define TURBO_FLV_VIDEO_H263   2  // Sorenson H.263
#define TURBO_FLV_VIDEO_SCREEN 3  // Screen video
#define TURBO_FLV_VIDEO_VP6    4  // On2 VP6
#define TURBO_FLV_VIDEO_H264   7  // AVC
#define TURBO_FLV_VIDEO_H265   12 // https://github.com/CDN-Union/H265
#define TURBO_FLV_VIDEO_AV1    13 // https://aomediacodec.github.io/av1-isobmff
#define TURBO_FLV_VIDEO_AVS3   14
#define TURBO_FLV_VIDEO_H266   15
#define TURBO_FLV_VIDEO_AVCC   (0x2000 | TURBO_FLV_VIDEO_H264) // AVCDecoderConfigurationRecord(ISO-14496-15)
#define TURBO_FLV_VIDEO_HVCC   (0x2100 | TURBO_FLV_VIDEO_H265) // HEVCDecoderConfigurationRecord(ISO-14496-15)
#define TURBO_FLV_VIDEO_AV1C   (0x2200 | TURBO_FLV_VIDEO_AV1)  // AV1CodecConfigurationRecord(av1-isobmff)
#define TURBO_FLV_VIDEO_AVSC   (0x2300 | TURBO_FLV_VIDEO_AVS3) // AVS3DecoderConfigurationRecord
#define TURBO_FLV_VIDEO_VVCC   (0x2400 | TURBO_FLV_VIDEO_H266) // VVCDecoderConfigurationRecord(ISO-14496-15)

#define TURBO_FLV_SCRIPT_METADATA 0x4000 // onMetaData

// ========== FLV 包类型 ==========
enum {
    TURBO_FLV_SEQUENCE_HEADER = 0,  // AVC/AAC sequence header
    TURBO_FLV_AVPACKET        = 1,  // AVC NALU / AAC raw
    TURBO_FLV_END_OF_SEQUENCE = 2,  // AVC end of sequence
    TURBO_FLV_PACKET_TYPE_CODED_FRAMES_X = 3,  // CompositionTime = 0 (优化)
    TURBO_FLV_PACKET_TYPE_METADATA = 4,         // AMF 编码的元数据（如 HDR 信息）
    TURBO_FLV_PACKET_TYPE_MPEG2TS_SEQUENCE_START = 5, // MPEG-2 TS 格式
    TURBO_FLV_PACKET_TYPE_MULTITRACK = 6,       // 视频多轨模式
    
    // 音频
    TURBO_FLV_AUDIO_PACKET_TYPE_MULTICHANNEL_CONFIG = 4,
    TURBO_FLV_AUDIO_PACKET_TYPE_MULTITRACK = 5,
};

// ========== FLV 视频帧类型 ==========
enum {
    TURBO_FLV_VIDEO_KEY_FRAME              = 1, // 关键帧 (seekable frame)
    TURBO_FLV_VIDEO_INTER_FRAME            = 2, // 非关键帧 (non-seekable frame)
    TURBO_FLV_VIDEO_DISPOSABLE_INTER_FRAME = 3, // H.263 only
    TURBO_FLV_VIDEO_GENERATED_KEY_FRAME    = 4, // 服务端生成的关键帧 (reserved)
    TURBO_FLV_VIDEO_COMMAND_FRAME          = 5, // video info/command frame
};

// ========== FLV 音频采样率 ==========
enum {
    TURBO_FLV_SOUND_RATE_5500  = 0, // 5.5 kHz
    TURBO_FLV_SOUND_RATE_11025 = 1, // 11 kHz
    TURBO_FLV_SOUND_RATE_22050 = 2, // 22 kHz
    TURBO_FLV_SOUND_RATE_44100 = 3, // 44 kHz
};

// ========== FLV 音频位深 ==========
enum {
    TURBO_FLV_SOUND_BIT_8  = 0, // 8-bit samples
    TURBO_FLV_SOUND_BIT_16 = 1, // 16-bit samples
};

// ========== FLV 音频声道 ==========
enum {
    TURBO_FLV_SOUND_CHANNEL_MONO   = 0, // 单声道
    TURBO_FLV_SOUND_CHANNEL_STEREO = 1, // 立体声
};

// ========== FLV 音频多轨类型 (Enhanced RTMP v2) ==========
enum turbo_flv_audio_multi_track_e {
    TURBO_FLV_AUDIO_MULTI_TRACK_ONE         = 0,   // OneTrack
    TURBO_FLV_AUDIO_MULTI_TRACK_MANY        = 1,   // ManyTracks
    TURBO_FLV_AUDIO_MULTI_TRACK_MANY_CODECS = 2,   // ManyTracksManyCodecs
    TURBO_FLV_AUDIO_MULTI_TRACK_NONE        = 255,
};

// ========== FLV FourCC 宏 ==========
#define TURBO_FLV_FOURCC(a, b, c, d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))

// 视频 FourCC
#define TURBO_FLV_VIDEO_FOURCC_VP9  TURBO_FLV_FOURCC('v', 'p', '0', '9')
#define TURBO_FLV_VIDEO_FOURCC_AV1  TURBO_FLV_FOURCC('a', 'v', '0', '1')
#define TURBO_FLV_VIDEO_FOURCC_AVC  TURBO_FLV_FOURCC('a', 'v', 'c', '1') // H.264
#define TURBO_FLV_VIDEO_FOURCC_HEVC TURBO_FLV_FOURCC('h', 'v', 'c', '1') // H.265
#define TURBO_FLV_VIDEO_FOURCC_VVC  TURBO_FLV_FOURCC('v', 'v', 'c', '1') // H.266

// 音频 FourCC
#define TURBO_FLV_AUDIO_FOURCC_AC3  TURBO_FLV_FOURCC('a', 'c', '-', '3') // AC-3
#define TURBO_FLV_AUDIO_FOURCC_EAC3 TURBO_FLV_FOURCC('e', 'c', '-', '3') // E-AC-3
#define TURBO_FLV_AUDIO_FOURCC_OPUS TURBO_FLV_FOURCC('O', 'p', 'u', 's') // Opus
#define TURBO_FLV_AUDIO_FOURCC_MP3  TURBO_FLV_FOURCC('.', 'm', 'p', '3') // MP3
#define TURBO_FLV_AUDIO_FOURCC_FLAC TURBO_FLV_FOURCC('f', 'L', 'a', 'C') // FLAC
#define TURBO_FLV_AUDIO_FOURCC_AAC  TURBO_FLV_FOURCC('m', 'p', '4', 'a') // AAC

#define FLV_TYPE_AUDIO TURBO_FLV_TYPE_AUDIO
#define FLV_TYPE_VIDEO TURBO_FLV_TYPE_VIDEO
#define FLV_TYPE_SCRIPT TURBO_FLV_TYPE_SCRIPT

#define FLV_AUDIO_LPCM TURBO_FLV_AUDIO_LPCM
#define FLV_AUDIO_ADPCM TURBO_FLV_AUDIO_ADPCM
#define FLV_AUDIO_MP3 TURBO_FLV_AUDIO_MP3
#define FLV_AUDIO_LLPCM TURBO_FLV_AUDIO_LLPCM
#define FLV_AUDIO_FLAC TURBO_FLV_AUDIO_FLAC
#define FLV_AUDIO_EAC3 TURBO_FLV_AUDIO_EAC3
#define FLV_AUDIO_NELLY TURBO_FLV_AUDIO_NELLY
#define FLV_AUDIO_G711A TURBO_FLV_AUDIO_G711A
#define FLV_AUDIO_G711U TURBO_FLV_AUDIO_G711U
#define FLV_AUDIO_FOURCC TURBO_FLV_AUDIO_FOURCC
#define FLV_AUDIO_AAC TURBO_FLV_AUDIO_AAC
#define FLV_AUDIO_SPEEX TURBO_FLV_AUDIO_SPEEX
#define FLV_AUDIO_AC3 TURBO_FLV_AUDIO_AC3
#define FLV_AUDIO_OPUS TURBO_FLV_AUDIO_OPUS
#define FLV_AUDIO_MP3_8K TURBO_FLV_AUDIO_MP3_8K
#define FLV_AUDIO_DEVICE TURBO_FLV_AUDIO_DEVICE
#define FLV_AUDIO_ASC TURBO_FLV_AUDIO_ASC
#define FLV_AUDIO_OPUS_HEAD TURBO_FLV_AUDIO_OPUS_HEAD
#define FLV_AUDIO_FLAC_HEAD TURBO_FLV_AUDIO_FLAC_HEAD

#define FLV_VIDEO_H263 TURBO_FLV_VIDEO_H263
#define FLV_VIDEO_SCREEN TURBO_FLV_VIDEO_SCREEN
#define FLV_VIDEO_VP6 TURBO_FLV_VIDEO_VP6
#define FLV_VIDEO_H264 TURBO_FLV_VIDEO_H264
#define FLV_VIDEO_H265 TURBO_FLV_VIDEO_H265
#define FLV_VIDEO_AV1 TURBO_FLV_VIDEO_AV1
#define FLV_VIDEO_AVS3 TURBO_FLV_VIDEO_AVS3
#define FLV_VIDEO_H266 TURBO_FLV_VIDEO_H266
#define FLV_VIDEO_AVCC TURBO_FLV_VIDEO_AVCC
#define FLV_VIDEO_HVCC TURBO_FLV_VIDEO_HVCC
#define FLV_VIDEO_AV1C TURBO_FLV_VIDEO_AV1C
#define FLV_VIDEO_AVSC TURBO_FLV_VIDEO_AVSC
#define FLV_VIDEO_VVCC TURBO_FLV_VIDEO_VVCC

#define FLV_SCRIPT_METADATA TURBO_FLV_SCRIPT_METADATA
#define FLV_SEQUENCE_HEADER TURBO_FLV_SEQUENCE_HEADER
#define FLV_AVPACKET TURBO_FLV_AVPACKET
#define FLV_END_OF_SEQUENCE TURBO_FLV_END_OF_SEQUENCE
#define FLV_PACKET_TYPE_CODED_FRAMES_X TURBO_FLV_PACKET_TYPE_CODED_FRAMES_X
#define FLV_PACKET_TYPE_MULTITRACK TURBO_FLV_PACKET_TYPE_MULTITRACK
#define FLV_VIDEO_KEY_FRAME TURBO_FLV_VIDEO_KEY_FRAME
#define FLV_VIDEO_INTER_FRAME TURBO_FLV_VIDEO_INTER_FRAME
#define FLV_SOUND_RATE_5500 TURBO_FLV_SOUND_RATE_5500
#define FLV_SOUND_RATE_11025 TURBO_FLV_SOUND_RATE_11025
#define FLV_SOUND_RATE_22050 TURBO_FLV_SOUND_RATE_22050
#define FLV_SOUND_RATE_44100 TURBO_FLV_SOUND_RATE_44100
#define FLV_SOUND_BIT_8 TURBO_FLV_SOUND_BIT_8
#define FLV_SOUND_BIT_16 TURBO_FLV_SOUND_BIT_16
#define FLV_SOUND_CHANNEL_MONO TURBO_FLV_SOUND_CHANNEL_MONO
#define FLV_SOUND_CHANNEL_STEREO TURBO_FLV_SOUND_CHANNEL_STEREO

#ifdef __cplusplus
}
#endif

#endif // TURBO_FLV_PROTO_H_
