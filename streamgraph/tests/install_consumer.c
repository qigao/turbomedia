#include <turbo_streamgraph.h>
#include <turbo_frame_store.h>
#include <turbo_streamgraph_fanout.h>
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
    if (tm_fs_seal(store) != TM_FS_OK) return 2;
    return tm_fs_destroy(store) == TM_FS_OK ? 0 : 3;
}
