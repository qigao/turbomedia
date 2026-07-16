/**
 * Android Video Capture (Camera)
 * 
 * Uses Android NDK Camera2 API for native camera access
 */
#include <android/log.h>
#include <android/native_window.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCaptureRequest.h>
#include <dlfcn.h>
#include <libyuv/convert.h>
#include <media/NdkImageReader.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "TurboNetCamera"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* =============================================================================
 * Camera Context
 * ============================================================================= */

typedef struct android_camera_ctx_t {
    ACameraManager *camera_manager;
    ACameraDevice *camera_device;
    ACameraCaptureSession *capture_session;
    AImageReader *image_reader;
    ANativeWindow *image_reader_window;
    ACameraOutputTarget *output_target;
    ACameraDevice_stateCallbacks device_callbacks;
    AImageReader_ImageListener image_listener;
    
    /* Configuration */
    int width;
    int height;
    int framerate;
    int facing;  /* 0 = back, 1 = front */
    
    /* State */
    int capturing;
    
    /* Callback */
    void (*on_frame)(void *user_data, const uint8_t *data, size_t len,
                     int width, int height, int64_t timestamp_us);
    void *user_data;
} android_camera_ctx_t;

int android_camera_stop(android_camera_ctx_t *ctx);

/* =============================================================================
 * Camera Callbacks
 * ============================================================================= */

static void camera_device_on_disconnected(void *context, ACameraDevice *device) {
    LOGI("Camera disconnected");
    android_camera_ctx_t *ctx = (android_camera_ctx_t *)context;
    ctx->capturing = 0;
}

static void camera_device_on_error(void *context, ACameraDevice *device, int error) {
    LOGE("Camera error: %d", error);
    android_camera_ctx_t *ctx = (android_camera_ctx_t *)context;
    ctx->capturing = 0;
}

static void camera_session_on_closed(void *context, ACameraCaptureSession *session) {
    (void)context;
    (void)session;
    LOGI("Camera session closed");
}

static void camera_session_on_ready(void *context, ACameraCaptureSession *session) {
    (void)context;
    (void)session;
    LOGI("Camera session ready");
}

static void camera_session_on_active(void *context, ACameraCaptureSession *session) {
    (void)context;
    (void)session;
    LOGI("Camera session active");
}

static ACameraCaptureSession_stateCallbacks camera_session_callbacks = {
    .context = NULL,
    .onClosed = camera_session_on_closed,
    .onReady = camera_session_on_ready,
    .onActive = camera_session_on_active,
};

static pthread_once_t camera_binder_once = PTHREAD_ONCE_INIT;
static void *camera_binder_handle;

static void camera_start_binder_thread_pool(void) {
    typedef void (*binder_start_thread_pool_fn)(void);

    camera_binder_handle = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
    if (!camera_binder_handle) {
        LOGI("NDK binder thread-pool API is unavailable on this Android version");
        return;
    }

    binder_start_thread_pool_fn start_thread_pool =
        (binder_start_thread_pool_fn)dlsym(camera_binder_handle,
                                           "ABinderProcess_startThreadPool");
    if (!start_thread_pool) {
        LOGI("NDK binder thread-pool entry point is unavailable on this Android version");
        dlclose(camera_binder_handle);
        camera_binder_handle = NULL;
        return;
    }

    start_thread_pool();
}

static int image_plane_covers(int length,
                              int row_stride,
                              int pixel_stride,
                              size_t width,
                              size_t height) {
    if (length < 0 || row_stride <= 0 || pixel_stride <= 0 ||
        width == 0u || height == 0u) {
        return 0;
    }

    if (width - 1u > (SIZE_MAX - 1u) / (size_t)pixel_stride) return 0;
    size_t last_row_bytes = (width - 1u) * (size_t)pixel_stride + 1u;
    if (height - 1u > (SIZE_MAX - last_row_bytes) / (size_t)row_stride) return 0;
    size_t required = (height - 1u) * (size_t)row_stride + last_row_bytes;
    return (size_t)length >= required;
}

