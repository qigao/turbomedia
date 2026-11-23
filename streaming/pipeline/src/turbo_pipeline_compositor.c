#include "pipeline_compositor_internal.h"
#include "pipeline_compositor_gpu.h"

#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <core/fmt.h>
#include <queue/ring_buffer_spsc.h>
#include <threads/threads.h>

#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>

enum { PIXEL_ALIGNMENT = 32, PIXEL_PADDING = 64 };

typedef struct compositor_pixels {
    uint8_t *data[4];
    int stride[4];
    size_t allocation;
} compositor_pixels_t;

typedef struct compositor_slot {
    compositor_pixels_t pixels;
    int64_t pts_us;
} compositor_slot_t;

typedef struct compositor_source {
    turbo_compositor_source_t config;
    int tile_width, tile_height;
    compositor_slot_t *slots;
    compositor_pixels_t input;
    struct SwsContext *scaler;
    ring_spsc_t ready, free_slots;
    uint8_t *ring_storage;
    uint32_t writing, held;
    int64_t last_pts_us;
    atomic_size_t used;
    atomic_uint_fast64_t submitted, full, invalid, expired, missing, peak;
} compositor_source_t;

struct turbo_pipeline_compositor {
    int width, height;
    turbo_compositor_input_color_t main_color;
    int64_t main_anchor_pts, main_anchor_us, last_main_us;
    size_t memory_limit, memory_used;
    compositor_layer_t *layers;
    size_t layer_count;
    sync_mutex_t positions_mutex;
    turbo_compositor_position_t *active_positions, *pending_positions;
    bool positions_pending;
    void *sources_allocation;
    compositor_source_t *sources;
    size_t source_count;
    compositor_pixels_t main_video;
    int main_width, main_height;
    compositor_gpu_t *gpu;
    bool histogram_ready;
    turbo_compositor_luma_t *luma;
    atomic_bool bound, closed;
};

static turbo_pipeline_status_t compositor_error(
    turbo_pipeline_error_t *error, turbo_pipeline_status_t code,
    const char *message) {
    if (error) {
        memset(error, 0, sizeof(*error));
        error->code = code;
        fmt(error->message, sizeof(error->message), "{}", message);
    }
    return code;
}

static int reserve_memory(turbo_pipeline_compositor_t *compositor, size_t bytes) {
    if (bytes > compositor->memory_limit - compositor->memory_used) return 0;
    compositor->memory_used += bytes;
    return 1;
}

static void *allocate(turbo_pipeline_compositor_t *compositor,
                      size_t count, size_t size) {
    if (count > SIZE_MAX / size || !reserve_memory(compositor, count * size)) return NULL;
    return calloc(count, size);
}

static int valid_size(int width, int height) {
    return width >= 2 && height >= 2 && !(width & 1) && !(height & 1) &&
           av_image_check_size((unsigned)width, (unsigned)height, 0, NULL) == 0;
}

static int valid_position(const turbo_pipeline_compositor_t *compositor,
                          const turbo_compositor_rect_t *rect,
                          turbo_compositor_position_t position) {
    return position.x >= 0 && position.y >= 0 && !(position.x & 1) && !(position.y & 1) &&
           position.x <= compositor->width - rect->width &&
           position.y <= compositor->height - rect->height;
}

static int allocate_pixels(turbo_pipeline_compositor_t *compositor,
                           compositor_pixels_t *pixels, enum AVPixelFormat format,
                           int width, int height) {
    int size = av_image_get_buffer_size(format, width, height, PIXEL_ALIGNMENT);
    uint8_t *storage;
    if (size < 0 || !reserve_memory(compositor, (size_t)size + PIXEL_PADDING))
        return 0;
    storage = av_mallocz((size_t)size + PIXEL_PADDING);
    if (!storage) return 0;
    if (av_image_fill_arrays(pixels->data, pixels->stride, storage,
                             format, width, height, PIXEL_ALIGNMENT) < 0) {
        av_free(storage);
        memset(pixels, 0, sizeof(*pixels));
        return 0;
    }
    pixels->allocation = (size_t)size + PIXEL_PADDING;
    return 1;
}

static void release_pixels(turbo_pipeline_compositor_t *compositor,
                           compositor_pixels_t *pixels) {
    av_freep(&pixels->data[0]);
    compositor->memory_used -= pixels->allocation;
}

static struct SwsContext *make_scaler(int sw, int sh, int dw, int dh,
                                      enum AVPixelFormat format) {
    struct SwsContext *context =
        sws_getContext(sw, sh, format, dw, dh, format, SWS_BILINEAR, NULL, NULL, NULL);
    if (context && format == AV_PIX_FMT_YUV420P) {
        const int *bt709 = sws_getCoefficients(SWS_CS_ITU709);
        if (sws_setColorspaceDetails(context, bt709, 0, bt709, 0, 0,
                                     1 << 16, 1 << 16) < 0) {
            sws_freeContext(context);
            return NULL;
        }
    }
    return context;
}

