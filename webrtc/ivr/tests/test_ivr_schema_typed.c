/* test_ivr_schema_typed.c - Gate A: salts-idlc-generated owning structs.
 * Uses the typed API (Type_from_json/to_bin/...) instead of the dynamic
 * DataBindObject API; both bind the same canonical schema. */
#include "turbomedia_ivr_v2.h"
#include "tinytest.h"
#include <string.h>

void test_typed_conference_join_roundtrip(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBind *codec = NULL;
    check_equal(TurboMediaIvrV2_codec_create(&codec, &err), DATA_BIND_OK);
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

    /* Both endpoints use the same fixed-first major 2 contract. Verify the
       generated owning route and dynamic decoder agree on the binary payload. */
    uint8_t *binary = NULL;
    size_t binary_len = 0;
    DataBindObject *dynamic = NULL;
    ConferenceJoinCommandV1_t decoded;
    ConferenceJoinCommandV1_init(&decoded);
    check_equal(ConferenceJoinCommandV1_to_bin(codec, &cmd, &binary,
                                              &binary_len, &err), DATA_BIND_OK);
    check_equal(ConferenceJoinCommandV1_from_bin(codec, &decoded, binary,
                                                binary_len, &err), DATA_BIND_OK);
    check_equal(decoded.message_id, "m-1");
    check_equal(decoded.call_generation, (uint64_t)7);
    check_equal(data_bind_object_from_bin(codec, "ConferenceJoinCommandV1",
                                          binary, binary_len, &dynamic, &err),
                DATA_BIND_OK);
    check_equal(data_bind_value_as_string(data_bind_value_get(
                    data_bind_object_value(dynamic), "participant_role")), "ivr-bot");
    data_bind_object_free(dynamic);
    ConferenceJoinCommandV1_clear(&decoded);
    TurboMediaIvrV2_schema_codec()->free_output(binary);
    char *out = NULL;
    size_t out_len = 0;
    check_equal(ConferenceJoinCommandV1_to_json(codec, &cmd, &out,
                                                      &out_len, &err), DATA_BIND_OK);
    check_not_null(out);
    check_true(strstr(out, "ivr-bot") != NULL);
    TurboMediaIvrV2_schema_codec()->free_output(out);

    ConferenceJoinCommandV1_clear(&cmd);
    data_bind_free(codec);
}

void test_typed_result_int32(void) {
    DataBindError err = DATA_BIND_ERROR_INIT;
    DataBind *codec = NULL;
    check_equal(TurboMediaIvrV2_codec_create(&codec, &err), DATA_BIND_OK);
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
    check_true(tstr_empty(res.error_code));
    IvrCommandResultV1_clear(&res);
    data_bind_free(codec);
}

spec("test_ivr_schema_typed") {
  it("test_typed_conference_join_roundtrip") { test_typed_conference_join_roundtrip(); };
  it("test_typed_result_int32") { test_typed_result_int32(); };
}
