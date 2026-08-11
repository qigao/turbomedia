/**
 * SRTP Session Implementation
 *
 * Wraps libsrtp for RTP/RTCP encryption and DTLS-SRTP key derivation
 */
#include "turbo_srtp.h"
#include "turbo_thread.h"
#pragma push_macro("SRTP_MAX_KEY_LEN")
#pragma push_macro("SRTP_MAX_TRAILER_LEN")
#undef SRTP_MAX_KEY_LEN
#undef SRTP_MAX_TRAILER_LEN
#include <srtp2/srtp.h>
#pragma pop_macro("SRTP_MAX_TRAILER_LEN")
#pragma pop_macro("SRTP_MAX_KEY_LEN")
#include <stdlib.h>
#include <limits.h>
#include <string.h>

/* SRTP Session internal structure */
struct srtp_session_s {
  srtp_t srtp_ctx;
  srtp_t srtcp_send_ctx;
  srtp_t srtcp_recv_ctx;
  int is_sender;
  int is_dtls_client;
  uint16_t profile;
  uint8_t rtp_master_key[SRTP_MAX_KEY_LEN + SRTP_MAX_SALT_LEN];
  uint8_t srtcp_send_master_key[SRTP_MAX_KEY_LEN + SRTP_MAX_SALT_LEN];
  uint8_t srtcp_recv_master_key[SRTP_MAX_KEY_LEN + SRTP_MAX_SALT_LEN];
};

static int srtp_init_policy_for_profile(uint16_t profile, srtp_policy_t *policy) {
  if (!policy) {
    return -1;
  }

  memset(policy, 0, sizeof(*policy));
  switch (profile) {
  case SRTP_PROFILE_AES128_CM_SHA1_80:
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy->rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy->rtcp);
    return 0;
  case SRTP_PROFILE_AES128_CM_SHA1_32:
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&policy->rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy->rtcp);
    return 0;
  case SRTP_PROFILE_AEAD_AES_128_GCM:
    srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy->rtp);
    srtp_crypto_policy_set_aes_gcm_128_16_auth(&policy->rtcp);
    return 0;
  case SRTP_PROFILE_AEAD_AES_256_GCM:
    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy->rtp);
    srtp_crypto_policy_set_aes_gcm_256_16_auth(&policy->rtcp);
    return 0;
  default:
    return -1;
  }
}

static int srtp_build_master_key(const srtp_keying_material_t *keys, int use_client_key,
                                 uint8_t *master_key, size_t master_key_size) {
  const uint8_t *key;
  const uint8_t *salt;
  size_t key_len;
  size_t salt_len;

  if (!keys || !master_key) {
    return -1;
  }

  key_len = keys->key_len;
  salt_len = keys->salt_len;
  if (key_len > sizeof(keys->client_key) || salt_len > sizeof(keys->client_salt) ||
      key_len > master_key_size || salt_len > master_key_size - key_len) {
    return -1;
  }

  key = use_client_key ? keys->client_key : keys->server_key;
  salt = use_client_key ? keys->client_salt : keys->server_salt;
  memcpy(master_key, key, key_len);
  memcpy(master_key + key_len, salt, salt_len);
  return 0;
}

/* libsrtp has process-wide initialization state. Session references defer a
 * concurrent shutdown until the last active session has been destroyed. */
static turbo_once_t g_srtp_lock_once = TURBO_ONCE_INIT;
static turbo_mutex_t g_srtp_lock;
static int g_srtp_initialized = 0;
static size_t g_srtp_session_count = 0;
static int g_srtp_shutdown_pending = 0;

static void srtp_init_lock(void) { turbo_mutex_init(&g_srtp_lock); }

static int srtp_init_locked(void) {
  srtp_err_status_t status;

  if (g_srtp_initialized) {
    return 0;
  }

  status = srtp_init();
  if (status != srtp_err_status_ok) {
    return -1;
  }

  g_srtp_initialized = 1;
  return 0;
}

