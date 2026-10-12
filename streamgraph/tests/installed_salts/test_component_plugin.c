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
#include <stdlib.h>
#include <string.h>
#ifdef TM_TEST_FFMPEG_DSO
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#endif

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

#ifdef TM_TEST_FFMPEG_DSO
enum { TM_LUMA_BYTES=16*16, TM_ENCODED_CAPACITY=65536 };
typedef struct tm_mjpeg_encoder_fixture {
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
} tm_mjpeg_encoder_fixture;
static void tm_mjpeg_encoder_init(tm_mjpeg_encoder_fixture *e) {
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    assert(codec!=NULL);
    e->codec=avcodec_alloc_context3(codec);
    assert(e->codec);
    e->codec->width=16;e->codec->height=16;
    e->codec->pix_fmt=AV_PIX_FMT_YUVJ420P;
    e->codec->color_range=AVCOL_RANGE_JPEG;
    e->codec->time_base=(AVRational){1,30};
    e->codec->thread_count=1;
    assert(avcodec_open2(e->codec,codec,NULL)==0);
    e->frame=av_frame_alloc();e->packet=av_packet_alloc();
    assert(e->frame && e->packet);
    e->frame->width=16;e->frame->height=16;
    e->frame->format=AV_PIX_FMT_YUVJ420P;
    assert(av_frame_get_buffer(e->frame,32)==0);
}
static size_t tm_make_mjpeg(tm_mjpeg_encoder_fixture *e,unsigned luma,
                             uint8_t *out,size_t capacity) {
    size_t n;
    int y;
    assert(av_frame_make_writable(e->frame)==0);
    for(y=0;y<16;++y)
        memset(e->frame->data[0]+(size_t)y*e->frame->linesize[0],(int)luma,16u);
    for(y=0;y<8;++y) {
        memset(e->frame->data[1]+(size_t)y*e->frame->linesize[1],128,8u);
        memset(e->frame->data[2]+(size_t)y*e->frame->linesize[2],128,8u);
    }
    e->frame->pts++;
    assert(avcodec_send_frame(e->codec,e->frame)==0);
    assert(avcodec_receive_packet(e->codec,e->packet)==0);
    n=(size_t)e->packet->size;
    assert(n>0u && n<=capacity);
    memcpy(out,e->packet->data,n);
    av_packet_unref(e->packet);
    return n;
}
static void tm_mjpeg_encoder_destroy(tm_mjpeg_encoder_fixture *e) {
    av_frame_free(&e->frame);
    av_packet_free(&e->packet);
    avcodec_free_context(&e->codec);
}
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
#ifdef TM_TEST_FFMPEG_DSO
    uint8_t compressed[TM_ENCODED_CAPACITY];
    size_t compressed_size;
    uint8_t luma[TM_LUMA_BYTES];
    size_t luma_bytes;
#endif
} provider_worker;
static thread_result THREAD_CALL invoke_provider(void *context) {
    provider_worker *worker=(provider_worker *)context;
    /* This is a REAL method call into the Salts-loaded Provider DSO. */
#ifdef TM_TEST_FFMPEG_DSO
    tm_test_decode_request job={0};
    job.size=sizeof(job);
    job.packet=worker->compressed;
    job.packet_size=worker->compressed_size;
    job.portal=worker->completion;
    job.ticket=worker->ticket;
    job.wake_client=worker->wake_owner;
    /* The actual tm_ffa_run_mjpeg inside this Plugin DSO publishes and
     * wakes; the wrapper must NOT complete or wake a second time. */
    worker->result=tm_test_provider_run_decode(&worker->service,&job);
#else
    worker->result=tm_test_provider_get(&worker->service);
    assert(tm_pc_complete(worker->completion,worker->ticket,worker->result)
           ==TM_PC_OK);
    worker->wake_status=cnet_client_wake(worker->wake_owner);
#endif
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
#ifdef TM_TEST_FFMPEG_DSO
    provider_worker *decoded;
    tm_test_provider service;
    unsigned frame_ready,frame_visited,frame_sink_visited;
#endif
} terminal_owner;

