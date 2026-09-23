#include "turbo_media_revocation_wire.h"

#include <json_parser.h>
#include <string.h>

static int wire_object_has_exact_keys(
    const json_value_t *object, const char *const *keys, size_t key_count) {
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

static int wire_uint32_required(
    const json_value_t *object, const char *key, uint32_t *output) {
    json_value_t *value;
    double number;
    uint32_t converted;

    if (!object || !key || !output) {
        return -1;
    }
    value = json_object_get(object, key);
    if (!value || json_type(value) != JSON_NUMBER) {
        return -1;
    }
    number = json_number(value);
    if (number < 0.0 || number > 4294967295.0) {
        return -1;
    }
    converted = (uint32_t)number;
    if ((double)converted != number) {
        return -1;
    }
    *output = converted;
    return 0;
}

static int wire_schema_v1(const json_value_t *object) {
    uint32_t version = 0U;
    return wire_uint32_required(object, "schema_version", &version) == 0 &&
           version == TURBO_MEDIA_REVOCATION_WIRE_SCHEMA_VERSION;
}

int turbo_media_revocation_wire_parse_snapshot(
    const char *body, size_t body_size,
    turbo_media_revocation_wire_snapshot_t *out_snapshot) {
    static const char *const keys[] = {
        "schema_version", "epoch", "sequence", "revoked_sha256"};
    json_value_t *root = NULL;
    json_value_t *list_value;
    const char *csv;
    size_t csv_size;
    char *cursor;

    if (!body || body_size == 0U ||
        body_size > TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES ||
        !out_snapshot) {
        return -1;
    }
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    root = json_parse(body, body_size);
    if (!root ||
        !wire_object_has_exact_keys(
            root, keys, sizeof(keys) / sizeof(keys[0])) ||
        !wire_schema_v1(root) ||
        wire_uint32_required(root, "epoch", &out_snapshot->epoch) != 0 ||
        out_snapshot->epoch == 0U ||
        wire_uint32_required(
            root, "sequence", &out_snapshot->sequence) != 0) {
        json_free(root);
        return -1;
    }

    list_value = json_object_get(root, "revoked_sha256");
    if (!list_value || json_type(list_value) != JSON_STRING) {
        json_free(root);
        return -1;
    }
    csv = json_string(list_value);
    csv_size = json_string_len(list_value);
    if (!csv || csv_size >= sizeof(out_snapshot->storage)) {
        json_free(root);
        return -1;
    }
    if (csv_size == 0U) {
        json_free(root);
        return 0;
    }

    memcpy(out_snapshot->storage, csv, csv_size);
    out_snapshot->storage[csv_size] = '\0';
    cursor = out_snapshot->storage;
    while (*cursor != '\0') {
        char *comma;
        size_t length;
        if (out_snapshot->count >=
            TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS) {
            json_free(root);
            memset(out_snapshot, 0, sizeof(*out_snapshot));
            return -1;
        }
        comma = strchr(cursor, ',');
        length = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (length != TURBO_MEDIA_REVOCATION_WIRE_SHA256_HEX_BYTES) {
            json_free(root);
            memset(out_snapshot, 0, sizeof(*out_snapshot));
            return -1;
        }
        out_snapshot->digests[out_snapshot->count++] = cursor;
        if (!comma) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
        if (*cursor == '\0') {
            json_free(root);
            memset(out_snapshot, 0, sizeof(*out_snapshot));
            return -1;
        }
    }

    json_free(root);
    return 0;
}

int turbo_media_revocation_wire_parse_revoke(
    const char *body, size_t body_size,
    turbo_media_revocation_wire_revoke_t *out_revoke) {
    static const char *const keys[] = {
        "schema_version", "epoch", "sequence", "sha256"};
    json_value_t *root = NULL;
    json_value_t *digest_value;
    const char *digest;

    if (!body || body_size == 0U ||
        body_size > TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES ||
        !out_revoke) {
        return -1;
    }
    memset(out_revoke, 0, sizeof(*out_revoke));
    root = json_parse(body, body_size);
    if (!root ||
        !wire_object_has_exact_keys(
            root, keys, sizeof(keys) / sizeof(keys[0])) ||
        !wire_schema_v1(root) ||
        wire_uint32_required(root, "epoch", &out_revoke->epoch) != 0 ||
        out_revoke->epoch == 0U ||
        wire_uint32_required(
            root, "sequence", &out_revoke->sequence) != 0 ||
        out_revoke->sequence == 0U) {
        json_free(root);
        return -1;
    }

    digest_value = json_object_get(root, "sha256");
    if (!digest_value || json_type(digest_value) != JSON_STRING ||
        json_string_len(digest_value) !=
            TURBO_MEDIA_REVOCATION_WIRE_SHA256_HEX_BYTES) {
        json_free(root);
        return -1;
    }
    digest = json_string(digest_value);
    if (!digest) {
        json_free(root);
        return -1;
    }
    memcpy(out_revoke->sha256, digest,
           TURBO_MEDIA_REVOCATION_WIRE_SHA256_HEX_BYTES);
    out_revoke->sha256[
        TURBO_MEDIA_REVOCATION_WIRE_SHA256_HEX_BYTES] = '\0';
    json_free(root);
    return 0;
}
