/**
 * H.264/AVC Video Codec Implementation
 * 
 * Uses OpenH264 library for encoding/decoding
 * Most widely supported video codec
 */
#include "turbo_codec.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef TURBO_MEDIA_HAS_H264

#include <wels/codec_api.h>
#include <wels/codec_app_def.h>
#include <wels/codec_def.h>

/* =============================================================================
 * H.264 Context
 * ============================================================================= */

typedef struct {
    ISVCEncoder *encoder;
    ISVCDecoder *decoder;
    
    /* Configuration */
    int width;
    int height;
    int framerate;
    int bitrate;
    int keyframe_interval;
    
    /* State */
    int frame_count;
    uint8_t *sps;
    size_t sps_len;
    uint8_t *pps;
    size_t pps_len;

    uint8_t *packet_bufs[TURBO_CODEC_MAX_PACKETS];
    size_t packet_caps[TURBO_CODEC_MAX_PACKETS];
    uint8_t *reassembly_buf;
    size_t reassembly_len;
    size_t reassembly_cap;
    int reassembly_started;
} h264_context_t;

static size_t h264_start_code_len(const uint8_t *data, size_t len) {
    if (!data || len < 3) {
        return 0;
    }
    if (len >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
        data[2] == 0x00 && data[3] == 0x01) {
        return 4;
    }
    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
        return 3;
    }
    return 0;
}

static int h264_store_param_set(uint8_t **dst, size_t *dst_len,
                                const uint8_t *nal, size_t nal_len) {
    uint8_t *copy;

    if (!dst || !dst_len || !nal || nal_len == 0) {
        return TURBO_CODEC_ERR_INVALID;
    }

    copy = (uint8_t *)realloc(*dst, nal_len);
    if (!copy) {
        return TURBO_CODEC_ERR_NOMEM;
    }

    memcpy(copy, nal, nal_len);
    *dst = copy;
    *dst_len = nal_len;
    return TURBO_CODEC_OK;
}

static void h264_normalize_sps_profile(uint8_t *nal, size_t nal_len) {
    if (!nal || nal_len < 4) {
        return;
    }

    /* Chrome commonly offers constrained-baseline (42e01f). OpenH264 emits
     * baseline-compatible SPS with only the high constraint flags set, so
     * normalize the constraint byte to constrained-baseline in-band. */
    if (nal[0] == 0x67 && nal[1] == 0x42 && (nal[2] & 0xE0) == 0xC0) {
        nal[2] = (uint8_t)(nal[2] | 0x20);
    }
}

static int h264_append_annexb_nal(uint8_t *output, size_t output_cap, size_t *offset,
                                  const uint8_t *nal, size_t nal_len) {
    if (!output || !offset || !nal || nal_len == 0) {
        return TURBO_CODEC_ERR_INVALID;
    }
    if (*offset + 4 + nal_len > output_cap) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    output[(*offset)++] = 0x00;
    output[(*offset)++] = 0x00;
    output[(*offset)++] = 0x00;
    output[(*offset)++] = 0x01;
    memcpy(output + *offset, nal, nal_len);
    *offset += nal_len;
    return TURBO_CODEC_OK;
}

static int h264_ensure_packet_buf(h264_context_t *ctx, int index, size_t needed) {
    uint8_t *buf;

    if (!ctx || index < 0 || index >= TURBO_CODEC_MAX_PACKETS) {
        return TURBO_CODEC_ERR_INVALID;
    }
    if (ctx->packet_caps[index] >= needed) {
        return TURBO_CODEC_OK;
    }

    buf = (uint8_t *)realloc(ctx->packet_bufs[index], needed);
    if (!buf) {
        return TURBO_CODEC_ERR_NOMEM;
    }

    ctx->packet_bufs[index] = buf;
    ctx->packet_caps[index] = needed;
    return TURBO_CODEC_OK;
}

