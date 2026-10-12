#include <turbo_streamgraph_owner_dispatch.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct fixture {
    unsigned calls, processed, recorded, previewed, noticed, nested_checks;
    tm_od_owner *dispatcher;
    tm_ho_inbox *inbox;
    tm_fs_store *store;
} fixture;

static tm_sg_port_v1 port(const char *name, uint32_t dir) {
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=name;p.direction=dir;p.kind=TM_SG_VIDEO_FRAME;
    p.memory_domain=TM_SG_MEMORY_CPU;p.format_id=1u;
    p.timebase_num=1u;p.timebase_den=30u;
    return p;
}
static tm_sg_program *program(void) {
    tm_sg_port_v1 src=port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 mid[2]={ {0}, {0} };
    tm_sg_port_v1 sink=port("in",TM_SG_INPUT);
    tm_sg_node_v1 nodes[4];
    tm_sg_edge_v1 edges[3];
    tm_sg_program *p=NULL;
    const char *ids[]={"src","processor","record","preview"};
    unsigned i;
    mid[0]=port("in",TM_SG_INPUT);
    mid[1]=port("out",TM_SG_OUTPUT);
    for(i=0u;i<4u;++i) {
        nodes[i]=(tm_sg_node_v1)TM_SG_NODE_V1_INIT;
        nodes[i].id=ids[i];
        nodes[i].kind=i==0u?TM_SG_SOURCE:(i==1u?TM_SG_PROCESSOR:TM_SG_SINK);
        nodes[i].port_count=i==1u?2u:1u;
        nodes[i].ports=i==0u?&src:(i==1u?mid:&sink);
    }
    for(i=0u;i<3u;++i) edges[i]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
    edges[0].source_node="src"; edges[0].source_port="out";
    edges[0].target_node="processor"; edges[0].target_port="in";
    for(i=1u;i<3u;++i) {
        edges[i].source_node="processor"; edges[i].source_port="out";
        edges[i].target_node=ids[i+1u]; edges[i].target_port="in";
    }
    assert(tm_sg_compile(nodes,4u,edges,3u,NULL,&p,NULL)==TM_SG_OK);
    return p;
}
static tm_ex_source_result source(void *ctx,uint8_t *dst,size_t cap,size_t *n) {
    fixture *f=(fixture *)ctx;
    assert(cap>=1u);
    if(f->dispatcher) {
        tm_od_receipt_v1 receipt=TM_OD_RECEIPT_V1_INIT;
        assert(tm_od_step(f->dispatcher,&receipt)==TM_OD_EBUSY);
        f->nested_checks++;
    }
    f->calls++;
    if(f->calls>=3u) return TM_EX_SOURCE_EOS;
    dst[0]=(uint8_t)f->calls;
    *n=1u;
    return TM_EX_SOURCE_FRAME;
}
static int process(void *ctx,const tm_fs_frame_ref_v1 *ref,
                   const tm_fs_cpu_view_v1 *view) {
    fixture *f=(fixture *)ctx;
    assert(view->size_bytes==1u && *view->data==ref->frame_id);
    f->processed++;
    return 0;
}
static int record(void *ctx,const tm_fs_frame_ref_v1 *ref,
                  const tm_fs_cpu_view_v1 *view) {
    fixture *f=(fixture *)ctx;
    assert(view->size_bytes==1u && *view->data==ref->frame_id);
    f->recorded++;
    return 0;
}
static int preview(void *ctx,const tm_fs_frame_ref_v1 *ref,
                   const tm_fs_cpu_view_v1 *view) {
    fixture *f=(fixture *)ctx;
    assert(view->size_bytes==1u && *view->data==ref->frame_id);
    f->previewed++;
    return 0;
}
static int visit_notice(void *ctx,const tm_fs_frame_ref_v1 *ref,
                        const tm_fs_cpu_view_v1 *view) {
    fixture *f=(fixture *)ctx;
    tm_ho_snapshot_v1 snapshot=TM_HO_SNAPSHOT_V1_INIT;
    tm_od_receipt_v1 receipt=TM_OD_RECEIPT_V1_INIT;
    assert(view->size_bytes==1u && *view->data==42u);
    assert(tm_fs_retire(f->store,ref)==TM_FS_EBUSY);
    assert(tm_ho_snapshot(f->inbox,&snapshot)==TM_HO_OK);
    assert(snapshot.taken==1u);
    assert(tm_od_step(f->dispatcher,&receipt)==TM_OD_EBUSY);
    f->noticed++;
    return 0;
}
static tm_fs_store *store(void) {
    tm_fs_config_v1 cfg=TM_FS_CONFIG_V1_INIT;
    tm_fs_store *s=NULL;
    cfg.store_id=555u; cfg.slot_capacity=8u; cfg.max_frame_bytes=8u;
    cfg.max_pins=16u; cfg.max_leases=16u; cfg.max_retained_bytes=64u;
    assert(tm_fs_create(&cfg,&s)==TM_FS_OK);
    return s;
}
static tm_ex_owner *executor(tm_sg_program *graph,tm_fs_store *s,fixture *f) {
    tm_ex_binding_v1 binds[4];
    tm_ex_config_v1 ec=TM_EX_CONFIG_V1_INIT;
    tm_ex_owner *owner=NULL;
    const char *ids[]={"src","processor","record","preview"};
    unsigned i;
    ec.max_frame_bytes=8u;
    ec.source_session_generation=2u;
    ec.display_generation=3u;
    ec.fanout.branch_count=2u;
    for(i=0u;i<2u;++i) {
        ec.fanout.branches[i]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
        ec.fanout.branches[i].policy=TM_FO_ORDERED_LOSSLESS;
        ec.fanout.branches[i].queue_depth=1u;
    }
    for(i=0u;i<4u;++i) {
        binds[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        binds[i].node_id=ids[i];
        binds[i].ctx=f;
    }
    binds[0].source_next=source;
    binds[1].visit=process;
    binds[2].visit=record;
    binds[3].visit=preview;
    assert(tm_ex_create(graph,s,&ec,binds,4u,&owner)==TM_EX_OK);
    return owner;
}
static tm_ho_inbox *inbox(void) {
    tm_ho_config_v1 config=TM_HO_CONFIG_V1_INIT;
    tm_ho_inbox *q=NULL;
    config.capacity=8u;
    assert(tm_ho_create(&config,&q)==TM_HO_OK);
    return q;
}
static tm_od_receipt_v1 send(tm_ho_inbox *q,tm_od_owner *owner,
                             uint32_t kind,uint32_t branch,uint64_t correlation,
                             const tm_fs_frame_ref_v1 *frame) {
    tm_ho_ticket_v1 t={0};
    tm_ho_command_v1 command=TM_HO_COMMAND_V1_INIT;
    tm_od_receipt_v1 result=TM_OD_RECEIPT_V1_INIT;
    command.kind=kind;
    command.branch=branch;
    command.correlation=correlation;
    if(frame) {
        command.has_frame=1u;
        command.frame=*frame;
    }
    assert(tm_ho_reserve(q,&t)==TM_HO_OK);
    assert(tm_ho_publish(q,t,&command)==TM_HO_OK);
    assert(tm_od_step(owner,&result)==TM_OD_OK);
    assert(result.correlation==correlation && result.command_kind==kind);
    return result;
}
static void cleanup(tm_od_owner *d,tm_ho_inbox *q,tm_ex_owner *ex,
                    tm_fs_store *s,tm_sg_program *g) {
    assert(tm_od_destroy(d)==TM_OD_OK);
    assert(tm_ho_destroy(q)==TM_HO_OK);
    assert(tm_ex_destroy(ex)==TM_EX_OK);
    assert(tm_fs_seal(s)==TM_FS_OK);
    assert(tm_fs_destroy(s)==TM_FS_OK);
    tm_sg_program_destroy(g);
}
static void test_executor_backpressure_and_eos(void) {
    fixture f={0};
    tm_sg_program *g=program();
    tm_fs_store *s=store();
    tm_ho_inbox *q=inbox();
    tm_ex_owner *ex=executor(g,s,&f);
    tm_od_owner *d=NULL;
    tm_od_config_v1 config=TM_OD_CONFIG_V1_INIT;
    tm_od_receipt_v1 r=TM_OD_RECEIPT_V1_INIT;
    assert(tm_od_create(q,ex,s,&config,&d)==TM_OD_OK);
    f.dispatcher=d;f.inbox=q;f.store=s;
    assert(tm_od_step(d,&r)==TM_OD_EMPTY);
    r=send(q,d,TM_HO_PUMP_SOURCE,0u,1u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.calls==1u && f.processed==1u);
    r=send(q,d,TM_HO_PUMP_SOURCE,0u,2u,NULL);
    assert(r.outcome==TM_OD_BACKPRESSURE && f.calls==2u && f.processed==2u);
    r=send(q,d,TM_HO_PUMP_SOURCE,0u,3u,NULL);
    assert(r.outcome==TM_OD_BACKPRESSURE && f.calls==2u);
    r=send(q,d,TM_HO_PUMP_SINK,0u,4u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.recorded==1u);
    r=send(q,d,TM_HO_PUMP_SINK,1u,5u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.previewed==1u);
    r=send(q,d,TM_HO_PUMP_SOURCE,0u,6u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.calls==2u);
    r=send(q,d,TM_HO_PUMP_SINK,0u,7u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.recorded==2u);
    r=send(q,d,TM_HO_PUMP_SINK,1u,8u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.previewed==2u);
    r=send(q,d,TM_HO_PUMP_SOURCE,0u,9u,NULL);
    assert(r.outcome==TM_OD_EOS && f.calls==3u && f.nested_checks==3u);
    assert(tm_od_step(d,&r)==TM_OD_EMPTY);
    cleanup(d,q,ex,s,g);
}
static void test_frame_pin_scope_and_stop(void) {
    fixture f={0};
    tm_sg_program *g=program();
    tm_fs_store *s=store();
    tm_ho_inbox *q=inbox();
    tm_ex_owner *ex=executor(g,s,&f);
    tm_od_owner *d=NULL;
    tm_od_config_v1 config=TM_OD_CONFIG_V1_INIT;
    tm_fs_frame_ref_v1 stale=TM_FS_FRAME_REF_V1_INIT;
    tm_fs_frame_ref_v1 valid=TM_FS_FRAME_REF_V1_INIT;
    tm_od_receipt_v1 r;
    uint8_t value=42u;
    config.visit_frame=visit_notice;
    config.visit_context=&f;
    assert(tm_od_create(q,ex,s,&config,&d)==TM_OD_OK);
    f.dispatcher=d;f.inbox=q;f.store=s;
    assert(tm_fs_publish_cpu(s,&value,1u,2u,3u,&stale)==TM_FS_OK);
    assert(tm_fs_retire(s,&stale)==TM_FS_OK);
    r=send(q,d,TM_HO_FRAME_NOTICE,0u,20u,&stale);
    assert(r.outcome==TM_OD_STALE_FRAME && f.noticed==0u);
    assert(tm_fs_publish_cpu(s,&value,1u,2u,3u,&valid)==TM_FS_OK);
    r=send(q,d,TM_HO_FRAME_NOTICE,0u,21u,&valid);
    assert(r.outcome==TM_OD_COMPLETED && f.noticed==1u);
    /* The callback requested retirement while the owner-held Pin/Lease
     * was active. Its final release auto-retires the slot. No second
     * retirement is needed or permitted for the now-stale identity. */
    assert(tm_fs_retire(s,&valid)==TM_FS_ESTALE);
    {
        tm_fs_lease_v1 old_lease={0};
        assert(tm_fs_acquire_weak(s,&valid,&old_lease)==TM_FS_ESTALE);
    }
    r=send(q,d,TM_HO_STOP,0u,22u,NULL);
    assert(r.outcome==TM_OD_COMPLETED);
    cleanup(d,q,ex,s,g);
}
int main(void) {
    test_executor_backpressure_and_eos();
    test_frame_pin_scope_and_stop();
    puts("StreamGraph owner dispatcher / retained frame visit: PASS");
    return 0;
}
