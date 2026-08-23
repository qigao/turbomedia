/**
 * RTP/RTCP Packet Tests
 */
#include "tinytest.h"
#include "turbo_rtp.h"
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static uint16_t test_read_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t test_read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* =============================================================================
 * RTP Packet Build Tests
 * ============================================================================= */

void test_rtp_packet_build_basic(void) {
  uint8_t buffer[256];
  uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  rtp_packet_t pkt;

  int result = rtp_packet_build(&pkt, 96,   /* PT */
                                1234,       /* seq */
                                90000,      /* timestamp */
                                0x12345678, /* SSRC */
                                1,          /* marker */
                                payload, sizeof(payload), buffer, sizeof(buffer));

  check_greater(result, 0);
  check_equal((size_t)((size_t)result), (size_t)(12 + sizeof(payload)));

  /* Check header fields */
  check_equal((uint8_t)(buffer[0] & 0xC0), (uint8_t)(0x80));              /* Version 2 */
  check_equal((uint8_t)(buffer[1]), (uint8_t)(0x80 | 96));                /* Marker + PT */
  check_equal((uint16_t)((buffer[2] << 8) | buffer[3]), (uint16_t)(1234)); /* Sequence */
}

void test_rtp_packet_build_no_marker(void) {
  uint8_t buffer[256];
  uint8_t payload[] = {0xAA, 0xBB};
  rtp_packet_t pkt;

  int result = rtp_packet_build(&pkt, 111,  /* PT */
                                5000,       /* seq */
                                48000,      /* timestamp */
                                0xAABBCCDD, /* SSRC */
                                0,          /* no marker */
                                payload, sizeof(payload), buffer, sizeof(buffer));

  check_greater(result, 0);
  check_equal((uint8_t)(buffer[1]), (uint8_t)(111)); /* No marker bit, PT=111 */
}

/* =============================================================================
 * RTP Packet Parse Tests
 * ============================================================================= */

void test_rtp_packet_parse_basic(void) {
  /* Build a valid RTP packet */
  uint8_t buffer[] = {
      0x80,                   /* V=2, P=0, X=0, CC=0 */
      0xE0,                   /* M=1, PT=96 */
      0x00, 0x01,             /* Seq = 1 */
      0x00, 0x00, 0x00, 0x64, /* Timestamp = 100 */
      0x12, 0x34, 0x56, 0x78, /* SSRC */
      0xDE, 0xAD, 0xBE, 0xEF  /* Payload */
  };

  rtp_packet_t pkt;
  int result = rtp_packet_parse(&pkt, buffer, sizeof(buffer));

  check_equal((int)(result), (int)(0));
  check_equal((uint8_t)(pkt.header.version), (uint8_t)(2));
  check_equal((uint8_t)(pkt.header.marker), (uint8_t)(1));
  check_equal((uint8_t)(pkt.header.payload_type), (uint8_t)(96));
  check_equal((uint16_t)(pkt.header.sequence), (uint16_t)(1));
  check_equal((uint32_t)(pkt.header.timestamp), (uint32_t)(100));
  check_equal((uint32_t)(pkt.header.ssrc), (uint32_t)(0x12345678));
  check_equal((size_t)(pkt.payload_len), (size_t)(4));
}

void test_rtp_packet_parse_too_short(void) {
  uint8_t buffer[] = {0x80, 0x60, 0x00}; /* Only 3 bytes */

  rtp_packet_t pkt;
  int result = rtp_packet_parse(&pkt, buffer, sizeof(buffer));

  check_equal((int)(result), (int)(-1));
}

void test_rtp_packet_parse_invalid_version(void) {
  uint8_t buffer[] = {0x40, /* V=1 (invalid) */
                      0x60, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x12, 0x34, 0x56, 0x78};

  rtp_packet_t pkt;
  int result = rtp_packet_parse(&pkt, buffer, sizeof(buffer));

  check_equal((int)(result), (int)(-1));
}

