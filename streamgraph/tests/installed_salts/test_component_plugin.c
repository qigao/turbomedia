#include "provider_contract.h"
#include <salts/component_plugin.h>
#include <cnet/cnet.h>
#include <turbo_streamgraph_provider_completion.h>
#include <turbo_streamgraph_owner_dispatch.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static void yield_worker(void) { Sleep(0); }
typedef DWORD thread_result;
#define THREAD_CALL WINAPI
#else
#include <pthread.h>
#include <sched.h>
static void yield_worker(void) { sched_yield(); }
typedef void *thread_result;
#define THREAD_CALL
#endif

/* Real Salts Generation bundles: stable storage must outlive all Scope users. */
typedef struct generation_fixture {
    salts_component_plugin_generation generation;
    salts_component_deployment deployments[1];
    salts_component_instance instances[1];
    salts_component_dependency dependencies[1];
    size_t activation_order[1];
    salts_component_plugin_module modules[1];
} generation_fixture;

static salts_component_plugin_status build(
    generation_fixture *f, uint64_t id,
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref) {
    const salts_component_plugin_generation_storage storage={
        f->deployments,1u,f->instances,1u,f->dependencies,1u,
        f->activation_order,1u,f->modules,1u
    };
    const salts_component_plugin_source source={
        ref,TM_TEST_PROVIDER_EXPORT_ID,NULL,NULL
    };
    return salts_component_plugin_generation_build(
        &f->generation,id,registry,&storage,
        NULL,0u,&source,1u,NULL,0u);
}
static tm_test_provider find_provider(salts_component_plugin_scope *scope) {
    salts_component_service service;
    tm_test_provider provider=tm_test_provider_bind(NULL,NULL);
    assert(salts_component_plugin_scope_find_service(
               scope,tm_test_provider_interface(),&service)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(tm_test_provider_borrow_from_object(
               service.object,service.interfaces,&provider)==CMETA_OK);
    return provider;
}
typedef struct provider_worker {
    tm_test_provider service; /* borrowed: guarded by original live Scope */
    tm_pc_portal *completion;
    tm_pc_ticket_v1 ticket;
    cnet_client *wake_owner; /* real CNet borrowed; owner outlives worker */
    int wake_status;
    _Atomic unsigned returned;
    int result;
} provider_worker;
static thread_result THREAD_CALL invoke_provider(void *context) {
    provider_worker *worker=(provider_worker *)context;
    /* This is a REAL method call into the Salts-loaded Provider DSO. */
    worker->result=tm_test_provider_get(&worker->service);
    assert(tm_pc_complete(worker->completion,worker->ticket,worker->result)
           ==TM_PC_OK);
    /* Submit the terminal value BEFORE waking its fixed Media/CNet Owner.
     * Wake is not a completion ACK or license to release the old DSO Scope.
     * This is the sole CNet operation permitted on Provider callback thread. */
    worker->wake_status=cnet_client_wake(worker->wake_owner);
    atomic_store_explicit(&worker->returned,1u,memory_order_release);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}
#if defined(_WIN32)
typedef HANDLE worker_thread;
static void start_worker(worker_thread *t,provider_worker *w) {
    *t=CreateThread(NULL,0,invoke_provider,w,0,NULL);
    assert(*t!=NULL);
}
static void join_worker(worker_thread *t) {
    assert(WaitForSingleObject(*t,INFINITE)==WAIT_OBJECT_0);
    CloseHandle(*t);
}
#else
typedef pthread_t worker_thread;
static void start_worker(worker_thread *t,provider_worker *w) {
    assert(pthread_create(t,NULL,invoke_provider,w)==0);
}
static void join_worker(worker_thread *t) {
    assert(pthread_join(*t,NULL)==0);
}
#endif
static void wait_entered(tm_test_provider *p) {
    unsigned count=0u;
    while (tm_test_provider_entered(p)==0) {
        assert(++count<20000000u); /* CI watchdog, no silent indefinite wait */
        yield_worker();
    }
}
static native_io_backend_kind wake_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__APPLE__)
    return NATIVE_IO_BACKEND_KQUEUE;
