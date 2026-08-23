/* test_ivr_frame.c - TIVR 12-byte frame header validation */
#include "ivr_frame.h"
#include "tinytest.h"
#include <string.h>

void test_encode_decode_roundtrip(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE];
    ivr_frame_info_t info = {0};
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_EVENT;
    info.schema_type_id = IVR_TYPE_CONFERENCE_PARTICIPANT_JOINED_EVENT_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    check_equal(ivr_frame_encode(buf, &info), IVR_OK);

    ivr_frame_info_t out = {0};
    check_equal(ivr_frame_decode(buf, sizeof(buf), &out), IVR_OK);
    check_equal((int)(out.format), (int)(IVR_FMT_BIN));
    check_equal((int)(out.kind), (int)(IVR_KIND_EVENT));
    check_equal((uint32_t)(out.schema_type_id), (uint32_t)(IVR_TYPE_CONFERENCE_PARTICIPANT_JOINED_EVENT_V1));
    check_equal((int)(out.schema_major), (int)(IVR_SCHEMA_MAJOR));
    check_equal((int)(out.schema_minor), (int)(IVR_SCHEMA_MINOR));
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
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_ESTATE);
    buf[0] = 'T';
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_OK);
}

void test_short_frame(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE - 1] = {0};
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_EINVAL);
    check_equal(ivr_frame_decode(NULL, 12, NULL), IVR_EINVAL);
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
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_ESTATE);
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
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_ESTATE);
    buf[5] = 3; /* XML is not a TIVR wire format */
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_ESTATE);
    buf[5] = IVR_FMT_BIN;

    buf[6] = 9; /* unknown kind */
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_ESTATE);
    buf[6] = IVR_KIND_COMMAND;

    buf[7] = 1; /* nonzero reserved flags */
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_ESTATE);
    buf[7] = 0;
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_OK);
}

void test_unknown_type_id(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE];
    ivr_frame_info_t info = {0};
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    check_equal(ivr_frame_encode(buf, &info), IVR_OK);
    buf[8] = 0xEE; /* unknown type id low byte */
    buf[9] = 0x7F;
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_EINVAL);
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
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_EVERSION);
    buf[10] = IVR_SCHEMA_MAJOR;
    buf[11] = 1; /* unknown minor */
    check_equal(ivr_frame_decode(buf, sizeof(buf), NULL), IVR_EVERSION);
}

void test_type_registry(void) {
    check_not_null(ivr_frame_type_name(IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1));
    check_equal(ivr_frame_type_name(IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1), "ConferenceJoinCommandV1");
    check_null(ivr_frame_type_name(9999));
    uint16_t id = 0;
    check_equal(ivr_frame_type_id_by_name("ConferenceJoinCommandV1", &id), IVR_OK);
    check_equal((uint32_t)(id), (uint32_t)(IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1));
    uint8_t kind = 0;
    check_equal(ivr_frame_type_kind(id, &kind), IVR_OK);
    check_equal((int)(kind), (int)(IVR_KIND_COMMAND));
    check_equal(ivr_frame_type_kind(9999, &kind), IVR_EINVAL);
    check_equal(ivr_frame_type_id_by_name("NoSuchTypeV1", &id), IVR_EINVAL);
    check_equal(ivr_frame_type_name(IVR_TYPE_MEDIA_SESSION_OPEN_COMMAND_V1), "MediaSessionOpenCommandV1");
    check_equal(ivr_frame_type_name(IVR_TYPE_MEDIA_SESSION_CLOSE_COMMAND_V1), "MediaSessionCloseCommandV1");
    check_equal(ivr_frame_type_name(IVR_TYPE_MEDIA_COMMAND_RESULT_V1), "MediaCommandResultV1");
    check_equal(ivr_frame_type_name(IVR_TYPE_WORKER_MEDIA_INVENTORY_QUERY_V1), "WorkerMediaInventoryQueryV1");
    check_equal(ivr_frame_type_name(IVR_TYPE_WORKER_MEDIA_INVENTORY_PAGE_V1), "WorkerMediaInventoryPageV1");
    check_equal(ivr_frame_type_name(IVR_TYPE_MEDIA_EVENT_V1), "MediaEventV1");
}

void test_type_kind_mismatch(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE];
    ivr_frame_info_t info = {0};
    info.format = IVR_FMT_BIN;
    info.kind = IVR_KIND_EVENT;
    info.schema_type_id = IVR_TYPE_CONFERENCE_JOIN_COMMAND_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    check_equal(ivr_frame_encode(buf, &info), IVR_EINVAL);
}

void test_encode_invalid(void) {
    uint8_t buf[IVR_FRAME_HEADER_SIZE];
    ivr_frame_info_t info = {0};
    info.format = 9; /* invalid */
    info.kind = IVR_KIND_COMMAND;
    info.schema_type_id = IVR_TYPE_WORKER_SYNC_RESULT_V1;
    info.schema_major = IVR_SCHEMA_MAJOR;
    info.schema_minor = IVR_SCHEMA_MINOR;
    check_equal(ivr_frame_encode(buf, &info), IVR_EINVAL);
    info.format = IVR_FMT_BIN;
    info.schema_major = 9; /* invalid schema version */
    check_equal(ivr_frame_encode(buf, &info), IVR_EVERSION);
    check_equal(ivr_frame_encode(NULL, &info), IVR_EINVAL);
}

spec("test_ivr_frame") {
  it("test_encode_decode_roundtrip") { test_encode_decode_roundtrip(); };
  it("test_magic_bytes") { test_magic_bytes(); };
  it("test_short_frame") { test_short_frame(); };
  it("test_unknown_frame_version") { test_unknown_frame_version(); };
  it("test_unknown_format_kind_flags") { test_unknown_format_kind_flags(); };
  it("test_unknown_type_id") { test_unknown_type_id(); };
  it("test_unknown_schema_version") { test_unknown_schema_version(); };
  it("test_type_registry") { test_type_registry(); };
  it("test_type_kind_mismatch") { test_type_kind_mismatch(); };
  it("test_encode_invalid") { test_encode_invalid(); };
}
