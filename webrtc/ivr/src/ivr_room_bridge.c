#include "ivr_room_bridge.h"
#include "ivr_frame.h"
#include "ivr_thread.h"
#include "ivr_internal.h"
#include "turbomedia_ivr_v1.h"
#include "flowmq_protocol.h"
#include "flowmq_router_endpoint.h"
#include "salts_error.h"
#include "salts_str.h"
#include <json_parser.h>
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
    return salts_monotonic_ms();
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

typedef struct ivr_route_message_s {
    uint8_t *payload;
    size_t payload_size;
    flowmq_router_route_t route;
    char peer_identity[128];
} ivr_route_message_t;

static void ivr_route_message_init(ivr_route_message_t *message) {
    if (message) memset(message, 0, sizeof(*message));
}

static void ivr_route_message_cleanup(ivr_route_message_t *message) {
    if (!message) return;
    free(message->payload);
    memset(message, 0, sizeof(*message));
}

typedef struct {
    ivr_route_message_t *items;
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
    q->items = (ivr_route_message_t *)calloc(q->capacity, sizeof(*q->items));
    if (!q->items) {
        /* keep the struct destroy-safe (capacity 0 + NULL items) so the
           create() failure path can run ivr_req_queue_destroy() */
        q->capacity = 0;
        return -1;
    }
    for (uint32_t i = 0; i < q->capacity; i++) {
        ivr_route_message_init(&q->items[i]);
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
        ivr_route_message_cleanup(&q->items[i]);
    }
    free(q->items);
    q->items = NULL;
    ivr_mutex_destroy(&q->lock);
    ivr_cond_destroy(&q->cond);
}

/* Moves ownership of `msg` into the queue; returns 0 on success, -1 on full. */
static int ivr_req_queue_push(ivr_req_queue_t *q, ivr_route_message_t *msg) {
    ivr_mutex_lock(&q->lock);
    if (q->stop || q->count >= q->capacity) {
        q->dropped++;
        ivr_mutex_unlock(&q->lock);
        return -1;
    }
    uint32_t tail = (q->head + q->count) % q->capacity;
    q->items[tail] = *msg;
    ivr_route_message_init(msg);
    q->count++;
    if (q->count > q->high_water) {
        q->high_water = q->count;
    }
    ivr_cond_signal(&q->cond);
    ivr_mutex_unlock(&q->lock);
    return 0;
}

static void ivr_req_queue_close_and_discard(ivr_req_queue_t *q) {
    ivr_mutex_lock(&q->lock);
    q->stop = 1;
    while (q->count > 0u) {
        ivr_route_message_cleanup(&q->items[q->head]);
        q->head = (q->head + 1u) % q->capacity;
        q->count--;
    }
    q->head = 0u;
    ivr_cond_broadcast(&q->cond);
    ivr_mutex_unlock(&q->lock);
}

/* Pops one message; caller owns it. Returns 0, or -1 when stopped. */
/* Returns 0 with a message, 1 on the owner tick, or -1 when stopped. */
static int ivr_req_queue_pop(ivr_req_queue_t *q, ivr_route_message_t *out) {
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
    ivr_route_message_init(&q->items[q->head]);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    ivr_mutex_unlock(&q->lock);
    return 0;
}

typedef struct {
    flowmq_router_endpoint_event_kind_t kind;
    flowmq_router_route_t route;
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
                                     const flowmq_router_endpoint_event_t *event) {
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
    q->items[tail].route = event->route;
    memcpy(q->items[tail].peer_identity, event->peer_identity.data, length);
    q->items[tail].peer_identity[length] = '\0';
    q->count++;
    if (q->count > q->high_water) {
        q->high_water = q->count;
    }
    ivr_mutex_unlock(&q->lock);
    return 0;
}

static void ivr_peer_event_queue_clear(ivr_peer_event_queue_t *q) {
    ivr_mutex_lock(&q->lock);
    q->head = 0u;
    q->count = 0u;
    q->overflowed = 0;
    ivr_mutex_unlock(&q->lock);
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
    char identity[128];
    flowmq_router_route_t route;
} ivr_peer_entry_t;

typedef struct {
    ivr_peer_entry_t *entries;
    uint32_t capacity;
    uint32_t count;
    ivr_mutex_t lock;
} ivr_peer_set_t;

/* One captured ROUTER route for a registered worker (from worker.sync). Used
   to push CallDispatchCommandV1 to the worker's DEALER later. */
typedef struct {
    char worker_id[128];
    flowmq_router_route_t route;
    int valid;
} ivr_route_entry_t;

struct ivr_room_bridge_s {
    flowmq_router_endpoint_t *endpoint;
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
    atomic_int accepting;
    atomic_uint_fast64_t next_completion_id;
    uint64_t start_timeout_ns;
    int (*verify_peer_identity)(void *context,
                                const char *certificate_sha256,
                                const char *claimed_identity);
    void *verify_peer_identity_context;
    ivr_peer_set_t peers;
    ivr_route_entry_t *routes;
    uint32_t route_capacity;
    uint32_t route_count;
    atomic_uint_fast64_t applied;
    atomic_uint_fast64_t dedup_hits;
    atomic_uint_fast64_t version_rejects;
    atomic_uint_fast64_t drops;
    atomic_uint_fast64_t events_published;
    atomic_uint_fast64_t dispatches_sent;
    atomic_uint_fast64_t dispatches_failed;
    atomic_uint_fast64_t dispatch_results;
    atomic_uint_fast64_t dispatch_result_rejects;
    atomic_uint_fast64_t media_results;
    atomic_uint_fast64_t media_result_rejects;
    atomic_uint_fast64_t media_events;
    atomic_uint_fast64_t media_event_rejects;
    atomic_uint_fast64_t inventory_pages;
    atomic_uint_fast64_t inventory_page_rejects;
    atomic_uint_fast64_t auth_rejects; /* worker.sync identity not connected */
    atomic_uint_fast64_t dedup_expired_rejects;
};

#define IVR_BRIDGE_COUNTER_INC(bridge, field)                              \
    ((void)atomic_fetch_add_explicit(&(bridge)->field, 1u,                 \
                                     memory_order_relaxed))

static int ivr_room_router_send_payload(ivr_room_bridge_t *bridge,
                                        flowmq_router_route_t route,
                                        const uint8_t *payload,
                                        size_t payload_size) {
    flowmq_protocol_frame_t frame;
    uint64_t completion_id;
    tstr encoded = NULL;
    int rc;
    if (!bridge || !bridge->endpoint || (!payload && payload_size > 0u)) {
        return SALTS_EINVAL;
    }
    memset(&frame, 0, sizeof(frame));
    frame.kind = FLOWMQ_PROTOCOL_FRAME_DATA;
    frame.pattern = FLOWMQ_PROTOCOL_ROUTER;
    completion_id = atomic_fetch_add_explicit(
        &bridge->next_completion_id, 1u, memory_order_relaxed);
    if (completion_id == 0u) {
        completion_id = atomic_fetch_add_explicit(
            &bridge->next_completion_id, 1u, memory_order_relaxed);
    }
    frame.message_id = completion_id;
    frame.payload = vstr_from_buf((const char *)payload, payload_size);
    rc = flowmq_protocol_encode_frame(
        &frame, FLOWMQ_ROUTER_ENDPOINT_DEFAULT_MAX_FRAME_SIZE, &encoded);
    if (rc == SALTS_OK) {
        rc = flowmq_router_endpoint_send_copy(
            bridge->endpoint, route, completion_id, encoded,
            tstr_len(encoded));
    }
    tstr_free(encoded);
    return rc;
}

