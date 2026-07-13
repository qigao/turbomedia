/**
 * Windows Audio Capture Implementation
 *
 * Uses WASAPI (Windows Audio Session API) for low-latency audio capture
 */
#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <rpc.h>
#include <rpcndr.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdlib.h>
#include <string.h>

#endif /* _WIN32 */

#include "turbo_capture.h"

#ifdef _WIN32

/* Define GUIDs that are not available in all Windows SDK versions */
#include <initguid.h>
DEFINE_GUID(TURBO_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C,
            0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(TURBO_IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35,
            0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(TURBO_IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4c32,
            0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(TURBO_IID_IAudioCaptureClient, 0xC8ADBD64, 0xE71E, 0x48a0,
            0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);

/* =============================================================================
 * Constants
 * ============================================================================= */

#define REFTIMES_PER_SEC        10000000
#define REFTIMES_PER_MILLISEC   10000
#define WASAPI_BUFFER_MS        20

/* =============================================================================
 * Context Structure
 * ============================================================================= */

typedef struct {
    IMMDeviceEnumerator *enumerator;
    IMMDevice *device;
    IAudioClient *audio_client;
    IAudioCaptureClient *capture_client;

    WAVEFORMATEX *wave_format;
    HANDLE capture_thread;
    HANDLE stop_event;
    int running;

    /* Configuration */
    int sample_rate;
    int channels;
    int bits_per_sample;
    int frame_size_ms;

    /* Resampling buffer (if needed) */
    uint8_t *resample_buf;
    size_t resample_buf_size;
} wasapi_capture_ctx_t;

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

static void wide_to_utf8(const WCHAR *wide, char *utf8, size_t utf8_len) {
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, (int)utf8_len, NULL, NULL);
}

static void utf8_to_wide(const char *utf8, WCHAR *wide, size_t wide_len) {
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)wide_len);
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

/* =============================================================================
 * Device Enumeration
 * ============================================================================= */

int turbo_capture_list_audio_devices(turbo_capture_device_t *devices, int max_count) {
    if (!devices || max_count <= 0) return -1;

    HRESULT hr;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDeviceCollection *collection = NULL;
    IMMDevice *device = NULL;
    IMMDevice *default_device = NULL;
    IPropertyStore *props = NULL;
    int count = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return -1;
    }

    hr = CoCreateInstance(&TURBO_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &TURBO_IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr)) goto cleanup;

    /* Get default device for comparison */
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eCapture,
                                                      eConsole, &default_device);
    LPWSTR default_id = NULL;
    if (SUCCEEDED(hr) && default_device) {
        IMMDevice_GetId(default_device, &default_id);
    }

    /* Enumerate capture devices */
    hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, eCapture,
                                                 DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) goto cleanup;

    UINT device_count;
    IMMDeviceCollection_GetCount(collection, &device_count);

    for (UINT i = 0; i < device_count && count < max_count; i++) {
        hr = IMMDeviceCollection_Item(collection, i, &device);
        if (FAILED(hr)) continue;

        turbo_capture_device_t *dev = &devices[count];
        memset(dev, 0, sizeof(*dev));
        dev->index = count;
        dev->type = TURBO_CAPTURE_TYPE_AUDIO;

        /* Get device ID */
        LPWSTR device_id;
        hr = IMMDevice_GetId(device, &device_id);
        if (SUCCEEDED(hr)) {
            wide_to_utf8(device_id, dev->id, sizeof(dev->id));

            /* Check if default */
            if (default_id && wcscmp(device_id, default_id) == 0) {
                dev->is_default = 1;
            }
            CoTaskMemFree(device_id);
        }

        /* Get device name */
        hr = IMMDevice_OpenPropertyStore(device, STGM_READ, &props);
        if (SUCCEEDED(hr)) {
            PROPVARIANT name;
            PropVariantInit(&name);
            hr = IPropertyStore_GetValue(props, &PKEY_Device_FriendlyName, &name);
            if (SUCCEEDED(hr)) {
                wide_to_utf8(name.pwszVal, dev->name, sizeof(dev->name));
                PropVariantClear(&name);
            }
            IPropertyStore_Release(props);
            props = NULL;
        }

        IMMDevice_Release(device);
        device = NULL;
        count++;
    }

cleanup:
    if (default_id) CoTaskMemFree(default_id);
    if (default_device) IMMDevice_Release(default_device);
    if (collection) IMMDeviceCollection_Release(collection);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);

    return count;
}

