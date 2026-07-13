/**
 * iOS Screen Capture Implementation
 *
 * Uses ReplayKit and emits contiguous NV12 frames through turbo_video_capture_cb.
 */

#import <Foundation/Foundation.h>
#import <ReplayKit/ReplayKit.h>
#include "turbo_capture.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    turbo_capture_t base;
    RPScreenRecorder *recorder;
    uint8_t *frame_buffer;
    size_t frame_buffer_size;
    int is_running;
} ios_screen_capture_t;

static uint8_t *copy_nv12_frame(ios_screen_capture_t *cap,
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

int ios_screen_start(turbo_capture_t *capture) {
    ios_screen_capture_t *cap = (ios_screen_capture_t *)capture;

    @autoreleasepool {
        if (!cap->recorder.isAvailable) {
            NSLog(@"[ScreenCapture] Screen recording not available");
            return -1;
        }

        ios_screen_capture_t *cap_ref = cap;
        [cap->recorder
            startCaptureWithHandler:^(CMSampleBufferRef sample_buffer,
                                      RPSampleBufferType buffer_type,
                                      NSError *error) {
                if (!cap_ref || !cap_ref->base.video_cb) return;

                if (error) {
                    NSLog(@"[ScreenCapture] Capture error: %@", error);
                    return;
                }

                if (buffer_type != RPSampleBufferTypeVideo) return;

                CVPixelBufferRef pixel_buffer = CMSampleBufferGetImageBuffer(sample_buffer);
                if (!pixel_buffer) return;

                CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);

                int width = 0;
                int height = 0;
                size_t len = 0;
                uint8_t *frame = copy_nv12_frame(cap_ref, pixel_buffer, &width, &height, &len);
                if (frame) {
                    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample_buffer);
                    uint64_t timestamp = (uint64_t)(CMTimeGetSeconds(pts) * 1000000.0);
                    cap_ref->base.video_cb((turbo_capture_t *)cap_ref, frame, len,
                                           width, height, timestamp,
                                           cap_ref->base.user_data);
                }

                CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
            }
            completionHandler:^(NSError *error) {
                if (error) {
                    NSLog(@"[ScreenCapture] Failed to start: %@", error);
                } else {
                    NSLog(@"[ScreenCapture] Started");
                    cap_ref->is_running = 1;
                }
            }];

        return 0;
    }
}

void ios_screen_stop(turbo_capture_t *capture) {
    ios_screen_capture_t *cap = (ios_screen_capture_t *)capture;

    @autoreleasepool {
        [cap->recorder stopCaptureWithHandler:^(NSError *error) {
            if (error) {
                NSLog(@"[ScreenCapture] Failed to stop: %@", error);
            } else {
                NSLog(@"[ScreenCapture] Stopped");
                cap->is_running = 0;
            }
        }];
    }
}

void ios_screen_destroy(turbo_capture_t *capture) {
    ios_screen_capture_t *cap = (ios_screen_capture_t *)capture;

    @autoreleasepool {
        if (cap->is_running) {
            [cap->recorder stopCaptureWithHandler:nil];
        }

        free(cap->frame_buffer);
        cap->frame_buffer = NULL;
        cap->frame_buffer_size = 0;
        cap->recorder = nil;
        free(cap);
    }
}

turbo_capture_t *turbo_screen_capture_create(const turbo_screen_capture_config_t *config) {
    (void)config;

    @autoreleasepool {
        ios_screen_capture_t *cap = calloc(1, sizeof(ios_screen_capture_t));
        if (!cap) return NULL;

        cap->base.type = TURBO_CAPTURE_TYPE_SCREEN;
        cap->base.state = TURBO_CAPTURE_STATE_STOPPED;
        cap->base.platform_ctx = cap;
        cap->recorder = [RPScreenRecorder sharedRecorder];

        if (!cap->recorder.isAvailable) {
            NSLog(@"[ScreenCapture] Screen recording not available on this device");
            free(cap);
            return NULL;
        }

        NSLog(@"[ScreenCapture] Created");
        return (turbo_capture_t *)cap;
    }
}
