/**
 * Android Screen Capture
 * 
 * Uses MediaProjection API for screen recording
 * Requires Java/JNI bridge for permission handling
 */
#include <android/log.h>
#include <jni.h>
#include <libyuv/convert.h>
#include <limits.h>
#include <media/NdkImageReader.h>
#include <media/NdkImage.h>
#include <stdint.h>
#include <stdatomic.h>
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
    AImageReader_ImageListener image_listener;
    
    /* Configuration */
    int width;
    int height;
    int framerate;
    
    /* State */
    int capturing;
    atomic_uint_fast64_t frame_count;
    
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
    int32_t format = 0;
    int32_t width, height;
    int64_t timestamp = 0;

    if (AImage_getFormat(image, &format) != AMEDIA_OK ||
        AImage_getWidth(image, &width) != AMEDIA_OK ||
        AImage_getHeight(image, &height) != AMEDIA_OK ||
        AImage_getTimestamp(image, &timestamp) != AMEDIA_OK ||
        format != AIMAGE_FORMAT_RGBA_8888 || width <= 0 || height <= 0) {
        LOGE("Invalid screen image metadata");
        AImage_delete(image);
        return;
    }
    
    /* Get RGBA plane (screen capture is typically RGBA) */
    uint8_t *rgba_data = NULL;
    int rgba_len = 0;
    int32_t rgba_stride = 0;
    int32_t rgba_pixel_stride = 0;
    if (width > INT_MAX / 4) {
        LOGE("Screen frame width exceeds RGBA stride range");
        AImage_delete(image);
        return;
    }

    if (AImage_getPlaneData(image, 0, &rgba_data, &rgba_len) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, 0, &rgba_stride) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, 0, &rgba_pixel_stride) != AMEDIA_OK ||
        !rgba_data || rgba_len < 0 || rgba_stride < width * 4 || rgba_pixel_stride != 4) {
        LOGE("Invalid RGBA screen image plane");
        AImage_delete(image);
        return;
    }

    size_t rgba_row_tail = (size_t)width * 4u;
    size_t prior_rows = (size_t)(height - 1);
    if ((prior_rows > 0u &&
         (size_t)rgba_stride > (SIZE_MAX - rgba_row_tail) / prior_rows) ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        LOGE("Screen frame dimensions exceed image buffer bounds");
        AImage_delete(image);
        return;
    }

    size_t required_rgba_size = (size_t)rgba_stride * prior_rows + rgba_row_tail;
    if ((size_t)rgba_len < required_rgba_size) {
        LOGE("RGBA screen image plane is smaller than its declared strides");
        AImage_delete(image);
        return;
    }

    size_t y_size = (size_t)width * (size_t)height;
    size_t uv_width = ((size_t)width + 1u) / 2u;
    size_t uv_height = ((size_t)height + 1u) / 2u;
    size_t uv_size = uv_width * uv_height;
    if (uv_size > (SIZE_MAX - y_size) / 2u) {
        LOGE("Screen frame dimensions overflow I420 buffer size");
        AImage_delete(image);
        return;
    }

    size_t i420_size = y_size + 2u * uv_size;
    uint8_t *i420_data = (uint8_t *)malloc(i420_size);
    if (i420_data) {
        uint8_t *y = i420_data;
        uint8_t *u = y + y_size;
        uint8_t *v = u + uv_size;

        int convert_result = ABGRToI420(rgba_data, rgba_stride,
                                        y, width,
                                        u, (int)uv_width,
                                        v, (int)uv_width,
                                        width, height);
        if (convert_result == 0) {
            atomic_fetch_add_explicit(&ctx->frame_count, 1u, memory_order_relaxed);
            if (ctx->on_frame) {
                ctx->on_frame(ctx->user_data, i420_data, i420_size,
                              width, height, timestamp / 1000);  /* ns to us */
            }
        } else {
            LOGE("Failed to convert screen frame to I420: %d", convert_result);
        }

        free(i420_data);
    }
    
    AImage_delete(image);
}

/* =============================================================================
 * Screen Capture Management
 * ============================================================================= */

android_screen_ctx_t *android_screen_create(int width, int height, int framerate) {
    if (width <= 0 || height <= 0 || framerate <= 0) return NULL;

    android_screen_ctx_t *ctx = (android_screen_ctx_t *)calloc(1, sizeof(android_screen_ctx_t));
    if (!ctx) return NULL;
    
    ctx->width = width;
    ctx->height = height;
    ctx->framerate = framerate;
    ctx->capturing = 0;
    atomic_init(&ctx->frame_count, 0u);
    ctx->image_listener.context = ctx;
    ctx->image_listener.onImageAvailable = on_image_available;
    
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
    status = AImageReader_setImageListener(ctx->image_reader, &ctx->image_listener);
    if (status != AMEDIA_OK) {
        LOGE("Failed to set screen image listener: %d", status);
        AImageReader_delete(ctx->image_reader);
        free(ctx);
        return NULL;
    }
    
    /* Get image reader window */
    status = AImageReader_getWindow(ctx->image_reader, &ctx->image_reader_window);
    if (status != AMEDIA_OK || !ctx->image_reader_window) {
        LOGE("Failed to get screen image reader surface: %d", status);
        AImageReader_delete(ctx->image_reader);
        free(ctx);
        return NULL;
    }
    
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

uint64_t android_screen_get_frame_count(android_screen_ctx_t *ctx) {
    return ctx ? atomic_load_explicit(&ctx->frame_count, memory_order_relaxed) : 0u;
}
