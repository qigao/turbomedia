#include "tinytest.h"
#include "turbo_rtp.h"
#include "turbo_srtp.h"
#include <limits.h>
#include <string.h>

static void fill_keying_material(srtp_keying_material_t *keys) {
  memset(keys, 0, sizeof(*keys));
  keys->key_len = 16;
  keys->salt_len = 14;

  for (size_t i = 0; i < keys->key_len; ++i) {
    keys->client_key[i] = (uint8_t)(0x10 + i);
    keys->server_key[i] = (uint8_t)(0x80 + i);
  }
  for (size_t i = 0; i < keys->salt_len; ++i) {
    keys->client_salt[i] = (uint8_t)(0x20 + i);
    keys->server_salt[i] = (uint8_t)(0x90 + i);
  }
}

static size_t build_test_rtp_packet(uint8_t *buffer, size_t buffer_len, uint16_t seq,
                                    uint32_t timestamp, uint32_t ssrc) {
  rtp_packet_t pkt;
  static const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
  int len = rtp_packet_build(&pkt, RTP_PT_VP8, seq, timestamp, ssrc, 1, payload, sizeof(payload),
                             buffer, buffer_len);
  check_greater(len, 0);
  return (size_t)len;
}

static size_t build_test_rtcp_packet(uint8_t *buffer, size_t buffer_len) {
  rtcp_compound_t compound;
  rtcp_compound_init(&compound, buffer, buffer_len);
  check_equal((int)(rtcp_compound_add_pli(&compound, 0x11223344u, 0x55667788u)), (int)(0));
  return rtcp_compound_finish(&compound);
}

void test_srtp_dtls_client_sender_to_server_receiver(void) {
  srtp_keying_material_t keys;
  srtp_session_config_t sender_cfg;
  srtp_session_config_t receiver_cfg;
  srtp_session_t *sender;
  srtp_session_t *receiver;
  uint8_t packet[RTP_MAX_PACKET + SRTP_MAX_TRAILER_LEN];
  size_t len;

  fill_keying_material(&keys);

  sender_cfg = (srtp_session_config_t){
      .is_sender = 1,
      .is_dtls_client = 1,
      .profile = SRTP_PROFILE_AES128_CM_SHA1_80,
      .keys = &keys,
  };
  receiver_cfg = (srtp_session_config_t){
      .is_sender = 0,
      .is_dtls_client = 0,
      .profile = SRTP_PROFILE_AES128_CM_SHA1_80,
      .keys = &keys,
  };

  sender = srtp_session_create(&sender_cfg);
  receiver = srtp_session_create(&receiver_cfg);
  check_not_null(sender);
  check_not_null(receiver);

  len = build_test_rtp_packet(packet, sizeof(packet), 321, 90000, 0x12345678u);
  check_equal((int)(turbo_srtp_protect(sender, packet, &len, sizeof(packet))), (int)(0));
  check_equal((int)(turbo_srtp_unprotect(receiver, packet, &len)), (int)(0));
  check_equal((size_t)(len), (size_t)(RTP_HEADER_SIZE + 6));

  srtp_session_destroy(receiver);
  srtp_session_destroy(sender);
}

void test_srtp_dtls_server_sender_to_client_receiver(void) {
  srtp_keying_material_t keys;
  srtp_session_config_t sender_cfg;
  srtp_session_config_t receiver_cfg;
  srtp_session_t *sender;
  srtp_session_t *receiver;
  uint8_t packet[RTP_MAX_PACKET + SRTP_MAX_TRAILER_LEN];
  size_t len;

  fill_keying_material(&keys);

  sender_cfg = (srtp_session_config_t){
      .is_sender = 1,
      .is_dtls_client = 0,
      .profile = SRTP_PROFILE_AES128_CM_SHA1_80,
      .keys = &keys,
  };
  receiver_cfg = (srtp_session_config_t){
      .is_sender = 0,
      .is_dtls_client = 1,
      .profile = SRTP_PROFILE_AES128_CM_SHA1_80,
      .keys = &keys,
  };

  sender = srtp_session_create(&sender_cfg);
  receiver = srtp_session_create(&receiver_cfg);
  check_not_null(sender);
  check_not_null(receiver);

  len = build_test_rtp_packet(packet, sizeof(packet), 654, 180000, 0x87654321u);
  check_equal((int)(turbo_srtp_protect(sender, packet, &len, sizeof(packet))), (int)(0));
  check_equal((int)(turbo_srtp_unprotect(receiver, packet, &len)), (int)(0));
  check_equal((size_t)(len), (size_t)(RTP_HEADER_SIZE + 6));

  srtp_session_destroy(receiver);
  srtp_session_destroy(sender);
}

