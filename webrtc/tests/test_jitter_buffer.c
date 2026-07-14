/**
 * Jitter Buffer Tests
 */
#include "jitter_buffer.h"
#include "tinytest_compat.h"
#include "turbo_rtp.h"
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* =============================================================================
 * Helper Functions
 * ============================================================================= */

static rtp_packet_t make_packet(uint16_t seq, uint32_t ts, const uint8_t *payload, size_t len) {
  rtp_packet_t pkt = {
      .header = {.version = 2, .sequence = seq, .timestamp = ts, .ssrc = 0x12345678},
      .payload = (uint8_t *)payload,
      .payload_len = len};
  return pkt;
}

/* =============================================================================
 * Creation/Destruction Tests
 * ============================================================================= */

void test_jitter_buffer_create(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 50, 1500);
  TEST_ASSERT_NOT_NULL(jb);
  TEST_ASSERT_EQUAL_UINT32(50, jitter_buffer_get_delay(jb));
  TEST_ASSERT_TRUE(jitter_buffer_is_empty(jb));
  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_create_null_on_zero(void) {
  jitter_buffer_t *jb = jitter_buffer_create(0, 50, 1500);
  /* Should still create with clock_rate = 0 */
  if (jb) {
    jitter_buffer_destroy(jb);
  }
}

/* =============================================================================
 * Put/Get Tests
 * ============================================================================= */

