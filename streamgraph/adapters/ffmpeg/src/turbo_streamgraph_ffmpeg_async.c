#include "turbo_streamgraph_ffmpeg_async.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Only short metadata/copy critical sections. No libavcodec, Graph, Portal
 * or wake callback ever executes while the metadata lock is held. */
enum { FFA_EMPTY=0u, FFA_DECODING=1u, FFA_READY=2u };
struct tm_ffa_decoder {
    tm_ff_source *codec; /* worker-only after creation */
    uint8_t *work;       /* worker-only */
    uint8_t *published;  /* guarded by lock */
    size_t frame_bytes;
    _Atomic uint32_t lock;
    _Atomic uint32_t worker_active;
    uint32_t state,armed,cancelled;
    tm_pc_ticket_v1 ticket;
    uint64_t decoded,delivered,discarded,notifications_failed;
};
static void ffa_lock(tm_ffa_decoder *d) {
    while(atomic_exchange_explicit(&d->lock,1u,memory_order_acquire)!=0u) {}
}
static void ffa_unlock(tm_ffa_decoder *d) {
    atomic_store_explicit(&d->lock,0u,memory_order_release);
}
static int same_ticket(tm_pc_ticket_v1 a,tm_pc_ticket_v1 b) {
    return a.incarnation==b.incarnation && a.slot==b.slot &&
           a.generation==b.generation;
}
tm_ff_status tm_ffa_create(const tm_ff_config_v1 *cfg,tm_ffa_decoder **out) {
    tm_ffa_decoder *d;
    tm_ff_status rc;
    size_t n;
    if(!out || *out || !cfg || !cfg->width || !cfg->height)
        return TM_FF_EINVAL;
    if(cfg->version!=TM_FF_ABI_V1) return TM_FF_EVERSION;
    if(cfg->size!=sizeof(*cfg) || cfg->reserved ||
       cfg->width>4096u || cfg->height>4096u ||
       (uint64_t)cfg->width*cfg->height>TM_FS_MAX_FRAME_BYTES)
        return TM_FF_EINVAL;
    n=(size_t)cfg->width*cfg->height;
    d=(tm_ffa_decoder *)calloc(1u,sizeof(*d));
    if(!d)return TM_FF_ENOMEM;
    d->work=(uint8_t *)malloc(n);
    d->published=(uint8_t *)malloc(n);
    if(!d->work || !d->published) {
        free(d->work);free(d->published);free(d);
        return TM_FF_ENOMEM;
    }
    d->frame_bytes=n;
    atomic_init(&d->lock,0u);
    atomic_init(&d->worker_active,0u);
    rc=tm_ff_create(cfg,&d->codec);
    if(rc!=TM_FF_OK) {
        free(d->work);free(d->published);free(d);
        return rc;
    }
    *out=d;
    return TM_FF_OK;
}
tm_ff_status tm_ffa_run_mjpeg(
    tm_ffa_decoder *d,const uint8_t *compressed,size_t bytes,
    tm_pc_portal *portal,tm_pc_ticket_v1 ticket,
    tm_ffa_wake_fn wake,void *wake_context) {
    tm_ex_source_result decoded=TM_EX_SOURCE_FAILED;
    tm_ff_status status=TM_FF_OK,committed;
    tm_pc_status terminal;
    size_t out_bytes=0u;
    int notify_rc=0;
    if(!d || !compressed || !bytes || !portal ||
       !ticket.incarnation || !ticket.generation)return TM_FF_EINVAL;
    if(atomic_exchange_explicit(&d->worker_active,1u,memory_order_acq_rel))
        return TM_FF_EFULL;
    ffa_lock(d);
    if(d->state!=FFA_EMPTY || d->cancelled) {
        ffa_unlock(d);
        atomic_store_explicit(&d->worker_active,0u,memory_order_release);
        return TM_FF_ESTATE;
    }
    d->state=FFA_DECODING;
    d->ticket=ticket;
    ffa_unlock(d);
    /* Sole FFmpeg owner is this explicitly admitted worker. The Main Media
     * Owner never accesses codec/work while worker_active is true. */
    status=tm_ff_submit_mjpeg(d->codec,compressed,bytes);
    if(status==TM_FF_OK) {
        decoded=tm_ff_next_cpu_luma(d->codec,d->work,d->frame_bytes,&out_bytes);
        if(decoded!=TM_EX_SOURCE_FRAME || out_bytes!=d->frame_bytes)
            status=TM_FF_ECODEC;
    }
    ffa_lock(d);
    if(!d->cancelled && status==TM_FF_OK) {
        memcpy(d->published,d->work,d->frame_bytes);
        d->state=FFA_READY;
        d->decoded++;
    } else {
        d->state=FFA_EMPTY;
        if(d->cancelled) {
            status=TM_FF_ESTATE;
            d->discarded++;
        }
    }
    ffa_unlock(d);
    /* This commit is ordered AFTER the CPU slot publication; the wake tail
     * is ordered AFTER terminal publication. Neither settles the Portal. */
    terminal=tm_pc_complete(portal,ticket,(int32_t)status);
    if(terminal!=TM_PC_OK) {
        ffa_lock(d);
        if(d->state==FFA_READY && same_ticket(d->ticket,ticket)) {
            d->state=FFA_EMPTY;
            d->armed=0u;
            d->discarded++;
        }
        ffa_unlock(d);
        status=TM_FF_ESTATE;
    }
    if(wake)notify_rc=wake(wake_context);
    if(notify_rc!=0) {
        ffa_lock(d);
        d->notifications_failed++;
        ffa_unlock(d);
        /* Wake failure does not roll back a committed Portal terminal and
         * must not cause a duplicate decode, callback or implicit retry. */
    }
    committed=status;
    atomic_store_explicit(&d->worker_active,0u,memory_order_release);
    return committed;
}
tm_ff_status tm_ffa_owner_accept(tm_ffa_decoder *d,tm_pc_ticket_v1 ticket) {
    tm_ff_status rc=TM_FF_ESTALE;
    if(!d || !ticket.incarnation || !ticket.generation)return TM_FF_EINVAL;
    ffa_lock(d);
    if(d->state==FFA_READY && same_ticket(d->ticket,ticket) && !d->cancelled) {
        rc=d->armed?TM_FF_ESTATE:TM_FF_OK;
        d->armed=1u;
    }
    ffa_unlock(d);
    return rc;
}
tm_ex_source_result tm_ffa_owner_next_luma(
    void *context,uint8_t *dst,size_t capacity,size_t *out_bytes) {
    tm_ffa_decoder *d=(tm_ffa_decoder *)context;
    tm_ex_source_result rc=TM_EX_SOURCE_FAILED;
    if(!d || !dst || !out_bytes)return TM_EX_SOURCE_FAILED;
    *out_bytes=0u;
    ffa_lock(d);
    if(d->state==FFA_READY && d->armed && !d->cancelled &&
       capacity>=d->frame_bytes) {
        memcpy(dst,d->published,d->frame_bytes);
        *out_bytes=d->frame_bytes;
        d->state=FFA_EMPTY;
        d->armed=0u;
        memset(&d->ticket,0,sizeof(d->ticket));
        d->delivered++;
        rc=TM_EX_SOURCE_FRAME;
    }
    ffa_unlock(d);
    return rc;
}
tm_ff_status tm_ffa_owner_cancel(tm_ffa_decoder *d) {
    if(!d)return TM_FF_EINVAL;
    ffa_lock(d);
    d->cancelled=1u;
    if(d->state==FFA_READY) {
        d->state=FFA_EMPTY;
        d->armed=0u;
        d->discarded++;
        memset(&d->ticket,0,sizeof(d->ticket));
    }
    ffa_unlock(d);
    return TM_FF_OK;
}
tm_ff_status tm_ffa_codec_activity(tm_ffa_decoder *d,
                                    tm_ff_codec_activity_v1 *out) {
    if(!d || !out)return TM_FF_EINVAL;
    /* Only atomic counters are accessed in the underlying Source. The
     * Media Owner retains a live decoder until the worker joins. */
    return tm_ff_codec_activity(d->codec,out);
}
tm_ff_status tm_ffa_stats(tm_ffa_decoder *d,tm_ffa_stats_v1 *out) {
    if(!d || !out)return TM_FF_EINVAL;
    if(out->version!=TM_FFA_ABI_V1)return TM_FF_EVERSION;
    if(out->size!=sizeof(*out))return TM_FF_EINVAL;
    ffa_lock(d);
    *out=(tm_ffa_stats_v1)TM_FFA_STATS_V1_INIT;
    out->state=d->state;
    out->worker_active=atomic_load_explicit(&d->worker_active,memory_order_acquire);
    out->decoded=d->decoded;out->delivered=d->delivered;
    out->discarded=d->discarded;out->cancelled=d->cancelled;
    out->notifications_failed=d->notifications_failed;
    ffa_unlock(d);
    return TM_FF_OK;
}
tm_ff_status tm_ffa_destroy(tm_ffa_decoder *d) {
    if(!d)return TM_FF_EINVAL;
    /* Exclusively called AFTER host-established wake/callback quiescence.
     * Acquire the gate to reject destruction during a live worker tail. */
    if(atomic_exchange_explicit(&d->worker_active,1u,memory_order_acq_rel))
        return TM_FF_ESTATE;
    ffa_lock(d);
    if(d->state!=FFA_EMPTY) {
        ffa_unlock(d);
        atomic_store_explicit(&d->worker_active,0u,memory_order_release);
        return TM_FF_ESTATE;
    }
    ffa_unlock(d);
    (void)tm_ff_destroy(d->codec);
    free(d->work);
    free(d->published);
    free(d);
    return TM_FF_OK;
}
