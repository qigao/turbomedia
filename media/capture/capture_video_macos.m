/**
 * macOS Video Capture Implementation
 *
 * Uses AVFoundation for camera capture
 */
#import "turbo_capture.h"

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

int turbo_capture_list_video_modes(const char *device_id,
                                   turbo_video_capture_mode_t *modes,
                                   int max_count) {
    (void)device_id;
    (void)modes;
    (void)max_count;
    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

/* =============================================================================
 * Video Capture Implementation
 * ============================================================================= */

turbo_capture_t *turbo_video_capture_create(const char *device_id,
                                             const turbo_video_capture_config_t *config) {
    @autoreleasepool {
        turbo_capture_t *capture = (turbo_capture_t *)calloc(1, sizeof(turbo_capture_t));
        if (!capture) return NULL;

        avf_video_ctx_t *ctx = (avf_video_ctx_t *)calloc(1, sizeof(avf_video_ctx_t));
        if (!ctx) {
            free(capture);
            return NULL;
        }

        capture->type = TURBO_CAPTURE_TYPE_VIDEO;
        capture->state = TURBO_CAPTURE_STATE_STOPPED;
        capture->platform_ctx = ctx;

        ctx->width = config ? config->width : 640;
        ctx->height = config ? config->height : 480;
        ctx->framerate = config ? config->framerate : 30;

        /* Find device */
        AVCaptureDevice *device = nil;

        if (device_id && strlen(device_id) > 0) {
            NSString *uniqueID = [NSString stringWithUTF8String:device_id];
            device = [AVCaptureDevice deviceWithUniqueID:uniqueID];
        }

        if (!device) {
            device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        }

        if (!device) {
            free(ctx);
            free(capture);
            return NULL;
        }

        /* Create session */
        ctx->session = [[AVCaptureSession alloc] init];
        if (!ctx->session) {
            free(ctx);
            free(capture);
            return NULL;
        }

        /* Configure session preset based on resolution */
        if (ctx->width >= 1920) {
            ctx->session.sessionPreset = AVCaptureSessionPreset1920x1080;
        } else if (ctx->width >= 1280) {
            ctx->session.sessionPreset = AVCaptureSessionPreset1280x720;
        } else if (ctx->width >= 640) {
            ctx->session.sessionPreset = AVCaptureSessionPreset640x480;
        } else {
            ctx->session.sessionPreset = AVCaptureSessionPreset352x288;
        }

        /* Create input */
        NSError *error = nil;
        ctx->input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
        if (!ctx->input || error) {
            ctx->session = nil;
            free(ctx);
            free(capture);
            return NULL;
        }

        if (![ctx->session canAddInput:ctx->input]) {
            ctx->session = nil;
            free(ctx);
            free(capture);
            return NULL;
        }
        [ctx->session addInput:ctx->input];

        /* Configure framerate */
        if ([device lockForConfiguration:&error]) {
            CMTime frameDuration = CMTimeMake(1, ctx->framerate);
            device.activeVideoMinFrameDuration = frameDuration;
            device.activeVideoMaxFrameDuration = frameDuration;
            [device unlockForConfiguration];
        }

        /* Create output */
        ctx->output = [[AVCaptureVideoDataOutput alloc] init];
        ctx->output.videoSettings = @{
            (NSString *)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA)
        };
        ctx->output.alwaysDiscardsLateVideoFrames = YES;

        /* Create delegate */
        ctx->delegate = [[TurboVideoCaptureDelegate alloc] init];
        ctx->delegate.capture = capture;

        /* Create capture queue */
        ctx->captureQueue = dispatch_queue_create("turbo.video.capture", DISPATCH_QUEUE_SERIAL);
        [ctx->output setSampleBufferDelegate:ctx->delegate queue:ctx->captureQueue];

        if (![ctx->session canAddOutput:ctx->output]) {
            ctx->session = nil;
            ctx->output = nil;
            ctx->delegate = nil;
            free(ctx);
            free(capture);
            return NULL;
        }
        [ctx->session addOutput:ctx->output];

        return capture;
    }
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
