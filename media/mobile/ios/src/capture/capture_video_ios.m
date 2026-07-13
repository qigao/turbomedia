/**
 * iOS Video Capture Implementation
 *
 * Uses AVCaptureSession and emits contiguous NV12 frames through
 * turbo_video_capture_cb.
 */

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include "turbo_capture.h"
#include <stdlib.h>
#include <string.h>

@interface TurboVideoCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, copy) void (^frameCallback)(CVPixelBufferRef);
@end

@implementation TurboVideoCaptureDelegate

- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection *)connection {
    (void)output;
    (void)connection;

    CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (pixelBuffer && self.frameCallback) {
        self.frameCallback(pixelBuffer);
    }
}

@end

typedef struct {
    turbo_capture_t base;
    AVCaptureSession *session;
    AVCaptureDevice *device;
    AVCaptureDeviceInput *input;
    AVCaptureVideoDataOutput *output;
    TurboVideoCaptureDelegate *delegate;
    dispatch_queue_t queue;
    uint8_t *frame_buffer;
    size_t frame_buffer_size;
    int width;
    int height;
    int fps;
} ios_video_capture_t;

static uint64_t now_us(void) {
    return (uint64_t)([[NSDate date] timeIntervalSince1970] * 1000000.0);
}

static uint8_t *copy_nv12_frame(ios_video_capture_t *cap,
                               CVPixelBufferRef pixel_buffer,
                               int *width,
                               int *height,
                               size_t *len) {
    const size_t w = CVPixelBufferGetWidth(pixel_buffer);
    const size_t h = CVPixelBufferGetHeight(pixel_buffer);
    const size_t y_size = w * h;
    const size_t uv_size = y_size / 2;
    const size_t needed = y_size + uv_size;

    if (cap->frame_buffer_size < needed) {
        uint8_t *new_buffer = realloc(cap->frame_buffer, needed);
        if (!new_buffer) return NULL;
        cap->frame_buffer = new_buffer;
        cap->frame_buffer_size = needed;
    }

    uint8_t *dst_y = cap->frame_buffer;
    uint8_t *dst_uv = cap->frame_buffer + y_size;

    const uint8_t *src_y = CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0);
    const uint8_t *src_uv = CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1);
    const size_t stride_y = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0);
    const size_t stride_uv = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1);

    for (size_t row = 0; row < h; ++row) {
        memcpy(dst_y + row * w, src_y + row * stride_y, w);
    }

    for (size_t row = 0; row < h / 2; ++row) {
        memcpy(dst_uv + row * w, src_uv + row * stride_uv, w);
    }

    *width = (int)w;
    *height = (int)h;
    *len = needed;
    return cap->frame_buffer;
}

int ios_video_start(turbo_capture_t *capture) {
    ios_video_capture_t *cap = (ios_video_capture_t *)capture;

    @autoreleasepool {
        [cap->session startRunning];
        NSLog(@"[VideoCapture] Started: %dx%d @ %d fps", cap->width, cap->height, cap->fps);
        return 0;
    }
}

void ios_video_stop(turbo_capture_t *capture) {
    ios_video_capture_t *cap = (ios_video_capture_t *)capture;

    @autoreleasepool {
        [cap->session stopRunning];
        NSLog(@"[VideoCapture] Stopped");
    }
}

void ios_video_destroy(turbo_capture_t *capture) {
    ios_video_capture_t *cap = (ios_video_capture_t *)capture;

    @autoreleasepool {
        if (cap->session) {
            [cap->session stopRunning];
            cap->session = nil;
        }

        free(cap->frame_buffer);
        cap->frame_buffer = NULL;
        cap->frame_buffer_size = 0;
        cap->device = nil;
        cap->input = nil;
        cap->output = nil;
        cap->delegate = nil;
        cap->queue = nil;
        free(cap);
    }
}

turbo_capture_t *turbo_video_capture_create(const char *device_id,
                                            const turbo_video_capture_config_t *config) {
    @autoreleasepool {
        ios_video_capture_t *cap = calloc(1, sizeof(ios_video_capture_t));
        if (!cap) return NULL;

        cap->base.type = TURBO_CAPTURE_TYPE_VIDEO;
        cap->base.state = TURBO_CAPTURE_STATE_STOPPED;
        cap->base.platform_ctx = cap;
        cap->width = (config && config->width > 0) ? config->width : 640;
        cap->height = (config && config->height > 0) ? config->height : 480;
        cap->fps = (config && config->framerate > 0) ? config->framerate : 30;

        if (device_id && strlen(device_id) > 0) {
            NSString *unique_id = [NSString stringWithUTF8String:device_id];
            cap->device = [AVCaptureDevice deviceWithUniqueID:unique_id];
        }

        if (!cap->device) {
            cap->device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        }

        if (!cap->device) {
            NSLog(@"[VideoCapture] No camera found");
            free(cap);
            return NULL;
        }

        cap->session = [[AVCaptureSession alloc] init];
        if (cap->width >= 1920) {
            cap->session.sessionPreset = AVCaptureSessionPreset1920x1080;
        } else if (cap->width >= 1280) {
            cap->session.sessionPreset = AVCaptureSessionPreset1280x720;
        } else if (cap->width >= 640) {
            cap->session.sessionPreset = AVCaptureSessionPreset640x480;
        } else {
            cap->session.sessionPreset = AVCaptureSessionPresetMedium;
        }

        NSError *error = nil;
        cap->input = [AVCaptureDeviceInput deviceInputWithDevice:cap->device error:&error];
        if (error || !cap->input || ![cap->session canAddInput:cap->input]) {
            NSLog(@"[VideoCapture] Failed to create input: %@", error);
            ios_video_destroy((turbo_capture_t *)cap);
            return NULL;
        }
        [cap->session addInput:cap->input];

        cap->output = [[AVCaptureVideoDataOutput alloc] init];
        cap->output.videoSettings = @{
            (NSString *)kCVPixelBufferPixelFormatTypeKey :
                @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
        };

        cap->delegate = [[TurboVideoCaptureDelegate alloc] init];
        cap->queue = dispatch_queue_create("com.turbomedia.video.capture", DISPATCH_QUEUE_SERIAL);
        [cap->output setSampleBufferDelegate:cap->delegate queue:cap->queue];

        if (![cap->session canAddOutput:cap->output]) {
            NSLog(@"[VideoCapture] Cannot add output");
            ios_video_destroy((turbo_capture_t *)cap);
            return NULL;
        }
        [cap->session addOutput:cap->output];

        if ([cap->device lockForConfiguration:&error]) {
            CMTime frame_duration = CMTimeMake(1, cap->fps);
            cap->device.activeVideoMinFrameDuration = frame_duration;
            cap->device.activeVideoMaxFrameDuration = frame_duration;
            [cap->device unlockForConfiguration];
        }

        ios_video_capture_t *cap_ref = cap;
        cap->delegate.frameCallback = ^(CVPixelBufferRef pixel_buffer) {
            if (!cap_ref || !cap_ref->base.video_cb) return;

            CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

            int width = 0;
            int height = 0;
            size_t len = 0;
            uint8_t *frame = copy_nv12_frame(cap_ref, pixel_buffer, &width, &height, &len);
            if (frame) {
                cap_ref->base.video_cb((turbo_capture_t *)cap_ref, frame, len,
                                       width, height, now_us(),
                                       cap_ref->base.user_data);
            }

            CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
        };

        NSLog(@"[VideoCapture] Created: %@ (%dx%d @ %d fps)",
              cap->device.localizedName, cap->width, cap->height, cap->fps);
        return (turbo_capture_t *)cap;
    }
}
