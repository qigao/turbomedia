#include "turbo_streamgraph_ffmpeg_delayed.h"

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>

#include <stdlib.h>
#include <string.h>

/* All mutable Codec state belongs to the caller's ONE serialized owner.
 * No callback, worker, retry queue or raw FFmpeg state crosses the C11 ABI.
 * Each packet slot is a preallocated bounded COPY (no hidden overflow). */
typedef struct tm_ffd_slot {
    uint8_t *data;
    size_t bytes;
    int64_t pts,dts;
} tm_ffd_slot;

struct tm_ffd_decoder {
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    tm_ffd_slot *slots;
    uint32_t width,height,limit,packet_limit;
    uint32_t head,tail,queued;
    uint32_t finishing,flushed,cancelled,eof;
    uint32_t have_last_pts;
    int64_t last_pts;
    int32_t codec_error;
    uint64_t admitted,sent,decoded,after_flush,flush_sends,eagain;
};

static void ff_destroy_storage(tm_ffd_decoder *d) {
    uint32_t i;
    if(!d)return;
    if(d->slots) {
        for(i=0u;i<d->limit;++i)free(d->slots[i].data);
    }
    free(d->slots);
    av_packet_free(&d->packet);
    av_frame_free(&d->frame);
    avcodec_free_context(&d->codec);
    free(d);
}
tm_ff_status tm_ffd_create(const tm_ffd_config_v1 *cfg,tm_ffd_decoder **out) {
    const AVCodec *codec;
    tm_ffd_decoder *d;
    uint32_t i;
    if(!out || *out || !cfg)return TM_FF_EINVAL;
    if(cfg->version!=TM_FFD_ABI_V1)return TM_FF_EVERSION;
    if(cfg->size!=sizeof(*cfg) || cfg->reserved || !cfg->width || !cfg->height ||
       cfg->width>4096u || cfg->height>4096u ||
       (uint64_t)cfg->width*cfg->height>TM_FS_MAX_FRAME_BYTES ||
       !cfg->queue_capacity || cfg->queue_capacity>TM_FFD_MAX_QUEUE_PACKETS ||
       !cfg->max_packet_bytes || cfg->max_packet_bytes>TM_FF_MAX_PACKET_BYTES ||
       (uint64_t)cfg->queue_capacity*cfg->max_packet_bytes>
           TM_FFD_MAX_QUEUED_BYTES)
        return TM_FF_EINVAL;
    codec=avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if(!codec)return TM_FF_EUNSUPPORTED;
    d=(tm_ffd_decoder *)calloc(1u,sizeof(*d));
    if(!d)return TM_FF_ENOMEM;
    d->width=cfg->width;
    d->height=cfg->height;
    d->limit=cfg->queue_capacity;
    d->packet_limit=cfg->max_packet_bytes;
    d->slots=(tm_ffd_slot *)calloc(d->limit,sizeof(*d->slots));
    d->frame=av_frame_alloc();
    d->packet=av_packet_alloc();
    d->codec=avcodec_alloc_context3(codec);
    if(!d->slots || !d->frame || !d->packet || !d->codec) {
        ff_destroy_storage(d);
        return TM_FF_ENOMEM;
    }
    for(i=0u;i<d->limit;++i) {
        d->slots[i].data=(uint8_t *)malloc(d->packet_limit);
        if(!d->slots[i].data) {
            ff_destroy_storage(d);
            return TM_FF_ENOMEM;
        }
    }
    d->codec->pkt_timebase=(AVRational){1,30};
    d->codec->thread_count=1;
    if(avcodec_open2(d->codec,codec,NULL)<0) {
        ff_destroy_storage(d);
        return TM_FF_ECODEC;
    }
    *out=d;
    return TM_FF_OK;
}
tm_ff_status tm_ffd_submit(tm_ffd_decoder *d,const tm_ffd_packet_v1 *p) {
    tm_ffd_slot *slot;
    if(!d || !p)return TM_FF_EINVAL;
    if(p->version!=TM_FFD_ABI_V1)return TM_FF_EVERSION;
    if(p->size!=sizeof(*p) || !p->data || !p->bytes)return TM_FF_EINVAL;
    if(d->cancelled || d->finishing || d->eof || d->codec_error)
        return TM_FF_ESTATE;
    if(p->bytes>d->packet_limit || d->queued==d->limit)return TM_FF_EFULL;
    slot=&d->slots[d->tail];
    memcpy(slot->data,p->data,p->bytes);
    slot->bytes=p->bytes;slot->pts=p->pts;slot->dts=p->dts;
    d->tail=(d->tail+1u)%d->limit;
    ++d->queued;
    ++d->admitted;
    return TM_FF_OK;
}
tm_ff_status tm_ffd_finish(tm_ffd_decoder *d) {
    if(!d)return TM_FF_EINVAL;
    if(d->finishing || d->cancelled || d->eof || d->codec_error)
        return TM_FF_ESTATE;
    d->finishing=1u;
    return TM_FF_OK;
}
tm_ff_status tm_ffd_cancel(tm_ffd_decoder *d) {
    uint32_t i;
    if(!d)return TM_FF_EINVAL;
    if(d->eof)return TM_FF_ESTATE;
    if(d->cancelled)return TM_FF_OK;
    d->cancelled=1u;
    for(i=0u;i<d->limit;++i)d->slots[i].bytes=0u;
    d->queued=0u;
    d->head=d->tail=0u;
    /* Deliberately no libavcodec call: only the Worker may use it while
     * alive. The caller must serialize cancel and worker quiescence. */
    return TM_FF_OK;
}
tm_ff_status tm_ffd_next_luma(tm_ffd_decoder *d,uint8_t *dst,size_t cap,
                               size_t *out_bytes,int64_t *out_pts) {
    uint32_t steps;
    if(!d || !dst || !out_bytes || !out_pts)return TM_FF_EINVAL;
    *out_bytes=0u;
    *out_pts=INT64_MIN;
    if(d->cancelled || d->codec_error)return TM_FF_ESTATE;
    if(d->eof)return TM_FF_EOF;
    if((uint64_t)d->width*d->height>cap)return TM_FF_EINVAL;
    /* Bounded number of non-blocking sends in a single Owner operation. */
    for(steps=0u;steps<=d->limit+2u;++steps) {
        int rc=avcodec_receive_frame(d->codec,d->frame);
        if(rc==0) {
            int64_t pts=d->frame->best_effort_timestamp;
            uint32_t y;
            if(d->frame->width!=(int)d->width ||
               d->frame->height!=(int)d->height ||
               d->frame->format!=AV_PIX_FMT_YUV420P ||
               !d->frame->data[0] ||
               d->frame->linesize[0]<(int)d->width ||
               pts==AV_NOPTS_VALUE ||
               (d->have_last_pts && pts<=d->last_pts)) {
                av_frame_unref(d->frame);
                d->codec_error=AVERROR(EINVAL);
                return TM_FF_ECODEC;
            }
            for(y=0u;y<d->height;++y)
                memcpy(dst+(size_t)y*d->width,
                       d->frame->data[0]+(size_t)y*d->frame->linesize[0],
                       d->width);
            *out_bytes=(size_t)d->width*d->height;
            *out_pts=pts;
            d->last_pts=pts;d->have_last_pts=1u;
            ++d->decoded;
            if(d->flushed)++d->after_flush;
            av_frame_unref(d->frame);
            return TM_FF_OK;
        }
        if(rc==AVERROR_EOF) {
            d->eof=1u;
            return TM_FF_EOF;
        }
        if(rc!=AVERROR(EAGAIN)) {
            d->codec_error=rc;
            return TM_FF_ECODEC;
        }
        ++d->eagain;
        if(d->queued) {
            tm_ffd_slot *slot=&d->slots[d->head];
            av_packet_unref(d->packet);
            if(av_new_packet(d->packet,(int)slot->bytes)<0)
                return TM_FF_ENOMEM; /* original private slot preserved */
            memcpy(d->packet->data,slot->data,slot->bytes);
            d->packet->pts=slot->pts;
            d->packet->dts=slot->dts;
            rc=avcodec_send_packet(d->codec,d->packet);
            av_packet_unref(d->packet);
            if(rc==AVERROR(EAGAIN))return TM_FF_EAGAIN; /* keep slot */
            if(rc<0) {
                d->codec_error=rc;
                return TM_FF_ECODEC;
            }
            slot->bytes=0u;
            d->head=(d->head+1u)%d->limit;
            --d->queued;
            ++d->sent;
            continue;
        }
        if(d->finishing && !d->flushed) {
            rc=avcodec_send_packet(d->codec,NULL);
            if(rc==AVERROR(EAGAIN))return TM_FF_EAGAIN;
            if(rc<0 && rc!=AVERROR_EOF) {
                d->codec_error=rc;
                return TM_FF_ECODEC;
            }
            d->flushed=1u;
            ++d->flush_sends;
            continue;
        }
        if(d->flushed) {
            /* Once the NULL packet was accepted, further EAGAIN is not a
             * terminal receipt and must never trigger a second NULL send. */
            d->codec_error=AVERROR(EAGAIN);
            return TM_FF_ECODEC;
        }
        return TM_FF_EAGAIN;
    }
    return TM_FF_EAGAIN; /* bounded progress, caller explicitly drives again */
}
tm_ff_status tm_ffd_stats(tm_ffd_decoder *d,tm_ffd_stats_v1 *out) {
    if(!d || !out)return TM_FF_EINVAL;
    if(out->version!=TM_FFD_ABI_V1)return TM_FF_EVERSION;
    if(out->size!=sizeof(*out))return TM_FF_EINVAL;
    *out=(tm_ffd_stats_v1)TM_FFD_STATS_V1_INIT;
    out->packets_admitted=d->admitted;
    out->packets_sent=d->sent;
    out->frames_decoded=d->decoded;
    out->frames_after_flush=d->after_flush;
    out->flush_sends=d->flush_sends;
    out->receive_eagain=d->eagain;
    out->queued=d->queued;
    out->finishing=d->finishing;
    out->eof=d->eof;
    out->cancelled=d->cancelled;
    out->codec_error=d->codec_error;
    return TM_FF_OK;
}
tm_ff_status tm_ffd_destroy(tm_ffd_decoder *d) {
    if(!d)return TM_FF_EINVAL;
    if(!d->cancelled && !d->eof)return TM_FF_ESTATE;
    ff_destroy_storage(d);
    return TM_FF_OK;
}
