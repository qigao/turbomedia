/**
 * Android Capture Dispatcher
 *
 * Adapts the staged Android capture backends to the current TurboMedia capture
 * API. Screen capture still needs Java MediaProjection handoff before it is
 * fully functional.
 */

#include "turbo_capture.h"
#include "capture_video_backend.h"

#include <android/log.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <jni.h>
#include <media/NdkImage.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOG_TAG "TurboMediaCapture"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ANDROID_STREAM_CONFIGURATION_OUTPUT 0

typedef struct android_camera_ctx_t android_camera_ctx_t;
typedef struct android_audio_ctx_t android_audio_ctx_t;
typedef struct android_screen_ctx_t android_screen_ctx_t;

extern android_camera_ctx_t *android_camera_create(int width, int height,
                                                   int min_framerate,
                                                   int max_framerate,
                                                   const char *camera_id);
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
    char camera_id[128];
    ACameraManager *manager;
    ACameraMetadata *metadata;
} android_video_device_ctx_t;

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

static int android_video_mode_from_metadata(
    const ACameraMetadata *metadata,
    uint64_t mode_id,
    turbo_video_native_mode_t *mode,
    int *out_min_framerate,
    int *out_max_framerate) {
    ACameraMetadata_const_entry configurations;
    ACameraMetadata_const_entry fps_ranges;
    ACameraMetadata_const_entry min_frame_durations;
    uint32_t stream_index = (uint32_t)(mode_id >> 32);
    uint32_t fps_index = (uint32_t)mode_id;
    uint32_t configuration_offset;
    uint32_t fps_offset;
    int32_t min_framerate;
    int32_t max_framerate;
    int64_t min_frame_duration = 0;

    if (!metadata || !mode ||
        ACameraMetadata_getConstEntry(
            metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
            &configurations) != ACAMERA_OK ||
        ACameraMetadata_getConstEntry(
            metadata, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
            &fps_ranges) != ACAMERA_OK ||
        ACameraMetadata_getConstEntry(
            metadata, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
            &min_frame_durations) != ACAMERA_OK) {
        return TURBO_CAPTURE_ERR_DEVICE;
    }

    configuration_offset = stream_index * 4u;
    fps_offset = fps_index * 2u;
    if (configuration_offset + 3u >= configurations.count ||
        fps_offset + 1u >= fps_ranges.count ||
        configurations.data.i32[configuration_offset] !=
            AIMAGE_FORMAT_YUV_420_888 ||
        configurations.data.i32[configuration_offset + 3u] !=
            ANDROID_STREAM_CONFIGURATION_OUTPUT) {
        return TURBO_CAPTURE_ERR_FORMAT;
    }

    min_framerate = fps_ranges.data.i32[fps_offset];
    max_framerate = fps_ranges.data.i32[fps_offset + 1u];
    if (configurations.data.i32[configuration_offset + 1u] <= 0 ||
        configurations.data.i32[configuration_offset + 2u] <= 0 ||
        min_framerate <= 0 || max_framerate != min_framerate) {
        return TURBO_CAPTURE_ERR_FORMAT;
    }

    for (uint32_t offset = 0; offset + 3u < min_frame_durations.count;
         offset += 4u) {
        if (min_frame_durations.data.i64[offset] ==
                configurations.data.i32[configuration_offset] &&
            min_frame_durations.data.i64[offset + 1u] ==
                configurations.data.i32[configuration_offset + 1u] &&
            min_frame_durations.data.i64[offset + 2u] ==
                configurations.data.i32[configuration_offset + 2u]) {
            min_frame_duration = min_frame_durations.data.i64[offset + 3u];
            break;
        }
    }
    if (min_frame_duration <= 0 ||
        max_framerate > INT64_C(1000000000) / min_frame_duration) {
        return TURBO_CAPTURE_ERR_UNSUPPORTED;
    }

    mode->width = configurations.data.i32[configuration_offset + 1u];
    mode->height = configurations.data.i32[configuration_offset + 2u];
    mode->framerate_numerator = (uint32_t)max_framerate;
    mode->framerate_denominator = 1;
    mode->format = TURBO_VIDEO_CAPTURE_FORMAT_I420;
    mode->mode_id = mode_id;
    if (out_min_framerate) *out_min_framerate = min_framerate;
    if (out_max_framerate) *out_max_framerate = max_framerate;
    return TURBO_CAPTURE_OK;
}

