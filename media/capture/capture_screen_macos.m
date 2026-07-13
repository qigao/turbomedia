/**
 * macOS Screen Capture Implementation
 *
 * Uses CGDisplayStream (macOS 10.8+) for screen capture
 * Note: ScreenCaptureKit requires macOS 12.3+ and is not used for broader compatibility
 */
#import "turbo_capture.h"

#if defined(__APPLE__) && defined(__MACH__)

#import <CoreGraphics/CoreGraphics.h>
#import <CoreFoundation/CoreFoundation.h>
#import <IOSurface/IOSurface.h>
#import <mach/mach_time.h>
#import <stdlib.h>
#import <string.h>
#import <dispatch/dispatch.h>

/* =============================================================================
 * Helper
 * ============================================================================= */

static uint64_t get_timestamp_us(void) {
    static mach_timebase_info_data_t timebase = {0};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    uint64_t time = mach_absolute_time();
    return (time * timebase.numer / timebase.denom) / 1000;
}

/* =============================================================================
 * Context Structure
 * ============================================================================= */

typedef struct {
    CGDisplayStreamRef stream;
    dispatch_queue_t captureQueue;
    CGDirectDisplayID displayID;

    volatile int running;

    int monitor_index;
    int framerate;
    int capture_cursor;
    int width;
    int height;

    uint8_t *frame_buffer;
    size_t frame_buffer_size;

    turbo_capture_t *capture;
} cgdisplay_ctx_t;

/* =============================================================================
 * Screen Enumeration
 * ============================================================================= */

int turbo_capture_list_screens(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    CGDirectDisplayID displayIDs[16];
    uint32_t displayCount = 0;

    CGError err = CGGetActiveDisplayList(16, displayIDs, &displayCount);
    if (err != kCGErrorSuccess) {
        return -1;
    }

    int count = 0;
    CGDirectDisplayID mainDisplay = CGMainDisplayID();

    for (uint32_t i = 0; i < displayCount && count < max_count; i++) {
        CGDirectDisplayID displayID = displayIDs[i];

        turbo_capture_device_t *dev = &devices[count];
        memset(dev, 0, sizeof(*dev));

        dev->index = count;
        dev->type = TURBO_CAPTURE_TYPE_SCREEN;
        dev->is_default = (displayID == mainDisplay) ? 1 : 0;

        size_t width = CGDisplayPixelsWide(displayID);
        size_t height = CGDisplayPixelsHigh(displayID);

        snprintf(dev->name, sizeof(dev->name), "Display %d (%zux%zu)",
                 count, width, height);
        snprintf(dev->id, sizeof(dev->id), "%u", displayID);

        count++;
    }

    return count;
}

/* =============================================================================
 * Screen Capture Implementation
 * ============================================================================= */

turbo_capture_t *turbo_screen_capture_create(const turbo_screen_capture_config_t *config) {
    turbo_capture_t *capture = (turbo_capture_t *)calloc(1, sizeof(turbo_capture_t));
    if (!capture) return NULL;

    cgdisplay_ctx_t *ctx = (cgdisplay_ctx_t *)calloc(1, sizeof(cgdisplay_ctx_t));
    if (!ctx) {
        free(capture);
        return NULL;
    }

    capture->type = TURBO_CAPTURE_TYPE_SCREEN;
    capture->state = TURBO_CAPTURE_STATE_STOPPED;
    capture->platform_ctx = ctx;
    ctx->capture = capture;

    ctx->monitor_index = config ? config->monitor_index : -1;
    ctx->framerate = config ? config->framerate : 30;
    ctx->capture_cursor = config ? config->capture_cursor : 1;

    /* Get display ID */
    if (ctx->monitor_index < 0) {
        ctx->displayID = CGMainDisplayID();
    } else {
        CGDirectDisplayID displayIDs[16];
        uint32_t displayCount = 0;
        CGGetActiveDisplayList(16, displayIDs, &displayCount);

        if (ctx->monitor_index < (int)displayCount) {
            ctx->displayID = displayIDs[ctx->monitor_index];
        } else {
            ctx->displayID = CGMainDisplayID();
        }
    }

    ctx->width = (int)CGDisplayPixelsWide(ctx->displayID);
    ctx->height = (int)CGDisplayPixelsHigh(ctx->displayID);

    /* Allocate frame buffer (BGRA) */
    ctx->frame_buffer_size = ctx->width * ctx->height * 4;
    ctx->frame_buffer = (uint8_t *)malloc(ctx->frame_buffer_size);
    if (!ctx->frame_buffer) {
        free(ctx);
        free(capture);
        return NULL;
    }

    /* Create dispatch queue */
    ctx->captureQueue = dispatch_queue_create("turbo.screen.capture", DISPATCH_QUEUE_SERIAL);

    return capture;
}

