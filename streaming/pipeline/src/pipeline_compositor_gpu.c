#include "pipeline_compositor_gpu.h"

#ifdef _WIN32

#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <stdlib.h>
#include <string.h>
#include "pipeline_compositor_d3d11_shader.h"

enum {
    HEADER_WORDS = 8, LAYER_WORDS = 16, GROUP_WIDTH = 128, HISTOGRAM_BINS = 256,
    MISSING_AUX_VIDEO = 3,
    GPU_BUFFER_LIMIT = 512 * 1024 * 1024 /* 2^27 uint elements in a typed view. */
};

typedef struct gpu_payload {
    uint32_t base, pitch_y, pitch_c;
} gpu_payload_t;

struct compositor_gpu {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11ComputeShader *shader;
    ID3D11Buffer *upload, *output, *readback, *bins, *bins_readback;
    ID3D11ShaderResourceView *input_view, *asset_view;
    ID3D11UnorderedAccessView *output_view, *bins_view;
    uint32_t *mapped;
    int width, height, histogram;
    size_t layer_count;
    gpu_payload_t background, output_layout;
    UINT groups_x, groups_y;
    gpu_payload_t layers[];
};

_Static_assert(sizeof(compositor_chroma_t) == 3 * sizeof(uint32_t),
               "GPU chroma expects three packed uints");

static void release_object(void *object) {
    if (object) IUnknown_Release((IUnknown *)object);
}

static UINT pitch(int width) {
    return ((UINT)width + 3u) & ~3u;
}

static size_t video_bytes(int width, int height) {
    return ((size_t)pitch(width) + pitch(width / 2)) * (unsigned)height;
}

static int advance(size_t *total, size_t bytes) {
    if (bytes > GPU_BUFFER_LIMIT - *total) return 0;
    *total += bytes;
    return 1;
}

static gpu_payload_t video_layout(size_t base, int width) {
    gpu_payload_t layout = {(uint32_t)base, pitch(width), pitch(width / 2)};
    return layout;
}

static void copy_video_to_gpu(uint8_t *mapped, gpu_payload_t layout, int width,
                              int height, const uint8_t *const data[3],
                              const ptrdiff_t stride[3]) {
    uint8_t *destination = mapped + layout.base;
    for (int i = 0; i < 3; ++i) {
        int rows = height >> (i != 0);
        int row_bytes = width >> (i != 0);
        uint32_t row_pitch = i == 0 ? layout.pitch_y : layout.pitch_c;
        for (int y = 0; y < rows; ++y)
            memcpy(destination + (size_t)y * row_pitch, data[i] + (ptrdiff_t)y * stride[i],
                   (size_t)row_bytes);
        destination += (size_t)rows * row_pitch;
    }
}

static void copy_video_from_gpu(const compositor_gpu_t *gpu, const uint8_t *mapped,
                                const turbo_pipeline_video_frame_t *frame) {
    for (int i = 0; i < 3; ++i) {
        int rows = gpu->height >> (i != 0);
        int row_bytes = gpu->width >> (i != 0);
        uint32_t row_pitch = i == 0 ? gpu->output_layout.pitch_y : gpu->output_layout.pitch_c;
        for (int y = 0; y < rows; ++y)
            memcpy(frame->planes[i].data + (ptrdiff_t)y * frame->planes[i].stride,
                   mapped + (size_t)y * row_pitch, (size_t)row_bytes);
        mapped += (size_t)rows * row_pitch;
    }
}

static int create_device(compositor_gpu_t *gpu) {
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    IDXGIFactory1 *factory = NULL;
    SIZE_T dedicated = 0;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory))) return 0;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1 *candidate = NULL;
        DXGI_ADAPTER_DESC1 description;
        if (FAILED(IDXGIFactory1_EnumAdapters1(factory, i, &candidate))) break;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(candidate, &description)) &&
            !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (!gpu->device || description.DedicatedVideoMemory > dedicated)) {
            ID3D11Device *device = NULL;
            ID3D11DeviceContext *context = NULL;
            if (SUCCEEDED(D3D11CreateDevice((IDXGIAdapter *)candidate, D3D_DRIVER_TYPE_UNKNOWN,
                    NULL, 0, &level, 1, D3D11_SDK_VERSION, &device, NULL, &context))) {
                release_object(gpu->context);
                release_object(gpu->device);
                gpu->device = device;
                gpu->context = context;
                dedicated = description.DedicatedVideoMemory;
            }
        }
        release_object(candidate);
    }
    release_object(factory);
    return gpu->device != NULL;
}

