/**
 * Media Recording API
 *
 * Record audio/video streams to disk in standard formats
 */
#ifndef TURBO_RECORDER_H
#define TURBO_RECORDER_H

#include <turbo_export.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct turbo_recorder_t turbo_recorder_t;
typedef struct rtp_recorder_ctx_t rtp_recorder_ctx_t;

/* =============================================================================
 * Types and Enums
 * ============================================================================= */

typedef enum {
    TURBO_RECORDER_FORMAT_MP4,   /* MP4 container (H.264/H.265/AAC) */
    TURBO_RECORDER_FORMAT_WEBM,  /* WebM container (VP8/VP9/Opus) */
    TURBO_RECORDER_FORMAT_MKV    /* Matroska container (H.26x/VPx/AAC/Opus) */
} turbo_recorder_format_t;

typedef enum {
    TURBO_RECORDER_TRACK_AUDIO,
    TURBO_RECORDER_TRACK_VIDEO
} turbo_recorder_track_type_t;

typedef enum {
    /* Audio codecs */
    TURBO_RECORDER_CODEC_OPUS,
    TURBO_RECORDER_CODEC_AAC,
    TURBO_RECORDER_CODEC_PCM, /* Reserved; sample format is not yet specified. */

    /* Video codecs */
    TURBO_RECORDER_CODEC_H264,
    TURBO_RECORDER_CODEC_H265,
    TURBO_RECORDER_CODEC_VP8,
    TURBO_RECORDER_CODEC_VP9,

    /* G.711 variants must remain distinct when creating container streams. */
    TURBO_RECORDER_CODEC_PCMU,
    TURBO_RECORDER_CODEC_PCMA
} turbo_recorder_codec_t;

/* =============================================================================
 * Configuration Structures
 * ============================================================================= */

typedef struct {
    turbo_recorder_format_t format;
    const char *filename;

    /* Optional metadata */
    const char *title;
    const char *author;
    const char *comment;
} turbo_recorder_config_t;

typedef struct {
    turbo_recorder_track_type_t type;
    turbo_recorder_codec_t codec;

    /* Video configuration */
    int width;
    int height;
    int framerate;

    /* Audio configuration */
    int sample_rate;
    int channels;

    /* Encoded stream metadata. H.26x recording requires codec configuration. */
    const uint8_t *extradata;
    size_t extradata_size;

    /* RTP payload type, or 0 to use the codec default (except PCMU, where 0 is valid). */
    int rtp_payload_type;
} turbo_recorder_track_config_t;

/* =============================================================================
 * Recorder Management
 * ============================================================================= */

/**
 * Create recorder
 *
 * @param config Recorder configuration
 * @return Recorder context or NULL on error
 */
CXX_C_API turbo_recorder_t *turbo_recorder_create(const turbo_recorder_config_t *config);

/**
 * Destroy recorder
 *
 * Stops recording if active and frees resources
 */
CXX_C_API void turbo_recorder_destroy(turbo_recorder_t *rec);

/**
 * Add track to recording
 *
 * Must be called before starting recording
 *
 * @param rec Recorder context
 * @param config Track configuration
 * @return Track ID (>= 0) on success, -1 on error
 */
CXX_C_API int turbo_recorder_add_track(turbo_recorder_t *rec,
                             const turbo_recorder_track_config_t *config);

/**
 * Start recording
 *
 * Opens file and writes container header
 *
 * @param rec Recorder context
 * @return 0 on success, -1 on error
 */
CXX_C_API int turbo_recorder_start(turbo_recorder_t *rec);

/**
 * Write frame to track
 *
 * @param rec Recorder context
 * @param track_id Track ID (from add_track)
 * @param data Frame data
 * @param len Frame length
 * @param timestamp_us Frame timestamp in microseconds
 * @param is_keyframe 1 if keyframe, 0 otherwise
 * @return 0 on success, -1 on error
 */
CXX_C_API int turbo_recorder_write_frame(turbo_recorder_t *rec, int track_id,
                               const uint8_t *data, size_t len,
                               int64_t timestamp_us, int is_keyframe);