static int valid_plane(const turbo_compositor_plane_t *plane,
                       size_t row_bytes, size_t rows) {
    size_t stride, offset;
    if (!plane->data || plane->size > (size_t)PTRDIFF_MAX ||
        plane->first_row > plane->size || plane->stride == PTRDIFF_MIN)
        return 0;
    stride = (size_t)(plane->stride < 0 ? -plane->stride : plane->stride);
    if (stride < row_bytes || (rows - 1u) > (size_t)PTRDIFF_MAX / stride)
        return 0;
    offset = (rows - 1u) * stride;
    if (plane->stride < 0)
        return offset <= plane->first_row &&
               row_bytes <= plane->size - plane->first_row;
    return offset <= plane->size - plane->first_row &&
           row_bytes <= plane->size - plane->first_row - offset;
}

static const uint8_t *input_row(const turbo_compositor_plane_t *plane, int row) {
    return plane->data + plane->first_row + (ptrdiff_t)row * plane->stride;
}

static turbo_compositor_rect_t fit_rect(turbo_compositor_rect_t box, int w, int h) {
    turbo_compositor_rect_t fit = box;
    if ((int64_t)w * box.height > (int64_t)h * box.width) {
        fit.height = (int)((int64_t)box.width * h / w) & ~1;
        fit.y += ((box.height - fit.height) / 2) & ~1;
    } else {
        fit.width = (int)((int64_t)box.height * w / h) & ~1;
        fit.x += ((box.width - fit.width) / 2) & ~1;
    }
    return fit;
}

static unsigned srgb_to_bt709(unsigned v) {
    double s = v / 255.0;
    double linear = s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
    double bt709 = linear < 0.018 ? linear * 4.5 : 1.099 * pow(linear, 0.45) - 0.099;
    return (unsigned)(bt709 * 255.0 + 0.5);
}

static unsigned unpremultiply(unsigned value, unsigned alpha) {
    unsigned raw = alpha ? (value * 255 + alpha / 2) / alpha : 0;
    return raw <= 255 ? raw : 255;
}

static void compile_image(compositor_layer_t *layer, const compositor_pixels_t *rgba) {
    int width = layer->image_rect.width;
    int height = layer->image_rect.height;
    int y, x;
    for (y = 0; y < height; ++y) {
        const uint8_t *row = rgba->data[0] + (ptrdiff_t)y * rgba->stride[0];
        for (x = 0; x < width; ++x, row += 4) {
            unsigned a = (row[3] * layer->opacity + 127u) / 255u;
            double r = unpremultiply(row[0], row[3]) / 255.0;
            double g = unpremultiply(row[1], row[3]) / 255.0;
            double b = unpremultiply(row[2], row[3]) / 255.0;
            double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            size_t pixel = ((size_t)y * width + x) * 2;
            compositor_chroma_t *chroma =
                &layer->chroma[(size_t)(y / 2) * (width / 2) + x / 2];
            layer->luma_alpha[pixel] = (uint8_t)(16.5 + 219.0 * luma);
            layer->luma_alpha[pixel + 1] = (uint8_t)a;
            chroma->alpha += a;
            chroma->u += a * (unsigned)(128.5 + 112.0 * (b - luma) / 0.9278);
            chroma->v += a * (unsigned)(128.5 + 112.0 * (r - luma) / 0.7874);
        }
    }
}

static turbo_pipeline_status_t prepare_image(turbo_pipeline_compositor_t *compositor,
                                              compositor_layer_t *layer,
                                              const turbo_compositor_layer_t *desc) {
    compositor_pixels_t source = {0}, resized = {0};
    struct SwsContext *scaler = NULL;
    turbo_pipeline_status_t status = TURBO_PIPELINE_ENOMEM;
    unsigned lut[256];
    int w = desc->input.image.width;
    int h = desc->input.image.height;
    const turbo_compositor_plane_t *plane = &desc->input.image.rgba;
    size_t count;
    int x, y;

    if (w <= 0 || h <= 0 || av_image_check_size((unsigned)w, (unsigned)h, 0, NULL) < 0 ||
        !valid_plane(plane, (size_t)w * 4, (size_t)h))
        return TURBO_PIPELINE_EINVAL;
    layer->image_rect = fit_rect(layer->rect, w, h);
    if (!valid_size(layer->image_rect.width, layer->image_rect.height))
        return TURBO_PIPELINE_ECONFIG;
    count = (size_t)layer->image_rect.width * layer->image_rect.height;
    layer->luma_alpha = allocate(compositor, count, 2);
    layer->chroma = allocate(compositor, count / 4, sizeof(*layer->chroma));
    if (!layer->luma_alpha || !layer->chroma ||
        !allocate_pixels(compositor, &source, AV_PIX_FMT_RGBA, w, h))
        goto done;

    for (x = 0; x < 256; ++x) lut[x] = srgb_to_bt709((unsigned)x);
    for (y = 0; y < h; ++y) {
        const uint8_t *in = input_row(plane, y);
        uint8_t *out = source.data[0] + (ptrdiff_t)y * source.stride[0];
        for (x = 0; x < w; ++x, in += 4, out += 4) {
            out[0] = (uint8_t)((lut[in[0]] * in[3] + 127u) / 255u);
            out[1] = (uint8_t)((lut[in[1]] * in[3] + 127u) / 255u);
            out[2] = (uint8_t)((lut[in[2]] * in[3] + 127u) / 255u);
            out[3] = in[3];
        }
    }
    if (w == layer->image_rect.width && h == layer->image_rect.height) {
        compile_image(layer, &source);
    } else {
        if (!allocate_pixels(compositor, &resized, AV_PIX_FMT_RGBA,
                              layer->image_rect.width, layer->image_rect.height))
            goto done;
        scaler = make_scaler(w, h, layer->image_rect.width, layer->image_rect.height,
                             AV_PIX_FMT_RGBA);
        if (!scaler) goto done;
        if (sws_scale(scaler, (const uint8_t *const *)source.data, source.stride,
                      0, h, resized.data, resized.stride) != layer->image_rect.height) {
            status = TURBO_PIPELINE_EFFMPEG;
            goto done;
        }
        compile_image(layer, &resized);
    }
    status = TURBO_PIPELINE_OK;
done:
    sws_freeContext(scaler);
    release_pixels(compositor, &source);
    release_pixels(compositor, &resized);
    return status;
}

