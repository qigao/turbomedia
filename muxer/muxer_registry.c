/**
 * Muxer Registry Implementation
 *
 * 管理所有可用的容器封装器
 */
#include "turbo_muxer.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Registry
 * ============================================================================= */

#define MAX_MUXERS 16

static const turbo_muxer_ops_t *g_muxers[MAX_MUXERS];
static int g_muxer_count = 0;
static atomic_int g_initialized = 0;

/* =============================================================================
 * Registry Functions
 * ============================================================================= */

void turbo_muxer_registry_init(void) {
    int expected = 0;

    /* 线程安全初始化 */
    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 0) return;
    if (!atomic_compare_exchange_strong_explicit(&g_initialized, &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return;
    }

    g_muxer_count = 0;
    memset((void *)g_muxers, 0, sizeof(g_muxers));

    /* 注册内置 Muxers */
#ifdef TURBO_MEDIA_HAS_FLV
    turbo_muxer_register(&turbo_flv_muxer_ops);
#endif

#ifdef TURBO_MEDIA_HAS_MP4
    turbo_muxer_register(&turbo_mp4_muxer_ops);
#endif

#ifdef TURBO_MEDIA_HAS_MKV
    turbo_muxer_register(&turbo_mkv_muxer_ops);
    turbo_muxer_register(&turbo_webm_muxer_ops);
#endif

#ifdef TURBO_MEDIA_HAS_MPEG
    turbo_muxer_register(&turbo_mpegts_muxer_ops);
    turbo_muxer_register(&turbo_mpegps_muxer_ops);
#endif
}

void turbo_muxer_registry_shutdown(void) {
    g_muxer_count = 0;
    atomic_store_explicit(&g_initialized, 0, memory_order_release);
}

int turbo_muxer_register(const turbo_muxer_ops_t *ops) {
    if (!ops || !ops->name) return -1;
    if (g_muxer_count >= MAX_MUXERS) return -1;

    /* 检查重复 */
    for (int i = 0; i < g_muxer_count; i++) {
        if (strcmp(g_muxers[i]->name, ops->name) == 0) {
            return -1;  /* 已注册 */
        }
    }

    g_muxers[g_muxer_count++] = ops;
    return 0;
}

const turbo_muxer_ops_t *turbo_muxer_find_by_format(turbo_muxer_format_t format) {
    for (int i = 0; i < g_muxer_count; i++) {
        if (g_muxers[i]->format == format) {
            return g_muxers[i];
        }
    }
    return NULL;
}

const turbo_muxer_ops_t *turbo_muxer_find_by_name(const char *name) {
    if (!name) return NULL;

    for (int i = 0; i < g_muxer_count; i++) {
        if (strcmp(g_muxers[i]->name, name) == 0) {
            return g_muxers[i];
        }
    }
    return NULL;
}

const turbo_muxer_ops_t *turbo_muxer_find_by_extension(const char *ext) {
    if (!ext) return NULL;

    /* 确保扩展名以点开头 */
    const char *search_ext = (ext[0] == '.') ? ext : NULL;
    char ext_with_dot[32];
    if (!search_ext) {
        snprintf(ext_with_dot, sizeof(ext_with_dot), ".%s", ext);
        search_ext = ext_with_dot;
    }

    for (int i = 0; i < g_muxer_count; i++) {
        if (!g_muxers[i]->extensions) continue;
        
        for (int j = 0; g_muxers[i]->extensions[j]; j++) {
            if (strcmp(g_muxers[i]->extensions[j], search_ext) == 0) {
                return g_muxers[i];
            }
        }
    }
    return NULL;
}

#ifdef TURBO_MEDIA_HAS_MKV
int turbo_mkv_muxer_set_memory_limit(void *ctx, size_t limit);
int turbo_mkv_muxer_write_packet_with_discard_padding(
    void *ctx, const turbo_muxer_packet_t *packet, uint64_t padding_ns);
#endif

int turbo_muxer_set_memory_limit(turbo_muxer_t *muxer, size_t limit) {
#ifdef TURBO_MEDIA_HAS_MKV
    if (!muxer || !muxer->ctx || muxer->config.output_path ||
        !muxer->ops || muxer->ops->format != TURBO_MUXER_WEBM)
        return -1;
    return turbo_mkv_muxer_set_memory_limit(muxer->ctx, limit);
#else
    (void)muxer;
    (void)limit;
    return -1;
#endif
}

