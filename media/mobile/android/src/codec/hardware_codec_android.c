/**
 * Android Hardware Codec Implementation
 * Uses MediaCodec API for hardware-accelerated encoding/decoding
 */

#include "turbo_mobile_codec.h"
#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "HWCodec"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef struct {
    turbo_mobile_codec_t base;
    AMediaCodec* codec;
    AMediaFormat* format;
    bool is_encoder;
    int width;
    int height;
    int bitrate;
} android_hw_codec_t;

static int android_hw_encode(turbo_mobile_codec_t* codec,
                             const turbo_mobile_video_frame_t* frame,
                             uint8_t* output, size_t* output_size) {
    android_hw_codec_t* hw = (android_hw_codec_t*)codec;
    
    // Get input buffer
    ssize_t buf_idx = AMediaCodec_dequeueInputBuffer(hw->codec, 10000);
    if (buf_idx < 0) {
        LOGE("Failed to get input buffer: %zd", buf_idx);
        return -1;
    }
    
    size_t buf_size;
    uint8_t* buf = AMediaCodec_getInputBuffer(hw->codec, buf_idx, &buf_size);
    if (!buf) {
        LOGE("Failed to get input buffer pointer");
        return -1;
    }
    
    // Copy frame data
    size_t frame_size = frame->width * frame->height * 3 / 2; // YUV420
    if (frame_size > buf_size) {
        LOGE("Frame too large: %zu > %zu", frame_size, buf_size);
        return -1;
    }
    memcpy(buf, frame->data[0], frame_size);
    
    // Queue input buffer
    media_status_t status = AMediaCodec_queueInputBuffer(
        hw->codec, buf_idx, 0, frame_size, frame->timestamp, 0);
    if (status != AMEDIA_OK) {
        LOGE("Failed to queue input buffer: %d", status);
        return -1;
    }
    
    // Get output buffer
    AMediaCodecBufferInfo info;
    buf_idx = AMediaCodec_dequeueOutputBuffer(hw->codec, &info, 10000);
    if (buf_idx < 0) {
        return 0; // No output yet
    }
    
    buf = AMediaCodec_getOutputBuffer(hw->codec, buf_idx, &buf_size);
    if (!buf || info.size > *output_size) {
        AMediaCodec_releaseOutputBuffer(hw->codec, buf_idx, false);
        return -1;
    }
    
    memcpy(output, buf, info.size);
    *output_size = info.size;
    
    AMediaCodec_releaseOutputBuffer(hw->codec, buf_idx, false);
    return 0;
}

static int android_hw_decode(turbo_mobile_codec_t* codec,
                             const uint8_t* data,
                             size_t size,
                             turbo_mobile_video_frame_t* frame) {
    android_hw_codec_t* hw = (android_hw_codec_t*)codec;
    
    // Get input buffer
    ssize_t buf_idx = AMediaCodec_dequeueInputBuffer(hw->codec, 10000);
    if (buf_idx < 0) {
        return -1;
    }
    
    size_t buf_size;
    uint8_t* buf = AMediaCodec_getInputBuffer(hw->codec, buf_idx, &buf_size);
    if (!buf || size > buf_size) {
        return -1;
    }
    
    memcpy(buf, data, size);
    AMediaCodec_queueInputBuffer(hw->codec, buf_idx, 0, size, 0, 0);
    
    // Get output buffer
    AMediaCodecBufferInfo info;
    buf_idx = AMediaCodec_dequeueOutputBuffer(hw->codec, &info, 10000);
    if (buf_idx < 0) {
        return 0;
    }
    
    buf = AMediaCodec_getOutputBuffer(hw->codec, buf_idx, &buf_size);
    if (buf) {
        frame->width = hw->width;
        frame->height = hw->height;
        frame->format = TURBO_MOBILE_PIXEL_FORMAT_I420;
        memcpy(frame->data[0], buf, info.size);
        frame->data_len[0] = (size_t)info.size;
    }
    
    AMediaCodec_releaseOutputBuffer(hw->codec, buf_idx, false);
    return 0;
}

static void android_hw_destroy(turbo_mobile_codec_t* codec) {
    android_hw_codec_t* hw = (android_hw_codec_t*)codec;
    if (hw->codec) {
        AMediaCodec_stop(hw->codec);
        AMediaCodec_delete(hw->codec);
    }
    if (hw->format) {
        AMediaFormat_delete(hw->format);
    }
    free(hw);
}

turbo_mobile_codec_t* turbo_mobile_codec_android_hw_h264_create(bool is_encoder,
                                                                int width,
                                                                int height,
                                                                int bitrate) {
    android_hw_codec_t* hw = calloc(1, sizeof(android_hw_codec_t));
    if (!hw) return NULL;
    
    const char* mime = "video/avc";
    hw->codec = is_encoder ? 
        AMediaCodec_createEncoderByType(mime) :
        AMediaCodec_createDecoderByType(mime);
    
    if (!hw->codec) {
        LOGE("Failed to create codec");
        free(hw);
        return NULL;
    }
    
    hw->format = AMediaFormat_new();
    AMediaFormat_setString(hw->format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(hw->format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(hw->format, AMEDIAFORMAT_KEY_HEIGHT, height);
    
    if (is_encoder) {
        AMediaFormat_setInt32(hw->format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
        AMediaFormat_setInt32(hw->format, AMEDIAFORMAT_KEY_FRAME_RATE, 30);
        AMediaFormat_setInt32(hw->format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 2);
        AMediaFormat_setInt32(hw->format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21); // YUV420
    }
    
    if (AMediaCodec_configure(hw->codec, hw->format, NULL, NULL, 
                             is_encoder ? AMEDIACODEC_CONFIGURE_FLAG_ENCODE : 0) != AMEDIA_OK) {
        LOGE("Failed to configure codec");
        android_hw_destroy((turbo_mobile_codec_t*)hw);
        return NULL;
    }
    
    if (AMediaCodec_start(hw->codec) != AMEDIA_OK) {
        LOGE("Failed to start codec");
        android_hw_destroy((turbo_mobile_codec_t*)hw);
        return NULL;
    }
    
    hw->base.encode = android_hw_encode;
    hw->base.decode = android_hw_decode;
    hw->base.destroy = android_hw_destroy;
    hw->is_encoder = is_encoder;
    hw->width = width;
    hw->height = height;
    hw->bitrate = bitrate;
    
    LOGI("Created hardware %s codec: %dx%d @ %d bps", 
         is_encoder ? "encoder" : "decoder", width, height, bitrate);
    
    return (turbo_mobile_codec_t*)hw;
}
