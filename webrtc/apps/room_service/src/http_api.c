#include "room_service/http_api.h"
#include "turbo_media_auth.h"
#include <iris/async.h>
#include <iris/iris_app.h>
#include <iris/server.h>
#include <iris/router.h>
#include <platform.h>
#include <turbo_coro_context.h>
#include <turbo_coro_socket.h>
#include <turbo_parser.h>
#include <turbo_thread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define ROOM_SERVICE_CONTROL_AUDIENCE "turbomedia-room-control"
#define ROOM_SERVICE_SCOPE_CONTROL_WRITE "room.control.write"
#define ROOM_SERVICE_SCOPE_CONTROL_DANGEROUS "room.control.dangerous"

static room_service_app_server_t *g_room_service_server = NULL;

struct room_service_http_api_s {
    iris_app_t *app;
    room_service_app_server_t *server;
    turbo_thread_t thread;
    int thread_started;
    turbo_mutex_t lifecycle_mutex;
    turbo_cond_t lifecycle_cond;
    coro_context_t *ctx;
    coro_socket_t *listener;
    int state;
    const char *host;
    int port;
};

typedef enum room_service_http_state_e {
    ROOM_SERVICE_HTTP_STOPPED = 0,
    ROOM_SERVICE_HTTP_STARTING,
    ROOM_SERVICE_HTTP_RUNNING,
    ROOM_SERVICE_HTTP_STOPPING,
    ROOM_SERVICE_HTTP_FAILED
} room_service_http_state_t;

typedef enum room_service_command_access_e {
    ROOM_SERVICE_COMMAND_ACCESS_READ = 0,
    ROOM_SERVICE_COMMAND_ACCESS_WRITE,
    ROOM_SERVICE_COMMAND_ACCESS_DANGEROUS
} room_service_command_access_t;

static void send_error_json(Res *res, int status, const char *code, const char *message);
static char *room_sync_diagnostic_json(room_service_app_server_t *server,
                                       const char *room_id);
static char *room_sync_view_json(room_service_app_server_t *server,
                                 turbo_room_service_t *service,
                                 const char *room_id);
static char *room_state_json(room_service_app_server_t *server,
                             turbo_room_service_t *service,
                             const char *room_id);
static char *conference_policy_json(room_service_app_server_t *server,
                                    const char *room_id);

#ifdef ROOM_SERVICE_ENABLE_TEST_HOOKS
typedef enum {
    ROOM_SERVICE_ROUTE_TEST_FAILURE_NONE = 0,
    ROOM_SERVICE_ROUTE_TEST_FAILURE_AFTER_ROOM_CREATE,
    ROOM_SERVICE_ROUTE_TEST_FAILURE_AFTER_CALLER_JOIN,
    ROOM_SERVICE_ROUTE_TEST_FAILURE_AFTER_CALLEE_JOIN
} room_service_route_test_failure_stage_t;

static int parse_route_test_failure_stage(
    const char *value, room_service_route_test_failure_stage_t *stage) {
    if (!stage) {
        return -1;
    }

    *stage = ROOM_SERVICE_ROUTE_TEST_FAILURE_NONE;
    if (!value || !value[0]) {
        return 0;
    }
    if (strcmp(value, "after_room_create") == 0) {
        *stage = ROOM_SERVICE_ROUTE_TEST_FAILURE_AFTER_ROOM_CREATE;
        return 0;
    }
    if (strcmp(value, "after_caller_join") == 0) {
        *stage = ROOM_SERVICE_ROUTE_TEST_FAILURE_AFTER_CALLER_JOIN;
        return 0;
    }
    if (strcmp(value, "after_callee_join") == 0) {
        *stage = ROOM_SERVICE_ROUTE_TEST_FAILURE_AFTER_CALLEE_JOIN;
        return 0;
    }

    return -1;
}
#endif

static const char *json_string_field(const json_value_t *obj, const char *key) {
    json_value_t *value;

    if (!obj || !key) {
        return NULL;
    }

    value = turbo_json_object_get(obj, key);
    if (!value || turbo_json_type(value) != TURBO_JSON_STRING) {
        return NULL;
    }

    return turbo_json_string(value);
}

static int control_auth_enabled(const room_service_app_config_t *config) {
    return config &&
           ((config->control_token && config->control_token[0] != '\0') ||
            (config->auth_active_secret &&
             config->auth_active_secret[0] != '\0'));
}

static turbo_media_auth_config_t signed_auth_config(
    const room_service_app_config_t *config) {
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
    const Req *req, const room_service_app_config_t *config,
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
    policy.audience = ROOM_SERVICE_CONTROL_AUDIENCE;
    policy.required_scope = required_scope;
    policy.room_id = room_id;
    policy.participant_id = participant_id;
    return turbo_media_auth_authorize(
               authorization, config->control_token, &auth, &policy) !=
           TURBO_MEDIA_AUTH_DENIED;
}

static room_service_command_access_t command_access_for_type(const char *type) {
    if (!type) {
        return ROOM_SERVICE_COMMAND_ACCESS_WRITE;
    }

    if (strcmp(type, "get_call_center_agent_state") == 0 ||
        strcmp(type, "get_call_center_room") == 0 ||
        strcmp(type, "get_call_center_events") == 0 ||
        strcmp(type, "get_call_center_queue_depth") == 0 ||
        strcmp(type, "get_participant") == 0 ||
        strcmp(type, "get_participant_bandwidth_diagnostic") == 0 ||
        strcmp(type, "get_room_diagnostic") == 0 ||
        strcmp(type, "get_room_state") == 0 ||
        strcmp(type, "get_room_sync_diagnostic") == 0 ||
        strcmp(type, "get_track") == 0 ||
        strcmp(type, "get_subscription") == 0 ||
        strcmp(type, "get_subscription_diagnostic") == 0 ||
        strcmp(type, "get_conference_policy") == 0 ||
        strcmp(type, "peek_call_center_queue") == 0) {
        return ROOM_SERVICE_COMMAND_ACCESS_READ;
    }

    if (strcmp(type, "pop_call_center_queue") == 0 ||
        strcmp(type, "complete_call_center_transfer") == 0 ||
        strcmp(type, "finalize_call_center_room") == 0 ||
        strcmp(type, "start_recording") == 0 ||
        strcmp(type, "stop_recording") == 0 ||
        strcmp(type, "close_room") == 0) {
        return ROOM_SERVICE_COMMAND_ACCESS_DANGEROUS;
    }

    return ROOM_SERVICE_COMMAND_ACCESS_WRITE;
}

static int command_requires_control_auth(const char *type) {
    return command_access_for_type(type) != ROOM_SERVICE_COMMAND_ACCESS_READ;
}

static int require_control_auth(
    Req *req, Res *res, const char *required_scope,
    const char *room_id, const char *participant_id) {
    const room_service_app_config_t *config =
        room_service_app_server_get_config(g_room_service_server);

    if (request_has_control_auth(req, config, required_scope, room_id,
                                 participant_id)) {
        return 0;
    }

    set_header(res, "WWW-Authenticate", "Bearer");
    send_error_json(res, 401, "UNAUTHORIZED", "control token required");
    return -1;
}

static const char *path_tail(const char *path) {
    const char *tail;

    if (!path || !*path) {
        return NULL;
    }

    tail = strrchr(path, '/');
    if (!tail || !tail[1]) {
        return NULL;
    }

    return tail + 1;
}

static int parse_subscription_diagnostic_path(const char *path, char *room_id,
                                              size_t room_id_size,
                                              char *subscriber_participant_id,
                                              size_t subscriber_size, char *track_id,
                                              size_t track_id_size) {
    const char *prefix = "/api/v1/rooms/";
    const char *room_start;
    const char *room_end;
    const char *subscriber_start;
    const char *subscriber_end;
    const char *track_start;
    size_t len;

    if (!path || !room_id || !subscriber_participant_id || !track_id) {
        return -1;
    }

    room_id[0] = '\0';
    subscriber_participant_id[0] = '\0';
    track_id[0] = '\0';

    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    room_start = path + strlen(prefix);
    room_end = strchr(room_start, '/');
    if (!room_end || strncmp(room_end, "/subscription_diagnostic/", 25) != 0) {
        return -1;
    }

    len = (size_t)(room_end - room_start);
    if (len == 0 || len >= room_id_size) {
        return -1;
    }
    memcpy(room_id, room_start, len);
    room_id[len] = '\0';

    subscriber_start = room_end + 25;
    subscriber_end = strchr(subscriber_start, '/');
    if (!subscriber_end) {
        return -1;
    }

    len = (size_t)(subscriber_end - subscriber_start);
    if (len == 0 || len >= subscriber_size) {
        return -1;
    }
    memcpy(subscriber_participant_id, subscriber_start, len);
    subscriber_participant_id[len] = '\0';

    track_start = subscriber_end + 1;
    if (!*track_start) {
        return -1;
    }

    len = strlen(track_start);
    if (len >= track_id_size) {
        return -1;
    }
    memcpy(track_id, track_start, len + 1);
    return 0;
}

static int json_bool_field(const json_value_t *obj, const char *key, int def) {
    json_value_t *value;

    if (!obj || !key) {
        return def;
    }

    value = turbo_json_object_get(obj, key);
    if (!value || turbo_json_type(value) != TURBO_JSON_BOOL) {
        return def;
    }

    return turbo_json_bool(value) ? 1 : 0;
}

static int json_uint32_field(const json_value_t *obj, const char *key,
                             uint32_t *out_value) {
    json_value_t *value;
    double number;

    if (!obj || !key || !out_value) {
        return -1;
    }

    value = turbo_json_object_get(obj, key);
    if (!value || turbo_json_type(value) != TURBO_JSON_NUMBER) {
        return -1;
    }

    number = turbo_json_number(value);
    if (number < 0 || number > 4294967295.0) {
        return -1;
    }

    *out_value = (uint32_t)number;
    return 0;
}

