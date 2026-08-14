#include "ivr_fmq_adapter.h"
#include "ivr/ivr_acl.h"
#include "ivr_thread.h"
#include "platform.h"
#include "turbo_uuid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IVR_FMQ_ADAPTER_DEFAULT_SEQ_CAPACITY 256u
#define IVR_FMQ_ADAPTER_DEFAULT_WORKER_CAPACITY 64u
#define IVR_FMQ_ADAPTER_DEFAULT_ASSIGNMENT_CAPACITY 256u
#define IVR_FMQ_ADAPTER_DEFAULT_DIALOG_CAPACITY 256u
#define IVR_FMQ_ADAPTER_DEFAULT_LEGACY_WORKER_MAX_SESSIONS 1u
#define IVR_FMQ_ADAPTER_DEFAULT_WORKER_LEASE_MS 15000u
#define IVR_FMQ_ADAPTER_DEFAULT_DISPATCH_DEADLINE_MS 5000u
#define IVR_FMQ_ADAPTER_DEFAULT_DISPATCH_MAX_ATTEMPTS 3u
#define IVR_FMQ_ADAPTER_DEFAULT_DEDUP_RETENTION_MS 60000u
#define IVR_FMQ_ADAPTER_MAX_DISPATCH_ATTEMPTS 8u
#define IVR_FMQ_ADAPTER_DEFAULT_PUB_TOPIC "room.events"
#define IVR_FMQ_ADAPTER_DEFAULT_CONTENT_PACKAGE "conference-greeting"

/* One bounded per-call domain sequence slot. The aggregate has no per-call
   counter; this derived table advances only when a command for that call is
   applied successfully (a derived view of the authoritative state). */
typedef struct {
    char room_id[TURBO_ROOM_ID_MAX];
    char call_id[TURBO_PARTICIPANT_ID_MAX];
    uint64_t call_generation;
    uint64_t last_sequence;
    int valid;
} ivr_fmq_call_seq_t;

typedef struct {
    enum {
        IVR_FMQ_CLEANUP_NONE = 0,
        IVR_FMQ_CLEANUP_PENDING = 1,
        IVR_FMQ_CLEANUP_COMPLETE = 2
    } cleanup_state;
    char message_id[128];
    char attempt_id[128];
    char worker_id[128];
    char worker_instance_id[128];
    uint64_t worker_connection_generation;
    char cleanup_release_message_id[128];
    uint64_t cleanup_deadline_at_ms;
} ivr_fmq_attempt_record_t;

typedef struct {
    ivr_fmq_assignment_t value;
    char causation_id[128];
    uint64_t room_version;
    uint64_t sequence;
    int event_published;
    int retry_pending;
    int worker_loss_applied;
    int worker_loss_event_published;
    ivr_fmq_attempt_record_t
        attempts[IVR_FMQ_ADAPTER_MAX_DISPATCH_ATTEMPTS];
    int valid;
} ivr_fmq_assignment_entry_t;

typedef struct {
    ivr_fmq_worker_snapshot_t value;
    ivr_fmq_worker_state_t post_reconcile_state;
    int lease_expiry_counted;
    int valid;
} ivr_fmq_worker_entry_t;

typedef enum {
    IVR_FMQ_DIALOG_OPENING = 1,
    IVR_FMQ_DIALOG_ACTIVE = 2,
    IVR_FMQ_DIALOG_CLOSING = 3,
    IVR_FMQ_DIALOG_WORKER_LOST = 4
} ivr_fmq_dialog_state_t;

/* Authoritative media-route fact owned by RoomService. Iris owns workflow
   state; this table only binds one typed media dialog to the worker instance
   that owns its media resources. */
typedef struct {
    char tenant_id[IVR_MEDIA_ID_CAPACITY];
    char provider_session_id[IVR_MEDIA_ID_CAPACITY];
    char dialog_id[IVR_MEDIA_ID_CAPACITY];
    char room_id[IVR_MEDIA_ID_CAPACITY];
    char call_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t call_generation;
    char worker_id[128];
    char worker_instance_id[128];
    uint64_t worker_connection_generation;
    char transition_command_id[128];
    ivr_media_command_kind_t pending_operation_kind;
    char pending_input_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t pending_input_generation;
    char active_input_id[IVR_MEDIA_ID_CAPACITY];
    uint64_t active_input_generation;
    char worker_lost_event_id[128];
    uint64_t worker_lost_occurred_at_ms;
    ivr_fmq_dialog_state_t state;
    int reconciliation_only;
    int valid;
} ivr_fmq_dialog_route_t;
typedef struct {
    char worker_id[128];
    char tenant_id[64];
    char room_scope[512];
    char call_scope[512];
    char content_capabilities[512];
    int valid;
} ivr_fmq_acl_entry_t;


struct ivr_fmq_adapter_s {
    turbo_room_service_t *service;
    ivr_room_bridge_t *bridge;
    ivr_fmq_call_seq_t *seq_entries;
    uint32_t seq_capacity;
    uint32_t seq_count;
    ivr_fmq_worker_entry_t *workers;
    uint32_t worker_capacity;
    uint32_t worker_count;
    uint32_t worker_high_water;
    uint32_t next_worker_index; /* round-robin dispatch cursor */
    uint32_t legacy_worker_max_sessions;
    uint64_t worker_lease_ms;
    uint64_t dispatch_deadline_ms;
    uint32_t dispatch_max_attempts;
    ivr_fmq_clock_ops_t clock;
    ivr_fmq_assignment_entry_t *assignments;
    uint32_t assignment_capacity;
    uint32_t assignment_count;
    uint32_t assignment_high_water;
    ivr_fmq_dialog_route_t *dialogs;
    uint32_t dialog_capacity;
    uint32_t dialog_count;
    uint32_t dialog_high_water;
    uint64_t lease_expired_total;
    uint64_t dispatch_timeout_total;
    uint64_t release_timeout_total;
    char content_package[64];   /* default package for dispatched calls */
    ivr_fmq_media_ops_t media;
    ivr_fmq_media_observer_ops_t media_observer;
    ivr_fmq_inventory_observer_ops_t inventory_observer;
    ivr_mutex_t seq_lock;
    ivr_fmq_acl_entry_t *acls;
    uint32_t acl_capacity;
    uint32_t acl_count;
    uint64_t dedup_retention_ms;
    uint64_t acl_rejects;
    int events_enabled;
    int started;
};

static ivr_fmq_worker_entry_t *ivr_fmq_worker_find_locked(
    ivr_fmq_adapter_t *a, const char *worker_id);
static int ivr_fmq_reconcile_required_locked(
    const ivr_fmq_adapter_t *adapter);
static uint64_t ivr_fmq_now_ms(const ivr_fmq_adapter_t *a);
static ivr_fmq_worker_state_t ivr_fmq_worker_effective_state(
    const ivr_fmq_worker_snapshot_t *worker, uint64_t now_ms);
static ivr_fmq_dialog_route_t *ivr_fmq_dialog_find_locked(
    ivr_fmq_adapter_t *adapter, const char *tenant_id,
    const char *provider_session_id,
    const char *dialog_id);
static int ivr_fmq_dialog_identity_matches(
    const ivr_fmq_dialog_route_t *route,
    const ivr_media_command_t *command);
static ivr_status_t ivr_fmq_dialog_prepare_command(
    ivr_fmq_adapter_t *adapter, const ivr_media_command_t *command,
    ivr_fmq_worker_snapshot_t *out_worker);
static void ivr_fmq_dialog_rollback_send(
    ivr_fmq_adapter_t *adapter, const ivr_media_command_t *command);
static void ivr_fmq_dialog_recover_worker_loss(
    ivr_fmq_adapter_t *adapter, uint32_t index, uint64_t now_ms);

static ivr_fmq_assignment_entry_t *ivr_fmq_assignment_find_locked(
    ivr_fmq_adapter_t *a, const char *message_id) {
    for (uint32_t i = 0; i < a->assignment_capacity; i++) {
        if (a->assignments[i].valid &&
            strcmp(a->assignments[i].value.message_id, message_id) == 0) {
            return &a->assignments[i];
        }
    }
    return NULL;
}

static ivr_fmq_assignment_entry_t *ivr_fmq_assignment_find_result_locked(
    ivr_fmq_adapter_t *a, const ivr_dispatch_result_t *result,
    ivr_fmq_attempt_record_t **out_attempt, int *out_is_current) {
    *out_attempt = NULL;
    *out_is_current = 0;
    if (result->wire_version != 2u) {
        return ivr_fmq_assignment_find_locked(a, result->message_id);
    }
    for (uint32_t i = 0; i < a->assignment_capacity; i++) {
        ivr_fmq_assignment_entry_t *entry = &a->assignments[i];
        if (!entry->valid ||
            strcmp(entry->value.assignment_id, result->assignment_id) != 0) {
            continue;
        }
        for (uint32_t j = 0; j < entry->value.attempt_count; j++) {
            ivr_fmq_attempt_record_t *attempt = &entry->attempts[j];
            if (strcmp(attempt->message_id, result->message_id) == 0 &&
                strcmp(attempt->attempt_id, result->attempt_id) == 0) {
                *out_attempt = attempt;
                *out_is_current =
                    strcmp(entry->value.attempt_id, result->attempt_id) == 0;
                return entry;
            }
        }
        return NULL;
    }
    return NULL;
}

static ivr_status_t ivr_fmq_assignment_reserve(
    ivr_fmq_adapter_t *a, const char *message_id,
    const ivr_fmq_worker_snapshot_t *worker,
    const ivr_room_command_t *command) {
    ivr_status_t rc = IVR_ENOSPC;
    uint64_t now_ms = ivr_fmq_now_ms(a);
    if (UINT64_MAX - now_ms < a->dispatch_deadline_ms) {
        return IVR_ESTATE;
    }
    ivr_mutex_lock(&a->seq_lock);
    if (ivr_fmq_assignment_find_locked(a, message_id)) {
        rc = IVR_OK;
    } else {
        ivr_fmq_assignment_entry_t *reusable = NULL;
        for (uint32_t i = 0; i < a->assignment_capacity; i++) {
            ivr_fmq_assignment_entry_t *entry = &a->assignments[i];
            if (entry->valid) {
                if (!reusable &&
                    entry->value.state == IVR_FMQ_ASSIGNMENT_REJECTED) {
                    reusable = entry;
                }
                continue;
            }
            reusable = entry;
            break;
        }
        if (reusable) {
            int was_valid = reusable->valid;
            memset(reusable, 0, sizeof(*reusable));
            reusable->valid = 1;
            snprintf(reusable->value.message_id,
                     sizeof(reusable->value.message_id), "%s", message_id);
            snprintf(reusable->value.assignment_id,
                     sizeof(reusable->value.assignment_id), "%s",
                     command->message_id);
            snprintf(reusable->value.attempt_id,
                     sizeof(reusable->value.attempt_id), "%s", message_id);
            reusable->value.attempt_count = 1;
            snprintf(reusable->value.worker_id,
                     sizeof(reusable->value.worker_id), "%s",
                     worker->worker_id);
            snprintf(reusable->value.room_id, sizeof(reusable->value.room_id),
                     "%s", command->room_id);
            snprintf(reusable->value.call_id, sizeof(reusable->value.call_id),
                     "%s", command->call_id);
            reusable->value.call_generation = command->call_generation;
            snprintf(reusable->value.worker_instance_id,
                     sizeof(reusable->value.worker_instance_id), "%s",
                     worker->instance_id);
            reusable->value.worker_connection_generation =
                worker->connection_generation;
            reusable->value.state = IVR_FMQ_ASSIGNMENT_PENDING;
            reusable->value.dispatch_deadline_at_ms =
                now_ms + a->dispatch_deadline_ms;
            snprintf(reusable->attempts[0].message_id,
                     sizeof(reusable->attempts[0].message_id), "%s",
                     message_id);
            snprintf(reusable->attempts[0].attempt_id,
                     sizeof(reusable->attempts[0].attempt_id), "%s",
                     message_id);
            snprintf(reusable->attempts[0].worker_id,
                     sizeof(reusable->attempts[0].worker_id), "%s",
                     worker->worker_id);
            snprintf(reusable->attempts[0].worker_instance_id,
                     sizeof(reusable->attempts[0].worker_instance_id), "%s",
                     worker->instance_id);
            reusable->attempts[0].worker_connection_generation =
                worker->connection_generation;
            if (!was_valid &&
                a->assignment_count < a->assignment_capacity) {
                a->assignment_count++;
                if (a->assignment_count > a->assignment_high_water) {
                    a->assignment_high_water = a->assignment_count;
                }
            }
            rc = IVR_OK;
        }
    }
    ivr_mutex_unlock(&a->seq_lock);
    return rc;
}

static ivr_fmq_assignment_entry_t *ivr_fmq_assignment_find_active_call_locked(
    ivr_fmq_adapter_t *a, const ivr_room_command_t *command) {
    for (uint32_t i = 0; i < a->assignment_capacity; i++) {
        ivr_fmq_assignment_entry_t *entry = &a->assignments[i];
        if (entry->valid &&
            entry->value.state == IVR_FMQ_ASSIGNMENT_ACCEPTED &&
            entry->value.call_generation == command->call_generation &&
            strcmp(entry->value.room_id, command->room_id) == 0 &&
            strcmp(entry->value.call_id, command->call_id) == 0) {
            return entry;
        }
    }
    return NULL;
}

static ivr_fmq_assignment_entry_t *ivr_fmq_assignment_find_call_locked(
    ivr_fmq_adapter_t *a, const ivr_room_command_t *command) {
    for (uint32_t i = 0; i < a->assignment_capacity; i++) {
        ivr_fmq_assignment_entry_t *entry = &a->assignments[i];
        if (entry->valid &&
            entry->value.call_generation == command->call_generation &&
            strcmp(entry->value.room_id, command->room_id) == 0 &&
            strcmp(entry->value.call_id, command->call_id) == 0) {
            return entry;
        }
    }
    return NULL;
}

static void ivr_fmq_worker_release_reservation(ivr_fmq_adapter_t *a,
                                                const char *worker_id,
                                                const char *instance_id,
                                                uint64_t generation) {
    ivr_mutex_lock(&a->seq_lock);
    ivr_fmq_worker_entry_t *worker =
        ivr_fmq_worker_find_locked(a, worker_id);
    if (worker && strcmp(worker->value.instance_id, instance_id) == 0 &&
        worker->value.connection_generation == generation &&
        worker->value.reserved_sessions > 0) {
        worker->value.reserved_sessions--;
    }
    ivr_mutex_unlock(&a->seq_lock);
}

static int ivr_fmq_assignment_active_for_call(
    ivr_fmq_adapter_t *a, const ivr_room_command_t *command,
    ivr_fmq_worker_snapshot_t *worker) {
    int found = 0;
    ivr_mutex_lock(&a->seq_lock);
    ivr_fmq_assignment_entry_t *assignment =
        ivr_fmq_assignment_find_active_call_locked(a, command);
    if (assignment) {
        ivr_fmq_worker_entry_t *entry = ivr_fmq_worker_find_locked(
            a, assignment->value.worker_id);
        if (entry && strcmp(entry->value.instance_id,
                            assignment->value.worker_instance_id) == 0 &&
            entry->value.connection_generation ==
                assignment->value.worker_connection_generation) {
            *worker = entry->value;
            found = 1;
        }
    }
    ivr_mutex_unlock(&a->seq_lock);
    return found;
}

ivr_status_t ivr_fmq_adapter_send_media_command(
    ivr_fmq_adapter_t *adapter, const ivr_media_command_t *command,
    char *out_worker_id, size_t out_worker_id_capacity) {
    ivr_fmq_worker_snapshot_t worker;
    ivr_media_command_t routed_command;
    size_t worker_id_size;

    if (!adapter || !command || !out_worker_id ||
        out_worker_id_capacity == 0 || command->worker_id[0] != '\0' ||
        command->message_id[0] == '\0' ||
        command->tenant_id[0] == '\0' ||
        command->provider_session_id[0] == '\0' ||
        command->dialog_id[0] == '\0' ||
        command->room_id[0] == '\0' || command->call_id[0] == '\0' ||
        command->call_generation == 0 || command->operation_generation == 0 ||
        command->deadline_timeout_ms == 0) {
        return IVR_EINVAL;
    }
    if (!adapter->bridge) {
        return IVR_ESTATE;
    }

    memset(&worker, 0, sizeof(worker));
    ivr_status_t status =
        ivr_fmq_dialog_prepare_command(adapter, command, &worker);
    if (status != IVR_OK) {
        return status;
    }
    if (!ivr_room_bridge_worker_available(adapter->bridge, worker.worker_id)) {
        ivr_fmq_dialog_rollback_send(adapter, command);
        return IVR_ESTATE;
    }
    worker_id_size = strlen(worker.worker_id);
    if (worker_id_size + 1 > out_worker_id_capacity) {
        ivr_fmq_dialog_rollback_send(adapter, command);
        return IVR_ENOSPC;
    }

    routed_command = *command;
    if (worker_id_size + 1 > sizeof(routed_command.worker_id)) {
        ivr_fmq_dialog_rollback_send(adapter, command);
        return IVR_ENOSPC;
    }
    memcpy(routed_command.worker_id, worker.worker_id, worker_id_size + 1);
    status = ivr_room_bridge_send_media_command(adapter->bridge,
                                                &routed_command);
    if (status != IVR_OK) {
        ivr_fmq_dialog_rollback_send(adapter, command);
        return status;
    }
    memcpy(out_worker_id, worker.worker_id, worker_id_size + 1);
    return IVR_OK;
}