static int push_index(ring_spsc_t *ring, uint32_t index) {
    uint8_t *write = ring_spsc_write_acquire(ring, sizeof(index));
    if (!write) return 0;
    memcpy(write, &index, sizeof(index));
    ring_spsc_write_release(ring, sizeof(index));
    return 1;
}

static uint32_t peek_index(ring_spsc_t *ring) {
    size_t bytes = 0;
    uint8_t *read = ring_spsc_read_acquire(ring, &bytes);
    uint32_t index = UINT32_MAX;
    if (read && bytes >= sizeof(index)) memcpy(&index, read, sizeof(index));
    return index;
}

static turbo_pipeline_status_t prepare_source(turbo_pipeline_compositor_t *compositor,
    compositor_source_t *source, const turbo_compositor_source_t *config,
    const turbo_compositor_rect_t *rect) {
    size_t capacity = sizeof(uint32_t);
    size_t i;
    if (!valid_size(config->width, config->height) || config->slots < 2 ||
        config->slots >= UINT32_MAX || config->slots >= SIZE_MAX / sizeof(uint32_t) ||
        config->max_age_us < 0)
        return TURBO_PIPELINE_ECONFIG;
    while (capacity <= config->slots * sizeof(uint32_t)) {
        if (capacity > SIZE_MAX / 2) return TURBO_PIPELINE_ECONFIG;
        capacity *= 2;
    }
    source->config = *config;
    source->tile_width = rect->width;
    source->tile_height = rect->height;
    source->writing = UINT32_MAX;
    source->held = UINT32_MAX;
    source->last_pts_us = INT64_MIN;
    atomic_init(&source->used, 0);
    atomic_init(&source->submitted, 0);
    atomic_init(&source->full, 0);
    atomic_init(&source->invalid, 0);
    atomic_init(&source->expired, 0);
    atomic_init(&source->missing, 0);
    atomic_init(&source->peak, 0);
    source->slots = allocate(compositor, config->slots, sizeof(*source->slots));
    source->ring_storage = allocate(compositor, 2, capacity);
    if (!source->slots || !source->ring_storage) return TURBO_PIPELINE_ENOMEM;
    if (!ring_spsc_init(&source->free_slots, source->ring_storage, capacity) ||
        !ring_spsc_init(&source->ready, source->ring_storage + capacity, capacity))
        return TURBO_PIPELINE_ECONFIG;
    if (config->width != rect->width || config->height != rect->height) {
        if (!allocate_pixels(compositor, &source->input, AV_PIX_FMT_YUV420P,
                              config->width, config->height))
            return TURBO_PIPELINE_ENOMEM;
        source->scaler = make_scaler(config->width, config->height, rect->width,
                                     rect->height, AV_PIX_FMT_YUV420P);
        if (!source->scaler) return TURBO_PIPELINE_EFFMPEG;
    }
    for (i = 0; i < config->slots; ++i) {
        if (!allocate_pixels(compositor, &source->slots[i].pixels, AV_PIX_FMT_YUV420P,
                              rect->width, rect->height))
            return TURBO_PIPELINE_ENOMEM;
        if (!push_index(&source->free_slots, (uint32_t)i)) return TURBO_PIPELINE_ESTATE;
    }
    return TURBO_PIPELINE_OK;
}

turbo_pipeline_compositor_t *turbo_pipeline_compositor_create(
    const turbo_compositor_config_t *config, turbo_pipeline_error_t *error) {
    return turbo_pipeline_compositor_create_ex(config, NULL, error);
}