#else
    return NATIVE_IO_BACKEND_EPOLL;
#endif
}

/* Caller-driven, actual CNet owner: no new I/O worker and no polling loop.
 * While a Provider thread owns a pending callback, the CNet Client must stay
 * alive and the owner performs at most one bounded idle wait per terminal. */
static void init_wake_client(cnet_client *client) {
    cnet_client_config config={0};
    config.backend=wake_backend();
    config.connection_capacity=2u;
    config.command_capacity=8u;
    config.request_capacity=8u;
    config.completion_batch_capacity=4u;
    config.event_capacity=8u;
    config.max_send_bytes=1024u;
    config.receive_buffer_bytes=1024u;
    assert(cnet_client_init(client,&config)==SALTS_OK);
}
static void advance_after_provider_wake(cnet_client *client) {
    cnet_progress_strategy strategy=CNET_PROGRESS_STRATEGY_INIT;
    size_t events=0u;
    /* One native OS wake (or an already recorded coalesced wake) terminates
     * this finite Owner wait. No timed retry of a Provider side effect. */
    strategy.idle_wait_ms=1000u;
    assert(cnet_client_poll_strategy(client,&strategy,1000u,&events)
           ==SALTS_OK);
    (void)events;
}
static tm_pc_portal *make_portal(uint64_t generation) {
    tm_pc_config_v1 c=TM_PC_CONFIG_V1_INIT;
    tm_pc_portal *portal=NULL;
    c.capacity=2u;c.provider_generation=generation;
    assert(tm_pc_create(&c,&portal)==TM_PC_OK);
    return portal;
}
/* Real Salts Provider terminal acceptance must cross the existing
 * StreamGraph Media Owner, NOT a second scheduler or direct test-local take. */
typedef struct terminal_owner {
    tm_sg_program *program;
    tm_fs_store *store;
    tm_ex_owner *executor;
    tm_ho_inbox *inbox;
    tm_od_owner *dispatcher;
    uint64_t expected_correlation;
    uint64_t expected_generation;
    uint32_t expected_outcome;
    int32_t expected_status;
    unsigned delivered;
    unsigned reject_once;
    unsigned rejections;
} terminal_owner;

