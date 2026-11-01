#include <turbo_frame_store.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static tm_fs_config_v1 config(void) {
    tm_fs_config_v1 c = TM_FS_CONFIG_V1_INIT;
    c.store_id = 987u;
    c.slot_capacity = 2u;
    c.max_frame_bytes = 32u;
    c.max_pins = 4u;
    c.max_leases = 4u;
    c.max_retained_bytes = 64u;
    return c;
}

static void test_pin_retire_and_aba(void) {
    tm_fs_config_v1 c = config();
    tm_fs_store *s = NULL, *reopened = NULL;
    tm_fs_frame_ref_v1 old = TM_FS_FRAME_REF_V1_INIT;
    tm_fs_frame_ref_v1 newer = TM_FS_FRAME_REF_V1_INIT;
    tm_fs_frame_ref_v1 after = TM_FS_FRAME_REF_V1_INIT;
    tm_fs_pin_v1 p1 = {0}, p2 = {0};
    tm_fs_lease_v1 lease1 = {0}, lease2 = {0};
    tm_fs_cpu_view_v1 view = TM_FS_CPU_VIEW_V1_INIT;
    const char input[] = "live-frame";
    assert(tm_fs_create(&c, &s) == TM_FS_OK);
    assert(tm_fs_publish_cpu(s, input, sizeof(input), 5u, 7u, &old) == TM_FS_OK);
    assert(old.store_generation != 0u && old.slot_generation == 1u);
    assert(tm_fs_reserve_pin(s, &old, &p1) == TM_FS_OK);
    assert(tm_fs_reserve_pin(s, &old, &p2) == TM_FS_OK);
    assert(p1.ticket_id && p2.ticket_id && p1.ticket_id != p2.ticket_id);
    assert(tm_fs_acquire_pinned(s, &p1, &lease1) == TM_FS_OK);
    assert(tm_fs_retire(s, &old) == TM_FS_EBUSY);
    assert(tm_fs_reserve_pin(s, &old, &(tm_fs_pin_v1){0}) == TM_FS_EBUSY);
    assert(tm_fs_acquire_pinned(s, &p2, &lease2) == TM_FS_OK);
    assert(tm_fs_map_cpu(s, &lease2, &view) == TM_FS_OK);
    assert(view.size_bytes == sizeof(input) && memcmp(view.data, input, sizeof(input)) == 0);
    assert(tm_fs_release_pin(s, &p1) == TM_FS_OK);
    assert(tm_fs_release_pin(s, &p1) == TM_FS_ESTALE);
    assert(tm_fs_release_lease(s, &lease1) == TM_FS_OK);
    assert(tm_fs_release_pin(s, &p2) == TM_FS_OK);
    assert(tm_fs_acquire_pinned(s, &p2, &(tm_fs_lease_v1){0}) == TM_FS_ESTALE);
    assert(tm_fs_release_lease(s, &lease2) == TM_FS_OK);
    assert(tm_fs_acquire_weak(s, &old, &lease1) == TM_FS_ESTALE);
    assert(tm_fs_publish_cpu(s, input, sizeof(input), 5u, 7u, &newer) == TM_FS_OK);
    assert(tm_fs_retire(s, &newer) == TM_FS_OK);
    assert(tm_fs_publish_cpu(s, input, sizeof(input), 5u, 7u, &after) == TM_FS_OK);
    if (old.slot == after.slot) assert(after.slot_generation > old.slot_generation);
    assert(tm_fs_reserve_pin(s, &old, &p1) == TM_FS_ESTALE);
    assert(tm_fs_seal(s) == TM_FS_OK);
    assert(tm_fs_publish_cpu(s, input, sizeof(input), 5u, 7u, &newer) == TM_FS_ESHUTDOWN);
    assert(tm_fs_destroy(s) == TM_FS_OK);
    assert(tm_fs_create(&c, &reopened) == TM_FS_OK);
    assert(tm_fs_publish_cpu(reopened, input, sizeof(input), 5u, 7u, &newer) == TM_FS_OK);
    assert(newer.store_generation != old.store_generation);
    assert(tm_fs_acquire_weak(reopened, &old, &lease1) == TM_FS_ESTALE);
    assert(tm_fs_seal(reopened) == TM_FS_OK);
    assert(tm_fs_destroy(reopened) == TM_FS_OK);
}

