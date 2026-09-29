#include <turbo_client_processing.h>

int main(void) {
    salts_video_capture_cb video_callback =
        turbo_client_processing_video_capture_callback;
    turbo_client_processing_config_t config;
    turbo_client_processing_snapshot_t snapshot;
    turbo_client_processing_t *processing = NULL;

    (void)video_callback;
    turbo_client_processing_config_init(&config);
    if (turbo_client_processing_create(&config, &processing) !=
        TURBO_CLIENT_PROCESSING_OK) {
        return 1;
    }
    if (turbo_client_processing_prepare(processing) !=
        TURBO_CLIENT_PROCESSING_OK) {
        return 2;
    }
    turbo_client_processing_snapshot_init(&snapshot);
    if (turbo_client_processing_snapshot(processing, &snapshot) !=
            TURBO_CLIENT_PROCESSING_OK ||
        snapshot.state != TURBO_CLIENT_PROCESSING_PREPARED) {
        return 3;
    }
    if (turbo_client_processing_request_stop(processing) !=
            TURBO_CLIENT_PROCESSING_OK ||
        turbo_client_processing_drain(processing) !=
            TURBO_CLIENT_PROCESSING_OK) {
        return 4;
    }
    if (turbo_client_processing_destroy(processing) !=
        TURBO_CLIENT_PROCESSING_OK) {
        return 5;
    }
    return 0;
}
