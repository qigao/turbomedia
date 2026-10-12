#include "provider_contract.h"
#include <salts/plugin_decl.h>
#include <stdatomic.h>

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
CMETA_IMPLEMENTS(tm_test_provider, tm_test_impl, 0u,
                  .get=fixture_get,
                  .entered=fixture_entered,
                  .release=fixture_release);

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
