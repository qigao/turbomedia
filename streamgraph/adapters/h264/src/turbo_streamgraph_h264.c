#include "turbo_streamgraph_h264.h"
#include "turbo_codec.h"
#include <stdlib.h>
#include <string.h>

struct tm_sh_decoder {
    tm_si_owner *ingress;
    turbo_codec_t *codec;
    uint8_t *decoded;
    size_t capacity, pending_bytes;
    tm_si_stream_v1 stream;
    tm_sh_stamp_v1 pending_stamp, last_stamp;
    uint32_t have_pending, have_last, eos, cancelled, failed;
};

tm_sh_status tm_sh_create(const tm_sh_config_v1 *cfg, tm_sh_decoder **out) {
    tm_sh_decoder *o;
    size_t pixels, needed;
    if(!cfg || !out || *out) return TM_SH_EINVAL;
    if(cfg->version!=TM_SH_ABI_V1) return TM_SH_EVERSION;
    if(cfg->size!=sizeof(*cfg) || cfg->reserved || cfg->reserved2)
        return TM_SH_EINVAL;
    if(!cfg->ingress.stream.width || !cfg->ingress.stream.height)
        return TM_SH_EINVAL;
    pixels=(size_t)cfg->ingress.stream.width*cfg->ingress.stream.height;
    needed=pixels+(pixels/2u); /* I420 even-width/even-height only */
    if(needed>TM_FS_MAX_FRAME_BYTES || !needed) return TM_SH_EINVAL;
    o=(tm_sh_decoder *)calloc(1u,sizeof(*o));
    if(!o) return TM_SH_ENOMEM;
    if(tm_si_create(&cfg->ingress,&o->ingress)!=TM_SI_OK) {
        free(o);
        return TM_SH_EINVAL;
    }
    /* The host initializes the registry explicitly. No alternate decoder
     * backend, auto-Codec registry side effect or fallback is allowed. */
    o->codec=turbo_codec_create_decoder("h264",NULL);
    if(!o->codec) {
        (void)tm_si_cancel(o->ingress);
        (void)tm_si_destroy(o->ingress);
        free(o);
        return TM_SH_ECODEC;
    }
    o->decoded=(uint8_t *)malloc(needed);
    if(!o->decoded) {
        turbo_codec_destroy(o->codec);
        (void)tm_si_cancel(o->ingress);
        (void)tm_si_destroy(o->ingress);
        free(o);
        return TM_SH_ENOMEM;
    }
    o->capacity=needed;
    o->stream=cfg->ingress.stream;
    *out=o;
    return TM_SH_OK;
}
tm_sh_status tm_sh_offer(tm_sh_decoder *o, const tm_si_packet_v1 *packet) {
    tm_si_status status;
    if(!o || !packet) return TM_SH_EINVAL;
    if(o->cancelled || o->failed || o->eos) return TM_SH_ESTATE;
    if(o->have_pending) return TM_SH_EFULL;
    status=tm_si_offer(o->ingress,packet);
    if(status==TM_SI_OK) return TM_SH_OK;
    if(status==TM_SI_EFULL) return TM_SH_EFULL;
    if(status==TM_SI_EVERSION) return TM_SH_EVERSION;
    if(status==TM_SI_ESHUTDOWN) return TM_SH_ESTATE;
    return TM_SH_EINGRESS;
}
tm_sh_status tm_sh_pump(tm_sh_decoder *o) {
    tm_si_packet_v1 p=TM_SI_PACKET_V1_INIT;
    tm_si_status ingress_status;
    size_t size_bytes;
    int code;
    if(!o) return TM_SH_EINVAL;
    if(o->cancelled || o->failed || o->eos) return TM_SH_ESTATE;
    if(o->have_pending) return TM_SH_EFULL;
    ingress_status=tm_si_take(o->ingress,&p);
    if(ingress_status==TM_SI_EMPTY) return TM_SH_EMPTY;
    if(ingress_status!=TM_SI_OK) return TM_SH_EINGRESS;
    if(p.kind==TM_SI_EOS) {
        o->eos=1u;
        (void)tm_si_settle(o->ingress);
        return TM_SH_EOS;
    }
    size_bytes=o->capacity;
    code=turbo_codec_decode(o->codec,p.bytes,p.byte_count,
                            o->decoded,&size_bytes);
    if(p.kind==TM_SI_CONFIG && code==TURBO_CODEC_ERR_NEED_MORE) {
        (void)tm_si_settle(o->ingress);
        return TM_SH_AGAIN;
    }
    if(code!=TURBO_CODEC_OK || p.kind==TM_SI_CONFIG ||
       size_bytes!=o->capacity) {
        /* A frame which did not immediately yield one full I420 frame
         * cannot be time-correlated by this restricted no-B profile.
         * Keep Codec failure terminal, without automatic packet replay. */
        (void)tm_si_settle(o->ingress);
        o->failed=1u;
        return TM_SH_ECODEC;
    }
    o->pending_stamp=(tm_sh_stamp_v1)TM_SH_STAMP_V1_INIT;
    o->pending_stamp.width=o->stream.width;
    o->pending_stamp.height=o->stream.height;
    o->pending_stamp.stream=o->stream;
    o->pending_stamp.packet_sequence=p.sequence;
    o->pending_stamp.pts_us=p.pts_us;
    o->pending_stamp.dts_us=p.dts_us;
    o->pending_stamp.decoded_bytes=size_bytes;
    o->pending_bytes=size_bytes;
    o->have_pending=1u;
    (void)tm_si_settle(o->ingress);
    return TM_SH_OK;
}
tm_sh_status tm_sh_ready(tm_sh_decoder *o,int *ready) {
    if(!o || !ready) return TM_SH_EINVAL;
    if(o->cancelled || o->failed) return TM_SH_ESTATE;
    *ready=o->have_pending || o->eos;
    return TM_SH_OK;
}
tm_ex_source_result tm_sh_next_cpu_i420(void *context,uint8_t *dst,
                                         size_t capacity,size_t *size_bytes) {
    tm_sh_decoder *o=(tm_sh_decoder *)context;
    if(!o || !dst || !size_bytes || o->cancelled || o->failed)
        return TM_EX_SOURCE_FAILED;
    if(o->have_pending) {
        if(capacity<o->pending_bytes) return TM_EX_SOURCE_FAILED;
        memcpy(dst,o->decoded,o->pending_bytes);
        *size_bytes=o->pending_bytes;
        o->last_stamp=o->pending_stamp;
        o->have_last=1u;
        o->have_pending=0u; o->pending_bytes=0u;
        return TM_EX_SOURCE_FRAME;
    }
    if(o->eos) { *size_bytes=0u; return TM_EX_SOURCE_EOS; }
    return TM_EX_SOURCE_FAILED; /* empty is NOT EOF */
}
tm_sh_status tm_sh_last_stamp(const tm_sh_decoder *o,tm_sh_stamp_v1 *out) {
    if(!o || !out || out->size!=sizeof(*out)) return TM_SH_EINVAL;
    if(out->version!=TM_SH_ABI_V1) return TM_SH_EVERSION;
    if(!o->have_last) return TM_SH_EMPTY;
    *out=o->last_stamp;
    return TM_SH_OK;
}
tm_sh_status tm_sh_cancel(tm_sh_decoder *o) {
    if(!o) return TM_SH_EINVAL;
    o->cancelled=1u;
    o->have_pending=0u;
    o->pending_bytes=0u;
    if(tm_si_cancel(o->ingress)==TM_SI_EBUSY) return TM_SH_ESTATE;
    return TM_SH_OK;
}
tm_sh_status tm_sh_destroy(tm_sh_decoder *o) {
    if(!o) return TM_SH_EINVAL;
    if(o->have_pending || (!o->eos && !o->cancelled))
        return TM_SH_EFULL;
    if(tm_si_seal(o->ingress)!=TM_SI_OK ||
       tm_si_destroy(o->ingress)!=TM_SI_OK) return TM_SH_ESTATE;
    turbo_codec_destroy(o->codec);
    free(o->decoded);
    memset(o,0,sizeof(*o));free(o);
    return TM_SH_OK;
}
