/**
 * TurboNet Media API
 *
 * Audio/video media track management for WebRTC
 * Supports voice chat, video calls, live streaming, and screen sharing
 */
#ifndef TURBO_MEDIA_ENGINE_H
#define TURBO_MEDIA_ENGINE_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>
#include <turbo_recognition.h>
#include <turbo_speech.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct turbo_dc_peer_s turbo_dc_peer_t;
typedef struct turbo_media_context_s turbo_media_context_t;
typedef struct turbo_media_track_s turbo_media_track_t;

/* =============================================================================
 * Constants
 * ============================================================================= */

#define TURBO_MEDIA_MAX_TRACKS 8
#define TURBO_MEDIA_JITTER_DEFAULT 50 /* Default jitter buffer delay (ms) */
#define TURBO_MEDIA_JITTER_MIN 10
#define TURBO_MEDIA_JITTER_MAX 500

/* =============================================================================
 * Types and Enums
 * ============================================================================= */

typedef enum {
  TURBO_RTC_MEDIA_TRACK_AUDIO = 1,
  TURBO_RTC_MEDIA_TRACK_VIDEO = 2
} turbo_rtc_media_track_type_t;

typedef enum {
  TURBO_MEDIA_DIRECTION_SENDONLY = 1,
  TURBO_MEDIA_DIRECTION_RECVONLY = 2,
  TURBO_MEDIA_DIRECTION_SENDRECV = 3
} turbo_media_direction_t;

typedef enum {
  TURBO_MEDIA_STATE_IDLE = 0,
  TURBO_MEDIA_STATE_STARTING,
  TURBO_MEDIA_STATE_ACTIVE,
  TURBO_MEDIA_STATE_STOPPING,
  TURBO_MEDIA_STATE_STOPPED,
  TURBO_MEDIA_STATE_ERROR
} turbo_media_state_t;

typedef enum {
  TURBO_CODEC_OPUS = 111, /* Opus audio (dynamic PT) */
  TURBO_CODEC_PCMU = 0,   /* G.711 μ-law (static PT) */
  TURBO_CODEC_PCMA = 8,   /* G.711 A-law (static PT) */
  TURBO_CODEC_VP8 = 96,   /* VP8 video (dynamic PT) */
  TURBO_CODEC_VP9 = 98,   /* VP9 video (dynamic PT) */
  TURBO_CODEC_H264 = 102, /* H.264 video (dynamic PT) */
  TURBO_CODEC_H265 = 103  /* H.265 video (dynamic PT) */
} turbo_codec_type_t;

#if defined(TURBO_MEDIA_PRODUCT_CLIENT)
typedef enum {
  TURBO_MEDIA_CAPTURE_NONE = 0,
  TURBO_MEDIA_CAPTURE_MICROPHONE,
  TURBO_MEDIA_CAPTURE_CAMERA,
  TURBO_MEDIA_CAPTURE_SCREEN,
  TURBO_MEDIA_CAPTURE_WINDOW
} turbo_media_capture_type_t;
#endif

/* =============================================================================
 * Configuration Structures
 * ============================================================================= */

/**
 * Audio configuration
 */
typedef struct {
  int sample_rate;   /* 8000, 16000, 24000, 48000 */
  int channels;      /* 1 (mono) or 2 (stereo) */
  int bitrate;       /* Target bitrate in bps (0 = auto) */
  int frame_size_ms; /* 10, 20, 40, or 60 ms */
  int enable_fec;    /* Forward error correction */
  int enable_dtx;    /* Discontinuous transmission */
} turbo_audio_config_t;

/**
 * Video configuration
 */
typedef struct {
  int width;
  int height;
  int framerate;         /* Target FPS */
  int bitrate;           /* Target bitrate in bps */
  int keyframe_interval; /* Keyframe every N frames */
} turbo_video_config_t;

/**
 * Screen capture configuration
 */
typedef struct {
  int monitor_index;  /* -1 for all monitors */
  int framerate;      /* Target FPS */
  int capture_cursor; /* Include cursor */
} turbo_screen_config_t;

/**
 * Media track configuration
 */
typedef struct {
  turbo_rtc_media_track_type_t type;
  turbo_media_direction_t direction;
  turbo_codec_type_t codec;

  union {
    turbo_audio_config_t audio;
    turbo_video_config_t video;
  };

  uint32_t jitter_buffer_ms; /* 0 = default */
} turbo_media_track_config_t;

#if defined(TURBO_MEDIA_PRODUCT_CLIENT)
/**
 * Capture configuration
 */