turbo_pipeline_compositor_t *turbo_pipeline_compositor_create_ex(
    const turbo_compositor_config_t *config, const turbo_compositor_options_t *options,
    turbo_pipeline_error_t *error) {
    turbo_pipeline_compositor_t *compositor;
    turbo_pipeline_status_t status = TURBO_PIPELINE_ECONFIG;
    size_t i, sources_prepared = 0;
    const char *failure = NULL;
    if (options && (options->size < sizeof(*options) ||
        (options->backend != TURBO_COMPOSITOR_CPU && options->backend != TURBO_COMPOSITOR_D3D11) ||
        (options->luma_histogram != 0 && options->luma_histogram != 1))) {
        compositor_error(error, TURBO_PIPELINE_EINVAL, "invalid compositor options");
        return NULL;
    }
    int use_gpu = options && options->backend == TURBO_COMPOSITOR_D3D11;
#ifndef _WIN32
    if (use_gpu) {
        compositor_error(error, TURBO_PIPELINE_ECONFIG, "the D3D11 compositor requires Windows");
        return NULL;
    }
#endif
    if (!config || !valid_size(config->width, config->height) ||
        !config->layers || !config->layer_count || config->source_count > UINT32_MAX ||
        (config->source_count && (!config->sources ||
          config->main_anchor_pts == INT64_MIN || config->main_anchor_us == INT64_MIN)) ||
        config->main_color < TURBO_COMPOSITOR_COLOR_FROM_FRAME ||
        config->main_color > TURBO_COMPOSITOR_COLOR_BT601_LIMITED) {
        compositor_error(error, TURBO_PIPELINE_EINVAL, "invalid compositor configuration");
        return NULL;
    }
    if (config->memory_limit < sizeof(*compositor) ||
        !(compositor = calloc(1, sizeof(*compositor)))) {
        compositor_error(error, TURBO_PIPELINE_ENOMEM, "cannot allocate compositor");
        return NULL;
    }
    compositor->width = config->width;
    compositor->height = config->height;
    compositor->main_color = config->main_color;
    compositor->main_anchor_pts = config->main_anchor_pts;
    compositor->main_anchor_us = config->main_anchor_us;
    compositor->last_main_us = INT64_MIN;
    compositor->memory_limit = config->memory_limit;
    compositor->memory_used = sizeof(*compositor);
    atomic_init(&compositor->bound, false);
    atomic_init(&compositor->closed, false);
    sync_mutex_init(&compositor->positions_mutex);
    if (!compositor->positions_mutex) goto memory;
    if (options && options->luma_histogram) {
        compositor->luma = allocate(compositor, 1, sizeof(*compositor->luma));
        if (!compositor->luma) goto memory;
    }
    compositor->layers = allocate(compositor, config->layer_count, sizeof(*compositor->layers));
    if (!compositor->layers) goto memory;
    compositor->layer_count = config->layer_count;
    compositor->active_positions = allocate(compositor, config->layer_count,
                                            sizeof(*compositor->active_positions));
    compositor->pending_positions = allocate(compositor, config->layer_count,
                                             sizeof(*compositor->pending_positions));
    if (!compositor->active_positions || !compositor->pending_positions) goto memory;
    if (config->source_count) {
        size_t alignment = _Alignof(compositor_source_t);
        uintptr_t address;
        if (config->source_count > (SIZE_MAX - alignment + 1) / sizeof(compositor_source_t))
            goto memory;
        compositor->sources_allocation = allocate(compositor, 1,
            config->source_count * sizeof(compositor_source_t) + alignment - 1);
        if (!compositor->sources_allocation) goto memory;
        address = (uintptr_t)compositor->sources_allocation;
        compositor->sources = (void *)(address + ((alignment - address % alignment) % alignment));
        compositor->source_count = config->source_count;
    }

    for (i = 0; i < config->layer_count; ++i) {
        const turbo_compositor_layer_t *desc = &config->layers[i];
        turbo_compositor_rect_t r = desc->rect;
        turbo_compositor_position_t position = {r.x, r.y};
        compositor_layer_t *layer = &compositor->layers[i];
        status = TURBO_PIPELINE_ECONFIG;
        if (!valid_size(r.width, r.height) ||
            !valid_position(compositor, &r, position)) goto fail;
        compositor->active_positions[i] = position;
        layer->kind = desc->kind;
        layer->opacity = desc->opacity;
        layer->rect = layer->image_rect = r;
        switch (desc->kind) {
        case TURBO_COMPOSITOR_MAIN_VIDEO:
            if (compositor->main_width) goto fail;
            if (!use_gpu && !allocate_pixels(compositor, &compositor->main_video, AV_PIX_FMT_YUV420P,
                                              r.width, r.height))
                goto memory;
            compositor->main_width = r.width;
            compositor->main_height = r.height;
            break;
        case TURBO_COMPOSITOR_RGBA_IMAGE:
            status = prepare_image(compositor, layer, desc);
            if (status != TURBO_PIPELINE_OK) goto fail;
            break;
        case TURBO_COMPOSITOR_AUX_VIDEO: {
            const turbo_compositor_source_t *source_config;
            layer->source = desc->input.source;
            if (layer->source >= config->source_count ||
                compositor->sources[layer->source].slots) goto fail;
            source_config = &config->sources[layer->source];
            if (!valid_size(source_config->width, source_config->height)) goto fail;
            layer->image_rect = fit_rect(r, source_config->width, source_config->height);
            if (!valid_size(layer->image_rect.width, layer->image_rect.height)) goto fail;
            status = prepare_source(compositor, &compositor->sources[layer->source],
                                    source_config, &layer->image_rect);
            if (status != TURBO_PIPELINE_OK) goto fail;
            ++sources_prepared;
            break;
        }
        default:
            goto fail;
        }
    }
    if (sources_prepared != config->source_count) {
        status = TURBO_PIPELINE_ECONFIG;
        goto fail;
    }
#ifdef _WIN32
    if (use_gpu) {
        compositor->gpu = compositor_gpu_create(compositor->width, compositor->height,
            compositor->main_width != 0, compositor->layers, compositor->layer_count,
            compositor->luma != NULL, compositor->memory_limit - compositor->memory_used,
            &status);
        if (!compositor->gpu) {
            failure = status == TURBO_PIPELINE_ECONFIG ? "D3D11 hardware feature level 11 is unavailable"
                    : status == TURBO_PIPELINE_ENOMEM ? "D3D11 compositor allocation or memory budget failed"
                    : "cannot create D3D11 compute resources";
            goto fail;
        }
        /* The immutable GPU buffer owns the prepared image data now. */
        for (i = 0; i < compositor->layer_count; ++i) {
            free(compositor->layers[i].luma_alpha);
            free(compositor->layers[i].chroma);
            compositor->layers[i].luma_alpha = NULL;
            compositor->layers[i].chroma = NULL;
        }
    }
#endif
    if (error) memset(error, 0, sizeof(*error));
    return compositor;
memory:
    status = TURBO_PIPELINE_ENOMEM;
fail:
    (void)turbo_pipeline_compositor_destroy(compositor);
    if (!failure) failure = status == TURBO_PIPELINE_ENOMEM
        ? "compositor allocation or memory budget failed" : "cannot prepare compositor layout";
    compositor_error(error, status, failure);
    return NULL;
}

