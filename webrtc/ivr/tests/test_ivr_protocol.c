/* test_ivr_protocol.c - shared-schema TIVR BIN/TEXT adapter */
#include "ivr_protocol.h"
#include "tinytest_compat.h"
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

    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_END));
    size = ftell(file);
    TEST_ASSERT_TRUE(size >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET));
    schema = (char *)malloc((size_t)size + 1);
    TEST_ASSERT_NOT_NULL(schema);
    read_size = fread(schema, 1, (size_t)size, file);
    TEST_ASSERT_EQUAL_UINT64((size_t)size, read_size);
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    schema[read_size] = '\0';
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_create_from_text(schema, read_size, &g_codec,
                                                 &error));
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
    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_json(
                          g_codec, "ConferenceJoinCommandV1", json,
                          sizeof(json) - 1, &object, &error));
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

    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_protocol_encode(g_codec, &info, source, frame,
                                          sizeof(frame), &frame_len));
    TEST_ASSERT_TRUE(frame_len > IVR_FRAME_HEADER_SIZE);
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_protocol_decode(g_codec, frame, frame_len, &decoded,
                                          &decoded_info));
    TEST_ASSERT_EQUAL_STRING("ConferenceJoinCommandV1",
                             data_bind_object_type_name(decoded));
    TEST_ASSERT_EQUAL_INT(format, decoded_info.format);
    TEST_ASSERT_EQUAL_INT(IVR_KIND_COMMAND, decoded_info.kind);
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

    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_protocol_encode(g_codec, &info, source, frame,
                                          sizeof(frame), &frame_len));
    TEST_ASSERT_EQUAL_INT('{', frame[IVR_FRAME_HEADER_SIZE]);
    TEST_ASSERT_NULL(memchr(frame + IVR_FRAME_HEADER_SIZE, '\n',
                            frame_len - IVR_FRAME_HEADER_SIZE));
    data_bind_object_free(source);
}

void test_malformed_text_rejected(void) {
    uint8_t frame[IVR_FRAME_HEADER_SIZE + 1];
    ivr_frame_info_t info = join_info(IVR_FMT_TEXT);
    DataBindObject *decoded = (DataBindObject *)1;

    TEST_ASSERT_EQUAL(IVR_OK, ivr_frame_encode(frame, &info));
    frame[IVR_FRAME_HEADER_SIZE] = '{';
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_protocol_decode(g_codec, frame, sizeof(frame),
                                          &decoded, NULL));
    TEST_ASSERT_NULL(decoded);
}

void test_object_type_mismatch_rejected(void) {
    static const char json[] =
        "{\"message_id\":\"sync-1\",\"worker_id\":\"w1\"}";
    uint8_t frame[256];
    size_t frame_len = 99;
    DataBindError error = DATA_BIND_ERROR_INIT;
    DataBindObject *object = NULL;
    ivr_frame_info_t info = join_info(IVR_FMT_BIN);

    TEST_ASSERT_EQUAL(DATA_BIND_OK,
                      data_bind_object_from_json(
                          g_codec, "WorkerSyncCommandV1", json,
                          sizeof(json) - 1, &object, &error));
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_protocol_encode(g_codec, &info, object, frame,
                                          sizeof(frame), &frame_len));
    TEST_ASSERT_EQUAL_UINT64(0, frame_len);
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
    TEST_ASSERT_EQUAL(IVR_ENOSPC,
                      ivr_protocol_encode(g_codec, &info, source, frame,
                                          sizeof(frame), &frame_len));
    TEST_ASSERT_EQUAL_UINT64(0, frame_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, frame, sizeof(frame));
    data_bind_object_free(source);
}

spec("test_ivr_protocol") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  TT_TEST(test_bin_roundtrip);
  TT_TEST(test_text_roundtrip);
  TT_TEST(test_text_is_compact_json);
  TT_TEST(test_malformed_text_rejected);
  TT_TEST(test_object_type_mismatch_rejected);
  TT_TEST(test_short_output_is_unchanged);
}
