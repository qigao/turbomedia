/**
 * Unit tests for NACK (Negative Acknowledgment)
 */
#include "tinytest_compat.h"
#include "turbo_nack.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Test NACK sender creation */
void test_nack_sender_create(void) {
  nack_sender_t *sender = nack_sender_create(0x12345678, 100);
  TEST_ASSERT_NOT_NULL(sender);
  nack_sender_destroy(sender);
}

/* Test NACK receiver creation */
void test_nack_receiver_create(void) {
  nack_receiver_t *receiver = nack_receiver_create(0xAABBCCDD);
  TEST_ASSERT_NOT_NULL(receiver);
  nack_receiver_destroy(receiver);
}

/* Test adding packets to sender history */
void test_nack_sender_add_packet(void) {
  nack_sender_t *sender = nack_sender_create(0x11111111, 50);
  TEST_ASSERT_NOT_NULL(sender);

  uint8_t packet[100];
  memset(packet, 0xAA, sizeof(packet));

  for (uint16_t i = 0; i < 10; i++) {
    nack_sender_add_packet(sender, i, packet, sizeof(packet));
  }

  nack_sender_destroy(sender);
}

/* Test NACK receiver detecting loss */
void test_nack_receiver_detect_loss(void) {
  nack_receiver_t *receiver = nack_receiver_create(0x22222222);
  TEST_ASSERT_NOT_NULL(receiver);

  /* Receive packets with gap */
  nack_receiver_process_packet(receiver, 100, 0);
  nack_receiver_process_packet(receiver, 101, 10000);
  /* Skip 102 (lost) */
  nack_receiver_process_packet(receiver, 103, 30000);

  /* Should detect missing packet 102 */
  uint16_t missing[16];
  int count = nack_receiver_get_nacks(receiver, missing, 16, 40000);

  TEST_ASSERT_GREATER_THAN(0, count);
  TEST_ASSERT_EQUAL_UINT16(102, missing[0]);

  nack_receiver_destroy(receiver);
}

/* Test NACK sender statistics */
void test_nack_sender_stats(void) {
  nack_sender_t *sender = nack_sender_create(0x33333333, 100);
  TEST_ASSERT_NOT_NULL(sender);

  int64_t nacks_received = -1;
  int64_t packets_retransmitted = -1;
  int64_t retransmit_failures = -1;

  nack_sender_get_stats(sender, &nacks_received, &packets_retransmitted, &retransmit_failures);

  TEST_ASSERT_EQUAL_INT64(0, nacks_received);
  TEST_ASSERT_EQUAL_INT64(0, packets_retransmitted);
  TEST_ASSERT_EQUAL_INT64(0, retransmit_failures);

  nack_sender_destroy(sender);
}

/* Test NACK receiver statistics */
void test_nack_receiver_stats(void) {
  nack_receiver_t *receiver = nack_receiver_create(0x44444444);
  TEST_ASSERT_NOT_NULL(receiver);

  int64_t packets_received = -1;
  int64_t packets_lost = -1;
  int64_t nacks_sent = -1;
  int64_t retransmissions_received = -1;
  int missing_count = -1;

  nack_receiver_get_stats(receiver, &packets_received, &packets_lost, &nacks_sent,
                          &retransmissions_received, &missing_count);

  TEST_ASSERT_EQUAL_INT64(0, packets_received);
  TEST_ASSERT_EQUAL_INT64(0, packets_lost);
  TEST_ASSERT_EQUAL_INT64(0, nacks_sent);
  TEST_ASSERT_EQUAL_INT64(0, retransmissions_received);
  TEST_ASSERT_EQUAL_INT(0, missing_count);

  nack_receiver_destroy(receiver);
}

/* Test NACK RTCP packet building */
void test_nack_build_rtcp(void) {
  uint16_t seq_nums[] = {100, 101, 103};
  uint8_t buffer[256];
  size_t len = sizeof(buffer);

  int result = nack_build_rtcp(0x11111111, 0x22222222, seq_nums, 3, buffer, &len);

  TEST_ASSERT_EQUAL_INT(0, result);
  TEST_ASSERT_GREATER_THAN(0, len);

  /* Verify RTCP header */
  TEST_ASSERT_EQUAL_UINT8(0x81, buffer[0]); /* V=2, P=0, FMT=1 */
  TEST_ASSERT_EQUAL_UINT8(205, buffer[1]);  /* PT=205 (RTPFB) */
}

/* Test NACK RTCP packet parsing */
void test_nack_parse_rtcp(void) {
  /* Build a NACK packet first */
  uint16_t seq_nums_out[] = {200, 201, 202};
  uint8_t buffer[256];
  size_t len = sizeof(buffer);

  nack_build_rtcp(0xAAAAAAAA, 0xBBBBBBBB, seq_nums_out, 3, buffer, &len);

  /* Parse it back */
  uint32_t sender_ssrc, media_ssrc;
  uint16_t seq_nums_in[16];
  int count;

  int result = nack_parse_rtcp(buffer, len, &sender_ssrc, &media_ssrc, seq_nums_in, &count, 16);

  TEST_ASSERT_EQUAL_INT(0, result);
  TEST_ASSERT_EQUAL_UINT32(0xAAAAAAAA, sender_ssrc);
  TEST_ASSERT_EQUAL_UINT32(0xBBBBBBBB, media_ssrc);
  TEST_ASSERT_GREATER_THAN(0, count);
}

spec("test_nack") {
  before_each() { setUp(); }
  after_each() { tearDown(); }
  TT_TEST(test_nack_sender_create);
  TT_TEST(test_nack_receiver_create);
  TT_TEST(test_nack_sender_add_packet);
  TT_TEST(test_nack_receiver_detect_loss);
  TT_TEST(test_nack_sender_stats);
  TT_TEST(test_nack_receiver_stats);
  TT_TEST(test_nack_build_rtcp);
  TT_TEST(test_nack_parse_rtcp);
}
