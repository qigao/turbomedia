#ifndef TURBO_PIPELINE_COMPOSITOR_H
#define TURBO_PIPELINE_COMPOSITOR_H

#include <turbo_pipeline.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_pipeline_compositor turbo_pipeline_compositor_t;

typedef enum turbo_compositor_input_color {
    TURBO_COMPOSITOR_COLOR_FROM_FRAME = 0,
    TURBO_COMPOSITOR_COLOR_BT709_LIMITED,
    TURBO_COMPOSITOR_COLOR_BT601_LIMITED
} turbo_compositor_input_color_t;

typedef enum turbo_compositor_layer_kind {
    TURBO_COMPOSITOR_MAIN_VIDEO = 0,
    TURBO_COMPOSITOR_RGBA_IMAGE,
    TURBO_COMPOSITOR_AUX_VIDEO
} turbo_compositor_layer_kind_t;

typedef struct turbo_compositor_plane {
    const uint8_t *data; /* Base of the accessible storage. */
    size_t size;
    size_t first_row;
    ptrdiff_t stride;
} turbo_compositor_plane_t;

typedef struct turbo_compositor_rect {
    int x, y, width, height;
} turbo_compositor_rect_t;

typedef struct turbo_compositor_position {
    int x, y;
} turbo_compositor_position_t;

typedef struct turbo_compositor_layer {
    turbo_compositor_layer_kind_t kind;
    turbo_compositor_rect_t rect;
    uint8_t opacity; /* 0 = transparent, 255 = opaque */
    union {
        struct {
            int width, height;
            turbo_compositor_plane_t rgba; /* Straight-alpha sRGB RGBA8. */
        } image;
        uint32_t source;
    } input;
} turbo_compositor_layer_t;

typedef struct turbo_compositor_source {
    int width, height;   /* Fixed I420 BT.709 SDR limited-range input. */
    size_t slots;        /* At least two, including the currently held frame. */
    int64_t max_age_us;
    int required;       /* A missing or expired frame fails the run when true. */
} turbo_compositor_source_t;

typedef struct turbo_compositor_config {
    int width, height;  /* Even output canvas dimensions. */
    turbo_compositor_input_color_t main_color;
    int64_t main_anchor_pts; /* In the Pipeline video encoder time base. */
    int64_t main_anchor_us;
    const turbo_compositor_layer_t *layers; /* Back to front. */
    size_t layer_count;
    const turbo_compositor_source_t *sources;
    size_t source_count;
    size_t memory_limit; /* Layout, queues, pixel and backend buffers in bytes. */
} turbo_compositor_config_t;

typedef struct turbo_compositor_stats {
    uint64_t submitted;
    uint64_t full;
    uint64_t invalid;
    uint64_t expired;
    uint64_t missing;
    uint64_t peak_slots;
} turbo_compositor_stats_t;

typedef enum turbo_compositor_backend {
    TURBO_COMPOSITOR_CPU = 0,
    TURBO_COMPOSITOR_D3D11
} turbo_compositor_backend_t;

typedef struct turbo_compositor_options {
    size_t size;
    turbo_compositor_backend_t backend;
    int luma_histogram;
} turbo_compositor_options_t;

typedef struct turbo_compositor_luma {
    int64_t pts;
    int time_base_num, time_base_den;
    uint32_t bins[256];
} turbo_compositor_luma_t;

/** Compile the layout and copy image pixels. A main video layer opts into a
 * black background. Otherwise the input canvas forms the background. Auxiliary
 * sources have one layer each. All inputs may be released after create. */
TURBO_MEDIA_API turbo_pipeline_compositor_t *turbo_pipeline_compositor_create(
    const turbo_compositor_config_t *config, turbo_pipeline_error_t *error);

/** Create with an explicit backend, or use CPU defaults with NULL options.
 * Set options->size to sizeof(*options) and luma_histogram to 0 or 1.
 * D3D11 requires Windows and a hardware feature-level 11 device. Creation
 * reports failure if the requested backend or its buffer budget is unavailable.
 * Pixel transfers use the existing I420 frame boundary. */
TURBO_MEDIA_API turbo_pipeline_compositor_t *turbo_pipeline_compositor_create_ex(
    const turbo_compositor_config_t *config, const turbo_compositor_options_t *options,
    turbo_pipeline_error_t *error);

/** Copy the histogram of the latest composed frame and its timestamp in the
 * encoder time base. The 256 bins count raw I420 luma samples, including bars
 * and overlays. Safe during render and submit.
 * ESTATE means analysis is disabled or no frame has been composed. */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_compositor_luma(
    turbo_pipeline_compositor_t *compositor, turbo_compositor_luma_t *result);

/** Bind to a CREATED video transcode Pipeline. Both objects must be quiescent.
 * The compositor must outlive the bound Pipeline. The binding occupies the
 * video editor slot and closes the compositor when the Pipeline is destroyed. */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_bind_compositor(
    turbo_pipeline_t *pipeline, turbo_pipeline_compositor_t *compositor,
    turbo_pipeline_error_t *error);

/** Replace all layer positions in original layer order. Each position must be
 * nonnegative, even, and leave the entire layer on the canvas. The array is
 * copied before return. Sizes, opacity and order come from the create config.
 *
 * Safe during render and concurrent control calls. The next render that
 * samples positions uses the latest accepted update for the whole frame.
 * EINVAL means invalid arguments/positions, ESTATE means closed.
 * A rejected update leaves accepted positions intact. */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_compositor_set_positions(
    turbo_pipeline_compositor_t *compositor,
    const turbo_compositor_position_t *positions, size_t count);

/** Copy three I420 planes into an auxiliary source slot. Each source has one
 * submitting thread. Start concurrent submissions after binding. pts_us uses
 * the common session timeline and must be nondecreasing for that source.
 * EBACKPRESSURE leaves the frame with the caller. */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_compositor_submit(
    turbo_pipeline_compositor_t *compositor, uint32_t source,
    const turbo_compositor_plane_t planes[3], int64_t pts_us);

/** Read a per-source snapshot. Safe during submit and render. */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_compositor_stats(
    const turbo_pipeline_compositor_t *compositor, uint32_t source,
    turbo_compositor_stats_t *stats);

/** Close frame and position admission. In-flight updates may finish. */
TURBO_MEDIA_API void turbo_pipeline_compositor_close(
    turbo_pipeline_compositor_t *compositor);

/** Destroy after quiescing all compositor callers and destroying the Pipeline.
 * A compositor still bound to a Pipeline returns ESTATE. */
TURBO_MEDIA_API turbo_pipeline_status_t turbo_pipeline_compositor_destroy(
    turbo_pipeline_compositor_t *compositor);

#ifdef __cplusplus
}
#endif
#endif
