#include "sfu_node/http_api.h"
#include "turbo_media_auth.h"
#include "turbo_sdp.h"
#include <chttp/chttp.h>
#include <json_parser.h>
#include <salts/error_codes.h>
#include <turbo_crypto.h>
#include <salts_thread.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SFU_NODE_MEDIA_SDP_MAX_LENGTH 65536u
#define SFU_NODE_MEDIA_LOCATION_CAPACITY 256u
#define SFU_NODE_MEDIA_ETAG_CAPACITY 32u
#define SFU_NODE_CONTROL_AUDIENCE "turbomedia-sfu-control"
#define SFU_NODE_MEDIA_AUDIENCE "turbomedia-sfu-media"
#define SFU_NODE_SCOPE_CONTROL_WRITE "sfu.control.write"
#define SFU_NODE_SCOPE_CONTROL_DANGEROUS "sfu.control.dangerous"
#define SFU_NODE_SCOPE_MEDIA_PUBLISH "sfu.media.publish"
#define SFU_NODE_SCOPE_MEDIA_SUBSCRIBE "sfu.media.subscribe"
#define SFU_NODE_SCOPE_MEDIA_TRICKLE "sfu.media.trickle"
#define SFU_NODE_SCOPE_MEDIA_DELETE "sfu.media.delete"

enum {
    OK = 200,
    CREATED = 201,
    NO_CONTENT = 204,
    BAD_REQUEST = 400,
    UNAUTHORIZED = 401,
    NOT_FOUND = 404,
    CONFLICT = 409,
    PRECONDITION_FAILED = 412,
    UNSUPPORTED_MEDIA_TYPE = 415,
    PRECONDITION_REQUIRED = 428,
    INTERNAL_SERVER_ERROR = 500,
    SERVICE_UNAVAILABLE = 503
};

enum {
    SFU_NODE_HTTP_CONNECTION_CAPACITY = 64,
    SFU_NODE_HTTP_COMMAND_CAPACITY = 128,
    SFU_NODE_HTTP_REQUEST_CAPACITY = 128,
    SFU_NODE_HTTP_COMPLETION_CAPACITY = 32,
    SFU_NODE_HTTP_EVENT_CAPACITY = 128,
    SFU_NODE_HTTP_ROUTE_CAPACITY = 32,
    SFU_NODE_HTTP_MAX_TARGET_BYTES = 4096,
    SFU_NODE_HTTP_MAX_HEADER_COUNT = 64,
    SFU_NODE_HTTP_MAX_HEADER_BYTES = 32 * 1024,
    SFU_NODE_HTTP_MAX_REQUEST_BODY_BYTES = 1024 * 1024,
    SFU_NODE_HTTP_MAX_RESPONSE_BODY_BYTES = 128 * 1024,
    SFU_NODE_HTTP_MAX_SEND_BYTES = 192 * 1024,
    SFU_NODE_HTTP_RECEIVE_BUFFER_BYTES = 32 * 1024,
    SFU_NODE_HTTP_TLS_IO_BUFFER_BYTES = 256 * 1024,
    SFU_NODE_HTTP_BUFFER_CAPACITY_BYTES = 16 * 1024 * 1024,
    SFU_NODE_HTTP_TIMEOUT_MS = 5000,
    SFU_NODE_HTTP_POLL_SLICE_MS = 10
};

typedef struct Req {
    const chttp_server_request_view *request;
    struct sfu_node_app_server_s *server;
    const char *path;
    const void *body;
    size_t body_len;
} Req;

typedef struct Res {
    chttp_server_response *response;
    int status;
} Res;

struct sfu_node_http_api_s {
    sfu_node_app_server_t *server;
    salts_mutex_t lifecycle_mutex;
    chttp_server http;
    int http_initialized;
    int state;
};

typedef enum sfu_node_http_state_e {
    SFU_NODE_HTTP_STOPPED = 0,
    SFU_NODE_HTTP_STARTING,
    SFU_NODE_HTTP_RUNNING,
    SFU_NODE_HTTP_STOPPING
} sfu_node_http_state_t;

static sfu_node_app_server_t *sfu_node_http_server_from_req(const Req *req) {
    return req ? req->server : NULL;
}

static const char *get_headers(const Req *req, const char *name) {
    return req && req->request
               ? chttp_server_request_header(req->request, name)
               : NULL;
}

static const char *get_params(const Req *req, const char *name) {
    return req && req->request
               ? chttp_server_request_param(req->request, name)
               : NULL;
}

static int sfu_node_http_add_cors_headers(Res *res) {
    int status;
    if (!res || !res->response) {
        return SALTS_EINVAL;
    }
    status = chttp_server_response_set_header(
        res->response, "Access-Control-Allow-Origin", "*");
    if (status == SALTS_OK) {
        status = chttp_server_response_set_header(
            res->response, "Access-Control-Allow-Methods",
            "GET, POST, PATCH, DELETE, OPTIONS");
    }
    if (status == SALTS_OK) {
        status = chttp_server_response_set_header(
            res->response, "Access-Control-Allow-Headers",
            "Content-Type, Authorization, If-Match");
    }
    if (status == SALTS_OK) {
        status = chttp_server_response_set_header(
            res->response, "Access-Control-Expose-Headers",
            "Location, ETag, Accept-Patch");
    }
    return status;
}

static void set_header(Res *res, const char *name, const char *value) {
    if (!res || res->status != SALTS_OK) {
        return;
    }
    res->status = chttp_server_response_set_header(res->response, name, value);
}

static void reply(Res *res, unsigned int status, const char *content_type,
                  const void *body, size_t body_size) {
    if (!res || res->status != SALTS_OK) {
        return;
    }
    res->status = sfu_node_http_add_cors_headers(res);
    if (res->status == SALTS_OK) {
        res->status = chttp_server_reply(res->response, status, content_type,
                                         body, body_size);
    }
}

static void send_json(Res *res, unsigned int status, const char *body) {
    reply(res, status, "application/json", body,
          body ? strlen(body) : 0u);
}

static void send_text(Res *res, unsigned int status, const char *body) {
    reply(res, status, "text/plain", body, body ? strlen(body) : 0u);
}

typedef enum sfu_node_command_access_e {
    SFU_NODE_COMMAND_ACCESS_READ = 0,
    SFU_NODE_COMMAND_ACCESS_WRITE,
    SFU_NODE_COMMAND_ACCESS_DANGEROUS
} sfu_node_command_access_t;

static const char *json_string_field(const json_value_t *obj, const char *key) {
    json_value_t *value;

    if (!obj || !key) {
        return NULL;
    }

    value = json_object_get(obj, key);
    if (!value || json_type(value) != JSON_STRING) {
        return NULL;
    }

    return json_string(value);
}

static int json_int_field(const json_value_t *obj, const char *key, int def) {
    json_value_t *value;

    if (!obj || !key) {
        return def;
    }

    value = json_object_get(obj, key);
    if (!value || json_type(value) != JSON_NUMBER) {
        return def;
    }

    return (int)json_number(value);
}

static int json_bool_field(const json_value_t *obj, const char *key, int def) {
    json_value_t *value;

    if (!obj || !key) {
        return def;
    }

    value = json_object_get(obj, key);
    if (!value || json_type(value) != JSON_BOOL) {
        return def;
    }

    return json_bool(value) ? 1 : 0;
}

static int json_uint32_field(const json_value_t *obj, const char *key,
                             uint32_t *out_value) {
    json_value_t *value;
    double number;

    if (!obj || !key || !out_value) {
        return -1;
    }

    value = json_object_get(obj, key);
    if (!value || json_type(value) != JSON_NUMBER) {
        return -1;
    }

    number = json_number(value);
    if (number < 0 || number > 4294967295.0) {
        return -1;
    }

    *out_value = (uint32_t)number;
    return 0;
}

static int json_uint32_array_field(const json_value_t *obj, const char *key,
                                   uint32_t **out_values, int *out_count) {
    json_value_t *array;
    size_t i;
    size_t count;
    uint32_t *values;

    if (!obj || !key || !out_values || !out_count) {
        return -1;
    }

    *out_values = NULL;
    *out_count = 0;

    array = json_object_get(obj, key);
    if (!array) {
        return 0;
    }
    if (json_type(array) != JSON_ARRAY) {
        return -1;
    }

    count = json_array_size(array);
    if (count == 0) {
        return 0;
    }

    values = (uint32_t *)calloc(count, sizeof(*values));
    if (!values) {
        return -1;
    }

    for (i = 0; i < count; ++i) {
        json_value_t *item = json_array_get(array, i);
        double number;

        if (!item || json_type(item) != JSON_NUMBER) {
            free(values);
            return -1;
        }

        number = json_number(item);
        if (number < 0 || number > 4294967295.0) {
            free(values);
            return -1;
        }

        values[i] = (uint32_t)number;
    }

    *out_values = values;
    *out_count = (int)count;
    return 0;
}

