#include <turbo_speech.h>

int main(void) {
    void (*callback)(salts_capture_t *, const uint8_t *, size_t, uint64_t, void *) =
        turbo_asr_capture_callback;
    return callback ? 0 : 1;
}
