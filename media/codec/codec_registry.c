/**
 * Codec Registry Implementation
 *
 * Manages available audio/video codecs
 */
#include "turbo_codec.h"
#include "codec_instance.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Codec-owned validators; keep capability rules out of the registry/media layer. */
int turbo_g711_validate_audio_config(const turbo_audio_codec_config_t *config);
#ifdef TURBO_MEDIA_HAS_OPUS
int turbo_opus_validate_audio_config(const turbo_audio_codec_config_t *config);
#endif

/* =============================================================================
 * Registry
 * ============================================================================= */

#define MAX_CODECS 16

static const turbo_codec_ops_t *g_codecs[MAX_CODECS];
static int g_codec_count = 0;
static atomic_int g_initialized = 0;

/* =============================================================================
 * Registry Functions
 * ============================================================================= */

void turbo_codec_registry_init(void) {
    int expected = 0;

    /* Thread-safe initialization using atomic CAS */
    if (atomic_load_explicit(&g_initialized, memory_order_acquire) != 0) return;
    if (!atomic_compare_exchange_strong_explicit(&g_initialized, &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return;
    }

    g_codec_count = 0;
    memset((void *)g_codecs, 0, sizeof(g_codecs));

    /* Register G.711 codecs (always available) */
    turbo_codec_register(&turbo_g711_pcmu_codec_ops);
    turbo_codec_register(&turbo_g711_pcma_codec_ops);

    /* Register built-in codecs */
#ifdef TURBO_MEDIA_HAS_OPUS
    turbo_codec_register(&turbo_opus_codec_ops);
#endif

#ifdef TURBO_MEDIA_HAS_VPX
    turbo_codec_register(&turbo_vp8_codec_ops);
    turbo_codec_register(&turbo_vp9_codec_ops);
#endif

#ifdef TURBO_MEDIA_HAS_H264
    turbo_codec_register(&turbo_h264_codec_ops);
#endif

#ifdef TURBO_MEDIA_HAS_H265
    turbo_codec_register(&turbo_h265_codec_ops);
#endif
}

void turbo_codec_registry_shutdown(void) {
    g_codec_count = 0;
    atomic_store_explicit(&g_initialized, 0, memory_order_release);
}

int turbo_codec_register(const turbo_codec_ops_t *ops) {
    if (!ops || !ops->name) return -1;
    if (g_codec_count >= MAX_CODECS) return -1;

    /* Check for duplicate */
    for (int i = 0; i < g_codec_count; i++) {
        if (strcmp(g_codecs[i]->name, ops->name) == 0) {
            return -1;  /* Already registered */
        }
    }

    g_codecs[g_codec_count++] = ops;
    return 0;
}

const turbo_codec_ops_t *turbo_codec_find_by_pt(int payload_type) {
    for (int i = 0; i < g_codec_count; i++) {
        if (g_codecs[i]->payload_type == payload_type) {
            return g_codecs[i];
        }
    }
    return NULL;
}

const turbo_codec_ops_t *turbo_codec_find_by_name(const char *name) {
    if (!name) return NULL;

    for (int i = 0; i < g_codec_count; i++) {
        if (strcmp(g_codecs[i]->name, name) == 0) {
            return g_codecs[i];
        }
    }
    return NULL;
}

int turbo_codec_audio_rtp_clock_rate(const char *name) {
    if (!name) return 0;
    if (strcmp(name, "pcmu") == 0) {
        return turbo_g711_pcmu_codec_ops.clock_rate;
    }
    if (strcmp(name, "pcma") == 0) {
        return turbo_g711_pcma_codec_ops.clock_rate;
    }
#ifdef TURBO_MEDIA_HAS_OPUS
    if (strcmp(name, "opus") == 0) {
        return turbo_opus_codec_ops.clock_rate;
    }
#endif
    return 0;
}

int turbo_codec_validate_audio_config(
    const char *name, const turbo_audio_codec_config_t *config) {
    if (!name || !config) {
        return TURBO_CODEC_ERR_INVALID;
    }
    if (strcmp(name, "pcmu") == 0 || strcmp(name, "pcma") == 0) {
        return turbo_g711_validate_audio_config(config);
    }
#ifdef TURBO_MEDIA_HAS_OPUS
    if (strcmp(name, "opus") == 0) {
        return turbo_opus_validate_audio_config(config);
    }
#endif
    return TURBO_CODEC_ERR_INVALID;
}

/* =============================================================================
 * Codec Instance Functions
 * ============================================================================= */

turbo_codec_t *turbo_codec_create_encoder(const char *name, const void *config) {
    return codec_instance_create_encoder(turbo_codec_find_by_name(name), config);
}

turbo_codec_t *turbo_codec_create_decoder(const char *name, const void *config) {
    return codec_instance_create_decoder(turbo_codec_find_by_name(name), config);
}

void turbo_codec_destroy(turbo_codec_t *codec) {
    codec_instance_destroy(codec);
}

int turbo_codec_encode(turbo_codec_t *codec,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len,
                       turbo_encoded_frame_t *info) {
    return codec_instance_encode(codec, input, input_len, output, output_len, info);
}

int turbo_codec_decode(turbo_codec_t *codec,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len) {
    return codec_instance_decode(codec, input, input_len, output, output_len);
}

void turbo_codec_request_keyframe(turbo_codec_t *codec) {
    codec_instance_request_keyframe(codec);
}

int turbo_codec_plc(turbo_codec_t *codec, uint8_t *output, size_t *output_len) {
    return codec_instance_plc(codec, output, output_len);
}

void turbo_codec_set_bitrate(turbo_codec_t *codec, int bitrate_bps) {
    codec_instance_set_bitrate(codec, bitrate_bps);
}
