/**
 * @file http_api.c
 * @brief HTTP Management API Implementation
 */

#include "http_api.h"
#include "signaling_management_internal.h"
#include "turbo_media_auth.h"
#include "webrtc_signaling.h"
#include <chttp/chttp.h>
#include <json_parser.h>
#include <salts/error_codes.h>
#include <salts_thread.h>
#include <tlog.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "salts_str.h"

#define SIGNALING_MANAGEMENT_AUDIENCE "turbomedia-signaling-management"
#define SIGNALING_MANAGEMENT_SCOPE_READ "signaling.management.read"
#define SIGNALING_MANAGEMENT_SCOPE_WRITE "signaling.management.write"
#define SIGNALING_MANAGEMENT_SCOPE_DANGEROUS "signaling.management.dangerous"
#define SIGNALING_SECURITY_CONTROL_AUDIENCE "turbomedia-security-control"
#define SIGNALING_SECURITY_REVOCATION_SCOPE "security.revocation.write"

enum {
    HTTP_API_CONNECTION_CAPACITY = 64,
    HTTP_API_COMMAND_CAPACITY = 128,
    HTTP_API_REQUEST_CAPACITY = 128,
    HTTP_API_COMPLETION_CAPACITY = 32,
    HTTP_API_EVENT_CAPACITY = 128,
    HTTP_API_ROUTE_CAPACITY = 16,
    HTTP_API_MAX_ROUTE_PARAM_COUNT = 2,
    HTTP_API_MAX_ROUTE_PARAM_BYTES = 1024,
    HTTP_API_MAX_TARGET_BYTES = 4096,
    HTTP_API_MAX_HEADER_COUNT = 64,
    HTTP_API_MAX_HEADER_BYTES = 32 * 1024,
    HTTP_API_MAX_REQUEST_BODY_BYTES = 4 * 1024 * 1024,
    HTTP_API_MAX_RESPONSE_BODY_BYTES = 4 * 1024 * 1024,
    HTTP_API_MAX_SEND_BYTES = HTTP_API_MAX_RESPONSE_BODY_BYTES + 64 * 1024,
    HTTP_API_RECEIVE_BUFFER_BYTES = 32 * 1024,
    HTTP_API_TLS_IO_BUFFER_BYTES = 256 * 1024,
    HTTP_API_BUFFER_CAPACITY_BYTES = 32 * 1024 * 1024,
    HTTP_API_TIMEOUT_MS = 5000,
    HTTP_API_POLL_SLICE_MS = 10,
    HTTP_API_REVOCATION_BODY_BYTES = 32 * 1024,
    HTTP_API_REVOCATION_DIGEST_BYTES = 64,
    HTTP_API_REVOCATION_MAX_CSV_BYTES =
        TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS *
            HTTP_API_REVOCATION_DIGEST_BYTES +
        TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS - 1
};

typedef enum http_api_state_e {
    HTTP_API_STOPPED = 0,
    HTTP_API_STARTING,
    HTTP_API_RUNNING,
    HTTP_API_STOPPING
} http_api_state_t;

struct http_api_server_s {
    http_api_config_t config;
    webrtc_signaling_server_t *signaling;
    salts_mutex_t lifecycle_mutex;
    chttp_server http;
    int http_initialized;
    int revocation_enabled;
    http_api_state_t state;
};

typedef struct http_api_response_s {
    http_api_server_t *server;
    chttp_server_response *response;
    int status;
} http_api_response_t;

static turbo_media_auth_config_t signed_auth_config(
    const http_api_config_t *config) {
    turbo_media_auth_config_t auth = {0};

    if (!config) {
        return auth;
    }
    auth.issuer = config->auth_issuer;
    auth.active_key_id = config->auth_active_key_id;
    auth.active_secret = config->auth_active_secret;
    auth.previous_key_id = config->auth_previous_key_id;
    auth.previous_secret = config->auth_previous_secret;
    auth.revoked_token_sha256 = config->auth_revoked_token_sha256;
    auth.clock_skew_seconds = config->auth_clock_skew_seconds;
    auth.max_ttl_seconds = config->auth_max_ttl_seconds;
    return auth;
}

