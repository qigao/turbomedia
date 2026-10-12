#ifndef TM_TEST_REAL_COMPONENT_PROVIDER_H
#define TM_TEST_REAL_COMPONENT_PROVIDER_H

#include <salts/component_plugin_abi.h>

/* Test-owned typed ABI consumed from the *installed* Salts SDK.
 * No copied Salts fixture or private Plugin descriptor. */
#if defined(TM_TEST_FFMPEG_DSO)
#include <stddef.h>
#include <stdint.h>
#include <cnet/cnet.h>
#include <turbo_streamgraph_ffmpeg_async.h>

/* Test-only Salts Component Provider envelope over the ACTUAL installed
 * tm_ffa_decoder object. All storage is borrowed only during the call.
 * The Portal/Client lifetimes are owned by the fixed Media Owner. */
typedef struct tm_test_decode_request {
    size_t size;
    const uint8_t *packet;
    size_t packet_size;
    tm_pc_portal *portal;
    tm_pc_ticket_v1 ticket;
    cnet_client *wake_client;
} tm_test_decode_request;
typedef struct tm_test_take_request {
    size_t size;
    tm_pc_ticket_v1 ticket;
    uint8_t *luma;
    size_t capacity;
    size_t *out_bytes;
} tm_test_take_request;
typedef struct tm_test_instance_request {\n    size_t size;\n    uint64_t instance_id;\n    uint64_t provider_generation;\n} tm_test_instance_request;\n#define TM_TEST_PROVIDER_FFMPEG_METHOD(X, I) \
    X(I, R1, int, create_instance, const tm_test_instance_request *, request) \\\n    X(I, R1, int, destroy_instance, const tm_test_instance_request *, request) \\\n    X(I, R1, int, run_decode, const tm_test_decode_request *, request) \
    X(I, R1, int, take_luma, const tm_test_take_request *, request) \
    X(I, R0, int, finish_decode, _) \
    X(I, R0, int, wake_status, _)
#else
#define TM_TEST_PROVIDER_FFMPEG_METHOD(X, I)
#endif

#define TM_TEST_PROVIDER_METHODS(X, I) \
    X(I, R0, int, get, _) \
    X(I, R0, int, entered, _) \
    X(I, R0, int, release, _) \
    TM_TEST_PROVIDER_FFMPEG_METHOD(X, I)

CMETA_INTERFACE(tm_test_provider, TM_TEST_PROVIDER_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(tm_test_provider);

#define TM_TEST_PROVIDER_EXPORT_ID "tm.streamgraph.test.provider"
#endif
