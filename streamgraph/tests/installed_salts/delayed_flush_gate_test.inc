/* Linux pinned FFmpeg 6.1.1 ONLY. Genuine delayed MPEG-2 CMeta Instance
 * is held INSIDE avcodec_send_packet(NULL) with its one real packet already
 * admitted and the original Salts Generation Scope retained. The pause is
 * after libavcodec set draining_started, not before tm_ffd_next_luma. */
extern int avcodec_tm_test_arm_flush(void);
extern unsigned avcodec_tm_test_flush_state(void);
extern int avcodec_tm_test_release_flush(void);

static void check_delayed_flush_under_scope(
    tm_test_provider *service,
    salts_component_plugin_runtime *runtime,
    salts_component_plugin_generation *generation,
    cmeta_plugin_registry *registry,cmeta_plugin_ref plugin,
    cnet_client *wake,AVPacket *packet) {
    tm_test_instance_request instance={sizeof(instance),903u,102u};
    tm_pc_portal *portal=make_portal(102u);
    unsigned i,cancelled=0u;
    salts_component_plugin_generation *closed=NULL;
    assert(tm_test_provider_create_delayed(service,&instance)==0);
    assert(submit(service,instance,packet)==TM_FF_OK);
    assert(tm_test_provider_finish_delayed(service,&instance)==TM_FF_OK);
    assert(avcodec_tm_test_arm_flush()==0);
    for(i=0u;i<12u;++i) {
        tm_pc_ticket_v1 ticket={0},taken={0};
        tm_pc_result_v1 terminal=TM_PC_RESULT_V1_INIT;
        tm_test_delayed_step_request request={0};
        step_worker worker={0};
        pthread_t thread;
        unsigned spins=0u;
        assert(tm_pc_begin(portal,19000u+i,&ticket)==TM_PC_OK);
        request.size=sizeof(request);
        request.instance_id=instance.instance_id;
        request.provider_generation=instance.provider_generation;
        request.portal=portal;request.ticket=ticket;request.wake_client=wake;
        worker.service=*service;worker.request=request;
        atomic_init(&worker.done,0u);
        assert(pthread_create(&thread,NULL,run_step,&worker)==0);
        while(avcodec_tm_test_flush_state()!=2u &&
              !atomic_load_explicit(&worker.done,memory_order_acquire) &&
              ++spins<5000000u)sched_yield();
        if(avcodec_tm_test_flush_state()==2u) {
            /* Actual libavcodec is now executing its NULL-packet function.
             * Destroy, DSO unload and Generation drain must not win this
             * previously admitted Codec operation. */
            assert(tm_test_provider_destroy_delayed(service,&instance)==-4);
            assert(salts_component_plugin_runtime_close(
                       runtime,&closed)==SALTS_COMPONENT_PLUGIN_OK);
            assert(closed==generation);
            assert(salts_component_plugin_generation_drain(
                       runtime,generation)==SALTS_COMPONENT_PLUGIN_BUSY);
            assert(cmeta_plugin_registry_unload(registry,plugin)==CMETA_PLUGIN_BUSY);
            assert(tm_pc_request_cancel(portal,ticket)==TM_PC_OK);
            assert(tm_test_provider_cancel_delayed(service,&instance)==0);
            assert(tm_test_provider_destroy_delayed(service,&instance)==-4);
            assert(avcodec_tm_test_release_flush()==0);
            assert(pthread_join(thread,NULL)==0);
            assert(avcodec_tm_test_flush_state()==4u);
            assert(worker.rc==TM_FF_ESTATE);
            assert(atomic_load_explicit(&worker.done,memory_order_acquire)==1u);
            assert(tm_test_provider_wake_status(service)==SALTS_OK);
            drain_wake(wake);
            assert(tm_pc_take(portal,&taken,&terminal)==TM_PC_OK);
            assert(terminal.correlation==19000u+i &&
                   terminal.provider_generation==102u &&
                   terminal.outcome==TM_PC_CANCELLED &&
                   terminal.completion_observed==1u &&
                   terminal.provider_status==TM_FF_ESTATE);
            assert(tm_pc_settle(portal,taken)==TM_PC_OK);
            assert(tm_pc_take(portal,&taken,&terminal)!=TM_PC_OK);
            /* No stale pixel may be taken after the original cancelled
             * terminal; wrong ticket identity must also remain rejected. */
            {
                tm_test_delayed_take_request take={0};
                tm_test_delayed_stats_request stats_request={0};
                tm_ffd_stats_v1 codec_stats=TM_FFD_STATS_V1_INIT;
                uint8_t pixels[LUMA];
                size_t bytes=99u;
                int64_t pts=99;
                take.size=sizeof(take);
                take.instance_id=instance.instance_id;
                take.provider_generation=instance.provider_generation;
                take.ticket=ticket;
                take.luma=pixels;take.capacity=sizeof(pixels);
                take.out_bytes=&bytes;take.out_pts=&pts;
                assert(tm_test_provider_take_delayed(service,&take)==-2);
                assert(bytes==0u && pts==INT64_MIN);
                take.ticket.generation++;
                assert(tm_test_provider_take_delayed(service,&take)==-2);
                stats_request.size=sizeof(stats_request);
                stats_request.instance_id=instance.instance_id;
                stats_request.provider_generation=instance.provider_generation;
                stats_request.out=&codec_stats;
                assert(tm_test_provider_stats_delayed(service,&stats_request)==TM_FF_OK);
                assert(codec_stats.cancelled==1u);
                assert(codec_stats.queued==0u);
                assert(codec_stats.frames_decoded<=1u);
                assert(codec_stats.flush_sends==1u);
            }
            cancelled=1u;
            break;
        }
        /* Some genuine delayed Codec profiles may yield a frame before
         * flushing; settle those receipts explicitly. Never falsely claim
         * a pre-call gate was inside libavcodec. */
        assert(atomic_load_explicit(&worker.done,memory_order_acquire)==1u);
        assert(pthread_join(thread,NULL)==0);
        assert(tm_test_provider_wake_status(service)==SALTS_OK);
        drain_wake(wake);
        assert(worker.rc==TM_FF_OK || worker.rc==TM_FF_EAGAIN);
        assert(tm_pc_take(portal,&taken,&terminal)==TM_PC_OK);
        assert(terminal.provider_generation==102u &&
               terminal.outcome==TM_PC_COMPLETED &&
               terminal.provider_status==worker.rc);
        if(worker.rc==TM_FF_OK) {
            tm_test_delayed_take_request take={0};
            uint8_t luma[LUMA];
            size_t bytes=0u;
            int64_t pts=INT64_MIN;
            take.size=sizeof(take);
            take.instance_id=instance.instance_id;
            take.provider_generation=102u;
            take.ticket=ticket;
            take.luma=luma;take.capacity=sizeof(luma);
            take.out_bytes=&bytes;take.out_pts=&pts;
            assert(tm_test_provider_take_delayed(service,&take)==LUMA);
            assert(bytes==LUMA);
        }
        assert(tm_pc_settle(portal,taken)==TM_PC_OK);
    }
    assert(cancelled==1u);
    assert(tm_test_provider_destroy_delayed(service,&instance)==0);
    assert(tm_pc_seal(portal)==TM_PC_OK);
    assert(tm_pc_destroy(portal)==TM_PC_OK);
}
