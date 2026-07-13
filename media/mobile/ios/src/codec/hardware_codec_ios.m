/**
 * iOS Hardware Codec Implementation
 * Uses VideoToolbox for hardware-accelerated H.264/HEVC encoding/decoding
 */

#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include "turbo_mobile_codec.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    turbo_mobile_codec_t base;
    VTCompressionSessionRef encoder;
    VTDecompressionSessionRef decoder;
    bool is_encoder;
    int width;
    int height;
    int bitrate;
    int fps;
    CMVideoCodecType codec_type;
} ios_hw_codec_t;

// Encoder callback
static void compressionOutputCallback(void* outputCallbackRefCon,
                                     void* sourceFrameRefCon,
                                     OSStatus status,
                                     VTEncodeInfoFlags infoFlags,
                                     CMSampleBufferRef sampleBuffer) {
    if (status != noErr) {
        NSLog(@"[HWCodec] Encoding error: %d", (int)status);
        return;
    }
    
    if (!sampleBuffer) return;
    
    // Get encoded data
    CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!blockBuffer) return;
    
    size_t length;
    char* dataPointer;
    CMBlockBufferGetDataPointer(blockBuffer, 0, NULL, &length, &dataPointer);
    
    // Store in user context if needed
    // This is handled by the encode function
}

static int ios_hw_encode(turbo_mobile_codec_t* codec,
                        const turbo_mobile_video_frame_t* frame,
                        uint8_t* output, size_t* output_size) {
    ios_hw_codec_t* hw = (ios_hw_codec_t*)codec;
    
    @autoreleasepool {
        // Create pixel buffer from frame data
        CVPixelBufferRef pixelBuffer = NULL;
        CVReturn status = CVPixelBufferCreate(
            kCFAllocatorDefault,
            frame->width,
            frame->height,
            kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            NULL,
            &pixelBuffer
        );
        
        if (status != kCVReturnSuccess || !pixelBuffer) {
            NSLog(@"[HWCodec] Failed to create pixel buffer: %d", status);
            return -1;
        }
        
        // Copy frame data to pixel buffer
        CVPixelBufferLockBaseAddress(pixelBuffer, 0);
        
        uint8_t* yDest = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
        uint8_t* uvDest = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);
        
        size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
        size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
        
        // Copy Y plane
        for (int y = 0; y < frame->height; y++) {
            memcpy(yDest + y * yStride, 
                   frame->data[0] + y * frame->linesize[0], 
                   frame->width);
        }
        
        // Copy UV plane
        for (int y = 0; y < frame->height / 2; y++) {
            memcpy(uvDest + y * uvStride, 
                   frame->data[1] + y * frame->linesize[1], 
                   frame->width);
        }
        
        CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
        
        // Encode frame
        CMTime presentationTime = CMTimeMake(frame->timestamp, 1000000);
        CMTime duration = CMTimeMake(1, hw->fps);
        
        VTEncodeInfoFlags flags;
        status = VTCompressionSessionEncodeFrame(
            hw->encoder,
            pixelBuffer,
            presentationTime,
            duration,
            NULL,
            NULL,
            &flags
        );
        
        CVPixelBufferRelease(pixelBuffer);
        
        if (status != noErr) {
            NSLog(@"[HWCodec] Encode failed: %d", (int)status);
            return -1;
        }
        
        // Get encoded data (simplified - in real implementation, use callback)
        *output_size = 0;
        return 0;
    }
}

static int ios_hw_decode(turbo_mobile_codec_t* codec,
                        const uint8_t* data,
                        size_t size,
                        turbo_mobile_video_frame_t* frame) {
    ios_hw_codec_t* hw = (ios_hw_codec_t*)codec;
    
    @autoreleasepool {
        // Create block buffer from data
        CMBlockBufferRef blockBuffer = NULL;
        OSStatus status = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault,
            (void*)data,
            size,
            kCFAllocatorNull,
            NULL,
            0,
            size,
            0,
            &blockBuffer
        );
        
        if (status != noErr || !blockBuffer) {
            NSLog(@"[HWCodec] Failed to create block buffer: %d", (int)status);
            return -1;
        }
        
        // Create sample buffer
        CMSampleBufferRef sampleBuffer = NULL;
        CMFormatDescriptionRef formatDesc = NULL;
        
        // Simplified - in real implementation, parse format from data
        
        // Decode
        VTDecodeFrameFlags flags = 0;
        VTDecodeInfoFlags infoFlags;
        
        status = VTDecompressionSessionDecodeFrame(
            hw->decoder,
            sampleBuffer,
            flags,
            NULL,
            &infoFlags
        );
        
        if (blockBuffer) CFRelease(blockBuffer);
        if (sampleBuffer) CFRelease(sampleBuffer);
        
        if (status != noErr) {
            NSLog(@"[HWCodec] Decode failed: %d", (int)status);
            return -1;
        }
        
        return 0;
    }
}

