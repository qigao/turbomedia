#include "provider_contract.h"
#include <salts/component_plugin.h>
#include <turbo_streamgraph_provider_completion.h>

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
    _Atomic unsigned returned;
    int result;
} provider_worker;
static thread_result THREAD_CALL invoke_provider(void *context) {
    provider_worker *worker=(provider_worker *)context;
    /* This is a REAL method call into the Salts-loaded Provider DSO. */
    worker->result=tm_test_provider_get(&worker->service);
    assert(tm_pc_complete(worker->completion,worker->ticket,worker->result)
           ==TM_PC_OK);
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
static tm_pc_portal *make_portal(uint64_t generation) {
    tm_pc_config_v1 c=TM_PC_CONFIG_V1_INIT;
    tm_pc_portal *portal=NULL;
    c.capacity=2u;c.provider_generation=generation;
    assert(tm_pc_create(&c,&portal)==TM_PC_OK);
    return portal;
}
static void settle_terminal(tm_pc_portal *p,uint64_t correlation,
                            uint32_t outcome,int32_t status,
                            uint64_t generation) {
    tm_pc_ticket_v1 taken={0};
    tm_pc_result_v1 terminal=TM_PC_RESULT_V1_INIT;
    assert(tm_pc_take(p,&taken,&terminal)==TM_PC_OK);
    assert(terminal.outcome==outcome && terminal.correlation==correlation);
    assert(terminal.provider_status==status);
    assert(terminal.provider_generation==generation);
    assert(terminal.completion_observed==1u);
    assert(tm_pc_settle(p,taken)==TM_PC_OK);
    assert(tm_pc_settle(p,taken)==TM_PC_ESTALE);
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
    provider_worker work_n={0},work_n1={0};
    worker_thread thread_n,thread_n1;
    cmeta_plugin_lifecycle_info info;
    tm_pc_snapshot_v1 snapshot=TM_PC_SNAPSHOT_V1_INIT;

    assert(argc==3);
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
    assert(tm_pc_begin(portal_n,501u,&work_n.ticket)==TM_PC_OK);
    work_n.service=value_n;
    work_n.completion=portal_n;
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
    assert(tm_pc_begin(portal_n1,601u,&work_n1.ticket)==TM_PC_OK);
    work_n1.service=value_n1;
    work_n1.completion=portal_n1;
    atomic_init(&work_n1.returned,0u);
    start_worker(&thread_n1,&work_n1);
    wait_entered(&value_n1);
    assert(tm_test_provider_release(&value_n1)==0);
    join_worker(&thread_n1);
    assert(atomic_load_explicit(&work_n1.returned,memory_order_acquire)==1u);
    assert(work_n1.result==102);
    assert(tm_pc_seal(portal_n1)==TM_PC_OK);
    settle_terminal(portal_n1,601u,TM_PC_COMPLETED,102,102u);
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
    join_worker(&thread_n);
    assert(work_n.result==101);
    assert(tm_pc_destroy(portal_n)==TM_PC_EBUSY);
    settle_terminal(portal_n,501u,TM_PC_CANCELLED,101,101u);
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
    puts("installed Salts ComponentPlugin true N->N+1 DSO cancellation/drain: PASS");
    return 0;
}