/* =============================================================================
 * Capture Thread
 * ============================================================================= */

static DWORD WINAPI wasapi_capture_thread(LPVOID param) {
    turbo_capture_t *capture = (turbo_capture_t *)param;
    wasapi_capture_ctx_t *ctx = (wasapi_capture_ctx_t *)capture->platform_ctx;

    UINT32 buffer_frame_count;
    IAudioClient_GetBufferSize(ctx->audio_client, &buffer_frame_count);

    REFERENCE_TIME duration = (REFERENCE_TIME)REFTIMES_PER_SEC *
                              buffer_frame_count / ctx->wave_format->nSamplesPerSec;
    DWORD sleep_ms = (DWORD)(duration / REFTIMES_PER_MILLISEC / 2);
    if (sleep_ms < 1) sleep_ms = 1;

    HRESULT hr = IAudioClient_Start(ctx->audio_client);
    if (FAILED(hr)) {
        capture->state = TURBO_CAPTURE_STATE_ERROR;
        return 1;
    }

    capture->state = TURBO_CAPTURE_STATE_RUNNING;
    if (capture->state_cb) {
        capture->state_cb(capture, capture->state, capture->user_data);
    }

    while (ctx->running) {
        DWORD result = WaitForSingleObject(ctx->stop_event, sleep_ms);
        if (result == WAIT_OBJECT_0) {
            break;
        }

        UINT32 packet_length;
        hr = IAudioCaptureClient_GetNextPacketSize(ctx->capture_client, &packet_length);
        if (FAILED(hr)) break;

        while (packet_length > 0) {
            BYTE *data;
            UINT32 frames_available;
            DWORD flags;
            UINT64 device_position;
            UINT64 qpc_position;

            hr = IAudioCaptureClient_GetBuffer(ctx->capture_client,
                                                &data, &frames_available,
                                                &flags, &device_position, &qpc_position);
            if (FAILED(hr)) break;

            size_t data_len = frames_available * ctx->wave_format->nBlockAlign;

            /* Handle silence flag */
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                /* Generate silence - could deliver zeros or skip */
                static uint8_t silence[8192];
                if (data_len <= sizeof(silence)) {
                    data = silence;
                }
            }

            /* Convert timestamp to microseconds */
            uint64_t timestamp = qpc_position / 10;  /* 100ns -> us */

            /* Deliver audio samples */
            if (capture->audio_cb) {
                capture->audio_cb(capture, data, data_len, timestamp, capture->user_data);
            }

            IAudioCaptureClient_ReleaseBuffer(ctx->capture_client, frames_available);

            hr = IAudioCaptureClient_GetNextPacketSize(ctx->capture_client, &packet_length);
            if (FAILED(hr)) break;
        }
    }

    IAudioClient_Stop(ctx->audio_client);

    return 0;
}

/* =============================================================================
 * Audio Capture Implementation
 * ============================================================================= */

