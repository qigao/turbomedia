#include "tinytest.h"
#include "turbo_client_processing.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int write_y4m_fixture(const char *path) {
  static const char header[] =
      "YUV4MPEG2 W16 H16 F10:1 Ip A0:0 C420jpeg\n";
  static const char frame_header[] = "FRAME\n";
  enum {
    WIDTH = 16,
    HEIGHT = 16,
    Y_BYTES = WIDTH * HEIGHT,
    UV_BYTES = (WIDTH / 2) * (HEIGHT / 2),
    FRAME_BYTES = Y_BYTES + UV_BYTES * 2,
    FRAME_COUNT = 2
  };
  const size_t total =
      (sizeof(header) - 1u) +
      FRAME_COUNT * ((sizeof(frame_header) - 1u) + FRAME_BYTES);
  uint8_t *data = (uint8_t *)malloc(total);
  size_t offset = 0u;
  int frame;
  int result;

  if (data == NULL) {
    return -1;
  }
  memcpy(data + offset, header, sizeof(header) - 1u);
  offset += sizeof(header) - 1u;
  for (frame = 0; frame < FRAME_COUNT; ++frame) {
    memcpy(data + offset, frame_header, sizeof(frame_header) - 1u);
    offset += sizeof(frame_header) - 1u;
    memset(data + offset, frame == 0 ? 32 : 192, Y_BYTES);
    offset += Y_BYTES;
    memset(data + offset, 128, UV_BYTES * 2u);
    offset += UV_BYTES * 2u;
  }
  result = tt_write_file(path, data, total);
  free(data);
  return result;
}

static int verify_h264_output(const char *path, int width, int height) {
  AVFormatContext *format = NULL;
  const AVCodec *decoder = NULL;
  int stream_index;
  int ok = 0;

  if (avformat_open_input(&format, path, NULL, NULL) < 0) {
    return 0;
  }
  if (avformat_find_stream_info(format, NULL) < 0) {
    goto cleanup;
  }
  stream_index =
      av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if (stream_index < 0 || decoder == NULL) {
    goto cleanup;
  }
  ok = format->streams[stream_index]->codecpar->codec_id == AV_CODEC_ID_H264 &&
       format->streams[stream_index]->codecpar->width == width &&
       format->streams[stream_index]->codecpar->height == height;

cleanup:
  avformat_close_input(&format);
  return ok;
}

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

  it("scales and encodes a local Y4M file through the explicit file plan") {
    char *input_path =
        tt_make_temp_file("turbo_client_processing_input_", ".y4m");
    char *output_path =
        tt_make_temp_file("turbo_client_processing_output_", ".mkv");
    turbo_client_processing_config_t config;
    turbo_client_processing_file_plan_t plan;
    turbo_client_processing_snapshot_t snapshot;
    turbo_client_processing_t *processing = NULL;

    check_not_null(input_path);
    check_not_null(output_path);
    if (input_path == NULL || output_path == NULL) {
      goto cleanup_file_slice;
    }
    check_equal(write_y4m_fixture(input_path), 0);

    turbo_client_processing_config_init(&config);
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);
    if (processing == NULL) {
      goto cleanup_file_slice;
    }

    turbo_client_processing_file_plan_init(&plan);
    plan.input_path = input_path;
    plan.output_path = output_path;
    plan.output_format = "matroska";
    plan.video_codec = "libopenh264";
    plan.output_width = 32u;
    plan.output_height = 32u;
    plan.frame_rate_num = 10u;
    plan.frame_rate_den = 1u;
    plan.bitrate = 100000u;

    check_equal(turbo_client_processing_set_file_plan(processing, &plan),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_prepare(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_run_file(processing),
                TURBO_CLIENT_PROCESSING_OK);

    turbo_client_processing_snapshot_init(&snapshot);
    check_equal(turbo_client_processing_snapshot(processing, &snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(snapshot.state, TURBO_CLIENT_PROCESSING_STOPPED);
    check_equal(snapshot.admitted_frames, 2u);
    check_true(verify_h264_output(output_path, 32, 32));

    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
    processing = NULL;

  cleanup_file_slice:
    if (processing != NULL) {
      turbo_client_processing_destroy(processing);
    }
    if (input_path != NULL) {
      tt_remove_file(input_path);
    }
    if (output_path != NULL) {
      tt_remove_file(output_path);
    }
    free(input_path);
    free(output_path);
  }

  it("fails unsupported file capabilities during prepare") {
    turbo_client_processing_config_t config;
    turbo_client_processing_file_plan_t plan;
    turbo_client_processing_snapshot_t snapshot;
    turbo_client_processing_t *processing = NULL;

    turbo_client_processing_config_init(&config);
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);

    turbo_client_processing_file_plan_init(&plan);
    plan.input_path = "missing-input.y4m";
    plan.output_path = "unused-output.mkv";
    plan.output_format = "matroska";
    plan.video_codec = "definitely-not-a-codec";
    plan.output_width = 32u;
    plan.output_height = 32u;
    plan.frame_rate_num = 10u;
    plan.frame_rate_den = 1u;
    plan.bitrate = 100000u;
    check_equal(turbo_client_processing_set_file_plan(processing, &plan),
                TURBO_CLIENT_PROCESSING_OK);

    /* Input open fails before codec discovery; failure must still be explicit. */
    check_equal(turbo_client_processing_prepare(processing),
                TURBO_CLIENT_PROCESSING_EOPEN);
    turbo_client_processing_snapshot_init(&snapshot);
    check_equal(turbo_client_processing_snapshot(processing, &snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(snapshot.state, TURBO_CLIENT_PROCESSING_FAILED);
    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
  }
}