turbo_pipeline_status_t turbo_pipeline_compositor_luma(turbo_pipeline_compositor_t *compositor,
                                                        turbo_compositor_luma_t *result) {
    if (!compositor || !result) return TURBO_PIPELINE_EINVAL;
    sync_mutex_lock(&compositor->positions_mutex);
    bool ready = compositor->histogram_ready;
    if (ready) *result = *compositor->luma;
    sync_mutex_unlock(&compositor->positions_mutex);
    return ready ? TURBO_PIPELINE_OK : TURBO_PIPELINE_ESTATE;
}

turbo_pipeline_status_t turbo_pipeline_compositor_set_positions(
    turbo_pipeline_compositor_t *compositor,
    const turbo_compositor_position_t *positions, size_t count) {
    size_t i;
    if (!compositor || !positions || count != compositor->layer_count)
        return TURBO_PIPELINE_EINVAL;
    for (i = 0; i < count; ++i)
        if (!valid_position(compositor, &compositor->layers[i].rect, positions[i]))
            return TURBO_PIPELINE_EINVAL;

    sync_mutex_lock(&compositor->positions_mutex);
    if (atomic_load_explicit(&compositor->closed, memory_order_acquire)) {
        sync_mutex_unlock(&compositor->positions_mutex);
        return TURBO_PIPELINE_ESTATE;
    }
    memcpy(compositor->pending_positions, positions, count * sizeof(*positions));
    compositor->positions_pending = true;
    sync_mutex_unlock(&compositor->positions_mutex);
    return TURBO_PIPELINE_OK;
}

int pipeline_compositor_claim(turbo_pipeline_compositor_t *compositor) {
    bool unbound = false;
    return !atomic_load_explicit(&compositor->closed, memory_order_acquire) &&
        atomic_compare_exchange_strong(&compositor->bound, &unbound, true);
}

void pipeline_compositor_unbind(turbo_pipeline_compositor_t *compositor) {
    turbo_pipeline_compositor_close(compositor);
    atomic_store_explicit(&compositor->bound, false, memory_order_release);
}

void pipeline_compositor_canvas(const turbo_pipeline_compositor_t *compositor,
                               int *width, int *height) {
    *width = compositor->width;
    *height = compositor->height;
}

int pipeline_compositor_main_tile(const turbo_pipeline_compositor_t *compositor,
                                  int *width, int *height) {
    *width = compositor->main_width;
    *height = compositor->main_height;
    return compositor->main_width > 0;
}

turbo_compositor_input_color_t pipeline_compositor_main_color(
    const turbo_pipeline_compositor_t *compositor) {
    return compositor->main_color;
}

