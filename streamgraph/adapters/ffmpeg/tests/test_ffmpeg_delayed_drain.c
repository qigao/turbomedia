/* M15c: a REAL delayed MPEG-2 B-frame codec, backed by libavcodec.
 * The test-only Source intentionally uses no separate scheduler or raw
 * public FFmpeg ABI. It exercises the canonical Executor -> FrameStore ->
 * Processor -> two independent ordered lossless Sinks under backpressure.
 *
 * This is not a shipped MPEG-2 Source profile: the installed tm_ff_source
 * still has an explicitly MJPEG-only admission contract.
 */
#include <turbo_streamgraph_executor.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/error.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    WIDTH=64, HEIGHT=64, LUMA=WIDTH*HEIGHT,
    FRAME_COUNT=12, PACKET_LIMIT=32
};

typedef struct delayed_source {
    AVCodecContext *decoder;
    AVFrame *frame;
    AVPacket *packets[PACKET_LIMIT]; /* test-owned, finite compressed fixture */
    unsigned packet_count,next_packet;
    unsigned frames_returned,frames_after_flush,receive_eagain;
    unsigned packets_sent,flush_sends,eof;
} delayed_source;

typedef struct sink_observer {
    unsigned visits;
    uint64_t previous_frame;
} sink_observer;

static unsigned level(unsigned i) { return 36u+13u*i; }

static void append_packet(delayed_source *src,AVPacket *packet) {
    assert(src->packet_count<PACKET_LIMIT);
    src->packets[src->packet_count]=av_packet_clone(packet);
    assert(src->packets[src->packet_count]!=NULL);
    ++src->packet_count;
    av_packet_unref(packet);
}
static void harvest_packets(AVCodecContext *encoder,AVPacket *packet,
                            delayed_source *src,int finishing) {
    for(;;) {
        int rc=avcodec_receive_packet(encoder,packet);
        if(rc==AVERROR(EAGAIN)) {
            assert(!finishing);
            return;
        }
        if(rc==AVERROR_EOF) {
            assert(finishing);
            return;
        }
        assert(rc==0);
        append_packet(src,packet);
    }
}
static void make_delayed_fixture(delayed_source *src) {
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    AVCodecContext *encoder;
    AVFrame *frame;
    AVPacket *packet;
    unsigned i,y;
    unsigned packets_before_flush;
    unsigned reordered=0u;
    int64_t previous_pts=INT64_MIN;

    assert(codec!=NULL);
    encoder=avcodec_alloc_context3(codec);
    assert(encoder!=NULL);
    encoder->width=WIDTH;encoder->height=HEIGHT;
    encoder->time_base=(AVRational){1,30};
    encoder->framerate=(AVRational){30,1};
    encoder->pix_fmt=AV_PIX_FMT_YUV420P;
    encoder->bit_rate=1200000;
    encoder->gop_size=12;
    encoder->max_b_frames=2; /* force a delayed/reordering Codec profile */
    encoder->thread_count=1;
    assert(avcodec_open2(encoder,codec,NULL)==0);

    frame=av_frame_alloc();packet=av_packet_alloc();
    assert(frame!=NULL && packet!=NULL);
    frame->width=WIDTH;frame->height=HEIGHT;
    frame->format=AV_PIX_FMT_YUV420P;
    assert(av_frame_get_buffer(frame,32)==0);
    for(i=0u;i<FRAME_COUNT;++i) {
        assert(av_frame_make_writable(frame)==0);
        for(y=0u;y<HEIGHT;++y)
            memset(frame->data[0]+(size_t)y*frame->linesize[0],
                   (int)level(i),WIDTH);
        for(y=0u;y<HEIGHT/2u;++y) {
            memset(frame->data[1]+(size_t)y*frame->linesize[1],128,WIDTH/2);
            memset(frame->data[2]+(size_t)y*frame->linesize[2],128,WIDTH/2);
        }
        frame->pts=(int64_t)i;
        assert(avcodec_send_frame(encoder,frame)==0);
        harvest_packets(encoder,packet,src,0);
    }
    packets_before_flush=src->packet_count;
    assert(packets_before_flush<FRAME_COUNT);
    assert(avcodec_send_frame(encoder,NULL)==0);
    harvest_packets(encoder,packet,src,1);
    assert(src->packet_count==FRAME_COUNT);

    for(i=0u;i<src->packet_count;++i) {
        assert(src->packets[i]->pts!=AV_NOPTS_VALUE);
        if(src->packets[i]->pts<previous_pts)++reordered;
        previous_pts=src->packets[i]->pts;
    }
    assert(reordered>=1u); /* prove B-frame decode-order != display-order */

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&encoder);

    codec=avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    assert(codec!=NULL);
    src->decoder=avcodec_alloc_context3(codec);
    assert(src->decoder!=NULL);
    src->decoder->pkt_timebase=(AVRational){1,30};
    src->decoder->thread_count=1;
    assert(avcodec_open2(src->decoder,codec,NULL)==0);
    src->frame=av_frame_alloc();
    assert(src->frame!=NULL);
}

