#include "tinytest.h"
#include "sctp_c11_contract.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

_Static_assert(TM_SC_ABI_V1 == 1u, "contract must be explicitly versioned");
_Static_assert(TM_SC_PPID_DCEP == 50u, "RFC 8831 DCEP PPID");
_Static_assert(TM_SC_PPID_STRING == 51u, "RFC 8831 text PPID");
_Static_assert(TM_SC_PPID_BINARY == 53u, "RFC 8831 binary PPID");
_Static_assert(TM_SC_PPID_STRING_EMPTY == 56u, "RFC 8831 empty text PPID");
_Static_assert(TM_SC_PPID_BINARY_EMPTY == 57u, "RFC 8831 empty binary PPID");

/* Merely compiling this translation unit checks the proposed C11-only
 * operations-table types against the actual installed TurboMedia public
 * channel config. There is intentionally NO fake SCTP session implementation. */
static void test_compile_only_owner_shape(void) {
    tm_sc_owner_ops_v1 ops = {0};
    tm_sc_session_config_v1 config = {0};
    check_equal((size_t)sizeof(ops), (size_t)sizeof(tm_sc_owner_ops_v1));
    check_equal((size_t)sizeof(config), (size_t)sizeof(tm_sc_session_config_v1));
    ops.size = sizeof(ops);
    ops.version = TM_SC_ABI_V1;
    config.size = sizeof(config);
    config.version = TM_SC_ABI_V1;
    config.peer_generation = 17u;
    config.mtu = 1188u;
    config.max_streams = 64u;
    config.max_tx_bytes = 4096u;
    config.max_rx_bytes = 4096u;
    config.max_events = 32u;
    check_equal(ops.version, TM_SC_ABI_V1);
    check_equal(config.peer_generation, UINT64_C(17));
    check_null(ops.send_message); /* upstream association API unavailable */
}

static void test_reliable_ordered_text(void) {
    static const char text[] = "hello";
    turbo_dc_channel_config_t cfg = {.ordered = 1};
    tm_sc_send_v1 request = TM_SC_SEND_V1_INIT;
    check_equal(tm_sc_make_user_send(&cfg, 14u, 2u, text,
                                     sizeof(text) - 1u, 0, 1024u, &request),
                TM_SC_OK);
    check_equal(request.ppid, TM_SC_PPID_STRING);
    check_equal(request.sid, (uint16_t)2u);
    check_equal(request.peer_generation, UINT64_C(14));
    check_equal(request.wire_bytes, sizeof(text) - 1u);
    check_equal(request.app_bytes, sizeof(text) - 1u);
    check_equal(request.policy, TM_SC_RELIABLE);
    check_equal(request.unordered, (uint8_t)0u);
    check_true(request.data == text);
}

static void test_reliability_unordered_and_binary(void) {
    static const uint8_t data[] = {0x10, 0x20, 0x30};
    turbo_dc_channel_config_t cfg = {.ordered = 0, .max_retransmits = 8};
    tm_sc_send_v1 request = TM_SC_SEND_V1_INIT;
    check_equal(tm_sc_make_user_send(&cfg, 1u, UINT16_MAX,
                                     data, sizeof(data), 1, sizeof(data),
                                     &request), TM_SC_OK);
    check_equal(request.ppid, TM_SC_PPID_BINARY);
    check_equal(request.sid, UINT16_MAX);
    check_equal(request.unordered, (uint8_t)1u);
    check_equal(request.policy, TM_SC_MAX_RETRANSMITS);
    check_equal(request.policy_value, UINT32_C(8));
    cfg.max_retransmits = 0;
    cfg.max_lifetime_ms = 33;
    check_equal(tm_sc_make_user_send(&cfg, 1u, 4u, data,
                                     sizeof(data), 0, sizeof(data),
                                     &request), TM_SC_OK);
    check_equal(request.ppid, TM_SC_PPID_STRING);
    check_equal(request.policy, TM_SC_MAX_LIFETIME_MS);
    check_equal(request.policy_value, UINT32_C(33));
}

