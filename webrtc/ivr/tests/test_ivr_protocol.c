/* test_ivr_protocol.c - shared-schema TIVR BIN/TEXT adapter */
#include "ivr_protocol.h"
#include "tinytest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IVR_TEST_SCHEMA_PATH
#define IVR_TEST_SCHEMA_PATH "turbomedia_ivr_v1.schema"
#endif

static DataBind *g_codec;

void setUp(void) {
    FILE *file = fopen(IVR_TEST_SCHEMA_PATH, "rb");
    char *schema = NULL;
    long size;
    size_t read_size;
    DataBindError error = DATA_BIND_ERROR_INIT;

    check_not_null(file);
    check_equal((int)(fseek(file, 0, SEEK_END)), (int)(0));
    size = ftell(file);
    check_true(size >= 0);
    check_equal((int)(fseek(file, 0, SEEK_SET)), (int)(0));
    schema = (char *)malloc((size_t)size + 1);
    check_not_null(schema);
    read_size = fread(schema, 1, (size_t)size, file);
    check_equal((uint64_t)(read_size), (uint64_t)((size_t)size));
    check_equal((int)(fclose(file)), (int)(0));
    schema[read_size] = '\0';
    check_equal(data_bind_create_from_text(schema, read_size, &g_codec,
                                                 &error), DATA_BIND_OK);
    free(schema);
}

void tearDown(void) {
    data_bind_free(g_codec);
    g_codec = NULL;
}

static DataBindObject *make_join(void) {
    static const char json[] =
        "{\"message_id\":\"m-1\",\"worker_id\":\"w1\",\"room_id\":\"r1\","
        "\"call_id\":\"c1\",\"call_generation\":7,"
        "\"expected_room_version\":42,\"participant_role\":\"ivr-bot\"}";
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    check_equal(data_bind_object_from_json(
                          g_codec, "ConferenceJoinCommandV1", json,
                          sizeof(json) - 1, &object, &error), DATA_BIND_OK);
    return object;
}

static ivr_frame_info_t join_info(uint8_t format) {
    ivr_frame_info_t info = {0};
    info.format = format;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    return info;
}

static void assert_roundtrip(uint8_t format) {
    uint8_t frame[1024];
    size_t frame_len = 0;
    DataBindObject *source = make_join();
    DataBindObject *decoded = NULL;
    ivr_frame_info_t info = join_info(format);
    ivr_frame_info_t decoded_info = {0};

    check_equal(ivr_protocol_encode(g_codec, &info, source, frame,
                                          sizeof(frame), &frame_len), IVR_OK);
    check_true(frame_len > IVR_FRAME_HEADER_SIZE);
    check_equal(ivr_protocol_decode(g_codec, frame, frame_len, &decoded,
                                          &decoded_info), IVR_OK);
    check_equal(data_bind_object_type_name(decoded), "ConferenceJoinCommandV1");
    check_equal((int)(decoded_info.format), (int)(format));
    check_equal((int)(decoded_info.kind), (int)(IVR_KIND_COMMAND));
    data_bind_object_free(decoded);
    data_bind_object_free(source);
}

void test_bin_roundtrip(void) { assert_roundtrip(IVR_FMT_BIN); }

void test_text_roundtrip(void) { assert_roundtrip(IVR_FMT_TEXT); }

void test_text_is_compact_json(void) {
    uint8_t frame[1024];
    size_t frame_len = 0;
    DataBindObject *source = make_join();
    ivr_frame_info_t info = join_info(IVR_FMT_TEXT);

    check_equal(ivr_protocol_encode(g_codec, &info, source, frame,
                                          sizeof(frame), &frame_len), IVR_OK);
    check_equal((int)(frame[IVR_FRAME_HEADER_SIZE]), (int)('{'));
    check_null(memchr(frame + IVR_FRAME_HEADER_SIZE, '\n',
                            frame_len - IVR_FRAME_HEADER_SIZE));
    data_bind_object_free(source);
}

void test_malformed_text_rejected(void) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 1];
    ivr_frame_info_t info = join_info(IVR_FMT_TEXT);
    DataBindObject *decoded = (DataBindObject *)1;

    check_equal(ivr_frame_encode(frame, &info), IVR_OK);
    frame[IVR_FRAME_HEADER_SIZE] = '{';
    check_equal(ivr_protocol_decode(g_codec, frame, sizeof(frame),
                                          &decoded, NULL), IVR_EINVAL);
    check_null(decoded);
}

void test_object_type_mismatch_rejected(void) {
    static const char json[] =
        "{\"message_id\":\"sync-1\",\"worker_id\":\"w1\"}";
    uint8_t frame[256];
    size_t frame_len = 99;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    ivr_frame_info_t info = join_info(IVR_FMT_BIN);

    check_equal(data_bind_object_from_json(
                          g_codec, "WorkerSyncCommandV1", json,
                          sizeof(json) - 1, &object, &error), DATA_BIND_OK);
    check_equal(ivr_protocol_encode(g_codec, &info, object, frame,
                                          sizeof(frame), &frame_len), IVR_EINVAL);
    check_equal((uint64_t)(frame_len), (uint64_t)(0));
    data_bind_object_free(object);
}

void test_short_output_is_unchanged(void) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 4];
    uint8_t expected[sizeof(frame)];
    size_t frame_len = 99;
    DataBindObject *source = make_join();
    ivr_frame_info_t info = join_info(IVR_FMT_BIN);

    memset(frame, 0xa5, sizeof(frame));
    memcpy(expected, frame, sizeof(frame));
    check_equal(ivr_protocol_encode(g_codec, &info, source, frame,
                                          sizeof(frame), &frame_len), IVR_ENOSPC);
    check_equal((uint64_t)(frame_len), (uint64_t)(0));
    check_equal(frame, expected, sizeof(frame));
    data_bind_object_free(source);
}

spec("test_ivr_protocol") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  it("test_bin_roundtrip") { test_bin_roundtrip(); };
  it("test_text_roundtrip") { test_text_roundtrip(); };
  it("test_text_is_compact_json") { test_text_is_compact_json(); };
  it("test_malformed_text_rejected") { test_malformed_text_rejected(); };
  it("test_object_type_mismatch_rejected") { test_object_type_mismatch_rejected(); };
  it("test_short_output_is_unchanged") { test_short_output_is_unchanged(); };
}
