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
typedef HMODULE tm_module;
typedef DWORD tm_thread_result;
#define TM_THREAD_CALL WINAPI
static void tm_yield(void) { Sleep(0); }
static tm_module tm_load(const char *path) { return LoadLibraryA(path); }
static void *tm_symbol(tm_module handle) {
    return (void *)(uintptr_t)GetProcAddress(handle,
                      "tm_streamgraph_test_provider_run");
}
static int tm_unload(tm_module module) { return FreeLibrary(module)!=0; }
#else
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
typedef void *tm_module;
typedef void *tm_thread_result;
#define TM_THREAD_CALL
static void tm_yield(void) { sched_yield(); }
static tm_module tm_load(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void *tm_symbol(tm_module handle) {
    return dlsym(handle,"tm_streamgraph_test_provider_run");
}
static int tm_unload(tm_module module) { return dlclose(module)==0; }
#endif

typedef uint32_t (*tm_provider_entry)(void (*work)(void *),void *);
typedef struct tm_dso_work {
    tm_provider_entry entry;
    tm_pc_portal *portal;
    tm_pc_ticket_v1 ticket;
    _Atomic unsigned active;
    _Atomic unsigned permit_complete;
    _Atomic unsigned finished;
    uint32_t returned_generation;
} tm_dso_work;

static void callback_on_dso_stack(void *opaque) {
    tm_dso_work *w=(tm_dso_work *)opaque;
    atomic_store_explicit(&w->active,1u,memory_order_release);
    while (!atomic_load_explicit(&w->permit_complete,memory_order_acquire))
        tm_yield();
    /* Callback is still executing on the DSO stack. It must never release
     * an outer generation Scope or cause DSO unload by itself. */
    assert(tm_pc_complete(w->portal,w->ticket,17)==TM_PC_OK);
}
static tm_thread_result TM_THREAD_CALL worker_entry(void *opaque) {
    tm_dso_work *w=(tm_dso_work *)opaque;
    w->returned_generation=w->entry(callback_on_dso_stack,w);
    atomic_store_explicit(&w->finished,1u,memory_order_release);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}
static tm_pc_portal *make_portal(uint64_t generation) {
    tm_pc_config_v1 cfg=TM_PC_CONFIG_V1_INIT;
    tm_pc_portal *portal=NULL;
    cfg.capacity=2u;
    cfg.provider_generation=generation;
    assert(tm_pc_create(&cfg,&portal)==TM_PC_OK);
    return portal;
}
static tm_provider_entry open_provider(const char *path,tm_module *out) {
    tm_provider_entry entry=NULL;
    void *symbol;
    *out=tm_load(path);
    assert(*out!=NULL);
    symbol=tm_symbol(*out);
    assert(symbol!=NULL);
    /* POSIX dlsym / Windows GetProcAddress document function lookup.
     * memcpy avoids a pedantic object-pointer -> function-pointer cast. */
    assert(sizeof(entry)==sizeof(symbol));
    memcpy(&entry,&symbol,sizeof(entry));
    return entry;
}
static void await_flag(_Atomic unsigned *flag) {
    while (!atomic_load_explicit(flag,memory_order_acquire)) tm_yield();
}
/* TEST HOST gate: this models a responsible native module owner but is NOT
 * Salts::ComponentPlugin's unload API. It proves why Portal drain and worker
 * quiescence are separate conditions. The exact Salts gate is an upstream
 * installed-SDK integration task. */
static int can_unload(tm_pc_portal *portal,int callbacks_joined) {
    tm_pc_snapshot_v1 snap=TM_PC_SNAPSHOT_V1_INIT;
    assert(tm_pc_snapshot(portal,&snap)==TM_PC_OK);
    return callbacks_joined && snap.sealed && snap.drained;
}
static void consume_terminal(tm_pc_portal *portal,uint64_t correlation,
                             uint32_t outcome,uint64_t generation) {
    tm_pc_ticket_v1 t={0};
    tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
    assert(tm_pc_take(portal,&t,&result)==TM_PC_OK);
    assert(result.correlation==correlation);
    assert(result.outcome==outcome);
    assert(result.provider_generation==generation);
    assert(result.completion_observed==1u);
    assert(tm_pc_settle(portal,t)==TM_PC_OK);
    assert(tm_pc_settle(portal,t)==TM_PC_ESTALE);
}
int main(int argc,char **argv) {
    tm_module old_mod=NULL,new_mod=NULL;
    tm_provider_entry old_entry,new_entry;
    tm_pc_portal *old_portal,*new_portal;
    tm_dso_work old_work={0},new_work={0};
    tm_pc_snapshot_v1 snapshot=TM_PC_SNAPSHOT_V1_INIT;
#if defined(_WIN32)
    HANDLE old_thread,new_thread;
#else
    pthread_t old_thread,new_thread;
#endif
    assert(argc==3);
    old_entry=open_provider(argv[1],&old_mod);
    new_entry=open_provider(argv[2],&new_mod);
    assert(old_mod && new_mod);
    old_portal=make_portal(101u);
    new_portal=make_portal(102u);
    old_work.entry=old_entry;
    old_work.portal=old_portal;
    new_work.entry=new_entry;
    new_work.portal=new_portal;
    atomic_init(&old_work.active,0u);
    atomic_init(&old_work.permit_complete,0u);
    atomic_init(&old_work.finished,0u);
    atomic_init(&new_work.active,0u);
    atomic_init(&new_work.permit_complete,0u);
    atomic_init(&new_work.finished,0u);
    assert(tm_pc_begin(old_portal,501u,&old_work.ticket)==TM_PC_OK);
#if defined(_WIN32)
    old_thread=CreateThread(NULL,0,worker_entry,&old_work,0,NULL);
    assert(old_thread!=NULL);
#else
    assert(pthread_create(&old_thread,NULL,worker_entry,&old_work)==0);
#endif
    await_flag(&old_work.active);
    /* Generation N stops admitting work, but the callback is still inside
     * its actual DSO and cancellation only marks a pending obligation. */
    assert(tm_pc_request_cancel(old_portal,old_work.ticket)==TM_PC_OK);
    assert(tm_pc_seal(old_portal)==TM_PC_OK);
    assert(tm_pc_destroy(old_portal)==TM_PC_EBUSY);
    assert(!can_unload(old_portal,0));
    assert(tm_pc_snapshot(old_portal,&snapshot)==TM_PC_OK);
    assert(snapshot.cancel_pending==1u && !snapshot.drained);
    /* Publish/execute N+1 without mutating or releasing the old DSO. */
    assert(tm_pc_begin(new_portal,601u,&new_work.ticket)==TM_PC_OK);
#if defined(_WIN32)
    new_thread=CreateThread(NULL,0,worker_entry,&new_work,0,NULL);
    assert(new_thread!=NULL);
#else
    assert(pthread_create(&new_thread,NULL,worker_entry,&new_work)==0);
#endif
    await_flag(&new_work.active);
    atomic_store_explicit(&new_work.permit_complete,1u,memory_order_release);
#if defined(_WIN32)
    assert(WaitForSingleObject(new_thread,INFINITE)==WAIT_OBJECT_0);
    CloseHandle(new_thread);
#else
    assert(pthread_join(new_thread,NULL)==0);
#endif
    assert(new_work.returned_generation==102u);
    assert(tm_pc_seal(new_portal)==TM_PC_OK);
    consume_terminal(new_portal,601u,TM_PC_COMPLETED,102u);
    assert(can_unload(new_portal,1));
    assert(tm_pc_destroy(new_portal)==TM_PC_OK);
    assert(tm_unload(new_mod));
    new_mod=NULL;
    /* N is still executing on the native DSO call stack and cannot unload.
     * A drain attempt before the callback's final completion is BUSY. */
    assert(!atomic_load_explicit(&old_work.finished,memory_order_acquire));
    assert(!can_unload(old_portal,0));
    assert(tm_pc_destroy(old_portal)==TM_PC_EBUSY);
    atomic_store_explicit(&old_work.permit_complete,1u,memory_order_release);
#if defined(_WIN32)
    assert(WaitForSingleObject(old_thread,INFINITE)==WAIT_OBJECT_0);
    CloseHandle(old_thread);
#else
    assert(pthread_join(old_thread,NULL)==0);
#endif
    assert(old_work.returned_generation==101u);
    /* Even after join, unconsumed terminal delivery holds a generation
     * obligation and must not be bypassed by unloading the code. */
    assert(!can_unload(old_portal,1));
    consume_terminal(old_portal,501u,TM_PC_CANCELLED,101u);
    assert(can_unload(old_portal,1));
    assert(tm_pc_destroy(old_portal)==TM_PC_OK);
    assert(tm_unload(old_mod));
    old_mod=NULL;
    puts("real DSO N->N+1 callback/cancel/unload host gate: PASS");
    return 0;
}
