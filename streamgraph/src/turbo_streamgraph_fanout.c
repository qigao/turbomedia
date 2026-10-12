#include "turbo_streamgraph_fanout.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum { FO_FREE=0, FO_READY=1, FO_TAKEN=2 } fo_state;
typedef struct {
    fo_state state;
    uint32_t generation;
    uint64_t order;
    tm_fs_frame_ref_v1 ref;
    tm_fs_pin_v1 pin;
} fo_row;
typedef struct {
    fo_row *rows;
    uint32_t depth;
    uint32_t policy;
    uint32_t sample_every;
    tm_fo_branch_stats_v1 stats;
} fo_branch;
struct tm_fo_owner {
    tm_fs_store *store;
    uint64_t incarnation;
    uint64_t next_order;
    fo_branch branches[TM_FO_MAX_BRANCHES];
    uint32_t branch_count;
    int sealed;
};
static _Atomic uint64_t fo_next_incarnation = 1u;

static tm_fo_status fo_fs_status(tm_fs_status rc) {
    switch (rc) {
    case TM_FS_OK: return TM_FO_OK;
    case TM_FS_EFULL: return TM_FO_EFULL;
    case TM_FS_EBUSY: return TM_FO_EBUSY;
    case TM_FS_ESTALE: return TM_FO_ESTALE;
    case TM_FS_ESHUTDOWN: return TM_FO_ESHUTDOWN;
    case TM_FS_EEXHAUSTED: return TM_FO_EEXHAUSTED;
    default: return TM_FO_EFRAME;
    }
}

static int fo_selected(const fo_branch *b, const tm_fs_frame_ref_v1 *ref) {
    return b->policy != TM_FO_SAMPLE ||
           ((ref->frame_id - 1u) % b->sample_every == 0u);
}

/* Find free row; LATEST may replace oldest READY but never TAKEN. */
static uint32_t fo_choose_row(const fo_branch *b) {
    uint32_t i, best = UINT32_MAX;
    uint64_t oldest = UINT64_MAX;
    for (i=0u; i<b->depth; ++i) {
        const fo_row *r = &b->rows[i];
        if (r->state == FO_FREE && r->generation != UINT32_MAX) return i;
    }
    if (b->policy != TM_FO_LATEST) return UINT32_MAX;
    for (i=0u; i<b->depth; ++i) {
        const fo_row *r = &b->rows[i];
        if (r->state == FO_READY && r->order < oldest) {
            best = i;
            oldest = r->order;
        }
    }
    return best;
}
static fo_row *fo_lookup(tm_fo_owner *o, tm_fo_ticket_v1 t) {
    fo_branch *b;
    fo_row *r;
    if (t.incarnation != o->incarnation || t.branch >= o->branch_count ||
        t.slot_generation == 0u) return NULL;
    b = &o->branches[t.branch];
    if (t.slot >= b->depth) return NULL;
    r = &b->rows[t.slot];
    return r->state != FO_FREE && r->generation == t.slot_generation ? r : NULL;
}
static tm_fo_status fo_release(tm_fo_owner *o, fo_branch *b, fo_row *r) {
    tm_fs_status rc = tm_fs_release_pin(o->store, &r->pin);
    if (rc != TM_FS_OK) return fo_fs_status(rc);
    if (r->state == FO_READY) b->stats.ready--;
    if (r->state == FO_TAKEN) b->stats.taken--;
    b->stats.completed++;
    r->state = FO_FREE;
    r->order = 0u;
    memset(&r->pin, 0, sizeof(r->pin));
    memset(&r->ref, 0, sizeof(r->ref));
    return TM_FO_OK;
}

