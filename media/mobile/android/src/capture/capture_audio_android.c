/**
 * Android Audio Capture
 * 
 * Uses OpenSL ES for low-latency audio capture
 * Falls back to miniaudio if OpenSL ES not available
 */
#include <android/log.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <stdlib.h>
#include <string.h>

#include "miniaudio.h"

#define LOG_TAG "TurboNetAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define BUFFER_COUNT 4
#define FRAMES_PER_BUFFER 480  /* 10ms @ 48kHz */

/* =============================================================================
 * Audio Context
 * ============================================================================= */

typedef struct android_audio_ctx_t {
    /* OpenSL ES objects */
    SLObjectItf engine_object;
    SLEngineItf engine;
    SLObjectItf recorder_object;
    SLRecordItf recorder;
    SLAndroidSimpleBufferQueueItf buffer_queue;
    
    /* Configuration */
    int sample_rate;
    int channels;
    int use_opensles;
    
    /* Buffers */
    int16_t *buffers[BUFFER_COUNT];
    size_t buffer_size;
    int current_buffer;
    
    /* State */
    int capturing;
    
    /* Miniaudio fallback */
    ma_device miniaudio_device;
    
    /* Callback */
    void (*on_audio)(void *user_data, const int16_t *data, size_t frames);
    void *user_data;
} android_audio_ctx_t;

int android_audio_stop(android_audio_ctx_t *ctx);

/* =============================================================================
 * OpenSL ES Implementation
 * ============================================================================= */

static void opensles_buffer_queue_callback(SLAndroidSimpleBufferQueueItf bq, void *context) {
    android_audio_ctx_t *ctx = (android_audio_ctx_t *)context;
    
    /* Get current buffer */
    int16_t *buffer = ctx->buffers[ctx->current_buffer];
    
    /* Call user callback */
    if (ctx->on_audio) {
        ctx->on_audio(ctx->user_data, buffer, FRAMES_PER_BUFFER);
    }
    
    /* Enqueue buffer again */
    (*bq)->Enqueue(bq, buffer, ctx->buffer_size);
    
    /* Move to next buffer */
    ctx->current_buffer = (ctx->current_buffer + 1) % BUFFER_COUNT;
}

static int opensles_init(android_audio_ctx_t *ctx) {
    SLresult result;
    
    /* Create engine */
    result = slCreateEngine(&ctx->engine_object, 0, NULL, 0, NULL, NULL);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to create OpenSL ES engine");
        return -1;
    }
    
    result = (*ctx->engine_object)->Realize(ctx->engine_object, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to realize engine");
        return -1;
    }
    
    result = (*ctx->engine_object)->GetInterface(ctx->engine_object,
                                                 SL_IID_ENGINE,
                                                 &ctx->engine);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to get engine interface");
        return -1;
    }
    
    /* Configure audio source */
    SLDataLocator_IODevice loc_dev = {
        SL_DATALOCATOR_IODEVICE,
        SL_IODEVICE_AUDIOINPUT,
        SL_DEFAULTDEVICEID_AUDIOINPUT,
        NULL
    };
    
    SLDataSource audio_src = {&loc_dev, NULL};
    
    /* Configure audio sink */
    SLDataLocator_AndroidSimpleBufferQueue loc_bq = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
        BUFFER_COUNT
    };
    
    SLDataFormat_PCM format_pcm = {
        SL_DATAFORMAT_PCM,
        ctx->channels,
        ctx->sample_rate * 1000,  /* mHz */
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        ctx->channels == 1 ? SL_SPEAKER_FRONT_CENTER :
                            (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT),
        SL_BYTEORDER_LITTLEENDIAN
    };
    
    SLDataSink audio_snk = {&loc_bq, &format_pcm};
    
    /* Create audio recorder */
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req[] = {SL_BOOLEAN_TRUE};
    
    result = (*ctx->engine)->CreateAudioRecorder(
        ctx->engine,
        &ctx->recorder_object,
        &audio_src,
        &audio_snk,
        1,
        ids,
        req
    );
    
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to create audio recorder");
        return -1;
    }
    
    result = (*ctx->recorder_object)->Realize(ctx->recorder_object, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to realize recorder");
        return -1;
    }
    
    /* Get recorder interface */
    result = (*ctx->recorder_object)->GetInterface(ctx->recorder_object,
                                                   SL_IID_RECORD,
                                                   &ctx->recorder);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to get recorder interface");
        return -1;
    }
    
    /* Get buffer queue interface */
    result = (*ctx->recorder_object)->GetInterface(ctx->recorder_object,
                                                   SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                   &ctx->buffer_queue);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to get buffer queue interface");
        return -1;
    }
    
    /* Register callback */
    result = (*ctx->buffer_queue)->RegisterCallback(ctx->buffer_queue,
                                                    opensles_buffer_queue_callback,
                                                    ctx);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to register callback");
        return -1;
    }
    
    LOGI("OpenSL ES initialized");
    
    return 0;
}

static int opensles_start(android_audio_ctx_t *ctx) {
    /* Enqueue all buffers */
    for (int i = 0; i < BUFFER_COUNT; i++) {
        (*ctx->buffer_queue)->Enqueue(ctx->buffer_queue,
                                      ctx->buffers[i],
                                      ctx->buffer_size);
    }
    
    /* Start recording */
    SLresult result = (*ctx->recorder)->SetRecordState(ctx->recorder,
                                                       SL_RECORDSTATE_RECORDING);
    if (result != SL_RESULT_SUCCESS) {
        LOGE("Failed to start recording");
        return -1;
    }
    
    LOGI("OpenSL ES recording started");
    
    return 0;
}

