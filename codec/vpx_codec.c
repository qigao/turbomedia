/**
 * VP8/VP9 Codec Implementation
 *
 * Wraps libvpx for video encoding/decoding
 */
#include "turbo_codec.h"

#ifdef TURBO_MEDIA_HAS_VPX

#include <vpx/vpx_encoder.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#include <stdlib.h>
#include <string.h>

/* =============================================================================
 * Constants
 * ============================================================================= */

#define VP8_PAYLOAD_TYPE        96
#define VP9_PAYLOAD_TYPE        98
#define VIDEO_CLOCK_RATE        90000
#define VPX_MAX_FRAME_SIZE      (1024 * 1024)   /* 1MB max frame */
#define VPX_MTU                 1200            /* RTP MTU for fragmentation */

/* VP8 payload descriptor sizes */
#define VP8_DESC_BASIC          1

/* =============================================================================
 * Context Structure
 * ============================================================================= */

typedef struct {
    vpx_codec_ctx_t codec;
    int is_encoder_ctx;
    vpx_codec_enc_cfg_t cfg;
    int width;
    int height;
    int framerate;
    int bitrate;
    int keyframe_interval;
    int frames_encoded;
    int keyframe_requested;
    int is_vp9;             /* 0 = VP8, 1 = VP9 */
    uint8_t *packet_bufs[TURBO_CODEC_MAX_PACKETS];
    size_t packet_caps[TURBO_CODEC_MAX_PACKETS];
} vpx_encoder_ctx_t;

typedef struct {
    vpx_codec_ctx_t codec;
    int is_encoder_ctx;
    int width;
    int height;
    int is_vp9;

    /* Reassembly buffer for fragmented frames */
    uint8_t *reassembly_buf;
    size_t reassembly_len;
    size_t reassembly_cap;
    int reassembly_started;
} vpx_decoder_ctx_t;

