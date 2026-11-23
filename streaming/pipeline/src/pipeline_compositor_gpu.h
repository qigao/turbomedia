#ifndef PIPELINE_COMPOSITOR_GPU_H
#define PIPELINE_COMPOSITOR_GPU_H

#include <turbo_pipeline_compositor.h>

typedef struct compositor_chroma {
    uint32_t alpha, u, v;
} compositor_chroma_t;

typedef struct compositor_layer {
    turbo_compositor_layer_kind_t kind;
    turbo_compositor_rect_t rect, image_rect;
    uint8_t opacity;
    uint32_t source;
    uint8_t *luma_alpha;
    compositor_chroma_t *chroma;
} compositor_layer_t;

typedef struct compositor_gpu compositor_gpu_t;

compositor_gpu_t *compositor_gpu_create(int width, int height, int main_tile,
    const compositor_layer_t *layers, size_t layer_count, int histogram,
    size_t memory_limit, turbo_pipeline_status_t *status);
void compositor_gpu_destroy(compositor_gpu_t *gpu);
int compositor_gpu_begin(compositor_gpu_t *gpu, const turbo_pipeline_video_frame_t *frame);
void compositor_gpu_layer(compositor_gpu_t *gpu, size_t i, const compositor_layer_t *layer,
                          const uint8_t *const data[3], const ptrdiff_t strides[3]);
int compositor_gpu_end(compositor_gpu_t *gpu, const turbo_pipeline_video_frame_t *frame,
                       uint32_t *histogram);

#endif