static tm_ex_source_result unused_source(
    void *ctx,uint8_t *dst,size_t cap,size_t *size) {
    (void)ctx;(void)dst;(void)cap;(void)size;
    return TM_EX_SOURCE_EOS;
}
static int unused_visit(void *ctx,const tm_fs_frame_ref_v1 *ref,
                        const tm_fs_cpu_view_v1 *frame) {
    (void)ctx;(void)ref;(void)frame;
    return 0;
}
static tm_sg_port_v1 owner_port(const char *name,uint32_t direction) {
    tm_sg_port_v1 p=TM_SG_PORT_V1_INIT;
    p.name=name; p.direction=direction;p.kind=TM_SG_VIDEO_FRAME;
    p.format_id=1u;p.memory_domain=TM_SG_MEMORY_CPU;
    p.timebase_num=1u;p.timebase_den=30u;
    return p;
}
static int accept_terminal(void *ctx,const tm_pc_result_v1 *terminal) {
    terminal_owner *o=(terminal_owner *)ctx;
    tm_pc_result_v1 recursive=TM_PC_RESULT_V1_INIT;
    assert(terminal->correlation==o->expected_correlation);
    assert(terminal->provider_generation==o->expected_generation);
    assert(terminal->outcome==o->expected_outcome);
    assert(terminal->provider_status==o->expected_status);
    assert(terminal->completion_observed==1u);
    assert(tm_od_step_completion(o->dispatcher,&recursive)==TM_OD_EBUSY);
    ++o->delivered;
    if (o->reject_once) {
        o->reject_once=0u;
        ++o->rejections;
        return -1; /* External acceptance is ambiguous: retain the credit. */
    }
    return 0; /* Downstream accepted exactly one terminal result. */
}
static void owner_init(terminal_owner *o,tm_pc_portal *portal,uint64_t store_id) {
    tm_sg_port_v1 source_port=owner_port("out",TM_SG_OUTPUT);
    tm_sg_port_v1 processor_ports[2]={
        {0}, {0}
    };
    tm_sg_port_v1 sink_port=owner_port("in",TM_SG_INPUT);
    tm_sg_node_v1 nodes[3];
    tm_sg_edge_v1 edges[2];
    tm_fs_config_v1 fs=TM_FS_CONFIG_V1_INIT;
    tm_ex_config_v1 ec=TM_EX_CONFIG_V1_INIT;
    tm_ex_binding_v1 binding[3];
    tm_ho_config_v1 queue=TM_HO_CONFIG_V1_INIT;
    tm_od_config_v1 dc=TM_OD_CONFIG_V1_INIT;
    const char *names[3]={"source","processor","sink"};
    uint32_t i;
    processor_ports[0]=owner_port("in",TM_SG_INPUT);
    processor_ports[1]=owner_port("out",TM_SG_OUTPUT);
    for(i=0u;i<3u;++i) {
        nodes[i]=(tm_sg_node_v1)TM_SG_NODE_V1_INIT;
        nodes[i].id=names[i];
        nodes[i].kind=i==0u?TM_SG_SOURCE:
                      i==1u?TM_SG_PROCESSOR:TM_SG_SINK;
        nodes[i].port_count=i==1u?2u:1u;
        nodes[i].ports=i==0u?&source_port:
                       i==1u?processor_ports:&sink_port;
        binding[i]=(tm_ex_binding_v1)TM_EX_BINDING_V1_INIT;
        binding[i].node_id=names[i];
        binding[i].ctx=NULL;
    }
    edges[0]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
    edges[0].source_node="source";edges[0].source_port="out";
    edges[0].target_node="processor";edges[0].target_port="in";
    edges[1]=(tm_sg_edge_v1)TM_SG_EDGE_V1_INIT;
    edges[1].source_node="processor";edges[1].source_port="out";
    edges[1].target_node="sink";edges[1].target_port="in";
    assert(tm_sg_compile(nodes,3u,edges,2u,NULL,&o->program,NULL)==TM_SG_OK);
    fs.store_id=store_id;
    fs.slot_capacity=2u;fs.max_frame_bytes=16u;
    fs.max_pins=4u;fs.max_leases=4u;fs.max_retained_bytes=32u;
    assert(tm_fs_create(&fs,&o->store)==TM_FS_OK);
    ec.max_frame_bytes=16u;
    ec.source_session_generation=1u;
    ec.display_generation=1u;
    ec.fanout.branch_count=1u;
    ec.fanout.branches[0]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
    ec.fanout.branches[0].policy=TM_FO_ORDERED_LOSSLESS;
    ec.fanout.branches[0].queue_depth=1u;
    binding[0].source_next=unused_source;
    binding[1].visit=unused_visit;
    binding[2].visit=unused_visit;
    assert(tm_ex_create(o->program,o->store,&ec,binding,3u,&o->executor)
           ==TM_EX_OK);
    queue.capacity=2u;
    assert(tm_ho_create(&queue,&o->inbox)==TM_HO_OK);
    assert(tm_od_create(o->inbox,o->executor,o->store,&dc,&o->dispatcher)
           ==TM_OD_OK);
    assert(tm_od_bind_completion(o->dispatcher,portal,accept_terminal,o)
           ==TM_OD_OK);
}
static void owner_drain(terminal_owner *o) {
    assert(tm_ho_seal(o->inbox)==TM_HO_OK);
    assert(tm_od_destroy(o->dispatcher)==TM_OD_OK);
    assert(tm_ho_destroy(o->inbox)==TM_HO_OK);
    assert(tm_ex_stop(o->executor)==TM_EX_OK);
    assert(tm_ex_destroy(o->executor)==TM_EX_OK);
    assert(tm_fs_seal(o->store)==TM_FS_OK);
    assert(tm_fs_destroy(o->store)==TM_FS_OK);
    tm_sg_program_destroy(o->program);
}
static void settle_terminal(terminal_owner *o,uint64_t correlation,
                            uint32_t outcome,int32_t status,
                            uint64_t generation) {
    tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
    o->expected_correlation=correlation;
    o->expected_outcome=outcome;
    o->expected_status=status;
    o->expected_generation=generation;
    assert(tm_od_step_completion(o->dispatcher,&result)==TM_OD_OK);
    assert(result.correlation==correlation);
    assert(result.outcome==outcome);
    assert(result.provider_generation==generation);
    assert(o->delivered==1u);
    assert(tm_od_step_completion(o->dispatcher,&result)==TM_OD_EMPTY);
}