turbo_pipeline_status_t turbo_pipeline_compositor_submit(
    turbo_pipeline_compositor_t *compositor, uint32_t index,
    const turbo_compositor_plane_t planes[3], int64_t pts_us) {
    compositor_source_t *source;
    compositor_slot_t *slot;
    compositor_pixels_t *destination;
    size_t used;
    int i, row;
    if (!compositor || !planes || index >= compositor->source_count)
        return TURBO_PIPELINE_EINVAL;
    if (atomic_load_explicit(&compositor->closed, memory_order_acquire))
        return TURBO_PIPELINE_ESTATE;
    source = &compositor->sources[index];
    if (pts_us == INT64_MIN || pts_us < source->last_pts_us) goto invalid;
    for (i = 0; i < 3; ++i) {
        if (!valid_plane(&planes[i], (size_t)(source->config.width >> (i != 0)),
                         (size_t)(source->config.height >> (i != 0))))
            goto invalid;
    }
    if (source->writing == UINT32_MAX) {
        source->writing = peek_index(&source->free_slots);
        if (source->writing == UINT32_MAX) {
            atomic_fetch_add_explicit(&source->full, 1, memory_order_relaxed);
            return TURBO_PIPELINE_EBACKPRESSURE;
        }
        ring_spsc_read_release(&source->free_slots, sizeof(source->writing));
    }
    slot = &source->slots[source->writing];
    destination = source->scaler ? &source->input : &slot->pixels;
    for (i = 0; i < 3; ++i) {
        int width = source->config.width >> (i != 0);
        int height = source->config.height >> (i != 0);
        for (row = 0; row < height; ++row)
            memcpy(destination->data[i] + (ptrdiff_t)row * destination->stride[i],
                   input_row(&planes[i], row), (size_t)width);
    }
    if (source->scaler &&
        sws_scale(source->scaler, (const uint8_t *const *)source->input.data,
                  source->input.stride, 0, source->config.height, slot->pixels.data,
                  slot->pixels.stride) != source->tile_height)
        return TURBO_PIPELINE_EFFMPEG;
    slot->pts_us = pts_us;
    used = atomic_fetch_add_explicit(&source->used, 1, memory_order_relaxed) + 1;
    if (!push_index(&source->ready, source->writing)) {
        atomic_fetch_sub_explicit(&source->used, 1, memory_order_relaxed);
        return TURBO_PIPELINE_ESTATE;
    }
    if (used > atomic_load_explicit(&source->peak, memory_order_relaxed))
        atomic_store_explicit(&source->peak, used, memory_order_relaxed);
    source->writing = UINT32_MAX;
    source->last_pts_us = pts_us;
    atomic_fetch_add_explicit(&source->submitted, 1, memory_order_relaxed);
    return TURBO_PIPELINE_OK;
invalid:
    atomic_fetch_add_explicit(&source->invalid, 1, memory_order_relaxed);
    return TURBO_PIPELINE_EINVAL;
}

turbo_pipeline_status_t turbo_pipeline_compositor_stats(
    const turbo_pipeline_compositor_t *compositor, uint32_t index,
    turbo_compositor_stats_t *stats) {
    const compositor_source_t *source;
    if (!compositor || !stats || index >= compositor->source_count)
        return TURBO_PIPELINE_EINVAL;
    source = &compositor->sources[index];
    stats->submitted = atomic_load_explicit(&source->submitted, memory_order_relaxed);
    stats->full = atomic_load_explicit(&source->full, memory_order_relaxed);
    stats->invalid = atomic_load_explicit(&source->invalid, memory_order_relaxed);
    stats->expired = atomic_load_explicit(&source->expired, memory_order_relaxed);
    stats->missing = atomic_load_explicit(&source->missing, memory_order_relaxed);
    stats->peak_slots = atomic_load_explicit(&source->peak, memory_order_relaxed);
    return TURBO_PIPELINE_OK;
}

static int release_held(compositor_source_t *source) {
    if (source->held == UINT32_MAX) return 1;
    atomic_fetch_sub_explicit(&source->used, 1, memory_order_relaxed);
    if (!push_index(&source->free_slots, source->held)) return 0;
    source->held = UINT32_MAX;
    return 1;
}

static int select_frame(compositor_source_t *source, int64_t now) {
    size_t i;
    for (i = 0; i < source->config.slots; ++i) {
        uint32_t next = peek_index(&source->ready);
        if (next == UINT32_MAX || source->slots[next].pts_us > now) break;
        if (!release_held(source)) return 0;
        ring_spsc_read_release(&source->ready, sizeof(next));
        source->held = next;
    }
    if (source->held != UINT32_MAX &&
        (uint64_t)now - (uint64_t)source->slots[source->held].pts_us >
            (uint64_t)source->config.max_age_us) {
        atomic_fetch_add_explicit(&source->expired, 1, memory_order_relaxed);
        if (!release_held(source)) return 0;
    }
    if (source->held == UINT32_MAX) {
        atomic_fetch_add_explicit(&source->missing, 1, memory_order_relaxed);
        return !source->config.required;
    }
    return 1;
}

static int main_time(turbo_pipeline_compositor_t *compositor,
                     const turbo_pipeline_video_frame_t *frame, int64_t *now) {
    int64_t delta, base = compositor->main_anchor_pts;
    int64_t scaled, origin = compositor->main_anchor_us;
    if (frame->pts == INT64_MIN || frame->time_base_num <= 0 || frame->time_base_den <= 0 ||
        (base > 0 && frame->pts < INT64_MIN + base) ||
        (base < 0 && frame->pts > INT64_MAX + base))
        return 0;
    delta = frame->pts - base;
    if (delta == INT64_MIN) return 0;
    scaled = av_rescale_q(delta, (AVRational){frame->time_base_num, frame->time_base_den},
                          (AVRational){1, 1000000});
    if (scaled == INT64_MIN ||
        (scaled > 0 && origin > INT64_MAX - scaled) ||
        (scaled < 0 && origin < INT64_MIN - scaled))
        return 0;
    *now = origin + scaled;
    if (*now == INT64_MIN || *now < compositor->last_main_us) return 0;
    compositor->last_main_us = *now;
    return 1;
}

static uint8_t blend(unsigned src, unsigned dst, unsigned alpha) {
    return (uint8_t)((alpha * src + (255 - alpha) * dst + 127) / 255);
}

static uint8_t *canvas_row(const turbo_pipeline_video_frame_t *frame,
                           int plane, turbo_compositor_rect_t rect, int row) {
    int shift = plane != 0;
    return frame->planes[plane].data +
           (ptrdiff_t)((rect.y >> shift) + row) * frame->planes[plane].stride +
           (rect.x >> shift);
}

