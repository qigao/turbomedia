#include "ivr_room_bridge.h"
#include "ivr_frame.h"
#include "ivr_thread.h"
#include "ivr_internal.h"
#include "turbomedia_ivr_v1.h"
#include "turbo_flow_fmq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* bounded message_id result cache (idempotency)                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char message_id[128];
    uint64_t call_generation;
    uint64_t stored_at_ms;
    int valid;
    ivr_room_command_result_t result;
} ivr_dedup_entry_t;

typedef struct {
    ivr_dedup_entry_t *entries;
    uint32_t capacity;
    uint32_t count;
    uint32_t next; /* ring write cursor */
    uint64_t retention_ms;
    uint64_t (*now_ms)(void *ctx);
    void *now_ctx;
    ivr_mutex_t lock;
} ivr_dedup_t;

static uint64_t ivr_dedup_default_now(void *ctx) {
    (void)ctx;
    return turbo_monotonic_ms();
}

static uint64_t ivr_dedup_now(const ivr_dedup_t *d) {
    return d->now_ms ? d->now_ms(d->now_ctx) : ivr_dedup_default_now(NULL);
}

static int ivr_dedup_init(ivr_dedup_t *d, uint32_t capacity,
                          uint64_t retention_ms,
                          uint64_t (*now_ms)(void *ctx), void *now_ctx) {
    memset(d, 0, sizeof(*d));
    d->capacity = capacity ? capacity : 64;
    d->retention_ms = retention_ms ? retention_ms : 60000u;
    d->now_ms = now_ms;
    d->now_ctx = now_ctx;
    d->entries = (ivr_dedup_entry_t *)calloc(d->capacity, sizeof(*d->entries));
    if (!d->entries) {
        d->capacity = 0;
        return -1;
    }
    if (ivr_mutex_init(&d->lock) != 0) {
        free(d->entries);
        d->entries = NULL;
        d->capacity = 0;
        return -1;
    }
    return 0;
}

static void ivr_dedup_destroy(ivr_dedup_t *d) {
    free(d->entries);
    d->entries = NULL;
    ivr_mutex_destroy(&d->lock);
}

/* Returns 1 and fills *result when the message_id was already applied
   inside the retention window; -1 when the cached result is older than the
   retention window (the replay must be explicitly rejected); 0 on a miss. */
static int ivr_dedup_lookup(ivr_dedup_t *d, const char *message_id,
                            uint64_t call_generation,
                            ivr_room_command_result_t *result) {
    uint64_t now_ms = ivr_dedup_now(d);
    ivr_mutex_lock(&d->lock);
    int found = 0;
    for (uint32_t i = 0; i < d->capacity; i++) {
        ivr_dedup_entry_t *entry = &d->entries[i];
        if (!entry->valid ||
            strcmp(entry->message_id, message_id) != 0 ||
            entry->call_generation != call_generation) {
            continue;
        }
        if (now_ms >= entry->stored_at_ms &&
            now_ms - entry->stored_at_ms < d->retention_ms) {
            *result = entry->result;
            found = 1;
        } else {
            found = -1;
        }
        break;
    }
    ivr_mutex_unlock(&d->lock);
    return found;
}

static void ivr_dedup_purge_expired(ivr_dedup_t *d, uint64_t now_ms) {
    for (uint32_t i = 0; i < d->capacity; i++) {
        ivr_dedup_entry_t *entry = &d->entries[i];
        if (entry->valid &&
            (now_ms < entry->stored_at_ms ||
             now_ms - entry->stored_at_ms >= d->retention_ms)) {
            entry->valid = 0;
            if (d->count > 0u) {
                d->count--;
            }
        }
    }
}

static void ivr_dedup_store(ivr_dedup_t *d, const char *message_id,
                            uint64_t call_generation,
                            const ivr_room_command_result_t *result) {
    uint64_t now_ms = ivr_dedup_now(d);
    ivr_mutex_lock(&d->lock);
    /* Evict expired entries first so the bounded ring keeps only live
       retention-window state; the ring still bounds the item count. */
    ivr_dedup_purge_expired(d, now_ms);
    ivr_dedup_entry_t *slot = &d->entries[d->next];
    slot->valid = 1;
    snprintf(slot->message_id, sizeof(slot->message_id), "%s", message_id);
    slot->call_generation = call_generation;
    slot->stored_at_ms = now_ms;
    slot->result = *result;
    d->next = (d->next + 1) % d->capacity;
    if (d->count < d->capacity) {
        d->count++;
    }
    ivr_mutex_unlock(&d->lock);
}

/* ------------------------------------------------------------------ */
/* bounded cloned-request queue                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    turbo_flow_msg_t *items;
    uint32_t capacity;
    uint32_t head;
    uint32_t count;
    uint32_t high_water;
    ivr_mutex_t lock;
    ivr_cond_t cond;
    int stop;
    uint64_t dropped;
} ivr_req_queue_t;

static int ivr_req_queue_init(ivr_req_queue_t *q, uint32_t capacity) {
    memset(q, 0, sizeof(*q));
    q->capacity = capacity ? capacity : 64;
    q->items = (turbo_flow_msg_t *)calloc(q->capacity, sizeof(*q->items));
    if (!q->items) {
        /* keep the struct destroy-safe (capacity 0 + NULL items) so the
           create() failure path can run ivr_req_queue_destroy() */
        q->capacity = 0;
        return -1;
    }
    for (uint32_t i = 0; i < q->capacity; i++) {
        turbo_flow_msg_init(&q->items[i]);
    }
    if (ivr_mutex_init(&q->lock) != 0) {
        free(q->items);
        q->items = NULL;
        q->capacity = 0;
        return -1;
    }
    if (ivr_cond_init(&q->cond) != 0) {
        ivr_mutex_destroy(&q->lock);
        free(q->items);
        q->items = NULL;
        q->capacity = 0;
        return -1;
    }
    return 0;
}

static void ivr_req_queue_destroy(ivr_req_queue_t *q) {
    for (uint32_t i = 0; i < q->capacity; i++) {
        turbo_flow_msg_cleanup(&q->items[i]);
    }
    free(q->items);
    q->items = NULL;
    ivr_mutex_destroy(&q->lock);
    ivr_cond_destroy(&q->cond);
}

/* Moves ownership of `msg` into the queue; returns 0 on success, -1 on full. */
static int ivr_req_queue_push(ivr_req_queue_t *q, turbo_flow_msg_t *msg) {
    ivr_mutex_lock(&q->lock);
    if (q->count >= q->capacity) {
        q->dropped++;
        ivr_mutex_unlock(&q->lock);
        return -1;
    }
    uint32_t tail = (q->head + q->count) % q->capacity;
    q->items[tail] = *msg;
    turbo_flow_msg_init(msg);
    q->count++;
    if (q->count > q->high_water) {
        q->high_water = q->count;
    }
    ivr_cond_signal(&q->cond);
    ivr_mutex_unlock(&q->lock);
    return 0;
}

/* Pops one message; caller owns it. Returns 0, or -1 when stopped. */
/* Returns 0 with a message, 1 on the owner tick, or -1 when stopped. */
static int ivr_req_queue_pop(ivr_req_queue_t *q, turbo_flow_msg_t *out) {
    ivr_mutex_lock(&q->lock);
    while (q->count == 0 && !q->stop) {
        if (!ivr_cond_timedwait(&q->cond, &q->lock, 100u)) {
            ivr_mutex_unlock(&q->lock);
            return 1;
        }
    }
    if (q->count == 0) {
        ivr_mutex_unlock(&q->lock);
        return -1;
    }
    *out = q->items[q->head];
    turbo_flow_msg_init(&q->items[q->head]);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    ivr_mutex_unlock(&q->lock);
    return 0;
}

typedef struct {
    turbo_flow_fmq_event_kind_t kind;
    char peer_identity[128];
} ivr_peer_event_t;

typedef struct {
    ivr_peer_event_t *items;
    uint32_t capacity;
    uint32_t head;
    uint32_t count;
    uint32_t high_water;
    uint64_t dropped;
    int overflowed;
    ivr_mutex_t lock;
} ivr_peer_event_queue_t;

static int ivr_peer_event_queue_init(ivr_peer_event_queue_t *q,
                                     uint32_t capacity) {
    memset(q, 0, sizeof(*q));
    q->capacity = capacity ? capacity : 64;
    q->items = (ivr_peer_event_t *)calloc(q->capacity, sizeof(*q->items));
    if (!q->items) {
        q->capacity = 0;
        return -1;
    }
    if (ivr_mutex_init(&q->lock) != 0) {
        free(q->items);
        q->items = NULL;
        q->capacity = 0;
        return -1;
    }
    return 0;
}

