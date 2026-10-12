#ifndef TM_TEST_REAL_COMPONENT_PROVIDER_H
#define TM_TEST_REAL_COMPONENT_PROVIDER_H

#include <salts/component_plugin_abi.h>

/* Test-owned typed ABI consumed from the *installed* Salts SDK.
 * No copied Salts fixture or private Plugin descriptor. */
#if defined(TM_TEST_FFMPEG_DSO)
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <cnet/cnet.h>
#include <turbo_streamgraph_ffmpeg_async.h>
#include <turbo_streamgraph_ffmpeg_delayed.h>

/* Test-only Salts Component Provider envelope over the ACTUAL installed
 * tm_ffa_decoder object. All storage is borrowed only during the call.
 * The Portal/Client lifetimes are owned by the fixed Media Owner. */
typedef struct tm_test_decode_request {
    size_t size;
    const uint8_t *packet;
    size_t packet_size;
    uint64_t instance_id; /* zero selects the legacy M14 fixture */
    tm_pc_portal *portal;
    tm_pc_ticket_v1 ticket;
    cnet_client *wake_client;
} tm_test_decode_request;
typedef struct tm_test_take_request {
    size_t size;
    tm_pc_ticket_v1 ticket;
    uint64_t instance_id;
    uint8_t *luma;
    size_t capacity;
    size_t *out_bytes;
} tm_test_take_request;
typedef struct tm_test_instance_request {
    size_t size;
    uint64_t instance_id;
    uint64_t provider_generation;
} tm_test_instance_request;
/* Linux installed-SDK-only typed CMeta envelope for the real bounded
 * MPEG-2 delayed Provider. Per-operation buffers are copied by tm_ffd_submit;
 * the decode worker stays within the live Salts Generation Scope. */
typedef struct tm_test_delayed_packet_request {
    size_t size;
    uint64_t instance_id,provider_generation;
    tm_ffd_packet_v1 packet;
} tm_test_delayed_packet_request;
/* Optional test-only pre-Codec barrier. The Host keeps this storage
 * and the real Salts Generation Scope alive until the DSO caller joins.
 * Entry is signalled only after the original Instance becomes active. */
typedef struct tm_test_delayed_step_gate {
    _Atomic unsigned entered,release,exited;
} tm_test_delayed_step_gate;
typedef struct tm_test_delayed_step_request {
    size_t size;
    uint64_t instance_id,provider_generation;
    tm_pc_portal *portal;
    tm_pc_ticket_v1 ticket;
    cnet_client *wake_client;
    tm_test_delayed_step_gate *test_gate; /* borrowed, opt-in test only */
} tm_test_delayed_step_request;
typedef struct tm_test_delayed_take_request {
    size_t size;
    uint64_t instance_id,provider_generation;
    tm_pc_ticket_v1 ticket;
    uint8_t *luma;
    size_t capacity;
    size_t *out_bytes;
    int64_t *out_pts;
} tm_test_delayed_take_request;
typedef struct tm_test_delayed_stats_request {
    size_t size;
    uint64_t instance_id,provider_generation;
    tm_ffd_stats_v1 *out;
} tm_test_delayed_stats_request;
#define TM_TEST_PROVIDER_FFMPEG_METHOD(X, I) \
    X(I, R1, int, create_instance, const tm_test_instance_request *, request) \
    X(I, R1, int, destroy_instance, const tm_test_instance_request *, request) \
    X(I, R1, int, cancel_instance, const tm_test_instance_request *, request) \
    X(I, R1, int, run_decode, const tm_test_decode_request *, request) \
    X(I, R1, int, run_eof, const tm_test_decode_request *, request) \
    X(I, R1, int, take_luma, const tm_test_take_request *, request) \
    X(I, R1, int, create_delayed, const tm_test_instance_request *, request) \
    X(I, R1, int, destroy_delayed, const tm_test_instance_request *, request) \
    X(I, R1, int, cancel_delayed, const tm_test_instance_request *, request) \
    X(I, R1, int, finish_delayed, const tm_test_instance_request *, request) \
    X(I, R1, int, submit_delayed, const tm_test_delayed_packet_request *, request) \
    X(I, R1, int, step_delayed, const tm_test_delayed_step_request *, request) \
    X(I, R1, int, take_delayed, const tm_test_delayed_take_request *, request) \
    X(I, R1, int, stats_delayed, const tm_test_delayed_stats_request *, request) \
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
