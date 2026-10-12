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
    if(!self || !job || job->size!=sizeof(*job) ||
       !job->packet || !job->packet_size || job->packet_size>65536u ||
       !job->portal || !job->ticket.incarnation ||
       !job->ticket.generation || !job->wake_client ||
       fixture_decoder!=NULL)return -1;
    config.width=16u;
    config.height=16u;
    config.max_packet_bytes=65536u;
    rc=tm_ffa_create(&config,&fixture_decoder);
    if(rc!=TM_FF_OK)return -2;
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
    return rc==TM_FF_OK ? *(int *)self : -3;
}

static int fixture_take_luma(void *self,const tm_test_take_request *request) {
    tm_ff_status rc;
    tm_ex_source_result got;
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
                  .run_decode=fixture_run_decode,
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
