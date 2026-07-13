/**
 * Windows Screen Capture Implementation
 *
 * Uses DXGI Desktop Duplication for efficient screen capture
 */
#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#define COBJMACROS
#define CINTERFACE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <rpc.h>
#include <rpcndr.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stb_sprintf.h>

#endif /* _WIN32 */

#include "turbo_capture.h"

#ifdef _WIN32

/* =============================================================================
 * Constants
 * ============================================================================= */

#define DXGI_TIMEOUT_MS     100

/* =============================================================================
 * Context Structure
 * ============================================================================= */

typedef struct {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    IDXGIOutputDuplication *duplication;
    ID3D11Texture2D *staging_texture;

    HANDLE capture_thread;
    HANDLE stop_event;
    int running;

    /* Configuration */
    int monitor_index;
    int framerate;
    int capture_cursor;
    int width;
    int height;

    /* Frame buffer */
    uint8_t *frame_buffer;
    size_t frame_buffer_size;

    /* Timing */
    LARGE_INTEGER freq;
    LONGLONG frame_interval;
    LONGLONG last_frame_time;
} dxgi_screen_capture_ctx_t;

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

static void wide_to_utf8_screen(const WCHAR *wide, char *utf8, size_t utf8_len) {
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, (int)utf8_len, NULL, NULL);
}

int turbo_capture_list_gpu_devices(turbo_capture_device_t *devices, int max_count) {
    IDXGIFactory1 *factory = NULL;
    HRESULT hr;
    UINT adapter_index;
    int count = 0;

    if (!devices || max_count <= 0) return -1;

    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr) || !factory) return -1;

    for (adapter_index = 0; count < max_count; ++adapter_index) {
        IDXGIAdapter1 *adapter = NULL;
        DXGI_ADAPTER_DESC1 desc;
        turbo_capture_device_t *dev;

        hr = IDXGIFactory1_EnumAdapters1(factory, adapter_index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr) || !adapter) continue;

        memset(&desc, 0, sizeof(desc));
        hr = IDXGIAdapter1_GetDesc1(adapter, &desc);
        if (SUCCEEDED(hr) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            dev = &devices[count];
            memset(dev, 0, sizeof(*dev));
            dev->index = (int)adapter_index;
            dev->type = TURBO_CAPTURE_TYPE_GPU;
            dev->is_default = (count == 0) ? 1 : 0;
            wide_to_utf8_screen(desc.Description, dev->name, sizeof(dev->name));
            if (strstr(dev->name, "Microsoft Basic Render") == NULL) {
                snprintf(dev->id, sizeof(dev->id), "%u", adapter_index);
                ++count;
            }
        }

        IDXGIAdapter1_Release(adapter);
    }

    IDXGIFactory1_Release(factory);
    return count;
}

static int init_dxgi(dxgi_screen_capture_ctx_t *ctx) {
    HRESULT hr;

    /* Create D3D11 device */
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                            feature_levels, 4,
                            D3D11_SDK_VERSION,
                            &ctx->device, NULL, &ctx->context);
    if (FAILED(hr)) return -1;

    /* Get DXGI device */
    IDXGIDevice *dxgi_device = NULL;
    hr = ID3D11Device_QueryInterface(ctx->device, &IID_IDXGIDevice,
                                      (void **)&dxgi_device);
    if (FAILED(hr)) return -1;

    /* Get adapter */
    IDXGIAdapter *adapter = NULL;
    hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    IDXGIDevice_Release(dxgi_device);
    if (FAILED(hr)) return -1;

    /* Get output (monitor) */
    IDXGIOutput *output = NULL;
    hr = IDXGIAdapter_EnumOutputs(adapter, ctx->monitor_index >= 0 ? ctx->monitor_index : 0,
                                   &output);
    IDXGIAdapter_Release(adapter);
    if (FAILED(hr)) return -1;

    /* Get output1 for duplication */
    IDXGIOutput1 *output1 = NULL;
    hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1);
    if (FAILED(hr)) {
        IDXGIOutput_Release(output);
        return -1;
    }

    /* Get output description */
    DXGI_OUTPUT_DESC desc;
    IDXGIOutput_GetDesc(output, &desc);
    ctx->width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    ctx->height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    IDXGIOutput_Release(output);

    /* Create duplication */
    hr = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)ctx->device,
                                       &ctx->duplication);
    IDXGIOutput1_Release(output1);
    if (FAILED(hr)) return -1;

    /* Create staging texture for CPU access */
    D3D11_TEXTURE2D_DESC tex_desc = {0};
    tex_desc.Width = ctx->width;
    tex_desc.Height = ctx->height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Usage = D3D11_USAGE_STAGING;
    tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = ID3D11Device_CreateTexture2D(ctx->device, &tex_desc, NULL,
                                       &ctx->staging_texture);
    if (FAILED(hr)) return -1;

    /* Allocate frame buffer */
    ctx->frame_buffer_size = ctx->width * ctx->height * 4;  /* BGRA */
    ctx->frame_buffer = (uint8_t *)malloc(ctx->frame_buffer_size);
    if (!ctx->frame_buffer) return -1;

    return 0;
}

