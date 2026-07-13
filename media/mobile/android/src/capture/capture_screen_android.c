/**
 * Android Screen Capture
 * 
 * Uses MediaProjection API for screen recording
 * Requires Java/JNI bridge for permission handling
 */
#include <android/log.h>
#include <jni.h>
#include <media/NdkImageReader.h>
#include <media/NdkImage.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "TurboNetScreen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* =============================================================================
 * Screen Capture Context
 * ============================================================================= */

typedef struct android_screen_ctx_t {
    AImageReader *image_reader;
    ANativeWindow *image_reader_window;
    
    /* Configuration */
    int width;
    int height;
    int framerate;
    
    /* State */
    int capturing;
    
    /* Java objects (passed from JNI) */
    jobject media_projection;  /* MediaProjection instance */
    jobject virtual_display;   /* VirtualDisplay instance */
    
    /* Callback */
    void (*on_frame)(void *user_data, const uint8_t *data, size_t len,
                     int width, int height, int64_t timestamp_us);
    void *user_data;
} android_screen_ctx_t;

int android_screen_stop(android_screen_ctx_t *ctx);

/* =============================================================================
 * Image Reader Callback
 * ============================================================================= */

static void on_image_available(void *context, AImageReader *reader) {
    android_screen_ctx_t *ctx = (android_screen_ctx_t *)context;
    
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
    
    /* Get RGBA plane (screen capture is typically RGBA) */
    uint8_t *rgba_data = NULL;
    int rgba_len = 0;
    AImage_getPlaneData(image, 0, &rgba_data, &rgba_len);
    
    /* Convert RGBA to I420 for encoding */
    size_t i420_size = width * height * 3 / 2;
    uint8_t *i420_data = (uint8_t *)malloc(i420_size);
    
    if (i420_data && rgba_data) {
        /* Simple RGBA to I420 conversion */
        uint8_t *y = i420_data;
        uint8_t *u = y + width * height;
        uint8_t *v = u + (width * height / 4);
        
        for (int i = 0; i < height; i++) {
            for (int j = 0; j < width; j++) {
                int rgba_idx = (i * width + j) * 4;
                int y_idx = i * width + j;
                
                uint8_t r = rgba_data[rgba_idx];
                uint8_t g = rgba_data[rgba_idx + 1];
                uint8_t b = rgba_data[rgba_idx + 2];
                
                /* RGB to Y */
                y[y_idx] = (uint8_t)((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                
                /* Subsample for U and V */
                if (i % 2 == 0 && j % 2 == 0) {
                    int uv_idx = (i / 2) * (width / 2) + (j / 2);
                    
                    /* RGB to U */
                    u[uv_idx] = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    
                    /* RGB to V */
                    v[uv_idx] = (uint8_t)((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                }
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
 * Screen Capture Management
 * ============================================================================= */

android_screen_ctx_t *android_screen_create(int width, int height, int framerate) {
    android_screen_ctx_t *ctx = (android_screen_ctx_t *)calloc(1, sizeof(android_screen_ctx_t));
    if (!ctx) return NULL;
    
    ctx->width = width;
    ctx->height = height;
    ctx->framerate = framerate;
    ctx->capturing = 0;
    
    /* Create image reader for RGBA format */
    media_status_t status = AImageReader_new(
        width, height,
        AIMAGE_FORMAT_RGBA_8888,
        2,  /* Max images */
        &ctx->image_reader
    );
    
    if (status != AMEDIA_OK) {
        LOGE("Failed to create image reader");
        free(ctx);
        return NULL;
    }
    
    /* Set image listener */
    image_listener.context = ctx;
    AImageReader_setImageListener(ctx->image_reader, &image_listener);
    
    /* Get image reader window */
    AImageReader_getWindow(ctx->image_reader, &ctx->image_reader_window);
    
    LOGI("Screen capture created: %dx%d @ %dfps", width, height, framerate);
    
    return ctx;
}

void android_screen_destroy(android_screen_ctx_t *ctx) {
    if (!ctx) return;
    
    if (ctx->capturing) {
        android_screen_stop(ctx);
    }
    
    if (ctx->image_reader) {
        AImageReader_delete(ctx->image_reader);
    }
    
    free(ctx);
}

/* Note: Start/stop require Java bridge for MediaProjection */
/* These are called from JNI after permission is granted */

int android_screen_start(android_screen_ctx_t *ctx, jobject media_projection) {
    if (!ctx || ctx->capturing) return -1;
    
    ctx->media_projection = media_projection;
    ctx->capturing = 1;
    
    LOGI("Screen capture started");
    
    /* Actual VirtualDisplay creation happens in Java */
    /* Java code will create VirtualDisplay with ctx->image_reader_window as surface */
    
    return 0;
}

int android_screen_stop(android_screen_ctx_t *ctx) {
    if (!ctx || !ctx->capturing) return -1;
    
    ctx->capturing = 0;
    ctx->media_projection = NULL;
    ctx->virtual_display = NULL;
    
    LOGI("Screen capture stopped");
    
    return 0;
}

void android_screen_set_callback(android_screen_ctx_t *ctx,
                                 void (*callback)(void *user_data,
                                                 const uint8_t *data, size_t len,
                                                 int width, int height,
                                                 int64_t timestamp_us),
                                 void *user_data) {
    if (!ctx) return;
    ctx->on_frame = callback;
    ctx->user_data = user_data;
}

ANativeWindow *android_screen_get_surface(android_screen_ctx_t *ctx) {
    return ctx ? ctx->image_reader_window : NULL;
}
