#ifndef TURBO_STREAMGRAPH_SCREEN_PACKET_H
#define TURBO_STREAMGRAPH_SCREEN_PACKET_H
#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Versioned in-process packet ingress; not an Android/PC wire protocol,
 * arbitrary H264 decoder, general Graph executor or authority credential. */
#define TM_SI_ABI_V1 1u
#define TM_SI_MAX_PACKET_BYTES (2u * 1024u * 1024u)
typedef enum tm_si_status {
    TM_SI_OK=0, TM_SI_EMPTY=1, TM_SI_EINVAL=-1, TM_SI_EVERSION=-2,
    TM_SI_EFORMAT=-3, TM_SI_ESTALE=-4, TM_SI_EFULL=-5,
    TM_SI_EREORDER=-6, TM_SI_ESHUTDOWN=-7, TM_SI_EBUSY=-8,
    TM_SI_ENOMEM=-9
} tm_si_status;

/* Every display/capture/stream incarnation has its OWN ingress owner. */
typedef struct tm_si_stream_v1 {
    uint32_t size, version;
    uint32_t display_id, width, height;
    uint64_t device_session_id, session_generation, display_generation;
    uint64_t geometry_generation, capture_hi, capture_lo, stream_generation;
} tm_si_stream_v1;
#define TM_SI_STREAM_V1_INIT {sizeof(tm_si_stream_v1),TM_SI_ABI_V1,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u}

typedef struct tm_si_config_v1 {
    uint32_t size, version;
    tm_si_stream_v1 stream;
    uint32_t max_packet_bytes, reserved;
} tm_si_config_v1;
#define TM_SI_CONFIG_V1_INIT {sizeof(tm_si_config_v1),TM_SI_ABI_V1,TM_SI_STREAM_V1_INIT,0u,0u}

/* Strict Annex-B H264 only; exactly one kind per packet.
 * CONFIG: SPS/PPS NALs (may be split before first IDR);
 * IDR: first decode-ready access unit only after SPS and PPS settled;
 * DELTA: subsequent non-IDR access units; EOS: zero-byte terminal.
 * Timestamps are 1/1,000,000 s; PTS must equal DTS, no B-frame reordering. */
typedef enum tm_si_packet_kind {
    TM_SI_CONFIG=1, TM_SI_IDR=2, TM_SI_DELTA=3, TM_SI_EOS=4
} tm_si_packet_kind;
typedef struct tm_si_packet_v1 {
    uint32_t size, version;
    tm_si_stream_v1 stream;
    uint32_t kind, reserved;
    uint64_t sequence;
    int64_t pts_us, dts_us;
    const uint8_t *bytes; /* caller-borrowed during offer only */
    size_t byte_count;
} tm_si_packet_v1;
#define TM_SI_PACKET_V1_INIT {sizeof(tm_si_packet_v1),TM_SI_ABI_V1,TM_SI_STREAM_V1_INIT,0u,0u,0u,0,0,NULL,0u}

typedef struct tm_si_owner tm_si_owner;
/* All calls are serialized by the EXISTING Media Owner. One preallocated
 * bounded buffer; offer performs one copy, no network or Codec side effects.
 * Full/invalid/stale/reordered packets are never consumed. */
TURBO_MEDIA_API tm_si_status tm_si_create(const tm_si_config_v1 *config,
                                          tm_si_owner **out);
TURBO_MEDIA_API tm_si_status tm_si_offer(tm_si_owner *owner,
                                         const tm_si_packet_v1 *packet);
/* take borrows immutable bytes until settle; no asynchronous retention of
 * this pointer. A TAKEN row prevents destroy and may not be overwritten. */
TURBO_MEDIA_API tm_si_status tm_si_take(tm_si_owner *owner,
                                        tm_si_packet_v1 *out);
TURBO_MEDIA_API tm_si_status tm_si_settle(tm_si_owner *owner);
TURBO_MEDIA_API tm_si_status tm_si_seal(tm_si_owner *owner);
TURBO_MEDIA_API tm_si_status tm_si_cancel(tm_si_owner *owner);
TURBO_MEDIA_API tm_si_status tm_si_destroy(tm_si_owner *owner);
#ifdef __cplusplus
}
#endif
#endif