static size_t h264_find_start_code_at(const uint8_t *data, size_t len, size_t pos, int *start_code_len) {
    if (!data || !start_code_len || pos >= len) {
        return len;
    }

    for (size_t i = pos; i + 3 < len; i++) {
        if (data[i] == 0 && data[i + 1] == 0 &&
            ((data[i + 2] == 0 && data[i + 3] == 1) || data[i + 2] == 1)) {
            *start_code_len = (data[i + 2] == 1) ? 3 : 4;
            return i;
        }
    }

    *start_code_len = 0;
    return len;
}

static int h264_find_nal(const uint8_t *data, size_t len, size_t pos,
                         size_t *nal_start, size_t *nal_end) {
    int start_code_len = 0;
    size_t start = h264_find_start_code_at(data, len, pos, &start_code_len);
    size_t next_start;
    int next_start_code_len = 0;

    if (start >= len || start_code_len == 0) {
        return 0;
    }

    *nal_start = start + (size_t)start_code_len;
    next_start = h264_find_start_code_at(data, len, *nal_start, &next_start_code_len);
    *nal_end = (next_start < len) ? next_start : len;
    return (*nal_start < *nal_end) ? 1 : 0;
}

/* =============================================================================
 * Encoder Operations
 * ============================================================================= */

static void *h264_create_encoder(const void *config) {
    const turbo_video_codec_config_t *cfg = (const turbo_video_codec_config_t *)config;
    enum { H264_MAX_FRAMERATE = 240 };
    if (!cfg || cfg->width <= 0 || cfg->width > TURBO_VIDEO_MAX_WIDTH ||
        cfg->height <= 0 || cfg->height > TURBO_VIDEO_MAX_HEIGHT ||
        cfg->framerate <= 0 || cfg->framerate > H264_MAX_FRAMERATE ||
        cfg->bitrate < 0 || cfg->keyframe_interval < 0 || cfg->threads < 0) {
        return NULL;
    }
    
    h264_context_t *ctx = (h264_context_t *)calloc(1, sizeof(h264_context_t));
    if (!ctx) return NULL;
    
    ctx->width = cfg->width;
    ctx->height = cfg->height;
    ctx->framerate = cfg->framerate > 0 ? cfg->framerate : 30;
    ctx->bitrate = cfg->bitrate > 0 ? cfg->bitrate : 1000000;
    ctx->keyframe_interval = cfg->keyframe_interval > 0 ? cfg->keyframe_interval : 60;
    
    /* Create encoder */
    if (WelsCreateSVCEncoder(&ctx->encoder) != 0) {
        free(ctx);
        return NULL;
    }
    
    /* Initialize encoder parameters */
    SEncParamExt param;
    memset(&param, 0, sizeof(SEncParamExt));
    
    if ((*ctx->encoder)->GetDefaultParams(ctx->encoder, &param) != 0) {
        WelsDestroySVCEncoder(ctx->encoder);
        free(ctx);
        return NULL;
    }
    
    /* Set parameters */
    param.iUsageType = CAMERA_VIDEO_REAL_TIME;
    param.fMaxFrameRate = (float)ctx->framerate;
    param.iPicWidth = ctx->width;
    param.iPicHeight = ctx->height;
    param.iTargetBitrate = ctx->bitrate;
    param.iRCMode = RC_BITRATE_MODE;
    param.iTemporalLayerNum = 1;
    param.iSpatialLayerNum = 1;
    param.bEnableDenoise = 0;
    param.bEnableBackgroundDetection = 1;
    param.bEnableAdaptiveQuant = 1;
    param.bEnableFrameSkip = 1;
    param.bEnableLongTermReference = 0;
    param.iLtrMarkPeriod = 30;
    param.uiIntraPeriod = ctx->keyframe_interval;
    param.eSpsPpsIdStrategy = CONSTANT_ID;
    param.bPrefixNalAddingCtrl = 0;
    param.iLoopFilterDisableIdc = 0;
    param.iEntropyCodingModeFlag = 0;
    param.iMultipleThreadIdc = 1;
    
    /* Spatial layer config */
    param.sSpatialLayers[0].iVideoWidth = ctx->width;
    param.sSpatialLayers[0].iVideoHeight = ctx->height;
    param.sSpatialLayers[0].fFrameRate = (float)ctx->framerate;
    param.sSpatialLayers[0].iSpatialBitrate = ctx->bitrate;
    param.sSpatialLayers[0].iMaxSpatialBitrate = ctx->bitrate * 2;
    param.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
    param.sSpatialLayers[0].uiLevelIdc = LEVEL_3_1;
    param.sSpatialLayers[0].iDLayerQp = 24;
    
    param.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
    
    /* Initialize encoder */
    if ((*ctx->encoder)->InitializeExt(ctx->encoder, &param) != 0) {
        WelsDestroySVCEncoder(ctx->encoder);
        free(ctx);
        return NULL;
    }
    
    /* Set options */
    int video_format = videoFormatI420;
    (*ctx->encoder)->SetOption(ctx->encoder, ENCODER_OPTION_DATAFORMAT, &video_format);
    (*ctx->encoder)->SetOption(ctx->encoder, ENCODER_OPTION_IDR_INTERVAL, &ctx->keyframe_interval);
    
    return ctx;
}

