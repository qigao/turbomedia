#include "turbo_streamgraph_ffmpeg.h"

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>

#include <stdlib.h>
#include <string.h>

struct tm_ff_source {
    AVCodecContext *decoder;
    AVPacket *packet;
    AVFrame *frame;
    uint8_t *encoded;
    uint32_t width,height,packet_capacity;
    size_t pending_bytes;
    uint64_t packets_submitted,frames_decoded,bytes_decoded;
    int32_t last_error;
    int finishing,flushed,cancelled,eof;
};

tm_ff_status tm_ff_create(const tm_ff_config_v1 *c,tm_ff_source **out) {
    const AVCodec *codec;
    tm_ff_source *s;
    if (!out || *out || !c) return TM_FF_EINVAL;
    if (c->version!=TM_FF_ABI_V1) return TM_FF_EVERSION;
    if (c->size!=sizeof(*c) || c->reserved || !c->width || !c->height ||
        c->width>4096u || c->height>4096u ||
        (uint64_t)c->width*c->height>TM_FS_MAX_FRAME_BYTES ||
        !c->max_packet_bytes || c->max_packet_bytes>TM_FF_MAX_PACKET_BYTES)
        return TM_FF_EINVAL;
    codec=avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) return TM_FF_EUNSUPPORTED;
    s=(tm_ff_source *)calloc(1u,sizeof(*s));
    if (!s) return TM_FF_ENOMEM;
    s->width=c->width;
    s->height=c->height;
    s->packet_capacity=c->max_packet_bytes;
    s->encoded=(uint8_t *)malloc(s->packet_capacity+AV_INPUT_BUFFER_PADDING_SIZE);
    s->packet=av_packet_alloc();
    s->frame=av_frame_alloc();
    s->decoder=avcodec_alloc_context3(codec);
    if (!s->encoded || !s->packet || !s->frame || !s->decoder) {
        (void)tm_ff_destroy(s);
        return TM_FF_ENOMEM;
    }
    memset(s->encoded+s->packet_capacity,0,AV_INPUT_BUFFER_PADDING_SIZE);
    s->decoder->thread_count=1; /* fixed owner, no implicit frame worker pool */
    if (avcodec_open2(s->decoder,codec,NULL)<0) {
        (void)tm_ff_destroy(s);
        return TM_FF_ECODEC;
    }
    *out=s;
    return TM_FF_OK;
}
tm_ff_status tm_ff_submit_mjpeg(tm_ff_source *s,const void *bytes,size_t n) {
    if (!s || !bytes || !n) return TM_FF_EINVAL;
    if (s->cancelled || s->finishing || s->eof || s->last_error)
        return TM_FF_ESTATE;
    if (n>s->packet_capacity) return TM_FF_EFULL;
    if (s->pending_bytes) return TM_FF_EFULL;
    memcpy(s->encoded,bytes,n);
    memset(s->encoded+n,0,AV_INPUT_BUFFER_PADDING_SIZE);
    s->pending_bytes=n;
    s->packets_submitted++;
    return TM_FF_OK;
}
tm_ff_status tm_ff_finish(tm_ff_source *s) {
    if (!s) return TM_FF_EINVAL;
    if (s->cancelled || s->last_error) return TM_FF_ESTATE;
    s->finishing=1;
    return TM_FF_OK;
}
tm_ff_status tm_ff_cancel(tm_ff_source *s) {
    if (!s) return TM_FF_EINVAL;
    s->cancelled=1;
    s->pending_bytes=0u;
    s->eof=1;
    return TM_FF_OK;
}
tm_ff_status tm_ff_ready(tm_ff_source *s,int *ready) {
    if (!s || !ready) return TM_FF_EINVAL;
    *ready=(s->pending_bytes!=0u || s->finishing || s->cancelled || s->eof);
    return TM_FF_OK;
}
tm_ex_source_result tm_ff_next_cpu_luma(void *ctx,uint8_t *dst,
                                          size_t capacity,size_t *size_bytes) {
    tm_ff_source *s=(tm_ff_source *)ctx;
    int rc;
    uint32_t y;
    if (!s || !dst || !size_bytes) return TM_EX_SOURCE_FAILED;
    *size_bytes=0u;
    if (s->cancelled || s->eof) return TM_EX_SOURCE_EOS;
    if (s->last_error) return TM_EX_SOURCE_FAILED;
    if ((uint64_t)s->width*s->height>capacity) return TM_EX_SOURCE_FAILED;
    if (s->pending_bytes) {
        av_packet_unref(s->packet);
        /* Non-owning packet bytes remain live through synchronous decode. */
        s->packet->data=s->encoded;
        s->packet->size=(int)s->pending_bytes;
        rc=avcodec_send_packet(s->decoder,s->packet);
        av_packet_unref(s->packet);
        if (rc<0) {
            s->last_error=rc;
            return TM_EX_SOURCE_FAILED;
        }
        s->pending_bytes=0u;
    } else if (s->finishing && !s->flushed) {
        rc=avcodec_send_packet(s->decoder,NULL);
        if (rc<0 && rc!=AVERROR_EOF) {
            s->last_error=rc;return TM_EX_SOURCE_FAILED;
        }
        s->flushed=1;
    } else if (!s->finishing) {
        /* This state is caller misuse. Empty ingress is not EOS. */
        return TM_EX_SOURCE_FAILED;
    }
    rc=avcodec_receive_frame(s->decoder,s->frame);
    if (rc==AVERROR_EOF || (rc==AVERROR(EAGAIN) && s->flushed)) {
        s->eof=1;
        av_frame_unref(s->frame);
        return TM_EX_SOURCE_EOS;
    }
    if (rc<0) {
        s->last_error=rc;
        return TM_EX_SOURCE_FAILED;
    }
    if (s->frame->width!=(int)s->width ||
        s->frame->height!=(int)s->height ||
        (s->frame->format!=AV_PIX_FMT_YUVJ420P &&
         s->frame->format!=AV_PIX_FMT_YUV420P &&
         s->frame->format!=AV_PIX_FMT_GRAY8) ||
        !s->frame->data[0] ||
        s->frame->linesize[0]<(int)s->width) {
        s->last_error=AVERROR(EINVAL);
        av_frame_unref(s->frame);
        return TM_EX_SOURCE_FAILED;
    }
    for (y=0u;y<s->height;++y)
        memcpy(dst+(size_t)y*s->width,
               s->frame->data[0]+(size_t)y*(size_t)s->frame->linesize[0],
               s->width);
    *size_bytes=(size_t)s->width*s->height;
    s->frames_decoded++;
    s->bytes_decoded+=*size_bytes;
    av_frame_unref(s->frame);
    return TM_EX_SOURCE_FRAME;
}
tm_ff_status tm_ff_stats(tm_ff_source *s,tm_ff_stats_v1 *out) {
    if (!s || !out) return TM_FF_EINVAL;
    if (out->version!=TM_FF_ABI_V1) return TM_FF_EVERSION;
    if (out->size!=sizeof(*out)) return TM_FF_EINVAL;
    *out=(tm_ff_stats_v1)TM_FF_STATS_V1_INIT;
    out->packets_submitted=s->packets_submitted;
    out->frames_decoded=s->frames_decoded;
    out->bytes_decoded=s->bytes_decoded;
    out->pending_packet=(uint32_t)(s->pending_bytes!=0u);
    out->draining=(uint32_t)s->finishing;
    out->cancelled=(uint32_t)s->cancelled;
    out->eof=(uint32_t)s->eof;
    out->ffmpeg_error=s->last_error;
    return TM_FF_OK;
}
tm_ff_status tm_ff_destroy(tm_ff_source *s) {
    if (!s) return TM_FF_EINVAL;
    avcodec_free_context(&s->decoder);
    av_packet_free(&s->packet);
    av_frame_free(&s->frame);
    free(s->encoded);
    free(s);
    return TM_FF_OK;
}
