/**
 * H.265/HEVC Video Codec Implementation
 * 
 * Uses x265 for encoding and libde265 for decoding
 * Next-generation video codec with 50% better compression than H.264
 */
#include "turbo_codec.h"
#include <stdlib.h>
#include <string.h>

#ifdef TURBO_MEDIA_HAS_H265

#include <x265.h>
#include <libde265/de265.h>

/* =============================================================================
 * H.265 Context
 * ============================================================================= */

typedef struct {
    x265_encoder *encoder;
    x265_param *param;
    x265_picture *pic_in;
    x265_picture *pic_out;
    
    /* Decoder */
    de265_decoder_context *decoder;
    
    /* Configuration */
    int width;
    int height;
    int framerate;
    int bitrate;
    int keyframe_interval;
    
    /* State */
    int frame_count;
    int64_t pts;
    
    uint8_t *packet_bufs[TURBO_CODEC_MAX_PACKETS];
    size_t packet_caps[TURBO_CODEC_MAX_PACKETS];
    uint8_t *reassembly_buf;
    size_t reassembly_len;
    size_t reassembly_cap;
    int reassembly_started;
} h265_context_t;

static void copy_plane_rows(uint8_t *dst, const uint8_t *src, int width, int height, int stride) {
    int y;

    for (y = 0; y < height; y++) {
        memcpy(dst + (size_t)y * (size_t)width, src + (size_t)y * (size_t)stride, (size_t)width);
    }
}

static size_t h265_find_start_code(const uint8_t *data, size_t len, size_t offset, int *prefix_len) {
    size_t i;

    if (prefix_len) {
        *prefix_len = 0;
    }

    if (!data || offset >= len) {
        return len;
    }

    for (i = offset; i + 3 < len; i++) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                if (prefix_len) {
                    *prefix_len = 3;
                }
                return i;
            }
            if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1) {
                if (prefix_len) {
                    *prefix_len = 4;
                }
                return i;
            }
        }
    }

    return len;
}

