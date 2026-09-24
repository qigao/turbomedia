#include "turbo_media_revocation_https.h"

#include "turbo_media_revocation_wire.h"
#include "turbo_transport.h"

#include <chttp/chttp.h>
#include <json_parser.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct revocation_https_target_s {
    char target_id[TURBO_MEDIA_REVOCATION_FANOUT_TARGET_ID_BYTES];
    char base_url[TURBO_MEDIA_REVOCATION_HTTPS_ENDPOINT_BYTES];
    char ca_file[TURBO_MEDIA_REVOCATION_HTTPS_CA_PATH_BYTES];
    char server_name[TURBO_MEDIA_REVOCATION_HTTPS_TLS_NAME_BYTES];
} revocation_https_target_t;

struct turbo_media_revocation_https_s {
    turbo_media_revocation_https_config_t config;
    revocation_https_target_t *targets;
    size_t target_count;
};

static int copy_bounded(char *output, size_t capacity, const char *value) {
    size_t length;
    if (!output || capacity == 0U || !value) {
        return -1;
    }
    length = strlen(value);
    if (length == 0U || length >= capacity) {
        return -1;
    }
    memcpy(output, value, length + 1U);
    return 0;
}

static int target_id_valid(const char *value) {
    size_t length;
    if (!value || value[0] == '\0') {
        return 0;
    }
    length = strlen(value);
    if (length >= TURBO_MEDIA_REVOCATION_FANOUT_TARGET_ID_BYTES) {
        return 0;
    }
    for (size_t index = 0U; index < length; ++index) {
        unsigned char ch = (unsigned char)value[index];
        if (!((ch >= 'A' && ch <= 'Z') ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-' || ch == '.' || ch == ':')) {
            return 0;
        }
    }
    return 1;
}

static void free_parsed_url(turbo_transport_config_t *config) {
    if (!config) {
        return;
    }
    free((void *)config->host);
    free((void *)config->path);
    config->host = NULL;
    config->path = NULL;
}

static int target_config_valid(
    const turbo_media_revocation_https_target_config_t *target) {
    turbo_transport_config_t parsed = {0};
    int valid;

    if (!target || !target_id_valid(target->target_id) ||
        !target->base_url || !target->ca_file || !target->server_name ||
        target->ca_file[0] == '\0' || target->server_name[0] == '\0' ||
        strlen(target->base_url) >= TURBO_MEDIA_REVOCATION_HTTPS_ENDPOINT_BYTES ||
        strlen(target->ca_file) >= TURBO_MEDIA_REVOCATION_HTTPS_CA_PATH_BYTES ||
        strlen(target->server_name) >= TURBO_MEDIA_REVOCATION_HTTPS_TLS_NAME_BYTES ||
        turbo_transport_parse_url(target->base_url, &parsed) != 0) {
        return 0;
    }
    valid = parsed.type == TURBO_TRANSPORT_HTTP && parsed.use_tls &&
            parsed.host && parsed.host[0] != '\0' &&
            (!parsed.path || strcmp(parsed.path, "/") == 0);
    free_parsed_url(&parsed);
    return valid;
}

turbo_media_revocation_https_t *turbo_media_revocation_https_create(
    const turbo_media_revocation_https_config_t *config,
    const turbo_media_revocation_https_target_config_t *targets,
    size_t target_count) {
    turbo_media_revocation_https_t *adapter;

    if (!config || !config->acquire_token || config->timeout_ms == 0U ||
        !targets || target_count == 0U ||
        target_count > TURBO_MEDIA_REVOCATION_HTTPS_MAX_TARGETS) {
        return NULL;
    }

    adapter = (turbo_media_revocation_https_t *)calloc(1U, sizeof(*adapter));
    if (!adapter) {
        return NULL;
    }
    adapter->targets = (revocation_https_target_t *)calloc(
        target_count, sizeof(*adapter->targets));
    if (!adapter->targets) {
        free(adapter);
        return NULL;
    }
    adapter->config = *config;
    adapter->target_count = target_count;

    for (size_t index = 0U; index < target_count; ++index) {
        revocation_https_target_t *destination = &adapter->targets[index];
        if (!target_config_valid(&targets[index])) {
            turbo_media_revocation_https_destroy(adapter);
            return NULL;
        }
        for (size_t prior = 0U; prior < index; ++prior) {
            if (strcmp(targets[index].target_id,
                       adapter->targets[prior].target_id) == 0) {
                turbo_media_revocation_https_destroy(adapter);
                return NULL;
            }
        }
        if (copy_bounded(destination->target_id,
                         sizeof(destination->target_id),
                         targets[index].target_id) != 0 ||
            copy_bounded(destination->base_url,
                         sizeof(destination->base_url),
                         targets[index].base_url) != 0 ||
            copy_bounded(destination->ca_file,
                         sizeof(destination->ca_file),
                         targets[index].ca_file) != 0 ||
            copy_bounded(destination->server_name,
                         sizeof(destination->server_name),
                         targets[index].server_name) != 0) {
            turbo_media_revocation_https_destroy(adapter);
            return NULL;
        }
    }

    return adapter;
}

void turbo_media_revocation_https_destroy(
    turbo_media_revocation_https_t *adapter) {
    if (!adapter) {
        return;
    }
    if (adapter->targets) {
        memset(adapter->targets, 0,
               adapter->target_count * sizeof(*adapter->targets));
    }
    free(adapter->targets);
    memset(adapter, 0, sizeof(*adapter));
    free(adapter);
}

size_t turbo_media_revocation_https_target_count(
    const turbo_media_revocation_https_t *adapter) {
    return adapter ? adapter->target_count : 0U;
}

int turbo_media_revocation_https_get_fanout_target(
    turbo_media_revocation_https_t *adapter,
    size_t index,
    turbo_media_revocation_fanout_target_t *out_target) {
    if (!adapter || !out_target || index >= adapter->target_count) {
        return -1;
    }
    out_target->target_id = adapter->targets[index].target_id;
    out_target->target_context = &adapter->targets[index];
    return 0;
}

static int response_uint64(
    const json_value_t *root, const char *key, uint64_t *output) {
    json_value_t *value;
    const char *text;
    size_t length = 0U;
    char *end = NULL;
    unsigned long long parsed;

    if (!root || !key || !output) {
        return -1;
    }
    value = json_object_get(root, key);
    if (!value || json_type(value) != JSON_NUMBER) {
        return -1;
    }
    text = json_number_text(value, &length);
    if (!text || length == 0U || length >= 32U) {
        return -1;
    }
    {
        char buffer[32];
        memcpy(buffer, text, length);
        buffer[length] = '\0';
        if (buffer[0] == '-') {
            return -1;
        }
        for (size_t index = 0U; index < length; ++index) {
            if (buffer[index] < '0' || buffer[index] > '9') {
                return -1;
            }
        }
        errno = 0;
        parsed = strtoull(buffer, &end, 10);
        if (errno == ERANGE || !end || *end != '\0') {
            return -1;
        }
    }
    *output = (uint64_t)parsed;
    return 0;
}

static turbo_media_revocation_fanout_transport_result_t parse_response(
    const chttp_response *raw,
    turbo_media_revocation_fanout_response_t *response) {
    json_value_t *root = NULL;
    json_value_t *result_value;
    json_value_t *synchronized_value;
    const char *result;
    turbo_media_revocation_fanout_transport_result_t mapped =
        TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_ERROR;

    if (!raw) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE;
    }
    if (raw->status_code == 401U || raw->status_code == 403U ||
        raw->status_code == 400U || raw->status_code == 404U) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    if (raw->status_code == 429U || raw->status_code >= 500U) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE;
    }
    if (!raw->body || raw->body_size == 0U) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_ERROR;
    }
    root = json_parse((const char *)raw->body, raw->body_size);
    if (!root || json_type(root) != JSON_OBJECT) {
        json_free(root);
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_ERROR;
    }
    result_value = json_object_get(root, "result");
    synchronized_value = json_object_get(root, "synchronized");
    if (!result_value || json_type(result_value) != JSON_STRING ||
        !synchronized_value || json_type(synchronized_value) != JSON_BOOL ||
        response_uint64(root, "epoch", &response->epoch) != 0 ||
        response_uint64(root, "sequence", &response->sequence) != 0) {
        json_free(root);
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_ERROR;
    }
    response->synchronized = json_bool(synchronized_value) ? 1 : 0;
    result = json_string(result_value);
    if (strcmp(result, "applied") == 0) {
        mapped = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_APPLIED;
    } else if (strcmp(result, "stale") == 0) {
        mapped = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_STALE;
    } else if (strcmp(result, "gap") == 0) {
        mapped = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_GAP;
    } else if (strcmp(result, "limit") == 0) {
        mapped = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_LIMIT;
    } else if (strcmp(result, "error") == 0) {
        mapped = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_ERROR;
    }
    if ((raw->status_code >= 200U && raw->status_code < 300U &&
         mapped != TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_APPLIED &&
         mapped != TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_STALE) ||
        (raw->status_code == 409U &&
         mapped != TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_GAP &&
         mapped != TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_LIMIT)) {
        mapped = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_ERROR;
    }
    json_free(root);
    return mapped;
}

