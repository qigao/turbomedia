#include "turbo_media_tenant_quota_allocator.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct tenant_quota_policy_slot_s {
    char tenant_id[TURBO_MEDIA_TENANT_QUOTA_TENANT_ID_BYTES];
    int occupied;
    uint32_t limits[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
} tenant_quota_policy_slot_t;

typedef struct tenant_quota_issued_slot_s {
    char tenant_id[TURBO_MEDIA_TENANT_QUOTA_TENANT_ID_BYTES];
    char node_id[TURBO_MEDIA_TENANT_QUOTA_NODE_ID_BYTES];
    int occupied;
    uint64_t expires_at_unix_ms;
    uint32_t limits[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
} tenant_quota_issued_slot_t;

struct turbo_media_tenant_quota_allocator_s {
    size_t max_tenants;
    size_t max_issued_leases;
    tenant_quota_policy_slot_t *policies;
    tenant_quota_issued_slot_t *leases;
    int synchronized;
    uint64_t epoch;
    uint64_t sequence;
    uint64_t required_epoch;
    uint64_t required_sequence;
};

static int allocator_version_compare(
    uint64_t left_epoch, uint64_t left_sequence,
    uint64_t right_epoch, uint64_t right_sequence) {
    if (left_epoch < right_epoch) return -1;
    if (left_epoch > right_epoch) return 1;
    if (left_sequence < right_sequence) return -1;
    if (left_sequence > right_sequence) return 1;
    return 0;
}

static void allocator_require_snapshot(
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t epoch, uint64_t sequence) {
    if (!allocator) return;
    allocator->synchronized = 0;
    if (allocator_version_compare(
            epoch, sequence,
            allocator->required_epoch,
            allocator->required_sequence) > 0) {
        allocator->required_epoch = epoch;
        allocator->required_sequence = sequence;
    }
}

static tenant_quota_policy_slot_t *allocator_find_policy(
    turbo_media_tenant_quota_allocator_t *allocator,
    const char *tenant_id) {
    size_t index;
    if (!allocator || !tenant_id) return NULL;
    for (index = 0U; index < allocator->max_tenants; ++index) {
        tenant_quota_policy_slot_t *slot = &allocator->policies[index];
        if (slot->occupied && strcmp(slot->tenant_id, tenant_id) == 0) {
            return slot;
        }
    }
    return NULL;
}

static const tenant_quota_policy_slot_t *allocator_find_policy_const(
    const turbo_media_tenant_quota_allocator_t *allocator,
    const char *tenant_id) {
    size_t index;
    if (!allocator || !tenant_id) return NULL;
    for (index = 0U; index < allocator->max_tenants; ++index) {
        const tenant_quota_policy_slot_t *slot = &allocator->policies[index];
        if (slot->occupied && strcmp(slot->tenant_id, tenant_id) == 0) {
            return slot;
        }
    }
    return NULL;
}

static tenant_quota_issued_slot_t *allocator_find_lease(
    turbo_media_tenant_quota_allocator_t *allocator,
    const char *tenant_id, const char *node_id) {
    size_t index;
    if (!allocator || !tenant_id || !node_id) return NULL;
    for (index = 0U; index < allocator->max_issued_leases; ++index) {
        tenant_quota_issued_slot_t *slot = &allocator->leases[index];
        if (slot->occupied &&
            strcmp(slot->tenant_id, tenant_id) == 0 &&
            strcmp(slot->node_id, node_id) == 0) {
            return slot;
        }
    }
    return NULL;
}

static void allocator_prune_expired(
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t now_unix_ms) {
    size_t index;
    if (!allocator) return;
    for (index = 0U; index < allocator->max_issued_leases; ++index) {
        tenant_quota_issued_slot_t *slot = &allocator->leases[index];
        if (slot->occupied &&
            now_unix_ms >= slot->expires_at_unix_ms) {
            memset(slot, 0, sizeof(*slot));
        }
    }
}

static int allocator_policy_input_valid(
    const turbo_media_tenant_quota_policy_t *policy) {
    return policy &&
           turbo_media_tenant_quota_tenant_id_valid(policy->tenant_id);
}

static int allocator_lease_input_valid(
    const turbo_media_tenant_quota_lease_t *lease,
    uint64_t now_unix_ms) {
    return lease &&
           turbo_media_tenant_quota_tenant_id_valid(lease->tenant_id) &&
           turbo_media_tenant_quota_node_id_valid(lease->node_id) &&
           lease->expires_at_unix_ms > now_unix_ms;
}

static int allocator_policy_snapshot_valid(
    const turbo_media_tenant_quota_policy_t *policies,
    size_t policy_count, size_t max_tenants) {
    size_t index;
    size_t previous;
    if ((policy_count > 0U && !policies) ||
        policy_count > max_tenants ||
        policy_count > TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS) {
        return 0;
    }
    for (index = 0U; index < policy_count; ++index) {
        if (!allocator_policy_input_valid(&policies[index])) return 0;
        for (previous = 0U; previous < index; ++previous) {
            if (strcmp(policies[previous].tenant_id,
                       policies[index].tenant_id) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

static const turbo_media_tenant_quota_policy_t *
allocator_input_policy_find(
    const turbo_media_tenant_quota_policy_t *policies,
    size_t policy_count, const char *tenant_id) {
    size_t index;
    for (index = 0U; index < policy_count; ++index) {
        if (strcmp(policies[index].tenant_id, tenant_id) == 0) {
            return &policies[index];
        }
    }
    return NULL;
}

static int allocator_snapshot_leases_valid(
    const turbo_media_tenant_quota_policy_t *policies,
    size_t policy_count,
    const turbo_media_tenant_quota_lease_t *leases,
    size_t lease_count,
    size_t max_issued_leases,
    uint64_t now_unix_ms) {
    uint64_t *totals = NULL;
    size_t index;
    size_t previous;
    int valid = 0;

    if ((lease_count > 0U && !leases) ||
        lease_count > max_issued_leases ||
        lease_count > TURBO_MEDIA_TENANT_QUOTA_MAX_ISSUED_LEASES) {
        return 0;
    }
    if (policy_count > SIZE_MAX /
            (TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT * sizeof(uint64_t))) {
        return 0;
    }
    totals = (uint64_t *)calloc(
        policy_count * TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT,
        sizeof(uint64_t));
    if (policy_count > 0U && !totals) return 0;

    for (index = 0U; index < lease_count; ++index) {
        const turbo_media_tenant_quota_policy_t *policy;
        size_t policy_index;
        size_t resource;

        if (!allocator_lease_input_valid(&leases[index], now_unix_ms)) {
            goto cleanup;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (strcmp(leases[previous].tenant_id,
                       leases[index].tenant_id) == 0 &&
                strcmp(leases[previous].node_id,
                       leases[index].node_id) == 0) {
                goto cleanup;
            }
        }
        policy = allocator_input_policy_find(
            policies, policy_count, leases[index].tenant_id);
        if (!policy) goto cleanup;
        policy_index = (size_t)(policy - policies);
        for (resource = 0U;
             resource < TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT;
             ++resource) {
            uint64_t *sum = &totals[
                policy_index * TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT +
                resource];
            *sum += leases[index].limits[resource];
            if (*sum > policy->limits[resource]) goto cleanup;
        }
    }

    valid = 1;
cleanup:
    free(totals);
    return valid;
}

static int allocator_exact_next(
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t epoch, uint64_t sequence) {
    if (!allocator || !allocator->synchronized) return 0;
    if (epoch != allocator->epoch ||
        allocator->sequence == UINT64_MAX ||
        sequence != allocator->sequence + 1U) {
        allocator_require_snapshot(allocator, epoch, sequence);
        return 0;
    }
    return 1;
}

static int allocator_commitments_for_tenant(
    const turbo_media_tenant_quota_allocator_t *allocator,
    const char *tenant_id,
    const tenant_quota_issued_slot_t *exclude,
    uint64_t out[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT]) {
    size_t index;
    size_t resource;
    if (!allocator || !tenant_id || !out) return -1;
    memset(out, 0,
           TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT * sizeof(uint64_t));
    for (index = 0U; index < allocator->max_issued_leases; ++index) {
        const tenant_quota_issued_slot_t *slot = &allocator->leases[index];
        if (!slot->occupied || slot == exclude ||
            strcmp(slot->tenant_id, tenant_id) != 0) {
            continue;
        }
        for (resource = 0U;
             resource < TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT;
             ++resource) {
            out[resource] += slot->limits[resource];
        }
    }
    return 0;
}

turbo_media_tenant_quota_allocator_t *
turbo_media_tenant_quota_allocator_create(
    size_t max_tenants, size_t max_issued_leases) {
    turbo_media_tenant_quota_allocator_t *allocator;
    if (max_tenants == 0U ||
        max_tenants > TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS ||
        max_issued_leases == 0U ||
        max_issued_leases >
            TURBO_MEDIA_TENANT_QUOTA_MAX_ISSUED_LEASES) {
        return NULL;
    }
    allocator = (turbo_media_tenant_quota_allocator_t *)calloc(
        1U, sizeof(*allocator));
    if (!allocator) return NULL;
    allocator->policies = (tenant_quota_policy_slot_t *)calloc(
        max_tenants, sizeof(*allocator->policies));
    allocator->leases = (tenant_quota_issued_slot_t *)calloc(
        max_issued_leases, sizeof(*allocator->leases));
    if (!allocator->policies || !allocator->leases) {
        free(allocator->leases);
        free(allocator->policies);
        free(allocator);
        return NULL;
    }
    allocator->max_tenants = max_tenants;
    allocator->max_issued_leases = max_issued_leases;
    return allocator;
}

void turbo_media_tenant_quota_allocator_destroy(
    turbo_media_tenant_quota_allocator_t *allocator) {
    if (!allocator) return;
    if (allocator->policies) {
        memset(allocator->policies, 0,
               allocator->max_tenants * sizeof(*allocator->policies));
        free(allocator->policies);
    }
    if (allocator->leases) {
        memset(allocator->leases, 0,
               allocator->max_issued_leases * sizeof(*allocator->leases));
        free(allocator->leases);
    }
    memset(allocator, 0, sizeof(*allocator));
    free(allocator);
}

turbo_media_tenant_quota_allocator_result_t
turbo_media_tenant_quota_allocator_apply_snapshot(
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t epoch, uint64_t sequence,
    const turbo_media_tenant_quota_policy_t *policies,
    size_t policy_count,
    const turbo_media_tenant_quota_lease_t *leases,
    size_t lease_count,
    uint64_t now_unix_ms) {
    tenant_quota_policy_slot_t *new_policies = NULL;
    tenant_quota_issued_slot_t *new_leases = NULL;
    size_t index;

    if (!allocator || epoch == 0U) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR;
    }
    if (allocator->synchronized &&
        allocator_version_compare(
            epoch, sequence,
            allocator->epoch, allocator->sequence) <= 0) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_STALE;
    }
    if (!allocator->synchronized &&
        allocator->required_epoch != 0U &&
        allocator_version_compare(
            epoch, sequence,
            allocator->required_epoch,
            allocator->required_sequence) < 0) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_STALE;
    }
    if (!allocator_policy_snapshot_valid(
            policies, policy_count, allocator->max_tenants) ||
        !allocator_snapshot_leases_valid(
            policies, policy_count, leases, lease_count,
            allocator->max_issued_leases, now_unix_ms)) {
        allocator_require_snapshot(allocator, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR;
    }

    new_policies = (tenant_quota_policy_slot_t *)calloc(
        allocator->max_tenants, sizeof(*new_policies));
    new_leases = (tenant_quota_issued_slot_t *)calloc(
        allocator->max_issued_leases, sizeof(*new_leases));
    if (!new_policies || !new_leases) {
        free(new_leases);
        free(new_policies);
        allocator_require_snapshot(allocator, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR;
    }

    for (index = 0U; index < policy_count; ++index) {
        new_policies[index].occupied = 1;
        memcpy(new_policies[index].tenant_id, policies[index].tenant_id,
               strlen(policies[index].tenant_id) + 1U);
        memcpy(new_policies[index].limits, policies[index].limits,
               sizeof(new_policies[index].limits));
    }
    for (index = 0U; index < lease_count; ++index) {
        new_leases[index].occupied = 1;
        memcpy(new_leases[index].tenant_id, leases[index].tenant_id,
               strlen(leases[index].tenant_id) + 1U);
        memcpy(new_leases[index].node_id, leases[index].node_id,
               strlen(leases[index].node_id) + 1U);
        new_leases[index].expires_at_unix_ms =
            leases[index].expires_at_unix_ms;
        memcpy(new_leases[index].limits, leases[index].limits,
               sizeof(new_leases[index].limits));
    }

    memcpy(allocator->policies, new_policies,
           allocator->max_tenants * sizeof(*new_policies));
    memcpy(allocator->leases, new_leases,
           allocator->max_issued_leases * sizeof(*new_leases));
    memset(new_policies, 0,
           allocator->max_tenants * sizeof(*new_policies));
    memset(new_leases, 0,
           allocator->max_issued_leases * sizeof(*new_leases));
    free(new_leases);
    free(new_policies);

    allocator->synchronized = 1;
    allocator->epoch = epoch;
    allocator->sequence = sequence;
    allocator->required_epoch = 0U;
    allocator->required_sequence = 0U;
    return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED;
}

turbo_media_tenant_quota_allocator_result_t
turbo_media_tenant_quota_allocator_set_policy(
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t epoch, uint64_t sequence,
    const turbo_media_tenant_quota_policy_t *policy,
    uint64_t now_unix_ms) {
    tenant_quota_policy_slot_t *slot;
    uint64_t committed[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
    size_t index;
    size_t resource;

    if (!allocator || !allocator_policy_input_valid(policy)) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR;
    }
    if (!allocator->synchronized) {
        allocator_require_snapshot(allocator, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_UNKNOWN;
    }
    if (allocator_version_compare(
            epoch, sequence,
            allocator->epoch, allocator->sequence) <= 0) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_STALE;
    }
    if (!allocator_exact_next(allocator, epoch, sequence)) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_GAP;
    }

    allocator_prune_expired(allocator, now_unix_ms);
    if (allocator_commitments_for_tenant(
            allocator, policy->tenant_id, NULL, committed) != 0) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR;
    }
    for (resource = 0U;
         resource < TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT;
         ++resource) {
        if (committed[resource] > policy->limits[resource]) {
            return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_OVERCOMMITTED;
        }
    }

    slot = allocator_find_policy(allocator, policy->tenant_id);
    if (!slot) {
        for (index = 0U; index < allocator->max_tenants; ++index) {
            if (!allocator->policies[index].occupied) {
                slot = &allocator->policies[index];
                break;
            }
        }
    }
    if (!slot) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_LIMIT;
    }
    if (!slot->occupied) {
        memset(slot, 0, sizeof(*slot));
        slot->occupied = 1;
        memcpy(slot->tenant_id, policy->tenant_id,
               strlen(policy->tenant_id) + 1U);
    }
    memcpy(slot->limits, policy->limits, sizeof(slot->limits));
    allocator->sequence = sequence;
    return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED;
}

turbo_media_tenant_quota_allocator_result_t
turbo_media_tenant_quota_allocator_grant(
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t epoch, uint64_t sequence,
    const turbo_media_tenant_quota_lease_t *lease,
    uint64_t now_unix_ms) {
    const tenant_quota_policy_slot_t *policy;
    tenant_quota_issued_slot_t *slot;
    uint64_t committed[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
    size_t index;
    size_t resource;

    if (!allocator ||
        !allocator_lease_input_valid(lease, now_unix_ms)) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR;
    }
    if (!allocator->synchronized) {
        allocator_require_snapshot(allocator, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_UNKNOWN;
    }
    if (allocator_version_compare(
            epoch, sequence,
            allocator->epoch, allocator->sequence) <= 0) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_STALE;
    }
    if (!allocator_exact_next(allocator, epoch, sequence)) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_GAP;
    }

    allocator_prune_expired(allocator, now_unix_ms);
    policy = allocator_find_policy_const(allocator, lease->tenant_id);
    if (!policy) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_NOT_FOUND;
    }
    slot = allocator_find_lease(
        allocator, lease->tenant_id, lease->node_id);
    if (allocator_commitments_for_tenant(
            allocator, lease->tenant_id, slot, committed) != 0) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR;
    }
    for (resource = 0U;
         resource < TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT;
         ++resource) {
        committed[resource] += lease->limits[resource];
        if (committed[resource] > policy->limits[resource]) {
            return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_OVERCOMMITTED;
        }
    }

    if (!slot) {
        for (index = 0U; index < allocator->max_issued_leases; ++index) {
            if (!allocator->leases[index].occupied) {
                slot = &allocator->leases[index];
                break;
            }
        }
    }
    if (!slot) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_LIMIT;
    }
    memset(slot, 0, sizeof(*slot));
    slot->occupied = 1;
    memcpy(slot->tenant_id, lease->tenant_id,
           strlen(lease->tenant_id) + 1U);
    memcpy(slot->node_id, lease->node_id,
           strlen(lease->node_id) + 1U);
    slot->expires_at_unix_ms = lease->expires_at_unix_ms;
    memcpy(slot->limits, lease->limits, sizeof(slot->limits));
    allocator->sequence = sequence;
    return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED;
}

