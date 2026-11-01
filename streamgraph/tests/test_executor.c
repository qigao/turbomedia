#include <turbo_streamgraph_executor.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned next,calls,processed,sink_calls[2];
    unsigned char output[2][10];
    int fail_source,fail_sink;
    tm_ex_owner *executor;
} fixture;
static tm_ex_source_result source(void *ctx,uint8_t *dst,size_t cap,size_t *n){
    fixture *f=(fixture *)ctx;f->calls++;
    /* The owner guard must reject reentrancy from arbitrary Source callbacks. */
    if(f->executor) assert(tm_ex_pump_source(f->executor)==TM_EX_EINVAL);
    if(f->fail_source || cap<1u)return TM_EX_SOURCE_FAILED;
    if(f->next>=3u)return TM_EX_SOURCE_EOS;
    dst[0]=(uint8_t)++f->next;*n=1u;return TM_EX_SOURCE_FRAME;
}
static int process(void *ctx,const tm_fs_frame_ref_v1 *ref,
                   const tm_fs_cpu_view_v1 *view){
    fixture *f=(fixture *)ctx;
    assert(view->size_bytes==1u && *view->data==ref->frame_id);
    f->processed++;return 0;
}
static int record(void *ctx,const tm_fs_frame_ref_v1 *ref,
                  const tm_fs_cpu_view_v1 *view){
    fixture *f=(fixture *)ctx;
    if(f->fail_sink)return -1;
    assert(view->size_bytes==1u && *view->data==ref->frame_id);
    f->output[0][f->sink_calls[0]++]=*view->data;return 0;
}
static int preview(void *ctx,const tm_fs_frame_ref_v1 *ref,
                   const tm_fs_cpu_view_v1 *view){
    fixture *f=(fixture *)ctx;
    assert(view->size_bytes==1u && *view->data==ref->frame_id);
    f->output[1][f->sink_calls[1]++]=*view->data;return 0;
}
static tm_sg_port_v1 port(const char *name,uint32_t direction){
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=name;p.direction=direction;p.kind=TM_SG_VIDEO_FRAME;
    p.memory_domain=TM_SG_MEMORY_CPU;p.format_id=1u;
    p.timebase_num=1u;p.timebase_den=30u;return p;
}
static tm_sg_program *make_program(void){
    tm_sg_node_v1 n[4];
    tm_sg_edge_v1 e[3];
    tm_sg_port_v1 source_port=port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 processor_ports[2]={port("in",TM_SG_INPUT),port("out",TM_SG_OUTPUT)};
    tm_sg_port_v1 sink_port=port("in",TM_SG_INPUT);
    tm_sg_program *p=NULL;
    const char *ids[4]={"src","proc","record","preview"};
    int i;
    for(i=0;i<4;++i){
        n[i]=(tm_sg_node_v1)TM_SG_NODE_V1_INIT;
        n[i].id=ids[i];
        n[i].kind=i==0?TM_SG_SOURCE:i==1?TM_SG_PROCESSOR:TM_SG_SINK;
        n[i].port_count=i==1?2u:1u;
        n[i].ports=i==0?&source_port:i==1?processor_ports:&sink_port;
    }
    e[0]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
    e[0].source_node="src";e[0].source_port="out";
    e[0].target_node="proc";e[0].target_port="in";
    for(i=1;i<3;++i){
        e[i]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
        e[i].source_node="proc";e[i].source_port="out";
        e[i].target_node=ids[i+1];e[i].target_port="in";
    }
    assert(tm_sg_compile(n,4u,e,3u,NULL,&p,NULL)==TM_SG_OK);
    return p;
}
static tm_fs_store *make_store(uint32_t slots){
    tm_fs_store *s=NULL;tm_fs_config_v1 c=TM_FS_CONFIG_V1_INIT;
    c.store_id=75u;c.slot_capacity=slots;c.max_frame_bytes=8u;
    c.max_pins=12u;c.max_leases=12u;c.max_retained_bytes=slots*8u;
    assert(tm_fs_create(&c,&s)==TM_FS_OK);return s;
}
static tm_ex_owner *make_executor(tm_sg_program *p,tm_fs_store *s,fixture *f){
    tm_ex_config_v1 c=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 b[4];
    const char *ids[4]={"src","proc","record","preview"};
    tm_ex_owner *o=NULL;
    int i;
    c.max_frame_bytes=8u;c.source_session_generation=22u;
    c.display_generation=19u;c.fanout.branch_count=2u;
    c.fanout.branches[0]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
    c.fanout.branches[0].policy=TM_FO_ORDERED_LOSSLESS;
    c.fanout.branches[0].queue_depth=1u;
    c.fanout.branches[1]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
    c.fanout.branches[1].policy=TM_FO_LATEST;
    c.fanout.branches[1].queue_depth=1u;
    for(i=0;i<4;++i){
        b[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        b[i].node_id=ids[i];b[i].ctx=f;
    }
    b[0].source_next=source;b[1].visit=process;
    b[2].visit=record;b[3].visit=preview;
    /* Preflight of an invalid binding must not create a partial executor. */
    assert(tm_ex_create(p,s,&c,b,3u,&o)==TM_EX_EGRAPH && o==NULL);
    assert(tm_ex_create(p,s,&c,b,4u,&o)==TM_EX_OK);
    f->executor=o;
    return o;
}
static void finish(tm_ex_owner *o,tm_fs_store *s,tm_sg_program *p){
    assert(tm_ex_destroy(o)==TM_EX_OK);
    assert(tm_fs_seal(s)==TM_FS_OK);
    assert(tm_fs_destroy(s)==TM_FS_OK);
    tm_sg_program_destroy(p);
}
static void test_full_lossless_and_eos(void){
    fixture f={0};
    tm_sg_program *p=make_program();tm_fs_store *s=make_store(4u);
    tm_ex_owner *o=make_executor(p,s,&f);
    tm_ex_stats_v1 st=TM_EX_STATS_V1_INIT;
    assert(tm_ex_pump_source(o)==TM_EX_OK);
    assert(tm_ex_pump_source(o)==TM_EX_EFULL);
    assert(tm_ex_stats(o,&st)==TM_EX_OK && st.pending==1u);
    assert(f.calls==2u && f.processed==2u);
    assert(tm_ex_pump_source(o)==TM_EX_EFULL && f.calls==2u);
    assert(tm_ex_pump_sink(o,0u)==TM_EX_OK);
    assert(tm_ex_pump_source(o)==TM_EX_OK && f.calls==2u);
    assert(tm_ex_pump_sink(o,1u)==TM_EX_OK);
    assert(tm_ex_pump_sink(o,0u)==TM_EX_OK);
    assert(tm_ex_pump_source(o)==TM_EX_OK);
    assert(tm_ex_pump_sink(o,0u)==TM_EX_OK);
    assert(tm_ex_pump_sink(o,1u)==TM_EX_OK);
    assert(tm_ex_pump_source(o)==TM_EX_EOS);
    assert(f.sink_calls[0]==3u && f.output[0][0]==1u &&
           f.output[0][1]==2u && f.output[0][2]==3u);
    assert(f.sink_calls[1]==2u && f.output[1][0]==2u && f.output[1][1]==3u);
    finish(o,s,p);
}
static void test_cancel_requires_drain(void){
    fixture f={0};tm_sg_program *p=make_program();tm_fs_store *s=make_store(4u);
    tm_ex_owner *o=make_executor(p,s,&f);
    assert(tm_ex_pump_source(o)==TM_EX_OK);
    assert(tm_ex_pump_source(o)==TM_EX_EFULL);
    assert(tm_ex_stop(o)==TM_EX_OK);
    assert(tm_ex_destroy(o)==TM_EX_EBUSY);
    assert(tm_ex_discard_ready(o,0u)==TM_EX_OK);
    assert(tm_ex_discard_ready(o,1u)==TM_EX_OK);
    finish(o,s,p);
}
static void test_provider_failure(void){
    fixture f={0};tm_sg_program *p=make_program();tm_fs_store *s=make_store(4u);
    tm_ex_owner *o=make_executor(p,s,&f);f.fail_sink=1;
    assert(tm_ex_pump_source(o)==TM_EX_OK);
    assert(tm_ex_pump_sink(o,0u)==TM_EX_EPROVIDER);
    assert(tm_ex_pump_source(o)==TM_EX_EOS);
    assert(tm_ex_discard_ready(o,1u)==TM_EX_OK);
    finish(o,s,p);
}
static void test_frame_store_full_buffered_source(void){
    fixture f={0};tm_sg_program *p=make_program();tm_fs_store *s=make_store(1u);
    tm_ex_owner *o=make_executor(p,s,&f);
    tm_ex_stats_v1 st=TM_EX_STATS_V1_INIT;
    assert(tm_ex_pump_source(o)==TM_EX_OK);
    assert(tm_ex_pump_source(o)==TM_EX_EFULL);
    assert(tm_ex_stats(o,&st)==TM_EX_OK && st.pending==1u);
    assert(f.calls==2u && f.processed==1u);
    assert(tm_ex_pump_sink(o,0u)==TM_EX_OK);
    assert(tm_ex_pump_sink(o,1u)==TM_EX_OK);
    assert(tm_ex_pump_source(o)==TM_EX_OK && f.calls==2u && f.processed==2u);
    assert(tm_ex_stop(o)==TM_EX_OK);
    assert(tm_ex_discard_ready(o,0u)==TM_EX_OK);
    assert(tm_ex_discard_ready(o,1u)==TM_EX_OK);
    finish(o,s,p);
}

typedef struct {
    unsigned counter, seen;
    uint64_t source_seq;
    tm_fs_store *store;
} meta_fixture;

static tm_ex_source_result meta_source_next(void *v,uint8_t *out,
                                            size_t cap,size_t *size) {
    meta_fixture *f=(meta_fixture *)v;
    if(f->counter==2u)return TM_EX_SOURCE_EOS;
    assert(cap>=1u);
    out[0]=(uint8_t)++f->counter;*size=1u;
    f->source_seq=f->counter;
    return TM_EX_SOURCE_FRAME;
}
static int meta_source_info(void *v,tm_fs_frame_metadata_v1 *m) {
    meta_fixture *f=(meta_fixture *)v;
    assert(m->size==sizeof(*m) && m->version==TM_FS_META_ABI_V1);
    m->width=1u;m->height=1u;m->format_id=1u;
    m->device_session_id=3u;m->session_generation=2u;m->display_id=7u;
    m->source_session_generation=22u;m->display_generation=19u;
    m->geometry_generation=4u;m->capture_session_lo=89u;
    m->stream_generation=3u;m->packet_sequence=f->source_seq;
    m->pts=f->source_seq*33000u;m->dts=m->pts;
    m->timebase_num=1u;m->timebase_den=1000000u;
    return 0;
}
static int meta_sink_visit(void *v,const tm_fs_frame_ref_v1 *ref,
                           const tm_fs_cpu_view_v1 *view) {
    meta_fixture *f=(meta_fixture *)v;
    tm_fs_frame_metadata_v1 m=TM_FS_FRAME_METADATA_V1_INIT;
    assert(view->size_bytes==1u && *view->data==ref->frame_id);
    assert(tm_fs_get_metadata(f->store,ref,&m)==TM_FS_OK);
    assert(m.packet_sequence==ref->frame_id && m.pts==ref->frame_id*33000u);
    f->seen++;
    return 0;
}
static void test_metadata_survives_pending_frame_backpressure(void) {
    meta_fixture f={0};
    tm_sg_program *p=make_program();
    tm_fs_store *s=make_store(4u);
    tm_ex_config_v1 cfg=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 binds[4];
    const char *names[4]={"src","proc","record","preview"};
    tm_ex_owner *owner=NULL;
    unsigned i;
    cfg.max_frame_bytes=8u;cfg.source_session_generation=22u;
    cfg.display_generation=19u;cfg.fanout.branch_count=2u;
    for(i=0u;i<2u;i++){
        cfg.fanout.branches[i]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
        cfg.fanout.branches[i].policy=TM_FO_ORDERED_LOSSLESS;
        cfg.fanout.branches[i].queue_depth=1u;
    }
    f.store=s;
    for(i=0u;i<4u;i++){
        binds[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        binds[i].node_id=names[i];binds[i].ctx=&f;
        binds[i].visit=meta_sink_visit;
    }
    binds[0].visit=NULL;binds[0].source_next=meta_source_next;
    assert(tm_ex_create(p,s,&cfg,binds,4u,&owner)==TM_EX_OK);
    assert(tm_ex_bind_source_metadata(owner,meta_source_info,&f)==TM_EX_OK);
    assert(tm_ex_pump_source(owner)==TM_EX_OK);
    assert(tm_ex_pump_source(owner)==TM_EX_EFULL);
    assert(f.counter==2u);
    assert(tm_ex_bind_source_metadata(owner,meta_source_info,&f)==TM_EX_ESTATE);
    assert(tm_ex_pump_sink(owner,0u)==TM_EX_OK);
    assert(tm_ex_pump_sink(owner,1u)==TM_EX_OK);
    assert(tm_ex_pump_source(owner)==TM_EX_OK && f.counter==2u);
    assert(tm_ex_pump_sink(owner,0u)==TM_EX_OK);
    assert(tm_ex_pump_sink(owner,1u)==TM_EX_OK);
    assert(tm_ex_pump_source(owner)==TM_EX_EOS);
    assert(f.seen==6u);
    finish(owner,s,p);
}

#ifdef TM_STREAMGRAPH_REAL_H264_TEST
void test_m17_real_h264_codec_into_graph(void);
#endif
int main(void){
#ifdef TM_STREAMGRAPH_REAL_H264_TEST
    test_m17_real_h264_codec_into_graph();
#endif
    test_metadata_survives_pending_frame_backpressure();
    test_full_lossless_and_eos();
    test_cancel_requires_drain();
    test_provider_failure();
    test_frame_store_full_buffered_source();
    puts("StreamGraph Executor: PASS");
    return 0;
}