ivr_status_t ivr_fmq_adapter_observe_media_command(
    const ivr_fmq_adapter_t *adapter, const ivr_media_command_t *command,
    iris_resource_observation_t *observation) {
    ivr_fmq_dialog_route_t *route;
    ivr_fmq_worker_entry_t *worker;
    uint64_t now_ms;
    if (!adapter || !command || !observation ||
        command->tenant_id[0] == '\0' ||
        command->provider_session_id[0] == '\0' ||
        command->dialog_id[0] == '\0' || command->room_id[0] == '\0' ||
        command->call_id[0] == '\0' || command->call_generation == 0u ||
        command->operation_generation == 0u) {
        return IVR_EINVAL;
    }
    memset(observation, 0, sizeof(*observation));
    now_ms = ivr_fmq_now_ms(adapter);
    ivr_mutex_lock((ivr_mutex_t *)&adapter->seq_lock);
    route = ivr_fmq_dialog_find_locked((ivr_fmq_adapter_t *)adapter,
                                       command->tenant_id,
                                       command->provider_session_id,
                                       command->dialog_id);
    if (!route) {
        observation->state =
            ivr_fmq_reconcile_required_locked(adapter)
                ? IRIS_RESOURCE_OBSERVATION_UNKNOWN
                : IRIS_RESOURCE_OBSERVATION_ABSENT;
        ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
        return IVR_OK;
    }
    if (!ivr_fmq_dialog_identity_matches(route, command)) {
        observation->state = IRIS_RESOURCE_OBSERVATION_ABSENT;
        ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
        return IVR_OK;
    }
    worker = ivr_fmq_worker_find_locked((ivr_fmq_adapter_t *)adapter,
                                        route->worker_id);
    if (!worker ||
        strcmp(worker->value.instance_id, route->worker_instance_id) != 0 ||
        worker->value.connection_generation !=
            route->worker_connection_generation ||
        ivr_fmq_worker_effective_state(&worker->value, now_ms) ==
            IVR_FMQ_WORKER_EXPIRED) {
        observation->state = IRIS_RESOURCE_OBSERVATION_UNKNOWN;
        ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
        return IVR_OK;
    }
    if ((command->kind == IVR_MEDIA_COMMAND_SESSION_OPEN &&
         (route->state == IVR_FMQ_DIALOG_ACTIVE ||
          (route->state == IVR_FMQ_DIALOG_OPENING &&
           strcmp(route->transition_command_id, command->message_id) == 0))) ||
        (command->kind == IVR_MEDIA_COMMAND_SESSION_CLOSE &&
         route->state == IVR_FMQ_DIALOG_CLOSING &&
         strcmp(route->transition_command_id, command->message_id) == 0)) {
        observation->state = IRIS_RESOURCE_OBSERVATION_ACTIVE;
        snprintf(observation->provider_resource_id,
                 sizeof(observation->provider_resource_id), "%s",
                 route->worker_id);
    } else if (command->kind == IVR_MEDIA_COMMAND_CANCEL) {
        if (route->active_input_generation == command->input_generation &&
            strcmp(route->active_input_id, command->input_id) == 0) {
            observation->state = IRIS_RESOURCE_OBSERVATION_ACTIVE;
            snprintf(observation->provider_resource_id,
                     sizeof(observation->provider_resource_id), "%s",
                     route->worker_id);
        } else {
            observation->state = IRIS_RESOURCE_OBSERVATION_ABSENT;
        }
    } else {
        observation->state = IRIS_RESOURCE_OBSERVATION_UNKNOWN;
    }
    ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
    return IVR_OK;
}

ivr_status_t ivr_fmq_adapter_request_inventory(
    ivr_fmq_adapter_t *adapter,
    const ivr_worker_inventory_request_t *request) {
    ivr_fmq_worker_snapshot_t worker;
    if (!adapter || !adapter->bridge || !request ||
        !request->message_id[0] || !request->worker_id[0] ||
        request->query.limit == 0 ||
        request->query.limit > IVR_WORKER_INVENTORY_MAX_PAGE_SIZE) {
        return IVR_EINVAL;
    }
    if (request->query.inventory_version !=
        IVR_WORKER_INVENTORY_VERSION) {
        return IVR_EVERSION;
    }
    if (!adapter->inventory_observer.on_inventory_page ||
        ivr_fmq_adapter_get_worker(adapter, request->worker_id, &worker) !=
            IVR_OK ||
        (worker.state != IVR_FMQ_WORKER_READY &&
         worker.state != IVR_FMQ_WORKER_DRAINING &&
         worker.state != IVR_FMQ_WORKER_RECONCILING) ||
        !ivr_room_bridge_worker_available(adapter->bridge,
                                          request->worker_id)) {
        return IVR_ESTATE;
    }
    return ivr_room_bridge_request_inventory(adapter->bridge, request);
}

static ivr_status_t ivr_fmq_assignment_prepare_leave(
    ivr_fmq_adapter_t *a, const ivr_room_command_t *command,
    const char *release_message_id, ivr_fmq_assignment_t *out,
    int *needs_release) {
    ivr_status_t rc = IVR_OK;
    *needs_release = 0;
    memset(out, 0, sizeof(*out));
    ivr_mutex_lock(&a->seq_lock);
    ivr_fmq_assignment_entry_t *assignment =
        ivr_fmq_assignment_find_call_locked(a, command);
    if (!assignment ||
        assignment->value.state == IVR_FMQ_ASSIGNMENT_REJECTED) {
        ivr_mutex_unlock(&a->seq_lock);
        return IVR_OK;
    }
    if (assignment->value.state == IVR_FMQ_ASSIGNMENT_PENDING) {
        ivr_fmq_worker_entry_t *worker = ivr_fmq_worker_find_locked(
            a, assignment->value.worker_id);
        if (worker &&
            strcmp(worker->value.instance_id,
                   assignment->value.worker_instance_id) == 0 &&
            worker->value.connection_generation ==
                assignment->value.worker_connection_generation &&
            worker->value.reserved_sessions > 0) {
            worker->value.reserved_sessions--;
        }
        assignment->value.state = IVR_FMQ_ASSIGNMENT_REJECTED;
        assignment->value.status_code = IVR_ECLOSED;
        snprintf(assignment->value.error_code,
                 sizeof(assignment->value.error_code),
                 "dispatch_cancelled");
        snprintf(assignment->value.error_message,
                 sizeof(assignment->value.error_message),
                 "conference.leave cancelled pending dispatch");
    } else if (assignment->value.state == IVR_FMQ_ASSIGNMENT_ACCEPTED) {
        uint64_t now_ms = ivr_fmq_now_ms(a);
        int written = snprintf(assignment->value.release_message_id,
                               sizeof(assignment->value.release_message_id),
                               "%s", release_message_id);
        if (written > 0 &&
            (size_t)written < sizeof(assignment->value.release_message_id) &&
            UINT64_MAX - now_ms >= a->dispatch_deadline_ms) {
            assignment->value.state = IVR_FMQ_ASSIGNMENT_RELEASING;
            assignment->value.release_deadline_at_ms =
                now_ms + a->dispatch_deadline_ms;
            *out = assignment->value;
            *needs_release = 1;
        } else {
            rc = IVR_ENOSPC;
        }
    } else {
        rc = IVR_ESTATE;
    }
    ivr_mutex_unlock(&a->seq_lock);
    return rc;
}

static void ivr_fmq_assignment_rollback_release(
    ivr_fmq_adapter_t *a, const char *release_message_id) {
    ivr_mutex_lock(&a->seq_lock);
    for (uint32_t i = 0; i < a->assignment_capacity; i++) {
        ivr_fmq_assignment_entry_t *entry = &a->assignments[i];
        if (entry->valid &&
            entry->value.state == IVR_FMQ_ASSIGNMENT_RELEASING &&
            strcmp(entry->value.release_message_id, release_message_id) == 0) {
            entry->value.state = IVR_FMQ_ASSIGNMENT_ACCEPTED;
            entry->value.release_message_id[0] = '\0';
            entry->value.release_deadline_at_ms = 0;
            break;
        }
    }
    ivr_mutex_unlock(&a->seq_lock);
}

static void ivr_fmq_assignment_remove(ivr_fmq_adapter_t *a,
                                       const char *message_id) {
    ivr_mutex_lock(&a->seq_lock);
    ivr_fmq_assignment_entry_t *entry =
        ivr_fmq_assignment_find_locked(a, message_id);
    if (entry) {
        memset(entry, 0, sizeof(*entry));
        if (a->assignment_count > 0) {
            a->assignment_count--;
        }
    }
    ivr_mutex_unlock(&a->seq_lock);
}