static int ivr_room_verify_peer_identity(void *context,
                                         const char *certificate_sha256,
                                         vstr claimed_identity) {
    ivr_room_bridge_t *bridge = (ivr_room_bridge_t *)context;
    char identity[128];
    if (!bridge || !bridge->verify_peer_identity ||
        !claimed_identity.data || claimed_identity.len == 0u ||
        claimed_identity.len >= sizeof(identity)) {
        return SALTS_EINVAL;
    }
    memcpy(identity, claimed_identity.data, claimed_identity.len);
    identity[claimed_identity.len] = '\0';
    return bridge->verify_peer_identity(bridge->verify_peer_identity_context,
                                        certificate_sha256, identity);
}

static ivr_status_t ivr_room_bridge_send_to_worker(
    ivr_room_bridge_t *bridge, const char *worker_id, const uint8_t *frame,
    size_t len);

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

static const char *field_text(const DataBindValue *root, const char *field) {
    const DataBindValue *value = data_bind_value_get(root, field);
    return value ? data_bind_value_as_string(value) : NULL;
}

static void field_str(const DataBindValue *root, const char *field, char *out,
                      size_t out_size) {
    const DataBindValue *v = data_bind_value_get(root, field);
    const char *s = v ? data_bind_value_as_string(v) : "";
    snprintf(out, out_size, "%s", s ? s : "");
}

