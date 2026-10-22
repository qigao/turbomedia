#include "provider_contract.h"
#include <salts/plugin_decl.h>
#include <stdatomic.h>
#ifdef TM_TEST_FFMPEG_DSO
#include <salts/error_codes.h>
#include <turbo_streamgraph_ffmpeg_async.h>
#endif

#ifndef TM_TEST_DSO_GENERATION
#error TM_TEST_DSO_GENERATION must be set on each DSO target
#endif

cmeta_component(StreamGraphTestProvider, cmeta_provides(tm_test_provider));

static int generation_value = TM_TEST_DSO_GENERATION;
static _Atomic unsigned in_get = 0u;
static _Atomic unsigned permit_return = 0u;

static int fixture_get(void *self) {
    atomic_store_explicit(&in_get, 1u, memory_order_release);
    while (!atomic_load_explicit(&permit_return, memory_order_acquire)) {
        /* Real provider method stays on the DSO call stack until released. */
    }
    return *(int *)self;
}
static int fixture_entered(void *self) {
    (void)self;
    return (int)atomic_load_explicit(&in_get, memory_order_acquire);
}
static int fixture_release(void *self) {
    (void)self;
    atomic_store_explicit(&permit_return,1u,memory_order_release);
    return 0;
}
#ifdef TM_TEST_FFMPEG_DSO
/* One address-stable production-style decoder per test Provider DSO.
 * Allocated through the actual tm_ffa_* API, NOT private copied libavcodec
 * logic. The Module Scope keeps the DSO code and borrowed service valid. */
static tm_ffa_decoder *fixture_decoder=NULL;
static _Atomic int fixture_last_wake=SALTS_EINVAL;
/* Includes the deterministic admission barrier BEFORE tm_ffa_run_mjpeg,
 * not only its internal worker_active flag. A scope/decoder may not be
 * retired while any CMeta DSO method still owns the native call stack. */
static _Atomic unsigned fixture_call_active=0u;

/* M15a bounded independent instance factory, in addition to the original
 * M14 one-request fixture. Each slot owns an actual tm_ffa_decoder.
 * Host must destroy all slots under the original live Generation Scope. */
enum { TM_INSTANCE_CAPACITY=2 };
typedef struct tm_fixture_instance {
    uint64_t id;
    tm_ffa_decoder *decoder;
    _Atomic unsigned active;
} tm_fixture_instance;
static tm_fixture_instance fixture_instances[TM_INSTANCE_CAPACITY];
static atomic_flag fixture_instances_lock=ATOMIC_FLAG_INIT;
static void fixture_instances_enter(void) {
    while(atomic_flag_test_and_set_explicit(&fixture_instances_lock,
                                            memory_order_acquire)) {}
}
static void fixture_instances_leave(void) {
    atomic_flag_clear_explicit(&fixture_instances_lock,memory_order_release);
}
static int fixture_instance_create(void *self,const tm_test_instance_request *req) {
    tm_ff_config_v1 cfg=TM_FF_CONFIG_V1_INIT;
    tm_ffa_decoder *decoder=NULL;
    unsigned i,free_slot=TM_INSTANCE_CAPACITY;
    int result=-4;
    if(!self || !req || req->size!=sizeof(*req) || !req->instance_id ||
       req->provider_generation!=(uint64_t)*(int *)self)return -1;
    fixture_instances_enter();
    for(i=0u;i<TM_INSTANCE_CAPACITY;++i) {
        if(fixture_instances[i].id==req->instance_id) { result=-2;goto done; }
        if(!fixture_instances[i].id && free_slot==TM_INSTANCE_CAPACITY)free_slot=i;
    }
    if(free_slot==TM_INSTANCE_CAPACITY)goto done;
    cfg.width=16u;cfg.height=16u;cfg.max_packet_bytes=65536u;
    if(tm_ffa_create(&cfg,&decoder)!=TM_FF_OK) { result=-3;goto done; }
    fixture_instances[free_slot].decoder=decoder;
    fixture_instances[free_slot].id=req->instance_id;
    result=0;
done:
    fixture_instances_leave();
    return result;
}
static int fixture_instance_destroy(void *self,const tm_test_instance_request *req) {
    unsigned i;
    int result=-2;
    if(!self || !req || req->size!=sizeof(*req) || !req->instance_id ||
       req->provider_generation!=(uint64_t)*(int *)self)return -1;
    fixture_instances_enter();
    for(i=0u;i<TM_INSTANCE_CAPACITY;++i) {
        if(fixture_instances[i].id!=req->instance_id)continue;
        /* destroy is exclusive after owner-controlled join and settlement.
         * No auto-retry; an active Codec retains the slot and DSO Scope. */
        if(atomic_load_explicit(&fixture_instances[i].active,memory_order_acquire) ||
           tm_ffa_destroy(fixture_instances[i].decoder)!=TM_FF_OK) {
            result=-4;break;
        }
        fixture_instances[i].decoder=NULL;
        fixture_instances[i].id=0u;
        result=0;break;
    }
    fixture_instances_leave();
    return result;
}
static int fixture_wake(void *context);
/* Shared DSO CMeta service includes a separate, bounded MPEG-2 Instance
 * family; all operations use the installed FFmpeg addon, not copied Codec
 * implementation. The single canonical Salts Scope still guards both. */