static turbo_media_revocation_fanout_transport_result_t post_json(
    turbo_media_revocation_https_t *adapter,
    revocation_https_target_t *target,
    const char *target_id,
    const char *path,
    const char *body,
    size_t body_size,
    unsigned int attempt,
    turbo_media_revocation_fanout_response_t *response) {
    turbo_transport_config_t config = {0};
    cnet_tls_client_config tls = {0};
    turbo_transport_t *transport = NULL;
    chttp_response *raw = NULL;
    const char *headers[] = {"Content-Type", "application/json"};
    char token[TURBO_MEDIA_REVOCATION_HTTPS_TOKEN_BYTES];
    turbo_media_revocation_fanout_transport_result_t result =
        TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;

    if (!adapter || !target || !target_id || !path || !body || !response ||
        strcmp(target_id, target->target_id) != 0) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    memset(response, 0, sizeof(*response));
    memset(token, 0, sizeof(token));
    if (adapter->config.acquire_token(
            adapter->config.token_context, target_id, attempt,
            token, sizeof(token)) != 0 ||
        token[0] == '\0') {
        memset(token, 0, sizeof(token));
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    if (turbo_transport_parse_url(target->base_url, &config) != 0) {
        memset(token, 0, sizeof(token));
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    config.connect_timeout_ms = (int)adapter->config.timeout_ms;
    config.read_timeout_ms = (int)adapter->config.timeout_ms;
    config.write_timeout_ms = (int)adapter->config.timeout_ms;
    config.auth_token = token;
    tls.size = sizeof(tls);
    tls.ca_file = target->ca_file;
    tls.server_name = target->server_name;
    config.tls = &tls;

    transport = turbo_transport_create(&config);
    free_parsed_url(&config);
    memset(token, 0, sizeof(token));
    if (!transport) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE;
    }
    raw = turbo_transport_http_request(
        transport, TURBO_HTTP_POST, path,
        (const uint8_t *)body, body_size, headers, 2);
    result = parse_response(raw, response);
    if (raw) {
        chttp_response_destroy(raw);
        free(raw);
    }
    if (turbo_transport_destroy(transport) != 0 &&
        (result == TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_APPLIED ||
         result == TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_STALE)) {
        result = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE;
    }
    return result;
}

static int digest_hex_valid(const char *value) {
    if (!value || strlen(value) !=
                      TURBO_MEDIA_REVOCATION_WIRE_SHA256_HEX_BYTES) {
        return 0;
    }
    for (size_t index = 0U;
         index < TURBO_MEDIA_REVOCATION_WIRE_SHA256_HEX_BYTES; ++index) {
        unsigned char ch = (unsigned char)value[index];
        if (!((ch >= '0' && ch <= '9') ||
              (ch >= 'a' && ch <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static char *build_snapshot_body(
    uint64_t epoch, uint64_t sequence,
    const char *const *sha256_hex, size_t count,
    size_t *out_size) {
    size_t capacity = TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES + 1U;
    char *body;
    int written;
    size_t offset;

    if (!out_size || count > TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS ||
        (count > 0U && !sha256_hex)) {
        return NULL;
    }
    body = (char *)calloc(capacity, 1U);
    if (!body) {
        return NULL;
    }
    written = snprintf(
        body, capacity,
        "{\"schema_version\":1,\"epoch\":%llu,\"sequence\":%llu,"
        "\"revoked_sha256\":\"",
        (unsigned long long)epoch, (unsigned long long)sequence);
    if (written < 0 || (size_t)written >= capacity) {
        free(body);
        return NULL;
    }
    offset = (size_t)written;
    for (size_t index = 0U; index < count; ++index) {
        size_t length;
        if (!digest_hex_valid(sha256_hex[index])) {
            memset(body, 0, capacity);
            free(body);
            return NULL;
        }
        length = strlen(sha256_hex[index]);
        if (
            offset + length + (index > 0U ? 1U : 0U) + 3U >= capacity) {
            memset(body, 0, capacity);
            free(body);
            return NULL;
        }
        if (index > 0U) {
            body[offset++] = ',';
        }
        memcpy(body + offset, sha256_hex[index], length);
        offset += length;
    }
    memcpy(body + offset, "\"}", 3U);
    offset += 2U;
    body[offset] = '\0';
    *out_size = offset;
    return body;
}

turbo_media_revocation_fanout_transport_result_t
turbo_media_revocation_https_send_snapshot(
    void *transport_context, void *target_context, const char *target_id,
    uint64_t epoch, uint64_t sequence,
    const char *const *sha256_hex, size_t count, unsigned int attempt,
    turbo_media_revocation_fanout_response_t *response) {
    turbo_media_revocation_https_t *adapter =
        (turbo_media_revocation_https_t *)transport_context;
    revocation_https_target_t *target =
        (revocation_https_target_t *)target_context;
    char *body;
    size_t body_size = 0U;
    turbo_media_revocation_fanout_transport_result_t result;

    body = build_snapshot_body(
        epoch, sequence, sha256_hex, count, &body_size);
    if (!body) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    result = post_json(
        adapter, target, target_id,
        "/api/v1/security/revocations/snapshot",
        body, body_size, attempt, response);
    memset(body, 0, TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES + 1U);
    free(body);
    return result;
}

turbo_media_revocation_fanout_transport_result_t
turbo_media_revocation_https_send_revoke(
    void *transport_context, void *target_context, const char *target_id,
    uint64_t epoch, uint64_t sequence, const char *sha256_hex,
    unsigned int attempt,
    turbo_media_revocation_fanout_response_t *response) {
    turbo_media_revocation_https_t *adapter =
        (turbo_media_revocation_https_t *)transport_context;
    revocation_https_target_t *target =
        (revocation_https_target_t *)target_context;
    char body[256];
    int written;

    if (!digest_hex_valid(sha256_hex)) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    written = snprintf(
        body, sizeof(body),
        "{\"schema_version\":1,\"epoch\":%llu,\"sequence\":%llu,"
        "\"sha256\":\"%s\"}",
        (unsigned long long)epoch, (unsigned long long)sequence,
        sha256_hex);
    if (written < 0 || (size_t)written >= sizeof(body)) {
        memset(body, 0, sizeof(body));
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    {
        turbo_media_revocation_fanout_transport_result_t result =
            post_json(
                adapter, target, target_id,
                "/api/v1/security/revocations/revoke",
                body, (size_t)written, attempt, response);
        memset(body, 0, sizeof(body));
        return result;
    }
}
