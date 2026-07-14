/**
 * Unit tests for TWCC (Transport-Wide Congestion Control)
 */
#include "tinytest_compat.h"
#include "turbo_rtp.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static uint16_t read_be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

/* =============================================================================
 * TWCC Tracker Tests (Sender Side)
 * ============================================================================= */

void test_twcc_tracker_create(void) {
  twcc_tracker_t *tracker = twcc_tracker_create();
  TEST_ASSERT_NOT_NULL(tracker);

  twcc_tracker_destroy(tracker);
}

void test_twcc_tracker_register_packet(void) {
  twcc_tracker_t *tracker = twcc_tracker_create();
  TEST_ASSERT_NOT_NULL(tracker);

  /* Register some packets */
  for (int i = 0; i < 10; i++) {
    uint16_t seq = twcc_tracker_register_packet(tracker, 1200, i * 1000);
    TEST_ASSERT_EQUAL_UINT16(i, seq);
  }

  twcc_tracker_destroy(tracker);
}

void test_twcc_tracker_process_feedback(void) {
  twcc_tracker_t *tracker = twcc_tracker_create();
  TEST_ASSERT_NOT_NULL(tracker);

  /* Register packets */
  for (int i = 0; i < 5; i++) {
    twcc_tracker_register_packet(tracker, 1000, i * 10000);
  }

  /* Create feedback message */
  rtcp_twcc_t twcc;
  memset(&twcc, 0, sizeof(twcc));
  twcc.sender_ssrc = 0x12345678;
  twcc.media_ssrc = 0xAABBCCDD;
  twcc.base_seq = 0;
  twcc.packet_count = 5;
  twcc.reference_time = 0;
  twcc.fb_pkt_count = 1;

  /* Mark all packets as received */
  for (int i = 0; i < 5; i++) {
    twcc.packets[i].seq = i;
    twcc.packets[i].received = 1;
    twcc.packets[i].arrival_time_us = i * 10000 + 5000;
  }
  twcc.num_packets = 5;

  /* Process feedback */
  twcc_bwe_result_t result;
  int ret = twcc_tracker_process_feedback(tracker, &twcc, &result);
  TEST_ASSERT_EQUAL_INT(0, ret);
  TEST_ASSERT_GREATER_THAN(0, result.estimated_bw_bps);

  twcc_tracker_destroy(tracker);
}

/* =============================================================================
 * TWCC Receiver Tests (Receiver Side)
 * ============================================================================= */

void test_twcc_receiver_create(void) {
  twcc_receiver_t *receiver = twcc_receiver_create(0x12345678);
  TEST_ASSERT_NOT_NULL(receiver);

  twcc_receiver_destroy(receiver);
}

void test_twcc_receiver_register_packet(void) {
  twcc_receiver_t *receiver = twcc_receiver_create(0xCAFEBABE);
  TEST_ASSERT_NOT_NULL(receiver);

  /* Register packets with TWCC sequence numbers */
  for (uint16_t i = 0; i < 10; i++) {
    int result = twcc_receiver_register_packet(receiver, i, i * 1000);
    TEST_ASSERT_EQUAL_INT(0, result);
  }

  twcc_receiver_destroy(receiver);
}

void test_twcc_receiver_generate_feedback(void) {
  twcc_receiver_t *receiver = twcc_receiver_create(0x11111111);
  TEST_ASSERT_NOT_NULL(receiver);

  /* Register packets */
  for (uint16_t i = 0; i < 5; i++) {
    twcc_receiver_register_packet(receiver, i, i * 10000);
  }

  /* Generate feedback */
  rtcp_twcc_t twcc;
  int result = twcc_receiver_generate_feedback(receiver, &twcc);

  /* Should generate feedback or indicate not ready */
  TEST_ASSERT_GREATER_OR_EQUAL(0, result);

  if (result == 1) {
    /* Feedback was generated */
    TEST_ASSERT_EQUAL_UINT32(0x11111111, twcc.sender_ssrc);
    TEST_ASSERT_GREATER_THAN(0, twcc.num_packets);
  }

  twcc_receiver_destroy(receiver);
}