static void *h264_create_decoder(const void *config) {
    h264_context_t *ctx = (h264_context_t *)calloc(1, sizeof(h264_context_t));
    if (!ctx) return NULL;

    ctx->reassembly_cap = TURBO_CODEC_MAX_FRAME_SIZE;
    ctx->reassembly_buf = (uint8_t *)malloc(ctx->reassembly_cap);
    if (!ctx->reassembly_buf) {
        free(ctx);
        return NULL;
    }
    
    /* Create decoder */
    if (WelsCreateDecoder(&ctx->decoder) != 0) {
        free(ctx->reassembly_buf);
        free(ctx);
        return NULL;
    }
    
    /* Initialize decoder parameters */
    SDecodingParam param;
    memset(&param, 0, sizeof(SDecodingParam));
    
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_DEFAULT;
    param.uiTargetDqLayer = UCHAR_MAX;
    param.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    param.bParseOnly = false;
    
    if ((*ctx->decoder)->Initialize(ctx->decoder, &param) != 0) {
        WelsDestroyDecoder(ctx->decoder);
        free(ctx->reassembly_buf);
        free(ctx);
        return NULL;
    }
    
    return ctx;
}

static void h264_destroy(void *ctx) {
    h264_context_t *h264 = (h264_context_t *)ctx;
    if (!h264) return;
    
    if (h264->encoder) {
        (*h264->encoder)->Uninitialize(h264->encoder);
        WelsDestroySVCEncoder(h264->encoder);
    }
    
    if (h264->decoder) {
        (*h264->decoder)->Uninitialize(h264->decoder);
        WelsDestroyDecoder(h264->decoder);
    }
    
    if (h264->sps) free(h264->sps);
    if (h264->pps) free(h264->pps);
    for (int i = 0; i < TURBO_CODEC_MAX_PACKETS; i++) {
        free(h264->packet_bufs[i]);
    }
    free(h264->reassembly_buf);
    
    free(h264);
}