static int control_auth_enabled(const sfu_node_app_config_t *config) {
    return config &&
           ((config->control_token && config->control_token[0] != '\0') ||
            (config->auth_active_secret &&
             config->auth_active_secret[0] != '\0'));
}

static turbo_media_auth_config_t signed_auth_config(
    const sfu_node_app_config_t *config) {
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

static int request_has_control_auth(
    const Req *req, const sfu_node_app_config_t *config,
    const char *required_scope, const char *room_id,
    const char *participant_id) {
    const char *authorization;
    turbo_media_auth_config_t auth;
    turbo_media_auth_policy_t policy;

    if (!control_auth_enabled(config)) {
        return 1;
    }

    authorization = get_headers(req, "Authorization");
    if (!authorization) {
        authorization = get_headers(req, "authorization");
    }
    auth = signed_auth_config(config);
    memset(&policy, 0, sizeof(policy));
    policy.audience = SFU_NODE_CONTROL_AUDIENCE;
    policy.required_scope = required_scope;
    policy.room_id = room_id;
    policy.participant_id = participant_id;
    return turbo_media_auth_authorize(
               authorization, config->control_token, &auth, &policy) !=
           TURBO_MEDIA_AUTH_DENIED;
}

static const char *request_header(const Req *req, const char *name,
                                  const char *lowercase_name) {
    const char *value;

    if (!req || !name) {
        return NULL;
    }
    value = get_headers(req, name);
    if (!value && lowercase_name) {
        value = get_headers(req, lowercase_name);
    }
    return value;
}

static int ascii_equal_ignore_case_n(const char *left, const char *right,
                                     size_t length) {
    size_t i;

    if (!left || !right) {
        return 0;
    }
    for (i = 0; i < length; ++i) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];

        if (a >= 'A' && a <= 'Z') {
            a = (unsigned char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (unsigned char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static int request_content_type_is(const Req *req, const char *expected) {
    const char *content_type = request_header(
        req, "Content-Type", "content-type");
    size_t expected_len;

    if (!content_type || !expected) {
        return 0;
    }
    expected_len = strlen(expected);
    return strlen(content_type) >= expected_len &&
           ascii_equal_ignore_case_n(content_type, expected, expected_len) &&
           (content_type[expected_len] == '\0' ||
            content_type[expected_len] == ';');
}

static int media_auth_enabled(const sfu_node_app_config_t *config) {
    return config &&
           ((config->media_access_token &&
             config->media_access_token[0] != '\0') ||
            (config->auth_active_secret &&
             config->auth_active_secret[0] != '\0'));
}

static int request_has_media_auth(
    const Req *req, const sfu_node_app_config_t *config,
    const char *required_scope, const char *room_id,
    const char *participant_id) {
    const char *authorization;
    turbo_media_auth_config_t auth;
    turbo_media_auth_policy_t policy;

    if (!media_auth_enabled(config)) {
        return 0;
    }
    authorization = request_header(req, "Authorization", "authorization");
    auth = signed_auth_config(config);
    memset(&policy, 0, sizeof(policy));
    policy.audience = SFU_NODE_MEDIA_AUDIENCE;
    policy.required_scope = required_scope;
    policy.room_id = room_id;
    policy.participant_id = participant_id;
    return turbo_media_auth_authorize(
               authorization, config->media_access_token, &auth, &policy) !=
           TURBO_MEDIA_AUTH_DENIED;
}

static int require_media_auth(Req *req, Res *res,
                              const sfu_node_app_config_t *config,
                              const char *required_scope,
                              const char *room_id,
                              const char *participant_id) {
    if (!media_auth_enabled(config)) {
        send_text(res, SERVICE_UNAVAILABLE,
                  "WHIP/WHEP media access is not configured");
        return -1;
    }
    if (!request_has_media_auth(req, config, required_scope, room_id,
                                participant_id)) {
        set_header(res, "WWW-Authenticate", "Bearer");
        send_text(res, UNAUTHORIZED, "Bearer authentication required");
        return -1;
    }
    return 0;
}

static int media_resource_id_valid(const char *value, size_t capacity) {
    size_t length;

    if (!value || capacity < 2) {
        return 0;
    }
    length = strlen(value);
    if (length == 0 || length >= capacity) {
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.' || ch == '~')) {
            return 0;
        }
    }
    return 1;
}

static int command_identifier_fields_valid(json_value_t *root) {
    const char *room_id = json_string_field(root, "room_id");
    const char *participant_id = json_string_field(root, "participant_id");
    const char *receiver_participant_id =
        json_string_field(root, "receiver_participant_id");
    const char *session_id = json_string_field(root, "session_id");
    const char *track_id = json_string_field(root, "track_id");
    const char *codec_name = json_string_field(root, "codec_name");
    const char *policy_source = json_string_field(root, "policy_source");
    const char *recording_id = json_string_field(root, "recording_id");
    const char *mode = json_string_field(root, "mode");

    return (!room_id || media_resource_id_valid(room_id, TURBO_ROOM_ID_MAX)) &&
           (!participant_id || media_resource_id_valid(
                                   participant_id, TURBO_PARTICIPANT_ID_MAX)) &&
           (!receiver_participant_id ||
            media_resource_id_valid(receiver_participant_id,
                                    TURBO_PARTICIPANT_ID_MAX)) &&
           (!session_id || media_resource_id_valid(
                               session_id, TURBO_PARTICIPANT_ID_MAX)) &&
           (!track_id || media_resource_id_valid(track_id, TURBO_TRACK_ID_MAX)) &&
           (!codec_name || media_resource_id_valid(codec_name, TURBO_CODEC_NAME_MAX)) &&
           (!policy_source ||
            media_resource_id_valid(policy_source, TURBO_POLICY_SOURCE_MAX)) &&
           (!recording_id ||
            media_resource_id_valid(recording_id, TURBO_RECORDING_ID_MAX)) &&
           (!mode || media_resource_id_valid(mode, TURBO_RECORDING_MODE_MAX));
}

static int generate_media_session_id(char output[TURBO_PARTICIPANT_ID_MAX]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t random_bytes[16];

    if (!output ||
        turbo_crypto_random(random_bytes, sizeof(random_bytes)) !=
            TURBO_CRYPTO_OK) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(random_bytes); ++i) {
        output[i * 2] = hex[random_bytes[i] >> 4];
        output[i * 2 + 1] = hex[random_bytes[i] & 0x0f];
    }
    output[sizeof(random_bytes) * 2] = '\0';
    return 0;
}

static void format_media_etag(uint64_t version,
                              char output[SFU_NODE_MEDIA_ETAG_CAPACITY]) {
    snprintf(output, SFU_NODE_MEDIA_ETAG_CAPACITY, "\"%" PRIu64 "\"",
             version);
}

static int parse_media_etag(const char *value, uint64_t *version_out) {
    uint64_t version = 0;
    size_t length;

    if (!value || !version_out) {
        return -1;
    }
    length = strlen(value);
    if (length < 3 || value[0] != '"' || value[length - 1] != '"') {
        return -1;
    }
    for (size_t i = 1; i + 1 < length; ++i) {
        unsigned int digit;

        if (value[i] < '0' || value[i] > '9') {
            return -1;
        }
        digit = (unsigned int)(value[i] - '0');
        if (version > (UINT64_MAX - digit) / 10u) {
            return -1;
        }
        version = version * 10u + digit;
    }
    if (version == 0) {
        return -1;
    }
    *version_out = version;
    return 0;
}

static int media_offer_direction_valid(const char *sdp,
                                       const char *resource_kind) {
    sdp_session_t parsed;
    int active_media_count = 0;
    int whip;

    if (!sdp || !resource_kind) {
        return 0;
    }
    whip = strcmp(resource_kind, "whip") == 0;
    if (!whip && strcmp(resource_kind, "whep") != 0) {
        return 0;
    }
    if (sdp_parse(sdp, strlen(sdp), &parsed) != 0) {
        return 0;
    }
    for (int i = 0; i < parsed.media_count; ++i) {
        const sdp_media_t *media = &parsed.media[i];

        if (media->port == 0 || media->type == SDP_MEDIA_APPLICATION) {
            continue;
        }
        active_media_count++;
        if (whip) {
            if (media->direction != SDP_DIRECTION_SENDONLY &&
                media->direction != SDP_DIRECTION_SENDRECV) {
                return 0;
            }
        } else if (media->direction != SDP_DIRECTION_RECVONLY &&
                   media->direction != SDP_DIRECTION_SENDRECV) {
            return 0;
        }
    }
    return active_media_count > 0;
}

static sfu_node_command_access_t command_access_for_type(const char *type) {
    if (!type) {
        return SFU_NODE_COMMAND_ACCESS_WRITE;
    }

    if (strcmp(type, "get_recording_status") == 0 ||
        strcmp(type, "get_room_stats") == 0 ||
        strcmp(type, "get_participant_stats") == 0 ||
        strcmp(type, "get_track_subscription") == 0 ||
        strcmp(type, "get_webrtc_session") == 0 ||
        strcmp(type, "get_node_stats") == 0) {
        return SFU_NODE_COMMAND_ACCESS_READ;
    }

    if (strcmp(type, "force_close_room") == 0 ||
        strcmp(type, "detach_room") == 0 ||
        strcmp(type, "disconnect_media_participant") == 0 ||
        strcmp(type, "set_node_drain") == 0 ||
        strcmp(type, "start_recording") == 0 ||
        strcmp(type, "stop_recording") == 0) {
        return SFU_NODE_COMMAND_ACCESS_DANGEROUS;
    }

    return SFU_NODE_COMMAND_ACCESS_WRITE;
}

static int command_requires_control_auth(const char *type) {
    return command_access_for_type(type) != SFU_NODE_COMMAND_ACCESS_READ;
}

static turbo_room_video_layer_t parse_video_layer(const char *value) {
    if (!value || strcmp(value, "none") == 0) {
        return TURBO_ROOM_VIDEO_LAYER_NONE;
    }
    if (strcmp(value, "low") == 0) {
        return TURBO_ROOM_VIDEO_LAYER_LOW;
    }
    if (strcmp(value, "medium") == 0 || strcmp(value, "mid") == 0) {
        return TURBO_ROOM_VIDEO_LAYER_MEDIUM;
    }
    if (strcmp(value, "high") == 0) {
        return TURBO_ROOM_VIDEO_LAYER_HIGH;
    }
    return 0;
}

static turbo_room_track_kind_t parse_track_kind(const char *value) {
    if (!value) {
        return 0;
    }
    if (strcmp(value, "audio") == 0) {
        return TURBO_ROOM_TRACK_AUDIO;
    }
    if (strcmp(value, "video") == 0) {
        return TURBO_ROOM_TRACK_VIDEO;
    }
    return 0;
}

static char *room_stats_json(turbo_sfu_node_t *node, const char *room_id) {
    turbo_sfu_node_room_stats_t stats;
    char *json;

    if (!node || !room_id || turbo_sfu_node_get_room_stats(node, room_id, &stats) != 0) {
        return NULL;
    }

    json = (char *)malloc(640);
    if (!json) {
        return NULL;
    }

    snprintf(json, 640,
             "{"
             "\"room_id\":\"%s\","
             "\"session_count\":%d,"
             "\"participant_count\":%d,"
             "\"published_track_count\":%d,"
             "\"total_packets_routed\":%lld,"
             "\"total_bytes_routed\":%lld,"
             "\"total_layer_switches\":%lld"
             "}",
             stats.room_id,
             stats.session_count,
             stats.participant_count,
             stats.published_track_count,
             (long long)stats.total_packets_routed,
             (long long)stats.total_bytes_routed,
             (long long)stats.total_layer_switches);

    return json;
}

static char *node_stats_json(sfu_node_app_server_t *server, turbo_sfu_node_t *node) {
    turbo_sfu_node_stats_t stats;
    char *json;
    int draining;

    if (!node) {
        return NULL;
    }

    turbo_sfu_node_get_stats(node, &stats);
    draining = sfu_node_app_server_is_draining(server);

    json = (char *)malloc(704);
    if (!json) {
        return NULL;
    }

    snprintf(json, 704,
             "{"
             "\"node_id\":\"%s\","
             "\"draining\":%s,"
             "\"room_count\":%d,"
             "\"session_count\":%d,"
             "\"published_track_count\":%d,"
             "\"total_packets_routed\":%lld,"
             "\"total_bytes_routed\":%lld,"
             "\"total_layer_switches\":%lld"
             "}",
             stats.node_id,
             draining ? "true" : "false",
             stats.room_count,
             stats.session_count,
             stats.published_track_count,
             (long long)stats.total_packets_routed,
             (long long)stats.total_bytes_routed,
             (long long)stats.total_layer_switches);

    return json;
}

static char *node_metrics_text(sfu_node_app_server_t *server, turbo_sfu_node_t *node) {
    turbo_sfu_node_stats_t stats;
    char *text;
    int draining;
    const sfu_node_app_config_t *config;

    if (!server || !node) {
        return NULL;
    }

    memset(&stats, 0, sizeof(stats));
    turbo_sfu_node_get_stats(node, &stats);
    draining = sfu_node_app_server_is_draining(server);
    config = sfu_node_app_server_get_config(server);

    text = (char *)malloc(1024);
    if (!text) {
        return NULL;
    }

    snprintf(text, 1024,
             "# TYPE turbo_sfu_node_up gauge\n"
             "turbo_sfu_node_up 1\n"
             "# TYPE turbo_sfu_node_draining gauge\n"
             "turbo_sfu_node_draining{node_id=\"%s\"} %d\n"
             "# TYPE turbo_sfu_node_control_auth_enabled gauge\n"
             "turbo_sfu_node_control_auth_enabled{node_id=\"%s\"} %d\n"
             "# TYPE turbo_sfu_node_rooms gauge\n"
             "turbo_sfu_node_rooms{node_id=\"%s\"} %d\n"
             "# TYPE turbo_sfu_node_sessions gauge\n"
             "turbo_sfu_node_sessions{node_id=\"%s\"} %d\n"
             "# TYPE turbo_sfu_node_published_tracks gauge\n"
             "turbo_sfu_node_published_tracks{node_id=\"%s\"} %d\n"
             "# TYPE turbo_sfu_node_packets_routed counter\n"
             "turbo_sfu_node_packets_routed{node_id=\"%s\"} %lld\n"
             "# TYPE turbo_sfu_node_bytes_routed counter\n"
             "turbo_sfu_node_bytes_routed{node_id=\"%s\"} %lld\n"
             "# TYPE turbo_sfu_node_layer_switches counter\n"
             "turbo_sfu_node_layer_switches{node_id=\"%s\"} %lld\n",
             stats.node_id,
             draining,
             stats.node_id,
             control_auth_enabled(config) ? 1 : 0,
             stats.node_id,
             stats.room_count,
             stats.node_id,
             stats.session_count,
             stats.node_id,
             stats.published_track_count,
             stats.node_id,
             (long long)stats.total_packets_routed,
             stats.node_id,
             (long long)stats.total_bytes_routed,
             stats.node_id,
             (long long)stats.total_layer_switches);

    return text;
}

static char *participant_stats_json(turbo_sfu_node_t *node, const char *room_id,
                                    const char *participant_id) {
    turbo_sfu_node_participant_stats_t stats;
    char *json;

    if (!node || !room_id || !participant_id) {
        return NULL;
    }

    memset(&stats, 0, sizeof(stats));
    if (turbo_sfu_node_get_participant_stats(node, room_id, participant_id, &stats) != 0) {
        return NULL;
    }

    json = (char *)malloc(512);
    if (!json) {
        return NULL;
    }

    snprintf(json, 512,
             "{"
             "\"participant_id\":\"%s\","
             "\"stream_count\":%d,"
             "\"available_bandwidth\":%d,"
             "\"packets_sent\":%lld,"
             "\"bytes_sent\":%lld,"
             "\"packets_received\":%lld,"
             "\"bytes_received\":%lld"
             "}",
             stats.participant_id,
             stats.stream_count,
             stats.available_bandwidth,
             (long long)stats.packets_sent,
             (long long)stats.bytes_sent,
             (long long)stats.packets_received,
             (long long)stats.bytes_received);

    return json;
}

static const char *video_layer_name(turbo_room_video_layer_t layer) {
    switch (layer) {
        case TURBO_ROOM_VIDEO_LAYER_NONE: return "none";
        case TURBO_ROOM_VIDEO_LAYER_LOW: return "low";
        case TURBO_ROOM_VIDEO_LAYER_MEDIUM: return "medium";
        case TURBO_ROOM_VIDEO_LAYER_HIGH: return "high";
        default: return "unknown";
    }
}

static char *track_subscription_json(turbo_sfu_node_t *node, const char *room_id,
                                     const char *receiver_participant_id,
                                     const char *track_id) {
    turbo_sfu_node_track_subscription_t subscription;
    char *json;

    if (!node || !room_id || !receiver_participant_id || !track_id) {
        return NULL;
    }

    memset(&subscription, 0, sizeof(subscription));
    if (turbo_sfu_node_get_track_subscription(node, room_id, receiver_participant_id, track_id,
                                              &subscription) != 0) {
        return NULL;
    }

    json = (char *)malloc(768);
    if (!json) {
        return NULL;
    }

    snprintf(json, 768,
             "{"
             "\"receiver_participant_id\":\"%s\","
             "\"sender_participant_id\":\"%s\","
             "\"track_id\":\"%s\","
             "\"enabled\":%s,"
             "\"priority\":%d,"
             "\"preferred_layer\":\"%s\","
             "\"target_layer\":\"%s\","
             "\"muted\":%s,"
             "\"policy_source\":\"%s\","
             "\"max_layer\":\"%s\""
             "}",
             subscription.receiver_participant_id,
             subscription.sender_participant_id,
             subscription.track_id,
             subscription.enabled ? "true" : "false",
             subscription.priority,
             video_layer_name(subscription.preferred_layer),
             video_layer_name(subscription.target_layer),
             subscription.muted ? "true" : "false",
             subscription.policy_source,
             video_layer_name(subscription.max_layer));

    return json;
}

static char *webrtc_session_json(sfu_node_app_server_t *server, const char *room_id,
                                 const char *session_id) {
    if (!server || !room_id || !session_id) {
        return NULL;
    }

    return sfu_node_app_server_build_webrtc_session_json(server, room_id, session_id);
}

static char *recording_status_json(sfu_node_app_server_t *server, const char *room_id) {
    if (!server || !room_id) {
        return NULL;
    }

    return sfu_node_app_server_build_recording_status_json(server, room_id);
}

static int parse_webrtc_session_path(const char *path, char *room_id,
                                     size_t room_id_size, char *session_id,
                                     size_t session_id_size) {
    const char *prefix = "/api/v1/rooms/";
    const char *room_start;
    const char *room_end;
    const char *session_start;
    size_t room_len;
    size_t session_len;

    if (!path || !room_id || room_id_size == 0 || !session_id || session_id_size == 0 ||
        strncmp(path, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    room_start = path + strlen(prefix);
    room_end = strstr(room_start, "/webrtc_sessions/");
    if (!room_end) {
        return -1;
    }

    session_start = room_end + strlen("/webrtc_sessions/");
    if (*session_start == '\0') {
        return -1;
    }

    room_len = (size_t)(room_end - room_start);
    session_len = strlen(session_start);
    if (room_len == 0 || room_len >= room_id_size || session_len == 0 ||
        session_len >= session_id_size) {
        return -1;
    }

    memcpy(room_id, room_start, room_len);
    room_id[room_len] = '\0';
    memcpy(session_id, session_start, session_len);
    session_id[session_len] = '\0';
    return 0;
}

static void send_error_json(Res *res, int status, const char *code, const char *message) {
    char json[512];

    snprintf(json, sizeof(json),
             "{\"ok\":false,\"code\":\"%s\",\"message\":\"%s\"}",
             code ? code : "INTERNAL_ERROR",
             message ? message : "internal error");
    send_json(res, status, json);
}

static void send_entity_ok_json(Res *res, const char *field_name, char *entity_json) {
    char *payload;
    size_t len;

    if (!field_name || !entity_json) {
        send_json(res, 200, "{\"ok\":true}");
        return;
    }

    len = strlen(field_name) + strlen(entity_json) + 32;
    payload = (char *)malloc(len);
    if (!payload) {
        free(entity_json);
        send_json(res, 200, "{\"ok\":true}");
        return;
    }

    snprintf(payload, len, "{\"ok\":true,\"%s\":%s}", field_name, entity_json);
    send_json(res, 200, payload);
    free(payload);
    free(entity_json);
}

static void handle_health(Req *req, Res *res) {
    sfu_node_app_server_t *server = sfu_node_http_server_from_req(req);
    turbo_sfu_node_t *node;
    char *json;

    if (!server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "sfu node not initialized");
        return;
    }

    node = sfu_node_app_server_get_node(server);
    json = node_stats_json(server, node);
    if (!json) {
        send_error_json(res, 500, "SERVER_NOT_READY", "sfu node unavailable");
        return;
    }

    send_entity_ok_json(res, "node_stats", json);
}

static void handle_ready(Req *req, Res *res) {
    sfu_node_app_server_t *server = sfu_node_http_server_from_req(req);
    turbo_sfu_node_t *node;
    int draining;
    char json[192];

    if (!server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "sfu node not initialized");
        return;
    }

    node = sfu_node_app_server_get_node(server);
    if (!node) {
        send_error_json(res, 500, "SERVER_NOT_READY", "sfu node unavailable");
        return;
    }

    draining = sfu_node_app_server_is_draining(server);
    snprintf(json, sizeof(json), "{\"ok\":%s,\"draining\":%s}",
             draining ? "false" : "true",
             draining ? "true" : "false");
    send_json(res, draining ? 503 : 200, json);
}

static void handle_metrics(Req *req, Res *res) {
    sfu_node_app_server_t *server = sfu_node_http_server_from_req(req);
    turbo_sfu_node_t *node;
    char *text;

    if (!server) {
        send_text(res, 500, "sfu node not initialized\n");
        return;
    }

    node = sfu_node_app_server_get_node(server);
    text = node_metrics_text(server, node);
    if (!text) {
        send_text(res, 500, "sfu node unavailable\n");
        return;
    }

    send_text(res, 200, text);
    free(text);
}

static void handle_get_webrtc_session(Req *req, Res *res) {
    sfu_node_app_server_t *server = sfu_node_http_server_from_req(req);
    char room_id_buf[TURBO_ROOM_ID_MAX];
    char session_id_buf[TURBO_PARTICIPANT_ID_MAX];
    const char *room_id = NULL;
    const char *session_id = NULL;
    char *json;

    if (!server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "sfu node not initialized");
        return;
    }

    if (req->path &&
        parse_webrtc_session_path(req->path, room_id_buf, sizeof(room_id_buf),
                                  session_id_buf, sizeof(session_id_buf)) == 0) {
        room_id = room_id_buf;
        session_id = session_id_buf;
    }
    if (!room_id || !session_id) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing room_id or session_id");
        return;
    }

    json = webrtc_session_json(server, room_id, session_id);
    if (!json) {
        send_error_json(res, 404, "WEBRTC_SESSION_NOT_FOUND", "webrtc session not found");
        return;
    }

    send_entity_ok_json(res, "webrtc_session", json);
}

