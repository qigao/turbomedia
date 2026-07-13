/**
 * Windows Video Capture Implementation
 *
 * Uses Media Foundation for camera capture
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
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <dshow.h>
#include <libyuv/convert.h>
#include <libyuv/scale.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#endif /* _WIN32 */

#include "turbo_capture.h"

#ifdef _WIN32

/* =============================================================================
 * Constants
 * ============================================================================= */

#define MF_MAX_DEVICES  16

/* =============================================================================
 * Helper Macros for Media Foundation
 * ============================================================================= */

static inline HRESULT MFSetAttributeSize(IMFAttributes *pAttributes,
                                          REFGUID guidKey,
                                          UINT32 unWidth, UINT32 unHeight) {
    return IMFAttributes_SetUINT64(pAttributes, guidKey,
                                    ((UINT64)unWidth << 32) | unHeight);
}

static inline HRESULT MFSetAttributeRatio(IMFAttributes *pAttributes,
                                           REFGUID guidKey,
                                           UINT32 unNumerator, UINT32 unDenominator) {
    return IMFAttributes_SetUINT64(pAttributes, guidKey,
                                    ((UINT64)unNumerator << 32) | unDenominator);
}

static inline HRESULT MFGetAttributeSizeLocal(IMFAttributes *pAttributes,
                                               REFGUID guidKey,
                                               UINT32 *punWidth, UINT32 *punHeight) {
    UINT64 value = 0;
    HRESULT hr = IMFAttributes_GetUINT64(pAttributes, guidKey, &value);
    if (FAILED(hr)) return hr;
    *punWidth = (UINT32)(value >> 32);
    *punHeight = (UINT32)value;
    return S_OK;
}

static inline HRESULT MFGetAttributeRatioLocal(IMFAttributes *pAttributes,
                                                REFGUID guidKey,
                                                UINT32 *punNumerator,
                                                UINT32 *punDenominator) {
    UINT64 value = 0;
    HRESULT hr = IMFAttributes_GetUINT64(pAttributes, guidKey, &value);
    if (FAILED(hr)) return hr;
    *punNumerator = (UINT32)(value >> 32);
    *punDenominator = (UINT32)value;
    return S_OK;
}

/* =============================================================================
 * Context Structure
 * ============================================================================= */

typedef struct {
    IMFSourceReader *reader;
    IMFMediaSource *source;
    IAMCameraControl *camera_control;
    IAMVideoProcAmp *video_proc_amp;

    HANDLE capture_thread;
    HANDLE stop_event;
    int running;

    /* Configuration */
    int width;
    int height;
    int framerate;
    int format;         /* 0=I420, 1=NV12, 2=RGB24, 3=BGRA */
    int source_format;  /* Media Foundation output format */

    /* Conversion buffer */
    uint8_t *convert_buf;
    size_t convert_buf_size;

    /* Software camera controls */
    int zoom_percent;
    turbo_video_crop_t crop;
    int crop_enabled;
    CRITICAL_SECTION control_lock;
    int control_lock_initialized;
    uint8_t *control_buf;
    size_t control_buf_size;
    int com_initialized;
} mf_video_capture_ctx_t;

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

static void wide_to_utf8_video(const WCHAR *wide, char *utf8, size_t utf8_len) {
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, (int)utf8_len, NULL, NULL);
}

static int parse_device_index(const char *device_id, int *device_index) {
    char *end;
    long value;

    if (!device_id || !device_id[0]) return 0;

    value = strtol(device_id, &end, 10);
    if (*end != '\0' || value < 0) return 0;

    *device_index = (int)value;
    return 1;
}

static int guid_equals(REFGUID lhs, REFGUID rhs) {
    return memcmp(lhs, rhs, sizeof(GUID)) == 0;
}

static int mf_subtype_to_turbo_format(REFGUID subtype) {
    if (guid_equals(subtype, &MFVideoFormat_I420) ||
        guid_equals(subtype, &MFVideoFormat_IYUV) ||
        guid_equals(subtype, &MFVideoFormat_YV12)) {
        return 0;
    }
    if (guid_equals(subtype, &MFVideoFormat_NV12)) {
        return 1;
    }
    if (guid_equals(subtype, &MFVideoFormat_RGB24)) {
        return 2;
    }
    if (guid_equals(subtype, &MFVideoFormat_ARGB32)) {
        return 3;
    }
    return -1;
}

static int video_mode_exists(const turbo_video_capture_mode_t *modes,
                             int count,
                             turbo_video_capture_mode_t mode) {
    for (int i = 0; i < count; ++i) {
        if (modes[i].width == mode.width &&
            modes[i].height == mode.height &&
            modes[i].framerate == mode.framerate &&
            modes[i].format == mode.format) {
            return 1;
        }
    }
    return 0;
}

static int read_moniker_string(IMoniker *moniker,
                               const WCHAR *name,
                               char *value,
                               size_t value_len) {
    IPropertyBag *bag = NULL;
    VARIANT var;
    HRESULT hr;

    if (!moniker || !name || !value || value_len == 0) return 0;
    value[0] = '\0';

    hr = IMoniker_BindToStorage(moniker, NULL, NULL, &IID_IPropertyBag, (void **)&bag);
    if (FAILED(hr) || !bag) return 0;

    VariantInit(&var);
    hr = IPropertyBag_Read(bag, name, &var, NULL);
    if (SUCCEEDED(hr) && var.vt == VT_BSTR && var.bstrVal) {
        wide_to_utf8_video(var.bstrVal, value, value_len);
        VariantClear(&var);
        IPropertyBag_Release(bag);
        return 1;
    }

    VariantClear(&var);
    IPropertyBag_Release(bag);
    return 0;
}