static int http_api_add_cors_headers(http_api_response_t *response) {
    int status;

    if (!response || !response->server || !response->response) {
        return SALTS_EINVAL;
    }
    if (response->server->config.auth_enabled) {
        return SALTS_OK;
    }
    status = chttp_server_response_set_header(
        response->response, "Access-Control-Allow-Origin", "*");
    if (status == SALTS_OK) {
        status = chttp_server_response_set_header(
            response->response, "Access-Control-Allow-Methods",
            "GET, POST, DELETE, OPTIONS");
    }
    if (status == SALTS_OK) {
        status = chttp_server_response_set_header(
            response->response, "Access-Control-Allow-Headers",
            "Content-Type, Authorization");
    }
    return status;
}

static void set_header(http_api_response_t *response, const char *name,
                       const char *value) {
    if (!response || response->status != SALTS_OK) {
        return;
    }
    response->status = chttp_server_response_set_header(
        response->response, name, value);
}

static void reply(http_api_response_t *response, unsigned int status,
                  const char *content_type, const void *body,
                  size_t body_size) {
    if (!response || response->status != SALTS_OK) {
        return;
    }
    response->status = http_api_add_cors_headers(response);
    if (response->status == SALTS_OK) {
        response->status = chttp_server_reply(
            response->response, status, content_type, body, body_size);
    }
}

static void send_json(http_api_response_t *response, unsigned int status,
                      const char *body) {
    reply(response, status, "application/json", body,
          body ? strlen(body) : 0u);
}

static void send_text(http_api_response_t *response, unsigned int status,
                      const char *body) {
    reply(response, status, "text/plain", body,
          body ? strlen(body) : 0u);
}

static int authorize_security_control_request(
    http_api_server_t *server, const chttp_server_request_view *request,
    http_api_response_t *response) {
    const char *authorization;
    turbo_media_auth_config_t auth;
    turbo_media_auth_policy_t policy;

    if (!server || !request || !response || !server->revocation_enabled ||
        !server->config.use_tls || !server->config.auth_enabled ||
        !server->config.auth_active_secret ||
        server->config.auth_active_secret[0] == '\0') {
        send_text(response, 503, "Security control unavailable");
        return 0;
    }

    authorization = chttp_server_request_header(request, "Authorization");
    auth = signed_auth_config(&server->config);
    memset(&policy, 0, sizeof(policy));
    policy.audience = SIGNALING_SECURITY_CONTROL_AUDIENCE;
    policy.required_scope = SIGNALING_SECURITY_REVOCATION_SCOPE;
    if (turbo_media_auth_authorize(
            authorization, NULL, &auth, &policy) !=
        TURBO_MEDIA_AUTH_SIGNED_TOKEN) {
        set_header(response, "WWW-Authenticate", "Bearer");
        send_text(response, 401, "Unauthorized");
        return 0;
    }
    return 1;
}

static int authorize_management_request(
    http_api_server_t *server, const chttp_server_request_view *request,
    http_api_response_t *response, const char *required_scope,
    const char *room_id, const char *participant_id) {
    const char *authorization;
    turbo_media_auth_config_t auth;
    turbo_media_auth_policy_t policy;

    if (!server || !request || !response) {
        send_text(response, 500, "Server not initialized");
        return 0;
    }
    if (!server->config.auth_enabled) {
        return 1;
    }

    authorization = chttp_server_request_header(request, "Authorization");
    auth = signed_auth_config(&server->config);
    memset(&policy, 0, sizeof(policy));
    policy.audience = SIGNALING_MANAGEMENT_AUDIENCE;
    policy.required_scope = required_scope;
    policy.room_id = room_id;
    policy.participant_id = participant_id;
    if (turbo_media_auth_authorize(
            authorization, server->config.admin_token, &auth, &policy) ==
        TURBO_MEDIA_AUTH_DENIED) {
        set_header(response, "WWW-Authenticate", "Bearer");
        send_text(response, 401, "Unauthorized");
        return 0;
    }
    return 1;
}

