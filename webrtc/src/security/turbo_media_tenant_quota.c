#include "turbo_media_tenant_quota.h"

#include <stdlib.h>
#include <string.h>

typedef struct turbo_media_tenant_quota_slot_s {
    char tenant_id[TURBO_MEDIA_TENANT_QUOTA_TENANT_ID_BYTES];
    int occupied;
    int has_lease;
    uint64_t expires_at_unix_ms;
    uint32_t limits[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
    uint32_t used[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
} turbo_media_tenant_quota_slot_t;

struct turbo_media_tenant_quota_projection_s {
    char node_id[TURBO_MEDIA_TENANT_QUOTA_NODE_ID_BYTES];
    size_t max_tenants;
    turbo_media_tenant_quota_slot_t *slots;
    int synchronized;
    uint64_t epoch;
    uint64_t sequence;
    uint64_t required_epoch;
    uint64_t required_sequence;
};

static int quota_identifier_valid(const char *value, size_t capacity) {
    size_t length;
    size_t index;

    if (!value || !value[0]) {
        return 0;
    }
    length = strlen(value);
    if (length >= capacity) {
        return 0;
    }
    for (index = 0U; index < length; ++index) {
        unsigned char ch = (unsigned char)value[index];
        if (!((ch >= (unsigned char)'a' && ch <= (unsigned char)'z') ||
              (ch >= (unsigned char)'A' && ch <= (unsigned char)'Z') ||
              (ch >= (unsigned char)'0' && ch <= (unsigned char)'9') ||
              ch == (unsigned char)'-' || ch == (unsigned char)'_' ||
              ch == (unsigned char)'.' || ch == (unsigned char)'~' ||
              ch == (unsigned char)':')) {
            return 0;
        }
    }
    return 1;
}

static int quota_tenant_valid(const char *tenant_id) {
    return quota_identifier_valid(
        tenant_id, TURBO_MEDIA_TENANT_QUOTA_TENANT_ID_BYTES);
}

static int quota_node_valid(const char *node_id) {
    return quota_identifier_valid(
        node_id, TURBO_MEDIA_TENANT_QUOTA_NODE_ID_BYTES);
}

static int quota_version_compare(
    uint64_t left_epoch, uint64_t left_sequence,
    uint64_t right_epoch, uint64_t right_sequence) {
    if (left_epoch < right_epoch) {
        return -1;
    }
    if (left_epoch > right_epoch) {
        return 1;
    }
    if (left_sequence < right_sequence) {
        return -1;
    }
    if (left_sequence > right_sequence) {
        return 1;
    }
    return 0;
}

static void quota_require_snapshot(
    turbo_media_tenant_quota_projection_t *projection,
    uint64_t epoch, uint64_t sequence) {
    if (!projection) {
        return;
    }
    projection->synchronized = 0;
    if (quota_version_compare(
            epoch, sequence,
            projection->required_epoch,
            projection->required_sequence) > 0) {
        projection->required_epoch = epoch;
        projection->required_sequence = sequence;
    }
}

static int quota_usage_present(
    const turbo_media_tenant_quota_slot_t *slot) {
    size_t index;

    if (!slot || !slot->occupied) {
        return 0;
    }
    for (index = 0U;
         index < TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT; ++index) {
        if (slot->used[index] != 0U) {
            return 1;
        }
    }
    return 0;
}

static turbo_media_tenant_quota_slot_t *quota_find_slot(
    turbo_media_tenant_quota_projection_t *projection,
    const char *tenant_id) {
    size_t index;

    if (!projection || !tenant_id) {
        return NULL;
    }
    for (index = 0U; index < projection->max_tenants; ++index) {
        turbo_media_tenant_quota_slot_t *slot = &projection->slots[index];
        if (slot->occupied &&
            strcmp(slot->tenant_id, tenant_id) == 0) {
            return slot;
        }
    }
    return NULL;
}

static const turbo_media_tenant_quota_slot_t *quota_find_slot_const(
    const turbo_media_tenant_quota_projection_t *projection,
    const char *tenant_id) {
    size_t index;

    if (!projection || !tenant_id) {
        return NULL;
    }
    for (index = 0U; index < projection->max_tenants; ++index) {
        const turbo_media_tenant_quota_slot_t *slot =
            &projection->slots[index];
        if (slot->occupied &&
            strcmp(slot->tenant_id, tenant_id) == 0) {
            return slot;
        }
    }
    return NULL;
}

static int quota_lease_valid(
    const turbo_media_tenant_quota_projection_t *projection,
    const turbo_media_tenant_quota_lease_t *lease) {
    return projection && lease &&
           quota_tenant_valid(lease->tenant_id) &&
           quota_node_valid(lease->node_id) &&
           strcmp(lease->node_id, projection->node_id) == 0 &&
           lease->expires_at_unix_ms != 0U;
}

static int quota_snapshot_contains(
    const turbo_media_tenant_quota_lease_t *leases,
    size_t lease_count, const char *tenant_id) {
    size_t index;

    if (!tenant_id) {
        return 0;
    }
    for (index = 0U; index < lease_count; ++index) {
        if (leases[index].tenant_id &&
            strcmp(leases[index].tenant_id, tenant_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int quota_snapshot_valid(
    const turbo_media_tenant_quota_projection_t *projection,
    const turbo_media_tenant_quota_lease_t *leases,
    size_t lease_count) {
    size_t index;
    size_t previous;

    if (!projection ||
        (lease_count > 0U && !leases) ||
        lease_count > projection->max_tenants ||
        lease_count > TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS) {
        return 0;
    }
    for (index = 0U; index < lease_count; ++index) {
        if (!quota_lease_valid(projection, &leases[index])) {
            return 0;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (strcmp(
                    leases[previous].tenant_id,
                    leases[index].tenant_id) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

turbo_media_tenant_quota_projection_t *
turbo_media_tenant_quota_projection_create(
    const char *node_id, size_t max_tenants) {
    turbo_media_tenant_quota_projection_t *projection;

    if (!quota_node_valid(node_id) ||
        max_tenants == 0U ||
        max_tenants > TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS) {
        return NULL;
    }
    projection = (turbo_media_tenant_quota_projection_t *)calloc(
        1U, sizeof(*projection));
    if (!projection) {
        return NULL;
    }
    projection->slots = (turbo_media_tenant_quota_slot_t *)calloc(
        max_tenants, sizeof(*projection->slots));
    if (!projection->slots) {
        free(projection);
        return NULL;
    }
    memcpy(projection->node_id, node_id, strlen(node_id) + 1U);
    projection->max_tenants = max_tenants;
    return projection;
}

void turbo_media_tenant_quota_projection_destroy(
    turbo_media_tenant_quota_projection_t *projection) {
    if (!projection) {
        return;
    }
    if (projection->slots) {
        memset(projection->slots, 0,
               projection->max_tenants * sizeof(*projection->slots));
        free(projection->slots);
    }
    memset(projection, 0, sizeof(*projection));
    free(projection);
}

turbo_media_tenant_quota_apply_result_t
turbo_media_tenant_quota_apply_snapshot(
    turbo_media_tenant_quota_projection_t *projection,
    uint64_t epoch, uint64_t sequence,
    const turbo_media_tenant_quota_lease_t *leases,
    size_t lease_count) {
    turbo_media_tenant_quota_slot_t *replacement = NULL;
    size_t retained = 0U;
    size_t next = 0U;
    size_t index;

    if (!projection || epoch == 0U) {
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_ERROR;
    }
    if (projection->synchronized &&
        quota_version_compare(
            epoch, sequence,
            projection->epoch, projection->sequence) <= 0) {
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_STALE;
    }
    if (!projection->synchronized &&
        projection->required_epoch != 0U &&
        quota_version_compare(
            epoch, sequence,
            projection->required_epoch,
            projection->required_sequence) < 0) {
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_STALE;
    }
    if (!quota_snapshot_valid(projection, leases, lease_count)) {
        quota_require_snapshot(projection, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_ERROR;
    }

    for (index = 0U; index < projection->max_tenants; ++index) {
        const turbo_media_tenant_quota_slot_t *slot =
            &projection->slots[index];
        if (quota_usage_present(slot) &&
            !quota_snapshot_contains(
                leases, lease_count, slot->tenant_id)) {
            retained++;
        }
    }
    if (lease_count + retained > projection->max_tenants) {
        quota_require_snapshot(projection, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_LIMIT;
    }

    replacement = (turbo_media_tenant_quota_slot_t *)calloc(
        projection->max_tenants, sizeof(*replacement));
    if (!replacement) {
        quota_require_snapshot(projection, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_ERROR;
    }

    for (index = 0U; index < lease_count; ++index) {
        const turbo_media_tenant_quota_slot_t *old =
            quota_find_slot_const(projection, leases[index].tenant_id);
        turbo_media_tenant_quota_slot_t *slot = &replacement[next++];

        slot->occupied = 1;
        slot->has_lease = 1;
        memcpy(slot->tenant_id, leases[index].tenant_id,
               strlen(leases[index].tenant_id) + 1U);
        slot->expires_at_unix_ms = leases[index].expires_at_unix_ms;
        memcpy(slot->limits, leases[index].limits,
               sizeof(slot->limits));
        if (old) {
            memcpy(slot->used, old->used, sizeof(slot->used));
        }
    }

    for (index = 0U; index < projection->max_tenants; ++index) {
        const turbo_media_tenant_quota_slot_t *old =
            &projection->slots[index];
        turbo_media_tenant_quota_slot_t *slot;

        if (!quota_usage_present(old) ||
            quota_snapshot_contains(
                leases, lease_count, old->tenant_id)) {
            continue;
        }
        slot = &replacement[next++];
        slot->occupied = 1;
        memcpy(slot->tenant_id, old->tenant_id,
               strlen(old->tenant_id) + 1U);
        memcpy(slot->used, old->used, sizeof(slot->used));
    }

    memcpy(projection->slots, replacement,
           projection->max_tenants * sizeof(*replacement));
    memset(replacement, 0,
           projection->max_tenants * sizeof(*replacement));
    free(replacement);

    projection->synchronized = 1;
    projection->epoch = epoch;
    projection->sequence = sequence;
    projection->required_epoch = 0U;
    projection->required_sequence = 0U;
    return TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED;
}

turbo_media_tenant_quota_apply_result_t
turbo_media_tenant_quota_apply_update(
    turbo_media_tenant_quota_projection_t *projection,
    uint64_t epoch, uint64_t sequence,
    const turbo_media_tenant_quota_lease_t *lease) {
    turbo_media_tenant_quota_slot_t *slot = NULL;
    size_t index;

    if (!projection || epoch == 0U || sequence == 0U) {
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_ERROR;
    }
    if (!projection->synchronized) {
        quota_require_snapshot(projection, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_GAP;
    }
    if (quota_version_compare(
            epoch, sequence,
            projection->epoch, projection->sequence) <= 0) {
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_STALE;
    }
    if (epoch != projection->epoch ||
        projection->sequence == UINT64_MAX ||
        sequence != projection->sequence + 1U) {
        quota_require_snapshot(projection, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_GAP;
    }
    if (!quota_lease_valid(projection, lease)) {
        quota_require_snapshot(projection, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_ERROR;
    }

    slot = quota_find_slot(projection, lease->tenant_id);
    if (!slot) {
        for (index = 0U; index < projection->max_tenants; ++index) {
            turbo_media_tenant_quota_slot_t *candidate =
                &projection->slots[index];
            if (!candidate->occupied ||
                (!candidate->has_lease &&
                 !quota_usage_present(candidate))) {
                slot = candidate;
                break;
            }
        }
    }
    if (!slot) {
        quota_require_snapshot(projection, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_APPLY_LIMIT;
    }

    if (!slot->occupied ||
        strcmp(slot->tenant_id, lease->tenant_id) != 0) {
        memset(slot, 0, sizeof(*slot));
        slot->occupied = 1;
        memcpy(slot->tenant_id, lease->tenant_id,
               strlen(lease->tenant_id) + 1U);
    }
    slot->has_lease = 1;
    slot->expires_at_unix_ms = lease->expires_at_unix_ms;
    memcpy(slot->limits, lease->limits, sizeof(slot->limits));

    projection->epoch = epoch;
    projection->sequence = sequence;
    return TURBO_MEDIA_TENANT_QUOTA_APPLY_APPLIED;
}

turbo_media_tenant_quota_reserve_result_t
turbo_media_tenant_quota_reserve(
    turbo_media_tenant_quota_projection_t *projection,
    const char *tenant_id,
    turbo_media_tenant_quota_resource_t resource,
    uint32_t amount,
    uint64_t now_unix_ms) {
    turbo_media_tenant_quota_slot_t *slot;
    uint32_t limit;
    uint32_t used;

    if (!projection || !quota_tenant_valid(tenant_id) ||
        (int)resource < 0 ||
        (int)resource >= TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT ||
        amount == 0U) {
        return TURBO_MEDIA_TENANT_QUOTA_RESERVE_ERROR;
    }
    if (!projection->synchronized) {
        return TURBO_MEDIA_TENANT_QUOTA_RESERVE_UNKNOWN;
    }
    slot = quota_find_slot(projection, tenant_id);
    if (!slot || !slot->has_lease) {
        return TURBO_MEDIA_TENANT_QUOTA_RESERVE_NO_LEASE;
    }
    if (now_unix_ms >= slot->expires_at_unix_ms) {
        return TURBO_MEDIA_TENANT_QUOTA_RESERVE_EXPIRED;
    }

    limit = slot->limits[(size_t)resource];
    used = slot->used[(size_t)resource];
    if (used > limit || amount > limit - used) {
        return TURBO_MEDIA_TENANT_QUOTA_RESERVE_LIMIT;
    }
    slot->used[(size_t)resource] = used + amount;
    return TURBO_MEDIA_TENANT_QUOTA_RESERVE_OK;
}

int turbo_media_tenant_quota_release(
    turbo_media_tenant_quota_projection_t *projection,
    const char *tenant_id,
    turbo_media_tenant_quota_resource_t resource,
    uint32_t amount) {
    turbo_media_tenant_quota_slot_t *slot;
    size_t index;
    int any_used = 0;

    if (!projection || !quota_tenant_valid(tenant_id) ||
        (int)resource < 0 ||
        (int)resource >= TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT ||
        amount == 0U) {
        return -1;
    }
    slot = quota_find_slot(projection, tenant_id);
    if (!slot ||
        slot->used[(size_t)resource] < amount) {
        return -1;
    }
    slot->used[(size_t)resource] -= amount;
    for (index = 0U;
         index < TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT; ++index) {
        if (slot->used[index] != 0U) {
            any_used = 1;
            break;
        }
    }
    if (!any_used && !slot->has_lease) {
        memset(slot, 0, sizeof(*slot));
    }
    return 0;
}

int turbo_media_tenant_quota_status(
    const turbo_media_tenant_quota_projection_t *projection,
    int *out_synchronized,
    uint64_t *out_epoch,
    uint64_t *out_sequence,
    size_t *out_lease_count) {
    size_t count = 0U;
    size_t index;

    if (!projection || !out_synchronized || !out_epoch ||
        !out_sequence || !out_lease_count) {
        return -1;
    }
    for (index = 0U; index < projection->max_tenants; ++index) {
        if (projection->slots[index].occupied &&
            projection->slots[index].has_lease) {
            count++;
        }
    }
    *out_synchronized = projection->synchronized;
    *out_epoch = projection->epoch;
    *out_sequence = projection->sequence;
    *out_lease_count = count;
    return 0;
}

int turbo_media_tenant_quota_usage(
    const turbo_media_tenant_quota_projection_t *projection,
    const char *tenant_id,
    turbo_media_tenant_quota_resource_t resource,
    uint32_t *out_used,
    uint32_t *out_limit,
    uint64_t *out_expires_at_unix_ms,
    int *out_has_lease) {
    const turbo_media_tenant_quota_slot_t *slot;

    if (!projection || !quota_tenant_valid(tenant_id) ||
        (int)resource < 0 ||
        (int)resource >= TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT ||
        !out_used || !out_limit || !out_expires_at_unix_ms ||
        !out_has_lease) {
        return -1;
    }
    slot = quota_find_slot_const(projection, tenant_id);
    if (!slot) {
        return -1;
    }
    *out_used = slot->used[(size_t)resource];
    *out_limit = slot->has_lease
                     ? slot->limits[(size_t)resource]
                     : 0U;
    *out_expires_at_unix_ms =
        slot->has_lease ? slot->expires_at_unix_ms : 0U;
    *out_has_lease = slot->has_lease;
    return 0;
}