static void draw_video(const turbo_pipeline_video_frame_t *frame,
                       turbo_compositor_rect_t rect,
                       const compositor_pixels_t *source, unsigned opacity) {
    int i, row, x;
    if (!opacity || !rect.width || !rect.height) return;
    for (i = 0; i < 3; ++i) {
        int width = rect.width >> (i != 0);
        int height = rect.height >> (i != 0);
        unsigned background = i == 0 ? 16u : 128u;
        for (row = 0; row < height; ++row) {
            uint8_t *out = canvas_row(frame, i, rect, row);
            const uint8_t *in = source ? source->data[i] + (ptrdiff_t)row * source->stride[i] : NULL;
            if (opacity == 255 && in) memcpy(out, in, (size_t)width);
            else if (opacity == 255) memset(out, (int)background, (size_t)width);
            else {
                for (x = 0; x < width; ++x)
                    out[x] = blend(in ? in[x] : background, out[x], opacity);
            }
        }
    }
}

static void draw_letterbox(const turbo_pipeline_video_frame_t *frame,
                            const compositor_layer_t *layer) {
    turbo_compositor_rect_t box = layer->rect, image = layer->image_rect;
    draw_video(frame, (turbo_compositor_rect_t){box.x, box.y, box.width, image.y - box.y},
               NULL, layer->opacity);
    draw_video(frame, (turbo_compositor_rect_t){box.x, image.y + image.height,
               box.width, box.y + box.height - image.y - image.height}, NULL, layer->opacity);
    draw_video(frame, (turbo_compositor_rect_t){box.x, image.y, image.x - box.x, image.height},
               NULL, layer->opacity);
    draw_video(frame, (turbo_compositor_rect_t){image.x + image.width, image.y,
               box.x + box.width - image.x - image.width, image.height}, NULL, layer->opacity);
}

static void draw_image(const turbo_pipeline_video_frame_t *frame,
                       const compositor_layer_t *layer) {
    turbo_compositor_rect_t r = layer->image_rect;
    int x, y;
    for (y = 0; y < r.height; ++y) {
        uint8_t *out = canvas_row(frame, 0, r, y);
        const uint8_t *in = layer->luma_alpha + (size_t)y * r.width * 2;
        for (x = 0; x < r.width; ++x, in += 2)
            out[x] = blend(in[0], out[x], in[1]);
    }
    for (y = 0; y < r.height / 2; ++y) {
        uint8_t *u = canvas_row(frame, 1, r, y);
        uint8_t *v = canvas_row(frame, 2, r, y);
        const compositor_chroma_t *in = layer->chroma + (size_t)y * (r.width / 2);
        for (x = 0; x < r.width / 2; ++x) {
            unsigned keep = 1020u - in[x].alpha;
            u[x] = (uint8_t)((in[x].u + keep * u[x] + 510u) / 1020u);
            v[x] = (uint8_t)((in[x].v + keep * v[x] + 510u) / 1020u);
        }
    }
}

static compositor_layer_t positioned_layer(const turbo_pipeline_compositor_t *compositor, size_t i) {
    compositor_layer_t layer = compositor->layers[i];
    turbo_compositor_position_t position = compositor->active_positions[i];
    layer.image_rect.x += position.x - layer.rect.x;
    layer.image_rect.y += position.y - layer.rect.y;
    layer.rect.x = position.x;
    layer.rect.y = position.y;
    return layer;
}

static void store_histogram(turbo_pipeline_compositor_t *compositor,
    const turbo_pipeline_video_frame_t *frame, const uint32_t bins[256]) {
    sync_mutex_lock(&compositor->positions_mutex);
    compositor->luma->pts = frame->pts;
    compositor->luma->time_base_num = frame->time_base_num;
    compositor->luma->time_base_den = frame->time_base_den;
    memcpy(compositor->luma->bins, bins, sizeof(compositor->luma->bins));
    compositor->histogram_ready = true;
    sync_mutex_unlock(&compositor->positions_mutex);
}

#ifdef _WIN32
static int render_gpu(turbo_pipeline_compositor_t *compositor,
                      const turbo_pipeline_video_frame_t *frame) {
    compositor_gpu_t *gpu = compositor->gpu;
    uint32_t bins[256];
    if (!compositor_gpu_begin(gpu, frame)) return TURBO_PIPELINE_EIO;
    for (size_t i = 0; i < compositor->layer_count; ++i) {
        compositor_layer_t layer = positioned_layer(compositor, i);
        if (layer.kind == TURBO_COMPOSITOR_MAIN_VIDEO) {
            const uint8_t *data[] = {frame->planes[0].data, frame->planes[1].data, frame->planes[2].data};
            const ptrdiff_t strides[] = {frame->planes[0].stride, frame->planes[1].stride, frame->planes[2].stride};
            compositor_gpu_layer(gpu, i, &layer, data, strides);
        } else if (layer.kind == TURBO_COMPOSITOR_AUX_VIDEO &&
                   compositor->sources[layer.source].held != UINT32_MAX) {
            const compositor_source_t *source = &compositor->sources[layer.source];
            const compositor_pixels_t *pixels = &source->slots[source->held].pixels;
            const uint8_t *data[] = {pixels->data[0], pixels->data[1], pixels->data[2]};
            const ptrdiff_t strides[] = {pixels->stride[0], pixels->stride[1], pixels->stride[2]};
            compositor_gpu_layer(gpu, i, &layer, data, strides);
        } else {
            compositor_gpu_layer(gpu, i, &layer, NULL, NULL);
        }
    }
    if (!compositor_gpu_end(gpu, frame, bins)) return TURBO_PIPELINE_EIO;
    if (compositor->luma) store_histogram(compositor, frame, bins);
    return TURBO_PIPELINE_OK;
}
#endif

