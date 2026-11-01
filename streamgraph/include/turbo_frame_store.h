#ifndef TURBO_FRAME_STORE_H
#define TURBO_FRAME_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TM_FS_ABI_V1 1u
#define TM_FS_MAX_SLOTS 64u
#define TM_FS_MAX_FRAME_BYTES (8u * 1024u * 1024u)
#define TM_FS_MAX_CREDITS 1024u

typedef enum tm_fs_status {
    TM_FS_OK = 0,
    TM_FS_EINVAL = -1,
    TM_FS_EVERSION = -2,
    TM_FS_ENOMEM = -3,
    TM_FS_EFULL = -4,
    TM_FS_ESTALE = -5,
    TM_FS_EBUSY = -6,
    TM_FS_ESHUTDOWN = -7,
    TM_FS_EEXHAUSTED = -8,
    TM_FS_ENOMETA = -9
} tm_fs_status;

typedef struct tm_fs_config_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t store_id;
    uint32_t slot_capacity;
    uint32_t max_frame_bytes;
    uint32_t max_pins;
    uint32_t max_leases;
    uint64_t max_retained_bytes;
} tm_fs_config_v1;
#define TM_FS_CONFIG_V1_INIT {sizeof(tm_fs_config_v1),TM_FS_ABI_V1,0,0,0,0,0,0}

typedef struct tm_fs_frame_ref_v1 {
    uint32_t size;
    uint32_t version;
    uint64_t store_id;
    uint64_t store_generation;
    uint32_t slot;
    uint32_t slot_generation;
    uint64_t frame_id;
    uint64_t source_session_generation;
    uint64_t display_generation;
} tm_fs_frame_ref_v1;
#define TM_FS_FRAME_REF_V1_INIT {sizeof(tm_fs_frame_ref_v1),TM_FS_ABI_V1,0,0,0,0,0,0,0}

/* Value identities; copying them does not create an additional resource right. */
typedef struct tm_fs_pin_v1 {
    uint64_t store_id;
    uint64_t store_generation;
    uint64_t ticket_id;
} tm_fs_pin_v1;

typedef struct tm_fs_lease_v1 {
    uint64_t store_id;
    uint64_t store_generation;
    uint64_t ticket_id;
} tm_fs_lease_v1;

typedef struct tm_fs_cpu_view_v1 {
    uint32_t size;
    uint32_t version;
    const uint8_t *data; /* borrowed until lease release; same-process CPU only */
    size_t size_bytes;
} tm_fs_cpu_view_v1;
#define TM_FS_CPU_VIEW_V1_INIT {sizeof(tm_fs_cpu_view_v1),TM_FS_ABI_V1,NULL,0}

typedef struct tm_fs_store tm_fs_store;

/* Owner-serialized lifecycle, no MPSC, cross-process or GPU access promises. */
TURBO_MEDIA_API tm_fs_status tm_fs_create(const tm_fs_config_v1 *config, tm_fs_store **out);
TURBO_MEDIA_API tm_fs_status tm_fs_publish_cpu(tm_fs_store *store, const void *data,
                                                size_t size_bytes,
                                                uint64_t source_session_generation,
                                                uint64_t display_generation,
                                                tm_fs_frame_ref_v1 *out_ref);

/* A FrameRef retains only identity. This immutable, owner-published sidecar
 * carries the capture/geometry identity and original media timestamps.
 * The Store owns it for the same slot lifetime as the pixel bytes. Nothing
 * here is a transferable pin or an Android compositor causal fence. */
#define TM_FS_META_ABI_V1 1u
typedef struct tm_fs_frame_metadata_v1 {
    uint32_t size, version;
    uint32_t width, height;
    uint32_t format_id, flags;
    uint64_t device_session_id, session_generation;
    uint32_t display_id, reserved;
    uint64_t source_session_generation, display_generation, geometry_generation;
    uint64_t capture_session_hi, capture_session_lo;
    uint64_t stream_generation, packet_sequence;
    uint64_t pts, dts;
    uint32_t timebase_num, timebase_den;
} tm_fs_frame_metadata_v1;
#define TM_FS_FRAME_METADATA_V1_INIT {sizeof(tm_fs_frame_metadata_v1),TM_FS_META_ABI_V1,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,0u}
#define TM_FS_META_FLAG_KEY_FRAME 1u
#define TM_FS_META_FLAG_DISCONTINUITY 2u

TURBO_MEDIA_API tm_fs_status tm_fs_validate_metadata(
    const tm_fs_frame_metadata_v1 *metadata);
/* Atomic CPU-byte + metadata publication. The original publish_cpu ABI
 * remains unchanged and produces an unannotated (legacy) FrameRef. */
TURBO_MEDIA_API tm_fs_status tm_fs_publish_cpu_with_metadata(
    tm_fs_store *store, const void *data, size_t size_bytes,
    uint64_t source_session_generation, uint64_t display_generation,
    const tm_fs_frame_metadata_v1 *metadata, tm_fs_frame_ref_v1 *out_ref);
/* Fails ESTALE after retirement/slot reuse, ENOMETA for legacy frames.
 * This lookup does NOT prolong the frame lifetime. Pin/Lease separately. */
TURBO_MEDIA_API tm_fs_status tm_fs_get_metadata(
    tm_fs_store *store, const tm_fs_frame_ref_v1 *ref,
    tm_fs_frame_metadata_v1 *out_metadata);

TURBO_MEDIA_API tm_fs_status tm_fs_reserve_pin(tm_fs_store,
                                                const tm_fs_frame_ref_v1 *ref,
                                                tm_fs_pin_v1 *out_pin);
TURBO_MEDIA_API tm_fs_status tm_fs_acquire_weak(tm_fs_store *store,
                                                const tm_fs_frame_ref_v1 *ref,
                                                tm_fs_lease_v1 *out_lease);
TURBO_MEDIA_API tm_fs_status tm_fs_acquire_pinned(tm_fs_store *store,
                                                  const tm_fs_pin_v1 *pin,
                                                  tm_fs_lease_v1 *out_lease);
TURBO_MEDIA_API tm_fs_status tm_fs_map_cpu(tm_fs_store *store,
                                           const tm_fs_lease_v1 *lease,
                                           tm_fs_cpu_view_v1 *out_view);
TURBO_MEDIA_API tm_fs_status tm_fs_release_lease(tm_fs_store *store,
                                                 const tm_fs_lease_v1 *lease);
TURBO_MEDIA_API tm_fs_status tm_fs_release_pin(tm_fs_store *store,
                                               const tm_fs_pin_v1 *pin);
TURBO_MEDIA_API tm_fs_status tm_fs_retire(tm_fs_store *store,
                                          const tm_fs_frame_ref_v1 *ref);
TURBO_MEDIA_API tm_fs_status tm_fs_seal(tm_fs_store *store);
TURBO_MEDIA_API tm_fs_status tm_fs_destroy(tm_fs_store *store);

#ifdef __cplusplus
}
#endif
#endif