static int create_shader(compositor_gpu_t *gpu) {
    ID3DBlob *shader = NULL;
    HRESULT result = D3DCompile(compositor_shader, sizeof(compositor_shader) - 1,
        "compositor", NULL, NULL, "compose", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &shader, NULL);
    if (SUCCEEDED(result))
        result = ID3D11Device_CreateComputeShader(gpu->device, ID3D10Blob_GetBufferPointer(shader),
            ID3D10Blob_GetBufferSize(shader), NULL, &gpu->shader);
    release_object(shader);
    return SUCCEEDED(result);
}

static int make_buffer(compositor_gpu_t *gpu, size_t bytes, D3D11_USAGE usage,
    UINT bind, UINT access, const void *data, ID3D11Buffer **out) {
    D3D11_BUFFER_DESC desc = {0};
    D3D11_SUBRESOURCE_DATA initial = {0};
    desc.ByteWidth = (UINT)bytes;
    desc.Usage = usage;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = access;
    initial.pSysMem = data;
    return SUCCEEDED(ID3D11Device_CreateBuffer(gpu->device, &desc, data ? &initial : NULL, out));
}

static int make_input(compositor_gpu_t *gpu, size_t bytes, const void *data,
                       ID3D11Buffer **buffer, ID3D11ShaderResourceView **view) {
    D3D11_SHADER_RESOURCE_VIEW_DESC desc = {0};
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = (UINT)(bytes / sizeof(uint32_t));
    return make_buffer(gpu, bytes, data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DYNAMIC,
                       D3D11_BIND_SHADER_RESOURCE, data ? 0 : D3D11_CPU_ACCESS_WRITE, data, buffer) &&
        SUCCEEDED(ID3D11Device_CreateShaderResourceView(gpu->device, (ID3D11Resource *)*buffer,
                                                        &desc, view));
}

static int make_output(compositor_gpu_t *gpu, size_t bytes, ID3D11Buffer **buffer,
                        ID3D11Buffer **readback, ID3D11UnorderedAccessView **view) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {0};
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = (UINT)(bytes / sizeof(uint32_t));
    return make_buffer(gpu, bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, NULL, buffer) &&
        make_buffer(gpu, bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, NULL, readback) &&
        SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(gpu->device, (ID3D11Resource *)*buffer,
                                                        &desc, view));
}

