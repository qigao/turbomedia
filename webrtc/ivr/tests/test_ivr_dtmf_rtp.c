#include "ivr_dtmf_rtp.h"
#include "tinytest.h"
#include <string.h>

static ivr_dtmf_ingress_t *g_ingress;
static const ivr_call_ref_t g_call = {
    .provider_session_id = {"session-1", 9},
    .dialog_id = {"dialog-1", 8},
    .room_id = {"room-1", 6},
    .call_id = {"call-1", 6},
    .call_generation = 7,
    .expected_room_version = 0};

void setUp(void) {
    ivr_dtmf_ingress_config_t config;
    memset(&config, 0, sizeof(config));
    config.window_capacity = 2;
    config.max_duration = 8000;
    check_equal(ivr_dtmf_ingress_create(&config, &g_ingress), IVR_OK);
    check_equal(ivr_dtmf_ingress_begin_input(g_ingress, &g_call,
                                                   "window-1", 11), IVR_OK);
}

void tearDown(void) {
    ivr_dtmf_ingress_destroy(g_ingress);
    g_ingress = NULL;
}

void test_final_digit_and_dedup(void) {
    const uint8_t payload[] = {1, 0x80u | 10u, 0x01u, 0x40u};
    ivr_dtmf_input_t input;
    check_equal(ivr_dtmf_ingress_submit_rtp(
                          g_ingress, &g_call, 3, 9000, payload,
                          sizeof(payload), &input), IVR_OK);
    check_equal((int)(input.digit), (int)('1'));
    check_equal((uint16_t)(input.duration), (uint16_t)(320u));
    check_equal((uint64_t)(input.input_generation), (uint64_t)(11u));
    check_equal(ivr_dtmf_ingress_submit_rtp(
                          g_ingress, &g_call, 3, 9000, payload,
                          sizeof(payload), &input), IVR_ESTATE);
}

void test_star_hash_and_abcd_mapping(void) {
    static const uint8_t events[] = {10, 11, 12, 13, 14, 15};
    static const char digits[] = "*#ABCD";
    for (size_t i = 0; i < sizeof(events); ++i) {
        uint8_t payload[] = {events[i], 0x80u, 0, 80};
        ivr_dtmf_input_t input;
        check_equal(ivr_dtmf_ingress_submit_rtp(
                              g_ingress, &g_call, 4, (uint32_t)(100 + i),
                              payload, sizeof(payload), &input), IVR_OK);
        check_equal((int)(input.digit), (int)(digits[i]));
    }
}

void test_invalid_and_unfinished_packets(void) {
    ivr_dtmf_input_t input;
    const uint8_t unfinished[] = {2, 7, 0, 80};
    const uint8_t invalid_event[] = {16, 0x80u, 0, 80};
    const uint8_t reserved[] = {2, 0xC0u, 0, 80};
    const uint8_t zero_duration[] = {2, 0x80u, 0, 0};
    check_equal(ivr_dtmf_ingress_submit_rtp(
                          g_ingress, &g_call, 1, 1, unfinished,
                          sizeof(unfinished), &input), IVR_ESTATE);
    check_equal(ivr_dtmf_ingress_submit_rtp(
                          g_ingress, &g_call, 1, 2, invalid_event,
                          sizeof(invalid_event), &input), IVR_EINVAL);
    check_equal(ivr_dtmf_ingress_submit_rtp(
                          g_ingress, &g_call, 1, 3, reserved,
                          sizeof(reserved), &input), IVR_EINVAL);
    check_equal(ivr_dtmf_ingress_submit_rtp(
                          g_ingress, &g_call, 1, 4, zero_duration,
                          sizeof(zero_duration), &input), IVR_EINVAL);
}

void test_stale_or_closed_window_rejected(void) {
    const uint8_t payload[] = {3, 0x80u, 0, 80};
    ivr_dtmf_input_t input;
    check_equal(ivr_dtmf_ingress_end_input(g_ingress, &g_call, 10), IVR_ESTATE);
    check_equal(ivr_dtmf_ingress_end_input(g_ingress, &g_call, 11), IVR_OK);
    check_equal(ivr_dtmf_ingress_submit_rtp(
                          g_ingress, &g_call, 1, 1, payload,
                          sizeof(payload), &input), IVR_ESTATE);
}

spec("test_ivr_dtmf_rtp") {
  before_each() { setUp(); }
  after_each() { tearDown(); }
  it("test_final_digit_and_dedup") { test_final_digit_and_dedup(); };
  it("test_star_hash_and_abcd_mapping") { test_star_hash_and_abcd_mapping(); };
  it("test_invalid_and_unfinished_packets") { test_invalid_and_unfinished_packets(); };
  it("test_stale_or_closed_window_rejected") { test_stale_or_closed_window_rejected(); };
}