/* Image reader callback */
static void on_image_available(void *context, AImageReader *reader) {
    android_camera_ctx_t *ctx = (android_camera_ctx_t *)context;
    
    AImage *image = NULL;
    media_status_t status = AImageReader_acquireLatestImage(reader, &image);
    
    if (status != AMEDIA_OK || !image) {
        return;
    }
    
    int32_t format = 0;
    int32_t width = 0;
    int32_t height = 0;
    int64_t timestamp = 0;
    if (AImage_getFormat(image, &format) != AMEDIA_OK ||
        AImage_getWidth(image, &width) != AMEDIA_OK ||
        AImage_getHeight(image, &height) != AMEDIA_OK ||
        AImage_getTimestamp(image, &timestamp) != AMEDIA_OK ||
        format != AIMAGE_FORMAT_YUV_420_888 || width <= 0 || height <= 0) {
        LOGE("Invalid Camera2 image metadata");
        AImage_delete(image);
        return;
    }

    uint8_t *y_data = NULL;
    int y_len = 0;
    uint8_t *u_data = NULL;
    int u_len = 0;
    uint8_t *v_data = NULL;
    int v_len = 0;
    int32_t y_stride = 0;
    int32_t u_stride = 0;
    int32_t v_stride = 0;
    int32_t y_pixel_stride = 0;
    int32_t u_pixel_stride = 0;
    int32_t v_pixel_stride = 0;
    if (AImage_getPlaneData(image, 0, &y_data, &y_len) != AMEDIA_OK ||
        AImage_getPlaneData(image, 1, &u_data, &u_len) != AMEDIA_OK ||
        AImage_getPlaneData(image, 2, &v_data, &v_len) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, 0, &y_stride) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, 1, &u_stride) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, 2, &v_stride) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, 0, &y_pixel_stride) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, 1, &u_pixel_stride) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, 2, &v_pixel_stride) != AMEDIA_OK ||
        !y_data || !u_data || !v_data || y_len < 0 || u_len < 0 || v_len < 0 ||
        y_pixel_stride != 1 || u_pixel_stride <= 0 ||
        u_pixel_stride != v_pixel_stride) {
        LOGE("Invalid Camera2 YUV planes");
        AImage_delete(image);
        return;
    }

    size_t uv_width = ((size_t)width + 1u) / 2u;
    size_t uv_height = ((size_t)height + 1u) / 2u;
    if (!image_plane_covers(y_len, y_stride, y_pixel_stride,
                            (size_t)width, (size_t)height) ||
        !image_plane_covers(u_len, u_stride, u_pixel_stride, uv_width, uv_height) ||
        !image_plane_covers(v_len, v_stride, v_pixel_stride, uv_width, uv_height) ||
        (size_t)width > SIZE_MAX / (size_t)height ||
        uv_width > SIZE_MAX / uv_height) {
        LOGE("Camera2 YUV planes do not cover the declared image dimensions");
        AImage_delete(image);
        return;
    }

    size_t y_size = (size_t)width * (size_t)height;
    size_t uv_size = uv_width * uv_height;
    if (uv_size > (SIZE_MAX - y_size) / 2u) {
        LOGE("Camera2 frame dimensions overflow I420 buffer size");
        AImage_delete(image);
        return;
    }

    size_t i420_size = y_size + 2u * uv_size;
    uint8_t *i420_data = (uint8_t *)malloc(i420_size);
    if (i420_data) {
        uint8_t *u_dst = i420_data + y_size;
        uint8_t *v_dst = u_dst + uv_size;
        int convert_result = Android420ToI420(y_data, y_stride,
                                              u_data, u_stride,
                                              v_data, v_stride,
                                              u_pixel_stride,
                                              i420_data, width,
                                              u_dst, (int)uv_width,
                                              v_dst, (int)uv_width,
                                              width, height);
        if (convert_result == 0) {
            if (ctx->on_frame) {
                ctx->on_frame(ctx->user_data, i420_data, i420_size,
                              width, height, timestamp / 1000);  /* ns to us */
            }
        } else {
            LOGE("Failed to convert Camera2 frame to I420: %d", convert_result);
        }

        free(i420_data);
    }
    
    AImage_delete(image);
}

/* =============================================================================
 * Camera Management
 * ============================================================================= */

