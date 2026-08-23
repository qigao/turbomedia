#include "ivr_worker_health.h"
#include "tinytest.h"
#include <stdio.h>
#include <string.h>

void test_health_snapshot_and_json(void) {
    ivr_worker_health_t health;
    ivr_worker_health_snapshot_t value;
    char json[1024];
    check_equal((int)(ivr_worker_health_init(&health, 4)), (int)(0));
    check_equal((int)(ivr_worker_health_snapshot(&health, &value)), (int)(0));
    check_false(value.ready);
    check_equal((uint32_t)(value.max_sessions), (uint32_t)(4u));
    value.ready = 1;
    value.content_ready = 1;
    value.schema_ready = 1;
    value.command_channel_ready = 1;
    value.event_channel_ready = 1;
    value.sync_ready = 1;
    value.speech_ready = 1;
    value.sfu_ready = 1;
    snprintf(value.capabilities, sizeof(value.capabilities), "health.ready");
    check_equal((int)(ivr_worker_health_update(&health, &value)), (int)(0));
    check_equal((int)(ivr_worker_health_snapshot(&health, &value)), (int)(0));
    check_true(value.ready);
    check_equal((uint64_t)(value.generation), (uint64_t)(2u));
    check_true(ivr_worker_health_json(&health, json, sizeof(json)) > 0);
    check_not_null(strstr(json, "\"ready\":true"));
    ivr_worker_health_destroy(&health);
}

void test_health_derives_readiness_and_rejects_stale_updates(void) {
    ivr_worker_health_t health;
    ivr_worker_health_snapshot_t value;
    ivr_worker_health_snapshot_t stale;
    int *dependencies[7];
    size_t i;

    check_equal((int)(ivr_worker_health_init(&health, 2)), (int)(0));
    check_equal((int)(ivr_worker_health_snapshot(&health, &value)), (int)(0));
    value.ready = 1;
    value.content_ready = 1;
    value.schema_ready = 1;
    value.command_channel_ready = 1;
    value.event_channel_ready = 1;
    value.sync_ready = 1;
    value.speech_ready = 1;
    value.sfu_ready = 1;
    check_equal((int)(ivr_worker_health_update(&health, &value)), (int)(0));
    check_equal((int)(ivr_worker_health_snapshot(&health, &value)), (int)(0));
    check_true(value.ready);
    stale = value;

    dependencies[0] = &value.content_ready;
    dependencies[1] = &value.schema_ready;
    dependencies[2] = &value.command_channel_ready;
    dependencies[3] = &value.event_channel_ready;
    dependencies[4] = &value.sync_ready;
    dependencies[5] = &value.speech_ready;
    dependencies[6] = &value.sfu_ready;
    for (i = 0; i < sizeof(dependencies) / sizeof(dependencies[0]); ++i) {
        *dependencies[i] = 0;
        check_equal((int)(ivr_worker_health_update(&health, &value)), (int)(0));
        check_equal((int)(ivr_worker_health_snapshot(&health, &value)), (int)(0));
        check_false(value.ready);
        *dependencies[i] = 1;
        value.ready = 1;
        check_equal((int)(ivr_worker_health_update(&health, &value)), (int)(0));
        check_equal((int)(ivr_worker_health_snapshot(&health, &value)), (int)(0));
        check_true(value.ready);
    }

    value.active_sessions = value.max_sessions;
    check_equal((int)(ivr_worker_health_update(&health, &value)), (int)(0));
    check_equal((int)(ivr_worker_health_snapshot(&health, &value)), (int)(0));
    check_false(value.ready);
    check_equal((int)(ivr_worker_health_update(&health, &stale)), (int)(-1));
    ivr_worker_health_destroy(&health);
}

void test_health_rejects_invalid_capacity(void) {
    ivr_worker_health_t health;
    ivr_worker_health_snapshot_t value;
    check_equal((int)(ivr_worker_health_init(&health, 2)), (int)(0));
    check_equal((int)(ivr_worker_health_snapshot(&health, &value)), (int)(0));
    value.active_sessions = 3;
    check_equal((int)(ivr_worker_health_update(&health, &value)), (int)(-1));
    ivr_worker_health_destroy(&health);
}

spec("test_ivr_worker_health") {
  it("test_health_snapshot_and_json") { test_health_snapshot_and_json(); };
  it("test_health_rejects_invalid_capacity") { test_health_rejects_invalid_capacity(); };
  it("test_health_derives_readiness_and_rejects_stale_updates") { test_health_derives_readiness_and_rejects_stale_updates(); };
}
