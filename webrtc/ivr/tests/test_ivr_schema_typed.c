/* test_ivr_schema_typed.c - Gate A: tbe_compiler-generated owning structs.
 * Uses the typed API (Type_from_json/to_bin/...) instead of the dynamic
 * DataBindObject API; both bind the same canonical schema. */
#include "turbomedia_ivr_v1.h"
#include "tinytest.h"
#include <string.h>

void test_typed_conference_join_roundtrip(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBind *codec = NULL;
    check_equal(TurboMediaIvrV1_codec_create(&codec, &err), DATA_BIND_OK);
    check_not_null(codec);

    const char *json =
        "{\"message_id\":\"m-1\",\"worker_id\":\"w1\",\"room_id\":\"r1\","
        "\"call_id\":\"c1\",\"call_generation\":7,\"expected_room_version\":42,"
        "\"participant_role\":\"ivr-bot\"}";

    ConferenceJoinCommandV1_t cmd;
    ConferenceJoinCommandV1_init(&cmd);
    check_equal(ConferenceJoinCommandV1_from_json(codec, &cmd, json,
                                                        strlen(json), &err), DATA_BIND_OK);
    check_equal(strcmp(cmd.message_id, "m-1"), 0);
    check_equal(strcmp(cmd.participant_role, "ivr-bot"), 0);
    check_equal((uint64_t)(cmd.call_generation), (uint64_t)(7u));
    check_equal((uint64_t)(cmd.expected_room_version), (uint64_t)(42u));

    static const uint8_t expected_bin[] = {
        0x07,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x2A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x03,0x00,0x00,0x00,0x6D,0x2D,0x31,
        0x02,0x00,0x00,0x00,0x77,0x31,
        0x02,0x00,0x00,0x00,0x72,0x31,
        0x02,0x00,0x00,0x00,0x63,0x31,
        0x07,0x00,0x00,0x00,0x69,0x76,0x72,0x2D,0x62,0x6F,0x74
    };
    uint8_t *bin = NULL;
    size_t bin_len = 0;
    ConferenceJoinCommandV1_t decoded;
    ConferenceJoinCommandV1_init(&decoded);
    check_equal(ConferenceJoinCommandV1_to_bin(
                    &cmd, &bin, &bin_len, &err),
                DATA_BIND_OK);
    check_equal(bin_len, sizeof(expected_bin));
    if (bin != NULL && bin_len == sizeof(expected_bin))
        check_equal(bin, expected_bin, sizeof(expected_bin));
    check_equal(ConferenceJoinCommandV1_from_bin(
                    codec, &decoded, bin, bin_len, &err),
                DATA_BIND_OK);
    check_equal(decoded.call_generation, UINT64_C(7));
    check_equal(decoded.expected_room_version, UINT64_C(42));
    check_equal(strcmp(decoded.message_id, "m-1"), 0);
    check_equal(strcmp(decoded.participant_role, "ivr-bot"), 0);
    tbe_typed_serialized_free(bin);
    ConferenceJoinCommandV1_clear(&decoded);

    char *out = NULL;
    size_t out_len = 0;
    check_equal(ConferenceJoinCommandV1_to_json(
                    codec, &cmd, &out, &out_len, &err),
                DATA_BIND_OK);
    check_not_null(out);
    check_true(strstr(out, "ivr-bot") != NULL);
    tbe_typed_serialized_free(out);

    ConferenceJoinCommandV1_clear(&cmd);
    data_bind_free(codec);
}

void test_typed_result_int32(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBind *codec = NULL;
    check_equal(TurboMediaIvrV1_codec_create(&codec, &err), DATA_BIND_OK);
    IvrCommandResultV1_t res;
    IvrCommandResultV1_init(&res);
    const char *json =
        "{\"message_id\":\"m\",\"worker_id\":\"w\",\"room_id\":\"r\","
        "\"call_id\":\"c\",\"call_generation\":1,\"status_code\":-3,"
        "\"room_version\":1,\"sequence\":9,\"error_code\":\"\","
        "\"error_message\":\"\"}";
    check_equal(IvrCommandResultV1_from_json(codec, &res, json,
                                                   strlen(json), &err), DATA_BIND_OK);
    check_equal(res.status_code, -3);
    check_equal((uint64_t)(res.sequence), (uint64_t)(9u));
    check_equal(strcmp(res.error_code, ""), 0);
    IvrCommandResultV1_clear(&res);
    data_bind_free(codec);
}

spec("test_ivr_schema_typed") {
  it("test_typed_conference_join_roundtrip") { test_typed_conference_join_roundtrip(); };
  it("test_typed_result_int32") { test_typed_result_int32(); };
}
