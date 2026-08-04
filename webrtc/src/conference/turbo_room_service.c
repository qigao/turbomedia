#include "turbo_room_service.h"
#include "turbo_thread.h"
#include <platform.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define CALL_CENTER_QUEUE_DEFAULT_CLAIM_LEASE_MS 30000ULL

typedef struct {
    char participant_id[TURBO_PARTICIPANT_ID_MAX];
    char user_id[TURBO_USER_ID_MAX];
    char display_name[TURBO_DISPLAY_NAME_MAX];
    turbo_participant_role_t role;
    turbo_participant_join_state_t join_state;
    turbo_participant_session_state_t session_state;
    int bandwidth_bps;
    int has_explicit_bandwidth;
    int64_t version;
} room_participant_t;

typedef struct {
    char track_id[TURBO_TRACK_ID_MAX];
    char owner_participant_id[TURBO_PARTICIPANT_ID_MAX];
    turbo_room_track_kind_t kind;
    turbo_room_track_source_t source;
    char codec_name[TURBO_CODEC_NAME_MAX];
    int simulcast_enabled;
    int muted;
    uint32_t main_ssrc;
    uint32_t layer_ssrcs[3];
    int layer_count;
    int64_t version;
} room_track_t;

typedef struct {
    char subscriber_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char track_id[TURBO_TRACK_ID_MAX];
    int enabled;
    int priority;
    turbo_room_video_layer_t preferred_layer;
    turbo_room_video_layer_t target_layer;
    int muted;
    char policy_source[TURBO_POLICY_SOURCE_MAX];
    int64_t version;
} room_subscription_t;

typedef enum {
    CALL_CENTER_QUEUE_ENTRY_QUEUED = 1,
    CALL_CENTER_QUEUE_ENTRY_CLAIMED = 2,
    CALL_CENTER_QUEUE_ENTRY_ROUTED = 3,
    CALL_CENTER_QUEUE_ENTRY_CANCELED = 4
} call_center_queue_entry_state_t;

typedef struct {
    char queue_id[TURBO_CALL_CENTER_QUEUE_ID_MAX];
    turbo_call_center_queue_side_t side;
    char entry_id[TURBO_CALL_CENTER_QUEUE_ENTRY_ID_MAX];
    char endpoint_id[TURBO_CALL_CENTER_ENDPOINT_ID_MAX];
    int priority;
    int64_t sequence;
    int64_t version;
    uint64_t claimed_at_ms;
    atomic_int state;
} call_center_queue_entry_t;

typedef struct {
    char endpoint_id[TURBO_CALL_CENTER_ENDPOINT_ID_MAX];
    turbo_call_center_agent_state_t state;
    int64_t version;
} call_center_agent_state_entry_t;

typedef struct {
    char room_id[TURBO_ROOM_ID_MAX];
    turbo_room_type_t room_type;
    turbo_room_status_t status;
    char assigned_sfu_node[TURBO_NODE_ID_MAX];
    turbo_room_recording_state_t recording_state;
    turbo_room_layout_mode_t layout_mode;
    char active_speaker_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char pinned_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char recording_id[TURBO_RECORDING_ID_MAX];
    char recording_mode[TURBO_RECORDING_MODE_MAX];
    turbo_call_center_room_state_t call_center_state;
    char call_center_customer_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char call_center_agent_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char call_center_consult_agent_participant_id[TURBO_PARTICIPANT_ID_MAX];
    char call_center_disposition_code[TURBO_CALL_CENTER_DISPOSITION_CODE_MAX];
    int64_t call_center_version;
    int64_t version;

    room_participant_t *participants;
    int participant_count;
    int participant_capacity;

    room_track_t *tracks;
    int track_count;
    int track_capacity;

    room_subscription_t *subscriptions;
    int subscription_count;
    int subscription_capacity;
} room_entry_t;

struct turbo_room_service_s {
    room_entry_t *rooms;
    int room_count;
    int room_capacity;
    turbo_mutex_t room_mutex;

