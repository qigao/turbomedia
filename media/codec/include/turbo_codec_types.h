/**
 * TurboNet Codec Abstraction
 *
 * Shared codec types and buffer contracts
 */
#ifndef TURBO_CODEC_TYPES_H
#define TURBO_CODEC_TYPES_H

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

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CODEC_TYPES_H */