#ifdef TM_TEST_FFMPEG_DSO
static tm_ex_source_result owner_decoded_source(
    void *ctx,uint8_t *dst,size_t cap,size_t *size) {
    terminal_owner *o=(terminal_owner *)ctx;
    if(!o || !o->decoded || !o->frame_ready || !dst || !size ||
       cap<TM_LUMA_BYTES)return TM_EX_SOURCE_FAILED;
    memcpy(dst,o->decoded->luma,TM_LUMA_BYTES);
    *size=TM_LUMA_BYTES;
    o->frame_ready=0u; /* one copy to Executor, never call DSO twice */
    return TM_EX_SOURCE_FRAME;
}
static int verify_decoded_frame(void *ctx,const tm_fs_frame_ref_v1 *ref,
                                const tm_fs_cpu_view_v1 *frame) {
    terminal_owner *o=(terminal_owner *)ctx;
    unsigned i;
    if(!o || !o->decoded || !ref || !ref->frame_id || !frame ||
       frame->size_bytes!=TM_LUMA_BYTES || !frame->data)return -1;
    for(i=0u;i<TM_LUMA_BYTES;++i)
        if(frame->data[i]!=o->decoded->luma[i])return -1;
    /* one read-only Processor visit and one Sink visit */
    if(o->frame_visited==0u)o->frame_visited++;
    else o->frame_sink_visited++;
    return 0;
}
#else
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
#endif
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
#ifdef TM_TEST_FFMPEG_DSO
    assert(o->decoded!=NULL);
    {
        tm_test_take_request request={0};
        request.size=sizeof(request);
        request.ticket=o->decoded->ticket;
        request.luma=o->decoded->luma;
        request.capacity=sizeof(o->decoded->luma);
        request.out_bytes=&o->decoded->luma_bytes;
        assert(tm_test_provider_take_luma(&o->service,&request)==TM_LUMA_BYTES);
        assert(o->decoded->luma_bytes==TM_LUMA_BYTES);
    }
    o->frame_ready=1u;
    assert(tm_ex_pump_source(o->executor)==TM_EX_OK);
    assert(tm_ex_pump_sink(o->executor,0u)==TM_EX_OK);
    assert(o->frame_visited==1u && o->frame_sink_visited==1u);
#endif
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
    fs.slot_capacity=2u;
#ifdef TM_TEST_FFMPEG_DSO
    fs.max_frame_bytes=TM_LUMA_BYTES;
#else
    fs.max_frame_bytes=16u;
#endif
    fs.max_pins=4u;fs.max_leases=4u;
#ifdef TM_TEST_FFMPEG_DSO
    fs.max_retained_bytes=2u*TM_LUMA_BYTES;
#else
    fs.max_retained_bytes=32u;
#endif
    assert(tm_fs_create(&fs,&o->store)==TM_FS_OK);
    ec.max_frame_bytes=fs.max_frame_bytes;
    ec.source_session_generation=1u;
    ec.display_generation=1u;
    ec.fanout.branch_count=1u;
    ec.fanout.branches[0]=(tm_fo_branch_config_v1)TM_FO_BRANCH_CONFIG_V1_INIT;
    ec.fanout.branches[0].policy=TM_FO_ORDERED_LOSSLESS;
    ec.fanout.branches[0].queue_depth=1u;
#ifdef TM_TEST_FFMPEG_DSO
    binding[0].source_next=owner_decoded_source;
    binding[0].ctx=o;
    binding[1].visit=verify_decoded_frame;binding[1].ctx=o;
    binding[2].visit=verify_decoded_frame;binding[2].ctx=o;
#else
    binding[0].source_next=unused_source;
    binding[1].visit=unused_visit;
    binding[2].visit=unused_visit;
#endif
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
#ifdef TM_TEST_FFMPEG_DSO
    o->expected_status=TM_FF_OK;
#else
    o->expected_status=101;
#endif
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
#ifdef TM_TEST_FFMPEG_DSO
    tm_mjpeg_encoder_fixture encoder={0};
#endif

    assert(argc==3);
#ifdef TM_TEST_FFMPEG_DSO
    tm_mjpeg_encoder_init(&encoder);
