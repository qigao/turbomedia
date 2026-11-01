#ifndef TURBO_STREAMGRAPH_H264_H
#define TURBO_STREAMGRAPH_H264_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_streamgraph_executor.h>
#include <turbo_codec.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Additive TurboMedia::StreamGraphH264 provider. This is the existing
 * TurboMedia::Codec/OpenH264 implementation, bound to the SAME Media Owner
 * as FrameStore/Executor. No decoder clone, threads or alternate scheduler.
 * The Source outputs exact-width I420 CPU frames only; not an arbitrary
 * EncodedPacket→Observation DAG execution engine.
 */
#define TM_SGH264_ABI_V1 1u
#define TM_SGH264_MAX_PACKET_BYTES (2u * 1024u * 1024u)

typedef enum tm_sgh264_status {
    TM_SGH264_OK=0, TM_SGH264_FRAME_READY=1, TM_SGH264_NEED_MORE=2,
    TM_SGH264_EINVAL=-1, TM_SGH264_EVERSION=-2, TM_SGH264_EFULL=-3,
    TM_SGH264_ESTATE=-4, TM_SGH264_ECODEC=-5, TM_SGH264_EPROTOCOL=-6,
    TM_SGH264_ENOMEM=-7, TM_SGH264_EUNSUPPORTED=-8
} tm_sgh264_status;

#define TM_SGH264_FRAMING_ANNEX_B 1u
#define TM_SGH264_PACKET_CONFIG 1u
#define TM_SGH264_PACKET_KEY_FRAME 2u
#define TM_SGH264_PACKET_EOS 4u

typedef struct tm_sgh264_config_v1 {
    uint32_t size, version, width, height;
    uint32_t max_packet_bytes, decoded_format_id;
    uint64_t device_session_id, session_generation;
    uint32_t display_id, reserved;
    uint64_t source_session_generation, display_generation, geometry_generation;
    uint64_t capture_session_hi, capture_session_lo;
    uint64_t stream_generation;
} tm_sgh264_config_v1;
#define TM_SGH264_CONFIG_V1_INIT {sizeof(tm_sgh264_config_v1),TM_SGH264_ABI_V1,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u}

/* The packet has only a *borrowed* byte pointer during submit; it is NOT a
 * cross-process IPC/wire struct or a raw Android MediaCodec buffer lease.
 * Metadata is a full versioned copy of the actual capture provenance.
 * A copy of a FrameRef, packet or metadata never grants a FramePin/Lease.
 * This v1 accepts only access units framed with Annex-B start codes and
 * strictly monotonic sequence/PTS=DTS (no B-frame reorder).
 *
 * EOS is an empty packet with EOS flag, never an empty image frame.
 * Codec config SPS/PPS can be a separate CONFIG packet and yields NEED_MORE.
 */
typedef struct tm_sgh264_packet_v1 {
    uint32_t size, version;
    uint32_t framing, flags;
    const uint8_t *bytes;
    size_t bytes_length;
    tm_fs_frame_metadata_v1 metadata;
} tm_sgh264_packet_v1;
#define TM_SGH264_PACKET_V1_INIT {sizeof(tm_sgh264_packet_v1),TM_SGH264_ABI_V1,0u,0u,NULL,0u,TM_FS_FRAME_METADATA_V1_INIT}

typedef struct tm_sgh264_source tm_sgh264_source;

/* Codec Registry must already be initialized; missing registered "h264"
 * is an explicit UNSUPPORTED result, not an automatic FFmpeg fallback.
 * One decoder/source instance per capture and geometry generation.
 * Source and all decoder methods are owner-serialized. */
TURBO_MEDIA_API tm_sgh264_status tm_sgh264_create(
    const tm_sgh264_config_v1 *config, tm_sgh264_source **out_source);

/* Synchronously decodes at most one bounded Annex-B access unit through the
 * EXISTING TurboMedia::Codec H264 decoder. No decode on another callback
 * thread; no second submitted packet while output is pending.
 * NEED_MORE is not EOS. The owner calls tm_ex_pump_source ONLY when ready. */
TURBO_MEDIA_API tm_sgh264_status tm_sgh264_submit(
    tm_sgh264_source *source, const tm_sgh264_packet_v1 *packet);

TURBO_MEDIA_API tm_sgh264_status tm_sgh264_ready(
    tm_sgh264_source *source, int *out_ready);

/* Existing tm_ex_source_next_fn and tm_ex_source_metadata_fn bindings.
 * The Executor publishes the I420 copy + metadata atomically in FrameStore,
 * retains pending bytes/metadata on saturation and fans out under its owner.
 */
TURBO_MEDIA_API tm_ex_source_result tm_sgh264_next_cpu_i420(
    void *source, uint8_t *dst, size_t capacity, size_t *size_bytes);
TURBO_MEDIA_API int tm_sgh264_source_metadata(
    void *source, tm_fs_frame_metadata_v1 *out_metadata);

/* Cancels admission and discards only the adapter's unclaimed decoded frame.
 * Does NOT revoke/retire existing FrameStore pins or claim codec wake
 * quiescence. The host drains the canonical Executor before destroying.
 */
TURBO_MEDIA_API tm_sgh264_status tm_sgh264_cancel(tm_sgh264_source *source);
TURBO_MEDIA_API tm_sgh264_status tm_sgh264_destroy(tm_sgh264_source *source);

#ifdef __cplusplus
}
#endif
#endif