static void ivr_peer_event_queue_destroy(ivr_peer_event_queue_t *q) {
    free(q->items);
    q->items = NULL;
    q->capacity = 0;
    ivr_mutex_destroy(&q->lock);
}

static int ivr_peer_event_queue_push(ivr_peer_event_queue_t *q,
                                     const turbo_flow_fmq_event_t *event) {
    size_t length = event->peer_identity.len;
    ivr_mutex_lock(&q->lock);
    if (!event->peer_identity.data || length == 0 || length >= 128 ||
        q->count >= q->capacity) {
        q->dropped++;
        q->overflowed = 1;
        ivr_mutex_unlock(&q->lock);
        return -1;
    }
    uint32_t tail = (q->head + q->count) % q->capacity;
    q->items[tail].kind = event->kind;
    memcpy(q->items[tail].peer_identity, event->peer_identity.data, length);
    q->items[tail].peer_identity[length] = '\0';
    q->count++;
    if (q->count > q->high_water) {
        q->high_water = q->count;
    }
    ivr_mutex_unlock(&q->lock);
    return 0;
}

/* Returns 1 with one event, 0 when empty, or -1 when overflow invalidated
   ordering. Overflow discards queued events and forces owner fail-closed. */
static int ivr_peer_event_queue_pop(ivr_peer_event_queue_t *q,
                                    ivr_peer_event_t *out) {
    ivr_mutex_lock(&q->lock);
    if (q->overflowed) {
        q->overflowed = 0;
        q->head = 0;
        q->count = 0;
        ivr_mutex_unlock(&q->lock);
        return -1;
    }
    if (q->count == 0) {
        ivr_mutex_unlock(&q->lock);
        return 0;
    }
    *out = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    ivr_mutex_unlock(&q->lock);
    return 1;
}

/* ------------------------------------------------------------------ */
/* bridge                                                              */
/* ------------------------------------------------------------------ */

/* Bounded set of currently connected peer identities, tracked from the FMQ
   connection events (peer_identity on PEER_CONNECTED/DISCONNECTED). Used to
   bind worker.sync registration to a real connected DEALER so a worker cannot
   register under an identity it is not connected as. */
typedef struct {
    char (*ids)[128];
    uint32_t capacity;
    uint32_t count;
    ivr_mutex_t lock;
} ivr_peer_set_t;

/* One captured ROUTER route for a registered worker (from worker.sync). Used
   to push CallDispatchCommandV1 to the worker's DEALER later. */
typedef struct {
    char worker_id[128];
    turbo_flow_msg_t msg; /* owns the detached route + last payload */
    int valid;
} ivr_route_entry_t;

struct ivr_room_bridge_s {
    turbo_flow_fmq_app_t *app;
    turbo_flow_fmq_app_t *pub_app;
    char pub_topic[128];
    DataBind *codec;
    ivr_room_command_handler_t handler;
    ivr_dedup_t dedup;
    ivr_req_queue_t queue;
    ivr_peer_event_queue_t peer_events;
    ivr_thread_t thread;
    /* Guards the worker route table (written by the worker thread during
       worker.sync, read/cloned/invalidated by the public dispatch API). */
    ivr_mutex_t routes_lock;
    /* 1 while the worker thread and FMQ apps are running. start/stop/destroy
       use it so repeated start() or destroy-without-stop cannot leak the
       thread or free objects the thread still references. */
    atomic_int started;
    ivr_peer_set_t peers;
    ivr_route_entry_t *routes;
    uint32_t route_capacity;
    uint32_t route_count;
    uint64_t applied;
    uint64_t dedup_hits;
    uint64_t version_rejects;
    uint64_t drops;
    uint64_t events_published;
    uint64_t dispatches_sent;
    uint64_t dispatches_failed;
    uint64_t dispatch_results;
    uint64_t dispatch_result_rejects;
    uint64_t auth_rejects; /* worker.sync identity not connected */
    uint64_t dedup_expired_rejects; /* replay outside the retention window */
};

/* ------------------------------------------------------------------ */
/* type id -> command name (reverse of the gateway map)                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t type_id;
    const char *command;
} ivr_type_cmd_t;

static const ivr_type_cmd_t kTypeCmd[] = {
    {IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1, "conference.join"},
    {IVR_TYPE_CONFERENCE_LEAVE_COMMAND_V1, "conference.leave"},
    {IVR_TYPE_GET_SNAPSHOT_COMMAND_V1, "get_snapshot"},
    {IVR_TYPE_WORKER_SYNC_COMMAND_V1, "worker.sync"},
    {IVR_TYPE_WORKER_SYNC_COMMAND_V2, "worker.sync.v2"},
    {IVR_TYPE_WORKER_HEARTBEAT_V1, "worker.heartbeat"},
};
#define IVR_TYPE_CMD_COUNT (sizeof(kTypeCmd) / sizeof(kTypeCmd[0]))

static const char *ivr_type_cmd_find(uint16_t type_id) {
    for (size_t i = 0; i < IVR_TYPE_CMD_COUNT; i++) {
        if (kTypeCmd[i].type_id == type_id) {
            return kTypeCmd[i].command;
        }
    }
    return NULL;
}

static const char *ivr_type_name_find(uint16_t type_id) {
    return ivr_frame_type_name(type_id);
}

/* ------------------------------------------------------------------ */
/* pure decode/encode                                                  */
/* ------------------------------------------------------------------ */

static uint64_t field_u64(const DataBindValue *root, const char *field) {
    const DataBindValue *v = data_bind_value_get(root, field);
    return v ? data_bind_value_as_uint64(v) : 0;
}

static uint32_t field_u32(const DataBindValue *root, const char *field) {
    uint64_t value = field_u64(root, field);
    return value <= UINT32_MAX ? (uint32_t)value : UINT32_MAX;
}

static int field_bool(const DataBindValue *root, const char *field) {
    const DataBindValue *v = data_bind_value_get(root, field);
    return v ? data_bind_value_as_bool(v) : 0;
}

static void field_str(const DataBindValue *root, const char *field, char *out,
                      size_t out_size) {
    const DataBindValue *v = data_bind_value_get(root, field);
    const char *s = v ? data_bind_value_as_string(v) : "";
    snprintf(out, out_size, "%s", s ? s : "");
}