static ivr_status_t ivr_fmq_assignment_set_committed_fact(
    ivr_fmq_adapter_t *a, const char *message_id, const char *causation_id,
    uint64_t room_version, uint64_t sequence) {
    ivr_status_t rc = IVR_ESTATE;
    ivr_mutex_lock(&a->seq_lock);
    ivr_fmq_assignment_entry_t *entry =
        ivr_fmq_assignment_find_locked(a, message_id);
    if (entry && entry->value.state == IVR_FMQ_ASSIGNMENT_PENDING &&
        room_version > 0 && sequence > 0) {
        snprintf(entry->causation_id, sizeof(entry->causation_id), "%s",
                 causation_id);
        entry->room_version = room_version;
        entry->sequence = sequence;
        rc = IVR_OK;
    }
    ivr_mutex_unlock(&a->seq_lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/* aggregate helpers                                                   */
/* ------------------------------------------------------------------ */

static uint64_t ivr_fmq_room_version(turbo_room_service_t *service,
                                     const char *room_id) {
    turbo_room_summary_t summary;
    if (!service || !room_id ||
        turbo_room_service_get_room_summary(service, room_id, &summary) != 0) {
        return 0;
    }
    return (uint64_t)summary.version;
}

/* Maps the wire participant_role string to the aggregate role enum.
   Returns 0 for unknown/empty roles (fail fast, never guess). */
static turbo_participant_role_t ivr_fmq_role_for(const char *role) {
    if (!role || role[0] == '\0') {
        return (turbo_participant_role_t)0;
    }
    if (strcmp(role, "caller") == 0 || strcmp(role, "customer") == 0) {
        return TURBO_PARTICIPANT_ROLE_CUSTOMER;
    }
    if (strcmp(role, "agent") == 0) {
        return TURBO_PARTICIPANT_ROLE_AGENT;
    }
    if (strcmp(role, "bot") == 0) {
        return TURBO_PARTICIPANT_ROLE_BOT;
    }
    if (strcmp(role, "supervisor") == 0) {
        return TURBO_PARTICIPANT_ROLE_SUPERVISOR;
    }
    if (strcmp(role, "guest") == 0) {
        return TURBO_PARTICIPANT_ROLE_GUEST;
    }
    return (turbo_participant_role_t)0;
}

static int ivr_fmq_call_seq_matches(const ivr_fmq_call_seq_t *entry,
                                    const ivr_room_command_t *cmd) {
    return entry->valid && entry->call_generation == cmd->call_generation &&
           strcmp(entry->room_id, cmd->room_id) == 0 &&
           strcmp(entry->call_id, cmd->call_id) == 0;
}

static uint64_t ivr_fmq_seq_current(ivr_fmq_adapter_t *a,
                                    const ivr_room_command_t *cmd) {
    uint64_t seq = 0;
    ivr_mutex_lock(&a->seq_lock);
    for (uint32_t i = 0; i < a->seq_capacity; i++) {
        if (ivr_fmq_call_seq_matches(&a->seq_entries[i], cmd)) {
            seq = a->seq_entries[i].last_sequence;
            break;
        }
    }
    ivr_mutex_unlock(&a->seq_lock);
    return seq;
}

/* Advance the per-call sequence; returns the new value, or 0 when the bounded
   table is full (the caller fails fast instead of dropping continuity). */
static uint64_t ivr_fmq_seq_next(ivr_fmq_adapter_t *a,
                                 const ivr_room_command_t *cmd) {
    uint64_t seq = 0;
    ivr_mutex_lock(&a->seq_lock);
    ivr_fmq_call_seq_t *free_slot = NULL;
    for (uint32_t i = 0; i < a->seq_capacity; i++) {
        ivr_fmq_call_seq_t *entry = &a->seq_entries[i];
        if (ivr_fmq_call_seq_matches(entry, cmd)) {
            entry->last_sequence++;
            seq = entry->last_sequence;
            break;
        }
        if (!entry->valid && !free_slot) {
            free_slot = entry;
        }
    }
    if (seq == 0 && free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        snprintf(free_slot->room_id, sizeof(free_slot->room_id), "%s",
                 cmd->room_id);
        snprintf(free_slot->call_id, sizeof(free_slot->call_id), "%s",
                 cmd->call_id);
        free_slot->call_generation = cmd->call_generation;
        free_slot->last_sequence = 1;
        free_slot->valid = 1;
        a->seq_count++;
        seq = 1;
    }
    ivr_mutex_unlock(&a->seq_lock);
    return seq;
}

/* Roll back a sequence reserved by a command whose aggregate mutation was
   compensated before any domain event was published. The bridge serializes
   command handling, so no later command for this call can overtake it. */
static void ivr_fmq_seq_rollback(ivr_fmq_adapter_t *a,
                                 const ivr_room_command_t *cmd) {
    ivr_mutex_lock(&a->seq_lock);
    for (uint32_t i = 0; i < a->seq_capacity; i++) {
        ivr_fmq_call_seq_t *entry = &a->seq_entries[i];
        if (!ivr_fmq_call_seq_matches(entry, cmd)) {
            continue;
        }
        if (entry->last_sequence > 1) {
            entry->last_sequence--;
        } else {
            memset(entry, 0, sizeof(*entry));
            if (a->seq_count > 0) {
                a->seq_count--;
            }
        }
        break;
    }
    ivr_mutex_unlock(&a->seq_lock);
}

/* ------------------------------------------------------------------ */
/* authoritative bounded worker registry                              */
/* ------------------------------------------------------------------ */

static uint64_t ivr_fmq_now_ms(const ivr_fmq_adapter_t *a) {
    return a->clock.now_ms ? a->clock.now_ms(a->clock.context)
                           : turbo_monotonic_ms();
}

static ivr_fmq_worker_entry_t *ivr_fmq_worker_find_locked(
    ivr_fmq_adapter_t *a, const char *worker_id) {
    for (uint32_t i = 0; i < a->worker_capacity; i++) {
        if (a->workers[i].valid &&
            strcmp(a->workers[i].value.worker_id, worker_id) == 0) {
            return &a->workers[i];
        }
    }
    return NULL;
}

static ivr_fmq_worker_state_t ivr_fmq_worker_effective_state(
    const ivr_fmq_worker_snapshot_t *worker, uint64_t now_ms) {
    return worker->lease_expires_at_ms <= now_ms ? IVR_FMQ_WORKER_EXPIRED
                                                 : worker->state;
}

static int ivr_fmq_capability_has(const char *csv, const char *wanted) {
    const char *p = csv;
    size_t wanted_len;
    if (!csv || !wanted || wanted[0] == '\0') {
        return 0;
    }
    wanted_len = strlen(wanted);
    while (*p) {
        const char *end = strchr(p, ',');
        size_t token_len = end ? (size_t)(end - p) : strlen(p);
        if (token_len == wanted_len && memcmp(p, wanted, wanted_len) == 0) {
            return 1;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/* worker scope / content capability ACL (P0-04.4)                     */
/* ------------------------------------------------------------------ */

static ivr_fmq_acl_entry_t *ivr_fmq_acl_find_locked(ivr_fmq_adapter_t *a,
                                                     const char *worker_id) {
    uint32_t i;
    if (!a->acls || !worker_id) {
        return NULL;
    }
    for (i = 0u; i < a->acl_count; ++i) {
        if (a->acls[i].valid &&
            strcmp(a->acls[i].worker_id, worker_id) == 0) {
            return &a->acls[i];
        }
    }
    return NULL;
}

/* 1 when the worker is authorized for the room/call scope and the content
   package. With no configured ACL every registered worker stays allowed
   (legacy behavior); with an ACL, an unlisted worker is denied. */
static int ivr_fmq_acl_worker_allows(const ivr_fmq_adapter_t *a,
                                     const char *worker_id,
                                     const char *room_id,
                                     const char *call_id,
                                     const char *content_package) {
    const ivr_fmq_acl_entry_t *entry;
    if (a->acl_count == 0u) {
        return 1;
    }
    entry = ivr_fmq_acl_find_locked((ivr_fmq_adapter_t *)a, worker_id);
    if (!entry) {
        return 0;
    }
    if (!ivr_acl_tenant_allows(entry->tenant_id, room_id) ||
        !ivr_acl_scope_allows(entry->room_scope, room_id) ||
        !ivr_acl_scope_allows(entry->call_scope, call_id) ||
        !ivr_acl_scope_allows(entry->content_capabilities, content_package)) {
        return 0;
    }
    return 1;
}

/* Authorize one room-scoped command. Fills *result with IVR_EAUTH and an
   explicit reason when denied; the authoritative room version is never
   advanced by a denied command. Returns 1 when authorized. */
static int ivr_fmq_adapter_authorize(ivr_fmq_adapter_t *a,
                                     const ivr_room_command_t *command,
                                     ivr_room_command_result_t *result) {
    int allowed;
    if (a->acl_count == 0u) {
        return 1;
    }
    if (!command->worker_id[0]) {
        result->status_code = IVR_EAUTH;
        snprintf(result->error_message, sizeof(result->error_message),
                 "worker identity missing");
        a->acl_rejects++;
        return 0;
    }
    allowed = ivr_fmq_acl_worker_allows(
        a, command->worker_id, command->room_id, command->call_id,
        a->content_package);
    if (!allowed) {
        result->status_code = IVR_EAUTH;
        snprintf(result->error_message, sizeof(result->error_message),
                 "worker %s not authorized for room/call scope",
                 command->worker_id);
        a->acl_rejects++;
        return 0;
    }
    return 1;
}

/* 1 when the worker registry record satisfies the ACL for a dispatch. */
static int ivr_fmq_acl_worker_allows_snapshot(
    const ivr_fmq_adapter_t *a, const ivr_fmq_worker_snapshot_t *worker,
    const char *room_id, const char *call_id, const char *content_package) {
    return ivr_fmq_acl_worker_allows(a, worker->worker_id, room_id, call_id,
                                     content_package);
}
static int ivr_fmq_worker_health_ready(
    const ivr_fmq_worker_snapshot_t *worker) {
    /* V1 remains usable for compatibility/shadow deployments. V2 must
       explicitly prove that its active dependencies are ready. */
    return worker->protocol_version != 2u ||
           (worker->health_generation != 0
                ? worker->health_ready != 0
                : ivr_fmq_capability_has(worker->capabilities, "health.ready"));
}

static int ivr_fmq_worker_command_is_v2(const ivr_room_command_t *command) {
    return strcmp(command->command, "worker.sync.v2") == 0 ||
           strcmp(command->command, "worker.heartbeat") == 0;
}

static ivr_status_t ivr_fmq_worker_command_validate(
    const ivr_fmq_adapter_t *a, const ivr_room_command_t *command) {
    if (!command->worker_id[0]) {
        return IVR_EINVAL;
    }
    if (!ivr_fmq_worker_command_is_v2(command)) {
        return strcmp(command->command, "worker.sync") == 0 ? IVR_OK
                                                             : IVR_EINVAL;
    }
    if (!command->instance_id[0] || command->connection_generation == 0 ||
        command->max_sessions == 0 || command->lease_duration_ms == 0 ||
        command->lease_duration_ms != a->worker_lease_ms ||
        command->active_sessions > command->max_sessions ||
        command->reserved_sessions != 0 ||
        (command->draining != 0 && command->draining != 1) ||
        (command->health_generation != 0 &&
         command->health_ready != 0 && command->health_ready != 1)) {
        return IVR_EINVAL;
    }
    return IVR_OK;
}

static void ivr_fmq_worker_fill_from_command(
    ivr_fmq_adapter_t *a, ivr_fmq_worker_snapshot_t *worker,
    const ivr_room_command_t *command, uint64_t now_ms) {
    memset(worker, 0, sizeof(*worker));
    snprintf(worker->worker_id, sizeof(worker->worker_id), "%s",
             command->worker_id);
    worker->protocol_version =
        ivr_fmq_worker_command_is_v2(command) ? 2u : 1u;
    if (ivr_fmq_worker_command_is_v2(command)) {
        snprintf(worker->instance_id, sizeof(worker->instance_id), "%s",
                 command->instance_id);
        worker->connection_generation = command->connection_generation;
        worker->max_sessions = command->max_sessions;
        worker->active_sessions = command->active_sessions;
        worker->reserved_sessions = command->reserved_sessions;
        worker->health_generation = command->health_generation;
        worker->health_ready = command->health_ready;
        snprintf(worker->capabilities, sizeof(worker->capabilities), "%s",
                 command->capabilities);
        worker->state = command->draining
                            ? IVR_FMQ_WORKER_DRAINING
                            : (ivr_fmq_worker_health_ready(worker)
                                   ? IVR_FMQ_WORKER_READY
                                   : IVR_FMQ_WORKER_SYNCED);
    } else {
        snprintf(worker->instance_id, sizeof(worker->instance_id),
                 "legacy:%s", command->worker_id);
        worker->connection_generation = 1;
        worker->max_sessions = a->legacy_worker_max_sessions;
        worker->state = IVR_FMQ_WORKER_READY;
        snprintf(worker->capabilities, sizeof(worker->capabilities), "%s",
                 "legacy-v1");
    }
    worker->lease_expires_at_ms = now_ms + a->worker_lease_ms;
}

static void ivr_fmq_worker_require_reconcile(
    ivr_fmq_worker_entry_t *entry) {
    entry->post_reconcile_state = entry->value.state;
    entry->value.state = IVR_FMQ_WORKER_RECONCILING;
    entry->value.requires_reconcile = 1;
}

static ivr_status_t ivr_fmq_adapter_register_worker(
    ivr_fmq_adapter_t *a, const ivr_room_command_t *command) {
    ivr_status_t rc = ivr_fmq_worker_command_validate(a, command);
    uint64_t now_ms;
    if (rc != IVR_OK) {
        return rc;
    }
    now_ms = ivr_fmq_now_ms(a);
    if (UINT64_MAX - now_ms < a->worker_lease_ms) {
        return IVR_ESTATE;
    }
    ivr_mutex_lock(&a->seq_lock);
    ivr_fmq_worker_entry_t *entry =
        ivr_fmq_worker_find_locked(a, command->worker_id);
    if (strcmp(command->command, "worker.heartbeat") == 0) {
        if (!entry || strcmp(entry->value.instance_id, command->instance_id) != 0 ||
            entry->value.connection_generation !=
                command->connection_generation ||
            entry->value.max_sessions != command->max_sessions) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_EVERSION;
        }
        entry->value.active_sessions = command->active_sessions;
        entry->value.health_generation = command->health_generation;
        entry->value.health_ready = command->health_ready;
        entry->value.lease_expires_at_ms = now_ms + a->worker_lease_ms;
        entry->lease_expiry_counted = 0;
        snprintf(entry->value.capabilities,
                 sizeof(entry->value.capabilities), "%s",
                 command->capabilities);
        entry->value.state = command->draining
                                 ? IVR_FMQ_WORKER_DRAINING
                                 : (ivr_fmq_worker_health_ready(&entry->value)
                                        ? IVR_FMQ_WORKER_READY
                                         : IVR_FMQ_WORKER_SYNCED);
        entry->post_reconcile_state = entry->value.state;
        if (entry->value.requires_reconcile) {
            entry->value.state = IVR_FMQ_WORKER_RECONCILING;
        }
        ivr_mutex_unlock(&a->seq_lock);
        return IVR_OK;
    }
    if (entry) {
        int same_generation =
            strcmp(entry->value.instance_id,
                   ivr_fmq_worker_command_is_v2(command)
                       ? command->instance_id
                       : entry->value.instance_id) == 0 &&
            (!ivr_fmq_worker_command_is_v2(command) ||
             entry->value.connection_generation ==
                 command->connection_generation);
        if (same_generation) {
            if (ivr_fmq_worker_command_is_v2(command) &&
                entry->value.max_sessions != command->max_sessions) {
                ivr_mutex_unlock(&a->seq_lock);
                return IVR_EVERSION;
            }
            if (ivr_fmq_worker_command_is_v2(command)) {
                entry->value.active_sessions = command->active_sessions;
                entry->value.health_generation = command->health_generation;
                entry->value.health_ready = command->health_ready;
                snprintf(entry->value.capabilities,
                         sizeof(entry->value.capabilities), "%s",
                         command->capabilities);
                entry->value.state = command->draining
                                         ? IVR_FMQ_WORKER_DRAINING
                                         : (ivr_fmq_worker_health_ready(
                                                &entry->value)
                                                ? IVR_FMQ_WORKER_READY
                                                 : IVR_FMQ_WORKER_SYNCED);
                entry->post_reconcile_state = entry->value.state;
                if (entry->value.requires_reconcile) {
                    entry->value.state = IVR_FMQ_WORKER_RECONCILING;
                }
            } else {
                entry->value.state = command->draining
                                         ? IVR_FMQ_WORKER_DRAINING
                                         : IVR_FMQ_WORKER_READY;
            }
            entry->value.lease_expires_at_ms = now_ms + a->worker_lease_ms;
            entry->lease_expiry_counted = 0;
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_OK;
        }
        if (entry->value.active_sessions != 0 ||
            entry->value.reserved_sessions != 0 ||
            (ivr_fmq_worker_effective_state(&entry->value, now_ms) !=
                 IVR_FMQ_WORKER_EXPIRED &&
             (!a->bridge || ivr_room_bridge_worker_available(
                                a->bridge, command->worker_id)))) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_ESTATE;
        }
        ivr_fmq_worker_fill_from_command(a, &entry->value, command, now_ms);
        entry->post_reconcile_state = entry->value.state;
        if (ivr_fmq_worker_command_is_v2(command) &&
            command->active_sessions != 0) {
            ivr_fmq_worker_require_reconcile(entry);
        }
        entry->lease_expiry_counted = 0;
        ivr_mutex_unlock(&a->seq_lock);
        return IVR_OK;
    }
    for (uint32_t i = 0; i < a->worker_capacity; i++) {
        if (!a->workers[i].valid ||
            (a->workers[i].value.active_sessions == 0 &&
             a->workers[i].value.reserved_sessions == 0 &&
             (ivr_fmq_worker_effective_state(&a->workers[i].value, now_ms) ==
                  IVR_FMQ_WORKER_EXPIRED ||
              (a->bridge && !ivr_room_bridge_worker_available(
                                a->bridge,
                                a->workers[i].value.worker_id))))) {
            int was_valid = a->workers[i].valid;
            a->workers[i].valid = 1;
            ivr_fmq_worker_fill_from_command(a, &a->workers[i].value, command,
                                             now_ms);
            a->workers[i].post_reconcile_state =
                a->workers[i].value.state;
            if (ivr_fmq_worker_command_is_v2(command) &&
                command->active_sessions != 0) {
                ivr_fmq_worker_require_reconcile(&a->workers[i]);
            }
            a->workers[i].lease_expiry_counted = 0;
            if (!was_valid) {
                a->worker_count++;
                if (a->worker_count > a->worker_high_water) {
                    a->worker_high_water = a->worker_count;
                }
            }
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_OK;
        }
    }
    ivr_mutex_unlock(&a->seq_lock);
    return IVR_ENOSPC;
}

ivr_status_t ivr_fmq_adapter_get_worker(
    const ivr_fmq_adapter_t *adapter, const char *worker_id,
    ivr_fmq_worker_snapshot_t *out) {
    if (!adapter || !worker_id || !out) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock((ivr_mutex_t *)&adapter->seq_lock);
    const ivr_fmq_worker_entry_t *entry = ivr_fmq_worker_find_locked(
        (ivr_fmq_adapter_t *)adapter, worker_id);
    if (!entry) {
        ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
        return IVR_ESTATE;
    }
    *out = entry->value;
    out->state = ivr_fmq_worker_effective_state(out, ivr_fmq_now_ms(adapter));
    ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
    return IVR_OK;
}

ivr_status_t ivr_fmq_adapter_list_workers(
    const ivr_fmq_adapter_t *adapter, ivr_fmq_worker_snapshot_t *out_workers,
    uint32_t capacity, uint32_t *out_count, uint32_t *out_total) {
    uint32_t count = 0;
    uint64_t now_ms;
    if (!adapter || !out_count || !out_total ||
        (capacity != 0 && !out_workers)) {
        return IVR_EINVAL;
    }
    *out_count = 0;
    *out_total = 0;
    now_ms = ivr_fmq_now_ms(adapter);
    ivr_mutex_lock((ivr_mutex_t *)&adapter->seq_lock);
    for (uint32_t i = 0; i < adapter->worker_capacity; ++i) {
        if (adapter->workers[i].valid) ++count;
    }
    *out_total = count;
    if (count > capacity) {
        ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
        return IVR_ENOSPC;
    }
    count = 0;
    for (uint32_t i = 0; i < adapter->worker_capacity; ++i) {
        if (!adapter->workers[i].valid) continue;
        out_workers[count] = adapter->workers[i].value;
        out_workers[count].state =
            ivr_fmq_worker_effective_state(&out_workers[count], now_ms);
        ++count;
    }
    ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
    *out_count = count;
    return IVR_OK;
}

int ivr_fmq_adapter_reconcile_required(
    const ivr_fmq_adapter_t *adapter) {
    int required;
    if (!adapter) return 1;
    ivr_mutex_lock((ivr_mutex_t *)&adapter->seq_lock);
    required = ivr_fmq_reconcile_required_locked(adapter);
    ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
    return required;
}

static int ivr_fmq_reconcile_required_locked(
    const ivr_fmq_adapter_t *adapter) {
    for (uint32_t i = 0; i < adapter->worker_capacity; ++i) {
        if (adapter->workers[i].valid &&
            adapter->workers[i].value.requires_reconcile) {
            return 1;
        }
    }
    return 0;
}

ivr_status_t ivr_fmq_adapter_rebind_dialog(
    ivr_fmq_adapter_t *adapter,
    const ivr_worker_inventory_record_t *record) {
    ivr_fmq_dialog_route_t *free_route = NULL;
    ivr_fmq_worker_entry_t *worker;
    uint32_t bound_count = 0;
    uint64_t now_ms;
    if (!adapter || !record || !record->worker_id[0] ||
        !record->worker_instance_id[0] || record->worker_epoch == 0 ||
        !record->provider_session_id[0] || !record->dialog_id[0] ||
        !record->room_id[0] || !record->call_id[0] ||
        record->call_generation == 0 ||
        record->state != IVR_WORKER_RESOURCE_ACTIVE || !record->rebindable) {
        return IVR_EINVAL;
    }
    now_ms = ivr_fmq_now_ms(adapter);
    ivr_mutex_lock(&adapter->seq_lock);
    worker = ivr_fmq_worker_find_locked(adapter, record->worker_id);
    if (!worker || !worker->value.requires_reconcile ||
        worker->value.state != IVR_FMQ_WORKER_RECONCILING ||
        ivr_fmq_worker_effective_state(&worker->value, now_ms) ==
            IVR_FMQ_WORKER_EXPIRED ||
        strcmp(worker->value.instance_id, record->worker_instance_id) != 0 ||
        worker->value.connection_generation != record->worker_epoch ||
        !ivr_fmq_acl_worker_allows_snapshot(
            adapter, &worker->value, record->room_id, record->call_id,
            adapter->content_package)) {
        ivr_mutex_unlock(&adapter->seq_lock);
        return IVR_EVERSION;
    }
    for (uint32_t i = 0; i < adapter->dialog_capacity; ++i) {
        ivr_fmq_dialog_route_t *route = &adapter->dialogs[i];
        if (!route->valid) {
            if (!free_route) free_route = route;
            continue;
        }
        if (strcmp(route->worker_id, record->worker_id) == 0 &&
            strcmp(route->worker_instance_id, record->worker_instance_id) ==
                0 &&
            route->worker_connection_generation == record->worker_epoch) {
            ++bound_count;
        }
        if (strcmp(route->tenant_id, record->tenant_id) == 0 &&
            strcmp(route->provider_session_id,
                   record->provider_session_id) == 0 &&
            strcmp(route->dialog_id, record->dialog_id) == 0) {
            int exact = route->state == IVR_FMQ_DIALOG_ACTIVE &&
                        route->call_generation == record->call_generation &&
                        strcmp(route->room_id, record->room_id) == 0 &&
                        strcmp(route->call_id, record->call_id) == 0 &&
                        strcmp(route->worker_id, record->worker_id) == 0 &&
                        strcmp(route->worker_instance_id,
                               record->worker_instance_id) == 0 &&
                        route->worker_connection_generation ==
                            record->worker_epoch;
            if (exact) {
                snprintf(route->active_input_id,
                         sizeof(route->active_input_id), "%s",
                         record->input_id);
                route->active_input_generation =
                    record->input_generation;
            }
            ivr_mutex_unlock(&adapter->seq_lock);
            return exact ? IVR_OK : IVR_EVERSION;
        }
        if (route->call_generation == record->call_generation &&
            strcmp(route->room_id, record->room_id) == 0 &&
            strcmp(route->call_id, record->call_id) == 0) {
            ivr_mutex_unlock(&adapter->seq_lock);
            return IVR_EVERSION;
        }
    }
    if (!free_route || bound_count >= worker->value.active_sessions) {
        ivr_mutex_unlock(&adapter->seq_lock);
        return IVR_ENOSPC;
    }
    memset(free_route, 0, sizeof(*free_route));
    snprintf(free_route->tenant_id, sizeof(free_route->tenant_id), "%s",
             record->tenant_id);
    snprintf(free_route->provider_session_id,
             sizeof(free_route->provider_session_id), "%s",
             record->provider_session_id);
    snprintf(free_route->dialog_id, sizeof(free_route->dialog_id), "%s",
             record->dialog_id);
    snprintf(free_route->room_id, sizeof(free_route->room_id), "%s",
             record->room_id);
    snprintf(free_route->call_id, sizeof(free_route->call_id), "%s",
             record->call_id);
    free_route->call_generation = record->call_generation;
    snprintf(free_route->worker_id, sizeof(free_route->worker_id), "%s",
             record->worker_id);
    snprintf(free_route->worker_instance_id,
             sizeof(free_route->worker_instance_id), "%s",
             record->worker_instance_id);
    free_route->worker_connection_generation = record->worker_epoch;
    snprintf(free_route->active_input_id,
             sizeof(free_route->active_input_id), "%s", record->input_id);
    free_route->active_input_generation = record->input_generation;
    free_route->state = IVR_FMQ_DIALOG_ACTIVE;
    free_route->valid = 1;
    ++adapter->dialog_count;
    if (adapter->dialog_count > adapter->dialog_high_water) {
        adapter->dialog_high_water = adapter->dialog_count;
    }
    ivr_mutex_unlock(&adapter->seq_lock);
    return IVR_OK;
}

ivr_status_t ivr_fmq_adapter_complete_worker_reconcile(
    ivr_fmq_adapter_t *adapter, const char *worker_id,
    const char *worker_instance_id, uint64_t worker_epoch) {
    ivr_fmq_worker_entry_t *worker;
    uint32_t bound_count = 0;
    if (!adapter || !worker_id || !worker_id[0] || !worker_instance_id ||
        !worker_instance_id[0] || worker_epoch == 0) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&adapter->seq_lock);
    worker = ivr_fmq_worker_find_locked(adapter, worker_id);
    if (!worker || !worker->value.requires_reconcile ||
        worker->value.state != IVR_FMQ_WORKER_RECONCILING ||
        strcmp(worker->value.instance_id, worker_instance_id) != 0 ||
        worker->value.connection_generation != worker_epoch) {
        ivr_mutex_unlock(&adapter->seq_lock);
        return IVR_EVERSION;
    }
    for (uint32_t i = 0; i < adapter->dialog_capacity; ++i) {
        const ivr_fmq_dialog_route_t *route = &adapter->dialogs[i];
        if (route->valid && strcmp(route->worker_id, worker_id) == 0 &&
            strcmp(route->worker_instance_id, worker_instance_id) == 0 &&
            route->worker_connection_generation == worker_epoch) {
            ++bound_count;
        }
    }
    if (bound_count != worker->value.active_sessions ||
        worker->value.reserved_sessions != 0) {
        ivr_mutex_unlock(&adapter->seq_lock);
        return IVR_ESTATE;
    }
    worker->value.requires_reconcile = 0;
    worker->value.state = worker->post_reconcile_state;
    ivr_mutex_unlock(&adapter->seq_lock);
    return IVR_OK;
}

ivr_status_t ivr_fmq_adapter_close_orphan(
    ivr_fmq_adapter_t *adapter,
    const ivr_worker_inventory_record_t *record, const char *message_id,
    uint64_t deadline_timeout_ms) {
    ivr_worker_inventory_record_t adopted;
    ivr_media_command_t command;
    ivr_fmq_dialog_route_t *route;
    char worker_id[128];
    ivr_status_t status;
    if (!adapter || !record || !message_id || !message_id[0] ||
        deadline_timeout_ms == 0 ||
        record->operation_generation == UINT64_MAX) {
        return IVR_EINVAL;
    }
    adopted = *record;
    adopted.state = IVR_WORKER_RESOURCE_ACTIVE;
    adopted.rebindable = 1;
    status = ivr_fmq_adapter_rebind_dialog(adapter, &adopted);
    if (status != IVR_OK) return status;

    ivr_mutex_lock(&adapter->seq_lock);
    route = ivr_fmq_dialog_find_locked(
        adapter, record->tenant_id, record->provider_session_id,
        record->dialog_id);
    if (!route || route->state != IVR_FMQ_DIALOG_ACTIVE ||
        strcmp(route->worker_id, record->worker_id) != 0 ||
        strcmp(route->worker_instance_id, record->worker_instance_id) != 0 ||
        route->worker_connection_generation != record->worker_epoch) {
        ivr_mutex_unlock(&adapter->seq_lock);
        return IVR_EVERSION;
    }
    route->reconciliation_only = 1;
    ivr_mutex_unlock(&adapter->seq_lock);

    memset(&command, 0, sizeof(command));
    snprintf(command.message_id, sizeof(command.message_id), "%s", message_id);
    snprintf(command.tenant_id, sizeof(command.tenant_id), "%s",
             record->tenant_id);
    snprintf(command.provider_session_id, sizeof(command.provider_session_id),
             "%s", record->provider_session_id);
    snprintf(command.dialog_id, sizeof(command.dialog_id), "%s",
             record->dialog_id);
    snprintf(command.room_id, sizeof(command.room_id), "%s", record->room_id);
    snprintf(command.call_id, sizeof(command.call_id), "%s", record->call_id);
    command.call_generation = record->call_generation;
    command.operation_generation = record->operation_generation + 1u;
    command.deadline_timeout_ms = deadline_timeout_ms;
    command.kind = IVR_MEDIA_COMMAND_SESSION_CLOSE;
    return ivr_fmq_adapter_send_media_command(
        adapter, &command, worker_id, sizeof(worker_id));
}

int ivr_fmq_adapter_worker_registered(const ivr_fmq_adapter_t *adapter,
                                      const char *worker_id) {
    ivr_fmq_worker_snapshot_t worker;
    if (ivr_fmq_adapter_get_worker(adapter, worker_id, &worker) != IVR_OK ||
        (worker.state != IVR_FMQ_WORKER_READY &&
         worker.state != IVR_FMQ_WORKER_DRAINING)) {
        return 0;
    }
    return !adapter->bridge ||
           ivr_room_bridge_worker_available(adapter->bridge, worker_id);
}

static int ivr_fmq_adapter_pick_worker(ivr_fmq_adapter_t *a,
                                       const char *room_id,
                                       const char *call_id,
                                       const char *content_package,
                                       ivr_fmq_worker_snapshot_t *out) {
    uint64_t now_ms = ivr_fmq_now_ms(a);
    ivr_mutex_lock(&a->seq_lock);
    if (a->worker_count == 0) {
        ivr_mutex_unlock(&a->seq_lock);
        return 0;
    }
    uint32_t start = a->next_worker_index % a->worker_capacity;
    for (uint32_t offset = 0; offset < a->worker_capacity; offset++) {
        uint32_t idx = (start + offset) % a->worker_capacity;
        ivr_fmq_worker_entry_t *entry = &a->workers[idx];
        if (!entry->valid ||
            ivr_fmq_worker_effective_state(&entry->value, now_ms) !=
                IVR_FMQ_WORKER_READY ||
            !ivr_fmq_worker_health_ready(&entry->value) ||
            entry->value.active_sessions + entry->value.reserved_sessions >=
                entry->value.max_sessions ||
            !ivr_fmq_acl_worker_allows_snapshot(
                a, &entry->value, room_id, call_id, content_package) ||
            (a->bridge && !ivr_room_bridge_worker_available(
                              a->bridge, entry->value.worker_id))) {
            continue;
        }
        entry->value.reserved_sessions++;
        *out = entry->value;
        a->next_worker_index = (idx + 1) % a->worker_capacity;
        ivr_mutex_unlock(&a->seq_lock);
        return 1;
    }
    ivr_mutex_unlock(&a->seq_lock);
    return 0;
}

static ivr_fmq_dialog_route_t *ivr_fmq_dialog_find_locked(
    ivr_fmq_adapter_t *adapter, const char *tenant_id,
    const char *provider_session_id,
    const char *dialog_id) {
    for (uint32_t i = 0; i < adapter->dialog_capacity; ++i) {
        ivr_fmq_dialog_route_t *route = &adapter->dialogs[i];
        if (route->valid && strcmp(route->tenant_id, tenant_id) == 0 &&
            strcmp(route->provider_session_id, provider_session_id) == 0 &&
            strcmp(route->dialog_id, dialog_id) == 0) {
            return route;
        }
    }
    return NULL;
}

static int ivr_fmq_dialog_identity_matches(
    const ivr_fmq_dialog_route_t *route,
    const ivr_media_command_t *command) {
    return route->call_generation == command->call_generation &&
           strcmp(route->room_id, command->room_id) == 0 &&
           strcmp(route->call_id, command->call_id) == 0;
}

static ivr_status_t ivr_fmq_dialog_reserve_open_locked(
    ivr_fmq_adapter_t *adapter, const ivr_media_command_t *command,
    ivr_fmq_worker_snapshot_t *out_worker) {
    ivr_fmq_dialog_route_t *route = NULL;
    uint64_t now_ms = ivr_fmq_now_ms(adapter);
    uint32_t start;

    if (ivr_fmq_dialog_find_locked(adapter, command->tenant_id,
                                   command->provider_session_id,
                                   command->dialog_id)) {
        return IVR_ESTATE;
    }
    for (uint32_t i = 0; i < adapter->dialog_capacity; ++i) {
        if (!adapter->dialogs[i].valid) {
            route = &adapter->dialogs[i];
            break;
        }
    }
    if (!route || adapter->worker_count == 0) {
        return IVR_ENOSPC;
    }

    start = adapter->next_worker_index % adapter->worker_capacity;
    for (uint32_t offset = 0; offset < adapter->worker_capacity; ++offset) {
        uint32_t index = (start + offset) % adapter->worker_capacity;
        ivr_fmq_worker_entry_t *worker = &adapter->workers[index];
        if (!worker->valid || worker->value.protocol_version != 2u ||
            ivr_fmq_worker_effective_state(&worker->value, now_ms) !=
                IVR_FMQ_WORKER_READY ||
            !ivr_fmq_worker_health_ready(&worker->value) ||
            worker->value.active_sessions + worker->value.reserved_sessions >=
                worker->value.max_sessions ||
            !ivr_fmq_acl_worker_allows_snapshot(
                adapter, &worker->value, command->room_id, command->call_id,
                adapter->content_package) ||
            (adapter->bridge && !ivr_room_bridge_worker_available(
                                    adapter->bridge,
                                    worker->value.worker_id))) {
            continue;
        }

        memset(route, 0, sizeof(*route));
        snprintf(route->tenant_id, sizeof(route->tenant_id), "%s",
                 command->tenant_id);
        snprintf(route->provider_session_id,
                 sizeof(route->provider_session_id), "%s",
                 command->provider_session_id);
        snprintf(route->dialog_id, sizeof(route->dialog_id), "%s",
                 command->dialog_id);
        snprintf(route->room_id, sizeof(route->room_id), "%s",
                 command->room_id);
        snprintf(route->call_id, sizeof(route->call_id), "%s",
                 command->call_id);
        route->call_generation = command->call_generation;
        snprintf(route->worker_id, sizeof(route->worker_id), "%s",
                 worker->value.worker_id);
        snprintf(route->worker_instance_id,
                 sizeof(route->worker_instance_id), "%s",
                 worker->value.instance_id);
        route->worker_connection_generation =
            worker->value.connection_generation;
        snprintf(route->transition_command_id,
                 sizeof(route->transition_command_id), "%s",
                 command->message_id);
        route->state = IVR_FMQ_DIALOG_OPENING;
        route->valid = 1;
        worker->value.reserved_sessions++;
        adapter->dialog_count++;
        if (adapter->dialog_count > adapter->dialog_high_water) {
            adapter->dialog_high_water = adapter->dialog_count;
        }
        adapter->next_worker_index =
            (index + 1u) % adapter->worker_capacity;
        *out_worker = worker->value;
        return IVR_OK;
    }
    return IVR_ESTATE;
}

static ivr_status_t ivr_fmq_dialog_prepare_command(
    ivr_fmq_adapter_t *adapter, const ivr_media_command_t *command,
    ivr_fmq_worker_snapshot_t *out_worker) {
    ivr_status_t status = IVR_OK;
    uint64_t now_ms = ivr_fmq_now_ms(adapter);

    ivr_mutex_lock(&adapter->seq_lock);
    if (command->kind == IVR_MEDIA_COMMAND_SESSION_OPEN) {
        ivr_fmq_dialog_route_t *route = ivr_fmq_dialog_find_locked(
            adapter, command->tenant_id, command->provider_session_id,
            command->dialog_id);
        if (!route) {
            status = ivr_fmq_dialog_reserve_open_locked(adapter, command,
                                                        out_worker);
        } else {
            ivr_fmq_worker_entry_t *worker =
                ivr_fmq_worker_find_locked(adapter, route->worker_id);
            ivr_fmq_worker_state_t worker_state =
                worker ? ivr_fmq_worker_effective_state(&worker->value, now_ms)
                       : IVR_FMQ_WORKER_EXPIRED;
            if (route->state != IVR_FMQ_DIALOG_ACTIVE ||
                !ivr_fmq_dialog_identity_matches(route, command) || !worker ||
                strcmp(worker->value.instance_id,
                       route->worker_instance_id) != 0 ||
                worker->value.connection_generation !=
                    route->worker_connection_generation ||
                (worker_state != IVR_FMQ_WORKER_READY &&
                 worker_state != IVR_FMQ_WORKER_DRAINING)) {
                status = IVR_EVERSION;
            } else {
                *out_worker = worker->value;
            }
        }
    } else {
        ivr_fmq_dialog_route_t *route = ivr_fmq_dialog_find_locked(
            adapter, command->tenant_id, command->provider_session_id,
            command->dialog_id);
        ivr_fmq_worker_entry_t *worker =
            route ? ivr_fmq_worker_find_locked(adapter, route->worker_id)
                  : NULL;
        if (!route) {
            status = (command->kind == IVR_MEDIA_COMMAND_SESSION_CLOSE ||
                      command->kind == IVR_MEDIA_COMMAND_CANCEL)
                         ? IVR_ENOTFOUND
                         : IVR_ESTATE;
        } else if (!ivr_fmq_dialog_identity_matches(route, command)) {
            status = IVR_ESTALE;
        } else if (route->state != IVR_FMQ_DIALOG_ACTIVE) {
            status = IVR_ESTATE;
        } else if (command->kind == IVR_MEDIA_COMMAND_CANCEL &&
                   route->active_input_id[0] == '\0') {
            status = IVR_ENOTFOUND;
        } else if (command->kind == IVR_MEDIA_COMMAND_CANCEL &&
                   (route->active_input_generation !=
                        command->input_generation ||
                    strcmp(route->active_input_id, command->input_id) != 0)) {
            status = IVR_ESTALE;
        } else if (!worker ||
                   strcmp(worker->value.instance_id,
                          route->worker_instance_id) != 0 ||
                   worker->value.connection_generation !=
                       route->worker_connection_generation ||
                    (ivr_fmq_worker_effective_state(&worker->value, now_ms) !=
                         IVR_FMQ_WORKER_READY &&
                     ivr_fmq_worker_effective_state(&worker->value, now_ms) !=
                         IVR_FMQ_WORKER_DRAINING &&
                     !(route->reconciliation_only &&
                       ivr_fmq_worker_effective_state(
                           &worker->value, now_ms) ==
                           IVR_FMQ_WORKER_RECONCILING))) {
            status = IVR_EVERSION;
        } else {
            if (command->kind == IVR_MEDIA_COMMAND_SESSION_CLOSE) {
                route->state = IVR_FMQ_DIALOG_CLOSING;
                snprintf(route->transition_command_id,
                         sizeof(route->transition_command_id), "%s",
                         command->message_id);
            } else if (command->kind == IVR_MEDIA_COMMAND_INPUT_START ||
                       command->kind == IVR_MEDIA_COMMAND_CANCEL) {
                if (route->pending_operation_kind != 0) {
                    status = IVR_EBUSY;
                } else {
                    route->pending_operation_kind = command->kind;
                    snprintf(route->transition_command_id,
                             sizeof(route->transition_command_id), "%s",
                             command->message_id);
                    snprintf(route->pending_input_id,
                             sizeof(route->pending_input_id), "%s",
                             command->input_id);
                    route->pending_input_generation =
                        command->input_generation;
                }
            }
            if (status == IVR_OK) *out_worker = worker->value;
        }
    }
    ivr_mutex_unlock(&adapter->seq_lock);
    return status;
}

static void ivr_fmq_dialog_rollback_send(
    ivr_fmq_adapter_t *adapter, const ivr_media_command_t *command) {
    ivr_mutex_lock(&adapter->seq_lock);
    ivr_fmq_dialog_route_t *route = ivr_fmq_dialog_find_locked(
        adapter, command->tenant_id, command->provider_session_id,
        command->dialog_id);
    if (route && strcmp(route->transition_command_id, command->message_id) == 0) {
        if (route->state == IVR_FMQ_DIALOG_OPENING) {
            ivr_fmq_worker_entry_t *worker =
                ivr_fmq_worker_find_locked(adapter, route->worker_id);
            if (worker &&
                strcmp(worker->value.instance_id,
                       route->worker_instance_id) == 0 &&
                worker->value.connection_generation ==
                    route->worker_connection_generation &&
                worker->value.reserved_sessions > 0) {
                worker->value.reserved_sessions--;
            }
            memset(route, 0, sizeof(*route));
            if (adapter->dialog_count > 0) adapter->dialog_count--;
        } else if (route->state == IVR_FMQ_DIALOG_CLOSING) {
            route->state = IVR_FMQ_DIALOG_ACTIVE;
            route->transition_command_id[0] = '\0';
        } else if (route->pending_operation_kind != 0) {
            route->pending_operation_kind = 0;
            route->transition_command_id[0] = '\0';
            route->pending_input_id[0] = '\0';
            route->pending_input_generation = 0;
        }
    }
    ivr_mutex_unlock(&adapter->seq_lock);
}

static int ivr_fmq_attempt_has_worker(
    const ivr_fmq_assignment_entry_t *assignment,
    const ivr_fmq_worker_snapshot_t *worker) {
    for (uint32_t i = 0; i < assignment->value.attempt_count; i++) {
        const ivr_fmq_attempt_record_t *attempt = &assignment->attempts[i];
        if (strcmp(attempt->worker_id, worker->worker_id) == 0 &&
            strcmp(attempt->worker_instance_id, worker->instance_id) == 0 &&
            attempt->worker_connection_generation ==
                worker->connection_generation) {
            return 1;
        }
    }
    return 0;
}

/* Called with seq_lock held. Selection, reservation and attempt transition
   are committed together; the FlowMQ send happens after the lock is released. */
static int ivr_fmq_assignment_prepare_retry_locked(
    ivr_fmq_adapter_t *a, ivr_fmq_assignment_entry_t *assignment,
    uint64_t now_ms, const char *content_package,
    ivr_fmq_assignment_entry_t *out) {
    if (!assignment->retry_pending ||
        assignment->value.attempt_count >= a->dispatch_max_attempts ||
        UINT64_MAX - now_ms < a->dispatch_deadline_ms) {
        return 0;
    }
    uint32_t start = a->next_worker_index % a->worker_capacity;
    for (uint32_t offset = 0; offset < a->worker_capacity; offset++) {
        uint32_t idx = (start + offset) % a->worker_capacity;
        ivr_fmq_worker_entry_t *worker = &a->workers[idx];
        if (!worker->valid || worker->value.protocol_version != 2u ||
            ivr_fmq_worker_effective_state(&worker->value, now_ms) !=
                IVR_FMQ_WORKER_READY ||
            !ivr_fmq_worker_health_ready(&worker->value) ||
            worker->value.active_sessions + worker->value.reserved_sessions >=
                worker->value.max_sessions ||
            !ivr_fmq_acl_worker_allows_snapshot(
                a, &worker->value, assignment->value.room_id,
                assignment->value.call_id, content_package) ||
            ivr_fmq_attempt_has_worker(assignment, &worker->value) ||
            (a->bridge && !ivr_room_bridge_worker_available(
                              a->bridge, worker->value.worker_id))) {
            continue;
        }
        uint32_t attempt_index = assignment->value.attempt_count;
        int written = snprintf(
            assignment->value.attempt_id,
            sizeof(assignment->value.attempt_id), "dispatch-%s-a%u",
            assignment->value.assignment_id, attempt_index + 1u);
        if (written <= 0 ||
            (size_t)written >= sizeof(assignment->value.attempt_id)) {
            return 0;
        }
        ivr_fmq_attempt_record_t *attempt =
            &assignment->attempts[attempt_index];
        memset(attempt, 0, sizeof(*attempt));
        snprintf(attempt->message_id, sizeof(attempt->message_id), "%s",
                 assignment->value.attempt_id);
        snprintf(attempt->attempt_id, sizeof(attempt->attempt_id), "%s",
                 assignment->value.attempt_id);
        snprintf(attempt->worker_id, sizeof(attempt->worker_id), "%s",
                 worker->value.worker_id);
        snprintf(attempt->worker_instance_id,
                 sizeof(attempt->worker_instance_id), "%s",
                 worker->value.instance_id);
        attempt->worker_connection_generation =
            worker->value.connection_generation;
        snprintf(assignment->value.worker_id,
                 sizeof(assignment->value.worker_id), "%s",
                 worker->value.worker_id);
        snprintf(assignment->value.worker_instance_id,
                 sizeof(assignment->value.worker_instance_id), "%s",
                 worker->value.instance_id);
        assignment->value.worker_connection_generation =
            worker->value.connection_generation;
        assignment->value.attempt_count++;
        assignment->value.state = IVR_FMQ_ASSIGNMENT_PENDING;
        assignment->value.status_code = 0;
        assignment->value.error_code[0] = '\0';
        assignment->value.error_message[0] = '\0';
        assignment->value.dispatch_deadline_at_ms =
            now_ms + a->dispatch_deadline_ms;
        assignment->retry_pending = 0;
        worker->value.reserved_sessions++;
        a->next_worker_index = (idx + 1u) % a->worker_capacity;
        *out = *assignment;
        return 1;
    }
    return 0;
}

static ivr_status_t ivr_fmq_assignment_send_v2(
    ivr_fmq_adapter_t *a, const ivr_fmq_assignment_entry_t *assignment) {
    ivr_call_dispatch_t dispatch;
    memset(&dispatch, 0, sizeof(dispatch));
    dispatch.wire_version = 2;
    snprintf(dispatch.message_id, sizeof(dispatch.message_id), "%s",
             assignment->value.attempt_id);
    snprintf(dispatch.assignment_id, sizeof(dispatch.assignment_id), "%s",
             assignment->value.assignment_id);
    snprintf(dispatch.attempt_id, sizeof(dispatch.attempt_id), "%s",
             assignment->value.attempt_id);
    snprintf(dispatch.worker_id, sizeof(dispatch.worker_id), "%s",
             assignment->value.worker_id);
    snprintf(dispatch.worker_instance_id,
             sizeof(dispatch.worker_instance_id), "%s",
             assignment->value.worker_instance_id);
    dispatch.worker_connection_generation =
        assignment->value.worker_connection_generation;
    snprintf(dispatch.room_id, sizeof(dispatch.room_id), "%s",
             assignment->value.room_id);
    snprintf(dispatch.call_id, sizeof(dispatch.call_id), "%s",
             assignment->value.call_id);
    dispatch.call_generation = assignment->value.call_generation;
    dispatch.expected_room_version = assignment->room_version;
    snprintf(dispatch.content_package, sizeof(dispatch.content_package), "%s",
             a->content_package);
    dispatch.deadline_timeout_ms = a->dispatch_deadline_ms;
    return ivr_room_bridge_dispatch_call_v2(a->bridge, &dispatch);
}

/* ------------------------------------------------------------------ */
/* command application (authoritative aggregate)                       */
/* ------------------------------------------------------------------ */

static ivr_status_t ivr_fmq_apply_join(ivr_fmq_adapter_t *a,
                                       const ivr_room_command_t *cmd,
                                       ivr_room_command_result_t *result) {
    turbo_participant_role_t role = ivr_fmq_role_for(cmd->participant_role);
    if (role == 0) {
        result->status_code = IVR_EINVAL;
        snprintf(result->error_message, sizeof(result->error_message),
                 "unsupported participant_role '%s'", cmd->participant_role);
        return IVR_OK;
    }
    if (ivr_fmq_room_version(a->service, cmd->room_id) == 0) {
        result->status_code = IVR_ESTATE;
        snprintf(result->error_message, sizeof(result->error_message),
                 "room not found: %s", cmd->room_id);
        return IVR_OK;
    }
    /* Idempotent join: an existing participant with the same role is a
       replay of a committed fact, not a new mutation. */
    turbo_room_participant_summary_t existing;
    if (turbo_room_service_get_participant_summary(a->service, cmd->room_id,
                                                   cmd->call_id,
                                                   &existing) == 0) {
        if (existing.role != role) {
            result->status_code = IVR_ESTATE;
            snprintf(result->error_message, sizeof(result->error_message),
                     "participant %s already joined with a different role",
                     cmd->call_id);
            return IVR_OK;
        }
        uint64_t seq = ivr_fmq_seq_current(a, cmd);
        if (seq == 0) {
            seq = ivr_fmq_seq_next(a, cmd); /* entry evicted: resume tracking */
        }
        result->status_code = 0;
        result->room_version = ivr_fmq_room_version(a->service, cmd->room_id);
        result->sequence = seq;
        return IVR_OK;
    }
    uint64_t seq = ivr_fmq_seq_next(a, cmd);
    if (seq == 0) {
        result->status_code = IVR_ENOSPC;
        snprintf(result->error_message, sizeof(result->error_message),
                 "per-call sequence table full");
        return IVR_OK;
    }
    turbo_room_participant_config_t participant;
    memset(&participant, 0, sizeof(participant));
    participant.participant_id = cmd->call_id;
    participant.call_generation = cmd->call_generation;
    participant.user_id = "";
    participant.display_name = cmd->call_id;
    participant.role = role;
    if (turbo_room_service_add_participant(a->service, cmd->room_id,
                                           &participant) != 0) {
        ivr_fmq_seq_rollback(a, cmd);
        result->status_code = IVR_ESTATE;
        snprintf(result->error_message, sizeof(result->error_message),
                 "add_participant failed for %s in room %s", cmd->call_id,
                 cmd->room_id);
        return IVR_OK;
    }
    result->status_code = 0;
    result->room_version = ivr_fmq_room_version(a->service, cmd->room_id);
    result->sequence = seq;
    return IVR_OK;
}

static ivr_status_t ivr_fmq_apply_leave(ivr_fmq_adapter_t *a,
                                        const ivr_room_command_t *cmd,
                                        ivr_room_command_result_t *result) {
    turbo_room_participant_summary_t existing;
    if (turbo_room_service_get_participant_summary(a->service, cmd->room_id,
                                                   cmd->call_id,
                                                   &existing) != 0) {
        result->status_code = IVR_ESTATE;
        snprintf(result->error_message, sizeof(result->error_message),
                 "no participant %s in room %s", cmd->call_id, cmd->room_id);
        return IVR_OK;
    }
    uint64_t seq = ivr_fmq_seq_next(a, cmd);
    if (seq == 0) {
        result->status_code = IVR_ENOSPC;
        snprintf(result->error_message, sizeof(result->error_message),
                 "per-call sequence table full");
        return IVR_OK;
    }
    if (turbo_room_service_remove_participant(a->service, cmd->room_id,
                                              cmd->call_id) != 0) {
        ivr_fmq_seq_rollback(a, cmd);
        result->status_code = IVR_ESTATE;
        snprintf(result->error_message, sizeof(result->error_message),
                 "remove_participant failed for %s in room %s", cmd->call_id,
                 cmd->room_id);
        return IVR_OK;
    }
    result->status_code = 0;
    result->room_version = ivr_fmq_room_version(a->service, cmd->room_id);
    result->sequence = seq;
    return IVR_OK;
}

static ivr_status_t ivr_fmq_apply_snapshot(ivr_fmq_adapter_t *a,
                                           const ivr_room_command_t *cmd,
                                           ivr_room_command_result_t *result) {
    uint64_t version = ivr_fmq_room_version(a->service, cmd->room_id);
    if (version == 0) {
        result->status_code = IVR_ESTATE;
        snprintf(result->error_message, sizeof(result->error_message),
                 "room not found: %s", cmd->room_id);
        return IVR_OK;
    }
    result->status_code = 0;
    result->room_version = version;
    result->sequence = ivr_fmq_seq_current(a, cmd);
    return IVR_OK;
}

ivr_status_t ivr_fmq_adapter_apply(ivr_fmq_adapter_t *adapter,
                                   const ivr_room_command_t *command,
                                   ivr_room_command_result_t *result) {
    if (!adapter || !command || !result) {
        return IVR_EINVAL;
    }
    memset(result, 0, sizeof(*result));
    if (strcmp(command->command, "conference.join") == 0 ||
        strcmp(command->command, "conference.leave") == 0 ||
        strcmp(command->command, "get_snapshot") == 0) {
        /* Command authorization: a connected worker cannot bypass the
           configured tenant/room/call scope or content capability ACL.
           A denied command never advances the authoritative room; the reply
           still reports the current (unchanged) version for observability. */
        if (!ivr_fmq_adapter_authorize(adapter, command, result)) {
            result->room_version =
                ivr_fmq_room_version(adapter->service, command->room_id);
            return IVR_OK;
        }
    }
    if (strcmp(command->command, "conference.join") == 0) {
        return ivr_fmq_apply_join(adapter, command, result);
    }
    if (strcmp(command->command, "conference.leave") == 0) {
        return ivr_fmq_apply_leave(adapter, command, result);
    }
    if (strcmp(command->command, "get_snapshot") == 0) {
        return ivr_fmq_apply_snapshot(adapter, command, result);
    }
    if (strcmp(command->command, "worker.sync") == 0 ||
        strcmp(command->command, "worker.sync.v2") == 0 ||
        strcmp(command->command, "worker.heartbeat") == 0) {
        result->status_code =
            ivr_fmq_adapter_register_worker(adapter, command);
        if (result->status_code != IVR_OK) {
            snprintf(result->error_message, sizeof(result->error_message),
                     "worker registration failed for %s", command->worker_id);
        }
        return IVR_OK;
    }
    result->status_code = IVR_EINVAL;
    snprintf(result->error_message, sizeof(result->error_message),
             "unknown command: %s", command->command);
    return IVR_OK;
}

/* ------------------------------------------------------------------ */
/* bridge host handler                                                 */
/* ------------------------------------------------------------------ */

static uint64_t ivr_fmq_adapter_get_room_version(void *ctx,
                                                 const char *room_id) {
    ivr_fmq_adapter_t *a = (ivr_fmq_adapter_t *)ctx;
    return a ? ivr_fmq_room_version(a->service, room_id) : 0;
}

static ivr_status_t ivr_fmq_adapter_on_media_result(
    void *ctx, const ivr_media_command_result_t *result) {
    ivr_fmq_adapter_t *adapter = (ivr_fmq_adapter_t *)ctx;
    int reconciliation_only = 0;
    if (!adapter || !result || !adapter->media_observer.on_media_result) {
        return IVR_ESTATE;
    }
    ivr_mutex_lock(&adapter->seq_lock);
    ivr_fmq_dialog_route_t *route = ivr_fmq_dialog_find_locked(
        adapter, result->tenant_id, result->provider_session_id,
        result->dialog_id);
    ivr_fmq_worker_entry_t *worker =
        route ? ivr_fmq_worker_find_locked(adapter, route->worker_id) : NULL;
    if (!route || !worker ||
        route->call_generation != result->call_generation ||
        strcmp(route->room_id, result->room_id) != 0 ||
        strcmp(route->call_id, result->call_id) != 0 ||
        strcmp(route->worker_id, result->worker_id) != 0 ||
        strcmp(worker->value.instance_id, route->worker_instance_id) != 0 ||
        worker->value.connection_generation !=
            route->worker_connection_generation) {
        ivr_mutex_unlock(&adapter->seq_lock);
        return IVR_EVERSION;
    }
    reconciliation_only = route->reconciliation_only;
    if (route->state == IVR_FMQ_DIALOG_OPENING) {
        if (strcmp(route->transition_command_id, result->message_id) != 0 ||
            worker->value.reserved_sessions == 0 ||
            (result->status_code == IVR_OK &&
             worker->value.active_sessions >= worker->value.max_sessions)) {
            ivr_mutex_unlock(&adapter->seq_lock);
            return IVR_EVERSION;
        }
        worker->value.reserved_sessions--;
        if (result->status_code == IVR_OK) {
            worker->value.active_sessions++;
            route->state = IVR_FMQ_DIALOG_ACTIVE;
            route->transition_command_id[0] = '\0';
        } else {
            memset(route, 0, sizeof(*route));
            if (adapter->dialog_count > 0) adapter->dialog_count--;
        }
    } else if (route->state == IVR_FMQ_DIALOG_CLOSING) {
        if (strcmp(route->transition_command_id, result->message_id) != 0) {
            ivr_mutex_unlock(&adapter->seq_lock);
            return IVR_EVERSION;
        }
        if (result->status_code == IVR_OK) {
            if (worker->value.active_sessions > 0) {
                worker->value.active_sessions--;
            }
            memset(route, 0, sizeof(*route));
            if (adapter->dialog_count > 0) adapter->dialog_count--;
        } else {
            route->state = IVR_FMQ_DIALOG_ACTIVE;
            route->transition_command_id[0] = '\0';
        }
    } else if (route->pending_operation_kind != 0) {
        if (strcmp(route->transition_command_id, result->message_id) != 0) {
            ivr_mutex_unlock(&adapter->seq_lock);
            return IVR_EVERSION;
        }
        if (result->status_code == IVR_OK &&
            route->pending_operation_kind ==
                IVR_MEDIA_COMMAND_INPUT_START) {
            snprintf(route->active_input_id,
                     sizeof(route->active_input_id), "%s",
                     route->pending_input_id);
            route->active_input_generation =
                route->pending_input_generation;
        } else if (result->status_code == IVR_OK &&
                   route->pending_operation_kind ==
                       IVR_MEDIA_COMMAND_CANCEL) {
            route->active_input_id[0] = '\0';
            route->active_input_generation = 0;
        }
        route->pending_operation_kind = 0;
        route->transition_command_id[0] = '\0';
        route->pending_input_id[0] = '\0';
        route->pending_input_generation = 0;
    }
    ivr_mutex_unlock(&adapter->seq_lock);
    if (reconciliation_only) return IVR_OK;
    return adapter->media_observer.on_media_result(
        adapter->media_observer.context, result);
}

static ivr_status_t ivr_fmq_adapter_on_media_event(
    void *ctx, const ivr_media_event_t *event) {
    ivr_fmq_adapter_t *adapter = (ivr_fmq_adapter_t *)ctx;
    if (!adapter || !event || !adapter->media_observer.on_media_event) {
        return IVR_ESTATE;
    }
    ivr_mutex_lock(&adapter->seq_lock);
    ivr_fmq_dialog_route_t *route = ivr_fmq_dialog_find_locked(
        adapter, event->tenant_id, event->provider_session_id,
        event->dialog_id);
    ivr_fmq_worker_entry_t *worker =
        route ? ivr_fmq_worker_find_locked(adapter, route->worker_id) : NULL;
    if (!route || route->state != IVR_FMQ_DIALOG_ACTIVE || !worker ||
        route->call_generation != event->call_generation ||
        strcmp(route->room_id, event->room_id) != 0 ||
        strcmp(route->call_id, event->call_id) != 0 ||
        strcmp(route->worker_id, event->worker_id) != 0 ||
        strcmp(worker->value.instance_id, route->worker_instance_id) != 0 ||
        worker->value.connection_generation !=
            route->worker_connection_generation) {
        ivr_mutex_unlock(&adapter->seq_lock);
        return IVR_EVERSION;
    }
    ivr_mutex_unlock(&adapter->seq_lock);
    return adapter->media_observer.on_media_event(
        adapter->media_observer.context, event);
}

static ivr_status_t ivr_fmq_adapter_on_inventory_page(
    void *ctx, const ivr_worker_inventory_envelope_t *result) {
    ivr_fmq_adapter_t *adapter = (ivr_fmq_adapter_t *)ctx;
    ivr_fmq_worker_entry_t *worker;
    if (!adapter || !result ||
        !adapter->inventory_observer.on_inventory_page) {
        return IVR_ESTATE;
    }
    ivr_mutex_lock(&adapter->seq_lock);
    worker = ivr_fmq_worker_find_locked(adapter, result->worker_id);
    if (!worker) {
        ivr_mutex_unlock(&adapter->seq_lock);
        return IVR_EVERSION;
    }
    for (uint32_t i = 0; i < result->page.count; ++i) {
        const ivr_worker_inventory_record_t *record =
            &result->page.records[i];
        if (strcmp(record->worker_id, worker->value.worker_id) != 0 ||
            strcmp(record->worker_instance_id,
                   worker->value.instance_id) != 0 ||
            record->worker_epoch != worker->value.connection_generation) {
            ivr_mutex_unlock(&adapter->seq_lock);
            return IVR_EVERSION;
        }
    }
    ivr_mutex_unlock(&adapter->seq_lock);
    return adapter->inventory_observer.on_inventory_page(
        adapter->inventory_observer.context, result);
}

static ivr_status_t ivr_fmq_adapter_on_dispatch_result(
    void *ctx, const ivr_dispatch_result_t *result) {
    ivr_fmq_adapter_t *a = (ivr_fmq_adapter_t *)ctx;
    ivr_fmq_assignment_t assignment;
    char causation_id[128];
    uint64_t room_version = 0;
    uint64_t sequence = 0;
    int should_publish = 0;
    int is_current_attempt = 0;
    ivr_fmq_attempt_record_t *attempt = NULL;
    if (!a || !result) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&a->seq_lock);
    ivr_fmq_assignment_entry_t *entry = ivr_fmq_assignment_find_result_locked(
        a, result, &attempt, &is_current_attempt);
    if (!entry ||
        strcmp(entry->value.room_id, result->room_id) != 0 ||
        strcmp(entry->value.call_id, result->call_id) != 0 ||
        entry->value.call_generation != result->call_generation) {
        ivr_mutex_unlock(&a->seq_lock);
        return IVR_ESTATE;
    }
    if (result->wire_version == 2u && attempt && !is_current_attempt) {
        if (strcmp(attempt->worker_id, result->worker_id) != 0 ||
            strcmp(attempt->worker_instance_id,
                   result->worker_instance_id) != 0 ||
            attempt->worker_connection_generation !=
                result->worker_connection_generation) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_EVERSION;
        }
        if (result->status_code != IVR_OK ||
            attempt->cleanup_state != IVR_FMQ_CLEANUP_NONE) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_OK;
        }
        ivr_fmq_worker_entry_t *stale_worker =
            ivr_fmq_worker_find_locked(a, attempt->worker_id);
        if (!stale_worker ||
            strcmp(stale_worker->value.instance_id,
                   attempt->worker_instance_id) != 0 ||
            stale_worker->value.connection_generation !=
                attempt->worker_connection_generation ||
            stale_worker->value.max_sessions != result->max_sessions ||
            stale_worker->value.active_sessions >=
                stale_worker->value.max_sessions ||
            UINT64_MAX - ivr_fmq_now_ms(a) < a->dispatch_deadline_ms) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_EVERSION;
        }
        int written = snprintf(attempt->cleanup_release_message_id,
                               sizeof(attempt->cleanup_release_message_id),
                               "release-late-%s", attempt->attempt_id);
        if (written <= 0 ||
            (size_t)written >=
                sizeof(attempt->cleanup_release_message_id)) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_ENOSPC;
        }
        stale_worker->value.active_sessions++;
        attempt->cleanup_state = IVR_FMQ_CLEANUP_PENDING;
        attempt->cleanup_deadline_at_ms =
            ivr_fmq_now_ms(a) + a->dispatch_deadline_ms;
        assignment = entry->value;
        snprintf(assignment.worker_id, sizeof(assignment.worker_id), "%s",
                 attempt->worker_id);
        snprintf(assignment.release_message_id,
                 sizeof(assignment.release_message_id), "%s",
                 attempt->cleanup_release_message_id);
        ivr_mutex_unlock(&a->seq_lock);
        return ivr_room_bridge_release_call(
            a->bridge, assignment.worker_id, assignment.release_message_id,
            assignment.room_id, assignment.call_id,
            assignment.call_generation, "stale.dispatch.accepted");
    }
    if (strcmp(entry->value.worker_id, result->worker_id) != 0) {
        ivr_mutex_unlock(&a->seq_lock);
        return IVR_ESTATE;
    }
    if (result->wire_version == 2u &&
        (strcmp(entry->value.assignment_id, result->assignment_id) != 0 ||
         strcmp(entry->value.attempt_id, result->attempt_id) != 0 ||
         strcmp(entry->value.worker_instance_id,
                result->worker_instance_id) != 0 ||
         entry->value.worker_connection_generation !=
             result->worker_connection_generation)) {
        ivr_mutex_unlock(&a->seq_lock);
        return IVR_EVERSION;
    }
    ivr_fmq_worker_entry_t *result_worker =
        ivr_fmq_worker_find_locked(a, entry->value.worker_id);
    if (!result_worker ||
        strcmp(result_worker->value.instance_id,
               entry->value.worker_instance_id) != 0 ||
        result_worker->value.connection_generation !=
            entry->value.worker_connection_generation ||
        (result->wire_version == 2u &&
         (result_worker->value.protocol_version != 2u ||
          result->max_sessions != result_worker->value.max_sessions))) {
        ivr_mutex_unlock(&a->seq_lock);
        return IVR_EVERSION;
    }
    if (entry->value.state == IVR_FMQ_ASSIGNMENT_REJECTED &&
        (strcmp(entry->value.error_code, "dispatch_timeout") == 0 ||
         strcmp(entry->value.error_code, "dispatch_cancelled") == 0)) {
        if (result->status_code != IVR_OK) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_OK;
        }
        ivr_fmq_worker_entry_t *worker = ivr_fmq_worker_find_locked(
            a, entry->value.worker_id);
        if (!worker ||
            strcmp(worker->value.instance_id,
                   entry->value.worker_instance_id) != 0 ||
            worker->value.connection_generation !=
                entry->value.worker_connection_generation ||
            worker->value.active_sessions >= worker->value.max_sessions) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_EVERSION;
        }
        int written = snprintf(entry->value.release_message_id,
                               sizeof(entry->value.release_message_id),
                               "release-late-%s", result->message_id);
        if (written <= 0 ||
            (size_t)written >= sizeof(entry->value.release_message_id)) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_ENOSPC;
        }
        worker->value.active_sessions++;
        entry->value.state = IVR_FMQ_ASSIGNMENT_RELEASING;
        uint64_t now_ms = ivr_fmq_now_ms(a);
        if (UINT64_MAX - now_ms < a->dispatch_deadline_ms) {
            worker->value.active_sessions--;
            entry->value.state = IVR_FMQ_ASSIGNMENT_REJECTED;
            entry->value.release_message_id[0] = '\0';
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_ESTATE;
        }
        entry->value.release_deadline_at_ms =
            now_ms + a->dispatch_deadline_ms;
        entry->value.status_code = IVR_OK;
        entry->value.error_code[0] = '\0';
        entry->value.error_message[0] = '\0';
        assignment = entry->value;
        ivr_mutex_unlock(&a->seq_lock);
        return ivr_room_bridge_release_call(
            a->bridge, assignment.worker_id,
            assignment.release_message_id, assignment.room_id,
            assignment.call_id, assignment.call_generation,
            "dispatch.accepted_after_deadline");
    }
    if (entry->retry_pending && result->status_code != IVR_OK) {
        ivr_mutex_unlock(&a->seq_lock);
        return IVR_OK;
    }
    ivr_fmq_assignment_state_t next_state =
        result->status_code == IVR_OK ? IVR_FMQ_ASSIGNMENT_ACCEPTED
                                      : IVR_FMQ_ASSIGNMENT_REJECTED;
    int was_pending = entry->value.state == IVR_FMQ_ASSIGNMENT_PENDING;
    if (entry->value.state != IVR_FMQ_ASSIGNMENT_PENDING &&
        (entry->value.state != next_state ||
         entry->value.status_code != result->status_code)) {
        ivr_mutex_unlock(&a->seq_lock);
        return IVR_ESTATE;
    }
    if (was_pending) {
        ivr_fmq_worker_entry_t *worker =
            ivr_fmq_worker_find_locked(a, entry->value.worker_id);
        if (!worker ||
            strcmp(worker->value.instance_id,
                   entry->value.worker_instance_id) != 0 ||
            worker->value.connection_generation !=
                entry->value.worker_connection_generation ||
            worker->value.reserved_sessions == 0 ||
            (next_state == IVR_FMQ_ASSIGNMENT_ACCEPTED &&
             worker->value.active_sessions >= worker->value.max_sessions)) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_EVERSION;
        }
        worker->value.reserved_sessions--;
        if (next_state == IVR_FMQ_ASSIGNMENT_ACCEPTED) {
            worker->value.active_sessions++;
        }
    }
    if (was_pending && result->wire_version == 2u &&
        next_state == IVR_FMQ_ASSIGNMENT_REJECTED &&
        entry->value.attempt_count < a->dispatch_max_attempts) {
        entry->retry_pending = 1;
        next_state = IVR_FMQ_ASSIGNMENT_PENDING;
    }
    entry->value.state = next_state;
    entry->value.status_code = result->status_code;
    snprintf(entry->value.error_code, sizeof(entry->value.error_code), "%s",
             result->error_code);
    snprintf(entry->value.error_message, sizeof(entry->value.error_message),
             "%s", result->error_message);
    if (a->events_enabled && next_state == IVR_FMQ_ASSIGNMENT_ACCEPTED &&
        !entry->event_published && entry->room_version > 0 &&
        entry->sequence > 0) {
        assignment = entry->value;
        snprintf(causation_id, sizeof(causation_id), "%s",
                 entry->causation_id);
        room_version = entry->room_version;
        sequence = entry->sequence;
        should_publish = 1;
    }
    ivr_mutex_unlock(&a->seq_lock);

    if (should_publish) {
        char event_id[160];
        snprintf(event_id, sizeof(event_id), "event-%s", assignment.message_id);
        if (ivr_room_bridge_publish_participant_joined(
                a->bridge, event_id, causation_id, assignment.worker_id,
                assignment.room_id, assignment.call_id,
                assignment.call_generation, room_version, sequence,
                assignment.call_id, "caller") != IVR_OK) {
            return IVR_ESTATE;
        }
        ivr_mutex_lock(&a->seq_lock);
        entry = ivr_fmq_assignment_find_locked(a, result->message_id);
        if (entry) {
            entry->event_published = 1;
        }
        ivr_mutex_unlock(&a->seq_lock);
    }
    return IVR_OK;
}

