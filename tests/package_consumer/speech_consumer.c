#include <turbo_speech.h>

int main(void) {
    salts_audio_capture_cb callback = turbo_asr_capture_callback;
    return callback ? 0 : 1;
}
