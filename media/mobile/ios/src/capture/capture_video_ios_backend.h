#ifndef TURBO_CAPTURE_VIDEO_IOS_BACKEND_H
#define TURBO_CAPTURE_VIDEO_IOS_BACKEND_H

#import <AVFoundation/AVFoundation.h>
#include "capture_video_backend.h"

typedef struct {
    char device_id[128];
} ios_video_device_ctx_t;

AVCaptureDevice *ios_video_find_device(const char *device_id);
int ios_video_make_mode(AVCaptureDevice *device,
                        uint32_t format_index,
                        uint32_t range_index,
                        uint32_t endpoint,
                        turbo_video_native_mode_t *mode,
                        AVCaptureDeviceFormat **out_format,
                        CMTime *out_duration);
int ios_video_modes_equal(const turbo_video_native_mode_t *lhs,
                          const turbo_video_native_mode_t *rhs);
int ios_video_device_create_capture(void *backend_ctx,
                                    const turbo_video_native_mode_t *mode,
                                    turbo_capture_t **out_capture);

#endif