static ivr_status_t ivr_fmq_adapter_on_release_result(
    void *ctx, const ivr_release_result_t *result) {
    ivr_fmq_adapter_t *a = (ivr_fmq_adapter_t *)ctx;
    if (!a || !result) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock(&a->seq_lock);
    ivr_fmq_assignment_entry_t *entry = NULL;
    ivr_fmq_attempt_record_t *cleanup_attempt = NULL;
    for (uint32_t i = 0; i < a->assignment_capacity; i++) {
        ivr_fmq_assignment_entry_t *candidate = &a->assignments[i];
        if (candidate->valid) {
            for (uint32_t j = 0; j < candidate->value.attempt_count; j++) {
                ivr_fmq_attempt_record_t *attempt = &candidate->attempts[j];
                if (attempt->cleanup_state != IVR_FMQ_CLEANUP_NONE &&
                    strcmp(attempt->cleanup_release_message_id,
                           result->message_id) == 0) {
                    entry = candidate;
                    cleanup_attempt = attempt;
                    break;
                }
            }
        }
        if (cleanup_attempt) {
            break;
        }
        if (candidate->valid &&
            candidate->value.state == IVR_FMQ_ASSIGNMENT_RELEASING &&
            strcmp(candidate->value.release_message_id,
                   result->message_id) == 0) {
            entry = candidate;
            break;
        }
    }
    if (cleanup_attempt) {
        if (strcmp(cleanup_attempt->worker_id, result->worker_id) != 0 ||
            strcmp(entry->value.room_id, result->room_id) != 0 ||
            strcmp(entry->value.call_id, result->call_id) != 0 ||
            entry->value.call_generation != result->call_generation) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_ESTATE;
        }
        if (cleanup_attempt->cleanup_state == IVR_FMQ_CLEANUP_COMPLETE) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_OK;
        }
        if (result->status_code == IVR_OK) {
            ivr_fmq_worker_entry_t *worker =
                ivr_fmq_worker_find_locked(a, cleanup_attempt->worker_id);
            if (!worker ||
                strcmp(worker->value.instance_id,
                       cleanup_attempt->worker_instance_id) != 0 ||
                worker->value.connection_generation !=
                    cleanup_attempt->worker_connection_generation) {
                ivr_mutex_unlock(&a->seq_lock);
                return IVR_EVERSION;
            }
            if (worker->value.active_sessions > 0) {
                worker->value.active_sessions--;
            }
            cleanup_attempt->cleanup_state = IVR_FMQ_CLEANUP_COMPLETE;
            cleanup_attempt->cleanup_deadline_at_ms = 0;
        } else {
            cleanup_attempt->cleanup_deadline_at_ms = ivr_fmq_now_ms(a);
        }
        ivr_mutex_unlock(&a->seq_lock);
        return IVR_OK;
    }
    if (!entry || strcmp(entry->value.worker_id, result->worker_id) != 0 ||
        strcmp(entry->value.room_id, result->room_id) != 0 ||
        strcmp(entry->value.call_id, result->call_id) != 0 ||
        entry->value.call_generation != result->call_generation) {
        ivr_mutex_unlock(&a->seq_lock);
        return IVR_ESTATE;
    }
    if (result->status_code == IVR_OK) {
        ivr_fmq_worker_entry_t *worker = ivr_fmq_worker_find_locked(
            a, entry->value.worker_id);
        if (!worker ||
            strcmp(worker->value.instance_id,
                   entry->value.worker_instance_id) != 0 ||
            worker->value.connection_generation !=
                entry->value.worker_connection_generation) {
            ivr_mutex_unlock(&a->seq_lock);
            return IVR_EVERSION;
        }
        if (worker->value.active_sessions > 0) {
            worker->value.active_sessions--;
        }
        memset(entry, 0, sizeof(*entry));
        if (a->assignment_count > 0) {
            a->assignment_count--;
        }
    } else {
        entry->value.state = IVR_FMQ_ASSIGNMENT_ACCEPTED;
        entry->value.status_code = result->status_code;
        snprintf(entry->value.error_code, sizeof(entry->value.error_code),
                 "%s", result->error_code);
        snprintf(entry->value.error_message,
                 sizeof(entry->value.error_message), "%s",
                 result->error_message);
        entry->value.release_message_id[0] = '\0';
        entry->value.release_deadline_at_ms = 0;
    }
    ivr_mutex_unlock(&a->seq_lock);
    return IVR_OK;
}

