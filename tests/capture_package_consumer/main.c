#include <salts_capture.h>

int main(void) {
    salts_video_native_mode_t mode = {
        .framerate_numerator = 30000,
        .framerate_denominator = 1001,
    };

    return salts_video_mode_fps(&mode) == 30 ? 0 : 1;
}
