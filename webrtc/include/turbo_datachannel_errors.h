/**
 * turbo_datachannel_errors.h - Error codes for WebRTC DataChannel
 *
 * Structured error reporting instead of string-based errors.
 */

#ifndef TURBO_DATACHANNEL_ERRORS_H
#define TURBO_DATACHANNEL_ERRORS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration to avoid circular dependency */
#include <turbo_export.h>

typedef struct turbo_dc_context_s turbo_dc_context_t;
typedef struct turbo_dc_peer_s turbo_dc_peer_t;

/* ============================================================================
 * Error Codes
 * ============================================================================ */

typedef enum {
    TURBO_DC_ERROR_NONE = 0,

    /* Context errors (1-99) */
    TURBO_DC_ERROR_INVALID_CONFIG = 1,
    TURBO_DC_ERROR_NULL_LOOP = 2,
    TURBO_DC_ERROR_ALLOC_CONTEXT = 3,
    TURBO_DC_ERROR_SSL_CONTEXT = 4,
    TURBO_DC_ERROR_CERT_LOAD = 5,
    TURBO_DC_ERROR_SCTP_INIT = 6,

    /* Peer errors (100-199) */
    TURBO_DC_ERROR_NULL_CONTEXT = 100,
    TURBO_DC_ERROR_ALLOC_PEER = 101,
    TURBO_DC_ERROR_NULL_PEER = 102,
    TURBO_DC_ERROR_CREATE_TRANSPORT = 103,
    TURBO_DC_ERROR_LISTEN_FAILED = 104,
    TURBO_DC_ERROR_CONNECT_FAILED = 105,

    /* DTLS errors (200-299) */
    TURBO_DC_ERROR_SSL_CREATE = 200,
    TURBO_DC_ERROR_DTLS_HANDSHAKE = 201,
    TURBO_DC_ERROR_DTLS_FINGERPRINT = 202,

    /* SCTP errors (300-399) */
    TURBO_DC_ERROR_SCTP_SOCKET = 300,
    TURBO_DC_ERROR_SCTP_CONFIG = 301,
    TURBO_DC_ERROR_SCTP_BIND = 302,
    TURBO_DC_ERROR_SCTP_CONNECT = 303,
    TURBO_DC_ERROR_SCTP_SEND = 304,

    /* Channel errors (400-499) */
    TURBO_DC_ERROR_NULL_LABEL = 400,
    TURBO_DC_ERROR_ALLOC_CHANNEL = 401,
    TURBO_DC_ERROR_INVALID_CHANNEL = 402,
    TURBO_DC_ERROR_PEER_NOT_CONNECTED = 403,
    TURBO_DC_ERROR_CHANNEL_NOT_OPEN = 404,
    TURBO_DC_ERROR_NULL_DATA = 405,
    TURBO_DC_ERROR_INVALID_PEER_STATE = 406,

    /* DCEP errors (500-599) */
    TURBO_DC_ERROR_DCEP_SEND_OPEN = 500,
    TURBO_DC_ERROR_DCEP_SEND_ACK = 501,

} turbo_dc_error_code_t;

/* ============================================================================
 * Error Info Structure
 * ============================================================================ */

typedef struct {
    turbo_dc_error_code_t code;
    const char *message;     /* Human-readable description */
    const char *detail;      /* Optional additional detail */
} turbo_dc_error_t;

/* ============================================================================
 * Error API
 * ============================================================================ */

/**
 * Get human-readable string for error code
 */
CXX_C_API const char *turbo_dc_error_string(turbo_dc_error_code_t code);

/**
 * Get last error info from peer
 * This is the preferred API for errors during peer/channel operations.
 */
CXX_C_API turbo_dc_error_t turbo_dc_peer_get_error(turbo_dc_peer_t *peer);

/**
 * Get last error info from context
 * Use this only for errors during turbo_dc_peer_create() when peer is NULL.
 * For all other errors, use turbo_dc_peer_get_error().
 */
CXX_C_API turbo_dc_error_t turbo_dc_context_get_error(turbo_dc_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_DATACHANNEL_ERRORS_H */
