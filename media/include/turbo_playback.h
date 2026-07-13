/**
 * TurboNet Audio Playback
 *
 * Unified interface for audio playback using miniaudio backend
 * Supports streaming PCM data and file playback
 */
#ifndef TURBO_PLAYBACK_H
#define TURBO_PLAYBACK_H

#include <stdint.h>
#include <stddef.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define TURBO_PLAYBACK_MAX_DEVICES 16

/* =============================================================================
 * Types
 * ============================================================================= */

typedef enum {
    TURBO_PLAYBACK_OK = 0,
    TURBO_PLAYBACK_ERR_NOMEM = -1,
    TURBO_PLAYBACK_ERR_DEVICE = -2,
    TURBO_PLAYBACK_ERR_FORMAT = -3,
    TURBO_PLAYBACK_ERR_BUSY = -4,
    TURBO_PLAYBACK_ERR_FILE = -5,
    TURBO_PLAYBACK_ERR_UNSUPPORTED = -6
} turbo_playback_result_t;

typedef enum {
    TURBO_PLAYBACK_STATE_STOPPED = 0,
    TURBO_PLAYBACK_STATE_STARTING,
    TURBO_PLAYBACK_STATE_PLAYING,
    TURBO_PLAYBACK_STATE_PAUSED,
    TURBO_PLAYBACK_STATE_STOPPING,
    TURBO_PLAYBACK_STATE_ERROR
} turbo_playback_state_t;

typedef enum {
    TURBO_PLAYBACK_FORMAT_S16 = 0,   /* 16-bit signed integer */
    TURBO_PLAYBACK_FORMAT_S32,       /* 32-bit signed integer */
    TURBO_PLAYBACK_FORMAT_F32        /* 32-bit float */
} turbo_playback_format_t;

/* =============================================================================
 * Device Info
 * ============================================================================= */

typedef struct {
    int index;
    char name[256];
    char id[128];
    int is_default;
} turbo_playback_device_t;

/* =============================================================================
 * Configuration
 * ============================================================================= */

typedef struct {
    int sample_rate;                 /* 8000, 16000, 24000, 48000 */
    int channels;                    /* 1 (mono) or 2 (stereo) */
    turbo_playback_format_t format;  /* Sample format */
    int buffer_size_ms;              /* Buffer size in ms (default: 50) */
} turbo_playback_config_t;

/* =============================================================================
 * Callbacks
 * ============================================================================= */

/* Forward declaration */
typedef struct turbo_playback_s turbo_playback_t;

/**
 * Called when device needs more audio data
 *
 * Fill the output buffer with PCM samples. Return the number of frames written.
 * Return 0 when no more data is available (end of stream).
 *
 * @param playback    Playback instance
 * @param output      Output buffer to fill
 * @param frame_count Number of frames requested
 * @param user_data   User context
 * @return            Number of frames written (0 = end of stream)
 */
typedef size_t (*turbo_playback_data_cb)(turbo_playback_t *playback,
                                          void *output, size_t frame_count,
                                          void *user_data);

/**
 * Called when playback state changes
 */
typedef void (*turbo_playback_state_cb)(turbo_playback_t *playback,
                                         turbo_playback_state_t state,
                                         void *user_data);

/**
 * Called when file/stream playback completes
 */
typedef void (*turbo_playback_complete_cb)(turbo_playback_t *playback,
                                            void *user_data);

/* =============================================================================
 * Device Enumeration
 * ============================================================================= */

/**
 * List audio output devices (speakers/headphones)
 *
 * @param devices   Output array
 * @param max_count Maximum devices to return
 * @return          Number of devices found, or negative on error
 */
CXX_C_API int turbo_playback_list_devices(turbo_playback_device_t *devices, int max_count);

/**
 * Get default output device
 *
 * @param device    Output device info
 * @return          0 on success, negative on error
 */
CXX_C_API int turbo_playback_get_default_device(turbo_playback_device_t *device);

/* =============================================================================
 * Playback Instance
 * ============================================================================= */

/**
 * Create playback instance for streaming PCM data
 *
 * Use this when you want to provide audio data via callback (e.g., WebRTC)
 *
 * @param device_id     Device ID (NULL for default)
 * @param config        Playback configuration
 * @return              Playback instance, or NULL on error
 */
CXX_C_API turbo_playback_t *turbo_playback_create(const char *device_id,
                                                   const turbo_playback_config_t *config);

/**
 * Create playback instance for file playback
 *
 * Supports WAV, MP3, FLAC, OGG via miniaudio decoder
 *
 * @param device_id     Device ID (NULL for default)
 * @param filepath      Path to audio file
 * @return              Playback instance, or NULL on error
 */
CXX_C_API turbo_playback_t *turbo_playback_create_file(const char *device_id,
                                                        const char *filepath);

/**
 * Destroy playback instance
 */
CXX_C_API void turbo_playback_destroy(turbo_playback_t *playback);

/* =============================================================================
 * Callbacks
 * ============================================================================= */