void turbo_screen_capture_set_callback(turbo_capture_t *capture,
                                        turbo_video_capture_cb cb,
                                        void *user_data) {
    if (!capture) return;
    capture->video_cb = cb;
    capture->user_data = user_data;
}

/* =============================================================================
 * Platform Hooks
 * ============================================================================= */

int macos_screen_start(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return -1;
    cgdisplay_ctx_t *ctx = (cgdisplay_ctx_t *)capture->platform_ctx;

    /* Calculate minimum interval based on framerate */
    CFTimeInterval minInterval = 1.0 / ctx->framerate;

    /* Properties */
    CFMutableDictionaryRef properties = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    /* Show cursor */
    CFDictionarySetValue(properties, kCGDisplayStreamShowCursor,
                         ctx->capture_cursor ? kCFBooleanTrue : kCFBooleanFalse);

    /* Minimum frame interval */
    CFNumberRef intervalRef = CFNumberCreate(kCFAllocatorDefault,
                                              kCFNumberDoubleType, &minInterval);
    CFDictionarySetValue(properties, kCGDisplayStreamMinimumFrameTime, intervalRef);
    CFRelease(intervalRef);

    /* Create stream */
    __block cgdisplay_ctx_t *blockCtx = ctx;

    ctx->stream = CGDisplayStreamCreateWithDispatchQueue(
        ctx->displayID,
        ctx->width,
        ctx->height,
        'BGRA',  /* kCVPixelFormatType_32BGRA */
        (__bridge CFDictionaryRef)properties,
        ctx->captureQueue,
        ^(CGDisplayStreamFrameStatus status,
          uint64_t displayTime,
          IOSurfaceRef frameSurface,
          CGDisplayStreamUpdateRef updateRef) {
            (void)displayTime;
            (void)updateRef;

            if (!blockCtx->running) return;

            if (status != kCGDisplayStreamFrameStatusFrameComplete) {
                return;
            }

            if (!frameSurface) return;

            turbo_capture_t *cap = blockCtx->capture;
            if (!cap || !cap->video_cb) return;

            /* Lock IOSurface for reading */
            IOSurfaceLock(frameSurface, kIOSurfaceLockReadOnly, NULL);

            void *baseAddress = IOSurfaceGetBaseAddress(frameSurface);
            size_t bytesPerRow = IOSurfaceGetBytesPerRow(frameSurface);
            size_t height = IOSurfaceGetHeight(frameSurface);
            size_t width = IOSurfaceGetWidth(frameSurface);

            size_t expectedBytesPerRow = width * 4;
            size_t frameSize = width * height * 4;

            uint8_t *frameData;
            if (bytesPerRow == expectedBytesPerRow) {
                frameData = (uint8_t *)baseAddress;
            } else {
                /* Handle stride mismatch */
                uint8_t *src = (uint8_t *)baseAddress;
                uint8_t *dst = blockCtx->frame_buffer;
                for (size_t y = 0; y < height; y++) {
                    memcpy(dst, src, expectedBytesPerRow);
                    src += bytesPerRow;
                    dst += expectedBytesPerRow;
                }
                frameData = blockCtx->frame_buffer;
            }

            uint64_t timestamp = get_timestamp_us();

            cap->video_cb(cap, frameData, frameSize,
                          (int)width, (int)height,
                          timestamp, cap->user_data);

            IOSurfaceUnlock(frameSurface, kIOSurfaceLockReadOnly, NULL);
        });

    CFRelease(properties);

    if (!ctx->stream) {
        return -1;
    }

    ctx->running = 1;
    CGError err = CGDisplayStreamStart(ctx->stream);
    if (err != kCGErrorSuccess) {
        CFRelease(ctx->stream);
        ctx->stream = NULL;
        ctx->running = 0;
        return -1;
    }

    return 0;
}

void macos_screen_stop(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return;
    cgdisplay_ctx_t *ctx = (cgdisplay_ctx_t *)capture->platform_ctx;

    ctx->running = 0;

    if (ctx->stream) {
        CGDisplayStreamStop(ctx->stream);
        CFRelease(ctx->stream);
        ctx->stream = NULL;
    }
}

void macos_screen_destroy(turbo_capture_t *capture) {
    if (!capture) return;

    macos_screen_stop(capture);

    cgdisplay_ctx_t *ctx = (cgdisplay_ctx_t *)capture->platform_ctx;
    if (ctx) {
        if (ctx->frame_buffer) {
            free(ctx->frame_buffer);
        }
        if (ctx->captureQueue) {
            ctx->captureQueue = nil;
        }
        free(ctx);
    }

    free(capture);
}

#endif /* __APPLE__ && __MACH__ */