turbo_media_tenant_quota_allocator_result_t
turbo_media_tenant_quota_allocator_revoke(
    turbo_media_tenant_quota_allocator_t *allocator,
    uint64_t epoch, uint64_t sequence,
    const char *tenant_id, const char *node_id,
    uint64_t now_unix_ms) {
    tenant_quota_issued_slot_t *slot;

    if (!allocator ||
        !turbo_media_tenant_quota_tenant_id_valid(tenant_id) ||
        !turbo_media_tenant_quota_node_id_valid(node_id)) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_ERROR;
    }
    if (!allocator->synchronized) {
        allocator_require_snapshot(allocator, epoch, sequence);
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_UNKNOWN;
    }
    if (allocator_version_compare(
            epoch, sequence,
            allocator->epoch, allocator->sequence) <= 0) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_STALE;
    }
    if (!allocator_exact_next(allocator, epoch, sequence)) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_GAP;
    }

    allocator_prune_expired(allocator, now_unix_ms);
    slot = allocator_find_lease(allocator, tenant_id, node_id);
    if (!slot) {
        return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_NOT_FOUND;
    }
    memset(slot, 0, sizeof(*slot));
    allocator->sequence = sequence;
    return TURBO_MEDIA_TENANT_QUOTA_ALLOCATOR_APPLIED;
}