static int vpx_ensure_packet_buf(vpx_encoder_ctx_t *ctx, int index, size_t needed) {
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
 * Encoder Functions
 * ============================================================================= */

static void *vpx_create_encoder_internal(const void *config, int is_vp9) {
    const turbo_video_codec_config_t *cfg = (const turbo_video_codec_config_t *)config;
    enum { VPX_MAX_FRAMERATE = 240 };
    if (!cfg || cfg->width <= 0 || cfg->width > TURBO_VIDEO_MAX_WIDTH ||
        cfg->height <= 0 || cfg->height > TURBO_VIDEO_MAX_HEIGHT ||
        cfg->framerate <= 0 || cfg->framerate > VPX_MAX_FRAMERATE ||
        cfg->bitrate < 0 || cfg->keyframe_interval < 0 || cfg->threads < 0) {
        return NULL;
    }

    vpx_encoder_ctx_t *ctx = (vpx_encoder_ctx_t *)calloc(1, sizeof(vpx_encoder_ctx_t));
    if (!ctx) return NULL;

    ctx->is_encoder_ctx = 1;
    ctx->is_vp9 = is_vp9;
    ctx->width = cfg->width;
    ctx->height = cfg->height;
    ctx->framerate = cfg->framerate;
    ctx->bitrate = cfg->bitrate > 0 ? cfg->bitrate : 1000000;
    ctx->keyframe_interval = cfg->keyframe_interval > 0 ? cfg->keyframe_interval : 60;

    /* Select codec interface */
    vpx_codec_iface_t *iface = is_vp9 ? vpx_codec_vp9_cx() : vpx_codec_vp8_cx();

    /* Get default config */
    if (vpx_codec_enc_config_default(iface, &ctx->cfg, 0) != VPX_CODEC_OK) {
        free(ctx);
        return NULL;
    }

    /* Configure encoder */
    ctx->cfg.g_w = ctx->width;
    ctx->cfg.g_h = ctx->height;
    ctx->cfg.g_timebase.num = 1;
    ctx->cfg.g_timebase.den = ctx->framerate;
    ctx->cfg.rc_target_bitrate = ctx->bitrate / 1000;   /* kbps */
    ctx->cfg.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
    ctx->cfg.g_lag_in_frames = 0;       /* Real-time mode */
    ctx->cfg.rc_end_usage = VPX_CBR;    /* Constant bitrate */
    ctx->cfg.kf_max_dist = ctx->keyframe_interval;
    ctx->cfg.kf_mode = VPX_KF_AUTO;

    /* Thread count */
    int threads = cfg->threads;
    if (threads <= 0) {
        threads = 2;    /* Default to 2 threads */
    }
    ctx->cfg.g_threads = threads;

    /* Initialize encoder */
    if (vpx_codec_enc_init(&ctx->codec, iface, &ctx->cfg, 0) != VPX_CODEC_OK) {
        free(ctx);
        return NULL;
    }

    /* Set real-time profile for VP8 */
    if (!is_vp9) {
        vpx_codec_control(&ctx->codec, VP8E_SET_CPUUSED, 8);  /* Fastest */
        vpx_codec_control(&ctx->codec, VP8E_SET_SCREEN_CONTENT_MODE, 0);
    } else {
        vpx_codec_control(&ctx->codec, VP9E_SET_TILE_COLUMNS, 2);
        vpx_codec_control(&ctx->codec, VP8E_SET_CPUUSED, 8);
    }

    return ctx;
}

static void *vp8_create_encoder(const void *config) {
    return vpx_create_encoder_internal(config, 0);
}

static void *vp9_create_encoder(const void *config) {
    return vpx_create_encoder_internal(config, 1);
}

static int vpx_encode(void *ctx_ptr,
                       const uint8_t *input, size_t input_len,
                       uint8_t *output, size_t *output_len,
                       turbo_encoded_frame_t *info) {
    vpx_encoder_ctx_t *ctx = (vpx_encoder_ctx_t *)ctx_ptr;
    if (!ctx) return TURBO_CODEC_ERR_INVALID;

    /* Input is I420/YUV420 format */
    size_t expected_len = ctx->width * ctx->height * 3 / 2;
    if (input_len < expected_len) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    /* Setup image wrapper */
    vpx_image_t img;
    if (!vpx_img_wrap(&img, VPX_IMG_FMT_I420,
                      ctx->width, ctx->height, 1, (uint8_t *)input)) {
        return TURBO_CODEC_ERR_CODEC;
    }

    /* Determine if keyframe should be forced */
    vpx_enc_frame_flags_t flags = 0;
    if (ctx->keyframe_requested || ctx->frames_encoded == 0) {
        flags |= VPX_EFLAG_FORCE_KF;
        ctx->keyframe_requested = 0;
    }

    /* Encode frame */
    vpx_codec_pts_t pts = ctx->frames_encoded;
    if (vpx_codec_encode(&ctx->codec, &img, pts, 1, flags, VPX_DL_REALTIME) != VPX_CODEC_OK) {
        return TURBO_CODEC_ERR_CODEC;
    }

    /* Get encoded data */
    vpx_codec_iter_t iter = NULL;
    const vpx_codec_cx_pkt_t *pkt;
    size_t total_len = 0;
    int is_keyframe = 0;

    while ((pkt = vpx_codec_get_cx_data(&ctx->codec, &iter)) != NULL) {
        if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
            if (total_len + pkt->data.frame.sz > *output_len) {
                return TURBO_CODEC_ERR_BUFFER;
            }

            memcpy(output + total_len, pkt->data.frame.buf, pkt->data.frame.sz);
            total_len += pkt->data.frame.sz;

            if (pkt->data.frame.flags & VPX_FRAME_IS_KEY) {
                is_keyframe = 1;
            }
        }
    }

    if (total_len == 0) {
        return TURBO_CODEC_ERR_NEED_MORE;
    }

    *output_len = total_len;
    ctx->frames_encoded++;

    if (info) {
        info->data = output;
        info->len = total_len;
        info->is_keyframe = is_keyframe;
        /* Calculate packet count based on MTU */
        info->packet_count = (int)((total_len + VPX_MTU - 1) / VPX_MTU);
        info->timestamp = 0;    /* Caller should set this */
    }

    return TURBO_CODEC_OK;
}

