#ifndef TURBO_STREAMGRAPH_OWNER_DISPATCH_H
#define TURBO_STREAMGRAPH_OWNER_DISPATCH_H

#include <stdint.h>
#include <turbo_export.h>
#include <turbo_streamgraph_handoff.h>
#include <turbo_streamgraph_executor.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One owner-lane dispatcher for commands accepted by a concurrent MPSC inbox.
 * The inbox, Executor and FrameStore are borrowed; no thread/poller is created.
 * No callback may recursively call step or destroy. */
#define TM_OD_ABI_V1 1u
typedef enum tm_od_status {
    TM_OD_OK = 0, TM_OD_EMPTY = 1,
    TM_OD_EINVAL = -1, TM_OD_EVERSION = -2,
    TM_OD_ENOMEM = -3, TM_OD_EBUSY = -4,
    TM_OD_EQUEUE = -5, TM_OD_EFRAME = -6
} tm_od_status;

typedef enum tm_od_outcome {
    TM_OD_COMPLETED = 1, TM_OD_BACKPRESSURE = 2,
    TM_OD_EOS = 3, TM_OD_REJECTED = 4,
    TM_OD_STALE_FRAME = 5, TM_OD_FAILED = 6
} tm_od_outcome;

typedef int (*tm_od_frame_visit_fn)(void *ctx, const tm_fs_frame_ref_v1 *ref,
                                      const tm_fs_cpu_view_v1 *borrowed_view);

typedef struct tm_od_config_v1 {
    uint32_t size, version;
    uint32_t reserved, reserved2;
    tm_od_frame_visit_fn visit_frame;
    void *visit_context;
} tm_od_config_v1;
#define TM_OD_CONFIG_V1_INIT {sizeof(tm_od_config_v1),TM_OD_ABI_V1,0u,0u,NULL,NULL}

typedef struct tm_od_receipt_v1 {
    uint32_t size, version;
    uint32_t command_kind, outcome;
    uint64_t correlation;
    int32_t executor_status;
    int32_t frame_status;
} tm_od_receipt_v1;
#define TM_OD_RECEIPT_V1_INIT {sizeof(tm_od_receipt_v1),TM_OD_ABI_V1,0u,0u,0u,0,0}

typedef struct tm_od_owner tm_od_owner;

/* All three borrowed owners must outlive the dispatcher, then be quiesced
 * independently. This object does not destroy the Graph, Inbox or Store. */
TURBO_MEDIA_API tm_od_status tm_od_create(
    tm_ho_inbox *inbox, tm_ex_owner *executor, tm_fs_store *store,
    const tm_od_config_v1 *config, tm_od_owner **out);

/* Execute at most one accepted command, then settle its Inbox credit.
 * BACKPRESSURE is terminal for the command, NOT an automatic retry: host may
 * explicitly enqueue a new request after making progress on Sink capacity.
 * STOP seals new Inbox admission but accepted Sink commands may drain.
 * FRAME_NOTICE is a weak lookup until this owner reserves a pin; it remains
 * pinned + leased only during the bounded visitor callback. */
TURBO_MEDIA_API tm_od_status tm_od_step(tm_od_owner *owner,
                                         tm_od_receipt_v1 *out_receipt);

/* Requires Inbox sealed+drained; caller has joined all producers including
 * wake tails and ended all concurrent owner operations before destroy. */
TURBO_MEDIA_API tm_od_status tm_od_destroy(tm_od_owner *owner);

#ifdef __cplusplus
}
#endif
#endif