int turbo_media_tenant_quota_allocator_build_node_snapshot(
    turbo_media_tenant_quota_allocator_t *allocator,
    const char *node_id, uint64_t now_unix_ms,
    turbo_media_tenant_quota_lease_t *out_leases,
    size_t capacity, size_t *out_count,
    uint64_t *out_epoch, uint64_t *out_sequence) {
    size_t count = 0U;
    size_t index;

    if (!allocator ||
        !turbo_media_tenant_quota_node_id_valid(node_id) ||
        !out_count || !out_epoch || !out_sequence ||
        (capacity > 0U && !out_leases) ||
        !allocator->synchronized) {
        return -1;
    }
    allocator_prune_expired(allocator, now_unix_ms);
    for (index = 0U; index < allocator->max_issued_leases; ++index) {
        const tenant_quota_issued_slot_t *slot = &allocator->leases[index];
        if (slot->occupied && strcmp(slot->node_id, node_id) == 0) {
            count++;
        }
    }
    if (count > capacity) {
        *out_count = count;
        return -1;
    }
    count = 0U;
    for (index = 0U; index < allocator->max_issued_leases; ++index) {
        const tenant_quota_issued_slot_t *slot = &allocator->leases[index];
        turbo_media_tenant_quota_lease_t *lease;
        if (!slot->occupied || strcmp(slot->node_id, node_id) != 0) {
            continue;
        }
        lease = &out_leases[count++];
        memset(lease, 0, sizeof(*lease));
        lease->tenant_id = slot->tenant_id;
        lease->node_id = slot->node_id;
        lease->expires_at_unix_ms = slot->expires_at_unix_ms;
        memcpy(lease->limits, slot->limits, sizeof(lease->limits));
    }
    *out_count = count;
    *out_epoch = allocator->epoch;
    *out_sequence = allocator->sequence;
    return 0;
}

