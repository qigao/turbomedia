/**
 * Demuxer Registry Implementation
 *
 * 管理所有可用的容器解封装器
 */
#include "turbo_demuxer.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Registry
 * ============================================================================= */

#define MAX_DEMUXERS 16

static const turbo_demuxer_ops_t *g_demuxers[MAX_DEMUXERS];
static int g_demuxer_count = 0;
static atomic_int g_initialized = 0;

/* =============================================================================
 * Registry Functions
 * ============================================================================= */

void turbo_demuxer_registry_init(void) {
    int expected = 0;

    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 0) return;
    if (!atomic_compare_exchange_strong_explicit(&g_initialized, &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return;
    }

    g_demuxer_count = 0;
    memset((void *)g_demuxers, 0, sizeof(g_demuxers));

    /* 注册内置 Demuxers */
#ifdef TURBO_MEDIA_HAS_FLV
    turbo_demuxer_register(&turbo_flv_demuxer_ops);
#endif

#ifdef TURBO_MEDIA_HAS_MP4
    turbo_demuxer_register(&turbo_mp4_demuxer_ops);
#endif

#ifdef TURBO_MEDIA_HAS_MKV
    turbo_demuxer_register(&turbo_mkv_demuxer_ops);
    turbo_demuxer_register(&turbo_webm_demuxer_ops);
#endif

#ifdef TURBO_MEDIA_HAS_MPEG
    turbo_demuxer_register(&turbo_mpegts_demuxer_ops);
    turbo_demuxer_register(&turbo_mpegps_demuxer_ops);
#endif
}

void turbo_demuxer_registry_shutdown(void) {
    g_demuxer_count = 0;
    atomic_store_explicit(&g_initialized, 0, memory_order_release);
}

int turbo_demuxer_register(const turbo_demuxer_ops_t *ops) {
    if (!ops || !ops->name) return -1;
    if (g_demuxer_count >= MAX_DEMUXERS) return -1;

    /* 检查重复 */
    for (int i = 0; i < g_demuxer_count; i++) {
        if (strcmp(g_demuxers[i]->name, ops->name) == 0) {
            return -1;
        }
    }

    g_demuxers[g_demuxer_count++] = ops;
    return 0;
}

const turbo_demuxer_ops_t *turbo_demuxer_find_by_name(const char *name) {
    if (!name) return NULL;

    for (int i = 0; i < g_demuxer_count; i++) {
        if (strcmp(g_demuxers[i]->name, name) == 0) {
            return g_demuxers[i];
        }
    }
    return NULL;
}

const turbo_demuxer_ops_t *turbo_demuxer_find_by_extension(const char *ext) {
    if (!ext) return NULL;

    const char *search_ext = (ext[0] == '.') ? ext : NULL;
    char ext_with_dot[32];
    if (!search_ext) {
        snprintf(ext_with_dot, sizeof(ext_with_dot), ".%s", ext);
        search_ext = ext_with_dot;
    }

    for (int i = 0; i < g_demuxer_count; i++) {
        if (!g_demuxers[i]->extensions) continue;
        
        for (int j = 0; g_demuxers[i]->extensions[j]; j++) {
            if (strcmp(g_demuxers[i]->extensions[j], search_ext) == 0) {
                return g_demuxers[i];
            }
        }
    }
    return NULL;
}

const turbo_demuxer_ops_t *turbo_demuxer_probe(const uint8_t *data, size_t size) {
    if (!data || size == 0) return NULL;

    const turbo_demuxer_ops_t *best_match = NULL;
    int best_score = 0;

    /* 遍历所有 demuxer，找到置信度最高的 */
    for (int i = 0; i < g_demuxer_count; i++) {
        if (!g_demuxers[i]->probe) continue;
        
        int score = g_demuxers[i]->probe(data, size);
        if (score > best_score) {
            best_score = score;
            best_match = g_demuxers[i];
        }
    }

    return (best_score > 50) ? best_match : NULL;  /* 至少 50% 置信度 */
}

/* =============================================================================
 * Demuxer Instance Functions
 * ============================================================================= */