/**
 * Set data callback for streaming playback
 *
 * The callback is called from the audio thread when more data is needed.
 * You must fill the output buffer with the requested number of frames.
 */
CXX_C_API void turbo_playback_set_data_callback(turbo_playback_t *playback,
                                                 turbo_playback_data_cb cb,
                                                 void *user_data);

/**
 * Set state change callback
 */
CXX_C_API void turbo_playback_on_state(turbo_playback_t *playback,
                                        turbo_playback_state_cb cb);

/**
 * Set completion callback (for file playback)
 */
CXX_C_API void turbo_playback_on_complete(turbo_playback_t *playback,
                                           turbo_playback_complete_cb cb);

/* =============================================================================
 * Playback Control
 * ============================================================================= */

/**
 * Start playback
 *
 * @return  0 on success
 */
CXX_C_API int turbo_playback_start(turbo_playback_t *playback);

/**
 * Stop playback
 */
CXX_C_API void turbo_playback_stop(turbo_playback_t *playback);

/**
 * Pause playback
 */
CXX_C_API void turbo_playback_pause(turbo_playback_t *playback);

/**
 * Resume playback
 */
CXX_C_API void turbo_playback_resume(turbo_playback_t *playback);

/**
 * Get playback state
 */
CXX_C_API turbo_playback_state_t turbo_playback_get_state(turbo_playback_t *playback);

/* =============================================================================
 * Volume and Position Control
 * ============================================================================= */

/**
 * Set playback volume
 *
 * @param volume    Volume level (0.0 = mute, 1.0 = normal, >1.0 = amplify)
 */
CXX_C_API void turbo_playback_set_volume(turbo_playback_t *playback, float volume);

/**
 * Get current volume
 */
CXX_C_API float turbo_playback_get_volume(turbo_playback_t *playback);

/**
 * Seek to position (file playback only)
 *
 * @param position_ms   Position in milliseconds
 * @return              0 on success
 */
CXX_C_API int turbo_playback_seek(turbo_playback_t *playback, uint64_t position_ms);

/**
 * Get current position (file playback only)
 *
 * @return  Current position in milliseconds
 */
CXX_C_API uint64_t turbo_playback_get_position(turbo_playback_t *playback);

/**
 * Get total duration (file playback only)
 *
 * @return  Duration in milliseconds, or 0 if unknown
 */
CXX_C_API uint64_t turbo_playback_get_duration(turbo_playback_t *playback);

/**
 * Set looping (file playback only)
 */
CXX_C_API void turbo_playback_set_looping(turbo_playback_t *playback, int loop);

/* =============================================================================
 * Playlist/Queue API - Play multiple files in sequence
 * ============================================================================= */

/**
 * Add file to playback queue
 *
 * Files are played in order. When current file completes, next file starts.
 * Use turbo_playback_on_complete to get notified when entire queue finishes.
 *
 * @param playback  Playback instance (must be created with turbo_playback_create_file)
 * @param filepath  Path to audio file to add
 * @return          0 on success, negative on error
 */
CXX_C_API int turbo_playback_queue_add(turbo_playback_t *playback, const char *filepath);

/**
 * Clear the playback queue
 *
 * Removes all queued files. Does not stop current playback.
 */
CXX_C_API void turbo_playback_queue_clear(turbo_playback_t *playback);

/**
 * Get number of files in queue (including current)
 */
CXX_C_API int turbo_playback_queue_count(turbo_playback_t *playback);

/**
 * Skip to next file in queue
 *
 * @return  0 on success, -1 if queue is empty
 */
CXX_C_API int turbo_playback_queue_next(turbo_playback_t *playback);

/**
 * Set queue looping
 *
 * When enabled, queue restarts from beginning after last file completes.
 */
CXX_C_API void turbo_playback_queue_set_looping(turbo_playback_t *playback, int loop);

/* =============================================================================
 * Streaming Buffer (Push-based API alternative)
 * ============================================================================= */

/**
 * Write PCM samples to playback buffer
 *
 * Alternative to callback-based streaming. Push audio data to an internal
 * ring buffer that the device will consume.
 *
 * @param playback  Playback instance
 * @param samples   PCM samples in configured format
 * @param len       Length in bytes
 * @return          Number of bytes written (may be less if buffer full)
 */
CXX_C_API size_t turbo_playback_write(turbo_playback_t *playback,
                                       const void *samples, size_t len);

/**
 * Get available space in playback buffer
 *
 * @return  Available space in bytes
 */
CXX_C_API size_t turbo_playback_get_available(turbo_playback_t *playback);

/**
 * Get current buffer level (for monitoring)
 *
 * @return  Buffered data in bytes
 */
CXX_C_API size_t turbo_playback_get_buffered(turbo_playback_t *playback);

/**
 * Clear buffered push-based streaming data
 *
 * Does not stop the audio device. Only affects instances created with
 * turbo_playback_create.
 */
CXX_C_API void turbo_playback_clear(turbo_playback_t *playback);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_PLAYBACK_H */
