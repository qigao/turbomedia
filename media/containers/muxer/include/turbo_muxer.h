/**
 * TurboMedia Muxer (Container) Abstraction
 *
 * 统一的容器封装接口，用于将编码后的音视频流打包成各种容器格式
 */
#ifndef TURBO_MUXER_H
#define TURBO_MUXER_H

#include <stdint.h>
#include <stddef.h>
#include <turbo_export.h>
#include <turbo_codec.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * 容器格式类型
 */
typedef enum {
    TURBO_MUXER_FLV,           /* Flash Video */
    TURBO_MUXER_MP4,           /* MPEG-4 Part 14 */
    TURBO_MUXER_MKV,           /* Matroska */
    TURBO_MUXER_WEBM,          /* WebM (MKV subset) */
    TURBO_MUXER_MPEG_TS,       /* MPEG Transport Stream */
    TURBO_MUXER_MPEG_PS,       /* MPEG Program Stream */
    TURBO_MUXER_HLS_TS,        /* HLS with TS segments */
    TURBO_MUXER_HLS_FMP4       /* HLS with fMP4 segments */
} turbo_muxer_format_t;

/**
 * Muxer 配置
 */
typedef struct {
    turbo_muxer_format_t format;
    const char *output_path;    /* 输出文件路径，NULL 表示内存模式 */
    int fragment_duration_ms;   /* 分片时长(HLS/DASH)，0 表示不分片 */
    int write_duration;         /* 是否写入时长元数据 */
    int faststart;              /* MP4: moov box 放在 mdat 前面 */
} turbo_muxer_config_t;

/**
 * 流（轨道）信息
 */
typedef struct {
    int stream_id;              /* 流 ID */
    turbo_codec_class_t type;   /* 音频或视频 */
    const char *codec_name;     /* 编解码器名称 */
    const uint8_t *extradata;   /* H26x: AVC/HEVC/VVC config 或参数集 Annex-B */
    size_t extradata_size;
    
    /* 视频特定 */
    int width;
    int height;
    int framerate;
    
    /* 音频特定 */
    int sample_rate;
    int channels;
} turbo_stream_info_t;

/**
 * 封装的数据包
 */
typedef struct {
    int stream_id;              /* 所属流 ID */
    const uint8_t *data;
    size_t size;
    int64_t pts;                /* 显示时间戳（微秒）*/
    int64_t dts;                /* 解码时间戳（微秒）*/
    int is_keyframe;            /* 是否关键帧 */
    int64_t duration;           /* 持续时长（可选）*/
} turbo_muxer_packet_t;

/* 前向声明 */
typedef struct turbo_muxer_s turbo_muxer_t;

/**
 * Muxer 操作表
 */
typedef struct {
    const char *name;
    turbo_muxer_format_t format;
    const char **extensions;    /* 支持的文件扩展名列表，NULL 结尾 */

    /* 创建/销毁 */
    void *(*create)(const turbo_muxer_config_t *config);
    void (*destroy)(void *ctx);

    /* 添加流轨道 */
    int (*add_stream)(void *ctx, const turbo_stream_info_t *stream_info, int *stream_id);

    /* 写入头部（在添加所有流后调用）*/
    int (*write_header)(void *ctx);

    /* 写入数据包 */
    int (*write_packet)(void *ctx, const turbo_muxer_packet_t *packet);

    /* 写入尾部并完成封装 */
    int (*write_trailer)(void *ctx);

    /* 获取输出数据（内存模式）*/
    int (*get_data)(void *ctx, uint8_t **data, size_t *size);

} turbo_muxer_ops_t;

/**
 * Muxer 实例
 */
struct turbo_muxer_s {
    const turbo_muxer_ops_t *ops;
    void *ctx;
    turbo_muxer_config_t config;
    int stream_count;
    int header_written;
};

/* =============================================================================
 * Muxer 注册表
 * ============================================================================= */

/**
 * 初始化 Muxer 注册表
 */
TURBO_MEDIA_API void turbo_muxer_registry_init(void);

/**
 * 关闭 Muxer 注册表
 */
TURBO_MEDIA_API void turbo_muxer_registry_shutdown(void);

/**
 * 注册 Muxer
 */
TURBO_MEDIA_API int turbo_muxer_register(const turbo_muxer_ops_t *ops);

/**
 * 根据格式查找 Muxer
 */
TURBO_MEDIA_API const turbo_muxer_ops_t *turbo_muxer_find_by_format(turbo_muxer_format_t format);

/**
 * 根据名称查找 Muxer
 */
TURBO_MEDIA_API const turbo_muxer_ops_t *turbo_muxer_find_by_name(const char *name);

/**
 * 根据文件扩展名查找 Muxer
 */
TURBO_MEDIA_API const turbo_muxer_ops_t *turbo_muxer_find_by_extension(const char *ext);

/* =============================================================================
 * Muxer 实例操作
 * ============================================================================= */

/**
 * 创建 Muxer 实例
 */
TURBO_MEDIA_API turbo_muxer_t *turbo_muxer_create(const turbo_muxer_config_t *config);

/**
 * 销毁 Muxer 实例
 */
TURBO_MEDIA_API void turbo_muxer_destroy(turbo_muxer_t *muxer);

/**
 * 添加流轨道
 */
TURBO_MEDIA_API int turbo_muxer_add_stream(turbo_muxer_t *muxer,
                                     const turbo_stream_info_t *stream_info,
                                     int *stream_id);

/**
 * 写入头部
 */
TURBO_MEDIA_API int turbo_muxer_write_header(turbo_muxer_t *muxer);

/**
 * 写入数据包
 */
TURBO_MEDIA_API int turbo_muxer_write_packet(turbo_muxer_t *muxer,
                                       const turbo_muxer_packet_t *packet);

/**
 * 写入尾部
 */
TURBO_MEDIA_API int turbo_muxer_write_trailer(turbo_muxer_t *muxer);

/**
 * 获取输出数据（内存模式）
 */
TURBO_MEDIA_API int turbo_muxer_get_data(turbo_muxer_t *muxer, uint8_t **data, size_t *size);

/* =============================================================================
 * 内置 Muxer 声明
 * ============================================================================= */

#ifdef TURBO_MEDIA_HAS_FLV
extern const turbo_muxer_ops_t turbo_flv_muxer_ops;
#endif

#ifdef TURBO_MEDIA_HAS_MP4
extern const turbo_muxer_ops_t turbo_mp4_muxer_ops;
#endif

#ifdef TURBO_MEDIA_HAS_MKV
extern const turbo_muxer_ops_t turbo_mkv_muxer_ops;
extern const turbo_muxer_ops_t turbo_webm_muxer_ops;
#endif

#ifdef TURBO_MEDIA_HAS_MPEG
extern const turbo_muxer_ops_t turbo_mpegts_muxer_ops;
extern const turbo_muxer_ops_t turbo_mpegps_muxer_ops;
#endif

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MUXER_H */
