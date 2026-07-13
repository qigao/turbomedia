/**
 * iOS Capture Dispatcher
 *
 * Adapts the staged iOS implementations to the current TurboMedia capture API.
 */

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include "turbo_capture.h"
#include <stdlib.h>
#include <string.h>

extern int ios_audio_start(turbo_capture_t *capture);
extern void ios_audio_stop(turbo_capture_t *capture);
extern void ios_audio_destroy(turbo_capture_t *capture);

extern int ios_video_start(turbo_capture_t *capture);
extern void ios_video_stop(turbo_capture_t *capture);
extern void ios_video_destroy(turbo_capture_t *capture);

extern int ios_screen_start(turbo_capture_t *capture);
extern void ios_screen_stop(turbo_capture_t *capture);
extern void ios_screen_destroy(turbo_capture_t *capture);

static void copy_nsstring(char *dst, size_t dst_size, NSString *value) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    const char *utf8 = value ? [value UTF8String] : NULL;
    if (utf8) {
        strncpy(dst, utf8, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

int turbo_capture_list_video_devices(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    @autoreleasepool {
        AVCaptureDeviceDiscoverySession *session =
            [AVCaptureDeviceDiscoverySession
                discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeBuiltInWideAngleCamera,
                                                   AVCaptureDeviceTypeExternalUnknown ]
                                      mediaType:AVMediaTypeVideo
                                       position:AVCaptureDevicePositionUnspecified];

        int count = 0;
        for (AVCaptureDevice *device in session.devices) {
            if (count >= max_count) break;
            turbo_capture_device_t *out = &devices[count];
            memset(out, 0, sizeof(*out));
            out->index = count;
            out->type = TURBO_CAPTURE_TYPE_VIDEO;
            out->is_default = (count == 0) ? 1 : 0;
            copy_nsstring(out->id, sizeof(out->id), device.uniqueID);
            copy_nsstring(out->name, sizeof(out->name), device.localizedName);
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

int turbo_capture_list_audio_devices(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    @autoreleasepool {
        NSArray<AVCaptureDevice *> *audio_devices =
            [AVCaptureDevice devicesWithMediaType:AVMediaTypeAudio];

        int count = 0;
        for (AVCaptureDevice *device in audio_devices) {
            if (count >= max_count) break;
            turbo_capture_device_t *out = &devices[count];
            memset(out, 0, sizeof(*out));
            out->index = count;
            out->type = TURBO_CAPTURE_TYPE_AUDIO;
            out->is_default = (count == 0) ? 1 : 0;
            copy_nsstring(out->id, sizeof(out->id), device.uniqueID);
            copy_nsstring(out->name, sizeof(out->name), device.localizedName);
            count++;
        }
        return count;
    }
}

int turbo_capture_list_screens(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    memset(&devices[0], 0, sizeof(devices[0]));
    devices[0].index = 0;
    devices[0].type = TURBO_CAPTURE_TYPE_SCREEN;
    devices[0].is_default = 1;
    strncpy(devices[0].id, "screen:0", sizeof(devices[0].id) - 1);
    strncpy(devices[0].name, "Main Screen", sizeof(devices[0].name) - 1);
    return 1;
}

int turbo_capture_start(turbo_capture_t *capture) {
    if (!capture) return -1;
    if (capture->state == TURBO_CAPTURE_STATE_RUNNING) return 0;

    capture->state = TURBO_CAPTURE_STATE_STARTING;

    int result = -1;
    switch (capture->type) {
        case TURBO_CAPTURE_TYPE_AUDIO:
            result = ios_audio_start(capture);
            break;
        case TURBO_CAPTURE_TYPE_VIDEO:
            result = ios_video_start(capture);
            break;
        case TURBO_CAPTURE_TYPE_SCREEN:
            result = ios_screen_start(capture);
            break;
        default:
            result = -1;
            break;
    }

    capture->state = (result == 0) ? TURBO_CAPTURE_STATE_RUNNING : TURBO_CAPTURE_STATE_ERROR;
    if (capture->state_cb) {
        capture->state_cb(capture, capture->state, capture->user_data);
    }
    return result;
}

void turbo_capture_stop(turbo_capture_t *capture) {
    if (!capture || capture->state == TURBO_CAPTURE_STATE_STOPPED) return;

    capture->state = TURBO_CAPTURE_STATE_STOPPING;

    switch (capture->type) {
        case TURBO_CAPTURE_TYPE_AUDIO:
            ios_audio_stop(capture);
            break;
        case TURBO_CAPTURE_TYPE_VIDEO:
            ios_video_stop(capture);
            break;
        case TURBO_CAPTURE_TYPE_SCREEN:
            ios_screen_stop(capture);
            break;
        default:
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
            ios_audio_destroy(capture);
            break;
        case TURBO_CAPTURE_TYPE_VIDEO:
            ios_video_destroy(capture);
            break;
        case TURBO_CAPTURE_TYPE_SCREEN:
            ios_screen_destroy(capture);
            break;
        default:
            free(capture);
            break;
    }
}

turbo_capture_state_t turbo_capture_get_state(turbo_capture_t *capture) {
    return capture ? capture->state : TURBO_CAPTURE_STATE_STOPPED;
}

void turbo_capture_on_state(turbo_capture_t *capture, turbo_capture_state_cb cb) {
    if (capture) capture->state_cb = cb;
}

void turbo_audio_capture_set_callback(turbo_capture_t *capture,
                                      turbo_audio_capture_cb cb,
                                      void *user_data) {
    if (!capture || capture->type != TURBO_CAPTURE_TYPE_AUDIO) return;
    capture->audio_cb = cb;
    capture->user_data = user_data;
}

void turbo_video_capture_set_callback(turbo_capture_t *capture,
                                      turbo_video_capture_cb cb,
                                      void *user_data) {
    if (!capture || capture->type != TURBO_CAPTURE_TYPE_VIDEO) return;
    capture->video_cb = cb;
    capture->user_data = user_data;
}

void turbo_screen_capture_set_callback(turbo_capture_t *capture,
                                       turbo_video_capture_cb cb,
                                       void *user_data) {
    if (!capture || capture->type != TURBO_CAPTURE_TYPE_SCREEN) return;
    capture->video_cb = cb;
    capture->user_data = user_data;
}
