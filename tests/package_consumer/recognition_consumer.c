#include <turbo_recognition.h>

int main(void) {
    void (*callback)(salts_capture_t *, const uint8_t *, size_t, uint64_t, void *) =
        turbo_fingerprint_capture_callback;
    return callback ? 0 : 1;
}