static int handle_health(void *user,
                         const chttp_server_request_view *request,
                         chttp_server_response *raw_response) {
    http_api_response_t response = {
        (http_api_server_t *)user, raw_response, SALTS_OK};
    (void)request;
    send_text(&response, 200, "OK");
    return response.status;
}

static int handle_status(void *user,
                         const chttp_server_request_view *request,
                         chttp_server_response *raw_response) {
    http_api_server_t *server = (http_api_server_t *)user;
    http_api_response_t response = {server, raw_response, SALTS_OK};
    char *json;

    if (!authorize_management_request(
            server, request, &response, SIGNALING_MANAGEMENT_SCOPE_READ,
            NULL, NULL)) {
        return response.status;
    }
    json = webrtc_signaling_get_status_json(server->signaling);
    if (!json) {
        send_text(&response, 500, "Internal Error");
        return response.status;
    }
    send_json(&response, 200, json);
    free(json);
    return response.status;
}

static int handle_get_rooms(void *user,
                            const chttp_server_request_view *request,
                            chttp_server_response *raw_response) {
    http_api_server_t *server = (http_api_server_t *)user;
    http_api_response_t response = {server, raw_response, SALTS_OK};
    char *json;

    if (!authorize_management_request(
            server, request, &response, SIGNALING_MANAGEMENT_SCOPE_READ,
            NULL, NULL)) {
        return response.status;
    }
    json = webrtc_signaling_get_rooms_json(server->signaling);
    if (!json) {
        send_text(&response, 500, "Internal Error");
        return response.status;
    }
    send_json(&response, 200, json);
    free(json);
    return response.status;
}

static int handle_get_room_peers(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *raw_response) {
    http_api_server_t *server = (http_api_server_t *)user;
    http_api_response_t response = {server, raw_response, SALTS_OK};
    const char *room_id = chttp_server_request_param(request, "id");
    char *json;

    if (!authorize_management_request(
            server, request, &response, SIGNALING_MANAGEMENT_SCOPE_READ,
            room_id, NULL)) {
        return response.status;
    }
    if (!room_id) {
        send_text(&response, 400, "Missing room id");
        return response.status;
    }
    json = webrtc_signaling_get_room_peers_json(server->signaling, room_id);
    if (!json) {
        send_text(&response, 404, "Room not found");
        return response.status;
    }
    send_json(&response, 200, json);
    free(json);
    return response.status;
}

static int handle_kick_peer(void *user,
                            const chttp_server_request_view *request,
                            chttp_server_response *raw_response) {
    http_api_server_t *server = (http_api_server_t *)user;
    http_api_response_t response = {server, raw_response, SALTS_OK};
    const char *room_id = chttp_server_request_param(request, "id");
    const char *peer_id = chttp_server_request_param(request, "peer_id");
    int result;

    if (!authorize_management_request(
            server, request, &response,
            SIGNALING_MANAGEMENT_SCOPE_DANGEROUS, room_id, peer_id)) {
        return response.status;
    }
    if (!room_id || !peer_id) {
        send_text(&response, 400, "Missing params");
        return response.status;
    }
    result = webrtc_signaling_kick_peer(
        server->signaling, room_id, peer_id, "Kicked by admin");
    if (result == 0) {
        send_json(&response, 200, "{\"success\":true}");
    } else {
        send_text(&response, 404, "Peer mismatch or not found");
    }
    return response.status;
}

