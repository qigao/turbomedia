/* M15c/M15d staged adapter: one canonical installed Salts CMeta service
 * owns TWO independent bounded MPEG-2 Source Instances in its real DSO.
 * This is a test-owned typed envelope, not a second Plugin registry.
 * All codec calls remain exclusively owned by the admitted DSO method;
 * requests have one exact Portal completion and one CNet wake. */
enum { TM_DELAYED_INSTANCE_CAPACITY=2, TM_DELAYED_LUMA_BYTES=64*64 };
typedef struct tm_delayed_component {
    uint64_t id;
    tm_ffd_decoder *decoder;
    uint8_t luma[TM_DELAYED_LUMA_BYTES];
    size_t bytes;
    int64_t pts;
    tm_pc_ticket_v1 ticket;
    unsigned ready,destroying; /* protected by fixture_instances_lock */
    _Atomic unsigned active,cancelled;
} tm_delayed_component;
static tm_delayed_component delayed_instances[TM_DELAYED_INSTANCE_CAPACITY];

/* Shared bounded metadata lock also protects the instance registry. It is
 * NEVER held during FFmpeg packet submission, receive or Portal callbacks.
 * release() is serialized with cancel() to prevent a lost cancellation. */
static void delayed_release(tm_delayed_component *slot) {
    fixture_instances_enter();
    if(atomic_load_explicit(&slot->cancelled,memory_order_acquire)) {
        (void)tm_ffd_cancel(slot->decoder);
        slot->ready=0u;
    }
    atomic_store_explicit(&slot->active,0u,memory_order_release);
    fixture_instances_leave();
}
static tm_delayed_component *delayed_acquire(uint64_t id) {
    tm_delayed_component *result=NULL;
    unsigned i;
    fixture_instances_enter();
    for(i=0u;i<TM_DELAYED_INSTANCE_CAPACITY;++i)
        if(delayed_instances[i].id==id && !delayed_instances[i].destroying &&
           !atomic_exchange_explicit(&delayed_instances[i].active,1u,
                                     memory_order_acq_rel)) {
            result=&delayed_instances[i];
            break;
        }
    fixture_instances_leave();
    return result;
}
static int delayed_identity(void *self,const tm_test_instance_request *req) {
    return self && req && req->size==sizeof(*req) &&
        req->instance_id && req->provider_generation==(uint64_t)*(int *)self;
}
static int delayed_create(void *self,const tm_test_instance_request *req) {
    tm_ffd_config_v1 cfg=TM_FFD_CONFIG_V1_INIT;
    tm_ffd_decoder *decoder=NULL;
    unsigned i,free_slot=TM_DELAYED_INSTANCE_CAPACITY;
    int rc=-4;
    if(!delayed_identity(self,req))return -1;
    fixture_instances_enter();
    for(i=0u;i<TM_DELAYED_INSTANCE_CAPACITY;++i) {
        if(delayed_instances[i].id==req->instance_id){rc=-2;goto done;}
        if(!delayed_instances[i].id && free_slot==TM_DELAYED_INSTANCE_CAPACITY)
            free_slot=i;
    }
    if(free_slot==TM_DELAYED_INSTANCE_CAPACITY)goto done;
    cfg.width=64u;cfg.height=64u;
    cfg.queue_capacity=2u;cfg.max_packet_bytes=65536u;
    if(tm_ffd_create(&cfg,&decoder)!=TM_FF_OK){rc=-3;goto done;}
    delayed_instances[free_slot].decoder=decoder;
    delayed_instances[free_slot].id=req->instance_id;
    delayed_instances[free_slot].bytes=0u;
    delayed_instances[free_slot].ready=0u;
    delayed_instances[free_slot].destroying=0u;
    atomic_store_explicit(&delayed_instances[free_slot].cancelled,0u,
                          memory_order_release);
    rc=0;
done:
    fixture_instances_leave();
    return rc;
}
/* Reserve a noncopyable closing slot while freeing the real Codec OUTSIDE
 * the tiny metadata lock. Racing submit/cancel/take cannot obtain that slot,
 * and no active Worker can be inside FFmpeg when exclusive destroy begins. */