static int h264_encode(void *ctx,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len,
                       turbo_encoded_frame_t *info) {
    h264_context_t *h264 = (h264_context_t *)ctx;
    if (!h264 || !h264->encoder || !input || !output || !output_len) {
        return TURBO_CODEC_ERR_INVALID;
    }
    
    /* Prepare source picture (I420 format) */
    SSourcePicture pic;
    memset(&pic, 0, sizeof(SSourcePicture));
    
    pic.iPicWidth = h264->width;
    pic.iPicHeight = h264->height;
    pic.iColorFormat = videoFormatI420;
    pic.iStride[0] = h264->width;
    pic.iStride[1] = h264->width / 2;
    pic.iStride[2] = h264->width / 2;
    
    size_t y_size = h264->width * h264->height;
    size_t uv_size = y_size / 4;
    
    pic.pData[0] = (unsigned char *)input;
    pic.pData[1] = (unsigned char *)input + y_size;
    pic.pData[2] = (unsigned char *)input + y_size + uv_size;
    
    if (h264->frame_count == 0 ||
        (h264->keyframe_interval > 0 && (h264->frame_count % h264->keyframe_interval) == 0)) {
        (*h264->encoder)->ForceIntraFrame(h264->encoder, true);
    }

    /* Encode frame */
    SFrameBSInfo frame_info;
    memset(&frame_info, 0, sizeof(SFrameBSInfo));
    
    int ret = (*h264->encoder)->EncodeFrame(h264->encoder, &pic, &frame_info);
    if (ret != cmResultSuccess) {
        return TURBO_CODEC_ERR_CODEC;
    }
    
    /* Copy encoded data */
    size_t total_len = 0;
    int is_keyframe = 0;
    
    if (frame_info.eFrameType != videoFrameTypeSkip) {
        for (int layer = 0; layer < frame_info.iLayerNum; layer++) {
            SLayerBSInfo *layer_info = &frame_info.sLayerInfo[layer];
            const uint8_t *layer_bs = layer_info->pBsBuf;
            int saw_sps = 0;
            int saw_pps = 0;
            
            for (int nal = 0; nal < layer_info->iNalCount; nal++) {
                size_t nal_len = layer_info->pNalLengthInByte[nal];
                size_t start_code_len;
                const uint8_t *nal_data;
                uint8_t nal_type;
                int rc;

                start_code_len = h264_start_code_len(layer_bs, nal_len);
                if (start_code_len >= nal_len) {
                    layer_bs += nal_len;
                    continue;
                }

                nal_data = layer_bs + start_code_len;
                nal_len -= start_code_len;
                nal_type = (uint8_t)(nal_data[0] & 0x1F);

                if (nal_type == 5) {
                    if (!saw_sps && h264->sps && h264->sps_len > 0) {
                        rc = h264_append_annexb_nal(output, *output_len, &total_len,
                                                    h264->sps, h264->sps_len);
                        if (rc != TURBO_CODEC_OK) {
                            return rc;
                        }
                        saw_sps = 1;
                    }
                    if (!saw_pps && h264->pps && h264->pps_len > 0) {
                        rc = h264_append_annexb_nal(output, *output_len, &total_len,
                                                    h264->pps, h264->pps_len);
                        if (rc != TURBO_CODEC_OK) {
                            return rc;
                        }
                        saw_pps = 1;
                    }
                }

                if (nal_type == 7) {
                    rc = h264_store_param_set(&h264->sps, &h264->sps_len, nal_data, nal_len);
                    if (rc != TURBO_CODEC_OK) {
                        return rc;
                    }
                    h264_normalize_sps_profile(h264->sps, h264->sps_len);
                    saw_sps = 1;
                } else if (nal_type == 8) {
                    rc = h264_store_param_set(&h264->pps, &h264->pps_len, nal_data, nal_len);
                    if (rc != TURBO_CODEC_OK) {
                        return rc;
                    }
                    saw_pps = 1;
                }

                if (nal_type == 7 && h264->sps && h264->sps_len == nal_len) {
                    rc = h264_append_annexb_nal(output, *output_len, &total_len,
                                                h264->sps, h264->sps_len);
                } else {
                    rc = h264_append_annexb_nal(output, *output_len, &total_len,
                                                nal_data, nal_len);
                }
                if (rc != TURBO_CODEC_OK) {
                    return rc;
                }
                layer_bs += layer_info->pNalLengthInByte[nal];
            }
        }
        
        is_keyframe = (frame_info.eFrameType == videoFrameTypeIDR);
    }
    
    *output_len = total_len;
    
    if (info) {
        info->data = output;
        info->len = total_len;
        info->is_keyframe = is_keyframe;
        info->packet_count = (int)((total_len + 1199) / 1200); /* Estimate RTP packets */
    }
    
    h264->frame_count++;
    
    return TURBO_CODEC_OK;
}