static int ivr_fmq_assignment_mark_worker_lost(ivr_fmq_adapter_t *a,
                                                uint32_t index) {
    ivr_fmq_assignment_t assignment;
    ivr_fmq_worker_snapshot_t worker;
    int worker_matches = 0;
    memset(&assignment, 0, sizeof(assignment));
    memset(&worker, 0, sizeof(worker));
    ivr_mutex_lock(&a->seq_lock);
    ivr_fmq_assignment_entry_t *entry = &a->assignments[index];
    if (!entry->valid ||
        entry->value.state != IVR_FMQ_ASSIGNMENT_ACCEPTED) {
        ivr_mutex_unlock(&a->seq_lock);
        return 0;
    }
    assignment = entry->value;
    ivr_fmq_worker_entry_t *worker_entry =
        ivr_fmq_worker_find_locked(a, assignment.worker_id);
    if (worker_entry &&
        strcmp(worker_entry->value.instance_id,
               assignment.worker_instance_id) == 0 &&
        worker_entry->value.connection_generation ==
            assignment.worker_connection_generation) {
        worker = worker_entry->value;
        worker_matches = 1;
    }
    ivr_mutex_unlock(&a->seq_lock);

    int lost = !worker_matches ||
               ivr_fmq_worker_effective_state(&worker, ivr_fmq_now_ms(a)) ==
                   IVR_FMQ_WORKER_EXPIRED ||
               (a->bridge && !ivr_room_bridge_worker_available(
                                 a->bridge, assignment.worker_id));
    if (!lost) {
        return 0;
    }

    ivr_mutex_lock(&a->seq_lock);
    entry = ivr_fmq_assignment_find_locked(a, assignment.message_id);
    if (entry && entry->value.state == IVR_FMQ_ASSIGNMENT_ACCEPTED &&
        strcmp(entry->value.worker_id, assignment.worker_id) == 0 &&
        entry->value.worker_connection_generation ==
            assignment.worker_connection_generation) {
        entry->value.state = IVR_FMQ_ASSIGNMENT_RECOVERING;
        entry->value.status_code = IVR_ECLOSED;
        snprintf(entry->value.error_code, sizeof(entry->value.error_code),
                 "worker_lost");
        snprintf(entry->value.error_message,
                 sizeof(entry->value.error_message),
                 "worker route or lease was lost; fail-closed cleanup pending");
        worker_entry = ivr_fmq_worker_find_locked(a, assignment.worker_id);
        if (worker_entry &&
            strcmp(worker_entry->value.instance_id,
                   assignment.worker_instance_id) == 0 &&
            worker_entry->value.connection_generation ==
                assignment.worker_connection_generation) {
            worker_entry->value.state = IVR_FMQ_WORKER_EXPIRED;
        }
        ivr_mutex_unlock(&a->seq_lock);
        return 1;
    }
    ivr_mutex_unlock(&a->seq_lock);
    return 0;
}