static int delayed_destroy(void *self,const tm_test_instance_request *req) {
    tm_delayed_component *slot=NULL;
    tm_ff_status status;
    unsigned i;
    if(!delayed_identity(self,req))return -1;
    fixture_instances_enter();
    for(i=0u;i<TM_DELAYED_INSTANCE_CAPACITY;++i) {
        if(delayed_instances[i].id!=req->instance_id)continue;
        slot=&delayed_instances[i];
        break;
    }
    if(!slot) {fixture_instances_leave();return -2;}
    if(slot->destroying || slot->ready ||
       atomic_exchange_explicit(&slot->active,1u,memory_order_acq_rel)) {
        fixture_instances_leave();
        return -4;
    }
    slot->destroying=1u;
    fixture_instances_leave();

    status=tm_ffd_destroy(slot->decoder);

    fixture_instances_enter();
    if(status==TM_FF_OK) {
        slot->decoder=NULL;slot->id=0u;slot->bytes=0u;
        memset(&slot->ticket,0,sizeof(slot->ticket));
        atomic_store_explicit(&slot->cancelled,0u,memory_order_release);
    }
    slot->destroying=0u;
    atomic_store_explicit(&slot->active,0u,memory_order_release);
    fixture_instances_leave();
    return status==TM_FF_OK?0:-4;
}
static int delayed_cancel(void *self,const tm_test_instance_request *req) {
    unsigned i;
    int rc=-2;
    if(!delayed_identity(self,req))return -1;
    fixture_instances_enter();
    for(i=0u;i<TM_DELAYED_INSTANCE_CAPACITY;++i) {
        tm_delayed_component *slot=&delayed_instances[i];
        if(slot->id!=req->instance_id)continue;
        if(slot->destroying){rc=-4;break;}
        atomic_store_explicit(&slot->cancelled,1u,memory_order_release);
        slot->ready=0u;slot->bytes=0u;
        /* If the Worker is in libavcodec, do NOT touch the Codec. The
         * original Worker performs delayed cleanup after returning.
         * If idle, the Media Owner owns the one exclusive cancel call. */
        if(!atomic_exchange_explicit(&slot->active,1u,memory_order_acq_rel)) {
            (void)tm_ffd_cancel(slot->decoder);
            atomic_store_explicit(&slot->active,0u,memory_order_release);
        }
        rc=0;break;
    }
    fixture_instances_leave();
    return rc;
}
static int delayed_finish(void *self,const tm_test_instance_request *req) {
    tm_delayed_component *slot;
    tm_ff_status rc;
    if(!delayed_identity(self,req))return -1;
    slot=delayed_acquire(req->instance_id);
    if(!slot)return -4;
    rc=atomic_load_explicit(&slot->cancelled,memory_order_acquire)
        ? TM_FF_ESTATE:tm_ffd_finish(slot->decoder);
    delayed_release(slot);
    return (int)rc;
}
static int delayed_submit(void *self,const tm_test_delayed_packet_request *req) {
    tm_delayed_component *slot;
    tm_ff_status rc;
    if(!self || !req || req->size!=sizeof(*req) || !req->instance_id ||
       req->provider_generation!=(uint64_t)*(int *)self)return -1;
    slot=delayed_acquire(req->instance_id);
    if(!slot)return -4;
    rc=atomic_load_explicit(&slot->cancelled,memory_order_acquire)
        ? TM_FF_ESTATE:tm_ffd_submit(slot->decoder,&req->packet);
    delayed_release(slot);
    return (int)rc;
}
static int delayed_stats(void *self,const tm_test_delayed_stats_request *req) {
    tm_delayed_component *slot;
    tm_ff_status rc;
    if(!self || !req || req->size!=sizeof(*req) || !req->instance_id ||
       req->provider_generation!=(uint64_t)*(int *)self || !req->out)return -1;
    slot=delayed_acquire(req->instance_id);
    if(!slot)return -4;
    rc=tm_ffd_stats(slot->decoder,req->out);
    delayed_release(slot);
    return (int)rc;
}
static int delayed_step(void *self,const tm_test_delayed_step_request *req) {
    tm_delayed_component *slot;
    tm_ff_status status;
    tm_pc_status terminal;
    int wake_rc;
    size_t bytes=0u;
    int64_t pts=INT64_MIN;
    if(!self || !req || req->size!=sizeof(*req) || !req->instance_id ||
       req->provider_generation!=(uint64_t)*(int *)self ||
       !req->portal || !req->ticket.incarnation ||
       !req->ticket.generation || !req->wake_client)return -1;
    slot=delayed_acquire(req->instance_id);
    if(!slot)return -4;
    /* One previously accepted CPU frame must be consumed/discarded before
     * a new Codec step. Never overwrite a retaining frame for progress. */
    {
        unsigned pending;
        fixture_instances_enter();
        pending=slot->ready;
        fixture_instances_leave();
        if(pending){delayed_release(slot);return -4;}
    }
    status=atomic_load_explicit(&slot->cancelled,memory_order_acquire)
        ? TM_FF_ESTATE:
          tm_ffd_next_luma(slot->decoder,slot->luma,sizeof(slot->luma),
                           &bytes,&pts);
    fixture_instances_enter();
    if(atomic_load_explicit(&slot->cancelled,memory_order_acquire)) {
        status=TM_FF_ESTATE;
        slot->ready=0u;
    } else if(status==TM_FF_OK && bytes==sizeof(slot->luma)) {
        slot->bytes=bytes;
        slot->pts=pts;
        slot->ticket=req->ticket;
        slot->ready=1u;
    } else if(status==TM_FF_OK) {
        status=TM_FF_ECODEC;
    }
    fixture_instances_leave();
    terminal=tm_pc_complete(req->portal,req->ticket,(int32_t)status);
    if(terminal!=TM_PC_OK) {
        fixture_instances_enter();
        slot->ready=0u;
        fixture_instances_leave();
        status=TM_FF_ESTATE;
    }
    /* One authoritative CNet wake after the original Portal completion.
     * Wake is not permission to release the DSO Generation Scope. */
    wake_rc=fixture_wake(req->wake_client);
    (void)wake_rc;
    delayed_release(slot);
    return (int)status;
}
static int delayed_take(void *self,const tm_test_delayed_take_request *req) {
    tm_delayed_component *slot;
    int rc=-2;
    if(!self || !req || req->size!=sizeof(*req) || !req->instance_id ||
       req->provider_generation!=(uint64_t)*(int *)self ||
       !req->luma || !req->out_bytes || !req->out_pts ||
       !req->ticket.incarnation || !req->ticket.generation)return -1;
    *req->out_bytes=0u;
    *req->out_pts=INT64_MIN;
    slot=delayed_acquire(req->instance_id);
    if(!slot)return -4;
    fixture_instances_enter();
    if(slot->ready && !atomic_load_explicit(&slot->cancelled,memory_order_acquire) &&
       slot->ticket.incarnation==req->ticket.incarnation &&
       slot->ticket.slot==req->ticket.slot &&
       slot->ticket.generation==req->ticket.generation &&
       req->capacity>=slot->bytes) {
        memcpy(req->luma,slot->luma,slot->bytes);
        *req->out_bytes=slot->bytes;
        *req->out_pts=slot->pts;
        slot->ready=0u;
        slot->bytes=0u;
        memset(&slot->ticket,0,sizeof(slot->ticket));
        rc=(int)*req->out_bytes;
    }
    fixture_instances_leave();
    delayed_release(slot);
    return rc;
}
