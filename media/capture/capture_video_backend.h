#ifndef TURBO_CAPTURE_VIDEO_BACKEND_H
#define TURBO_CAPTURE_VIDEO_BACKEND_H

#include "turbo_capture.h"

typedef struct {
    int (*open_device)(const char *device_id, void **backend_ctx);
    void (*close_device)(void *backend_ctx);
    int (*list_modes)(void *backend_ctx,
                      turbo_video_native_mode_t *modes,
                      size_t capacity,
                      size_t *out_count);
    int (*create_capture)(void *backend_ctx,
                          const turbo_video_native_mode_t *mode,
                          turbo_capture_t **out_capture);
} turbo_video_backend_ops_t;

const turbo_video_backend_ops_t *turbo_video_platform_backend(void);

#endif
