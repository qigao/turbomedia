#ifndef TURBO_STREAMGRAPH_FFMPEG_H
#define TURBO_STREAMGRAPH_FFMPEG_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_streamgraph_executor.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Desktop-only FFmpeg Source adapter. No libav* types enter StreamGraph Core
 * or this public C11 ABI. It is driven exclusively on the Media Owner lane;
 * no second scheduler or automatic packet retry is created. */
#define TM_FF_ABI_V1 1u
#define TM_FF_MAX_PACKET_BYTES (2u * 1024u * 1024u)
typedef enum tm_ff_status {
    TM_FF_OK=0, TM_FF_EOF=1, TM_FF_EINVAL=-1, TM_FF_EVERSION=-2,
    TM_FF_ENOMEM=-3, TM_FF_EFULL=-4, TM_FF_ESTATE=-5,
    TM_FF_ECODEC=-6, TM_FF_EUNSUPPORTED=-7, TM_FF_ESTALE=-8
} tm_ff_status;

typedef struct tm_ff_config_v1 {
    uint32_t size, version;
    uint32_t width, height;
    uint32_t max_packet_bytes, reserved;
} tm_ff_config_v1;
#define TM_FF_CONFIG_V1_INIT {sizeof(tm_ff_config_v1),TM_FF_ABI_V1,0u,0u,0u,0u}

typedef struct tm_ff_stats_v1 {
    uint32_t size, version;
    uint64_t packets_submitted, frames_decoded, bytes_decoded;
    uint32_t pending_packet, draining, cancelled, eof;
    int32_t ffmpeg_error;
} tm_ff_stats_v1;
#define TM_FF_STATS_V1_INIT {sizeof(tm_ff_stats_v1),TM_FF_ABI_V1,0u,0u,0u,0u,0u,0u,0u,0}

/* First-party call-site observability: the in_call flag spans the actual
 * synchronous FFmpeg invocation, not a pre-call admission barrier.
 * This does not independently prove observation while inside FFmpeg. */
typedef struct tm_ff_codec_activity_v1 {
    uint32_t size,version,in_call,reserved;
    uint64_t packet_calls,flush_calls;
} tm_ff_codec_activity_v1;
#define TM_FF_CODEC_ACTIVITY_V1_INIT {sizeof(tm_ff_codec_activity_v1),TM_FF_ABI_V1,0u,0u,0u,0u}

typedef struct tm_ff_source tm_ff_source;
TURBO_MEDIA_API tm_ff_status tm_ff_codec_activity(
    tm_ff_source *source,tm_ff_codec_activity_v1 *out);

/* First qualified profile: MJPEG compressed video -> planar 8-bit luma
 * (width*height bytes); native FFmpeg decoder is used, not a custom decoder.
 * Dimension/profile negotiation is strict: mismatches fail, not convert.
 * All APIs and the returned tm_ex_source_next_fn are owner-serialized. */
TURBO_MEDIA_API tm_ff_status tm_ff_create(const tm_ff_config_v1 *cfg,
                                          tm_ff_source **out);
/* Own an independent bounded COPY of one compressed MJPEG packet until next.
 * Full packet slot rejects before consuming input. Successful submit never
 * silently overwrites a pending packet; caller explicitly handles FULL. */
TURBO_MEDIA_API tm_ff_status tm_ff_submit_mjpeg(tm_ff_source *source,
                                               const void *encoded,
                                               size_t encoded_bytes);
/* Explicit compressed-source EOF; decoder flush is driven by source_next
 * when the Graph Executor has capacity. No automatic settlement retry. */
TURBO_MEDIA_API tm_ff_status tm_ff_finish(tm_ff_source *source);
/* Reject subsequent input and release pending compressed packet without
 * touching in-flight FrameStore credits. Graph Owner must stop/drain separately. */
TURBO_MEDIA_API tm_ff_status tm_ff_cancel(tm_ff_source *source);
/* True for a committed packet, EOF flush or cancellation. Call the Executor
 * only if the source is ready; empty/nonfinished input is not implicit EOS. */
TURBO_MEDIA_API tm_ff_status tm_ff_ready(tm_ff_source *source,int *out_ready);
TURBO_MEDIA_API tm_ex_source_result tm_ff_next_cpu_luma(
    void *context,uint8_t *dst,size_t capacity,size_t *size_bytes);
TURBO_MEDIA_API tm_ff_status tm_ff_stats(tm_ff_source *source,tm_ff_stats_v1 *out);
/* Exclusive lifecycle call, after Graph Executor has stopped and the last
 * borrowed source callback has returned; no concurrent calls permitted. */
TURBO_MEDIA_API tm_ff_status tm_ff_destroy(tm_ff_source *source);

#ifdef __cplusplus
}
#endif
#endif
