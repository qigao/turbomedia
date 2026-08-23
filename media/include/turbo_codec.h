/**
 * TurboNet Codec Abstraction
 *
 * Unified interface for audio/video codecs
 */
#ifndef TURBO_CODEC_H
#define TURBO_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define TURBO_CODEC_MAX_FRAME_SIZE      65536
#define TURBO_CODEC_MAX_PACKETS         64

/* Audio constants */
#define TURBO_AUDIO_SAMPLE_RATE_8K      8000
#define TURBO_AUDIO_SAMPLE_RATE_16K     16000
#define TURBO_AUDIO_SAMPLE_RATE_24K     24000
#define TURBO_AUDIO_SAMPLE_RATE_48K     48000

/* Video constants */
#define TURBO_VIDEO_MAX_WIDTH           3840
#define TURBO_VIDEO_MAX_HEIGHT          2160

/* =============================================================================
 * Types
 * ============================================================================= */

typedef enum {
    TURBO_CODEC_TYPE_AUDIO = 1,
    TURBO_CODEC_TYPE_VIDEO = 2
} turbo_codec_class_t;

typedef enum {
    TURBO_CODEC_OK = 0,
    TURBO_CODEC_ERR_NOMEM = -1,
    TURBO_CODEC_ERR_INVALID = -2,
    TURBO_CODEC_ERR_BUFFER = -3,
    TURBO_CODEC_ERR_CODEC = -4,
    TURBO_CODEC_ERR_NEED_MORE = -5
} turbo_codec_result_t;

/**
 * Audio codec configuration
 */
typedef struct {
    int sample_rate;        /* 8000, 16000, 24000, 48000 */
    int channels;           /* 1 or 2 */
    int bitrate;            /* Target bitrate (0 = auto) */
    int frame_size_ms;      /* 10, 20, 40, or 60 ms */
    int enable_fec;         /* Forward error correction */
    int enable_dtx;         /* Discontinuous transmission */
    int complexity;         /* 0-10, higher = better quality */
} turbo_audio_codec_config_t;

/**
 * Video codec configuration
 */
typedef struct {
    int width;
    int height;
    int framerate;          /* Target FPS */
    int bitrate;            /* Target bitrate in bps */
    int keyframe_interval;  /* Keyframe every N frames */
    int threads;            /* Encoder threads (0 = auto) */
    int quality;            /* 0-63 for VP8, lower = better */
} turbo_video_codec_config_t;

/**
 * Encoded frame info
 */
typedef struct {
    uint8_t *data;
    size_t len;
    uint32_t timestamp;
    int is_keyframe;
    int packet_count;       /* For video: number of RTP packets needed */
} turbo_encoded_frame_t;

/**
 * RTP packetization info
 */
typedef struct {
    const uint8_t *data;
    size_t len;
    int marker;             /* RTP marker bit */
    int fragment_start;     /* First fragment of frame */
    int fragment_end;       /* Last fragment of frame */
} turbo_rtp_fragment_t;

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
 * Register a codec
 *
 * @param ops   Codec operations
 * @return      0 on success
 */
TURBO_MEDIA_API int turbo_codec_register(const turbo_codec_ops_t *ops);

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
