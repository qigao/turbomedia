#include <turbo_streamgraph_handoff.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void yield_now(void) { Sleep(0); }
#else
#include <pthread.h>
#include <sched.h>
static void yield_now(void) { sched_yield(); }
#endif

static tm_ho_command_v1 command(uint32_t kind, uint32_t branch, uint64_t id) {
    tm_ho_command_v1 c = TM_HO_COMMAND_V1_INIT;
    c.kind = kind;
    c.branch = branch;
    c.correlation = id;
    return c;
}
static tm_ho_inbox *create(uint32_t capacity) {
    tm_ho_inbox *q = NULL;
    tm_ho_config_v1 c = TM_HO_CONFIG_V1_INIT;
    c.capacity = capacity;
    assert(tm_ho_create(&c, &q) == TM_HO_OK);
    return q;
}
static void test_reserve_publish_take_settle(void) {
    tm_ho_inbox *q = create(2u), *again;
    tm_ho_ticket_v1 a={0}, b={0}, t={0}, reject={0};
    tm_ho_command_v1 cmd=command(TM_HO_PUMP_SINK,1u,77u), received={0};
    tm_ho_snapshot_v1 snap=TM_HO_SNAPSHOT_V1_INIT;
    assert(tm_ho_reserve(q,&a)==TM_HO_OK);
    assert(tm_ho_reserve(q,&b)==TM_HO_OK);
    assert(tm_ho_reserve(q,&reject)==TM_HO_EFULL);
    assert(reject.incarnation==0u);
    assert(tm_ho_publish(q,a,&cmd)==TM_HO_OK);
    assert(tm_ho_publish(q,a,&cmd)==TM_HO_EBUSY);
    assert(tm_ho_snapshot(q,&snap)==TM_HO_OK);
    assert(snap.reserved==1u && snap.ready==1u && snap.taken==0u);
    assert(tm_ho_take(q,&t,&received)==TM_HO_OK);
    assert(t.incarnation==a.incarnation && t.slot==a.slot);
    assert(received.kind==TM_HO_PUMP_SINK && received.branch==1u);
    assert(received.correlation==77u);
    assert(tm_ho_abort(q,t)==TM_HO_EBUSY);
    assert(tm_ho_destroy(q)==TM_HO_EBUSY);
    assert(tm_ho_seal(q)==TM_HO_OK);
    assert(tm_ho_publish(q,b,&cmd)==TM_HO_ESHUTDOWN);
    assert(tm_ho_abort(q,b)==TM_HO_OK);
    assert(tm_ho_settle(q,t)==TM_HO_OK);
    assert(tm_ho_settle(q,t)==TM_HO_ESTALE);
    assert(tm_ho_reserve(q,&reject)==TM_HO_ESHUTDOWN);
    assert(tm_ho_take(q,&reject,&received)==TM_HO_EBUSY);
    assert(tm_ho_snapshot(q,&snap)==TM_HO_OK);
    assert(snap.sealed==1u && snap.drained==1u);
    assert(tm_ho_destroy(q)==TM_HO_OK);
    again=create(2u);
    assert(tm_ho_settle(again,t)==TM_HO_ESTALE);
    assert(tm_ho_seal(again)==TM_HO_OK);
    assert(tm_ho_destroy(again)==TM_HO_OK);
}
static void test_weak_ref_is_not_a_pin(void) {
    tm_ho_inbox *q=create(1u);
    tm_ho_ticket_v1 t={0},taken={0};
    tm_ho_command_v1 c=command(TM_HO_FRAME_NOTICE,0u,4u),out={0};
    assert(tm_ho_reserve(q,&t)==TM_HO_OK);
    assert(tm_ho_publish(q,t,&c)==TM_HO_EINVAL);
    c.has_frame=1u;
    c.frame=(tm_fs_frame_ref_v1)TM_FS_FRAME_REF_V1_INIT;
    c.frame.store_id=5u;
    c.frame.store_generation=1u;
    c.frame.slot_generation=2u;
    c.frame.frame_id=55u;
    c.frame.source_session_generation=3u;
    c.frame.display_generation=4u;
    assert(tm_ho_publish(q,t,&c)==TM_HO_OK);
    assert(tm_ho_take(q,&taken,&out)==TM_HO_OK);
    assert(out.frame.frame_id==55u && out.has_frame==1u);
    assert(tm_ho_settle(q,taken)==TM_HO_OK);
    assert(tm_ho_seal(q)==TM_HO_OK);
    assert(tm_ho_destroy(q)==TM_HO_OK);
}
#define PRODUCERS 4u
#define PER_PRODUCER 150u
typedef struct producer_args {
    tm_ho_inbox *q;
    unsigned id;
    _Atomic unsigned *done;
} producer_args;

