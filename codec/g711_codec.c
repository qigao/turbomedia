/**
 * G.711 Audio Codec Implementation
 * 
 * Implements G.711 μ-law (PCMU) and A-law (PCMA) codecs
 * No external dependencies - pure C implementation
 */
#include "turbo_codec.h"
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * G.711 μ-law (PCMU) Implementation
 * ============================================================================= */

/* μ-law compression tables */
static const int16_t mulaw_exp_table[256] = {
    -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
    -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
    -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
    -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
     -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
     -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
     -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
     -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
     -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
     -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
      -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
      -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
      -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
      -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
      -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
       -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
     32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
     23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
     15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
     11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
      7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
      5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
      3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
      2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
      1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
      1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
       876,   844,   812,   780,   748,   716,   684,   652,
       620,   588,   556,   524,   492,   460,   428,   396,
       372,   356,   340,   324,   308,   292,   276,   260,
       244,   228,   212,   196,   180,   164,   148,   132,
       120,   112,   104,    96,    88,    80,    72,    64,
        56,    48,    40,    32,    24,    16,     8,     0
};

static uint8_t linear_to_mulaw(int16_t pcm_val) {
    int16_t mask;
    int16_t seg;
    uint8_t uval;

    /* Get the sign and the magnitude of the value */
    if (pcm_val < 0) {
        pcm_val = -pcm_val;
        mask = 0x7F;
    } else {
        mask = 0xFF;
    }
    
    if (pcm_val > 32635) pcm_val = 32635;
    
    /* Convert to μ-law */
    pcm_val += 0x84;
    
    /* Find segment */
    if (pcm_val >= 0x4000) seg = 7;
    else if (pcm_val >= 0x2000) seg = 6;
    else if (pcm_val >= 0x1000) seg = 5;
    else if (pcm_val >= 0x0800) seg = 4;
    else if (pcm_val >= 0x0400) seg = 3;
    else if (pcm_val >= 0x0200) seg = 2;
    else if (pcm_val >= 0x0100) seg = 1;
    else seg = 0;
    
    /* Combine segment and quantization bits */
    uval = (uint8_t)((seg << 4) | ((pcm_val >> (seg + 3)) & 0x0F));
    
    return (uint8_t)(uval ^ mask);
}

/* =============================================================================
 * G.711 A-law (PCMA) Implementation
 * ============================================================================= */

static const int16_t alaw_exp_table[256] = {
     -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736,
     -7552, -7296, -8064, -7808, -6528, -6272, -7040, -6784,
     -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368,
     -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392,
    -22016,-20992,-24064,-23040,-17920,-16896,-19968,-18944,
    -30208,-29184,-32256,-31232,-26112,-25088,-28160,-27136,
    -11008,-10496,-12032,-11520, -8960, -8448, -9984, -9472,
    -15104,-14592,-16128,-15616,-13056,-12544,-14080,-13568,
      -344,  -328,  -376,  -360,  -280,  -264,  -312,  -296,
      -472,  -456,  -504,  -488,  -408,  -392,  -440,  -424,
       -88,   -72,  -120,  -104,   -24,    -8,   -56,   -40,
      -216,  -200,  -248,  -232,  -152,  -136,  -184,  -168,
     -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184,
     -1888, -1824, -2016, -1952, -1632, -1568, -1760, -1696,
      -688,  -656,  -752,  -720,  -560,  -528,  -624,  -592,
      -944,  -912, -1008,  -976,  -816,  -784,  -880,  -848,
      5504,  5248,  6016,  5760,  4480,  4224,  4992,  4736,
      7552,  7296,  8064,  7808,  6528,  6272,  7040,  6784,
      2752,  2624,  3008,  2880,  2240,  2112,  2496,  2368,
      3776,  3648,  4032,  3904,  3264,  3136,  3520,  3392,
     22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944,
     30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136,
     11008, 10496, 12032, 11520,  8960,  8448,  9984,  9472,
     15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568,
       344,   328,   376,   360,   280,   264,   312,   296,
       472,   456,   504,   488,   408,   392,   440,   424,
        88,    72,   120,   104,    24,     8,    56,    40,
       216,   200,   248,   232,   152,   136,   184,   168,
      1376,  1312,  1504,  1440,  1120,  1056,  1248,  1184,
      1888,  1824,  2016,  1952,  1632,  1568,  1760,  1696,
       688,   656,   752,   720,   560,   528,   624,   592,
       944,   912,  1008,   976,   816,   784,   880,   848
};

static uint8_t linear_to_alaw(int16_t pcm_val) {
    int16_t mask;
    int16_t seg;
    uint8_t aval;

    if (pcm_val >= 0) {
        mask = 0xD5;
    } else {
        mask = 0x55;
        pcm_val = -pcm_val - 1;
    }

    if (pcm_val > 32635) pcm_val = 32635;

    /* Find segment */
    if (pcm_val >= 0x2000) seg = 7;
    else if (pcm_val >= 0x1000) seg = 6;
    else if (pcm_val >= 0x0800) seg = 5;
    else if (pcm_val >= 0x0400) seg = 4;
    else if (pcm_val >= 0x0200) seg = 3;
    else if (pcm_val >= 0x0100) seg = 2;
    else if (pcm_val >= 0x0080) seg = 1;
    else seg = 0;

    /* Combine segment and quantization bits */
    if (seg >= 2) {
        aval = (uint8_t)((seg << 4) | ((pcm_val >> (seg + 3)) & 0x0F));
    } else {
        aval = (uint8_t)((seg << 4) | ((pcm_val >> 4) & 0x0F));
    }

    return (uint8_t)(aval ^ mask);
}

