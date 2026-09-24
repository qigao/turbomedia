#ifndef TURBO_MEDIA_TENANT_QUOTA_WIRE_H
#define TURBO_MEDIA_TENANT_QUOTA_WIRE_H

#include "turbo_media_tenant_quota.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MEDIA_TENANT_QUOTA_WIRE_SCHEMA_VERSION 1U
#define TURBO_MEDIA_TENANT_QUOTA_WIRE_MAX_BODY_BYTES (256U * 1024U)

typedef struct turbo_media_tenant_quota_wire_snapshot_s
    turbo_media_tenant_quota_wire_snapshot_t;

typedef struct turbo_media_tenant_quota_wire_update_s {
    uint64_t epoch;
    uint64_t sequence;
    char node_id[TURBO_MEDIA_TENANT_QUOTA_NODE_ID_BYTES];
    char tenant_id[TURBO_MEDIA_TENANT_QUOTA_TENANT_ID_BYTES];
    turbo_media_tenant_quota_lease_t lease;
} turbo_media_tenant_quota_wire_update_t;

turbo_media_tenant_quota_wire_snapshot_t *
turbo_media_tenant_quota_wire_parse_snapshot(
    const char *body, size_t body_size);

void turbo_media_tenant_quota_wire_snapshot_destroy(
    turbo_media_tenant_quota_wire_snapshot_t *snapshot);

uint64_t turbo_media_tenant_quota_wire_snapshot_epoch(
    const turbo_media_tenant_quota_wire_snapshot_t *snapshot);

uint64_t turbo_media_tenant_quota_wire_snapshot_sequence(
    const turbo_media_tenant_quota_wire_snapshot_t *snapshot);

const char *turbo_media_tenant_quota_wire_snapshot_node_id(
    const turbo_media_tenant_quota_wire_snapshot_t *snapshot);

size_t turbo_media_tenant_quota_wire_snapshot_count(
    const turbo_media_tenant_quota_wire_snapshot_t *snapshot);

const turbo_media_tenant_quota_lease_t *
turbo_media_tenant_quota_wire_snapshot_leases(
    const turbo_media_tenant_quota_wire_snapshot_t *snapshot);

int turbo_media_tenant_quota_wire_parse_update(
    const char *body, size_t body_size,
    turbo_media_tenant_quota_wire_update_t *out_update);

#ifdef __cplusplus
}
#endif

#endif