static int moniker_matches_device(IMoniker *moniker,
                                  int target_index,
                                  const char *device_id,
                                  int index) {
    char path[512];
    char friendly_name[256];

    if (target_index >= 0) {
        return index == target_index;
    }

    if (!device_id || !device_id[0]) {
        return index == 0;
    }

    if (read_moniker_string(moniker, L"DevicePath", path, sizeof(path)) &&
        strcmp(path, device_id) == 0) {
        return 1;
    }

    if (read_moniker_string(moniker, L"FriendlyName", friendly_name, sizeof(friendly_name)) &&
        strcmp(friendly_name, device_id) == 0) {
        return 1;
    }

    return 0;
}

static int attach_camera_control(mf_video_capture_ctx_t *ctx,
                                 int target_index,
                                 const char *device_id) {
    ICreateDevEnum *dev_enum = NULL;
    IEnumMoniker *enum_moniker = NULL;
    IMoniker *moniker = NULL;
    IBaseFilter *filter = NULL;
    HRESULT hr;
    ULONG fetched = 0;
    int index = 0;
    int result = TURBO_CAPTURE_ERR_UNSUPPORTED;

    hr = CoCreateInstance(&CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
                          &IID_ICreateDevEnum, (void **)&dev_enum);
    if (FAILED(hr) || !dev_enum) goto cleanup;

    hr = ICreateDevEnum_CreateClassEnumerator(dev_enum,
                                              &CLSID_VideoInputDeviceCategory,
                                              &enum_moniker,
                                              0);
    if (hr != S_OK || !enum_moniker) goto cleanup;

    while (IEnumMoniker_Next(enum_moniker, 1, &moniker, &fetched) == S_OK) {
        if (moniker_matches_device(moniker, target_index, device_id, index)) {
            hr = IMoniker_BindToObject(moniker, NULL, NULL,
                                       &IID_IBaseFilter,
                                       (void **)&filter);
            if (SUCCEEDED(hr) && filter) {
                hr = IBaseFilter_QueryInterface(filter,
                                                &IID_IAMCameraControl,
                                                (void **)&ctx->camera_control);
                if (SUCCEEDED(hr) && ctx->camera_control) {
                    result = TURBO_CAPTURE_OK;
                }

                hr = IBaseFilter_QueryInterface(filter,
                                                &IID_IAMVideoProcAmp,
                                                (void **)&ctx->video_proc_amp);
                if (SUCCEEDED(hr) && ctx->video_proc_amp) {
                    result = TURBO_CAPTURE_OK;
                }

                result = (ctx->camera_control || ctx->video_proc_amp)
                         ? TURBO_CAPTURE_OK
                         : TURBO_CAPTURE_ERR_UNSUPPORTED;
            }
            IMoniker_Release(moniker);
            moniker = NULL;
            break;
        }

        IMoniker_Release(moniker);
        moniker = NULL;
        index++;
    }

cleanup:
    if (filter) IBaseFilter_Release(filter);
    if (moniker) IMoniker_Release(moniker);
    if (enum_moniker) IEnumMoniker_Release(enum_moniker);
    if (dev_enum) ICreateDevEnum_Release(dev_enum);
    return result;
}

static int camera_control_to_dshow(turbo_camera_control_t control, long *property) {
    if (!property) return 0;

    switch (control) {
    case TURBO_CAMERA_CONTROL_ZOOM:
        *property = CameraControl_Zoom;
        return 1;
    case TURBO_CAMERA_CONTROL_FOCUS:
        *property = CameraControl_Focus;
        return 1;
    case TURBO_CAMERA_CONTROL_EXPOSURE:
        *property = CameraControl_Exposure;
        return 1;
    case TURBO_CAMERA_CONTROL_PAN:
        *property = CameraControl_Pan;
        return 1;
    case TURBO_CAMERA_CONTROL_TILT:
        *property = CameraControl_Tilt;
        return 1;
    default:
        return 0;
    }
}

static int video_proc_amp_to_dshow(turbo_camera_control_t control, long *property) {
    if (!property) return 0;

    switch (control) {
    case TURBO_CAMERA_CONTROL_BRIGHTNESS:
        *property = VideoProcAmp_Brightness;
        return 1;
    case TURBO_CAMERA_CONTROL_CONTRAST:
        *property = VideoProcAmp_Contrast;
        return 1;
    case TURBO_CAMERA_CONTROL_HUE:
        *property = VideoProcAmp_Hue;
        return 1;
    case TURBO_CAMERA_CONTROL_WHITE_BALANCE:
        *property = VideoProcAmp_WhiteBalance;
        return 1;
    default:
        return 0;
    }
}

static int ensure_convert_buffer(mf_video_capture_ctx_t *ctx, size_t size) {
    uint8_t *buffer;

    if (ctx->convert_buf_size >= size) return 0;

    buffer = (uint8_t *)realloc(ctx->convert_buf, size);
    if (!buffer) return -1;

    ctx->convert_buf = buffer;
    ctx->convert_buf_size = size;
    return 0;
}

