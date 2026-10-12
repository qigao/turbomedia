#ifndef TURBO_STREAMGRAPH_EXECUTOR_H
#define TURBO_STREAMGRAPH_EXECUTOR_H
#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>
#include <turbo_streamgraph.h>
#include <turbo_frame_store.h>
#include <turbo_streamgraph_fanout.h>
#ifdef __cplusplus
extern "C" {
#endif

/* First executable owner-driven CPU VideoFrame profile:
 * Source -> Processor -> 1..8 Sinks. Not a generic FFmpeg/RTC scheduler. */
#define TM_EX_ABI_V1 1u
typedef enum tm_ex_status {
    TM_EX_OK=0, TM_EX_EINVAL=-1, TM_EX_EVERSION=-2,
    TM_EX_EGRAPH=-3, TM_EX_ENOMEM=-4, TM_EX_EFULL=-5,
    TM_EX_EBUSY=-6, TM_EX_ESTATE=-7, TM_EX_EPROVIDER=-8,
    TM_EX_EFRAME=-9, TM_EX_EOS=1
} tm_ex_status;
typedef enum tm_ex_source_result {
    TM_EX_SOURCE_FRAME=1, TM_EX_SOURCE_EOS=2, TM_EX_SOURCE_FAILED=3
} tm_ex_source_result;

typedef tm_ex_source_result (*tm_ex_source_next_fn)(
    void *ctx, uint8_t *dst, size_t capacity, size_t *size_bytes);
typedef int (*tm_ex_visit_fn)(void *ctx, const tm_fs_frame_ref_v1 *ref,
                              const tm_fs_cpu_view_v1 *frame);
typedef struct tm_ex_binding_v1 {
    uint32_t size,version;
    const char *node_id;
    tm_ex_source_next_fn source_next;
    tm_ex_visit_fn visit;
    void *ctx;
} tm_ex_binding_v1;
#define TM_EX_BINDING_V1_INIT {sizeof(tm_ex_binding_v1),TM_EX_ABI_V1,NULL,NULL,NULL,NULL}
typedef struct tm_ex_config_v1 {
    uint32_t size,version,max_frame_bytes,reserved;
    uint64_t source_session_generation,display_generation;
    tm_fo_config_v1 fanout;
} tm_ex_config_v1;
#define TM_EX_CONFIG_V1_INIT {sizeof(tm_ex_config_v1),TM_EX_ABI_V1,0u,0u,0u,0u,TM_FO_CONFIG_V1_INIT}
typedef struct tm_ex_stats_v1 {
    uint32_t size,version;
    uint64_t sourced,submitted,processed,delivered,backpressure;
    uint32_t pending,stopped;
} tm_ex_stats_v1;
#define TM_EX_STATS_V1_INIT {sizeof(tm_ex_stats_v1),TM_EX_ABI_V1,0u,0u,0u,0u,0u,0u,0u}
typedef struct tm_ex_owner tm_ex_owner;

/* Immutable program and Store are borrowed. All APIs/callbacks are serialized
 * by one external owner; callbacks must not recursively drive this executor.
 * Source produces into a fixed-capacity owner scratch buffer. Processor/Sinks
 * borrow mapped read-only FrameLease views for the callback only. */
TURBO_MEDIA_API tm_ex_status tm_ex_create(
    const tm_sg_program *program,tm_fs_store *store,
    const tm_ex_config_v1 *config,const tm_ex_binding_v1 *bindings,
    size_t binding_count,tm_ex_owner **out);
/* Full Fanout or FrameStore never advances the Source again while one
 * produced frame remains uncommitted. One pending frame at most. */
TURBO_MEDIA_API tm_ex_status tm_ex_pump_source(tm_ex_owner *owner);
TURBO_MEDIA_API tm_ex_status tm_ex_pump_sink(tm_ex_owner *owner,uint32_t branch);
/* Stop prevents new source admission, seals Fanout and abandons uncommitted
 * input; READY/TAKEN sink credits still need explicit drain/settlement. */
TURBO_MEDIA_API tm_ex_status tm_ex_stop(tm_ex_owner *owner);
TURBO_MEDIA_API tm_ex_status tm_ex_discard_ready(tm_ex_owner *owner,uint32_t branch);
TURBO_MEDIA_API tm_ex_status tm_ex_stats(tm_ex_owner *owner,tm_ex_stats_v1 *out);
TURBO_MEDIA_API tm_ex_status tm_ex_destroy(tm_ex_owner *owner);
#ifdef __cplusplus
}
#endif
#endif