static void handle_media_session_post(Req *req, Res *res,
                                      const char *resource_kind) {
    sfu_node_app_server_t *server = sfu_node_http_server_from_req(req);
    const sfu_node_app_config_t *config;
    turbo_sfu_node_t *node;
    turbo_sfu_node_room_stats_t room_stats;
    const char *room_id = get_params(req, "room_id");
    const char *participant_id = get_params(req, "participant_id");
    char session_id[TURBO_PARTICIPANT_ID_MAX];
    char location[SFU_NODE_MEDIA_LOCATION_CAPACITY];
    char etag[SFU_NODE_MEDIA_ETAG_CAPACITY];
    char *offer = NULL;
    char *answer = NULL;
    uint64_t version = 0;
    int created = 0;
    int location_length;

    if (!server || !resource_kind) {
        send_text(res, INTERNAL_SERVER_ERROR, "SFU node is unavailable");
        return;
    }
    config = sfu_node_app_server_get_config(server);
    if (require_media_auth(
            req, res, config,
            strcmp(resource_kind, "whip") == 0
                ? SFU_NODE_SCOPE_MEDIA_PUBLISH
                : SFU_NODE_SCOPE_MEDIA_SUBSCRIBE,
            room_id, participant_id) != 0) {
        return;
    }
    if (sfu_node_app_server_is_draining(server)) {
        send_text(res, SERVICE_UNAVAILABLE, "SFU node is draining");
        return;
    }
    if (!media_resource_id_valid(room_id, TURBO_ROOM_ID_MAX) ||
        !media_resource_id_valid(participant_id, TURBO_PARTICIPANT_ID_MAX)) {
        send_text(res, BAD_REQUEST, "Invalid room or participant identifier");
        return;
    }
    if (!request_content_type_is(req, "application/sdp")) {
        send_text(res, UNSUPPORTED_MEDIA_TYPE,
                  "Content-Type must be application/sdp");
        return;
    }
    if (!req->body || req->body_len == 0 ||
        req->body_len > SFU_NODE_MEDIA_SDP_MAX_LENGTH ||
        memchr(req->body, '\0', req->body_len) != NULL) {
        send_text(res, BAD_REQUEST, "Invalid SDP offer body");
        return;
    }

    node = sfu_node_app_server_get_node(server);
    memset(&room_stats, 0, sizeof(room_stats));
    if (!node || turbo_sfu_node_get_room_stats(node, room_id, &room_stats) != 0) {
        send_text(res, NOT_FOUND, "Room must be provisioned before media setup");
        return;
    }
    if (generate_media_session_id(session_id) != 0) {
        send_text(res, INTERNAL_SERVER_ERROR,
                  "Failed to allocate media session identifier");
        return;
    }

    offer = (char *)malloc(req->body_len + 1);
    if (!offer) {
        send_text(res, INTERNAL_SERVER_ERROR, "Failed to allocate SDP offer");
        return;
    }
    memcpy(offer, req->body, req->body_len);
    offer[req->body_len] = '\0';
    if (!media_offer_direction_valid(offer, resource_kind)) {
        send_text(res, BAD_REQUEST,
                  "SDP media direction is incompatible with this endpoint");
        goto cleanup;
    }

    if (sfu_node_app_server_create_owned_media_session(
            server, room_id, participant_id, session_id) != 0) {
        send_text(res, CONFLICT,
                  "Participant already has an active WebRTC media session");
        goto cleanup;
    }
    created = 1;
    if (sfu_node_app_server_set_remote_offer(
            server, room_id, session_id, offer) != 0) {
        send_text(res, BAD_REQUEST, "Invalid or unsupported SDP offer");
        goto cleanup;
    }
    answer = sfu_node_app_server_copy_local_answer(
        server, room_id, participant_id, session_id);
    if (!answer ||
        sfu_node_app_server_get_media_session_version(
            server, room_id, participant_id, session_id, &version) != 0) {
        send_text(res, INTERNAL_SERVER_ERROR,
                  "Failed to create the SDP answer");
        goto cleanup;
    }
    location_length = snprintf(
        location, sizeof(location), "/%s/%s/%s/sessions/%s",
        resource_kind, room_id, participant_id, session_id);
    if (location_length < 0 ||
        (size_t)location_length >= sizeof(location)) {
        send_text(res, INTERNAL_SERVER_ERROR,
                  "Failed to create media resource location");
        goto cleanup;
    }
    format_media_etag(version, etag);
    set_header(res, "Location", location);
    set_header(res, "ETag", etag);
    set_header(res, "Accept-Patch", "application/trickle-ice-sdpfrag");
    set_header(res, "Cache-Control", "no-store");
    reply(res, CREATED, "application/sdp", answer, strlen(answer));
    created = 0;

cleanup:
    if (created) {
        sfu_node_app_server_remove_media_session(
            server, room_id, participant_id, session_id);
    }
    free(answer);
    free(offer);
}

