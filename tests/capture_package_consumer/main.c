#include <turbo_capture.h>

#ifndef TURBO_CAPTURE_API
#error "TurboMedia package consumer resolved a stale pre-TurboUtils capture header"
#endif

int main(void) {
    turbo_video_native_mode_t mode = {
        .framerate_numerator = 30000,
        .framerate_denominator = 1001,
    };

    return turbo_video_mode_fps(&mode) == 30 ? 0 : 1;
}
