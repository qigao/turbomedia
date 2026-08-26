#ifndef TURBO_CAPTURE_H
#define TURBO_CAPTURE_H

#include <stdint.h>

typedef enum turbo_video_capture_format_t {
    TURBO_VIDEO_CAPTURE_FORMAT_NV12 = 0
} turbo_video_capture_format_t;

typedef struct turbo_video_native_mode_t {
    int width;
    int height;
    uint32_t framerate_numerator;
    uint32_t framerate_denominator;
    turbo_video_capture_format_t format;
    uint64_t mode_id;
} turbo_video_native_mode_t;

int turbo_video_mode_fps(const turbo_video_native_mode_t *mode);

#endif