static int json_uint32_array_field(const json_value_t *obj, const char *key,
                                   uint32_t *values, int max_values, int *out_count) {
    json_value_t *array;
    size_t count;
    size_t i;

    if (!obj || !key || !values || max_values <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;
    memset(values, 0, sizeof(uint32_t) * (size_t)max_values);

    array = turbo_json_object_get(obj, key);
    if (!array) {
        return 0;
    }
    if (turbo_json_type(array) != TURBO_JSON_ARRAY) {
        return -1;
    }

    count = turbo_json_array_size(array);
    if ((int)count > max_values) {
        count = (size_t)max_values;
    }

    for (i = 0; i < count; ++i) {
        json_value_t *item = turbo_json_array_get(array, i);
        double number;

        if (!item || turbo_json_type(item) != TURBO_JSON_NUMBER) {
            return -1;
        }

        number = turbo_json_number(item);
        if (number < 0 || number > 4294967295.0) {
            return -1;
        }

        values[i] = (uint32_t)number;
    }

    *out_count = (int)count;
    return 0;
}

static int append_json_fragment(char **buffer, size_t *length, size_t *capacity,
                                const char *fragment) {
    size_t fragment_len;
    size_t required;
    char *new_buffer;

    if (!buffer || !length || !capacity || !fragment) {
        return -1;
    }

    fragment_len = strlen(fragment);
    required = *length + fragment_len + 1;
    if (required > *capacity) {
        size_t new_capacity = *capacity > 0 ? *capacity : 256;
        while (new_capacity < required) {
            new_capacity *= 2;
        }
        new_buffer = (char *)realloc(*buffer, new_capacity);
        if (!new_buffer) {
            return -1;
        }
        *buffer = new_buffer;
        *capacity = new_capacity;
    }

    memcpy(*buffer + *length, fragment, fragment_len + 1);
    *length += fragment_len;
    return 0;
}

static turbo_room_type_t parse_room_type(const char *value) {
    if (!value || strcmp(value, "conference") == 0) {
        return TURBO_ROOM_TYPE_CONFERENCE;
    }
    if (strcmp(value, "call") == 0) {
        return TURBO_ROOM_TYPE_CALL;
    }
    if (strcmp(value, "webinar") == 0) {
        return TURBO_ROOM_TYPE_WEBINAR;
    }
    return 0;
}

static turbo_participant_role_t parse_role(const char *value) {
    if (!value || strcmp(value, "guest") == 0) {
        return TURBO_PARTICIPANT_ROLE_GUEST;
    }
    if (strcmp(value, "host") == 0) {
        return TURBO_PARTICIPANT_ROLE_HOST;
    }
    if (strcmp(value, "cohost") == 0) {
        return TURBO_PARTICIPANT_ROLE_COHOST;
    }
    if (strcmp(value, "audience") == 0) {
        return TURBO_PARTICIPANT_ROLE_AUDIENCE;
    }
    if (strcmp(value, "customer") == 0) {
        return TURBO_PARTICIPANT_ROLE_CUSTOMER;
    }
    if (strcmp(value, "agent") == 0) {
        return TURBO_PARTICIPANT_ROLE_AGENT;
    }
    if (strcmp(value, "supervisor") == 0) {
        return TURBO_PARTICIPANT_ROLE_SUPERVISOR;
    }
    if (strcmp(value, "qa_observer") == 0 || strcmp(value, "qa") == 0) {
        return TURBO_PARTICIPANT_ROLE_QA_OBSERVER;
    }
    if (strcmp(value, "bot") == 0) {
        return TURBO_PARTICIPANT_ROLE_BOT;
    }
    return 0;
}

static turbo_call_center_supervisor_mode_t parse_call_center_supervisor_mode(
    const char *value) {
    if (!value || strcmp(value, "none") == 0) {
        return TURBO_CALL_CENTER_SUPERVISOR_NONE;
    }
    if (strcmp(value, "monitor") == 0) {
        return TURBO_CALL_CENTER_SUPERVISOR_MONITOR;
    }
    if (strcmp(value, "whisper") == 0) {
        return TURBO_CALL_CENTER_SUPERVISOR_WHISPER;
    }
    if (strcmp(value, "barge") == 0) {
        return TURBO_CALL_CENTER_SUPERVISOR_BARGE;
    }
    return -1;
}

static const char *call_center_supervisor_mode_name(
    turbo_call_center_supervisor_mode_t mode) {
    switch (mode) {
        case TURBO_CALL_CENTER_SUPERVISOR_MONITOR: return "monitor";
        case TURBO_CALL_CENTER_SUPERVISOR_WHISPER: return "whisper";
        case TURBO_CALL_CENTER_SUPERVISOR_BARGE: return "barge";
        case TURBO_CALL_CENTER_SUPERVISOR_NONE:
        default: return "none";
    }
}

static turbo_call_center_queue_side_t parse_call_center_queue_side(const char *value) {
    if (!value) {
        return 0;
    }
    if (strcmp(value, "caller") == 0 || strcmp(value, "customer") == 0) {
        return TURBO_CALL_CENTER_QUEUE_CALLER;
    }
    if (strcmp(value, "callee") == 0 || strcmp(value, "agent") == 0) {
        return TURBO_CALL_CENTER_QUEUE_CALLEE;
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

static turbo_room_track_source_t parse_track_source(const char *value) {
    if (!value) {
        return 0;
    }
    if (strcmp(value, "mic") == 0) {
        return TURBO_ROOM_SOURCE_MIC;
    }
    if (strcmp(value, "camera") == 0) {
        return TURBO_ROOM_SOURCE_CAMERA;
    }
    if (strcmp(value, "screen") == 0) {
        return TURBO_ROOM_SOURCE_SCREEN;
    }
    return 0;
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

static turbo_participant_session_state_t parse_session_state(const char *value) {
    if (!value) {
        return 0;
    }
    if (strcmp(value, "none") == 0) {
        return TURBO_PARTICIPANT_SESSION_NONE;
    }
    if (strcmp(value, "negotiating") == 0) {
        return TURBO_PARTICIPANT_SESSION_NEGOTIATING;
    }
    if (strcmp(value, "connected") == 0) {
        return TURBO_PARTICIPANT_SESSION_CONNECTED;
    }
    if (strcmp(value, "reconnecting") == 0) {
        return TURBO_PARTICIPANT_SESSION_RECONNECTING;
    }
    if (strcmp(value, "failed") == 0) {
        return TURBO_PARTICIPANT_SESSION_FAILED;
    }
    if (strcmp(value, "closed") == 0) {
        return TURBO_PARTICIPANT_SESSION_CLOSED;
    }
    return 0;
}

static turbo_call_center_agent_state_t parse_call_center_agent_state(const char *value) {
    if (!value) {
        return 0;
    }
    if (strcmp(value, "available") == 0) {
        return TURBO_CALL_CENTER_AGENT_AVAILABLE;
    }
    if (strcmp(value, "reserved") == 0) {
        return TURBO_CALL_CENTER_AGENT_RESERVED;
    }
    if (strcmp(value, "busy") == 0) {
        return TURBO_CALL_CENTER_AGENT_BUSY;
    }
    if (strcmp(value, "offline") == 0) {
        return TURBO_CALL_CENTER_AGENT_OFFLINE;
    }
    if (strcmp(value, "wrap_up") == 0 || strcmp(value, "wrapup") == 0) {
        return TURBO_CALL_CENTER_AGENT_WRAP_UP;
    }
    return 0;
}

static turbo_call_center_room_state_t parse_call_center_room_state(const char *value) {
    if (!value) {
        return 0;
    }
    if (strcmp(value, "active") == 0) {
        return TURBO_CALL_CENTER_ROOM_ACTIVE;
    }
    if (strcmp(value, "wrap_up") == 0 || strcmp(value, "wrapup") == 0) {
        return TURBO_CALL_CENTER_ROOM_WRAP_UP;
    }
    if (strcmp(value, "completed") == 0) {
        return TURBO_CALL_CENTER_ROOM_COMPLETED;
    }
    return 0;
}

static const char *room_status_name(turbo_room_status_t status) {
    switch (status) {
        case TURBO_ROOM_STATUS_OPEN: return "open";
        case TURBO_ROOM_STATUS_ACTIVE: return "active";
        case TURBO_ROOM_STATUS_CLOSING: return "closing";
        case TURBO_ROOM_STATUS_CLOSED: return "closed";
        default: return "unknown";
    }
}

static const char *recording_state_name(turbo_room_recording_state_t state) {
    switch (state) {
        case TURBO_ROOM_RECORDING_STOPPED: return "stopped";
        case TURBO_ROOM_RECORDING_STARTING: return "starting";
        case TURBO_ROOM_RECORDING_ACTIVE: return "active";
        case TURBO_ROOM_RECORDING_STOPPING: return "stopping";
        case TURBO_ROOM_RECORDING_FAILED: return "failed";
        default: return "unknown";
    }
}

static const char *participant_role_name(turbo_participant_role_t role) {
    switch (role) {
        case TURBO_PARTICIPANT_ROLE_HOST: return "host";
        case TURBO_PARTICIPANT_ROLE_COHOST: return "cohost";
        case TURBO_PARTICIPANT_ROLE_GUEST: return "guest";
        case TURBO_PARTICIPANT_ROLE_AUDIENCE: return "audience";
        case TURBO_PARTICIPANT_ROLE_CUSTOMER: return "customer";
        case TURBO_PARTICIPANT_ROLE_AGENT: return "agent";
        case TURBO_PARTICIPANT_ROLE_SUPERVISOR: return "supervisor";
        case TURBO_PARTICIPANT_ROLE_QA_OBSERVER: return "qa_observer";
        case TURBO_PARTICIPANT_ROLE_BOT: return "bot";
        default: return "unknown";
    }
}

static const char *participant_session_state_name(turbo_participant_session_state_t state) {
    switch (state) {
        case TURBO_PARTICIPANT_SESSION_NONE: return "none";
        case TURBO_PARTICIPANT_SESSION_NEGOTIATING: return "negotiating";
        case TURBO_PARTICIPANT_SESSION_CONNECTED: return "connected";
        case TURBO_PARTICIPANT_SESSION_RECONNECTING: return "reconnecting";
        case TURBO_PARTICIPANT_SESSION_FAILED: return "failed";
        case TURBO_PARTICIPANT_SESSION_CLOSED: return "closed";
        default: return "unknown";
    }
}

static const char *call_center_queue_side_name(turbo_call_center_queue_side_t side) {
    switch (side) {
        case TURBO_CALL_CENTER_QUEUE_CALLER: return "caller";
        case TURBO_CALL_CENTER_QUEUE_CALLEE: return "callee";
        default: return "unknown";
    }
}

static const char *call_center_agent_state_name(turbo_call_center_agent_state_t state) {
    switch (state) {
        case TURBO_CALL_CENTER_AGENT_AVAILABLE: return "available";
        case TURBO_CALL_CENTER_AGENT_RESERVED: return "reserved";
        case TURBO_CALL_CENTER_AGENT_BUSY: return "busy";
        case TURBO_CALL_CENTER_AGENT_OFFLINE: return "offline";
        case TURBO_CALL_CENTER_AGENT_WRAP_UP: return "wrap_up";
        default: return "unknown";
    }
}

static const char *call_center_room_state_name(turbo_call_center_room_state_t state) {
    switch (state) {
        case TURBO_CALL_CENTER_ROOM_ACTIVE: return "active";
        case TURBO_CALL_CENTER_ROOM_WRAP_UP: return "wrap_up";
        case TURBO_CALL_CENTER_ROOM_COMPLETED: return "completed";
        default: return "unknown";
    }
}

static const char *track_kind_name(turbo_room_track_kind_t kind) {
    switch (kind) {
        case TURBO_ROOM_TRACK_AUDIO: return "audio";
        case TURBO_ROOM_TRACK_VIDEO: return "video";
        default: return "unknown";
    }
}

static const char *track_source_name(turbo_room_track_source_t source) {
    switch (source) {
        case TURBO_ROOM_SOURCE_MIC: return "mic";
        case TURBO_ROOM_SOURCE_CAMERA: return "camera";
        case TURBO_ROOM_SOURCE_SCREEN: return "screen";
        default: return "unknown";
    }
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

static turbo_room_video_layer_t resolve_subscription_max_layer(
    const turbo_room_subscription_summary_t *summary) {
    if (!summary || !summary->enabled || summary->muted) {
        return TURBO_ROOM_VIDEO_LAYER_NONE;
    }
    if (summary->target_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
        return summary->target_layer;
    }
    if (summary->preferred_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
        return summary->preferred_layer;
    }
    return TURBO_ROOM_VIDEO_LAYER_HIGH;
}

static char *conference_policy_json(room_service_app_server_t *server,
                                    const char *room_id) {
    room_service_conference_policy_t policy;
    char *json;

    if (!server || !room_id ||
        room_service_app_server_get_conference_policy(server, room_id, &policy) != 0 ||
        !policy.available) {
        return NULL;
    }

    json = (char *)malloc(512);
    if (!json) {
        return NULL;
    }

    snprintf(json, 512,
             "{"
             "\"room_id\":\"%s\","
             "\"layout_mode\":\"%s\","
             "\"active_speaker_participant_id\":\"%s\","
             "\"pinned_participant_id\":\"%s\","
             "\"supervisor_mode\":\"%s\","
             "\"version\":%lld"
             "}",
             policy.room_id,
             policy.layout_mode,
             policy.active_speaker_participant_id,
             policy.pinned_participant_id,
             call_center_supervisor_mode_name(policy.supervisor_mode),
             (long long)policy.version);
    return json;
}

static int current_call_center_supervisor_mode(
    room_service_app_server_t *server, const char *room_id,
    turbo_call_center_supervisor_mode_t *mode) {
    room_service_conference_policy_t policy;

    if (mode) {
        *mode = TURBO_CALL_CENTER_SUPERVISOR_NONE;
    }
    if (!server || !room_id || !mode) {
        return -1;
    }
    if (room_service_app_server_get_conference_policy(server, room_id, &policy) == 0 &&
        policy.available) {
        *mode = policy.supervisor_mode;
    }
    return 0;
}

static int reconcile_call_center_policy_for_room(
    room_service_app_server_t *server, turbo_room_service_t *service,
    const char *room_id, room_service_conference_policy_apply_result_t *result) {
    turbo_call_center_room_summary_t call_center_room;
    turbo_call_center_supervisor_mode_t supervisor_mode =
        TURBO_CALL_CENTER_SUPERVISOR_NONE;

    if (result) {
        memset(result, 0, sizeof(*result));
    }
    if (!server || !service || !room_id) {
        return -1;
    }
    if (turbo_room_service_get_call_center_room_summary(service, room_id,
                                                        &call_center_room) != 0 ||
        !call_center_room.available) {
        return 0;
    }
    if (current_call_center_supervisor_mode(server, room_id, &supervisor_mode) != 0) {
        return -1;
    }
    return room_service_app_server_apply_call_center_policy(server, room_id,
                                                            supervisor_mode, result);
}

static int is_call_center_observer_role(turbo_participant_role_t role) {
    return role == TURBO_PARTICIPANT_ROLE_SUPERVISOR ||
           role == TURBO_PARTICIPANT_ROLE_QA_OBSERVER ||
           role == TURBO_PARTICIPANT_ROLE_BOT;
}

static void record_call_center_event_if_room(
    room_service_app_server_t *server, turbo_room_service_t *service,
    const char *room_id, const char *event_type, const char *participant_id,
    const char *peer_participant_id, const char *state, const char *detail) {
    turbo_call_center_room_summary_t summary;

    if (!server || !service || !room_id || !event_type) {
        return;
    }
    if (turbo_room_service_get_call_center_room_summary(service, room_id, &summary) != 0 ||
        !summary.available) {
        return;
    }
    (void)room_service_app_server_record_call_center_event(
        server, room_id, event_type, participant_id, peer_participant_id, state, detail);
}

static char *room_summary_json(turbo_room_service_t *service, const char *room_id) {
    turbo_room_summary_t summary;
    char *json;

    if (!service || !room_id ||
        turbo_room_service_get_room_summary(service, room_id, &summary) != 0) {
        return NULL;
    }

    json = (char *)malloc(1024);
    if (!json) {
        return NULL;
    }

    snprintf(
        json, 1024,
        "{"
        "\"room_id\":\"%s\","
        "\"room_type\":%d,"
        "\"status\":\"%s\","
        "\"assigned_sfu_node\":\"%s\","
        "\"recording_state\":\"%s\","
        "\"recording_id\":\"%s\","
        "\"recording_mode\":\"%s\","
        "\"participant_count\":%d,"
        "\"published_track_count\":%d,"
        "\"subscription_count\":%d,"
        "\"version\":%lld"
        "}",
        summary.room_id,
        (int)summary.room_type,
        room_status_name(summary.status),
        summary.assigned_sfu_node,
        recording_state_name(summary.recording_state),
        summary.recording_id,
        summary.recording_mode,
        summary.participant_count,
        summary.published_track_count,
        summary.subscription_count,
        (long long)summary.version);

    return json;
}

static char *call_center_queue_entry_summary_json(
    const turbo_call_center_queue_entry_summary_t *summary) {
    char *json;

    if (!summary) {
        return NULL;
    }

    json = (char *)malloc(384);
    if (!json) {
        return NULL;
    }

    snprintf(json, 384,
             "{"
             "\"queue_id\":\"%s\","
             "\"side\":\"%s\","
             "\"entry_id\":\"%s\","
             "\"endpoint_id\":\"%s\","
             "\"priority\":%d,"
             "\"sequence\":%lld,"
             "\"version\":%lld"
             "}",
             summary->queue_id,
             call_center_queue_side_name(summary->side),
             summary->entry_id,
             summary->endpoint_id,
             summary->priority,
             (long long)summary->sequence,
             (long long)summary->version);

    return json;
}

static char *call_center_queue_match_summary_json(
    const turbo_call_center_queue_match_summary_t *summary) {
    char *caller_json = NULL;
    char *callee_json = NULL;
    char *json;
    size_t len;

    if (!summary) {
        return NULL;
    }

    if (summary->matched) {
        caller_json = call_center_queue_entry_summary_json(&summary->caller);
        callee_json = call_center_queue_entry_summary_json(&summary->callee);
        if (!caller_json || !callee_json) {
            free(caller_json);
            free(callee_json);
            return NULL;
        }
    }

    len = (caller_json ? strlen(caller_json) : 4) +
          (callee_json ? strlen(callee_json) : 4) + 64;
    json = (char *)malloc(len);
    if (!json) {
        free(caller_json);
        free(callee_json);
        return NULL;
    }

    snprintf(json, len,
             "{"
             "\"matched\":%s,"
             "\"caller\":%s,"
             "\"callee\":%s"
             "}",
             summary->matched ? "true" : "false",
             caller_json ? caller_json : "null",
             callee_json ? callee_json : "null");

    free(caller_json);
    free(callee_json);
    return json;
}

static char *call_center_agent_state_summary_json(
    const turbo_call_center_agent_state_summary_t *summary) {
    char *json;

    if (!summary) {
        return NULL;
    }

    json = (char *)malloc(256);
    if (!json) {
        return NULL;
    }

    snprintf(json, 256,
             "{"
             "\"endpoint_id\":\"%s\","
             "\"state\":\"%s\","
             "\"explicit_state\":%s,"
             "\"version\":%lld"
             "}",
             summary->endpoint_id,
             call_center_agent_state_name(summary->state),
             summary->explicit_state ? "true" : "false",
             (long long)summary->version);
    return json;
}

static char *call_center_room_summary_json(
    const turbo_call_center_room_summary_t *summary) {
    char *json;

    if (!summary) {
        return NULL;
    }

    json = (char *)malloc(448);
    if (!json) {
        return NULL;
    }

    snprintf(json, 448,
             "{"
             "\"room_id\":\"%s\","
             "\"state\":\"%s\","
             "\"customer_participant_id\":\"%s\","
             "\"agent_participant_id\":\"%s\","
             "\"consult_agent_participant_id\":\"%s\","
             "\"disposition_code\":\"%s\","
             "\"version\":%lld"
             "}",
             summary->room_id,
             call_center_room_state_name(summary->state),
             summary->customer_participant_id,
             summary->agent_participant_id,
             summary->consult_agent_participant_id,
             summary->disposition_code,
             (long long)summary->version);
    return json;
}

static char *call_center_route_result_json(
    const char *room_id,
    const turbo_call_center_queue_entry_summary_t *caller,
    const turbo_call_center_queue_entry_summary_t *callee) {
    turbo_call_center_queue_match_summary_t match;
    char *match_json;
    char *json;
    size_t len;

    if (!room_id || !caller || !callee) {
        return NULL;
    }

    memset(&match, 0, sizeof(match));
    match.matched = 1;
    match.caller = *caller;
    match.callee = *callee;

    match_json = call_center_queue_match_summary_json(&match);
    if (!match_json) {
        return NULL;
    }

    len = strlen(room_id) + strlen(match_json) + 64;
    json = (char *)malloc(len);
    if (!json) {
        free(match_json);
        return NULL;
    }

    snprintf(json, len,
             "{"
             "\"room_id\":\"%s\","
             "\"match\":%s"
             "}",
             room_id, match_json);

    free(match_json);
    return json;
}

static char *call_center_event_json(const room_service_call_center_event_t *event) {
    char *json;

    if (!event) {
        return NULL;
    }

    json = (char *)malloc(512);
    if (!json) {
        return NULL;
    }

    snprintf(json, 512,
             "{"
             "\"sequence\":%lld,"
             "\"room_id\":\"%s\","
             "\"event_type\":\"%s\","
             "\"participant_id\":\"%s\","
             "\"peer_participant_id\":\"%s\","
             "\"state\":\"%s\","
             "\"detail\":\"%s\""
             "}",
             (long long)event->sequence,
             event->room_id,
             event->event_type,
             event->participant_id,
             event->peer_participant_id,
             event->state,
             event->detail);
    return json;
}

static char *call_center_events_json(room_service_app_server_t *server, const char *room_id,
                                     int64_t after_sequence, int limit) {
    room_service_call_center_event_t events[64];
    char *events_json = NULL;
    char *result = NULL;
    size_t events_len = 0;
    size_t events_cap = 0;
    int count = 0;
    int64_t latest_sequence = 0;
    int i;

    if (!server || !room_id) {
        return NULL;
    }

    if (room_service_app_server_list_call_center_events(server, room_id, after_sequence,
                                                        limit, events,
                                                        (int)(sizeof(events) /
                                                              sizeof(events[0])),
                                                        &count, &latest_sequence) != 0) {
        return NULL;
    }

    if (append_json_fragment(&events_json, &events_len, &events_cap, "[") != 0) {
        goto cleanup;
    }

    for (i = 0; i < count; ++i) {
        char *event_json = call_center_event_json(&events[i]);

        if (!event_json) {
            goto cleanup;
        }
        if (i > 0 &&
            append_json_fragment(&events_json, &events_len, &events_cap, ",") != 0) {
            free(event_json);
            goto cleanup;
        }
        if (append_json_fragment(&events_json, &events_len, &events_cap, event_json) != 0) {
            free(event_json);
            goto cleanup;
        }
        free(event_json);
    }

    if (append_json_fragment(&events_json, &events_len, &events_cap, "]") != 0) {
        goto cleanup;
    }

    {
        size_t len = strlen(room_id) + strlen(events_json) + 96;
        result = (char *)malloc(len);
        if (!result) {
            goto cleanup;
        }
        snprintf(result, len,
                 "{"
                 "\"room_id\":\"%s\","
                 "\"latest_sequence\":%lld,"
                 "\"events\":%s"
                 "}",
                 room_id,
                 (long long)latest_sequence,
                 events_json);
    }

cleanup:
    free(events_json);
    return result;
}

static char *participant_summary_json(turbo_room_service_t *service, const char *room_id,
                                      const char *participant_id) {
    turbo_room_participant_summary_t summary;
    int effective_bandwidth_bps = 0;
    int is_explicit_bandwidth = 0;
    char *json;

    if (!service || !room_id || !participant_id ||
        turbo_room_service_get_participant_summary(service, room_id, participant_id, &summary) != 0) {
        return NULL;
    }
    turbo_room_service_get_effective_receiver_bandwidth(service, room_id, participant_id,
                                                        &effective_bandwidth_bps,
                                                        &is_explicit_bandwidth);

    json = (char *)malloc(768);
    if (!json) {
        return NULL;
    }

    snprintf(json, 768,
             "{"
             "\"participant_id\":\"%s\","
             "\"user_id\":\"%s\","
             "\"display_name\":\"%s\","
             "\"role\":\"%s\","
             "\"session_state\":\"%s\","
             "\"bandwidth_bps\":%d,"
             "\"effective_receiver_bandwidth_bps\":%d,"
             "\"bandwidth_source\":\"%s\","
             "\"version\":%lld"
             "}",
             summary.participant_id,
             summary.user_id,
             summary.display_name,
             participant_role_name(summary.role),
             participant_session_state_name(summary.session_state),
             summary.bandwidth_bps,
             effective_bandwidth_bps,
             is_explicit_bandwidth ? "explicit" : "subscription_derived",
             (long long)summary.version);

    return json;
}

static char *participant_bandwidth_diagnostic_json(room_service_app_server_t *server,
                                                   turbo_room_service_t *service,
                                                   const char *room_id,
                                                   const char *participant_id,
                                                   int *out_in_sync) {
    turbo_room_summary_t room_summary;
    turbo_room_participant_summary_t participant_summary;
    room_service_sfu_participant_stats_t sfu_stats;
    int effective_bandwidth_bps = 0;
    int is_explicit_bandwidth = 0;
    const char *mirror_status = "ok";
    const char *bandwidth_source;
    int in_sync = 0;
    char *participant_json;
    char *json;
    char sfu_json[1024];
    size_t len;
    int fetch_rc;

    if (!server || !service || !room_id || !participant_id) {
        return NULL;
    }

    if (out_in_sync) {
        *out_in_sync = 0;
    }

    if (turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0 ||
        turbo_room_service_get_participant_summary(service, room_id, participant_id,
                                                   &participant_summary) != 0 ||
        turbo_room_service_get_effective_receiver_bandwidth(service, room_id, participant_id,
                                                            &effective_bandwidth_bps,
                                                            &is_explicit_bandwidth) != 0) {
        return NULL;
    }

    participant_json = participant_summary_json(service, room_id, participant_id);
    if (!participant_json) {
        return NULL;
    }

    bandwidth_source = is_explicit_bandwidth ? "explicit" : "subscription_derived";
    memset(&sfu_stats, 0, sizeof(sfu_stats));
    if (room_summary.assigned_sfu_node[0] == '\0') {
        mirror_status = "unassigned";
        snprintf(sfu_json, sizeof(sfu_json), "null");
    } else {
        fetch_rc = room_service_app_server_fetch_participant_stats(
            server, room_id, participant_id, &sfu_stats);
        if (fetch_rc != 0) {
            mirror_status = "error";
            snprintf(sfu_json, sizeof(sfu_json), "null");
        } else if (!sfu_stats.available) {
            mirror_status = "unavailable";
            snprintf(sfu_json, sizeof(sfu_json), "null");
        } else if (!sfu_stats.found) {
            mirror_status = "not_found";
            snprintf(sfu_json, sizeof(sfu_json), "null");
        } else {
            mirror_status = "ok";
            snprintf(sfu_json, sizeof(sfu_json),
                     "{"
                     "\"participant_id\":\"%s\","
                     "\"stream_count\":%d,"
                     "\"available_bandwidth\":%d,"
                     "\"packets_sent\":%lld,"
                     "\"bytes_sent\":%lld,"
                     "\"packets_received\":%lld,"
                     "\"bytes_received\":%lld"
                     "}",
                     sfu_stats.participant_id,
                     sfu_stats.stream_count,
                     sfu_stats.available_bandwidth,
                     (long long)sfu_stats.packets_sent,
                     (long long)sfu_stats.bytes_sent,
                     (long long)sfu_stats.packets_received,
                     (long long)sfu_stats.bytes_received);
            in_sync = strcmp(participant_id, sfu_stats.participant_id) == 0 &&
                      effective_bandwidth_bps == sfu_stats.available_bandwidth;
        }
    }

    len = strlen(participant_json) + strlen(sfu_json) + strlen(room_id) +
          strlen(participant_id) + strlen(mirror_status) + strlen(bandwidth_source) + 768;
    json = (char *)malloc(len);
    if (!json) {
        free(participant_json);
        return NULL;
    }

    snprintf(json, len,
             "{"
             "\"room_id\":\"%s\","
             "\"participant_id\":\"%s\","
             "\"local_participant\":%s,"
             "\"expected_bandwidth\":{"
             "\"effective_receiver_bandwidth_bps\":%d,"
             "\"bandwidth_source\":\"%s\""
             "},"
             "\"sfu_mirror_status\":\"%s\","
             "\"sfu_participant_stats\":%s,"
             "\"in_sync\":%s"
             "}",
             room_id,
             participant_id,
             participant_json,
             effective_bandwidth_bps,
             bandwidth_source,
             mirror_status,
             sfu_json,
             in_sync ? "true" : "false");

    if (out_in_sync) {
        *out_in_sync = in_sync;
    }

    free(participant_json);
    return json;
}

static char *track_summary_json(turbo_room_service_t *service, const char *room_id,
                                const char *track_id) {
    turbo_room_track_summary_t summary;
    char *json;
    char layer_json[96];
    int written;
    int i;

    if (!service || !room_id || !track_id ||
        turbo_room_service_get_track_summary(service, room_id, track_id, &summary) != 0) {
        return NULL;
    }

    json = (char *)malloc(768);
    if (!json) {
        return NULL;
    }

    written = snprintf(layer_json, sizeof(layer_json), "[");
    if (written < 0 || written >= (int)sizeof(layer_json)) {
        free(json);
        return NULL;
    }

    for (i = 0; i < summary.layer_count && i < 3; ++i) {
        int rc = snprintf(layer_json + written, sizeof(layer_json) - (size_t)written,
                          "%s%u", i == 0 ? "" : ",",
                          (unsigned)summary.layer_ssrcs[i]);
        if (rc < 0 || rc >= (int)(sizeof(layer_json) - (size_t)written)) {
            free(json);
            return NULL;
        }
        written += rc;
    }

    if (snprintf(layer_json + written, sizeof(layer_json) - (size_t)written, "]")
        >= (int)(sizeof(layer_json) - (size_t)written)) {
        free(json);
        return NULL;
    }

    snprintf(json, 768,
             "{"
             "\"track_id\":\"%s\","
             "\"owner_participant_id\":\"%s\","
             "\"kind\":\"%s\","
             "\"source\":\"%s\","
             "\"codec_name\":\"%s\","
             "\"simulcast_enabled\":%s,"
             "\"muted\":%s,"
             "\"main_ssrc\":%u,"
             "\"layer_ssrcs\":%s,"
             "\"layer_count\":%d,"
             "\"version\":%lld"
             "}",
             summary.track_id,
             summary.owner_participant_id,
             track_kind_name(summary.kind),
             track_source_name(summary.source),
             summary.codec_name,
             summary.simulcast_enabled ? "true" : "false",
             summary.muted ? "true" : "false",
             (unsigned)summary.main_ssrc,
             layer_json,
             summary.layer_count,
             (long long)summary.version);

    return json;
}

static char *subscription_summary_json(turbo_room_service_t *service, const char *room_id,
                                       const char *subscriber_participant_id,
                                       const char *track_id) {
    turbo_room_subscription_summary_t summary;
    char *json;

    if (!service || !room_id || !subscriber_participant_id || !track_id ||
        turbo_room_service_get_subscription_summary(service, room_id, subscriber_participant_id,
                                                    track_id, &summary) != 0) {
        return NULL;
    }

    json = (char *)malloc(768);
    if (!json) {
        return NULL;
    }

    snprintf(json, 768,
             "{"
             "\"subscriber_participant_id\":\"%s\","
             "\"track_id\":\"%s\","
             "\"enabled\":%s,"
             "\"priority\":%d,"
             "\"preferred_layer\":\"%s\","
             "\"target_layer\":\"%s\","
             "\"muted\":%s,"
             "\"policy_source\":\"%s\","
             "\"version\":%lld"
             "}",
             summary.subscriber_participant_id,
             summary.track_id,
             summary.enabled ? "true" : "false",
             summary.priority,
             video_layer_name(summary.preferred_layer),
             video_layer_name(summary.target_layer),
             summary.muted ? "true" : "false",
             summary.policy_source,
             (long long)summary.version);

    return json;
}

static char *subscription_diagnostic_json(room_service_app_server_t *server,
                                          turbo_room_service_t *service,
                                          const char *room_id,
                                          const char *subscriber_participant_id,
                                          const char *track_id,
                                          int *out_in_sync) {
    turbo_room_summary_t room_summary;
    turbo_room_subscription_summary_t subscription_summary;
    turbo_room_track_summary_t track_summary;
    room_service_sfu_track_subscription_t sfu_subscription;
    turbo_room_video_layer_t expected_max_layer;
    const char *mirror_status = "ok";
    int expected_enabled;
    int in_sync = 0;
    char *subscription_json;
    char *track_json;
    char *json;
    char sfu_json[512];
    size_t len;
    int fetch_rc;

    if (!server || !service || !room_id || !subscriber_participant_id || !track_id) {
        return NULL;
    }

    if (out_in_sync) {
        *out_in_sync = 0;
    }

    if (turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0 ||
        turbo_room_service_get_subscription_summary(service, room_id,
                                                    subscriber_participant_id, track_id,
                                                    &subscription_summary) != 0 ||
        turbo_room_service_get_track_summary(service, room_id, track_id, &track_summary) != 0) {
        return NULL;
    }

    subscription_json = subscription_summary_json(service, room_id,
                                                  subscriber_participant_id, track_id);
    track_json = track_summary_json(service, room_id, track_id);
    if (!subscription_json || !track_json) {
        free(subscription_json);
        free(track_json);
        return NULL;
    }

    expected_enabled = subscription_summary.enabled && !subscription_summary.muted;
    expected_max_layer = resolve_subscription_max_layer(&subscription_summary);

    memset(&sfu_subscription, 0, sizeof(sfu_subscription));
    if (room_summary.assigned_sfu_node[0] == '\0') {
        mirror_status = "unassigned";
        snprintf(sfu_json, sizeof(sfu_json), "null");
    } else {
        fetch_rc = room_service_app_server_fetch_track_subscription(
            server, room_id, subscriber_participant_id, track_id, &sfu_subscription);
        if (fetch_rc != 0) {
            mirror_status = "error";
            snprintf(sfu_json, sizeof(sfu_json), "null");
        } else if (!sfu_subscription.available) {
            mirror_status = "unavailable";
            snprintf(sfu_json, sizeof(sfu_json), "null");
        } else if (!sfu_subscription.found) {
            mirror_status = "not_found";
            snprintf(sfu_json, sizeof(sfu_json), "null");
        } else {
            mirror_status = "ok";
            snprintf(sfu_json, sizeof(sfu_json),
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
                     sfu_subscription.receiver_participant_id,
                     sfu_subscription.sender_participant_id,
                     sfu_subscription.track_id,
                     sfu_subscription.enabled ? "true" : "false",
                     sfu_subscription.priority,
                     video_layer_name(sfu_subscription.preferred_layer),
                     video_layer_name(sfu_subscription.target_layer),
                     sfu_subscription.muted ? "true" : "false",
                     sfu_subscription.policy_source,
                     video_layer_name(sfu_subscription.max_layer));
            in_sync = expected_enabled == sfu_subscription.enabled &&
                      subscription_summary.priority == sfu_subscription.priority &&
                      subscription_summary.preferred_layer ==
                          sfu_subscription.preferred_layer &&
                      subscription_summary.target_layer == sfu_subscription.target_layer &&
                      subscription_summary.muted == sfu_subscription.muted &&
                      strcmp(subscription_summary.policy_source,
                             sfu_subscription.policy_source) == 0 &&
                      expected_max_layer == sfu_subscription.max_layer &&
                      strcmp(subscriber_participant_id,
                             sfu_subscription.receiver_participant_id) == 0 &&
                      strcmp(track_id, sfu_subscription.track_id) == 0 &&
                      strcmp(track_summary.owner_participant_id,
                             sfu_subscription.sender_participant_id) == 0;
        }
    }

    len = strlen(subscription_json) + strlen(track_json) + strlen(sfu_json) +
          strlen(room_id) + strlen(subscriber_participant_id) + strlen(track_id) +
          strlen(track_summary.owner_participant_id) + strlen(mirror_status) + 1024;
    json = (char *)malloc(len);
    if (!json) {
        free(subscription_json);
        free(track_json);
        return NULL;
    }

    snprintf(json, len,
             "{"
             "\"room_id\":\"%s\","
             "\"subscriber_participant_id\":\"%s\","
             "\"track_id\":\"%s\","
             "\"local_subscription\":%s,"
             "\"local_track\":%s,"
             "\"expected_execution\":{"
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
             "},"
             "\"sfu_mirror_status\":\"%s\","
             "\"sfu_track_subscription\":%s,"
             "\"in_sync\":%s"
             "}",
             room_id,
             subscriber_participant_id,
             track_id,
             subscription_json,
             track_json,
             subscriber_participant_id,
             track_summary.owner_participant_id,
             track_id,
             expected_enabled ? "true" : "false",
             subscription_summary.priority,
             video_layer_name(subscription_summary.preferred_layer),
             video_layer_name(subscription_summary.target_layer),
             subscription_summary.muted ? "true" : "false",
             subscription_summary.policy_source,
             video_layer_name(expected_max_layer),
             mirror_status,
             sfu_json,
             in_sync ? "true" : "false");

    if (out_in_sync) {
        *out_in_sync = in_sync;
    }

    free(subscription_json);
    free(track_json);
    return json;
}

static char *room_diagnostic_json(room_service_app_server_t *server,
                                  turbo_room_service_t *service,
                                  const char *room_id) {
    turbo_room_summary_t room_summary;
    char *room_json = NULL;
    char *policy_json = NULL;
    char *last_sync_json = NULL;
    char *participants_json = NULL;
    char *subscriptions_json = NULL;
    char *result = NULL;
    size_t participants_len = 0;
    size_t participants_cap = 0;
    size_t subscriptions_len = 0;
    size_t subscriptions_cap = 0;
    int participant_mismatch_count = 0;
    int subscription_mismatch_count = 0;
    int i;

    if (!server || !service || !room_id ||
        turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0) {
        return NULL;
    }

    room_json = room_summary_json(service, room_id);
    if (!room_json) {
        return NULL;
    }
    policy_json = conference_policy_json(server, room_id);
    if (!policy_json) {
        goto cleanup;
    }
    last_sync_json = room_sync_diagnostic_json(server, room_id);
    if (!last_sync_json) {
        goto cleanup;
    }

    if (room_summary.status == TURBO_ROOM_STATUS_CLOSED) {
        size_t len = strlen(room_json) + strlen(policy_json) + strlen(last_sync_json) +
                     strlen(room_id) + 448;
        result = (char *)malloc(len);
        if (!result) {
            goto cleanup;
        }

        snprintf(result, len,
                 "{"
                 "\"room_id\":\"%s\","
                 "\"room\":%s,"
                 "\"conference_policy\":%s,"
                 "\"runtime_sync_expected\":false,"
                 "\"last_sfu_sync\":%s,"
                 "\"participant_bandwidth_diagnostics\":[],"
                 "\"subscription_diagnostics\":[],"
                 "\"participant_mismatch_count\":0,"
                 "\"subscription_mismatch_count\":0,"
                 "\"in_sync\":true"
                 "}",
                 room_id, room_json, policy_json, last_sync_json);
        goto cleanup;
    }

    if (append_json_fragment(&participants_json, &participants_len, &participants_cap, "[") != 0 ||
        append_json_fragment(&subscriptions_json, &subscriptions_len, &subscriptions_cap, "[") != 0) {
        goto cleanup;
    }

    for (i = 0; i < room_summary.participant_count; ++i) {
        turbo_room_participant_summary_t participant_summary;
        char *participant_json;
        int participant_in_sync = 0;

        if (turbo_room_service_get_participant_summary_at(service, room_id, i,
                                                          &participant_summary) != 0) {
            goto cleanup;
        }

        participant_json = participant_bandwidth_diagnostic_json(
            server, service, room_id, participant_summary.participant_id,
            &participant_in_sync);
        if (!participant_json) {
            goto cleanup;
        }

        if (i > 0 &&
            append_json_fragment(&participants_json, &participants_len, &participants_cap, ",") != 0) {
            free(participant_json);
            goto cleanup;
        }
        if (append_json_fragment(&participants_json, &participants_len, &participants_cap,
                                 participant_json) != 0) {
            free(participant_json);
            goto cleanup;
        }
        if (!participant_in_sync) {
            participant_mismatch_count++;
        }
        free(participant_json);
    }

    if (append_json_fragment(&participants_json, &participants_len, &participants_cap, "]") != 0) {
        goto cleanup;
    }

    for (i = 0; i < room_summary.subscription_count; ++i) {
        turbo_room_subscription_summary_t subscription_summary;
        char *subscription_json;
        int subscription_in_sync = 0;

        if (turbo_room_service_get_subscription_summary_at(service, room_id, i,
                                                           &subscription_summary) != 0) {
            goto cleanup;
        }

        subscription_json = subscription_diagnostic_json(
            server, service, room_id, subscription_summary.subscriber_participant_id,
            subscription_summary.track_id, &subscription_in_sync);
        if (!subscription_json) {
            goto cleanup;
        }

        if (i > 0 && append_json_fragment(&subscriptions_json, &subscriptions_len,
                                          &subscriptions_cap, ",") != 0) {
            free(subscription_json);
            goto cleanup;
        }
        if (append_json_fragment(&subscriptions_json, &subscriptions_len,
                                 &subscriptions_cap, subscription_json) != 0) {
            free(subscription_json);
            goto cleanup;
        }
        if (!subscription_in_sync) {
            subscription_mismatch_count++;
        }
        free(subscription_json);
    }

    if (append_json_fragment(&subscriptions_json, &subscriptions_len, &subscriptions_cap, "]") != 0) {
        goto cleanup;
    }

    {
        size_t len = strlen(room_json) + strlen(policy_json) + strlen(last_sync_json) +
                     strlen(participants_json) + strlen(subscriptions_json) +
                     strlen(room_id) + 320;
        result = (char *)malloc(len);
        if (!result) {
            goto cleanup;
        }

        snprintf(result, len,
                 "{"
                 "\"room_id\":\"%s\","
                 "\"room\":%s,"
                 "\"conference_policy\":%s,"
                 "\"runtime_sync_expected\":true,"
                 "\"last_sfu_sync\":%s,"
                 "\"participant_bandwidth_diagnostics\":%s,"
                 "\"subscription_diagnostics\":%s,"
                 "\"participant_mismatch_count\":%d,"
                 "\"subscription_mismatch_count\":%d,"
                 "\"in_sync\":%s"
                 "}",
                 room_id,
                 room_json,
                 policy_json,
                 last_sync_json,
                 participants_json,
                 subscriptions_json,
                 participant_mismatch_count,
                 subscription_mismatch_count,
                 (participant_mismatch_count == 0 && subscription_mismatch_count == 0)
                     ? "true"
                     : "false");
    }

cleanup:
    free(room_json);
    free(policy_json);
    free(last_sync_json);
    free(participants_json);
    free(subscriptions_json);
    return result;
}

static char *room_state_json(room_service_app_server_t *server,
                             turbo_room_service_t *service,
                             const char *room_id) {
    turbo_room_summary_t room_summary;
    char *room_json = NULL;
    char *policy_json = NULL;
    char *participants_json = NULL;
    char *tracks_json = NULL;
    char *subscriptions_json = NULL;
    char *result = NULL;
    size_t participants_len = 0;
    size_t participants_cap = 0;
    size_t tracks_len = 0;
    size_t tracks_cap = 0;
    size_t subscriptions_len = 0;
    size_t subscriptions_cap = 0;
    int i;

    if (!server || !service || !room_id ||
        turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0) {
        return NULL;
    }

    room_json = room_summary_json(service, room_id);
    policy_json = conference_policy_json(server, room_id);
    if (!room_json || !policy_json) {
        goto cleanup;
    }

    if (append_json_fragment(&participants_json, &participants_len, &participants_cap, "[") != 0 ||
        append_json_fragment(&tracks_json, &tracks_len, &tracks_cap, "[") != 0 ||
        append_json_fragment(&subscriptions_json, &subscriptions_len, &subscriptions_cap, "[") != 0) {
        goto cleanup;
    }

    for (i = 0; i < room_summary.participant_count; ++i) {
        turbo_room_participant_summary_t participant_summary;
        char *participant_json;

        if (turbo_room_service_get_participant_summary_at(service, room_id, i,
                                                          &participant_summary) != 0) {
            goto cleanup;
        }
        participant_json = participant_summary_json(service, room_id,
                                                    participant_summary.participant_id);
        if (!participant_json) {
            goto cleanup;
        }
        if (i > 0 &&
            append_json_fragment(&participants_json, &participants_len, &participants_cap, ",") != 0) {
            free(participant_json);
            goto cleanup;
        }
        if (append_json_fragment(&participants_json, &participants_len, &participants_cap,
                                 participant_json) != 0) {
            free(participant_json);
            goto cleanup;
        }
        free(participant_json);
    }

    for (i = 0; i < room_summary.published_track_count; ++i) {
        turbo_room_track_summary_t track_summary;
        char *track_json;

        if (turbo_room_service_get_track_summary_at(service, room_id, i, &track_summary) != 0) {
            goto cleanup;
        }
        track_json = track_summary_json(service, room_id, track_summary.track_id);
        if (!track_json) {
            goto cleanup;
        }
        if (i > 0 && append_json_fragment(&tracks_json, &tracks_len, &tracks_cap, ",") != 0) {
            free(track_json);
            goto cleanup;
        }
        if (append_json_fragment(&tracks_json, &tracks_len, &tracks_cap, track_json) != 0) {
            free(track_json);
            goto cleanup;
        }
        free(track_json);
    }

    for (i = 0; i < room_summary.subscription_count; ++i) {
        turbo_room_subscription_summary_t subscription_summary;
        char *subscription_json;

        if (turbo_room_service_get_subscription_summary_at(service, room_id, i,
                                                           &subscription_summary) != 0) {
            goto cleanup;
        }
        subscription_json = subscription_summary_json(
            service, room_id, subscription_summary.subscriber_participant_id,
            subscription_summary.track_id);
        if (!subscription_json) {
            goto cleanup;
        }
        if (i > 0 &&
            append_json_fragment(&subscriptions_json, &subscriptions_len,
                                 &subscriptions_cap, ",") != 0) {
            free(subscription_json);
            goto cleanup;
        }
        if (append_json_fragment(&subscriptions_json, &subscriptions_len,
                                 &subscriptions_cap, subscription_json) != 0) {
            free(subscription_json);
            goto cleanup;
        }
        free(subscription_json);
    }

    if (append_json_fragment(&participants_json, &participants_len, &participants_cap, "]") != 0 ||
        append_json_fragment(&tracks_json, &tracks_len, &tracks_cap, "]") != 0 ||
        append_json_fragment(&subscriptions_json, &subscriptions_len,
                             &subscriptions_cap, "]") != 0) {
        goto cleanup;
    }

    {
        size_t len = strlen(room_json) + strlen(policy_json) + strlen(participants_json) +
                     strlen(tracks_json) + strlen(subscriptions_json) + 128;
        result = (char *)malloc(len);
        if (!result) {
            goto cleanup;
        }

        snprintf(result, len,
                 "{"
                 "\"room\":%s,"
                 "\"conference_policy\":%s,"
                 "\"participants\":%s,"
                 "\"published_tracks\":%s,"
                 "\"subscriptions\":%s"
                 "}",
                 room_json,
                 policy_json,
                 participants_json,
                 tracks_json,
                 subscriptions_json);
    }

cleanup:
    free(room_json);
    free(policy_json);
    free(participants_json);
    free(tracks_json);
    free(subscriptions_json);
    return result;
}

static void send_error_json(Res *res, int status, const char *code, const char *message) {
    char json[512];
    snprintf(json, sizeof(json),
             "{\"ok\":false,\"code\":\"%s\",\"message\":\"%s\"}",
             code ? code : "INTERNAL_ERROR",
             message ? message : "internal error");
    send_json(res, status, json);
}

static void send_room_ok_json(Res *res, turbo_room_service_t *service, const char *room_id) {
    char *room_json = room_summary_json(service, room_id);
    char *payload;
    size_t len;

    if (!room_json) {
        send_json(res, 200, "{\"ok\":true}");
        return;
    }

    len = strlen(room_json) + 32;
    payload = (char *)malloc(len);
    if (!payload) {
        free(room_json);
        send_json(res, 200, "{\"ok\":true}");
        return;
    }

    snprintf(payload, len, "{\"ok\":true,\"room\":%s}", room_json);
    send_json(res, 200, payload);
    free(payload);
    free(room_json);
}

static void send_room_warning_json(Res *res, turbo_room_service_t *service,
                                   const char *room_id, const char *warning_code,
                                   const char *warning_message) {
    char *room_json = room_summary_json(service, room_id);
    char *payload;
    size_t len;

    if (!warning_code || !warning_message) {
        send_room_ok_json(res, service, room_id);
        return;
    }

    if (!room_json) {
        send_error_json(res, 500, "INTERNAL_ERROR", "room summary unavailable");
        return;
    }

    len = strlen(room_json) + strlen(warning_code) + strlen(warning_message) + 80;
    payload = (char *)malloc(len);
    if (!payload) {
        free(room_json);
        send_room_ok_json(res, service, room_id);
        return;
    }

    snprintf(payload, len,
             "{"
             "\"ok\":true,"
             "\"room\":%s,"
             "\"warning_code\":\"%s\","
             "\"warning\":\"%s\""
             "}",
             room_json, warning_code, warning_message);
    send_json(res, 200, payload);
    free(payload);
    free(room_json);
}

static int sync_derived_receiver_bandwidth_if_needed(
    turbo_room_service_t *service, const char *room_id, const char *participant_id,
    const char **warning_code, const char **warning_message) {
    turbo_room_summary_t room_summary;
    int effective_bandwidth_bps = 0;
    int is_explicit_bandwidth = 0;

    if (!service || !room_id || !participant_id) {
        return -1;
    }

    if (turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0 ||
        room_summary.assigned_sfu_node[0] == '\0') {
        return 0;
    }

    if (turbo_room_service_get_effective_receiver_bandwidth(service, room_id, participant_id,
                                                            &effective_bandwidth_bps,
                                                            &is_explicit_bandwidth) != 0 ||
        is_explicit_bandwidth) {
        return 0;
    }

    if (room_service_app_server_sync_set_receiver_bandwidth(g_room_service_server, room_id,
                                                            participant_id,
                                                            effective_bandwidth_bps) != 0) {
        if (warning_code) {
            *warning_code = "SFU_SYNC_FAILED";
        }
        if (warning_message) {
            *warning_message =
                "subscription state committed locally; derived receiver bandwidth was not forwarded to sfu node";
        }
        return -1;
    }

    return 0;
}

static int sync_track_subscription_if_needed(
    turbo_room_service_t *service, const char *room_id, const char *participant_id,
    const char *track_id, const char **warning_code, const char **warning_message) {
    turbo_room_summary_t room_summary;
    turbo_room_track_summary_t track_summary;
    turbo_room_subscription_summary_t subscription_summary;

    if (!service || !room_id || !participant_id || !track_id) {
        return -1;
    }

    if (turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0 ||
        room_summary.assigned_sfu_node[0] == '\0') {
        return 0;
    }

    if (turbo_room_service_get_track_summary(service, room_id, track_id, &track_summary) != 0 ||
        track_summary.main_ssrc == 0) {
        if (warning_code) {
            *warning_code = "SFU_SYNC_SKIPPED";
        }
        if (warning_message) {
            *warning_message =
                "subscription state committed locally; track lacks SSRC metadata for sfu sync";
        }
        return -1;
    }

    if (turbo_room_service_get_subscription_summary(service, room_id, participant_id, track_id,
                                                    &subscription_summary) == 0) {
        if (room_service_app_server_sync_apply_track_subscription(
                g_room_service_server, room_id, &subscription_summary) != 0) {
            if (warning_code) {
                *warning_code = "SFU_SYNC_FAILED";
            }
            if (warning_message) {
                *warning_message =
                    "subscription state committed locally; set_track_subscription was not forwarded to sfu node";
            }
            return -1;
        }
        return 0;
    }

    {
        turbo_room_subscription_summary_t disabled_summary;
        memset(&disabled_summary, 0, sizeof(disabled_summary));
        strncpy(disabled_summary.subscriber_participant_id, participant_id,
                sizeof(disabled_summary.subscriber_participant_id) - 1);
        strncpy(disabled_summary.track_id, track_id, sizeof(disabled_summary.track_id) - 1);
        disabled_summary.enabled = 0;
        disabled_summary.muted = 1;
        if (room_service_app_server_sync_apply_track_subscription(
                g_room_service_server, room_id, &disabled_summary) != 0) {
            if (warning_code) {
                *warning_code = "SFU_SYNC_FAILED";
            }
            if (warning_message) {
                *warning_message =
                    "subscription state committed locally; track unsubscribe was not forwarded to sfu node";
            }
            return -1;
        }
    }

    return 0;
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

static char *sfu_replay_stats_json(const room_service_sfu_replay_stats_t *stats) {
    char *json;

    if (!stats) {
        return NULL;
    }

    json = (char *)malloc(256);
    if (!json) {
        return NULL;
    }

    snprintf(json, 256,
             "{"
             "\"participants_replayed\":%d,"
             "\"receiver_bandwidths_replayed\":%d,"
             "\"tracks_replayed\":%d,"
             "\"subscriptions_replayed\":%d,"
             "\"recordings_replayed\":%d,"
             "\"skipped_items\":%d"
             "}",
             stats->participants_replayed,
             stats->receiver_bandwidths_replayed,
             stats->tracks_replayed,
             stats->subscriptions_replayed,
             stats->recordings_replayed,
             stats->skipped_items);
    return json;
}

static char *room_sync_diagnostic_json(room_service_app_server_t *server,
                                       const char *room_id) {
    room_service_room_sync_diagnostic_t diagnostic;
    char *stats_json;
    char *json;
    size_t len;

    if (!server || !room_id) {
        return NULL;
    }

    if (room_service_app_server_get_room_sync_diagnostic(server, room_id, &diagnostic) != 0 ||
        !diagnostic.available) {
        json = (char *)malloc(5);
        if (!json) {
            return NULL;
        }
        memcpy(json, "null", 5);
        return json;
    }

    stats_json = sfu_replay_stats_json(&diagnostic.replay_stats);
    if (!stats_json) {
        return NULL;
    }

    len = strlen(diagnostic.room_id) + strlen(diagnostic.operation) + strlen(stats_json) + 192;
    if (diagnostic.had_warning) {
        len += strlen(diagnostic.warning_code) + strlen(diagnostic.warning_message) + 8;
    }

    json = (char *)malloc(len);
    if (!json) {
        free(stats_json);
        return NULL;
    }

    if (diagnostic.had_warning) {
        snprintf(json, len,
                 "{"
                 "\"room_id\":\"%s\","
                 "\"operation\":\"%s\","
                 "\"sequence\":%lld,"
                 "\"had_warning\":true,"
                 "\"warning_code\":\"%s\","
                 "\"warning_message\":\"%s\","
                 "\"sfu_replay_stats\":%s"
                 "}",
                 diagnostic.room_id,
                 diagnostic.operation,
                 (long long)diagnostic.sequence,
                 diagnostic.warning_code,
                 diagnostic.warning_message,
                 stats_json);
    } else {
        snprintf(json, len,
                 "{"
                 "\"room_id\":\"%s\","
                 "\"operation\":\"%s\","
                 "\"sequence\":%lld,"
                 "\"had_warning\":false,"
                 "\"warning_code\":null,"
                 "\"warning_message\":null,"
                 "\"sfu_replay_stats\":%s"
                 "}",
                 diagnostic.room_id,
                 diagnostic.operation,
                 (long long)diagnostic.sequence,
                 stats_json);
    }

    free(stats_json);
    return json;
}

static char *room_sync_view_json(room_service_app_server_t *server,
                                 turbo_room_service_t *service,
                                 const char *room_id) {
    char *room_json;
    char *last_sync_json;
    char *json;
    int has_last_sync;
    size_t len;

    if (!server || !service || !room_id) {
        return NULL;
    }

    room_json = room_summary_json(service, room_id);
    if (!room_json) {
        return NULL;
    }

    last_sync_json = room_sync_diagnostic_json(server, room_id);
    if (!last_sync_json) {
        free(room_json);
        return NULL;
    }

    has_last_sync = strcmp(last_sync_json, "null") != 0;
    len = strlen(room_id) + strlen(room_json) + strlen(last_sync_json) + 96;
    json = (char *)malloc(len);
    if (!json) {
        free(room_json);
        free(last_sync_json);
        return NULL;
    }

    snprintf(json, len,
             "{"
             "\"room_id\":\"%s\","
             "\"room\":%s,"
             "\"has_last_sfu_sync\":%s,"
             "\"last_sfu_sync\":%s"
             "}",
             room_id, room_json, has_last_sync ? "true" : "false", last_sync_json);

    free(room_json);
    free(last_sync_json);
    return json;
}

static void send_room_sync_result_json(Res *res, turbo_room_service_t *service,
                                       const char *room_id, const char *result_field_name,
                                       const room_service_sfu_replay_stats_t *stats,
                                       const char *warning_code,
                                       const char *warning_message) {
    char *room_json;
    char *stats_json;
    char *payload;
    size_t len;

    if (!res || !service || !room_id || !result_field_name || !stats) {
        send_error_json(res, 500, "INTERNAL_ERROR", "room sync result unavailable");
        return;
    }

    room_json = room_summary_json(service, room_id);
    stats_json = sfu_replay_stats_json(stats);
    if (!room_json || !stats_json) {
        free(room_json);
        free(stats_json);
        send_error_json(res, 500, "INTERNAL_ERROR", "room sync result unavailable");
        return;
    }

    len = strlen(room_json) + strlen(result_field_name) + strlen(stats_json) + 64;
    if (warning_code && warning_message) {
        len += strlen(warning_code) + strlen(warning_message) + 48;
    }

    payload = (char *)malloc(len);
    if (!payload) {
        free(room_json);
        free(stats_json);
        send_error_json(res, 500, "INTERNAL_ERROR", "room sync result unavailable");
        return;
    }

    if (warning_code && warning_message) {
        snprintf(payload, len,
                 "{"
                 "\"ok\":true,"
                 "\"room\":%s,"
                 "\"%s\":%s,"
                 "\"warning_code\":\"%s\","
                 "\"warning\":\"%s\""
                 "}",
                 room_json, result_field_name, stats_json, warning_code, warning_message);
    } else {
        snprintf(payload, len,
                 "{"
                 "\"ok\":true,"
                 "\"room\":%s,"
                 "\"%s\":%s"
                 "}",
                 room_json, result_field_name, stats_json);
    }

    send_json(res, 200, payload);
    free(payload);
    free(room_json);
    free(stats_json);
}

static char *conference_policy_apply_result_json(
    const room_service_conference_policy_apply_result_t *result) {
    char *json;

    if (!result) {
        return NULL;
    }

    json = (char *)malloc(256);
    if (!json) {
        return NULL;
    }

    snprintf(json, 256,
             "{"
             "\"subscriptions_applied\":%d,"
             "\"subscriptions_removed\":%d,"
             "\"receiver_bandwidths_reconciled\":%d"
             "}",
             result->subscriptions_applied,
             result->subscriptions_removed,
             result->receiver_bandwidths_reconciled);
    return json;
}

static void send_conference_policy_result_json(
    Res *res, room_service_app_server_t *server, turbo_room_service_t *service,
    const char *room_id,
    const room_service_conference_policy_apply_result_t *apply_result) {
    char *room_json;
    char *policy_json;
    char *apply_json = NULL;
    char *payload;
    size_t len;
    const char *warning_code = NULL;
    const char *warning_message = NULL;

    if (!res || !server || !service || !room_id) {
        send_error_json(res, 500, "INTERNAL_ERROR", "conference policy unavailable");
        return;
    }

    room_json = room_summary_json(service, room_id);
    policy_json = conference_policy_json(server, room_id);
    if (apply_result) {
        apply_json = conference_policy_apply_result_json(apply_result);
        if (apply_result->had_warning) {
            warning_code = apply_result->warning_code;
            warning_message = apply_result->warning_message;
        }
    }

    if (!room_json || !policy_json || (apply_result && !apply_json)) {
        free(room_json);
        free(policy_json);
        free(apply_json);
        send_error_json(res, 500, "INTERNAL_ERROR", "conference policy unavailable");
        return;
    }

    len = strlen(room_json) + strlen(policy_json) + 64;
    if (apply_json) {
        len += strlen(apply_json) + 32;
    }
    if (warning_code && warning_message) {
        len += strlen(warning_code) + strlen(warning_message) + 48;
    }

    payload = (char *)malloc(len);
    if (!payload) {
        free(room_json);
        free(policy_json);
        free(apply_json);
        send_error_json(res, 500, "INTERNAL_ERROR", "conference policy unavailable");
        return;
    }

    if (apply_json && warning_code && warning_message) {
        snprintf(payload, len,
                 "{"
                 "\"ok\":true,"
                 "\"room\":%s,"
                 "\"conference_policy\":%s,"
                 "\"policy_apply_result\":%s,"
                 "\"warning_code\":\"%s\","
                 "\"warning\":\"%s\""
                 "}",
                 room_json, policy_json, apply_json, warning_code, warning_message);
    } else if (apply_json) {
        snprintf(payload, len,
                 "{"
                 "\"ok\":true,"
                 "\"room\":%s,"
                 "\"conference_policy\":%s,"
                 "\"policy_apply_result\":%s"
                 "}",
                 room_json, policy_json, apply_json);
    } else {
        snprintf(payload, len,
                 "{"
                 "\"ok\":true,"
                 "\"room\":%s,"
                 "\"conference_policy\":%s"
                 "}",
                 room_json, policy_json);
    }

    send_json(res, 200, payload);
    free(payload);
    free(apply_json);
    free(policy_json);
    free(room_json);
}

static void send_call_center_policy_result_json(
    Res *res, turbo_room_service_t *service, const char *room_id,
    const char *supervisor_mode,
    const room_service_conference_policy_apply_result_t *apply_result) {
    char *room_json;
    char *apply_json = NULL;
    char *payload;
    size_t len;
    const char *warning_code = NULL;
    const char *warning_message = NULL;

    if (!res || !service || !room_id || !supervisor_mode) {
        send_error_json(res, 500, "INTERNAL_ERROR", "call-center policy unavailable");
        return;
    }

    room_json = room_summary_json(service, room_id);
    if (apply_result) {
        apply_json = conference_policy_apply_result_json(apply_result);
        if (apply_result->had_warning) {
            warning_code = apply_result->warning_code;
            warning_message = apply_result->warning_message;
        }
    }

    if (!room_json || (apply_result && !apply_json)) {
        free(room_json);
        free(apply_json);
        send_error_json(res, 500, "INTERNAL_ERROR", "call-center policy unavailable");
        return;
    }

    len = strlen(room_json) + strlen(supervisor_mode) + 96;
    if (apply_json) {
        len += strlen(apply_json) + 32;
    }
    if (warning_code && warning_message) {
        len += strlen(warning_code) + strlen(warning_message) + 48;
    }

    payload = (char *)malloc(len);
    if (!payload) {
        free(room_json);
        free(apply_json);
        send_error_json(res, 500, "INTERNAL_ERROR", "call-center policy unavailable");
        return;
    }

    if (apply_json && warning_code && warning_message) {
        snprintf(payload, len,
                 "{"
                 "\"ok\":true,"
                 "\"room\":%s,"
                 "\"call_center_policy\":{\"supervisor_mode\":\"%s\"},"
                 "\"policy_apply_result\":%s,"
                 "\"warning_code\":\"%s\","
                 "\"warning\":\"%s\""
                 "}",
                 room_json, supervisor_mode, apply_json, warning_code, warning_message);
    } else if (apply_json) {
        snprintf(payload, len,
                 "{"
                 "\"ok\":true,"
                 "\"room\":%s,"
                 "\"call_center_policy\":{\"supervisor_mode\":\"%s\"},"
                 "\"policy_apply_result\":%s"
                 "}",
                 room_json, supervisor_mode, apply_json);
    } else {
        snprintf(payload, len,
                 "{"
                 "\"ok\":true,"
                 "\"room\":%s,"
                 "\"call_center_policy\":{\"supervisor_mode\":\"%s\"}"
                 "}",
                 room_json, supervisor_mode);
    }

    send_json(res, 200, payload);
    free(payload);
    free(apply_json);
    free(room_json);
}

static json_value_t *parse_request_object(Req *req, Res *res) {
    json_value_t *root = NULL;

    if (!req->body || req->body_len == 0) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing request body");
        return NULL;
    }

    if (turbo_parse_json((const uint8_t *)req->body, req->body_len, &root) != 0 ||
        !root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
        send_error_json(res, 400, "INVALID_JSON", "request body must be a JSON object");
        turbo_free_json(&root);
        return NULL;
    }

    return root;
}

static const json_value_t *object_field_or_self(const json_value_t *root,
                                                const char *field_name) {
    json_value_t *field;

    if (!root || !field_name) {
        return root;
    }

    field = turbo_json_object_get(root, field_name);
    if (field && turbo_json_type(field) == TURBO_JSON_OBJECT) {
        return field;
    }

    return root;
}

static int ensure_room_exists(turbo_room_service_t *service, const char *room_id,
                              turbo_room_type_t room_type, const char *created_by) {
    turbo_room_summary_t summary;
    turbo_room_config_t config;

    if (!service || !room_id || room_type == 0) {
        return -1;
    }

    if (turbo_room_service_get_room_summary(service, room_id, &summary) == 0) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    config.room_id = room_id;
    config.room_type = room_type;
    config.created_by = created_by;
    return turbo_room_service_create_room(service, &config);
}

static int ensure_participant_joined(turbo_room_service_t *service, const char *room_id,
                                     const json_value_t *participant_obj,
                                     const char **out_participant_id,
                                     const char **warning_code,
                                     const char **warning_message) {
    turbo_room_participant_summary_t summary;
    turbo_room_participant_config_t config;
    room_service_conference_policy_apply_result_t apply_result;

    if (out_participant_id) {
        *out_participant_id = NULL;
    }
    if (!service || !room_id || !participant_obj) {
        return -1;
    }

    memset(&config, 0, sizeof(config));
    config.participant_id = json_string_field(participant_obj, "participant_id");
    config.user_id = json_string_field(participant_obj, "user_id");
    config.display_name = json_string_field(participant_obj, "display_name");
    config.role = parse_role(json_string_field(participant_obj, "role"));
    if (!config.participant_id || config.role == 0) {
        return -1;
    }
    if (out_participant_id) {
        *out_participant_id = config.participant_id;
    }

    if (turbo_room_service_get_participant_summary(service, room_id,
                                                   config.participant_id,
                                                   &summary) == 0) {
        return 0;
    }

    if (turbo_room_service_add_participant(service, room_id, &config) != 0) {
        return -1;
    }

    if (room_service_app_server_sync_add_session(g_room_service_server, room_id,
                                                 config.participant_id) != 0) {
        if (warning_code && !*warning_code) {
            *warning_code = "SFU_SYNC_FAILED";
        }
        if (warning_message && !*warning_message) {
            *warning_message =
                "participant state committed locally; add_session was not forwarded to sfu node";
        }
    } else {
        sync_derived_receiver_bandwidth_if_needed(service, room_id, config.participant_id,
                                                  warning_code, warning_message);
    }

    memset(&apply_result, 0, sizeof(apply_result));
    if (reconcile_call_center_policy_for_room(g_room_service_server, service, room_id,
                                              &apply_result) != 0) {
        if (warning_code && !*warning_code) {
            *warning_code = "POLICY_RECONCILE_FAILED";
        }
        if (warning_message && !*warning_message) {
            *warning_message =
                "participant state committed locally; call-center subscriptions were not reconciled";
        }
    } else if (apply_result.had_warning) {
        if (warning_code && !*warning_code) {
            *warning_code = apply_result.warning_code;
        }
        if (warning_message && !*warning_message) {
            *warning_message = apply_result.warning_message;
        }
    }

    if (is_call_center_observer_role(config.role)) {
        record_call_center_event_if_room(g_room_service_server, service, room_id,
                                         "observer_joined", config.participant_id, "",
                                         participant_role_name(config.role), "join");
    }

    return 0;
}

static int publish_track_from_request(turbo_room_service_t *service, const char *room_id,
                                      const json_value_t *track_obj,
                                      const char **out_track_id,
                                      const char **warning_code,
                                      const char **warning_message) {
    turbo_room_track_config_t config;
    turbo_room_summary_t room_summary;
    turbo_room_track_summary_t track_summary;
    room_service_conference_policy_apply_result_t apply_result;
    uint32_t layer_ssrcs[3] = {0};

    if (out_track_id) {
        *out_track_id = NULL;
    }
    if (!service || !room_id || !track_obj) {
        return -1;
    }

    memset(&config, 0, sizeof(config));
    config.track_id = json_string_field(track_obj, "track_id");
    config.owner_participant_id = json_string_field(track_obj, "owner_participant_id");
    if (!config.owner_participant_id) {
        config.owner_participant_id = json_string_field(track_obj, "participant_id");
    }
    config.kind = parse_track_kind(json_string_field(track_obj, "kind"));
    config.source = parse_track_source(json_string_field(track_obj, "source"));
    config.codec_name = json_string_field(track_obj, "codec_name");
    config.simulcast_enabled = json_bool_field(track_obj, "simulcast_enabled", 0);
    (void)json_uint32_field(track_obj, "main_ssrc", &config.main_ssrc);
    if (json_uint32_array_field(track_obj, "layer_ssrcs", layer_ssrcs, 3,
                                &config.layer_count) != 0) {
        return -1;
    }
    config.layer_ssrcs = layer_ssrcs;
    if (!config.track_id || !config.owner_participant_id || config.kind == 0 ||
        config.source == 0 || !config.codec_name) {
        return -1;
    }
    if (out_track_id) {
        *out_track_id = config.track_id;
    }

    if (turbo_room_service_get_track_summary(service, room_id, config.track_id,
                                             &track_summary) != 0 &&
        turbo_room_service_publish_track(service, room_id, &config) != 0) {
        return -1;
    }

    if (turbo_room_service_get_room_summary(service, room_id, &room_summary) == 0 &&
        room_summary.assigned_sfu_node[0] != '\0') {
        if (turbo_room_service_get_track_summary(service, room_id, config.track_id,
                                                 &track_summary) != 0 ||
            track_summary.main_ssrc == 0) {
            if (warning_code && !*warning_code) {
                *warning_code = "SFU_SYNC_SKIPPED";
            }
            if (warning_message && !*warning_message) {
                *warning_message =
                    "track state committed locally; publish_track lacks SSRC metadata for sfu sync";
            }
        } else if (room_service_app_server_sync_register_track(g_room_service_server, room_id,
                                                               &track_summary) != 0) {
            if (warning_code && !*warning_code) {
                *warning_code = "SFU_SYNC_FAILED";
            }
            if (warning_message && !*warning_message) {
                *warning_message =
                    "track state committed locally; register_published_track was not forwarded to sfu node";
            }
        }
    }

    memset(&apply_result, 0, sizeof(apply_result));
    if (reconcile_call_center_policy_for_room(g_room_service_server, service, room_id,
                                              &apply_result) != 0) {
        if (warning_code && !*warning_code) {
            *warning_code = "POLICY_RECONCILE_FAILED";
        }
        if (warning_message && !*warning_message) {
            *warning_message =
                "track state committed locally; call-center subscriptions were not reconciled";
        }
    } else if (apply_result.had_warning) {
        if (warning_code && !*warning_code) {
            *warning_code = apply_result.warning_code;
        }
        if (warning_message && !*warning_message) {
            *warning_message = apply_result.warning_message;
        }
    }

    return 0;
}

static int subscribe_track_from_request(turbo_room_service_t *service, const char *room_id,
                                        const json_value_t *subscription_obj,
                                        const char **out_subscriber_id,
                                        const char **out_track_id,
                                        const char **warning_code,
                                        const char **warning_message) {
    turbo_room_subscription_config_t config;

    if (out_subscriber_id) {
        *out_subscriber_id = NULL;
    }
    if (out_track_id) {
        *out_track_id = NULL;
    }
    if (!service || !room_id || !subscription_obj) {
        return -1;
    }

    memset(&config, 0, sizeof(config));
    config.subscriber_participant_id =
        json_string_field(subscription_obj, "subscriber_participant_id");
    if (!config.subscriber_participant_id) {
        config.subscriber_participant_id =
            json_string_field(subscription_obj, "participant_id");
    }
    config.track_id = json_string_field(subscription_obj, "track_id");
    config.enabled = json_bool_field(subscription_obj, "enabled", 1);
    config.priority = turbo_json_get_int(subscription_obj, "priority", 0);
    config.preferred_layer =
        parse_video_layer(json_string_field(subscription_obj, "preferred_layer"));
    config.target_layer =
        parse_video_layer(json_string_field(subscription_obj, "target_layer"));
    config.muted = json_bool_field(subscription_obj, "muted", 0);
    config.policy_source = json_string_field(subscription_obj, "policy_source");
    if (!config.subscriber_participant_id || !config.track_id) {
        return -1;
    }
    if (out_subscriber_id) {
        *out_subscriber_id = config.subscriber_participant_id;
    }
    if (out_track_id) {
        *out_track_id = config.track_id;
    }

    if (turbo_room_service_set_subscription(service, room_id, &config) != 0) {
        return -1;
    }

    sync_track_subscription_if_needed(service, room_id, config.subscriber_participant_id,
                                      config.track_id, warning_code, warning_message);
    sync_derived_receiver_bandwidth_if_needed(service, room_id,
                                              config.subscriber_participant_id,
                                              warning_code, warning_message);
    return 0;
}

static void send_facade_entity_json(Res *res, const char *field_name, char *entity_json,
                                    turbo_room_service_t *service, const char *room_id,
                                    const char *warning_code,
                                    const char *warning_message) {
    char *room_json = NULL;
    char *payload;
    size_t len;

    if (!field_name || !entity_json || !service || !room_id) {
        free(entity_json);
        send_error_json(res, 500, "INTERNAL_ERROR", "facade result unavailable");
        return;
    }

    room_json = room_summary_json(service, room_id);
    if (!room_json) {
        free(entity_json);
        send_error_json(res, 500, "INTERNAL_ERROR", "room summary unavailable");
        return;
    }

    len = strlen(room_json) + strlen(field_name) + strlen(entity_json) + 48;
    if (warning_code && warning_message) {
        len += strlen(warning_code) + strlen(warning_message) + 48;
    }

    payload = (char *)malloc(len);
    if (!payload) {
        free(room_json);
        free(entity_json);
        send_error_json(res, 500, "INTERNAL_ERROR", "facade result unavailable");
        return;
    }

    if (warning_code && warning_message) {
        snprintf(payload, len,
                 "{"
                 "\"ok\":true,"
                 "\"room\":%s,"
                 "\"%s\":%s,"
                 "\"warning_code\":\"%s\","
                 "\"warning\":\"%s\""
                 "}",
                 room_json, field_name, entity_json, warning_code, warning_message);
    } else {
        snprintf(payload, len,
                 "{"
                 "\"ok\":true,"
                 "\"room\":%s,"
                 "\"%s\":%s"
                 "}",
                 room_json, field_name, entity_json);
    }

    send_json(res, 200, payload);
    free(payload);
    free(room_json);
    free(entity_json);
}

static void handle_join(Req *req, Res *res) {
    turbo_room_service_t *service;
    json_value_t *root;
    const json_value_t *participant_obj;
    const char *room_id;
    const char *participant_id = NULL;
    const char *warning_code = NULL;
    const char *warning_message = NULL;
    char *participant_json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }
    service = room_service_app_server_get_service(g_room_service_server);
    root = parse_request_object(req, res);
    if (!service || !root) {
        turbo_free_json(&root);
        return;
    }

    room_id = json_string_field(root, "room_id");
    participant_obj = object_field_or_self(root, "participant");
    participant_id = json_string_field(participant_obj, "participant_id");
    if (require_control_auth(req, res, ROOM_SERVICE_SCOPE_CONTROL_WRITE,
                             room_id, participant_id) != 0) {
        turbo_free_json(&root);
        return;
    }
    if (!room_id ||
        ensure_room_exists(service, room_id, parse_room_type(json_string_field(root, "room_type")),
                           json_string_field(root, "created_by")) != 0 ||
        ensure_participant_joined(service, room_id, participant_obj, &participant_id,
                                  &warning_code, &warning_message) != 0) {
        send_error_json(res, 400, "INVALID_REQUEST", "invalid join payload");
        turbo_free_json(&root);
        return;
    }

    participant_json = participant_summary_json(service, room_id, participant_id);
    if (!participant_json) {
        send_error_json(res, 500, "INTERNAL_ERROR", "participant summary unavailable");
        turbo_free_json(&root);
        return;
    }

    send_facade_entity_json(res, "participant", participant_json, service, room_id,
                            warning_code, warning_message);
    turbo_free_json(&root);
}

static void handle_publish(Req *req, Res *res) {
    turbo_room_service_t *service;
    json_value_t *root;
    const json_value_t *track_obj;
    const char *room_id;
    const char *track_id = NULL;
    const char *warning_code = NULL;
    const char *warning_message = NULL;
    char *track_json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }
    service = room_service_app_server_get_service(g_room_service_server);
    root = parse_request_object(req, res);
    if (!service || !root) {
        turbo_free_json(&root);
        return;
    }

    room_id = json_string_field(root, "room_id");
    track_obj = object_field_or_self(root, "track");
    if (require_control_auth(
            req, res, ROOM_SERVICE_SCOPE_CONTROL_WRITE, room_id,
            json_string_field(track_obj, "owner_participant_id")) != 0) {
        turbo_free_json(&root);
        return;
    }
    if (!room_id ||
        publish_track_from_request(service, room_id, track_obj, &track_id,
                                   &warning_code, &warning_message) != 0) {
        send_error_json(res, 400, "INVALID_REQUEST", "invalid publish payload");
        turbo_free_json(&root);
        return;
    }

    track_json = track_summary_json(service, room_id, track_id);
    if (!track_json) {
        send_error_json(res, 500, "INTERNAL_ERROR", "track summary unavailable");
        turbo_free_json(&root);
        return;
    }

    send_facade_entity_json(res, "track", track_json, service, room_id,
                            warning_code, warning_message);
    turbo_free_json(&root);
}

static void handle_subscribe(Req *req, Res *res) {
    turbo_room_service_t *service;
    json_value_t *root;
    const json_value_t *subscription_obj;
    const char *room_id;
    const char *subscriber_participant_id = NULL;
    const char *track_id = NULL;
    const char *warning_code = NULL;
    const char *warning_message = NULL;
    char *subscription_json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }
    service = room_service_app_server_get_service(g_room_service_server);
    root = parse_request_object(req, res);
    if (!service || !root) {
        turbo_free_json(&root);
        return;
    }

    room_id = json_string_field(root, "room_id");
    subscription_obj = object_field_or_self(root, "subscription");
    if (require_control_auth(
            req, res, ROOM_SERVICE_SCOPE_CONTROL_WRITE, room_id,
            json_string_field(subscription_obj,
                              "subscriber_participant_id")) != 0) {
        turbo_free_json(&root);
        return;
    }
    if (!room_id ||
        subscribe_track_from_request(service, room_id, subscription_obj,
                                     &subscriber_participant_id, &track_id,
                                     &warning_code, &warning_message) != 0) {
        send_error_json(res, 400, "INVALID_REQUEST", "invalid subscribe payload");
        turbo_free_json(&root);
        return;
    }

    subscription_json = subscription_summary_json(service, room_id,
                                                  subscriber_participant_id, track_id);
    if (!subscription_json) {
        send_error_json(res, 500, "INTERNAL_ERROR", "subscription summary unavailable");
        turbo_free_json(&root);
        return;
    }

    send_facade_entity_json(res, "subscription", subscription_json, service, room_id,
                            warning_code, warning_message);
    turbo_free_json(&root);
}

static void handle_health(Req *req, Res *res) {
    (void)req;
    send_text(res, 200, "OK");
}

static void handle_metrics(Req *req, Res *res) {
    room_service_app_stats_t stats;
    room_service_ivr_metrics_t ivr;
    const room_service_app_config_t *config;
    enum { ROOM_SERVICE_METRICS_CAPACITY = 4096 };
    char text[ROOM_SERVICE_METRICS_CAPACITY];
    int written;

    (void)req;

    if (!g_room_service_server) {
        send_text(res, 500, "room service not initialized\n");
        return;
    }

    memset(&stats, 0, sizeof(stats));
    if (room_service_app_server_get_stats(g_room_service_server, &stats) != 0) {
        send_text(res, 500, "room service unavailable\n");
        return;
    }
    if (room_service_app_server_get_ivr_metrics(g_room_service_server, &ivr) !=
        0) {
        send_text(res, 500, "room service IVR metrics unavailable\n");
        return;
    }

    config = room_service_app_server_get_config(g_room_service_server);
    written = snprintf(text, sizeof(text),
             "# TYPE turbo_room_service_up gauge\n"
             "turbo_room_service_up %d\n"
             "# TYPE turbo_room_service_control_auth_enabled gauge\n"
             "turbo_room_service_control_auth_enabled %d\n"
             "# TYPE turbo_room_service_sfu_control_configured gauge\n"
             "turbo_room_service_sfu_control_configured %d\n"
             "# TYPE turbo_room_service_room_sync_diagnostics gauge\n"
             "turbo_room_service_room_sync_diagnostics %d\n"
             "# TYPE turbo_room_service_call_center_events gauge\n"
             "turbo_room_service_call_center_events %d\n"
             "# TYPE turbo_room_service_conference_policies gauge\n"
             "turbo_room_service_conference_policies %d\n"
             "# TYPE turbo_room_service_sfu_nodes gauge\n"
             "turbo_room_service_sfu_nodes %d\n"
             "# TYPE turbo_room_service_ivr_enabled gauge\n"
             "turbo_room_service_ivr_enabled %d\n"
             "# TYPE turbo_room_service_ivr_workers gauge\n"
             "turbo_room_service_ivr_workers %u\n"
             "# TYPE turbo_room_service_ivr_worker_capacity gauge\n"
             "turbo_room_service_ivr_worker_capacity %u\n"
             "# TYPE turbo_room_service_ivr_worker_high_water gauge\n"
             "turbo_room_service_ivr_worker_high_water %u\n"
             "# TYPE turbo_room_service_ivr_assignments gauge\n"
             "turbo_room_service_ivr_assignments %u\n"
             "# TYPE turbo_room_service_ivr_assignment_capacity gauge\n"
             "turbo_room_service_ivr_assignment_capacity %u\n"
             "# TYPE turbo_room_service_ivr_assignment_high_water gauge\n"
             "turbo_room_service_ivr_assignment_high_water %u\n"
             "# TYPE turbo_room_service_ivr_lease_expired_total counter\n"
             "turbo_room_service_ivr_lease_expired_total %llu\n"
             "# TYPE turbo_room_service_ivr_dispatch_timeout_total counter\n"
             "turbo_room_service_ivr_dispatch_timeout_total %llu\n"
             "# TYPE turbo_room_service_ivr_release_timeout_total counter\n"
             "turbo_room_service_ivr_release_timeout_total %llu\n"
             "# TYPE turbo_room_service_ivr_request_queue_items gauge\n"
             "turbo_room_service_ivr_request_queue_items %u\n"
             "# TYPE turbo_room_service_ivr_request_queue_capacity gauge\n"
             "turbo_room_service_ivr_request_queue_capacity %u\n"
             "# TYPE turbo_room_service_ivr_request_queue_high_water gauge\n"
             "turbo_room_service_ivr_request_queue_high_water %u\n"
             "# TYPE turbo_room_service_ivr_request_queue_drops_total counter\n"
             "turbo_room_service_ivr_request_queue_drops_total %llu\n"
             "# TYPE turbo_room_service_ivr_peer_event_queue_items gauge\n"
             "turbo_room_service_ivr_peer_event_queue_items %u\n"
             "# TYPE turbo_room_service_ivr_peer_event_queue_capacity gauge\n"
             "turbo_room_service_ivr_peer_event_queue_capacity %u\n"
             "# TYPE turbo_room_service_ivr_peer_event_queue_high_water gauge\n"
             "turbo_room_service_ivr_peer_event_queue_high_water %u\n"
             "# TYPE turbo_room_service_ivr_peer_event_queue_drops_total counter\n"
             "turbo_room_service_ivr_peer_event_queue_drops_total %llu\n"
             "# TYPE turbo_room_service_ivr_peer_event_queue_overflowed gauge\n"
             "turbo_room_service_ivr_peer_event_queue_overflowed %d\n",
             stats.running ? 1 : 0,
             control_auth_enabled(config) ? 1 : 0,
             (config && config->sfu_control_token && config->sfu_control_token[0] != '\0') ? 1 : 0,
             stats.room_sync_diagnostic_count,
             stats.call_center_event_count,
             stats.conference_policy_count,
             stats.sfu_node_count,
             ivr.enabled,
             ivr.workers,
             ivr.worker_capacity,
             ivr.worker_high_water,
             ivr.assignments,
             ivr.assignment_capacity,
             ivr.assignment_high_water,
             (unsigned long long)ivr.lease_expired_total,
             (unsigned long long)ivr.dispatch_timeout_total,
             (unsigned long long)ivr.release_timeout_total,
             ivr.request_queue_items,
             ivr.request_queue_capacity,
             ivr.request_queue_high_water,
             (unsigned long long)ivr.request_queue_drops_total,
             ivr.peer_event_queue_items,
             ivr.peer_event_queue_capacity,
             ivr.peer_event_queue_high_water,
             (unsigned long long)ivr.peer_event_queue_drops_total,
             ivr.peer_event_queue_overflowed);

    if (written < 0 || (size_t)written >= sizeof(text)) {
        send_text(res, 500, "room service metrics overflow\n");
        return;
    }

    send_text(res, 200, text);
}

static void handle_get_room(Req *req, Res *res) {
    turbo_room_service_t *service;
    const char *room_id = get_params(req, "id");
    char *json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }

    service = room_service_app_server_get_service(g_room_service_server);
    if (!room_id) {
        room_id = get_query(req, "room_id");
    }
    if (!room_id) {
        room_id = path_tail(req->path);
    }
    if (!service || !room_id) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing room id");
        return;
    }

    json = room_summary_json(service, room_id);
    if (!json) {
        send_error_json(res, 404, "ROOM_NOT_FOUND", "room not found");
        return;
    }

    send_json(res, 200, json);
    free(json);
}

