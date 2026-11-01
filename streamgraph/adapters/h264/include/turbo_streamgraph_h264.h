#ifndef TURBO_STREAMGRAPH_H264_H
#define TURBO_STREAMGRAPH_H264_H
#include <stddef.h>
#include <stdint.h>
#include <turbo_streamgraph_screen_packet.h>
#include <turbo_streamgraph_executor.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Optional production Codec Provider on ONE existing Media Owner lane.
 * Reuses TurboMedia::Codec ("h264") + StreamGraph ingress/FrameStore.
 * No new decoder, executor, poller, transport or worker thread is allocated. */
#define TM_SH_ABI_V1 1u
typedef enum tm_sh_status {
    TM_SH_OK=0, TM_SH_EMPTY=1, TM_SH_AGAIN=2, TM_SH_EOS=3,
    TM_SH_EINVAL=-1, TM_SH_EVERSION=-2, TM_SH_ENOMEM=-3,
    TM_SH_ECODEC=-4, TM_SH_EFULL=-5, TM_SH_ESTATE=-6,
    TM_SH_EINGRESS=-7
} tm_sh_status;
typedef struct tm_sh_config_v1 {
    uint32_t size,version,reserved, reserved2;
    tm_si_config_v1 ingress;
} tm_sh_config_v1;
#define TM_SH_CONFIG_V1_INIT {sizeof(tm_sh_config_v1),TM_SH_ABI_V1,0u,0u,TM_SI_CONFIG_V1_INIT}

/* Only the last FRAME emitted by this CPU Source; not a Pixel Lease or
 * persistent FrameRef. Copy this alongside tm_fs_frame_ref_v1 inside an
 * owner-serialized graph visitor. Source session and display IDs are
 * nonsecret provenance values, never authorization credentials. */
typedef struct tm_sh_stamp_v1 {
    uint32_t size,version,width,height;
    tm_si_stream_v1 stream;
    uint64_t packet_sequence;
    int64_t pts_us,dts_us;
    size_t decoded_bytes;
} tm_sh_stamp_v1;
#define TM_SH_STAMP_V1_INIT {sizeof(tm_sh_stamp_v1),TM_SH_ABI_V1,0u,0u,TM_SI_STREAM_V1_INIT,0u,0,0,0u}

typedef struct tm_sh_decoder tm_sh_decoder;
/* Requires an explicitly initialized TurboMedia Codec registry with "h264"
 * available. Checks CPU I420 frame size <= FrameStore 8 MiB bound. */
TURBO_MEDIA_API tm_sh_status tm_sh_create(const tm_sh_config_v1 *cfg,
                                          tm_sh_decoder **out);
/* The sole ingress is the existing bounded tm_si owner internally.
 * Output pending -> FULL; never silently discard or decode twice. */
TURBO_MEDIA_API tm_sh_status tm_sh_offer(tm_sh_decoder *owner,
                                         const tm_si_packet_v1 *packet);
/* Advance at most ONE pending compressed packet through the real
 * TurboMedia::Codec decoder. CONFIG may return AGAIN, a decoded IDR/DELTA
 * yields OK with one owned I420 output, EOS yields EOS. Missing delayed
 * output for a frame is ERROR (no unsupported B-frame inference). */
TURBO_MEDIA_API tm_sh_status tm_sh_pump(tm_sh_decoder *owner);
/* Source ready only when a complete I420 frame is staged, or explicit EOS.
 * Must be checked by the external Media Owner before tm_ex_pump_source,
 * except when the Executor itself retains an already-produced pending frame. */
TURBO_MEDIA_API tm_sh_status tm_sh_ready(tm_sh_decoder *owner,int *ready);
/* Exact tm_ex_source_next_fn. Copies one decoded I420 frame into the existing
 * Graph Executor scratch; never call before ready. Does not retain dst. */
TURBO_MEDIA_API tm_ex_source_result tm_sh_next_cpu_i420(
    void *owner,uint8_t *dst,size_t capacity,size_t *size_bytes);
TURBO_MEDIA_API tm_sh_status tm_sh_last_stamp(const tm_sh_decoder *owner,
                                               tm_sh_stamp_v1 *out);
/* Terminal cancellation stops THIS Source; Graph Executor/FrameStore pins,
 * Provider callbacks and optional DSO generations require separate drain. */
TURBO_MEDIA_API tm_sh_status tm_sh_cancel(tm_sh_decoder *owner);
TURBO_MEDIA_API tm_sh_status tm_sh_destroy(tm_sh_decoder *owner);

#ifdef __cplusplus
}
#endif
#endif