static void test_quota_and_destroy(void) {
    tm_fs_config_v1 c = config();
    tm_fs_store *s = NULL;
    tm_fs_frame_ref_v1 f1 = TM_FS_FRAME_REF_V1_INIT, f2 = TM_FS_FRAME_REF_V1_INIT, reject = TM_FS_FRAME_REF_V1_INIT;
    tm_fs_pin_v1 p = {0};
    tm_fs_lease_v1 lease = {0};
    const uint8_t payload[] = {0, 1, 2, 3, 4};
    assert(tm_fs_create(&c, &s) == TM_FS_OK);
    assert(tm_fs_publish_cpu(s, payload, sizeof(payload), 1u, 1u, &f1) == TM_FS_OK);
    assert(tm_fs_publish_cpu(s, payload, sizeof(payload), 1u, 1u, &f2) == TM_FS_OK);
    assert(tm_fs_publish_cpu(s, payload, sizeof(payload), 1u, 1u, &reject) == TM_FS_EFULL);
    assert(tm_fs_reserve_pin(s, &f1, &p) == TM_FS_OK);
    assert(tm_fs_acquire_pinned(s, &p, &lease) == TM_FS_OK);
    assert(tm_fs_seal(s) == TM_FS_OK);
    assert(tm_fs_destroy(s) == TM_FS_EBUSY);
    assert(tm_fs_retire(s, &f1) == TM_FS_EBUSY);
    assert(tm_fs_release_pin(s, &p) == TM_FS_OK);
    assert(tm_fs_destroy(s) == TM_FS_EBUSY);
    assert(tm_fs_release_lease(s, &lease) == TM_FS_OK);
    assert(tm_fs_destroy(s) == TM_FS_OK);
}

static void test_m17_frame_metadata_slot_and_aba(void) {
    tm_fs_store *s=NULL;
    tm_fs_config_v1 cfg=TM_FS_CONFIG_V1_INIT;
    tm_fs_frame_ref_v1 ref=TM_FS_FRAME_REF_V1_INIT;
    tm_fs_frame_ref_v1 next=TM_FS_FRAME_REF_V1_INIT;
    tm_fs_frame_metadata_v1 m=TM_FS_FRAME_METADATA_V1_INIT;
    tm_fs_frame_metadata_v1 read=TM_FS_FRAME_METADATA_V1_INIT;
    uint8_t data[4]={1u,2u,3u,4u};
    cfg.store_id=871u;cfg.slot_capacity=1u;cfg.max_frame_bytes=16u;
    cfg.max_pins=2u;cfg.max_leases=2u;cfg.max_retained_bytes=16u;
    assert(tm_fs_create(&cfg,&s)==TM_FS_OK);
    m.width=2u;m.height=2u;m.format_id=1u;
    m.device_session_id=12u;m.session_generation=2u;m.display_id=7u;
    m.source_session_generation=91u;m.display_generation=3u;
    m.geometry_generation=4u;m.capture_session_hi=123u;
    m.stream_generation=8u;m.packet_sequence=11u;
    m.pts=33000u;m.dts=33000u;m.timebase_num=1u;m.timebase_den=1000000u;
    assert(tm_fs_publish_cpu_with_metadata(s,data,sizeof(data),91u,3u,
                                            &m,&ref)==TM_FS_OK);
    assert(tm_fs_get_metadata(s,&ref,&read)==TM_FS_OK);
    assert(read.capture_session_hi==123u && read.packet_sequence==11u &&
           read.pts==33000u && read.geometry_generation==4u);
    m.packet_sequence=12u;
    assert(tm_fs_get_metadata(s,&ref,&read)==TM_FS_OK &&
           read.packet_sequence==11u); /* deep copied, not caller lifetime */
    assert(tm_fs_publish_cpu_with_metadata(s,data,sizeof(data),91u,3u,
                                            &m,&next)==TM_FS_EFULL);
    assert(tm_fs_retire(s,&ref)==TM_FS_OK);
    assert(tm_fs_get_metadata(s,&ref,&read)==TM_FS_ESTALE);
    assert(tm_fs_publish_cpu(s,data,sizeof(data),91u,3u,&next)==TM_FS_OK);
    assert(tm_fs_get_metadata(s,&next,&read)==TM_FS_ENOMETA);
    assert(tm_fs_get_metadata(s,&ref,&read)==TM_FS_ESTALE);
    assert(tm_fs_retire(s,&next)==TM_FS_OK);
    m.source_session_generation=1u;
    assert(tm_fs_publish_cpu_with_metadata(s,data,sizeof(data),91u,3u,
                                            &m,&next)==TM_FS_EINVAL);
    m.source_session_generation=91u;m.version=99u;
    assert(tm_fs_publish_cpu_with_metadata(s,data,sizeof(data),91u,3u,
                                            &m,&next)==TM_FS_EVERSION);
    assert(tm_fs_seal(s)==TM_FS_OK);
    assert(tm_fs_destroy(s)==TM_FS_OK);
}

int main(void) {
    test_m17_frame_metadata_slot_and_aba();
    test_pin_retire_and_aba();
    test_quota_and_destroy();
    puts("FrameStore CPU owner contract tests: PASS");
    return 0;
}
