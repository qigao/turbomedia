/**
 * RTP/RTCP Packet Tests
 */
#include "tinytest_compat.h"
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

  TEST_ASSERT_GREATER_THAN(0, result);
  TEST_ASSERT_EQUAL_size_t(12 + sizeof(payload), (size_t)result);

  /* Check header fields */
  TEST_ASSERT_EQUAL_UINT8(0x80, buffer[0] & 0xC0);              /* Version 2 */
  TEST_ASSERT_EQUAL_UINT8(0x80 | 96, buffer[1]);                /* Marker + PT */
  TEST_ASSERT_EQUAL_UINT16(1234, (buffer[2] << 8) | buffer[3]); /* Sequence */
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

  TEST_ASSERT_GREATER_THAN(0, result);
  TEST_ASSERT_EQUAL_UINT8(111, buffer[1]); /* No marker bit, PT=111 */
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

  TEST_ASSERT_EQUAL_INT(0, result);
  TEST_ASSERT_EQUAL_UINT8(2, pkt.header.version);
  TEST_ASSERT_EQUAL_UINT8(1, pkt.header.marker);
  TEST_ASSERT_EQUAL_UINT8(96, pkt.header.payload_type);
  TEST_ASSERT_EQUAL_UINT16(1, pkt.header.sequence);
  TEST_ASSERT_EQUAL_UINT32(100, pkt.header.timestamp);
  TEST_ASSERT_EQUAL_UINT32(0x12345678, pkt.header.ssrc);
  TEST_ASSERT_EQUAL_size_t(4, pkt.payload_len);
}

void test_rtp_packet_parse_too_short(void) {
  uint8_t buffer[] = {0x80, 0x60, 0x00}; /* Only 3 bytes */

  rtp_packet_t pkt;
  int result = rtp_packet_parse(&pkt, buffer, sizeof(buffer));

  TEST_ASSERT_EQUAL_INT(-1, result);
}

void test_rtp_packet_parse_invalid_version(void) {
  uint8_t buffer[] = {0x40, /* V=1 (invalid) */
                      0x60, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x12, 0x34, 0x56, 0x78};

  rtp_packet_t pkt;
  int result = rtp_packet_parse(&pkt, buffer, sizeof(buffer));

  TEST_ASSERT_EQUAL_INT(-1, result);
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
  TEST_ASSERT_GREATER_THAN(0, len);

  /* Parse */
  rtp_packet_t pkt_in;
  int result = rtp_packet_parse(&pkt_in, buffer, len);
  TEST_ASSERT_EQUAL_INT(0, result);

  /* Verify */
  TEST_ASSERT_EQUAL_UINT8(pkt_out.header.marker, pkt_in.header.marker);
  TEST_ASSERT_EQUAL_UINT8(pkt_out.header.payload_type, pkt_in.header.payload_type);
  TEST_ASSERT_EQUAL_UINT16(pkt_out.header.sequence, pkt_in.header.sequence);
  TEST_ASSERT_EQUAL_UINT32(pkt_out.header.timestamp, pkt_in.header.timestamp);
  TEST_ASSERT_EQUAL_UINT32(pkt_out.header.ssrc, pkt_in.header.ssrc);
  TEST_ASSERT_EQUAL_size_t(pkt_out.payload_len, pkt_in.payload_len);
  TEST_ASSERT_EQUAL_MEMORY(original_payload, pkt_in.payload, pkt_in.payload_len);
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
  TEST_ASSERT_GREATER_THAN(0, len);
  TEST_ASSERT_EQUAL_INT(
      0, rtp_packet_add_extension(&pkt_out, RTP_EXT_TRANSPORT_CC, twcc, sizeof(twcc)));

  len = rtp_packet_serialize(&pkt_out, buffer, sizeof(buffer));
  TEST_ASSERT_EQUAL_size_t(12 + 4 + 4 + sizeof(payload), (size_t)len);
  TEST_ASSERT_TRUE((buffer[0] & 0x10) != 0);

  TEST_ASSERT_EQUAL_INT(0, rtp_packet_parse(&pkt_in, buffer, (size_t)len));
  TEST_ASSERT_EQUAL_INT(1, pkt_in.header.extension);
  TEST_ASSERT_EQUAL_INT(1, pkt_in.extension_count);
  TEST_ASSERT_EQUAL_size_t(sizeof(payload), pkt_in.payload_len);
  TEST_ASSERT_EQUAL_MEMORY(payload, pkt_in.payload, pkt_in.payload_len);

  TEST_ASSERT_EQUAL_INT(
      0, rtp_packet_get_extension(&pkt_in, RTP_EXT_TRANSPORT_CC, parsed_twcc, &parsed_len));
  TEST_ASSERT_EQUAL_UINT8(sizeof(twcc), parsed_len);
  TEST_ASSERT_EQUAL_MEMORY(twcc, parsed_twcc, sizeof(twcc));
}

