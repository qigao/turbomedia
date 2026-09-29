#include "internal/client_processing_internal.h"

void turbo_client_processing_video_capture_callback(
    salts_capture_t *capture,
    const uint8_t *frame, size_t len,
    int width, int height,
    uint64_t timestamp_us, void *user_data) {
    (void)capture;
    (void)turbo_client_processing_admit_video_frame(
        (turbo_client_processing_t *)user_data,
        frame, len, width, height, timestamp_us);
}

void turbo_client_processing_audio_capture_callback(
    salts_capture_t *capture,
    const uint8_t *samples, size_t len,
    uint64_t timestamp_us, void *user_data) {
    (void)capture;
    (void)turbo_client_processing_admit_audio_frame(
        (turbo_client_processing_t *)user_data,
        samples, len, timestamp_us);
}

turbo_client_processing_status_t turbo_client_processing_write_playback(
    turbo_client_processing_t *processing,
    salts_playback_t *playback,
    const void *samples, size_t len,
    size_t *out_written) {
    int status;

    if (out_written != NULL) {
        *out_written = 0u;
    }
    if (processing == NULL || playback == NULL || samples == NULL ||
        len == 0u || out_written == NULL) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (turbo_client_processing_state_get(processing) !=
        TURBO_CLIENT_PROCESSING_RUNNING) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }

    status = salts_playback_write(playback, samples, len, out_written);
    if (status == SALTS_PLAYBACK_ERR_FORMAT) {
        return TURBO_CLIENT_PROCESSING_EINVAL;
    }
    if (status == SALTS_PLAYBACK_ERR_BUSY) {
        return TURBO_CLIENT_PROCESSING_ESTATE;
    }
    if (status == SALTS_PLAYBACK_ERR_DEVICE) {
        return TURBO_CLIENT_PROCESSING_EIO;
    }
    if (status != SALTS_PLAYBACK_OK) {
        return TURBO_CLIENT_PROCESSING_EIO;
    }
    return *out_written == len
               ? TURBO_CLIENT_PROCESSING_OK
               : TURBO_CLIENT_PROCESSING_EFULL;
}
