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
#include <media/NdkImageReader.h>
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

static ACameraDevice_stateCallbacks camera_device_callbacks = {
    .context = NULL,
    .onDisconnected = camera_device_on_disconnected,
    .onError = camera_device_on_error,
};

/* Image reader callback */
static void on_image_available(void *context, AImageReader *reader) {
    android_camera_ctx_t *ctx = (android_camera_ctx_t *)context;
    
    AImage *image = NULL;
    media_status_t status = AImageReader_acquireLatestImage(reader, &image);
    
    if (status != AMEDIA_OK || !image) {
        return;
    }
    
    /* Get image properties */
    int32_t format;
    AImage_getFormat(image, &format);
    
    int32_t width, height;
    AImage_getWidth(image, &width);
    AImage_getHeight(image, &height);
    
    int64_t timestamp;
    AImage_getTimestamp(image, &timestamp);
    
    /* Get Y plane (for YUV_420_888) */
    uint8_t *y_data = NULL;
    int y_len = 0;
    AImage_getPlaneData(image, 0, &y_data, &y_len);
    
    /* Get U plane */
    uint8_t *u_data = NULL;
    int u_len = 0;
    AImage_getPlaneData(image, 1, &u_data, &u_len);
    
    /* Get V plane */
    uint8_t *v_data = NULL;
    int v_len = 0;
    AImage_getPlaneData(image, 2, &v_data, &v_len);
    
    /* Convert to I420 format */
    size_t i420_size = width * height * 3 / 2;
    uint8_t *i420_data = (uint8_t *)malloc(i420_size);
    
    if (i420_data) {
        /* Copy Y plane */
        int32_t y_stride, y_pixel_stride;
        AImage_getPlaneRowStride(image, 0, &y_stride);
        AImage_getPlanePixelStride(image, 0, &y_pixel_stride);
        
        for (int i = 0; i < height; i++) {
            memcpy(i420_data + i * width, y_data + i * y_stride, width);
        }
        
        /* Copy U and V planes (semi-planar to planar conversion) */
        int32_t uv_stride, uv_pixel_stride;
        AImage_getPlaneRowStride(image, 1, &uv_stride);
        AImage_getPlanePixelStride(image, 1, &uv_pixel_stride);
        
        uint8_t *u_dst = i420_data + width * height;
        uint8_t *v_dst = u_dst + (width * height / 4);
        
        for (int i = 0; i < height / 2; i++) {
            for (int j = 0; j < width / 2; j++) {
                u_dst[i * (width / 2) + j] = u_data[i * uv_stride + j * uv_pixel_stride];
                v_dst[i * (width / 2) + j] = v_data[i * uv_stride + j * uv_pixel_stride];
            }
        }
        
        /* Call user callback */
        if (ctx->on_frame) {
            ctx->on_frame(ctx->user_data, i420_data, i420_size,
                         width, height, timestamp / 1000);  /* ns to us */
        }
        
        free(i420_data);
    }
    
    AImage_delete(image);
}

static AImageReader_ImageListener image_listener = {
    .context = NULL,
    .onImageAvailable = on_image_available,
};

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
    image_listener.context = ctx;
    AImageReader_setImageListener(ctx->image_reader, &image_listener);
    
    /* Get image reader window */
    AImageReader_getWindow(ctx->image_reader, &ctx->image_reader_window);
    
    LOGI("Camera created: %dx%d @ %dfps", width, height, framerate);
    
    return ctx;
}

void android_camera_destroy(android_camera_ctx_t *ctx) {
    if (!ctx) return;
    
    if (ctx->capturing) {
        android_camera_stop(ctx);
    }
    
    if (ctx->image_reader) {
        AImageReader_delete(ctx->image_reader);
    }
    
    if (ctx->camera_manager) {
        ACameraManager_delete(ctx->camera_manager);
    }
    
    free(ctx);
}

int android_camera_start(android_camera_ctx_t *ctx) {
    if (!ctx || ctx->capturing) return -1;
    
    /* Get camera ID list */
    ACameraIdList *camera_id_list = NULL;
    ACameraManager_getCameraIdList(ctx->camera_manager, &camera_id_list);
    
    if (!camera_id_list || camera_id_list->numCameras == 0) {
        LOGE("No cameras available");
        return -1;
    }
    
    /* Find camera with desired facing */
    const char *camera_id = NULL;
    for (int i = 0; i < camera_id_list->numCameras; i++) {
        ACameraMetadata *metadata = NULL;
        ACameraManager_getCameraCharacteristics(
            ctx->camera_manager,
            camera_id_list->cameraIds[i],
            &metadata
        );
        
        if (metadata) {
            ACameraMetadata_const_entry entry;
            ACameraMetadata_getConstEntry(
                metadata,
                ACAMERA_LENS_FACING,
                &entry
            );
            
            int facing = entry.data.u8[0];
            if ((ctx->facing == 0 && facing == ACAMERA_LENS_FACING_BACK) ||
                (ctx->facing == 1 && facing == ACAMERA_LENS_FACING_FRONT)) {
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
    camera_device_callbacks.context = ctx;
    camera_status_t status = ACameraManager_openCamera(
        ctx->camera_manager,
        camera_id,
        &camera_device_callbacks,
        &ctx->camera_device
    );
    
    ACameraManager_deleteCameraIdList(camera_id_list);
    
    if (status != ACAMERA_OK) {
        LOGE("Failed to open camera: %d", status);
        return -1;
    }
    
    /* Create capture request */
    ACaptureRequest *capture_request = NULL;
    ACameraDevice_createCaptureRequest(
        ctx->camera_device,
        TEMPLATE_PREVIEW,
        &capture_request
    );
    
    /* Add target window */
    ACaptureRequest_addTarget(capture_request, ctx->image_reader_window);
    
    /* Set FPS range */
    int32_t fps_range[2] = {ctx->framerate, ctx->framerate};
    ACaptureRequest_setEntry_i32(
        capture_request,
        ACAMERA_CONTROL_AE_TARGET_FPS_RANGE,
        2,
        fps_range
    );
    
    /* Create capture session */
    ACaptureSessionOutputContainer *output_container = NULL;
    ACaptureSessionOutput *session_output = NULL;
    
    ACaptureSessionOutputContainer_create(&output_container);
    ACaptureSessionOutput_create(ctx->image_reader_window, &session_output);
    ACaptureSessionOutputContainer_add(output_container, session_output);
    
    ACameraDevice_createCaptureSession(
        ctx->camera_device,
        output_container,
        NULL,  /* Session state callbacks */
        &ctx->capture_session
    );
    
    /* Start repeating request */
    ACameraCaptureSession_setRepeatingRequest(
        ctx->capture_session,
        NULL,  /* Capture callbacks */
        1,
        &capture_request,
        NULL   /* Sequence ID */
    );
    
    /* Cleanup */
    ACaptureRequest_free(capture_request);
    ACaptureSessionOutput_free(session_output);
    ACaptureSessionOutputContainer_free(output_container);
    
    ctx->capturing = 1;
    
    LOGI("Camera started");
    
    return 0;
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