/**
 * Stop recording
 *
 * Finalizes container and closes file
 *
 * @param rec Recorder context
 * @return 0 on success, -1 on error
 */
CXX_C_API int turbo_recorder_stop(turbo_recorder_t *rec);

/**
 * Pause recording
 *
 * Flushes buffers, keeps file open, and temporarily rejects new frames
 *
 * @param rec Recorder context
 * @return 0 on success, -1 on error
 */
CXX_C_API int turbo_recorder_pause(turbo_recorder_t *rec);

/**
 * Resume recording
 *
 * Re-enables frame writes after pause
 *
 * @param rec Recorder context
 * @return 0 on success, -1 on error
 */
CXX_C_API int turbo_recorder_resume(turbo_recorder_t *rec);

/* =============================================================================
 * Statistics
 * ============================================================================= */

/**
 * Get recording statistics
 *
 * @param rec Recorder context
 * @param duration_us Output: recording duration in microseconds (can be NULL)
 * @param bytes_written Output: total bytes written (can be NULL)
 * @param track_count Output: number of tracks (can be NULL)
 */
CXX_C_API void turbo_recorder_get_stats(turbo_recorder_t *rec,
                              int64_t *duration_us,
                              int64_t *bytes_written,
                              int *track_count);

/**
 * Get track statistics
 *
 * @param rec Recorder context
 * @param track_id Track ID
 * @param bytes_written Output: bytes written for this track (can be NULL)
 * @param frames_written Output: frames written for this track (can be NULL)
 */
CXX_C_API void turbo_recorder_get_track_stats(turbo_recorder_t *rec, int track_id,
                                   int64_t *bytes_written,
                                   int64_t *frames_written);

/**
 * Check if currently recording
 *
 * @param rec Recorder context
 * @return 1 if recording, 0 otherwise
 */
CXX_C_API int turbo_recorder_is_recording(turbo_recorder_t *rec);

/* =============================================================================
 * Callbacks
 * ============================================================================= */

/**
 * Set error callback
 *
 * Called when errors occur during recording
 *
 * @param rec Recorder context
 * @param callback Callback function
 * @param user_data User data passed to callback
 */
CXX_C_API void turbo_recorder_set_error_callback(turbo_recorder_t *rec,
                                       void (*callback)(void *user_data,
                                                       const char *error),
                                       void *user_data);

/* =============================================================================
 * Utility: Recording from RTP Stream
 * ============================================================================= */

/**
 * Create RTP recording context
 *
 * Helper for recording directly from RTP streams
 *
 * The recorder and track must outlive the returned RTP context. Destroy all
 * RTP contexts before destroying the recorder.
 *
 * @param rec Recorder context
 * @param track_id Track ID to record to
 * @return RTP recorder context or NULL on error
 */
CXX_C_API rtp_recorder_ctx_t *turbo_recorder_create_rtp_context(turbo_recorder_t *rec,
                                                       int track_id);

/**
 * Destroy RTP recording context
 */
CXX_C_API void turbo_recorder_destroy_rtp_context(rtp_recorder_ctx_t *ctx);

/**
 * Write RTP frame
 *
 * Automatically converts RTP timestamps to recording timestamps
 *
 * @param ctx RTP recorder context
 * @param data Frame data
 * @param len Frame length
 * @param rtp_timestamp RTP timestamp
 * @param is_keyframe 1 if keyframe, 0 otherwise
 * @return 0 on success, -1 on error
 */
CXX_C_API int turbo_recorder_write_rtp_frame(rtp_recorder_ctx_t *ctx,
                                   const uint8_t *data, size_t len,
                                   uint32_t rtp_timestamp, int is_keyframe);

/**
 * Write one complete RTP packet.
 *
 * The recorder depacketizes and reassembles access units before muxing them.
 * The packet memory is borrowed only for the duration of this call.
 * Returns -1 for malformed, discarded, incomplete, or corrupt packets/frames.
 * A rejected access unit is never muxed.
 */
CXX_C_API int turbo_recorder_write_rtp_packet(rtp_recorder_ctx_t *ctx,
                                              const uint8_t *packet,
                                              size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_RECORDER_H */
