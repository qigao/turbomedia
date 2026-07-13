/**
 * Capture to Image Example
 * 
 * Captures a single frame from the webcam and saves it as a PNG file.
 */

#include "turbo_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libyuv/convert_argb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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

    printf("Captured frame (I420): %dx%d, %zu bytes\n", width, height, len);

    uint8_t *rgb_frame = (uint8_t *)malloc(width * height * 3);
    if (!rgb_frame) return;

    const size_t y_size = (size_t)width * (size_t)height;
    const size_t uv_size = y_size / 4;
    const uint8_t *y_plane = frame;
    const uint8_t *u_plane = frame + y_size;
    const uint8_t *v_plane = u_plane + uv_size;

    if (I420ToRAW(y_plane, width,
                  u_plane, width / 2,
                  v_plane, width / 2,
                  rgb_frame, width * 3,
                  width, height) != 0) {
        printf("Failed to convert I420 to RGB.\n");
        free(rgb_frame);
        return;
    }

    const char *filename = "captured_frame.png";
    if (stbi_write_png(filename, width, height, 3, rgb_frame, width * 3)) {
        printf("Success! Saved to %s\n", filename);
        image_saved = 1;
    } else {
        printf("Failed to save image.\n");
    }
    
    free(rgb_frame);
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

    /* Request I420, the raw video format used by the WebRTC encoder path. */
    turbo_video_capture_config_t config = {0};
    config.width = 640;
    config.height = 480;
    config.framerate = 30;
    config.format = 0; /* I420 */

    char device_id[16];
    snprintf(device_id, sizeof(device_id), "%d", devices[0].index);
    printf("Opening device: %s\n", devices[0].name);

    turbo_capture_t *cap = turbo_video_capture_create(device_id, &config);
    if (!cap) {
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