static int field_str_checked(const DataBindValue *root, const char *field,
                             char *out, size_t out_size, int required) {
    const DataBindValue *value = data_bind_value_get(root, field);
    const char *text = value ? data_bind_value_as_string(value) : NULL;
    size_t size = text ? strlen(text) : 0u;
    if (!out || out_size == 0 || size >= out_size ||
        (required && size == 0)) {
        return 0;
    }
    if (size > 0) {
        memcpy(out, text, size);
    }
    out[size] = '\0';
    return 1;
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

ivr_status_t ivr_room_decode_media_result(
    DataBind *codec, const uint8_t *frame, size_t len,
    ivr_media_command_result_t *out) {
    ivr_frame_info_t info;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    const DataBindValue *root;
    const DataBindValue *status;

    if (!codec || !frame || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (ivr_frame_decode(frame, len, &info) != IVR_OK ||
        info.format != IVR_FMT_BIN || info.kind != IVR_KIND_RESULT ||
        info.schema_type_id != IVR_TYPE_MEDIA_COMMAND_RESULT_V1 ||
        data_bind_object_from_bin(codec, "MediaCommandResultV1",
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &object,
                                  &error) != DATA_BIND_OK) {
        return IVR_ESTATE;
    }
    root = data_bind_object_value(object);
    status = data_bind_value_get(root, "status_code");
    if (!field_str_checked(root, "message_id", out->message_id,
                           sizeof(out->message_id), 1) ||
        !field_str_checked(root, "tenant_id", out->tenant_id,
                           sizeof(out->tenant_id), 1) ||
        !field_str_checked(root, "provider_session_id",
                           out->provider_session_id,
                           sizeof(out->provider_session_id), 1) ||
        !field_str_checked(root, "dialog_id", out->dialog_id,
                           sizeof(out->dialog_id), 1) ||
        !field_str_checked(root, "worker_id", out->worker_id,
                           sizeof(out->worker_id), 1) ||
        !field_str_checked(root, "room_id", out->room_id,
                           sizeof(out->room_id), 1) ||
        !field_str_checked(root, "call_id", out->call_id,
                           sizeof(out->call_id), 1) ||
        !field_str_checked(root, "error_code", out->error_code,
                           sizeof(out->error_code), 0) ||
        !field_str_checked(root, "error_message", out->error_message,
                           sizeof(out->error_message), 0)) {
        data_bind_object_free(object);
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    out->call_generation = field_u64(root, "call_generation");
    out->operation_generation = field_u64(root, "operation_generation");
    out->status_code = status ? data_bind_value_as_int(status) : 1;
    data_bind_object_free(object);
    if (out->call_generation == 0 || out->operation_generation == 0 ||
        out->status_code > 0) {
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    return IVR_OK;
}

ivr_status_t ivr_room_decode_media_event(DataBind *codec,
                                         const uint8_t *frame, size_t len,
                                         ivr_media_event_t *out) {
    ivr_frame_info_t info;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    const DataBindValue *root;

    if (!codec || !frame || !out) {
        return IVR_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    if (ivr_frame_decode(frame, len, &info) != IVR_OK ||
        info.format != IVR_FMT_BIN || info.kind != IVR_KIND_EVENT ||
        info.schema_type_id != IVR_TYPE_MEDIA_EVENT_V1 ||
        data_bind_object_from_bin(codec, "MediaEventV1",
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &object,
                                  &error) != DATA_BIND_OK) {
        return IVR_ESTATE;
    }
    root = data_bind_object_value(object);
    if (!field_str_checked(root, "event_id", out->event_id,
                           sizeof(out->event_id), 1) ||
        !field_str_checked(root, "tenant_id", out->tenant_id,
                           sizeof(out->tenant_id), 1) ||
        !field_str_checked(root, "provider_session_id",
                           out->provider_session_id,
                           sizeof(out->provider_session_id), 1) ||
        !field_str_checked(root, "dialog_id", out->dialog_id,
                           sizeof(out->dialog_id), 1) ||
        !field_str_checked(root, "worker_id", out->worker_id,
                           sizeof(out->worker_id), 1) ||
        !field_str_checked(root, "room_id", out->room_id,
                           sizeof(out->room_id), 1) ||
        !field_str_checked(root, "call_id", out->call_id,
                           sizeof(out->call_id), 1) ||
        !field_str_checked(root, "event_type", out->event_type,
                           sizeof(out->event_type), 1) ||
        !field_str_checked(root, "input_id", out->input_id,
                           sizeof(out->input_id), 0) ||
        !field_str_checked(root, "input_value", out->input_value,
                           sizeof(out->input_value), 0) ||
        !field_str_checked(root, "payload_json", out->payload_json,
                           sizeof(out->payload_json), 0)) {
        data_bind_object_free(object);
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    out->call_generation = field_u64(root, "call_generation");
    out->sequence = field_u64(root, "sequence");
    out->occurred_at_ms = field_u64(root, "occurred_at_ms");
    data_bind_object_free(object);
    if (out->call_generation == 0 || out->sequence == 0 ||
        out->occurred_at_ms == 0) {
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    return IVR_OK;
}

static ivr_worker_resource_state_t inventory_state_from_name(
    const char *state) {
    if (!state) return 0;
    if (strcmp(state, "opening") == 0) return IVR_WORKER_RESOURCE_OPENING;
    if (strcmp(state, "active") == 0) return IVR_WORKER_RESOURCE_ACTIVE;
    if (strcmp(state, "closing") == 0) return IVR_WORKER_RESOURCE_CLOSING;
    return 0;
}

static int inventory_record_json_shape_valid(const json_value_t *item) {
    static const char *const keys[] = {
        "workerId",          "workerInstanceId", "workerEpoch",
        "tenantId",          "providerSessionId", "dialogId",
        "roomId",
        "callId",            "callGeneration",   "operationGeneration",
        "inputId",           "inputGeneration",  "inputActive",
        "state",
        "rebindable"};
    size_t count;
    if (!item || json_type(item) != JSON_OBJECT) return 0;
    count = json_object_size(item);
    if (count != sizeof(keys) / sizeof(keys[0])) return 0;
    for (size_t i = 0; i < count; ++i) {
        const char *key = json_object_key(item, i);
        int known = 0;
        for (size_t j = 0; j < sizeof(keys) / sizeof(keys[0]); ++j) {
            if (key && strcmp(key, keys[j]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) return 0;
    }
    return 1;
}

static int decode_inventory_record(DataBind *codec, const json_value_t *item,
                                   const char *worker_id,
                                   ivr_worker_inventory_record_t *out) {
    char *json = NULL;
    size_t json_size = 0;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    const DataBindValue *root;
    const char *state;
    int valid = 0;

    if (!codec || !worker_id || !out ||
        !inventory_record_json_shape_valid(item)) {
        return 0;
    }
    json = json_serialize(item, &json_size);
    if (!json ||
        data_bind_object_from_json(codec, "WorkerMediaInventoryRecordV2",
                                   json, json_size, &object,
                                   &error) != DATA_BIND_OK) {
        json_serialize_free(json);
        return 0;
    }
    json_serialize_free(json);
    memset(out, 0, sizeof(*out));
    root = data_bind_object_value(object);
    state = field_text(root, "state");
    if (field_str_checked(root, "workerId", out->worker_id,
                          sizeof(out->worker_id), 1) &&
        field_str_checked(root, "workerInstanceId",
                          out->worker_instance_id,
                          sizeof(out->worker_instance_id), 1) &&
        field_str_checked(root, "tenantId", out->tenant_id,
                          sizeof(out->tenant_id), 1) &&
        field_str_checked(root, "providerSessionId",
                          out->provider_session_id,
                          sizeof(out->provider_session_id), 1) &&
        field_str_checked(root, "dialogId", out->dialog_id,
                          sizeof(out->dialog_id), 1) &&
        field_str_checked(root, "roomId", out->room_id,
                          sizeof(out->room_id), 1) &&
        field_str_checked(root, "callId", out->call_id,
                          sizeof(out->call_id), 1)) {
        out->worker_epoch = field_u64(root, "workerEpoch");
        out->call_generation = field_u64(root, "callGeneration");
        out->operation_generation = field_u64(root, "operationGeneration");
        if (!field_str_checked(root, "inputId", out->input_id,
                               sizeof(out->input_id), 0)) {
            data_bind_object_free(object);
            memset(out, 0, sizeof(*out));
            return 0;
        }
        out->input_generation = field_u64(root, "inputGeneration");
        out->input_active = field_bool(root, "inputActive");
        out->state = inventory_state_from_name(state);
        out->rebindable = field_bool(root, "rebindable");
        valid = strcmp(out->worker_id, worker_id) == 0 &&
                out->worker_epoch != 0 && out->call_generation != 0 &&
                out->state != 0 &&
                (!out->input_active ||
                 (out->input_id[0] && out->input_generation != 0)) &&
                (out->input_active ||
                 (!out->input_id[0] && out->input_generation == 0)) &&
                (out->rebindable == 0 || out->rebindable == 1) &&
                (out->state == IVR_WORKER_RESOURCE_ACTIVE ||
                 !out->rebindable);
    }
    data_bind_object_free(object);
    if (!valid) memset(out, 0, sizeof(*out));
    return valid;
}

ivr_status_t ivr_room_decode_inventory_page(
    DataBind *codec, const uint8_t *frame, size_t len,
    ivr_worker_inventory_envelope_t *out) {
    ivr_frame_info_t info;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    const DataBindValue *root;
    const char *records_json;
    json_value_t *records = NULL;
    size_t records_count;
    const DataBindValue *status_value;

    if (!codec || !frame || !out) return IVR_EINVAL;
    memset(out, 0, sizeof(*out));
    if (ivr_frame_decode(frame, len, &info) != IVR_OK ||
        info.format != IVR_FMT_BIN || info.kind != IVR_KIND_RESULT ||
        info.schema_type_id != IVR_TYPE_WORKER_MEDIA_INVENTORY_PAGE_V1 ||
        data_bind_object_from_bin(codec, "WorkerMediaInventoryPageV1",
                                  frame + IVR_FRAME_HEADER_SIZE,
                                  len - IVR_FRAME_HEADER_SIZE, &object,
                                  &error) != DATA_BIND_OK) {
        return IVR_ESTATE;
    }
    root = data_bind_object_value(object);
    status_value = data_bind_value_get(root, "status_code");
    records_json = field_text(root, "records_json");
    if (!field_str_checked(root, "message_id", out->message_id,
                           sizeof(out->message_id), 1) ||
        !field_str_checked(root, "worker_id", out->worker_id,
                           sizeof(out->worker_id), 1) ||
        !field_str_checked(root, "error_code", out->error_code,
                           sizeof(out->error_code), 0) ||
        !field_str_checked(root, "error_message", out->error_message,
                           sizeof(out->error_message), 0) ||
        !records_json ||
        ((records = json_parse((const char *)((const uint8_t *)records_json), strlen(records_json))) ? 0 : -1) != 0 || !records ||
        json_type(records) != JSON_ARRAY) {
        data_bind_object_free(object);
        json_free(records);
        records = NULL;
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    out->page.inventory_version = field_u32(root, "inventory_version");
    out->page.revision = field_u64(root, "revision");
    out->page.cursor = field_u32(root, "cursor");
    out->page.next_cursor = field_u32(root, "next_cursor");
    out->page.total_active = field_u32(root, "total_active");
    out->page.count = field_u32(root, "count");
    out->page.has_more = field_bool(root, "has_more");
    out->status_code = status_value ? data_bind_value_as_int(status_value) : 1;
    records_count = json_array_size(records);
    if (out->status_code > 0 ||
        out->page.count > IVR_WORKER_INVENTORY_MAX_PAGE_SIZE ||
        records_count != out->page.count ||
        (out->status_code == IVR_OK &&
         (out->page.inventory_version != IVR_WORKER_INVENTORY_VERSION ||
          out->page.revision == 0 ||
          out->page.total_active < out->page.count ||
          (out->page.has_more && out->page.next_cursor == 0) ||
          (!out->page.has_more && out->page.next_cursor != 0))) ||
        (out->status_code != IVR_OK && out->page.count != 0)) {
        data_bind_object_free(object);
        json_free(records);
        records = NULL;
        memset(out, 0, sizeof(*out));
        return IVR_ESTATE;
    }
    for (size_t i = 0; i < records_count; ++i) {
        if (!decode_inventory_record(
                codec, json_array_get(records, i), out->worker_id,
                &out->page.records[i])) {
            data_bind_object_free(object);
            json_free(records);
            records = NULL;
            memset(out, 0, sizeof(*out));
            return IVR_ESTATE;
        }
    }
    data_bind_object_free(object);
    json_free(records);
    records = NULL;
    return IVR_OK;
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
    if (!bridge || !worker_id) {
        return IVR_EINVAL;
    }
    if (ivr_room_bridge_encode_participant_joined(
            bridge->codec, event_id, causation_id, worker_id, room_id, call_id,
            call_generation, room_version, sequence, 0, participant_id,
            participant_role, frame, sizeof(frame), &len) != IVR_OK) {
        return IVR_ESTATE;
    }
    if (ivr_room_bridge_send_to_worker(bridge, worker_id, frame, len) !=
        IVR_OK) {
        return IVR_ESTATE;
    }
    IVR_BRIDGE_COUNTER_INC(bridge, events_published);
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
    if (!bridge || !event_id || !causation_id ||
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
    if (ivr_room_bridge_send_to_worker(bridge, worker_id, frame, frame_len) !=
        IVR_OK) {
        return IVR_ESTATE;
    }
    IVR_BRIDGE_COUNTER_INC(bridge, events_published);
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

static int route_equal(flowmq_router_route_t left,
                       flowmq_router_route_t right) {
    return left.endpoint_id == right.endpoint_id &&
           left.generation == right.generation &&
           left.session_id == right.session_id;
}

static void peer_add(ivr_peer_set_t *p, const char *data, size_t len,
                     flowmq_router_route_t route) {
    if (!p || !data || len == 0) {
        return;
    }
    if (len > 127) {
        len = 127;
    }
    ivr_mutex_lock(&p->lock);
    for (uint32_t i = 0; i < p->count; i++) {
        if (strncmp(p->entries[i].identity, data, len) == 0 &&
            p->entries[i].identity[len] == '\0') {
            p->entries[i].route = route;
            ivr_mutex_unlock(&p->lock);
            return;
        }
    }
    if (p->count < p->capacity) {
        memcpy(p->entries[p->count].identity, data, len);
        p->entries[p->count].identity[len] = '\0';
        p->entries[p->count].route = route;
        p->count++;
    }
    ivr_mutex_unlock(&p->lock);
}

static void peer_remove(ivr_peer_set_t *p, const char *data, size_t len,
                        flowmq_router_route_t route) {
    if (!p || !data || len == 0) {
        return;
    }
    if (len > 127) {
        len = 127;
    }
    ivr_mutex_lock(&p->lock);
    for (uint32_t i = 0; i < p->count; i++) {
        if (strncmp(p->entries[i].identity, data, len) == 0 &&
            p->entries[i].identity[len] == '\0' &&
            route_equal(p->entries[i].route, route)) {
            for (uint32_t j = i + 1; j < p->count; j++) {
                p->entries[j - 1] = p->entries[j];
            }
            p->count--;
            memset(&p->entries[p->count], 0, sizeof(p->entries[p->count]));
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
        if (strcmp(p->entries[i].identity, id) == 0) {
            found = 1;
            break;
        }
    }
    ivr_mutex_unlock((ivr_mutex_t *)&p->lock);
    return found;
}

static int peer_route_matches(const ivr_peer_set_t *p, const char *id,
                              flowmq_router_route_t route) {
    int found = 0;
    if (!p || !id) return 0;
    ivr_mutex_lock((ivr_mutex_t *)&p->lock);
    for (uint32_t i = 0; i < p->count; ++i) {
        if (strcmp(p->entries[i].identity, id) == 0 &&
            route_equal(p->entries[i].route, route)) {
            found = 1;
            break;
        }
    }
    ivr_mutex_unlock((ivr_mutex_t *)&p->lock);
    return found;
}

static void route_invalidate_peer(ivr_room_bridge_t *bridge,
                                  const char *peer_identity, size_t length,
                                  flowmq_router_route_t route);

static void bridge_on_fmq_event(
    void *ctx, const flowmq_router_endpoint_event_t *event) {
    ivr_room_bridge_t *b = (ivr_room_bridge_t *)ctx;
    if (!b || !event ||
        !atomic_load_explicit(&b->accepting, memory_order_acquire)) {
        return;
    }
    if (event->kind == FLOWMQ_ROUTER_EVENT_PEER_CONNECTED ||
        event->kind == FLOWMQ_ROUTER_EVENT_PEER_DISCONNECTED) {
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
                                  const char *peer_identity, size_t length,
                                  flowmq_router_route_t route) {
    if (!bridge || !peer_identity || length == 0 || length >= 128) {
        return;
    }
    ivr_mutex_lock(&bridge->routes_lock);
    for (uint32_t i = 0; i < bridge->route_count; i++) {
        ivr_route_entry_t *entry = &bridge->routes[i];
        if (entry->valid &&
            strncmp(entry->worker_id, peer_identity, length) == 0 &&
            entry->worker_id[length] == '\0' &&
            route_equal(entry->route, route)) {
            entry->valid = 0;
            memset(&entry->route, 0, sizeof(entry->route));
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
            memset(&entry->route, 0, sizeof(entry->route));
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
        if (event.kind == FLOWMQ_ROUTER_EVENT_PEER_CONNECTED) {
            peer_add(&bridge->peers, event.peer_identity, identity_len,
                     event.route);
        } else if (event.kind == FLOWMQ_ROUTER_EVENT_PEER_DISCONNECTED) {
            peer_remove(&bridge->peers, event.peer_identity, identity_len,
                        event.route);
            route_invalidate_peer(bridge, event.peer_identity, identity_len,
                                  event.route);
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
    out->dedup_hits = atomic_load_explicit(&bridge->dedup_hits,
                                           memory_order_relaxed);
    out->dedup_expired_rejects = atomic_load_explicit(
        &bridge->dedup_expired_rejects, memory_order_relaxed);
    out->version_rejects = atomic_load_explicit(&bridge->version_rejects,
                                                memory_order_relaxed);
    out->auth_rejects = atomic_load_explicit(&bridge->auth_rejects,
                                             memory_order_relaxed);
    out->dispatch_result_rejects = atomic_load_explicit(
        &bridge->dispatch_result_rejects, memory_order_relaxed);
    out->media_results = atomic_load_explicit(&bridge->media_results,
                                              memory_order_relaxed);
    out->media_result_rejects = atomic_load_explicit(
        &bridge->media_result_rejects, memory_order_relaxed);
    out->media_events = atomic_load_explicit(&bridge->media_events,
                                             memory_order_relaxed);
    out->media_event_rejects = atomic_load_explicit(
        &bridge->media_event_rejects, memory_order_relaxed);
    out->inventory_pages = atomic_load_explicit(&bridge->inventory_pages,
                                                memory_order_relaxed);
    out->inventory_page_rejects = atomic_load_explicit(
        &bridge->inventory_page_rejects, memory_order_relaxed);
}

/* Capture/refresh the ROUTER route of a registered worker (called on a
   successful worker.sync so the route is generation-fenced and current). */
static void route_store(ivr_room_bridge_t *b, const char *worker_id,
                        ivr_route_message_t *msg) {
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
    entry->route = msg->route;
    entry->valid = 1;
    ivr_mutex_unlock(&b->routes_lock);
}

/* Bind an inbound result to the exact live ROUTER session captured by the
   worker's latest successful worker.sync. A connected peer name alone is not
   sufficient because another connected DEALER could spoof worker_id. */
static int route_matches_worker(ivr_room_bridge_t *b, const char *worker_id,
                                const ivr_route_message_t *msg) {
    int matches = 0;

    if (!b || !worker_id || !msg ||
        !peer_route_matches(&b->peers, worker_id, msg->route)) {
        return 0;
    }
    ivr_mutex_lock(&b->routes_lock);
    ivr_route_entry_t *entry = route_find_locked(b, worker_id);
    if (entry && entry->valid &&
        entry->route.endpoint_id == msg->route.endpoint_id &&
        entry->route.generation == msg->route.generation &&
        entry->route.session_id == msg->route.session_id) {
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
    flowmq_router_route_t route;
    ivr_mutex_lock(&bridge->routes_lock);
    ivr_route_entry_t *entry = route_find_locked(bridge, worker_id);
    if (!entry || !entry->valid) {
        ivr_mutex_unlock(&bridge->routes_lock);
        return IVR_ESTATE;
    }
    route = entry->route;
    ivr_mutex_unlock(&bridge->routes_lock);
    if (ivr_room_router_send_payload(bridge, route, frame, len) != SALTS_OK) {
        ivr_mutex_lock(&bridge->routes_lock);
        entry = route_find_locked(bridge, worker_id);
        if (entry && entry->valid &&
            entry->route.endpoint_id == route.endpoint_id &&
            entry->route.generation == route.generation &&
            entry->route.session_id == route.session_id) {
            entry->valid = 0;
            memset(&entry->route, 0, sizeof(entry->route));
        }
        ivr_mutex_unlock(&bridge->routes_lock);
        return IVR_ESTATE;
    }
    return IVR_OK;
}

ivr_status_t ivr_room_bridge_encode_media_command(
    DataBind *codec, const ivr_media_command_t *command, uint8_t *frame,
    size_t frame_cap, size_t *out_len) {
    const char *type_name;
    uint16_t type_id;
    char json[8192];
    char number[32];
    ivr_json_builder_t builder;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    uint8_t *binary = NULL;
    size_t binary_size = 0;
    ivr_frame_info_t info;

    if (!codec || !command || !frame || !out_len) {
        return IVR_EINVAL;
    }
    if (frame_cap < IVR_FRAME_HEADER_SIZE) {
        *out_len = 0;
        return IVR_ENOSPC;
    }
    if (
        command->message_id[0] == '\0' ||
        command->tenant_id[0] == '\0' ||
        command->provider_session_id[0] == '\0' ||
        command->dialog_id[0] == '\0' ||
        command->worker_id[0] == '\0' || command->room_id[0] == '\0' ||
        command->call_id[0] == '\0' || command->call_generation == 0 ||
        command->operation_generation == 0 ||
        command->deadline_timeout_ms == 0) {
        return IVR_EINVAL;
    }
    *out_len = 0;
    switch (command->kind) {
        case IVR_MEDIA_COMMAND_SESSION_OPEN:
            type_name = "MediaSessionOpenCommandV1";
            type_id = IVR_TYPE_MEDIA_SESSION_OPEN_COMMAND_V1;
            break;
        case IVR_MEDIA_COMMAND_PLAY:
            if (command->text[0] == '\0') return IVR_EINVAL;
            type_name = "MediaPlayCommandV1";
            type_id = IVR_TYPE_MEDIA_PLAY_COMMAND_V1;
            break;
        case IVR_MEDIA_COMMAND_INPUT_START:
            if (command->input_id[0] == '\0' ||
                command->input_generation == 0) return IVR_EINVAL;
            type_name = "MediaInputStartCommandV1";
            type_id = IVR_TYPE_MEDIA_INPUT_START_COMMAND_V1;
            break;
        case IVR_MEDIA_COMMAND_INPUT_STOP:
            if (command->input_id[0] == '\0' ||
                command->input_generation == 0) return IVR_EINVAL;
            type_name = "MediaInputStopCommandV1";
            type_id = IVR_TYPE_MEDIA_INPUT_STOP_COMMAND_V1;
            break;
        case IVR_MEDIA_COMMAND_CANCEL:
            if (command->input_id[0] == '\0' ||
                command->input_generation == 0) return IVR_EINVAL;
            type_name = "MediaCancelCommandV2";
            type_id = IVR_TYPE_MEDIA_CANCEL_COMMAND_V2;
            break;
        case IVR_MEDIA_COMMAND_SESSION_CLOSE:
            type_name = "MediaSessionCloseCommandV1";
            type_id = IVR_TYPE_MEDIA_SESSION_CLOSE_COMMAND_V1;
            break;
        default:
            return IVR_EINVAL;
    }

    ivr_json_builder_init(&builder, json, sizeof(json));
#define MEDIA_COMMAND_STRING(name, value)                                    \
    do {                                                                      \
        ivr_json_builder_raw(&builder, ",\"" name "\":");                 \
        ivr_json_builder_string_cstr(&builder, (value));                      \
    } while (0)
    ivr_json_builder_raw(&builder, "{\"message_id\":");
    ivr_json_builder_string_cstr(&builder, command->message_id);
    MEDIA_COMMAND_STRING("tenant_id", command->tenant_id);
    MEDIA_COMMAND_STRING("provider_session_id",
                         command->provider_session_id);
    MEDIA_COMMAND_STRING("dialog_id", command->dialog_id);
    MEDIA_COMMAND_STRING("worker_id", command->worker_id);
    MEDIA_COMMAND_STRING("room_id", command->room_id);
    MEDIA_COMMAND_STRING("call_id", command->call_id);
    ivr_json_builder_raw(&builder, ",\"call_generation\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)command->call_generation);
    ivr_json_builder_raw(&builder, number);
    ivr_json_builder_raw(&builder, ",\"operation_generation\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)command->operation_generation);
    ivr_json_builder_raw(&builder, number);
    ivr_json_builder_raw(&builder, ",\"deadline_timeout_ms\":");
    snprintf(number, sizeof(number), "%llu",
             (unsigned long long)command->deadline_timeout_ms);
    ivr_json_builder_raw(&builder, number);
    if (command->kind == IVR_MEDIA_COMMAND_PLAY) {
        MEDIA_COMMAND_STRING("text", command->text);
    } else if (command->kind == IVR_MEDIA_COMMAND_INPUT_START ||
               command->kind == IVR_MEDIA_COMMAND_INPUT_STOP ||
               command->kind == IVR_MEDIA_COMMAND_CANCEL) {
        MEDIA_COMMAND_STRING("input_id", command->input_id);
        ivr_json_builder_raw(&builder, ",\"input_generation\":");
        snprintf(number, sizeof(number), "%llu",
                 (unsigned long long)command->input_generation);
        ivr_json_builder_raw(&builder, number);
    } else if (command->kind == IVR_MEDIA_COMMAND_SESSION_CLOSE) {
        MEDIA_COMMAND_STRING("reason", command->reason);
    }
    ivr_json_builder_raw(&builder, "}");
#undef MEDIA_COMMAND_STRING
    if (!ivr_json_builder_ok(&builder) ||
        data_bind_object_from_json(codec, type_name, json, strlen(json),
                                   &object, &error) != DATA_BIND_OK ||
        data_bind_object_serialize_bin(codec, object, &binary, &binary_size,
                                       &error) != DATA_BIND_OK) {
        data_bind_object_free(object);
        return IVR_ESTATE;
    }
    data_bind_object_free(object);
    if (binary_size > frame_cap - IVR_FRAME_HEADER_SIZE) {
        data_bind_binary_free(binary);
        return IVR_ENOSPC;
    }
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = type_id;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(binary);
        return IVR_ESTATE;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, binary, binary_size);
    *out_len = IVR_FRAME_HEADER_SIZE + binary_size;
    data_bind_binary_free(binary);
    return IVR_OK;
}

ivr_status_t ivr_room_bridge_send_media_command(
    ivr_room_bridge_t *bridge, const ivr_media_command_t *command) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 8192u];
    size_t frame_size = 0;
    ivr_status_t status;
    if (!bridge || !command) {
        return IVR_EINVAL;
    }
    status = ivr_room_bridge_encode_media_command(
        bridge->codec, command, frame, sizeof(frame), &frame_size);
    if (status != IVR_OK) {
        return status;
    }
    return ivr_room_bridge_send_to_worker(bridge, command->worker_id, frame,
                                          frame_size);
}

ivr_status_t ivr_room_bridge_encode_inventory_query(
    DataBind *codec, const ivr_worker_inventory_request_t *request,
    uint8_t *frame, size_t frame_capacity, size_t *out_size) {
    char json[1024];
    char number[32];
    ivr_json_builder_t builder;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    uint8_t *binary = NULL;
    size_t binary_size = 0;
    ivr_frame_info_t info;

    if (!codec || !request || !frame || !out_size ||
        frame_capacity < IVR_FRAME_HEADER_SIZE || !request->message_id[0] ||
        !request->worker_id[0] ||
        request->query.inventory_version != IVR_WORKER_INVENTORY_VERSION ||
        request->query.limit == 0 ||
        request->query.limit > IVR_WORKER_INVENTORY_MAX_PAGE_SIZE) {
        return request && request->query.inventory_version !=
                              IVR_WORKER_INVENTORY_VERSION
                   ? IVR_EVERSION
                   : IVR_EINVAL;
    }
    *out_size = 0;
    ivr_json_builder_init(&builder, json, sizeof(json));
    ivr_json_builder_raw(&builder, "{\"message_id\":");
    ivr_json_builder_string_cstr(&builder, request->message_id);
    ivr_json_builder_raw(&builder, ",\"worker_id\":");
    ivr_json_builder_string_cstr(&builder, request->worker_id);
#define INVENTORY_QUERY_NUMBER(name, value)                                  \
    do {                                                                      \
        ivr_json_builder_raw(&builder, ",\"" name "\":");             \
        snprintf(number, sizeof(number), "%llu",                            \
                 (unsigned long long)(value));                                \
        ivr_json_builder_raw(&builder, number);                               \
    } while (0)
    INVENTORY_QUERY_NUMBER("inventory_version",
                           request->query.inventory_version);
    INVENTORY_QUERY_NUMBER("expected_revision",
                           request->query.expected_revision);
    INVENTORY_QUERY_NUMBER("cursor", request->query.cursor);
    INVENTORY_QUERY_NUMBER("limit", request->query.limit);
#undef INVENTORY_QUERY_NUMBER
    ivr_json_builder_raw(&builder, "}");
    if (!ivr_json_builder_ok(&builder) ||
        data_bind_object_from_json(codec, "WorkerMediaInventoryQueryV1", json,
                                   strlen(json), &object,
                                   &error) != DATA_BIND_OK ||
        data_bind_object_serialize_bin(codec, object, &binary, &binary_size,
                                       &error) != DATA_BIND_OK) {
        data_bind_object_free(object);
        return IVR_ESTATE;
    }
    data_bind_object_free(object);
    if (binary_size > frame_capacity - IVR_FRAME_HEADER_SIZE) {
        data_bind_binary_free(binary);
        return IVR_ENOSPC;
    }
    memset(&info, 0, sizeof(info));
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_WORKER_MEDIA_INVENTORY_QUERY_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    if (ivr_frame_encode(frame, &info) != IVR_OK) {
        data_bind_binary_free(binary);
        return IVR_ESTATE;
    }
    memcpy(frame + IVR_FRAME_HEADER_SIZE, binary, binary_size);
    *out_size = IVR_FRAME_HEADER_SIZE + binary_size;
    data_bind_binary_free(binary);
    return IVR_OK;
}

ivr_status_t ivr_room_bridge_request_inventory(
    ivr_room_bridge_t *bridge,
    const ivr_worker_inventory_request_t *request) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 1024u];
    size_t frame_size = 0;
    ivr_status_t status;
    if (!bridge || !request) return IVR_EINVAL;
    status = ivr_room_bridge_encode_inventory_query(
        bridge->codec, request, frame, sizeof(frame), &frame_size);
    if (status != IVR_OK) return status;
    return ivr_room_bridge_send_to_worker(bridge, request->worker_id, frame,
                                          frame_size);
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
        IVR_BRIDGE_COUNTER_INC(bridge, dispatches_failed);
        return IVR_ESTATE;
    }
    if (ivr_room_bridge_send_to_worker(bridge, worker_id, frame, len) !=
        IVR_OK) {
        IVR_BRIDGE_COUNTER_INC(bridge, dispatches_failed);
        return IVR_ESTATE;
    }
    IVR_BRIDGE_COUNTER_INC(bridge, dispatches_sent);
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
        IVR_BRIDGE_COUNTER_INC(bridge, dispatches_failed);
        return IVR_ESTATE;
    }
    IVR_BRIDGE_COUNTER_INC(bridge, dispatches_sent);
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
        IVR_BRIDGE_COUNTER_INC(bridge, dispatches_failed);
        return IVR_ESTATE;
    }
    if (ivr_room_bridge_send_to_worker(bridge, worker_id, frame, len) !=
        IVR_OK) {
        IVR_BRIDGE_COUNTER_INC(bridge, dispatches_failed);
        return IVR_ESTATE;
    }
    IVR_BRIDGE_COUNTER_INC(bridge, dispatches_sent);
    return IVR_OK;
}

/* ------------------------------------------------------------------ */
/* command handling (bridge worker thread)                             */
/* ------------------------------------------------------------------ */

static void ivr_bridge_reply(ivr_room_bridge_t *b, ivr_route_message_t *msg,
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
    (void)ivr_room_router_send_payload(b, msg->route, frame, len);
}

static void ivr_bridge_handle_request(ivr_room_bridge_t *b,
                                      ivr_route_message_t *msg) {
    ivr_frame_info_t info;
    if (ivr_frame_decode((const uint8_t *)msg->payload, msg->payload_size,
                         &info) != IVR_OK) {
        return;
    }
    if (info.kind == IVR_KIND_RESULT &&
        (info.schema_type_id == IVR_TYPE_CALL_DISPATCH_RESULT_V1 ||
         info.schema_type_id == IVR_TYPE_CALL_DISPATCH_RESULT_V2)) {
        ivr_dispatch_result_t dispatch_result;
        if (ivr_room_decode_dispatch_result(
                b->codec, (const uint8_t *)msg->payload, msg->payload_size,
                &dispatch_result) != IVR_OK ||
            !route_matches_worker(b, dispatch_result.worker_id, msg)) {
            IVR_BRIDGE_COUNTER_INC(b, dispatch_result_rejects);
            return;
        }
        if (!b->handler.on_dispatch_result ||
            b->handler.on_dispatch_result(b->handler.context,
                                          &dispatch_result) != IVR_OK) {
            IVR_BRIDGE_COUNTER_INC(b, dispatch_result_rejects);
            return;
        }
        IVR_BRIDGE_COUNTER_INC(b, dispatch_results);
        return;
    }
    if (info.kind == IVR_KIND_RESULT &&
        info.schema_type_id == IVR_TYPE_CALL_RELEASE_RESULT_V1) {
        ivr_release_result_t release_result;
        if (ivr_room_decode_release_result(
                b->codec, (const uint8_t *)msg->payload, msg->payload_size,
                &release_result) != IVR_OK ||
            !route_matches_worker(b, release_result.worker_id, msg)) {
            IVR_BRIDGE_COUNTER_INC(b, dispatch_result_rejects);
            return;
        }
        if (!b->handler.on_release_result ||
            b->handler.on_release_result(b->handler.context,
                                         &release_result) != IVR_OK) {
            IVR_BRIDGE_COUNTER_INC(b, dispatch_result_rejects);
            return;
        }
        IVR_BRIDGE_COUNTER_INC(b, dispatch_results);
        return;
    }
    if (info.kind == IVR_KIND_RESULT &&
        info.schema_type_id == IVR_TYPE_MEDIA_COMMAND_RESULT_V1) {
        ivr_media_command_result_t media_result;
        if (ivr_room_decode_media_result(
                b->codec, (const uint8_t *)msg->payload, msg->payload_size,
                &media_result) != IVR_OK ||
            !route_matches_worker(b, media_result.worker_id, msg) ||
            !b->handler.on_media_result ||
            b->handler.on_media_result(b->handler.context, &media_result) !=
                IVR_OK) {
            IVR_BRIDGE_COUNTER_INC(b, media_result_rejects);
            return;
        }
        IVR_BRIDGE_COUNTER_INC(b, media_results);
        return;
    }
    if (info.kind == IVR_KIND_RESULT &&
        info.schema_type_id == IVR_TYPE_WORKER_MEDIA_INVENTORY_PAGE_V1) {
        ivr_worker_inventory_envelope_t inventory;
        if (ivr_room_decode_inventory_page(
                b->codec, (const uint8_t *)msg->payload,
                msg->payload_size, &inventory) != IVR_OK ||
            !route_matches_worker(b, inventory.worker_id, msg) ||
            !b->handler.on_inventory_page ||
            b->handler.on_inventory_page(b->handler.context, &inventory) !=
                IVR_OK) {
            IVR_BRIDGE_COUNTER_INC(b, inventory_page_rejects);
            return;
        }
        IVR_BRIDGE_COUNTER_INC(b, inventory_pages);
        return;
    }
    if (info.kind == IVR_KIND_EVENT &&
        info.schema_type_id == IVR_TYPE_MEDIA_EVENT_V1) {
        ivr_media_event_t media_event;
        if (ivr_room_decode_media_event(
                b->codec, (const uint8_t *)msg->payload, msg->payload_size,
                &media_event) != IVR_OK ||
            !route_matches_worker(b, media_event.worker_id, msg) ||
            !b->handler.on_media_event ||
            b->handler.on_media_event(b->handler.context, &media_event) !=
                IVR_OK) {
            IVR_BRIDGE_COUNTER_INC(b, media_event_rejects);
            return;
        }
        IVR_BRIDGE_COUNTER_INC(b, media_events);
        return;
    }
    if (info.kind != IVR_KIND_COMMAND) {
        return;
    }
    ivr_room_command_t cmd;
    if (ivr_room_decode_frame(b->codec, (const uint8_t *)msg->payload,
                              msg->payload_size, &cmd) != IVR_OK) {
        return; /* malformed frame: no reply (fail fast, drop + count below) */
    }
    ivr_room_command_result_t result;
    memset(&result, 0, sizeof(result));
    int dedup_rc = ivr_dedup_lookup(&b->dedup, cmd.message_id,
                                    cmd.call_generation, &result);
    if (dedup_rc > 0) {
        IVR_BRIDGE_COUNTER_INC(b, dedup_hits);
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
        IVR_BRIDGE_COUNTER_INC(b, dedup_expired_rejects);
        ivr_bridge_reply(b, msg, &cmd, &result);
        return;
    }
    if (strcmp(cmd.command, "worker.sync") == 0 ||
        strcmp(cmd.command, "worker.sync.v2") == 0 ||
        strcmp(cmd.command, "worker.heartbeat") == 0) {
        /* Independent FlowMQ supplies the authenticated identity for this
           exact ROUTER message. Bind registration to that peer instead of
           accepting a claim merely because some peer with the same ID is
           connected. */
        if (msg->peer_identity[0] == '\0' ||
            strcmp(msg->peer_identity, cmd.worker_id) != 0 ||
            !peer_route_matches(&b->peers, cmd.worker_id, msg->route)) {
            result.status_code = IVR_EAUTH;
            snprintf(result.error_message, sizeof(result.error_message),
                     "worker identity not connected");
            IVR_BRIDGE_COUNTER_INC(b, auth_rejects);
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
            IVR_BRIDGE_COUNTER_INC(b, version_rejects);
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
        IVR_BRIDGE_COUNTER_INC(b, applied);
        if (strcmp(cmd.command, "worker.sync") == 0 ||
            strcmp(cmd.command, "worker.sync.v2") == 0 ||
            strcmp(cmd.command, "worker.heartbeat") == 0) {
            /* capture the worker's current ROUTER route for later dispatch
               pushes; re-registration refreshes it after a reconnect */
            route_store(b, cmd.worker_id, msg);
        }
        /* A join event is gated by CallDispatchResultV1 accepted so the
           worker session exists before its first sequenced event arrives. */
        if (strcmp(cmd.command, "get_snapshot") == 0) {
            /* authoritative snapshot domain event: confirms continuity for the
               worker after a sequence gap (worker.sync/get_snapshot) */
            uint8_t ev[IVR_FRAME_HEADER_SIZE + 64u * 1024u];
            size_t ev_len = 0;
            if (ivr_room_bridge_encode_snapshot(
                    b->codec, cmd.room_id, cmd.call_id, cmd.call_generation,
                    result.room_version, result.sequence, "", "", ev,
                    sizeof(ev), &ev_len) == IVR_OK) {
                if (ivr_room_router_send_payload(b, msg->route, ev, ev_len) ==
                    SALTS_OK) {
                    IVR_BRIDGE_COUNTER_INC(b, events_published);
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
    ivr_route_message_t msg;
    ivr_route_message_init(&msg);
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
        ivr_route_message_cleanup(&msg);
        ivr_route_message_init(&msg);
    }
    ivr_route_message_cleanup(&msg);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* FlowMQ ROUTER ingress callback                                      */
/* ------------------------------------------------------------------ */

static int bridge_on_router_request(
    void *ctx, const flowmq_router_route_t *route, vstr peer_identity,
    vstr peer_topic, const flowmq_protocol_frame_t *message) {
    ivr_room_bridge_t *b = (ivr_room_bridge_t *)ctx;
    ivr_route_message_t clone;
    (void)peer_topic;
    if (!b || !route || !message ||
        !atomic_load_explicit(&b->accepting, memory_order_acquire) ||
        message->kind != FLOWMQ_PROTOCOL_FRAME_DATA) {
        return SALTS_EINVAL;
    }
    ivr_route_message_init(&clone);
    if ((!message->payload.data && message->payload.len > 0u) ||
        !peer_identity.data || peer_identity.len == 0u ||
        peer_identity.len >= sizeof(clone.peer_identity)) {
        return SALTS_EINVAL;
    }
    if (message->payload.len > 0u) {
        clone.payload = (uint8_t *)malloc(message->payload.len);
    }
    if (message->payload.len > 0u && !clone.payload) {
        ivr_route_message_cleanup(&clone);
        return SALTS_ENOMEM;
    }
    if (message->payload.len > 0u) {
        memcpy(clone.payload, message->payload.data, message->payload.len);
    }
    clone.payload_size = message->payload.len;
    clone.route = *route;
    memcpy(clone.peer_identity, peer_identity.data, peer_identity.len);
    clone.peer_identity[peer_identity.len] = '\0';
    if (!atomic_load_explicit(&b->accepting, memory_order_acquire) ||
        ivr_req_queue_push(&b->queue, &clone) != 0) {
        ivr_route_message_cleanup(&clone);
        IVR_BRIDGE_COUNTER_INC(b, drops);
    }
    return SALTS_OK;
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
    atomic_init(&b->accepting, 0);
    atomic_init(&b->next_completion_id, 1u);
    atomic_init(&b->applied, 0u);
    atomic_init(&b->dedup_hits, 0u);
    atomic_init(&b->version_rejects, 0u);
    atomic_init(&b->drops, 0u);
    atomic_init(&b->events_published, 0u);
    atomic_init(&b->dispatches_sent, 0u);
    atomic_init(&b->dispatches_failed, 0u);
    atomic_init(&b->dispatch_results, 0u);
    atomic_init(&b->dispatch_result_rejects, 0u);
    atomic_init(&b->media_results, 0u);
    atomic_init(&b->media_result_rejects, 0u);
    atomic_init(&b->media_events, 0u);
    atomic_init(&b->media_event_rejects, 0u);
    atomic_init(&b->inventory_pages, 0u);
    atomic_init(&b->inventory_page_rejects, 0u);
    atomic_init(&b->auth_rejects, 0u);
    atomic_init(&b->dedup_expired_rejects, 0u);

    b->peers.capacity = 64;
    b->peers.entries = (ivr_peer_entry_t *)calloc(
        b->peers.capacity, sizeof(*b->peers.entries));
    if (!b->peers.entries) {
        goto fail_resources;
    }
    flowmq_router_endpoint_config_t ep;
    uint64_t timeout_ms = config->timeout_ms ? config->timeout_ms : 5000u;
    if (timeout_ms > UINT64_MAX / UINT64_C(1000000)) {
        goto fail_resources;
    }
    b->start_timeout_ns = timeout_ms * UINT64_C(1000000);
    b->verify_peer_identity = config->verify_peer_identity;
    b->verify_peer_identity_context = config->verify_peer_identity_context;
    flowmq_router_endpoint_config_init(&ep);
    ep.transport = config->transport
                       ? (flowmq_coronet_transport_t)config->transport
                       : FLOWMQ_TRANSPORT_TCP;
    ep.host = config->host;
    ep.port = config->port;
    ep.identity = "room-service";
    ep.topic = "ivr.internal";
    ep.max_frame_size = FLOWMQ_ROUTER_ENDPOINT_DEFAULT_MAX_FRAME_SIZE;
    ep.timeouts.timeout_ms = timeout_ms;
    ep.timeouts.set_flags = FLOWMQ_TIMEOUT_SET_DEFAULT;
    ep.tls = config->tls;
    ep.path = config->path ? config->path : "";
    ep.context = NULL;
    ep.drive_context = 1;
    ep.own_context = 1;
    ep.on_frame = bridge_on_router_request;
    ep.on_event = bridge_on_fmq_event;
    ep.callback_ctx = b;
    if (config->verify_peer_identity) {
        ep.verify_peer_identity = ivr_room_verify_peer_identity;
        ep.verify_peer_identity_ctx = b;
    }
    int app_rc = flowmq_router_endpoint_create(&ep, &b->endpoint);
    if (app_rc != SALTS_OK) {
        goto fail_resources;
    }
    *out_bridge = b;
    return IVR_OK;

fail_resources:
    /* ivr_req_queue_destroy/ivr_dedup_destroy/ivr_mutex_destroy are safe on
       zero-initialized or partially initialized members (NULL handles). */
    if (b->endpoint) {
        flowmq_router_endpoint_destroy(b->endpoint);
        b->endpoint = NULL;
    }
    free(b->peers.entries);
    b->peers.entries = NULL;
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
    atomic_store_explicit(&bridge->accepting, 0, memory_order_release);
    ivr_req_queue_close_and_discard(&bridge->queue);
    flowmq_router_endpoint_stop(bridge->endpoint);
    ivr_thread_join(&bridge->thread);
    ivr_peer_event_queue_clear(&bridge->peer_events);
    peer_and_routes_clear(bridge);
}

ivr_status_t ivr_room_bridge_start(ivr_room_bridge_t *bridge) {
    if (!bridge) {
        return IVR_EINVAL;
    }
    if (atomic_exchange(&bridge->started, 1) != 0) {
        return IVR_ESTATE; /* already started: fail fast, never spawn twice */
    }
    /* A new lifecycle generation starts with no requests, peer events or
       routes retained from the previous endpoint generation. */
    ivr_peer_event_queue_clear(&bridge->peer_events);
    peer_and_routes_clear(bridge);
    ivr_mutex_lock(&bridge->queue.lock);
    bridge->queue.head = 0u;
    bridge->queue.count = 0u;
    bridge->queue.stop = 0;
    ivr_mutex_unlock(&bridge->queue.lock);
    if (ivr_thread_create(&bridge->thread, ivr_bridge_thread_main, bridge) < 0) {
        atomic_store(&bridge->started, 0);
        return IVR_ENOSPC;
    }
    atomic_store_explicit(&bridge->accepting, 1, memory_order_release);
    if (flowmq_router_endpoint_start(bridge->endpoint,
                                     bridge->start_timeout_ns) != SALTS_OK) {
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
    /* Close ingress first. Queue close rejects any callback already in flight;
       endpoint stop then establishes callback quiescence before owner join. */
    atomic_store_explicit(&bridge->accepting, 0, memory_order_release);
    ivr_req_queue_close_and_discard(&bridge->queue);
    flowmq_router_endpoint_stop(bridge->endpoint);
    ivr_thread_join(&bridge->thread);
    ivr_peer_event_queue_clear(&bridge->peer_events);
    peer_and_routes_clear(bridge);
}

void ivr_room_bridge_destroy(ivr_room_bridge_t *bridge) {
    if (!bridge) {
        return;
    }
    /* Stop and join the worker thread (and stop the FMQ apps) before freeing
       resources the worker thread and the FlowMQ callbacks still reference. */
    ivr_room_bridge_stop(bridge);
    if (bridge->endpoint) {
        flowmq_router_endpoint_destroy(bridge->endpoint);
        bridge->endpoint = NULL;
    }
    ivr_req_queue_destroy(&bridge->queue);
    ivr_peer_event_queue_destroy(&bridge->peer_events);
    ivr_dedup_destroy(&bridge->dedup);
    ivr_mutex_destroy(&bridge->routes_lock);
    ivr_mutex_destroy(&bridge->peers.lock);
    free(bridge->peers.entries);
    bridge->peers.entries = NULL;
    if (bridge->routes) {
        for (uint32_t i = 0; i < bridge->route_count; i++) {
            memset(&bridge->routes[i].route, 0,
                   sizeof(bridge->routes[i].route));
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
