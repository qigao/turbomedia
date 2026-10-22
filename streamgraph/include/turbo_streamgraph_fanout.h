#ifndef TURBO_STREAMGRAPH_FANOUT_H
#define TURBO_STREAMGRAPH_FANOUT_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>
#include <turbo_frame_store.h>

#ifdef __cplusplus
extern "C" {
#endif

/* M2 owner-serialized, CPU FrameStore-backed bounded branch admission.
 * Not a thread pool, generic executor, or FFmpeg migration. */
#define TM_FO_ABI_V1 1u
#define TM_FO_MAX_BRANCHES 8u
#define TM_FO_MAX_QUEUE_DEPTH 64u

typedef enum tm_fo_status {
    TM_FO_OK = 0,
    TM_FO_EINVAL = -1,
    TM_FO_EVERSION = -2,
    TM_FO_ENOMEM = -3,
    TM_FO_EFULL = -4,
    TM_FO_EBUSY = -5,
    TM_FO_ESTALE = -6,
    TM_FO_ESHUTDOWN = -7,
    TM_FO_EFRAME = -8,
    TM_FO_EEXHAUSTED = -9
} tm_fo_status;

typedef enum tm_fo_policy {
    TM_FO_ORDERED_LOSSLESS = 1,
    TM_FO_SAMPLE = 2,
    TM_FO_LATEST = 3
} tm_fo_policy;

typedef struct tm_fo_branch_config_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t policy;
    uint32_t queue_depth;
    uint32_t sample_every;
} tm_fo_branch_config_v1;
#define TM_FO_BRANCH_CONFIG_V1_INIT {sizeof(tm_fo_branch_config_v1),TM_FO_ABI_V1,0u,0u,0u}

typedef struct tm_fo_config_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t branch_count;
    uint32_t reserved;
    tm_fo_branch_config_v1 branches[TM_FO_MAX_BRANCHES];
} tm_fo_config_v1;
#define TM_FO_CONFIG_V1_INIT {sizeof(tm_fo_config_v1),TM_FO_ABI_V1,0u,0u,{{0}}}

typedef struct tm_fo_ticket_v1 {
    uint64_t incarnation;
    uint32_t branch;
    uint32_t slot;
    uint32_t slot_generation;
} tm_fo_ticket_v1;

typedef struct tm_fo_branch_stats_v1 {
    uint32_t size;
    uint32_t version;
    uint32_t ready;
    uint32_t taken;
    uint64_t admitted;
    uint64_t sampled_out;
    uint64_t replaced_ready;
    uint64_t rejected;
    uint64_t completed;
} tm_fo_branch_stats_v1;
#define TM_FO_BRANCH_STATS_V1_INIT {sizeof(tm_fo_branch_stats_v1),TM_FO_ABI_V1,0u,0u,0u,0u,0u,0u,0u}

typedef struct tm_fo_owner tm_fo_owner;

/* One caller lane serializes all fanout and FrameStore operations.
 * FrameStore must outlive this owner. No callbacks or threads are created. */
TURBO_MEDIA_API tm_fo_status tm_fo_create(tm_fs_store *store,
                                           const tm_fo_config_v1 *config,
                                           tm_fo_owner **out);

/* One Store pin per admitted consumer. Check every lossless branch before
 * any modification. SAMPLE drops unadmitted source frame IDs; LATEST replaces
 * only READY work, never a TAKEN frame. Pixel data is never copied. */
TURBO_MEDIA_API tm_fo_status tm_fo_submit(tm_fo_owner *owner,
                                           const tm_fs_frame_ref_v1 *ref);

/* A TAKEN row pins the frame until successful settlement; a ticket is
 * identity, not a transferable reference count. */
TURBO_MEDIA_API tm_fo_status tm_fo_take(tm_fo_owner *owner, uint32_t branch,
                                         tm_fo_ticket_v1 *out_ticket,
                                         tm_fs_frame_ref_v1 *out_frame);
TURBO_MEDIA_API tm_fo_status tm_fo_acquire_lease(tm_fo_owner *owner,
                                                  tm_fo_ticket_v1 ticket,
                                                  tm_fs_lease_v1 *out_lease);
TURBO_MEDIA_API tm_fo_status tm_fo_settle(tm_fo_owner *owner,
                                           tm_fo_ticket_v1 ticket);
TURBO_MEDIA_API tm_fo_status tm_fo_discard_ready(tm_fo_owner *owner,
                                                  uint32_t branch);
TURBO_MEDIA_API tm_fo_status tm_fo_stats(tm_fo_owner *owner, uint32_t branch,
                                          tm_fo_branch_stats_v1 *out_stats);
TURBO_MEDIA_API tm_fo_status tm_fo_seal(tm_fo_owner *owner);
TURBO_MEDIA_API tm_fo_status tm_fo_destroy(tm_fo_owner *owner);

#ifdef __cplusplus
}
#endif
#endif
