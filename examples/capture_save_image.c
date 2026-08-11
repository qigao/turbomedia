/**
 * Capture to Image Example
 * 
 * Captures a native MJPEG frame from the webcam and saves it as a JPEG file.
 */

#include "turbo_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(x) Sleep(x)
#else
#include <unistd.h>
#define SLEEP_MS(x) usleep((x) * 1000)
#endif

static volatile int image_saved = 0;

static void on_video_frame(turbo_capture_t *capture,
                           const uint8_t *frame, size_t len,
                           int width, int height,
                           uint64_t timestamp, void *user_data) {
    (void)capture;
    (void)len;
    (void)timestamp;
    (void)user_data;

    if (image_saved) return;

    if (len < 2 || frame[0] != 0xFF || frame[1] != 0xD8) return;

    const char *filename = "captured_frame.jpg";
    FILE *file = fopen(filename, "wb");
    if (!file) return;
    size_t written = fwrite(frame, 1, len, file);
    int close_result = fclose(file);
    if (written == len && close_result == 0) {
        printf("Captured MJPEG frame: %dx%d, %zu bytes\n", width, height, len);
        printf("Success! Saved to %s\n", filename);
        image_saved = 1;
    }
}

int main(void) {
    printf("=== Video Capture to Image Example ===\n");

    /* List devices */
    turbo_capture_device_t devices[16];
    int count = turbo_capture_list_video_devices(devices, 16);
    
    if (count <= 0) {
        printf("No video devices found.\n");
        return 1;
    }

    printf("Found %d devices:\n", count);
    for (int i = 0; i < count; i++) {
        printf("  %d: %s (ID: %s)\n", i, devices[i].name, devices[i].id);
    }

    turbo_video_device_t *device = NULL;
    turbo_video_native_mode_t modes[256];
    const turbo_video_native_mode_t *selected_mode = NULL;
    size_t mode_count = 0;
    printf("Opening device: %s\n", devices[0].name);

    if (turbo_video_device_open(devices[0].id, &device) != TURBO_CAPTURE_OK) {
        printf("Failed to open video device.\n");
        return 1;
    }
    if (turbo_video_device_list_modes(device, modes, 256, &mode_count) !=
        TURBO_CAPTURE_OK) {
        printf("Failed to enumerate native video modes.\n");
        turbo_video_device_close(device);
        return 1;
    }
    for (size_t i = 0; i < mode_count; ++i) {
        if (modes[i].format == TURBO_VIDEO_CAPTURE_FORMAT_MJPEG) {
            selected_mode = &modes[i];
            break;
        }
    }
    if (!selected_mode) {
        printf("No native MJPEG mode found.\n");
        turbo_video_device_close(device);
        return 1;
    }

    turbo_capture_t *cap = NULL;
    int create_result = turbo_video_device_create_capture(
        device, selected_mode, &cap);
    turbo_video_device_close(device);
    if (create_result != TURBO_CAPTURE_OK || !cap) {
        printf("Failed to create capture instance.\n");
        return 1;
    }

    turbo_video_capture_set_callback(cap, on_video_frame, NULL);

    if (turbo_capture_start(cap) != 0) {
        printf("Failed to start capture.\n");
        turbo_capture_destroy(cap);
        return 1;
    }

    printf("Capture started. Waiting for frame...\n");

    /* Wait up to 5 seconds for a frame */
    for (int i = 0; i < 50; i++) {
        if (image_saved) break;
        SLEEP_MS(100);
    }


    turbo_capture_stop(cap);
    turbo_capture_destroy(cap);

    if (image_saved) {
        printf("Exiting... \n");
        return 0;
    } else {
        printf("Timeout: No frame captured.\n");
        return 1;
    }
}