    call_center_queue_entry_t *call_center_queue_entries;
    int call_center_queue_entry_count;
    int call_center_queue_entry_capacity;
    call_center_agent_state_entry_t *call_center_agent_states;
    int call_center_agent_state_count;
    int call_center_agent_state_capacity;
    atomic_llong next_call_center_queue_sequence;
    turbo_mutex_t call_center_queue_mutex;
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

static room_entry_t *find_room(turbo_room_service_t *service, const char *room_id) {
    int i;

    if (!service || !room_id) {
        return NULL;
    }

    for (i = 0; i < service->room_count; ++i) {
        if (strcmp(service->rooms[i].room_id, room_id) == 0) {
            return &service->rooms[i];
        }
    }

    return NULL;
}

static room_participant_t *find_participant(room_entry_t *room, const char *participant_id) {
    int i;

    if (!room || !participant_id) {
        return NULL;
    }

    for (i = 0; i < room->participant_count; ++i) {
        if (strcmp(room->participants[i].participant_id, participant_id) == 0) {
            return &room->participants[i];
        }
    }

    return NULL;
}

static room_track_t *find_track(room_entry_t *room, const char *track_id) {
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

static int is_call_center_primary_role(turbo_participant_role_t role) {
    return role == TURBO_PARTICIPANT_ROLE_CUSTOMER ||
           role == TURBO_PARTICIPANT_ROLE_AGENT;
}

static int is_call_center_customer_participant(const room_entry_t *room,
                                               const room_participant_t *participant) {
    return room && participant &&
           room->call_center_customer_participant_id[0] != '\0' &&
           participant->participant_id[0] != '\0' &&
           strcmp(room->call_center_customer_participant_id,
                  participant->participant_id) == 0;
}

static int is_call_center_primary_agent_participant(const room_entry_t *room,
                                                    const room_participant_t *participant) {
    return room && participant &&
           room->call_center_agent_participant_id[0] != '\0' &&
           participant->participant_id[0] != '\0' &&
           strcmp(room->call_center_agent_participant_id,
                  participant->participant_id) == 0;
}

static int is_call_center_consult_agent_participant(const room_entry_t *room,
                                                    const room_participant_t *participant) {
    return room && participant &&
           room->call_center_consult_agent_participant_id[0] != '\0' &&
           participant->participant_id[0] != '\0' &&
           strcmp(room->call_center_consult_agent_participant_id,
                  participant->participant_id) == 0;
}

static int is_call_center_engaged_agent_participant(const room_entry_t *room,
                                                    const room_participant_t *participant) {
    return is_call_center_primary_agent_participant(room, participant) ||
           is_call_center_consult_agent_participant(room, participant);
}

static int is_call_center_primary_endpoint(const room_entry_t *room,
                                           const room_participant_t *participant) {
    return is_call_center_customer_participant(room, participant) ||
           is_call_center_primary_agent_participant(room, participant);
}

static int is_call_center_observable_participant(const room_entry_t *room,
                                                 const room_participant_t *participant) {
    return is_call_center_customer_participant(room, participant) ||
           is_call_center_engaged_agent_participant(room, participant);
}

static int room_has_call_center_topology(const room_entry_t *room) {
    return room && room->call_center_customer_participant_id[0] != '\0' &&
           room->call_center_agent_participant_id[0] != '\0';
}

static int role_can_publish(turbo_participant_role_t role) {
    return role != TURBO_PARTICIPANT_ROLE_QA_OBSERVER;
}

static room_subscription_t *find_subscription(room_entry_t *room,
                                              const char *subscriber_participant_id,
                                              const char *track_id) {
    int i;

    if (!room || !subscriber_participant_id || !track_id) {
        return NULL;
    }

    for (i = 0; i < room->subscription_count; ++i) {
        room_subscription_t *subscription = &room->subscriptions[i];
        if (strcmp(subscription->subscriber_participant_id, subscriber_participant_id) == 0 &&
            strcmp(subscription->track_id, track_id) == 0) {
            return subscription;
        }
    }

    return NULL;
}

static int is_valid_call_center_queue_side(turbo_call_center_queue_side_t side) {
    return side == TURBO_CALL_CENTER_QUEUE_CALLER ||
           side == TURBO_CALL_CENTER_QUEUE_CALLEE;
}

static int is_valid_call_center_agent_state(turbo_call_center_agent_state_t state) {
    return state == TURBO_CALL_CENTER_AGENT_AVAILABLE ||
           state == TURBO_CALL_CENTER_AGENT_RESERVED ||
           state == TURBO_CALL_CENTER_AGENT_BUSY ||
           state == TURBO_CALL_CENTER_AGENT_OFFLINE ||
           state == TURBO_CALL_CENTER_AGENT_WRAP_UP;
}

static int is_valid_call_center_room_state(turbo_call_center_room_state_t state) {
    return state == TURBO_CALL_CENTER_ROOM_ACTIVE ||
           state == TURBO_CALL_CENTER_ROOM_WRAP_UP ||
           state == TURBO_CALL_CENTER_ROOM_COMPLETED;
}

static int call_center_queue_entry_has_state(
    const call_center_queue_entry_t *entry,
    call_center_queue_entry_state_t state) {
    return entry &&
           atomic_load_explicit(&entry->state, memory_order_acquire) == (int)state;
}

static int is_active_call_center_queue_entry_state(int state) {
    return state == (int)CALL_CENTER_QUEUE_ENTRY_QUEUED ||
           state == (int)CALL_CENTER_QUEUE_ENTRY_CLAIMED;
}

static int call_center_queue_claim_is_expired(
    const call_center_queue_entry_t *entry, uint64_t now_ms, uint64_t lease_ms) {
    if (!entry) {
        return 0;
    }

    if (lease_ms == 0) {
        return 1;
    }

    return entry->claimed_at_ms != 0 &&
           now_ms - entry->claimed_at_ms >= lease_ms;
}

static int set_call_center_agent_state_locked(
    turbo_room_service_t *service, const char *endpoint_id,
    turbo_call_center_agent_state_t state);

static int recover_stale_call_center_queue_claims_locked(
    turbo_room_service_t *service, const char *queue_id,
    uint64_t lease_ms, int *recovered) {
    uint64_t now_ms;
    int count = 0;
    int i;

    if (!service || !queue_id) {
        return -1;
    }

    now_ms = turbo_monotonic_ms();
    for (i = 0; i < service->call_center_queue_entry_count; ++i) {
        call_center_queue_entry_t *entry = &service->call_center_queue_entries[i];
        if (strcmp(entry->queue_id, queue_id) != 0 ||
            !call_center_queue_entry_has_state(entry, CALL_CENTER_QUEUE_ENTRY_CLAIMED) ||
            !call_center_queue_claim_is_expired(entry, now_ms, lease_ms)) {
            continue;
        }

        atomic_store_explicit(&entry->state, (int)CALL_CENTER_QUEUE_ENTRY_QUEUED,
                              memory_order_release);
        entry->claimed_at_ms = 0;
        entry->version++;
        if (entry->side == TURBO_CALL_CENTER_QUEUE_CALLEE) {
            (void)set_call_center_agent_state_locked(
                service, entry->endpoint_id, TURBO_CALL_CENTER_AGENT_AVAILABLE);
        }
        count++;
    }

    if (recovered) {
        *recovered = count;
    }
    return 0;
}

static call_center_agent_state_entry_t *find_call_center_agent_state_entry_locked(
    turbo_room_service_t *service, const char *endpoint_id) {
    int i;

    if (!service || !endpoint_id || !endpoint_id[0]) {
        return NULL;
    }

    for (i = 0; i < service->call_center_agent_state_count; ++i) {
        call_center_agent_state_entry_t *entry = &service->call_center_agent_states[i];
        if (strcmp(entry->endpoint_id, endpoint_id) == 0) {
            return entry;
        }
    }

    return NULL;
}

static turbo_call_center_agent_state_t get_call_center_agent_effective_state_locked(
    turbo_room_service_t *service, const char *endpoint_id, int *explicit_state) {
    call_center_agent_state_entry_t *entry;

    if (explicit_state) {
        *explicit_state = 0;
    }
    if (!service || !endpoint_id || !endpoint_id[0]) {
        return TURBO_CALL_CENTER_AGENT_OFFLINE;
    }

    entry = find_call_center_agent_state_entry_locked(service, endpoint_id);
    if (!entry) {
        return TURBO_CALL_CENTER_AGENT_AVAILABLE;
    }

    if (explicit_state) {
        *explicit_state = 1;
    }
    return entry->state;
}

static int set_call_center_agent_state_locked(
    turbo_room_service_t *service, const char *endpoint_id,
    turbo_call_center_agent_state_t state) {
    call_center_agent_state_entry_t *entry;

    if (!service || !endpoint_id || !endpoint_id[0] ||
        !is_valid_call_center_agent_state(state)) {
        return -1;
    }

    entry = find_call_center_agent_state_entry_locked(service, endpoint_id);
    if (!entry) {
        if (ensure_capacity((void **)&service->call_center_agent_states,
                            &service->call_center_agent_state_capacity,
                            sizeof(*service->call_center_agent_states),
                            service->call_center_agent_state_count + 1) != 0) {
            return -1;
        }

        entry = &service->call_center_agent_states[service->call_center_agent_state_count++];
        memset(entry, 0, sizeof(*entry));
        copy_string(entry->endpoint_id, sizeof(entry->endpoint_id), endpoint_id);
    }

    if (entry->state != state) {
        entry->state = state;
        entry->version++;
    }
    return 0;
}

static call_center_queue_entry_t *find_call_center_queue_entry(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_side_t side, const char *entry_id) {
    int i;

    if (!service || !queue_id || !entry_id || !is_valid_call_center_queue_side(side)) {
        return NULL;
    }

    for (i = 0; i < service->call_center_queue_entry_count; ++i) {
        call_center_queue_entry_t *entry = &service->call_center_queue_entries[i];
        if (entry->side == side && strcmp(entry->queue_id, queue_id) == 0 &&
            strcmp(entry->entry_id, entry_id) == 0) {
            return entry;
        }
    }

    return NULL;
}

static int find_best_call_center_queue_entry_index(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_side_t side) {
    int best_index = -1;
    int i;

    if (!service || !queue_id || !is_valid_call_center_queue_side(side)) {
        return -1;
    }

    for (i = 0; i < service->call_center_queue_entry_count; ++i) {
        call_center_queue_entry_t *entry = &service->call_center_queue_entries[i];
        call_center_queue_entry_t *best;

        if (entry->side != side || strcmp(entry->queue_id, queue_id) != 0 ||
            !call_center_queue_entry_has_state(entry, CALL_CENTER_QUEUE_ENTRY_QUEUED)) {
            continue;
        }
        if (side == TURBO_CALL_CENTER_QUEUE_CALLEE &&
            get_call_center_agent_effective_state_locked(
                service, entry->endpoint_id, NULL) !=
                TURBO_CALL_CENTER_AGENT_AVAILABLE) {
            continue;
        }

        if (best_index < 0) {
            best_index = i;
            continue;
        }

        best = &service->call_center_queue_entries[best_index];
        if (entry->priority > best->priority ||
            (entry->priority == best->priority && entry->sequence < best->sequence)) {
            best_index = i;
        }
    }

    return best_index;
}

static void fill_call_center_queue_entry_summary(
    const call_center_queue_entry_t *entry,
    turbo_call_center_queue_entry_summary_t *summary) {
    if (!entry || !summary) {
        return;
    }

    memset(summary, 0, sizeof(*summary));
    copy_string(summary->queue_id, sizeof(summary->queue_id), entry->queue_id);
    copy_string(summary->entry_id, sizeof(summary->entry_id), entry->entry_id);
    copy_string(summary->endpoint_id, sizeof(summary->endpoint_id), entry->endpoint_id);
    summary->side = entry->side;
    summary->priority = entry->priority;
    summary->sequence = entry->sequence;
    summary->version = entry->version;
}

static void copy_call_center_queue_entry(call_center_queue_entry_t *dest,
                                         const call_center_queue_entry_t *src,
                                         int state) {
    if (!dest || !src) {
        return;
    }

    copy_string(dest->queue_id, sizeof(dest->queue_id), src->queue_id);
    dest->side = src->side;
    copy_string(dest->entry_id, sizeof(dest->entry_id), src->entry_id);
    copy_string(dest->endpoint_id, sizeof(dest->endpoint_id), src->endpoint_id);
    dest->priority = src->priority;
    dest->sequence = src->sequence;
    dest->version = src->version;
    dest->claimed_at_ms = src->claimed_at_ms;
    atomic_store_explicit(&dest->state, state, memory_order_release);
}

static void compact_call_center_queue_entries_locked(turbo_room_service_t *service) {
    int read_index;
    int write_index = 0;

    if (!service) {
        return;
    }

    for (read_index = 0; read_index < service->call_center_queue_entry_count;
         ++read_index) {
        call_center_queue_entry_t *entry =
            &service->call_center_queue_entries[read_index];
        int state = atomic_load_explicit(&entry->state, memory_order_acquire);

        if (!is_active_call_center_queue_entry_state(state)) {
            continue;
        }

        if (write_index != read_index) {
            copy_call_center_queue_entry(
                &service->call_center_queue_entries[write_index], entry, state);
        }
        write_index++;
    }

    service->call_center_queue_entry_count = write_index;
}

static int mark_call_center_queue_entry_state(
    call_center_queue_entry_t *entry,
    call_center_queue_entry_state_t expected,
    call_center_queue_entry_state_t desired) {
    int expected_state = (int)expected;

    if (!entry) {
        return -1;
    }

    if (!atomic_compare_exchange_strong_explicit(
            &entry->state, &expected_state, (int)desired,
            memory_order_acq_rel, memory_order_acquire)) {
        return -1;
    }

    entry->claimed_at_ms = desired == CALL_CENTER_QUEUE_ENTRY_CLAIMED
                               ? turbo_monotonic_ms()
                               : 0;
    entry->version++;
    return 0;
}

static void remove_subscription_at(room_entry_t *room, int index) {
    if (!room || index < 0 || index >= room->subscription_count) {
        return;
    }

    if (index + 1 < room->subscription_count) {
        memmove(&room->subscriptions[index], &room->subscriptions[index + 1],
                (size_t)(room->subscription_count - index - 1) * sizeof(room_subscription_t));
    }
    room->subscription_count--;
}

static void free_room_entry(room_entry_t *room) {
    if (!room) {
        return;
    }

    free(room->participants);
    free(room->tracks);
    free(room->subscriptions);
    memset(room, 0, sizeof(*room));
}

static void remove_room_at(turbo_room_service_t *service, int index) {
    if (!service || index < 0 || index >= service->room_count) {
        return;
    }

    free_room_entry(&service->rooms[index]);
    if (index + 1 < service->room_count) {
        memmove(&service->rooms[index], &service->rooms[index + 1],
                (size_t)(service->room_count - index - 1) * sizeof(room_entry_t));
    }
    service->room_count--;
    if (service->room_count >= 0) {
        memset(&service->rooms[service->room_count], 0, sizeof(room_entry_t));
    }
}

static turbo_room_video_layer_t resolve_subscription_video_layer(
    const room_subscription_t *subscription) {
    if (!subscription) {
        return TURBO_ROOM_VIDEO_LAYER_NONE;
    }

    if (subscription->target_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
        return subscription->target_layer;
    }
    if (subscription->preferred_layer != TURBO_ROOM_VIDEO_LAYER_NONE) {
        return subscription->preferred_layer;
    }

    return TURBO_ROOM_VIDEO_LAYER_LOW;
}

static int video_subscription_budget_bps(const room_track_t *track,
                                         turbo_room_video_layer_t layer) {
    if (!track || layer == TURBO_ROOM_VIDEO_LAYER_NONE) {
        return 0;
    }

    if (track->source == TURBO_ROOM_SOURCE_SCREEN) {
        switch (layer) {
            case TURBO_ROOM_VIDEO_LAYER_LOW: return 700000;
            case TURBO_ROOM_VIDEO_LAYER_MEDIUM: return 1500000;
            case TURBO_ROOM_VIDEO_LAYER_HIGH: return 2500000;
            default: return 0;
        }
    }

    switch (layer) {
        case TURBO_ROOM_VIDEO_LAYER_LOW: return 250000;
        case TURBO_ROOM_VIDEO_LAYER_MEDIUM: return 800000;
        case TURBO_ROOM_VIDEO_LAYER_HIGH: return 1500000;
        default: return 0;
    }
}

static int string_equals(const char *lhs, const char *rhs) {
    if (!lhs || !rhs) {
        return 0;
    }

    return strcmp(lhs, rhs) == 0;
}

static int set_subscription_fields(room_subscription_t *subscription, int enabled,
                                   int priority,
                                   turbo_room_video_layer_t preferred_layer,
                                   turbo_room_video_layer_t target_layer,
                                   int muted, const char *policy_source) {
    char policy_source_buf[TURBO_POLICY_SOURCE_MAX];
    int changed = 0;

    if (!subscription) {
        return 0;
    }

    copy_string(policy_source_buf, sizeof(policy_source_buf), policy_source);
    if (subscription->enabled != (enabled ? 1 : 0)) {
        subscription->enabled = enabled ? 1 : 0;
        changed = 1;
    }
    if (subscription->priority != priority) {
        subscription->priority = priority;
        changed = 1;
    }
    if (subscription->preferred_layer != preferred_layer) {
        subscription->preferred_layer = preferred_layer;
        changed = 1;
    }
    if (subscription->target_layer != target_layer) {
        subscription->target_layer = target_layer;
        changed = 1;
    }
    if (subscription->muted != (muted ? 1 : 0)) {
        subscription->muted = muted ? 1 : 0;
        changed = 1;
    }
    if (strcmp(subscription->policy_source, policy_source_buf) != 0) {
        copy_string(subscription->policy_source, sizeof(subscription->policy_source),
                    policy_source_buf);
        changed = 1;
    }

    if (changed) {
        subscription->version++;
    }

    return changed;
}

static int ensure_subscription(room_entry_t *room, const char *subscriber_participant_id,
                               const char *track_id,
                               room_subscription_t **out_subscription) {
    room_subscription_t *subscription;

    if (out_subscription) {
        *out_subscription = NULL;
    }
    if (!room || !subscriber_participant_id || !track_id) {
        return -1;
    }

    subscription = find_subscription(room, subscriber_participant_id, track_id);
    if (!subscription) {
        if (ensure_capacity((void **)&room->subscriptions, &room->subscription_capacity,
                            sizeof(room_subscription_t), room->subscription_count + 1) != 0) {
            return -1;
        }
        subscription = &room->subscriptions[room->subscription_count++];
        memset(subscription, 0, sizeof(*subscription));
        copy_string(subscription->subscriber_participant_id,
                    sizeof(subscription->subscriber_participant_id),
                    subscriber_participant_id);
        copy_string(subscription->track_id, sizeof(subscription->track_id), track_id);
        subscription->version = 1;
    }

    if (out_subscription) {
        *out_subscription = subscription;
    }
    return 0;
}

static void derive_video_subscription(const room_entry_t *room, const room_track_t *track,
                                      int *enabled, int *priority,
                                      turbo_room_video_layer_t *preferred_layer,
                                      turbo_room_video_layer_t *target_layer,
                                      int *muted, const char **policy_source) {
    int derived_enabled = 1;
    int derived_priority = 25;
    turbo_room_video_layer_t derived_layer = TURBO_ROOM_VIDEO_LAYER_LOW;
    const char *derived_policy = "layout_speaker";

    if (!room || !track) {
        return;
    }

    if (track->muted) {
        derived_enabled = 0;
        derived_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
        derived_policy = "layout_muted";
    } else if (track->source == TURBO_ROOM_SOURCE_SCREEN) {
        derived_priority = 100;
        derived_layer = TURBO_ROOM_VIDEO_LAYER_HIGH;
        derived_policy = "layout_screenshare";
    } else if (room->pinned_participant_id[0] != '\0' &&
               string_equals(room->pinned_participant_id, track->owner_participant_id)) {
        derived_priority = 95;
        derived_layer = TURBO_ROOM_VIDEO_LAYER_HIGH;
        derived_policy = "layout_pin";
    } else if (room->active_speaker_participant_id[0] != '\0' &&
               string_equals(room->active_speaker_participant_id,
                             track->owner_participant_id)) {
        derived_priority = 80;
        derived_layer = room->layout_mode == TURBO_ROOM_LAYOUT_GRID
                            ? TURBO_ROOM_VIDEO_LAYER_MEDIUM
                            : TURBO_ROOM_VIDEO_LAYER_HIGH;
        derived_policy = room->layout_mode == TURBO_ROOM_LAYOUT_GRID
                             ? "layout_grid"
                             : "layout_speaker";
    } else if (room->layout_mode == TURBO_ROOM_LAYOUT_GRID) {
        derived_priority = 40;
        derived_layer = TURBO_ROOM_VIDEO_LAYER_LOW;
        derived_policy = "layout_grid";
    }

    if (enabled) {
        *enabled = derived_enabled;
    }
    if (priority) {
        *priority = derived_priority;
    }
    if (preferred_layer) {
        *preferred_layer = derived_layer;
    }
    if (target_layer) {
        *target_layer = derived_layer;
    }
    if (muted) {
        *muted = derived_enabled ? 0 : 1;
    }
    if (policy_source) {
        *policy_source = derived_policy;
    }
}

static void remove_track_at(room_entry_t *room, int index) {
    if (!room || index < 0 || index >= room->track_count) {
        return;
    }

    if (index + 1 < room->track_count) {
        memmove(&room->tracks[index], &room->tracks[index + 1],
                (size_t)(room->track_count - index - 1) * sizeof(room_track_t));
    }
    room->track_count--;
}

static void room_bump_version(room_entry_t *room) {
    if (!room) {
        return;
    }

    room->version++;
}

static void room_bump_call_center_version(room_entry_t *room) {
    if (!room) {
        return;
    }

    room->call_center_version++;
    room_bump_version(room);
}

static void update_room_activity(room_entry_t *room) {
    if (!room || room->status == TURBO_ROOM_STATUS_CLOSED ||
        room->status == TURBO_ROOM_STATUS_CLOSING) {
        return;
    }

    room->status = room->participant_count > 0 ? TURBO_ROOM_STATUS_ACTIVE : TURBO_ROOM_STATUS_OPEN;
}

static void fill_participant_summary(const room_participant_t *participant,
                                     turbo_room_participant_summary_t *summary) {
    if (!participant || !summary) {
        return;
    }

    memset(summary, 0, sizeof(*summary));
    copy_string(summary->participant_id, sizeof(summary->participant_id),
                participant->participant_id);
    copy_string(summary->user_id, sizeof(summary->user_id), participant->user_id);
    copy_string(summary->display_name, sizeof(summary->display_name),
                participant->display_name);
    summary->role = participant->role;
    summary->join_state = participant->join_state;
    summary->session_state = participant->session_state;
    summary->bandwidth_bps = participant->bandwidth_bps;
    summary->version = participant->version;
}

static void fill_track_summary(const room_track_t *track,
                               turbo_room_track_summary_t *summary) {
    if (!track || !summary) {
        return;
    }

    memset(summary, 0, sizeof(*summary));
    copy_string(summary->track_id, sizeof(summary->track_id), track->track_id);
    copy_string(summary->owner_participant_id, sizeof(summary->owner_participant_id),
                track->owner_participant_id);
    copy_string(summary->codec_name, sizeof(summary->codec_name), track->codec_name);
    summary->kind = track->kind;
    summary->source = track->source;
    summary->simulcast_enabled = track->simulcast_enabled;
    summary->muted = track->muted;
    summary->main_ssrc = track->main_ssrc;
    summary->layer_count = track->layer_count;
    memcpy(summary->layer_ssrcs, track->layer_ssrcs, sizeof(summary->layer_ssrcs));
    summary->version = track->version;
}

static void fill_subscription_summary(const room_subscription_t *subscription,
                                      turbo_room_subscription_summary_t *summary) {
    if (!subscription || !summary) {
        return;
    }

    memset(summary, 0, sizeof(*summary));
    copy_string(summary->subscriber_participant_id,
                sizeof(summary->subscriber_participant_id),
                subscription->subscriber_participant_id);
    copy_string(summary->track_id, sizeof(summary->track_id), subscription->track_id);
    copy_string(summary->policy_source, sizeof(summary->policy_source),
                subscription->policy_source);
    summary->enabled = subscription->enabled;
    summary->priority = subscription->priority;
    summary->preferred_layer = subscription->preferred_layer;
    summary->target_layer = subscription->target_layer;
    summary->muted = subscription->muted;
    summary->version = subscription->version;
}

turbo_room_service_t *turbo_room_service_create(void) {
    turbo_room_service_t *service =
        (turbo_room_service_t *)calloc(1, sizeof(turbo_room_service_t));

    if (!service) {
        return NULL;
    }

    atomic_init(&service->next_call_center_queue_sequence, 0);
    turbo_mutex_init(&service->room_mutex);
    turbo_mutex_init(&service->call_center_queue_mutex);
    return service;
}

void turbo_room_service_destroy(turbo_room_service_t *service) {
    int i;

    if (!service) {
        return;
    }

    for (i = 0; i < service->room_count; ++i) {
        free_room_entry(&service->rooms[i]);
    }
    free(service->rooms);
    free(service->call_center_queue_entries);
    free(service->call_center_agent_states);
    turbo_mutex_destroy(&service->room_mutex);
    turbo_mutex_destroy(&service->call_center_queue_mutex);
    free(service);
}

int turbo_room_service_create_room(turbo_room_service_t *service,
                                   const turbo_room_config_t *config) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !config || !config->room_id || config->room_type == 0) {
        return -1;
    }
    turbo_mutex_lock(&service->room_mutex);
    if (find_room(service, config->room_id)) {
        goto out;
    }
    if (ensure_capacity((void **)&service->rooms, &service->room_capacity,
                        sizeof(room_entry_t), service->room_count + 1) != 0) {
        goto out;
    }

