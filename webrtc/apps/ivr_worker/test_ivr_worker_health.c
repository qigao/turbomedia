#include "ivr_worker_health.h"
#include "tinytest_compat.h"
#include <stdio.h>
#include <string.h>

void test_health_snapshot_and_json(void) {
    ivr_worker_health_t health;
    ivr_worker_health_snapshot_t value;
    char json[1024];
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_init(&health, 4));
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_snapshot(&health, &value));
    TEST_ASSERT_FALSE(value.ready);
    TEST_ASSERT_EQUAL_UINT32(4u, value.max_sessions);
    value.ready = 1;
    value.content_ready = 1;
    value.schema_ready = 1;
    value.command_channel_ready = 1;
    value.event_channel_ready = 1;
    value.sync_ready = 1;
    value.speech_ready = 1;
    value.sfu_ready = 1;
    snprintf(value.capabilities, sizeof(value.capabilities), "health.ready");
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_update(&health, &value));
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_snapshot(&health, &value));
    TEST_ASSERT_TRUE(value.ready);
    TEST_ASSERT_EQUAL_UINT64(2u, value.generation);
    TEST_ASSERT_TRUE(ivr_worker_health_json(&health, json, sizeof(json)) > 0);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"ready\":true"));
    ivr_worker_health_destroy(&health);
}

void test_health_derives_readiness_and_rejects_stale_updates(void) {
    ivr_worker_health_t health;
    ivr_worker_health_snapshot_t value;
    ivr_worker_health_snapshot_t stale;
    int *dependencies[7];
    size_t i;

    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_init(&health, 2));
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_snapshot(&health, &value));
    value.ready = 1;
    value.content_ready = 1;
    value.schema_ready = 1;
    value.command_channel_ready = 1;
    value.event_channel_ready = 1;
    value.sync_ready = 1;
    value.speech_ready = 1;
    value.sfu_ready = 1;
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_update(&health, &value));
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_snapshot(&health, &value));
    TEST_ASSERT_TRUE(value.ready);
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
        TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_update(&health, &value));
        TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_snapshot(&health, &value));
        TEST_ASSERT_FALSE(value.ready);
        *dependencies[i] = 1;
        value.ready = 1;
        TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_update(&health, &value));
        TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_snapshot(&health, &value));
        TEST_ASSERT_TRUE(value.ready);
    }

    value.active_sessions = value.max_sessions;
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_update(&health, &value));
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_snapshot(&health, &value));
    TEST_ASSERT_FALSE(value.ready);
    TEST_ASSERT_EQUAL_INT(-1, ivr_worker_health_update(&health, &stale));
    ivr_worker_health_destroy(&health);
}

void test_health_rejects_invalid_capacity(void) {
    ivr_worker_health_t health;
    ivr_worker_health_snapshot_t value;
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_init(&health, 2));
    TEST_ASSERT_EQUAL_INT(0, ivr_worker_health_snapshot(&health, &value));
    value.active_sessions = 3;
    TEST_ASSERT_EQUAL_INT(-1, ivr_worker_health_update(&health, &value));
    ivr_worker_health_destroy(&health);
}

spec("test_ivr_worker_health") {
  TT_TEST(test_health_snapshot_and_json);
  TT_TEST(test_health_rejects_invalid_capacity);
  TT_TEST(test_health_derives_readiness_and_rejects_stale_updates);
}
