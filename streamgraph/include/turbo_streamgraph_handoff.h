#ifndef TURBO_STREAMGRAPH_HANDOFF_H
#define TURBO_STREAMGRAPH_HANDOFF_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>
#include <turbo_frame_store.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A bounded MPSC command inbox. It does not call FrameStore or the Executor
 * from producer threads. The Media Owner alone executes accepted commands.
 * No internal thread, poller, heap growth after create, or hidden retries. */
#define TM_HO_ABI_V1 1u
#define TM_HO_MAX_CAPACITY 1024u

typedef enum tm_ho_status {
    TM_HO_OK = 0,
    TM_HO_EINVAL = -1,
    TM_HO_EVERSION = -2,
    TM_HO_ENOMEM = -3,
    TM_HO_EFULL = -4,
    TM_HO_ESTALE = -5,
    TM_HO_EBUSY = -6,
    TM_HO_ESHUTDOWN = -7,
    TM_HO_EEXHAUSTED = -8
} tm_ho_status;

typedef enum tm_ho_command_kind {
    TM_HO_PUMP_SOURCE = 1,
    TM_HO_PUMP_SINK = 2,
    TM_HO_STOP = 3,
    TM_HO_FRAME_NOTICE = 4
} tm_ho_command_kind;

/* Immutable, pointer-free command envelope. FRAME_NOTICE is a *weak*
 * FrameRef, not a pin, a FrameLease or permission to access pixels.
 * Owner must reserve a pin and validate generation before async usage. */
typedef struct tm_ho_command_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t kind;
    uint32_t branch;
    uint64_t correlation;
    uint32_t has_frame;
    uint32_t reserved;
    tm_fs_frame_ref_v1 frame;
} tm_ho_command_v1;
#define TM_HO_COMMAND_V1_INIT {sizeof(tm_ho_command_v1),TM_HO_ABI_V1,0u,0u,0u,0u,0u,{0}}

typedef struct tm_ho_ticket_v1 {
    uint64_t incarnation;
    uint32_t slot;
    uint32_t generation;
} tm_ho_ticket_v1;

typedef struct tm_ho_config_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t capacity;
    uint32_t reserved;
} tm_ho_config_v1;
#define TM_HO_CONFIG_V1_INIT {sizeof(tm_ho_config_v1),TM_HO_ABI_V1,0u,0u}

typedef struct tm_ho_snapshot_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t capacity;
    uint32_t reserved;
    uint32_t ready;
    uint32_t taken;
    uint32_t sealed;
    uint32_t drained;
} tm_ho_snapshot_v1;
#define TM_HO_SNAPSHOT_V1_INIT {sizeof(tm_ho_snapshot_v1),TM_HO_ABI_V1,0u,0u,0u,0u,0u,0u}

typedef struct tm_ho_inbox tm_ho_inbox;

/* create/destroy are exclusive-lifecycle operations; all other calls may
 * overlap across producer threads and the single consuming owner lane. */
TURBO_MEDIA_API tm_ho_status tm_ho_create(
    const tm_ho_config_v1 *config, tm_ho_inbox **out);
TURBO_MEDIA_API tm_ho_status tm_ho_reserve(
    tm_ho_inbox *inbox, tm_ho_ticket_v1 *out_ticket);
TURBO_MEDIA_API tm_ho_status tm_ho_publish(
    tm_ho_inbox *inbox, tm_ho_ticket_v1 ticket,
    const tm_ho_command_v1 *command);
/* RESERVED can be aborted after seal; no effect ever became visible. */
TURBO_MEDIA_API tm_ho_status tm_ho_abort(
    tm_ho_inbox *inbox, tm_ho_ticket_v1 ticket);
/* Only the final Media Owner calls take and settles TAKEN work.
 * A taken ticket is still an outstanding capacity credit until settle. */
TURBO_MEDIA_API tm_ho_status tm_ho_take(
    tm_ho_inbox *inbox, tm_ho_ticket_v1 *out_ticket,
    tm_ho_command_v1 *out_command);
TURBO_MEDIA_API tm_ho_status tm_ho_settle(
    tm_ho_inbox *inbox, tm_ho_ticket_v1 ticket);
/* Seal rejects new reserve/publish; already published READY commands drain.
 * No automatic cancellation or replay; rejected commit retains RESERVED. */
TURBO_MEDIA_API tm_ho_status tm_ho_seal(tm_ho_inbox *inbox);
TURBO_MEDIA_API tm_ho_status tm_ho_snapshot(
    tm_ho_inbox *inbox, tm_ho_snapshot_v1 *out);

/* Requires seal + drained and host-established quiescence of ALL producers
 * (including publish/wake tails) and the Media Owner. A drained snapshot
 * alone cannot prove it is safe to destroy a concurrently accessed inbox. */
TURBO_MEDIA_API tm_ho_status tm_ho_destroy(tm_ho_inbox *inbox);

#ifdef __cplusplus
}
#endif
#endif