#ifdef _WIN32
static DWORD WINAPI producer_run(LPVOID ptr)
#else
static void *producer_run(void *ptr)
#endif
{
    producer_args *a=(producer_args *)ptr;
    unsigned i;
    for(i=0u;i<PER_PRODUCER;++i) {
        tm_ho_ticket_v1 t={0};
        tm_ho_command_v1 cmd=command(TM_HO_PUMP_SINK,a->id,(uint64_t)i+1u);
        tm_ho_status st;
        do {
            st=tm_ho_reserve(a->q,&t);
            if(st==TM_HO_EFULL)yield_now();
        } while(st==TM_HO_EFULL);
        assert(st==TM_HO_OK);
        assert(tm_ho_publish(a->q,t,&cmd)==TM_HO_OK);
    }
    atomic_fetch_add_explicit(a->done,1u,memory_order_release);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void test_concurrent_mpsc_integrity(void) {
    tm_ho_inbox *q=create(16u);
    _Atomic unsigned done;
    producer_args args[PRODUCERS];
    unsigned seen[PRODUCERS][PER_PRODUCER]={{0}};
    unsigned count=0u,i;
#ifdef _WIN32
    HANDLE threads[PRODUCERS]={0};
#else
    pthread_t threads[PRODUCERS];
#endif
    atomic_init(&done,0u);
    for(i=0u;i<PRODUCERS;++i) {
        args[i].q=q;
        args[i].id=i;
        args[i].done=&done;
#ifdef _WIN32
        threads[i]=CreateThread(NULL,0,producer_run,&args[i],0,NULL);
        assert(threads[i]!=NULL);
#else
        assert(pthread_create(&threads[i],NULL,producer_run,&args[i])==0);
#endif
    }
    while(count<PRODUCERS*PER_PRODUCER) {
        tm_ho_ticket_v1 ticket={0};
        tm_ho_command_v1 cmd={0};
        tm_ho_status st=tm_ho_take(q,&ticket,&cmd);
        if(st==TM_HO_EBUSY) { yield_now(); continue; }
        assert(st==TM_HO_OK);
        assert(cmd.kind==TM_HO_PUMP_SINK);
        assert(cmd.branch<PRODUCERS);
        assert(cmd.correlation>=1u && cmd.correlation<=PER_PRODUCER);
        assert(seen[cmd.branch][cmd.correlation-1u]==0u);
        seen[cmd.branch][cmd.correlation-1u]=1u;
        assert(tm_ho_settle(q,ticket)==TM_HO_OK);
        ++count;
    }
    for(i=0u;i<PRODUCERS;++i) {
#ifdef _WIN32
        assert(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0);
        CloseHandle(threads[i]);
#else
        assert(pthread_join(threads[i],NULL)==0);
#endif
    }
    assert(atomic_load_explicit(&done,memory_order_acquire)==PRODUCERS);
    for(i=0u;i<PRODUCERS;++i) {
        unsigned j;
        for(j=0u;j<PER_PRODUCER;++j)assert(seen[i][j]==1u);
    }
    {
        tm_ho_snapshot_v1 snap=TM_HO_SNAPSHOT_V1_INIT;
        assert(tm_ho_snapshot(q,&snap)==TM_HO_OK);
        assert(snap.drained==1u);
    }
    assert(tm_ho_seal(q)==TM_HO_OK);
    /* Producers are joined and the owner has returned from every call. */
    assert(tm_ho_destroy(q)==TM_HO_OK);
}
typedef struct close_args {
    tm_ho_inbox *q;
    _Atomic unsigned reserved;
    _Atomic unsigned progress;
} close_args;
#ifdef _WIN32
static DWORD WINAPI close_race_run(LPVOID ptr)
#else
static void *close_race_run(void *ptr)
#endif
{
    close_args *a=(close_args *)ptr;
    tm_ho_ticket_v1 ticket={0};
    tm_ho_command_v1 c=command(TM_HO_STOP,0u,0u);
    assert(tm_ho_reserve(a->q,&ticket)==TM_HO_OK);
    atomic_store_explicit(&a->reserved,1u,memory_order_release);
    while(!atomic_load_explicit(&a->progress,memory_order_acquire))yield_now();
    assert(tm_ho_publish(a->q,ticket,&c)==TM_HO_ESHUTDOWN);
    assert(tm_ho_abort(a->q,ticket)==TM_HO_OK);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void test_seal_vs_reserved_producer(void) {
    close_args a={0};
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    a.q=create(1u);
    atomic_init(&a.reserved,0u);
    atomic_init(&a.progress,0u);
#ifdef _WIN32
    thread=CreateThread(NULL,0,close_race_run,&a,0,NULL);
    assert(thread!=NULL);
#else
    assert(pthread_create(&thread,NULL,close_race_run,&a)==0);
#endif
    while(!atomic_load_explicit(&a.reserved,memory_order_acquire))yield_now();
    assert(tm_ho_seal(a.q)==TM_HO_OK);
    assert(tm_ho_destroy(a.q)==TM_HO_EBUSY);
    atomic_store_explicit(&a.progress,1u,memory_order_release);
#ifdef _WIN32
    assert(WaitForSingleObject(thread,INFINITE)==WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    assert(pthread_join(thread,NULL)==0);
#endif
    assert(tm_ho_destroy(a.q)==TM_HO_OK);
}
int main(void) {
    test_reserve_publish_take_settle();
    test_weak_ref_is_not_a_pin();
    test_concurrent_mpsc_integrity();
    test_seal_vs_reserved_producer();
    puts("StreamGraph owner inbox MPSC: PASS");
    return 0;
}