static void handle_whip_post(Req *req, Res *res) {
    handle_media_session_post(req, res, "whip");
}

static void handle_whep_post(Req *req, Res *res) {
    handle_media_session_post(req, res, "whep");
}

static void handle_media_session_patch(Req *req, Res *res) {
    sfu_node_app_server_t *server = sfu_node_http_server_from_req(req);
    const sfu_node_app_config_t *config;
    const char *room_id = get_params(req, "room_id");
    const char *participant_id = get_params(req, "participant_id");
    const char *session_id = get_params(req, "session_id");
    const char *if_match;
    char *local_fragment = NULL;
    char etag[SFU_NODE_MEDIA_ETAG_CAPACITY];
    uint64_t expected_version;
    uint64_t current_version = 0;
    int result;

    if (!server) {
        send_text(res, INTERNAL_SERVER_ERROR, "SFU node is unavailable");
        return;
    }
    config = sfu_node_app_server_get_config(server);
    if (require_media_auth(req, res, config, SFU_NODE_SCOPE_MEDIA_TRICKLE,
                           room_id, participant_id) != 0) {
        return;
    }
    if (!media_resource_id_valid(room_id, TURBO_ROOM_ID_MAX) ||
        !media_resource_id_valid(participant_id, TURBO_PARTICIPANT_ID_MAX) ||
        !media_resource_id_valid(session_id, TURBO_PARTICIPANT_ID_MAX)) {
        send_text(res, BAD_REQUEST, "Invalid media resource identifier");
        return;
    }
    if (!request_content_type_is(req, "application/trickle-ice-sdpfrag")) {
        send_text(res, UNSUPPORTED_MEDIA_TYPE,
                  "Content-Type must be application/trickle-ice-sdpfrag");
        return;
    }
    if (!req->body || req->body_len == 0 ||
        req->body_len > SFU_NODE_MEDIA_SDP_MAX_LENGTH) {
        send_text(res, BAD_REQUEST, "Invalid ICE SDP fragment body");
        return;
    }
    if_match = request_header(req, "If-Match", "if-match");
    if (!if_match) {
        send_text(res, PRECONDITION_REQUIRED, "If-Match is required");
        return;
    }
    if (parse_media_etag(if_match, &expected_version) != 0) {
        send_text(res, PRECONDITION_FAILED, "Invalid media resource ETag");
        return;
    }

    local_fragment = (char *)malloc(SFU_NODE_MEDIA_SDP_MAX_LENGTH + 1);
    if (!local_fragment) {
        send_text(res, INTERNAL_SERVER_ERROR,
                  "Failed to allocate ICE SDP fragment");
        return;
    }
    result = sfu_node_app_server_apply_remote_ice_sdpfrag(
        server, room_id, participant_id, session_id, expected_version,
        req->body, req->body_len, local_fragment,
        SFU_NODE_MEDIA_SDP_MAX_LENGTH + 1, &current_version);
    if (result == SFU_NODE_MEDIA_SESSION_NOT_FOUND) {
        send_text(res, NOT_FOUND, "Media session not found");
    } else if (result == SFU_NODE_MEDIA_SESSION_PRECONDITION_FAILED) {
        send_text(res, PRECONDITION_FAILED, "Media resource ETag is stale");
    } else if (result < 0) {
        send_text(res, BAD_REQUEST, "Invalid ICE SDP fragment");
    } else {
        format_media_etag(current_version, etag);
        set_header(res, "ETag", etag);
        set_header(res, "Cache-Control", "no-store");
        if (result > 0) {
            reply(res, OK, "application/trickle-ice-sdpfrag",
                  local_fragment, strlen(local_fragment));
        } else {
            reply(res, NO_CONTENT, "application/trickle-ice-sdpfrag", "", 0);
        }
    }
    free(local_fragment);
}