static int srtp_session_acquire(void) {
  int result;

  turbo_once(&g_srtp_lock_once, srtp_init_lock);
  turbo_mutex_lock(&g_srtp_lock);
  result = srtp_init_locked();
  if (result == 0) {
    g_srtp_session_count++;
    g_srtp_shutdown_pending = 0;
  }
  turbo_mutex_unlock(&g_srtp_lock);
  return result;
}

static void srtp_session_release(void) {
  turbo_once(&g_srtp_lock_once, srtp_init_lock);
  turbo_mutex_lock(&g_srtp_lock);
  if (g_srtp_session_count > 0) {
    g_srtp_session_count--;
  }
  if (g_srtp_session_count == 0 && g_srtp_shutdown_pending && g_srtp_initialized) {
    srtp_shutdown();
    g_srtp_initialized = 0;
    g_srtp_shutdown_pending = 0;
  }
  turbo_mutex_unlock(&g_srtp_lock);
}

int srtp_lib_init(void) {
  int result;

  turbo_once(&g_srtp_lock_once, srtp_init_lock);
  turbo_mutex_lock(&g_srtp_lock);
  result = srtp_init_locked();
  if (result == 0) {
    g_srtp_shutdown_pending = 0;
  }
  turbo_mutex_unlock(&g_srtp_lock);
  return result;
}

void srtp_lib_shutdown(void) {
  turbo_once(&g_srtp_lock_once, srtp_init_lock);
  turbo_mutex_lock(&g_srtp_lock);
  if (g_srtp_session_count == 0 && g_srtp_initialized) {
    srtp_shutdown();
    g_srtp_initialized = 0;
  } else if (g_srtp_session_count > 0) {
    g_srtp_shutdown_pending = 1;
  }
  turbo_mutex_unlock(&g_srtp_lock);
}

srtp_session_t *srtp_session_create(const srtp_session_config_t *config) {
  if (!config || !config->keys) return NULL;

  if (srtp_session_acquire() != 0) return NULL;

  srtp_session_t *session = (srtp_session_t *)calloc(1, sizeof(srtp_session_t));
  if (!session) {
    srtp_session_release();
    return NULL;
  }

  session->is_sender = config->is_sender;
  session->is_dtls_client = config->is_dtls_client;
  session->profile = config->profile;

  int rtp_uses_client_key = ((config->is_sender && config->is_dtls_client) ||
                             (!config->is_sender && !config->is_dtls_client))
                                ? 1
                                : 0;
  int rtcp_send_uses_client_key = config->is_dtls_client ? 1 : 0;
  int rtcp_recv_uses_client_key = rtcp_send_uses_client_key ? 0 : 1;
  srtp_policy_t policy;
  if (srtp_init_policy_for_profile(config->profile, &policy) != 0) {
    free(session);
    srtp_session_release();
    return NULL;
  }

  /* Create SRTP context */
  if (srtp_build_master_key(config->keys, rtp_uses_client_key, session->rtp_master_key,
                            sizeof(session->rtp_master_key)) != 0) {
    free(session);
    srtp_session_release();
    return NULL;
  }
  policy.key = session->rtp_master_key;
  policy.ssrc.type = config->is_sender ? ssrc_any_outbound : ssrc_any_inbound;
  policy.next = NULL;

  srtp_err_status_t status = srtp_create(&session->srtp_ctx, &policy);
  if (status != srtp_err_status_ok) {
    free(session);
    srtp_session_release();
    return NULL;
  }

  /* RTCP is bidirectional for every media track:
   * - local endpoint always sends outbound RTCP
   * - local endpoint always receives inbound RTCP feedback/reports
   * Keep dedicated contexts for each direction instead of mirroring RTP direction. */
  if (srtp_build_master_key(config->keys, rtcp_send_uses_client_key, session->srtcp_send_master_key,
                            sizeof(session->srtcp_send_master_key)) != 0) {
    srtp_dealloc(session->srtp_ctx);
    free(session);
    srtp_session_release();
    return NULL;
  }
  policy.key = session->srtcp_send_master_key;
  policy.ssrc.type = ssrc_any_outbound;
  status = srtp_create(&session->srtcp_send_ctx, &policy);
  if (status != srtp_err_status_ok) {
    srtp_dealloc(session->srtp_ctx);
    free(session);
    srtp_session_release();
    return NULL;
  }

  if (srtp_build_master_key(config->keys, rtcp_recv_uses_client_key, session->srtcp_recv_master_key,
                            sizeof(session->srtcp_recv_master_key)) != 0) {
    srtp_dealloc(session->srtcp_send_ctx);
    srtp_dealloc(session->srtp_ctx);
    free(session);
    srtp_session_release();
    return NULL;
  }
  policy.key = session->srtcp_recv_master_key;
  policy.ssrc.type = ssrc_any_inbound;
  status = srtp_create(&session->srtcp_recv_ctx, &policy);
  if (status != srtp_err_status_ok) {
    srtp_dealloc(session->srtcp_send_ctx);
    srtp_dealloc(session->srtp_ctx);
    free(session);
    srtp_session_release();
    return NULL;
  }

  return session;
}

