/**
 * Jitter Buffer Tests
 */
#include "jitter_buffer.h"
#include "tinytest.h"
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
  check_not_null(jb);
  check_equal((uint32_t)(jitter_buffer_get_delay(jb)), (uint32_t)(50));
  check_true(jitter_buffer_is_empty(jb));
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
  check_not_null(jb);

  uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
  rtp_packet_t pkt = make_packet(1, 960, payload, sizeof(payload));

  int result = jitter_buffer_put(jb, &pkt, 0);
  check_equal((int)(result), (int)(0));
  check_equal((int)(jitter_buffer_get_count(jb)), (int)(1));

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_put_get_sequence(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 0, 1500); /* No delay */
  check_not_null(jb);

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

  check_equal((int)(jitter_buffer_get_count(jb)), (int)(3));

  /* Get packets */
  uint8_t out[256];
  size_t out_len;
  uint32_t ts;

  int result = jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 100);
  check_equal((int)(result), (int)(1));
  check_equal((size_t)(out_len), (size_t)(2));
  check_equal(out, payload1, out_len);

  result = jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 110);
  check_equal((int)(result), (int)(1));
  check_equal(out, payload2, out_len);

  result = jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 120);
  check_equal((int)(result), (int)(1));
  check_equal(out, payload3, out_len);

  check_true(jitter_buffer_is_empty(jb));

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_get_ex_returns_marker(void) {
  jitter_buffer_t *jb = jitter_buffer_create(90000, 0, 1500);
  check_not_null(jb);

  uint8_t payload[] = {0x99, 0x88};
  rtp_packet_t pkt = make_packet(321, 90000, payload, sizeof(payload));
  pkt.header.marker = 1;

  check_equal((int)(jitter_buffer_put(jb, &pkt, 0)), (int)(0));

  uint8_t out[256];
  size_t out_len = 0;
  uint32_t ts = 0;
  int marker = 0;

  check_equal((int)(jitter_buffer_get_ex(jb, out, sizeof(out), &out_len, &ts, &marker, 0)), (int)(1));
  check_equal((size_t)(out_len), (size_t)(sizeof(payload)));
  check_equal(out, payload, out_len);
  check_equal((uint32_t)(ts), (uint32_t)(90000));
  check_equal((int)(marker), (int)(1));

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_reorder(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 0, 1500);
  check_not_null(jb);

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
  check_equal((uint8_t)(out[0]), (uint8_t)(0xAA));

  jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 110);
  check_equal((uint8_t)(out[0]), (uint8_t)(0xBB));

  jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 120);
  check_equal((uint8_t)(out[0]), (uint8_t)(0xCC));

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_duplicate(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 20, 1500);
  check_not_null(jb);

  uint8_t payload[] = {0x12, 0x34};
  rtp_packet_t pkt = make_packet(500, 960, payload, sizeof(payload));

  int result1 = jitter_buffer_put(jb, &pkt, 0);
  check_equal((int)(result1), (int)(0));

  int result2 = jitter_buffer_put(jb, &pkt, 5);
  check_equal((int)(result2), (int)(1)); /* Duplicate returns 1 */

  check_equal((int)(jitter_buffer_get_count(jb)), (int)(1));

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_late_packet(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 0, 1500);
  check_not_null(jb);

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
  check_equal((int)(result), (int)(-1)); /* Too late */

  jitter_buffer_destroy(jb);
}

/* =============================================================================
 * Delay Tests
 * ============================================================================= */

void test_jitter_buffer_delay_enforcement(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 50, 1500); /* 50ms delay */
  check_not_null(jb);

  uint8_t payload[] = {0x01};
  rtp_packet_t pkt = make_packet(1, 960, payload, 1);

  jitter_buffer_put(jb, &pkt, 0);

  uint8_t out[256];
  size_t out_len;
  uint32_t ts;

  /* Try to get before delay has passed */
  int result = jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 30);
  check_equal((int)(result), (int)(0)); /* Not ready */

  /* Wait for delay */
  result = jitter_buffer_get(jb, out, sizeof(out), &out_len, &ts, 60);
  check_equal((int)(result), (int)(1)); /* Ready now */

  jitter_buffer_destroy(jb);
}