static int ensure_control_buffer(mf_video_capture_ctx_t *ctx, size_t size) {
    uint8_t *buffer;

    if (ctx->control_buf_size >= size) return 0;

    buffer = (uint8_t *)realloc(ctx->control_buf, size);
    if (!buffer) return -1;

    ctx->control_buf = buffer;
    ctx->control_buf_size = size;
    return 0;
}

static int align_even_down(int value) {
    return value & ~1;
}

static int get_effective_crop(mf_video_capture_ctx_t *ctx, turbo_video_crop_t *crop) {
    int crop_width;
    int crop_height;
    int zoom_percent;
    int crop_enabled;
    turbo_video_crop_t configured_crop;

    if (!ctx || !crop) return 0;

    EnterCriticalSection(&ctx->control_lock);
    crop_enabled = ctx->crop_enabled;
    configured_crop = ctx->crop;
    zoom_percent = ctx->zoom_percent;
    LeaveCriticalSection(&ctx->control_lock);

    if (crop_enabled) {
        *crop = configured_crop;
    } else if (zoom_percent > 100) {
        crop_width = (ctx->width * 100) / zoom_percent;
        crop_height = (ctx->height * 100) / zoom_percent;
        crop->width = align_even_down(crop_width);
        crop->height = align_even_down(crop_height);
        crop->x = align_even_down((ctx->width - crop->width) / 2);
        crop->y = align_even_down((ctx->height - crop->height) / 2);
    } else {
        return 0;
    }

    crop->x = align_even_down(crop->x);
    crop->y = align_even_down(crop->y);
    crop->width = align_even_down(crop->width);
    crop->height = align_even_down(crop->height);

    if (crop->x < 0 || crop->y < 0 ||
        crop->width < 2 || crop->height < 2 ||
        crop->x + crop->width > ctx->width ||
        crop->y + crop->height > ctx->height) {
        return 0;
    }

    if (crop->x == 0 && crop->y == 0 &&
        crop->width == ctx->width && crop->height == ctx->height) {
        return 0;
    }

    return 1;
}

static int deliver_i420_frame(turbo_capture_t *capture,
                              mf_video_capture_ctx_t *ctx,
                              const uint8_t *data,
                              size_t data_len,
                              uint64_t timestamp) {
    const size_t y_size = (size_t)ctx->width * (size_t)ctx->height;
    const size_t frame_size = y_size + y_size / 2;
    const uint8_t *src_y = data;
    const uint8_t *src_u = data + y_size;
    const uint8_t *src_v = src_u + y_size / 4;
    turbo_video_crop_t crop;
    uint8_t *dst_y;
    uint8_t *dst_u;
    uint8_t *dst_v;
    int result;

    if (data_len < frame_size) return -1;

    if (!get_effective_crop(ctx, &crop)) {
        capture->video_cb(capture, data, frame_size,
                          ctx->width, ctx->height,
                          timestamp, capture->user_data);
        return 0;
    }

    if (ensure_control_buffer(ctx, frame_size) != 0) return -1;

    src_y += crop.y * ctx->width + crop.x;
    src_u += (crop.y / 2) * (ctx->width / 2) + (crop.x / 2);
    src_v += (crop.y / 2) * (ctx->width / 2) + (crop.x / 2);

    dst_y = ctx->control_buf;
    dst_u = dst_y + y_size;
    dst_v = dst_u + y_size / 4;

    result = I420Scale(src_y, ctx->width,
                       src_u, ctx->width / 2,
                       src_v, ctx->width / 2,
                       crop.width, crop.height,
                       dst_y, ctx->width,
                       dst_u, ctx->width / 2,
                       dst_v, ctx->width / 2,
                       ctx->width, ctx->height,
                       kFilterBox);
    if (result != 0) return -1;

    capture->video_cb(capture, ctx->control_buf, frame_size,
                      ctx->width, ctx->height,
                      timestamp, capture->user_data);
    return 0;
}

static int deliver_nv12_frame(turbo_capture_t *capture,
                              mf_video_capture_ctx_t *ctx,
                              const uint8_t *data,
                              size_t data_len,
                              uint64_t timestamp) {
    const size_t y_size = (size_t)ctx->width * (size_t)ctx->height;
    const size_t frame_size = y_size + y_size / 2;
    const uint8_t *src_y = data;
    const uint8_t *src_uv = data + y_size;
    turbo_video_crop_t crop;
    uint8_t *dst_y;
    uint8_t *dst_uv;
    int result;

    if (data_len < frame_size) return -1;

    if (!get_effective_crop(ctx, &crop)) {
        capture->video_cb(capture, data, frame_size,
                          ctx->width, ctx->height,
                          timestamp, capture->user_data);
        return 0;
    }

    if (ensure_control_buffer(ctx, frame_size) != 0) return -1;

    src_y += crop.y * ctx->width + crop.x;
    src_uv += (crop.y / 2) * ctx->width + crop.x;

    dst_y = ctx->control_buf;
    dst_uv = dst_y + y_size;

    result = NV12Scale(src_y, ctx->width,
                       src_uv, ctx->width,
                       crop.width, crop.height,
                       dst_y, ctx->width,
                       dst_uv, ctx->width,
                       ctx->width, ctx->height,
                       kFilterBox);
    if (result != 0) return -1;

    capture->video_cb(capture, ctx->control_buf, frame_size,
                      ctx->width, ctx->height,
                      timestamp, capture->user_data);
    return 0;
}