#include "delayed_component_provider.inc"
static int fixture_instance_cancel(void *self,const tm_test_instance_request *req) {
    unsigned i;
    int rc=-2;
    if(!self || !req || req->size!=sizeof(*req) || !req->instance_id ||
       req->provider_generation!=(uint64_t)*(int *)self)return -1;
    fixture_instances_enter();
    for(i=0u;i<TM_INSTANCE_CAPACITY;++i)
        if(fixture_instances[i].id==req->instance_id) {
            rc=tm_ffa_owner_cancel(fixture_instances[i].decoder)==TM_FF_OK?0:-3;
            break;
        }
    fixture_instances_leave();
    return rc;
}
static tm_fixture_instance *fixture_instance_acquire(uint64_t id) {
    unsigned i;
    tm_fixture_instance *slot=NULL;
    fixture_instances_enter();
    for(i=0u;i<TM_INSTANCE_CAPACITY;++i)
        if(fixture_instances[i].id==id &&
           !atomic_exchange_explicit(&fixture_instances[i].active,1u,
                                     memory_order_acq_rel)) {
            slot=&fixture_instances[i];
            break;
        }
    fixture_instances_leave();
    return slot;
}
static void fixture_instance_release(tm_fixture_instance *slot) {
    atomic_store_explicit(&slot->active,0u,memory_order_release);
}
/* The instance call retains an active lease through the actual Codec call,
 * Portal completion, and wake callback. Destroy refuses while it is active. */
static int fixture_run_instance(void *self,const tm_test_decode_request *job) {
    tm_fixture_instance *slot;
    tm_ff_status rc;
    if(!self || !job || job->size!=sizeof(*job) || !job->instance_id ||
       !job->packet || !job->packet_size || job->packet_size>65536u ||
       !job->portal || !job->wake_client || !job->ticket.incarnation ||
       !job->ticket.generation)return -1;
    slot=fixture_instance_acquire(job->instance_id);
    if(!slot)return -4;
    rc=tm_ffa_run_mjpeg(slot->decoder,job->packet,job->packet_size,
                        job->portal,job->ticket,fixture_wake,job->wake_client);
    fixture_instance_release(slot);
    return rc==TM_FF_OK?*(int *)self:-3;
}
/* Only the actual independent Instance can flush its worker-owned Codec.
 * The host retains the same installed Salts Generation Scope through this
 * CMeta method, its Portal completion and its native CNet wake tail.
 * A NULL packet never goes through the old one-shot singleton path. */
