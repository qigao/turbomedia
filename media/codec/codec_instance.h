#ifndef TURBO_CODEC_INSTANCE_H
#define TURBO_CODEC_INSTANCE_H

#include <turbo_codec.h>
#include <stdlib.h>

static inline turbo_codec_t *codec_instance_create_encoder(const turbo_codec_ops_t *ops, const void *config) {
    if (!ops || !ops->create_encoder) return NULL;

    void *encoder_ctx = ops->create_encoder(config);
    if (!encoder_ctx) return NULL;

    turbo_codec_t *codec = (turbo_codec_t *)calloc(1, sizeof(turbo_codec_t));
    if (!codec) {
        ops->destroy(encoder_ctx);
        return NULL;
    }

    codec->ops = ops;
    codec->encoder_ctx = encoder_ctx;
    codec->is_encoder = 1;

    return codec;
}

static inline turbo_codec_t *codec_instance_create_decoder(const turbo_codec_ops_t *ops, const void *config) {
    if (!ops || !ops->create_decoder) return NULL;

    void *decoder_ctx = ops->create_decoder(config);
    if (!decoder_ctx) return NULL;

    turbo_codec_t *codec = (turbo_codec_t *)calloc(1, sizeof(turbo_codec_t));
    if (!codec) {
        ops->destroy(decoder_ctx);
        return NULL;
    }

    codec->ops = ops;
    codec->decoder_ctx = decoder_ctx;
    codec->is_decoder = 1;

    return codec;
}

static inline void codec_instance_destroy(turbo_codec_t *codec) {
    if (!codec) return;

    if (codec->encoder_ctx && codec->ops->destroy) {
        codec->ops->destroy(codec->encoder_ctx);
    }
    if (codec->decoder_ctx && codec->ops->destroy) {
        codec->ops->destroy(codec->decoder_ctx);
    }

    free(codec);
}

static inline int codec_instance_encode(turbo_codec_t *codec,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len,
                       turbo_encoded_frame_t *info) {
    if (!codec || !codec->is_encoder || !codec->ops->encode) {
        return TURBO_CODEC_ERR_INVALID;
    }

    return codec->ops->encode(codec->encoder_ctx,
                              input, input_len,
                              output, output_len,
                              info);
}

static inline int codec_instance_decode(turbo_codec_t *codec,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len) {
    if (!codec || !codec->is_decoder || !codec->ops->decode) {
        return TURBO_CODEC_ERR_INVALID;
    }

    return codec->ops->decode(codec->decoder_ctx,
                              input, input_len,
                              output, output_len);
}

static inline void codec_instance_request_keyframe(turbo_codec_t *codec) {
    if (!codec || !codec->is_encoder || !codec->ops->request_keyframe) {
        return;
    }

    codec->ops->request_keyframe(codec->encoder_ctx);
}

static inline int codec_instance_plc(turbo_codec_t *codec, uint8_t *output, size_t *output_len) {
    if (!codec || !codec->is_decoder || !codec->ops->plc) {
        return TURBO_CODEC_ERR_INVALID;
    }

    return codec->ops->plc(codec->decoder_ctx, output, output_len);
}

static inline void codec_instance_set_bitrate(turbo_codec_t *codec, int bitrate_bps) {
    if (!codec || !codec->is_encoder || !codec->ops->set_bitrate) {
        return;
    }

    codec->ops->set_bitrate(codec->encoder_ctx, bitrate_bps);
}

#endif