typedef struct {
  turbo_media_capture_type_t type;
  int device_index; /* Device index or -1 for default */

  union {
    turbo_audio_config_t audio;
    turbo_video_config_t video;
    turbo_screen_config_t screen;
  };
} turbo_capture_config_t;
#endif

/* =============================================================================
 * Statistics
 * ============================================================================= */

typedef struct {
  /* Sending stats */
  uint64_t packets_sent;
  uint64_t bytes_sent;
  uint64_t frames_sent;
  uint32_t encode_time_ms; /* Average encode time */

  /* Receiving stats */
  uint64_t packets_recv;
  uint64_t bytes_recv;
  uint64_t frames_recv;
  uint64_t packets_lost;
  uint32_t decode_time_ms; /* Average decode time */

  /* Quality metrics */
  uint32_t jitter_ms;
  uint32_t rtt_ms;
  uint32_t bitrate_bps; /* Current bitrate */
  float fraction_lost;  /* 0.0 - 1.0 */
} turbo_media_stats_t;

/* =============================================================================
 * Callbacks
 * ============================================================================= */

/**
 * Called when decoded audio/video frame is available
 *
 * @param track     Media track
 * @param data      Raw frame data (PCM for audio, YUV/RGB for video)
 * @param len       Data length in bytes
 * @param timestamp RTP timestamp
 * @param user_data User-provided context
 */
typedef void (*turbo_rtc_media_frame_cb)(turbo_media_track_t *track, const uint8_t *data,
                                         size_t len, uint64_t timestamp, void *user_data);

/**
 * Called when media track state changes
 */
typedef void (*turbo_media_state_cb)(turbo_media_track_t *track, turbo_media_state_t state,
                                     void *user_data);

/**
 * Called when keyframe is needed (video)
 */
typedef void (*turbo_media_keyframe_cb)(turbo_media_track_t *track, void *user_data);

/**
 * Called when a raw RTP packet for a receive track is available.
 *
 * The packet bytes are the
 * parsed/decrypted RTP wire packet as received on the
 * transport. The callback must consume or
 * copy the data synchronously.
 */
typedef void (*turbo_media_rtp_packet_cb)(turbo_media_track_t *track, const uint8_t *packet,
                                          size_t len, void *user_data);

/* =============================================================================
 * Context Functions
 * ============================================================================= */

/**
 * Create media context attached to DataChannel peer
 *
 * Uses the peer's DTLS session for SRTP key derivation
 *
 * @param peer      DataChannel peer; it must outlive the returned context
 * @param user_data
 * User-provided context
 * @return          Media context, or NULL on error
 */
TURBO_MEDIA_API turbo_media_context_t *turbo_media_create(turbo_dc_peer_t *peer, void *user_data);

/**
 * Destroy media context and all tracks, and detach it from the DataChannel peer.
 */
TURBO_MEDIA_API void turbo_media_destroy(turbo_media_context_t *ctx);

/**
 * Get user data from context
 */
TURBO_MEDIA_API void *turbo_media_get_user_data(turbo_media_context_t *ctx);

/**
 * Initialize SRTP sessions for all tracks using negotiated keys
 *
 * Must be called after DTLS handshake is complete
 *
 * @param ctx   Media context
 * @return      0 on success, -1 if keys not ready
 */
TURBO_MEDIA_API int turbo_media_setup_srtp(turbo_media_context_t *ctx);

/**
 * Process media timers (call periodically from event loop)
 *
 * Handles RTCP sending, jitter buffer playout, etc.
 *
 * @param ctx       Media context
 * @return          Time until next timer in ms, or 0 if none
 */
TURBO_MEDIA_API int turbo_media_handle_timers(turbo_media_context_t *ctx);

/**
 * Feed received SRTP/SRTCP packet
 *
 * Called when media data is received from network. The context must have
 * completed turbo_media_setup_srtp(); unauthenticated packets are rejected.
 *
 * @param ctx       Media context
 * @param data      Packet data
 * @param len       Packet length
 * @return          0 on success, -1 on error
 */
TURBO_MEDIA_API int turbo_media_feed_data(turbo_media_context_t *ctx, const uint8_t *data, size_t len);

/**
 * Get the number of tracks currently attached to a media context.
 */
TURBO_MEDIA_API int turbo_media_get_track_count(turbo_media_context_t *ctx);

/**
 * Get a track by index from a media context.
 */
TURBO_MEDIA_API turbo_media_track_t *turbo_media_get_track(turbo_media_context_t *ctx, int index);

/* =============================================================================
 * Track Functions
 * ============================================================================= */

/**
 * Add media track
 *
 * @param ctx       Media context
 * @param config    Track configuration
 * @return          Track handle, or NULL on error
 */