static int h265_ensure_packet_buf(h265_context_t *ctx, int index, size_t needed) {
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

/* =============================================================================
 * Encoder Operations
 * ============================================================================= */

static void *h265_create_encoder(const void *config) {
    const turbo_video_codec_config_t *cfg = (const turbo_video_codec_config_t *)config;
    enum { H265_MAX_FRAMERATE = 240 };
    if (!cfg || cfg->width <= 0 || cfg->width > TURBO_VIDEO_MAX_WIDTH ||
        cfg->height <= 0 || cfg->height > TURBO_VIDEO_MAX_HEIGHT ||
        cfg->framerate <= 0 || cfg->framerate > H265_MAX_FRAMERATE ||
        cfg->bitrate < 0 || cfg->keyframe_interval < 0 || cfg->threads < 0) {
        return NULL;
    }
    
    h265_context_t *ctx = (h265_context_t *)calloc(1, sizeof(h265_context_t));
    if (!ctx) return NULL;
    
    ctx->width = cfg->width;
    ctx->height = cfg->height;
    ctx->framerate = cfg->framerate > 0 ? cfg->framerate : 30;
    ctx->bitrate = cfg->bitrate > 0 ? cfg->bitrate : 1000000;
    ctx->keyframe_interval = cfg->keyframe_interval > 0 ? cfg->keyframe_interval : 60;
    
    /* Allocate parameter structure */
    ctx->param = x265_param_alloc();
    if (!ctx->param) {
        free(ctx);
        return NULL;
    }
    
    /* Set preset and tune */
    if (x265_param_default_preset(ctx->param, "medium", "zerolatency") < 0) {
        x265_param_free(ctx->param);
        free(ctx);
        return NULL;
    }
    
    /* Configure parameters */
    ctx->param->sourceWidth = ctx->width;
    ctx->param->sourceHeight = ctx->height;
    ctx->param->fpsNum = ctx->framerate;
    ctx->param->fpsDenom = 1;
    ctx->param->bAnnexB = 1;
    ctx->param->bRepeatHeaders = 1;
    
    /* Bitrate control */
    ctx->param->rc.rateControlMode = X265_RC_ABR;
    ctx->param->rc.bitrate = ctx->bitrate / 1000; /* kbps */
    ctx->param->rc.vbvMaxBitrate = ctx->bitrate * 2 / 1000;
    ctx->param->rc.vbvBufferSize = ctx->bitrate / 1000;
    
    /* Keyframe interval */
    ctx->param->keyframeMax = ctx->keyframe_interval;
    ctx->param->keyframeMin = 1;
    
    /* Low latency settings for real-time */
    ctx->param->bframes = 0;  /* No B-frames for low latency */
    ctx->param->bFrameAdaptive = 0;
    ctx->param->scenecutThreshold = 0;
    ctx->param->lookaheadDepth = 0;
    ctx->param->rc.cuTree = 0;
    
    /* Quality settings */
    ctx->param->bEnablePsnr = 0;
    ctx->param->bEnableSsim = 0;
    
    /* Threading */
    int threads = cfg->threads > 0 ? cfg->threads : 4;
    ctx->param->frameNumThreads = threads;
    /* Note: poolNumThreads removed in newer x265 versions */
    
    /* Profile and level */
    ctx->param->internalCsp = X265_CSP_I420;
    
    /* Apply profile */
    if (x265_param_apply_profile(ctx->param, "main") < 0) {
        x265_param_free(ctx->param);
        free(ctx);
        return NULL;
    }
    
    /* Create encoder */
    ctx->encoder = x265_encoder_open(ctx->param);
    if (!ctx->encoder) {
        x265_param_free(ctx->param);
        free(ctx);
        return NULL;
    }
    
    /* Allocate input picture */
    ctx->pic_in = x265_picture_alloc();
    if (!ctx->pic_in) {
        x265_encoder_close(ctx->encoder);
        x265_param_free(ctx->param);
        free(ctx);
        return NULL;
    }
    
    x265_picture_init(ctx->param, ctx->pic_in);
    
    /* Allocate output picture */
    ctx->pic_out = x265_picture_alloc();
    if (!ctx->pic_out) {
        x265_picture_free(ctx->pic_in);
        x265_encoder_close(ctx->encoder);
        x265_param_free(ctx->param);
        free(ctx);
        return NULL;
    }
    
    ctx->pts = 0;
    ctx->frame_count = 0;
    
    return ctx;
}

static void *h265_create_decoder(const void *config) {
    const turbo_video_codec_config_t *cfg = (const turbo_video_codec_config_t *)config;
    h265_context_t *ctx = (h265_context_t *)calloc(1, sizeof(h265_context_t));
    de265_error err;

    if (!ctx) return NULL;

    if (cfg) {
        ctx->width = cfg->width;
        ctx->height = cfg->height;
    }

    ctx->decoder = de265_new_decoder();
    if (!ctx->decoder) {
        free(ctx);
        return NULL;
    }

    de265_set_parameter_int(ctx->decoder, DE265_DECODER_PARAM_ACCELERATION_CODE,
                            de265_acceleration_AUTO);
    de265_set_parameter_bool(ctx->decoder, DE265_DECODER_PARAM_SUPPRESS_FAULTY_PICTURES, 1);

    if (cfg && cfg->threads > 0) {
        err = de265_start_worker_threads(ctx->decoder, cfg->threads);
        if (!de265_isOK(err)) {
            de265_free_decoder(ctx->decoder);
            free(ctx);
            return NULL;
        }
    }

    ctx->reassembly_cap = TURBO_CODEC_MAX_FRAME_SIZE;
    ctx->reassembly_buf = (uint8_t *)malloc(ctx->reassembly_cap);
    if (!ctx->reassembly_buf) {
        de265_free_decoder(ctx->decoder);
        free(ctx);
        return NULL;
    }
    
    return ctx;
}

static void h265_destroy(void *ctx) {
    h265_context_t *h265 = (h265_context_t *)ctx;
    int i;
    if (!h265) return;
    
    if (h265->encoder) {
        x265_encoder_close(h265->encoder);
    }
    
    if (h265->pic_in) {
        x265_picture_free(h265->pic_in);
    }
    
    if (h265->pic_out) {
        x265_picture_free(h265->pic_out);
    }
    
    if (h265->param) {
        x265_param_free(h265->param);
    }
    if (h265->decoder) {
        de265_free_decoder(h265->decoder);
    }
    
    for (i = 0; i < TURBO_CODEC_MAX_PACKETS; i++) {
        free(h265->packet_bufs[i]);
    }
    free(h265->reassembly_buf);
    
    free(h265);
}

static int h265_encode(void *ctx,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len,
                       turbo_encoded_frame_t *info) {
    h265_context_t *h265 = (h265_context_t *)ctx;
    if (!h265 || !h265->encoder || !input || !output || !output_len) {
        return TURBO_CODEC_ERR_INVALID;
    }
    
    /* Setup input picture (I420 format) */
    size_t y_size = h265->width * h265->height;
    size_t uv_size = y_size / 4;
    
    if (input_len < y_size + uv_size * 2) {
        return TURBO_CODEC_ERR_INVALID;
    }
    
    /* Copy input data to picture */
    h265->pic_in->planes[0] = (void *)input;
    h265->pic_in->planes[1] = (void *)(input + y_size);
    h265->pic_in->planes[2] = (void *)(input + y_size + uv_size);
    
    h265->pic_in->stride[0] = h265->width;
    h265->pic_in->stride[1] = h265->width / 2;
    h265->pic_in->stride[2] = h265->width / 2;
    
    h265->pic_in->pts = h265->pts++;
    h265->pic_in->bitDepth = 8;
    h265->pic_in->colorSpace = X265_CSP_I420;
    
    /* Encode frame */
    x265_nal *nals = NULL;
    uint32_t nal_count = 0;
    
    int ret = x265_encoder_encode(h265->encoder, &nals, &nal_count, 
                                   h265->pic_in, h265->pic_out);
    
    if (ret < 0) {
        return TURBO_CODEC_ERR_CODEC;
    }
    
    /* Copy encoded data */
    size_t total_len = 0;
    int is_keyframe = 0;
    
    if (nal_count > 0) {
        for (uint32_t i = 0; i < nal_count; i++) {
            x265_nal *nal = &nals[i];
            const uint8_t *nal_data = nal->payload;
            size_t nal_size = nal->sizeBytes;
            int prefix_len = 0;
            
            if (total_len + nal->sizeBytes > *output_len) {
                return TURBO_CODEC_ERR_BUFFER;
            }
            
            memcpy(output + total_len, nal->payload, nal->sizeBytes);
            total_len += nal->sizeBytes;
            
            if (h265_find_start_code(nal_data, nal_size, 0, &prefix_len) == 0 &&
                prefix_len > 0) {
                nal_data += (size_t)prefix_len;
                nal_size -= (size_t)prefix_len;
            }
            if (nal_size < 2) continue;

            /* HEVC IRAP pictures (BLA/IDR/CRA) are valid random-access points. */
            int nal_type = (nal_data[0] >> 1) & 0x3F;
            if (nal_type >= 16 && nal_type <= 23) {
                is_keyframe = 1;
            }
        }
    }
    
    *output_len = total_len;
    
    if (info) {
        info->data = output;
        info->len = total_len;
        info->is_keyframe = is_keyframe;
        info->packet_count = (int)((total_len + 1199) / 1200); /* Estimate RTP packets */
    }
    
    h265->frame_count++;
    
    return TURBO_CODEC_OK;
}

static int h265_decode(void *ctx,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len) {
    h265_context_t *h265 = (h265_context_t *)ctx;
    de265_error err;
    const struct de265_image *img;
    int more = 0;
    int y_width, y_height, uv_width, uv_height;
    int y_stride = 0, u_stride = 0, v_stride = 0;
    const uint8_t *y_plane;
    const uint8_t *u_plane;
    const uint8_t *v_plane;
    size_t required;
    uint8_t *dst;

    if (!h265 || !h265->decoder || !input || !output || !output_len) {
        return TURBO_CODEC_ERR_INVALID;
    }

    if (input_len > INT32_MAX) {
        return TURBO_CODEC_ERR_INVALID;
    }

    {
        size_t pos = h265_find_start_code(input, input_len, 0, NULL);

        if (pos >= input_len) {
            err = de265_push_NAL(h265->decoder, input, (int)input_len, 0, NULL);
            if (!de265_isOK(err)) {
                return TURBO_CODEC_ERR_CODEC;
            }
        } else {
            while (pos < input_len) {
                int prefix_len = 0;
                size_t nal_start = h265_find_start_code(input, input_len, pos, &prefix_len);
                size_t next = h265_find_start_code(input, input_len, nal_start + (size_t)prefix_len, NULL);

                if (nal_start >= input_len) {
                    break;
                }

                nal_start += (size_t)prefix_len;
                if (nal_start >= input_len) {
                    break;
                }

                if (next <= nal_start) {
                    next = input_len;
                }

                err = de265_push_NAL(h265->decoder, input + nal_start, (int)(next - nal_start), 0, NULL);
                if (!de265_isOK(err)) {
                    return TURBO_CODEC_ERR_CODEC;
                }

                pos = next;
            }
        }
    }

    de265_push_end_of_frame(h265->decoder);

    img = NULL;
    do {
        err = de265_decode(h265->decoder, &more);
        if (!de265_isOK(err) &&
            err != DE265_ERROR_WAITING_FOR_INPUT_DATA &&
            err != DE265_ERROR_IMAGE_BUFFER_FULL) {
            return TURBO_CODEC_ERR_CODEC;
        }

        img = de265_get_next_picture(h265->decoder);
        if (img) {
            break;
        }
    } while (more);

    if (!img) {
        return TURBO_CODEC_ERR_NEED_MORE;
    }

    if (de265_get_bits_per_pixel(img, 0) != 8 ||
        de265_get_chroma_format(img) != de265_chroma_420) {
        de265_release_next_picture(h265->decoder);
        return TURBO_CODEC_ERR_CODEC;
    }

    y_width = de265_get_image_width(img, 0);
    y_height = de265_get_image_height(img, 0);
    uv_width = de265_get_image_width(img, 1);
    uv_height = de265_get_image_height(img, 1);
    required = (size_t)y_width * (size_t)y_height +
               2U * (size_t)uv_width * (size_t)uv_height;

    if (*output_len < required) {
        *output_len = required;
        de265_release_next_picture(h265->decoder);
        return TURBO_CODEC_ERR_BUFFER;
    }

    y_plane = de265_get_image_plane(img, 0, &y_stride);
    u_plane = de265_get_image_plane(img, 1, &u_stride);
    v_plane = de265_get_image_plane(img, 2, &v_stride);
    if (!y_plane || !u_plane || !v_plane) {
        de265_release_next_picture(h265->decoder);
        return TURBO_CODEC_ERR_CODEC;
    }

    dst = output;
    copy_plane_rows(dst, y_plane, y_width, y_height, y_stride);
    dst += (size_t)y_width * (size_t)y_height;
    copy_plane_rows(dst, u_plane, uv_width, uv_height, u_stride);
    dst += (size_t)uv_width * (size_t)uv_height;
    copy_plane_rows(dst, v_plane, uv_width, uv_height, v_stride);

    *output_len = required;
    h265->width = y_width;
    h265->height = y_height;
    de265_release_next_picture(h265->decoder);
    return TURBO_CODEC_OK;
}

static void h265_request_keyframe(void *ctx) {
    h265_context_t *h265 = (h265_context_t *)ctx;
    if (!h265 || !h265->encoder) return;
    
    /* Force IDR frame on next encode */
    h265->pic_in->sliceType = X265_TYPE_IDR;
}

static void h265_set_bitrate(void *ctx, int bitrate_bps) {
    h265_context_t *h265 = (h265_context_t *)ctx;
    if (!h265 || !h265->encoder) return;
    
    /* Update bitrate dynamically */
    x265_param *param = x265_param_alloc();
    if (!param) return;
    
    /* x265_encoder_parameters returns void in newer versions */
    x265_encoder_parameters(h265->encoder, param);
    
    param->rc.bitrate = bitrate_bps / 1000; /* Convert to kbps */
    param->rc.vbvMaxBitrate = bitrate_bps * 2 / 1000;
    
    if (x265_encoder_reconfig(h265->encoder, param) == 0) {
        h265->bitrate = bitrate_bps;
    }
    
    x265_param_free(param);
}

/* =============================================================================
 * H.265 RTP Packetization (RFC 7798)
 * ============================================================================= */

static int h265_packetize(void *ctx,
                          const turbo_encoded_frame_t *frame,
                          turbo_rtp_fragment_t *fragments, int max_fragments,
                          size_t mtu) {
    h265_context_t *h265 = (h265_context_t *)ctx;

    if (!h265 || !frame || !frame->data || !fragments || max_fragments == 0) {
        return TURBO_CODEC_ERR_INVALID;
    }
    if (mtu == 0) mtu = 1200;
    if (mtu <= 3) {
        return TURBO_CODEC_ERR_BUFFER;
    }
    
    /* H.265 RTP packetization is similar to H.264 but with different NAL unit types */
    const uint8_t *data = frame->data;
    size_t len = frame->len;
    int frag_count = 0;
    
    /* Find NAL units (start with 0x00 0x00 0x00 0x01 or 0x00 0x00 0x01) */
    size_t pos = 0;
    while (pos < len && frag_count < max_fragments) {
        /* Find start code */
        int start_code_len = 0;
        
        if (pos + 3 < len && data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 0 && data[pos+3] == 1) {
            start_code_len = 4;
        } else if (pos + 2 < len && data[pos] == 0 && data[pos+1] == 0 && data[pos+2] == 1) {
            start_code_len = 3;
        }
        
        if (start_code_len == 0) {
            pos++;
            continue;
        }
        
        pos += start_code_len;
        size_t nal_start = pos;
        
        /* Find next start code or end */
        size_t nal_end = len;
        for (size_t i = pos; i < len - 3; i++) {
            if (data[i] == 0 && data[i+1] == 0 && (data[i+2] == 1 || (data[i+2] == 0 && data[i+3] == 1))) {
                nal_end = i;
                break;
            }
        }
        
        size_t nal_len = nal_end - nal_start;
        
        /* Single NAL unit or FU payload */
        if (nal_len <= mtu) {
            fragments[frag_count].data = data + nal_start;
            fragments[frag_count].len = nal_len;
            fragments[frag_count].marker = (nal_end == len) ? 1 : 0;
            fragments[frag_count].fragment_start = 1;
            fragments[frag_count].fragment_end = 1;
            frag_count++;
        } else {
            const uint8_t *nal = data + nal_start;
            const uint8_t *payload = nal + 2;
            size_t remaining_payload;
            uint8_t fu_indicator0;
            uint8_t fu_indicator1;
            uint8_t nal_type;

            if (nal_len <= 2) {
                return TURBO_CODEC_ERR_INVALID;
            }

            remaining_payload = nal_len - 2;
            nal_type = (uint8_t)((nal[0] >> 1) & 0x3F);
            fu_indicator0 = (uint8_t)((nal[0] & 0x81U) | (49U << 1));
            fu_indicator1 = nal[1];

            while (remaining_payload > 0) {
                size_t chunk = mtu - 3;
                size_t packet_len;
                uint8_t fu_header = nal_type;
                int rc;

                if (frag_count >= max_fragments) {
                    return TURBO_CODEC_ERR_BUFFER;
                }
                if (chunk > remaining_payload) {
                    chunk = remaining_payload;
                }

                if (payload == nal + 2) {
                    fu_header |= 0x80; /* Start */
                }
                if (remaining_payload <= chunk) {
                    fu_header |= 0x40; /* End */
                }

                packet_len = chunk + 3;
                rc = h265_ensure_packet_buf(h265, frag_count, packet_len);
                if (rc != TURBO_CODEC_OK) {
                    return rc;
                }

                h265->packet_bufs[frag_count][0] = fu_indicator0;
                h265->packet_bufs[frag_count][1] = fu_indicator1;
                h265->packet_bufs[frag_count][2] = fu_header;
                memcpy(h265->packet_bufs[frag_count] + 3, payload, chunk);

                fragments[frag_count].data = h265->packet_bufs[frag_count];
                fragments[frag_count].len = packet_len;
                fragments[frag_count].marker = ((remaining_payload <= chunk) && (nal_end == len)) ? 1 : 0;
                fragments[frag_count].fragment_start = (payload == nal + 2) ? 1 : 0;
                fragments[frag_count].fragment_end = (remaining_payload <= chunk) ? 1 : 0;

                payload += chunk;
                remaining_payload -= chunk;
                frag_count++;
            }
        }
        
        pos = nal_end;
    }
    
    return frag_count;
}

static int h265_depacketize(void *ctx,
                            const uint8_t *rtp_payload, size_t payload_len,
                            uint8_t *output, size_t *output_len,
                            int *complete) {
    h265_context_t *h265 = (h265_context_t *)ctx;
    uint8_t nal_type;

    if (!h265 || !rtp_payload || payload_len < 2 || !output || !output_len) {
        return TURBO_CODEC_ERR_INVALID;
    }

    nal_type = (uint8_t)((rtp_payload[0] >> 1) & 0x3F);

    if (nal_type == 49) {
        const uint8_t *fu_payload;
        size_t fu_payload_len;
        uint8_t fu_header;
        int start;
        int end;

        if (payload_len < 3) {
            return TURBO_CODEC_ERR_INVALID;
        }

        fu_header = rtp_payload[2];
        start = (fu_header & 0x80) != 0;
        end = (fu_header & 0x40) != 0;
        fu_payload = rtp_payload + 3;
        fu_payload_len = payload_len - 3;

        if (start) {
            if (h265->reassembly_cap < fu_payload_len + 6) {
                return TURBO_CODEC_ERR_BUFFER;
            }
            h265->reassembly_buf[0] = 0x00;
            h265->reassembly_buf[1] = 0x00;
            h265->reassembly_buf[2] = 0x00;
            h265->reassembly_buf[3] = 0x01;
            h265->reassembly_buf[4] = (uint8_t)((rtp_payload[0] & 0x81U) | ((fu_header & 0x3FU) << 1));
            h265->reassembly_buf[5] = rtp_payload[1];
            h265->reassembly_len = 6;
            h265->reassembly_started = 1;
        } else if (!h265->reassembly_started) {
            return TURBO_CODEC_ERR_INVALID;
        }

        if (h265->reassembly_len + fu_payload_len > h265->reassembly_cap) {
            return TURBO_CODEC_ERR_BUFFER;
        }

        memcpy(h265->reassembly_buf + h265->reassembly_len, fu_payload, fu_payload_len);
        h265->reassembly_len += fu_payload_len;

        if (*output_len < h265->reassembly_len) {
            return TURBO_CODEC_ERR_BUFFER;
        }

        memcpy(output, h265->reassembly_buf, h265->reassembly_len);
        *output_len = h265->reassembly_len;
        if (complete) {
            *complete = end ? 1 : 0;
        }
        if (end) {
            h265->reassembly_started = 0;
        }
        return TURBO_CODEC_OK;
    }

    if (*output_len < payload_len + 4) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    output[0] = 0x00;
    output[1] = 0x00;
    output[2] = 0x00;
    output[3] = 0x01;
    memcpy(output + 4, rtp_payload, payload_len);
    *output_len = payload_len + 4;
    if (complete) {
        *complete = 1;
    }
    return TURBO_CODEC_OK;
}

/* =============================================================================
 * Codec Operations Table
 * ============================================================================= */

const turbo_codec_ops_t turbo_h265_codec_ops = {
    .name = "h265",
    .type = TURBO_CODEC_TYPE_VIDEO,
    .payload_type = 103,  /* Dynamic PT */
    .clock_rate = 90000,
    
    .create_encoder = h265_create_encoder,
    .create_decoder = h265_create_decoder,
    .destroy = h265_destroy,
    .encode = h265_encode,
    .decode = h265_decode,
    
    .packetize = h265_packetize,
    .depacketize = h265_depacketize,
    .request_keyframe = h265_request_keyframe,
    .set_bitrate = h265_set_bitrate,
    .plc = NULL
};

#endif /* TURBO_MEDIA_HAS_H265 */
