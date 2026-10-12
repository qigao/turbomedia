/* M15d installed-Salts ComponentPlugin integration of the SAME real
 * RTSP/CNet RFC2250 picture stream used by test_rtsp_mpeg_video_codec.inc.
 *
 * A genuine CMeta provider DSO performs the real tm_ffd operations; the
 * host owns RTSP admission, one Portal ticket per Codec step, CNet wake,
 * original scope leases and the exact Generation replacement.
 * No private dlopen, second codec executor, automatic retry or hidden flush.
 *
 * Linux-only opt-in test fixture. Provider ABI is test-owned and not yet a
 * production RTSP Codec Provider API.
 */
#include <salts/component_plugin.h>
#include <cnet/cnet.h>
#include "provider_contract.h"

typedef struct rtsp_mpv_gen_fixture {
    salts_component_plugin_generation generation;
    salts_component_deployment deployments[1];
    salts_component_instance instances[1];
    salts_component_dependency dependencies[1];
    size_t order[1];
    salts_component_plugin_module modules[1];
} rtsp_mpv_gen_fixture;

static salts_component_plugin_status rtsp_mpv_build_generation(
    rtsp_mpv_gen_fixture *f,uint64_t id,cmeta_plugin_registry *r,
    cmeta_plugin_ref ref) {
    const salts_component_plugin_generation_storage storage={
        f->deployments,1u,f->instances,1u,f->dependencies,1u,
        f->order,1u,f->modules,1u};
    const salts_component_plugin_source src={
        ref,TM_TEST_PROVIDER_EXPORT_ID,NULL,NULL};
    return salts_component_plugin_generation_build(
        &f->generation,id,r,&storage,NULL,0u,&src,1u,NULL,0u);
}
static tm_test_provider rtsp_mpv_find_dso_service(
    salts_component_plugin_scope *scope) {
    salts_component_service s;
    tm_test_provider service=tm_test_provider_bind(NULL,NULL);
    check_equal(salts_component_plugin_scope_find_service(
        scope,tm_test_provider_interface(),&s),SALTS_COMPONENT_PLUGIN_OK);
    check_equal(tm_test_provider_borrow_from_object(
        s.object,s.interfaces,&service),CMETA_OK);
    return service;
}
static int rtsp_mpv_provider_submit(
    tm_test_provider *service,tm_test_instance_request identity,
    uint64_t expected_epoch,uint64_t epoch,uint8_t channel,
    const uint8_t *wire,size_t length,uint16_t seq,uint32_t ssrc) {
    tm_test_delayed_packet_request req={0};
    if(rtsp_mpv_prepare_packet(expected_epoch,epoch,channel,wire,
                              length,seq,ssrc,&req.packet)!=0)
        return -1;
    req.size=sizeof(req);
    req.instance_id=identity.instance_id;
    req.provider_generation=identity.provider_generation;
    return tm_test_provider_submit_delayed(service,&req);
}
static void rtsp_mpv_cnet_wake_init(cnet_client *wake) {
    cnet_client_config cfg={0};
    cfg.backend=NATIVE_IO_BACKEND_EPOLL;
    cfg.connection_capacity=2u;
    cfg.command_capacity=8u;cfg.request_capacity=8u;
    cfg.completion_batch_capacity=4u;cfg.event_capacity=8u;
    cfg.max_send_bytes=1024u;cfg.receive_buffer_bytes=1024u;
    check_equal(cnet_client_init(wake,&cfg),SALTS_OK);
}
static void rtsp_mpv_dso_stop(
    cmeta_plugin_registry *reg,cmeta_plugin_ref plugin) {
    bool quiet=false;
    check_equal(cmeta_plugin_registry_request_stop(
        reg,plugin),CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        reg,plugin,&quiet),CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(
        reg,plugin),CMETA_PLUGIN_OK);
}
static tm_pc_portal *rtsp_mpv_portal(uint64_t gen) {
    tm_pc_portal *p=NULL;
    tm_pc_config_v1 c=TM_PC_CONFIG_V1_INIT;
    c.capacity=1u;c.provider_generation=gen;
    check_equal(tm_pc_create(&c,&p),TM_PC_OK);
    return p;
}
/* One original Portal slot/credit is retained until the Media Owner
 * settles the *real* Provider terminal. A drained snapshot by itself is
 * never authority to unload the DSO: the ComponentPlugin Scope is separate. */
