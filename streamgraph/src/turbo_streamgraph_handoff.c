#include "turbo_streamgraph_handoff.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum { HO_FREE=0, HO_RESERVED=1, HO_READY=2, HO_TAKEN=3 } ho_row_state;
typedef struct ho_row {
    uint32_t generation;
    ho_row_state state;
    uint64_t order;
    tm_ho_command_v1 command;
} ho_row;

struct tm_ho_inbox {
    _Atomic uint32_t lock;
    uint64_t incarnation;
    uint64_t next_order;
    uint32_t capacity;
    uint32_t sealed;
    ho_row *rows;
};
static _Atomic uint64_t ho_incarnation = 1u;

static void ho_lock(tm_ho_inbox *q) {
    while (atomic_exchange_explicit(&q->lock, 1u, memory_order_acquire) != 0u) {
        /* Bounded metadata-only critical sections. No callbacks while locked. */
    }
}
static void ho_unlock(tm_ho_inbox *q) {
    atomic_store_explicit(&q->lock, 0u, memory_order_release);
}
static ho_row *ho_find(tm_ho_inbox *q, tm_ho_ticket_v1 t) {
    ho_row *r;
    if (t.incarnation != q->incarnation || t.slot >= q->capacity ||
        t.generation == 0u) return NULL;
    r = &q->rows[t.slot];
    return r->state != HO_FREE && r->generation == t.generation ? r : NULL;
}
static int ho_valid_command(const tm_ho_command_v1 *c) {
    if (!c || c->size != sizeof(*c) || c->version != TM_HO_ABI_V1 ||
        c->reserved != 0u || c->kind < TM_HO_PUMP_SOURCE ||
        c->kind > TM_HO_FRAME_NOTICE || c->has_frame > 1u)
        return 0;
    if (c->kind == TM_HO_PUMP_SINK)
        return c->has_frame == 0u && c->branch < 8u;
    if (c->kind == TM_HO_FRAME_NOTICE)
        return c->branch == 0u && c->has_frame == 1u &&
               c->frame.size == sizeof(c->frame) &&
               c->frame.version == TM_FS_ABI_V1 &&
               c->frame.store_id != 0u && c->frame.store_generation != 0u &&
               c->frame.frame_id != 0u && c->frame.slot_generation != 0u &&
               c->frame.source_session_generation != 0u &&
               c->frame.display_generation != 0u;
    return c->branch == 0u && c->has_frame == 0u;
}