/* Caller-driven: no unbounded frame queue. The Codec's internal reorder
 * buffer may delay results; only ONE decoded luma is copied per invocation.
 * Executor backpressure prevents this callback from advancing again. */
static tm_ex_source_result delayed_next(void *context,uint8_t *dst,
                                        size_t cap,size_t *out_bytes) {
    delayed_source *src=(delayed_source *)context;
    unsigned iteration;
    if(!src || !dst || !out_bytes || cap<LUMA)return TM_EX_SOURCE_FAILED;
    *out_bytes=0u;
    for(iteration=0u;iteration<PACKET_LIMIT*4u;++iteration) {
        int rc=avcodec_receive_frame(src->decoder,src->frame);
        if(rc==0) {
            unsigned i=src->frames_returned,y;
            int64_t pts=src->frame->best_effort_timestamp;
            if(i>=FRAME_COUNT || pts!=(int64_t)i ||
               src->frame->width!=WIDTH || src->frame->height!=HEIGHT ||
               src->frame->format!=AV_PIX_FMT_YUV420P ||
               !src->frame->data[0] ||
               src->frame->linesize[0]<WIDTH) {
                av_frame_unref(src->frame);
                return TM_EX_SOURCE_FAILED;
            }
            if(abs((int)src->frame->data[0][0]-(int)level(i))>22) {
                av_frame_unref(src->frame);
                return TM_EX_SOURCE_FAILED;
            }
            for(y=0u;y<HEIGHT;++y)
                memcpy(dst+(size_t)y*WIDTH,
                       src->frame->data[0]+(size_t)y*src->frame->linesize[0],
                       WIDTH);
            *out_bytes=LUMA;
            if(src->flush_sends)++src->frames_after_flush;
            ++src->frames_returned;
            av_frame_unref(src->frame);
            return TM_EX_SOURCE_FRAME;
        }
        if(rc==AVERROR_EOF) {
            src->eof=1u;
            return TM_EX_SOURCE_EOS;
        }
        if(rc!=AVERROR(EAGAIN))return TM_EX_SOURCE_FAILED;
        ++src->receive_eagain;
        if(src->next_packet<src->packet_count) {
            rc=avcodec_send_packet(src->decoder,src->packets[src->next_packet]);
            if(rc!=0)return TM_EX_SOURCE_FAILED;
            ++src->next_packet;
            ++src->packets_sent;
        } else if(!src->flush_sends) {
            rc=avcodec_send_packet(src->decoder,NULL);
            if(rc!=0)return TM_EX_SOURCE_FAILED;
            ++src->flush_sends;
        } else {
            /* FFmpeg draining must yield held frames or EOF, never a
             * misleading infinite EAGAIN / implicit second NULL send. */
            return TM_EX_SOURCE_FAILED;
        }
    }
    return TM_EX_SOURCE_FAILED;
}