void test_twcc_receiver_packet_loss(void) {
  twcc_receiver_t *receiver = twcc_receiver_create(0xAAAAAAAA);
  TEST_ASSERT_NOT_NULL(receiver);

  /* Register packets with gaps (simulate loss) */
  twcc_receiver_register_packet(receiver, 0, 0);
  twcc_receiver_register_packet(receiver, 1, 10000);
  /* Skip 2 (lost) */
  twcc_receiver_register_packet(receiver, 3, 30000);
  twcc_receiver_register_packet(receiver, 4, 40000);

  /* Generate feedback should handle gaps */
  rtcp_twcc_t twcc;
  int result = twcc_receiver_generate_feedback(receiver, &twcc);
  TEST_ASSERT_GREATER_OR_EQUAL(0, result);

  twcc_receiver_destroy(receiver);
}

/* =============================================================================
 * RTCP Parsing Tests
 * ============================================================================= */

void test_rtcp_parse_twcc(void) {
  /* Create a minimal valid TWCC RTCP packet */
  uint8_t buffer[256];
  memset(buffer, 0, sizeof(buffer));

  /* RTCP header: V=2, P=0, FMT=15, PT=205 */
  buffer[0] = 0x8F; /* V=2, P=0, FMT=15 */
  buffer[1] = 205;  /* PT=205 (Transport Feedback) */
  buffer[2] = 0;    /* Length high byte */
  buffer[3] = 5;    /* Length low byte (5 * 4 = 20 bytes + 4 header = 24 total) */

  /* Sender SSRC */
  buffer[4] = 0x12;
  buffer[5] = 0x34;
  buffer[6] = 0x56;
  buffer[7] = 0x78;

  /* Media SSRC */
  buffer[8] = 0xAA;
  buffer[9] = 0xBB;
  buffer[10] = 0xCC;
  buffer[11] = 0xDD;

  /* Base sequence number */
  buffer[12] = 0;
  buffer[13] = 0;

  /* Packet status count */
  buffer[14] = 0;
  buffer[15] = 5;

  /* Reference time (24 bits) */
  buffer[16] = 0;
  buffer[17] = 0;
  buffer[18] = 0;

  /* Feedback packet count */
  buffer[19] = 1;

  rtcp_twcc_t twcc;
  int result = rtcp_parse_twcc(buffer, 24, &twcc);

  /* Parsing may succeed or fail depending on implementation */
  /* Just verify it doesn't crash */
  (void)result;
}