static int deliver_video_frame(turbo_capture_t *capture,
                               mf_video_capture_ctx_t *ctx,
                               const uint8_t *data,
                               size_t data_len,
                               uint64_t timestamp) {
    size_t y_size;
    size_t i420_size;
    uint8_t *dst_y;
    uint8_t *dst_u;
    uint8_t *dst_v;
    int result;

    if (!capture->video_cb) return 0;

    if (ctx->format == 1) {
        return deliver_nv12_frame(capture, ctx, data, data_len, timestamp);
    }

    if (ctx->format != 0 || ctx->source_format == 0) {
        capture->video_cb(capture, data, data_len,
                          ctx->width, ctx->height,
                          timestamp, capture->user_data);
        return 0;
    }

    y_size = (size_t)ctx->width * (size_t)ctx->height;
    i420_size = y_size + y_size / 2;
    if (data_len < i420_size) return -1;
    if (ensure_convert_buffer(ctx, i420_size) != 0) return -1;

    dst_y = ctx->convert_buf;
    dst_u = dst_y + y_size;
    dst_v = dst_u + y_size / 4;

    result = NV12ToI420(data, ctx->width,
                        data + y_size, ctx->width,
                        dst_y, ctx->width,
                        dst_u, ctx->width / 2,
                        dst_v, ctx->width / 2,
                        ctx->width, ctx->height);
    if (result != 0) return -1;

    return deliver_i420_frame(capture, ctx, ctx->convert_buf, i420_size, timestamp);
}

/* =============================================================================
 * Device Enumeration
 * ============================================================================= */

int turbo_capture_list_video_devices(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    HRESULT hr;
    IMFAttributes *attributes = NULL;
    IMFActivate **device_list = NULL;
    UINT32 device_count = 0;
    int count = 0;

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return -1;

    /* Create attributes for video capture devices */
    hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) goto cleanup;

    hr = IMFAttributes_SetGUID(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) goto cleanup;

    /* Enumerate devices */
    hr = MFEnumDeviceSources(attributes, &device_list, &device_count);
    if (FAILED(hr)) goto cleanup;

    for (UINT32 i = 0; i < device_count && count < max_count; i++) {
        turbo_capture_device_t *dev = &devices[count];
        memset(dev, 0, sizeof(*dev));
        dev->index = count;
        dev->type = TURBO_CAPTURE_TYPE_VIDEO;
        dev->is_default = (i == 0) ? 1 : 0;

        /* Get friendly name */
        WCHAR *name = NULL;
        UINT32 name_len = 0;
        hr = IMFActivate_GetAllocatedString(device_list[i],
                                             &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                             &name, &name_len);
        if (SUCCEEDED(hr) && name) {
            wide_to_utf8_video(name, dev->name, sizeof(dev->name));
            CoTaskMemFree(name);
        }

        /* Get symbolic link as ID */
        WCHAR *link = NULL;
        UINT32 link_len = 0;
        hr = IMFActivate_GetAllocatedString(device_list[i],
                                             &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                             &link, &link_len);
        if (SUCCEEDED(hr) && link) {
            wide_to_utf8_video(link, dev->id, sizeof(dev->id));
            CoTaskMemFree(link);
        }

        IMFActivate_Release(device_list[i]);
        count++;
    }

cleanup:
    if (device_list) CoTaskMemFree(device_list);
    if (attributes) IMFAttributes_Release(attributes);
    MFShutdown();

    return count;
}

