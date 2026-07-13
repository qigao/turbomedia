/**
 * Linux Capture Dispatcher
 *
 * Centralizes capture start/stop/destroy for all types on Linux
 */
#include "turbo_capture.h"

#if defined(__linux__) && !defined(__ANDROID__)

#include <stdlib.h>

/* Internal hooks from specific implementations */
extern int linux_audio_start(turbo_capture_t *capture);
extern void linux_audio_stop(turbo_capture_t *capture);
extern void linux_audio_destroy(turbo_capture_t *capture);

extern int v4l2_video_start(turbo_capture_t *capture);
extern void v4l2_video_stop(turbo_capture_t *capture);
extern void v4l2_video_destroy(turbo_capture_t *capture);

extern int linux_screen_start(turbo_capture_t *capture);
extern void linux_screen_stop(turbo_capture_t *capture);
extern void linux_screen_destroy(turbo_capture_t *capture);

int turbo_capture_start(turbo_capture_t *capture) {
    if (!capture) return -1;
    if (capture->state == TURBO_CAPTURE_STATE_RUNNING) return 0;

    capture->state = TURBO_CAPTURE_STATE_STARTING;

    int res = -1;
    switch (capture->type) {
        case TURBO_CAPTURE_TYPE_AUDIO:
            res = linux_audio_start(capture);
            break;
        case TURBO_CAPTURE_TYPE_VIDEO:
            res = v4l2_video_start(capture);
            break;
        case TURBO_CAPTURE_TYPE_SCREEN:
            res = linux_screen_start(capture);
            break;
        default:
            return -1;
    }

    if (res == 0) {
        capture->state = TURBO_CAPTURE_STATE_RUNNING;
        if (capture->state_cb) {
            capture->state_cb(capture, capture->state, capture->user_data);
        }
    } else {
        capture->state = TURBO_CAPTURE_STATE_ERROR;
    }

    return res;
}

void turbo_capture_stop(turbo_capture_t *capture) {
    if (!capture || capture->state == TURBO_CAPTURE_STATE_STOPPED) return;

    capture->state = TURBO_CAPTURE_STATE_STOPPING;

    switch (capture->type) {
        case TURBO_CAPTURE_TYPE_AUDIO:
            linux_audio_stop(capture);
            break;
        case TURBO_CAPTURE_TYPE_VIDEO:
            v4l2_video_stop(capture);
            break;
        case TURBO_CAPTURE_TYPE_SCREEN:
            linux_screen_stop(capture);
            break;
    }

    capture->state = TURBO_CAPTURE_STATE_STOPPED;
    if (capture->state_cb) {
        capture->state_cb(capture, capture->state, capture->user_data);
    }
}

void turbo_capture_destroy(turbo_capture_t *capture) {
    if (!capture) return;

    switch (capture->type) {
        case TURBO_CAPTURE_TYPE_AUDIO:
            linux_audio_destroy(capture);
            break;
        case TURBO_CAPTURE_TYPE_VIDEO:
            v4l2_video_destroy(capture);
            break;
        case TURBO_CAPTURE_TYPE_SCREEN:
            linux_screen_destroy(capture);
            break;
    }
}

turbo_capture_state_t turbo_capture_get_state(turbo_capture_t *capture) {
    return capture ? capture->state : TURBO_CAPTURE_STATE_STOPPED;
}

void turbo_capture_on_state(turbo_capture_t *capture, turbo_capture_state_cb cb) {
    if (capture) capture->state_cb = cb;
}

int turbo_capture_list_gpu_devices(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;
    return 0;
}

#endif /* __linux__ && !__ANDROID__ */