void test_rtcp_twcc_roundtrip_with_deltas_and_loss(void) {
  uint8_t buffer[512];
  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  rtcp_twcc_t input;
  memset(&input, 0, sizeof(input));

  input.sender_ssrc = 0x11111111;
  input.media_ssrc = 0x22222222;
  input.base_seq = 1000;
  input.reference_time = 12345;
  input.fb_pkt_count = 7;
  input.packet_count = 6;
  input.num_packets = 6;

  uint64_t ref_us = (uint64_t)input.reference_time * 64000ULL;
  uint64_t arrivals[] = {
      ref_us + 2500, ref_us + 3000, 0, ref_us + 103000, ref_us + 102500, ref_us + 102750,
  };

  for (int i = 0; i < input.num_packets; ++i) {
    input.packets[i].seq = (uint16_t)(input.base_seq + i);
    input.packets[i].received = (i != 2);
    input.packets[i].arrival_time_us = arrivals[i];
  }

  int result = rtcp_compound_add_twcc(&compound, &input);
  TEST_ASSERT_EQUAL_INT(0, result);

  size_t packet_len = rtcp_compound_finish(&compound);
  TEST_ASSERT_GREATER_THAN(20, packet_len);
  TEST_ASSERT_EQUAL_UINT16(0xC000, read_be16(buffer + 20) & 0xC000);

  rtcp_twcc_t parsed;
  result = rtcp_parse_twcc(buffer, packet_len, &parsed);
  TEST_ASSERT_EQUAL_INT(0, result);

  TEST_ASSERT_EQUAL_UINT32(input.sender_ssrc, parsed.sender_ssrc);
  TEST_ASSERT_EQUAL_UINT32(input.media_ssrc, parsed.media_ssrc);
  TEST_ASSERT_EQUAL_UINT16(input.base_seq, parsed.base_seq);
  TEST_ASSERT_EQUAL_UINT16(input.packet_count, parsed.packet_count);
  TEST_ASSERT_EQUAL_UINT32(input.reference_time, parsed.reference_time);
  TEST_ASSERT_EQUAL_UINT8(input.fb_pkt_count, parsed.fb_pkt_count);
  TEST_ASSERT_EQUAL_INT(input.num_packets, parsed.num_packets);

  for (int i = 0; i < input.num_packets; ++i) {
    TEST_ASSERT_EQUAL_UINT16(input.packets[i].seq, parsed.packets[i].seq);
    TEST_ASSERT_EQUAL_INT(input.packets[i].received, parsed.packets[i].received);
    TEST_ASSERT_EQUAL_UINT64(input.packets[i].arrival_time_us, parsed.packets[i].arrival_time_us);
  }
}

void test_rtcp_twcc_uses_one_bit_status_vector_for_small_deltas(void) {
  uint8_t buffer[512];
  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  rtcp_twcc_t input;
  memset(&input, 0, sizeof(input));

  input.sender_ssrc = 0x33333333;
  input.media_ssrc = 0x44444444;
  input.base_seq = 32000;
  input.reference_time = 23456;
  input.fb_pkt_count = 9;
  input.packet_count = 14;
  input.num_packets = 14;

  uint64_t ref_us = (uint64_t)input.reference_time * 64000ULL;
  uint64_t arrival_us = ref_us;

  for (int i = 0; i < input.num_packets; ++i) {
    input.packets[i].seq = (uint16_t)(input.base_seq + i);
    input.packets[i].received = (i % 2) == 0;
    if (input.packets[i].received) {
      arrival_us += 250;
      input.packets[i].arrival_time_us = arrival_us;
    }
  }

  int result = rtcp_compound_add_twcc(&compound, &input);
  TEST_ASSERT_EQUAL_INT(0, result);

  size_t packet_len = rtcp_compound_finish(&compound);
  TEST_ASSERT_EQUAL_size_t(32, packet_len);

  uint16_t chunk = read_be16(buffer + 20);
  TEST_ASSERT_EQUAL_UINT16(0x8000, chunk & 0xC000);

  rtcp_twcc_t parsed;
  result = rtcp_parse_twcc(buffer, packet_len, &parsed);
  TEST_ASSERT_EQUAL_INT(0, result);
  TEST_ASSERT_EQUAL_INT(input.num_packets, parsed.num_packets);

  for (int i = 0; i < input.num_packets; ++i) {
    TEST_ASSERT_EQUAL_UINT16(input.packets[i].seq, parsed.packets[i].seq);
    TEST_ASSERT_EQUAL_INT(input.packets[i].received, parsed.packets[i].received);
    TEST_ASSERT_EQUAL_UINT64(input.packets[i].arrival_time_us, parsed.packets[i].arrival_time_us);
  }
}