int pipeline_compositor_render(const turbo_pipeline_video_frame_t *frame, void *context) {
    turbo_pipeline_compositor_t *compositor = context;
    size_t i;
    int64_t now;
    if (frame->width != compositor->width || frame->height != compositor->height)
        return TURBO_PIPELINE_ECONFIG;
    sync_mutex_lock(&compositor->positions_mutex);
    if (compositor->positions_pending) {
        turbo_compositor_position_t *previous = compositor->active_positions;
        compositor->active_positions = compositor->pending_positions;
        compositor->pending_positions = previous;
        compositor->positions_pending = false;
    }
    sync_mutex_unlock(&compositor->positions_mutex);

    if (compositor->source_count) {
        if (!main_time(compositor, frame, &now)) return TURBO_PIPELINE_EINVAL;
        for (i = 0; i < compositor->source_count; ++i)
            if (!select_frame(&compositor->sources[i], now)) return TURBO_PIPELINE_EIO;
    }
#ifdef _WIN32
    if (compositor->gpu) return render_gpu(compositor, frame);
#endif
    if (compositor->main_width) {
        /* The filter has fitted the main tile in the upper-left of the canvas.
         * Keep it before clearing the canvas and drawing at its active position. */
        for (i = 0; i < 3; ++i) {
            int width = compositor->main_width >> (i != 0);
            int height = compositor->main_height >> (i != 0);
            int row;
            for (row = 0; row < height; ++row)
                memcpy(compositor->main_video.data[i] +
                           (ptrdiff_t)row * compositor->main_video.stride[i],
                       frame->planes[i].data + (ptrdiff_t)row * frame->planes[i].stride,
                       (size_t)width);
        }
        draw_video(frame, (turbo_compositor_rect_t){0, 0, frame->width, frame->height},
                   NULL, 255);
    }
    for (i = 0; i < compositor->layer_count; ++i) {
        compositor_layer_t layer = positioned_layer(compositor, i);
        if (layer.kind == TURBO_COMPOSITOR_RGBA_IMAGE) {
            draw_image(frame, &layer);
        } else if (layer.kind == TURBO_COMPOSITOR_MAIN_VIDEO) {
            draw_video(frame, layer.rect, &compositor->main_video, layer.opacity);
        } else {
            const compositor_source_t *source = &compositor->sources[layer.source];
            if (source->held == UINT32_MAX) {
                draw_video(frame, layer.rect, NULL, layer.opacity);
            } else {
                draw_letterbox(frame, &layer);
                draw_video(frame, layer.image_rect, &source->slots[source->held].pixels,
                           layer.opacity);
            }
        }
    }
    if (compositor->luma) {
        uint32_t bins[256] = {0};
        for (int y = 0; y < frame->height; ++y) {
            const uint8_t *row = frame->planes[0].data + (ptrdiff_t)y * frame->planes[0].stride;
            for (int x = 0; x < frame->width; ++x) ++bins[row[x]];
        }
        store_histogram(compositor, frame, bins);
    }
    return 0;
}

void turbo_pipeline_compositor_close(turbo_pipeline_compositor_t *compositor) {
    if (compositor) atomic_store_explicit(&compositor->closed, true, memory_order_release);
}

turbo_pipeline_status_t turbo_pipeline_compositor_destroy(
    turbo_pipeline_compositor_t *compositor) {
    size_t i, j;
    if (!compositor) return TURBO_PIPELINE_OK;
    if (atomic_load_explicit(&compositor->bound, memory_order_acquire))
        return TURBO_PIPELINE_ESTATE;
#ifdef _WIN32
    compositor_gpu_destroy(compositor->gpu);
#endif
    for (i = 0; i < compositor->source_count; ++i) {
        compositor_source_t *source = &compositor->sources[i];
        if (source->slots) {
            for (j = 0; j < source->config.slots; ++j)
                release_pixels(compositor, &source->slots[j].pixels);
        }
        release_pixels(compositor, &source->input);
        sws_freeContext(source->scaler);
        free(source->slots);
        free(source->ring_storage);
    }
    for (i = 0; i < compositor->layer_count; ++i) {
        free(compositor->layers[i].luma_alpha);
        free(compositor->layers[i].chroma);
    }
    release_pixels(compositor, &compositor->main_video);
    free(compositor->sources_allocation);
    free(compositor->layers);
    free(compositor->active_positions);
    free(compositor->pending_positions);
    free(compositor->luma);
    sync_mutex_destroy(&compositor->positions_mutex);
    free(compositor);
    return TURBO_PIPELINE_OK;
}