    room = &service->rooms[service->room_count++];
    memset(room, 0, sizeof(*room));
    copy_string(room->room_id, sizeof(room->room_id), config->room_id);
    room->room_type = config->room_type;
    room->status = TURBO_ROOM_STATUS_OPEN;
    room->recording_state = TURBO_ROOM_RECORDING_STOPPED;
    room->layout_mode = TURBO_ROOM_LAYOUT_SPEAKER;
    room->call_center_state = TURBO_CALL_CENTER_ROOM_NONE;
    room->call_center_version = 0;
    room->version = 1;
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_close_room(turbo_room_service_t *service, const char *room_id) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }

    if (room->call_center_state == TURBO_CALL_CENTER_ROOM_ACTIVE) {
        turbo_mutex_lock(&service->call_center_queue_mutex);
        if (room->call_center_agent_participant_id[0] != '\0' &&
            set_call_center_agent_state_locked(
                service, room->call_center_agent_participant_id,
                TURBO_CALL_CENTER_AGENT_AVAILABLE) != 0) {
            turbo_mutex_unlock(&service->call_center_queue_mutex);
            goto out;
        }
        if (room->call_center_consult_agent_participant_id[0] != '\0' &&
            set_call_center_agent_state_locked(
                service, room->call_center_consult_agent_participant_id,
                TURBO_CALL_CENTER_AGENT_AVAILABLE) != 0) {
            turbo_mutex_unlock(&service->call_center_queue_mutex);
            goto out;
        }
        turbo_mutex_unlock(&service->call_center_queue_mutex);

        room->call_center_state = TURBO_CALL_CENTER_ROOM_COMPLETED;
        room_bump_call_center_version(room);
    }

    room->status = TURBO_ROOM_STATUS_CLOSED;
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_discard_unassigned_room(turbo_room_service_t *service,
                                               const char *room_id) {
    int i;
    int rc = -1;

    if (!service || !room_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    for (i = 0; i < service->room_count; ++i) {
        room_entry_t *room = &service->rooms[i];
        if (strcmp(room->room_id, room_id) != 0) {
            continue;
        }
        if (room->assigned_sfu_node[0] != '\0') {
            goto out;
        }
        remove_room_at(service, i);
        rc = 0;
        goto out;
    }

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_assign_sfu_node(turbo_room_service_t *service, const char *room_id,
                                       const char *node_id) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id || !node_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }

    copy_string(room->assigned_sfu_node, sizeof(room->assigned_sfu_node), node_id);
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_get_room_summary(turbo_room_service_t *service, const char *room_id,
                                        turbo_room_summary_t *summary) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id || !summary) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }

    memset(summary, 0, sizeof(*summary));
    copy_string(summary->room_id, sizeof(summary->room_id), room->room_id);
    summary->room_type = room->room_type;
    summary->status = room->status;
    copy_string(summary->assigned_sfu_node, sizeof(summary->assigned_sfu_node),
                room->assigned_sfu_node);
    summary->recording_state = room->recording_state;
    summary->layout_mode = room->layout_mode;
    copy_string(summary->active_speaker_participant_id,
                sizeof(summary->active_speaker_participant_id),
                room->active_speaker_participant_id);
    copy_string(summary->pinned_participant_id, sizeof(summary->pinned_participant_id),
                room->pinned_participant_id);
    copy_string(summary->recording_id, sizeof(summary->recording_id), room->recording_id);
    copy_string(summary->recording_mode, sizeof(summary->recording_mode), room->recording_mode);
    summary->participant_count = room->participant_count;
    summary->published_track_count = room->track_count;
    summary->subscription_count = room->subscription_count;
    summary->version = room->version;
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_add_participant(turbo_room_service_t *service, const char *room_id,
                                       const turbo_room_participant_config_t *config) {
    room_entry_t *room;
    room_participant_t *participant;
    int rc = -1;

    if (!service || !room_id || !config || !config->participant_id) {
        return -1;
    }
    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }
    if (find_participant(room, config->participant_id)) {
        goto out;
    }
    if (ensure_capacity((void **)&room->participants, &room->participant_capacity,
                        sizeof(room_participant_t), room->participant_count + 1) != 0) {
        goto out;
    }

    participant = &room->participants[room->participant_count++];
    memset(participant, 0, sizeof(*participant));
    copy_string(participant->participant_id, sizeof(participant->participant_id),
                config->participant_id);
    copy_string(participant->user_id, sizeof(participant->user_id), config->user_id);
    copy_string(participant->display_name, sizeof(participant->display_name),
                config->display_name);
    participant->role = config->role;
    participant->join_state = TURBO_PARTICIPANT_JOIN_JOINED;
    participant->session_state = TURBO_PARTICIPANT_SESSION_NONE;
    participant->bandwidth_bps = 0;
    participant->has_explicit_bandwidth = 0;
    participant->version = 1;
    update_room_activity(room);
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_remove_participant(turbo_room_service_t *service, const char *room_id,
                                          const char *participant_id) {
    room_entry_t *room;
    char primary_agent_id[TURBO_PARTICIPANT_ID_MAX];
    char consult_agent_id[TURBO_PARTICIPANT_ID_MAX];
    int removed_is_customer = 0;
    int removed_is_agent = 0;
    int removed_is_consult = 0;
    int i;
    int rc = -1;

    if (!service || !room_id || !participant_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }

    memset(primary_agent_id, 0, sizeof(primary_agent_id));
    memset(consult_agent_id, 0, sizeof(consult_agent_id));
    copy_string(primary_agent_id, sizeof(primary_agent_id),
                room->call_center_agent_participant_id);
    copy_string(consult_agent_id, sizeof(consult_agent_id),
                room->call_center_consult_agent_participant_id);
    removed_is_customer =
        string_equals(room->call_center_customer_participant_id, participant_id);
    removed_is_agent = string_equals(room->call_center_agent_participant_id, participant_id);
    removed_is_consult =
        string_equals(room->call_center_consult_agent_participant_id, participant_id);

    if (removed_is_consult) {
        room->call_center_consult_agent_participant_id[0] = '\0';
        if (room->call_center_state == TURBO_CALL_CENTER_ROOM_ACTIVE) {
            (void)set_call_center_agent_state_locked(service, participant_id,
                                                     TURBO_CALL_CENTER_AGENT_AVAILABLE);
        }
    }

    if ((removed_is_customer || removed_is_agent) &&
        room->call_center_state != TURBO_CALL_CENTER_ROOM_NONE &&
        room->call_center_state != TURBO_CALL_CENTER_ROOM_COMPLETED) {
        if (primary_agent_id[0] != '\0') {
            (void)set_call_center_agent_state_locked(service, primary_agent_id,
                                                     TURBO_CALL_CENTER_AGENT_AVAILABLE);
        }
        if (consult_agent_id[0] != '\0' &&
            !string_equals(consult_agent_id, primary_agent_id)) {
            (void)set_call_center_agent_state_locked(service, consult_agent_id,
                                                     TURBO_CALL_CENTER_AGENT_AVAILABLE);
        }
        room->call_center_state = TURBO_CALL_CENTER_ROOM_COMPLETED;
        room->call_center_customer_participant_id[0] = '\0';
        room->call_center_agent_participant_id[0] = '\0';
        room->call_center_consult_agent_participant_id[0] = '\0';
    }

    for (i = room->subscription_count - 1; i >= 0; --i) {
        room_subscription_t *subscription = &room->subscriptions[i];
        room_track_t *track = find_track(room, subscription->track_id);
        if (strcmp(subscription->subscriber_participant_id, participant_id) == 0 ||
            (track && strcmp(track->owner_participant_id, participant_id) == 0)) {
            remove_subscription_at(room, i);
        }
    }

    for (i = room->track_count - 1; i >= 0; --i) {
        if (strcmp(room->tracks[i].owner_participant_id, participant_id) == 0) {
            remove_track_at(room, i);
        }
    }

    for (i = 0; i < room->participant_count; ++i) {
        if (strcmp(room->participants[i].participant_id, participant_id) == 0) {
            if (i + 1 < room->participant_count) {
                memmove(&room->participants[i], &room->participants[i + 1],
                        (size_t)(room->participant_count - i - 1) *
                            sizeof(room_participant_t));
            }
            room->participant_count--;
            update_room_activity(room);
            room_bump_version(room);
            rc = 0;
            goto out;
        }
    }

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_set_participant_session_state(
    turbo_room_service_t *service, const char *room_id, const char *participant_id,
    turbo_participant_session_state_t session_state) {
    room_entry_t *room;
    room_participant_t *participant;
    int rc = -1;

    if (!service || !room_id || !participant_id || session_state == 0) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    participant = find_participant(room, participant_id);
    if (!participant) {
        goto out;
    }

    participant->session_state = session_state;
    participant->version++;
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_set_participant_bandwidth(
    turbo_room_service_t *service, const char *room_id, const char *participant_id,
    int bandwidth_bps) {
    room_entry_t *room;
    room_participant_t *participant;
    int rc = -1;

    if (!service || !room_id || !participant_id || bandwidth_bps < 0) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    participant = find_participant(room, participant_id);
    if (!participant) {
        goto out;
    }

    participant->bandwidth_bps = bandwidth_bps;
    participant->has_explicit_bandwidth = 1;
    participant->version++;
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_get_participant_summary(
    turbo_room_service_t *service, const char *room_id, const char *participant_id,
    turbo_room_participant_summary_t *summary) {
    room_entry_t *room;
    room_participant_t *participant;
    int rc = -1;

    if (!service || !room_id || !participant_id || !summary) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    participant = find_participant(room, participant_id);
    if (!participant) {
        goto out;
    }

    fill_participant_summary(participant, summary);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_get_participant_summary_at(
    turbo_room_service_t *service, const char *room_id, int index,
    turbo_room_participant_summary_t *summary) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id || !summary || index < 0) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room || index >= room->participant_count) {
        goto out;
    }

    fill_participant_summary(&room->participants[index], summary);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_get_effective_receiver_bandwidth(
    turbo_room_service_t *service, const char *room_id, const char *participant_id,
    int *bandwidth_bps, int *is_explicit) {
    room_entry_t *room;
    room_participant_t *participant;
    int total_budget = 0;
    int i;
    int rc = -1;

    if (!service || !room_id || !participant_id || !bandwidth_bps) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    participant = find_participant(room, participant_id);
    if (!participant) {
        goto out;
    }

    if (participant->has_explicit_bandwidth) {
        *bandwidth_bps = participant->bandwidth_bps;
        if (is_explicit) {
            *is_explicit = 1;
        }
        rc = 0;
        goto out;
    }

    for (i = 0; i < room->subscription_count; ++i) {
        room_subscription_t *subscription = &room->subscriptions[i];
        room_track_t *track;

        if (strcmp(subscription->subscriber_participant_id, participant_id) != 0 ||
            !subscription->enabled || subscription->muted) {
            continue;
        }

        track = find_track(room, subscription->track_id);
        if (!track || track->muted) {
            continue;
        }

        if (track->kind == TURBO_ROOM_TRACK_AUDIO) {
            total_budget += 64000;
            continue;
        }

        if (track->kind == TURBO_ROOM_TRACK_VIDEO) {
            total_budget += video_subscription_budget_bps(
                track, resolve_subscription_video_layer(subscription));
        }
    }

    *bandwidth_bps = total_budget;
    if (is_explicit) {
        *is_explicit = 0;
    }
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_publish_track(turbo_room_service_t *service, const char *room_id,
                                     const turbo_room_track_config_t *config) {
    room_entry_t *room;
    room_track_t *track;
    room_participant_t *owner;
    int i;
    int rc = -1;

    if (!service || !room_id || !config || !config->track_id ||
        !config->owner_participant_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    owner = find_participant(room, config->owner_participant_id);
    if (!room || !owner || !role_can_publish(owner->role)) {
        goto out;
    }

    track = find_track(room, config->track_id);
    if (!track) {
        if (ensure_capacity((void **)&room->tracks, &room->track_capacity, sizeof(room_track_t),
                            room->track_count + 1) != 0) {
            goto out;
        }
        track = &room->tracks[room->track_count++];
        memset(track, 0, sizeof(*track));
        copy_string(track->track_id, sizeof(track->track_id), config->track_id);
    }

    copy_string(track->owner_participant_id, sizeof(track->owner_participant_id),
                config->owner_participant_id);
    track->kind = config->kind;
    track->source = config->source;
    copy_string(track->codec_name, sizeof(track->codec_name), config->codec_name);
    track->simulcast_enabled = config->simulcast_enabled ? 1 : 0;
    track->main_ssrc = config->main_ssrc;
    track->layer_count = 0;
    memset(track->layer_ssrcs, 0, sizeof(track->layer_ssrcs));
    if (config->layer_ssrcs && config->layer_count > 0) {
        track->layer_count = config->layer_count > 3 ? 3 : config->layer_count;
        for (i = 0; i < track->layer_count; ++i) {
            track->layer_ssrcs[i] = config->layer_ssrcs[i];
        }
    }
    track->version++;
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_unpublish_track(turbo_room_service_t *service, const char *room_id,
                                       const char *track_id) {
    room_entry_t *room;
    int i;
    int rc = -1;

    if (!service || !room_id || !track_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }

    for (i = room->subscription_count - 1; i >= 0; --i) {
        if (strcmp(room->subscriptions[i].track_id, track_id) == 0) {
            remove_subscription_at(room, i);
        }
    }

    for (i = 0; i < room->track_count; ++i) {
        if (strcmp(room->tracks[i].track_id, track_id) == 0) {
            remove_track_at(room, i);
            room_bump_version(room);
            rc = 0;
            goto out;
        }
    }

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_set_track_muted(turbo_room_service_t *service, const char *room_id,
                                       const char *track_id, int muted) {
    room_entry_t *room;
    room_track_t *track;
    int rc = -1;

    if (!service || !room_id || !track_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    track = find_track(room, track_id);
    if (!track) {
        goto out;
    }

    track->muted = muted ? 1 : 0;
    track->version++;
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_get_track_summary(turbo_room_service_t *service, const char *room_id,
                                         const char *track_id,
                                         turbo_room_track_summary_t *summary) {
    room_entry_t *room;
    room_track_t *track;
    int rc = -1;

    if (!service || !room_id || !track_id || !summary) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    track = find_track(room, track_id);
    if (!track) {
        goto out;
    }

    fill_track_summary(track, summary);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_get_track_summary_at(
    turbo_room_service_t *service, const char *room_id, int index,
    turbo_room_track_summary_t *summary) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id || !summary || index < 0) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room || index >= room->track_count) {
        goto out;
    }

    fill_track_summary(&room->tracks[index], summary);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_set_subscription(turbo_room_service_t *service, const char *room_id,
                                        const turbo_room_subscription_config_t *config) {
    room_entry_t *room;
    room_subscription_t *subscription;
    int rc = -1;

    if (!service || !room_id || !config || !config->subscriber_participant_id ||
        !config->track_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room || !find_participant(room, config->subscriber_participant_id) ||
        !find_track(room, config->track_id)) {
        goto out;
    }

    subscription = find_subscription(room, config->subscriber_participant_id, config->track_id);
    if (!subscription) {
        if (ensure_capacity((void **)&room->subscriptions, &room->subscription_capacity,
                            sizeof(room_subscription_t), room->subscription_count + 1) != 0) {
            goto out;
        }
        subscription = &room->subscriptions[room->subscription_count++];
        memset(subscription, 0, sizeof(*subscription));
        copy_string(subscription->subscriber_participant_id,
                    sizeof(subscription->subscriber_participant_id),
                    config->subscriber_participant_id);
        copy_string(subscription->track_id, sizeof(subscription->track_id), config->track_id);
    }

    subscription->enabled = config->enabled ? 1 : 0;
    subscription->priority = config->priority;
    subscription->preferred_layer = config->preferred_layer;
    subscription->target_layer = config->target_layer;
    subscription->muted = config->muted ? 1 : 0;
    copy_string(subscription->policy_source, sizeof(subscription->policy_source),
                config->policy_source);
    subscription->version++;
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_remove_subscription(turbo_room_service_t *service, const char *room_id,
                                           const char *subscriber_participant_id,
                                           const char *track_id) {
    room_entry_t *room;
    int i;
    int rc = -1;

    if (!service || !room_id || !subscriber_participant_id || !track_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }

    for (i = 0; i < room->subscription_count; ++i) {
        room_subscription_t *subscription = &room->subscriptions[i];
        if (strcmp(subscription->subscriber_participant_id, subscriber_participant_id) == 0 &&
            strcmp(subscription->track_id, track_id) == 0) {
            remove_subscription_at(room, i);
            room_bump_version(room);
            rc = 0;
            goto out;
        }
    }

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_get_subscription_summary(
    turbo_room_service_t *service, const char *room_id,
    const char *subscriber_participant_id, const char *track_id,
    turbo_room_subscription_summary_t *summary) {
    room_entry_t *room;
    room_subscription_t *subscription;
    int rc = -1;

    if (!service || !room_id || !subscriber_participant_id || !track_id || !summary) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    subscription = find_subscription(room, subscriber_participant_id, track_id);
    if (!subscription) {
        goto out;
    }

    fill_subscription_summary(subscription, summary);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_get_subscription_summary_at(
    turbo_room_service_t *service, const char *room_id, int index,
    turbo_room_subscription_summary_t *summary) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id || !summary || index < 0) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room || index >= room->subscription_count) {
        goto out;
    }

    fill_subscription_summary(&room->subscriptions[index], summary);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_start_recording(turbo_room_service_t *service, const char *room_id,
                                       const char *recording_id, const char *mode) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id || !recording_id || !mode) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }
    if (room->recording_state == TURBO_ROOM_RECORDING_ACTIVE ||
        room->recording_state == TURBO_ROOM_RECORDING_STARTING) {
        goto out;
    }

    room->recording_state = TURBO_ROOM_RECORDING_ACTIVE;
    copy_string(room->recording_id, sizeof(room->recording_id), recording_id);
    copy_string(room->recording_mode, sizeof(room->recording_mode), mode);
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_stop_recording(turbo_room_service_t *service, const char *room_id) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id) {
        return -1;
    }
    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }
    if (room->recording_state != TURBO_ROOM_RECORDING_ACTIVE) {
        goto out;
    }

    room->recording_state = TURBO_ROOM_RECORDING_STOPPED;
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_set_layout_mode(turbo_room_service_t *service, const char *room_id,
                                       turbo_room_layout_mode_t layout_mode) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id || layout_mode == 0) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }

    room->layout_mode = layout_mode;
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_set_active_speaker(turbo_room_service_t *service, const char *room_id,
                                          const char *participant_id) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }
    if (participant_id && participant_id[0] != '\0' &&
        !find_participant(room, participant_id)) {
        goto out;
    }

    copy_string(room->active_speaker_participant_id,
                sizeof(room->active_speaker_participant_id), participant_id);
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_pin_participant(turbo_room_service_t *service, const char *room_id,
                                       const char *participant_id) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }
    if (participant_id && participant_id[0] != '\0' &&
        !find_participant(room, participant_id)) {
        goto out;
    }

    copy_string(room->pinned_participant_id, sizeof(room->pinned_participant_id),
                participant_id);
    room_bump_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

