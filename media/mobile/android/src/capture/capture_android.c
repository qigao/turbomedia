/**
 * Android Capture Dispatcher
 *
 * Adapts the staged Android capture backends to the current TurboMedia capture
 * API. Screen capture still needs Java MediaProjection handoff before it is
 * fully functional.
 */

#include "turbo_capture.h"

#include <android/log.h>
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_TAG "TurboMediaCapture"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef struct android_camera_ctx_t android_camera_ctx_t;
typedef struct android_audio_ctx_t android_audio_ctx_t;
typedef struct android_screen_ctx_t android_screen_ctx_t;

extern android_camera_ctx_t *android_camera_create(int width, int height,
                                                   int framerate, int facing);
extern void android_camera_destroy(android_camera_ctx_t *ctx);
extern int android_camera_start(android_camera_ctx_t *ctx);
extern int android_camera_stop(android_camera_ctx_t *ctx);
extern void android_camera_set_callback(android_camera_ctx_t *ctx,
                                        void (*callback)(void *user_data,
                                                         const uint8_t *data,
                                                         size_t len,
                                                         int width,
                                                         int height,
                                                         int64_t timestamp_us),
                                        void *user_data);

extern android_audio_ctx_t *android_audio_create(int sample_rate, int channels,
                                                 int use_opensles);
extern void android_audio_destroy(android_audio_ctx_t *ctx);
extern int android_audio_start(android_audio_ctx_t *ctx);
extern int android_audio_stop(android_audio_ctx_t *ctx);
extern void android_audio_set_callback(android_audio_ctx_t *ctx,
                                       void (*callback)(void *user_data,
                                                        const int16_t *data,
                                                        size_t frames),
                                       void *user_data);

extern android_screen_ctx_t *android_screen_create(int width, int height,
                                                   int framerate);
extern void android_screen_destroy(android_screen_ctx_t *ctx);
extern int android_screen_start(android_screen_ctx_t *ctx, jobject media_projection);
extern int android_screen_stop(android_screen_ctx_t *ctx);
extern void android_screen_set_callback(android_screen_ctx_t *ctx,
                                        void (*callback)(void *user_data,
                                                         const uint8_t *data,
                                                         size_t len,
                                                         int width,
                                                         int height,
                                                         int64_t timestamp_us),
                                        void *user_data);

typedef struct {
    android_audio_ctx_t *native;
    int channels;
} android_audio_platform_t;

typedef struct {
    android_camera_ctx_t *native;
} android_video_platform_t;

typedef struct {
    android_screen_ctx_t *native;
} android_screen_platform_t;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void android_audio_callback(void *user_data, const int16_t *data, size_t frames) {
    turbo_capture_t *capture = (turbo_capture_t *)user_data;
    if (!capture || !capture->audio_cb || !capture->platform_ctx) return;

    android_audio_platform_t *platform = (android_audio_platform_t *)capture->platform_ctx;
    size_t len = frames * (size_t)platform->channels * sizeof(int16_t);
    capture->audio_cb(capture, (const uint8_t *)data, len, now_us(), capture->user_data);
}

static void android_video_callback(void *user_data,
                                   const uint8_t *data,
                                   size_t len,
                                   int width,
                                   int height,
                                   int64_t timestamp_us) {
    turbo_capture_t *capture = (turbo_capture_t *)user_data;
    if (!capture || !capture->video_cb) return;
    capture->video_cb(capture, data, len, width, height,
                      (uint64_t)timestamp_us, capture->user_data);
}

int turbo_capture_list_audio_devices(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    memset(&devices[0], 0, sizeof(devices[0]));
    devices[0].index = 0;
    devices[0].type = TURBO_CAPTURE_TYPE_AUDIO;
    devices[0].is_default = 1;
    strncpy(devices[0].id, "default", sizeof(devices[0].id) - 1);
    strncpy(devices[0].name, "Default Microphone", sizeof(devices[0].name) - 1);
    return 1;
}

int turbo_capture_list_video_devices(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    memset(&devices[0], 0, sizeof(devices[0]));
    devices[0].index = 0;
    devices[0].type = TURBO_CAPTURE_TYPE_VIDEO;
    devices[0].is_default = 1;
    strncpy(devices[0].id, "back", sizeof(devices[0].id) - 1);
    strncpy(devices[0].name, "Back Camera", sizeof(devices[0].name) - 1);

    if (max_count == 1) return 1;

    memset(&devices[1], 0, sizeof(devices[1]));
    devices[1].index = 1;
    devices[1].type = TURBO_CAPTURE_TYPE_VIDEO;
    devices[1].is_default = 0;
    strncpy(devices[1].id, "front", sizeof(devices[1].id) - 1);
    strncpy(devices[1].name, "Front Camera", sizeof(devices[1].name) - 1);
    return 2;
}

