/**
 * Opus Codec Implementation
 *
 * Wraps libopus for audio encoding/decoding
 */
#include "turbo_codec.h"

#ifdef TURBO_MEDIA_HAS_OPUS

#include <opus/opus.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Constants
 * ============================================================================= */

#define OPUS_PAYLOAD_TYPE       111
#define OPUS_CLOCK_RATE         48000
#define OPUS_MAX_FRAME_SIZE     5760    /* 120ms at 48kHz */
#define OPUS_MAX_PACKET_SIZE    4000

/* =============================================================================
 * Context Structure
 * ============================================================================= */

typedef struct {
    int is_encoder_ctx;
    OpusEncoder *encoder;
    int sample_rate;
    int channels;
    int frame_size;         /* Samples per frame */
    int bitrate;
    int fec_enabled;
    int dtx_enabled;
} opus_encoder_ctx_t;

typedef struct {
    int is_encoder_ctx;
    OpusDecoder *decoder;
    int sample_rate;
    int channels;
    int frame_size;         /* Samples per frame for PLC */
} opus_decoder_ctx_t;

static int opus_config_valid(const turbo_audio_codec_config_t *cfg) {
    if (!cfg || (cfg->sample_rate != 8000 && cfg->sample_rate != 12000 &&
                 cfg->sample_rate != 16000 && cfg->sample_rate != 24000 &&
                 cfg->sample_rate != 48000))
        return 0;
    if (cfg->channels < 1 || cfg->channels > 2 || cfg->bitrate < 0 ||
        (cfg->frame_size_ms != 10 && cfg->frame_size_ms != 20 &&
         cfg->frame_size_ms != 40 && cfg->frame_size_ms != 60) ||
        cfg->complexity < 0 || cfg->complexity > 10)
        return 0;
    return 1;
}

/* =============================================================================
 * Encoder Functions
 * ============================================================================= */

