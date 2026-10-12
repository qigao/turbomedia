#include <turbo_streamgraph_provider_completion.h>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
static void pause_thread(void) { Sleep(0); }
#else
#include <pthread.h>
#include <sched.h>
static void pause_thread(void) { sched_yield(); }
#endif

static tm_pc_portal *new_portal(uint32_t capacity,uint64_t generation) {
    tm_pc_config_v1 cfg=TM_PC_CONFIG_V1_INIT;
    tm_pc_portal *portal=NULL;
    cfg.capacity=capacity;
    cfg.provider_generation=generation;
    assert(tm_pc_create(&cfg,&portal)==TM_PC_OK);
    return portal;
}
static tm_pc_result_v1 take_one(tm_pc_portal *p,uint64_t correlation) {
    tm_pc_ticket_v1 taken={0};
    tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
    assert(tm_pc_take(p,&taken,&result)==TM_PC_OK);
    assert(result.correlation==correlation);
    assert(tm_pc_settle(p,taken)==TM_PC_OK);
    assert(tm_pc_settle(p,taken)==TM_PC_ESTALE);
    return result;
}
static void test_terminal_and_seal(void) {
    tm_pc_portal *p=new_portal(2u,53u),*other;
    tm_pc_ticket_v1 a={0},b={0},c={0};
    tm_pc_snapshot_v1 snap=TM_PC_SNAPSHOT_V1_INIT;
    tm_pc_result_v1 result;
    assert(tm_pc_begin(p,11u,&a)==TM_PC_OK);
    assert(tm_pc_begin(p,12u,&b)==TM_PC_OK);
    assert(tm_pc_begin(p,13u,&c)==TM_PC_EFULL);
    assert(!c.incarnation);
    assert(tm_pc_fail_start(p,a,-11)==TM_PC_OK);
    assert(tm_pc_complete(p,a,0)==TM_PC_EALREADY);
    assert(tm_pc_request_cancel(p,b)==TM_PC_OK);
    assert(tm_pc_request_cancel(p,b)==TM_PC_EALREADY);
    assert(tm_pc_snapshot(p,&snap)==TM_PC_OK);
    assert(snap.ready==1u && snap.cancel_pending==1u && !snap.drained);
    assert(tm_pc_seal(p)==TM_PC_OK);
    assert(tm_pc_begin(p,14u,&c)==TM_PC_ESHUTDOWN);
    assert(tm_pc_destroy(p)==TM_PC_EBUSY);
    /* Provider callback is authorized to finish work admitted BEFORE seal. */
    assert(tm_pc_complete(p,b,201)==TM_PC_OK);
    assert(tm_pc_complete(p,b,999)==TM_PC_EALREADY);
    result=take_one(p,11u);
    assert(result.outcome==TM_PC_START_FAILED && result.provider_status==-11);
    assert(result.provider_generation==53u);
    result=take_one(p,12u);
    assert(result.outcome==TM_PC_CANCELLED && result.completion_observed==1u);
    assert(result.provider_status==201);
    assert(tm_pc_snapshot(p,&snap)==TM_PC_OK && snap.drained);
    assert(tm_pc_destroy(p)==TM_PC_OK);
    other=new_portal(1u,54u);
    assert(tm_pc_complete(other,a,0)==TM_PC_ESTALE);
    assert(tm_pc_seal(other)==TM_PC_OK);
    assert(tm_pc_destroy(other)==TM_PC_OK);
}
static void test_cancel_quiescence_and_completion_wins(void) {
    tm_pc_portal *p=new_portal(1u,99u);
    tm_pc_ticket_v1 a={0};
    tm_pc_result_v1 out;
    assert(tm_pc_begin(p,1u,&a)==TM_PC_OK);
    assert(tm_pc_request_cancel(p,a)==TM_PC_OK);
    assert(tm_pc_cancel_quiesced(p,a)==TM_PC_OK);
    assert(tm_pc_complete(p,a,0)==TM_PC_EALREADY);
    out=take_one(p,1u);
    assert(out.outcome==TM_PC_CANCELLED && out.completion_observed==0u);
    assert(tm_pc_begin(p,2u,&a)==TM_PC_OK);
    assert(tm_pc_complete(p,a,73)==TM_PC_OK);
    assert(tm_pc_request_cancel(p,a)==TM_PC_EALREADY);
    out=take_one(p,2u);
    assert(out.outcome==TM_PC_COMPLETED && out.completion_observed==1u);
    assert(out.provider_status==73);
    assert(tm_pc_seal(p)==TM_PC_OK);
    assert(tm_pc_destroy(p)==TM_PC_OK);
}
typedef struct pc_racer {
    tm_pc_portal *portal;
    tm_pc_ticket_v1 ticket;
    _Atomic uint32_t *go;
    tm_pc_status status;
    uint32_t cancel;
} pc_racer;
#ifdef _WIN32
static DWORD WINAPI race_thread(LPVOID arg)
#else
static void *race_thread(void *arg)
#endif
{
    pc_racer *r=(pc_racer *)arg;
    while (!atomic_load_explicit(r->go,memory_order_acquire))pause_thread();
    if (r->cancel)
        r->status=tm_pc_request_cancel(r->portal,r->ticket);
    else
        r->status=tm_pc_complete(r->portal,r->ticket,123);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void run_race(pc_racer *a,pc_racer *b) {
#ifdef _WIN32
    HANDLE handles[2];
    handles[0]=CreateThread(NULL,0,race_thread,a,0,NULL);
    handles[1]=CreateThread(NULL,0,race_thread,b,0,NULL);
    assert(handles[0] && handles[1]);
    atomic_store_explicit(a->go,1u,memory_order_release);
    assert(WaitForMultipleObjects(2,handles,TRUE,INFINITE)==WAIT_OBJECT_0);
    CloseHandle(handles[0]);
    CloseHandle(handles[1]);
#else
    pthread_t threads[2];
    assert(pthread_create(&threads[0],NULL,race_thread,a)==0);
    assert(pthread_create(&threads[1],NULL,race_thread,b)==0);
    atomic_store_explicit(a->go,1u,memory_order_release);
    assert(pthread_join(threads[0],NULL)==0);
    assert(pthread_join(threads[1],NULL)==0);
#endif
}
static void test_completion_cancel_thread_race(void) {
    tm_pc_portal *p=new_portal(4u,150u);
    uint32_t i,cancel_wins=0u,complete_wins=0u;
    for (i=0u;i<160u;++i) {
        tm_pc_ticket_v1 ticket={0},taken={0};
        tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
        _Atomic uint32_t go;
        pc_racer a,b;
        atomic_init(&go,0u);
        assert(tm_pc_begin(p,(uint64_t)i+1u,&ticket)==TM_PC_OK);
        a=(pc_racer){p,ticket,&go,TM_PC_EBUSY,1u};
        b=(pc_racer){p,ticket,&go,TM_PC_EBUSY,0u};
        run_race(&a,&b);
        assert(b.status==TM_PC_OK);
        assert(a.status==TM_PC_OK || a.status==TM_PC_EALREADY);
        assert(tm_pc_take(p,&taken,&result)==TM_PC_OK);
        assert(result.correlation==(uint64_t)i+1u);
        if (a.status==TM_PC_OK) {
            assert(result.outcome==TM_PC_CANCELLED);
            cancel_wins++;
        } else {
            assert(result.outcome==TM_PC_COMPLETED);
            complete_wins++;
        }
        assert(result.completion_observed==1u);
        assert(result.provider_status==123);
        assert(tm_pc_settle(p,taken)==TM_PC_OK);
        assert(tm_pc_take(p,&taken,&result)==TM_PC_EBUSY);
    }
    assert(cancel_wins+complete_wins==160u);
    assert(tm_pc_seal(p)==TM_PC_OK);
    assert(tm_pc_destroy(p)==TM_PC_OK);
}
#define CALLBACKS 4u
typedef struct pc_batch {
    tm_pc_portal *portal;
    tm_pc_ticket_v1 tickets[80];
    unsigned count;
    _Atomic uint32_t *ready;
} pc_batch;
#ifdef _WIN32
static DWORD WINAPI batch_thread(LPVOID arg)
#else
static void *batch_thread(void *arg)
#endif
{
    pc_batch *b=(pc_batch *)arg;
    unsigned i;
    while (!atomic_load_explicit(b->ready,memory_order_acquire))pause_thread();
    for (i=0u;i<b->count;++i)
        assert(tm_pc_complete(b->portal,b->tickets[i],(int32_t)i)==TM_PC_OK);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}
static void test_parallel_callback_concurrency(void) {
    tm_pc_portal *p=new_portal(320u,442u);
    pc_batch batches[CALLBACKS];
    _Atomic uint32_t ready;
    unsigned i,j,count=0u;
    unsigned char seen[320]={0};
#ifdef _WIN32
    HANDLE handles[CALLBACKS];
#else
    pthread_t threads[CALLBACKS];
#endif
    atomic_init(&ready,0u);
    for (i=0u;i<CALLBACKS;++i) {
        batches[i].portal=p;
        batches[i].count=80u;
        batches[i].ready=&ready;
        for (j=0u;j<80u;++j)
            assert(tm_pc_begin(p,(uint64_t)i*80u+j+1u,
                    &batches[i].tickets[j])==TM_PC_OK);
#ifdef _WIN32
        handles[i]=CreateThread(NULL,0,batch_thread,&batches[i],0,NULL);
        assert(handles[i]!=NULL);
#else
        assert(pthread_create(&threads[i],NULL,batch_thread,&batches[i])==0);
#endif
    }
    atomic_store_explicit(&ready,1u,memory_order_release);
    while (count<320u) {
        tm_pc_ticket_v1 ticket={0};
        tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
        tm_pc_status rc=tm_pc_take(p,&ticket,&result);
        if (rc==TM_PC_EBUSY) {pause_thread();continue;}
        assert(rc==TM_PC_OK);
        assert(result.correlation>=1u && result.correlation<=320u);
        assert(result.provider_generation==442u);
        assert(result.outcome==TM_PC_COMPLETED);
        assert(seen[result.correlation-1u]==0u);
        seen[result.correlation-1u]=1u;
        assert(tm_pc_settle(p,ticket)==TM_PC_OK);
        ++count;
    }
    for (i=0u;i<CALLBACKS;++i) {
#ifdef _WIN32
        assert(WaitForSingleObject(handles[i],INFINITE)==WAIT_OBJECT_0);
        CloseHandle(handles[i]);
#else
        assert(pthread_join(threads[i],NULL)==0);
#endif
    }
    for (i=0u;i<320u;++i) assert(seen[i]);
    assert(tm_pc_seal(p)==TM_PC_OK);
    assert(tm_pc_destroy(p)==TM_PC_OK);
}
int main(void) {
    test_terminal_and_seal();
    test_cancel_quiescence_and_completion_wins();
    test_completion_cancel_thread_race();
    test_parallel_callback_concurrency();
    puts("StreamGraph Provider completion portal: PASS");
    return 0;
}