static int android_video_device_open(const char *device_id,
                                     void **backend_ctx) {
    android_video_device_ctx_t *ctx;
    ACameraManager *manager;
    ACameraIdList *camera_ids = NULL;
    int use_facing;
    int requested_facing;

    if (!backend_ctx) return TURBO_CAPTURE_ERR_FORMAT;
    *backend_ctx = NULL;
    use_facing = !device_id || !device_id[0] ||
                 strcmp(device_id, "back") == 0 ||
                 strcmp(device_id, "front") == 0;
    requested_facing = device_id && strcmp(device_id, "front") == 0
                           ? ACAMERA_LENS_FACING_FRONT
                           : ACAMERA_LENS_FACING_BACK;

    manager = ACameraManager_create();
    if (!manager ||
        ACameraManager_getCameraIdList(manager, &camera_ids) != ACAMERA_OK ||
        !camera_ids) {
        if (camera_ids) ACameraManager_deleteCameraIdList(camera_ids);
        if (manager) ACameraManager_delete(manager);
        return TURBO_CAPTURE_ERR_DEVICE;
    }

    ctx = (android_video_device_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        ACameraManager_deleteCameraIdList(camera_ids);
        ACameraManager_delete(manager);
        return TURBO_CAPTURE_ERR_NOMEM;
    }
    ctx->manager = manager;

    for (int i = 0; i < camera_ids->numCameras; ++i) {
        const char *camera_id = camera_ids->cameraIds[i];
        ACameraMetadata *metadata = NULL;
        ACameraMetadata_const_entry lens_facing = {0};
        int matches = !use_facing && device_id &&
                      strcmp(device_id, camera_id) == 0;

        if (ACameraManager_getCameraCharacteristics(
                manager, camera_id, &metadata) != ACAMERA_OK || !metadata) {
            continue;
        }
        if (use_facing &&
            ACameraMetadata_getConstEntry(
                metadata, ACAMERA_LENS_FACING, &lens_facing) == ACAMERA_OK &&
            lens_facing.count > 0 &&
            lens_facing.data.u8[0] == requested_facing) {
            matches = 1;
        }
        if (matches && strlen(camera_id) < sizeof(ctx->camera_id)) {
            memcpy(ctx->camera_id, camera_id, strlen(camera_id) + 1);
            ctx->metadata = metadata;
            break;
        }
        ACameraMetadata_free(metadata);
    }
    ACameraManager_deleteCameraIdList(camera_ids);

    if (!ctx->metadata) {
        ACameraManager_delete(ctx->manager);
        free(ctx);
        return TURBO_CAPTURE_ERR_DEVICE;
    }
    *backend_ctx = ctx;
    return TURBO_CAPTURE_OK;
}

static void android_video_device_close(void *backend_ctx) {
    android_video_device_ctx_t *ctx =
        (android_video_device_ctx_t *)backend_ctx;
    if (!ctx) return;
    if (ctx->metadata) ACameraMetadata_free(ctx->metadata);
    if (ctx->manager) ACameraManager_delete(ctx->manager);
    free(ctx);
}