static int fixture_run_eof(void *self,const tm_test_decode_request *job) {
    tm_fixture_instance *slot;
    tm_ff_status rc;
    if(!self || !job || job->size!=sizeof(*job) ||
       !job->instance_id || job->packet || job->packet_size ||
       !job->portal || !job->wake_client || !job->ticket.incarnation ||
       !job->ticket.generation)return -1;
    slot=fixture_instance_acquire(job->instance_id);
    if(!slot)return -4;
    rc=tm_ffa_run_eof(slot->decoder,job->portal,job->ticket,
                      fixture_wake,job->wake_client);
    fixture_instance_release(slot);
    return (int)rc;
}
static int fixture_take_instance(void *self,const tm_test_take_request *req) {
    tm_fixture_instance *slot;
    tm_ff_status rc;
    tm_ex_source_result got;
    if(!self || !req || req->size!=sizeof(*req) || !req->instance_id ||
       !req->luma || !req->out_bytes)return -1;
    *req->out_bytes=0u;
    slot=fixture_instance_acquire(req->instance_id);
    if(!slot)return -4;
    rc=tm_ffa_owner_accept(slot->decoder,req->ticket);
    if(rc!=TM_FF_OK) { fixture_instance_release(slot);return -2; }
    got=tm_ffa_owner_next_luma(slot->decoder,req->luma,req->capacity,req->out_bytes);
    fixture_instance_release(slot);
    return got==TM_EX_SOURCE_FRAME && *req->out_bytes==256u?256:-3;
}
/* This callback executes on the REAL Provider DSO call stack AFTER
 * tm_ffa_run_mjpeg publishes its exact terminal to the Portal. N can be
 * cancelled/retired while the DSO remains on this stack. */
static int fixture_wake(void *context) {
    cnet_client *client=(cnet_client *)context;
    int rc=cnet_client_wake(client);
    atomic_store_explicit(&fixture_last_wake,rc,memory_order_release);
    return rc==SALTS_OK ? 0 : -1;
}

static int fixture_run_decode(void *self,const tm_test_decode_request *job) {
    tm_ff_config_v1 config=TM_FF_CONFIG_V1_INIT;
    tm_ff_status rc;
    if(job && job->instance_id)return fixture_run_instance(self,job);
    if(!self || !job || job->size!=sizeof(*job) ||
       !job->packet || !job->packet_size || job->packet_size>65536u ||
       !job->portal || !job->ticket.incarnation ||
       !job->ticket.generation || !job->wake_client ||
       fixture_decoder!=NULL)return -1;
    if(atomic_exchange_explicit(&fixture_call_active,1u,
                                memory_order_acq_rel)!=0u)return -4;
    config.width=16u;
    config.height=16u;
    config.max_packet_bytes=65536u;
    rc=tm_ffa_create(&config,&fixture_decoder);
    if(rc!=TM_FF_OK) {
        atomic_store_explicit(&fixture_call_active,0u,memory_order_release);
        return -2;
    }
    /* Admission is now inside the actual Plugin DSO; this deterministic
     * gate allows the owner to publish N+1 and CANCEL the old Portal ticket
     * BEFORE the old codec call commits a completion. The late real FFmpeg
     * decode must still execute under the retained old Scope, but its
     * completed pixels may not leak into a cancelled StreamGraph branch.
     * Not a proof of cancellation during libavcodec's inner instructions. */
    atomic_store_explicit(&in_get,1u,memory_order_release);
    while(!atomic_load_explicit(&permit_return,memory_order_acquire)) {}
    rc=tm_ffa_run_mjpeg(fixture_decoder,job->packet,job->packet_size,
                        job->portal,job->ticket,fixture_wake,job->wake_client);
    atomic_store_explicit(&fixture_call_active,0u,memory_order_release);
    return rc==TM_FF_OK ? *(int *)self : -3;
}

static int fixture_take_luma(void *self,const tm_test_take_request *request) {
    tm_ff_status rc;
    tm_ex_source_result got;
    if(request && request->instance_id)return fixture_take_instance(self,request);
    if(!self || !request || request->size!=sizeof(*request) ||
       !request->luma || !request->out_bytes || !fixture_decoder)return -1;
    *request->out_bytes=0u;
    rc=tm_ffa_owner_accept(fixture_decoder,request->ticket);
    if(rc!=TM_FF_OK)return -2;
    got=tm_ffa_owner_next_luma(fixture_decoder,request->luma,
                               request->capacity,request->out_bytes);
    return got==TM_EX_SOURCE_FRAME && *request->out_bytes==256u ? 256 : -3;
}

/* Only the Media Owner calls this AFTER callback-thread join, terminal
 * settlement, and any retained decoded frame has been consumed/discarded.
 * No ComponentPlugin Scope is acquired/released by tm_ffa_destroy itself. */