TURBO_MEDIA_API turbo_media_track_t *turbo_media_add_track(turbo_media_context_t *ctx,
                                                     const turbo_media_track_config_t *config);

#if defined(TURBO_MEDIA_PRODUCT_CLIENT)
/**
 * Add screen sharing track (convenience)
 *
 * @param ctx           Media context
 * @param screen_index  Screen index (-1 for primary)
 * @param fps           Target framerate
 * @return              Track handle, or NULL on error
 */
TURBO_MEDIA_API turbo_media_track_t *turbo_media_add_screen_track(turbo_media_context_t *ctx,
                                                            int screen_index, int fps);

/**
 * Add audio track (convenience)
 */
TURBO_MEDIA_API turbo_media_track_t *turbo_media_add_audio_track(turbo_media_context_t *ctx,
                                                           int device_index);

/**
 * Add video track (convenience)
 */
TURBO_MEDIA_API turbo_media_track_t *turbo_media_add_video_track(turbo_media_context_t *ctx,
                                                           int device_index);
#endif

/**
 * Remove media track
 */
TURBO_MEDIA_API void turbo_media_remove_track(turbo_media_track_t *track);

#if defined(TURBO_MEDIA_PRODUCT_CLIENT)
/**
 * Set capture source for track
 *
 * @param track     Media track
 * @param config    Capture configuration
 * @return          0 on success, -1 on error
 */
TURBO_MEDIA_API int turbo_media_track_set_capture(turbo_media_track_t *track,
                                            const turbo_capture_config_t *config);
#endif

/**
 * Attach a running ASR session to the raw microphone PCM path.
 *
 * The track keeps a borrowed ASR pointer and does not destroy it. The formats
 * must match. Attach and detach are control-plane operations allowed only
 * while the track is IDLE or STOPPED. ASR rejection never interrupts the
 * WebRTC send path.
 */
TURBO_MEDIA_API int turbo_media_track_attach_asr(turbo_media_track_t *track, turbo_asr_t *asr);
TURBO_MEDIA_API int turbo_media_track_detach_asr(turbo_media_track_t *track, turbo_asr_t *asr);

/**
 * Attach real-time voice activity and voiceprint analysis to microphone PCM.
 * The track borrows both handles. Attach/detach is allowed only while the
 * track is IDLE or STOPPED and exact PCM formats must match.
 */
TURBO_MEDIA_API int turbo_media_track_attach_voice_detector(turbo_media_track_t *track,
                                                      turbo_voice_detector_t *detector);
TURBO_MEDIA_API int turbo_media_track_detach_voice_detector(turbo_media_track_t *track,
                                                      turbo_voice_detector_t *detector);
TURBO_MEDIA_API int turbo_media_track_attach_voice_fingerprint(
    turbo_media_track_t *track, turbo_fingerprint_extractor_t *extractor);
TURBO_MEDIA_API int turbo_media_track_detach_voice_fingerprint(
    turbo_media_track_t *track, turbo_fingerprint_extractor_t *extractor);

/**
 * Set frame callback for received frames
 */
TURBO_MEDIA_API void turbo_media_track_on_frame(turbo_media_track_t *track, turbo_rtc_media_frame_cb cb);

/**
 * Set user data passed to track callbacks.
 */
TURBO_MEDIA_API void turbo_media_track_set_user_data(turbo_media_track_t *track, void *user_data);

/**
 * Get user data associated with a track.
 */
TURBO_MEDIA_API void *turbo_media_track_get_user_data(turbo_media_track_t *track);

/**
 * Set state change callback
 */
TURBO_MEDIA_API void turbo_media_track_on_state(turbo_media_track_t *track, turbo_media_state_cb cb);

/**
 * Set keyframe request callback
 */
TURBO_MEDIA_API void turbo_media_track_on_keyframe_request(turbo_media_track_t *track,
                                                     turbo_media_keyframe_cb cb);

/**
 * Set raw RTP packet callback for receive tracks.
 */
TURBO_MEDIA_API void turbo_media_track_on_rtp_packet(turbo_media_track_t *track,
                                               turbo_media_rtp_packet_cb cb);

/**
 * Start media track
 *
 * @return  0 on success, -1 on error
 */
TURBO_MEDIA_API int turbo_media_track_start(turbo_media_track_t *track);

/**
 * Stop media track
 */
TURBO_MEDIA_API void turbo_media_track_stop(turbo_media_track_t *track);

/**
 * Send raw frame (alternative to capture)
 *
 * For audio: PCM samples (16-bit signed, interleaved)
 * For video: YUV420 or RGB frame
 *
 * @param track     Media track
 * @param data      Frame data
 * @param len       Data length
 * @param timestamp Frame timestamp (0 = auto)
 * @return          0 on success, -1 on error
 */