ivr_status_t ivr_room_decode_frame(DataBind *codec, const uint8_t *frame,
                                   size_t len, ivr_room_command_t *out) {
    if (!codec || !frame || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    ivr_frame_info_t info;
    if (ivr_frame_decode(frame, len, &info) != IVR_OK) {
        return IVR_ESTATE;
    }
    if (info.kind != IVR_KIND_COMMAND || info.format != IVR_FMT_BIN) {
        return IVR_ESTATE;
    }
    const char *command = ivr_type_cmd_find(info.schema_type_id);
    if (!command) {
        return IVR_ESTATE;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    const char *type_name = ivr_type_name_find(info.schema_type_id);
    if (data_bind_object_from_bin(codec, type_name,
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return IVR_ESTATE;
    }
    const DataBindValue *root = data_bind_object_value(obj);
    out->wire_version =
        info.schema_type_id == IVR_TYPE_WORKER_SYNC_COMMAND_V2 ||
                info.schema_type_id == IVR_TYPE_WORKER_HEARTBEAT_V1
            ? 2u
            : 1u;
    field_str(root, "message_id", out->message_id, sizeof(out->message_id));
    field_str(root, "worker_id", out->worker_id, sizeof(out->worker_id));
    field_str(root, "room_id", out->room_id, sizeof(out->room_id));
    field_str(root, "call_id", out->call_id, sizeof(out->call_id));
    out->call_generation = field_u64(root, "call_generation");
    out->expected_room_version = field_u64(root, "expected_room_version");
    field_str(root, "participant_role", out->participant_role,
              sizeof(out->participant_role));
    field_str(root, "instance_id", out->instance_id,
              sizeof(out->instance_id));
    out->connection_generation = field_u64(root, "connection_generation");
    out->max_sessions = field_u32(root, "max_sessions");
    out->active_sessions = field_u32(root, "active_sessions");
    out->reserved_sessions = field_u32(root, "reserved_sessions");
    out->lease_duration_ms = field_u64(root, "lease_duration_ms");
    out->draining = field_bool(root, "draining");
    out->health_generation = field_u64(root, "health_generation");
    out->health_ready = field_bool(root, "health_ready");
    field_str(root, "capabilities", out->capabilities,
              sizeof(out->capabilities));
    snprintf(out->command, sizeof(out->command), "%s", command);
    data_bind_object_free(obj);
    return IVR_OK;
}

static ivr_status_t ivr_room_decode_call_result(
    DataBind *codec, const uint8_t *frame, size_t len, uint16_t type_id,
    const char *type_name, ivr_dispatch_result_t *out) {
    ivr_frame_info_t info;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    const DataBindValue *root;
    const DataBindValue *status;

    if (!codec || !frame || !type_name || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (ivr_frame_decode(frame, len, &info) != IVR_OK ||
        info.format != IVR_FMT_BIN || info.kind != IVR_KIND_RESULT ||
        info.schema_type_id != type_id) {
        return IVR_ESTATE;
    }
    if (data_bind_object_from_bin(codec, type_name,
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &obj,
                                  &err) != DATA_BIND_OK) {
        return IVR_ESTATE;
    }
    root = data_bind_object_value(obj);
    out->wire_version =
        type_id == IVR_TYPE_CALL_DISPATCH_RESULT_V2 ? 2u : 1u;
    field_str(root, "message_id", out->message_id, sizeof(out->message_id));
    field_str(root, "worker_id", out->worker_id, sizeof(out->worker_id));
    field_str(root, "room_id", out->room_id, sizeof(out->room_id));
    field_str(root, "call_id", out->call_id, sizeof(out->call_id));
    out->call_generation = field_u64(root, "call_generation");
    status = data_bind_value_get(root, "status_code");
    out->status_code = status ? data_bind_value_as_int(status) : IVR_ESTATE;
    field_str(root, "error_code", out->error_code, sizeof(out->error_code));
    field_str(root, "error_message", out->error_message,
              sizeof(out->error_message));
    if (out->wire_version == 2u) {
        field_str(root, "assignment_id", out->assignment_id,
                  sizeof(out->assignment_id));
        field_str(root, "attempt_id", out->attempt_id,
                  sizeof(out->attempt_id));
        field_str(root, "worker_instance_id", out->worker_instance_id,
                  sizeof(out->worker_instance_id));
        out->worker_connection_generation =
            field_u64(root, "worker_connection_generation");
        out->active_sessions = field_u32(root, "active_sessions");
        out->max_sessions = field_u32(root, "max_sessions");
    }
    data_bind_object_free(obj);
    if (out->message_id[0] == '\0' || out->worker_id[0] == '\0' ||
        out->room_id[0] == '\0' || out->call_id[0] == '\0' ||
        out->call_generation == 0 || out->status_code > 0 ||
        (out->wire_version == 2u &&
         (out->assignment_id[0] == '\0' || out->attempt_id[0] == '\0' ||
          out->worker_instance_id[0] == '\0' ||
          out->worker_connection_generation == 0 ||
          out->max_sessions == 0 ||
          out->active_sessions > out->max_sessions))) {
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    return IVR_OK;
}

ivr_status_t ivr_room_decode_dispatch_result(DataBind *codec,
                                             const uint8_t *frame, size_t len,
                                             ivr_dispatch_result_t *out) {
    ivr_frame_info_t info;
    if (!frame || ivr_frame_decode(frame, len, &info) != IVR_OK) {
        return IVR_ESTATE;
    }
    if (info.schema_type_id == IVR_TYPE_CALL_DISPATCH_RESULT_V2) {
        return ivr_room_decode_call_result(
            codec, frame, len, IVR_TYPE_CALL_DISPATCH_RESULT_V2,
            "CallDispatchResultV2", out);
    }
    return ivr_room_decode_call_result(codec, frame, len,
                                       IVR_TYPE_CALL_DISPATCH_RESULT_V1,
                                       "CallDispatchResultV1", out);
}

ivr_status_t ivr_room_decode_release_result(DataBind *codec,
                                            const uint8_t *frame, size_t len,
                                            ivr_release_result_t *out) {
    return ivr_room_decode_call_result(codec, frame, len,
                                       IVR_TYPE_CALL_RELEASE_RESULT_V1,
                                       "CallReleaseResultV1", out);
}

ivr_status_t ivr_room_bridge_encode_participant_joined(
    DataBind *codec, const char *event_id, const char *causation_id,
    const char *worker_id, const char *room_id, const char *call_id,
    uint64_t call_generation, uint64_t room_version, uint64_t sequence,
    uint64_t occurred_at_ms, const char *participant_id,
    const char *participant_role, uint8_t *frame, size_t frame_cap,
    size_t *out_len) {
    if (!codec || !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    char json[1024];
    char num[32];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, json, sizeof(json));
    ivr_json_builder_raw(&jb, "{\"event_id\":");
    ivr_json_builder_string_cstr(&jb, event_id);
    ivr_json_builder_raw(&jb, ",\"causation_id\":");
    ivr_json_builder_string_cstr(&jb, causation_id);
    ivr_json_builder_raw(&jb, ",\"worker_id\":");
    ivr_json_builder_string_cstr(&jb, worker_id);
    ivr_json_builder_raw(&jb, ",\"room_id\":");
    ivr_json_builder_string_cstr(&jb, room_id);
    ivr_json_builder_raw(&jb, ",\"call_id\":");
    ivr_json_builder_string_cstr(&jb, call_id);
    ivr_json_builder_raw(&jb, ",\"call_generation\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)call_generation);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"room_version\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)room_version);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"sequence\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)sequence);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"occurred_at_ms\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)occurred_at_ms);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"participant_id\":");
    ivr_json_builder_string_cstr(&jb, participant_id);
    ivr_json_builder_raw(&jb, ",\"participant_role\":");
    ivr_json_builder_string_cstr(&jb, participant_role);
    ivr_json_builder_raw(&jb, "}");
    if (!ivr_json_builder_ok(&jb)) {
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_json(codec, "ConferenceParticipantJoinedEventV1",
                                   json, strlen(json), &obj, &err) !=
        DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_EVENT;
    info.schema_type_id = IVR_TYPE_CONFERENCE_PARTICIPANT_JOINED_EVENT_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

ivr_status_t ivr_room_bridge_publish_participant_joined(
    ivr_room_bridge_t *bridge, const char *event_id,
    const char *causation_id, const char *worker_id, const char *room_id,
    const char *call_id, uint64_t call_generation, uint64_t room_version,
    uint64_t sequence, const char *participant_id,
    const char *participant_role) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 64u * 1024u];
    size_t len = 0;
    if (!bridge || !bridge->pub_app) {
        return IVR_ESTATE;
    }
    if (ivr_room_bridge_encode_participant_joined(
            bridge->codec, event_id, causation_id, worker_id, room_id, call_id,
            call_generation, room_version, sequence, 0, participant_id,
            participant_role, frame, sizeof(frame), &len) != IVR_OK) {
        return IVR_ESTATE;
    }
    if (turbo_flow_fmq_app_send(bridge->pub_app, frame, len) != TURBO_OK) {
        return IVR_ESTATE;
    }
    bridge->events_published++;
    return IVR_OK;
}

ivr_status_t ivr_room_bridge_publish_worker_lost(
    ivr_room_bridge_t *bridge, const char *event_id,
    const char *causation_id, const char *worker_id, const char *room_id,
    const char *call_id, uint64_t call_generation, uint64_t room_version,
    uint64_t sequence, const char *reason) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 64u * 1024u];
    size_t frame_len = 0;
    char json[1280];
    char num[32];
    ivr_json_builder_t jb;
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (!bridge || !bridge->pub_app || !event_id || !causation_id ||
        !worker_id || !room_id || !call_id || !reason) {
        return IVR_EINVAL;
    }
    ivr_json_builder_init(&jb, json, sizeof(json));
    ivr_json_builder_raw(&jb, "{\"event_id\":");
    ivr_json_builder_string_cstr(&jb, event_id);
    ivr_json_builder_raw(&jb, ",\"causation_id\":");
    ivr_json_builder_string_cstr(&jb, causation_id);
    ivr_json_builder_raw(&jb, ",\"worker_id\":");
    ivr_json_builder_string_cstr(&jb, worker_id);
    ivr_json_builder_raw(&jb, ",\"room_id\":");
    ivr_json_builder_string_cstr(&jb, room_id);
    ivr_json_builder_raw(&jb, ",\"call_id\":");
    ivr_json_builder_string_cstr(&jb, call_id);
    ivr_json_builder_raw(&jb, ",\"call_generation\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)call_generation);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"room_version\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)room_version);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"sequence\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)sequence);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"occurred_at_ms\":0,\"reason\":");
    ivr_json_builder_string_cstr(&jb, reason);
    ivr_json_builder_raw(&jb, "}");
    if (!ivr_json_builder_ok(&jb) ||
        data_bind_object_from_json(bridge->codec, "IvrWorkerLostEventV1",
                                   json, strlen(json), &obj, &err) !=
            DATA_BIND_OK ||
        data_bind_object_serialize_bin(bridge->codec, obj, &bin, &bin_len,
                                       &err) != DATA_BIND_OK) {
        if (obj) {
            data_bind_object_free(obj);
        }
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > sizeof(frame)) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_EVENT;
    info.schema_type_id = IVR_TYPE_IVR_WORKER_LOST_EVENT_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_ESTATE;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    frame_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    if (turbo_flow_fmq_app_send(bridge->pub_app, frame, frame_len) != TURBO_OK) {
        return IVR_ESTATE;
    }
    bridge->events_published++;
    return IVR_OK;
}