void test_rtp_packet_roundtrip(void) {
  uint8_t original_payload[] = "Hello RTP!";
  uint8_t buffer[256];
  rtp_packet_t pkt_out;

  /* Build */
  int len = rtp_packet_build(&pkt_out, 111, /* PT */
                             12345,         /* seq */
                             960000,        /* timestamp */
                             0xDEADBEEF,    /* SSRC */
                             0,             /* marker */
                             original_payload, sizeof(original_payload), buffer, sizeof(buffer));
  check_greater(len, 0);

  /* Parse */
  rtp_packet_t pkt_in;
  int result = rtp_packet_parse(&pkt_in, buffer, len);
  check_equal((int)(result), (int)(0));

  /* Verify */
  check_equal((uint8_t)(pkt_in.header.marker), (uint8_t)(pkt_out.header.marker));
  check_equal((uint8_t)(pkt_in.header.payload_type), (uint8_t)(pkt_out.header.payload_type));
  check_equal((uint16_t)(pkt_in.header.sequence), (uint16_t)(pkt_out.header.sequence));
  check_equal((uint32_t)(pkt_in.header.timestamp), (uint32_t)(pkt_out.header.timestamp));
  check_equal((uint32_t)(pkt_in.header.ssrc), (uint32_t)(pkt_out.header.ssrc));
  check_equal((size_t)(pkt_in.payload_len), (size_t)(pkt_out.payload_len));
  check_equal(pkt_in.payload, original_payload, pkt_in.payload_len);
}

void test_rtp_packet_extension_wire_roundtrip(void) {
  uint8_t payload[] = {0xDE, 0xAD, 0xBE};
  uint8_t twcc[] = {0x12, 0x34};
  uint8_t buffer[256];
  rtp_packet_t pkt_out;
  rtp_packet_t pkt_in;
  uint8_t parsed_twcc[2] = {0};
  uint8_t parsed_len = 0;

  int len = rtp_packet_build(&pkt_out, 96, 77, 123456, 0x01020304, 1, payload, sizeof(payload),
                             buffer, sizeof(buffer));
  check_greater(len, 0);
  check_equal((int)(rtp_packet_add_extension(&pkt_out, RTP_EXT_TRANSPORT_CC, twcc, sizeof(twcc))), (int)(0));

  len = rtp_packet_serialize(&pkt_out, buffer, sizeof(buffer));
  check_equal((size_t)((size_t)len), (size_t)(12 + 4 + 4 + sizeof(payload)));
  check_true((buffer[0] & 0x10) != 0);

  check_equal((int)(rtp_packet_parse(&pkt_in, buffer, (size_t)len)), (int)(0));
  check_equal((int)(pkt_in.header.extension), (int)(1));
  check_equal((int)(pkt_in.extension_count), (int)(1));
  check_equal((size_t)(pkt_in.payload_len), (size_t)(sizeof(payload)));
  check_equal(pkt_in.payload, payload, pkt_in.payload_len);

  check_equal((int)(rtp_packet_get_extension(&pkt_in, RTP_EXT_TRANSPORT_CC, parsed_twcc, &parsed_len)), (int)(0));
  check_equal((uint8_t)(parsed_len), (uint8_t)(sizeof(twcc)));
  check_equal(parsed_twcc, twcc, sizeof(twcc));
}

void test_rtp_packet_serialize_clears_empty_extension_bit(void) {
  uint8_t payload[] = {0x01, 0x02};
  uint8_t buffer[256];
  rtp_packet_t pkt;

  int len = rtp_packet_build(&pkt, 96, 10, 20, 0x11223344, 0, payload, sizeof(payload), buffer,
                             sizeof(buffer));
  check_greater(len, 0);

  pkt.header.extension = 1;
  pkt.extension_count = 0;

  len = rtp_packet_serialize(&pkt, buffer, sizeof(buffer));
  check_equal((size_t)((size_t)len), (size_t)(12 + sizeof(payload)));
  check_true((buffer[0] & 0x10) == 0);
}