static int is_valid_call_center_supervisor_mode(
    turbo_call_center_supervisor_mode_t supervisor_mode) {
    return supervisor_mode == TURBO_CALL_CENTER_SUPERVISOR_NONE ||
           supervisor_mode == TURBO_CALL_CENTER_SUPERVISOR_MONITOR ||
           supervisor_mode == TURBO_CALL_CENTER_SUPERVISOR_WHISPER ||
           supervisor_mode == TURBO_CALL_CENTER_SUPERVISOR_BARGE;
}

static int call_center_subscription_allowed(
    const room_entry_t *room, const room_participant_t *subscriber,
    const room_participant_t *owner, const room_track_t *track,
    turbo_call_center_supervisor_mode_t supervisor_mode, const char **policy_source) {
    if (policy_source) {
        *policy_source = "call_center";
    }
    if (!subscriber || !owner || !track ||
        string_equals(subscriber->participant_id, owner->participant_id)) {
        return 0;
    }

    if (room_has_call_center_topology(room)) {
        if (is_call_center_primary_endpoint(room, subscriber) &&
            is_call_center_primary_endpoint(room, owner)) {
            if (policy_source) {
                *policy_source = "call_center_call";
            }
            return 1;
        }

        if (room->call_center_consult_agent_participant_id[0] != '\0') {
            if (is_call_center_customer_participant(room, subscriber) &&
                is_call_center_consult_agent_participant(room, owner)) {
                return 0;
            }

            if (is_call_center_primary_agent_participant(room, subscriber) &&
                is_call_center_consult_agent_participant(room, owner)) {
                if (policy_source) {
                    *policy_source = "call_center_consult";
                }
                return 1;
            }

            if (is_call_center_consult_agent_participant(room, subscriber) &&
                (is_call_center_primary_agent_participant(room, owner) ||
                 is_call_center_customer_participant(room, owner))) {
                if (policy_source) {
                    *policy_source = "call_center_consult";
                }
                return 1;
            }
        }

        if (subscriber->role == TURBO_PARTICIPANT_ROLE_SUPERVISOR &&
            is_call_center_observable_participant(room, owner) &&
            supervisor_mode != TURBO_CALL_CENTER_SUPERVISOR_NONE) {
            if (policy_source) {
                *policy_source = "call_center_monitor";
            }
            return 1;
        }

        if ((subscriber->role == TURBO_PARTICIPANT_ROLE_QA_OBSERVER ||
             subscriber->role == TURBO_PARTICIPANT_ROLE_BOT) &&
            is_call_center_observable_participant(room, owner)) {
            if (policy_source) {
                *policy_source = "call_center_observe";
            }
            return 1;
        }

        if (owner->role == TURBO_PARTICIPANT_ROLE_SUPERVISOR &&
            is_call_center_engaged_agent_participant(room, subscriber) &&
            supervisor_mode == TURBO_CALL_CENTER_SUPERVISOR_WHISPER &&
            track->kind == TURBO_ROOM_TRACK_AUDIO) {
            if (policy_source) {
                *policy_source = "call_center_whisper";
            }
            return 1;
        }

        if (owner->role == TURBO_PARTICIPANT_ROLE_SUPERVISOR &&
            is_call_center_observable_participant(room, subscriber) &&
            supervisor_mode == TURBO_CALL_CENTER_SUPERVISOR_BARGE) {
            if (policy_source) {
                *policy_source = "call_center_barge";
            }
            return 1;
        }

        return 0;
    }

    if (is_call_center_primary_role(subscriber->role) &&
        is_call_center_primary_role(owner->role)) {
        if (policy_source) {
            *policy_source = "call_center_call";
        }
        return 1;
    }

    if (subscriber->role == TURBO_PARTICIPANT_ROLE_SUPERVISOR &&
        is_call_center_primary_role(owner->role) &&
        supervisor_mode != TURBO_CALL_CENTER_SUPERVISOR_NONE) {
        if (policy_source) {
            *policy_source = "call_center_monitor";
        }
        return 1;
    }

    if ((subscriber->role == TURBO_PARTICIPANT_ROLE_QA_OBSERVER ||
         subscriber->role == TURBO_PARTICIPANT_ROLE_BOT) &&
        is_call_center_primary_role(owner->role)) {
        if (policy_source) {
            *policy_source = "call_center_observe";
        }
        return 1;
    }

    if (owner->role == TURBO_PARTICIPANT_ROLE_SUPERVISOR &&
        subscriber->role == TURBO_PARTICIPANT_ROLE_AGENT &&
        supervisor_mode == TURBO_CALL_CENTER_SUPERVISOR_WHISPER &&
        track->kind == TURBO_ROOM_TRACK_AUDIO) {
        if (policy_source) {
            *policy_source = "call_center_whisper";
        }
        return 1;
    }

    if (owner->role == TURBO_PARTICIPANT_ROLE_SUPERVISOR &&
        is_call_center_primary_role(subscriber->role) &&
        supervisor_mode == TURBO_CALL_CENTER_SUPERVISOR_BARGE) {
        if (policy_source) {
            *policy_source = "call_center_barge";
        }
        return 1;
    }

    return 0;
}

