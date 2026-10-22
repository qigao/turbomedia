#include "turbo_streamgraph_provider_completion.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PC_FREE=0, PC_INFLIGHT, PC_CANCEL_PENDING, PC_READY, PC_TAKEN
} pc_state;
typedef struct {
    pc_state state;
    uint32_t generation;
    uint64_t order,correlation;
    tm_pc_result_v1 result;
} pc_row;
struct tm_pc_portal {
    _Atomic uint32_t lock;
    uint64_t incarnation,provider_generation,next_order;
    uint32_t capacity,sealed;
    pc_row *rows;
};
static _Atomic uint64_t pc_incarnations=1u;

static void pc_lock(tm_pc_portal *p) {
    while (atomic_exchange_explicit(&p->lock,1u,memory_order_acquire)) {
        /* The critical section only changes bounded metadata, never calls
         * arbitrary Provider or executor callbacks. */
    }
}
static void pc_unlock(tm_pc_portal *p) {
    atomic_store_explicit(&p->lock,0u,memory_order_release);
}
static pc_row *pc_find(tm_pc_portal *p,tm_pc_ticket_v1 t) {
    pc_row *r;
    if (t.incarnation!=p->incarnation || t.slot>=p->capacity || !t.generation)
        return NULL;
    r=&p->rows[t.slot];
    return r->state!=PC_FREE && r->generation==t.generation?r:NULL;
}
static void pc_ready(pc_row *r,uint32_t outcome,uint32_t completed,
                     int32_t status) {
    r->result.outcome=outcome;
    r->result.completion_observed=completed;
    r->result.provider_status=status;
    r->state=PC_READY;
}
tm_pc_status tm_pc_create(const tm_pc_config_v1 *c,tm_pc_portal **out) {
    tm_pc_portal *p;
    uint64_t incarnation;
    if (!out || *out || !c) return TM_PC_EINVAL;
    if (c->version!=TM_PC_ABI_V1) return TM_PC_EVERSION;
    if (c->size!=sizeof(*c) || c->reserved || !c->capacity ||
        c->capacity>TM_PC_MAX_CAPACITY || !c->provider_generation)
        return TM_PC_EINVAL;
    incarnation=atomic_fetch_add_explicit(&pc_incarnations,1u,memory_order_relaxed);
    if (!incarnation || incarnation==UINT64_MAX) return TM_PC_EEXHAUSTED;
    p=(tm_pc_portal *)calloc(1u,sizeof(*p));
    if (!p) return TM_PC_ENOMEM;
    p->rows=(pc_row *)calloc(c->capacity,sizeof(*p->rows));
    if (!p->rows) { free(p); return TM_PC_ENOMEM; }
    atomic_init(&p->lock,0u);
    p->incarnation=incarnation;
    p->provider_generation=c->provider_generation;
    p->capacity=c->capacity;
    p->next_order=1u;
    *out=p;
    return TM_PC_OK;
}
tm_pc_status tm_pc_begin(tm_pc_portal *p,uint64_t correlation,
                         tm_pc_ticket_v1 *out) {
    uint32_t i;
    tm_pc_status rc=TM_PC_EFULL;
    if (!p || !out || !correlation) return TM_PC_EINVAL;
    memset(out,0,sizeof(*out));
    pc_lock(p);
    if (p->sealed) rc=TM_PC_ESHUTDOWN;
    else if (p->next_order==UINT64_MAX) rc=TM_PC_EEXHAUSTED;
    else for (i=0u;i<p->capacity;++i) {
        pc_row *r=&p->rows[i];
        if (r->state!=PC_FREE || r->generation==UINT32_MAX) continue;
        r->state=PC_INFLIGHT;
        r->generation++;
        r->order=p->next_order++;
        r->correlation=correlation;
        r->result=(tm_pc_result_v1)TM_PC_RESULT_V1_INIT;
        r->result.correlation=correlation;
        r->result.provider_generation=p->provider_generation;
        out->incarnation=p->incarnation;
        out->slot=i;
        out->generation=r->generation;
        rc=TM_PC_OK;
        break;
    }
    pc_unlock(p);
    return rc;
}
tm_pc_status tm_pc_fail_start(tm_pc_portal *p,tm_pc_ticket_v1 t,int32_t status) {
    pc_row *r;
    tm_pc_status rc;
    if (!p) return TM_PC_EINVAL;
    pc_lock(p);
    r=pc_find(p,t);
    if (!r) rc=TM_PC_ESTALE;
    else if (r->state!=PC_INFLIGHT) rc=TM_PC_EALREADY;
    else { pc_ready(r,TM_PC_START_FAILED,0u,status); rc=TM_PC_OK; }
    pc_unlock(p);
    return rc;
}
tm_pc_status tm_pc_complete(tm_pc_portal *p,tm_pc_ticket_v1 t,int32_t status) {
    pc_row *r;
    tm_pc_status rc;
    if (!p) return TM_PC_EINVAL;
    pc_lock(p);
    r=pc_find(p,t);
    if (!r) rc=TM_PC_ESTALE;
    else if (r->state==PC_INFLIGHT) {
        pc_ready(r,TM_PC_COMPLETED,1u,status);
        rc=TM_PC_OK;
    } else if (r->state==PC_CANCEL_PENDING) {
        pc_ready(r,TM_PC_CANCELLED,1u,status);
        rc=TM_PC_OK;
    } else rc=TM_PC_EALREADY;
    pc_unlock(p);
    return rc;
}
tm_pc_status tm_pc_request_cancel(tm_pc_portal *p,tm_pc_ticket_v1 t) {
    pc_row *r;
    tm_pc_status rc;
    if (!p) return TM_PC_EINVAL;
    pc_lock(p);
    r=pc_find(p,t);
    if (!r) rc=TM_PC_ESTALE;
    else if (r->state==PC_INFLIGHT) {
        r->state=PC_CANCEL_PENDING;
        rc=TM_PC_OK;
    } else rc=TM_PC_EALREADY;
    pc_unlock(p);
    return rc;
}
tm_pc_status tm_pc_cancel_quiesced(tm_pc_portal *p,tm_pc_ticket_v1 t) {
    pc_row *r;
    tm_pc_status rc;
    if (!p) return TM_PC_EINVAL;
    pc_lock(p);
    r=pc_find(p,t);
    if (!r) rc=TM_PC_ESTALE;
    else if (r->state==PC_CANCEL_PENDING) {
        pc_ready(r,TM_PC_CANCELLED,0u,0);
        rc=TM_PC_OK;
    } else rc=TM_PC_EALREADY;
    pc_unlock(p);
    return rc;
}
tm_pc_status tm_pc_take(tm_pc_portal *p,tm_pc_ticket_v1 *out,
                        tm_pc_result_v1 *result) {
    uint32_t i,index=UINT32_MAX;
    uint64_t order=UINT64_MAX;
    tm_pc_status rc=TM_PC_EBUSY;
    if (!p || !out || !result) return TM_PC_EINVAL;
    if (result->version!=TM_PC_ABI_V1) return TM_PC_EVERSION;
    if (result->size!=sizeof(*result)) return TM_PC_EINVAL;
    memset(out,0,sizeof(*out));
    pc_lock(p);
    for (i=0u;i<p->capacity;++i)
        if (p->rows[i].state==PC_READY && p->rows[i].order<order) {
            index=i;order=p->rows[i].order;
        }
    if (index!=UINT32_MAX) {
        pc_row *r=&p->rows[index];
        r->state=PC_TAKEN;
        *result=r->result;
        out->incarnation=p->incarnation;
        out->slot=index;
        out->generation=r->generation;
        rc=TM_PC_OK;
    }
    pc_unlock(p);
    return rc;
}
tm_pc_status tm_pc_settle(tm_pc_portal *p,tm_pc_ticket_v1 t) {
    pc_row *r;
    tm_pc_status rc;
    if (!p) return TM_PC_EINVAL;
    pc_lock(p);
    r=pc_find(p,t);
    if (!r) rc=TM_PC_ESTALE;
    else if (r->state!=PC_TAKEN) rc=TM_PC_EBUSY;
    else {
        r->state=PC_FREE;
        r->order=0u;
        r->correlation=0u;
        memset(&r->result,0,sizeof(r->result));
        rc=TM_PC_OK;
    }
    pc_unlock(p);
    return rc;
}
tm_pc_status tm_pc_seal(tm_pc_portal *p) {
    if (!p) return TM_PC_EINVAL;
    pc_lock(p);
    p->sealed=1u;
    pc_unlock(p);
    return TM_PC_OK;
}
tm_pc_status tm_pc_snapshot(tm_pc_portal *p,tm_pc_snapshot_v1 *out) {
    uint32_t i;
    if (!p || !out) return TM_PC_EINVAL;
    if (out->version!=TM_PC_ABI_V1) return TM_PC_EVERSION;
    if (out->size!=sizeof(*out)) return TM_PC_EINVAL;
    pc_lock(p);
    out->capacity=p->capacity;
    out->inflight=out->cancel_pending=out->ready=out->taken=0u;
    out->sealed=p->sealed;
    for (i=0u;i<p->capacity;++i) {
        switch (p->rows[i].state) {
        case PC_INFLIGHT:out->inflight++;break;
        case PC_CANCEL_PENDING:out->cancel_pending++;break;
        case PC_READY:out->ready++;break;
        case PC_TAKEN:out->taken++;break;
        default:break;
        }
    }
    out->drained=!(out->inflight || out->cancel_pending || out->ready || out->taken);
    pc_unlock(p);
    return TM_PC_OK;
}
tm_pc_status tm_pc_destroy(tm_pc_portal *p) {
    uint32_t i;
    if (!p) return TM_PC_EINVAL;
    /* Exclusive lifecycle: no producer/callback may be inside or about to
     * call this portal. The host must join callbacks before this call. */
    pc_lock(p);
    if (!p->sealed) {pc_unlock(p);return TM_PC_EBUSY;}
    for(i=0u;i<p->capacity;++i)
        if (p->rows[i].state!=PC_FREE) {
            pc_unlock(p);
            return TM_PC_EBUSY;
        }
    pc_unlock(p);
    free(p->rows);
    free(p);
    return TM_PC_OK;
}