compositor_gpu_t *compositor_gpu_create(int width, int height, int main_tile,
    const compositor_layer_t *layers, size_t layer_count, int histogram,
    size_t budget, turbo_pipeline_status_t *status) {
    compositor_gpu_t *gpu = NULL;
    uint8_t *assets = NULL;
    ID3D11Buffer *asset_buffer = NULL;
    size_t upload_bytes, output_bytes = video_bytes(width, height), asset_bytes = 0, state_bytes;
    size_t i;
    *status = TURBO_PIPELINE_ENOMEM;
    if (layer_count > (GPU_BUFFER_LIMIT / sizeof(uint32_t) - HEADER_WORDS) / LAYER_WORDS ||
        output_bytes > GPU_BUFFER_LIMIT) return NULL;
    upload_bytes = (HEADER_WORDS + layer_count * LAYER_WORDS) * sizeof(uint32_t);
    if (!main_tile && !advance(&upload_bytes, output_bytes)) return NULL;
    for (i = 0; i < layer_count; ++i) {
        int w = layers[i].image_rect.width, h = layers[i].image_rect.height;
        if (layers[i].kind == TURBO_COMPOSITOR_RGBA_IMAGE) {
            size_t pixels = (size_t)w * h;
            if (pixels > GPU_BUFFER_LIMIT / 5 || !advance(&asset_bytes, pixels * 5)) return NULL;
        } else if (!advance(&upload_bytes, video_bytes(w, h))) return NULL;
    }
    if (!asset_bytes) asset_bytes = sizeof(uint32_t);
    state_bytes = sizeof(*gpu) + layer_count * sizeof(gpu_payload_t);
    if (state_bytes > budget) return NULL;
    budget -= state_bytes;
    if (upload_bytes > budget) return NULL;
    budget -= upload_bytes;
    if (output_bytes > budget / 2) return NULL;
    budget -= output_bytes * 2;
    if (asset_bytes > budget / 2) return NULL; /* Staging copy plus immutable buffer. */
    budget -= asset_bytes * 2;
    if (histogram && budget < 2 * HISTOGRAM_BINS * sizeof(uint32_t)) return NULL;

    gpu = calloc(1, state_bytes);
    if (!gpu) return NULL;
    gpu->width = width;
    gpu->height = height;
    gpu->histogram = histogram;
    gpu->layer_count = layer_count;
    gpu->output_layout = video_layout(0, width);
    UINT groups = (UINT)((output_bytes / sizeof(uint32_t) + GROUP_WIDTH - 1) / GROUP_WIDTH);
    gpu->groups_x = groups <= D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION
        ? groups : D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
    gpu->groups_y = (groups + gpu->groups_x - 1) / gpu->groups_x;
    if (!create_device(gpu)) {
        *status = TURBO_PIPELINE_ECONFIG;
        goto fail;
    }
    assets = calloc(1, asset_bytes);
    if (!assets) goto fail;
    size_t video_offset = (HEADER_WORDS + layer_count * LAYER_WORDS) * sizeof(uint32_t), asset_offset = 0;
    if (!main_tile) {
        gpu->background = video_layout(video_offset, width);
        video_offset += output_bytes;
    }
    for (i = 0; i < layer_count; ++i) {
        int w = layers[i].image_rect.width, h = layers[i].image_rect.height;
        if (layers[i].kind == TURBO_COMPOSITOR_RGBA_IMAGE) {
            size_t pixels = (size_t)w * h;
            gpu->layers[i].base = (uint32_t)asset_offset;
            memcpy(assets + asset_offset, layers[i].luma_alpha, pixels * 2);
            memcpy(assets + asset_offset + pixels * 2, layers[i].chroma, pixels * 3);
            asset_offset += pixels * 5;
        } else {
            gpu->layers[i] = video_layout(video_offset, w);
            video_offset += video_bytes(w, h);
        }
    }
    *status = TURBO_PIPELINE_EIO;
    if (!create_shader(gpu) ||
        !make_input(gpu, upload_bytes, NULL, &gpu->upload, &gpu->input_view) ||
        !make_input(gpu, asset_bytes, assets, &asset_buffer, &gpu->asset_view) ||
        !make_output(gpu, output_bytes, &gpu->output, &gpu->readback, &gpu->output_view) ||
        (histogram && !make_output(gpu, HISTOGRAM_BINS * sizeof(uint32_t), &gpu->bins,
                                   &gpu->bins_readback, &gpu->bins_view)))
        goto fail;
    release_object(asset_buffer);
    free(assets);
    *status = TURBO_PIPELINE_OK;
    return gpu;
fail:
    release_object(asset_buffer);
    free(assets);
    compositor_gpu_destroy(gpu);
    return NULL;
}