void srtp_session_destroy(srtp_session_t *session) {
  if (!session) return;

  if (session->srtp_ctx) {
    srtp_dealloc(session->srtp_ctx);
  }
  if (session->srtcp_send_ctx) {
    srtp_dealloc(session->srtcp_send_ctx);
  }
  if (session->srtcp_recv_ctx) {
    srtp_dealloc(session->srtcp_recv_ctx);
  }

  free(session);
  srtp_session_release();
}

int turbo_srtp_protect(srtp_session_t *session, uint8_t *packet, size_t *len, size_t max_len) {
  if (!session || !packet || !len) return -1;
  if (*len > (size_t)INT_MAX || max_len < *len ||
      max_len - *len < SRTP_MAX_TRAILER_LEN) {
    return -1;
  }

  int pkt_len = (int)*len;
  srtp_err_status_t status = srtp_protect(session->srtp_ctx, packet, &pkt_len);
  if (status != srtp_err_status_ok) {
    return -1;
  }

  *len = (size_t)pkt_len;
  return 0;
}

int turbo_srtp_unprotect(srtp_session_t *session, uint8_t *packet, size_t *len) {
  if (!session || !packet || !len) return -1;
  if (*len > (size_t)INT_MAX) return -1;

  int pkt_len = (int)*len;
  srtp_err_status_t status = srtp_unprotect(session->srtp_ctx, packet, &pkt_len);
  if (status != srtp_err_status_ok) {
    return -1;
  }

  *len = (size_t)pkt_len;
  return 0;
}

int turbo_srtcp_protect(srtp_session_t *session, uint8_t *packet, size_t *len, size_t max_len) {
  if (!session || !packet || !len) return -1;
  if (*len > (size_t)INT_MAX || max_len < *len ||
      max_len - *len < SRTP_MAX_TRAILER_LEN + 4) {
    return -1; /* RTCP has extra index */
  }

  int pkt_len = (int)*len;
  srtp_err_status_t status = srtp_protect_rtcp(session->srtcp_send_ctx, packet, &pkt_len);
  if (status != srtp_err_status_ok) {
    return -1;
  }

  *len = (size_t)pkt_len;
  return 0;
}

int turbo_srtcp_unprotect(srtp_session_t *session, uint8_t *packet, size_t *len) {
  if (!session || !packet || !len) return -1;
  if (*len > (size_t)INT_MAX) return -1;

  int pkt_len = (int)*len;
  srtp_err_status_t status = srtp_unprotect_rtcp(session->srtcp_recv_ctx, packet, &pkt_len);
  if (status != srtp_err_status_ok) {
    return -1;
  }

  *len = (size_t)pkt_len;
  return 0;
}