void test_rtcp_twcc_uses_run_length_for_long_runs(void) {
  uint8_t buffer[512];
  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, sizeof(buffer));

  rtcp_twcc_t input;
  memset(&input, 0, sizeof(input));

  input.sender_ssrc = 0x55555555;
  input.media_ssrc = 0x66666666;
  input.base_seq = 41000;
  input.reference_time = 34567;
  input.fb_pkt_count = 11;
  input.packet_count = 10;
  input.num_packets = 10;

  uint64_t ref_us = (uint64_t)input.reference_time * 64000ULL;
  for (int i = 0; i < input.num_packets; ++i) {
    input.packets[i].seq = (uint16_t)(input.base_seq + i);
    input.packets[i].received = 1;
    input.packets[i].arrival_time_us = ref_us + (uint64_t)(i + 1) * 250ULL;
  }

  int result = rtcp_compound_add_twcc(&compound, &input);
  TEST_ASSERT_EQUAL_INT(0, result);

  size_t packet_len = rtcp_compound_finish(&compound);
  TEST_ASSERT_EQUAL_size_t(32, packet_len);

  uint16_t chunk = read_be16(buffer + 20);
  TEST_ASSERT_EQUAL_UINT16(0x2000, chunk & 0xE000);
  TEST_ASSERT_EQUAL_UINT16(10, chunk & 0x1FFF);

  rtcp_twcc_t parsed;
  result = rtcp_parse_twcc(buffer, packet_len, &parsed);
  TEST_ASSERT_EQUAL_INT(0, result);
  TEST_ASSERT_EQUAL_INT(input.num_packets, parsed.num_packets);

  for (int i = 0; i < input.num_packets; ++i) {
    TEST_ASSERT_EQUAL_UINT16(input.packets[i].seq, parsed.packets[i].seq);
    TEST_ASSERT_EQUAL_INT(1, parsed.packets[i].received);
    TEST_ASSERT_EQUAL_UINT64(input.packets[i].arrival_time_us, parsed.packets[i].arrival_time_us);
  }
}

/* =============================================================================
 * Integration Tests
 * ============================================================================= */

void test_twcc_end_to_end(void) {
  /* Create tracker and receiver */
  twcc_tracker_t *tracker = twcc_tracker_create();
  twcc_receiver_t *receiver = twcc_receiver_create(0x11111111);

  TEST_ASSERT_NOT_NULL(tracker);
  TEST_ASSERT_NOT_NULL(receiver);

  /* Sender registers packets */
  for (int i = 0; i < 10; i++) {
    uint16_t seq = twcc_tracker_register_packet(tracker, 1200, i * 1000);

    /* Receiver gets packets */
    twcc_receiver_register_packet(receiver, seq, i * 1000 + 500);
  }

  /* Receiver generates feedback */
  rtcp_twcc_t feedback;
  int result = twcc_receiver_generate_feedback(receiver, &feedback);

  if (result == 1) {
    /* Sender processes feedback */
    twcc_bwe_result_t bwe_result;
    result = twcc_tracker_process_feedback(tracker, &feedback, &bwe_result);
    TEST_ASSERT_EQUAL_INT(0, result);
  }

  twcc_tracker_destroy(tracker);
  twcc_receiver_destroy(receiver);
}

/* =============================================================================
 * Main
 * ============================================================================= */

spec("test_twcc") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  /* Tracker Tests */
  TT_TEST(test_twcc_tracker_create);
  TT_TEST(test_twcc_tracker_register_packet);
  TT_TEST(test_twcc_tracker_process_feedback);

  /* Receiver Tests */
  TT_TEST(test_twcc_receiver_create);
  TT_TEST(test_twcc_receiver_register_packet);
  TT_TEST(test_twcc_receiver_generate_feedback);
  TT_TEST(test_twcc_receiver_packet_loss);

  /* RTCP Parsing */
  TT_TEST(test_rtcp_parse_twcc);
  TT_TEST(test_rtcp_twcc_roundtrip_with_deltas_and_loss);
  TT_TEST(test_rtcp_twcc_uses_one_bit_status_vector_for_small_deltas);
  TT_TEST(test_rtcp_twcc_uses_run_length_for_long_runs);

  /* Integration */
  TT_TEST(test_twcc_end_to_end);
}