static void handle_get_participant(Req *req, Res *res) {
    turbo_room_service_t *service;
    const char *room_id = get_params(req, "id");
    const char *participant_id = get_params(req, "participant_id");
    char *json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }

    service = room_service_app_server_get_service(g_room_service_server);
    if (!room_id) {
        room_id = get_query(req, "room_id");
    }
    if (!participant_id) {
        participant_id = get_query(req, "participant_id");
    }
    if (!service || !room_id || !participant_id) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing room or participant id");
        return;
    }

    json = participant_summary_json(service, room_id, participant_id);
    if (!json) {
        send_error_json(res, 404, "PARTICIPANT_NOT_FOUND", "participant not found");
        return;
    }

    send_json(res, 200, json);
    free(json);
}

static int parse_participant_bandwidth_diagnostic_path(const char *path, char *room_id,
                                                       size_t room_id_size,
                                                       char *participant_id,
                                                       size_t participant_id_size) {
    const char *prefix = "/api/v1/rooms/";
    const char *room_start;
    const char *room_end;
    const char *participant_start;
    size_t len;

    if (!path || !room_id || !participant_id) {
        return -1;
    }

    room_id[0] = '\0';
    participant_id[0] = '\0';

    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    room_start = path + strlen(prefix);
    room_end = strchr(room_start, '/');
    if (!room_end || strncmp(room_end, "/participant_bandwidth_diagnostic/", 34) != 0) {
        return -1;
    }

    len = (size_t)(room_end - room_start);
    if (len == 0 || len >= room_id_size) {
        return -1;
    }
    memcpy(room_id, room_start, len);
    room_id[len] = '\0';

    participant_start = room_end + 34;
    if (!*participant_start) {
        return -1;
    }

    len = strlen(participant_start);
    if (len >= participant_id_size) {
        return -1;
    }
    memcpy(participant_id, participant_start, len + 1);
    return 0;
}

