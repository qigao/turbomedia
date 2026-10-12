#include <turbo_streamgraph_ffmpeg.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>

enum { WIDTH=16, HEIGHT=16, LUMA_BYTES=WIDTH*HEIGHT, PACKET_MAX=65536 };

typedef struct test_visit {
    uint32_t count;
    uint32_t id[4];
    uint8_t expected_luma[4];
    int failures;
} test_visit;

static int check_pixels(void *context,const tm_fs_frame_ref_v1 *ref,
                        const tm_fs_cpu_view_v1 *view) {
    test_visit *v=(test_visit *)context;
    uint32_t i;
    int first;
    if (v->count>=4u || view->size_bytes!=LUMA_BYTES ||
        !view->data || !ref || ref->frame_id==0u) return -1;
    i=v->count++;
    first=(int)view->data[0];
    if (abs(first-(int)v->expected_luma[i])>7) return -1;
    if (view->data[LUMA_BYTES-1u]!=view->data[0]) return -1;
    if (i && ref->frame_id<=v->id[i-1u]) return -1;
    v->id[i]=(uint32_t)ref->frame_id;
    return 0;
}
static tm_sg_port_v1 port(const char *id,uint32_t direction) {
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=id;p.direction=direction;p.kind=TM_SG_VIDEO_FRAME;
    p.memory_domain=TM_SG_MEMORY_CPU;
    p.format_id=301u; /* profile-local LUMA8 resolved before execution */
    p.timebase_num=1u;p.timebase_den=30u;
    return p;
}
static tm_sg_program *graph(void) {
    tm_sg_port_v1 src=port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 proc[2]={ {0},{0} };
    tm_sg_port_v1 sink=port("in",TM_SG_INPUT);
    tm_sg_node_v1 nodes[4];
    tm_sg_edge_v1 edges[3];
    const char *ids[]={"src","decode_visit","record","preview"};
    tm_sg_program *result=NULL;
    uint32_t i;
    proc[0]=port("in",TM_SG_INPUT);
    proc[1]=port("out",TM_SG_OUTPUT);
    for(i=0u;i<4u;++i) {
        nodes[i]=(tm_sg_node_v1)TM_SG_NODE_V1_INIT;
        nodes[i].id=ids[i];
        nodes[i].kind=i==0u?TM_SG_SOURCE:(i==1u?TM_SG_PROCESSOR:TM_SG_SINK);
        nodes[i].port_count=i==1u?2u:1u;
        nodes[i].ports=i==0u?&src:i==1u?proc:&sink;
    }
    for(i=0u;i<3u;++i)edges[i]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
    edges[0].source_node="src";edges[0].source_port="out";
    edges[0].target_node="decode_visit";edges[0].target_port="in";
    for(i=1u;i<3u;++i) {
        edges[i].source_node="decode_visit";edges[i].source_port="out";
        edges[i].target_node=ids[i+1u];edges[i].target_port="in";
    }
    assert(tm_sg_compile(nodes,4u,edges,3u,NULL,&result,NULL)==TM_SG_OK);
    return result;
}
static tm_fs_store *store(void) {
    tm_fs_config_v1 config=TM_FS_CONFIG_V1_INIT;
    tm_fs_store *s=NULL;
    config.store_id=18731u;
    config.slot_capacity=4u;config.max_frame_bytes=LUMA_BYTES;
    config.max_pins=8u;config.max_leases=8u;
    config.max_retained_bytes=4u*LUMA_BYTES;
    assert(tm_fs_create(&config,&s)==TM_FS_OK);
    return s;
}
static tm_ex_owner *executor(tm_sg_program *g,tm_fs_store *s,
                              tm_ff_source *decoder,test_visit *visits) {
    tm_ex_config_v1 cfg=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 binds[4];
    const char *ids[]={"src","decode_visit","record","preview"};
    tm_ex_owner *e=NULL;
    uint32_t i;
    cfg.max_frame_bytes=LUMA_BYTES;
    cfg.source_session_generation=3u;
    cfg.display_generation=5u;
    cfg.fanout.branch_count=2u;
    for(i=0u;i<2u;++i) {
        cfg.fanout.branches[i]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
        cfg.fanout.branches[i].policy=TM_FO_ORDERED_LOSSLESS;
        cfg.fanout.branches[i].queue_depth=1u;
    }
    for(i=0u;i<4u;++i) {
        binds[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        binds[i].node_id=ids[i];
        binds[i].ctx=i==0u?(void *)decoder:(void *)&visits[i];
        binds[i].visit=i==0u?NULL:check_pixels;
    }
    binds[0].source_next=tm_ff_next_cpu_luma;
    assert(tm_ex_create(g,s,&cfg,binds,4u,&e)==TM_EX_OK);
    return e;
}
typedef struct encoder_fixture {
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *packet;
    uint64_t next_pts;
} encoder_fixture;
static void encoder_init(encoder_fixture *f) {
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    assert(codec!=NULL);
    f->ctx=avcodec_alloc_context3(codec);
    assert(f->ctx);
    f->ctx->width=WIDTH;f->ctx->height=HEIGHT;
    f->ctx->time_base=(AVRational){1,30};
    f->ctx->pix_fmt=AV_PIX_FMT_YUVJ420P;
    f->ctx->color_range=AVCOL_RANGE_JPEG;
    f->ctx->thread_count=1;
    assert(avcodec_open2(f->ctx,codec,NULL)==0);
    f->frame=av_frame_alloc();
    f->packet=av_packet_alloc();
    assert(f->frame && f->packet);
    f->frame->format=AV_PIX_FMT_YUVJ420P;
    f->frame->width=WIDTH;f->frame->height=HEIGHT;
    assert(av_frame_get_buffer(f->frame,32)==0);
}
static size_t make_mjpeg(encoder_fixture *f,uint8_t luma,
                          uint8_t *dst,size_t capacity) {
    uint32_t y;
    int rc;
    size_t n;
    assert(av_frame_make_writable(f->frame)==0);
    for(y=0u;y<HEIGHT;++y)
        memset(f->frame->data[0]+(size_t)y*f->frame->linesize[0],luma,WIDTH);
    for(y=0u;y<HEIGHT/2u;++y) {
        memset(f->frame->data[1]+(size_t)y*f->frame->linesize[1],128u,WIDTH/2u);
        memset(f->frame->data[2]+(size_t)y*f->frame->linesize[2],128u,WIDTH/2u);
    }
    f->frame->pts=(int64_t)f->next_pts++;
    rc=avcodec_send_frame(f->ctx,f->frame);
    assert(rc==0);
    rc=avcodec_receive_packet(f->ctx,f->packet);
    assert(rc==0);
    n=(size_t)f->packet->size;
    assert(n && n<=capacity);
    memcpy(dst,f->packet->data,n);
    av_packet_unref(f->packet);
    return n;
}
static void encoder_destroy(encoder_fixture *f) {
    av_frame_free(&f->frame);
    av_packet_free(&f->packet);
    avcodec_free_context(&f->ctx);
}
static void cleanup(tm_ex_owner *e,tm_fs_store *s,tm_ff_source *decode,
                    tm_sg_program *g) {
    assert(tm_ex_destroy(e)==TM_EX_OK);
    assert(tm_ff_destroy(decode)==TM_FF_OK);
    assert(tm_fs_seal(s)==TM_FS_OK);
    assert(tm_fs_destroy(s)==TM_FS_OK);
    tm_sg_program_destroy(g);
}
static void test_real_mjpeg_backpressure_flush(void) {
    tm_ff_config_v1 fcfg=TM_FF_CONFIG_V1_INIT;
    tm_ff_source *d=NULL;
    encoder_fixture enc={0};
    tm_sg_program *g=graph();
    tm_fs_store *s=store();
    tm_ex_owner *e;
    tm_ff_stats_v1 fs=TM_FF_STATS_V1_INIT;
    tm_ff_codec_activity_v1 activity=TM_FF_CODEC_ACTIVITY_V1_INIT;
    tm_ex_stats_v1 es=TM_EX_STATS_V1_INIT;
    test_visit visits[4]={{0}};
    const uint8_t levels[]={48u,96u,160u};
    uint8_t compressed[PACKET_MAX];
    size_t count;
    uint32_t i;
    int ready=0;
    for(i=1u;i<4u;++i) {
        visits[i].expected_luma[0]=levels[0];
        visits[i].expected_luma[1]=levels[1];
        visits[i].expected_luma[2]=levels[2];
    }
    fcfg.width=WIDTH; fcfg.height=HEIGHT;fcfg.max_packet_bytes=PACKET_MAX;
    assert(tm_ff_create(&fcfg,&d)==TM_FF_OK);
    encoder_init(&enc);
    e=executor(g,s,d,visits);

    count=make_mjpeg(&enc,levels[0],compressed,sizeof(compressed));
    assert(tm_ff_submit_mjpeg(d,compressed,count)==TM_FF_OK);
    assert(tm_ff_submit_mjpeg(d,compressed,count)==TM_FF_EFULL);
    assert(tm_ff_ready(d,&ready)==TM_FF_OK && ready);
    assert(tm_ex_pump_source(e)==TM_EX_OK);
    assert(visits[1].count==1u);

    count=make_mjpeg(&enc,levels[1],compressed,sizeof(compressed));
    assert(tm_ff_submit_mjpeg(d,compressed,count)==TM_FF_OK);
    assert(tm_ex_pump_source(e)==TM_EX_EFULL);
    assert(visits[1].count==2u); /* frame 2 is held under owner backpressure */
    assert(tm_ex_pump_source(e)==TM_EX_EFULL);
    assert(visits[1].count==2u); /* no second Provider decode */
    assert(tm_ex_pump_sink(e,0u)==TM_EX_OK);
    assert(tm_ex_pump_sink(e,1u)==TM_EX_OK);
    assert(tm_ex_pump_source(e)==TM_EX_OK); /* release pending frame 2 */

    count=make_mjpeg(&enc,levels[2],compressed,sizeof(compressed));
    assert(tm_ff_submit_mjpeg(d,compressed,count)==TM_FF_OK);
    assert(tm_ex_pump_source(e)==TM_EX_EFULL);
    assert(visits[1].count==3u);
    assert(tm_ex_pump_sink(e,0u)==TM_EX_OK);
    assert(tm_ex_pump_sink(e,1u)==TM_EX_OK);
    assert(tm_ex_pump_source(e)==TM_EX_OK);
    assert(tm_ex_pump_sink(e,0u)==TM_EX_OK);
    assert(tm_ex_pump_sink(e,1u)==TM_EX_OK);

    assert(tm_ff_codec_activity(d,&activity)==TM_FF_OK);
    assert(activity.in_call==0u && activity.packet_calls==3u && activity.flush_calls==0u);
    assert(tm_ff_finish(d)==TM_FF_OK);
    assert(tm_ex_pump_source(e)==TM_EX_EOS);
    assert(tm_ff_codec_activity(d,&activity)==TM_FF_OK);
    assert(activity.in_call==0u && activity.packet_calls==3u && activity.flush_calls==1u);
    assert(tm_ex_stats(e,&es)==TM_EX_OK);
    assert(es.sourced==3u && es.processed==3u &&
           es.submitted==3u && es.delivered==6u && es.backpressure>=3u);
    assert(tm_ff_stats(d,&fs)==TM_FF_OK);
    assert(fs.packets_submitted==3u && fs.frames_decoded==3u &&
           fs.bytes_decoded==3u*LUMA_BYTES && fs.eof);
    for(i=1u;i<4u;++i)assert(visits[i].count==3u);
    encoder_destroy(&enc);
    cleanup(e,s,d,g);
}
static void test_explicit_cancel_and_bounds(void) {
    tm_ff_config_v1 cfg=TM_FF_CONFIG_V1_INIT;
    tm_ff_source *d=NULL;
    encoder_fixture encoder={0};
    tm_sg_program *g=graph();
    tm_fs_store *s=store();
    tm_ex_owner *e;
    tm_ff_stats_v1 stats=TM_FF_STATS_V1_INIT;
    test_visit visits[4]={{0}};
    uint8_t encoded[PACKET_MAX];
    size_t n;
    cfg.width=WIDTH;cfg.height=HEIGHT;cfg.max_packet_bytes=PACKET_MAX;
    assert(tm_ff_create(&cfg,&d)==TM_FF_OK);
    encoder_init(&encoder);
    e=executor(g,s,d,visits);
    n=make_mjpeg(&encoder,77u,encoded,sizeof(encoded));
    assert(tm_ff_submit_mjpeg(d,encoded,n)==TM_FF_OK);
    assert(tm_ff_submit_mjpeg(d,encoded,PACKET_MAX+1u)==TM_FF_EFULL);
    assert(tm_ff_cancel(d)==TM_FF_OK);
    assert(tm_ff_submit_mjpeg(d,encoded,n)==TM_FF_ESTATE);
    assert(tm_ex_stop(e)==TM_EX_OK);
    assert(tm_ex_discard_ready(e,0u)==TM_EX_OK);
    assert(tm_ex_discard_ready(e,1u)==TM_EX_OK);
    assert(tm_ff_stats(d,&stats)==TM_FF_OK);
    assert(stats.cancelled==1u && stats.frames_decoded==0u);
    encoder_destroy(&encoder);
    cleanup(e,s,d,g);
}
int main(void) {
    test_real_mjpeg_backpressure_flush();
    test_explicit_cancel_and_bounds();
    puts("Native FFmpeg MJPEG decode -> StreamGraph FrameStore/fanout: PASS");
    return 0;
}