void test_rtp_packet_serialize_clears_empty_extension_bit(void) {
  uint8_t payload[] = {0x01, 0x02};
  uint8_t buffer[256];
  rtp_packet_t pkt;

  int len = rtp_packet_build(&pkt, 96, 10, 20, 0x11223344, 0, payload, sizeof(payload), buffer,
                             sizeof(buffer));
  TEST_ASSERT_GREATER_THAN(0, len);

  pkt.header.extension = 1;
  pkt.extension_count = 0;

  len = rtp_packet_serialize(&pkt, buffer, sizeof(buffer));
  TEST_ASSERT_EQUAL_size_t(12 + sizeof(payload), (size_t)len);
  TEST_ASSERT_TRUE((buffer[0] & 0x10) == 0);
}

void test_rtp_packet_parse_stops_at_reserved_extension_id(void) {
  uint8_t buffer[] = {0x90, 0x60, 0x00, 0x01, 0x00, 0x00, 0x00, 0x64, 0x12, 0x34, 0x56,
                      0x78, 0xBE, 0xDE, 0x00, 0x01, 0xF0, 0x00, 0x00, 0x00, 0xAA, 0xBB};
  rtp_packet_t pkt;

  TEST_ASSERT_EQUAL_INT(0, rtp_packet_parse(&pkt, buffer, sizeof(buffer)));
  TEST_ASSERT_EQUAL_INT(1, pkt.header.extension);
  TEST_ASSERT_EQUAL_INT(0, pkt.extension_count);
  TEST_ASSERT_EQUAL_size_t(2, pkt.payload_len);
  TEST_ASSERT_EQUAL_UINT8(0xAA, pkt.payload[0]);
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
  TEST_ASSERT_NOT_NULL(session);

  uint32_t ssrc = rtp_session_get_ssrc(session);
  TEST_ASSERT_NOT_EQUAL(0, ssrc);

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

  TEST_ASSERT_NOT_NULL(session_a);
  TEST_ASSERT_NOT_NULL(session_b);
  TEST_ASSERT_NOT_NULL(session_c);

  ssrc_a = rtp_session_get_ssrc(session_a);
  ssrc_b = rtp_session_get_ssrc(session_b);
  ssrc_c = rtp_session_get_ssrc(session_c);

  TEST_ASSERT_NOT_EQUAL(0, ssrc_a);
  TEST_ASSERT_NOT_EQUAL(0, ssrc_b);
  TEST_ASSERT_NOT_EQUAL(0, ssrc_c);
  TEST_ASSERT_TRUE(ssrc_a != ssrc_b);
  TEST_ASSERT_TRUE(ssrc_a != ssrc_c);
  TEST_ASSERT_TRUE(ssrc_b != ssrc_c);

  rtp_session_destroy(session_c);
  rtp_session_destroy(session_b);
  rtp_session_destroy(session_a);
}

