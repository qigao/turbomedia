#include <turbo_recognition.h>

int main(void) {
    salts_audio_capture_cb callback = turbo_fingerprint_capture_callback;
    return callback ? 0 : 1;
}
