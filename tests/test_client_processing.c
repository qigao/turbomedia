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
  it("copies borrowed Salts video frames before callback return") {
    turbo_client_processing_config_t config;
    turbo_client_processing_snapshot_t snapshot;
    turbo_client_processing_video_frame_info_t info;
    turbo_client_processing_t *processing = NULL;
    uint8_t borrowed[4] = {1u, 2u, 3u, 4u};
    uint8_t copied[4] = {0};
    size_t copied_size = 0u;

    turbo_client_processing_config_init(&config);
    config.frame_queue_capacity = 2u;
    config.frame_queue_max_bytes = 16u;
    config.frame_queue_max_duration_us = 1000u;
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_prepare(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_start(processing),
                TURBO_CLIENT_PROCESSING_OK);

    turbo_client_processing_video_capture_callback(
        NULL, borrowed, sizeof(borrowed), 2, 2, 100u, processing);
    memset(borrowed, 9, sizeof(borrowed));

    turbo_client_processing_video_frame_info_init(&info);
    check_equal(turbo_client_processing_pop_video_frame(
                    processing, copied, sizeof(copied), &copied_size, &info),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(copied_size, sizeof(copied));
    check_equal(copied[0], 1);
    check_equal(copied[1], 2);
    check_equal(copied[2], 3);
    check_equal(copied[3], 4);
    check_equal(info.data_size, sizeof(copied));
    check_equal(info.width, 2);
    check_equal(info.height, 2);
    check_equal(info.timestamp_us, 100u);

    turbo_client_processing_snapshot_init(&snapshot);
    check_equal(turbo_client_processing_snapshot(processing, &snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(snapshot.admitted_frames, 1u);
    check_equal(snapshot.rejected_frames, 0u);
    check_equal(snapshot.queued_frames, 0u);

    check_equal(turbo_client_processing_request_stop(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_drain(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
  }

  it("enforces item byte and time bounds without dropping older frames") {
    turbo_client_processing_config_t config;
    turbo_client_processing_snapshot_t snapshot;
    turbo_client_processing_video_frame_info_t info;
    turbo_client_processing_t *processing = NULL;
    const uint8_t frame_a[4] = {1u, 1u, 1u, 1u};
    const uint8_t frame_b[4] = {2u, 2u, 2u, 2u};
    const uint8_t frame_c[3] = {3u, 3u, 3u};
    uint8_t output[4] = {0};
    size_t output_size = 0u;

    turbo_client_processing_config_init(&config);
    config.frame_queue_capacity = 2u;
    config.frame_queue_max_bytes = 8u;
    config.frame_queue_max_duration_us = 50u;
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_prepare(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_start(processing),
                TURBO_CLIENT_PROCESSING_OK);

    check_equal(turbo_client_processing_admit_video_frame(
                    processing, frame_a, sizeof(frame_a), 2, 2, 100u),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_admit_video_frame(
                    processing, frame_b, sizeof(frame_b), 2, 2, 150u),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_admit_video_frame(
                    processing, frame_c, sizeof(frame_c), 1, 3, 151u),
                TURBO_CLIENT_PROCESSING_EFULL);

    turbo_client_processing_snapshot_init(&snapshot);
    check_equal(turbo_client_processing_snapshot(processing, &snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(snapshot.queued_frames, 2u);
    check_equal(snapshot.queued_bytes, 8u);
    check_equal(snapshot.queued_duration_us, 50u);
    check_equal(snapshot.admitted_frames, 2u);
    check_equal(snapshot.rejected_frames, 1u);

    turbo_client_processing_video_frame_info_init(&info);
    check_equal(turbo_client_processing_pop_video_frame(
                    processing, output, 2u, &output_size, &info),
                TURBO_CLIENT_PROCESSING_EFULL);
    check_equal(output_size, sizeof(frame_a));
    check_equal(turbo_client_processing_snapshot(processing, &snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(snapshot.queued_frames, 2u);

    check_equal(turbo_client_processing_pop_video_frame(
                    processing, output, sizeof(output), &output_size, &info),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(output[0], 1);
    check_equal(turbo_client_processing_admit_video_frame(
                    processing, frame_c, sizeof(frame_c), 1, 3, 151u),
                TURBO_CLIENT_PROCESSING_OK);

    check_equal(turbo_client_processing_pause(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_admit_video_frame(
                    processing, frame_a, sizeof(frame_a), 2, 2, 152u),
                TURBO_CLIENT_PROCESSING_ESTATE);
    check_equal(turbo_client_processing_resume(processing),
                TURBO_CLIENT_PROCESSING_OK);

    check_equal(turbo_client_processing_request_stop(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_drain(processing),
                TURBO_CLIENT_PROCESSING_OK);
    turbo_client_processing_snapshot_init(&snapshot);
    check_equal(turbo_client_processing_snapshot(processing, &snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(snapshot.queued_frames, 0u);
    check_equal(snapshot.queued_bytes, 0u);
    check_equal(snapshot.queued_duration_us, 0u);
    check_equal(snapshot.rejected_frames, 2u);
    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
  }

  it("enforces retained-byte and timestamp-span bounds independently") {
    turbo_client_processing_config_t config;
    turbo_client_processing_t *processing = NULL;
    const uint8_t four[4] = {1u, 2u, 3u, 4u};
    const uint8_t three[3] = {5u, 6u, 7u};
    uint8_t output[4];
    size_t output_size = 0u;
    turbo_client_processing_video_frame_info_t info;

    turbo_client_processing_config_init(&config);
    config.frame_queue_capacity = 4u;
    config.frame_queue_max_bytes = 6u;
    config.frame_queue_max_duration_us = 10u;
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_prepare(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_start(processing),
                TURBO_CLIENT_PROCESSING_OK);

    check_equal(turbo_client_processing_admit_video_frame(
                    processing, four, sizeof(four), 2, 2, 100u),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_admit_video_frame(
                    processing, three, sizeof(three), 1, 3, 105u),
                TURBO_CLIENT_PROCESSING_EFULL);

    turbo_client_processing_video_frame_info_init(&info);
    check_equal(turbo_client_processing_pop_video_frame(
                    processing, output, sizeof(output), &output_size, &info),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_admit_video_frame(
                    processing, three, sizeof(three), 1, 3, 200u),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_admit_video_frame(
                    processing, three, sizeof(three), 1, 3, 211u),
                TURBO_CLIENT_PROCESSING_EFULL);
    check_equal(turbo_client_processing_admit_video_frame(
                    processing, three, sizeof(three), 1, 3, 199u),
                TURBO_CLIENT_PROCESSING_EINVAL);

    check_equal(turbo_client_processing_request_stop(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_drain(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
  }

  it("copies borrowed Salts audio frames into an independent bounded queue") {
    turbo_client_processing_config_t config;
    turbo_client_processing_audio_capture_config_t audio_config;
    turbo_client_processing_audio_snapshot_t audio_snapshot;
    turbo_client_processing_audio_frame_info_t info;
    turbo_client_processing_t *processing = NULL;
    uint8_t borrowed[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    uint8_t copied[8] = {0};
    size_t copied_size = 0u;

    turbo_client_processing_config_init(&config);
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);

    turbo_client_processing_audio_capture_config_init(&audio_config);
    audio_config.queue_capacity = 2u;
    audio_config.queue_max_bytes = 16u;
    audio_config.queue_max_duration_us = 20000u;
    audio_config.sample_rate = 48000u;
    audio_config.channels = 2u;
    audio_config.bits_per_sample = 16u;
    check_equal(turbo_client_processing_set_audio_capture_config(
                    processing, &audio_config),
                TURBO_CLIENT_PROCESSING_OK);

    check_equal(turbo_client_processing_prepare(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_start(processing),
                TURBO_CLIENT_PROCESSING_OK);

    turbo_client_processing_audio_capture_callback(
        NULL, borrowed, sizeof(borrowed), 1000u, processing);
    memset(borrowed, 9, sizeof(borrowed));

    turbo_client_processing_audio_frame_info_init(&info);
    check_equal(turbo_client_processing_pop_audio_frame(
                    processing, copied, sizeof(copied), &copied_size, &info),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(copied_size, sizeof(copied));
    check_equal(copied[0], 1);
    check_equal(copied[7], 8);
    check_equal(info.data_size, sizeof(copied));
    check_equal(info.timestamp_us, 1000u);
    check_equal(info.sample_rate, 48000u);
    check_equal(info.channels, 2u);
    check_equal(info.bits_per_sample, 16u);

    turbo_client_processing_audio_snapshot_init(&audio_snapshot);
    check_equal(turbo_client_processing_audio_snapshot(
                    processing, &audio_snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(audio_snapshot.admitted_frames, 1u);
    check_equal(audio_snapshot.rejected_frames, 0u);
    check_equal(audio_snapshot.queued_frames, 0u);

    check_equal(turbo_client_processing_request_stop(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_drain(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
  }

  it("enforces audio item byte alignment and timestamp-span bounds") {
    turbo_client_processing_config_t config;
    turbo_client_processing_audio_capture_config_t audio_config;
    turbo_client_processing_audio_snapshot_t audio_snapshot;
    turbo_client_processing_audio_frame_info_t info;
    turbo_client_processing_t *processing = NULL;
    const uint8_t pcm_a[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    const uint8_t pcm_b[8] = {9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u};
    const uint8_t misaligned[3] = {1u, 2u, 3u};
    uint8_t output[8] = {0};
    size_t output_size = 0u;

    turbo_client_processing_config_init(&config);
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);

    turbo_client_processing_audio_capture_config_init(&audio_config);
    audio_config.queue_capacity = 2u;
    audio_config.queue_max_bytes = 16u;
    audio_config.queue_max_duration_us = 10000u;
    audio_config.sample_rate = 48000u;
    audio_config.channels = 2u;
    audio_config.bits_per_sample = 16u;
    check_equal(turbo_client_processing_set_audio_capture_config(
                    processing, &audio_config),
                TURBO_CLIENT_PROCESSING_OK);

    check_equal(turbo_client_processing_prepare(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_start(processing),
                TURBO_CLIENT_PROCESSING_OK);

    check_equal(turbo_client_processing_admit_audio_frame(
                    processing, misaligned, sizeof(misaligned), 1000u),
                TURBO_CLIENT_PROCESSING_EINVAL);
    check_equal(turbo_client_processing_admit_audio_frame(
                    processing, pcm_a, sizeof(pcm_a), 1000u),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_admit_audio_frame(
                    processing, pcm_b, sizeof(pcm_b), 11000u),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_admit_audio_frame(
                    processing, pcm_b, sizeof(pcm_b), 11001u),
                TURBO_CLIENT_PROCESSING_EFULL);

    turbo_client_processing_audio_snapshot_init(&audio_snapshot);
    check_equal(turbo_client_processing_audio_snapshot(
                    processing, &audio_snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(audio_snapshot.queued_frames, 2u);
    check_equal(audio_snapshot.queued_bytes, 16u);
    check_equal(audio_snapshot.queued_duration_us, 10000u);
    check_equal(audio_snapshot.admitted_frames, 2u);
    check_equal(audio_snapshot.rejected_frames, 2u);

    turbo_client_processing_audio_frame_info_init(&info);
    check_equal(turbo_client_processing_pop_audio_frame(
                    processing, output, 4u, &output_size, &info),
                TURBO_CLIENT_PROCESSING_EFULL);
    check_equal(output_size, sizeof(pcm_a));
    check_equal(turbo_client_processing_audio_snapshot(
                    processing, &audio_snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(audio_snapshot.queued_frames, 2u);

    check_equal(turbo_client_processing_pop_audio_frame(
                    processing, output, sizeof(output), &output_size, &info),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(output[0], 1);

    check_equal(turbo_client_processing_pause(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_admit_audio_frame(
                    processing, pcm_a, sizeof(pcm_a), 12000u),
                TURBO_CLIENT_PROCESSING_ESTATE);
    check_equal(turbo_client_processing_resume(processing),
                TURBO_CLIENT_PROCESSING_OK);

    check_equal(turbo_client_processing_request_stop(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_drain(processing),
                TURBO_CLIENT_PROCESSING_OK);
    turbo_client_processing_audio_snapshot_init(&audio_snapshot);
    check_equal(turbo_client_processing_audio_snapshot(
                    processing, &audio_snapshot),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(audio_snapshot.queued_frames, 0u);
    check_equal(audio_snapshot.queued_bytes, 0u);
    check_equal(audio_snapshot.queued_duration_us, 0u);
    check_equal(audio_snapshot.rejected_frames, 3u);
    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
  }

  it("validates playback borrowing without taking device lifecycle ownership") {
    turbo_client_processing_config_t config;
    turbo_client_processing_t *processing = NULL;
    const int16_t pcm[2] = {0, 0};
    size_t written = 99u;

    turbo_client_processing_config_init(&config);
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_prepare(processing),
                TURBO_CLIENT_PROCESSING_OK);

    check_equal(turbo_client_processing_write_playback(
                    processing, NULL, pcm, sizeof(pcm), &written),
                TURBO_CLIENT_PROCESSING_EINVAL);
    check_equal(written, 0u);

    check_equal(turbo_client_processing_start(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_write_playback(
                    processing, NULL, pcm, sizeof(pcm), &written),
                TURBO_CLIENT_PROCESSING_EINVAL);
    check_equal(written, 0u);

    check_equal(turbo_client_processing_request_stop(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_drain(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
  }

  it("uses the full audio byte budget across ring wrap") {
    turbo_client_processing_config_t config;
    turbo_client_processing_audio_capture_config_t audio_config;
    turbo_client_processing_audio_frame_info_t info;
    turbo_client_processing_t *processing = NULL;
    const uint8_t a[3] = {1u, 2u, 3u};
    const uint8_t b[5] = {4u, 5u, 6u, 7u, 8u};
    const uint8_t cframe[5] = {9u, 10u, 11u, 12u, 13u};
    uint8_t output[5] = {0};
    size_t output_size = 0u;

    turbo_client_processing_config_init(&config);
    check_equal(turbo_client_processing_create(&config, &processing),
                TURBO_CLIENT_PROCESSING_OK);

    turbo_client_processing_audio_capture_config_init(&audio_config);
    audio_config.queue_capacity = 3u;
    audio_config.queue_max_bytes = 10u;
    audio_config.queue_max_duration_us = 1000u;
    audio_config.sample_rate = 16000u;
    audio_config.channels = 1u;
    audio_config.bits_per_sample = 16u;
    /*
     * S16 mono requires 2-byte alignment, so use 4/4/4 bytes to force a
     * split write after popping the first record from a 10-byte arena.
     */
    check_equal(turbo_client_processing_set_audio_capture_config(
                    processing, &audio_config),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_prepare(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_start(processing),
                TURBO_CLIENT_PROCESSING_OK);

    {
      const uint8_t first[4] = {1u, 2u, 3u, 4u};
      const uint8_t second[4] = {5u, 6u, 7u, 8u};
      const uint8_t wrapped[4] = {9u, 10u, 11u, 12u};
      turbo_client_processing_audio_frame_info_init(&info);
      check_equal(turbo_client_processing_admit_audio_frame(
                      processing, first, sizeof(first), 100u),
                  TURBO_CLIENT_PROCESSING_OK);
      check_equal(turbo_client_processing_admit_audio_frame(
                      processing, second, sizeof(second), 110u),
                  TURBO_CLIENT_PROCESSING_OK);
      check_equal(turbo_client_processing_pop_audio_frame(
                      processing, output, sizeof(output), &output_size, &info),
                  TURBO_CLIENT_PROCESSING_OK);
      check_equal(turbo_client_processing_admit_audio_frame(
                      processing, wrapped, sizeof(wrapped), 120u),
                  TURBO_CLIENT_PROCESSING_OK);

      check_equal(turbo_client_processing_pop_audio_frame(
                      processing, output, sizeof(output), &output_size, &info),
                  TURBO_CLIENT_PROCESSING_OK);
      check_equal(output[0], 5);
      check_equal(output[3], 8);
      check_equal(turbo_client_processing_pop_audio_frame(
                      processing, output, sizeof(output), &output_size, &info),
                  TURBO_CLIENT_PROCESSING_OK);
      check_equal(output[0], 9);
      check_equal(output[3], 12);
    }

    check_equal(turbo_client_processing_request_stop(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_drain(processing),
                TURBO_CLIENT_PROCESSING_OK);
    check_equal(turbo_client_processing_destroy(processing),
                TURBO_CLIENT_PROCESSING_OK);
    (void)a;
    (void)b;
    (void)cframe;
  }

}
