#include <turbo_streamgraph_fanout.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static tm_fs_config_v1 store_config(uint32_t pins) {
    tm_fs_config_v1 c = TM_FS_CONFIG_V1_INIT;
    c.store_id = 111u;
    c.slot_capacity = 8u;
    c.max_frame_bytes = 32u;
    c.max_pins = pins;
    c.max_leases = 16u;
    c.max_retained_bytes = 256u;
    return c;
}
static tm_fo_config_v1 fanout_config(void) {
    tm_fo_config_v1 c = TM_FO_CONFIG_V1_INIT;
    tm_fo_branch_config_v1 b = TM_FO_BRANCH_CONFIG_V1_INIT;
    c.branch_count = 3u;
    b.policy = TM_FO_ORDERED_LOSSLESS;
    b.queue_depth = 2u;
    c.branches[0] = b;
    b.policy = TM_FO_LATEST;
    b.queue_depth = 1u;
    c.branches[1] = b;
    b.policy = TM_FO_SAMPLE;
    b.queue_depth = 2u;
    b.sample_every = 2u;
    c.branches[2] = b;
    return c;
}
static tm_fs_frame_ref_v1 frame(tm_fs_store *store, unsigned char data) {
    tm_fs_frame_ref_v1 f = TM_FS_FRAME_REF_V1_INIT;
    assert(tm_fs_publish_cpu(store, &data, 1u, 9u, 10u, &f) == TM_FS_OK);
    return f;
}
static void take_and_finish(tm_fo_owner *fo, tm_fs_store *store,
                            uint32_t branch, uint64_t expected) {
    tm_fo_ticket_v1 ticket = {0};
    tm_fs_frame_ref_v1 f = TM_FS_FRAME_REF_V1_INIT;
    tm_fs_lease_v1 lease = {0};
    tm_fs_cpu_view_v1 view = TM_FS_CPU_VIEW_V1_INIT;
    assert(tm_fo_take(fo, branch, &ticket, &f) == TM_FO_OK);
    assert(f.frame_id == expected);
    assert(tm_fo_acquire_lease(fo, ticket, &lease) == TM_FO_OK);
    assert(tm_fs_map_cpu(store, &lease, &view) == TM_FS_OK);
    assert(view.size_bytes == 1u && *view.data == (uint8_t)expected);
    assert(tm_fs_release_lease(store, &lease) == TM_FS_OK);
    assert(tm_fo_settle(fo, ticket) == TM_FO_OK);
    assert(tm_fo_settle(fo, ticket) == TM_FO_ESTALE);
}
static void test_latest_sample_lossless_and_drain(void) {
    tm_fs_store *store = NULL;
    tm_fo_owner *fo = NULL;
    tm_fs_config_v1 sc = store_config(16u);
    tm_fo_config_v1 fc = fanout_config();
    tm_fs_frame_ref_v1 a, b, c, consumed;
    tm_fo_ticket_v1 in_flight = {0};
    tm_fo_branch_stats_v1 stats = TM_FO_BRANCH_STATS_V1_INIT;
    assert(tm_fs_create(&sc, &store) == TM_FS_OK);
    assert(tm_fo_create(store, &fc, &fo) == TM_FO_OK);
    a = frame(store, 1u);
    assert(tm_fo_submit(fo, &a) == TM_FO_OK);
    assert(tm_fs_retire(store, &a) == TM_FS_EBUSY);
    b = frame(store, 2u);
    assert(tm_fo_submit(fo, &b) == TM_FO_OK);
    assert(tm_fs_retire(store, &b) == TM_FS_EBUSY);
    c = frame(store, 3u);
    assert(tm_fo_submit(fo, &c) == TM_FO_EFULL);
    assert(tm_fo_stats(fo, 0u, &stats) == TM_FO_OK);
    assert(stats.ready == 2u && stats.rejected == 1u);
    assert(tm_fo_stats(fo, 1u, &stats) == TM_FO_OK);
    assert(stats.replaced_ready == 1u && stats.ready == 1u);
    assert(tm_fo_stats(fo, 2u, &stats) == TM_FO_OK);
    assert(stats.sampled_out == 1u && stats.ready == 1u);
    assert(tm_fo_take(fo, 1u, &in_flight, &consumed) == TM_FO_OK);
    assert(consumed.frame_id == b.frame_id);
    take_and_finish(fo, store, 0u, 1u);
    assert(tm_fo_submit(fo, &c) == TM_FO_EFULL);
    {
        tm_fs_lease_v1 lease = {0};
        tm_fs_cpu_view_v1 view = TM_FS_CPU_VIEW_V1_INIT;
        assert(tm_fo_acquire_lease(fo, in_flight, &lease) == TM_FO_OK);
        assert(tm_fs_map_cpu(store, &lease, &view) == TM_FS_OK);
        assert(view.size_bytes == 1u && *view.data == 2u);
        assert(tm_fs_release_lease(store, &lease) == TM_FS_OK);
    }
    assert(tm_fo_settle(fo, in_flight) == TM_FO_OK);
    assert(tm_fo_submit(fo, &c) == TM_FO_OK);
    assert(tm_fs_retire(store, &c) == TM_FS_EBUSY);
    take_and_finish(fo, store, 0u, 2u);
    take_and_finish(fo, store, 0u, 3u);
    take_and_finish(fo, store, 1u, 3u);
    take_and_finish(fo, store, 2u, 1u);
    take_and_finish(fo, store, 2u, 3u);
    assert(tm_fo_seal(fo) == TM_FO_OK);
    assert(tm_fo_submit(fo, &c) == TM_FO_ESHUTDOWN);
    assert(tm_fo_destroy(fo) == TM_FO_OK);
    assert(tm_fs_seal(store) == TM_FS_OK);
    assert(tm_fs_destroy(store) == TM_FS_OK);
}
static void test_capacity_rollback_and_cancel(void) {
    tm_fs_store *store = NULL;
    tm_fo_owner *fo = NULL;
    tm_fs_config_v1 sc = store_config(2u);
    tm_fo_config_v1 fc = fanout_config();
    tm_fs_frame_ref_v1 f;
    assert(tm_fs_create(&sc, &store) == TM_FS_OK);
    assert(tm_fo_create(store, &fc, &fo) == TM_FO_OK);
    f = frame(store, 1u);
    assert(tm_fo_submit(fo, &f) == TM_FO_EFULL);
    assert(tm_fs_retire(store, &f) == TM_FS_OK);
    assert(tm_fo_seal(fo) == TM_FO_OK);
    assert(tm_fo_destroy(fo) == TM_FO_OK);
    assert(tm_fs_seal(store) == TM_FS_OK);
    assert(tm_fs_destroy(store) == TM_FS_OK);
}
static void test_destroy_requires_terminal(void) {
    tm_fs_store *store = NULL;
    tm_fo_owner *fo = NULL;
    tm_fs_config_v1 sc = store_config(8u);
    tm_fo_config_v1 fc = fanout_config();
    tm_fs_frame_ref_v1 f, taken;
    tm_fo_ticket_v1 ticket = {0};
    assert(tm_fs_create(&sc, &store) == TM_FS_OK);
    assert(tm_fo_create(store, &fc, &fo) == TM_FO_OK);
    f = frame(store, 1u);
    assert(tm_fo_submit(fo, &f) == TM_FO_OK);
    assert(tm_fs_retire(store, &f) == TM_FS_EBUSY);
    assert(tm_fo_take(fo, 0u, &ticket, &taken) == TM_FO_OK);
    assert(tm_fo_seal(fo) == TM_FO_OK);
    assert(tm_fo_destroy(fo) == TM_FO_EBUSY);
    assert(tm_fo_discard_ready(fo, 0u) == TM_FO_OK);
    assert(tm_fo_discard_ready(fo, 1u) == TM_FO_OK);
    assert(tm_fo_discard_ready(fo, 2u) == TM_FO_OK);
    assert(tm_fo_destroy(fo) == TM_FO_EBUSY);
    assert(tm_fo_settle(fo, ticket) == TM_FO_OK);
    assert(tm_fo_destroy(fo) == TM_FO_OK);
    assert(tm_fs_seal(store) == TM_FS_OK);
    assert(tm_fs_destroy(store) == TM_FS_OK);
}
int main(void) {
    test_latest_sample_lossless_and_drain();
    test_capacity_rollback_and_cancel();
    test_destroy_requires_terminal();
    puts("StreamGraph owner-serialized fanout/QoS: PASS");
    return 0;
}