tm_ho_status tm_ho_create(const tm_ho_config_v1 *cfg, tm_ho_inbox **out) {
    tm_ho_inbox *q;
    uint64_t incarnation;
    if (!out || *out != NULL || !cfg) return TM_HO_EINVAL;
    if (cfg->version != TM_HO_ABI_V1) return TM_HO_EVERSION;
    if (cfg->size != sizeof(*cfg) || cfg->reserved || !cfg->capacity ||
        cfg->capacity > TM_HO_MAX_CAPACITY) return TM_HO_EINVAL;
    incarnation = atomic_fetch_add_explicit(&ho_incarnation, 1u,
                                             memory_order_relaxed);
    if (incarnation == 0u || incarnation == UINT64_MAX) return TM_HO_EEXHAUSTED;
    q = (tm_ho_inbox *)calloc(1u, sizeof(*q));
    if (!q) return TM_HO_ENOMEM;
    q->rows = (ho_row *)calloc(cfg->capacity, sizeof(*q->rows));
    if (!q->rows) { free(q); return TM_HO_ENOMEM; }
    atomic_init(&q->lock, 0u);
    q->capacity = cfg->capacity;
    q->incarnation = incarnation;
    q->next_order = 1u;
    *out = q;
    return TM_HO_OK;
}
tm_ho_status tm_ho_reserve(tm_ho_inbox *q, tm_ho_ticket_v1 *out) {
    uint32_t i;
    tm_ho_status result = TM_HO_EFULL;
    if (!q || !out) return TM_HO_EINVAL;
    memset(out, 0, sizeof(*out));
    ho_lock(q);
    if (q->sealed) result = TM_HO_ESHUTDOWN;
    else for (i=0u; i<q->capacity; ++i) {
        ho_row *r = &q->rows[i];
        if (r->state != HO_FREE || r->generation == UINT32_MAX) continue;
        r->generation++;
        r->state = HO_RESERVED;
        out->incarnation = q->incarnation;
        out->slot = i;
        out->generation = r->generation;
        result = TM_HO_OK;
        break;
    }
    ho_unlock(q);
    return result;
}
tm_ho_status tm_ho_publish(tm_ho_inbox *q, tm_ho_ticket_v1 ticket,
                           const tm_ho_command_v1 *command) {
    ho_row *r;
    tm_ho_status rc = TM_HO_OK;
    if (!q || !ho_valid_command(command)) return TM_HO_EINVAL;
    ho_lock(q);
    r = ho_find(q, ticket);
    if (!r) rc = TM_HO_ESTALE;
    else if (r->state != HO_RESERVED) rc = TM_HO_EBUSY;
    else if (q->sealed) rc = TM_HO_ESHUTDOWN;
    else if (q->next_order == UINT64_MAX) rc = TM_HO_EEXHAUSTED;
    else {
        r->command = *command;
        r->order = q->next_order++;
        r->state = HO_READY;
    }
    ho_unlock(q);
    return rc;
}
tm_ho_status tm_ho_abort(tm_ho_inbox *q, tm_ho_ticket_v1 ticket) {
    ho_row *r;
    tm_ho_status rc = TM_HO_OK;
    if (!q) return TM_HO_EINVAL;
    ho_lock(q);
    r = ho_find(q, ticket);
    if (!r) rc = TM_HO_ESTALE;
    else if (r->state != HO_RESERVED) rc = TM_HO_EBUSY;
    else {
        r->state = HO_FREE;
        r->order = 0u;
        memset(&r->command, 0, sizeof(r->command));
    }
    ho_unlock(q);
    return rc;
}
tm_ho_status tm_ho_take(tm_ho_inbox *q, tm_ho_ticket_v1 *out,
                        tm_ho_command_v1 *command) {
    uint32_t i, slot = UINT32_MAX;
    uint64_t order = UINT64_MAX;
    tm_ho_status rc = TM_HO_EBUSY;
    if (!q || !out || !command) return TM_HO_EINVAL;
    memset(out, 0, sizeof(*out));
    memset(command, 0, sizeof(*command));
    ho_lock(q);
    for (i=0u; i<q->capacity; ++i)
        if (q->rows[i].state == HO_READY && q->rows[i].order < order) {
            order = q->rows[i].order;
            slot = i;
        }
    if (slot != UINT32_MAX) {
        ho_row *r = &q->rows[slot];
        r->state = HO_TAKEN;
        out->incarnation = q->incarnation;
        out->slot = slot;
        out->generation = r->generation;
        *command = r->command;
        rc = TM_HO_OK;
    }
    ho_unlock(q);
    return rc;
}
tm_ho_status tm_ho_settle(tm_ho_inbox *q, tm_ho_ticket_v1 ticket) {
    ho_row *r;
    tm_ho_status rc = TM_HO_OK;
    if (!q) return TM_HO_EINVAL;
    ho_lock(q);
    r = ho_find(q, ticket);
    if (!r) rc = TM_HO_ESTALE;
    else if (r->state != HO_TAKEN) rc = TM_HO_EBUSY;
    else {
        r->state = HO_FREE;
        r->order = 0u;
        memset(&r->command, 0, sizeof(r->command));
    }
    ho_unlock(q);
    return rc;
}
tm_ho_status tm_ho_seal(tm_ho_inbox *q) {
    if (!q) return TM_HO_EINVAL;
    ho_lock(q);
    q->sealed = 1u;
    ho_unlock(q);
    return TM_HO_OK;
}
tm_ho_status tm_ho_snapshot(tm_ho_inbox *q, tm_ho_snapshot_v1 *out) {
    uint32_t i;
    if (!q || !out) return TM_HO_EINVAL;
    if (out->version != TM_HO_ABI_V1) return TM_HO_EVERSION;
    if (out->size != sizeof(*out)) return TM_HO_EINVAL;
    ho_lock(q);
    out->capacity = q->capacity;
    out->reserved = out->ready = out->taken = 0u;
    out->sealed = q->sealed;
    for (i=0u;i<q->capacity;++i) {
        if (q->rows[i].state == HO_RESERVED) out->reserved++;
        else if (q->rows[i].state == HO_READY) out->ready++;
        else if (q->rows[i].state == HO_TAKEN) out->taken++;
    }
    out->drained = out->reserved == 0u && out->ready == 0u && out->taken == 0u;
    ho_unlock(q);
    return TM_HO_OK;
}
tm_ho_status tm_ho_destroy(tm_ho_inbox *q) {
    uint32_t i;
    if (!q) return TM_HO_EINVAL;
    /* Exclusive lifecycle call only: producers joined and owner quiesced. */
    ho_lock(q);
    if (!q->sealed) { ho_unlock(q); return TM_HO_EBUSY; }
    for (i=0u;i<q->capacity;++i)
        if (q->rows[i].state != HO_FREE) {
            ho_unlock(q);
            return TM_HO_EBUSY;
        }
    ho_unlock(q);
    free(q->rows);
    free(q);
    return TM_HO_OK;
}