static int handle_broadcast(void *user,
                            const chttp_server_request_view *request,
                            chttp_server_response *raw_response) {
    http_api_server_t *server = (http_api_server_t *)user;
    http_api_response_t response = {server, raw_response, SALTS_OK};
    const char *room_id = chttp_server_request_param(request, "id");
    char result_json[64];
    tstr message;
    int sent;
    int result_size;

    if (!authorize_management_request(
            server, request, &response, SIGNALING_MANAGEMENT_SCOPE_WRITE,
            room_id, NULL)) {
        return response.status;
    }
    if (!room_id || !request->body || request->body_size == 0u) {
        send_text(&response, 400, "Missing param body");
        return response.status;
    }
    message = tstr_dup_len((const char *)request->body, request->body_size);
    if (!message) {
        send_text(&response, 500, "Allocation failed");
        return response.status;
    }
    sent = webrtc_signaling_broadcast(
        server->signaling, room_id, "admin", message);
    tstr_free(message);
    result_size = snprintf(result_json, sizeof(result_json),
                           "{\"sent\":%d}", sent);
    if (result_size < 0 || (size_t)result_size >= sizeof(result_json)) {
        send_text(&response, 500, "Internal Error");
        return response.status;
    }
    reply(&response, 200, "application/json", result_json,
          (size_t)result_size);
    return response.status;
}

