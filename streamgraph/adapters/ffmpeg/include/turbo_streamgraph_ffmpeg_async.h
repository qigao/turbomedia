#ifndef TURBO_STREAMGRAPH_FFMPEG_ASYNC_H
#define TURBO_STREAMGRAPH_FFMPEG_ASYNC_H

#include <turbo_streamgraph_ffmpeg.h>
#include <turbo_streamgraph_provider_completion.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One bounded MJPEG Decode Owner -> Media Owner handoff. The Host provides
 * its already-owned Provider thread and wake mechanism; this adapter starts
 * no threads, creates no scheduler, and never touches FrameStore off-owner.
 * The exact Portal ticket is reserved by the Host BEFORE starting its worker.
 */
#define TM_FFA_ABI_V1 1u
typedef struct tm_ffa_decoder tm_ffa_decoder;
typedef int (*tm_ffa_wake_fn)(void *context);

typedef struct tm_ffa_stats_v1 {
    uint32_t size,version,state,worker_active;
    uint64_t decoded,delivered,discarded,notifications_failed;
    uint32_t cancelled,reserved;
} tm_ffa_stats_v1;
#define TM_FFA_STATS_V1_INIT {sizeof(tm_ffa_stats_v1),TM_FFA_ABI_V1,0u,0u,0u,0u,0u,0u,0u,0u}

/* The FFmpeg Codec and fixed CPU luma buffers are owned by this object.
 * Config has exactly the same size/version/profile rules as tm_ff_create.
 * Create is exclusive; the decoder may be invoked from ONE worker at a time.
 */
TURBO_MEDIA_API tm_ff_status tm_ffa_create(
    const tm_ff_config_v1 *cfg,tm_ffa_decoder **out);

/* Called on the Host's Provider worker. For a successful admission this
 * always reports a terminal via tm_pc_complete(), then calls wake if non-NULL
 * AFTER the result and its luma slot are published. Input remains borrowed
 * only until this call returns. Portal and wake context must stay alive
 * through the worker's post-completion notification tail.
 * Does not reserve Portal credits or request cancellation on its behalf.
 * Full/invalid admission returns without consuming Portal ticket.
 */
TURBO_MEDIA_API tm_ff_status tm_ffa_run_mjpeg(
    tm_ffa_decoder *decoder,const uint8_t *compressed,size_t bytes,
    tm_pc_portal *portal,tm_pc_ticket_v1 ticket,
    tm_ffa_wake_fn wake,void *wake_context);

/* Media Owner alone admits a completed packet to Source->FrameStore.
 * Must match the SAME portal ticket whose terminal was delivered. A wrong
 * generation can never steal the next frame; no raw pointer is returned.
 */
TURBO_MEDIA_API tm_ff_status tm_ffa_owner_accept(
    tm_ffa_decoder *decoder,tm_pc_ticket_v1 ticket);
/* tm_ex_source_next_fn. Copies the accepted decoded frame into the existing
 * Executor scratch exactly once. Only call from the owner after accept.
 * An empty Source is NOT EOS: calling early yields FAILED (host error).
 */
TURBO_MEDIA_API tm_ex_source_result tm_ffa_owner_next_luma(
    void *decoder,uint8_t *dst,size_t capacity,size_t *out_bytes);

/* Only the Media Owner requests cancellation, in coordination with
 * tm_pc_request_cancel(ticket); it cannot close an active Codec callback.
 * An already-ready frame is discarded, but the Provider callback tail and
 * Portal terminal credit must still finish. No DSO lease released here.
 */
TURBO_MEDIA_API tm_ff_status tm_ffa_owner_cancel(tm_ffa_decoder *decoder);
TURBO_MEDIA_API tm_ff_status tm_ffa_stats(
    tm_ffa_decoder *decoder,tm_ffa_stats_v1 *out);
/* Exclusive lifecycle, after worker join AND portal terminal settlement.
 * Returns ESTATE while any worker callback/wake is active or a frame awaits
 * Owner acceptance; never silently discards a retaining frame. */
TURBO_MEDIA_API tm_ff_status tm_ffa_destroy(tm_ffa_decoder *decoder);

#ifdef __cplusplus
}
#endif
#endif