static void handle_media_session_delete(Req *req, Res *res) {
    sfu_node_app_server_t *server = sfu_node_http_server_from_req(req);
    const sfu_node_app_config_t *config;
    const char *room_id = get_params(req, "room_id");
    const char *participant_id = get_params(req, "participant_id");
    const char *session_id = get_params(req, "session_id");
    int result;

    if (!server) {
        send_text(res, INTERNAL_SERVER_ERROR, "SFU node is unavailable");
        return;
    }
    config = sfu_node_app_server_get_config(server);
    if (require_media_auth(req, res, config, SFU_NODE_SCOPE_MEDIA_DELETE,
                           room_id, participant_id) != 0) {
        return;
    }
    if (!media_resource_id_valid(room_id, TURBO_ROOM_ID_MAX) ||
        !media_resource_id_valid(participant_id, TURBO_PARTICIPANT_ID_MAX) ||
        !media_resource_id_valid(session_id, TURBO_PARTICIPANT_ID_MAX)) {
        send_text(res, BAD_REQUEST, "Invalid media resource identifier");
        return;
    }
    result = sfu_node_app_server_remove_media_session(
        server, room_id, participant_id, session_id);
    if (result == SFU_NODE_MEDIA_SESSION_NOT_FOUND) {
        send_text(res, NOT_FOUND, "Media session not found");
        return;
    }
    if (result != 0) {
        send_text(res, INTERNAL_SERVER_ERROR,
                  "Failed to terminate media session");
        return;
    }
    set_header(res, "Cache-Control", "no-store");
    reply(res, NO_CONTENT, "text/plain", "", 0);
}

