/* test_ivr_frame.c - TIVR 12-byte frame header validation */
#include "ivr_frame.h"
#include "tinytest_compat.h"
#include <string.h>

void test_encode_decode_roundtrip(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE];
    ivr_frame_info_t info = {0};
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_EVENT;
    info.schema_type_id = IVR_TYPE_CONFERENCE_PARTICIPANT_JOINED_EVENT_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_frame_encode(buf, &info));

    ivr_frame_info_t out = {0};
    TEST_ASSERT_EQUAL(IVR_OK, ivr_frame_decode(buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_INT(IVR_FMT_BIN, out.format);
    TEST_ASSERT_EQUAL_INT(IVR_KIND_EVENT, out.kind);
    TEST_ASSERT_EQUAL_UINT32(IVR_TYPE_CONFERENCE_PARTICIPANT_JOINED_EVENT_V1,
                             out.schema_type_id);
    TEST_ASSERT_EQUAL_INT(IVR_SCHEMA_MAJOR, out.schema_major);
    TEST_ASSERT_EQUAL_INT(IVR_SCHEMA_MINOR, out.schema_minor);
}

void test_magic_bytes(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE] = {0};
    ivr_frame_info_t info = {0};
    info.format = IVR_FMT_TEXT;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    ivr_frame_encode(buf, &info);
    buf[0] = 'X'; /* corrupt magic */
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_frame_decode(buf, sizeof(buf), NULL));
    buf[0] = 'T';
    TEST_ASSERT_EQUAL(IVR_OK, ivr_frame_decode(buf, sizeof(buf), NULL));
}

void test_short_frame(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE - 1] = {0};
    TEST_ASSERT_EQUAL(IVR_EINVAL, ivr_frame_decode(buf, sizeof(buf), NULL));
    TEST_ASSERT_EQUAL(IVR_EINVAL, ivr_frame_decode(NULL, 12, NULL));
}

void test_unknown_frame_version(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE];
    ivr_frame_info_t info = {0};
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_TRANSFER_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    ivr_frame_encode(buf, &info);
    buf[4] = 99; /* unknown frame version */
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_frame_decode(buf, sizeof(buf), NULL));
}

void test_unknown_format_kind_flags(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE];
    ivr_frame_info_t info = {0};
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_WORKER_SYNC_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    ivr_frame_encode(buf, &info);

    buf[5] = 9; /* unknown format */
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_frame_decode(buf, sizeof(buf), NULL));
    buf[5] = 3; /* XML is not a TIVR wire format */
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_frame_decode(buf, sizeof(buf), NULL));
    buf[5] = IVR_FMT_BIN;

    buf[6] = 9; /* unknown kind */
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_frame_decode(buf, sizeof(buf), NULL));
    buf[6] = IVR_KIND_COMMAND;

    buf[7] = 1; /* nonzero reserved flags */
    TEST_ASSERT_EQUAL(IVR_ESTATE, ivr_frame_decode(buf, sizeof(buf), NULL));
    buf[7] = 0;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_frame_decode(buf, sizeof(buf), NULL));
}

void test_unknown_type_id(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE];
    ivr_frame_info_t info = {0};
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_frame_encode(buf, &info));
    buf[8] = 0xEE; /* unknown type id low byte */
    buf[9] = 0x7F;
    TEST_ASSERT_EQUAL(IVR_EINVAL, ivr_frame_decode(buf, sizeof(buf), NULL));
}

void test_unknown_schema_version(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE];
    ivr_frame_info_t info = {0};
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_GET_SNAPSHOT_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    ivr_frame_encode(buf, &info);
    buf[10] = 2; /* unknown major */
    TEST_ASSERT_EQUAL(IVR_EVERSION, ivr_frame_decode(buf, sizeof(buf), NULL));
    buf[10] = IVR_SCHEMA_MAJOR;
    buf[11] = 1; /* unknown minor */
    TEST_ASSERT_EQUAL(IVR_EVERSION, ivr_frame_decode(buf, sizeof(buf), NULL));
}

void test_type_registry(void) {
    TEST_ASSERT_NOT_NULL(ivr_frame_type_name(IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1));
    TEST_ASSERT_EQUAL_STRING("ConferenceJoinCommandV1",
                             ivr_frame_type_name(IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1));
    TEST_ASSERT_NULL(ivr_frame_type_name(9999));
    uint16_t id = 0;
    TEST_ASSERT_EQUAL(IVR_OK,
                      ivr_frame_type_id_by_name("ConferenceJoinCommandV1", &id));
    TEST_ASSERT_EQUAL_UINT32(IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1, id);
    uint8_t kind = 0;
    TEST_ASSERT_EQUAL(IVR_OK, ivr_frame_type_kind(id, &kind));
    TEST_ASSERT_EQUAL_INT(IVR_KIND_COMMAND, kind);
    TEST_ASSERT_EQUAL(IVR_EINVAL, ivr_frame_type_kind(9999, &kind));
    TEST_ASSERT_EQUAL(IVR_EINVAL,
                      ivr_frame_type_id_by_name("NoSuchTypeV1", &id));
}

void test_type_kind_mismatch(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE];
    ivr_frame_info_t info = {0};
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_EVENT;
    info.schema_type_id = IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    TEST_ASSERT_EQUAL(IVR_EINVAL, ivr_frame_encode(buf, &info));
}

void test_encode_invalid(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE];
    ivr_frame_info_t info = {0};
    info.format = 9; /* invalid */
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_WORKER_SYNC_RESULT_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    TEST_ASSERT_EQUAL(IVR_EINVAL, ivr_frame_encode(buf, &info));
    info.format = IVR_FMT_BIN;
    info.schema_major = 9; /* invalid schema version */
    TEST_ASSERT_EQUAL(IVR_EVERSION, ivr_frame_encode(buf, &info));
    TEST_ASSERT_EQUAL(IVR_EINVAL, ivr_frame_encode(NULL, &info));
}

spec("test_ivr_frame") {
  TT_TEST(test_encode_decode_roundtrip);
  TT_TEST(test_magic_bytes);
  TT_TEST(test_short_frame);
  TT_TEST(test_unknown_frame_version);
  TT_TEST(test_unknown_format_kind_flags);
  TT_TEST(test_unknown_type_id);
  TT_TEST(test_unknown_schema_version);
  TT_TEST(test_type_registry);
  TT_TEST(test_type_kind_mismatch);
  TT_TEST(test_encode_invalid);
}
