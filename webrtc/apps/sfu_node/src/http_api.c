#include "sfu_node/http_api.h"
#include <async.h>
#include <iris/iris_app.h>
#include <iris/server.h>
#include <iris/router.h>
#include <platform.h>
#include <turbo_coro_context.h>
#include <turbo_coro_socket.h>
#include <turbo_parser.h>
#include <turbo_thread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct sfu_node_http_api_s {
    iris_app_t *app;
    sfu_node_app_server_t *server;
    turbo_thread_t thread;
    int thread_started;
    coro_context_t *ctx;
    coro_socket_t *listener;
    int running;
    int port;
    int registered;
    struct sfu_node_http_api_s *next;
};

static turbo_mutex_t g_sfu_node_http_registry_mutex;
static turbo_once_t g_sfu_node_http_registry_once = TURBO_ONCE_INIT;
static sfu_node_http_api_t *g_sfu_node_http_registry = NULL;

static void sfu_node_http_registry_init(void) {
    turbo_mutex_init(&g_sfu_node_http_registry_mutex);
}

static void sfu_node_http_registry_lock(void) {
    turbo_once(&g_sfu_node_http_registry_once, sfu_node_http_registry_init);
    turbo_mutex_lock(&g_sfu_node_http_registry_mutex);
}

static void sfu_node_http_registry_register(sfu_node_http_api_t *api) {
    if (!api || !api->app || api->registered) {
        return;
    }

    sfu_node_http_registry_lock();
    api->next = g_sfu_node_http_registry;
    g_sfu_node_http_registry = api;
    api->registered = 1;
    turbo_mutex_unlock(&g_sfu_node_http_registry_mutex);
}

static void sfu_node_http_registry_unregister(sfu_node_http_api_t *api) {
    sfu_node_http_api_t **cursor;

    if (!api || !api->registered) {
        return;
    }

    sfu_node_http_registry_lock();
    cursor = &g_sfu_node_http_registry;
    while (*cursor) {
        if (*cursor == api) {
            *cursor = api->next;
            api->next = NULL;
            api->registered = 0;
            turbo_mutex_unlock(&g_sfu_node_http_registry_mutex);
            return;
        }
        cursor = &(*cursor)->next;
    }
    api->registered = 0;
    turbo_mutex_unlock(&g_sfu_node_http_registry_mutex);
}

static sfu_node_app_server_t *sfu_node_http_server_from_req(const Req *req) {
    sfu_node_http_api_t *cursor;
    sfu_node_app_server_t *server = NULL;

    if (!req || !req->app) {
        return NULL;
    }

    sfu_node_http_registry_lock();
    cursor = g_sfu_node_http_registry;
    while (cursor) {
        if (cursor->app == req->app) {
            server = cursor->server;
            break;
        }
        cursor = cursor->next;
    }
    turbo_mutex_unlock(&g_sfu_node_http_registry_mutex);
    return server;
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

    value = turbo_json_object_get(obj, key);
    if (!value || turbo_json_type(value) != TURBO_JSON_STRING) {
        return NULL;
    }

    return turbo_json_string(value);
}

