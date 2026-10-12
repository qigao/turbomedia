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
} fixture;
static tm_ex_source_result source(void *ctx,uint8_t *dst,size_t cap,size_t *n){
    fixture *f=(fixture *)ctx;f->calls++;
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
    assert(tm_ex_create(p,s,&c,b,4u,&o)==TM_EX_OK);return o;
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
int main(void){
    test_full_lossless_and_eos();
    test_cancel_requires_drain();
    test_provider_failure();
    test_frame_store_full_buffered_source();
    puts("StreamGraph Executor: PASS");
    return 0;
}