ivr_status_t ivr_room_bridge_encode_snapshot(
    DataBind *codec, const char *room_id, const char *call_id,
    uint64_t call_generation, uint64_t room_version, uint64_t last_sequence,
    const char *participant_role, const char *call_state, uint8_t *frame,
    size_t frame_cap, size_t *out_len) {
    if (!codec || !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    char json[1024];
    char num[32];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, json, sizeof(json));
    ivr_json_builder_raw(&jb, "{\"room_id\":");
    ivr_json_builder_string_cstr(&jb, room_id);
    ivr_json_builder_raw(&jb, ",\"call_id\":");
    ivr_json_builder_string_cstr(&jb, call_id);
    ivr_json_builder_raw(&jb, ",\"call_generation\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)call_generation);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"room_version\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)room_version);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"last_sequence\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)last_sequence);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"participant_role\":");
    ivr_json_builder_string_cstr(&jb, participant_role);
    ivr_json_builder_raw(&jb, ",\"call_state\":");
    ivr_json_builder_string_cstr(&jb, call_state);
    ivr_json_builder_raw(&jb, "}");
    if (!ivr_json_builder_ok(&jb)) {
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_json(codec, "RoomSnapshotV1", json, strlen(json),
                                   &obj, &err) != DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_SNAPSHOT;
    info.schema_type_id = IVR_TYPE_ROOM_SNAPSHOT_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

ivr_status_t ivr_room_encode_worker_sync_result(
    DataBind *codec, const char *message_id, const char *worker_id,
    const ivr_room_command_result_t *result, uint8_t *frame,
    size_t frame_cap, size_t *out_len) {
    if (!codec || !message_id || !worker_id || !result || !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    char json[512];
    char num[32];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, json, sizeof(json));
    ivr_json_builder_raw(&jb, "{\"message_id\":");
    ivr_json_builder_string(&jb, message_id, strlen(message_id));
    ivr_json_builder_raw(&jb, ",\"worker_id\":");
    ivr_json_builder_string(&jb, worker_id, strlen(worker_id));
    ivr_json_builder_raw(&jb, ",\"status_code\":");
    snprintf(num, sizeof(num), "%d", result->status_code);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"error_code\":\"\",\"error_message\":");
    ivr_json_builder_string_cstr(&jb, result->error_message);
    ivr_json_builder_raw(&jb, "}");
    if (!ivr_json_builder_ok(&jb)) {
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_json(codec, "WorkerSyncResultV1", json,
                                   strlen(json), &obj, &err) != DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_RESULT;
    info.schema_type_id = IVR_TYPE_WORKER_SYNC_RESULT_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

ivr_status_t ivr_room_encode_result(
    DataBind *codec, const char *message_id, const char *worker_id,
    const char *room_id, const char *call_id, uint64_t call_generation,
    const ivr_room_command_result_t *result, uint8_t *frame,
    size_t frame_cap, size_t *out_len) {
    if (!codec || !result || !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    char json[1024];
    char num[32];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, json, sizeof(json));
    ivr_json_builder_raw(&jb, "{\"message_id\":");
    ivr_json_builder_string_cstr(&jb, message_id);
    ivr_json_builder_raw(&jb, ",\"worker_id\":");
    ivr_json_builder_string_cstr(&jb, worker_id);
    ivr_json_builder_raw(&jb, ",\"room_id\":");
    ivr_json_builder_string_cstr(&jb, room_id);
    ivr_json_builder_raw(&jb, ",\"call_id\":");
    ivr_json_builder_string_cstr(&jb, call_id);
    ivr_json_builder_raw(&jb, ",\"call_generation\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)call_generation);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"status_code\":");
    snprintf(num, sizeof(num), "%d", result->status_code);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"room_version\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)result->room_version);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"sequence\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)result->sequence);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"error_code\":\"\",\"error_message\":");
    ivr_json_builder_string_cstr(&jb, result->error_message);
    ivr_json_builder_raw(&jb, "}");
    if (!ivr_json_builder_ok(&jb)) {
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_json(codec, "IvrCommandResultV1", json,
                                   strlen(json), &obj, &err) != DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_RESULT;
    info.schema_type_id = IVR_TYPE_IVR_COMMAND_RESULT_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

/* ------------------------------------------------------------------ */
/* connected peer identity set (from FMQ connection events)              */
/* ------------------------------------------------------------------ */

static void peer_add(ivr_peer_set_t *p, const char *data, size_t len) {
    if (!data || len == 0) {
        return;
    }
    if (len > 127) {
        len = 127;
    }
    ivr_mutex_lock(&p->lock);
    for (uint32_t i = 0; i < p->count; i++) {
        if (strncmp(p->ids[i], data, len) == 0 && p->ids[i][len] == '\0') {
            ivr_mutex_unlock(&p->lock);
            return;
        }
    }
    if (p->count < p->capacity) {
        memcpy(p->ids[p->count], data, len);
        p->ids[p->count][len] = '\0';
        p->count++;
    }
    ivr_mutex_unlock(&p->lock);
}

static void peer_remove(ivr_peer_set_t *p, const char *data, size_t len) {
    if (!data || len == 0) {
        return;
    }
    if (len > 127) {
        len = 127;
    }
    ivr_mutex_lock(&p->lock);
    for (uint32_t i = 0; i < p->count; i++) {
        if (strncmp(p->ids[i], data, len) == 0 && p->ids[i][len] == '\0') {
            for (uint32_t j = i + 1; j < p->count; j++) {
                memcpy(p->ids[j - 1], p->ids[j], 128);
            }
            p->count--;
            break;
        }
    }
    ivr_mutex_unlock(&p->lock);
}

static int peer_connected(const ivr_peer_set_t *p, const char *id) {
    if (!p || !id) {
        return 0;
    }
    int found = 0;
    ivr_mutex_lock((ivr_mutex_t *)&p->lock);
    for (uint32_t i = 0; i < p->count; i++) {
        if (strcmp(p->ids[i], id) == 0) {
            found = 1;
            break;
        }
    }
    ivr_mutex_unlock((ivr_mutex_t *)&p->lock);
    return found;
}

static void route_invalidate_peer(ivr_room_bridge_t *bridge,
                                  const char *peer_identity, size_t length);

static void bridge_on_fmq_event(void *ctx, const turbo_flow_fmq_event_t *event) {
    ivr_room_bridge_t *b = (ivr_room_bridge_t *)ctx;
    if (!b || !event) {
        return;
    }
    if (event->kind == TURBO_FLOW_FMQ_EVENT_PEER_CONNECTED ||
        event->kind == TURBO_FLOW_FMQ_EVENT_PEER_DISCONNECTED) {
        (void)ivr_peer_event_queue_push(&b->peer_events, event);
    }
}

/* ------------------------------------------------------------------ */
/* worker route registry (bounded) + dispatch push                      */
/* ------------------------------------------------------------------ */

/* Must be called with b->routes_lock held: the worker thread writes routes
   during worker.sync while the public dispatch API reads/clones/invalidates
   them, so every route access goes through routes_lock. */
static ivr_route_entry_t *route_find_locked(ivr_room_bridge_t *b,
                                            const char *worker_id) {
    for (uint32_t i = 0; i < b->route_count; i++) {
        if (strcmp(b->routes[i].worker_id, worker_id) == 0) {
            return &b->routes[i];
        }
    }
    return NULL;
}

static void route_invalidate_peer(ivr_room_bridge_t *bridge,
                                  const char *peer_identity, size_t length) {
    if (!bridge || !peer_identity || length == 0 || length >= 128) {
        return;
    }
    ivr_mutex_lock(&bridge->routes_lock);
    for (uint32_t i = 0; i < bridge->route_count; i++) {
        ivr_route_entry_t *entry = &bridge->routes[i];
        if (entry->valid && strncmp(entry->worker_id, peer_identity, length) == 0 &&
            entry->worker_id[length] == '\0') {
            entry->valid = 0;
            turbo_flow_msg_cleanup(&entry->msg);
            turbo_flow_msg_init(&entry->msg);
        }
    }
    ivr_mutex_unlock(&bridge->routes_lock);
}

static void peer_and_routes_clear(ivr_room_bridge_t *bridge) {
    ivr_mutex_lock(&bridge->peers.lock);
    bridge->peers.count = 0;
    ivr_mutex_unlock(&bridge->peers.lock);
    ivr_mutex_lock(&bridge->routes_lock);
    for (uint32_t i = 0; i < bridge->route_count; i++) {
        ivr_route_entry_t *entry = &bridge->routes[i];
        if (entry->valid) {
            entry->valid = 0;
            turbo_flow_msg_cleanup(&entry->msg);
            turbo_flow_msg_init(&entry->msg);
        }
    }
    ivr_mutex_unlock(&bridge->routes_lock);
}

static void ivr_bridge_process_peer_events(ivr_room_bridge_t *bridge) {
    ivr_peer_event_t event;
    for (;;) {
        int rc = ivr_peer_event_queue_pop(&bridge->peer_events, &event);
        if (rc == 0) {
            return;
        }
        if (rc < 0) {
            peer_and_routes_clear(bridge);
            return;
        }
        size_t identity_len = strlen(event.peer_identity);
        if (event.kind == TURBO_FLOW_FMQ_EVENT_PEER_CONNECTED) {
            peer_add(&bridge->peers, event.peer_identity, identity_len);
        } else if (event.kind == TURBO_FLOW_FMQ_EVENT_PEER_DISCONNECTED) {
            peer_remove(&bridge->peers, event.peer_identity, identity_len);
            route_invalidate_peer(bridge, event.peer_identity, identity_len);
        }
    }
}

int ivr_room_bridge_worker_available(const ivr_room_bridge_t *bridge,
                                     const char *worker_id) {
    int route_available = 0;
    if (!bridge || !worker_id || worker_id[0] == '\0') {
        return 0;
    }
    ivr_mutex_lock((ivr_mutex_t *)&bridge->routes_lock);
    ivr_route_entry_t *entry = route_find_locked((ivr_room_bridge_t *)bridge,
                                                  worker_id);
    route_available = entry && entry->valid;
    ivr_mutex_unlock((ivr_mutex_t *)&bridge->routes_lock);
    return route_available && peer_connected(&bridge->peers, worker_id);
}

void ivr_room_bridge_get_stats(const ivr_room_bridge_t *bridge,
                               ivr_room_bridge_stats_t *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!bridge) {
        return;
    }
    ivr_mutex_lock((ivr_mutex_t *)&bridge->queue.lock);
    out->request_queue_items = bridge->queue.count;
    out->request_queue_capacity = bridge->queue.capacity;
    out->request_queue_high_water = bridge->queue.high_water;
    out->request_queue_drops = bridge->queue.dropped;
    ivr_mutex_unlock((ivr_mutex_t *)&bridge->queue.lock);
    ivr_mutex_lock((ivr_mutex_t *)&bridge->peer_events.lock);
    out->peer_event_queue_items = bridge->peer_events.count;
    out->peer_event_queue_capacity = bridge->peer_events.capacity;
    out->peer_event_queue_high_water = bridge->peer_events.high_water;
    out->peer_event_queue_drops = bridge->peer_events.dropped;
    out->peer_event_queue_overflowed = bridge->peer_events.overflowed;
    ivr_mutex_unlock((ivr_mutex_t *)&bridge->peer_events.lock);
    ivr_mutex_lock((ivr_mutex_t *)&bridge->dedup.lock);
    out->dedup_hits = bridge->dedup_hits;
    out->dedup_expired_rejects = bridge->dedup_expired_rejects;
    out->version_rejects = bridge->version_rejects;
    out->auth_rejects = bridge->auth_rejects;
    out->dispatch_result_rejects = bridge->dispatch_result_rejects;
    ivr_mutex_unlock((ivr_mutex_t *)&bridge->dedup.lock);
}

