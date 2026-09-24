#ifndef TURBO_MEDIA_SIGNALING_TENANT_QUOTA_INTERNAL_H
#define TURBO_MEDIA_SIGNALING_TENANT_QUOTA_INTERNAL_H

#include "turbo_media_tenant_quota.h"
#include "webrtc_signaling.h"
#include <turbo_export.h>

#ifdef __cplusplus
extern "C" {
#endif

TURBO_MEDIA_API int signaling_tenant_quota_enable(
    webrtc_signaling_server_t *server,
    const char *node_id,
    size_t max_tenants);

TURBO_MEDIA_API int signaling_tenant_quota_enabled(
    webrtc_signaling_server_t *server);

TURBO_MEDIA_API turbo_media_tenant_quota_apply_result_t
signaling_tenant_quota_apply_snapshot(
    webrtc_signaling_server_t *server,
    const char *node_id,
    uint64_t epoch,
    uint64_t sequence,
    const turbo_media_tenant_quota_lease_t *leases,
    size_t lease_count);

TURBO_MEDIA_API turbo_media_tenant_quota_apply_result_t
signaling_tenant_quota_apply_update(
    webrtc_signaling_server_t *server,
    const char *node_id,
    uint64_t epoch,
    uint64_t sequence,
    const turbo_media_tenant_quota_lease_t *lease);

TURBO_MEDIA_API int signaling_tenant_quota_status(
    webrtc_signaling_server_t *server,
    int *out_synchronized,
    uint64_t *out_epoch,
    uint64_t *out_sequence,
    size_t *out_lease_count);

TURBO_MEDIA_API turbo_media_tenant_quota_reserve_result_t
signaling_tenant_quota_reserve(
    webrtc_signaling_server_t *server,
    const char *tenant_id,
    turbo_media_tenant_quota_resource_t resource,
    uint32_t amount,
    uint64_t now_unix_ms);

TURBO_MEDIA_API int signaling_tenant_quota_release(
    webrtc_signaling_server_t *server,
    const char *tenant_id,
    turbo_media_tenant_quota_resource_t resource,
    uint32_t amount);

#ifdef __cplusplus
}
#endif

#endif