static void vpx_request_keyframe(void *ctx_ptr) {
    vpx_encoder_ctx_t *ctx = (vpx_encoder_ctx_t *)ctx_ptr;
    if (ctx) {
        ctx->keyframe_requested = 1;
    }
}

static void vpx_set_bitrate(void *ctx_ptr, int bitrate_bps) {
    vpx_encoder_ctx_t *ctx = (vpx_encoder_ctx_t *)ctx_ptr;
    if (!ctx || bitrate_bps <= 0) return;

    ctx->bitrate = bitrate_bps;
    ctx->cfg.rc_target_bitrate = bitrate_bps / 1000;
    vpx_codec_enc_config_set(&ctx->codec, &ctx->cfg);
}

/* =============================================================================
 * Decoder Functions
 * ============================================================================= */

static void *vpx_create_decoder_internal(const void *config, int is_vp9) {
    (void)config;   /* Config optional for decoder */

    vpx_decoder_ctx_t *ctx = (vpx_decoder_ctx_t *)calloc(1, sizeof(vpx_decoder_ctx_t));
    if (!ctx) return NULL;

    ctx->is_encoder_ctx = 0;
    ctx->is_vp9 = is_vp9;

    /* Allocate reassembly buffer */
    ctx->reassembly_cap = VPX_MAX_FRAME_SIZE;
    ctx->reassembly_buf = (uint8_t *)malloc(ctx->reassembly_cap);
    if (!ctx->reassembly_buf) {
        free(ctx);
        return NULL;
    }

    /* Select codec interface */
    vpx_codec_iface_t *iface = is_vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx();

    /* Initialize decoder */
    if (vpx_codec_dec_init(&ctx->codec, iface, NULL, 0) != VPX_CODEC_OK) {
        free(ctx->reassembly_buf);
        free(ctx);
        return NULL;
    }

    return ctx;
}

static void *vp8_create_decoder(const void *config) {
    return vpx_create_decoder_internal(config, 0);
}

static void *vp9_create_decoder(const void *config) {
    return vpx_create_decoder_internal(config, 1);
}

static int vpx_decode(void *ctx_ptr,
                        const uint8_t *input, size_t input_len,
                        uint8_t *output, size_t *output_len) {
    vpx_decoder_ctx_t *ctx = (vpx_decoder_ctx_t *)ctx_ptr;
    if (!ctx) return TURBO_CODEC_ERR_INVALID;

    /* Decode frame */
    if (vpx_codec_decode(&ctx->codec, input, (unsigned int)input_len, NULL, 0) != VPX_CODEC_OK) {
        return TURBO_CODEC_ERR_CODEC;
    }

    /* Get decoded image */
    vpx_codec_iter_t iter = NULL;
    vpx_image_t *img = vpx_codec_get_frame(&ctx->codec, &iter);

    if (!img) {
        return TURBO_CODEC_ERR_NEED_MORE;
    }

    /* Update dimensions */
    ctx->width = img->d_w;
    ctx->height = img->d_h;

    /* Calculate output size (I420 format) */
    size_t y_size = img->d_w * img->d_h;
    size_t uv_size = y_size / 4;
    size_t total_size = y_size + 2 * uv_size;

    if (*output_len < total_size) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    /* Copy Y plane */
    uint8_t *dst = output;
    for (unsigned int row = 0; row < img->d_h; row++) {
        memcpy(dst, img->planes[VPX_PLANE_Y] + row * img->stride[VPX_PLANE_Y], img->d_w);
        dst += img->d_w;
    }

    /* Copy U plane */
    for (unsigned int row = 0; row < img->d_h / 2; row++) {
        memcpy(dst, img->planes[VPX_PLANE_U] + row * img->stride[VPX_PLANE_U], img->d_w / 2);
        dst += img->d_w / 2;
    }

    /* Copy V plane */
    for (unsigned int row = 0; row < img->d_h / 2; row++) {
        memcpy(dst, img->planes[VPX_PLANE_V] + row * img->stride[VPX_PLANE_V], img->d_w / 2);
        dst += img->d_w / 2;
    }

    *output_len = total_size;

    return TURBO_CODEC_OK;
}

