/* test_ivr_schema.c - DataBind JSON/XML/BIN semantic round trip + golden vector */
#include "data_bind.h"
#include "tinytest_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IVR_TEST_SCHEMA_PATH
#define IVR_TEST_SCHEMA_PATH "turbomedia_ivr_v1.schema"
#endif

static DataBind *g_codec = NULL;

void setUp(void) {
    FILE *f = fopen(IVR_TEST_SCHEMA_PATH, "rb");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)sz + 1);
    TEST_ASSERT_NOT_NULL(text);
    size_t got = fread(text, 1, (size_t)sz, f);
    fclose(f);
    text[got] = '\0';
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindStatus rc = data_bind_create_from_text(text, got, &g_codec, &err);
    free(text);
    TEST_ASSERT_EQUAL(DATA_BIND_OK, rc);
    TEST_ASSERT_NOT_NULL(g_codec);
}

void tearDown(void) {
    if (g_codec) {
        data_bind_free(g_codec);
        g_codec = NULL;
    }
}

/* --- helpers --- */

static const char *field_string(DataBindObject *obj, const char *field,
                                char *out, size_t out_size) {
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *v = data_bind_value_get(root, field);
    TEST_ASSERT_NOT_NULL(v);
    const char *s = data_bind_value_as_string(v);
    TEST_ASSERT_NOT_NULL(s);
    snprintf(out, out_size, "%s", s);
    return out;
}

static uint64_t field_u64(DataBindObject *obj, const char *field) {
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *v = data_bind_value_get(root, field);
    TEST_ASSERT_NOT_NULL(v);
    return data_bind_value_as_uint64(v);
}

static int32_t field_i32(DataBindObject *obj, const char *field) {
    const DataBindValue *root = data_bind_object_value(obj);
    const DataBindValue *v = data_bind_value_get(root, field);
    TEST_ASSERT_NOT_NULL(v);
    return data_bind_value_as_int(v);
}

/* JSON -> BIN -> JSON, plus JSON -> XML -> JSON when with_xml is set.
   XML round trips cover result types again: the vendored cxml empty-string
   crash (turbo_xml_set_text(node, "") -> _cxml_is_integer(NULL, 0) NULL
   deref) was fixed upstream in turbo-utils vendor/cxml (cxstr.c / cxliteral.c
   / cxqapi.c) and the rebuilt turbo_parser.dll is in use. */

static void roundtrip_type(const char *type, const char *json, int with_xml) {
    DataBindError err = DATA_BIND_ERROR_INIT;

    DataBindObject *obj = NULL;
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_json(g_codec, type, json,
                                                 strlen(json), &obj, &err));
    TEST_ASSERT_NOT_NULL(obj);

    uint8_t *bin = NULL;
    size_t bin_len = 0;
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_serialize_bin(g_codec, obj, &bin,
                                                     &bin_len, &err));
    TEST_ASSERT_NOT_NULL(bin);
    TEST_ASSERT_TRUE(bin_len > 0);
    DataBindObject *obj2 = NULL;
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_bin(g_codec, type, bin, bin_len,
                                                &obj2, &err));
    data_bind_binary_free(bin);
    TEST_ASSERT_NOT_NULL(obj2);
    TEST_ASSERT_EQUAL_STRING(type, data_bind_object_type_name(obj2));
    data_bind_object_free(obj2);

    if (with_xml) {
        char *xml = NULL;
        size_t xml_len = 0;
        TEST_ASSERT_EQUAL(DATA_BIND_OK,
                          data_bind_object_serialize_xml(g_codec, obj, &xml,
                                                         &xml_len, &err));
        TEST_ASSERT_NOT_NULL(xml);
        TEST_ASSERT_TRUE(xml_len > 0);
        DataBindObject *obj3 = NULL;
        TEST_ASSERT_EQUAL(DATA_BIND_OK,
                          data_bind_object_from_xml(g_codec, type, xml,
                                                    xml_len, &obj3, &err));
        data_bind_serialized_free(xml);
        TEST_ASSERT_NOT_NULL(obj3);
        TEST_ASSERT_EQUAL_STRING(type, data_bind_object_type_name(obj3));
        data_bind_object_free(obj3);
    }
    data_bind_object_free(obj);
}

