/* M15c public C11 API: actual FFmpeg MPEG-2 B-frame packet reordering,
 * bounded independent copied ingress, exact EAGAIN/EOF distinction, one
 * flush and the CANONICAL Executor -> CPU FrameStore -> dual Sink model.
 * The encoder here generates deterministic input; the decoder under test
 * is the exported TurboMedia::StreamGraphFFmpeg add-on API. */
#include <turbo_streamgraph_ffmpeg_delayed.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W=64,H=64,LUMA=W*H,NFRAMES=12,PACKETS=32,PACKET_BYTES=65536 };
typedef struct fixture {
    AVPacket *packets[PACKETS];
    unsigned count;
} fixture;
typedef struct observer {
    unsigned seen;
    uint64_t last_id;
} observer;
typedef struct provider_source {
    tm_ffd_decoder *decoder;
    fixture *encoded;
    unsigned next_input,seen,source_calls,finished,eof;
} provider_source;
static unsigned level(unsigned i) {return 36u+13u*i;}

static void collect_packets(AVCodecContext *encoder,AVPacket *packet,
                            fixture *f,int finishing) {
    for(;;) {
        int rc=avcodec_receive_packet(encoder,packet);
        if(rc==AVERROR(EAGAIN)) {assert(!finishing);return;}
        if(rc==AVERROR_EOF) {assert(finishing);return;}
        assert(rc==0 && f->count<PACKETS);
        f->packets[f->count]=av_packet_clone(packet);
        assert(f->packets[f->count]!=NULL);
        ++f->count;
        av_packet_unref(packet);
    }
}
static void make_packets(fixture *f) {
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    AVCodecContext *enc;
    AVFrame *frame;
    AVPacket *pkt;
    unsigned i,y,before,reordered=0u;
    int64_t previous=INT64_MIN;
    assert(codec!=NULL);
    enc=avcodec_alloc_context3(codec);
    assert(enc!=NULL);
    enc->width=W;enc->height=H;
    enc->time_base=(AVRational){1,30};enc->framerate=(AVRational){30,1};
    enc->pix_fmt=AV_PIX_FMT_YUV420P;
    enc->bit_rate=1200000;enc->gop_size=12;
    enc->max_b_frames=2;enc->thread_count=1;
    assert(avcodec_open2(enc,codec,NULL)==0);
    frame=av_frame_alloc();pkt=av_packet_alloc();
    assert(frame&&pkt);
    frame->width=W;frame->height=H;frame->format=AV_PIX_FMT_YUV420P;
    assert(av_frame_get_buffer(frame,32)==0);
    for(i=0u;i<NFRAMES;++i) {
        assert(av_frame_make_writable(frame)==0);
        for(y=0u;y<H;++y)
            memset(frame->data[0]+(size_t)y*frame->linesize[0],
                   (int)level(i),W);
        for(y=0u;y<H/2u;++y) {
            memset(frame->data[1]+(size_t)y*frame->linesize[1],128,W/2);
            memset(frame->data[2]+(size_t)y*frame->linesize[2],128,W/2);
        }
        frame->pts=(int64_t)i;
        assert(avcodec_send_frame(enc,frame)==0);
        collect_packets(enc,pkt,f,0);
    }
    before=f->count;
    assert(before<NFRAMES);
    assert(avcodec_send_frame(enc,NULL)==0);
    collect_packets(enc,pkt,f,1);
    assert(f->count==NFRAMES);
    for(i=0u;i<f->count;++i) {
        assert(f->packets[i]->pts!=AV_NOPTS_VALUE);
        if(f->packets[i]->pts<previous)++reordered;
        previous=f->packets[i]->pts;
    }
    assert(reordered>=1u); /* qualified real out-of-order B frames */
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
}
static void destroy_packets(fixture *f) {
    unsigned i;
    for(i=0u;i<f->count;++i)av_packet_free(&f->packets[i]);
}
static tm_ffd_config_v1 config(void) {
    tm_ffd_config_v1 c=TM_FFD_CONFIG_V1_INIT;
    c.width=W;c.height=H;
    c.queue_capacity=2u; /* deliberately smaller than GOP */
    c.max_packet_bytes=PACKET_BYTES;
    return c;
}
static tm_ff_status admit(tm_ffd_decoder *d,const AVPacket *p) {
    tm_ffd_packet_v1 input=TM_FFD_PACKET_V1_INIT;
    tm_ff_status result;
    uint8_t *temporary;
    assert(p->size>0 && p->size<=PACKET_BYTES);
    temporary=(uint8_t *)malloc((size_t)p->size);
    assert(temporary!=NULL);
    memcpy(temporary,p->data,(size_t)p->size);
    input.data=temporary;input.bytes=(size_t)p->size;
    input.pts=p->pts;input.dts=p->dts;
    result=tm_ffd_submit(d,&input);
    /* The Host invalidates its entire borrowed packet immediately:
     * the production session MUST decode only the bounded private COPY. */
    memset(temporary,0,(size_t)p->size);
    free(temporary);
    return result;
}
static tm_ex_source_result source_next(void *ctx,uint8_t *dst,
                                       size_t capacity,size_t *out_bytes) {
    provider_source *s=(provider_source *)ctx;
    unsigned step;
    ++s->source_calls;
    for(step=0u;step<NFRAMES*5u;++step) {
        tm_ffd_stats_v1 stats=TM_FFD_STATS_V1_INIT;
        tm_ff_status rc;
        int64_t pts=INT64_MIN;
        assert(tm_ffd_stats(s->decoder,&stats)==TM_FF_OK);
        while(stats.queued<2u && s->next_input<s->encoded->count) {
            rc=admit(s->decoder,s->encoded->packets[s->next_input]);
            if(rc==TM_FF_EFULL)break;
            if(rc!=TM_FF_OK)return TM_EX_SOURCE_FAILED;
            ++s->next_input; ++stats.queued;
        }
        if(s->next_input==s->encoded->count && !s->finished) {
            if(tm_ffd_finish(s->decoder)!=TM_FF_OK)return TM_EX_SOURCE_FAILED;
            s->finished=1u;
        }
        rc=tm_ffd_next_luma(s->decoder,dst,capacity,out_bytes,&pts);
        if(rc==TM_FF_OK) {
            if(pts!=(int64_t)s->seen || s->seen>=NFRAMES ||
               *out_bytes!=LUMA ||
               abs((int)dst[0]-(int)level(s->seen))>22 ||
               abs((int)dst[LUMA-1]-(int)level(s->seen))>22)
                return TM_EX_SOURCE_FAILED;
            ++s->seen;
            return TM_EX_SOURCE_FRAME;
        }
        if(rc==TM_FF_EOF) {
            ++s->eof;
            return TM_EX_SOURCE_EOS;
        }
        if(rc!=TM_FF_EAGAIN)return TM_EX_SOURCE_FAILED;
        /* An explicit nonterminal no-frame result is NOT EOS. More
         * queued packets may still be in the finite decoder ring. The
         * bounded loop, not an implicit scheduler, controls progress. */
    }
    return TM_EX_SOURCE_FAILED;
}
static int visit(void *ctx,const tm_fs_frame_ref_v1 *ref,
                 const tm_fs_cpu_view_v1 *frame) {
    observer *v=(observer *)ctx;
    unsigned i=v->seen;
    if(!ref || !ref->frame_id || ref->frame_id<=v->last_id ||
       !frame || !frame->data || frame->size_bytes!=LUMA ||
       i>=NFRAMES ||
       abs((int)frame->data[0]-(int)level(i))>22 ||
       abs((int)frame->data[LUMA-1]-(int)level(i))>22)return -1;
    v->last_id=ref->frame_id;
    ++v->seen;
    return 0;
}
static tm_sg_port_v1 port(const char *name,uint32_t dir) {
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=name;p.direction=dir;
    p.kind=TM_SG_VIDEO_FRAME;p.memory_domain=TM_SG_MEMORY_CPU;
    p.format_id=301u;p.timebase_num=1u;p.timebase_den=30u;
    return p;
}
static tm_sg_program *graph(void) {
    tm_sg_port_v1 src=port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 middle[2]={{0},{0}};
    tm_sg_port_v1 sink=port("in",TM_SG_INPUT);
    tm_sg_node_v1 nodes[4];
    tm_sg_edge_v1 edges[3];
    const char *ids[]={"source","processor","record","preview"};
    tm_sg_program *out=NULL;
    unsigned i;
    middle[0]=port("in",TM_SG_INPUT);middle[1]=port("out",TM_SG_OUTPUT);
    for(i=0u;i<4u;++i) {
        nodes[i]=(tm_sg_node_v1)TM_SG_NODE_V1_INIT;
        nodes[i].id=ids[i];
        nodes[i].kind=i==0u?TM_SG_SOURCE:(i==1u?TM_SG_PROCESSOR:TM_SG_SINK);
        nodes[i].port_count=i==1u?2u:1u;
        nodes[i].ports=i==0u?&src:(i==1u?middle:&sink);
    }
    for(i=0u;i<3u;++i)edges[i]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
    edges[0].source_node="source";edges[0].source_port="out";
    edges[0].target_node="processor";edges[0].target_port="in";
    for(i=1u;i<3u;++i) {
        edges[i].source_node="processor";edges[i].source_port="out";
        edges[i].target_node=ids[i+1u];edges[i].target_port="in";
    }
    assert(tm_sg_compile(nodes,4u,edges,3u,NULL,&out,NULL)==TM_SG_OK);
    return out;
}
static void run_graph(fixture *f) {
    tm_ffd_decoder *decoder=NULL;
    tm_ffd_config_v1 cfg=config();
    provider_source source={0};
    tm_sg_program *g=graph();
    tm_fs_store *store=NULL;
    tm_fs_config_v1 fs=TM_FS_CONFIG_V1_INIT;
    tm_ex_owner *exec=NULL;
    tm_ex_config_v1 ec=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 binds[4];
    tm_ffd_stats_v1 stats=TM_FFD_STATS_V1_INIT;
    tm_ex_stats_v1 exstats=TM_EX_STATS_V1_INIT;
    observer visits[3]={{0}};
    const char *ids[]={"source","processor","record","preview"};
    tm_ex_status status;
    unsigned i,step,calls;
    assert(tm_ffd_create(&cfg,&decoder)==TM_FF_OK);
    source.decoder=decoder;source.encoded=f;
    fs.store_id=190194u;
    fs.slot_capacity=2u;fs.max_frame_bytes=LUMA;
    fs.max_pins=8u;fs.max_leases=8u;fs.max_retained_bytes=2u*LUMA;
    assert(tm_fs_create(&fs,&store)==TM_FS_OK);
    ec.max_frame_bytes=LUMA;ec.source_session_generation=190u;
    ec.display_generation=194u;ec.fanout.branch_count=2u;
    for(i=0u;i<2u;++i) {
        ec.fanout.branches[i]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
        ec.fanout.branches[i].policy=TM_FO_ORDERED_LOSSLESS;
        ec.fanout.branches[i].queue_depth=1u;
    }
    for(i=0u;i<4u;++i) {
        binds[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        binds[i].node_id=ids[i];
        binds[i].ctx=i==0u?(void *)&source:(void *)&visits[i-1u];
        binds[i].visit=i==0u?NULL:visit;
    }
    binds[0].source_next=source_next;
    assert(tm_ex_create(g,store,&ec,binds,4u,&exec)==TM_EX_OK);
    assert(tm_ex_pump_source(exec)==TM_EX_OK);
    assert(tm_ex_pump_source(exec)==TM_EX_EFULL);
    calls=source.source_calls;
    assert(source.seen==2u);
    assert(tm_ex_pump_source(exec)==TM_EX_EFULL);
    assert(source.source_calls==calls);
    assert(tm_ex_pump_sink(exec,0u)==TM_EX_OK);
    assert(tm_ex_pump_sink(exec,1u)==TM_EX_OK);
    for(step=0u;step<NFRAMES*5u;++step) {
        status=tm_ex_pump_source(exec);
        if(status==TM_EX_EOS)break;
        assert(status==TM_EX_OK || status==TM_EX_EFULL);
        assert(tm_ex_pump_sink(exec,0u)==TM_EX_OK);
        assert(tm_ex_pump_sink(exec,1u)==TM_EX_OK);
    }
    assert(step<NFRAMES*5u);
    assert(source.seen==NFRAMES && source.eof==1u);
    for(i=0u;i<3u;++i)assert(visits[i].seen==NFRAMES);
    assert(tm_ffd_stats(decoder,&stats)==TM_FF_OK);
    assert(stats.packets_admitted==NFRAMES &&
           stats.packets_sent==NFRAMES &&
           stats.frames_decoded==NFRAMES &&
           stats.frames_after_flush>=1u && stats.flush_sends==1u &&
           stats.receive_eagain>=1u && stats.queued==0u &&
           stats.finishing==1u && stats.eof==1u && !stats.cancelled &&
           !stats.codec_error);
    assert(tm_ex_stats(exec,&exstats)==TM_EX_OK);
    assert(exstats.sourced==NFRAMES && exstats.processed==NFRAMES &&
           exstats.delivered==2u*NFRAMES &&
           exstats.submitted==NFRAMES && exstats.backpressure>=1u);
    assert(tm_ex_destroy(exec)==TM_EX_OK);
    assert(tm_fs_seal(store)==TM_FS_OK);
    assert(tm_fs_destroy(store)==TM_FS_OK);
    tm_sg_program_destroy(g);
    assert(tm_ffd_destroy(decoder)==TM_FF_OK);
}
static void run_admission_cancel(fixture *f) {
    tm_ffd_config_v1 cfg=config();
    tm_ffd_decoder *d=NULL;
    tm_ffd_stats_v1 stats=TM_FFD_STATS_V1_INIT;
    tm_ffd_packet_v1 bad=TM_FFD_PACKET_V1_INIT;
    uint8_t scratch[LUMA];
    size_t bytes=5u;
    int64_t pts=5;
    assert(tm_ffd_create(&cfg,&d)==TM_FF_OK);
    assert(tm_ffd_next_luma(d,scratch,sizeof(scratch),&bytes,&pts)
           ==TM_FF_EAGAIN);
    assert(bytes==0u && pts==INT64_MIN);
    assert(admit(d,f->packets[0])==TM_FF_OK);
    assert(admit(d,f->packets[1])==TM_FF_OK);
    assert(admit(d,f->packets[2])==TM_FF_EFULL);
    bad.data=f->packets[0]->data;bad.bytes=PACKET_BYTES+1u;
    assert(tm_ffd_submit(d,&bad)==TM_FF_EFULL);
    assert(tm_ffd_finish(d)==TM_FF_OK);
    assert(tm_ffd_finish(d)==TM_FF_ESTATE);
    assert(tm_ffd_cancel(d)==TM_FF_OK);
    assert(tm_ffd_cancel(d)==TM_FF_OK);
    assert(tm_ffd_next_luma(d,scratch,sizeof(scratch),&bytes,&pts)
           ==TM_FF_ESTATE);
    assert(bytes==0u && pts==INT64_MIN);
    assert(admit(d,f->packets[0])==TM_FF_ESTATE);
    assert(tm_ffd_stats(d,&stats)==TM_FF_OK);
    assert(stats.queued==0u && stats.cancelled && stats.finishing &&
           stats.packets_admitted==2u && stats.packets_sent==0u &&
           stats.frames_decoded==0u && stats.flush_sends==0u);
    assert(tm_ffd_destroy(d)==TM_FF_OK);
}
int main(void) {
    fixture f={0};
    make_packets(&f);
    run_admission_cancel(&f);
    run_graph(&f);
    destroy_packets(&f);
    puts("installed-style delayed MPEG-2 packet/frame Provider and graph: PASS");
    return 0;
}