static void test_empty_user_message_wire_octet(void) {
    static const uint8_t placeholder = 0xA5;
    turbo_dc_channel_config_t cfg = {.ordered = 1};
    tm_sc_send_v1 request = TM_SC_SEND_V1_INIT;
    tm_sc_receive_v1 incoming = TM_SC_RECEIVE_V1_INIT;
    check_equal(tm_sc_make_user_send(&cfg, 22u, 0u, &placeholder,
                                     0u, 0, 1u, &request), TM_SC_OK);
    check_equal(request.ppid, TM_SC_PPID_STRING_EMPTY);
    check_equal(request.wire_bytes, (size_t)1u);
    check_equal(request.app_bytes, (size_t)0u);
    check_not_null(request.data);
    check_equal(*(const uint8_t *)request.data, (uint8_t)0u);
    check_equal(tm_sc_classify_received(22u, 0u, request.ppid, request.data,
                                         request.wire_bytes, &incoming), TM_SC_OK);
    check_equal(incoming.kind, TM_SC_TEXT);
    check_equal(incoming.app_bytes, (size_t)0u);
    check_null(incoming.data);
    check_equal(tm_sc_make_user_send(&cfg, 22u, 1u, &placeholder,
                                     0u, 1, 1u, &request), TM_SC_OK);
    check_equal(request.ppid, TM_SC_PPID_BINARY_EMPTY);
    check_equal(tm_sc_classify_received(22u, 1u, request.ppid, request.data,
                                         request.wire_bytes, &incoming), TM_SC_OK);
    check_equal(incoming.kind, TM_SC_BINARY);
    check_equal(incoming.app_bytes, (size_t)0u);
}

static void test_dcep_envelope_is_reliable_ordered(void) {
    const uint8_t ack[] = {0x02};
    const uint8_t open[12] = {0x03};
    tm_sc_send_v1 request = TM_SC_SEND_V1_INIT;
    tm_sc_receive_v1 received = TM_SC_RECEIVE_V1_INIT;
    check_equal(tm_sc_make_dcep_send(5u, 3u, open, sizeof(open),
                                      sizeof(open), &request), TM_SC_OK);
    check_equal(request.ppid, TM_SC_PPID_DCEP);
    check_equal(request.unordered, (uint8_t)0u);
    check_equal(request.policy, TM_SC_RELIABLE);
    check_equal(request.data, (const void *)open);
    check_equal(tm_sc_make_dcep_send(5u, 3u, ack, sizeof(ack),
                                      sizeof(ack), &request), TM_SC_OK);
    check_equal(request.wire_bytes, (size_t)1u);
    check_equal(tm_sc_classify_received(5u, 3u, TM_SC_PPID_DCEP,
                                         ack, sizeof(ack), &received), TM_SC_OK);
    check_equal(received.kind, TM_SC_DCEP);
    check_true(received.data == ack);
}

static void test_bad_configuration_clears_previous_borrow(void) {
    const uint8_t data[] = {0xDE, 0xAD};
    turbo_dc_channel_config_t cfg = {.ordered = 1};
    tm_sc_send_v1 req = TM_SC_SEND_V1_INIT;
    check_equal(tm_sc_make_user_send(&cfg, 1u, 1u, data, sizeof(data),
                                      0, 8u, &req), TM_SC_OK);
    cfg.max_retransmits = 3;
    cfg.max_lifetime_ms = 5;
    check_equal(tm_sc_make_user_send(&cfg, 1u, 1u, data, sizeof(data),
                                      0, 8u, &req), TM_SC_EINVAL);
    check_null(req.data);
    check_equal(req.wire_bytes, (size_t)0u);
    cfg.max_lifetime_ms = 0;
    cfg.max_retransmits = -1;
    check_equal(tm_sc_make_user_send(&cfg, 1u, 1u, data, sizeof(data),
                                      0, 8u, &req), TM_SC_EINVAL);
    cfg.max_retransmits = UINT16_MAX + 1;
    check_equal(tm_sc_make_user_send(&cfg, 1u, 1u, data, sizeof(data),
                                      0, 8u, &req), TM_SC_EINVAL);
    cfg.max_retransmits = 0;
    check_equal(tm_sc_make_user_send(&cfg, 0u, 1u, data, sizeof(data),
                                      0, 8u, &req), TM_SC_EINVAL);
    check_equal(tm_sc_make_user_send(&cfg, 1u, 1u, NULL, 0u,
                                      0, 8u, &req), TM_SC_EINVAL);
    check_equal(tm_sc_make_user_send(&cfg, 1u, 1u, data, sizeof(data),
                                      2, 8u, &req), TM_SC_EINVAL);
}

