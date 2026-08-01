#include "room_service/server.h"
#include "room_service/http_api.h"
#include "http_client.h"
#include "turbo_media_auth.h"
#include "turbo_parser.h"
#include "turbo_room_service.h"
#include "turbo_thread.h"
#ifdef ENABLE_RTC_SCXML_WORKFLOW
#include "room_workflow_adapter.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static void room_service_sleep_ms(unsigned int ms) { Sleep(ms); }
#else
#include <unistd.h>
static void room_service_sleep_ms(unsigned int ms) { usleep(ms * 1000); }
#endif

struct room_service_app_server_s {
    room_service_app_config_t config;
    turbo_room_service_t *service;
    room_service_http_api_t *http_api;
    struct room_service_sfu_node_entry_s *sfu_nodes;
    int sfu_node_count;
    int sfu_node_capacity;
    int next_sfu_node_index;
    room_service_room_sync_diagnostic_t *room_sync_diagnostics;
    int room_sync_diagnostic_count;
    int room_sync_diagnostic_capacity;
    room_service_call_center_event_t *call_center_events;
    int call_center_event_count;
    int call_center_event_capacity;
    room_service_conference_policy_t *conference_policies;
    int conference_policy_count;
    int conference_policy_capacity;
    int64_t room_sync_sequence;
    int64_t call_center_event_sequence;
    int64_t conference_policy_sequence;
    turbo_mutex_t mutex;
    int running;
};

typedef struct {
    char subscriber_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char track_id[TURBO_TRACK_ID_MAX];
    int enabled;
    int priority;
    turbo_room_video_layer_t preferred_layer;
    turbo_room_video_layer_t target_layer;
    int muted;
    char policy_source[TURBO_POLICY_SOURCE_MAX];
} room_service_policy_subscription_t;

#define ROOM_SERVICE_LAYOUT_MODE_SPEAKER "speaker"
#define ROOM_SERVICE_LAYOUT_MODE_GRID "grid"
#define ROOM_SERVICE_POLICY_SOURCE_PREFIX "conference_policy"
#define ROOM_SERVICE_POLICY_SOURCE_AUDIO "conference_policy_audio"
#define ROOM_SERVICE_POLICY_SOURCE_SCREEN "conference_policy_screen"
#define ROOM_SERVICE_POLICY_SOURCE_PIN "conference_policy_pin"
#define ROOM_SERVICE_POLICY_SOURCE_ACTIVE "conference_policy_active"
#define ROOM_SERVICE_POLICY_SOURCE_CAMERA "conference_policy_camera"
#define ROOM_SERVICE_CALL_CENTER_POLICY_SOURCE_PREFIX "call_center"
#define ROOM_SERVICE_MAX_CALL_CENTER_EVENTS 512
#define ROOM_SERVICE_SFU_CONTROL_URL_MAX 512
#define ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX 256
#define ROOM_SERVICE_SFU_CONTROL_AUDIENCE "turbomedia-sfu-control"
#define ROOM_SERVICE_SFU_SCOPE_CONTROL_WRITE "sfu.control.write"
#define ROOM_SERVICE_SFU_SCOPE_CONTROL_DANGEROUS "sfu.control.dangerous"

typedef struct room_service_sfu_node_entry_s {
    char node_id[TURBO_NODE_ID_MAX];
    char control_url[ROOM_SERVICE_SFU_CONTROL_URL_MAX];
    char control_token[ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX];
} room_service_sfu_node_entry_t;

static const char *room_service_json_string_field(
    const json_value_t *obj, const char *key);