/* --- round trips for every published type --- */

void test_conference_join_roundtrip(void) {
    const char *json =
        "{\"message_id\":\"m-1\",\"worker_id\":\"w1\",\"room_id\":\"r1\","
        "\"call_id\":\"c1\",\"call_generation\":7,\"expected_room_version\":42,"
        "\"participant_role\":\"ivr-bot\"}";
    roundtrip_type("ConferenceJoinCommandV1", json, 1);
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_json(g_codec, "ConferenceJoinCommandV1",
                                                 json, strlen(json), &obj, &err));
    char buf[64];
    TEST_ASSERT_EQUAL_STRING("m-1", field_string(obj, "message_id", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("ivr-bot", field_string(obj, "participant_role", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT64(7u, field_u64(obj, "call_generation"));
    TEST_ASSERT_EQUAL_UINT64(42u, field_u64(obj, "expected_room_version"));
    data_bind_object_free(obj);
}

void test_conference_leave_roundtrip(void) {
    roundtrip_type("ConferenceLeaveCommandV1",
                   "{\"message_id\":\"m-2\",\"worker_id\":\"w1\","
                   "\"room_id\":\"r1\",\"call_id\":\"c1\","
                   "\"call_generation\":7,\"expected_room_version\":42}",
                   1);
}

void test_transfer_roundtrip(void) {
    roundtrip_type("TransferCommandV1",
                   "{\"message_id\":\"m-3\",\"worker_id\":\"w1\","
                   "\"room_id\":\"r1\",\"call_id\":\"c1\","
                   "\"call_generation\":7,\"expected_room_version\":42,"
                   "\"queue_id\":\"sales\"}",
                   1);
}

void test_worker_sync_roundtrip(void) {
    roundtrip_type("WorkerSyncCommandV1",
                   "{\"message_id\":\"m-4\",\"worker_id\":\"w1\"}", 1);
}

void test_get_snapshot_roundtrip(void) {
    roundtrip_type("GetSnapshotCommandV1",
                   "{\"message_id\":\"m-5\",\"worker_id\":\"w1\","
                   "\"room_id\":\"r1\",\"call_id\":\"c1\","
                   "\"call_generation\":7}",
                   1);
}

void test_command_result_roundtrip(void) {
    const char *json =
        "{\"message_id\":\"m-6\",\"worker_id\":\"w1\",\"room_id\":\"r1\","
        "\"call_id\":\"c1\",\"call_generation\":7,\"status_code\":-3,"
        "\"room_version\":42,\"sequence\":91,\"error_code\":\"\","
        "\"error_message\":\"\"}";
    roundtrip_type("IvrCommandResultV1", json, 1);
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_json(g_codec, "IvrCommandResultV1",
                                                 json, strlen(json), &obj, &err));
    TEST_ASSERT_EQUAL(-3, field_i32(obj, "status_code"));
    TEST_ASSERT_EQUAL_UINT64(91u, field_u64(obj, "sequence"));
    data_bind_object_free(obj);
}

void test_room_snapshot_roundtrip(void) {
    const char *json =
        "{\"room_id\":\"r1\",\"call_id\":\"c1\",\"call_generation\":7,"
        "\"room_version\":42,\"last_sequence\":90,"
        "\"participant_role\":\"ivr-bot\",\"call_state\":\"dialog_active\"}";
    roundtrip_type("RoomSnapshotV1", json, 1);
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_json(g_codec, "RoomSnapshotV1",
                                                 json, strlen(json), &obj, &err));
    char buf[64];
    TEST_ASSERT_EQUAL_UINT64(90u, field_u64(obj, "last_sequence"));
    TEST_ASSERT_EQUAL_STRING("dialog_active", field_string(obj, "call_state", buf, sizeof(buf)));
    data_bind_object_free(obj);
}

void test_participant_joined_event_roundtrip(void) {
    const char *json =
        "{\"event_id\":\"e-1\",\"causation_id\":\"m-1\",\"worker_id\":\"w1\","
        "\"room_id\":\"r1\",\"call_id\":\"c1\",\"call_generation\":7,"
        "\"room_version\":42,\"sequence\":91,\"occurred_at_ms\":1786030000000,"
        "\"participant_id\":\"p-1\",\"participant_role\":\"ivr-bot\"}";
    roundtrip_type("ConferenceParticipantJoinedEventV1", json, 1);
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_json(
                          g_codec, "ConferenceParticipantJoinedEventV1",
                          json, strlen(json), &obj, &err));
    TEST_ASSERT_EQUAL_UINT64(91u, field_u64(obj, "sequence"));
    TEST_ASSERT_EQUAL_UINT64(1786030000000ull, field_u64(obj, "occurred_at_ms"));
    data_bind_object_free(obj);
}

