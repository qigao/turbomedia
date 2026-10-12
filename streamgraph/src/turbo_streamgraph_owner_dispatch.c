#include "turbo_streamgraph_owner_dispatch.h"

#include <stdlib.h>
#include <string.h>

struct tm_od_owner {
    tm_ho_inbox *inbox;
    tm_ex_owner *executor;
    tm_fs_store *store;
    tm_od_frame_visit_fn visitor;
    void *visitor_context;
    uint32_t in_step;
    uint32_t source_ended;
    uint32_t owner_failed;
};

tm_od_status tm_od_create(tm_ho_inbox *inbox, tm_ex_owner *executor,
                          tm_fs_store *store, const tm_od_config_v1 *cfg,
                          tm_od_owner **out) {
    tm_od_owner *o;
    if (!inbox || !executor || !store || !cfg || !out || *out)
        return TM_OD_EINVAL;
    if (cfg->version != TM_OD_ABI_V1) return TM_OD_EVERSION;
    if (cfg->size != sizeof(*cfg) || cfg->reserved || cfg->reserved2)
        return TM_OD_EINVAL;
    o=(tm_od_owner *)calloc(1u,sizeof(*o));
    if (!o) return TM_OD_ENOMEM;
    o->inbox=inbox;
    o->executor=executor;
    o->store=store;
    o->visitor=cfg->visit_frame;
    o->visitor_context=cfg->visit_context;
    *out=o;
    return TM_OD_OK;
}

/* A borrowed FrameRef has no lifetime guarantee during the MPSC queue wait.
 * Promotion to a pin and Lease happens ONLY on the serialized Media Owner. */
static void od_frame_notice(tm_od_owner *o, const tm_fs_frame_ref_v1 *ref,
                            tm_od_receipt_v1 *r) {
    tm_fs_pin_v1 pin={0};
    tm_fs_lease_v1 lease={0};
    tm_fs_cpu_view_v1 view=TM_FS_CPU_VIEW_V1_INIT;
    tm_fs_status status;
    int callback_status=0;
    if (!o->visitor) {
        r->outcome=TM_OD_REJECTED;
        return;
    }
    status=tm_fs_reserve_pin(o->store,ref,&pin);
    if (status!=TM_FS_OK) {
        r->frame_status=status;
        r->outcome=status==TM_FS_ESTALE || status==TM_FS_EBUSY ?
            TM_OD_STALE_FRAME : TM_OD_FAILED;
        return;
    }
    status=tm_fs_acquire_pinned(o->store,&pin,&lease);
    if (status==TM_FS_OK)
        status=tm_fs_map_cpu(o->store,&lease,&view);
    if (status==TM_FS_OK)
        callback_status=o->visitor(o->visitor_context,ref,&view);
    if (lease.ticket_id) {
        tm_fs_status released=tm_fs_release_lease(o->store,&lease);
        if (released!=TM_FS_OK) status=released;
    }
    {
        tm_fs_status released=tm_fs_release_pin(o->store,&pin);
        if (released!=TM_FS_OK) status=released;
    }
    r->frame_status=status;
    if (status!=TM_FS_OK) {
        r->outcome=TM_OD_FAILED;
        o->owner_failed=1u;
        (void)tm_ho_seal(o->inbox);
        (void)tm_ex_stop(o->executor);
    } else if (callback_status!=0) {
        r->outcome=TM_OD_FAILED;
        r->executor_status=TM_EX_EPROVIDER;
        o->owner_failed=1u;
        (void)tm_ho_seal(o->inbox);
        (void)tm_ex_stop(o->executor);
    } else r->outcome=TM_OD_COMPLETED;
}

tm_od_status tm_od_step(tm_od_owner *o, tm_od_receipt_v1 *receipt) {
    tm_ho_ticket_v1 ticket={0};
    tm_ho_command_v1 command={0};
    tm_ho_status qrc;
    tm_ex_status erc=TM_EX_OK;
    tm_od_receipt_v1 result=TM_OD_RECEIPT_V1_INIT;
    if (!o || !receipt) return TM_OD_EINVAL;
    if (receipt->version!=TM_OD_ABI_V1) return TM_OD_EVERSION;
    if (receipt->size!=sizeof(*receipt)) return TM_OD_EINVAL;
    if (o->in_step) return TM_OD_EBUSY;
    o->in_step=1u;
    qrc=tm_ho_take(o->inbox,&ticket,&command);
    if (qrc==TM_HO_EBUSY) {
        o->in_step=0u;
        return TM_OD_EMPTY;
    }
    if (qrc!=TM_HO_OK) {
        o->in_step=0u;
        return TM_OD_EQUEUE;
    }
    result.command_kind=command.kind;
    result.correlation=command.correlation;
    result.outcome=TM_OD_REJECTED;
    if (command.kind==TM_HO_STOP) {
        (void)tm_ho_seal(o->inbox);
        erc=tm_ex_stop(o->executor);
        o->source_ended=1u;
    } else if (command.kind==TM_HO_PUMP_SOURCE) {
        if (!o->source_ended && !o->owner_failed)
            erc=tm_ex_pump_source(o->executor);
        else erc=TM_EX_ESTATE;
    } else if (command.kind==TM_HO_PUMP_SINK) {
        if (!o->owner_failed)
            erc=tm_ex_pump_sink(o->executor,command.branch);
        else erc=TM_EX_ESTATE;
    } else if (command.kind==TM_HO_FRAME_NOTICE) {
        if (!o->source_ended && !o->owner_failed)
            od_frame_notice(o,&command.frame,&result);
    }
    if (command.kind!=TM_HO_FRAME_NOTICE) {
        result.executor_status=erc;
        if (erc==TM_EX_OK) result.outcome=TM_OD_COMPLETED;
        else if (erc==TM_EX_EFULL || erc==TM_EX_EBUSY)
            result.outcome=TM_OD_BACKPRESSURE;
        else if (erc==TM_EX_EOS) result.outcome=TM_OD_EOS;
        else if (erc==TM_EX_ESTATE) result.outcome=TM_OD_REJECTED;
        else {
            result.outcome=TM_OD_FAILED;
            o->owner_failed=1u;
            (void)tm_ho_seal(o->inbox);
            (void)tm_ex_stop(o->executor);
        }
        if (command.kind==TM_HO_PUMP_SOURCE && erc==TM_EX_EOS) {
            o->source_ended=1u;
            (void)tm_ho_seal(o->inbox);
        }
    }
    qrc=tm_ho_settle(o->inbox,ticket);
    if (qrc!=TM_HO_OK) {
        /* No owner operation can safely proceed after a violated ticket
         * settlement. Keep the failed dispatcher alive for diagnosis. */
        o->owner_failed=1u;
        o->in_step=0u;
        return TM_OD_EQUEUE;
    }
    *receipt=result;
    o->in_step=0u;
    return TM_OD_OK;
}
tm_od_status tm_od_destroy(tm_od_owner *o) {
    tm_ho_snapshot_v1 snapshot=TM_HO_SNAPSHOT_V1_INIT;
    if (!o) return TM_OD_EINVAL;
    if (o->in_step) return TM_OD_EBUSY;
    if (tm_ho_snapshot(o->inbox,&snapshot)!=TM_HO_OK)
        return TM_OD_EQUEUE;
    if (!snapshot.sealed || !snapshot.drained)
        return TM_OD_EBUSY;
    /* Host must independently join producers, stop callbacks and drain
     * the borrowed Executor before destroying any of these owners. */
    free(o);
    return TM_OD_OK;
}