static void ios_hw_destroy(turbo_mobile_codec_t* codec) {
    ios_hw_codec_t* hw = (ios_hw_codec_t*)codec;
    
    @autoreleasepool {
        if (hw->encoder) {
            VTCompressionSessionInvalidate(hw->encoder);
            CFRelease(hw->encoder);
        }
        
        if (hw->decoder) {
            VTDecompressionSessionInvalidate(hw->decoder);
            CFRelease(hw->decoder);
        }
        
        free(hw);
    }
}

turbo_mobile_codec_t* turbo_mobile_codec_ios_hw_h264_create(bool is_encoder,
                                                            int width,
                                                            int height,
                                                            int bitrate,
                                                            int fps) {
    @autoreleasepool {
        ios_hw_codec_t* hw = calloc(1, sizeof(ios_hw_codec_t));
        if (!hw) return NULL;
        
        hw->is_encoder = is_encoder;
        hw->width = width;
        hw->height = height;
        hw->bitrate = bitrate;
        hw->fps = fps;
        hw->codec_type = kCMVideoCodecType_H264;
        
        if (is_encoder) {
            // Create encoder
            NSDictionary* encoderSpec = @{
                (NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder: @YES
            };
            
            NSDictionary* sourceImageBufferAttributes = @{
                (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
                (NSString*)kCVPixelBufferWidthKey: @(width),
                (NSString*)kCVPixelBufferHeightKey: @(height)
            };
            
            OSStatus status = VTCompressionSessionCreate(
                kCFAllocatorDefault,
                width,
                height,
                kCMVideoCodecType_H264,
                (__bridge CFDictionaryRef)encoderSpec,
                (__bridge CFDictionaryRef)sourceImageBufferAttributes,
                NULL,
                compressionOutputCallback,
                NULL,
                &hw->encoder
            );
            
            if (status != noErr || !hw->encoder) {
                NSLog(@"[HWCodec] Failed to create encoder: %d", (int)status);
                free(hw);
                return NULL;
            }
            
            // Set encoder properties
            VTSessionSetProperty(hw->encoder, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
            VTSessionSetProperty(hw->encoder, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_Main_AutoLevel);
            
            CFNumberRef bitrateNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bitrate);
            VTSessionSetProperty(hw->encoder, kVTCompressionPropertyKey_AverageBitRate, bitrateNum);
            CFRelease(bitrateNum);
            
            int maxKeyFrameInterval = fps * 2; // Keyframe every 2 seconds
            CFNumberRef keyFrameNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &maxKeyFrameInterval);
            VTSessionSetProperty(hw->encoder, kVTCompressionPropertyKey_MaxKeyFrameInterval, keyFrameNum);
            CFRelease(keyFrameNum);
            
            VTCompressionSessionPrepareToEncodeFrames(hw->encoder);
            
            NSLog(@"[HWCodec] Created H.264 encoder: %dx%d @ %d bps, %d fps", 
                  width, height, bitrate, fps);
        } else {
            // Create decoder
            NSDictionary* decoderSpec = @{
                (NSString*)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: @YES
            };
            
            // Simplified - in real implementation, create format description from SPS/PPS
            CMFormatDescriptionRef formatDesc = NULL;
            
            VTDecompressionOutputCallbackRecord callback = {0};
            
            OSStatus status = VTDecompressionSessionCreate(
                kCFAllocatorDefault,
                formatDesc,
                (__bridge CFDictionaryRef)decoderSpec,
                NULL,
                &callback,
                &hw->decoder
            );
            
            if (status != noErr || !hw->decoder) {
                NSLog(@"[HWCodec] Failed to create decoder: %d", (int)status);
                free(hw);
                return NULL;
            }
            
            NSLog(@"[HWCodec] Created H.264 decoder: %dx%d", width, height);
        }
        
        hw->base.encode = ios_hw_encode;
        hw->base.decode = ios_hw_decode;
        hw->base.destroy = ios_hw_destroy;
        
        return (turbo_mobile_codec_t*)hw;
    }
}

turbo_mobile_codec_t* turbo_mobile_codec_ios_hw_hevc_create(bool is_encoder,
                                                            int width,
                                                            int height,
                                                            int bitrate,
                                                            int fps) {
    // Similar to H.264 but with kCMVideoCodecType_HEVC
    // Implementation omitted for brevity
    return NULL;
}