static void handle_command(Req *req, Res *res) {
    sfu_node_app_server_t *server = sfu_node_http_server_from_req(req);
    turbo_sfu_node_t *node;
    json_value_t *root = NULL;
    const char *type;
    const char *room_id;
    const char *auth_participant_id;
    const char *required_scope;
    const sfu_node_app_config_t *config;
    sfu_node_command_access_t access;
    int rc = -1;

    if (!server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "sfu node not initialized");
        return;
    }
    if (!req->body || req->body_len == 0) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing request body");
        return;
    }

    node = sfu_node_app_server_get_node(server);
    if (!node) {
        send_error_json(res, 500, "SERVER_NOT_READY", "sfu node unavailable");
        return;
    }

    if (((root = json_parse((const char *)((const uint8_t *)req->body), req->body_len)) ? 0 : -1) != 0 ||
        !root || json_type(root) != JSON_OBJECT) {
        send_error_json(res, 400, "INVALID_JSON", "request body must be a JSON object");
        json_free(root);
        root = NULL;
        return;
    }

    type = json_string_field(root, "type");
    room_id = json_string_field(root, "room_id");
    auth_participant_id = json_string_field(root, "participant_id");
    if (!type) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing command type");
        json_free(root);
        root = NULL;
        return;
    }
    if (!command_identifier_fields_valid(root)) {
        send_error_json(res, 400, "INVALID_REQUEST",
                        "invalid command identifier");
        json_free(root);
        root = NULL;
        return;
    }
    config = sfu_node_app_server_get_config(server);
    access = command_access_for_type(type);
    required_scope =
        access == SFU_NODE_COMMAND_ACCESS_DANGEROUS
            ? SFU_NODE_SCOPE_CONTROL_DANGEROUS
            : SFU_NODE_SCOPE_CONTROL_WRITE;
    if (command_requires_control_auth(type) &&
        !request_has_control_auth(req, config, required_scope, room_id,
                                  auth_participant_id)) {
        send_error_json(res, 401, "UNAUTHORIZED", "control token required");
        json_free(root);
        root = NULL;
        return;
    }

    if (strcmp(type, "set_node_drain") == 0) {
        char *json;

        rc = sfu_node_app_server_set_draining(
            server, json_bool_field(root, "draining", 1));
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            json_free(root);
            root = NULL;
            return;
        }
        json = node_stats_json(server, node);
        if (!json) {
            send_error_json(res, 500, "SERVER_NOT_READY", "sfu node unavailable");
            json_free(root);
            root = NULL;
            return;
        }
        send_entity_ok_json(res, "node_stats", json);
        json_free(root);
        root = NULL;
        return;
    } else if (strcmp(type, "attach_room") == 0) {
        if (!room_id) {
            send_error_json(res, 400, "INVALID_REQUEST", "missing room_id");
            json_free(root);
            root = NULL;
            return;
        }
        if (sfu_node_app_server_is_draining(server)) {
            send_error_json(res, 409, "NODE_DRAINING",
                            "sfu node is draining and not accepting new rooms");
            json_free(root);
            root = NULL;
            return;
        }
        rc = turbo_sfu_node_attach_room(node, room_id,
                                        json_int_field(root, "max_participants", 0));
    } else if (strcmp(type, "force_close_room") == 0) {
        rc = turbo_sfu_node_force_close_room(node, room_id);
        if (rc == 0) {
            sfu_node_app_server_remove_room_webrtc_sessions(server, room_id);
            sfu_node_app_server_clear_room_runtime(server, room_id);
        }
    } else if (strcmp(type, "detach_room") == 0) {
        rc = turbo_sfu_node_detach_room(node, room_id);
        if (rc == 0) {
            sfu_node_app_server_remove_room_webrtc_sessions(server, room_id);
            sfu_node_app_server_clear_room_runtime(server, room_id);
        }
    } else if (strcmp(type, "add_session") == 0) {
        const char *participant_id = json_string_field(root, "participant_id");
        const char *session_id = json_string_field(root, "session_id");

        if (!room_id || !participant_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "missing room_id or participant_id");
            json_free(root);
            root = NULL;
            return;
        }

        rc = turbo_sfu_node_add_session(node, room_id, participant_id,
                                        session_id ? session_id : participant_id, NULL);
    } else if (strcmp(type, "remove_session") == 0) {
        const char *session_id = json_string_field(root, "session_id");
        rc = turbo_sfu_node_remove_session(node, room_id, session_id);
        if (rc == 0 && room_id && session_id) {
            sfu_node_app_server_remove_webrtc_session(server, room_id, session_id);
        }
    } else if (strcmp(type, "disconnect_media_participant") == 0) {
        const char *participant_id = json_string_field(root, "participant_id");

        if (!room_id || !participant_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "missing room_id or participant_id");
            json_free(root);
            root = NULL;
            return;
        }
        rc = sfu_node_app_server_disconnect_media_participant(
            server, room_id, participant_id);
    } else if (strcmp(type, "create_webrtc_session") == 0) {
        const char *participant_id = json_string_field(root, "participant_id");
        const char *session_id = json_string_field(root, "session_id");
        char *json;

        if (!room_id || !participant_id || !session_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "missing room_id, participant_id or session_id");
            json_free(root);
            root = NULL;
            return;
        }

        rc = sfu_node_app_server_create_webrtc_session(server, room_id,
                                                       participant_id, session_id);
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            json_free(root);
            root = NULL;
            return;
        }

        json = webrtc_session_json(server, room_id, session_id);
        if (!json) {
            send_error_json(res, 500, "COMMAND_FAILED", "webrtc session creation incomplete");
            json_free(root);
            root = NULL;
            return;
        }

        send_entity_ok_json(res, "webrtc_session", json);
        json_free(root);
        root = NULL;
        return;
    } else if (strcmp(type, "set_remote_offer") == 0) {
        const char *session_id = json_string_field(root, "session_id");
        const char *sdp = json_string_field(root, "sdp");
        char *json;

        if (!room_id || !session_id || !sdp) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "missing room_id, session_id or sdp");
            json_free(root);
            root = NULL;
            return;
        }

        rc = sfu_node_app_server_set_remote_offer(server, room_id, session_id, sdp);
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            json_free(root);
            root = NULL;
            return;
        }

        json = webrtc_session_json(server, room_id, session_id);
        if (!json) {
            send_error_json(res, 500, "COMMAND_FAILED", "webrtc session update incomplete");
            json_free(root);
            root = NULL;
            return;
        }

        send_entity_ok_json(res, "webrtc_session", json);
        json_free(root);
        root = NULL;
        return;
    } else if (strcmp(type, "add_remote_ice_candidate") == 0) {
        const char *session_id = json_string_field(root, "session_id");
        const char *candidate = json_string_field(root, "candidate");
        char *json;

        if (!room_id || !session_id || !candidate) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "missing room_id, session_id or candidate");
            json_free(root);
            root = NULL;
            return;
        }

        rc = sfu_node_app_server_add_remote_ice_candidate(server, room_id,
                                                          session_id, candidate);
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            json_free(root);
            root = NULL;
            return;
        }

        json = webrtc_session_json(server, room_id, session_id);
        if (!json) {
            send_error_json(res, 500, "COMMAND_FAILED", "webrtc session update incomplete");
            json_free(root);
            root = NULL;
            return;
        }

        send_entity_ok_json(res, "webrtc_session", json);
        json_free(root);
        root = NULL;
        return;
    } else if (strcmp(type, "register_published_track") == 0) {
        const char *participant_id = json_string_field(root, "participant_id");
        const char *track_id = json_string_field(root, "track_id");
        const char *codec_name = json_string_field(root, "codec_name");
        turbo_room_track_kind_t kind = parse_track_kind(json_string_field(root, "kind"));
        uint32_t main_ssrc = 0;
        uint32_t *layer_ssrcs = NULL;
        int layer_count = 0;

        if (!room_id || !participant_id || !track_id ||
            json_uint32_field(root, "main_ssrc", &main_ssrc) != 0 ||
            json_uint32_array_field(root, "layer_ssrcs", &layer_ssrcs, &layer_count) != 0) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid register_published_track payload");
            json_free(root);
            root = NULL;
            free(layer_ssrcs);
            return;
        }

        rc = turbo_sfu_node_register_published_track(node, room_id, participant_id, track_id,
                                                     main_ssrc, layer_ssrcs, layer_count);
        if (rc == 0) {
            rc = sfu_node_app_server_register_published_track(
                server, room_id, participant_id, track_id, main_ssrc,
                layer_ssrcs, layer_count, kind, codec_name);
        }
        free(layer_ssrcs);
    } else if (strcmp(type, "unregister_published_track") == 0) {
        rc = turbo_sfu_node_unregister_published_track(node, room_id,
                                                       json_string_field(root, "track_id"));
        if (rc == 0) {
            rc = sfu_node_app_server_unregister_published_track(
                server, room_id, json_string_field(root, "track_id"));
        }
    } else if (strcmp(type, "set_receiver_bandwidth") == 0) {
        rc = turbo_sfu_node_set_receiver_bandwidth(
            node, room_id, json_string_field(root, "participant_id"),
            json_int_field(root, "bandwidth_bps", 0));
    } else if (strcmp(type, "set_track_subscription") == 0) {
        const char *receiver_participant_id = json_string_field(root, "receiver_participant_id");
        const char *track_id = json_string_field(root, "track_id");
        turbo_sfu_node_track_subscription_t subscription;

        memset(&subscription, 0, sizeof(subscription));
        if (receiver_participant_id) {
            strncpy(subscription.receiver_participant_id, receiver_participant_id,
                    sizeof(subscription.receiver_participant_id) - 1);
        }
        if (track_id) {
            strncpy(subscription.track_id, track_id, sizeof(subscription.track_id) - 1);
        }
        subscription.enabled = json_bool_field(root, "enabled", 1);
        subscription.priority = json_int_field(root, "priority", 0);
        subscription.preferred_layer =
            parse_video_layer(json_string_field(root, "preferred_layer"));
        subscription.target_layer =
            parse_video_layer(json_string_field(root, "target_layer"));
        subscription.muted = json_bool_field(root, "muted", 0);
        if (json_string_field(root, "policy_source")) {
            strncpy(subscription.policy_source, json_string_field(root, "policy_source"),
                    sizeof(subscription.policy_source) - 1);
        }
        subscription.max_layer = parse_video_layer(json_string_field(root, "max_layer"));
        if (subscription.target_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
            subscription.max_layer = subscription.target_layer;
        } else if (subscription.preferred_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
            subscription.max_layer = subscription.preferred_layer;
        }

        /* The app server owns desired subscription state. It accepts a
           subscription before the WHEP receiver exists and applies it to the
           core SFU when that receiver session is created. */
        rc = sfu_node_app_server_apply_track_subscription(
            server, room_id, &subscription);
    } else if (strcmp(type, "start_recording") == 0) {
        const char *recording_id = json_string_field(root, "recording_id");
        const char *mode = json_string_field(root, "mode");
        char *json;

        if (!room_id || !recording_id || !mode) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "missing room_id, recording_id or mode");
            json_free(root);
            root = NULL;
            return;
        }

        rc = sfu_node_app_server_start_recording(server, room_id,
                                                 recording_id, mode);
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            json_free(root);
            root = NULL;
            return;
        }

        json = recording_status_json(server, room_id);
        if (!json) {
            send_error_json(res, 500, "COMMAND_FAILED", "recording start incomplete");
            json_free(root);
            root = NULL;
            return;
        }

        send_entity_ok_json(res, "recording_status", json);
        json_free(root);
        root = NULL;
        return;
    } else if (strcmp(type, "stop_recording") == 0) {
        char *json;

        if (!room_id) {
            send_error_json(res, 400, "INVALID_REQUEST", "missing room_id");
            json_free(root);
            root = NULL;
            return;
        }

        rc = sfu_node_app_server_stop_recording(server, room_id);
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            json_free(root);
            root = NULL;
            return;
        }

        json = recording_status_json(server, room_id);
        if (!json) {
            send_error_json(res, 500, "COMMAND_FAILED", "recording stop incomplete");
            json_free(root);
            root = NULL;
            return;
        }

        send_entity_ok_json(res, "recording_status", json);
        json_free(root);
        root = NULL;
        return;
    } else if (strcmp(type, "get_recording_status") == 0) {
        char *json;

        if (!room_id) {
            send_error_json(res, 400, "INVALID_REQUEST", "missing room_id");
            json_free(root);
            root = NULL;
            return;
        }

        json = recording_status_json(server, room_id);
        if (!json) {
            send_error_json(res, 404, "RECORDING_NOT_FOUND", "recording not found");
            json_free(root);
            root = NULL;
            return;
        }

        send_entity_ok_json(res, "recording_status", json);
        json_free(root);
        root = NULL;
        return;
    } else if (strcmp(type, "get_room_stats") == 0) {
        char *json = room_stats_json(node, room_id);

        if (!json) {
            send_error_json(res, 404, "ROOM_NOT_FOUND", "room not found");
            json_free(root);
            root = NULL;
            return;
        }

        send_entity_ok_json(res, "room_stats", json);
        json_free(root);
        root = NULL;
        return;
    } else if (strcmp(type, "get_participant_stats") == 0) {
        char *json = participant_stats_json(node, room_id,
                                            json_string_field(root, "participant_id"));

        if (!json) {
            send_error_json(res, 404, "PARTICIPANT_NOT_FOUND", "participant not found");
            json_free(root);
            root = NULL;
            return;
        }

        send_entity_ok_json(res, "participant_stats", json);
        json_free(root);
        root = NULL;
        return;
    } else if (strcmp(type, "get_track_subscription") == 0) {
        char *json = track_subscription_json(
            node, room_id, json_string_field(root, "receiver_participant_id"),
            json_string_field(root, "track_id"));

        if (!json) {
            send_error_json(res, 404, "TRACK_SUBSCRIPTION_NOT_FOUND",
                            "track subscription not found");
            json_free(root);
            root = NULL;
            return;
        }

        send_entity_ok_json(res, "track_subscription", json);
        json_free(root);
        root = NULL;
        return;
    } else if (strcmp(type, "get_webrtc_session") == 0) {
        char *json = webrtc_session_json(server, room_id,
                                         json_string_field(root, "session_id"));

        if (!json) {
            send_error_json(res, 404, "WEBRTC_SESSION_NOT_FOUND",
                            "webrtc session not found");
            json_free(root);
            root = NULL;
            return;
        }

        send_entity_ok_json(res, "webrtc_session", json);
        json_free(root);
        root = NULL;
        return;
    } else if (strcmp(type, "get_node_stats") == 0) {
        char *json = node_stats_json(server, node);

        if (!json) {
            send_error_json(res, 500, "SERVER_NOT_READY", "sfu node unavailable");
            json_free(root);
            root = NULL;
            return;
        }

        send_entity_ok_json(res, "node_stats", json);
        json_free(root);
        root = NULL;
        return;
    } else {
        send_error_json(res, 400, "UNKNOWN_COMMAND", "unsupported command type");
        json_free(root);
        root = NULL;
        return;
    }

    if (rc != 0) {
        send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
        json_free(root);
        root = NULL;
        return;
    }

    if (room_id && strcmp(type, "detach_room") != 0 &&
        strcmp(type, "force_close_room") != 0) {
        char *json = room_stats_json(node, room_id);
        if (json) {
            send_entity_ok_json(res, "room_stats", json);
            json_free(root);
            root = NULL;
            return;
        }
    }

    send_json(res, 200, "{\"ok\":true}");
    json_free(root);
    root = NULL;
}

