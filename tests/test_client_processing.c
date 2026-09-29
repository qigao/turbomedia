#include "tinytest.h"
#include "turbo_client_processing.h"

spec("TurboMedia ClientProcessing core") {
  it("initializes bounded defaults and snapshots the created state") {
    turbo_client_processing_config_t config;
    turbo_client_processing_snapshot_t snapshot;
    turbo_client_processing_t *processing = NULL;

    turbo_client_processing_config_init(&config);
    check_equal(config.size, sizeof(config));
    check(config.frame_queue_capacity > 0u);
    check(config.frame_queue_max_bytes > 0u);
    check(config.frame_queue_max_duration_us > 0u);

    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_not_null(processing);

    turbo_client_processing_snapshot_init(&snapshot);
    check_equal(turbo_client_processing_snapshot(processing, &snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(snapshot.state, TURBO_CLIENT_PROCESSING_CREATED);
    check_equal(snapshot.frame_queue_capacity, config.frame_queue_capacity);
    check_equal(snapshot.frame_queue_max_bytes, config.frame_queue_max_bytes);
    check_equal(snapshot.frame_queue_max_duration_us,
                config.frame_queue_max_duration_us);
    check_equal(snapshot.queued_frames, 0u);
    check_equal(snapshot.queued_bytes, 0u);
    check_equal(snapshot.queued_duration_us, 0u);

    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
  }

  it("rejects malformed or unbounded configuration") {
    turbo_client_processing_config_t config;
    turbo_client_processing_t *processing = (turbo_client_processing_t *)1;

    turbo_client_processing_config_init(&config);
    config.size = 0u;
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_EINVAL);
    check_null(processing);

    turbo_client_processing_config_init(&config);
    config.frame_queue_capacity = 0u;
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_EINVAL);
    check_null(processing);

    turbo_client_processing_config_init(&config);
    config.frame_queue_max_bytes = 0u;
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_EINVAL);
    check_null(processing);

    turbo_client_processing_config_init(&config);
    config.frame_queue_max_duration_us = 0u;
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_EINVAL);
    check_null(processing);
  }

  it("enforces the owner lifecycle and explicit drain") {
    turbo_client_processing_config_t config;
    turbo_client_processing_snapshot_t snapshot;
    turbo_client_processing_t *processing = NULL;

    turbo_client_processing_config_init(&config);
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);

    check_equal(turbo_client_processing_start(processing),
                TURBO_CLIENT_PROCESSING_ESTATE);
    check_equal(turbo_client_processing_prepare(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_prepare(processing),
                TURBO_CLIENT_PROCESSING_ESTATE);
    check_equal(turbo_client_processing_start(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_ESTATE);

    check_equal(turbo_client_processing_pause(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_pause(processing),
                TURBO_CLIENT_PROCESSING_ESTATE);
    check_equal(turbo_client_processing_resume(processing),
                TURBO_CLIENT_PROCESSING_OK);

    check_equal(turbo_client_processing_request_stop(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_resume(processing),
                TURBO_CLIENT_PROCESSING_ESTATE);
    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_ESTATE);
    check_equal(turbo_client_processing_drain(processing),
                TURBO_CLIENT_PROCESSING_OK);

    turbo_client_processing_snapshot_init(&snapshot);
    check_equal(turbo_client_processing_snapshot(processing, &snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(snapshot.state, TURBO_CLIENT_PROCESSING_STOPPED);
    check_equal(snapshot.queued_frames, 0u);
    check_equal(snapshot.queued_bytes, 0u);
    check_equal(snapshot.queued_duration_us, 0u);

    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
  }

  it("requires versioned snapshot storage") {
    turbo_client_processing_config_t config;
    turbo_client_processing_snapshot_t snapshot = {0};
    turbo_client_processing_t *processing = NULL;

    turbo_client_processing_config_init(&config);
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_snapshot(processing, &snapshot),
                TURBO_CLIENT_PROCESSING_EINVAL);
    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
  }
}