android_camera_ctx_t *android_camera_create(int width, int height, int framerate, int facing) {
    android_camera_ctx_t *ctx = (android_camera_ctx_t *)calloc(1, sizeof(android_camera_ctx_t));
    if (!ctx) return NULL;
    
    ctx->width = width;
    ctx->height = height;
    ctx->framerate = framerate;
    ctx->facing = facing;
    ctx->capturing = 0;
    ctx->device_callbacks.context = ctx;
    ctx->device_callbacks.onDisconnected = camera_device_on_disconnected;
    ctx->device_callbacks.onError = camera_device_on_error;
    ctx->image_listener.context = ctx;
    ctx->image_listener.onImageAvailable = on_image_available;

    pthread_once(&camera_binder_once, camera_start_binder_thread_pool);
    
    /* Create camera manager */
    ctx->camera_manager = ACameraManager_create();
    if (!ctx->camera_manager) {
        LOGE("Failed to create camera manager");
        free(ctx);
        return NULL;
    }
    
    /* Create image reader */
    media_status_t status = AImageReader_new(
        width, height,
        AIMAGE_FORMAT_YUV_420_888,
        2,  /* Max images */
        &ctx->image_reader
    );
    
    if (status != AMEDIA_OK) {
        LOGE("Failed to create image reader");
        ACameraManager_delete(ctx->camera_manager);
        free(ctx);
        return NULL;
    }
    
    /* Set image listener */
    status = AImageReader_setImageListener(ctx->image_reader, &ctx->image_listener);
    if (status != AMEDIA_OK) {
        LOGE("Failed to set image reader listener: %d", status);
        AImageReader_delete(ctx->image_reader);
        ACameraManager_delete(ctx->camera_manager);
        free(ctx);
        return NULL;
    }
    
    /* Get image reader window */
    status = AImageReader_getWindow(ctx->image_reader, &ctx->image_reader_window);
    if (status != AMEDIA_OK || !ctx->image_reader_window ||
        ACameraOutputTarget_create(ctx->image_reader_window, &ctx->output_target) != ACAMERA_OK) {
        LOGE("Failed to create camera output target");
        AImageReader_delete(ctx->image_reader);
        ACameraManager_delete(ctx->camera_manager);
        free(ctx);
        return NULL;
    }
    
    LOGI("Camera created: %dx%d @ %dfps", width, height, framerate);
    
    return ctx;
}

void android_camera_destroy(android_camera_ctx_t *ctx) {
    if (!ctx) return;
    
    if (ctx->capturing) {
        android_camera_stop(ctx);
    }
    
    if (ctx->image_reader) {
        ACameraOutputTarget_free(ctx->output_target);
        AImageReader_delete(ctx->image_reader);
    }
    
    if (ctx->camera_manager) {
        ACameraManager_delete(ctx->camera_manager);
    }
    
    free(ctx);
}

