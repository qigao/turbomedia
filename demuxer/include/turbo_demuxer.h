/**
 * TurboMedia Demuxer (Container Parser) Abstraction
 *
 * 统一的容器解封装接口，用于从各种容器格式中提取音视频流
 */
#ifndef TURBO_DEMUXER_H
#define TURBO_DEMUXER_H

#include <stdint.h>
#include <stddef.h>
#include <turbo_export.h>
#include <turbo_codec.h>
#include <turbo_muxer.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * 类型定义
 * ============================================================================= */

/**
 * Demuxer 配置
 */
typedef struct {
    const char *input_path;     /* 输入文件路径 */
    const uint8_t *data;        /* 内存模式：数据指针 */
    size_t data_size;           /* 内存模式：数据大小 */
    int probe_size;             /* 探测大小（字节）*/
} turbo_demuxer_config_t;

/**
 * 解封装的数据包
 */
typedef struct {
    int stream_index;           /* 所属流索引 */
    uint8_t *data;
    size_t size;
    int64_t pts;                /* 显示时间戳 */
    int64_t dts;                /* 解码时间戳 */
    int64_t duration;
    int is_keyframe;
} turbo_demuxer_packet_t;

/**
 * 容器元数据
 */
typedef struct {
    int64_t duration_ms;        /* 总时长（毫秒）*/
    int64_t bitrate;            /* 比特率 */
    const char *title;
    const char *author;
    const char *copyright;
    const char *comment;
} turbo_container_metadata_t;

/* 前向声明 */
typedef struct turbo_demuxer_s turbo_demuxer_t;

/**
 * Demuxer 操作表
 */
typedef struct {
    const char *name;
    const char **extensions;    /* 支持的文件扩展名列表 */

    /* 创建/销毁 */
    void *(*create)(const turbo_demuxer_config_t *config);
    void (*destroy)(void *ctx);

    /* 探测格式（返回置信度 0-100）*/
    int (*probe)(const uint8_t *data, size_t size);

    /* 打开并解析头部 */
    int (*open)(void *ctx);

    /* 读取下一个数据包 */
    int (*read_packet)(void *ctx, turbo_demuxer_packet_t *packet);

    /* 定位到指定时间戳 */
    int (*seek)(void *ctx, int64_t timestamp_ms, int flags);

    /* 获取流数量 */
    int (*get_stream_count)(void *ctx);

    /* 获取流信息 */
    int (*get_stream_info)(void *ctx, int stream_index, turbo_stream_info_t *info);

    /* 获取容器元数据 */
    int (*get_metadata)(void *ctx, turbo_container_metadata_t *metadata);

} turbo_demuxer_ops_t;

/**
 * Demuxer 实例
 */
struct turbo_demuxer_s {
    const turbo_demuxer_ops_t *ops;
    void *ctx;
    turbo_demuxer_config_t config;
    int stream_count;
};

/* =============================================================================
 * Seek 标志
 * ============================================================================= */

#define TURBO_DEMUXER_SEEK_BACKWARD  0x01  /* 向后查找关键帧 */
#define TURBO_DEMUXER_SEEK_FORWARD   0x02  /* 向前查找关键帧 */
#define TURBO_DEMUXER_SEEK_ANY       0x04  /* 允许非关键帧 */

/* =============================================================================
 * Demuxer 注册表
 * ============================================================================= */

/**
 * 初始化 Demuxer 注册表
 */
CXX_C_API void turbo_demuxer_registry_init(void);

/**
 * 关闭 Demuxer 注册表
 */
CXX_C_API void turbo_demuxer_registry_shutdown(void);

/**
 * 注册 Demuxer
 */
CXX_C_API int turbo_demuxer_register(const turbo_demuxer_ops_t *ops);

/**
 * 根据名称查找 Demuxer
 */
CXX_C_API const turbo_demuxer_ops_t *turbo_demuxer_find_by_name(const char *name);

/**
 * 根据文件扩展名查找 Demuxer
 */
CXX_C_API const turbo_demuxer_ops_t *turbo_demuxer_find_by_extension(const char *ext);

/**
 * 自动探测格式
 */
CXX_C_API const turbo_demuxer_ops_t *turbo_demuxer_probe(const uint8_t *data, size_t size);

/* =============================================================================
 * Demuxer 实例操作
 * ============================================================================= */

/**
 * 创建 Demuxer 实例（自动探测格式）
 */
CXX_C_API turbo_demuxer_t *turbo_demuxer_create(const turbo_demuxer_config_t *config);

/**
 * 创建指定格式的 Demuxer 实例
 */
CXX_C_API turbo_demuxer_t *turbo_demuxer_create_by_name(const char *name,
                                                        const turbo_demuxer_config_t *config);

/**
 * 销毁 Demuxer 实例
 */
CXX_C_API void turbo_demuxer_destroy(turbo_demuxer_t *demuxer);

/**
 * 打开容器
 */
CXX_C_API int turbo_demuxer_open(turbo_demuxer_t *demuxer);

/**
 * 读取数据包
 */
CXX_C_API int turbo_demuxer_read_packet(turbo_demuxer_t *demuxer, 
                                        turbo_demuxer_packet_t *packet);

/**
 * 释放数据包
 */
CXX_C_API void turbo_demuxer_free_packet(turbo_demuxer_packet_t *packet);

/**
 * 定位
 */
CXX_C_API int turbo_demuxer_seek(turbo_demuxer_t *demuxer, int64_t timestamp_ms, int flags);

/**
 * 获取流数量
 */
CXX_C_API int turbo_demuxer_get_stream_count(turbo_demuxer_t *demuxer);

/**
 * 获取流信息
 */
CXX_C_API int turbo_demuxer_get_stream_info(turbo_demuxer_t *demuxer, 
                                            int stream_index,
                                            turbo_stream_info_t *info);

/**
 * 获取元数据
 */
CXX_C_API int turbo_demuxer_get_metadata(turbo_demuxer_t *demuxer,
                                         turbo_container_metadata_t *metadata);

/* =============================================================================
 * 内置 Demuxer 声明
 * ============================================================================= */

#ifdef TURBO_MEDIA_HAS_FLV
extern const turbo_demuxer_ops_t turbo_flv_demuxer_ops;
#endif

#ifdef TURBO_MEDIA_HAS_MP4
extern const turbo_demuxer_ops_t turbo_mp4_demuxer_ops;
#endif

#ifdef TURBO_MEDIA_HAS_MKV
extern const turbo_demuxer_ops_t turbo_mkv_demuxer_ops;
extern const turbo_demuxer_ops_t turbo_webm_demuxer_ops;
#endif

#ifdef TURBO_MEDIA_HAS_MPEG
extern const turbo_demuxer_ops_t turbo_mpegts_demuxer_ops;
extern const turbo_demuxer_ops_t turbo_mpegps_demuxer_ops;
#endif

#ifdef __cplusplus
}
#endif

#endif /* TURBO_DEMUXER_H */
