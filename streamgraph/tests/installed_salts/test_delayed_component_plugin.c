/* M15c: REAL installed Salts ComponentPlugin DSO + independently installed
 * desktop FFmpeg addon. The bounded MPEG-2 CMeta Instance retains the
 * original Generation Scope across N -> N+1, exact Portal tickets and
 * genuine CNet wakes. No second Plugin loader or Source scheduler.
 *
 * Multi-packet step is explicitly Host/Owner admitted: EAGAIN is its own
 * completed terminal receipt, NOT permission to retry a side effect. */
#include "provider_contract.h"
#include <salts/component_plugin.h>
#include <cnet/cnet.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

enum { WIDTH=64,HEIGHT=64,LUMA=WIDTH*HEIGHT,FRAME_COUNT=12 };
typedef struct generation_fixture {
    salts_component_plugin_generation generation;
    salts_component_deployment deployments[1];
    salts_component_instance instances[1];
    salts_component_dependency dependencies[1];
    size_t activation_order[1];
    salts_component_plugin_module modules[1];
} generation_fixture;
static salts_component_plugin_status build(
    generation_fixture *f,uint64_t generation,
    cmeta_plugin_registry *registry,cmeta_plugin_ref plugin) {
    const salts_component_plugin_generation_storage storage={
        f->deployments,1u,f->instances,1u,f->dependencies,1u,
        f->activation_order,1u,f->modules,1u
    };
    const salts_component_plugin_source source={
        plugin,TM_TEST_PROVIDER_EXPORT_ID,NULL,NULL
    };
    return salts_component_plugin_generation_build(
        &f->generation,generation,registry,&storage,
        NULL,0u,&source,1u,NULL,0u);
}
static tm_test_provider find_provider(salts_component_plugin_scope *scope) {
    salts_component_service service;
    tm_test_provider result=tm_test_provider_bind(NULL,NULL);
    assert(salts_component_plugin_scope_find_service(
               scope,tm_test_provider_interface(),&service)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(tm_test_provider_borrow_from_object(
               service.object,service.interfaces,&result)==CMETA_OK);
    return result;
}
static void init_client(cnet_client *client) {
    cnet_client_config cfg={0};
    cfg.backend=NATIVE_IO_BACKEND_EPOLL;
    cfg.connection_capacity=2u;
    cfg.command_capacity=8u;
    cfg.request_capacity=8u;
    cfg.completion_batch_capacity=4u;
    cfg.event_capacity=8u;
    cfg.max_send_bytes=1024u;
    cfg.receive_buffer_bytes=1024u;
    assert(cnet_client_init(client,&cfg)==SALTS_OK);
}
static void drain_wake(cnet_client *client) {
    cnet_progress_strategy strategy=CNET_PROGRESS_STRATEGY_INIT;
    size_t events=0u;
    strategy.idle_wait_ms=1000u;
    assert(cnet_client_poll_strategy(client,&strategy,1000u,&events)==SALTS_OK);
    (void)events;
}
typedef struct encoded {
    AVPacket *packet[FRAME_COUNT];
    unsigned count;
} encoded;
static unsigned level(unsigned i){return 36u+13u*i;}
static void collect(AVCodecContext *encoder,AVPacket *packet,
                    encoded *out,int finishing) {
    for(;;) {
        int rc=avcodec_receive_packet(encoder,packet);
        if(rc==AVERROR(EAGAIN)){assert(!finishing);return;}
        if(rc==AVERROR_EOF){assert(finishing);return;}
        assert(rc==0 && out->count<FRAME_COUNT);
        out->packet[out->count]=av_packet_clone(packet);
        assert(out->packet[out->count]!=NULL);
        ++out->count;
        av_packet_unref(packet);
    }
}
static void make_encoded(encoded *out) {
    const AVCodec *codec=avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    AVCodecContext *encoder;
    AVFrame *frame;
    AVPacket *packet;
    unsigned i,y,reordered=0u;
    int64_t last=INT64_MIN;
    assert(codec!=NULL);
    encoder=avcodec_alloc_context3(codec);
    assert(encoder!=NULL);
    encoder->width=WIDTH;encoder->height=HEIGHT;
    encoder->time_base=(AVRational){1,30};
    encoder->framerate=(AVRational){30,1};
    encoder->pix_fmt=AV_PIX_FMT_YUV420P;
    encoder->bit_rate=1200000;encoder->gop_size=12;
    encoder->max_b_frames=2;encoder->thread_count=1;
    assert(avcodec_open2(encoder,codec,NULL)==0);
    frame=av_frame_alloc();packet=av_packet_alloc();
    assert(frame && packet);
    frame->width=WIDTH;frame->height=HEIGHT;
    frame->format=AV_PIX_FMT_YUV420P;
    assert(av_frame_get_buffer(frame,32)==0);
    for(i=0u;i<FRAME_COUNT;++i) {
        assert(av_frame_make_writable(frame)==0);
        for(y=0u;y<HEIGHT;++y)
            memset(frame->data[0]+(size_t)y*frame->linesize[0],
                   (int)level(i),WIDTH);
        for(y=0u;y<HEIGHT/2u;++y) {
            memset(frame->data[1]+(size_t)y*frame->linesize[1],128,WIDTH/2);
            memset(frame->data[2]+(size_t)y*frame->linesize[2],128,WIDTH/2);
        }
        frame->pts=(int64_t)i;
        assert(avcodec_send_frame(encoder,frame)==0);
        collect(encoder,packet,out,0);
    }
    assert(out->count<FRAME_COUNT);
    assert(avcodec_send_frame(encoder,NULL)==0);
    collect(encoder,packet,out,1);
    assert(out->count==FRAME_COUNT);
    for(i=0u;i<out->count;++i) {
        if(out->packet[i]->pts<last)++reordered;
        last=out->packet[i]->pts;
    }
    assert(reordered>0u);
    av_frame_free(&frame);av_packet_free(&packet);
    avcodec_free_context(&encoder);
}
typedef struct step_worker {
    tm_test_provider service; /* borrowed under live original Scope */
    tm_test_delayed_step_request request;
    int rc;
    _Atomic unsigned done;
} step_worker;
static void *run_step(void *ctx) {
    step_worker *worker=(step_worker *)ctx;
    worker->rc=tm_test_provider_step_delayed(&worker->service,
                                            &worker->request);
    atomic_store_explicit(&worker->done,1u,memory_order_release);
    return NULL;
}
static tm_pc_portal *make_portal(uint64_t generation) {
    tm_pc_config_v1 cfg=TM_PC_CONFIG_V1_INIT;
    tm_pc_portal *result=NULL;
    cfg.capacity=1u;cfg.provider_generation=generation;
    assert(tm_pc_create(&cfg,&result)==TM_PC_OK);
    return result;
}
static int submit(tm_test_provider *p,tm_test_instance_request identity,
                  AVPacket *packet) {
    tm_test_delayed_packet_request request={0};
    uint8_t *temporary;
    int rc;
    assert(packet->size>0 && packet->size<=65536);
    temporary=(uint8_t *)malloc((size_t)packet->size);
    assert(temporary!=NULL);
    memcpy(temporary,packet->data,(size_t)packet->size);
    request.size=sizeof(request);
    request.instance_id=identity.instance_id;
    request.provider_generation=identity.provider_generation;
    request.packet=(tm_ffd_packet_v1)TM_FFD_PACKET_V1_INIT;
    request.packet.data=temporary;
    request.packet.bytes=(size_t)packet->size;
    request.packet.pts=packet->pts;
    request.packet.dts=packet->dts;
    rc=tm_test_provider_submit_delayed(p,&request);
    /* All compressed bytes are borrowed ONLY during this CMeta call;
     * the DSO's bounded Codec Provider must own the accepted copy. */
    memset(temporary,0,(size_t)packet->size);
    free(temporary);
    return rc;
}
int main(int argc,char **argv) {
    const cmeta_plugin_registry_config config={2};
    cmeta_plugin_registry registry={0};
    cmeta_plugin_ref ref_n={0},ref_n1={0};
    generation_fixture n={0},n1={0};
    salts_component_plugin_runtime runtime=SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *previous=NULL;
    salts_component_plugin_scope scope_n=SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    salts_component_plugin_scope scope_n1=SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    tm_test_provider service_n,service_n1;
    tm_test_instance_request a={sizeof(a),501u,101u};
    tm_test_instance_request b={sizeof(b),502u,101u};
    tm_test_instance_request third={sizeof(third),503u,101u};
    tm_test_instance_request wrong={sizeof(wrong),504u,102u};
    tm_test_instance_request newer={sizeof(newer),902u,102u};
    tm_test_delayed_stats_request stats_req={0};
    tm_ffd_stats_v1 stats=TM_FFD_STATS_V1_INIT;
    tm_pc_portal *portal=NULL;
    cnet_client wake={0};
    encoded input={0};
    unsigned next=0u,frames=0u,step=0u,finished=0u,published=0u;
    unsigned saw_eagain=0u,saw_eof=0u;
    assert(argc==3);
    make_encoded(&input);
    init_client(&wake);
    assert(cmeta_plugin_registry_init(&registry,&config)==CMETA_PLUGIN_OK);
    assert(cmeta_plugin_registry_load(&registry,argv[1],&ref_n)==CMETA_PLUGIN_OK);
    assert(cmeta_plugin_registry_start(&registry,ref_n)==CMETA_PLUGIN_OK);
    assert(cmeta_plugin_registry_load(&registry,argv[2],&ref_n1)==CMETA_PLUGIN_OK);
    assert(cmeta_plugin_registry_start(&registry,ref_n1)==CMETA_PLUGIN_OK);
    assert(build(&n,101u,&registry,ref_n)==SALTS_COMPONENT_PLUGIN_OK);
    assert(build(&n1,102u,&registry,ref_n1)==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_runtime_init(&runtime)==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_runtime_publish(
               &runtime,&n.generation,&previous)==SALTS_COMPONENT_PLUGIN_OK);
    assert(previous==NULL);
    assert(salts_component_plugin_scope_acquire(&runtime,&scope_n)
           ==SALTS_COMPONENT_PLUGIN_OK);
    service_n=find_provider(&scope_n);
    assert(tm_test_provider_create_delayed(&service_n,&wrong)==-1);
    assert(tm_test_provider_create_delayed(&service_n,&a)==0);
    assert(tm_test_provider_create_delayed(&service_n,&b)==0);
    assert(tm_test_provider_create_delayed(&service_n,&a)==-2);
    assert(tm_test_provider_create_delayed(&service_n,&third)==-4);
    portal=make_portal(101u);
    /* Deterministic no-input admission: MPEG-2 may produce its first frame
     * before a two-packet queue exhausts, so never assume a scheduler race
     * will happen to emit EAGAIN during ordinary decoding. */
    {
        tm_pc_ticket_v1 ticket={0},taken={0};
        tm_pc_result_v1 receipt=TM_PC_RESULT_V1_INIT;
        tm_test_delayed_step_request request={0};
        tm_test_delayed_take_request take={0};
        step_worker worker={0};
        pthread_t thread;
        uint8_t pixels[LUMA];
        size_t count=0u;
        int64_t pts=INT64_MIN;
        assert(tm_pc_begin(portal,11999u,&ticket)==TM_PC_OK);
        request.size=sizeof(request);
        request.instance_id=a.instance_id;
        request.provider_generation=a.provider_generation;
        request.portal=portal;request.ticket=ticket;request.wake_client=&wake;
        worker.service=service_n;worker.request=request;
        atomic_init(&worker.done,0u);
        assert(pthread_create(&thread,NULL,run_step,&worker)==0);
        assert(pthread_join(thread,NULL)==0);
        assert(worker.rc==TM_FF_EAGAIN);
        assert(atomic_load_explicit(&worker.done,memory_order_acquire)==1u);
        assert(tm_test_provider_wake_status(&service_n)==SALTS_OK);
        drain_wake(&wake);
        assert(tm_pc_take(portal,&taken,&receipt)==TM_PC_OK);
        assert(receipt.correlation==11999u &&
               receipt.outcome==TM_PC_COMPLETED &&
               receipt.provider_status==TM_FF_EAGAIN &&
               receipt.completion_observed==1u);
        take.size=sizeof(take);
        take.instance_id=a.instance_id;
        take.provider_generation=a.provider_generation;
        take.ticket=ticket;
        take.luma=pixels;take.capacity=sizeof(pixels);
        take.out_bytes=&count;take.out_pts=&pts;
        assert(tm_test_provider_take_delayed(&service_n,&take)==-2);
        assert(count==0u && pts==INT64_MIN);
        /* Without EOS or explicit Cancel, destroy is not authorized
         * even if this single EAGAIN worker already quiesced. */
        assert(tm_test_provider_destroy_delayed(&service_n,&a)==-4);
        assert(tm_pc_settle(portal,taken)==TM_PC_OK);
        ++saw_eagain;
    }
    for(step=0u;step<FRAME_COUNT*6u;++step) {
        tm_pc_ticket_v1 ticket={0},taken={0};
        tm_pc_result_v1 receipt=TM_PC_RESULT_V1_INIT;
        tm_test_delayed_take_request take={0};
        tm_test_delayed_step_request req={0};
        step_worker worker={0};
        pthread_t thread;
        uint8_t luma[LUMA];
        size_t copied=0u;
        int64_t pts=INT64_MIN;
        int got;
        /* Bounded two-packet admission: source caller explicitly repolls
         * only after the Codec has consumed credits. No background retry. */
        while(next<input.count) {
            int rc=submit(&service_n,a,input.packet[next]);
            if(rc==TM_FF_EFULL)break;
            assert(rc==TM_FF_OK);
            ++next;
        }
        if(next==input.count && !finished) {
            assert(tm_test_provider_finish_delayed(&service_n,&a)==TM_FF_OK);
            finished=1u;
        }
        assert(tm_pc_begin(portal,12000u+step,&ticket)==TM_PC_OK);
        req.size=sizeof(req);
        req.instance_id=a.instance_id;
        req.provider_generation=a.provider_generation;
        req.portal=portal;req.ticket=ticket;req.wake_client=&wake;
        worker.service=service_n;worker.request=req;
        atomic_init(&worker.done,0u);
        assert(pthread_create(&thread,NULL,run_step,&worker)==0);
        assert(pthread_join(thread,NULL)==0);
        assert(atomic_load_explicit(&worker.done,memory_order_acquire)==1u);
        got=worker.rc;
        assert(got==TM_FF_OK || got==TM_FF_EAGAIN || got==TM_FF_EOF);
        assert(tm_test_provider_wake_status(&service_n)==SALTS_OK);
        drain_wake(&wake);
        assert(tm_pc_take(portal,&taken,&receipt)==TM_PC_OK);
        assert(receipt.correlation==12000u+step &&
               receipt.provider_generation==101u &&
               receipt.outcome==TM_PC_COMPLETED &&
               receipt.provider_status==got &&
               receipt.completion_observed==1u);
        take.size=sizeof(take);
        take.instance_id=a.instance_id;
        take.provider_generation=a.provider_generation;
        take.ticket=ticket;
        take.luma=luma;take.capacity=sizeof(luma);
        take.out_bytes=&copied;take.out_pts=&pts;
        if(got==TM_FF_OK) {
            /* A pending decoded frame is a retaining Owner credit.
             * DSO teardown must reject until exact ticket acceptance. */
            assert(tm_test_provider_destroy_delayed(&service_n,&a)==-4);
            tm_test_delayed_take_request stale=take;
            stale.ticket.generation++;
            assert(tm_test_provider_take_delayed(&service_n,&stale)==-2);
            assert(tm_test_provider_take_delayed(&service_n,&take)==LUMA);
            assert(copied==LUMA && pts==(int64_t)frames);
            assert(abs((int)luma[0]-(int)level(frames))<=22);
            assert(abs((int)luma[LUMA-1]-(int)level(frames))<=22);
            assert(tm_test_provider_take_delayed(&service_n,&take)==-2);
            ++frames;
        } else {
            assert(tm_test_provider_take_delayed(&service_n,&take)==-2);
            if(got==TM_FF_EAGAIN)++saw_eagain;
            else ++saw_eof;
        }
        assert(tm_pc_settle(portal,taken)==TM_PC_OK);
        assert(tm_pc_take(portal,&taken,&receipt)!=TM_PC_OK);
        if(frames>=3u && !published) {
            /* Live old Scope authorizes pending Provider resources when
             * N+1 becomes current. N cannot drain/unload prematurely. */
            assert(salts_component_plugin_runtime_publish(
                       &runtime,&n1.generation,&previous)==SALTS_COMPONENT_PLUGIN_OK);
            assert(previous==&n.generation);
            assert(salts_component_plugin_generation_drain(
                       &runtime,&n.generation)==SALTS_COMPONENT_PLUGIN_BUSY);
            assert(cmeta_plugin_registry_unload(&registry,ref_n)==CMETA_PLUGIN_BUSY);
            assert(salts_component_plugin_scope_acquire(&runtime,&scope_n1)
                   ==SALTS_COMPONENT_PLUGIN_OK);
            service_n1=find_provider(&scope_n1);
            assert(tm_test_provider_create_delayed(&service_n1,&a)==-1);
            assert(tm_test_provider_create_delayed(&service_n1,&newer)==0);
            assert(tm_test_provider_cancel_delayed(&service_n1,&newer)==0);
            assert(tm_test_provider_destroy_delayed(&service_n1,&newer)==0);
            published=1u;
        }
        if(got==TM_FF_EOF)break;
    }
    assert(published && finished && saw_eof==1u && frames==FRAME_COUNT);
    assert(next==input.count && saw_eagain>=1u);
    stats_req.size=sizeof(stats_req);
    stats_req.instance_id=a.instance_id;
    stats_req.provider_generation=a.provider_generation;
    stats_req.out=&stats;
    assert(tm_test_provider_stats_delayed(&service_n,&stats_req)==TM_FF_OK);
    assert(stats.packets_admitted==FRAME_COUNT &&
           stats.packets_sent==FRAME_COUNT &&
           stats.frames_decoded==FRAME_COUNT &&
           stats.frames_after_flush>=1u && stats.flush_sends==1u &&
           stats.queued==0u && stats.finishing && stats.eof &&
           !stats.cancelled && !stats.codec_error);
    assert(tm_pc_seal(portal)==TM_PC_OK);
    assert(tm_pc_destroy(portal)==TM_PC_OK);
    /* B never received a packet: explicit host cancel before DSO disposal. */
    assert(tm_test_provider_cancel_delayed(&service_n,&b)==0);
    assert(tm_test_provider_destroy_delayed(&service_n,&b)==0);
    assert(tm_test_provider_destroy_delayed(&service_n,&a)==0);
    assert(tm_test_provider_destroy_delayed(&service_n,&a)==-2);
    assert(salts_component_plugin_generation_drain(
               &runtime,&n.generation)==SALTS_COMPONENT_PLUGIN_BUSY);
    assert(salts_component_plugin_scope_release(&scope_n)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_generation_drain(
               &runtime,&n.generation)==SALTS_COMPONENT_PLUGIN_OK);
    {
        bool quiet=false;
        assert(cmeta_plugin_registry_request_stop(&registry,ref_n)==CMETA_PLUGIN_OK);
        assert(cmeta_plugin_registry_poll_quiescent(&registry,ref_n,&quiet)==CMETA_PLUGIN_OK);
        assert(quiet);
        assert(cmeta_plugin_registry_unload(&registry,ref_n)==CMETA_PLUGIN_OK);
    }
    assert(salts_component_plugin_runtime_close(&runtime,&previous)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(previous==&n1.generation);
    assert(salts_component_plugin_generation_drain(
               &runtime,&n1.generation)==SALTS_COMPONENT_PLUGIN_BUSY);
    assert(salts_component_plugin_scope_release(&scope_n1)
           ==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_generation_drain(
               &runtime,&n1.generation)==SALTS_COMPONENT_PLUGIN_OK);
    assert(salts_component_plugin_runtime_destroy(&runtime)==SALTS_COMPONENT_PLUGIN_OK);
    {
        bool quiet=false;
        assert(cmeta_plugin_registry_request_stop(&registry,ref_n1)==CMETA_PLUGIN_OK);
        assert(cmeta_plugin_registry_poll_quiescent(&registry,ref_n1,&quiet)==CMETA_PLUGIN_OK);
        assert(quiet);
        assert(cmeta_plugin_registry_unload(&registry,ref_n1)==CMETA_PLUGIN_OK);
    }
    assert(cmeta_plugin_registry_destroy(&registry)==CMETA_PLUGIN_OK);
    assert(cnet_client_stop(&wake,1000u)==SALTS_OK);
    assert(cnet_client_destroy(&wake)==SALTS_OK);
    for(step=0u;step<input.count;++step)av_packet_free(&input.packet[step]);
    puts("installed Salts CMeta real MPEG2 B-frame multi-packet generation handoff: PASS");
    return 0;
}
