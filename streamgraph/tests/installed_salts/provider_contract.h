#ifndef TM_TEST_REAL_COMPONENT_PROVIDER_H
#define TM_TEST_REAL_COMPONENT_PROVIDER_H

#include <salts/component_plugin_abi.h>

/* Test-owned typed ABI consumed from the *installed* Salts SDK.
 * No copied Salts fixture or private Plugin descriptor. */
#define TM_TEST_PROVIDER_METHODS(X, I) \
    X(I, R0, int, get, _) \
    X(I, R0, int, entered, _) \
    X(I, R0, int, release, _)

CMETA_INTERFACE(tm_test_provider, TM_TEST_PROVIDER_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(tm_test_provider);

#define TM_TEST_PROVIDER_EXPORT_ID "tm.streamgraph.test.provider"
#endif