void test_srtp_aead_aes_128_gcm_round_trip(void) {
  srtp_keying_material_t keys;
  srtp_session_config_t sender_cfg;
  srtp_session_config_t receiver_cfg;
  srtp_session_t *sender;
  srtp_session_t *receiver;
  uint8_t packet[RTP_MAX_PACKET + SRTP_MAX_TRAILER_LEN];
  size_t len;

  fill_keying_material(&keys);
  keys.salt_len = 12;

  sender_cfg = (srtp_session_config_t){
      .is_sender = 1,
      .is_dtls_client = 1,
      .profile = SRTP_PROFILE_AEAD_AES_128_GCM,
      .keys = &keys,
  };
  receiver_cfg = (srtp_session_config_t){
      .is_sender = 0,
      .is_dtls_client = 0,
      .profile = SRTP_PROFILE_AEAD_AES_128_GCM,
      .keys = &keys,
  };

  sender = srtp_session_create(&sender_cfg);
  receiver = srtp_session_create(&receiver_cfg);
  check_not_null(sender);
  check_not_null(receiver);

  len = build_test_rtp_packet(packet, sizeof(packet), 901, 270000, 0xABCDEF12u);
  check_equal((int)(turbo_srtp_protect(sender, packet, &len, sizeof(packet))), (int)(0));
  check_equal((int)(turbo_srtp_unprotect(receiver, packet, &len)), (int)(0));
  check_equal((size_t)(len), (size_t)(RTP_HEADER_SIZE + 6));

  srtp_session_destroy(receiver);
  srtp_session_destroy(sender);
}

void test_srtcp_dtls_server_sender_to_client_receiver(void) {
  srtp_keying_material_t keys;
  srtp_session_config_t sender_cfg;
  srtp_session_config_t receiver_cfg;
  srtp_session_t *sender;
  srtp_session_t *receiver;
  uint8_t packet[256];
  size_t len;

  fill_keying_material(&keys);

  sender_cfg = (srtp_session_config_t){
      .is_sender = 1,
      .is_dtls_client = 0,
      .profile = SRTP_PROFILE_AES128_CM_SHA1_80,
      .keys = &keys,
  };
  receiver_cfg = (srtp_session_config_t){
      .is_sender = 0,
      .is_dtls_client = 1,
      .profile = SRTP_PROFILE_AES128_CM_SHA1_80,
      .keys = &keys,
  };

  sender = srtp_session_create(&sender_cfg);
  receiver = srtp_session_create(&receiver_cfg);
  check_not_null(sender);
  check_not_null(receiver);

  len = build_test_rtcp_packet(packet, sizeof(packet));
  check_greater(len, 0);
  check_equal((int)(turbo_srtcp_protect(sender, packet, &len, sizeof(packet))), (int)(0));
  check_equal((int)(turbo_srtcp_unprotect(receiver, packet, &len)), (int)(0));
  check_equal((size_t)(len), (size_t)(12));

  srtp_session_destroy(receiver);
  srtp_session_destroy(sender);
}

void test_srtcp_dtls_client_sender_to_server_receiver(void) {
  srtp_keying_material_t keys;
  srtp_session_config_t sender_cfg;
  srtp_session_config_t receiver_cfg;
  srtp_session_t *sender;
  srtp_session_t *receiver;
  uint8_t packet[256];
  size_t len;

  fill_keying_material(&keys);

  sender_cfg = (srtp_session_config_t){
      .is_sender = 1,
      .is_dtls_client = 1,
      .profile = SRTP_PROFILE_AES128_CM_SHA1_80,
      .keys = &keys,
  };
  receiver_cfg = (srtp_session_config_t){
      .is_sender = 0,
      .is_dtls_client = 0,
      .profile = SRTP_PROFILE_AES128_CM_SHA1_80,
      .keys = &keys,
  };

  sender = srtp_session_create(&sender_cfg);
  receiver = srtp_session_create(&receiver_cfg);
  check_not_null(sender);
  check_not_null(receiver);

  len = build_test_rtcp_packet(packet, sizeof(packet));
  check_greater(len, 0);
  check_equal((int)(turbo_srtcp_protect(sender, packet, &len, sizeof(packet))), (int)(0));
  check_equal((int)(turbo_srtcp_unprotect(receiver, packet, &len)), (int)(0));
  check_equal((size_t)(len), (size_t)(12));

  srtp_session_destroy(receiver);
  srtp_session_destroy(sender);
}