int turbo_capture_list_video_modes(const char *device_id,
                                   turbo_video_capture_mode_t *modes,
                                   int max_count) {
    if (!modes || max_count <= 0) return TURBO_CAPTURE_ERR_FORMAT;

    HRESULT hr;
    int device_index = -1;
    IMFAttributes *attributes = NULL;
    IMFActivate **device_list = NULL;
    UINT32 device_count = 0;
    IMFActivate *selected = NULL;
    IMFMediaSource *source = NULL;
    IMFSourceReader *reader = NULL;
    int count = 0;

    memset(modes, 0, sizeof(*modes) * (size_t)max_count);

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return TURBO_CAPTURE_ERR_DEVICE;

    hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) goto cleanup;

    hr = IMFAttributes_SetGUID(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) goto cleanup;

    hr = MFEnumDeviceSources(attributes, &device_list, &device_count);
    if (FAILED(hr) || device_count == 0) goto cleanup;

    if (parse_device_index(device_id, &device_index)) {
        if ((UINT32)device_index >= device_count) goto cleanup;
        selected = device_list[device_index];
    } else if (device_id && device_id[0]) {
        for (UINT32 i = 0; i < device_count; i++) {
            WCHAR *link = NULL;
            UINT32 link_len = 0;
            hr = IMFActivate_GetAllocatedString(device_list[i],
                                                 &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                 &link, &link_len);
            if (SUCCEEDED(hr) && link) {
                char id[128];
                wide_to_utf8_video(link, id, sizeof(id));
                CoTaskMemFree(link);
                if (strcmp(id, device_id) == 0) {
                    selected = device_list[i];
                    break;
                }
            }
        }
        if (!selected) goto cleanup;
    } else {
        selected = device_list[0];
    }

    hr = IMFActivate_ActivateObject(selected, &IID_IMFMediaSource, (void **)&source);
    if (FAILED(hr)) goto cleanup;

    hr = MFCreateSourceReaderFromMediaSource(source, NULL, &reader);
    if (FAILED(hr)) goto cleanup;

    for (DWORD type_index = 0; count < max_count; ++type_index) {
        IMFMediaType *media_type = NULL;
        UINT32 width = 0;
        UINT32 height = 0;
        UINT32 fps_num = 0;
        UINT32 fps_den = 0;
        GUID subtype;
        turbo_video_capture_mode_t mode;

        hr = IMFSourceReader_GetNativeMediaType(reader,
                                                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                type_index,
                                                &media_type);
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(hr) || !media_type) {
            continue;
        }

        memset(&subtype, 0, sizeof(subtype));
        if (FAILED(MFGetAttributeSizeLocal((IMFAttributes *)media_type,
                                           &MF_MT_FRAME_SIZE, &width, &height)) ||
            width == 0 || height == 0) {
            IMFMediaType_Release(media_type);
            continue;
        }
        if (FAILED(MFGetAttributeRatioLocal((IMFAttributes *)media_type,
                                            &MF_MT_FRAME_RATE, &fps_num, &fps_den)) ||
            fps_den == 0) {
            fps_num = 0;
            fps_den = 1;
        }
        hr = IMFMediaType_GetGUID(media_type, &MF_MT_SUBTYPE, &subtype);

        mode.width = (int)width;
        mode.height = (int)height;
        mode.framerate = fps_den > 0 ? (int)((fps_num + fps_den / 2) / fps_den) : 0;
        mode.format = SUCCEEDED(hr) ? mf_subtype_to_turbo_format(&subtype) : -1;

        if (!video_mode_exists(modes, count, mode)) {
            modes[count++] = mode;
        }
        IMFMediaType_Release(media_type);
    }

cleanup:
    if (reader) IMFSourceReader_Release(reader);
    if (source) {
        IMFMediaSource_Shutdown(source);
        IMFMediaSource_Release(source);
    }
    if (device_list) {
        for (UINT32 i = 0; i < device_count; i++) {
            IMFActivate_Release(device_list[i]);
        }
        CoTaskMemFree(device_list);
    }
    if (attributes) IMFAttributes_Release(attributes);
    MFShutdown();

    return count;
}

/* =============================================================================
 * Capture Thread
 * ============================================================================= */

static DWORD WINAPI mf_video_capture_thread(LPVOID param) {
    turbo_capture_t *capture = (turbo_capture_t *)param;
    mf_video_capture_ctx_t *ctx = (mf_video_capture_ctx_t *)capture->platform_ctx;

    capture->state = TURBO_CAPTURE_STATE_RUNNING;
    if (capture->state_cb) {
        capture->state_cb(capture, capture->state, capture->user_data);
    }

    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    while (ctx->running) {
        DWORD result = WaitForSingleObject(ctx->stop_event, 0);
        if (result == WAIT_OBJECT_0) break;

        IMFSample *sample = NULL;
        DWORD flags = 0;
        LONGLONG timestamp = 0;

        HRESULT hr = IMFSourceReader_ReadSample(ctx->reader,
                                                 MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                 0, NULL, &flags, &timestamp, &sample);

        if (FAILED(hr) || !sample) {
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            continue;
        }

        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            IMFSample_Release(sample);
            continue;
        }

        /* Get buffer from sample */
        IMFMediaBuffer *buffer = NULL;
        hr = IMFSample_ConvertToContiguousBuffer(sample, &buffer);
        if (SUCCEEDED(hr)) {
            BYTE *data = NULL;
            DWORD data_len = 0;

            hr = IMFMediaBuffer_Lock(buffer, &data, NULL, &data_len);
            if (SUCCEEDED(hr)) {
                /* Calculate timestamp in microseconds */
                uint64_t ts_us = (uint64_t)timestamp / 10;  /* 100ns -> us */

                deliver_video_frame(capture, ctx, data, data_len, ts_us);

                IMFMediaBuffer_Unlock(buffer);
            }

            IMFMediaBuffer_Release(buffer);
        }

        IMFSample_Release(sample);
    }

    return 0;
}

/* =============================================================================
 * Video Capture Implementation
 * ============================================================================= */