tm_fo_status tm_fo_create(tm_fs_store *store, const tm_fo_config_v1 *cfg,
                           tm_fo_owner **out) {
    tm_fo_owner *o;
    uint32_t i;
    uint64_t incarnation;
    if (!store || !out || *out != NULL || !cfg) return TM_FO_EINVAL;
    if (cfg->version != TM_FO_ABI_V1) return TM_FO_EVERSION;
    if (cfg->size != sizeof(*cfg) || cfg->reserved ||
        cfg->branch_count == 0u || cfg->branch_count > TM_FO_MAX_BRANCHES)
        return TM_FO_EINVAL;
    for (i=0u; i<cfg->branch_count; ++i) {
        const tm_fo_branch_config_v1 *b = &cfg->branches[i];
        if (b->version != TM_FO_ABI_V1) return TM_FO_EVERSION;
        if (b->size != sizeof(*b) || !b->queue_depth ||
            b->queue_depth > TM_FO_MAX_QUEUE_DEPTH ||
            (b->policy != TM_FO_ORDERED_LOSSLESS &&
             b->policy != TM_FO_SAMPLE && b->policy != TM_FO_LATEST) ||
            (b->policy == TM_FO_SAMPLE ? !b->sample_every : b->sample_every != 0u))
            return TM_FO_EINVAL;
    }
    incarnation = atomic_fetch_add_explicit(&fo_next_incarnation, 1u,
                                              memory_order_relaxed);
    if (!incarnation || incarnation == UINT64_MAX) return TM_FO_EEXHAUSTED;
    o = (tm_fo_owner *)calloc(1u, sizeof(*o));
    if (!o) return TM_FO_ENOMEM;
    o->store = store;
    o->incarnation = incarnation;
    o->next_order = 1u;
    o->branch_count = cfg->branch_count;
    for (i=0u; i<o->branch_count; ++i) {
        fo_branch *b = &o->branches[i];
        b->depth = cfg->branches[i].queue_depth;
        b->policy = cfg->branches[i].policy;
        b->sample_every = cfg->branches[i].sample_every;
        b->rows = (fo_row *)calloc(b->depth, sizeof(fo_row));
        if (!b->rows) {
            uint32_t k;
            for (k=0u; k<i; ++k) free(o->branches[k].rows);
            free(o);
            return TM_FO_ENOMEM;
        }
        b->stats.size = sizeof(b->stats);
        b->stats.version = TM_FO_ABI_V1;
    }
    *out = o;
    return TM_FO_OK;
}

tm_fo_status tm_fo_submit(tm_fo_owner *o, const tm_fs_frame_ref_v1 *ref) {
    tm_fs_pin_v1 reservations[TM_FO_MAX_BRANCHES] = {{0}};
    uint32_t indexes[TM_FO_MAX_BRANCHES];
    uint32_t i, reserved = 0u;
    tm_fo_status result;
    if (!o || !ref) return TM_FO_EINVAL;
    if (!ref->frame_id || ref->size != sizeof(*ref) ||
        ref->version != TM_FS_ABI_V1) return TM_FO_EINVAL;
    if (o->sealed) return TM_FO_ESHUTDOWN;
    if (o->next_order == UINT64_MAX) return TM_FO_EEXHAUSTED;
    /* Preflight every selected branch. A full ordered recorder is terminal
     * for the submission BEFORE any latest-preview row can be modified. */
    for (i=0u; i<o->branch_count; ++i) {
        fo_branch *b = &o->branches[i];
        indexes[i] = UINT32_MAX;
        if (!fo_selected(b, ref)) continue;
        indexes[i] = fo_choose_row(b);
        if (indexes[i] == UINT32_MAX) {
            b->stats.rejected++;
            return TM_FO_EFULL;
        }
    }
    /* Reserve an independent pin for each admitted consumer. */
    for (i=0u; i<o->branch_count; ++i) {
        tm_fs_status rc;
        if (indexes[i] == UINT32_MAX) continue;
        rc = tm_fs_reserve_pin(o->store, ref, &reservations[i]);
        if (rc != TM_FS_OK) {
            result = fo_fs_status(rc);
            o->branches[i].stats.rejected++;
            goto rollback;
        }
        reserved++;
    }
    /* LATEST may evict only READY entries. An eviction failure retains its
     * original pin/row; new reservations are discarded. */
    for (i=0u; i<o->branch_count; ++i) {
        fo_branch *b = &o->branches[i];
        fo_row *row;
        if (indexes[i] == UINT32_MAX) continue;
        row = &b->rows[indexes[i]];
        if (row->state == FO_READY) {
            result = fo_release(o, b, row);
            if (result != TM_FO_OK) goto rollback;
            b->stats.replaced_ready++;
        }
    }
    for (i=0u; i<o->branch_count; ++i) {
        fo_branch *b = &o->branches[i];
        fo_row *row;
        if (indexes[i] == UINT32_MAX) {
            b->stats.sampled_out++;
            continue;
        }
        row = &b->rows[indexes[i]];
        row->generation++;
        row->ref = *ref;
        row->pin = reservations[i];
        row->order = o->next_order;
        row->state = FO_READY;
        b->stats.ready++;
        b->stats.admitted++;
    }
    o->next_order++;
    return TM_FO_OK;
rollback:
    /* Pin release for our newly reserved valid Store tickets is exact while
     * all APIs obey their single-owner serialization contract. */
    for (i=0u; i<o->branch_count && reserved; ++i) {
        if (reservations[i].ticket_id) {
            (void)tm_fs_release_pin(o->store, &reservations[i]);
            --reserved;
        }
    }
    return result;
}