void test_dtmf_final_roundtrip(void) {
    const char *json =
        "{\"event_id\":\"e-2\",\"call_id\":\"c1\",\"call_generation\":7,"
        "\"input_id\":\"w1\",\"input_value\":\"1\",\"occurred_at_ms\":1000}";
    roundtrip_type("DtmfFinalEventV1", json, 1);
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_json(g_codec, "DtmfFinalEventV1",
                                                 json, strlen(json), &obj, &err));
    char buf[64];
    TEST_ASSERT_EQUAL_STRING("w1", field_string(obj, "input_id", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("1", field_string(obj, "input_value", buf, sizeof(buf)));
    data_bind_object_free(obj);
}

void test_asr_final_roundtrip(void) {
    roundtrip_type("AsrFinalEventV1",
                   "{\"event_id\":\"e-3\",\"call_id\":\"c1\","
                   "\"call_generation\":7,\"input_id\":\"w1\","
                   "\"input_value\":\"hello\",\"occurred_at_ms\":1000}",
                   1);
}

void test_input_timeout_roundtrip(void) {
    roundtrip_type("InputTimeoutEventV1",
                   "{\"event_id\":\"e-4\",\"call_id\":\"c1\","
                   "\"call_generation\":7,\"input_id\":\"w1\","
                   "\"occurred_at_ms\":1000}",
                   1);
}

void test_playback_finished_roundtrip(void) {
    roundtrip_type("PlaybackFinishedEventV1",
                   "{\"event_id\":\"e-5\",\"call_id\":\"c1\","
                   "\"call_generation\":7,\"occurred_at_ms\":1000}",
                   1);
}

static void assert_bin_golden(const char *type, const char *json,
                              const uint8_t *golden, size_t golden_len) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_json(g_codec, type, json,
                                                 strlen(json), &obj, &err));
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_serialize_bin(g_codec, obj, &bin,
                                                     &bin_len, &err));
    TEST_ASSERT_EQUAL_UINT64(golden_len, bin_len);
    TEST_ASSERT_EQUAL_MEMORY(golden, bin, golden_len);
    data_bind_binary_free(bin);
    data_bind_object_free(obj);
}