static void test_capacity_version_and_zero_budget(void) {
    const uint8_t bytes[] = {1,2,3};
    turbo_dc_channel_config_t cfg = {.ordered = 1};
    tm_sc_send_v1 req = TM_SC_SEND_V1_INIT;
    check_equal(tm_sc_make_user_send(&cfg, 1u, 1u, bytes, sizeof(bytes),
                                      1, 2u, &req), TM_SC_ECAPACITY);
    check_null(req.data);
    check_equal(tm_sc_make_user_send(&cfg, 1u, 1u, bytes, sizeof(bytes),
                                      1, 0u, &req), TM_SC_EINVAL);
    req.version = 2u;
    check_equal(tm_sc_make_user_send(&cfg, 1u, 1u, bytes, sizeof(bytes),
                                      1, 4u, &req), TM_SC_EVERSION);
    req.version = TM_SC_ABI_V1;
    req.size = 0u;
    check_equal(tm_sc_make_user_send(&cfg, 1u, 1u, bytes, sizeof(bytes),
                                      1, 4u, &req), TM_SC_EVERSION);
    check_equal(tm_sc_make_user_send(&cfg, 1u, 1u, bytes, sizeof(bytes),
                                      1, 4u, NULL), TM_SC_EINVAL);
}

static void test_receive_unknown_and_malformed_empty(void) {
    const uint8_t sample[] = {0,1,2};
    tm_sc_receive_v1 incoming = TM_SC_RECEIVE_V1_INIT;
    check_equal(tm_sc_classify_received(1u, 5u, TM_SC_PPID_BINARY,
                                         sample, sizeof(sample), &incoming), TM_SC_OK);
    check_equal(incoming.app_bytes, sizeof(sample));
    check_true(incoming.data == sample);
    check_equal(incoming.kind, TM_SC_BINARY);
    check_equal(tm_sc_classify_received(1u, 5u, 99u,
                                         sample, sizeof(sample), &incoming), TM_SC_EUNSUPPORTED);
    check_null(incoming.data);
    check_equal(tm_sc_classify_received(1u, 5u, TM_SC_PPID_STRING_EMPTY,
                                         sample, 2u, &incoming), TM_SC_EINVAL);
    check_equal(tm_sc_classify_received(1u, 5u, TM_SC_PPID_BINARY_EMPTY,
                                         sample + 1u, 1u, &incoming), TM_SC_EINVAL);
    check_equal(tm_sc_classify_received(1u, 5u, TM_SC_PPID_DCEP,
                                         NULL, 0u, &incoming), TM_SC_EINVAL);
    check_equal(tm_sc_classify_received(0u, 5u, TM_SC_PPID_DCEP,
                                         sample, 1u, &incoming), TM_SC_EINVAL);
}

static void test_dcep_rejects_invalid_envelope_and_capacity(void) {
    const uint8_t bad[] = {0x04,0x00};
    const uint8_t short_open[] = {0x03,0x00};
    const uint8_t ack[] = {0x02};
    tm_sc_send_v1 req = TM_SC_SEND_V1_INIT;
    check_equal(tm_sc_make_dcep_send(1u, 2u, bad, sizeof(bad),
                                      10u, &req), TM_SC_EINVAL);
    check_equal(tm_sc_make_dcep_send(1u, 2u, short_open, sizeof(short_open),
                                      10u, &req), TM_SC_EINVAL);
    check_equal(tm_sc_make_dcep_send(1u, 2u, ack, sizeof(ack),
                                      0u, &req), TM_SC_EINVAL);
    check_equal(tm_sc_make_dcep_send(1u, 2u, ack, sizeof(ack),
                                      1u, &req), TM_SC_OK);
    check_equal(tm_sc_make_dcep_send(1u, 2u, ack, sizeof(ack),
                                      0u, &req), TM_SC_EINVAL);
    check_null(req.data);
}

spec("test_sctp_c11_contract") {
  it("only C11 Owner operations-table shape") {test_compile_only_owner_shape();};
  it("reliable ordered text PPID") {test_reliable_ordered_text();};
  it("unordered binary RTX and TTL") {test_reliability_unordered_and_binary();};
  it("RFC empty PPID exact zero octet") {test_empty_user_message_wire_octet();};
  it("DCEP ACK/OPEN envelope") {test_dcep_envelope_is_reliable_ordered();};
  it("negative reliability and stale borrow") {test_bad_configuration_clears_previous_borrow();};
  it("capacity and ABI version") {test_capacity_version_and_zero_budget();};
  it("receive PPID rejection and empty shape") {test_receive_unknown_and_malformed_empty();};
  it("reject invalid DCEP envelope") {test_dcep_rejects_invalid_envelope_and_capacity();};
}
