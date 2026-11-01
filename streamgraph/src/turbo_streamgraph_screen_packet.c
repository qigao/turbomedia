#include "turbo_streamgraph_screen_packet.h"
#include <stdlib.h>
#include <string.h>

struct tm_si_owner {
    tm_si_config_v1 cfg;
    uint8_t *bytes;
    tm_si_packet_v1 active;
    uint64_t last_sequence;
    int64_t last_pts_us;
    uint32_t pending; /* 0 FREE, 1 READY, 2 TAKEN */
    uint32_t sealed, cancelled, have_sps, have_pps, have_idr;
};
static int stream_valid(const tm_si_stream_v1 *s) {
    return s && s->size == sizeof(*s) && s->version == TM_SI_ABI_V1 &&
        s->device_session_id && s->session_generation && s->display_id &&
        s->display_generation && s->geometry_generation &&
        (s->capture_hi || s->capture_lo) && s->stream_generation &&
        s->width >= 2u && s->height >= 2u &&
        !(s->width & 1u) && !(s->height & 1u) &&
        s->width <= 4096u && s->height <= 4096u;
}
static int stream_equal(const tm_si_stream_v1 *a, const tm_si_stream_v1 *b) {
    return stream_valid(a) &&
        a->device_session_id==b->device_session_id &&
        a->session_generation==b->session_generation &&
        a->display_id==b->display_id &&
        a->display_generation==b->display_generation &&
        a->geometry_generation==b->geometry_generation &&
        a->capture_hi==b->capture_hi && a->capture_lo==b->capture_lo &&
        a->stream_generation==b->stream_generation &&
        a->width==b->width && a->height==b->height;
}
static size_t start_code(const uint8_t *p, size_t n, size_t at) {
    if (at+3u<=n && p[at]==0u && p[at+1u]==0u && p[at+2u]==1u) return 3u;
    if (at+4u<=n && p[at]==0u && p[at+1u]==0u &&
        p[at+2u]==0u && p[at+3u]==1u) return 4u;
    return 0u;
}
/* Framing/NAL-type validator, not a semantic SPS/PPS or slice decoder.
 * A platform AVC length-prefix requires a separate explicit converter. */