static int parse_room_diagnostic_path(const char *path, char *room_id,
                                      size_t room_id_size) {
    const char *prefix = "/api/v1/rooms/";
    const char *room_start;
    const char *room_end;
    size_t len;

    if (!path || !room_id) {
        return -1;
    }

    room_id[0] = '\0';
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    room_start = path + strlen(prefix);
    room_end = strchr(room_start, '/');
    if (!room_end || strcmp(room_end, "/room_diagnostic") != 0) {
        return -1;
    }

    len = (size_t)(room_end - room_start);
    if (len == 0 || len >= room_id_size) {
        return -1;
    }

    memcpy(room_id, room_start, len);
    room_id[len] = '\0';
    return 0;
}

static int parse_room_sync_diagnostic_path(const char *path, char *room_id,
                                           size_t room_id_size) {
    const char *prefix = "/api/v1/rooms/";
    const char *room_start;
    const char *room_end;
    size_t len;

    if (!path || !room_id) {
        return -1;
    }

    room_id[0] = '\0';
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    room_start = path + strlen(prefix);
    room_end = strchr(room_start, '/');
    if (!room_end || strcmp(room_end, "/room_sync_diagnostic") != 0) {
        return -1;
    }

    len = (size_t)(room_end - room_start);
    if (len == 0 || len >= room_id_size) {
        return -1;
    }

    memcpy(room_id, room_start, len);
    room_id[len] = '\0';
    return 0;
}

