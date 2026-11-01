#include "turbo_streamgraph_h264.h"
#include <stdlib.h>
#include <string.h>

/* Only the existing TurboMedia::Codec decoder is called; it already binds
 * OpenH264. This adapter is an owner-local Source, not a Graph scheduler. */
struct tm_sgh264_source {
    tm_sgh264_config_v1 config;
    turbo_codec_t *decoder;
    uint8_t *decoded;
    size_t frame_bytes;
    uint64_t last_sequence;
    uint64_t last_pts;
    tm_fs_frame_metadata_v1 ready_meta;
    tm_fs_frame_metadata_v1 consumed_meta;
    unsigned has_sps, has_pps, has_idr;
    int ready, have_consumed, have_last_pts, finished, failed;
};

static size_t annexb_prefix(const uint8_t *data, size_t bytes, size_t off) {
    if (off + 3u > bytes || data[off] || data[off + 1u]) return 0u;
    if (data[off + 2u] == 1u) return 3u;
    if (off + 4u <= bytes && data[off + 2u] == 0u && data[off + 3u] == 1u)
        return 4u;
    return 0u;
}

/* No AVCC length prefixed, FU-A fragments, truncated access units or
 * assumption that one RTP packet == one H264 frame. Only complete Annex-B
 * access units are admitted to this decoder provider. */
static int scan_nals(const uint8_t *data, size_t bytes,
                     unsigned *sps, unsigned *pps,
                     unsigned *idr, unsigned *vcl) {
    size_t off=0u;
    if (!data || !bytes || !annexb_prefix(data,bytes,0u)) return 0;
    while (off < bytes) {
        size_t pre=annexb_prefix(data,bytes,off);
        size_t next, payload;
        uint8_t type;
        if (!pre || off > bytes-pre || off+pre>=bytes) return 0;
        payload=off+pre;
        type=data[payload] & 31u;
        if (type==0u || type>23u) return 0;
        if (type==7u) *sps=1u;
        if (type==8u) *pps=1u;
        if (type==5u) *idr=1u;
        if (type==1u || type==5u) *vcl=1u;
        next=payload+1u;
        while (next<bytes && !annexb_prefix(data,bytes,next)) next++;
        if (next>=bytes) break;
        off=next;
    }
    return 1;
}

tm_sgh264_status tm_sgh264_create(const tm_sgh264_config_v1 *cfg,
                                   tm_sgh264_source **out) {
    tm_sgh264_source *s;
    uint64_t pixels,bytes;
    if (!out || *out || !cfg) return TM_SGH264_EINVAL;
    if (cfg->version!=TM_SGH264_ABI_V1) return TM_SGH264_EVERSION;
    if (cfg->size!=sizeof(*cfg) || cfg->reserved ||
        cfg->width < 16u || cfg->width > 4096u || (cfg->width & 1u) ||
        cfg->height < 16u || cfg->height > 4096u || (cfg->height & 1u) ||
        !cfg->max_packet_bytes ||
        cfg->max_packet_bytes>TM_SGH264_MAX_PACKET_BYTES ||
        !cfg->decoded_format_id ||
        !cfg->device_session_id || !cfg->session_generation ||
        !cfg->display_id || !cfg->source_session_generation ||
        !cfg->display_generation || !cfg->geometry_generation ||
        (!cfg->capture_session_hi && !cfg->capture_session_lo) ||
        !cfg->stream_generation) return TM_SGH264_EINVAL;
    pixels=(uint64_t)cfg->width*cfg->height;
    bytes=pixels+(pixels/2u);
    if (bytes>TM_FS_MAX_FRAME_BYTES) return TM_SGH264_EUNSUPPORTED;
    s=(tm_sgh264_source *)calloc(1u,sizeof(*s));
    if (!s) return TM_SGH264_ENOMEM;
    s->config=*cfg;
    s->frame_bytes=(size_t)bytes;
    s->decoded=(uint8_t *)malloc(s->frame_bytes);
    if (!s->decoded) { free(s); return TM_SGH264_ENOMEM; }
    /* Caller explicitly initialized the current Codec registry. No hidden
     * registry reset or decoder-provider fallback can alter other sessions. */
    s->decoder=turbo_codec_create_decoder("h264",NULL);
    if (!s->decoder) { free(s->decoded); free(s); return TM_SGH264_EUNSUPPORTED; }
    *out=s;
    return TM_SGH264_OK;
}