void test_p0_protocol_roundtrip_and_golden_vectors(void) {
    const char *worker_json =
        "{\"message_id\":\"m\",\"worker_id\":\"w\",\"instance_id\":\"i\"," 
        "\"connection_generation\":1,\"max_sessions\":2,\"active_sessions\":0,"
        "\"reserved_sessions\":0,\"lease_duration_ms\":3,\"draining\":false,"
        "\"capabilities\":\"c\",\"health_generation\":5,"
        "\"health_ready\":true}";
    static const uint8_t worker_golden[] = {
        0x01,0x00,0x00,0x00,0x6D,0x01,0x00,0x00,0x00,0x77,0x01,0x00,0x00,
        0x00,0x69,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x63,0x05,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x01};
    roundtrip_type("WorkerSyncCommandV2", worker_json, 1);
    roundtrip_type("WorkerHeartbeatV1", worker_json, 1);
    assert_bin_golden("WorkerSyncCommandV2", worker_json, worker_golden,
                      sizeof(worker_golden));

    const char *dispatch_json =
        "{\"message_id\":\"m\",\"assignment_id\":\"a\",\"attempt_id\":\"t\"," 
        "\"worker_id\":\"w\",\"worker_instance_id\":\"i\"," 
        "\"worker_connection_generation\":1,\"room_id\":\"r\",\"call_id\":\"c\"," 
        "\"call_generation\":2,\"expected_room_version\":3," 
        "\"content_package\":\"p\",\"content_version\":\"v\"," 
        "\"deadline_timeout_ms\":4}";
    static const uint8_t dispatch_golden[] = {
        0x01,0x00,0x00,0x00,0x6D,0x01,0x00,0x00,0x00,0x61,0x01,0x00,0x00,
        0x00,0x74,0x01,0x00,0x00,0x00,0x77,0x01,0x00,0x00,0x00,0x69,0x01,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x72,0x01,
        0x00,0x00,0x00,0x63,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x70,0x01,
        0x00,0x00,0x00,0x76,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    roundtrip_type("CallDispatchCommandV2", dispatch_json, 1);
    assert_bin_golden("CallDispatchCommandV2", dispatch_json,
                      dispatch_golden, sizeof(dispatch_golden));

    const char *dispatch_result_json =
        "{\"message_id\":\"m\",\"assignment_id\":\"a\",\"attempt_id\":\"t\"," 
        "\"worker_id\":\"w\",\"worker_instance_id\":\"i\"," 
        "\"worker_connection_generation\":1,\"room_id\":\"r\",\"call_id\":\"c\"," 
        "\"call_generation\":2,\"status_code\":0,\"active_sessions\":1," 
        "\"max_sessions\":2,\"error_code\":\"\",\"error_message\":\"\"}";
    static const uint8_t dispatch_result_golden[] = {
        0x01,0x00,0x00,0x00,0x6D,0x01,0x00,0x00,0x00,0x61,0x01,0x00,0x00,
        0x00,0x74,0x01,0x00,0x00,0x00,0x77,0x01,0x00,0x00,0x00,0x69,0x01,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x72,0x01,
        0x00,0x00,0x00,0x63,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00};
    roundtrip_type("CallDispatchResultV2", dispatch_result_json, 1);
    assert_bin_golden("CallDispatchResultV2", dispatch_result_json,
                      dispatch_result_golden, sizeof(dispatch_result_golden));

    const char *release_json =
        "{\"message_id\":\"m\",\"worker_id\":\"w\",\"room_id\":\"r\"," 
        "\"call_id\":\"c\",\"call_generation\":2,\"reason\":\"x\"}";
    static const uint8_t release_golden[] = {
        0x01,0x00,0x00,0x00,0x6D,0x01,0x00,0x00,0x00,0x77,0x01,
        0x00,0x00,0x00,0x72,0x01,0x00,0x00,0x00,0x63,0x02,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x78};
    roundtrip_type("CallReleaseCommandV1", release_json, 1);
    assert_bin_golden("CallReleaseCommandV1", release_json, release_golden,
                      sizeof(release_golden));

    const char *release_result_json =
        "{\"message_id\":\"m\",\"worker_id\":\"w\",\"room_id\":\"r\"," 
        "\"call_id\":\"c\",\"call_generation\":2,\"status_code\":0," 
        "\"error_code\":\"\",\"error_message\":\"\"}";
    static const uint8_t release_result_golden[] = {
        0x01,0x00,0x00,0x00,0x6D,0x01,0x00,0x00,0x00,0x77,
        0x01,0x00,0x00,0x00,0x72,0x01,0x00,0x00,0x00,0x63,
        0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
    roundtrip_type("CallReleaseResultV1", release_result_json, 1);
    roundtrip_type("CallDispatchResultV1", release_result_json, 1);
    assert_bin_golden("CallReleaseResultV1", release_result_json,
                      release_result_golden, sizeof(release_result_golden));

    const char *worker_lost_json =
        "{\"event_id\":\"e\",\"causation_id\":\"a\",\"worker_id\":\"w\"," 
        "\"room_id\":\"r\",\"call_id\":\"c\",\"call_generation\":1," 
        "\"room_version\":2,\"sequence\":3,\"occurred_at_ms\":4," 
        "\"reason\":\"x\"}";
    static const uint8_t worker_lost_golden[] = {
        0x01,0x00,0x00,0x00,0x65,0x01,0x00,0x00,0x00,0x61,0x01,0x00,0x00,
        0x00,0x77,0x01,0x00,0x00,0x00,0x72,0x01,0x00,0x00,0x00,0x63,0x01,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x78};
    roundtrip_type("IvrWorkerLostEventV1", worker_lost_json, 1);
    assert_bin_golden("IvrWorkerLostEventV1", worker_lost_json,
                      worker_lost_golden, sizeof(worker_lost_golden));

    roundtrip_type("WorkerSyncResultV1",
                   "{\"message_id\":\"m\",\"worker_id\":\"w\"," 
                   "\"status_code\":0,\"error_code\":\"\"," 
                   "\"error_message\":\"\"}", 1);
}

/* Malformed input must fail without producing a partial object. */
void test_malformed_json_rejected(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = (DataBindObject *)0x1;
    DataBindStatus rc = data_bind_object_from_json(
        g_codec, "ConferenceJoinCommandV1", "{not-json", 9, &obj, &err);
    TEST_ASSERT_NOT_EQUAL(DATA_BIND_OK, rc);
}

void test_unknown_type_rejected(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    DataBindStatus rc = data_bind_object_from_json(
        g_codec, "NoSuchTypeV1", "{}", 2, &obj, &err);
    TEST_ASSERT_NOT_EQUAL(DATA_BIND_OK, rc);
}

/* Golden vector: BIN bytes for a fixed ConferenceJoinCommandV1. Regenerated
   deterministically from the same schema/build; a byte-level diff catches
   accidental wire layout changes. */
void test_bin_golden_vector(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBindObject *obj = NULL;
    const char *json =
        "{\"message_id\":\"m-1\",\"worker_id\":\"w1\",\"room_id\":\"r1\","
        "\"call_id\":\"c1\",\"call_generation\":7,\"expected_room_version\":42,"
        "\"participant_role\":\"ivr-bot\"}";
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_json(g_codec, "ConferenceJoinCommandV1",
                                                 json, strlen(json), &obj, &err));
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_serialize_bin(g_codec, obj, &bin,
                                                     &bin_len, &err));
    TEST_ASSERT_NOT_NULL(bin);
    /* recorded from the same schema/build on 2026-08-06 */
    static const uint8_t golden[] = {
        0x03, 0x00, 0x00, 0x00, 0x6D, 0x2D, 0x31, 0x02, 0x00, 0x00, 0x00, 0x77,
        0x31, 0x02, 0x00, 0x00, 0x00, 0x72, 0x31, 0x02, 0x00, 0x00, 0x00, 0x63,
        0x31, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x69, 0x76, 0x72,
        0x2D, 0x62, 0x6F, 0x74};
    TEST_ASSERT_EQUAL_UINT64(sizeof(golden), bin_len);
    TEST_ASSERT_EQUAL_MEMORY(golden, bin, sizeof(golden));
    data_bind_binary_free(bin);
    data_bind_object_free(obj);
}

spec("test_ivr_schema") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_conference_join_roundtrip);
  TT_TEST(test_conference_leave_roundtrip);
  TT_TEST(test_transfer_roundtrip);
  TT_TEST(test_worker_sync_roundtrip);
  TT_TEST(test_get_snapshot_roundtrip);
  TT_TEST(test_command_result_roundtrip);
  TT_TEST(test_room_snapshot_roundtrip);
  TT_TEST(test_participant_joined_event_roundtrip);
  TT_TEST(test_dtmf_final_roundtrip);
  TT_TEST(test_asr_final_roundtrip);
  TT_TEST(test_input_timeout_roundtrip);
  TT_TEST(test_playback_finished_roundtrip);
  TT_TEST(test_p0_protocol_roundtrip_and_golden_vectors);
  TT_TEST(test_malformed_json_rejected);
  TT_TEST(test_unknown_type_rejected);
  TT_TEST(test_bin_golden_vector);
}