int android_camera_start(android_camera_ctx_t *ctx) {
    if (!ctx || ctx->capturing) return -1;

    camera_status_t status;
    ACaptureRequest *capture_request = NULL;
    ACaptureSessionOutputContainer *output_container = NULL;
    ACaptureSessionOutput *session_output = NULL;
    
    /* Get camera ID list */
    ACameraIdList *camera_id_list = NULL;
    status = ACameraManager_getCameraIdList(ctx->camera_manager, &camera_id_list);
    
    if (status != ACAMERA_OK || !camera_id_list || camera_id_list->numCameras == 0) {
        LOGE("Failed to enumerate cameras: %d", status);
        if (camera_id_list) {
            ACameraManager_deleteCameraIdList(camera_id_list);
        }
        return -1;
    }
    
    /* Find camera with desired facing */
    const char *camera_id = NULL;
    for (int i = 0; i < camera_id_list->numCameras; i++) {
        ACameraMetadata *metadata = NULL;
        status = ACameraManager_getCameraCharacteristics(
            ctx->camera_manager,
            camera_id_list->cameraIds[i],
            &metadata
        );
        
        if (status == ACAMERA_OK && metadata) {
            ACameraMetadata_const_entry entry;
            status = ACameraMetadata_getConstEntry(
                metadata,
                ACAMERA_LENS_FACING,
                &entry
            );

            if (status == ACAMERA_OK && entry.count > 0 &&
                ((ctx->facing == 0 && entry.data.u8[0] == ACAMERA_LENS_FACING_BACK) ||
                 (ctx->facing == 1 && entry.data.u8[0] == ACAMERA_LENS_FACING_FRONT))) {
                camera_id = camera_id_list->cameraIds[i];
                ACameraMetadata_free(metadata);
                break;
            }
            
            ACameraMetadata_free(metadata);
        }
    }
    
    if (!camera_id) {
        LOGE("Camera not found");
        ACameraManager_deleteCameraIdList(camera_id_list);
        return -1;
    }
    
    /* Open camera */
    status = ACameraManager_openCamera(
        ctx->camera_manager,
        camera_id,
        &ctx->device_callbacks,
        &ctx->camera_device
    );
    
    ACameraManager_deleteCameraIdList(camera_id_list);
    
    if (status != ACAMERA_OK) {
        LOGE("Failed to open camera: %d", status);
        return -1;
    }
    
    /* Create capture request */
    status = ACameraDevice_createCaptureRequest(
        ctx->camera_device,
        TEMPLATE_PREVIEW,
        &capture_request
    );
    if (status != ACAMERA_OK || !capture_request) {
        LOGE("Failed to create capture request: %d", status);
        goto fail;
    }
    
    /* Add target window */
    status = ACaptureRequest_addTarget(capture_request, ctx->output_target);
    if (status != ACAMERA_OK) {
        LOGE("Failed to add camera output target: %d", status);
        goto fail;
    }
    
    /* Set FPS range */
    int32_t fps_range[2] = {ctx->framerate, ctx->framerate};
    status = ACaptureRequest_setEntry_i32(
        capture_request,
        ACAMERA_CONTROL_AE_TARGET_FPS_RANGE,
        2,
        fps_range
    );
    if (status != ACAMERA_OK) {
        LOGE("Failed to set camera FPS range: %d", status);
        goto fail;
    }
    
    /* Create capture session */
    status = ACaptureSessionOutputContainer_create(&output_container);
    if (status != ACAMERA_OK || !output_container) {
        LOGE("Failed to create camera output container: %d", status);
        goto fail;
    }

    status = ACaptureSessionOutput_create(ctx->image_reader_window, &session_output);
    if (status != ACAMERA_OK || !session_output) {
        LOGE("Failed to create camera session output: %d", status);
        goto fail;
    }

    status = ACaptureSessionOutputContainer_add(output_container, session_output);
    if (status != ACAMERA_OK) {
        LOGE("Failed to add camera session output: %d", status);
        goto fail;
    }

    status = ACameraDevice_createCaptureSession(
        ctx->camera_device,
        output_container,
        &camera_session_callbacks,
        &ctx->capture_session
    );
    if (status != ACAMERA_OK || !ctx->capture_session) {
        LOGE("Failed to create camera capture session: %d", status);
        goto fail;
    }
    
    /* Start repeating request */
    status = ACameraCaptureSession_setRepeatingRequest(
        ctx->capture_session,
        NULL,  /* Capture callbacks */
        1,
        &capture_request,
        NULL   /* Sequence ID */
    );
    if (status != ACAMERA_OK) {
        LOGE("Failed to start camera repeating request: %d", status);
        goto fail;
    }
    
    /* Cleanup */
    ACaptureRequest_free(capture_request);
    ACaptureSessionOutput_free(session_output);
    ACaptureSessionOutputContainer_free(output_container);
    
    ctx->capturing = 1;
    
    LOGI("Camera started");
    
    return 0;

fail:
    if (ctx->capture_session) {
        ACameraCaptureSession_close(ctx->capture_session);
        ctx->capture_session = NULL;
    }
    if (session_output) {
        ACaptureSessionOutput_free(session_output);
    }
    if (output_container) {
        ACaptureSessionOutputContainer_free(output_container);
    }
    if (capture_request) {
        ACaptureRequest_free(capture_request);
    }
    if (ctx->camera_device) {
        ACameraDevice_close(ctx->camera_device);
        ctx->camera_device = NULL;
    }
    return -1;
}

int android_camera_stop(android_camera_ctx_t *ctx) {
    if (!ctx || !ctx->capturing) return -1;
    
    /* Stop capture session */
    if (ctx->capture_session) {
        ACameraCaptureSession_stopRepeating(ctx->capture_session);
        ACameraCaptureSession_close(ctx->capture_session);
        ctx->capture_session = NULL;
    }
    
    /* Close camera */
    if (ctx->camera_device) {
        ACameraDevice_close(ctx->camera_device);
        ctx->camera_device = NULL;
    }
    
    ctx->capturing = 0;
    
    LOGI("Camera stopped");
    
    return 0;
}

void android_camera_set_callback(android_camera_ctx_t *ctx,
                                 void (*callback)(void *user_data,
                                                 const uint8_t *data, size_t len,
                                                 int width, int height,
                                                 int64_t timestamp_us),
                                 void *user_data) {
    if (!ctx) return;
    ctx->on_frame = callback;
    ctx->user_data = user_data;
}