void test_jitter_buffer_put_single(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 20, 1500);
  TEST_ASSERT_NOT_NULL(jb);

  uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
  rtp_packet_t pkt = make_packet(1, 960, payload, sizeof(payload));

  int result = jitter_buffer_put(jb, &pkt, 0);
  TEST_ASSERT_EQUAL_INT(0, result);
  TEST_ASSERT_EQUAL_INT(1, jitter_buffer_get_count(jb));

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_put_get_sequence(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 0, 1500); /* No delay */
  TEST_ASSERT_NOT_NULL(jb);

  uint8_t payload1[] = {0x11, 0x22};
  uint8_t payload2[] = {0x33, 0x44};
  uint8_t payload3[] = {0x55, 0x66};

  rtp_packet_t pkt1 = make_packet(100, 960, payload1, sizeof(payload1));
  rtp_packet_t pkt2 = make_packet(101, 1920, payload2, sizeof(payload2));
  rtp_packet_t pkt3 = make_packet(102, 2880, payload3, sizeof(payload3));

  /* Insert in order */
  jitter_buffer_put(jb, &pkt1, 0);
  jitter_buffer_put(jb, &pkt2, 10);
  jitter_buffer_put(jb, &pkt3, 20);

  TEST_ASSERT_EQUAL_INT(3, jitter_buffer_get_count(jb));

  /* Get packets */
  uint8_t out[256];
  size_t out_len;
  uint32_t ts;

  int result = jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 100);
  TEST_ASSERT_EQUAL_INT(1, result);
  TEST_ASSERT_EQUAL_size_t(2, out_len);
  TEST_ASSERT_EQUAL_MEMORY(payload1, out, out_len);

  result = jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 110);
  TEST_ASSERT_EQUAL_INT(1, result);
  TEST_ASSERT_EQUAL_MEMORY(payload2, out, out_len);

  result = jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 120);
  TEST_ASSERT_EQUAL_INT(1, result);
  TEST_ASSERT_EQUAL_MEMORY(payload3, out, out_len);

  TEST_ASSERT_TRUE(jitter_buffer_is_empty(jb));

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_get_ex_returns_marker(void) {
  jitter_buffer_t *jb = jitter_buffer_create(90000, 0, 1500);
  TEST_ASSERT_NOT_NULL(jb);

  uint8_t payload[] = {0x99, 0x88};
  rtp_packet_t pkt = make_packet(321, 90000, payload, sizeof(payload));
  pkt.header.marker = 1;

  TEST_ASSERT_EQUAL_INT(0, jitter_buffer_put(jb, &pkt, 0));

  uint8_t out[256];
  size_t out_len = 0;
  uint32_t ts = 0;
  int marker = 0;

  TEST_ASSERT_EQUAL_INT(1, jitter_buffer_get_ex(jb, out, sizeof(out), &out_len, &ts, &marker, 0));
  TEST_ASSERT_EQUAL_size_t(sizeof(payload), out_len);
  TEST_ASSERT_EQUAL_MEMORY(payload, out, out_len);
  TEST_ASSERT_EQUAL_UINT32(90000, ts);
  TEST_ASSERT_EQUAL_INT(1, marker);

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_reorder(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 0, 1500);
  TEST_ASSERT_NOT_NULL(jb);

  uint8_t payload1[] = {0xAA};
  uint8_t payload2[] = {0xBB};
  uint8_t payload3[] = {0xCC};

  rtp_packet_t pkt1 = make_packet(200, 960, payload1, 1);
  rtp_packet_t pkt2 = make_packet(201, 1920, payload2, 1);
  rtp_packet_t pkt3 = make_packet(202, 2880, payload3, 1);

  /* Insert out of order: 1, 3, 2 */
  jitter_buffer_put(jb, &pkt1, 0);
  jitter_buffer_put(jb, &pkt3, 5);
  jitter_buffer_put(jb, &pkt2, 10);

  /* Should get in order: 1, 2, 3 */
  uint8_t out[256];
  size_t out_len;
  uint32_t ts;

  jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 100);
  TEST_ASSERT_EQUAL_UINT8(0xAA, out[0]);

  jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 110);
  TEST_ASSERT_EQUAL_UINT8(0xBB, out[0]);

  jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 120);
  TEST_ASSERT_EQUAL_UINT8(0xCC, out[0]);

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_duplicate(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 20, 1500);
  TEST_ASSERT_NOT_NULL(jb);

  uint8_t payload[] = {0x12, 0x34};
  rtp_packet_t pkt = make_packet(500, 960, payload, sizeof(payload));

  int result1 = jitter_buffer_put(jb, &pkt, 0);
  TEST_ASSERT_EQUAL_INT(0, result1);

  int result2 = jitter_buffer_put(jb, &pkt, 5);
  TEST_ASSERT_EQUAL_INT(1, result2); /* Duplicate returns 1 */

  TEST_ASSERT_EQUAL_INT(1, jitter_buffer_get_count(jb));

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_late_packet(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 0, 1500);
  TEST_ASSERT_NOT_NULL(jb);

  /* Insert packets starting at seq 200 (high enough that seq 50 is >128 behind) */
  uint8_t payload[] = {0xFF};
  for (int i = 0; i < 10; i++) {
    rtp_packet_t pkt = make_packet(200 + i, 960 * i, payload, 1);
    jitter_buffer_put(jb, &pkt, i * 10);
  }

  /* Read some packets to advance next_seq_out to 205 */
  uint8_t out[256];
  size_t out_len;
  uint32_t ts;
  for (int i = 0; i < 5; i++) {
    jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 500);
  }

  /* Now try to insert a very old packet (seq 50)
   * diff = seq_diff(50, 205) = -155, which is < -128 (JITTER_BUFFER_SIZE/2)
   * So this should be rejected as "too late" */
  rtp_packet_t late_pkt = make_packet(50, 0, payload, 1);
  int result = jitter_buffer_put(jb, &late_pkt, 600);
  TEST_ASSERT_EQUAL_INT(-1, result); /* Too late */

  jitter_buffer_destroy(jb);
}

/* =============================================================================
 * Delay Tests
 * ============================================================================= */

void test_jitter_buffer_delay_enforcement(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 50, 1500); /* 50ms delay */
  TEST_ASSERT_NOT_NULL(jb);

  uint8_t payload[] = {0x01};
  rtp_packet_t pkt = make_packet(1, 960, payload, 1);

  jitter_buffer_put(jb, &pkt, 0);

  uint8_t out[256];
  size_t out_len;
  uint32_t ts;

  /* Try to get before delay has passed */
  int result = jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 30);
  TEST_ASSERT_EQUAL_INT(0, result); /* Not ready */

  /* Wait for delay */
  result = jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 60);
  TEST_ASSERT_EQUAL_INT(1, result); /* Ready now */

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_set_delay_bounds(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 50, 1500);
  TEST_ASSERT_NOT_NULL(jb);

  jitter_buffer_set_delay_bounds(jb, 20, 200);
  TEST_ASSERT_EQUAL_UINT32(50, jitter_buffer_get_delay(jb)); /* Within bounds */

  /* Create new buffer with delay outside bounds */
  jitter_buffer_t *jb2 = jitter_buffer_create(48000, 10, 1500);
  jitter_buffer_set_delay_bounds(jb2, 20, 200);
  TEST_ASSERT_EQUAL_UINT32(20, jitter_buffer_get_delay(jb2)); /* Clamped to min */

  jitter_buffer_destroy(jb);
  jitter_buffer_destroy(jb2);
}