static void ivr_fmq_assignment_recover_worker_loss(ivr_fmq_adapter_t *a,
                                                    uint32_t index) {
    ivr_fmq_assignment_t assignment;
    int cleanup_applied;
    int event_published;
    uint64_t terminal_room_version;
    uint64_t terminal_sequence;
    memset(&assignment, 0, sizeof(assignment));
    ivr_mutex_lock(&a->seq_lock);
    ivr_fmq_assignment_entry_t *entry = &a->assignments[index];
    if (!entry->valid ||
        entry->value.state != IVR_FMQ_ASSIGNMENT_RECOVERING) {
        ivr_mutex_unlock(&a->seq_lock);
        return;
    }
    assignment = entry->value;
    cleanup_applied = entry->worker_loss_applied;
    event_published = entry->worker_loss_event_published;
    terminal_room_version = entry->room_version;
    terminal_sequence = entry->sequence;
    ivr_mutex_unlock(&a->seq_lock);

    if (!cleanup_applied) {
        if (a->media.release_caller_audio) {
            char media_error[128] = {0};
            if (a->media.release_caller_audio(
                    a->media.context, assignment.room_id, assignment.call_id,
                    media_error, sizeof(media_error)) != IVR_OK) {
                return;
            }
        }
        ivr_room_command_t command;
        turbo_room_participant_summary_t participant;
        uint64_t sequence;
        memset(&command, 0, sizeof(command));
        snprintf(command.room_id, sizeof(command.room_id), "%s",
                 assignment.room_id);
        snprintf(command.call_id, sizeof(command.call_id), "%s",
                 assignment.call_id);
        command.call_generation = assignment.call_generation;
        if (turbo_room_service_get_participant_summary(
                a->service, assignment.room_id, assignment.call_id,
                &participant) == 0) {
            sequence = ivr_fmq_seq_next(a, &command);
            if (sequence == 0) {
                return;
            }
            if (turbo_room_service_remove_participant(
                    a->service, assignment.room_id, assignment.call_id) != 0) {
                ivr_fmq_seq_rollback(a, &command);
                return;
            }
        } else {
            sequence = ivr_fmq_seq_current(a, &command);
        }
        uint64_t room_version =
            ivr_fmq_room_version(a->service, assignment.room_id);
        if (room_version == 0 || sequence == 0) {
            return;
        }

        ivr_mutex_lock(&a->seq_lock);
        entry = ivr_fmq_assignment_find_locked(a, assignment.message_id);
        if (!entry ||
            entry->value.state != IVR_FMQ_ASSIGNMENT_RECOVERING) {
            ivr_mutex_unlock(&a->seq_lock);
            return;
        }
        if (!entry->worker_loss_applied) {
            ivr_fmq_worker_entry_t *worker =
                ivr_fmq_worker_find_locked(a, assignment.worker_id);
            if (worker &&
                strcmp(worker->value.instance_id,
                       assignment.worker_instance_id) == 0 &&
                worker->value.connection_generation ==
                    assignment.worker_connection_generation &&
                worker->value.active_sessions > 0) {
                worker->value.active_sessions--;
            }
            entry->room_version = room_version;
            entry->sequence = sequence;
            entry->worker_loss_applied = 1;
        }
        assignment = entry->value;
        cleanup_applied = 1;
        event_published = entry->worker_loss_event_published;
        terminal_room_version = entry->room_version;
        terminal_sequence = entry->sequence;
        ivr_mutex_unlock(&a->seq_lock);
    }

    if (cleanup_applied && a->events_enabled && !event_published) {
        char event_id[192];
        const char *fact_id = assignment.assignment_id[0]
                                  ? assignment.assignment_id
                                  : assignment.message_id;
        int written = snprintf(event_id, sizeof(event_id),
                               "event-worker-lost-%s", fact_id);
        if (written <= 0 || (size_t)written >= sizeof(event_id) ||
            ivr_room_bridge_publish_worker_lost(
                a->bridge, event_id, fact_id, assignment.worker_id,
                assignment.room_id, assignment.call_id,
                assignment.call_generation,
                terminal_room_version, terminal_sequence,
                "worker_lost") != IVR_OK) {
            return;
        }
    }

    ivr_mutex_lock(&a->seq_lock);
    entry = ivr_fmq_assignment_find_locked(a, assignment.message_id);
    if (entry && entry->value.state == IVR_FMQ_ASSIGNMENT_RECOVERING &&
        entry->worker_loss_applied) {
        entry->worker_loss_event_published = 1;
        memset(entry, 0, sizeof(*entry));
        if (a->assignment_count > 0) {
            a->assignment_count--;
        }
    }
    ivr_mutex_unlock(&a->seq_lock);
}