static int h264_decode(void *ctx,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len) {
    h264_context_t *h264 = (h264_context_t *)ctx;
    if (!h264 || !h264->decoder || !input || !output || !output_len) {
        return TURBO_CODEC_ERR_INVALID;
    }
    
    uint8_t *dst[3] = {NULL};
    SBufferInfo buf_info;
    memset(&buf_info, 0, sizeof(SBufferInfo));
    
    /* AVC is best handled with the no-delay path so a complete access unit
     * can produce output immediately without requiring a follow-up flush call. */
    DECODING_STATE state = (*h264->decoder)->DecodeFrameNoDelay(
        h264->decoder,
        (unsigned char *)input,
        (int)input_len,
        dst,
        &buf_info
    );
    
    if (state != dsErrorFree) {
        return TURBO_CODEC_ERR_CODEC;
    }
    
    if (buf_info.iBufferStatus != 1) {
        state = (*h264->decoder)->DecodeFrame2(
            h264->decoder,
            NULL,
            0,
            dst,
            &buf_info
        );
        if (state != dsErrorFree) {
            return TURBO_CODEC_ERR_CODEC;
        }
        if (buf_info.iBufferStatus != 1) {
            return TURBO_CODEC_ERR_NEED_MORE;
        }
    }
    
    /* Copy decoded frame (I420 format) */
    int width = buf_info.UsrData.sSystemBuffer.iWidth;
    int height = buf_info.UsrData.sSystemBuffer.iHeight;
    int stride[2] = {
        buf_info.UsrData.sSystemBuffer.iStride[0],
        buf_info.UsrData.sSystemBuffer.iStride[1]
    };
    
    size_t y_size = width * height;
    size_t uv_size = y_size / 4;
    size_t required_size = y_size + uv_size * 2;
    
    if (*output_len < required_size) {
        return TURBO_CODEC_ERR_BUFFER;
    }
    
    /* Copy Y plane */
    for (int i = 0; i < height; i++) {
        memcpy(output + i * width, dst[0] + i * stride[0], width);
    }
    
    /* Copy U plane */
    for (int i = 0; i < height / 2; i++) {
        memcpy(output + y_size + i * (width / 2), dst[1] + i * stride[1], width / 2);
    }
    
    /* Copy V plane */
    for (int i = 0; i < height / 2; i++) {
        memcpy(output + y_size + uv_size + i * (width / 2), dst[2] + i * stride[1], width / 2);
    }
    
    *output_len = required_size;
    
    return TURBO_CODEC_OK;
}

static void h264_request_keyframe(void *ctx) {
    h264_context_t *h264 = (h264_context_t *)ctx;
    if (!h264 || !h264->encoder) return;
    
    /* Force IDR frame */
    (*h264->encoder)->ForceIntraFrame(h264->encoder, true);
}

static void h264_set_bitrate(void *ctx, int bitrate_bps) {
    h264_context_t *h264 = (h264_context_t *)ctx;
    if (!h264 || !h264->encoder) return;
    
    SBitrateInfo bitrate_info;
    bitrate_info.iLayer = SPATIAL_LAYER_ALL;
    bitrate_info.iBitrate = bitrate_bps;
    
    (*h264->encoder)->SetOption(h264->encoder, ENCODER_OPTION_BITRATE, &bitrate_info);
    h264->bitrate = bitrate_bps;
}

/* =============================================================================
 * H.264 RTP Packetization (RFC 6184)
 * ============================================================================= */