turbo_capture_t *turbo_video_capture_create(const char *device_id,
                                             const turbo_video_capture_config_t *config) {
    HRESULT hr;
    int device_index = -1;
    int selected_index = -1;

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) return NULL;

    turbo_capture_t *capture = (turbo_capture_t *)calloc(1, sizeof(turbo_capture_t));
    if (!capture) return NULL;

    mf_video_capture_ctx_t *ctx = (mf_video_capture_ctx_t *)calloc(1, sizeof(mf_video_capture_ctx_t));
    if (!ctx) {
        free(capture);
        return NULL;
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        ctx->com_initialized = 1;
    } else if (hr != RPC_E_CHANGED_MODE) {
        free(ctx);
        free(capture);
        MFShutdown();
        return NULL;
    }

    capture->type = TURBO_CAPTURE_TYPE_VIDEO;
    capture->state = TURBO_CAPTURE_STATE_STOPPED;
    capture->platform_ctx = ctx;
    InitializeCriticalSection(&ctx->control_lock);
    ctx->control_lock_initialized = 1;

    /* Store config */
    ctx->width = config ? config->width : 640;
    ctx->height = config ? config->height : 480;
    ctx->framerate = config ? config->framerate : 30;
    ctx->format = config ? config->format : 1;  /* NV12 default */
    ctx->source_format = ctx->format;
    ctx->zoom_percent = 100;

    /* Find and activate device */
    IMFAttributes *attributes = NULL;
    IMFActivate **device_list = NULL;
    UINT32 device_count = 0;
    IMFActivate *selected = NULL;

    hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) goto error;

    hr = IMFAttributes_SetGUID(attributes, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) goto error;

    hr = MFEnumDeviceSources(attributes, &device_list, &device_count);
    if (FAILED(hr) || device_count == 0) goto error;

    if (parse_device_index(device_id, &device_index)) {
        if ((UINT32)device_index >= device_count) {
            goto error;
        }
        selected = device_list[device_index];
        selected_index = device_index;
    } else if (device_id && device_id[0]) {
        for (UINT32 i = 0; i < device_count; i++) {
            WCHAR *link = NULL;
            UINT32 link_len = 0;
            hr = IMFActivate_GetAllocatedString(device_list[i],
                                                 &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                                 &link, &link_len);
            if (SUCCEEDED(hr) && link) {
                char id[128];
                wide_to_utf8_video(link, id, sizeof(id));
                CoTaskMemFree(link);

                if (strcmp(id, device_id) == 0) {
                    selected = device_list[i];
                    selected_index = (int)i;
                    break;
                }
            }
        }
        if (!selected) {
            goto error;
        }
    } else {
        selected = device_list[0];
        selected_index = 0;
    }

    /* Activate media source */
    hr = IMFActivate_ActivateObject(selected, &IID_IMFMediaSource,
                                     (void **)&ctx->source);
    if (FAILED(hr)) goto error;

    /* Create source reader */
    IMFAttributes *reader_attrs = NULL;
    MFCreateAttributes(&reader_attrs, 1);
    if (reader_attrs) {
        IMFAttributes_SetUINT32(reader_attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        IMFAttributes_SetUINT32(reader_attrs, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }

    hr = MFCreateSourceReaderFromMediaSource(ctx->source, reader_attrs, &ctx->reader);
    if (reader_attrs) IMFAttributes_Release(reader_attrs);
    if (FAILED(hr)) goto error;

    /* Configure output format */
    IMFMediaType *media_type = NULL;
    hr = MFCreateMediaType(&media_type);
    if (SUCCEEDED(hr)) {
        IMFMediaType_SetGUID(media_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);

        /* Set subtype based on format */
        const GUID *subtype = &MFVideoFormat_NV12;
        if (ctx->format == 0) {
            subtype = &MFVideoFormat_NV12;
            ctx->source_format = 1;
        }
        else if (ctx->format == 2) subtype = &MFVideoFormat_RGB24;
        else if (ctx->format == 3) subtype = &MFVideoFormat_ARGB32;

        IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, subtype);
        MFSetAttributeSize((IMFAttributes*)media_type, &MF_MT_FRAME_SIZE, ctx->width, ctx->height);
        MFSetAttributeRatio((IMFAttributes*)media_type, &MF_MT_FRAME_RATE, ctx->framerate, 1);

        hr = IMFSourceReader_SetCurrentMediaType(ctx->reader,
                                                   MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                   NULL, media_type);
        IMFMediaType_Release(media_type);
        
        if (FAILED(hr)) {
             printf("Error: SetCurrentMediaType failed: 0x%08X\n", (unsigned int)hr);
             goto error;
        }
    }

    /* Create stop event */
    ctx->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ctx->stop_event) goto error;

    (void)attach_camera_control(ctx, selected_index, device_id);

    /* Cleanup device list */
    for (UINT32 i = 0; i < device_count; i++) {
        if (device_list[i] != selected) {
            IMFActivate_Release(device_list[i]);
        }
    }
    CoTaskMemFree(device_list);
    IMFAttributes_Release(attributes);

    return capture;

error:
    if (device_list) {
        for (UINT32 i = 0; i < device_count; i++) {
            IMFActivate_Release(device_list[i]);
        }
        CoTaskMemFree(device_list);
    }
    if (attributes) IMFAttributes_Release(attributes);
    turbo_capture_destroy(capture);
    return NULL;
}

void turbo_video_capture_set_callback(turbo_capture_t *capture,
                                        turbo_video_capture_cb cb,
                                        void *user_data) {
    if (!capture) return;
    capture->video_cb = cb;
    capture->user_data = user_data;
}