/* =============================================================================
 * RTP Packetization
 * ============================================================================= */

static int vp8_packetize(void *ctx_ptr,
                          const turbo_encoded_frame_t *frame,
                          turbo_rtp_fragment_t *fragments, int max_fragments,
                          size_t mtu) {
    vpx_encoder_ctx_t *ctx = (vpx_encoder_ctx_t *)ctx_ptr;

    if (!ctx || !frame || !frame->data || !fragments || max_fragments < 1) {
        return TURBO_CODEC_ERR_INVALID;
    }

    if (mtu == 0) mtu = VPX_MTU;
    if (mtu <= VP8_DESC_BASIC) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    /* VP8 RTP payload format (RFC 7741):
     * Byte 0: X R N S PartID
     *   X=1 means extended header follows
     *   R=0 reserved
     *   N=1 non-reference frame
     *   S=1 start of partition
     *   PartID = partition index
     */

    const uint8_t *data = frame->data;
    size_t remaining = frame->len;
    int fragment_count = 0;

    while (remaining > 0 && fragment_count < max_fragments) {
        /* Calculate payload size (leave room for descriptor) */
        size_t payload_size = mtu - VP8_DESC_BASIC;
        size_t packet_len;
        uint8_t descriptor = 0;
        int rc;
        if (payload_size > remaining) {
            payload_size = remaining;
        }

        packet_len = payload_size + VP8_DESC_BASIC;
        rc = vpx_ensure_packet_buf(ctx, fragment_count, packet_len);
        if (rc != TURBO_CODEC_OK) {
            return rc;
        }

        if (fragment_count == 0) {
            descriptor |= 0x10; /* S bit for start of partition 0 */
        }

        ctx->packet_bufs[fragment_count][0] = descriptor;
        memcpy(ctx->packet_bufs[fragment_count] + VP8_DESC_BASIC, data, payload_size);

        turbo_rtp_fragment_t *frag = &fragments[fragment_count];
        frag->data = ctx->packet_bufs[fragment_count];
        frag->len = packet_len;
        frag->fragment_start = (fragment_count == 0) ? 1 : 0;
        frag->fragment_end = (remaining <= payload_size) ? 1 : 0;
        frag->marker = frag->fragment_end;  /* RTP marker on last fragment */

        data += payload_size;
        remaining -= payload_size;
        fragment_count++;
    }

    return (remaining == 0) ? fragment_count : TURBO_CODEC_ERR_BUFFER;
}

static int vp9_packetize(void *ctx_ptr,
                         const turbo_encoded_frame_t *frame,
                         turbo_rtp_fragment_t *fragments, int max_fragments,
                         size_t mtu) {
    vpx_encoder_ctx_t *ctx = (vpx_encoder_ctx_t *)ctx_ptr;
    const uint8_t *data;
    size_t remaining;
    int fragment_count = 0;

    if (!ctx || !frame || !frame->data || !fragments || max_fragments < 1) {
        return TURBO_CODEC_ERR_INVALID;
    }

    if (mtu == 0) mtu = VPX_MTU;
    if (mtu <= 1) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    data = frame->data;
    remaining = frame->len;

    while (remaining > 0 && fragment_count < max_fragments) {
        size_t payload_size = mtu - 1; /* 1 byte VP9 payload descriptor */
        size_t packet_len;
        uint8_t descriptor = 0;
        int rc;

        if (payload_size > remaining) {
            payload_size = remaining;
        }

        packet_len = payload_size + 1;
        rc = vpx_ensure_packet_buf(ctx, fragment_count, packet_len);
        if (rc != TURBO_CODEC_OK) {
            return rc;
        }

        if (fragment_count == 0) {
            descriptor |= 0x08; /* B */
        }
        if (remaining <= payload_size) {
            descriptor |= 0x04; /* E */
        }

        ctx->packet_bufs[fragment_count][0] = descriptor;
        memcpy(ctx->packet_bufs[fragment_count] + 1, data, payload_size);

        fragments[fragment_count].data = ctx->packet_bufs[fragment_count];
        fragments[fragment_count].len = packet_len;
        fragments[fragment_count].fragment_start = (fragment_count == 0) ? 1 : 0;
        fragments[fragment_count].fragment_end = (remaining <= payload_size) ? 1 : 0;
        fragments[fragment_count].marker = fragments[fragment_count].fragment_end;

        data += payload_size;
        remaining -= payload_size;
        fragment_count++;
    }

    return (remaining == 0) ? fragment_count : TURBO_CODEC_ERR_BUFFER;
}