static void derive_call_center_subscription(
    const room_track_t *track, const char *base_policy_source, int *enabled,
    int *priority, turbo_room_video_layer_t *preferred_layer,
    turbo_room_video_layer_t *target_layer, int *muted,
    const char **policy_source) {
    int derived_enabled = 1;
    int derived_priority = 100;
    turbo_room_video_layer_t derived_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
    const char *derived_policy = base_policy_source ? base_policy_source : "call_center";

    if (!track) {
        return;
    }

    if (track->muted) {
        derived_enabled = 0;
        derived_priority = 0;
        derived_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
        derived_policy = "call_center_muted";
    } else if (track->kind == TURBO_ROOM_TRACK_AUDIO) {
        derived_priority = 100;
        derived_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
    } else if (track->source == TURBO_ROOM_SOURCE_SCREEN) {
        derived_priority = 95;
        derived_layer = TURBO_ROOM_VIDEO_LAYER_HIGH;
        derived_policy = "call_center_screenshare";
    } else {
        derived_priority = 40;
        derived_layer = TURBO_ROOM_VIDEO_LAYER_LOW;
    }

    if (enabled) {
        *enabled = derived_enabled;
    }
    if (priority) {
        *priority = derived_priority;
    }
    if (preferred_layer) {
        *preferred_layer = derived_layer;
    }
    if (target_layer) {
        *target_layer = derived_layer;
    }
    if (muted) {
        *muted = derived_enabled ? 0 : 1;
    }
    if (policy_source) {
        *policy_source = derived_policy;
    }
}