int turbo_video_capture_get_control_range(turbo_capture_t *capture,
                                           turbo_camera_control_t control,
                                           turbo_camera_control_range_t *range) {
    mf_video_capture_ctx_t *ctx;
    long property;
    long min_value;
    long max_value;
    long step;
    long default_value;
    long ignored_caps;
    long current_value;
    HRESULT hr;

    if (!capture || !range || capture->type != TURBO_CAPTURE_TYPE_VIDEO) {
        return TURBO_CAPTURE_ERR_DEVICE;
    }

    ctx = (mf_video_capture_ctx_t *)capture->platform_ctx;
    if (!ctx) return TURBO_CAPTURE_ERR_DEVICE;

    memset(range, 0, sizeof(*range));

    if (ctx->camera_control && camera_control_to_dshow(control, &property)) {
        hr = IAMCameraControl_GetRange(ctx->camera_control, property,
                                       &min_value, &max_value, &step,
                                       &default_value, &ignored_caps);
        if (SUCCEEDED(hr)) {
            range->min_value = (int)min_value;
            range->max_value = (int)max_value;
            range->step = (int)step;
            range->default_value = (int)default_value;

            current_value = default_value;
            if (SUCCEEDED(IAMCameraControl_Get(ctx->camera_control, property,
                                               &current_value,
                                               &ignored_caps))) {
                range->current_value = (int)current_value;
            } else {
                range->current_value = range->default_value;
            }

            return TURBO_CAPTURE_OK;
        }
    }

    if (ctx->video_proc_amp && video_proc_amp_to_dshow(control, &property)) {
        hr = IAMVideoProcAmp_GetRange(ctx->video_proc_amp, property,
                                      &min_value, &max_value, &step,
                                      &default_value, &ignored_caps);
        if (SUCCEEDED(hr)) {
            range->min_value = (int)min_value;
            range->max_value = (int)max_value;
            range->step = (int)step;
            range->default_value = (int)default_value;

            current_value = default_value;
            if (SUCCEEDED(IAMVideoProcAmp_Get(ctx->video_proc_amp, property,
                                              &current_value,
                                              &ignored_caps))) {
                range->current_value = (int)current_value;
            } else {
                range->current_value = range->default_value;
            }

            return TURBO_CAPTURE_OK;
        }
    }

    if (control == TURBO_CAMERA_CONTROL_ZOOM) {
        range->min_value = 100;
        range->max_value = 400;
        range->step = 1;
        range->default_value = 100;
        EnterCriticalSection(&ctx->control_lock);
        range->current_value = ctx->zoom_percent;
        LeaveCriticalSection(&ctx->control_lock);
        return TURBO_CAPTURE_OK;
    }

    return TURBO_CAPTURE_ERR_UNSUPPORTED;
}

int turbo_video_capture_set_control(turbo_capture_t *capture,
                                     turbo_camera_control_t control,
                                     int value) {
    mf_video_capture_ctx_t *ctx;
    long property;
    HRESULT hr;

    if (!capture || capture->type != TURBO_CAPTURE_TYPE_VIDEO) {
        return TURBO_CAPTURE_ERR_DEVICE;
    }

    ctx = (mf_video_capture_ctx_t *)capture->platform_ctx;
    if (!ctx) return TURBO_CAPTURE_ERR_DEVICE;

    if (ctx->camera_control && camera_control_to_dshow(control, &property)) {
        hr = IAMCameraControl_Set(ctx->camera_control, property, value, CameraControl_Flags_Manual);
        if (SUCCEEDED(hr)) {
            return TURBO_CAPTURE_OK;
        }
    }

    if (ctx->video_proc_amp && video_proc_amp_to_dshow(control, &property)) {
        hr = IAMVideoProcAmp_Set(ctx->video_proc_amp, property, value, VideoProcAmp_Flags_Manual);
        if (SUCCEEDED(hr)) {
            return TURBO_CAPTURE_OK;
        }
    }

    if (control != TURBO_CAMERA_CONTROL_ZOOM) {
        return TURBO_CAPTURE_ERR_UNSUPPORTED;
    }

    if (value < 100 || value > 400) {
        return TURBO_CAPTURE_ERR_FORMAT;
    }

    if (ctx->format != 0 && ctx->format != 1) {
        return TURBO_CAPTURE_ERR_UNSUPPORTED;
    }

    EnterCriticalSection(&ctx->control_lock);
    ctx->zoom_percent = value;
    LeaveCriticalSection(&ctx->control_lock);
    return TURBO_CAPTURE_OK;
}

int turbo_video_capture_get_control(turbo_capture_t *capture,
                                     turbo_camera_control_t control,
                                     int *value) {
    mf_video_capture_ctx_t *ctx;
    long property;
    long ds_value;
    long ignored_flags;
    HRESULT hr;

    if (!capture || !value || capture->type != TURBO_CAPTURE_TYPE_VIDEO) {
        return TURBO_CAPTURE_ERR_DEVICE;
    }

    ctx = (mf_video_capture_ctx_t *)capture->platform_ctx;
    if (!ctx) return TURBO_CAPTURE_ERR_DEVICE;

    if (ctx->camera_control && camera_control_to_dshow(control, &property)) {
        hr = IAMCameraControl_Get(ctx->camera_control, property, &ds_value, &ignored_flags);
        if (SUCCEEDED(hr)) {
            *value = (int)ds_value;
            return TURBO_CAPTURE_OK;
        }
    }

    if (ctx->video_proc_amp && video_proc_amp_to_dshow(control, &property)) {
        hr = IAMVideoProcAmp_Get(ctx->video_proc_amp, property, &ds_value, &ignored_flags);
        if (SUCCEEDED(hr)) {
            *value = (int)ds_value;
            return TURBO_CAPTURE_OK;
        }
    }

    if (control != TURBO_CAMERA_CONTROL_ZOOM) {
        return TURBO_CAPTURE_ERR_UNSUPPORTED;
    }

    EnterCriticalSection(&ctx->control_lock);
    *value = ctx->zoom_percent;
    LeaveCriticalSection(&ctx->control_lock);
    return TURBO_CAPTURE_OK;
}