static int metadata_matches(const tm_sgh264_source *s,
                            const tm_fs_frame_metadata_v1 *m) {
    const tm_sgh264_config_v1 *c=&s->config;
    return tm_fs_validate_metadata(m)==TM_FS_OK &&
        m->width==c->width && m->height==c->height &&
        m->format_id==c->decoded_format_id &&
        m->device_session_id==c->device_session_id &&
        m->session_generation==c->session_generation &&
        m->display_id==c->display_id &&
        m->source_session_generation==c->source_session_generation &&
        m->display_generation==c->display_generation &&
        m->geometry_generation==c->geometry_generation &&
        m->capture_session_hi==c->capture_session_hi &&
        m->capture_session_lo==c->capture_session_lo &&
        m->stream_generation==c->stream_generation &&
        m->timebase_num==1u && m->timebase_den==1000000u &&
        m->pts==m->dts &&
        (m->flags & ~(TM_FS_META_FLAG_KEY_FRAME))==0u;
}

tm_sgh264_status tm_sgh264_submit(tm_sgh264_source *s,
                                   const tm_sgh264_packet_v1 *p) {
    unsigned saw_sps=0u,saw_pps=0u,saw_idr=0u,saw_vcl=0u;
    size_t decoded_bytes;
    int codec_rc, eosp;
    if (!s || !p) return TM_SGH264_EINVAL;
    if (p->version!=TM_SGH264_ABI_V1) return TM_SGH264_EVERSION;
    if (p->size!=sizeof(*p)) return TM_SGH264_EINVAL;
    if (s->failed || s->finished) return TM_SGH264_ESTATE;
    if (s->ready) return TM_SGH264_EFULL; /* MUST NOT decode twice */
    if (p->framing!=TM_SGH264_FRAMING_ANNEX_B) return TM_SGH264_EUNSUPPORTED;
    if (p->flags & ~(TM_SGH264_PACKET_CONFIG |
                     TM_SGH264_PACKET_KEY_FRAME |
                     TM_SGH264_PACKET_EOS)) return TM_SGH264_EINVAL;
    if (!metadata_matches(s,&p->metadata)) return TM_SGH264_EPROTOCOL;
    if (s->last_sequence==UINT64_MAX ||
        p->metadata.packet_sequence!=s->last_sequence+1u) return TM_SGH264_EPROTOCOL;
    eosp=(p->flags & TM_SGH264_PACKET_EOS)!=0;
    if (eosp) {
        if (p->bytes || p->bytes_length ||
            p->flags!=TM_SGH264_PACKET_EOS) return TM_SGH264_EPROTOCOL;
        s->last_sequence=p->metadata.packet_sequence;
        s->finished=1;
        return TM_SGH264_OK;
    }
    if (!p->bytes || !p->bytes_length ||
        p->bytes_length>s->config.max_packet_bytes) return TM_SGH264_EINVAL;
    if (!scan_nals(p->bytes,p->bytes_length,&saw_sps,&saw_pps,
                   &saw_idr,&saw_vcl)) return TM_SGH264_EPROTOCOL;
    if (!!(p->flags & TM_SGH264_PACKET_KEY_FRAME)!=!!saw_idr ||
        !!(p->metadata.flags & TM_FS_META_FLAG_KEY_FRAME)!=!!saw_idr)
        return TM_SGH264_EPROTOCOL;
    if (!!(p->flags & TM_SGH264_PACKET_CONFIG) && (saw_vcl || saw_idr))
        return TM_SGH264_EPROTOCOL;
    if (!(p->flags & TM_SGH264_PACKET_CONFIG) && !saw_vcl)
        return TM_SGH264_EPROTOCOL;
    if (saw_vcl && ((!s->has_sps && !saw_sps) ||
                    (!s->has_pps && !saw_pps) ||
                    (!s->has_idr && !saw_idr))) return TM_SGH264_EPROTOCOL;
    if (saw_vcl && s->have_last_pts && p->metadata.pts<=s->last_pts)
        return TM_SGH264_EPROTOCOL;
    decoded_bytes=s->frame_bytes;
    codec_rc=turbo_codec_decode(s->decoder,p->bytes,p->bytes_length,
                                s->decoded,&decoded_bytes);
    if (codec_rc!=TURBO_CODEC_OK && codec_rc!=TURBO_CODEC_ERR_NEED_MORE) {
        s->failed=1; /* Mutable Codec state is uncertain. Must reconstruct. */
        return TM_SGH264_ECODEC;
    }
    /* A config-only packet must never masquerade as the frame that was
     * previously delayed inside a decoder. This initial no-B-frame profile
     * requires each decoded output to originate from a VCL access unit. */
    if (codec_rc==TURBO_CODEC_OK &&
        ((p->flags & TM_SGH264_PACKET_CONFIG) || !saw_vcl)) {
        s->failed=1;
        return TM_SGH264_ECODEC;
    }
    if (codec_rc==TURBO_CODEC_OK && decoded_bytes!=s->frame_bytes) {
        s->failed=1;
        return TM_SGH264_ECODEC;
    }
    s->last_sequence=p->metadata.packet_sequence;
    s->has_sps|=saw_sps;
    s->has_pps|=saw_pps;
    s->has_idr|=saw_idr;
    if (codec_rc==TURBO_CODEC_ERR_NEED_MORE) return TM_SGH264_NEED_MORE;
    s->last_pts=p->metadata.pts;
    s->have_last_pts=1;
    s->ready_meta=p->metadata;
    s->ready=1;
    return TM_SGH264_FRAME_READY;
}

