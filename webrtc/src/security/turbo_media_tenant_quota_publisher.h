#ifndef TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_H
#define TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_H

#include "turbo_media_tenant_quota_allocator.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_MAX_TARGETS 64U
#define TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_MAX_ATTEMPTS 8U

typedef struct turbo_media_tenant_quota_publisher_s
    turbo_media_tenant_quota_publisher_t;

typedef enum turbo_media_tenant_quota_publish_transport_result_e {
    TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_APPLIED = 0,
    TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_STALE = 1,
    TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_GAP = 2,
    TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_LIMIT = 3,
    TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_ERROR = 4,
    TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_RETRYABLE = 5,
    TURBO_MEDIA_TENANT_QUOTA_PUBLISH_TRANSPORT_FATAL = 6
} turbo_media_tenant_quota_publish_transport_result_t;

typedef struct turbo_media_tenant_quota_publish_response_s {
    int synchronized;
    uint64_t epoch;
    uint64_t sequence;
    size_t lease_count;
} turbo_media_tenant_quota_publish_response_t;

typedef turbo_media_tenant_quota_publish_transport_result_t
(*turbo_media_tenant_quota_publish_snapshot_fn)(
    void *transport_context,
    void *target_context,
    const char *target_id,
    uint64_t epoch,
    uint64_t sequence,
    const turbo_media_tenant_quota_lease_t *leases,
    size_t lease_count,
    unsigned int attempt,
    turbo_media_tenant_quota_publish_response_t *response);

typedef struct turbo_media_tenant_quota_publisher_target_s {
    const char *target_id;
    void *target_context;
} turbo_media_tenant_quota_publisher_target_t;

typedef struct turbo_media_tenant_quota_publisher_config_s {
    unsigned int max_attempts;
    turbo_media_tenant_quota_publish_snapshot_fn send_snapshot;
    void *transport_context;
} turbo_media_tenant_quota_publisher_config_t;

typedef enum turbo_media_tenant_quota_publisher_target_state_e {
    TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_TARGET_UNKNOWN = 0,
    TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_TARGET_SYNCHRONIZED = 1,
    TURBO_MEDIA_TENANT_QUOTA_PUBLISHER_TARGET_FAILED = 2
} turbo_media_tenant_quota_publisher_target_state_t;

typedef struct turbo_media_tenant_quota_publisher_target_status_s {
    char target_id[TURBO_MEDIA_TENANT_QUOTA_NODE_ID_BYTES];
    turbo_media_tenant_quota_publisher_target_state_t state;
    turbo_media_tenant_quota_publish_transport_result_t
        last_transport_result;
    uint64_t epoch;
    uint64_t sequence;
    size_t lease_count;
    unsigned int attempts;
} turbo_media_tenant_quota_publisher_target_status_t;

typedef struct turbo_media_tenant_quota_publisher_report_s {
    uint64_t epoch;
    uint64_t sequence;
    size_t target_count;
    size_t synchronized_count;
    size_t failed_count;
} turbo_media_tenant_quota_publisher_report_t;

/**
 * Create a bounded caller-serialized publisher.
 *
 * The publisher owns target identifiers and acknowledgement state only. It
 * never owns quota policy, lease payloads, bearer tokens, or credentials.
 */
turbo_media_tenant_quota_publisher_t *
turbo_media_tenant_quota_publisher_create(
    const turbo_media_tenant_quota_publisher_config_t *config,
    const turbo_media_tenant_quota_publisher_target_t *targets,
    size_t target_count);

void turbo_media_tenant_quota_publisher_destroy(
    turbo_media_tenant_quota_publisher_t *publisher);

/**
 * Publish the allocator's current canonical version to every target.
 *
 * Each target receives its complete node-specific covering snapshot. The
 * allocator is the sole canonical version source. A successful target ack
 * must report exactly the requested epoch/sequence and lease count; a target
 * reporting a newer version is treated as conflicting writer evidence.
 *
 * Returns 0 when all targets acknowledge the exact version, 1 on partial
 * delivery, and -1 for invalid/UNSYNCHRONIZED allocator input.
 */
int turbo_media_tenant_quota_publisher_publish(
    turbo_media_tenant_quota_publisher_t *publisher,
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t now_unix_ms,
    turbo_media_tenant_quota_publisher_report_t *report);

size_t turbo_media_tenant_quota_publisher_target_count(
    const turbo_media_tenant_quota_publisher_t *publisher);

int turbo_media_tenant_quota_publisher_get_target_status(
    const turbo_media_tenant_quota_publisher_t *publisher,
    size_t index,
    turbo_media_tenant_quota_publisher_target_status_t *status);

#ifdef __cplusplus
}
#endif

#endif