TURBO_MEDIA_API int turbo_media_track_send_frame(turbo_media_track_t *track, const uint8_t *data,
                                           size_t len, uint64_t timestamp);

/**
 * Send a TTS/provider PCM frame through an active audio track.
 *
 * The frame is borrowed for this call. Its format and sample count must match
 * the track's configured PCM input and frame_size_ms. RTP timestamp generation
 * remains owned by the track.
 */
TURBO_MEDIA_API int turbo_media_track_send_speech_frame(turbo_media_track_t *track,
                                                  const turbo_speech_audio_frame_t *frame);

/**
 * Relay a pre-packetized RTP packet through a local send track.
 *
 * The input packet must be
 * a plaintext RTP packet. The helper rewrites it to
 * the local track's send state, applies SRTP
 * when configured, and sends it on
 * the track transport.
 *
 * @param track     Active send track
 * that owns the egress RTP/SRTP state
 * @param packet    Input plaintext RTP packet
 * @param len
 * Packet length
 * @return          0 on success, -1 on error
 */
TURBO_MEDIA_API int turbo_media_track_send_rtp_packet(turbo_media_track_t *track, const uint8_t *packet,
                                                size_t len);

/**
 * Request keyframe (for video tracks)
 *
 * Sends RTCP PLI to remote
 */
TURBO_MEDIA_API void turbo_media_track_request_keyframe(turbo_media_track_t *track);

/**
 * Get track statistics
 */
TURBO_MEDIA_API void turbo_media_track_get_stats(turbo_media_track_t *track, turbo_media_stats_t *stats);

/**
 * Get track state
 */
TURBO_MEDIA_API turbo_media_state_t turbo_media_track_get_state(turbo_media_track_t *track);

/**
 * Get track type
 */
TURBO_MEDIA_API turbo_rtc_media_track_type_t turbo_media_track_get_type(turbo_media_track_t *track);

/**
 * Get track direction.
 */
TURBO_MEDIA_API turbo_media_direction_t turbo_media_track_get_direction(turbo_media_track_t *track);

/**
 * Get configured codec for track.
 */
TURBO_MEDIA_API turbo_codec_type_t turbo_media_track_get_codec(turbo_media_track_t *track);

/**
 * Get negotiated RTP payload type for this track.
 */
TURBO_MEDIA_API uint8_t turbo_media_track_get_payload_type(turbo_media_track_t *track);

/**
 * Override RTP payload type for this track.
 */
TURBO_MEDIA_API void turbo_media_track_set_payload_type(turbo_media_track_t *track, uint8_t payload_type);

/**
 * Get SSRC for track
 */
TURBO_MEDIA_API uint32_t turbo_media_track_get_ssrc(turbo_media_track_t *track);

/**
 * Get remote SSRC for track
 */
TURBO_MEDIA_API uint32_t turbo_media_track_get_remote_ssrc(turbo_media_track_t *track);

/**
 * Set remote SSRC for track
 */
TURBO_MEDIA_API void turbo_media_track_set_remote_ssrc(turbo_media_track_t *track, uint32_t ssrc);

/**
 * Set RTP header extension ID used for transport-wide congestion control.
 *
 * Pass 0 to
 * disable TWCC RTP header extensions for this track.
 */
TURBO_MEDIA_API void turbo_media_track_set_transport_cc_ext_id(turbo_media_track_t *track, int ext_id);

/**
 * Get RTP header extension ID used for transport-wide congestion control.
 */
TURBO_MEDIA_API int turbo_media_track_get_transport_cc_ext_id(turbo_media_track_t *track);

#if defined(TURBO_MEDIA_PRODUCT_CLIENT)
/* =============================================================================
 * Device
 * Enumeration
 * ============================================================================= */

typedef struct {
  int index;
  char name[256];
  char id[64];
  int is_default;
} turbo_media_device_t;

/**
 * List audio input devices (microphones)
 *
 * @param devices   Output array
 * @param max_count Maximum devices to return
 * @return          Number of devices found
 */
TURBO_MEDIA_API int turbo_media_list_audio_inputs(turbo_media_device_t *devices, int max_count);

/**
 * List video input devices (cameras)
 */
TURBO_MEDIA_API int turbo_media_list_video_inputs(turbo_media_device_t *devices, int max_count);

/**
 * List screens/monitors
 */
TURBO_MEDIA_API int turbo_media_list_screens(turbo_media_device_t *devices, int max_count);
#endif

#ifdef __cplusplus
}
#endif

#endif /* TURBO_MEDIA_ENGINE_H */
