#ifndef TURBO_STREAMGRAPH_PROVIDER_COMPLETION_H
#define TURBO_STREAMGRAPH_PROVIDER_COMPLETION_H

#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded asynchronous Provider completion/cancellation registry.
 * Provider callback threads may ONLY complete a pre-admitted ticket; they
 * never advance the StreamGraph Executor or release Component/DSO lifetimes.
 * No worker, poller, implicit retry, or dynamically growing queue. */
#define TM_PC_ABI_V1 1u
#define TM_PC_MAX_CAPACITY 1024u

typedef enum tm_pc_status {
    TM_PC_OK=0, TM_PC_EINVAL=-1, TM_PC_EVERSION=-2,
    TM_PC_ENOMEM=-3, TM_PC_EFULL=-4, TM_PC_ESTALE=-5,
    TM_PC_EALREADY=-6, TM_PC_EBUSY=-7,
    TM_PC_ESHUTDOWN=-8, TM_PC_EEXHAUSTED=-9
} tm_pc_status;

typedef enum tm_pc_outcome {
    TM_PC_COMPLETED=1, TM_PC_CANCELLED=2, TM_PC_START_FAILED=3
} tm_pc_outcome;

typedef struct tm_pc_config_v1 {
    uint32_t size,version,capacity,reserved;
    uint64_t provider_generation;
} tm_pc_config_v1;
#define TM_PC_CONFIG_V1_INIT {sizeof(tm_pc_config_v1),TM_PC_ABI_V1,0u,0u,0u}

typedef struct tm_pc_ticket_v1 {
    uint64_t incarnation;
    uint32_t slot,generation;
} tm_pc_ticket_v1;

typedef struct tm_pc_result_v1 {
    uint32_t size,version;
    uint32_t outcome,completion_observed;
    uint64_t correlation,provider_generation;
    int32_t provider_status;
    int32_t reserved;
} tm_pc_result_v1;
#define TM_PC_RESULT_V1_INIT {sizeof(tm_pc_result_v1),TM_PC_ABI_V1,0u,0u,0u,0u,0,0}

typedef struct tm_pc_snapshot_v1 {
    uint32_t size,version,capacity,inflight;
    uint32_t cancel_pending,ready,taken,sealed,drained;
} tm_pc_snapshot_v1;
#define TM_PC_SNAPSHOT_V1_INIT {sizeof(tm_pc_snapshot_v1),TM_PC_ABI_V1,0u,0u,0u,0u,0u,0u,0u}

typedef struct tm_pc_portal tm_pc_portal;

/* All operations except create/destroy are serialized internally. Owner must
 * begin BEFORE calling Provider start; callback may complete immediately. */
TURBO_MEDIA_API tm_pc_status tm_pc_create(
    const tm_pc_config_v1 *config,tm_pc_portal **out);
TURBO_MEDIA_API tm_pc_status tm_pc_begin(
    tm_pc_portal *portal,uint64_t correlation,tm_pc_ticket_v1 *out);
/* Called after a synchronous Provider start failure, iff no completion can
 * ever arrive for that ticket. This creates a terminal result; no retry. */
TURBO_MEDIA_API tm_pc_status tm_pc_fail_start(
    tm_pc_portal *portal,tm_pc_ticket_v1 ticket,int32_t status);
/* May run from a Provider-owned callback thread. If cancellation won first,
 * completion is observed but the terminal outcome remains CANCELLED.
 * Callback does not free Provider storage. */
TURBO_MEDIA_API tm_pc_status tm_pc_complete(
    tm_pc_portal *portal,tm_pc_ticket_v1 ticket,int32_t status);
/* Owner requests cancellation; Provider cancellation executes OUTSIDE portal.
 * CANCEL_PENDING retains all credits and Provider generation until terminal
 * completion or a genuinely quiescent cancellation acknowledgment arrives. */
TURBO_MEDIA_API tm_pc_status tm_pc_request_cancel(
    tm_pc_portal *portal,tm_pc_ticket_v1 ticket);
/* Provider-owner reports terminal cancellation only after proving no callback
 * for this ticket can subsequently run. A competing completion wins one
 * serialized terminal transition; duplicate terminal calls are rejected. */
TURBO_MEDIA_API tm_pc_status tm_pc_cancel_quiesced(
    tm_pc_portal *portal,tm_pc_ticket_v1 ticket);
/* The Media Owner alone consumes terminal outcomes and settles rows. */
TURBO_MEDIA_API tm_pc_status tm_pc_take(
    tm_pc_portal *portal,tm_pc_ticket_v1 *out_ticket,
    tm_pc_result_v1 *out_result);
TURBO_MEDIA_API tm_pc_status tm_pc_settle(
    tm_pc_portal *portal,tm_pc_ticket_v1 ticket);
TURBO_MEDIA_API tm_pc_status tm_pc_seal(tm_pc_portal *portal);
TURBO_MEDIA_API tm_pc_status tm_pc_snapshot(
    tm_pc_portal *portal,tm_pc_snapshot_v1 *out);
/* Requires seal, all credits settled, every producer/callback joined and
 * provider DSO generation leases independently quiesced. A drained snapshot
 * alone never authorizes concurrent destruction. */
TURBO_MEDIA_API tm_pc_status tm_pc_destroy(tm_pc_portal *portal);

#ifdef __cplusplus
}
#endif
#endif