/* Capture/refresh the ROUTER route of a registered worker (called on a
   successful worker.sync so the route is generation-fenced and current). */
static void route_store(ivr_room_bridge_t *b, const char *worker_id,
                        turbo_flow_msg_t *msg) {
    ivr_mutex_lock(&b->routes_lock);
    ivr_route_entry_t *entry = route_find_locked(b, worker_id);
    if (!entry && b->route_count < b->route_capacity) {
        entry = &b->routes[b->route_count++];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->worker_id, sizeof(entry->worker_id), "%s", worker_id);
    }
    if (!entry) {
        for (uint32_t i = 0; i < b->route_count; i++) {
            if (!b->routes[i].valid) {
                entry = &b->routes[i];
                snprintf(entry->worker_id, sizeof(entry->worker_id), "%s",
                         worker_id);
                break;
            }
        }
    }
    if (!entry) {
        ivr_mutex_unlock(&b->routes_lock);
        return; /* route table full: dispatch to this worker stays unavailable */
    }
    if (entry->valid) {
        turbo_flow_msg_cleanup(&entry->msg);
        turbo_flow_msg_init(&entry->msg);
    }
    entry->valid = 0;
    if (turbo_flow_msg_clone(&entry->msg, msg) == TURBO_OK) {
        entry->valid = 1;
    }
    ivr_mutex_unlock(&b->routes_lock);
}

/* Bind an inbound result to the exact live ROUTER session captured by the
   worker's latest successful worker.sync. A connected peer name alone is not
   sufficient because another connected DEALER could spoof worker_id. */
static int route_matches_worker(ivr_room_bridge_t *b, const char *worker_id,
                                const turbo_flow_msg_t *msg) {
    const turbo_flow_protocol_route_t *incoming;
    int matches = 0;

    if (!b || !worker_id || !msg || !peer_connected(&b->peers, worker_id)) {
        return 0;
    }
    incoming = turbo_flow_msg_protocol_route(msg);
    if (!incoming) {
        return 0;
    }
    ivr_mutex_lock(&b->routes_lock);
    ivr_route_entry_t *entry = route_find_locked(b, worker_id);
    const turbo_flow_protocol_route_t *registered =
        entry && entry->valid ? turbo_flow_msg_protocol_route(&entry->msg)
                              : NULL;
    if (registered && registered->protocol == incoming->protocol &&
        registered->owner_instance_id == incoming->owner_instance_id &&
        registered->session_id == incoming->session_id &&
        registered->session_generation == incoming->session_generation) {
        matches = 1;
    }
    ivr_mutex_unlock(&b->routes_lock);
    return matches;
}