int turbo_video_capture_set_crop(turbo_capture_t *capture,
                                  const turbo_video_crop_t *crop) {
    mf_video_capture_ctx_t *ctx;
    turbo_video_crop_t normalized;

    if (!capture || capture->type != TURBO_CAPTURE_TYPE_VIDEO) {
        return TURBO_CAPTURE_ERR_DEVICE;
    }

    ctx = (mf_video_capture_ctx_t *)capture->platform_ctx;
    if (!ctx) return TURBO_CAPTURE_ERR_DEVICE;

    if (ctx->format != 0 && ctx->format != 1) {
        return TURBO_CAPTURE_ERR_UNSUPPORTED;
    }

    if (!crop || crop->width <= 0 || crop->height <= 0) {
        EnterCriticalSection(&ctx->control_lock);
        ctx->crop_enabled = 0;
        memset(&ctx->crop, 0, sizeof(ctx->crop));
        LeaveCriticalSection(&ctx->control_lock);
        return TURBO_CAPTURE_OK;
    }

    normalized = *crop;
    normalized.x = align_even_down(normalized.x);
    normalized.y = align_even_down(normalized.y);
    normalized.width = align_even_down(normalized.width);
    normalized.height = align_even_down(normalized.height);

    if (normalized.x < 0 || normalized.y < 0 ||
        normalized.width < 2 || normalized.height < 2 ||
        normalized.x + normalized.width > ctx->width ||
        normalized.y + normalized.height > ctx->height) {
        return TURBO_CAPTURE_ERR_FORMAT;
    }

    EnterCriticalSection(&ctx->control_lock);
    ctx->crop = normalized;
    ctx->crop_enabled = 1;
    LeaveCriticalSection(&ctx->control_lock);
    return TURBO_CAPTURE_OK;
}

int turbo_video_capture_get_crop(turbo_capture_t *capture,
                                  turbo_video_crop_t *crop) {
    mf_video_capture_ctx_t *ctx;

    if (!capture || !crop || capture->type != TURBO_CAPTURE_TYPE_VIDEO) {
        return TURBO_CAPTURE_ERR_DEVICE;
    }

    ctx = (mf_video_capture_ctx_t *)capture->platform_ctx;
    if (!ctx) return TURBO_CAPTURE_ERR_DEVICE;

    EnterCriticalSection(&ctx->control_lock);
    if (ctx->crop_enabled) {
        *crop = ctx->crop;
    } else {
        crop->x = 0;
        crop->y = 0;
        crop->width = ctx->width;
        crop->height = ctx->height;
    }
    LeaveCriticalSection(&ctx->control_lock);

    return TURBO_CAPTURE_OK;
}

/* =============================================================================
 * Hooks for centralized dispatcher
 * ============================================================================= */

int mf_video_start(turbo_capture_t *capture) {
    mf_video_capture_ctx_t *ctx = (mf_video_capture_ctx_t *)capture->platform_ctx;

    ResetEvent(ctx->stop_event);
    ctx->running = 1;

    ctx->capture_thread = CreateThread(NULL, 0, mf_video_capture_thread,
                                         capture, 0, NULL);
    return ctx->capture_thread ? 0 : -1;
}

void mf_video_stop(turbo_capture_t *capture) {
    mf_video_capture_ctx_t *ctx = (mf_video_capture_ctx_t *)capture->platform_ctx;

    ctx->running = 0;
    SetEvent(ctx->stop_event);

    if (ctx->capture_thread) {
        WaitForSingleObject(ctx->capture_thread, 5000);
        CloseHandle(ctx->capture_thread);
        ctx->capture_thread = NULL;
    }
}

void mf_video_destroy(turbo_capture_t *capture) {
    mf_video_capture_ctx_t *ctx = (mf_video_capture_ctx_t *)capture->platform_ctx;
    if (!ctx) return;

    mf_video_stop(capture);

    if (ctx->stop_event) CloseHandle(ctx->stop_event);
    if (ctx->camera_control) IAMCameraControl_Release(ctx->camera_control);
    if (ctx->video_proc_amp) IAMVideoProcAmp_Release(ctx->video_proc_amp);
    if (ctx->reader) IMFSourceReader_Release(ctx->reader);
    if (ctx->source) {
        IMFMediaSource_Shutdown(ctx->source);
        IMFMediaSource_Release(ctx->source);
    }
    if (ctx->convert_buf) free(ctx->convert_buf);
    if (ctx->control_buf) free(ctx->control_buf);
    if (ctx->control_lock_initialized) DeleteCriticalSection(&ctx->control_lock);

    if (ctx->com_initialized) CoUninitialize();

    free(ctx);
    free(capture);

    MFShutdown();
}

#endif /* _WIN32 */
