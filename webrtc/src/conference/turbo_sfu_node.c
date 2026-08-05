#include "turbo_sfu_node.h"
#include "turbo_sfu.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    char session_id[TURBO_PARTICIPANT_ID_MAX];
    char participant_id[TURBO_PARTICIPANT_ID_MAX];
    turbo_peer_connection_t *pc;
} node_session_t;

typedef struct {
    char track_id[TURBO_TRACK_ID_MAX];
    char participant_id[TURBO_PARTICIPANT_ID_MAX];
    uint32_t main_ssrc;
} node_track_t;

typedef struct {
    char receiver_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char sender_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char track_id[TURBO_TRACK_ID_MAX];
    int enabled;
    int priority;
    turbo_room_video_layer_t preferred_layer;
    turbo_room_video_layer_t target_layer;
    int muted;
    char policy_source[TURBO_POLICY_SOURCE_MAX];
    turbo_room_video_layer_t max_layer;
} node_track_subscription_t;

typedef struct {
    char room_id[TURBO_ROOM_ID_MAX];
    sfu_context_t *sfu;
    int max_participants;
    node_session_t *sessions;
    int session_count;
    int session_capacity;
    node_track_t *tracks;
    int track_count;
    int track_capacity;

    node_track_subscription_t *subscriptions;
    int subscription_count;
    int subscription_capacity;
} node_room_t;

struct turbo_sfu_node_s {
    char node_id[TURBO_NODE_ID_MAX];
    int max_rooms;
    int default_room_capacity;
    node_room_t *rooms;
    int room_count;
    int room_capacity;
};

