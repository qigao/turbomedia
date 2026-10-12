#include "turbo_streamgraph_executor.h"
#include <stdlib.h>
#include <string.h>

struct tm_ex_owner {
    const tm_sg_program *program;
    tm_fs_store *store;
    tm_fo_owner *fanout;
    tm_ex_binding_v1 source,processor,sinks[TM_FO_MAX_BRANCHES];
    uint32_t sink_count;
    uint8_t *scratch;
    size_t scratch_capacity,pending_bytes;
    uint64_t source_session_generation,display_generation;
    tm_fs_frame_ref_v1 pending;
    tm_ex_stats_v1 stats;
    int stopped,in_callback;
};
static tm_ex_status fo_code(tm_fo_status rc) {
    switch(rc){
    case TM_FO_OK:return TM_EX_OK;
    case TM_FO_EFULL:return TM_EX_EFULL;
    case TM_FO_EBUSY:return TM_EX_EBUSY;
    case TM_FO_ESHUTDOWN:return TM_EX_ESTATE;
    default:return TM_EX_EFRAME;
    }
}
static const tm_sg_node_v1 *node_named(const tm_sg_program *p,const char *id){
    uint32_t i;
    if(!id)return NULL;
    for(i=0u;i<tm_sg_program_node_count(p);++i){
        const tm_sg_node_v1 *n=NULL;
        if(tm_sg_program_node_at(p,i,&n)==TM_SG_OK &&
           strcmp(n->id,id)==0)return n;
    }
    return NULL;
}
static int valid_port(const tm_sg_port_v1 *p){
    return (p->direction==TM_SG_INPUT || p->direction==TM_SG_OUTPUT) &&
        p->kind==TM_SG_VIDEO_FRAME && p->memory_domain==TM_SG_MEMORY_CPU &&
        p->format_id!=0u && p->timebase_num!=0u && p->timebase_den!=0u;
}
/* Only a shape that this first owner runtime can execute is accepted.
 * Unknown DAGs must fail rather than silently skip a node. */