static int parse_room_state_path(const char *path, char *room_id,
                                 size_t room_id_size) {
    const char *prefix = "/api/v1/rooms/";
    const char *room_start;
    const char *room_end;
    size_t len;

    if (!path || !room_id) {
        return -1;
    }

    room_id[0] = '\0';
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    room_start = path + strlen(prefix);
    room_end = strchr(room_start, '/');
    if (!room_end || strcmp(room_end, "/state") != 0) {
        return -1;
    }

    len = (size_t)(room_end - room_start);
    if (len == 0 || len >= room_id_size) {
        return -1;
    }

    memcpy(room_id, room_start, len);
    room_id[len] = '\0';
    return 0;
}

static int parse_call_center_events_path(const char *path, char *room_id,
                                         size_t room_id_size) {
    const char *prefix = "/api/v1/rooms/";
    const char *room_start;
    const char *room_end;
    size_t len;

    if (!path || !room_id) {
        return -1;
    }

    room_id[0] = '\0';
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    room_start = path + strlen(prefix);
    room_end = strchr(room_start, '/');
    if (!room_end || strcmp(room_end, "/call_center_events") != 0) {
        return -1;
    }

    len = (size_t)(room_end - room_start);
    if (len == 0 || len >= room_id_size) {
        return -1;
    }

    memcpy(room_id, room_start, len);
    room_id[len] = '\0';
    return 0;
}

static void handle_get_room_diagnostic(Req *req, Res *res) {
    turbo_room_service_t *service;
    const char *room_id = get_params(req, "id");
    char room_id_buf[TURBO_ROOM_ID_MAX];
    char *json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }

    service = room_service_app_server_get_service(g_room_service_server);
    if (!room_id) {
        room_id = get_query(req, "room_id");
    }
    if (!room_id &&
        parse_room_diagnostic_path(req->path, room_id_buf, sizeof(room_id_buf)) == 0) {
        room_id = room_id_buf;
    }
    if (!service || !room_id) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing room_id");
        return;
    }

    json = room_diagnostic_json(g_room_service_server, service, room_id);
    if (!json) {
        send_error_json(res, 404, "ROOM_NOT_FOUND", "room diagnostic not found");
        return;
    }

    send_json(res, 200, json);
    free(json);
}

static void handle_get_room_sync_diagnostic(Req *req, Res *res) {
    turbo_room_service_t *service;
    const char *room_id = get_params(req, "id");
    char room_id_buf[TURBO_ROOM_ID_MAX];
    char *json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }

    service = room_service_app_server_get_service(g_room_service_server);
    if (!room_id) {
        room_id = get_query(req, "room_id");
    }
    if (!room_id &&
        parse_room_sync_diagnostic_path(req->path, room_id_buf, sizeof(room_id_buf)) == 0) {
        room_id = room_id_buf;
    }
    if (!service || !room_id) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing room_id");
        return;
    }

    json = room_sync_view_json(g_room_service_server, service, room_id);
    if (!json) {
        send_error_json(res, 404, "ROOM_NOT_FOUND", "room sync diagnostic not found");
        return;
    }

    send_json(res, 200, json);
    free(json);
}

static void handle_get_room_state(Req *req, Res *res) {
    turbo_room_service_t *service;
    const char *room_id = get_params(req, "id");
    char room_id_buf[TURBO_ROOM_ID_MAX];
    char *json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }

    service = room_service_app_server_get_service(g_room_service_server);
    if (!room_id) {
        room_id = get_query(req, "room_id");
    }
    if (!room_id &&
        parse_room_state_path(req->path, room_id_buf, sizeof(room_id_buf)) == 0) {
        room_id = room_id_buf;
    }
    if (!service || !room_id) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing room_id");
        return;
    }

    json = room_state_json(g_room_service_server, service, room_id);
    if (!json) {
        send_error_json(res, 404, "ROOM_NOT_FOUND", "room state not found");
        return;
    }

    send_json(res, 200, json);
    free(json);
}

static void handle_get_call_center_events(Req *req, Res *res) {
    turbo_room_service_t *service;
    const char *room_id = get_params(req, "id");
    const char *after_sequence_text = get_query(req, "after_sequence");
    const char *limit_text = get_query(req, "limit");
    char room_id_buf[TURBO_ROOM_ID_MAX];
    char *json;
    int64_t after_sequence = 0;
    int limit = 32;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }

    service = room_service_app_server_get_service(g_room_service_server);
    if (!room_id) {
        room_id = get_query(req, "room_id");
    }
    if (!room_id &&
        parse_call_center_events_path(req->path, room_id_buf, sizeof(room_id_buf)) == 0) {
        room_id = room_id_buf;
    }
    if (after_sequence_text && after_sequence_text[0]) {
        after_sequence = strtoll(after_sequence_text, NULL, 10);
    }
    if (limit_text && limit_text[0]) {
        limit = atoi(limit_text);
    }
    if (limit <= 0) {
        limit = 32;
    }
    if (limit > 64) {
        limit = 64;
    }
    if (!service || !room_id) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing room_id");
        return;
    }

    json = call_center_events_json(g_room_service_server, room_id, after_sequence, limit);
    if (!json) {
        send_error_json(res, 500, "INTERNAL_ERROR", "call-center events unavailable");
        return;
    }

    send_json(res, 200, json);
    free(json);
}