tm_fo_status tm_fo_take(tm_fo_owner *o, uint32_t branch,
                         tm_fo_ticket_v1 *out, tm_fs_frame_ref_v1 *out_ref) {
    fo_branch *b;
    fo_row *oldest = NULL;
    uint32_t i, selected = 0u;
    if (!o || !out || !out_ref || branch >= o->branch_count)
        return TM_FO_EINVAL;
    memset(out, 0, sizeof(*out));
    memset(out_ref, 0, sizeof(*out_ref));
    b = &o->branches[branch];
    for (i=0u; i<b->depth; ++i) {
        fo_row *r = &b->rows[i];
        if (r->state == FO_READY && (!oldest || r->order < oldest->order)) {
            oldest = r;
            selected = i;
        }
    }
    if (!oldest) return TM_FO_EBUSY;
    oldest->state = FO_TAKEN;
    b->stats.ready--;
    b->stats.taken++;
    out->incarnation = o->incarnation;
    out->branch = branch;
    out->slot = selected;
    out->slot_generation = oldest->generation;
    *out_ref = oldest->ref;
    return TM_FO_OK;
}

tm_fo_status tm_fo_acquire_lease(tm_fo_owner *o, tm_fo_ticket_v1 ticket,
                                  tm_fs_lease_v1 *out_lease) {
    fo_row *r;
    if (!o || !out_lease) return TM_FO_EINVAL;
    memset(out_lease, 0, sizeof(*out_lease));
    r = fo_lookup(o, ticket);
    if (!r) return TM_FO_ESTALE;
    if (r->state != FO_TAKEN) return TM_FO_EBUSY;
    return fo_fs_status(tm_fs_acquire_pinned(o->store, &r->pin, out_lease));
}

tm_fo_status tm_fo_settle(tm_fo_owner *o, tm_fo_ticket_v1 ticket) {
    fo_row *r;
    if (!o) return TM_FO_EINVAL;
    r = fo_lookup(o, ticket);
    if (!r) return TM_FO_ESTALE;
    if (r->state != FO_TAKEN) return TM_FO_EBUSY;
    return fo_release(o, &o->branches[ticket.branch], r);
}
tm_fo_status tm_fo_discard_ready(tm_fo_owner *o, uint32_t branch) {
    fo_branch *b;
    uint32_t i;
    if (!o || branch >= o->branch_count) return TM_FO_EINVAL;
    b = &o->branches[branch];
    for (i=0u; i<b->depth; ++i) {
        if (b->rows[i].state == FO_READY) {
            tm_fo_status rc = fo_release(o, b, &b->rows[i]);
            if (rc != TM_FO_OK) return rc;
        }
    }
    return TM_FO_OK;
}
tm_fo_status tm_fo_stats(tm_fo_owner *o, uint32_t branch,
                          tm_fo_branch_stats_v1 *out) {
    if (!o || !out || branch >= o->branch_count) return TM_FO_EINVAL;
    if (out->version != TM_FO_ABI_V1) return TM_FO_EVERSION;
    if (out->size != sizeof(*out)) return TM_FO_EINVAL;
    *out = o->branches[branch].stats;
    return TM_FO_OK;
}
tm_fo_status tm_fo_seal(tm_fo_owner *o) {
    if (!o) return TM_FO_EINVAL;
    o->sealed = 1;
    return TM_FO_OK;
}
tm_fo_status tm_fo_destroy(tm_fo_owner *o) {
    uint32_t i;
    if (!o) return TM_FO_EINVAL;
    if (!o->sealed) return TM_FO_EBUSY;
    for (i=0u; i<o->branch_count; ++i)
        if (o->branches[i].stats.ready || o->branches[i].stats.taken)
            return TM_FO_EBUSY;
    for (i=0u; i<o->branch_count; ++i) free(o->branches[i].rows);
    free(o);
    return TM_FO_OK;
}