void test_rtp_packet_parse_stops_at_reserved_extension_id(void) {
  uint8_t buffer[] = {0x90, 0x60, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x12, 0x34, 0x56,
                      0x78, 0xBE, 0xDE, 0x00, 0x01, 0xF0, 0x00, 0x00, 0x00, 0xAA, 0xBB};
  rtp_packet_t pkt;

  check_equal((int)(rtp_packet_parse(&pkt, buffer, sizeof(buffer))), (int)(0));
  check_equal((int)(pkt.header.extension), (int)(1));
  check_equal((int)(pkt.extension_count), (int)(0));
  check_equal((size_t)(pkt.payload_len), (size_t)(2));
  check_equal((uint8_t)(pkt.payload[0]), (uint8_t)(0xAA));
}

/* =============================================================================
 * RTP Session
 * Tests
 * ============================================================================= */

void test_rtp_session_create_destroy(void) {
  rtp_session_config_t config = {.ssrc = 0, /* Auto-generate */
                                 .payload_type = 96,
                                 .clock_rate = 90000,
                                 .is_audio = 0};

  rtp_session_t *session = rtp_session_create(&config);
  check_not_null(session);

  uint32_t ssrc = rtp_session_get_ssrc(session);
  check_not_equal(ssrc, 0);

  rtp_session_destroy(session);
}

void test_rtp_session_auto_generated_ssrcs_are_unique(void) {
  rtp_session_config_t config = {.ssrc = 0, .payload_type = 96, .clock_rate = 90000, .is_audio = 0};
  rtp_session_t *session_a = rtp_session_create(&config);
  rtp_session_t *session_b = rtp_session_create(&config);
  rtp_session_t *session_c = rtp_session_create(&config);
  uint32_t ssrc_a;
  uint32_t ssrc_b;
  uint32_t ssrc_c;

  check_not_null(session_a);
  check_not_null(session_b);
  check_not_null(session_c);

  ssrc_a = rtp_session_get_ssrc(session_a);
  ssrc_b = rtp_session_get_ssrc(session_b);
  ssrc_c = rtp_session_get_ssrc(session_c);

  check_not_equal(ssrc_a, 0);
  check_not_equal(ssrc_b, 0);
  check_not_equal(ssrc_c, 0);
  check_true(ssrc_a != ssrc_b);
  check_true(ssrc_a != ssrc_c);
  check_true(ssrc_b != ssrc_c);

  rtp_session_destroy(session_c);
  rtp_session_destroy(session_b);
  rtp_session_destroy(session_a);
}

void test_rtp_session_send_sequence_increment(void) {
  rtp_session_config_t config = {
      .ssrc = 0x12345678, .payload_type = 111, .clock_rate = 48000, .is_audio = 1};

  rtp_session_t *session = rtp_session_create(&config);
  check_not_null(session);

  uint8_t payload[] = {0x01, 0x02, 0x03};
  uint8_t buffer1[256], buffer2[256];
  rtp_packet_t pkt1, pkt2;

  /* Send first packet */
  int len1 =
      rtp_session_send(session, payload, sizeof(payload), 0, &pkt1, buffer1, sizeof(buffer1));
  check_greater(len1, 0);

  /* Send second packet */
  int len2 =
      rtp_session_send(session, payload, sizeof(payload), 0, &pkt2, buffer2, sizeof(buffer2));
  check_greater(len2, 0);

  /* Verify sequence increment */
  check_equal((uint16_t)(pkt2.header.sequence), (uint16_t)(pkt1.header.sequence + 1));

  rtp_session_destroy(session);
}

void test_rtp_session_send_with_marker(void) {
  rtp_session_config_t config = {
      .ssrc = 0xCAFEBABE, .payload_type = 96, .clock_rate = 90000, .is_audio = 0};

  rtp_session_t *session = rtp_session_create(&config);
  check_not_null(session);

  uint8_t payload[] = {0xFF};
  uint8_t buffer[256];
  rtp_packet_t pkt;

  int len = rtp_session_send(session, payload, sizeof(payload), 1, &pkt, buffer, sizeof(buffer));
  check_greater(len, 0);
  check_equal((uint8_t)(pkt.header.marker), (uint8_t)(1));

  rtp_session_destroy(session);
}