/* An ambiguous external result never gets an implicit retry. The real
 * ComponentPlugin Scope stays live while the Media Owner's terminal credit
 * remains TAKEN. Host reconciliation is explicit before Scope release. */
static void reconcile_cancelled_terminal(
    terminal_owner *o, salts_component_plugin_runtime *runtime,
    salts_component_plugin_generation *generation,
    cmeta_plugin_registry *registry, cmeta_plugin_ref plugin) {
    tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
    o->expected_correlation=501u;
    o->expected_outcome=TM_PC_CANCELLED;
    o->expected_status=101;
    o->expected_generation=101u;
    o->reject_once=1u;
    assert(tm_ho_seal(o->inbox)==TM_HO_OK);
    assert(tm_od_step_completion(o->dispatcher,&result)==TM_OD_EDELIVERY);
    assert(result.correlation==501u && result.outcome==TM_PC_CANCELLED);
    assert(o->delivered==1u && o->rejections==1u);
    assert(tm_od_pending_completion(o->dispatcher,&result)==TM_OD_OK);
    assert(result.correlation==501u && result.provider_generation==101u);
    assert(tm_od_step_completion(o->dispatcher,&result)==TM_OD_EBUSY);
    assert(o->delivered==1u); /* No duplicated native effect. */
    assert(tm_od_destroy(o->dispatcher)==TM_OD_EBUSY);
    assert(salts_component_plugin_generation_drain(runtime,generation)
           ==SALTS_COMPONENT_PLUGIN_BUSY);
    assert(cmeta_plugin_registry_unload(registry,plugin)==CMETA_PLUGIN_BUSY);
    /* The Host has explicitly reconciled the ambiguous downstream effect;
     * settlement does not claim success or call the provider a second time. */
    assert(tm_od_settle_pending_completion(o->dispatcher)==TM_OD_OK);
    assert(tm_od_settle_pending_completion(o->dispatcher)==TM_OD_EMPTY);
    assert(tm_od_pending_completion(o->dispatcher,&result)==TM_OD_EMPTY);
    assert(o->delivered==1u);
 }

static void drain_plugin(cmeta_plugin_registry *registry,cmeta_plugin_ref ref) {
    cmeta_plugin_lifecycle_info info;
    bool quiet=false;
    assert(cmeta_plugin_registry_get_lifecycle(registry,ref,&info)==CMETA_PLUGIN_OK);
    assert(info.active_leases==0u);
    assert(cmeta_plugin_registry_request_stop(registry,ref)==CMETA_PLUGIN_OK);
    assert(cmeta_plugin_registry_poll_quiescent(registry,ref,&quiet)==CMETA_PLUGIN_OK);
    assert(quiet);
    assert(cmeta_plugin_registry_unload(registry,ref)==CMETA_PLUGIN_OK);
}