static void cleanup_dxgi(dxgi_screen_capture_ctx_t *ctx) {
    if (ctx->staging_texture) {
        ID3D11Texture2D_Release(ctx->staging_texture);
        ctx->staging_texture = NULL;
    }
    if (ctx->duplication) {
        IDXGIOutputDuplication_Release(ctx->duplication);
        ctx->duplication = NULL;
    }
    if (ctx->context) {
        ID3D11DeviceContext_Release(ctx->context);
        ctx->context = NULL;
    }
    if (ctx->device) {
        ID3D11Device_Release(ctx->device);
        ctx->device = NULL;
    }
    if (ctx->frame_buffer) {
        free(ctx->frame_buffer);
        ctx->frame_buffer = NULL;
    }
}

/* =============================================================================
 * Capture Thread
 * ============================================================================= */

static DWORD WINAPI dxgi_screen_capture_thread(LPVOID param) {
    turbo_capture_t *capture = (turbo_capture_t *)param;
    dxgi_screen_capture_ctx_t *ctx = (dxgi_screen_capture_ctx_t *)capture->platform_ctx;

    capture->state = TURBO_CAPTURE_STATE_RUNNING;
    if (capture->state_cb) {
        capture->state_cb(capture, capture->state, capture->user_data);
    }

    QueryPerformanceFrequency(&ctx->freq);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    ctx->last_frame_time = now.QuadPart;

    while (ctx->running) {
        DWORD result = WaitForSingleObject(ctx->stop_event, 0);
        if (result == WAIT_OBJECT_0) break;

        /* Rate limiting */
        QueryPerformanceCounter(&now);
        LONGLONG elapsed = now.QuadPart - ctx->last_frame_time;
        if (elapsed < ctx->frame_interval) {
            Sleep(1);
            continue;
        }

        /* Acquire next frame */
        IDXGIResource *desktop_resource = NULL;
        DXGI_OUTDUPL_FRAME_INFO frame_info;

        HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(ctx->duplication,
                                                              DXGI_TIMEOUT_MS,
                                                              &frame_info,
                                                              &desktop_resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }

        if (FAILED(hr)) {
            /* Need to reinitialize (e.g., mode change) */
            cleanup_dxgi(ctx);
            if (init_dxgi(ctx) != 0) {
                capture->state = TURBO_CAPTURE_STATE_ERROR;
                break;
            }
            continue;
        }

        /* Get texture from resource */
        ID3D11Texture2D *desktop_texture = NULL;
        hr = IDXGIResource_QueryInterface(desktop_resource, &IID_ID3D11Texture2D,
                                           (void **)&desktop_texture);
        IDXGIResource_Release(desktop_resource);

        if (SUCCEEDED(hr)) {
            /* Copy to staging texture */
            ID3D11DeviceContext_CopyResource(ctx->context,
                                              (ID3D11Resource *)ctx->staging_texture,
                                              (ID3D11Resource *)desktop_texture);

            /* Map staging texture */
            D3D11_MAPPED_SUBRESOURCE mapped;
            hr = ID3D11DeviceContext_Map(ctx->context,
                                          (ID3D11Resource *)ctx->staging_texture,
                                          0, D3D11_MAP_READ, 0, &mapped);

            if (SUCCEEDED(hr)) {
                /* Copy frame data */
                uint8_t *src = (uint8_t *)mapped.pData;
                uint8_t *dst = ctx->frame_buffer;
                size_t row_size = ctx->width * 4;

                for (int y = 0; y < ctx->height; y++) {
                    memcpy(dst, src, row_size);
                    src += mapped.RowPitch;
                    dst += row_size;
                }

                ID3D11DeviceContext_Unmap(ctx->context,
                                           (ID3D11Resource *)ctx->staging_texture, 0);

                /* Calculate timestamp */
                uint64_t timestamp = (now.QuadPart * 1000000) / ctx->freq.QuadPart;

                /* Deliver frame */
                if (capture->video_cb) {
                    capture->video_cb(capture, ctx->frame_buffer,
                                       ctx->frame_buffer_size,
                                       ctx->width, ctx->height,
                                       timestamp, capture->user_data);
                }

                ctx->last_frame_time = now.QuadPart;
            }

            ID3D11Texture2D_Release(desktop_texture);
        }

        IDXGIOutputDuplication_ReleaseFrame(ctx->duplication);
    }

    return 0;
}