static int json_int_field(const json_value_t *obj, const char *key, int def) {
    json_value_t *value;

    if (!obj || !key) {
        return def;
    }

    value = turbo_json_object_get(obj, key);
    if (!value || turbo_json_type(value) != TURBO_JSON_NUMBER) {
        return def;
    }

    return (int)turbo_json_number(value);
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

    array = turbo_json_object_get(obj, key);
    if (!array) {
        return 0;
    }
    if (turbo_json_type(array) != TURBO_JSON_ARRAY) {
        return -1;
    }

    count = turbo_json_array_size(array);
    if (count == 0) {
        return 0;
    }

    values = (uint32_t *)calloc(count, sizeof(*values));
    if (!values) {
        return -1;
    }

    for (i = 0; i < count; ++i) {
        json_value_t *item = turbo_json_array_get(array, i);
        double number;

        if (!item || turbo_json_type(item) != TURBO_JSON_NUMBER) {
            free(values);
            return -1;
        }

        number = turbo_json_number(item);
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
    return config && config->control_token && config->control_token[0] != '\0';
}

static int bearer_token_matches(const char *authorization, const char *token) {
    const char *prefix = "Bearer ";
    size_t prefix_len = strlen(prefix);

    if (!authorization || !token || strncmp(authorization, prefix, prefix_len) != 0) {
        return 0;
    }

    return strcmp(authorization + prefix_len, token) == 0;
}

static int request_has_control_auth(const Req *req, const sfu_node_app_config_t *config) {
    const char *authorization;

    if (!control_auth_enabled(config)) {
        return 1;
    }

    authorization = get_headers(req, "Authorization");
    if (!authorization) {
        authorization = get_headers(req, "authorization");
    }

    return bearer_token_matches(authorization, config->control_token);
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

static void handle_command(Req *req, Res *res) {
    sfu_node_app_server_t *server = sfu_node_http_server_from_req(req);
    turbo_sfu_node_t *node;
    json_value_t *root = NULL;
    const char *type;
    const char *room_id;
    const sfu_node_app_config_t *config;
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

    if (turbo_parse_json((const uint8_t *)req->body, req->body_len, &root) != 0 ||
        !root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
        send_error_json(res, 400, "INVALID_JSON", "request body must be a JSON object");
        turbo_free_json(&root);
        return;
    }

    type = json_string_field(root, "type");
    room_id = json_string_field(root, "room_id");
    if (!type) {
        send_error_json(res, 400, "INVALID_REQUEST", "missing command type");
        turbo_free_json(&root);
        return;
    }
    config = sfu_node_app_server_get_config(server);
    if (command_requires_control_auth(type) && !request_has_control_auth(req, config)) {
        send_error_json(res, 401, "UNAUTHORIZED", "control token required");
        turbo_free_json(&root);
        return;
    }

    if (strcmp(type, "set_node_drain") == 0) {
        char *json;

        rc = sfu_node_app_server_set_draining(
            server, json_bool_field(root, "draining", 1));
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            turbo_free_json(&root);
            return;
        }
        json = node_stats_json(server, node);
        if (!json) {
            send_error_json(res, 500, "SERVER_NOT_READY", "sfu node unavailable");
            turbo_free_json(&root);
            return;
        }
        send_entity_ok_json(res, "node_stats", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "attach_room") == 0) {
        if (!room_id) {
            send_error_json(res, 400, "INVALID_REQUEST", "missing room_id");
            turbo_free_json(&root);
            return;
        }
        if (sfu_node_app_server_is_draining(server)) {
            send_error_json(res, 409, "NODE_DRAINING",
                            "sfu node is draining and not accepting new rooms");
            turbo_free_json(&root);
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
            turbo_free_json(&root);
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
    } else if (strcmp(type, "create_webrtc_session") == 0) {
        const char *participant_id = json_string_field(root, "participant_id");
        const char *session_id = json_string_field(root, "session_id");
        char *json;

        if (!room_id || !participant_id || !session_id) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "missing room_id, participant_id or session_id");
            turbo_free_json(&root);
            return;
        }

        rc = sfu_node_app_server_create_webrtc_session(server, room_id,
                                                       participant_id, session_id);
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            turbo_free_json(&root);
            return;
        }

        json = webrtc_session_json(server, room_id, session_id);
        if (!json) {
            send_error_json(res, 500, "COMMAND_FAILED", "webrtc session creation incomplete");
            turbo_free_json(&root);
            return;
        }

        send_entity_ok_json(res, "webrtc_session", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "set_remote_offer") == 0) {
        const char *session_id = json_string_field(root, "session_id");
        const char *sdp = json_string_field(root, "sdp");
        char *json;

        if (!room_id || !session_id || !sdp) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "missing room_id, session_id or sdp");
            turbo_free_json(&root);
            return;
        }

        rc = sfu_node_app_server_set_remote_offer(server, room_id, session_id, sdp);
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            turbo_free_json(&root);
            return;
        }

        json = webrtc_session_json(server, room_id, session_id);
        if (!json) {
            send_error_json(res, 500, "COMMAND_FAILED", "webrtc session update incomplete");
            turbo_free_json(&root);
            return;
        }

        send_entity_ok_json(res, "webrtc_session", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "add_remote_ice_candidate") == 0) {
        const char *session_id = json_string_field(root, "session_id");
        const char *candidate = json_string_field(root, "candidate");
        char *json;

        if (!room_id || !session_id || !candidate) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "missing room_id, session_id or candidate");
            turbo_free_json(&root);
            return;
        }

        rc = sfu_node_app_server_add_remote_ice_candidate(server, room_id,
                                                          session_id, candidate);
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            turbo_free_json(&root);
            return;
        }

        json = webrtc_session_json(server, room_id, session_id);
        if (!json) {
            send_error_json(res, 500, "COMMAND_FAILED", "webrtc session update incomplete");
            turbo_free_json(&root);
            return;
        }

        send_entity_ok_json(res, "webrtc_session", json);
        turbo_free_json(&root);
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
            turbo_free_json(&root);
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

        rc = turbo_sfu_node_apply_track_subscription(node, room_id, &subscription);
        if (rc == 0) {
            rc = sfu_node_app_server_apply_track_subscription(
                server, room_id, &subscription);
        }
    } else if (strcmp(type, "start_recording") == 0) {
        const char *recording_id = json_string_field(root, "recording_id");
        const char *mode = json_string_field(root, "mode");
        char *json;

        if (!room_id || !recording_id || !mode) {
            send_error_json(res, 400, "INVALID_REQUEST",
                            "missing room_id, recording_id or mode");
            turbo_free_json(&root);
            return;
        }

        rc = sfu_node_app_server_start_recording(server, room_id,
                                                 recording_id, mode);
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            turbo_free_json(&root);
            return;
        }

        json = recording_status_json(server, room_id);
        if (!json) {
            send_error_json(res, 500, "COMMAND_FAILED", "recording start incomplete");
            turbo_free_json(&root);
            return;
        }

        send_entity_ok_json(res, "recording_status", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "stop_recording") == 0) {
        char *json;

        if (!room_id) {
            send_error_json(res, 400, "INVALID_REQUEST", "missing room_id");
            turbo_free_json(&root);
            return;
        }

        rc = sfu_node_app_server_stop_recording(server, room_id);
        if (rc != 0) {
            send_error_json(res, 400, "COMMAND_FAILED", "command execution failed");
            turbo_free_json(&root);
            return;
        }

        json = recording_status_json(server, room_id);
        if (!json) {
            send_error_json(res, 500, "COMMAND_FAILED", "recording stop incomplete");
            turbo_free_json(&root);
            return;
        }

        send_entity_ok_json(res, "recording_status", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_recording_status") == 0) {
        char *json;

        if (!room_id) {
            send_error_json(res, 400, "INVALID_REQUEST", "missing room_id");
            turbo_free_json(&root);
            return;
        }

        json = recording_status_json(server, room_id);
        if (!json) {
            send_error_json(res, 404, "RECORDING_NOT_FOUND", "recording not found");
            turbo_free_json(&root);
            return;
        }

        send_entity_ok_json(res, "recording_status", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_room_stats") == 0) {
        char *json = room_stats_json(node, room_id);

        if (!json) {
            send_error_json(res, 404, "ROOM_NOT_FOUND", "room not found");
            turbo_free_json(&root);
            return;
        }

        send_entity_ok_json(res, "room_stats", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_participant_stats") == 0) {
        char *json = participant_stats_json(node, room_id,
                                            json_string_field(root, "participant_id"));

        if (!json) {
            send_error_json(res, 404, "PARTICIPANT_NOT_FOUND", "participant not found");
            turbo_free_json(&root);
            return;
        }

        send_entity_ok_json(res, "participant_stats", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_track_subscription") == 0) {
        char *json = track_subscription_json(
            node, room_id, json_string_field(root, "receiver_participant_id"),
            json_string_field(root, "track_id"));

        if (!json) {
            send_error_json(res, 404, "TRACK_SUBSCRIPTION_NOT_FOUND",
                            "track subscription not found");
            turbo_free_json(&root);
            return;
        }

        send_entity_ok_json(res, "track_subscription", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_webrtc_session") == 0) {
        char *json = webrtc_session_json(server, room_id,
                                         json_string_field(root, "session_id"));

        if (!json) {
            send_error_json(res, 404, "WEBRTC_SESSION_NOT_FOUND",
                            "webrtc session not found");
            turbo_free_json(&root);
            return;
        }

        send_entity_ok_json(res, "webrtc_session", json);
        turbo_free_json(&root);
        return;
    } else if (strcmp(type, "get_node_stats") == 0) {
        char *json = node_stats_json(server, node);

        if (!json) {
            send_error_json(res, 500, "SERVER_NOT_READY", "sfu node unavailable");
            turbo_free_json(&root);
            return;
        }

        send_entity_ok_json(res, "node_stats", json);
        turbo_free_json(&root);
        return;
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

    if (room_id && strcmp(type, "detach_room") != 0 &&
        strcmp(type, "force_close_room") != 0) {
        char *json = room_stats_json(node, room_id);
        if (json) {
            send_entity_ok_json(res, "room_stats", json);
            turbo_free_json(&root);
            return;
        }
    }

    send_json(res, 200, "{\"ok\":true}");
    turbo_free_json(&root);
}

static void sfu_node_http_thread(void *arg) {
    sfu_node_http_api_t *api = (sfu_node_http_api_t *)arg;
    int async_initialized = 0;

    api->ctx = coro_context_create(NULL);
    if (!api->ctx) {
        api->running = 0;
        return;
    }

    if (iris_async_init(1) == 0) {
        async_initialized = 1;
    }

    api->listener = iris_server_start(api->app, api->ctx, (unsigned short)api->port);
    if (!api->listener) {
        coro_context_destroy(api->ctx);
        api->ctx = NULL;
        api->running = 0;
        if (async_initialized) {
            iris_async_shutdown();
        }
        return;
    }

    coro_context_set_persistent(api->ctx, 1);
    coro_context_run(api->ctx, TURBO_RUN_DEFAULT);

    if (api->listener) {
        coro_socket_destroy(api->listener);
        api->listener = NULL;
    }
    if (api->ctx) {
        coro_context_destroy(api->ctx);
        api->ctx = NULL;
    }
    if (async_initialized) {
        iris_async_shutdown();
    }
}

sfu_node_http_api_t *sfu_node_http_api_create(sfu_node_app_server_t *server) {
    sfu_node_http_api_t *api;
    cors_t cors_opts;

    if (!server) {
        return NULL;
    }

    api = (sfu_node_http_api_t *)calloc(1, sizeof(*api));
    if (!api) {
        return NULL;
    }

    api->app = iris_app_create();
    if (!api->app) {
        free(api);
        return NULL;
    }

    api->server = server;
    sfu_node_http_registry_register(api);

    memset(&cors_opts, 0, sizeof(cors_opts));
    cors_opts.origin = "*";
    cors_opts.methods = "GET, POST, OPTIONS";
    cors_opts.headers = "Content-Type, Authorization";
    cors_opts.enabled = 1;
    iris_app_cors(api->app, &cors_opts);

    iris_app_get(api->app, "/health", handle_health);
    iris_app_get(api->app, "/ready", handle_ready);
    iris_app_get(api->app, "/metrics", handle_metrics);
    iris_app_get(api->app, "/api/v1/rooms/:room_id/webrtc_sessions/:session_id",
                 handle_get_webrtc_session);
    iris_app_post(api->app, "/api/v1/commands", handle_command);

    return api;
}

int sfu_node_http_api_start(sfu_node_http_api_t *api, const char *host, int port) {
    (void)host;

    if (!api || api->running || port <= 0) {
        return -1;
    }

    api->port = port;
    if (turbo_thread_create(&api->thread, sfu_node_http_thread, api) != 0) {
        return -1;
    }

    api->thread_started = 1;
    api->running = 1;
    return 0;
}

void sfu_node_http_api_stop(sfu_node_http_api_t *api) {
    if (!api) {
        return;
    }

    api->running = 0;
    if (api->ctx) {
        coro_context_set_persistent(api->ctx, 0);
    }
    if (api->listener) {
        coro_socket_destroy(api->listener);
        api->listener = NULL;
    }
    if (api->ctx) {
        coro_context_stop(api->ctx);
    }
    if (api->thread_started) {
        turbo_thread_join(&api->thread);
        api->thread_started = 0;
    }
}

void sfu_node_http_api_destroy(sfu_node_http_api_t *api) {
    if (!api) {
        return;
    }

    if (api->running) {
        sfu_node_http_api_stop(api);
    } else if (api->thread_started) {
        turbo_thread_join(&api->thread);
        api->thread_started = 0;
    }
    if (api->app) {
        sfu_node_http_registry_unregister(api);
        iris_app_destroy(api->app);
    }
    free(api);
}
