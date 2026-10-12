#include <turbo_streamgraph_ffmpeg_async.h>
#include <turbo_streamgraph_owner_dispatch.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>

enum { WIDTH=16, HEIGHT=16, LUMA=WIDTH*HEIGHT, MAX_PACKET=65536 };
typedef struct observed {
    uint8_t levels[3];
    uint64_t last_id;
    unsigned count;
} observed;
static int visit(void *ctx,const tm_fs_frame_ref_v1 *ref,
                 const tm_fs_cpu_view_v1 *view) {
    observed *o=(observed *)ctx;
    unsigned i=o->count;
    if(i>=3u || !view || !ref || view->size_bytes!=LUMA ||
       ref->frame_id<=o->last_id || !view->data ||
       abs((int)view->data[0]-(int)o->levels[i])>7 ||
       view->data[LUMA-1u]!=view->data[0]) return -1;
    o->last_id=ref->frame_id;
    ++o->count;
    return 0;
}
static tm_sg_port_v1 port(const char *name,uint32_t dir) {
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=name; p.direction=dir;
    p.kind=TM_SG_VIDEO_FRAME; p.memory_domain=TM_SG_MEMORY_CPU;
    p.format_id=301u; p.timebase_num=1u;p.timebase_den=30u;
    return p;
}
static tm_sg_program *graph(void) {
    tm_sg_node_v1 nodes[4];
    tm_sg_edge_v1 edges[3];
    tm_sg_port_v1 src=port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 middle[2]={ {0},{0} };
    tm_sg_port_v1 sink=port("in",TM_SG_INPUT);
    const char *names[]={"source","processor","record","preview"};
    tm_sg_program *g=NULL;
    uint32_t i;
    middle[0]=port("in",TM_SG_INPUT);
    middle[1]=port("out",TM_SG_OUTPUT);
    for(i=0u;i<4u;++i) {
        nodes[i]=(tm_sg_node_v1)TM_SG_NODE_V1_INIT;
        nodes[i].id=names[i];
        nodes[i].kind=i==0u?TM_SG_SOURCE:(i==1u?TM_SG_PROCESSOR:TM_SG_SINK);
        nodes[i].port_count=i==1u?2u:1u;
        nodes[i].ports=i==0u?&src:(i==1u?middle:&sink);
    }
    for(i=0u;i<3u;++i)edges[i]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
    edges[0].source_node="source";edges[0].source_port="out";
    edges[0].target_node="processor";edges[0].target_port="in";
    for(i=1u;i<3u;++i) {
        edges[i].source_node="processor";edges[i].source_port="out";
        edges[i].target_node=names[i+1u];edges[i].target_port="in";
    }
    assert(tm_sg_compile(nodes,4u,edges,3u,NULL,&g,NULL)==TM_SG_OK);
    return g;
}
static tm_fs_store *store(void) {
    tm_fs_config_v1 c=TM_FS_CONFIG_V1_INIT;
    tm_fs_store *s=NULL;
    c.store_id=21212u;
    c.slot_capacity=4u; c.max_frame_bytes=LUMA;
    c.max_pins=8u;c.max_leases=8u;c.max_retained_bytes=4u*LUMA;
    assert(tm_fs_create(&c,&s)==TM_FS_OK);
    return s;
}
static tm_ex_owner *executor(tm_sg_program *g,tm_fs_store *s,
                              tm_ffa_decoder *d,observed *visits) {
    tm_ex_config_v1 c=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 b[4];
    const char *names[]={"source","processor","record","preview"};
    tm_ex_owner *ex=NULL;
    uint32_t i;
    c.source_session_generation=4u;c.display_generation=8u;
    c.max_frame_bytes=LUMA;
    c.fanout.branch_count=2u;
    for(i=0u;i<2u;++i) {
        c.fanout.branches[i]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
        c.fanout.branches[i].policy=TM_FO_ORDERED_LOSSLESS;
        c.fanout.branches[i].queue_depth=1u;
    }
    for(i=0u;i<4u;++i) {
        b[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        b[i].node_id=names[i];
        b[i].ctx=i==0u?(void *)d:(void *)&visits[i-1u];
        b[i].visit=i==0u?NULL:visit;
    }
    b[0].source_next=tm_ffa_owner_next_luma;
    assert(tm_ex_create(g,s,&c,b,4u,&ex)==TM_EX_OK);
    return ex;
}
typedef struct codec_fixture {
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *packet;
    uint64_t pts;
} codec_fixture;
static void encoder_init(codec_fixture *e) {
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    assert(codec!=NULL);
    e->ctx=avcodec_alloc_context3(codec);
    assert(e->ctx);
    e->ctx->width=WIDTH;e->ctx->height=HEIGHT;
    e->ctx->time_base=(AVRational){1,30};
    e->ctx->pix_fmt=AV_PIX_FMT_YUVJ420P;
    e->ctx->color_range=AVCOL_RANGE_JPEG;
    e->ctx->thread_count=1;
    assert(avcodec_open2(e->ctx,codec,NULL)==0);
    e->frame=av_frame_alloc();e->packet=av_packet_alloc();
    assert(e->frame && e->packet);
    e->frame->format=AV_PIX_FMT_YUVJ420P;
    e->frame->width=WIDTH;e->frame->height=HEIGHT;
    assert(av_frame_get_buffer(e->frame,32)==0);
}
static size_t make_packet(codec_fixture *e,uint8_t luma,
                          uint8_t *out,size_t capacity) {
    uint32_t y;
    size_t n;
    assert(av_frame_make_writable(e->frame)==0);
    for(y=0u;y<HEIGHT;++y)
        memset(e->frame->data[0]+(size_t)y*e->frame->linesize[0],luma,WIDTH);
    for(y=0u;y<HEIGHT/2u;++y) {
        memset(e->frame->data[1]+(size_t)y*e->frame->linesize[1],128u,WIDTH/2u);
        memset(e->frame->data[2]+(size_t)y*e->frame->linesize[2],128u,WIDTH/2u);
    }
    e->frame->pts=(int64_t)e->pts++;
    assert(avcodec_send_frame(e->ctx,e->frame)==0);
    assert(avcodec_receive_packet(e->ctx,e->packet)==0);
    n=(size_t)e->packet->size;
    assert(n>0u && n<=capacity);
    memcpy(out,e->packet->data,n);
    av_packet_unref(e->packet);
    return n;
}
static void encoder_destroy(codec_fixture *e) {
    av_frame_free(&e->frame);av_packet_free(&e->packet);
    avcodec_free_context(&e->ctx);
}
typedef struct worker {
    tm_ffa_decoder *decode;
    tm_pc_portal *portal;
    tm_pc_ticket_v1 ticket;
    uint8_t compressed[MAX_PACKET];
    size_t count;
    _Atomic unsigned woke;
    _Atomic unsigned release;
    int wake_status;
    tm_ff_status result;
} worker;
static int wake_callback(void *context) {
    worker *w=(worker *)context;
    atomic_store_explicit(&w->woke,1u,memory_order_release);
    while(!atomic_load_explicit(&w->release,memory_order_acquire))
        sched_yield();
    return w->wake_status;
}
static void *worker_run(void *context) {
    worker *w=(worker *)context;
    w->result=tm_ffa_run_mjpeg(w->decode,w->compressed,w->count,
                              w->portal,w->ticket,wake_callback,w);
    return NULL;
}
static void await_wake(worker *w) {
    unsigned tries=0u;
    while(!atomic_load_explicit(&w->woke,memory_order_acquire)) {
        assert(++tries<100000000u);
        sched_yield();
    }
}
typedef struct terminal_delivery {
    tm_ffa_decoder *decoder;
    tm_ex_owner *executor;
    tm_od_owner *owner;
    tm_pc_ticket_v1 ticket;
    uint64_t correlation;
    tm_ex_status expected_pump;
    uint32_t delivered;
    uint32_t reject_once,refused;
} terminal_delivery;
static int deliver_to_graph(void *ctx,const tm_pc_result_v1 *terminal) {
    terminal_delivery *o=(terminal_delivery *)ctx;
    tm_pc_result_v1 recursive=TM_PC_RESULT_V1_INIT;
    tm_ex_status pumped;
    assert(terminal->correlation==o->correlation);
    assert(terminal->provider_generation==44u);
    assert(terminal->outcome==TM_PC_COMPLETED);
    assert(terminal->completion_observed==1u);
    assert(terminal->provider_status==TM_FF_OK);
    assert(tm_od_step_completion(o->owner,&recursive)==TM_OD_EBUSY);
    if(o->reject_once) {
        --o->reject_once;
        ++o->refused;
        return -1; /* Portal TAKEN + luma buffer both remain unresolved. */
    }
    assert(tm_ffa_owner_accept(o->decoder,o->ticket)==TM_FF_OK);
    pumped=tm_ex_pump_source(o->executor);
    assert(pumped==o->expected_pump);
    ++o->delivered;
    /* Successful delivery means the Executor either owns the new FrameRef
     * or its explicit bounded pending frame. No codec redo is authorized. */
    return 0;
}
static void test_real_async_provider_and_credits(void) {
    const uint8_t levels[]={47u,95u,161u};
    tm_ff_config_v1 cfg=TM_FF_CONFIG_V1_INIT;
    tm_ffa_decoder *decoder=NULL;
    tm_pc_config_v1 pc=TM_PC_CONFIG_V1_INIT;
    tm_pc_portal *portal=NULL;
    tm_ho_inbox *inbox=NULL;
    tm_od_owner *owner=NULL;
    tm_ho_config_v1 hcfg=TM_HO_CONFIG_V1_INIT;
    tm_od_config_v1 ocfg=TM_OD_CONFIG_V1_INIT;
    terminal_delivery delivery={0};
    tm_sg_program *g=graph();
    tm_fs_store *s=store();
    tm_ex_owner *ex;
    codec_fixture encoder={0};
    observed visits[3]={0};
    tm_ffa_stats_v1 fstats=TM_FFA_STATS_V1_INIT;
    tm_ex_stats_v1 estats=TM_EX_STATS_V1_INIT;
    uint32_t i;
    cfg.width=WIDTH;cfg.height=HEIGHT;cfg.max_packet_bytes=MAX_PACKET;
    assert(tm_ffa_create(&cfg,&decoder)==TM_FF_OK);
    pc.capacity=3u;pc.provider_generation=44u;
    assert(tm_pc_create(&pc,&portal)==TM_PC_OK);
    for(i=0u;i<3u;++i)memcpy(visits[i].levels,levels,sizeof(levels));
    encoder_init(&encoder);
    ex=executor(g,s,decoder,visits);
    hcfg.capacity=2u;
    assert(tm_ho_create(&hcfg,&inbox)==TM_HO_OK);
    assert(tm_od_create(inbox,ex,s,&ocfg,&owner)==TM_OD_OK);
    delivery.decoder=decoder;
    delivery.executor=ex;
    delivery.owner=owner;
    assert(tm_od_bind_completion(owner,portal,deliver_to_graph,&delivery)==TM_OD_OK);
    for(i=0u;i<3u;++i) {
        tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
        tm_pc_ticket_v1 wrong;
        worker w={0};
        pthread_t thread;
        unsigned j;
        w.decode=decoder;w.portal=portal;
        w.count=make_packet(&encoder,levels[i],w.compressed,sizeof(w.compressed));
        w.wake_status=i==2u?-17:0; /* a dropped wake cannot lose a terminal */
        atomic_init(&w.woke,0u);atomic_init(&w.release,0u);
        assert(tm_pc_begin(portal,900u+i,&w.ticket)==TM_PC_OK);
        assert(pthread_create(&thread,NULL,worker_run,&w)==0);
        await_wake(&w);
        assert(tm_ffa_destroy(decoder)==TM_FF_ESTATE);
        delivery.ticket=w.ticket;
        delivery.correlation=900u+i;
        delivery.expected_pump=i==1u?TM_EX_EFULL:TM_EX_OK;
        wrong=w.ticket;wrong.generation++;
        assert(tm_ffa_owner_accept(decoder,wrong)==TM_FF_ESTALE);
        assert(tm_od_step_completion(owner,&result)==TM_OD_OK);
        assert(result.correlation==900u+i &&
               result.provider_generation==44u &&
               result.outcome==TM_PC_COMPLETED &&
               result.provider_status==TM_FF_OK);
        assert(delivery.delivered==i+1u);
        assert(tm_od_step_completion(owner,&result)==TM_OD_EMPTY);
        assert(visits[0].count==i+1u);
        /* Source callback has copied a released pixel buffer once; another
         * source pump under backpressure cannot re-decode packet #2. */
        if(i==1u) {
            assert(tm_ex_pump_source(ex)==TM_EX_EFULL);
            assert(visits[0].count==2u);
        }
        assert(tm_pc_settle(portal,w.ticket)==TM_PC_ESTALE);
        atomic_store_explicit(&w.release,1u,memory_order_release);
        assert(pthread_join(thread,NULL)==0);
        assert(w.result==TM_FF_OK);
        if(i==1u) {
            /* #1 retires first; #2 then leaves the Executor's pending
             * FrameRef and is submitted exactly once after credits free. */
            for(j=0u;j<2u;++j)assert(tm_ex_pump_sink(ex,j)==TM_EX_OK);
            assert(tm_ex_pump_source(ex)==TM_EX_OK);
            for(j=0u;j<2u;++j)assert(tm_ex_pump_sink(ex,j)==TM_EX_OK);
        }
    }
    for(i=0u;i<2u;++i)assert(tm_ex_pump_sink(ex,i)==TM_EX_OK);
    assert(tm_ex_stats(ex,&estats)==TM_EX_OK);
    assert(estats.sourced==3u && estats.processed==3u &&
           estats.submitted==3u && estats.delivered==6u &&
           estats.backpressure>=1u);
    for(i=0u;i<3u;++i)assert(visits[i].count==3u);
    assert(tm_ffa_stats(decoder,&fstats)==TM_FF_OK);
    assert(fstats.decoded==3u && fstats.delivered==3u &&
           fstats.discarded==0u && fstats.notifications_failed==1u &&
           fstats.worker_active==0u && fstats.state==0u);
    assert(tm_ho_seal(inbox)==TM_HO_OK);
    assert(tm_pc_seal(portal)==TM_PC_OK);
    assert(tm_od_destroy(owner)==TM_OD_OK);
    assert(tm_ho_destroy(inbox)==TM_HO_OK);
    assert(tm_ex_stop(ex)==TM_EX_OK);
    assert(tm_ex_destroy(ex)==TM_EX_OK);
    assert(tm_pc_destroy(portal)==TM_PC_OK);
    assert(tm_ffa_destroy(decoder)==TM_FF_OK);
    encoder_destroy(&encoder);
    assert(tm_fs_seal(s)==TM_FS_OK);
    assert(tm_fs_destroy(s)==TM_FS_OK);
    tm_sg_program_destroy(g);
}
/* Completion wins before a late cancel: the Portal result remains
 * COMPLETED. The owner may explicitly discard the ready CPU buffer, but
 * may not forge a CANCELLED terminal or unload across a wake tail. */
static void test_complete_then_explicit_discard(void) {
    tm_ff_config_v1 cfg=TM_FF_CONFIG_V1_INIT;
    tm_pc_config_v1 pc=TM_PC_CONFIG_V1_INIT;
    tm_ffa_decoder *decoder=NULL;
    tm_pc_portal *portal=NULL;
    tm_pc_ticket_v1 ticket={0},taken={0};
    tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
    tm_ffa_stats_v1 stats=TM_FFA_STATS_V1_INIT;
    codec_fixture encoder={0};
    worker w={0};
    pthread_t thread;
    cfg.width=WIDTH;cfg.height=HEIGHT;cfg.max_packet_bytes=MAX_PACKET;
    pc.capacity=1u;pc.provider_generation=46u;
    assert(tm_ffa_create(&cfg,&decoder)==TM_FF_OK);
    assert(tm_pc_create(&pc,&portal)==TM_PC_OK);
    encoder_init(&encoder);
    w.decode=decoder;w.portal=portal;
    w.count=make_packet(&encoder,83u,w.compressed,sizeof(w.compressed));
    atomic_init(&w.woke,0u);
    atomic_init(&w.release,0u);
    assert(tm_pc_begin(portal,1002u,&ticket)==TM_PC_OK);
    w.ticket=ticket;
    assert(pthread_create(&thread,NULL,worker_run,&w)==0);
    await_wake(&w);
    assert(tm_pc_request_cancel(portal,ticket)==TM_PC_EALREADY);
    assert(tm_ffa_owner_cancel(decoder)==TM_FF_OK);
    assert(tm_ffa_owner_accept(decoder,ticket)==TM_FF_ESTALE);
    assert(tm_pc_take(portal,&taken,&result)==TM_PC_OK);
    assert(result.outcome==TM_PC_COMPLETED &&
           result.completion_observed==1u &&
           result.provider_status==TM_FF_OK);
    assert(tm_ffa_stats(decoder,&stats)==TM_FF_OK);
    assert(stats.decoded==1u && stats.discarded==1u &&
           stats.delivered==0u && stats.worker_active);
    assert(tm_ffa_destroy(decoder)==TM_FF_ESTATE);
    assert(tm_pc_settle(portal,taken)==TM_PC_OK);
    atomic_store_explicit(&w.release,1u,memory_order_release);
    assert(pthread_join(thread,NULL)==0);
    assert(w.result==TM_FF_OK);
    assert(tm_pc_seal(portal)==TM_PC_OK);
    assert(tm_pc_destroy(portal)==TM_PC_OK);
    assert(tm_ffa_destroy(decoder)==TM_FF_OK);
    encoder_destroy(&encoder);
}
static void test_rejected_terminal_keeps_real_decoded_credit(void) {
    tm_ff_config_v1 cfg=TM_FF_CONFIG_V1_INIT;
    tm_pc_config_v1 pc=TM_PC_CONFIG_V1_INIT;
    tm_ho_config_v1 hc=TM_HO_CONFIG_V1_INIT;
    tm_od_config_v1 oc=TM_OD_CONFIG_V1_INIT;
    tm_ffa_decoder *decoder=NULL;
    tm_pc_portal *portal=NULL;
    tm_ho_inbox *inbox=NULL;
    tm_od_owner *owner=NULL;
    tm_sg_program *g=graph();
    tm_fs_store *s=store();
    tm_ex_owner *ex;
    codec_fixture encoder={0};
    observed visits[3]={0};
    terminal_delivery delivery={0};
    tm_ffa_stats_v1 stats=TM_FFA_STATS_V1_INIT;
    tm_pc_snapshot_v1 snapshot=TM_PC_SNAPSHOT_V1_INIT;
    tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
    worker w={0};
    pthread_t thread;
    cfg.width=WIDTH;cfg.height=HEIGHT;cfg.max_packet_bytes=MAX_PACKET;
    pc.capacity=1u;pc.provider_generation=44u;
    hc.capacity=1u;
    assert(tm_ffa_create(&cfg,&decoder)==TM_FF_OK);
    assert(tm_pc_create(&pc,&portal)==TM_PC_OK);
    encoder_init(&encoder);
    ex=executor(g,s,decoder,visits);
    assert(tm_ho_create(&hc,&inbox)==TM_HO_OK);
    assert(tm_od_create(inbox,ex,s,&oc,&owner)==TM_OD_OK);
    delivery.owner=owner;delivery.executor=ex;delivery.decoder=decoder;
    delivery.reject_once=1u;delivery.correlation=2001u;
    delivery.expected_pump=TM_EX_OK;
    assert(tm_od_bind_completion(owner,portal,deliver_to_graph,&delivery)==TM_OD_OK);
    w.decode=decoder;w.portal=portal;
    w.count=make_packet(&encoder,123u,w.compressed,sizeof(w.compressed));
    atomic_init(&w.woke,0u);atomic_init(&w.release,0u);
    assert(tm_pc_begin(portal,2001u,&w.ticket)==TM_PC_OK);
    delivery.ticket=w.ticket;
    assert(pthread_create(&thread,NULL,worker_run,&w)==0);
    await_wake(&w);
    assert(tm_od_step_completion(owner,&result)==TM_OD_EDELIVERY);
    assert(result.outcome==TM_PC_COMPLETED && result.correlation==2001u);
    assert(delivery.refused==1u && delivery.delivered==0u);
    assert(tm_od_pending_completion(owner,&result)==TM_OD_OK);
    assert(tm_od_step_completion(owner,&result)==TM_OD_EBUSY);
    assert(delivery.refused==1u); /* Strictly no automatic re-emission. */
    assert(tm_od_destroy(owner)==TM_OD_EBUSY);
    assert(tm_pc_snapshot(portal,&snapshot)==TM_PC_OK);
    assert(snapshot.taken==1u && !snapshot.drained);
    assert(tm_ffa_stats(decoder,&stats)==TM_FF_OK);
    assert(stats.state==2u && stats.decoded==1u && stats.delivered==0u);
    assert(tm_ffa_destroy(decoder)==TM_FF_ESTATE);
    /* Host explicitly reconciles the unknown downstream acceptance and
     * discards the frame; no FFmpeg decoder retry or duplicate graph effect. */
    assert(tm_ffa_owner_cancel(decoder)==TM_FF_OK);
    assert(tm_od_settle_pending_completion(owner)==TM_OD_OK);
    assert(tm_od_settle_pending_completion(owner)==TM_OD_EMPTY);
    atomic_store_explicit(&w.release,1u,memory_order_release);
    assert(pthread_join(thread,NULL)==0);
    assert(w.result==TM_FF_OK);
    assert(tm_ffa_stats(decoder,&stats)==TM_FF_OK);
    assert(stats.discarded==1u && stats.delivered==0u &&
           stats.worker_active==0u);
    assert(tm_pc_seal(portal)==TM_PC_OK);
    assert(tm_ho_seal(inbox)==TM_HO_OK);
    assert(tm_od_destroy(owner)==TM_OD_OK);
    assert(tm_ho_destroy(inbox)==TM_HO_OK);
    assert(tm_ex_stop(ex)==TM_EX_OK);
    assert(tm_ex_destroy(ex)==TM_EX_OK);
    assert(tm_pc_destroy(portal)==TM_PC_OK);
    assert(tm_ffa_destroy(decoder)==TM_FF_OK);
    encoder_destroy(&encoder);
    assert(tm_fs_seal(s)==TM_FS_OK);
    assert(tm_fs_destroy(s)==TM_FS_OK);
    tm_sg_program_destroy(g);
}
static void test_cancel_before_worker_admission(void) {
    tm_ff_config_v1 cfg=TM_FF_CONFIG_V1_INIT;
    tm_pc_config_v1 pc=TM_PC_CONFIG_V1_INIT;
    tm_ffa_decoder *decoder=NULL;
    tm_pc_portal *portal=NULL;
    tm_pc_ticket_v1 ticket={0},taken={0};
    tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
    tm_ffa_stats_v1 stats=TM_FFA_STATS_V1_INIT;
    cfg.width=WIDTH;cfg.height=HEIGHT;cfg.max_packet_bytes=MAX_PACKET;
    pc.capacity=1u;pc.provider_generation=45u;
    assert(tm_ffa_create(&cfg,&decoder)==TM_FF_OK);
    assert(tm_pc_create(&pc,&portal)==TM_PC_OK);
    assert(tm_pc_begin(portal,1001u,&ticket)==TM_PC_OK);
    assert(tm_pc_request_cancel(portal,ticket)==TM_PC_OK);
    assert(tm_ffa_owner_cancel(decoder)==TM_FF_OK);
    /* Host proves no worker was ever launched; cancel_quiesced is valid. */
    assert(tm_pc_cancel_quiesced(portal,ticket)==TM_PC_OK);
    assert(tm_pc_take(portal,&taken,&result)==TM_PC_OK);
    assert(result.outcome==TM_PC_CANCELLED &&
           result.completion_observed==0u);
    assert(tm_pc_settle(portal,taken)==TM_PC_OK);
    assert(tm_ffa_stats(decoder,&stats)==TM_FF_OK);
    assert(stats.cancelled && !stats.decoded && !stats.delivered);
    assert(tm_pc_seal(portal)==TM_PC_OK);
    assert(tm_pc_destroy(portal)==TM_PC_OK);
    assert(tm_ffa_destroy(decoder)==TM_FF_OK);
}

/* M15a foundation: independent Codec state and cancellation per instance.
 * Both real FFmpeg decodes have entered their wake tails concurrently.
 * Cancel A while B remains publishable. No extra scheduler is introduced. */
static void test_two_independent_active_decoders(void) {
    tm_ff_config_v1 cfg=TM_FF_CONFIG_V1_INIT;
    tm_pc_config_v1 pc=TM_PC_CONFIG_V1_INIT;
    tm_ffa_decoder *a=NULL,*b=NULL;
    tm_pc_portal *pa=NULL,*pb=NULL;
    tm_pc_ticket_v1 ta={0},tb={0},taken={0};
    tm_pc_result_v1 ra=TM_PC_RESULT_V1_INIT;
    tm_pc_result_v1 rb=TM_PC_RESULT_V1_INIT;
    tm_ffa_stats_v1 sa=TM_FFA_STATS_V1_INIT;
    tm_ffa_stats_v1 sb=TM_FFA_STATS_V1_INIT;
    codec_fixture enc={0};
    worker wa={0},wb={0};
    pthread_t thread_a,thread_b;
    uint8_t luma[LUMA];
    size_t n=0u;
    cfg.width=WIDTH;cfg.height=HEIGHT;cfg.max_packet_bytes=MAX_PACKET;
    assert(tm_ffa_create(&cfg,&a)==TM_FF_OK);
    assert(tm_ffa_create(&cfg,&b)==TM_FF_OK);
    assert(a!=b);
    pc.capacity=1u;pc.provider_generation=71u;
    assert(tm_pc_create(&pc,&pa)==TM_PC_OK);
    pc.provider_generation=71u;
    assert(tm_pc_create(&pc,&pb)==TM_PC_OK);
    encoder_init(&enc);
    wa.decode=a;wa.portal=pa;
    wb.decode=b;wb.portal=pb;
    wa.count=make_packet(&enc,41u,wa.compressed,sizeof(wa.compressed));
    wb.count=make_packet(&enc,183u,wb.compressed,sizeof(wb.compressed));
    atomic_init(&wa.woke,0u);atomic_init(&wa.release,0u);
    atomic_init(&wb.woke,0u);atomic_init(&wb.release,0u);
    assert(tm_pc_begin(pa,7101u,&ta)==TM_PC_OK);
    assert(tm_pc_begin(pb,7201u,&tb)==TM_PC_OK);
    /* Same Provider generation, but Portal incarnation still fences slots. */
    assert(ta.incarnation!=tb.incarnation);
    assert(ta.generation==tb.generation && ta.slot==tb.slot);
    wa.ticket=ta;wb.ticket=tb;
    assert(pthread_create(&thread_a,NULL,worker_run,&wa)==0);
    assert(pthread_create(&thread_b,NULL,worker_run,&wb)==0);
    await_wake(&wa);await_wake(&wb);
    assert(tm_ffa_destroy(a)==TM_FF_ESTATE);
    assert(tm_ffa_destroy(b)==TM_FF_ESTATE);
    /* A has already completed; a late owner cancel discards A's slot
     * without changing its recorded completion or B's independent state. */
    assert(tm_ffa_owner_cancel(a)==TM_FF_OK);
    assert(tm_ffa_owner_accept(a,ta)==TM_FF_ESTALE);
    assert(tm_ffa_owner_accept(a,tb)==TM_FF_ESTALE);
    assert(tm_ffa_owner_accept(b,ta)==TM_FF_ESTALE);
    assert(tm_ffa_owner_accept(b,tb)==TM_FF_OK);
    assert(tm_ffa_owner_next_luma(b,luma,sizeof(luma),&n)==TM_EX_SOURCE_FRAME);
    assert(n==LUMA);
    assert(abs((int)luma[0]-183)<=7);
    assert(luma[0]==luma[LUMA-1u]);
    assert(tm_ffa_owner_next_luma(b,luma,sizeof(luma),&n)==TM_EX_SOURCE_FAILED);
    assert(tm_pc_take(pa,&taken,&ra)==TM_PC_OK);
    assert(ra.correlation==7101u && ra.provider_generation==71u &&
           ra.outcome==TM_PC_COMPLETED);
    assert(tm_pc_settle(pa,taken)==TM_PC_OK);
    assert(tm_pc_take(pb,&taken,&rb)==TM_PC_OK);
    assert(rb.correlation==7201u && rb.provider_generation==71u &&
           rb.outcome==TM_PC_COMPLETED);
    assert(tm_pc_settle(pb,taken)==TM_PC_OK);
    assert(tm_ffa_stats(a,&sa)==TM_FF_OK);
    assert(tm_ffa_stats(b,&sb)==TM_FF_OK);
    assert(sa.decoded==1u && sa.discarded==1u && sa.delivered==0u);
    assert(sb.decoded==1u && sb.discarded==0u && sb.delivered==1u);
    assert(sa.worker_active && sb.worker_active);
    atomic_store_explicit(&wa.release,1u,memory_order_release);
    atomic_store_explicit(&wb.release,1u,memory_order_release);
    assert(pthread_join(thread_a,NULL)==0);
    assert(pthread_join(thread_b,NULL)==0);
    assert(wa.result==TM_FF_OK && wb.result==TM_FF_OK);
    assert(tm_pc_seal(pa)==TM_PC_OK);
    assert(tm_pc_seal(pb)==TM_PC_OK);
    assert(tm_pc_destroy(pa)==TM_PC_OK);
    assert(tm_pc_destroy(pb)==TM_PC_OK);
    assert(tm_ffa_destroy(a)==TM_FF_OK);
    assert(tm_ffa_destroy(b)==TM_FF_OK);
    encoder_destroy(&enc);
}

int main(void) {
    test_two_independent_active_decoders();
    test_real_async_provider_and_credits();
    test_cancel_before_worker_admission();
    test_complete_then_explicit_discard();
    test_rejected_terminal_keeps_real_decoded_credit();
    puts("Real FFmpeg async worker -> typed Portal -> Media Owner FrameStore: PASS");
    return 0;
}