static void copy_string(char *dest, size_t dest_size, const char *src) {
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

static int identifier_fits(const char *value, size_t capacity) {
    size_t length = 0;

    if (!value || value[0] == '\0' || capacity < 2) {
        return 0;
    }
    while (length < capacity && value[length] != '\0') {
        length++;
    }
    return length > 0 && length < capacity;
}

static turbo_room_video_layer_t resolve_max_layer_from_subscription(
    const turbo_sfu_node_track_subscription_t *subscription) {
    if (!subscription || !subscription->enabled || subscription->muted) {
        return TURBO_ROOM_VIDEO_LAYER_NONE;
    }
    if (subscription->target_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
        return subscription->target_layer;
    }
    if (subscription->preferred_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
        return subscription->preferred_layer;
    }
    return subscription->max_layer;
}

static int ensure_capacity(void **items, int *capacity, size_t item_size, int count_needed) {
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

static node_room_t *find_room(turbo_sfu_node_t *node, const char *room_id) {
    int i;

    if (!node || !room_id) {
        return NULL;
    }

    for (i = 0; i < node->room_count; ++i) {
        if (strcmp(node->rooms[i].room_id, room_id) == 0) {
            return &node->rooms[i];
        }
    }

    return NULL;
}

static node_session_t *find_session_by_id(node_room_t *room, const char *session_id) {
    int i;

    if (!room || !session_id) {
        return NULL;
    }

    for (i = 0; i < room->session_count; ++i) {
        if (strcmp(room->sessions[i].session_id, session_id) == 0) {
            return &room->sessions[i];
        }
    }

    return NULL;
}

static node_session_t *find_session_by_participant(node_room_t *room,
                                                   const char *participant_id) {
    int i;

    if (!room || !participant_id) {
        return NULL;
    }

    for (i = 0; i < room->session_count; ++i) {
        if (strcmp(room->sessions[i].participant_id, participant_id) == 0) {
            return &room->sessions[i];
        }
    }

    return NULL;
}

static node_track_t *find_track(node_room_t *room, const char *track_id) {
    int i;

    if (!room || !track_id) {
        return NULL;
    }

    for (i = 0; i < room->track_count; ++i) {
        if (strcmp(room->tracks[i].track_id, track_id) == 0) {
            return &room->tracks[i];
        }
    }

    return NULL;
}

static node_track_subscription_t *find_track_subscription(node_room_t *room,
                                                          const char *receiver_participant_id,
                                                          const char *track_id) {
    int i;

    if (!room || !receiver_participant_id || !track_id) {
        return NULL;
    }

    for (i = 0; i < room->subscription_count; ++i) {
        node_track_subscription_t *subscription = &room->subscriptions[i];
        if (strcmp(subscription->receiver_participant_id, receiver_participant_id) == 0 &&
            strcmp(subscription->track_id, track_id) == 0) {
            return subscription;
        }
    }

    return NULL;
}

static void remove_track_subscription_at(node_room_t *room, int index) {
    if (!room || index < 0 || index >= room->subscription_count) {
        return;
    }

    if (index + 1 < room->subscription_count) {
        memmove(&room->subscriptions[index], &room->subscriptions[index + 1],
                (size_t)(room->subscription_count - index - 1) *
                    sizeof(node_track_subscription_t));
    }
    room->subscription_count--;
}

turbo_sfu_node_t *turbo_sfu_node_create(const turbo_sfu_node_config_t *config) {
    if (config && config->node_id &&
        !identifier_fits(config->node_id, TURBO_NODE_ID_MAX)) {
        return NULL;
    }

    turbo_sfu_node_t *node = (turbo_sfu_node_t *)calloc(1, sizeof(turbo_sfu_node_t));
    if (!node) {
        return NULL;
    }

    if (config) {
        copy_string(node->node_id, sizeof(node->node_id), config->node_id);
        node->max_rooms = config->max_rooms > 0 ? config->max_rooms : 16;
        node->default_room_capacity =
            config->default_room_capacity > 0 ? config->default_room_capacity : 50;
    } else {
        node->max_rooms = 16;
        node->default_room_capacity = 50;
    }

    return node;
}

void turbo_sfu_node_destroy(turbo_sfu_node_t *node) {
    int i;

    if (!node) {
        return;
    }

    for (i = 0; i < node->room_count; ++i) {
        sfu_destroy(node->rooms[i].sfu);
        free(node->rooms[i].sessions);
        free(node->rooms[i].tracks);
        free(node->rooms[i].subscriptions);
    }
    free(node->rooms);
    free(node);
}

int turbo_sfu_node_attach_room(turbo_sfu_node_t *node, const char *room_id, int max_participants) {
    node_room_t *room;

    if (!node || !identifier_fits(room_id, TURBO_ROOM_ID_MAX) ||
        find_room(node, room_id)) {
        return -1;
    }
    if (node->room_count >= node->max_rooms) {
        return -1;
    }
    if (ensure_capacity((void **)&node->rooms, &node->room_capacity, sizeof(node_room_t),
                        node->room_count + 1) != 0) {
        return -1;
    }

    room = &node->rooms[node->room_count++];
    memset(room, 0, sizeof(*room));
    copy_string(room->room_id, sizeof(room->room_id), room_id);
    room->max_participants = max_participants > 0 ? max_participants : node->default_room_capacity;
    room->sfu = sfu_create(room->max_participants);
    if (!room->sfu) {
        node->room_count--;
        return -1;
    }

    return 0;
}

int turbo_sfu_node_detach_room(turbo_sfu_node_t *node, const char *room_id) {
    int i;

    if (!node || !room_id) {
        return -1;
    }

    for (i = 0; i < node->room_count; ++i) {
        if (strcmp(node->rooms[i].room_id, room_id) == 0) {
            node_room_t *room = &node->rooms[i];
            if (room->session_count > 0) {
                return -1;
            }
            sfu_destroy(room->sfu);
            free(room->sessions);
            free(room->tracks);
            free(room->subscriptions);
            if (i + 1 < node->room_count) {
                memmove(&node->rooms[i], &node->rooms[i + 1],
                        (size_t)(node->room_count - i - 1) * sizeof(node_room_t));
            }
            node->room_count--;
            return 0;
        }
    }

    return -1;
}

int turbo_sfu_node_force_close_room(turbo_sfu_node_t *node, const char *room_id) {
    int i;

    if (!node || !room_id) {
        return -1;
    }

    for (i = 0; i < node->room_count; ++i) {
        if (strcmp(node->rooms[i].room_id, room_id) == 0) {
            node_room_t *room = &node->rooms[i];
            sfu_destroy(room->sfu);
            free(room->sessions);
            free(room->tracks);
            free(room->subscriptions);
            if (i + 1 < node->room_count) {
                memmove(&node->rooms[i], &node->rooms[i + 1],
                        (size_t)(node->room_count - i - 1) * sizeof(node_room_t));
            }
            node->room_count--;
            return 0;
        }
    }

    return -1;
}

int turbo_sfu_node_add_session(turbo_sfu_node_t *node, const char *room_id,
                               const char *participant_id, const char *session_id,
                               turbo_peer_connection_t *pc) {
    node_room_t *room = find_room(node, room_id);
    node_session_t *session;

    if (!room || !identifier_fits(participant_id, TURBO_PARTICIPANT_ID_MAX) ||
        !identifier_fits(session_id, TURBO_PARTICIPANT_ID_MAX)) {
        return -1;
    }
    if (find_session_by_id(room, session_id) || find_session_by_participant(room, participant_id)) {
        return -1;
    }
    if (ensure_capacity((void **)&room->sessions, &room->session_capacity,
                        sizeof(node_session_t), room->session_count + 1) != 0) {
        return -1;
    }
    if (sfu_add_participant(room->sfu, participant_id) != 0) {
        return -1;
    }

    session = &room->sessions[room->session_count++];
    memset(session, 0, sizeof(*session));
    copy_string(session->session_id, sizeof(session->session_id), session_id);
    copy_string(session->participant_id, sizeof(session->participant_id), participant_id);
    session->pc = pc;
    return 0;
}

int turbo_sfu_node_bind_session_pc(turbo_sfu_node_t *node, const char *room_id,
                                   const char *participant_id, const char *session_id,
                                   turbo_peer_connection_t *pc) {
    node_room_t *room = find_room(node, room_id);
    node_session_t *session;

    if (!room || !participant_id || !session_id) {
        return -1;
    }

    session = find_session_by_id(room, session_id);
    if (!session || strcmp(session->participant_id, participant_id) != 0) {
        return -1;
    }

    session->pc = pc;
    return 0;
}

int turbo_sfu_node_remove_session(turbo_sfu_node_t *node, const char *room_id,
                                  const char *session_id) {
    node_room_t *room = find_room(node, room_id);
    int i;

    if (!room || !session_id) {
        return -1;
    }

    for (i = 0; i < room->session_count; ++i) {
        node_session_t *session = &room->sessions[i];
        if (strcmp(session->session_id, session_id) == 0) {
            for (int j = room->subscription_count - 1; j >= 0; --j) {
                node_track_subscription_t *subscription = &room->subscriptions[j];
                if (strcmp(subscription->receiver_participant_id, session->participant_id) == 0 ||
                    strcmp(subscription->sender_participant_id, session->participant_id) == 0) {
                    remove_track_subscription_at(room, j);
                }
            }
            sfu_remove_participant(room->sfu, session->participant_id);
            if (i + 1 < room->session_count) {
                memmove(&room->sessions[i], &room->sessions[i + 1],
                        (size_t)(room->session_count - i - 1) * sizeof(node_session_t));
            }
            room->session_count--;
            return 0;
        }
    }

    return -1;
}

int turbo_sfu_node_register_published_track(turbo_sfu_node_t *node, const char *room_id,
                                            const char *participant_id, const char *track_id,
                                            uint32_t main_ssrc, const uint32_t *layer_ssrcs,
                                            int layer_count) {
    node_room_t *room = find_room(node, room_id);
    node_track_t *track;

    if (!room || !identifier_fits(participant_id, TURBO_PARTICIPANT_ID_MAX) ||
        !identifier_fits(track_id, TURBO_TRACK_ID_MAX) ||
        !find_session_by_participant(room, participant_id)) {
        return -1;
    }
    if (find_track(room, track_id)) {
        return -1;
    }
    if (ensure_capacity((void **)&room->tracks, &room->track_capacity, sizeof(node_track_t),
                        room->track_count + 1) != 0) {
        return -1;
    }
    if (sfu_add_stream(room->sfu, participant_id, main_ssrc, layer_ssrcs, layer_count) != 0) {
        return -1;
    }

    track = &room->tracks[room->track_count++];
    memset(track, 0, sizeof(*track));
    copy_string(track->track_id, sizeof(track->track_id), track_id);
    copy_string(track->participant_id, sizeof(track->participant_id), participant_id);
    track->main_ssrc = main_ssrc;
    return 0;
}

int turbo_sfu_node_unregister_published_track(turbo_sfu_node_t *node, const char *room_id,
                                              const char *track_id) {
    node_room_t *room = find_room(node, room_id);
    int i;

    if (!room || !track_id) {
        return -1;
    }

    for (i = 0; i < room->track_count; ++i) {
        node_track_t *track = &room->tracks[i];
        if (strcmp(track->track_id, track_id) != 0) {
            continue;
        }

        if (sfu_remove_stream(room->sfu, track->participant_id, track->main_ssrc) != 0) {
            return -1;
        }

        for (int j = room->subscription_count - 1; j >= 0; --j) {
            if (strcmp(room->subscriptions[j].track_id, track_id) == 0) {
                remove_track_subscription_at(room, j);
            }
        }

        if (i + 1 < room->track_count) {
            memmove(&room->tracks[i], &room->tracks[i + 1],
                    (size_t)(room->track_count - i - 1) * sizeof(node_track_t));
        }
        room->track_count--;
        return 0;
    }

    return -1;
}

int turbo_sfu_node_set_receiver_bandwidth(turbo_sfu_node_t *node, const char *room_id,
                                          const char *participant_id, int bandwidth_bps) {
    node_room_t *room = find_room(node, room_id);
    if (!room || !participant_id || !find_session_by_participant(room, participant_id)) {
        return -1;
    }

    sfu_set_participant_bandwidth(room->sfu, participant_id, bandwidth_bps);
    return 0;
}

int turbo_sfu_node_set_track_subscription(
    turbo_sfu_node_t *node, const char *room_id, const char *receiver_participant_id,
    const char *track_id, int enabled, turbo_room_video_layer_t max_layer) {
    turbo_sfu_node_track_subscription_t subscription;

    if (!identifier_fits(receiver_participant_id, TURBO_PARTICIPANT_ID_MAX) ||
        !identifier_fits(track_id, TURBO_TRACK_ID_MAX)) {
        return -1;
    }

    memset(&subscription, 0, sizeof(subscription));
    copy_string(subscription.receiver_participant_id,
                sizeof(subscription.receiver_participant_id),
                receiver_participant_id);
    copy_string(subscription.track_id, sizeof(subscription.track_id), track_id);
    subscription.enabled = enabled ? 1 : 0;
    subscription.preferred_layer = max_layer;
    subscription.target_layer = max_layer;
    subscription.max_layer = max_layer;
    subscription.muted = enabled ? 0 : 1;
    return turbo_sfu_node_apply_track_subscription(node, room_id, &subscription);
}

int turbo_sfu_node_apply_track_subscription(
    turbo_sfu_node_t *node, const char *room_id,
    const turbo_sfu_node_track_subscription_t *subscription_config) {
    node_room_t *room = find_room(node, room_id);
    node_track_t *track;
    node_track_subscription_t *subscription;
    turbo_room_video_layer_t max_layer;
    int sfu_max_layer;

    if (!room || !subscription_config ||
        !identifier_fits(subscription_config->receiver_participant_id,
                         sizeof(subscription_config->receiver_participant_id)) ||
        !identifier_fits(subscription_config->track_id,
                         sizeof(subscription_config->track_id)) ||
        !find_session_by_participant(room, subscription_config->receiver_participant_id)) {
        return -1;
    }

    track = find_track(room, subscription_config->track_id);
    if (!track) {
        return -1;
    }

    max_layer = resolve_max_layer_from_subscription(subscription_config);
    switch (max_layer) {
        case TURBO_ROOM_VIDEO_LAYER_NONE: sfu_max_layer = 0; break;
        case TURBO_ROOM_VIDEO_LAYER_LOW: sfu_max_layer = 0; break;
        case TURBO_ROOM_VIDEO_LAYER_MEDIUM: sfu_max_layer = 1; break;
        case TURBO_ROOM_VIDEO_LAYER_HIGH: sfu_max_layer = 2; break;
        default: return -1;
    }

    subscription = find_track_subscription(room, subscription_config->receiver_participant_id,
                                           subscription_config->track_id);
    if (!subscription) {
        if (ensure_capacity((void **)&room->subscriptions, &room->subscription_capacity,
                            sizeof(node_track_subscription_t),
                            room->subscription_count + 1) != 0) {
            return -1;
        }
    }

    if (sfu_set_receiver_stream_policy(
            room->sfu, subscription_config->receiver_participant_id,
            track->participant_id, track->main_ssrc,
            subscription_config->enabled && !subscription_config->muted,
            sfu_max_layer) != 0) {
        return -1;
    }

    if (!subscription) {
        subscription = &room->subscriptions[room->subscription_count++];
        memset(subscription, 0, sizeof(*subscription));
        copy_string(subscription->receiver_participant_id,
                    sizeof(subscription->receiver_participant_id),
                    subscription_config->receiver_participant_id);
        copy_string(subscription->track_id, sizeof(subscription->track_id),
                    subscription_config->track_id);
    }

    copy_string(subscription->sender_participant_id,
                sizeof(subscription->sender_participant_id), track->participant_id);
    subscription->enabled = subscription_config->enabled ? 1 : 0;
    subscription->priority = subscription_config->priority;
    subscription->preferred_layer = subscription_config->preferred_layer;
    subscription->target_layer = subscription_config->target_layer;
    subscription->muted = subscription_config->muted ? 1 : 0;
    copy_string(subscription->policy_source, sizeof(subscription->policy_source),
                subscription_config->policy_source);
    subscription->max_layer = max_layer;

    return 0;
}

int turbo_sfu_node_set_participant_packet_callback(
    turbo_sfu_node_t *node, const char *room_id, const char *participant_id,
    turbo_sfu_node_packet_cb on_packet, void *user_data) {
    node_room_t *room = find_room(node, room_id);

    if (!room || !participant_id || !find_session_by_participant(room, participant_id)) {
        return -1;
    }

    sfu_set_participant_callback(room->sfu, participant_id, on_packet, user_data);
    return 0;
}

int turbo_sfu_node_set_participant_keyframe_callback(
    turbo_sfu_node_t *node, const char *room_id, const char *participant_id,
    turbo_sfu_node_keyframe_request_cb on_keyframe_request, void *user_data) {
    node_room_t *room = find_room(node, room_id);

    if (!room || !participant_id || !find_session_by_participant(room, participant_id)) {
        return -1;
    }

    sfu_set_participant_keyframe_callback(room->sfu, participant_id,
                                          on_keyframe_request, user_data);
    return 0;
}

int turbo_sfu_node_forward_packet(turbo_sfu_node_t *node, const char *room_id,
                                  const char *sender_participant_id,
                                  const uint8_t *packet, size_t len) {
    node_room_t *room = find_room(node, room_id);

    if (!room || !sender_participant_id || !packet || len == 0 ||
        !find_session_by_participant(room, sender_participant_id)) {
        return -1;
    }

    return sfu_forward_packet(room->sfu, sender_participant_id, packet, len);
}

int turbo_sfu_node_get_room_stats(turbo_sfu_node_t *node, const char *room_id,
                                  turbo_sfu_node_room_stats_t *stats) {
    node_room_t *room = find_room(node, room_id);
    sfu_stats_t sfu_stats;

    if (!room || !stats) {
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    memset(&sfu_stats, 0, sizeof(sfu_stats));
    sfu_get_stats(room->sfu, &sfu_stats);

    copy_string(stats->room_id, sizeof(stats->room_id), room->room_id);
    stats->session_count = room->session_count;
    stats->published_track_count = room->track_count;
    stats->participant_count = sfu_stats.participant_count;
    stats->total_packets_routed = sfu_stats.total_packets_routed;
    stats->total_bytes_routed = sfu_stats.total_bytes_routed;
    stats->total_layer_switches = sfu_stats.total_layer_switches;
    return 0;
}

int turbo_sfu_node_get_participant_stats(turbo_sfu_node_t *node, const char *room_id,
                                         const char *participant_id,
                                         turbo_sfu_node_participant_stats_t *stats) {
    node_room_t *room = find_room(node, room_id);
    sfu_participant_stats_t sfu_stats;

    if (!room || !participant_id || !stats) {
        return -1;
    }

    memset(stats, 0, sizeof(*stats));
    memset(&sfu_stats, 0, sizeof(sfu_stats));
    sfu_get_participant_stats(room->sfu, participant_id, &sfu_stats);
    if (sfu_stats.participant_id[0] == '\0') {
        return -1;
    }

    copy_string(stats->participant_id, sizeof(stats->participant_id), sfu_stats.participant_id);
    stats->stream_count = sfu_stats.stream_count;
    stats->available_bandwidth = sfu_stats.available_bandwidth;
    stats->packets_sent = sfu_stats.packets_sent;
    stats->bytes_sent = sfu_stats.bytes_sent;
    stats->packets_received = sfu_stats.packets_received;
    stats->bytes_received = sfu_stats.bytes_received;
    return 0;
}

int turbo_sfu_node_get_track_subscription(
    turbo_sfu_node_t *node, const char *room_id, const char *receiver_participant_id,
    const char *track_id, turbo_sfu_node_track_subscription_t *subscription) {
    node_room_t *room = find_room(node, room_id);
    node_track_subscription_t *entry;

    if (!room || !receiver_participant_id || !track_id || !subscription) {
        return -1;
    }

    entry = find_track_subscription(room, receiver_participant_id, track_id);
    if (!entry) {
        return -1;
    }

    memset(subscription, 0, sizeof(*subscription));
    copy_string(subscription->receiver_participant_id,
                sizeof(subscription->receiver_participant_id),
                entry->receiver_participant_id);
    copy_string(subscription->sender_participant_id,
                sizeof(subscription->sender_participant_id),
                entry->sender_participant_id);
    copy_string(subscription->track_id, sizeof(subscription->track_id),
                entry->track_id);
    subscription->enabled = entry->enabled;
    subscription->priority = entry->priority;
    subscription->preferred_layer = entry->preferred_layer;
    subscription->target_layer = entry->target_layer;
    subscription->muted = entry->muted;
    copy_string(subscription->policy_source, sizeof(subscription->policy_source),
                entry->policy_source);
    subscription->max_layer = entry->max_layer;
    return 0;
}

void turbo_sfu_node_get_stats(turbo_sfu_node_t *node, turbo_sfu_node_stats_t *stats) {
    int i;

    if (!node || !stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    copy_string(stats->node_id, sizeof(stats->node_id), node->node_id);
    stats->room_count = node->room_count;

    for (i = 0; i < node->room_count; ++i) {
        sfu_stats_t sfu_stats;
        node_room_t *room = &node->rooms[i];

        memset(&sfu_stats, 0, sizeof(sfu_stats));
        sfu_get_stats(room->sfu, &sfu_stats);
        stats->session_count += room->session_count;
        stats->published_track_count += room->track_count;
        stats->total_packets_routed += sfu_stats.total_packets_routed;
        stats->total_bytes_routed += sfu_stats.total_bytes_routed;
        stats->total_layer_switches += sfu_stats.total_layer_switches;
    }
}