static void *opus_create_encoder(const void *config) {
    const turbo_audio_codec_config_t *cfg = (const turbo_audio_codec_config_t *)config;
    if (!opus_config_valid(cfg)) return NULL;

    opus_encoder_ctx_t *ctx = (opus_encoder_ctx_t *)calloc(1, sizeof(opus_encoder_ctx_t));
    if (!ctx) return NULL;

    int sample_rate = cfg->sample_rate;
    int channels = cfg->channels;
    int frame_size_ms = cfg->frame_size_ms;
    int frame_size = (sample_rate * frame_size_ms) / 1000;

    /* Create encoder */
    int error;
    ctx->encoder = opus_encoder_create(sample_rate, channels,
                                         OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !ctx->encoder) {
        free(ctx);
        return NULL;
    }

    ctx->sample_rate = sample_rate;
    ctx->channels = channels;
    ctx->frame_size = frame_size;
    ctx->is_encoder_ctx = 1;

    /* Configure bitrate */
    ctx->bitrate = cfg->bitrate > 0 ? cfg->bitrate : 32000;
    opus_encoder_ctl(ctx->encoder, OPUS_SET_BITRATE(ctx->bitrate));

    /* Configure complexity */
    int complexity = cfg->complexity;
    opus_encoder_ctl(ctx->encoder, OPUS_SET_COMPLEXITY(complexity));

    /* Configure FEC */
    ctx->fec_enabled = cfg->enable_fec;
    if (ctx->fec_enabled) {
        opus_encoder_ctl(ctx->encoder, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(ctx->encoder, OPUS_SET_PACKET_LOSS_PERC(10));
    }

    /* Configure DTX */
    ctx->dtx_enabled = cfg->enable_dtx;
    if (ctx->dtx_enabled) {
        opus_encoder_ctl(ctx->encoder, OPUS_SET_DTX(1));
    }

    return ctx;
}

static int turbo_opus_encode(void *ctx_ptr,
                              const uint8_t *input, size_t input_len,
                              uint8_t *output, size_t *output_len,
                              turbo_encoded_frame_t *info) {
    opus_encoder_ctx_t *ctx = (opus_encoder_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->encoder) return TURBO_CODEC_ERR_INVALID;

    /* Input is 16-bit PCM samples */
    const opus_int16 *pcm = (const opus_int16 *)input;
    int frame_samples = ctx->frame_size;
    size_t expected_len = frame_samples * ctx->channels * sizeof(opus_int16);

    if (input_len < expected_len) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    if (*output_len < OPUS_MAX_PACKET_SIZE) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    /* Encode */
    int encoded_bytes = opus_encode(ctx->encoder, pcm, frame_samples,
                                     output, (opus_int32)*output_len);
    if (encoded_bytes < 0) {
        return TURBO_CODEC_ERR_CODEC;
    }

    *output_len = encoded_bytes;

    if (info) {
        info->data = output;
        info->len = encoded_bytes;
        info->is_keyframe = 1;  /* Audio frames are always "key" */
        info->packet_count = 1;
        /* Timestamp in samples at 48kHz clock rate */
        info->timestamp = 0;    /* Caller should set this */
    }

    return TURBO_CODEC_OK;
}

/* =============================================================================
 * Decoder Functions
 * ============================================================================= */

static void *opus_create_decoder(const void *config) {
    const turbo_audio_codec_config_t *cfg = (const turbo_audio_codec_config_t *)config;
    if (!opus_config_valid(cfg)) return NULL;

    opus_decoder_ctx_t *ctx = (opus_decoder_ctx_t *)calloc(1, sizeof(opus_decoder_ctx_t));
    if (!ctx) return NULL;

    int sample_rate = cfg->sample_rate;
    int channels = cfg->channels;
    int frame_size_ms = cfg->frame_size_ms;

    /* Create decoder */
    int error;
    ctx->decoder = opus_decoder_create(sample_rate, channels, &error);
    if (error != OPUS_OK || !ctx->decoder) {
        free(ctx);
        return NULL;
    }

    ctx->sample_rate = sample_rate;
    ctx->channels = channels;
    ctx->frame_size = (sample_rate * frame_size_ms) / 1000;
    ctx->is_encoder_ctx = 0;

    return ctx;
}

static int turbo_opus_decode(void *ctx_ptr,
                              const uint8_t *input, size_t input_len,
                              uint8_t *output, size_t *output_len) {
    opus_decoder_ctx_t *ctx = (opus_decoder_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->decoder) return TURBO_CODEC_ERR_INVALID;

    /* Output is 16-bit PCM samples */
    opus_int16 *pcm = (opus_int16 *)output;
    size_t max_samples = *output_len / (ctx->channels * sizeof(opus_int16));

    if (max_samples < (size_t)ctx->frame_size) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    /* Decode */
    int decoded_samples = opus_decode(ctx->decoder,
                                       input, (opus_int32)input_len,
                                       pcm, (int)max_samples,
                                       0);  /* No FEC decode */
    if (decoded_samples < 0) {
        return TURBO_CODEC_ERR_CODEC;
    }

    *output_len = decoded_samples * ctx->channels * sizeof(opus_int16);

    return TURBO_CODEC_OK;
}

static int turbo_opus_plc(void *ctx_ptr, uint8_t *output, size_t *output_len) {
    opus_decoder_ctx_t *ctx = (opus_decoder_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->decoder) return TURBO_CODEC_ERR_INVALID;

    opus_int16 *pcm = (opus_int16 *)output;
    size_t max_samples = *output_len / (ctx->channels * sizeof(opus_int16));

    if (max_samples < (size_t)ctx->frame_size) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    /* PLC - pass NULL input */
    int decoded_samples = opus_decode(ctx->decoder,
                                       NULL, 0,
                                       pcm, ctx->frame_size,
                                       0);
    if (decoded_samples < 0) {
        return TURBO_CODEC_ERR_CODEC;
    }

    *output_len = decoded_samples * ctx->channels * sizeof(opus_int16);

    return TURBO_CODEC_OK;
}

static void turbo_opus_set_bitrate(void *ctx_ptr, int bitrate_bps) {
    opus_encoder_ctx_t *ctx = (opus_encoder_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->encoder || bitrate_bps <= 0) {
        return;
    }

    ctx->bitrate = bitrate_bps;
    opus_encoder_ctl(ctx->encoder, OPUS_SET_BITRATE(ctx->bitrate));
}

/* =============================================================================
 * Common Functions
 * ============================================================================= */

static void turbo_opus_destroy(void *ctx_ptr) {
    opus_encoder_ctx_t *enc = (opus_encoder_ctx_t *)ctx_ptr;
    opus_decoder_ctx_t *dec = (opus_decoder_ctx_t *)ctx_ptr;

    if (!ctx_ptr) {
        return;
    }

    if (enc->is_encoder_ctx) {
        opus_encoder_destroy(enc->encoder);
        free(enc);
        return;
    }

    if (dec->decoder) {
        opus_decoder_destroy(dec->decoder);
        free(dec);
    }
}

/* =============================================================================
 * Codec Operations
 * ============================================================================= */

const turbo_codec_ops_t turbo_opus_codec_ops = {
    .name = "opus",
    .type = TURBO_CODEC_TYPE_AUDIO,
    .payload_type = OPUS_PAYLOAD_TYPE,
    .clock_rate = OPUS_CLOCK_RATE,
    .create_encoder = opus_create_encoder,
    .create_decoder = opus_create_decoder,
    .destroy = turbo_opus_destroy,
    .encode = turbo_opus_encode,
    .decode = turbo_opus_decode,
    .packetize = NULL,      /* Audio doesn't need fragmentation */
    .depacketize = NULL,
    .request_keyframe = NULL,
    .plc = turbo_opus_plc,
    .set_bitrate = turbo_opus_set_bitrate
};

#endif /* TURBO_MEDIA_HAS_OPUS */