void test_jitter_buffer_set_delay_bounds(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 50, 1500);
  check_not_null(jb);

  jitter_buffer_set_delay_bounds(jb, 20, 200);
  check_equal((uint32_t)(jitter_buffer_get_delay(jb)), (uint32_t)(50)); /* Within bounds */

  /* Create new buffer with delay outside bounds */
  jitter_buffer_t *jb2 = jitter_buffer_create(48000, 10, 1500);
  jitter_buffer_set_delay_bounds(jb2, 20, 200);
  check_equal((uint32_t)(jitter_buffer_get_delay(jb2)), (uint32_t)(20)); /* Clamped to min */

  jitter_buffer_destroy(jb);
  jitter_buffer_destroy(jb2);
}

/* =============================================================================
 * Missing Packet Detection Tests
 * ============================================================================= */

void test_jitter_buffer_get_missing(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 0, 1500);
  check_not_null(jb);

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

  check_equal((int)(count), (int)(2));
  check_equal((uint16_t)(missing[0]), (uint16_t)(101));
  check_equal((uint16_t)(missing[1]), (uint16_t)(103));

  jitter_buffer_destroy(jb);
}

/* =============================================================================
 * Reset Test
 * ============================================================================= */

void test_jitter_buffer_reset(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 20, 1500);
  check_not_null(jb);

  uint8_t payload[] = {0x01, 0x02};
  for (int i = 0; i < 5; i++) {
    rtp_packet_t pkt = make_packet(i, i * 960, payload, sizeof(payload));
    jitter_buffer_put(jb, &pkt, i * 10);
  }

  check_equal((int)(jitter_buffer_get_count(jb)), (int)(5));

  jitter_buffer_reset(jb);

  check_equal((int)(jitter_buffer_get_count(jb)), (int)(0));
  check_true(jitter_buffer_is_empty(jb));

  jitter_buffer_destroy(jb);
}

/* =============================================================================
 * Peek Test
 * ============================================================================= */

void test_jitter_buffer_peek(void) {
  jitter_buffer_t *jb = jitter_buffer_create(48000, 0, 1500);
  check_not_null(jb);

  uint8_t payload[] = {0xAB};
  rtp_packet_t pkt = make_packet(42, 12345, payload, 1);
  jitter_buffer_put(jb, &pkt, 0);

  uint32_t ts;
  int result = jitter_buffer_peek(jb, &ts);
  check_equal((int)(result), (int)(1));
  check_equal((uint32_t)(ts), (uint32_t)(12345));

  /* Packet should still be there */
  check_equal((int)(jitter_buffer_get_count(jb)), (int)(1));

  jitter_buffer_destroy(jb);
}

/* =============================================================================
 * Main
 * ============================================================================= */

spec("test_jitter_buffer") {
  before_each() { setUp(); }
  after_each() { tearDown(); }

  /* Creation/Destruction */
  it("test_jitter_buffer_create") { test_jitter_buffer_create(); };
  it("test_jitter_buffer_create_null_on_zero") { test_jitter_buffer_create_null_on_zero(); };

  /* Put/Get */
  it("test_jitter_buffer_put_single") { test_jitter_buffer_put_single(); };
  it("test_jitter_buffer_put_get_sequence") { test_jitter_buffer_put_get_sequence(); };
  it("test_jitter_buffer_get_ex_returns_marker") { test_jitter_buffer_get_ex_returns_marker(); };
  it("test_jitter_buffer_reorder") { test_jitter_buffer_reorder(); };
  it("test_jitter_buffer_duplicate") { test_jitter_buffer_duplicate(); };
  it("test_jitter_buffer_late_packet") { test_jitter_buffer_late_packet(); };

  /* Delay */
  it("test_jitter_buffer_delay_enforcement") { test_jitter_buffer_delay_enforcement(); };
  it("test_jitter_buffer_set_delay_bounds") { test_jitter_buffer_set_delay_bounds(); };

  /* Missing Packets */
  it("test_jitter_buffer_get_missing") { test_jitter_buffer_get_missing(); };

  /* Reset */
  it("test_jitter_buffer_reset") { test_jitter_buffer_reset(); };

  /* Peek */
  it("test_jitter_buffer_peek") { test_jitter_buffer_peek(); };
}