void test_srtp_shutdown_waits_for_active_sessions(void) {
  srtp_keying_material_t keys;
  srtp_session_config_t sender_cfg;
  srtp_session_config_t receiver_cfg;
  srtp_session_t *sender;
  srtp_session_t *receiver;
  uint8_t packet[RTP_MAX_PACKET + SRTP_MAX_TRAILER_LEN];
  size_t len;

  fill_keying_material(&keys);
  sender_cfg = (srtp_session_config_t){
      .is_sender = 1,
      .is_dtls_client = 1,
      .profile = SRTP_PROFILE_AES128_CM_SHA1_80,
      .keys = &keys,
  };
  receiver_cfg = (srtp_session_config_t){
      .is_sender = 0,
      .is_dtls_client = 0,
      .profile = SRTP_PROFILE_AES128_CM_SHA1_80,
      .keys = &keys,
  };

  sender = srtp_session_create(&sender_cfg);
  receiver = srtp_session_create(&receiver_cfg);
  check_not_null(sender);
  check_not_null(receiver);

  srtp_lib_shutdown();
  len = build_test_rtp_packet(packet, sizeof(packet), 77, 48000, 0x10203040u);
  check_equal((int)(turbo_srtp_protect(sender, packet, &len, sizeof(packet))), (int)(0));
  check_equal((int)(turbo_srtp_unprotect(receiver, packet, &len)), (int)(0));

  srtp_session_destroy(receiver);
  srtp_session_destroy(sender);

  sender = srtp_session_create(&sender_cfg);
  check_not_null(sender);
  srtp_session_destroy(sender);
  srtp_lib_shutdown();
}

void test_srtp_rejects_oversized_and_insufficient_buffers(void) {
  srtp_keying_material_t keys;
  srtp_session_config_t config;
  srtp_session_t *session;
  uint8_t packet[64] = {0};
  size_t len;

  fill_keying_material(&keys);
  config = (srtp_session_config_t){
      .is_sender = 1,
      .is_dtls_client = 1,
      .profile = SRTP_PROFILE_AES128_CM_SHA1_80,
      .keys = &keys,
  };
  session = srtp_session_create(&config);
  check_not_null(session);

  len = sizeof(packet);
  check_equal((int)(turbo_srtp_protect(session, packet, &len, sizeof(packet) - 1)), (int)(-1));
  len = (size_t)INT_MAX + 1;
  check_equal((int)(turbo_srtp_protect(session, packet, &len, SIZE_MAX)), (int)(-1));
  len = (size_t)INT_MAX + 1;
  check_equal((int)(turbo_srtp_unprotect(session, packet, &len)), (int)(-1));

  srtp_session_destroy(session);
}

spec("test_srtp") {
  it("test_srtp_dtls_client_sender_to_server_receiver") { test_srtp_dtls_client_sender_to_server_receiver(); };
  it("test_srtp_dtls_server_sender_to_client_receiver") { test_srtp_dtls_server_sender_to_client_receiver(); };
  it("test_srtp_aead_aes_128_gcm_round_trip") { test_srtp_aead_aes_128_gcm_round_trip(); };
  it("test_srtcp_dtls_server_sender_to_client_receiver") { test_srtcp_dtls_server_sender_to_client_receiver(); };
  it("test_srtcp_dtls_client_sender_to_server_receiver") { test_srtcp_dtls_client_sender_to_server_receiver(); };
  it("test_srtp_shutdown_waits_for_active_sessions") { test_srtp_shutdown_waits_for_active_sessions(); };
  it("test_srtp_rejects_oversized_and_insufficient_buffers") { test_srtp_rejects_oversized_and_insufficient_buffers(); };
}
