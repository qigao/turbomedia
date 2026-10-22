#include <turbo_streamgraph_executor.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned sourced, visited_first, visited_second, recorded, previewed;
    unsigned expected_stage;
    int reject_second;
} fixture;

static tm_sg_port_v1 port(const char *id, uint32_t direction) {
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=id; p.direction=direction; p.kind=TM_SG_VIDEO_FRAME;
    p.memory_domain=TM_SG_MEMORY_CPU; p.format_id=1u;
    p.timebase_num=1u; p.timebase_den=30u;
    return p;
}
static tm_sg_program *program(int illegal_branch) {
    static const char *names[]={"source","observe1","observe2","record","preview"};
    tm_sg_node_v1 nodes[5];
    tm_sg_port_v1 src=port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 proc[2]={ {0}, {0} };
    tm_sg_port_v1 sink=port("in",TM_SG_INPUT);
    tm_sg_edge_v1 edges[4];
    tm_sg_program *result=NULL;
    unsigned i;
    proc[0]=port("in",TM_SG_INPUT);
    proc[1]=port("out",TM_SG_OUTPUT);
    for(i=0;i<5u;i++) {
        nodes[i]=(tm_sg_node_v1)TM_SG_NODE_V1_INIT;
        nodes[i].id=names[i];
        nodes[i].kind=i==0u?TM_SG_SOURCE:(i<3u?TM_SG_PROCESSOR:TM_SG_SINK);
        nodes[i].port_count=i>0u && i<3u?2u:1u;
        nodes[i].ports=i==0u?&src:(i<3u?proc:&sink);
    }
    for(i=0;i<4u;i++) edges[i]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
    edges[0].source_node="source";edges[0].source_port="out";
    edges[0].target_node="observe1";edges[0].target_port="in";
    edges[1].source_node="observe1";edges[1].source_port="out";
    edges[1].target_node="observe2";edges[1].target_port="in";
    edges[2].source_node=illegal_branch?"observe1":"observe2";
    edges[2].source_port="out";
    edges[2].target_node="record";edges[2].target_port="in";
    edges[3].source_node="observe2";edges[3].source_port="out";
    edges[3].target_node="preview";edges[3].target_port="in";
    assert(tm_sg_compile(nodes,5u,edges,4u,NULL,&result,NULL)==TM_SG_OK);
    return result;
}
static tm_ex_source_result produce(void *ctx,uint8_t *dst,size_t cap,size_t *out) {
    fixture *f=ctx;
    if(f->sourced==2u)return TM_EX_SOURCE_EOS;
    assert(cap>=1u);
    dst[0]=(uint8_t)++f->sourced;
    *out=1u;
    f->expected_stage=1u;
    return TM_EX_SOURCE_FRAME;
}
static int observe1(void *ctx,const tm_fs_frame_ref_v1 *ref,
                    const tm_fs_cpu_view_v1 *view) {
    fixture *f=ctx;
    assert(f->expected_stage==1u && view->size_bytes==1u);
    assert(*view->data==ref->frame_id);
    f->expected_stage=2u;
    f->visited_first++;
    return 0;
}
static int observe2(void *ctx,const tm_fs_frame_ref_v1 *ref,
                    const tm_fs_cpu_view_v1 *view) {
    fixture *f=ctx;
    assert(f->expected_stage==2u && *view->data==ref->frame_id);
    f->expected_stage=3u;
    f->visited_second++;
    return f->reject_second?-1:0;
}
static int record(void *ctx,const tm_fs_frame_ref_v1 *ref,
                  const tm_fs_cpu_view_v1 *view) {
    fixture *f=ctx;
    assert(*view->data==ref->frame_id);
    f->recorded++;
    return 0;
}
static int preview(void *ctx,const tm_fs_frame_ref_v1 *ref,
                   const tm_fs_cpu_view_v1 *view) {
    fixture *f=ctx;
    assert(*view->data==ref->frame_id);
    f->previewed++;
    return 0;
}
static void run(int illegal_branch,int reject_second) {
    fixture f={0};
    tm_sg_program *p=program(illegal_branch);
    tm_fs_store *store=NULL;
    tm_ex_owner *owner=NULL;
    tm_fs_config_v1 sc=TM_FS_CONFIG_V1_INIT;
    tm_ex_config_v1 ec=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 bindings[5];
    const char *names[]={"source","observe1","observe2","record","preview"};
    tm_ex_stats_v1 stats=TM_EX_STATS_V1_INIT;
    unsigned i;
    f.reject_second=reject_second;
    sc.store_id=909u;sc.slot_capacity=4u;sc.max_frame_bytes=8u;
    sc.max_pins=8u;sc.max_leases=8u;sc.max_retained_bytes=32u;
    assert(tm_fs_create(&sc,&store)==TM_FS_OK);
    ec.max_frame_bytes=8u;ec.source_session_generation=9u;ec.display_generation=10u;
    ec.fanout.branch_count=2u;
    for(i=0;i<2u;i++) {
        ec.fanout.branches[i]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
        ec.fanout.branches[i].policy=TM_FO_ORDERED_LOSSLESS;
        ec.fanout.branches[i].queue_depth=1u;
    }
    for(i=0;i<5u;i++) {
        bindings[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        bindings[i].node_id=names[i];
        bindings[i].ctx=&f;
    }
    bindings[0].source_next=produce;
    bindings[1].visit=observe1;
    bindings[2].visit=observe2;
    bindings[3].visit=record;
    bindings[4].visit=preview;
    assert(tm_ex_create(p,store,&ec,bindings,4u,&owner)==TM_EX_EGRAPH && owner==NULL);
    if(illegal_branch) {
        assert(tm_ex_create(p,store,&ec,bindings,5u,&owner)==TM_EX_EGRAPH && owner==NULL);
    } else {
        assert(tm_ex_create(p,store,&ec,bindings,5u,&owner)==TM_EX_OK);
        if(reject_second) {
            assert(tm_ex_pump_source(owner)==TM_EX_EPROVIDER);
            assert(f.visited_first==1u && f.visited_second==1u);
            assert(tm_ex_stats(owner,&stats)==TM_EX_OK && stats.processed==0u);
        } else {
            assert(tm_ex_pump_source(owner)==TM_EX_OK);
            assert(tm_ex_pump_source(owner)==TM_EX_EFULL);
            assert(tm_ex_pump_sink(owner,0u)==TM_EX_OK);
            assert(tm_ex_pump_sink(owner,1u)==TM_EX_OK);
            assert(tm_ex_pump_source(owner)==TM_EX_OK);
            assert(tm_ex_pump_sink(owner,0u)==TM_EX_OK);
            assert(tm_ex_pump_sink(owner,1u)==TM_EX_OK);
            assert(tm_ex_pump_source(owner)==TM_EX_EOS);
            assert(f.visited_first==2u && f.visited_second==2u);
            assert(f.recorded==2u && f.previewed==2u);
            assert(tm_ex_stats(owner,&stats)==TM_EX_OK && stats.processed==2u);
        }
        assert(tm_ex_destroy(owner)==TM_EX_OK);
    }
    assert(tm_fs_seal(store)==TM_FS_OK);
    assert(tm_fs_destroy(store)==TM_FS_OK);
    tm_sg_program_destroy(p);
}
int main(void) {
    run(0,0);
    run(1,0);
    run(0,1);
    puts("StreamGraph 2-Processor owner-driven profile: PASS");
    return 0;
}
