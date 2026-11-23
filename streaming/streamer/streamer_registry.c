/**
 * Streamer Registry Implementation
 *
 * 管理所有可用的流媒体协议
 */
#include "turbo_streamer.h"
#include "hls_streamer_internal.h"
#include "rtmp_streamer_internal.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Registry
 * ============================================================================= */

#define MAX_STREAMERS 16

static const turbo_streamer_ops_t *g_streamers[MAX_STREAMERS];
static int g_streamer_count = 0;
static atomic_int g_initialized = 0;

/* =============================================================================
 * Registry Functions
 * ============================================================================= */

void turbo_streamer_registry_init(void) {
    int expected = 0;

    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 0) return;
    if (!atomic_compare_exchange_strong_explicit(&g_initialized, &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return;
    }

    g_streamer_count = 0;
    memset((void *)g_streamers, 0, sizeof(g_streamers));

    /* 注册内置 Streamers */
#ifdef TURBO_MEDIA_HAS_HLS
    turbo_streamer_register(&turbo_hls_streamer_ops);
#endif

#ifdef TURBO_MEDIA_HAS_DASH
    turbo_streamer_register(&turbo_dash_streamer_ops);
#endif

#ifdef TURBO_MEDIA_HAS_RTMP
    turbo_streamer_register(&turbo_rtmp_streamer_ops);
#endif

#ifdef TURBO_MEDIA_HAS_HTTP_FLV
    turbo_streamer_register(&turbo_http_flv_streamer_ops);
#endif
}

void turbo_streamer_registry_shutdown(void) {
    g_streamer_count = 0;
    atomic_store_explicit(&g_initialized, 0, memory_order_release);
}

int turbo_streamer_register(const turbo_streamer_ops_t *ops) {
    if (!ops || !ops->name) return -1;
    if (g_streamer_count >= MAX_STREAMERS) return -1;

    /* 检查重复 */
    for (int i = 0; i < g_streamer_count; i++) {
        if (strcmp(g_streamers[i]->name, ops->name) == 0) {
            return -1;
        }
    }

    g_streamers[g_streamer_count++] = ops;
    return 0;
}

const turbo_streamer_ops_t *turbo_streamer_find_by_protocol(turbo_streamer_protocol_t protocol) {
    for (int i = 0; i < g_streamer_count; i++) {
        if (g_streamers[i]->protocol == protocol) {
            return g_streamers[i];
        }
    }
    return NULL;
}

const turbo_streamer_ops_t *turbo_streamer_find_by_name(const char *name) {
    if (!name) return NULL;

    for (int i = 0; i < g_streamer_count; i++) {
        if (strcmp(g_streamers[i]->name, name) == 0) {
            return g_streamers[i];
        }
    }
    return NULL;
}

turbo_streamer_protocol_t turbo_streamer_detect_protocol(const char *url) {
    if (!url) return -1;

    /* 根据 URL scheme 检测协议 */
    if (strncmp(url, "hls://", 6) == 0 || 
        strstr(url, ".m3u8") != NULL) {
        return TURBO_STREAMER_HLS;
    } else if (strncmp(url, "dash://", 7) == 0 ||
               strstr(url, ".mpd") != NULL) {
        return TURBO_STREAMER_DASH;
    } else if (strncmp(url, "rtmp://", 7) == 0 ||
               strncmp(url, "rtmps://", 8) == 0) {
        return TURBO_STREAMER_RTMP;
    } else if (strstr(url, ".flv") != NULL) {
        return TURBO_STREAMER_HTTP_FLV;
    }

    return -1;  /* 未知协议 */
}

/* =============================================================================
 * Streamer Instance Functions
 * ============================================================================= */

turbo_streamer_t *turbo_streamer_create(const turbo_streamer_config_t *config) {
    if (!config) return NULL;

    /* 查找 streamer */
    const turbo_streamer_ops_t *ops = NULL;
    
    if (config->protocol >= 0) {
        ops = turbo_streamer_find_by_protocol(config->protocol);
    } else if (config->url) {
        /* 尝试从 URL 自动检测 */
        turbo_streamer_protocol_t protocol = turbo_streamer_detect_protocol(config->url);
        if (protocol >= 0) {
            ops = turbo_streamer_find_by_protocol(protocol);
        }
    }

    if (!ops || !ops->create) return NULL;

    void *ctx = ops->create(config);
    if (!ctx) return NULL;

    turbo_streamer_t *streamer = (turbo_streamer_t *)calloc(1, sizeof(turbo_streamer_t));
    if (!streamer) {
        (void)ops->destroy(ctx);
        return NULL;
    }

    streamer->ops = ops;
    streamer->ctx = ctx;
    streamer->config = *config;
    streamer->connected = 0;

    return streamer;
}