static int android_video_device_list_modes(
    void *backend_ctx,
    turbo_video_native_mode_t *modes,
    size_t capacity,
    size_t *out_count) {
    android_video_device_ctx_t *ctx =
        (android_video_device_ctx_t *)backend_ctx;
    ACameraMetadata_const_entry configurations;
    ACameraMetadata_const_entry fps_ranges;
    uint32_t stream_count;
    uint32_t fps_count;
    size_t count = 0;

    if (!ctx || !modes || capacity == 0 || !out_count) {
        return TURBO_CAPTURE_ERR_FORMAT;
    }
    if (ACameraMetadata_getConstEntry(
            ctx->metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
            &configurations) != ACAMERA_OK ||
        ACameraMetadata_getConstEntry(
            ctx->metadata, ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
            &fps_ranges) != ACAMERA_OK) {
        return TURBO_CAPTURE_ERR_DEVICE;
    }

    memset(modes, 0, sizeof(*modes) * capacity);
    stream_count = configurations.count / 4u;
    fps_count = fps_ranges.count / 2u;
    for (uint32_t stream_index = 0; stream_index < stream_count;
         ++stream_index) {
        for (uint32_t fps_index = 0; fps_index < fps_count; ++fps_index) {
            uint64_t mode_id = ((uint64_t)stream_index << 32) | fps_index;
            if (android_video_mode_from_metadata(
                    ctx->metadata, mode_id, &modes[count], NULL, NULL) ==
                TURBO_CAPTURE_OK) {
                if (++count == capacity) goto done;
            }
        }
    }

done:
    *out_count = count;
    return TURBO_CAPTURE_OK;
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

static int android_video_device_create_capture(
    void *backend_ctx,
    const turbo_video_native_mode_t *mode,
    turbo_capture_t **out_capture) {
    android_video_device_ctx_t *device =
        (android_video_device_ctx_t *)backend_ctx;
    ACameraMetadata *metadata = NULL;
    turbo_video_native_mode_t actual_mode;
    turbo_capture_t *capture;
    android_video_platform_t *platform;
    int min_framerate = 0;
    int max_framerate = 0;

    if (!device || !mode || !out_capture) return TURBO_CAPTURE_ERR_FORMAT;
    *out_capture = NULL;
    if (ACameraManager_getCameraCharacteristics(
            device->manager, device->camera_id, &metadata) != ACAMERA_OK ||
        !metadata) {
        return TURBO_CAPTURE_ERR_DEVICE;
    }
    if (android_video_mode_from_metadata(
            metadata, mode->mode_id, &actual_mode,
            &min_framerate, &max_framerate) != TURBO_CAPTURE_OK ||
        actual_mode.width != mode->width ||
        actual_mode.height != mode->height ||
        actual_mode.framerate_numerator != mode->framerate_numerator ||
        actual_mode.framerate_denominator != mode->framerate_denominator ||
        actual_mode.format != mode->format ||
        actual_mode.mode_id != mode->mode_id) {
        ACameraMetadata_free(metadata);
        return TURBO_CAPTURE_ERR_FORMAT;
    }
    ACameraMetadata_free(metadata);

    capture = (turbo_capture_t *)calloc(1, sizeof(*capture));
    platform = (android_video_platform_t *)calloc(1, sizeof(*platform));
    if (!capture || !platform) {
        free(capture);
        free(platform);
        return TURBO_CAPTURE_ERR_NOMEM;
    }
    platform->native = android_camera_create(
        mode->width, mode->height, min_framerate, max_framerate,
        device->camera_id);
    if (!platform->native) {
        free(platform);
        free(capture);
        return TURBO_CAPTURE_ERR_DEVICE;
    }

    capture->type = TURBO_CAPTURE_TYPE_VIDEO;
    capture->state = TURBO_CAPTURE_STATE_STOPPED;
    capture->platform_ctx = platform;
    android_camera_set_callback(platform->native,
                                android_video_callback, capture);
    *out_capture = capture;
    return TURBO_CAPTURE_OK;
}

const turbo_video_backend_ops_t *turbo_video_platform_backend(void) {
    static const turbo_video_backend_ops_t ops = {
        android_video_device_open,
        android_video_device_close,
        android_video_device_list_modes,
        android_video_device_create_capture
    };
    return &ops;
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