typedef void (*sfu_node_http_handler_fn)(Req *req, Res *res);

static int sfu_node_http_dispatch(
    void *user, const chttp_server_request_view *request,
    chttp_server_response *response, sfu_node_http_handler_fn handler) {
    sfu_node_http_api_t *api = (sfu_node_http_api_t *)user;
    Req req = {.request = request,
               .server = api ? api->server : NULL,
               .path = request ? request->path : NULL,
               .body = request ? request->body : NULL,
               .body_len = request ? request->body_size : 0u};
    Res res = {.response = response, .status = SALTS_OK};
    if (!api || !request || !response || !handler) {
        return SALTS_EINVAL;
    }
    handler(&req, &res);
    return res.status;
}

#define SFU_NODE_HTTP_ROUTE_ADAPTER(name)                                  \
    static int name##_route(void *user,                                    \
                            const chttp_server_request_view *request,       \
                            chttp_server_response *response) {              \
        return sfu_node_http_dispatch(user, request, response, name);       \
    }

SFU_NODE_HTTP_ROUTE_ADAPTER(handle_health)
SFU_NODE_HTTP_ROUTE_ADAPTER(handle_ready)
SFU_NODE_HTTP_ROUTE_ADAPTER(handle_metrics)
SFU_NODE_HTTP_ROUTE_ADAPTER(handle_get_webrtc_session)
SFU_NODE_HTTP_ROUTE_ADAPTER(handle_command)
SFU_NODE_HTTP_ROUTE_ADAPTER(handle_whip_post)
SFU_NODE_HTTP_ROUTE_ADAPTER(handle_whep_post)
SFU_NODE_HTTP_ROUTE_ADAPTER(handle_media_session_patch)
SFU_NODE_HTTP_ROUTE_ADAPTER(handle_media_session_delete)

static int handle_options_route(void *user,
                                const chttp_server_request_view *request,
                                chttp_server_response *response) {
    Res res = {.response = response, .status = SALTS_OK};
    (void)user;
    (void)request;
    reply(&res, NO_CONTENT, NULL, NULL, 0u);
    return res.status;
}

