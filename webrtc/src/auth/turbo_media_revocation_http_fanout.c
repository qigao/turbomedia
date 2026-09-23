#include "turbo_media_revocation_http_fanout.h"

#include "turbo_media_revocation_wire.h"
#include "turbo_transport.h"

#include <chttp/chttp.h>
#include <json_parser.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REVOCATION_SNAPSHOT_PATH "/api/v1/security/revocations/snapshot"
#define REVOCATION_REVOKE_PATH "/api/v1/security/revocations/revoke"

typedef struct http_response_s {
    unsigned int status_code;
    size_t body_size;
    char body[TURBO_MEDIA_REVOCATION_HTTP_RESPONSE_BYTES];
} http_response_t;

typedef struct http_target_state_s {
    turbo_media_revocation_http_target_t view;
    char target_id[TURBO_MEDIA_REVOCATION_FANOUT_TARGET_ID_BYTES];
    char base_url[TURBO_MEDIA_REVOCATION_HTTP_BASE_URL_BYTES];
    char ca_file[TURBO_MEDIA_REVOCATION_HTTP_CA_PATH_BYTES];
    char server_name[TURBO_MEDIA_REVOCATION_HTTP_SERVER_NAME_BYTES];
} http_target_state_t;

struct turbo_media_revocation_http_fanout_s {
    turbo_media_revocation_http_fanout_config_t config;
    turbo_media_revocation_http_dependencies_t dependencies;
    http_target_state_t *targets;
    size_t target_count;
    turbo_media_revocation_fanout_t *fanout;
};

static void secure_zero(void *data, size_t size) {
    volatile unsigned char *bytes = (volatile unsigned char *)data;
    if (!data) {
        return;
    }
    while (size > 0U) {
        *bytes++ = 0U;
        --size;
    }
}

static int copy_bounded(char *destination, size_t capacity,
                        const char *source) {
    size_t length;

    if (!destination || capacity == 0U || !source) {
        return -1;
    }
    length = strlen(source);
    if (length == 0U || length >= capacity) {
        return -1;
    }
    memcpy(destination, source, length + 1U);
    return 0;
}