static void handle_get_participant_bandwidth_diagnostic(Req *req, Res *res) {
    turbo_room_service_t *service;
    const char *room_id = get_params(req, "id");
    const char *participant_id = get_params(req, "participant_id");
    char room_id_buf[TURBO_ROOM_ID_MAX];
    char participant_id_buf[TURBO_PARTICIPANT_ID_MAX];
    char *json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }

    service = room_service_app_server_get_service(g_room_service_server);
    if (!room_id) {
        room_id = get_query(req, "room_id");
    }
    if (!participant_id) {
        participant_id = get_query(req, "participant_id");
    }
    if ((!room_id || !participant_id) &&
        parse_participant_bandwidth_diagnostic_path(req->path, room_id_buf,
                                                    sizeof(room_id_buf),
                                                    participant_id_buf,
                                                    sizeof(participant_id_buf)) == 0) {
        if (!room_id) {
            room_id = room_id_buf;
        }
        if (!participant_id) {
            participant_id = participant_id_buf;
        }
    }
    if (!service || !room_id || !participant_id) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing room_id or participant_id");
        return;
    }

    json = participant_bandwidth_diagnostic_json(g_room_service_server, service,
                                                 room_id, participant_id, NULL);
    if (!json) {
        send_error_json(res, 404, "PARTICIPANT_NOT_FOUND",
                        "participant bandwidth diagnostic not found");
        return;
    }

    send_json(res, 200, json);
    free(json);
}

static void handle_get_track(Req *req, Res *res) {
    turbo_room_service_t *service;
    const char *room_id = get_params(req, "id");
    const char *track_id = get_params(req, "track_id");
    char *json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }

    service = room_service_app_server_get_service(g_room_service_server);
    if (!room_id) {
        room_id = get_query(req, "room_id");
    }
    if (!track_id) {
        track_id = get_query(req, "track_id");
    }
    if (!service || !room_id || !track_id) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing room or track id");
        return;
    }

    json = track_summary_json(service, room_id, track_id);
    if (!json) {
        send_error_json(res, 404, "TRACK_NOT_FOUND", "track not found");
        return;
    }

    send_json(res, 200, json);
    free(json);
}

static void handle_get_subscription(Req *req, Res *res) {
    turbo_room_service_t *service;
    const char *room_id = get_params(req, "id");
    const char *subscriber_participant_id = get_query(req, "subscriber_participant_id");
    const char *track_id = get_query(req, "track_id");
    char *json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }

    service = room_service_app_server_get_service(g_room_service_server);
    if (!room_id) {
        room_id = get_query(req, "room_id");
    }
    if (!service || !room_id || !subscriber_participant_id || !track_id) {
        send_error_json(res, 400, "INVALID_REQUEST",
                        "missing room_id, subscriber_participant_id, or track_id");
        return;
    }

    json = subscription_summary_json(service, room_id, subscriber_participant_id, track_id);
    if (!json) {
        send_error_json(res, 404, "SUBSCRIPTION_NOT_FOUND", "subscription not found");
        return;
    }

    send_json(res, 200, json);
    free(json);
}

static void handle_get_subscription_diagnostic(Req *req, Res *res) {
    turbo_room_service_t *service;
    const char *room_id = get_params(req, "id");
    const char *subscriber_participant_id = get_params(req, "subscriber_participant_id");
    const char *track_id = get_params(req, "track_id");
    char room_id_buf[TURBO_ROOM_ID_MAX];
    char subscriber_id_buf[TURBO_PARTICIPANT_ID_MAX];
    char track_id_buf[TURBO_TRACK_ID_MAX];
    char *json;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }

    service = room_service_app_server_get_service(g_room_service_server);
    if (!room_id) {
        room_id = get_query(req, "room_id");
    }
    if (!subscriber_participant_id) {
        subscriber_participant_id = get_params(req, "subscriber_id");
    }
    if (!subscriber_participant_id) {
        subscriber_participant_id = get_query(req, "subscriber_participant_id");
    }
    if (!track_id) {
        track_id = get_params(req, "track");
    }
    if (!track_id) {
        track_id = get_query(req, "track_id");
    }
    if ((!room_id || !subscriber_participant_id || !track_id) &&
        parse_subscription_diagnostic_path(req->path, room_id_buf, sizeof(room_id_buf),
                                           subscriber_id_buf, sizeof(subscriber_id_buf),
                                           track_id_buf, sizeof(track_id_buf)) == 0) {
        if (!room_id) {
            room_id = room_id_buf;
        }
        if (!subscriber_participant_id) {
            subscriber_participant_id = subscriber_id_buf;
        }
        if (!track_id) {
            track_id = track_id_buf;
        }
    }
    if (!service || !room_id || !subscriber_participant_id || !track_id) {
        send_error_json(res, 400, "INVALID_REQUEST",
                        "missing room_id, subscriber_participant_id, or track_id");
        return;
    }

    json = subscription_diagnostic_json(g_room_service_server, service, room_id,
                                        subscriber_participant_id, track_id, NULL);
    if (!json) {
        send_error_json(res, 404, "SUBSCRIPTION_NOT_FOUND",
                        "subscription diagnostic not found");
        return;
    }

    send_json(res, 200, json);
    free(json);
}

static void handle_command(Req *req, Res *res) {
    turbo_room_service_t *service;
    json_value_t *root = NULL;
    const char *type;
    const char *room_id;
    const char *auth_participant_id;
    const char *required_scope;
    const room_service_app_config_t *app_config;
    room_service_command_access_t access;
    int rc = -1;
    const char *warning_code = NULL;
    const char *warning_message = NULL;

    if (!g_room_service_server) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service not initialized");
        return;
    }
    if (!req->body || req->body_len == 0) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing request body");
        return;
    }

    service = room_service_app_server_get_service(g_room_service_server);
    if (!service) {
        send_error_json(res, 500, "SERVER_NOT_READY", "room service unavailable");
        return;
    }

    if (turbo_parse_json((const uint8_t *)req->body, req->body_len, &root) != 0 ||
        !root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
        send_error_json(res, 400, "INVALID_JSON", "request body must be a JSON object");
        turbo_free_json(&root);
        return;
    }

    type = json_string_field(root, "type");
    room_id = json_string_field(root, "room_id");
    auth_participant_id = json_string_field(root, "participant_id");
    if (!type) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing command type");
        turbo_free_json(&root);
        return;
    }
    app_config = room_service_app_server_get_config(g_room_service_server);
    access = command_access_for_type(type);
    required_scope =
        access == ROOM_SERVICE_COMMAND_ACCESS_DANGEROUS
            ? ROOM_SERVICE_SCOPE_CONTROL_DANGEROUS
            : ROOM_SERVICE_SCOPE_CONTROL_WRITE;
    if (command_requires_control_auth(type) &&
        !request_has_control_auth(req, app_config, required_scope, room_id,
                                  auth_participant_id)) {
        set_header(res, "WWW-Authenticate", "Bearer");
        send_error_json(res, 401, "UNAUTHORIZED", "control token required");
        turbo_free_json(&root);
        return;
    }

    if (strcmp(type, "enqueue_call_center_queue") == 0) {
        turbo_call_center_queue_entry_config_t config;
        int depth = 0;
        char payload[128];

        memset(&config, 0, sizeof(config));
        config.queue_id = json_string_field(root, "queue_id");
        config.side = parse_call_center_queue_side(json_string_field(root, "side"));
        config.entry_id = json_string_field(root, "entry_id");
        config.endpoint_id = json_string_field(root, "endpoint_id");
        config.priority = turbo_json_get_int(root, "priority", 0);

        if (!config.queue_id || config.side == 0 || !config.entry_id ||
            !config.endpoint_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center queue entry payload");
            turbo_free_json(&root);
            return;
        }

        rc = turbo_room_service_enqueue_call_center_queue_entry(service, &config);
        if (rc == 0) {
            (void)turbo_room_service_get_call_center_queue_depth(
                service, config.queue_id, config.side, &depth);
            snprintf(payload, sizeof(payload), "{\"ok\":true,\"queue_depth\":%d}",
                     depth);
            send_json(res, 200, payload);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "remove_call_center_queue_entry") == 0) {
        const char *queue_id = json_string_field(root, "queue_id");
        turbo_call_center_queue_side_t side =
            parse_call_center_queue_side(json_string_field(root, "side"));
        const char *entry_id = json_string_field(root, "entry_id");

        if (!queue_id || side == 0 || !entry_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center queue remove payload");
            turbo_free_json(&root);
            return;
        }

        rc = turbo_room_service_remove_call_center_queue_entry(service, queue_id, side,
                                                               entry_id);
        if (rc == 0) {
            send_json(res, 200, "{\"ok\":true}");
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "peek_call_center_queue") == 0 ||
               strcmp(type, "pop_call_center_queue") == 0) {
        const char *queue_id = json_string_field(root, "queue_id");
        turbo_call_center_queue_side_t side =
            parse_call_center_queue_side(json_string_field(root, "side"));
        turbo_call_center_queue_entry_summary_t summary;
        char *json = NULL;

        if (!queue_id || side == 0) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center queue lookup payload");
            turbo_free_json(&root);
            return;
        }

        memset(&summary, 0, sizeof(summary));
        if (strcmp(type, "pop_call_center_queue") == 0) {
            rc = turbo_room_service_pop_call_center_queue_entry(service, queue_id, side,
                                                                &summary);
        } else {
            rc = turbo_room_service_peek_call_center_queue_entry(service, queue_id, side,
                                                                 &summary);
        }
        if (rc != 0) {
            send_error_json(res, 404, "QUEUE_ENTRY_NOT_FOUND",
                            "call-center queue entry not found");
            turbo_free_json(&root);
            return;
        }

        json = call_center_queue_entry_summary_json(&summary);
        if (!json) {
            send_error_json(res, 500, "INTERNAL_ERROR",
                            "call-center queue entry unavailable");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "queue_entry", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "match_call_center_queue") == 0) {
        const char *queue_id = json_string_field(root, "queue_id");
        turbo_call_center_queue_match_summary_t summary;
        char *json;

        if (!queue_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center queue match payload");
            turbo_free_json(&root);
            return;
        }

        memset(&summary, 0, sizeof(summary));
        rc = turbo_room_service_match_call_center_queue(service, queue_id, &summary);
        if (rc == 0) {
            json = call_center_queue_match_summary_json(&summary);
            if (!json) {
                send_error_json(res, 500, "INTERNAL_ERROR",
                                "call-center queue match unavailable");
                turbo_free_json(&root);
                return;
            }
            send_entity_ok_json(res, "call_center_match", json);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "claim_call_center_queue") == 0) {
        const char *queue_id = json_string_field(root, "queue_id");
        turbo_call_center_queue_match_summary_t summary;
        char *json;

        if (!queue_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center queue claim payload");
            turbo_free_json(&root);
            return;
        }

        memset(&summary, 0, sizeof(summary));
        rc = turbo_room_service_claim_call_center_queue_match(service, queue_id,
                                                              &summary);
        if (rc == 0) {
            json = call_center_queue_match_summary_json(&summary);
            if (!json) {
                send_error_json(res, 500, "INTERNAL_ERROR",
                                "call-center queue claim unavailable");
                turbo_free_json(&root);
                return;
            }
            send_entity_ok_json(res, "call_center_match", json);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "complete_call_center_queue_match") == 0 ||
               strcmp(type, "rollback_call_center_queue_match") == 0) {
        const char *queue_id = json_string_field(root, "queue_id");
        const char *caller_entry_id = json_string_field(root, "caller_entry_id");
        const char *callee_entry_id = json_string_field(root, "callee_entry_id");

        if (!queue_id || !caller_entry_id || !callee_entry_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center queue claim state payload");
            turbo_free_json(&root);
            return;
        }

        if (strcmp(type, "complete_call_center_queue_match") == 0) {
            rc = turbo_room_service_complete_call_center_queue_match(
                service, queue_id, caller_entry_id, callee_entry_id);
        } else {
            rc = turbo_room_service_rollback_call_center_queue_match(
                service, queue_id, caller_entry_id, callee_entry_id);
        }
        if (rc == 0) {
            send_json(res, 200, "{\"ok\":true}");
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "recover_call_center_queue_claims") == 0) {
        const char *queue_id = json_string_field(root, "queue_id");
        int lease_ms = turbo_json_get_int(root, "lease_ms", 30000);
        int recovered = 0;
        char payload[128];

        if (!queue_id || lease_ms < 0) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center queue recovery payload");
            turbo_free_json(&root);
            return;
        }

        rc = turbo_room_service_recover_stale_call_center_queue_claims(
            service, queue_id, (uint64_t)lease_ms, &recovered);
        if (rc == 0) {
            snprintf(payload, sizeof(payload), "{\"ok\":true,\"recovered\":%d}",
                     recovered);
            send_json(res, 200, payload);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "set_call_center_agent_state") == 0) {
        const char *endpoint_id = json_string_field(root, "endpoint_id");
        turbo_call_center_agent_state_t state =
            parse_call_center_agent_state(json_string_field(root, "state"));
        turbo_call_center_agent_state_summary_t summary;
        char *json;

        if (!endpoint_id || !state) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center agent state payload");
            turbo_free_json(&root);
            return;
        }

        if (turbo_room_service_set_call_center_agent_state(service, endpoint_id,
                                                           state) != 0 ||
            turbo_room_service_get_call_center_agent_state(service, endpoint_id,
                                                           &summary) != 0) {
            send_error_json(res, 500, "INTERNAL_ERROR",
                            "call-center agent state unavailable");
            turbo_free_json(&root);
            return;
        }

        json = call_center_agent_state_summary_json(&summary);
        if (!json) {
            send_error_json(res, 500, "INTERNAL_ERROR",
                            "call-center agent state unavailable");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "call_center_agent_state", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_call_center_agent_state") == 0) {
        const char *endpoint_id = json_string_field(root, "endpoint_id");
        turbo_call_center_agent_state_summary_t summary;
        char *json;

        if (!endpoint_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center agent state lookup payload");
            turbo_free_json(&root);
            return;
        }

        if (turbo_room_service_get_call_center_agent_state(service, endpoint_id,
                                                           &summary) != 0) {
            send_error_json(res, 500, "INTERNAL_ERROR",
                            "call-center agent state unavailable");
            turbo_free_json(&root);
            return;
        }

        json = call_center_agent_state_summary_json(&summary);
        if (!json) {
            send_error_json(res, 500, "INTERNAL_ERROR",
                            "call-center agent state unavailable");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "call_center_agent_state", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_call_center_room") == 0) {
        turbo_call_center_room_summary_t summary;
        char *json;

        if (!room_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center room lookup payload");
            turbo_free_json(&root);
            return;
        }

        if (turbo_room_service_get_call_center_room_summary(service, room_id,
                                                            &summary) != 0) {
            send_error_json(res, 404, "CALL_CENTER_ROOM_NOT_FOUND",
                            "call-center room state not found");
            turbo_free_json(&root);
            return;
        }

        json = call_center_room_summary_json(&summary);
        if (!json) {
            send_error_json(res, 500, "INTERNAL_ERROR",
                            "call-center room state unavailable");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "call_center_room", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_call_center_events") == 0) {
        int64_t after_sequence = (int64_t)turbo_json_get_int(root, "after_sequence", 0);
        int limit = turbo_json_get_int(root, "limit", 32);
        char *json;

        if (!room_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center event lookup payload");
            turbo_free_json(&root);
            return;
        }
        if (limit <= 0) {
            limit = 32;
        }
        if (limit > 64) {
            limit = 64;
        }

        json = call_center_events_json(g_room_service_server, room_id, after_sequence,
                                       limit);
        if (!json) {
            send_error_json(res, 500, "INTERNAL_ERROR",
                            "call-center events unavailable");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "call_center_events", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "set_call_center_consult_agent") == 0) {
        const char *consult_agent_participant_id =
            json_string_field(root, "consult_agent_participant_id");
        turbo_call_center_supervisor_mode_t supervisor_mode =
            TURBO_CALL_CENTER_SUPERVISOR_NONE;
        turbo_call_center_room_summary_t summary;
        char *json;

        if (!room_id || !consult_agent_participant_id ||
            !consult_agent_participant_id[0]) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center consult payload");
            turbo_free_json(&root);
            return;
        }

        if (turbo_room_service_set_call_center_consult_agent(
                service, room_id, consult_agent_participant_id) != 0 ||
            current_call_center_supervisor_mode(g_room_service_server, room_id,
                                               &supervisor_mode) != 0 ||
            turbo_room_service_reconcile_call_center_subscriptions(
                service, room_id, supervisor_mode) != 0 ||
            turbo_room_service_get_call_center_room_summary(service, room_id,
                                                            &summary) != 0) {
            send_error_json(res, 400, "COMMAND_FAILED",
                            "call-center consult failed");
            turbo_free_json(&root);
            return;
        }

        json = call_center_room_summary_json(&summary);
        if (!json) {
            send_error_json(res, 500, "INTERNAL_ERROR",
                            "call-center room state unavailable");
            turbo_free_json(&root);
            return;
        }
        record_call_center_event_if_room(g_room_service_server, service, room_id,
                                         "consult_started",
                                         consult_agent_participant_id,
                                         summary.agent_participant_id,
                                         summary.state == TURBO_CALL_CENTER_ROOM_ACTIVE
                                             ? "active"
                                             : call_center_room_state_name(summary.state),
                                         "consult");
        send_entity_ok_json(res, "call_center_room", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "complete_call_center_transfer") == 0) {
        turbo_call_center_agent_state_t released_agent_state =
            parse_call_center_agent_state(json_string_field(root, "released_agent_state"));
        turbo_call_center_supervisor_mode_t supervisor_mode =
            TURBO_CALL_CENTER_SUPERVISOR_NONE;
        turbo_call_center_room_summary_t summary;
        char *json;

        if (!room_id || !released_agent_state) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center transfer payload");
            turbo_free_json(&root);
            return;
        }

        if (turbo_room_service_complete_call_center_transfer(
                service, room_id, released_agent_state) != 0 ||
            current_call_center_supervisor_mode(g_room_service_server, room_id,
                                               &supervisor_mode) != 0 ||
            turbo_room_service_reconcile_call_center_subscriptions(
                service, room_id, supervisor_mode) != 0 ||
            turbo_room_service_get_call_center_room_summary(service, room_id,
                                                            &summary) != 0) {
            send_error_json(res, 400, "COMMAND_FAILED",
                            "call-center transfer failed");
            turbo_free_json(&root);
            return;
        }

        json = call_center_room_summary_json(&summary);
        if (!json) {
            send_error_json(res, 500, "INTERNAL_ERROR",
                            "call-center room state unavailable");
            turbo_free_json(&root);
            return;
        }
        record_call_center_event_if_room(g_room_service_server, service, room_id,
                                         "transfer_completed",
                                         summary.agent_participant_id,
                                         "",
                                         call_center_room_state_name(summary.state),
                                         call_center_agent_state_name(released_agent_state));
        send_entity_ok_json(res, "call_center_room", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "finalize_call_center_room") == 0) {
        turbo_call_center_room_state_t state =
            parse_call_center_room_state(json_string_field(root, "state"));
        turbo_call_center_agent_state_t agent_state =
            parse_call_center_agent_state(json_string_field(root, "agent_state"));
        const char *disposition_code = json_string_field(root, "disposition_code");
        turbo_call_center_room_summary_t summary;
        char *json;

        if (!room_id || !state || !agent_state) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center room finalize payload");
            turbo_free_json(&root);
            return;
        }

        if (turbo_room_service_finalize_call_center_room(
                service, room_id, state, disposition_code, agent_state) != 0 ||
            turbo_room_service_get_call_center_room_summary(service, room_id,
                                                            &summary) != 0) {
            send_error_json(res, 400, "COMMAND_FAILED",
                            "call-center room finalize failed");
            turbo_free_json(&root);
            return;
        }

        json = call_center_room_summary_json(&summary);
        if (!json) {
            send_error_json(res, 500, "INTERNAL_ERROR",
                            "call-center room state unavailable");
            turbo_free_json(&root);
            return;
        }
        record_call_center_event_if_room(g_room_service_server, service, room_id,
                                         "room_finalized",
                                         summary.agent_participant_id,
                                         summary.customer_participant_id,
                                         call_center_room_state_name(state),
                                         disposition_code ? disposition_code : "");
        send_entity_ok_json(res, "call_center_room", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_call_center_queue_depth") == 0) {
        const char *queue_id = json_string_field(root, "queue_id");
        const char *side_text = json_string_field(root, "side");
        turbo_call_center_queue_side_t side = parse_call_center_queue_side(side_text);
        int caller_depth = 0;
        int callee_depth = 0;
        char payload[160];

        if (!queue_id || (side_text && side == 0)) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center queue depth payload");
            turbo_free_json(&root);
            return;
        }

        rc = 0;
        if (side == 0 || side == TURBO_CALL_CENTER_QUEUE_CALLER) {
            if (turbo_room_service_get_call_center_queue_depth(
                    service, queue_id, TURBO_CALL_CENTER_QUEUE_CALLER,
                    &caller_depth) != 0) {
                rc = -1;
            }
        }
        if (side == 0 || side == TURBO_CALL_CENTER_QUEUE_CALLEE) {
            if (turbo_room_service_get_call_center_queue_depth(
                    service, queue_id, TURBO_CALL_CENTER_QUEUE_CALLEE,
                    &callee_depth) != 0) {
                rc = -1;
            }
        }
        if (rc != -1) {
            snprintf(payload, sizeof(payload),
                     "{\"ok\":true,\"caller_depth\":%d,\"callee_depth\":%d}",
                     caller_depth, callee_depth);
            send_json(res, 200, payload);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "route_call_center_queue") == 0) {
        const char *queue_id = json_string_field(root, "queue_id");
        const char *route_room_id = json_string_field(root, "route_room_id");
        const char *created_by = json_string_field(root, "created_by");
        const char *recording_id = json_string_field(root, "recording_id");
        const char *recording_mode = json_string_field(root, "recording_mode");
#ifdef ROOM_SERVICE_ENABLE_TEST_HOOKS
        const char *test_failure_stage_name =
            json_string_field(root, "test_failure_stage");
        room_service_route_test_failure_stage_t test_failure_stage =
            ROOM_SERVICE_ROUTE_TEST_FAILURE_NONE;
#endif
        turbo_call_center_queue_match_summary_t match;
        turbo_room_summary_t existing_room;
        turbo_room_config_t room_config;
        turbo_room_participant_config_t caller_participant;
        turbo_room_participant_config_t callee_participant;
        char *json;
        int route_room_created = 0;

        if (!queue_id || !route_room_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center route payload");
            turbo_free_json(&root);
            return;
        }
#ifdef ROOM_SERVICE_ENABLE_TEST_HOOKS
        if (parse_route_test_failure_stage(test_failure_stage_name,
                                           &test_failure_stage) != 0) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "invalid call-center route test failure stage");
            turbo_free_json(&root);
            return;
        }
