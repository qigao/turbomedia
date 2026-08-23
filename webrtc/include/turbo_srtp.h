/**
 * TurboNet SRTP Implementation
 *
 * Secure RTP wrapper using libsrtp
 * Provides RTP/RTCP encryption services.
 */
#ifndef TURBO_SRTP_H
#define TURBO_SRTP_H

#include <turbo_export.h>
#include <turbo_srtp_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Types
 * ============================================================================= */

typedef struct srtp_session_s srtp_session_t;

/**
 * SRTP session configuration
 */
typedef struct {
  int is_sender;      /* 1 = we're the sender, 0 = receiver */
  int is_dtls_client; /* 1 = local DTLS role is client, 0 = server */
  uint16_t profile;   /* SRTP profile (e.g., AES128_CM_SHA1_80) */
  const srtp_keying_material_t *keys;
} srtp_session_config_t;

/* =============================================================================
 * SRTP Library Initialization
 * ============================================================================= */

/**
 * Initialize SRTP library (call once at startup)
 *
 * @return  0 on success, -1 on error
 */
TURBO_MEDIA_API int srtp_lib_init(void);

/**
 * Shutdown SRTP library (call once at cleanup)
 */
TURBO_MEDIA_API void srtp_lib_shutdown(void);

/* =============================================================================
 * SRTP Session Functions
 * ============================================================================= */

/**
 * Create SRTP session
 *
 * @param config    Session configuration with keying material
 * @return          Session handle, or NULL on error
 */
TURBO_MEDIA_API srtp_session_t *srtp_session_create(const srtp_session_config_t *config);

/**
 * Destroy SRTP session
 */
TURBO_MEDIA_API void srtp_session_destroy(srtp_session_t *session);

/**
 * Protect (encrypt) RTP packet
 *
 * @param session   SRTP session
 * @param packet    RTP packet buffer (in-place modification)
 * @param len       Input: packet length, Output: protected packet length
 * @param max_len   Maximum buffer size (must allow for auth tag)
 * @return          0 on success, -1 on error
 */
TURBO_MEDIA_API int turbo_srtp_protect(srtp_session_t *session, uint8_t *packet, size_t *len,
                                 size_t max_len);

/**
 * Unprotect (decrypt) RTP packet
 *
 * @param session   SRTP session
 * @param packet    SRTP packet buffer (in-place modification)
 * @param len       Input: packet length, Output: unprotected packet length
 * @return          0 on success, -1 on error (auth fail, replay, etc.)
 */
TURBO_MEDIA_API int turbo_srtp_unprotect(srtp_session_t *session, uint8_t *packet, size_t *len);

/**
 * Protect RTCP packet
 *
 * @param session   SRTP session
 * @param packet    RTCP packet buffer (in-place modification)
 * @param len       Input: packet length, Output: protected packet length
 * @param max_len   Maximum buffer size
 * @return          0 on success, -1 on error
 */
TURBO_MEDIA_API int turbo_srtcp_protect(srtp_session_t *session, uint8_t *packet, size_t *len,
                                  size_t max_len);

/**
 * Unprotect RTCP packet
 *
 * @param session   SRTP session
 * @param packet    SRTCP packet buffer (in-place modification)
 * @param len       Input: packet length, Output: unprotected packet length
 * @return          0 on success, -1 on error
 */
TURBO_MEDIA_API int turbo_srtcp_unprotect(srtp_session_t *session, uint8_t *packet, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SRTP_H */
