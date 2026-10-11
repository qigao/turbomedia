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

/* These tests also exercise libSRTP's cipher/auth known-answer self-tests via
 * session initialization. Packet round trips alone cannot establish that a
 * replacement provider preserves the standard cipher output. */
static const uint16_t provider_profiles[] = {
    SRTP_PROFILE_AES128_CM_SHA1_80,
    SRTP_PROFILE_AES128_CM_SHA1_32,
    SRTP_PROFILE_AEAD_AES_128_GCM,
    SRTP_PROFILE_AEAD_AES_256_GCM
};
static srtp_session_t *provider_sender, *provider_receiver, *provider_wrong_key;

static void provider_cleanup(void) {
  srtp_session_destroy(provider_wrong_key);
  srtp_session_destroy(provider_receiver);
  srtp_session_destroy(provider_sender);
  provider_wrong_key = provider_receiver = provider_sender = NULL;
}

static void provider_setup(uint16_t profile, int client_sender) {
  srtp_keying_material_t keys;
  fill_keying_material(&keys);
  keys.key_len = profile == SRTP_PROFILE_AEAD_AES_256_GCM ? 32 : 16;
  keys.salt_len = profile == SRTP_PROFILE_AEAD_AES_128_GCM ||
                  profile == SRTP_PROFILE_AEAD_AES_256_GCM ? 12 : 14;
  for (size_t i = 0; i < keys.key_len; ++i) {
    keys.client_key[i] = (uint8_t)(0x10 + i);
    keys.server_key[i] = (uint8_t)(0x80 + i);
  }
  srtp_session_config_t config = {
      .is_sender = 1, .is_dtls_client = client_sender,
      .profile = profile, .keys = &keys
  };
  provider_sender = srtp_session_create(&config);
  check_not_null(provider_sender);
  config.is_sender = 0;
  config.is_dtls_client = !client_sender;
  provider_receiver = srtp_session_create(&config);
  check_not_null(provider_receiver);
  keys.client_key[0] ^= 1;
  keys.server_key[0] ^= 1;
  provider_wrong_key = srtp_session_create(&config);
  check_not_null(provider_wrong_key);
}

spec("SRTP crypto provider compatibility") {
  before_each() {
    provider_sender = provider_receiver = provider_wrong_key = NULL;
  }
  after_each() { provider_cleanup(); }

  it("preserves RTP bytes for every profile and both DTLS key directions") {
    for (size_t p = 0; p < sizeof(provider_profiles) / sizeof(provider_profiles[0]); ++p) {
      for (int client = 0; client < 2; ++client) {
        uint8_t packet[256], original[256];
        provider_setup(provider_profiles[p], client);
        for (uint16_t seq = 1; seq <= 3; ++seq) {
          size_t length = build_test_rtp_packet(packet, sizeof(packet), seq, 90000, 0x12345678);
          size_t original_length = length;
          memcpy(original, packet, length);
          check_equal(turbo_srtp_protect(provider_sender, packet, &length, sizeof(packet)), 0);
          check_equal(turbo_srtp_unprotect(provider_receiver, packet, &length), 0);
          check_equal(length, original_length);
          check_equal(memcmp(packet, original, length), 0);
        }
        provider_cleanup();
      }
    }
  }

  it("preserves SRTCP bytes and successive AAD contributions for every profile") {
    for (size_t p = 0; p < sizeof(provider_profiles) / sizeof(provider_profiles[0]); ++p) {
      for (int client = 0; client < 2; ++client) {
        uint8_t packet[256], original[256];
        provider_setup(provider_profiles[p], client);
        for (int repeat = 0; repeat < 3; ++repeat) {
          size_t length = build_test_rtcp_packet(packet, sizeof(packet));
          size_t original_length = length;
          memcpy(original, packet, length);
          check_equal(turbo_srtcp_protect(provider_sender, packet, &length, sizeof(packet)), 0);
          check_equal(turbo_srtcp_unprotect(provider_receiver, packet, &length), 0);
          check_equal(length, original_length);
          check_equal(memcmp(packet, original, length), 0);
        }
        provider_cleanup();
      }
    }
  }

  it("rejects wrong keys and damaged tags without consuming valid packet replay state") {
    for (size_t p = 0; p < sizeof(provider_profiles) / sizeof(provider_profiles[0]); ++p) {
      for (int rtcp = 0; rtcp < 2; ++rtcp) {
        uint8_t packet[256], original[256], damaged[256];
        provider_setup(provider_profiles[p], 1);
        size_t length = rtcp ? build_test_rtcp_packet(packet, sizeof(packet))
                            : build_test_rtp_packet(packet, sizeof(packet), 1, 90000, 0x12345678);
        size_t original_length = length;
        memcpy(original, packet, length);
        check_equal(rtcp ? turbo_srtcp_protect(provider_sender, packet, &length, sizeof(packet))
                         : turbo_srtp_protect(provider_sender, packet, &length, sizeof(packet)), 0);
        size_t rejected_length = length;
        memcpy(damaged, packet, length);
        check_not_equal(rtcp ? turbo_srtcp_unprotect(provider_wrong_key, damaged, &rejected_length)
                             : turbo_srtp_unprotect(provider_wrong_key, damaged, &rejected_length), 0);
        rejected_length = length;
        memcpy(damaged, packet, length);
        /* For AEAD SRTCP the four-byte index follows the authentication tag. */
        size_t tag_end = length;
        if (rtcp && (provider_profiles[p] == SRTP_PROFILE_AEAD_AES_128_GCM ||
                     provider_profiles[p] == SRTP_PROFILE_AEAD_AES_256_GCM)) tag_end -= 4;
        damaged[tag_end - 1] ^= 1;
        check_not_equal(rtcp ? turbo_srtcp_unprotect(provider_receiver, damaged, &rejected_length)
                             : turbo_srtp_unprotect(provider_receiver, damaged, &rejected_length), 0);
        check_equal(rtcp ? turbo_srtcp_unprotect(provider_receiver, packet, &length)
                         : turbo_srtp_unprotect(provider_receiver, packet, &length), 0);
        check_equal(length, original_length);
        check_equal(memcmp(packet, original, length), 0);
        provider_cleanup();
      }
    }
  }
}
