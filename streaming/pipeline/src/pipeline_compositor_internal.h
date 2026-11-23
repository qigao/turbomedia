#ifndef PIPELINE_COMPOSITOR_INTERNAL_H
#define PIPELINE_COMPOSITOR_INTERNAL_H

#include <turbo_pipeline_compositor.h>

int pipeline_compositor_claim(turbo_pipeline_compositor_t *compositor);
void pipeline_compositor_unbind(turbo_pipeline_compositor_t *compositor);
void pipeline_compositor_canvas(const turbo_pipeline_compositor_t *compositor,
                               int *width, int *height);
int pipeline_compositor_main_tile(const turbo_pipeline_compositor_t *compositor,
                                  int *width, int *height);
turbo_compositor_input_color_t pipeline_compositor_main_color(
    const turbo_pipeline_compositor_t *compositor);
int pipeline_compositor_render(const turbo_pipeline_video_frame_t *frame,
                               void *context);

#endif