void test_rtp_session_stats(void) {
  rtp_session_config_t config = {
      .ssrc = 0x11223344, .payload_type = 111, .clock_rate = 48000, .is_audio = 1};

  rtp_session_t *session = rtp_session_create(&config);
  check_not_null(session);

  /* Send a few packets */
  uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
  uint8_t buffer[256];
  rtp_packet_t pkt;

  for (int i = 0; i < 5; i++) {
    rtp_session_send(session, payload, sizeof(payload), 0, &pkt, buffer, sizeof(buffer));
  }

  /* Check stats */
  rtp_session_stats_t stats;
  rtp_session_get_stats(session, &stats);

  check_equal((uint64_t)(stats.packets_sent), (uint64_t)(5));
  check_equal((uint64_t)(stats.octets_sent), (uint64_t)(20)); /* 5 * 4 bytes */

  rtp_session_destroy(session);
}

void test_rtp_history_rejects_invalid_capacities(void) {
  check_null(rtp_history_create(0, 1));
  check_null(rtp_history_create(1, 0));
  check_null(rtp_history_create((size_t)-1, 2));
}

void test_rtp_history_roundtrips_full_capacity_packet(void) {
  enum { TEST_HISTORY_PACKET_SIZE = RTP_MAX_PACKET + 14 };
  uint8_t packet[TEST_HISTORY_PACKET_SIZE];
  uint8_t restored[TEST_HISTORY_PACKET_SIZE];
  size_t restored_len = sizeof(restored);
  rtp_history_t *history = rtp_history_create(2, sizeof(packet));

  check_not_null(history);
  memset(packet, 0x5a, sizeof(packet));
  rtp_history_put(history, 7, packet, sizeof(packet));
  check_equal((int)(rtp_history_get(history, 7, restored, &restored_len, sizeof(restored))), (int)(0));
  check_equal((size_t)(restored_len), (size_t)(sizeof(packet)));
  check_equal(restored, packet, sizeof(packet));

  rtp_history_destroy(history);
}

/* =============================================================================
 * RTCP Tests
 * ============================================================================= */

void test_rtcp_compound_sr(void) {
  uint8_t buffer[256];

  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  rtcp_sr_t sr = {.ssrc = 0x12345678,
                  .ntp_sec = 3600,
                  .ntp_frac = 0,
                  .rtp_ts = 90000,
                  .packet_count = 100,
                  .octet_count = 50000};

  int result = rtcp_compound_add_sr(&compound, &sr, NULL, 0);
  check_equal((int)(result), (int)(0));

  size_t total_len = rtcp_compound_finish(&compound);
  check_greater(total_len, 0);

  /* Verify RTCP header */
  check_equal((uint8_t)(buffer[0] & 0xC0), (uint8_t)(0x80)); /* Version 2 */
  check_equal((uint8_t)(buffer[1]), (uint8_t)(200));         /* PT = SR */
}

void test_rtcp_compound_nack(void) {
  uint8_t buffer[256];

  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  /* NACK for sequence 100 with bitmap 0x0005 (also missing 101, 103) */
  int result = rtcp_compound_add_nack(&compound, 0xAABBCCDD, 0x11223344, 100, 0x0005);
  check_equal((int)(result), (int)(0));

  size_t total_len = rtcp_compound_finish(&compound);
  check_greater(total_len, 0);
}

void test_rtcp_compound_pli(void) {
  uint8_t buffer[256];

  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  int result = rtcp_compound_add_pli(&compound, 0xDEADBEEF, 0xCAFEBABE);
  check_equal((int)(result), (int)(0));

  size_t total_len = rtcp_compound_finish(&compound);
  check_equal((size_t)(total_len), (size_t)(12)); /* Fixed size for PLI */
}

void test_rtcp_compound_fir(void) {
  uint8_t buffer[256];

  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  int result = rtcp_compound_add_fir(&compound, 0x11111111, 0x22222222, 5);
  check_equal((int)(result), (int)(0));

  size_t total_len = rtcp_compound_finish(&compound);
  check_greater(total_len, 0);
}

