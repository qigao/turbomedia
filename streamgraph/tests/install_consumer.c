#include <turbo_streamgraph.h>
#include <turbo_frame_store.h>
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
    if (tm_fs_seal(store) != TM_FS_OK) return 2;
    return tm_fs_destroy(store) == TM_FS_OK ? 0 : 3;
}