static int fixture_finish_decode(void *self) {
    tm_ff_status rc;
    if(!self || !fixture_decoder)return -1;
    if(atomic_load_explicit(&fixture_call_active,memory_order_acquire))
        return -4; /* cannot release a decoder during DSO callback tail */
    rc=tm_ffa_owner_cancel(fixture_decoder);
    if(rc!=TM_FF_OK)return -2;
    rc=tm_ffa_destroy(fixture_decoder);
    if(rc!=TM_FF_OK)return -3;
    fixture_decoder=NULL;
    return 0;
}
static int fixture_wake_status(void *self) {
    if(!self)return SALTS_EINVAL;
    return atomic_load_explicit(&fixture_last_wake,memory_order_acquire);
}
CMETA_IMPLEMENTS(tm_test_provider, tm_test_impl, 0u,
                  .get=fixture_get,
                  .entered=fixture_entered,
                  .release=fixture_release,
                  .create_instance=fixture_instance_create, 
                  .destroy_instance=fixture_instance_destroy,
                  .cancel_instance=fixture_instance_cancel, 
                  .run_decode=fixture_run_decode,
                  .run_eof=fixture_run_eof,
                  .create_delayed=delayed_create,
                  .destroy_delayed=delayed_destroy,
                  .cancel_delayed=delayed_cancel,
                  .finish_delayed=delayed_finish,
                  .submit_delayed=delayed_submit,
                  .step_delayed=delayed_step,
                  .take_delayed=delayed_take,
                  .stats_delayed=delayed_stats,
                  .take_luma=fixture_take_luma,
                  .finish_decode=fixture_finish_decode,
                  .wake_status=fixture_wake_status);
#else
CMETA_IMPLEMENTS(tm_test_provider, tm_test_impl, 0u,
                  .get=fixture_get,
                  .entered=fixture_entered,
                  .release=fixture_release);
#endif

static cmeta_status fixture_project(
    void *ctx, const cmeta_object_ref *object,
    const cmeta_interface_desc *expected, cmeta_interface_projection *out) {
    (void)ctx;
    if (!object || !out) return CMETA_INVALID_ARGUMENT;
    if (!cmeta_interface_desc_equal(expected, tm_test_provider_interface()))
        return CMETA_TRAIT_MISSING;
    out->size=sizeof(*out);
    out->self=object->object;
    out->interface=tm_test_provider_interface();
    out->dispatch=&tm_test_impl_vtable;
    return CMETA_OK;
}
static const cmeta_object_interface_provider interfaces = {
    sizeof(cmeta_object_interface_provider), NULL, fixture_project
};
static cmeta_status SALTS_COMPONENT_CALL fixture_create(
    void *context, const cmeta_data_desc *config,
    const void *config_value, const salts_component_dependency *deps,
    size_t dep_count, cmeta_object_ref *out) {
    (void)deps;
    if (!context || config || config_value || dep_count)
        return CMETA_INVALID_ARGUMENT;
    return cmeta_object_borrow(out,context,&cmeta_data_int,NULL);
}
static const salts_component_provider_binding binding = {
    sizeof(salts_component_provider_binding),
    SALTS_COMPONENT_PROVIDER_BINDING_ABI_VERSION,
    cmeta_component_meta(StreamGraphTestProvider),
    &generation_value, &interfaces, fixture_create, NULL, NULL
};
static const salts_component_provider_binding *get_binding(void *self) {
    return (const salts_component_provider_binding *)self;
}
CMETA_IMPLEMENTS(salts_component_provider, tm_component_provider_impl, 0u,
                  .get_binding=get_binding);
static salts_component_provider provider = {
    (void *)&binding, &tm_component_provider_impl_vtable
};
#define TM_TEST_EXPORTS(X) \
    X(interface, (salts_component_provider, &provider), \
      TM_TEST_PROVIDER_EXPORT_ID, SALTS_COMPONENT_PROVIDER_CONTRACT_ID, \
      SALTS_COMPONENT_PROVIDER_CONTRACT_VERSION, 0)

#define TM_TEST_STRINGIZE_I(x) #x
#define TM_TEST_STRINGIZE(x) TM_TEST_STRINGIZE_I(x)
CMETA_PLUGIN_DECLARE(streamgraph_real_component_fixture,
    "tm.streamgraph.component.provider.gen" TM_TEST_STRINGIZE(TM_TEST_DSO_GENERATION), (1,0,0),
    TM_TEST_EXPORTS, CMETA_PLUGIN_PASSIVE());