void test_rtcp_compound_remb(void) {
  uint8_t buffer[256];

  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  int result = rtcp_compound_add_remb(&compound, 0xAAAAAAAA, 0xBBBBBBBB, 2000000); /* 2 Mbps */
  check_equal((int)(result), (int)(0));

  size_t total_len = rtcp_compound_finish(&compound);
  check_equal((size_t)(total_len), (size_t)(24));
  check_equal((uint8_t)(buffer[0]), (uint8_t)((uint8_t)((2u << 6) | RTCP_REMB)));
  check_equal((uint8_t)(buffer[1]), (uint8_t)(RTCP_PSFB));
  check_equal((uint16_t)(test_read_be16(buffer + 2)), (uint16_t)(5u));
  check_equal((uint32_t)(test_read_be32(buffer + 12)), (uint32_t)(0x52454d42u));
  check_equal((uint8_t)(buffer[16]), (uint8_t)(1u));
  check_equal((uint32_t)(test_read_be32(buffer + 20)), (uint32_t)(0xBBBBBBBBu));
}

/* =============================================================================
 * Sequence Number Utility Tests
 * ============================================================================= */

void test_rtp_seq_newer(void) {
  /* Normal case */
  check_true(rtp_seq_newer(100, 99));
  check_false(rtp_seq_newer(99, 100));

  /* Wraparound case */
  check_true(rtp_seq_newer(1, 65535));
  check_false(rtp_seq_newer(65535, 1));

  /* Equal */
  check_false(rtp_seq_newer(50, 50));
}

void test_rtp_seq_diff(void) {
  /* Normal case */
  check_equal((int)(rtp_seq_diff(100, 99)), (int)(1));
  check_equal((int)(rtp_seq_diff(99, 100)), (int)(-1));

  /* Wraparound case */
  check_equal((int)(rtp_seq_diff(1, 65535)), (int)(2));
  check_equal((int)(rtp_seq_diff(65535, 1)), (int)(-2));
}

/* =============================================================================
 * Main
 * ============================================================================= */

spec("test_rtp") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  /* RTP Build */
  it("test_rtp_packet_build_basic") { test_rtp_packet_build_basic(); };
  it("test_rtp_packet_build_no_marker") { test_rtp_packet_build_no_marker(); };

  /* RTP Parse */
  it("test_rtp_packet_parse_basic") { test_rtp_packet_parse_basic(); };
  it("test_rtp_packet_parse_too_short") { test_rtp_packet_parse_too_short(); };
  it("test_rtp_packet_parse_invalid_version") { test_rtp_packet_parse_invalid_version(); };
  it("test_rtp_packet_roundtrip") { test_rtp_packet_roundtrip(); };
  it("test_rtp_packet_extension_wire_roundtrip") { test_rtp_packet_extension_wire_roundtrip(); };
  it("test_rtp_packet_serialize_clears_empty_extension_bit") { test_rtp_packet_serialize_clears_empty_extension_bit(); };
  it("test_rtp_packet_parse_stops_at_reserved_extension_id") { test_rtp_packet_parse_stops_at_reserved_extension_id(); };

  /* RTP Session */
  it("test_rtp_session_create_destroy") { test_rtp_session_create_destroy(); };
  it("test_rtp_session_auto_generated_ssrcs_are_unique") { test_rtp_session_auto_generated_ssrcs_are_unique(); };
  it("test_rtp_session_send_sequence_increment") { test_rtp_session_send_sequence_increment(); };
  it("test_rtp_session_send_with_marker") { test_rtp_session_send_with_marker(); };
  it("test_rtp_session_stats") { test_rtp_session_stats(); };
  it("test_rtp_history_rejects_invalid_capacities") { test_rtp_history_rejects_invalid_capacities(); };
  it("test_rtp_history_roundtrips_full_capacity_packet") { test_rtp_history_roundtrips_full_capacity_packet(); };

  /* RTCP */
  it("test_rtcp_compound_sr") { test_rtcp_compound_sr(); };
  it("test_rtcp_compound_nack") { test_rtcp_compound_nack(); };
  it("test_rtcp_compound_pli") { test_rtcp_compound_pli(); };
  it("test_rtcp_compound_fir") { test_rtcp_compound_fir(); };
  it("test_rtcp_compound_remb") { test_rtcp_compound_remb(); };

  /* Sequence utilities */
  it("test_rtp_seq_newer") { test_rtp_seq_newer(); };
  it("test_rtp_seq_diff") { test_rtp_seq_diff(); };
}