static int vp8_depacketize(void *ctx_ptr,
                             const uint8_t *rtp_payload, size_t payload_len,
                             uint8_t *output, size_t *output_len,
                             int *complete) {
    vpx_decoder_ctx_t *ctx = (vpx_decoder_ctx_t *)ctx_ptr;
    size_t desc_len = VP8_DESC_BASIC;
    uint8_t byte0;
    int has_extension;
    int start_of_partition;
    int partition_id;

    if (!ctx || !rtp_payload || payload_len < VP8_DESC_BASIC || !output || !output_len) {
        return TURBO_CODEC_ERR_INVALID;
    }

    /* Parse VP8 payload descriptor */
    byte0 = rtp_payload[0];
    has_extension = (byte0 >> 7) & 1;
    start_of_partition = (byte0 >> 4) & 1;
    partition_id = byte0 & 0x07;

    if (has_extension) {
        uint8_t ext_bits;
        int has_picture_id;
        int has_tl0picidx;
        int has_tid_or_keyidx;

        if (payload_len < desc_len + 1) {
            return TURBO_CODEC_ERR_INVALID;
        }
        ext_bits = rtp_payload[desc_len++];
        has_picture_id = (ext_bits & 0x80) != 0;
        has_tl0picidx = (ext_bits & 0x40) != 0;
        has_tid_or_keyidx = (ext_bits & 0x30) != 0;

        if (has_picture_id) {
            if (payload_len < desc_len + 1) {
                return TURBO_CODEC_ERR_INVALID;
            }
            if (rtp_payload[desc_len] & 0x80) {
                if (payload_len < desc_len + 2) {
                    return TURBO_CODEC_ERR_INVALID;
                }
                desc_len += 2;
            } else {
                desc_len += 1;
            }
        }
        if (has_tl0picidx) {
            if (payload_len < desc_len + 1) {
                return TURBO_CODEC_ERR_INVALID;
            }
            desc_len += 1;
        }
        if (has_tid_or_keyidx) {
            if (payload_len < desc_len + 1) {
                return TURBO_CODEC_ERR_INVALID;
            }
            desc_len += 1;
        }
    }

    if (payload_len <= desc_len) {
        return TURBO_CODEC_ERR_NEED_MORE;
    }

    const uint8_t *frame_data = rtp_payload + desc_len;
    size_t frame_len = payload_len - desc_len;

    /* Handle reassembly */
    if (start_of_partition && partition_id == 0) {
        /* Start new frame */
        ctx->reassembly_len = 0;
        ctx->reassembly_started = 1;
    }

    if (!ctx->reassembly_started) {
        return TURBO_CODEC_ERR_INVALID;  /* Missing start */
    }

    /* Append to reassembly buffer */
    if (ctx->reassembly_len + frame_len > ctx->reassembly_cap) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    memcpy(ctx->reassembly_buf + ctx->reassembly_len, frame_data, frame_len);
    ctx->reassembly_len += frame_len;

    /* Check if frame is complete (caller should check RTP marker bit) */
    if (complete) {
        *complete = 0;  /* Caller needs to check marker bit */
    }

    /* Copy accumulated data to output */
    if (*output_len < ctx->reassembly_len) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    memcpy(output, ctx->reassembly_buf, ctx->reassembly_len);
    *output_len = ctx->reassembly_len;

    return TURBO_CODEC_OK;
}