static int h264_packetize(void *ctx,
                          const turbo_encoded_frame_t *frame,
                          turbo_rtp_fragment_t *fragments, int max_fragments,
                          size_t mtu) {
    h264_context_t *h264 = (h264_context_t *)ctx;

    if (!h264 || !frame || !frame->data || !fragments || max_fragments == 0) {
        return TURBO_CODEC_ERR_INVALID;
    }
    if (mtu == 0) mtu = 1200;
    if (mtu <= 2) {
        return TURBO_CODEC_ERR_BUFFER;
    }
    
    /* Simple implementation: Single NAL unit mode or FU-A fragmentation */
    const uint8_t *data = frame->data;
    size_t len = frame->len;
    int frag_count = 0;
    
    size_t pos = 0;
    while (pos < len && frag_count < max_fragments) {
        size_t nal_start;
        size_t nal_end;
        size_t nal_len;
        uint8_t nal_type;

        if (!h264_find_nal(data, len, pos, &nal_start, &nal_end)) {
            break;
        }
        nal_len = nal_end - nal_start;
        
        if (nal_len == 0) {
            pos = nal_end;
            continue;
        }
        nal_type = (uint8_t)(data[nal_start] & 0x1F);

        if ((nal_type == 7 || nal_type == 8) && frag_count < max_fragments) {
            size_t scan_pos = nal_end;
            size_t stap_payload_len = 1 + 2 + nal_len;
            size_t stap_end = nal_end;
            int stap_count = 1;

            while (scan_pos < len) {
                size_t next_start;
                size_t next_end;
                size_t next_len;
                uint8_t ps_type;

                if (!h264_find_nal(data, len, scan_pos, &next_start, &next_end)) {
                    break;
                }
                next_len = next_end - next_start;
                if (next_len == 0) {
                    scan_pos = next_end;
                    continue;
                }

                ps_type = (uint8_t)(data[next_start] & 0x1F);
                if (ps_type != 7 && ps_type != 8) {
                    break;
                }
                if (stap_payload_len + 2 + next_len > mtu) {
                    break;
                }

                stap_payload_len += 2 + next_len;
                stap_end = next_end;
                stap_count++;
                scan_pos = next_end;
            }

            if (stap_count > 1) {
                size_t write_pos = 0;
                int rc = h264_ensure_packet_buf(h264, frag_count, stap_payload_len);
                size_t write_scan = pos;

                if (rc != TURBO_CODEC_OK) {
                    return rc;
                }

                h264->packet_bufs[frag_count][write_pos++] = 24; /* STAP-A */

                while (write_scan < stap_end) {
                    size_t ps_start;
                    size_t ps_end;
                    size_t ps_len;

                    if (!h264_find_nal(data, len, write_scan, &ps_start, &ps_end)) {
                        break;
                    }
                    ps_len = ps_end - ps_start;
                    if (ps_len == 0) {
                        write_scan = ps_end;
                        continue;
                    }

                    h264->packet_bufs[frag_count][write_pos++] = (uint8_t)(ps_len >> 8);
                    h264->packet_bufs[frag_count][write_pos++] = (uint8_t)(ps_len & 0xFF);
                    memcpy(h264->packet_bufs[frag_count] + write_pos, data + ps_start, ps_len);
                    write_pos += ps_len;
                    write_scan = ps_end;
                }

                fragments[frag_count].data = h264->packet_bufs[frag_count];
                fragments[frag_count].len = write_pos;
                fragments[frag_count].marker = 0;
                fragments[frag_count].fragment_start = 1;
                fragments[frag_count].fragment_end = 0;
                frag_count++;
                pos = stap_end;
                continue;
            }
        }

        /* Single NAL unit or FU-A fragment. The mtu is the RTP payload budget. */
        if (nal_len <= mtu) {
            fragments[frag_count].data = data + nal_start;
            fragments[frag_count].len = nal_len;
            fragments[frag_count].marker = (nal_end == len) ? 1 : 0;
            fragments[frag_count].fragment_start = 1;
            fragments[frag_count].fragment_end = 1;
            frag_count++;
        } else {
            const uint8_t *nal = data + nal_start;
            const uint8_t *payload = nal + 1;
            size_t remaining_payload;
            uint8_t nal_header;
            uint8_t fu_nal_type;

            if (nal_len <= 1) {
                return TURBO_CODEC_ERR_INVALID;
            }

            remaining_payload = nal_len - 1;
            nal_header = nal[0];
            fu_nal_type = (uint8_t)(nal_header & 0x1F);

            while (remaining_payload > 0) {
                size_t chunk = mtu - 2;
                size_t packet_len;
                uint8_t fu_header = fu_nal_type;
                int rc;

                if (frag_count >= max_fragments) {
                    return TURBO_CODEC_ERR_BUFFER;
                }
                if (chunk > remaining_payload) {
                    chunk = remaining_payload;
                }

                if (payload == nal + 1) {
                    fu_header |= 0x80; /* Start */
                }
                if (remaining_payload <= chunk) {
                    fu_header |= 0x40; /* End */
                }

                packet_len = chunk + 2;
                rc = h264_ensure_packet_buf(h264, frag_count, packet_len);
                if (rc != TURBO_CODEC_OK) {
                    return rc;
                }

                h264->packet_bufs[frag_count][0] = (uint8_t)((nal_header & 0xE0) | 28);
                h264->packet_bufs[frag_count][1] = fu_header;
                memcpy(h264->packet_bufs[frag_count] + 2, payload, chunk);

                fragments[frag_count].data = h264->packet_bufs[frag_count];
                fragments[frag_count].len = packet_len;
                fragments[frag_count].marker = ((remaining_payload <= chunk) && (nal_end == len)) ? 1 : 0;
                fragments[frag_count].fragment_start = (payload == nal + 1) ? 1 : 0;
                fragments[frag_count].fragment_end = (remaining_payload <= chunk) ? 1 : 0;

                payload += chunk;
                remaining_payload -= chunk;
                frag_count++;
            }
        }
        
        pos = nal_end;
    }
    
    return (pos >= len) ? frag_count : TURBO_CODEC_ERR_BUFFER;
}