static int nal_types(const uint8_t *p, size_t n, unsigned *types) {
    size_t pos=0u;
    *types=0u;
    while(pos<n) {
        size_t prefix=start_code(p,n,pos), begin, end, i;
        unsigned type;
        if(!prefix) return 0;
        begin=pos+prefix;
        if(begin>=n || (p[begin]&0x80u)) return 0;
        type=p[begin]&0x1fu;
        if(!type || type>23u) return 0;
        if(type==7u) *types|=1u;
        if(type==8u) *types|=2u;
        if(type==5u) *types|=4u;
        if(type==1u) *types|=8u;
        end=n;
        for(i=begin+1u;i<n;i++) if(start_code(p,n,i)) {end=i;break;}
        if(end==begin) return 0;
        pos=end;
    }
    return *types!=0u;
}
tm_si_status tm_si_create(const tm_si_config_v1 *c, tm_si_owner **out) {
    tm_si_owner *o;
    if(!out || *out || !c) return TM_SI_EINVAL;
    if(c->version!=TM_SI_ABI_V1 || c->stream.version!=TM_SI_ABI_V1)
        return TM_SI_EVERSION;
    if(c->size!=sizeof(*c) || c->reserved || !stream_valid(&c->stream) ||
       !c->max_packet_bytes || c->max_packet_bytes>TM_SI_MAX_PACKET_BYTES)
        return TM_SI_EINVAL;
    o=(tm_si_owner *)calloc(1u,sizeof(*o));
    if(!o) return TM_SI_ENOMEM;
    o->bytes=(uint8_t *)malloc(c->max_packet_bytes);
    if(!o->bytes) {free(o);return TM_SI_ENOMEM;}
    o->cfg=*c;
    *out=o;
    return TM_SI_OK;
}
tm_si_status tm_si_offer(tm_si_owner *o, const tm_si_packet_v1 *p) {
    unsigned types=0u;
    if(!o || !p) return TM_SI_EINVAL;
    if(p->version!=TM_SI_ABI_V1 || p->stream.version!=TM_SI_ABI_V1)
        return TM_SI_EVERSION;
    if(p->size!=sizeof(*p) || p->reserved || !stream_equal(&p->stream,&o->cfg.stream))
        return TM_SI_ESTALE;
    if(o->sealed) return TM_SI_ESHUTDOWN;
    if(o->pending) return TM_SI_EFULL;
    if(!p->sequence || p->sequence!=o->last_sequence+1u)
        return TM_SI_ESTALE; /* no invisible packet loss */
    if(p->pts_us<0 || p->dts_us<0 || p->pts_us!=p->dts_us ||
       (o->last_sequence && p->pts_us<o->last_pts_us))
        return TM_SI_EREORDER;
    if(p->kind<TM_SI_CONFIG || p->kind>TM_SI_EOS ||
       p->byte_count>o->cfg.max_packet_bytes) return TM_SI_EINVAL;
    if(p->kind==TM_SI_EOS) {
        if(!o->have_idr || p->bytes || p->byte_count) return TM_SI_EFORMAT;
    } else {
        if(!p->bytes || !p->byte_count ||
           !nal_types(p->bytes,p->byte_count,&types)) return TM_SI_EFORMAT;
        if(p->kind==TM_SI_CONFIG) {
            if(o->have_idr || !(types&3u) || (types&12u)) return TM_SI_EFORMAT;
        } else if(p->kind==TM_SI_IDR) {
            if(!o->have_sps || !o->have_pps || !(types&4u) || (types&11u))
                return TM_SI_EFORMAT;
        } else if(!o->have_idr || !(types&8u) || (types&7u)) {
            return TM_SI_EFORMAT;
        }
    }
    if(p->byte_count) memcpy(o->bytes,p->bytes,p->byte_count);
    o->active=*p;
    o->active.bytes=p->byte_count?o->bytes:NULL;
    o->pending=1u;
    return TM_SI_OK;
}
tm_si_status tm_si_take(tm_si_owner *o, tm_si_packet_v1 *out) {
    if(!o || !out) return TM_SI_EINVAL;
    if(!o->pending) return TM_SI_EMPTY;
    if(o->pending!=1u) return TM_SI_EBUSY;
    *out=o->active;
    o->pending=2u;
    return TM_SI_OK;
}
tm_si_status tm_si_settle(tm_si_owner *o) {
    if(!o) return TM_SI_EINVAL;
    if(o->pending!=2u) return TM_SI_EBUSY;
    if(!o->cancelled) {
        unsigned types=0u;
        if(o->active.kind==TM_SI_CONFIG) {
            (void)nal_types(o->bytes,o->active.byte_count,&types);
            if(types&1u) o->have_sps=1u;
            if(types&2u) o->have_pps=1u;
        } else if(o->active.kind==TM_SI_IDR) {
            o->have_idr=1u;
        } else if(o->active.kind==TM_SI_EOS) o->sealed=1u;
        o->last_sequence=o->active.sequence;
        o->last_pts_us=o->active.pts_us;
    }
    o->pending=0u;
    memset(&o->active,0,sizeof(o->active));
    return TM_SI_OK;
}
tm_si_status tm_si_seal(tm_si_owner *o) {
    if(!o) return TM_SI_EINVAL;
    o->sealed=1u;
    return TM_SI_OK;
}
tm_si_status tm_si_cancel(tm_si_owner *o) {
    if(!o) return TM_SI_EINVAL;
    o->sealed=1u; o->cancelled=1u;
    if(o->pending==2u) return TM_SI_EBUSY;
    o->pending=0u;
    memset(&o->active,0,sizeof(o->active));
    return TM_SI_OK;
}
tm_si_status tm_si_destroy(tm_si_owner *o) {
    if(!o) return TM_SI_EINVAL;
    if(o->pending || !o->sealed) return TM_SI_EBUSY;
    free(o->bytes);
    memset(o,0,sizeof(*o));
    free(o);
    return TM_SI_OK;
}