turbo_capture_t *turbo_audio_capture_create(const char *device_id,
                                             const turbo_audio_capture_config_t *config) {
    HRESULT hr;
    IMMDeviceCollection *collection = NULL;
    int device_index = -1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return NULL;
    }

    turbo_capture_t *capture = (turbo_capture_t *)calloc(1, sizeof(turbo_capture_t));
    if (!capture) return NULL;

    wasapi_capture_ctx_t *ctx = (wasapi_capture_ctx_t *)calloc(1, sizeof(wasapi_capture_ctx_t));
    if (!ctx) {
        free(capture);
        return NULL;
    }

    capture->type = TURBO_CAPTURE_TYPE_AUDIO;
    capture->state = TURBO_CAPTURE_STATE_STOPPED;
    capture->platform_ctx = ctx;

    /* Store config */
    ctx->sample_rate = config ? config->sample_rate : 48000;
    ctx->channels = config ? config->channels : 1;
    ctx->bits_per_sample = config ? config->bits_per_sample : 16;
    ctx->frame_size_ms = config ? config->frame_size_ms : 20;

    /* Create device enumerator */
    hr = CoCreateInstance(&TURBO_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &TURBO_IID_IMMDeviceEnumerator, (void **)&ctx->enumerator);
    if (FAILED(hr)) goto error;

    if (parse_device_index(device_id, &device_index)) {
        UINT count = 0;

        hr = IMMDeviceEnumerator_EnumAudioEndpoints(ctx->enumerator, eCapture,
                                                     DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr)) goto error;

        hr = IMMDeviceCollection_GetCount(collection, &count);
        if (FAILED(hr) || (UINT)device_index >= count) goto error;

        hr = IMMDeviceCollection_Item(collection, (UINT)device_index, &ctx->device);
        if (FAILED(hr)) goto error;
    } else if (device_id && device_id[0]) {
        WCHAR wide_id[128];
        utf8_to_wide(device_id, wide_id, 128);
        hr = IMMDeviceEnumerator_GetDevice(ctx->enumerator, wide_id, &ctx->device);
    } else {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(ctx->enumerator,
                                                          eCapture, eConsole,
                                                          &ctx->device);
    }
    if (FAILED(hr)) goto error;

    /* Activate audio client */
    hr = IMMDevice_Activate(ctx->device, &TURBO_IID_IAudioClient, CLSCTX_ALL,
                            NULL, (void **)&ctx->audio_client);
    if (FAILED(hr)) goto error;

    /* Get mix format */
    hr = IAudioClient_GetMixFormat(ctx->audio_client, &ctx->wave_format);
    if (FAILED(hr)) goto error;

    /* Initialize audio client */
    REFERENCE_TIME buffer_duration = WASAPI_BUFFER_MS * REFTIMES_PER_MILLISEC;

    hr = IAudioClient_Initialize(ctx->audio_client,
                                  AUDCLNT_SHAREMODE_SHARED,
                                  0,
                                  buffer_duration,
                                  0,
                                  ctx->wave_format,
                                  NULL);
    if (FAILED(hr)) goto error;

    /* Get capture client */
    hr = IAudioClient_GetService(ctx->audio_client, &TURBO_IID_IAudioCaptureClient,
                                  (void **)&ctx->capture_client);
    if (FAILED(hr)) goto error;

    /* Create stop event */
    ctx->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ctx->stop_event) goto error;

    if (collection) IMMDeviceCollection_Release(collection);
    return capture;

error:
    if (collection) IMMDeviceCollection_Release(collection);
    turbo_capture_destroy(capture);
    return NULL;
}

void turbo_audio_capture_set_callback(turbo_capture_t *capture,
                                       turbo_audio_capture_cb cb,
                                       void *user_data) {
    if (!capture) return;
    capture->audio_cb = cb;
    capture->user_data = user_data;
}

/* =============================================================================
 * Common Functions
 * ============================================================================= */

int wasapi_audio_start(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return -1;
    wasapi_capture_ctx_t *ctx = (wasapi_capture_ctx_t *)capture->platform_ctx;

    ResetEvent(ctx->stop_event);
    ctx->running = 1;

    ctx->capture_thread = CreateThread(NULL, 0, wasapi_capture_thread,
                                        capture, 0, NULL);
    if (!ctx->capture_thread) {
        return -1;
    }

    return 0;
}
 
 void wasapi_audio_stop(turbo_capture_t *capture) {
    if (!capture || !capture->platform_ctx) return;
    wasapi_capture_ctx_t *ctx = (wasapi_capture_ctx_t *)capture->platform_ctx;
    
    if (ctx->running) {
        ctx->running = 0;
        SetEvent(ctx->stop_event);

        if (ctx->capture_thread) {
            WaitForSingleObject(ctx->capture_thread, 5000);
            CloseHandle(ctx->capture_thread);
            ctx->capture_thread = NULL;
        }
    }
}
 
 void wasapi_audio_destroy(turbo_capture_t *capture) {
    if (!capture) return;

    wasapi_audio_stop(capture);

    wasapi_capture_ctx_t *ctx = (wasapi_capture_ctx_t *)capture->platform_ctx;
    if (ctx) {
        if (ctx->stop_event) CloseHandle(ctx->stop_event);
        if (ctx->capture_client) IAudioCaptureClient_Release(ctx->capture_client);
        if (ctx->audio_client) IAudioClient_Release(ctx->audio_client);
        if (ctx->wave_format) CoTaskMemFree(ctx->wave_format);
        if (ctx->device) IMMDevice_Release(ctx->device);
        if (ctx->enumerator) IMMDeviceEnumerator_Release(ctx->enumerator);
        if (ctx->resample_buf) free(ctx->resample_buf);
        free(ctx);
    }

    free(capture);
}

/* turbo_capture_get_state and turbo_capture_on_state moved to capture_win32.c */

#endif /* _WIN32 */
