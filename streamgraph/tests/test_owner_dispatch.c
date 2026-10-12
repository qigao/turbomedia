#include <turbo_streamgraph_owner_dispatch.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static void yield_owner(void) { Sleep(0); }
#else
#include <pthread.h>
#include <sched.h>
static void yield_owner(void) { sched_yield(); }
#endif

typedef struct fixture {
    unsigned calls, processed, recorded, previewed, noticed, nested_checks;
    unsigned unlimited_source;
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
    if(!f->unlimited_source && f->calls>=3u) return TM_EX_SOURCE_EOS;
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
static tm_od_receipt_v1 dispatch_command(tm_ho_inbox *q,tm_od_owner *owner,
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
    r=dispatch_command(q,d,TM_HO_PUMP_SOURCE,0u,1u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.calls==1u && f.processed==1u);
    r=dispatch_command(q,d,TM_HO_PUMP_SOURCE,0u,2u,NULL);
    assert(r.outcome==TM_OD_BACKPRESSURE && f.calls==2u && f.processed==2u);
    r=dispatch_command(q,d,TM_HO_PUMP_SOURCE,0u,3u,NULL);
    assert(r.outcome==TM_OD_BACKPRESSURE && f.calls==2u);
    r=dispatch_command(q,d,TM_HO_PUMP_SINK,0u,4u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.recorded==1u);
    r=dispatch_command(q,d,TM_HO_PUMP_SINK,1u,5u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.previewed==1u);
    r=dispatch_command(q,d,TM_HO_PUMP_SOURCE,0u,6u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.calls==2u);
    r=dispatch_command(q,d,TM_HO_PUMP_SINK,0u,7u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.recorded==2u);
    r=dispatch_command(q,d,TM_HO_PUMP_SINK,1u,8u,NULL);
    assert(r.outcome==TM_OD_COMPLETED && f.previewed==2u);
    r=dispatch_command(q,d,TM_HO_PUMP_SOURCE,0u,9u,NULL);
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
    r=dispatch_command(q,d,TM_HO_FRAME_NOTICE,0u,20u,&stale);
    assert(r.outcome==TM_OD_STALE_FRAME && f.noticed==0u);
    assert(tm_fs_publish_cpu(s,&value,1u,2u,3u,&valid)==TM_FS_OK);
    r=dispatch_command(q,d,TM_HO_FRAME_NOTICE,0u,21u,&valid);
    assert(r.outcome==TM_OD_COMPLETED && f.noticed==1u);
    /* The callback requested retirement while the owner-held Pin/Lease
     * was active. Its final release auto-retires the slot. No second
     * retirement is needed or permitted for the now-stale identity. */
    assert(tm_fs_retire(s,&valid)==TM_FS_ESTALE);
    {
        tm_fs_lease_v1 old_lease={0};
        assert(tm_fs_acquire_weak(s,&valid,&old_lease)==TM_FS_ESTALE);
    }
    r=dispatch_command(q,d,TM_HO_STOP,0u,22u,NULL);
    assert(r.outcome==TM_OD_COMPLETED);
    cleanup(d,q,ex,s,g);
}
#define OWNER_THREADS 4u
#define OWNER_COMMANDS 90u
typedef struct concurrent_producer {
    tm_ho_inbox *queue;
    unsigned producer_id;
} concurrent_producer;

#ifdef _WIN32
static DWORD WINAPI dispatch_producer(LPVOID ptr)
#else
static void *dispatch_producer(void *ptr)
#endif
{
    concurrent_producer *p=(concurrent_producer *)ptr;
    unsigned i;
    for(i=0u;i<OWNER_COMMANDS;++i) {
        tm_ho_ticket_v1 ticket={0};
        tm_ho_command_v1 cmd=TM_HO_COMMAND_V1_INIT;
        tm_ho_status rc;
        cmd.kind=(i%3u)==0u?TM_HO_PUMP_SOURCE:TM_HO_PUMP_SINK;
        cmd.branch=cmd.kind==TM_HO_PUMP_SINK?(i%3u)-1u:0u;
        cmd.correlation=10000u+(uint64_t)p->producer_id*OWNER_COMMANDS+i;
        do {
            rc=tm_ho_reserve(p->queue,&ticket);
            if(rc==TM_HO_EFULL) yield_owner();
        } while(rc==TM_HO_EFULL);
        assert(rc==TM_HO_OK);
        assert(tm_ho_publish(p->queue,ticket,&cmd)==TM_HO_OK);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void test_multithread_producers_and_real_executor(void) {
    fixture f={0};
    tm_sg_program *g=program();
    tm_fs_store *s=store();
    tm_ho_inbox *q=inbox();
    tm_ex_owner *ex=executor(g,s,&f);
    tm_od_owner *d=NULL;
    tm_od_config_v1 config=TM_OD_CONFIG_V1_INIT;
    concurrent_producer args[OWNER_THREADS];
    unsigned char observed[OWNER_THREADS][OWNER_COMMANDS]={{0}};
    unsigned n=0u,i;
#ifdef _WIN32
    HANDLE threads[OWNER_THREADS]={0};
#else
    pthread_t threads[OWNER_THREADS];
#endif
    f.unlimited_source=1u;
    assert(tm_od_create(q,ex,s,&config,&d)==TM_OD_OK);
    f.dispatcher=d; f.inbox=q; f.store=s;
    for(i=0u;i<OWNER_THREADS;++i) {
        args[i].queue=q;
        args[i].producer_id=i;
#ifdef _WIN32
        threads[i]=CreateThread(NULL,0,dispatch_producer,&args[i],0,NULL);
        assert(threads[i]!=NULL);
#else
        assert(pthread_create(&threads[i],NULL,dispatch_producer,&args[i])==0);
#endif
    }
    while(n<OWNER_THREADS*OWNER_COMMANDS) {
        tm_od_receipt_v1 receipt=TM_OD_RECEIPT_V1_INIT;
        tm_od_status rc=tm_od_step(d,&receipt);
        if(rc==TM_OD_EMPTY) { yield_owner(); continue; }
        assert(rc==TM_OD_OK);
        assert(receipt.correlation>=10000u);
        {
            uint64_t id=receipt.correlation-10000u;
            unsigned worker=(unsigned)(id/OWNER_COMMANDS);
            unsigned sequence=(unsigned)(id%OWNER_COMMANDS);
            assert(worker<OWNER_THREADS && sequence<OWNER_COMMANDS);
            assert(observed[worker][sequence]==0u);
            observed[worker][sequence]=1u;
        }
        assert(receipt.outcome==TM_OD_COMPLETED ||
               receipt.outcome==TM_OD_BACKPRESSURE);
        ++n;
    }
    for(i=0u;i<OWNER_THREADS;++i) {
        unsigned j;
#ifdef _WIN32
        assert(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0);
        CloseHandle(threads[i]);
#else
        assert(pthread_join(threads[i],NULL)==0);
#endif
        for(j=0u;j<OWNER_COMMANDS;++j)assert(observed[i][j]==1u);
    }
    {
        tm_ho_snapshot_v1 snapshot=TM_HO_SNAPSHOT_V1_INIT;
        assert(tm_ho_snapshot(q,&snapshot)==TM_HO_OK);
        assert(snapshot.drained==1u && snapshot.sealed==0u);
    }
    assert(dispatch_command(q,d,TM_HO_STOP,0u,999999u,NULL).outcome==TM_OD_COMPLETED);
    assert(tm_ex_discard_ready(ex,0u)==TM_EX_OK);
    assert(tm_ex_discard_ready(ex,1u)==TM_EX_OK);
    cleanup(d,q,ex,s,g);
}

/* Real asynchronous Provider callback -> Portal -> existing Media Owner.
 * The worker touches only the Portal; all delivery happens on owner lane. */
typedef struct terminal_fixture {
    tm_od_owner *owner;
    uint32_t calls;
    uint32_t rejections;
    uint32_t fail_once;
    uint64_t correlations[128];
} terminal_fixture;
static int terminal_emit(void *context, const tm_pc_result_v1 *terminal) {
    terminal_fixture *f=(terminal_fixture *)context;
    tm_pc_result_v1 recursive=TM_PC_RESULT_V1_INIT;
    assert(tm_od_step_completion(f->owner,&recursive)==TM_OD_EBUSY);
    assert(terminal->provider_generation==78u);
    assert(terminal->correlation>0u);
    assert(f->calls<128u);
    f->correlations[f->calls++]=terminal->correlation;
    if (f->fail_once) {
        f->fail_once=0u;
        f->rejections++;
        return -1; /* Downstream acceptance is unresolved; no auto replay. */
    }
    return 0;
}
typedef struct provider_worker_args {
    tm_pc_portal *portal;
    tm_pc_ticket_v1 tickets[96];
    uint32_t count;
} provider_worker_args;
#ifdef _WIN32
static DWORD WINAPI provider_worker(LPVOID ptr)
#else
static void *provider_worker(void *ptr)
#endif
{
    provider_worker_args *work=(provider_worker_args *)ptr;
    uint32_t i;
    for (i=0u;i<work->count;++i)
        assert(tm_pc_complete(work->portal,work->tickets[i],(int32_t)i)==TM_PC_OK);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void test_provider_terminal_on_owner(void) {
    fixture f={0};
    terminal_fixture receiver={0};
    tm_sg_program *g=program();
    tm_fs_store *s=store();
    tm_ho_inbox *q=inbox();
    tm_ex_owner *ex=executor(g,s,&f);
    tm_od_owner *d=NULL;
    tm_od_config_v1 cfg=TM_OD_CONFIG_V1_INIT;
    tm_pc_config_v1 pcfg=TM_PC_CONFIG_V1_INIT;
    tm_pc_portal *portal=NULL;
    tm_pc_ticket_v1 ticket={0};
    tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
    provider_worker_args work={0};
    uint32_t i;
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    pcfg.capacity=100u;
    pcfg.provider_generation=78u;
    assert(tm_pc_create(&pcfg,&portal)==TM_PC_OK);
    assert(tm_od_create(q,ex,s,&cfg,&d)==TM_OD_OK);
    receiver.owner=d;
    assert(tm_od_bind_completion(d,portal,terminal_emit,&receiver)==TM_OD_OK);
    assert(tm_od_bind_completion(d,portal,terminal_emit,&receiver)==TM_OD_EBUSY);
    assert(tm_od_step_completion(d,&result)==TM_OD_EMPTY);
    /* A failed output is retained as TAKEN, not delivered twice. */
    receiver.fail_once=1u;
    assert(tm_pc_begin(portal,901u,&ticket)==TM_PC_OK);
    assert(tm_pc_complete(portal,ticket,42)==TM_PC_OK);
    assert(tm_od_step_completion(d,&result)==TM_OD_EDELIVERY);
    assert(result.correlation==901u && result.provider_status==42);
    assert(receiver.calls==1u && receiver.rejections==1u);
    assert(tm_od_pending_completion(d,&result)==TM_OD_OK);
    assert(result.correlation==901u);
    assert(tm_od_step_completion(d,&result)==TM_OD_EBUSY);
    assert(receiver.calls==1u);
    assert(tm_od_destroy(d)==TM_OD_EBUSY);
    /* Host has now reconciled its external delivery obligation and elects
     * explicit settlement, not automatic callback replay. */
    assert(tm_od_settle_pending_completion(d)==TM_OD_OK);
    assert(tm_od_settle_pending_completion(d)==TM_OD_EMPTY);
    assert(tm_od_pending_completion(d,&result)==TM_OD_EMPTY);
    assert(tm_pc_begin(portal,902u,&ticket)==TM_PC_OK);
    assert(tm_pc_request_cancel(portal,ticket)==TM_PC_OK);
    assert(tm_pc_complete(portal,ticket,99)==TM_PC_OK);
    assert(tm_od_step_completion(d,&result)==TM_OD_OK);
    assert(result.outcome==TM_PC_CANCELLED &&
           result.completion_observed==1u &&
           receiver.correlations[1]==902u);
    /* Real callback thread races the owner polling the same portal. */
    work.portal=portal;work.count=96u;
    for (i=0u;i<work.count;++i)
        assert(tm_pc_begin(portal,(uint64_t)i+1000u,&work.tickets[i])==TM_PC_OK);
#ifdef _WIN32
    thread=CreateThread(NULL,0,provider_worker,&work,0,NULL);
    assert(thread!=NULL);
#else
    assert(pthread_create(&thread,NULL,provider_worker,&work)==0);
#endif
    i=0u;
    while(i<work.count) {
        tm_od_status rc=tm_od_step_completion(d,&result);
        if (rc==TM_OD_EMPTY) {yield_owner();continue;}
        assert(rc==TM_OD_OK);
        assert(result.outcome==TM_PC_COMPLETED);
        assert(result.correlation>=1000u && result.correlation<1096u);
        ++i;
    }
#ifdef _WIN32
    assert(WaitForSingleObject(thread,INFINITE)==WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    assert(pthread_join(thread,NULL)==0);
#endif
    assert(receiver.calls==98u);
    for(i=0u;i<work.count;++i)
        assert(receiver.correlations[2u+i]==(uint64_t)i+1000u);
    assert(tm_od_step_completion(d,&result)==TM_OD_EMPTY);
    /* Closing the command side alone is insufficient: an attached Portal
     * requires its own seal, drain, callback-join, and generation quiescence. */
    assert(tm_ho_seal(q)==TM_HO_OK);
    assert(tm_od_destroy(d)==TM_OD_EBUSY);
    assert(tm_pc_seal(portal)==TM_PC_OK);
    {
        tm_pc_snapshot_v1 snap=TM_PC_SNAPSHOT_V1_INIT;
        assert(tm_pc_snapshot(portal,&snap)==TM_PC_OK && snap.drained);
    }
    assert(tm_od_destroy(d)==TM_OD_OK);
    assert(tm_pc_destroy(portal)==TM_PC_OK);
    assert(tm_ho_destroy(q)==TM_HO_OK);
    assert(tm_ex_stop(ex)==TM_EX_OK);
    assert(tm_ex_destroy(ex)==TM_EX_OK);
    assert(tm_fs_seal(s)==TM_FS_OK);
    assert(tm_fs_destroy(s)==TM_FS_OK);
    tm_sg_program_destroy(g);
}

int main(void) {
    test_executor_backpressure_and_eos();
    test_frame_pin_scope_and_stop();
    test_multithread_producers_and_real_executor();
    test_provider_terminal_on_owner();
    puts("StreamGraph owner dispatcher / retained frame visit: PASS");
    return 0;
}