void test_rtp_session_send_sequence_increment(void) {
  rtp_session_config_t config = {
      .ssrc = 0x12345678, .payload_type = 111, .clock_rate = 48000, .is_audio = 1};

  rtp_session_t *session = rtp_session_create(&config);
  TEST_ASSERT_NOT_NULL(session);

  uint8_t payload[] = {0x01, 0x02, 0x03};
  uint8_t buffer1[256], buffer2[256];
  rtp_packet_t pkt1, pkt2;

  /* Send first packet */
  int len1 =
      rtp_session_send(session, payload, sizeof(payload), 0, &pkt1, buffer1, sizeof(buffer1));
  TEST_ASSERT_GREATER_THAN(0, len1);

  /* Send second packet */
  int len2 =
      rtp_session_send(session, payload, sizeof(payload), 0, &pkt2, buffer2, sizeof(buffer2));
  TEST_ASSERT_GREATER_THAN(0, len2);

  /* Verify sequence increment */
  TEST_ASSERT_EQUAL_UINT16(pkt1.header.sequence + 1, pkt2.header.sequence);

  rtp_session_destroy(session);
}

void test_rtp_session_send_with_marker(void) {
  rtp_session_config_t config = {
      .ssrc = 0xCAFEBABE, .payload_type = 96, .clock_rate = 90000, .is_audio = 0};

  rtp_session_t *session = rtp_session_create(&config);
  TEST_ASSERT_NOT_NULL(session);

  uint8_t payload[] = {0xFF};
  uint8_t buffer[256];
  rtp_packet_t pkt;

  int len = rtp_session_send(session, payload, sizeof(payload), 1, &pkt, buffer, sizeof(buffer));
  TEST_ASSERT_GREATER_THAN(0, len);
  TEST_ASSERT_EQUAL_UINT8(1, pkt.header.marker);

  rtp_session_destroy(session);
}

void test_rtp_session_stats(void) {
  rtp_session_config_t config = {
      .ssrc = 0x11223344, .payload_type = 111, .clock_rate = 48000, .is_audio = 1};

  rtp_session_t *session = rtp_session_create(&config);
  TEST_ASSERT_NOT_NULL(session);

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

  TEST_ASSERT_EQUAL_UINT64(5, stats.packets_sent);
  TEST_ASSERT_EQUAL_UINT64(20, stats.octets_sent); /* 5 * 4 bytes */

  rtp_session_destroy(session);
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
  TEST_ASSERT_EQUAL_INT(0, result);

  size_t total_len = rtcp_compound_finish(&compound);
  TEST_ASSERT_GREATER_THAN(0, total_len);

  /* Verify RTCP header */
  TEST_ASSERT_EQUAL_UINT8(0x80, buffer[0] & 0xC0); /* Version 2 */
  TEST_ASSERT_EQUAL_UINT8(200, buffer[1]);         /* PT = SR */
}

void test_rtcp_compound_nack(void) {
  uint8_t buffer[256];

  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  /* NACK for sequence 100 with bitmap 0x0005 (also missing 101, 103) */
  int result = rtcp_compound_add_nack(&compound, 0xAABBCCDD, 0x11223344, 100, 0x0005);
  TEST_ASSERT_EQUAL_INT(0, result);

  size_t total_len = rtcp_compound_finish(&compound);
  TEST_ASSERT_GREATER_THAN(0, total_len);
}

void test_rtcp_compound_pli(void) {
  uint8_t buffer[256];

  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  int result = rtcp_compound_add_pli(&compound, 0xDEADBEEF, 0xCAFEBABE);
  TEST_ASSERT_EQUAL_INT(0, result);

  size_t total_len = rtcp_compound_finish(&compound);
  TEST_ASSERT_EQUAL_size_t(12, total_len); /* Fixed size for PLI */
}

void test_rtcp_compound_fir(void) {
  uint8_t buffer[256];

  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  int result = rtcp_compound_add_fir(&compound, 0x11111111, 0x22222222, 5);
  TEST_ASSERT_EQUAL_INT(0, result);

  size_t total_len = rtcp_compound_finish(&compound);
  TEST_ASSERT_GREATER_THAN(0, total_len);
}