static int h264_depacketize(void *ctx,
                            const uint8_t *rtp_payload, size_t payload_len,
                            uint8_t *output, size_t *output_len,
                            int *complete) {
    h264_context_t *h264 = (h264_context_t *)ctx;
    uint8_t nal_type;

    if (!h264 || !rtp_payload || payload_len < 1 || !output || !output_len) {
        return TURBO_CODEC_ERR_INVALID;
    }

    nal_type = (uint8_t)(rtp_payload[0] & 0x1F);

    if (nal_type == 28) {
        const uint8_t *fu_payload;
        size_t fu_payload_len;
        uint8_t fu_indicator;
        uint8_t fu_header;
        int start;
        int end;

        if (payload_len < 2) {
            return TURBO_CODEC_ERR_INVALID;
        }

        fu_indicator = rtp_payload[0];
        fu_header = rtp_payload[1];
        start = (fu_header & 0x80) != 0;
        end = (fu_header & 0x40) != 0;
        fu_payload = rtp_payload + 2;
        fu_payload_len = payload_len - 2;

        if (start) {
            size_t prefix_len = (h264->reassembly_started && h264->reassembly_len > 0)
                                    ? h264->reassembly_len
                                    : 0;
            if (h264->reassembly_cap < prefix_len + fu_payload_len + 5) {
                return TURBO_CODEC_ERR_BUFFER;
            }
            h264->reassembly_len = prefix_len;
            h264->reassembly_buf[h264->reassembly_len++] = 0x00;
            h264->reassembly_buf[h264->reassembly_len++] = 0x00;
            h264->reassembly_buf[h264->reassembly_len++] = 0x00;
            h264->reassembly_buf[h264->reassembly_len++] = 0x01;
            h264->reassembly_buf[h264->reassembly_len++] =
                (uint8_t)((fu_indicator & 0xE0) | (fu_header & 0x1F));
            h264->reassembly_started = 1;
        } else if (!h264->reassembly_started) {
            return TURBO_CODEC_ERR_INVALID;
        }

        if (h264->reassembly_len + fu_payload_len > h264->reassembly_cap) {
            return TURBO_CODEC_ERR_BUFFER;
        }

        memcpy(h264->reassembly_buf + h264->reassembly_len, fu_payload, fu_payload_len);
        h264->reassembly_len += fu_payload_len;

        if (*output_len < h264->reassembly_len) {
            return TURBO_CODEC_ERR_BUFFER;
        }

        memcpy(output, h264->reassembly_buf, h264->reassembly_len);
        *output_len = h264->reassembly_len;
        if (complete) {
            *complete = end ? 1 : 0;
        }
        if (end) {
            h264->reassembly_started = 0;
        }
        return TURBO_CODEC_OK;
    }

    if (nal_type == 24) {
        size_t pos = 1;
        size_t total_len = 0;
        int has_vcl = 0;

        while (pos + 2 <= payload_len) {
            size_t nal_len = ((size_t)rtp_payload[pos] << 8) | rtp_payload[pos + 1];
            uint8_t stap_nal_type;
            pos += 2;

            if (nal_len == 0 || pos + nal_len > payload_len) {
                return TURBO_CODEC_ERR_INVALID;
            }
            stap_nal_type = (uint8_t)(rtp_payload[pos] & 0x1F);
            if (stap_nal_type == 1 || stap_nal_type == 5) {
                has_vcl = 1;
            }

            if (total_len + nal_len + 4 > *output_len) {
                return TURBO_CODEC_ERR_BUFFER;
            }

            output[total_len++] = 0x00;
            output[total_len++] = 0x00;
            output[total_len++] = 0x00;
            output[total_len++] = 0x01;
            memcpy(output + total_len, rtp_payload + pos, nal_len);
            total_len += nal_len;
            pos += nal_len;
        }

        *output_len = total_len;
        if (complete) {
            *complete = has_vcl ? 1 : 0;
        }
        if (!has_vcl) {
            if (total_len > h264->reassembly_cap) {
                return TURBO_CODEC_ERR_BUFFER;
            }
            memcpy(h264->reassembly_buf, output, total_len);
            h264->reassembly_len = total_len;
            h264->reassembly_started = 1;
        }
        return TURBO_CODEC_OK;
    }

    if (nal_type >= 24) {
        return TURBO_CODEC_ERR_INVALID;
    }

    if (*output_len < payload_len + 4 + h264->reassembly_len) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    if (h264->reassembly_started && h264->reassembly_len > 0) {
        memcpy(output, h264->reassembly_buf, h264->reassembly_len);
    }
    output[h264->reassembly_len + 0] = 0x00;
    output[h264->reassembly_len + 1] = 0x00;
    output[h264->reassembly_len + 2] = 0x00;
    output[h264->reassembly_len + 3] = 0x01;
    memcpy(output + h264->reassembly_len + 4, rtp_payload, payload_len);
    *output_len = h264->reassembly_len + payload_len + 4;
    if (complete) {
        *complete = 1;
    }
    h264->reassembly_started = 0;
    h264->reassembly_len = 0;
    return TURBO_CODEC_OK;
}

/* =============================================================================
 * Codec Operations Table
 * ============================================================================= */

const turbo_codec_ops_t turbo_h264_codec_ops = {
    .name = "h264",
    .type = TURBO_CODEC_TYPE_VIDEO,
    .payload_type = 102,  /* Dynamic PT */
    .clock_rate = 90000,
    
    .create_encoder = h264_create_encoder,
    .create_decoder = h264_create_decoder,
    .destroy = h264_destroy,
    .encode = h264_encode,
    .decode = h264_decode,
    
    .packetize = h264_packetize,
    .depacketize = h264_depacketize,
    .request_keyframe = h264_request_keyframe,
    .set_bitrate = h264_set_bitrate,
    .plc = NULL
};

#endif /* TURBO_MEDIA_HAS_H264 */