static int bearer_valid(const char *token, size_t capacity) {
    size_t length;

    if (!token || capacity < 2U) {
        return 0;
    }
    for (length = 0U; length < capacity && token[length] != '\0'; ++length) {
        unsigned char ch = (unsigned char)token[length];
        if (!((ch >= 'A' && ch <= 'Z') ||
              (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.')) {
            return 0;
        }
    }
    return length > 0U && length < capacity;
}

static void free_parsed_transport_config(turbo_transport_config_t *config) {
    if (!config) {
        return;
    }
    free((void *)config->host);
    free((void *)config->path);
    memset(config, 0, sizeof(*config));
}

static int target_origin_valid(
    const turbo_media_revocation_http_target_t *target) {
    turbo_transport_config_t parsed;
    int valid = 0;

    if (!target || !target->target_id || !target->base_url ||
        !target->ca_file || !target->server_name ||
        target->target_id[0] == '\0' || target->base_url[0] == '\0' ||
        target->ca_file[0] == '\0' || target->server_name[0] == '\0' ||
        strlen(target->target_id) >=
            TURBO_MEDIA_REVOCATION_FANOUT_TARGET_ID_BYTES ||
        strlen(target->base_url) >=
            TURBO_MEDIA_REVOCATION_HTTP_BASE_URL_BYTES ||
        strlen(target->ca_file) >=
            TURBO_MEDIA_REVOCATION_HTTP_CA_PATH_BYTES ||
        strlen(target->server_name) >=
            TURBO_MEDIA_REVOCATION_HTTP_SERVER_NAME_BYTES ||
        strchr(target->base_url, '@') ||
        strchr(target->base_url, '?') ||
        strchr(target->base_url, '#')) {
        return 0;
    }
    memset(&parsed, 0, sizeof(parsed));
    if (turbo_transport_parse_url(target->base_url, &parsed) == 0) {
        valid = parsed.type == TURBO_TRANSPORT_HTTP &&
                parsed.use_tls &&
                parsed.path && strcmp(parsed.path, "/") == 0;
    }
    free_parsed_transport_config(&parsed);
    return valid;
}

static int json_uint32_field(
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

static int response_object_exact(const json_value_t *object) {
    static const char *const keys[] = {
        "schema_version", "result", "synchronized",
        "epoch", "sequence", "count"};
    size_t index;
    size_t expected;

    if (!object || json_type(object) != JSON_OBJECT ||
        json_object_size(object) != sizeof(keys) / sizeof(keys[0])) {
        return 0;
    }
    for (index = 0U; index < json_object_size(object); ++index) {
        const char *key = json_object_key(object, index);
        int found = 0;
        if (!key) {
            return 0;
        }
        for (expected = 0U;
             expected < sizeof(keys) / sizeof(keys[0]); ++expected) {
            if (strcmp(key, keys[expected]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return 0;
        }
    }
    return 1;
}

static turbo_media_revocation_fanout_transport_result_t parse_http_response(
    const http_response_t *http,
    turbo_media_revocation_fanout_response_t *response) {
    json_value_t *root = NULL;
    json_value_t *result_value;
    json_value_t *sync_value;
    const char *result;
    uint32_t schema = 0U;
    uint32_t epoch = 0U;
    uint32_t sequence = 0U;
    uint32_t ignored_count = 0U;
    turbo_media_revocation_fanout_transport_result_t mapped =
        TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;

    if (!http || !response) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    if (http->status_code == 401U || http->status_code == 403U ||
        http->status_code == 404U) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    if (http->status_code == 408U || http->status_code == 425U ||
        http->status_code == 429U ||
        (http->status_code >= 500U && http->status_code <= 599U)) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE;
    }
    if (http->body_size == 0U ||
        http->body_size >= TURBO_MEDIA_REVOCATION_HTTP_RESPONSE_BYTES ||
        http->body[http->body_size] != '\0') {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }

    root = json_parse(http->body, http->body_size);
    if (!root || !response_object_exact(root) ||
        json_uint32_field(root, "schema_version", &schema) != 0 ||
        schema != TURBO_MEDIA_REVOCATION_WIRE_SCHEMA_VERSION ||
        json_uint32_field(root, "epoch", &epoch) != 0 ||
        json_uint32_field(root, "sequence", &sequence) != 0 ||
        json_uint32_field(root, "count", &ignored_count) != 0) {
        json_free(root);
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    result_value = json_object_get(root, "result");
    sync_value = json_object_get(root, "synchronized");
    if (!result_value || json_type(result_value) != JSON_STRING ||
        !sync_value || json_type(sync_value) != JSON_BOOL) {
        json_free(root);
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    result = json_string(result_value);
    response->synchronized = json_bool(sync_value) ? 1 : 0;
    response->epoch = epoch;
    response->sequence = sequence;

    if (http->status_code == 200U && strcmp(result, "applied") == 0) {
        mapped = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_APPLIED;
    } else if (http->status_code == 200U &&
               strcmp(result, "stale") == 0) {
        mapped = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_STALE;
    } else if (http->status_code == 409U &&
               strcmp(result, "gap") == 0) {
        mapped = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_GAP;
    } else if (http->status_code == 409U &&
               strcmp(result, "limit") == 0) {
        mapped = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_LIMIT;
    } else if (http->status_code == 400U &&
               strcmp(result, "error") == 0) {
        mapped = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_ERROR;
    }
    json_free(root);
    return mapped;
}

static turbo_media_revocation_fanout_transport_result_t
real_https_request(
    void *context,
    const turbo_media_revocation_http_target_t *target,
    const char *path,
    const char *bearer_token,
    const char *body,
    size_t body_size,
    uint32_t connect_timeout_ms,
    uint32_t read_timeout_ms,
    uint32_t write_timeout_ms,
    turbo_media_revocation_fanout_response_t *response) {
    turbo_transport_config_t parsed;
    cnet_tls_client_config tls;
    http_response_t http;
    turbo_transport_t *transport = NULL;
    chttp_response *raw = NULL;
    const char *headers[] = {"Content-Type", "application/json"};
    turbo_media_revocation_fanout_transport_result_t result =
        TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE;
    (void)context;

    if (!target || !path || !bearer_token || !body || !response) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    memset(&parsed, 0, sizeof(parsed));
    memset(&tls, 0, sizeof(tls));
    memset(&http, 0, sizeof(http));
    memset(response, 0, sizeof(*response));
    if (turbo_transport_parse_url(target->base_url, &parsed) != 0 ||
        parsed.type != TURBO_TRANSPORT_HTTP || !parsed.use_tls ||
        !parsed.path || strcmp(parsed.path, "/") != 0) {
        free_parsed_transport_config(&parsed);
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }

    tls.size = sizeof(tls);
    tls.ca_file = target->ca_file;
    tls.server_name = target->server_name;
    parsed.connect_timeout_ms = (int)connect_timeout_ms;
    parsed.read_timeout_ms = (int)read_timeout_ms;
    parsed.write_timeout_ms = (int)write_timeout_ms;
    parsed.tls = &tls;
    parsed.auth_token = bearer_token;
    transport = turbo_transport_create(&parsed);
    free_parsed_transport_config(&parsed);
    if (!transport) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE;
    }

    raw = turbo_transport_http_request(
        transport, TURBO_HTTP_POST, path,
        (const uint8_t *)body, body_size, headers, 2);
    if (raw) {
        http.status_code = raw->status_code;
        if (raw->body_size < sizeof(http.body)) {
            http.body_size = raw->body_size;
            if (raw->body_size > 0U && raw->body) {
                memcpy(http.body, raw->body, raw->body_size);
            }
            http.body[http.body_size] = '\0';
            result = parse_http_response(&http, response);
        } else {
            result = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
        }
        chttp_response_destroy(raw);
        free(raw);
    }
    if (turbo_transport_destroy(transport) != 0 &&
        result != TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL) {
        result = TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_RETRYABLE;
    }
    secure_zero(&http, sizeof(http));
    return result;
}

static int append_text(char *buffer, size_t capacity, size_t *offset,
                       const char *text) {
    size_t length;
    if (!buffer || !offset || !text) {
        return -1;
    }
    length = strlen(text);
    if (*offset > capacity || length >= capacity - *offset) {
        return -1;
    }
    memcpy(buffer + *offset, text, length);
    *offset += length;
    buffer[*offset] = '\0';
    return 0;
}

static char *format_snapshot_body(
    uint64_t epoch, uint64_t sequence,
    const char *const *sha256_hex, size_t count, size_t *out_size) {
    char *body;
    size_t offset = 0U;
    int written;

    if (!out_size || epoch == 0U || epoch > UINT32_MAX ||
        sequence > UINT32_MAX ||
        count > TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS ||
        (count > 0U && !sha256_hex)) {
        return NULL;
    }
    body = (char *)calloc(
        TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES + 1U, 1U);
    if (!body) {
        return NULL;
    }
    written = snprintf(
        body, TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES + 1U,
        "{\"schema_version\":1,\"epoch\":%llu,\"sequence\":%llu,"
        "\"revoked_sha256\":\"",
        (unsigned long long)epoch, (unsigned long long)sequence);
    if (written < 0 ||
        (size_t)written > TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES) {
        free(body);
        return NULL;
    }
    offset = (size_t)written;
    for (size_t index = 0U; index < count; ++index) {
        if ((index > 0U && append_text(
                              body,
                              TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES + 1U,
                              &offset, ",") != 0) ||
            append_text(
                body, TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES + 1U,
                &offset, sha256_hex[index]) != 0) {
            free(body);
            return NULL;
        }
    }
    if (append_text(
            body, TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES + 1U,
            &offset, "\"}") != 0 ||
        offset > TURBO_MEDIA_REVOCATION_WIRE_MAX_BODY_BYTES) {
        free(body);
        return NULL;
    }
    *out_size = offset;
    return body;
}

static int format_revoke_body(
    uint64_t epoch, uint64_t sequence, const char *sha256_hex,
    char *body, size_t capacity, size_t *out_size) {
    int written;

    if (!sha256_hex || !body || !out_size ||
        epoch == 0U || epoch > UINT32_MAX ||
        sequence == 0U || sequence > UINT32_MAX ||
        strlen(sha256_hex) !=
            TURBO_MEDIA_REVOCATION_WIRE_SHA256_HEX_BYTES) {
        return -1;
    }
    written = snprintf(
        body, capacity,
        "{\"schema_version\":1,\"epoch\":%llu,\"sequence\":%llu,"
        "\"sha256\":\"%s\"}",
        (unsigned long long)epoch, (unsigned long long)sequence,
        sha256_hex);
    if (written < 0 || (size_t)written >= capacity) {
        return -1;
    }
    *out_size = (size_t)written;
    return 0;
}

static turbo_media_revocation_fanout_transport_result_t send_request(
    turbo_media_revocation_http_fanout_t *fanout,
    http_target_state_t *target,
    const char *path,
    const char *body,
    size_t body_size,
    turbo_media_revocation_fanout_response_t *response) {
    char bearer[TURBO_MEDIA_REVOCATION_HTTP_BEARER_BYTES];
    turbo_media_revocation_fanout_transport_result_t result =
        TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;

    if (!fanout || !target || !path || !body || !response) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    memset(bearer, 0, sizeof(bearer));
    if (fanout->config.token_provider(
            fanout->config.token_context, target->target_id,
            bearer, sizeof(bearer)) != 0 ||
        !bearer_valid(bearer, sizeof(bearer))) {
        secure_zero(bearer, sizeof(bearer));
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    result = fanout->dependencies.request(
        fanout->dependencies.context, &target->view, path, bearer,
        body, body_size, fanout->config.connect_timeout_ms,
        fanout->config.read_timeout_ms, fanout->config.write_timeout_ms,
        response);
    secure_zero(bearer, sizeof(bearer));
    return result;
}

static turbo_media_revocation_fanout_transport_result_t send_snapshot(
    void *transport_context, void *target_context, const char *target_id,
    uint64_t epoch, uint64_t sequence,
    const char *const *sha256_hex, size_t count, unsigned int attempt,
    turbo_media_revocation_fanout_response_t *response) {
    turbo_media_revocation_http_fanout_t *fanout =
        (turbo_media_revocation_http_fanout_t *)transport_context;
    http_target_state_t *target = (http_target_state_t *)target_context;
    char *body;
    size_t body_size = 0U;
    turbo_media_revocation_fanout_transport_result_t result;
    (void)target_id;
    (void)attempt;

    body = format_snapshot_body(
        epoch, sequence, sha256_hex, count, &body_size);
    if (!body) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    result = send_request(
        fanout, target, REVOCATION_SNAPSHOT_PATH,
        body, body_size, response);
    free(body);
    return result;
}

static turbo_media_revocation_fanout_transport_result_t send_revoke(
    void *transport_context, void *target_context, const char *target_id,
    uint64_t epoch, uint64_t sequence, const char *sha256_hex,
    unsigned int attempt,
    turbo_media_revocation_fanout_response_t *response) {
    turbo_media_revocation_http_fanout_t *fanout =
        (turbo_media_revocation_http_fanout_t *)transport_context;
    http_target_state_t *target = (http_target_state_t *)target_context;
    char body[256];
    size_t body_size = 0U;
    (void)target_id;
    (void)attempt;

    memset(body, 0, sizeof(body));
    if (format_revoke_body(
            epoch, sequence, sha256_hex,
            body, sizeof(body), &body_size) != 0) {
        return TURBO_MEDIA_REVOCATION_FANOUT_TRANSPORT_FATAL;
    }
    return send_request(
        fanout, target, REVOCATION_REVOKE_PATH,
        body, body_size, response);
}

static turbo_media_revocation_http_fanout_t *create_impl(
    const turbo_media_revocation_http_fanout_config_t *config,
    const turbo_media_revocation_http_target_t *targets,
    size_t target_count,
    const turbo_media_revocation_http_dependencies_t *dependencies) {
    turbo_media_revocation_http_fanout_t *fanout;
    turbo_media_revocation_fanout_target_t fanout_targets[
        TURBO_MEDIA_REVOCATION_FANOUT_MAX_TARGETS];
    turbo_media_revocation_fanout_config_t fanout_config;

    if (!config || !targets || target_count == 0U ||
        target_count > TURBO_MEDIA_REVOCATION_FANOUT_MAX_TARGETS ||
        !config->token_provider || !dependencies ||
        !dependencies->request ||
        config->max_attempts == 0U ||
        config->max_attempts > TURBO_MEDIA_REVOCATION_FANOUT_MAX_ATTEMPTS ||
        config->connect_timeout_ms == 0U ||
        config->read_timeout_ms == 0U ||
        config->write_timeout_ms == 0U ||
        config->connect_timeout_ms > INT32_MAX ||
        config->read_timeout_ms > INT32_MAX ||
        config->write_timeout_ms > INT32_MAX) {
        return NULL;
    }
    fanout = (turbo_media_revocation_http_fanout_t *)calloc(
        1U, sizeof(*fanout));
    if (!fanout) {
        return NULL;
    }
    fanout->targets = (http_target_state_t *)calloc(
        target_count, sizeof(*fanout->targets));
    if (!fanout->targets) {
        free(fanout);
        return NULL;
    }
    fanout->config = *config;
    fanout->dependencies = *dependencies;
    fanout->target_count = target_count;

    for (size_t index = 0U; index < target_count; ++index) {
        http_target_state_t *target = &fanout->targets[index];
        if (!target_origin_valid(&targets[index]) ||
            copy_bounded(
                target->target_id, sizeof(target->target_id),
                targets[index].target_id) != 0 ||
            copy_bounded(
                target->base_url, sizeof(target->base_url),
                targets[index].base_url) != 0 ||
            copy_bounded(
                target->ca_file, sizeof(target->ca_file),
                targets[index].ca_file) != 0 ||
            copy_bounded(
                target->server_name, sizeof(target->server_name),
                targets[index].server_name) != 0) {
            turbo_media_revocation_http_fanout_destroy(fanout);
            return NULL;
        }
        target->view.target_id = target->target_id;
        target->view.base_url = target->base_url;
        target->view.ca_file = target->ca_file;
        target->view.server_name = target->server_name;
        fanout_targets[index].target_id = target->target_id;
        fanout_targets[index].target_context = target;
    }

    memset(&fanout_config, 0, sizeof(fanout_config));
    fanout_config.max_attempts = config->max_attempts;
    fanout_config.send_snapshot = send_snapshot;
    fanout_config.send_revoke = send_revoke;
    fanout_config.transport_context = fanout;
    fanout->fanout = turbo_media_revocation_fanout_create(
        &fanout_config, fanout_targets, target_count);
    if (!fanout->fanout) {
        turbo_media_revocation_http_fanout_destroy(fanout);
        return NULL;
    }
    return fanout;
}

turbo_media_revocation_http_fanout_t *
turbo_media_revocation_http_fanout_create(
    const turbo_media_revocation_http_fanout_config_t *config,
    const turbo_media_revocation_http_target_t *targets,
    size_t target_count) {
    const turbo_media_revocation_http_dependencies_t dependencies = {
        real_https_request, NULL};
    return create_impl(config, targets, target_count, &dependencies);
}

turbo_media_revocation_http_fanout_t *
turbo_media_revocation_http_fanout_create_with_dependencies(
    const turbo_media_revocation_http_fanout_config_t *config,
    const turbo_media_revocation_http_target_t *targets,
    size_t target_count,
    const turbo_media_revocation_http_dependencies_t *dependencies) {
    return create_impl(config, targets, target_count, dependencies);
}

void turbo_media_revocation_http_fanout_destroy(
    turbo_media_revocation_http_fanout_t *fanout) {
    if (!fanout) {
        return;
    }
    turbo_media_revocation_fanout_destroy(fanout->fanout);
    fanout->fanout = NULL;
    if (fanout->targets) {
        secure_zero(
            fanout->targets,
            fanout->target_count * sizeof(*fanout->targets));
    }
    free(fanout->targets);
    secure_zero(&fanout->config, sizeof(fanout->config));
    secure_zero(&fanout->dependencies, sizeof(fanout->dependencies));
    free(fanout);
}

int turbo_media_revocation_http_fanout_publish_snapshot(
    turbo_media_revocation_http_fanout_t *fanout,
    uint64_t epoch, uint64_t sequence,
    const char *const *sha256_hex, size_t count,
    turbo_media_revocation_fanout_report_t *report) {
    return fanout && fanout->fanout
               ? turbo_media_revocation_fanout_publish_snapshot(
                     fanout->fanout, epoch, sequence,
                     sha256_hex, count, report)
               : -1;
}

int turbo_media_revocation_http_fanout_publish_revoke(
    turbo_media_revocation_http_fanout_t *fanout,
    uint64_t epoch, uint64_t sequence,
    const char *sha256_hex,
    const char *const *covering_sha256_hex,
    size_t covering_count,
    turbo_media_revocation_fanout_report_t *report) {
    return fanout && fanout->fanout
               ? turbo_media_revocation_fanout_publish_revoke(
                     fanout->fanout, epoch, sequence, sha256_hex,
                     covering_sha256_hex, covering_count, report)
               : -1;
}

size_t turbo_media_revocation_http_fanout_target_count(
    const turbo_media_revocation_http_fanout_t *fanout) {
    return fanout && fanout->fanout
               ? turbo_media_revocation_fanout_target_count(fanout->fanout)
               : 0U;
}

int turbo_media_revocation_http_fanout_get_target_status(
    const turbo_media_revocation_http_fanout_t *fanout,
    size_t index,
    turbo_media_revocation_fanout_target_status_t *status) {
    return fanout && fanout->fanout
               ? turbo_media_revocation_fanout_get_target_status(
                     fanout->fanout, index, status)
               : -1;
}