static int opensles_stop(android_audio_ctx_t *ctx) {
    if (ctx->recorder) {
        (*ctx->recorder)->SetRecordState(ctx->recorder, SL_RECORDSTATE_STOPPED);
    }
    
    if (ctx->buffer_queue) {
        (*ctx->buffer_queue)->Clear(ctx->buffer_queue);
    }
    
    LOGI("OpenSL ES recording stopped");
    
    return 0;
}

static void opensles_cleanup(android_audio_ctx_t *ctx) {
    if (ctx->recorder_object) {
        (*ctx->recorder_object)->Destroy(ctx->recorder_object);
        ctx->recorder_object = NULL;
    }
    
    if (ctx->engine_object) {
        (*ctx->engine_object)->Destroy(ctx->engine_object);
        ctx->engine_object = NULL;
    }
}

/* =============================================================================
 * Miniaudio Fallback Implementation
 * ============================================================================= */

static void miniaudio_data_callback(ma_device *device, void *output,
                                   const void *input, ma_uint32 frame_count) {
    android_audio_ctx_t *ctx = (android_audio_ctx_t *)device->pUserData;
    
    if (ctx->on_audio && input) {
        ctx->on_audio(ctx->user_data, (const int16_t *)input, frame_count);
    }
}

static int miniaudio_init(android_audio_ctx_t *ctx) {
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = ctx->channels;
    config.sampleRate = ctx->sample_rate;
    config.dataCallback = miniaudio_data_callback;
    config.pUserData = ctx;
    
    if (ma_device_init(NULL, &config, &ctx->miniaudio_device) != MA_SUCCESS) {
        LOGE("Failed to initialize miniaudio");
        return -1;
    }
    
    LOGI("Miniaudio initialized");
    
    return 0;
}

static int miniaudio_start(android_audio_ctx_t *ctx) {
    if (ma_device_start(&ctx->miniaudio_device) != MA_SUCCESS) {
        LOGE("Failed to start miniaudio");
        return -1;
    }
    
    LOGI("Miniaudio recording started");
    
    return 0;
}

static int miniaudio_stop(android_audio_ctx_t *ctx) {
    ma_device_stop(&ctx->miniaudio_device);
    
    LOGI("Miniaudio recording stopped");
    
    return 0;
}

static void miniaudio_cleanup(android_audio_ctx_t *ctx) {
    ma_device_uninit(&ctx->miniaudio_device);
}

/* =============================================================================
 * Public API
 * ============================================================================= */

android_audio_ctx_t *android_audio_create(int sample_rate, int channels, int use_opensles) {
    android_audio_ctx_t *ctx = (android_audio_ctx_t *)calloc(1, sizeof(android_audio_ctx_t));
    if (!ctx) return NULL;
    
    ctx->sample_rate = sample_rate;
    ctx->channels = channels;
    ctx->use_opensles = use_opensles;
    ctx->capturing = 0;
    ctx->current_buffer = 0;
    
    /* Allocate buffers */
    ctx->buffer_size = FRAMES_PER_BUFFER * channels * sizeof(int16_t);
    for (int i = 0; i < BUFFER_COUNT; i++) {
        ctx->buffers[i] = (int16_t *)malloc(ctx->buffer_size);
        if (!ctx->buffers[i]) {
            for (int j = 0; j < i; j++) {
                free(ctx->buffers[j]);
            }
            free(ctx);
            return NULL;
        }
    }
    
    /* Initialize audio backend */
    int ret;
    if (use_opensles) {
        ret = opensles_init(ctx);
        if (ret != 0) {
            LOGI("OpenSL ES failed, falling back to miniaudio");
            ctx->use_opensles = 0;
            ret = miniaudio_init(ctx);
        }
    } else {
        ret = miniaudio_init(ctx);
    }
    
    if (ret != 0) {
        for (int i = 0; i < BUFFER_COUNT; i++) {
            free(ctx->buffers[i]);
        }
        free(ctx);
        return NULL;
    }
    
    LOGI("Audio created: %dHz %dch (%s)", sample_rate, channels,
         ctx->use_opensles ? "OpenSL ES" : "miniaudio");
    
    return ctx;
}

void android_audio_destroy(android_audio_ctx_t *ctx) {
    if (!ctx) return;
    
    if (ctx->capturing) {
        android_audio_stop(ctx);
    }
    
    if (ctx->use_opensles) {
        opensles_cleanup(ctx);
    } else {
        miniaudio_cleanup(ctx);
    }
    
    for (int i = 0; i < BUFFER_COUNT; i++) {
        free(ctx->buffers[i]);
    }
    
    free(ctx);
}

int android_audio_start(android_audio_ctx_t *ctx) {
    if (!ctx || ctx->capturing) return -1;
    
    int ret;
    if (ctx->use_opensles) {
        ret = opensles_start(ctx);
    } else {
        ret = miniaudio_start(ctx);
    }
    
    if (ret == 0) {
        ctx->capturing = 1;
    }
    
    return ret;
}

int android_audio_stop(android_audio_ctx_t *ctx) {
    if (!ctx || !ctx->capturing) return -1;
    
    if (ctx->use_opensles) {
        opensles_stop(ctx);
    } else {
        miniaudio_stop(ctx);
    }
    
    ctx->capturing = 0;
    
    return 0;
}

void android_audio_set_callback(android_audio_ctx_t *ctx,
                                void (*callback)(void *user_data,
                                               const int16_t *data, size_t frames),
                                void *user_data) {
    if (!ctx) return;
    ctx->on_audio = callback;
    ctx->user_data = user_data;
}
