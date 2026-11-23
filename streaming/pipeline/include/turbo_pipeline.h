#ifndef TURBO_PIPELINE_H
#define TURBO_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

#include "turbo_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_pipeline turbo_pipeline_t;
typedef struct turbo_media_server_runtime_s turbo_media_server_runtime_t;

typedef enum turbo_pipeline_status {
    TURBO_PIPELINE_OK = 0,
    TURBO_PIPELINE_EINVAL = -1,
    TURBO_PIPELINE_ECONFIG = -2,
    TURBO_PIPELINE_EGRAPH = -3,
    TURBO_PIPELINE_ENOMEM = -4,
    TURBO_PIPELINE_EIO = -5,
    TURBO_PIPELINE_EFFMPEG = -6,
    TURBO_PIPELINE_ESTATE = -7,
    TURBO_PIPELINE_ESTOPPED = -8,
    TURBO_PIPELINE_EBACKPRESSURE = -9,
    TURBO_PIPELINE_ECALLBACK = -10
} turbo_pipeline_status_t;

typedef enum turbo_pipeline_state {
    TURBO_PIPELINE_STATE_CREATED = 0,
    TURBO_PIPELINE_STATE_PREPARED,
    TURBO_PIPELINE_STATE_RUNNING,
    TURBO_PIPELINE_STATE_STOPPING,
    TURBO_PIPELINE_STATE_STOPPED,
    TURBO_PIPELINE_STATE_FAILED
} turbo_pipeline_state_t;

typedef struct turbo_pipeline_error {
    turbo_pipeline_status_t code;
    char node_id[64];
    char message[512];
} turbo_pipeline_error_t;

typedef struct turbo_pipeline_stats {
    uint64_t packets_read;
    uint64_t packets_written;
    uint64_t frames_decoded;
    uint64_t frames_encoded;
    uint64_t bytes_read;
    uint64_t bytes_written;
} turbo_pipeline_stats_t;

typedef struct turbo_pipeline_video_plane {
    uint8_t *data;     /* First logical row. */
    ptrdiff_t stride;  /* Signed byte offset from one row to the next. */
    size_t row_bytes;  /* Writable bytes per row, excluding padding. */
    size_t rows;
} turbo_pipeline_video_plane_t;

/** Writable 8-bit YUV420P pixels, with planes in Y, U, V order. */
typedef struct turbo_pipeline_video_frame {
    int width;
    int height;
    int64_t pts;  /* INT64_MIN if unknown. Uses the encoder time base. */
    int time_base_num;
    int time_base_den;
    turbo_pipeline_video_plane_t planes[3];
} turbo_pipeline_video_frame_t;

typedef int (*turbo_pipeline_video_frame_editor_fn)(
    const turbo_pipeline_video_frame_t *frame, void *context);

/**
 * Parse and validate a version 1 graph configuration.
 *
 * Creation performs no network or file I/O. The returned pipeline owns an
 * immutable copy of all configuration data.
 *
 * @param yaml UTF-8 YAML bytes; the buffer may be released after this call.
 * @param yaml_size Exact byte count excluding any optional trailing NUL.
 * @param error Optional caller-owned error detail.
 * @return A pipeline in CREATED state, or NULL for invalid input, schema,
 *         graph, limit or allocation errors.
 */
TURBO_MEDIA_API turbo_pipeline_t *turbo_pipeline_create_from_yaml(
    const char *yaml, size_t yaml_size, turbo_pipeline_error_t *error);

/**
 * Bind the ServerRuntime used by media.runtime_source/runtime_sink nodes.
 *
 * The runtime is borrowed and must outlive the pipeline. Binding is accepted
 * only in CREATED state. Keep the referenced Runtime sources alive through
 * destruction. Serialize prepare and destroy with the Runtime owner that
 * manages source subscriptions. The input publisher may continue while run()
 * stops or fails. Quiesce it before destruction.
 */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_bind_server_runtime(
    turbo_pipeline_t *pipeline,
    turbo_media_server_runtime_t *runtime,
    turbo_pipeline_error_t *error);

/**
 * Set a synchronous video editor in CREATED state on a video transcoding graph.
 *
 * The run thread calls editor once per frame after filtering and before encoding.
 * A configured editor requires the video encoder to use 8-bit YUV420P.
 * The frame view and its writable row bytes are borrowed until editor returns.
 * Edit only the active bytes of each row. Keep context alive through run().
 * Serialize editor binding with all pipeline lifecycle operations.
 * Return zero to encode the edited frame or a nonzero value to fail the run
 * with TURBO_PIPELINE_ECALLBACK. A NULL editor clears the binding.
 */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_set_video_frame_editor(
    turbo_pipeline_t *pipeline, turbo_pipeline_video_frame_editor_fn editor,
    void *context, turbo_pipeline_error_t *error);

/**
 * Open the configured FFmpeg resources or attach Runtime/RTP graph adapters.
 *
 * This function fails fast. On failure the pipeline enters FAILED and all
 * resources opened by this call are released.
 *
 * @return TURBO_PIPELINE_OK, or a configuration, I/O, FFmpeg or state error.
 */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_prepare(
    turbo_pipeline_t *pipeline, turbo_pipeline_error_t *error);

/**
 * Run the prepared pipeline synchronously until input EOF or a stop request.
 *
 * The caller owns the running thread. Only turbo_pipeline_request_stop(),
 * turbo_pipeline_state() and turbo_pipeline_stats() may be called concurrently
 * with this function. A Runtime/RTP graph copies incoming RTP packets into a
 * bounded queue; a full queue is reported to both publisher and runner.
 * Runtime exit seals the input queue. The input subscription remains attached
 * until destroy() and ignores subsequent packets.
 *
 * @return TURBO_PIPELINE_OK at EOF, TURBO_PIPELINE_ESTOPPED after cooperative
 *         cancellation, or a fatal processing error.
 */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_run(
    turbo_pipeline_t *pipeline, turbo_pipeline_error_t *error);

/**
 * Request cooperative cancellation of blocking FFmpeg I/O and processing.
 * @return TURBO_PIPELINE_OK, TURBO_PIPELINE_EINVAL or TURBO_PIPELINE_ESTATE.
 */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_request_stop(turbo_pipeline_t *pipeline);

/** Return the current lifecycle state. */
TURBO_MEDIA_API turbo_pipeline_state_t turbo_pipeline_state(const turbo_pipeline_t *pipeline);

/**
 * Copy a point-in-time statistics snapshot.
 *
 * packets_written and bytes_written count successful sink deliveries. A media
 * packet delivered to two FFmpeg outputs contributes two writes.
 */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_stats(
    const turbo_pipeline_t *pipeline, turbo_pipeline_stats_t *stats);

/**
 * Release a non-running pipeline and all resources.
 *
 * Wait for run() and concurrent stop/state/stat calls to return before freeing
 * the pipeline. Calling destroy while it is running requests stop but does not
 * free it. In Runtime/RTP mode, destroy from the Runtime owner after quiescing
 * the input publisher. Destruction removes the input subscription.
 */
TURBO_MEDIA_API void turbo_pipeline_destroy(turbo_pipeline_t *pipeline);

#ifdef __cplusplus
}
#endif

#endif