static const turbo_demuxer_ops_t *find_demuxer_for_config(const turbo_demuxer_config_t *config) {
    const turbo_demuxer_ops_t *ops = NULL;

    /* 1. 先尝试探测格式 */
    if (config->data && config->data_size > 0) {
        ops = turbo_demuxer_probe(config->data, config->data_size);
        if (ops) return ops;
    }

    /* 2. 根据文件扩展名查找 */
    if (config->input_path) {
        const char *ext = strrchr(config->input_path, '.');
        if (ext) {
            ops = turbo_demuxer_find_by_extension(ext);
            if (ops) return ops;
        }
    }

    return NULL;
}

turbo_demuxer_t *turbo_demuxer_create(const turbo_demuxer_config_t *config) {
    if (!config) return NULL;

    const turbo_demuxer_ops_t *ops = find_demuxer_for_config(config);
    if (!ops || !ops->create) return NULL;

    void *ctx = ops->create(config);
    if (!ctx) return NULL;

    turbo_demuxer_t *demuxer = (turbo_demuxer_t *)calloc(1, sizeof(turbo_demuxer_t));
    if (!demuxer) {
        ops->destroy(ctx);
        return NULL;
    }

    demuxer->ops = ops;
    demuxer->ctx = ctx;
    demuxer->config = *config;
    demuxer->stream_count = 0;

    return demuxer;
}

turbo_demuxer_t *turbo_demuxer_create_by_name(const char *name,
                                              const turbo_demuxer_config_t *config) {
    if (!name || !config) return NULL;

    const turbo_demuxer_ops_t *ops = turbo_demuxer_find_by_name(name);
    if (!ops || !ops->create) return NULL;

    void *ctx = ops->create(config);
    if (!ctx) return NULL;

    turbo_demuxer_t *demuxer = (turbo_demuxer_t *)calloc(1, sizeof(turbo_demuxer_t));
    if (!demuxer) {
        ops->destroy(ctx);
        return NULL;
    }

    demuxer->ops = ops;
    demuxer->ctx = ctx;
    demuxer->config = *config;
    demuxer->stream_count = 0;

    return demuxer;
}

void turbo_demuxer_destroy(turbo_demuxer_t *demuxer) {
    if (!demuxer) return;

    if (demuxer->ctx && demuxer->ops->destroy) {
        demuxer->ops->destroy(demuxer->ctx);
    }

    free(demuxer);
}

int turbo_demuxer_open(turbo_demuxer_t *demuxer) {
    if (!demuxer || !demuxer->ops->open) {
        return -1;
    }

    int ret = demuxer->ops->open(demuxer->ctx);
    if (ret == 0 && demuxer->ops->get_stream_count) {
        demuxer->stream_count = demuxer->ops->get_stream_count(demuxer->ctx);
    }

    return ret;
}

int turbo_demuxer_read_packet(turbo_demuxer_t *demuxer, 
                              turbo_demuxer_packet_t *packet) {
    if (!demuxer || !packet || !demuxer->ops->read_packet) {
        return -1;
    }

    return demuxer->ops->read_packet(demuxer->ctx, packet);
}

void turbo_demuxer_free_packet(turbo_demuxer_packet_t *packet) {
    if (!packet) return;
    
    if (packet->data) {
        free(packet->data);
        packet->data = NULL;
    }
    packet->size = 0;
}

int turbo_demuxer_seek(turbo_demuxer_t *demuxer, int64_t timestamp_ms, int flags) {
    if (!demuxer || !demuxer->ops->seek) {
        return -1;
    }

    return demuxer->ops->seek(demuxer->ctx, timestamp_ms, flags);
}

int turbo_demuxer_get_stream_count(turbo_demuxer_t *demuxer) {
    if (!demuxer) return -1;
    return demuxer->stream_count;
}

int turbo_demuxer_get_stream_info(turbo_demuxer_t *demuxer, 
                                  int stream_index,
                                  turbo_stream_info_t *info) {
    if (!demuxer || !info || !demuxer->ops->get_stream_info) {
        return -1;
    }

    if (stream_index < 0 || stream_index >= demuxer->stream_count) {
        return -1;
    }

    return demuxer->ops->get_stream_info(demuxer->ctx, stream_index, info);
}

int turbo_demuxer_get_metadata(turbo_demuxer_t *demuxer,
                               turbo_container_metadata_t *metadata) {
    if (!demuxer || !metadata) {
        return -1;
    }

    if (!demuxer->ops->get_metadata) {
        /* 填充默认值 */
        memset(metadata, 0, sizeof(turbo_container_metadata_t));
        return 0;
    }

    return demuxer->ops->get_metadata(demuxer->ctx, metadata);
}