#endif
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
#ifdef TM_TEST_FFMPEG_DSO
    work_n.compressed_size=tm_make_mjpeg(
        &encoder,101u,work_n.compressed,sizeof(work_n.compressed));
    delivery_n.decoded=&work_n;
    delivery_n.service=value_n;
#endif
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
#ifdef TM_TEST_FFMPEG_DSO
    work_n1.compressed_size=tm_make_mjpeg(
        &encoder,102u,work_n1.compressed,sizeof(work_n1.compressed));
    delivery_n1.decoded=&work_n1;
    delivery_n1.service=value_n1;
#endif
    atomic_init(&work_n1.returned,0u);
    start_worker(&thread_n1,&work_n1);
    wait_entered(&value_n1);
    assert(tm_test_provider_release(&value_n1)==0);
    advance_after_provider_wake(&wake_client);
    join_worker(&thread_n1);
#ifdef TM_TEST_FFMPEG_DSO
    assert(tm_test_provider_wake_status(&value_n1)==SALTS_OK);
#else
    assert(work_n1.wake_status==SALTS_OK);
#endif
    assert(atomic_load_explicit(&work_n1.returned,memory_order_acquire)==1u);
    assert(work_n1.result==102);
    assert(tm_pc_seal(portal_n1)==TM_PC_OK);
#ifdef TM_TEST_FFMPEG_DSO
    settle_terminal(&delivery_n1,601u,TM_PC_COMPLETED,TM_FF_OK,102u);
    /* Only Media Owner's accepted terminal may retrieve the decoded bytes
     * through the real CMeta Provider method. No premature cross-thread read. */
    assert(work_n1.luma_bytes==TM_LUMA_BYTES);
    assert(abs((int)work_n1.luma[0]-102)<=7);
    assert(work_n1.luma[0]==work_n1.luma[TM_LUMA_BYTES-1u]);
#else
    settle_terminal(&delivery_n1,601u,TM_PC_COMPLETED,102,102u);
#endif
    owner_drain(&delivery_n1);
    assert(tm_pc_destroy(portal_n1)==TM_PC_OK);
#ifdef TM_TEST_FFMPEG_DSO
    assert(tm_test_provider_finish_decode(&value_n1)==0);
#endif

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
#ifdef TM_TEST_FFMPEG_DSO
    assert(tm_test_provider_wake_status(&value_n)==SALTS_OK);
#else
    assert(work_n.wake_status==SALTS_OK);
#endif
    assert(work_n.result==101);
#ifdef TM_TEST_FFMPEG_DSO
    /* Old Generation decoded into its private decoder object but cannot
     * transfer or publish the pixels of a cancelled/rejected terminal. */
    assert(work_n.luma_bytes==0u);
    assert(delivery_n.frame_visited==0u);
#endif
    assert(tm_pc_destroy(portal_n)==TM_PC_EBUSY);
    reconcile_cancelled_terminal(&delivery_n,&runtime,&n.generation,
                                 &registry,ref_n);
    {
        tm_pc_snapshot_v1 after=TM_PC_SNAPSHOT_V1_INIT;
        assert(tm_pc_snapshot(portal_n,&after)==TM_PC_OK && after.drained);
    }
    owner_drain(&delivery_n);
    assert(tm_pc_destroy(portal_n)==TM_PC_OK);
#ifdef TM_TEST_FFMPEG_DSO
    assert(tm_test_provider_finish_decode(&value_n)==0);
#endif
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
#ifdef TM_TEST_FFMPEG_DSO
    /* N+1 is a real decoded FFmpeg frame, not a fabricated generation byte.
     * Cancelled old N must never publish a frame after rejection. */
    assert(delivery_n1.frame_visited==1u);
    assert(delivery_n1.frame_sink_visited==1u);
    assert(delivery_n.frame_visited==0u);
    assert(delivery_n.frame_sink_visited==0u);
    tm_mjpeg_encoder_destroy(&encoder);
#endif
    puts("installed Salts ComponentPlugin true N->N+1 DSO cancellation/drain: PASS");
    return 0;
}