ivr_status_t ivr_room_bridge_encode_dispatch(
    DataBind *codec, const char *message_id, const char *worker_id,
    const char *room_id, const char *call_id, uint64_t call_generation,
    uint64_t expected_room_version, const char *content_package,
    uint8_t *frame, size_t frame_cap, size_t *out_len) {
    if (!codec || !message_id || !worker_id || !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    char json[1024];
    char num[32];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, json, sizeof(json));
    ivr_json_builder_raw(&jb, "{\"message_id\":");
    ivr_json_builder_string(&jb, message_id, strlen(message_id));
    ivr_json_builder_raw(&jb, ",\"worker_id\":");
    ivr_json_builder_string(&jb, worker_id, strlen(worker_id));
    ivr_json_builder_raw(&jb, ",\"room_id\":");
    ivr_json_builder_string_cstr(&jb, room_id);
    ivr_json_builder_raw(&jb, ",\"call_id\":");
    ivr_json_builder_string_cstr(&jb, call_id);
    ivr_json_builder_raw(&jb, ",\"call_generation\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)call_generation);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"expected_room_version\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)expected_room_version);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"content_package\":");
    ivr_json_builder_string_cstr(&jb, content_package);
    ivr_json_builder_raw(&jb, "}");
    if (!ivr_json_builder_ok(&jb)) {
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_json(codec, "CallDispatchCommandV1", json,
                                   strlen(json), &obj, &err) != DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_CALL_DISPATCH_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

ivr_status_t ivr_room_bridge_encode_release(
    DataBind *codec, const char *message_id, const char *worker_id,
    const char *room_id, const char *call_id, uint64_t call_generation,
    const char *reason, uint8_t *frame, size_t frame_cap, size_t *out_len) {
    if (!codec || !message_id || !worker_id || !room_id || !call_id ||
        !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    char json[1024];
    char num[32];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, json, sizeof(json));
    ivr_json_builder_raw(&jb, "{\"message_id\":");
    ivr_json_builder_string_cstr(&jb, message_id);
    ivr_json_builder_raw(&jb, ",\"worker_id\":");
    ivr_json_builder_string_cstr(&jb, worker_id);
    ivr_json_builder_raw(&jb, ",\"room_id\":");
    ivr_json_builder_string_cstr(&jb, room_id);
    ivr_json_builder_raw(&jb, ",\"call_id\":");
    ivr_json_builder_string_cstr(&jb, call_id);
    ivr_json_builder_raw(&jb, ",\"call_generation\":");
    snprintf(num, sizeof(num), "%llu", (unsigned long long)call_generation);
    ivr_json_builder_raw(&jb, num);
    ivr_json_builder_raw(&jb, ",\"reason\":");
    ivr_json_builder_string_cstr(&jb, reason ? reason : "conference.leave");
    ivr_json_builder_raw(&jb, "}");
    if (!ivr_json_builder_ok(&jb)) {
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_json(codec, "CallReleaseCommandV1", json,
                                   strlen(json), &obj, &err) != DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_CALL_RELEASE_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

ivr_status_t ivr_room_bridge_encode_dispatch_v2(
    DataBind *codec, const ivr_call_dispatch_t *dispatch, uint8_t *frame,
    size_t frame_cap, size_t *out_len) {
    if (!codec || !dispatch || dispatch->wire_version != 2u ||
        !dispatch->message_id[0] || !dispatch->assignment_id[0] ||
        !dispatch->attempt_id[0] || !dispatch->worker_id[0] ||
        !dispatch->worker_instance_id[0] ||
        dispatch->worker_connection_generation == 0 ||
        !dispatch->room_id[0] || !dispatch->call_id[0] ||
        dispatch->call_generation == 0 || !dispatch->content_package[0] ||
        dispatch->deadline_timeout_ms == 0 || !frame || !out_len) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    char json[2048];
    char number[32];
    ivr_json_builder_t jb;
    ivr_json_builder_init(&jb, json, sizeof(json));
#define IVR_V2_DISPATCH_STRING(name, value)                                    \
    do {                                                                        \
        ivr_json_builder_raw(&jb, ",\"" name "\":");                        \
        ivr_json_builder_string_cstr(&jb, (value));                             \
    } while (0)
    ivr_json_builder_raw(&jb, "{\"message_id\":");
    ivr_json_builder_string_cstr(&jb, dispatch->message_id);
    IVR_V2_DISPATCH_STRING("assignment_id", dispatch->assignment_id);
    IVR_V2_DISPATCH_STRING("attempt_id", dispatch->attempt_id);
    IVR_V2_DISPATCH_STRING("worker_id", dispatch->worker_id);
    IVR_V2_DISPATCH_STRING("worker_instance_id",
                           dispatch->worker_instance_id);
    ivr_json_builder_raw(&jb, ",\"worker_connection_generation\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)dispatch->worker_connection_generation);
    ivr_json_builder_raw(&jb, number);
    IVR_V2_DISPATCH_STRING("room_id", dispatch->room_id);
    IVR_V2_DISPATCH_STRING("call_id", dispatch->call_id);
    ivr_json_builder_raw(&jb, ",\"call_generation\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)dispatch->call_generation);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, ",\"expected_room_version\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)dispatch->expected_room_version);
    ivr_json_builder_raw(&jb, number);
    IVR_V2_DISPATCH_STRING("content_package", dispatch->content_package);
    IVR_V2_DISPATCH_STRING("content_version", dispatch->content_version);
    ivr_json_builder_raw(&jb, ",\"deadline_timeout_ms\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)dispatch->deadline_timeout_ms);
    ivr_json_builder_raw(&jb, number);
    ivr_json_builder_raw(&jb, "}");
#undef IVR_V2_DISPATCH_STRING
    if (!ivr_json_builder_ok(&jb)) {
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    if (data_bind_object_from_json(codec, "CallDispatchCommandV2", json,
                                   strlen(json), &obj, &err) != DATA_BIND_OK) {
        return IVR_EINVAL;
    }
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (data_bind_object_serialize_bin(codec, obj, &bin, &bin_len, &err) !=
        DATA_BIND_OK) {
        data_bind_object_free(obj);
        return IVR_ESTATE;
    }
    data_bind_object_free(obj);
    if (IVR_FRAME_HEADER_SIZE + bin_len > frame_cap) {
        data_bind_binary_free(bin);
        return IVR_ENOSPC;
    }
    ivr_frame_info_t info;
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_CALL_DISPATCH_COMMAND_V2;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(bin);
        return IVR_EINVAL;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, bin, bin_len);
    *out_len = IVR_FRAME_HEADER_SIZE + bin_len;
    data_bind_binary_free(bin);
    return IVR_OK;
}

static ivr_status_t ivr_room_bridge_send_to_worker(
    ivr_room_bridge_t *bridge, const char *worker_id, const uint8_t *frame,
    size_t len) {
    turbo_flow_msg_t push;
    turbo_flow_msg_init(&push);
    ivr_mutex_lock(&bridge->routes_lock);
    ivr_route_entry_t *entry = route_find_locked(bridge, worker_id);
    if (!entry || !entry->valid) {
        ivr_mutex_unlock(&bridge->routes_lock);
        return IVR_ESTATE;
    }
    int clone_rc = turbo_flow_msg_clone(&push, &entry->msg);
    ivr_mutex_unlock(&bridge->routes_lock);
    if (clone_rc != TURBO_OK ||
        turbo_flow_fmq_app_message_set_payload_copy(&push, frame, len) !=
            TURBO_OK ||
        turbo_flow_fmq_app_send_message(bridge->app, &push) != TURBO_OK) {
        turbo_flow_msg_cleanup(&push);
        ivr_mutex_lock(&bridge->routes_lock);
        entry = route_find_locked(bridge, worker_id);
        if (entry) {
            entry->valid = 0;
            turbo_flow_msg_cleanup(&entry->msg);
            turbo_flow_msg_init(&entry->msg);
        }
        ivr_mutex_unlock(&bridge->routes_lock);
        return IVR_ESTATE;
    }
    turbo_flow_msg_cleanup(&push);
    return IVR_OK;
}

ivr_status_t ivr_room_bridge_dispatch_call(
    ivr_room_bridge_t *bridge, const char *worker_id, const char *message_id,
    const char *room_id, const char *call_id, uint64_t call_generation,
    uint64_t expected_room_version, const char *content_package) {
    if (!bridge || !worker_id || !message_id) {
        return IVR_EINVAL;
    }
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4u * 1024u];
    size_t len = 0;
    if (ivr_room_bridge_encode_dispatch(
            bridge->codec, message_id, worker_id, room_id, call_id,
            call_generation, expected_room_version, content_package, frame,
            sizeof(frame), &len) != IVR_OK) {
        bridge->dispatches_failed++;
        return IVR_ESTATE;
    }
    if (ivr_room_bridge_send_to_worker(bridge, worker_id, frame, len) !=
        IVR_OK) {
        bridge->dispatches_failed++;
        return IVR_ESTATE;
    }
    bridge->dispatches_sent++;
    return IVR_OK;
}

ivr_status_t ivr_room_bridge_dispatch_call_v2(
    ivr_room_bridge_t *bridge, const ivr_call_dispatch_t *dispatch) {
    if (!bridge || !dispatch) {
        return IVR_EINVAL;
    }
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4u * 1024u];
    size_t len = 0;
    if (ivr_room_bridge_encode_dispatch_v2(bridge->codec, dispatch, frame,
                                           sizeof(frame), &len) != IVR_OK ||
        ivr_room_bridge_send_to_worker(bridge, dispatch->worker_id, frame,
                                       len) != IVR_OK) {
        bridge->dispatches_failed++;
        return IVR_ESTATE;
    }
    bridge->dispatches_sent++;
    return IVR_OK;
}

ivr_status_t ivr_room_bridge_release_call(
    ivr_room_bridge_t *bridge, const char *worker_id, const char *message_id,
    const char *room_id, const char *call_id, uint64_t call_generation,
    const char *reason) {
    if (!bridge || !worker_id || !message_id) {
        return IVR_EINVAL;
    }
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4u * 1024u];
    size_t len = 0;
    if (ivr_room_bridge_encode_release(
            bridge->codec, message_id, worker_id, room_id, call_id,
            call_generation, reason, frame, sizeof(frame), &len) != IVR_OK) {
        bridge->dispatches_failed++;
        return IVR_ESTATE;
    }
    if (ivr_room_bridge_send_to_worker(bridge, worker_id, frame, len) !=
        IVR_OK) {
        bridge->dispatches_failed++;
        return IVR_ESTATE;
    }
    bridge->dispatches_sent++;
    return IVR_OK;
}

/* ------------------------------------------------------------------ */
/* command handling (bridge worker thread)                             */
/* ------------------------------------------------------------------ */

static void ivr_bridge_reply(ivr_room_bridge_t *b, turbo_flow_msg_t *msg,
                             const ivr_room_command_t *cmd,
                             const ivr_room_command_result_t *result) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 64u * 1024u];
    size_t len = 0;
    ivr_status_t rc;
    if (strcmp(cmd->command, "worker.sync") == 0 ||
        strcmp(cmd->command, "worker.sync.v2") == 0 ||
        strcmp(cmd->command, "worker.heartbeat") == 0) {
        rc = ivr_room_encode_worker_sync_result(
            b->codec, cmd->message_id, cmd->worker_id, result, frame,
            sizeof(frame), &len);
    } else {
        rc = ivr_room_encode_result(
            b->codec, cmd->message_id, cmd->worker_id, cmd->room_id,
            cmd->call_id, cmd->call_generation, result, frame,
            sizeof(frame), &len);
    }
    if (rc != IVR_OK) {
        return;
    }
    if (turbo_flow_fmq_app_message_set_payload_copy(msg, frame, len) !=
        TURBO_OK) {
        return;
    }
    (void)turbo_flow_fmq_app_send_message(b->app, msg);
}

static void ivr_bridge_handle_request(ivr_room_bridge_t *b,
                                      turbo_flow_msg_t *msg) {
    ivr_frame_info_t info;
    if (ivr_frame_decode((const uint8_t *)msg->payload.data, msg->payload.len,
                         &info) != IVR_OK) {
        return;
    }
    if (info.kind == IVR_KIND_RESULT &&
        (info.schema_type_id == IVR_TYPE_CALL_DISPATCH_RESULT_V1 ||
         info.schema_type_id == IVR_TYPE_CALL_DISPATCH_RESULT_V2)) {
        ivr_dispatch_result_t dispatch_result;
        if (ivr_room_decode_dispatch_result(
                b->codec, (const uint8_t *)msg->payload.data, msg->payload.len,
                &dispatch_result) != IVR_OK ||
            !route_matches_worker(b, dispatch_result.worker_id, msg)) {
            b->dispatch_result_rejects++;
            return;
        }
        if (!b->handler.on_dispatch_result ||
            b->handler.on_dispatch_result(b->handler.context,
                                          &dispatch_result) != IVR_OK) {
            b->dispatch_result_rejects++;
            return;
        }
        b->dispatch_results++;
        return;
    }
    if (info.kind == IVR_KIND_RESULT &&
        info.schema_type_id == IVR_TYPE_CALL_RELEASE_RESULT_V1) {
        ivr_release_result_t release_result;
        if (ivr_room_decode_release_result(
                b->codec, (const uint8_t *)msg->payload.data, msg->payload.len,
                &release_result) != IVR_OK ||
            !route_matches_worker(b, release_result.worker_id, msg)) {
            b->dispatch_result_rejects++;
            return;
        }
        if (!b->handler.on_release_result ||
            b->handler.on_release_result(b->handler.context,
                                         &release_result) != IVR_OK) {
            b->dispatch_result_rejects++;
            return;
        }
        b->dispatch_results++;
        return;
    }
    if (info.kind != IVR_KIND_COMMAND) {
        return;
    }
    ivr_room_command_t cmd;
    if (ivr_room_decode_frame(b->codec, (const uint8_t *)msg->payload.data,
                              msg->payload.len, &cmd) != IVR_OK) {
        return; /* malformed frame: no reply (fail fast, drop + count below) */
    }
    ivr_room_command_result_t result;
    memset(&result, 0, sizeof(result));
    int dedup_rc = ivr_dedup_lookup(&b->dedup, cmd.message_id,
                                    cmd.call_generation, &result);
    if (dedup_rc > 0) {
        b->dedup_hits++;
        ivr_bridge_reply(b, msg, &cmd, &result);
        return;
    }
    if (dedup_rc < 0) {
        /* A replay outside the retention window is explicitly rejected:
           fail fast, never silently re-apply the mutation. */
        memset(&result, 0, sizeof(result));
        result.status_code = IVR_ESTALE;
        snprintf(result.error_message, sizeof(result.error_message),
                 "replay outside dedup retention window");
        b->dedup_expired_rejects++;
        ivr_bridge_reply(b, msg, &cmd, &result);
        return;
    }
    if (strcmp(cmd.command, "worker.sync") == 0 ||
        strcmp(cmd.command, "worker.sync.v2") == 0 ||
        strcmp(cmd.command, "worker.heartbeat") == 0) {
        /* Registration binds the claimed worker_id to a real connected DEALER
           identity (tracked from FMQ connection events): a worker must not
           register under an identity it is not connected as. */
        if (!peer_connected(&b->peers, cmd.worker_id)) {
            result.status_code = IVR_EAUTH;
            snprintf(result.error_message, sizeof(result.error_message),
                     "worker identity not connected");
            b->auth_rejects++;
            ivr_dedup_store(&b->dedup, cmd.message_id, cmd.call_generation,
                            &result);
            ivr_bridge_reply(b, msg, &cmd, &result);
            return;
        }
    }
    if (b->handler.get_room_version) {
        cmd.current_room_version =
            b->handler.get_room_version(b->handler.context, cmd.room_id);
        if (cmd.expected_room_version != 0 &&
            cmd.current_room_version != 0 &&
            cmd.expected_room_version != cmd.current_room_version) {
            result.status_code = IVR_EVERSION;
            snprintf(result.error_message, sizeof(result.error_message),
                     "stale expected_room_version");
            b->version_rejects++;
            ivr_dedup_store(&b->dedup, cmd.message_id, cmd.call_generation,
                            &result);
            ivr_bridge_reply(b, msg, &cmd, &result);
            return;
        }
    }
    if (b->handler.on_command) {
        if (b->handler.on_command(b->handler.context, &cmd, &result) !=
            IVR_OK) {
            result.status_code = result.status_code ? result.status_code
                                                    : IVR_ESTATE;
        }
    } else {
        result.status_code = IVR_ESTATE;
        snprintf(result.error_message, sizeof(result.error_message),
                 "no command handler");
    }
    if (result.status_code == 0) {
        b->applied++;
        if (strcmp(cmd.command, "worker.sync") == 0 ||
            strcmp(cmd.command, "worker.sync.v2") == 0 ||
            strcmp(cmd.command, "worker.heartbeat") == 0) {
            /* capture the worker's current ROUTER route for later dispatch
               pushes; re-registration refreshes it after a reconnect */
            route_store(b, cmd.worker_id, msg);
        }
        /* A join event is gated by CallDispatchResultV1 accepted so the
           worker session exists before its first sequenced event arrives. */
        if (b->pub_app && strcmp(cmd.command, "get_snapshot") == 0) {
            /* authoritative snapshot domain event: confirms continuity for the
               worker after a sequence gap (worker.sync/get_snapshot) */
            uint8_t ev[IVR_FRAME_HEADER_SIZE + 64u * 1024u];
            size_t ev_len = 0;
            if (ivr_room_bridge_encode_snapshot(
                    b->codec, cmd.room_id, cmd.call_id, cmd.call_generation,
                    result.room_version, result.sequence, "", "", ev,
                    sizeof(ev), &ev_len) == IVR_OK) {
                if (turbo_flow_fmq_app_send(b->pub_app, ev, ev_len) == TURBO_OK) {
                    b->events_published++;
                }
            }
        }
    }
    if (!result.retryable) {
        ivr_dedup_store(&b->dedup, cmd.message_id, cmd.call_generation,
                        &result);
    }
    ivr_bridge_reply(b, msg, &cmd, &result);
}

static void *ivr_bridge_thread_main(void *opaque) {
    ivr_room_bridge_t *b = (ivr_room_bridge_t *)opaque;
    turbo_flow_msg_t msg;
    turbo_flow_msg_init(&msg);
    for (;;) {
        int pop_rc = ivr_req_queue_pop(&b->queue, &msg);
        if (pop_rc < 0) {
            break;
        }
        if (pop_rc > 0) {
            ivr_bridge_process_peer_events(b);
            if (b->handler.on_tick) {
                b->handler.on_tick(b->handler.context);
            }
            continue;
        }
        ivr_bridge_process_peer_events(b);
        ivr_bridge_handle_request(b, &msg);
        turbo_flow_msg_cleanup(&msg);
        turbo_flow_msg_init(&msg);
    }
    turbo_flow_msg_cleanup(&msg);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* FlowMQ ROUTER ingress callback                                      */
/* ------------------------------------------------------------------ */

static int bridge_on_router_request(turbo_flow_fmq_app_t *app,
                                    turbo_flow_msg_t *message, void *ctx) {
    ivr_room_bridge_t *b = (ivr_room_bridge_t *)ctx;
    (void)app;
    if (!b || !message) {
        return TURBO_EINVAL;
    }
    turbo_flow_msg_t clone;
    turbo_flow_msg_init(&clone);
    if (turbo_flow_fmq_message_detach_router_route(message) != TURBO_OK ||
        turbo_flow_msg_clone(&clone, message) != TURBO_OK) {
        turbo_flow_msg_cleanup(&clone);
        return TURBO_OK;
    }
    if (ivr_req_queue_push(&b->queue, &clone) != 0) {
        turbo_flow_msg_cleanup(&clone);
        b->drops++;
    }
    return TURBO_OK;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

ivr_status_t ivr_room_bridge_create(const ivr_room_bridge_config_t *config,
                                    ivr_room_bridge_t **out_bridge) {
    if (!config || !config->host || config->port <= 0 || !out_bridge) {
        return IVR_EINVAL;
    }
    ivr_room_bridge_t *b = (ivr_room_bridge_t *)calloc(1, sizeof(*b));
    if (!b) {
        return IVR_ENOSPC;
    }
    b->handler = config->handler;
    b->route_capacity = 64;
    b->routes = (ivr_route_entry_t *)calloc(b->route_capacity,
                                            sizeof(*b->routes));
    if (!b->routes) {
        free(b);
        return IVR_ENOSPC;
    }
    DataBindError err = DATA_BIND_ERROR_INIT;
    if (TurboMediaIvrV1_codec_create(&b->codec, &err) != DATA_BIND_OK ||
        ivr_dedup_init(&b->dedup, config->dedup_capacity,
                                  config->dedup_retention_ms, config->now_ms,
                                  config->now_ctx) != 0 ||
        ivr_req_queue_init(&b->queue, config->queue_capacity) != 0 ||
        ivr_peer_event_queue_init(&b->peer_events,
                                  config->queue_capacity) != 0 ||
        ivr_mutex_init(&b->routes_lock) != 0 ||
        ivr_mutex_init(&b->peers.lock) != 0) {
        goto fail_resources;
    }
    atomic_init(&b->started, 0);

    b->peers.capacity = 64;
    b->peers.ids = (char(*)[128])calloc(b->peers.capacity, 128);
    if (!b->peers.ids) {
        goto fail_resources;
    }
    turbo_flow_fmq_config_t ep = TURBO_FLOW_FMQ_CONFIG_INIT;
    ep.pattern = TURBO_FLOW_FMQ_ROUTER;
    ep.mode = TURBO_FLOW_FMQ_BIND;
    ep.transport = config->transport ? config->transport : TURBO_FLOW_FMQ_TCP;
    ep.host = config->host;
    ep.port = config->port;
    ep.max_frame_size = TURBO_FLOW_FMQ_DEFAULT_MAX_FRAME_SIZE;
    ep.timeout_ms = config->timeout_ms ? config->timeout_ms : 5000;
    ep.tls = config->tls;
    ep.path = config->path ? config->path : "/";
    ep.event_callback = bridge_on_fmq_event;
    ep.event_ctx = b;

    turbo_flow_fmq_app_options_t opt = TURBO_FLOW_FMQ_APP_OPTIONS_INIT;
    opt.on_message = bridge_on_router_request;
    opt.message_ctx = b;
    int app_rc = config->security
                     ? turbo_flow_fmq_app_create_secure(&ep, &opt,
                                                        config->security,
                                                        &b->app)
                     : turbo_flow_fmq_app_create(&ep, &opt, &b->app);
    if (app_rc != TURBO_OK) {
        goto fail_resources;
    }
    if (config->pub_port > 0) {
        snprintf(b->pub_topic, sizeof(b->pub_topic), "%s",
                 config->pub_topic ? config->pub_topic : "room.events");
        turbo_flow_fmq_config_t pub = TURBO_FLOW_FMQ_CONFIG_INIT;
        pub.pattern = TURBO_FLOW_FMQ_PUB;
        pub.mode = TURBO_FLOW_FMQ_BIND;
        pub.transport = config->pub_transport ? config->pub_transport
                                              : TURBO_FLOW_FMQ_TCP;
        pub.host = config->pub_host ? config->pub_host : "127.0.0.1";
        pub.port = config->pub_port;
        pub.topic = b->pub_topic;
        pub.max_frame_size = TURBO_FLOW_FMQ_DEFAULT_MAX_FRAME_SIZE;
        pub.timeout_ms = config->timeout_ms ? config->timeout_ms : 5000;
        pub.tls = config->pub_tls;
        pub.path = config->pub_path ? config->pub_path : "/";
        turbo_flow_fmq_app_options_t pub_opt = TURBO_FLOW_FMQ_APP_OPTIONS_INIT;
        int pub_rc = config->pub_security
                         ? turbo_flow_fmq_app_create_secure(
                               &pub, &pub_opt, config->pub_security, &b->pub_app)
                         : turbo_flow_fmq_app_create(&pub, &pub_opt, &b->pub_app);
        if (pub_rc != TURBO_OK) {
            goto fail_resources;
        }
    }
    *out_bridge = b;
    return IVR_OK;

fail_resources:
    /* ivr_req_queue_destroy/ivr_dedup_destroy/ivr_mutex_destroy are safe on
       zero-initialized or partially initialized members (NULL handles). */
    if (b->app) {
        turbo_flow_fmq_app_destroy(b->app);
        b->app = NULL;
    }
    if (b->pub_app) {
        turbo_flow_fmq_app_destroy(b->pub_app);
        b->pub_app = NULL;
    }
    free(b->peers.ids);
    b->peers.ids = NULL;
    ivr_mutex_destroy(&b->peers.lock);
    ivr_mutex_destroy(&b->routes_lock);
    ivr_peer_event_queue_destroy(&b->peer_events);
    ivr_req_queue_destroy(&b->queue);
    ivr_dedup_destroy(&b->dedup);
    if (b->codec) {
        data_bind_free(b->codec);
        b->codec = NULL;
    }
    free(b->routes);
    free(b);
    return IVR_ENOSPC;
}

static void ivr_bridge_abort_start(ivr_room_bridge_t *bridge) {
    /* The worker thread is already running: stop it before failing so a
       caller that only destroys on start failure cannot leak the thread. */
    ivr_mutex_lock(&bridge->queue.lock);
    bridge->queue.stop = 1;
    ivr_cond_broadcast(&bridge->queue.cond);
    ivr_mutex_unlock(&bridge->queue.lock);
    ivr_thread_join(&bridge->thread);
}

ivr_status_t ivr_room_bridge_start(ivr_room_bridge_t *bridge) {
    if (!bridge) {
        return IVR_EINVAL;
    }
    if (atomic_exchange(&bridge->started, 1) != 0) {
        return IVR_ESTATE; /* already started: fail fast, never spawn twice */
    }
    /* a previous stop() left the queue latched; allow a clean restart */
    ivr_mutex_lock(&bridge->queue.lock);
    bridge->queue.stop = 0;
    ivr_mutex_unlock(&bridge->queue.lock);
    if (ivr_thread_create(&bridge->thread, ivr_bridge_thread_main, bridge) < 0) {
        atomic_store(&bridge->started, 0);
        return IVR_ENOSPC;
    }
    if (bridge->pub_app &&
        turbo_flow_fmq_app_start(bridge->pub_app) != TURBO_OK) {
        ivr_bridge_abort_start(bridge);
        atomic_store(&bridge->started, 0);
        return IVR_ESTATE;
    }
    if (turbo_flow_fmq_app_start(bridge->app) != TURBO_OK) {
        ivr_bridge_abort_start(bridge);
        atomic_store(&bridge->started, 0);
        return IVR_ESTATE;
    }
    return IVR_OK;
}

void ivr_room_bridge_stop(ivr_room_bridge_t *bridge) {
    if (!bridge) {
        return;
    }
    if (atomic_exchange(&bridge->started, 0) == 0) {
        return; /* not started: idempotent */
    }
    ivr_mutex_lock(&bridge->queue.lock);
    bridge->queue.stop = 1;
    ivr_cond_broadcast(&bridge->queue.cond);
    ivr_mutex_unlock(&bridge->queue.lock);
    ivr_thread_join(&bridge->thread);
    if (bridge->app) {
        (void)turbo_flow_fmq_app_stop(bridge->app);
    }
    if (bridge->pub_app) {
        (void)turbo_flow_fmq_app_stop(bridge->pub_app);
    }
}

void ivr_room_bridge_destroy(ivr_room_bridge_t *bridge) {
    if (!bridge) {
        return;
    }
    /* Stop and join the worker thread (and stop the FMQ apps) before freeing
       resources the worker thread and the FlowMQ callbacks still reference. */
    ivr_room_bridge_stop(bridge);
    if (bridge->app) {
        turbo_flow_fmq_app_destroy(bridge->app);
        bridge->app = NULL;
    }
    if (bridge->pub_app) {
        turbo_flow_fmq_app_destroy(bridge->pub_app);
        bridge->pub_app = NULL;
    }
    ivr_req_queue_destroy(&bridge->queue);
    ivr_peer_event_queue_destroy(&bridge->peer_events);
    ivr_dedup_destroy(&bridge->dedup);
    ivr_mutex_destroy(&bridge->routes_lock);
    ivr_mutex_destroy(&bridge->peers.lock);
    free(bridge->peers.ids);
    bridge->peers.ids = NULL;
    if (bridge->routes) {
        for (uint32_t i = 0; i < bridge->route_count; i++) {
            if (bridge->routes[i].valid) {
                turbo_flow_msg_cleanup(&bridge->routes[i].msg);
            }
        }
        free(bridge->routes);
        bridge->routes = NULL;
    }
    if (bridge->codec) {
        data_bind_free(bridge->codec);
        bridge->codec = NULL;
    }
    free(bridge);
}