int main(int argc,char **argv) {
    const cmeta_plugin_registry_config registry_config={2};
    cmeta_plugin_registry registry={0};
    cmeta_plugin_ref ref_n={0},ref_n1={0};
    generation_fixture n={0},n1={0};
    salts_component_plugin_runtime runtime=SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *previous=NULL;
    salts_component_plugin_scope scope_n=SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    salts_component_plugin_scope scope_n1=SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    salts_component_plugin_scope copied=SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    tm_test_provider value_n,value_n1;
    tm_pc_portal *portal_n,*portal_n1;
    terminal_owner delivery_n={0},delivery_n1={0};
    provider_worker work_n={0},work_n1={0};
    worker_thread thread_n,thread_n1;
    cmeta_plugin_lifecycle_info info;
    tm_pc_snapshot_v1 snapshot=TM_PC_SNAPSHOT_V1_INIT;
    cnet_client wake_client={0};

    assert(argc==3);
    init_wake_client(&wake_client);
    assert(cmeta_plugin_registry_init(&registry,&registry_config)==CMETA_PLUGIN_OK);
    assert(cmeta_plugin_registry_load(&registry,argv[1],&ref_n)==CMETA_PLUGIN_OK);
    assert(cmeta_plugin_registry_start(&registry,ref_n)==CMETA_PLUGIN_OK);
    assert(cmeta_plugin_registry_load(&registry,argv[2],&ref_n1)==CMETA_PLUGIN_OK);
    assert(cmeta_plugin_registry_start(&registry,ref_n1)==CMETA_PLUGIN_OK);
    assert(build(&n,101u,&registry,ref_n)==SALTS_COMPONENT_PLUGIN_OK);
    assert(build(&n1,102u,&registry,ref_n1)==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_runtime_init(&runtime)==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_runtime_publish(&runtime,&n.generation,&previous)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(previous==NULL);
    assert(salts_component_plugin_scope_acquire(&runtime,&scope_n)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_scope_generation_id(&scope_n)==101u);
    value_n=find_provider(&scope_n);
    copied=scope_n;
    assert(salts_component_plugin_scope_generation_id(&copied)==0u);
    assert(salts_component_plugin_scope_release(&copied)
           ==SALTS_COMPONENT_PLUGIN_INVALID_STATE);

    portal_n=make_portal(101u);
    owner_init(&delivery_n,portal_n,10101u);
    assert(tm_pc_begin(portal_n,501u,&work_n.ticket)==TM_PC_OK);
    work_n.service=value_n;
    work_n.completion=portal_n;
    work_n.wake_owner=&wake_client;
    atomic_init(&work_n.returned,0u);
    start_worker(&thread_n,&work_n);
    wait_entered(&value_n);

    /* Real DSO N method is still on the worker's native call stack.
     * Close its admission by publishing N+1 without revoking its Scope. */
    assert(salts_component_plugin_runtime_publish(
               &runtime,&n1.generation,&previous)==SALTS_COMPONENT_PLUGIN_OK);
    assert(previous==&n.generation);
    assert(salts_component_plugin_generation_drain(&runtime,&n.generation)
           ==SALTS_COMPONENT_PLUGIN_BUSY);
    assert(cmeta_plugin_registry_get_lifecycle(&registry,ref_n,&info)
           ==CMETA_PLUGIN_OK);
    assert(info.active_leases==1u);
    assert(cmeta_plugin_registry_request_stop(&registry,ref_n)==CMETA_PLUGIN_OK);
    assert(cmeta_plugin_registry_unload(&registry,ref_n)==CMETA_PLUGIN_BUSY);
    assert(tm_pc_request_cancel(portal_n,work_n.ticket)==TM_PC_OK);
    assert(tm_pc_seal(portal_n)==TM_PC_OK);
    assert(tm_pc_destroy(portal_n)==TM_PC_EBUSY);

    /* Run N+1 independently, complete its own Portal and release its Scope. */
    assert(salts_component_plugin_scope_acquire(&runtime,&scope_n1)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_scope_generation_id(&scope_n1)==102u);
    value_n1=find_provider(&scope_n1);
    portal_n1=make_portal(102u);
    owner_init(&delivery_n1,portal_n1,10202u);
    assert(tm_pc_begin(portal_n1,601u,&work_n1.ticket)==TM_PC_OK);
    work_n1.service=value_n1;
    work_n1.completion=portal_n1;
    work_n1.wake_owner=&wake_client;
    atomic_init(&work_n1.returned,0u);
    start_worker(&thread_n1,&work_n1);
    wait_entered(&value_n1);
    assert(tm_test_provider_release(&value_n1)==0);
    advance_after_provider_wake(&wake_client);
    join_worker(&thread_n1);
    assert(work_n1.wake_status==SALTS_OK);
    assert(atomic_load_explicit(&work_n1.returned,memory_order_acquire)==1u);
    assert(work_n1.result==102);
    assert(tm_pc_seal(portal_n1)==TM_PC_OK);
    settle_terminal(&delivery_n1,601u,TM_PC_COMPLETED,102,102u);
    owner_drain(&delivery_n1);
    assert(tm_pc_destroy(portal_n1)==TM_PC_OK);

    assert(salts_component_plugin_runtime_close(&runtime,&previous)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(previous==&n1.generation);
    assert(salts_component_plugin_generation_drain(&runtime,&n1.generation)
           ==SALTS_COMPONENT_PLUGIN_BUSY);
    assert(salts_component_plugin_scope_release(&scope_n1)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_generation_drain(&runtime,&n1.generation)
           ==SALTS_COMPONENT_PLUGIN_OK);
    drain_plugin(&registry,ref_n1);

    /* N is still on the DSO call stack, cancellation alone is not unload. */
    assert(atomic_load_explicit(&work_n.returned,memory_order_acquire)==0u);
    assert(salts_component_plugin_generation_drain(&runtime,&n.generation)
           ==SALTS_COMPONENT_PLUGIN_BUSY);
    assert(cmeta_plugin_registry_unload(&registry,ref_n)==CMETA_PLUGIN_BUSY);
    assert(tm_pc_snapshot(portal_n,&snapshot)==TM_PC_OK);
    assert(snapshot.cancel_pending==1u && !snapshot.drained);
    assert(tm_test_provider_release(&value_n)==0);
    advance_after_provider_wake(&wake_client);
    join_worker(&thread_n);
    assert(work_n.wake_status==SALTS_OK);
    assert(work_n.result==101);
    assert(tm_pc_destroy(portal_n)==TM_PC_EBUSY);
    reconcile_cancelled_terminal(&delivery_n,&runtime,&n.generation,
                                 &registry,ref_n);
    {
        tm_pc_snapshot_v1 after=TM_PC_SNAPSHOT_V1_INIT;
        assert(tm_pc_snapshot(portal_n,&after)==TM_PC_OK && after.drained);
    }
    owner_drain(&delivery_n);
    assert(tm_pc_destroy(portal_n)==TM_PC_OK);
    assert(salts_component_plugin_generation_drain(&runtime,&n.generation)
           ==SALTS_COMPONENT_PLUGIN_BUSY);
    assert(salts_component_plugin_scope_release(&scope_n)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_generation_drain(&runtime,&n.generation)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_runtime_destroy(&runtime)==SALTS_COMPONENT_PLUGIN_OK);
    /* STOP was requested while N's worker was active; after Scope/drain
     * release, actual quiescence and unload now succeed. */
    {
        bool quiet=false;
        assert(cmeta_plugin_registry_poll_quiescent(&registry,ref_n,&quiet)==CMETA_PLUGIN_OK);
        assert(quiet);
    }
    assert(cmeta_plugin_registry_unload(&registry,ref_n)==CMETA_PLUGIN_OK);
    assert(cmeta_plugin_registry_destroy(&registry)==CMETA_PLUGIN_OK);
    /* Wake callers have joined and no callback/wake tail can use this
     * client. The Owner alone performs terminal CNet stop and destroy. */
    assert(cnet_client_stop(&wake_client,1000u)==SALTS_OK);
    assert(cnet_client_destroy(&wake_client)==SALTS_OK);
    puts("installed Salts ComponentPlugin true N->N+1 DSO cancellation/drain: PASS");
    return 0;
}