static void room_service_copy_string(char *dest, size_t dest_size, const char *src) {
    if (!dest || dest_size == 0) {
        return;
    }

    if (!src) {
        dest[0] = '\0';
        return;
    }

    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static int room_service_ensure_capacity(void **items, int *capacity, size_t item_size,
                                        int count_needed) {
    void *new_items;
    int new_capacity;

    if (!items || !capacity || item_size == 0 || count_needed <= *capacity) {
        return 0;
    }

    new_capacity = (*capacity > 0) ? *capacity : 4;
    while (new_capacity < count_needed) {
        new_capacity *= 2;
    }

    new_items = realloc(*items, item_size * (size_t)new_capacity);
    if (!new_items) {
        return -1;
    }

    *items = new_items;
    *capacity = new_capacity;
    return 0;
}

static int room_service_policy_has_managed_source(const char *policy_source) {
    size_t prefix_len = strlen(ROOM_SERVICE_POLICY_SOURCE_PREFIX);

    return policy_source &&
           strncmp(policy_source, ROOM_SERVICE_POLICY_SOURCE_PREFIX, prefix_len) == 0;
}

static int room_service_policy_has_call_center_source(const char *policy_source) {
    size_t prefix_len = strlen(ROOM_SERVICE_CALL_CENTER_POLICY_SOURCE_PREFIX);

    return policy_source &&
           strncmp(policy_source, ROOM_SERVICE_CALL_CENTER_POLICY_SOURCE_PREFIX,
                   prefix_len) == 0;
}

static int room_service_layout_mode_is_valid(const char *layout_mode) {
    return layout_mode &&
           (strcmp(layout_mode, ROOM_SERVICE_LAYOUT_MODE_SPEAKER) == 0 ||
            strcmp(layout_mode, ROOM_SERVICE_LAYOUT_MODE_GRID) == 0);
}

static const char *room_service_normalize_layout_mode(const char *layout_mode) {
    if (!room_service_layout_mode_is_valid(layout_mode)) {
        return ROOM_SERVICE_LAYOUT_MODE_SPEAKER;
    }

    return layout_mode;
}

static turbo_room_layout_mode_t room_service_layout_mode_to_core(
    const char *layout_mode) {
    if (layout_mode && strcmp(layout_mode, ROOM_SERVICE_LAYOUT_MODE_GRID) == 0) {
        return TURBO_ROOM_LAYOUT_GRID;
    }

    return TURBO_ROOM_LAYOUT_SPEAKER;
}

static room_service_conference_policy_t *room_service_find_conference_policy(
    room_service_app_server_t *server, const char *room_id) {
    int i;

    if (!server || !room_id) {
        return NULL;
    }

    for (i = 0; i < server->conference_policy_count; ++i) {
        if (strcmp(server->conference_policies[i].room_id, room_id) == 0) {
            return &server->conference_policies[i];
        }
    }

    return NULL;
}

static int room_service_room_exists(room_service_app_server_t *server, const char *room_id) {
    turbo_room_summary_t summary;

    if (!server || !server->service || !room_id) {
        return 0;
    }

    return turbo_room_service_get_room_summary(server->service, room_id, &summary) == 0;
}

#ifdef ENABLE_RTC_SCXML_WORKFLOW
static const char *room_service_workflow_path_for_room(room_service_app_server_t *server,
                                                       const char *room_id) {
    turbo_room_summary_t room_summary;
    turbo_call_center_room_summary_t call_center_summary;
    int i;

    if (server && server->service && room_id &&
        turbo_room_service_get_call_center_room_summary(server->service, room_id,
                                                        &call_center_summary) == 0) {
        return "media/workflows/call_center_room.scxml";
    }

    if (!server || !server->service || !room_id ||
        turbo_room_service_get_room_summary(server->service, room_id, &room_summary) != 0) {
        return "media/workflows/conference_room.scxml";
    }

    for (i = 0; i < room_summary.participant_count; ++i) {
        turbo_room_participant_summary_t participant_summary;

        if (turbo_room_service_get_participant_summary_at(server->service, room_id, i,
                                                          &participant_summary) != 0) {
            continue;
        }

        switch (participant_summary.role) {
            case TURBO_PARTICIPANT_ROLE_CUSTOMER:
            case TURBO_PARTICIPANT_ROLE_AGENT:
            case TURBO_PARTICIPANT_ROLE_SUPERVISOR:
            case TURBO_PARTICIPANT_ROLE_QA_OBSERVER:
            case TURBO_PARTICIPANT_ROLE_BOT:
                return "media/workflows/call_center_room.scxml";
            default:
                break;
        }
    }

    return "media/workflows/conference_room.scxml";
}
#endif

static int room_service_participant_exists(room_service_app_server_t *server,
                                           const char *room_id,
                                           const char *participant_id) {
    turbo_room_participant_summary_t summary;

    if (!server || !server->service || !room_id || !participant_id || !participant_id[0]) {
        return 0;
    }

    return turbo_room_service_get_participant_summary(server->service, room_id, participant_id,
                                                      &summary) == 0;
}

static room_service_conference_policy_t *room_service_get_or_create_conference_policy_locked(
    room_service_app_server_t *server, const char *room_id) {
    room_service_conference_policy_t *policy;

    if (!server || !room_id) {
        return NULL;
    }

    policy = room_service_find_conference_policy(server, room_id);
    if (policy) {
        return policy;
    }

    if (room_service_ensure_capacity((void **)&server->conference_policies,
                                     &server->conference_policy_capacity,
                                     sizeof(*server->conference_policies),
                                     server->conference_policy_count + 1) != 0) {
        return NULL;
    }

    policy = &server->conference_policies[server->conference_policy_count++];
    memset(policy, 0, sizeof(*policy));
    policy->available = 1;
    room_service_copy_string(policy->room_id, sizeof(policy->room_id), room_id);
    room_service_copy_string(policy->layout_mode, sizeof(policy->layout_mode),
                             ROOM_SERVICE_LAYOUT_MODE_SPEAKER);
    policy->supervisor_mode = TURBO_CALL_CENTER_SUPERVISOR_NONE;
#ifdef ENABLE_RTC_SCXML_WORKFLOW
    {
        const char *wf_path = room_service_workflow_path_for_room(server, room_id);
        if (room_workflow_init(server, room_id, wf_path,
                               (room_workflow_context_t **)&policy->workflow_ctx) != 0) {
            fprintf(stderr, "[RoomService] Workflow init failed for room %s\n", room_id);
            /* policy is still usable without workflow */
        }
    }
#endif
    return policy;
}

static int room_service_fill_conference_policy(room_service_app_server_t *server,
                                               const char *room_id,
                                               room_service_conference_policy_t *out_policy) {
    room_service_conference_policy_t *policy;
    int rc = 0;

    if (!server || !room_id || !out_policy || !room_service_room_exists(server, room_id)) {
        return -1;
    }

    memset(out_policy, 0, sizeof(*out_policy));
    turbo_mutex_lock(&server->mutex);
    policy = room_service_find_conference_policy(server, room_id);
    if (policy) {
        *out_policy = *policy;
        goto out;
    }

    out_policy->available = 1;
    room_service_copy_string(out_policy->room_id, sizeof(out_policy->room_id), room_id);
    room_service_copy_string(out_policy->layout_mode, sizeof(out_policy->layout_mode),
                             ROOM_SERVICE_LAYOUT_MODE_SPEAKER);
    out_policy->supervisor_mode = TURBO_CALL_CENTER_SUPERVISOR_NONE;

out:
    turbo_mutex_unlock(&server->mutex);
    return rc;
}

static void room_service_capture_policy_warning(
    room_service_conference_policy_apply_result_t *result, const char *warning_code,
    const char *warning_message) {
    if (!result || result->had_warning || !warning_code || !warning_message) {
        return;
    }

    result->had_warning = 1;
    room_service_copy_string(result->warning_code, sizeof(result->warning_code),
                             warning_code);
    room_service_copy_string(result->warning_message, sizeof(result->warning_message),
                             warning_message);
}

static turbo_room_video_layer_t room_service_primary_camera_layer(
    const room_service_conference_policy_t *policy) {
    if (policy && strcmp(policy->layout_mode, ROOM_SERVICE_LAYOUT_MODE_GRID) == 0) {
        return TURBO_ROOM_VIDEO_LAYER_MEDIUM;
    }

    return TURBO_ROOM_VIDEO_LAYER_HIGH;
}

static void room_service_build_policy_subscription(
    const room_service_conference_policy_t *policy,
    const turbo_room_track_summary_t *track_summary, const char *subscriber_participant_id,
    room_service_policy_subscription_t *subscription) {
    int is_screen;
    int is_pinned_owner;
    int is_active_owner;
    turbo_room_video_layer_t primary_camera_layer;

    if (!policy || !track_summary || !subscriber_participant_id || !subscription) {
        return;
    }

    memset(subscription, 0, sizeof(*subscription));
    room_service_copy_string(subscription->subscriber_participant_id,
                             sizeof(subscription->subscriber_participant_id),
                             subscriber_participant_id);
    room_service_copy_string(subscription->track_id, sizeof(subscription->track_id),
                             track_summary->track_id);
    subscription->enabled = 1;

    if (track_summary->kind == TURBO_ROOM_TRACK_AUDIO) {
        subscription->priority = 400;
        subscription->preferred_layer = TURBO_ROOM_VIDEO_LAYER_LOW;
        subscription->target_layer = TURBO_ROOM_VIDEO_LAYER_LOW;
        room_service_copy_string(subscription->policy_source,
                                 sizeof(subscription->policy_source),
                                 ROOM_SERVICE_POLICY_SOURCE_AUDIO);
        return;
    }

    primary_camera_layer = room_service_primary_camera_layer(policy);
    is_screen = track_summary->source == TURBO_ROOM_SOURCE_SCREEN;
    is_pinned_owner =
        policy->pinned_participant_id[0] != '\0' &&
        strcmp(track_summary->owner_participant_id,
               policy->pinned_participant_id) == 0;
    is_active_owner =
        policy->active_speaker_participant_id[0] != '\0' &&
        strcmp(track_summary->owner_participant_id,
               policy->active_speaker_participant_id) == 0;

    if (is_screen) {
        subscription->priority = 300;
        subscription->preferred_layer = TURBO_ROOM_VIDEO_LAYER_HIGH;
        subscription->target_layer = TURBO_ROOM_VIDEO_LAYER_HIGH;
        room_service_copy_string(subscription->policy_source,
                                 sizeof(subscription->policy_source),
                                 ROOM_SERVICE_POLICY_SOURCE_SCREEN);
        return;
    }

    if (is_pinned_owner) {
        subscription->priority = 250;
        subscription->preferred_layer = primary_camera_layer;
        subscription->target_layer = primary_camera_layer;
        room_service_copy_string(subscription->policy_source,
                                 sizeof(subscription->policy_source),
                                 ROOM_SERVICE_POLICY_SOURCE_PIN);
        return;
    }

    if (!policy->pinned_participant_id[0] && is_active_owner) {
        subscription->priority = 200;
        subscription->preferred_layer = primary_camera_layer;
        subscription->target_layer = primary_camera_layer;
        room_service_copy_string(subscription->policy_source,
                                 sizeof(subscription->policy_source),
                                 ROOM_SERVICE_POLICY_SOURCE_ACTIVE);
        return;
    }

    subscription->priority = 100;
    subscription->preferred_layer = TURBO_ROOM_VIDEO_LAYER_LOW;
    subscription->target_layer = TURBO_ROOM_VIDEO_LAYER_LOW;
    room_service_copy_string(subscription->policy_source,
                             sizeof(subscription->policy_source),
                             ROOM_SERVICE_POLICY_SOURCE_CAMERA);
}

static int room_service_desired_subscription_matches(
    const turbo_room_subscription_summary_t *summary,
    const room_service_policy_subscription_t *desired) {
    return summary && desired &&
           strcmp(summary->subscriber_participant_id,
                  desired->subscriber_participant_id) == 0 &&
           strcmp(summary->track_id, desired->track_id) == 0 &&
           summary->enabled == desired->enabled &&
           summary->priority == desired->priority &&
           summary->preferred_layer == desired->preferred_layer &&
           summary->target_layer == desired->target_layer &&
           summary->muted == desired->muted &&
           strcmp(summary->policy_source, desired->policy_source) == 0;
}

static int room_service_policy_has_desired_subscription(
    const room_service_policy_subscription_t *subscriptions, int subscription_count,
    const char *subscriber_participant_id, const char *track_id) {
    int i;

    if (!subscriptions || subscription_count <= 0 || !subscriber_participant_id || !track_id) {
        return 0;
    }

    for (i = 0; i < subscription_count; ++i) {
        if (strcmp(subscriptions[i].subscriber_participant_id,
                   subscriber_participant_id) == 0 &&
            strcmp(subscriptions[i].track_id, track_id) == 0) {
            return 1;
        }
    }

    return 0;
}

static int room_service_capture_policy_subscriptions(
    turbo_room_service_t *service, const char *room_id,
    int (*source_filter)(const char *policy_source),
    room_service_policy_subscription_t **out_subscriptions, int *out_count) {
    turbo_room_summary_t room_summary;
    room_service_policy_subscription_t *subscriptions = NULL;
    int count = 0;
    int capacity = 0;
    int i;

    if (out_subscriptions) {
        *out_subscriptions = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!service || !room_id || !source_filter || !out_subscriptions || !out_count ||
        turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0) {
        return -1;
    }

    for (i = 0; i < room_summary.subscription_count; ++i) {
        turbo_room_subscription_summary_t summary;

        if (turbo_room_service_get_subscription_summary_at(service, room_id, i,
                                                           &summary) != 0) {
            free(subscriptions);
            return -1;
        }
        if (!source_filter(summary.policy_source)) {
            continue;
        }

        if (room_service_ensure_capacity((void **)&subscriptions, &capacity,
                                         sizeof(*subscriptions), count + 1) != 0) {
            free(subscriptions);
            return -1;
        }

        memset(&subscriptions[count], 0, sizeof(subscriptions[count]));
        room_service_copy_string(subscriptions[count].subscriber_participant_id,
                                 sizeof(subscriptions[count].subscriber_participant_id),
                                 summary.subscriber_participant_id);
        room_service_copy_string(subscriptions[count].track_id,
                                 sizeof(subscriptions[count].track_id),
                                 summary.track_id);
        subscriptions[count].enabled = summary.enabled;
        subscriptions[count].priority = summary.priority;
        subscriptions[count].preferred_layer = summary.preferred_layer;
        subscriptions[count].target_layer = summary.target_layer;
        subscriptions[count].muted = summary.muted;
        room_service_copy_string(subscriptions[count].policy_source,
                                 sizeof(subscriptions[count].policy_source),
                                 summary.policy_source);
        count++;
    }

    *out_subscriptions = subscriptions;
    *out_count = count;
    return 0;
}

static room_service_room_sync_diagnostic_t *room_service_find_room_sync_diagnostic(
    room_service_app_server_t *server, const char *room_id) {
    int i;

    if (!server || !room_id) {
        return NULL;
    }

    for (i = 0; i < server->room_sync_diagnostic_count; ++i) {
        if (strcmp(server->room_sync_diagnostics[i].room_id, room_id) == 0) {
            return &server->room_sync_diagnostics[i];
        }
    }

    return NULL;
}

static void room_service_prune_call_center_events_locked(
    room_service_app_server_t *server) {
    int excess;

    if (!server || server->call_center_event_count <= ROOM_SERVICE_MAX_CALL_CENTER_EVENTS) {
        return;
    }

    excess = server->call_center_event_count - ROOM_SERVICE_MAX_CALL_CENTER_EVENTS;
    memmove(server->call_center_events,
            server->call_center_events + excess,
            sizeof(*server->call_center_events) *
                (size_t)(server->call_center_event_count - excess));
    server->call_center_event_count -= excess;
}

static char *room_service_trim_token(char *value) {
    char *end;

    if (!value) {
        return NULL;
    }
    while (*value && isspace((unsigned char)*value)) {
        value++;
    }
    end = value + strlen(value);
    while (end > value && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    *end = '\0';
    return value;
}

static room_service_sfu_node_entry_t *room_service_find_sfu_node_locked(
    room_service_app_server_t *server, const char *node_id) {
    int i;

    if (!server || !node_id || node_id[0] == '\0') {
        return NULL;
    }

    for (i = 0; i < server->sfu_node_count; ++i) {
        if (strcmp(server->sfu_nodes[i].node_id, node_id) == 0) {
            return &server->sfu_nodes[i];
        }
    }

    return NULL;
}

int room_service_app_server_register_sfu_node(room_service_app_server_t *server,
                                              const char *node_id,
                                              const char *control_url,
                                              const char *control_token) {
    room_service_sfu_node_entry_t *entry;

    if (!server || !node_id || node_id[0] == '\0' || !control_url ||
        control_url[0] == '\0') {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    entry = room_service_find_sfu_node_locked(server, node_id);
    if (!entry) {
        if (room_service_ensure_capacity((void **)&server->sfu_nodes,
                                         &server->sfu_node_capacity,
                                         sizeof(*server->sfu_nodes),
                                         server->sfu_node_count + 1) != 0) {
            turbo_mutex_unlock(&server->mutex);
            return -1;
        }
        entry = &server->sfu_nodes[server->sfu_node_count++];
        memset(entry, 0, sizeof(*entry));
        room_service_copy_string(entry->node_id, sizeof(entry->node_id), node_id);
    }
    room_service_copy_string(entry->control_url, sizeof(entry->control_url), control_url);
    room_service_copy_string(entry->control_token, sizeof(entry->control_token),
                             control_token);
    turbo_mutex_unlock(&server->mutex);
    return 0;
}

int room_service_app_server_has_sfu_node(room_service_app_server_t *server,
                                         const char *node_id) {
    int found;

    if (!server || !node_id || node_id[0] == '\0') {
        return 0;
    }

    turbo_mutex_lock(&server->mutex);
    found = room_service_find_sfu_node_locked(server, node_id) != NULL;
    turbo_mutex_unlock(&server->mutex);
    return found;
}

int room_service_app_server_choose_sfu_node(room_service_app_server_t *server,
                                            char *node_id, size_t node_id_size) {
    int index;

    if (!server || !node_id || node_id_size == 0) {
        return -1;
    }

    node_id[0] = '\0';
    turbo_mutex_lock(&server->mutex);
    if (server->sfu_node_count > 0) {
        index = server->next_sfu_node_index % server->sfu_node_count;
        server->next_sfu_node_index++;
        room_service_copy_string(node_id, node_id_size, server->sfu_nodes[index].node_id);
        turbo_mutex_unlock(&server->mutex);
        return 0;
    }
    turbo_mutex_unlock(&server->mutex);

    if (server->config.sfu_control_url && server->config.sfu_control_url[0] != '\0') {
        room_service_copy_string(node_id, node_id_size, "default-sfu");
        return 0;
    }

    return -1;
}

static int room_service_register_sfu_nodes_from_config(
    room_service_app_server_t *server, const char *nodes) {
    char *copy;
    char *cursor;
    char *entry;

    if (!server || !nodes || nodes[0] == '\0') {
        return 0;
    }

    copy = (char *)malloc(strlen(nodes) + 1);
    if (!copy) {
        return -1;
    }
    strcpy(copy, nodes);

    cursor = copy;
    while ((entry = cursor) != NULL && *entry != '\0') {
        char *comma = strchr(entry, ',');
        char *equals;
        char *node_id;
        char *url;

        if (comma) {
            *comma = '\0';
            cursor = comma + 1;
        } else {
            cursor = NULL;
        }

        entry = room_service_trim_token(entry);
        if (!entry || entry[0] == '\0') {
            continue;
        }

        equals = strchr(entry, '=');
        if (!equals) {
            free(copy);
            return -1;
        }
        *equals = '\0';
        node_id = room_service_trim_token(entry);
        url = room_service_trim_token(equals + 1);
        if (!node_id || node_id[0] == '\0' || !url || url[0] == '\0' ||
            room_service_app_server_register_sfu_node(server, node_id, url,
                                                      server->config.sfu_control_token) != 0) {
            free(copy);
            return -1;
        }
    }

    free(copy);
    return 0;
}

static int room_service_copy_sfu_route_for_room(room_service_app_server_t *server,
                                                const char *room_id,
                                                char *control_url,
                                                size_t control_url_size,
                                                char *control_token,
                                                size_t control_token_size) {
    turbo_room_summary_t room_summary;
    turbo_room_service_t *service;
    room_service_sfu_node_entry_t *entry;
    int sfu_node_count;

    if (!server || !room_id || !control_url || control_url_size == 0 ||
        !control_token || control_token_size == 0) {
        return -1;
    }

    control_url[0] = '\0';
    control_token[0] = '\0';
    service = room_service_app_server_get_service(server);
    if (!service ||
        turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0 ||
        room_summary.assigned_sfu_node[0] == '\0') {
        if (server->config.sfu_control_url && server->config.sfu_control_url[0] != '\0') {
            room_service_copy_string(control_url, control_url_size,
                                     server->config.sfu_control_url);
            room_service_copy_string(control_token, control_token_size,
                                     server->config.sfu_control_token);
        }
        return 0;
    }

    turbo_mutex_lock(&server->mutex);
    sfu_node_count = server->sfu_node_count;
    entry = room_service_find_sfu_node_locked(server, room_summary.assigned_sfu_node);
    if (entry) {
        room_service_copy_string(control_url, control_url_size, entry->control_url);
        room_service_copy_string(control_token, control_token_size, entry->control_token);
        turbo_mutex_unlock(&server->mutex);
        return 0;
    }
    turbo_mutex_unlock(&server->mutex);

    if (sfu_node_count == 0 && server->config.sfu_control_url &&
        server->config.sfu_control_url[0] != '\0') {
        room_service_copy_string(control_url, control_url_size,
                                 server->config.sfu_control_url);
        room_service_copy_string(control_token, control_token_size,
                                 server->config.sfu_control_token);
        return 0;
    }

    return -1;
}

static http_client_t *room_service_create_sfu_client_for_route(
    room_service_app_server_t *server, const char *control_url,
    const char *control_token) {
    http_client_t *client;
    const char *ca_file;

    if (!control_url || control_url[0] == '\0') {
        return NULL;
    }

    client = http_client_create(control_url);
    if (!client) {
        return NULL;
    }

    ca_file = server ? server->config.sfu_ca_file : NULL;
    if (ca_file) {
        turbo_tls_client_config_t tls_config;

        memset(&tls_config, 0, sizeof(tls_config));
        tls_config.ca_file = ca_file;
        tls_config.verify_peer = 1;
        if (http_client_set_tls_client_config(client, &tls_config) != 0) {
            http_client_destroy(client);
            return NULL;
        }
    }

    http_client_set_timeout(client, 3000);
    http_client_set_user_agent(client, "TurboRoomService/0.1");
    if (control_token && control_token[0] != '\0') {
        http_client_set_bearer_token(client, control_token);
    }

    return client;
}

static int room_service_sfu_command_is_dangerous(const char *type) {
    return type &&
           (strcmp(type, "force_close_room") == 0 ||
            strcmp(type, "detach_room") == 0 ||
            strcmp(type, "set_node_drain") == 0 ||
            strcmp(type, "start_recording") == 0 ||
            strcmp(type, "stop_recording") == 0);
}

static int room_service_issue_sfu_command_token(
    room_service_app_server_t *server, const char *room_id,
    const char *command_json, char **out_token) {
    turbo_media_auth_config_t auth = {0};
    turbo_media_auth_claims_t claims = {0};
    json_value_t *root = NULL;
    const char *command_room_id;
    const char *participant_id;
    const char *type;
    int64_t now;

    if (!server || !room_id || !command_json || !out_token) {
        return -1;
    }
    *out_token = NULL;
    if (!server->config.sfu_auth_secret ||
        server->config.sfu_auth_secret[0] == '\0') {
        return 0;
    }
    if (turbo_parse_json((const uint8_t *)command_json, strlen(command_json),
                         &root) != 0 ||
        !root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
        turbo_free_json(&root);
        return -1;
    }
    type = room_service_json_string_field(root, "type");
    command_room_id = room_service_json_string_field(root, "room_id");
    participant_id = room_service_json_string_field(root, "participant_id");
    if (!type || !command_room_id || strcmp(command_room_id, room_id) != 0) {
        turbo_free_json(&root);
        return -1;
    }

    auth.issuer = server->config.sfu_auth_issuer;
    auth.active_key_id = server->config.sfu_auth_key_id;
    auth.active_secret = server->config.sfu_auth_secret;
    auth.clock_skew_seconds = 0;
    auth.max_ttl_seconds = server->config.sfu_auth_ttl_seconds;
    now = (int64_t)time(NULL);
    claims.subject = server->config.node_id;
    claims.audience = ROOM_SERVICE_SFU_CONTROL_AUDIENCE;
    claims.scope = room_service_sfu_command_is_dangerous(type)
                       ? ROOM_SERVICE_SFU_SCOPE_CONTROL_DANGEROUS
                       : ROOM_SERVICE_SFU_SCOPE_CONTROL_WRITE;
    claims.room_id = command_room_id;
    claims.participant_id = participant_id;
    claims.issued_at = now;
    claims.expires_at = now + server->config.sfu_auth_ttl_seconds;
    *out_token = turbo_media_auth_issue(&auth, &claims);
    turbo_free_json(&root);
    return *out_token ? 0 : -1;
}

static int room_service_post_sfu_command(room_service_app_server_t *server,
                                         const char *room_id,
                                         const char *command_json) {
    http_client_t *client;
    http_response_t *response;
    char control_url[ROOM_SERVICE_SFU_CONTROL_URL_MAX];
    char control_token[ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX];
    char *signed_token = NULL;
    const char *request_token;

    if (!server || !room_id || !command_json) {
        return 0;
    }

    if (room_service_copy_sfu_route_for_room(server, room_id, control_url,
                                             sizeof(control_url), control_token,
                                             sizeof(control_token)) != 0) {
        return -1;
    }
    if (control_url[0] == '\0') {
        return 0;
    }
    if (room_service_issue_sfu_command_token(server, room_id, command_json,
                                             &signed_token) != 0) {
        return -1;
    }
    request_token = signed_token ? signed_token : control_token;

    client = room_service_create_sfu_client_for_route(
        server, control_url, request_token);
    free(signed_token);
    if (!client) {
        return -1;
    }

    response = http_post_json(client, "/api/v1/commands", command_json);
    if (!response) {
        http_client_destroy(client);
        return -1;
    }

    if (response->error_code != HTTP_ERROR_NONE ||
        response->status_code < 200 || response->status_code >= 300) {
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    http_response_free(response);
    http_client_destroy(client);
    return 0;
}

static turbo_room_video_layer_t room_service_parse_video_layer(const char *value) {
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

static const char *room_service_video_layer_name(turbo_room_video_layer_t layer) {
    switch (layer) {
        case TURBO_ROOM_VIDEO_LAYER_NONE: return "none";
        case TURBO_ROOM_VIDEO_LAYER_LOW: return "low";
        case TURBO_ROOM_VIDEO_LAYER_MEDIUM: return "medium";
        case TURBO_ROOM_VIDEO_LAYER_HIGH: return "high";
        default: return "unknown";
    }
}

static turbo_room_video_layer_t room_service_resolve_subscription_max_layer(
    const turbo_room_subscription_summary_t *subscription) {
    if (!subscription || !subscription->enabled || subscription->muted) {
        return TURBO_ROOM_VIDEO_LAYER_NONE;
    }

    if (subscription->target_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
        return subscription->target_layer;
    }
    if (subscription->preferred_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
        return subscription->preferred_layer;
    }

    return TURBO_ROOM_VIDEO_LAYER_HIGH;
}

static const char *room_service_json_string_field(const json_value_t *obj, const char *key) {
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

static int room_service_json_bool_field(const json_value_t *obj, const char *key, int def) {
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

static int64_t room_service_json_int64_field(const json_value_t *obj, const char *key,
                                             int64_t def) {
    json_value_t *value;

    if (!obj || !key) {
        return def;
    }

    value = turbo_json_object_get(obj, key);
    if (!value || turbo_json_type(value) != TURBO_JSON_NUMBER) {
        return def;
    }

    return (int64_t)turbo_json_number(value);
}

room_service_app_server_t *room_service_app_server_create(
    const room_service_app_config_t *config) {
    room_service_app_server_t *server;

    if (!config || room_service_app_config_validate(config) != 0) {
        return NULL;
    }

    server = (room_service_app_server_t *)calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }

    memcpy(&server->config, config, sizeof(*config));
    turbo_mutex_init(&server->mutex);
    server->service = turbo_room_service_create();
    if (!server->service) {
        turbo_mutex_destroy(&server->mutex);
        free(server);
        return NULL;
    }
    if (room_service_register_sfu_nodes_from_config(server, config->sfu_nodes) != 0) {
        turbo_room_service_destroy(server->service);
        free(server->sfu_nodes);
        turbo_mutex_destroy(&server->mutex);
        free(server);
        return NULL;
    }
    server->http_api = room_service_http_api_create(server);
    if (!server->http_api) {
        turbo_room_service_destroy(server->service);
        free(server->sfu_nodes);
        turbo_mutex_destroy(&server->mutex);
        free(server);
        return NULL;
    }

    return server;
}

int room_service_app_server_start(room_service_app_server_t *server) {
    if (!server || !server->service) {
        return -1;
    }

    if (room_service_http_api_start(server->http_api, server->config.bind_host,
                                    server->config.bind_port) != 0) {
        return -1;
    }

    server->running = 1;
    return 0;
}

int room_service_app_server_run(room_service_app_server_t *server) {
    if (!server || !server->running) {
        return -1;
    }

    if (server->config.dry_run) {
        printf("room_service: dry-run complete\n");
        return 0;
    }

    printf("room_service: running on %s:%d with node_id=%s\n",
           server->config.bind_host,
           server->config.bind_port,
           server->config.node_id);

    while (server->running) {
        room_service_sleep_ms(100);
    }

    return 0;
}

void room_service_app_server_stop(room_service_app_server_t *server) {
    if (!server) {
        return;
    }

    server->running = 0;
    if (server->http_api) {
        room_service_http_api_stop(server->http_api);
    }
}

void room_service_app_server_destroy(room_service_app_server_t *server) {
    if (!server) {
        return;
    }

    if (server->service) {
        turbo_room_service_destroy(server->service);
    }
    if (server->http_api) {
        room_service_http_api_destroy(server->http_api);
    }
    free(server->sfu_nodes);
    free(server->call_center_events);
#ifdef ENABLE_RTC_SCXML_WORKFLOW
    for (int i = 0; i < server->conference_policy_count; ++i) {
        if (server->conference_policies[i].workflow_ctx) {
            room_workflow_destroy((room_workflow_context_t*)server->conference_policies[i].workflow_ctx);
        }
    }
#endif
    free(server->conference_policies);
    free(server->room_sync_diagnostics);
    turbo_mutex_destroy(&server->mutex);
    free(server);
}

turbo_room_service_t *room_service_app_server_get_service(
    room_service_app_server_t *server) {
    if (!server) {
        return NULL;
    }

    return server->service;
}

const room_service_app_config_t *room_service_app_server_get_config(
    room_service_app_server_t *server) {
    return server ? &server->config : NULL;
}

int room_service_app_server_get_stats(room_service_app_server_t *server,
                                      room_service_app_stats_t *stats) {
    if (!server || !stats) {
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    turbo_mutex_lock(&server->mutex);
    stats->running = server->running;
    stats->room_sync_diagnostic_count = server->room_sync_diagnostic_count;
    stats->call_center_event_count = server->call_center_event_count;
    stats->conference_policy_count = server->conference_policy_count;
    stats->sfu_node_count = server->sfu_node_count;
    turbo_mutex_unlock(&server->mutex);
    return 0;
}

int room_service_app_server_sync_attach_room(room_service_app_server_t *server,
                                             const char *room_id) {
    char json[256];

    if (!server || !room_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"attach_room\","
             "\"room_id\":\"%s\""
             "}",
             room_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_detach_room(room_service_app_server_t *server,
                                             const char *room_id) {
    char json[256];

    if (!server || !room_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"detach_room\","
             "\"room_id\":\"%s\""
             "}",
             room_id);

    int rc = room_service_post_sfu_command(server, room_id, json);
#ifdef ENABLE_RTC_SCXML_WORKFLOW
    if (rc == 0) {
        room_workflow_context_t *wf_ctx = NULL;
        turbo_mutex_lock(&server->mutex);
        {
            room_service_conference_policy_t *policy =
                room_service_find_conference_policy(server, room_id);
            if (policy) {
                wf_ctx = (room_workflow_context_t *)policy->workflow_ctx;
            }
        }
        turbo_mutex_unlock(&server->mutex);
        if (wf_ctx) {
            turbo_rtc_workflow_param_t params[1];
            params[0].name = "participant_id";
            params[0].value = participant_id;
            room_workflow_receive(wf_ctx, "participant.left", params, 1);
        }
    }
#endif
    return rc;
}

int room_service_app_server_sync_force_close_room(room_service_app_server_t *server,
                                                  const char *room_id) {
    char json[256];

    if (!server || !room_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"force_close_room\","
             "\"room_id\":\"%s\""
             "}",
             room_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_add_session(room_service_app_server_t *server,
                                             const char *room_id,
                                             const char *participant_id) {
    char json[384];

    if (!server || !room_id || !participant_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"add_session\","
             "\"room_id\":\"%s\","
             "\"participant_id\":\"%s\","
             "\"session_id\":\"%s\""
             "}",
             room_id, participant_id, participant_id);

    int rc = room_service_post_sfu_command(server, room_id, json);
#ifdef ENABLE_RTC_SCXML_WORKFLOW
    if (rc == 0) {
        room_workflow_context_t *wf_ctx = NULL;
        turbo_mutex_lock(&server->mutex);
        {
            room_service_conference_policy_t *policy =
                room_service_find_conference_policy(server, room_id);
            if (policy)
                wf_ctx = (room_workflow_context_t *)policy->workflow_ctx;
        }
        turbo_mutex_unlock(&server->mutex);
        /* Dispatch outside the lock: room_workflow_receive may call
         * sync_apply_track_subscription which performs a blocking HTTP request. */
        if (wf_ctx) {
            turbo_room_participant_summary_t p_summary;
            const char *role_str = "guest";
            if (turbo_room_service_get_participant_summary(
                    server->service, room_id, participant_id, &p_summary) == 0) {
                switch (p_summary.role) {
                    case TURBO_PARTICIPANT_ROLE_AGENT:      role_str = "agent";      break;
                    case TURBO_PARTICIPANT_ROLE_CUSTOMER:   role_str = "customer";   break;
                    case TURBO_PARTICIPANT_ROLE_SUPERVISOR: role_str = "supervisor"; break;
                    default:                                role_str = "guest";      break;
                }
            }
            turbo_rtc_workflow_param_t params[2];
            params[0].name  = "participant_id";
            params[0].value = participant_id;
            params[1].name  = "role";
            params[1].value = role_str;
            room_workflow_receive(wf_ctx, "participant.joined", params, 2);
        }
    }
#endif
    return rc;
}

int room_service_app_server_sync_remove_session(room_service_app_server_t *server,
                                                const char *room_id,
                                                const char *participant_id) {
    char json[384];

    if (!server || !room_id || !participant_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"remove_session\","
             "\"room_id\":\"%s\","
             "\"session_id\":\"%s\""
             "}",
             room_id, participant_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_set_receiver_bandwidth(
    room_service_app_server_t *server, const char *room_id, const char *participant_id,
    int bandwidth_bps) {
    char json[384];

    if (!server || !room_id || !participant_id || bandwidth_bps < 0) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"set_receiver_bandwidth\","
             "\"room_id\":\"%s\","
             "\"participant_id\":\"%s\","
             "\"bandwidth_bps\":%d"
             "}",
             room_id, participant_id, bandwidth_bps);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_set_track_subscription(
    room_service_app_server_t *server, const char *room_id,
    const char *receiver_participant_id, const char *track_id, int enabled,
    turbo_room_video_layer_t max_layer) {
    turbo_room_subscription_summary_t subscription;

    memset(&subscription, 0, sizeof(subscription));
    room_service_copy_string(subscription.subscriber_participant_id,
                             sizeof(subscription.subscriber_participant_id),
                             receiver_participant_id);
    room_service_copy_string(subscription.track_id, sizeof(subscription.track_id),
                             track_id);
    subscription.enabled = enabled ? 1 : 0;
    subscription.preferred_layer = max_layer;
    subscription.target_layer = max_layer;
    subscription.muted = enabled ? 0 : 1;
    return room_service_app_server_sync_apply_track_subscription(server, room_id,
                                                                 &subscription);
}

int room_service_app_server_sync_apply_track_subscription(
    room_service_app_server_t *server, const char *room_id,
    const turbo_room_subscription_summary_t *subscription) {
    char json[768];
    const char *preferred_layer_name;
    const char *target_layer_name;
    const char *max_layer_name;
    turbo_room_video_layer_t max_layer;

    if (!server || !room_id || !subscription ||
        !subscription->subscriber_participant_id[0] || !subscription->track_id[0]) {
        return -1;
    }

    max_layer = room_service_resolve_subscription_max_layer(subscription);
    if (max_layer < TURBO_ROOM_VIDEO_LAYER_NONE ||
        max_layer > TURBO_ROOM_VIDEO_LAYER_HIGH) {
        return -1;
    }
    preferred_layer_name = room_service_video_layer_name(subscription->preferred_layer);
    target_layer_name = room_service_video_layer_name(subscription->target_layer);
    max_layer_name = room_service_video_layer_name(max_layer);

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"set_track_subscription\","
             "\"room_id\":\"%s\","
             "\"receiver_participant_id\":\"%s\","
             "\"track_id\":\"%s\","
             "\"enabled\":%s,"
             "\"priority\":%d,"
             "\"preferred_layer\":\"%s\","
             "\"target_layer\":\"%s\","
             "\"muted\":%s,"
             "\"policy_source\":\"%s\","
             "\"max_layer\":\"%s\""
             "}",
             room_id, subscription->subscriber_participant_id, subscription->track_id,
             subscription->enabled ? "true" : "false",
             subscription->priority,
             preferred_layer_name,
             target_layer_name,
             subscription->muted ? "true" : "false",
             subscription->policy_source,
             max_layer_name);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_start_recording(room_service_app_server_t *server,
                                                 const char *room_id,
                                                 const char *recording_id,
                                                 const char *mode) {
    char json[512];

    if (!server || !room_id || !recording_id || !mode) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"start_recording\","
             "\"room_id\":\"%s\","
             "\"recording_id\":\"%s\","
             "\"mode\":\"%s\""
             "}",
             room_id, recording_id, mode);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_stop_recording(room_service_app_server_t *server,
                                                const char *room_id) {
    char json[256];

    if (!server || !room_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"stop_recording\","
             "\"room_id\":\"%s\""
             "}",
             room_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_ensure_recording(
    room_service_app_server_t *server, const char *room_id,
    const char *recording_id, const char *mode, int *started) {
    room_service_sfu_recording_status_t status;

    if (started) {
        *started = 0;
    }
    if (!server || !room_id || !recording_id || !recording_id[0] ||
        !mode || !mode[0]) {
        return -1;
    }

    memset(&status, 0, sizeof(status));
    if (room_service_app_server_fetch_recording_status(server, room_id,
                                                       &status) != 0) {
        return -1;
    }

    if (status.found && status.active) {
        if (strcmp(status.recording_id, recording_id) == 0 &&
            strcmp(status.mode, mode) == 0) {
            return 0;
        }
        if (room_service_app_server_sync_stop_recording(server, room_id) != 0) {
            return -1;
        }
    }

    if (room_service_app_server_sync_start_recording(server, room_id,
                                                     recording_id, mode) != 0) {
        return -1;
    }
    if (started) {
        *started = 1;
    }
    return 0;
}

int room_service_app_server_sync_register_track(room_service_app_server_t *server,
                                                const char *room_id,
                                                const turbo_room_track_summary_t *track) {
    char json[640];
    int written;
    int i;

    if (!server || !room_id || !track || track->main_ssrc == 0) {
        return -1;
    }

    written = snprintf(json, sizeof(json),
                       "{"
                       "\"type\":\"register_published_track\","
                       "\"room_id\":\"%s\","
                       "\"participant_id\":\"%s\","
                       "\"track_id\":\"%s\","
                       "\"kind\":\"%s\","
                       "\"codec_name\":\"%s\","
                       "\"main_ssrc\":%u,"
                       "\"layer_ssrcs\":[",
                       room_id, track->owner_participant_id, track->track_id,
                       track->kind == TURBO_ROOM_TRACK_AUDIO ? "audio" : "video",
                       track->codec_name,
                       (unsigned)track->main_ssrc);
    if (written < 0 || written >= (int)sizeof(json)) {
        return -1;
    }

    for (i = 0; i < track->layer_count && i < 3; ++i) {
        int rc = snprintf(json + written, sizeof(json) - (size_t)written,
                          "%s%u", i == 0 ? "" : ",",
                          (unsigned)track->layer_ssrcs[i]);
        if (rc < 0 || rc >= (int)(sizeof(json) - (size_t)written)) {
            return -1;
        }
        written += rc;
    }

    if (track->layer_count <= 0) {
        int rc = snprintf(json + written, sizeof(json) - (size_t)written, "%u",
                          (unsigned)track->main_ssrc);
        if (rc < 0 || rc >= (int)(sizeof(json) - (size_t)written)) {
            return -1;
        }
        written += rc;
    }

    if (snprintf(json + written, sizeof(json) - (size_t)written, "]}")
        >= (int)(sizeof(json) - (size_t)written)) {
        return -1;
    }

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_unregister_track(room_service_app_server_t *server,
                                                  const char *room_id,
                                                  const char *track_id) {
    char json[384];

    if (!server || !room_id || !track_id) {
        return -1;
    }

    snprintf(json, sizeof(json),
             "{"
             "\"type\":\"unregister_published_track\","
             "\"room_id\":\"%s\","
             "\"track_id\":\"%s\""
             "}",
             room_id, track_id);

    return room_service_post_sfu_command(server, room_id, json);
}

int room_service_app_server_sync_replay_room_state(
    room_service_app_server_t *server, const char *room_id,
    room_service_sfu_replay_stats_t *out_stats) {
    turbo_room_service_t *service;
    turbo_room_summary_t room_summary;
    room_service_sfu_replay_stats_t stats;
    int i;

    memset(&stats, 0, sizeof(stats));

    if (!server || !room_id) {
        return -1;
    }

    service = room_service_app_server_get_service(server);
    if (!service ||
        turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0) {
        return -1;
    }

    if (room_service_app_server_sync_attach_room(server, room_id) != 0) {
        return -1;
    }

    for (i = 0; i < room_summary.participant_count; ++i) {
        turbo_room_participant_summary_t participant_summary;
        int effective_bandwidth_bps = 0;

        if (turbo_room_service_get_participant_summary_at(service, room_id, i,
                                                          &participant_summary) != 0) {
            return -1;
        }

        if (room_service_app_server_sync_add_session(server, room_id,
                                                     participant_summary.participant_id) != 0) {
            return -1;
        }
        stats.participants_replayed++;

        if (turbo_room_service_get_effective_receiver_bandwidth(
                service, room_id, participant_summary.participant_id,
                &effective_bandwidth_bps, NULL) != 0) {
            return -1;
        }

        if (room_service_app_server_sync_set_receiver_bandwidth(
                server, room_id, participant_summary.participant_id,
                effective_bandwidth_bps) != 0) {
            return -1;
        }
        stats.receiver_bandwidths_replayed++;
    }

    for (i = 0; i < room_summary.published_track_count; ++i) {
        turbo_room_track_summary_t track_summary;

        if (turbo_room_service_get_track_summary_at(service, room_id, i, &track_summary) != 0) {
            return -1;
        }

        if (track_summary.main_ssrc == 0) {
            stats.skipped_items++;
            continue;
        }

        if (room_service_app_server_sync_register_track(server, room_id, &track_summary) != 0) {
            return -1;
        }
        stats.tracks_replayed++;
    }

    for (i = 0; i < room_summary.subscription_count; ++i) {
        turbo_room_subscription_summary_t subscription_summary;
        turbo_room_track_summary_t track_summary;

        if (turbo_room_service_get_subscription_summary_at(service, room_id, i,
                                                           &subscription_summary) != 0) {
            return -1;
        }

        if (turbo_room_service_get_track_summary(service, room_id, subscription_summary.track_id,
                                                 &track_summary) != 0 ||
            track_summary.main_ssrc == 0) {
            stats.skipped_items++;
            continue;
        }

        if (room_service_app_server_sync_apply_track_subscription(
                server, room_id, &subscription_summary) != 0) {
            return -1;
        }
        stats.subscriptions_replayed++;
    }

    if (room_summary.recording_state == TURBO_ROOM_RECORDING_ACTIVE) {
        int recording_started = 0;

        if (room_summary.recording_id[0] == '\0' ||
            room_summary.recording_mode[0] == '\0' ||
            stats.tracks_replayed == 0) {
            stats.skipped_items++;
        } else if (room_service_app_server_sync_ensure_recording(
                       server, room_id, room_summary.recording_id,
                       room_summary.recording_mode, &recording_started) != 0) {
            return -1;
        } else if (recording_started) {
            stats.recordings_replayed++;
        }
    }

    if (out_stats) {
        *out_stats = stats;
    }
    return 0;
}

int room_service_app_server_sync_resync_room(
    room_service_app_server_t *server, const char *room_id,
    room_service_sfu_replay_stats_t *out_stats) {
    turbo_room_service_t *service;
    turbo_room_summary_t room_summary;

    if (out_stats) {
        memset(out_stats, 0, sizeof(*out_stats));
    }

    if (!server || !room_id) {
        return -1;
    }

    service = room_service_app_server_get_service(server);
    if (!service ||
        turbo_room_service_get_room_summary(service, room_id, &room_summary) != 0 ||
        room_summary.assigned_sfu_node[0] == '\0') {
        return -1;
    }

    if (room_service_app_server_sync_force_close_room(server, room_id) != 0) {
        return -1;
    }

    return room_service_app_server_sync_replay_room_state(server, room_id,
                                                          out_stats);
}

int room_service_app_server_fetch_track_subscription(
    room_service_app_server_t *server, const char *room_id,
    const char *receiver_participant_id, const char *track_id,
    room_service_sfu_track_subscription_t *subscription) {
    http_client_t *client;
    http_response_t *response;
    json_value_t *root = NULL;
    json_value_t *track_subscription;
    char command_json[512];
    char control_url[ROOM_SERVICE_SFU_CONTROL_URL_MAX];
    char control_token[ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX];

    if (!subscription) {
        return -1;
    }

    memset(subscription, 0, sizeof(*subscription));
    if (!server || !room_id || !receiver_participant_id || !track_id) {
        return 0;
    }

    if (room_service_copy_sfu_route_for_room(server, room_id, control_url,
                                             sizeof(control_url), control_token,
                                             sizeof(control_token)) != 0) {
        return -1;
    }
    if (control_url[0] == '\0') {
        return 0;
    }

    client = room_service_create_sfu_client_for_route(
        server, control_url, control_token);
    if (!client) {
        return -1;
    }

    snprintf(command_json, sizeof(command_json),
             "{"
             "\"type\":\"get_track_subscription\","
             "\"room_id\":\"%s\","
             "\"receiver_participant_id\":\"%s\","
             "\"track_id\":\"%s\""
             "}",
             room_id, receiver_participant_id, track_id);

    response = http_post_json(client, "/api/v1/commands", command_json);
    if (!response) {
        http_client_destroy(client);
        return -1;
    }

    if (response->error_code != HTTP_ERROR_NONE) {
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    if (response->status_code == 404) {
        subscription->available = 1;
        http_response_free(response);
        http_client_destroy(client);
        return 0;
    }

    if (response->status_code < 200 || response->status_code >= 300 ||
        !http_response_is_json(response)) {
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    root = http_response_parse_json(response);
    if (!root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
        turbo_free_json(&root);
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    track_subscription = turbo_json_object_get(root, "track_subscription");
    if (!track_subscription || turbo_json_type(track_subscription) != TURBO_JSON_OBJECT) {
        turbo_free_json(&root);
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    subscription->available = 1;
    subscription->found = 1;
    subscription->enabled =
        room_service_json_bool_field(track_subscription, "enabled", 0);
    subscription->priority = turbo_json_get_int(track_subscription, "priority", 0);
    subscription->preferred_layer = room_service_parse_video_layer(
        room_service_json_string_field(track_subscription, "preferred_layer"));
    subscription->target_layer = room_service_parse_video_layer(
        room_service_json_string_field(track_subscription, "target_layer"));
    subscription->muted =
        room_service_json_bool_field(track_subscription, "muted", 0);
    subscription->max_layer = room_service_parse_video_layer(
        room_service_json_string_field(track_subscription, "max_layer"));

    if (room_service_json_string_field(track_subscription, "receiver_participant_id")) {
        strncpy(subscription->receiver_participant_id,
                room_service_json_string_field(track_subscription,
                                               "receiver_participant_id"),
                sizeof(subscription->receiver_participant_id) - 1);
    }
    if (room_service_json_string_field(track_subscription, "sender_participant_id")) {
        strncpy(subscription->sender_participant_id,
                room_service_json_string_field(track_subscription,
                                               "sender_participant_id"),
                sizeof(subscription->sender_participant_id) - 1);
    }
    if (room_service_json_string_field(track_subscription, "track_id")) {
        strncpy(subscription->track_id,
                room_service_json_string_field(track_subscription, "track_id"),
                sizeof(subscription->track_id) - 1);
    }
    if (room_service_json_string_field(track_subscription, "policy_source")) {
        strncpy(subscription->policy_source,
                room_service_json_string_field(track_subscription, "policy_source"),
                sizeof(subscription->policy_source) - 1);
    }

    turbo_free_json(&root);
    http_response_free(response);
    http_client_destroy(client);
    return 0;
}

int room_service_app_server_fetch_participant_stats(
    room_service_app_server_t *server, const char *room_id, const char *participant_id,
    room_service_sfu_participant_stats_t *stats) {
    http_client_t *client;
    http_response_t *response;
    json_value_t *root = NULL;
    json_value_t *participant_stats;
    char command_json[384];
    char control_url[ROOM_SERVICE_SFU_CONTROL_URL_MAX];
    char control_token[ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX];

    if (!stats) {
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    if (!server || !room_id || !participant_id) {
        return 0;
    }

    if (room_service_copy_sfu_route_for_room(server, room_id, control_url,
                                             sizeof(control_url), control_token,
                                             sizeof(control_token)) != 0) {
        return -1;
    }
    if (control_url[0] == '\0') {
        return 0;
    }

    client = room_service_create_sfu_client_for_route(
        server, control_url, control_token);
    if (!client) {
        return -1;
    }

    snprintf(command_json, sizeof(command_json),
             "{"
             "\"type\":\"get_participant_stats\","
             "\"room_id\":\"%s\","
             "\"participant_id\":\"%s\""
             "}",
             room_id, participant_id);

    response = http_post_json(client, "/api/v1/commands", command_json);
    if (!response) {
        http_client_destroy(client);
        return -1;
    }

    if (response->error_code != HTTP_ERROR_NONE) {
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    if (response->status_code == 404) {
        stats->available = 1;
        http_response_free(response);
        http_client_destroy(client);
        return 0;
    }

    if (response->status_code < 200 || response->status_code >= 300 ||
        !http_response_is_json(response)) {
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    root = http_response_parse_json(response);
    if (!root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
        turbo_free_json(&root);
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    participant_stats = turbo_json_object_get(root, "participant_stats");
    if (!participant_stats || turbo_json_type(participant_stats) != TURBO_JSON_OBJECT) {
        turbo_free_json(&root);
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    stats->available = 1;
    stats->found = 1;
    if (room_service_json_string_field(participant_stats, "participant_id")) {
        strncpy(stats->participant_id,
                room_service_json_string_field(participant_stats, "participant_id"),
                sizeof(stats->participant_id) - 1);
    }
    stats->stream_count = turbo_json_get_int(participant_stats, "stream_count", 0);
    stats->available_bandwidth =
        turbo_json_get_int(participant_stats, "available_bandwidth", 0);
    stats->packets_sent =
        room_service_json_int64_field(participant_stats, "packets_sent", 0);
    stats->bytes_sent =
        room_service_json_int64_field(participant_stats, "bytes_sent", 0);
    stats->packets_received =
        room_service_json_int64_field(participant_stats, "packets_received", 0);
    stats->bytes_received =
        room_service_json_int64_field(participant_stats, "bytes_received", 0);

    turbo_free_json(&root);
    http_response_free(response);
    http_client_destroy(client);
    return 0;
}

int room_service_app_server_fetch_recording_status(
    room_service_app_server_t *server, const char *room_id,
    room_service_sfu_recording_status_t *status) {
    http_client_t *client;
    http_response_t *response;
    json_value_t *root = NULL;
    json_value_t *recording_status;
    char command_json[256];
    char control_url[ROOM_SERVICE_SFU_CONTROL_URL_MAX];
    char control_token[ROOM_SERVICE_SFU_CONTROL_TOKEN_MAX];

    if (!status) {
        return -1;
    }

    memset(status, 0, sizeof(*status));
    if (!server || !room_id) {
        return 0;
    }

    if (room_service_copy_sfu_route_for_room(server, room_id, control_url,
                                             sizeof(control_url), control_token,
                                             sizeof(control_token)) != 0) {
        return -1;
    }
    if (control_url[0] == '\0') {
        return 0;
    }

    client = room_service_create_sfu_client_for_route(
        server, control_url, control_token);
    if (!client) {
        return -1;
    }

    snprintf(command_json, sizeof(command_json),
             "{"
             "\"type\":\"get_recording_status\","
             "\"room_id\":\"%s\""
             "}",
             room_id);

    response = http_post_json(client, "/api/v1/commands", command_json);
    if (!response) {
        http_client_destroy(client);
        return -1;
    }

    if (response->error_code != HTTP_ERROR_NONE) {
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    if (response->status_code == 404) {
        status->available = 1;
        http_response_free(response);
        http_client_destroy(client);
        return 0;
    }

    if (response->status_code < 200 || response->status_code >= 300 ||
        !http_response_is_json(response)) {
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    root = http_response_parse_json(response);
    if (!root || turbo_json_type(root) != TURBO_JSON_OBJECT) {
        turbo_free_json(&root);
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    recording_status = turbo_json_object_get(root, "recording_status");
    if (!recording_status || turbo_json_type(recording_status) != TURBO_JSON_OBJECT) {
        turbo_free_json(&root);
        http_response_free(response);
        http_client_destroy(client);
        return -1;
    }

    status->available = 1;
    status->found = 1;
    status->active = room_service_json_bool_field(recording_status, "active", 0);
    status->track_count = turbo_json_get_int(recording_status, "track_count", 0);
    room_service_copy_string(status->room_id, sizeof(status->room_id),
                             room_service_json_string_field(recording_status, "room_id"));
    room_service_copy_string(
        status->recording_id, sizeof(status->recording_id),
        room_service_json_string_field(recording_status, "recording_id"));
    room_service_copy_string(status->mode, sizeof(status->mode),
                             room_service_json_string_field(recording_status, "mode"));

    turbo_free_json(&root);
    http_response_free(response);
    http_client_destroy(client);
    return 0;
}

int room_service_app_server_record_room_sync(
    room_service_app_server_t *server, const char *room_id, const char *operation,
    const room_service_sfu_replay_stats_t *stats, const char *warning_code,
    const char *warning_message) {
    room_service_room_sync_diagnostic_t *diagnostic;
    int rc = -1;

    if (!server || !room_id || !operation || !stats) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    diagnostic = room_service_find_room_sync_diagnostic(server, room_id);
    if (!diagnostic) {
        if (room_service_ensure_capacity((void **)&server->room_sync_diagnostics,
                                         &server->room_sync_diagnostic_capacity,
                                         sizeof(*server->room_sync_diagnostics),
                                         server->room_sync_diagnostic_count + 1) != 0) {
            goto out;
        }

        diagnostic = &server->room_sync_diagnostics[server->room_sync_diagnostic_count++];
        memset(diagnostic, 0, sizeof(*diagnostic));
    }

    diagnostic->available = 1;
    room_service_copy_string(diagnostic->room_id, sizeof(diagnostic->room_id), room_id);
    room_service_copy_string(diagnostic->operation, sizeof(diagnostic->operation), operation);
    diagnostic->replay_stats = *stats;
    diagnostic->had_warning = (warning_code && warning_message) ? 1 : 0;
    room_service_copy_string(diagnostic->warning_code, sizeof(diagnostic->warning_code),
                             warning_code);
    room_service_copy_string(diagnostic->warning_message,
                             sizeof(diagnostic->warning_message), warning_message);
    diagnostic->sequence = ++server->room_sync_sequence;
    rc = 0;

out:
    turbo_mutex_unlock(&server->mutex);
    return rc;
}

int room_service_app_server_get_room_sync_diagnostic(
    room_service_app_server_t *server, const char *room_id,
    room_service_room_sync_diagnostic_t *diagnostic) {
    room_service_room_sync_diagnostic_t *stored;

    if (!server || !room_id || !diagnostic) {
        return -1;
    }

    memset(diagnostic, 0, sizeof(*diagnostic));
    turbo_mutex_lock(&server->mutex);
    stored = room_service_find_room_sync_diagnostic(server, room_id);
    if (!stored) {
        turbo_mutex_unlock(&server->mutex);
        return -1;
    }

    *diagnostic = *stored;
    turbo_mutex_unlock(&server->mutex);
    return 0;
}

int room_service_app_server_record_call_center_event(
    room_service_app_server_t *server, const char *room_id, const char *event_type,
    const char *participant_id, const char *peer_participant_id, const char *state,
    const char *detail) {
    room_service_call_center_event_t *event;
    int rc = -1;

    if (!server || !room_id || !room_id[0] || !event_type || !event_type[0]) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    if (room_service_ensure_capacity((void **)&server->call_center_events,
                                     &server->call_center_event_capacity,
                                     sizeof(*server->call_center_events),
                                     server->call_center_event_count + 1) != 0) {
        goto out;
    }

    event = &server->call_center_events[server->call_center_event_count++];
    memset(event, 0, sizeof(*event));
    event->available = 1;
    room_service_copy_string(event->room_id, sizeof(event->room_id), room_id);
    room_service_copy_string(event->event_type, sizeof(event->event_type), event_type);
    room_service_copy_string(event->participant_id, sizeof(event->participant_id),
                             participant_id);
    room_service_copy_string(event->peer_participant_id,
                             sizeof(event->peer_participant_id), peer_participant_id);
    room_service_copy_string(event->state, sizeof(event->state), state);
    room_service_copy_string(event->detail, sizeof(event->detail), detail);
    event->sequence = ++server->call_center_event_sequence;
    room_service_prune_call_center_events_locked(server);
    rc = 0;

out:
    turbo_mutex_unlock(&server->mutex);
    return rc;
}

int room_service_app_server_list_call_center_events(
    room_service_app_server_t *server, const char *room_id, int64_t after_sequence,
    int limit, room_service_call_center_event_t *events, int event_capacity,
    int *out_count, int64_t *out_latest_sequence) {
    int count = 0;
    int i;

    if (out_count) {
        *out_count = 0;
    }
    if (out_latest_sequence) {
        *out_latest_sequence = 0;
    }
    if (!server || !room_id || !events || event_capacity <= 0 || limit < 0) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    if (limit == 0 || limit > event_capacity) {
        limit = event_capacity;
    }

    for (i = 0; i < server->call_center_event_count; ++i) {
        room_service_call_center_event_t *event = &server->call_center_events[i];

        if (!event->available || strcmp(event->room_id, room_id) != 0) {
            continue;
        }
        if (event->sequence > after_sequence) {
            if (count < limit) {
                events[count++] = *event;
            }
        }
        if (out_latest_sequence && event->sequence > *out_latest_sequence) {
            *out_latest_sequence = event->sequence;
        }
    }

    turbo_mutex_unlock(&server->mutex);

    if (out_count) {
        *out_count = count;
    }
    return 0;
}

int room_service_app_server_set_conference_layout_mode(
    room_service_app_server_t *server, const char *room_id, const char *layout_mode) {
    room_service_conference_policy_t *policy;
    const char *normalized_layout;
    int rc = -1;

    if (!server || !room_id || !room_service_layout_mode_is_valid(layout_mode) ||
        !room_service_room_exists(server, room_id)) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    policy = room_service_get_or_create_conference_policy_locked(server, room_id);
    if (!policy) {
        goto out;
    }

    normalized_layout = room_service_normalize_layout_mode(layout_mode);
    if (strcmp(policy->layout_mode, normalized_layout) == 0) {
        rc = 0;
        goto out;
    }

    if (turbo_room_service_set_layout_mode(server->service, room_id,
                                           room_service_layout_mode_to_core(
                                               normalized_layout)) != 0) {
        goto out;
    }

    room_service_copy_string(policy->layout_mode, sizeof(policy->layout_mode),
                             normalized_layout);
    policy->version = ++server->conference_policy_sequence;
    rc = 0;

out:
    turbo_mutex_unlock(&server->mutex);
    return rc;
}

int room_service_app_server_set_conference_active_speaker(
    room_service_app_server_t *server, const char *room_id, const char *participant_id) {
    room_service_conference_policy_t *policy;
    const char *normalized_participant_id = participant_id ? participant_id : "";
    int rc = -1;

    if (!server || !room_id || !room_service_room_exists(server, room_id)) {
        return -1;
    }

    if (normalized_participant_id[0] != '\0' &&
        !room_service_participant_exists(server, room_id, normalized_participant_id)) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    policy = room_service_get_or_create_conference_policy_locked(server, room_id);
    if (!policy) {
        goto out;
    }

    if (strcmp(policy->active_speaker_participant_id, normalized_participant_id) == 0) {
        rc = 0;
        goto out;
    }

    if (turbo_room_service_set_active_speaker(
            server->service, room_id,
            normalized_participant_id[0] != '\0' ? normalized_participant_id : NULL) != 0) {
        goto out;
    }

    room_service_copy_string(policy->active_speaker_participant_id,
                             sizeof(policy->active_speaker_participant_id),
                             normalized_participant_id);
    policy->version = ++server->conference_policy_sequence;
    rc = 0;

out:
    turbo_mutex_unlock(&server->mutex);
    return rc;
}

int room_service_app_server_set_conference_pin(room_service_app_server_t *server,
                                               const char *room_id,
                                               const char *participant_id) {
    room_service_conference_policy_t *policy;
    const char *normalized_participant_id = participant_id ? participant_id : "";
    int rc = -1;

    if (!server || !room_id || !room_service_room_exists(server, room_id)) {
        return -1;
    }

    if (normalized_participant_id[0] != '\0' &&
        !room_service_participant_exists(server, room_id, normalized_participant_id)) {
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    policy = room_service_get_or_create_conference_policy_locked(server, room_id);
    if (!policy) {
        goto out;
    }

    if (strcmp(policy->pinned_participant_id, normalized_participant_id) == 0) {
        rc = 0;
        goto out;
    }

    if (turbo_room_service_pin_participant(
            server->service, room_id,
            normalized_participant_id[0] != '\0' ? normalized_participant_id : NULL) != 0) {
        goto out;
    }

    room_service_copy_string(policy->pinned_participant_id,
                             sizeof(policy->pinned_participant_id),
                             normalized_participant_id);
    policy->version = ++server->conference_policy_sequence;
    rc = 0;

out:
    turbo_mutex_unlock(&server->mutex);
    return rc;
}

int room_service_app_server_get_conference_policy(
    room_service_app_server_t *server, const char *room_id,
    room_service_conference_policy_t *policy) {
    if (!policy) {
        return -1;
    }

    return room_service_fill_conference_policy(server, room_id, policy);
}

int room_service_app_server_apply_conference_policy(
    room_service_app_server_t *server, const char *room_id,
    room_service_conference_policy_apply_result_t *result) {
    turbo_room_summary_t room_summary;
    room_service_conference_policy_t policy;
    room_service_policy_subscription_t *desired_subscriptions = NULL;
    room_service_policy_subscription_t *stale_subscriptions = NULL;
    int desired_count = 0;
    int desired_capacity = 0;
    int stale_count = 0;
    int stale_capacity = 0;
    int i;

    if (result) {
        memset(result, 0, sizeof(*result));
    }

    if (!server || !server->service || !room_id ||
        turbo_room_service_get_room_summary(server->service, room_id, &room_summary) != 0 ||
        room_service_fill_conference_policy(server, room_id, &policy) != 0) {
        return -1;
    }

    for (i = 0; i < room_summary.participant_count; ++i) {
        turbo_room_participant_summary_t participant_summary;
        int j;

        if (turbo_room_service_get_participant_summary_at(server->service, room_id, i,
                                                          &participant_summary) != 0) {
            free(desired_subscriptions);
            return -1;
        }

        for (j = 0; j < room_summary.published_track_count; ++j) {
            turbo_room_track_summary_t track_summary;
            room_service_policy_subscription_t desired;

            if (turbo_room_service_get_track_summary_at(server->service, room_id, j,
                                                        &track_summary) != 0) {
                free(desired_subscriptions);
                return -1;
            }

            if (strcmp(participant_summary.participant_id,
                       track_summary.owner_participant_id) == 0) {
                continue;
            }

            room_service_build_policy_subscription(&policy, &track_summary,
                                                   participant_summary.participant_id,
                                                   &desired);
            if (room_service_ensure_capacity((void **)&desired_subscriptions, &desired_capacity,
                                             sizeof(*desired_subscriptions),
                                             desired_count + 1) != 0) {
                free(desired_subscriptions);
                return -1;
            }
            desired_subscriptions[desired_count++] = desired;
        }
    }

    for (i = 0; i < desired_count; ++i) {
        turbo_room_subscription_summary_t existing_summary;
        turbo_room_subscription_config_t config;
        const char *warning_code = NULL;
        const char *warning_message = NULL;
        int already_matches = 0;

        memset(&config, 0, sizeof(config));
        if (turbo_room_service_get_subscription_summary(server->service, room_id,
                                                        desired_subscriptions[i]
                                                            .subscriber_participant_id,
                                                        desired_subscriptions[i].track_id,
                                                        &existing_summary) == 0) {
            already_matches = room_service_desired_subscription_matches(
                &existing_summary, &desired_subscriptions[i]);
        }

        if (already_matches) {
            continue;
        }

        config.subscriber_participant_id =
            desired_subscriptions[i].subscriber_participant_id;
        config.track_id = desired_subscriptions[i].track_id;
        config.enabled = desired_subscriptions[i].enabled;
        config.priority = desired_subscriptions[i].priority;
        config.preferred_layer = desired_subscriptions[i].preferred_layer;
        config.target_layer = desired_subscriptions[i].target_layer;
        config.muted = desired_subscriptions[i].muted;
        config.policy_source = desired_subscriptions[i].policy_source;

        if (turbo_room_service_set_subscription(server->service, room_id, &config) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        if (result) {
            result->subscriptions_applied++;
        }

        if (room_summary.assigned_sfu_node[0] != '\0') {
            turbo_room_subscription_summary_t applied_summary;
            if (turbo_room_service_get_subscription_summary(
                    server->service, room_id, config.subscriber_participant_id,
                    config.track_id, &applied_summary) != 0 ||
                room_service_app_server_sync_apply_track_subscription(
                    server, room_id, &applied_summary) != 0) {
                warning_code = "SFU_SYNC_FAILED";
                warning_message =
                    "conference policy committed locally; set_track_subscription was not forwarded to sfu node";
            }
            room_service_capture_policy_warning(result, warning_code, warning_message);
        }
    }

    if (turbo_room_service_get_room_summary(server->service, room_id, &room_summary) != 0) {
        free(stale_subscriptions);
        free(desired_subscriptions);
        return -1;
    }

    for (i = 0; i < room_summary.subscription_count; ++i) {
        turbo_room_subscription_summary_t subscription_summary;

        if (turbo_room_service_get_subscription_summary_at(server->service, room_id, i,
                                                           &subscription_summary) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        if (!room_service_policy_has_managed_source(subscription_summary.policy_source) ||
            room_service_policy_has_desired_subscription(
                desired_subscriptions, desired_count,
                subscription_summary.subscriber_participant_id,
                subscription_summary.track_id)) {
            continue;
        }

        if (room_service_ensure_capacity((void **)&stale_subscriptions, &stale_capacity,
                                         sizeof(*stale_subscriptions),
                                         stale_count + 1) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        memset(&stale_subscriptions[stale_count], 0, sizeof(stale_subscriptions[stale_count]));
        room_service_copy_string(stale_subscriptions[stale_count].subscriber_participant_id,
                                 sizeof(stale_subscriptions[stale_count]
                                            .subscriber_participant_id),
                                 subscription_summary.subscriber_participant_id);
        room_service_copy_string(stale_subscriptions[stale_count].track_id,
                                 sizeof(stale_subscriptions[stale_count].track_id),
                                 subscription_summary.track_id);
        stale_count++;
    }

    for (i = 0; i < stale_count; ++i) {
        if (turbo_room_service_remove_subscription(
                server->service, room_id,
                stale_subscriptions[i].subscriber_participant_id,
                stale_subscriptions[i].track_id) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        if (result) {
            result->subscriptions_removed++;
        }

        if (room_summary.assigned_sfu_node[0] != '\0') {
            turbo_room_subscription_summary_t disabled_summary;

            memset(&disabled_summary, 0, sizeof(disabled_summary));
            room_service_copy_string(disabled_summary.subscriber_participant_id,
                                     sizeof(disabled_summary.subscriber_participant_id),
                                     stale_subscriptions[i].subscriber_participant_id);
            room_service_copy_string(disabled_summary.track_id,
                                     sizeof(disabled_summary.track_id),
                                     stale_subscriptions[i].track_id);
            disabled_summary.enabled = 0;
            disabled_summary.muted = 1;
            disabled_summary.preferred_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
            disabled_summary.target_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
            if (room_service_app_server_sync_apply_track_subscription(
                    server, room_id, &disabled_summary) != 0) {
                room_service_capture_policy_warning(
                    result, "SFU_SYNC_FAILED",
                    "conference policy committed locally; track unsubscribe was not forwarded to sfu node");
            }
        }
    }

    for (i = 0; i < room_summary.participant_count; ++i) {
        turbo_room_participant_summary_t participant_summary;
        int effective_bandwidth_bps = 0;
        int is_explicit_bandwidth = 0;

        if (turbo_room_service_get_participant_summary_at(server->service, room_id, i,
                                                          &participant_summary) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        if (turbo_room_service_get_effective_receiver_bandwidth(
                server->service, room_id, participant_summary.participant_id,
                &effective_bandwidth_bps, &is_explicit_bandwidth) != 0) {
            free(stale_subscriptions);
            free(desired_subscriptions);
            return -1;
        }

        if (result) {
            result->receiver_bandwidths_reconciled++;
        }

        if (room_summary.assigned_sfu_node[0] != '\0' && !is_explicit_bandwidth &&
            room_service_app_server_sync_set_receiver_bandwidth(
                server, room_id, participant_summary.participant_id,
                effective_bandwidth_bps) != 0) {
            room_service_capture_policy_warning(
                result, "SFU_SYNC_FAILED",
                "conference policy committed locally; derived receiver bandwidth was not forwarded to sfu node");
        }
    }

    free(stale_subscriptions);
    free(desired_subscriptions);
    return 0;
}

int room_service_app_server_apply_call_center_policy(
    room_service_app_server_t *server, const char *room_id,
    turbo_call_center_supervisor_mode_t supervisor_mode,
    room_service_conference_policy_apply_result_t *result) {
    turbo_room_summary_t room_summary;
    room_service_conference_policy_t *policy;
    room_service_policy_subscription_t *before_subscriptions = NULL;
    room_service_policy_subscription_t *after_subscriptions = NULL;
    int before_count = 0;
    int after_count = 0;
    int i;

    if (result) {
        memset(result, 0, sizeof(*result));
    }

    if (!server || !server->service || !room_id ||
        turbo_room_service_get_room_summary(server->service, room_id, &room_summary) != 0) {
        return -1;
    }

    if (room_service_capture_policy_subscriptions(
            server->service, room_id, room_service_policy_has_call_center_source,
            &before_subscriptions, &before_count) != 0) {
        return -1;
    }

    if (turbo_room_service_reconcile_call_center_subscriptions(
            server->service, room_id, supervisor_mode) != 0) {
        free(before_subscriptions);
        return -1;
    }

    turbo_mutex_lock(&server->mutex);
    policy = room_service_get_or_create_conference_policy_locked(server, room_id);
    if (!policy) {
        turbo_mutex_unlock(&server->mutex);
        free(before_subscriptions);
        return -1;
    }
    policy->supervisor_mode = supervisor_mode;
    policy->version = ++server->conference_policy_sequence;
    turbo_mutex_unlock(&server->mutex);

    if (room_service_capture_policy_subscriptions(
            server->service, room_id, room_service_policy_has_call_center_source,
            &after_subscriptions, &after_count) != 0) {
        free(before_subscriptions);
        return -1;
    }

    for (i = 0; i < after_count; ++i) {
        turbo_room_subscription_summary_t subscription_summary;

        if (turbo_room_service_get_subscription_summary(
                server->service, room_id,
                after_subscriptions[i].subscriber_participant_id,
                after_subscriptions[i].track_id, &subscription_summary) != 0) {
            free(after_subscriptions);
            free(before_subscriptions);
            return -1;
        }

        if (result) {
            result->subscriptions_applied++;
        }

        if (room_summary.assigned_sfu_node[0] != '\0' &&
            room_service_app_server_sync_apply_track_subscription(
                server, room_id, &subscription_summary) != 0) {
            room_service_capture_policy_warning(
                result, "SFU_SYNC_FAILED",
                "call-center policy committed locally; set_track_subscription was not forwarded to sfu node");
        }
    }

    for (i = 0; i < before_count; ++i) {
        turbo_room_subscription_summary_t disabled_summary;

        if (room_service_policy_has_desired_subscription(
                after_subscriptions, after_count,
                before_subscriptions[i].subscriber_participant_id,
                before_subscriptions[i].track_id)) {
            continue;
        }

        if (result) {
            result->subscriptions_removed++;
        }

        if (room_summary.assigned_sfu_node[0] == '\0') {
            continue;
        }

        memset(&disabled_summary, 0, sizeof(disabled_summary));
        room_service_copy_string(disabled_summary.subscriber_participant_id,
                                 sizeof(disabled_summary.subscriber_participant_id),
                                 before_subscriptions[i].subscriber_participant_id);
        room_service_copy_string(disabled_summary.track_id,
                                 sizeof(disabled_summary.track_id),
                                 before_subscriptions[i].track_id);
        disabled_summary.enabled = 0;
        disabled_summary.muted = 1;
        disabled_summary.preferred_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
        disabled_summary.target_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
        room_service_copy_string(disabled_summary.policy_source,
                                 sizeof(disabled_summary.policy_source),
                                 before_subscriptions[i].policy_source);
        if (room_service_app_server_sync_apply_track_subscription(
                server, room_id, &disabled_summary) != 0) {
            room_service_capture_policy_warning(
                result, "SFU_SYNC_FAILED",
                "call-center policy committed locally; track unsubscribe was not forwarded to sfu node");
        }
    }

    if (turbo_room_service_get_room_summary(server->service, room_id, &room_summary) != 0) {
        free(after_subscriptions);
        free(before_subscriptions);
        return -1;
    }

    for (i = 0; i < room_summary.participant_count; ++i) {
        turbo_room_participant_summary_t participant_summary;
        int effective_bandwidth_bps = 0;
        int is_explicit_bandwidth = 0;

        if (turbo_room_service_get_participant_summary_at(server->service, room_id, i,
                                                          &participant_summary) != 0 ||
            turbo_room_service_get_effective_receiver_bandwidth(
                server->service, room_id, participant_summary.participant_id,
                &effective_bandwidth_bps, &is_explicit_bandwidth) != 0) {
            free(after_subscriptions);
            free(before_subscriptions);
            return -1;
        }

        if (result) {
            result->receiver_bandwidths_reconciled++;
        }

        if (room_summary.assigned_sfu_node[0] != '\0' && !is_explicit_bandwidth &&
            room_service_app_server_sync_set_receiver_bandwidth(
                server, room_id, participant_summary.participant_id,
                effective_bandwidth_bps) != 0) {
            room_service_capture_policy_warning(
                result, "SFU_SYNC_FAILED",
                "call-center policy committed locally; derived receiver bandwidth was not forwarded to sfu node");
        }
    }

    free(after_subscriptions);
    free(before_subscriptions);
    return 0;
}