/* =============================================================================
 * Muxer Instance Functions
 * ============================================================================= */

turbo_muxer_t *turbo_muxer_create(const turbo_muxer_config_t *config) {
    if (!config) return NULL;

    /* 查找 muxer */
    const turbo_muxer_ops_t *ops = turbo_muxer_find_by_format(config->format);
    if (!ops) {
        /* 尝试根据文件扩展名查找 */
        if (config->output_path) {
            const char *ext = strrchr(config->output_path, '.');
            if (ext) {
                ops = turbo_muxer_find_by_extension(ext);
            }
        }
    }

    if (!ops || !ops->create) return NULL;

    /* 创建上下文 */
    void *ctx = ops->create(config);
    if (!ctx) return NULL;

    /* 创建 muxer 实例 */
    turbo_muxer_t *muxer = (turbo_muxer_t *)calloc(1, sizeof(turbo_muxer_t));
    if (!muxer) {
        ops->destroy(ctx);
        return NULL;
    }

    muxer->ops = ops;
    muxer->ctx = ctx;
    muxer->config = *config;
    muxer->stream_count = 0;
    muxer->header_written = 0;

    return muxer;
}

void turbo_muxer_destroy(turbo_muxer_t *muxer) {
    if (!muxer) return;

    if (muxer->ctx && muxer->ops->destroy) {
        muxer->ops->destroy(muxer->ctx);
    }

    free(muxer);
}

int turbo_muxer_add_stream(turbo_muxer_t *muxer, 
                           const turbo_stream_info_t *stream_info,
                           int *stream_id) {
    if (!muxer || !stream_info || !muxer->ops->add_stream) {
        return -1;
    }

    int ret = muxer->ops->add_stream(muxer->ctx, stream_info, stream_id);
    if (ret == 0) {
        muxer->stream_count++;
    }

    return ret;
}

int turbo_muxer_write_header(turbo_muxer_t *muxer) {
    if (!muxer || !muxer->ops->write_header) {
        return -1;
    }

    if (muxer->header_written) {
        return 0;  /* 已写入 */
    }

    int ret = muxer->ops->write_header(muxer->ctx);
    if (ret == 0) {
        muxer->header_written = 1;
    }

    return ret;
}

int turbo_muxer_write_packet(turbo_muxer_t *muxer, 
                             const turbo_muxer_packet_t *packet) {
    if (!muxer || !packet || !muxer->ops->write_packet) {
        return -1;
    }

    /* 自动写入头部 */
    if (!muxer->header_written) {
        int ret = turbo_muxer_write_header(muxer);
        if (ret < 0) return ret;
    }

    return muxer->ops->write_packet(muxer->ctx, packet);
}

int turbo_muxer_write_packet_with_discard_padding(
    turbo_muxer_t *muxer, const turbo_muxer_packet_t *packet,
    uint64_t discard_padding_ns) {
#ifdef TURBO_MEDIA_HAS_MKV
    int result;
    if (!muxer || !muxer->ctx || !packet ||
        muxer->ops != &turbo_webm_muxer_ops || discard_padding_ns == 0)
        return -1;
    if (!muxer->header_written) {
        result = turbo_muxer_write_header(muxer);
        if (result != 0) return result;
    }
    return turbo_mkv_muxer_write_packet_with_discard_padding(
        muxer->ctx, packet, discard_padding_ns);
#else
    (void)muxer; (void)packet; (void)discard_padding_ns;
    return -1;
#endif
}

int turbo_muxer_write_trailer(turbo_muxer_t *muxer) {
    if (!muxer || !muxer->ops->write_trailer) {
        return -1;
    }

    return muxer->ops->write_trailer(muxer->ctx);
}

int turbo_muxer_get_data(turbo_muxer_t *muxer, uint8_t **data, size_t *size) {
    if (!muxer || !data || !size) {
        return -1;
    }

    if (!muxer->ops->get_data) {
        return -1;  /* 不支持内存模式 */
    }

    return muxer->ops->get_data(muxer->ctx, data, size);
}
