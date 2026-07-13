/**
 * TurboNet Capture Abstraction
 *
 * Unified interface for audio/video/screen capture
 */
#ifndef TURBO_CAPTURE_H
#define TURBO_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Constants
 * ============================================================================= */

#define TURBO_CAPTURE_MAX_DEVICES   16
#define TURBO_CAPTURE_MAX_VIDEO_MODES 64

/* =============================================================================
 * Types
 * ============================================================================= */

typedef enum {
    TURBO_CAPTURE_TYPE_AUDIO = 1,
    TURBO_CAPTURE_TYPE_VIDEO = 2,
    TURBO_CAPTURE_TYPE_SCREEN = 3,
    TURBO_CAPTURE_TYPE_GPU = 4
} turbo_capture_type_t;

typedef enum {
    TURBO_CAPTURE_OK = 0,
    TURBO_CAPTURE_ERR_NOMEM = -1,
    TURBO_CAPTURE_ERR_DEVICE = -2,
    TURBO_CAPTURE_ERR_FORMAT = -3,
    TURBO_CAPTURE_ERR_BUSY = -4,
    TURBO_CAPTURE_ERR_UNSUPPORTED = -5
} turbo_capture_result_t;

typedef enum {
    TURBO_CAPTURE_STATE_STOPPED = 0,
    TURBO_CAPTURE_STATE_STARTING,
    TURBO_CAPTURE_STATE_RUNNING,
    TURBO_CAPTURE_STATE_STOPPING,
    TURBO_CAPTURE_STATE_ERROR
} turbo_capture_state_t;

/* =============================================================================
 * Device Info
 * ============================================================================= */

typedef struct {
    int index;
    char name[256];
    char id[128];
    int is_default;
    turbo_capture_type_t type;
} turbo_capture_device_t;

/* =============================================================================
 * Capture Configuration
 * ============================================================================= */

typedef struct {
    int sample_rate;        /* 8000, 16000, 24000, 48000 */
    int channels;           /* 1 or 2 */
    int bits_per_sample;    /* 16 or 32 */
    int frame_size_ms;      /* Buffer size in ms (10, 20, etc.) */
} turbo_audio_capture_config_t;

typedef struct {
    int width;
    int height;
    int framerate;
    int format;             /* 0=I420, 1=NV12, 2=RGB24, 3=BGRA */
} turbo_video_capture_config_t;

typedef enum {
    TURBO_CAMERA_CONTROL_ZOOM = 1,      /* Digital/hardware zoom, value is percent: 100 == 1.0x */
    TURBO_CAMERA_CONTROL_FOCUS = 2,
    TURBO_CAMERA_CONTROL_EXPOSURE = 3,
    TURBO_CAMERA_CONTROL_PAN = 4,
    TURBO_CAMERA_CONTROL_TILT = 5,
    TURBO_CAMERA_CONTROL_BRIGHTNESS = 6,
    TURBO_CAMERA_CONTROL_CONTRAST = 7,
    TURBO_CAMERA_CONTROL_HUE = 8,
    TURBO_CAMERA_CONTROL_WHITE_BALANCE = 9
} turbo_camera_control_t;

typedef struct {
    int min_value;
    int max_value;
    int step;
    int default_value;       /* Driver/backend default manual value */
    int current_value;
} turbo_camera_control_range_t;

typedef struct {
    int x;
    int y;
    int width;
    int height;
} turbo_video_crop_t;

typedef struct {
    int width;
    int height;
    int framerate;
    int format;             /* 0=I420, 1=NV12, 2=RGB24, 3=BGRA, -1=unknown/native */
} turbo_video_capture_mode_t;

typedef struct {
    int monitor_index;      /* -1 for all monitors */
    int framerate;
    int capture_cursor;
    int capture_audio;      /* Capture system audio too */
} turbo_screen_capture_config_t;

/* =============================================================================
 * Callbacks
 * ============================================================================= */

/* Forward declaration */
typedef struct turbo_capture_s turbo_capture_t;

/**
 * Called when audio samples are captured
 *
 * @param capture   Capture instance
 * @param samples   PCM samples (16-bit or 32-bit based on config)
 * @param len       Length in bytes
 * @param timestamp Capture timestamp in microseconds
 * @param user_data User context
 */
typedef void (*turbo_audio_capture_cb)(turbo_capture_t *capture,
                                        const uint8_t *samples, size_t len,
                                        uint64_t timestamp, void *user_data);

/**
 * Called when video frame is captured
 *
 * @param capture   Capture instance
 * @param frame     Frame data (format based on config)
 * @param len       Frame length in bytes
 * @param width     Frame width
 * @param height    Frame height
 * @param timestamp Capture timestamp in microseconds
 * @param user_data User context
 */
typedef void (*turbo_video_capture_cb)(turbo_capture_t *capture,
                                        const uint8_t *frame, size_t len,
                                        int width, int height,
                                        uint64_t timestamp, void *user_data);

/**
 * Called when capture state changes
 */
typedef void (*turbo_capture_state_cb)(turbo_capture_t *capture,
                                        turbo_capture_state_t state,
                                        void *user_data);

/* =============================================================================
 * Capture Instance
 * ============================================================================= */

struct turbo_capture_s {
    turbo_capture_type_t type;
    turbo_capture_state_t state;
    void *platform_ctx;     /* Platform-specific context */
    void *user_data;