#endif

        memset(&existing_room, 0, sizeof(existing_room));
        if (turbo_room_service_get_room_summary(service, route_room_id,
                                                &existing_room) == 0) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "call-center route room already exists");
            turbo_free_json(&root);
            return;
        }

        memset(&match, 0, sizeof(match));
        rc = turbo_room_service_claim_call_center_queue_match(service, queue_id,
                                                              &match);
        if (rc != 0) {
            send_error_json(res, 500, "INTERNAL_ERROR",
                            "call-center route unavailable");
            turbo_free_json(&root);
            return;
        }
        if (!match.matched) {
            char *match_json;

            match_json = call_center_queue_match_summary_json(&match);
            if (!match_json) {
                send_error_json(res, 500, "INTERNAL_ERROR",
                                "call-center route unavailable");
                turbo_free_json(&root);
                return;
            }
            send_entity_ok_json(res, "call_center_route", match_json);
            turbo_free_json(&root);
            return;
        }
        if (strcmp(match.caller.endpoint_id, match.callee.endpoint_id) == 0) {
            (void)turbo_room_service_rollback_call_center_queue_match(
                service, queue_id, match.caller.entry_id, match.callee.entry_id);
            send_error_json(res, 400, "INVALID_REQUEST",
                            "call-center route endpoints must be distinct");
            turbo_free_json(&root);
            return;
        }

        memset(&room_config, 0, sizeof(room_config));
        room_config.room_id = route_room_id;
        room_config.room_type = TURBO_ROOM_TYPE_CALL;
        room_config.created_by = created_by ? created_by : "call_center_router";
        rc = turbo_room_service_create_room(service, &room_config);
        if (rc == 0) {
            route_room_created = 1;
        }
#ifdef ROOM_SERVICE_ENABLE_TEST_HOOKS
        if (rc == 0 &&
            test_failure_stage == ROOM_SERVICE_ROUTE_TEST_FAILURE_AFTER_ROOM_CREATE) {
            rc = -1;
        }
#endif

        memset(&caller_participant, 0, sizeof(caller_participant));
        caller_participant.participant_id = match.caller.endpoint_id;
        caller_participant.user_id = match.caller.endpoint_id;
        caller_participant.display_name = match.caller.entry_id;
        caller_participant.role = TURBO_PARTICIPANT_ROLE_CUSTOMER;
        if (rc == 0) {
            rc = turbo_room_service_add_participant(service, route_room_id,
                                                    &caller_participant);
        }
#ifdef ROOM_SERVICE_ENABLE_TEST_HOOKS
        if (rc == 0 &&
            test_failure_stage == ROOM_SERVICE_ROUTE_TEST_FAILURE_AFTER_CALLER_JOIN) {
            rc = -1;
        }
#endif

        memset(&callee_participant, 0, sizeof(callee_participant));
        callee_participant.participant_id = match.callee.endpoint_id;
        callee_participant.user_id = match.callee.endpoint_id;
        callee_participant.display_name = match.callee.entry_id;
        callee_participant.role = TURBO_PARTICIPANT_ROLE_AGENT;
        if (rc == 0) {
            rc = turbo_room_service_add_participant(service, route_room_id,
                                                    &callee_participant);
        }
        if (rc == 0) {
            rc = turbo_room_service_start_call_center_room(
                service, route_room_id, caller_participant.participant_id,
                callee_participant.participant_id);
        }
#ifdef ROOM_SERVICE_ENABLE_TEST_HOOKS
        if (rc == 0 &&
            test_failure_stage == ROOM_SERVICE_ROUTE_TEST_FAILURE_AFTER_CALLEE_JOIN) {
            rc = -1;
        }
