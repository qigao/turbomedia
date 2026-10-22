#ifndef TURBO_STREAMGRAPH_FFMPEG_DELAYED_H
#define TURBO_STREAMGRAPH_FFMPEG_DELAYED_H

#include <turbo_streamgraph_ffmpeg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* M15c: opt-in desktop delayed-frame MPEG-2 profile, no FFmpeg types in
 * this C11 boundary. The Host/Media Owner serializes ALL calls on one
 * decoder, including cancel and destroy: these are not concurrent Codec
 * interruption methods. No thread, scheduler, socket or automatic retry. */
#define TM_FFD_ABI_V1 1u
#define TM_FFD_MAX_QUEUE_PACKETS 8u
#define TM_FFD_MAX_QUEUED_BYTES (8u * 1024u * 1024u)
typedef struct tm_ffd_decoder tm_ffd_decoder;

typedef struct tm_ffd_config_v1 {
    uint32_t size, version;
    uint32_t width, height;
    uint32_t queue_capacity, max_packet_bytes;
    uint32_t reserved;
} tm_ffd_config_v1;
#define TM_FFD_CONFIG_V1_INIT {sizeof(tm_ffd_config_v1),TM_FFD_ABI_V1,0u,0u,0u,0u,0u}

/* Exact MPEG-2 compressed packet; a successful admission makes a private
 * bounded COPY. Caller owns data only until submit returns. PTS/DTS are
 * integer ticks of this explicitly qualified 1/30 timebase. */
typedef struct tm_ffd_packet_v1 {
    uint32_t size, version;
    const uint8_t *data;
    size_t bytes;
    int64_t pts, dts;
} tm_ffd_packet_v1;
#define TM_FFD_PACKET_V1_INIT {sizeof(tm_ffd_packet_v1),TM_FFD_ABI_V1,NULL,0u,0,0}

typedef struct tm_ffd_stats_v1 {
    uint32_t size, version;
    uint64_t packets_admitted, packets_sent, frames_decoded;
    uint64_t frames_after_flush, flush_sends, receive_eagain;
    uint32_t queued, finishing, eof, cancelled;
    int32_t codec_error;
} tm_ffd_stats_v1;
#define TM_FFD_STATS_V1_INIT {sizeof(tm_ffd_stats_v1),TM_FFD_ABI_V1,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0}

/* Only MPEG-2 video -> CPU 8-bit luma is qualified in this version.
 * Queue storage is preallocated and bounded by
 * queue_capacity * max_packet_bytes <= TM_FFD_MAX_QUEUED_BYTES.
 * FFmpeg internal reference pictures remain owned by the Codec. */
TURBO_MEDIA_API tm_ff_status tm_ffd_create(
    const tm_ffd_config_v1 *cfg,tm_ffd_decoder **out);
TURBO_MEDIA_API tm_ff_status tm_ffd_submit(
    tm_ffd_decoder *decoder,const tm_ffd_packet_v1 *packet);

/* Once-only EOF admission; all accepted compressed packets are processed
 * before avcodec_send_packet(NULL). No implicit drain at empty input. */
TURBO_MEDIA_API tm_ff_status tm_ffd_finish(tm_ffd_decoder *decoder);

/* Returns:
 * TM_FF_OK: one real decoded frame copied to dst, exact *pts.
 * TM_FF_EAGAIN: no decoded frame yet; caller must explicitly admit input.
 * TM_FF_EOF: Codec terminal drain completed; all future reads return EOF.
 * TM_FF_ESTATE: cancelled or invalid lifecycle.
 * Other errors: no frame transferred.
 * No unbounded internal loop and no fabricated terminal frame. The caller
 * may drive this from its existing Source binding / executor owner. */
TURBO_MEDIA_API tm_ff_status tm_ffd_next_luma(
    tm_ffd_decoder *decoder,uint8_t *dst,size_t capacity,
    size_t *out_bytes,int64_t *out_pts);

/* Exclusive after the Host has quiesced calls. Cancel discards all queued
 * compressed input, prevents future frame publication and NEVER invokes
 * libavcodec_flush_buffers or avcodec_send_packet(NULL) behind the Owner. */
TURBO_MEDIA_API tm_ff_status tm_ffd_cancel(tm_ffd_decoder *decoder);
TURBO_MEDIA_API tm_ff_status tm_ffd_stats(
    tm_ffd_decoder *decoder,tm_ffd_stats_v1 *out);
TURBO_MEDIA_API tm_ff_status tm_ffd_destroy(tm_ffd_decoder *decoder);

#ifdef __cplusplus
}
#endif
#endif
