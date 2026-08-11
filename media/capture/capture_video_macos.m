/**
 * macOS Video Capture Implementation
 *
 * Uses AVFoundation for camera capture
 */
#import "turbo_capture.h"
#import "capture_video_backend.h"

#if defined(__APPLE__) && defined(__MACH__)

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <mach/mach_time.h>
#import <stdlib.h>
#import <string.h>

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
 * Capture Delegate
 * ============================================================================= */

@interface TurboVideoCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, assign) turbo_capture_t *capture;
@property (nonatomic, assign) int width;
@property (nonatomic, assign) int height;
@property (nonatomic, assign) uint8_t *convertBuffer;
@property (nonatomic, assign) size_t convertBufferSize;
@end

@implementation TurboVideoCaptureDelegate

- (void)captureOutput:(AVCaptureOutput *)output
        didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        fromConnection:(AVCaptureConnection *)connection {
    (void)output;
    (void)connection;

    if (!self.capture || !self.capture->video_cb) return;

    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) return;

    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);

    size_t width = CVPixelBufferGetWidth(imageBuffer);
    size_t height = CVPixelBufferGetHeight(imageBuffer);
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);
    void *baseAddress = CVPixelBufferGetBaseAddress(imageBuffer);

    self.width = (int)width;
    self.height = (int)height;

    /* Handle stride mismatch - copy to contiguous buffer */
    size_t expectedBytesPerRow = width * 4;  /* BGRA */
    size_t frameSize = width * height * 4;

    uint8_t *frameData;
    if (bytesPerRow == expectedBytesPerRow) {
        frameData = (uint8_t *)baseAddress;
    } else {
        /* Need to copy with stride handling */
        if (self.convertBufferSize < frameSize) {
            self.convertBuffer = realloc(self.convertBuffer, frameSize);
            self.convertBufferSize = frameSize;
        }
        uint8_t *src = (uint8_t *)baseAddress;
        uint8_t *dst = self.convertBuffer;
        for (size_t y = 0; y < height; y++) {
            memcpy(dst, src, expectedBytesPerRow);
            src += bytesPerRow;
            dst += expectedBytesPerRow;
        }
        frameData = self.convertBuffer;
    }

    uint64_t timestamp = get_timestamp_us();

    self.capture->video_cb(self.capture, frameData, frameSize,
                           (int)width, (int)height,
                           timestamp, self.capture->user_data);

    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

- (void)dealloc {
    if (self.convertBuffer) {
        free(self.convertBuffer);
    }
}

@end

/* =============================================================================
 * Context Structure
 * ============================================================================= */

typedef struct {
    AVCaptureSession *session;
    AVCaptureDeviceInput *input;
    AVCaptureVideoDataOutput *output;
    TurboVideoCaptureDelegate *delegate;
    dispatch_queue_t captureQueue;

    int width;
    int height;
    int framerate;
} avf_video_ctx_t;

typedef struct {
    char device_id[128];
} avf_video_device_ctx_t;

void avfoundation_video_destroy(turbo_capture_t *capture);

static AVCaptureDevice *avf_find_video_device(const char *device_id) {
    if (device_id && device_id[0]) {
        NSString *uniqueID = [NSString stringWithUTF8String:device_id];
        return uniqueID ? [AVCaptureDevice deviceWithUniqueID:uniqueID] : nil;
    }
    return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
}

static uint64_t avf_mode_id(uint32_t format_index,
                            uint32_t range_index,
                            uint32_t endpoint) {
    return ((uint64_t)format_index << 32) |
           ((uint64_t)range_index << 1) |
           endpoint;
}