static int graph_shape(const tm_sg_program *p,uint32_t *sink_count){
    uint32_t i,j,sources=0,processors=0,sinks=0,srcproc=0,procsink=0;
    const tm_sg_node_v1 *source=NULL,*processor=NULL;
    for(i=0;i<tm_sg_program_node_count(p);++i){
        const tm_sg_node_v1 *n=NULL;
        if(tm_sg_program_node_at(p,i,&n)!=TM_SG_OK)return 0;
        if(n->kind==TM_SG_SOURCE){
            if(++sources!=1u || n->port_count!=1u)return 0;
            source=n;
        }else if(n->kind==TM_SG_PROCESSOR){
            if(++processors!=1u || n->port_count!=2u)return 0;
            processor=n;
        }else if(n->kind==TM_SG_SINK){
            if(++sinks>TM_FO_MAX_BRANCHES || n->port_count!=1u)return 0;
        }else return 0;
        for(j=0;j<n->port_count;++j)if(!valid_port(&n->ports[j]))return 0;
    }
    if(sources!=1u || processors!=1u || !sinks ||
       tm_sg_program_edge_count(p)!=sinks+1u)return 0;
    for(i=0;i<tm_sg_program_edge_count(p);++i){
        const tm_sg_edge_v1 *e=NULL;
        if(tm_sg_program_edge_at(p,i,&e)!=TM_SG_OK)return 0;
        if(strcmp(e->source_node,source->id)==0 &&
           strcmp(e->target_node,processor->id)==0)++srcproc;
        else if(strcmp(e->source_node,processor->id)==0){
            const tm_sg_node_v1 *sink=node_named(p,e->target_node);
            if(!sink || sink->kind!=TM_SG_SINK)return 0;
            ++procsink;
        }else return 0;
    }
    if(srcproc!=1u || procsink!=sinks)return 0;
    *sink_count=sinks;
    return 1;
}
static int binding_valid(const tm_ex_binding_v1 *b){
    return b->size==sizeof(*b) && b->version==TM_EX_ABI_V1 &&
           b->node_id && (!!b->source_next != !!b->visit);
}
tm_ex_status tm_ex_create(const tm_sg_program *p,tm_fs_store *store,
    const tm_ex_config_v1 *cfg,const tm_ex_binding_v1 *bindings,
    size_t binding_count,tm_ex_owner **out){
    tm_ex_owner *o;
    uint32_t sinks=0,i,k=0;
    if(!out || *out || !p || !store || !cfg || !bindings ||
       !cfg->source_session_generation || !cfg->display_generation ||
       cfg->reserved || !cfg->max_frame_bytes ||
       cfg->max_frame_bytes>TM_FS_MAX_FRAME_BYTES)return TM_EX_EINVAL;
    if(cfg->version!=TM_EX_ABI_V1)return TM_EX_EVERSION;
    if(cfg->size!=sizeof(*cfg) || !graph_shape(p,&sinks) ||
       cfg->fanout.branch_count!=sinks || binding_count!=sinks+2u)
        return TM_EX_EGRAPH;
    for(i=0;i<binding_count;++i){
        const tm_sg_node_v1 *n;
        uint32_t j;
        if(!binding_valid(&bindings[i]) ||
           !(n=node_named(p,bindings[i].node_id)) ||
           ((n->kind==TM_SG_SOURCE)!=!!bindings[i].source_next))return TM_EX_EGRAPH;
        for(j=0;j<i;++j)
            if(strcmp(bindings[i].node_id,bindings[j].node_id)==0)return TM_EX_EGRAPH;
    }
    o=(tm_ex_owner *)calloc(1,sizeof(*o));
    if(!o)return TM_EX_ENOMEM;
    o->scratch=(uint8_t *)malloc(cfg->max_frame_bytes);
    if(!o->scratch){free(o);return TM_EX_ENOMEM;}
    o->scratch_capacity=cfg->max_frame_bytes;
    o->source_session_generation=cfg->source_session_generation;
    o->display_generation=cfg->display_generation;
    o->program=p; o->store=store; o->sink_count=sinks;
    o->stats.size=sizeof(o->stats); o->stats.version=TM_EX_ABI_V1;
    for(i=0;i<binding_count;++i){
        const tm_sg_node_v1 *n=node_named(p,bindings[i].node_id);
        if(n->kind==TM_SG_SOURCE)o->source=bindings[i];
        else if(n->kind==TM_SG_PROCESSOR)o->processor=bindings[i];
    }
    for(i=0;i<tm_sg_program_edge_count(p);++i){
        const tm_sg_edge_v1 *e=NULL;
        const tm_sg_node_v1 *n=NULL;
        uint32_t j;
        (void)tm_sg_program_edge_at(p,i,&e);
        n=node_named(p,e->target_node);
        if(n && n->kind==TM_SG_SINK){
            for(j=0;j<binding_count;++j)
                if(strcmp(e->target_node,bindings[j].node_id)==0){
                    o->sinks[k++]=bindings[j]; break;
                }
        }
    }
    if(k!=sinks || tm_fo_create(store,&cfg->fanout,&o->fanout)!=TM_FO_OK){
        free(o->scratch);free(o);return TM_EX_EFRAME;
    }
    *out=o;
    return TM_EX_OK;
}
static tm_ex_status retire_ref(tm_ex_owner *o,tm_fs_frame_ref_v1 *ref){
    tm_fs_status st=tm_fs_retire(o->store,ref);
    if(st!=TM_FS_OK && st!=TM_FS_EBUSY)return TM_EX_EFRAME;
    memset(ref,0,sizeof(*ref));
    return TM_EX_OK;
}
tm_ex_status tm_ex_pump_source(tm_ex_owner *o){
    tm_fs_lease_v1 lease={0};
    tm_fs_cpu_view_v1 view=TM_FS_CPU_VIEW_V1_INIT;
    if(!o || o->in_callback)return TM_EX_EINVAL;
    if(o->stopped)return TM_EX_EOS;
    if(o->pending.frame_id){
        tm_fo_status fo=tm_fo_submit(o->fanout,&o->pending);
        if(fo==TM_FO_EFULL){o->stats.backpressure++;return TM_EX_EFULL;}
        if(fo!=TM_FO_OK)return fo_code(fo);
        o->stats.submitted++;
        return retire_ref(o,&o->pending);
    }
    if(!o->pending_bytes){
        size_t bytes=0u;
        tm_ex_source_result result;
        o->in_callback=1;
        result=o->source.source_next(o->source.ctx,o->scratch,o->scratch_capacity,&bytes);
        o->in_callback=0;
        if(result==TM_EX_SOURCE_EOS){
            o->stopped=1; o->stats.stopped=1u;
            return fo_code(tm_fo_seal(o->fanout))==TM_EX_OK?TM_EX_EOS:TM_EX_EFRAME;
        }
        if(result!=TM_EX_SOURCE_FRAME || !bytes || bytes>o->scratch_capacity){
            (void)tm_ex_stop(o); return TM_EX_EPROVIDER;
        }
        o->pending_bytes=bytes;
        o->stats.sourced++;
    }
    {
        tm_fs_status fs=tm_fs_publish_cpu(o->store,o->scratch,o->pending_bytes,
                          o->source_session_generation,o->display_generation,&o->pending);
        if(fs==TM_FS_EFULL){o->stats.backpressure++;return TM_EX_EFULL;}
        if(fs!=TM_FS_OK){(void)tm_ex_stop(o);return TM_EX_EFRAME;}
        o->pending_bytes=0u;
    }
    if(tm_fs_acquire_weak(o->store,&o->pending,&lease)!=TM_FS_OK ||
       tm_fs_map_cpu(o->store,&lease,&view)!=TM_FS_OK){
        if(lease.ticket_id)(void)tm_fs_release_lease(o->store,&lease);
        (void)tm_ex_stop(o);return TM_EX_EFRAME;
    }
    o->in_callback=1;
    {
        int result=o->processor.visit(o->processor.ctx,&o->pending,&view);
        o->in_callback=0;
        (void)tm_fs_release_lease(o->store,&lease);
        if(result!=0){(void)tm_ex_stop(o);return TM_EX_EPROVIDER;}
    }
    o->stats.processed++;
    return tm_ex_pump_source(o); /* submit once; never repoll */
}
tm_ex_status tm_ex_pump_sink(tm_ex_owner *o,uint32_t branch){
    tm_fo_ticket_v1 t={0};
    tm_fs_frame_ref_v1 ref=TM_FS_FRAME_REF_V1_INIT;
    tm_fs_lease_v1 lease={0};
    tm_fs_cpu_view_v1 view=TM_FS_CPU_VIEW_V1_INIT;
    tm_fo_status st;
    int provider_rc;
    if(!o || o->in_callback || branch>=o->sink_count)return TM_EX_EINVAL;
    st=tm_fo_take(o->fanout,branch,&t,&ref);
    if(st==TM_FO_EBUSY)return TM_EX_EBUSY;
    if(st!=TM_FO_OK)return fo_code(st);
    if(tm_fo_acquire_lease(o->fanout,t,&lease)!=TM_FO_OK ||
       tm_fs_map_cpu(o->store,&lease,&view)!=TM_FS_OK){
        if(lease.ticket_id)(void)tm_fs_release_lease(o->store,&lease);
        (void)tm_fo_settle(o->fanout,t);
        (void)tm_ex_stop(o);return TM_EX_EFRAME;
    }
    o->in_callback=1;
    provider_rc=o->sinks[branch].visit(o->sinks[branch].ctx,&ref,&view);
    o->in_callback=0;
    if(tm_fs_release_lease(o->store,&lease)!=TM_FS_OK ||
       tm_fo_settle(o->fanout,t)!=TM_FO_OK){
        (void)tm_ex_stop(o);return TM_EX_EFRAME;
    }
    if(provider_rc!=0){(void)tm_ex_stop(o);return TM_EX_EPROVIDER;}
    o->stats.delivered++;
    return TM_EX_OK;
}
tm_ex_status tm_ex_stop(tm_ex_owner *o){
    if(!o || o->in_callback)return TM_EX_EINVAL;
    if(o->stopped)return TM_EX_OK;
    o->stopped=1;o->stats.stopped=1u;o->pending_bytes=0u;
    if(o->pending.frame_id && retire_ref(o,&o->pending)!=TM_EX_OK)return TM_EX_EFRAME;
    return fo_code(tm_fo_seal(o->fanout));
}
tm_ex_status tm_ex_discard_ready(tm_ex_owner *o,uint32_t branch){
    if(!o || o->in_callback || branch>=o->sink_count)return TM_EX_EINVAL;
    if(!o->stopped)return TM_EX_ESTATE;
    return fo_code(tm_fo_discard_ready(o->fanout,branch));
}
tm_ex_status tm_ex_stats(tm_ex_owner *o,tm_ex_stats_v1 *out){
    if(!o || !out || o->in_callback)return TM_EX_EINVAL;
    if(out->version!=TM_EX_ABI_V1)return TM_EX_EVERSION;
    if(out->size!=sizeof(*out))return TM_EX_EINVAL;
    *out=o->stats;
    out->pending=!!o->pending.frame_id || !!o->pending_bytes;
    return TM_EX_OK;
}
tm_ex_status tm_ex_destroy(tm_ex_owner *o){
    tm_fo_status st;
    if(!o || o->in_callback)return TM_EX_EINVAL;
    if(!o->stopped)return TM_EX_EBUSY;
    st=tm_fo_destroy(o->fanout);
    if(st!=TM_FO_OK)return fo_code(st);
    free(o->scratch);free(o);
    return TM_EX_OK;
}