    /* Callbacks */
    union {
        turbo_audio_capture_cb audio_cb;
        turbo_video_capture_cb video_cb;
    };
    turbo_capture_state_cb state_cb;
};

/* =============================================================================
 * Device Enumeration
 * ============================================================================= */

/**
 * List audio input devices (microphones)
 *
 * @param devices   Output array
 * @param max_count Maximum devices to return
 * @return          Number of devices found, or negative on error
 */
CXX_C_API int turbo_capture_list_audio_devices(turbo_capture_device_t *devices, int max_count);

/**
 * List video input devices (cameras)
 */
CXX_C_API int turbo_capture_list_video_devices(turbo_capture_device_t *devices, int max_count);

/**
 * List supported video capture modes for a camera.
 *
 * @param device_id Device index string, symbolic id, or NULL/empty for default camera.
 * @param modes Output array.
 * @param max_count Maximum modes to return.
 * @return Number of modes found, or a negative turbo_capture_result_t on error.
 */
CXX_C_API int turbo_capture_list_video_modes(const char *device_id,
                                             turbo_video_capture_mode_t *modes,
                                             int max_count);

/**
 * List screens/monitors
 */
CXX_C_API int turbo_capture_list_screens(turbo_capture_device_t *devices, int max_count);

/**
 * List GPU adapters.
 *
 * Device indexes are platform adapter indexes and are not compacted when
 * unusable software adapters are skipped.
 */
CXX_C_API int turbo_capture_list_gpu_devices(turbo_capture_device_t *devices, int max_count);

/* =============================================================================
 * Audio Capture
 * ============================================================================= */

/**
 * Create audio capture instance
 *
 * @param device_id     Device ID (NULL for default)
 * @param config        Capture configuration
 * @return              Capture instance, or NULL on error
 */
CXX_C_API turbo_capture_t *turbo_audio_capture_create(const char *device_id,
                                                        const turbo_audio_capture_config_t *config);

/**
 * Set audio capture callback
 */
CXX_C_API void turbo_audio_capture_set_callback(turbo_capture_t *capture,
                                                  turbo_audio_capture_cb cb,
                                                  void *user_data);

/* =============================================================================
 * Video Capture
 * ============================================================================= */

/**
 * Create video capture instance
 */
CXX_C_API turbo_capture_t *turbo_video_capture_create(const char *device_id,
                                                        const turbo_video_capture_config_t *config);

/**
 * Set video capture callback
 */
CXX_C_API void turbo_video_capture_set_callback(turbo_capture_t *capture,
                                                  turbo_video_capture_cb cb,
                                                  void *user_data);

/**
 * Query a camera control range.
 *
 * Not every backend/device supports every control. Unsupported controls return
 * TURBO_CAPTURE_ERR_UNSUPPORTED. Backends should prefer hardware controls and
 * may fall back to software controls when a hardware control is unavailable.
 */
CXX_C_API int turbo_video_capture_get_control_range(turbo_capture_t *capture,
                                                     turbo_camera_control_t control,
                                                     turbo_camera_control_range_t *range);

/**
 * Set a camera control value.
 *
 * For TURBO_CAMERA_CONTROL_ZOOM, value is percent: 100 means 1.0x. Backends
 * should apply hardware zoom first and fall back to software zoom if needed.
 */
CXX_C_API int turbo_video_capture_set_control(turbo_capture_t *capture,
                                               turbo_camera_control_t control,
                                               int value);

CXX_C_API int turbo_video_capture_get_control(turbo_capture_t *capture,
                                               turbo_camera_control_t control,
                                               int *value);

/**
 * Set a software crop region in capture output coordinates.
 *
 * Passing NULL or a rectangle with non-positive width/height disables crop.
 */
CXX_C_API int turbo_video_capture_set_crop(turbo_capture_t *capture,
                                            const turbo_video_crop_t *crop);

CXX_C_API int turbo_video_capture_get_crop(turbo_capture_t *capture,
                                            turbo_video_crop_t *crop);

/* =============================================================================
 * Screen Capture
 * ============================================================================= */

/**
 * Create screen capture instance
 */
CXX_C_API turbo_capture_t *turbo_screen_capture_create(const turbo_screen_capture_config_t *config);

/**
 * Set screen capture callback (uses video callback signature)
 */
CXX_C_API void turbo_screen_capture_set_callback(turbo_capture_t *capture,
                                                   turbo_video_capture_cb cb,
                                                   void *user_data);

/* =============================================================================
 * Common Functions
 * ============================================================================= */

/**
 * Start capture
 *
 * @return  0 on success
 */
CXX_C_API int turbo_capture_start(turbo_capture_t *capture);

/**
 * Stop capture
 */
CXX_C_API void turbo_capture_stop(turbo_capture_t *capture);

/**
 * Destroy capture instance
 */
CXX_C_API void turbo_capture_destroy(turbo_capture_t *capture);

/**
 * Get capture state
 */
CXX_C_API turbo_capture_state_t turbo_capture_get_state(turbo_capture_t *capture);

/**
 * Set state change callback
 */
CXX_C_API void turbo_capture_on_state(turbo_capture_t *capture,
                                        turbo_capture_state_cb cb);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_CAPTURE_H */
