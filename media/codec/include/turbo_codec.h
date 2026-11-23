#ifndef TURBO_CODEC_H
#define TURBO_CODEC_H

#include <turbo_codec_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct turbo_codec_s turbo_codec_t;

/**
 * Codec operations vtable
 */
typedef struct {
    const char *name;
    turbo_codec_class_t type;
    int payload_type;       /* RTP payload type */
    int clock_rate;         /* RTP clock rate */

    /* Create encoder/decoder context */
    void *(*create_encoder)(const void *config);
    void *(*create_decoder)(const void *config);
    void (*destroy)(void *ctx);

    /* Encode raw frame to compressed data */
    int (*encode)(void *ctx,
                  const uint8_t *input, size_t input_len,
                  uint8_t *output, size_t *output_len,
                  turbo_encoded_frame_t *info);

    /* Decode compressed data to raw frame */
    int (*decode)(void *ctx,
                  const uint8_t *input, size_t input_len,
                  uint8_t *output, size_t *output_len);

    /* Packetize for RTP (video only) */
    int (*packetize)(void *ctx,
                     const turbo_encoded_frame_t *frame,
                     turbo_rtp_fragment_t *fragments, int max_fragments,
                     size_t mtu);

    /* Depacketize from RTP (video only) */
    int (*depacketize)(void *ctx,
                       const uint8_t *rtp_payload, size_t payload_len,
                       uint8_t *output, size_t *output_len,
                       int *complete);

    /* Request keyframe (video encoder only) */
    void (*request_keyframe)(void *ctx);

    /* Set target bitrate (encoder only) */
    void (*set_bitrate)(void *ctx, int bitrate_bps);

    /* Get packet loss concealment frame (audio decoder only) */
    int (*plc)(void *ctx, uint8_t *output, size_t *output_len);

} turbo_codec_ops_t;

/**
 * Codec instance
 */
struct turbo_codec_s {
    const turbo_codec_ops_t *ops;
    void *encoder_ctx;
    void *decoder_ctx;
    int is_encoder;
    int is_decoder;
};

/* =============================================================================
 * Codec Registry Functions
 * ============================================================================= */

/**
 * Initialize codec registry
 *
 * Registers all available codecs
 */
TURBO_MEDIA_API void turbo_codec_registry_init(void);

/**
 * Shutdown codec registry
 */
TURBO_MEDIA_API void turbo_codec_registry_shutdown(void);

/**
 * Find codec by payload type
 *
 * @param payload_type  RTP payload type
 * @return              Codec ops, or NULL if not found
 */
TURBO_MEDIA_API const turbo_codec_ops_t *turbo_codec_find_by_pt(int payload_type);

/**
 * Find codec by name
 *
 * @param name  Codec name (e.g., "opus", "vp8")
 * @return      Codec ops, or NULL if not found
 */
TURBO_MEDIA_API const turbo_codec_ops_t *turbo_codec_find_by_name(const char *name);

/**
 * Return the codec-owned RTP clock rate for a built-in audio codec without
 * requiring registry initialization.
 *
 * @return Positive RTP clock rate, or 0 for unknown/unavailable audio codecs.
 */
TURBO_MEDIA_API int turbo_codec_audio_rtp_clock_rate(const char *name);

/**
 * Register a codec
 *
 * @param ops   Codec operations
 * @return      0 on success
 */
TURBO_MEDIA_API int turbo_codec_register(const turbo_codec_ops_t *ops);

/**
 * Validate an audio configuration against a built-in audio codec's exact
 * capabilities without allocating an encoder/decoder instance.
 *
 * This helper covers the codec identities accepted by TurboMedia media-track
 * configuration; arbitrary externally registered codec names are not implied.
 *
 * @param name    Built-in audio codec name.
 * @param config  Required audio configuration.
 * @return        TURBO_CODEC_OK when supported, TURBO_CODEC_ERR_INVALID otherwise.
 */
TURBO_MEDIA_API int turbo_codec_validate_audio_config(
    const char *name, const turbo_audio_codec_config_t *config);

/* =============================================================================
 * Codec Instance Functions
 * ============================================================================= */

/**
 * Create codec instance for encoding
 *
 * @param name      Codec name
 * @param config    Codec-specific configuration
 * @return          Codec instance, or NULL on error
 */
TURBO_MEDIA_API turbo_codec_t *turbo_codec_create_encoder(const char *name, const void *config);

/**
 * Create codec instance for decoding
 *
 * @param name      Codec name
 * @param config    Codec-specific configuration
 * @return          Codec instance, or NULL on error
 */
TURBO_MEDIA_API turbo_codec_t *turbo_codec_create_decoder(const char *name, const void *config);

/**
 * Destroy codec instance
 */
TURBO_MEDIA_API void turbo_codec_destroy(turbo_codec_t *codec);

/**
 * Encode a frame
 *
 * @param codec         Codec instance
 * @param input         Raw input data
 * @param input_len     Input length
 * @param output        Output buffer
 * @param output_len    In: buffer size, Out: encoded size
 * @param info          Output: frame information
 * @return              0 on success
 */
TURBO_MEDIA_API int turbo_codec_encode(turbo_codec_t *codec,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len,
                       turbo_encoded_frame_t *info);

/**
 * Decode a frame
 *
 * @param codec         Codec instance
 * @param input         Encoded input data
 * @param input_len     Input length
 * @param output        Output buffer
 * @param output_len    In: buffer size, Out: decoded size
 * @return              0 on success
 */
TURBO_MEDIA_API int turbo_codec_decode(turbo_codec_t *codec,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len);

/**
 * Request keyframe from encoder
 */
TURBO_MEDIA_API void turbo_codec_request_keyframe(turbo_codec_t *codec);

/**
 * Get packet loss concealment frame
 */
TURBO_MEDIA_API int turbo_codec_plc(turbo_codec_t *codec, uint8_t *output, size_t *output_len);

/**
 * Set target bitrate
 */
TURBO_MEDIA_API void turbo_codec_set_bitrate(turbo_codec_t *codec, int bitrate_bps);

/* =============================================================================
 * Built-in Codec Declarations
 * ============================================================================= */

#ifdef TURBO_MEDIA_HAS_OPUS
extern const turbo_codec_ops_t turbo_opus_codec_ops;
#endif

#ifdef TURBO_MEDIA_HAS_VPX
extern const turbo_codec_ops_t turbo_vp8_codec_ops;
extern const turbo_codec_ops_t turbo_vp9_codec_ops;
#endif

/* G.711 - Always available (no external dependencies) */
extern const turbo_codec_ops_t turbo_g711_pcmu_codec_ops;
extern const turbo_codec_ops_t turbo_g711_pcma_codec_ops;

#ifdef TURBO_MEDIA_HAS_H264
extern const turbo_codec_ops_t turbo_h264_codec_ops;
#endif

#ifdef TURBO_MEDIA_HAS_H265
extern const turbo_codec_ops_t turbo_h265_codec_ops;
#endif

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CODEC_H */