/* =============================================================================
 * Screen Enumeration
 * ============================================================================= */

int turbo_capture_list_screens(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    HRESULT hr;
    IDXGIFactory1 *factory = NULL;
    int count = 0;

    hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
    if (FAILED(hr)) return -1;

    IDXGIAdapter1 *adapter = NULL;
    for (UINT adapter_idx = 0;
         SUCCEEDED(IDXGIFactory1_EnumAdapters1(factory, adapter_idx, &adapter));
         adapter_idx++) {

        IDXGIOutput *output = NULL;
        for (UINT output_idx = 0;
             count < max_count &&
             SUCCEEDED(IDXGIAdapter1_EnumOutputs(adapter, output_idx, &output));
             output_idx++) {

            DXGI_OUTPUT_DESC desc;
            hr = IDXGIOutput_GetDesc(output, &desc);
            if (SUCCEEDED(hr)) {
                turbo_capture_device_t *dev = &devices[count];
                memset(dev, 0, sizeof(*dev));

                dev->index = count;
                dev->type = TURBO_CAPTURE_TYPE_SCREEN;
                dev->is_default = (count == 0) ? 1 : 0;

                /* Convert device name */
                WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1,
                                     dev->name, sizeof(dev->name), NULL, NULL);

                /* Use index as ID */
                snprintf(dev->id, sizeof(dev->id), "%d", count);

                count++;
            }

            IDXGIOutput_Release(output);
        }

        IDXGIAdapter1_Release(adapter);
    }

    IDXGIFactory1_Release(factory);

    return count;
}

/* =============================================================================
 * Screen Capture Implementation
 * ============================================================================= */

turbo_capture_t *turbo_screen_capture_create(const turbo_screen_capture_config_t *config) {
    turbo_capture_t *capture = (turbo_capture_t *)calloc(1, sizeof(turbo_capture_t));
    if (!capture) return NULL;

    dxgi_screen_capture_ctx_t *ctx = (dxgi_screen_capture_ctx_t *)calloc(1, sizeof(dxgi_screen_capture_ctx_t));
    if (!ctx) {
        free(capture);
        return NULL;
    }

    capture->type = TURBO_CAPTURE_TYPE_SCREEN;
    capture->state = TURBO_CAPTURE_STATE_STOPPED;
    capture->platform_ctx = ctx;

    /* Store config */
    ctx->monitor_index = config ? config->monitor_index : -1;
    ctx->framerate = config ? config->framerate : 30;
    ctx->capture_cursor = config ? config->capture_cursor : 1;

    /* Calculate frame interval */
    QueryPerformanceFrequency(&ctx->freq);
    ctx->frame_interval = ctx->freq.QuadPart / ctx->framerate;

    /* Initialize DXGI */
    if (init_dxgi(ctx) != 0) {
        turbo_capture_destroy(capture);
        return NULL;
    }

    /* Create stop event */
    ctx->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ctx->stop_event) {
        turbo_capture_destroy(capture);
        return NULL;
    }

    return capture;
}

void turbo_screen_capture_set_callback(turbo_capture_t *capture,
                                         turbo_video_capture_cb cb,
                                         void *user_data) {
    if (!capture) return;
    capture->video_cb = cb;
    capture->user_data = user_data;
}

/* =============================================================================
 * Hooks for centralized dispatcher
 * ============================================================================= */

int dxgi_screen_start(turbo_capture_t *capture) {
    dxgi_screen_capture_ctx_t *ctx = (dxgi_screen_capture_ctx_t *)capture->platform_ctx;

    ResetEvent(ctx->stop_event);
    ctx->running = 1;

    ctx->capture_thread = CreateThread(NULL, 0, dxgi_screen_capture_thread,
                                         capture, 0, NULL);
    return ctx->capture_thread ? 0 : -1;
}

void dxgi_screen_stop(turbo_capture_t *capture) {
    dxgi_screen_capture_ctx_t *ctx = (dxgi_screen_capture_ctx_t *)capture->platform_ctx;

    ctx->running = 0;
    SetEvent(ctx->stop_event);

    if (ctx->capture_thread) {
        WaitForSingleObject(ctx->capture_thread, 5000);
        CloseHandle(ctx->capture_thread);
        ctx->capture_thread = NULL;
    }
}

void dxgi_screen_destroy(turbo_capture_t *capture) {
    dxgi_screen_capture_ctx_t *ctx = (dxgi_screen_capture_ctx_t *)capture->platform_ctx;
    if (!ctx) return;

    dxgi_screen_stop(capture);
    cleanup_dxgi(ctx);

    if (ctx->stop_event) CloseHandle(ctx->stop_event);

    free(ctx);
    free(capture);
}

/* Dispatcher logic moved to capture_win32.c */

#endif /* _WIN32 */