int turbo_room_service_reconcile_call_center_subscriptions(
    turbo_room_service_t *service, const char *room_id,
    turbo_call_center_supervisor_mode_t supervisor_mode) {
    room_entry_t *room;
    int changed = 0;
    int i;
    int j;
    int rc = -1;

    if (!service || !room_id || !is_valid_call_center_supervisor_mode(supervisor_mode)) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }

    for (i = room->subscription_count - 1; i >= 0; --i) {
        room_subscription_t *subscription = &room->subscriptions[i];
        room_participant_t *subscriber =
            find_participant(room, subscription->subscriber_participant_id);
        room_track_t *track = find_track(room, subscription->track_id);
        room_participant_t *owner =
            track ? find_participant(room, track->owner_participant_id) : NULL;

        if (!subscriber || !track || !owner ||
            !call_center_subscription_allowed(room, subscriber, owner, track,
                                              supervisor_mode, NULL)) {
            remove_subscription_at(room, i);
            changed = 1;
        }
    }

    for (i = 0; i < room->participant_count; ++i) {
        room_participant_t *subscriber = &room->participants[i];

        for (j = 0; j < room->track_count; ++j) {
            room_track_t *track = &room->tracks[j];
            room_participant_t *owner = find_participant(room, track->owner_participant_id);
            room_subscription_t *subscription = NULL;
            int enabled = 0;
            int priority = 0;
            int muted = 0;
            turbo_room_video_layer_t preferred_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
            turbo_room_video_layer_t target_layer = TURBO_ROOM_VIDEO_LAYER_NONE;
            const char *policy_source = "call_center";

            if (!call_center_subscription_allowed(room, subscriber, owner, track,
                                                  supervisor_mode, &policy_source)) {
                continue;
            }
            if (ensure_subscription(room, subscriber->participant_id, track->track_id,
                                    &subscription) != 0) {
                goto out;
            }

            derive_call_center_subscription(track, policy_source, &enabled, &priority,
                                            &preferred_layer, &target_layer, &muted,
                                            &policy_source);
            changed |= set_subscription_fields(subscription, enabled, priority,
                                               preferred_layer, target_layer, muted,
                                               policy_source);
        }
    }

    if (changed) {
        room_bump_version(room);
    }

    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_enqueue_call_center_queue_entry(
    turbo_room_service_t *service,
    const turbo_call_center_queue_entry_config_t *config) {
    call_center_queue_entry_t *entry;
    int changed = 0;
    int state;
    int rc = 0;

    if (!service || !config || !config->queue_id || !config->queue_id[0] ||
        !config->entry_id || !config->entry_id[0] || !config->endpoint_id ||
        !config->endpoint_id[0] || !is_valid_call_center_queue_side(config->side)) {
        return -1;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    entry = find_call_center_queue_entry(service, config->queue_id, config->side,
                                         config->entry_id);
    if (!entry) {
        compact_call_center_queue_entries_locked(service);
        if (ensure_capacity((void **)&service->call_center_queue_entries,
                            &service->call_center_queue_entry_capacity,
                            sizeof(call_center_queue_entry_t),
                            service->call_center_queue_entry_count + 1) != 0) {
            rc = -1;
            goto out;
        }

        entry = &service->call_center_queue_entries[service->call_center_queue_entry_count++];
        memset(entry, 0, sizeof(*entry));
        copy_string(entry->queue_id, sizeof(entry->queue_id), config->queue_id);
        entry->side = config->side;
        copy_string(entry->entry_id, sizeof(entry->entry_id), config->entry_id);
        copy_string(entry->endpoint_id, sizeof(entry->endpoint_id), config->endpoint_id);
        entry->priority = config->priority;
        entry->sequence = (int64_t)atomic_fetch_add_explicit(
                              &service->next_call_center_queue_sequence, 1,
                              memory_order_relaxed) +
                          1;
        entry->version = 1;
        entry->claimed_at_ms = 0;
        atomic_init(&entry->state, (int)CALL_CENTER_QUEUE_ENTRY_QUEUED);
        goto out;
    }

    state = atomic_load_explicit(&entry->state, memory_order_acquire);
    if (state == (int)CALL_CENTER_QUEUE_ENTRY_CLAIMED) {
        rc = -1;
        goto out;
    }
    if (state != (int)CALL_CENTER_QUEUE_ENTRY_QUEUED) {
        copy_string(entry->endpoint_id, sizeof(entry->endpoint_id), config->endpoint_id);
        entry->priority = config->priority;
        entry->sequence = (int64_t)atomic_fetch_add_explicit(
                              &service->next_call_center_queue_sequence, 1,
                              memory_order_relaxed) +
                          1;
        entry->version++;
        entry->claimed_at_ms = 0;
        atomic_store_explicit(&entry->state, (int)CALL_CENTER_QUEUE_ENTRY_QUEUED,
                              memory_order_release);
        goto out;
    }

    if (strcmp(entry->endpoint_id, config->endpoint_id) != 0) {
        copy_string(entry->endpoint_id, sizeof(entry->endpoint_id), config->endpoint_id);
        changed = 1;
    }
    if (entry->priority != config->priority) {
        entry->priority = config->priority;
        changed = 1;
    }
    if (changed) {
        entry->version++;
    }

out:
    turbo_mutex_unlock(&service->call_center_queue_mutex);
    return rc;
}

int turbo_room_service_remove_call_center_queue_entry(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_side_t side, const char *entry_id) {
    call_center_queue_entry_t *entry;
    int rc;

    if (!service || !queue_id || !entry_id || !is_valid_call_center_queue_side(side)) {
        return -1;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    entry = find_call_center_queue_entry(service, queue_id, side, entry_id);
    rc = mark_call_center_queue_entry_state(entry, CALL_CENTER_QUEUE_ENTRY_QUEUED,
                                            CALL_CENTER_QUEUE_ENTRY_CANCELED);
    turbo_mutex_unlock(&service->call_center_queue_mutex);
    return rc;
}

int turbo_room_service_get_call_center_queue_depth(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_side_t side, int *depth) {
    int count = 0;
    int i;

    if (!service || !queue_id || !is_valid_call_center_queue_side(side) || !depth) {
        return -1;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    (void)recover_stale_call_center_queue_claims_locked(
        service, queue_id, CALL_CENTER_QUEUE_DEFAULT_CLAIM_LEASE_MS, NULL);
    for (i = 0; i < service->call_center_queue_entry_count; ++i) {
        call_center_queue_entry_t *entry = &service->call_center_queue_entries[i];
        if (entry->side == side && strcmp(entry->queue_id, queue_id) == 0 &&
            call_center_queue_entry_has_state(entry, CALL_CENTER_QUEUE_ENTRY_QUEUED)) {
            count++;
        }
    }
    turbo_mutex_unlock(&service->call_center_queue_mutex);

    *depth = count;
    return 0;
}

int turbo_room_service_peek_call_center_queue_entry(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_side_t side,
    turbo_call_center_queue_entry_summary_t *summary) {
    int index;

    if (!service || !queue_id || !is_valid_call_center_queue_side(side) || !summary) {
        return -1;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    (void)recover_stale_call_center_queue_claims_locked(
        service, queue_id, CALL_CENTER_QUEUE_DEFAULT_CLAIM_LEASE_MS, NULL);
    index = find_best_call_center_queue_entry_index(service, queue_id, side);
    if (index < 0) {
        turbo_mutex_unlock(&service->call_center_queue_mutex);
        return -1;
    }

    fill_call_center_queue_entry_summary(&service->call_center_queue_entries[index],
                                         summary);
    turbo_mutex_unlock(&service->call_center_queue_mutex);
    return 0;
}

int turbo_room_service_pop_call_center_queue_entry(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_side_t side,
    turbo_call_center_queue_entry_summary_t *summary) {
    int index;

    if (!service || !queue_id || !is_valid_call_center_queue_side(side) || !summary) {
        return -1;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    (void)recover_stale_call_center_queue_claims_locked(
        service, queue_id, CALL_CENTER_QUEUE_DEFAULT_CLAIM_LEASE_MS, NULL);
    index = find_best_call_center_queue_entry_index(service, queue_id, side);
    if (index < 0) {
        turbo_mutex_unlock(&service->call_center_queue_mutex);
        return -1;
    }

    fill_call_center_queue_entry_summary(&service->call_center_queue_entries[index],
                                         summary);
    if (mark_call_center_queue_entry_state(&service->call_center_queue_entries[index],
                                           CALL_CENTER_QUEUE_ENTRY_QUEUED,
                                           CALL_CENTER_QUEUE_ENTRY_CANCELED) != 0) {
        turbo_mutex_unlock(&service->call_center_queue_mutex);
        return -1;
    }
    turbo_mutex_unlock(&service->call_center_queue_mutex);
    return 0;
}

static int claim_call_center_queue_match_locked(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_match_summary_t *summary) {
    int caller_index;
    int callee_index;

    memset(summary, 0, sizeof(*summary));
    caller_index = find_best_call_center_queue_entry_index(
        service, queue_id, TURBO_CALL_CENTER_QUEUE_CALLER);
    callee_index = find_best_call_center_queue_entry_index(
        service, queue_id, TURBO_CALL_CENTER_QUEUE_CALLEE);

    if (caller_index < 0 || callee_index < 0) {
        return 0;
    }

    if (mark_call_center_queue_entry_state(&service->call_center_queue_entries[caller_index],
                                           CALL_CENTER_QUEUE_ENTRY_QUEUED,
                                           CALL_CENTER_QUEUE_ENTRY_CLAIMED) != 0) {
        return -1;
    }
    if (mark_call_center_queue_entry_state(&service->call_center_queue_entries[callee_index],
                                           CALL_CENTER_QUEUE_ENTRY_QUEUED,
                                           CALL_CENTER_QUEUE_ENTRY_CLAIMED) != 0) {
        (void)mark_call_center_queue_entry_state(
            &service->call_center_queue_entries[caller_index],
            CALL_CENTER_QUEUE_ENTRY_CLAIMED, CALL_CENTER_QUEUE_ENTRY_QUEUED);
        return -1;
    }
    if (set_call_center_agent_state_locked(
            service, service->call_center_queue_entries[callee_index].endpoint_id,
            TURBO_CALL_CENTER_AGENT_RESERVED) != 0) {
        (void)mark_call_center_queue_entry_state(
            &service->call_center_queue_entries[callee_index],
            CALL_CENTER_QUEUE_ENTRY_CLAIMED, CALL_CENTER_QUEUE_ENTRY_QUEUED);
        (void)mark_call_center_queue_entry_state(
            &service->call_center_queue_entries[caller_index],
            CALL_CENTER_QUEUE_ENTRY_CLAIMED, CALL_CENTER_QUEUE_ENTRY_QUEUED);
        return -1;
    }

    summary->matched = 1;
    fill_call_center_queue_entry_summary(&service->call_center_queue_entries[caller_index],
                                         &summary->caller);
    fill_call_center_queue_entry_summary(&service->call_center_queue_entries[callee_index],
                                         &summary->callee);
    return 0;
}

static int set_claimed_call_center_queue_match_state(
    turbo_room_service_t *service, const char *queue_id,
    const char *caller_entry_id, const char *callee_entry_id,
    call_center_queue_entry_state_t desired) {
    call_center_queue_entry_t *caller;
    call_center_queue_entry_t *callee;
    int rc = -1;

    if (!service || !queue_id || !caller_entry_id || !callee_entry_id) {
        return -1;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    caller = find_call_center_queue_entry(service, queue_id,
                                          TURBO_CALL_CENTER_QUEUE_CALLER,
                                          caller_entry_id);
    callee = find_call_center_queue_entry(service, queue_id,
                                          TURBO_CALL_CENTER_QUEUE_CALLEE,
                                          callee_entry_id);
    if (caller && callee &&
        call_center_queue_entry_has_state(caller, CALL_CENTER_QUEUE_ENTRY_CLAIMED) &&
        call_center_queue_entry_has_state(callee, CALL_CENTER_QUEUE_ENTRY_CLAIMED)) {
        atomic_store_explicit(&caller->state, (int)desired, memory_order_release);
        atomic_store_explicit(&callee->state, (int)desired, memory_order_release);
        caller->claimed_at_ms = 0;
        callee->claimed_at_ms = 0;
        caller->version++;
        callee->version++;
        if (desired == CALL_CENTER_QUEUE_ENTRY_ROUTED) {
            (void)set_call_center_agent_state_locked(
                service, callee->endpoint_id, TURBO_CALL_CENTER_AGENT_BUSY);
        } else if (desired == CALL_CENTER_QUEUE_ENTRY_QUEUED) {
            (void)set_call_center_agent_state_locked(
                service, callee->endpoint_id, TURBO_CALL_CENTER_AGENT_AVAILABLE);
        }
        rc = 0;
    }
    turbo_mutex_unlock(&service->call_center_queue_mutex);
    return rc;
}

int turbo_room_service_match_call_center_queue(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_match_summary_t *summary) {
    call_center_queue_entry_t *caller;
    call_center_queue_entry_t *callee;
    int rc;

    if (!service || !queue_id || !summary) {
        return -1;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    (void)recover_stale_call_center_queue_claims_locked(
        service, queue_id, CALL_CENTER_QUEUE_DEFAULT_CLAIM_LEASE_MS, NULL);
    rc = claim_call_center_queue_match_locked(service, queue_id, summary);
    if (rc == 0 && summary->matched) {
        caller = find_call_center_queue_entry(service, queue_id,
                                              TURBO_CALL_CENTER_QUEUE_CALLER,
                                              summary->caller.entry_id);
        callee = find_call_center_queue_entry(service, queue_id,
                                              TURBO_CALL_CENTER_QUEUE_CALLEE,
                                              summary->callee.entry_id);
        if (!caller || !callee) {
            rc = -1;
        } else {
            atomic_store_explicit(&caller->state, (int)CALL_CENTER_QUEUE_ENTRY_ROUTED,
                                  memory_order_release);
            atomic_store_explicit(&callee->state, (int)CALL_CENTER_QUEUE_ENTRY_ROUTED,
                                  memory_order_release);
            caller->claimed_at_ms = 0;
            callee->claimed_at_ms = 0;
            caller->version++;
            callee->version++;
            (void)set_call_center_agent_state_locked(
                service, callee->endpoint_id, TURBO_CALL_CENTER_AGENT_BUSY);
        }
    }
    turbo_mutex_unlock(&service->call_center_queue_mutex);

    return rc;
}

int turbo_room_service_claim_call_center_queue_match(
    turbo_room_service_t *service, const char *queue_id,
    turbo_call_center_queue_match_summary_t *summary) {
    int rc;

    if (!service || !queue_id || !summary) {
        return -1;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    (void)recover_stale_call_center_queue_claims_locked(
        service, queue_id, CALL_CENTER_QUEUE_DEFAULT_CLAIM_LEASE_MS, NULL);
    rc = claim_call_center_queue_match_locked(service, queue_id, summary);
    turbo_mutex_unlock(&service->call_center_queue_mutex);
    return rc;
}

int turbo_room_service_complete_call_center_queue_match(
    turbo_room_service_t *service, const char *queue_id,
    const char *caller_entry_id, const char *callee_entry_id) {
    return set_claimed_call_center_queue_match_state(
        service, queue_id, caller_entry_id, callee_entry_id,
        CALL_CENTER_QUEUE_ENTRY_ROUTED);
}

int turbo_room_service_rollback_call_center_queue_match(
    turbo_room_service_t *service, const char *queue_id,
    const char *caller_entry_id, const char *callee_entry_id) {
    return set_claimed_call_center_queue_match_state(
        service, queue_id, caller_entry_id, callee_entry_id,
        CALL_CENTER_QUEUE_ENTRY_QUEUED);
}

int turbo_room_service_recover_stale_call_center_queue_claims(
    turbo_room_service_t *service, const char *queue_id,
    uint64_t lease_ms, int *recovered) {
    int rc;

    if (!service || !queue_id) {
        return -1;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    rc = recover_stale_call_center_queue_claims_locked(service, queue_id,
                                                       lease_ms, recovered);
    turbo_mutex_unlock(&service->call_center_queue_mutex);
    return rc;
}

int turbo_room_service_set_call_center_agent_state(
    turbo_room_service_t *service, const char *endpoint_id,
    turbo_call_center_agent_state_t state) {
    int rc;

    if (!service || !endpoint_id || !endpoint_id[0] ||
        !is_valid_call_center_agent_state(state)) {
        return -1;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    rc = set_call_center_agent_state_locked(service, endpoint_id, state);
    turbo_mutex_unlock(&service->call_center_queue_mutex);
    return rc;
}

int turbo_room_service_get_call_center_agent_state(
    turbo_room_service_t *service, const char *endpoint_id,
    turbo_call_center_agent_state_summary_t *summary) {
    call_center_agent_state_entry_t *entry;

    if (!service || !endpoint_id || !endpoint_id[0] || !summary) {
        return -1;
    }

    memset(summary, 0, sizeof(*summary));
    copy_string(summary->endpoint_id, sizeof(summary->endpoint_id), endpoint_id);

    turbo_mutex_lock(&service->call_center_queue_mutex);
    entry = find_call_center_agent_state_entry_locked(service, endpoint_id);
    if (!entry) {
        summary->state = TURBO_CALL_CENTER_AGENT_AVAILABLE;
        turbo_mutex_unlock(&service->call_center_queue_mutex);
        return 0;
    }

    summary->state = entry->state;
    summary->explicit_state = 1;
    summary->version = entry->version;
    turbo_mutex_unlock(&service->call_center_queue_mutex);
    return 0;
}

int turbo_room_service_start_call_center_room(
    turbo_room_service_t *service, const char *room_id,
    const char *customer_participant_id, const char *agent_participant_id) {
    room_entry_t *room;
    room_participant_t *customer;
    room_participant_t *agent;
    int rc = -1;

    if (!service || !room_id || !customer_participant_id || !customer_participant_id[0] ||
        !agent_participant_id || !agent_participant_id[0]) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    customer = find_participant(room, customer_participant_id);
    agent = find_participant(room, agent_participant_id);
    if (!room || !customer || !agent ||
        customer->role != TURBO_PARTICIPANT_ROLE_CUSTOMER ||
        agent->role != TURBO_PARTICIPANT_ROLE_AGENT) {
        goto out;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    if (set_call_center_agent_state_locked(service, agent_participant_id,
                                           TURBO_CALL_CENTER_AGENT_BUSY) != 0) {
        turbo_mutex_unlock(&service->call_center_queue_mutex);
        goto out;
    }
    turbo_mutex_unlock(&service->call_center_queue_mutex);

    room->call_center_state = TURBO_CALL_CENTER_ROOM_ACTIVE;
    copy_string(room->call_center_customer_participant_id,
                sizeof(room->call_center_customer_participant_id),
                customer_participant_id);
    copy_string(room->call_center_agent_participant_id,
                sizeof(room->call_center_agent_participant_id), agent_participant_id);
    room->call_center_consult_agent_participant_id[0] = '\0';
    room->call_center_disposition_code[0] = '\0';
    room_bump_call_center_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_set_call_center_consult_agent(
    turbo_room_service_t *service, const char *room_id,
    const char *consult_agent_participant_id) {
    room_entry_t *room;
    room_participant_t *consult_agent;
    char previous_consult_agent_id[TURBO_PARTICIPANT_ID_MAX];
    int rc = -1;

    if (!service || !room_id || !consult_agent_participant_id ||
        !consult_agent_participant_id[0]) {
        return -1;
    }

    memset(previous_consult_agent_id, 0, sizeof(previous_consult_agent_id));
    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    consult_agent = find_participant(room, consult_agent_participant_id);
    if (!room || room->call_center_state != TURBO_CALL_CENTER_ROOM_ACTIVE ||
        room->call_center_agent_participant_id[0] == '\0' || !consult_agent ||
        consult_agent->role != TURBO_PARTICIPANT_ROLE_AGENT ||
        string_equals(room->call_center_agent_participant_id,
                      consult_agent_participant_id)) {
        goto out;
    }

    copy_string(previous_consult_agent_id, sizeof(previous_consult_agent_id),
                room->call_center_consult_agent_participant_id);

    turbo_mutex_lock(&service->call_center_queue_mutex);
    if (previous_consult_agent_id[0] != '\0' &&
        !string_equals(previous_consult_agent_id, consult_agent_participant_id) &&
        set_call_center_agent_state_locked(service, previous_consult_agent_id,
                                           TURBO_CALL_CENTER_AGENT_AVAILABLE) != 0) {
        turbo_mutex_unlock(&service->call_center_queue_mutex);
        goto out;
    }
    if (set_call_center_agent_state_locked(service, consult_agent_participant_id,
                                           TURBO_CALL_CENTER_AGENT_BUSY) != 0) {
        if (previous_consult_agent_id[0] != '\0' &&
            !string_equals(previous_consult_agent_id, consult_agent_participant_id)) {
            (void)set_call_center_agent_state_locked(service,
                                                     previous_consult_agent_id,
                                                     TURBO_CALL_CENTER_AGENT_BUSY);
        }
        turbo_mutex_unlock(&service->call_center_queue_mutex);
        goto out;
    }
    turbo_mutex_unlock(&service->call_center_queue_mutex);

    copy_string(room->call_center_consult_agent_participant_id,
                sizeof(room->call_center_consult_agent_participant_id),
                consult_agent_participant_id);
    room_bump_call_center_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_complete_call_center_transfer(
    turbo_room_service_t *service, const char *room_id,
    turbo_call_center_agent_state_t released_agent_state) {
    room_entry_t *room;
    room_participant_t *agent;
    room_participant_t *consult_agent;
    char released_agent_id[TURBO_PARTICIPANT_ID_MAX];
    char new_agent_id[TURBO_PARTICIPANT_ID_MAX];
    int rc = -1;

    if (!service || !room_id ||
        !(released_agent_state == TURBO_CALL_CENTER_AGENT_AVAILABLE ||
          released_agent_state == TURBO_CALL_CENTER_AGENT_OFFLINE ||
          released_agent_state == TURBO_CALL_CENTER_AGENT_WRAP_UP)) {
        return -1;
    }

    memset(released_agent_id, 0, sizeof(released_agent_id));
    memset(new_agent_id, 0, sizeof(new_agent_id));
    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    agent = find_participant(room, room ? room->call_center_agent_participant_id : NULL);
    consult_agent = find_participant(
        room, room ? room->call_center_consult_agent_participant_id : NULL);
    if (!room || room->call_center_state != TURBO_CALL_CENTER_ROOM_ACTIVE || !agent ||
        !consult_agent || agent->role != TURBO_PARTICIPANT_ROLE_AGENT ||
        consult_agent->role != TURBO_PARTICIPANT_ROLE_AGENT) {
        goto out;
    }

    copy_string(released_agent_id, sizeof(released_agent_id),
                room->call_center_agent_participant_id);
    copy_string(new_agent_id, sizeof(new_agent_id),
                room->call_center_consult_agent_participant_id);

    turbo_mutex_lock(&service->call_center_queue_mutex);
    if (set_call_center_agent_state_locked(service, released_agent_id,
                                           released_agent_state) != 0) {
        turbo_mutex_unlock(&service->call_center_queue_mutex);
        goto out;
    }
    if (set_call_center_agent_state_locked(service, new_agent_id,
                                           TURBO_CALL_CENTER_AGENT_BUSY) != 0) {
        (void)set_call_center_agent_state_locked(service, released_agent_id,
                                                 TURBO_CALL_CENTER_AGENT_BUSY);
        turbo_mutex_unlock(&service->call_center_queue_mutex);
        goto out;
    }
    turbo_mutex_unlock(&service->call_center_queue_mutex);

    copy_string(room->call_center_agent_participant_id,
                sizeof(room->call_center_agent_participant_id), new_agent_id);
    room->call_center_consult_agent_participant_id[0] = '\0';
    room_bump_call_center_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_finalize_call_center_room(
    turbo_room_service_t *service, const char *room_id,
    turbo_call_center_room_state_t state, const char *disposition_code,
    turbo_call_center_agent_state_t agent_state) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id || !is_valid_call_center_room_state(state) ||
        !is_valid_call_center_agent_state(agent_state)) {
        return -1;
    }
    if ((state == TURBO_CALL_CENTER_ROOM_WRAP_UP &&
         !(agent_state == TURBO_CALL_CENTER_AGENT_WRAP_UP ||
           agent_state == TURBO_CALL_CENTER_AGENT_OFFLINE)) ||
        (state == TURBO_CALL_CENTER_ROOM_COMPLETED &&
         !(agent_state == TURBO_CALL_CENTER_AGENT_AVAILABLE ||
           agent_state == TURBO_CALL_CENTER_AGENT_OFFLINE))) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room || room->call_center_state == TURBO_CALL_CENTER_ROOM_NONE ||
        room->call_center_agent_participant_id[0] == '\0') {
        goto out;
    }

    turbo_mutex_lock(&service->call_center_queue_mutex);
    if (set_call_center_agent_state_locked(service,
                                           room->call_center_agent_participant_id,
                                           agent_state) != 0) {
        turbo_mutex_unlock(&service->call_center_queue_mutex);
        goto out;
    }
    turbo_mutex_unlock(&service->call_center_queue_mutex);

    room->call_center_state = state;
    copy_string(room->call_center_disposition_code,
                sizeof(room->call_center_disposition_code), disposition_code);
    room_bump_call_center_version(room);
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_get_call_center_room_summary(
    turbo_room_service_t *service, const char *room_id,
    turbo_call_center_room_summary_t *summary) {
    room_entry_t *room;
    int rc = -1;

    if (!service || !room_id || !summary) {
        return -1;
    }

    memset(summary, 0, sizeof(*summary));
    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room || room->call_center_state == TURBO_CALL_CENTER_ROOM_NONE) {
        goto out;
    }

    summary->available = 1;
    copy_string(summary->room_id, sizeof(summary->room_id), room->room_id);
    summary->state = room->call_center_state;
    copy_string(summary->customer_participant_id,
                sizeof(summary->customer_participant_id),
                room->call_center_customer_participant_id);
    copy_string(summary->agent_participant_id,
                sizeof(summary->agent_participant_id),
                room->call_center_agent_participant_id);
    copy_string(summary->consult_agent_participant_id,
                sizeof(summary->consult_agent_participant_id),
                room->call_center_consult_agent_participant_id);
    copy_string(summary->disposition_code, sizeof(summary->disposition_code),
                room->call_center_disposition_code);
    summary->version = room->call_center_version;
    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}

int turbo_room_service_reconcile_subscriptions(turbo_room_service_t *service,
                                               const char *room_id) {
    room_entry_t *room;
    int changed = 0;
    int i;
    int j;
    int rc = -1;

    if (!service || !room_id) {
        return -1;
    }

    turbo_mutex_lock(&service->room_mutex);
    room = find_room(service, room_id);
    if (!room) {
        goto out;
    }

    for (i = 0; i < room->participant_count; ++i) {
        room_participant_t *subscriber = &room->participants[i];

        for (j = 0; j < room->track_count; ++j) {
            room_track_t *track = &room->tracks[j];
            room_subscription_t *subscription = NULL;

            if (string_equals(subscriber->participant_id, track->owner_participant_id)) {
                continue;
            }
            if (ensure_subscription(room, subscriber->participant_id, track->track_id,
                                    &subscription) != 0) {
                goto out;
            }

            if (track->kind == TURBO_ROOM_TRACK_AUDIO) {
                changed |= set_subscription_fields(subscription,
                                                   track->muted ? 0 : 1,
                                                   100,
                                                   TURBO_ROOM_VIDEO_LAYER_NONE,
                                                   TURBO_ROOM_VIDEO_LAYER_NONE,
                                                   track->muted ? 1 : 0,
                                                   "layout_audio");
                continue;
            }

            if (track->kind == TURBO_ROOM_TRACK_VIDEO) {
                int enabled = 0;
                int priority = 0;
                int muted = 0;
                turbo_room_video_layer_t preferred_layer =
                    TURBO_ROOM_VIDEO_LAYER_NONE;
                turbo_room_video_layer_t target_layer =
                    TURBO_ROOM_VIDEO_LAYER_NONE;
                const char *policy_source = "layout_speaker";

                derive_video_subscription(room, track, &enabled, &priority,
                                          &preferred_layer, &target_layer, &muted,
                                          &policy_source);
                changed |= set_subscription_fields(subscription, enabled, priority,
                                                   preferred_layer, target_layer,
                                                   muted, policy_source);
            }
        }
    }

    if (changed) {
        room_bump_version(room);
    }

    rc = 0;

out:
    turbo_mutex_unlock(&service->room_mutex);
    return rc;
}