static int json_object_has_exact_keys(
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

static int json_uint32_required(
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

static int json_schema_v1(const json_value_t *object) {
    uint32_t version = 0U;
    return json_uint32_required(object, "schema_version", &version) == 0 &&
           version == 1U;
}

static void send_revocation_result(
    http_api_response_t *response,
    webrtc_signaling_server_t *signaling,
    webrtc_signaling_revocation_apply_result_t result) {
    int synchronized = 0;
    uint64_t epoch = 0U;
    uint64_t sequence = 0U;
    size_t count = 0U;
    const char *name = "error";
    unsigned int status = 400U;
    char body[256];
    int length;

    if (result == WEBRTC_SIGNALING_REVOCATION_APPLY_APPLIED) {
        name = "applied";
        status = 200U;
    } else if (result == WEBRTC_SIGNALING_REVOCATION_APPLY_STALE) {
        name = "stale";
        status = 200U;
    } else if (result == WEBRTC_SIGNALING_REVOCATION_APPLY_GAP) {
        name = "gap";
        status = 409U;
    } else if (result == WEBRTC_SIGNALING_REVOCATION_APPLY_LIMIT) {
        name = "limit";
        status = 409U;
    }
    if (webrtc_signaling_get_revocation_status(
            signaling, &synchronized, &epoch, &sequence, &count) != 0) {
        send_text(response, 500, "Revocation status unavailable");
        return;
    }
    length = snprintf(
        body, sizeof(body),
        "{\"schema_version\":1,\"result\":\"%s\","
        "\"synchronized\":%s,\"epoch\":%llu,\"sequence\":%llu,"
        "\"count\":%zu}",
        name, synchronized ? "true" : "false",
        (unsigned long long)epoch, (unsigned long long)sequence, count);
    if (length < 0 || (size_t)length >= sizeof(body)) {
        send_text(response, 500, "Internal Error");
        return;
    }
    reply(response, status, "application/json", body, (size_t)length);
}

static int handle_revocation_snapshot(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *raw_response) {
    static const char *const keys[] = {
        "schema_version", "epoch", "sequence", "revoked_sha256"};
    http_api_server_t *server = (http_api_server_t *)user;
    http_api_response_t response = {server, raw_response, SALTS_OK};
    json_value_t *root = NULL;
    json_value_t *list_value;
    const char *csv;
    size_t csv_size;
    char *owned = NULL;
    const char *digests[TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS];
    size_t count = 0U;
    uint32_t epoch = 0U;
    uint32_t sequence = 0U;
    webrtc_signaling_revocation_apply_result_t result;

    if (!authorize_security_control_request(
            server, request, &response)) {
        return response.status;
    }
    if (!request->body || request->body_size == 0U ||
        request->body_size > HTTP_API_REVOCATION_BODY_BYTES) {
        send_text(&response, 400, "Invalid revocation snapshot");
        return response.status;
    }
    root = json_parse((const char *)request->body, request->body_size);
    if (!root || !json_object_has_exact_keys(
                     root, keys, sizeof(keys) / sizeof(keys[0])) ||
        !json_schema_v1(root) ||
        json_uint32_required(root, "epoch", &epoch) != 0 || epoch == 0U ||
        json_uint32_required(root, "sequence", &sequence) != 0) {
        json_free(root);
        send_text(&response, 400, "Invalid revocation snapshot");
        return response.status;
    }
    list_value = json_object_get(root, "revoked_sha256");
    if (!list_value || json_type(list_value) != JSON_STRING) {
        json_free(root);
        send_text(&response, 400, "Invalid revocation snapshot");
        return response.status;
    }
    csv = json_string(list_value);
    csv_size = json_string_len(list_value);
    if (!csv || csv_size > HTTP_API_REVOCATION_MAX_CSV_BYTES) {
        json_free(root);
        send_text(&response, 400, "Invalid revocation snapshot");
        return response.status;
    }
    if (csv_size > 0U) {
        char *cursor;
        owned = (char *)malloc(csv_size + 1U);
        if (!owned) {
            json_free(root);
            send_text(&response, 500, "Allocation failed");
            return response.status;
        }
        memcpy(owned, csv, csv_size);
        owned[csv_size] = '\0';
        cursor = owned;
        while (cursor && *cursor != '\0') {
            char *comma;
            if (count >= TURBO_MEDIA_AUTH_MAX_REVOKED_TOKENS) {
                free(owned);
                json_free(root);
                send_text(&response, 400, "Invalid revocation snapshot");
                return response.status;
            }
            digests[count++] = cursor;
            comma = strchr(cursor, ',');
            if (!comma) {
                break;
            }
            *comma = '\0';
            cursor = comma + 1;
            if (*cursor == '\0') {
                free(owned);
                json_free(root);
                send_text(&response, 400, "Invalid revocation snapshot");
                return response.status;
            }
        }
    }

    result = webrtc_signaling_apply_revocation_snapshot(
        server->signaling, epoch, sequence,
        count > 0U ? digests : NULL, count);
    free(owned);
    json_free(root);
    send_revocation_result(&response, server->signaling, result);
    return response.status;
}

static int handle_revocation_revoke(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *raw_response) {
    static const char *const keys[] = {
        "schema_version", "epoch", "sequence", "sha256"};
    http_api_server_t *server = (http_api_server_t *)user;
    http_api_response_t response = {server, raw_response, SALTS_OK};
    json_value_t *root = NULL;
    json_value_t *digest_value;
    const char *digest;
    uint32_t epoch = 0U;
    uint32_t sequence = 0U;
    webrtc_signaling_revocation_apply_result_t result;

    if (!authorize_security_control_request(
            server, request, &response)) {
        return response.status;
    }
    if (!request->body || request->body_size == 0U ||
        request->body_size > HTTP_API_REVOCATION_BODY_BYTES) {
        send_text(&response, 400, "Invalid revocation event");
        return response.status;
    }
    root = json_parse((const char *)request->body, request->body_size);
    if (!root || !json_object_has_exact_keys(
                     root, keys, sizeof(keys) / sizeof(keys[0])) ||
        !json_schema_v1(root) ||
        json_uint32_required(root, "epoch", &epoch) != 0 || epoch == 0U ||
        json_uint32_required(root, "sequence", &sequence) != 0 ||
        sequence == 0U) {
        json_free(root);
        send_text(&response, 400, "Invalid revocation event");
        return response.status;
    }
    digest_value = json_object_get(root, "sha256");
    if (!digest_value || json_type(digest_value) != JSON_STRING ||
        json_string_len(digest_value) != HTTP_API_REVOCATION_DIGEST_BYTES) {
        json_free(root);
        send_text(&response, 400, "Invalid revocation event");
        return response.status;
    }
    digest = json_string(digest_value);
    result = webrtc_signaling_apply_revocation(
        server->signaling, epoch, sequence, digest);
    json_free(root);
    send_revocation_result(&response, server->signaling, result);
    return response.status;
}

static int handle_options(void *user,
                          const chttp_server_request_view *request,
                          chttp_server_response *raw_response) {
    http_api_response_t response = {
        (http_api_server_t *)user, raw_response, SALTS_OK};
    (void)request;
    reply(&response, 204, NULL, NULL, 0u);
    return response.status;
}

static int http_api_auth_config_valid(const http_api_config_t *config) {
    turbo_media_auth_config_t auth;
    int has_static;
    int has_signed;

    if (!config || !config->auth_enabled) {
        return config != NULL;
    }
    has_static = config->admin_token && config->admin_token[0] != '\0';
    has_signed = config->auth_active_secret &&
                 config->auth_active_secret[0] != '\0';
    if (!has_static && !has_signed) {
        return 0;
    }
    if (!has_signed) {
        return 1;
    }
    auth = signed_auth_config(config);
    return turbo_media_auth_config_validate(&auth) == 0;
}

static void http_api_free_config_strings(http_api_config_t *config) {
    if (!config) {
        return;
    }
    tstr_free((tstr)config->auth_revoked_token_sha256);
    tstr_free((tstr)config->auth_previous_secret);
    tstr_free((tstr)config->auth_previous_key_id);
    tstr_free((tstr)config->auth_active_secret);
    tstr_free((tstr)config->auth_active_key_id);
    tstr_free((tstr)config->auth_issuer);
    tstr_free((tstr)config->admin_token);
    tstr_free((tstr)config->cert_file);
    tstr_free((tstr)config->key_file);
    tstr_free((tstr)config->host);
    memset(config, 0, sizeof(*config));
}

static int http_api_copy_optional_string(const char *source,
                                         const char **destination) {
    if (!destination) {
        return -1;
    }
    *destination = source ? tstr_dup(source) : NULL;
    return source && !*destination ? -1 : 0;
}

static native_io_backend_kind http_api_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config http_api_network_config(int use_tls) {
    const cnet_client_config config = {
        .backend = http_api_backend(),
        .connection_capacity = HTTP_API_CONNECTION_CAPACITY,
        .command_capacity = HTTP_API_COMMAND_CAPACITY,
        .request_capacity = HTTP_API_REQUEST_CAPACITY,
        .completion_batch_capacity = HTTP_API_COMPLETION_CAPACITY,
        .event_capacity = HTTP_API_EVENT_CAPACITY,
        .max_send_bytes = HTTP_API_MAX_SEND_BYTES,
        .receive_buffer_bytes = HTTP_API_RECEIVE_BUFFER_BYTES,
        .connect_timeout_ms = HTTP_API_TIMEOUT_MS,
        .read_timeout_ms = HTTP_API_TIMEOUT_MS,
        .write_timeout_ms = HTTP_API_TIMEOUT_MS,
        .tls_io_buffer_bytes = use_tls
                                   ? HTTP_API_TLS_IO_BUFFER_BYTES
                                   : 0u,
        .tls_handshake_timeout_ms = use_tls ? HTTP_API_TIMEOUT_MS : 0u};
    return config;
}

static chttp_server_config http_api_server_config(
    const http_api_config_t *source, const cnet_tls_server_config *tls) {
    const chttp_server_config config = {
        .host = source->host,
        .port = (uint16_t)source->port,
        .backlog = HTTP_API_CONNECTION_CAPACITY,
        .network = http_api_network_config(tls != NULL),
        .route_capacity = HTTP_API_ROUTE_CAPACITY,
        .max_route_param_count = HTTP_API_MAX_ROUTE_PARAM_COUNT,
        .max_route_param_bytes = HTTP_API_MAX_ROUTE_PARAM_BYTES,
        .max_target_bytes = HTTP_API_MAX_TARGET_BYTES,
        .max_header_count = HTTP_API_MAX_HEADER_COUNT,
        .max_header_bytes = HTTP_API_MAX_HEADER_BYTES,
        .max_request_body_bytes = HTTP_API_MAX_REQUEST_BODY_BYTES,
        .max_response_header_count = HTTP_API_MAX_HEADER_COUNT,
        .max_response_header_bytes = HTTP_API_MAX_HEADER_BYTES,
        .max_response_body_bytes = HTTP_API_MAX_RESPONSE_BODY_BYTES,
        .poll_slice_ms = HTTP_API_POLL_SLICE_MS,
        .tls = tls,
        .max_buffered_response_body_bytes =
            HTTP_API_MAX_RESPONSE_BODY_BYTES,
        .buffer_capacity_bytes = HTTP_API_BUFFER_CAPACITY_BYTES};
    return config;
}

static int http_api_register_routes(http_api_server_t *server) {
    static const char *const option_paths[] = {
        "/health",
        "/api/v1/status",
        "/api/v1/rooms",
        "/api/v1/rooms/:id/peers",
        "/api/v1/rooms/:id/peers/:peer_id",
        "/api/v1/rooms/:id/broadcast",
        "/api/v1/security/revocations/snapshot",
        "/api/v1/security/revocations/revoke"};
    int status;
    size_t index;

    status = chttp_server_get(&server->http, "/health", handle_health,
                              server);
    if (status == SALTS_OK) {
        status = chttp_server_get(&server->http, "/api/v1/status",
                                  handle_status, server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_get(&server->http, "/api/v1/rooms",
                                  handle_get_rooms, server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_get(&server->http,
                                  "/api/v1/rooms/:id/peers",
                                  handle_get_room_peers, server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_delete(
            &server->http, "/api/v1/rooms/:id/peers/:peer_id",
            handle_kick_peer, server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_post(
            &server->http, "/api/v1/rooms/:id/broadcast",
            handle_broadcast, server);
    }
    if (status == SALTS_OK && server->revocation_enabled) {
        status = chttp_server_post(
            &server->http, "/api/v1/security/revocations/snapshot",
            handle_revocation_snapshot, server);
    }
    if (status == SALTS_OK && server->revocation_enabled) {
        status = chttp_server_post(
            &server->http, "/api/v1/security/revocations/revoke",
            handle_revocation_revoke, server);
    }
    for (index = 0u;
         status == SALTS_OK && !server->config.auth_enabled &&
         index < sizeof(option_paths) / sizeof(option_paths[0]);
         ++index) {
        status = chttp_server_options(&server->http, option_paths[index],
                                      handle_options, server);
    }
    return status;
}

http_api_server_t *http_api_create(
    void *loop, const http_api_config_t *config,
    webrtc_signaling_server_t *signaling) {
    http_api_server_t *server;

    if (!config || !signaling || config->port <= 0 ||
        config->port > UINT16_MAX || !config->host ||
        config->host[0] == '\0' || !http_api_auth_config_valid(config) ||
        (config->use_tls &&
         (!config->cert_file || config->cert_file[0] == '\0' ||
          !config->key_file || config->key_file[0] == '\0'))) {
        return NULL;
    }
    (void)loop;
    server = (http_api_server_t *)calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }

    server->config = *config;
    server->config.host = tstr_dup(config->host);
    server->config.admin_token = NULL;
    server->config.auth_issuer = NULL;
    server->config.auth_active_key_id = NULL;
    server->config.auth_active_secret = NULL;
    server->config.auth_previous_key_id = NULL;
    server->config.auth_previous_secret = NULL;
    server->config.auth_revoked_token_sha256 = NULL;
    server->config.cert_file = NULL;
    server->config.key_file = NULL;
    if (!server->config.host ||
        http_api_copy_optional_string(config->admin_token,
                                      &server->config.admin_token) != 0 ||
        http_api_copy_optional_string(config->auth_issuer,
                                      &server->config.auth_issuer) != 0 ||
        http_api_copy_optional_string(config->auth_active_key_id,
                                      &server->config.auth_active_key_id) != 0 ||
        http_api_copy_optional_string(config->auth_active_secret,
                                      &server->config.auth_active_secret) != 0 ||
        http_api_copy_optional_string(config->auth_previous_key_id,
                                      &server->config.auth_previous_key_id) != 0 ||
        http_api_copy_optional_string(config->auth_previous_secret,
                                      &server->config.auth_previous_secret) != 0 ||
        http_api_copy_optional_string(
            config->auth_revoked_token_sha256,
            &server->config.auth_revoked_token_sha256) != 0) {
        http_api_free_config_strings(&server->config);
        free(server);
        return NULL;
    }
    if (config->use_tls) {
        server->config.cert_file = tstr_dup(config->cert_file);
        server->config.key_file = tstr_dup(config->key_file);
        if (!server->config.cert_file || !server->config.key_file) {
            http_api_free_config_strings(&server->config);
            free(server);
            return NULL;
        }
    }
    server->signaling = signaling;
    {
        int synchronized = 0;
        uint64_t epoch = 0U;
        uint64_t sequence = 0U;
        size_t count = 0U;
        server->revocation_enabled =
            webrtc_signaling_get_revocation_status(
                signaling, &synchronized, &epoch, &sequence, &count) == 0;
        if (server->revocation_enabled &&
            (!server->config.use_tls || !server->config.auth_enabled ||
             !server->config.auth_active_secret ||
             server->config.auth_active_secret[0] == '\0')) {
            http_api_free_config_strings(&server->config);
            free(server);
            return NULL;
        }
    }
    salts_mutex_init(&server->lifecycle_mutex);
    server->state = HTTP_API_STOPPED;
    return server;
}

int http_api_start(http_api_server_t *server) {
    cnet_tls_server_config tls;
    chttp_server_config config;
    int status;

    if (!server) {
        return -1;
    }
    salts_mutex_lock(&server->lifecycle_mutex);
    if (server->state == HTTP_API_RUNNING) {
        salts_mutex_unlock(&server->lifecycle_mutex);
        return 0;
    }
    if (server->state != HTTP_API_STOPPED || server->http_initialized) {
        salts_mutex_unlock(&server->lifecycle_mutex);
        return -1;
    }
    server->state = HTTP_API_STARTING;

    memset(&tls, 0, sizeof(tls));
    if (server->config.use_tls) {
        tls.size = sizeof(tls);
        tls.cert_file = server->config.cert_file;
        tls.key_file = server->config.key_file;
        tls.client_auth = CNET_TLS_CLIENT_AUTH_NONE;
    }
    config = http_api_server_config(
        &server->config, server->config.use_tls ? &tls : NULL);
    status = chttp_server_init(&server->http, &config);
    if (status == SALTS_OK) {
        server->http_initialized = 1;
        status = http_api_register_routes(server);
    }
    if (status == SALTS_OK) {
        status = chttp_server_start(&server->http);
    }
    if (status != SALTS_OK) {
        if (server->http_initialized) {
            (void)chttp_server_destroy(&server->http);
        }
        server->http_initialized = 0;
        server->state = HTTP_API_STOPPED;
        salts_mutex_unlock(&server->lifecycle_mutex);
        TLOG_ERRORF("Failed to start HTTP management API on {}:{}: {}",
                    server->config.host, server->config.port, status);
        return -1;
    }
    server->state = HTTP_API_RUNNING;
    salts_mutex_unlock(&server->lifecycle_mutex);
    TLOG_INFOF("{} management API listening on {}:{}",
               server->config.use_tls ? "HTTPS" : "HTTP",
               server->config.host, server->config.port);
    return 0;
}

void http_api_stop(http_api_server_t *server) {
    int stop_status;
    int destroy_status;

    if (!server) {
        return;
    }
    salts_mutex_lock(&server->lifecycle_mutex);
    if (server->state == HTTP_API_STOPPED) {
        salts_mutex_unlock(&server->lifecycle_mutex);
        return;
    }
    server->state = HTTP_API_STOPPING;
    stop_status = server->http_initialized
                      ? chttp_server_stop(&server->http, 0u)
                      : SALTS_OK;
    destroy_status = server->http_initialized
                         ? chttp_server_destroy(&server->http)
                         : SALTS_OK;
    server->http_initialized = 0;
    server->state = HTTP_API_STOPPED;
    salts_mutex_unlock(&server->lifecycle_mutex);
    if (stop_status != SALTS_OK || destroy_status != SALTS_OK) {
        TLOG_ERRORF("Failed to stop HTTP management API: stop={}, destroy={}",
                    stop_status, destroy_status);
    }
}

void http_api_destroy(http_api_server_t *server) {
    if (!server) {
        return;
    }
    http_api_stop(server);
    http_api_free_config_strings(&server->config);
    salts_mutex_destroy(&server->lifecycle_mutex);
    free(server);
}