static void rtsp_mpv_assert_portal_credits(
    tm_pc_portal *portal,unsigned inflight,unsigned cancel_pending,
    unsigned ready,unsigned taken) {
    tm_pc_snapshot_v1 snap=TM_PC_SNAPSHOT_V1_INIT;
    check_equal(tm_pc_snapshot(portal,&snap),TM_PC_OK);
    check_equal(snap.capacity,1u);
    check_equal(snap.inflight,inflight);
    check_equal(snap.cancel_pending,cancel_pending);
    check_equal(snap.ready,ready);
    check_equal(snap.taken,taken);
    check_equal(snap.drained,(inflight+cancel_pending+ready+taken)==0u);
}

static void rtsp_mpv_step_dso(
    tm_test_provider *service,tm_test_instance_request identity,
    cnet_client *wake,tm_pc_portal *portal,
    uint64_t correlation,rtsp_mpv_fixture *f,unsigned *eof) {
    tm_pc_ticket_v1 ticket={0},taken={0};
    tm_pc_result_v1 receipt=TM_PC_RESULT_V1_INIT;
    tm_test_delayed_step_request req={0};
    tm_test_delayed_take_request take={0};
    uint8_t luma[RTSP_MPV_LUMA]={0};
    size_t bytes=0u;
    int64_t pts=INT64_MIN;
    int result;
    check_equal(tm_pc_begin(portal,correlation,&ticket),TM_PC_OK);
    rtsp_mpv_assert_portal_credits(portal,1u,0u,0u,0u);
    req.size=sizeof(req);
    req.instance_id=identity.instance_id;
    req.provider_generation=identity.provider_generation;
    req.portal=portal;req.ticket=ticket;req.wake_client=wake;
    result=tm_test_provider_step_delayed(service,&req);
    check_true(result==TM_FF_OK || result==TM_FF_EAGAIN ||
               result==TM_FF_EOF);
    check_equal(tm_test_provider_wake_status(service),SALTS_OK);
    rtsp_mpv_assert_portal_credits(portal,0u,0u,1u,0u);
    check_equal(tm_pc_take(portal,&taken,&receipt),TM_PC_OK);
    rtsp_mpv_assert_portal_credits(portal,0u,0u,0u,1u);
    check_equal(receipt.correlation,correlation);
    check_equal(receipt.provider_generation,identity.provider_generation);
    check_equal(receipt.outcome,TM_PC_COMPLETED);
    check_equal(receipt.completion_observed,1u);
    check_equal(receipt.provider_status,result);
    /* Completion won the actual DSO step before a later Media Owner
     * cancel. The original Portal outcome is immutable: neither a
     * late cancel nor a duplicate complete may rewrite the receipt. */
    check_equal(tm_pc_request_cancel(portal,ticket),TM_PC_EALREADY);
    check_equal(tm_pc_complete(portal,ticket,TM_FF_ESTATE),TM_PC_EALREADY);

    take.size=sizeof(take);
    take.instance_id=identity.instance_id;
    take.provider_generation=identity.provider_generation;
    take.ticket=ticket;
    take.luma=luma;take.capacity=sizeof(luma);
    take.out_bytes=&bytes;take.out_pts=&pts;
    if(result==TM_FF_OK) {
        /* A decoded frame retains the exact original ticket until take.
         * No DSO unload or Instance destroy may steal its pixels. */
        check_equal(tm_test_provider_destroy_delayed(
            service,&identity),-4);
        check_equal(tm_test_provider_take_delayed(
            service,&take),RTSP_MPV_LUMA);
        check_equal(bytes,RTSP_MPV_LUMA);
        check_equal(pts,(int64_t)f->display_frames);
        if(f->display_frames<RTSP_MPV_FRAMES) {
            unsigned level=rtsp_mpv_level(f->display_frames);
            check_true(abs((int)luma[0]-(int)level)<=22);
            check_true(abs((int)luma[RTSP_MPV_LUMA-1]-(int)level)<=22);
        }
        ++f->display_frames;
    } else {
        check_equal(tm_test_provider_take_delayed(service,&take),-2);
        if(result==TM_FF_EOF)*eof=1u;
        else ++f->eagain;
    }
    check_equal(tm_pc_settle(portal,taken),TM_PC_OK);
    rtsp_mpv_assert_portal_credits(portal,0u,0u,0u,0u);
    check_equal(tm_pc_request_cancel(portal,ticket),TM_PC_ESTALE);
    check_equal(tm_pc_complete(portal,ticket,TM_FF_ESTATE),TM_PC_ESTALE);
    check_true(tm_pc_take(portal,&taken,&receipt)!=TM_PC_OK);
}
static void rtsp_test_mpeg2_real_rtp_to_salts_generation(void) {
    const char *path101=getenv("TM_RTSP_TEST_DSO_101");
    const char *path102=getenv("TM_RTSP_TEST_DSO_102");
    const cmeta_plugin_registry_config registry_cfg={2};
    cmeta_plugin_registry registry={0};
    cmeta_plugin_ref ref101={0},ref102={0};
    rtsp_mpv_gen_fixture gen101={0},gen102={0};
    salts_component_plugin_runtime runtime=SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *prior=NULL;
    salts_component_plugin_scope scope101=SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    salts_component_plugin_scope scope102=SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    tm_test_provider service101,service102;
    tm_test_instance_request first={sizeof(first),19501u,101u};
    tm_test_instance_request second={sizeof(second),19502u,102u};
    tm_test_delayed_stats_request stats_request={0};
    tm_ffd_stats_v1 stats=TM_FFD_STATS_V1_INIT;
    tm_pc_portal *p101=NULL,*p102=NULL;
    rtsp_mpv_fixture f={0};
    rtsp_test_state_t state;
    turbo_rtsp_server_t *server=NULL;
    turbo_rtsp_client_t *client=NULL;
    cnet_client wake={0};
    uint8_t wire[RTSP_MPV_MAX_PACKET];
    size_t bytes=0u;
    const uint32_t ssrc101=0x77776666u,ssrc102=0x88889999u;
    unsigned next=0u,round=0u,finished=0u,eof=0u,published=0u;

    check_not_null(path101);check_not_null(path102);
    if(!path101 || !path102)return;
    rtsp_test_state_init(&state);
    state.require_mpvideo_sdp=1;
    rtsp_mpv_fixture_make(&f);
    server=rtsp_test_server_start(&state,TURBO_RTSP_CONTROL_TRANSPORT_TCP,
                                  20564,NULL);
    check_not_null(server);
    if(!server)goto cleanup;
    rtsp_mpv_reject_bad_announce(&state,20564);
    client=rtsp_test_client_connect(TURBO_RTSP_CONTROL_TRANSPORT_TCP,
                                    20564,NULL);
    check_not_null(client);
    if(!client)goto cleanup;
    check_true(rtsp_mpv_begin_record(client));
    rtsp_mpv_cnet_wake_init(&wake);
    check_equal(cmeta_plugin_registry_init(&registry,&registry_cfg),CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(&registry,path101,&ref101),CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry,ref101),CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(&registry,path102,&ref102),CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry,ref102),CMETA_PLUGIN_OK);
    check_equal(rtsp_mpv_build_generation(&gen101,101u,&registry,ref101),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(rtsp_mpv_build_generation(&gen102,102u,&registry,ref102),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_init(&runtime),SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_publish(
                &runtime,&gen101.generation,&prior),SALTS_COMPONENT_PLUGIN_OK);
    check_null(prior);
    check_equal(salts_component_plugin_scope_acquire(
                &runtime,&scope101),SALTS_COMPONENT_PLUGIN_OK);
    service101=rtsp_mpv_find_dso_service(&scope101);
    check_equal(tm_test_provider_create_delayed(&service101,&first),0);
    p101=rtsp_mpv_portal(101u);

    /* Only the RTSP Media Owner admits new data. Never queue compressed
     * input without checking the exact DSO's bounded queue credits. */
    for(round=0u;round<RTSP_MPV_FRAMES*9u && !eof;++round) {
        tm_ffd_stats_v1 snapshot=TM_FFD_STATS_V1_INIT;
        stats_request.size=sizeof(stats_request);
        stats_request.instance_id=first.instance_id;
        stats_request.provider_generation=first.provider_generation;
        stats_request.out=&snapshot;
        check_equal(tm_test_provider_stats_delayed(
                    &service101,&stats_request),TM_FF_OK);
        while(next<f.packets_count && snapshot.queued<2u) {
            rtsp_mpv_wire_exchange(client,f.packets[next],
                                   (uint16_t)(RTSP_MPV_RTP_SEQUENCE+next),
                                   ssrc101,wire,&bytes);
            check_equal(rtsp_mpv_provider_submit(
                        &service101,first,101u,101u,1u,wire,bytes,
                        (uint16_t)(RTSP_MPV_RTP_SEQUENCE+next),
                        ssrc101),TM_FF_OK);
            /* Borrowed RTP bytes may be reclaimed immediately. */
            memset(wire,0,bytes);
            ++snapshot.queued;
            ++next;
        }
        if(next==f.packets_count && !finished) {
            check_equal(tm_test_provider_finish_delayed(
                        &service101,&first),TM_FF_OK);
            finished=1u;
        }
        rtsp_mpv_step_dso(&service101,first,&wake,p101,
                          20000u+round,&f,&eof);
        if(f.display_frames>=3u && !published) {
            check_equal(salts_component_plugin_runtime_publish(
                        &runtime,&gen102.generation,&prior),SALTS_COMPONENT_PLUGIN_OK);
            check_true(prior==&gen101.generation);
            check_equal(salts_component_plugin_generation_drain(
                        &runtime,&gen101.generation),SALTS_COMPONENT_PLUGIN_BUSY);
            check_equal(cmeta_plugin_registry_unload(
                        &registry,ref101),CMETA_PLUGIN_BUSY);
            check_equal(salts_component_plugin_scope_acquire(
                        &runtime,&scope102),SALTS_COMPONENT_PLUGIN_OK);
            service102=rtsp_mpv_find_dso_service(&scope102);
            check_equal(tm_test_provider_create_delayed(&service102,&second),0);
            published=1u;
        }
    }
    check_true(eof && published && finished && next==f.packets_count);
    check_equal(f.display_frames,RTSP_MPV_FRAMES);
    stats=(tm_ffd_stats_v1)TM_FFD_STATS_V1_INIT;
    stats_request.out=&stats;
    check_equal(tm_test_provider_stats_delayed(
                &service101,&stats_request),TM_FF_OK);
    check_equal(stats.packets_admitted,RTSP_MPV_FRAMES);
    check_equal(stats.packets_sent,RTSP_MPV_FRAMES);
    check_equal(stats.frames_decoded,RTSP_MPV_FRAMES);
    check_true(stats.frames_after_flush>=1u);
    check_equal(stats.flush_sends,1u);
    check_equal(stats.queued,0u);
    check_equal(tm_test_provider_destroy_delayed(&service101,&first),0);
    check_equal(tm_pc_seal(p101),TM_PC_OK);
    check_equal(tm_pc_destroy(p101),TM_PC_OK);p101=NULL;
    check_equal(salts_component_plugin_scope_release(&scope101),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_generation_drain(
                &runtime,&gen101.generation),SALTS_COMPONENT_PLUGIN_OK);
    rtsp_mpv_dso_stop(&registry,ref101);

    /* Abort a NEW transport Session and N+1 Decoder Generation after two
     * actual RTP packets. No hidden Codec drain or stale old packet. */
    check_equal(turbo_rtsp_client_teardown(client),0);
    turbo_rtsp_client_destroy(client);client=NULL;
    client=rtsp_test_client_connect(TURBO_RTSP_CONTROL_TRANSPORT_TCP,
                                    20564,NULL);
    check_not_null(client);
    if(!client)goto cleanup;
    check_true(rtsp_mpv_begin_record(client));
    for(round=0u;round<2u;++round) {
        uint16_t seq=(uint16_t)(RTSP_MPV_RTP_SEQUENCE+round);
        rtsp_mpv_wire_exchange(client,f.packets[round],seq,ssrc102,wire,&bytes);
        check_equal(rtsp_mpv_provider_submit(
                    &service102,second,102u,101u,1u,wire,bytes,seq,ssrc102),-1);
        check_equal(rtsp_mpv_provider_submit(
                    &service102,second,102u,102u,1u,wire,bytes,seq,ssrc102),
                    TM_FF_OK);
        memset(wire,0,bytes);
    }
    /* Deterministic triple ownership crossover: one live RTSP/CNet
     * server callback still borrows the original Session, while the real
     * N+1 ComponentPlugin Scope and one exact original Portal Ticket
     * remain retained. Only the fixed Host/Media Owner cancels; no
     * concurrent progress on one CNet Client and no Codec method may
     * race with an in-libavcodec worker in this particular gate. */
    p102=rtsp_mpv_portal(102u);
    {
        tm_pc_ticket_v1 ticket={0},taken={0};
        tm_pc_result_v1 receipt=TM_PC_RESULT_V1_INIT;
        tm_test_delayed_step_request request={0};
        tm_test_delayed_take_request take={0};
        uint8_t luma[RTSP_MPV_LUMA];
        size_t out_bytes=13u;
        int64_t pts=13;
        check_equal(tm_pc_begin(p102,30001u,&ticket),TM_PC_OK);
        rtsp_mpv_assert_portal_credits(p102,1u,0u,0u,0u);
        {
            uint8_t pending_rtp[RTSP_MPV_MAX_PACKET]={0};
            size_t pending_bytes=0u;
            unsigned callback_entered,callback_exited;
            /* A THIRD real RTP packet is intentionally stopped inside
             * the server's actual CNet on_interleaved_frame callback.
             * It is not admitted to the Codec, and cannot leak from the
             * cancelled Session into the next Generation. */
            check_equal(rtsp_mpv_make_rtp(f.packets[2],
                        (uint16_t)(RTSP_MPV_RTP_SEQUENCE+2u),ssrc102,
                        pending_rtp,sizeof(pending_rtp),&pending_bytes),0);
            atomic_store_explicit(&state.hold_interleaved,1u,
                                  memory_order_release);
            check_equal(turbo_rtsp_client_send_interleaved_frame(
                        client,0u,pending_rtp,pending_bytes),0);
            callback_entered=(unsigned)rtsp_test_wait_for_callback(
                &state.interleaved_entered);
            check_true(callback_entered);
            /* The CNet callback is alive with old Session storage.
             * Retire N+1 publication while its canonical DSO Scope
             * and original cancellation Ticket are still live.
             * Early drain and DSO unload MUST observe BUSY. */
            check_equal(salts_component_plugin_runtime_close(
                        &runtime,&prior),SALTS_COMPONENT_PLUGIN_OK);
            check_true(prior==&gen102.generation);
            check_equal(salts_component_plugin_generation_drain(
                        &runtime,&gen102.generation),SALTS_COMPONENT_PLUGIN_BUSY);
            check_equal(cmeta_plugin_registry_unload(
                        &registry,ref102),CMETA_PLUGIN_BUSY);
            check_equal(tm_pc_request_cancel(p102,ticket),TM_PC_OK);
            rtsp_mpv_assert_portal_credits(p102,0u,1u,0u,0u);
            /* The original RTSP Client Owner closes NativeIO while the
             * server callback is STILL on its old borrowed stack.
             * Only after CNet callback exit does the Host touch Codec. */
            turbo_rtsp_client_close(client);
            atomic_store_explicit(&state.interleaved_release,1u,
                                  memory_order_release);
            callback_exited=(unsigned)rtsp_test_wait_for_callback(
                &state.interleaved_exited);
            check_true(callback_exited);
            atomic_store_explicit(&state.hold_interleaved,0u,
                                  memory_order_release);
            check_equal(tm_test_provider_cancel_delayed(
                        &service102,&second),0);
            /* Portal still owns the cancelled original Ticket until
             * its one authoritative DSO completion is settled. */
            check_equal(salts_component_plugin_generation_drain(
                        &runtime,&gen102.generation),SALTS_COMPONENT_PLUGIN_BUSY);
            check_equal(cmeta_plugin_registry_unload(
                        &registry,ref102),CMETA_PLUGIN_BUSY);
        }
        request.size=sizeof(request);
        request.instance_id=second.instance_id;
        request.provider_generation=second.provider_generation;
        request.portal=p102;request.ticket=ticket;request.wake_client=&wake;
        check_equal(tm_test_provider_step_delayed(
                    &service102,&request),TM_FF_ESTATE);
        rtsp_mpv_assert_portal_credits(p102,0u,0u,1u,0u);
        check_equal(tm_test_provider_wake_status(&service102),SALTS_OK);
        check_equal(tm_pc_take(p102,&taken,&receipt),TM_PC_OK);
        rtsp_mpv_assert_portal_credits(p102,0u,0u,0u,1u);
        check_equal(receipt.outcome,TM_PC_CANCELLED);
        check_equal(receipt.completion_observed,1u);
        check_equal(receipt.correlation,30001u);
        check_equal(receipt.provider_status,TM_FF_ESTATE);
        /* In this real RTP/NativeIO/DSO crossover cancel won first.
         * A second cancel or completion cannot convert the original
         * authoritative CANCELLED result into COMPLETED. */
        check_equal(tm_pc_request_cancel(p102,ticket),TM_PC_EALREADY);
        check_equal(tm_pc_complete(p102,ticket,TM_FF_OK),TM_PC_EALREADY);
        check_equal(tm_pc_cancel_quiesced(p102,ticket),TM_PC_EALREADY);
        take.size=sizeof(take);
        take.instance_id=second.instance_id;
        take.provider_generation=second.provider_generation;
        take.ticket=ticket;
        take.luma=luma;take.capacity=sizeof(luma);
        take.out_bytes=&out_bytes;take.out_pts=&pts;
        check_equal(tm_test_provider_take_delayed(&service102,&take),-2);
        check_equal(out_bytes,0u);
        check_equal(pts,INT64_MIN);
        check_equal(tm_pc_settle(p102,taken),TM_PC_OK);
        rtsp_mpv_assert_portal_credits(p102,0u,0u,0u,0u);
        check_equal(tm_pc_request_cancel(p102,ticket),TM_PC_ESTALE);
        check_equal(tm_pc_complete(p102,ticket,TM_FF_OK),TM_PC_ESTALE);
        check_true(tm_pc_take(p102,&taken,&receipt)!=TM_PC_OK);
    }
    stats=(tm_ffd_stats_v1)TM_FFD_STATS_V1_INIT;
    stats_request.instance_id=second.instance_id;
    stats_request.provider_generation=second.provider_generation;
    stats_request.out=&stats;
    check_equal(tm_test_provider_stats_delayed(
                &service102,&stats_request),TM_FF_OK);
    check_equal(stats.cancelled,1u);
    check_equal(stats.queued,0u);
    check_equal(stats.flush_sends,0u);
    check_equal(stats.frames_decoded,0u);
    check_equal(tm_test_provider_destroy_delayed(&service102,&second),0);
    check_equal(tm_pc_seal(p102),TM_PC_OK);
    check_equal(tm_pc_destroy(p102),TM_PC_OK);p102=NULL;
    check_equal(rtsp_test_read(&state,&state.record_count),2);
    check_equal(rtsp_test_read(&state,&state.teardown_count),1);
    check_equal(rtsp_test_read(&state,&state.accepted_mpvideo_sdp),2);
    check_equal(rtsp_test_read(&state,&state.rejected_mpvideo_sdp),2);
    /* Runtime was already retired while the original CNet callback was
     * still in flight; after Portal settlement the old Scope still
     * prevents premature unload until its final explicit release. */
    check_true(prior==&gen102.generation);
    check_equal(salts_component_plugin_generation_drain(
                &runtime,&gen102.generation),SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(cmeta_plugin_registry_unload(
                &registry,ref102),CMETA_PLUGIN_BUSY);
    check_equal(salts_component_plugin_scope_release(&scope102),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_generation_drain(
                &runtime,&gen102.generation),SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_destroy(
                &runtime),SALTS_COMPONENT_PLUGIN_OK);
    rtsp_mpv_dso_stop(&registry,ref102);
    check_equal(cmeta_plugin_registry_destroy(&registry),CMETA_PLUGIN_OK);
    check_equal(cnet_client_stop(&wake,1000u),SALTS_OK);
    check_equal(cnet_client_destroy(&wake),SALTS_OK);
cleanup:
    /* A failed assertion must not strand the real server callback at
     * its test-only gate while server destroy awaits quiescence. */
    atomic_store_explicit(&state.interleaved_release,1u,memory_order_release);
    if(p101)(void)tm_pc_destroy(p101);
    if(p102)(void)tm_pc_destroy(p102);
    if(client)turbo_rtsp_client_destroy(client);
    if(server)turbo_rtsp_server_destroy(server);
    rtsp_mpv_fixture_destroy(&f);
    rtsp_test_state_destroy(&state);
}