/* Convert a lost worker route into one durable upstream fact before releasing
   its bounded dialog/worker capacity. The observer runs outside seq_lock. A
   failed observer admission leaves the stable event on the route for retry. */
static void ivr_fmq_dialog_recover_worker_loss(
    ivr_fmq_adapter_t *adapter, uint32_t index, uint64_t now_ms) {
    ivr_fmq_dialog_route_t snapshot;
    ivr_fmq_worker_entry_t *worker;
    turbo_uuid_t event_uuid;
    char new_event_id[128];
    uint64_t occurred_at_ms = 0;
    int worker_available;
    int marked = 0;

    if (!adapter->media_observer.on_media_event) {
        return;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    memset(new_event_id, 0, sizeof(new_event_id));

    ivr_mutex_lock(&adapter->seq_lock);
    if (index >= adapter->dialog_capacity ||
        !adapter->dialogs[index].valid) {
        ivr_mutex_unlock(&adapter->seq_lock);
        return;
    }
    snapshot = adapter->dialogs[index];
    worker = ivr_fmq_worker_find_locked(adapter, snapshot.worker_id);
    worker_available =
        worker &&
        strcmp(worker->value.instance_id, snapshot.worker_instance_id) == 0 &&
        worker->value.connection_generation ==
            snapshot.worker_connection_generation &&
        (ivr_fmq_worker_effective_state(&worker->value, now_ms) ==
             IVR_FMQ_WORKER_READY ||
         ivr_fmq_worker_effective_state(&worker->value, now_ms) ==
             IVR_FMQ_WORKER_DRAINING);
    ivr_mutex_unlock(&adapter->seq_lock);

    if (snapshot.state != IVR_FMQ_DIALOG_WORKER_LOST && worker_available &&
        (!adapter->bridge || ivr_room_bridge_worker_available(
                                 adapter->bridge, snapshot.worker_id))) {
        return;
    }
    if (snapshot.state != IVR_FMQ_DIALOG_WORKER_LOST) {
        occurred_at_ms = turbo_realtime_ms();
        if (occurred_at_ms == 0 ||
            turbo_uuid_v4_generate(&event_uuid) != TURBO_OK ||
            turbo_uuid_format(&event_uuid, new_event_id,
                              sizeof(new_event_id)) != TURBO_OK) {
            return;
        }

        ivr_mutex_lock(&adapter->seq_lock);
        ivr_fmq_dialog_route_t *route = &adapter->dialogs[index];
        if (route->valid &&
            route->state != IVR_FMQ_DIALOG_WORKER_LOST &&
            strcmp(route->provider_session_id,
                   snapshot.provider_session_id) == 0 &&
            strcmp(route->dialog_id, snapshot.dialog_id) == 0 &&
            strcmp(route->worker_id, snapshot.worker_id) == 0 &&
            route->worker_connection_generation ==
                snapshot.worker_connection_generation) {
            worker = ivr_fmq_worker_find_locked(adapter, route->worker_id);
            if (worker &&
                strcmp(worker->value.instance_id,
                       route->worker_instance_id) == 0 &&
                worker->value.connection_generation ==
                    route->worker_connection_generation) {
                if (route->state == IVR_FMQ_DIALOG_OPENING) {
                    if (worker->value.reserved_sessions > 0) {
                        worker->value.reserved_sessions--;
                    }
                } else if (worker->value.active_sessions > 0) {
                    worker->value.active_sessions--;
                }
            }
            route->state = IVR_FMQ_DIALOG_WORKER_LOST;
            snprintf(route->worker_lost_event_id,
                     sizeof(route->worker_lost_event_id), "%s",
                     new_event_id);
            route->worker_lost_occurred_at_ms = occurred_at_ms;
            snapshot = *route;
            marked = 1;
        }
        ivr_mutex_unlock(&adapter->seq_lock);
        if (!marked) {
            return;
        }
    }

    {
        ivr_media_event_t event;
        memset(&event, 0, sizeof(event));
        snprintf(event.event_id, sizeof(event.event_id), "%s",
                 snapshot.worker_lost_event_id);
        snprintf(event.tenant_id, sizeof(event.tenant_id), "%s",
                 snapshot.tenant_id);
        snprintf(event.provider_session_id,
                 sizeof(event.provider_session_id), "%s",
                 snapshot.provider_session_id);
        snprintf(event.dialog_id, sizeof(event.dialog_id), "%s",
                 snapshot.dialog_id);
        snprintf(event.worker_id, sizeof(event.worker_id), "%s",
                 snapshot.worker_id);
        snprintf(event.room_id, sizeof(event.room_id), "%s",
                 snapshot.room_id);
        snprintf(event.call_id, sizeof(event.call_id), "%s",
                 snapshot.call_id);
        event.call_generation = snapshot.call_generation;
        event.sequence = snapshot.call_generation;
        snprintf(event.event_type, sizeof(event.event_type), "%s",
                 "provider.media.worker_lost");
        event.occurred_at_ms = snapshot.worker_lost_occurred_at_ms;
        snprintf(event.payload_json, sizeof(event.payload_json), "%s",
                 "{\"reason\":\"worker_lost\"}");
        if (adapter->media_observer.on_media_event(
                adapter->media_observer.context, &event) != IVR_OK) {
            return;
        }
    }

    ivr_mutex_lock(&adapter->seq_lock);
    if (adapter->dialogs[index].valid &&
        adapter->dialogs[index].state == IVR_FMQ_DIALOG_WORKER_LOST &&
        strcmp(adapter->dialogs[index].worker_lost_event_id,
               snapshot.worker_lost_event_id) == 0) {
        memset(&adapter->dialogs[index], 0, sizeof(adapter->dialogs[index]));
        if (adapter->dialog_count > 0) {
            adapter->dialog_count--;
        }
    }
    ivr_mutex_unlock(&adapter->seq_lock);
}

void ivr_fmq_adapter_poll(ivr_fmq_adapter_t *adapter) {
    if (!adapter) {
        return;
    }
    uint64_t now_ms = ivr_fmq_now_ms(adapter);
    ivr_mutex_lock(&adapter->seq_lock);
    for (uint32_t i = 0; i < adapter->worker_capacity; i++) {
        ivr_fmq_worker_entry_t *worker = &adapter->workers[i];
        if (worker->valid && !worker->lease_expiry_counted &&
            worker->value.lease_expires_at_ms <= now_ms) {
            worker->lease_expiry_counted = 1;
            worker->value.state = IVR_FMQ_WORKER_EXPIRED;
            adapter->lease_expired_total++;
        }
    }
    for (uint32_t i = 0; i < adapter->assignment_capacity; i++) {
        ivr_fmq_assignment_entry_t *entry = &adapter->assignments[i];
        if (!entry->valid ||
            entry->value.state != IVR_FMQ_ASSIGNMENT_PENDING ||
            entry->value.dispatch_deadline_at_ms == 0 ||
            entry->value.dispatch_deadline_at_ms > now_ms) {
            continue;
        }
        ivr_fmq_worker_entry_t *worker = ivr_fmq_worker_find_locked(
            adapter, entry->value.worker_id);
        if (worker &&
            strcmp(worker->value.instance_id,
                   entry->value.worker_instance_id) == 0 &&
            worker->value.connection_generation ==
                entry->value.worker_connection_generation &&
            worker->value.reserved_sessions > 0) {
            worker->value.reserved_sessions--;
        }
        int can_retry =
            worker && worker->value.protocol_version == 2u &&
            entry->value.attempt_count < adapter->dispatch_max_attempts;
        entry->value.state = can_retry ? IVR_FMQ_ASSIGNMENT_PENDING
                                       : IVR_FMQ_ASSIGNMENT_REJECTED;
        entry->retry_pending = can_retry;
        entry->value.status_code = IVR_ECLOSED;
        snprintf(entry->value.error_code, sizeof(entry->value.error_code),
                 "dispatch_timeout");
        snprintf(entry->value.error_message,
                 sizeof(entry->value.error_message),
                 "worker dispatch ACK deadline expired");
        adapter->dispatch_timeout_total++;
    }
    ivr_mutex_unlock(&adapter->seq_lock);

    for (uint32_t i = 0; i < adapter->assignment_capacity; i++) {
        (void)ivr_fmq_assignment_mark_worker_lost(adapter, i);
        ivr_fmq_assignment_recover_worker_loss(adapter, i);
    }
    for (uint32_t i = 0; i < adapter->dialog_capacity; i++) {
        ivr_fmq_dialog_recover_worker_loss(adapter, i, now_ms);
    }

    if (!adapter->bridge ||
        UINT64_MAX - now_ms < adapter->dispatch_deadline_ms) {
        return;
    }
    for (uint32_t i = 0; i < adapter->assignment_capacity; i++) {
        ivr_fmq_assignment_entry_t retry;
        int should_dispatch = 0;
        memset(&retry, 0, sizeof(retry));
        ivr_mutex_lock(&adapter->seq_lock);
        ivr_fmq_assignment_entry_t *entry = &adapter->assignments[i];
        if (entry->valid && entry->retry_pending) {
            should_dispatch = ivr_fmq_assignment_prepare_retry_locked(
                adapter, entry, now_ms, adapter->content_package, &retry);
            if (!should_dispatch && entry->retry_pending) {
                entry->retry_pending = 0;
                entry->value.state = IVR_FMQ_ASSIGNMENT_REJECTED;
            }
        }
        ivr_mutex_unlock(&adapter->seq_lock);
        if (should_dispatch &&
            ivr_fmq_assignment_send_v2(adapter, &retry) != IVR_OK) {
            ivr_fmq_worker_release_reservation(
                adapter, retry.value.worker_id,
                retry.value.worker_instance_id,
                retry.value.worker_connection_generation);
            ivr_mutex_lock(&adapter->seq_lock);
            entry = ivr_fmq_assignment_find_locked(
                adapter, retry.value.message_id);
            if (entry &&
                strcmp(entry->value.attempt_id,
                       retry.value.attempt_id) == 0) {
                entry->retry_pending =
                    entry->value.attempt_count <
                    adapter->dispatch_max_attempts;
                entry->value.state = entry->retry_pending
                                         ? IVR_FMQ_ASSIGNMENT_PENDING
                                         : IVR_FMQ_ASSIGNMENT_REJECTED;
                entry->value.status_code = IVR_ESTATE;
                snprintf(entry->value.error_code,
                         sizeof(entry->value.error_code),
                         "dispatch_send_failed");
                snprintf(entry->value.error_message,
                         sizeof(entry->value.error_message),
                         "retry dispatch could not reach worker route");
            }
            ivr_mutex_unlock(&adapter->seq_lock);
        }
    }
    for (uint32_t i = 0; i < adapter->assignment_capacity; i++) {
        for (uint32_t j = 0; j < adapter->dispatch_max_attempts; j++) {
            ivr_fmq_assignment_t cleanup;
            int should_cleanup = 0;
            memset(&cleanup, 0, sizeof(cleanup));
            ivr_mutex_lock(&adapter->seq_lock);
            ivr_fmq_assignment_entry_t *entry = &adapter->assignments[i];
            if (entry->valid && j < entry->value.attempt_count) {
                ivr_fmq_attempt_record_t *attempt = &entry->attempts[j];
                if (attempt->cleanup_state == IVR_FMQ_CLEANUP_PENDING &&
                    attempt->cleanup_deadline_at_ms <= now_ms) {
                    cleanup = entry->value;
                    snprintf(cleanup.worker_id, sizeof(cleanup.worker_id), "%s",
                             attempt->worker_id);
                    snprintf(cleanup.release_message_id,
                             sizeof(cleanup.release_message_id), "%s",
                             attempt->cleanup_release_message_id);
                    attempt->cleanup_deadline_at_ms =
                        now_ms + adapter->dispatch_deadline_ms;
                    should_cleanup = 1;
                }
            }
            ivr_mutex_unlock(&adapter->seq_lock);
            if (should_cleanup) {
                (void)ivr_room_bridge_release_call(
                    adapter->bridge, cleanup.worker_id,
                    cleanup.release_message_id, cleanup.room_id,
                    cleanup.call_id, cleanup.call_generation,
                    "stale.dispatch.cleanup_retry");
            }
        }
    }
    for (uint32_t i = 0; i < adapter->assignment_capacity; i++) {
        ivr_fmq_assignment_t retry = {0};
        int should_retry = 0;
        ivr_mutex_lock(&adapter->seq_lock);
        ivr_fmq_assignment_entry_t *entry = &adapter->assignments[i];
        if (entry->valid &&
            entry->value.state == IVR_FMQ_ASSIGNMENT_RELEASING &&
            entry->value.release_message_id[0] != '\0' &&
            entry->value.release_deadline_at_ms <= now_ms) {
            retry = entry->value;
            entry->value.release_deadline_at_ms =
                now_ms + adapter->dispatch_deadline_ms;
            adapter->release_timeout_total++;
            should_retry = 1;
        }
        ivr_mutex_unlock(&adapter->seq_lock);
        if (should_retry) {
            (void)ivr_room_bridge_release_call(
                adapter->bridge, retry.worker_id, retry.release_message_id,
                retry.room_id, retry.call_id, retry.call_generation,
                "release.result_timeout");
        }
    }
}

void ivr_fmq_adapter_get_stats(const ivr_fmq_adapter_t *adapter,
                               ivr_fmq_adapter_stats_t *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!adapter) {
        return;
    }
    ivr_mutex_lock((ivr_mutex_t *)&adapter->seq_lock);
    out->workers = adapter->worker_count;
    out->worker_capacity = adapter->worker_capacity;
    out->worker_high_water = adapter->worker_high_water;
    out->assignments = adapter->assignment_count;
    out->assignment_capacity = adapter->assignment_capacity;
    out->assignment_high_water = adapter->assignment_high_water;
    out->dialogs = adapter->dialog_count;
    out->dialog_capacity = adapter->dialog_capacity;
    out->dialog_high_water = adapter->dialog_high_water;
    out->lease_expired_total = adapter->lease_expired_total;
    out->dispatch_timeout_total = adapter->dispatch_timeout_total;
    out->release_timeout_total = adapter->release_timeout_total;
    out->acl_rejects = adapter->acl_rejects;
    ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
    ivr_room_bridge_get_stats(adapter->bridge, &out->bridge);
}

