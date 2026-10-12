#include <stdint.h>

#if defined(_WIN32)
#define TM_FIXTURE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define TM_FIXTURE_EXPORT __attribute__((visibility("default")))
#else
#define TM_FIXTURE_EXPORT
#endif

#ifndef TM_PROVIDER_GENERATION
#error "Each native Provider DSO needs a distinct generation."
#endif

/* TEST-ONLY DSO entry. A host callback runs while this module is on the
 * actual worker-thread call stack; unloading before the worker exits is
 * forbidden by the test host's explicit generation barrier. */
TM_FIXTURE_EXPORT uint32_t tm_streamgraph_test_provider_run(
    void (*work)(void *), void *context) {
    if (!work) return 0u;
    work(context);
    return (uint32_t)TM_PROVIDER_GENERATION;
}