static int vp9_depacketize(void *ctx_ptr,
                           const uint8_t *rtp_payload, size_t payload_len,
                           uint8_t *output, size_t *output_len,
                           int *complete) {
    vpx_decoder_ctx_t *ctx = (vpx_decoder_ctx_t *)ctx_ptr;
    uint8_t descriptor;
    int has_picture_id;
    int inter_picture_predicted;
    int has_layer_indices;
    int start;
    int end;
    int flexible;
    int has_scalability_structure;
    const uint8_t *frame_data;
    size_t frame_len;
    size_t desc_len = 1;

    if (!ctx || !rtp_payload || payload_len < 1 || !output || !output_len) {
        return TURBO_CODEC_ERR_INVALID;
    }

    descriptor = rtp_payload[0];
    has_picture_id = (descriptor & 0x80) != 0;
    inter_picture_predicted = (descriptor & 0x40) != 0;
    has_layer_indices = (descriptor & 0x20) != 0;
    flexible = (descriptor & 0x10) != 0;
    start = (descriptor & 0x08) != 0;
    end = (descriptor & 0x04) != 0;
    has_scalability_structure = (descriptor & 0x02) != 0;

    if (has_picture_id) {
        if (payload_len < desc_len + 1) {
            return TURBO_CODEC_ERR_INVALID;
        }
        if ((rtp_payload[desc_len] & 0x80) != 0) {
            if (payload_len < desc_len + 2) {
                return TURBO_CODEC_ERR_INVALID;
            }
            desc_len += 2;
        } else {
            desc_len += 1;
        }
    }

    if (has_layer_indices) {
        if (payload_len < desc_len + 1) {
            return TURBO_CODEC_ERR_INVALID;
        }
        desc_len += 1; /* Layer indices. */
        if (!flexible) {
            if (payload_len < desc_len + 1) {
                return TURBO_CODEC_ERR_INVALID;
            }
            desc_len += 1; /* TL0PICIDX in non-flexible mode. */
        }
    }

    if (flexible && inter_picture_predicted) {
        do {
            if (payload_len < desc_len + 1) {
                return TURBO_CODEC_ERR_INVALID;
            }
        } while ((rtp_payload[desc_len++] & 0x01) != 0);
    }

    if (has_scalability_structure) {
        uint8_t ss_header;
        int spatial_layers;
        int has_resolution_data;
        int has_gof;

        if (payload_len < desc_len + 1) {
            return TURBO_CODEC_ERR_INVALID;
        }

        ss_header = rtp_payload[desc_len++];
        spatial_layers = ((ss_header >> 5) & 0x07) + 1;
        has_resolution_data = (ss_header & 0x10) != 0;
        has_gof = (ss_header & 0x08) != 0;

        if (has_resolution_data) {
            size_t resolution_bytes = (size_t)spatial_layers * 4;
            if (payload_len < desc_len + resolution_bytes) {
                return TURBO_CODEC_ERR_INVALID;
            }
            desc_len += resolution_bytes;
        }

        if (has_gof) {
            if (payload_len < desc_len + 1) {
                return TURBO_CODEC_ERR_INVALID;
            }

            uint8_t gof_count = rtp_payload[desc_len++];
            for (uint8_t i = 0; i < gof_count; i++) {
                uint8_t gof_desc;
                uint8_t ref_count;

                if (payload_len < desc_len + 1) {
                    return TURBO_CODEC_ERR_INVALID;
                }

                gof_desc = rtp_payload[desc_len++];
                ref_count = (uint8_t)(gof_desc & 0x03);
                if (payload_len < desc_len + ref_count) {
                    return TURBO_CODEC_ERR_INVALID;
                }
                desc_len += ref_count;
            }
        }
    }

    if (payload_len <= desc_len) {
        return TURBO_CODEC_ERR_NEED_MORE;
    }

    frame_data = rtp_payload + desc_len;
    frame_len = payload_len - desc_len;

    if (start) {
        ctx->reassembly_len = 0;
        ctx->reassembly_started = 1;
    } else if (!ctx->reassembly_started) {
        return TURBO_CODEC_ERR_INVALID;
    }

    if (ctx->reassembly_len + frame_len > ctx->reassembly_cap) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    memcpy(ctx->reassembly_buf + ctx->reassembly_len, frame_data, frame_len);
    ctx->reassembly_len += frame_len;

    if (*output_len < ctx->reassembly_len) {
        return TURBO_CODEC_ERR_BUFFER;
    }

    memcpy(output, ctx->reassembly_buf, ctx->reassembly_len);
    *output_len = ctx->reassembly_len;
    if (complete) {
        *complete = end ? 1 : 0;
    }
    if (end) {
        ctx->reassembly_started = 0;
    }

    return TURBO_CODEC_OK;
}