static int avf_make_mode(AVCaptureDevice *device,
                         uint32_t format_index,
                         uint32_t range_index,
                         uint32_t endpoint,
                         turbo_video_native_mode_t *mode,
                         AVCaptureDeviceFormat **out_format,
                         CMTime *out_duration) {
    if (!device || !mode || endpoint > 1 ||
        format_index >= (uint32_t)device.formats.count) {
        return TURBO_CAPTURE_ERR_FORMAT;
    }

    AVCaptureDeviceFormat *format = device.formats[format_index];
    NSArray<AVFrameRateRange *> *ranges = format.videoSupportedFrameRateRanges;
    if (range_index >= (uint32_t)ranges.count) return TURBO_CAPTURE_ERR_FORMAT;

    AVFrameRateRange *range = ranges[range_index];
    CMTime duration = endpoint == 0 ? range.minFrameDuration
                                    : range.maxFrameDuration;
    CMVideoDimensions dimensions =
        CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    if (!CMTIME_IS_NUMERIC(duration) || duration.value <= 0 ||
        duration.timescale <= 0 ||
        (uint64_t)duration.value > UINT32_MAX ||
        (uint64_t)duration.timescale > UINT32_MAX ||
        dimensions.width <= 0 || dimensions.height <= 0) {
        return TURBO_CAPTURE_ERR_FORMAT;
    }

    mode->width = dimensions.width;
    mode->height = dimensions.height;
    mode->framerate_numerator = (uint32_t)duration.timescale;
    mode->framerate_denominator = (uint32_t)duration.value;
    mode->format = TURBO_VIDEO_CAPTURE_FORMAT_BGRA;
    mode->mode_id = avf_mode_id(format_index, range_index, endpoint);
    if (out_format) *out_format = format;
    if (out_duration) *out_duration = duration;
    return TURBO_CAPTURE_OK;
}

static int avf_modes_equal(const turbo_video_native_mode_t *lhs,
                           const turbo_video_native_mode_t *rhs) {
    return lhs->width == rhs->width && lhs->height == rhs->height &&
           lhs->framerate_numerator == rhs->framerate_numerator &&
           lhs->framerate_denominator == rhs->framerate_denominator &&
           lhs->format == rhs->format && lhs->mode_id == rhs->mode_id;
}

/* =============================================================================
 * Device Enumeration
 * ============================================================================= */

int turbo_capture_list_video_devices(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    @autoreleasepool {
        int count = 0;

        AVCaptureDeviceDiscoverySession *discoverySession =
            [AVCaptureDeviceDiscoverySession
                discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera,
                                                   AVCaptureDeviceTypeExternalUnknown]
                mediaType:AVMediaTypeVideo
                position:AVCaptureDevicePositionUnspecified];

        NSArray<AVCaptureDevice *> *videoDevices = discoverySession.devices;

        for (AVCaptureDevice *device in videoDevices) {
            if (count >= max_count) break;

            turbo_capture_device_t *dev = &devices[count];
            memset(dev, 0, sizeof(*dev));

            dev->index = count;
            dev->type = TURBO_CAPTURE_TYPE_VIDEO;
            dev->is_default = (count == 0) ? 1 : 0;

            const char *name = [device.localizedName UTF8String];
            const char *uniqueID = [device.uniqueID UTF8String];

            if (name) {
                strncpy(dev->name, name, sizeof(dev->name) - 1);
            }
            if (uniqueID) {
                strncpy(dev->id, uniqueID, sizeof(dev->id) - 1);
            }

            count++;
        }

        return count;
    }
}

static int avf_video_device_open(const char *device_id, void **backend_ctx) {
    avf_video_device_ctx_t *ctx;

    if (!backend_ctx) return TURBO_CAPTURE_ERR_FORMAT;
    *backend_ctx = NULL;
    @autoreleasepool {
        AVCaptureDevice *device = avf_find_video_device(device_id);
        const char *unique_id;
        if (!device) return TURBO_CAPTURE_ERR_DEVICE;
        unique_id = [device.uniqueID UTF8String];
        if (!unique_id || strlen(unique_id) >= sizeof(ctx->device_id)) {
            return TURBO_CAPTURE_ERR_FORMAT;
        }

        ctx = (avf_video_device_ctx_t *)calloc(1, sizeof(*ctx));
        if (!ctx) return TURBO_CAPTURE_ERR_NOMEM;
        memcpy(ctx->device_id, unique_id, strlen(unique_id) + 1);
        *backend_ctx = ctx;
        return TURBO_CAPTURE_OK;
    }
}