void test_rtcp_compound_remb(void) {
  uint8_t buffer[256];

  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  int result = rtcp_compound_add_remb(&compound, 0xAAAAAAAA, 0xBBBBBBBB, 2000000); /* 2 Mbps */
  TEST_ASSERT_EQUAL_INT(0, result);

  size_t total_len = rtcp_compound_finish(&compound);
  TEST_ASSERT_EQUAL_size_t(24, total_len);
  TEST_ASSERT_EQUAL_UINT8((uint8_t)((2u << 6) | RTCP_REMB), buffer[0]);
  TEST_ASSERT_EQUAL_UINT8(RTCP_PSFB, buffer[1]);
  TEST_ASSERT_EQUAL_UINT16(5u, test_read_be16(buffer + 2));
  TEST_ASSERT_EQUAL_UINT32(0x52454d42u, test_read_be32(buffer + 12));
  TEST_ASSERT_EQUAL_UINT8(1u, buffer[16]);
  TEST_ASSERT_EQUAL_UINT32(0xBBBBBBBBu, test_read_be32(buffer + 20));
}

/* =============================================================================
 * Sequence Number Utility Tests
 * ============================================================================= */

void test_rtp_seq_newer(void) {
  /* Normal case */
  TEST_ASSERT_TRUE(rtp_seq_newer(100, 99));
  TEST_ASSERT_FALSE(rtp_seq_newer(99, 100));

  /* Wraparound case */
  TEST_ASSERT_TRUE(rtp_seq_newer(1, 65535));
  TEST_ASSERT_FALSE(rtp_seq_newer(65535, 1));

  /* Equal */
  TEST_ASSERT_FALSE(rtp_seq_newer(50, 50));
}

void test_rtp_seq_diff(void) {
  /* Normal case */
  TEST_ASSERT_EQUAL_INT(1, rtp_seq_diff(100, 99));
  TEST_ASSERT_EQUAL_INT(-1, rtp_seq_diff(99, 100));

  /* Wraparound case */
  TEST_ASSERT_EQUAL_INT(2, rtp_seq_diff(1, 65535));
  TEST_ASSERT_EQUAL_INT(-2, rtp_seq_diff(65535, 1));
}

/* =============================================================================
 * Main
 * ============================================================================= */

spec("test_rtp") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  /* RTP Build */
  TT_TEST(test_rtp_packet_build_basic);
  TT_TEST(test_rtp_packet_build_no_marker);

  /* RTP Parse */
  TT_TEST(test_rtp_packet_parse_basic);
  TT_TEST(test_rtp_packet_parse_too_short);
  TT_TEST(test_rtp_packet_parse_invalid_version);
  TT_TEST(test_rtp_packet_roundtrip);
  TT_TEST(test_rtp_packet_extension_wire_roundtrip);
  TT_TEST(test_rtp_packet_serialize_clears_empty_extension_bit);
  TT_TEST(test_rtp_packet_parse_stops_at_reserved_extension_id);

  /* RTP Session */
  TT_TEST(test_rtp_session_create_destroy);
  TT_TEST(test_rtp_session_auto_generated_ssrcs_are_unique);
  TT_TEST(test_rtp_session_send_sequence_increment);
  TT_TEST(test_rtp_session_send_with_marker);
  TT_TEST(test_rtp_session_stats);

  /* RTCP */
  TT_TEST(test_rtcp_compound_sr);
  TT_TEST(test_rtcp_compound_nack);
  TT_TEST(test_rtcp_compound_pli);
  TT_TEST(test_rtcp_compound_fir);
  TT_TEST(test_rtcp_compound_remb);

  /* Sequence utilities */
  TT_TEST(test_rtp_seq_newer);
  TT_TEST(test_rtp_seq_diff);
}