/* =============================================================================
 * G.711 Codec Context
 * ============================================================================= */

typedef struct {
    int is_mulaw;  /* 1 for μ-law, 0 for A-law */
    int sample_rate;
    int channels;
} g711_context_t;

/* =============================================================================
 * Codec Operations
 * ============================================================================= */

static int g711_is_valid_sample_rate(int sample_rate) {
    return sample_rate == 8000 ||
           sample_rate == 16000 ||
           sample_rate == 24000 ||
           sample_rate == 48000;
}

static int g711_is_valid_frame_size(int frame_size_ms) {
    return frame_size_ms == 10 ||
           frame_size_ms == 20 ||
           frame_size_ms == 40 ||
           frame_size_ms == 60;
}

static int g711_is_valid_config(const turbo_audio_codec_config_t *cfg) {
    if (!cfg) return 1;

    return g711_is_valid_sample_rate(cfg->sample_rate) &&
           (cfg->channels == 1 || cfg->channels == 2) &&
           g711_is_valid_frame_size(cfg->frame_size_ms);
}

static void *g711_create_context(const void *config, int is_mulaw) {
    const turbo_audio_codec_config_t *cfg = (const turbo_audio_codec_config_t *)config;
    if (!g711_is_valid_config(cfg)) return NULL;

    g711_context_t *ctx = (g711_context_t *)calloc(1, sizeof(g711_context_t));
    if (!ctx) return NULL;

    ctx->sample_rate = cfg ? cfg->sample_rate : 8000;
    ctx->channels = cfg ? cfg->channels : 1;
    ctx->is_mulaw = is_mulaw;

    return ctx;
}

static void *g711_create_mulaw_encoder(const void *config) {
    return g711_create_context(config, 1);
}

static void *g711_create_mulaw_decoder(const void *config) {
    return g711_create_context(config, 1);
}

static void *g711_create_alaw_encoder(const void *config) {
    return g711_create_context(config, 0);
}

static void *g711_create_alaw_decoder(const void *config) {
    return g711_create_context(config, 0);
}

static void g711_destroy(void *ctx) {
    if (ctx) {
        free(ctx);
    }
}

static int g711_encode(void *ctx,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len,
                       turbo_encoded_frame_t *info) {
    g711_context_t *g711 = (g711_context_t *)ctx;
    if (!g711 || !input || !output || !output_len) {
        return TURBO_CODEC_ERR_INVALID;
    }
    
    /* Input is 16-bit PCM samples */
    size_t sample_count = input_len / 2;
    const int16_t *pcm = (const int16_t *)input;
    
    if (*output_len < sample_count) {
        return TURBO_CODEC_ERR_BUFFER;
    }
    
    /* Encode each sample */
    for (size_t i = 0; i < sample_count; i++) {
        if (g711->is_mulaw) {
            output[i] = linear_to_mulaw(pcm[i]);
        } else {
            output[i] = linear_to_alaw(pcm[i]);
        }
    }
    
    *output_len = sample_count;
    
    if (info) {
        info->data = output;
        info->len = sample_count;
        info->is_keyframe = 0;
        info->packet_count = 1;
    }
    
    return TURBO_CODEC_OK;
}

static int g711_decode(void *ctx,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len) {
    g711_context_t *g711 = (g711_context_t *)ctx;
    if (!g711 || !input || !output || !output_len) {
        return TURBO_CODEC_ERR_INVALID;
    }
    
    /* Output is 16-bit PCM samples */
    size_t required_size = input_len * 2;
    if (*output_len < required_size) {
        return TURBO_CODEC_ERR_BUFFER;
    }
    
    int16_t *pcm = (int16_t *)output;
    const int16_t *table = g711->is_mulaw ? mulaw_exp_table : alaw_exp_table;
    
    /* Decode each sample */
    for (size_t i = 0; i < input_len; i++) {
        pcm[i] = table[input[i]];
    }
    
    *output_len = required_size;
    return TURBO_CODEC_OK;
}

/* =============================================================================
 * Codec Operations Tables
 * ============================================================================= */

const turbo_codec_ops_t turbo_g711_pcmu_codec_ops = {
    .name = "pcmu",
    .type = TURBO_CODEC_TYPE_AUDIO,
    .payload_type = 0,  /* RTP PT 0 for PCMU */
    .clock_rate = 8000,
    
    .create_encoder = g711_create_mulaw_encoder,
    .create_decoder = g711_create_mulaw_decoder,
    .destroy = g711_destroy,
    .encode = g711_encode,
    .decode = g711_decode,
    
    .packetize = NULL,
    .depacketize = NULL,
    .request_keyframe = NULL,
    .set_bitrate = NULL,
    .plc = NULL
};

const turbo_codec_ops_t turbo_g711_pcma_codec_ops = {
    .name = "pcma",
    .type = TURBO_CODEC_TYPE_AUDIO,
    .payload_type = 8,  /* RTP PT 8 for PCMA */
    .clock_rate = 8000,
    
    .create_encoder = g711_create_alaw_encoder,
    .create_decoder = g711_create_alaw_decoder,
    .destroy = g711_destroy,
    .encode = g711_encode,
    .decode = g711_decode,
    
    .packetize = NULL,
    .depacketize = NULL,
    .request_keyframe = NULL,
    .set_bitrate = NULL,
    .plc = NULL
};