static void avf_video_device_close(void *backend_ctx) {
    free(backend_ctx);
}

static int avf_video_device_list_modes(
    void *backend_ctx,
    turbo_video_native_mode_t *modes,
    size_t capacity,
    size_t *out_count) {
    avf_video_device_ctx_t *ctx = (avf_video_device_ctx_t *)backend_ctx;
    size_t count = 0;

    if (!ctx || !modes || capacity == 0 || !out_count) {
        return TURBO_CAPTURE_ERR_FORMAT;
    }
    memset(modes, 0, sizeof(*modes) * capacity);

    @autoreleasepool {
        AVCaptureDevice *device = avf_find_video_device(ctx->device_id);
        if (!device) return TURBO_CAPTURE_ERR_DEVICE;

        for (uint32_t format_index = 0;
             format_index < (uint32_t)device.formats.count;
             ++format_index) {
            AVCaptureDeviceFormat *format = device.formats[format_index];
            NSArray<AVFrameRateRange *> *ranges =
                format.videoSupportedFrameRateRanges;
            for (uint32_t range_index = 0;
                 range_index < (uint32_t)ranges.count;
                 ++range_index) {
                AVFrameRateRange *range = ranges[range_index];
                if (avf_make_mode(device, format_index, range_index, 0,
                                  &modes[count], NULL, NULL) ==
                    TURBO_CAPTURE_OK) {
                    if (++count == capacity) goto done;
                }
                if (CMTimeCompare(range.minFrameDuration,
                                  range.maxFrameDuration) != 0 &&
                    avf_make_mode(device, format_index, range_index, 1,
                                  &modes[count], NULL, NULL) ==
                        TURBO_CAPTURE_OK) {
                    if (++count == capacity) goto done;
                }
            }
        }
done:
        *out_count = count;
        return TURBO_CAPTURE_OK;
    }
}

/* =============================================================================
 * Video Capture Implementation
 * ============================================================================= */