static void ivr_fmq_adapter_on_tick(void *ctx) {
    ivr_fmq_adapter_poll((ivr_fmq_adapter_t *)ctx);
}

static ivr_status_t ivr_fmq_adapter_on_command(
    void *ctx, const ivr_room_command_t *command,
    ivr_room_command_result_t *result) {
    ivr_fmq_adapter_t *a = (ivr_fmq_adapter_t *)ctx;
    if (!a || !command || !result) {
        return IVR_EINVAL;
    }
    if (strcmp(command->command, "conference.join") == 0 ||
        strcmp(command->command, "conference.leave") == 0 ||
        strcmp(command->command, "get_snapshot") == 0) {
        /* Authorize before any reservation, media side effect or aggregate
           mutation: connection success never bypasses the scope ACL. */
        if (!ivr_fmq_adapter_authorize(a, command, result)) {
            result->room_version =
                ivr_fmq_room_version(a->service, command->room_id);
            return IVR_OK;
        }
    }
    /* Conference membership is a room-domain mutation only. Typed
       dialog.start/dialog.terminate commands independently create and destroy
       media-worker routes; a room join never starts an IVR session. */
    return ivr_fmq_adapter_apply(a, command, result);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

ivr_status_t ivr_fmq_adapter_create(turbo_room_service_t *service,
                                    const ivr_fmq_adapter_config_t *config,
                                    ivr_fmq_adapter_t **out_adapter) {
    if (!service || !config || !out_adapter) {
        return IVR_EINVAL;
    }
    if (config->dispatch_max_attempts >
        IVR_FMQ_ADAPTER_MAX_DISPATCH_ATTEMPTS) {
        return IVR_EINVAL;
    }
    ivr_fmq_adapter_t *a = (ivr_fmq_adapter_t *)calloc(1, sizeof(*a));
    if (!a) {
        return IVR_ENOSPC;
    }
    a->service = service;
    a->media = config->media;
    a->events_enabled = config->pub_port > 0;
    a->media_observer = config->media_observer;
    a->inventory_observer = config->inventory_observer;
    snprintf(a->content_package, sizeof(a->content_package), "%s",
             config->default_content_package
                 ? config->default_content_package
                 : IVR_FMQ_ADAPTER_DEFAULT_CONTENT_PACKAGE);
    a->seq_capacity = config->seq_capacity ? config->seq_capacity
                                           : IVR_FMQ_ADAPTER_DEFAULT_SEQ_CAPACITY;
    a->seq_entries =
        (ivr_fmq_call_seq_t *)calloc(a->seq_capacity, sizeof(*a->seq_entries));
    if (!a->seq_entries) {
        free(a);
        return IVR_ENOSPC;
    }
    a->worker_capacity = config->worker_capacity
                              ? config->worker_capacity
                              : IVR_FMQ_ADAPTER_DEFAULT_WORKER_CAPACITY;
    a->legacy_worker_max_sessions =
        config->legacy_worker_max_sessions
            ? config->legacy_worker_max_sessions
            : IVR_FMQ_ADAPTER_DEFAULT_LEGACY_WORKER_MAX_SESSIONS;
    a->worker_lease_ms = config->worker_lease_ms
                             ? config->worker_lease_ms
                             : IVR_FMQ_ADAPTER_DEFAULT_WORKER_LEASE_MS;
    a->dispatch_deadline_ms =
        config->dispatch_deadline_ms
            ? config->dispatch_deadline_ms
            : IVR_FMQ_ADAPTER_DEFAULT_DISPATCH_DEADLINE_MS;
    a->dispatch_max_attempts =
        config->dispatch_max_attempts
            ? config->dispatch_max_attempts
            : IVR_FMQ_ADAPTER_DEFAULT_DISPATCH_MAX_ATTEMPTS;
    a->clock = config->clock;
    a->dedup_retention_ms =
        config->dedup_retention_ms ? config->dedup_retention_ms
                                   : IVR_FMQ_ADAPTER_DEFAULT_DEDUP_RETENTION_MS;
    if (config->worker_acl_count > IVR_FMQ_ADAPTER_MAX_WORKER_ACLS) {
        free(a->seq_entries);
        free(a);
        return IVR_EINVAL;
    }
    a->acl_capacity = (uint32_t)config->worker_acl_count;
    if (a->acl_capacity > 0u) {
        uint32_t acl_i;
        a->acls = (ivr_fmq_acl_entry_t *)calloc(a->acl_capacity,
                                                sizeof(*a->acls));
        if (!a->acls) {
            free(a->seq_entries);
            free(a);
            return IVR_ENOSPC;
        }
        for (acl_i = 0u; acl_i < a->acl_capacity; ++acl_i) {
            const ivr_fmq_worker_acl_entry_t *src =
                &config->worker_acls[acl_i];
            ivr_fmq_acl_entry_t *dst = &a->acls[acl_i];
            if (!src->worker_id || src->worker_id[0] == '\0' ||
                strlen(src->worker_id) >= sizeof(dst->worker_id) ||
                (src->tenant_id &&
                 strlen(src->tenant_id) >= sizeof(dst->tenant_id)) ||
                (src->room_scope && !ivr_acl_scope_valid(src->room_scope)) ||
                (src->call_scope && !ivr_acl_scope_valid(src->call_scope)) ||
                (src->content_capabilities &&
                 !ivr_acl_scope_valid(src->content_capabilities))) {
                free(a->acls);
                free(a->seq_entries);
                free(a);
                return IVR_EINVAL;
            }
            snprintf(dst->worker_id, sizeof(dst->worker_id), "%s",
                     src->worker_id);
            snprintf(dst->tenant_id, sizeof(dst->tenant_id), "%s",
                     src->tenant_id ? src->tenant_id : "");
            snprintf(dst->room_scope, sizeof(dst->room_scope), "%s",
                     src->room_scope && src->room_scope[0] ? src->room_scope
                                                           : "*");
            snprintf(dst->call_scope, sizeof(dst->call_scope), "%s",
                     src->call_scope && src->call_scope[0] ? src->call_scope
                                                           : "*");
            snprintf(dst->content_capabilities,
                     sizeof(dst->content_capabilities), "%s",
                     src->content_capabilities &&
                             src->content_capabilities[0]
                         ? src->content_capabilities
                         : "*");
            dst->valid = 1;
            a->acl_count++;
        }
    }
    a->workers = (ivr_fmq_worker_entry_t *)calloc(a->worker_capacity,
                                                   sizeof(*a->workers));
    if (!a->workers) {
        free(a->acls);
        free(a->seq_entries);
        free(a);
        return IVR_ENOSPC;
    }
    a->assignment_capacity =
        config->assignment_capacity ? config->assignment_capacity
                                    : IVR_FMQ_ADAPTER_DEFAULT_ASSIGNMENT_CAPACITY;
    a->assignments = (ivr_fmq_assignment_entry_t *)calloc(
        a->assignment_capacity, sizeof(*a->assignments));
    if (!a->assignments) {
        free(a->acls);
        free(a->workers);
        free(a->seq_entries);
        free(a);
        return IVR_ENOSPC;
    }
    a->dialog_capacity = config->dialog_capacity
                             ? config->dialog_capacity
                             : IVR_FMQ_ADAPTER_DEFAULT_DIALOG_CAPACITY;
    a->dialogs = (ivr_fmq_dialog_route_t *)calloc(a->dialog_capacity,
                                                   sizeof(*a->dialogs));
    if (!a->dialogs) {
        free(a->acls);
        free(a->assignments);
        free(a->workers);
        free(a->seq_entries);
        free(a);
        return IVR_ENOSPC;
    }
    if (ivr_mutex_init(&a->seq_lock) != 0) {
        free(a->acls);
        free(a->dialogs);
        free(a->assignments);
        free(a->workers);
        free(a->seq_entries);
        free(a);
        return IVR_ENOSPC;
    }
    if (config->bind_port > 0) {
        ivr_room_bridge_config_t bridge_config;
        memset(&bridge_config, 0, sizeof(bridge_config));
        bridge_config.host =
            config->bind_host ? config->bind_host : "127.0.0.1";
        bridge_config.port = config->bind_port;
        bridge_config.transport = config->transport;
        bridge_config.path = config->path;
        bridge_config.tls = config->tls;
        bridge_config.security = config->security;
        /* The worker heartbeat is the liveness signal. When callers leave
           transport timeout unset, keep the bridge alive for at least one
           full lease so the default heartbeat cannot race an idle timeout. */
        bridge_config.timeout_ms =
            config->timeout_ms ? config->timeout_ms : a->worker_lease_ms;
        bridge_config.queue_capacity = config->queue_capacity;
        bridge_config.dedup_capacity = config->dedup_capacity;
        bridge_config.dedup_retention_ms = a->dedup_retention_ms;
        bridge_config.now_ms = a->clock.now_ms;
        bridge_config.now_ctx = a->clock.context;
        bridge_config.pub_host = "127.0.0.1";
        bridge_config.pub_port = config->pub_port;
        bridge_config.pub_transport = config->transport;
        bridge_config.pub_path = config->path;
        bridge_config.pub_tls = config->tls;
        bridge_config.pub_security = config->security;
        bridge_config.pub_topic =
            config->pub_topic ? config->pub_topic
                              : IVR_FMQ_ADAPTER_DEFAULT_PUB_TOPIC;
        bridge_config.handler.context = a;
        bridge_config.handler.get_room_version = ivr_fmq_adapter_get_room_version;
        bridge_config.handler.on_command = ivr_fmq_adapter_on_command;
        bridge_config.handler.on_dispatch_result =
            ivr_fmq_adapter_on_dispatch_result;
        bridge_config.handler.on_release_result =
            ivr_fmq_adapter_on_release_result;
        bridge_config.handler.on_media_result =
            ivr_fmq_adapter_on_media_result;
        bridge_config.handler.on_media_event = ivr_fmq_adapter_on_media_event;
        bridge_config.handler.on_inventory_page =
            ivr_fmq_adapter_on_inventory_page;
        bridge_config.handler.on_tick = ivr_fmq_adapter_on_tick;
        if (ivr_room_bridge_create(&bridge_config, &a->bridge) != IVR_OK) {
            ivr_mutex_destroy(&a->seq_lock);
            free(a->acls);
            free(a->dialogs);
            free(a->assignments);
            free(a->workers);
            free(a->seq_entries);
            free(a);
            return IVR_ENOSPC;
        }
    }
    *out_adapter = a;
    return IVR_OK;
}

ivr_status_t ivr_fmq_adapter_start(ivr_fmq_adapter_t *adapter) {
    if (!adapter) {
        return IVR_EINVAL;
    }
    if (!adapter->bridge) {
        return IVR_OK; /* no endpoint configured */
    }
    ivr_status_t rc = ivr_room_bridge_start(adapter->bridge);
    if (rc == IVR_OK) {
        adapter->started = 1;
    }
    return rc;
}

void ivr_fmq_adapter_stop(ivr_fmq_adapter_t *adapter) {
    if (!adapter || !adapter->bridge || !adapter->started) {
        return;
    }
    ivr_room_bridge_stop(adapter->bridge);
    adapter->started = 0;
}

void ivr_fmq_adapter_destroy(ivr_fmq_adapter_t *adapter) {
    if (!adapter) {
        return;
    }
    if (adapter->bridge) {
        if (adapter->started) {
            ivr_room_bridge_stop(adapter->bridge);
            adapter->started = 0;
        }
        ivr_room_bridge_destroy(adapter->bridge);
        adapter->bridge = NULL;
    }
    free(adapter->acls);
    adapter->acls = NULL;
    free(adapter->seq_entries);
    adapter->seq_entries = NULL;
    free(adapter->assignments);
    adapter->assignments = NULL;
    free(adapter->dialogs);
    adapter->dialogs = NULL;
    free(adapter->workers);
    adapter->workers = NULL;
    ivr_mutex_destroy(&adapter->seq_lock);
    free(adapter);
}

ivr_status_t ivr_fmq_adapter_get_assignment(
    const ivr_fmq_adapter_t *adapter, const char *message_id,
    ivr_fmq_assignment_t *out) {
    if (!adapter || !message_id || !out) {
        return IVR_EINVAL;
    }
    ivr_mutex_lock((ivr_mutex_t *)&adapter->seq_lock);
    const ivr_fmq_assignment_entry_t *entry =
        ivr_fmq_assignment_find_locked((ivr_fmq_adapter_t *)adapter,
                                        message_id);
    if (!entry) {
        ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
        return IVR_ESTATE;
    }
    *out = entry->value;
    ivr_mutex_unlock((ivr_mutex_t *)&adapter->seq_lock);
    return IVR_OK;
}
