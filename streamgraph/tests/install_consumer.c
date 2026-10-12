#include <turbo_streamgraph.h>
#include <turbo_frame_store.h>
#include <turbo_streamgraph_fanout.h>
#include <turbo_streamgraph_executor.h>
#include <turbo_streamgraph_handoff.h>
#include <turbo_streamgraph_owner_dispatch.h>
#include <turbo_streamgraph_provider_completion.h>
#include <assert.h>
int main(void) {
    tm_sg_port_v1 p = TM_SG_PORT_V1_INIT;
    tm_sg_diagnostic_v1 d = TM_SG_DIAGNOSTIC_V1_INIT;
    tm_sg_program *program = NULL;
    assert(p.size == sizeof(p));
    assert(d.version == TM_SG_ABI_VERSION);
    assert(tm_sg_compile(NULL, 0, NULL, 0, NULL, &program, &d) == TM_SG_ELIMIT);
    tm_fs_config_v1 cfg = TM_FS_CONFIG_V1_INIT;
    tm_fs_store *store = NULL;
    cfg.store_id = 400u;
    cfg.slot_capacity = 2u;
    cfg.max_frame_bytes = 64u;
    cfg.max_pins = 2u;
    cfg.max_leases = 2u;
    cfg.max_retained_bytes = 128u;
    if (program != NULL || tm_fs_create(&cfg, &store) != TM_FS_OK) return 1;
    {
        tm_fo_config_v1 fc = TM_FO_CONFIG_V1_INIT;
        tm_fo_branch_config_v1 bc = TM_FO_BRANCH_CONFIG_V1_INIT;
        tm_fo_owner *fanout = NULL;
        fc.branch_count = 1u;
        bc.policy = TM_FO_ORDERED_LOSSLESS;
        bc.queue_depth = 1u;
        fc.branches[0] = bc;
        if (tm_fo_create(store, &fc, &fanout) != TM_FO_OK) return 4;
        if (tm_fo_seal(fanout) != TM_FO_OK || tm_fo_destroy(fanout) != TM_FO_OK) return 5;
    }
    {
        tm_ho_inbox *inbox = NULL;
        tm_ho_config_v1 ic = TM_HO_CONFIG_V1_INIT;
        tm_ho_ticket_v1 prepared={0},taken={0};
        tm_ho_command_v1 send = TM_HO_COMMAND_V1_INIT, received={0};
        tm_ho_snapshot_v1 snapshot = TM_HO_SNAPSHOT_V1_INIT;
        ic.capacity = 2u;
        send.kind = TM_HO_PUMP_SOURCE;
        send.correlation = 16u;
        if (tm_ho_create(&ic,&inbox)!=TM_HO_OK) return 6;
        if (tm_ho_reserve(inbox,&prepared)!=TM_HO_OK) return 7;
        if (tm_ho_publish(inbox,prepared,&send)!=TM_HO_OK) return 8;
        if (tm_ho_take(inbox,&taken,&received)!=TM_HO_OK ||
            received.kind!=TM_HO_PUMP_SOURCE || received.correlation!=16u) return 9;
        if (tm_ho_settle(inbox,taken)!=TM_HO_OK) return 10;
        if (tm_ho_seal(inbox)!=TM_HO_OK ||
            tm_ho_snapshot(inbox,&snapshot)!=TM_HO_OK || !snapshot.drained ||
            tm_ho_destroy(inbox)!=TM_HO_OK) return 11;
    }
    {
        tm_od_config_v1 dispatch_cfg=TM_OD_CONFIG_V1_INIT;
        tm_od_receipt_v1 receipt=TM_OD_RECEIPT_V1_INIT;
        tm_od_owner *dispatcher=NULL;
        if(dispatch_cfg.size!=sizeof(dispatch_cfg) ||
           receipt.version!=TM_OD_ABI_V1 ||
           tm_od_create(NULL,NULL,store,&dispatch_cfg,&dispatcher)!=TM_OD_EINVAL ||
           dispatcher!=NULL) return 12;
    }
    {
        tm_pc_config_v1 pc_cfg=TM_PC_CONFIG_V1_INIT;
        tm_pc_portal *portal=NULL;
        tm_pc_ticket_v1 ticket={0},taken={0};
        tm_pc_result_v1 result=TM_PC_RESULT_V1_INIT;
        pc_cfg.capacity=2u; pc_cfg.provider_generation=7u;
        if (tm_pc_create(&pc_cfg,&portal)!=TM_PC_OK) return 13;
        if (tm_pc_begin(portal,67u,&ticket)!=TM_PC_OK) return 14;
        if (tm_pc_complete(portal,ticket,0)!=TM_PC_OK) return 15;
        if (tm_pc_take(portal,&taken,&result)!=TM_PC_OK ||
            result.correlation!=67u || result.outcome!=TM_PC_COMPLETED) return 16;
        if (tm_pc_settle(portal,taken)!=TM_PC_OK) return 17;
        if (tm_pc_seal(portal)!=TM_PC_OK ||
            tm_pc_destroy(portal)!=TM_PC_OK) return 18;
    }
    if (tm_fs_seal(store) != TM_FS_OK) return 2;
    return tm_fs_destroy(store) == TM_FS_OK ? 0 : 3;
}