int turbo_streamer_destroy(turbo_streamer_t *streamer) {
    if (!streamer) return 0;

    /* 自动断开连接 */
    if (streamer->connected) {
        if (turbo_streamer_disconnect(streamer) != 0) return -1;
    }

    if (streamer->ctx && streamer->ops->destroy) {
        if (streamer->ops->destroy(streamer->ctx) != 0) return -1;
        streamer->ctx = NULL;
    }

    free(streamer);
    return 0;
}

int turbo_streamer_connect(turbo_streamer_t *streamer) {
    if (!streamer || !streamer->ops->connect) {
        return -1;
    }

    if (streamer->connected) {
        return 0;  /* 已连接 */
    }

    int ret = streamer->ops->connect(streamer->ctx);
    if (ret == 0) {
        streamer->connected = 1;
    }

    return ret;
}

int turbo_streamer_disconnect(turbo_streamer_t *streamer) {
    if (!streamer) return -1;

    if (!streamer->connected) {
        return 0;
    }

    int ret = 0;
    if (streamer->ops->disconnect) {
        ret = streamer->ops->disconnect(streamer->ctx);
    }

    if (ret == 0) streamer->connected = 0;
    return ret;
}

int turbo_streamer_add_stream(turbo_streamer_t *streamer,
                              const turbo_stream_info_t *stream_info,
                              int *stream_id) {
    if (!streamer || !stream_info || !streamer->ops->add_stream) {
        return -1;
    }

    return streamer->ops->add_stream(streamer->ctx, stream_info, stream_id);
}

int turbo_streamer_write_packet(turbo_streamer_t *streamer,
                                const turbo_muxer_packet_t *packet) {
    if (!streamer || !packet || !streamer->ops->write_packet) {
        return -1;
    }

    /* 自动连接 */
    if (!streamer->connected) {
        int ret = turbo_streamer_connect(streamer);
        if (ret < 0) return ret;
    }

    return streamer->ops->write_packet(streamer->ctx, packet);
}

int turbo_streamer_read_packet(turbo_streamer_t *streamer,
                               turbo_demuxer_packet_t *packet) {
    if (!streamer || !packet || !streamer->ops->read_packet) {
        return -1;
    }

    /* 自动连接 */
    if (!streamer->connected) {
        int ret = turbo_streamer_connect(streamer);
        if (ret < 0) return ret;
    }

    return streamer->ops->read_packet(streamer->ctx, packet);
}

int turbo_streamer_get_stats(turbo_streamer_t *streamer,
                             turbo_streamer_stats_t *stats) {
    if (!streamer || !stats) {
        return -1;
    }

    if (!streamer->ops->get_stats) {
        /* 返回默认统计 */
        memset(stats, 0, sizeof(turbo_streamer_stats_t));
        return 0;
    }

    return streamer->ops->get_stats(streamer->ctx, stats);
}

void turbo_streamer_set_event_callback(turbo_streamer_t *streamer,
                                       turbo_streamer_event_cb callback,
                                       void *user_data) {
    if (!streamer) return;

    if (streamer->ops->set_event_callback) {
        streamer->ops->set_event_callback(streamer->ctx, callback, user_data);
    }
}

/* =============================================================================
 * HLS 特定接口
 * ============================================================================= */

int turbo_streamer_hls_get_playlist(turbo_streamer_t *streamer,
                                    char **playlist,
                                    size_t *size) {
    if (!streamer || streamer->ops->protocol != TURBO_STREAMER_HLS || !playlist || !size) {
        return -1;
    }
    return turbo_hls_streamer_get_playlist(streamer->ctx, playlist, size);
}

void turbo_streamer_hls_set_playlist_type(turbo_streamer_t *streamer,
                                          turbo_hls_playlist_type_t type) {
    if (!streamer || streamer->ops->protocol != TURBO_STREAMER_HLS) return;
    turbo_hls_streamer_set_playlist_type(streamer->ctx, type);
}

/* =============================================================================
 * RTMP 特定接口
 * ============================================================================= */

int turbo_streamer_rtmp_set_metadata(turbo_streamer_t *streamer,
                                     const char *key,
                                     const char *value) {
    if (!streamer || streamer->ops->protocol != TURBO_STREAMER_RTMP || !key || !value) {
        return -1;
    }
    return turbo_rtmp_streamer_set_metadata(streamer->ctx, key, value);
}