static native_io_backend_kind sfu_node_http_backend(void) {
#if defined(_WIN32)
    return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
    return NATIVE_IO_BACKEND_EPOLL;
#else
    return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static cnet_client_config sfu_node_http_network_config(int use_tls) {
    const cnet_client_config config = {
        .backend = sfu_node_http_backend(),
        .connection_capacity = SFU_NODE_HTTP_CONNECTION_CAPACITY,
        .command_capacity = SFU_NODE_HTTP_COMMAND_CAPACITY,
        .request_capacity = SFU_NODE_HTTP_REQUEST_CAPACITY,
        .completion_batch_capacity = SFU_NODE_HTTP_COMPLETION_CAPACITY,
        .event_capacity = SFU_NODE_HTTP_EVENT_CAPACITY,
        .max_send_bytes = SFU_NODE_HTTP_MAX_SEND_BYTES,
        .receive_buffer_bytes = SFU_NODE_HTTP_RECEIVE_BUFFER_BYTES,
        .connect_timeout_ms = SFU_NODE_HTTP_TIMEOUT_MS,
        .read_timeout_ms = SFU_NODE_HTTP_TIMEOUT_MS,
        .write_timeout_ms = SFU_NODE_HTTP_TIMEOUT_MS,
        .tls_io_buffer_bytes = use_tls
                                   ? SFU_NODE_HTTP_TLS_IO_BUFFER_BYTES
                                   : 0u,
        .tls_handshake_timeout_ms = use_tls
                                        ? SFU_NODE_HTTP_TIMEOUT_MS
                                        : 0u};
    return config;
}

static chttp_server_config sfu_node_http_config(
    const char *host, uint16_t port, const cnet_tls_server_config *tls) {
    const chttp_server_config config = {
        .host = host,
        .port = port,
        .backlog = SFU_NODE_HTTP_CONNECTION_CAPACITY,
        .network = sfu_node_http_network_config(tls != NULL),
        .route_capacity = SFU_NODE_HTTP_ROUTE_CAPACITY,
        .max_route_param_count = 4u,
        .max_route_param_bytes = 512u,
        .max_target_bytes = SFU_NODE_HTTP_MAX_TARGET_BYTES,
        .max_header_count = SFU_NODE_HTTP_MAX_HEADER_COUNT,
        .max_header_bytes = SFU_NODE_HTTP_MAX_HEADER_BYTES,
        .max_request_body_bytes = SFU_NODE_HTTP_MAX_REQUEST_BODY_BYTES,
        .max_response_header_count = SFU_NODE_HTTP_MAX_HEADER_COUNT,
        .max_response_header_bytes = SFU_NODE_HTTP_MAX_HEADER_BYTES,
        .max_response_body_bytes = SFU_NODE_HTTP_MAX_RESPONSE_BODY_BYTES,
        .poll_slice_ms = SFU_NODE_HTTP_POLL_SLICE_MS,
        .tls = tls,
        .buffer_capacity_bytes = SFU_NODE_HTTP_BUFFER_CAPACITY_BYTES};
    return config;
}

static int sfu_node_http_register_routes(sfu_node_http_api_t *api) {
    static const char *const option_paths[] = {
        "/health",
        "/ready",
        "/metrics",
        "/api/v1/rooms/:room_id/webrtc_sessions/:session_id",
        "/api/v1/commands",
        "/whip/:room_id/:participant_id",
        "/whep/:room_id/:participant_id",
        "/whip/:room_id/:participant_id/sessions/:session_id",
        "/whep/:room_id/:participant_id/sessions/:session_id"};
    int status = chttp_server_get(&api->http, "/health",
                                  handle_health_route, api);
    if (status == SALTS_OK) {
        status = chttp_server_get(&api->http, "/ready", handle_ready_route,
                                  api);
    }
    if (status == SALTS_OK) {
        status = chttp_server_get(&api->http, "/metrics",
                                  handle_metrics_route, api);
    }
    if (status == SALTS_OK) {
        status = chttp_server_get(
            &api->http,
            "/api/v1/rooms/:room_id/webrtc_sessions/:session_id",
            handle_get_webrtc_session_route, api);
    }
    if (status == SALTS_OK) {
        status = chttp_server_post(&api->http, "/api/v1/commands",
                                   handle_command_route, api);
    }
    if (status == SALTS_OK) {
        status = chttp_server_post(&api->http,
                                   "/whip/:room_id/:participant_id",
                                   handle_whip_post_route, api);
    }
    if (status == SALTS_OK) {
        status = chttp_server_post(&api->http,
                                   "/whep/:room_id/:participant_id",
                                   handle_whep_post_route, api);
    }
    if (status == SALTS_OK) {
        status = chttp_server_patch(
            &api->http,
            "/whip/:room_id/:participant_id/sessions/:session_id",
            handle_media_session_patch_route, api);
    }
    if (status == SALTS_OK) {
        status = chttp_server_patch(
            &api->http,
            "/whep/:room_id/:participant_id/sessions/:session_id",
            handle_media_session_patch_route, api);
    }
    if (status == SALTS_OK) {
        status = chttp_server_delete(
            &api->http,
            "/whip/:room_id/:participant_id/sessions/:session_id",
            handle_media_session_delete_route, api);
    }
    if (status == SALTS_OK) {
        status = chttp_server_delete(
            &api->http,
            "/whep/:room_id/:participant_id/sessions/:session_id",
            handle_media_session_delete_route, api);
    }
    for (size_t index = 0u;
         status == SALTS_OK &&
         index < sizeof(option_paths) / sizeof(option_paths[0]);
         ++index) {
        status = chttp_server_options(&api->http, option_paths[index],
                                      handle_options_route, api);
    }
    return status;
}

sfu_node_http_api_t *sfu_node_http_api_create(sfu_node_app_server_t *server) {
    sfu_node_http_api_t *api;

    if (!server) {
        return NULL;
    }

    api = (sfu_node_http_api_t *)calloc(1, sizeof(*api));
    if (!api) {
        return NULL;
    }

    salts_mutex_init(&api->lifecycle_mutex);
    api->server = server;
    api->state = SFU_NODE_HTTP_STOPPED;
    return api;
}

int sfu_node_http_api_start(sfu_node_http_api_t *api, const char *host, int port) {
    const sfu_node_app_config_t *app_config;
    cnet_tls_server_config tls;
    chttp_server_config config;
    int status;
    if (!api || !host || host[0] == '\0' || port <= 0 || port > UINT16_MAX) {
        return -1;
    }
    if (sfu_node_app_server_ensure_webrtc_worker(api->server) != 0) {
        return -1;
    }

    salts_mutex_lock(&api->lifecycle_mutex);
    if (api->state != SFU_NODE_HTTP_STOPPED || api->http_initialized) {
        salts_mutex_unlock(&api->lifecycle_mutex);
        return -1;
    }
    api->state = SFU_NODE_HTTP_STARTING;
    salts_mutex_unlock(&api->lifecycle_mutex);

    app_config = sfu_node_app_server_get_config(api->server);
    memset(&tls, 0, sizeof(tls));
    if (app_config && app_config->use_tls) {
        tls.size = sizeof(tls);
        tls.cert_file = app_config->tls_cert_file;
        tls.key_file = app_config->tls_key_file;
        tls.client_auth = CNET_TLS_CLIENT_AUTH_NONE;
    }
    config = sfu_node_http_config(
        host, (uint16_t)port,
        app_config && app_config->use_tls ? &tls : NULL);
    status = chttp_server_init(&api->http, &config);
    if (status == SALTS_OK) {
        salts_mutex_lock(&api->lifecycle_mutex);
        api->http_initialized = 1;
        salts_mutex_unlock(&api->lifecycle_mutex);
        status = sfu_node_http_register_routes(api);
    }
    if (status == SALTS_OK) {
        status = chttp_server_start(&api->http);
    }
    if (status != SALTS_OK) {
        (void)chttp_server_destroy(&api->http);
        salts_mutex_lock(&api->lifecycle_mutex);
        api->http_initialized = 0;
        api->state = SFU_NODE_HTTP_STOPPED;
        salts_mutex_unlock(&api->lifecycle_mutex);
        return -1;
    }
    salts_mutex_lock(&api->lifecycle_mutex);
    api->state = SFU_NODE_HTTP_RUNNING;
    salts_mutex_unlock(&api->lifecycle_mutex);
    return 0;
}

void sfu_node_http_api_stop(sfu_node_http_api_t *api) {
    int initialized;

    if (!api) {
        return;
    }

    salts_mutex_lock(&api->lifecycle_mutex);
    if (api->state == SFU_NODE_HTTP_STOPPED) {
        salts_mutex_unlock(&api->lifecycle_mutex);
        return;
    }
    api->state = SFU_NODE_HTTP_STOPPING;
    initialized = api->http_initialized;
    salts_mutex_unlock(&api->lifecycle_mutex);

    if (initialized) {
        (void)chttp_server_stop(&api->http, 0u);
        (void)chttp_server_destroy(&api->http);
    }
    salts_mutex_lock(&api->lifecycle_mutex);
    api->http_initialized = 0;
    api->state = SFU_NODE_HTTP_STOPPED;
    salts_mutex_unlock(&api->lifecycle_mutex);
}

void sfu_node_http_api_destroy(sfu_node_http_api_t *api) {
    if (!api) {
        return;
    }

    sfu_node_http_api_stop(api);
    salts_mutex_destroy(&api->lifecycle_mutex);
    free(api);
}