static int avf_video_device_create_capture(
    void *backend_ctx,
    const turbo_video_native_mode_t *mode,
    turbo_capture_t **out_capture) {
    avf_video_device_ctx_t *device_ctx =
        (avf_video_device_ctx_t *)backend_ctx;
    uint32_t format_index = (uint32_t)(mode->mode_id >> 32);
    uint32_t range_index =
        (uint32_t)(mode->mode_id & UINT32_MAX) >> 1;
    uint32_t endpoint = (uint32_t)(mode->mode_id & 1u);

    if (!device_ctx || !mode || !out_capture) return TURBO_CAPTURE_ERR_FORMAT;
    *out_capture = NULL;

    @autoreleasepool {
        AVCaptureDevice *device = avf_find_video_device(device_ctx->device_id);
        turbo_video_native_mode_t actual_mode;
        AVCaptureDeviceFormat *selected_format = nil;
        CMTime duration = kCMTimeInvalid;
        NSError *error = nil;
        turbo_capture_t *capture;
        avf_video_ctx_t *ctx;

        if (!device ||
            avf_make_mode(device, format_index, range_index, endpoint,
                          &actual_mode, &selected_format, &duration) !=
                TURBO_CAPTURE_OK ||
            !avf_modes_equal(&actual_mode, mode)) {
            return TURBO_CAPTURE_ERR_FORMAT;
        }

        capture = (turbo_capture_t *)calloc(1, sizeof(*capture));
        ctx = (avf_video_ctx_t *)calloc(1, sizeof(*ctx));
        if (!capture || !ctx) {
            free(capture);
            free(ctx);
            return TURBO_CAPTURE_ERR_NOMEM;
        }
        capture->type = TURBO_CAPTURE_TYPE_VIDEO;
        capture->state = TURBO_CAPTURE_STATE_STOPPED;
        capture->platform_ctx = ctx;
        ctx->width = mode->width;
        ctx->height = mode->height;
        ctx->framerate = (int)(((uint64_t)mode->framerate_numerator +
                                mode->framerate_denominator / 2u) /
                               mode->framerate_denominator);

        if (![device lockForConfiguration:&error]) goto error;
        device.activeFormat = selected_format;
        device.activeVideoMinFrameDuration = duration;
        device.activeVideoMaxFrameDuration = duration;
        [device unlockForConfiguration];

        ctx->session = [[AVCaptureSession alloc] init];
        if (!ctx->session) goto error;
        ctx->input = [AVCaptureDeviceInput deviceInputWithDevice:device
                                                           error:&error];
        if (!ctx->input || error ||
            ![ctx->session canAddInput:ctx->input]) {
            goto error;
        }
        [ctx->session addInput:ctx->input];

        ctx->output = [[AVCaptureVideoDataOutput alloc] init];
        ctx->output.videoSettings = @{
            (NSString *)kCVPixelBufferPixelFormatTypeKey :
                @(kCVPixelFormatType_32BGRA)
        };
        ctx->output.alwaysDiscardsLateVideoFrames = YES;
        ctx->delegate = [[TurboVideoCaptureDelegate alloc] init];
        ctx->delegate.capture = capture;
        ctx->captureQueue = dispatch_queue_create(
            "turbo.video.capture", DISPATCH_QUEUE_SERIAL);
        [ctx->output setSampleBufferDelegate:ctx->delegate
                                       queue:ctx->captureQueue];
        if (![ctx->session canAddOutput:ctx->output]) goto error;
        [ctx->session addOutput:ctx->output];

        *out_capture = capture;
        return TURBO_CAPTURE_OK;

error:
        avfoundation_video_destroy(capture);
        return TURBO_CAPTURE_ERR_DEVICE;
    }
}

const turbo_video_backend_ops_t *turbo_video_platform_backend(void) {
    static const turbo_video_backend_ops_t ops = {
        avf_video_device_open,
        avf_video_device_close,
        avf_video_device_list_modes,
        avf_video_device_create_capture
    };
    return &ops;
}

void turbo_video_capture_set_callback(turbo_capture_t *capture,
                                       turbo_video_capture_cb cb,
                                       void *user_data) {
    if (!capture) return;
    capture->video_cb = cb;
    capture->user_data = user_data;
}

/* =============================================================================
 * Platform Hooks
 * ============================================================================= */

int avfoundation_video_start(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return -1;
    avf_video_ctx_t *ctx = (avf_video_ctx_t *)capture->platform_ctx;

    @autoreleasepool {
        if (!ctx->session.isRunning) {
            [ctx->session startRunning];
        }
        return ctx->session.isRunning ? 0 : -1;
    }
}

void avfoundation_video_stop(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return;
    avf_video_ctx_t *ctx = (avf_video_ctx_t *)capture->platform_ctx;

    @autoreleasepool {
        if (ctx->session.isRunning) {
            [ctx->session stopRunning];
        }
    }
}

void avfoundation_video_destroy(turbo_capture_t *capture) {
    if (!capture) return;

    avfoundation_video_stop(capture);

    avf_video_ctx_t *ctx = (avf_video_ctx_t *)capture->platform_ctx;
    if (ctx) {
        @autoreleasepool {
            if (ctx->session) {
                [ctx->session removeInput:ctx->input];
                [ctx->session removeOutput:ctx->output];
                ctx->session = nil;
            }
            ctx->input = nil;
            ctx->output = nil;
            ctx->delegate = nil;
            ctx->captureQueue = nil;
        }
        free(ctx);
    }

    free(capture);
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

#endif /* __APPLE__ && __MACH__ */