/* =============================================================================
 * Common Functions
 * ============================================================================= */

static void vpx_encoder_destroy(void *ctx_ptr) {
    vpx_encoder_ctx_t *ctx = (vpx_encoder_ctx_t *)ctx_ptr;
    int i;
    if (!ctx) return;

    for (i = 0; i < TURBO_CODEC_MAX_PACKETS; i++) {
        free(ctx->packet_bufs[i]);
    }
    vpx_codec_destroy(&ctx->codec);
    free(ctx);
}

static void vpx_decoder_destroy(void *ctx_ptr) {
    vpx_decoder_ctx_t *ctx = (vpx_decoder_ctx_t *)ctx_ptr;
    if (!ctx) return;

    vpx_codec_destroy(&ctx->codec);
    if (ctx->reassembly_buf) {
        free(ctx->reassembly_buf);
    }
    free(ctx);
}

static void vp8_destroy(void *ctx_ptr) {
    vpx_encoder_ctx_t *enc = (vpx_encoder_ctx_t *)ctx_ptr;
    if (!enc) {
        return;
    }
    if (enc->is_encoder_ctx) {
        vpx_encoder_destroy(ctx_ptr);
    } else {
        vpx_decoder_destroy(ctx_ptr);
    }
}

/* =============================================================================
 * Codec Operations
 * ============================================================================= */

const turbo_codec_ops_t turbo_vp8_codec_ops = {
    .name = "vp8",
    .type = TURBO_CODEC_TYPE_VIDEO,
    .payload_type = VP8_PAYLOAD_TYPE,
    .clock_rate = VIDEO_CLOCK_RATE,
    .create_encoder = vp8_create_encoder,
    .create_decoder = vp8_create_decoder,
    .destroy = vp8_destroy,
    .encode = vpx_encode,
    .decode = vpx_decode,
    .packetize = vp8_packetize,
    .depacketize = vp8_depacketize,
    .request_keyframe = vpx_request_keyframe,
    .set_bitrate = vpx_set_bitrate,
    .plc = NULL     /* Video doesn't have PLC */
};

const turbo_codec_ops_t turbo_vp9_codec_ops = {
    .name = "vp9",
    .type = TURBO_CODEC_TYPE_VIDEO,
    .payload_type = VP9_PAYLOAD_TYPE,
    .clock_rate = VIDEO_CLOCK_RATE,
    .create_encoder = vp9_create_encoder,
    .create_decoder = vp9_create_decoder,
    .destroy = vp8_destroy,     /* Same destroy logic */
    .encode = vpx_encode,
    .decode = vpx_decode,
    .packetize = vp9_packetize,
    .depacketize = vp9_depacketize,
    .request_keyframe = vpx_request_keyframe,
    .set_bitrate = vpx_set_bitrate,
    .plc = NULL
};

#endif /* TURBO_MEDIA_HAS_VPX */