#endif
        if (rc == 0 && recording_id && recording_id[0]) {
            rc = turbo_room_service_start_recording(
                service, route_room_id, recording_id,
                recording_mode && recording_mode[0] ? recording_mode : "compliance");
        }

        if (rc != 0) {
            (void)turbo_room_service_rollback_call_center_queue_match(
                service, queue_id, match.caller.entry_id, match.callee.entry_id);
            if (route_room_created) {
                if (turbo_room_service_discard_unassigned_room(service,
                                                               route_room_id) != 0) {
                    (void)turbo_room_service_close_room(service, route_room_id);
                }
            }
        } else {
            json = call_center_route_result_json(route_room_id, &match.caller,
                                                 &match.callee);
            if (!json) {
                (void)turbo_room_service_rollback_call_center_queue_match(
                    service, queue_id, match.caller.entry_id, match.callee.entry_id);
                if (turbo_room_service_discard_unassigned_room(service,
                                                               route_room_id) != 0) {
                    (void)turbo_room_service_close_room(service, route_room_id);
                }
                send_error_json(res, 500, "INTERNAL_ERROR",
                                "call-center route unavailable");
                turbo_free_json(&root);
                return;
            }
            rc = turbo_room_service_complete_call_center_queue_match(
                service, queue_id, match.caller.entry_id, match.callee.entry_id);
            if (rc != 0) {
                free(json);
                (void)turbo_room_service_rollback_call_center_queue_match(
                    service, queue_id, match.caller.entry_id, match.callee.entry_id);
                if (turbo_room_service_discard_unassigned_room(service,
                                                               route_room_id) != 0) {
                    (void)turbo_room_service_close_room(service, route_room_id);
                }
            } else {
                record_call_center_event_if_room(g_room_service_server, service,
                                                 route_room_id, "room_routed",
                                                 match.caller.endpoint_id,
                                                 match.callee.endpoint_id, "active",
                                                 queue_id);
                send_entity_ok_json(res, "call_center_route", json);
                turbo_free_json(&root);
                return;
            }
        }
    } else if (strcmp(type, "create_room") == 0) {
        turbo_room_config_t config;
        memset(&config, 0, sizeof(config));
        config.room_id = room_id;
        config.room_type = parse_room_type(json_string_field(root, "room_type"));
        config.created_by = json_string_field(root, "created_by");
        if (!config.room_id || config.room_type == 0) {
            send_error_json(res, 400, "INVALID_REQUEST", "invalid create_room payload");
            turbo_free_json(&root);
            return;
        }
        rc = turbo_room_service_create_room(service, &config);
    } else if (strcmp(type, "assign_sfu_node") == 0) {
        room_service_sfu_replay_stats_t replay_stats;
        const char *node_id = json_string_field(root, "node_id");
        char selected_node_id[TURBO_NODE_ID_MAX];

        memset(&replay_stats, 0, sizeof(replay_stats));
        if (!room_id) {
            send_error_json(res, 400, "INVALID_REQUEST", "missing room_id");
            turbo_free_json(&root);
            return;
        }
        if (!node_id) {
            if (room_service_app_server_choose_sfu_node(
                    g_room_service_server, selected_node_id,
                    sizeof(selected_node_id)) != 0) {
                send_error_json(res, 409, "NO_SFU_NODE_AVAILABLE",
                                "no sfu node is registered or configured");
                turbo_free_json(&root);
                return;
            }
            node_id = selected_node_id;
        } else if (!room_service_app_server_has_sfu_node(g_room_service_server, node_id)) {
            const room_service_app_config_t *config =
                room_service_app_server_get_config(g_room_service_server);
            room_service_app_stats_t app_stats;

            memset(&app_stats, 0, sizeof(app_stats));
            (void)room_service_app_server_get_stats(g_room_service_server, &app_stats);
            if (app_stats.sfu_node_count > 0 ||
                !config || !config->sfu_control_url || config->sfu_control_url[0] == '\0') {
                send_error_json(res, 404, "SFU_NODE_NOT_FOUND",
                                "sfu node is not registered");
                turbo_free_json(&root);
                return;
            }
        }
        rc = turbo_room_service_assign_sfu_node(service, room_id, node_id);
        if (rc == 0) {
            int sync_rc = room_service_app_server_sync_replay_room_state(
                g_room_service_server, room_id, &replay_stats);
            if (sync_rc != 0) {
                warning_code = "SFU_SYNC_FAILED";
                warning_message =
                    "room state committed locally; existing runtime was not fully replayed to sfu node";
            } else if (replay_stats.skipped_items > 0) {
                warning_code = "SFU_SYNC_SKIPPED";
                warning_message =
                    "room state committed locally; some tracks or subscriptions were skipped during sfu replay because SSRC metadata was incomplete";
            }
            room_service_app_server_record_room_sync(g_room_service_server, room_id, "assign",
                                                     &replay_stats, warning_code,
                                                     warning_message);
        }
        if (rc == 0) {
            send_room_sync_result_json(res, service, room_id, "sfu_replay_stats",
                                       &replay_stats, warning_code, warning_message);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "resync_room") == 0) {
        turbo_room_summary_t room_summary;
        room_service_sfu_replay_stats_t replay_stats;
        memset(&replay_stats, 0, sizeof(replay_stats));

        if (!room_id ||
            turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0) {
            send_error_json(res, 404, "ROOM_NOT_FOUND", "room not found");
            turbo_free_json(&root);
            return;
        }
        if (room_summary.assigned_sfu_node[0] == '\0') {
            send_error_json(res, 400, "ROOM_UNASSIGNED",
                            "room has no assigned sfu node");
            turbo_free_json(&root);
            return;
        }

        rc = room_service_app_server_sync_resync_room(g_room_service_server, room_id,
                                                      &replay_stats);
        if (rc == 0 && replay_stats.skipped_items > 0) {
            warning_code = "SFU_SYNC_SKIPPED";
            warning_message =
                "room runtime resynced, but some tracks or subscriptions were skipped because SSRC metadata was incomplete";
        }
        if (rc == 0) {
            room_service_app_server_record_room_sync(g_room_service_server, room_id, "resync",
                                                     &replay_stats, warning_code,
                                                     warning_message);
        }
        if (rc == 0) {
            send_room_sync_result_json(res, service, room_id, "sfu_replay_stats",
                                       &replay_stats, warning_code, warning_message);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "add_participant") == 0) {
        json_value_t *participant = turbo_json_object_get(root, "participant");
        turbo_room_participant_config_t config;
        memset(&config, 0, sizeof(config));
        if (!participant || turbo_json_type(participant) != TURBO_JSON_OBJECT) {
            send_error_json(res, 400, "INVALID_REQUEST", "missing participant object");
            turbo_free_json(&root);
            return;
        }
        config.participant_id = json_string_field(participant, "participant_id");
        config.user_id = json_string_field(participant, "user_id");
        config.display_name = json_string_field(participant, "display_name");
        config.role = parse_role(json_string_field(participant, "role"));
        if (!room_id || !config.participant_id || config.role == 0) {
            send_error_json(res, 400, "INVALID_REQUEST", "invalid participant payload");
            turbo_free_json(&root);
            return;
        }
        rc = turbo_room_service_add_participant(service, room_id, &config);
        if (rc == 0) {
            room_service_conference_policy_apply_result_t call_center_apply_result = {0};

            if (room_service_app_server_sync_add_session(g_room_service_server, room_id,
                                                         config.participant_id) != 0) {
                warning_code = "SFU_SYNC_FAILED";
                warning_message =
                    "participant state committed locally; add_session was not forwarded to sfu node";
            } else {
                sync_derived_receiver_bandwidth_if_needed(service, room_id,
                                                          config.participant_id,
                                                          &warning_code,
                                                          &warning_message);
            }

            if (reconcile_call_center_policy_for_room(g_room_service_server, service, room_id,
                                                      &call_center_apply_result) != 0) {
                if (!warning_code) {
                    warning_code = "POLICY_RECONCILE_FAILED";
                    warning_message =
                        "participant state committed locally; call-center subscriptions were not reconciled";
                }
            } else if (!warning_code && call_center_apply_result.had_warning) {
                warning_code = call_center_apply_result.warning_code;
                warning_message = call_center_apply_result.warning_message;
            }

            if (is_call_center_observer_role(config.role)) {
                record_call_center_event_if_room(g_room_service_server, service, room_id,
                                                 "observer_joined",
                                                 config.participant_id, "",
                                                 participant_role_name(config.role),
                                                 "join");
            }
        }
    } else if (strcmp(type, "remove_participant") == 0) {
        const char *participant_id = json_string_field(root, "participant_id");
        rc = turbo_room_service_remove_participant(service, room_id, participant_id);
        if (rc == 0 &&
            room_service_app_server_sync_remove_session(g_room_service_server, room_id,
                                                        participant_id) != 0) {
            warning_code = "SFU_SYNC_FAILED";
            warning_message =
                "participant state committed locally; remove_session was not forwarded to sfu node";
        }
        if (rc == 0) {
            room_service_conference_policy_apply_result_t call_center_apply_result = {0};

            if (reconcile_call_center_policy_for_room(g_room_service_server, service, room_id,
                                                      &call_center_apply_result) != 0) {
                if (!warning_code) {
                    warning_code = "POLICY_RECONCILE_FAILED";
                    warning_message =
                        "participant state committed locally; call-center subscriptions were not reconciled";
                }
            } else if (!warning_code && call_center_apply_result.had_warning) {
                warning_code = call_center_apply_result.warning_code;
                warning_message = call_center_apply_result.warning_message;
            }
        }
    } else if (strcmp(type, "set_participant_session_state") == 0) {
        rc = turbo_room_service_set_participant_session_state(
            service, room_id,
            json_string_field(root, "participant_id"),
            parse_session_state(json_string_field(root, "session_state")));
    } else if (strcmp(type, "set_participant_bandwidth") == 0) {
        const char *participant_id = json_string_field(root, "participant_id");
        int bandwidth_bps = turbo_json_get_int(root, "bandwidth_bps", -1);
        turbo_room_summary_t room_summary;

        rc = turbo_room_service_set_participant_bandwidth(service, room_id, participant_id,
                                                          bandwidth_bps);
        if (rc == 0 &&
            turbo_room_service_get_room_summary(service, room_id, &room_summary) == 0 &&
            room_summary.assigned_sfu_node[0] != '\0' &&
            room_service_app_server_sync_set_receiver_bandwidth(
                g_room_service_server, room_id, participant_id, bandwidth_bps) != 0) {
            warning_code = "SFU_SYNC_FAILED";
            warning_message =
                "participant bandwidth committed locally; set_receiver_bandwidth was not forwarded to sfu node";
        }
    } else if (strcmp(type, "publish_track") == 0) {
        json_value_t *track = turbo_json_object_get(root, "track");
        turbo_room_track_config_t config;
        room_service_conference_policy_apply_result_t call_center_apply_result = {0};
        turbo_room_summary_t room_summary;
        turbo_room_track_summary_t track_summary;
        uint32_t layer_ssrcs[3] = {0};
        memset(&config, 0, sizeof(config));
        if (!track || turbo_json_type(track) != TURBO_JSON_OBJECT) {
            send_error_json(res, 400, "INVALID_REQUEST", "missing track object");
            turbo_free_json(&root);
            return;
        }
        config.track_id = json_string_field(track, "track_id");
        config.owner_participant_id = json_string_field(track, "owner_participant_id");
        config.kind = parse_track_kind(json_string_field(track, "kind"));
        config.source = parse_track_source(json_string_field(track, "source"));
        config.codec_name = json_string_field(track, "codec_name");
        config.simulcast_enabled = json_bool_field(track, "simulcast_enabled", 0);
        (void)json_uint32_field(track, "main_ssrc", &config.main_ssrc);
        if (json_uint32_array_field(track, "layer_ssrcs", layer_ssrcs, 3,
                                    &config.layer_count) != 0) {
            send_error_json(res, 400, "INVALID_REQUEST", "invalid layer_ssrcs payload");
            turbo_free_json(&root);
            return;
        }
        config.layer_ssrcs = layer_ssrcs;
        if (!room_id || !config.track_id || !config.owner_participant_id ||
            config.kind == 0 || config.source == 0 || !config.codec_name) {
            send_error_json(res, 400, "INVALID_REQUEST", "invalid track payload");
            turbo_free_json(&root);
            return;
        }
        rc = turbo_room_service_publish_track(service, room_id, &config);
        if (rc == 0 &&
            turbo_room_service_get_room_summary(service, room_id, &room_summary) == 0 &&
            room_summary.assigned_sfu_node[0] != '\0') {
            if (turbo_room_service_get_track_summary(service, room_id, config.track_id,
                                                     &track_summary) != 0 ||
                track_summary.main_ssrc == 0) {
                warning_code = "SFU_SYNC_SKIPPED";
                warning_message =
                    "track state committed locally; publish_track lacks SSRC metadata for sfu sync";
            } else if (room_service_app_server_sync_register_track(g_room_service_server, room_id,
                                                                   &track_summary) != 0) {
                warning_code = "SFU_SYNC_FAILED";
                warning_message =
                    "track state committed locally; register_published_track was not forwarded to sfu node";
            } else if (room_summary.recording_state == TURBO_ROOM_RECORDING_ACTIVE &&
                       room_summary.recording_id[0] != '\0' &&
                       room_summary.recording_mode[0] != '\0') {
                int recording_started = 0;

                if (room_service_app_server_sync_ensure_recording(
                        g_room_service_server, room_id, room_summary.recording_id,
                        room_summary.recording_mode, &recording_started) != 0) {
                    warning_code = "SFU_SYNC_FAILED";
                    warning_message =
                        "track state committed locally; active recording intent was not reconciled to sfu node";
                }
            }
        }
        if (rc == 0 &&
            reconcile_call_center_policy_for_room(g_room_service_server, service, room_id,
                                                  &call_center_apply_result) != 0) {
            if (!warning_code) {
                warning_code = "POLICY_RECONCILE_FAILED";
                warning_message =
                    "track state committed locally; call-center subscriptions were not reconciled";
            }
        } else if (rc == 0 && !warning_code && call_center_apply_result.had_warning) {
            warning_code = call_center_apply_result.warning_code;
            warning_message = call_center_apply_result.warning_message;
        }
    } else if (strcmp(type, "unpublish_track") == 0) {
        const char *track_id = json_string_field(root, "track_id");
        turbo_room_summary_t room_summary;
        turbo_room_track_summary_t track_summary = {0};
        int have_track_summary = 0;

        if (room_id && track_id &&
            turbo_room_service_get_track_summary(service, room_id, track_id, &track_summary) == 0) {
            have_track_summary = 1;
        }

        rc = turbo_room_service_unpublish_track(service, room_id, track_id);
        if (rc == 0 &&
            turbo_room_service_get_room_summary(service, room_id, &room_summary) == 0 &&
            room_summary.assigned_sfu_node[0] != '\0') {
            if (!have_track_summary || track_summary.main_ssrc == 0) {
                warning_code = "SFU_SYNC_SKIPPED";
                warning_message =
                    "track state committed locally; unpublish_track lacks SSRC metadata for sfu sync";
            } else if (room_service_app_server_sync_unregister_track(g_room_service_server, room_id,
                                                                     track_id) != 0) {
                warning_code = "SFU_SYNC_FAILED";
                warning_message =
                    "track state committed locally; unregister_published_track was not forwarded to sfu node";
            }
        }
    } else if (strcmp(type, "set_track_muted") == 0) {
        rc = turbo_room_service_set_track_muted(service, room_id,
                                                json_string_field(root, "track_id"),
                                                json_bool_field(root, "muted", 0));
    } else if (strcmp(type, "set_subscription") == 0) {
        json_value_t *subscription = turbo_json_object_get(root, "subscription");
        turbo_room_subscription_config_t config;
        memset(&config, 0, sizeof(config));
        if (!subscription || turbo_json_type(subscription) != TURBO_JSON_OBJECT) {
            send_error_json(res, 400, "INVALID_REQUEST", "missing subscription object");
            turbo_free_json(&root);
            return;
        }
        config.subscriber_participant_id =
            json_string_field(subscription, "subscriber_participant_id");
        config.track_id = json_string_field(subscription, "track_id");
        config.enabled = json_bool_field(subscription, "enabled", 1);
        config.priority = turbo_json_get_int(subscription, "priority", 0);
        config.preferred_layer =
            parse_video_layer(json_string_field(subscription, "preferred_layer"));
        config.target_layer =
            parse_video_layer(json_string_field(subscription, "target_layer"));
        config.muted = json_bool_field(subscription, "muted", 0);
        config.policy_source = json_string_field(subscription, "policy_source");
        if (!room_id || !config.subscriber_participant_id || !config.track_id) {
            send_error_json(res, 400, "INVALID_REQUEST", "invalid subscription payload");
            turbo_free_json(&root);
            return;
        }
        rc = turbo_room_service_set_subscription(service, room_id, &config);
        if (rc == 0) {
            sync_track_subscription_if_needed(service, room_id,
                                              config.subscriber_participant_id,
                                              config.track_id, &warning_code,
                                              &warning_message);
            sync_derived_receiver_bandwidth_if_needed(service, room_id,
                                                      config.subscriber_participant_id,
                                                      &warning_code, &warning_message);
        }
    } else if (strcmp(type, "remove_subscription") == 0) {
        const char *subscriber_participant_id = json_string_field(root, "subscriber_participant_id");
        const char *track_id = json_string_field(root, "track_id");
        rc = turbo_room_service_remove_subscription(
            service, room_id, subscriber_participant_id, track_id);
        if (rc == 0 && subscriber_participant_id) {
            sync_track_subscription_if_needed(service, room_id, subscriber_participant_id,
                                              track_id, &warning_code, &warning_message);
            sync_derived_receiver_bandwidth_if_needed(service, room_id,
                                                      subscriber_participant_id,
                                                      &warning_code, &warning_message);
        }
    } else if (strcmp(type, "set_layout_mode") == 0) {
        const char *layout_mode = json_string_field(root, "layout_mode");
        room_service_conference_policy_apply_result_t apply_result;
        memset(&apply_result, 0, sizeof(apply_result));

        if (!room_id || !layout_mode) {
            send_error_json(res, 400, "INVALID_REQUEST", "invalid conference policy payload");
            turbo_free_json(&root);
            return;
        }

        rc = room_service_app_server_set_conference_layout_mode(
            g_room_service_server, room_id, layout_mode);
        if (rc == 0) {
            rc = room_service_app_server_apply_conference_policy(
                g_room_service_server, room_id, &apply_result);
        }
        if (rc == 0) {
            send_conference_policy_result_json(res, g_room_service_server, service, room_id,
                                               &apply_result);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "set_active_speaker") == 0) {
        room_service_conference_policy_apply_result_t apply_result;
        memset(&apply_result, 0, sizeof(apply_result));

        rc = room_service_app_server_set_conference_active_speaker(
            g_room_service_server, room_id, json_string_field(root, "participant_id"));
        if (rc == 0) {
            rc = room_service_app_server_apply_conference_policy(
                g_room_service_server, room_id, &apply_result);
        }
        if (rc == 0) {
            send_conference_policy_result_json(res, g_room_service_server, service, room_id,
                                               &apply_result);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "set_pin") == 0 ||
               strcmp(type, "pin_participant") == 0 ||
               strcmp(type, "clear_pin") == 0) {
        room_service_conference_policy_apply_result_t apply_result;
        const char *participant_id = NULL;
        memset(&apply_result, 0, sizeof(apply_result));

        if (strcmp(type, "clear_pin") != 0) {
            participant_id = json_string_field(root, "participant_id");
        }
        rc = room_service_app_server_set_conference_pin(
            g_room_service_server, room_id, participant_id);
        if (rc == 0) {
            rc = room_service_app_server_apply_conference_policy(
                g_room_service_server, room_id, &apply_result);
        }
        if (rc == 0) {
            send_conference_policy_result_json(res, g_room_service_server, service, room_id,
                                               &apply_result);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "apply_conference_policy") == 0) {
        room_service_conference_policy_apply_result_t apply_result;
        memset(&apply_result, 0, sizeof(apply_result));

        rc = room_service_app_server_apply_conference_policy(g_room_service_server, room_id,
                                                             &apply_result);
        if (rc == 0) {
            send_conference_policy_result_json(res, g_room_service_server, service, room_id,
                                               &apply_result);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "apply_call_center_policy") == 0) {
        const char *supervisor_mode_text = json_string_field(root, "supervisor_mode");
        turbo_call_center_supervisor_mode_t supervisor_mode =
            parse_call_center_supervisor_mode(supervisor_mode_text);
        room_service_conference_policy_apply_result_t apply_result;
        memset(&apply_result, 0, sizeof(apply_result));

        if (!room_id || supervisor_mode < TURBO_CALL_CENTER_SUPERVISOR_NONE) {
            send_error_json(res, 400, "INVALID_REQUEST", "invalid call-center policy payload");
            turbo_free_json(&root);
            return;
        }

        rc = room_service_app_server_apply_call_center_policy(
            g_room_service_server, room_id, supervisor_mode, &apply_result);
        if (rc == 0) {
            record_call_center_event_if_room(g_room_service_server, service, room_id,
                                             "policy_applied", "", "",
                                             supervisor_mode_text ? supervisor_mode_text
                                                                  : "none",
                                             "supervisor_mode");
            send_call_center_policy_result_json(
                res, service, room_id,
                supervisor_mode_text ? supervisor_mode_text : "none",
                &apply_result);
            turbo_free_json(&root);
            return;
        }
    } else if (strcmp(type, "get_participant") == 0) {
        char *json = participant_summary_json(service, room_id,
                                              json_string_field(root, "participant_id"));
        if (!json) {
            send_error_json(res, 404, "PARTICIPANT_NOT_FOUND", "participant not found");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "participant", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_participant_bandwidth_diagnostic") == 0) {
        char *json = participant_bandwidth_diagnostic_json(
            g_room_service_server, service, room_id,
            json_string_field(root, "participant_id"), NULL);
        if (!json) {
            send_error_json(res, 404, "PARTICIPANT_NOT_FOUND",
                            "participant bandwidth diagnostic not found");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "participant_bandwidth_diagnostic", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_room_diagnostic") == 0) {
        char *json = room_diagnostic_json(g_room_service_server, service, room_id);
        if (!json) {
            send_error_json(res, 404, "ROOM_NOT_FOUND", "room diagnostic not found");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "room_diagnostic", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_room_state") == 0) {
        char *json = room_state_json(g_room_service_server, service, room_id);
        if (!json) {
            send_error_json(res, 404, "ROOM_NOT_FOUND", "room state not found");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "room_state", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_room_sync_diagnostic") == 0) {
        char *json = room_sync_view_json(g_room_service_server, service, room_id);
        if (!json) {
            send_error_json(res, 404, "ROOM_NOT_FOUND", "room sync diagnostic not found");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "room_sync_diagnostic", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_track") == 0) {
        char *json = track_summary_json(service, room_id,
                                        json_string_field(root, "track_id"));
        if (!json) {
            send_error_json(res, 404, "TRACK_NOT_FOUND", "track not found");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "track", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_subscription") == 0) {
        char *json = subscription_summary_json(
            service, room_id,
            json_string_field(root, "subscriber_participant_id"),
            json_string_field(root, "track_id"));
        if (!json) {
            send_error_json(res, 404, "SUBSCRIPTION_NOT_FOUND", "subscription not found");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "subscription", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_subscription_diagnostic") == 0) {
        char *json = subscription_diagnostic_json(
            g_room_service_server, service, room_id,
            json_string_field(root, "subscriber_participant_id"),
            json_string_field(root, "track_id"), NULL);
        if (!json) {
            send_error_json(res, 404, "SUBSCRIPTION_NOT_FOUND",
                            "subscription diagnostic not found");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "subscription_diagnostic", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_conference_policy") == 0) {
        char *json = conference_policy_json(g_room_service_server, room_id);
        if (!json) {
            send_error_json(res, 404, "ROOM_NOT_FOUND", "conference policy not found");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "conference_policy", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "start_recording") == 0) {
        turbo_room_summary_t room_summary;
        rc = turbo_room_service_start_recording(service, room_id,
                                                json_string_field(root, "recording_id"),
                                                json_string_field(root, "mode"));
        if (rc == 0 &&
            turbo_room_service_get_room_summary(service, room_id, &room_summary) == 0 &&
            room_summary.assigned_sfu_node[0] != '\0' &&
            room_service_app_server_sync_start_recording(
                g_room_service_server, room_id, room_summary.recording_id,
                room_summary.recording_mode) != 0) {
            warning_code = "SFU_SYNC_FAILED";
            warning_message =
                "recording state committed locally; start_recording was not forwarded to sfu node";
        }
    } else if (strcmp(type, "stop_recording") == 0) {
        turbo_room_summary_t room_summary;
        rc = turbo_room_service_stop_recording(service, room_id);
        if (rc == 0 &&
            turbo_room_service_get_room_summary(service, room_id, &room_summary) == 0 &&
            room_summary.assigned_sfu_node[0] != '\0' &&
            room_service_app_server_sync_stop_recording(g_room_service_server, room_id) != 0) {
            warning_code = "SFU_SYNC_FAILED";
            warning_message =
                "recording state committed locally; stop_recording was not forwarded to sfu node";
        }
    } else if (strcmp(type, "close_room") == 0) {
        rc = turbo_room_service_close_room(service, room_id);
        if (rc == 0 &&
            room_service_app_server_sync_force_close_room(g_room_service_server, room_id) != 0) {
            warning_code = "SFU_SYNC_FAILED";
            warning_message =
                "room state committed locally; force_close_room was not forwarded to sfu node";
        }
    } else {
        send_error_json(res, 400, "UNKNOWN_COMMAND", "unsupported command type");
        turbo_free_json(&root);
        return;
    }

    if (rc != 0) {
        send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
        turbo_free_json(&root);
        return;
    }

    if (warning_code) {
        send_room_warning_json(res, service, room_id, warning_code, warning_message);
    } else {
        send_room_ok_json(res, service, room_id);
    }
    turbo_free_json(&root);
}

static void room_service_http_mark_running(void *arg1, void *arg2) {
    room_service_http_api_t *api = (room_service_http_api_t *)arg1;
    (void)arg2;

    turbo_mutex_lock(&api->lifecycle_mutex);
    if (api->state == ROOM_SERVICE_HTTP_STARTING) {
        api->state = ROOM_SERVICE_HTTP_RUNNING;
        turbo_cond_broadcast(&api->lifecycle_cond);
    }
    turbo_mutex_unlock(&api->lifecycle_mutex);
}

static void room_service_http_thread(void *arg) {
    room_service_http_api_t *api = (room_service_http_api_t *)arg;
    const room_service_app_config_t *config =
        room_service_app_server_get_config(api->server);
    coro_context_t *ctx = NULL;
    coro_socket_t *listener = NULL;
    int async_initialized = 0;

    ctx = coro_context_create(NULL);
    if (!ctx) {
        turbo_mutex_lock(&api->lifecycle_mutex);
        api->state = ROOM_SERVICE_HTTP_FAILED;
        turbo_cond_broadcast(&api->lifecycle_cond);
        turbo_mutex_unlock(&api->lifecycle_mutex);
        return;
    }

    if (iris_async_init(1) == 0) {
        async_initialized = 1;
    }

    if (config && config->use_tls) {
        turbo_tls_server_config_t tls_config;

        memset(&tls_config, 0, sizeof(tls_config));
        tls_config.size = sizeof(tls_config);
        tls_config.cert_file = config->tls_cert_file;
        tls_config.key_file = config->tls_key_file;
        tls_config.client_auth = TURBO_TLS_CLIENT_AUTH_NONE;
        listener = iris_server_start_tls_on(
            api->app, ctx, api->host, (unsigned short)api->port,
            &tls_config);
    } else {
        listener = iris_server_start_on(
            api->app, ctx, api->host, (unsigned short)api->port);
    }
    if (!listener) {
        coro_context_destroy(ctx);
        if (async_initialized) {
            iris_async_shutdown();
        }
        turbo_mutex_lock(&api->lifecycle_mutex);
        api->state = ROOM_SERVICE_HTTP_FAILED;
        turbo_cond_broadcast(&api->lifecycle_cond);
        turbo_mutex_unlock(&api->lifecycle_mutex);
        return;
    }

    coro_context_set_persistent(ctx, 1);
    turbo_mutex_lock(&api->lifecycle_mutex);
    api->ctx = ctx;
    api->listener = listener;
    turbo_mutex_unlock(&api->lifecycle_mutex);

    if (coro_post(ctx, room_service_http_mark_running, api, NULL) != 0) {
        turbo_mutex_lock(&api->lifecycle_mutex);
        api->listener = NULL;
        api->ctx = NULL;
        api->state = ROOM_SERVICE_HTTP_FAILED;
        turbo_cond_broadcast(&api->lifecycle_cond);
        turbo_mutex_unlock(&api->lifecycle_mutex);
        coro_context_set_persistent(ctx, 0);
        coro_socket_destroy(listener);
        coro_context_destroy(ctx);
        if (async_initialized) {
            iris_async_shutdown();
        }
        return;
    }

    coro_context_run(ctx, TURBO_RUN_DEFAULT);

    turbo_mutex_lock(&api->lifecycle_mutex);
    api->listener = NULL;
    api->ctx = NULL;
    api->state = ROOM_SERVICE_HTTP_STOPPING;
    turbo_mutex_unlock(&api->lifecycle_mutex);

    coro_context_set_persistent(ctx, 0);
    coro_socket_destroy(listener);
    coro_context_destroy(ctx);
    if (async_initialized) {
        iris_async_shutdown();
    }

    turbo_mutex_lock(&api->lifecycle_mutex);
    api->state = ROOM_SERVICE_HTTP_STOPPED;
    turbo_cond_broadcast(&api->lifecycle_cond);
    turbo_mutex_unlock(&api->lifecycle_mutex);
}

room_service_http_api_t *room_service_http_api_create(room_service_app_server_t *server) {
    room_service_http_api_t *api;
    cors_t cors_opts;

    if (!server) {
        return NULL;
    }

    api = (room_service_http_api_t *)calloc(1, sizeof(*api));
    if (!api) {
        return NULL;
    }

    turbo_mutex_init(&api->lifecycle_mutex);
    turbo_cond_init(&api->lifecycle_cond);
    api->app = iris_app_create();
    if (!api->app) {
        turbo_cond_destroy(&api->lifecycle_cond);
        turbo_mutex_destroy(&api->lifecycle_mutex);
        free(api);
        return NULL;
    }
    api->server = server;

    g_room_service_server = server;

    memset(&cors_opts, 0, sizeof(cors_opts));
    cors_opts.origin = "*";
    cors_opts.methods = "GET, POST, OPTIONS";
    cors_opts.headers = "Content-Type, Authorization";
    cors_opts.enabled = 1;
    iris_app_cors(api->app, &cors_opts);

    iris_app_get(api->app, "/health", handle_health);
    iris_app_get(api->app, "/metrics", handle_metrics);
    iris_app_post(api->app, "/api/v1/join", handle_join);
    iris_app_post(api->app, "/api/v1/publish", handle_publish);
    iris_app_post(api->app, "/api/v1/subscribe", handle_subscribe);
    iris_app_get(api->app, "/api/v1/rooms/:id", handle_get_room);
    iris_app_get(api->app, "/api/v1/room_state", handle_get_room_state);
    iris_app_get(api->app, "/api/v1/rooms/:id/state", handle_get_room_state);
    iris_app_get(api->app, "/api/v1/call_center_events", handle_get_call_center_events);
    iris_app_get(api->app, "/api/v1/rooms/:id/call_center_events",
                 handle_get_call_center_events);
    iris_app_get(api->app, "/api/v1/participant", handle_get_participant);
    iris_app_get(api->app, "/api/v1/room_diagnostic", handle_get_room_diagnostic);
    iris_app_get(api->app, "/api/v1/rooms/:id/room_diagnostic",
                 handle_get_room_diagnostic);
    iris_app_get(api->app, "/api/v1/room_sync_diagnostic",
                 handle_get_room_sync_diagnostic);
    iris_app_get(api->app, "/api/v1/rooms/:id/room_sync_diagnostic",
                 handle_get_room_sync_diagnostic);
    iris_app_get(api->app, "/api/v1/participant_bandwidth_diagnostic",
                 handle_get_participant_bandwidth_diagnostic);
    iris_app_get(api->app,
                 "/api/v1/rooms/:id/participant_bandwidth_diagnostic/:participant_id",
                 handle_get_participant_bandwidth_diagnostic);
    iris_app_get(api->app, "/api/v1/track", handle_get_track);
    iris_app_get(api->app, "/api/v1/subscription", handle_get_subscription);
    iris_app_get(api->app, "/api/v1/subscription_diagnostic",
                 handle_get_subscription_diagnostic);
    iris_app_get(api->app,
                 "/api/v1/rooms/:id/subscription_diagnostic/:subscriber_participant_id/:track_id",
                 handle_get_subscription_diagnostic);
    iris_app_get(api->app,
                 "/api/v1/rooms/:id/subscription_diagnostic/:subscriber_id/:track",
                 handle_get_subscription_diagnostic);
    iris_app_post(api->app, "/api/v1/commands", handle_command);

    return api;
}

int room_service_http_api_start(room_service_http_api_t *api, const char *host, int port) {
    if (!api || !host || host[0] == '\0' || port <= 0 || port > UINT16_MAX) {
        return -1;
    }

    turbo_mutex_lock(&api->lifecycle_mutex);
    if (api->state != ROOM_SERVICE_HTTP_STOPPED || api->thread_started) {
        turbo_mutex_unlock(&api->lifecycle_mutex);
        return -1;
    }
    api->host = host;
    api->port = port;
    api->state = ROOM_SERVICE_HTTP_STARTING;
    if (turbo_thread_create(&api->thread, room_service_http_thread, api) != 0) {
        api->state = ROOM_SERVICE_HTTP_STOPPED;
        turbo_mutex_unlock(&api->lifecycle_mutex);
        return -1;
    }

    api->thread_started = 1;
    while (api->state == ROOM_SERVICE_HTTP_STARTING) {
        turbo_cond_wait(&api->lifecycle_cond, &api->lifecycle_mutex);
    }
    if (api->state == ROOM_SERVICE_HTTP_RUNNING) {
        turbo_mutex_unlock(&api->lifecycle_mutex);
        return 0;
    }
    turbo_mutex_unlock(&api->lifecycle_mutex);

    turbo_thread_join(&api->thread);
    turbo_mutex_lock(&api->lifecycle_mutex);
    api->thread_started = 0;
    api->state = ROOM_SERVICE_HTTP_STOPPED;
    turbo_mutex_unlock(&api->lifecycle_mutex);
    return -1;
}

void room_service_http_api_stop(room_service_http_api_t *api) {
    int should_join;

    if (!api) {
        return;
    }

    turbo_mutex_lock(&api->lifecycle_mutex);
    if (api->state == ROOM_SERVICE_HTTP_RUNNING && api->ctx) {
        api->state = ROOM_SERVICE_HTTP_STOPPING;
        coro_context_stop(api->ctx);
    }
    should_join = api->thread_started;
    turbo_mutex_unlock(&api->lifecycle_mutex);

    if (should_join) {
        turbo_thread_join(&api->thread);
        turbo_mutex_lock(&api->lifecycle_mutex);
        api->thread_started = 0;
        api->state = ROOM_SERVICE_HTTP_STOPPED;
        turbo_mutex_unlock(&api->lifecycle_mutex);
    }
}

void room_service_http_api_destroy(room_service_http_api_t *api) {
    if (!api) {
        return;
    }

    room_service_http_api_stop(api);
    if (api->app) {
        iris_app_destroy(api->app);
    }
    turbo_cond_destroy(&api->lifecycle_cond);
    turbo_mutex_destroy(&api->lifecycle_mutex);
    free(api);
}

char *room_service_http_api_build_room_diagnostic(room_service_app_server_t *server,
                                                  const char *room_id) {
    turbo_room_service_t *service;

    if (!server || !room_id) {
        return NULL;
    }

    service = room_service_app_server_get_service(server);
    if (!service) {
        return NULL;
    }

    return room_diagnostic_json(server, service, room_id);
}

char *room_service_http_api_build_room_sync_diagnostic(room_service_app_server_t *server,
                                                       const char *room_id) {
    turbo_room_service_t *service;

    if (!server || !room_id) {
        return NULL;
    }

    service = room_service_app_server_get_service(server);
    if (!service) {
        return NULL;
    }

    return room_sync_view_json(server, service, room_id);
}