int turbo_media_tenant_quota_allocator_commitment(
    turbo_media_tenant_quota_allocator_t *allocator,
    const char *tenant_id, uint64_t now_unix_ms,
    uint32_t out_policy[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT],
    uint32_t out_issued[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT]) {
    const tenant_quota_policy_slot_t *policy;
    uint64_t committed[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT];
    size_t resource;

    if (!allocator ||
        !turbo_media_tenant_quota_tenant_id_valid(tenant_id) ||
        !out_policy || !out_issued || !allocator->synchronized) {
        return -1;
    }
    allocator_prune_expired(allocator, now_unix_ms);
    policy = allocator_find_policy_const(allocator, tenant_id);
    if (!policy ||
        allocator_commitments_for_tenant(
            allocator, tenant_id, NULL, committed) != 0) {
        return -1;
    }
    for (resource = 0U;
         resource < TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT;
         ++resource) {
        if (committed[resource] > UINT32_MAX) return -1;
        out_policy[resource] = policy->limits[resource];
        out_issued[resource] = (uint32_t)committed[resource];
    }
    return 0;
}

int turbo_media_tenant_quota_allocator_status(
    const turbo_media_tenant_quota_allocator_t *allocator,
    int *out_synchronized, uint64_t *out_epoch,
    uint64_t *out_sequence, size_t *out_policy_count,
    size_t *out_lease_count) {
    size_t policies = 0U;
    size_t leases = 0U;
    size_t index;

    if (!allocator || !out_synchronized || !out_epoch ||
        !out_sequence || !out_policy_count || !out_lease_count) {
        return -1;
    }
    for (index = 0U; index < allocator->max_tenants; ++index) {
        if (allocator->policies[index].occupied) policies++;
    }
    for (index = 0U; index < allocator->max_issued_leases; ++index) {
        if (allocator->leases[index].occupied) leases++;
    }
    *out_synchronized = allocator->synchronized;
    *out_epoch = allocator->epoch;
    *out_sequence = allocator->sequence;
    *out_policy_count = policies;
    *out_lease_count = leases;
    return 0;
}