tm_sgh264_status tm_sgh264_ready(tm_sgh264_source *s,int *ready) {
    if (!s || !ready) return TM_SGH264_EINVAL;
    *ready=(s->ready || s->finished) && !s->failed;
    return s->failed ? TM_SGH264_ESTATE : TM_SGH264_OK;
}

tm_ex_source_result tm_sgh264_next_cpu_i420(
    void *context,uint8_t *out,size_t capacity,size_t *n) {
    tm_sgh264_source *s=(tm_sgh264_source *)context;
    if (!s || !out || !n || s->failed) return TM_EX_SOURCE_FAILED;
    *n=0u;
    if (s->ready) {
        if (capacity<s->frame_bytes) return TM_EX_SOURCE_FAILED;
        memcpy(out,s->decoded,s->frame_bytes);
        *n=s->frame_bytes;
        s->consumed_meta=s->ready_meta;
        s->have_consumed=1;
        s->ready=0;
        return TM_EX_SOURCE_FRAME;
    }
    if (s->finished) return TM_EX_SOURCE_EOS;
    /* Empty/parameter-only Source is NOT EOF. Caller must check ready(). */
    return TM_EX_SOURCE_FAILED;
}

int tm_sgh264_source_metadata(void *context,tm_fs_frame_metadata_v1 *out) {
    tm_sgh264_source *s=(tm_sgh264_source *)context;
    if (!s || !out || !s->have_consumed || s->failed ||
        out->size!=sizeof(*out) || out->version!=TM_FS_META_ABI_V1) return -1;
    *out=s->consumed_meta;
    s->have_consumed=0;
    memset(&s->consumed_meta,0,sizeof(s->consumed_meta));
    return 0;
}

tm_sgh264_status tm_sgh264_cancel(tm_sgh264_source *s) {
    if (!s) return TM_SGH264_EINVAL;
    s->finished=1;
    s->ready=0;
    s->have_consumed=0;
    memset(&s->ready_meta,0,sizeof(s->ready_meta));
    memset(&s->consumed_meta,0,sizeof(s->consumed_meta));
    return TM_SGH264_OK;
}

tm_sgh264_status tm_sgh264_destroy(tm_sgh264_source *s) {
    if (!s) return TM_SGH264_EINVAL;
    if (s->ready || s->have_consumed) return TM_SGH264_ESTATE;
    turbo_codec_destroy(s->decoder);
    memset(s->decoded,0,s->frame_bytes);
    free(s->decoded);
    memset(s,0,sizeof(*s));
    free(s);
    return TM_SGH264_OK;
}
