#include "turbo_media_tenant_quota_wire.h"

#include <json_parser.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct turbo_media_tenant_quota_wire_snapshot_s {
    uint64_t epoch;
    uint64_t sequence;
    char node_id[TURBO_MEDIA_TENANT_QUOTA_NODE_ID_BYTES];
    size_t count;
    turbo_media_tenant_quota_lease_t
        leases[TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS];
    char tenant_ids[TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS]
                   [TURBO_MEDIA_TENANT_QUOTA_TENANT_ID_BYTES];
};

static int quota_wire_object_has_exact_keys(
    const json_value_t *object,
    const char *const *keys,
    size_t key_count) {
    size_t index;
    size_t expected;

    if (!object || json_type(object) != JSON_OBJECT ||
        json_object_size(object) != key_count) {
        return 0;
    }
    for (index = 0U; index < key_count; ++index) {
        const char *key = json_object_key(object, index);
        int found = 0;
        if (!key) {
            return 0;
        }
        for (expected = 0U; expected < key_count; ++expected) {
            if (strcmp(key, keys[expected]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return 0;
        }
    }
    for (expected = 0U; expected < key_count; ++expected) {
        if (!json_object_get(object, keys[expected])) {
            return 0;
        }
    }
    return 1;
}

static int quota_wire_uint64(
    const json_value_t *object, const char *key, uint64_t *output) {
    json_value_t *value;
    const char *text;
    size_t length = 0U;
    size_t index;
    uint64_t parsed = 0U;

    if (!object || !key || !output) {
        return -1;
    }
    value = json_object_get(object, key);
    if (!value || json_type(value) != JSON_NUMBER) {
        return -1;
    }
    text = json_number_text(value, &length);
    if (!text || length == 0U) {
        return -1;
    }
    for (index = 0U; index < length; ++index) {
        unsigned int digit;
        if (text[index] < '0' || text[index] > '9') {
            return -1;
        }
        digit = (unsigned int)(text[index] - '0');
        if (parsed > (UINT64_MAX - digit) / 10U) {
            return -1;
        }
        parsed = parsed * 10U + digit;
    }
    *output = parsed;
    return 0;
}

static int quota_wire_uint32(
    const json_value_t *object, const char *key, uint32_t *output) {
    uint64_t value;
    if (quota_wire_uint64(object, key, &value) != 0 ||
        value > UINT32_MAX) {
        return -1;
    }
    *output = (uint32_t)value;
    return 0;
}

static int quota_wire_copy_string(
    const json_value_t *object, const char *key,
    char *output, size_t capacity) {
    json_value_t *value;
    const char *text;
    size_t length;

    if (!object || !key || !output || capacity < 2U) {
        return -1;
    }
    value = json_object_get(object, key);
    if (!value || json_type(value) != JSON_STRING) {
        return -1;
    }
    text = json_string(value);
    length = json_string_len(value);
    if (!text || length == 0U || length >= capacity ||
        memchr(text, '\0', length) != NULL) {
        return -1;
    }
    memcpy(output, text, length);
    output[length] = '\0';
    return 0;
}

static int quota_wire_limits(
    const json_value_t *lease_object,
    uint32_t limits[TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT]) {
    static const char *const keys[] = {
        "signaling_connections",
        "rooms",
        "participants",
        "media_sessions",
        "published_tracks"};
    json_value_t *object;
    size_t index;

    if (!lease_object || !limits) {
        return -1;
    }
    object = json_object_get(lease_object, "limits");
    if (!object ||
        !quota_wire_object_has_exact_keys(
            object, keys, sizeof(keys) / sizeof(keys[0]))) {
        return -1;
    }
    for (index = 0U;
         index < TURBO_MEDIA_TENANT_QUOTA_RESOURCE_COUNT; ++index) {
        if (quota_wire_uint32(object, keys[index], &limits[index]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int quota_wire_lease(
    const json_value_t *object,
    const char *node_id,
    char tenant_id[TURBO_MEDIA_TENANT_QUOTA_TENANT_ID_BYTES],
    turbo_media_tenant_quota_lease_t *lease) {
    static const char *const keys[] = {
        "tenant_id", "expires_at_unix_ms", "limits"};
    uint64_t expires_at_unix_ms;

    if (!object || !node_id || !tenant_id || !lease ||
        !quota_wire_object_has_exact_keys(
            object, keys, sizeof(keys) / sizeof(keys[0])) ||
        quota_wire_copy_string(
            object, "tenant_id", tenant_id,
            TURBO_MEDIA_TENANT_QUOTA_TENANT_ID_BYTES) != 0 ||
        quota_wire_uint64(
            object, "expires_at_unix_ms", &expires_at_unix_ms) != 0 ||
        quota_wire_limits(object, lease->limits) != 0) {
        return -1;
    }
    lease->tenant_id = tenant_id;
    lease->node_id = node_id;
    lease->expires_at_unix_ms = expires_at_unix_ms;
    return 0;
}

static int quota_wire_root_version(
    const json_value_t *root, uint64_t *epoch, uint64_t *sequence) {
    uint64_t version = 0U;

    return quota_wire_uint64(root, "schema_version", &version) == 0 &&
           version == TURBO_MEDIA_TENANT_QUOTA_WIRE_SCHEMA_VERSION &&
           quota_wire_uint64(root, "epoch", epoch) == 0 &&
           *epoch != 0U &&
           quota_wire_uint64(root, "sequence", sequence) == 0;
}

turbo_media_tenant_quota_wire_snapshot_t *
turbo_media_tenant_quota_wire_parse_snapshot(
    const char *body, size_t body_size) {
    static const char *const keys[] = {
        "schema_version", "epoch", "sequence", "node_id", "leases"};
    turbo_media_tenant_quota_wire_snapshot_t *snapshot = NULL;
    json_value_t *root = NULL;
    json_value_t *leases;
    size_t count;
    size_t index;

    if (!body || body_size == 0U ||
        body_size > TURBO_MEDIA_TENANT_QUOTA_WIRE_MAX_BODY_BYTES) {
        return NULL;
    }
    root = json_parse(body, body_size);
    if (!root ||
        !quota_wire_object_has_exact_keys(
            root, keys, sizeof(keys) / sizeof(keys[0]))) {
        json_free(root);
        return NULL;
    }

    snapshot = (turbo_media_tenant_quota_wire_snapshot_t *)calloc(
        1U, sizeof(*snapshot));
    if (!snapshot) {
        json_free(root);
        return NULL;
    }
    if (!quota_wire_root_version(
            root, &snapshot->epoch, &snapshot->sequence) ||
        quota_wire_copy_string(
            root, "node_id", snapshot->node_id,
            sizeof(snapshot->node_id)) != 0) {
        json_free(root);
        free(snapshot);
        return NULL;
    }

    leases = json_object_get(root, "leases");
    if (!leases || json_type(leases) != JSON_ARRAY) {
        json_free(root);
        free(snapshot);
        return NULL;
    }
    count = json_array_size(leases);
    if (count > TURBO_MEDIA_TENANT_QUOTA_MAX_TENANTS) {
        json_free(root);
        free(snapshot);
        return NULL;
    }
    snapshot->count = count;
    for (index = 0U; index < count; ++index) {
        json_value_t *lease_object = json_array_get(leases, index);
        if (!lease_object ||
            quota_wire_lease(
                lease_object, snapshot->node_id,
                snapshot->tenant_ids[index],
                &snapshot->leases[index]) != 0) {
            json_free(root);
            memset(snapshot, 0, sizeof(*snapshot));
            free(snapshot);
            return NULL;
        }
    }

    json_free(root);
    return snapshot;
}

void turbo_media_tenant_quota_wire_snapshot_destroy(
    turbo_media_tenant_quota_wire_snapshot_t *snapshot) {
    if (!snapshot) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    free(snapshot);
}

uint64_t turbo_media_tenant_quota_wire_snapshot_epoch(
    const turbo_media_tenant_quota_wire_snapshot_t *snapshot) {
    return snapshot ? snapshot->epoch : 0U;
}

uint64_t turbo_media_tenant_quota_wire_snapshot_sequence(
    const turbo_media_tenant_quota_wire_snapshot_t *snapshot) {
    return snapshot ? snapshot->sequence : 0U;
}

const char *turbo_media_tenant_quota_wire_snapshot_node_id(
    const turbo_media_tenant_quota_wire_snapshot_t *snapshot) {
    return snapshot ? snapshot->node_id : NULL;
}

size_t turbo_media_tenant_quota_wire_snapshot_count(
    const turbo_media_tenant_quota_wire_snapshot_t *snapshot) {
    return snapshot ? snapshot->count : 0U;
}

const turbo_media_tenant_quota_lease_t *
turbo_media_tenant_quota_wire_snapshot_leases(
    const turbo_media_tenant_quota_wire_snapshot_t *snapshot) {
    return snapshot ? snapshot->leases : NULL;
}

int turbo_media_tenant_quota_wire_parse_update(
    const char *body, size_t body_size,
    turbo_media_tenant_quota_wire_update_t *out_update) {
    static const char *const keys[] = {
        "schema_version", "epoch", "sequence", "node_id", "lease"};
    json_value_t *root = NULL;
    json_value_t *lease_object;
    int result = -1;

    if (!body || body_size == 0U ||
        body_size > TURBO_MEDIA_TENANT_QUOTA_WIRE_MAX_BODY_BYTES ||
        !out_update) {
        return -1;
    }
    memset(out_update, 0, sizeof(*out_update));
    root = json_parse(body, body_size);
    if (!root ||
        !quota_wire_object_has_exact_keys(
            root, keys, sizeof(keys) / sizeof(keys[0])) ||
        !quota_wire_root_version(
            root, &out_update->epoch, &out_update->sequence) ||
        quota_wire_copy_string(
            root, "node_id", out_update->node_id,
            sizeof(out_update->node_id)) != 0) {
        goto cleanup;
    }
    lease_object = json_object_get(root, "lease");
    if (!lease_object ||
        quota_wire_lease(
            lease_object, out_update->node_id,
            out_update->tenant_id, &out_update->lease) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    json_free(root);
    if (result != 0) {
        memset(out_update, 0, sizeof(*out_update));
    }
    return result;
}