/* =============================================================================
 * Missing Packet Detection Tests
 * ============================================================================= */

void test_jitter_buffer_get_missing(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 0, 1500);
  TEST_ASSERT_NOT_NULL(jb);

  uint8_t payload[] = {0x01};

  /* Insert packets with gaps: 100, 102, 104 (missing 101, 103) */
  rtp_packet_t pkt100 = make_packet(100, 960, payload, 1);
  rtp_packet_t pkt102 = make_packet(102, 2880, payload, 1);
  rtp_packet_t pkt104 = make_packet(104, 4800, payload, 1);

  jitter_buffer_put(jb, &pkt100, 0);
  jitter_buffer_put(jb, &pkt102, 20);
  jitter_buffer_put(jb, &pkt104, 40);

  /* Get missing sequence numbers */
  uint16_t missing[16];
  int count = jitter_buffer_get_missing(jb, missing, 16);

  TEST_ASSERT_EQUAL_INT(2, count);
  TEST_ASSERT_EQUAL_UINT16(101, missing[0]);
  TEST_ASSERT_EQUAL_UINT16(103, missing[1]);

  jitter_buffer_destroy(jb);
}

/* =============================================================================
 * Reset Test
 * ============================================================================= */

void test_jitter_buffer_reset(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 20, 1500);
  TEST_ASSERT_NOT_NULL(jb);

  uint8_t payload[] = {0x01, 0x02};
  for (int i = 0; i < 5; i++) {
    rtp_packet_t pkt = make_packet(i, i * 960, payload, sizeof(payload));
    jitter_buffer_put(jb, &pkt, i * 10);
  }

  TEST_ASSERT_EQUAL_INT(5, jitter_buffer_get_count(jb));

  jitter_buffer_reset(jb);

  TEST_ASSERT_EQUAL_INT(0, jitter_buffer_get_count(jb));
  TEST_ASSERT_TRUE(jitter_buffer_is_empty(jb));

  jitter_buffer_destroy(jb);
}

/* =============================================================================
 * Peek Test
 * ============================================================================= */

void test_jitter_buffer_peek(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 0, 1500);
  TEST_ASSERT_NOT_NULL(jb);

  uint8_t payload[] = {0xAB};
  rtp_packet_t pkt = make_packet(42, 12345, payload, 1);
  jitter_buffer_put(jb, &pkt, 0);

  uint32_t ts;
  int result = jitter_buffer_peek(jb, &ts);
  TEST_ASSERT_EQUAL_INT(1, result);
  TEST_ASSERT_EQUAL_UINT32(12345, ts);

  /* Packet should still be there */
  TEST_ASSERT_EQUAL_INT(1, jitter_buffer_get_count(jb));

  jitter_buffer_destroy(jb);
}

/* =============================================================================
 * Main
 * ============================================================================= */

spec("test_jitter_buffer") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  /* Creation/Destruction */
  TT_TEST(test_jitter_buffer_create);
  TT_TEST(test_jitter_buffer_create_null_on_zero);

  /* Put/Get */
  TT_TEST(test_jitter_buffer_put_single);
  TT_TEST(test_jitter_buffer_put_get_sequence);
  TT_TEST(test_jitter_buffer_get_ex_returns_marker);
  TT_TEST(test_jitter_buffer_reorder);
  TT_TEST(test_jitter_buffer_duplicate);
  TT_TEST(test_jitter_buffer_late_packet);

  /* Delay */
  TT_TEST(test_jitter_buffer_delay_enforcement);
  TT_TEST(test_jitter_buffer_set_delay_bounds);

  /* Missing Packets */
  TT_TEST(test_jitter_buffer_get_missing);

  /* Reset */
  TT_TEST(test_jitter_buffer_reset);

  /* Peek */
  TT_TEST(test_jitter_buffer_peek);
}