int turbo_capture_list_video_modes(const char *device_id,
                                   turbo_video_capture_mode_t *modes,
                                   int max_count) {
    (void)device_id;
    (void)modes;
    (void)max_count;
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

int turbo_capture_list_screens(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    memset(&devices[0], 0, sizeof(devices[0]));
    devices[0].index = 0;
    devices[0].type = TURBO_CAPTURE_TYPE_SCREEN;
    devices[0].is_default = 1;
    strncpy(devices[0].id, "screen:0", sizeof(devices[0].id) - 1);
    strncpy(devices[0].name, "Device Screen", sizeof(devices[0].name) - 1);
    return 1;
}

int turbo_capture_list_gpu_devices(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return TURBO_CAPTURE_ERR_NOMEM;
    return 0;
}

turbo_capture_t *turbo_audio_capture_create(const char *device_id,
                                            const turbo_audio_capture_config_t *config) {
    (void)device_id;

    turbo_capture_t *capture = (turbo_capture_t *)calloc(1, sizeof(*capture));
    android_audio_platform_t *platform =
        (android_audio_platform_t *)calloc(1, sizeof(*platform));
    if (!capture || !platform) {
        free(capture);
        free(platform);
        return NULL;
    }

    int sample_rate = (config && config->sample_rate > 0) ? config->sample_rate : 48000;
    int channels = (config && config->channels > 0) ? config->channels : 1;
    platform->native = android_audio_create(sample_rate, channels, 1);
    platform->channels = channels;
    if (!platform->native) {
        free(platform);
        free(capture);
        return NULL;
    }

    capture->type = TURBO_CAPTURE_TYPE_AUDIO;
    capture->state = TURBO_CAPTURE_STATE_STOPPED;
    capture->platform_ctx = platform;
    android_audio_set_callback(platform->native, android_audio_callback, capture);
    return capture;
}

turbo_capture_t *turbo_video_capture_create(const char *device_id,
                                            const turbo_video_capture_config_t *config) {
    turbo_capture_t *capture = (turbo_capture_t *)calloc(1, sizeof(*capture));
    android_video_platform_t *platform =
        (android_video_platform_t *)calloc(1, sizeof(*platform));
    if (!capture || !platform) {
        free(capture);
        free(platform);
        return NULL;
    }

    int width = (config && config->width > 0) ? config->width : 640;
    int height = (config && config->height > 0) ? config->height : 480;
    int framerate = (config && config->framerate > 0) ? config->framerate : 30;
    int facing = (device_id && strcmp(device_id, "front") == 0) ? 1 : 0;

    platform->native = android_camera_create(width, height, framerate, facing);
    if (!platform->native) {
        free(platform);
        free(capture);
        return NULL;
    }

    capture->type = TURBO_CAPTURE_TYPE_VIDEO;
    capture->state = TURBO_CAPTURE_STATE_STOPPED;
    capture->platform_ctx = platform;
    android_camera_set_callback(platform->native, android_video_callback, capture);
    return capture;
}

turbo_capture_t *turbo_screen_capture_create(const turbo_screen_capture_config_t *config) {
    turbo_capture_t *capture = (turbo_capture_t *)calloc(1, sizeof(*capture));
    android_screen_platform_t *platform =
        (android_screen_platform_t *)calloc(1, sizeof(*platform));
    if (!capture || !platform) {
        free(capture);
        free(platform);
        return NULL;
    }

    int framerate = (config && config->framerate > 0) ? config->framerate : 30;
    platform->native = android_screen_create(1280, 720, framerate);
    if (!platform->native) {
        free(platform);
        free(capture);
        return NULL;
    }

    capture->type = TURBO_CAPTURE_TYPE_SCREEN;
    capture->state = TURBO_CAPTURE_STATE_STOPPED;
    capture->platform_ctx = platform;
    android_screen_set_callback(platform->native, android_video_callback, capture);
    return capture;
}

void turbo_audio_capture_set_callback(turbo_capture_t *capture,
                                      turbo_audio_capture_cb cb,
                                      void *user_data) {
    if (!capture || capture->type != TURBO_CAPTURE_TYPE_AUDIO) return;
    capture->audio_cb = cb;
    capture->user_data = user_data;
}

void turbo_video_capture_set_callback(turbo_capture_t *capture,
                                      turbo_video_capture_cb cb,
                                      void *user_data) {
    if (!capture || capture->type != TURBO_CAPTURE_TYPE_VIDEO) return;
    capture->video_cb = cb;
    capture->user_data = user_data;
}

void turbo_screen_capture_set_callback(turbo_capture_t *capture,
                                       turbo_video_capture_cb cb,
                                       void *user_data) {
    if (!capture || capture->type != TURBO_CAPTURE_TYPE_SCREEN) return;
    capture->video_cb = cb;
    capture->user_data = user_data;
}

int turbo_video_capture_get_control_range(turbo_capture_t *capture,
                                          turbo_camera_control_t control,
                                          turbo_camera_control_range_t *range) {
    (void)capture;
    (void)control;
    if (!range) return TURBO_CAPTURE_ERR_DEVICE;
    memset(range, 0, sizeof(*range));
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

int turbo_video_capture_set_control(turbo_capture_t *capture,
                                    turbo_camera_control_t control,
                                    int value) {
    (void)capture;
    (void)control;
    (void)value;
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

int turbo_video_capture_get_control(turbo_capture_t *capture,
                                    turbo_camera_control_t control,
                                    int *value) {
    (void)capture;
    (void)control;
    if (value) *value = 0;
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

int turbo_video_capture_set_crop(turbo_capture_t *capture,
                                 const turbo_video_crop_t *crop) {
    (void)capture;
    (void)crop;
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

int turbo_video_capture_get_crop(turbo_capture_t *capture,
                                 turbo_video_crop_t *crop) {
    (void)capture;
    if (!crop) return TURBO_CAPTURE_ERR_DEVICE;
    memset(crop, 0, sizeof(*crop));
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

int turbo_capture_start(turbo_capture_t *capture) {
    if (!capture) return -1;
    if (capture->state == TURBO_CAPTURE_STATE_RUNNING) return 0;

    capture->state = TURBO_CAPTURE_STATE_STARTING;

    int result = -1;
    switch (capture->type) {
        case TURBO_CAPTURE_TYPE_AUDIO: {
            android_audio_platform_t *platform =
                (android_audio_platform_t *)capture->platform_ctx;
            result = platform ? android_audio_start(platform->native) : -1;
            break;
        }
        case TURBO_CAPTURE_TYPE_VIDEO: {
            android_video_platform_t *platform =
                (android_video_platform_t *)capture->platform_ctx;
            result = platform ? android_camera_start(platform->native) : -1;
            break;
        }
        case TURBO_CAPTURE_TYPE_SCREEN: {
            android_screen_platform_t *platform =
                (android_screen_platform_t *)capture->platform_ctx;
            result = platform ? android_screen_start(platform->native, NULL) : -1;
            break;
        }
        default:
            result = -1;
            break;
    }

    capture->state = (result == 0) ? TURBO_CAPTURE_STATE_RUNNING : TURBO_CAPTURE_STATE_ERROR;
    if (capture->state_cb) {
        capture->state_cb(capture, capture->state, capture->user_data);
    }
    return result;
}

void turbo_capture_stop(turbo_capture_t *capture) {
    if (!capture || capture->state == TURBO_CAPTURE_STATE_STOPPED) return;

    capture->state = TURBO_CAPTURE_STATE_STOPPING;

    switch (capture->type) {
        case TURBO_CAPTURE_TYPE_AUDIO: {
            android_audio_platform_t *platform =
                (android_audio_platform_t *)capture->platform_ctx;
            if (platform) android_audio_stop(platform->native);
            break;
        }
        case TURBO_CAPTURE_TYPE_VIDEO: {
            android_video_platform_t *platform =
                (android_video_platform_t *)capture->platform_ctx;
            if (platform) android_camera_stop(platform->native);
            break;
        }
        case TURBO_CAPTURE_TYPE_SCREEN: {
            android_screen_platform_t *platform =
                (android_screen_platform_t *)capture->platform_ctx;
            if (platform) android_screen_stop(platform->native);
            break;
        }
        default:
            break;
    }

    capture->state = TURBO_CAPTURE_STATE_STOPPED;
    if (capture->state_cb) {
        capture->state_cb(capture, capture->state, capture->user_data);
    }
}

void turbo_capture_destroy(turbo_capture_t *capture) {
    if (!capture) return;

    switch (capture->type) {
        case TURBO_CAPTURE_TYPE_AUDIO: {
            android_audio_platform_t *platform =
                (android_audio_platform_t *)capture->platform_ctx;
            if (platform) {
                android_audio_destroy(platform->native);
                free(platform);
            }
            break;
        }
        case TURBO_CAPTURE_TYPE_VIDEO: {
            android_video_platform_t *platform =
                (android_video_platform_t *)capture->platform_ctx;
            if (platform) {
                android_camera_destroy(platform->native);
                free(platform);
            }
            break;
        }
        case TURBO_CAPTURE_TYPE_SCREEN: {
            android_screen_platform_t *platform =
                (android_screen_platform_t *)capture->platform_ctx;
            if (platform) {
                android_screen_destroy(platform->native);
                free(platform);
            }
            break;
        }
        default:
            break;
    }

    free(capture);
}

turbo_capture_state_t turbo_capture_get_state(turbo_capture_t *capture) {
    return capture ? capture->state : TURBO_CAPTURE_STATE_STOPPED;
}

void turbo_capture_on_state(turbo_capture_t *capture, turbo_capture_state_cb cb) {
    if (capture) capture->state_cb = cb;
}