int compositor_gpu_begin(compositor_gpu_t *gpu, const turbo_pipeline_video_frame_t *frame) {
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ID3D11DeviceContext_Map(gpu->context, (ID3D11Resource *)gpu->upload,
                                    0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return 0;
    gpu->mapped = map.pData;
    const uint32_t header[] = {
        (uint32_t)gpu->width, (uint32_t)gpu->height, gpu->output_layout.pitch_y,
        gpu->output_layout.pitch_c, (uint32_t)gpu->layer_count, gpu->background.base,
        gpu->groups_x * GROUP_WIDTH, (uint32_t)gpu->histogram
    };
    memcpy(gpu->mapped, header, sizeof(header));
    if (gpu->background.base) {
        const uint8_t *data[] = {frame->planes[0].data, frame->planes[1].data, frame->planes[2].data};
        const ptrdiff_t stride[] = {frame->planes[0].stride, frame->planes[1].stride, frame->planes[2].stride};
        copy_video_to_gpu(map.pData, gpu->background, gpu->width, gpu->height, data, stride);
    }
    return 1;
}

void compositor_gpu_layer(compositor_gpu_t *gpu, size_t i, const compositor_layer_t *layer,
                          const uint8_t *const data[3], const ptrdiff_t strides[3]) {
    gpu_payload_t payload = gpu->layers[i];
    const turbo_compositor_rect_t box = layer->rect, image = layer->image_rect;
    const uint32_t words[LAYER_WORDS] = {
        !data && layer->kind == TURBO_COMPOSITOR_AUX_VIDEO ? MISSING_AUX_VIDEO : (uint32_t)layer->kind,
        layer->opacity, (uint32_t)box.x, (uint32_t)box.y, (uint32_t)box.width,
        (uint32_t)box.height, (uint32_t)image.x, (uint32_t)image.y, (uint32_t)image.width,
        (uint32_t)image.height, payload.base, payload.pitch_y, payload.pitch_c
    };
    memcpy(gpu->mapped + HEADER_WORDS + i * LAYER_WORDS, words, sizeof(words));
    if (data)
        copy_video_to_gpu((uint8_t *)gpu->mapped, payload, image.width, image.height, data, strides);
}

int compositor_gpu_end(compositor_gpu_t *gpu, const turbo_pipeline_video_frame_t *frame,
                       uint32_t *histogram) {
    ID3D11DeviceContext *context = gpu->context;
    ID3D11ShaderResourceView *inputs[] = {gpu->input_view, gpu->asset_view};
    ID3D11UnorderedAccessView *outputs[] = {gpu->output_view, gpu->bins_view};
    ID3D11ShaderResourceView *no_inputs[] = {NULL, NULL};
    ID3D11UnorderedAccessView *no_outputs[] = {NULL, NULL};
    D3D11_MAPPED_SUBRESOURCE map;
    const UINT zero[] = {0, 0, 0, 0};
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)gpu->upload, 0);
    gpu->mapped = NULL;
    if (gpu->histogram) ID3D11DeviceContext_ClearUnorderedAccessViewUint(context, gpu->bins_view, zero);
    ID3D11DeviceContext_CSSetShader(context, gpu->shader, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(context, 0, 2, inputs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(context, 0, 2, outputs, NULL);
    ID3D11DeviceContext_Dispatch(context, gpu->groups_x, gpu->groups_y, 1);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(context, 0, 2, no_outputs, NULL);
    ID3D11DeviceContext_CSSetShaderResources(context, 0, 2, no_inputs);
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)gpu->readback, (ID3D11Resource *)gpu->output);
    if (gpu->histogram)
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)gpu->bins_readback, (ID3D11Resource *)gpu->bins);
    if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)gpu->readback,
                                     0, D3D11_MAP_READ, 0, &map))) return 0;
    copy_video_from_gpu(gpu, map.pData, frame);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)gpu->readback, 0);
    if (gpu->histogram) {
        if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)gpu->bins_readback,
                                         0, D3D11_MAP_READ, 0, &map))) return 0;
        memcpy(histogram, map.pData, HISTOGRAM_BINS * sizeof(*histogram));
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)gpu->bins_readback, 0);
    }
    return 1;
}

void compositor_gpu_destroy(compositor_gpu_t *gpu) {
    if (!gpu) return;
    release_object(gpu->bins_view);
    release_object(gpu->output_view);
    release_object(gpu->asset_view);
    release_object(gpu->input_view);
    release_object(gpu->bins_readback);
    release_object(gpu->bins);
    release_object(gpu->readback);
    release_object(gpu->output);
    release_object(gpu->upload);
    release_object(gpu->shader);
    release_object(gpu->context);
    release_object(gpu->device);
    free(gpu);
}

#endif