static int observe(void *ctx,const tm_fs_frame_ref_v1 *ref,
                   const tm_fs_cpu_view_v1 *frame) {
    sink_observer *o=(sink_observer *)ctx;
    unsigned index=o->visits;
    if(!ref || !ref->frame_id || ref->frame_id<=o->previous_frame ||
       !frame || !frame->data || frame->size_bytes!=LUMA ||
       index>=FRAME_COUNT ||
       abs((int)frame->data[0]-(int)level(index))>22 ||
       abs((int)frame->data[LUMA-1]-(int)level(index))>22)
        return -1;
    o->previous_frame=ref->frame_id;
    ++o->visits;
    return 0;
}
static tm_sg_port_v1 port(const char *name,uint32_t direction) {
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=name;p.direction=direction;
    p.kind=TM_SG_VIDEO_FRAME;p.memory_domain=TM_SG_MEMORY_CPU;
    p.format_id=301u;p.timebase_num=1u;p.timebase_den=30u;
    return p;
}
static tm_sg_program *make_graph(void) {
    tm_sg_port_v1 source=port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 middle[2]={TM_SG_PORT_V1_INIT,TM_SG_PORT_V1_INIT};
    tm_sg_port_v1 sink=port("in",TM_SG_INPUT);
    tm_sg_node_v1 nodes[4];
    tm_sg_edge_v1 edges[3];
    const char *ids[]={"source","processor","record","preview"};
    tm_sg_program *graph=NULL;
    unsigned i;
    middle[0]=port("in",TM_SG_INPUT);
    middle[1]=port("out",TM_SG_OUTPUT);
    for(i=0u;i<4u;++i) {
        nodes[i]=(tm_sg_node_v1)TM_SG_NODE_V1_INIT;
        nodes[i].id=ids[i];
        nodes[i].kind=i==0u?TM_SG_SOURCE:(i==1u?TM_SG_PROCESSOR:TM_SG_SINK);
        nodes[i].port_count=i==1u?2u:1u;
        nodes[i].ports=i==0u?&source:(i==1u?middle:&sink);
    }
    for(i=0u;i<3u;++i)edges[i]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
    edges[0].source_node="source";edges[0].source_port="out";
    edges[0].target_node="processor";edges[0].target_port="in";
    for(i=1u;i<3u;++i) {
        edges[i].source_node="processor";edges[i].source_port="out";
        edges[i].target_node=ids[i+1u];edges[i].target_port="in";
    }
    assert(tm_sg_compile(nodes,4u,edges,3u,NULL,&graph,NULL)==TM_SG_OK);
    return graph;
}
static void run_delayed_drain(void) {
    delayed_source source={0};
    sink_observer visits[3]={{0}};
    tm_sg_program *graph=make_graph();
    tm_fs_config_v1 fs=TM_FS_CONFIG_V1_INIT;
    tm_fs_store *store=NULL;
    tm_ex_config_v1 cfg=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 binds[4];
    tm_ex_owner *exec=NULL;
    tm_ex_stats_v1 stats=TM_EX_STATS_V1_INIT;
    const char *ids[]={"source","processor","record","preview"};
    tm_ex_status status;
    unsigned step,i;
    make_delayed_fixture(&source);
    fs.store_id=190190u;fs.slot_capacity=2u;
    fs.max_frame_bytes=LUMA;fs.max_retained_bytes=2u*LUMA;
    fs.max_leases=8u;fs.max_pins=8u;
    assert(tm_fs_create(&fs,&store)==TM_FS_OK);
    cfg.max_frame_bytes=LUMA;
    cfg.source_session_generation=190u;cfg.display_generation=191u;
    cfg.fanout.branch_count=2u;
    for(i=0u;i<2u;++i) {
        cfg.fanout.branches[i]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
        cfg.fanout.branches[i].policy=TM_FO_ORDERED_LOSSLESS;
        cfg.fanout.branches[i].queue_depth=1u;
    }
    for(i=0u;i<4u;++i) {
        binds[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        binds[i].node_id=ids[i];
        binds[i].ctx=i==0u?(void *)&source:(void *)&visits[i-1u];
        binds[i].visit=i==0u?NULL:observe;
    }
    binds[0].source_next=delayed_next;
    assert(tm_ex_create(graph,store,&cfg,binds,4u,&exec)==TM_EX_OK);

    assert(tm_ex_pump_source(exec)==TM_EX_OK);
    assert(tm_ex_pump_source(exec)==TM_EX_EFULL);
    {
        unsigned produced=source.frames_returned;
        assert(produced==2u);
        /* No extra Codec receives while the Executor owns a pending frame. */
        assert(tm_ex_pump_source(exec)==TM_EX_EFULL);
        assert(source.frames_returned==produced);
    }
    assert(tm_ex_pump_sink(exec,0u)==TM_EX_OK);
    assert(tm_ex_pump_sink(exec,1u)==TM_EX_OK);

    for(step=0u;step<FRAME_COUNT*5u;++step) {
        status=tm_ex_pump_source(exec);
        if(status==TM_EX_EOS)break;
        assert(status==TM_EX_OK || status==TM_EX_EFULL);
        assert(tm_ex_pump_sink(exec,0u)==TM_EX_OK);
        assert(tm_ex_pump_sink(exec,1u)==TM_EX_OK);
    }
    assert(step<FRAME_COUNT*5u);
    assert(source.frames_returned==FRAME_COUNT);
    assert(source.packet_count==FRAME_COUNT && source.packets_sent==FRAME_COUNT);
    assert(source.frames_after_flush>=1u);
    assert(source.receive_eagain>=1u);
    assert(source.flush_sends==1u && source.eof==1u);
    for(i=0u;i<3u;++i)assert(visits[i].visits==FRAME_COUNT);
    assert(tm_ex_stats(exec,&stats)==TM_EX_OK);
    assert(stats.sourced==FRAME_COUNT && stats.processed==FRAME_COUNT &&
           stats.submitted==FRAME_COUNT && stats.delivered==2u*FRAME_COUNT &&
           stats.backpressure>=1u && stats.stopped==1u && !stats.pending);
    assert(tm_ex_destroy(exec)==TM_EX_OK);
    assert(tm_fs_seal(store)==TM_FS_OK);
    assert(tm_fs_destroy(store)==TM_FS_OK);
    tm_sg_program_destroy(graph);
    av_frame_free(&source.frame);
    avcodec_free_context(&source.decoder);
    for(i=0u;i<source.packet_count;++i)av_packet_free(&source.packets[i]);
}


/* Owner-stop failure domain: branch credits may already hold a real
 * decoded MPEG-2 frame and the Executor may retain one pending FrameRef.
 * Stop must not re-enter Codec, implicitly flush or publish more frames.
 * Host then explicitly discards each branch's retaining credit. */
static void run_stop_before_delayed_flush(void) {
    delayed_source source={0};
    sink_observer visits[3]={{0}};
    tm_sg_program *graph=make_graph();
    tm_fs_config_v1 fs=TM_FS_CONFIG_V1_INIT;
    tm_fs_store *store=NULL;
    tm_ex_config_v1 cfg=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 binds[4];
    tm_ex_owner *exec=NULL;
    tm_ex_stats_v1 stats=TM_EX_STATS_V1_INIT;
    const char *ids[]={"source","processor","record","preview"};
    unsigned produced,sent,i;
    make_delayed_fixture(&source);
    fs.store_id=190191u;fs.slot_capacity=2u;
    fs.max_frame_bytes=LUMA;fs.max_retained_bytes=2u*LUMA;
    fs.max_leases=8u;fs.max_pins=8u;
    assert(tm_fs_create(&fs,&store)==TM_FS_OK);
    cfg.max_frame_bytes=LUMA;
    cfg.source_session_generation=192u;cfg.display_generation=193u;
    cfg.fanout.branch_count=2u;
    for(i=0u;i<2u;++i) {
        cfg.fanout.branches[i]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
        cfg.fanout.branches[i].policy=TM_FO_ORDERED_LOSSLESS;
        cfg.fanout.branches[i].queue_depth=1u;
    }
    for(i=0u;i<4u;++i) {
        binds[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        binds[i].node_id=ids[i];
        binds[i].ctx=i==0u?(void *)&source:(void *)&visits[i-1u];
        binds[i].visit=i==0u?NULL:observe;
    }
    binds[0].source_next=delayed_next;
    assert(tm_ex_create(graph,store,&cfg,binds,4u,&exec)==TM_EX_OK);
    assert(tm_ex_pump_source(exec)==TM_EX_OK);
    assert(tm_ex_pump_source(exec)==TM_EX_EFULL);
    produced=source.frames_returned;sent=source.packets_sent;
    assert(produced==2u && sent<source.packet_count && !source.flush_sends);
    assert(tm_ex_pump_source(exec)==TM_EX_EFULL);
    assert(source.frames_returned==produced);
    assert(tm_ex_stop(exec)==TM_EX_OK);
    assert(tm_ex_pump_source(exec)==TM_EX_EOS);
    assert(source.frames_returned==produced && source.packets_sent==sent);
    assert(source.flush_sends==0u);
    assert(tm_ex_discard_ready(exec,0u)==TM_EX_OK);
    assert(tm_ex_discard_ready(exec,1u)==TM_EX_OK);
    assert(tm_ex_stats(exec,&stats)==TM_EX_OK);
    assert(stats.stopped==1u);
    assert(tm_ex_destroy(exec)==TM_EX_OK);
    /* Processor has visited both decoded frames already, but the two
     * retaining Sinks saw neither: first is explicitly discarded and the
     * second pending FrameRef is retired by stop without downstream effects. */
    assert(visits[0].visits==2u);
    assert(visits[1].visits==0u && visits[2].visits==0u);
    assert(tm_fs_seal(store)==TM_FS_OK);
    assert(tm_fs_destroy(store)==TM_FS_OK);
    tm_sg_program_destroy(graph);
    av_frame_free(&source.frame);
    avcodec_free_context(&source.decoder);
    for(i=0u;i<source.packet_count;++i)av_packet_free(&source.packets[i]);
}

int main(void) {
    run_delayed_drain();
    run_stop_before_delayed_flush();
    puts("Real MPEG2 B-frame reorder -> bounded StreamGraph drain: PASS");
    return 0;
}
